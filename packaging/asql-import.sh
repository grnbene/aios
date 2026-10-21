#!/usr/bin/env bash
# asql-import.sh - load a file made by asql-export.sh into this host's ASQL.
#
#   asql-import.sh ARCHIVE [--conf /etc/aios/asql.conf] [--unit aios-compute]
#                          [--force] [--keep-replication-state]
#                          [--no-service]   the engine is not run by that systemd
#                                           unit: you have stopped it, you start it
#
# Checks the archive against ARCHIVE.sha256, stops the service, puts the data
# in place, starts the service and waits until it answers. An existing store
# is never deleted: --force moves it to <store>.replaced-<time>.
#
# The source server's replication position (cdc_position, seed_positions) is
# dropped unless --keep-replication-state: it names a binlog on the SOURCE's
# MariaDB, and this host's engine would look for it on its own.
set -euo pipefail

CONF=/etc/aios/asql.conf UNIT=aios-compute
FORCE=0 KEEP_REPL=0 START=1 NOSVC=0 ARCHIVE=
while [ $# -gt 0 ]; do
  case "$1" in
    --conf) CONF=$2; shift 2 ;;
    --unit) UNIT=$2; shift 2 ;;
    --force) FORCE=1; shift ;;
    --keep-replication-state) KEEP_REPL=1; shift ;;
    --no-start) START=0; shift ;;
    --no-service) NOSVC=1; shift ;;
    -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
    -*) echo "unknown argument: $1" >&2; exit 2 ;;
    *) ARCHIVE=$1; shift ;;
  esac
done
[ -f "$ARCHIVE" ] || { echo "usage: asql-import.sh ARCHIVE [options]" >&2; exit 2; }
[ -r "$CONF" ] || { echo "cannot read $CONF (install ASQL first: install-asql.sh)" >&2; exit 1; }
# shellcheck disable=SC1090
. "$CONF"

ROOT=${AIOS_ROOT:-/opt/aios/root}
STORE=$ROOT/${SQL_DB:-sql_db}
[ -d "$ROOT" ] || { echo "$ROOT does not exist" >&2; exit 1; }
stamp=$(date +%Y%m%d-%H%M%S)
STAGE=$STORE.import-$stamp

if [ -e "$STORE/tables/MANIFEST" ] && [ "$FORCE" = 0 ]; then
  echo "$STORE already holds data; --force keeps it aside as $STORE.replaced-<time> and replaces it" >&2
  exit 1
fi

if [ -f "$ARCHIVE.sha256" ]; then
  echo "== verify"
  ( cd "$(dirname "$ARCHIVE")" && sha256sum -c "$(basename "$ARCHIVE").sha256" )
else
  echo "== no $ARCHIVE.sha256 next to it: integrity not checked"
fi

echo "== unpack"
rm -rf -- "$STAGE"; mkdir -p "$STAGE"
trap 'rm -rf -- "$STAGE"' EXIT
tar -C "$STAGE" -xaf "$ARCHIVE"
[ -s "$STAGE/tables/MANIFEST" ] || { echo "not an ASQL export: no tables/MANIFEST inside" >&2; exit 1; }
if [ "$KEEP_REPL" = 0 ]; then rm -f "$STAGE/cdc_position" "$STAGE/seed_positions"; fi
echo "   $(ls "$STAGE/tables" | grep -c '\.tbl$') table files, $(du -sh "$STAGE" | cut -f1)"

have_unit=0
if [ "$NOSVC" = 0 ]; then
  if command -v systemctl >/dev/null && systemctl cat "$UNIT" >/dev/null 2>&1; then have_unit=1
  else echo "no systemd unit '$UNIT' (--unit NAME, or --no-service if you run the engine yourself and have stopped it)" >&2; exit 1; fi
fi
unit_prop() { systemctl show -p "$1" "$UNIT" 2>/dev/null | cut -d= -f2-; }

echo "== stop"
if [ "$have_unit" = 1 ]; then systemctl stop "$UNIT"; else echo "   --no-service: not touching the engine"; fi

echo "== place"
if [ -e "$STORE" ]; then
  mv "$STORE" "$STORE.replaced-$stamp"
  # connections.ini belongs to this host, not to the data
  [ -f "$STORE.replaced-$stamp/connections.ini" ] && cp -p "$STORE.replaced-$stamp/connections.ini" "$STAGE/"
  echo "   previous store kept: $STORE.replaced-$stamp"
fi
owner=$(stat -c %U:%G "$ROOT")
chown -R "$owner" "$STAGE"
mv "$STAGE" "$STORE"
trap - EXIT

[ "$START" = 1 ] || { echo "== not starting (--no-start). Data is at $STORE"; exit 0; }

echo "== start"
if [ "$have_unit" = 1 ]; then
  before=$(unit_prop ExecMainStartTimestamp)
  systemctl start "$UNIT"
  after=$(unit_prop ExecMainStartTimestamp)
  [ "$before" != "$after" ] || { echo "the service did not restart" >&2; exit 1; }
  if command -v mysql >/dev/null; then
    if [ -n "${MYSQL_SOCKET:-}" ]; then conn=(-S "$MYSQL_SOCKET"); else conn=(-h "${MYSQL_BIND:-127.0.0.1}" -P "${MYSQL_PORT:-3307}"); fi
    if [ -n "${MYSQL_PASS_FILE:-}" ] && [ -r "$MYSQL_PASS_FILE" ]; then MYSQL_PWD=$(head -n1 "$MYSQL_PASS_FILE"); export MYSQL_PWD; fi
    for i in $(seq 1 120); do
      n=$(mysql "${conn[@]}" -u"${MYSQL_USER:-erp}" --batch --skip-column-names -e \
          "SELECT COUNT(*) FROM information_schema.tables" 2>/dev/null) && break
      sleep 2
    done
    echo "   answering; ${n:-?} tables visible"
  fi
else
  echo "   start the engine on $ROOT yourself"
fi
