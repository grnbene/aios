#!/usr/bin/env bash
# many.sh PORT: 40 orders invoiced by 8 workers at once, then the books checked
P=$1
t0=$(date +%s.%N)
for w in 0 1 2 3 4 5 6 7; do (for o in $(seq $((w*5+1)) $((w*5+5))); do php drive.php $P $o TAG=w$w; done) > /tmp/many.$P.$w 2>&1 & done
wait
t1=$(date +%s.%N)
C(){ ${AIOS_CONSOLE:-aios-asql-console} --no-conf -h 127.0.0.1 -P $P -u root -p secret -d summitholding -e "$1" | grep -E '^\| [0-9A-Z-]' | grep -v -E '^\| [a-z_]+ ' | tr -s ' '; }
echo "wall $(echo "$t1 - $t0" | bc) s; per invoice (ms): $(cat /tmp/many.$P.* | grep -o '([0-9]* ms' | tr -d '(ms ' | sort -n | awk '{a[NR]=$1} END{print "median", a[int(NR/2)+1], "max", a[NR]}')"
echo "failed: $(cat /tmp/many.$P.* | grep -c THREW)"
echo "invoices / distinct numbers / orders: $(C 'select count(*), count(distinct transno), count(distinct order_) from debtortrans where type = 10')"
echo "GL rows, balance: $(C 'select count(*), round(sum(amount),2) from gltrans')"
echo "stock left vs expected: $(C "select l.stockid, l.quantity, (case l.stockid when 'DRYER' then 100 else 1000 end) - d.q from locstock l join (select stkcode, sum(quantity) q from salesorderdetails group by stkcode) d on d.stkcode = l.stockid order by l.stockid" | tr "\n" " ")"
echo "stock moves / with newqoh / bin moves linked to a stock move: $(C 'select count(*), sum(newqoh is not null) from stockmoves') $(C 'select count(*) from binmoves b join stockmoves s on s.stkmoveno = b.stkmoveno and s.stockid = b.stockid')"
