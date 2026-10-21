<?php
/**
 * The daily report asks `stocktrans` for two columns that both live in
 * `stockmoves`. `stocktrans` is a view over an 8-way join with a correlated
 * subquery, so the whole join is materialised to answer it. Read-only
 * comparison of the three ways to get the same answer.
 *
 *   V8_SERVER=/var/www/api php stocktrans-view-test.php [db=versioneight]
 */
$SRV = getenv('V8_SERVER') ?: '/var/www/v8/server';
include "$SRV/config.php";
$dbn = $argv[1] ?? 'versioneight';
$m = new mysqli(DB_HOSTNAME, DB_USERNAME, DB_PASSWORD, $dbn);
if ($m->connect_errno) { fwrite(STDERR, "connect failed\n"); exit(1); }
$s = date('Y-m-d', strtotime('-365 days'));
$e = date('Y-m-d');
function ms($t) { return (microtime(true) - $t) * 1000; }

echo "stockmoves indexes: ";
$r = $m->query('show index from stockmoves'); $k = array();
while ($x = $r->fetch_assoc()) $k[] = $x['Key_name'] . '(' . $x['Column_name'] . ')';
echo implode(' ', array_unique($k)), "\n\n";

// 1. through the view, as the report does
$t = microtime(true);
$r1 = $m->query("SELECT trandate AS d, SUM(qty) total FROM stocktrans WHERE trandate BETWEEN DATE('$s') AND DATE('$e') GROUP BY trandate");
$a = array(); while ($x = $r1->fetch_assoc()) $a[substr($x['d'], 0, 10)] = round((float) $x['total'], 4);
printf("1  via the stocktrans VIEW:            %8.0f ms -> %d days with sales\n", ms($t), count($a));

// 2. straight at stockmoves, carrying the view's own filters
$filter = "stockid NOT IN ('T-A','FET','FEE','FGT','EDF1','C') AND stockid NOT LIKE 'DWI-%' AND (type = 10 OR type = 11)";
$t = microtime(true);
$r2 = $m->query("SELECT DATE(trandate) d, SUM(qty) total FROM stockmoves WHERE trandate BETWEEN DATE('$s') AND DATE('$e') AND $filter GROUP BY DATE(trandate)");
$b = array(); while ($x = $r2->fetch_assoc()) $b[$x['d']] = round((float) $x['total'], 4);
printf("2  straight at stockmoves (same filters): %8.0f ms -> %d days with sales\n", ms($t), count($b));

// 3. only what the report actually needs: which days had any sale at all
$t = microtime(true);
$r3 = $m->query("SELECT DISTINCT DATE(trandate) d FROM stockmoves WHERE trandate BETWEEN DATE('$s') AND DATE('$e') AND $filter");
$c = array(); while ($x = $r3->fetch_assoc()) $c[$x['d']] = true;
$msq = ms($t);
$t = microtime(true); $closed = array();
for ($d = strtotime($s); $d <= strtotime($e); $d += 86400) { $key = date('Y-m-d', $d); if (!isset($c[$key])) $closed[] = $key; }
printf("3  days-with-sales only:               %8.0f ms -> %d days; complement in PHP %.2f ms -> %d closed days\n", $msq, count($c), ms($t), count($closed));

ksort($a); ksort($b);
$sameDays = array_keys($a) == array_keys($b);
$sameTotals = true;
foreach ($a as $d => $v) if (!isset($b[$d]) || abs($b[$d] - $v) > 0.01) { $sameTotals = false; break; }
printf("\nsame days: %s   same totals: %s\n", $sameDays ? 'yes' : 'NO', $sameTotals ? 'yes' : 'NO');
printf("report's current cost for this step: 6649 ms  ->  option 3: ~%.0f ms\n", $msq);
