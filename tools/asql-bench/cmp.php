<?php
// Each query's answer from every engine against MariaDB's: same rows in the same order, text equal, numbers within
// 1e-9 relative (sums of doubles differ in their last bits with the order they are added in).
$d = __DIR__ . "/" . ($argv[1] ?? "results");
$r = []; foreach (['asql', 'maria', 'cedar', 'duck'] as $e) $r[$e] = json_decode(file_get_contents("$d/$e.json"), true)['query'];
$same = function ($a, $b) {
  if (count($a) !== count($b)) return false;
  foreach ($a as $i => $row) {
    if (count($row) !== count($b[$i])) return false;
    foreach ($row as $j => $v) {
      $w = $b[$i][$j];
      if (is_numeric($v) && is_numeric($w)) { if (abs($v - $w) > 1e-9 * max(abs((float)$v), 1.0)) return false; }
      elseif ((string)$v !== (string)$w) return false;
    }
  }
  return true;
};
foreach (array_keys($r['asql']) as $q) {
  $line = "$q:";
  foreach ($r as $e => $x) $line .= " $e=" . ($same($x[$q]['answer'], $r['maria'][$q]['answer']) ? 'same' : 'DIFF');
  echo $line, " (", count($r['maria'][$q]['answer']), " rows)\n";
}
