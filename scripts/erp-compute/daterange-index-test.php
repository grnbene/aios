<?php
/**
 * What an index on stocktrans.trandate would buy the daily report — measured on
 * a throwaway copy, so the real table is never touched. Drops the copy after.
 *
 *   V8_SERVER=/var/www/api php daterange-index-test.php [db=versioneight]
 */
$SRV = getenv('V8_SERVER') ?: '/var/www/v8/server';
include "$SRV/config.php";
$dbn = $argv[1] ?? 'versioneight';
$m = new mysqli(DB_HOSTNAME, DB_USERNAME, DB_PASSWORD, $dbn);
if ($m->connect_errno) { fwrite(STDERR, "connect failed\n"); exit(1); }
$s = date('Y-m-d', strtotime('-365 days'));
$e = date('Y-m-d');
$tmp = 'aios_tmp_stocktrans';

function ms($t) { return (microtime(true) - $t) * 1000; }

$m->query("DROP TABLE IF EXISTS `$tmp`");
$t = microtime(true);
$m->query("CREATE TABLE `$tmp` LIKE stocktrans");
$m->query("INSERT INTO `$tmp` SELECT * FROM stocktrans");
printf("copied %s rows in %.0f ms\n", $m->query("select count(*) c from `$tmp`")->fetch_assoc()['c'], ms($t));

$q = "SELECT trandate, SUM(qty) FROM `$tmp` WHERE trandate BETWEEN DATE('$s') AND DATE('$e') GROUP BY trandate";
$t = microtime(true); $m->query($q); $before = ms($t);
printf("aggregate on the copy, no index:   %8.0f ms\n", $before);

$t = microtime(true); $m->query("CREATE INDEX idx_aios_trandate ON `$tmp` (trandate)"); $build = ms($t);
printf("building the index:                %8.0f ms (one time)\n", $build);

$t = microtime(true); $m->query($q); $after1 = ms($t);
$t = microtime(true); $m->query($q); $after2 = ms($t);
printf("aggregate WITH index:              %8.0f ms (again: %.0f ms)\n", $after1, $after2);

$t = microtime(true); $r = $m->query("SELECT DISTINCT DATE(trandate) d FROM `$tmp` WHERE trandate BETWEEN DATE('$s') AND DATE('$e')"); $dist = ms($t);
printf("selling days WITH index:           %8.0f ms -> %d days\n", $dist, $r->num_rows);

$m->query("DROP TABLE `$tmp`");
echo "copy dropped\n";
printf("\nreport's current cost for this step: 6649 ms  ->  with index + no CTE: ~%.0f ms\n", $dist);
