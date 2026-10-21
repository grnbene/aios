#!/usr/bin/env bash
# Derive /etc/aios/asql.conf from a host's LIVE aios-compute unit.
#
# Use this before installing onto a host that already runs ASQL from a
# hand-written unit with its arguments inline. A key the unit does not set is
# emitted empty, so the engine keeps its own default instead of being pinned to
# a value the host was never running.
set -euo pipefail

TARGET_HOST=${1:-}
if [ -z "$TARGET_HOST" ]; then
  echo "Usage: $0 <ssh-alias> [output-file]" >&2
  echo "  writes the derived conf to stdout, or to <output-file>" >&2
  exit 2
fi
OUT=${2:-}

RAW=$(ssh -o BatchMode=yes -o ConnectTimeout=15 "$TARGET_HOST" \
      'systemctl show -p ExecStart --value aios-compute')
[ -n "$RAW" ] || { echo "no ExecStart for aios-compute on $TARGET_HOST" >&2; exit 1; }

# systemd's ExecStart dump mixes the real arguments with its own metadata
# fields (path=, pid=, code=, status=...). Take only what the engine parses.
get() {
  printf '%s' "$RAW" | tr ' ' '\n' | awk -F= -v k="$1" '$1==k {sub(/^[^=]*=/,""); print; exit}'
}

ROOT=$(printf '%s' "$RAW" | tr ' ' '\n' | awk '/^--root$/{getline; print; exit}')
: "${ROOT:=/opt/aios/root}"
BIN=$(printf '%s' "$RAW" | tr ' ' '\n' | awk -F= '$1=="path"{print $2; exit}')
: "${BIN:=/opt/aios/bin/aios}"

emit() {
  cat <<CONF
# /etc/aios/asql.conf — derived from the live aios-compute unit on $TARGET_HOST
# on $(date -u +%Y-%m-%dT%H:%M:%SZ). Empty values are keys that unit did not
# set; leave them empty to keep the engine's own defaults.

AIOS_BIN=$BIN
AIOS_ROOT=$ROOT

RPC_PORT=$(get port)
LANES=$(get lanes)
SQL=$(get sql)
SQL_DB=$(get sql_db)
PUBLIC=$(get public)
MAX_POINTS=$(get max_points)

MYSQL_PORT=$(get mysql_port)
MYSQL_BIND=$(get mysql_bind)
MYSQL_USER=$(get mysql_user)
MYSQL_PASS_FILE=$(get mysql_pass_file)
TOKEN_FILE=$(get token_file)

CDC_HOST=$(get cdc_host)
CDC_PORT=$(get cdc_port)
CDC_USER=$(get cdc_user)
CDC_PASS_FILE=$(get cdc_pass_file)
CDC_DB=$(get cdc_db)
CDC_LOG_FILE=$(get cdc_log_file)
CDC_LOG_POS=$(get cdc_log_pos)

CDC_TABLES=$(get cdc_tables)
CONF
}

if [ -n "$OUT" ]; then
  emit > "$OUT"
  echo "wrote $OUT" >&2
else
  emit
fi
