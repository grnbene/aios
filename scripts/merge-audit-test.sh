#!/bin/bash
# merge-audit-test.sh [aios-git]: clean line merges that are wrong unit by unit, and ones that are right, through a real
# `git merge -X structural` (see git_structural_audit). Exits non-zero on the first case that comes out wrong.
GIT=${1:-$(dirname "$0")/../build/aios-git}
GIT=$(cd "$(dirname "$GIT")" && pwd)/$(basename "$GIT")
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fails=0

# case <name> <file> <expect: clean|conflict> <base> <ours> <theirs> [<expected merged text, for clean>]
case_() {
  local name=$1 file=$2 expect=$3 base=$4 ours=$5 theirs=$6 want=${7-}
  local d=$T/$name; mkdir -p "$d"; cd "$d" || exit 1
  "$GIT" init -q -b main . && "$GIT" config user.name t && "$GIT" config user.email t@t
  printf '%s' "$base" > "$file"; "$GIT" add "$file"; "$GIT" commit -qm base
  "$GIT" checkout -qb theirs; printf '%s' "$theirs" > "$file"; "$GIT" commit -qam theirs
  "$GIT" checkout -q main; printf '%s' "$ours" > "$file"; "$GIT" commit -qam ours
  local out; out=$("$GIT" merge -X structural --no-edit theirs 2>&1); local rc=$?
  local got=clean; [ $rc -ne 0 ] && got=conflict
  if [ "$got" != "$expect" ]; then
    echo "FAIL $name: expected $expect, got $got"; echo "$out" | head -5; sed 's/^/    | /' "$file" | head -30; fails=$((fails + 1)); return
  fi
  if [ "$expect" = clean ] && [ -n "$want" ] && [ "$(cat "$file")" != "$(printf '%s' "$want")" ]; then
    echo "FAIL $name: merged text differs"; diff <(printf '%s' "$want") "$file" | head -20; fails=$((fails + 1)); return
  fi
  echo "ok   $name ($got)"
}

# Both sides add a function of one name at different places: lines merge cleanly into a duplicate.
case_ js-dup-different app.js conflict \
'function a() { return 1; }

function z() { return 26; }
' \
'function helper() { return "ours"; }

function a() { return 1; }

function z() { return 26; }
' \
'function a() { return 1; }

function z() { return 26; }

function helper() { return "theirs"; }
'

# The same function added identically by both, at different places: one copy, not two.
case_ js-dup-identical app.js clean \
'function a() { return 1; }

function z() { return 26; }
' \
'function helper() { return 0; }

function a() { return 1; }

function z() { return 26; }
' \
'function a() { return 1; }

function z() { return 26; }

function helper() { return 0; }
' \
'function helper() { return 0; }

function a() { return 1; }

function z() { return 26; }
'

# C++: both add bloom_of_ancestors-like helpers of one name and signature, different bodies.
case_ cpp-dup-different m.cpp conflict \
'int a() { return 1; }

int z() { return 26; }
' \
'static int helper(int x) { return x; }

int a() { return 1; }

int z() { return 26; }
' \
'int a() { return 1; }

int z() { return 26; }

static int helper(int x) { return x + 1; }
'

# C++ overloads (different parameters) added by each side are two units, not a duplicate.
case_ cpp-overloads m.cpp clean \
'int a() { return 1; }

int z() { return 26; }
' \
'int f(int x) { return x; }

int a() { return 1; }

int z() { return 26; }
' \
'int a() { return 1; }

int z() { return 26; }

int f(double x) { return 2; }
'

# Each side edits a different function: the ordinary case must stay clean and keep both edits.
case_ cpp-two-edits m.cpp clean \
'int a() {
  return 1;
}

int b() {
  return 2;
}
' \
'int a() {
  return 10;
}

int b() {
  return 2;
}
' \
'int a() {
  return 1;
}

int b() {
  return 20;
}
' \
'int a() {
  return 10;
}

int b() {
  return 20;
}
'

# Both sides edit one function in different lines: the line merge combines them; not the audit's to refuse.
case_ cpp-same-unit-both m.cpp clean \
'int a() {
  int x = 1;
  int y = 2;
  int z = 3;
  return x + y + z;
}
' \
'int a() {
  int x = 100;
  int y = 2;
  int z = 3;
  return x + y + z;
}
' \
'int a() {
  int x = 1;
  int y = 2;
  int z = 300;
  return x + y + z;
}
'

# Python: two sides each define one method name in a class.
case_ py-dup-method m.py conflict \
'class C:
    def a(self):
        return 1

    def z(self):
        return 26
' \
'class C:
    def run(self):
        return "ours"

    def a(self):
        return 1

    def z(self):
        return 26
' \
'class C:
    def a(self):
        return 1

    def z(self):
        return 26

    def run(self):
        return "theirs"
'

[ $fails -eq 0 ] && echo "all merge audit cases pass" || { echo "$fails case(s) failed"; exit 1; }
