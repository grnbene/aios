#!/usr/bin/env bash
# asql-subquery-batch-race-check.sh [aios binary]: a correlated scalar subquery over a view, answered for every key at
# once (scalar_batch_for), stays right and keeps the engine up while other sessions write.
#
# A batch over a view is current only while no table has been written since. A write elsewhere made the next lane
# to look find it out of date and free it while it built the next one, but the batch stayed published for the
# readers that take no lock (sub_batch_ready): another lane of the same statement read the freed batch's table
# pointer. 3307 died with SIGSEGV at 0x58 on the ERP's nopurchase report (totalspend / lastseen per customer from
# the stocktrans view) while the v8 app wrote.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
bin=$(realpath "${1:-$here/build/aios}")
work=$(mktemp -d /tmp/asql-subquery-batch-race.XXXXXX)
read -r port cport < <(python3 -c 'import socket
a, b = socket.socket(), socket.socket()
a.bind(("127.0.0.1", 0)); b.bind(("127.0.0.1", 0))
print(a.getsockname()[1], b.getsockname()[1])')
pid=""
fail=0
cleanup() {
  set +e
  [ -n "$pid" ] && kill "$pid" 2>/dev/null; wait 2>/dev/null
  [ "$fail" = 0 ] || grep -E 'fatal signal|Last statement' "$work/engine.log" 2>/dev/null | cut -c1-200 | head -4
  [ -n "${KEEP_LOG:-}" ] && cp "$work/engine.log" "$KEEP_LOG"
  rm -rf "$work"
}
trap cleanup EXIT
umask 077
mkdir -p "$work/root"
head -c 12 /dev/urandom | od -An -tx1 | tr -d ' \n' > "$work/pw"
printf '[client]\nuser=erp\npassword=%s\nhost=127.0.0.1\nport=%s\n' "$(cat "$work/pw")" "$port" > "$work/my.cnf"
q() { mysql --defaults-extra-file="$work/my.cnf" -N -e "$1"; }
check() { if [ "$1" = ok ]; then echo "ok   $2"; else echo "FAIL $2"; fail=1; fi; }
(cd "$work" && exec env AIOS_NO_ACCEL=1 "$bin" --root "$work/root" exec \
   "compute serve port=$cport lanes=8 sql=1 mysql_port=$port mysql_bind=127.0.0.1 mysql_user=erp mysql_pass_file=$work/pw" \
   > "$work/engine.log" 2>&1) &
pid=$!
for i in $(seq 1 150); do q "SELECT 1" >/dev/null 2>&1 && break; sleep 0.2; done
q "SELECT 1" >/dev/null || { fail=1; echo "FAIL the engine did not start"; exit 1; }

# 4,000 customers, 400,000 moves over 50 items, a view joining them (as stocktrans joins stockmoves to its parents),
# and a table the writers touch that the query never reads.
q "CREATE DATABASE r"
q "CREATE TABLE r.n (n INT PRIMARY KEY AUTO_INCREMENT, v INT)"
q "INSERT INTO r.n (v) VALUES (1)"
for k in $(seq 1 12); do q "INSERT INTO r.n (v) SELECT v FROM r.n"; done   # 4,096 rows
q "CREATE TABLE r.cust (debtorno VARCHAR(12) PRIMARY KEY, name VARCHAR(40))"
q "INSERT INTO r.cust SELECT CONCAT('C', n), CONCAT('name ', n) FROM r.n WHERE n <= 4000"
q "CREATE TABLE r.item (stockid VARCHAR(12) PRIMARY KEY, cat INT)"
q "INSERT INTO r.item SELECT CONCAT('S', n), n % 5 FROM r.n WHERE n <= 50"
q "CREATE TABLE r.moves (id INT PRIMARY KEY AUTO_INCREMENT, debtorno VARCHAR(12), stockid VARCHAR(12), amt INT, trandate DATE)"
for rep in $(seq 1 100); do
  q "INSERT INTO r.moves (debtorno, stockid, amt, trandate) SELECT CONCAT('C', ((n * 7 + $rep) % 4000) + 1), CONCAT('S', ((n + $rep) % 50) + 1), (n + $rep) % 97, DATE_ADD('2024-01-01', INTERVAL (n + $rep) % 600 DAY) FROM r.n WHERE n <= 4000"
done
q "CREATE VIEW r.trans AS SELECT m.debtorno, m.stockid, m.amt, m.trandate, i.cat FROM r.moves m JOIN r.item i ON i.stockid = m.stockid"
q "CREATE TABLE r.touch (id INT PRIMARY KEY AUTO_INCREMENT, v INT)"

sel="SELECT c.debtorno, (SELECT SUM(amt) FROM r.trans WHERE debtorno = c.debtorno) AS spend, (SELECT MAX(trandate) FROM r.trans WHERE r.trans.debtorno = c.debtorno) AS lastseen FROM r.cust c ORDER BY c.debtorno"
# The answer the nested statements give one at a time, with no batch: every run must match it.
want=$(q "SELECT MD5(GROUP_CONCAT(CONCAT(d, ':', IFNULL(s, ''), ':', IFNULL(l, '')) ORDER BY d SEPARATOR ',')) FROM (SELECT c.debtorno AS d, SUM(m.amt) AS s, MAX(m.trandate) AS l FROM r.cust c LEFT JOIN r.moves m ON m.debtorno = c.debtorno GROUP BY c.debtorno) x")

# Writers: a row a few milliseconds into the table the query never reads, so each batch goes out of date mid-run.
stop_at=$(( $(date +%s) + ${RACE_SECS:-40} ))
for w in 1 2; do
  ( while [ "$(date +%s)" -lt "$stop_at" ]; do q "INSERT INTO r.touch (v) VALUES ($w)" >/dev/null 2>&1 || true; done ) &
done
# Readers: four sessions running the report over and over, each answer checked.
for s in 1 2 3 4; do
  ( bad=0; runs=0
    while [ "$(date +%s)" -lt "$stop_at" ]; do
      got=$(q "SELECT MD5(GROUP_CONCAT(CONCAT(debtorno, ':', IFNULL(spend, ''), ':', IFNULL(lastseen, '')) ORDER BY debtorno SEPARATOR ',')) FROM ($sel) x" 2>&1) || { echo "error $got" > "$work/reader$s"; exit 0; }
      runs=$((runs + 1)); [ "$got" = "$want" ] || bad=$((bad + 1))
    done
    echo "runs $runs bad $bad" > "$work/reader$s" ) &
done
wait $(jobs -p | grep -v "^$pid$") 2>/dev/null || true
for s in 1 2 3 4; do r=$(cat "$work/reader$s" 2>/dev/null || echo "no result"); echo "     reader $s: $r"; done
alive=$(kill -0 "$pid" 2>/dev/null && echo ok || true)
check "$alive" "the engine stayed up through $(q "SELECT COUNT(*) FROM r.touch" 2>/dev/null || echo '?') writes beside the reports"
check "$( ! grep -q 'fatal signal' "$work/engine.log" && echo ok)" "no fatal signal in the engine log"
total=0; badsum=0; errs=0
for s in 1 2 3 4; do
  read -r _ r _ b < "$work/reader$s" 2>/dev/null || { errs=$((errs + 1)); continue; }
  [ "$_" = "runs" ] 2>/dev/null || true
  case "$r" in ''|*[!0-9]*) errs=$((errs + 1)); continue;; esac
  total=$((total + r)); badsum=$((badsum + b))
done
check "$( [ "$errs" = 0 ] && [ "$total" -ge 8 ] && [ "$badsum" = 0 ] && echo ok)" "$total reports, $badsum wrong, $errs sessions cut off"
exit $fail
