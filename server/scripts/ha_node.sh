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
# ha_node.sh - the node side of ha_add_nodes.sh: one action per call on an HA node (or on this server), started by
# "ssh <node> <$CUBRID>/share/cms/ha_node.sh" with the call on stdin (see node_main). Installed with the manager.
# Each call prints its result between "@@BEGIN" and "@@END <status>", so text from login files does not mix in.

{
SCRIPT_VERSION=5.0.0
HERE_FILE=$(readlink -f "$0")
HERE_CUBRID=$(readlink -f "${HERE_FILE%/*}/../..")
JOB=""

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
# The CUBRID environment of the account's login shell (ssh runs a command without the login files: bash keeps the
# environment in .bash_profile, csh/tcsh in .cshrc). $CUBRID must be the installation this file belongs to.
node_env_bootstrap() {
  local sh_ line q='echo "CUBRID=$CUBRID"; echo "CUBRID_DATABASES=$CUBRID_DATABASES"; echo "PATH=$PATH"; echo "LD_LIBRARY_PATH=$LD_LIBRARY_PATH"'
  if [ -z "${CUBRID:-}" ] || [ -z "${CUBRID_DATABASES:-}" ]; then
    sh_=$(getent passwd "$(id -un)" | cut -d: -f7)
    while IFS= read -r line; do
      case $line in
        CUBRID=?*|CUBRID_DATABASES=?*|PATH=?*|LD_LIBRARY_PATH=*) export "${line?}" ;;
      esac
    done < <(case $sh_ in */csh|*/tcsh) "$sh_" -c "$q" ;; *) bash -l -c "$q" ;; esac </dev/null 2>/dev/null)
  fi
  [ -n "${CUBRID:-}" ] || { echo "CUBRID is not set for this account"; return 1; }
  [ "$(readlink -f "$CUBRID")" = "$HERE_CUBRID" ] || { echo "CUBRID=$CUBRID is not the installation of this script ($HERE_CUBRID)"; return 1; }
  CUBRID_DATABASES=${CUBRID_DATABASES:-$CUBRID/databases}
  export CUBRID CUBRID_DATABASES
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
  # name:volume:log of every registered database (S2 refuses paths that overlap another database's)
  echo "DBPATHS=$(awk '$1 !~ /^#/ && NF >= 4 { print $1 ":" $2 ":" $4 }' "$CUBRID_DATABASES/databases.txt" 2>/dev/null | tr '\n' ' ')"
  if timeout 15 cubrid heartbeat status </dev/null 2>/dev/null | grep -q "HA-Node Info"; then echo "HEARTBEAT=yes"; else echo "HEARTBEAT=no"; fi
  echo "CUBRID_PORT_ID=$(conf_get "$CUBRID/conf/cubrid.conf" "[common]" cubrid_port_id)"
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


node_server_start() { local db rc=0; for db in "$@"; do node_detached cubrid server start "$db" || rc=1; done; return $rc; }
node_server_ensure() {   # node server_ensure <db>... : start only the ones that do not run (an undo may find them running)
  local db rc=0 up
  up=" $(cubrid server status 2>/dev/null </dev/null | awk '$1 == "Server" || $1 == "HA-Server" { print $2 }' | tr '\n' ' ') "
  for db in "$@"; do
    if [[ $up == *" $db "* ]]; then echo "already running: $db"; else node_detached cubrid server start "$db" || rc=1; fi
  done
  return $rc
}
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

# undo forms: done when there is nothing left to do (the state may already be the wanted one, e.g. heartbeat stopped)
node_server_stop_ensure() {   # stop only the ones that run
  local db rc=0 up
  up=" $(cubrid server status 2>/dev/null </dev/null | awk '$1 == "Server" || $1 == "HA-Server" { print $2 }' | tr '\n' ' ') "
  for db in "$@"; do
    if [[ $up == *" $db "* ]]; then node_server_stop "$db" || rc=1; else echo "not running: $db"; fi
  done
  return $rc
}
node_reload_ensure() {   # reload when heartbeat runs; without heartbeat the files are read at the next start
  local out rc
  cub_master_running || { echo "cub_master is not running: nothing to reload"; return 0; }
  out=$(timeout 30 cubrid heartbeat status </dev/null 2>&1)
  rc=$?
  # no answer is not "no heartbeat": the reload is not skipped then
  [ $rc -eq 124 ] && { echo "heartbeat status did not answer in 30 s"; return 1; }
  case $out in *"not configured for HA"*) echo "not in HA mode: nothing to reload"; return 0 ;; esac
  node_reload
}
repl_procs() {   # repl_procs <node>: copylogdb/applylogdb processes for that node
  local f a
  for f in /proc/[0-9]*/cmdline; do
    a=$(tr '\0' ' ' < "$f" 2>/dev/null) || continue
    case $a in *copylogdb*"@$1 "*|*copylogdb*"@$1"|*applylogdb*"_$1 "*|*applylogdb*"_$1") echo "$a" ;; esac
  done
}
node_repl_stop_ensure() {   # repl stop <node>; done when no log copy or apply process for that node is left
  local out
  local i
  out=$(node_detached cubrid heartbeat repl stop "$1")
  echo "$out"
  for i in 1 2 3 4 5 6 7 8 9 10; do
    out=$(repl_procs "$1")
    [ -z "$out" ] && return 0
    sleep 1
  done
  echo "still running: $out"
  return 1
}

# node busy <action> [args...] : the processes of that node action that still run ("BUSY <command line>"), rc 1 if any.
# Used before undoing on a node whose connection broke while the action ran.
node_busy() {
  local action=$1 f a pat=() p hit=0
  shift
  case $action in
    restore) pat=("restoreslave" " $1 ") ;;
    first_copylog) pat=("copylogdb" "$1@$2") ;;
    cubrid|backup) pat=("backupdb" "") ;;
    ha_start|ha_stop|reload|reload_ensure|repl|repl_stop_ensure) pat=("cubrid heartbeat " "") ;;
    server_start|server_ensure|server_stop|server_stop_ensure) pat=("cubrid server " "") ;;
    master_down|master_start) pat=("cub_commdb" "") ;;
    *) return 0 ;;
  esac
  for f in /proc/[0-9]*/cmdline; do
    # not this call itself (its shell, the ssh session's shell, the pipeline of this loop)
    case ${f%/cmdline} in "/proc/$$"|"/proc/$PPID"|"/proc/$BASHPID") continue ;; esac
    a=$(tr '\0' ' ' < "$f" 2>/dev/null) || continue
    [[ $a == *"${pat[0]}"* ]] || continue
    # the utilities that change something, not someone's status call
    case ${pat[0]} in
      "cubrid heartbeat "|"cubrid server ") [[ $a =~ cubrid\ (heartbeat|server)\ (start|stop|reload|repl|replication) ]] || continue ;;
    esac
    [ -z "${pat[1]}" ] || [[ $a == *"${pat[1]}"* ]] || continue
    echo "BUSY $a"; hit=1
  done
  [ $hit = 0 ]
}

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

node_server_pids() {   # node server_pids <db>... : "<db> <pid> <pid start time>" (none when it does not run)
  local st db pid
  st=$(cubrid server status 2>/dev/null </dev/null)
  for db in "$@"; do
    pid=$(printf '%s\n' "$st" | sed -n "s/^ *\(HA-\)\{0,1\}Server $db (.*pid \([0-9]*\)).*/\2/p" | head -1)
    echo "$db ${pid:-none} $(proc_start "${pid:-0}")"
  done
}

node_hb_status() { timeout 30 cubrid heartbeat status 2>&1 </dev/null; }

node_exists() { local p; for p in "$@"; do [ -e "$p" ] && echo "EXISTS $p" || echo "MISSING $p"; done; }

# keep_old <db> [<node>...] : move what is there aside; print MOVED/PARENT/CREATED/EMPTY lines. The volume and log
# folders come from this node's databases.txt, the log copy folders are <copy base>/<db>_<node> of the given HA nodes
# only (not every <db>_* name: another database can be called <db>_something)
# (PARENT: a missing parent folder that mkdir -p creates, outermost first)
node_keep_old() {
  local suffix=".ha_add_nodes_$JOB" db=$1 vol log home p d a x paths=() parents
  shift
  vol=$(db_field "$db" 2); log=$(db_field "$db" 4); home=$(copy_base)
  [ -n "$vol" ] && [ -n "$log" ] || { echo "no databases.txt line for $db"; return 1; }
  paths=("$vol")
  [ "$log" = "$vol" ] || paths+=("$log")
  for p in "${paths[@]}"; do
    if [ -d "$p" ] && [ -n "$(ls -A "$p" 2>/dev/null)" ]; then
      mv "$p" "$p$suffix" && echo "MOVED $p" || return 1
    fi
  done
  for x in "$@"; do
    d="$home/${db}_$x"
    [ -d "$d" ] || continue
    mv "$d" "$d$suffix" && echo "MOVED $d" || return 1
  done
  for p in "${paths[@]}"; do
    if [ -d "$p" ]; then echo "EMPTY $p"; continue; fi
    parents=() a=$(dirname "$p")
    while [ ! -e "$a" ] && [ "$a" != / ]; do parents=("$a" "${parents[@]}"); a=$(dirname "$a"); done
    mkdir -p "$p" || return 1
    for a in "${parents[@]}"; do echo "PARENT $a"; done
    echo "CREATED $p"
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

node_restore() {   # restore <db> <backup dir> <master> <source state>
  # -u: the volumes go where this node's databases.txt says, not where they were on the master
  local log
  log=$(db_field "$1" 4)
  cp -f "$2/${1}_bkvinf" "$log/" || return 1
  cubrid restoreslave -s "$4" -m "$3" -u -B "$2" "$1" 2>&1 </dev/null
}

node_first_copylog() {   # first_copylog <db> <master> : into <copy base>/<db>_<master>
  local dir
  dir="$(copy_base)/${1}_$2"
  mkdir -p "$dir" || return 1
  echo "DIR $dir"
  cub_admin copylogdb -L "$dir" -m async --start-page-id=-1 "$1@$2" 2>&1 </dev/null
}

node_backup() {   # backup <db> <dir> : online full backup (on the backup source)
  mkdir -p "$2" && chmod 700 "$2" || return 1
  cubrid backupdb -C -D "$2" -o "$2/backup.out" --no-check "$1@localhost" 2>&1 </dev/null
}

# recv <file> <size> <sha256> [--append] : the rest of stdin into <file>.part; when the size and sha256 match, the
# part file becomes <file>. recv_size <file> : bytes already in <file>.part (to continue after a broken transfer)
node_recv() {
  local f=$1 size=$2 sum=$3 got
  mkdir -p "${f%/*}" || return 1
  if [ "${4:-}" = --append ]; then cat >> "$f.part" || return 1; else cat > "$f.part" || return 1; fi
  got=$(wc -c < "$f.part" | tr -d ' ')
  [ "$got" = "$size" ] || { echo "SIZE $got"; return 1; }
  [ "$(sha256sum "$f.part" | cut -d' ' -f1)" = "$sum" ] || { echo "sha256 differs"; rm -f "$f.part"; return 1; }
  mv -f "$f.part" "$f" && echo "OK $f"
}
node_recv_size() { local n; n=$(wc -c < "$1.part" 2>/dev/null | tr -d ' '); echo "SIZE ${n:-0}"; }

node_version() {
  echo "SCRIPT_VERSION=$SCRIPT_VERSION"
  echo "CUBRID_VERSION=$("$CUBRID/bin/cubrid_rel" 2>/dev/null </dev/null | grep -m1 CUBRID | sed -n 's/.*(\([0-9][0-9.]*\)[-)].*/\1/p')"
  echo "CUBRID=$CUBRID"
  echo "SELF_SHA256=$(sha256sum "$HERE_FILE" | cut -d' ' -f1)"
  echo "ME=$(id -un)"
  local p=$HERE_FILE
  while :; do
    echo "OWNER $(stat -c '%U %a' "$p" 2>/dev/null) $p"
    [ "$p" = / ] && break
    p=$(dirname "$p")
  done
}


# --------------------------------------------------------------------------- what a call may touch

# Every call names its job (--job <uuid>); the paths a call may change are this job's marked paths, the setting files,
# and the paths of the HA databases that this node computes itself from its databases.txt and cubrid_ha.conf.
CONF_FILES=()
set_conf_files() { CONF_FILES=("$CUBRID/conf/cubrid.conf" "$CUBRID/conf/cubrid_ha.conf" "$CUBRID_DATABASES/databases.txt"); }
is_conf() { local f; for f in "${CONF_FILES[@]}"; do [ "$1" = "$f" ] && return 0; done; return 1; }
tag_ok() { [[ $1 =~ ^${JOB}\.[0-9]+$ ]]; }
db_field() { awk -v db="$1" -v n="$2" '{ t=$0; sub(/^[ \t]+/, "", t); split(t, g, /[ \t]+/) } t !~ /^#/ && g[1] == db { print g[n]; exit }' "$CUBRID_DATABASES/databases.txt" 2>/dev/null; }
copy_base() {
  local b
  b=$(awk -F= '/^[ \t]*ha_copy_log_base[ \t]*=/ { v = $2 } END { gsub(/[ \t]/, "", v); print v }' "$CUBRID/conf/cubrid_ha.conf" 2>/dev/null)
  printf '%s' "${b:-$CUBRID_DATABASES}"
}
ha_dbs() { awk -F= '/^[ \t]*ha_db_list[ \t]*=/ { v = $2 } END { gsub(/[ \t]/, "", v); gsub(/,/, " ", v); print v }' "$CUBRID/conf/cubrid_ha.conf" 2>/dev/null; }
is_ha_db() { [[ " $(ha_dbs) " == *" $1 "* ]] && [ -n "$(db_field "$1" 2)" ]; }
d_paths() { local db; for db in $(ha_dbs); do db_field "$db" 2; db_field "$db" 4; done; }
is_d_path() { local p; for p in $(d_paths); do [ "$1" = "$p" ] && return 0; done; return 1; }
holds_d_path() { local p; for p in $(d_paths); do [[ $p == "$1"/* ]] && return 0; done; return 1; }
under_backup() { [[ $1 == */ha_add_nodes_backup/$JOB || $1 == */ha_add_nodes_backup/$JOB/* || $1 == */ha_add_nodes_backup/$JOB.preflight || $1 == */ha_add_nodes_backup/$JOB.preflight/* ]]; }
is_marked() { [[ $1 == *".ha_add_nodes_$JOB" ]]; }
is_copy_dir() {
  local b n
  b=$(copy_base); [[ $1 == "$b"/* ]] || return 1
  n=${1#"$b"/}; [[ $n == */* ]] && return 1
  local db
  for db in $(ha_dbs) $(awk '$1 !~ /^#/ && NF >= 4 { print $1 }' "$CUBRID_DATABASES/databases.txt" 2>/dev/null); do
    [[ $n == "${db}_"* ]] && valid_name "${n#"${db}"_}" && return 0
  done
  return 1
}
paths_ok() { local p; for p in "$@"; do valid_path "$p" || return 1; done; }
names_ok() { local n; for n in "$@"; do valid_name "$n" || return 1; done; }
entries_ok() {   # <section> <key> <value> ...
  while [ $# -ge 3 ]; do
    [[ $1 == "[common]" || ( $1 =~ ^\[@[A-Za-z0-9][A-Za-z0-9._-]*\]$ ) ]] || return 1
    [[ $2 =~ ^[a-z_]+$ ]] || return 1
    [[ $3 =~ ^[A-Za-z0-9@:,._/-]*$ ]] || return 1
    shift 3
  done
  [ $# -eq 0 ]
}
# allowed <action> <args...> : 0 when this call may run
allowed() {
  local a=$1 p
  shift
  case $a in
    version|info|hb_status) return 0 ;;
    resolve|mode|server_pids|server_start|server_ensure|server_stop|server_stop_ensure) names_ok "$@" ;;
    free|writable|exists) paths_ok "$@" ;;
    master_down|master_start|ha_start|ha_stop|reload|reload_ensure) [ $# -eq 0 ] ;;
    repl) [[ $1 == start || $1 == stop ]] && names_ok "$2" && [ $# -eq 2 ] ;;
    repl_stop_ensure) [ $# -eq 1 ] && names_ok "$1" ;;
    busy) [[ $1 =~ ^[a-z_]+$ ]] ;;
    conf_edit) [[ $1 == "$CUBRID/conf/cubrid.conf" || $1 == "$CUBRID/conf/cubrid_ha.conf" ]] && tag_ok "$2" && { shift 2; entries_ok "$@"; } ;;
    dbtxt_put) tag_ok "$1" && names_ok "$2" && paths_ok "$3" "$5" && [[ $4 =~ ^[A-Za-z0-9._:-]+$ ]] && { [ -z "${6:-}" ] || { [[ $6 == file:* ]] && valid_path "${6#file:}"; }; } ;;
    dbtxt_add_hosts) tag_ok "$1" && [[ $2 =~ ^[A-Za-z0-9._,-]+$ ]] && { shift 2; names_ok "$@"; } ;;
    undo_file) is_conf "$1" && tag_ok "$2" ;;
    drop_backups) tag_ok "$1" && { shift; for p in "$@"; do is_conf "$p" || return 1; done; } ;;
    mkdir) for p in "$@"; do valid_path "$p" && { under_backup "$p" || [ "$p" = "$(copy_base)" ]; } || return 1; done ;;
    recv|recv_size) valid_path "$1" && under_backup "$1" ;;
    remove) for p in "$@"; do valid_path "$p" && { under_backup "$p" || is_marked "$p" || is_copy_dir "$p"; } || return 1; done ;;
    clear) [[ $1 == created || $1 == empty ]] && valid_path "$2" && is_d_path "$2" ;;
    move_back) valid_path "$1" && [ "$2" = "$1.ha_add_nodes_$JOB" ] && { is_d_path "$1" || is_copy_dir "$1"; } ;;
    rmdir_empty) valid_path "$1" && { under_backup "$1" || [[ $1 == */ha_add_nodes_backup ]] || [ "$1" = "$(copy_base)" ] || is_d_path "$1" || holds_d_path "$1"; } ;;
    keep_old) is_ha_db "$1" && { shift; names_ok "$@"; } ;;
    restore) is_ha_db "$1" && valid_path "$2" && under_backup "$2" && names_ok "$3" && [[ $4 == master || $4 == slave || $4 == replica ]] ;;
    first_copylog) is_ha_db "$1" && names_ok "$2" ;;
    backup) names_ok "$1" && [ -n "$(db_field "$1" 2)" ] && valid_path "$2" && under_backup "$2" ;;
    *) return 1 ;;
  esac
}

node_dispatch() {
  local action=$1
  shift
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
    server_ensure) node_server_ensure "$@" ;;
    server_stop) node_server_stop "$@" ;;
    master_down) node_master_down ;;
    master_start) node_master_start ;;
    ha_start) node_ha_start ;;
    ha_stop) node_ha_stop ;;
    reload) node_reload ;;
    reload_ensure) node_reload_ensure ;;
    busy) node_busy "$@" ;;
    server_stop_ensure) node_server_stop_ensure "$@" ;;
    repl_stop_ensure) node_repl_stop_ensure "$@" ;;
    repl) node_repl "$@" ;;
    hb_status) node_hb_status ;;
    server_pids) node_server_pids "$@" ;;
    exists) node_exists "$@" ;;
    keep_old) node_keep_old "$@" ;;
    move_back) node_move_back "$@" ;;
    clear) node_clear "$@" ;;
    remove) node_remove "$@" ;;
    rmdir_empty) rmdir "$1" 2>/dev/null; true ;;
    mkdir) mkdir -p "$@" ;;
    restore) node_restore "$@" ;;
    backup) node_backup "$@" ;;
    recv_size) node_recv_size "$@" ;;
    version) node_version ;;
    first_copylog) node_first_copylog "$@" ;;
    *) echo "unknown node action $action"; false ;;
  esac
}

# --------------------------------------------------------------------------- entry

# The command line is only this file's path (it can be put in a ForceCommand or an allow list). The call comes on
# stdin: a line with the number of arguments, then the arguments each ended by a NUL ("--job" <uuid> <action>
# <args...>). For recv the file content follows.
node_main() {
  local n i a args=() action rc
  IFS= read -r n || { echo "@@BEGIN"; echo "no call on stdin"; echo "@@END 2"; return 2; }
  [[ $n =~ ^[0-9]{1,3}$ ]] && [ "$n" -ge 3 ] || { echo "@@BEGIN"; echo "bad call"; echo "@@END 2"; return 2; }
  for ((i = 0; i < n; i++)); do IFS= read -r -d '' a || break; args+=("$a"); done
  [ ${#args[@]} -eq "$n" ] && [ "${args[0]}" = --job ] && [[ ${args[1]} =~ ^[0-9a-f]{1,64}$ ]] \
    || { echo "@@BEGIN"; echo "bad call"; echo "@@END 2"; return 2; }
  JOB=${args[1]} action=${args[2]}
  args=("${args[@]:3}")
  echo "@@BEGIN"
  node_env_bootstrap || { echo "@@END 3"; return 3; }
  set_conf_files
  if ! allowed "$action" "${args[@]}"; then echo "not allowed: $action $(one_line "${args[*]}" | cut -c1-300)"; echo "@@END 4"; return 4; fi
  set -- "${args[@]}"
  case $action in
    recv) node_recv "$@" ;;
    *) node_dispatch "$action" "$@" </dev/null ;;
  esac
  rc=$?
  echo "@@END $rc"
  return $rc
}

node_main
exit $?
}
