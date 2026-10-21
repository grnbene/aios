#!/bin/sh
# The web engine against the frozen corpus.
#
# Lay every page out at two widths and diff the layout tree against the golden
# beside it. Text, not pixels: a reviewer can read the diff, and a change to the
# font atlas does not churn every file — the driver uses synthetic metrics, so a
# golden records what the CASCADE decided, which is the thing under test.
#
#   scripts/drive-web.sh <build-dir>            check against the goldens
#   AIOS_BLESS=1 scripts/drive-web.sh <build>   rewrite the goldens (review the diff!)
set -eu

BUILD=${1:-build}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CORPUS="$HERE/web-corpus"
GOLDEN="$CORPUS/golden"
DRIVE="$BUILD/aios-web-layout-drive"

if [ ! -x "$DRIVE" ]; then
  echo "drive.web: no $DRIVE — build aios-web-layout-drive first" >&2
  exit 1
fi

mkdir -p "$GOLDEN"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# Two widths, because a reflow is where a layout bug actually shows: 1024 is a
# window, 420 is the phone width every wrapping rule is written for.
# The second is a phone; the third carries a HEIGHT, because `100vh` and a
# `height: 100%` chain -- an application shell -- have nothing to resolve
# against without one.
WIDTHS="1024 420 900x600"
fail=0
checked=0

for page in "$CORPUS"/*.html; do
  name=$(basename "$page" .html)
  for w in $WIDTHS; do
    out="$WORK/$name.$w.txt"
    gold="$GOLDEN/$name.$w.txt"
    if ! "$DRIVE" "$w" "$page" > "$out" 2>"$WORK/$name.$w.err"; then
      echo "drive.web: FAILED to lay out $name at ${w}px" >&2
      sed 's/^/    /' "$WORK/$name.$w.err" >&2
      fail=$((fail + 1))
      continue
    fi
    checked=$((checked + 1))
    if [ "${AIOS_BLESS:-0}" = "1" ]; then
      cp "$out" "$gold"
      echo "  blessed $name at ${w}px"
      continue
    fi
    if [ ! -f "$gold" ]; then
      echo "drive.web: no golden for $name at ${w}px — run AIOS_BLESS=1 $0 $BUILD" >&2
      fail=$((fail + 1))
      continue
    fi
    if ! diff -u "$gold" "$out" > "$WORK/$name.$w.diff"; then
      echo "drive.web: $name at ${w}px does not match its golden" >&2
      sed 's/^/    /' "$WORK/$name.$w.diff" >&2
      fail=$((fail + 1))
    fi
  done
done

# What a submitted form would ask for. The request is the contract between the
# page and the server, and it is worth holding to a golden without needing a
# server on the other end.
FORMS="$BUILD/aios-form-check"
if [ -x "$FORMS" ]; then
  for page in "$CORPUS"/form.html; do
    name=$(basename "$page" .html)
    out="$WORK/$name.forms.txt"
    gold="$GOLDEN/$name.forms.txt"
    "$FORMS" "$page" "http://example.test/page?old=query" > "$out"
    checked=$((checked + 1))
    if [ "${AIOS_BLESS:-0}" = "1" ]; then
      cp "$out" "$gold"
      echo "  blessed $name forms"
    elif [ ! -f "$gold" ]; then
      echo "drive.web: no golden for $name forms" >&2
      fail=$((fail + 1))
    elif ! diff -u "$gold" "$out" > "$WORK/$name.forms.diff"; then
      echo "drive.web: $name forms do not match their golden" >&2
      sed 's/^/    /' "$WORK/$name.forms.diff" >&2
      fail=$((fail + 1))
    fi
  done
fi

if [ "$fail" -ne 0 ]; then
  echo "drive.web: $fail of $checked checks differ" >&2
  exit 1
fi
echo "drive.web: $checked checks match their goldens"
