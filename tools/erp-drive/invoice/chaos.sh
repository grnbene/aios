#!/usr/bin/env bash
# chaos.sh PORT: 40 invoices by 8 workers, every 5th failing part way (at a different statement each time), while a
# POS worker sells SHAMPOO and DRYER from its own connections the whole time. Every sale must survive every rollback.
P=$1
fails=("insert into stockmoves" "insert into gltrans" "UPDATE locstock" "insert into binmoves" "update salesorders set orderstatusid")
for w in 0 1 2 3 4 5 6 7; do (for o in $(seq $((w*5+1)) $((w*5+5))); do
  if [ $((o % 5)) = 0 ]; then php drive.php $P $o "FAIL_AT=${fails[$((o / 5 % 5))]}" TAG=w$w; else php drive.php $P $o TAG=w$w; fi
done) > /tmp/chaos.$P.$w 2>&1 & done
php -r '$m = new mysqli("127.0.0.1", "root", "secret", "summitholding", (int)$argv[1]); $n = 0;
  for ($i = 0; $i < 300; $i++) { $m->query("update locstock set quantity = quantity - 1 where stockid = \"" . ($i % 2 ? "DRYER" : "SHAMPOO") . "\" and bin <> \"\""); $m->query("insert into gltrans (type, typeno, account, amount) values (12, 5000 + $i, \"POS\", 1)"); $n++; usleep(2000); }
  echo "POS made $n sales\n";' $P &
wait
C(){ ${AIOS_CONSOLE:-aios-asql-console} --no-conf -h 127.0.0.1 -P $P -u root -p secret -d summitholding -e "$1" | grep -E '^\| [0-9A-Z-]' | tr -s ' '; }
echo "invoices posted: $(cat /tmp/chaos.$P.* | grep -c '"success":true'), failed: $(cat /tmp/chaos.$P.* | grep -c '"success":false')"
echo "invoices in the books / orders: $(C 'select count(*), count(distinct order_) from debtortrans where type = 10')"
echo "invoice GL balance: $(C 'select round(sum(amount),2) from gltrans where type = 10') POS gltrans rows (want 300): $(C 'select count(*) from gltrans where type = 12')"
echo "stock vs (start - invoiced lines of posted invoices - POS sales):"
C "select l.stockid, l.quantity, (case l.stockid when 'DRYER' then 100 else 1000 end) - coalesce(d.q, 0) - (case l.stockid when 'SHAMPOO' then 150 when 'DRYER' then 150 else 0 end) from locstock l left join (select d.stkcode, sum(d.quantity) q from salesorderdetails d join (select distinct order_ from debtortrans where type = 10) i on i.order_ = d.orderno group by d.stkcode) d on d.stkcode = l.stockid order by l.stockid"
echo "half-posted invoices (picks invoiced without an invoice): $(C 'select count(*) from pickingfreeze p where p.invoiced = 1 and p.orderno not in (select order_ from debtortrans where type = 10)')"
