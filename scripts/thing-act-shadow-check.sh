#!/usr/bin/env bash
# A row Thing's act in shadow: WHEN .. DO .. TO <profile> SHADOW works the act out and records it ("would act",
# SHOW THING SHADOW) without writing; SET THING <name> LIVE turns it on for what comes true from then on, and
# SET THING <name> SHADOW back off. Two real engines: the Thing on one, the table its act would write on the other.
#
#   thing-act-shadow-check.sh <aios binary>
set -u
BIN=$(realpath "${1:?usage: thing-act-shadow-check.sh <aios binary>}")
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
SRC=$(free_port); DST=$(free_port)

start "$W/dst" "$DST"
q "$DST" "CREATE DATABASE sink; USE sink; CREATE TABLE ready (orderno INT PRIMARY KEY, n INT)" > /dev/null
rows() { q "$DST" "SELECT GROUP_CONCAT(orderno ORDER BY orderno) FROM sink.ready"; }

# two profiles for the same target: one a Thing may write through, one no one has said it may
mkdir -p "$W/src/sql_db"
printf '[profile]\nid=sink\nname=writable\nhost=127.0.0.1\nport=%s\nuser=app\npassword_file=%s\ndatabase=sink\nthings_write=1\n[profile]\nid=look\nname=not opted in\nhost=127.0.0.1\nport=%s\nuser=app\npassword_file=%s\ndatabase=sink\n' \
  "$DST" "$W/pass" "$DST" "$W/pass" > "$W/src/sql_db/connections.ini"
start "$W/src" "$SRC"; SRC_PID=${PIDS[-1]}
q "$SRC" "CREATE DATABASE shop; USE shop;
  CREATE TABLE lines (id INT PRIMARY KEY, orderno INT, qty INT, picked INT);
  INSERT INTO lines VALUES (1, 100, 2, 2), (2, 200, 3, 0), (3, 300, 1, 0)" > /dev/null
mk() {   # mk <name> <TO clause>
  q "$SRC" "USE shop; CREATE THING $1 KEY orderno PARTITION BY orderno READS lines
    AS SELECT orderno, SUM(qty) AS qty, SUM(picked) AS picked FROM lines GROUP BY orderno
    WHEN picked >= qty DO INSERT INTO ready (orderno, n) VALUES (KEY, 1) TO $2"
}

# without SHADOW, a profile not opted in is refused (as before); with it, the Thing is made
out=$(mk nope look); echo "$out" | grep -q "things_write=1"; check "a profile without things_write=1 is refused for a live act" $([ $? = 0 ] && echo 1 || echo 0)
out=$(mk watch "look SHADOW")
q "$SRC" "USE shop; SHOW THINGS" | grep -q '^watch'; check "... and accepted in shadow $out" $([ $? = 0 ] && echo 1 || echo 0)
sleep 0.3
check "in shadow nothing is written to the target [$(rows)]" $([ "$(rows)" = "NULL" ] && echo 1 || echo 0)
s=$(q "$SRC" "USE shop; SHOW THING SHADOW watch")
echo "$s" | grep -q "	100	INSERT INTO ready (orderno, n) VALUES (100, 1) TO look	shadow"
check "SHOW THING SHADOW lists the act for the key already true at CREATE, as it would run" $([ $? = 0 ] && echo 1 || echo 0)
q "$SRC" "USE shop; UPDATE lines SET picked = 3 WHERE id = 2" > /dev/null
sleep 0.3
s=$(q "$SRC" "USE shop; SHOW THING SHADOW watch")
check "a key that comes true later is listed too, newest first [$(echo "$s" | cut -f2 | tr '\n' ' ')]" \
  $([ "$(echo "$s" | cut -f2 | tr '\n' ' ')" = "200 100 " ] && echo 1 || echo 0)
q "$SRC" "USE shop; SHOW THING HISTORY watch '200'" | grep -q "would act"
check "the key's history says it would have acted" $([ $? = 0 ] && echo 1 || echo 0)
out=$(q "$SRC" "USE shop; SET THING watch LIVE"); echo "$out" | grep -q "things_write=1"
check "SET THING .. LIVE is refused while the profile is not opted in" $([ $? = 0 ] && echo 1 || echo 0)

# shadow on a writable profile, then live: only what comes true after that writes
mk act "sink SHADOW" > /dev/null
sleep 0.3
check "a shadow Thing on a writable profile writes nothing either [$(rows)]" $([ "$(rows)" = "NULL" ] && echo 1 || echo 0)
q "$SRC" "USE shop; SHOW THINGS" | grep '^act' | grep -q "on sink (shadow)"
check "SHOW THINGS says it is in shadow" $([ $? = 0 ] && echo 1 || echo 0)
out=$(q "$SRC" "USE shop; SET THING act LIVE")
q "$SRC" "USE shop; SHOW THINGS" | grep '^act' | grep "on sink" | grep -vq "(shadow)"
check "SET THING act LIVE: SHOW THINGS no longer says shadow $out" $([ $? = 0 ] && echo 1 || echo 0)
sleep 0.3
check "going live does not replay what it would have done [$(rows)]" $([ "$(rows)" = "NULL" ] && echo 1 || echo 0)
q "$SRC" "USE shop; UPDATE lines SET picked = 1 WHERE id = 3" > /dev/null
for _ in $(seq 60); do [ "$(rows)" = "300" ] && break; sleep 0.05; done
check "a key that comes true after it went live writes [$(rows)]" $([ "$(rows)" = "300" ] && echo 1 || echo 0)

# the mode outlives a restart; SHADOW again stops the writes
kill "$SRC_PID"; wait "$SRC_PID" 2>/dev/null
start "$W/src" "$SRC"
q "$SRC" "USE shop; INSERT INTO lines VALUES (4, 400, 1, 1)" > /dev/null
for _ in $(seq 60); do [ "$(rows)" = "300,400" ] && break; sleep 0.05; done
check "after a restart it is still live [$(rows)]" $([ "$(rows)" = "300,400" ] && echo 1 || echo 0)
q "$SRC" "USE shop; SET THING act SHADOW" > /dev/null
q "$SRC" "USE shop; INSERT INTO lines VALUES (5, 500, 1, 1)" > /dev/null
sleep 0.3
check "SET THING act SHADOW: the next one is recorded, not written [$(rows)]" \
  $([ "$(rows)" = "300,400" ] && q "$SRC" "USE shop; SHOW THING SHADOW act" | grep -q "	500	" && echo 1 || echo 0)

[ "$failed" = 0 ] && echo "thing-act-shadow: ok"
exit $failed
