<?php
/**
 * Seed the moov_sandbox board for a scale comparison — SANDBOX ONLY.
 *
 *   php sandbox-seed.php seed [n=999] [truck=27] [ontruck=12]   clone the newest open order n times
 *                                                                 onto distinct geocoded branches, and put
 *                                                                 `ontruck` of them on a truck column
 *   php sandbox-seed.php clean                                    remove everything this script created
 *
 * Every row it writes carries customerref 'AIOS-SEED' (salesorders) or userid
 * 'AIOS-SEED' (oms), so `clean` removes exactly those and nothing else.
 * Refuses to run against any database but moov_sandbox.
 */
include __DIR__ . "/config.php";
$DB = "moov_sandbox";
$m = new mysqli(DB_HOSTNAME, DB_USERNAME, DB_PASSWORD, $DB);
if ($m->connect_errno) { fwrite(STDERR, "connect failed\n"); exit(1); }
if ($m->query("select database() d")->fetch_assoc()["d"] !== $DB) { fwrite(STDERR, "not the sandbox\n"); exit(1); }

$cmd = $argv[1] ?? 'seed';
if ($cmd === 'clean') {
	$m->query("delete from oms where userid='AIOS-SEED'");
	$o = $m->affected_rows;
	$m->query("delete sd from salesorderdetails sd join salesorders so on so.orderno=sd.orderno where so.customerref='AIOS-SEED'");
	$d = $m->affected_rows;
	$m->query("delete from salesorders where customerref='AIOS-SEED'");
	$s = $m->affected_rows;
	echo "cleaned: $s orders, $d lines, $o oms rows\n";
	exit(0);
}

$n = (int) ($argv[2] ?? 999); $truck = (string) ($argv[3] ?? '27'); $ontruck = (int) ($argv[4] ?? 12);
$existing = (int) $m->query("select count(*) c from salesorders where customerref='AIOS-SEED'")->fetch_assoc()["c"];
if ($existing) { echo "already seeded ($existing orders) — run clean first\n"; exit(1); }

$tpl = $m->query("select so.* from salesorders so join salesorderdetails sd on sd.orderno=so.orderno where so.quotation=0 and sd.completed=0 and sd.quantity>sd.qtyinvoiced and cast(so.orderno as char) not in (select invoice from dispatch) order by so.orderno desc limit 1")->fetch_assoc();
if (!$tpl) { fwrite(STDERR, "no template open order\n"); exit(1); }
$lines = []; $r = $m->query("select * from salesorderdetails where orderno='" . $m->real_escape_string($tpl['orderno']) . "'");
while ($x = $r->fetch_assoc()) $lines[] = $x;
echo "template order {$tpl['orderno']} ({$tpl['debtorno']}.{$tpl['branchcode']}), ", count($lines), " line(s)\n";

$branches = []; $r = $m->query("select debtorno, branchcode, brname, braddress1, braddress2, braddress3, braddress4, braddress5, lat, lng from custbranch where lat<>0 and lat is not null and lng<>0 order by rand(20260911)");
while ($x = $r->fetch_assoc()) $branches[] = $x;
echo count($branches), " geocoded branches\n";

$next = (int) $m->query("select max(orderno) mx from salesorders")->fetch_assoc()["mx"] + 1;
$m->begin_transaction();
$created = [];
for ($i = 0; $i < $n; $i++) {
	$b = $branches[$i % count($branches)];
	$row = $tpl;
	$row['orderno'] = $next + $i;
	$row['debtorno'] = $b['debtorno']; $row['branchcode'] = $b['branchcode'];
	$row['customerref'] = 'AIOS-SEED';
	$row['deliverto'] = $b['brname']; $row['deladd1'] = $b['braddress1']; $row['deladd2'] = $b['braddress2'];
	$row['deladd3'] = $b['braddress3']; $row['deladd4'] = $b['braddress4']; $row['deladd5'] = $b['braddress5'];
	$row['orderstatusid'] = 1; $row['quotation'] = 0; $row['invoiceno'] = null; $row['printedpackingslip'] = 0;
	$row['orddate'] = date('Y-m-d'); $row['deliverydate'] = date('Y-m-d', time() + 86400); $row['date_created'] = date('Y-m-d H:i:s');
	$cols = []; $vals = [];
	foreach ($row as $k => $v) { $cols[] = "`$k`"; $vals[] = $v === null ? "NULL" : "'" . $m->real_escape_string((string) $v) . "'"; }
	if (!$m->query("insert into salesorders (" . implode(",", $cols) . ") values (" . implode(",", $vals) . ")")) { $m->rollback(); fwrite(STDERR, "insert failed: " . $m->error . "\n"); exit(1); }
	foreach ($lines as $ln) {
		$l = $ln; $l['orderno'] = $row['orderno']; $l['qtyinvoiced'] = 0; $l['completed'] = 0;
		$c2 = []; $v2 = [];
		foreach ($l as $k => $v) { if ($k === 'orderlineno' && isset($l['orderlineno']) && false) continue; $c2[] = "`$k`"; $v2[] = $v === null ? "NULL" : "'" . $m->real_escape_string((string) $v) . "'"; }
		if (!$m->query("insert into salesorderdetails (" . implode(",", $c2) . ") values (" . implode(",", $v2) . ")")) { $m->rollback(); fwrite(STDERR, "line insert failed: " . $m->error . "\n"); exit(1); }
	}
	$created[] = ['orderno' => $row['orderno'], 'lat' => $b['lat'], 'lng' => $b['lng'], 'name' => $b['brname']];
}
// A truck column with a handful of them: what recalc() re-sorts.
$sort = 0;
foreach (array_slice($created, 0, $ontruck) as $c) {
	$content = $m->real_escape_string($c['name']);
	$m->query("insert into oms (orderno, column_position, sort_value, content, date_added, date_updated, status_id, userid, lat, lng, original_sort, distance, customer, location, type, passed, invoiced) values ('{$c['orderno']}','" . $m->real_escape_string($truck) . "','$sort','$content',now(),now(),0,'AIOS-SEED','{$c['lat']}','{$c['lng']}','$sort',0,'','01',10,0,0)");
	$sort++;
}
$m->commit();
echo "seeded ", count($created), " orders (", $created[0]['orderno'], "..", end($created)['orderno'], "), $ontruck on truck column $truck\n";
