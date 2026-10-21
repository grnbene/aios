#!/usr/bin/env bash
# Drives aios-git's file watcher the way a user meets it, on whatever system this runs on (Linux, macOS, Windows under
# Git Bash): a repository in the change layer, the daemon started, files edited, and then what it must have done --
# @ snapshotted without a command, every saved version journaled, `status` answered from the watcher, two commands at
# once serialized by the repository lock, the daemon stopped.
#
#   tools/aios-git/drive-fsmonitor.sh <path to aios-git>
set -euo pipefail
G="$1"
[ -x "$G" ] || { echo "no aios-git at $G" >&2; exit 2; }
say() { printf '\n== %s\n' "$*"; }
fail() { echo "FAILED: $*" >&2; exit 1; }
export GIT_AUTHOR_NAME=drive GIT_AUTHOR_EMAIL=drive@example.com GIT_COMMITTER_NAME=drive GIT_COMMITTER_EMAIL=drive@example.com
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1
W="$(mktemp -d)"
cd "$W"
"$G" init -q repo
cd repo
echo one > a.txt
"$G" add a.txt
"$G" commit -qm first
"$G" change init > /dev/null

say "start the watcher"
"$G" fsmonitor--daemon start
trap '"$G" fsmonitor--daemon stop >/dev/null 2>&1 || true' EXIT
"$G" fsmonitor--daemon status

say "edit, and let the edits settle"
echo two > a.txt
sleep 0.4
echo three > a.txt
mkdir -p sub/deeper
echo new > sub/deeper/b.txt
for i in $(seq 1 50); do
  st="$("$G" fsmonitor--daemon status)"
  snaps="$(printf '%s\n' "$st" | sed -n 's/^snapshots \([0-9]*\).*/\1/p')"
  kept="$(printf '%s\n' "$st" | sed -n 's/^journal \([0-9]*\).*/\1/p')"
  [ "${snaps:-0}" -ge 1 ] && [ "${kept:-0}" -ge 3 ] && break
  sleep 0.2
done
printf '%s\n' "$st"
[ "${snaps:-0}" -ge 1 ] || fail "the watcher took no snapshot of @"
[ "${kept:-0}" -ge 3 ] || fail "the journal kept $kept versions (two of a.txt and b.txt expected)"

say "@ holds the files as they are now, with no command run"
for i in $(seq 1 25); do
  [ "$("$G" show HEAD:a.txt 2>/dev/null || true)" = three ] && [ "$("$G" show HEAD:sub/deeper/b.txt 2>/dev/null || true)" = new ] && break
  sleep 0.2
done
[ "$("$G" show HEAD:a.txt)" = three ] || fail "@ has a.txt as '$("$G" show HEAD:a.txt)'"
[ "$("$G" show HEAD:sub/deeper/b.txt)" = new ] || fail "@ lacks sub/deeper/b.txt (a directory made after the watcher started)"

say "every saved version is in the journal"
"$G" journal a.txt
"$G" journal a.txt | grep -q . || fail "git journal lists nothing for a.txt"
n="$("$G" journal a.txt | grep -c .)"
[ "$n" -ge 2 ] || fail "only $n version(s) of a.txt journaled"

say "a command looks where the watcher says: a new file reaches @ through status's own snapshot"
# (the daemon's own snapshots off, so only the command's snapshot, which stats what the watcher names, can find it)
"$G" config change.autosnapshot false
"$G" fsmonitor--daemon stop
"$G" fsmonitor--daemon start > /dev/null
"$G" status > /dev/null   # the first look after a start is "everything"; from here on, what changed
echo loose > loose.txt
echo four > a.txt
sleep 0.3
"$G" status > /dev/null
[ "$("$G" show HEAD:loose.txt 2>/dev/null || true)" = loose ] || fail "status's snapshot did not see loose.txt"
[ "$("$G" show HEAD:a.txt)" = four ] || fail "status's snapshot did not see a.txt change"

say "two commands at once: the repository lock serializes them"
"$G" change describe -m "from one" > /dev/null &
"$G" change new > /dev/null &
wait
"$G" change log -n 3 | head -8
"$G" op log | head -4

say "stop"
"$G" fsmonitor--daemon stop
trap - EXIT
"$G" fsmonitor--daemon status && fail "the daemon still answers after stop" || true
echo
echo "drive passed"
