<?php
/**
 * Replay real dispatch runs against the aios kernels — read-only.
 *
 * For each truck's run on a given day (rows of `dispatch`), three totals in
 * haversine miles from the run's own start point:
 *   driven   the order the run was actually sorted in (`sort`)
 *   greedy   nearest-unvisited from the start — what getshortest() does
 *   aios     the exact optimum when the run fits under the exact ceiling,
 *            otherwise the 2-opt/Or-opt improvement of the greedy order
 *
 *   php replay-runs.php [db=versioneight] [date=yesterday] [maxExact=14]
 */
include "/var/www/api/config.php";
require "/var/www/api/system/library/dispatchroute.php";
require "/var/www/api/system/library/aioscompute.php";

$db = $argv[1] ?? 'versioneight'; $day = $argv[2] ?? date('Y-m-d', strtotime('yesterday')); $maxExact = (int) ($argv[3] ?? 14);
$m = new mysqli(DB_HOSTNAME, DB_USERNAME, DB_PASSWORD, $db);
if ($m->connect_errno) { fwrite(STDERR, "connect failed\n"); exit(1); }
$dr = new DispatchRoute();
$depot = array('lat' => 42.928455044944165, 'lng' => -78.82142723259965);   // getshortest()'s start; used when a run has no start row

$runs = array();
$r = $m->query("select truckid, dispatch_id, invoice, ispoint, sort, destination, lat, lng from dispatch where date(datecreated)='" . $m->real_escape_string($day) . "' and truckid not in (0,99) order by truckid, ispoint desc, sort, dispatch_id");
while ($x = $r->fetch_assoc()) {
	$lat = (float) $x['lat']; $lng = (float) $x['lng'];
	if ($lat == 0.0 && $lng == 0.0 && $x['destination']) { $d = json_decode($x['destination'], true); if (is_array($d)) { $lat = (float) ($d['lat'] ?? 0); $lng = (float) ($d['lng'] ?? 0); } }
	$runs[$x['truckid']][] = array('id' => (string) $x['dispatch_id'], 'invoice' => $x['invoice'], 'ispoint' => (int) $x['ispoint'], 'sort' => (int) $x['sort'], 'lat' => $lat, 'lng' => $lng);
}
if (!$runs) { echo "no dispatch rows on $day in $db\n"; exit(0); }

function path_miles(DispatchRoute $dr, array $start, array $seq) { $t = 0.0; $p = $start; foreach ($seq as $s) { $t += $dr->distance($p['lat'], $p['lng'], $s['lat'], $s['lng']); $p = $s; } return $t; }
function greedy(DispatchRoute $dr, array $start, array $stops) { $out = array(); $p = $start; $left = $stops; while ($left) { $bi = null; $bd = INF; foreach ($left as $i => $s) { $d = $dr->distance($p['lat'], $p['lng'], $s['lat'], $s['lng']); if ($d < $bd) { $bd = $d; $bi = $i; } } $out[] = $left[$bi]; $p = $left[$bi]; unset($left[$bi]); } return $out; }

$ping = AiosCompute::ping();
if (!$ping) { fwrite(STDERR, "service not reachable: " . AiosCompute::lastError() . "\n"); exit(2); }
printf("%s runs on %s (%s), exact ceiling %d stops\n\n", count($runs), $day, $db, $maxExact);
printf("%-6s %-5s %-10s %-10s %-10s %-18s %-9s %s\n", "truck", "stops", "driven_mi", "greedy_mi", "aios_mi", "engine", "kernel_ms", "saved vs driven");
$tot = array('driven' => 0, 'greedy' => 0, 'aios' => 0);
foreach ($runs as $truck => $rows) {
	$start = null; $stops = array();
	foreach ($rows as $row) { if ($row['ispoint'] === 1) $start = $row; elseif ($row['ispoint'] === 0 && !($row['lat'] == 0.0 && $row['lng'] == 0.0)) $stops[] = $row; }
	if (!$start) $start = $depot;
	$n = count($stops);
	if ($n < 2) continue;
	usort($stops, function ($a, $b) { return $a['sort'] <=> $b['sort'] ?: strcmp($a['id'], $b['id']); });
	$driven = path_miles($dr, $start, $stops);
	$g = greedy($dr, $start, $stops);
	$greedyMiles = path_miles($dr, $start, $g);
	$engine = ''; $aiosMiles = null; $ms = 0;
	if ($n <= $maxExact) {
		$e = AiosCompute::tspExact(array('id' => 'S', 'lat' => $start['lat'], 'lng' => $start['lng']), $stops, array('deadline_ms' => 60000));
		if ($e && $e['status'] === 'done') { $aiosMiles = $e['distance_miles']; $engine = 'tsp_exact'; $ms = $e['elapsed_ms']; }
	} else {
		$e = AiosCompute::routeImprove(array('lat' => $start['lat'], 'lng' => $start['lng']), 'last', null, $stops, array('deadline_ms' => 60000));
		if ($e && $e['status'] === 'done') { $aiosMiles = $e['total_distance_miles']; $engine = 'route_improve'; $ms = $e['elapsed_ms']; }
	}
	if ($aiosMiles === null) { $engine = 'FAILED ' . AiosCompute::lastError(); $aiosMiles = $greedyMiles; }
	$tot['driven'] += $driven; $tot['greedy'] += $greedyMiles; $tot['aios'] += $aiosMiles;
	printf("%-6s %-5d %-10.1f %-10.1f %-10.1f %-18s %-9.1f %+.1f mi (%+.0f%%)\n", $truck, $n, $driven, $greedyMiles, $aiosMiles, $engine, $ms, $aiosMiles - $driven, $driven > 0 ? ($aiosMiles - $driven) / $driven * 100 : 0);
}
printf("\nTOTAL  driven %.1f mi   greedy %.1f mi   aios %.1f mi   => aios vs driven %+.1f mi, vs greedy %+.1f mi\n", $tot['driven'], $tot['greedy'], $tot['aios'], $tot['aios'] - $tot['driven'], $tot['aios'] - $tot['greedy']);
