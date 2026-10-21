<?php
// Runs the v8 ERP's own B2b::createInvoice against an ASQL (or MariaDB) port, for one order.
//   php drive.php PORT ORDERNO [FAIL_AT=<statement substring>] [TAG=name]
// FAIL_AT makes the first statement containing that text fail (as a dropped connection or a crash would), to see
// what an invoice leaves behind when it stops part way.
error_reporting(E_ALL & ~E_DEPRECATED & ~E_NOTICE & ~E_WARNING);
$V8 = getenv('V8') ?: '/repos/v8/server/';
define('DIR_SYSTEM', $V8 . 'system/');
define('DIR_APPLICATION', $V8 . 'catalog/');
define('DIR_LOGS', sys_get_temp_dir() . '/');
define('DB_PREFIX', '');
require DIR_SYSTEM . 'engine/registry.php';
require DIR_SYSTEM . 'engine/model.php';
require DIR_SYSTEM . 'engine/loader.php';
require DIR_SYSTEM . 'engine/proxy.php';
require DIR_SYSTEM . 'library/db/mysqli.php';
$port = (int)$argv[1];
$orderno = $argv[2];
$opt = [];
foreach (array_slice($argv, 3) as $a) if (preg_match('/^([A-Z_]+)=(.*)$/', $a, $m)) $opt[$m[1]] = $m[2];
$tag = $opt['TAG'] ?? "o$orderno";

class LoggedDB {
  public $log = [];
  private $a; private $fail; private $tag; private $rtt;
  function __construct($a, $fail, $tag, $rtt = 0) { $this->a = $a; $this->fail = $fail; $this->tag = $tag; $this->rtt = $rtt; }
  function query($sql, $params = []) {
    if ($this->fail !== null && $this->fail !== '' && stripos($sql, $this->fail) !== false) {
      $this->fail = null;
      $this->log[] = ['FAILED (injected)', $sql];
      throw new \Exception("injected failure at: " . substr(preg_replace('/\s+/', ' ', $sql), 0, 80));
    }
    if ($this->rtt) usleep($this->rtt);   // the web server's round trip to the database
    $t = hrtime(true);
    try { $r = $this->a->query($sql, $params); }
    catch (\Throwable $e) { $this->log[] = ['ERROR ' . $e->getMessage(), $sql]; throw $e; }
    $this->log[] = [sprintf('%.2fms', (hrtime(true) - $t) / 1e6), $sql];
    return $r;
  }
  function escape($v) { return $this->a->escape((string)$v); }
  function countAffected() { return $this->a->countAffected(); }
  function getLastId() { return $this->a->getLastId(); }
  function connected() { return true; }
}
class Bag { public $data = []; function get($k) { return $this->data[$k] ?? null; } function set($k, $v) { $this->data[$k] = $v; } function has($k) { return isset($this->data[$k]); } function __call($n, $a) { return null; } }

$raw = new DB\MySQLi('127.0.0.1', getenv('DBUSER') ?: 'root', getenv('DBPASS') ?: 'secret', 'summitholding', $port);
$db = new LoggedDB($raw, $opt['FAIL_AT'] ?? null, $tag, (int)($opt['RTT_US'] ?? 0));
$registry = new Registry();
$registry->set('db', $db);
$config = new Bag();
$config->data = ['decimalplaces' => 2, 'BackOrdersAllowed' => 0, 'PicksDictateInvoice' => 1, 'metrc_enabled' => 0];
$registry->set('config', $config);
$session = new Bag();
$session->data = ['userid' => 'admin', 'date_format' => 'm/d/Y', 'location' => 'MAIN'];
$registry->set('session', $session);
$registry->set('event', new Bag());
$registry->set('load', new Loader($registry));
$registry->set('request', new Bag());
require_once DIR_SYSTEM . 'library/weberp/sales.php';
$registry->set('sales', new Weberp\Sales($registry));
require_once DIR_SYSTEM . 'library/weberp/b2b.php';
$shop = new Weberp\B2b($registry);

// The order as the invoice screen hands it over: header, lines to dispatch, totals.
$h = $raw->query("SELECT * FROM salesorders WHERE orderno = '$orderno'")->row;
$lines = $raw->query("SELECT d.*, d.stkcode AS stockid, d.quantity AS ordered, d.quantity - d.qtyinvoiced AS dispatch, d.qtyinvoiced AS invoiced,
                             s.materialcost, s.labourcost, s.overheadcost, s.decimalplaces, s.mbflag, s.categoryid
                      FROM salesorderdetails d JOIN stockmaster s ON s.stockid = d.stkcode WHERE d.orderno = '$orderno'")->rows;
$sub = 0;
foreach ($lines as &$l) { $l['taxes'] = []; $l['fetvalue'] = 0; $sub += $l['unitprice'] * $l['dispatch'] * (1 - $l['discountpercent']); }
unset($l);
$order = [
  'order' => ['header' => $h + ['coupon_used' => ''], 'details' => $lines, 'tax_data' => ['authorities' => [], 'total' => 0]],
  'totals' => ['subtotal' => round($sub, 2), 'tax' => 0, 'freight' => 0, 'discount' => 0, 'adjustment' => 0, 'fet' => 0, 'total' => round($sub, 2)],
  'userid' => 'admin', 'document_form' => [],
];
$t = hrtime(true);
try { $r = $shop->createInvoice($order); $out = is_array($r) ? json_encode($r) : var_export($r, true); }
catch (\Throwable $e) { $out = 'THREW ' . get_class($e) . ': ' . $e->getMessage() . ' @' . basename($e->getFile()) . ':' . $e->getLine(); }
$ms = (hrtime(true) - $t) / 1e6;
printf("[%s] order %s: %s (%.0f ms, %d statements)\n", $tag, $orderno, $out, $ms, count($db->log));
if (!empty($opt['LOG'])) foreach ($db->log as $e) printf("  %-28s %s\n", $e[0], substr(preg_replace('/\s+/', ' ', $e[1]), 0, 150));
else foreach ($db->log as $e) if (strncmp($e[0], 'ERROR', 5) === 0 || strncmp($e[0], 'FAILED', 6) === 0) printf("  %s\n    %s\n", $e[0], substr(preg_replace('/\s+/', ' ', $e[1]), 0, 300));
