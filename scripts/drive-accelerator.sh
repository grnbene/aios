#!/usr/bin/env sh
# Drive the accelerator path on whatever accelerator this host has — a GPU, an
# NPU, or an OpenCL CPU runtime. The device is pinned by name prefix, which
# differs per host, so it is substituted into the script here. A host with no
# accelerator has nothing to drive: report a skip, not a failure.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
build=${1:-$here/../build}
root=${2:-$build/drive-accel}
rm -rf "$root"
devices=$("$build/aios" --root "$root" exec devices 2>/dev/null || true)
accel=$(printf '%s\n' "$devices" | grep -E "ocl-cpu|gpu:|npu:" | head -1 | awk '{print $2}')
rm -rf "$root"
if [ -z "$accel" ]; then
  echo "SKIP: no accelerator on this host (no OpenCL runtime), so there is nothing to drive"
  echo "      the same code path runs on a GPU or an OpenCL CPU runtime where one exists"
  exit 0
fi
echo "accelerator: $accel"
sed "s|dev=ocl-cpu|dev=$accel|g" "$here/drive-accelerator.aos" > "$build/drive-accelerator.resolved.aos"
exec "$build/aios" --root "$root" run "$build/drive-accelerator.resolved.aos"
