#!/bin/bash
BIN=/repos/mcp-tools/aios/build/aios; R=/tmp/ready-root; P=13417
export MYSQL_PWD=$(cat /tmp/gt.pass)
start() { $BIN --root $R exec "compute serve port=7417 lanes=1 sql=1 mysql_port=$P mysql_bind=127.0.0.1 mysql_user=asql mysql_pass_file=/tmp/gt.pass" >>/tmp/ready.log 2>&1 & echo $! > /tmp/ready.pid; until mysql -h127.0.0.1 -P$P -uasql -e "SELECT 1" >/dev/null 2>&1; do sleep 0.3; done; }
stop() { kill $(cat /tmp/ready.pid); while ss -ltn | grep -q ":$P "; do sleep 0.2; done; }
for p in $(ss -ltnp 2>/dev/null | grep ":$P " | grep -o "pid=[0-9]*" | cut -d= -f2); do kill $p; done; sleep 1; rm -rf $R /tmp/ready.log; mkdir -p $R
start
q() { mysql -N -B -h127.0.0.1 -P$P -uasql erp -e "$1" 2>&1; }
mysql -h127.0.0.1 -P$P -uasql -e "CREATE DATABASE erp"
q "CREATE TABLE locstock (loccode VARCHAR(5), stockid VARCHAR(10), quantity DECIMAL(10,2), PRIMARY KEY (loccode, stockid))"
q "CREATE TABLE salesorders (orderno INT PRIMARY KEY, orddate DATE, fromstkloc VARCHAR(5), quotation INT)"
q "CREATE TABLE salesorderdetails (orderno INT, orderlineno INT, stkcode VARCHAR(10), quantity DECIMAL(10,2), qtyinvoiced DECIMAL(10,2), completed INT, PRIMARY KEY (orderno, orderlineno))"
q "INSERT INTO locstock VALUES ('W','A',5),('W','Z',0),('W','B',10),('W','C',4),('W','D',4)"
q "INSERT INTO salesorders VALUES (1,'2026-09-01','W',0),(2,'2026-09-02','W',0),(3,'2026-09-03','W',0),(4,'2026-09-04','W',0),(5,'2026-09-05','W',0),(6,'2026-09-06','W',0),(7,'2026-09-07','W',0),(8,'2026-09-08','W',0),(9,'2026-09-09','W',0)"
q "INSERT INTO salesorderdetails VALUES (1,1,'A',5,0,0),(1,2,'Z',1,0,0),(2,1,'A',3,0,0),(3,1,'A',2,0,0),(4,1,'B',6,0,0),(5,1,'B',5,0,0),(6,1,'B',5,0,0),(7,1,'C',4,0,0),(7,2,'D',4,0,0),(8,1,'C',4,0,0),(9,1,'D',4,0,0)"
DECL="READS salesorderdetails, salesorders AS SELECT d.orderno, d.orderlineno, so.fromstkloc, d.stkcode, d.quantity - d.qtyinvoiced FROM salesorderdetails d JOIN salesorders so ON so.orderno = d.orderno WHERE d.completed = 0 AND so.quotation = 0 AND d.quantity > d.qtyinvoiced"
q "CREATE THING stock RESOURCE KEY loccode, stockid CAPACITY quantity FROM locstock READS locstock" | tail -2
q "CREATE THING fifo_order CLAIMS stock KEY orderno PRIORITY so.orddate, d.orderno, d.orderlineno $DECL" | tail -3
echo "== first come, first served:"; q "SHOW THINGS fifo_order" | cut -f1,2 | tr '\n' ' '; echo
q "DROP THING fifo_order" >/dev/null
q "CREATE THING open_order CLAIMS stock KEY orderno PRIORITY so.orddate, d.orderno, d.orderlineno ALLOCATE READY $DECL" | tail -3
echo "== ALLOCATE READY:"; q "SHOW THINGS open_order" | cut -f1,2 | tr '\n' ' '; echo
q "SHOW THING ALLOCATION open_order"
echo "== lines (order line loc item need allocated pos queue):"; q "SHOW THING LINES open_order" | tr '\t' ' '
echo "== 1 more A arrives (6): order 1 still blocked by Z; nothing changes"; q "UPDATE locstock SET quantity = 6 WHERE stockid = 'A'"; q "SHOW THINGS open_order" | cut -f1,2 | tr '\n' ' '; echo
echo "== Z arrives (1): order 1 can now be whole - 1 + (2 or 3)? cap A=6: 1(5) + ... "; q "UPDATE locstock SET quantity = 1 WHERE stockid = 'Z'"; q "SHOW THINGS open_order" | cut -f1,2 | tr '\n' ' '; echo; q "SHOW THING ALLOCATION open_order" | cut -f3-7
echo "== order 6 invoiced (gone): B back to 4 & 5?"; q "UPDATE salesorderdetails SET completed = 1 WHERE orderno = 6"; q "SHOW THINGS open_order" | cut -f1,2 | tr '\n' ' '; echo
q "SHOW THING HISTORY open_order '2'" | cut -f1-5 | head -5
echo "== restart: same plan"; before=$(q "SHOW THING LINES open_order" | md5sum); stop; start; after=$(q "SHOW THING LINES open_order" | md5sum); [ "$before" = "$after" ] && echo "identical after restart" || echo "DIFFERENT after restart"
q "SHOW THING ALLOCATION open_order" | cut -f3-7
stop
