/*
 * Copyright 2008 Search Solution Corporation
 * Copyright 2016 CUBRID Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

/*
 * cm_ssh - ssh and sftp for the HA node-adding script (ha_add_nodes.sh), on a statically linked libssh2.
 *
 *   cm_ssh hostkey <host> <port>
 *   cm_ssh check <host> <port> <user> [<SHA256:fingerprint>] [--password-fd N]
 *   cm_ssh agent --socket <path> (--credentials-fd N | --ask <host>:<port>:<user>:<fingerprint> ...) [--log <file>]
 *   cm_ssh bind  --agent <socket> <pid>
 *   cm_ssh exec  --agent <socket> <host> [--stdin] [--daemon] -- <command>
 *   cm_ssh put   --agent <socket> <host> <local file> <remote file>
 *   cm_ssh quit  --agent <socket>
 *
 * The agent holds the node passwords in memory only (never on disk, in arguments or in the environment) and runs
 * commands and transfers for its clients over one ssh session per node. Clients name a node, never a password.
 * Credentials come over a pipe (fd N) as lines "host<TAB>port<TAB>user<TAB>fingerprint<TAB>password": the password
 * is everything after the fourth tab. Each node is logged in to at most once per failure: a failed login is not
 * retried (an OS account must not get locked by repeated attempts).
 *
 * Exit status of a client: the remote command's status; 255 when cm_ssh itself failed, with a line
 * "cm_ssh: <code>: <message>" on stderr (code: unreachable, hostkey_mismatch, auth_failed, unknown_host,
 * no_agent, transfer_failed, internal, usage).
 */

#include <libssh2.h>
#include <libssh2_sftp.h>
#include <openssl/evp.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

using std::string;
using std::vector;

namespace
{
  const int EXIT_HELPER = 255;

  /* ------------------------------------------------------------ small helpers */

  void wipe (string &s)
  {
    volatile char *p = s.empty () ? NULL : &s[0];
    for (size_t i = 0; p != NULL && i < s.size (); i++)
      {
	p[i] = 0;
      }
    s.clear ();
    s.shrink_to_fit ();
  }

  int fail (const string &code, const string &msg)
  {
    fprintf (stderr, "cm_ssh: %s: %s\n", code.c_str (), msg.c_str ());
    return EXIT_HELPER;
  }

  string shq (const string &s)
  {
    string out = "'";
    for (char c : s)
      {
	if (c == '\'')
	  {
	    out += "'\\''";
	  }
	else
	  {
	    out += c;
	  }
      }
    return out + "'";
  }

  bool parse_port (const string &s, int *port)
  {
    if (s.empty () || s.size () > 5 || s.find_first_not_of ("0123456789") != string::npos)
      {
	return false;
      }
    *port = atoi (s.c_str ());
    return *port >= 1 && *port <= 65535;
  }

  /* host names: letters, digits, '.', '_', '-', not starting with '-' */
  bool valid_name (const string &s)
  {
    if (s.empty () || s.size () > 255 || !isalnum ((unsigned char) s[0]))
      {
	return false;
      }
    for (char c : s)
      {
	if (!isalnum ((unsigned char) c) && c != '.' && c != '_' && c != '-')
	  {
	    return false;
	  }
      }
    return true;
  }

  bool write_all (int fd, const void *buf, size_t len)
  {
    const char *p = (const char *) buf;
    while (len > 0)
      {
	ssize_t n = write (fd, p, len);
	if (n < 0 && errno == EINTR)
	  {
	    continue;
	  }
	if (n <= 0)
	  {
	    return false;
	  }
	p += n;
	len -= (size_t) n;
      }
    return true;
  }

  /* start time of a process (field 22 of /proc/<pid>/stat), "" when it is gone */
  string proc_start_time (pid_t pid)
  {
    char path[64];
    snprintf (path, sizeof (path), "/proc/%d/stat", (int) pid);
    FILE *fp = fopen (path, "r");
    if (fp == NULL)
      {
	return "";
      }
    char buf[1024];
    size_t n = fread (buf, 1, sizeof (buf) - 1, fp);
    fclose (fp);
    buf[n] = 0;
    char *p = strrchr (buf, ')');	/* the command name may contain spaces */
    if (p == NULL)
      {
	return "";
      }
    int field = 2;
    for (char *t = strtok (p + 1, " "); t != NULL; t = strtok (NULL, " "))
      {
	if (++field == 22)
	  {
	    return t;
	  }
      }
    return "";
  }

  bool sha256_file (const string &path, string *hex)
  {
    FILE *fp = fopen (path.c_str (), "rb");
    if (fp == NULL)
      {
	return false;
      }
    EVP_MD_CTX *ctx = EVP_MD_CTX_new ();
    EVP_DigestInit_ex (ctx, EVP_sha256 (), NULL);
    char buf[65536];
    size_t n;
    while ((n = fread (buf, 1, sizeof (buf), fp)) > 0)
      {
	EVP_DigestUpdate (ctx, buf, n);
      }
    bool ok = !ferror (fp);
    fclose (fp);
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_DigestFinal_ex (ctx, md, &len);
    EVP_MD_CTX_free (ctx);
    char out[2 * EVP_MAX_MD_SIZE + 1];
    for (unsigned int i = 0; i < len; i++)
      {
	snprintf (out + 2 * i, 3, "%02x", md[i]);
      }
    *hex = string (out, 2 * len);
    return ok;
  }

  /* ------------------------------------------------------------ framing between client and agent */

  /*
   * A frame is one type byte, a 4-byte big-endian length, then the data.
   *   client -> agent: 'R' request (arguments separated by NUL), 'I' stdin data, 'E' end of stdin
   *   agent -> client: 'O' stdout data, 'e' stderr data, 'X' exit status (decimal), 'F' failure "<code> <message>"
   */
  bool send_frame (int fd, char type, const string &data)
  {
    unsigned char h[5];
    uint32_t len = (uint32_t) data.size ();
    h[0] = (unsigned char) type;
    h[1] = (unsigned char) (len >> 24);
    h[2] = (unsigned char) (len >> 16);
    h[3] = (unsigned char) (len >> 8);
    h[4] = (unsigned char) len;
    return write_all (fd, h, 5) && (data.empty () || write_all (fd, data.data (), data.size ()));
  }

  /* collects bytes from a socket and hands out whole frames */
  struct frame_reader
  {
    string buf;
    bool closed = false;

    /* reads what is there (fd may be non-blocking); false on a read error */
    bool fill (int fd)
    {
      char tmp[65536];
      ssize_t n = read (fd, tmp, sizeof (tmp));
      if (n > 0)
	{
	  buf.append (tmp, (size_t) n);
	  return true;
	}
      if (n == 0)
	{
	  closed = true;
	  return true;
	}
      return errno == EAGAIN || errno == EINTR;
    }

    bool next (char *type, string *data)
    {
      if (buf.size () < 5)
	{
	  return false;
	}
      const unsigned char *h = (const unsigned char *) buf.data ();
      uint32_t len = ((uint32_t) h[1] << 24) | ((uint32_t) h[2] << 16) | ((uint32_t) h[3] << 8) | h[4];
      if (buf.size () < 5 + (size_t) len)
	{
	  return false;
	}
      *type = (char) h[0];
      *data = buf.substr (5, len);
      buf.erase (0, 5 + (size_t) len);
      return true;
    }
  };

  /* blocking read of one frame; false when the peer closed */
  bool read_frame (int fd, frame_reader &r, char *type, string *data)
  {
    while (!r.next (type, data))
      {
	if (r.closed)
	  {
	    return false;
	  }
	struct pollfd p = { fd, POLLIN, 0 };
	if (poll (&p, 1, -1) < 0 && errno != EINTR)
	  {
	    return false;
	  }
	if (!r.fill (fd))
	  {
	    return false;
	  }
      }
    return true;
  }

  /* ------------------------------------------------------------ ssh */

  std::once_flag g_ssh_init_once;

  /* OpenSSH shows SHA256 fingerprints as base64 without '=' padding */
  string sha256_fingerprint (const unsigned char *hash)
  {
    unsigned char b64[64];
    int n = EVP_EncodeBlock (b64, hash, 32);
    string s ((const char *) b64, n > 0 ? n : 0);
    while (!s.empty () && s.back () == '=')
      {
	s.pop_back ();
      }
    return "SHA256:" + s;
  }

  const char *hostkey_type_name (int type)
  {
    switch (type)
      {
      case LIBSSH2_HOSTKEY_TYPE_RSA:
	return "ssh-rsa";
      case LIBSSH2_HOSTKEY_TYPE_DSS:
	return "ssh-dss";
#ifdef LIBSSH2_HOSTKEY_TYPE_ECDSA_256
      case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:
	return "ecdsa-sha2-nistp256";
      case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:
	return "ecdsa-sha2-nistp384";
      case LIBSSH2_HOSTKEY_TYPE_ECDSA_521:
	return "ecdsa-sha2-nistp521";
#endif
#ifdef LIBSSH2_HOSTKEY_TYPE_ED25519
      case LIBSSH2_HOSTKEY_TYPE_ED25519:
	return "ssh-ed25519";
#endif
      default:
	return "unknown";
      }
  }

  /* keyboard-interactive: answer every prompt with the password kept in the session's abstract */
  void kbdint_cb (const char *, int, const char *, int, int num_prompts, const LIBSSH2_USERAUTH_KBDINT_PROMPT *,
		  LIBSSH2_USERAUTH_KBDINT_RESPONSE *responses, void **abstract)
  {
    const string *pw = (abstract != NULL) ? (const string *) *abstract : NULL;
    for (int i = 0; i < num_prompts; i++)
      {
	responses[i].text = (pw != NULL) ? strdup (pw->c_str ()) : strdup ("");
	responses[i].length = (pw != NULL) ? (unsigned int) pw->size () : 0;
      }
  }

  class ssh_conn
  {
    public:
      LIBSSH2_SESSION *s = NULL;
      LIBSSH2_SFTP *sf = NULL;
      int sock = -1;
      bool broken = false;
      string fp, keytype, user;

      ~ssh_conn ()
      {
	close ();
      }

      void close ()
      {
	if (sf != NULL)
	  {
	    libssh2_sftp_shutdown (sf);
	    sf = NULL;
	  }
	if (s != NULL)
	  {
	    libssh2_session_disconnect (s, "bye");
	    libssh2_session_free (s);
	    s = NULL;
	  }
	if (sock >= 0)
	  {
	    ::close (sock);
	    sock = -1;
	  }
	broken = false;
      }

      bool alive () const
      {
	return s != NULL && !broken;
      }

      string last_error ()
      {
	char *m = NULL;
	if (s == NULL)
	  {
	    return "no session";
	  }
	libssh2_session_last_error (s, &m, NULL, 0);
	return m ? m : "";
      }

      static int tcp_connect (const string &host, int port, int timeout_sec, string *msg)
      {
	struct addrinfo hints, *res = NULL;
	memset (&hints, 0, sizeof (hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	int rc = getaddrinfo (host.c_str (), std::to_string (port).c_str (), &hints, &res);
	if (rc != 0)
	  {
	    *msg = "cannot resolve " + host + ": " + gai_strerror (rc);
	    return -1;
	  }
	int fd = -1;
	string last = "no address";
	for (struct addrinfo *a = res; a != NULL && fd < 0; a = a->ai_next)
	  {
	    fd = socket (a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
	    if (fd < 0)
	      {
		continue;
	      }
	    int fl = fcntl (fd, F_GETFL);
	    fcntl (fd, F_SETFL, fl | O_NONBLOCK);
	    rc = ::connect (fd, a->ai_addr, a->ai_addrlen);
	    if (rc != 0 && errno == EINPROGRESS)
	      {
		struct pollfd p = { fd, POLLOUT, 0 };
		rc = poll (&p, 1, timeout_sec * 1000);
		if (rc == 1)
		  {
		    int err = 0;
		    socklen_t len = sizeof (err);
		    getsockopt (fd, SOL_SOCKET, SO_ERROR, &err, &len);
		    rc = err ? -1 : 0;
		    errno = err;
		  }
		else
		  {
		    rc = -1;
		    errno = ETIMEDOUT;
		  }
	      }
	    if (rc != 0)
	      {
		last = strerror (errno);
		::close (fd);
		fd = -1;
		continue;
	      }
	    fcntl (fd, F_SETFL, fl);
	  }
	freeaddrinfo (res);
	if (fd < 0)
	  {
	    *msg = "cannot connect to " + host + ":" + std::to_string (port) + ": " + last;
	  }
	return fd;
      }

      /* TCP + key exchange; fills fp/keytype. "" on success, else an error code */
      string connect (const string &host, int port, const string &u, string *msg)
      {
	std::call_once (g_ssh_init_once, []
	{
	  libssh2_init (0);
	});
	close ();
	user = u;
	sock = tcp_connect (host, port, 10, msg);
	if (sock < 0)
	  {
	    return "unreachable";
	  }
	s = libssh2_session_init_ex (NULL, NULL, NULL, NULL);
	if (s == NULL)
	  {
	    *msg = "cannot create an ssh session";
	    close ();
	    return "internal";
	  }
	libssh2_session_set_blocking (s, 1);
	libssh2_session_set_timeout (s, 60000);
	if (libssh2_session_handshake (s, sock) != 0)
	  {
	    *msg = "ssh handshake with " + host + " failed: " + last_error ();
	    close ();
	    return "unreachable";
	  }
	size_t klen = 0;
	int ktype = 0;
	const char *key = libssh2_session_hostkey (s, &klen, &ktype);
	const char *hash = libssh2_hostkey_hash (s, LIBSSH2_HOSTKEY_HASH_SHA256);
	if (key == NULL || hash == NULL)
	  {
	    *msg = "cannot read the host key of " + host;
	    close ();
	    return "internal";
	  }
	fp = sha256_fingerprint ((const unsigned char *) hash);
	keytype = hostkey_type_name (ktype);
	return "";
      }

      /* one attempt: password if the server offers it, else keyboard-interactive */
      string auth (const string &password, string *msg)
      {
	char *methods = libssh2_userauth_list (s, user.c_str (), (unsigned int) user.size ());
	if (methods == NULL && libssh2_userauth_authenticated (s))
	  {
	    return "";
	  }
	string m = methods ? methods : "";
	int rc = -1;
	if (m.find ("password") != string::npos)
	  {
	    rc = libssh2_userauth_password (s, user.c_str (), password.c_str ());
	  }
	else if (m.find ("keyboard-interactive") != string::npos)
	  {
	    void **abs = libssh2_session_abstract (s);
	    *abs = (void *) &password;
	    rc = libssh2_userauth_keyboard_interactive (s, user.c_str (), kbdint_cb);
	    *abs = NULL;
	  }
	else
	  {
	    *msg = "the server accepts neither password nor keyboard-interactive login (" + m + ")";
	    return "auth_failed";
	  }
	if (rc == 0)
	  {
	    return "";
	  }
	*msg = "login failed for " + user + " (wrong OS user or password)";
	return "auth_failed";
      }

      void wait_socket (int ms, int extra_fd = -1)
      {
	int dir = libssh2_session_block_directions (s);
	struct pollfd p[2] = { { sock, 0, 0 }, { extra_fd, POLLIN, 0 } };
	if (dir & LIBSSH2_SESSION_BLOCK_INBOUND)
	  {
	    p[0].events |= POLLIN;
	  }
	if (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND)
	  {
	    p[0].events |= POLLOUT;
	  }
	if (p[0].events == 0)
	  {
	    p[0].events = POLLIN;
	  }
	poll (p, extra_fd >= 0 ? 2 : 1, ms);
      }

      bool sftp_ready ()
      {
	if (sf != NULL)
	  {
	    return true;
	  }
	sf = libssh2_sftp_init (s);
	if (sf == NULL)
	  {
	    broken = true;
	  }
	return sf != NULL;
      }

      bool stat_size (const string &path, uint64_t *size)
      {
	if (!sftp_ready ())
	  {
	    return false;
	  }
	LIBSSH2_SFTP_ATTRIBUTES a;
	if (libssh2_sftp_stat (sf, path.c_str (), &a) != 0 || !(a.flags & LIBSSH2_SFTP_ATTR_SIZE))
	  {
	    return false;
	  }
	*size = a.filesize;
	return true;
      }
  };

  /* ------------------------------------------------------------ one-shot commands */

  /* prints the CUBRID environment of the account's login shell (bash/sh: login shell, csh/tcsh: .cshrc) */
  const char *ENV_QUERY =
    "u=$(id -un); sh_=$(getent passwd \"$u\" | cut -d: -f7); echo \"LOGIN_SHELL=$sh_\"; echo \"HOSTNAME=$(uname -n)\";"
    " case \"$sh_\" in"
    " */csh|*/tcsh) \"$sh_\" -c 'echo \"CUBRID=$CUBRID\"; echo \"CUBRID_DATABASES=$CUBRID_DATABASES\";"
    " \"$CUBRID/bin/cubrid_rel\" | grep -m1 CUBRID' ;;"
    " *) bash -lc 'echo \"CUBRID=$CUBRID\"; echo \"CUBRID_DATABASES=$CUBRID_DATABASES\";"
    " \"$CUBRID/bin/cubrid_rel\" | grep -m1 CUBRID' ;;"
    " esac </dev/null 2>/dev/null";

  /* run a command on an authenticated session (blocking), collecting stdout */
  int run_simple (ssh_conn &c, const string &cmd, string *out)
  {
    LIBSSH2_CHANNEL *ch = libssh2_channel_open_session (c.s);
    if (ch == NULL)
      {
	return -1;
      }
    string full = "/bin/sh -c " + shq (cmd);
    if (libssh2_channel_exec (ch, full.c_str ()) != 0)
      {
	libssh2_channel_free (ch);
	return -1;
      }
    libssh2_channel_send_eof (ch);
    char buf[8192];
    ssize_t n;
    while ((n = libssh2_channel_read (ch, buf, sizeof (buf))) > 0)
      {
	out->append (buf, (size_t) n);
      }
    while ((n = libssh2_channel_read_stderr (ch, buf, sizeof (buf))) > 0)
      {
      }
    libssh2_channel_close (ch);
    libssh2_channel_wait_closed (ch);
    int rc = libssh2_channel_get_exit_status (ch);
    libssh2_channel_free (ch);
    return rc;
  }

  int cmd_hostkey (const vector<string> &a)
  {
    int port = 0;
    if (a.size () != 2 || !valid_name (a[0]) || !parse_port (a[1], &port))
      {
	return fail ("usage", "cm_ssh hostkey <host> <port>");
      }
    ssh_conn c;
    string msg;
    string err = c.connect (a[0], port, "", &msg);
    if (!err.empty ())
      {
	return fail (err, msg);
      }
    printf ("%s %s\n", c.keytype.c_str (), c.fp.c_str ());
    return 0;
  }

  string read_password_fd (int fd)
  {
    string pw;
    char ch;
    while (read (fd, &ch, 1) == 1 && ch != '\n')
      {
	pw += ch;
      }
    return pw;
  }

  /*
   * cm_ssh check <host> <port> <user> [<fingerprint>] [--password-fd N]
   * prints key=value lines: result (ok, hostkey_unknown, hostkey_mismatch, auth_failed, unreachable,
   * cubrid_not_found, error), message, hostkey_type, hostkey_sha256 and, after a login, the remote environment.
   */
  int cmd_check (vector<string> a)
  {
    int pfd = -1;
    for (size_t i = 0; i + 1 < a.size (); i++)
      {
	if (a[i] == "--password-fd")
	  {
	    pfd = atoi (a[i + 1].c_str ());
	    a.erase (a.begin () + (long) i, a.begin () + (long) i + 2);
	    break;
	  }
      }
    int port = 0;
    if (a.size () < 3 || a.size () > 4 || !valid_name (a[0]) || !parse_port (a[1], &port) || !valid_name (a[2]))
      {
	return fail ("usage", "cm_ssh check <host> <port> <user> [<fingerprint>] [--password-fd N]");
      }
    string want = a.size () == 4 ? a[3] : "";
    string pw = pfd >= 0 ? read_password_fd (pfd) : "";
    if (pfd >= 0)
      {
	::close (pfd);
      }
    ssh_conn c;
    string msg;
    string err = c.connect (a[0], port, a[2], &msg);
    string result = "ok";
    if (!err.empty ())
      {
	result = err == "unreachable" ? "unreachable" : "error";
      }
    else if (want.empty ())
      {
	result = "hostkey_unknown";
	msg = "confirm the host key " + c.fp + " and check again with it";
      }
    else if (want != c.fp)
      {
	result = "hostkey_mismatch";
	msg = "the host key is " + c.fp + ", not " + want;
      }
    else if (pw.empty ())
      {
	result = "error";
	msg = "a password is needed once the host key is confirmed";
      }
    else
      {
	err = c.auth (pw, &msg);
	if (!err.empty ())
	  {
	    result = err;
	  }
      }
    wipe (pw);
    printf ("result=%s\nmessage=%s\nhostkey_type=%s\nhostkey_sha256=%s\n", result.c_str (), msg.c_str (),
	    c.keytype.c_str (), c.fp.c_str ());
    if (result == "ok")
      {
	string out;
	int rc = run_simple (c, ENV_QUERY, &out);
	bool cubrid = false;
	size_t pos = 0;
	while (pos < out.size ())
	  {
	    size_t nl = out.find ('\n', pos);
	    string l = out.substr (pos, nl == string::npos ? string::npos : nl - pos);
	    pos = nl == string::npos ? out.size () : nl + 1;
	    if (l.compare (0, 12, "LOGIN_SHELL=") == 0 || l.compare (0, 9, "HOSTNAME=") == 0
		|| l.compare (0, 7, "CUBRID=") == 0 || l.compare (0, 17, "CUBRID_DATABASES=") == 0)
	      {
		printf ("remote_%s\n", l.c_str ());
		cubrid = cubrid || (l.compare (0, 7, "CUBRID=") == 0 && l.size () > 7);
	      }
	    else if (l.compare (0, 7, "CUBRID ") == 0)
	      {
		printf ("remote_CUBRID_REL=%s\n", l.c_str ());
	      }
	  }
	if (rc < 0)
	  {
	    printf ("result_after_login=error\n");
	  }
	else if (!cubrid)
	  {
	    printf ("result_after_login=cubrid_not_found\n");
	  }
      }
    c.close ();
    return 0;
  }

  /* ------------------------------------------------------------ agent */

  struct node_t
  {
    string host, user, fp, pw;
    int port = 22;
    bool auth_failed = false;
    string auth_msg;
    ssh_conn conn;
    std::mutex mu;		/* one request at a time per node */
  };

  std::map<string, std::unique_ptr<node_t>> g_nodes;
  std::mutex g_log_mu;

  /* the agent's own log (stderr, sent to --log): requests and failures, never a password */
  void alog (const string &line)
  {
    std::lock_guard<std::mutex> lg (g_log_mu);
    char ts[32];
    time_t t = time (NULL);
    struct tm tmv;
    localtime_r (&t, &tmv);
    strftime (ts, sizeof (ts), "%H:%M:%S", &tmv);
    fprintf (stderr, "[%s] %s\n", ts, line.c_str ());
    fflush (stderr);
  }
  std::atomic<bool> g_quit {false};
  std::atomic<int> g_bound_pid {0};
  string g_bound_start;
  std::mutex g_bind_mu;

  bool parse_credential_line (const string &line, std::unique_ptr<node_t> *out, string *err)
  {
    vector<string> f;
    size_t pos = 0;
    for (int i = 0; i < 4; i++)
      {
	size_t t = line.find ('\t', pos);
	if (t == string::npos)
	  {
	    *err = "a credentials line has fewer than five fields";
	    return false;
	  }
	f.push_back (line.substr (pos, t - pos));
	pos = t + 1;
      }
    std::unique_ptr<node_t> n (new node_t ());
    n->host = f[0];
    n->user = f[2];
    n->fp = f[3];
    n->pw = line.substr (pos);	/* everything after the fourth tab */
    if (!valid_name (n->host) || !parse_port (f[1], &n->port) || !valid_name (n->user)
	|| n->fp.compare (0, 7, "SHA256:") != 0 || n->pw.empty ())
      {
	*err = "bad credentials for " + f[0];
	wipe (n->pw);
	return false;
      }
    *out = std::move (n);
    return true;
  }

  bool read_credentials_fd (int fd, string *err)
  {
    string all, line;
    char buf[4096];
    ssize_t n;
    while ((n = read (fd, buf, sizeof (buf))) > 0)
      {
	all.append (buf, (size_t) n);
      }
    ::close (fd);
    bool ok = true;
    size_t pos = 0;
    while (ok && pos < all.size ())
      {
	size_t nl = all.find ('\n', pos);
	line = all.substr (pos, nl == string::npos ? string::npos : nl - pos);
	pos = nl == string::npos ? all.size () : nl + 1;
	if (line.empty ())
	  {
	    continue;
	  }
	std::unique_ptr<node_t> node;
	ok = parse_credential_line (line, &node, err);
	if (ok)
	  {
	    string h = node->host;
	    g_nodes[h] = std::move (node);
	  }
	wipe (line);
      }
    wipe (all);
    if (ok && g_nodes.empty ())
      {
	*err = "no credentials were given";
	ok = false;
      }
    return ok;
  }

  /* --ask host:port:user:fingerprint ... (the fingerprint itself has a ':') */
  bool ask_credentials (const vector<string> &specs, string *err)
  {
    int tty = open ("/dev/tty", O_RDWR | O_CLOEXEC);
    if (tty < 0)
      {
	*err = "--ask needs a terminal";
	return false;
      }
    for (const string &spec : specs)
      {
	size_t a = spec.find (':'), b = a == string::npos ? a : spec.find (':', a + 1),
	       c = b == string::npos ? b : spec.find (':', b + 1);
	if (c == string::npos)
	  {
	    *err = "bad node " + spec + " (host:port:user:fingerprint)";
	    ::close (tty);
	    return false;
	  }
	string line = spec.substr (0, a) + "\t" + spec.substr (a + 1, b - a - 1) + "\t" + spec.substr (b + 1, c - b - 1)
		      + "\t" + spec.substr (c + 1) + "\t";
	string prompt = "Password of " + spec.substr (b + 1, c - b - 1) + "@" + spec.substr (0, a) + ": ";
	write_all (tty, prompt.data (), prompt.size ());
	struct termios old, noecho;
	tcgetattr (tty, &old);
	noecho = old;
	noecho.c_lflag &= ~(tcflag_t) ECHO;
	tcsetattr (tty, TCSAFLUSH, &noecho);
	string pw = read_password_fd (tty);
	tcsetattr (tty, TCSAFLUSH, &old);
	write_all (tty, "\n", 1);
	line += pw;
	wipe (pw);
	std::unique_ptr<node_t> node;
	bool ok = parse_credential_line (line, &node, err);
	wipe (line);
	if (!ok)
	  {
	    ::close (tty);
	    return false;
	  }
	string h = node->host;
	g_nodes[h] = std::move (node);
      }
    ::close (tty);
    return true;
  }

  /* logged-in session of a node, connecting (once per failure) when needed; caller holds n->mu */
  string ensure_session (node_t *n, string *msg)
  {
    if (n->auth_failed)
      {
	*msg = n->auth_msg + " (not tried again)";
	return "auth_failed";
      }
    if (n->conn.alive ())
      {
	return "";
      }
    string err = n->conn.connect (n->host, n->port, n->user, msg);
    if (!err.empty ())
      {
	return err;
      }
    if (n->conn.fp != n->fp)
      {
	*msg = n->host + ": the host key is " + n->conn.fp + ", not the confirmed " + n->fp;
	n->conn.close ();
	return "hostkey_mismatch";
      }
    err = n->conn.auth (n->pw, msg);
    if (!err.empty ())
      {
	n->auth_failed = true;
	n->auth_msg = *msg;
	n->conn.close ();
	return err;
      }
    return "";
  }

  bool client_gone (int cfd)
  {
    struct pollfd p = { cfd, POLLIN, 0 };
    if (poll (&p, 1, 0) <= 0)
      {
	return false;
      }
    if (p.revents & (POLLHUP | POLLERR))
      {
	return true;
      }
    char c;
    return recv (cfd, &c, 1, MSG_PEEK | MSG_DONTWAIT) == 0;
  }

  /*
   * exec: the command runs as /bin/sh -c '<command>' whatever the account's login shell is.
   * --daemon: the command's output goes to a remote temporary file that is read afterwards, so daemons it starts
   * cannot hold the channel open. With --stdin the client's stdin is forwarded and EOF is sent at its end;
   * without it EOF is sent at once.
   */
  void agent_exec (int cfd, frame_reader &fr, node_t *n, const string &cmd, bool use_stdin, bool daemon)
  {
    string msg;
    string err = ensure_session (n, &msg);
    if (!err.empty ())
      {
	send_frame (cfd, 'F', err + " " + msg);
	return;
      }
    ssh_conn &c = n->conn;
    string body = daemon ? "f=$(mktemp) || exit 125; { " + cmd + "\n} </dev/null >\"$f\" 2>&1; rc=$?; cat \"$f\"; rm -f \"$f\"; exit $rc"
		  : cmd;
    string full = "/bin/sh -c " + shq (body);
    LIBSSH2_CHANNEL *ch = libssh2_channel_open_session (c.s);
    if (ch == NULL)
      {
	c.broken = true;
	/* one reconnect: an idle session may have been dropped by a firewall */
	if (ensure_session (n, &msg).empty ())
	  {
	    ch = libssh2_channel_open_session (c.s);
	  }
      }
    if (ch == NULL || libssh2_channel_exec (ch, full.c_str ()) != 0)
      {
	if (ch != NULL)
	  {
	    libssh2_channel_free (ch);
	  }
	c.broken = true;
	send_frame (cfd, 'F', "unreachable " + n->host + ": cannot run a command: " + c.last_error ());
	return;
      }
    bool stdin_done = !use_stdin;
    bool eof_sent = false;
    string pending;
    libssh2_session_set_blocking (c.s, 0);
    char buf[32768];
    bool aborted = false;
    for (;;)
      {
	/* client -> channel. Frames may already sit in the buffer: the first read, done for the request,
	   can take the whole stdin and its end with it, so the buffer is drained before waiting for more. */
	{
	  struct pollfd p = { cfd, POLLIN, 0 };
	  if (poll (&p, 1, 0) > 0 && !fr.fill (cfd))
	    {
	      aborted = true;
	      break;
	    }
	  char t;
	  string d;
	  while (fr.next (&t, &d))
	    {
	      if (t == 'I')
		{
		  pending += d;
		}
	      else if (t == 'E')
		{
		  stdin_done = true;
		}
	    }
	  if (fr.closed)
	    {
	      /* the client went away (killed or stopped waiting) */
	      aborted = true;
	      break;
	    }
	}
	while (!pending.empty ())
	  {
	    ssize_t w = libssh2_channel_write (ch, pending.data (), pending.size ());
	    if (w == LIBSSH2_ERROR_EAGAIN)
	      {
		break;
	      }
	    if (w < 0)
	      {
		pending.clear ();
		break;
	      }
	    pending.erase (0, (size_t) w);
	  }
	if (stdin_done && pending.empty () && !eof_sent)
	  {
	    int r = libssh2_channel_send_eof (ch);
	    if (r != LIBSSH2_ERROR_EAGAIN)
	      {
		eof_sent = true;
	      }
	  }
	/* channel -> client */
	bool got = false, again = false, error = false;
	for (int is_err = 0; is_err <= 1; is_err++)
	  {
	    ssize_t r = is_err ? libssh2_channel_read_stderr (ch, buf, sizeof (buf)) : libssh2_channel_read (ch, buf, sizeof (buf));
	    if (r == LIBSSH2_ERROR_EAGAIN)
	      {
		again = true;
	      }
	    else if (r < 0)
	      {
		error = true;
	      }
	    else if (r > 0)
	      {
		got = true;
		if (!send_frame (cfd, is_err ? 'e' : 'O', string (buf, (size_t) r)))
		  {
		    aborted = true;
		  }
	      }
	  }
	if (aborted || error)
	  {
	    if (error)
	      {
		c.broken = true;
	      }
	    break;
	  }
	if (libssh2_channel_eof (ch) && !got)
	  {
	    break;
	  }
	if (!got && (again || !pending.empty ()))
	  {
	    c.wait_socket (200, stdin_done ? -1 : cfd);
	  }
      }
    libssh2_session_set_blocking (c.s, 1);
    if (aborted)
      {
	alog ("exec on " + n->host + ": the client went away");
	/* the client went away (the script stopped waiting): leave the remote command alone, drop the channel */
	libssh2_channel_free (ch);
	c.broken = true;
	return;
      }
    libssh2_channel_close (ch);
    libssh2_channel_wait_closed (ch);
    int status = libssh2_channel_get_exit_status (ch);
    libssh2_channel_free (ch);
    if (c.broken)
      {
	send_frame (cfd, 'F', "unreachable " + n->host + ": the connection broke while the command ran");
	return;
      }
    send_frame (cfd, 'X', std::to_string (status));
  }

  /* put: SFTP upload continuing a partial remote file, then size and sha256 compared on the remote side */
  void agent_put (int cfd, node_t *n, const string &local, const string &remote)
  {
    string msg;
    string err = ensure_session (n, &msg);
    if (!err.empty ())
      {
	send_frame (cfd, 'F', err + " " + msg);
	return;
      }
    ssh_conn &c = n->conn;
    struct stat st;
    if (stat (local.c_str (), &st) != 0 || !S_ISREG (st.st_mode))
      {
	send_frame (cfd, 'F', "transfer_failed cannot read " + local);
	return;
      }
    string sum;
    if (!sha256_file (local, &sum))
      {
	send_frame (cfd, 'F', "transfer_failed cannot read " + local);
	return;
      }
    uint64_t size = (uint64_t) st.st_size, have = 0;
    bool ok = false;
    for (int attempt = 1; attempt <= 3 && !ok; attempt++)
      {
	if (!c.alive () && !ensure_session (n, &msg).empty ())
	  {
	    continue;
	  }
	if (!c.sftp_ready ())
	  {
	    continue;
	  }
	if (!c.stat_size (remote, &have) || have > size)
	  {
	    have = 0;
	  }
	unsigned long flags = LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | (have == 0 ? LIBSSH2_FXF_TRUNC : 0);
	LIBSSH2_SFTP_HANDLE *f = libssh2_sftp_open (c.sf, remote.c_str (), flags,
				 LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR);
	if (f == NULL)
	  {
	    send_frame (cfd, 'F', "transfer_failed cannot write " + n->host + ":" + remote + ": " + c.last_error ());
	    return;
	  }
	if (have > 0)
	  {
	    libssh2_sftp_seek64 (f, have);
	  }
	int fd = open (local.c_str (), O_RDONLY | O_CLOEXEC);
	if (fd < 0 || (have > 0 && lseek (fd, (off_t) have, SEEK_SET) < 0))
	  {
	    if (fd >= 0)
	      {
		::close (fd);
	      }
	    libssh2_sftp_close (f);
	    send_frame (cfd, 'F', "transfer_failed cannot read " + local);
	    return;
	  }
	char buf[65536];
	ssize_t r;
	bool broke = false;
	uint64_t done = have;
	time_t last_report = 0;
	while (!broke && (r = read (fd, buf, sizeof (buf))) > 0)
	  {
	    ssize_t off = 0;
	    while (off < r)
	      {
		ssize_t w = libssh2_sftp_write (f, buf + off, (size_t) (r - off));
		if (w <= 0)
		  {
		    broke = true;
		    break;
		  }
		off += w;
	      }
	    done += (uint64_t) off;
	    if (client_gone (cfd))
	      {
		::close (fd);
		libssh2_sftp_close (f);
		return;		/* cancelled: the script removes the partial file in its undo */
	      }
	    if (time (NULL) != last_report)
	      {
		last_report = time (NULL);
		send_frame (cfd, 'O', "progress " + std::to_string (done) + " " + std::to_string (size) + "\n");
	      }
	  }
	::close (fd);
	if (libssh2_sftp_close (f) != 0)
	  {
	    broke = true;
	  }
	if (broke)
	  {
	    c.broken = true;
	    c.close ();
	    send_frame (cfd, 'O', "progress interrupted at " + std::to_string (done) + ", attempt "
			+ std::to_string (attempt) + "\n");
	    continue;
	  }
	ok = true;
      }
    if (!ok)
      {
	send_frame (cfd, 'F', "transfer_failed sending " + local + " to " + n->host + " broke three times");
	return;
      }
    uint64_t rsize = 0;
    string out;
    if (!c.stat_size (remote, &rsize) || rsize != size)
      {
	send_frame (cfd, 'F', "transfer_failed size of " + n->host + ":" + remote + " does not match");
	return;
      }
    int rc = run_simple (c, "sha256sum " + shq (remote), &out);
    if (rc != 0 || out.compare (0, sum.size (), sum) != 0)
      {
	send_frame (cfd, 'F', "transfer_failed checksum of " + n->host + ":" + remote + " does not match");
	return;
      }
    send_frame (cfd, 'O', "sent " + std::to_string (size) + " bytes, sha256 ok\n");
    send_frame (cfd, 'X', "0");
  }

  void handle_client (int cfd)
  {
    struct ucred cr;
    socklen_t len = sizeof (cr);
    if (getsockopt (cfd, SOL_SOCKET, SO_PEERCRED, &cr, &len) != 0 || cr.uid != getuid ())
      {
	::close (cfd);
	return;
      }
    frame_reader fr;
    char type;
    string data;
    if (!read_frame (cfd, fr, &type, &data) || type != 'R')
      {
	::close (cfd);
	return;
      }
    vector<string> a;
    size_t pos = 0;
    while (pos <= data.size ())
      {
	size_t z = data.find ('\0', pos);
	a.push_back (data.substr (pos, z == string::npos ? string::npos : z - pos));
	if (z == string::npos)
	  {
	    break;
	  }
	pos = z + 1;
      }
    fcntl (cfd, F_SETFL, fcntl (cfd, F_GETFL) | O_NONBLOCK);
    if (a.empty ())
      {
	::close (cfd);
	return;
      }
    alog ("request " + a[0] + (a.size () > 1 ? " " + a[1] : string ("")));
    if (a[0] == "quit")
      {
	g_quit = true;
	send_frame (cfd, 'X', "0");
      }
    else if (a[0] == "bind" && a.size () == 2)
      {
	pid_t pid = (pid_t) atoi (a[1].c_str ());
	string start = pid > 0 ? proc_start_time (pid) : "";
	if (start.empty ())
	  {
	    send_frame (cfd, 'F', "usage no such process " + a[1]);
	  }
	else
	  {
	    std::lock_guard<std::mutex> lg (g_bind_mu);
	    g_bound_start = start;
	    g_bound_pid = (int) pid;
	    send_frame (cfd, 'X', "0");
	  }
      }
    else if ((a[0] == "exec" && a.size () == 5) || (a[0] == "put" && a.size () == 4))
      {
	auto it = g_nodes.find (a[1]);
	if (it == g_nodes.end ())
	  {
	    send_frame (cfd, 'F', "unknown_host no credentials for " + a[1]);
	  }
	else
	  {
	    node_t *n = it->second.get ();
	    std::lock_guard<std::mutex> lg (n->mu);
	    if (a[0] == "exec")
	      {
		agent_exec (cfd, fr, n, a[4], a[2] == "1", a[3] == "1");
		alog ("exec on " + a[1] + " done");
	      }
	    else
	      {
		agent_put (cfd, n, a[2], a[3]);
	      }
	  }
      }
    else
      {
	send_frame (cfd, 'F', "usage unknown request " + a[0]);
      }
    ::close (cfd);
  }

  /* exits when told to, when the bound script is gone, or when nobody binds within 30 seconds */
  void watchdog ()
  {
    time_t started = time (NULL);
    while (!g_quit)
      {
	sleep (1);
	int pid = g_bound_pid;
	if (pid == 0)
	  {
	    if (time (NULL) - started > 30)
	      {
		g_quit = true;
	      }
	    continue;
	  }
	string start;
	{
	  std::lock_guard<std::mutex> lg (g_bind_mu);
	  start = g_bound_start;
	}
	if (proc_start_time ((pid_t) pid) != start)
	  {
	    g_quit = true;
	  }
      }
  }

  int cmd_agent (const vector<string> &a)
  {
    string sock_path, log_path, err;
    int cred_fd = -1;
    vector<string> ask;
    for (size_t i = 0; i < a.size (); i++)
      {
	if (a[i] == "--socket" && i + 1 < a.size ())
	  {
	    sock_path = a[++i];
	  }
	else if (a[i] == "--credentials-fd" && i + 1 < a.size ())
	  {
	    cred_fd = atoi (a[++i].c_str ());
	  }
	else if (a[i] == "--log" && i + 1 < a.size ())
	  {
	    log_path = a[++i];
	  }
	else if (a[i] == "--ask")
	  {
	    ask.assign (a.begin () + (long) i + 1, a.end ());
	    break;
	  }
	else
	  {
	    return fail ("usage", "unknown agent option " + a[i]);
	  }
      }
    struct sockaddr_un sa;
    if (sock_path.empty () || sock_path.size () >= sizeof (sa.sun_path) || (cred_fd < 0 && ask.empty ()))
      {
	return fail ("usage", "cm_ssh agent --socket <path, shorter than " + std::to_string (sizeof (sa.sun_path))
		     + "> (--credentials-fd N | --ask host:port:user:fingerprint ...)");
      }
    /* nobody, not even this account, reads this process's memory or gets a core of it */
    prctl (PR_SET_DUMPABLE, 0, 0, 0, 0);
    struct rlimit nocore = { 0, 0 };
    setrlimit (RLIMIT_CORE, &nocore);

    bool ok = cred_fd >= 0 ? read_credentials_fd (cred_fd, &err) : ask_credentials (ask, &err);
    if (!ok)
      {
	return fail ("usage", err);
      }
    int ls = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    memset (&sa, 0, sizeof (sa));
    sa.sun_family = AF_UNIX;
    strncpy (sa.sun_path, sock_path.c_str (), sizeof (sa.sun_path) - 1);
    unlink (sock_path.c_str ());
    mode_t old = umask (077);
    int rc = bind (ls, (struct sockaddr *) &sa, sizeof (sa));
    umask (old);
    if (ls < 0 || rc != 0 || listen (ls, 16) != 0)
      {
	return fail ("internal", "cannot listen on " + sock_path + ": " + strerror (errno));
      }
    fflush (stdout);
    pid_t child = fork ();
    if (child < 0)
      {
	return fail ("internal", "fork failed");
      }
    if (child > 0)
      {
	/* the parent only reports; the child serves */
	printf ("READY %d\n", (int) child);
	fflush (stdout);
	_exit (0);
      }
    setsid ();
    signal (SIGPIPE, SIG_IGN);
    signal (SIGHUP, SIG_IGN);
    int devnull = open ("/dev/null", O_RDWR);
    int logfd = log_path.empty () ? -1 : open (log_path.c_str (), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    dup2 (devnull, 0);
    dup2 (devnull, 1);
    dup2 (logfd >= 0 ? logfd : devnull, 2);
    if (devnull > 2)
      {
	::close (devnull);
      }
    std::thread (watchdog).detach ();
    while (!g_quit)
      {
	struct pollfd p = { ls, POLLIN, 0 };
	if (poll (&p, 1, 1000) <= 0)
	  {
	    continue;
	  }
	/* accept + FD_CLOEXEC rather than accept4, which needs glibc 2.10 (the helper is built for glibc 2.7 and up) */
	int cfd = accept (ls, NULL, NULL);
	if (cfd < 0)
	  {
	    continue;
	  }
	fcntl (cfd, F_SETFD, FD_CLOEXEC);
	std::thread (handle_client, cfd).detach ();
      }
    ::close (ls);
    unlink (sock_path.c_str ());
    for (auto &kv : g_nodes)
      {
	wipe (kv.second->pw);
      }
    _exit (0);
  }

  /* ------------------------------------------------------------ clients */

  int connect_agent (const string &path)
  {
    struct sockaddr_un sa;
    if (path.empty () || path.size () >= sizeof (sa.sun_path))
      {
	return -1;
      }
    int fd = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    memset (&sa, 0, sizeof (sa));
    sa.sun_family = AF_UNIX;
    strncpy (sa.sun_path, path.c_str (), sizeof (sa.sun_path) - 1);
    if (fd < 0 || connect (fd, (struct sockaddr *) &sa, sizeof (sa)) != 0)
      {
	if (fd >= 0)
	  {
	    ::close (fd);
	  }
	return -1;
      }
    return fd;
  }

  /* sends a request and relays the agent's frames to stdout/stderr; with_stdin forwards this process's stdin */
  int run_client (const string &agent, const vector<string> &req, bool with_stdin)
  {
    int fd = connect_agent (agent);
    if (fd < 0)
      {
	return fail ("no_agent", "cannot reach the agent at " + agent);
      }
    string data;
    for (size_t i = 0; i < req.size (); i++)
      {
	data += (i ? string (1, '\0') : string ()) + req[i];
      }
    if (!send_frame (fd, 'R', data))
      {
	return fail ("no_agent", "the agent closed the connection");
      }
    bool in_done = !with_stdin;
    if (in_done && req[0] == "exec")
      {
	send_frame (fd, 'E', "");
      }
    frame_reader fr;
    for (;;)
      {
	struct pollfd p[2] = { { fd, POLLIN, 0 }, { 0, POLLIN, 0 } };
	int np = in_done ? 1 : 2;
	if (poll (p, (nfds_t) np, -1) < 0 && errno != EINTR)
	  {
	    break;
	  }
	if (!in_done && (p[1].revents & (POLLIN | POLLHUP)))
	  {
	    char buf[32768];
	    ssize_t n = read (0, buf, sizeof (buf));
	    if (n > 0)
	      {
		send_frame (fd, 'I', string (buf, (size_t) n));
	      }
	    else
	      {
		send_frame (fd, 'E', "");
		in_done = true;
	      }
	  }
	if (p[0].revents & (POLLIN | POLLHUP))
	  {
	    if (!fr.fill (fd))
	      {
		break;
	      }
	    char t;
	    string d;
	    while (fr.next (&t, &d))
	      {
		if (t == 'O')
		  {
		    write_all (1, d.data (), d.size ());
		  }
		else if (t == 'e')
		  {
		    write_all (2, d.data (), d.size ());
		  }
		else if (t == 'X')
		  {
		    ::close (fd);
		    return atoi (d.c_str ());
		  }
		else if (t == 'F')
		  {
		    ::close (fd);
		    size_t sp = d.find (' ');
		    return fail (d.substr (0, sp), sp == string::npos ? "" : d.substr (sp + 1));
		  }
	      }
	    if (fr.closed)
	      {
		break;
	      }
	  }
      }
    ::close (fd);
    return fail ("no_agent", "the agent closed the connection");
  }

  int usage ()
  {
    fprintf (stderr,
	     "usage:\n"
	     "  cm_ssh hostkey <host> <port>\n"
	     "  cm_ssh check <host> <port> <user> [<SHA256:fingerprint>] [--password-fd N]\n"
	     "  cm_ssh agent --socket <path> (--credentials-fd N | --ask <host>:<port>:<user>:<fingerprint> ...) [--log <file>]\n"
	     "  cm_ssh bind --agent <socket> <pid>\n"
	     "  cm_ssh exec --agent <socket> <host> [--stdin] [--daemon] -- <command>\n"
	     "  cm_ssh put --agent <socket> <host> <local file> <remote file>\n"
	     "  cm_ssh quit --agent <socket>\n");
    return EXIT_HELPER;
  }
}

int
main (int argc, char *argv[])
{
  signal (SIGPIPE, SIG_IGN);
  if (argc < 2)
    {
      return usage ();
    }
  string cmd = argv[1];
  vector<string> a (argv + 2, argv + argc);
  if (cmd == "hostkey")
    {
      return cmd_hostkey (a);
    }
  if (cmd == "check")
    {
      return cmd_check (a);
    }
  if (cmd == "agent")
    {
      return cmd_agent (a);
    }
  if (a.size () < 2 || a[0] != "--agent")
    {
      return usage ();
    }
  string agent = a[1];
  a.erase (a.begin (), a.begin () + 2);
  if (cmd == "quit" && a.empty ())
    {
      return run_client (agent, { "quit" }, false);
    }
  if (cmd == "bind" && a.size () == 1)
    {
      return run_client (agent, { "bind", a[0] }, false);
    }
  if (cmd == "put" && a.size () == 3)
    {
      return run_client (agent, { "put", a[0], a[1], a[2] }, false);
    }
  if (cmd == "exec" && !a.empty ())
    {
      string host = a[0];
      bool with_stdin = false, daemon = false;
      size_t i = 1;
      for (; i < a.size () && a[i] != "--"; i++)
	{
	  if (a[i] == "--stdin")
	    {
	      with_stdin = true;
	    }
	  else if (a[i] == "--daemon")
	    {
	      daemon = true;
	    }
	  else
	    {
	      return usage ();
	    }
	}
      if (i + 1 >= a.size ())
	{
	  return usage ();
	}
      string command;
      for (size_t k = i + 1; k < a.size (); k++)
	{
	  command += (k > i + 1 ? " " : "") + a[k];
	}
      return run_client (agent, { "exec", host, with_stdin ? "1" : "0", daemon ? "1" : "0", command }, with_stdin);
    }
  return usage ();
}
