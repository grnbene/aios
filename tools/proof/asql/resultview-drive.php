<?php
// Drives CALL VIEW against a live ASQL through PDO and checks each mode against a plain SELECT.
//   php resultview-drive.php            (RV_SQL overrides the statement; see resultview-lib.php)
require __DIR__ . '/resultview-lib.php';
$pdo = rv_connect();
$sql = getenv('RV_SQL') ?: "select * from locstock where bin<>''";

$t = microtime(true);
$direct = $pdo->query($sql)->fetchAll(PDO::FETCH_NUM);
$direct_ms = round((microtime(true) - $t) * 1000, 1);
$cols = array();
$st = $pdo->query($sql . ' limit 1');
for ($i = 0; $i < $st->columnCount(); $i++) { $m = $st->getColumnMeta($i); $cols[] = $m['name']; }
echo "statement: $sql\ndirect select+fetchAll: " . count($direct) . " rows x " . count($cols) . " cols in {$direct_ms} ms\n\n";

list($m1, $b1) = view($pdo, array(), $sql);
$j1 = json_decode($b1, true);
echo "build: " . json_encode($m1) . "\n";
check('full payload is valid JSON', is_array($j1), json_last_error_msg());
check('columns match', $j1['columns'] === $cols);
check('rows identical to direct SELECT (values, NULLs, order)', $j1['rows'] === $direct, count($j1['rows']) . ' vs ' . count($direct));
check('recordsTotal', $j1['recordsTotal'] === count($direct));

list($m2, $b2) = view($pdo, array(), $sql);
echo "hit:   " . json_encode($m2) . "\n";
check('second call byte-identical', $b2 === $b1);
check('version stable', $m2['version'] === $m1['version'], $m1['version']);

list($m3, $b3) = view($pdo, array('if_version' => $m1['version']), $sql);
check('if_version -> not_modified, no payload', $m3['status'] === 'not_modified' && $m3['bytes'] === 0, json_encode($m3));
list($m3b, $b3b) = view($pdo, array('if_version' => 'stale'), $sql);
check('wrong version -> full payload', $m3b['status'] === 'ok' && $b3b === $b1);

list($m4, $b4) = view($pdo, array('gzip' => true), $sql);
echo "gzip:  " . json_encode($m4) . " ratio " . round(strlen($b1) / max(1, strlen($b4)), 1) . "x\n";
check('gzip decodes to the same payload', $m4['encoding'] === 'gzip' && gzdecode($b4) === $b1);

$orders = array(
    array(array(1, 'asc'), array(0, 'asc')),
    array(array(2, 'desc'), array(1, 'asc'), array(0, 'asc')),
);
foreach ($orders as $ord) {
    $by = implode(', ', array_map(function ($o) use ($cols) { return $cols[$o[0]] . ' ' . $o[1]; }, $ord));
    foreach (array(array(0, 100), array(5000, 50), array(count($direct) - 17, 100)) as $w) {
        list($mw, $bw) = view($pdo, array('draw' => 7, 'window' => array('start' => $w[0], 'length' => $w[1], 'order' => $ord)), $sql);
        $jw = json_decode($bw, true);
        $ref = $pdo->query("$sql order by $by limit {$w[1]} offset {$w[0]}")->fetchAll(PDO::FETCH_NUM);
        check("window order by $by, start {$w[0]} len {$w[1]}", $jw['rows'] === $ref && $jw['draw'] === 7,
              count($jw['rows']) . " rows, {$mw['ms']} ms");
    }
}

foreach (array('BRAD', 'a1', 'zzqqnotthere', 'rack 12') as $q) {
    list($ms, $bs) = view($pdo, array('window' => array('start' => 0, 'length' => -1, 'search' => $q)), $sql);
    $js = json_decode($bs, true);
    $words = preg_split('/\s+/', strtolower(trim($q)));
    $expect = array_values(array_filter($direct, function ($row) use ($words) {
        $hay = strtolower(implode("\x1f", array_map(function ($v) { return $v === null ? '' : $v; }, $row)));
        foreach ($words as $w) if (strpos($hay, $w) === false) return false;
        return true;
    }));
    check("search '$q'", $js['rows'] === $expect && $js['recordsFiltered'] === count($expect),
          "{$js['recordsFiltered']} of {$js['recordsTotal']}, {$ms['ms']} ms");
}

$pdo->exec("drop table if exists rv_text");
$pdo->exec("create table rv_text (id int primary key, t varchar(60))");
$samples = array(1 => "plain", 2 => "\xC3\x83\xE2\x80\x94", 3 => "caf\xC3\xA9", 4 => "\xE2\x80\x9Cquote\xE2\x80\x9D", 5 => "line\xE2\x80\xA8sep");
foreach ($samples as $id => $t) $pdo->exec("insert into rv_text values ($id, " . $pdo->quote($t) . ")");
foreach (array('utf8', 'latin1', 'latin1_iso') as $mode) {
    list($mt, $bt) = view($pdo, array('text' => $mode), "select * from rv_text order by id");
    $jt = json_decode($bt, true);
    $ok = is_array($jt);
    foreach ($samples as $id => $t) {
        $session = mb_convert_encoding($t, 'Windows-1252', 'UTF-8');
        if ($mode === 'utf8') $want = $t;
        elseif ($mode === 'latin1_iso') $want = utf8_encode($session);
        else $want = mb_check_encoding($session, 'UTF-8') ? $session : mb_convert_encoding($session, 'UTF-8', 'Windows-1252');
        if (!$ok || $jt['rows'][$id - 1][1] !== $want) { $ok = false; echo "  $mode id $id: got " . bin2hex($jt['rows'][$id - 1][1]) . " want " . bin2hex($want) . "\n"; }
    }
    check("text mode $mode", $ok, $mode === 'latin1' ? 'double-encoded row reads as ' . $jt['rows'][1][1] : '');
}
$pdo->exec("drop table if exists rv_text");

$st = $pdo->query("CALL VIEW STATUS")->fetchAll(PDO::FETCH_ASSOC);
echo "\nCALL VIEW STATUS:\n";
foreach ($st as $r) echo '  ' . json_encode($r) . "\n";
echo "\n" . ($fails ? "$fails FAILED" : "all checks passed") . "\n";
exit($fails ? 1 : 0);
