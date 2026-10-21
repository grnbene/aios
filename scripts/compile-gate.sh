#!/bin/bash
# compile-gate.sh: does the C++ a change touched still compile? The check converge runs on every submission and train.
#
#   CONVERGE_BASE=<commit> scripts/compile-gate.sh      run in a checkout of the commit to check
#
# Every .cpp the change added or modified, and every .cpp that includes a header it changed, is parsed and type-checked
# with g++ -fsyntax-only (no code generation, no linking: seconds, not a build). A file that fails is checked again at
# the base commit; one that fails there too is not this change's fault and is reported but not counted.
#
#   COMPILE_GATE_JOBS  parallel compiles (default 3)      COMPILE_GATE_MAX  most files to compile (default 80)
set -u
cd "$(git rev-parse --show-toplevel)"
base=${CONVERGE_BASE:-}
[ -n "$base" ] || { echo "gate: no base commit given: nothing to compare with"; exit 0; }
git cat-file -e "$base^{commit}" 2>/dev/null || { echo "gate: base ${base:0:8} is not in this checkout: nothing to compare with"; exit 0; }
command -v g++ >/dev/null || { echo "gate: no g++ here: not checked"; exit 0; }

jobs=${COMPILE_GATE_JOBS:-3}
max=${COMPILE_GATE_MAX:-80}
flags=(-std=c++20 -fsyntax-only -w -Iinclude -Ithird_party/tree-sitter/lib/include -I/usr/include/SDL2
       -DAIOS_HAS_SDL2=1 -DAIOS_HAVE_LIBDEFLATE=1 -DAIOS_HAVE_ZLIB=1 -DAIOS_HAVE_ZSTD=1)

tmp=$(mktemp -d); trap 'git worktree remove --force "$tmp/base" 2>/dev/null; rm -rf "$tmp"' EXIT

git diff --name-only --diff-filter=AM -z "$base" HEAD | tr '\0' '\n' > "$tmp/changed"
grep -E '\.(cpp|cc)$' "$tmp/changed" | grep -vE '^(third_party|build)/|(^|/)font_atlas\.cpp$|\.c$' | sort -u > "$tmp/tus"
grep -E '\.(hpp|hh|h|inc)$' "$tmp/changed" | grep -v '^third_party/' > "$tmp/hdrs"

# Translation units that include a changed header, by the include's path under include/ or by its file name.
if [ -s "$tmp/hdrs" ]; then
  while read -r h; do
    rel=${h#include/}
    git grep -l -E "#[[:space:]]*include[[:space:]]*[\"<]([^\">]*/)?$(basename "$h")[\">]" -- '*.cpp' '*.cc' '*.hpp' '*.inc' 2>/dev/null |
      grep -vE '^(third_party|build)/' >> "$tmp/inc"
    : "$rel"
  done < "$tmp/hdrs"
  # An includer that is itself a header is not compiled alone; its own includers are not followed (one level is
  # what a header edit breaks in practice, and the cap below keeps a wide header from compiling the whole tree).
  grep -E '\.(cpp|cc)$' "$tmp/inc" 2>/dev/null | sort -u >> "$tmp/tus"
  sort -u -o "$tmp/tus" "$tmp/tus"
fi

n=$(wc -l < "$tmp/tus")
[ "$n" -gt 0 ] || { echo "PASS gate: no C++ translation unit is touched by this change ($(wc -l < "$tmp/changed") files changed)"; exit 0; }
if [ "$n" -gt "$max" ]; then
  echo "gate: $n translation units are touched, checking the first $max (COMPILE_GATE_MAX)"
  head -n "$max" "$tmp/tus" > "$tmp/tus2"; mv "$tmp/tus2" "$tmp/tus"; n=$max
fi

FLAGS="${flags[*]}"
export FLAGS tmp
# shellcheck disable=SC2016
xargs -a "$tmp/tus" -d '\n' -P "$jobs" -I{} bash -c 'out=$(g++ $FLAGS "$1" 2>&1) && exit 0; { echo "== $1"; echo "$out" | grep -E "error" | head -6; } > "$tmp/fail.$(echo "$1" | tr / _)"' _ {}

ls "$tmp"/fail.* >/dev/null 2>&1 || { echo "PASS gate: $n C++ file(s) compile ($(wc -l < "$tmp/changed") changed)"; exit 0; }

# Which failures are new? Compile the failing files again at the base commit.
git worktree add --detach -f "$tmp/base" "$base" >/dev/null 2>&1
mine=0; old=0
for f in "$tmp"/fail.*; do
  file=$(head -1 "$f" | sed 's/^== //')
  if [ -f "$tmp/base/$file" ] && (cd "$tmp/base" && g++ $FLAGS "$file" >/dev/null 2>&1); then
    mine=$((mine+1)); cat "$f"
  elif [ -f "$tmp/base/$file" ]; then
    old=$((old+1)); echo "(already failing at ${base:0:8}, not counted: $file)"
  else
    mine=$((mine+1)); cat "$f"
  fi
done
if [ "$mine" -gt 0 ]; then echo "FAIL gate: $mine C++ file(s) no longer compile (checked $n)"; exit 1; fi
echo "PASS gate: $n checked, $old already failing at the base"
