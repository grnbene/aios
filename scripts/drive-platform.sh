#!/bin/sh
# The platform a page script runs against, exercised through the engine that runs
# it. The layout driver cannot do this -- it runs no script -- so this drives
# the pane, which does.
#
# The page checks itself and writes PASS/FAIL lines into an element; this reads
# them back out of the fragments the engine produced. A test that asks the
# engine what it rendered cannot be fooled by the engine's own reporting.
#
#   scripts/drive.platform.sh <build-dir>
set -eu
BUILD=${1:-build}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PAGE="$HERE/web-corpus/platform.html"
DRIVE="$BUILD/aios-web-pane-drive"
[ -x "$DRIVE" ] || { echo "drive.platform: no $DRIVE" >&2; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

AIOS_PANE_DUMP=1 "$DRIVE" "$WORK/root" "$WORK" "file://$PAGE" 2>/dev/null \
  | grep -oE '"(PASS|FAIL)[^"]*"' | sed 's/"//g' > "$WORK/results" || true

pass=$(grep -c '^PASS' "$WORK/results" || true)
fail=$(grep -c '^FAIL' "$WORK/results" || true)

if [ "$pass" -eq 0 ]; then
  echo "drive.platform: the page produced no results at all -- the script did not run" >&2
  exit 1
fi
if [ "$fail" -ne 0 ]; then
  echo "drive.platform: $fail of $((pass + fail)) checks failed" >&2
  grep '^FAIL' "$WORK/results" | sed 's/^/    /' >&2
  exit 1
fi
echo "drive.platform: $pass platform checks pass"
