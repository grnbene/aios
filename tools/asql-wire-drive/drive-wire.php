<?php
// Drives one server (ASQL or MariaDB) through the real client paths the ERP
// and its tools use, and prints one line per check with a value that can be
// diffed between the two servers. Usage: php drive-wire.php host port user pass [socket]
mysqli_report(MYSQLI_REPORT_OFF);
[$_, $host, $port, $user, $pass] = $argv; $sock = $argv[5] ?? null;
function conn($host, $port, $user, $pass, $sock, $flags = 0, $persistent = false) {
  $c = mysqli_init();
  if (!@$c->real_connect(($persistent ? 'p:' : '') . $host, $user, $pass, 'wdrv', (int)$port, $sock, $flags)) { echo "CONNECT FAIL " . mysqli_connect_error() . "\n"; exit(1); }
  return $c;
}
$c = conn($host, $port, $user, $pass, $sock);
$out = [];
$say = function ($k, $v) { echo $k, ' = ', (is_string($v) ? $v : json_encode($v)), "\n"; };
// Every result use goes through these so one refused statement prints an ERR line instead of ending the drive.
function rows($r, $mode = MYSQLI_ASSOC) { return $r instanceof mysqli_result ? $r->fetch_all($mode) : ($r === true ? 'OK(no result)' : 'ERR'); }
function row($r) { return $r instanceof mysqli_result ? $r->fetch_row() : 'ERR'; }
function sres($s) { if (!$s) return 'PREPARE-ERR'; $r = $s->get_result(); return $r ? $r->fetch_all(MYSQLI_NUM) : 'ERR ' . $s->errno . ' ' . $s->error; }
set_error_handler(function ($no, $str, $file, $line) { echo "WARN line $line: $str\n"; return true; });

// 1. text protocol, types, NULLs, decimals, binary, dates - twice (miss then cached hit)
$q = "SELECT orderno, debtorno, orddate, total, status, comment, HEX(blob_col) h, created FROM orders WHERE orderno IN (1,5,10,4999) ORDER BY orderno";
for ($i = 0; $i < 2; $i++) { $r = $c->query($q); $say("text$i", rows($r)); }
// column metadata
$r = $c->query($q); $meta = [];
foreach (($r ? $r->fetch_fields() : []) as $f) $meta[] = [$f->name, $f->type, $f->charsetnr, $f->decimals, $f->flags & 0x80 ? 'bin' : 'txt'];
$say("meta", $meta);

// 2. prepared statement (binary protocol), twice, with bound params of several types
$s = $c->prepare("SELECT orderno, total, orddate, comment, status FROM orders WHERE orderno = ? OR debtorno = ? ORDER BY orderno LIMIT 3");
$a = 10; $b = 'D5';
for ($i = 0; $i < 2; $i++) { $s->bind_param('is', $a, $b); $s->execute(); $say("prep$i", sres($s)); }
// same prepared statement, different params: must not get the first answer
$a = 4999; $b = 'nope'; $s->bind_param('is', $a, $b); $s->execute(); $say("prep_other", sres($s));
// binary result via bind_result (no get_result): the pure binary row path
$s2 = $c->prepare("SELECT orderno, total, orddate, created FROM orders WHERE orderno = ?"); $x = 7; $s2->bind_param('i', $x); $s2->execute();
$s2->bind_result($o, $t, $d, $cr); $s2->fetch(); $say("bind_result", [$o, $t, $d, $cr]); $s2->close();
// aggregate through prepared
$s3 = $c->prepare("SELECT COUNT(*), SUM(qty), MAX(price) FROM olines WHERE orderno = ?"); $x = 17; $s3->bind_param('i', $x); $s3->execute(); $say("prep_agg", sres($s3));
// long data
$s4 = $c->prepare("SELECT LENGTH(?) l, LEFT(?, 5) p"); $big = str_repeat('x', 70000); $s4->bind_param('bb', $n1, $n2); $s4->send_long_data(0, $big); $s4->send_long_data(1, 'hello world'); $s4->execute(); $say("long_data", sres($s4));

// 3. write then read on the same connection: cached answers must refresh
$c->query("UPDATE orders SET comment = 'changed-by-drive' WHERE orderno = 5");
$r = $c->query($q); $x = rows($r); $say("after_update", is_array($x) ? $x[1]['comment'] : $x);
$s->bind_param('is', $a, $b); $a = 5; $b = 'zz'; $s->execute(); $x = sres($s); $say("prep_after_update", is_array($x) ? $x[0][3] : $x);
$c->query("UPDATE orders SET comment = 'note 5' WHERE orderno = 5");

// 4. multi-statement
$c2 = conn($host, $port, $user, $pass, $sock, 0);
$ok = $c2->multi_query("SELECT 1 AS a; SELECT orderno FROM orders WHERE orderno < 3 ORDER BY orderno; SELECT COUNT(*) c FROM notes");
$sets = [];
do { if ($r = $c2->store_result()) { $sets[] = $r->fetch_all(MYSQLI_NUM); $r->free(); } } while ($c2->more_results() && $c2->next_result());
$say("multi", $sets);
// error mid-way ends the reply
$c2->multi_query("SELECT 1; SELECT * FROM nope; SELECT 2"); $sets = []; do { if ($r = $c2->store_result()) { $sets[] = $r->fetch_all(MYSQLI_NUM); } } while ($c2->more_results() && @$c2->next_result());
$say("multi_err", [$sets, $c2->errno]);
$c2->close();

// 5. charset: utf8mb4 round trip, and a latin1 session
$say("utf8", rows($c->query("SELECT id, txt, LENGTH(txt) l, CHAR_LENGTH(txt) cl FROM notes ORDER BY id"), MYSQLI_NUM));
$c->set_charset('latin1'); $rows = rows($c->query("SELECT id, txt FROM notes ORDER BY id"), MYSQLI_NUM); if (is_array($rows)) foreach ($rows as &$rw) $rw[1] = bin2hex($rw[1]); $say("latin1_hex", $rows);
$rows = rows($c->query("SELECT id, txt FROM notes ORDER BY id"), MYSQLI_NUM); if (is_array($rows)) foreach ($rows as &$rw) $rw[1] = bin2hex($rw[1]); $say("latin1_hex_hit", $rows);
$c->set_charset('utf8mb4'); $say("utf8_again", rows($c->query("SELECT id, txt FROM notes ORDER BY id"), MYSQLI_NUM));

// 5b. character functions on every path: projection, WHERE (compiled predicate), ORDER BY, and a prepared statement
$say("char_len_proj", rows($c->query("SELECT id, LENGTH(txt) b, CHAR_LENGTH(txt) ch, CHARACTER_LENGTH(txt) ch2, OCTET_LENGTH(txt) o FROM notes ORDER BY id"), MYSQLI_NUM));
$say("char_len_where", rows($c->query("SELECT id FROM notes WHERE CHAR_LENGTH(txt) < 6 ORDER BY id"), MYSQLI_NUM));
$say("char_len_order", rows($c->query("SELECT id FROM notes ORDER BY CHAR_LENGTH(txt), id"), MYSQLI_NUM));
$s5 = $c->prepare("SELECT CHAR_LENGTH(txt), LENGTH(txt) FROM notes WHERE id = ?"); $x = 2; $s5->bind_param('i', $x); $s5->execute(); $say("char_len_prep", sres($s5));
// SHOW CREATE TABLE: the column lines only (MariaDB adds ENGINE/CHARSET after the closing paren)
$x = row($c->query("SHOW CREATE TABLE olines")); $say("info_show_create", is_array($x) ? preg_replace('/\s+/', ' ', $x[1]) : $x);
$say("repeat_20mb", is_array($x = row($c->query("SELECT LENGTH(REPEAT('ab', 10000000))"))) ? $x[0] : $x);

// 6. errors and sqlstate
@$c->query("SELECT nope FROM orders"); $say("err_col", [$c->errno, $c->sqlstate]);
@$c->query("SELECT * FROM nope"); $say("err_tbl", [$c->errno, $c->sqlstate]);
@$c->query("SELEC 1"); $say("err_syntax", [$c->errno, $c->sqlstate]);
@$c->query("SELECT o.x FROM orders o JOIN olines l ON l.orderno = o.orderno WHERE o.orderno = 1"); $say("err_unknown_qualified", [$c->errno, $c->sqlstate, substr($c->error, 0, 40)]);

// 7. reset / change user (what p: connections do), ping, statistics
$say("reset", [$c->query("SET @v = 1") !== false, row($c->query("SELECT @v")), $c->errno]);
$say("ping", $c->ping());
$say("stat_has_uptime", strpos((string)$c->stat(), 'Uptime') !== false);
$say("change_user", @$c->change_user($user, $pass, 'wdrv'));
$say("after_change_user", row($c->query("SELECT COUNT(*) FROM orders")));
$c->close();
// persistent connection reuse: two connects in a row with p:
$p1 = conn($host, $port, $user, $pass, $sock, 0, true); $p1->query("SET @keep = 42"); $p1->close();
$p2 = conn($host, $port, $user, $pass, $sock, 0, true); $say("persistent_reset_clears_vars", row($p2->query("SELECT @keep"))); $p2->close();

// 8. big result and big packet
$c = conn($host, $port, $user, $pass, $sock);
$r = $c->query("SELECT l.orderlineno, l.orderno, l.stockid, l.qty, l.price, o.debtorno FROM olines l JOIN orders o ON o.orderno = l.orderno ORDER BY l.orderno, l.orderlineno"); $rows = rows($r, MYSQLI_NUM); $say("big_join", is_array($rows) ? [count($rows), md5(serialize($rows))] : $rows);
$r = $c->query("SELECT REPEAT('a', 17000000) big"); $x = row($r); $say("big_packet", is_array($x) ? strlen($x[0]) : $x . " " . $c->error);
$c->close();
