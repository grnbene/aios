#!/bin/sh
# `:active`: a button pressed and held takes its :active style, and loses it on release. Drives the pane against
# web-corpus/active.html, holds the button down, reads the fragments the engine painted while it was down and after
# it came up, and checks them against the golden.
#
#   scripts/drive-active.sh <build-dir>            check against the golden
#   AIOS_BLESS=1 scripts/drive-active.sh <build>   rewrite the golden (review the diff!)
set -eu

BUILD=${1:-build}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CORPUS="$HERE/web-corpus"
GOLDEN="$CORPUS/golden"
DRIVE="$BUILD/aios-web-pane-drive"

if [ ! -x "$DRIVE" ]; then
  echo "drive.active: no $DRIVE — build aios-web-pane-drive first" >&2
  exit 1
fi

mkdir -p "$GOLDEN"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/root" "$WORK/out"
OUT="$WORK/active.txt"
GOLD="$GOLDEN/active.txt"

AIOS_PRESS_ID=go "$DRIVE" "$WORK/root" "$WORK/out" "file://$CORPUS/active.html" > "$WORK/raw.txt" 2>"$WORK/raw.err"
# The button's box while pressed and after release, by its colour: #083d66 down (the :active rule), #0f6ab0 up.
grep -E '^(PRESSED|RELEASED)' "$WORK/raw.txt" | grep -E '#ff083d66|#ff0f6ab0|#ff1b7fcc|"Go"' > "$OUT" || true

if [ "${AIOS_BLESS:-0}" = "1" ]; then
  cp "$OUT" "$GOLD"
  echo "  blessed active"
  exit 0
fi
if [ ! -f "$GOLD" ]; then
  echo "drive.active: no golden at $GOLD (AIOS_BLESS=1 to write it)" >&2
  cat "$OUT" >&2
  exit 1
fi
# What the test is about, independent of exact pixels: pressed = the :active colour, released = the rest colour.
grep -q '^PRESSED rect .* #ff083d66' "$OUT" || { echo "drive.active: no :active-coloured box while the button was down" >&2; cat "$OUT" >&2; exit 1; }
grep -q '^RELEASED rect .* #ff0f6ab0\|^RELEASED rect .* #ff1b7fcc' "$OUT" || { echo "drive.active: the box kept the :active colour after release" >&2; cat "$OUT" >&2; exit 1; }
if ! diff -u "$GOLD" "$OUT"; then
  echo "drive.active: differs from the golden (AIOS_BLESS=1 to accept)" >&2
  exit 1
fi
echo "  active ok"
