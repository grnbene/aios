#!/usr/bin/env sh
# Drive the whole system from a clean root: kernel script, then a restart in a
# new process restoring the checkpoint. Exit code is the verdict.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
build=$(cd "${1:-$here/../build}" && pwd)
root=${AIOS_ROOT:-$build/drive-root}
rm -rf "$root"
cd "$build"
./aios --root "$root" run "$here/drive-kernel.aos"
ckpt=$(ls -t "$root"/checkpoints/*/*.aoc | head -1)
echo
echo "=== restart: new process, same root, restoring $ckpt"
./aios --root "$root" exec "restore $ckpt" "entity list" "state list" "info" "git repos" "git log" "git status"

# The subsystem must survive its own database being damaged: corrupt a loose
# object under the machine's repository and make it say so, in one process.
echo
echo "=== a corrupt loose object is reported, not fatal"
obj=$(find "$root"/checkpoints/.git/objects -type f | head -1)
printf 'this is not a zlib stream' > "$obj"
out=$(./aios --root "$root" exec "git fsck" 2>&1 || true)
echo "$out"
echo "$out" | grep -q "corrupt object" || { echo "FAIL: fsck did not report the damaged object"; exit 1; }
