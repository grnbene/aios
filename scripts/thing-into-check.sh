#!/usr/bin/env bash
# CREATE THING .. INTO <db>.<table> (docs/design/thing-into.md): a table the engine keeps equal to a Thing, which
# another Thing can read (a Thing built on a Thing) and SQL can join. One engine: the table at CREATE, a Thing on
# it, a change flowing through both, client writes refused, a cycle refused, a rolled-back and a committed
# transaction, a restart, DROP THING taking its table.
#
#   thing-into-check.sh <aios binary>
set -u
BIN=$(realpath "${1:?usage: thing-into-check.sh <aios binary>}")
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
P=$(free_port)
ENGINE=
start() {
  "$BIN" --root "$W/r" exec "compute serve port=$(free_port) lanes=1 sql=1 mysql_port=$P mysql_bind=127.0.0.1 mysql_user=app mysql_pass_file=$W/pass token_file=$W/token" > "$W/e.log" 2>&1 < /dev/null &
  ENGINE=$!; PIDS+=($ENGINE)
  for _ in $(seq 400); do mysql --defaults-extra-file="$W/c.cnf" -P "$P" -N -B -e "SELECT 1" 2>/dev/null | grep -qx 1 && return 0; sleep 0.05; done
  echo "FAIL engine did not start"; exit 1
}
s() { mysql --defaults-extra-file="$W/c.cnf" -P "$P" -N -B -e "USE s; $1" 2>&1 | grep -v -- '^-*$' | grep -v '^$'; }
rows() { s "SELECT GROUP_CONCAT(CONCAT($2) ORDER BY $3) FROM $1"; }
start
mysql --defaults-extra-file="$W/c.cnf" -P "$P" -e "CREATE DATABASE s" > /dev/null 2>&1
s "CREATE TABLE lines (id INT PRIMARY KEY, orderno INT, wh VARCHAR(4), qty INT);
   INSERT INTO lines VALUES (1, 100, 'a', 2), (2, 100, 'b', 1), (3, 200, 'b', 5)" > /dev/null

s "CREATE THING orders KEY orderno PARTITION BY orderno READS lines INTO s.orders_t
   AS SELECT orderno, SUM(qty) AS qty, MAX(wh) AS wh FROM lines GROUP BY orderno" > /dev/null
r=$(rows orders_t "orderno, ':', qty, ':', wh" orderno)
check "the table is there when CREATE returns, one row per order [$r]" $([ "$r" = "100:3:b,200:5:b" ] && echo 1 || echo 0)
s "CREATE THING by_wh KEY wh PARTITION BY wh READS orders_t
   AS SELECT wh, SUM(CAST(qty AS SIGNED)) AS qty, COUNT(*) AS orders FROM orders_t GROUP BY wh" > /dev/null
r=$(s "SHOW THING by_wh" | cut -f1-3 | tr '\t\n' ': ')
check "a Thing built on it [$r]" $([ "$r" = "b:8:2 " ] && echo 1 || echo 0)

s "UPDATE lines SET qty = 10 WHERE id = 3; INSERT INTO lines VALUES (4, 300, 'a', 7)" > /dev/null
r=$(rows orders_t "orderno, ':', qty" orderno); r2=$(s "SHOW THING by_wh" | cut -f1-3 | tr '\t\n' ': ')
check "a change reaches the table and the Thing on it [$r | $r2]" $([ "$r" = "100:3,200:10,300:7" ] && [ "$r2" = "a:7:1 b:13:2 " ] && echo 1 || echo 0)
r=$(s "SELECT GROUP_CONCAT(CONCAT(l.id, ':', o.qty) ORDER BY l.id) FROM lines l JOIN orders_t o ON o.orderno = l.orderno")
check "SQL joins it like a table [$r]" $([ "$r" = "1:3,2:3,3:10,4:7" ] && echo 1 || echo 0)

n=0
for q in "UPDATE orders_t SET qty = '0'" "DELETE FROM s.orders_t" "INSERT INTO \`orders_t\` VALUES ('9', '9', 'a')" "REPLACE INTO orders_t VALUES ('9', '9', 'a')" "TRUNCATE TABLE orders_t" "DROP TABLE orders_t"; do
  s "$q" | grep -q "kept by Thing orders" && n=$((n + 1))
done
r=$(rows orders_t "orderno, ':', qty" orderno)
check "a client's UPDATE, DELETE, INSERT, REPLACE, TRUNCATE and DROP are refused ($n of 6), the table untouched [$r]" \
  $([ "$n" = 6 ] && [ "$r" = "100:3,200:10,300:7" ] && echo 1 || echo 0)

r=$(s "START TRANSACTION; UPDATE lines SET qty = 50 WHERE id = 1; SELECT qty FROM orders_t WHERE orderno = '100'; ROLLBACK; SELECT qty FROM orders_t WHERE orderno = '100'" | tr '\n' ' ')
check "an open transaction is not in the table, nor after its ROLLBACK [$r]" $([ "$r" = "3 3 " ] && echo 1 || echo 0)
r=$(s "START TRANSACTION; UPDATE lines SET qty = 50 WHERE id = 1; COMMIT; SELECT qty FROM orders_t WHERE orderno = '100'")
check "a committed one is [$r]" $([ "$r" = "51" ] && echo 1 || echo 0)

# a cycle: e keeps z, f reads z and keeps w; e goes and z is made by hand, then dropped; h reading w may not keep z
s "CREATE THING e KEY orderno PARTITION BY orderno READS lines INTO s.z AS SELECT orderno, COUNT(*) AS n FROM lines GROUP BY orderno" > /dev/null
s "CREATE THING f KEY orderno PARTITION BY orderno READS z INTO s.w AS SELECT orderno, COUNT(*) AS c FROM z GROUP BY orderno" > /dev/null
s "DROP THING e" > /dev/null
r=$(s "CREATE THING h KEY orderno PARTITION BY orderno READS w INTO s.z AS SELECT orderno, COUNT(*) AS c FROM w GROUP BY orderno")
check "a Thing whose reads lead back to its own table is refused, naming the chain [$r]" $(echo "$r" | grep -q "a cycle: h reads f, which reads s.z, which h keeps" && echo 1 || echo 0)
r=$(s "CREATE THING x KEY orderno PARTITION BY orderno READS lines INTO s.lines AS SELECT orderno, COUNT(*) AS n FROM lines GROUP BY orderno")
check "INTO a table that exists is refused [$r]" $(echo "$r" | grep -q "s.lines exists" && echo 1 || echo 0)

kill "$ENGINE"; wait "$ENGINE" 2>/dev/null; start
r=$(rows orders_t "orderno, ':', qty" orderno)
check "after a restart the table is as the Thing [$r]" $([ "$r" = "100:51,200:10,300:7" ] && echo 1 || echo 0)
s "UPDATE lines SET qty = 1 WHERE id = 2" > /dev/null
r=$(s "SHOW THING by_wh" | cut -f1-3 | tr '\t\n' ': ')
check "  and a change still flows through both [$r]" $([ "$r" = "a:7:1 b:61:2 " ] && echo 1 || echo 0)

s "DROP THING by_wh" > /dev/null; s "DROP THING f" > /dev/null; s "DROP THING orders" > /dev/null
r=$(s "SHOW TABLES" | tr '\n' ' ')
check "DROP THING takes its table with it [$r]" $([ "$r" = "lines " ] && echo 1 || echo 0)

[ "$failed" = 0 ] && echo "thing-into: ok"
exit $failed
