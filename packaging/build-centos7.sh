#!/usr/bin/env bash
# Build aios binaries for CentOS 7 (glibc 2.17) in a container - no build box
# of the customer's, and no production box of ours, needed.
#
#   packaging/build-centos7.sh [git-ref=origin/main] [out_dir=build-centos7]
#   TARGETS="aios-cli aios-asql-top aios-asql-console" packaging/build-centos7.sh
#
# manylinux2014 is CentOS 7.9 with devtoolset-10 (GCC 10.2): the binary it
# links against glibc 2.17, and -static-libstdc++ keeps CentOS 7's old
# libstdc++ out of it. OpenCL is loaded at run time (AIOS_NO_ACCEL=1 turns it
# off), but its headers are needed to compile, so the host's /usr/include/CL
# is mounted in.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
ref=${1:-origin/main}
out_dir=${2:-$here/build-centos7}
work=${AIOS_C7_WORK:-/tmp/aios-c7}
image=quay.io/pypa/manylinux2014_x86_64
docker_=(docker --context default)
TARGETS=${TARGETS:-"aios-cli aios-asql-top aios-asql-console aios-webserver"}

[ -f /usr/include/CL/cl.h ] || { echo "need OpenCL headers at /usr/include/CL (opencl-headers)" >&2; exit 1; }
rm -rf "$work/src" && mkdir -p "$work/src" "$work/inc"
git -C "$here/.." archive "$ref" aios | tar -x -C "$work/src"
cp -r /usr/include/CL "$work/inc/"
"${docker_[@]}" run --rm -u "$(id -u):$(id -g)" -v "$work:/w" -w /w "$image" bash -c "
  cmake -S src/aios -B build -DCMAKE_BUILD_TYPE=Release -DAIOS_ENABLE_SDL2=OFF \
    -DCMAKE_CXX_FLAGS='-I/w/inc' -DCMAKE_EXE_LINKER_FLAGS='-static-libstdc++ -static-libgcc' > cmake.log 2>&1 \
    || { tail -30 cmake.log; exit 1; }
  make -C build -j\"\$(nproc)\" $TARGETS > make.log 2>&1 || { grep -nE 'error:|undefined reference' make.log | head -30; exit 1; }
"

mkdir -p "$out_dir"
for target in $TARGETS; do
  # aios-cli's executable is named "aios" (CMakeLists' OUTPUT_NAME); every
  # other target here is named exactly like its target.
  bin_name=$target
  [ "$target" = "aios-cli" ] && bin_name=aios
  bin="$work/build/$bin_name"
  [ -f "$bin" ] || { echo "expected binary not found: $bin (target '$target' didn't link?)" >&2; exit 1; }
  glibc=$(objdump -T "$bin" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)
  if objdump -T "$bin" | grep -q GLIBCXX; then
    echo "$bin_name: libstdc++ is dynamically linked - would not run on CentOS 7" >&2
    exit 1
  fi
  cp "$bin" "$out_dir/$bin_name"
  echo "built $out_dir/$bin_name from $ref ($(git -C "$here/.." rev-parse --short "$ref")), needs up to $glibc (CentOS 7 has GLIBC_2.17)"
done
