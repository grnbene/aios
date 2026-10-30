#!/usr/bin/env bash
# asql-warm-check.sh [aios binary]: an engine restarted with primary-key row maps to warm
#   [1] answers, and takes a keyed write, without waiting for the warm-up to finish;
#   [2] warms the row maps only as far as the row-index budget (AIOS_SQL_ROWINDEX_MB), most recently used first.
#
# st's engine warmed 23 row maps (2 GB of a 1 GB budget) under one statement lock held for all of it: 34 s
# unanswering on every restart, and every map marked "in use" so none could be evicted.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
bin=$(realpath "${1:-$here/build/aios}")
work=$(mktemp -d /tmp/asql-warm-check.XXXXXX)
# Two ports the system has free (the wire's and the compute service's), not a random pair: runs side by side - two
# sessions, ctest -j - drew the same one now and then, and one engine answered the other's queries or never started.
read -r port cport < <(python3 -c 'import socket
a, b = socket.socket(), socket.socket()
a.bind(("127.0.0.1", 0)); b.bind(("127.0.0.1", 0))
print(a.getsockname()[1], b.getsockname()[1])')
pid=""
cleanup() { [ -n "$pid" ] && kill "$pid" 2>/dev/null; wait 2>/dev/null; [ "${fail:-0}" = 0 ] || tail -5 "$work/engine.log" 2>/dev/null; rm -rf "$work"; }
trap cleanup EXIT
umask 077
mkdir -p "$work/root"
head -c 12 /dev/urandom | od -An -tx1 | tr -d ' \n' > "$work/pw"
printf '[client]\nuser=erp\npassword=%s\nhost=127.0.0.1\nport=%s\n' "$(cat "$work/pw")" "$port" > "$work/my.cnf"
q() { mysql --defaults-extra-file="$work/my.cnf" -N -e "$1"; }
start() {   # start [env...]
  (cd "$work" && exec env AIOS_NO_ACCEL=1 AIOS_CHECKPOINT_SECS=3 "$@" "$bin" --root "$work/root" exec \
     "compute serve port=$cport lanes=4 sql=1 mysql_port=$port mysql_bind=127.0.0.1 mysql_user=erp mysql_pass_file=$work/pw" \
     > "$work/engine.log" 2>&1) &
  pid=$!
}
stop() { kill "$pid"; wait "$pid" 2>/dev/null || true; pid=""; }
fail=0
check() { if [ "$1" = ok ]; then echo "ok   $2"; else echo "FAIL $2"; fail=1; fi; }

# Four tables of 2M rows with text keys (as st's are; sequential numbers compress to a few runs), each given a row
# map by a keyed write, t4 last (most recently used).
start
for i in $(seq 1 150); do q "SELECT 1" >/dev/null 2>&1 && break; sleep 0.2; done
q "SELECT 1" >/dev/null || { fail=1; echo "FAIL the engine did not start"; exit 1; }
q "CREATE DATABASE w"
q "CREATE TABLE w.n (n INT PRIMARY KEY AUTO_INCREMENT, v INT)"
q "INSERT INTO w.n (v) VALUES (1)"
for k in $(seq 1 21); do q "INSERT INTO w.n (v) SELECT v FROM w.n"; done
q "CREATE TABLE w.small (id INT PRIMARY KEY AUTO_INCREMENT, v INT)"
q "INSERT INTO w.small (v) VALUES (1), (2)"
for t in t1 t2 t3 t4; do
  q "CREATE TABLE w.$t (id VARCHAR(24) PRIMARY KEY, v INT)"
  q "INSERT INTO w.$t (id, v) SELECT CONCAT('k', (n * 2654435761) % 4294967296), 1 FROM w.n"
done
key="'$(q "SELECT CONCAT('k', (7 * 2654435761) % 4294967296)")'"
for t in t1 t2 t3 t4; do q "UPDATE w.$t SET v = v + 1 WHERE id = $key"; sleep 1.1; done
one=$(q "SHOW GLOBAL STATUS" | awk '$1=="Asql_row_index_mem_bytes"{print $2}')
per=$((one / 4))
stop   # a clean stop writes the store, and the list of row maps with it
listed=$(tr '\n' ' ' < "$work/root/sql_db/rowindexes.list" 2>/dev/null || true)
check "$( [ "$(head -1 "$work/root/sql_db/rowindexes.list" 2>/dev/null)" = t4 ] && echo ok)" "rowindexes.list is most recently used first: $listed"

# Restarted with a budget that holds some of the maps, not all of them.
budget_mb=${WARM_BUDGET_MB:-100}
t0=$(date +%s%N)
start AIOS_SQL_ROWINDEX_MB=$budget_mb
until q "SELECT 1" >/dev/null 2>&1; do sleep 0.05; done
answered=$(( ($(date +%s%N) - t0) / 1000000 ))
# a write to a table with no map to warm: it waits for the lock, nothing else
w0=$(date +%s%N)
q "INSERT INTO w.small (v) VALUES (3)"
wrote=$(( ($(date +%s%N) - w0) / 1000000 ))
for i in $(seq 1 200); do grep -q 'asql: warmed' "$work/engine.log" && break; sleep 0.1; done
warm=$(grep 'asql: warmed' "$work/engine.log" || true)
warmed_at=$(( ($(date +%s%N) - t0) / 1000000 ))
mem=$(q "SHOW GLOBAL STATUS" | awk '$1=="Asql_row_index_mem_bytes"{print $2}')
echo "     restart: answered at ${answered} ms, a write took ${wrote} ms; $warm"
warm_ms=$(echo "$warm" | sed -nE 's/.* in ([0-9.]+) s.*/\1/p' | awk '{printf "%d", $1 * 1000}')
check "$( [ -n "$warm_ms" ] && [ $((wrote * 2)) -lt "$warm_ms" ] && echo ok)" "a write during the ${warm_ms:-?} ms warm-up waits ${wrote} ms, not for the warm-up"
n=$(echo "$warm" | sed -nE 's/.* ([0-9]+) primary-key indexes in.*/\1/p')
left=$(echo "$warm" | sed -nE 's/.*\(([0-9]+) primary-key maps left.*/\1/p')
check "$( [ -n "$n" ] && [ "$n" -ge 1 ] && [ "${left:-0}" -ge 1 ] && echo ok)" "warmed ${n:-?} row maps and left ${left:-?} for their first write: a $budget_mb MB budget holds some of the $((one >> 20)) MB, not all"
check "$( [ "$mem" -le $(( (budget_mb << 20) + per * 2 )) ] && echo ok)" "row maps after the warm-up: $((mem >> 20)) MB (budget $budget_mb MB, plus at most the last one built)"
stop
exit $fail
