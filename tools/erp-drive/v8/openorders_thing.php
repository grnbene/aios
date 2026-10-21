<?php
// catalog/model/oms/openorders_thing.php: apir/orders from the ASQL Thing open_orders (aios/things/
// open_orders_parts.sql) - the board's own query as a Thing, one per open order, kept by the rows, with
// the queries the board ran per order as parts. This model runs none of them: it reads the Things as JSON
// and shapes each one into the entry getOpenOrdersInvDispatch() returned, with the same arithmetic
// (order totals, the join's sums), the same formatting and the same whitelist of fields. The four tax
// lookup tables are read once per request. null = use the usual path (lastError() says why).
// On with config OpenOrdersThing = 1 or &thing=1; &nothing=1 forces the usual path.
//
// Not reproduced on purpose: getOpenOrdersInvDispatch() writes an order's status to 21 (Picked) when
// it finds its picking complete, inside a read. The entry here shows Picked the same way; the write
// belongs to the picking flow.
class ModelOmsOpenordersThing extends Model {
	const THING = 'open_orders';
	const INV_THING = 'on_dispatch';

	private static $last_error = '';
	private static $version = '';
	private $tax = null;      // the four lookup tables, read once per request
	private $orders_real = null;
	private $orders_model = null;

	public function lastError() { return self::$last_error; }
	// where the last results() spent its time, for the X-OpenOrders-Time header
	private static $timing = array();
	public function timing() { $o = array(); foreach (self::$timing as $k => $v) $o[] = $k . '=' . round($v * 1000) . 'ms'; return implode(' ', $o); }
	private function mark($k, $t0) { self::$timing[$k] = (self::$timing[$k] ?? 0) + microtime(true) - $t0; }
	public function version() { return self::$version; }

	public function results($filters, $get) {
		self::$timing = array();
		$docs = $this->things($filters, $get);
		if ($docs === null) return null;
		$entries = array(); $ids = array(); $meta = array();
		$t = microtime(true);
		foreach ($docs['open'] as $d) { $entries[] = $this->entry($d); $ids[] = 'o:' . $d['orderno']; $meta['o:' . $d['orderno']] = array($d['date_created'], $d['confirmeddate']); }
		$this->mark('map', $t); $t = microtime(true);
		foreach ($docs['inv'] as $d) { $e = $this->inv_entry($d, $filters); if ($e) { $entries[] = $e; $ids[] = 'i:' . $d['orderno']; $meta['i:' . $d['orderno']] = array($d['created'], null); } }
		$this->mark('inv', $t); $t = microtime(true);
		self::$version = $docs['clock'];
		$this->remember($filters, $docs['clock'], $ids, $meta);
		$this->mark('snapshot', $t);
		return $entries;
	}

	// What moved since the clock a client holds, from the engine's own feed (SHOW THING .. JSON SINCE):
	// 'not_modified'; 'changed' with the full ordered ids, the ids the client's list was built from, and
	// the entries that moved; or 'reload' when nothing is kept for that clock. The list order comes from a
	// snapshot of ids and their sort dates per (user, filters, clock) - a few KB, no renders.
	public function changes($filters, $get, $since) {
		$since = (string) $since;
		if (!preg_match('/^\d+$/', $since)) return array('status' => 'reload', 'version' => '');
		if (!$this->gates($filters, $get)) return null;
		$clock = $this->asql('SHOW THING ' . self::THING . ' CLOCK');
		if (!$clock) return null;
		$now = (string) $clock['rows'][0][0];
		if ($now === $since) return array('status' => 'not_modified', 'version' => $now);
		$snap = $this->recall($filters, $since);
		if ($snap === null) return array('status' => 'reload', 'version' => $now);
		$moved = $this->asql('SHOW THING ' . self::THING . ' JSON SINCE ' . $since);
		if (!$moved) return null;
		$ctx = $this->context();
		$meta = $snap['meta']; $entries = array();
		foreach ($moved['rows'] as $r) {
			$id = 'o:' . $r[0];
			$d = $r[2] === 'live' ? json_decode($r[3], true) : null;
			if (!is_array($d) || !$this->visible($d, $filters, $ctx, true)) { unset($meta[$id]); continue; }
			$meta[$id] = array($d['date_created'], $d['confirmeddate']);
			$entries[$id] = $this->entry($d);
		}
		$open = array(); $inv = array();
		foreach ($meta as $id => $m) { if ($id[0] === 'o') $open[] = $id; else $inv[] = $id; }
		usort($open, function ($x, $y) use ($meta) {
			return $this->cmp_desc($meta[$x][0], $meta[$y][0]) ?: $this->cmp_desc($meta[$x][1], $meta[$y][1]) ?: ((int) substr($y, 2) - (int) substr($x, 2));
		});
		$ids = array_merge($open, $inv);   // the invoiced half keeps its order until its Thing has parts
		self::$version = $now;
		$this->remember($filters, $now, $ids, $meta);
		return array('status' => 'changed', 'version' => $now, 'ids' => $ids, 'since_ids' => $snap['ids'], 'entries' => (object) $entries);
	}

	// ---- the id snapshot per (user, filters, clock): what a later changes() diffs against ----------
	private function snap_file($filters, $clock) {
		$dir = rtrim(DIR_CACHE, '/') . '/oms_openorders_' . preg_replace('/[^A-Za-z0-9_]/', '', (string) $this->db->database) . '/snap/';
		if (!is_dir($dir) && !@mkdir($dir, 0775, true) && !is_dir($dir)) return null;
		$who = isset($this->session->data['userid']) ? (string) $this->session->data['userid'] : '';
		return $dir . md5($who . '|' . serialize($filters)) . '_' . preg_replace('/\D/', '', (string) $clock) . '.json';
	}

	private function remember($filters, $clock, $ids, $meta) {
		$f = $this->snap_file($filters, $clock);
		if ($f === null) return;
		$tmp = $f . '.' . getmypid() . '.tmp';
		if (@file_put_contents($tmp, json_encode(array('ids' => $ids, 'meta' => $meta))) !== false) @rename($tmp, $f);
		foreach ((array) glob(dirname($f) . '/*.json') as $old) if ($old !== $f && @filemtime($old) < time() - 600) @unlink($old);
	}

	private function recall($filters, $clock) {
		$f = $this->snap_file($filters, $clock);
		if ($f === null) return null;
		$raw = @file_get_contents($f);
		$snap = $raw === false ? null : json_decode($raw, true);
		return is_array($snap) && isset($snap['ids'], $snap['meta']) ? $snap : null;
	}

	// ---- the Things ---------------------------------------------------------------------------------

	private function gates($filters, $get) {
		if (!empty($get['nothing'])) { self::$last_error = 'off for this request (nothing=1)'; return false; }
		if (empty($get['thing']) && (string) $this->config->get('OpenOrdersThing') !== '1') { self::$last_error = 'off (config OpenOrdersThing is not 1, no thing=1)'; return false; }
		foreach (array('stockid', 'to', 'from', 'filtered', 'canceled', 'canceled_from', 'canceled_to', 'trackingreq') as $f)
			if (!empty($filters[$f])) { self::$last_error = 'filter ' . $f; return false; }
		if (isset($filters['picked']) || !empty($get['stickerdetails'])) { self::$last_error = 'picked/stickerdetails'; return false; }
		if (in_array($this->config->get('db_database'), array('aa', 'moov', 'moov_sandbox'), true)) { self::$last_error = 'tenant ' . $this->config->get('db_database'); return false; }
		if (getenv('AIOS_ASQL_PRIMARY') !== '1') {
			$status = $this->asql('SHOW SLAVE STATUS');
			if (!$status || !$this->caught_up($status)) { self::$last_error = self::$last_error ?: 'mirror not replicating'; return false; }
		}
		return true;
	}

	private function things($filters, $get) {
		if (!$this->gates($filters, $get)) return null;
		$ctx = $this->context();
		$open = array();
		$t = microtime(true);
		$a = $this->asql('SHOW THING ' . self::THING . ' JSON');
		$this->mark('read', $t); $t = microtime(true);
		if (!$a) { self::$last_error = self::$last_error ?: 'no Thing ' . self::THING; return null; }
		foreach ($a['rows'] as $r) {
			if ($r[2] !== 'live') continue;
			$d = json_decode($r[3], true);
			if (is_array($d) && $this->visible($d, $filters, $ctx, true)) $open[] = $d;
		}
		$this->mark('decode', $t); $t = microtime(true);
		usort($open, function ($x, $y) {
			return $this->cmp_desc($x['date_created'], $y['date_created']) ?: $this->cmp_desc($x['confirmeddate'], $y['confirmeddate']) ?: ((int) $y['orderno'] - (int) $x['orderno']);
		});
		$this->mark('sort', $t);
		$inv = array();
		$b = $this->asql('SHOW THING ' . self::INV_THING . ' JSON');
		if ($b) {
			foreach ($b['rows'] as $r) {
				if ($r[2] !== 'live') continue;
				$d = json_decode($r[3], true);
				if (is_array($d) && $this->visible($d, $filters, $ctx, false)) $inv[] = $d;
			}
			usort($inv, function ($x, $y) { return $this->cmp_desc($x['created'], $y['created']) ?: ((int) $x['orderno'] - (int) $y['orderno']); });
		}
		$clock = $this->asql('SHOW THING ' . self::THING . ' CLOCK');
		return array('open' => $open, 'inv' => $inv, 'clock' => $clock ? (string) $clock['rows'][0][0] : '');
	}

	// The per-user and per-session filters the board's query carried
	private function context() {
		$userid = isset($this->session->data['userid']) ? (string) $this->session->data['userid'] : '';
		$locs = null;
		if ($userid !== '') {
			$locs = array();
			$q = $this->db->query("SELECT loccode FROM locationusers WHERE userid = '" . $this->db->escape($userid) . "' AND canview = 1");
			foreach ($q->rows as $r) $locs[] = $r['loccode'];
		}
		$this->load->model('weberp/salesman');
		return array('userid' => $userid, 'locs' => $locs,
			'salesman' => (string) $this->model_weberp_salesman->getSalesmanCodeByUser($userid),
			'only_app' => !empty($this->session->data['issalesman']) && $this->config->get('SalemanOrdersVisibleOnly') == 1);
	}

	private function visible($d, $filters, $ctx, $open_half) {
		$loc = $open_half ? $d['fromstkloc'] : $d['loccode'];
		$loccode = isset($filters['loccode']) ? (string) $filters['loccode'] : '';
		if ($loccode !== '' && !$this->eq($loc, $loccode)) return false;
		if ($ctx['locs'] !== null && ($loccode === '' || !$open_half) && !$this->in_locs($loc, $ctx['locs'])) return false;
		foreach (array('debtorno', 'branchcode', 'placedfrom') as $f)
			if (isset($filters[$f]) && $filters[$f] !== '' && !$this->eq($d[$f], $filters[$f])) return false;
		$salesman = $open_half ? $ctx['salesman'] : ($filters['salesman'] ?? '');
		if ($salesman !== '') {
			$mine = $this->eq($d['salesperson'], $salesman) || (($d['salesperson'] === null || trim($d['salesperson']) === '') && $this->eq($d['takenby'], $ctx['userid']));
			if (!$mine) return false;
		}
		if ($open_half && $ctx['only_app'] && !$this->eq($d['placedfrom'], '3')) return false;
		return true;
	}

	// ---- one entry from one Thing -------------------------------------------------------------------

	private function entry($d) {
		if ($this->orders_model === null) {   // once per request, not per order: the loader checks files each time
			$this->load->model('oms/orders');
			$this->load->model('weberp/picking');
			$this->orders_model = $this->model_oms_orders;
		}
		$m = $this->orders_model;
		$order = $this->head($d);

		// the join the board's query made over its WHERE (open lines) - lines x invoices x moves - summed
		// the way SUM() did over those rows, and printed the way MySQL printed them
		$lines = array(); foreach ($d['invlines'] as $L) if ((int) $L['completed'] == 0) $lines[] = $L;
		$trans = $d['trans'];
		$by = array(); foreach ($d['moves'] as $M) $by[$M['transno'] . '|' . $M['orderlineref']][] = $M;
		$sumqty = null; $sumquantity = 0; $ordervalue = 0.0; $t = null; $first = true; $qints = true; $qscale = 0;
		foreach ($lines as $L) {
			if (!preg_match('/^-?\d+$/', (string) $L['quantity'])) { $qints = false; $qscale = max($qscale, strlen(substr(strrchr((string) $L['quantity'], '.'), 1))); }
			foreach ($trans ?: array(null) as $T) {
				$ms = $T && isset($by[$T['transno'] . '|' . $L['orderlineno']]) ? $by[$T['transno'] . '|' . $L['orderlineno']] : array(null);
				foreach ($ms as $M) {
					if ($first) { $t = $T ? $T['type'] : null; $first = false; }
					if ($M && $M['qty'] !== null) $sumqty = ($sumqty ?? 0) + (float) $M['qty'];
					$sumquantity += (float) $L['quantity'];
					$ordervalue += (float) $L['unitprice'] * ((float) $L['quantity'] - (float) $L['qtyinvoiced']);
				}
			}
		}
		$order['t'] = $t;
		$order['sumqty'] = $sumqty === null ? null : $this->dbl($sumqty);
		$order['sumquantity'] = $qints ? (string) (int) $sumquantity : number_format($sumquantity, $qscale, '.', '');
		$order['ordervalue'] = $this->dbl($ordervalue);
		$order['fillable'] = $this->fillable($d);

		$payment_details = json_decode($order['payment_details'], true);
		$tracking = $this->tracking($d, $m);
		$auth_data = $this->auth_data($d);
		$format = '{name}' . "\n" . '{address1}' . " " . '{address3}';
		$find = array('{name}', '{address1}', '{address3}');
		$replace = array('(<b>' . $order['debtorno'] . '.' . $order['branchcode'] . '</b>) ' . $order['brname'],
			$this->shop->stripslashes_deep($order['deladd1']), $this->shop->stripslashes_deep($order['deladd3']));
		$shipto = str_replace(array("\r\n", "\r", "\n"), '<br />', preg_replace(array("/\s\s+/", "/\r\r+/", "/\n\n+/"), '<br />', trim(str_replace($find, $replace, $format))));
		$date = new DateTime($order['deliverydate']);
		$date = $date->format($this->config->get('date_format_short'));
		$snoozed = '';
		if (isset($order['snoozed']) && $order['snoozed'] == '1') $snoozed = $d['snooze'] ? $d['snooze'][0] : array();
		$needspo = false; $polinks = null; $pos = null;
		if (!$this->config->get('NoPOFromOrder')) {
			$polinks = $d['po_links'];
			if (!$order['fillable']) {
				if (!$polinks) $polinks = $d['open_po_items'] ?: false;
				if (!$order['fillable'] && !$polinks) $needspo = true;
			}
			if (!is_array($polinks)) $polinks = $polinks ? (array) $polinks : array();
			if ($polinks) {
				$uniqueHaspo = array_values(array_unique(array_column($polinks, 'purchase_orderno')));
				$polinks = array_map(function ($orderno) { return array("purchase_orderno" => $orderno); }, $uniqueHaspo);
			} else {
				$polinks = array();
			}
			if ($order['orderstatusid'] < 23 && $order['orderstatusid'] != 21 && $order['orderstatusid'] != 22) {
				if ($this->order_picked($d)) { $order['orderstatusid'] = 21; $order['statusname'] = 'Picked'; }
			}
		}
		$totals = $this->order_totals($order, $d);
		$orddate = new DateTime($order['orddate']);
		$orddate = $orddate->format($this->cfg('DefaultDateFormat'));
		$ordlines = $d['lines'];
		$usetotal = $this->cfg('ShowTaxOnOpenOrders');
		$picked = $this->has_been_picked($d);
		$order['statusname'] = $order['statusname'];
		return array(
			'orderno' => $order['orderno'],
			'status' => $order['statusname'],
			'orderstatusid' => $order['orderstatusid'],
			'placedfrom' => $order['placedfrom'],
			'order_channel' => isset($order['order_channel']) ? $order['order_channel'] : '',
			'transno' => $order['transno'],
			'ponumber' => $order['customerref'],
			'fillable' => ($order['fillable']) ? true : false,
			'shipto' => $shipto,
			'debtorno' => $order['debtorno'],
			'branchcode' => $order['branchcode'],
			'deliverto' => $this->shop->stripslashes_deep($order['name']),
			'deliverydate' => $date,
			'details' => $order,
			'is_cod' => !empty($order['is_cod']) ? 1 : 0,
			'cod_called' => !empty($order['cod_called']) ? 1 : 0,
			'cod_called_by' => isset($order['cod_called_by']) ? $order['cod_called_by'] : '',
			'cod_called_at' => isset($order['cod_called_at']) ? $order['cod_called_at'] : null,
			'snoozed' => isset($order['snoozed']) ? $order['snoozed'] : null,
			'snoozed_details' => $snoozed,
			'notes' => $this->shop->stripslashes_deep(html_entity_decode((string) $order['notes'], ENT_QUOTES)),
			'comments' => $this->shop->stripslashes_deep(html_entity_decode((string) $order['comments'], ENT_QUOTES)),
			'daystilldue' => (int) $order['daystilldue'],
			'deladd1' => $order['deladd1'], 'deladd2' => $order['deladd2'], 'deladd3' => $order['deladd3'], 'deladd4' => $order['deladd4'], 'deladd5' => $order['deladd5'],
			'shipvia' => $order['shipvia'],
			'lat' => $order['lat'], 'lng' => $order['lng'],
			'haspo' => $polinks,
			'suppliers' => (!$this->config->get('NoPOFromOrder')) ? $this->supplier_ref($d) : false,
			'pos' => $pos,
			'needspo' => $needspo,
			'statusname' => $order['statusname'],
			'typename' => $order['statusname'],
			'ordertype' => $order['ordertype'],
			'phoneno' => $order['phoneno'],
			'takenby' => $order['takenby'],
			'printedpackingslip' => $order['printedpackingslip'],
			'date_created' => $order['date_created'],
			'trandate' => $orddate,
			'order_' => $order['orderno'],
			'email' => $order['email'],
			'usetotal' => $usetotal,
			'total' => (float) number_format($totals['total'], 2, '.', ''),
			'subtotal' => (float) number_format($totals['subtotal'], 2, '.', ''),
			'ovamount' => (float) number_format($totals['subtotal'], 2, '.', ''),
			'ovgst' => (float) number_format($totals['taxtotal'], 2, '.', ''),
			'ovfreight' => (float) number_format($totals['freighttotal'], 2, '.', ''),
			'ovfee' => 0,
			'taxtotal' => (float) number_format($totals['taxtotal'], 2, '.', ''),
			'payment_details' => $payment_details,
			'freighttotal' => (float) number_format($totals['freighttotal'], 2, '.', ''),
			'tracking' => is_array($tracking) ? $tracking : array(),
			'tracking_display' => $m->buildTrackingSearchText($tracking),
			'lines' => $ordlines,
			'picked' => $picked,
			'auth_data' => $auth_data,
			'has_payment_attached' => (int) ($order['has_order_payment_method'] ?? 0) === 1,
		);
	}

	// The head row as the board's query returned it: every scalar of the Thing, as strings or null
	private function head($d) {
		$row = array();
		foreach ($d as $k => $v) if (!is_array($v) && $k[0] !== '_') $row[$k] = $v;
		return $row;
	}

	private $cfgs = array();
	private function cfg($name) {
		if (!array_key_exists($name, $this->cfgs)) $this->cfgs[$name] = $this->shop->getSingleConfig($name);
		return $this->cfgs[$name];
	}

	// A DOUBLE as MySQL prints it: the shortest text that reads back as the same number
	private function dbl($v) {
		if ($v == floor($v) && abs($v) < 1e15) return (string) (int) $v;
		for ($p = 15; $p <= 17; $p++) { $s = sprintf('%.' . $p . 'g', $v); if ((float) $s === (float) $v) break; }
		if (strpos($s, 'e') !== false || strpos($s, 'E') !== false) $s = rtrim(rtrim(sprintf('%.17f', $v), '0'), '.');
		return $s;
	}

	// The query's `fillable`: for the row MySQL grouped on (the first line), is stock on hand >= the picks frozen for it
	private function fillable($d) {
		if (!$d['invlines']) return null;
		$L = $d['invlines'][0];
		$picks = null;
		foreach ($d['picking'] as $p) if ((string) $p['linenumber'] === (string) $L['orderlineno']) $picks = ($picks ?? 0) + (float) $p['pick'];
		if ($picks === null) return null;
		$stock = null;
		foreach ($d['stock'] as $s) if ($this->eq($s['stkcode'], $L['stkcode'])) $stock = $s['quantity'];
		return ($stock === null) ? null : (((float) $stock >= $picks) ? '1' : '0');
	}

	private function order_picked($d) {
		if (!$d['picking']) return false;
		$incomplete = array();
		foreach ($d['picking'] as $p) if ((float) $p['picked'] < (float) $p['pick']) $incomplete[(string) $p['linenumber']] = true;
		return count($incomplete) == 0;
	}

	private function has_been_picked($d) {
		if (!$d['picking']) return false;
		foreach ($d['picking'] as $p) if ($p['picked'] < $p['pick']) return false;
		return true;
	}

	private function auth_data($d) {
		if (!$d['auth']) return false;
		$ret = $d['auth'][0];
		$format = $this->config->get('defaultDateFormat');
		if (!isset($format) || $format == '') $format = 'm/d/Y';
		$ret['transdate'] = date($format . ' h:i A', strtotime($ret['transdate']));
		return $ret;
	}

	private function supplier_ref($d) {
		// GROUP_CONCAT(bankref) of the suppliers of the POs linked to the order: not carried by the Thing
		// on a tenant that makes POs from orders; declare a part for it there.
		return null;
	}

	// getOrderTrackingNumber(): the tracking rows, 3PL shipments folded in, urls resolved by the model's own rules
	private function tracking($d, $m) {
		$tracking = $d['tracking'];
		foreach ($tracking as &$r) $r['shipper_url_template'] = '';
		unset($r);
		if ($d['tpl']) {
			$seen = array_flip(array_column($tracking, 'trackingnumber'));
			foreach ($d['tpl'] as $r) {
				$num = isset($r['tracking_number']) ? trim($r['tracking_number']) : '';
				if ($num === '' || isset($seen[$num])) continue;
				$seen[$num] = true;
				$date = !empty($r['ship_date']) ? $r['ship_date'] : (!empty($r['created_at']) ? substr($r['created_at'], 0, 10) : date('Y-m-d'));
				$tracking[] = array('trackingnumber' => $num, 'shippername' => !empty($r['carrier']) ? $r['carrier'] : '3PL', 'type_id' => null,
					'shipstatus' => 0, 'datemon' => $date, 'bind' => 0, 'url' => '', 'transno' => $d['orderno'], 'shipper_url_template' => '');
			}
		}
		// the loader hands out a Proxy carrying only the public methods; the private helper is
		// reached on a real instance of the class (the loader already required its file)
		if ($this->orders_real === null) $this->orders_real = new ModelOmsOrders($this->registry);
		$enrich = Closure::bind(function (&$t) { $this->enrichTrackingUrls($t); }, $this->orders_real, ModelOmsOrders::class);
		$enrich($tracking);
		return $tracking;
	}

	// getOrderTotals() over the Thing: header from the head row, lines from the part, rates from four tables read once
	private function order_totals($order, $d) {
		$tax = $this->tax_tables();
		$taxgroupid = ($order['order_taxgroup']) ? $order['order_taxgroup'] : $order['taxgroupid'];
		$fromstkloc = $order['fromstkloc'];
		if ($order['fromstkloc'] != $this->config->get('MainWarehouseLocation')) {
			$check = isset($tax['td'][$taxgroupid]) ? $tax['td'][$taxgroupid] : null;
			$allowed_excempt = false;
			switch ($check) {
				case 2: if ($order['tax_ref'] == '') $order['tax_ref'] = 'xx'; break;
				case 3: if ($order['tax_ref'] == '') $order['tax_ref'] = 'xx'; $allowed_excempt = true; break;
			}
			$field = 'taxgroupid_cert';
			if ($order['tax_ref'] == '' || !$order['tax_ref']) $field = 'taxgroupid';
			if (!$allowed_excempt) $taxgroupid = isset($tax['loc'][$fromstkloc]) ? $tax['loc'][$fromstkloc][$field] : null;
		}
		$subtotal = 0; $taxtotal = 0;
		$freighttotal = (float) number_format($order['freightcost'], 2, '.', '');
		foreach ($d['invlines'] as $order_line) {
			$quantity = $order_line['quantity'] - $order_line['qtyinvoiced'];
			if ($quantity < 0) $quantity = 0;
			$discountprice = ($order_line['unitprice'] - round($order_line['unitprice'] * $order_line['discountpercent'], 2));
			if ($order_line['taxable'] == 'No' || $order_line['taxable'] == 'N') $order_line['taxcatid'] = '4';
			$lineTotal = number_format($discountprice * $quantity, 2, '.', '');
			$subtotal += $lineTotal;
			$taxes = $this->tax_rates($tax, $taxgroupid, $fromstkloc, $order_line['taxcatid']);
			$overwritten = json_decode((string) $order_line['taxoverride'], true);
			if ($overwritten && !empty($overwritten) && $overwritten[0]['taxauthid']) $taxes = $overwritten;
			foreach ($taxes as $tx) {
				$lineTax = $lineTotal * $tx['taxrate'];
				$taxtotal += number_format($lineTax, 2, '.', '');
			}
		}
		if ($taxtotal && $freighttotal) {
			foreach ($this->tax_rates($tax, $taxgroupid, $fromstkloc, '5') as $tx) {
				$shippingTax = $freighttotal * $tx['taxrate'];
				$taxtotal += (float) number_format((float) $shippingTax, 2, '.', '');
			}
		}
		$total = (float) number_format($subtotal + $freighttotal + $taxtotal, 2, '.', '');
		return array('total' => (float) number_format($total, 2, '.', ''), 'subtotal' => (float) number_format($subtotal, 2, '.', ''),
			'taxtotal' => (float) number_format($taxtotal, 2, '.', ''), 'freighttotal' => (float) number_format($freighttotal, 2, '.', ''));
	}

	// getTaxRatesByTaxGroupId() over the tables read once: the rates indexed by (group, province, category),
	// each bucket shaped and ordered once - a line's lookup is one array read, not a scan of every rate
	private function tax_rates($tax, $taxgroupid, $fromstkloc, $taxcatid) {
		$province = isset($tax['loc'][$fromstkloc]) ? $tax['loc'][$fromstkloc]['taxprovinceid'] : null;
		$k = (string) $taxgroupid . '|' . (string) $province . '|' . ($taxcatid === null ? '*' : (string) $taxcatid);
		return isset($tax['idx'][$k]) ? $tax['idx'][$k] : array();
	}

	private function index_rates($rates) {
		$buckets = array();
		foreach ($rates as $r) {
			$row = array('taxauthid' => $r['taxauthid'], 'description' => $r['description'], 'taxrate' => $r['taxrate'],
				'taxratepercent' => $r['taxrate'] * 100, 'taxontax' => $r['taxontax'], 'taxglcode' => $r['taxglcode'], 'calculationorder' => $r['calculationorder']);
			$gp = (string) $r['taxgroupid'] . '|' . (string) $r['dispatchtaxprovince'];
			$buckets[$gp . '|' . (string) $r['taxcatid']][] = $row;
			$buckets[$gp . '|*'][] = $row;
		}
		foreach ($buckets as &$b) usort($b, function ($a, $c) { return $a['calculationorder'] <=> $c['calculationorder']; });
		unset($b);
		return $buckets;
	}

	private function tax_tables() {
		if ($this->tax !== null) return $this->tax;
		$t = array('td' => array(), 'loc' => array(), 'rates' => array());
		$a = $this->asql("SELECT taxgroupid, LEFT(taxgroupdescription, 1) AS td FROM taxgroups");
		if ($a) foreach ($this->assoc($a) as $r) $t['td'][$r['taxgroupid']] = $r['td'];
		$a = $this->asql("SELECT loccode, taxgroupid, taxgroupid_cert, taxprovinceid FROM locations");
		if ($a) foreach ($this->assoc($a) as $r) $t['loc'][$r['loccode']] = $r;
		$a = $this->asql("SELECT tt.taxgroupid, tas.dispatchtaxprovince, tas.taxcatid, tt.calculationorder, ta.description, tt.taxauthid, ta.taxglcode, tt.taxontax, tas.taxrate
			FROM taxauthrates tas INNER JOIN taxgrouptaxes tt ON tas.taxauthority = tt.taxauthid INNER JOIN taxauthorities ta ON tas.taxauthority = ta.taxid");
		if ($a) $t['rates'] = $this->assoc($a);
		$t['idx'] = $this->index_rates($t['rates']);
		return $this->tax = $t;
	}

	// ---- the invoiced half: on_dispatch (the list) with the board's own render until its parts are declared
	private function inv_entry($d, $filters) {
		$this->load->model('oms/orders');
		$this->load->model('weberp/orders');     // getInvoicedOnDispatch() reaches for these, loaded by its usual caller
		$this->load->model('weberp/picking');
		$f = $filters;
		$f['filtered'] = array(array('ORDER_NUMBER' => $d['orderno']));
		$rows = $this->model_oms_orders->getInvoicedOnDispatch($f);
		return $rows ? $rows[0] : null;
	}

	// ---- helpers --------------------------------------------------------------------------------------

	private function num($v) {   // as MySQL prints a SUM of DECIMALs / integers
		$s = rtrim(rtrim(number_format($v, 6, '.', ''), '0'), '.');
		return $s === '-0' ? '0' : $s;
	}

	private function caught_up($status) {
		$s = $this->assoc($status);
		if (!$s) return false;
		$r = $s[0];
		return ($r['Slave_IO_Running'] ?? '') === 'Yes' && ($r['Slave_SQL_Running'] ?? '') === 'Yes'
			&& ($r['Last_IO_Errno'] ?? '1') === '0' && ($r['Slave_IO_State'] ?? '') === 'Waiting for master to send event';
	}

	private function in_locs($loc, $locs) { foreach ($locs as $l) if ($this->eq($loc, $l)) return true; return false; }

	private function eq($a, $b) {
		if ($a === null || $b === null) return false;
		return strcasecmp(rtrim((string) $a), rtrim((string) $b)) === 0;
	}

	private function cmp_desc($a, $b) {
		if ($a === $b) return 0;
		if ($a === null) return 1;
		if ($b === null) return -1;
		return strcmp((string) $b, (string) $a);
	}

	private function assoc($answer) {
		$out = array();
		foreach ($answer['rows'] as $row) {
			$r = array();
			foreach ($answer['columns'] as $i => $name) { $v = isset($row[$i]) ? $row[$i] : null; $r[$name] = ($v === 'NULL') ? null : $v; }
			$out[] = $r;
		}
		return $out;
	}

	private function asql($sql) {
		if (!class_exists('AiosCompute')) { self::$last_error = 'no AiosCompute'; return null; }
		$answer = AiosCompute::query($sql, array('db' => (string) $this->db->database, 'deadline_ms' => 10000));
		if (!is_array($answer) || !isset($answer['columns'], $answer['rows'])) { self::$last_error = AiosCompute::lastError() ?: 'no usable answer'; return null; }
		return $answer;
	}
}
