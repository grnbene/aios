<?php
/**
 * How much of the reorder refresh is arithmetic? Runs the script's own
 * compute_seasonal_from_history_rows() over every item in stockhistory —
 * read-only, no inserts — and times the SELECT and the compute separately.
 *
 *   V8_SERVER=/var/www/api php seasonal-timing.php [db=versioneight]
 */
$SRV = getenv('V8_SERVER') ?: '/var/www/v8/server';
include "$SRV/config.php";
$db = $argv[1] ?? 'versioneight';
$m = new mysqli(DB_HOSTNAME, DB_USERNAME, DB_PASSWORD, $db);
if ($m->connect_errno) { fwrite(STDERR, "connect failed\n"); exit(1); }

// verbatim from reorder_daily_snapshot_refresh.php
function compute_seasonal_from_history_rows($stockid, array $rows) {
	$default = array('grandaverage' => 0);
	for ($m = 1; $m <= 12; $m++) { $default['m' . $m . 'seasonal'] = 1; $default['m' . $m . 'avg'] = 0; }
	if (empty($rows)) return $default;
	$today = new DateTime(); $current_month = (int) $today->format('n'); $current_year = (int) $today->format('Y');
	$all_months = range(1, 12); $stock_result = array(); $totalnumberofmonthscalc = 0; $hasonlycurrentdata = true; $excludedmonths = array();
	foreach ($rows as $item) {
		$yearcode = (int) $item['yearcode'];
		if ($yearcode == $current_year) {
			foreach ($all_months as $month) {
				if ($month < $current_month) {
					$totalnumberofmonthscalc++;
					if (!isset($stock_result['grandtotal'])) $stock_result['grandtotal'] = 0;
					$stock_result['grandtotal'] += (float) $item['qty' . $month];
					$stock_result['grandaverage'] = $stock_result['grandtotal'] / $totalnumberofmonthscalc;
					if (!isset($stock_result['m' . $month]['details'])) { $stock_result['m' . $month]['details'] = array(); $stock_result['m' . $month . 'total'] = 0; }
					$stock_result['m' . $month]['details'][] = $item['qty' . $month];
					$stock_result['m' . $month . 'total'] += (float) $item['qty' . $month];
					$stock_result['m' . $month . 'avg'] = $stock_result['m' . $month . 'total'] / count($stock_result['m' . $month]['details']);
					$avg = $stock_result['grandaverage'];
					$stock_result['m' . $month . 'seasonal'] = $avg ? $stock_result['m' . $month . 'avg'] / $avg : 0;
				}
				if ($month > $current_month) $excludedmonths[] = $month;
			}
		} else {
			$hasonlycurrentdata = false;
			foreach ($all_months as $month) {
				$totalnumberofmonthscalc++;
				if (!isset($stock_result['grandtotal'])) $stock_result['grandtotal'] = 0;
				$stock_result['grandtotal'] += (float) $item['qty' . $month];
				$stock_result['grandaverage'] = $stock_result['grandtotal'] / $totalnumberofmonthscalc;
				if (!isset($stock_result['m' . $month]['details'])) { $stock_result['m' . $month]['details'] = array(); $stock_result['m' . $month . 'total'] = 0; }
				$stock_result['m' . $month]['details'][] = $item['qty' . $month];
				$stock_result['m' . $month . 'total'] += (float) $item['qty' . $month];
				$stock_result['m' . $month . 'avg'] = $stock_result['m' . $month . 'total'] / count($stock_result['m' . $month]['details']);
				$avg = $stock_result['grandaverage'];
				$stock_result['m' . $month . 'seasonal'] = $avg ? $stock_result['m' . $month . 'avg'] / $avg : 0;
			}
		}
	}
	if ($hasonlycurrentdata && !empty($excludedmonths)) {
		foreach ($excludedmonths as $month) { $stock_result['m' . $month . 'avg'] = isset($stock_result['grandaverage']) ? $stock_result['grandaverage'] : 0; $avg = isset($stock_result['grandaverage']) ? $stock_result['grandaverage'] : 0; $stock_result['m' . $month . 'seasonal'] = $avg; }
	}
	$out = array('grandaverage' => isset($stock_result['grandaverage']) ? $stock_result['grandaverage'] : 0);
	for ($m = 1; $m <= 12; $m++) { $out['m' . $m . 'seasonal'] = isset($stock_result['m' . $m . 'seasonal']) ? $stock_result['m' . $m . 'seasonal'] : 1; $out['m' . $m . 'avg'] = isset($stock_result['m' . $m . 'avg']) ? $stock_result['m' . $m . 'avg'] : 0; }
	return $out;
}

$t = microtime(true);
$r = $m->query("SELECT * FROM stockhistory ORDER BY stockid, yearcode ASC");
$by = array(); $n = 0;
while ($x = $r->fetch_assoc()) { $by[$x['stockid']][] = $x; $n++; }
$selectMs = (microtime(true) - $t) * 1000;
$t = microtime(true);
$items = 0; $checksum = 0.0;
foreach ($by as $sid => $rows) { $s = compute_seasonal_from_history_rows($sid, $rows); $items++; $checksum += $s['grandaverage'] + $s['m1seasonal'] + $s['m12avg']; }
$computeMs = (microtime(true) - $t) * 1000;
printf("SELECT + group: %d rows, %d items, %.0f ms\ncompute_seasonal_from_history_rows over all items: %.0f ms (%.4f ms per item), checksum %.4f\npeak memory %.0f MB\n", $n, $items, $selectMs, $computeMs, $computeMs / max(1, $items), $checksum, memory_get_peak_usage(true) / 1048576);
