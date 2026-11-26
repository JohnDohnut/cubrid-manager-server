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
#   ha_add_nodes.sh <plan|run|rollback|cleanup> --job-dir <dir> --ssh-config <file> [--node-script-sha256 <hex>] [options]
#     --master-host <name>            this server's HA node name (default: uname -n)
#     --db <db>                       (single server only) a database to put into HA; repeat for more
#     --confirm-master-restart        (single server) the running databases restart
#     --confirm-other-db-restart      (single server) databases staying single but running restart too
#     --confirm-new-node-db-restart   other databases running on a new node restart (they stay single)
#     --new <slave|replica>:<host>:<port>:<user>[:<fingerprint>]       a new node; repeat
#     --existing <host>:<port>:<user>[:<fingerprint>]                  an existing node of the group; repeat
#     --repl-log-home <path>          new nodes' replication log folder (default: their $CUBRID_DATABASES)
#     --backup-path <path>            the backup goes under <path>/ha_add_nodes_backup (default <path>: $CUBRID_DATABASES)
#     --remote-backup-path <path>     the same on the new nodes (default: their $CUBRID_DATABASES)
#     --ha-group <name> --ha-port-id <port>   (single server) HA group name (default cubrid) and port (59901)
#     --rollback-convert              on failure after the promotion, go back to a single server as well
#     --no-start-ha                   prepare the new nodes but do not start HA on them
#     --keep <host>:<path>            (cleanup) an old database folder a successful job kept aside; repeat
#
# Every value is an argument; nothing in this file is meant to be edited. Passwords never reach this script: the
# manager logs in to every node first (one ssh ControlMaster per node) and gives the job's ssh settings file
# (--ssh-config); every ssh here goes through those connections with all authentication turned off.
#
# Work on a node runs the installed node script: "ssh <node> <$CUBRID>/share/cms/ha_node.sh" with the call on stdin;
# this server runs its own ha_node.sh the same way. --node-script-sha256 is the manager's reference hash of it.

{
PROG=ha_add_nodes.sh
# the manager runs this file from fd 3 (the descriptor it hashed): not passed on to the servers started here
exec 3<&-

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


# --------------------------------------------------------------------------- orchestration (on this server, L)

umask 077
[ -n "${CUBRID:-}" ] || { echo "$PROG: CUBRID is not set" >&2; exit 2; }
NODE_SCRIPT_L="$CUBRID/share/cms/ha_node.sh"
CANCEL=0
IN_ROLLBACK=0

usage() { echo "usage: $PROG <plan|run|rollback|cleanup> --job-dir <dir> --ssh-config <file> [--node-script-sha256 <hex>] [options] (see the head of the file)"; }

ACTION=${1:-}
shift || true
JOB_DIR="" SSH_CONFIG="" NODE_SHA="" MASTER_HOST="" HA_GROUP=cubrid HA_PORT_ID=59901 REPL_HOME="" BK_PATH="" RBK_PATH=""
CONFIRM_RESTART=0 CONFIRM_OTHERS=0 CONFIRM_NEWDB=0 ROLLBACK_CONVERT=0 START_HA=1
DBS_ARG=() NEW_SPEC=() EXIST_SPEC=() KEEP_ARG=()
while [ $# -gt 0 ]; do
  case $1 in
    --job-dir) JOB_DIR=$2; shift 2 ;;
    --ssh-config) SSH_CONFIG=$2; shift 2 ;;
    --node-script-sha256) NODE_SHA=$2; shift 2 ;;
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
job_write() {   # swapped in only when the whole file was written (a full disk keeps the last good state)
  local k tmp="$JOB_DIR/job.tmp" ok=1
  : > "$tmp" || return 1
  for k in "${!JOB[@]}"; do printf '%s=%s\n' "$k" "$(one_line "${JOB[$k]}")" >> "$tmp" || ok=0; done
  [ $ok = 1 ] && mv -f "$tmp" "$JOB_DIR/job"
}
job_set() { JOB[$1]=$2; job_write; }

S_ID=() S_TITLE=() S_NODE=() S_DB=() S_STATE=() S_DETAIL=() S_STAGE=()
steps_write() {
  local i tmp="$JOB_DIR/steps.tmp" ok=1
  : > "$tmp" || return 1
  for i in "${!S_ID[@]}"; do
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$((i + 1))" "${S_ID[$i]}" "${S_TITLE[$i]}" "${S_NODE[$i]}" "${S_DB[$i]}" \
      "${S_STATE[$i]}" "$(one_line "${S_DETAIL[$i]}")" "${S_STAGE[$i]:-}" >> "$tmp" || ok=0
  done
  [ $ok = 1 ] && mv -f "$tmp" "$JOB_DIR/steps"
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
  # running: held while its script lives, and after it died without finishing (interrupted) until that job is rolled
  # back. blocked: a job stopped with rollback_blocked, held until it is rolled back. Only that job's rollback passes.
  local w u p t st
  read -r w u p t <<< "$LOCKED"
  [ "$u" = "$UUID" ] && return 1
  case $w in
    running)
      [ "$(proc_start "$p")" = "$t" ] && return 0
      [[ $u =~ ^[0-9a-f]+$ ]] || return 1
      st=$(sed -n 's/^status=//p; s/^action=/action /p' "${JOB_DIR%/*}/$u/job" 2>/dev/null | tr '\n' ' ')
      # a plan changes nothing: it does not hold the lock after it died
      [[ " $st " == *" running "* ]] && [[ " $st " != *" action plan "* ]] && [[ " $st " != *" action cleanup "* ]] ;;
    blocked) return 0 ;;
    starting) [ $(( $(epoch) - p )) -lt 60 ] ;;
    *) return 1 ;;
  esac
}
lock_take_fn() {
  if lock_in_use; then return 1; fi
  printf 'running %s %s %s\n' "$UUID" "$$" "$(proc_start $$)" > "$LOCK"
}
lock_free_fn() { case $LOCKED in "running $UUID "*) : > "$LOCK" ;; esac; }
lock_block_fn() { case $LOCKED in "running $UUID "*) printf 'blocked %s\n' "$UUID" > "$LOCK" ;; esac; }

# ------------------------------------------------ agent and remote execution

declare -A NODE_LOCAL=() NODE_INFO=() N_CUBRID=()
OUT="" RC=0 LAST_ACTION="" LAST_ARGS="" TIMED_OUT=0 CANCEL_KILLED=0 NO_RESULT=0 MAY_HAVE_RUN=0 CONN_LOST=0 UNDO_FAILED=0
# Each command runs in the background so a cancel is noticed at once. interruptible: on cancel the client is
# stopped (the agent then drops the transfer); otherwise the command is waited for, up to its limit.
proc_alive() { local st; st=$(cat "/proc/$1/stat" 2>/dev/null) || return 1; st=${st##*) }; [ "${st%% *}" != Z ]; }
run_bg() {   # run_bg <limit seconds> <interruptible 0|1> <command...> ; sets OUT, RC
  local limit=$1 intr=$2 pid t0 tmp noted=0
  shift 2
  TIMED_OUT=0 CANCEL_KILLED=0
  # nothing changes once the undo log cannot be written (I4)
  if [ "$UNDO_FAILED" = 1 ] && [ "$IN_ROLLBACK" = 0 ]; then OUT="the undo log cannot be written"; RC=1; return 1; fi
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
        OUT=$(cat "$tmp"); rm -f "$tmp"; RC=130; CANCEL_KILLED=1
        return 130
      fi
      if [ $noted = 0 ]; then
        noted=1
        job_set detail "cancel pending: waiting for $(one_line "$*" | cut -c1-150)"
      fi
    fi
    if [ $(( $(epoch) - t0 )) -gt "$limit" ]; then
      # KILL: during the rollback TERM is ignored, and an ignored signal stays ignored in the commands it starts
      kill -KILL "$pid" 2>/dev/null
      wait "$pid" 2>/dev/null
      OUT=$(cat "$tmp"); rm -f "$tmp"; RC=124; TIMED_OUT=1
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

ssh_node() { ssh -F "$SSH_CONFIG" -T "$@"; }
ssh_alive() { timeout 10 ssh -F "$SSH_CONFIG" -O check "$1" </dev/null >/dev/null 2>&1; }

# the node's $CUBRID, read through its login shell (bash: .bash_profile, csh/tcsh: .cshrc); the command line stays
# fixed ("sh -s"), the small reader comes on stdin
discover_cubrid() {   # discover_cubrid <host> : sets N_CUBRID[host]; OUT has the reason when it fails
  local out
  out=$(timeout 60 ssh -F "$SSH_CONFIG" -T "$1" 'sh -s' 2>&1 <<'READ'
s=$(getent passwd "$(id -un)" | cut -d: -f7)
case $s in
  */csh|*/tcsh) v=$("$s" -c 'echo "$CUBRID"' </dev/null 2>/dev/null) ;;
  *) v=$(bash -l -c 'echo "$CUBRID"' </dev/null 2>/dev/null) ;;
esac
echo "@@CUBRID=$v"
READ
)
  local v
  v=$(printf '%s\n' "$out" | sed -n 's/^@@CUBRID=//p' | tail -1)
  if [ -z "$v" ] || ! valid_path "$v"; then OUT=${v:-$(printf '%s\n' "$out" | grep -v '^$' | tail -1)}; return 1; fi
  N_CUBRID[$1]=$v
}

# A call of the node script: on stdin a line with the number of arguments, then each argument ended by a NUL
# ("--job" <uuid> <action> <args...>); for recv the file follows (FEED_DATA from byte FEED_FROM).
FEED_DATA="" FEED_FROM=0
write_call() {   # write_call <file> <action> [args...]
  local f=$1
  shift
  { printf '%s\n' "$(( $# + 2 ))"; printf '%s\0' --job "$UUID" "$@"; } > "$f"
}
kill_tree() {   # kill_tree <pid> : the process and its children (a pipeline started in the background)
  local c
  for c in $(awk -v p="$1" '{ s=$0; sub(/.*\) /, "", s); split(s, f, " "); if (f[2] == p) { n=split(FILENAME, q, "/"); print q[3] } }' /proc/[0-9]*/stat 2>/dev/null); do
    kill_tree "$c"
  done
  kill -KILL "$1" 2>/dev/null
}

# on_node <host> <limit> <interruptible> <action> [args...] : the node action's output in OUT, status in RC.
# Flags for the caller: TIMED_OUT / CANCEL_KILLED (from run_bg), NO_RESULT (no @@END line), CONN_LOST (the host's
# connection is gone), MAY_HAVE_RUN (no result although the command may have started).
on_node() {
  local host=$1 limit=$2 intr=$3 call feeder="" fifo=""
  shift 3
  LAST_ACTION="$host $1" LAST_ARGS="${*:2}"
  log "$host: $*"
  NO_RESULT=0 MAY_HAVE_RUN=0 CONN_LOST=0
  call=$(mktemp "$JOB_DIR/call.XXXXXX")
  write_call "$call" "$@"
  local in=$call
  if [ -n "$FEED_DATA" ]; then
    # the call and then the data, through a FIFO so ssh (or bash) stays the direct child that run_bg stops
    fifo="$call.fifo"; mkfifo -m 600 "$fifo"
    { cat "$call"; tail -c +$(( FEED_FROM + 1 )) "$FEED_DATA"; } > "$fifo" 2>/dev/null &
    feeder=$!
    in=$fifo
  fi
  if [ "${NODE_LOCAL[$host]:-0}" = 1 ]; then
    RUN_IN=$in run_bg "$limit" "$intr" bash "$NODE_SCRIPT_L"
  elif [ -z "${N_CUBRID[$host]:-}" ] && ! discover_cubrid "$host"; then
    RC=255; TIMED_OUT=0; CANCEL_KILLED=0
  elif ! ssh_alive "$host"; then
    OUT="the connection to $host is gone"; RC=255; TIMED_OUT=0; CANCEL_KILLED=0
  else
    RUN_IN=$in run_bg "$limit" "$intr" ssh -F "$SSH_CONFIG" -T "$host" "${N_CUBRID[$host]}/share/cms/ha_node.sh"
  fi
  if [ -n "$feeder" ]; then kill_tree "$feeder"; wait "$feeder" 2>/dev/null; fi
  rm -f "$call" "$fifo"
  FEED_DATA="" FEED_FROM=0
  local full=$OUT body
  body=$(printf '%s\n' "$full" | awk '/^@@BEGIN$/ { on=1; next } /^@@END / { on=0; next } on')
  printf '%s\n' "$full" | sed 's/^/  /' >> "$LOG"
  if [ "$TIMED_OUT" = 1 ] || [ "$CANCEL_KILLED" = 1 ]; then OUT=$body; return $RC; fi
  if ! printf '%s\n' "$full" | grep -q '^@@END '; then
    NO_RESULT=1
    OUT=$(printf '%s\n' "$full" | grep -v '^[[:space:]]*$' | tail -1)
    [ -n "$OUT" ] || OUT="no result from $host"
    if [ "${NODE_LOCAL[$host]:-0}" = 1 ]; then MAY_HAVE_RUN=1
    elif ! ssh_alive "$host"; then
      # 255 and no result and the master is gone: the connection broke (the command may have started)
      CONN_LOST=1; MAY_HAVE_RUN=1; OUT="the connection to $host broke: $OUT"
    else
      # some other ssh failure (a session refused, ...): the command may still have started
      MAY_HAVE_RUN=1; OUT="ssh to $host failed: $OUT"
    fi
    [ "$RC" -eq 0 ] && RC=255
    return "$RC"
  fi
  OUT=$body
  RC=$(printf '%s\n' "$full" | sed -n 's/^@@END \([0-9]*\).*/\1/p' | tail -1)
  return "${RC:-1}"
}

# send_file <host> <local file> <remote file> : through the node's recv (size and sha256 compared there); a broken
# transfer continues from what arrived, up to 3 times while the connection is there
send_file() {
  local h=$1 f=$2 dst=$3 size sum try off=0 mode=()
  size=$(wc -c < "$f" | tr -d ' '); sum=$(sha256sum "$f" | cut -d' ' -f1)
  for try in 1 2 3; do
    FEED_DATA=$f FEED_FROM=$off
    on_node "$h" 21600 1 recv "$dst" "$size" "$sum" "${mode[@]}" && return 0
    [ "$CANCEL_KILLED" = 1 ] && return 130
    { [ "$TIMED_OUT" = 1 ] || [ "$CONN_LOST" = 1 ]; } && return 1
    on_node "$h" 60 0 recv_size "$dst" || return 1
    off=$(printf '%s\n' "$OUT" | sed -n 's/^SIZE //p'); off=${off:-0}
    if [ "$off" -gt 0 ] && [ "$off" -lt "$size" ]; then mode=(--append); else off=0 mode=(); fi
    log "$h: transfer of $f continues from byte $off"
  done
  return 1
}

kv() { printf '%s\n' "$1" | sed -n "s/^$2=//p" | head -1; }

# ------------------------------------------------ undo log

UNDO="$JOB_DIR/undo"
undo_add() {   # undo_add <step no> <host> <kind> [args...] (no tabs in values)
  local n
  n=$(( $(grep -c '^U' "$UNDO" 2>/dev/null || echo 0) + 1 ))
  local IFS=$'\t'
  if ! printf 'U\t%s\t%s\t%s\t%s\n' "$n" "$1" "$2" "${*:3}" >> "$UNDO"; then
    UNDO_FAILED=1
    return 1
  fi
}

# ------------------------------------------------ failure and cancel

FAIL_CODE="" FAIL_MSG=""
fail() { FAIL_CODE=$1; FAIL_MSG=$2; return 1; }
# A command that did not answer in time: the long ones and the ones that start or stop CUBRID processes may still
# be running on the node, so nothing is undone on top of them (rollback_blocked). The others (file and folder
# edits, reads) finish at once on the node: no answer means the connection is gone, and the job rolls back.
may_still_run() {
  case $1 in
    restore|first_copylog|cubrid|ha_start|ha_stop|server_start|server_ensure|server_stop|master_down|master_start|reload|repl) return 0 ;;
  esac
  return 1
}
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
declare -A DB_VOL=() DB_LOG=() DB_LOB=() DB_HOSTS=() PK_STATUS=() PK_TABLES=() PK_MSG=()
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
    # the existing nodes in the order of the lists (slaves by priority, then replicas), whatever the input order
    local oh=() op=() ou=() orl=() j
    for h in "${want[@]}"; do
      for j in "${!EX_HOST[@]}"; do
        [ "${EX_HOST[$j]}" = "$h" ] || continue
        oh+=("$h"); op+=("${EX_PORT[$j]}"); ou+=("${EX_USER[$j]}")
        if [[ " $(list_hosts "$NODE_LIST") " == *" $h "* ]]; then orl+=(slave); else orl+=(replica); fi
      done
    done
    EX_HOST=("${oh[@]}"); EX_PORT=("${op[@]}"); EX_USER=("${ou[@]}"); EX_ROLE=("${orl[@]}")
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
  # HA replicates only tables with a primary key. The manager checks with the database logins of the screen
  # (pk_check in the job folder: "db<TAB>ok|partial|failed<TAB>tables or message"); this script gets no password.
  local st rest
  for db in "${D[@]}"; do
    IFS=$'\t' read -r _ st rest < <(awk -F'\t' -v d="$db" '$1 == d' "$JOB_DIR/pk_check" 2>/dev/null)
    case ${st:-} in
      ok) PK_STATUS[$db]=ok; PK_TABLES[$db]=$rest
          [ -n "$rest" ] && WARN+=("$db: tables without a primary key are not replicated: $rest") ;;
      partial) PK_STATUS[$db]=partial; PK_TABLES[$db]=$rest
          WARN+=("$db: not a dba account, only part of the tables checked${rest:+; without a primary key: $rest}") ;;
      failed) PK_STATUS[$db]=failed; PK_MSG[$db]=$rest; WARN+=("$db: primary keys not checked ($rest)") ;;
      *) PK_STATUS[$db]=not_logged_in; WARN+=("$db: primary keys not checked (not logged in)") ;;
    esac
  done
  BK_PATH=${BK_PATH:-$CUBRID_DATABASES}; BK_PATH=${BK_PATH%/}; [[ $BK_PATH == */ha_add_nodes_backup ]] || BK_PATH+=/ha_add_nodes_backup
  [ "$LOCAL_STATE" = single ] || check_ha_values || return 1
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

# the HA settings copied to the new nodes have to pass the node script's value check (letters, digits, @:,._/-)
check_ha_values() {
  local k v bad=""
  while IFS=$'\t' read -r k v; do
    [ -n "$k" ] || continue
    [[ $k =~ ^[a-z_]+$ && $v =~ ^[A-Za-z0-9@:,._/-]*$ ]] || bad+="$k=$v "
  done < <(conf_pairs "$CUBRID/conf/cubrid_ha.conf" "[common]")
  [ -z "$bad" ] || { fail precheck_failed "cubrid_ha.conf has values the node script does not take: $bad"; return 1; }
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
rbk_root() { local b=${RBK_PATH:-${N_CUBDB[$1]}}; b=${b%/}; [[ $b == */ha_add_nodes_backup ]] || b+=/ha_add_nodes_backup; printf '%s' "$b"; }
all_names() { printf '%s ' "$L" "${EX_HOST[@]}" "${NEW_HOST[@]}"; }

# node_login_check <host>: the shell first, then $CUBRID, then the node script (missing, other version, changed,
# loosely owned), each with its own error code
node_login_check() {
  local h=$1 out rc
  ssh_alive "$h" || { fail connection_lost "$h: no connection (the manager's login to it is gone)"; return 1; }
  out=$(timeout 60 ssh -F "$SSH_CONFIG" -T "$h" true 2>&1 </dev/null); rc=$?
  if [ $rc -ne 0 ]; then
    case $out in
      *"currently not available"*) fail account_no_shell "$h: the account's login shell does not allow a login ($(one_line "$out"))" ;;
      *) if [ $rc -eq 126 ] || [ $rc -eq 127 ]; then fail remote_shell_failed "$h: the account's login shell cannot run a command ($rc)"
         elif [ $rc -eq 255 ] && ! ssh_alive "$h"; then fail connection_lost "$h: the connection broke"
         else fail ssh_failed "$h: $(one_line "$out" | tail -c 200)"
         fi ;;
    esac
    return 1
  fi
  discover_cubrid "$h" || { fail cubrid_not_found "$h: CUBRID was not found for this account ($(one_line "$OUT"))"; return 1; }
  on_node "$h" 60 0 version
  if [ "$NO_RESULT" = 1 ]; then
    if [ "$RC" = 127 ] || [[ $OUT == *"No such file"* ]]; then
      fail node_script_missing "$h: ${N_CUBRID[$h]}/share/cms/ha_node.sh is missing (the CUBRID package with the manager is needed)"
    elif [ "$CONN_LOST" = 1 ]; then fail connection_lost "$h: $OUT"
    else fail ssh_failed "$h: $OUT"
    fi
    return 1
  fi
  local v=$OUT sv sh sv_l me
  sv=$(kv "$v" SCRIPT_VERSION); sh=$(kv "$v" SELF_SHA256); me=$(kv "$v" ME)
  sv_l=$(sed -n 's/^SCRIPT_VERSION=//p' "$NODE_SCRIPT_L" | head -1)
  if [ -n "$NODE_SHA" ] && [ "$sh" != "$NODE_SHA" ]; then
    if [ "$sv" != "$sv_l" ]; then fail node_script_version_mismatch "$h: its node script is version $sv, this server's $sv_l"
    else fail node_script_tampered "$h: its node script differs from the package's (sha256 $sh)"
    fi
    return 1
  fi
  # the file and every folder up to / : owned by the account or root, no group or other write
  local owner mode path bad=""
  while read -r _ owner mode path; do
    [ -n "$path" ] || continue
    if { [ "$owner" != "$me" ] && [ "$owner" != root ]; } || (( (8#${mode: -2:1} & 2) || (8#${mode: -1} & 2) )); then
      bad+="$path ($owner $mode) "
    fi
  done < <(printf '%s\n' "$v" | grep '^OWNER ')
  [ -z "$bad" ] || { fail node_script_unsafe "$h: others can change the node script: $bad"; return 1; }
  return 0
}

st_node_check() {   # st_node_check <host> <existing|new> <role>
  local h=$1 kind=$2 role=$3 db
  if [ "${NODE_LOCAL[$h]:-0}" != 1 ]; then node_login_check "$h" || return 1; fi
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
    # a new node must not be in an HA setup (another group would lose this node)
    local ham hnl hrl hb ham_on ni=${NODE_INFO[$h]}
    ham=$(kv "$ni" HA_MODE); hnl=$(kv "$ni" HA_NODE_LIST); hrl=$(kv "$ni" HA_REPLICA_LIST); hb=$(kv "$ni" HEARTBEAT)
    case $ham in on|yes|replica|ON|YES|REPLICA) ham_on=1 ;; *) ham_on=0 ;; esac
    if [ "$ham_on" = 1 ] || [ -n "$hnl" ] || [ -n "$hrl" ] || [ "$hb" = yes ]; then
      fail new_node_in_ha "$h: already set up for HA (ha_mode=${ham:-off}, ha_node_list=${hnl:-none}, ha_replica_list=${hrl:-none}, heartbeat running: $hb); a new node has to be a server without HA settings"
      return 1
    fi
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
    # S12 moves the volume and log folders aside as a whole: each has to be a folder of this database only
    local p q e other
    for db in "${D[@]}"; do
      for p in "${NV[$h/$db]}" "${NLG[$h/$db]}"; do
        for q in "${N_CUBDB[$h]}" "${N_HOME[$h]}"; do
          if [ "$p" = "$q" ] || [[ $q/ == "$p"/* ]]; then
            fail db_path_conflict "$h: the folder of $db would be $p, which is or holds $q; it needs a folder of its own (give the database a folder of its own on $L)"
            return 1
          fi
        done
        for e in $(kv "${NODE_INFO[$h]}" DBPATHS); do
          other=${e%%:*}
          [[ " ${D[*]} " == *" $other "* ]] && continue
          for q in $(e=${e#*:}; printf '%s %s' "${e%%:*}" "${e#*:}"); do
            if [ "$p" = "$q" ] || [[ $q/ == "$p"/* ]] || [[ $p/ == "$q"/* ]]; then
              fail db_path_conflict "$h: the folder of $db would be $p, which overlaps $q of database $other there"
              return 1
            fi
          done
        done
      done
    done
  fi
  return 0
}

# ------------------------------------------------ S3 preflight: what the later steps write is tried first

# each failure also goes to the job folder as "node<TAB>path<TAB>reason<TAB>detail" (preflight_failures in the status;
# reason: not_writable, no_room, cannot_create, transfer_failed) so the screen does not have to parse the message
# node_checks: "node<TAB>code<TAB>message<TAB>action" per node (the manager writes the login results, S2 replaces
# the node's row)
action_for() {
  case $1 in
    ok) echo "" ;;
    connection_lost) echo "Check the network and the node, then run again with the passwords" ;;
    ssh_failed) echo "Check the ssh message and the node's sshd settings" ;;
    account_no_shell|remote_shell_failed) echo "Check the account's login shell on the node" ;;
    cubrid_not_found) echo "Give the account that has CUBRID installed" ;;
    node_script_missing) echo "Install the CUBRID package with the manager on the node" ;;
    node_script_version_mismatch|version_mismatch) echo "Install the same CUBRID version on every node" ;;
    node_script_tampered) echo "Put back the packaged ha_node.sh and find out who changed it" ;;
    node_script_unsafe) echo "Remove group and other write permission from the node script and its folders" ;;
    new_node_in_ha) echo "Use a server without HA settings as a new node" ;;
    db_path_conflict) echo "Give the database a folder of its own" ;;
    port_mismatch) echo "Use the same cubrid_port_id on every node" ;;
    slave_db_running) echo "Stop the database on the new node" ;;
    *) echo "See the message" ;;
  esac
}
nc_set() {   # nc_set <node> <code> <message>
  local f="$JOB_DIR/node_checks" tmp="$JOB_DIR/node_checks.tmp" line
  line="$1"$'\t'"$2"$'\t'"$(one_line "$3")"$'\t'"$(action_for "$2")"
  { [ -f "$f" ] && awk -F'\t' -v n="$1" '$1 != n' "$f"; printf '%s\n' "$line"; } > "$tmp" && mv -f "$tmp" "$f"
}

pf_note() { printf '%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$(one_line "$4")" >> "$JOB_DIR/preflight_failures"; }

st_preflight() {   # <host>: L tries its backup folder; a new node its receiving folder (a small transfer),
                   # the folders the restore writes to and the replication log folder
  local h=$1 db need=0 p r bad=0 path anc st
  if [ "$h" = "$L" ]; then
    on_node "$L" 30 0 writable "$BK_PATH"
    if [[ $OUT != OK* ]]; then
      read -r _ path anc <<< "$OUT"
      pf_note "$L" "$BK_PATH" not_writable "${anc:-$OUT} is not writable"
      fail precheck_failed "cannot write $BK_PATH ($(one_line "$OUT"))"
      return 1
    fi
    return 0
  fi
  # the new node connects to the master's cub_master with its own cubrid_port_id (copylogdb, S14)
  local mp np
  mp=$(conf_get "$CUBRID/conf/cubrid.conf" "[common]" cubrid_port_id); np=$(kv "${NODE_INFO[$h]}" CUBRID_PORT_ID)
  if [ "${np:-1523}" != "${mp:-1523}" ]; then
    pf_note "$h" "$(kv "${NODE_INFO[$h]}" CUBRID)/conf/cubrid.conf" port_mismatch "cubrid_port_id ${np:-1523}, $L has ${mp:-1523}"
    fail port_mismatch "$h: cubrid_port_id is ${np:-1523}, $L has ${mp:-1523}; they must be the same"
    return 1
  fi
  r=$(rbk_root "$h")
  local paths=() x
  for x in "$r" "${N_HOME[$h]}" $(for db in "${D[@]}"; do printf '%s %s ' "${NV[$h/$db]}" "${NLG[$h/$db]}"; done); do
    [[ " ${paths[*]} " == *" $x "* ]] || paths+=("$x")
  done
  on_node "$h" 30 0 writable "${paths[@]}"
  local wout=$OUT
  while read -r st path anc; do
    [ "$st" = NO ] || continue
    pf_note "$h" "$path" not_writable "$anc is not writable"
    bad=1
  done <<< "$wout"
  if [ $bad = 1 ]; then
    fail precheck_failed "$h cannot write: $(printf '%s\n' "$wout" | awk '$1 == "NO" { printf "%s (%s is not writable) ", $2, $3 }')"
    return 1
  fi
  # room: the backup arrives, then the restore writes about the same again
  need=$(( ${JOB[backup_bytes]:-0} ))
  p=$(printf '%s\n' "$wout" | awk -v r="$r" '$1 == "OK" && $2 == r { print $3 }')
  if [ "${p:-0}" -le "$need" ]; then
    pf_note "$h" "$r" no_room "${p:-0} bytes free, about $need needed"
    fail precheck_failed "$h: not enough room for the backup under $r (${p:-0} bytes free, about $need needed)"
    return 1
  fi
  # a small transfer into a folder of its own, removed again
  local t="$r/$TAG.preflight" made=0
  on_node "$h" 30 0 exists "$r"; [[ $OUT == *"MISSING"* ]] && made=1
  if ! on_node "$h" 30 0 mkdir "$t"; then
    pf_note "$h" "$t" cannot_create "$OUT"
    fail transfer_failed "$h: cannot create $t"
    return 1
  fi
  local tf="$JOB_DIR/transfer-test"
  head -c 65536 /dev/urandom > "$tf" 2>/dev/null || : > "$tf"
  send_file "$h" "$tf" "$t/transfer-test"
  local prc=$? pout=$OUT
  rm -f "$tf"
  on_node "$h" 30 0 remove "$t"
  [ $made = 1 ] && on_node "$h" 30 0 rmdir_empty "$r"
  if [ "$prc" -ne 0 ]; then
    pf_note "$h" "$t" transfer_failed "$(one_line "$pout")"
    fail transfer_failed "$h: a test transfer failed: $(one_line "$pout")"
    return 1
  fi
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
  local db waiting=("${D[@]}") still end=$(( $(epoch) + 180 ))
  while [ "$(epoch)" -lt "$end" ]; do
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
  mkdir -p "$BK_PATH/$TAG" && chmod 700 "$BK_PATH/$TAG" || { fail command_failed "cannot create $BK_PATH/$TAG"; return 1; }
  on_node "$L" 21600 0 backup "$db" "$dir" || { fail command_failed "cubrid backupdb $db failed ($RC): $(one_line "$OUT" | tail -c 300)"; return 1; }
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
    send_file "$h" "$f" "$dst/$(basename "$f")"
    case $? in
      0) ;;
      130) fail cancelled cancelled; return 1 ;;
      *) fail transfer_failed "$h: sending $(basename "$f") failed: $(one_line "$OUT")"; return 1 ;;
    esac
  done
}

declare -A CLEAR_AT_RESTORE=()
st_keep_old() {   # <no> <host> <db>
  local no=$1 h=$2 db=$3 l kind p
  local peers=() x
  for x in $(list_hosts "$(final_node_list)"); do [ "$x" = "$h" ] || peers+=("$x"); done
  on_node "$h" 600 0 keep_old "$db" "${peers[@]}"
  local rc=$RC
  while read -r kind p; do
    case $kind in
      MOVED) undo_add "$no" "$h" move_back "$p" "$p.ha_add_nodes_$TAG"; JOB[kept]+=" $h:$p.ha_add_nodes_$TAG" ;;
      # empty until the restore; the restore's own undo clears what it wrote first (it comes later in the log)
      PARENT) undo_add "$no" "$h" rmdir_empty "$p" ;;
      CREATED) undo_add "$no" "$h" rmdir_empty "$p"; CLEAR_AT_RESTORE[$h/$db]+="created $p"$'\n' ;;
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
  on_node "$h" 21600 0 restore "$db" "$dst" "$MASTER_HOST" "$SRC_STATE" \
    || { fail command_failed "$h: cubrid restoreslave $db failed ($RC): $(one_line "$OUT" | tail -c 300)"; return 1; }
}

st_first_copylog() {   # <no> <host> <db>
  local no=$1 h=$2 db=$3 dir="${N_HOME[$2]}/$3_$MASTER_HOST"
  undo_add "$no" "$h" remove "$dir"
  on_node "$h" 3600 0 first_copylog "$db" "$MASTER_HOST" \
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
  # heartbeat start makes a log copy folder for every other ha_node_list node; keep_old moved any old ones aside,
  # so the missing ones are this job's (removed after heartbeat stop: recorded before it)
  local x db dirs=()
  for x in $(list_hosts "$(final_node_list)"); do
    [ "$x" = "$h" ] && continue
    for db in "${D[@]}"; do dirs+=("${N_HOME[$h]}/${db}_$x"); done
  done
  on_node "$h" 60 0 exists "${dirs[@]}" || { fail command_failed "$h: cannot check ${N_HOME[$h]}"; return 1; }
  for x in "${dirs[@]}"; do [[ $OUT == *"MISSING $x"* ]] && undo_add "$no" "$h" remove "$x"; done
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
  local no=$1 h=$2 db x want=() missing last="" end=$(( $(epoch) + 120 ))
  for x in $(list_hosts "$(final_node_list)"); do [ "$x" = "$h" ] || want+=("$x"); done
  while [ "$(epoch)" -lt "$end" ]; do
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

# ------------------------------------------------ the master stays the same server

# Taken before the backup (S9), compared after the first log copies (before S15) and before the existing nodes are
# changed (S17): a failover or a restart of the master in between would leave the new nodes copying from a server
# that is no longer the one the backup came from.
MSNAP=""
master_state() {   # sets MSTATE: "<db> <mode> <pid> <pid start>" lines of the master
  local m p
  on_node "$MASTER_HOST" 120 0 mode "${D[@]}" || return 1
  m=$OUT
  on_node "$MASTER_HOST" 60 0 server_pids "${D[@]}" || return 1
  p=$OUT
  MSTATE=$(local db; for db in "${D[@]}"; do
    printf '%s %s %s\n' "$db" "$(printf '%s\n' "$m" | awk -v d="$db" '$1 == d { print $2 }')" \
      "$(printf '%s\n' "$p" | awk -v d="$db" '$1 == d { print $2, $3 }')"
  done)
}
master_guard() {   # master_guard <index>: before S9 record, before the first S15 and the first S17 compare
  local id=${S_ID[$1]} prev=""
  [ "$1" -gt 0 ] && prev=${S_ID[$(($1 - 1))]}
  case $id in
    backup) [ -n "$MSNAP" ] && return 0 ;;
    new_ha_start|existing_config) [ "$prev" = "$id" ] && return 0; [ -n "$MSNAP" ] || return 0 ;;
    *) return 0 ;;
  esac
  # a busy server can answer changemode with "unknown" once: read up to 5 times, 3 s apart, before deciding
  local try
  for try in 1 2 3 4 5; do
    master_state || { fail master_changed "$MASTER_HOST: cannot read the state of the master ($(one_line "$OUT"))"; return 1; }
    if [ "$id" = backup ]; then
      printf '%s\n' "$MSTATE" | awk '$2 != "active" || $3 == "none" { bad = 1 } END { exit !bad }' || break
    else
      [ "$MSTATE" != "$MSNAP" ] || break
    fi
    [ $try -lt 5 ] && sleep 3
  done
  if [ "$id" = backup ]; then
    if printf '%s\n' "$MSTATE" | awk '$2 != "active" || $3 == "none" { bad = 1 } END { exit !bad }'; then
      fail master_changed "$MASTER_HOST is not the active master of every database: $(one_line "$MSTATE")"; return 1
    fi
    MSNAP=$MSTATE; JOB[master_state]=$(one_line "$MSTATE"); job_write
    log "master state: $(one_line "$MSTATE")"
    return 0
  fi
  if [ "$MSTATE" != "$MSNAP" ]; then
    fail master_changed "the master changed since the backup (failover or restart): before $(one_line "$MSNAP"), now $(one_line "$MSTATE")"
    return 1
  fi
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
    server_start) [ $# -eq 0 ] && return 0; on_node "$h" 600 0 server_ensure "$@" ;;
    server_stop) on_node "$h" 600 0 server_stop_ensure "$@" ;;
    ha_stop) on_node "$h" 600 0 ha_stop ;;
    master_start) on_node "$h" 120 0 master_start ;;
    reload) on_node "$h" 300 0 reload_ensure ;;
    repl) if [ "$1" = stop ]; then on_node "$h" 300 0 repl_stop_ensure "$2"; else on_node "$h" 300 0 repl "$@"; fi ;;
    *) OUT="unknown undo $kind"; return 1 ;;
  esac
}

# node_quiet <host>: none of the commands that ran on that node when its connection broke still runs (sets BUSY_NOTE)
node_quiet() {
  local h=$1 e eh act rest list=() av=()
  BUSY_NOTE=""
  IFS=';' read -ra list <<< "${JOB[lost_actions]:-}"
  for e in "${list[@]}"; do
    read -r eh act rest <<< "$e"
    [ "$eh" = "$h" ] || continue
    read -ra av <<< "$rest"
    on_node "$h" 60 0 busy "$act" "${av[@]}"
    if [ "$RC" -ne 0 ]; then
      if [ "$NO_RESULT" = 1 ] || [ "$TIMED_OUT" = 1 ]; then BUSY_NOTE="no answer"
      else BUSY_NOTE="$act still runs there: $(printf '%s\n' "$OUT" | grep '^BUSY' | head -1 | cut -c6-200)"
      fi
      return 1
    fi
  done
  return 0
}

# the promotion steps (convert_*) are undone only with --rollback-convert, unless the promotion itself failed
CONVERT_LAST=0
rollback_all() {
  IN_ROLLBACK=1
  trap '' TERM INT
  job_set phase rollback
  log "rolling back"
  [ -s "$UNDO" ] || { log "nothing to undo"; return 0; }
  local lines=() line n step h kind args unrestored="" blocked=0
  # when a node does not answer an undo (time limit, lost connection, helper error), its later lines are not tried
  # in this run: they are left for a later rollback. A timeout, or a lost connection under a command that may still
  # run, also blocks (I6). Other failures are marked and the node's later lines still run (the undo forms of repl
  # stop, reload and server stop succeed when there is nothing left to do, so a second rollback can finish).
  local -A dead=() checked=()
  # nodes whose connection broke under a command (this run or the job being undone): checked before their first undo
  local lost_before=${JOB[lost_nodes]:-}
  [ "$CONVERT_LAST" -gt 0 ] || CONVERT_LAST=$(awk -F'\t' '$1 == "C" { v = $2 } END { print v + 0 }' "$UNDO")
  mapfile -t lines < <(grep '^U' "$UNDO")
  local i
  for (( i=${#lines[@]}-1; i>=0; i-- )); do
    IFS=$'\t' read -r _ n step h kind args <<< "${lines[$i]}"
    grep -qx "D"$'\t'"$n" "$UNDO" && continue
    if [ "$CONVERT_LAST" -gt 0 ] && [ "$step" -le "$CONVERT_LAST" ] && [ "$ROLLBACK_CONVERT" = 0 ]; then
      continue
    fi
    local IFS_SAVE=$IFS
    IFS=$'\t'; read -ra a <<< "$args"; IFS=$IFS_SAVE
    if [ -n "${dead[$h]:-}" ]; then
      unrestored+="$h $kind ${args//$'\t'/ };"
      if [ ${#S_ID[@]} -ge "$step" ]; then step_set $((step - 1)) rollback_failed "$h: not tried (${dead[$h]})"; fi
      continue
    fi
    if [ -z "${checked[$h]:-}" ] && [[ " ${lost_before} " == *" $h "* ]]; then
      checked[$h]=1
      if node_quiet "$h"; then
        # checked: nothing from before the broken connection runs there any more
        local ln="" x en="" e2
        for x in ${JOB[lost_nodes]:-}; do [ "$x" = "$h" ] || ln+="${ln:+ }$x"; done
        IFS=';' read -ra e2 <<< "${JOB[lost_actions]:-}"
        for x in "${e2[@]}"; do [ -n "$x" ] && [ "${x%% *}" != "$h" ] && en+="$x;"; done
        JOB[lost_nodes]=$ln; JOB[lost_actions]=$en; job_write
      else
        blocked=1; dead[$h]="$BUSY_NOTE"
        RB_NOTE="undoing stopped on $h: $BUSY_NOTE"
        unrestored+="$h $kind ${args//$'\t'/ };"
        if [ ${#S_ID[@]} -ge "$step" ]; then step_set $((step - 1)) rollback_failed "$h: not tried ($BUSY_NOTE)"; fi
        continue
      fi
    fi
    if undo_one "$h" "$kind" "${a[@]}"; then
      printf 'D\t%s\n' "$n" >> "$UNDO"
      if [ ${#S_ID[@]} -ge "$step" ] && [ "${S_STATE[$((step - 1))]}" != rollback_failed ]; then step_set $((step - 1)) rolled_back; fi
    else
      unrestored+="$h $kind ${args//$'\t'/ };"
      if [ "$TIMED_OUT" = 1 ] || [ "$NO_RESULT" = 1 ]; then
        [[ " ${JOB[lost_nodes]:-} " == *" $h "* ]] || JOB[lost_nodes]+="${JOB[lost_nodes]:+ }$h"
        JOB[lost_actions]+="${LAST_ACTION} ${LAST_ARGS};"
      fi
      if [ "$TIMED_OUT" = 1 ] || { [ "$NO_RESULT" = 1 ] && [ "$MAY_HAVE_RUN" = 1 ] && may_still_run "${LAST_ACTION#* }"; }; then
        blocked=1; dead[$h]="no answer to an earlier undo"
        RB_NOTE="undoing stopped on $h: $kind did not finish in time or the connection broke"
      elif [ "$NO_RESULT" = 1 ]; then dead[$h]="no answer to an earlier undo"
      else
        # the undo forms are idempotent: a stop that fails means something still runs there, and the node's later
        # lines (remove, clear, move_back) would act under it
        case $kind in ha_stop|server_stop|repl|master_down) dead[$h]="$kind failed: something may still run" ;; esac
      fi
      # any other failure is only marked: the node's later lines (the setting files above all) still run
      if [ ${#S_ID[@]} -ge "$step" ]; then step_set $((step - 1)) rollback_failed "$(one_line "$OUT")"; fi
      log "could not undo on $h: $kind $args ($OUT)"
    fi
  done
  JOB[unrestored]=$unrestored
  [ -z "$unrestored" ] && JOB[kept]=""
  job_write
  [ "$blocked" = 1 ] && return 2
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
    printf '],"primary_key_check":['
    first=1
    for db in "${D[@]}"; do
      [ $first = 1 ] || printf ','; first=0
      printf '{"db":%s,"status":%s,"tables":%s,"message":%s}' "$(json_str "$db")" "$(json_str "${PK_STATUS[$db]:-not_logged_in}")" \
        "$(json_arr ${PK_TABLES[$db]:-})" "$(json_str "${PK_MSG[$db]:-}")"
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

close_masters() {   # the job's ssh connections, one per node (the watcher closes them too; this covers a lost watcher)
  [ -f "$SSH_CONFIG" ] || return 0
  local h
  for h in $(awk '$1 == "Host" { for (i = 2; i <= NF; i++) if ($i !~ /[*?!]/) print $i }' "$SSH_CONFIG"); do
    timeout 10 ssh -F "$SSH_CONFIG" -O exit "$h" </dev/null >/dev/null 2>&1
  done
}

finish() {   # finish <status>
  JOB[status]=$1; JOB[phase]=done; JOB[ended_at]=$(epoch); JOB[detail]=""
  job_write
  # rollback_blocked keeps the lock (a command may still run on a node) until this job is rolled back
  if [ "$1" = rollback_blocked ]; then lock_edit lock_block_fn; else lock_edit lock_free_fn; fi
  close_masters
  log "job ends: $1"
  exit 0
}

# rollback and cleanup work on an earlier job's folder: keep what describes that job
if [ "$ACTION" = rollback ] || [ "$ACTION" = cleanup ]; then
  while IFS= read -r kvl; do
    case ${kvl%%=*} in case|db_names|kept|downtime_sec|lost_nodes|lost_actions) JOB[${kvl%%=*}]=${kvl#*=} ;; esac
  done < <(cat "$JOB_DIR/job" 2>/dev/null)
fi
JOB[uuid]=$UUID; JOB[action]=$ACTION; JOB[status]=running; JOB[phase]=$([ "$ACTION" = rollback ] && echo rollback || echo run); JOB[started_at]=$(epoch); JOB[pid]=$$
JOB[pid_start]=$(proc_start $$)
# the step table first, so the first state the manager sees already has a running step
if [ "$ACTION" = plan ] || [ "$ACTION" = run ]; then
  S_ID=(precheck) S_TITLE=("Check this server") S_NODE=("") S_DB=("") S_STATE=(running) S_DETAIL=("") S_STAGE=(S1)
  steps_write
fi
job_write
log "$PROG $ACTION (job $UUID)"
if ! lock_edit lock_take_fn; then
  JOB[error]=busy; JOB[message]="another job holds the lock (roll back a stopped job first): $LOCKED"; job_write
  JOB[status]=fail; JOB[phase]=done; job_write
  exit 1
fi
if [ -z "$SSH_CONFIG" ] || [ ! -f "$SSH_CONFIG" ]; then JOB[error]=no_ssh_config; JOB[message]="no --ssh-config (the manager logs in to the nodes first)"; finish fail; fi
[ -f "$NODE_SCRIPT_L" ] || { JOB[error]=node_script_missing; JOB[message]="$NODE_SCRIPT_L is missing"; finish fail; }

if ! parse_inputs; then JOB[error]=$FAIL_CODE; JOB[message]=$FAIL_MSG; finish fail; fi

if [ "$ACTION" = rollback ]; then
  # the step list is not needed: the undo log has everything, done lines are skipped
  # the promotion is undone only with the job's --rollback-convert (the undo log marks where it ended)
  # the step table of the job being undone, so the screen sees rolled_back / rollback_failed per step
  if [ -s "$JOB_DIR/steps" ]; then
    S_ID=() S_TITLE=() S_NODE=() S_DB=() S_STATE=() S_DETAIL=() S_STAGE=()
    line="" f=()
    while IFS= read -r line; do
      # split by hand: read with a tab IFS would merge empty fields
      f=(); while :; do f+=("${line%%$'\t'*}"); [[ $line == *$'\t'* ]] || break; line=${line#*$'\t'}; done
      S_ID+=("${f[1]}"); S_TITLE+=("${f[2]}"); S_NODE+=("${f[3]}"); S_DB+=("${f[4]}"); S_STATE+=("${f[5]}"); S_DETAIL+=("${f[6]}"); S_STAGE+=("${f[7]:-}")
      # the step the job was in when it stopped
      [ "${f[5]}" = running ] && { S_STATE[${#S_STATE[@]}-1]=failed; S_DETAIL[${#S_DETAIL[@]}-1]="interrupted"; }
      # an earlier rollback's marks are set again by this one
      [ "${f[5]}" = rollback_failed ] && { S_STATE[${#S_STATE[@]}-1]=done; S_DETAIL[${#S_DETAIL[@]}-1]=""; }
    done < "$JOB_DIR/steps"
  fi
  for h in "${EX_HOST[@]}" "${NEW_HOST[@]}"; do NODE_LOCAL[$h]=0; done
  NODE_LOCAL[$L]=1
  RB_NOTE=""
  rollback_all; rb=$?
  [ $rb = 0 ] && finish rolled_back
  JOB[error]=rollback_incomplete; JOB[message]="some changes could not be undone${RB_NOTE:+; $RB_NOTE}"
  [ $rb = 2 ] && finish rollback_blocked
  finish rollback_failed
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
# where this server keeps its log copies (S18 checks there which copy folders the job makes)
N_COPYBASE[$L]=$(conf_get "$CUBRID/conf/cubrid_ha.conf" "[common]" ha_copy_log_base)
N_COPYBASE[$L]=${N_COPYBASE[$L]:-$CUBRID_DATABASES}
build_steps
S_STATE[0]=done
steps_write
JOB[case]=$CASE_NO; JOB[db_names]="${D[*]}"; job_write
# the node checks (plan: only these, nothing changes)
for i in "${!S_ID[@]}"; do
  case ${S_ID[$i]} in node_check|preflight) ;; *) continue ;; esac
  step_set "$i" running
  if run_step "$i"; then
    step_set "$i" done
    [ "${S_ID[$i]}" = node_check ] && nc_set "${S_NODE[$i]}" ok ""
  else
    [ -n "${S_NODE[$i]}" ] && nc_set "${S_NODE[$i]}" "$FAIL_CODE" "$FAIL_MSG"
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
  if [ "$CANCEL" = 1 ]; then FAIL_CODE=cancelled; FAIL_MSG="cancelled before ${S_ID[$i]}${S_NODE[$i]:+ ${S_NODE[$i]}}${S_DB[$i]:+ ${S_DB[$i]}} (step $((i + 1)))"; failed_at=$i; break; fi
  step_set "$i" running
  JOB[step_no]=$((i + 1)); job_write
  log "step $((i + 1))/${#S_ID[@]} ${S_ID[$i]}${S_NODE[$i]:+ [${S_NODE[$i]}]}${S_DB[$i]:+ [${S_DB[$i]}]}"
  FAIL_CODE="" FAIL_MSG=""
  if master_guard "$i" && run_step "$i"; then
    step_set "$i" done
    if [ "${S_ID[$i]}" = convert_start_others ]; then
      JOB[convert_done]=1; CONVERT_LAST=$((i + 1)); job_write
      printf 'C\t%s\n' "$CONVERT_LAST" >> "$UNDO" || UNDO_FAILED=1
    fi
    continue
  fi
  if [ "$FAIL_CODE" = master_changed ]; then :
  elif [ "$TIMED_OUT" = 1 ]; then
    if may_still_run "${LAST_ACTION#* }"; then FAIL_CODE=no_response; FAIL_MSG="no answer in time: $FAIL_MSG"
    else FAIL_CODE=no_answer; FAIL_MSG="no answer in time from ${LAST_ACTION%% *} (${LAST_ACTION#* }), taken as a lost connection: $FAIL_MSG"
    fi
  elif [ "$NO_RESULT" = 1 ] && [ "$MAY_HAVE_RUN" = 1 ] && may_still_run "${LAST_ACTION#* }"; then
    # a lost connection, or another ssh failure, under a command that may still run there (V6): blocked
    FAIL_CODE=connection_lost; [ "$CONN_LOST" = 1 ] || FAIL_CODE=ssh_failed
    STOP_BLOCKED=1
    JOB[lost_nodes]="${LAST_ACTION%% *}"; JOB[lost_actions]="${LAST_ACTION} ${LAST_ARGS};"
    FAIL_MSG="no result from ${LAST_ACTION%% *} while ${LAST_ACTION#* } ran; it may still be running there: $FAIL_MSG"
  elif [ "$NO_RESULT" = 1 ]; then
    # a quick command: rolled back; a node whose connection is gone keeps its lines for a later rollback
    if [ "$CONN_LOST" = 1 ]; then FAIL_CODE=connection_lost; JOB[lost_nodes]="${LAST_ACTION%% *}"; JOB[lost_actions]="${LAST_ACTION} ${LAST_ARGS};"
    else FAIL_CODE=ssh_failed
    fi
  fi
  [ "$UNDO_FAILED" = 1 ] && { FAIL_CODE=undo_write_failed; FAIL_MSG="cannot write the undo log $UNDO: $FAIL_MSG"; }
  case $FAIL_CODE in no_response|connection_lost|ssh_failed|undo_write_failed|master_changed) ;; *)
    [ "$CANCEL" = 1 ] && { FAIL_CODE=cancelled; FAIL_MSG="cancelled during ${S_ID[$i]}${FAIL_MSG:+ ($FAIL_MSG)}"; } ;;
  esac
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
if [ "$FAIL_CODE" = no_response ] || [ "${STOP_BLOCKED:-0}" = 1 ]; then
  # a command did not come back and may still run: nothing is undone on top of it; the user checks the node
  # and calls rollback (the lock stays until then)
  finish rollback_blocked
fi
RB_NOTE=""
rollback_all; rb=$?
if [ $rb = 0 ]; then
  [ "$FAIL_CODE" = cancelled ] && finish cancelled
  finish fail
fi
[ -n "$RB_NOTE" ] && JOB[message]="$FAIL_MSG; $RB_NOTE"
[ $rb = 2 ] && finish rollback_blocked
finish rollback_failed
}
