#!/bin/bash
#
#  Copyright 2016 CUBRID Corporation
#
#   Licensed under the Apache License, Version 2.0 (the "License");
#   you may not use this file except in compliance with the License.
#   You may obtain a copy of the License at
#
#       http://www.apache.org/licenses/LICENSE-2.0
#
#   Unless required by applicable law or agreed to in writing, software
#   distributed under the License is distributed on an "AS IS" BASIS,
#   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#   See the License for the specific language governing permissions and
#   limitations under the License.
#
# ha_add_nodes.sh - promote a single database server to an HA master and/or add slaves and replicas to an HA group.
#
#   ha_add_nodes.sh <plan|run|rollback|cleanup> --job-dir <dir> [--agent <socket>] [options]
#     --master-host <name>            this server's HA node name (default: uname -n)
#     --db <db>                       (single server only) a database to put into HA; repeat for more
#     --confirm-master-restart        (single server) the running databases restart
#     --confirm-other-db-restart      (single server) databases staying single but running restart too
#     --confirm-new-node-db-restart   other databases running on a new node restart (they stay single)
#     --new <slave|replica>:<host>:<port>:<user>[:<fingerprint>]       a new node; repeat
#     --existing <host>:<port>:<user>[:<fingerprint>]                  an existing node of the group; repeat
#     --repl-log-home <path>          new nodes' replication log folder (default: their $CUBRID_DATABASES)
#     --backup-path <path>            backup folder on this server (default: $CUBRID_DATABASES/ha_add_nodes_backup)
#     --remote-backup-path <path>     folder for the received backup on new nodes (same default there)
#     --ha-group <name> --ha-port-id <port>   (single server) HA group name (default cubrid) and port (59901)
#     --rollback-convert              on failure after the promotion, go back to a single server as well
#     --no-start-ha                   prepare the new nodes but do not start HA on them
#     --keep <host>:<path>            (cleanup) an old database folder a successful job kept aside; repeat
#
# Every value is an argument; nothing in this file is meant to be edited. Passwords never reach this script: the
# ssh agent (cm_ssh agent, started by the manager or, from a terminal, by this script with --ask) holds them and
# runs the remote commands. The fingerprints in --new/--existing are only used when this script starts the agent.
#
# Remote work runs this same file on the node: "ha_add_nodes.sh node <action> ...". Each node action prints its
# result between "@@BEGIN" and "@@END <status>" lines, so text printed by login files does not mix in.

SELF=$(readlink -f "$0")
PROG=ha_add_nodes.sh

# --------------------------------------------------------------------------- common helpers (both sides)

valid_name() { [[ $1 =~ ^[A-Za-z0-9][A-Za-z0-9._-]{0,254}$ ]]; }
valid_path() { [[ $1 =~ ^/[A-Za-z0-9._/-]*$ && $1 != *..* && ${#1} -gt 1 ]]; }
one_line() { local s=${1//$'\t'/ }; s=${s//$'\r'/}; printf '%s' "${s//$'\n'/ | }"; }
epoch() { date +%s; }

# start time of a process (field 22 of /proc/<pid>/stat)
proc_start() {
  local s
  s=$(cat "/proc/$1/stat" 2>/dev/null) || return 1
  s=${s##*) }
  set -- $s
  printf '%s' "${20}"
}

# --------------------------------------------------------------------------- node actions

# CUBRID environment for a login shell that is not bash (csh/tcsh keep it in .cshrc)
node_env_bootstrap() {
  [ -n "${CUBRID:-}" ] && return 0
  local sh_ line
  sh_=$(getent passwd "$(id -un)" | cut -d: -f7)
  case $sh_ in
    */csh|*/tcsh)
      while IFS= read -r line; do
        case $line in
          CUBRID=*|CUBRID_DATABASES=*|PATH=*|LD_LIBRARY_PATH=*) export "${line?}" ;;
        esac
      done < <("$sh_" -c 'echo "CUBRID=$CUBRID"; echo "CUBRID_DATABASES=$CUBRID_DATABASES"; echo "PATH=$PATH"; echo "LD_LIBRARY_PATH=$LD_LIBRARY_PATH"' </dev/null 2>/dev/null)
      ;;
  esac
}

cub_master_running() {
  local f c me
  me=$(id -u)
  for f in /proc/[0-9]*; do
    [ -O "$f" ] || continue
    c=$(tr '\0' ' ' < "$f/cmdline" 2>/dev/null) || continue
    case " $c" in
      " cub_master "*|*"/cub_master "*) return 0 ;;
    esac
  done
  return 1
}

# key=value of one [section] (last value wins)
conf_get() {
  awk -v sec="$2" -v key="$3" '
    { t=$0; gsub(/^[ \t]+|[ \t]+$/, "", t) }
    t ~ /^\[/ { cur=t; next }
    t ~ /^#/ || t == "" { next }
    cur == sec { i=index(t, "="); if (i > 0) { k=substr(t, 1, i-1); gsub(/[ \t]+$/, "", k);
                 if (k == key) { v=substr(t, i+1); gsub(/^[ \t]+/, "", v); val=v } } }
    END { printf "%s", val }' "$1" 2>/dev/null
}

# all key=value pairs of one section, "key<TAB>value" lines
conf_pairs() {
  awk -v sec="$2" '
    { t=$0; gsub(/^[ \t]+|[ \t]+$/, "", t) }
    t ~ /^\[/ { cur=t; next }
    t ~ /^#/ || t == "" { next }
    cur == sec { i=index(t, "="); if (i > 0) { k=substr(t, 1, i-1); gsub(/[ \t]+$/, "", k); v=substr(t, i+1);
                 gsub(/^[ \t]+/, "", v); if (!(k in seen)) order[++n]=k; seen[k]=v } }
    END { for (j=1; j<=n; j++) printf "%s\t%s\n", order[j], seen[order[j]] }' "$1" 2>/dev/null
}

# keep path.bak.<tag> (or an empty path.new.<tag> marker when there was no file), then write the new content.
# The undo looks at which of the two exists.
backup_once() {
  local path=$1 tag=$2
  if [ -e "$path.bak.$tag" ] || [ -e "$path.new.$tag" ]; then return 0; fi
  if [ -e "$path" ]; then cp -p "$path" "$path.bak.$tag" || return 1
  else : > "$path.new.$tag" || return 1
  fi
}

replace_file() {   # replace_file <path> <tmp> : atomic, keeps the mode of the old file
  [ -e "$1" ] && chmod --reference="$1" "$2" 2>/dev/null
  mv -f "$2" "$1"
}

# node conf_edit <file> <tag> <section> <key> <value> [<section> <key> <value> ...]
node_conf_edit() {
  local file=$1 tag=$2 edits="" tmp
  shift 2
  while [ $# -ge 3 ]; do edits+="$1"$'\t'"$2"$'\t'"$3"$'\n'; shift 3; done
  backup_once "$file" "$tag" || { echo "cannot keep a copy of $file"; return 1; }
  tmp="$file.tmp.$tag"
  [ -e "$file" ] || : > "$file"
  EDITS=$edits awk '
    BEGIN { n=split(ENVIRON["EDITS"], E, "\n"); m=0
            for (i=1; i<=n; i++) { if (E[i] == "") continue; split(E[i], f, "\t"); m++; sec[m]=f[1]; key[m]=f[2]; val[m]=f[3] } }
    function flush(s,   i) { for (i=1; i<=m; i++) if (sec[i] == s && !done[i]) { print key[i] "=" val[i]; done[i]=1 } }
    { line=$0; t=line; gsub(/^[ \t]+|[ \t]+$/, "", t) }
    t ~ /^\[/ { if (cur != "") flush(cur); cur=t; seen[t]=1; print line; next }
    t !~ /^#/ && index(t, "=") > 0 {
      k=substr(t, 1, index(t, "=")-1); gsub(/[ \t]+$/, "", k); hit=0
      for (i=1; i<=m; i++) if (sec[i] == cur && key[i] == k) { if (!done[i]) { print k "=" val[i]; done[i]=1 }; hit=1 }
      if (hit) next
    }
    { print line }
    END { if (cur != "") flush(cur)
          for (i=1; i<=m; i++) if (!done[i]) { if (!(sec[i] in seen)) { print ""; print sec[i]; seen[sec[i]]=1 } flush(sec[i]) } }
  ' "$file" > "$tmp" && replace_file "$file" "$tmp"
}

# node dbtxt_put <tag> <db> <vol> <hosts> <log> [<lob>] : the databases.txt line of <db>
node_dbtxt_put() {
  local tag=$1 db=$2 line file="$CUBRID_DATABASES/databases.txt" tmp
  line="$2 $3 $4 $5${6:+ $6}"
  backup_once "$file" "$tag" || return 1
  tmp="$file.tmp.$tag"
  [ -e "$file" ] || : > "$file"
  awk -v db="$db" -v line="$line" '
    { split($0, f, /[ \t]+/); t=$0; sub(/^[ \t]+/, "", t); split(t, g, /[ \t]+/) }
    g[1] == db && t !~ /^#/ { if (!done) print line; done=1; next }
    { print }
    END { if (!done) print line }' "$file" > "$tmp" && replace_file "$file" "$tmp"
}

# node dbtxt_add_hosts <tag> <host,host> <db> [<db> ...] : add hosts to the db-host field (end of the list)
node_dbtxt_add_hosts() {
  local tag=$1 hosts=$2 file="$CUBRID_DATABASES/databases.txt" tmp dbs
  shift 2
  dbs=" $* "
  backup_once "$file" "$tag" || return 1
  tmp="$file.tmp.$tag"
  awk -v dbs="$dbs" -v add="$hosts" '
    { t=$0; sub(/^[ \t]+/, "", t); n=split(t, g, /[ \t]+/) }
    t !~ /^#/ && n >= 4 && index(dbs, " " g[1] " ") > 0 {
      split(g[3], h, ":"); for (k in h) have[h[k]]=1; out=g[3]
      na=split(add, a, ","); for (k=1; k<=na; k++) if (!(a[k] in have)) out=out ":" a[k]
      delete have; g[3]=out; s=g[1]; for (k=2; k<=n; k++) s=s " " g[k]; print s; next }
    { print }' "$file" > "$tmp" && replace_file "$file" "$tmp"
}

# node undo_file <path> <tag> : the copy back, or the file removed when there was none
node_undo_file() {
  local path=$1 tag=$2
  if [ -e "$path.bak.$tag" ]; then mv -f "$path.bak.$tag" "$path" && echo "restored $path"
  elif [ -e "$path.new.$tag" ]; then rm -f "$path" "$path.new.$tag" && echo "removed $path (it did not exist)"
  else echo "nothing to undo for $path"
  fi
}

node_drop_backups() {   # node drop_backups <tag> <path>... : after success, remove the kept copies
  local tag=$1 p
  shift
  for p in "$@"; do rm -f "$p.bak.$tag" "$p.new.$tag"; done
}

node_info() {
  local sh_ rel
  sh_=$(getent passwd "$(id -un)" | cut -d: -f7)
  echo "HOSTNAME=$(uname -n)"
  echo "LOGIN_SHELL=$sh_"
  echo "CUBRID=${CUBRID:-}"
  echo "CUBRID_DATABASES=${CUBRID_DATABASES:-}"
  rel=$("$CUBRID/bin/cubrid_rel" 2>/dev/null </dev/null | grep -m1 CUBRID)
  echo "REL=$rel"
  echo "VERSION=$(printf '%s' "$rel" | sed -n 's/.*(\([0-9][0-9.]*\)[-)].*/\1/p')"
  echo "HA_MODE=$(conf_get "$CUBRID/conf/cubrid.conf" "[common]" ha_mode)"
  local f="$CUBRID/conf/cubrid_ha.conf"
  echo "HA_NODE_LIST=$(conf_get "$f" "[common]" ha_node_list)"
  echo "HA_REPLICA_LIST=$(conf_get "$f" "[common]" ha_replica_list)"
  echo "HA_DB_LIST=$(conf_get "$f" "[common]" ha_db_list)"
  echo "HA_SYNC=$(conf_get "$f" "[common]" ha_copy_sync_mode)"
  echo "HA_COPY_LOG_BASE=$(conf_get "$f" "[common]" ha_copy_log_base)"
  if cub_master_running; then echo "CUB_MASTER=yes"; else echo "CUB_MASTER=no"; fi
  echo "SERVERS=$(cubrid server status 2>/dev/null </dev/null | awk '$1 == "Server" || $1 == "HA-Server" { print $2 }' | tr '\n' ' ')"
  echo "DBTXT=$(awk '$1 !~ /^#/ && NF >= 4 { print $1 }' "$CUBRID_DATABASES/databases.txt" 2>/dev/null | tr '\n' ' ')"
}

# the nearest folder of a path that exists (the path itself when it exists)
existing_ancestor() { local p=$1; while [ ! -e "$p" ] && [ "$p" != / ]; do p=$(dirname "$p"); done; printf '%s' "$p"; }

node_writable() {   # node writable <path>... : "OK <path> <free bytes>" or "NO <path> <folder that is not writable>"
  local p a
  for p in "$@"; do
    a=$(existing_ancestor "$p")
    if [ -d "$a" ] && [ -w "$a" ]; then echo "OK $p $(df -Pk "$a" 2>/dev/null | awk 'NR == 2 { print $4 * 1024 }')"
    else echo "NO $p $a"
    fi
  done
}

node_resolve() {   # node resolve <name>... : prints the names that do not resolve
  local n bad=""
  for n in "$@"; do getent hosts "$n" >/dev/null 2>&1 || bad+=" $n"; done
  echo "UNRESOLVED=${bad# }"
}

node_free_bytes() { df -Pk "$(existing_ancestor "$1")" 2>/dev/null | awk 'NR == 2 { print $4 * 1024 }'; }

node_mode() {   # node mode <db>... : "<db> <active|standby|...>"
  local db out t0
  for db in "$@"; do
    t0=$SECONDS
    # a server that is busy can keep changemode waiting; the caller asks again
    out=$(timeout 20 cubrid changemode "$db@localhost" 2>&1 </dev/null)
    [ $(( SECONDS - t0 )) -ge 5 ] && echo "# changemode $db took $(( SECONDS - t0 )) s"
    case $out in
      *"is active"*) echo "$db active" ;;
      *"is standby"*) echo "$db standby" ;;
      *"to-be-active"*) echo "$db to-be-active" ;;
      *"to-be-standby"*) echo "$db to-be-standby" ;;
      *"not configured for HA"*) echo "$db not_ha" ;;
      *) echo "$db unknown $(one_line "$out")" ;;
    esac
  done
}

# long-lived processes are started detached from everything this script holds open
node_detached() {   # node detached <command...>
  local out rc
  out=$(mktemp) || return 1
  setsid "$@" </dev/null >"$out" 2>&1
  rc=$?
  cat "$out"
  rm -f "$out"
  return $rc
}

node_cubrid() { cubrid "$@" 2>&1 </dev/null; }

node_server_start() { local db rc=0; for db in "$@"; do node_detached cubrid server start "$db" || rc=1; done; return $rc; }
node_server_stop() { local db rc=0 out; for db in "$@"; do out=$(cubrid server stop "$db" 2>&1 </dev/null) || rc=1; echo "$out"; [[ $out == *": fail"* ]] && rc=1; done; return $rc; }

node_master_down() {   # cub_master down (`cubrid master stop` is cub_commdb -A); the manager keeps running
  local i
  cub_master_running || { echo "no cub_master"; return 0; }
  cub_commdb -A </dev/null 2>&1
  for i in $(seq 1 30); do cub_master_running || { echo "cub_master stopped"; return 0; }; sleep 1; done
  echo "cub_master did not stop"
  return 1
}

node_ha_start() {   # heartbeat start for the whole node (every database of ha_db_list)
  local out rc
  out=$(node_detached cubrid heartbeat start)
  rc=$?
  echo "$out"
  [ $rc -eq 0 ] && [[ $out != *": fail"* ]]
}

node_ha_stop() {
  local out
  out=$(node_detached cubrid heartbeat stop)
  echo "$out"
  node_master_down
}

node_master_start() { node_detached cub_master; sleep 2; cub_master_running; }

node_reload() {
  local out rc
  out=$(cubrid heartbeat reload 2>&1 </dev/null)
  rc=$?
  echo "$out"
  [ $rc -eq 0 ] && [[ $out != *fail* ]]
}

node_repl() {   # node repl <start|stop> <node> : success only when the output has no failure line
  local out rc
  out=$(node_detached cubrid heartbeat repl "$1" "$2")
  rc=$?
  echo "$out"
  [ $rc -eq 0 ] && [[ $out != *": fail"* ]] && [[ $out != *"not listed"* ]]
}

node_hb_status() { timeout 30 cubrid heartbeat status 2>&1 </dev/null; }

node_exists() { local p; for p in "$@"; do [ -e "$p" ] && echo "EXISTS $p" || echo "MISSING $p"; done; }

# node keep_old <suffix> <db> <vol> <log> <repl_home> : move what is there aside; print MOVED/CREATED lines
node_keep_old() {
  local suffix=$1 db=$2 vol=$3 log=$4 home=$5 p d paths=("$3")
  [ "$log" = "$vol" ] || paths+=("$log")
  for p in "${paths[@]}"; do
    if [ -d "$p" ] && [ -n "$(ls -A "$p" 2>/dev/null)" ]; then
      mv "$p" "$p$suffix" && echo "MOVED $p" || return 1
    fi
  done
  for d in "$home/${db}_"*; do
    case $d in *.ha_add_nodes_*) continue ;; esac
    [ -d "$d" ] || continue
    mv "$d" "$d$suffix" && echo "MOVED $d" || return 1
  done
  for p in "${paths[@]}"; do
    if [ -d "$p" ]; then echo "EMPTY $p"; else mkdir -p "$p" && echo "CREATED $p" || return 1; fi
  done
}

node_move_back() {   # node move_back <path> <moved path>
  [ -e "$2" ] || { echo "nothing at $2"; return 0; }
  if [ -e "$1" ]; then rm -rf "$1" || return 1; fi
  mv "$2" "$1" && echo "moved back $1"
}

# node clear <created|empty> <path> : what the job wrote into a folder it created (or found empty)
node_clear() {
  case $1 in
    created) rm -rf "$2" ;;
    empty) [ -d "$2" ] || return 0; ( shopt -s dotglob nullglob; rm -rf -- "$2"/* ) ;;
  esac
}

node_remove() { local p; for p in "$@"; do rm -rf "$p"; done; }

node_restore() {   # node restore <db> <backup dir> <log path> <master> <source state>
  # -u: the volumes go where this node's databases.txt says, not where they were on the master
  cp -f "$2/${1}_bkvinf" "$3/" || return 1
  cubrid restoreslave -s "$5" -m "$4" -u -B "$2" "$1" 2>&1 </dev/null
}

node_first_copylog() {   # node first_copylog <db> <master> <dir>
  mkdir -p "$3" || return 1
  cub_admin copylogdb -L "$3" -m async --start-page-id=-1 "$1@$2" 2>&1 </dev/null
}

node_main() {
  local action=$1 rc
  shift
  node_env_bootstrap
  echo "@@BEGIN"
  case $action in
    info) node_info ;;
    resolve) node_resolve "$@" ;;
    free) node_free_bytes "$@" ;;
    writable) node_writable "$@" ;;
    mode) node_mode "$@" ;;
    conf_edit) node_conf_edit "$@" ;;
    dbtxt_put) node_dbtxt_put "$@" ;;
    dbtxt_add_hosts) node_dbtxt_add_hosts "$@" ;;
    undo_file) node_undo_file "$@" ;;
    drop_backups) node_drop_backups "$@" ;;
    server_start) node_server_start "$@" ;;
    server_stop) node_server_stop "$@" ;;
    master_down) node_master_down ;;
    master_start) node_master_start ;;
    ha_start) node_ha_start ;;
    ha_stop) node_ha_stop ;;
    reload) node_reload ;;
    repl) node_repl "$@" ;;
    hb_status) node_hb_status ;;
    exists) node_exists "$@" ;;
    keep_old) node_keep_old "$@" ;;
    move_back) node_move_back "$@" ;;
    clear) node_clear "$@" ;;
    remove) node_remove "$@" ;;
    rmdir_empty) rmdir "$1" 2>/dev/null; true ;;
    mkdir) mkdir -p "$@" ;;
    restore) node_restore "$@" ;;
    first_copylog) node_first_copylog "$@" ;;
    cubrid) node_cubrid "$@" ;;
    *) echo "unknown node action $action"; false ;;
  esac
  rc=$?
  echo "@@END $rc"
  return $rc
}

if [ "${1:-}" = node ]; then
  shift
  node_main "$@" </dev/null
  exit $?
fi

# --------------------------------------------------------------------------- orchestration (on this server, L)

umask 077
[ -n "${CUBRID:-}" ] || { echo "$PROG: CUBRID is not set" >&2; exit 2; }
CM_SSH="$CUBRID/bin/cm_ssh"
CANCEL=0
IN_ROLLBACK=0

usage() { sed -n '/^# ha_add_nodes.sh - /,/^# Every value/p' "$SELF" | sed '$d; s/^# \{0,1\}//'; }

ACTION=${1:-}
shift || true
JOB_DIR="" AGENT="" MASTER_HOST="" HA_GROUP=cubrid HA_PORT_ID=59901 REPL_HOME="" BK_PATH="" RBK_PATH=""
CONFIRM_RESTART=0 CONFIRM_OTHERS=0 CONFIRM_NEWDB=0 ROLLBACK_CONVERT=0 START_HA=1
DBS_ARG=() NEW_SPEC=() EXIST_SPEC=() KEEP_ARG=()
while [ $# -gt 0 ]; do
  case $1 in
    --job-dir) JOB_DIR=$2; shift 2 ;;
    --agent) AGENT=$2; shift 2 ;;
    --master-host) MASTER_HOST=$2; shift 2 ;;
    --db) DBS_ARG+=("$2"); shift 2 ;;
    --confirm-master-restart) CONFIRM_RESTART=1; shift ;;
    --confirm-other-db-restart) CONFIRM_OTHERS=1; shift ;;
    --confirm-new-node-db-restart) CONFIRM_NEWDB=1; shift ;;
    --new) NEW_SPEC+=("$2"); shift 2 ;;
    --existing) EXIST_SPEC+=("$2"); shift 2 ;;
    --repl-log-home) REPL_HOME=$2; shift 2 ;;
    --backup-path) BK_PATH=$2; shift 2 ;;
    --remote-backup-path) RBK_PATH=$2; shift 2 ;;
    --ha-group) HA_GROUP=$2; shift 2 ;;
    --ha-port-id) HA_PORT_ID=$2; shift 2 ;;
    --rollback-convert) ROLLBACK_CONVERT=1; shift ;;
    --no-start-ha) START_HA=0; shift ;;
    --keep) KEEP_ARG+=("$2"); shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "$PROG: unknown argument $1" >&2; usage >&2; exit 2 ;;
  esac
done
case $ACTION in plan|run|rollback|cleanup) ;; *) usage >&2; exit 2 ;; esac
[ -n "$JOB_DIR" ] || { echo "$PROG: --job-dir is required" >&2; exit 2; }
valid_path "$JOB_DIR" || { echo "$PROG: bad --job-dir" >&2; exit 2; }
mkdir -p "$JOB_DIR" || exit 2
UUID=$(basename "$JOB_DIR")
TAG="$UUID"
LOG="$JOB_DIR/log"
LOCK="$CUBRID/tmp/ha_add_nodes/lock"
mkdir -p "$(dirname "$LOCK")"

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*" >> "$LOG"; }

# ------------------------------------------------ job and step state files (read by the manager)

declare -A JOB=()
job_write() {
  local k tmp="$JOB_DIR/job.tmp"
  : > "$tmp"
  for k in "${!JOB[@]}"; do printf '%s=%s\n' "$k" "$(one_line "${JOB[$k]}")" >> "$tmp"; done
  mv -f "$tmp" "$JOB_DIR/job"
}
job_set() { JOB[$1]=$2; job_write; }

S_ID=() S_TITLE=() S_NODE=() S_DB=() S_STATE=() S_DETAIL=() S_STAGE=()
steps_write() {
  local i tmp="$JOB_DIR/steps.tmp"
  : > "$tmp"
  for i in "${!S_ID[@]}"; do
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$((i + 1))" "${S_ID[$i]}" "${S_TITLE[$i]}" "${S_NODE[$i]}" "${S_DB[$i]}" \
      "${S_STATE[$i]}" "$(one_line "${S_DETAIL[$i]}")" "${S_STAGE[$i]:-}" >> "$tmp"
  done
  mv -f "$tmp" "$JOB_DIR/steps"
}
step_add() { S_STAGE+=("$1"); S_ID+=("$2"); S_TITLE+=("$3"); S_NODE+=("$4"); S_DB+=("$5"); S_STATE+=(pending); S_DETAIL+=(""); }
step_set() { S_STATE[$1]=$2; [ $# -ge 3 ] && S_DETAIL[$1]=$3; steps_write; }

# ------------------------------------------------ lock: content-based, flock only while the file is changed

lock_edit() {   # lock_edit <function>: runs the function with the lock file content in $LOCKED, under flock
  local rc
  exec {lfd}>>"$LOCK"
  flock -x "$lfd"
  LOCKED=$(cat "$LOCK" 2>/dev/null)
  "$1"
  rc=$?
  exec {lfd}>&-
  return $rc
}
lock_in_use() {   # 0 when another job holds the lock
  local w u p t
  read -r w u p t <<< "$LOCKED"
  case $w in
    running) [ "$u" != "$UUID" ] && [ "$(proc_start "$p")" = "$t" ] ;;
    starting) [ "$u" != "$UUID" ] && [ $(( $(epoch) - p )) -lt 60 ] ;;
    *) return 1 ;;
  esac
}
lock_take_fn() {
  if lock_in_use; then return 1; fi
  printf 'running %s %s %s\n' "$UUID" "$$" "$(proc_start $$)" > "$LOCK"
}
lock_free_fn() { case $LOCKED in "running $UUID "*) : > "$LOCK" ;; esac; }

# ------------------------------------------------ agent and remote execution

AGENT_STARTED=0
start_own_agent() {   # from a terminal: start the agent, which asks for the passwords
  local specs=() s out
  for s in "${EXIST_SPEC[@]}" "${NEW_SPEC[@]#*:}"; do specs+=("$s"); done
  AGENT="$JOB_DIR/agent.$$.sock"
  out=$("$CM_SSH" agent --socket "$AGENT" --log "$JOB_DIR/agent.log" --ask "${specs[@]}" </dev/tty) || return 1
  AGENT_STARTED=1
  "$CM_SSH" bind --agent "$AGENT" "$$" >/dev/null || return 1
  log "ssh agent started (${out})"
}

declare -A NODE_LOCAL=() NODE_INFO=()
OUT="" RC=0
# Each command runs in the background so a cancel is noticed at once. interruptible: on cancel the client is
# stopped (the agent then drops the transfer); otherwise the command is waited for, up to its limit.
proc_alive() { local st; st=$(cat "/proc/$1/stat" 2>/dev/null) || return 1; st=${st##*) }; [ "${st%% *}" != Z ]; }
run_bg() {   # run_bg <limit seconds> <interruptible 0|1> <command...> ; sets OUT, RC
  local limit=$1 intr=$2 pid t0 tmp noted=0
  shift 2
  tmp=$(mktemp "$JOB_DIR/out.XXXXXX")
  # an asynchronous command gets /dev/null as stdin unless it is given one explicitly
  "$@" < "${RUN_IN:-/dev/null}" > "$tmp" 2>&1 &
  pid=$!
  t0=$(epoch)
  while proc_alive "$pid"; do
    if [ "$CANCEL" = 1 ] && [ "$IN_ROLLBACK" = 0 ]; then
      if [ "$intr" = 1 ]; then
        kill "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
        OUT=$(cat "$tmp"); rm -f "$tmp"; RC=130
        return 130
      fi
      if [ $noted = 0 ]; then
        noted=1
        job_set detail "cancel pending: waiting for $(one_line "$*" | cut -c1-150)"
      fi
    fi
    if [ $(( $(epoch) - t0 )) -gt "$limit" ]; then
      kill "$pid" 2>/dev/null
      wait "$pid" 2>/dev/null
      OUT=$(cat "$tmp"); rm -f "$tmp"; RC=124
      return 124
    fi
    sleep 0.2
  done
  wait "$pid"
  RC=$?
  OUT=$(cat "$tmp")
  rm -f "$tmp"
  return $RC
}

# on_node <host> <limit> <interruptible> <action> [args...] : the node action's output in OUT, status in RC
on_node() {
  local host=$1 limit=$2 intr=$3
  shift 3
  # node actions start their daemons detached (setsid, own stdin/stdout), so the channel is not held open
  log "$host: $*"
  if [ "${NODE_LOCAL[$host]:-0}" = 1 ]; then
    run_bg "$limit" "$intr" bash "$SELF" node "$@"
  else
    local q="" a
    for a in "$@"; do q+=" $(printf '%q' "$a")"; done
    RUN_IN=$SELF run_bg "$limit" "$intr" "$CM_SSH" exec --agent "$AGENT" "$host" --stdin -- \
      "f=\$(mktemp) && cat >\"\$f\" && bash -l \"\$f\" node$q </dev/null; rc=\$?; rm -f \"\$f\"; exit \$rc"
  fi
  local full=$OUT body
  body=$(printf '%s\n' "$full" | awk '/^@@BEGIN$/ { on=1; next } /^@@END / { on=0; next } on')
  printf '%s\n' "$full" | sed 's/^/  /' >> "$LOG"
  if [ "$RC" -eq 124 ] || [ "$RC" -eq 130 ]; then OUT=$body; return $RC; fi
  if ! printf '%s\n' "$full" | grep -q '^@@END '; then
    OUT=$(printf '%s' "$full" | grep '^cm_ssh: ' | tail -1)
    [ -n "$OUT" ] || OUT="no result from $host"
    RC=255
    return 255
  fi
  OUT=$body
  return "$RC"
}

kv() { printf '%s\n' "$1" | sed -n "s/^$2=//p" | head -1; }

# ------------------------------------------------ undo log

UNDO="$JOB_DIR/undo"
undo_add() {   # undo_add <step no> <host> <kind> [args...] (no tabs in values)
  local n
  n=$(( $(grep -c '^U' "$UNDO" 2>/dev/null || echo 0) + 1 ))
  local IFS=$'\t'
  printf 'U\t%s\t%s\t%s\t%s\n' "$n" "$1" "$2" "${*:3}" >> "$UNDO"
}

# ------------------------------------------------ failure and cancel

FAIL_CODE="" FAIL_MSG=""
fail() { FAIL_CODE=$1; FAIL_MSG=$2; return 1; }
trap 'CANCEL=1; log "cancel requested"' TERM INT

# ------------------------------------------------ inputs

NEW_HOST=() NEW_ROLE=() NEW_PORT=() NEW_USER=()
EX_HOST=() EX_PORT=() EX_USER=() EX_ROLE=()
parse_inputs() {
  local s r h p u
  for s in "${NEW_SPEC[@]}"; do
    IFS=: read -r r h p u _ <<< "$s"
    [ "$r" = slave ] || [ "$r" = replica ] || { fail usage "bad --new $s (role slave or replica)"; return 1; }
    valid_name "$h" && [[ $p =~ ^[0-9]{1,5}$ ]] && valid_name "$u" || { fail usage "bad --new $s"; return 1; }
    NEW_ROLE+=("$r"); NEW_HOST+=("$h"); NEW_PORT+=("$p"); NEW_USER+=("$u")
  done
  for s in "${EXIST_SPEC[@]}"; do
    IFS=: read -r h p u _ <<< "$s"
    valid_name "$h" && [[ $p =~ ^[0-9]{1,5}$ ]] && valid_name "$u" || { fail usage "bad --existing $s"; return 1; }
    EX_HOST+=("$h"); EX_PORT+=("$p"); EX_USER+=("$u")
  done
  for s in "${DBS_ARG[@]}"; do valid_name "$s" || { fail usage "bad --db $s"; return 1; }; done
  for s in "$REPL_HOME" "$BK_PATH" "$RBK_PATH"; do
    [ -z "$s" ] || valid_path "$s" || { fail usage "bad path $s"; return 1; }
  done
  valid_name "$HA_GROUP" && [[ $HA_PORT_ID =~ ^[0-9]{1,5}$ ]] || { fail usage "bad --ha-group or --ha-port-id"; return 1; }
  case $ACTION in plan|run) [ ${#NEW_HOST[@]} -ge 1 ] || { fail usage "no new node (--new)"; return 1; } ;; esac
  local all=" " x
  for x in "${NEW_HOST[@]}" "${EX_HOST[@]}"; do
    [[ $all == *" $x "* ]] && { fail usage "$x is listed twice"; return 1; }
    all+="$x "
  done
  return 0
}

# "group@h1:h2" helpers
list_group() { printf '%s' "${1%%@*}"; }
list_hosts() { local l=${1#*@}; printf '%s' "${l//:/ }"; }
join_by() { local IFS=$1; shift; printf '%s' "$*"; }

# ------------------------------------------------ S1 precheck

L=$(uname -n)
LOCAL_STATE="" CASE_NO=0 VERSION=""
D=() NODE_LIST="" REPLICA_LIST="" SYNC_MODE="" HA_CONF_LOCAL="" UP_BEFORE=() OTHERS_UP=() SINGLES=()
SRC_STATE=master
NEW_SLAVES=() NEW_REPLICAS=()
declare -A DB_VOL=() DB_LOG=() DB_LOB=() DB_HOSTS=()
WARN=()

st_precheck() {
  MASTER_HOST=${MASTER_HOST:-$L}
  [ "$MASTER_HOST" = "$L" ] || { fail precheck_failed "--master-host $MASTER_HOST is not this server's host name ($L)"; return 1; }
  NODE_LOCAL[$L]=1
  on_node "$L" 60 0 info || { fail precheck_failed "cannot read this server: $OUT"; return 1; }
  NODE_INFO[$L]=$OUT
  VERSION=$(kv "$OUT" VERSION)
  [ -n "$VERSION" ] || { fail precheck_failed "cannot read the CUBRID version here"; return 1; }
  local mode db dbtxt="$CUBRID_DATABASES/databases.txt" i x
  mode=$(kv "$OUT" HA_MODE)
  read -ra UP_BEFORE <<< "$(kv "$OUT" SERVERS)"
  case $mode in
    on|yes)
      [ ${#DBS_ARG[@]} -eq 0 ] || { fail usage "--db is for a single server; here the databases are ha_db_list"; return 1; }
      NODE_LIST=$(kv "$OUT" HA_NODE_LIST); REPLICA_LIST=$(kv "$OUT" HA_REPLICA_LIST); SYNC_MODE=$(kv "$OUT" HA_SYNC)
      read -ra D <<< "$(kv "$OUT" HA_DB_LIST | tr ',:' '  ')"
      [ ${#D[@]} -ge 1 ] || { fail precheck_failed "ha_db_list is empty"; return 1; }
      on_node "$L" 120 0 mode "${D[@]}"
      if [[ $OUT == *" standby"* ]]; then LOCAL_STATE=slave; else LOCAL_STATE=master; fi
      for db in "${D[@]}"; do
        if [[ $LOCAL_STATE = master && $OUT != *"$db active"* ]]; then
          fail master_not_active "$db is not the active master here: $(printf '%s\n' "$OUT" | grep "^$db ")"; return 1
        fi
      done
      ;;
    replica) LOCAL_STATE=replica ;;
    *) LOCAL_STATE=single ;;
  esac
  case $LOCAL_STATE in
    slave) CASE_NO=5; fail unsupported_case "adding nodes from a slave is not supported yet (run it on the master)"; return 1 ;;
    replica) CASE_NO=6; fail unsupported_case "adding nodes from a replica is not supported yet (run it on the master)"; return 1 ;;
  esac
  for i in "${!NEW_HOST[@]}"; do
    [ "${NEW_ROLE[$i]}" = slave ] && NEW_SLAVES+=("${NEW_HOST[$i]}") || NEW_REPLICAS+=("${NEW_HOST[$i]}")
  done
  if [ "$LOCAL_STATE" = single ]; then
    [ ${#DBS_ARG[@]} -ge 1 ] || { fail usage "this server is not HA: give the databases to put into HA with --db"; return 1; }
    [ -z "$(kv "${NODE_INFO[$L]}" HA_NODE_LIST)" ] || { fail precheck_failed "cubrid_ha.conf already has ha_node_list; fix the HA settings by hand first"; return 1; }
    [ ${#EX_HOST[@]} -eq 0 ] || { fail usage "a single server has no existing nodes"; return 1; }
    D=("${DBS_ARG[@]}")
    for db in "${D[@]}"; do
      [ -z "$(conf_get "$CUBRID/conf/cubrid.conf" "[@$db]" ha_mode)" ] || { fail precheck_failed "cubrid.conf sets ha_mode in [@$db]; remove it first"; return 1; }
    done
    NODE_LIST="$HA_GROUP@$L"; REPLICA_LIST=""; SYNC_MODE=sync
    OTHERS_UP=() SINGLES=()
    for x in "${UP_BEFORE[@]}"; do [[ " ${D[*]} " == *" $x "* ]] || OTHERS_UP+=("$x"); done
    while read -r x _; do
      [ -n "$x" ] && [[ $x != \#* ]] && [[ " ${D[*]} " != *" $x "* ]] && [[ " ${SINGLES[*]} " != *" $x "* ]] && SINGLES+=("$x")
    done < "$dbtxt"
    if [ ${#NEW_SLAVES[@]} -ge 1 ]; then CASE_NO=1; else CASE_NO=2; fi
  else
    if [ ${#NEW_SLAVES[@]} -ge 1 ]; then CASE_NO=3; else CASE_NO=4; fi
    # the existing nodes must be exactly the other members of both lists
    local want=() h
    for h in $(list_hosts "$NODE_LIST"); do [ "$h" = "$L" ] || want+=("$h"); done
    [ -z "$REPLICA_LIST" ] || for h in $(list_hosts "$REPLICA_LIST"); do want+=("$h"); done
    for h in "${want[@]}"; do
      [[ " ${EX_HOST[*]} " == *" $h "* ]] || { fail precheck_failed "$h is in this server's HA lists; give it with --existing so it learns the new nodes"; return 1; }
    done
    for h in "${EX_HOST[@]}"; do
      [[ " ${want[*]} " == *" $h "* ]] || { fail precheck_failed "$h is not in this server's ha_node_list ($NODE_LIST) or ha_replica_list ($REPLICA_LIST)"; return 1; }
    done
    for h in "${NEW_HOST[@]}"; do
      [[ " $L ${want[*]} " != *" $h "* ]] || { fail precheck_failed "$h is already in the HA lists"; return 1; }
    done
    for i in "${!EX_HOST[@]}"; do
      if [[ " $(list_hosts "$NODE_LIST") " == *" ${EX_HOST[$i]} "* ]]; then EX_ROLE[$i]=slave; else EX_ROLE[$i]=replica; fi
    done
  fi
  for db in "${D[@]}"; do
    local line
    line=$(awk -v db="$db" '{ t=$0; sub(/^[ \t]+/, "", t); split(t, g, /[ \t]+/) } t !~ /^#/ && g[1] == db { print t; exit }' "$dbtxt")
    read -r _ DB_VOL[$db] DB_HOSTS[$db] DB_LOG[$db] DB_LOB[$db] <<< "$line"
    valid_path "${DB_VOL[$db]:-x}" && valid_path "${DB_LOG[$db]:-x}" || { fail precheck_failed "databases.txt has no usable line for $db"; return 1; }
    if grep -qs "^$db[[:space:]]" "$CUBRID/conf/autobackupdb.conf" "$CUBRID/conf/autoaddvoldb.conf"; then
      WARN+=("$db has automatic backup or volume adding set in the manager; avoid running them while this job runs")
    fi
  done
  # HA replicates only tables with a primary key; listing the others needs a database login, which this
  # script does not get (the manager can check with the database account the screen logged in with)
  WARN+=("tables without a primary key are not replicated; not checked here (no database account): ${D[*]}")
  BK_PATH=${BK_PATH:-$CUBRID_DATABASES/ha_add_nodes_backup}
  # backup room here: the size of the databases' volumes
  local need=0 free sz
  for db in "${D[@]}"; do sz=$(du -sk "${DB_VOL[$db]}" 2>/dev/null | awk '{ print $1 * 1024 }'); need=$(( need + ${sz:-0} )); done
  free=$(node_free_bytes "$BK_PATH")
  JOB[backup_bytes]=$need
  [ "${free:-0}" -gt "$need" ] || { fail precheck_failed "not enough room in $BK_PATH (${free:-0} bytes free, about $need needed)"; return 1; }
  for i in "${!EX_HOST[@]}"; do NODE_LOCAL[${EX_HOST[$i]}]=0; done
  for i in "${!NEW_HOST[@]}"; do NODE_LOCAL[${NEW_HOST[$i]}]=0; done
  log "this server: $L ($LOCAL_STATE), CUBRID $VERSION, databases ${D[*]}, case $CASE_NO"
  return 0
}

# ------------------------------------------------ S2 node checks

declare -A N_CUBDB=() N_HOME=() N_MASTER=() N_COPYBASE=() N_OTHERS_UP=() N_OTHERS_REG=() NV=() NLG=() NLOB=()

# a path of this server rebased onto a new node: under its $CUBRID_DATABASES if it is under ours, else as it is
node_path() {   # node_path <host> <path>
  local p=$2
  case $p in
    "$CUBRID_DATABASES") printf '%s' "${N_CUBDB[$1]}" ;;
    "$CUBRID_DATABASES"/*) printf '%s' "${N_CUBDB[$1]}${p#"$CUBRID_DATABASES"}" ;;
    *) printf '%s' "$p" ;;
  esac
}
rbk_root() { printf '%s' "${RBK_PATH:-${N_CUBDB[$1]}/ha_add_nodes_backup}"; }
all_names() { printf '%s ' "$L" "${EX_HOST[@]}" "${NEW_HOST[@]}"; }

st_node_check() {   # st_node_check <host> <existing|new> <role>
  local h=$1 kind=$2 role=$3 db
  if ! on_node "$h" 60 0 info; then
    local code
    code=$(sed -n 's/^cm_ssh: \([a-z_]*\):.*/\1/p' <<< "$OUT")
    fail "${code:-unreachable}" "$h: $OUT"
    return 1
  fi
  NODE_INFO[$h]=$OUT
  [ "$(kv "$OUT" HOSTNAME)" = "$h" ] || { fail precheck_failed "$h: its host name is $(kv "$OUT" HOSTNAME); the HA node name must be the same"; return 1; }
  [ -n "$(kv "$OUT" CUBRID)" ] || { fail cubrid_not_found "$h: CUBRID was not found for this account"; return 1; }
  [ "$(kv "$OUT" VERSION)" = "$VERSION" ] || { fail version_mismatch "$h: CUBRID $(kv "$OUT" VERSION), this server $VERSION"; return 1; }
  N_CUBDB[$h]=$(kv "$OUT" CUBRID_DATABASES)
  N_MASTER[$h]=$(kv "$OUT" CUB_MASTER)
  local servers
  servers=$(kv "$OUT" SERVERS)
  on_node "$h" 60 0 resolve $(all_names) || { fail precheck_failed "$h: $OUT"; return 1; }
  [ -z "$(kv "$OUT" UNRESOLVED)" ] || { fail precheck_failed "$h cannot resolve: $(kv "$OUT" UNRESOLVED)"; return 1; }
  if [ "$kind" = existing ]; then
    local info=${NODE_INFO[$h]}
    [ "$(kv "$info" HA_NODE_LIST)" = "$NODE_LIST" ] || { fail precheck_failed "$h: ha_node_list is $(kv "$info" HA_NODE_LIST), this server has $NODE_LIST (they must be the same, in the same order)"; return 1; }
    [ "$(kv "$info" HA_REPLICA_LIST)" = "$REPLICA_LIST" ] || { fail precheck_failed "$h: ha_replica_list is $(kv "$info" HA_REPLICA_LIST), this server has $REPLICA_LIST"; return 1; }
    on_node "$h" 120 0 mode "${D[@]}"
    for db in "${D[@]}"; do
      [[ $OUT == *"$db standby"* ]] || { fail precheck_failed "$h: $db is not standby ($(printf '%s\n' "$OUT" | grep "^$db "))"; return 1; }
    done
    N_COPYBASE[$h]=$(kv "$info" HA_COPY_LOG_BASE); N_COPYBASE[$h]=${N_COPYBASE[$h]:-${N_CUBDB[$h]}}
  else
    for db in "${D[@]}"; do
      [[ " $servers " != *" $db "* ]] || { fail slave_db_running "$h: database $db is running there; stop it first"; return 1; }
    done
    # other databases of the node stay single: [@db] ha_mode=off; the running ones restart with the HA start
    local x reg=""
    N_OTHERS_UP[$h]=""
    for x in $servers; do [[ " ${D[*]} " == *" $x "* ]] || N_OTHERS_UP[$h]+="$x "; done
    for x in $(kv "${NODE_INFO[$h]}" DBTXT); do [[ " ${D[*]} " == *" $x "* ]] || reg+="$x "; done
    N_OTHERS_REG[$h]=$reg
    N_HOME[$h]=${REPL_HOME:-${N_CUBDB[$h]}}
    N_COPYBASE[$h]=${N_HOME[$h]}
    # paths on this node: under its own $CUBRID_DATABASES when the master's are under the master's
    for db in "${D[@]}"; do
      NV[$h/$db]=$(node_path "$h" "${DB_VOL[$db]}")
      NLG[$h/$db]=$(node_path "$h" "${DB_LOG[$db]}")
      NLOB[$h/$db]=""
      [ -n "${DB_LOB[$db]}" ] && NLOB[$h/$db]="file:$(node_path "$h" "${DB_LOB[$db]#file:}")"
    done
  fi
  return 0
}

# ------------------------------------------------ S3 preflight: what the later steps write is tried first

st_preflight() {   # <host>: L tries its backup folder; a new node its receiving folder (a small transfer),
                   # the folders the restore writes to and the replication log folder
  local h=$1 db need=0 sz p r
  if [ "$h" = "$L" ]; then
    on_node "$L" 30 0 writable "$BK_PATH"
    [[ $OUT == OK* ]] || { fail precheck_failed "cannot write $BK_PATH ($(one_line "$OUT"))"; return 1; }
    return 0
  fi
  r=$(rbk_root "$h")
  local paths=("$r" "${N_HOME[$h]}")
  for db in "${D[@]}"; do paths+=("${NV[$h/$db]}" "${NLG[$h/$db]}"); done
  on_node "$h" 30 0 writable "${paths[@]}"
  if [[ $OUT == *"NO "* ]]; then
    fail precheck_failed "$h cannot write: $(printf '%s\n' "$OUT" | awk '$1 == "NO" { printf "%s (%s is not writable) ", $2, $3 }')"
    return 1
  fi
  # room: the backup arrives, then the restore writes about the same again
  need=$(( ${JOB[backup_bytes]:-0} ))
  p=$(printf '%s\n' "$OUT" | awk -v r="$r" '$1 == "OK" && $2 == r { print $3 }')
  [ "${p:-0}" -gt "$need" ] || { fail precheck_failed "$h: not enough room for the backup under $r (${p:-0} bytes free, about $need needed)"; return 1; }
  # a small transfer into a folder of its own, removed again
  local t="$r/$TAG.preflight" made=0
  on_node "$h" 30 0 exists "$r"; [[ $OUT == *"MISSING"* ]] && made=1
  on_node "$h" 30 0 mkdir "$t" || { fail transfer_failed "$h: cannot create $t"; return 1; }
  run_bg 120 1 "$CM_SSH" put --agent "$AGENT" "$h" "$SELF" "$t/transfer-test"
  local prc=$RC pout=$OUT
  on_node "$h" 30 0 remove "$t"
  [ $made = 1 ] && on_node "$h" 30 0 rmdir_empty "$r"
  [ "$prc" -eq 0 ] || { fail transfer_failed "$h: a test transfer failed: $(printf '%s' "$pout" | grep '^cm_ssh: ' | tail -1)"; return 1; }
  return 0
}

# ------------------------------------------------ lists after the job

final_node_list() { local l=$NODE_LIST x; for x in "${NEW_SLAVES[@]}"; do l+=":$x"; done; printf '%s' "$l"; }
final_replica_list() {
  local l=$REPLICA_LIST x
  for x in "${NEW_REPLICAS[@]}"; do
    if [ -z "$l" ]; then l="$(list_group "$NODE_LIST")@$x"; else l+=":$x"; fi
  done
  printf '%s' "$l"
}
final_sync() {   # one value per ha_node_list node: the new slaves take the last value
  local s=$SYNC_MODE last x
  [ -n "$s" ] || { printf ''; return; }
  last=${s##*:}
  for x in "${NEW_SLAVES[@]}"; do s+=":$last"; done
  printf '%s' "$s"
}

# ------------------------------------------------ B: promotion to HA master (single server only)

st_convert_stop() {
  local no=$1
  JOB[down_at]=$(epoch); job_write
  undo_add "$no" "$L" server_start "${UP_BEFORE[@]}"
  [ ${#UP_BEFORE[@]} -eq 0 ] && return 0
  log "promotion to HA master: service interruption starts, ${UP_BEFORE[*]} stop"
  on_node "$L" 600 0 server_stop "${UP_BEFORE[@]}" || { fail command_failed "cubrid server stop failed: $(one_line "$OUT")"; return 1; }
}

st_convert_config() {
  local no=$1 s args=()
  undo_add "$no" "$L" undo_file "$CUBRID/conf/cubrid.conf" "$TAG.$no"
  args=("[common]" ha_mode on "[common]" force_remove_log_archives no)
  for s in "${SINGLES[@]}"; do args+=("[@$s]" ha_mode off); done
  on_node "$L" 60 0 conf_edit "$CUBRID/conf/cubrid.conf" "$TAG.$no" "${args[@]}" || { fail config_edit_failed "cubrid.conf: $OUT"; return 1; }
  undo_add "$no" "$L" undo_file "$CUBRID/conf/cubrid_ha.conf" "$TAG.$no"
  on_node "$L" 60 0 conf_edit "$CUBRID/conf/cubrid_ha.conf" "$TAG.$no" "[common]" ha_port_id "$HA_PORT_ID" \
    "[common]" ha_node_list "$NODE_LIST" "[common]" ha_db_list "$(join_by , "${D[@]}")" "[common]" ha_copy_sync_mode sync \
    || { fail config_edit_failed "cubrid_ha.conf: $OUT"; return 1; }
  undo_add "$no" "$L" undo_file "$CUBRID_DATABASES/databases.txt" "$TAG.$no"
  local db
  for db in "${D[@]}"; do
    on_node "$L" 60 0 dbtxt_put "$TAG.$no" "$db" "${DB_VOL[$db]}" "$L" "${DB_LOG[$db]}" "${DB_LOB[$db]}" || { fail config_edit_failed "databases.txt: $OUT"; return 1; }
    DB_HOSTS[$db]=$L
  done
  [ "$(conf_get "$CUBRID/conf/cubrid.conf" "[common]" ha_mode)" = on ] || { fail config_edit_failed "cubrid.conf did not read back with ha_mode=on"; return 1; }
}

st_convert_restart() {
  local no=$1
  undo_add "$no" "$L" ha_stop
  on_node "$L" 120 0 master_down || { fail command_failed "cub_master did not stop"; return 1; }
  on_node "$L" 600 0 ha_start || { fail command_failed "cubrid heartbeat start failed: $(one_line "$OUT")"; return 1; }
}

st_convert_verify() {
  local i db waiting=("${D[@]}") still
  for i in $(seq 1 90); do
    on_node "$L" 120 0 mode "${waiting[@]}"
    still=()
    for db in "${waiting[@]}"; do [[ $OUT == *"$db active"* ]] || still+=("$db"); done
    waiting=("${still[@]}")
    if [ ${#waiting[@]} -eq 0 ]; then
      JOB[downtime_sec]=$(( $(epoch) - ${JOB[down_at]} )); job_write
      log "service interruption ends: ${D[*]} active after ${JOB[downtime_sec]} s"
      return 0
    fi
    [ "$CANCEL" = 1 ] && { fail cancelled cancelled; return 1; }
    sleep 2
  done
  fail verify_failed "not the active master: ${waiting[*]}"
}

st_convert_start_others() {
  local no=$1
  [ ${#OTHERS_UP[@]} -eq 0 ] && { log "no other databases"; return 0; }
  undo_add "$no" "$L" server_stop "${OTHERS_UP[@]}"
  on_node "$L" 600 0 server_start "${OTHERS_UP[@]}" || { fail command_failed "cubrid server start (without HA) failed: $(one_line "$OUT")"; return 1; }
  log "running again without HA: ${OTHERS_UP[*]}"
}

# ------------------------------------------------ C: new nodes

st_new_config() {   # <no> <host> <role>
  local no=$1 h=$2 role=$3 cubrid cubdb k v args=() db hosts x
  cubrid=$(kv "${NODE_INFO[$h]}" CUBRID); cubdb=${N_CUBDB[$h]}
  # what HA start takes down comes back once the settings are back: first cub_master (if it ran without HA),
  # then the other databases that ran (the undo log runs backwards, so they are recorded the other way round)
  if [ "$START_HA" = 1 ]; then
    [ -n "${N_OTHERS_UP[$h]// /}" ] && undo_add "$no" "$h" server_start ${N_OTHERS_UP[$h]}
    [ "${N_MASTER[$h]}" = yes ] && undo_add "$no" "$h" master_start
  fi
  undo_add "$no" "$h" undo_file "$cubrid/conf/cubrid.conf" "$TAG.$no"
  if [ "$role" = slave ]; then
    args=("[common]" ha_mode on "[common]" force_remove_log_archives no)
  else
    args=("[common]" ha_mode replica)
  fi
  for x in ${N_OTHERS_REG[$h]}; do args+=("[@$x]" ha_mode off); done
  on_node "$h" 60 0 conf_edit "$cubrid/conf/cubrid.conf" "$TAG.$no" "${args[@]}" || { fail config_edit_failed "$h cubrid.conf: $OUT"; return 1; }
  # the HA settings of this server (ha_port_id, ha_apply_max_mem_size, ...) with the final lists, in the same order
  args=()
  while IFS=$'\t' read -r k v; do
    case $k in ha_node_list|ha_replica_list|ha_db_list|ha_copy_sync_mode|ha_copy_log_base|"") continue ;; esac
    args+=("[common]" "$k" "$v")
  done < <(conf_pairs "$CUBRID/conf/cubrid_ha.conf" "[common]")
  args+=("[common]" ha_node_list "$(final_node_list)" "[common]" ha_db_list "$(join_by , "${D[@]}")")
  [ -n "$(final_replica_list)" ] && args+=("[common]" ha_replica_list "$(final_replica_list)")
  [ -n "$(final_sync)" ] && args+=("[common]" ha_copy_sync_mode "$(final_sync)")
  [ "${N_HOME[$h]}" != "$cubdb" ] && args+=("[common]" ha_copy_log_base "${N_HOME[$h]}")
  undo_add "$no" "$h" undo_file "$cubrid/conf/cubrid_ha.conf" "$TAG.$no"
  on_node "$h" 60 0 conf_edit "$cubrid/conf/cubrid_ha.conf" "$TAG.$no" "${args[@]}" || { fail config_edit_failed "$h cubrid_ha.conf: $OUT"; return 1; }
  # restoreslave computes the copied log path from this folder (realpath): it has to exist before the restore
  on_node "$h" 30 0 exists "${N_HOME[$h]}"
  if [[ $OUT == *MISSING* ]]; then
    undo_add "$no" "$h" rmdir_empty "${N_HOME[$h]}"
    on_node "$h" 30 0 mkdir "${N_HOME[$h]}" || { fail command_failed "$h: cannot create ${N_HOME[$h]}"; return 1; }
  fi
  undo_add "$no" "$h" undo_file "$cubdb/databases.txt" "$TAG.$no"
  for db in "${D[@]}"; do
    hosts=$(list_hosts "$(final_node_list)"); hosts=${hosts// /:}
    for x in ${DB_HOSTS[$db]//:/ }; do [[ :$hosts: == *":$x:"* || $x = localhost ]] || hosts+=":$x"; done
    on_node "$h" 60 0 dbtxt_put "$TAG.$no" "$db" "${NV[$h/$db]}" "$hosts" "${NLG[$h/$db]}" "${NLOB[$h/$db]}" || { fail config_edit_failed "$h databases.txt: $OUT"; return 1; }
  done
  log "$h ($role): ha_node_list=$(final_node_list) ha_replica_list=$(final_replica_list)"
}

declare -A PARENT_RECORDED=()
st_backup() {   # <no> <db>
  local no=$1 db=$2 dir="$BK_PATH/$TAG/$2"
  if [ -z "${PARENT_RECORDED[$L]:-}" ]; then
    undo_add "$no" "$L" rmdir_empty "$BK_PATH"
    undo_add "$no" "$L" remove "$BK_PATH/$TAG"
    PARENT_RECORDED[$L]=1
  fi
  undo_add "$no" "$L" remove "$dir"
  mkdir -p "$dir" || { fail command_failed "cannot create $dir"; return 1; }
  chmod 700 "$BK_PATH/$TAG"
  on_node "$L" 21600 0 cubrid backupdb -C -D "$dir" -o "$dir/backup.out" --no-check "$db@localhost" \
    || { fail command_failed "cubrid backupdb $db failed ($RC)"; return 1; }
}

st_send_backup() {   # <no> <host> <db>
  local no=$1 h=$2 db=$3 src="$BK_PATH/$TAG/$3" dst f
  dst="$(rbk_root "$h")/$TAG/$db"
  if [ -z "${PARENT_RECORDED[$h]:-}" ]; then
    undo_add "$no" "$h" rmdir_empty "${dst%/*/*}"
    undo_add "$no" "$h" remove "${dst%/*}"
    PARENT_RECORDED[$h]=1
  fi
  undo_add "$no" "$h" remove "$dst"
  on_node "$h" 60 0 mkdir "$dst" || { fail transfer_failed "$h: cannot create $dst"; return 1; }
  for f in "$src"/* "${DB_LOG[$db]}/${db}_bkvinf"; do
    [ -f "$f" ] || continue
    [ "$(basename "$f")" = backup.out ] && continue
    log "$h: put $f"
    run_bg 21600 1 "$CM_SSH" put --agent "$AGENT" "$h" "$f" "$dst/$(basename "$f")"
    printf '%s\n' "$OUT" | grep -v '^progress [0-9]' | sed 's/^/  /' >> "$LOG"
    [ "$RC" -eq 0 ] || { [ "$RC" -eq 130 ] && { fail cancelled cancelled; return 1; }; fail transfer_failed "$h: $(printf '%s' "$OUT" | grep '^cm_ssh: ' | tail -1)"; return 1; }
  done
}

declare -A CLEAR_AT_RESTORE=()
st_keep_old() {   # <no> <host> <db>
  local no=$1 h=$2 db=$3 l kind p
  on_node "$h" 600 0 keep_old ".ha_add_nodes_$TAG" "$db" "${NV[$h/$db]}" "${NLG[$h/$db]}" "${N_HOME[$h]}"
  local rc=$RC
  while read -r kind p; do
    case $kind in
      MOVED) undo_add "$no" "$h" move_back "$p" "$p.ha_add_nodes_$TAG"; JOB[kept]+=" $h:$p.ha_add_nodes_$TAG" ;;
      CREATED) CLEAR_AT_RESTORE[$h/$db]+="created $p"$'\n' ;;
      EMPTY) CLEAR_AT_RESTORE[$h/$db]+="empty $p"$'\n' ;;
    esac
  done <<< "$OUT"
  job_write
  [ "$rc" -eq 0 ] || { fail command_failed "$h: moving the old $db aside failed: $(one_line "$OUT")"; return 1; }
}

st_restore() {   # <no> <host> <db>
  local no=$1 h=$2 db=$3 dst
  dst="$(rbk_root "$h")/$TAG/$db"
  # what restoreslave writes goes into folders keep_old created or found empty: the undo clears just those
  local how p
  while read -r how p; do [ -n "$p" ] && undo_add "$no" "$h" clear "$how" "$p"; done <<< "${CLEAR_AT_RESTORE[$h/$db]:-}"
  on_node "$h" 21600 0 restore "$db" "$dst" "${NLG[$h/$db]}" "$MASTER_HOST" "$SRC_STATE" \
    || { fail command_failed "$h: cubrid restoreslave $db failed ($RC): $(one_line "$OUT" | tail -c 300)"; return 1; }
}

st_first_copylog() {   # <no> <host> <db>
  local no=$1 h=$2 db=$3 dir="${N_HOME[$2]}/$3_$MASTER_HOST"
  undo_add "$no" "$h" remove "$dir"
  on_node "$h" 3600 0 first_copylog "$db" "$MASTER_HOST" "$dir" \
    || { fail command_failed "$h: copylogdb $db failed ($RC): $(one_line "$OUT" | tail -c 300)"; return 1; }
}

# ------------------------------------------------ D: HA start on the new nodes

st_new_ha_start() {   # <no> <host>
  local no=$1 h=$2 others=${N_OTHERS_UP[$2]}
  # the node's other running databases stop (cub_master restarts in HA mode) and run again without HA
  if [ -n "${others// /}" ]; then
    on_node "$h" 600 0 server_stop $others || { fail command_failed "$h: cubrid server stop $others failed: $(one_line "$OUT")"; return 1; }
  fi
  if [ "${N_MASTER[$h]}" = yes ]; then
    on_node "$h" 120 0 master_down || { fail command_failed "$h: cub_master did not stop"; return 1; }
  fi
  undo_add "$no" "$h" ha_stop
  on_node "$h" 600 0 ha_start || { fail command_failed "$h: cubrid heartbeat start failed: $(one_line "$OUT" | tail -c 300)"; return 1; }
  if [ -n "${others// /}" ]; then
    undo_add "$no" "$h" server_stop $others
    on_node "$h" 600 0 server_start $others || { fail command_failed "$h: cubrid server start $others (without HA) failed: $(one_line "$OUT")"; return 1; }
  fi
}

st_peer_repl_start() {   # <no> <host>: copies from the other new slaves that heartbeat start did not start
  local no=$1 h=$2 x db missing
  on_node "$h" 60 0 hb_status
  for x in "${NEW_SLAVES[@]}"; do
    [ "$x" = "$h" ] && continue
    missing=0
    for db in "${D[@]}"; do [[ $OUT == *"Copylogdb $db@$x:"* ]] || missing=1; done
    [ $missing = 0 ] && continue
    undo_add "$no" "$h" repl stop "$x"
    on_node "$h" 300 0 repl start "$x" || { fail command_failed "$h: cubrid heartbeat repl start $x failed: $(one_line "$OUT")"; return 1; }
  done
}

# ------------------------------------------------ E: existing nodes learn the new ones

st_existing_config() {   # <no> <host>
  local no=$1 h=$2 cubrid cubdb args=() news
  cubrid=$(kv "${NODE_INFO[$h]}" CUBRID); cubdb=${N_CUBDB[$h]:-$CUBRID_DATABASES}
  args=("[common]" ha_node_list "$(final_node_list)")
  [ -n "$(final_replica_list)" ] && args+=("[common]" ha_replica_list "$(final_replica_list)")
  [ -n "$(final_sync)" ] && args+=("[common]" ha_copy_sync_mode "$(final_sync)")
  undo_add "$no" "$h" reload
  undo_add "$no" "$h" undo_file "$cubrid/conf/cubrid_ha.conf" "$TAG.$no"
  on_node "$h" 60 0 conf_edit "$cubrid/conf/cubrid_ha.conf" "$TAG.$no" "${args[@]}" || { fail config_edit_failed "$h cubrid_ha.conf: $OUT"; return 1; }
  if [ ${#NEW_SLAVES[@]} -ge 1 ]; then
    news=$(join_by , "${NEW_SLAVES[@]}")
    undo_add "$no" "$h" undo_file "$cubdb/databases.txt" "$TAG.$no"
    on_node "$h" 60 0 dbtxt_add_hosts "$TAG.$no" "$news" "${D[@]}" || { fail config_edit_failed "$h databases.txt: $OUT"; return 1; }
  fi
  on_node "$h" 300 0 reload || { fail command_failed "$h: cubrid heartbeat reload failed: $(one_line "$OUT")"; return 1; }
  # the reload result is read from heartbeat status, not from the exit code alone
  on_node "$h" 60 0 hb_status
  local x
  for x in "${NEW_HOST[@]}"; do
    [[ $OUT == *"Node $x "* ]] || { fail command_failed "$h: $x does not show in heartbeat status after the reload"; return 1; }
  done
}

st_existing_repl_start() {   # <no> <host>
  local no=$1 h=$2 x db base=${N_COPYBASE[$2]}
  for x in "${NEW_SLAVES[@]}"; do
    local dirs=()
    for db in "${D[@]}"; do dirs+=("$base/${db}_$x"); done
    on_node "$h" 60 0 exists "${dirs[@]}"
    for db in "${D[@]}"; do
      [[ $OUT == *"MISSING $base/${db}_$x"* ]] && undo_add "$no" "$h" remove "$base/${db}_$x"
    done
    undo_add "$no" "$h" repl stop "$x"
    on_node "$h" 300 0 repl start "$x" || { fail command_failed "$h: cubrid heartbeat repl start $x failed: $(one_line "$OUT")"; return 1; }
  done
}

# ------------------------------------------------ F: verify, cleanup

st_verify() {   # <no> <host>: standby, and a log copy from every other ha_node_list node
  local no=$1 h=$2 i db x want=() missing last=""
  for x in $(list_hosts "$(final_node_list)"); do [ "$x" = "$h" ] || want+=("$x"); done
  for i in $(seq 1 40); do
    on_node "$h" 120 0 mode "${D[@]}"
    missing=()
    for db in "${D[@]}"; do [[ $OUT == *"$db standby"* ]] || missing+=("$db not standby"); done
    if [ ${#missing[@]} -eq 0 ]; then
      on_node "$h" 60 0 hb_status
      for db in "${D[@]}"; do
        for x in "${want[@]}"; do [[ $OUT == *"Copylogdb $db@$x:"* ]] || missing+=("no copy of $db from $x"); done
      done
    fi
    [ ${#missing[@]} -eq 0 ] && return 0
    last=$(join_by , "${missing[@]}")
    [ "$CANCEL" = 1 ] && { fail cancelled cancelled; return 1; }
    sleep 3
  done
  fail verify_failed "$h: $last"
}

st_verify_existing() {   # every existing node copies the new slaves' logs
  local h db x
  [ ${#NEW_SLAVES[@]} -eq 0 ] && return 0
  for h in "$L" "${EX_HOST[@]}"; do
    on_node "$h" 60 0 hb_status
    for db in "${D[@]}"; do
      for x in "${NEW_SLAVES[@]}"; do
        [[ $OUT == *"Copylogdb $db@$x:"* ]] || { fail verify_failed "$h: no copy of $db from $x"; return 1; }
      done
    done
  done
}

st_cleanup() {
  local h
  rm -rf "${BK_PATH:?}/$TAG"
  rmdir "$BK_PATH" 2>/dev/null
  for h in "${NEW_HOST[@]}"; do
    on_node "$h" 600 0 remove "$(rbk_root "$h")/$TAG"
    on_node "$h" 60 0 rmdir_empty "$(rbk_root "$h")"
  done
  return 0
}

# ------------------------------------------------ step list

# One cycle for every case; the promotion (S4-S8) is there only for a single server. Stage numbers (S1..S21)
# never go backwards: a stage done per database and per node is done for all of them before the next stage.
build_steps() {
  local h db
  S_ID=() S_TITLE=() S_NODE=() S_DB=() S_STATE=() S_DETAIL=() S_STAGE=()
  step_add S1 precheck "Check this server" "" ""
  for h in "${EX_HOST[@]}"; do step_add S2 node_check "Check an existing node" "$h" ""; done
  for h in "${NEW_HOST[@]}"; do step_add S2 node_check "Check a new node" "$h" ""; done
  step_add S3 preflight "Try the backup folder" "$L" ""
  for h in "${NEW_HOST[@]}"; do step_add S3 preflight "Try a transfer and the folders the restore writes" "$h" ""; done
  if [ "$LOCAL_STATE" = single ]; then
    step_add S4 convert_stop "Promote to HA master: stop the running databases (service stops)" "$L" ""
    step_add S5 convert_config "Promote to HA master: write the HA settings" "$L" ""
    step_add S6 convert_restart "Promote to HA master: start the HA databases under heartbeat" "$L" ""
    step_add S7 convert_verify "Promote to HA master: check that they are the active master (service resumes)" "$L" ""
    step_add S8 convert_start_others "Promote to HA master: start the other databases again without HA" "$L" ""
  fi
  for db in "${D[@]}"; do step_add S9 backup "Back up the database online" "$L" "$db"; done
  for db in "${D[@]}"; do for h in "${NEW_HOST[@]}"; do step_add S10 send_backup "Send the backup" "$h" "$db"; done; done
  for h in "${NEW_HOST[@]}"; do step_add S11 new_config "Write the new node's HA settings" "$h" ""; done
  for db in "${D[@]}"; do for h in "${NEW_HOST[@]}"; do step_add S12 keep_old_db "Keep the old database aside" "$h" "$db"; done; done
  for db in "${D[@]}"; do for h in "${NEW_HOST[@]}"; do step_add S13 restore "Restore" "$h" "$db"; done; done
  for db in "${D[@]}"; do for h in "${NEW_HOST[@]}"; do step_add S14 first_copylog "First copy of the replication log" "$h" "$db"; done; done
  if [ "$START_HA" = 1 ]; then
    for h in "${NEW_SLAVES[@]}"; do step_add S15 new_ha_start "Start HA on the new slave" "$h" ""; done
    for h in "${NEW_REPLICAS[@]}"; do step_add S15 new_ha_start "Start HA on the new replica" "$h" ""; done
    if [ ${#NEW_SLAVES[@]} -ge 2 ]; then
      for h in "${NEW_SLAVES[@]}"; do step_add S16 peer_repl_start "Start replication between the new slaves" "$h" ""; done
    fi
  fi
  step_add S17 existing_config "Add the new nodes to the HA settings" "$L" ""
  for h in "${EX_HOST[@]}"; do step_add S17 existing_config "Add the new nodes to the HA settings" "$h" ""; done
  if [ ${#NEW_SLAVES[@]} -ge 1 ]; then
    step_add S18 existing_repl_start "Start replication of the new slaves" "$L" ""
    for h in "${EX_HOST[@]}"; do step_add S18 existing_repl_start "Start replication of the new slaves" "$h" ""; done
  fi
  if [ "$START_HA" = 1 ]; then
    for h in "${NEW_HOST[@]}"; do step_add S19 verify "Check the new node" "$h" ""; done
    step_add S20 verify_existing "Check that the existing nodes copy the new slaves' logs" "" ""
  fi
  step_add S21 cleanup "Remove the backup files" "" ""
}

run_step() {   # run_step <index>
  local i=$1 id=${S_ID[$1]} h=${S_NODE[$1]} db=${S_DB[$1]} no=$(( $1 + 1 )) role=""
  local k
  for k in "${!NEW_HOST[@]}"; do [ "${NEW_HOST[$k]}" = "$h" ] && role=${NEW_ROLE[$k]}; done
  case $id in
    precheck) st_precheck ;;
    preflight) st_preflight "$h" ;;
    node_check)
      if [[ " ${EX_HOST[*]} " == *" $h "* ]]; then st_node_check "$h" existing ""; else st_node_check "$h" new "$role"; fi ;;
    convert_stop) st_convert_stop "$no" ;;
    convert_config) st_convert_config "$no" ;;
    convert_restart) st_convert_restart "$no" ;;
    convert_verify) st_convert_verify ;;
    convert_start_others) st_convert_start_others "$no" ;;
    new_config) st_new_config "$no" "$h" "$role" ;;
    backup) st_backup "$no" "$db" ;;
    send_backup) st_send_backup "$no" "$h" "$db" ;;
    keep_old_db) st_keep_old "$no" "$h" "$db" ;;
    restore) st_restore "$no" "$h" "$db" ;;
    first_copylog) st_first_copylog "$no" "$h" "$db" ;;
    new_ha_start) st_new_ha_start "$no" "$h" ;;
    peer_repl_start) st_peer_repl_start "$no" "$h" ;;
    existing_config) st_existing_config "$no" "$h" ;;
    existing_repl_start) st_existing_repl_start "$no" "$h" ;;
    verify) st_verify "$no" "$h" ;;
    verify_existing) st_verify_existing ;;
    cleanup) st_cleanup ;;
    *) fail internal "unknown step $id" ;;
  esac
}

# ------------------------------------------------ rollback

undo_one() {   # undo_one <host> <kind> [args...] ; 0 when it is back
  local h=$1 kind=$2
  shift 2
  case $kind in
    undo_file) on_node "$h" 120 0 undo_file "$@" ;;
    remove) on_node "$h" 600 0 remove "$@" ;;
    rmdir_empty) on_node "$h" 60 0 rmdir_empty "$@" ;;
    move_back) on_node "$h" 600 0 move_back "$@" ;;
    clear) on_node "$h" 600 0 clear "$@" ;;
    server_start) [ $# -eq 0 ] && return 0; on_node "$h" 600 0 server_start "$@" ;;
    server_stop) on_node "$h" 600 0 server_stop "$@" ;;
    ha_stop) on_node "$h" 600 0 ha_stop ;;
    master_start) on_node "$h" 120 0 master_start ;;
    reload) on_node "$h" 300 0 reload ;;
    repl) on_node "$h" 300 0 repl "$@" ;;
    *) OUT="unknown undo $kind"; return 1 ;;
  esac
}

# the promotion steps (convert_*) are undone only with --rollback-convert, unless the promotion itself failed
CONVERT_LAST=0
rollback_all() {
  IN_ROLLBACK=1
  trap '' TERM INT
  job_set phase rollback
  log "rolling back"
  [ -s "$UNDO" ] || { log "nothing to undo"; return 0; }
  local lines=() line n step h kind args unrestored=""
  mapfile -t lines < <(grep '^U' "$UNDO")
  local i
  for (( i=${#lines[@]}-1; i>=0; i-- )); do
    IFS=$'\t' read -r _ n step h kind args <<< "${lines[$i]}"
    grep -qx "D"$'\t'"$n" "$UNDO" && continue
    if [ "$CONVERT_LAST" -gt 0 ] && [ "$step" -le "$CONVERT_LAST" ] && [ "$ROLLBACK_CONVERT" = 0 ] && [ "${JOB[convert_done]:-0}" = 1 ]; then
      continue
    fi
    local IFS_SAVE=$IFS
    IFS=$'\t'; read -ra a <<< "$args"; IFS=$IFS_SAVE
    if undo_one "$h" "$kind" "${a[@]}"; then
      printf 'D\t%s\n' "$n" >> "$UNDO"
      if [ ${#S_ID[@]} -ge "$step" ] && [ "${S_STATE[$((step - 1))]}" != rollback_failed ]; then step_set $((step - 1)) rolled_back; fi
    else
      unrestored+="$h $kind ${args//$'\t'/ };"
      if [ ${#S_ID[@]} -ge "$step" ]; then step_set $((step - 1)) rollback_failed "$(one_line "$OUT")"; fi
      log "could not undo on $h: $kind $args ($OUT)"
    fi
  done
  JOB[unrestored]=$unrestored
  [ -z "$unrestored" ] && JOB[kept]=""
  job_write
  [ -z "$unrestored" ]
}

# ------------------------------------------------ plan output (JSON)

json_str() {
  local s=$1
  s=${s//\\/\\\\}; s=${s//\"/\\\"}; s=${s//$'\n'/\\n}; s=${s//$'\t'/\\t}; s=${s//$'\r'/}
  printf '"%s"' "$s"
}
json_arr() { local first=1 x; printf '['; for x in "$@"; do [ $first = 1 ] || printf ','; first=0; json_str "$x"; done; printf ']'; }

write_plan() {
  local i h db first=1 tmp="$JOB_DIR/plan.tmp" final_n final_r k=${#D[@]}
  final_n=$(final_node_list); final_r=$(final_replica_list)
  {
    printf '{"local":{"host":%s,"state":%s},"case":%s,"master_host":%s,"db_names":%s,' \
      "$(json_str "$L")" "$(json_str "$LOCAL_STATE")" "$CASE_NO" "$(json_str "$MASTER_HOST")" "$(json_arr "${D[@]}")"
    printf '"nodes":['
    printf '{"host":%s,"role":"master","new":false,"version":%s,"local":true}' "$(json_str "$L")" "$(json_str "$VERSION")"
    for i in "${!EX_HOST[@]}"; do
      printf ',{"host":%s,"role":%s,"new":false,"version":%s,"local":false}' "$(json_str "${EX_HOST[$i]}")" \
        "$(json_str "${EX_ROLE[$i]}")" "$(json_str "$(kv "${NODE_INFO[${EX_HOST[$i]}]:-}" VERSION)")"
    done
    for i in "${!NEW_HOST[@]}"; do
      printf ',{"host":%s,"role":%s,"new":true,"version":%s,"local":false}' "$(json_str "${NEW_HOST[$i]}")" \
        "$(json_str "${NEW_ROLE[$i]}")" "$(json_str "$(kv "${NODE_INFO[${NEW_HOST[$i]}]:-}" VERSION)")"
    done
    printf '],'
    if [ "$LOCAL_STATE" = single ]; then
      printf '"convert":{"ha_databases":%s,"restart_databases":%s,"other_running_databases":%s,"single_databases":%s},' \
        "$(json_arr "${D[@]}")" "$(json_arr "${UP_BEFORE[@]}")" "$(json_arr "${OTHERS_UP[@]}")" "$(json_arr "${SINGLES[@]}")"
    else
      printf '"convert":null,'
    fi
    local conf=()
    if [ "$LOCAL_STATE" = single ]; then
      conf+=("confirm_master_restart")
      [ ${#OTHERS_UP[@]} -ge 1 ] && conf+=("confirm_other_db_restart")
    fi
    for h in "${NEW_HOST[@]}"; do [ -n "${N_OTHERS_UP[$h]// /}" ] && [ "$START_HA" = 1 ] && { conf+=("confirm_new_node_db_restart"); break; }; done
    printf '"confirm_required":%s,' "$(json_arr "${conf[@]}")"
    printf '"new_node_databases":['
    first=1
    for h in "${NEW_HOST[@]}"; do
      [ $first = 1 ] || printf ','; first=0
      printf '{"host":%s,"running":%s,"staying_single":%s}' "$(json_str "$h")" "$(json_arr ${N_OTHERS_UP[$h]})" "$(json_arr ${N_OTHERS_REG[$h]})"
    done
    printf '],"node_paths":['
    first=1
    for h in "${NEW_HOST[@]}"; do
      for db in "${D[@]}"; do
        [ $first = 1 ] || printf ','; first=0
        printf '{"host":%s,"db":%s,"vol":%s,"log":%s,"lob":%s,"repl_log_home":%s,"receive":%s}' "$(json_str "$h")" "$(json_str "$db")" \
          "$(json_str "${NV[$h/$db]}")" "$(json_str "${NLG[$h/$db]}")" "$(json_str "${NLOB[$h/$db]}")" "$(json_str "${N_HOME[$h]}")" "$(json_str "$(rbk_root "$h")")"
      done
    done
    printf '],'

    printf '"changes":['
    first=1
    for h in "$L" "${EX_HOST[@]}"; do
      [ $first = 1 ] || printf ','; first=0
      printf '{"host":%s,"file":"cubrid_ha.conf","key":"ha_node_list","before":%s,"after":%s}' "$(json_str "$h")" "$(json_str "$NODE_LIST")" "$(json_str "$final_n")"
      printf ',{"host":%s,"file":"cubrid_ha.conf","key":"ha_replica_list","before":%s,"after":%s}' "$(json_str "$h")" "$(json_str "$REPLICA_LIST")" "$(json_str "$final_r")"
    done
    for h in "${NEW_HOST[@]}"; do
      printf ',{"host":%s,"file":"cubrid_ha.conf","key":"ha_node_list","before":"","after":%s}' "$(json_str "$h")" "$(json_str "$final_n")"
    done
    printf '],"backup_bytes":%s,"transfer_bytes":%s,' "${JOB[backup_bytes]:-0}" "$(( ${JOB[backup_bytes]:-0} * ${#NEW_HOST[@]} ))"
    local nn
    nn=$(list_hosts "$final_n" | wc -w)
    printf '"process_increase":['
    first=1
    for h in "$L" "${EX_HOST[@]}"; do
      [ $first = 1 ] || printf ','; first=0
      printf '{"host":%s,"copylogdb":%s,"applylogdb":%s}' "$(json_str "$h")" "$(( ${#NEW_SLAVES[@]} * k ))" "$(( ${#NEW_SLAVES[@]} * k ))"
    done
    for i in "${!NEW_HOST[@]}"; do
      local c=$(( (nn - 1) * k ))
      [ "${NEW_ROLE[$i]}" = replica ] && c=$(( nn * k ))
      printf ',{"host":%s,"copylogdb":%s,"applylogdb":%s}' "$(json_str "${NEW_HOST[$i]}")" "$c" "$c"
    done
    printf '],"warnings":%s,"steps":[' "$(json_arr "${WARN[@]}")"
    for i in "${!S_ID[@]}"; do
      [ "$i" = 0 ] || printf ','
      printf '{"no":%s,"stage":%s,"id":%s,"title":%s,"node":%s,"db":%s,"actor":"script"}' "$((i + 1))" "$(json_str "${S_STAGE[$i]}")" \
        "$(json_str "${S_ID[$i]}")" "$(json_str "${S_TITLE[$i]}")" "$(json_str "${S_NODE[$i]}")" "$(json_str "${S_DB[$i]}")"
    done
    printf ']}\n'
  } > "$tmp"
  mv -f "$tmp" "$JOB_DIR/plan"
}

# ------------------------------------------------ main

finish() {   # finish <status>
  JOB[status]=$1; JOB[phase]=done; JOB[ended_at]=$(epoch); JOB[detail]=""
  job_write
  lock_edit lock_free_fn
  [ -n "$AGENT" ] && "$CM_SSH" quit --agent "$AGENT" >/dev/null 2>&1
  log "job ends: $1"
  exit 0
}

JOB[uuid]=$UUID; JOB[action]=$ACTION; JOB[status]=running; JOB[phase]=run; JOB[started_at]=$(epoch); JOB[pid]=$$
JOB[pid_start]=$(proc_start $$)
job_write
log "$PROG $ACTION (job $UUID)"
if ! lock_edit lock_take_fn; then
  JOB[error]=busy; JOB[message]="another job is running: $LOCKED"; job_write
  JOB[status]=fail; JOB[phase]=done; job_write
  [ -n "$AGENT" ] && "$CM_SSH" quit --agent "$AGENT" >/dev/null 2>&1
  exit 1
fi
# the agent ends by itself once this process is gone
[ -n "$AGENT" ] && "$CM_SSH" bind --agent "$AGENT" "$$" >/dev/null 2>&1
if [ -z "$AGENT" ]; then
  if [ -t 0 ]; then start_own_agent || { JOB[error]=no_agent; JOB[message]="cannot start the ssh agent"; finish fail; }
  else JOB[error]=no_agent; JOB[message]="no --agent given"; finish fail
  fi
fi

if ! parse_inputs; then JOB[error]=$FAIL_CODE; JOB[message]=$FAIL_MSG; finish fail; fi

if [ "$ACTION" = rollback ]; then
  # the step list is not needed: the undo log has everything, done lines are skipped
  ROLLBACK_CONVERT=1
  for h in "${EX_HOST[@]}" "${NEW_HOST[@]}"; do NODE_LOCAL[$h]=0; done
  NODE_LOCAL[$L]=1
  if rollback_all; then finish rolled_back; else JOB[error]=rollback_incomplete; JOB[message]="some changes could not be undone"; finish rollback_failed; fi
fi
if [ "$ACTION" = cleanup ]; then
  # the old databases kept aside by a successful job: "kept" of its job file, given again by the manager
  removed=""
  for k in "${KEEP_ARG[@]}"; do
    h=${k%%:*}; p=${k#*:}
    [[ $p == *".ha_add_nodes_"* ]] && valid_path "$p" || continue
    on_node "$h" 600 0 remove "$p" && removed+=" $k"
  done
  JOB[removed]=$removed
  finish success
fi

# plan and run share the checks
S_ID=(precheck) S_TITLE=("Check this server") S_NODE=("") S_DB=("") S_STATE=(running) S_DETAIL=("") S_STAGE=(S1)
steps_write
if ! st_precheck; then
  JOB[error]=$FAIL_CODE; JOB[message]=$FAIL_MSG; JOB[case]=$CASE_NO
  step_set 0 failed "$FAIL_MSG"
  finish fail
fi
build_steps
S_STATE[0]=done
steps_write
JOB[case]=$CASE_NO; JOB[db_names]="${D[*]}"; job_write
# the node checks (plan: only these, nothing changes)
for i in "${!S_ID[@]}"; do
  case ${S_ID[$i]} in node_check|preflight) ;; *) continue ;; esac
  step_set "$i" running
  if run_step "$i"; then step_set "$i" done; else
    step_set "$i" failed "$FAIL_MSG"
    JOB[error]=$FAIL_CODE; JOB[message]=$FAIL_MSG; finish fail
  fi
done
write_plan
if [ "$ACTION" = plan ]; then finish success; fi

if [ "$LOCAL_STATE" = single ]; then
  [ "$CONFIRM_RESTART" = 1 ] || { JOB[error]=confirm_master_restart; JOB[message]="the running databases restart: ${UP_BEFORE[*]:-none}"; finish fail; }
  if [ ${#OTHERS_UP[@]} -ge 1 ] && [ "$CONFIRM_OTHERS" != 1 ]; then
    JOB[error]=confirm_other_db_restart; JOB[message]="also restart (staying single): ${OTHERS_UP[*]}"; finish fail
  fi
fi

if [ "$START_HA" = 1 ] && [ "$CONFIRM_NEWDB" != 1 ]; then
  for h in "${NEW_HOST[@]}"; do
    if [ -n "${N_OTHERS_UP[$h]// /}" ]; then
      JOB[error]=confirm_new_node_db_restart; JOB[message]="$h: also restart (staying single): ${N_OTHERS_UP[$h]}"; finish fail
    fi
  done
fi

failed_at=-1
for i in "${!S_ID[@]}"; do
  case ${S_ID[$i]} in precheck|node_check|preflight) continue ;; esac
  if [ "$CANCEL" = 1 ]; then FAIL_CODE=cancelled; FAIL_MSG="cancelled before ${S_ID[$i]}"; failed_at=$i; break; fi
  step_set "$i" running
  JOB[step_no]=$((i + 1)); job_write
  log "step $((i + 1))/${#S_ID[@]} ${S_ID[$i]}${S_NODE[$i]:+ [${S_NODE[$i]}]}${S_DB[$i]:+ [${S_DB[$i]}]}"
  FAIL_CODE="" FAIL_MSG=""
  if run_step "$i"; then
    step_set "$i" done
    [ "${S_ID[$i]}" = convert_start_others ] && { JOB[convert_done]=1; CONVERT_LAST=$((i + 1)); job_write; }
    continue
  fi
  [ "$RC" = 124 ] && { FAIL_CODE=no_response; FAIL_MSG="no answer in time: $FAIL_MSG"; }
  [ "$CANCEL" = 1 ] && [ "$FAIL_CODE" != no_response ] && { FAIL_CODE=cancelled; FAIL_MSG="cancelled during ${S_ID[$i]}${FAIL_MSG:+ ($FAIL_MSG)}"; }
  step_set "$i" failed "$FAIL_MSG"
  failed_at=$i
  break
done

if [ "$failed_at" -lt 0 ]; then
  # the copies of the edited files (<file>.bak.<job>.<step>) stay for reference
  log "completed"
  finish success
fi

JOB[error]=$FAIL_CODE; JOB[message]=$FAIL_MSG; job_write
if [ "$FAIL_CODE" = no_response ]; then
  # a command did not come back: nothing is undone on top of it; the user checks the node and calls rollback
  finish rollback_blocked
fi
if rollback_all; then
  [ "$FAIL_CODE" = cancelled ] && finish cancelled
  finish fail
fi
finish rollback_failed
