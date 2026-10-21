<?php
/**
 * The OMS board's open-order ordering at pool scale: oms.php::getshortest()'s
 * own greedy walk (copied here statement for statement, with DispatchRoute's
 * distance() and minkey()) against the aios erp.pool_greedy kernel, on REAL
 * moov branch coordinates. Same order, same miles required; the point is time.
 *
 *   php pool_parity.php [sizes=50,200,999]
 */
$SRV = getenv('V8_SERVER') ?: '/repos/v8/server';
require $SRV . '/system/library/dispatchroute.php';
require $SRV . '/system/library/aioscompute.php';

$sizes = isset($argv[1]) ? array_map('intval', explode(',', $argv[1])) : array(50, 200, 999);
$branchFile = $argv[2] ?? (__DIR__ . '/moov_branches.jsonl');
$branches = array();
foreach (file($branchFile, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) as $line) {
	$b = json_decode($line, true);
	if ($b && $b['lat'] != 0) $branches[] = $b;
}
echo count($branches), " real geocoded branches from ", basename($branchFile), "\n";
$depot = array('id' => '-1', 'lat' => 42.928455044944165, 'lng' => -78.82142723259965);   // what getshortest() starts from

/** getshortest()'s greedy, as written: full matrix, then nearest-unvisited with minkey(). */
function php_getshortest(array $points, array $depot) {
	$dr = new DispatchRoute();
	$input = $points;
	array_unshift($input, $depot);
	$distancesarray = array();
	foreach ($input as $point) {
		foreach ($input as $dest) {
			if ($point['id'] != $dest['id']) {
				$distancesarray[$point['id']][$dest['id']] = $dr->distance($point['lat'], $point['lng'], $dest['lat'], $dest['lng']);
			}
		}
	}
	$allorderstodeliver = array_keys($distancesarray);
	$maxloop = count($allorderstodeliver);
	$countof = array();
	$pickedistances = array();
	while (count($countof) < $maxloop) {
		$counted = count($countof);
		if (!$counted) {
			$order = '-1';
			$countof[] = $allorderstodeliver[$counted];
			$distances = $distancesarray[$order];
			$value = $dr->minkey($distances, $order);
			$pickedistances[$value] = $distances[$value];
			$countof[] = $value;
		} else {
			$prevorder = $countof[$counted - 1];
			$distances = $distancesarray[$prevorder];
			$allowed = array();
			foreach ($distances as $order => $distance) {
				if (!in_array($order, $countof)) {
					$allowed[$order] = $distances[$order];
				}
			}
			$value = $dr->minkey($allowed);
			$pickedistances[$value] = $distances[$value];
			$countof[] = $value;
		}
	}
	$order = array(); $total = 0.0;
	foreach ($countof as $o) { if ($o != '-1' && $o !== '') { $order[] = (string) $o; $total += $pickedistances[$o]; } }
	return array('order' => $order, 'total' => $total);
}

$ping = AiosCompute::ping();
if (!$ping) { fwrite(STDERR, "service not reachable: " . AiosCompute::lastError() . "\n"); exit(2); }
printf("%-6s %-14s %-14s %-12s %-12s %s\n", "orders", "php_miles", "aios_miles", "php_ms", "aios_ms", "verdict");
$fail = 0;
foreach ($sizes as $n) {
	mt_srand(7 + $n);
	$idx = range(0, count($branches) - 1); shuffle($idx);
	$points = array();
	for ($i = 0; $i < $n; $i++) { $b = $branches[$idx[$i % count($idx)]]; $points[] = array('id' => 'O' . ($i + 1), 'lat' => $b['lat'] + ($i >= count($idx) ? 0.0001 * $i : 0), 'lng' => $b['lng']); }
	$t0 = microtime(true); $php = php_getshortest($points, $depot); $phpMs = (microtime(true) - $t0) * 1000;
	$aios = AiosCompute::poolGreedy($depot, $points, array('deadline_ms' => 60000));
	if (!$aios || $aios['status'] !== 'done') { $fail++; printf("%-6d %-14.6f %-14s %-12.0f %-12s FAIL %s\n", $n, $php['total'], '-', $phpMs, '-', AiosCompute::lastError()); continue; }
	$ok = ($php['order'] === $aios['order']) && abs($php['total'] - $aios['total_miles']) < 1e-6;
	if (!$ok) $fail++;
	printf("%-6d %-14.6f %-14.6f %-12.0f %-12.2f %s\n", $n, $php['total'], $aios['total_miles'], $phpMs, $aios['elapsed_ms'], $ok ? 'match' : 'MISMATCH');
}
exit($fail ? 1 : 0);
