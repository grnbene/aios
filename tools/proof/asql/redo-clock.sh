#!/bin/bash
# NOW() in a statement that only lives in redo.log (kill -9 before any
# checkpoint) must replay to the value it had, not the restart's time.
BIN=$1; R=/tmp/redo-clock-root; P=13412
rm -rf $R; mkdir -p $R
start() { $BIN --root $R exec "compute serve port=7412 lanes=1 sql=1 mysql_port=$P mysql_bind=127.0.0.1 mysql_user=asql mysql_pass_file=/tmp/gt.pass" >/tmp/redo-clock.log 2>&1 & echo $! > /tmp/redo-clock.pid
  for i in $(seq 60); do MYSQL_PWD=$(cat /tmp/gt.pass) mysql -h127.0.0.1 -P$P -uasql -e "SELECT 1" >/dev/null 2>&1 && return; sleep 0.5; done; echo "no server"; }
q() { MYSQL_PWD=$(cat /tmp/gt.pass) mysql -N -B -h127.0.0.1 -P$P -uasql "$@"; }
start
q -e "CREATE DATABASE t" 2>/dev/null
q t -e "CREATE TABLE c (id INT PRIMARY KEY, at DATETIME, d DATE, ts TIMESTAMP DEFAULT CURRENT_TIMESTAMP)"
q t -e "INSERT INTO c (id, at, d) VALUES (1, NOW() + INTERVAL 25 SECOND, CURDATE())"
before=$(q t -e "SELECT id, at, d, ts FROM c")
kill -9 $(cat /tmp/redo-clock.pid); sleep 7
start
after=$(q t -e "SELECT id, at, d, ts FROM c")
echo "before: $before"; echo "after:  $after"
[ "$before" = "$after" ] && echo SAME || echo CHANGED
kill $(cat /tmp/redo-clock.pid)
