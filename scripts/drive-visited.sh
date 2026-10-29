#!/bin/sh
# `:visited`: a link whose address this browser has been to takes its :visited colour. Drives the pane against
# web-corpus/visited.html, follows the same-page link (its address is then visited), reads the link texts painted
# after the click, and checks them against the golden: the followed link in the :visited colour, the other not.
#
#   scripts/drive-visited.sh <build-dir>            check against the golden
#   AIOS_BLESS=1 scripts/drive-visited.sh <build>   rewrite the golden (review the diff!)
set -eu

BUILD=${1:-build}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CORPUS="$HERE/web-corpus"
GOLDEN="$CORPUS/golden"
DRIVE="$BUILD/aios-web-pane-drive"

if [ ! -x "$DRIVE" ]; then
  echo "drive.visited: no $DRIVE — build aios-web-pane-drive first" >&2
  exit 1
fi

mkdir -p "$GOLDEN"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/root" "$WORK/out"
OUT="$WORK/visited.txt"
GOLD="$GOLDEN/visited.txt"

AIOS_CLICK_ID=go "$DRIVE" "$WORK/root" "$WORK/out" "file://$CORPUS/visited.html" > "$WORK/raw.txt" 2>"$WORK/raw.err"
# The two links' text after the click: #681da8 is the :visited colour, #1a0dab the unvisited one.
grep -E '^CLICKED text' "$WORK/raw.txt" | grep -E 'link into this page|one to another page' > "$OUT" || true

if [ "${AIOS_BLESS:-0}" = "1" ]; then
  cp "$OUT" "$GOLD"
  echo "  blessed visited"
  exit 0
fi
if [ ! -f "$GOLD" ]; then
  echo "drive.visited: no golden at $GOLD (AIOS_BLESS=1 to write it)" >&2
  cat "$OUT" >&2
  exit 1
fi
grep -q 'link into this page' "$OUT" && grep 'link into this page' "$OUT" | grep -q '#ff681da8' || { echo "drive.visited: the followed link did not take the :visited colour" >&2; cat "$OUT" >&2; exit 1; }
grep 'one to another page' "$OUT" | grep -q '#ff1a0dab' || { echo "drive.visited: the unvisited link lost its colour" >&2; cat "$OUT" >&2; exit 1; }
if ! diff -u "$GOLD" "$OUT"; then
  echo "drive.visited: differs from the golden (AIOS_BLESS=1 to accept)" >&2
  exit 1
fi
echo "  visited ok"
