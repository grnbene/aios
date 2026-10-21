<?php
// Column definitions as a client reads them, ASQL against MariaDB, for every SELECT in a DUMP= dir
// (see erp-drive.php): name, original name, table, original table, schema, type class, decimals.
//   php cmpmeta.php STMT_DIR [asql=127.0.0.1:13401] [maria=127.0.0.1:3306]
mysqli_report(MYSQLI_REPORT_OFF);
$opt = ['asql' => '127.0.0.1:13401', 'maria' => '127.0.0.1:3306'];
foreach (array_slice($argv, 2) as $a) if (preg_match('/^(asql|maria)=(.+)$/', $a, $m)) $opt[$m[1]] = $m[2];
function c($spec, $u, $p) { [$h, $port] = explode(':', $spec); return new mysqli($h, $u, $p, 'summitholding', (int)$port); }
$A = c($opt['asql'], 'root', ''); $M = c($opt['maria'], 'bench', 'bench');
function cls($t) {
  if (in_array($t, [1, 2, 3, 8, 9, 13], true)) return 'int';
  if ($t === 0 || $t === 246) return 'decimal';
  if ($t === 4 || $t === 5) return 'float';
  if ($t === 10) return 'date'; if ($t === 12 || $t === 7) return 'datetime'; if ($t === 11) return 'time';
  return 'string';
}
$n = 0; $cols = 0; $diff = []; $seen = [];
foreach (glob($argv[1] . '/stmt-*.sql') as $f) {
  $q = file_get_contents($f);
  if (!preg_match('/^\s*(SELECT|WITH)/i', $q) || isset($seen[md5($q)])) continue; $seen[md5($q)] = 1;
  $ra = $A->query($q); $rm = $M->query($q);
  if (!($ra instanceof mysqli_result) || !($rm instanceof mysqli_result)) continue;
  $fa = $ra->fetch_fields(); $fm = $rm->fetch_fields(); $n++;
  if (count($fa) !== count($fm)) { $diff['column count'][] = basename($f); continue; }
  foreach ($fa as $i => $x) {
    $y = $fm[$i]; $cols++;
    foreach (['name', 'orgname', 'table', 'orgtable', 'db'] as $k) if ($x->$k !== $y->$k) $diff[$k][] = basename($f) . ":$i [" . $x->$k . "] vs [" . $y->$k . "]";
    if (cls($x->type) !== cls($y->type)) $diff['type class'][] = basename($f) . ":{$x->name} ASQL " . cls($x->type) . "({$x->type}) vs MariaDB " . cls($y->type) . "({$y->type})";
    elseif (cls($y->type) === 'decimal' && $x->decimals !== $y->decimals) $diff['decimals'][] = basename($f) . ":{$x->name} ASQL {$x->decimals} vs MariaDB {$y->decimals}";
  }
}
printf("%d statements, %d columns compared\n", $n, $cols);
if (!$diff) { echo "all column definitions agree\n"; exit; }
foreach ($diff as $k => $v) { printf("%-12s %d columns differ, e.g.\n", $k, count($v)); foreach (array_slice($v, 0, (int)(getenv("ALL") ?: 4)) as $e) echo "    $e\n"; }
