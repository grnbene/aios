<?php
// Shared by the CALL VIEW drivers: the connection, one CALL VIEW round trip, and a checker.
// RV_DSN / RV_USER / RV_PASS_FILE override the local defaults.
function rv_connect() {
    $pass = trim(file_get_contents(getenv('RV_PASS_FILE') ?: getenv('HOME') . '/Docker/mcp/build/asql-local-root/mysql.pass'));
    return new PDO(getenv('RV_DSN') ?: 'mysql:host=127.0.0.1;port=3307;dbname=summitholding', getenv('RV_USER') ?: 'root', $pass,
                   array(PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION));
}

// Returns array(meta row without the chunk + ms + bytes, the joined payload bytes).
function view($pdo, $opts, $sql) {
    $t = microtime(true);
    $st = $pdo->query("CALL VIEW " . $pdo->quote(json_encode((object)$opts)) . " " . $sql);
    $body = ''; $meta = null;
    while ($r = $st->fetch(PDO::FETCH_ASSOC)) { if (!$meta) $meta = $r; $body .= $r['chunk']; }
    $meta['ms'] = round((microtime(true) - $t) * 1000, 1);
    $meta['bytes'] = strlen($body);
    unset($meta['chunk']);
    return array($meta, $body);
}

$fails = 0;
function check($name, $ok, $detail = '') {
    printf("%-4s %s%s\n", $ok ? 'OK' : 'FAIL', $name, $detail !== '' ? "  ($detail)" : '');
    if (!$ok) $GLOBALS['fails']++;
}
