<?php
$B = __DIR__; $E = ['asql', 'maria', 'cedar', 'duck']; $R = [];
foreach ($E as $e) foreach (['results', 'results2'] as $p) $R[$e][] = json_decode(file_get_contents("$B/$p/$e.json"), true);
function rng($a, $b, $f) { $lo = sprintf($f, min($a, $b)); $hi = sprintf($f, max($a, $b)); return $lo === $hi ? $lo : "$lo-$hi"; }
$row = function ($label, $get, $f) use ($E, $R) { $l = str_pad($label, 14); foreach ($E as $e) { $a = $get($R[$e][0]); $b = $get($R[$e][1]); $l .= ' | ' . str_pad($a === null ? '-' : rng($a, $b, $f), 13); } echo $l, "\n"; };
echo str_pad('', 14), ' | ', implode(' | ', array_map(fn($e) => str_pad($e, 13), $E)), "\n";
foreach (['full_agg', 'group_topn', 'range_agg', 'pivot', 'window'] as $q) $row($q, fn($r) => $r['query'][$q]['median_ms'], '%.2f');
$row('wire_insert', fn($r) => $r['wire_insert_ms'], '%.0f');
$row('bulk_load', fn($r) => $r['bulk_load_ms'] ?? null, '%.0f');
foreach (['p50_ms' => '%.3f', 'p99_ms' => '%.3f', 'qps' => '%.0f'] as $k => $f) $row("point.$k", fn($r) => $r['point'][$k], $f);
foreach (['p50_ms' => '%.3f', 'p99_ms' => '%.3f', 'tps' => '%.0f'] as $k => $f) $row("update.$k", fn($r) => $r['update'][$k], $f);
