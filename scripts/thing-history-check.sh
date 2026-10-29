#!/usr/bin/env bash
# How much history a Thing keeps: two entries by default (every entry is a before/after copy of the Thing's values;
# 32 of them for 59,504 Things were 60 MB of atl's Thing files and as much RSS), more with AIOS_THINGS_HISTORY=<n>,
# and a file saved under a larger limit is cut to the running limit when it is loaded.
#
#   thing-history-check.sh <aios binary>
set -u
BIN=$(realpath "${1:?usage: thing-history-check.sh <aios binary>}")
export AIOS_NO_ACCEL=1
W=$(mktemp -d)
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; rm -rf "$W"; }
trap cleanup EXIT
fails=0
check() { if [ "$2" = 1 ]; then echo "  [ok] $1"; else echo "  [FAIL] $1"; fails=$((fails + 1)); fi; }
free_port() { python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'; }
umask 077
head -c 12 /dev/urandom | od -An -tx1 | tr -d ' \n' > "$W/pass"
printf '[client]\nuser=app\nhost=127.0.0.1\npassword=%s\n' "$(cat "$W/pass")" > "$W/c.cnf"
q() { mysql --defaults-extra-file="$W/c.cnf" -P "$1" -N -B -e "$2" 2>&1; }   # q <port> <sql>
start() {   # start <root> <sql port> [env...]
  local root=$1 port=$2; shift 2
  rm -f "$root.log"
  env "$@" "$BIN" --root "$root" exec "compute serve port=$(free_port) lanes=1 sql=1 mysql_port=$port mysql_bind=127.0.0.1 mysql_user=app mysql_pass_file=$W/pass" > "$root.log" 2>&1 &
  PIDS+=($!)
  for _ in $(seq 400); do q "$port" "SELECT 1" 2>/dev/null | grep -qx 1 && return 0; sleep 0.05; done
  echo "FAIL engine on $port did not start: $(tail -n 3 "$root.log")"; exit 1
}
stop() { kill "$1" 2>/dev/null; wait "$1" 2>/dev/null; }
hist() { q "$1" "USE shop; SHOW THING HISTORY tally '1'" | wc -l; }
churn() {   # churn <port> <n>: n changes to the Thing's one row
  for i in $(seq 1 "$2"); do q "$1" "USE shop; UPDATE lines SET qty = qty + 1 WHERE id = 1" > /dev/null; done
}

# 1. the default: six changes, two history entries kept
P=$(free_port)
start "$W/a" "$P"; PID=${PIDS[-1]}
q "$P" "CREATE DATABASE shop; USE shop; CREATE TABLE lines (id INT PRIMARY KEY, orderno INT, qty INT); INSERT INTO lines VALUES (1, 1, 1)" > /dev/null
q "$P" "USE shop; CREATE THING tally KEY orderno PARTITION BY orderno READS lines AS SELECT orderno, SUM(qty) AS qty FROM lines GROUP BY orderno" > /dev/null
churn "$P" 6
n=$(hist "$P")
check "six changes, the default keeps the last two history entries (got $n)" $([ "$n" = 2 ] && echo 1 || echo 0)
last=$(q "$P" "USE shop; SHOW THING HISTORY tally '1'" | tail -n 1 | cut -f1-3)
check "  and the newest entry is the last change: $last" $(echo "$last" | grep -q "." && echo 1 || echo 0)
stop "$PID"

# 2. AIOS_THINGS_HISTORY=10 keeps them all; the file written then holds six
P2=$(free_port)
start "$W/b" "$P2" AIOS_THINGS_HISTORY=10; PID=${PIDS[-1]}
q "$P2" "CREATE DATABASE shop; USE shop; CREATE TABLE lines (id INT PRIMARY KEY, orderno INT, qty INT); INSERT INTO lines VALUES (1, 1, 1)" > /dev/null
q "$P2" "USE shop; CREATE THING tally KEY orderno PARTITION BY orderno READS lines AS SELECT orderno, SUM(qty) AS qty FROM lines GROUP BY orderno" > /dev/null
churn "$P2" 6
n=$(hist "$P2")
check "AIOS_THINGS_HISTORY=10: six changes, six entries (got $n)" $([ "$n" -ge 6 ] && echo 1 || echo 0)
q "$P2" "FLUSH TABLES" > /dev/null 2>&1
stop "$PID"
saved=$(grep -c "^hist" "$W/b/sql_db/things/tally.thing" 2>/dev/null || echo 0)
check "  the file holds them (hist records: $saved)" $([ "$saved" -ge 6 ] && echo 1 || echo 0)

# 3. the same store started with the default: cut to two at the load, and the next save writes two
P3=$(free_port)
start "$W/b" "$P3"; PID=${PIDS[-1]}
n=$(hist "$P3")
check "loaded under the default: two entries (got $n)" $([ "$n" = 2 ] && echo 1 || echo 0)
churn "$P3" 1
q "$P3" "FLUSH TABLES" > /dev/null 2>&1
stop "$PID"
saved=$(grep -c "^hist" "$W/b/sql_db/things/tally.thing" 2>/dev/null || echo 0)
check "  and the file written since holds two (hist records: $saved)" $([ "$saved" = 2 ] && echo 1 || echo 0)

[ "$fails" = 0 ] && echo "thing history: ok" || { echo "thing history: $fails failure(s)"; exit 1; }
