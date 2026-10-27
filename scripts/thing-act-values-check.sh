#!/usr/bin/env bash
# A row Thing's act given the Thing's values: inside DO, THING.<label> is that value of the Thing that acted, as a
# literal (a number as itself, text quoted, NULL as NULL), as KEY is its key. Two real engines: the Thing on one, its
# DO statements writing into a table on the other through a things_write=1 profile.
#
#   thing-act-values-check.sh <aios binary>
set -u
BIN=$(realpath "${1:?usage: thing-act-values-check.sh <aios binary>}")
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
q() { mysql --defaults-extra-file="$W/c.cnf" -P "$1" -N -B -e "$2" 2>&1; }   # q <port> <sql>
start() {   # start <root> <sql port>
  rm -f "$1.log"
  "$BIN" --root "$1" exec "compute serve port=$(free_port) lanes=1 sql=1 mysql_port=$2 mysql_bind=127.0.0.1 mysql_user=app mysql_pass_file=$W/pass token_file=$W/token" > "$1.log" 2>&1 < /dev/null & PIDS+=($!)
  # the port opens before the tables are loaded ("ASQL is starting"): up is when a query is answered
  for _ in $(seq 400); do q "$2" "SELECT 1" 2>/dev/null | grep -qx 1 && return 0; sleep 0.05; done
  echo "FAIL engine on $2 did not start: $(tail -n 3 "$1.log")"; exit 1
}
SRC_PID=
SRC=$(free_port); DST=$(free_port)

# the target: a table the act writes into
start "$W/dst" "$DST"
q "$DST" "CREATE DATABASE sink; USE sink; CREATE TABLE ready (orderno INT PRIMARY KEY, lines INT, total DECIMAL(10,2), note VARCHAR(40), who VARCHAR(20), tag VARCHAR(20))" > /dev/null

# the source, with a profile for the target
mkdir -p "$W/src/sql_db"
printf '[profile]\nid=sink\nname=the target\nhost=127.0.0.1\nport=%s\nuser=app\npassword_file=%s\ndatabase=sink\nthings_write=1\n' "$DST" "$W/pass" > "$W/src/sql_db/connections.ini"
start "$W/src" "$SRC"; SRC_PID=${PIDS[-1]}
q "$SRC" "CREATE DATABASE shop; USE shop;
  CREATE TABLE lines (id INT PRIMARY KEY, orderno INT, qty INT, picked INT, price DECIMAL(10,2), note VARCHAR(40), who VARCHAR(20));
  INSERT INTO lines VALUES (1, 100, 2, 2, 5.25, 'it''s fine', NULL), (2, 100, 1, 1, 10.00, 'it''s fine', NULL),
                           (3, 200, 3, 0, 1.50, 'later', 'ann')" > /dev/null

# a label the Thing does not have: refused when the Thing is made, not when it first acts
out=$(q "$SRC" "USE shop; CREATE THING bad KEY orderno PARTITION BY orderno READS lines
  AS SELECT orderno, COUNT(*) AS line_count FROM lines GROUP BY orderno
  WHEN line_count > 0 DO INSERT INTO ready (orderno, lines) VALUES (KEY, THING.nope) TO sink")
echo "$out" | grep -q "THING.nope" && echo "$out" | grep -qi "line_count"
check "an unknown THING.<label> is refused at CREATE, naming the labels there are" $([ $? = 0 ] && echo 1 || echo 0)
q "$SRC" "USE shop; SHOW THINGS" | grep -q '^bad' && check "  and no Thing was made" 0 || check "  and no Thing was made" 1

# the Thing: its values in the act, in any case, beside KEY; 'thing.x' inside quotes stays text
out=$(q "$SRC" "USE shop; CREATE THING order_pick KEY orderno PARTITION BY orderno READS lines
  AS SELECT orderno, COUNT(*) AS line_count, SUM(qty) AS qty, SUM(picked) AS picked, SUM(price) AS total,
            MAX(note) AS note, MAX(who) AS who FROM lines GROUP BY orderno
  WHEN picked >= qty DO INSERT INTO ready (orderno, lines, total, note, who, tag)
                         VALUES (KEY, THING.line_count, thing.Total, THING.note, THING.who, 'thing.note') TO sink")
q "$SRC" "USE shop; SHOW THINGS" | grep -q '^order_pick'; check "a Thing whose act uses its values is made $out" $([ $? = 0 ] && echo 1 || echo 0)

wait_row() { for _ in $(seq 100); do r=$(q "$DST" "SELECT orderno, lines, total, note, IFNULL(who, 'NULL'), tag FROM sink.ready WHERE orderno = $1"); [ -n "$r" ] && { echo "$r"; return; }; sleep 0.05; done; }
r=$(wait_row 100)
check "an order already picked at CREATE acted with its values: count, decimal, text with a quote, NULL [$r]" \
  $([ "$r" = "$(printf '100\t2\t15.25\tit'"'"'s fine\tNULL\tthing.note')" ] && echo 1 || echo 0)

# a change that makes the condition true: the act carries the values as they are now
q "$SRC" "USE shop; UPDATE lines SET picked = 3, price = 2.75 WHERE id = 3" > /dev/null
r=$(wait_row 200)
check "an order picked later acted with the values after the change [$r]" \
  $([ "$r" = "$(printf '200\t1\t2.75\tlater\tann\tthing.note')" ] && echo 1 || echo 0)

# a label with a space, written with backticks
out=$(q "$SRC" "USE shop; CREATE THING big_order KEY orderno PARTITION BY orderno READS lines
  AS SELECT orderno, SUM(qty) AS \`units in all\` FROM lines GROUP BY orderno
  WHEN \`units in all\` >= 3 DO INSERT INTO ready (orderno, lines, tag) VALUES (KEY + 1000, THING.\`units in all\`, 'big') TO sink")
r=$(wait_row 1100)
check "THING.\`label\` for a label with a space [$r] $out" $(echo "$r" | grep -q "^1100	3	" && echo 1 || echo 0)

# after a restart the Thing is read back from its file: its act still fills in the values
kill "$SRC_PID"; wait "$SRC_PID" 2>/dev/null
start "$W/src" "$SRC"
q "$SRC" "USE shop; INSERT INTO lines VALUES (4, 300, 1, 1, 9.99, 'after restart', 'bo')" > /dev/null
r=$(wait_row 300)
check "after a restart, a new order acts with its values [$r]" \
  $([ "$r" = "$(printf '300\t1\t9.99\tafter restart\tbo\tthing.note')" ] && echo 1 || echo 0)

h=$(q "$SRC" "USE shop; SHOW THING HISTORY order_pick '200'")
echo "$h" | grep -q "acted"; check "the history records the act" $([ $? = 0 ] && echo 1 || echo 0)

[ "$failed" = 0 ] && echo "thing-act-values: ok"
exit $failed
