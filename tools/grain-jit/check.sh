#!/bin/bash
# grain's AArch64 native code vs its interpreter, on this (x86) machine.
#
# Builds grain for aarch64 Linux and runs it under qemu-aarch64-static: every
# example and every program in cases/ runs twice - native code, then
# GRAIN_NOJIT=1 interpreted - and the outputs (errors and stack traces
# included) must match line for line, and both must tear down to zero live
# heap allocations (a refcount slip in generated code shows up as a leak or
# a crash). Timings are dropped from the comparison.
#
#   tools/grain-jit/check.sh            build + compare
#   tools/grain-jit/check.sh --no-build compare with the last build
# Needs: aarch64-linux-gnu-g++, qemu-user-static.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
S="$(cd "$here/../.." && pwd)"
out="${GRAIN_JIT_OUT:-/tmp/grain-jit-check}"
mkdir -p "$out"
CXX=aarch64-linux-gnu-g++
FLAGS=(-std=c++20 -O2 -I"$S/include" -I"$S/src/userland" -I"$S/src")
if [ "${1:-}" != "--no-build" ]; then
  # The portable standard library only (install_core_builtins): nothing here
  # reaches the sidecar, SQL or the MariaDB client, so everything links.
  srcs=(src/userland/grain_lexer src/userland/grain_parser src/userland/grain_compiler src/userland/grain_vm
        src/userland/grain_jit_arm64 src/userland/grain_builtins src/userland/grain_std_hash
        src/userland/font_atlas src/userland/glyph_engine src/hal/posix_sync
        src/crypto/sha256 src/crypto/sha1 src/crypto/sha512 src/crypto/md5 src/crypto/keccak256 src/crypto/hmac_sha256)
  for f in "${srcs[@]}"; do $CXX "${FLAGS[@]}" -c "$S/$f.cpp" -o "$out/$(basename $f).o" & done
  $CXX "${FLAGS[@]}" -c "$here/runner.cpp" -o "$out/runner.o" &
  wait
  rm -f "$out/stubs.o" "$out/mysql_wire.o" "$out/linux_net.o"   # objects an older check left behind
  $CXX -static -o "$out/gr" "$out"/*.o -lpthread || { echo "link failed"; exit 1; }
fi
cd "$out"
bad=0
for f in "$S"/examples/grain/{tour,closures,words,fib,selftest,bench}.gr "$here"/cases/*.gr; do
  n=$(basename "$f" .gr)
  GRAIN_LEAKS=1 qemu-aarch64-static ./gr "$f" > "native.$n" 2> "native.$n.err"; echo "exit $?" >> "native.$n"
  GRAIN_LEAKS=1 GRAIN_NOJIT=1 qemu-aarch64-static ./gr "$f" > "interp.$n" 2> "interp.$n.err"; echo "exit $?" >> "interp.$n"
  d=$(diff <(grep -av " ms" "interp.$n") <(grep -av " ms" "native.$n") | grep -c '^[<>]')
  ln=$(grep -a live "native.$n.err" | sed 's/.*: //'); li=$(grep -a live "interp.$n.err" | sed 's/.*: //')
  status=ok; { [ "$d" != 0 ] || [ "$ln" != 0 ] || [ "$li" != 0 ]; } && { status=FAIL; bad=1; }
  printf "  %-5s %-10s %3s differing lines, live after teardown: native %s interp %s\n" "$status" "$n" "$d" "$ln" "$li"
done
exit $bad
