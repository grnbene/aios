<?php
/**
 * AiosCompute — the ERP's client for the aios compute service.
 *
 * One TCP request per call, framed as a 4-byte big-endian length followed by
 * JSON, answered the same way. SQL reads go over ASQL's MySQL unix socket
 * instead when AIOS_ASQL_SOCKET is set (see wire()). Every method returns null when the service is
 * absent, slow, or unhappy, so a caller can always fall back to the PHP path
 * it already has. Nothing here throws.
 *
 * Settings come from config_aios.php beside config.php (see that file); the
 * defaults below are what a fresh install gets.
 *
 * PHP 7.4 compatible.
 */
class AiosCompute {

	const DEFAULT_HOST        = '127.0.0.1';
	const DEFAULT_PORT        = 7311;
	const DEFAULT_CONNECT_MS  = 250;
	const DEFAULT_DEADLINE_MS = 1500;
	const DEFAULT_MAX_STOPS   = 13;

	private static $loaded = false;
	private static $lastError = '';

	private static function settings() {
		if (!self::$loaded) {
			self::$loaded = true;
			$cfg = __DIR__ . '/../../config_aios.php';
			if (is_file($cfg)) {
				include_once $cfg;
			}
		}
	}

	public static function enabled() {
		self::settings();
		return defined('AIOS_COMPUTE_ENABLED') ? (bool) AIOS_COMPUTE_ENABLED : true;
	}

	/** Stops (excluding the start) up to which the exact solver is asked. */
	public static function maxExactStops() {
		self::settings();
		return defined('AIOS_EXACT_MAX_STOPS') ? (int) AIOS_EXACT_MAX_STOPS : self::DEFAULT_MAX_STOPS;
	}

	public static function deadlineMs() {
		self::settings();
		return defined('AIOS_COMPUTE_DEADLINE_MS') ? (int) AIOS_COMPUTE_DEADLINE_MS : self::DEFAULT_DEADLINE_MS;
	}

	public static function lastError() {
		return self::$lastError;
	}

	/**
	 * Exact open-path route: the start first, then every stop, no return leg.
	 *
	 * @param array $start  ['id' => string, 'lat' => float, 'lng' => float]
	 * @param array $stops  list of the same shape
	 * @param array $opts   'deadline_ms' => int, 'prune' => bool
	 * @return array|null   ['order' => [ids, start first], 'distance_miles' => float,
	 *                       'evaluated' => int, 'pruned' => int, 'elapsed_ms' => float,
	 *                       'device' => string, 'lanes' => int, 'cached' => bool]
	 *                      or ['status' => 'pending'] when the deadline passed first,
	 *                      or null when the service could not answer.
	 */
	public static function tspExact(array $start, array $stops, array $opts = array()) {
		if (!self::enabled()) {
			self::$lastError = 'disabled';
			return null;
		}
		$deadline = isset($opts['deadline_ms']) ? (int) $opts['deadline_ms'] : self::deadlineMs();
		$req = array(
			'op'          => 'tsp_exact',
			'start'       => array('id' => (string) $start['id'], 'lat' => (float) $start['lat'], 'lng' => (float) $start['lng']),
			'stops'       => array(),
			'deadline_ms' => $deadline,
			'prune'       => isset($opts['prune']) ? (bool) $opts['prune'] : true,
		);
		foreach ($stops as $s) {
			$req['stops'][] = array('id' => (string) $s['id'], 'lat' => (float) $s['lat'], 'lng' => (float) $s['lng']);
		}
		$res = self::call($req, $deadline + 500);
		if ($res === null) {
			return null;
		}
		if (empty($res['ok'])) {
			self::$lastError = isset($res['error']) ? (string) $res['error'] : 'service refused';
			return null;
		}
		if (isset($res['status']) && $res['status'] === 'pending') {
			return array('status' => 'pending');
		}
		if (empty($res['order']) || !is_array($res['order'])) {
			self::$lastError = 'malformed answer';
			return null;
		}
		return array(
			'status'         => 'done',
			'order'          => array_map('strval', $res['order']),
			'distance_miles' => (float) $res['distance_miles'],
			'evaluated'      => isset($res['evaluated']) ? (int) $res['evaluated'] : 0,
			'pruned'         => isset($res['pruned']) ? (int) $res['pruned'] : 0,
			'elapsed_ms'     => isset($res['elapsed_ms']) ? (float) $res['elapsed_ms'] : 0.0,
			'device'         => isset($res['device']) ? (string) $res['device'] : '',
			'lanes'          => isset($res['lanes']) ? (int) $res['lanes'] : 0,
			'cached'         => !empty($res['cached']),
		);
	}

	/**
	 * The sales route builder's optimizer — nearest-neighbour seed, then 2-opt
	 * and Or-opt — run by the service. Same moves, same acceptance, same miles;
	 * the order comes back as the ids given.
	 *
	 * @param array       $start   ['lat' => float, 'lng' => float]
	 * @param string      $endMode 'home' (return to start), 'last' (one-way) or 'custom'
	 * @param array|null  $end     ['lat' => float, 'lng' => float] when $endMode is 'custom'
	 * @param array       $stops   list of ['id' => string, 'lat' => float, 'lng' => float]
	 * @return array|null ['order' => [ids], 'total_distance_miles' => float, 'evaluated' => int,
	 *                     'passes' => int, 'elapsed_ms' => float, 'device' => string] or
	 *                    ['status' => 'pending'], or null when the service could not answer.
	 */
	public static function routeImprove(array $start, $endMode, $end, array $stops, array $opts = array()) {
		if (!self::enabled()) {
			self::$lastError = 'disabled';
			return null;
		}
		$deadline = isset($opts['deadline_ms']) ? (int) $opts['deadline_ms'] : self::deadlineMs();
		$req = array(
			'op'          => 'route_improve',
			'start'       => array('lat' => (float) $start['lat'], 'lng' => (float) $start['lng']),
			'end_mode'    => in_array($endMode, array('home', 'last', 'custom'), true) ? $endMode : 'home',
			'stops'       => array(),
			'deadline_ms' => $deadline,
		);
		if ($req['end_mode'] === 'custom') {
			if (!is_array($end) || !isset($end['lat'], $end['lng'])) {
				$req['end_mode'] = 'home';
			} else {
				$req['end'] = array('lat' => (float) $end['lat'], 'lng' => (float) $end['lng']);
			}
		}
		foreach ($stops as $i => $s) {
			$req['stops'][] = array('id' => isset($s['id']) ? (string) $s['id'] : (string) $i, 'lat' => (float) $s['lat'], 'lng' => (float) $s['lng']);
		}
		$res = self::call($req, $deadline + 500);
		if ($res === null) {
			return null;
		}
		if (empty($res['ok'])) {
			self::$lastError = isset($res['error']) ? (string) $res['error'] : 'service refused';
			return null;
		}
		if (isset($res['status']) && $res['status'] === 'pending') {
			return array('status' => 'pending');
		}
		if (!isset($res['order']) || !is_array($res['order'])) {
			self::$lastError = 'malformed answer';
			return null;
		}
		return array(
			'status'               => 'done',
			'order'                => array_map('strval', $res['order']),
			'total_distance_miles' => (float) $res['total_distance_miles'],
			'evaluated'            => isset($res['evaluated']) ? (int) $res['evaluated'] : 0,
			'passes'               => isset($res['passes']) ? (int) $res['passes'] : 0,
			'elapsed_ms'           => isset($res['elapsed_ms']) ? (float) $res['elapsed_ms'] : 0.0,
			'device'               => isset($res['device']) ? (string) $res['device'] : '',
			'cached'               => !empty($res['cached']),
		);
	}

	/**
	 * The OMS board's ordering of a whole open-order pool: from the depot,
	 * repeatedly the nearest unvisited order — getshortest()'s walk, with its
	 * tie rule, at pool sizes exact search cannot take.
	 *
	 * @return array|null ['order' => [ids], 'legs' => [miles from previous], 'total_miles' => float,
	 *                     'elapsed_ms' => float, 'device' => string] or null when the service could not answer.
	 */
	public static function poolGreedy(array $start, array $stops, array $opts = array()) {
		if (!self::enabled()) {
			self::$lastError = 'disabled';
			return null;
		}
		$deadline = isset($opts['deadline_ms']) ? (int) $opts['deadline_ms'] : self::deadlineMs();
		$req = array(
			'op'          => 'pool_greedy',
			'start'       => array('id' => (string) $start['id'], 'lat' => (float) $start['lat'], 'lng' => (float) $start['lng']),
			'stops'       => array(),
			'deadline_ms' => $deadline,
		);
		foreach ($stops as $s) {
			$req['stops'][] = array('id' => (string) $s['id'], 'lat' => (float) $s['lat'], 'lng' => (float) $s['lng']);
		}
		$res = self::call($req, $deadline + 500);
		if ($res === null) {
			return null;
		}
		if (empty($res['ok']) || !isset($res['order']) || !is_array($res['order'])) {
			self::$lastError = isset($res['error']) ? (string) $res['error'] : 'malformed answer';
			return null;
		}
		if (isset($res['status']) && $res['status'] === 'pending') {
			return array('status' => 'pending');
		}
		return array(
			'status'      => 'done',
			'order'       => array_map('strval', $res['order']),
			'legs'        => array_map('floatval', isset($res['legs']) ? $res['legs'] : array()),
			'total_miles' => (float) $res['total_miles'],
			'elapsed_ms'  => isset($res['elapsed_ms']) ? (float) $res['elapsed_ms'] : 0.0,
			'device'      => isset($res['device']) ? (string) $res['device'] : '',
			'cached'      => !empty($res['cached']),
		);
	}

	/** Liveness and identity of the service, or null. */
	public static function ping() {
		$res = self::call(array('op' => 'ping'), 1000);
		return ($res && !empty($res['ok'])) ? $res : null;
	}

	/**
	 * A read straight off the aios in-memory tables (asql) this instance was
	 * started against — the same data `aios sql query` sees, without the
	 * kernel round trip a shell command needs. Reads only: the service itself
	 * refuses anything that isn't SELECT/SHOW/DESCRIBE/EXPLAIN/WITH, and this
	 * checks the same thing first so a mistaken write fails locally instead of
	 * spending a round trip to be told no. There is no write path here on
	 * purpose — see the compute_serve.cpp "sql" op notes for why.
	 *
	 * @param string $sql
	 * @param array  $opts 'deadline_ms' => int, 'db' => string,
	 *                     'assoc' => true for ['assoc' => [[name => value], ...]]
	 *                     in place of columns + positional rows
	 * @return array|null ['columns' => [...], 'rows' => [[...], ...], 'row_count' => int]
	 *                     or null when the service could not answer, was not
	 *                     built with a database attached, or the statement
	 *                     was not a read.
	 */
	public static function query($sql, array $opts = array()) {
		if (!self::enabled()) {
			self::$lastError = 'disabled';
			return null;
		}
		if (!preg_match('/^\s*(SELECT|SHOW|DESCRIBE|DESC|EXPLAIN|WITH)\b/i', $sql)) {
			self::$lastError = 'reads only: SELECT / SHOW / DESCRIBE / EXPLAIN / WITH';
			return null;
		}
		$deadline = isset($opts['deadline_ms']) ? (int) $opts['deadline_ms'] : self::deadlineMs();
		$db = isset($opts['db']) ? (string) $opts['db'] : '';
		$wire = self::wire($db, $deadline);
		if ($wire !== null) {
			return self::wireQuery($wire, $sql, !empty($opts['assoc']));
		}
		$req = array('op' => 'sql', 'query' => (string) $sql);
		// The caller's database: the engine answers in it, and refuses one it
		// does not hold rather than answer from another tenant's mirror.
		if (isset($opts['db']) && (string) $opts['db'] !== '') {
			$req['db'] = (string) $opts['db'];
		}
		$res = self::call($req, $deadline + 500);
		if ($res === null) {
			return null;
		}
		if (empty($res['ok'])) {
			self::$lastError = isset($res['error']) ? (string) $res['error'] : 'service refused';
			return null;
		}
		if (!isset($res['columns']) || !is_array($res['columns']) || !isset($res['rows']) || !is_array($res['rows'])) {
			self::$lastError = 'malformed answer';
			return null;
		}
		$columns = array_map('strval', $res['columns']);
		if (!empty($opts['assoc'])) {
			$assoc = array();
			foreach ($res['rows'] as $positional) {
				$row = array();
				foreach ($columns as $i => $name) {
					$row[$name] = isset($positional[$i]) ? $positional[$i] : null;
				}
				$assoc[] = $row;
			}
			return array('assoc' => $assoc, 'row_count' => count($assoc));
		}
		return array(
			'columns'   => $columns,
			'rows'      => $res['rows'],
			'row_count' => isset($res['row_count']) ? (int) $res['row_count'] : count($res['rows']),
		);
	}

	/**
	 * The MySQL wire server's unix socket (AIOS_ASQL_SOCKET), one connection
	 * kept for the rest of the request. A read over the RPC port opened a new
	 * TCP connection and round-tripped JSON every time: 300-600 us a statement
	 * against ~10 us here, on pages that issue 100k+ reads.
	 *
	 * Only for a database listed in AIOS_ASQL_DATABASES: the wire server takes
	 * any database name it is given, where the RPC op refuses one ASQL does not
	 * hold. Null (and the RPC port is used) when no socket is configured, the
	 * database is not listed, or the socket cannot be reached.
	 */
	private static $wire = null;
	private static $wireDb = '';
	private static $wireDown = false;
	private static $wireHeld = null;

	private static function wire($db, $deadlineMs) {
		self::settings();
		if (self::$wireDown || !defined('AIOS_ASQL_SOCKET') || AIOS_ASQL_SOCKET === '') {
			return null;
		}
		if (self::$wire !== null && $db === self::$wireDb) {
			return self::$wire;
		}
		if (self::$wireHeld === null) {
			self::$wireHeld = defined('AIOS_ASQL_DATABASES') ? array_map('trim', explode(',', AIOS_ASQL_DATABASES)) : array();
		}
		$held = self::$wireHeld;
		if ($db === '') {
			$db = isset($held[0]) ? $held[0] : '';
		}
		if ($db === '' || !in_array($db, $held, true)) {
			return null;
		}
		if (self::$wire === null) {
			$c = mysqli_init();
			$c->options(MYSQLI_OPT_CONNECT_TIMEOUT, 1);
			// The connection outlives the call that opened it, so its read timeout cannot be
			// that call's deadline: a 1.5 s deadline on a lookup closed the connection under a
			// later whole-Thing read (60 MB). A generous constant; the RPC path keeps per-call deadlines.
			$c->options(MYSQLI_OPT_READ_TIMEOUT, max(60, (int) ceil($deadlineMs / 1000)));
			$user = defined('AIOS_ASQL_USER') ? AIOS_ASQL_USER : 'asql';
			$pass = defined('AIOS_ASQL_PASS') ? AIOS_ASQL_PASS : '';
			if (!@$c->real_connect('localhost', $user, $pass, $db, 0, AIOS_ASQL_SOCKET)) {
				self::$lastError = 'connect ' . AIOS_ASQL_SOCKET . ' failed: ' . mysqli_connect_error();
				self::$wireDown = true;
				return null;
			}
			// The engine's text is UTF-8 and the ERP speaks utf8; asked for nothing, the
			// session takes the server's announced default (latin1 where it mirrors a latin1
			// MariaDB) and every high byte comes back as cp1252 - JSON documents then fail to decode.
			$c->set_charset('utf8mb4');
			self::$wire = $c;
			self::$wireDb = $db;
		} elseif (self::$wireDb !== $db) {
			if (!@self::$wire->select_db($db)) {
				self::$lastError = 'cannot use ' . $db . ': ' . self::$wire->error;
				return null;
			}
			self::$wireDb = $db;
		}
		return self::$wire;
	}

	private static function wireQuery($c, $sql, $assoc = false) {
		$r = @$c->query((string) $sql);
		if ($r === false) {
			self::$lastError = $c->error !== '' ? $c->error : 'no answer';
			// Gone away / lost mid-answer / timed out: the connection is unusable,
			// and the rest of this request goes to the RPC port or MariaDB.
			if ($c->errno == 2006 || $c->errno == 2013 || $c->errno == 2014 || $c->errno == 0) {
				@$c->close();
				self::$wire = null;
				self::$wireDown = true;
			}
			return null;
		}
		if ($r === true) {
			self::$lastError = 'not a result set';
			return null;
		}
		// Named rows straight from mysqlnd: building a field object per column
		// (fetch_fields) and then re-keying every row cost more than the read
		// on the page's wide SELECT * joins.
		if ($assoc) {
			$rows = $r->fetch_all(MYSQLI_ASSOC);
			$r->free();
			return array('assoc' => $rows, 'row_count' => count($rows));
		}
		$columns = array();
		foreach ($r->fetch_fields() as $f) {
			$columns[] = (string) $f->name;
		}
		$rows = $r->fetch_all(MYSQLI_NUM);
		$r->free();
		return array('columns' => $columns, 'rows' => $rows, 'row_count' => count($rows));
	}

	// Only this process sees both halves of a bounce — the ASQL refusal and the
	// MariaDB retry land in different servers. Best-effort, short deadline.
	public static function traceBounce($sql, $reason, $durationMs = 0.0) {
		if (!self::enabled()) {
			return false;
		}
		$prevError = self::$lastError;
		$res = self::call(array(
			'op'          => 'trace',
			'query'       => (string) $sql,
			'reason'      => (string) $reason,
			'origin'      => 'erp',
			'duration_ms' => (float) $durationMs,
		), 250);
		// A transport failure here overwrites lastError, destroying the very
		// reason being traced.
		self::$lastError = $prevError;
		return is_array($res) && !empty($res['ok']);
	}

	/** One framed request, one framed answer, decoded. */
	private static function call(array $req, $readTimeoutMs) {
		self::settings();
		$host = defined('AIOS_COMPUTE_HOST') ? AIOS_COMPUTE_HOST : self::DEFAULT_HOST;
		$port = defined('AIOS_COMPUTE_PORT') ? (int) AIOS_COMPUTE_PORT : self::DEFAULT_PORT;
		$connectMs = defined('AIOS_COMPUTE_CONNECT_MS') ? (int) AIOS_COMPUTE_CONNECT_MS : self::DEFAULT_CONNECT_MS;
		if (defined('AIOS_COMPUTE_TOKEN') && AIOS_COMPUTE_TOKEN !== '') {
			$req['token'] = AIOS_COMPUTE_TOKEN;
		}

		$errno = 0;
		$errstr = '';
		$fp = @fsockopen($host, $port, $errno, $errstr, $connectMs / 1000.0);
		if (!$fp) {
			self::$lastError = 'connect ' . $host . ':' . $port . ' failed: ' . $errstr;
			return null;
		}
		stream_set_timeout($fp, (int) floor($readTimeoutMs / 1000), ($readTimeoutMs % 1000) * 1000);

		$body = json_encode($req);
		$frame = pack('N', strlen($body)) . $body;
		$written = 0;
		$len = strlen($frame);
		while ($written < $len) {
			$n = @fwrite($fp, substr($frame, $written));
			if ($n === false || $n === 0) {
				self::$lastError = 'write failed';
				fclose($fp);
				return null;
			}
			$written += $n;
		}

		$head = self::readExact($fp, 4);
		if ($head === null) {
			self::$lastError = 'no answer within ' . $readTimeoutMs . 'ms';
			fclose($fp);
			return null;
		}
		$unpacked = unpack('Nlen', $head);
		$blen = (int) $unpacked['len'];
		// The open-orders page's biggest read answers 5.4 MB; the old 4 MB cap
		// turned it into a failure that sent the rest of the request to MariaDB.
		$maxReply = defined('AIOS_COMPUTE_MAX_REPLY_BYTES') ? (int) AIOS_COMPUTE_MAX_REPLY_BYTES : 100 * 1024 * 1024;
		if ($blen <= 0 || $blen > $maxReply) {
			self::$lastError = 'bad frame length ' . $blen;
			fclose($fp);
			return null;
		}
		$json = self::readExact($fp, $blen);
		fclose($fp);
		if ($json === null) {
			self::$lastError = 'truncated answer';
			return null;
		}
		$res = json_decode($json, true);
		if (!is_array($res)) {
			self::$lastError = 'answer was not JSON';
			return null;
		}
		return $res;
	}

	private static function readExact($fp, $n) {
		$buf = '';
		while (strlen($buf) < $n) {
			$chunk = @fread($fp, $n - strlen($buf));
			if ($chunk === false || $chunk === '') {
				$meta = stream_get_meta_data($fp);
				if (!empty($meta['timed_out']) || feof($fp)) {
					return null;
				}
				continue;
			}
			$buf .= $chunk;
		}
		return $buf;
	}
}
