<?php
// Compares one query's full result on ASQL and MariaDB, row by row. Numbers
// within a tolerance are equal (the ERP's amounts are DOUBLE, so two engines
// add them in different orders and differ in the last digits: a balance of 0
// reads 5.7e-14 on one and 0 on the other); everything else must match exactly.
//   php cmpreport.php 'SELECT ... ORDER BY k' [tol=0.005] [skip=3,5] [asql=127.0.0.1:13401] [maria=127.0.0.1:3306]
// skip=: column numbers SQL leaves unspecified (a column the query neither groups nor aggregates).
mysqli_report(MYSQLI_REPORT_OFF);
$q = $argv[1]; $tol = 0.005; $skip = []; $opt = ['asql' => '127.0.0.1:13401', 'maria' => '127.0.0.1:3306'];
foreach (array_slice($argv, 2) as $a) { if (preg_match('/^tol=(.+)$/', $a, $m)) $tol = (float)$m[1]; elseif (preg_match('/^skip=(.+)$/', $a, $m)) $skip = array_map('intval', explode(',', $m[1])); elseif (preg_match('/^(asql|maria)=(.+)$/', $a, $m)) $opt[$m[1]] = $m[2]; }
function c($spec, $u, $p) { [$h, $port] = explode(':', $spec); return new mysqli($h, $u, $p, 'summitholding', (int)$port); }
$A = c($opt['asql'], 'root', ''); $M = c($opt['maria'], 'bench', 'bench');
$t = hrtime(true); $ra = $A->query($q); $ta = (hrtime(true) - $t) / 1e6;
$t = hrtime(true); $rm = $M->query($q); $tm = (hrtime(true) - $t) / 1e6;
if (!($ra instanceof mysqli_result) || !($rm instanceof mysqli_result)) { echo "ERR asql[{$A->error}] maria[{$M->error}]\n"; exit(2); }
$a = $ra->fetch_all(); $m = $rm->fetch_all();
$bad = 0; $first = [];
if (count($a) !== count($m)) { printf("ROWS differ: ASQL %d  MariaDB %d\n", count($a), count($m)); }
for ($i = 0; $i < min(count($a), count($m)); $i++) {
  foreach ($a[$i] as $j => $v) {
    if (in_array($j, $skip, true)) continue;
    $w = $m[$i][$j];
    if ($v === $w) continue;
    if ($v !== null && $w !== null && is_numeric($v) && is_numeric($w) && abs($v - $w) <= $tol) continue;
    $bad++; if (count($first) < 3) $first[] = sprintf("row %d col %d: ASQL [%s] MariaDB [%s]", $i, $j, substr((string)$v, 0, 40), substr((string)$w, 0, 40));
  }
}
printf("%s  rows %d/%d  ASQL %.0f ms  MariaDB %.0f ms  cells differing: %d\n", $bad === 0 && count($a) === count($m) ? 'SAME' : 'DIFF', count($a), count($m), $ta, $tm, $bad);
foreach ($first as $f) echo "  $f\n";
