<?php
// Drives the v8 ERP's own model code against an ASQL wire port and times every
// statement it sends. No SQL is written here: the statements are whatever the
// ERP's models build. Usage: php erp-drive.php PORT [nocache]
error_reporting(E_ALL & ~E_DEPRECATED & ~E_NOTICE & ~E_WARNING);
define('V8', '/repos/v8/server/');
define('DIR_SYSTEM', V8 . 'system/');
define('DIR_APPLICATION', V8 . 'catalog/');
define('DB_PREFIX', '');
require DIR_SYSTEM . 'engine/registry.php';
require DIR_SYSTEM . 'engine/model.php';
require DIR_SYSTEM . 'engine/loader.php';
require DIR_SYSTEM . 'engine/proxy.php';
require DIR_SYSTEM . 'library/db/mysqli.php';

$port = (int)($argv[1] ?? 13401);
$nocache = in_array('nocache', $argv, true);

class TimedDB {
  public $log = [];
  private $a;
  private $nocache;
  function __construct($a, $nocache) { $this->a = $a; $this->nocache = $nocache; }
  function query($sql, $params = []) {
    if ($this->nocache) {
      // A CTE's main SELECT is the one the result cache keys on.
      if (preg_match('/^\s*WITH\s/i', $sql)) $sql = preg_replace('/\)\s*SELECT\s+(?!SQL_NO_CACHE)/i', ') SELECT SQL_NO_CACHE ', $sql, 1);
      else $sql = preg_replace('/^\s*SELECT\s/i', 'SELECT SQL_NO_CACHE ', $sql, 1);
    }
    if ($lf = getenv('LOG')) file_put_contents($lf, $sql . "\n;;;\n", FILE_APPEND);
    $t = hrtime(true);
    try { $r = $this->a->query($sql, $params); $err = null; }
    catch (\Throwable $e) { $r = null; $err = $e->getMessage(); }
    $this->log[] = [(hrtime(true) - $t) / 1e3, $sql, $err];
    if ($err !== null) throw new \Exception($err);
    return $r;
  }
  function escape($v) { return $this->a->escape($v); }
  function countAffected() { return $this->a->countAffected(); }
  function getLastId() { return $this->a->getLastId(); }
  function connected() { return true; }
}

// Anything the models read from config/session that this drive does not set is null.
class Bag { public $data = []; function get($k) { return $this->data[$k] ?? null; } function set($k, $v) { $this->data[$k] = $v; } function has($k) { return isset($this->data[$k]); } function __call($n, $a) { return null; } }

$raw = new DB\MySQLi(getenv('DBHOST') ?: '127.0.0.1', getenv('DBUSER') ?: 'root', getenv('DBPASS') ?: '', getenv('DBNAME') ?: 'summitholding', $port);
$db = new TimedDB($raw, $nocache);
$registry = new Registry();
$registry->set('db', $db);
$registry->set('config', new Bag());
$session = new Bag();
$registry->set('session', $session);
$registry->set('event', new Bag());
$registry->set('load', new Loader($registry));
$registry->set('request', new Bag());
require_once DIR_SYSTEM . 'library/weberp/b2b.php';
$registry->set('shop', new Weberp\B2b($registry));
$registry->get('load')->model('oms/orders');
$orders = $registry->get('model_oms_orders');

// Real values: a user who can see locations, and recent orders from the data.
$one = function ($sql) use ($raw) { $r = $raw->query($sql); return $r->row ? array_values($r->row)[0] : null; };
$user = $one("SELECT userid FROM locationusers WHERE canview = 1 LIMIT 1");
$session->data['userid'] = $user;
$recent = $raw->query("SELECT orderno, debtorno, fromstkloc FROM salesorders ORDER BY orderno DESC LIMIT 5")->rows;
$o = $recent[0];

$calls = [
  'getOpenOrders(user locations)' => fn() => $orders->getOpenOrders([]),
  'getOpenOrders(debtor)'         => fn() => $orders->getOpenOrders(['debtorno' => $o['debtorno']]),
  'getOpenOrders(location)'       => fn() => $orders->getOpenOrders(['loccode' => $o['fromstkloc']]),
  'getOpenOrder'                  => fn() => $orders->getOpenOrder($o['orderno']),
  'getOrderDetails'               => fn() => $orders->getOrderDetails($o['orderno']),
  'getOrderTotals'                => fn() => $orders->getOrderTotals($o['orderno']),
  'getOrderProductLines'          => fn() => $orders->getOrderProductLines($o['orderno']),
  'getPickingInvoiceLines'        => fn() => $orders->getPickingInvoiceLines($o['orderno']),
  'getDebtorDispatchNotes'        => fn() => $orders->getDebtorDispatchNotes($o['debtorno']),
  'getOpenCredits'                => fn() => $orders->getOpenCredits([]),
  'getOpenQuotes'                 => fn() => $orders->getOpenQuotes([]),
  'getDispatchOpenOrders'         => fn() => $orders->getDispatchOpenOrders([]),
];
$only = getenv('ONLY');
printf("user=%s order=%s debtor=%s loc=%s  %s\n", $user, $o['orderno'], $o['debtorno'], $o['fromstkloc'], $nocache ? 'SQL_NO_CACHE' : 'as sent');
$passes = (int)(getenv('PASSES') ?: 2);
$warm = [];
for ($pass = 1; $pass <= $passes; $pass++) {
echo "-- pass $pass\n";
$all = [];
foreach ($calls as $name => $fn) {
  if ($only && stripos($name, $only) === false) continue;
  $db->log = [];
  $t = hrtime(true);
  $rows = null; $err = '';
  try { $res = $fn(); $rows = is_array($res) ? count($res) : (is_null($res) ? 0 : 1); }
  catch (\Throwable $e) { $err = "  ERROR: " . substr($e->getMessage(), 0, 160) . " @" . basename($e->getFile()) . ":" . $e->getLine(); }
  $ms = (hrtime(true) - $t) / 1e6;
  $sq = array_sum(array_column($db->log, 0)) / 1e3;
  printf("%-30s %9.1f ms  %6d stmts  %9.1f ms in SQL  rows=%s%s\n", $name, $ms, count($db->log), $sq, $rows ?? '-', $err);
  if ($pass > 1) $warm[$name][] = $ms;
  foreach ($db->log as $l) { $all[] = [$l[0], $name, $l[1], $l[2]]; }
}
}
usort($all, fn($a, $b) => $b[0] <=> $a[0]);
echo "\nslowest statements:\n";
foreach (array_slice($all, 0, (int)(getenv('TOP') ?: 12)) as $l)
  printf("%9.1f ms  %-24s %s%s\n", $l[0] / 1e3, $l[1], substr(preg_replace('/\s+/', ' ', $l[2]), 0, 220), $l[3] ? "  ERR: " . $l[3] : '');
$errs = array_filter($all, fn($l) => $l[3]);
if ($errs) { echo "\nerrors:\n"; foreach (array_slice($errs, 0, 10) as $l) printf("  %s: %s\n    %s\n", $l[1], $l[3], substr(preg_replace('/\s+/', ' ', $l[2]), 0, 300)); }
if ($d = getenv('DUMP')) { $n = 0; foreach ($all as $l) file_put_contents("$d/stmt-" . (++$n) . ".sql", $l[2]); echo "dumped $n\n"; }

if ($warm) {
  echo "\nwarm median (passes 2.." . $passes . "):\n";
  foreach ($warm as $name => $v) { sort($v); printf("%-30s %9.2f ms\n", $name, $v[intdiv(count($v), 2)]); }
}
