#!/usr/bin/env bash
# WHAT IF <statement>[; ...]: what the writes would do to the Things - each key that would change, before and
# after, and the act a WHEN .. DO would run - without doing any of it (docs/design/thing-what-if.md). Two engines:
# the Things on one, the table a live act writes on the other, so "nothing was written" is checked on a real target.
#
#   thing-whatif-check.sh <aios binary>
set -u
BIN=$(realpath "${1:?usage: thing-whatif-check.sh <aios binary>}")
export AIOS_NO_ACCEL=1
W=$(mktemp -d)
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; rm -rf "$W"; }
trap cleanup EXIT
failed=0
check() { if [ "$2" = 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=1; fi; }
free_port() { python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'; }
umask 077
head -c 12 /dev/urandom | od -An -tx1 | tr -d ' \n' > "$W/pass"
head -c 12 /dev/urandom | od -An -tx1 | tr -d ' \n' > "$W/token"
printf '[client]\nuser=app\nhost=127.0.0.1\npassword=%s\n' "$(cat "$W/pass")" > "$W/c.cnf"
q() { mysql --defaults-extra-file="$W/c.cnf" -P "$1" -N -B -e "$2" 2>&1; }
start() {
  "$BIN" --root "$1" exec "compute serve port=$(free_port) lanes=1 sql=1 mysql_port=$2 mysql_bind=127.0.0.1 mysql_user=app mysql_pass_file=$W/pass token_file=$W/token" > "$1.log" 2>&1 < /dev/null & PIDS+=($!)
  for _ in $(seq 400); do q "$2" "SELECT 1" 2>/dev/null | grep -qx 1 && return 0; sleep 0.05; done
  echo "FAIL engine on $2 did not start"; exit 1
}
SRC=$(free_port); DST=$(free_port)
start "$W/dst" "$DST"
q "$DST" "CREATE DATABASE sink; USE sink; CREATE TABLE ready (orderno INT PRIMARY KEY, n INT)" > /dev/null
sink() { q "$DST" "SELECT IFNULL(GROUP_CONCAT(orderno ORDER BY orderno), '-') FROM sink.ready"; }
mkdir -p "$W/src/sql_db"
printf '[profile]\nid=sink\nhost=127.0.0.1\nport=%s\nuser=app\npassword_file=%s\ndatabase=sink\nthings_write=1\n' "$DST" "$W/pass" > "$W/src/sql_db/connections.ini"
start "$W/src" "$SRC"
s() { q "$SRC" "USE shop; $1"; }
q "$SRC" "CREATE DATABASE shop" > /dev/null
s "CREATE TABLE lines (id INT PRIMARY KEY, orderno INT, wh VARCHAR(4), qty INT, picked INT);
   INSERT INTO lines VALUES (1, 100, 'a', 2, 2), (2, 200, 'a', 5, 0), (3, 200, 'b', 1, 0)" > /dev/null
s "CREATE THING orders KEY orderno PARTITION BY orderno READS lines
   AS SELECT orderno, SUM(qty) AS qty, SUM(picked) AS picked, COUNT(*) AS n FROM lines GROUP BY orderno
   WHEN picked >= qty DO INSERT INTO ready (orderno, n) VALUES (KEY, THING.n) TO sink" > /dev/null
s "CREATE THING by_wh KEY wh PARTITION BY orderno READS lines AS SELECT wh, SUM(qty) AS qty FROM lines GROUP BY wh" > /dev/null
s "CREATE THING gt GRAIN KEY wh READS lines AS 'fn enroll() { return [\"a\"] } fn shared() { return 1 } fn things(ks, sh) { let o = {} for k in ks { o[k] = {\"n\": 1} } return o }'" > /dev/null
[ "$(s "SHOW THINGS" | cut -f1 | grep -cxE 'orders|by_wh|gt')" = 3 ] || { echo "FAIL setup: $(s "SHOW THINGS" | cut -f1 | tr '\n' ' ')"; exit 1; }
sleep 0.3
state() { echo "$(s "SELECT GROUP_CONCAT(CONCAT(id, ':', picked, ':', qty) ORDER BY id) FROM lines")|$(s "SHOW THING orders" | cut -f1-4 | tr '\n' ' ')|$(s "SHOW THING HISTORY orders '200'" | wc -l)|$(stat -c %s "$W/src/sql_db/things/orders.asof")|$(sink)"; }
S0=$(state)
check "order 100, already picked at CREATE, acted [$(sink)]" $([ "$(sink)" = "100" ] && echo 1 || echo 0)

r=$(s "WHAT IF UPDATE lines SET picked = qty WHERE orderno = 200")
echo "$r" | grep -q "^orders	200	changed	picked=0	picked=6	would act: INSERT INTO ready (orderno, n) VALUES (200, 2) TO sink"; rc=$?
check "a write that completes order 200: the change and the act it would run
       [$(echo "$r" | grep '^orders' | tr '\t' '|')]" $([ $rc = 0 ] && echo 1 || echo 0)
check "  the partitioned Thing is not changed by it (qty unchanged), so it is not listed" $(echo "$r" | grep -q '^orders' && ! echo "$r" | grep -q '^by_wh' && echo 1 || echo 0)
echo "$r" | grep -q "^gt	.*would re-run"; rc=$?; check "  the grain Thing that reads lines is named as one that would re-run" $([ $rc = 0 ] && echo 1 || echo 0)
check "  and nothing happened: the table, the Thing, its history, its AS OF log, the target [$(state)]" $([ "$(state)" = "$S0" ] && echo 1 || echo 0)

# several statements: in one string, as the mysql command line needs (it splits a query on its own ';'s)
r=$(s "WHAT IF 'INSERT INTO lines VALUES (9, 400, ''b'', 3, 0); UPDATE lines SET qty = 4 WHERE id = 1'")
echo "$r" | grep -q "^orders	400	new		qty=3 | picked=0 | n=1	" && echo "$r" | grep -q "^orders	100	changed	qty=2	qty=4	would re-arm" \
  && echo "$r" | grep -q "^by_wh	b	changed	qty=1	qty=4	" && echo "$r" | grep -q "^by_wh	a	changed	qty=7	qty=9	"; rc=$?
check "two statements: a new order, an order no longer picked in full (re-arm), both warehouses
       [$(echo "$r" | tr '\t' '|' | tr '\n' ' ')]" $([ $rc = 0 ] && echo 1 || echo 0)
r=$(s "WHAT IF DELETE FROM lines WHERE orderno = 100")
echo "$r" | grep -q "^orders	100	gone	qty=2 | picked=2 | n=1		"; rc=$?; check "a delete: the order would be gone [$(echo "$r" | grep '^orders' | tr '\t' '|')]" $([ $rc = 0 ] && echo 1 || echo 0)
check "  and still nothing happened [$(state)]" $([ "$(state)" = "$S0" ] && echo 1 || echo 0)

r=$(s "WHAT IF 'UPDATE lines SET picked = 1 WHERE id = 2; INSERT INTO lines VALUES (1, 1, ''a'', 1, 1)'")
check "a statement that fails: its error [$(echo "$r" | tail -1 | cut -c1-80)]" $(echo "$r" | grep -qi "duplicate" && echo 1 || echo 0)
check "  and the one before it was rolled back too [$(state)]" $([ "$(state)" = "$S0" ] && echo 1 || echo 0)
r=$(s "START TRANSACTION; WHAT IF UPDATE lines SET picked = 0")
check "inside an open transaction it is refused [$(echo "$r" | tail -1 | cut -c1-90)]" $(echo "$r" | grep -qi "transaction of its own" && echo 1 || echo 0)

# after all that, a real write acts as it always did
s "UPDATE lines SET picked = qty WHERE orderno = 200" > /dev/null
for _ in $(seq 60); do [ "$(sink)" = "100,200" ] && break; sleep 0.05; done
check "a real write afterwards acts as before [$(sink)]" $([ "$(sink)" = "100,200" ] && echo 1 || echo 0)

[ "$failed" = 0 ] && echo "thing-whatif: ok"
exit $failed
