<?php
/**
 * Is the daily report's WITH RECURSIVE date-range query expensive because of
 * the dates, or because of the stocktrans aggregate it joins against?
 *
 * Compares, read-only, on the same window:
 *   A  the query as the report issues it (recursive CTE + LEFT JOIN)
 *   B  only the days that HAVE sales, complement computed outside SQL
 *   C  the aggregate alone, to see where A's time actually goes
 *
 *   V8_SERVER=/var/www/api php daterange-test.php [db=versioneight] [days=365]
 */
$SRV = getenv('V8_SERVER') ?: '/var/www/v8/server';
include "$SRV/config.php";
$dbn = $argv[1] ?? 'versioneight';
$days = (int) ($argv[2] ?? 365);
$m = new mysqli(DB_HOSTNAME, DB_USERNAME, DB_PASSWORD, $dbn);
if ($m->connect_errno) { fwrite(STDERR, "connect failed\n"); exit(1); }
$s = date('Y-m-d', strtotime("-$days days"));
$e = date('Y-m-d');

echo "stocktrans rows: ", $m->query("select count(*) c from stocktrans")->fetch_assoc()['c'], "\n";
$r = $m->query("show index from stocktrans"); $k = array();
while ($x = $r->fetch_assoc()) $k[] = $x['Key_name'] . '(' . $x['Column_name'] . ')';
echo "indexes: ", implode(' ', array_unique($k)), "\n";
echo "window: $s .. $e\n\n";

$orig = "WITH RECURSIVE daterange AS (
    SELECT DATE('$s') AS dayof
    UNION ALL
    SELECT dayof + INTERVAL 1 DAY FROM daterange WHERE dayof < DATE('$e')
), daily_trans AS (
    SELECT trandate AS dayof, SUM(qty) AS total FROM stocktrans
    WHERE trandate BETWEEN DATE('$s') AND DATE('$e') GROUP BY trandate
)
SELECT d.dayof FROM daterange d LEFT JOIN daily_trans t ON t.dayof = d.dayof WHERE t.total IS NULL";

$t = microtime(true); $ra = $m->query($orig); $msA = (microtime(true) - $t) * 1000;
$closedA = array(); while ($x = $ra->fetch_row()) $closedA[] = $x[0];
printf("A  report's query (recursive CTE + LEFT JOIN): %8.0f ms -> %d closed days\n", $msA, count($closedA));

$t = microtime(true); $rb = $m->query("SELECT DISTINCT DATE(trandate) d FROM stocktrans WHERE trandate BETWEEN DATE('$s') AND DATE('$e')"); $msB = (microtime(true) - $t) * 1000;
$have = array(); while ($x = $rb->fetch_row()) $have[$x[0]] = true;
$t = microtime(true); $closedB = array();
for ($d = strtotime($s); $d <= strtotime($e); $d += 86400) { $key = date('Y-m-d', $d); if (!isset($have[$key])) $closedB[] = $key; }
$msComplement = (microtime(true) - $t) * 1000;
printf("B  selling days only (no CTE):              %8.0f ms -> %d selling days\n", $msB, count($have));
printf("   complement computed outside SQL:        %8.2f ms -> %d closed days\n", $msComplement, count($closedB));

$t = microtime(true); $m->query("SELECT trandate, SUM(qty) FROM stocktrans WHERE trandate BETWEEN DATE('$s') AND DATE('$e') GROUP BY trandate"); $msC = (microtime(true) - $t) * 1000;
printf("C  the aggregate alone (A's inner CTE):     %8.0f ms\n", $msC);

sort($closedA); sort($closedB);
printf("\nsame answer: %s\n", $closedA == $closedB ? 'yes, identical' : 'NO — A=' . count($closedA) . ' B=' . count($closedB));
printf("B total (query + complement) vs A: %.0f ms vs %.0f ms\n", $msB + $msComplement, $msA);
