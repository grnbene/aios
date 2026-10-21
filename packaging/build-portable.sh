#!/usr/bin/env bash
# Build the portable ASQL binary and pack a release tarball.
#
# The binary MUST be built on the oldest glibc in the fleet, because glibc is
# only backward compatible: sn is CentOS 7 / glibc 2.17, so anything built on
# AlmaLinux 9 (2.34) or arch (2.39) simply will not start there. Building here
# with devtoolset-11 plus -static-libstdc++/-static-libgcc yields one artifact
# that runs on every host in the set.
#
# Fully static is deliberately NOT used: src/compute/opencl_device.cpp:341
# documents that a statically linked build cannot safely dlopen.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
SRC="$(cd "$HERE/.." && pwd -P)"

BUILDER=${BUILDER:-}   # the el7 ssh host to build on; no default, a wrong guess could land on production
REMOTE_DIR=${REMOTE_DIR:-/tmp/asql-portable-build}
DEVTOOLSET=${DEVTOOLSET:-/opt/rh/devtoolset-11/enable}
JOBS=${JOBS:-}
OUT_DIR=${OUT_DIR:-$HERE/dist}
CMAKE_MIN=${CMAKE_MIN:-3.20}
CMAKE_BOOT=${CMAKE_BOOT:-3.28.6}
OPENCL_HEADERS=${OPENCL_HEADERS:-v2023.12.14}

VERSION=${VERSION:-$(git -C "$SRC" describe --always --dirty 2>/dev/null || date +%Y%m%d)}
STAMP="aios-asql-${VERSION}-linux-x86_64-glibc2.17"

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
die() { printf 'build-portable: %s\n' "$*" >&2; exit 1; }

[ -n "$BUILDER" ] || die "set BUILDER=<el7 ssh host with devtoolset-11>, e.g. BUILDER=devops ./build-portable.sh"
say "builder=$BUILDER  version=$VERSION"

ssh -o BatchMode=yes -o ConnectTimeout=15 "$BUILDER" true \
  || die "cannot reach builder '$BUILDER' over ssh"

ssh -o BatchMode=yes "$BUILDER" "[ -r '$DEVTOOLSET' ]" \
  || die "builder '$BUILDER' has no devtoolset at $DEVTOOLSET (needs GCC>=10 for C++20)"

say "shipping source to $BUILDER:$REMOTE_DIR"
ssh -o BatchMode=yes "$BUILDER" "rm -rf '$REMOTE_DIR' && mkdir -p '$REMOTE_DIR'"
COPYFILE_DISABLE=1 tar -C "$SRC" -cz --no-mac-metadata --no-xattrs \
  --exclude='build' --exclude='build-*' --exclude='.git' --exclude='packaging/dist' \
  --exclude='*.ppm' --exclude='screenshots' --exclude='Testing' \
  . | ssh -o BatchMode=yes "$BUILDER" "tar -C '$REMOTE_DIR' -xz"

say "compiling (devtoolset-11, static libstdc++/libgcc)"
ssh -o BatchMode=yes "$BUILDER" "bash -eu -o pipefail -s" <<REMOTE
source '$DEVTOOLSET'
cd '$REMOTE_DIR'

# el7 ships cmake 3.16/3.17 but the project needs 3.20. Rather than touch the
# builder's packages, drop an official static cmake into /tmp and use that.
need_cmake() {
  v=\$("\$1" --version 2>/dev/null | awk 'NR==1{print \$3}') || return 1
  [ -n "\$v" ] || return 1
  [ "\$(printf '%s\n%s\n' "$CMAKE_MIN" "\$v" | sort -V | head -1)" = "$CMAKE_MIN" ]
}
CM=""
for c in cmake3 cmake /usr/local/bin/cmake; do
  p=\$(command -v "\$c" 2>/dev/null) || continue
  if need_cmake "\$p"; then CM="\$p"; break; fi
done
if [ -z "\$CM" ]; then
  echo "  no cmake >= $CMAKE_MIN on builder; fetching $CMAKE_BOOT" >&2
  BOOT=/tmp/cmake-boot-$CMAKE_BOOT
  if [ ! -x "\$BOOT/bin/cmake" ]; then
    rm -rf "\$BOOT" && mkdir -p "\$BOOT"
    curl -sSLf --max-time 180 -o /tmp/cmake-boot.tgz \
      "https://github.com/Kitware/CMake/releases/download/v$CMAKE_BOOT/cmake-$CMAKE_BOOT-linux-x86_64.tar.gz"
    tar -C "\$BOOT" --strip-components=1 -xzf /tmp/cmake-boot.tgz
  fi
  CM="\$BOOT/bin/cmake"
  need_cmake "\$CM" || { echo "bootstrapped cmake still too old" >&2; exit 1; }
fi
echo "  using cmake: \$("\$CM" --version | awk 'NR==1')" >&2

# src/compute/opencl_device.cpp includes <CL/cl.h> unconditionally. The OpenCL
# library itself is dlopen'd at runtime, so these headers are a compile-time
# need only and vendoring them adds no runtime dependency.
CLINC=""
if [ ! -e /usr/include/CL/cl.h ]; then
  CLH=/tmp/opencl-headers-$OPENCL_HEADERS
  if [ ! -e "\$CLH/CL/cl.h" ]; then
    echo "  vendoring OpenCL headers $OPENCL_HEADERS" >&2
    rm -rf "\$CLH" && mkdir -p "\$CLH"
    curl -sSLf --max-time 120 -o /tmp/opencl-headers.tgz \
      "https://github.com/KhronosGroup/OpenCL-Headers/archive/refs/tags/$OPENCL_HEADERS.tar.gz"
    tar -C "\$CLH" --strip-components=1 -xzf /tmp/opencl-headers.tgz
  fi
  [ -e "\$CLH/CL/cl.h" ] || { echo 'could not obtain CL/cl.h' >&2; exit 1; }
  CLINC=" -I\$CLH"
fi

"\$CM" -S . -B build-portable \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS="-O2\$CLINC" \
  -DCMAKE_EXE_LINKER_FLAGS='-static-libstdc++ -static-libgcc' >/dev/null
"\$CM" --build build-portable --target aios-cli aios-asql-top aios-asql-console -j${JOBS:-\$(nproc)}
REMOTE

say "verifying the artifact on the builder"
FLOOR=$(ssh -o BatchMode=yes "$BUILDER" "
  cd '$REMOTE_DIR/build-portable'
  ./aios --help >/dev/null 2>&1 || { echo 'SMOKE-FAILED'; exit 1; }
  objdump -T ./aios 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sort -Vu | tail -1
")
[ "$FLOOR" = "SMOKE-FAILED" ] && die "built binary failed its --help smoke test on the builder"
say "binary requires at most $FLOOR"

case "$FLOOR" in
  GLIBC_2.1[0-7]|GLIBC_2.[0-9]|GLIBC_2.[0-9].*) : ;;
  *) die "binary needs $FLOOR — above the 2.17 floor, it will NOT run on sn" ;;
esac

say "packing $STAMP.tar.gz"
mkdir -p "$OUT_DIR"
PKG=$(mktemp -d)
trap 'rm -rf "$PKG"' EXIT
mkdir -p "$PKG/$STAMP/bin" "$PKG/$STAMP/systemd" "$PKG/$STAMP/conf"

scp -q -o BatchMode=yes "$BUILDER:$REMOTE_DIR/build-portable/aios" "$PKG/$STAMP/bin/aios"
if ssh -o BatchMode=yes "$BUILDER" "[ -f '$REMOTE_DIR/build-portable/aios-asql-top' ]"; then
  scp -q -o BatchMode=yes "$BUILDER:$REMOTE_DIR/build-portable/aios-asql-top" "$PKG/$STAMP/bin/aios-asql-top"
fi
if ssh -o BatchMode=yes "$BUILDER" "[ -f '$REMOTE_DIR/build-portable/aios-asql-console' ]"; then
  scp -q -o BatchMode=yes "$BUILDER:$REMOTE_DIR/build-portable/aios-asql-console" "$PKG/$STAMP/bin/aios-asql-console"
fi
cp "$HERE/asql-serve"               "$PKG/$STAMP/bin/asql-serve"
cp "$HERE/asql-export.sh"           "$PKG/$STAMP/bin/asql-export.sh"
cp "$HERE/asql-import.sh"           "$PKG/$STAMP/bin/asql-import.sh"
cp "$HERE/aios-compute.service.in"  "$PKG/$STAMP/systemd/"
cp "$HERE/asql.conf.example"        "$PKG/$STAMP/conf/"
cp "$HERE/install-asql.sh"          "$PKG/$STAMP/"
chmod +x "$PKG/$STAMP/bin/"* "$PKG/$STAMP/install-asql.sh"

cat > "$PKG/$STAMP/MANIFEST" <<MANIFEST
version=$VERSION
built=$(date -u +%Y-%m-%dT%H:%M:%SZ)
builder=$BUILDER
glibc_floor=$FLOOR
sha256=$(shasum -a 256 "$PKG/$STAMP/bin/aios" | cut -d' ' -f1)
MANIFEST

tar -C "$PKG" -czf "$OUT_DIR/$STAMP.tar.gz" "$STAMP"
shasum -a 256 "$OUT_DIR/$STAMP.tar.gz" > "$OUT_DIR/$STAMP.tar.gz.sha256"

say "done: $OUT_DIR/$STAMP.tar.gz"
cat "$PKG/$STAMP/MANIFEST"
