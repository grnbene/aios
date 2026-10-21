#!/bin/bash
# st-thing-trial.sh: a scratch ASQL (the new engine, /tmp/aios-cand) over a fresh copy of the live
# root, the open_orders Thing redeclared with its parts, then the board harness: the new consumer
# (THING_FILE) against the board's usual path, entries diffed.
set -u
BIN=/tmp/aios-cand; ROOT=/tmp/th-root; SOCK=/tmp/th.sock
pid=$(ss -ltnp 2>/dev/null | grep ":14901 " | grep -o "pid=[0-9]*" | cut -d= -f2 | head -1); [ -n "$pid" ] && kill $pid; sleep 2
rm -rf $ROOT; cp -a /opt/aios/root $ROOT
cd /tmp; (setsid nohup $BIN --root $ROOT exec "compute serve port=7901 lanes=8 sql=1 mysql_port=14901 mysql_bind=127.0.0.1 mysql_user=asql mysql_pass_file=/opt/aios/mysql.pass mysql_socket=$SOCK" > /tmp/th.log 2>&1 &)
for i in $(seq 240); do grep -aq "asql: warmed" /tmp/th.log && break; sleep 1; done; for i in $(seq 60); do [ -S $SOCK ] && break; sleep 1; done; sleep 2
grep -a "warmed\|\[things\]" /tmp/th.log | head -3
export MYSQL_PWD=$(cat /opt/aios/mysql.pass)
M="mysql -S $SOCK -uasql -D summitholding"
$M -e "DROP THING open_orders" 2>&1 | head -1
t=$(date +%s%N); $M < /root/oo/open_orders_parts.sql 2>&1 | head -2; echo "create: $(( ($(date +%s%N)-t)/1000000 )) ms"
$M -N -e "SHOW THINGS" | cut -f1,2 | tr "\t" "=" | tr "\n" " "; echo
php -r '$pw=trim(file_get_contents("/opt/aios/mysql.pass")); $c=new mysqli("localhost","asql",$pw,"summitholding",null,"/tmp/th.sock"); $t=microtime(true); $r=$c->query("SHOW THING open_orders JSON"); $rows=$r->fetch_all(MYSQLI_NUM); $bytes=0; $bad=0; $parts=array(); foreach($rows as $x){ $bytes+=strlen($x[3]); $d=json_decode($x[3],true); if(!is_array($d)){$bad++;continue;} foreach($d as $k=>$v) if(is_array($v)) $parts[$k]=($parts[$k]??0)+count($v);} printf("JSON read: %d rows, %d KB, %.0f ms, bad %d, parts %s\n", count($rows), $bytes/1024, (microtime(true)-$t)*1000, $bad, json_encode($parts));'
echo "== board harness: new consumer (Things) vs the usual path"
rm -rf /tmp/board-cache; mkdir -p /tmp/board-cache
cd /root && AIOS_ASQL_PRIMARY=1 BOARD_SOCK=$SOCK THING_FILE=/tmp/openorders_thing.v2.php php board.php 2>&1 | grep -v "^PHP" | head -40
