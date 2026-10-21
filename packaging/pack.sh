#!/usr/bin/env bash
# Pack built binaries into the install tarball that install-asql.sh consumes.
#
#   packaging/pack.sh BIN_DIR [OUT_DIR] [VERSION]
#
# BIN_DIR holds `aios` (required) and, when built, `aios-asql-top` and
# `aios-asql-console` - the output of build-centos7.sh or build-portable.sh.
# The tarball carries the scripts, the unit template, the example conf and a
# MANIFEST with the binary's sha256 and glibc floor; the installer checks
# both before it touches /opt/aios.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
SRC="$(cd "$HERE/.." && pwd -P)"
BIN_DIR=${1:?usage: pack.sh BIN_DIR [OUT_DIR] [VERSION]}
OUT_DIR=${2:-$HERE/dist}
VERSION=${3:-$(git -C "$SRC" describe --always --dirty 2>/dev/null || date +%Y%m%d)}

[ -f "$BIN_DIR/aios" ] || { echo "pack.sh: no aios binary in $BIN_DIR" >&2; exit 1; }

FLOOR=$(objdump -T "$BIN_DIR/aios" 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sort -Vu | tail -1)
: "${FLOOR:=GLIBC_2.17}"
ARCH=$(uname -m)
STAMP="aios-asql-${VERSION}-linux-${ARCH}-glibc${FLOOR#GLIBC_}"

PKG=$(mktemp -d); trap 'rm -rf "$PKG"' EXIT
mkdir -p "$PKG/$STAMP/bin" "$PKG/$STAMP/systemd" "$PKG/$STAMP/conf"
cp "$BIN_DIR/aios" "$PKG/$STAMP/bin/aios"
for t in aios-asql-top aios-asql-console aios-webserver; do
  [ -f "$BIN_DIR/$t" ] && cp "$BIN_DIR/$t" "$PKG/$STAMP/bin/$t"
done
cp "$HERE/asql-serve" "$HERE/asql-export.sh" "$HERE/asql-import.sh" "$HERE/asql-check.sh" "$PKG/$STAMP/bin/"
cp "$HERE/aios-compute.service.in" "$HERE/aios-webserver.service.in" "$PKG/$STAMP/systemd/"
cp "$HERE/asql.conf.example" "$HERE/webserver.conf.example" "$PKG/$STAMP/conf/"
# The Things declared for the ERP: installed beside the engine, declared by the operator.
mkdir -p "$PKG/$STAMP/things"; cp "$SRC/things/"*.sql "$SRC/things/README.md" "$PKG/$STAMP/things/" 2>/dev/null || true
cp "$HERE/install-asql.sh"         "$PKG/$STAMP/"
cp "$SRC/README-ASQL.md"           "$PKG/$STAMP/README.md"
chmod +x "$PKG/$STAMP/bin/"* "$PKG/$STAMP/install-asql.sh"

cat > "$PKG/$STAMP/MANIFEST" <<MANIFEST
version=$VERSION
built=$(date -u +%Y-%m-%dT%H:%M:%SZ)
builder=$(hostname -s)
glibc_floor=$FLOOR
sha256=$(sha256sum "$PKG/$STAMP/bin/aios" | cut -d' ' -f1)
webserver=$([ -f "$PKG/$STAMP/bin/aios-webserver" ] && echo yes || echo no)
MANIFEST

mkdir -p "$OUT_DIR"
tar -C "$PKG" -czf "$OUT_DIR/$STAMP.tar.gz" "$STAMP"
( cd "$OUT_DIR" && sha256sum "$STAMP.tar.gz" > "$STAMP.tar.gz.sha256" )
echo "packed $OUT_DIR/$STAMP.tar.gz ($(du -h "$OUT_DIR/$STAMP.tar.gz" | cut -f1))"
cat "$PKG/$STAMP/MANIFEST"
