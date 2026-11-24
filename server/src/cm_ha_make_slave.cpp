/*
 * HA slave POC (plan 2 of the HA slave review), run on the master's CMS: it converts a single
 * database here into an HA master, and makes a new server the slave of this master. Work on
 * the new slave and the replicas goes through libssh2 (command execution and SFTP); work on
 * this server runs locally. No ha_make_slavedb.sh, no system ssh/scp.
 * Tasks and steps: docs/ha-make-slave-poc-api.md.
 *
 * The OS password lives only in memory (the request value is blanked on arrival,
 * the job copy is wiped when the job ends). It never goes to a command line, a log,
 * the state file, or the CMS async request copies (these tasks do not use them).
 */

#if !defined(WINDOWS)

#include "cm_log.h"
#include "cm_dep.h"
#include "cm_cmd_exec.h"
#include "cm_server_util.h"
#include "cm_server_extend_interface.h"
#include "cm_ha_make_slave.h"

#include <libssh2.h>
#include <libssh2_sftp.h>
#include <openssl/evp.h>

#include <atomic>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <netdb.h>
#include <sys/socket.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

using std::string;
using std::vector;

namespace
{
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

  string trim (const string &s)
  {
    size_t b = s.find_first_not_of (" \t\r\n");
    if (b == string::npos)
      {
	return "";
      }
    size_t e = s.find_last_not_of (" \t\r\n");
    return s.substr (b, e - b + 1);
  }

  vector<string> split_any (const string &s, const char *seps)
  {
    vector<string> out;
    string cur;
    for (char c : s)
      {
	if (strchr (seps, c) != NULL)
	  {
	    if (!cur.empty ())
	      {
		out.push_back (cur);
	      }
	    cur.clear ();
	  }
	else
	  {
	    cur += c;
	  }
      }
    if (!cur.empty ())
      {
	out.push_back (cur);
      }
    return out;
  }

  vector<string> lines_of (const string &s)
  {
    vector<string> out;
    std::istringstream in (s);
    string line;
    while (std::getline (in, line))
      {
	if (!line.empty () && line.back () == '\r')
	  {
	    line.pop_back ();
	  }
	out.push_back (line);
      }
    return out;
  }

  string join (const vector<string> &v, const string &sep)
  {
    string out;
    for (size_t i = 0; i < v.size (); i++)
      {
	out += (i ? sep : "") + v[i];
      }
    return out;
  }

  bool contains (const vector<string> &v, const string &x)
  {
    for (const string &s : v)
      {
	if (s == x)
	  {
	    return true;
	  }
      }
    return false;
  }

  /* host or db name: letters, digits, '.', '_', '-' and not starting with '-' */
  bool valid_name (const string &s)
  {
    if (s.empty () || s.size () > 255 || s[0] == '-')
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

  bool valid_path (const string &s)
  {
    if (s.size () < 2 || s.size () > 1000 || s[0] != '/' || s.find ("..") != string::npos)
      {
	return false;
      }
    for (char c : s)
      {
	if (!isalnum ((unsigned char) c) && c != '.' && c != '_' && c != '-' && c != '/')
	  {
	    return false;
	  }
      }
    return true;
  }

  /* single-quote for a POSIX shell; inputs are validated as well */
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

  /* NCA sends numbers as strings ("22"); accept both */
  string req_str (const Json::Value &req, const char *key, const string &def = "")
  {
    const Json::Value &v = req[key];
    if (v.isString ())
      {
	return v.asString ();
      }
    if (v.isIntegral ())
      {
	return std::to_string (v.asInt ());
      }
    return def;
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

  /* "CUBRID 11.4 (11.4.5.1908-0ee2e8d) (64bit ...)" -> "11.4.5.1908" */
  string parse_version (const string &rel)
  {
    size_t p = rel.find ('(');
    while (p != string::npos)
      {
	size_t e = p + 1;
	while (e < rel.size () && (isdigit ((unsigned char) rel[e]) || rel[e] == '.'))
	  {
	    e++;
	  }
	if (e > p + 1 && rel.find ('.', p) < e)
	  {
	    return rel.substr (p + 1, e - p - 1);
	  }
	p = rel.find ('(', p + 1);
      }
    return "";
  }

  string now_str ()
  {
    char buf[32];
    time_t t = time (NULL);
    struct tm tmv;
    localtime_r (&t, &tmv);
    strftime (buf, sizeof (buf), "%H:%M:%S", &tmv);
    return buf;
  }

  string local_hostname ()
  {
    char buf[256] = { 0 };
    if (gethostname (buf, sizeof (buf) - 1) != 0)
      {
	return "";
      }
    return buf;
  }

  string env_or (const char *name, const string &def)
  {
    const char *v = getenv (name);
    return (v != NULL && *v != '\0') ? string (v) : def;
  }

  bool read_local_file (const string &path, string *data, bool *exists)
  {
    std::ifstream in (path.c_str (), std::ios::binary);
    if (!in)
      {
	*exists = (access (path.c_str (), F_OK) == 0);
	return !*exists;
      }
    *exists = true;
    std::ostringstream ss;
    ss << in.rdbuf ();
    *data = ss.str ();
    return true;
  }

  bool write_local_file (const string &path, const string &data)
  {
    string tmp = path + ".tmp." + std::to_string (getpid ());
    {
      std::ofstream out (tmp.c_str (), std::ios::binary | std::ios::trunc);
      if (!out)
	{
	  return false;
	}
      out << data;
      if (!out.good ())
	{
	  return false;
	}
    }
    return rename (tmp.c_str (), path.c_str ()) == 0;
  }

  bool is_dir (const string &path)
  {
    struct stat st;
    return stat (path.c_str (), &st) == 0 && S_ISDIR (st.st_mode);
  }

  bool mkdir_p (const string &path)
  {
    string cur;
    for (const string &part : split_any (path, "/"))
      {
	cur += "/" + part;
	if (mkdir (cur.c_str (), 0755) != 0 && errno != EEXIST)
	  {
	    return false;
	  }
      }
    return is_dir (path);
  }

  /* rm -rf without a shell */
  bool remove_tree (const string &path)
  {
    struct stat st;
    if (lstat (path.c_str (), &st) != 0)
      {
	return errno == ENOENT;
      }
    if (!S_ISDIR (st.st_mode))
      {
	return unlink (path.c_str ()) == 0;
      }
    DIR *d = opendir (path.c_str ());
    if (d == NULL)
      {
	return false;
      }
    bool ok = true;
    struct dirent *e;
    while ((e = readdir (d)) != NULL)
      {
	if (strcmp (e->d_name, ".") == 0 || strcmp (e->d_name, "..") == 0)
	  {
	    continue;
	  }
	ok = remove_tree (path + "/" + e->d_name) && ok;
      }
    closedir (d);
    return rmdir (path.c_str ()) == 0 && ok;
  }

  bool dir_has_entries (const string &path)
  {
    DIR *d = opendir (path.c_str ());
    if (d == NULL)
      {
	return false;
      }
    bool any = false;
    struct dirent *e;
    while ((e = readdir (d)) != NULL)
      {
	if (strcmp (e->d_name, ".") != 0 && strcmp (e->d_name, "..") != 0)
	  {
	    any = true;
	    break;
	  }
      }
    closedir (d);
    return any;
  }

  bool sha256_local (const string &path, string *hex)
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

  /* ------------------------------------------------------------ conf files */

  /* key=value files with [section] headers (cubrid.conf, cubrid_ha.conf) */
  string conf_get (const string &content, const string &section, const string &key)
  {
    string cur, value;
    for (const string &raw : lines_of (content))
      {
	string line = trim (raw);
	if (line.empty () || line[0] == '#')
	  {
	    continue;
	  }
	if (line[0] == '[')
	  {
	    cur = line;
	    continue;
	  }
	size_t eq = line.find ('=');
	if (cur == section && eq != string::npos && trim (line.substr (0, eq)) == key)
	  {
	    value = trim (line.substr (eq + 1));
	  }
      }
    return value;
  }

  /* key/value pairs of one section, last value wins, in first-seen order */
  vector<std::pair<string, string>> conf_pairs (const string &content, const string &section)
  {
    vector<std::pair<string, string>> out;
    string cur;
    for (const string &raw : lines_of (content))
      {
	string line = trim (raw);
	if (line.empty () || line[0] == '#')
	  {
	    continue;
	  }
	if (line[0] == '[')
	  {
	    cur = line;
	    continue;
	  }
	size_t eq = line.find ('=');
	if (cur != section || eq == string::npos)
	  {
	    continue;
	  }
	string k = trim (line.substr (0, eq)), v = trim (line.substr (eq + 1));
	bool found = false;
	for (auto &kv : out)
	  {
	    if (kv.first == k)
	      {
		kv.second = v;
		found = true;
	      }
	  }
	if (!found)
	  {
	    out.push_back (std::make_pair (k, v));
	  }
      }
    return out;
  }

  string conf_set (const string &content, const string &section, const string &key, const string &value)
  {
    vector<string> ls = lines_of (content);
    string cur;
    int sec_line = -1, sec_last = -1, key_line = -1;
    for (int i = 0; i < (int) ls.size (); i++)
      {
	string line = trim (ls[i]);
	if (!line.empty () && line[0] == '[')
	  {
	    cur = line;
	    if (cur == section && sec_line < 0)
	      {
		sec_line = i;
		sec_last = i;
	      }
	    continue;
	  }
	if (cur != section)
	  {
	    continue;
	  }
	if (!line.empty ())
	  {
	    sec_last = i;
	  }
	size_t eq = line.find ('=');
	if (!line.empty () && line[0] != '#' && eq != string::npos && trim (line.substr (0, eq)) == key)
	  {
	    key_line = i;
	  }
      }
    string kv = key + "=" + value;
    if (key_line >= 0)
      {
	ls[key_line] = kv;
      }
    else if (sec_line >= 0)
      {
	ls.insert (ls.begin () + sec_last + 1, kv);
      }
    else
      {
	if (!ls.empty () && !trim (ls.back ()).empty ())
	  {
	    ls.push_back ("");
	  }
	ls.push_back (section);
	ls.push_back (kv);
      }
    return join (ls, "\n") + "\n";
  }

  /* "group@h1:h2" + host */
  string node_list_add (const string &list, const string &host, bool *changed)
  {
    size_t at = list.find ('@');
    string group = (at == string::npos) ? "" : list.substr (0, at);
    vector<string> hosts = split_any (at == string::npos ? list : list.substr (at + 1), ":");
    *changed = !contains (hosts, host);
    if (*changed)
      {
	hosts.push_back (host);
      }
    return group + "@" + join (hosts, ":");
  }

  /* ha_copy_sync_mode has one value per node; reload fails when it is short */
  string sync_mode_for (const string &mode, size_t nodes)
  {
    if (mode.empty ())
      {
	return mode;
      }
    vector<string> v = split_any (mode, ":");
    while (v.size () < nodes)
      {
	v.push_back (v.empty () ? "sync" : v.back ());
      }
    return join (v, ":");
  }

  vector<string> node_list_hosts (const string &list)
  {
    size_t at = list.find ('@');
    return split_any (at == string::npos ? list : list.substr (at + 1), ":");
  }

  /* databases.txt: "db vol hosts log [lob]" */
  vector<string> dbtxt_line (const string &content, const string &db)
  {
    for (const string &raw : lines_of (content))
      {
	string line = trim (raw);
	if (line.empty () || line[0] == '#')
	  {
	    continue;
	  }
	vector<string> t = split_any (line, " \t");
	if (!t.empty () && t[0] == db)
	  {
	    return t;
	  }
      }
    return vector<string> ();
  }

  string dbtxt_put (const string &content, const string &db, const vector<string> &tokens)
  {
    vector<string> ls = lines_of (content);
    string newline = join (tokens, " ");
    for (string &raw : ls)
      {
	vector<string> t = split_any (trim (raw), " \t");
	if (!t.empty () && t[0] == db && trim (raw)[0] != '#')
	  {
	    raw = newline;
	    return join (ls, "\n") + "\n";
	  }
      }
    while (!ls.empty () && trim (ls.back ()).empty ())
      {
	ls.pop_back ();
      }
    ls.push_back (newline);
    return join (ls, "\n") + "\n";
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
  void kbdint_cb (const char *name, int name_len, const char *instruction, int instruction_len, int num_prompts,
		  const LIBSSH2_USERAUTH_KBDINT_PROMPT *prompts, LIBSSH2_USERAUTH_KBDINT_RESPONSE *responses, void **abstract)
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
      const string *auth_pw = NULL;	/* only during auth (), read by kbdint_cb */

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

      /* TCP connect with a timeout */
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
	    fd = socket (a->ai_family, a->ai_socktype, a->ai_protocol);
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
	s = libssh2_session_init_ex (NULL, NULL, NULL, (void *) this);
	if (s == NULL)
	  {
	    *msg = "cannot create an ssh session";
	    close ();
	    return "error";
	  }
	libssh2_session_set_blocking (s, 1);
	libssh2_session_set_timeout (s, 60000);	/* no blocking call waits more than 60 s */
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
	    *msg = "cannot read the host key";
	    close ();
	    return "error";
	  }
	fp = sha256_fingerprint ((const unsigned char *) hash);
	keytype = hostkey_type_name (ktype);
	return "";
      }

      string auth (const string &password, string *msg)
      {
	char *methods = libssh2_userauth_list (s, user.c_str (), (unsigned int) user.size ());
	if (methods == NULL && libssh2_userauth_authenticated (s))
	  {
	    return "";
	  }
	string m = methods ? methods : "";
	if (m.find ("password") != string::npos
	    && libssh2_userauth_password (s, user.c_str (), password.c_str ()) == 0)
	  {
	    return "";
	  }
	if (m.find ("keyboard-interactive") != string::npos)
	  {
	    void **abs = libssh2_session_abstract (s);
	    *abs = (void *) &password;
	    int rc = libssh2_userauth_keyboard_interactive (s, user.c_str (), kbdint_cb);
	    *abs = NULL;
	    if (rc == 0)
	      {
		return "";
	      }
	  }
	if (m.find ("password") == string::npos && m.find ("keyboard-interactive") == string::npos)
	  {
	    *msg = "the server accepts neither password nor keyboard-interactive login (" + m + ")";
	  }
	else
	  {
	    *msg = "login failed (wrong OS user or password)";
	  }
	return "auth_failed";
      }

      void wait_socket (int ms)
      {
	int dir = libssh2_session_block_directions (s);
	struct pollfd p = { sock, 0, 0 };
	if (dir & LIBSSH2_SESSION_BLOCK_INBOUND)
	  {
	    p.events |= POLLIN;
	  }
	if (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND)
	  {
	    p.events |= POLLOUT;
	  }
	if (p.events == 0)
	  {
	    p.events = POLLIN;
	  }
	poll (&p, 1, ms);
      }

      /*
       * run `bash -lc '<cmd>'` (login shell: the account's CUBRID environment).
       * returns the exit status, -1 on a channel error, -2 when cancelled, -3 on timeout.
       */
      int exec (const string &cmd, string *out, const std::function<void (const string &)> &on_line,
		const std::atomic<bool> *cancel, int timeout_sec)
      {
	LIBSSH2_CHANNEL *ch = libssh2_channel_open_session (s);
	if (ch == NULL)
	  {
	    broken = true;
	    return -1;
	  }
	/* the redirect applies to the whole command, not to the last command of a pipeline */
	string full = "bash -lc " + shq ("{ " + cmd + "\n} </dev/null");
	if (libssh2_channel_exec (ch, full.c_str ()) != 0)
	  {
	    libssh2_channel_free (ch);
	    broken = true;
	    return -1;
	  }
	libssh2_session_set_blocking (s, 0);
	time_t start = time (NULL);
	string pending;
	char buf[8192];
	int result = 0;
	for (;;)
	  {
	    bool got = false, again = false;
	    for (int is_err = 0; is_err <= 1; is_err++)
	      {
		ssize_t n = is_err ? libssh2_channel_read_stderr (ch, buf, sizeof (buf))
			    : libssh2_channel_read (ch, buf, sizeof (buf));
		if (n == LIBSSH2_ERROR_EAGAIN)
		  {
		    again = true;
		    continue;
		  }
		if (n < 0)
		  {
		    result = -1;
		    break;
		  }
		if (n > 0)
		  {
		    got = true;
		    string chunk (buf, n);
		    if (out != NULL)
		      {
			*out += chunk;
		      }
		    pending += chunk;
		    size_t nl;
		    while ((nl = pending.find ('\n')) != string::npos)
		      {
			if (on_line)
			  {
			    on_line (pending.substr (0, nl));
			  }
			pending.erase (0, nl + 1);
		      }
		  }
	      }
	    if (result != 0)
	      {
		break;
	      }
	    if (libssh2_channel_eof (ch) && !got)
	      {
		break;
	      }
	    if (cancel != NULL && cancel->load ())
	      {
		libssh2_channel_signal_ex (ch, "TERM", 4);
		result = -2;
		break;
	      }
	    if (timeout_sec > 0 && time (NULL) - start > timeout_sec)
	      {
		libssh2_channel_signal_ex (ch, "TERM", 4);
		result = -3;
		break;
	      }
	    if (!got && again)
	      {
		wait_socket (300);
	      }
	  }
	if (!pending.empty () && on_line)
	  {
	    on_line (pending);
	  }
	libssh2_session_set_blocking (s, 1);
	libssh2_channel_close (ch);
	if (result == 0)
	  {
	    libssh2_channel_wait_closed (ch);
	    result = libssh2_channel_get_exit_status (ch);
	  }
	else if (result == -1)
	  {
	    broken = true;
	  }
	libssh2_channel_free (ch);
	return result;
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

      /* false on error; *exists=false (and true returned) when the file is missing */
      bool read_file (const string &path, string *data, bool *exists)
      {
	*exists = false;
	data->clear ();
	if (!sftp_ready ())
	  {
	    return false;
	  }
	LIBSSH2_SFTP_HANDLE *f = libssh2_sftp_open (sf, path.c_str (), LIBSSH2_FXF_READ, 0);
	if (f == NULL)
	  {
	    return libssh2_sftp_last_error (sf) == LIBSSH2_FX_NO_SUCH_FILE;
	  }
	*exists = true;
	char buf[16384];
	ssize_t n;
	while ((n = libssh2_sftp_read (f, buf, sizeof (buf))) > 0)
	  {
	    data->append (buf, n);
	  }
	libssh2_sftp_close (f);
	return n == 0;
      }

      bool write_file (const string &path, const string &data)
      {
	if (!sftp_ready ())
	  {
	    return false;
	  }
	LIBSSH2_SFTP_HANDLE *f = libssh2_sftp_open (sf, path.c_str (),
				 LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
				 LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH);
	if (f == NULL)
	  {
	    return false;
	  }
	size_t off = 0;
	while (off < data.size ())
	  {
	    ssize_t n = libssh2_sftp_write (f, data.data () + off, std::min ((size_t) 32768, data.size () - off));
	    if (n <= 0)
	      {
		libssh2_sftp_close (f);
		return false;
	      }
	    off += n;
	  }
	return libssh2_sftp_close (f) == 0;
      }

      /* regular files in a remote folder with their sizes */
      bool list_files (const string &dir, vector<std::pair<string, uint64_t>> *files)
      {
	if (!sftp_ready ())
	  {
	    return false;
	  }
	LIBSSH2_SFTP_HANDLE *d = libssh2_sftp_opendir (sf, dir.c_str ());
	if (d == NULL)
	  {
	    return false;
	  }
	char name[1024];
	LIBSSH2_SFTP_ATTRIBUTES a;
	int n;
	while ((n = libssh2_sftp_readdir (d, name, sizeof (name), &a)) > 0)
	  {
	    if ((a.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && LIBSSH2_SFTP_S_ISREG (a.permissions))
	      {
		files->push_back (std::make_pair (string (name, n), (uint64_t) a.filesize));
	      }
	  }
	libssh2_sftp_closedir (d);
	return n == 0;
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

      /* download, continuing a partial local file; false on error or cancel */
      bool download (const string &remote, const string &local, uint64_t size, const std::atomic<bool> *cancel,
		     string *msg)
      {
	if (!sftp_ready ())
	  {
	    *msg = "SFTP is not available: " + last_error ();
	    return false;
	  }
	struct stat st;
	uint64_t have = (stat (local.c_str (), &st) == 0) ? (uint64_t) st.st_size : 0;
	if (have > size)
	  {
	    unlink (local.c_str ());
	    have = 0;
	  }
	if (have == size)
	  {
	    return true;
	  }
	LIBSSH2_SFTP_HANDLE *f = libssh2_sftp_open (sf, remote.c_str (), LIBSSH2_FXF_READ, 0);
	if (f == NULL)
	  {
	    *msg = "cannot open " + remote + ": " + last_error ();
	    return false;
	  }
	if (have > 0)
	  {
	    libssh2_sftp_seek64 (f, have);
	  }
	int fd = open (local.c_str (), O_WRONLY | O_CREAT | (have > 0 ? O_APPEND : O_TRUNC), 0600);
	if (fd < 0)
	  {
	    libssh2_sftp_close (f);
	    *msg = "cannot write " + local + ": " + strerror (errno);
	    return false;
	  }
	char buf[65536];
	ssize_t n;
	bool ok = true;
	while ((n = libssh2_sftp_read (f, buf, sizeof (buf))) > 0)
	  {
	    if (write (fd, buf, n) != n)
	      {
		*msg = "cannot write " + local + ": " + strerror (errno);
		ok = false;
		break;
	      }
	    if (cancel != NULL && cancel->load ())
	      {
		*msg = "cancelled";
		ok = false;
		break;
	      }
	  }
	if (n < 0)
	  {
	    *msg = "read failed on " + remote + ": " + last_error ();
	    broken = true;
	    ok = false;
	  }
	::close (fd);
	libssh2_sftp_close (f);
	return ok;
      }

      /* upload, continuing a partial remote file; false on error or cancel */
      bool upload (const string &local, const string &remote, const std::atomic<bool> *cancel, string *msg)
      {
	if (!sftp_ready ())
	  {
	    *msg = "SFTP is not available: " + last_error ();
	    return false;
	  }
	struct stat st;
	if (stat (local.c_str (), &st) != 0)
	  {
	    *msg = "cannot read " + local + ": " + strerror (errno);
	    return false;
	  }
	uint64_t size = (uint64_t) st.st_size, have = 0;
	if (!stat_size (remote, &have) || have > size)
	  {
	    have = 0;
	  }
	if (have == size && size > 0)
	  {
	    return true;
	  }
	unsigned long flags = LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | (have == 0 ? LIBSSH2_FXF_TRUNC : 0);
	LIBSSH2_SFTP_HANDLE *f = libssh2_sftp_open (sf, remote.c_str (), flags, LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR);
	if (f == NULL)
	  {
	    *msg = "cannot write " + remote + ": " + last_error ();
	    return false;
	  }
	if (have > 0)
	  {
	    libssh2_sftp_seek64 (f, have);
	  }
	int fd = open (local.c_str (), O_RDONLY);
	if (fd < 0 || (have > 0 && lseek (fd, (off_t) have, SEEK_SET) < 0))
	  {
	    if (fd >= 0)
	      {
		::close (fd);
	      }
	    libssh2_sftp_close (f);
	    *msg = "cannot read " + local + ": " + strerror (errno);
	    return false;
	  }
	char buf[65536];
	ssize_t n;
	bool ok = true;
	while (ok && (n = read (fd, buf, sizeof (buf))) > 0)
	  {
	    ssize_t off = 0;
	    while (off < n)
	      {
		ssize_t w = libssh2_sftp_write (f, buf + off, n - off);
		if (w <= 0)
		  {
		    *msg = "write failed on " + remote + ": " + last_error ();
		    broken = true;
		    ok = false;
		    break;
		  }
		off += w;
	      }
	    if (ok && cancel != NULL && cancel->load ())
	      {
		*msg = "cancelled";
		ok = false;
	      }
	  }
	::close (fd);
	if (libssh2_sftp_close (f) != 0 && ok)
	  {
	    *msg = "closing " + remote + " failed: " + last_error ();
	    ok = false;
	  }
	return ok;
      }
  };

  /* ------------------------------------------------------------ jobs */

  /*
   * Two kinds of job run on the master's (M) CMS:
   *   convert_master: a single database on M becomes a one-node HA master (M restarts)
   *   make_slave:     M is an HA master; a new server (NS) becomes its slave over ssh, replicas learn it over ssh
   * Nodes: -1 this server (M), 0 the new slave (NS), k>0 replica k-1.
   */

  struct step_t
  {
    string id, title, state, detail;
  };

  struct edit_t
  {
    int node;
    string path, bak;
    bool existed;
  };

  struct rep_t
  {
    string host, user, fp, pw;
    int port = 22;
    ssh_conn ssh;
    string cubrid, dbs, version, copy_base;
    bool reloaded = false, repl_started = false, repl_dir_existed = true;
    ~rep_t ()
    {
      wipe (pw);
    }
  };

  struct job_t
  {
    /* fixed after start */
    string kind, uuid, db, master, slave, user, fp, repl_home, m_bk_root, s_bk_root, ha_group, ha_port_id;
    int port = 22;
    bool start_ha = true;
    string cubrid, cubrid_dbs, local_version, state_dir;

    /* only the job thread touches these */
    string pw;			/* NS OS password */
    ssh_conn ssh;		/* NS */
    vector<std::unique_ptr<rep_t>> reps;
    string m_haconf, m_node_list, m_sync_mode, m_replica_list, m_copy_base;
    vector<string> m_db_line, s_db_line;
    string s_cubrid, s_dbs, s_version, s_tmp;
    bool s_master_running = false, s_master_stopped = false;
    string m_bk_dir, s_bk_dir, s_repl_dir;
    vector<edit_t> edits;
    vector<std::pair<string, string>> moved;	/* on NS */
    bool restored_new = false, ha_started = false, reloaded = false, repl_started = false, m_repl_dir_existed = true;
    bool c_restarted = false;
    time_t c_down_at = 0;

    /* shared with the status/cancel tasks, under mu */
    std::mutex mu;
    vector<step_t> steps;
    int cur = -1;
    string status = "running", phase = "run", error, message;
    int exit_code = -1;
    bool has_exit_code = false;
    string log;
    unsigned long long log_base = 0;
    vector<string> kept;
    time_t started = 0, ended = 0;
    int downtime_sec = -1;
    std::atomic<bool> cancel {false};
    std::atomic<bool> rolling_back {false};	/* undo commands must not see the cancel */

    ~job_t ()
    {
      wipe (pw);
    }
  };

  std::mutex g_jobs_mu;
  std::shared_ptr<job_t> g_job;	/* the current or last job */

  const size_t LOG_MAX = 1024 * 1024;

  const char *MAKE_STEPS[][2] =
  {
    {"precheck", "Check this server (the master)"},
    {"slave_connect", "Connect to the new slave over ssh"},
    {"slave_check", "Check the new slave"},
    {"replica_check", "Connect to and check the replicas"},
    {"slave_config", "Prepare the new slave's HA settings"},
    {"master_backup", "Back up the database online"},
    {"send_backup", "Send the backup to the new slave"},
    {"slave_keep_old_db", "Keep the new slave's old database aside"},
    {"slave_restore", "Restore on the new slave"},
    {"slave_copylog", "First copy of the replication log on the new slave"},
    {"slave_ha_start", "Start HA on the new slave"},
    {"master_config", "Add the new slave to this server's HA settings"},
    {"replication_start", "Start replication on this server"},
    {"replica_config", "Add the new slave to the replicas' HA settings"},
    {"replica_replication_start", "Start replication on the replicas"},
    {"verify", "Check that the new slave is standby"},
    {"cleanup", "Remove the backup files"},
  };

  const char *CONVERT_STEPS[][2] =
  {
    {"convert_precheck", "Check the database"},
    {"convert_config", "Write the HA settings"},
    {"convert_restart", "Restart the database under heartbeat (service stops)"},
    {"convert_verify", "Check that the database is the active master"},
  };

  void job_log (job_t *j, const string &line)
  {
    std::lock_guard<std::mutex> lg (j->mu);
    j->log += "[" + now_str () + "] " + line + "\n";
    if (j->log.size () > LOG_MAX)
      {
	size_t drop = j->log.size () - LOG_MAX;
	j->log.erase (0, drop);
	j->log_base += drop;
      }
  }

  Json::Value job_state_json (job_t *j)	/* caller holds j->mu */
  {
    Json::Value v;
    v["uuid"] = j->uuid;
    v["job_kind"] = j->kind;
    v["db_name"] = j->db;
    v["master_host"] = j->master;
    v["slave_host"] = j->slave;
    Json::Value rh (Json::arrayValue);
    for (auto &r : j->reps)
      {
	rh.append (r->host);
      }
    v["replica_hosts"] = rh;
    v["job_status"] = j->status;
    v["phase"] = j->phase;
    v["error"] = j->error;
    v["message"] = j->message;
    v["started_at"] = (int) j->started;
    v["ended_at"] = (int) j->ended;
    if (j->has_exit_code)
      {
	v["exit_code"] = j->exit_code;
      }
    if (j->downtime_sec >= 0)
      {
	v["downtime_sec"] = j->downtime_sec;
      }
    v["step_count"] = (int) j->steps.size ();
    v["step_no"] = j->cur + 1;
    v["step"] = (j->cur >= 0 && j->cur < (int) j->steps.size ()) ? j->steps[j->cur].id : "";
    Json::Value steps (Json::arrayValue);
    for (int i = 0; i < (int) j->steps.size (); i++)
      {
	Json::Value s;
	s["id"] = j->steps[i].id;
	s["no"] = i + 1;
	s["title"] = j->steps[i].title;
	s["state"] = j->steps[i].state;
	s["detail"] = j->steps[i].detail;
	steps.append (s);
      }
    v["steps"] = steps;
    Json::Value kept (Json::arrayValue);
    for (const string &k : j->kept)
      {
	kept.append (k);
      }
    v["kept_db_paths"] = kept;
    return v;
  }

  string node_label (job_t *j, int node)
  {
    return node < 0 ? string ("") : node == 0 ? "slave " + j->slave : "replica " + j->reps[node - 1]->host;
  }

  void save_state (job_t *j)
  {
    Json::Value v;
    {
      std::lock_guard<std::mutex> lg (j->mu);
      v = job_state_json (j);
    }
    /* how to reach NS again (cleanup), without the password */
    Json::Value ss;
    ss["host"] = j->slave;
    ss["port"] = j->port;
    ss["os_user"] = j->user;
    ss["hostkey_sha256"] = j->fp;
    v["slave_ssh"] = ss;
    Json::Value moved (Json::arrayValue);
    for (auto &m : j->moved)
      {
	Json::Value x;
	x["node"] = "slave " + j->slave;
	x["from"] = m.first;
	x["to"] = m.second;
	moved.append (x);
      }
    v["moved"] = moved;
    Json::Value edits (Json::arrayValue);
    for (auto &e : j->edits)
      {
	Json::Value x;
	x["node"] = e.node < 0 ? string ("master (this server)") : node_label (j, e.node);
	x["path"] = e.path;
	x["bak"] = e.existed ? e.bak : "";
	edits.append (x);
      }
    v["edited_files"] = edits;
    Json::StyledWriter w;
    write_local_file (j->state_dir + "/state.json", w.write (v));
  }

  void set_step (job_t *j, int i, const string &state, const string &detail = "")
  {
    {
      std::lock_guard<std::mutex> lg (j->mu);
      j->steps[i].state = state;
      if (!detail.empty ())
	{
	  j->steps[i].detail = detail;
	}
      if (state == "running")
	{
	  j->cur = i;
	}
    }
    save_state (j);
  }

  struct step_fail
  {
    string error, message;
    int exit_code = -1;
    bool has_exit = false;
  };

  /* ------------------------------------------------------------ local commands (this server) */

  /*
   * run argv without a shell, as this CMS account; stdout+stderr go to the job log.
   * returns the exit status, -1 when it could not run, -2 when cancelled, -3 on timeout.
   * Waits for the child, not for EOF: `cubrid heartbeat start` leaves daemons holding the pipe.
   */
  int run_local (job_t *j, const vector<string> &argv, string *out, int timeout_sec)
  {
    int pfd[2];
    if (pipe (pfd) != 0)
      {
	return -1;
      }
    job_log (j, "$ " + join (argv, " "));
    pid_t pid = fork ();
    if (pid < 0)
      {
	close (pfd[0]);
	close (pfd[1]);
	return -1;
      }
    if (pid == 0)
      {
	setpgid (0, 0);
	int devnull = open ("/dev/null", O_RDONLY);
	if (devnull >= 0)
	  {
	    dup2 (devnull, 0);
	  }
	dup2 (pfd[1], 1);
	dup2 (pfd[1], 2);
	for (int fd = 3; fd < 4096; fd++)
	  {
	    close (fd);
	  }
	vector<char *> av;
	for (const string &a : argv)
	  {
	    av.push_back (const_cast<char *> (a.c_str ()));
	  }
	av.push_back (NULL);
	execv (av[0], av.data ());
	_exit (127);
      }
    close (pfd[1]);
    fcntl (pfd[0], F_SETFL, fcntl (pfd[0], F_GETFL) | O_NONBLOCK);
    time_t start = time (NULL), exited_at = 0, term_at = 0;
    int status = 0, result = 0;
    bool done = false;
    string pending;
    char buf[8192];
    for (;;)
      {
	struct pollfd p = { pfd[0], POLLIN, 0 };
	poll (&p, 1, 300);
	ssize_t n;
	while ((n = read (pfd[0], buf, sizeof (buf))) > 0)
	  {
	    string chunk (buf, n);
	    if (out != NULL)
	      {
		*out += chunk;
	      }
	    pending += chunk;
	    size_t nl;
	    while ((nl = pending.find ('\n')) != string::npos)
	      {
		job_log (j, "  " + pending.substr (0, nl));
		pending.erase (0, nl + 1);
	      }
	  }
	if (!done && waitpid (pid, &status, WNOHANG) == pid)
	  {
	    done = true;
	    exited_at = time (NULL);
	  }
	if (done && (n == 0 || time (NULL) - exited_at >= 1))
	  {
	    break;
	  }
	bool cancelled = j->cancel.load () && !j->rolling_back.load ();
	if (!done && term_at == 0 && (cancelled || (timeout_sec > 0 && time (NULL) - start > timeout_sec)))
	  {
	    result = cancelled ? -2 : -3;
	    killpg (pid, SIGTERM);
	    term_at = time (NULL);
	  }
	if (!done && term_at != 0 && time (NULL) - term_at > 5)
	  {
	    killpg (pid, SIGKILL);
	  }
      }
    if (!pending.empty ())
      {
	job_log (j, "  " + pending);
      }
    close (pfd[0]);
    if (result != 0)
      {
	return result;
      }
    if (WIFEXITED (status))
      {
	return WEXITSTATUS (status);
      }
    return -1;
  }

  string bin (job_t *j, const char *name)
  {
    return j->cubrid + "/bin/" + name;
  }

  /* a cub_master of this account runs here (argv[0] basename "cub_master") */
  bool cub_master_running ()
  {
    DIR *d = opendir ("/proc");
    if (d == NULL)
      {
	return false;
      }
    bool found = false;
    struct dirent *e;
    while (!found && (e = readdir (d)) != NULL)
      {
	if (!isdigit ((unsigned char) e->d_name[0]))
	  {
	    continue;
	  }
	string data;
	bool ex = false;
	read_local_file (string ("/proc/") + e->d_name + "/cmdline", &data, &ex);
	size_t z = data.find ('\0');
	string a0 = z == string::npos ? data : data.substr (0, z);
	string a1 = (z == string::npos) ? "" : data.substr (z + 1, data.find ('\0', z + 1) - z - 1);
	for (const string &arg : { a0, a1 })	/* a1: under an emulator argv[0] is the emulator */
	  {
	    size_t sl = arg.rfind ('/');
	    if ((sl == string::npos ? arg : arg.substr (sl + 1)) == "cub_master")
	      {
		struct stat st;
		if (stat ((string ("/proc/") + e->d_name).c_str (), &st) == 0 && st.st_uid == getuid ())
		  {
		    found = true;
		  }
	      }
	  }
      }
    closedir (d);
    return found;
  }

  bool stop_local_cub_master (job_t *j)
  {
    if (!cub_master_running ())
      {
	return true;
      }
    /* `cubrid master stop` is cub_commdb -A; this CMS is not a child of cub_master and keeps running */
    run_local (j, { bin (j, "cub_commdb"), "-A" }, NULL, 120);
    for (int i = 0; i < 30 && cub_master_running (); i++)
      {
	sleep (1);
      }
    return !cub_master_running ();
  }

  /* "Server <db> (rel ...)" lines of `cubrid server status` */
  vector<string> running_servers (const string &status_out)
  {
    vector<string> out;
    for (const string &l : lines_of (status_out))
      {
	vector<string> t = split_any (trim (l), " ");
	if (t.size () >= 2 && t[0] == "Server")
	  {
	    out.push_back (t[1]);
	  }
      }
    return out;
  }

  /* ------------------------------------------------------------ remote commands (NS, replicas) */

  ssh_conn &node_ssh (job_t *j, int node)
  {
    return node == 0 ? j->ssh : j->reps[node - 1]->ssh;
  }

  int remote_on (job_t *j, int node, const string &cmd, string *out, int timeout_sec)
  {
    job_log (j, node_label (j, node) + "$ " + cmd);
    return node_ssh (j, node).exec (cmd, out, [j] (const string &l)
    {
      job_log (j, "  " + l);
    }, j->rolling_back.load () ? NULL : &j->cancel, timeout_sec);
  }

  /*
   * for commands that leave daemons behind (heartbeat start, cub_master): the daemon would hold the
   * ssh channel open, so the output goes to a temporary file that is read back
   */
  int remote_daemon (job_t *j, int node, const string &cmd, string *out, int timeout_sec)
  {
    string wrapped = "f=$(mktemp) && { " + cmd + " ; } >\"$f\" 2>&1 </dev/null; rc=$?; cat \"$f\"; rm -f \"$f\"; exit $rc";
    job_log (j, node_label (j, node) + "$ " + cmd);
    return node_ssh (j, node).exec (wrapped, out, [j] (const string &l)
    {
      job_log (j, "  " + l);
    }, j->rolling_back.load () ? NULL : &j->cancel, timeout_sec);
  }

  string login_node (job_t *j, int node, string *msg)
  {
    ssh_conn &c = node_ssh (j, node);
    if (c.alive ())
      {
	return "";
      }
    const string &host = node == 0 ? j->slave : j->reps[node - 1]->host;
    const string &user = node == 0 ? j->user : j->reps[node - 1]->user;
    const string &fp = node == 0 ? j->fp : j->reps[node - 1]->fp;
    const string &pw = node == 0 ? j->pw : j->reps[node - 1]->pw;
    int port = node == 0 ? j->port : j->reps[node - 1]->port;
    string err = c.connect (host, port, user, msg);
    if (!err.empty ())
      {
	*msg = node_label (j, node) + ": " + *msg;
	return err == "unreachable" ? "unreachable" : "internal";
      }
    if (c.fp != fp)
      {
	*msg = node_label (j, node) + ": the host key is " + c.fp + ", not the confirmed " + fp;
	c.close ();
	return "hostkey_mismatch";
      }
    err = c.auth (pw, msg);
    if (!err.empty ())
      {
	*msg = node_label (j, node) + ": " + *msg;
	c.close ();
	return "auth_failed";
      }
    job_log (j, "logged in to " + user + "@" + host + " (" + c.keytype + " " + c.fp + ")");
    return "";
  }

  bool reconnect_node (job_t *j, int node, string *msg)
  {
    return login_node (j, node, msg).empty ();
  }

  /* `cubrid ...` on any node: locally without a shell, remotely through the login shell */
  int cubrid_on (job_t *j, int node, const vector<string> &args, string *out, int timeout_sec)
  {
    if (node < 0)
      {
	vector<string> argv = { bin (j, "cubrid") };
	argv.insert (argv.end (), args.begin (), args.end ());
	return run_local (j, argv, out, timeout_sec);
      }
    string cmd = "cubrid";
    for (const string &a : args)
      {
	cmd += " " + shq (a);
      }
    return remote_on (j, node, cmd + " 2>&1", out, timeout_sec);
  }

  /* edit one file on a node: keep path.bak.<uuid>, write the new content in place */
  bool edit_file (job_t *j, int node, const string &path, const std::function<string (const string &)> &change,
		  string *msg)
  {
    bool on_remote = node >= 0;
    string where = on_remote ? node_label (j, node) + ": " : "";
    string data;
    bool exists = false;
    bool ok = on_remote ? node_ssh (j, node).read_file (path, &data, &exists) : read_local_file (path, &data, &exists);
    if (!ok)
      {
	*msg = where + "cannot read " + path;
	return false;
      }
    string next = change (data);
    if (next == data && exists)
      {
	job_log (j, where + path + " already has the values");
	return true;
      }
    edit_t e;
    e.node = node;
    e.path = path;
    e.bak = path + ".bak." + j->uuid;
    e.existed = exists;
    if (exists)
      {
	ok = on_remote ? node_ssh (j, node).write_file (e.bak, data) : write_local_file (e.bak, data);
	if (!ok)
	  {
	    *msg = where + "cannot write " + e.bak;
	    return false;
	  }
      }
    j->edits.push_back (e);
    save_state (j);
    if (on_remote)
      {
	string tmp = path + ".tmp." + j->uuid;
	ok = node_ssh (j, node).write_file (tmp, next) && remote_on (j, node, "mv -f " + shq (tmp) + " " + shq (path), NULL, 30) == 0;
      }
    else
      {
	ok = write_local_file (path, next);
      }
    if (!ok)
      {
	*msg = where + "cannot write " + path;
	return false;
      }
    job_log (j, where + "edited " + path + (exists ? " (kept " + e.bak + ")" : " (new)"));
    return true;
  }

  bool read_on (job_t *j, int node, const string &path, string *data, bool *exists)
  {
    return node < 0 ? read_local_file (path, data, exists) : node_ssh (j, node).read_file (path, data, exists);
  }

  /* undo the edits a step made on one node (newest first) */
  bool restore_edits (job_t *j, int node)
  {
    bool ok = true;
    string msg;
    if (node >= 0 && !reconnect_node (j, node, &msg))
      {
	job_log (j, msg);
	return false;
      }
    for (auto it = j->edits.rbegin (); it != j->edits.rend (); ++it)
      {
	if (it->node != node)
	  {
	    continue;
	  }
	if (node >= 0)
	  {
	    string cmd = it->existed ? "mv -f " + shq (it->bak) + " " + shq (it->path) : "rm -f " + shq (it->path);
	    ok = remote_on (j, node, cmd, NULL, 60) == 0 && ok;
	  }
	else if (it->existed)
	  {
	    ok = rename (it->bak.c_str (), it->path.c_str ()) == 0 && ok;
	  }
	else
	  {
	    ok = unlink (it->path.c_str ()) == 0 && ok;
	  }
	job_log (j, (node >= 0 ? node_label (j, node) + ": " : string ("")) + "restored " + it->path);
      }
    return ok;
  }

  /* ------------------------------------------------------------ steps */

#define FAIL(code, text) do { f->error = (code); f->message = (text); return false; } while (0)

  bool check_local_basics (job_t *j, step_fail *f)
  {
    if (j->cubrid.empty () || j->cubrid_dbs.empty ())
      {
	FAIL ("precheck_failed", "CUBRID or CUBRID_DATABASES is not set for this CMS");
      }
    string out;
    run_local (j, { bin (j, "cubrid_rel") }, &out, 30);
    j->local_version = parse_version (out);
    if (j->local_version.empty ())
      {
	FAIL ("precheck_failed", "cannot read the local CUBRID version");
      }
    if (local_hostname () != j->master)
      {
	FAIL ("precheck_failed", "master_host " + j->master + " is not this server's host name (" + local_hostname () + ")");
      }
    mkdir_p (j->state_dir);
    return true;
  }

  /* -------- convert_master */

  bool st_convert_precheck (job_t *j, step_fail *f)
  {
    if (!check_local_basics (j, f))
      {
	return false;
      }
    string conf, out;
    bool ex = false;
    read_local_file (j->cubrid + "/conf/cubrid.conf", &conf, &ex);
    if (conf_get (conf, "[common]", "ha_mode") == "on" || conf_get (conf, "[common]", "ha_mode") == "yes")
      {
	FAIL ("already_ha", "ha_mode is already on in cubrid.conf");
      }
    run_local (j, { bin (j, "cubrid"), "server", "status" }, &out, 60);
    vector<string> up = running_servers (out);
    if (!contains (up, j->db))
      {
	FAIL ("precheck_failed", "database " + j->db + " is not running on this server");
      }
    vector<string> others;
    for (const string &s : up)
      {
	if (s != j->db)
	  {
	    others.push_back (s);
	  }
      }
    if (!others.empty ())
      {
	FAIL ("other_db_running", "other databases run on this server (" + join (others, ", ") +
	      "); restarting cub_master in HA mode would stop them too. Stop them first and try again");
      }
    string dbtxt;
    read_local_file (j->cubrid_dbs + "/databases.txt", &dbtxt, &ex);
    j->m_db_line = dbtxt_line (dbtxt, j->db);
    if (j->m_db_line.size () < 4)
      {
	FAIL ("precheck_failed", "databases.txt has no line for " + j->db);
      }
    job_log (j, "this server: " + j->master + ", CUBRID " + j->local_version + ", " + j->db + " runs without HA");
    return true;
  }

  bool st_convert_config (job_t *j, step_fail *f)
  {
    string msg;
    if (!edit_file (j, -1, j->cubrid + "/conf/cubrid.conf", [] (const string &c)
    {
      string n = conf_set (c, "[common]", "ha_mode", "on");
      return conf_set (n, "[common]", "force_remove_log_archives", "no");
      }, &msg))
      {
	FAIL ("config_edit_failed", msg);
      }
    if (!edit_file (j, -1, j->cubrid + "/conf/cubrid_ha.conf", [j] (const string &c)
    {
      string n = conf_set (c, "[common]", "ha_port_id", j->ha_port_id);
      n = conf_set (n, "[common]", "ha_node_list", j->ha_group + "@" + j->master);
	n = conf_set (n, "[common]", "ha_db_list", j->db);
	return conf_set (n, "[common]", "ha_copy_sync_mode", "sync");
      }, &msg))
      {
	FAIL ("config_edit_failed", msg);
      }
    if (!edit_file (j, -1, j->cubrid_dbs + "/databases.txt", [j] (const string &c)
    {
      vector<string> t = dbtxt_line (c, j->db);
      if (t.size () < 4 || t[2] == j->master)
	  {
	    return c;
	  }
	t[2] = j->master;
	return dbtxt_put (c, j->db, t);
      }, &msg))
      {
	FAIL ("config_edit_failed", msg);
      }
    return true;
  }

  bool st_convert_restart (job_t *j, step_fail *f)
  {
    j->c_down_at = time (NULL);
    j->c_restarted = true;
    save_state (j);
    job_log (j, "service interruption starts: " + j->db + " stops");
    int rc = run_local (j, { bin (j, "cubrid"), "server", "stop", j->db }, NULL, 600);
    if (rc != 0)
      {
	f->exit_code = rc;
	f->has_exit = rc > 0;
	FAIL ("command_failed", "cubrid server stop " + j->db + " failed (" + std::to_string (rc) + ")");
      }
    if (!stop_local_cub_master (j))
      {
	FAIL ("command_failed", "cub_master did not stop");
      }
    string out;
    rc = run_local (j, { bin (j, "cubrid"), "heartbeat", "start" }, &out, 600);
    if (rc != 0 || out.find (": fail") != string::npos)
      {
	f->exit_code = rc;
	f->has_exit = rc > 0;
	FAIL ("command_failed", "cubrid heartbeat start failed");
      }
    return true;
  }

  bool st_convert_verify (job_t *j, step_fail *f)
  {
    string last;
    for (int i = 0; i < 90; i++)
      {
	string out;
	run_local (j, { bin (j, "cubrid"), "changemode", j->db + "@localhost" }, &out, 60);
	last = trim (out);
	if (out.find ("is active") != string::npos)
	  {
	    std::lock_guard<std::mutex> lg (j->mu);
	    j->downtime_sec = (int) (time (NULL) - j->c_down_at);
	    j->log += "[" + now_str () + "] service interruption ends: " + j->db + " is the active master after "
		      + std::to_string (j->downtime_sec) + " s\n";
	    return true;
	  }
	if (j->cancel.load ())
	  {
	    FAIL ("cancelled", "cancelled");
	  }
	sleep (2);
      }
    FAIL ("verify_failed", j->db + " did not become active: " + last);
  }

  /* -------- make_slave */

  bool st_precheck (job_t *j, step_fail *f)
  {
    if (!check_local_basics (j, f))
      {
	return false;
      }
    string conf, ha, dbtxt, out;
    bool ex = false;
    read_local_file (j->cubrid + "/conf/cubrid.conf", &conf, &ex);
    if (conf_get (conf, "[common]", "ha_mode") != "on")
      {
	FAIL ("master_not_ha", j->db + " is not an HA master here; convert it with ha_convert_master first");
      }
    run_local (j, { bin (j, "cubrid"), "changemode", j->db + "@localhost" }, &out, 60);
    if (out.find ("is active") == string::npos)
      {
	FAIL ("master_not_active", "database " + j->db + " is not the active master: " + trim (out));
      }
    if (!read_local_file (j->cubrid + "/conf/cubrid_ha.conf", &ha, &ex) || !ex)
      {
	FAIL ("precheck_failed", "cannot read cubrid_ha.conf");
      }
    j->m_haconf = ha;
    j->m_node_list = conf_get (ha, "[common]", "ha_node_list");
    j->m_sync_mode = conf_get (ha, "[common]", "ha_copy_sync_mode");
    j->m_replica_list = conf_get (ha, "[common]", "ha_replica_list");
    j->m_copy_base = conf_get (ha, "[common]", "ha_copy_log_base");
    if (j->m_copy_base.empty ())
      {
	j->m_copy_base = j->cubrid_dbs;
      }
    if (!contains (node_list_hosts (j->m_node_list), j->master))
      {
	FAIL ("precheck_failed", "ha_node_list (" + j->m_node_list + ") does not have " + j->master);
      }
    if (!contains (split_any (conf_get (ha, "[common]", "ha_db_list"), " ,:"), j->db))
      {
	FAIL ("precheck_failed", "ha_db_list does not have " + j->db);
      }
    if (contains (node_list_hosts (j->m_node_list), j->slave))
      {
	job_log (j, j->slave + " is already in ha_node_list; continuing (settings that are already there stay as they are)");
      }
    vector<string> mr = j->m_replica_list.empty () ? vector<string> () : node_list_hosts (j->m_replica_list);
    vector<string> asked;
    for (auto &r : j->reps)
      {
	asked.push_back (r->host);
	if (!contains (mr, r->host))
	  {
	    FAIL ("precheck_failed", "replica " + r->host + " is not in ha_replica_list (" + j->m_replica_list + ")");
	  }
      }
    for (const string &h : mr)
      {
	if (!contains (asked, h))
	  {
	    FAIL ("precheck_failed", "ha_replica_list has " + h + "; pass it in replicas so it learns the new slave too");
	  }
      }
    read_local_file (j->cubrid_dbs + "/databases.txt", &dbtxt, &ex);
    j->m_db_line = dbtxt_line (dbtxt, j->db);
    if (j->m_db_line.size () < 4 || !valid_path (j->m_db_line[1]) || !valid_path (j->m_db_line[3]))
      {
	FAIL ("precheck_failed", "databases.txt has no usable line for " + j->db);
      }
    struct addrinfo hints, *res = NULL;
    memset (&hints, 0, sizeof (hints));
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo (j->slave.c_str (), NULL, &hints, &res) != 0)
      {
	FAIL ("precheck_failed", "this server cannot resolve the new slave's name " + j->slave);
      }
    freeaddrinfo (res);
    job_log (j, "this server: " + j->master + ", CUBRID " + j->local_version + ", " + j->db + " is the active master, ha_node_list="
	     + j->m_node_list);
    return true;
  }

  bool st_slave_connect (job_t *j, step_fail *f)
  {
    string msg;
    string err = login_node (j, 0, &msg);
    if (!err.empty ())
      {
	FAIL (err, msg);
      }
    return true;
  }

  bool st_slave_check (job_t *j, step_fail *f)
  {
    string out, hostname;
    int rc = remote_on (j, 0, "echo \"CUBRID=$CUBRID\"; echo \"CUBRID_DATABASES=$CUBRID_DATABASES\"; "
			"echo \"HOSTNAME=$(uname -n)\"; \"$CUBRID/bin/cubrid_rel\" | grep -m1 CUBRID; "
			"getent hosts " + shq (j->master) + " >/dev/null && echo MASTER_RESOLVES", &out, 60);
    bool resolves = false;
    for (const string &l : lines_of (out))
      {
	if (l.compare (0, 7, "CUBRID=") == 0)
	  {
	    j->s_cubrid = l.substr (7);
	  }
	else if (l.compare (0, 17, "CUBRID_DATABASES=") == 0)
	  {
	    j->s_dbs = l.substr (17);
	  }
	else if (l.compare (0, 9, "HOSTNAME=") == 0)
	  {
	    hostname = l.substr (9);
	  }
	else if (l == "MASTER_RESOLVES")
	  {
	    resolves = true;
	  }
	else if (l.find ("CUBRID ") == 0 && j->s_version.empty ())
	  {
	    j->s_version = parse_version (l);
	  }
      }
    if (rc < 0 || j->s_cubrid.empty () || j->s_version.empty () || !valid_path (j->s_cubrid) || !valid_path (j->s_dbs))
      {
	FAIL ("precheck_failed", "slave " + j->slave + ": CUBRID was not found for " + j->user);
      }
    if (hostname != j->slave)
      {
	FAIL ("precheck_failed", "slave " + j->slave + ": its host name is " + hostname + "; slave_host must be the same");
      }
    if (j->s_version != j->local_version)
      {
	FAIL ("version_mismatch", "slave " + j->slave + ": CUBRID " + j->s_version + ", this server " + j->local_version);
      }
    if (!resolves)
      {
	FAIL ("precheck_failed", "slave " + j->slave + ": cannot resolve the master's name " + j->master);
      }
    out.clear ();
    remote_on (j, 0, "cubrid server status 2>&1; for f in /proc/[0-9]*/cmdline; do c=$(tr '\\0' ' ' < $f 2>/dev/null); "
	       "case \"$c\" in bash*|*-lc*) ;; cub_master*|*/bin/cub_master\\ *) echo CUB_MASTER_RUNNING;; esac; done", &out, 60);
    vector<string> up = running_servers (out);
    j->s_master_running = out.find ("CUB_MASTER_RUNNING") != string::npos;
    if (contains (up, j->db))
      {
	FAIL ("slave_db_running", "slave " + j->slave + ": database " + j->db + " is running there; stop it first");
      }
    if (j->s_master_running && !up.empty () && j->start_ha)
      {
	FAIL ("other_db_running", "slave " + j->slave + ": other databases run there (" + join (up, ", ") +
	      "); cub_master has to restart in HA mode, which would stop them. Stop them first and try again");
      }
    if (j->repl_home.empty ())
      {
	j->repl_home = j->s_dbs;
      }
    if (j->s_bk_root.empty ())
      {
	j->s_bk_root = j->s_dbs + "/ha_make_slave_backup";
      }
    if (!valid_path (j->repl_home) || !valid_path (j->s_bk_root))
      {
	FAIL ("precheck_failed", "invalid repl_log_home or slave_backup_path");
      }
    job_log (j, "slave " + j->slave + ": CUBRID " + j->s_version + (j->s_master_running ? ", cub_master runs" : ", no cub_master"));
    return true;
  }

  bool st_replica_check (job_t *j, step_fail *f)
  {
    if (j->reps.empty ())
      {
	job_log (j, "no replicas");
	return true;
      }
    for (size_t k = 0; k < j->reps.size (); k++)
      {
	rep_t *r = j->reps[k].get ();
	int node = (int) k + 1;
	string msg;
	string err = login_node (j, node, &msg);
	if (!err.empty ())
	  {
	    FAIL (err, msg);
	  }
	string out;
	int rc = remote_on (j, node, "echo \"CUBRID=$CUBRID\"; echo \"CUBRID_DATABASES=$CUBRID_DATABASES\"; "
			    "\"$CUBRID/bin/cubrid_rel\" | grep -m1 CUBRID", &out, 60);
	for (const string &l : lines_of (out))
	  {
	    if (l.compare (0, 7, "CUBRID=") == 0)
	      {
		r->cubrid = l.substr (7);
	      }
	    else if (l.compare (0, 17, "CUBRID_DATABASES=") == 0)
	      {
		r->dbs = l.substr (17);
	      }
	    else if (l.find ("CUBRID ") == 0 && r->version.empty ())
	      {
		r->version = parse_version (l);
	      }
	  }
	if (rc != 0 || r->cubrid.empty () || r->version.empty ())
	  {
	    FAIL ("precheck_failed", "replica " + r->host + ": CUBRID was not found for " + r->user);
	  }
	if (r->version != j->local_version)
	  {
	    FAIL ("version_mismatch", "replica " + r->host + ": CUBRID " + r->version + ", this server " + j->local_version);
	  }
	out.clear ();
	remote_on (j, node, "cubrid changemode " + shq (j->db + "@localhost") + " 2>&1", &out, 60);
	if (out.find ("is standby") == string::npos)
	  {
	    FAIL ("precheck_failed", "replica " + r->host + ": " + j->db + " is not running as standby: " + trim (out));
	  }
	string ha;
	bool ex = false;
	if (!r->ssh.read_file (r->cubrid + "/conf/cubrid_ha.conf", &ha, &ex) || !ex)
	  {
	    FAIL ("precheck_failed", "replica " + r->host + ": cannot read cubrid_ha.conf");
	  }
	if (!contains (split_any (conf_get (ha, "[common]", "ha_db_list"), " ,:"), j->db)
	    || !contains (node_list_hosts (conf_get (ha, "[common]", "ha_node_list")), j->master))
	  {
	    FAIL ("precheck_failed", "replica " + r->host + ": not replicating " + j->db + " from " + j->master);
	  }
	r->copy_base = conf_get (ha, "[common]", "ha_copy_log_base");
	if (r->copy_base.empty ())
	  {
	    r->copy_base = r->dbs;
	  }
	job_log (j, "replica " + r->host + ": CUBRID " + r->version + ", " + j->db + " is standby");
      }
    return true;
  }

  bool st_slave_config (job_t *j, step_fail *f)
  {
    string msg;
    string conf = j->s_cubrid + "/conf/cubrid.conf";
    if (!edit_file (j, 0, conf, [] (const string &c)
    {
      string n = conf_get (c, "[common]", "ha_mode") == "on" ? c : conf_set (c, "[common]", "ha_mode", "on");
      /* manual (HA, adding a slave): keep logs that are not replicated yet */
      return conf_get (n, "[common]", "force_remove_log_archives") == "no" ? n
	     : conf_set (n, "[common]", "force_remove_log_archives", "no");
      }, &msg))
      {
	FAIL ("config_edit_failed", msg);
      }
    string haconf = j->s_cubrid + "/conf/cubrid_ha.conf";
    bool ch;
    string list = node_list_add (j->m_node_list, j->slave, &ch);
    if (!edit_file (j, 0, haconf, [j, list] (const string &c)
    {
      string n = conf_set (c, "[common]", "ha_mode", "on");
      /* the HA settings as on the master (ha_port_id, ha_replica_list, ha_apply_max_mem_size, ...) */
      for (auto &kv : conf_pairs (j->m_haconf, "[common]"))
	  {
	    if (kv.first != "ha_node_list" && kv.first != "ha_db_list" && kv.first != "ha_copy_sync_mode"
		&& kv.first != "ha_copy_log_base")
	      {
		n = conf_set (n, "[common]", kv.first, kv.second);
	      }
	  }
	vector<string> dbs = split_any (conf_get (c, "[common]", "ha_db_list"), " ,:");
	if (!contains (dbs, j->db))
	  {
	    dbs.push_back (j->db);
	  }
	n = conf_set (n, "[common]", "ha_node_list", list);
	n = conf_set (n, "[common]", "ha_db_list", join (dbs, ","));
	string sync = sync_mode_for (j->m_sync_mode, node_list_hosts (list).size ());
	if (!sync.empty ())
	  {
	    n = conf_set (n, "[common]", "ha_copy_sync_mode", sync);
	  }
	if (j->repl_home != j->s_dbs)
	  {
	    n = conf_set (n, "[common]", "ha_copy_log_base", j->repl_home);
	  }
	return n;
      }, &msg))
      {
	FAIL ("config_edit_failed", msg);
      }
    string dbtxt_path = j->s_dbs + "/databases.txt", dbtxt;
    bool ex = false;
    j->ssh.read_file (dbtxt_path, &dbtxt, &ex);
    vector<string> cur = dbtxt_line (dbtxt, j->db);
    vector<string> want = (cur.size () >= 4) ? cur : j->m_db_line;
    vector<string> ordered;
    for (const string &h : node_list_hosts (list))
      {
	if (!contains (ordered, h))
	  {
	    ordered.push_back (h);
	  }
      }
    for (const string &h : split_any (want[2], ":"))
      {
	if (!contains (ordered, h) && h != "localhost")
	  {
	    ordered.push_back (h);
	  }
      }
    want[2] = join (ordered, ":");
    if (!valid_path (want[1]) || !valid_path (want[3]))
      {
	FAIL ("precheck_failed", "unexpected volume or log path for " + j->db);
      }
    if (!edit_file (j, 0, dbtxt_path, [&] (const string &c)
    {
      return dbtxt_line (c, j->db) == want ? c : dbtxt_put (c, j->db, want);
      }, &msg))
      {
	FAIL ("config_edit_failed", msg);
      }
    j->s_db_line = want;
    string c1, c2, c3;
    bool e1, e2, e3;
    j->ssh.read_file (conf, &c1, &e1);
    j->ssh.read_file (haconf, &c2, &e2);
    j->ssh.read_file (dbtxt_path, &c3, &e3);
    if (conf_get (c1, "[common]", "ha_mode") != "on" || conf_get (c2, "[common]", "ha_node_list") != list
	|| dbtxt_line (c3, j->db) != want)
      {
	FAIL ("config_edit_failed", "slave " + j->slave + ": the settings did not read back as written");
      }
    job_log (j, "slave " + j->slave + ": ha_node_list=" + list + ", " + j->db + " -> " + want[1]);
    return true;
  }

  bool st_master_backup (job_t *j, step_fail *f)
  {
    j->m_bk_dir = j->m_bk_root + "/" + j->uuid;
    save_state (j);
    if (!mkdir_p (j->m_bk_dir))
      {
	FAIL ("command_failed", "cannot create " + j->m_bk_dir);
      }
    int rc = run_local (j, { bin (j, "cubrid"), "backupdb", "-C", "-D", j->m_bk_dir, "-o", j->m_bk_dir + "/backup.out",
			     "--no-check", j->db + "@localhost"
			   }, NULL, 6 * 3600);
    if (rc != 0)
      {
	f->exit_code = rc;
	f->has_exit = rc > 0;
	FAIL (rc == -2 ? "cancelled" : "command_failed", "cubrid backupdb failed (" + std::to_string (rc) + ")");
      }
    return true;
  }

  bool st_send_backup (job_t *j, step_fail *f)
  {
    j->s_bk_dir = j->s_bk_root + "/" + j->uuid;
    save_state (j);
    string msg;
    if (remote_on (j, 0, "mkdir -p " + shq (j->s_bk_dir), NULL, 60) != 0)
      {
	FAIL ("transfer_failed", "slave " + j->slave + ": cannot create " + j->s_bk_dir);
      }
    vector<std::pair<string, string>> todo;	/* local path, remote name */
    DIR *d = opendir (j->m_bk_dir.c_str ());
    if (d != NULL)
      {
	struct dirent *e;
	while ((e = readdir (d)) != NULL)
	  {
	    string n = e->d_name;
	    if (n != "." && n != ".." && n != "backup.out")
	      {
		todo.push_back (std::make_pair (j->m_bk_dir + "/" + n, n));
	      }
	  }
	closedir (d);
      }
    string bkvinf = j->db + "_bkvinf";
    todo.push_back (std::make_pair (j->m_db_line[3] + "/" + bkvinf, bkvinf));
    string names;
    for (auto &t : todo)
      {
	string sum;
	if (!sha256_local (t.first, &sum))
	  {
	    FAIL ("transfer_failed", "cannot read " + t.first);
	  }
	string remote = j->s_bk_dir + "/" + t.second;
	bool ok = false;
	for (int attempt = 1; attempt <= 3 && !ok; attempt++)
	  {
	    if (j->cancel.load ())
	      {
		FAIL ("cancelled", "cancelled");
	      }
	    if (!reconnect_node (j, 0, &msg))
	      {
		continue;
	      }
	    ok = j->ssh.upload (t.first, remote, &j->cancel, &msg);
	    if (!ok)
	      {
		job_log (j, "sending " + t.second + " stopped (" + msg + "), attempt " + std::to_string (attempt));
		j->ssh.close ();
	      }
	  }
	if (!ok)
	  {
	    FAIL (j->cancel.load () ? "cancelled" : "transfer_failed", "slave " + j->slave + ": " + msg);
	  }
	string out;
	remote_on (j, 0, "sha256sum " + shq (remote), &out, 3600);
	struct stat st;
	uint64_t rsize = 0;
	if (out.compare (0, sum.size (), sum) != 0 || stat (t.first.c_str (), &st) != 0 || !j->ssh.stat_size (remote, &rsize)
	    || rsize != (uint64_t) st.st_size)
	  {
	    FAIL ("transfer_failed", "slave " + j->slave + ": size or checksum of " + t.second + " does not match");
	  }
	job_log (j, "sent " + t.second + " (" + std::to_string (rsize) + " bytes, sha256 ok)");
      }
    return true;
  }

  bool st_slave_keep_old_db (job_t *j, step_fail *f)
  {
    vector<string> paths = { j->s_db_line[1] };
    if (j->s_db_line[3] != j->s_db_line[1])
      {
	paths.push_back (j->s_db_line[3]);
      }
    string suffix = ".ha_make_slave_" + j->uuid;
    string cmd;
    for (const string &p : paths)
      {
	cmd += "if [ -d " + shq (p) + " ] && [ -n \"$(ls -A " + shq (p) + ")\" ]; then mv " + shq (p) + " " + shq (p + suffix)
	       + " && echo MOVED " + p + "; fi; ";
      }
    cmd += "for d in " + shq (j->repl_home) + "/" + j->db + "_*; do case \"$d\" in *.ha_make_slave_*) continue;; esac; "
	   "[ -d \"$d\" ] && mv \"$d\" \"$d" + suffix + "\" && echo MOVED $d; done; "
	   "mkdir -p " + shq (j->s_db_line[1]) + " " + shq (j->s_db_line[3]);
    string out;
    int rc = remote_on (j, 0, cmd, &out, 600);
    for (const string &l : lines_of (out))
      {
	if (l.compare (0, 6, "MOVED ") == 0)
	  {
	    string from = l.substr (6);
	    j->moved.push_back (std::make_pair (from, from + suffix));
	    std::lock_guard<std::mutex> lg (j->mu);
	    j->kept.push_back (from + suffix);
	  }
      }
    save_state (j);
    if (rc != 0)
      {
	FAIL ("command_failed", "slave " + j->slave + ": moving the old database aside failed");
      }
    if (j->moved.empty ())
      {
	job_log (j, "no old database files on the new slave");
      }
    return true;
  }

  bool st_slave_restore (job_t *j, step_fail *f)
  {
    j->restored_new = true;
    save_state (j);
    string bkvinf = j->db + "_bkvinf";
    int rc = remote_on (j, 0, "cp -f " + shq (j->s_bk_dir + "/" + bkvinf) + " " + shq (j->s_db_line[3] + "/") +
			" && cubrid restoreslave -s master -m " + shq (j->master) + " -B " + shq (j->s_bk_dir) + " " + shq (j->db)
			+ " 2>&1", NULL, 6 * 3600);
    if (rc != 0)
      {
	f->exit_code = rc;
	f->has_exit = rc > 0;
	FAIL (rc == -2 ? "cancelled" : "command_failed", "slave " + j->slave + ": cubrid restoreslave failed (" + std::to_string (rc) + ")");
      }
    return true;
  }

  bool st_slave_copylog (job_t *j, step_fail *f)
  {
    j->s_repl_dir = j->repl_home + "/" + j->db + "_" + j->master;
    save_state (j);
    int rc = remote_on (j, 0, "rm -rf " + shq (j->s_repl_dir) + " && mkdir -p " + shq (j->s_repl_dir) +
			" && cub_admin copylogdb -L " + shq (j->s_repl_dir) + " -m async --start-page-id=-1 " +
			shq (j->db + "@" + j->master) + " 2>&1", NULL, 3600);
    if (rc != 0)
      {
	f->exit_code = rc;
	f->has_exit = rc > 0;
	FAIL (rc == -2 ? "cancelled" : "command_failed", "slave " + j->slave + ": copylogdb failed (" + std::to_string (rc) + ")");
      }
    return true;
  }

  string remote_master_check_cmd ()
  {
    return "for f in /proc/[0-9]*/cmdline; do c=$(tr '\\0' ' ' < $f 2>/dev/null); "
	   "case \"$c\" in bash*|*-lc*) ;; cub_master*|*/bin/cub_master\\ *) echo CUB_MASTER_RUNNING;; esac; done";
  }

  bool slave_master_running (job_t *j)
  {
    string out;
    remote_on (j, 0, remote_master_check_cmd (), &out, 60);
    return out.find ("CUB_MASTER_RUNNING") != string::npos;
  }

  bool st_slave_ha_start (job_t *j, step_fail *f)
  {
    if (j->s_master_running)
      {
	/* checked in slave_check: no database server runs there, so only cub_master goes down */
	j->s_master_stopped = true;
	save_state (j);
	remote_on (j, 0, "cub_commdb -A 2>&1", NULL, 120);
	for (int i = 0; i < 30 && slave_master_running (j); i++)
	  {
	    sleep (1);
	  }
      }
    j->ha_started = true;
    save_state (j);
    string out;
    int rc = remote_daemon (j, 0, "cubrid heartbeat start " + shq (j->db), &out, 600);
    if (rc != 0 || out.find (": fail") != string::npos)
      {
	f->exit_code = rc;
	f->has_exit = rc > 0;
	FAIL (rc == -2 ? "cancelled" : "command_failed", "slave " + j->slave + ": cubrid heartbeat start " + j->db + " failed");
      }
    return true;
  }

  /* procedure 6 and 8 on an existing node (this server or a replica): ha_node_list (+ ha_copy_sync_mode), databases.txt, reload */
  bool add_slave_to_node (job_t *j, int node, const string &cubrid, const string &dbs, bool *reloaded, step_fail *f)
  {
    string msg;
    if (node >= 0 && !reconnect_node (j, node, &msg))
      {
	FAIL ("unreachable", msg);
      }
    if (!edit_file (j, node, cubrid + "/conf/cubrid_ha.conf", [j] (const string &c)
    {
      bool ch;
      string list = node_list_add (conf_get (c, "[common]", "ha_node_list"), j->slave, &ch);
	string n = ch ? conf_set (c, "[common]", "ha_node_list", list) : c;
	string mode = conf_get (n, "[common]", "ha_copy_sync_mode");
	string sync = sync_mode_for (mode, node_list_hosts (list).size ());
	return sync != mode ? conf_set (n, "[common]", "ha_copy_sync_mode", sync) : n;
      }, &msg))
      {
	FAIL ("config_edit_failed", msg);
      }
    if (!edit_file (j, node, dbs + "/databases.txt", [j] (const string &c)
    {
      vector<string> t = dbtxt_line (c, j->db);
	if (t.size () < 4)
	  {
	    return c;
	  }
	vector<string> hosts = split_any (t[2], ":");
	if (contains (hosts, j->slave))
	  {
	    return c;
	  }
	hosts.push_back (j->slave);
	t[2] = join (hosts, ":");
	return dbtxt_put (c, j->db, t);
      }, &msg))
      {
	FAIL ("config_edit_failed", msg);
      }
    string ha;
    bool ex = false;
    read_on (j, node, cubrid + "/conf/cubrid_ha.conf", &ha, &ex);
    if (!contains (node_list_hosts (conf_get (ha, "[common]", "ha_node_list")), j->slave))
      {
	FAIL ("config_edit_failed", node_label (j, node) + (node >= 0 ? ": " : "") + "ha_node_list did not read back with " + j->slave);
      }
    string out;
    int rc = cubrid_on (j, node, { "heartbeat", "reload" }, &out, 300);
    *reloaded = true;
    save_state (j);
    if (rc != 0 || out.find ("fail") != string::npos)
      {
	f->exit_code = rc;
	f->has_exit = rc > 0;
	FAIL ("command_failed", node_label (j, node) + (node >= 0 ? ": " : "") + "cubrid heartbeat reload failed: " + trim (out));
      }
    return true;
  }

  bool st_master_config (job_t *j, step_fail *f)
  {
    return add_slave_to_node (j, -1, j->cubrid, j->cubrid_dbs, &j->reloaded, f);
  }

  /* the folder `replication start <slave>` copies the slave's log into on that node */
  string slave_log_dir (job_t *j, const string &copy_base)
  {
    return copy_base + "/" + j->db + "_" + j->slave;
  }

  bool st_replication_start (job_t *j, step_fail *f)
  {
    j->m_repl_dir_existed = is_dir (slave_log_dir (j, j->m_copy_base));
    j->repl_started = true;
    save_state (j);
    string out;
    int rc = run_local (j, { bin (j, "cubrid"), "heartbeat", "replication", "start", j->slave }, &out, 300);
    if (rc != 0 || out.find ("fail") != string::npos)
      {
	f->exit_code = rc;
	f->has_exit = rc > 0;
	FAIL (rc == -2 ? "cancelled" : "command_failed", "cubrid heartbeat replication start failed: " + trim (out));
      }
    return true;
  }

  bool st_replica_config (job_t *j, step_fail *f)
  {
    for (size_t k = 0; k < j->reps.size (); k++)
      {
	rep_t *r = j->reps[k].get ();
	if (!add_slave_to_node (j, (int) k + 1, r->cubrid, r->dbs, &r->reloaded, f))
	  {
	    return false;
	  }
      }
    if (j->reps.empty ())
      {
	job_log (j, "no replicas");
      }
    return true;
  }

  bool st_replica_replication_start (job_t *j, step_fail *f)
  {
    for (size_t k = 0; k < j->reps.size (); k++)
      {
	rep_t *r = j->reps[k].get ();
	string out;
	remote_on (j, (int) k + 1, "test -d " + shq (slave_log_dir (j, r->copy_base)) + " && echo exists", &out, 30);
	r->repl_dir_existed = out.find ("exists") != string::npos;
	r->repl_started = true;
	save_state (j);
	out.clear ();
	int rc = remote_on (j, (int) k + 1, "cubrid heartbeat replication start " + shq (j->slave) + " 2>&1", &out, 300);
	if (rc != 0 || out.find ("fail") != string::npos)
	  {
	    f->exit_code = rc;
	    f->has_exit = rc > 0;
	    FAIL (rc == -2 ? "cancelled" : "command_failed", "replica " + r->host + ": cubrid heartbeat replication start failed: " + trim (out));
	  }
      }
    if (j->reps.empty ())
      {
	job_log (j, "no replicas");
      }
    return true;
  }

  bool st_verify (job_t *j, step_fail *f)
  {
    string last;
    for (int i = 0; i < 40; i++)
      {
	string out;
	remote_on (j, 0, "cubrid changemode " + shq (j->db + "@localhost") + " 2>&1", &out, 60);
	last = trim (out);
	if (out.find ("is standby") != string::npos)
	  {
	    return true;
	  }
	if (j->cancel.load ())
	  {
	    FAIL ("cancelled", "cancelled");
	  }
	sleep (3);
      }
    FAIL ("verify_failed", "slave " + j->slave + ": did not become standby: " + last);
  }

  bool st_cleanup (job_t *j, step_fail *f)
  {
    string msg;
    if (!j->m_bk_dir.empty ())
      {
	remove_tree (j->m_bk_dir);
	job_log (j, "removed " + j->m_bk_dir);
      }
    if (!j->s_bk_dir.empty () && reconnect_node (j, 0, &msg))
      {
	remote_on (j, 0, "rm -rf " + shq (j->s_bk_dir), NULL, 600);
      }
    return true;
  }

#undef FAIL

  /* ------------------------------------------------------------ rollback */

  bool has_undo (const string &id)
  {
    static const char *ids[] = { "convert_config", "convert_restart", "slave_config", "master_backup", "send_backup",
				 "slave_keep_old_db", "slave_restore", "slave_copylog", "slave_ha_start", "master_config",
				 "replication_start", "replica_config", "replica_replication_start", NULL
			       };
    for (int i = 0; ids[i] != NULL; i++)
      {
	if (id == ids[i])
	  {
	    return true;
	  }
      }
    return false;
  }

  /* undo one finished (or failed) step; true when it is back as before */
  bool rollback_step (job_t *j, const string &id)
  {
    string msg, out;
    bool ok = true;
    if (id == "convert_restart" && j->c_restarted)
      {
	/* back to a single database: heartbeat off, cub_master down; convert_config is undone next, then the
	   database starts again without HA (in the convert_config undo) */
	run_local (j, { bin (j, "cubrid"), "heartbeat", "stop" }, NULL, 600);
	ok = stop_local_cub_master (j);
      }
    else if (id == "convert_config")
      {
	ok = restore_edits (j, -1);
	if (j->c_restarted)
	  {
	    int rc = run_local (j, { bin (j, "cubrid"), "server", "start", j->db }, &out, 600);
	    ok = rc == 0 && out.find (": fail") == string::npos && ok;
	    job_log (j, string ("service interruption ends: ") + j->db + (ok ? " runs again without HA" : " did not start again"));
	  }
      }
    else if (id == "replica_replication_start")
      {
	/* before replica_config is undone: replication stop fails once the node is not in ha_node_list, and the
	   command then still ends with "success", so the output is read */
	for (size_t k = 0; k < j->reps.size (); k++)
	  {
	    rep_t *r = j->reps[k].get ();
	    if (!r->repl_started)
	      {
		continue;
	      }
	    out.clear ();
	    bool one = reconnect_node (j, (int) k + 1, &msg)
		       && remote_on (j, (int) k + 1, "cubrid heartbeat replication stop " + shq (j->slave) + " 2>&1", &out, 300) == 0
		       && out.find (": fail") == string::npos;
	    if (one && !r->repl_dir_existed)
	      {
		one = remote_on (j, (int) k + 1, "rm -rf " + shq (slave_log_dir (j, r->copy_base)), NULL, 120) == 0;
	      }
	    ok = one && ok;
	  }
      }
    else if (id == "replica_config")
      {
	for (size_t k = 0; k < j->reps.size (); k++)
	  {
	    ok = restore_edits (j, (int) k + 1) && ok;
	    if (j->reps[k]->reloaded)
	      {
		ok = remote_on (j, (int) k + 1, "cubrid heartbeat reload 2>&1", NULL, 300) == 0 && ok;
	      }
	  }
      }
    else if (id == "replication_start" && j->repl_started)
      {
	int rc = run_local (j, { bin (j, "cubrid"), "heartbeat", "replication", "stop", j->slave }, &out, 300);
	ok = rc == 0 && out.find (": fail") == string::npos;
	if (ok && !j->m_repl_dir_existed)
	  {
	    ok = remove_tree (slave_log_dir (j, j->m_copy_base));
	  }
      }
    else if (id == "master_config")
      {
	ok = restore_edits (j, -1);
	if (j->reloaded)
	  {
	    ok = run_local (j, { bin (j, "cubrid"), "heartbeat", "reload" }, NULL, 300) == 0 && ok;
	  }
      }
    else if (id == "slave_ha_start" && j->ha_started)
      {
	ok = reconnect_node (j, 0, &msg) && remote_daemon (j, 0, "cubrid heartbeat stop " + shq (j->db), NULL, 600) == 0;
	/* heartbeat start left cub_master in HA mode; take it down (slave_config undo starts the old one again) */
	remote_on (j, 0, "cub_commdb -A 2>&1", NULL, 120);
	for (int i = 0; i < 30 && slave_master_running (j); i++)
	  {
	    sleep (1);
	  }
	ok = !slave_master_running (j) && ok;
      }
    else if (id == "slave_copylog" && !j->s_repl_dir.empty ())
      {
	ok = reconnect_node (j, 0, &msg) && remote_on (j, 0, "rm -rf " + shq (j->s_repl_dir), NULL, 120) == 0;
      }
    else if (id == "slave_restore" && j->restored_new)
      {
	ok = reconnect_node (j, 0, &msg)
	     && remote_on (j, 0, "rm -rf " + shq (j->s_db_line[1]) + " " + shq (j->s_db_line[3]), NULL, 600) == 0;
      }
    else if (id == "slave_keep_old_db")
      {
	ok = reconnect_node (j, 0, &msg);
	for (auto it = j->moved.rbegin (); ok && it != j->moved.rend (); ++it)
	  {
	    bool r = remote_on (j, 0, "rm -rf " + shq (it->first) + " && mv " + shq (it->second) + " " + shq (it->first), NULL, 600) == 0;
	    job_log (j, string (r ? "moved back " : "could not move back ") + it->second);
	    ok = r && ok;
	  }
	if (ok)
	  {
	    std::lock_guard<std::mutex> lg (j->mu);
	    j->kept.clear ();
	  }
      }
    else if (id == "send_backup" && !j->s_bk_dir.empty ())
      {
	ok = reconnect_node (j, 0, &msg) && remote_on (j, 0, "rm -rf " + shq (j->s_bk_dir), NULL, 600) == 0;
      }
    else if (id == "master_backup" && !j->m_bk_dir.empty ())
      {
	ok = remove_tree (j->m_bk_dir);
      }
    else if (id == "slave_config")
      {
	ok = restore_edits (j, 0);
	if (j->s_master_stopped)
	  {
	    /* it ran (without HA) before slave_ha_start took it down: start it again, now with the old settings */
	    remote_daemon (j, 0, "cub_master", NULL, 120);
	    bool up = false;
	    for (int i = 0; i < 20 && !(up = slave_master_running (j)); i++)
	      {
		sleep (1);
	      }
	    ok = up && ok;
	  }
      }
    return ok;
  }

  void finish (job_t *j, const string &status)
  {
    wipe (j->pw);
    j->ssh.close ();
    for (auto &r : j->reps)
      {
	wipe (r->pw);
	r->ssh.close ();
      }
    {
      std::lock_guard<std::mutex> lg (j->mu);
      j->status = status;
      j->phase = "done";
      j->ended = time (NULL);
    }
    save_state (j);
  }

  typedef bool (*step_fn) (job_t *, step_fail *);

  void run_job (std::shared_ptr<job_t> jp)
  {
    job_t *j = jp.get ();
    vector<step_fn> fns;
    if (j->kind == "convert_master")
      {
	fns = { st_convert_precheck, st_convert_config, st_convert_restart, st_convert_verify };
      }
    else
      {
	fns = { st_precheck, st_slave_connect, st_slave_check, st_replica_check, st_slave_config, st_master_backup,
		st_send_backup, st_slave_keep_old_db, st_slave_restore, st_slave_copylog, st_slave_ha_start,
		st_master_config, st_replication_start, st_replica_config, st_replica_replication_start, st_verify,
		st_cleanup
	      };
      }
    int count = (int) j->steps.size ();
    int failed_at = -1;
    step_fail f;
    for (int i = 0; i < count; i++)
      {
	string id = j->steps[i].id;
	if (j->cancel.load ())
	  {
	    f.error = "cancelled";
	    f.message = "cancelled before " + id;
	    failed_at = i - 1;
	    break;
	  }
	if (!j->start_ha && (id == "slave_ha_start" || id == "verify"))
	  {
	    set_step (j, i, "skipped", "start_ha=no");
	    continue;
	  }
	set_step (j, i, "running");
	job_log (j, "step " + std::to_string (i + 1) + "/" + std::to_string (count) + " " + id + ": " + j->steps[i].title);
	bool ok = false;
	try
	  {
	    ok = fns[i] (j, &f);
	  }
	catch (std::exception &e)
	  {
	    f.error = "internal";
	    f.message = e.what ();
	  }
	if (ok)
	  {
	    set_step (j, i, "done");
	    continue;
	  }
	if (f.error == "cancelled" || j->cancel.load ())
	  {
	    f.error = "cancelled";
	    f.message = "cancelled during " + id + (f.message.empty () || f.message == "cancelled" ? "" : " (" + f.message + ")");
	  }
	set_step (j, i, "failed", f.message);
	job_log (j, "FAILED: " + f.message);
	failed_at = i;
	break;
      }

    if (failed_at < 0 && f.error.empty ())
      {
	job_log (j, "completed");
	finish (j, "success");
	return;
      }

    {
      std::lock_guard<std::mutex> lg (j->mu);
      j->error = f.error;
      j->message = f.message;
      j->exit_code = f.exit_code;
      j->has_exit_code = f.has_exit;
      j->phase = "rollback";
    }
    j->rolling_back.store (true);
    save_state (j);
    job_log (j, "rolling back");
    for (int i = failed_at; i >= 0; i--)
      {
	string id = j->steps[i].id, state;
	{
	  std::lock_guard<std::mutex> lg (j->mu);
	  state = j->steps[i].state;
	}
	if ((state != "done" && state != "failed") || !has_undo (id))
	  {
	    continue;
	  }
	bool ok = false;
	try
	  {
	    ok = rollback_step (j, id);
	  }
	catch (std::exception &e)
	  {
	    ok = false;
	  }
	set_step (j, i, ok ? "rolled_back" : "rollback_failed");
      }
    finish (j, f.error == "cancelled" ? "cancelled" : "fail");
  }

  /* ------------------------------------------------------------ task helpers */

  string new_uuid ()
  {
    unsigned char r[8];
    FILE *fp = fopen ("/dev/urandom", "rb");
    if (fp == NULL || fread (r, 1, sizeof (r), fp) != sizeof (r))
      {
	for (size_t i = 0; i < sizeof (r); i++)
	  {
	    r[i] = (unsigned char) (rand () & 0xff);
	  }
      }
    if (fp != NULL)
      {
	fclose (fp);
      }
    char buf[40];
    snprintf (buf, sizeof (buf), "%lx", (unsigned long) time (NULL));
    string out = buf;
    for (size_t i = 0; i < sizeof (r); i++)
      {
	snprintf (buf, sizeof (buf), "%02x", r[i]);
	out += buf;
      }
    return out;
  }

  bool valid_uuid (const string &u)
  {
    return !u.empty () && u.size () <= 64 && u.find_first_not_of ("0123456789abcdef") == string::npos;
  }

  string state_root ()
  {
    return env_or ("CUBRID", "") + "/tmp/ha_make_slave";
  }

  int fail_resp (Json::Value &response, const string &note)
  {
    return build_server_header (response, ERR_WITH_MSG, note.c_str ());
  }

  /* take the password out of the request so no later copy of the request has it */
  string take_password (Json::Value &request)
  {
    string pw;
    if (request["os_password"].isString ())
      {
	pw = request["os_password"].asString ();
	request["os_password"] = "";
      }
    return pw;
  }

  string quick_cubrid_rel ()
  {
    string out;
    FILE *p = popen ((env_or ("CUBRID", "") + "/bin/cubrid_rel 2>/dev/null").c_str (), "r");
    if (p != NULL)
      {
	char buf[512];
	while (fgets (buf, sizeof (buf), p) != NULL)
	  {
	    if (out.empty () && strstr (buf, "CUBRID") != NULL)
	      {
		out = buf;
	      }
	  }
	pclose (p);
      }
    return out;
  }

  /* `cubrid ...` from the HTTP thread (quick checks before a job starts) */
  string quick_cubrid (const string &args)
  {
    string out;
    FILE *p = popen ((env_or ("CUBRID", "") + "/bin/cubrid " + args + " 2>&1").c_str (), "r");
    if (p != NULL)
      {
	char buf[512];
	while (fgets (buf, sizeof (buf), p) != NULL)
	  {
	    out += buf;
	  }
	pclose (p);
      }
    return out;
  }

  /* start a job; caller filled everything but uuid/state/steps */
  int start_job (std::shared_ptr<job_t> j, const char *defs[][2], int ndefs, Json::Value &response)
  {
    std::lock_guard<std::mutex> lg (g_jobs_mu);
    if (g_job)
      {
	std::lock_guard<std::mutex> jl (g_job->mu);
	if (g_job->status == "running")
	  {
	    return fail_resp (response, "another job is running: " + g_job->uuid);
	  }
      }
    j->uuid = new_uuid ();
    j->state_dir = state_root () + "/" + j->uuid;
    j->started = time (NULL);
    for (int i = 0; i < ndefs; i++)
      {
	step_t s;
	s.id = defs[i][0];
	s.title = defs[i][1];
	s.state = "pending";
	j->steps.push_back (s);
      }
    if (!mkdir_p (j->state_dir))
      {
	return fail_resp (response, "cannot create " + j->state_dir);
      }
    save_state (j.get ());
    g_job = j;
    try
      {
	std::thread (run_job, j).detach ();
      }
    catch (std::exception &e)
      {
	g_job.reset ();
	return fail_resp (response, string ("cannot start the job: ") + e.what ());
      }
    LOG_INFO ("ha_make_slave: started %s job %s (db %s)", j->kind.c_str (), j->uuid.c_str (), j->db.c_str ());
    response["uuid"] = j->uuid;
    response["job_kind"] = j->kind;
    response["job_status"] = "running";
    return build_server_header (response, ERR_NO_ERROR, "none");
  }
}

/* ---------------------------------------------------------------- tasks */

int
ext_ha_check_ssh (Json::Value &request, Json::Value &response)
{
  string pw = take_password (request);
  string host = req_str (request, "host");
  string user = req_str (request, "os_user");
  string want_fp = req_str (request, "hostkey_sha256");
  int port = 22;
  if (!valid_name (host) || !valid_name (user) || !parse_port (req_str (request, "ssh_port", "22"), &port))
    {
      wipe (pw);
      return fail_resp (response, "invalid host, os_user or ssh_port");
    }

  ssh_conn c;
  string msg;
  string err = c.connect (host, port, user, &msg);
  response["hostkey_type"] = c.keytype;
  response["hostkey_sha256"] = c.fp;
  response["local_version"] = parse_version (quick_cubrid_rel ());

  string result = "ok";
  if (!err.empty ())
    {
      result = err;
    }
  else if (want_fp.empty ())
    {
      result = "hostkey_unknown";
      msg = "confirm the host key " + c.fp + " and call again with hostkey_sha256";
    }
  else if (want_fp != c.fp)
    {
      result = "hostkey_mismatch";
      msg = "the host key is " + c.fp + ", not " + want_fp;
    }
  else if (pw.empty ())
    {
      wipe (pw);
      return fail_resp (response, "os_password is required once the host key is confirmed");
    }
  else
    {
      err = c.auth (pw, &msg);
      if (!err.empty ())
	{
	  result = err;
	}
      else
	{
	  string out;
	  int rc = c.exec ("echo \"CUBRID=$CUBRID\"; echo \"CUBRID_DATABASES=$CUBRID_DATABASES\"; "
			   "echo \"HOSTNAME=$(uname -n)\"; \"$CUBRID/bin/cubrid_rel\" 2>/dev/null | grep -m1 CUBRID", &out,
			   nullptr, NULL, 15);
	  string rel;
	  for (const string &l : lines_of (out))
	    {
	      if (l.compare (0, 7, "CUBRID=") == 0)
		{
		  response["remote_cubrid"] = l.substr (7);
		}
	      else if (l.compare (0, 17, "CUBRID_DATABASES=") == 0)
		{
		  response["remote_cubrid_databases"] = l.substr (17);
		}
	      else if (l.compare (0, 9, "HOSTNAME=") == 0)
		{
		  response["remote_hostname"] = l.substr (9);
		}
	      else if (l.find ("CUBRID ") == 0 && rel.empty ())
		{
		  rel = l;
		}
	    }
	  response["remote_cubrid_rel"] = rel;
	  response["remote_version"] = parse_version (rel);
	  if (rc < 0)
	    {
	      result = "error";
	      msg = "could not run commands on " + host;
	    }
	  else if (rel.empty () || response["remote_cubrid"].asString ().empty ())
	    {
	      result = "cubrid_not_found";
	      msg = "CUBRID was not found for " + user + " on " + host + " (login shell environment)";
	    }
	  else if (parse_version (rel) != response["local_version"].asString ())
	    {
	      result = "version_mismatch";
	      msg = host + " CUBRID " + parse_version (rel) + ", this server " + response["local_version"].asString ();
	    }
	}
    }
  wipe (pw);
  c.close ();
  response["result"] = result;
  response["message"] = (result == "ok") ? "" : msg;
  return build_server_header (response, ERR_NO_ERROR, "none");
}

int
ext_ha_convert_master (Json::Value &request, Json::Value &response)
{
  std::shared_ptr<job_t> j = std::make_shared<job_t> ();
  j->kind = "convert_master";
  j->db = req_str (request, "db_name");
  j->master = req_str (request, "master_host", local_hostname ());
  j->ha_group = req_str (request, "ha_group", "cubrid");
  j->ha_port_id = req_str (request, "ha_port_id", "59901");
  j->cubrid = env_or ("CUBRID", "");
  j->cubrid_dbs = env_or ("CUBRID_DATABASES", "");
  int port_id = 0;
  if (!valid_name (j->db) || !valid_name (j->master) || !valid_name (j->ha_group) || !parse_port (j->ha_port_id, &port_id))
    {
      return fail_resp (response, "invalid db_name, master_host, ha_group or ha_port_id");
    }
  /* quick checks so the screen can tell the user before anything stops */
  string conf, ha;
  bool ex = false;
  read_local_file (j->cubrid + "/conf/cubrid.conf", &conf, &ex);
  if (conf_get (conf, "[common]", "ha_mode") == "on")
    {
      return fail_resp (response, "already_ha");
    }
  vector<string> up = running_servers (quick_cubrid ("server status"));
  if (!contains (up, j->db))
    {
      return fail_resp (response, "database " + j->db + " is not running");
    }
  vector<string> others;
  for (const string &s : up)
    {
      if (s != j->db)
	{
	  others.push_back (s);
	}
    }
  if (!others.empty ())
    {
      response["running_databases"] = join (others, ",");
      return fail_resp (response, "other_db_running: " + join (others, ", ") + " (they would stop too; stop them first)");
    }
  read_local_file (j->cubrid + "/conf/cubrid_ha.conf", &ha, &ex);
  if (!conf_get (ha, "[common]", "ha_node_list").empty ())
    {
      return fail_resp (response, "cubrid_ha.conf already has ha_node_list; fix the HA settings by hand first");
    }
  response["master_restart"] = "yes";
  if (req_str (request, "confirm_master_restart") != "yes")
    {
      response["message"] = "converting " + j->db + " to an HA master restarts it (the service stops until it is the active master)";
      return fail_resp (response, "confirm_master_restart");
    }
  return start_job (j, CONVERT_STEPS, sizeof (CONVERT_STEPS) / sizeof (CONVERT_STEPS[0]), response);
}

int
ext_ha_make_slave (Json::Value &request, Json::Value &response)
{
  string pw = take_password (request);
  std::shared_ptr<job_t> j = std::make_shared<job_t> ();
  j->kind = "make_slave";
  j->pw = pw;
  wipe (pw);
  j->db = req_str (request, "db_name");
  j->slave = req_str (request, "slave_host");
  j->user = req_str (request, "os_user");
  j->fp = req_str (request, "hostkey_sha256");
  j->master = req_str (request, "master_host", local_hostname ());
  j->cubrid = env_or ("CUBRID", "");
  j->cubrid_dbs = env_or ("CUBRID_DATABASES", "");
  j->repl_home = req_str (request, "repl_log_home");
  j->m_bk_root = req_str (request, "master_backup_path", j->cubrid_dbs + "/ha_make_slave_backup");
  j->s_bk_root = req_str (request, "slave_backup_path");
  string start_ha = req_str (request, "start_ha", "yes");
  j->start_ha = (start_ha != "no");

  string bad;
  if (!valid_name (j->db))
    {
      bad = "db_name";
    }
  else if (!valid_name (j->slave))
    {
      bad = "slave_host";
    }
  else if (!valid_name (j->user))
    {
      bad = "os_user";
    }
  else if (!valid_name (j->master))
    {
      bad = "master_host";
    }
  else if (j->fp.compare (0, 7, "SHA256:") != 0)
    {
      bad = "hostkey_sha256";
    }
  else if (!parse_port (req_str (request, "ssh_port", "22"), &j->port))
    {
      bad = "ssh_port";
    }
  else if (!j->repl_home.empty () && !valid_path (j->repl_home))
    {
      bad = "repl_log_home";
    }
  else if (!valid_path (j->m_bk_root))
    {
      bad = "master_backup_path";
    }
  else if (!j->s_bk_root.empty () && !valid_path (j->s_bk_root))
    {
      bad = "slave_backup_path";
    }
  else if (start_ha != "yes" && start_ha != "no")
    {
      bad = "start_ha";
    }
  else if (j->pw.empty ())
    {
      bad = "os_password";
    }
  else if (j->slave == j->master)
    {
      bad = "slave_host (same as master_host)";
    }
  if (!request["replicas"].isNull () && !request["replicas"].isArray ())
    {
      bad = "replicas (must be an array)";
    }
  for (Json::UInt i = 0; request["replicas"].isArray () && i < request["replicas"].size (); i++)
    {
      Json::Value &e = request["replicas"][i];
      std::unique_ptr<rep_t> r (new rep_t ());
      if (e.isObject ())
	{
	  r->pw = take_password (e);
	  r->host = req_str (e, "host");
	  r->user = req_str (e, "os_user");
	  r->fp = req_str (e, "hostkey_sha256");
	}
      string what = "replicas[" + std::to_string (i) + "].";
      if (!bad.empty ())
	{
	}
      else if (!e.isObject () || !valid_name (r->host))
	{
	  bad = what + "host";
	}
      else if (!valid_name (r->user))
	{
	  bad = what + "os_user";
	}
      else if (r->fp.compare (0, 7, "SHA256:") != 0)
	{
	  bad = what + "hostkey_sha256";
	}
      else if (!parse_port (req_str (e, "ssh_port", "22"), &r->port))
	{
	  bad = what + "ssh_port";
	}
      else if (r->pw.empty ())
	{
	  bad = what + "os_password";
	}
      else if (r->host == j->master || r->host == j->slave)
	{
	  bad = what + "host (same as master_host or slave_host)";
	}
      for (auto &o : j->reps)
	{
	  if (bad.empty () && o->host == r->host)
	    {
	      bad = what + "host (listed twice)";
	    }
	}
      j->reps.push_back (std::move (r));
    }
  if (!bad.empty ())
    {
      return fail_resp (response, "invalid or missing " + bad);
    }
  string conf;
  bool ex = false;
  read_local_file (j->cubrid + "/conf/cubrid.conf", &conf, &ex);
  if (conf_get (conf, "[common]", "ha_mode") != "on")
    {
      response["message"] = j->db + " is not an HA master here; convert it with ha_convert_master first";
      return fail_resp (response, "master_not_ha");
    }
  return start_job (j, MAKE_STEPS, sizeof (MAKE_STEPS) / sizeof (MAKE_STEPS[0]), response);
}

int
ext_ha_make_slave_status (Json::Value &request, Json::Value &response)
{
  string uuid = req_str (request, "uuid");
  string off_s = req_str (request, "offset", "0");
  if (!valid_uuid (uuid) || off_s.empty () || off_s.find_first_not_of ("0123456789") != string::npos)
    {
      return fail_resp (response, "invalid uuid or offset");
    }
  unsigned long long offset = strtoull (off_s.c_str (), NULL, 10);

  std::shared_ptr<job_t> j;
  {
    std::lock_guard<std::mutex> lg (g_jobs_mu);
    if (g_job && g_job->uuid == uuid)
      {
	j = g_job;
      }
  }
  if (!j)
    {
      /* not in memory: CMS restarted, or an older job. Read what the state file says. */
      string data;
      bool ex = false;
      read_local_file (state_root () + "/" + uuid + "/state.json", &data, &ex);
      Json::Value v;
      Json::Reader r;
      if (!ex || !r.parse (data, v))
	{
	  return fail_resp (response, "no such job: " + uuid);
	}
      for (const string &k : v.getMemberNames ())
	{
	  if (k != "moved" && k != "edited_files" && k != "slave_ssh")
	    {
	      response[k] = v[k];
	    }
	}
      if (v["job_status"].asString () == "running")
	{
	  response["job_status"] = "interrupted";
	  response["phase"] = "done";
	}
      response["output"] = "";
      response["next_offset"] = off_s;
      return build_server_header (response, ERR_NO_ERROR, "none");
    }

  std::lock_guard<std::mutex> lg (j->mu);
  Json::Value v = job_state_json (j.get ());
  for (const string &k : v.getMemberNames ())
    {
      response[k] = v[k];
    }
  unsigned long long end = j->log_base + j->log.size ();
  if (offset < j->log_base)
    {
      offset = j->log_base;
      response["output_truncated"] = "yes";
    }
  if (offset > end)
    {
      offset = end;
    }
  response["output"] = j->log.substr (offset - j->log_base);
  response["next_offset"] = std::to_string (end);
  return build_server_header (response, ERR_NO_ERROR, "none");
}

int
ext_ha_make_slave_cancel (Json::Value &request, Json::Value &response)
{
  string uuid = req_str (request, "uuid");
  std::shared_ptr<job_t> j;
  {
    std::lock_guard<std::mutex> lg (g_jobs_mu);
    if (g_job && g_job->uuid == uuid)
      {
	j = g_job;
      }
  }
  if (!j)
    {
      return fail_resp (response, "no such running job: " + uuid);
    }
  j->cancel.store (true);
  job_log (j.get (), "cancel requested");
  std::lock_guard<std::mutex> lg (j->mu);
  response["uuid"] = uuid;
  response["job_status"] = j->status;
  response["phase"] = j->phase;
  return build_server_header (response, ERR_NO_ERROR, "none");
}

int
ext_ha_make_slave_cleanup (Json::Value &request, Json::Value &response)
{
  string pw = take_password (request);
  string uuid = req_str (request, "uuid");
  if (!valid_uuid (uuid) || pw.empty ())
    {
      wipe (pw);
      return fail_resp (response, "invalid uuid or missing os_password (of the new slave)");
    }
  string data;
  bool ex = false;
  read_local_file (state_root () + "/" + uuid + "/state.json", &data, &ex);
  Json::Value v;
  Json::Reader r;
  if (!ex || !r.parse (data, v))
    {
      wipe (pw);
      return fail_resp (response, "no such job: " + uuid);
    }
  if (v["job_kind"].asString () != "make_slave" || v["job_status"].asString () != "success")
    {
      wipe (pw);
      return fail_resp (response, "only a successful make_slave job's kept database can be removed");
    }
  const Json::Value &ss = v["slave_ssh"];
  string host = ss["host"].asString (), user = ss["os_user"].asString (), fp = ss["hostkey_sha256"].asString ();
  int port = ss["port"].isIntegral () ? ss["port"].asInt () : 22;
  ssh_conn c;
  string msg;
  string err = c.connect (host, port, user, &msg);
  if (err.empty () && c.fp != fp)
    {
      err = "hostkey_mismatch";
      msg = "the host key of " + host + " is " + c.fp + ", not " + fp;
    }
  if (err.empty ())
    {
      err = c.auth (pw, &msg);
    }
  wipe (pw);
  if (!err.empty ())
    {
      return fail_resp (response, err + ": " + msg);
    }
  Json::Value removed (Json::arrayValue);
  string suffix = ".ha_make_slave_" + uuid;
  for (const Json::Value &p : v["kept_db_paths"])
    {
      string path = p.asString ();
      /* only folders this job created by moving */
      if (!valid_path (path) || path.size () <= suffix.size ()
	  || path.compare (path.size () - suffix.size (), suffix.size (), suffix) != 0)
	{
	  continue;
	}
      if (c.exec ("rm -rf " + shq (path), NULL, nullptr, NULL, 600) == 0)
	{
	  removed.append (path);
	}
    }
  c.close ();
  v["kept_db_paths"] = Json::Value (Json::arrayValue);
  Json::StyledWriter w;
  write_local_file (state_root () + "/" + uuid + "/state.json", w.write (v));
  response["removed"] = removed;
  return build_server_header (response, ERR_NO_ERROR, "none");
}

#endif /* !WINDOWS */
