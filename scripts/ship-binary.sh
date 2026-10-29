#!/usr/bin/env bash
# ship-binary.sh <built aios> [out dir]: the binary to deploy, without its symbol table, and the symbols kept
# beside it to read a crash with.
#
#   aios-<commit>        stripped: what goes to a machine (about 4.8 MB smaller)
#   aios-<commit>.sym    its symbols, tied to it by build id (gnu-debuglink)
#
# Nothing at run time reads the symbol table: a static aios has no dynamic symbols, so a crash log's backtrace
# (crash_log.cpp) is addresses either way. Read one with the .sym file:
#   addr2line -f -C -e aios-<commit>.sym 0x4a1b2c
#   gdb aios-<commit> core   (finds aios-<commit>.sym beside it)
set -euo pipefail

bin=${1:?usage: ship-binary.sh <built aios> [out dir]}
out=${2:-$(dirname "$bin")}
here=$(cd "$(dirname "$0")/.." && pwd)
rev=$(git -C "$here" rev-parse --short HEAD)
[ -z "$(git -C "$here" status --porcelain --untracked-files=no)" ] || rev="$rev-dirty"

name="aios-$rev"
mkdir -p "$out"
objcopy --only-keep-debug "$bin" "$out/$name.sym"
strip --strip-all -o "$out/$name" "$bin"
(cd "$out" && objcopy --add-gnu-debuglink="$name.sym" "$name")
chmod 755 "$out/$name"

id_of() { readelf -n "$1" | awk '/Build ID/ {print $3}'; }
[ "$(id_of "$bin")" = "$(id_of "$out/$name")" ] || { echo "ship-binary: build id changed" >&2; exit 1; }
printf '%s  %s MB (was %s MB)  md5 %s\n' "$out/$name" "$(du -m "$out/$name" | cut -f1)" "$(du -m "$bin" | cut -f1)" \
  "$(md5sum < "$out/$name" | cut -c1-32)"
printf '%s  symbols, build id %s\n' "$out/$name.sym" "$(id_of "$bin")"
