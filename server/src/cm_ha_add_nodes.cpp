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
 * cm_ha_add_nodes.cpp - the manager side of adding HA nodes: thin on purpose.
 *
 * The work is done by $CUBRID/share/cms/ha_add_nodes.sh. The manager
 *   - checks the shape of the request (names, paths, ports),
 *   - takes the job lock and makes the job folder $CUBRID/tmp/ha_add_nodes/<uuid>,
 *   - starts the ssh agent ($CUBRID/bin/cm_ssh agent) and hands it the node passwords over a pipe,
 *   - starts the script with arguments only (no password), detached from the manager,
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

  string helper_path ()
  {
    return cubrid_dir () + "/bin/cm_ssh";
  }

  /* the script and the helper run as this account: they must be ours and not writable by others */
  bool trusted_program (const string &path, string *why)
  {
    struct stat st;
    if (stat (path.c_str (), &st) != 0 || !S_ISREG (st.st_mode))
      {
	*why = path + " is missing";
	return false;
      }
    if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0 || (st.st_uid != getuid () && st.st_uid != 0))
      {
	*why = path + " is writable by others or owned by another account";
	return false;
      }
    return true;
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
    return f.size () > 19 ? f[19] : "";	/* field 22 of stat; f[0] is field 3 */
  }

  /* ------------------------------------------------------------ the job lock (shared with the script) */

  /* "starting <uuid> <epoch>" or "running <uuid> <pid> <pid start>"; free when the holder is gone */
  bool lock_in_use (const string &content)
  {
    std::istringstream in (content);
    string what, uuid, a, b;
    in >> what >> uuid >> a >> b;
    if (what == "running")
      {
	return proc_start ((pid_t) atoi (a.c_str ())) == b && !b.empty ();
      }
    if (what == "starting")
      {
	return time (NULL) - atol (a.c_str ()) < 60;
      }
    return false;
  }

  /* reserve the lock for a new job; the script turns it into "running" when it starts */
  bool lock_reserve (const string &uuid, string *holder)
  {
    string path = jobs_root () + "/lock";
    int fd = open (path.c_str (), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
      {
	*holder = "cannot open " + path;
	return false;
      }
    flock (fd, LOCK_EX);
    string content;
    read_file (path, &content);
    bool ok = !lock_in_use (content);
    if (ok)
      {
	string line = "starting " + uuid + " " + std::to_string ((long) time (NULL)) + "\n";
	ftruncate (fd, 0);
	pwrite (fd, line.data (), line.size (), 0);
      }
    else
      {
	*holder = trim (content);
      }
    flock (fd, LOCK_UN);
    close (fd);
    return ok;
  }

  void lock_release_if (const string &uuid)
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
      }
    flock (fd, LOCK_UN);
    close (fd);
  }

  /* ------------------------------------------------------------ starting programs */

  /*
   * fork + exec with stdin/stdout/stderr on /dev/null (or out_fd for stdout), keep_fd moved to 3,
   * every other descriptor closed and a new session. Only async-signal-safe calls after fork.
   */
  pid_t spawn (const vector<string> &argv, int keep_fd, int out_fd, bool detach_twice)
  {
    vector<char *> av;
    for (const string &a : argv)
      {
	av.push_back (const_cast<char *> (a.c_str ()));
      }
    av.push_back (NULL);
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
    if (detach_twice && fork () != 0)
      {
	_exit (0);		/* the grandchild belongs to init, so the manager never has to reap it */
      }
    if (keep_fd >= 0 && keep_fd != 3)
      {
	dup2 (keep_fd, 3);
      }
    dup2 (devnull, 0);
    dup2 (out_fd >= 0 ? out_fd : devnull, 1);
    dup2 (devnull, 2);
    for (long fd = keep_fd >= 0 ? 4 : 3; fd < maxfd; fd++)
      {
	close ((int) fd);
      }
    signal (SIGPIPE, SIG_DFL);
    signal (SIGCHLD, SIG_DFL);
    execve (av[0], av.data (), environ);
    _exit (127);
  }

  /* runs a program and collects its stdout (up to the timeout); returns the exit status or -1 */
  int run_collect (const vector<string> &argv, int keep_fd, string *out, int timeout_sec)
  {
    int p[2];
    if (pipe2 (p, O_CLOEXEC) != 0)
      {
	return -1;
      }
    pid_t pid = spawn (argv, keep_fd, p[1], false);
    close (p[1]);
    if (pid < 0)
      {
	close (p[0]);
	return -1;
      }
    time_t end = time (NULL) + timeout_sec;
    char buf[4096];
    for (;;)
      {
	int left = (int) (end - time (NULL));
	if (left <= 0)
	  {
	    kill (pid, SIGTERM);
	    break;
	  }
	struct pollfd pf = { p[0], POLLIN, 0 };
	int r = poll (&pf, 1, left * 1000);
	if (r <= 0)
	  {
	    continue;
	  }
	ssize_t n = read (p[0], buf, sizeof (buf));
	if (n <= 0)
	  {
	    break;
	  }
	out->append (buf, (size_t) n);
      }
    close (p[0]);
    int st = 0;
    if (waitpid (pid, &st, 0) != pid)
      {
	return -1;
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

  /* starts the agent with the credentials on fd 3; "" when it is ready */
  string start_agent (const string &dir, const string &sock, vector<node_in> &nodes)
  {
    int cp[2];
    if (pipe2 (cp, O_CLOEXEC) != 0)
      {
	return "cannot create a pipe";
      }
    int op[2];
    if (pipe2 (op, O_CLOEXEC) != 0)
      {
	close (cp[0]);
	close (cp[1]);
	return "cannot create a pipe";
      }
    pid_t pid = spawn ({ helper_path (), "agent", "--socket", sock, "--log", dir + "/agent.log", "--credentials-fd", "3" },
		       cp[0], op[1], false);
    close (cp[0]);
    close (op[1]);
    if (pid < 0)
      {
	close (cp[1]);
	close (op[0]);
	return "cannot start " + helper_path ();
      }
    string creds;
    for (node_in &n : nodes)
      {
	creds += n.host + "\t" + n.port + "\t" + n.user + "\t" + n.fp + "\t" + n.pw + "\n";
      }
    ssize_t w = write (cp[1], creds.data (), creds.size ());
    wipe (creds);
    close (cp[1]);
    string out;
    char buf[256];
    time_t end = time (NULL) + 10;
    while (out.find ('\n') == string::npos && time (NULL) < end)
      {
	struct pollfd pf = { op[0], POLLIN, 0 };
	if (poll (&pf, 1, 1000) <= 0)
	  {
	    continue;
	  }
	ssize_t n = read (op[0], buf, sizeof (buf));
	if (n <= 0)
	  {
	    break;
	  }
	out.append (buf, (size_t) n);
      }
    close (op[0]);
    int st = 0;
    waitpid (pid, &st, 0);	/* the agent's first process only reports; the server runs in its child */
    if (w < 0 || out.compare (0, 6, "READY ") != 0)
      {
	string log;
	read_file (dir + "/agent.log", &log);
	return "the ssh agent did not start" + (log.empty () ? string ("") : ": " + trim (log));
      }
    return "";
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

  string quick_version ()
  {
    string out;
    run_collect ({ cubrid_dir () + "/bin/cubrid_rel" }, -1, &out, 10);
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
}

/* ---------------------------------------------------------------- tasks */

/*
 * ha_check_ssh: host key first (no password), then the login with the confirmed key.
 * request: host, ssh_port, os_user, hostkey_sha256 (after the first call), os_password (with the key)
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
  string why;
  if (!valid_name (host) || !valid_port (port) || !valid_name (user) || (!fp.empty () && !valid_fp (fp))
      || pw.find ('\n') != string::npos)
    {
      wipe (pw);
      return fail_resp (response, "invalid host, ssh_port, os_user or hostkey_sha256");
    }
  if (!trusted_program (helper_path (), &why))
    {
      wipe (pw);
      return fail_resp (response, why);
    }
  vector<string> argv = { helper_path (), "check", host, port, user };
  if (!fp.empty ())
    {
      argv.push_back (fp);
    }
  int p[2] = { -1, -1 };
  if (!pw.empty ())
    {
      if (pipe2 (p, O_CLOEXEC) != 0)
	{
	  wipe (pw);
	  return fail_resp (response, "cannot create a pipe");
	}
      argv.push_back ("--password-fd");
      argv.push_back ("3");
      pw += "\n";
      ssize_t w = write (p[1], pw.data (), pw.size ());
      (void) w;
      close (p[1]);
    }
  wipe (pw);
  string out;
  int rc = run_collect (argv, p[0], &out, 25);
  if (p[0] >= 0)
    {
      close (p[0]);
    }
  std::map<string, string> kv = key_values (out);
  string result = kv["result"];
  if (rc != 0 || result.empty ())
    {
      result = "error";
      kv["message"] = "the ssh check did not finish";
    }
  if (result == "ok" && !kv["result_after_login"].empty ())
    {
      result = kv["result_after_login"];
      kv["message"] = result == "cubrid_not_found" ? "CUBRID was not found for " + user + " on " + host : "could not run commands on " + host;
    }
  string rel = kv["remote_CUBRID_REL"];
  string remote_version;
  size_t b = rel.find ('(');
  while (b != string::npos && remote_version.empty ())
    {
      size_t e = b + 1;
      while (e < rel.size () && (isdigit ((unsigned char) rel[e]) || rel[e] == '.'))
	{
	  e++;
	}
      if (e > b + 1 && rel.find ('.', b) < e)
	{
	  remote_version = rel.substr (b + 1, e - b - 1);
	}
      b = rel.find ('(', b + 1);
    }
  string local_version = quick_version ();
  if (result == "ok" && remote_version != local_version)
    {
      result = "version_mismatch";
      kv["message"] = host + " CUBRID " + remote_version + ", this server " + local_version;
    }
  response["result"] = result;
  response["message"] = result == "ok" ? "" : kv["message"];
  response["hostkey_type"] = kv["hostkey_type"];
  response["hostkey_sha256"] = kv["hostkey_sha256"];
  response["remote_hostname"] = kv["remote_HOSTNAME"];
  response["remote_login_shell"] = kv["remote_LOGIN_SHELL"];
  response["remote_cubrid"] = kv["remote_CUBRID"];
  response["remote_cubrid_databases"] = kv["remote_CUBRID_DATABASES"];
  response["remote_cubrid_rel"] = rel;
  response["remote_version"] = remote_version;
  response["local_version"] = local_version;
  return build_server_header (response, ERR_NO_ERROR, "none");
}

/*
 * ha_add_nodes: action plan | run | rollback | cleanup
 * plan/run: new_nodes[], existing_nodes[], db_names, confirm_master_restart, confirm_other_db_restart, master_host,
 *           repl_log_home, backup_path, remote_backup_path, ha_group, ha_port_id, rollback_convert, start_ha
 * rollback/cleanup: uuid of the earlier job + the nodes' passwords (new_nodes / existing_nodes as before)
 */
int
ext_ha_add_nodes (Json::Value &request, Json::Value &response)
{
  string action = req_str (request, "action");
  vector<node_in> news, olds;
  string bad, why;
  bool ok_new = read_nodes (request, "new_nodes", true, &news, &bad);
  bool ok_old = ok_new && read_nodes (request, "existing_nodes", false, &olds, &bad);
  vector<node_in> all = news;
  all.insert (all.end (), olds.begin (), olds.end ());
  auto give_up = [&] (const string &note)
  {
    wipe_nodes (news);
    wipe_nodes (olds);
    wipe_nodes (all);
    return fail_resp (response, note);
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
  if (!trusted_program (script_path (), &why) || !trusted_program (helper_path (), &why))
    {
      return give_up (why);
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

  string uuid, dir;
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

  if (!lock_reserve (uuid, &why))
    {
      return give_up ("another job is running: " + why);
    }
  if (action == "plan" || action == "run")
    {
      if (mkdir (dir.c_str (), 0700) != 0)
	{
	  lock_release_if (uuid);
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
  string sock = dir + "/agent." + std::to_string ((long) time (NULL)) + ".sock";
  string err = start_agent (dir, sock, all);
  wipe_nodes (news);
  wipe_nodes (olds);
  wipe_nodes (all);
  if (!err.empty ())
    {
      lock_release_if (uuid);
      return fail_resp (response, err);
    }
  vector<string> argv = { script_path (), action, "--job-dir", dir, "--agent", sock };
  argv.insert (argv.end (), args.begin (), args.end ());
  pid_t pid = spawn (argv, -1, -1, true);
  int st = 0;
  if (pid < 0 || waitpid (pid, &st, 0) != pid)
    {
      string q;
      run_collect ({ helper_path (), "quit", "--agent", sock }, -1, &q, 5);
      lock_release_if (uuid);
      return fail_resp (response, "cannot start " + script_path ());
    }
  LOG_INFO ("ha_add_nodes: %s job %s started", action.c_str (), uuid.c_str ());
  response["uuid"] = uuid;
  response["action"] = action;
  response["job_status"] = "running";
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
    }
  response["uuid"] = uuid;
  response["action"] = job["action"];
  response["job_status"] = status;
  response["phase"] = status == "interrupted" ? "done" : job["phase"];
  response["case"] = atoi (job["case"].c_str ());
  response["error"] = job["error"];
  response["message"] = job["message"];
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

  Json::Value steps (Json::arrayValue);
  std::istringstream ss (state_of (dir, "steps"));
  string line;
  int cur = 0;
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
      if (f[5] == "running" || (cur == 0 && f[5] == "failed"))
	{
	  cur = (int) steps.size () + 1;
	}
      steps.append (s);
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
