#!/usr/bin/env bash
# Build the portable Linux `grain` and run its tests, inside the manylinux2014
# image (CentOS 7, glibc 2.17, GCC 10), on a Linux machine that has Docker.
#
#   grain/scripts/build-linux.sh                 # x86_64, into grain/dist/
#   ARCH=aarch64 grain/scripts/build-linux.sh    # 64-bit Arm (needs an arm64 host or binfmt/qemu)
#
# glibc is only backward compatible, so the binary is built against the oldest
# one in use (2.17) with libstdc++ and libgcc linked in: one file that starts on
# every distribution since CentOS 7. Run it ON the build machine (devops), from
# a checkout there - this script never reaches out to another host.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
SRC="$(cd "$HERE/../.." && pwd -P)"          # the aios checkout: grain builds from its sources
ARCH=${ARCH:-x86_64}
IMAGE=${IMAGE:-quay.io/pypa/manylinux2014_${ARCH}}
OUT=${OUT:-$SRC/grain/dist}
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 4)}

die() { printf 'build-linux: %s\n' "$*" >&2; exit 1; }
command -v docker >/dev/null || die "docker is not installed on this machine"
[ "$(uname -s)" = Linux ] || die "run this on a Linux machine (it uses Linux containers)"

VERSION=$(sed -n 's/^project(grain VERSION \([0-9.]*\).*/\1/p' "$SRC/grain/CMakeLists.txt")
[ -n "$VERSION" ] || die "cannot read the version from grain/CMakeLists.txt"
mkdir -p "$OUT"

# The build tree lives in the container only; the source is mounted read-only.
docker run --rm -v "$SRC:/src:ro" -v "$OUT:/out" -e JOBS="$JOBS" -e VERSION="$VERSION" -e ARCH="$ARCH" "$IMAGE" bash -euo pipefail -c '
  gcc_major=$(gcc -dumpversion | cut -d. -f1)
  [ "$gcc_major" -ge 10 ] || { echo "build-linux: the image has GCC $gcc_major; grain needs 10 or newer" >&2; exit 1; }
  command -v cmake >/dev/null || { echo "build-linux: the image has no cmake" >&2; exit 1; }
  cmake -S /src/grain -B /tmp/b -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc"
  cmake --build /tmp/b --parallel "$JOBS"
  ctest --test-dir /tmp/b --output-on-failure -j "$JOBS"
  strip /tmp/b/grain
  cp /tmp/b/grain "/out/grain-$VERSION-linux-$ARCH"
'
echo "built $OUT/grain-$VERSION-linux-$ARCH"
