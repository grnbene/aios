#!/bin/bash
BIN=/repos/mcp-tools/aios/build/aios; R=/tmp/alert-root2; P=13413
export MYSQL_PWD=$(cat /tmp/gt.pass)
pkill -f "mysql_port=$P"; rm -rf $R; mkdir $R
AIOS_THING_WRITES=1 $BIN --root $R exec "compute serve port=7413 lanes=1 sql=1 mysql_port=$P mysql_bind=127.0.0.1 mysql_user=asql mysql_pass_file=/tmp/gt.pass" >/tmp/alert-serve2.log 2>&1 & pid=$!
for i in $(seq 60); do mysql -h127.0.0.1 -P$P -uasql -e "SELECT 1" >/dev/null 2>&1 && break; sleep 0.5; done
q() { mysql -N -B -h127.0.0.1 -P$P -uasql t -e "$1" 2>&1; }
alerts() { q "SHOW THING ALERTS" | awk -F'\t' '{print "   #" $1, $3, $5, "[" $6 "]", $7}'; }
mysql -h127.0.0.1 -P$P -uasql -e "CREATE DATABASE t"
q "CREATE TABLE src (id INT PRIMARY KEY)"; q "CREATE TABLE out1 (id INT PRIMARY KEY)"; q "CREATE TABLE other (id INT PRIMARY KEY)"
q "CREATE THING bad AT '2020-01-01 00:00:00' GRAIN READS src WRITES out1 AS 'fn fire(c) { write(\"INSERT INTO other VALUES (1)\") return 1 }'"
sleep 3; echo "== fire() writes outside WRITES:"; alerts
q "RETRY THING bad"; sleep 3; echo "== after RETRY (fails again):"; alerts
q "CREATE THING good AT '2020-01-01 00:00:00' GRAIN READS src WRITES out1 AS 'fn fire(c) { return write(\"INSERT INTO out1 VALUES (1)\") } fn verify(c, f) { return {ok: len(read(\"SELECT id FROM out1\")) == 1} }'"
sleep 3; echo "== a good event:"; alerts | head -1
q "DROP THING bad"; echo "== dropped bad:"; alerts | head -1
kill $pid
