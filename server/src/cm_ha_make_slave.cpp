/*
 * HA slave POC (plan 2 of the HA slave review): this CMS turns its own server into
 * an HA slave of a running master. Work on the master goes through libssh (command
 * execution and SFTP); work on this server runs locally. No ha_make_slavedb.sh, no
 * system ssh/scp. Tasks and steps: docs/ha-make-slave-poc-api.md.
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

#include <libssh/libssh.h>
#include <libssh/sftp.h>
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

  class ssh_conn
  {
    public:
      ssh_session s = NULL;
      sftp_session sf = NULL;
      string fp, keytype;

      ~ssh_conn ()
      {
	close ();
      }

      void close ()
      {
	if (sf != NULL)
	  {
	    sftp_free (sf);
	    sf = NULL;
	  }
	if (s != NULL)
	  {
	    ssh_disconnect (s);
	    ssh_free (s);
	    s = NULL;
	  }
      }

      bool alive () const
      {
	return s != NULL && ssh_is_connected (s);
      }

      /* TCP + key exchange; fills fp/keytype. "" on success, else an error code */
      string connect (const string &host, int port, const string &user, string *msg)
      {
	std::call_once (g_ssh_init_once, []
	{
	  ssh_init ();
	});
	close ();
	s = ssh_new ();
	if (s == NULL)
	  {
	    *msg = "ssh_new failed";
	    return "error";
	  }
	long timeout = 10;
	bool no = false;
	ssh_options_set (s, SSH_OPTIONS_HOST, host.c_str ());
	ssh_options_set (s, SSH_OPTIONS_PORT, &port);
	ssh_options_set (s, SSH_OPTIONS_USER, user.c_str ());
	ssh_options_set (s, SSH_OPTIONS_TIMEOUT, &timeout);
	ssh_options_set (s, SSH_OPTIONS_PROCESS_CONFIG, &no);
	if (ssh_connect (s) != SSH_OK)
	  {
	    *msg = string ("cannot connect to ") + host + ":" + std::to_string (port) + ": " + ssh_get_error (s);
	    close ();
	    return "unreachable";
	  }
	ssh_key key = NULL;
	if (ssh_get_server_publickey (s, &key) != SSH_OK)
	  {
	    *msg = string ("cannot read the host key: ") + ssh_get_error (s);
	    close ();
	    return "error";
	  }
	unsigned char *hash = NULL;
	size_t hlen = 0;
	if (ssh_get_publickey_hash (key, SSH_PUBLICKEY_HASH_SHA256, &hash, &hlen) == 0)
	  {
	    char *f = ssh_get_fingerprint_hash (SSH_PUBLICKEY_HASH_SHA256, hash, hlen);
	    if (f != NULL)
	      {
		fp = f;
		ssh_string_free_char (f);
	      }
	    ssh_clean_pubkey_hash (&hash);
	  }
	const char *kt = ssh_key_type_to_char (ssh_key_type (key));
	keytype = kt ? kt : "";
	ssh_key_free (key);
	if (fp.empty ())
	  {
	    *msg = "cannot compute the host key fingerprint";
	    close ();
	    return "error";
	  }
	return "";
      }

      string auth (const string &password, string *msg)
      {
	int rc = ssh_userauth_none (s, NULL);
	if (rc == SSH_AUTH_SUCCESS)
	  {
	    return "";
	  }
	int methods = ssh_userauth_list (s, NULL);
	if (methods & SSH_AUTH_METHOD_PASSWORD)
	  {
	    rc = ssh_userauth_password (s, NULL, password.c_str ());
	    if (rc == SSH_AUTH_SUCCESS)
	      {
		return "";
	      }
	  }
	if (methods & SSH_AUTH_METHOD_INTERACTIVE)
	  {
	    rc = ssh_userauth_kbdint (s, NULL, NULL);
	    for (int round = 0; rc == SSH_AUTH_INFO && round < 5; round++)
	      {
		int n = ssh_userauth_kbdint_getnprompts (s);
		for (int i = 0; i < n; i++)
		  {
		    ssh_userauth_kbdint_setanswer (s, i, password.c_str ());
		  }
		rc = ssh_userauth_kbdint (s, NULL, NULL);
	      }
	    if (rc == SSH_AUTH_SUCCESS)
	      {
		return "";
	      }
	  }
	if (!(methods & (SSH_AUTH_METHOD_PASSWORD | SSH_AUTH_METHOD_INTERACTIVE)))
	  {
	    *msg = "the server accepts neither password nor keyboard-interactive login";
	  }
	else
	  {
	    *msg = "login failed (wrong OS user or password)";
	  }
	return "auth_failed";
      }

      /*
       * run `bash -lc '<cmd>'` (login shell: the account's CUBRID environment).
       * returns the exit status, -1 on a channel error, -2 when cancelled, -3 on timeout.
       */
      int exec (const string &cmd, string *out, const std::function<void (const string &)> &on_line,
		const std::atomic<bool> *cancel, int timeout_sec)
      {
	ssh_channel ch = ssh_channel_new (s);
	if (ch == NULL)
	  {
	    return -1;
	  }
	if (ssh_channel_open_session (ch) != SSH_OK)
	  {
	    ssh_channel_free (ch);
	    return -1;
	  }
	/* the redirect applies to the whole command, not to the last command of a pipeline */
	string full = "bash -lc " + shq ("{ " + cmd + "\n} </dev/null");
	if (ssh_channel_request_exec (ch, full.c_str ()) != SSH_OK)
	  {
	    ssh_channel_close (ch);
	    ssh_channel_free (ch);
	    return -1;
	  }
	time_t start = time (NULL);
	string pending;
	char buf[8192];
	int result = 0;
	for (;;)
	  {
	    bool got = false;
	    for (int is_err = 0; is_err <= 1; is_err++)
	      {
		int n = ssh_channel_read_timeout (ch, buf, sizeof (buf), is_err, is_err ? 0 : 300);
		if (n == SSH_ERROR)
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
	    if (ssh_channel_is_eof (ch) && !got)
	      {
		break;
	      }
	    if (cancel != NULL && cancel->load ())
	      {
		ssh_channel_request_send_signal (ch, "TERM");
		result = -2;
		break;
	      }
	    if (timeout_sec > 0 && time (NULL) - start > timeout_sec)
	      {
		ssh_channel_request_send_signal (ch, "TERM");
		result = -3;
		break;
	      }
	  }
	if (!pending.empty () && on_line)
	  {
	    on_line (pending);
	  }
	if (result == 0)
	  {
	    ssh_channel_send_eof (ch);
	    result = ssh_channel_get_exit_status (ch);
	    if (result < 0)
	      {
		result = -1;
	      }
	  }
	ssh_channel_close (ch);
	ssh_channel_free (ch);
	return result;
      }

      bool sftp_ready ()
      {
	if (sf != NULL)
	  {
	    return true;
	  }
	sf = sftp_new (s);
	if (sf == NULL)
	  {
	    return false;
	  }
	if (sftp_init (sf) != SSH_OK)
	  {
	    sftp_free (sf);
	    sf = NULL;
	    return false;
	  }
	return true;
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
	sftp_file f = sftp_open (sf, path.c_str (), O_RDONLY, 0);
	if (f == NULL)
	  {
	    return sftp_get_error (sf) == SSH_FX_NO_SUCH_FILE;
	  }
	*exists = true;
	char buf[16384];
	ssize_t n;
	while ((n = sftp_read (f, buf, sizeof (buf))) > 0)
	  {
	    data->append (buf, n);
	  }
	sftp_close (f);
	return n == 0;
      }

      bool write_file (const string &path, const string &data)
      {
	if (!sftp_ready ())
	  {
	    return false;
	  }
	sftp_file f = sftp_open (sf, path.c_str (), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (f == NULL)
	  {
	    return false;
	  }
	size_t off = 0;
	while (off < data.size ())
	  {
	    ssize_t n = sftp_write (f, data.data () + off, std::min ((size_t) 32768, data.size () - off));
	    if (n <= 0)
	      {
		sftp_close (f);
		return false;
	      }
	    off += n;
	  }
	return sftp_close (f) == SSH_OK;
      }

      /* regular files in a remote folder with their sizes */
      bool list_files (const string &dir, vector<std::pair<string, uint64_t>> *files)
      {
	if (!sftp_ready ())
	  {
	    return false;
	  }
	sftp_dir d = sftp_opendir (sf, dir.c_str ());
	if (d == NULL)
	  {
	    return false;
	  }
	sftp_attributes a;
	while ((a = sftp_readdir (sf, d)) != NULL)
	  {
	    if (a->type == SSH_FILEXFER_TYPE_REGULAR)
	      {
		files->push_back (std::make_pair (string (a->name), (uint64_t) a->size));
	      }
	    sftp_attributes_free (a);
	  }
	bool ok = sftp_dir_eof (d);
	sftp_closedir (d);
	return ok;
      }

      bool stat_size (const string &path, uint64_t *size)
      {
	if (!sftp_ready ())
	  {
	    return false;
	  }
	sftp_attributes a = sftp_stat (sf, path.c_str ());
	if (a == NULL)
	  {
	    return false;
	  }
	*size = a->size;
	sftp_attributes_free (a);
	return true;
      }

      /* download, continuing a partial local file; false on error or cancel */
      bool download (const string &remote, const string &local, uint64_t size, const std::atomic<bool> *cancel,
		     string *msg)
      {
	if (!sftp_ready ())
	  {
	    *msg = "SFTP is not available";
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
	sftp_file f = sftp_open (sf, remote.c_str (), O_RDONLY, 0);
	if (f == NULL)
	  {
	    *msg = "cannot open " + remote + ": " + ssh_get_error (s);
	    return false;
	  }
	if (have > 0 && sftp_seek64 (f, have) != 0)
	  {
	    sftp_close (f);
	    *msg = "cannot resume " + remote;
	    return false;
	  }
	int fd = open (local.c_str (), O_WRONLY | O_CREAT | (have > 0 ? O_APPEND : O_TRUNC), 0600);
	if (fd < 0)
	  {
	    sftp_close (f);
	    *msg = "cannot write " + local + ": " + strerror (errno);
	    return false;
	  }
	char buf[65536];
	ssize_t n;
	bool ok = true;
	while ((n = sftp_read (f, buf, sizeof (buf))) > 0)
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
	    *msg = "read failed on " + remote + ": " + ssh_get_error (s);
	    ok = false;
	  }
	::close (fd);
	sftp_close (f);
	return ok;
      }
  };

  /* ------------------------------------------------------------ jobs */

  struct step_t
  {
    string id, title, state, detail;
  };

  struct edit_t
  {
    bool remote;
    string path, bak;
    bool existed;
  };

  struct job_t
  {
    /* fixed after start */
    string uuid, db, master, user, fp, slave, repl_home, m_bk_root, s_bk_root;
    int port = 22;
    bool start_ha = true;
    string cubrid, cubrid_dbs, local_version, state_dir;

    /* only the job thread touches these */
    string pw;
    ssh_conn ssh;
    string m_cubrid, m_dbs, m_version, m_node_list, m_port_id, m_db_list, m_sync_mode;
    vector<string> m_db_line, s_db_line;
    string m_bk_dir, s_bk_dir, repl_dir;
    vector<edit_t> edits;
    vector<std::pair<string, string>> moved;
    bool restored_new = false, repl_started = false, ha_started = false, reloaded = false;

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
    std::atomic<bool> cancel {false};
  };

  std::mutex g_jobs_mu;
  std::shared_ptr<job_t> g_job;	/* the current or last job */

  const size_t LOG_MAX = 1024 * 1024;

  const char *STEP_DEFS[][2] =
  {
    {"precheck", "Check this server"},
    {"connect", "Connect to the master over ssh"},
    {"master_check", "Check the master"},
    {"slave_config", "Prepare this server's HA settings"},
    {"master_backup", "Back up the master database online"},
    {"fetch_backup", "Fetch the backup"},
    {"keep_old_db", "Keep the old slave database aside"},
    {"restore", "Restore as a slave"},
    {"copylog", "First copy of the replication log"},
    {"master_config", "Add this server to the master's HA settings"},
    {"replication_start", "Start replication on the master"},
    {"slave_ha_start", "Start HA on this server"},
    {"verify", "Check that this server is standby"},
    {"cleanup", "Remove the backup files"},
  };
  const int STEP_COUNT = sizeof (STEP_DEFS) / sizeof (STEP_DEFS[0]);

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
    v["db_name"] = j->db;
    v["master_host"] = j->master;
    v["slave_host"] = j->slave;
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
    v["step_count"] = STEP_COUNT;
    v["step_no"] = j->cur + 1;
    v["step"] = (j->cur >= 0 && j->cur < STEP_COUNT) ? j->steps[j->cur].id : "";
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

  void save_state (job_t *j)
  {
    Json::Value v;
    {
      std::lock_guard<std::mutex> lg (j->mu);
      v = job_state_json (j);
    }
    Json::Value moved (Json::arrayValue);
    for (auto &m : j->moved)
      {
	Json::Value x;
	x["from"] = m.first;
	x["to"] = m.second;
	moved.append (x);
      }
    v["moved"] = moved;
    Json::Value edits (Json::arrayValue);
    for (auto &e : j->edits)
      {
	Json::Value x;
	x["node"] = e.remote ? "master" : "slave";
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

  /* a failed step: what went wrong, for status */
  struct step_fail
  {
    string error, message;
    int exit_code;
    bool has_exit;
  };

  /* ------------------------------------------------------------ local commands */

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
	if (!done && term_at == 0 && (j->cancel.load () || (timeout_sec > 0 && time (NULL) - start > timeout_sec)))
	  {
	    result = j->cancel.load () ? -2 : -3;
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

  /* a cub_master process of this user is running (argv element whose basename is cub_master) */
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
	for (const string &arg : split_any (data, string (1, '\0').c_str ()))
	  {
	    size_t sl = arg.rfind ('/');
	    if ((sl == string::npos ? arg : arg.substr (sl + 1)) == "cub_master")
	      {
		struct stat st;
		if (stat ((string ("/proc/") + e->d_name).c_str (), &st) == 0 && st.st_uid == getuid ())
		  {
		    found = true;
		  }
		break;
	      }
	  }
      }
    closedir (d);
    return found;
  }

  string bin (job_t *j, const char *name)
  {
    return j->cubrid + "/bin/" + name;
  }

  /* ------------------------------------------------------------ remote helpers */

  int remote (job_t *j, const string &cmd, string *out, int timeout_sec)
  {
    job_log (j, "master$ " + cmd);
    return j->ssh.exec (cmd, out, [j] (const string &l)
    {
      job_log (j, "  " + l);
    }, &j->cancel, timeout_sec);
  }

  bool reconnect (job_t *j, string *msg)
  {
    if (j->ssh.alive ())
      {
	return true;
      }
    string err = j->ssh.connect (j->master, j->port, j->user, msg);
    if (!err.empty ())
      {
	return false;
      }
    if (j->ssh.fp != j->fp)
      {
	*msg = "the master host key changed: " + j->ssh.fp;
	j->ssh.close ();
	return false;
      }
    err = j->ssh.auth (j->pw, msg);
    if (!err.empty ())
      {
	j->ssh.close ();
	return false;
      }
    return true;
  }

  /* edit one file (local or on the master): keep path.bak.<uuid>, write the new content in place */
  bool edit_file (job_t *j, bool on_master, const string &path, const std::function<string (const string &)> &change,
		  string *msg)
  {
    string data;
    bool exists = false;
    bool ok = on_master ? j->ssh.read_file (path, &data, &exists) : read_local_file (path, &data, &exists);
    if (!ok)
      {
	*msg = "cannot read " + path;
	return false;
      }
    string next = change (data);
    if (next == data && exists)
      {
	job_log (j, string (on_master ? "master: " : "") + path + " already has the values");
	return true;
      }
    edit_t e;
    e.remote = on_master;
    e.path = path;
    e.bak = path + ".bak." + j->uuid;
    e.existed = exists;
    if (exists)
      {
	ok = on_master ? j->ssh.write_file (e.bak, data) : write_local_file (e.bak, data);
	if (!ok)
	  {
	    *msg = "cannot write " + e.bak;
	    return false;
	  }
      }
    j->edits.push_back (e);
    save_state (j);
    if (on_master)
      {
	string tmp = path + ".tmp." + j->uuid;
	ok = j->ssh.write_file (tmp, next) && remote (j, "mv -f " + shq (tmp) + " " + shq (path), NULL, 30) == 0;
      }
    else
      {
	ok = write_local_file (path, next);
      }
    if (!ok)
      {
	*msg = "cannot write " + path;
	return false;
      }
    job_log (j, string (on_master ? "master: " : "") + "edited " + path + (exists ? " (kept " + e.bak + ")" : " (new)"));
    return true;
  }

  /* ------------------------------------------------------------ steps */

#define FAIL(code, text) do { f->error = (code); f->message = (text); return false; } while (0)

  bool st_precheck (job_t *j, step_fail *f)
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
    out.clear ();
    run_local (j, { bin (j, "cubrid"), "server", "status" }, &out, 60);
    for (const string &l : lines_of (out))
      {
	vector<string> t = split_any (trim (l), " ");
	if (t.size () >= 2 && t[0] == "Server" && t[1] == j->db)
	  {
	    FAIL ("slave_db_running", "database " + j->db + " is running on this server; stop it first");
	  }
      }
    if (j->start_ha)
      {
	string conf;
	bool ex = false;
	read_local_file (j->cubrid + "/conf/cubrid.conf", &conf, &ex);
	if (conf_get (conf, "[common]", "ha_mode") != "on" && cub_master_running ())
	  {
	    FAIL ("precheck_failed", "cub_master is running without HA on this server, so heartbeat start would fail. "
		  "Stop the CUBRID service here (keep only the manager running), or use start_ha=no");
	  }
      }
    mkdir_p (j->state_dir);
    job_log (j, "this server: " + j->slave + ", CUBRID " + j->local_version);
    return true;
  }

  bool st_connect (job_t *j, step_fail *f)
  {
    string msg;
    string err = j->ssh.connect (j->master, j->port, j->user, &msg);
    if (!err.empty ())
      {
	FAIL (err == "unreachable" ? "unreachable" : "internal", msg);
      }
    if (j->ssh.fp != j->fp)
      {
	j->ssh.close ();
	FAIL ("hostkey_mismatch", "the master host key is " + j->ssh.fp + ", not the confirmed " + j->fp);
      }
    err = j->ssh.auth (j->pw, &msg);
    if (!err.empty ())
      {
	j->ssh.close ();
	FAIL ("auth_failed", msg);
      }
    job_log (j, "logged in to " + j->user + "@" + j->master + " (" + j->ssh.keytype + " " + j->ssh.fp + ")");
    return true;
  }

  bool st_master_check (job_t *j, step_fail *f)
  {
    string out;
    int rc = remote (j, "echo \"CUBRID=$CUBRID\"; echo \"CUBRID_DATABASES=$CUBRID_DATABASES\"; "
		     "echo \"HOSTNAME=$(uname -n)\"; \"$CUBRID/bin/cubrid_rel\" | grep -m1 CUBRID", &out, 60);
    for (const string &l : lines_of (out))
      {
	if (l.compare (0, 7, "CUBRID=") == 0)
	  {
	    j->m_cubrid = l.substr (7);
	  }
	else if (l.compare (0, 17, "CUBRID_DATABASES=") == 0)
	  {
	    j->m_dbs = l.substr (17);
	  }
	else if (l.find ("CUBRID ") == 0 && j->m_version.empty ())
	  {
	    j->m_version = parse_version (l);
	  }
      }
    if (rc != 0 || j->m_cubrid.empty () || j->m_version.empty ())
      {
	FAIL ("precheck_failed", "CUBRID was not found for " + j->user + " on the master");
      }
    if (j->m_version != j->local_version)
      {
	FAIL ("version_mismatch", "master CUBRID " + j->m_version + ", this server " + j->local_version);
      }
    out.clear ();
    remote (j, "cubrid changemode " + shq (j->db + "@localhost") + " 2>&1", &out, 60);
    if (out.find ("is active") == string::npos)
      {
	FAIL ("master_not_active", "database " + j->db + " is not active on the master: " + trim (out));
      }
    string ha, dbtxt;
    bool ex1 = false, ex2 = false;
    if (!j->ssh.read_file (j->m_cubrid + "/conf/cubrid_ha.conf", &ha, &ex1) || !ex1)
      {
	FAIL ("precheck_failed", "cannot read cubrid_ha.conf on the master");
      }
    j->m_node_list = conf_get (ha, "[common]", "ha_node_list");
    j->m_port_id = conf_get (ha, "[common]", "ha_port_id");
    j->m_db_list = conf_get (ha, "[common]", "ha_db_list");
    j->m_sync_mode = conf_get (ha, "[common]", "ha_copy_sync_mode");
    if (j->m_node_list.empty () || !contains (node_list_hosts (j->m_node_list), j->master))
      {
	FAIL ("precheck_failed", "the master's ha_node_list (" + j->m_node_list + ") does not have " + j->master);
      }
    if (!contains (split_any (j->m_db_list, " ,:"), j->db))
      {
	FAIL ("precheck_failed", "the master's ha_db_list (" + j->m_db_list + ") does not have " + j->db);
      }
    if (!j->ssh.read_file (j->m_dbs + "/databases.txt", &dbtxt, &ex2) || !ex2)
      {
	FAIL ("precheck_failed", "cannot read databases.txt on the master");
      }
    j->m_db_line = dbtxt_line (dbtxt, j->db);
    if (j->m_db_line.size () < 4)
      {
	FAIL ("precheck_failed", "databases.txt on the master has no line for " + j->db);
      }
    if (j->m_bk_root.empty ())
      {
	j->m_bk_root = j->m_dbs + "/ha_make_slave_backup";
      }
    if (!valid_path (j->m_bk_root) || !valid_path (j->m_db_line[1]) || !valid_path (j->m_db_line[3]))
      {
	FAIL ("precheck_failed", "unexpected path on the master");
      }
    job_log (j, "master: CUBRID " + j->m_version + ", ha_node_list=" + j->m_node_list + ", " + j->db + " is active");
    return true;
  }

  bool st_slave_config (job_t *j, step_fail *f)
  {
    string msg;
    string conf = j->cubrid + "/conf/cubrid.conf";
    if (!edit_file (j, false, conf, [] (const string &c)
    {
      return conf_get (c, "[common]", "ha_mode") == "on" ? c : conf_set (c, "[common]", "ha_mode", "on");
      }, &msg))
      {
	FAIL ("config_edit_failed", msg);
      }
    string haconf = j->cubrid + "/conf/cubrid_ha.conf";
    if (!edit_file (j, false, haconf, [j] (const string &c)
    {
      bool ch;
      string list = node_list_add (j->m_node_list, j->slave, &ch);
	string dbl = conf_get (c, "[common]", "ha_db_list");
	vector<string> dbs = split_any (dbl, " ,:");
	if (!contains (dbs, j->db))
	  {
	    dbs.push_back (j->db);
	  }
	string n = conf_set (c, "[common]", "ha_mode", "on");
	if (!j->m_port_id.empty ())
	  {
	    n = conf_set (n, "[common]", "ha_port_id", j->m_port_id);
	  }
	n = conf_set (n, "[common]", "ha_node_list", list);
	n = conf_set (n, "[common]", "ha_db_list", join (dbs, ","));
	string sync = sync_mode_for (j->m_sync_mode, node_list_hosts (list).size ());
	if (!sync.empty ())
	  {
	    n = conf_set (n, "[common]", "ha_copy_sync_mode", sync);
	  }
	if (j->repl_home != j->cubrid_dbs)
	  {
	    n = conf_set (n, "[common]", "ha_copy_log_base", j->repl_home);
	  }
	return n;
      }, &msg))
      {
	FAIL ("config_edit_failed", msg);
      }
    string dbtxt_path = j->cubrid_dbs + "/databases.txt";
    string dbtxt;
    bool ex = false;
    read_local_file (dbtxt_path, &dbtxt, &ex);
    vector<string> cur = dbtxt_line (dbtxt, j->db);
    vector<string> want = (cur.size () >= 4) ? cur : j->m_db_line;
    vector<string> hosts = split_any (want[2], ":");
    vector<string> ordered;
    for (const string &h : node_list_hosts (j->m_node_list))
      {
	if (!contains (ordered, h))
	  {
	    ordered.push_back (h);
	  }
      }
    if (!contains (ordered, j->slave))
      {
	ordered.push_back (j->slave);
      }
    for (const string &h : hosts)
      {
	if (!contains (ordered, h) && h != "localhost")
	  {
	    ordered.push_back (h);
	  }
      }
    want[2] = join (ordered, ":");
    if (!edit_file (j, false, dbtxt_path, [&] (const string &c)
    {
      return dbtxt_line (c, j->db) == want ? c : dbtxt_put (c, j->db, want);
      }, &msg))
      {
	FAIL ("config_edit_failed", msg);
      }
    j->s_db_line = want;
    /* read back */
    string c1, c2, c3;
    bool e1, e2, e3;
    read_local_file (conf, &c1, &e1);
    read_local_file (haconf, &c2, &e2);
    read_local_file (dbtxt_path, &c3, &e3);
    if (conf_get (c1, "[common]", "ha_mode") != "on" || conf_get (c2, "[common]", "ha_node_list").empty ()
	|| dbtxt_line (c3, j->db) != want)
      {
	FAIL ("config_edit_failed", "the settings did not read back as written");
      }
    if (!valid_path (want[1]) || !valid_path (want[3]))
      {
	FAIL ("precheck_failed", "unexpected volume or log path for " + j->db);
      }
    job_log (j, "this server: ha_node_list=" + conf_get (c2, "[common]", "ha_node_list") + ", " + j->db + " -> " + want[1]);
    return true;
  }

  bool st_master_backup (job_t *j, step_fail *f)
  {
    j->m_bk_dir = j->m_bk_root + "/" + j->uuid;
    save_state (j);
    string dir = shq (j->m_bk_dir);
    int rc = remote (j, "mkdir -p " + dir + " && cubrid backupdb -C -D " + dir + " -o " + shq (j->m_bk_dir + "/backup.out")
		     + " --no-check " + shq (j->db + "@localhost") + " 2>&1", NULL, 6 * 3600);
    if (rc != 0)
      {
	f->exit_code = rc;
	f->has_exit = rc > 0;
	FAIL (rc == -2 ? "cancelled" : "command_failed", "cubrid backupdb on the master failed (" + std::to_string (rc) + ")");
      }
    return true;
  }

  bool st_fetch_backup (job_t *j, step_fail *f)
  {
    j->s_bk_dir = j->s_bk_root + "/" + j->uuid;
    save_state (j);
    if (!mkdir_p (j->s_bk_dir))
      {
	FAIL ("transfer_failed", "cannot create " + j->s_bk_dir);
      }
    vector<std::pair<string, uint64_t>> files;
    if (!j->ssh.list_files (j->m_bk_dir, &files))
      {
	FAIL ("transfer_failed", "cannot list " + j->m_bk_dir + " on the master");
      }
    vector<std::pair<string, string>> todo;	/* remote path, local name */
    for (auto &fi : files)
      {
	if (fi.first != "backup.out")
	  {
	    todo.push_back (std::make_pair (j->m_bk_dir + "/" + fi.first, fi.first));
	  }
      }
    string bkvinf = j->db + "_bkvinf";
    todo.push_back (std::make_pair (j->m_db_line[3] + "/" + bkvinf, bkvinf));
    string sums_cmd = "sha256sum";
    for (auto &t : todo)
      {
	sums_cmd += " " + shq (t.first);
      }
    string sums;
    if (remote (j, sums_cmd, &sums, 3600) != 0)
      {
	FAIL ("transfer_failed", "sha256sum on the master failed");
      }
    for (auto &t : todo)
      {
	string want;
	for (const string &l : lines_of (sums))
	  {
	    size_t sp = l.find ("  ");
	    if (sp != string::npos && l.substr (sp + 2) == t.first)
	      {
		want = l.substr (0, sp);
	      }
	  }
	uint64_t size = 0;
	if (want.empty () || !j->ssh.stat_size (t.first, &size))
	  {
	    FAIL ("transfer_failed", "cannot read size or checksum of " + t.first);
	  }
	string local = j->s_bk_dir + "/" + t.second, msg;
	bool ok = false;
	for (int attempt = 1; attempt <= 3 && !ok; attempt++)
	  {
	    if (j->cancel.load ())
	      {
		FAIL ("cancelled", "cancelled");
	      }
	    if (!reconnect (j, &msg))
	      {
		continue;
	      }
	    ok = j->ssh.download (t.first, local, size, &j->cancel, &msg);
	    if (!ok)
	      {
		job_log (j, "transfer of " + t.second + " stopped (" + msg + "), attempt " + std::to_string (attempt));
		j->ssh.close ();
	      }
	  }
	if (!ok)
	  {
	    FAIL (j->cancel.load () ? "cancelled" : "transfer_failed", msg);
	  }
	struct stat st;
	string got;
	if (stat (local.c_str (), &st) != 0 || (uint64_t) st.st_size != size || !sha256_local (local, &got) || got != want)
	  {
	    FAIL ("transfer_failed", "size or checksum of " + t.second + " does not match the master");
	  }
	job_log (j, "received " + t.second + " (" + std::to_string (size) + " bytes, sha256 ok)");
      }
    return true;
  }

  bool st_keep_old_db (job_t *j, step_fail *f)
  {
    vector<string> paths;
    paths.push_back (j->s_db_line[1]);
    if (j->s_db_line[3] != j->s_db_line[1])
      {
	paths.push_back (j->s_db_line[3]);
      }
    DIR *d = opendir (j->repl_home.c_str ());
    if (d != NULL)
      {
	struct dirent *e;
	string prefix = j->db + "_";
	while ((e = readdir (d)) != NULL)
	  {
	    string n = e->d_name;
	    if (n.compare (0, prefix.size (), prefix) == 0 && n.find (".ha_make_slave_") == string::npos
		&& is_dir (j->repl_home + "/" + n))
	      {
		paths.push_back (j->repl_home + "/" + n);
	      }
	  }
	closedir (d);
      }
    for (const string &p : paths)
      {
	if (!is_dir (p) || !dir_has_entries (p))
	  {
	    continue;
	  }
	string to = p + ".ha_make_slave_" + j->uuid;
	if (rename (p.c_str (), to.c_str ()) != 0)
	  {
	    FAIL ("command_failed", "cannot move " + p + ": " + strerror (errno));
	  }
	j->moved.push_back (std::make_pair (p, to));
	{
	  std::lock_guard<std::mutex> lg (j->mu);
	  j->kept.push_back (to);
	}
	save_state (j);
	job_log (j, "kept " + p + " as " + to);
      }
    if (j->moved.empty ())
      {
	job_log (j, "no old database files on this server");
      }
    if (!mkdir_p (j->s_db_line[1]) || !mkdir_p (j->s_db_line[3]))
      {
	FAIL ("command_failed", "cannot create the database folders");
      }
    return true;
  }

  bool st_restore (job_t *j, step_fail *f)
  {
    string bkvinf = j->db + "_bkvinf";
    string data;
    bool ex = false;
    read_local_file (j->s_bk_dir + "/" + bkvinf, &data, &ex);
    if (!ex || !write_local_file (j->s_db_line[3] + "/" + bkvinf, data))
      {
	FAIL ("command_failed", "cannot place " + bkvinf);
      }
    j->restored_new = true;
    save_state (j);
    int rc = run_local (j, { bin (j, "cubrid"), "restoreslave", "-s", "master", "-m", j->master, "-B", j->s_bk_dir, j->db },
			NULL, 6 * 3600);
    if (rc != 0)
      {
	f->exit_code = rc;
	f->has_exit = rc > 0;
	FAIL (rc == -2 ? "cancelled" : "command_failed", "cubrid restoreslave failed (" + std::to_string (rc) + ")");
      }
    return true;
  }

  bool st_copylog (job_t *j, step_fail *f)
  {
    j->repl_dir = j->repl_home + "/" + j->db + "_" + j->master;
    save_state (j);
    remove_tree (j->repl_dir);
    if (!mkdir_p (j->repl_dir))
      {
	FAIL ("command_failed", "cannot create " + j->repl_dir);
      }
    int rc = run_local (j, { bin (j, "cub_admin"), "copylogdb", "-L", j->repl_dir, "-m", "async", "--start-page-id=-1",
			     j->db + "@" + j->master
			   }, NULL, 3600);
    if (rc != 0)
      {
	f->exit_code = rc;
	f->has_exit = rc > 0;
	FAIL (rc == -2 ? "cancelled" : "command_failed", "copylogdb failed (" + std::to_string (rc) + ")");
      }
    return true;
  }

  bool st_master_config (job_t *j, step_fail *f)
  {
    string msg;
    if (!reconnect (j, &msg))
      {
	FAIL ("unreachable", msg);
      }
    if (!edit_file (j, true, j->m_cubrid + "/conf/cubrid_ha.conf", [j] (const string &c)
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
    if (!edit_file (j, true, j->m_dbs + "/databases.txt", [j] (const string &c)
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
    j->ssh.read_file (j->m_cubrid + "/conf/cubrid_ha.conf", &ha, &ex);
    if (!contains (node_list_hosts (conf_get (ha, "[common]", "ha_node_list")), j->slave))
      {
	FAIL ("config_edit_failed", "the master's ha_node_list did not read back with " + j->slave);
      }
    string out;
    int rc = remote (j, "cubrid heartbeat reload 2>&1", &out, 300);
    j->reloaded = true;
    save_state (j);
    if (rc != 0 || out.find ("fail") != string::npos)
      {
	f->exit_code = rc;
	f->has_exit = rc > 0;
	FAIL ("command_failed", "cubrid heartbeat reload on the master failed: " + trim (out));
      }
    return true;
  }

  bool st_replication_start (job_t *j, step_fail *f)
  {
    string out;
    j->repl_started = true;
    save_state (j);
    int rc = remote (j, "cubrid heartbeat replication start " + shq (j->slave) + " 2>&1", &out, 300);
    if (rc != 0 || out.find ("fail") != string::npos)
      {
	f->exit_code = rc;
	f->has_exit = rc > 0;
	FAIL ("command_failed", "cubrid heartbeat replication start on the master failed: " + trim (out));
      }
    return true;
  }

  bool st_slave_ha_start (job_t *j, step_fail *f)
  {
    string out;
    j->ha_started = true;
    save_state (j);
    int rc = run_local (j, { bin (j, "cubrid"), "heartbeat", "start", j->db }, &out, 600);
    if (rc != 0 || out.find ("fail") != string::npos)
      {
	f->exit_code = rc;
	f->has_exit = rc > 0;
	FAIL (rc == -2 ? "cancelled" : "command_failed", "cubrid heartbeat start " + j->db + " failed");
      }
    return true;
  }

  bool st_verify (job_t *j, step_fail *f)
  {
    string last;
    for (int i = 0; i < 40; i++)
      {
	string out;
	run_local (j, { bin (j, "cubrid"), "changemode", j->db + "@localhost" }, &out, 60);
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
    FAIL ("verify_failed", "this server did not become standby: " + last);
  }

  bool st_cleanup (job_t *j, step_fail *f)
  {
    string msg;
    if (!j->m_bk_dir.empty () && reconnect (j, &msg))
      {
	remote (j, "rm -rf " + shq (j->m_bk_dir), NULL, 600);
      }
    if (!j->s_bk_dir.empty ())
      {
	remove_tree (j->s_bk_dir);
	job_log (j, "removed " + j->s_bk_dir);
      }
    return true;
  }

#undef FAIL

  /* undo one finished (or failed) step; true when it is back as before */
  bool rollback_step (job_t *j, const string &id)
  {
    string msg;
    bool ok = true;
    if (id == "slave_ha_start" && j->ha_started)
      {
	ok = run_local (j, { bin (j, "cubrid"), "heartbeat", "stop", j->db }, NULL, 600) == 0;
      }
    else if (id == "replication_start" && j->repl_started)
      {
	ok = reconnect (j, &msg) && remote (j, "cubrid heartbeat replication stop " + shq (j->slave) + " 2>&1", NULL, 300) == 0;
      }
    else if (id == "master_config" || id == "slave_config")
      {
	bool on_master = (id == "master_config");
	if (on_master && !reconnect (j, &msg))
	  {
	    return false;
	  }
	for (auto it = j->edits.rbegin (); it != j->edits.rend (); ++it)
	  {
	    if (it->remote != on_master)
	      {
		continue;
	      }
	    if (on_master)
	      {
		string cmd = it->existed ? "cp -f " + shq (it->bak) + " " + shq (it->path) : "rm -f " + shq (it->path);
		ok = remote (j, cmd, NULL, 60) == 0 && ok;
	      }
	    else if (it->existed)
	      {
		ok = rename (it->bak.c_str (), it->path.c_str ()) == 0 && ok;
	      }
	    else
	      {
		ok = unlink (it->path.c_str ()) == 0 && ok;
	      }
	    job_log (j, string (on_master ? "master: " : "") + "restored " + it->path);
	  }
	if (on_master && j->reloaded)
	  {
	    ok = remote (j, "cubrid heartbeat reload 2>&1", NULL, 300) == 0 && ok;
	  }
      }
    else if (id == "copylog" && !j->repl_dir.empty ())
      {
	ok = remove_tree (j->repl_dir);
      }
    else if (id == "restore" && j->restored_new)
      {
	ok = remove_tree (j->s_db_line[1]);
	if (j->s_db_line[3] != j->s_db_line[1])
	  {
	    ok = remove_tree (j->s_db_line[3]) && ok;
	  }
      }
    else if (id == "keep_old_db")
      {
	for (auto it = j->moved.rbegin (); it != j->moved.rend (); ++it)
	  {
	    remove_tree (it->first);
	    bool r = rename (it->second.c_str (), it->first.c_str ()) == 0;
	    job_log (j, string (r ? "moved back " : "could not move back ") + it->second);
	    ok = r && ok;
	  }
	if (ok)
	  {
	    std::lock_guard<std::mutex> lg (j->mu);
	    j->kept.clear ();
	  }
      }
    else if (id == "fetch_backup" && !j->s_bk_dir.empty ())
      {
	ok = remove_tree (j->s_bk_dir);
      }
    else if (id == "master_backup" && !j->m_bk_dir.empty ())
      {
	ok = reconnect (j, &msg) && remote (j, "rm -rf " + shq (j->m_bk_dir), NULL, 600) == 0;
      }
    return ok;
  }

  void finish (job_t *j, const string &status)
  {
    wipe (j->pw);
    j->ssh.close ();
    {
      std::lock_guard<std::mutex> lg (j->mu);
      j->status = status;
      j->phase = "done";
      j->ended = time (NULL);
    }
    save_state (j);
  }

  void run_job (std::shared_ptr<job_t> jp)
  {
    job_t *j = jp.get ();
    typedef bool (*step_fn) (job_t *, step_fail *);
    step_fn fns[] = { st_precheck, st_connect, st_master_check, st_slave_config, st_master_backup, st_fetch_backup,
		      st_keep_old_db, st_restore, st_copylog, st_master_config, st_replication_start, st_slave_ha_start,
		      st_verify, st_cleanup
		    };
    int failed_at = -1;
    step_fail f;
    f.exit_code = -1;
    f.has_exit = false;
    for (int i = 0; i < STEP_COUNT; i++)
      {
	if (j->cancel.load ())
	  {
	    f.error = "cancelled";
	    f.message = "cancelled before " + j->steps[i].id;
	    failed_at = i - 1;
	    break;
	  }
	string id = j->steps[i].id;
	if (!j->start_ha && (id == "slave_ha_start" || id == "verify"))
	  {
	    set_step (j, i, "skipped", "start_ha=no");
	    continue;
	  }
	set_step (j, i, "running");
	job_log (j, "step " + std::to_string (i + 1) + "/" + std::to_string (STEP_COUNT) + " " + id + ": "
		 + j->steps[i].title);
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
    save_state (j);
    job_log (j, "rolling back");
    /* the failed step may have changed things too, so it is undone as well */
    for (int i = failed_at; i >= 0; i--)
      {
	string id = j->steps[i].id;
	string state;
	{
	  std::lock_guard<std::mutex> lg (j->mu);
	  state = j->steps[i].state;
	}
	if (state != "done" && state != "failed")
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
  string local_rel;
  {
    FILE *p = popen ((env_or ("CUBRID", "") + "/bin/cubrid_rel 2>/dev/null").c_str (), "r");
    if (p != NULL)
      {
	char buf[512];
	while (fgets (buf, sizeof (buf), p) != NULL)
	  {
	    if (local_rel.empty () && strstr (buf, "CUBRID") != NULL)
	      {
		local_rel = buf;
	      }
	  }
	pclose (p);
      }
  }
  response["local_version"] = parse_version (local_rel);

  string result = "ok";
  if (!err.empty ())
    {
      result = err;
    }
  else if (want_fp.empty ())
    {
      result = "hostkey_unknown";
      msg = "confirm the master host key " + c.fp + " and call again with hostkey_sha256";
    }
  else if (want_fp != c.fp)
    {
      result = "hostkey_mismatch";
      msg = "the master host key is " + c.fp + ", not " + want_fp;
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
	      msg = "could not run commands on the master";
	    }
	  else if (rel.empty () || response["remote_cubrid"].asString ().empty ())
	    {
	      result = "cubrid_not_found";
	      msg = "CUBRID was not found for " + user + " on the master (login shell environment)";
	    }
	  else if (parse_version (rel) != response["local_version"].asString ())
	    {
	      result = "version_mismatch";
	      msg = "master CUBRID " + parse_version (rel) + ", this server " + response["local_version"].asString ();
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
ext_ha_make_slave (Json::Value &request, Json::Value &response)
{
  string pw = take_password (request);
  std::shared_ptr<job_t> j = std::make_shared<job_t> ();
  j->db = req_str (request, "db_name");
  j->master = req_str (request, "master_host");
  j->user = req_str (request, "os_user");
  j->fp = req_str (request, "hostkey_sha256");
  j->slave = req_str (request, "slave_host", local_hostname ());
  j->cubrid = env_or ("CUBRID", "");
  j->cubrid_dbs = env_or ("CUBRID_DATABASES", "");
  j->repl_home = req_str (request, "repl_log_home", j->cubrid_dbs);
  j->m_bk_root = req_str (request, "master_backup_path");
  j->s_bk_root = req_str (request, "slave_backup_path", j->cubrid_dbs + "/ha_make_slave_backup");
  string start_ha = req_str (request, "start_ha", "yes");
  j->start_ha = (start_ha != "no");

  string bad;
  if (!valid_name (j->db))
    {
      bad = "db_name";
    }
  else if (!valid_name (j->master))
    {
      bad = "master_host";
    }
  else if (!valid_name (j->user))
    {
      bad = "os_user";
    }
  else if (!valid_name (j->slave))
    {
      bad = "slave_host";
    }
  else if (j->fp.compare (0, 7, "SHA256:") != 0)
    {
      bad = "hostkey_sha256";
    }
  else if (!parse_port (req_str (request, "ssh_port", "22"), &j->port))
    {
      bad = "ssh_port";
    }
  else if (!valid_path (j->repl_home))
    {
      bad = "repl_log_home";
    }
  else if (!j->m_bk_root.empty () && !valid_path (j->m_bk_root))
    {
      bad = "master_backup_path";
    }
  else if (!valid_path (j->s_bk_root))
    {
      bad = "slave_backup_path";
    }
  else if (start_ha != "yes" && start_ha != "no")
    {
      bad = "start_ha";
    }
  else if (pw.empty ())
    {
      bad = "os_password";
    }
  if (!bad.empty ())
    {
      wipe (pw);
      return fail_resp (response, "invalid or missing " + bad);
    }
  if (request["replicas"].isArray () && request["replicas"].size () > 0)
    {
      wipe (pw);
      return fail_resp (response, "replicas are not supported in this POC");
    }
  if (j->master == j->slave)
    {
      wipe (pw);
      return fail_resp (response, "master_host and slave_host are the same");
    }

  std::lock_guard<std::mutex> lg (g_jobs_mu);
  if (g_job)
    {
      std::lock_guard<std::mutex> jl (g_job->mu);
      if (g_job->status == "running")
	{
	  wipe (pw);
	  return fail_resp (response, "another ha_make_slave job is running: " + g_job->uuid);
	}
    }
  j->uuid = new_uuid ();
  j->state_dir = state_root () + "/" + j->uuid;
  j->pw = pw;
  wipe (pw);
  j->started = time (NULL);
  for (int i = 0; i < STEP_COUNT; i++)
    {
      step_t s;
      s.id = STEP_DEFS[i][0];
      s.title = STEP_DEFS[i][1];
      s.state = "pending";
      j->steps.push_back (s);
    }
  if (!mkdir_p (j->state_dir))
    {
      wipe (j->pw);
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
      wipe (j->pw);
      g_job.reset ();
      return fail_resp (response, string ("cannot start the job: ") + e.what ());
    }
  LOG_INFO ("ha_make_slave: started job %s (db %s, master %s)", j->uuid.c_str (), j->db.c_str (), j->master.c_str ());
  response["uuid"] = j->uuid;
  response["job_status"] = "running";
  return build_server_header (response, ERR_NO_ERROR, "none");
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
	  if (k != "moved" && k != "edited_files")
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
  string uuid = req_str (request, "uuid");
  if (!valid_uuid (uuid))
    {
      return fail_resp (response, "invalid uuid");
    }
  string data;
  bool ex = false;
  read_local_file (state_root () + "/" + uuid + "/state.json", &data, &ex);
  Json::Value v;
  Json::Reader r;
  if (!ex || !r.parse (data, v))
    {
      return fail_resp (response, "no such job: " + uuid);
    }
  if (v["job_status"].asString () != "success")
    {
      return fail_resp (response, "only a successful job's kept database can be removed");
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
      if (remove_tree (path))
	{
	  removed.append (path);
	}
    }
  v["kept_db_paths"] = Json::Value (Json::arrayValue);
  Json::StyledWriter w;
  write_local_file (state_root () + "/" + uuid + "/state.json", w.write (v));
  response["removed"] = removed;
  return build_server_header (response, ERR_NO_ERROR, "none");
}

#endif /* !WINDOWS */
