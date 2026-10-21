<?php
/**
 * Same math, same answer, timed: the ERP's own PHP against the aios kernels on
 * REAL dispatch runs, on this box. Read-only.
 *
 * For each truck's run on a day:
 *   greedy   PHP getshortest()'s nearest-unvisited walk   vs  erp.pool_greedy
 *   improve  PHP salesRouteImprove() (seed + 2-opt + Or-opt) vs erp.route_improve
 *   exact    PHP DispatchRoute::compute()                  vs  erp.tsp_exact
 *            (PHP is only asked up to `phpExactMax` stops — beyond that it
 *             cannot finish; the kernel's time is still shown)
 * A row PASSES only if the order and the miles are identical.
 *
 *   V8_SERVER=/var/www/api php replay-parity.php [db=versioneight] [date=yesterday] [phpExactMax=9]
 */
$SRV = getenv('V8_SERVER') ?: '/var/www/v8/server';
include "$SRV/config.php";
require "$SRV/system/library/dispatchroute.php";
require "$SRV/system/library/aioscompute.php";

$db = $argv[1] ?? 'versioneight'; $day = $argv[2] ?? date('Y-m-d', strtotime('yesterday')); $phpExactMax = (int) ($argv[3] ?? 9);
$m = new mysqli(DB_HOSTNAME, DB_USERNAME, DB_PASSWORD, $db);
if ($m->connect_errno) { fwrite(STDERR, "connect failed\n"); exit(1); }
$dr = new DispatchRoute();

// --- PHP, verbatim -----------------------------------------------------------
function php_greedy(DispatchRoute $dr, array $start, array $stops) {          // getshortest(), as written: full matrix, then nearest-unvisited chosen by minkey()
	$input = $stops; array_unshift($input, array('id' => '-1', 'lat' => $start['lat'], 'lng' => $start['lng']));
	$distancesarray = array();
	foreach ($input as $point) foreach ($input as $dest) if ($point['id'] != $dest['id']) $distancesarray[$point['id']][$dest['id']] = $dr->distance($point['lat'], $point['lng'], $dest['lat'], $dest['lng']);
	$allorderstodeliver = array_keys($distancesarray); $maxloop = count($allorderstodeliver); $countof = array(); $picked = array();
	while (count($countof) < $maxloop) {
		$counted = count($countof);
		if (!$counted) { $order = '-1'; $countof[] = $allorderstodeliver[$counted]; $distances = $distancesarray[$order]; $value = @$dr->minkey($distances, $order); $picked[$value] = $distances[$value]; $countof[] = $value; }
		else { $prevorder = $countof[$counted - 1]; $distances = $distancesarray[$prevorder]; $allowed = array(); foreach ($distances as $order => $distance) if (!in_array($order, $countof)) $allowed[$order] = $distances[$order]; $value = @$dr->minkey($allowed); $picked[$value] = $distances[$value]; $countof[] = $value; }
	}
	$out = array(); $total = 0.0;
	foreach ($countof as $o) if ($o != '-1' && $o !== '') { $out[] = (string) $o; $total += $picked[$o]; }
	return array($out, $total);
}
function salesRouteHaversine($lat1, $lon1, $lat2, $lon2) { $dLat = deg2rad($lat2 - $lat1); $dLon = deg2rad($lon2 - $lon1); $a = sin($dLat / 2) * sin($dLat / 2) + cos(deg2rad($lat1)) * cos(deg2rad($lat2)) * sin($dLon / 2) * sin($dLon / 2); return 3958.8 * 2 * atan2(sqrt($a), sqrt(1 - $a)); }
function salesRouteTourLength($seq, $startPt, $endPt) { $n = count($seq); if ($n === 0) return 0.0; $d = salesRouteHaversine($startPt[0], $startPt[1], (float) $seq[0]['lat'], (float) $seq[0]['lng']); for ($i = 1; $i < $n; $i++) $d += salesRouteHaversine((float) $seq[$i - 1]['lat'], (float) $seq[$i - 1]['lng'], (float) $seq[$i]['lat'], (float) $seq[$i]['lng']); if ($endPt !== null) $d += salesRouteHaversine((float) $seq[$n - 1]['lat'], (float) $seq[$n - 1]['lng'], $endPt[0], $endPt[1]); return $d; }
function salesRouteImprove($seq, $startPt, $endPt) { $n = count($seq); if ($n < 3 || $n > 120) return $seq; $best = salesRouteTourLength($seq, $startPt, $endPt); $guard = 0; $improved = true; while ($improved && $guard++ < 80) { $improved = false; for ($i = 0; $i < $n - 1; $i++) { for ($j = $i + 1; $j < $n; $j++) { $cand = $seq; $seg = array_reverse(array_slice($cand, $i, $j - $i + 1)); array_splice($cand, $i, $j - $i + 1, $seg); $len = salesRouteTourLength($cand, $startPt, $endPt); if ($len + 1e-9 < $best) { $seq = $cand; $best = $len; $improved = true; } } } foreach (array(1, 2, 3) as $L) { for ($i = 0; $i + $L <= $n; $i++) { $run = array_slice($seq, $i, $L); $rest = $seq; array_splice($rest, $i, $L); $mm = count($rest); for ($k = 0; $k <= $mm; $k++) { $cand = $rest; array_splice($cand, $k, 0, $run); $len = salesRouteTourLength($cand, $startPt, $endPt); if ($len + 1e-9 < $best) { $seq = $cand; $best = $len; $improved = true; break 3; } } } } } return $seq; }
function php_improve(array $start, array $stops) {                            // sales_route_optimize(): NN seed then salesRouteImprove, one-way ('last')
	$unvisited = array_values($stops); $optimized = array(); $clat = (float) $start['lat']; $clng = (float) $start['lng'];
	while (!empty($unvisited)) { $ni = 0; $nd = PHP_FLOAT_MAX; foreach ($unvisited as $idx => $stop) { $d = salesRouteHaversine($clat, $clng, (float) $stop['lat'], (float) $stop['lng']); if ($d < $nd) { $nd = $d; $ni = $idx; } } $nearest = $unvisited[$ni]; $clat = (float) $nearest['lat']; $clng = (float) $nearest['lng']; $optimized[] = $nearest; array_splice($unvisited, $ni, 1); }
	$startPt = array((float) $start['lat'], (float) $start['lng']);
	$optimized = salesRouteImprove($optimized, $startPt, null);
	return array(array_map(function ($s) { return (string) $s['id']; }, $optimized), salesRouteTourLength($optimized, $startPt, null));
}
function php_exact(array $start, array $stops) {                              // DispatchRoute::compute(): every ordering after the fixed start
	$r = new DispatchRoute(); $r->setStart('S'); $r->add('S', $start['lat'], $start['lng']);
	foreach ($stops as $s) $r->add($s['id'], $s['lat'], $s['lng']);
	$r->compute(); $best = $r->matching_shortest_routes();
	$o = array_values($best[0]); array_shift($o);   // drop the start
	return array(array_map('strval', $o), $r->shortest_distance());
}
function verdict($po, $pm, $ao, $am) { return ($po === $ao && abs($pm - $am) < 1e-6) ? 'SAME' : 'DIFFERENT'; }

// --- the runs -----------------------------------------------------------------
$runs = array();
$r = $m->query("select truckid, dispatch_id, ispoint, sort, destination, lat, lng from dispatch where date(datecreated)='" . $m->real_escape_string($day) . "' and truckid not in (0,99) order by truckid, ispoint desc, sort, dispatch_id");
while ($x = $r->fetch_assoc()) { $lat = (float) $x['lat']; $lng = (float) $x['lng']; if ($lat == 0.0 && $lng == 0.0 && $x['destination']) { $d = json_decode($x['destination'], true); if (is_array($d)) { $lat = (float) ($d['lat'] ?? 0); $lng = (float) ($d['lng'] ?? 0); } } $runs[$x['truckid']][] = array('id' => (string) $x['dispatch_id'], 'ispoint' => (int) $x['ispoint'], 'lat' => $lat, 'lng' => $lng); }
$ping = AiosCompute::ping(); if (!$ping) { fwrite(STDERR, "service not reachable: " . AiosCompute::lastError() . "\n"); exit(2); }
printf("%d runs on %s (%s). Every row: PHP's own function vs the kernel on the same stops — identical order and miles required.\n\n", count($runs), $day, $db);
printf("%-6s %-5s | %-24s %-9s %-9s %-9s | %-24s %-9s %-9s %-9s | %-24s %-9s %-9s %-9s\n", "truck", "stops", "GREEDY", "php_ms", "aios_ms", "same", "IMPROVE", "php_ms", "aios_ms", "same", "EXACT", "php_ms", "aios_ms", "same");
$fail = 0; $T = array('gp' => 0, 'ga' => 0, 'ip' => 0, 'ia' => 0, 'ep' => 0, 'ea' => 0);
foreach ($runs as $truck => $rows) {
	$start = null; $stops = array();
	foreach ($rows as $row) { if ($row['ispoint'] === 1) $start = $row; elseif ($row['ispoint'] === 0 && !($row['lat'] == 0.0 && $row['lng'] == 0.0)) $stops[] = $row; }
	if (!$start || count($stops) < 2) continue;
	$n = count($stops); $S = array('id' => 'S', 'lat' => $start['lat'], 'lng' => $start['lng']);
	// greedy
	$t = microtime(true); list($po, $pm) = php_greedy($dr, $S, $stops); $gp = (microtime(true) - $t) * 1000;
	$a = AiosCompute::poolGreedy($S, $stops, array('deadline_ms' => 30000)); $ga = $a ? $a['elapsed_ms'] : -1;
	$gs = $a ? verdict($po, $pm, $a['order'], $a['total_miles']) : 'FAIL';
	// improve
	$t = microtime(true); list($po2, $pm2) = php_improve($S, $stops); $ip = (microtime(true) - $t) * 1000;
	$b = AiosCompute::routeImprove(array('lat' => $S['lat'], 'lng' => $S['lng']), 'last', null, $stops, array('deadline_ms' => 60000)); $ia = $b ? $b['elapsed_ms'] : -1;
	$is = $b ? verdict($po2, $pm2, $b['order'], $b['total_distance_miles']) : 'FAIL';
	// exact
	$c = ($n <= AiosCompute::maxExactStops()) ? AiosCompute::tspExact($S, $stops, array('deadline_ms' => 120000)) : null; $ea = $c ? $c['elapsed_ms'] : 0;
	if ($n <= $phpExactMax) { $t = microtime(true); list($po3, $pm3) = php_exact($S, $stops); $ep = (microtime(true) - $t) * 1000; $es = $c ? verdict($po3, $pm3, array_slice($c['order'], 1), $c['distance_miles']) : 'FAIL'; $epS = sprintf('%.1f', $ep); }
	elseif ($n <= AiosCompute::maxExactStops()) { $ep = 0; $es = 'php cannot'; $epS = sprintf('~%.0fs*', array_product(range(1, $n)) / 1.4e6); }
	else { $ep = 0; $es = 'over ceiling'; $epS = '-'; }
	foreach (array($gs, $is, $es) as $v) if ($v === 'DIFFERENT' || $v === 'FAIL') $fail++;
	$T['gp'] += $gp; $T['ga'] += $ga; $T['ip'] += $ip; $T['ia'] += $ia; $T['ep'] += $ep; $T['ea'] += $ea;
	printf("%-6s %-5d | %-24s %-9.2f %-9.2f %-9s | %-24s %-9.1f %-9.2f %-9s | %-24s %-9s %-9.1f %-9s\n", $truck, $n, sprintf('%.3f mi', $pm), $gp, $ga, $gs, sprintf('%.3f mi', $pm2), $ip, $ia, $is, $c ? sprintf('%.3f mi', $c['distance_miles']) : '-', $epS, $ea, $es);
}
printf("\nTOTAL ms  greedy: php %.1f vs aios %.1f | improve: php %.1f vs aios %.1f | exact (where PHP could run): php %.1f vs aios %.1f\n", $T['gp'], $T['ga'], $T['ip'], $T['ia'], $T['ep'], $T['ea']);
echo "* estimated from PHP's measured 1.4M permutations/s; DispatchRoute stops at 3,000,000 iterations, so above 10 stops it never finishes.\n";
echo $fail ? "MISMATCHES: $fail\n" : "every comparable row identical\n";
exit($fail ? 1 : 0);
