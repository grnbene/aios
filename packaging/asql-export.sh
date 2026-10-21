#!/usr/bin/env bash
# asql-export.sh - one compressed file holding this host's ASQL data, for
# moving to another server (asql-import.sh loads it there).
#
# Asks the running engine for a consistent snapshot (EXPORT SNAPSHOT: it
# checkpoints, then hard-links one generation of table files - instant, no
# extra disk, writers wait milliseconds), packs that, and removes the staging
# copy. Never reads the live files, so it cannot catch a save half way.
#
#   asql-export.sh [-o OUT.tar.zst|OUT.tar.gz|OUT.tar] [--conf /etc/aios/asql.conf]
set -euo pipefail

CONF=/etc/aios/asql.conf
OUT=
while [ $# -gt 0 ]; do
  case "$1" in
    -o|--out) OUT=$2; shift 2 ;;
    --conf)   CONF=$2; shift 2 ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

[ -r "$CONF" ] || { echo "cannot read $CONF" >&2; exit 1; }
# shellcheck disable=SC1090
. "$CONF"
command -v mysql >/dev/null || { echo "needs the mysql client (any MariaDB/MySQL client package)" >&2; exit 1; }

if [ -n "${MYSQL_SOCKET:-}" ] && [ -S "$MYSQL_SOCKET" ]; then
  conn=(-S "$MYSQL_SOCKET")
else
  conn=(-h "${MYSQL_BIND:-127.0.0.1}" -P "${MYSQL_PORT:-3307}")
fi
if [ -n "${MYSQL_PASS_FILE:-}" ] && [ -r "$MYSQL_PASS_FILE" ]; then
  MYSQL_PWD=$(head -n1 "$MYSQL_PASS_FILE"); export MYSQL_PWD
fi

stamp=$(date +%Y%m%d-%H%M%S)
name="export-$stamp"
# The compression follows the name asked for (-o x.tar.gz is gzip even where
# zstd is installed); with no -o, zstd when there is one, else gzip.
case "$OUT" in
  "")
    if command -v zstd >/dev/null; then ext=tar.zst; pack=(zstd -T0 -3 -q -c); else ext=tar.gz; pack=(gzip -c); fi
    OUT="$PWD/asql-$(hostname -s)-$stamp.$ext" ;;
  *.tar.zst|*.tzst)
    command -v zstd >/dev/null || { echo "$OUT asks for zstd, which is not installed (use -o NAME.tar.gz)" >&2; exit 1; }
    pack=(zstd -T0 -3 -q -c) ;;
  *.tar.gz|*.tgz) pack=(gzip -c) ;;
  *.tar) pack=(cat) ;;
  *) echo "-o $OUT: name it .tar.zst, .tar.gz (.tgz) or .tar - the extension picks the compression" >&2; exit 2 ;;
esac

echo "== snapshot"
# A snapshot is refused while a transaction is open on another session (its rows
# are not committed yet); those are short, so ask again.
res= err=
for try in $(seq 1 40); do
  if res=$(mysql "${conn[@]}" -u"${MYSQL_USER:-erp}" --batch --skip-column-names -e "EXPORT SNAPSHOT '$name'" 2>&1); then err=; break; fi
  err=$res
  case "$err" in *"transaction is open"*) sleep 0.5 ;; *) break ;; esac
done
[ -z "$err" ] || { echo "$err" >&2; echo "the engine refused (is it running, and is it a build with EXPORT SNAPSHOT?)" >&2; exit 1; }
staging=$(printf '%s\n' "$res" | awk -F'\t' '$1=="path"{print $2}')
gen=$(printf '%s\n' "$res" | awk -F'\t' '$1=="generation"{print $2}')
case "$staging" in
  */exports/"$name") ;;
  *) echo "unexpected staging path '$staging'" >&2; exit 1 ;;
esac
trap 'rm -rf -- "$staging"' EXIT
trap 'rm -f -- "$OUT" "$OUT.sha256"; exit 1' INT TERM
echo "   generation $gen, staged at $staging"

echo "== pack"
tar -C "$staging" -cf - . | "${pack[@]}" > "$OUT"
( cd "$(dirname "$OUT")" && sha256sum "$(basename "$OUT")" > "$(basename "$OUT").sha256" )
echo "   $(du -h "$OUT" | cut -f1)  $OUT"
echo "   $(cut -c1-16 "$OUT.sha256")...  $OUT.sha256"
echo
echo "copy both files, then on the other server:  asql-import.sh $(basename "$OUT")"
