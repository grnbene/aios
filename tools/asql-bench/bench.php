<?php
// One engine's run of the bench_sales tests over its wire protocol: php bench.php asql|maria|cedar
// Every time is measured here, around the client call that sends the statement and reads its whole answer.
// Prints JSON: each test's times (ms) and each query's answer, for the cross-engine check.
error_reporting(E_ALL);
$engine = $argv[1] ?? '';
$dir = __DIR__;
$pw = trim(file_get_contents("$dir/pw"));
$cfg = ['asql' => ['mysql', 3399], 'maria' => ['mysql', 13306], 'cedar' => ['pg', 15432]];
if (!isset($cfg[$engine])) { fwrite(STDERR, "usage: bench.php asql|maria|cedar\n"); exit(2); }
[$kind, $port] = $cfg[$engine];

function now_ms() { return hrtime(true) / 1e6; }
function median($a) { sort($a); $n = count($a); return $n % 2 ? $a[intdiv($n, 2)] : ($a[$n / 2 - 1] + $a[$n / 2]) / 2; }
function pct($a, $p) { sort($a); return $a[min(count($a) - 1, (int)floor($p / 100 * count($a)))]; }

if ($kind === 'mysql') {
  $db = mysqli_init();
  if (!$db->real_connect('127.0.0.1', 'root', $pw, '', $port)) { fwrite(STDERR, "connect failed\n"); exit(1); }
  $q = function ($sql) use ($db) {
    $r = $db->query($sql);
    if ($r === false) throw new Exception($db->error . " in: " . substr($sql, 0, 120));
    return $r === true ? [] : $r->fetch_all(MYSQLI_NUM);
  };
} else {
  $db = pg_connect("host=127.0.0.1 port=$port user=postgres password=$pw dbname=postgres");
  if (!$db) { fwrite(STDERR, "connect failed\n"); exit(1); }
  $q = function ($sql) use ($db) {
    $r = @pg_query($db, $sql);
    if ($r === false) throw new Exception(pg_last_error($db) . " in: " . substr($sql, 0, 120));
    return pg_fetch_all($r, PGSQL_NUM) ?: [];
  };
}
$out = ['engine' => $engine];
// The amount column: AMOUNT=decimal (the default; the ERP's money columns are DECIMAL) or AMOUNT=double. ASQL adds a
// DOUBLE column in row order, one thread, so that its sums are MariaDB's to the last bit; a DECIMAL it adds in parallel.
$amount = getenv('AMOUNT') ?: 'decimal';
if (!in_array($amount, ['decimal', 'double'], true)) { fwrite(STDERR, "AMOUNT=decimal|double\n"); exit(2); }
$double = $amount === 'double' ? ($kind === 'pg' ? 'DOUBLE PRECISION' : 'DOUBLE') : ($kind === 'pg' ? 'NUMERIC(12,2)' : 'DECIMAL(12,2)');
$out['amount_type'] = $double;
$ddl = fn($t) => "CREATE TABLE $t (id INT PRIMARY KEY, customer_id INT, category_id INT, amount $double, trans_date DATE, status VARCHAR(16))";

if ($kind === 'mysql') { $q("DROP DATABASE IF EXISTS bench"); $q("CREATE DATABASE bench"); $q("USE bench"); }
else { $q("DROP TABLE IF EXISTS bench_sales"); $q("DROP TABLE IF EXISTS bench_load"); }

// 1B. the 500,000 rows over the wire: 100 INSERTs of 5,000 rows, one connection, autocommit
$q($ddl('bench_sales'));
$lines = file("$dir/data/inserts.sql", FILE_IGNORE_NEW_LINES);
$t0 = now_ms();
foreach ($lines as $l) $q(rtrim($l, ';'));
$out['wire_insert_ms'] = now_ms() - $t0;
$out['rows'] = (int)$q("SELECT COUNT(*) FROM bench_sales")[0][0];

// 1A. the same rows from the CSV file, by the engine's own bulk loader
$q($ddl('bench_load'));
$load = ['maria' => "LOAD DATA INFILE '/data/bench_sales.csv' INTO TABLE bench_load FIELDS TERMINATED BY ','",
         'cedar' => "COPY bench_load FROM '/data/bench_sales.csv' WITH (FORMAT csv)",
         'asql'  => "LOAD DATA INFILE '$dir/data/bench_sales.csv' INTO TABLE bench_load FIELDS TERMINATED BY ','"][$engine];
try {
  $t0 = now_ms(); $q($load); $out['bulk_load_ms'] = now_ms() - $t0;
  $out['bulk_rows'] = (int)$q("SELECT COUNT(*) FROM bench_load")[0][0];
} catch (Exception $e) { $out['bulk_load_error'] = $e->getMessage(); }
$q("DROP TABLE bench_load");

// 2. the queries: one warm-up run, then 5 timed runs; the median and the range
$queries = [
  'full_agg' => "SELECT COUNT(*), SUM(amount), AVG(amount), MIN(amount), MAX(amount) FROM bench_sales",
  'group_topn' => "SELECT category_id, COUNT(*) AS n, SUM(amount) AS total FROM bench_sales GROUP BY category_id ORDER BY total DESC LIMIT 10",
  'range_agg' => "SELECT COUNT(*), SUM(amount) FROM bench_sales WHERE trans_date BETWEEN '2024-03-01' AND '2024-08-31' AND amount > 100",
  'pivot' => "SELECT category_id, SUM(CASE WHEN status = 'shipped' THEN amount ELSE 0 END) AS s, SUM(CASE WHEN status = 'pending' THEN amount ELSE 0 END) AS p, SUM(CASE WHEN status = 'returned' THEN amount ELSE 0 END) AS r FROM bench_sales GROUP BY category_id ORDER BY category_id",
  'window' => "SELECT COUNT(*), SUM(amount) FROM (SELECT amount, ROW_NUMBER() OVER (PARTITION BY customer_id ORDER BY trans_date DESC, id DESC) AS rn FROM bench_sales) t WHERE rn = 1",
];
$nocache = fn($sql) => $engine === 'asql' ? preg_replace('/^SELECT /', 'SELECT SQL_NO_CACHE ', $sql, 1) : $sql;
foreach ($queries as $name => $sql) {
  $s = $nocache($sql);
  $q($s);
  $ts = [];
  for ($i = 0; $i < 5; $i++) { $t0 = now_ms(); $rows = $q($s); $ts[] = now_ms() - $t0; }
  $out['query'][$name] = ['median_ms' => median($ts), 'min_ms' => min($ts), 'max_ms' => max($ts), 'answer' => $rows];
  if ($engine === 'asql') {   // the same text again and again: what ASQL's result cache answers
    $q($sql); $q($sql);
    $ts = [];
    for ($i = 0; $i < 5; $i++) { $t0 = now_ms(); $q($sql); $ts[] = now_ms() - $t0; }
    $out['query_cached'][$name] = median($ts);
  }
}

// 3. point lookups: 10,000 random ids, one after another
mt_srand(7);
$ids = []; for ($i = 0; $i < 10000; $i++) $ids[] = mt_rand(1, 500000);
$ts = []; $t0 = now_ms();
foreach ($ids as $id) { $a = now_ms(); $q("SELECT id, customer_id, amount, status FROM bench_sales WHERE id = $id"); $ts[] = now_ms() - $a; }
$total = now_ms() - $t0;
$out['point'] = ['n' => 10000, 'p50_ms' => pct($ts, 50), 'p99_ms' => pct($ts, 99), 'qps' => 10000 / ($total / 1000)];

// 4. single-row updates: 2,000, autocommit
mt_srand(11);
$ts = []; $t0 = now_ms();
for ($i = 0; $i < 2000; $i++) { $id = mt_rand(1, 500000); $a = now_ms(); $q("UPDATE bench_sales SET amount = amount + 0.01 WHERE id = $id"); $ts[] = now_ms() - $a; }
$total = now_ms() - $t0;
$out['update'] = ['n' => 2000, 'p50_ms' => pct($ts, 50), 'p99_ms' => pct($ts, 99), 'tps' => 2000 / ($total / 1000)];

// storage the engine reports for the table
if ($engine === 'maria') {
  $r = $q("SELECT data_length, index_length FROM information_schema.TABLES WHERE table_schema = 'bench' AND table_name = 'bench_sales'");
  $out['storage'] = ['data_bytes' => (int)$r[0][0], 'index_bytes' => (int)$r[0][1]];
} elseif ($engine === 'cedar') {
  try { $out['storage'] = ['total_bytes' => (int)$q("SELECT pg_total_relation_size('bench_sales')")[0][0]]; }
  catch (Exception $e) { $out['storage_error'] = $e->getMessage(); }
} else {
  $q("FLUSH TABLES");
}
echo json_encode($out, JSON_PRETTY_PRINT), "\n";
