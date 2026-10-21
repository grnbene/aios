#!/usr/bin/env sh
# Two kernels, two processes, one machine: B serves, A attaches and uses B's
# compute, memory and channels. Exit code is the verdict.
set -eu
here=$(cd "$(dirname "$0")" && pwd); build=${1:-$here/../build}
rm -rf "$build/node-a" "$build/node-b"
"$build/aios" --root "$build/node-b" run "$here/drive-node-b.aos" > "$build/node-b.out" 2>&1 &
bpid=$!
sleep 1
"$build/aios" --root "$build/node-a" run "$here/drive-node-a.aos" 2>&1 | tee "$build/node-a.out" | tail -40
wait $bpid && echo "--- node B:" && sed 's/\x1b\[[0-9;]*m//g' "$build/node-b.out" | tail -14
