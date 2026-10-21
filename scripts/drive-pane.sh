#!/bin/sh
# Drives the web pane against a local corpus page with clicks and keystrokes,
# and checks the output against its golden.
#
#   scripts/drive-pane.sh <build-dir>            check against the golden
#   AIOS_BLESS=1 scripts/drive-pane.sh <build>   rewrite the golden
set -eu

BUILD=${1:-build}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CORPUS="$HERE/web-corpus"
GOLDEN="$CORPUS/golden"
DRIVE="$BUILD/aios-web-pane-drive"

if [ ! -x "$DRIVE" ]; then
  echo "drive.pane: no $DRIVE — build aios-web-pane-drive first" >&2
  exit 1
fi

mkdir -p "$GOLDEN"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/root" "$WORK/out"
PAGE="file://$CORPUS/form.html"
OUT="$WORK/pane.txt"
GOLD="$GOLDEN/pane.txt"

# Run with clicks and keystrokes: click #who field, type name, press Enter
AIOS_CLICK_ID=who AIOS_TYPE=" Lovelace" AIOS_KEYS=Enter \
  "$DRIVE" "$WORK/root" "$WORK/out" "$PAGE" > "$WORK/raw.txt" 2>"$WORK/raw.err"

# Sanitize machine-specific paths (output directory and corpus location)
sed -e "s|$WORK/out|OUTDIR|g" -e "s|$CORPUS|CORPUS|g" "$WORK/raw.txt" > "$OUT"

if [ "${AIOS_BLESS:-0}" = "1" ]; then
  cp "$OUT" "$GOLD"
  echo "  blessed pane"
  exit 0
fi

if [ ! -f "$GOLD" ]; then
  echo "drive.pane: no golden $GOLD — run AIOS_BLESS=1 $0 $BUILD" >&2
  exit 1
fi

if ! diff -u "$GOLD" "$OUT" > "$WORK/pane.diff"; then
  echo "drive.pane: output does not match its golden" >&2
  sed 's/^/    /' "$WORK/pane.diff" >&2
  exit 1
fi

echo "drive.pane: checks match golden"
