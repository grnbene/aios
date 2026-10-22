<?php
// Drives every read method of the v8 ERP's model classes against an ASQL wire
// port - the statements are whatever the models build, with arguments taken
// from the data (a recent order, its debtor, a stock item, ...). Read-only on
// both sides: a statement that would write is refused before it is sent, so a
// model's write path ends there and the next method runs.
//   php erp-drive-all.php PORT DIR[,DIR...] [nocache]      e.g. weberp,report,customer
// env: DBHOST DBUSER DBPASS DBNAME, DUMP=dir (one file per distinct statement),
//      ONLY=substring (model/method filter), TOP=n (slowest statements shown)
error_reporting(E_ALL & ~E_DEPRECATED & ~E_NOTICE & ~E_WARNING);
ini_set('memory_limit', '3G');
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
$dirs = explode(',', $argv[2] ?? 'weberp');
$nocache = in_array('nocache', $argv, true);

class ReadOnlyRefused extends \Exception {}

class TimedDB {
  // A method called with empty filters may ask for every invoice ever: more
  // rows than this and the statement is run through a capping derived table,
  // reported as too big and left out of the dump (its full result would not
  // fit a comparison either).
  const CAP = 50000;
  public $log = [];
  public $refused = 0;
  public $capped = 0;
  private $a;
  private $nocache;
  function __construct($a, $nocache) { $this->a = $a; $this->nocache = $nocache; }
  function query($sql, $params = []) {
    if (!preg_match('/^\s*(SELECT|WITH|SHOW|DESCRIBE|EXPLAIN)\b/i', $sql)) { $this->refused++; throw new ReadOnlyRefused('write refused: ' . substr(preg_replace('/\s+/', ' ', $sql), 0, 80)); }
    if ($this->nocache) {
      if (preg_match('/^\s*WITH\s/i', $sql)) $sql = preg_replace('/\)\s*SELECT\s+(?!SQL_NO_CACHE)/i', ') SELECT SQL_NO_CACHE ', $sql, 1);
      else $sql = preg_replace('/^\s*SELECT\s(?!SQL_NO_CACHE)/i', 'SELECT SQL_NO_CACHE ', $sql, 1);
    }
    $orig = $sql;
    $wrap = preg_match('/^\s*(SELECT|WITH)\b/i', $sql) && !preg_match('/\bLIMIT\s+\d+\s*(,\s*\d+\s*)?;?\s*$/i', $sql);
    if ($wrap) $sql = 'SELECT * FROM (' . rtrim(trim($sql), '; ') . ') __cap LIMIT ' . (self::CAP + 1);
    $t = hrtime(true);
    try { $r = $this->a->query($sql, $params); $err = null; }
    catch (\Throwable $e) { $r = null; $err = $e->getMessage(); }
    $big = $wrap && $r && is_object($r) && count($r->rows) > self::CAP;
    if ($big) { $this->capped++; $r->rows = array_slice($r->rows, 0, self::CAP); }
    $this->log[] = [(hrtime(true) - $t) / 1e3, $orig, $err, $big];
    if ($err !== null) throw new \Exception($err);
    return $r;
  }
  function escape($v) { return $this->a->escape($v); }
  function countAffected() { return 0; }
  function getLastId() { return 0; }
  function connected() { return true; }
}

class Bag { public $data = []; function get($k) { return $this->data[$k] ?? null; } function set($k, $v) { $this->data[$k] = $v; } function has($k) { return isset($this->data[$k]); } function __call($n, $a) { return null; } }

$raw = new DB\MySQLi(getenv('DBHOST') ?: '127.0.0.1', getenv('DBUSER') ?: 'root', getenv('DBPASS') ?: '', getenv('DBNAME') ?: 'versioneight', $port);
$db = new TimedDB($raw, $nocache);
$registry = new Registry();
$registry->set('db', $db);
$config = new Bag();
$registry->set('config', $config);
$session = new Bag();
$registry->set('session', $session);
$registry->set('event', new Bag());
$registry->set('load', new Loader($registry));
$registry->set('request', new Bag());
$registry->set('cache', new Bag());
$registry->set('log', new Bag());
$registry->set('user', new Bag());
$registry->set('customer', new Bag());
require_once DIR_SYSTEM . 'library/weberp/b2b.php';
$registry->set('shop', new Weberp\B2b($registry));

// Real values from the data for the parameter names the models use.
$one = function ($sql) use ($raw) { try { $r = $raw->query($sql); return $r->row ? array_values($r->row)[0] : null; } catch (\Throwable $e) { return null; } };
$o = $raw->query("SELECT orderno, debtorno, branchcode, fromstkloc FROM salesorders ORDER BY orderno DESC LIMIT 1")->row;
$v = [
  'orderno' => $o['orderno'], 'order_id' => $o['orderno'], 'order' => $o['orderno'], 'oldid' => $o['orderno'], 'newid' => $o['orderno'],
  'debtorno' => $o['debtorno'], 'customer_id' => $o['debtorno'], 'customer' => $o['debtorno'], 'debtor' => $o['debtorno'],
  'branchcode' => $o['branchcode'], 'branch' => $o['branchcode'],
  'loccode' => $o['fromstkloc'], 'location' => $o['fromstkloc'], 'warehouse' => $o['fromstkloc'], 'loc' => $o['fromstkloc'],
  'stockid' => $one("SELECT stockid FROM salesorderdetails WHERE orderno = '{$o['orderno']}' LIMIT 1") ?? $one("SELECT stockid FROM stockmaster LIMIT 1"),
  'supplierno' => $one("SELECT supplierno FROM suppliers LIMIT 1"), 'supplierid' => $one("SELECT supplierno FROM suppliers LIMIT 1"), 'supplierId' => $one("SELECT supplierno FROM suppliers LIMIT 1"), 'supplier' => $one("SELECT supplierno FROM suppliers LIMIT 1"),
  'userid' => $one("SELECT userid FROM locationusers WHERE canview = 1 LIMIT 1"), 'user' => $one("SELECT userid FROM locationusers WHERE canview = 1 LIMIT 1"), 'user_id' => $one("SELECT userid FROM locationusers WHERE canview = 1 LIMIT 1"),
  'transno' => $one("SELECT MAX(transno) FROM debtortrans"), 'transid' => $one("SELECT MAX(id) FROM debtortrans"), 'id' => $one("SELECT MAX(id) FROM debtortrans"),
  'salesman' => $one("SELECT salesmancode FROM salesman LIMIT 1"), 'salesmancode' => $one("SELECT salesmancode FROM salesman LIMIT 1"),
  'categoryid' => $one("SELECT categoryid FROM stockcategory LIMIT 1"), 'category_id' => $one("SELECT categoryid FROM stockcategory LIMIT 1"),
  'date' => date('Y-m-d'), 'from' => date('Y-m-01'), 'to' => date('Y-m-d'), 'start' => date('Y-m-01'), 'end' => date('Y-m-d'),
  'fromdate' => date('Y-m-01'), 'todate' => date('Y-m-d'), 'startdate' => date('Y-m-01'), 'enddate' => date('Y-m-d'), 'datefrom' => date('Y-m-01'), 'dateto' => date('Y-m-d'),
  'year' => (int)date('Y'), 'month' => (int)date('m'), 'period' => (int)date('Ym'), 'periodno' => $one("SELECT MAX(periodno) FROM periods"),
  'limit' => 20, 'offset' => 0, 'page' => 1, 'start_row' => 0, 'rows' => 20,
  'type' => 'all', 'status' => 'all', 'term' => 'a', 'search' => 'a', 'q' => 'a', 'name' => 'a', 'code' => 'a', 'keyword' => 'a', 'query' => 'a',
  'currency' => 'USD', 'currcode' => 'USD', 'pline' => $one("SELECT DISTINCT productline FROM stockmaster LIMIT 1") ?? '',
  'truckid' => $one("SELECT MAX(truckid) FROM trucks") ?? 1, 'dispatchid' => $one("SELECT MAX(dispatchid) FROM dispatch") ?? 1,
  'batch_id' => 1, 'runId' => 1, 'groupid' => 1, 'typeid' => 1, 'parent_id' => 0, 'productline_id' => 1, 'manufacturer_id' => 1, 'product_id' => 1, 'custom_field_id' => 1,
];
$session->data['userid'] = $v['userid'];
$session->data['user_id'] = $v['userid'];
$session->data['customer_id'] = $v['customer_id'];
$config->data['config_language_id'] = 1;
$config->data['config_store_id'] = 0;
$config->data['config_customer_group_id'] = 1;

$skip = '/^(__|add|insert|update|delete|del|set|save|remove|create|edit|cancel|post|write|mark|clear|reset|apply|process|send|import|upload|sync|move|assign|release|close|commit|log|record|register|generate|rebuild|refresh|flush|archive|restore|purge|run|exec|execute|do|put|push|toggle|activate|deactivate|approve|reject|void|reverse|allocate|unallocate|adjust|transfer|receive|ship|dispatch|pick|pack|print|email|mail|notify|queue|schedule|finali[sz]e|confirm|complete|convert|copy|clone|duplicate|merge|split|recalc|recompute|reprice|repost|unpost|undo|redo|invoice|credit|pay|charge|refund|capture|authori[sz]e|lock|unlock|enable|disable|install|uninstall|migrate|seed|truncate|drop|alter|replace|increment|decrement|bump|touch|store|cache|init|setup|start|stop|kill|abort|rollback|begin|end|wipe|destroy|forget|expire|renew|reorder|resequence|rename|relink|attach|detach|bind|unbind|link|unlink)/i';

$arg_for = function (\ReflectionParameter $p) use ($v) {
  if ($p->isDefaultValueAvailable()) return $p->getDefaultValue();
  $n = $p->getName();
  if (array_key_exists($n, $v)) return $v[$n];
  $ln = strtolower($n);
  foreach ($v as $k => $val) if (strtolower($k) === $ln) return $val;
  $t = $p->getType();
  $tn = $t instanceof \ReflectionNamedType ? $t->getName() : '';
  if ($tn === 'array' || preg_match('/^(data|filter|filters|input|params|options|args|where|conditions|fields|columns|ids|items|lines|rows|post|get)$/i', $n)) return [];
  if ($tn === 'int' || $tn === 'float' || preg_match('/(id|no|count|qty|quantity|amount|number|num|limit|offset|page|days|year|month)$/i', $n)) return 1;
  if ($tn === 'bool') return false;
  if (preg_match('/date|from|to|start|end/i', $n)) return date('Y-m-d');
  return '';
};

$only = getenv('ONLY');
$routes = getenv('MODELS') ? array_map('trim', explode(',', getenv('MODELS'))) : [];   // MODELS=oms/orders,weberp/items: those routes only
$all = []; $called = 0; $errs = []; $refused = 0; $empty = 0; $models = 0;
foreach ($dirs as $dir) {
  foreach (glob(DIR_APPLICATION . 'model/' . trim($dir) . '/*.php') as $file) {
    $route = trim($dir) . '/' . basename($file, '.php');
    if ($routes && !in_array($route, $routes, true)) continue;
    if (preg_match('/[^a-zA-Z0-9_\/]/', $route)) continue;   // the loader drops other names
    $class = 'Model' . preg_replace('/[^a-zA-Z0-9]/', '', $route);
    // The file must declare the class the loader expects, and one no earlier file declared (a stale copy of a model
    // under another name is a fatal "cannot declare class" at include time, not an exception).
    if (!preg_match('/^\s*(?:final\s+|abstract\s+)?class\s+(\w+)/m', file_get_contents($file), $mm) || strcasecmp($mm[1], $class) !== 0 || class_exists($mm[1], false)) { $errs["load $route"] = 'skipped: declares ' . ($mm[1] ?? 'no class'); continue; }
    try { $registry->get('load')->model($route); } catch (\Throwable $e) { $errs["load $route"] = substr($e->getMessage(), 0, 120); continue; }
    if (!class_exists($class)) continue;
    $models++;
    $rc = new \ReflectionClass($class);
    foreach ($rc->getMethods(\ReflectionMethod::IS_PUBLIC) as $m) {
      if ($m->isStatic() || strcasecmp($m->getDeclaringClass()->getName(), $class) !== 0) continue;   // the loader lower-cases the route
      $name = $m->getName();
      if (preg_match($skip, $name)) continue;
      $label = "$route::$name";
      if ($only && stripos($label, $only) === false) continue;
      $args = [];
      try { foreach ($m->getParameters() as $p) { if ($p->isVariadic()) break; $args[] = $arg_for($p); } }
      catch (\Throwable $e) { continue; }
      $db->log = [];
      $t = hrtime(true);
      $err = '';
      try { $registry->get(str_replace('/', '_', 'model_' . $route))->{$name}(...$args); }
      catch (ReadOnlyRefused $e) { $refused++; $err = 'refused'; }
      catch (\Throwable $e) { $err = substr(preg_replace('/\s+/', ' ', $e->getMessage()), 0, 140); }
      $ms = (hrtime(true) - $t) / 1e6;
      $called++;
      if (!$db->log) $empty++;
      foreach ($db->log as $l) $all[] = [$l[0], $label, $l[1], $l[2], $l[3]];
      $sql_errs = array_filter($db->log, fn($l) => $l[2]);
      if ($sql_errs) { $errs[$label] = $sql_errs[array_key_first($sql_errs)][2]; }
      elseif ($err && $err !== 'refused' && !str_starts_with($err, 'write refused')) $errs[$label] = 'php: ' . $err;
      if ($ms > 2000) printf("%-60s %8.0f ms  %d stmts\n", $label, $ms, count($db->log));
    }
  }
}
$distinct = [];
foreach ($all as $l) if (!$l[4]) $distinct[md5($l[2])] = $l;
printf("\n%d models, %d methods called, %d issued no statement, %d ended at a refused write; %d statements, %d distinct, %d too big (over %d rows, left out)\n",
       $models, $called, $empty, $refused, count($all), count($distinct), $db->capped, TimedDB::CAP);
usort($all, fn($a, $b) => $b[0] <=> $a[0]);
echo "\nslowest statements:\n";
foreach (array_slice($all, 0, (int)(getenv('TOP') ?: 15)) as $l)
  printf("%9.1f ms  %-40s %s\n", $l[0] / 1e3, substr($l[1], 0, 40), substr(preg_replace('/\s+/', ' ', $l[2]), 0, 160));
$sql_errs = array_filter($all, fn($l) => $l[3]);
$by = [];
foreach ($sql_errs as $l) { $k = preg_replace('/[\'"][^\'"]*[\'"]|\d+/', '_', substr($l[3], 0, 70)); $by[$k][] = $l; }
if ($by) { printf("\nSQL errors (%d statements, %d kinds):\n", count($sql_errs), count($by)); foreach ($by as $k => $ls) printf("  %4d  %s\n        %s\n        %s\n", count($ls), $ls[0][3], $ls[0][1], substr(preg_replace('/\s+/', ' ', $ls[0][2]), 0, 240)); }
$php = array_filter($errs, fn($e) => str_starts_with($e, 'php:'));
if ($php) { printf("\nPHP errors in %d methods (first 10):\n", count($php)); foreach (array_slice($php, 0, 10, true) as $k => $e) printf("  %s: %s\n", $k, $e); }
if ($d = getenv('DUMP')) { $n = 0; foreach ($distinct as $l) file_put_contents("$d/stmt-" . (++$n) . ".sql", $l[2]); echo "dumped $n\n"; }
