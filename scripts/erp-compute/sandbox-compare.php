<?php
/**
 * Time the board's endpoints on the moov_sandbox tenant with the aios bridge
 * off, then on — from the server itself, through nginx and php-fpm exactly as
 * the client hits them. Prints engines, miles and milliseconds; never a key.
 *
 *   php sandbox-compare.php [truck=27] [runs=3] [base=https://api2.qbcsys.com/]
 */
// The ERP server tree this box runs (V8_SERVER), default the moov layout.
$SRV = getenv('V8_SERVER') ?: '/var/www/v8/server';
include $SRV . "/config.php";
$truck = (string) ($argv[1] ?? '27'); $runs = (int) ($argv[2] ?? 3); $base = $argv[3] ?? 'https://api2.qbcsys.com/';
$tenant = $argv[4] ?? 'moov_sandbox'; $user = $argv[5] ?? 'JACK';
$m = new mysqli(DB_HOSTNAME, DB_USERNAME, DB_PASSWORD, "versioneight");
$key = $m->query("select keycode from apikeys where `database`='" . $m->real_escape_string($tenant) . "' and active=1 limit 1")->fetch_assoc()["keycode"];
if (!$key) { fwrite(STDERR, "no api key for tenant $tenant\n"); exit(1); }

function call($base, $key, $user, $route) {
	$url = $base . "?route=apir/" . $route . "&apikey=" . urlencode($key) . "&userid=" . urlencode($user);
	$ch = curl_init($url);
	curl_setopt_array($ch, [CURLOPT_RETURNTRANSFER => true, CURLOPT_HTTPHEADER => ["Authorization: Bearer {$key}_{$user}", "X-Device-UUID: aios-compare"], CURLOPT_TIMEOUT => 300, CURLOPT_SSL_VERIFYPEER => false, CURLOPT_SSL_VERIFYHOST => 0]);
	$t0 = microtime(true); $body = curl_exec($ch); $ms = (microtime(true) - $t0) * 1000; $code = curl_getinfo($ch, CURLINFO_HTTP_CODE); curl_close($ch);
	return [$code, json_decode($body, true), $ms, $body];
}
function setBridge($on) {
	global $SRV; $p = $SRV . "/config_aios.php"; $s = file_get_contents($p);
	$s = preg_replace("/define\('AIOS_COMPUTE_ENABLED', (true|false)\);/", "define('AIOS_COMPUTE_ENABLED', " . ($on ? 'true' : 'false') . ");", $s);
	file_put_contents($p, $s);
}
function miles($rows) { $t = 0.0; foreach ((array) $rows as $r) $t += (float) ($r['distance'] ?? 0); return $t; }

foreach ([false, true] as $on) {
	setBridge($on);
	echo "\n=== bridge ", $on ? "ON" : "OFF", " ===\n";
	for ($i = 0; $i < $runs; $i++) {
		[$code, $j, $ms] = call($base, $key, $user, 'oms/cards');
		$orders = $j['orders'] ?? [];
		$meta = $j['aios'] ?? null;
		printf("oms/cards   http %d  %6.0f ms  orders=%d  miles=%.3f  engine=%s%s\n", $code, $ms, count($orders), miles($orders), $meta['engine'] ?? '-', isset($meta['kernel_ms']) ? sprintf("  kernel=%.1fms", $meta['kernel_ms']) : '');
	}
	for ($i = 0; $i < $runs; $i++) {
		[$code, $j, $ms, $body] = call($base, $key, $user, 'oms/recalc&column=' . urlencode($truck));
		$rows = is_array($j) ? $j : [];
		$eng = $rows ? ($rows[0]['engine'] ?? '-') : '-';
		$dist = 0.0; foreach ($rows as $r) $dist += (float) ($r['distance'] ?? 0);
		printf("oms/recalc  http %d  %6.0f ms  rows=%d  engine=%s  legs_sum=%.3f\n", $code, $ms, count($rows), $eng, $dist);
		if (!$rows && $i === 0) echo "   raw: ", substr($body, 0, 200), "\n";
	}
}
setBridge(true);
echo "\n(bridge left ON)\n";
