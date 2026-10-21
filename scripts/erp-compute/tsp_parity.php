<?php
/**
 * Parity: the ERP's own DispatchRoute::compute() versus the aios erp.tsp_exact
 * kernel, on the same random instances. PHP can only finish small runs, so
 * the exact comparison stops at 9 stops; above that the kernel is compared
 * against itself with pruning on and off, which must agree exactly.
 *
 *   php tsp_parity.php [instances=60] [maxStops=9]
 */
require '/repos/v8/server/system/library/dispatchroute.php';
require '/repos/v8/server/system/library/aioscompute.php';

$instances = isset($argv[1]) ? (int) $argv[1] : 60;
$maxStops  = isset($argv[2]) ? (int) $argv[2] : 9;

mt_srand(20260911);
$start = array('id' => 'S', 'lat' => 42.928455044944165, 'lng' => -78.82142723259965);

function rnd($a, $b) { return $a + ($b - $a) * mt_rand() / mt_getrandmax(); }

function php_exact(array $start, array $stops) {
	$r = new DispatchRoute();
	$r->setStart($start['id']);
	$r->add($start['id'], $start['lat'], $start['lng']);
	foreach ($stops as $s) $r->add($s['id'], $s['lat'], $s['lng']);
	$t0 = microtime(true);
	$r->compute();
	$ms = (microtime(true) - $t0) * 1000;
	$best = $r->matching_shortest_routes();
	return array('order' => array_values($best[0]), 'distance' => $r->shortest_distance(), 'ms' => $ms, 'ties' => count($best));
}

$ping = AiosCompute::ping();
if (!$ping) { fwrite(STDERR, "aios compute service not reachable: " . AiosCompute::lastError() . "\n"); exit(2); }
echo "service: kernel {$ping['kernel']} lanes {$ping['lanes']} devices " . json_encode($ping['devices']) . "\n\n";

$pass = 0; $tie = 0; $fail = 0; $phpTotal = 0; $aiosTotal = 0;
printf("%-4s %-5s %-12s %-12s %-9s %-9s %s\n", "n", "stops", "php_miles", "aios_miles", "php_ms", "aios_ms", "verdict");
for ($k = 0; $k < $instances; $k++) {
	$n = 2 + ($k % ($maxStops - 1));            // 2..maxStops stops
	$stops = array();
	for ($i = 0; $i < $n; $i++) $stops[] = array('id' => 'O' . ($i + 1), 'lat' => rnd(42.75, 43.10), 'lng' => rnd(-79.05, -78.55));

	$php = php_exact($start, $stops);
	$t0 = microtime(true);
	$aios = AiosCompute::tspExact($start, $stops, array('deadline_ms' => 30000, 'prune' => true));
	$aiosMs = (microtime(true) - $t0) * 1000;
	if (!$aios || $aios['status'] !== 'done') { $fail++; printf("%-4d %-5d %-12.6f %-12s %-9.2f %-9.2f FAIL %s\n", $k, $n, $php['distance'], '-', $php['ms'], $aiosMs, AiosCompute::lastError()); continue; }

	$dEq = abs($php['distance'] - $aios['distance_miles']) < 1e-6;
	$oEq = ($php['order'] === $aios['order']);
	$verdict = $dEq ? ($oEq ? 'match' : ($php['ties'] > 1 ? 'tie (equal miles, different order)' : 'MISMATCH order at equal miles')) : 'MISMATCH miles';
	if ($dEq && $oEq) $pass++; elseif ($dEq && $php['ties'] > 1) $tie++; else $fail++;
	$phpTotal += $php['ms']; $aiosTotal += $aios['elapsed_ms'];
	printf("%-4d %-5d %-12.6f %-12.6f %-9.2f %-9.2f %s%s\n", $k, $n, $php['distance'], $aios['distance_miles'], $php['ms'], $aios['elapsed_ms'], $verdict, $aios['cached'] ? ' (cached)' : '');
}
echo "\nexact-vs-PHP: $pass identical, $tie tied, $fail failed. PHP total {$phpTotal} ms, kernel total {$aiosTotal} ms\n\n";

// Above PHP's reach: prune on/off must agree, and this is where the time goes.
echo "kernel alone, prune off vs on (same optimum required):\n";
printf("%-5s %-12s %-12s %-10s %-10s %-14s %-12s %s\n", "stops", "miles_off", "miles_on", "ms_off", "ms_on", "evaluated_off", "evaluated_on", "verdict");
foreach (array(10, 11, 12, 13) as $n) {
	$stops = array();
	for ($i = 0; $i < $n; $i++) $stops[] = array('id' => 'O' . ($i + 1), 'lat' => rnd(42.75, 43.10), 'lng' => rnd(-79.05, -78.55));
	$off = AiosCompute::tspExact($start, $stops, array('deadline_ms' => 120000, 'prune' => false));
	$on  = AiosCompute::tspExact($start, $stops, array('deadline_ms' => 120000, 'prune' => true));
	if (!$off || !$on || $off['status'] !== 'done' || $on['status'] !== 'done') { echo "  n=$n: " . AiosCompute::lastError() . "\n"; continue; }
	$same = abs($off['distance_miles'] - $on['distance_miles']) < 1e-6 && $off['order'] === $on['order'];
	printf("%-5d %-12.6f %-12.6f %-10.1f %-10.1f %-14d %-12d %s\n", $n, $off['distance_miles'], $on['distance_miles'], $off['elapsed_ms'], $on['elapsed_ms'], $off['evaluated'], $on['evaluated'], $same ? 'match' : 'MISMATCH');
}
exit($fail ? 1 : 0);
