#!/bin/sh
# tree-build.sh <target...>: build the shared live tree in the agents' build directory, one build at a time.
#
# Agents build into build-agents/ (-O1, no debug info: a 47k-line file recompiles in a fraction of what the owner's -O3
# build/ takes, and the owner's binaries are left alone). Two makes in one directory corrupt each other's objects, so
# builds take build-agents/.tree-build.lock in turn (the wait is printed). The build runs in one of the machine's build
# slots at idle priority (scripts/build-slot.sh), -j2. It builds the tree as it is now: everyone's latest writes.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
[ $# -ge 1 ] || { echo "usage: tree-build.sh <target...>   (e.g. aios-asql-memory-drive)"; exit 2; }
B="$ROOT/build-agents"
[ -f "$B/Makefile" ] || (mkdir -p "$B" && cd "$B" && cmake .. -DCMAKE_BUILD_TYPE=None -DCMAKE_CXX_FLAGS="-O1 -g0" -DCMAKE_C_FLAGS="-O1 -g0" >/dev/null)
exec 9>"$B/.tree-build.lock"
# While waiting, and while building, it keeps printing (a line every 20 s at least): the agents' command runner kills a
# command that goes quiet, and a build waiting its turn or compiling one big file is not stuck.
if ! flock -n 9; then
  echo "tree-build: another agent is building; waiting for it"
  while ! flock -w 20 9; do echo "tree-build: still waiting for the other build ($(date +%H:%M:%S))"; done
fi
cd "$B" || exit 1
out=$(mktemp)
( while sleep 20; do echo "tree-build: compiling ($(date +%H:%M:%S)) $(grep -c 'Building' "$out" 2>/dev/null) object(s) so far"; done ) & beat=$!
"$ROOT/scripts/build-slot.sh" make -j2 "$@" > "$out" 2>&1; rc=$?
kill $beat 2>/dev/null
grep -E "error|Built target" "$out" | tail -40
rm -f "$out"
exit $rc
