#!/bin/bash
# Every check of the WebAssembly build, in order: the VM, the pane protocol, and a pane in a real Chrome.
#
#   emcmake cmake -S grain -B build-grain-wasm -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE="-Oz -DNDEBUG"
#   cmake --build build-grain-wasm --parallel
#   grain/wasm/check.sh build-grain-wasm [chrome]
set -u
build=${1:?usage: check.sh <build dir> [chrome]}
chrome=${2:-"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"}
here=$(cd "$(dirname "$0")" && pwd)
status=0
step() { echo "== $1"; shift; "$@" | tail -1 || status=1; [ "${PIPESTATUS[0]}" -eq 0 ] || status=1; }
step "the VM"          node "$here/test.mjs" "$build/grain_wasm.js"
step "the pane"        node "$here/pane-test.mjs" "$build/grain_wasm.js" "$here/../tests/web"
if [ -x "$chrome" ] || command -v "$chrome" >/dev/null 2>&1; then
  echo "== a pane in Chrome"; python3 "$here/../tests/web/wasm_pane_test.py" "$build" "$chrome" | grep -c '^ok' | sed 's/$/ checks ok/'
  [ "${PIPESTATUS[0]}" -eq 0 ] || status=1
else
  echo "== a pane in Chrome: skipped (no Chrome at $chrome)"
fi
exit $status
