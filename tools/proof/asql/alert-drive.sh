#!/bin/bash
# Thing alerts, driven: raise / change / clear reach a real HTTP receiver in
# order; a receiver that is down or failing holds them (durably, across kill -9)
# until it is back; a crash before checkpoint does not raise twice; a broken
# program and a failed event raise their own.
BIN=${BIN:-/repos/mcp-tools/aios/build/aios}; R=/tmp/alert-root; P=13412
export MYSQL_PWD=$(cat /tmp/gt.pass)
start() { $BIN --root $R exec "compute serve port=7412 lanes=1 sql=1 mysql_port=$P mysql_bind=127.0.0.1 mysql_user=asql mysql_pass_file=/tmp/gt.pass" >>/tmp/alert-serve.log 2>&1 & echo $! >/tmp/alert.pid
  for i in $(seq 60); do mysql -h127.0.0.1 -P$P -uasql -e "SELECT 1" >/dev/null 2>&1 && return; sleep 0.5; done; echo "no server"; }
q() { mysql -N -B -h127.0.0.1 -P$P -uasql t -e "$1" 2>&1; }
recv_up() { python3 /tmp/alert-recv.py 18099 & echo $! >/tmp/alert-recv.pid; sleep 0.5; }
recv_down() { kill $(cat /tmp/alert-recv.pid) 2>/dev/null; sleep 0.3; }
posts() { wc -l < /tmp/alert-recv.log; }
waitposts() { for i in $(seq 40); do [ "$(grep -c '^200' /tmp/alert-recv.log)" -ge "$1" ] && return; sleep 0.5; done; }
alerts() { q "SHOW THING ALERTS" | awk -F'\t' '{print "   #" $1, $3, $4, $5, "[" $6 "]", $7}'; }
pkill -f "mysql_port=$P" ; recv_down; rm -rf $R /tmp/alert-recv.log /tmp/alert-recv.mode /tmp/alert-serve.log; mkdir -p $R; touch /tmp/alert-recv.log
recv_up; start
mysql -h127.0.0.1 -P$P -uasql -e "CREATE DATABASE t"
q "CREATE TABLE items (stockid VARCHAR(10) PRIMARY KEY, qty INT)"
q "INSERT INTO items VALUES ('A', 10), ('B', 3)"
q "SET THING ALERTS TO 'http://127.0.0.1:18099/hook'"
mysql -h127.0.0.1 -P$P -uasql t < /tmp/alert-thing.sql
echo "== 1. created: B is already low"; waitposts 1; alerts
q "UPDATE items SET qty = 4 WHERE stockid = 'A'"
q "UPDATE items SET qty = 2 WHERE stockid = 'A'"
q "UPDATE items SET qty = 2 WHERE stockid = 'A'"
q "UPDATE items SET qty = 9 WHERE stockid = 'A'"
echo "== 2. A: 10 -> 4 (raise) -> 2 (change) -> 2 (nothing) -> 9 (clear)"; waitposts 4; alerts
echo "   receiver got:"; cut -c1-220 /tmp/alert-recv.log | sed 's/^/     /'
echo "== 3. receiver DOWN; B drops to 1, then kill -9 before it is back"
recv_down
q "UPDATE items SET qty = 1 WHERE stockid = 'B'"
sleep 3; alerts | head -2
kill -9 $(cat /tmp/alert.pid); sleep 1
echo "== 4. restart (redo replays B=1: must not raise it again), receiver back"
start; recv_up; waitposts 5; sleep 3; alerts | head -3
echo "   receiver now has $(grep -c '^200' /tmp/alert-recv.log) delivered posts; last: $(tail -1 /tmp/alert-recv.log | cut -c1-200)"
echo "== 5. receiver answers 500: held, retried, delivered when it answers 200"
echo fail > /tmp/alert-recv.mode
q "DELETE FROM items WHERE stockid = 'B'"
sleep 4; alerts | head -1; echo "   500s seen: $(grep -c '^500' /tmp/alert-recv.log)"
echo ok > /tmp/alert-recv.mode; waitposts 6; sleep 1; alerts | head -1
echo "== 6. the program breaks (negative stock) and recovers"
q "UPDATE items SET qty = -1 WHERE stockid = 'A'"
sleep 1; q "SELECT 1" >/dev/null
q "UPDATE items SET qty = 7 WHERE stockid = 'A'"
waitposts 8; alerts | head -2
echo "== 7. an event that cannot fire (no AIOS_THING_WRITES) and RETRY"
q "CREATE TABLE out1 (id INT PRIMARY KEY)"
q "CREATE THING ev AT '2020-01-01 00:00:00' GRAIN READS items WRITES out1 AS 'fn fire(c) { write(\"INSERT INTO out1 VALUES (1)\") return 1 }'"
sleep 3; alerts | head -1
q "RETRY THING ev" >/dev/null; sleep 3; alerts | head -2
echo "== 8. SHOW THING ALERTS low_stock only: $(q 'SHOW THING ALERTS low_stock' | wc -l) rows; total posts $(grep -c '^200' /tmp/alert-recv.log)"
echo "   console lines: $(grep -c 'ALERT' /tmp/alert-serve.log)"; grep ALERT /tmp/alert-serve.log | head -3
kill $(cat /tmp/alert.pid); recv_down
