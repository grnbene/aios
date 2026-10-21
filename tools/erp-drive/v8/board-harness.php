<?php
// Runs the dashboard's two open-order model calls through the real v8 models
// and the real DB proxy, counting and timing every query by the model method
// that issued it. Prints method names, counts and times only - no values.
$V = '/var/www/v8/server';
if (getenv('BOARD_SOCK')) define('AIOS_ASQL_SOCKET', getenv('BOARD_SOCK'));
define('AIOS_COMPUTE_ENABLED', true); define('AIOS_GLOBAL_DB_PROXY', true); define('AIOS_ASQL_READS', true);
if (!defined('DIR_CACHE')) define('DIR_CACHE', '/tmp/board-cache/');
error_reporting(E_ALL & ~E_NOTICE & ~E_WARNING);
chdir($V);
define('DIR_SYSTEM', $V . '/system/');
if (getenv('ASQL_READS') === '1') {   // this harness only; config_aios.php's defines then no-op
    define('AIOS_COMPUTE_ENABLED', true); define('AIOS_GLOBAL_DB_PROXY', true); define('AIOS_ASQL_READS', true);
}
require "$V/config_aios.php";
require "$V/system/library/aioscompute.php";
require "$V/system/engine/registry.php";
require "$V/system/engine/model.php";
require "$V/system/library/db.php";
require "$V/system/library/db/mysqli.php";

class CountingDB extends DB {
    public static $by = array(); public static $n = 0; public static $t = 0.0; public static $src = array();
    public function query($sql, $params = array()) {
        $bt = debug_backtrace(DEBUG_BACKTRACE_IGNORE_ARGS, 4);
        $who = '?';
        foreach ($bt as $i => $f) {
            if ($i === 0) continue;
            if (isset($f['class']) && $f['class'] !== 'CountingDB' && $f['class'] !== 'DB') { $who = $f['class'] . '::' . $f['function']; break; }
        }
        $t0 = microtime(true);
        $r = parent::query($sql, $params);
        $dt = microtime(true) - $t0;
        $src = $this->lastSource();
        static $seen_first = false;
        if (!$seen_first && getenv('ASQL_READS') === '1' && $src === 'db') {
            $seen_first = true;
            $rp = new ReflectionProperty('DB', 'skip_reason'); $rp->setAccessible(true);
            $sk = $rp->getValue($this);
            // the shape of the statement (literals out), and why it went to MariaDB
            $shape = preg_replace(array("/'[^']*'/", '/\b\d+(\.\d+)?\b/', '/\s+/'), array('?', '?', ' '), $sql);
            fprintf(STDERR, "FIRST-FALLBACK reason=[%s] err=[%s] sql=%s\n", $sk, $this->lastError(), substr($shape, 0, 400));
        }
        if (!isset(self::$src[$src])) self::$src[$src] = array(0, 0.0);
        self::$src[$src][0]++; self::$src[$src][1] += $dt;
        if (!isset(self::$by[$who])) self::$by[$who] = array(0, 0.0);
        self::$by[$who][0]++; self::$by[$who][1] += $dt; self::$n++; self::$t += $dt;
        return $r;
    }
}
class Loader {
    private $registry;
    public function __construct($r) { $this->registry = $r; }
    public function model($route) {
        $key = 'model_' . str_replace('/', '_', $route);
        if ($this->registry->has($key)) return;
        $file = $GLOBALS['V'] . '/catalog/model/' . $route . '.php';
        if ($route === 'oms/orders' && getenv('ORDERS_FILE')) $file = getenv('ORDERS_FILE');
        if ($route === 'oms/openorders_thing' && getenv('THING_FILE')) $file = getenv('THING_FILE');
        require_once $file;
        $class = 'Model' . preg_replace('/[^a-zA-Z0-9]/', '', ucwords($route, '/_'));
        $this->registry->set($key, new $class($this->registry));
    }
}
class Cfg {
    public $d = array();
    public function get($k) { return isset($this->d[$k]) ? $this->d[$k] : null; }
    public function set($k, $v) { $this->d[$k] = $v; }
    public function has($k) { return isset($this->d[$k]); }
}
class Sess { public $data = array('userid' => 'JACK'); }
class Req { public $get = array('loccode' => null); public $post = array(); public $request = array(); public $server = array(); }

$cfgSrc = file_get_contents("$V/config.php");   // DB_* for the real connection, as the app reads them
preg_match("/\\\$host\s*=\s*'([^']*)'/", $cfgSrc, $h);
$pw = trim(file_get_contents('/opt/aios/mysql.pass'));
$db = new CountingDB('MySQLi', 'localhost', 'root', $pw, 'summitholding', '3306');
$reg = new Registry();
$cfg = new Cfg();
// The live page's config: OpenCart `setting` (store 0), then webERP `config`.
foreach ($db->query("SELECT * FROM setting WHERE store_id = '0'")->rows as $r) $cfg->set($r['key'], !empty($r['serialized']) ? json_decode($r['value'], true) : $r['value']);
foreach ($db->query("SELECT confname, confvalue FROM config")->rows as $r) $cfg->set($r['confname'], $r['confvalue']);
CountingDB::$by = array(); CountingDB::$n = 0; CountingDB::$t = 0.0; CountingDB::$src = array();
$reg->set('db', $db); $reg->set('config', $cfg); $reg->set('session', new Sess()); $reg->set('request', new Req());
$reg->set('load', new Loader($reg));
require_once "$V/system/library/weberp/sales.php";
require_once "$V/system/library/weberp/b2b.php";
$reg->set('sales', new Weberp\Sales($reg));
$reg->set('shop', new Weberp\B2b($reg));
$reg->get('load')->model('oms/orders');
$reg->get('load')->model('weberp/salesman');
$m = $reg->get('model_oms_orders');

$filters = array('loccode' => null);
$sc = $reg->get('model_weberp_salesman')->getSalesmanCodeByUser('JACK');
if ($sc != '' && isset($sc)) $filters['salesman'] = $sc;
$reg->get('load')->model('oms/openorders_thing');
$th = $reg->get('model_oms_openorders_thing');
$get = array('thing' => 1, 'loccode' => null);
function ksort_deep($v) { if (!is_array($v)) return $v; foreach ($v as $k => $x) $v[$k] = ksort_deep($x); if (array_keys($v) !== range(0, count($v) - 1)) ksort($v); return $v; }
function norm($e) { unset($e['snoozed_details']); return ksort_deep($e); }
function kid($e) { return ((isset($e['invoiced_dispatch']) && $e['invoiced_dispatch']) ? 'i:' : 'o:') . $e['orderno']; }
function keyed($list) { $o = array(); foreach ($list as $e) $o[kid($e)] = md5(serialize(norm($e))); return $o; }
if (getenv('BOARD_MODE') === 'version') {
  $fast = $th->results($filters, $get);
  echo $fast === null ? "NULL " . $th->lastError() : $th->version(), "\n";
  exit;
}
if (getenv('BOARD_MODE') === 'changes') {
  $t0 = microtime(true); $c = $th->changes($filters, $get, (string) getenv('BOARD_SINCE')); $t1 = microtime(true);
  if ($c === null) { echo "NULL ", $th->lastError(), "\n"; exit; }
  printf("%s version=%s %.0f ms", $c['status'], substr($c['version'], 0, 10), ($t1 - $t0) * 1000);
  if ($c['status'] === 'changed') {
    $gone = array_diff($c['since_ids'], $c['ids']); $new = array_diff($c['ids'], $c['since_ids']);
    printf(" ids=%d since=%d gone=[%s] new=[%s] entries_changed=[%s]", count($c['ids']), count($c['since_ids']), implode(',', $gone), implode(',', $new), implode(',', array_keys((array) $c['entries'])));
  }
  echo "\n"; exit;
}
for ($run = 1; $run <= 2; $run++) {
  CountingDB::$n = 0; CountingDB::$t = 0.0; CountingDB::$src = array();
  $t0 = microtime(true); $fast = $th->results($filters, $get); $t1 = microtime(true);
  printf("fast run %d: %s, %d entries, %.0f ms, %d DB queries (%.0f ms in DB), version %s\n", $run, $fast === null ? 'NULL (' . $th->lastError() . ')' : 'served', is_array($fast) ? count($fast) : 0, ($t1 - $t0) * 1000, CountingDB::$n, CountingDB::$t * 1000, substr((string) $th->version(), 0, 10));
}
CountingDB::$n = 0; CountingDB::$t = 0.0;
$t0 = microtime(true); $o = $m->getOpenOrdersInvDispatch($filters); $i = $m->getInvoicedOnDispatch($filters); $slow = $m->combine(is_array($o) ? $o : array(), is_array($i) ? $i : array()); $t1 = microtime(true);
printf("fallback: %d entries (%d open + %d inv), %.0f ms, %d DB queries\n", count($slow), count($o), count($i), ($t1 - $t0) * 1000, CountingDB::$n);
if (is_array($fast)) {
  $a = keyed($fast); $b = keyed($slow);
  $only_a = array_diff_key($a, $b); $only_b = array_diff_key($b, $a); $diff = 0; foreach ($a as $k => $v) if (isset($b[$k]) && $b[$k] !== $v) $diff++;
  printf("compare: only_fast=%d only_fallback=%d differing=%d  same_order=%s\n", count($only_a), count($only_b), $diff, array_keys($a) === array_keys($b) ? 'yes' : 'no');
  foreach (array_slice(array_keys($only_a), 0, 3) as $k) echo "  only fast: $k\n";
  foreach (array_slice(array_keys($only_b), 0, 3) as $k) echo "  only fallback: $k\n";
  // which fields differ, over every entry both paths list
  $fa = array(); $fb = array();
  foreach ($fast as $e) $fa[kid($e)] = norm($e);
  foreach ($slow as $e) $fb[kid($e)] = norm($e);
  $fields = array(); $examples = array();
  foreach ($fa as $k => $ea) {
    if (!isset($fb[$k])) continue;
    $eb = $fb[$k];
    foreach (array_unique(array_merge(array_keys($ea), array_keys($eb))) as $f) {
      $va = $ea[$f] ?? null; $vb = $eb[$f] ?? null;
      if ($f === 'details' && is_array($va) && is_array($vb)) {
        foreach (array_unique(array_merge(array_keys($va), array_keys($vb))) as $g) {
          if (($va[$g] ?? null) !== ($vb[$g] ?? null)) { $fields["details.$g"] = ($fields["details.$g"] ?? 0) + 1; if (!isset($examples["details.$g"])) $examples["details.$g"] = array($k, $va[$g] ?? null, $vb[$g] ?? null); }
        }
        continue;
      }
      if ($va !== $vb) { $fields[$f] = ($fields[$f] ?? 0) + 1; if (!isset($examples[$f])) $examples[$f] = array($k, $va, $vb); }
    }
  }
  arsort($fields);
  printf("fields differing (of %d common entries): %d\n", count(array_intersect_key($fa, $fb)), count($fields));
  foreach (array_slice($fields, 0, 25, true) as $f => $n) { $ex = $examples[$f]; printf("  %5d  %-28s e.g. %s: fast=%s fallback=%s\n", $n, $f, $ex[0], substr(json_encode($ex[1]), 0, 70), substr(json_encode($ex[2]), 0, 70)); }
  $shown = 0;
  foreach ($a as $k => $v) {
    if (!isset($b[$k]) || $b[$k] === $v || $shown++ >= 0) continue;
    $fa = $fb = null; foreach ($fast as $e) if (kid($e) === $k) $fa = norm($e); foreach ($slow as $e) if (kid($e) === $k) $fb = norm($e);
    foreach ($fa as $f => $v2) if (!array_key_exists($f, $fb) || $fb[$f] !== $v2) echo "  $k.$f: fast=" . substr(json_encode($v2), 0, 80) . " fallback=" . substr(json_encode($fb[$f] ?? null), 0, 80) . "\n";
  }
}
