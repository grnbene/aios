#!/usr/bin/env bash
# SHOW THING <name> AS OF '<time>': a row Thing as it stood at a moment, from the change log kept beside it
# (docs/design/thing-time-travel.md). One engine: values at each moment, a key that went away, KEYS, a time before
# the log, a restart, KEEP 0 DAYS, the size cap; and, given the binary before this, a Thing that binary made gets
# its log started when this one loads it.
#
#   thing-asof-check.sh <aios binary> [<an aios binary without AS OF>]
set -u
BIN=$(realpath "${1:?usage: thing-asof-check.sh <aios binary> [older aios binary]}")
OLD=${2:+$(realpath "$2")}
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
q() { mysql --defaults-extra-file="$W/c.cnf" -P "$P" -N -B -e "$1" 2>&1; }
s() { q "USE shop; $1"; }
ENGINE=
start() {   # start <binary> [env...]
  local bin=$1; shift
  rm -f "$W/e.log"
  env "$@" "$bin" --root "$W/r" exec "compute serve port=$(free_port) lanes=1 sql=1 mysql_port=$P mysql_bind=127.0.0.1 mysql_user=app mysql_pass_file=$W/pass token_file=$W/token" > "$W/e.log" 2>&1 < /dev/null &
  ENGINE=$!; PIDS+=($ENGINE)
  for _ in $(seq 400); do q "SELECT 1" 2>/dev/null | grep -qx 1 && return 0; sleep 0.05; done
  echo "FAIL engine did not start: $(tail -n 3 "$W/e.log")"; exit 1
}
stop() { kill "$ENGINE"; wait "$ENGINE" 2>/dev/null; }
now() { date '+%F %T'; }
tick() { sleep 1.1; }   # AS OF has a resolution of one second

start "$BIN"
q "CREATE DATABASE shop" > /dev/null
s "CREATE TABLE lines (id INT PRIMARY KEY, orderno INT, wh VARCHAR(4), qty INT, picked INT)
   ; INSERT INTO lines VALUES (1, 100, 'a', 2, 0), (2, 100, 'b', 1, 0), (3, 200, 'a', 5, 5)" > /dev/null
s "CREATE THING orders KEY orderno PARTITION BY orderno READS lines
   AS SELECT orderno, SUM(qty) AS qty, SUM(picked) AS picked FROM lines GROUP BY orderno" > /dev/null
s "CREATE THING by_wh KEY wh PARTITION BY orderno READS lines AS SELECT wh, SUM(qty) AS qty FROM lines GROUP BY wh" > /dev/null
tick; T0=$(now); tick
s "UPDATE lines SET picked = 1 WHERE id = 1" > /dev/null
tick; T1=$(now); tick
s "DELETE FROM lines WHERE orderno = 200" > /dev/null
tick; T2=$(now)

r=$(s "SHOW THING orders AS OF '$T0'" | cut -f1-3 | tr '\n' ' ')
check "AS OF before the change: both orders, nothing picked [$r]" $([ "$r" = "100	3	0 200	5	5 " ] && echo 1 || echo 0)
r=$(s "SHOW THING orders AS OF '$T1'" | cut -f1-3 | tr '\n' ' ')
check "AS OF after it: order 100 picked 1 [$r]" $([ "$r" = "100	3	1 200	5	5 " ] && echo 1 || echo 0)
r=$(s "SHOW THING orders AS OF '$T2'" | cut -f1-3 | tr '\n' ' ')
check "AS OF after order 200's lines were deleted: it is gone [$r]" $([ "$r" = "100	3	1 " ] && echo 1 || echo 0)
r=$(s "SHOW THING orders AS OF '$T1'" | grep '^100')
echo "$r" | grep -q "lines update" && [ "$(echo "$r" | cut -f4)" \> "$T0" ]; rc=$?
check "each row says when its value was set and what set it [$(echo "$r" | cut -f4-)]" $([ $rc = 0 ] && echo 1 || echo 0)
r=$(s "SHOW THING orders AS OF '$T1' KEYS ('200')" | cut -f1 | tr '\n' ' ')
check "KEYS narrows it [$r]" $([ "$r" = "200 " ] && echo 1 || echo 0)
r=$(s "SHOW THING by_wh AS OF '$T0'" | cut -f1-2 | tr '\n' ' ')
r2=$(s "SHOW THING by_wh AS OF '$T2'" | cut -f1-2 | tr '\n' ' ')
check "a partitioned Thing too [$r] -> [$r2]" $([ "$r" = "a	7 b	1 " ] && [ "$r2" = "a	2 b	1 " ] && echo 1 || echo 0)
r=$(s "SHOW THING orders AS OF '2001-01-01 00:00:00'")
check "a time before the log began is refused, naming when it begins [$r]" $(echo "$r" | grep -q "$(date '+%F')" && echo 1 || echo 0)
r=$(s "SHOW THING orders AS OF 'yesterday'")
check "a time that is not one is refused [$r]" $(echo "$r" | grep -qi "YYYY-MM-DD" && echo 1 || echo 0)

stop; start "$BIN"
r=$(s "SHOW THING orders AS OF '$T0'" | cut -f1-3 | tr '\n' ' ')
check "after a restart the past is still there [$r]" $([ "$r" = "100	3	0 200	5	5 " ] && echo 1 || echo 0)

s "SET THING orders KEEP 0 DAYS" > /dev/null
r=$(s "SHOW THING orders AS OF '$T1'")
check "KEEP 0 DAYS turns it off [$r]" $(echo "$r" | grep -qi "keeps no" && [ ! -e "$W/r/sql_db/things/orders.asof" ] && echo 1 || echo 0)
s "SET THING orders KEEP 30 DAYS" > /dev/null
s "UPDATE lines SET picked = 2 WHERE id = 1" > /dev/null
tick
r=$(s "SHOW THING orders AS OF '$(now)'" | cut -f1-3 | tr '\n' ' ')
check "KEEP 30 DAYS turns it on again from now [$r]" $([ "$r" = "100	3	2 " ] && echo 1 || echo 0)

# the size cap: a busy Thing keeps the last half-cap of changes, and answers for the latest moment
stop; start "$BIN" AIOS_THING_KEEP_KB=16
for i in $(seq 1 120); do echo "UPDATE lines SET picked = $((i % 3)), qty = $((i + 2)) WHERE id = 2;"; done > "$W/busy.sql"
s "$(cat "$W/busy.sql")" > /dev/null
tick
sz=$(stat -c %s "$W/r/sql_db/things/orders.asof" 2>/dev/null || echo 0)
r=$(s "SHOW THING orders AS OF '$(now)'" | cut -f1-3 | tr '\n' ' ')
check "under a 16 KB cap the log stays under it ($sz bytes) and answers the latest state [$r]" \
  $([ "$sz" -gt 0 ] && [ "$sz" -le 16384 ] && [ "$r" = "100	124	2 " ] && echo 1 || echo 0)
r=$(s "SHOW THING orders AS OF '$T1'")
check "  and refuses a time older than what it kept [$(echo "$r" | tail -1 | cut -c1-90)]" $(echo "$r" | grep -qi "kept from" && echo 1 || echo 0)

# many keys under a small cap: the base alone is over half the cap, and the log is not rewritten at every change
s "CREATE TABLE wide (id INT PRIMARY KEY, v INT)" > /dev/null
for i in $(seq 1 400); do echo "INSERT INTO wide VALUES ($i, 0);"; done > "$W/wide.sql"
s "$(cat "$W/wide.sql")" > /dev/null
s "CREATE THING wide_t KEY id PARTITION BY id READS wide AS SELECT id, MAX(v) AS v FROM wide GROUP BY id" > /dev/null
for i in $(seq 1 60); do echo "UPDATE wide SET v = $i WHERE id = 1;"; done > "$W/wide2.sql"
before=$(grep -c "wide_t: history kept" "$W/e.log")
s "$(cat "$W/wide2.sql")" > /dev/null
cuts=$(( $(grep -c "wide_t: history kept" "$W/e.log") - before ))
r=$(s "SHOW THING wide_t AS OF '$(date '+%F %T' -d '+1 sec')' KEYS ('1', '400')" | cut -f1-2 | tr '\n' ' ')
check "a base larger than the cap is not rewritten at every change ($cuts rewrites for 60 changes) [$r]" \
  $([ "$cuts" -le 3 ] && [ "$r" = "1	60 400	0 " ] && echo 1 || echo 0)

if [ -n "$OLD" ]; then
  # a Thing made by a binary that kept no log: this one starts it at load, with the state then
  stop; rm -rf "$W/r"
  start "$OLD"
  q "CREATE DATABASE shop" > /dev/null
  s "CREATE TABLE t (id INT PRIMARY KEY, g INT, v INT); INSERT INTO t VALUES (1, 1, 10), (2, 2, 20)" > /dev/null
  s "CREATE THING old_one KEY g PARTITION BY g READS t AS SELECT g, SUM(v) AS v FROM t GROUP BY g" > /dev/null
  stop; start "$BIN"
  tick
  r=$(s "SHOW THING old_one AS OF '$(now)'" | cut -f1-2 | tr '\n' ' ')
  check "a Thing an older binary made: its log starts when this one loads it [$r]" $([ "$r" = "1	10 2	20 " ] && echo 1 || echo 0)
fi

[ "$failed" = 0 ] && echo "thing-asof: ok"
exit $failed
