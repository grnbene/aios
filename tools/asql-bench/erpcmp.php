<?php
$B = __DIR__ . '/erp';
$a = json_decode(file_get_contents("$B/asql.{$argv[1]}.json"), true); $d = json_decode(file_get_contents("$B/duck.{$argv[1]}.json"), true);
foreach ($a as $k => $v) {
  printf("%s: asql %.0f [%.0f-%.0f] | duck %.0f [%.0f-%.0f] ms\n", $k, $v['median_ms'], $v['min_ms'], $v['max_ms'], $d[$k]['median_ms'], $d[$k]['min_ms'], $d[$k]['max_ms']);
  $diff = 0; $first = '';
  foreach ($v['answer'] as $i => $row) foreach ($row as $c => $x) {
    $y = $d[$k]['answer'][$i][$c] ?? null;
    $eq = (is_numeric($x) && is_numeric($y)) ? abs($x - $y) <= 1e-6 * max(abs((float)$x), 1) : (string)$x === (string)$y;
    if (!$eq) { $diff++; if (!$first) $first = "row $i col $c: asql=$x duck=" . var_export($y, true); }
  }
  echo "   ", count($v['answer']), " rows vs ", count($d[$k]['answer']), ", $diff cells differ", $first ? " (first: $first)" : "", "\n";
}
