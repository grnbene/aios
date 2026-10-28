#!/usr/bin/env bash
# A Thing reacts only to what was committed. While a transaction is open its rows are not in any Thing - for
# another session or its own; COMMIT brings them in, ROLLBACK leaves no trace (no value, no history line, no act,
# nothing in the AS OF log). Savepoints, a failed statement inside a transaction, a transaction past the held-row
# cap, and plain autocommit writes too. Two sessions at once, from a grain program.
#
#   thing-txn-check.sh <aios binary>
set -u
BIN=$(realpath "${1:?usage: thing-txn-check.sh <aios binary>}")
export AIOS_NO_ACCEL=1
W=$(mktemp -d)
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; rm -rf "$W"; }
trap cleanup EXIT
free_port() { python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'; }
umask 077
head -c 12 /dev/urandom | od -An -tx1 | tr -d ' \n' > "$W/pass"
head -c 12 /dev/urandom | od -An -tx1 | tr -d ' \n' > "$W/token"
printf '[client]\nuser=app\nhost=127.0.0.1\npassword=%s\n' "$(cat "$W/pass")" > "$W/c.cnf"
P=$(free_port)
mkdir -p "$W/r/sql_db"
# a profile the shadow act names (shadow never connects)
printf '[profile]\nid=look\nhost=127.0.0.1\nport=1\nuser=x\ndatabase=x\n' > "$W/r/sql_db/connections.ini"
AIOS_THING_HOLD_ROWS=50 "$BIN" --root "$W/r" exec "compute serve port=$(free_port) lanes=1 sql=1 mysql_port=$P mysql_bind=127.0.0.1 mysql_user=app mysql_pass_file=$W/pass token_file=$W/token" > "$W/e.log" 2>&1 < /dev/null & PIDS+=($!)
for _ in $(seq 400); do mysql --defaults-extra-file="$W/c.cnf" -P "$P" -N -B -e "SELECT 1" 2>/dev/null | grep -qx 1 && break; sleep 0.05; done

cat > "$W/t.gr" <<EOF
let CFG = {"host": "127.0.0.1", "port": $P, "user": "app", "password": trim(read_file("$W/pass"))}
let failed = 0
fn check(what, ok) {
  print((ok ? "ok   " : "FAIL ") + what)
  if not ok { failed += 1 }
}
fn q(c, sql) {
  let r = db_query(c, sql)
  if not r.ok { error(sql + ": " + r.error) }
  return r
}
// one Thing value: v of key 1 (nil when there is none)
fn v_of(c) {
  let r = q(c, "SHOW THING tg")
  let row = r.rows == nil ? nil : find_if(r.rows, fn(x) { return x[0] == "1" })
  return row == nil ? nil : row[1]
}
fn n_lines(c, sql) { let r = q(c, sql) return r.rows == nil ? 0 : len(r.rows) }

let a = db_connect(CFG)
let b = db_connect(CFG)
if not a.ok or not b.ok { error("connect") }
for s in ["CREATE DATABASE s", "USE s", "CREATE TABLE t (id INT PRIMARY KEY, g INT, v INT)",
          "INSERT INTO t VALUES (1, 1, 10)",
          "CREATE THING tg KEY g PARTITION BY g READS t AS SELECT g, SUM(v) AS v FROM t GROUP BY g WHEN v > 50 DO UPDATE x SET y = KEY TO look SHADOW"] {
  q(a, s)
}
q(b, "USE s")
let hist0 = n_lines(a, "SHOW THING HISTORY tg '1'")
let asof0 = len(read_file("$W/r/sql_db/things/tg.asof"))

// an open transaction is in no Thing
q(a, "START TRANSACTION")
q(a, "UPDATE t SET v = 100 WHERE id = 1")
check("while it is open, another session sees the committed value (" + str(v_of(b)) + ")", v_of(b) == "10")
check("  and so does its own session (" + str(v_of(a)) + ")", v_of(a) == "10")
// rolled back: no trace
q(a, "ROLLBACK")
check("after ROLLBACK the value is as it was (" + str(v_of(b)) + ")", v_of(b) == "10")
check("  no history line for it", n_lines(a, "SHOW THING HISTORY tg '1'") == hist0)
check("  no act was worked out for it", n_lines(a, "SHOW THING SHADOW tg") == 0)
check("  nothing went into the AS OF log", len(read_file("$W/r/sql_db/things/tg.asof")) == asof0)

// committed: in the Thing, and its act
q(a, "START TRANSACTION")
q(a, "UPDATE t SET v = 100 WHERE id = 1")
q(a, "INSERT INTO t VALUES (2, 1, 5)")
q(a, "COMMIT")
check("after COMMIT both rows are in it (" + str(v_of(b)) + ")", v_of(b) == "105")
check("  and its act is worked out once", n_lines(a, "SHOW THING SHADOW tg") == 1)

// a savepoint rolled back inside a committed transaction: only what was kept
q(a, "START TRANSACTION")
q(a, "UPDATE t SET v = 1 WHERE id = 2")
q(a, "SAVEPOINT sp")
q(a, "UPDATE t SET v = 999 WHERE id = 1")
q(a, "ROLLBACK TO SAVEPOINT sp")
q(a, "COMMIT")
check("a savepoint rolled back: the Thing has what was committed (" + str(v_of(b)) + ")", v_of(b) == "101")

// a statement that fails inside a transaction (a duplicate key) and the transaction committed after it
q(a, "START TRANSACTION")
q(a, "UPDATE t SET v = 20 WHERE id = 2")
let dup = db_query(a, "INSERT INTO t VALUES (1, 1, 7)")
check("a duplicate key fails inside the transaction", not dup.ok)
q(a, "COMMIT")
check("  the Thing has the committed rows (" + str(v_of(b)) + ")", v_of(b) == "120")

// past the held-row cap (50 here): the table is re-read whole at COMMIT
q(a, "START TRANSACTION")
for i in range(3, 103) { q(a, "INSERT INTO t VALUES (" + str(i) + ", 1, 1)") }
check("a 100-row transaction is still not in the Thing while open (" + str(v_of(b)) + ")", v_of(b) == "120")
q(a, "COMMIT")
check("  and is whole after COMMIT (" + str(v_of(b)) + ")", v_of(b) == "220")

// autocommit writes are in at once, as before
q(a, "UPDATE t SET v = 0 WHERE id > 2")
check("an autocommit write is in the Thing at once (" + str(v_of(b)) + ")", v_of(b) == "120")

if failed == 0 { print("thing-txn: ok") }
EOF
out=$("$BIN" --root "$W/cli" exec "grain run $W/t.gr" 2>&1)
echo "$out" | grep -vE '^\s*$'
echo "$out" | grep -q "^thing-txn: ok"
