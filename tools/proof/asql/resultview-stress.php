<?php
// CALL VIEW under concurrency: `reader <seconds>` hammers random windows, searches and
// full payloads; `writer <seconds>` churns rv_stress (a table one of the views reads);
// `setup` / `verify` bracket a run. The shell runs several readers and one writer at once.
require __DIR__ . '/resultview-lib.php';
$pdo = rv_connect();
$mode = isset($argv[1]) ? $argv[1] : 'verify';
$secs = isset($argv[2]) ? (float)$argv[2] : 20;
$big = "select * from locstock where bin<>''";
$churn = "select * from rv_stress order by id";

if ($mode === 'setup') {
    $pdo->exec("drop table if exists rv_stress");
    $pdo->exec("create table rv_stress (id int primary key, v varchar(20))");
    for ($i = 1; $i <= 200; $i++) $pdo->exec("insert into rv_stress values ($i, 'v$i')");
    echo "setup done\n";
    exit(0);
}
if ($mode === 'writer') {
    $n = 0; $end = microtime(true) + $secs;
    while (microtime(true) < $end) {
        $id = mt_rand(1, 400);
        $pdo->exec(mt_rand(0, 1) ? "replace into rv_stress values ($id, 'w" . mt_rand() . "')" : "delete from rv_stress where id = $id");
        $n++;
    }
    echo "writer: $n writes\n";
    exit(0);
}
if ($mode === 'reader') {
    $n = 0; $err = 0; $end = microtime(true) + $secs;
    while (microtime(true) < $end) {
        try {
            $pick = mt_rand(0, 9);
            if ($pick < 5) {
                $opts = array('window' => array('start' => mt_rand(0, 11000), 'length' => 100,
                              'order' => array(array(mt_rand(0, 10), mt_rand(0, 1) ? 'asc' : 'desc'))));
                list($m, $b) = view($pdo, $opts, $big);
            } elseif ($pick < 7) {
                list($m, $b) = view($pdo, array('window' => array('start' => 0, 'length' => 50, 'search' => chr(mt_rand(97, 122)) . mt_rand(0, 9))), $big);
            } elseif ($pick < 8) {
                list($m, $b) = view($pdo, array('gzip' => (bool)mt_rand(0, 1)), $big);
            } else {
                list($m, $b) = view($pdo, array(), $churn);
            }
            if ($m['encoding'] === 'identity' && json_decode($b, true) === null) $err++;
            $n++;
        } catch (Exception $e) { $err++; fwrite(STDERR, $e->getMessage() . "\n"); }
    }
    echo "reader: $n calls, $err errors\n";
    exit($err ? 1 : 0);
}
list($m, $b) = view($pdo, array(), $churn);
$j = json_decode($b, true);
$direct = $pdo->query($churn)->fetchAll(PDO::FETCH_NUM);
check('after the churn the view equals a direct SELECT', $j['rows'] === $direct, count($direct) . ' rows');
$pdo->exec("drop table if exists rv_stress");
echo ($fails ? "$fails FAILED" : "verify passed") . "\n";
exit($fails ? 1 : 0);
