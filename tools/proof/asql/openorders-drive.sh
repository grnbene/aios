#!/bin/bash
# Drives the open_orders_view Thing (things/open_orders_view.*) on a throwaway
# ASQL with summitholding's real column sets: which orders it lists, and which
# orders' versions move when each kind of row changes.
here=$(cd "$(dirname "$0")" && pwd)
BIN=${BIN:-$here/../../../build/aios}
R=/tmp/oov-root; P=13415; PASS=/tmp/oov.pass
echo pw > $PASS; export MYSQL_PWD=pw
start() {
  $BIN --root $R exec "compute serve port=7415 lanes=1 sql=1 mysql_port=$P mysql_bind=127.0.0.1 mysql_user=asql mysql_pass_file=$PASS" >>/tmp/oov.log 2>&1 &
  echo $! > /tmp/oov.pid
  until mysql -h127.0.0.1 -P$P -uasql -e "SELECT 1" >/dev/null 2>&1; do sleep 0.3; done
}
stop() { kill "$(cat /tmp/oov.pid)"; while mysql -h127.0.0.1 -P$P -uasql -e "SELECT 1" >/dev/null 2>&1; do sleep 0.2; done; }
q() { mysql -N -B -h127.0.0.1 -P$P -uasql summitholding -e "$1" 2>&1; }
# orderno:version[open/inv] for every Thing, sorted
v() {
  mysql -B -h127.0.0.1 -P$P -uasql summitholding -e "SHOW THINGS open_orders_view" 2>&1 | awk -F'\t' '
    NR == 1 { for (i = 1; i <= NF; i++) c[$i] = i; next }
    { w = $c["wake_at"]; gsub(/ /, "T", w)
      printf "%s:v%s[%s%s%s] ", $c["orderno"], $c["version"], ($c["open"] == "1" ? "o" : ""), ($c["inv"] == "1" ? "i" : ""), (w != "NULL" && w != "" ? ",wake@" w : "") }
    END { print "" }' | tr ' ' '\n' | sort | tr '\n' ' '
  echo
}
# what each Thing says, without its version (a restart replays notices, so versions may coalesce)
vals() {
  mysql -B -h127.0.0.1 -P$P -uasql summitholding -e "SHOW THINGS open_orders_view" 2>&1 | awk -F'\t' '
    NR == 1 { for (i = 1; i <= NF; i++) c[$i] = i; next }
    { print $c["orderno"] ":" $c["open"] $c["inv"] ":" $c["h"] ":" $c["g"] }' | sort | tr '\n' ' '
}
settle() { sleep 0.6; }
step() { echo "== $1"; }

kill "$(cat /tmp/oov.pid 2>/dev/null)" 2>/dev/null; sleep 0.5; rm -rf $R /tmp/oov.log; mkdir -p $R
start
mysql -h127.0.0.1 -P$P -uasql -e "CREATE DATABASE summitholding"
while IFS= read -r ddl; do [ -n "$ddl" ] && q "$ddl"; done < "$here/openorders-schema.sql"

q "INSERT INTO config VALUES ('OpenOrdersExcludedStatuses', '29,30,40,90,93', 'text'), ('DefaultDateFormat', 'd/m/Y', 'text')"
q "INSERT INTO setting (setting_id, store_id, code, \`key\`, value, serialized) VALUES (1, 0, 'config', 'config_name', 'summit', 0)"
q "INSERT INTO order_status (order_status_id, language_id, name, isdispatch, badge_bg, badge_fg) VALUES (1, 1, 'Open', 0, '#fff', '#000'), (50, 1, 'Ready', 0, '#0f0', '#000'), (29, 1, 'Hold', 0, '#f00', '#fff')"
q "INSERT INTO shippers (shipper_id, shippername, mincharge, autoprint, thirdpartpickup) VALUES (1, 'UPS', 0, 0, 0)"
q "INSERT INTO locations (loccode, locationname, taxprovinceid, taxgroupid) VALUES ('00', 'Main', 1, 1), ('01', 'East', 1, 1)"
q "INSERT INTO taxgroups VALUES (1, 'Default')"
q "INSERT INTO taxauthorities (taxid, description) VALUES (1, 'NY')"
q "INSERT INTO taxauthrates VALUES (1, 1, 1, 0.08)"
q "INSERT INTO taxgrouptaxes VALUES (1, 1, 1, 0)"
q "INSERT INTO debtorsmaster (debtorno, name, paymentterms, lastpaid) VALUES ('C1', 'Acme', '30', 0), ('C2', 'Bolt', 'COD', 0)"
q "INSERT INTO custbranch (branchcode, debtorno, brname, taxgroupid) VALUES ('B1', 'C1', 'Acme Main', 1), ('B2', 'C2', 'Bolt Main', 1)"
q "INSERT INTO stockmaster (stockid, description, taxcatid) VALUES ('A', 'Tire A', 1), ('B', 'Tire B', 1)"
q "INSERT INTO locstock (loccode, stockid, quantity) VALUES ('00', 'A', 10), ('01', 'A', 5), ('00', 'B', 3)"
q "INSERT INTO salesorders (orderno, debtorno, branchcode, fromstkloc, orderstatusid, salesperson, takenby, placedfrom, date_created, confirmeddate, deliverydate, comments) VALUES
   (1001, 'C1', 'B1', '00', 1, 'S1', 'JACK', 1, '2026-09-20 10:00:00', '2026-09-20', '2026-09-30', ''),
   (1002, 'C2', 'B2', '00', 50, '', 'JACK', 3, '2026-09-21 11:00:00', '2026-09-21', '2026-09-22', ''),
   (1003, 'C1', 'B1', '00', 1, 'S1', 'JACK', 1, '2026-09-19 09:00:00', '2026-09-19', '2026-09-19', ''),
   (1004, 'C1', 'B1', '00', 29, 'S1', 'JACK', 1, '2026-09-22 09:00:00', '2026-09-22', '2026-09-29', ''),
   (1005, 'C2', 'B2', '00', 1, 'S2', 'JACK', 1, '2026-09-23 09:00:00', '2026-09-23', '2026-09-29', ''),
   (1006, 'C1', 'B1', '01', 1, 'S1', 'JACK', 1, '2026-09-24 09:00:00', '2026-09-24', '2026-10-01', '')"
q "INSERT INTO salesorderdetails (orderlineno, orderno, stkcode, quantity, qtyinvoiced, unitprice, completed) VALUES
   (1, 1001, 'A', 2, 0, 100, 0), (2, 1001, 'B', 1, 0, 50, 0), (1, 1002, 'A', 1, 1, 100, 1), (1, 1003, 'B', 1, 1, 50, 1),
   (1, 1004, 'A', 1, 0, 100, 0), (1, 1005, 'Z', 1, 0, 10, 0), (1, 1006, 'B', 4, 0, 50, 0)"
q "INSERT INTO dispatch (dispatch_id, invoice, cleared, oclater) VALUES (1, '1002', 0, 0), (2, '1003', 1, 0)"
q "INSERT INTO debtortrans (id, transno, type, order_, debtorno, branchcode) VALUES (1, 500, 10, 1002, 'C2', 'B2')"
q "INSERT INTO stockmoves (stkmoveno, stockid, type, transno, orderlineref, qty, loccode) VALUES (1, 'A', 10, 500, 1, -1, '00')"

step "create (expect 1001 open, 1002 invoiced-on-dispatch, 1006 open; not 1003 done, 1004 on hold, 1005 unknown item)"
t0=$(date +%s)
"$here/../../../things/open_orders_view.sh" | mysql -h127.0.0.1 -P$P -uasql summitholding 2>&1 | tail -3
echo "   $(( $(date +%s) - t0 ))s"; v

step "salesorders 1001 comment edited -> 1001 only"; q "UPDATE salesorders SET comments = 'call first' WHERE orderno = 1001"; settle; v
step "debtorsmaster C2 lastpaid -> 1002 only"; q "UPDATE debtorsmaster SET lastpaid = 9 WHERE debtorno = 'C2'"; settle; v
step "stockmaster B description -> 1001, 1006"; q "UPDATE stockmaster SET description = 'Tire B v2' WHERE stockid = 'B'"; settle; v
step "stockmoves type 10 on transno 500 -> 1002"; q "INSERT INTO stockmoves (stkmoveno, stockid, type, transno, orderlineref, qty, loccode) VALUES (2, 'A', 10, 500, 1, -1, '00')"; settle; v
step "stockmoves type 25 (a receipt) -> nothing"; q "INSERT INTO stockmoves (stkmoveno, stockid, type, transno, orderlineref, qty, loccode) VALUES (3, 'A', 25, 900, 0, 5, '00')"; settle; v
step "locstock A, no picks yet -> nothing"; q "UPDATE locstock SET quantity = 7 WHERE loccode = '00' AND stockid = 'A'"; settle; v
step "pickingfreeze row for 1001 line 1 -> 1001"; q "INSERT INTO pickingfreeze (pick_id, stockid, loccode, linenumber, orderno, pick, picked) VALUES (1, 'A', '00', 1, 1001, 1, 0)"; settle; v
step "locstock A again, now 1001 picks A -> 1001"; q "UPDATE locstock SET quantity = 6 WHERE loccode = '00' AND stockid = 'A'"; settle; v
step "open PO line for A -> 1001, 1002 (1004 is not a Thing)"; q "INSERT INTO purchorderdetails (podetailitem, orderno, itemcode, completed, quantityord, quantityrecd) VALUES (77, 9000, 'A', 0, 5, 0)"; settle; v
step "PO line received in full -> 1001, 1002"; q "UPDATE purchorderdetails SET quantityrecd = 5 WHERE podetailitem = 77"; settle; v
step "1004 taken off hold -> 1004 appears"; q "UPDATE salesorders SET orderstatusid = 1 WHERE orderno = 1004"; settle; v
step "1006's only line completed -> 1006 goes"; q "UPDATE salesorderdetails SET completed = 1 WHERE orderno = 1006"; settle; v
step "1002 snoozed for 3s -> wake_at set"; q "INSERT INTO oms_snoozed (orderno, date_added, date_end, initials, autoid) VALUES ('1002', NOW(), NOW() + INTERVAL 3 SECOND, 'JK', 1)"; settle; v
step "... 5s later the snooze has ended -> 1002 re-read, wake_at cleared"; sleep 5; q "SELECT 1" >/dev/null; settle; v
step "status 50 excluded by config -> 1002 goes, every other order's g moves"; q "UPDATE config SET confvalue = '29,30,40,50,90,93' WHERE confname = 'OpenOrdersExcludedStatuses'"; settle; v
step "shippers edited -> every order's g moves"; q "UPDATE shippers SET mincharge = 1 WHERE shipper_id = 1"; settle; v
step "restart -> same Things, same digests"; before=$(vals); stop; start; after=$(vals); echo "   $after"; [ -n "$before" ] && [ "$before" = "$after" ] && echo "   identical after restart" || echo "   DIFFERENT after restart: was $before"
step "rebuilt with the bulk path forced (SMALL = 0) -> same digests as the per-order path"
before=$(vals); q "DROP THING open_orders_view" >/dev/null
"$here/../../../things/open_orders_view.sh" | sed 's/let SMALL = 250/let SMALL = 0/' | mysql -h127.0.0.1 -P$P -uasql summitholding 2>&1 | tail -3
after=$(vals); [ -n "$before" ] && [ "$before" = "$after" ] && echo "   identical" || { echo "   DIFFERENT"; echo "   per-order: $before"; echo "   bulk:      $after"; }
step "SHOW THINGS summary"; q "SHOW THINGS" | cut -f1-6
stop
