/*
 * Copyright 2008 Search Solution Corporation
 * Copyright 2016 CUBRID Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *	http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

/*
 * cm_ha_add_nodes.cpp - the manager side of adding HA nodes: thin on purpose.
 *
 * The work is done by $CUBRID/share/cms/ha_add_nodes.sh. The manager
 *   - checks the shape of the request (names, paths, ports),
 *   - checks that the scripts are the files of this build (sha256) and that nobody else can write them,
 *   - takes the job lock and makes the job folder $CUBRID/tmp/ha_add_nodes/<uuid>,
 *   - logs in to every node with the OpenSSH client of this server: one ControlMaster per node. The password
 *     goes to ssh through a FIFO that ha_askpass.sh reads, once, and is not kept anywhere after the login,
 *   - starts the script with arguments only (no password), detached from the manager. The script reaches the
 *     nodes through the ControlMasters, with the ssh settings file the manager wrote for this run,
 *   - reads the script's files for the status, and sends it SIGTERM to cancel.
 * Passwords are never written to a file, an argument or the environment; the script never sees them.
 */

#if !defined(WINDOWS)

#include "cm_log.h"
#include "cm_dep.h"
#include "cm_server_util.h"
#include "cm_server_extend_interface.h"
#include "cm_ha_add_nodes.h"

#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <dirent.h>
#include <errno.h>
#include <pty.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <thread>

#include <netdb.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <openssl/evp.h>

#include "cm_ha_script_hash.h"

using std::string;
using std::vector;

extern char **environ;

namespace
{
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

  string env_or (const char *name, const string &def)
  {
    const char *v = getenv (name);
    return (v != NULL && *v != '\0') ? string (v) : def;
  }

  /* letters, digits, '.', '_', '-' and starting with a letter or a digit */
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

  bool valid_fp (const string &s)
  {
    if (s.compare (0, 7, "SHA256:") != 0 || s.size () > 80)
      {
	return false;
      }
    for (size_t i = 7; i < s.size (); i++)
      {
	char c = s[i];
	if (!isalnum ((unsigned char) c) && c != '+' && c != '/')
	  {
	    return false;
	  }
      }
    return true;
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

  bool valid_port (const string &s)
  {
    if (s.empty () || s.size () > 5 || s.find_first_not_of ("0123456789") != string::npos)
      {
	return false;
      }
    int p = atoi (s.c_str ());
    return p >= 1 && p <= 65535;
  }

  bool read_file (const string &path, string *data)
  {
    std::ifstream in (path.c_str (), std::ios::binary);
    if (!in)
      {
	return false;
      }
    std::ostringstream ss;
    ss << in.rdbuf ();
    *data = ss.str ();
    return true;
  }

  vector<string> split (const string &s, char sep)
  {
    vector<string> out;
    string cur;
    for (char c : s)
      {
	if (c == sep)
	  {
	    out.push_back (cur);
	    cur.clear ();
	  }
	else
	  {
	    cur += c;
	  }
      }
    out.push_back (cur);
    return out;
  }

  string trim (const string &s)
  {
    size_t b = s.find_first_not_of (" \t\r\n");
    if (b == string::npos)
      {
	return "";
      }
    return s.substr (b, s.find_last_not_of (" \t\r\n") - b + 1);
  }

  int fail_resp (Json::Value &response, const string &note)
  {
    return build_server_header (response, ERR_WITH_MSG, note.c_str ());
  }

  string cubrid_dir ()
  {
    return env_or ("CUBRID", "");
  }

  string jobs_root ()
  {
    return cubrid_dir () + "/tmp/ha_add_nodes";
  }

  string script_path ()
  {
    return cubrid_dir () + "/share/cms/ha_add_nodes.sh";
  }

  string watch_path ()
  {
    return cubrid_dir () + "/share/cms/ha_watch.sh";
  }

  string node_script_path ()
  {
    return cubrid_dir () + "/share/cms/ha_node.sh";
  }

  string askpass_path ()
  {
    return cubrid_dir () + "/share/cms/ha_askpass.sh";
  }

  string to_hex (const unsigned char *d, size_t n)
  {
    static const char *x = "0123456789abcdef";
    string out;
    for (size_t i = 0; i < n; i++)
      {
	out += x[d[i] >> 4];
	out += x[d[i] & 15];
      }
    return out;
  }

  /* sha256 of everything a descriptor has (read from the start with pread: the offset is left alone) */
  bool sha256_fd (int fd, string *hex)
  {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new ();
    if (ctx == NULL || EVP_DigestInit_ex (ctx, EVP_sha256 (), NULL) != 1)
      {
	EVP_MD_CTX_free (ctx);
	return false;
      }
    unsigned char buf[8192], md[EVP_MAX_MD_SIZE];
    off_t off = 0;
    ssize_t n;
    while ((n = pread (fd, buf, sizeof (buf), off)) > 0)
      {
	EVP_DigestUpdate (ctx, buf, (size_t) n);
	off += n;
      }
    unsigned int len = 0;
    bool ok = n == 0 && EVP_DigestFinal_ex (ctx, md, &len) == 1;
    EVP_MD_CTX_free (ctx);
    if (ok)
      {
	*hex = to_hex (md, len);
      }
    return ok;
  }

  /* "SHA256:<base64 without padding>" of a public key given in base64, as ssh-keygen -l prints it */
  string key_fingerprint (const string &b64)
  {
    if (b64.empty () || b64.size () % 4 != 0 || b64.size () > 16384)
      {
	return "";
      }
    vector<unsigned char> blob (b64.size ());
    int n = EVP_DecodeBlock (blob.data (), (const unsigned char *) b64.data (), (int) b64.size ());
    if (n <= 0)
      {
	return "";
      }
    for (size_t i = b64.size (); i > 0 && b64[i - 1] == '='; i--)
      {
	n--;
      }
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (EVP_Digest (blob.data (), (size_t) n, md, &len, EVP_sha256 (), NULL) != 1)
      {
	return "";
      }
    unsigned char enc[EVP_MAX_MD_SIZE * 2];
    int e = EVP_EncodeBlock (enc, md, (int) len);
    string out ((const char *) enc, (size_t) e);
    while (!out.empty () && out[out.size () - 1] == '=')
      {
	out.erase (out.size () - 1);
      }
    return "SHA256:" + out;
  }

  /*
   * A file the manager runs, and every folder above it up to /, has to belong to this account or to root and
   * must not be writable by the group or by others: whoever can write one of them can change what runs.
   */
  bool safe_owner_mode (const struct stat &st)
  {
    return (st.st_mode & (S_IWGRP | S_IWOTH)) == 0 && (st.st_uid == getuid () || st.st_uid == 0);
  }

  bool safe_parents (const string &path, string *why)
  {
    string cur = path;
    for (;;)
      {
	size_t p = cur.rfind ('/');
	if (p == string::npos)
	  {
	    return true;
	  }
	cur = p == 0 ? "/" : cur.substr (0, p);
	struct stat st;
	if (stat (cur.c_str (), &st) != 0 || !S_ISDIR (st.st_mode) || !safe_owner_mode (st))
	  {
	    *why = cur + " is writable by others or owned by another account";
	    return false;
	  }
	if (cur == "/")
	  {
	    return true;
	  }
      }
  }

  /*
   * One of the scripts of this build: "" when it is the built-in file and safe, else the error code
   * (script_tampered, script_unsafe) with *why. keep_fd: the descriptor the file was hashed through is
   * handed back open, so that what was checked is what runs.
   */
  string check_script (const string &path, const char *want, int *keep_fd, string *why)
  {
    int fd = open (path.c_str (), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    string hex;
    string code;
    if (fd < 0 || fstat (fd, &st) != 0 || !S_ISREG (st.st_mode))
      {
	code = "script_tampered";
	*why = path + " is missing";
      }
    else if (!safe_owner_mode (st))
      {
	code = "script_unsafe";
	*why = path + " is writable by others or owned by another account";
      }
    else if (!safe_parents (path, why))
      {
	code = "script_unsafe";
      }
    else if (want[0] == '\0')
      {
	code = "script_tampered";
	*why = path + " was not part of the build of this manager";
      }
    else if (!sha256_fd (fd, &hex) || hex != want)
      {
	code = "script_tampered";
	*why = path + " is not the file this manager was built with";
      }
    if (fd >= 0 && (keep_fd == NULL || !code.empty ()))
      {
	close (fd);
	fd = -1;
      }
    if (keep_fd != NULL)
      {
	*keep_fd = fd;
      }
    return code;
  }

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

  string proc_start (pid_t pid)
  {
    string data;
    if (pid <= 0 || !read_file ("/proc/" + std::to_string ((int) pid) + "/stat", &data))
      {
	return "";
      }
    size_t p = data.rfind (')');
    if (p == string::npos)
      {
	return "";
      }
    vector<string> f;
    std::istringstream in (data.substr (p + 2));
    string t;
    while (in >> t)
      {
	f.push_back (t);
      }
    return f.size () > 19 ? f[19] : ""; /* field 22 of stat; f[0] is field 3 */
  }

  /* ------------------------------------------------------------ the job lock (shared with the script) */

  /* "starting <uuid> <epoch>" or "running <uuid> <pid> <pid start>"; free when the holder is gone */
  std::map<string, string> key_values (const string &text);
  string state_of (const string &dir, const string &name);
  bool valid_uuid (const string &u);
  string jobs_root ();

  /*
   * running: held while the script lives, and after it died without finishing (interrupted) until that job is rolled
   * back (a plan or cleanup changes nothing and does not hold it). blocked: a job that stopped with rollback_blocked,
   * held until it is rolled back. The job's own rollback (me) passes.
   */
  bool lock_in_use (const string &content, const string &me, string *why)
  {
    std::istringstream in (content);
    string what, uuid, a, b;
    in >> what >> uuid >> a >> b;
    if (!uuid.empty () && uuid == me)
      {
	return false;
      }
    if (what == "running")
      {
	if (proc_start ((pid_t) atoi (a.c_str ())) == b && !b.empty ())
	  {
	    *why = "job " + uuid + " is running";
	    return true;
	  }
	if (!valid_uuid (uuid))
	  {
	    return false;
	  }
	std::map<string, string> job = key_values (state_of (jobs_root () + "/" + uuid, "job"));
	if (job["status"] == "running" && job["action"] != "plan" && job["action"] != "cleanup")
	  {
	    *why = "job " + uuid + " stopped without finishing (interrupted); roll it back first";
	    return true;
	  }
	return false;
      }
    if (what == "blocked")
      {
	*why = "job " + uuid + " stopped with rollback_blocked; roll it back first";
	return true;
      }
    if (what == "starting" && time (NULL) - atol (a.c_str ()) < 60)
      {
	*why = "a job is starting";
	return true;
      }
    return false;
  }

  bool mkdir_p (const string &path)
  {
    string cur;
    for (const string &part : split (path, '/'))
      {
	if (part.empty ())
	  {
	    continue;
	  }
	cur += "/" + part;
	if (mkdir (cur.c_str (), 0755) != 0 && errno != EEXIST)
	  {
	    return false;
	  }
      }
    return true;
  }

  /*
   * reserve the lock for a new job; the script turns it into "running" when it starts.
   * 0: reserved, 1: another job holds it (*holder), -1: the lock file cannot be used (*holder)
   */
  int lock_reserve (const string &uuid, string *holder, string *prev)
  {
    string path = jobs_root () + "/lock";
    int fd = (mkdir_p (jobs_root ())) ? open (path.c_str (), O_RDWR | O_CREAT | O_CLOEXEC, 0600) : -1;
    if (fd < 0)
      {
	*holder = "cannot open " + path + ": " + strerror (errno);
	return -1;
      }
    flock (fd, LOCK_EX);
    string content, why;
    read_file (path, &content);
    bool ok = !lock_in_use (content, uuid, &why);
    if (ok)
      {
	*prev = content;	/* this job's own blocked/interrupted mark comes back if it cannot start */
	string line = "starting " + uuid + " " + std::to_string ((long) time (NULL)) + "\n";
	ftruncate (fd, 0);
	pwrite (fd, line.data (), line.size (), 0);
      }
    else
      {
	*holder = why;
      }
    flock (fd, LOCK_UN);
    close (fd);
    return ok ? 0 : 1;
  }

  void lock_release_if (const string &uuid, const string &prev)
  {
    string path = jobs_root () + "/lock";
    int fd = open (path.c_str (), O_RDWR | O_CLOEXEC);
    if (fd < 0)
      {
	return;
      }
    flock (fd, LOCK_EX);
    string content;
    read_file (path, &content);
    if (content.compare (0, 9 + uuid.size (), "starting " + uuid) == 0)
      {
	ftruncate (fd, 0);
	std::istringstream was (prev);
	string w, u;
	was >> w >> u;
	if (u == uuid && (w == "blocked" || w == "running"))
	  {
	    pwrite (fd, prev.data (), prev.size (), 0);
	  }
      }
    flock (fd, LOCK_UN);
    close (fd);
  }

  /* a long start (the logins) must not let the reservation run out before the script takes the lock over */
  void lock_touch (const string &uuid)
  {
    string path = jobs_root () + "/lock";
    int fd = open (path.c_str (), O_RDWR | O_CLOEXEC);
    if (fd < 0)
      {
	return;
      }
    flock (fd, LOCK_EX);
    string content;
    read_file (path, &content);
    if (content.compare (0, 9 + uuid.size (), "starting " + uuid) == 0)
      {
	string line = "starting " + uuid + " " + std::to_string ((long) time (NULL)) + "\n";
	ftruncate (fd, 0);
	pwrite (fd, line.data (), line.size (), 0);
      }
    flock (fd, LOCK_UN);
    close (fd);
  }

  /* ------------------------------------------------------------ starting programs */

  long long now_ms ()
  {
    struct timespec ts;
    clock_gettime (CLOCK_MONOTONIC, &ts);
    return (long long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
  }

  string random_hex (size_t bytes)
  {
    vector<unsigned char> r (bytes);
    FILE *fp = fopen ("/dev/urandom", "rb");
    if (fp == NULL || fread (r.data (), 1, bytes, fp) != bytes)
      {
	for (size_t i = 0; i < bytes; i++)
	  {
	    r[i] = (unsigned char) (rand () & 0xff);
	  }
      }
    if (fp != NULL)
      {
	fclose (fp);
      }
    return to_hex (r.data (), bytes);
  }

  struct spawn_opts
  {
    int in_fd, out_fd, err_fd;	/* -1: /dev/null */
    int keep_fd;		/* -1, or a descriptor the program gets as 3 */
    bool detach_twice;		/* the program belongs to init, so the manager never has to reap it */
    const vector<string> *env;	/* NULL: the manager's environment */
    spawn_opts () : in_fd (-1), out_fd (-1), err_fd (-1), keep_fd (-1), detach_twice (false), env (NULL)
    {
    }
  };

  /*
   * fork + exec in a new session (no controlling terminal), with the given stdin/stdout/stderr, keep_fd as 3
   * and every other descriptor closed. Only async-signal-safe calls after fork.
   */
  pid_t spawn (const vector<string> &argv, const spawn_opts &o)
  {
    vector<char *> av, ev;
    for (const string &a : argv)
      {
	av.push_back (const_cast<char *> (a.c_str ()));
      }
    av.push_back (NULL);
    if (o.env != NULL)
      {
	for (const string &e : *o.env)
	  {
	    ev.push_back (const_cast<char *> (e.c_str ()));
	  }
	ev.push_back (NULL);
      }
    char **envp = o.env != NULL ? ev.data () : environ;
    long maxfd = sysconf (_SC_OPEN_MAX);
    if (maxfd < 0 || maxfd > 65536)
      {
	maxfd = 65536;
      }
    int devnull = open ("/dev/null", O_RDWR | O_CLOEXEC);
    pid_t pid = fork ();
    if (pid != 0)
      {
	if (devnull >= 0)
	  {
	    close (devnull);
	  }
	return pid;
      }
    /* child */
    setsid ();
    if (o.detach_twice && fork () != 0)
      {
	_exit (0);
      }
    /* the sources go above the numbers they are copied to, so no copy overwrites a source */
    int src[4] = { o.in_fd, o.out_fd, o.err_fd, o.keep_fd };
    int dn = fcntl (devnull, F_DUPFD, 10);
    for (int i = 0; i < 4; i++)
      {
	if (src[i] >= 0)
	  {
	    src[i] = fcntl (src[i], F_DUPFD, 10);
	  }
      }
    for (int i = 0; i < 3; i++)
      {
	dup2 (src[i] >= 0 ? src[i] : dn, i);
      }
    if (src[3] >= 0)
      {
	dup2 (src[3], 3);
      }
    for (long fd = src[3] >= 0 ? 4 : 3; fd < maxfd; fd++)
      {
	close ((int) fd);
      }
    /* nothing of the manager's signal settings goes along: an ignored signal would stay ignored in the program
       and in what it starts (a script could not even trap it) */
    struct sigaction sa;
    memset (&sa, 0, sizeof (sa));
    sa.sa_handler = SIG_DFL;
    for (int sig = 1; sig < NSIG; sig++)
      {
	sigaction (sig, &sa, NULL);
      }
    sigset_t none;
    sigemptyset (&none);
    sigprocmask (SIG_SETMASK, &none, NULL);
    execve (av[0], av.data (), envp);
    _exit (127);
  }

  void close_fd (int *fd)
  {
    if (*fd >= 0)
      {
	close (*fd);
	*fd = -1;
      }
  }

  /*
   * runs a program to its end: stdin from *in (or /dev/null), stdout and stderr collected (either may be NULL).
   * returns the exit status, -1 when it could not run or was killed, -2 when it took longer than the limit
   * (the program and what it started in its session are then killed).
   */
  int run_cmd (const vector<string> &argv, const vector<string> *env, const string *in, string *out, string *err,
	       double limit_sec)
  {
    int ip[2] = { -1, -1 }, op[2] = { -1, -1 }, ep[2] = { -1, -1 };
    if ((in != NULL && pipe2 (ip, O_CLOEXEC) != 0) || pipe2 (op, O_CLOEXEC) != 0 || pipe2 (ep, O_CLOEXEC) != 0)
      {
	close_fd (&ip[0]);
	close_fd (&ip[1]);
	close_fd (&op[0]);
	close_fd (&op[1]);
	return -1;
      }
    spawn_opts o;
    o.in_fd = ip[0];
    o.out_fd = op[1];
    o.err_fd = ep[1];
    o.env = env;
    pid_t pid = spawn (argv, o);
    close_fd (&ip[0]);
    close_fd (&op[1]);
    close_fd (&ep[1]);
    if (pid < 0)
      {
	close_fd (&ip[1]);
	close_fd (&op[0]);
	close_fd (&ep[0]);
	return -1;
      }
    if (ip[1] >= 0)
      {
	fcntl (ip[1], F_SETFL, fcntl (ip[1], F_GETFL) | O_NONBLOCK);
      }
    int rfd[2] = { op[0], ep[0] };
    string *dst[2] = { out, err };
    size_t sent = 0;
    bool exited = false, timed_out = false;
    int st = 0;
    long long end = now_ms () + (long long) (limit_sec * 1000), drain_end = 0;
    char buf[4096];
    for (;;)
      {
	long long now = now_ms ();
	if (!exited && waitpid (pid, &st, WNOHANG) == pid)
	  {
	    exited = true;
	    drain_end = now + 300;	/* something it started may still hold the pipes: do not wait for that */
	  }
	if (exited && ((rfd[0] < 0 && rfd[1] < 0) || now >= drain_end))
	  {
	    break;
	  }
	if (!exited && now >= end)
	  {
	    kill (-pid, SIGKILL);
	    kill (pid, SIGKILL);
	    waitpid (pid, &st, 0);
	    timed_out = true;
	    break;
	  }
	if (ip[1] >= 0 && sent >= in->size ())
	  {
	    close_fd (&ip[1]);	/* end of input */
	  }
	struct pollfd pf[3];
	int which[3], n = 0;
	for (int i = 0; i < 2; i++)
	  {
	    if (rfd[i] >= 0)
	      {
		pf[n].fd = rfd[i];
		pf[n].events = POLLIN;
		which[n++] = i;
	      }
	  }
	if (ip[1] >= 0)
	  {
	    pf[n].fd = ip[1];
	    pf[n].events = POLLOUT;
	    which[n++] = 2;
	  }
	if (poll (pf, (nfds_t) n, 50) <= 0)
	  {
	    continue;
	  }
	for (int k = 0; k < n; k++)
	  {
	    if (pf[k].revents == 0)
	      {
		continue;
	      }
	    if (which[k] == 2)
	      {
		ssize_t w = write (ip[1], in->data () + sent, in->size () - sent);
		if (w > 0)
		  {
		    sent += (size_t) w;
		  }
		else if (errno != EAGAIN && errno != EINTR)
		  {
		    close_fd (&ip[1]);
		  }
		continue;
	      }
	    int i = which[k];
	    ssize_t r = read (rfd[i], buf, sizeof (buf));
	    if (r > 0)
	      {
		if (dst[i] != NULL && dst[i]->size () < 1024 * 1024)
		  {
		    dst[i]->append (buf, (size_t) r);
		  }
	      }
	    else if (r == 0 || (errno != EAGAIN && errno != EINTR))
	      {
		close_fd (&rfd[i]);
	      }
	  }
      }
    close_fd (&ip[1]);
    close_fd (&rfd[0]);
    close_fd (&rfd[1]);
    if (timed_out)
      {
	return -2;
      }
    return WIFEXITED (st) ? WEXITSTATUS (st) : -1;
  }

  struct node_in
  {
    string role, host, port, user, fp, pw;
  };

  /* new_nodes / existing_nodes of a request; the passwords are taken out of the request */
  bool read_nodes (Json::Value &request, const char *key, bool with_role, vector<node_in> *out, string *bad)
  {
    Json::Value &arr = request[key];
    if (arr.isNull ())
      {
	return true;
      }
    if (!arr.isArray ())
      {
	*bad = string (key) + " (must be an array)";
	return false;
      }
    for (Json::UInt i = 0; i < arr.size (); i++)
      {
	Json::Value &e = arr[i];
	string where = string (key) + "[" + std::to_string (i) + "].";
	if (!e.isObject ())
	  {
	    *bad = where;
	    return false;
	  }
	node_in n;
	if (e["os_password"].isString ())
	  {
	    n.pw = e["os_password"].asString ();
	    e["os_password"] = "";
	  }
	n.role = with_role ? req_str (e, "role") : "";
	n.host = req_str (e, "host");
	n.port = req_str (e, "ssh_port", "22");
	n.user = req_str (e, "os_user");
	n.fp = req_str (e, "hostkey_sha256");
	bool ok = true;
	if (with_role && n.role != "slave" && n.role != "replica")
	  {
	    *bad = where + "role";
	    ok = false;
	  }
	else if (!valid_name (n.host))
	  {
	    *bad = where + "host";
	    ok = false;
	  }
	else if (!valid_port (n.port))
	  {
	    *bad = where + "ssh_port";
	    ok = false;
	  }
	else if (!valid_name (n.user))
	  {
	    *bad = where + "os_user";
	    ok = false;
	  }
	else if (!valid_fp (n.fp))
	  {
	    *bad = where + "hostkey_sha256";
	    ok = false;
	  }
	else if (n.pw.empty () || n.pw.find ('\n') != string::npos || n.pw.find ('\0') != string::npos)
	  {
	    *bad = where + "os_password";
	    ok = false;
	  }
	if (!ok)
	  {
	    wipe (n.pw);
	    return false;
	  }
	out->push_back (n);
      }
    return true;
  }

  void wipe_nodes (vector<node_in> &v)
  {
    for (node_in &n : v)
      {
	wipe (n.pw);
      }
  }

  string state_of (const string &dir, const string &file)
  {
    string data;
    read_file (dir + "/" + file, &data);
    return data;
  }

  std::map<string, string> key_values (const string &data)
  {
    std::map<string, string> kv;
    std::istringstream in (data);
    string line;
    while (std::getline (in, line))
      {
	size_t eq = line.find ('=');
	if (eq != string::npos)
	  {
	    kv[line.substr (0, eq)] = line.substr (eq + 1);
	  }
      }
    return kv;
  }

  /*
   * Tables without a primary key in one database (HA does not replicate them), read with the account the screen
   * logged in with. csql asks for a password only on a terminal, so it runs on a pseudo terminal and the password
   * is typed at its prompt: it never goes into an argument, the environment or a file.
   * returns "ok", "partial" (not dba: other users' tables are not visible) or "failed"; *tables: owner.name list
   */
  string pk_check (const string &db, const string &user, string &password, string *tables, string *msg)
  {
    const char *q = "select 'PKCHK|' || c.owner_name || '.' || c.class_name from db_class c where c.is_system_class = 'NO'"
		    " and c.class_type = 'CLASS' and not exists (select 1 from db_index i where i.class_name = c.class_name"
		    " and i.is_primary_key = 'YES');";
    string csql = cubrid_dir () + "/bin/csql";
    string target = db + "@localhost";
    int master = -1;
    pid_t pid = forkpty (&master, NULL, NULL, NULL);
    if (pid < 0)
      {
	*msg = "cannot open a pseudo terminal";
	return "failed";
      }
    if (pid == 0)
      {
	long maxfd = sysconf (_SC_OPEN_MAX);
	for (long fd = 3; fd < (maxfd > 0 && maxfd < 65536 ? maxfd : 65536); fd++)
	  {
	    close ((int) fd);
	  }
	execl (csql.c_str (), csql.c_str (), "-u", user.c_str (), target.c_str (), (char *) NULL);
	_exit (127);
      }
    string out;
    bool sent_pw = false, sent_q = false;
    time_t end = time (NULL) + 30;
    char buf[4096];
    while (time (NULL) < end)
      {
	if (!sent_pw && out.find ("assword") != string::npos)
	  {
	    string line = password + "\n";
	    ssize_t w = write (master, line.data (), line.size ());
	    (void) w;
	    wipe (line);
	    sent_pw = true;
	  }
	if (sent_pw && out.find ("assword", out.find ("assword") + 7) != string::npos)
	  {
	    break;		/* asked again: the password was wrong */
	  }
	if (!sent_q && out.find ("csql>") != string::npos)
	  {
	    string line = string (q) + "\n;exit\n";
	    ssize_t w = write (master, line.data (), line.size ());
	    (void) w;
	    sent_q = true;
	  }
	struct pollfd pf = { master, POLLIN, 0 };
	if (poll (&pf, 1, 500) <= 0)
	  {
	    continue;
	  }
	ssize_t n = read (master, buf, sizeof (buf));
	if (n <= 0)
	  {
	    break;		/* csql ended */
	  }
	out.append (buf, (size_t) n);
      }
    wipe (password);
    kill (pid, SIGKILL);
    close (master);
    waitpid (pid, NULL, 0);
    if (!sent_q)
      {
	*msg = sent_pw ? "login failed" : "csql did not start";
	return "failed";
      }
    /* result rows look like  'PKCHK|PUBLIC.t1' ; the echoed query has 'PKCHK|' with nothing after the bar */
    size_t pos = 0;
    while ((pos = out.find ("'PKCHK|", pos)) != string::npos)
      {
	size_t b = pos + 7, e = out.find ('\'', b);
	if (e != string::npos && e > b)
	  {
	    *tables += (tables->empty () ? "" : " ") + out.substr (b, e - b);
	  }
	pos = b;
      }
    if (out.find ("ERROR") != string::npos && tables->empty ())
      {
	*msg = "the query failed";
	return "failed";
      }
    string u = user;
    for (char &c : u)
      {
	c = (char) toupper ((unsigned char) c);
      }
    return u == "DBA" ? "ok" : "partial";
  }

  /* ------------------------------------------------------------ ssh: the OpenSSH client of this server */

  struct ssh_tools
  {
    string ssh, keyscan;
  };

  string find_program (const char *name)
  {
    const char *dirs[] = { "/usr/bin/", "/bin/", "/usr/local/bin/", "/usr/sbin/" };
    for (const char *d : dirs)
      {
	string p = string (d) + name;
	if (access (p.c_str (), X_OK) == 0)
	  {
	    return p;
	  }
      }
    return "";
  }

  /* *missing: what this server lacks ("ssh ssh-keyscan setsid"); the script needs setsid */
  bool find_ssh_tools (ssh_tools *t, string *missing)
  {
    t->ssh = find_program ("ssh");
    t->keyscan = find_program ("ssh-keyscan");
    const char *names[] = { "ssh", "ssh-keyscan", "setsid" };
    for (const char *n : names)
      {
	if (find_program (n).empty ())
	  {
	    *missing += (missing->empty () ? "" : " ") + string (n);
	  }
      }
    return missing->empty ();
  }

  /* what the user can do about an error, one line */
  string action_of (const string &code)
  {
    static const char *table[][2] =
    {
      { "local_ssh_missing", "Install the OpenSSH client (openssh-clients) on this server." },
      { "name_unresolved", "Register the node's name in /etc/hosts or DNS on this server." },
      { "port_refused", "Check that sshd runs on the node and that the port number is right." },
      { "port_timeout", "Check the firewall and the network path to the node." },
      { "hostkey_mismatch", "Find out why the node's host key changed, then confirm the fingerprint again." },
      { "no_password_auth", "Allow PasswordAuthentication or KbdInteractiveAuthentication in the node's sshd." },
      { "auth_failed", "Check the account and the password. If the account is locked, ask its administrator to unlock it." },
      { "remote_shell_failed", "Check the account's login shell on the node." },
      { "cubrid_not_found", "Enter the account that CUBRID is installed for." },
      { "node_script_missing", "Check the node's CUBRID package: it has to include the manager server." },
      { "node_script_version_mismatch", "Install the same CUBRID package on the node." },
      { "node_script_tampered", "Put the package's file back on the node and find out how it changed." },
      { "node_script_unsafe", "Restore the owner and the permissions of the installed files on the node." },
      { "version_mismatch", "Bring every node to the same CUBRID version." },
      { "controlpath_too_long", "Install CUBRID under a shorter path." },
      { "script_tampered", "Put the package's file back on this server and find out how it changed." },
      { "script_unsafe", "Restore the owner and the permissions of the installed files on this server." },
      { "login_timeout", "Find out why logins to the node are slow (sshd UseDNS, PAM), then try again." },
      { "ssh_failed", "See the message and the node's sshd log." },
    };
    for (auto &row : table)
      {
	if (code == row[0])
	  {
	    return row[1];
	  }
      }
    return "";
  }

  /* the manager's environment for ssh: nothing that changes how ssh asks or what it reaches, plus `extra` */
  vector<string> ssh_env (const vector<string> &extra)
  {
    const char *drop[] = { "SSH_ASKPASS=", "SSH_ASKPASS_REQUIRE=", "SSH_AUTH_SOCK=", "DISPLAY=", "HA_ASKPASS_FIFO=",
      "LC_ALL=", "LANG=", "LANGUAGE="
    };
    vector<string> env;
    for (char **e = environ; e != NULL && *e != NULL; e++)
      {
	bool skip = false;
	for (const char *d : drop)
	  {
	    skip = skip || strncmp (*e, d, strlen (d)) == 0;
	  }
	if (!skip)
	  {
	    env.push_back (*e);
	  }
      }
    env.push_back ("LC_ALL=C");
    env.insert (env.end (), extra.begin (), extra.end ());
    return env;
  }

  string last_line (const string &text)
  {
    string last;
    for (const string &l : split (text, '\n'))
      {
	if (!trim (l).empty ())
	  {
	    last = trim (l);
	  }
      }
    for (char &c : last)
      {
	if (c == '\t' || (unsigned char) c < 0x20)
	  {
	    c = ' ';
	  }
      }
    return last.size () > 300 ? last.substr (0, 300) : last;
  }

  /*
   * The settings every ssh call of a run starts from (-F): the account's ~/.ssh/config is not read, the system's
   * settings (crypto policies) still are, after ours - the first value of a setting wins. Every way to log in is
   * off: a command that finds its ControlMaster gone ends with 255 instead of trying a login of its own.
   */
  string common_ssh_config ()
  {
    return "Host *\n"
	   "  ControlMaster no\n"
	   "  BatchMode yes\n"
	   "  PubkeyAuthentication no\n"
	   "  GSSAPIAuthentication no\n"
	   "  HostbasedAuthentication no\n"
	   "  PasswordAuthentication no\n"
	   "  KbdInteractiveAuthentication no\n"
	   "  ChallengeResponseAuthentication no\n"
	   "  ProxyJump none\n"
	   "  StrictHostKeyChecking yes\n"
	   "  GlobalKnownHostsFile /dev/null\n"
	   "  UpdateHostKeys no\n"
	   "  CheckHostIP no\n"
	   "  ConnectTimeout 10\n"
	   "  ServerAliveInterval 15\n"
	   "  ServerAliveCountMax 12\n"
	   "  ForwardAgent no\n"
	   "  ForwardX11 no\n"
	   "  ClearAllForwardings yes\n"
	   "  EscapeChar none\n"
	   "Include /etc/ssh/ssh_config\n";
  }

  bool write_file (const string &path, const string &data, mode_t mode)
  {
    string tmp = path + ".tmp";
    int fd = open (tmp.c_str (), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0)
      {
	return false;
      }
    bool ok = write (fd, data.data (), data.size ()) == (ssize_t) data.size ();
    close (fd);
    return ok && rename (tmp.c_str (), path.c_str ()) == 0;
  }

  struct host_key
  {
    string type, b64, fp;
  };

  struct node_result
  {
    string host, port, user;
    string code, message;	/* code: "ok" or an error code */
    vector<host_key> keys;	/* what the node offers */
    host_key key;		/* the confirmed one */
    string alg, kh, ctl, auth, log;
    bool master;		/* a ControlMaster was started */
    node_result () : master (false)
    {
    }
  };

  void set_error (node_result *r, const string &code, const string &message)
  {
    r->code = code;
    r->message = r->host + ": " + message;
  }

  /* 0: connected, 1: refused, 2: no answer in time (or no route) */
  int tcp_probe (struct addrinfo *ai, int timeout_ms)
  {
    int worst = 2;
    for (struct addrinfo *a = ai; a != NULL; a = a->ai_next)
      {
	int s = socket (a->ai_family, a->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, a->ai_protocol);
	if (s < 0)
	  {
	    continue;
	  }
	int rc = connect (s, a->ai_addr, a->ai_addrlen), e = errno;
	if (rc != 0 && e == EINPROGRESS)
	  {
	    struct pollfd pf = { s, POLLOUT, 0 };
	    e = ETIMEDOUT;
	    if (poll (&pf, 1, timeout_ms) > 0)
	      {
		socklen_t len = sizeof (e);
		getsockopt (s, SOL_SOCKET, SO_ERROR, &e, &len);
		rc = e == 0 ? 0 : -1;
	      }
	  }
	close (s);
	if (rc == 0)
	  {
	    return 0;
	  }
	if (e == ECONNREFUSED)
	  {
	    worst = 1;
	  }
      }
    return worst;
  }

  /*
   * The host key algorithms this server's ssh settings allow (the system's crypto policy comes in through the
   * settings file). Empty when ssh does not tell: then nothing is left out.
   */
  vector<string> allowed_hostkey_algs (const ssh_tools &t, const string &cfg)
  {
    string out;
    vector<string> env = ssh_env ({ });
    run_cmd ({ t.ssh, "-F", cfg, "-G", "ha_node" }, &env, NULL, &out, NULL, 5);
    for (const string &l : split (out, '\n'))
      {
	if (l.compare (0, 18, "hostkeyalgorithms ") == 0)
	  {
	    vector<string> algs = split (trim (l.substr (18)), ',');
	    for (const string &a : algs)
	      {
		if (a.empty () || a[0] == '+' || a[0] == '-' || a[0] == '^')
		  {
		    return { };
		  }
	      }
	    return algs;
	  }
      }
    return { };
  }

  bool fips_mode ()
  {
    string v;
    return read_file ("/proc/sys/crypto/fips_enabled", &v) && trim (v) == "1";
  }

  /*
   * The signature algorithms ssh is held to for one host key type, out of what this server allows; "" when
   * this server's ssh cannot use the type (a FIPS system has no ed25519).
   */
  string alg_for (const string &type, const vector<string> &allowed)
  {
    if (type == "ssh-ed25519" && fips_mode ())
      {
	return "";
      }
    vector<string> mine = type == "ssh-rsa" ? vector<string> { "rsa-sha2-512", "rsa-sha2-256" } : vector<string> { type };
    string out;
    for (const string &m : mine)
      {
	bool ok = allowed.empty ();
	for (const string &a : allowed)
	  {
	    ok = ok || a == m;
	  }
	if (ok)
	  {
	    out += (out.empty () ? "" : ",") + m;
	  }
      }
    return out;
  }

  int key_rank (const string &type)
  {
    return type == "ssh-ed25519" ? 0 : type.compare (0, 6, "ecdsa-") == 0 ? 1 : type == "ssh-rsa" ? 2 : 3;
  }

  /* an ssh failure before the login, by what ssh printed */
  void classify_ssh_error (node_result *r, const string &err)
  {
    auto has = [&] (const char *s) { return err.find (s) != string::npos; };
    if (has ("Could not resolve hostname"))
      {
	set_error (r, "name_unresolved", "the name cannot be resolved on this server");
      }
    else if (has ("Connection refused"))
      {
	set_error (r, "port_refused", "connection refused on port " + r->port);
      }
    else if (has ("timed out") || has ("No route to host") || has ("Network is unreachable"))
      {
	set_error (r, "port_timeout", "no answer on port " + r->port);
      }
    else if (has ("Host key verification failed") || has ("HOST IDENTIFICATION HAS CHANGED") || has ("host key is known"))
      {
	set_error (r, "hostkey_mismatch", "the host key is not the confirmed one any more");
      }
    else
      {
	string l = last_line (err);
	set_error (r, "ssh_failed", l.empty () ? "ssh ended without a message" : l);
      }
  }

  /*
   * Step 1 for a node: resolve the name, read the host keys (no login), and pin the one whose fingerprint the
   * user confirmed (want): it goes alone into a known_hosts file of its own (kh), and its type is the only one
   * ssh will accept from the node. Without `want` the result is hostkey_unknown with the key to confirm: the
   * best of the types this server's ssh can use (allowed).
   */
  void scan_node (const ssh_tools &t, const vector<string> &allowed, const string &want, const string &kh, node_result *r,
		  long long deadline)
  {
    struct addrinfo hints, *ai = NULL;
    memset (&hints, 0, sizeof (hints));
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo (r->host.c_str (), r->port.c_str (), &hints, &ai) != 0 || ai == NULL)
      {
	set_error (r, "name_unresolved", "the name cannot be resolved on this server");
	return;
      }
    string out, err;
    vector<string> env = ssh_env ({ });
    double limit = (double) (deadline - now_ms ()) / 1000;
    run_cmd ({ t.keyscan, "-T", "10", "-p", r->port, r->host }, &env, NULL, &out, &err, limit > 12 ? 12 : limit < 1 ? 1 : limit);
    for (const string &line : split (out, '\n'))
      {
	std::istringstream in (line);
	string spec;
	host_key k;
	in >> spec >> k.type >> k.b64;
	if (spec.empty () || spec[0] == '#' || k.b64.empty ())
	  {
	    continue;
	  }
	k.fp = key_fingerprint (k.b64);
	if (!k.fp.empty ())
	  {
	    r->keys.push_back (k);
	  }
      }
    if (r->keys.empty ())
      {
	int pr = tcp_probe (ai, 3000);
	freeaddrinfo (ai);
	if (pr == 1)
	  {
	    set_error (r, "port_refused", "connection refused on port " + r->port);
	  }
	else if (pr == 2)
	  {
	    set_error (r, "port_timeout", "no answer on port " + r->port);
	  }
	else
	  {
	    set_error (r, "ssh_failed", "port " + r->port + " answers, but no ssh host key came back");
	  }
	return;
      }
    freeaddrinfo (ai);
    const host_key *best = NULL, *match = NULL;
    string types;
    for (const host_key &k : r->keys)
      {
	types += (types.empty () ? "" : ", ") + k.type;
	if (!alg_for (k.type, allowed).empty () && (best == NULL || key_rank (k.type) < key_rank (best->type)))
	  {
	    best = &k;
	  }
	if (k.fp == want)
	  {
	    match = &k;
	  }
      }
    if (best == NULL)
      {
	set_error (r, "ssh_failed", "this server's ssh settings allow none of the node's host key types (" + types + ")");
	return;
      }
    if (want.empty ())
      {
	r->key = *best;
	r->code = "hostkey_unknown";
	r->message = "confirm the host key " + best->fp + " and check again with it";
	return;
      }
    if (match == NULL)
      {
	r->key = *best;
	set_error (r, "hostkey_mismatch", "the host key is " + best->fp + ", not " + want);
	return;
      }
    r->key = *match;
    r->alg = alg_for (match->type, allowed);
    r->kh = kh;
    if (r->alg.empty ())
      {
	set_error (r, "ssh_failed", "the confirmed host key is " + match->type + ", which this server's ssh settings do not allow;"
		   " check the node again and confirm the key shown then");
	return;
      }
    string name = r->host;
    for (char &c : name)
      {
	c = (char) tolower ((unsigned char) c);
      }
    string spec = r->port == "22" ? name : "[" + name + "]:" + r->port;
    if (!write_file (kh, spec + " " + match->type + " " + match->b64 + "\n", 0600))
      {
	set_error (r, "ssh_failed", "cannot write " + kh);
      }
  }

  /* the options of every ssh call the manager makes for one node, on top of the settings file */
  vector<string> node_opts (const string &cfg, const node_result &r, bool with_master)
  {
    return { "-F", cfg, "-o", "ControlPath=" + (with_master ? r.ctl : string ("none")), "-o", "UserKnownHostsFile=" + r.kh,
	     "-o", "HostKeyAlgorithms=" + r.alg, "-p", r.port, "-l", r.user
	   };
  }

  /*
   * Step 2: which ways to log in the node's sshd allows for this account. A connection that tries none of them
   * gets the list from the server; nothing is counted as a failed login. password is used when it is allowed,
   * else keyboard-interactive, and only that one.
   */
  void probe_auth (const ssh_tools &t, const string &cfg, node_result *r, long long deadline)
  {
    vector<string> argv = { t.ssh };
    vector<string> opts = node_opts (cfg, *r, false);
    argv.insert (argv.end (), opts.begin (), opts.end ());
    argv.insert (argv.end (), { "-T", r->host, "true" });
    string out, err;
    vector<string> env = ssh_env ({ });
    double limit = (double) (deadline - now_ms ()) / 1000;
    int rc = run_cmd (argv, &env, NULL, &out, &err, limit > 12 ? 12 : limit < 1 ? 1 : limit);
    size_t b = err.find ("Permission denied ("), e = b == string::npos ? b : err.find (')', b);
    if (b == string::npos || e == string::npos)
      {
	if (rc == 0)
	  {
	    set_error (r, "no_password_auth", "the node let " + r->user + " in without any login; a password login is needed");
	  }
	else if (rc == -2)
	  {
	    set_error (r, "login_timeout", "sshd did not answer in time (step: reading the allowed login methods)");
	  }
	else
	  {
	    classify_ssh_error (r, err);
	  }
	return;
      }
    string list = "," + err.substr (b + 19, e - b - 19) + ",";
    if (list.find (",password,") != string::npos)
      {
	r->auth = "password";
      }
    else if (list.find (",keyboard-interactive,") != string::npos)
      {
	r->auth = "keyboard-interactive";
      }
    else
      {
	set_error (r, "no_password_auth", "sshd allows only " + list.substr (1, list.size () - 2) + " for " + r->user);
      }
  }

  bool master_alive (const ssh_tools &t, const string &ctl)
  {
    vector<string> env = ssh_env ({ });
    return run_cmd ({ t.ssh, "-F", "/dev/null", "-o", "ControlPath=" + ctl, "-O", "check", "ha_node" }, &env, NULL, NULL, NULL, 5) == 0;
  }

  void master_exit (const ssh_tools &t, const string &ctl)
  {
    vector<string> env = ssh_env ({ });
    run_cmd ({ t.ssh, "-F", "/dev/null", "-o", "ControlPath=" + ctl, "-O", "exit", "ha_node" }, &env, NULL, NULL, NULL, 5);
    unlink (ctl.c_str ());
  }

  /*
   * Step 3: the one login of a node. ssh starts as a ControlMaster in a session without a terminal and asks
   * for the password through ha_askpass.sh, which reads the FIFO named in HA_ASKPASS_FIFO. The FIFO is opened
   * without blocking: no reader (askpass not called: ssh failed earlier) never holds the manager. The password
   * is written once. A second reader after that means the server asked for something more (a one-time code, a
   * new password): nothing is answered and ssh is stopped. The password is wiped in every case.
   */
  void master_login (const ssh_tools &t, const string &cfg, const string &dir, node_result *r, string &pw, long long deadline)
  {
    string fifo = dir + "/pw." + random_hex (8);
    int log_fd = open (r->log.c_str (), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (log_fd < 0 || mkfifo (fifo.c_str (), 0600) != 0)
      {
	close_fd (&log_fd);
	wipe (pw);
	set_error (r, "ssh_failed", "cannot create a file in " + dir);
	return;
      }
    vector<string> argv = { t.ssh, "-M", "-N", "-f", "-o", "ControlMaster=yes", "-o", "ControlPersist=yes", "-o", "BatchMode=no",
      "-o", "PreferredAuthentications=" + r->auth, "-o", "NumberOfPasswordPrompts=1"
    };
    if (r->auth == "password")
      {
	argv.insert (argv.end (), { "-o", "PasswordAuthentication=yes" });
      }
    else
      {
	argv.insert (argv.end (), { "-o", "KbdInteractiveAuthentication=yes", "-o", "ChallengeResponseAuthentication=yes" });
      }
    vector<string> opts = node_opts (cfg, *r, true);
    argv.insert (argv.end (), opts.begin (), opts.end ());
    argv.push_back (r->host);
    vector<string> env = ssh_env ({ "SSH_ASKPASS=" + askpass_path (), "SSH_ASKPASS_REQUIRE=force", "DISPLAY=x",
				    "HA_ASKPASS_FIFO=" + fifo
				  });
    spawn_opts o;
    o.out_fd = log_fd;
    o.err_fd = log_fd;
    o.env = &env;
    pid_t pid = spawn (argv, o);
    close_fd (&log_fd);
    if (pid < 0)
      {
	unlink (fifo.c_str ());
	wipe (pw);
	set_error (r, "ssh_failed", "cannot start " + t.ssh);
	return;
      }
    long long end = now_ms () + 20000;
    if (end > deadline)
      {
	end = deadline;
      }
    enum { WAIT_ASK, GIVEN, GIVEN_AND_READ } state = WAIT_ASK;
    bool exited = false, asked_more = false, timed_out = false;
    int st = 0;
    while (!exited)
      {
	if (waitpid (pid, &st, WNOHANG) == pid)
	  {
	    exited = true;
	    break;
	  }
	if (now_ms () >= end)
	  {
	    timed_out = true;
	  }
	else
	  {
	    int w = open (fifo.c_str (), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	    if (w >= 0)
	      {
		if (state == WAIT_ASK)
		  {
		    string line = pw + "\n";
		    ssize_t n = write (w, line.data (), line.size ());
		    (void) n;
		    wipe (line);
		    wipe (pw);
		    state = GIVEN;
		  }
		else if (state == GIVEN_AND_READ)
		  {
		    asked_more = true;
		  }
		close (w);
	      }
	    else if (state == GIVEN)
	      {
		state = GIVEN_AND_READ; /* the first reader has gone */
	      }
	  }
	if (asked_more || timed_out)
	  {
	    kill (-pid, SIGKILL);
	    kill (pid, SIGKILL);
	    waitpid (pid, &st, 0);
	    break;
	  }
	poll (NULL, 0, 20);
      }
    unlink (fifo.c_str ());
    wipe (pw);
    string log;
    read_file (r->log, &log);
    if (asked_more)
      {
	set_error (r, "auth_failed", "the node asked for more than the password (a one-time code or a new password);"
		   " only a login with the password alone is supported");
      }
    else if (timed_out)
      {
	set_error (r, "login_timeout", state == WAIT_ASK ? "the login did not get as far as the password in time (step: login)"
		   : "the node did not answer the password in time (step: login)");
      }
    else if (WIFEXITED (st) && WEXITSTATUS (st) == 0)
      {
	r->master = true;
	if (!master_alive (t, r->ctl))
	  {
	    set_error (r, "ssh_failed", "the connection closed right after the login");
	  }
	else
	  {
	    /* an expired password logs in, but the node then runs nothing: found here, not at the first step */
	    vector<string> cmd = { t.ssh };
	    cmd.insert (cmd.end (), opts.begin (), opts.end ());
	    cmd.insert (cmd.end (), { "-T", r->host, "true" });
	    string out, err;
	    vector<string> cenv = ssh_env ({ });
	    run_cmd (cmd, &cenv, NULL, &out, &err, 10);
	    if ((out + err).find ("Password change required") != string::npos)
	      {
		set_error (r, "auth_failed", "the password of " + r->user + " has expired on the node; change it there first");
	      }
	  }
      }
    else if (log.find ("Permission denied") != string::npos)
      {
	if (state == WAIT_ASK)
	  {
	    set_error (r, "ssh_failed", "the password was not asked for (" + last_line (log) + ")");
	  }
	else
	  {
	    set_error (r, "auth_failed", "the login of " + r->user + " failed (wrong password, or the account is locked)");
	  }
      }
    else
      {
	classify_ssh_error (r, log);
      }
  }

  /* a new folder for the ControlMaster sockets of one run: $CUBRID/tmp/hs<8 hex>, ours alone */
  string make_control_dir ()
  {
    mkdir ((cubrid_dir () + "/tmp").c_str (), 0755);
    for (int i = 0; i < 5; i++)
      {
	string d = cubrid_dir () + "/tmp/hs" + random_hex (4);
	if (mkdir (d.c_str (), 0700) == 0)
	  {
	    return d;
	  }
      }
    return "";
  }

  /* a unix socket path holds 108 bytes, and ssh adds a 17 character suffix while it creates the socket */
  bool control_path_fits (const string &ctl)
  {
    struct sockaddr_un un;
    return ctl.size () + 17 < sizeof (un.sun_path);
  }

  /* ends the ControlMasters of a folder and removes it; only a folder make_control_dir () made */
  void remove_control_dir (const ssh_tools &t, const string &d)
  {
    string prefix = cubrid_dir () + "/tmp/hs";
    if (d.compare (0, prefix.size (), prefix) != 0 || d.find ('/', prefix.size ()) != string::npos || !valid_path (d))
      {
	return;
      }
    DIR *dp = opendir (d.c_str ());
    if (dp == NULL)
      {
	return;
      }
    struct dirent *e;
    while ((e = readdir (dp)) != NULL)
      {
	string name = e->d_name;
	if (name == "." || name == "..")
	  {
	    continue;
	  }
	string p = d + "/" + name;
	struct stat st;
	if (lstat (p.c_str (), &st) == 0 && S_ISSOCK (st.st_mode) && !t.ssh.empty ())
	  {
	    master_exit (t, p);
	  }
	unlink (p.c_str ());
      }
    closedir (dp);
    rmdir (d.c_str ());
  }

  /* the ControlMasters a job's last run left behind (its watcher is gone too): end them */
  void end_old_masters (const string &dir)
  {
    string d;
    if (!read_file (dir + "/control_dir", &d) || trim (d).empty ())
      {
	return;
      }
    ssh_tools t;
    string missing;
    find_ssh_tools (&t, &missing);
    remove_control_dir (t, trim (d));
    unlink ((dir + "/control_dir").c_str ());
  }

  struct login_set
  {
    ssh_tools tools;
    string cfg, ctl_dir, run;
    vector<node_result> nodes;
  };

  /* the settings file of a run: one Host block per node, then the common part */
  string run_ssh_config (const vector<node_result> &nodes)
  {
    string s = "# written by the manager for one run of ha_add_nodes; it holds no secret\n";
    for (const node_result &r : nodes)
      {
	s += "Host " + r.host + "\n  HostName " + r.host + "\n	Port " + r.port + "\n  User " + r.user + "\n  ControlPath " + r.ctl
	     + "\n  UserKnownHostsFile " + r.kh + "\n  HostKeyAlgorithms " + r.alg + "\n";
      }
    return s + common_ssh_config ();
  }

  /*
   * C3: logs in to every node (the nodes side by side, one login try each) and leaves a ControlMaster per node.
   * true: all are in, and the run's settings file is written. false: *code and *message say why (the first node
   * that failed), the nodes that did get in are logged out again. s->nodes has every node's result either way.
   * dir: where the run's files go (settings, known_hosts, login logs); the passwords in `nodes` are wiped.
   */
  bool login_all (const string &dir, vector<node_in> &nodes, login_set *s, string *code, string *message)
  {
    string missing, why;
    auto fail = [&] (const string &c, const string &m)
    {
      *code = c;
      *message = m;
      for (node_in &n : nodes)
	{
	  wipe (n.pw);
	}
      return false;
    };
    if (!find_ssh_tools (&s->tools, &missing))
      {
	return fail ("local_ssh_missing", "this server has no " + missing);
      }
    string sc = check_script (askpass_path (), HA_ASKPASS_SHA256, NULL, &why);
    if (!sc.empty ())
      {
	return fail (sc, why);
      }
    s->run = random_hex (4);
    s->cfg = dir + "/ssh_config." + s->run;
    s->ctl_dir = make_control_dir ();
    if (s->ctl_dir.empty () || !write_file (s->cfg, common_ssh_config (), 0600))
      {
	return fail ("ssh_failed", "cannot create the files of the run under " + cubrid_dir () + "/tmp");
      }
    s->nodes.assign (nodes.size (), node_result ());
    vector<string> allowed = allowed_hostkey_algs (s->tools, s->cfg);
    /* every step has its own limit (12 + 12 + 20 + 5 seconds); the whole is as long as their sum */
    long long deadline = now_ms () + 50000;
    auto one = [&] (size_t i)
    {
      node_result *r = &s->nodes[i];
      r->host = nodes[i].host;
      r->port = nodes[i].port;
      r->user = nodes[i].user;
      r->code = "ok";
      r->ctl = s->ctl_dir + "/" + std::to_string (i);
      r->log = dir + "/login." + s->run + "." + std::to_string (i) + ".log";
      if (!control_path_fits (r->ctl))
	{
	  set_error (r, "controlpath_too_long", r->ctl + " is too long for a socket");
	}
      if (r->code == "ok")
	{
	  scan_node (s->tools, allowed, nodes[i].fp, dir + "/known_hosts." + s->run + "." + std::to_string (i), r, deadline);
	}
      if (r->code == "ok")
	{
	  probe_auth (s->tools, s->cfg, r, deadline);
	}
      if (r->code == "ok")
	{
	  master_login (s->tools, s->cfg, dir, r, nodes[i].pw, deadline);
	}
      wipe (nodes[i].pw);
    };
    vector<std::thread> threads;
    for (size_t i = 0; i < nodes.size (); i++)
      {
	try
	  {
	    threads.emplace_back (one, i);
	  }
	catch (...)
	  {
	    one (i);		/* no thread to spare: in turn */
	  }
      }
    for (std::thread &th : threads)
      {
	th.join ();
      }
    const node_result *bad = NULL;
    for (const node_result &r : s->nodes)
      {
	if (r.code != "ok" && bad == NULL)
	  {
	    bad = &r;
	  }
	LOG_INFO ("ha_add_nodes: login %s port %s user %s: %s %s", r.host.c_str (), r.port.c_str (), r.user.c_str (),
		  r.code.c_str (), r.message.c_str ());
      }
    if (bad == NULL && !write_file (s->cfg, run_ssh_config (s->nodes), 0600))
      {
	remove_control_dir (s->tools, s->ctl_dir);
	return fail ("ssh_failed", "cannot write " + s->cfg);
      }
    if (bad != NULL)
      {
	remove_control_dir (s->tools, s->ctl_dir);
	return fail (bad->code, bad->message);
      }
    return true;
  }

  /* the files a run left in a job folder (settings, known_hosts, login logs) */
  void remove_run_files (const string &dir, const string &run)
  {
    DIR *dp = opendir (dir.c_str ());
    if (dp == NULL || run.empty ())
      {
	return;
      }
    string mark = "." + run;
    struct dirent *e;
    while ((e = readdir (dp)) != NULL)
      {
	string name = e->d_name;
	if (name.find (mark) != string::npos || name.compare (0, 3, "pw.") == 0)
	  {
	    unlink ((dir + "/" + name).c_str ());
	  }
      }
    closedir (dp);
  }

  /* a job folder with everything in it (plain files only) */
  void remove_job_dir (const string &dir)
  {
    DIR *dp = opendir (dir.c_str ());
    if (dp == NULL)
      {
	return;
      }
    struct dirent *e;
    while ((e = readdir (dp)) != NULL)
      {
	string name = e->d_name;
	if (name != "." && name != "..")
	  {
	    unlink ((dir + "/" + name).c_str ());
	  }
      }
    closedir (dp);
    rmdir (dir.c_str ());
  }

  string one_line (string s)
  {
    for (char &c : s)
      {
	if (c == '\t' || c == '\n' || c == '\r')
	  {
	    c = ' ';
	  }
      }
    return s;
  }

  /* node_checks of a job folder: node, code, message, action per line (the script rewrites its nodes' lines) */
  string node_checks_text (const vector<node_result> &nodes)
  {
    string s;
    for (const node_result &r : nodes)
      {
	s += r.host + "\t" + r.code + "\t" + one_line (r.message) + "\t" + action_of (r.code) + "\n";
      }
    return s;
  }

  Json::Value node_checks_json (const string &text)
  {
    Json::Value arr (Json::arrayValue);
    for (const string &line : split (text, '\n'))
      {
	vector<string> f = split (line, '\t');
	if (f.size () >= 2 && !f[0].empty ())
	  {
	    Json::Value e;
	    e["node"] = f[0];
	    e["code"] = f[1];
	    e["message"] = f.size () > 2 ? f[2] : "";
	    e["action"] = f.size () > 3 ? f[3] : "";
	    arr.append (e);
	  }
      }
    return arr;
  }

  /* "11.4.5.1908" out of what cubrid_rel prints */
  string version_in (const string &out)
  {
    size_t p = out.find ('(');
    while (p != string::npos)
      {
	size_t e = p + 1;
	while (e < out.size () && (isdigit ((unsigned char) out[e]) || out[e] == '.'))
	  {
	    e++;
	  }
	if (e > p + 1 && out.find ('.', p) < e)
	  {
	    return out.substr (p + 1, e - p - 1);
	  }
	p = out.find ('(', p + 1);
      }
    return "";
  }

  string quick_version ()
  {
    string out;
    run_cmd ({ cubrid_dir () + "/bin/cubrid_rel" }, NULL, NULL, &out, NULL, 10);
    return version_in (out);
  }

  /* SCRIPT_VERSION=<value> of a node script's text */
  string script_version_in (const string &text)
  {
    for (const string &l : split (text, '\n'))
      {
	if (l.compare (0, 15, "SCRIPT_VERSION=") == 0)
	  {
	    string v = trim (l.substr (15));
	    while (!v.empty () && (v[0] == '"' || v[0] == '\''))
	      {
		v.erase (0, 1);
	      }
	    while (!v.empty () && (v[v.size () - 1] == '"' || v[v.size () - 1] == '\''))
	      {
		v.erase (v.size () - 1);
	      }
	    return v;
	  }
      }
    return "";
  }

  /*
   * What ha_check_ssh reads on a node once it is logged in, sent to /bin/sh on stdin (the command line is
   * "/bin/sh" alone, so the account's login shell has nothing to interpret). The CUBRID environment comes from
   * the account's login shell (bash/sh: a login shell, csh/tcsh: .cshrc).
   */
  const char *NODE_QUERY = R"SH(u=$(id -un); sh_=$(getent passwd "$u" | cut -d: -f7)
echo "LOGIN_SHELL=$sh_"
echo "HOSTNAME=$(uname -n)"
q='echo "CUBRID=$CUBRID"; echo "CUBRID_DATABASES=$CUBRID_DATABASES"; "$CUBRID/bin/cubrid_rel" | grep -m1 CUBRID'
case "$sh_" in
  */csh|*/tcsh) e=$("$sh_" -c "$q" </dev/null 2>/dev/null) ;;
  *) e=$(bash -lc "$q" </dev/null 2>/dev/null) ;;
esac
printf '%s\n' "$e"
c=$(printf '%s\n' "$e" | sed -n 's/^CUBRID=//p' | head -1)
f="$c/share/cms/ha_node.sh"
if [ -n "$c" ] && [ -f "$f" ]; then
  echo "NODE_SCRIPT_SHA256=$(sha256sum "$f" | cut -d' ' -f1)"
  echo "NODE_SCRIPT_VERSION=$(sed -n 's/^SCRIPT_VERSION=//p' "$f" | head -1)"
  echo "NODE_UID=$(id -u)"
  p=$f
  while :; do
    echo "PERM=$(stat -c '%u %a' "$p") $p"
    [ "$p" = / ] && break
    p=$(dirname "$p")
  done
elif [ -n "$c" ]; then
  echo "NODE_SCRIPT_SHA256=missing"
fi
echo "CHECK_DONE=yes"
)SH";

  /* after the login: the node's name, CUBRID and node script, through the ControlMaster. Fills *kv. */
  void check_node (const ssh_tools &t, const string &cfg, node_result *r, std::map<string, string> *kv,
		   const string &local_version)
  {
    vector<string> argv = { t.ssh };
    vector<string> opts = node_opts (cfg, *r, true);
    argv.insert (argv.end (), opts.begin (), opts.end ());
    argv.insert (argv.end (), { "-T", r->host, "/bin/sh" });
    string in = NODE_QUERY, out, err;
    vector<string> env = ssh_env ({ });
    int rc = run_cmd (argv, &env, &in, &out, &err, 20);
    *kv = key_values (out);
    string rel, bad_perm;
    for (const string &l : split (out, '\n'))
      {
	if (l.compare (0, 7, "CUBRID ") == 0 && rel.empty ())
	  {
	    rel = l;
	  }
	if (l.compare (0, 5, "PERM=") == 0 && bad_perm.empty ())
	  {
	    /* PERM=<uid> <octal mode> <path>: the node's account or root, no write bit for group or others */
	    std::istringstream ps (l.substr (5));
	    string uid, mode, path;
	    ps >> uid >> mode;
	    std::getline (ps, path);
	    long m = strtol (mode.c_str (), NULL, 8);
	    if ((uid != (*kv)["NODE_UID"] && uid != "0") || (m & 022) != 0 || mode.empty ())
	      {
		bad_perm = trim (path);
	      }
	  }
      }
    (*kv)["CUBRID_REL"] = rel;
    (*kv)["VERSION"] = version_in (rel);
    string local_script;
    read_file (node_script_path (), &local_script);
    if ((*kv)["CHECK_DONE"] != "yes")
      {
	if (rc == 255 || rc == -2)
	  {
	    set_error (r, "ssh_failed", rc == -2 ? "the node did not answer in time" : last_line (err));
	  }
	else
	  {
	    set_error (r, "remote_shell_failed", "cannot run commands as " + r->user + " (" + last_line (out + "\n" + err) + ")");
	  }
      }
    else if ((*kv)["CUBRID"].empty () || rel.empty ())
      {
	set_error (r, "cubrid_not_found", "CUBRID was not found for " + r->user);
      }
    else if ((*kv)["VERSION"] != local_version)
      {
	set_error (r, "version_mismatch", "CUBRID " + (*kv)["VERSION"] + ", this server " + local_version);
      }
    else if ((*kv)["NODE_SCRIPT_SHA256"] == "missing")
      {
	set_error (r, "node_script_missing", (*kv)["CUBRID"] + "/share/cms/ha_node.sh is missing");
      }
    else if ((*kv)["NODE_SCRIPT_SHA256"] != HA_NODE_SHA256)
      {
	if ((*kv)["NODE_SCRIPT_VERSION"] != script_version_in ("\n" + local_script))
	  {
	    set_error (r, "node_script_version_mismatch", "ha_node.sh is version " + (*kv)["NODE_SCRIPT_VERSION"]
		       + ", this server has " + script_version_in ("\n" + local_script));
	  }
	else
	  {
	    set_error (r, "node_script_tampered", (*kv)["CUBRID"] + "/share/cms/ha_node.sh is not the package's file");
	  }
      }
    else if (!bad_perm.empty ())
      {
	set_error (r, "node_script_unsafe", bad_perm + " is writable by others or owned by another account");
      }
  }
}

/* ---------------------------------------------------------------- tasks */

/*
 * ha_check_ssh: host key first (no password), then the login with the confirmed key.
 * request: host, ssh_port, os_user, hostkey_sha256 (after the first call), os_password (with the key)
 * response: result ("ok", "hostkey_unknown" or an error code), message, action (what the user can do),
 *	     hostkey_type, hostkey_sha256, hostkeys[], and after a login what the node has (remote_*)
 */
int
ext_ha_check_ssh (Json::Value &request, Json::Value &response)
{
  string pw;
  if (request["os_password"].isString ())
    {
      pw = request["os_password"].asString ();
      request["os_password"] = "";
    }
  string host = req_str (request, "host"), port = req_str (request, "ssh_port", "22"), user = req_str (request, "os_user");
  string fp = req_str (request, "hostkey_sha256");
  if (!valid_name (host) || !valid_port (port) || !valid_name (user) || (!fp.empty () && !valid_fp (fp))
      || pw.find ('\n') != string::npos || cubrid_dir ().empty ())
    {
      wipe (pw);
      return fail_resp (response, "invalid host, ssh_port, os_user or hostkey_sha256");
    }
  ssh_tools t;
  node_result r;
  std::map<string, string> kv;
  string missing, why, local_version;
  r.host = host;
  r.port = port;
  r.user = user;
  r.code = "ok";
  string dir = jobs_root () + "/check." + random_hex (6), ctl_dir;
  string cfg = dir + "/ssh_config";
  long long deadline = now_ms () + 50000;
  if (!find_ssh_tools (&t, &missing))
    {
      r.code = "local_ssh_missing";
      r.message = "this server has no " + missing;
    }
  else if (!mkdir_p (jobs_root ()) || mkdir (dir.c_str (), 0700) != 0 || !write_file (cfg, common_ssh_config (), 0600))
    {
      set_error (&r, "ssh_failed", "cannot create " + dir);
    }
  if (r.code == "ok")
    {
      scan_node (t, allowed_hostkey_algs (t, cfg), fp, dir + "/known_hosts", &r, deadline);
    }
  if (r.code == "ok" && pw.empty ())
    {
      r.code = "error";
      r.message = "a password is needed once the host key is confirmed";
    }
  if (r.code == "ok")
    {
      string sc = check_script (askpass_path (), HA_ASKPASS_SHA256, NULL, &why);
      if (!sc.empty ())
	{
	  r.code = sc;
	  r.message = why;
	}
    }
  if (r.code == "ok")
    {
      ctl_dir = make_control_dir ();
      r.ctl = ctl_dir + "/0";
      r.log = dir + "/login.log";
      if (ctl_dir.empty ())
	{
	  set_error (&r, "ssh_failed", "cannot create a folder under " + cubrid_dir () + "/tmp");
	}
      else if (!control_path_fits (r.ctl))
	{
	  set_error (&r, "controlpath_too_long", r.ctl + " is too long for a socket");
	}
    }
  if (r.code == "ok")
    {
      probe_auth (t, cfg, &r, deadline);
    }
  if (r.code == "ok")
    {
      master_login (t, cfg, dir, &r, pw, deadline);
    }
  wipe (pw);
  if (r.code == "ok")
    {
      local_version = quick_version ();
      check_node (t, cfg, &r, &kv, local_version);
    }
  if (!ctl_dir.empty ())
    {
      remove_control_dir (t, ctl_dir);
    }
  remove_job_dir (dir);
  LOG_INFO ("ha_check_ssh: %s port %s user %s: %s %s", host.c_str (), port.c_str (), user.c_str (), r.code.c_str (),
	    r.message.c_str ());
  response["result"] = r.code;
  response["message"] = r.code == "ok" ? "" : r.message;
  response["action"] = action_of (r.code);
  response["hostkey_type"] = r.key.type;
  response["hostkey_sha256"] = r.key.fp;
  Json::Value keys (Json::arrayValue);
  for (const host_key &k : r.keys)
    {
      Json::Value e;
      e["type"] = k.type;
      e["sha256"] = k.fp;
      keys.append (e);
    }
  response["hostkeys"] = keys;
  response["auth_method"] = r.auth;
  response["remote_hostname"] = kv["HOSTNAME"];
  response["remote_login_shell"] = kv["LOGIN_SHELL"];
  response["remote_cubrid"] = kv["CUBRID"];
  response["remote_cubrid_databases"] = kv["CUBRID_DATABASES"];
  response["remote_cubrid_rel"] = kv["CUBRID_REL"];
  response["remote_version"] = kv["VERSION"];
  response["local_version"] = local_version.empty () ? quick_version () : local_version;
  return build_server_header (response, ERR_NO_ERROR, "none");
}

/*
 * ha_add_nodes: action plan | run | rollback | cleanup
 * plan/run: new_nodes[], existing_nodes[], db_names, confirm_master_restart, confirm_other_db_restart, master_host,
 *	     repl_log_home, backup_path, remote_backup_path, ha_group, ha_port_id, rollback_convert, start_ha
 * rollback/cleanup: uuid of the earlier job + the nodes' passwords (new_nodes / existing_nodes as before)
 * A node that cannot be logged in to fails the request itself: error (code), message, error_action and
 * node_checks[] (node, code, message, action) come with the failure, and no job is started.
 */
int
ext_ha_add_nodes (Json::Value &request, Json::Value &response)
{
  string action = req_str (request, "action");
  vector<node_in> news, olds;
  string bad, why;
  int script_fd = -1;
  string lock_prev;
  bool ok_new = read_nodes (request, "new_nodes", true, &news, &bad);
  bool ok_old = ok_new && read_nodes (request, "existing_nodes", false, &olds, &bad);
  vector<node_in> all = news;
  all.insert (all.end (), olds.begin (), olds.end ());
  auto give_up = [&] (const string &note)
  {
    wipe_nodes (news);
    wipe_nodes (olds);
    wipe_nodes (all);
    close_fd (&script_fd);
    return fail_resp (response, note);
  };
  /* a failure with a code: what the screen shows per cause */
  auto give_up_code = [&] (const string &code, const string &message)
  {
    response["error"] = code;
    response["message"] = message;
    response["error_action"] = action_of (code);
    return give_up (message);
  };
  if (!ok_new || !ok_old)
    {
      return give_up ("invalid or missing " + bad);
    }
  if (action != "plan" && action != "run" && action != "rollback" && action != "cleanup")
    {
      return give_up ("invalid action (plan, run, rollback or cleanup)");
    }
  if (cubrid_dir ().empty ())
    {
      return give_up ("CUBRID is not set for the manager");
    }
  /* the scripts of this build only; ha_add_nodes.sh stays open and is run through this descriptor (C4) */
  const char *others[][2] = { { "ha_node.sh", HA_NODE_SHA256 }, { "ha_askpass.sh", HA_ASKPASS_SHA256 },
    { "ha_watch.sh", HA_WATCH_SHA256 }
  };
  string sc = check_script (script_path (), HA_ADD_NODES_SHA256, &script_fd, &why);
  for (size_t i = 0; sc.empty () && i < 3; i++)
    {
      sc = check_script (cubrid_dir () + "/share/cms/" + others[i][0], others[i][1], NULL, &why);
    }
  if (!sc.empty ())
    {
      return give_up_code (sc, why);
    }
  for (size_t i = 0; i < all.size (); i++)
    {
      for (size_t k = i + 1; k < all.size (); k++)
	{
	  if (all[i].host == all[k].host)
	    {
	      return give_up ("invalid " + all[i].host + " (listed twice)");
	    }
	}
    }

  string uuid, dir, old_pid;
  vector<string> args;
  if (action == "plan" || action == "run")
    {
      if (news.empty ())
	{
	  return give_up ("invalid or missing new_nodes (at least one)");
	}
      uuid = new_uuid ();
      dir = jobs_root () + "/" + uuid;
      string mh = req_str (request, "master_host");
      if (!mh.empty ())
	{
	  if (!valid_name (mh))
	    {
	      return give_up ("invalid master_host");
	    }
	  args.insert (args.end (), { "--master-host", mh });
	}
      vector<string> dbs;
      const Json::Value &d = request["db_names"];
      if (d.isArray ())
	{
	  for (Json::UInt i = 0; i < d.size (); i++)
	    {
	      dbs.push_back (d[i].isString () ? d[i].asString () : "");
	    }
	}
      else if (d.isString ())
	{
	  for (const string &x : split (d.asString (), ','))
	    {
	      if (!trim (x).empty ())
		{
		  dbs.push_back (trim (x));
		}
	    }
	}
      for (const string &x : dbs)
	{
	  if (!valid_name (x))
	    {
	      return give_up ("invalid db_names");
	    }
	  args.insert (args.end (), { "--db", x });
	}
      if (!request["db_name"].isNull ())
	{
	  return give_up ("invalid db_name (not used: pass db_names)");
	}
      if (req_str (request, "confirm_master_restart") == "yes")
	{
	  args.push_back ("--confirm-master-restart");
	}
      if (req_str (request, "confirm_other_db_restart") == "yes")
	{
	  args.push_back ("--confirm-other-db-restart");
	}
      if (req_str (request, "confirm_new_node_db_restart") == "yes")
	{
	  args.push_back ("--confirm-new-node-db-restart");
	}
      for (node_in &n : news)
	{
	  args.insert (args.end (), { "--new", n.role + ":" + n.host + ":" + n.port + ":" + n.user });
	}
      for (node_in &n : olds)
	{
	  args.insert (args.end (), { "--existing", n.host + ":" + n.port + ":" + n.user });
	}
      const char *paths[][2] = { { "repl_log_home", "--repl-log-home" }, { "backup_path", "--backup-path" },
	{ "remote_backup_path", "--remote-backup-path" }
      };
      for (auto &pk : paths)
	{
	  string v = req_str (request, pk[0]);
	  if (v.empty ())
	    {
	      continue;
	    }
	  if (!valid_path (v))
	    {
	      return give_up (string ("invalid ") + pk[0]);
	    }
	  args.insert (args.end (), { pk[1], v });
	}
      string group = req_str (request, "ha_group", "cubrid"), port_id = req_str (request, "ha_port_id", "59901");
      if (!valid_name (group) || !valid_port (port_id))
	{
	  return give_up ("invalid ha_group or ha_port_id");
	}
      args.insert (args.end (), { "--ha-group", group, "--ha-port-id", port_id });
      if (req_str (request, "rollback_convert") == "yes")
	{
	  args.push_back ("--rollback-convert");
	}
      if (req_str (request, "start_ha", "yes") == "no")
	{
	  args.push_back ("--no-start-ha");
	}
    }
  else
    {
      /* rollback / cleanup act on an earlier job's folder with the arguments it was given */
      uuid = req_str (request, "uuid");
      if (!valid_uuid (uuid))
	{
	  return give_up ("invalid uuid");
	}
      dir = jobs_root () + "/" + uuid;
      string saved;
      if (!read_file (dir + "/args", &saved))
	{
	  return give_up ("no such job: " + uuid);
	}
      std::map<string, string> job = key_values (state_of (dir, "job"));
      old_pid = job["pid"];
      if (job["status"] == "running" && proc_start ((pid_t) atoi (job["pid"].c_str ())) == job["pid_start"])
	{
	  return give_up ("the job is still running: " + uuid);
	}
      std::istringstream in (saved);
      string line;
      while (std::getline (in, line))
	{
	  if (!line.empty ())
	    {
	      args.push_back (line);
	    }
	}
      if (action == "cleanup")
	{
	  if (job["status"] != "success")
	    {
	      return give_up ("only a successful job's kept databases can be removed");
	    }
	  std::istringstream ks (job["kept"]);
	  string k;
	  while (ks >> k)
	    {
	      args.insert (args.end (), { "--keep", k });
	    }
	}
    }

  int lr = lock_reserve (uuid, &why, &lock_prev);
  if (lr != 0)
    {
      return give_up (lr > 0 ? "busy: " + why : why);
    }
  if (action == "plan" || action == "run")
    {
      if (mkdir (dir.c_str (), 0700) != 0)
	{
	  lock_release_if (uuid, lock_prev);
	  return give_up ("cannot create " + dir);
	}
      string saved;
      for (const string &a : args)
	{
	  saved += a + "\n";
	}
      std::ofstream out ((dir + "/args").c_str ());
      out << saved;
    }
  if (action == "plan" && request["db_accounts"].isArray ())
    {
      /* primary key check with the screen's database logins: results only (no password) go to the job folder */
      string lines;
      Json::Value &acc = request["db_accounts"];
      for (Json::UInt i = 0; i < acc.size (); i++)
	{
	  Json::Value &a = acc[i];
	  string db = req_str (a, "db"), user = req_str (a, "user"), pw;
	  if (a["password"].isString ())
	    {
	      pw = a["password"].asString ();
	      a["password"] = "";
	    }
	  if (!valid_name (db) || !valid_name (user) || pw.find ('\n') != string::npos)
	    {
	      wipe (pw);
	      continue;
	    }
	  string tables, msg, st = pk_check (db, user, pw, &tables, &msg);
	  lines += db + "\t" + st + "\t" + (st == "failed" ? msg : tables) + "\n";
	}
      std::ofstream pk ((dir + "/pk_check").c_str ());
      pk << lines;
    }
  bool fresh = action == "plan" || action == "run";
  if (!fresh)
    {
      end_old_masters (dir);	/* of the job's last run, when its watcher did not get to it */
    }
  /* C3: one login per node, the nodes side by side */
  login_set ls;
  string code, message;
  bool in = login_all (dir, all, &ls, &code, &message);
  wipe_nodes (news);
  wipe_nodes (olds);
  wipe_nodes (all);
  if (!in)
    {
      response["node_checks"] = node_checks_json (node_checks_text (ls.nodes));
      if (fresh)
	{
	  remove_job_dir (dir);
	}
      else
	{
	  remove_run_files (dir, ls.run);
	}
      lock_release_if (uuid, lock_prev);
      return give_up_code (code, message);
    }
  lock_touch (uuid);
  write_file (dir + "/node_checks", node_checks_text (ls.nodes), 0600);
  write_file (dir + "/control_dir", ls.ctl_dir + "\n", 0600);
  /* C4: the script runs as the child of the watcher, which logs out of every node when the script ends */
  vector<string> argv = { watch_path (), "--ssh-config", ls.cfg, "--cp-dir", ls.ctl_dir, "--", action, "--job-dir", dir,
    "--ssh-config", ls.cfg, "--node-script-sha256", HA_NODE_SHA256
  };
  argv.insert (argv.end (), args.begin (), args.end ());
  spawn_opts so;
  so.keep_fd = script_fd;
  so.detach_twice = true;
  pid_t pid = spawn (argv, so);
  close_fd (&script_fd);
  int st = 0;
  bool spawned = pid >= 0 && waitpid (pid, &st, 0) == pid;
  bool started = false;
  if (spawned)
    {
      /* answer once the script has written its first state: a status call right after this answer then sees
	 this job running at its first step (rollback and cleanup: not the earlier job's end state) */
      for (int i = 0; i < 100 && !started; i++)
	{
	  std::map<string, string> now = key_values (state_of (dir, "job"));
	  started = !now["pid"].empty () && now["pid"] != old_pid;
	  if (!started)
	    {
	      usleep (100 * 1000);
	    }
	}
    }
  if (!spawned)
    {
      remove_control_dir (ls.tools, ls.ctl_dir);
      unlink ((dir + "/control_dir").c_str ());
      lock_release_if (uuid, lock_prev);
      return fail_resp (response, "cannot start " + watch_path ());
    }
  LOG_INFO ("ha_add_nodes: %s job %s started", action.c_str (), uuid.c_str ());
  response["uuid"] = uuid;
  response["action"] = action;
  response["job_status"] = "running";
  response["node_checks"] = node_checks_json (node_checks_text (ls.nodes));
  return build_server_header (response, ERR_NO_ERROR, "none");
}

/* ha_add_nodes_status: uuid, offset -> the job, its steps, the plan and the log from offset */
int
ext_ha_add_nodes_status (Json::Value &request, Json::Value &response)
{
  string uuid = req_str (request, "uuid"), off_s = req_str (request, "offset", "0");
  if (!valid_uuid (uuid) || off_s.empty () || off_s.find_first_not_of ("0123456789") != string::npos)
    {
      return fail_resp (response, "invalid uuid or offset");
    }
  string dir = jobs_root () + "/" + uuid, jobdata;
  if (!read_file (dir + "/job", &jobdata))
    {
      /* the script may not have written its first state yet */
      if (access (dir.c_str (), F_OK) == 0)
	{
	  response["uuid"] = uuid;
	  response["job_status"] = "running";
	  response["phase"] = "start";
	  response["steps"] = Json::Value (Json::arrayValue);
	  response["output"] = "";
	  response["next_offset"] = off_s;
	  return build_server_header (response, ERR_NO_ERROR, "none");
	}
      return fail_resp (response, "no such job: " + uuid);
    }
  std::map<string, string> job = key_values (jobdata);
  string status = job["status"];
  if (status == "running" && proc_start ((pid_t) atoi (job["pid"].c_str ())) != job["pid_start"])
    {
      status = "interrupted";	/* the script is gone without finishing: `rollback` can undo what it did */
      end_old_masters (dir);	/* and its watcher may be gone with it */
    }
  response["uuid"] = uuid;
  response["action"] = job["action"];
  response["job_status"] = status;
  response["phase"] = status == "interrupted" ? "done" : job["phase"];
  response["case"] = atoi (job["case"].c_str ());
  response["error"] = job["error"];
  response["message"] = job["message"];
  response["error_action"] = job["error_action"].empty () ? action_of (job["error"]) : job["error_action"];
  response["detail"] = job["detail"];
  response["started_at"] = atoi (job["started_at"].c_str ());
  response["ended_at"] = atoi (job["ended_at"].c_str ());
  if (!job["downtime_sec"].empty ())
    {
      response["downtime_sec"] = atoi (job["downtime_sec"].c_str ());
    }
  Json::Value dbs (Json::arrayValue);
  std::istringstream ds (job["db_names"]);
  string x;
  while (ds >> x)
    {
      dbs.append (x);
    }
  response["db_names"] = dbs;
  Json::Value kept (Json::arrayValue);
  std::istringstream ks (job["kept"]);
  while (ks >> x)
    {
      kept.append (x);
    }
  response["kept_db_paths"] = kept;
  Json::Value unrestored (Json::arrayValue);
  for (const string &u : split (job["unrestored"], ';'))
    {
      std::istringstream us (u);
      string node, kind, path;
      us >> node >> kind;
      std::getline (us, path);
      if (!node.empty ())
	{
	  Json::Value e;
	  e["node"] = node;
	  e["kind"] = kind;
	  e["path"] = trim (path);
	  unrestored.append (e);
	}
    }
  response["unrestored"] = unrestored;

  /* what the preflight (S3) found, one entry per node and path */
  Json::Value pf (Json::arrayValue);
  std::istringstream ps (state_of (dir, "preflight_failures"));
  string pl;
  while (std::getline (ps, pl))
    {
      vector<string> f = split (pl, '\t');
      if (f.size () >= 4)
	{
	  Json::Value e;
	  e["node"] = f[0];
	  e["path"] = f[1];
	  e["reason"] = f[2];
	  e["detail"] = f[3];
	  pf.append (e);
	}
    }
  response["preflight_failures"] = pf;
  /* per node: the login (C3) and what the script found on it (S2) */
  response["node_checks"] = node_checks_json (state_of (dir, "node_checks"));
  Json::Value lost (Json::arrayValue);
  std::istringstream los (job["lost_nodes"]);
  while (los >> x)
    {
      lost.append (x);
    }
  response["lost_nodes"] = lost;

  Json::Value steps (Json::arrayValue);
  std::istringstream ss (state_of (dir, "steps"));
  string line;
  int cur = 0, first_pending = 0;
  while (std::getline (ss, line))
    {
      vector<string> f = split (line, '\t');
      if (f.size () < 7)
	{
	  continue;
	}
      Json::Value s;
      s["no"] = atoi (f[0].c_str ());
      s["id"] = f[1];
      s["title"] = f[2];
      s["node"] = f[3];
      s["db"] = f[4];
      s["state"] = f[5];
      s["detail"] = f[6];
      s["stage"] = f.size () > 7 ? f[7] : "";
      if (f[5] == "running" || (cur == 0 && f[5] == "failed"))
	{
	  cur = (int) steps.size () + 1;
	}
      if (first_pending == 0 && f[5] == "pending")
	{
	  first_pending = (int) steps.size () + 1;
	}
      steps.append (s);
    }
  if (cur == 0 && status == "running" && !steps.empty ())
    {
      /* between two steps: the one that comes next (after the last one: the last one) */
      cur = first_pending > 0 ? first_pending : (int) steps.size ();
    }
  response["steps"] = steps;
  response["step_count"] = (int) steps.size ();
  response["step_no"] = cur;
  response["step"] = cur > 0 ? steps[cur - 1]["id"] : Json::Value ("");
  response["step_node"] = cur > 0 ? steps[cur - 1]["node"] : Json::Value ("");
  response["step_db"] = cur > 0 ? steps[cur - 1]["db"] : Json::Value ("");

  string plan;
  if (read_file (dir + "/plan", &plan))
    {
      Json::Value pv;
      Json::Reader r;
      if (r.parse (plan, pv))
	{
	  response["plan"] = pv;
	  response["nodes"] = pv["nodes"];
	}
    }

  string log;
  read_file (dir + "/log", &log);
  unsigned long long offset = strtoull (off_s.c_str (), NULL, 10);
  if (offset > log.size ())
    {
      offset = log.size ();
    }
  response["output"] = log.substr ((size_t) offset);
  response["next_offset"] = std::to_string (log.size ());
  return build_server_header (response, ERR_NO_ERROR, "none");
}

/* ha_add_nodes_cancel: uuid -> SIGTERM to the script only (it decides when the cancel takes effect) */
int
ext_ha_add_nodes_cancel (Json::Value &request, Json::Value &response)
{
  string uuid = req_str (request, "uuid");
  if (!valid_uuid (uuid))
    {
      return fail_resp (response, "invalid uuid");
    }
  string dir = jobs_root () + "/" + uuid;
  std::map<string, string> job = key_values (state_of (dir, "job"));
  pid_t pid = (pid_t) atoi (job["pid"].c_str ());
  if (job["status"] != "running" || pid <= 0 || proc_start (pid) != job["pid_start"])
    {
      return fail_resp (response, "no such running job: " + uuid);
    }
  if (access ((dir + "/cancel").c_str (), F_OK) != 0)
    {
      kill (pid, SIGTERM);
      std::ofstream out ((dir + "/cancel").c_str ());
      out << time (NULL) << "\n";
    }
  response["uuid"] = uuid;
  response["job_status"] = "running";
  response["phase"] = job["phase"];
  return build_server_header (response, ERR_NO_ERROR, "none");
}

#endif /* !WINDOWS */

