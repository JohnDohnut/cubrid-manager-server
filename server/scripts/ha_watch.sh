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
# ha_watch.sh - runs ha_add_nodes.sh (already open on fd 3, hashed by the manager) and, however it ends, closes the
# job's ssh connections (one ControlMaster per node) and removes their socket folder.
#   ha_watch.sh --ssh-config <file> --cp-dir <folder> -- <ha_add_nodes.sh arguments...>
# A cancel goes to the script's pid (job file), not to this shell.

main() {
  local cfg="" cp="" pid h
  while [ $# -gt 0 ]; do
    case $1 in
      --ssh-config) cfg=$2; shift 2 ;;
      --cp-dir) cp=$2; shift 2 ;;
      --) shift; break ;;
      *) echo "ha_watch.sh: unknown argument $1" >&2; return 2 ;;
    esac
  done
  [ -f "$cfg" ] && [ -n "$cp" ] || { echo "ha_watch.sh: --ssh-config and --cp-dir are required" >&2; return 2; }
  # A handler that does nothing, not an ignored signal: a signal ignored here would stay ignored in the script,
  # and bash cannot trap a signal that was ignored when it started (the cancel would do nothing).
  trap ':' TERM INT HUP
  bash /dev/fd/3 "$@" &
  pid=$!
  exec 3<&-
  # the handler makes wait return early: wait again until the script is gone
  while kill -0 "$pid" 2>/dev/null; do wait "$pid"; done
  # the node blocks only (not patterns such as the common "Host *")
  for h in $(awk '$1 == "Host" { for (i = 2; i <= NF; i++) if ($i !~ /[*?!]/) print $i }' "$cfg"); do
    ssh -F "$cfg" -O exit "$h" </dev/null >/dev/null 2>&1
  done
  case $cp in */hs?*) rm -rf -- "$cp" ;; esac
  return 0
}
main "$@"
exit $?
