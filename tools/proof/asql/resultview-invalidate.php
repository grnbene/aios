<?php
// CALL VIEW invalidation: writes to a table a view reads rebuild it (new version),
// writes elsewhere leave it cached, and a rebuild that changed nothing keeps its version.
// Uses scratch tables rv_probe / rv_other, created and dropped here.
require __DIR__ . '/resultview-lib.php';
$pdo = rv_connect();

function rows_of($pdo, $sql, $opts = array()) {
    list($meta, $body) = view($pdo, $opts, $sql);
    return array($meta, json_decode($body, true));
}
function builds($pdo) {
    foreach ($pdo->query("CALL VIEW STATUS")->fetchAll(PDO::FETCH_ASSOC) as $r)
        if ($r['statement'] === '(total)') return (int)substr($r['age_s'], 7);
    return -1;
}

$pdo->exec("drop table if exists rv_probe");
$pdo->exec("drop table if exists rv_other");
$pdo->exec("create table rv_probe (id int primary key, name varchar(40), qty int)");
$pdo->exec("create table rv_other (id int primary key)");
$pdo->exec("insert into rv_probe values (1,'alpha',5),(2,'beta',7),(3,'gamma',null)");
$q = "select * from rv_probe order by id";

list($m0, $j0) = rows_of($pdo, $q);
$b0 = builds($pdo);
check('initial view', count($j0['rows']) === 3 && $j0['rows'][2][2] === null, $m0['version']);

$pdo->exec("insert into rv_other values (1)");
list($m1, $j1) = rows_of($pdo, $q);
check('write to an unrelated table keeps it cached', $m1['version'] === $m0['version'] && builds($pdo) === $b0);

$pdo->exec("insert into rv_probe values (4,'delta',9)");
list($m2, $j2) = rows_of($pdo, $q);
check('insert rebuilds with a new version', $m2['version'] !== $m0['version'] && count($j2['rows']) === 4 && builds($pdo) === $b0 + 1,
      $m2['version']);
list($m2b, $j2b) = rows_of($pdo, $q, array('if_version' => $m0['version']));
check('if_version of the old rows -> full new payload', $m2b['status'] === 'ok' && count($j2b['rows']) === 4);

$pdo->exec("update rv_probe set qty = 70 where id = 2");
list($m3, $j3) = rows_of($pdo, $q);
check('update is seen', $j3['rows'][1][2] === '70' && $m3['version'] !== $m2['version']);

$pdo->exec("update rv_probe set qty = 70 where id = 2");
list($m4, $j4) = rows_of($pdo, $q);
check('a write that changed nothing keeps the version', $m4['version'] === $m3['version'], 'rebuilt: ' . (builds($pdo) > $b0 + 2 ? 'yes' : 'no'));

$pdo->exec("delete from rv_probe where id = 1");
list($m5, $j5) = rows_of($pdo, $q);
check('delete is seen', count($j5['rows']) === 3 && $j5['rows'][0][0] === '2');

$pdo->exec("drop table rv_probe");
$threw = false;
try { rows_of($pdo, $q); } catch (PDOException $e) { $threw = true; }
check('dropped table -> error, not a stale answer', $threw);

$pdo->exec("drop table if exists rv_other");
echo "\n" . ($fails ? "$fails FAILED" : "all checks passed") . "\n";
exit($fails ? 1 : 0);
