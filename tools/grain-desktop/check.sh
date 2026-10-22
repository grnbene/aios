#!/bin/bash
# grain programs on the aios desktop, checked headless (no screen needed).
#
#   tools/grain-desktop/check.sh [path/to/aios]      default: build/aios
#
# Each scenario opens a program with `grain open` in `aios desktop --headless`,
# drives it with scripted input (the desktop's own --do steps: clicks, typing,
# keys, the wheel, a corner drag, the close button) and compares the events
# the program logged with what it must have received.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root_dir="$(cd "$here/../.." && pwd)"
aios="${1:-$root_dir/build/aios}"
[ -x "$aios" ] || { echo "no aios binary at $aios"; exit 2; }
work="$(mktemp -d "${TMPDIR:-/tmp}/grain-desktop.XXXXXX")"
trap 'rm -rf "$work"' EXIT
cp "$here"/*.gr "$work"/
mkdir -p "$work/root"
bad=0

run_desktop() {   # frames, then --do steps
  local frames=$1; shift
  local args=()
  for step in "$@"; do args+=(--do "$step"); done
  "$aios" --root "$work/root" desktop --headless --size 1280x800 --frames "$frames" --dump "$work/last.ppm" "${args[@]}" > "$work/desktop.log" 2>&1
}

expect() {   # name, file, expected text
  if [ "$(cat "$2" 2>/dev/null)" == "$3" ]; then echo "  ok    $1"; else
    echo "  FAIL  $1"; echo "--- expected"; echo "$3"; echo "--- got"; cat "$2" 2>/dev/null; bad=1; fi
}

# The window is created at (140, 90); its content starts one pixel in and 26 down: (141, 116).
run_desktop 40 "2:cmd:grain open $work/demo.gr" "8:click:241,166" "10:click:251,170" "12:type:hi" \
  "13:key:Enter" "14:wheel:-2" "20:key:Escape"
expect "clicks, typing, keys, wheel and Escape reach the program" "$work/events.log" "mouse_down at 100,50
mouse_up at 100,50
click at 100,50
mouse_down at 110,54
mouse_up at 110,54
click at 110,54
text \"hi\"
key return
wheel dy=-2.0
key escape
typed: hi, clicks: 2"

# Drag the bottom-right corner 100 right and 80 down: the pane follows, 200x120 -> 300x200.
run_desktop 30 "2:cmd:grain open $work/resize_close.gr" "8:down:340,236" "10:move:440,316" "12:up:440,316"
expect "a corner drag resizes the pane" "$work/rc.log" "resize 300x200"

# Its close button: the program hears close and ends.
run_desktop 30 "2:cmd:grain open $work/resize_close.gr" "10:click:330,103"
expect "the close button closes the program" "$work/rc.log" "close"

# A draw callback that fails: the desktop carries on and later input still works.
run_desktop 40 "2:cmd:grain open $work/broken.gr" "8:cmd:grain open $work/demo.gr" "14:key:Escape"
expect "a failing program does not stop another" "$work/events.log" "key escape
typed: , clicks: 0"

exit $bad
