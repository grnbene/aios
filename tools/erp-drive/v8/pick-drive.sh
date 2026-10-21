#!/bin/bash
# pick-drive.sh ORDERNO: drive one Pending order through picking the way the ERP's own tables see it,
# and watch the picked_orders Thing do the marking. Freeze rows are written with the columns
# createFreeze writes (weberp/dispatching.php); the pick is the picking screen's own UPDATE
# (oms/orders.php). Everything goes into MariaDB -> binlog -> ASQL -> the Thing -> MariaDB.
O=$1; export MYSQL_PWD=$(cat /opt/aios/mysql.pass)
A="key=E0XQ6C2WTQ4U8GGO&userid=JACK"; B="http://127.0.0.1:18080/?route=apir/orders"
AS="mysql -S /opt/aios/run/asql.sock -uasql -D summitholding -N"
MA="mysql summitholding -N"
st() { $MA -e "SELECT orderstatusid FROM salesorders WHERE orderno = $O"; }
echo "order $O: status $(st), lines: $($MA -e "SELECT COUNT(*) FROM salesorderdetails WHERE orderno = $O AND completed = 0")"
curl -s -D /tmp/pd-h.txt -o /dev/null "$B&$A"; V=$(grep "^X-OpenOrders-Version:" /tmp/pd-h.txt | tr -d "\r" | awk '{print $2}'); echo "board version $V"
echo "== print the pick list (freeze rows as createFreeze writes them), status -> 23 Picking"
$MA -e "INSERT INTO pickingfreeze (loccode, stockid, bin, linenumber, document, ordered, qoh, pick, cost, orderno, price, purchaseorder, date_created, userid, type, picked, completed)
  SELECT so.fromstkloc, sd.stkcode, '', sd.orderlineno, $O, sd.quantity, 0, sd.quantity, 0, $O, sd.unitprice, '', NOW(), 'JACK', 10, 0, 0
  FROM salesorderdetails sd JOIN salesorders so ON so.orderno = sd.orderno WHERE sd.orderno = $O AND sd.completed = 0;
  UPDATE salesorders SET orderstatusid = 23 WHERE orderno = $O AND orderstatusid < 29;"
sleep 3
echo "Thing row (ASQL): $($AS -e "SHOW THING picked_orders" | awk -F'\t' -v o=$O '$1==o' | cut -f1-7)"
echo "== the picker picks every line (the picking screen's own UPDATE)"
for id in $($MA -e "SELECT pick_id FROM pickingfreeze WHERE orderno = $O AND completed = 0"); do
  $MA -e "update pickingfreeze set picked = pick, pickstatus = '23', userid ='JACK',  date_pickupdate = NOW() where pick_id ='$id'"
done
t=$(date +%s%N); for i in $(seq 100); do [ "$(st)" = "21" ] && break; sleep 0.1; done
echo "status now $(st) (Picked=21) after $(( ($(date +%s%N)-t)/1000000 )) ms"
sleep 2
echo "Thing row now: $($AS -e "SHOW THING picked_orders" | awk -F'\t' -v o=$O '$1==o' | cut -f1-7)  (gone = it left the pick floor)"
echo "history: "; $AS -e "SHOW THING picked_orders JSON SINCE $(($(date +%s) - 1000000000))" >/dev/null 2>&1; $AS -e "SHOW THINGS" | grep "^picked_orders" | cut -f1-8 | cut -c1-240
echo "== the board sees it"; curl -s -o /tmp/pd-ch.json "$B/changes&since=$V&$A"; php -r '$d=json_decode(file_get_contents("/tmp/pd-ch.json"),true); $e=((array)($d["entries"]??[]))["o:'$O'"]??null; echo "changes: status ".$d["status"].", entries ".count((array)($d["entries"]??[])).($e?", order '$O' now \"".$e["status"]."\" (orderstatusid ".$e["orderstatusid"].")":", order '$O' not among them")."\n";'
echo "== engine log"; tail -c 200000 /var/log/messages 2>/dev/null | grep -a "things\] picked_orders" | tail -3 | cut -c1-200; journalctl -u aios-compute --since "-5 min" --no-pager 2>/dev/null | grep -a "things\] picked_orders" | tail -3 | cut -c1-200
echo "== restore: status back, freeze rows removed"
$MA -e "UPDATE salesorders SET orderstatusid = 1 WHERE orderno = $O; DELETE FROM pickingfreeze WHERE orderno = $O AND document = $O AND userid = 'JACK';"
echo "status $(st), freeze rows $($MA -e "SELECT COUNT(*) FROM pickingfreeze WHERE orderno = $O")"
