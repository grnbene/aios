<?php
/**
 * Parity for erp.route_improve: the sales route builder's own seed + 2-opt +
 * Or-opt (copied verbatim from reports.php::sales_route_optimize and its
 * helpers) against the aios kernel, on the same random instances. The kernel
 * must return the same order and the same miles — this algorithm is
 * deterministic, so anything else is a porting error.
 *
 *   php route_parity.php [instances=40] [maxStops=40]
 */
require '/repos/v8/server/system/library/aioscompute.php';

function salesRouteHaversine($lat1, $lon1, $lat2, $lon2) {
	$dLat = deg2rad($lat2 - $lat1);
	$dLon = deg2rad($lon2 - $lon1);
	$a    = sin($dLat / 2) * sin($dLat / 2)
	      + cos(deg2rad($lat1)) * cos(deg2rad($lat2)) * sin($dLon / 2) * sin($dLon / 2);
	return 3958.8 * 2 * atan2(sqrt($a), sqrt(1 - $a));
}
function salesRouteTourLength($seq, $startPt, $endPt) {
	$n = count($seq);
	if ($n === 0) return 0.0;
	$d = salesRouteHaversine($startPt[0], $startPt[1], (float) $seq[0]['lat'], (float) $seq[0]['lng']);
	for ($i = 1; $i < $n; $i++) $d += salesRouteHaversine((float) $seq[$i - 1]['lat'], (float) $seq[$i - 1]['lng'], (float) $seq[$i]['lat'], (float) $seq[$i]['lng']);
	if ($endPt !== null) $d += salesRouteHaversine((float) $seq[$n - 1]['lat'], (float) $seq[$n - 1]['lng'], $endPt[0], $endPt[1]);
	return $d;
}
function salesRouteImprove($seq, $startPt, $endPt) {
	$n = count($seq);
	if ($n < 3 || $n > 120) return $seq;
	$best = salesRouteTourLength($seq, $startPt, $endPt);
	$guard = 0; $improved = true;
	while ($improved && $guard++ < 80) {
		$improved = false;
		for ($i = 0; $i < $n - 1; $i++) {
			for ($j = $i + 1; $j < $n; $j++) {
				$cand = $seq;
				$seg  = array_reverse(array_slice($cand, $i, $j - $i + 1));
				array_splice($cand, $i, $j - $i + 1, $seg);
				$len = salesRouteTourLength($cand, $startPt, $endPt);
				if ($len + 1e-9 < $best) { $seq = $cand; $best = $len; $improved = true; }
			}
		}
		foreach (array(1, 2, 3) as $L) {
			for ($i = 0; $i + $L <= $n; $i++) {
				$run  = array_slice($seq, $i, $L);
				$rest = $seq; array_splice($rest, $i, $L);
				$m = count($rest);
				for ($k = 0; $k <= $m; $k++) {
					$cand = $rest; array_splice($cand, $k, 0, $run);
					$len = salesRouteTourLength($cand, $startPt, $endPt);
					if ($len + 1e-9 < $best) { $seq = $cand; $best = $len; $improved = true; break 3; }
				}
			}
		}
	}
	return $seq;
}
function php_optimize($stops, $start, $endMode, $end) {
	$unvisited = array_values($stops); $optimized = array();
	$clat = (float) $start['lat']; $clng = (float) $start['lng'];
	while (!empty($unvisited)) {
		$ni = 0; $nd = PHP_FLOAT_MAX;
		foreach ($unvisited as $idx => $stop) { $d = salesRouteHaversine($clat, $clng, (float) $stop['lat'], (float) $stop['lng']); if ($d < $nd) { $nd = $d; $ni = $idx; } }
		$nearest = $unvisited[$ni]; $clat = (float) $nearest['lat']; $clng = (float) $nearest['lng']; $optimized[] = $nearest; array_splice($unvisited, $ni, 1);
	}
	$startPt = array((float) $start['lat'], (float) $start['lng']);
	$endPt = ($endMode === 'custom' && $end) ? array((float) $end['lat'], (float) $end['lng']) : ($endMode === 'last' ? null : $startPt);
	$t0 = microtime(true);
	$optimized = salesRouteImprove($optimized, $startPt, $endPt);
	$ms = (microtime(true) - $t0) * 1000;
	return array('order' => array_map(function ($s) { return (string) $s['id']; }, $optimized), 'distance' => salesRouteTourLength($optimized, $startPt, $endPt), 'ms' => $ms);
}

$instances = isset($argv[1]) ? (int) $argv[1] : 40;
$maxStops  = isset($argv[2]) ? (int) $argv[2] : 40;
mt_srand(20260912);
function rnd($a, $b) { return $a + ($b - $a) * mt_rand() / mt_getrandmax(); }

$ping = AiosCompute::ping();
if (!$ping) { fwrite(STDERR, "service not reachable: " . AiosCompute::lastError() . "\n"); exit(2); }
$modes = array('home', 'last', 'custom');
$pass = 0; $fail = 0; $phpTotal = 0; $kTotal = 0;
printf("%-4s %-5s %-7s %-12s %-12s %-9s %-9s %s\n", "n", "stops", "end", "php_miles", "aios_miles", "php_ms", "aios_ms", "verdict");
for ($k = 0; $k < $instances; $k++) {
	$n = 3 + ($k % ($maxStops - 2));
	$mode = $modes[$k % 3];
	$start = array('lat' => rnd(42.9, 43.2), 'lng' => rnd(-76.3, -76.0));
	$end = array('lat' => rnd(42.9, 43.2), 'lng' => rnd(-76.3, -76.0));
	$stops = array(); for ($i = 0; $i < $n; $i++) $stops[] = array('id' => 'S' . $i, 'lat' => rnd(42.6, 43.5), 'lng' => rnd(-76.8, -75.5));
	$php = php_optimize($stops, $start, $mode, $end);
	$aios = AiosCompute::routeImprove($start, $mode, $end, $stops, array('deadline_ms' => 60000));
	if (!$aios || $aios['status'] !== 'done') { $fail++; printf("%-4d %-5d %-7s %-12.6f %-12s %-9.1f %-9s FAIL %s\n", $k, $n, $mode, $php['distance'], '-', $php['ms'], '-', AiosCompute::lastError()); continue; }
	$ok = ($php['order'] === $aios['order']) && abs($php['distance'] - $aios['total_distance_miles']) < 1e-6;
	if ($ok) $pass++; else $fail++;
	$phpTotal += $php['ms']; $kTotal += $aios['elapsed_ms'];
	printf("%-4d %-5d %-7s %-12.6f %-12.6f %-9.1f %-9.2f %s\n", $k, $n, $mode, $php['distance'], $aios['total_distance_miles'], $php['ms'], $aios['elapsed_ms'], $ok ? 'match' : 'MISMATCH ' . implode(',', $php['order']) . ' vs ' . implode(',', $aios['order']));
}
echo "\nroute_improve vs PHP: $pass identical, $fail failed. PHP total {$phpTotal} ms, kernel total {$kTotal} ms\n";
foreach (array(80, 120) as $n) {
	$start = array('lat' => 43.0481, 'lng' => -76.1474); $stops = array(); for ($i = 0; $i < $n; $i++) $stops[] = array('id' => 'S' . $i, 'lat' => rnd(42.6, 43.5), 'lng' => rnd(-76.8, -75.5));
	$php = php_optimize($stops, $start, 'home', null);
	$aios = AiosCompute::routeImprove($start, 'home', null, $stops, array('deadline_ms' => 60000));
	printf("timing n=%d: PHP %.0f ms, kernel %.1f ms, %s\n", $n, $php['ms'], $aios ? $aios['elapsed_ms'] : -1, ($aios && $php['order'] === $aios['order']) ? 'same order' : 'ORDER DIFFERS');
}
exit($fail ? 1 : 0);
