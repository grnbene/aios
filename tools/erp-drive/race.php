<?php
// ASQL against MariaDB on the ERP's own statements (a DUMP= dir from
// erp-drive.php), same data in both, same transport. Per statement: median
// microseconds as the ERP sends it (each side's result/query cache in play)
// and with SQL_NO_CACHE (the engines themselves), plus whether the answers
// match - a side note, the timing is the result.
//
//   php race.php STMT_DIR [N] [asql=host:port] [maria=host:port]
//   php race.php STMT_DIR 200 asql=/opt/aios/run/asql.sock maria=/run/mysqld/mysqld.sock
// A value starting with '/' is a unix socket.
mysqli_report(MYSQLI_REPORT_OFF);
$dir = $argv[1];
$N = (int)($argv[2] ?? 100);
$opt = ['asql' => '127.0.0.1:13401', 'maria' => '127.0.0.1:3306'];
foreach (array_slice($argv, 3) as $a) if (preg_match('/^(asql|maria)=(.+)$/', $a, $m)) $opt[$m[1]] = $m[2];
function conn($spec, $user, $pass) {
  if ($spec[0] === '/') return new mysqli('localhost', $user, $pass, 'summitholding', 0, $spec);
  [$h, $p] = explode(':', $spec);
  return new mysqli($h, $user, $pass, 'summitholding', (int)$p);
}
$A = conn($opt['asql'], getenv('ASQL_USER') ?: 'root', getenv('ASQL_PASS') ?: '');
$M = conn($opt['maria'], getenv('MARIA_USER') ?: 'bench', getenv('MARIA_PASS') ?: 'bench');
if ($A->connect_errno || $M->connect_errno) { fwrite(STDERR, "connect failed: {$A->connect_error} {$M->connect_error}\n"); exit(1); }

function nocache($q) {
  if (preg_match('/^\s*WITH\s/i', $q)) return preg_replace('/\)\s*SELECT\s+(?!SQL_NO_CACHE)/i', ') SELECT SQL_NO_CACHE ', $q, 1);
  return preg_replace('/^\s*SELECT\s+(?!SQL_NO_CACHE)/i', 'SELECT SQL_NO_CACHE ', $q, 1);
}
function run($c, $q) { $r = $c->query($q); if ($r instanceof mysqli_result) { $rows = $r->fetch_all(); return $rows; } return $r ? [] : null; }
// Median of rounds, each round N/rounds executions: A and M interleaved per
// round so neither gets a quieter moment of the machine.
function race($A, $M, $q, $N) {
  $ta = []; $tm = [];
  run($A, $q); run($M, $q);
  $rounds = 5; $per = max(1, intdiv($N, $rounds));
  for ($r = 0; $r < $rounds; $r++) {
    $s = hrtime(true); for ($i = 0; $i < $per; $i++) run($A, $q); $ta[] = (hrtime(true) - $s) / 1e3 / $per;
    $s = hrtime(true); for ($i = 0; $i < $per; $i++) run($M, $q); $tm[] = (hrtime(true) - $s) / 1e3 / $per;
  }
  sort($ta); sort($tm);
  return [$ta[2], $tm[2]];
}
// Rows as MariaDB and ASQL print them can differ only in how a number is
// written; compare loosely on numbers so the note means something.
function norm($rows) {
  if ($rows === null) return 'ERR';
  return json_encode(array_map(fn($r) => array_map(fn($v) => is_numeric($v) ? (string)(0 + $v) : $v, $r), $rows));
}

$files = glob("$dir/stmt-*.sql");
natsort($files);
$tot = ['sent' => [0, 0], 'nc' => [0, 0]];
$seen = [];
printf("%-7s %10s %10s %6s   %10s %10s %6s  %s\n", 'stmt', 'ASQL', 'MariaDB', 'x', 'ASQL nc', 'Maria nc', 'x', 'answers / statement');
foreach ($files as $f) {
  $q = file_get_contents($f);
  if (!preg_match('/^\s*(SELECT|WITH)/i', $q)) continue;
  $key = md5($q);
  if (isset($seen[$key])) continue;
  $seen[$key] = 1;
  [$as, $ms] = race($A, $M, $q, $N);
  $qn = nocache($q);
  $n2 = $as > 20000 ? max(5, intdiv($N, 20)) : $N;
  [$an, $mn] = race($A, $M, $qn, $n2);
  $same = norm(run($A, $qn)) === norm(run($M, $qn)) ? 'same' : 'DIFF';
  $tot['sent'][0] += $as; $tot['sent'][1] += $ms; $tot['nc'][0] += $an; $tot['nc'][1] += $mn;
  printf("%-7s %8.0fus %8.0fus %5.1fx   %8.0fus %8.0fus %5.1fx  %s  %s\n", basename($f, '.sql'), $as, $ms, $ms / max($as, 0.001),
         $an, $mn, $mn / max($an, 0.001), $same, substr(preg_replace('/\s+/', ' ', $q), 0, 70));
}
printf("\nsum of per-statement medians: as sent  ASQL %.1f ms  MariaDB %.1f ms  (%.2fx)\n", $tot['sent'][0] / 1e3, $tot['sent'][1] / 1e3, $tot['sent'][1] / max($tot['sent'][0], 0.001));
printf("                              no cache ASQL %.1f ms  MariaDB %.1f ms  (%.2fx)\n", $tot['nc'][0] / 1e3, $tot['nc'][1] / 1e3, $tot['nc'][1] / max($tot['nc'][0], 0.001));
echo "x = MariaDB time / ASQL time: above 1 ASQL is faster.\n";
