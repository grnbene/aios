#!/usr/bin/env bash
# Pack macOS binaries into a release tarball (the Linux pack.sh's counterpart: no systemd unit, no installer).
#
#   packaging/pack-macos.sh BIN_DIR [OUT_DIR] [VERSION]
#
# BIN_DIR holds `aios` (required) and, when built, aios-webserver, aios-asql-console and aios-asql-top. Build them
# with -DCMAKE_OSX_DEPLOYMENT_TARGET=13.3 (the oldest macOS with every library call the code makes) and
# -DAIOS_ENABLE_SDL2=OFF, so the binaries need nothing outside macOS itself; this refuses one that links a library
# from anywhere else (Homebrew, say). A public build (-DAIOS_PUBLIC_BUILD=ON) is named -public, as pack.sh does.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
SRC="$(cd "$HERE/.." && pwd -P)"
BIN_DIR=${1:?usage: pack-macos.sh BIN_DIR [OUT_DIR] [VERSION]}
OUT_DIR=${2:-$HERE/dist}
VERSION=${3:-$(git -C "$SRC" describe --always --dirty 2>/dev/null || date +%Y%m%d)}

[ "$(uname -s)" = Darwin ] || { echo "pack-macos.sh: run it on a Mac" >&2; exit 1; }
[ -f "$BIN_DIR/aios" ] || { echo "pack-macos.sh: no aios binary in $BIN_DIR" >&2; exit 1; }

ARCH=$(lipo -archs "$BIN_DIR/aios" | tr ' ' '+')
MINOS=$(otool -l "$BIN_DIR/aios" | awk '/LC_BUILD_VERSION/{f=1} f&&/minos/{print $2; exit}')
LICENSE=none
grep -q "build: public (" "$BIN_DIR/aios" && LICENSE=trial
STAMP="aios-asql-${VERSION}$([ "$LICENSE" = trial ] && echo -public)-macos-${ARCH}"

PKG=$(mktemp -d); trap 'rm -rf "$PKG"' EXIT
mkdir -p "$PKG/$STAMP/bin"
for t in aios aios-webserver aios-asql-console aios-asql-top; do
  [ -f "$BIN_DIR/$t" ] || continue
  outside=$(otool -L "$BIN_DIR/$t" | tail -n +2 | awk '{print $1}' | grep -vE '^(/usr/lib/|/System/Library/)' || true)
  [ -z "$outside" ] || { echo "pack-macos.sh: $t links outside macOS: $outside" >&2; exit 1; }
  cp "$BIN_DIR/$t" "$PKG/$STAMP/bin/$t"
done
mkdir -p "$PKG/$STAMP/things"; cp "$SRC/things/"*.sql "$SRC/things/README.md" "$PKG/$STAMP/things/" 2>/dev/null || true
cp "$SRC/README-ASQL.md" "$PKG/$STAMP/README.md"
chmod +x "$PKG/$STAMP/bin/"*

cat > "$PKG/$STAMP/MANIFEST" <<MANIFEST
version=$VERSION
built=$(date -u +%Y-%m-%dT%H:%M:%SZ)
builder=$(hostname -s)
arch=$ARCH
macos_min=$MINOS
license=$LICENSE
sha256=$(shasum -a 256 "$PKG/$STAMP/bin/aios" | cut -d' ' -f1)
MANIFEST

mkdir -p "$OUT_DIR"
COPYFILE_DISABLE=1 tar -C "$PKG" --no-mac-metadata --no-xattrs -czf "$OUT_DIR/$STAMP.tar.gz" "$STAMP"
( cd "$OUT_DIR" && shasum -a 256 "$STAMP.tar.gz" > "$STAMP.tar.gz.sha256" )
echo "packed $OUT_DIR/$STAMP.tar.gz ($(du -h "$OUT_DIR/$STAMP.tar.gz" | cut -f1))"
cat "$PKG/$STAMP/MANIFEST"
