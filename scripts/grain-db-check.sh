#!/usr/bin/env bash
# Runs the grain scripts that need a database (tests/grain/test_db_transaction.gr) against a throwaway aios SQL
# server: its own root, a random password (mode 600), a free loopback port; all removed afterwards.
# The scripts run on the standalone grain (the database client an app gets); aios only serves the database.
#   scripts/grain-db-check.sh [path/to/aios] [path/to/grain]
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
aios="${1:-$here/build-static/aios}"
grain="${2:-$here/build-grain/grain}"
work="$(mktemp -d)"
server_pid=""
cleanup() { [ -n "$server_pid" ] && kill "$server_pid" 2>/dev/null && wait "$server_pid" 2>/dev/null; rm -rf "$work"; }
trap cleanup EXIT
umask 077
head -c 18 /dev/urandom | base64 > "$work/pass"
port=$(( 20000 + RANDOM % 20000 ))
AIOS_NO_ACCEL=1 "$aios" --root "$work/root" exec "compute serve sql=1 mysql_port=$port mysql_user=root mysql_pass_file=$work/pass" > "$work/server.log" 2>&1 &
server_pid=$!
for _ in $(seq 1 60); do
  (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null && break
  kill -0 "$server_pid" 2>/dev/null || { echo "the SQL server did not start:"; cat "$work/server.log"; exit 1; }
  sleep 0.5
done
export GRAIN_TEST_DB_PORT="$port" GRAIN_TEST_DB_PASS_FILE="$work/pass"
rc=0
for t in "$here"/tests/grain/test_db_transaction.gr; do
  echo "== $(basename "$t")"
  "$grain" "$t" || rc=1
done
exit $rc
