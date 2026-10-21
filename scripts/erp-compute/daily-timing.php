<?php
/**
 * Time the daily reorder report ("daily churn") exactly as the API runs it:
 * bootstrap the framework, load the model, build the input the way
 * purchasing/reorder::daily() does from the screen's default form, call
 * getDaily(). Reports wall time, queries issued, rows returned and a checksum
 * of the numeric outputs so a later run can be compared for identity.
 *
 *   V8_SERVER=/var/www/api V8_DB=versioneight php daily-timing.php [loccode=01] [days_supply=60] [stockid,stockid,...]
 */
$SRV = getenv('V8_SERVER') ?: '/var/www/v8/server';
$DBN = getenv('V8_DB') ?: 'versioneight';
chdir($SRV);
require_once $SRV . '/config.php';
require_once DIR_SYSTEM . 'startup.php';
require_once DIR_SYSTEM . 'weberp-framework.php';
$config->set('db_database', $DBN);
$registry->set('db', new DB($config->get('db_type'), $config->get('db_hostname'), $config->get('db_username'), $config->get('db_password'), $DBN, $config->get('db_port')));
// Every statement the report issues, timed and grouped by shape, so the
// seconds can be attributed to SQL or to PHP.
class TimingDB {
	public $inner; public $stats = array(); public $total_ms = 0.0; public $count = 0;
	public function __construct($inner) { $this->inner = $inner; }
	public function query($sql) {
		$t = microtime(true); $r = $this->inner->query($sql); $ms = (microtime(true) - $t) * 1000;
		$k = preg_replace(array("/'[^']*'/", "/\b\d+(\.\d+)?\b/", "/\s+/"), array("'?'", "N", " "), trim($sql));
		$k = substr($k, 0, 110);
		if (!isset($this->stats[$k])) $this->stats[$k] = array('n' => 0, 'ms' => 0.0);
		$this->stats[$k]['n']++; $this->stats[$k]['ms'] += $ms; $this->total_ms += $ms; $this->count++;
		return $r;
	}
	public function __call($m, $a) { return call_user_func_array(array($this->inner, $m), $a); }
}
$timing = new TimingDB($registry->get('db'));
$registry->set('db', $timing);
$db = $registry->get('db');
$session = $registry->get('session');
$session->data['userid'] = 'JACK';
$loader->model('oms/purchases');
$loader->model('oms/orders');
$loader->model('weberp/purchasing');
$model = $registry->get('model_weberp_purchasing');

$loccode = $argv[1] ?? '01';
$daysSupply = (int) ($argv[2] ?? 60);
$stockids = isset($argv[3]) ? array_filter(array_map('trim', explode(',', $argv[3]))) : array();

// The screen's default form, then the controller's munging.
$input = array(
	'startdate' => date('Y-m-d', strtotime('-365 days')), 'enddate' => date('Y-m-d'),
	'minimum_sales' => 0, 'item_type' => 1, 'numberofmonths' => 2, 'numberofweeks' => 8,
	'loccode' => $loccode, 'filters' => array(), 'brands' => array(), 'season' => array(), 'lines' => array(),
	'supplier' => '', 'days_supply' => $daysSupply,
);
if ($stockids) $input['stockids'] = array_values($stockids);
$d1 = new DateTime($input['enddate']); $d2 = new DateTime($input['startdate']);
$Months = $d2->diff($d1);
$input['sales_periods'] = '';
$input['daysbetween'] = $d2->diff($d1)->format('%r%a');
$input['monthsbetween'] = (($Months->y) * 12) + ($Months->m);

$snap = $db->query("SELECT COUNT(*) c, MAX(updated_at) u FROM reorder_daily_snapshot")->row;
$hist = $db->query("SELECT COUNT(*) c FROM reorder_snapshot_history")->row['c'];
$seas = $db->query("SELECT COUNT(*) c FROM reorder_snapshot_seasonal")->row['c'];
printf("snapshot: %s rows (updated %s), history %s, seasonal %s  | input: %s..%s loccode=%s days_supply=%d%s\n", $snap['c'], $snap['u'], $hist, $seas, $input['startdate'], $input['enddate'], $loccode, $daysSupply, $stockids ? ' stockids=' . count($stockids) : '');

$q0 = (int) $db->query("SHOW GLOBAL STATUS LIKE 'Questions'")->row['Value'];
$t0 = microtime(true);
$results = $model->getDaily($input);
$ms = (microtime(true) - $t0) * 1000;
$q1 = (int) $db->query("SHOW GLOBAL STATUS LIKE 'Questions'")->row['Value'];

$rows = is_array($results) ? $results : array();
$n = 0; $sum = array('avg_day' => 0.0, 'days_supply' => 0.0, 'target_avg_day' => 0.0);
$walk = function ($x) use (&$walk, &$n, &$sum) {
	if (!is_array($x)) return;
	if (isset($x['avg_day']) && isset($x['days_supply'])) { $n++; foreach ($sum as $k => $v) $sum[$k] += (float) ($x[$k] ?? 0); return; }
	foreach ($x as $v) $walk($v);
};
$walk($rows);
printf("getDaily: %.0f ms wall, of which SQL %.0f ms in %d statements (PHP %.0f ms); %d item rows, json %.1f MB, peak %.0f MB\nchecksum: avg_day=%.6f days_supply=%.6f target=%.0f\n", $ms, $timing->total_ms, $timing->count, $ms - $timing->total_ms, $n, strlen(json_encode($rows)) / 1048576, memory_get_peak_usage(true) / 1048576, $sum['avg_day'], $sum['days_supply'], $sum['target_avg_day']);
uasort($timing->stats, function ($a, $b) { return $b['ms'] <=> $a['ms']; });
echo "top statements by time:\n";
$i = 0; foreach ($timing->stats as $k => $s) { printf("  %7.0f ms  x%-6d %s\n", $s['ms'], $s['n'], $k); if (++$i >= 10) break; }
