#!/bin/sh
# A same-page anchor: following `href="#second"` scrolls the page to that element and `:target` matches it. Drives
# the pane against web-corpus/target.html, clicks the link, reads where the page sits and the boxes painted after
# the click, and checks them against the golden.
#
#   scripts/drive-target.sh <build-dir>            check against the golden
#   AIOS_BLESS=1 scripts/drive-target.sh <build>   rewrite the golden (review the diff!)
set -eu

BUILD=${1:-build}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CORPUS="$HERE/web-corpus"
GOLDEN="$CORPUS/golden"
DRIVE="$BUILD/aios-web-pane-drive"

if [ ! -x "$DRIVE" ]; then
  echo "drive.target: no $DRIVE — build aios-web-pane-drive first" >&2
  exit 1
fi

mkdir -p "$GOLDEN"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/root" "$WORK/out"
OUT="$WORK/target.txt"
GOLD="$GOLDEN/target.txt"

AIOS_CLICK_ID=go "$DRIVE" "$WORK/root" "$WORK/out" "file://$CORPUS/target.html" > "$WORK/raw.txt" 2>"$WORK/raw.err"
# The heading's box (#ffe08a once it is the target, #e8e8e8 before) and the scroll position after the click.
grep -E '^(CLICKED|SCROLL)' "$WORK/raw.txt" | grep -E '#ffffe08a|#ffe8e8e8|^SCROLL' > "$OUT" || true

if [ "${AIOS_BLESS:-0}" = "1" ]; then
  cp "$OUT" "$GOLD"
  echo "  blessed target"
  exit 0
fi
if [ ! -f "$GOLD" ]; then
  echo "drive.target: no golden at $GOLD (AIOS_BLESS=1 to write it)" >&2
  cat "$OUT" >&2
  exit 1
fi
grep -q '^CLICKED rect .* #ffffe08a' "$OUT" || { echo "drive.target: the heading did not take its :target colour" >&2; cat "$OUT" >&2; exit 1; }
scroll=$(sed -n 's/^SCROLL \([0-9]*\).*/\1/p' "$OUT" | head -1)
[ "${scroll:-0}" -gt 1000 ] || { echo "drive.target: the page did not scroll to the heading (SCROLL ${scroll:-?})" >&2; cat "$OUT" >&2; exit 1; }
if ! diff -u "$GOLD" "$OUT"; then
  echo "drive.target: differs from the golden (AIOS_BLESS=1 to accept)" >&2
  exit 1
fi
echo "  target ok"
