#!/bin/bash
BIN=$1; R=/tmp/redo-clock-root; P=13412; rm -rf $R; mkdir $R
export MYSQL_PWD=$(cat /tmp/gt.pass)
start() { $BIN --root $R exec "compute serve port=7412 lanes=1 sql=1 mysql_port=$P mysql_bind=127.0.0.1 mysql_user=asql mysql_pass_file=/tmp/gt.pass" >/tmp/redo-clock.log 2>&1 & echo $! >/tmp/redo-clock.pid; sleep 4; }
start
printf 'CREATE DATABASE t;\nCREATE TABLE t.c (id INT PRIMARY KEY, at DATETIME);\nSELECT NOW();\n\\! sleep 3\nINSERT INTO t.c VALUES (1, NOW());\nSELECT "stale probe", at FROM t.c WHERE id=1;\nSELECT NOW();\nBEGIN;\nINSERT INTO t.c VALUES (2, NOW());\n\\! sleep 3\nINSERT INTO t.c VALUES (3, NOW());\nCOMMIT;\n' | mysql -N -h127.0.0.1 -P$P -uasql
before=$(mysql -N -B -h127.0.0.1 -P$P -uasql -e "SELECT id, at FROM t.c ORDER BY id")
kill -9 $(cat /tmp/redo-clock.pid); sleep 3; start
after=$(mysql -N -B -h127.0.0.1 -P$P -uasql -e "SELECT id, at FROM t.c ORDER BY id")
echo "before:"; echo "$before"; echo "after:"; echo "$after"; [ "$before" = "$after" ] && echo SAME || echo CHANGED
kill $(cat /tmp/redo-clock.pid)
