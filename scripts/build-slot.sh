#!/bin/sh
# build-slot.sh <command...>: run a compile in one of the machine's build slots, at idle priority.
#
# Agents' forks and converge's checks all compile on this one box, and the people using it come first: at most
# AIOS_BUILD_SLOTS compiles (default 2 of the 4 cores) run at once machine-wide, each at nice 19 and idle I/O, however
# many agents ask. A slot is a flock on /var/tmp/aios-build-slots/slot<N> (made here if missing, world-writable so the
# checks' jail, which can read but not create there, can still take it). The command's exit status is returned.
SLOTS=${AIOS_BUILD_SLOTS:-2}
DIR=/var/tmp/aios-build-slots
[ $# -ge 1 ] || { echo "usage: build-slot.sh <command...>" >&2; exit 2; }
if [ ! -d "$DIR" ]; then mkdir -p "$DIR" 2>/dev/null && chmod 1777 "$DIR" 2>/dev/null; fi
i=0
while [ "$i" -lt "$SLOTS" ]; do
  [ -e "$DIR/slot$i" ] || { : > "$DIR/slot$i" 2>/dev/null && chmod 666 "$DIR/slot$i" 2>/dev/null; }
  i=$((i + 1))
done
low() { nice -n 19 ionice -c 3 "$@" 2>/dev/null || nice -n 19 "$@"; }
command -v flock >/dev/null 2>&1 && [ -r "$DIR/slot0" ] || { low "$@"; exit $?; }   # no flock here: idle priority only
# A free slot now if there is one; otherwise wait for the slot this process hashes to.
i=0
while [ "$i" -lt "$SLOTS" ]; do
  flock -n -E 75 "$DIR/slot$i" nice -n 19 ionice -c 3 "$@"; rc=$?
  [ "$rc" -ne 75 ] && exit "$rc"
  i=$((i + 1))
done
exec flock "$DIR/slot$(( $$ % SLOTS ))" nice -n 19 ionice -c 3 "$@"
