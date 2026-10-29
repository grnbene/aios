#!/bin/sh
# `:focus` and `:focus-within`: a click on a field gives it the keyboard, its :focus border shows, and the form
# around it takes its :focus-within background. Drives the pane against web-corpus/focus.html, clicks the field,
# reads the boxes the engine painted after the click, and checks them against the golden.
#
#   scripts/drive-focus.sh <build-dir>            check against the golden
#   AIOS_BLESS=1 scripts/drive-focus.sh <build>   rewrite the golden (review the diff!)
set -eu

BUILD=${1:-build}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CORPUS="$HERE/web-corpus"
GOLDEN="$CORPUS/golden"
DRIVE="$BUILD/aios-web-pane-drive"

if [ ! -x "$DRIVE" ]; then
  echo "drive.focus: no $DRIVE — build aios-web-pane-drive first" >&2
  exit 1
fi

mkdir -p "$GOLDEN"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/root" "$WORK/out"
OUT="$WORK/focus.txt"
GOLD="$GOLDEN/focus.txt"

AIOS_CLICK_ID=who "$DRIVE" "$WORK/root" "$WORK/out" "file://$CORPUS/focus.html" > "$WORK/raw.txt" 2>"$WORK/raw.err"
# The field's border (#ff3300 focused, #c0c0c0 at rest) and the form's background (#fff5e0 with focus within it).
grep -E '^CLICKED' "$WORK/raw.txt" | grep -E '#ffff3300|#ffc0c0c0|#fffff5e0' > "$OUT" || true

if [ "${AIOS_BLESS:-0}" = "1" ]; then
  cp "$OUT" "$GOLD"
  echo "  blessed focus"
  exit 0
fi
if [ ! -f "$GOLD" ]; then
  echo "drive.focus: no golden at $GOLD (AIOS_BLESS=1 to write it)" >&2
  cat "$OUT" >&2
  exit 1
fi
grep -q '^CLICKED border .* #ffff3300' "$OUT" || { echo "drive.focus: the clicked field has no :focus border" >&2; cat "$OUT" >&2; exit 1; }
grep -q '^CLICKED rect .* #fffff5e0' "$OUT" || { echo "drive.focus: the form has no :focus-within background" >&2; cat "$OUT" >&2; exit 1; }
if ! diff -u "$GOLD" "$OUT"; then
  echo "drive.focus: differs from the golden (AIOS_BLESS=1 to accept)" >&2
  exit 1
fi
echo "  focus ok"
