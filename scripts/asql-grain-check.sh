#!/usr/bin/env bash
# grain against a fresh ASQL engine: live() following writes (ASQL_CHANGES), two random-write fuzzers that check
# ORDER BY / MIN / MAX / GROUP BY / DISTINCT / lookups against the rows themselves, and - when a MariaDB is given -
# the same SQL on both, answer for answer, then random SQL (sql_fuzz.gr) on both.
#   asql-grain-check.sh <aios-engine> <grain> [mariadb-port mariadb-password]   (MariaDB account: g)
set -u
ENGINE=${1:?usage: asql-grain-check.sh <aios-engine> <grain> [mariadb-port mariadb-password]}
GRAIN=${2:?}
HERE=$(cd "$(dirname "$0")" && pwd)
T="$HERE/../grain/tests/db"
WORK=$(mktemp -d)
PORT=$((20000 + RANDOM % 20000))
printf 'checkpass\n' > "$WORK/pass"; chmod 600 "$WORK/pass"
AIOS_NO_ACCEL=1 "$ENGINE" --root="$WORK/root" --port=$PORT --bind=127.0.0.1 --sql-user=g --sql-pass-file="$WORK/pass" > "$WORK/engine.log" 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null; wait $PID 2>/dev/null; rm -rf "$WORK"' EXIT
for _ in $(seq 1 80); do (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && break; sleep 0.25; done
FAILED=0
run() { echo "== $*"; "$@" || FAILED=1; }
run "$GRAIN" "$T/live_asql.gr" $PORT checkpass
for seed in 7 23 99; do run "$GRAIN" "$T/order_fuzz.gr" $PORT checkpass 300 $seed; run "$GRAIN" "$T/query_fuzz.gr" $PORT checkpass 300 $seed; done
if [ $# -ge 4 ]; then
  run "$GRAIN" "$T/differential.gr" $PORT checkpass "$3" "$4" "$T"/sql/*.sql
  for seed in 1 2 3 4 5; do run "$GRAIN" "$T/sql_fuzz.gr" $PORT checkpass "$3" "$4" 300 $seed; done
fi
exit $FAILED
