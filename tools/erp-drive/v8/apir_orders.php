<?php

use Cart\Length;

class ControllerApirOrders extends Controller
{
	// apir/orders/changes&since=<X-OpenOrders-Version>&<index's own params>: only the open
	// orders that changed since that version, from the Thing fingerprints (see
	// ModelOmsOpenordersThing::changes). success false with reload: the client refetches apir/orders.
	public function changes()
	{
		$this->load->model('oms/orders');
		$filters = $this->filters($this->request->input);
		$since = isset($this->request->get['since']) ? (string) $this->request->get['since'] : '';
		$this->load->model('oms/openorders_thing');
		$answer = $this->model_oms_openorders_thing->changes($filters, $this->request->get, $since);
		if ($answer === null) {
			$this->response->setJsonOutput(array('success' => false, 'reload' => true,
				'message' => 'fast path off: ' . $this->model_oms_openorders_thing->lastError()));
			return;
		}
		$this->response->setJsonOutput(array('success' => true) + $answer);
	}

	// The filters index() and changes() both list open orders by
	private function filters($data)
	{
		$filters = array();

		$filters['loccode'] = $this->request->get['loccode'];

		if (isset($this->request->get['debtorno']) && $this->request->get['debtorno'] != '') {
			$filters['debtorno'] = $this->request->get['debtorno'];
		}

		if (isset($data['item']) && $data['item'] != '') {
			$filters['stockid'] = $data['item'];
		}

		if (isset($data['from']) && $data['from'] != '') {
			$filters['to'] = date('Y-m-d', strtotime($data['to']));
		}

		if (isset($data['from']) && $data['from'] != '') {
			$filters['from'] = date('Y-m-d', strtotime($data['from']));
		}

		if (isset($data['item']) && $data['item'] != '') {
			$filters['stockid'] = $data['item'];
		}

		if (isset($data['filtered']) && $data['filtered'] != '') {
			$filters['filtered'] = $data['filtered'];
		}

		if (isset($data['picked'])) {
			$filters['picked'] = $data['picked'];
		}

		if (isset($data['loccode']) && $data['loccode'] != '') {
			$filters['loccode'] = $data['loccode'];
		}

		if (isset($data['orders']) && count($data['orders'])) {
			$filters['filtered'] = $data['orders'];
		}

		if(isset($this->request->get['canceled']) && $this->request->get['canceled']){
			$filters['canceled'] = $this->request->get['canceled'];
		}

		if(isset($this->request->get['canceledfrom']) && $this->request->get['canceledfrom']){
			$filters['canceled_from'] = $this->request->get['canceledfrom'];
		}

		if(isset($this->request->get['canceledto']) && $this->request->get['canceledto']){
			$filters['canceled_to'] = $this->request->get['canceledto'];
		}

		if(isset($this->request->get['branchcode']) && !empty($this->request->get['branchcode']) && $this->request->get['branchcode'] != 'all'){
			$filters['branchcode'] = $this->request->get['branchcode'];
		}

		if(isset($this->request->get['placedfrom']) && !empty($this->request->get['placedfrom'])){
			$filters['placedfrom'] = $this->request->get['placedfrom'];
		}

		if(isset($this->request->get['trackingreq']) && !empty($this->request->get['trackingreq']) && $this->request->get['trackingreq'] == '1'){
			$filters['trackingreq'] = true;
		}

		$this->load->model('weberp/salesman');
		$salesman_check = $this->model_weberp_salesman->getSalesmanCodeByUser($this->session->data['userid']);

		if ($salesman_check != '' && isset($salesman_check)) {
			$filters['salesman'] = $salesman_check;
		}

		return $filters;
	}

	public function index()
	{
		$data = $this->request->input;
		$results = array();

		$quotecriteria = '';

		$this->load->model('customer/customer');
		$this->load->model('oms/orders');
		$filters = $this->filters($data);

		$this->load->model('oms/openorders_thing');
		$__t0 = microtime(true);
		$fast = $this->model_oms_openorders_thing->results($filters, $this->request->get);
		$__t1 = microtime(true);
		// where the request's time went: before the Thing was read (bootstrap, filters), the read + shaping, the encode
		$this->response->addHeader(sprintf('X-OpenOrders-Time: before=%.0fms results=%.0fms', ($__t0 - $_SERVER['REQUEST_TIME_FLOAT']) * 1000, ($__t1 - $__t0) * 1000) . ' ' . $this->model_oms_openorders_thing->timing());
		// says which path answered, and why the fast one declined, without touching the body
		$this->response->addHeader('X-OpenOrders-Thing: ' . ($fast !== null ? 'served' : 'fallback: ' . ($this->model_oms_openorders_thing->lastError() ?: 'not enabled')));
		$this->response->addHeader('Access-Control-Expose-Headers: X-OpenOrders-Thing, X-OpenOrders-Version');
		if ($fast !== null) {
			// the version a client passes to orders/changes to get only what moved after this
			$this->response->addHeader('X-OpenOrders-Version: ' . $this->model_oms_openorders_thing->version());
			$this->response->setJsonOutput($fast);
			return;
		}

		$orders = $this->model_oms_orders->getOpenOrdersInvDispatch($filters);
		$invoiced = $this->model_oms_orders->getInvoicedOnDispatch($filters);
		$orders = is_array($orders) ? $orders : array();
		$invoiced = is_array($invoiced) ? $invoiced : array();
		$results = $this->model_oms_orders->combine($orders, $invoiced);

		if(isset($this->request->get['stickerdetails']) && $this->request->get['stickerdetails']){
			foreach($results as &$ord){
				$stickerdetails = $this->model_oms_orders->getOrderStickerDetails($ord['orderno']);
				$ord['stickerdetails'] = $stickerdetails;
			}
		}

		$this->response->setJsonOutput($results);
	}

	public function canceled(){

	}

	public function batchvoid()
	{

		$vs = array('244464', '244466', '244437', '244413', '244394', '244404', '244467', '244468', '244397', '244388', '244388', '244461', '244400', '244404', '244471', '244478', '244462', '244415', '244479', '244422', '244451', '244431', '244474', '244453', '244475', '244411', '244397', '244383', '244397', '244424');
		echo '<pre>';

		$payment_response = array(
			'success' => false
		);

		foreach ($vs as $v) {


			$existingauths = $this->payment->getExistingAuths($v);

			if (!empty($existingauths)) {
				foreach ($existingauths as $void) {
					$datavoid = array(
						'transid' => $void['transid'],
						'uid' => $void['uid'],
						'amount' => $void['amount'],
						'orderno' => $v,
					);
					$payment_response = $this->payment->voidPayment($datavoid);
					print_r($payment_response);
				}
			}
		}

		$this->response->setJsonOutput($payment_response);
	}


	public function voidtrans()
	{
		$payment_response = array();


		if ($this->request->input['orderno']) {
			$existingauths = $this->payment->getExistingAuths($this->request->input['orderno']);

			$payment_response = array(
				'success' => false
			);

			if (!empty($existingauths)) {
				foreach ($existingauths as $void) {
					$datavoid = array(
						'transid' => $void['transid'],
						'uid' => $void['uid'],
						'amount' => $void['amount'],
						'orderno' => $this->request->input['orderno'],
					);

					if(PACKAGE === 'tires'){
						$datavoid['tokenid'] = $void['tokenid'];
						$payment_response = $this->payment->voidPaymentAuth($datavoid);
					}else{
						$payment_response = $this->payment->voidPayment($datavoid);
					}


				}
			}

			$this->db->query("update salesorders set orderstatusid = 1 where orderno = '{$this->request->input['orderno']}'");
		}

		$this->response->setJsonOutput($payment_response);
	}

	// order_status_id	language_id	name
	// 1	1	Pending
	// 5	1	Picking
	// 20	1	Printed
	// 21	1	Picked
	// 22	1	Loading
	// 23	1	Picking
	// 29	1	Dispatched
	// 30	1	Shipped
	// 31	1	On Route
	// 40	1	Delivered
	// 50	1	Printed
	// 80	1	Credit Hold
	// 90	1	Cancelled
	/**
	 * Record that a COD customer has been phoned about this order, or clear it.
	 *
	 * POST { orderno, called }. Advisory only — nothing downstream refuses to pick,
	 * dispatch or invoice an order that nobody called; the flag exists so the
	 * warehouse can see the call was made instead of chasing the office.
	 *
	 * Salesmen are read-only here, as they are on updatestatus(): the call is an
	 * office/AR job and the flag is what the floor trusts. The userid is stamped
	 * from the session, never from the payload.
	 */
	public function codcalled()
	{
		$input = $this->request->input;
		$response = array('success' => false);

		if (!empty($this->session->data['issalesman'])) {
			$response['message'] = 'Not permitted.';
			$this->response->setJsonOutput($response);
			return;
		}

		$orderno = isset($input['orderno']) ? (int) $input['orderno'] : 0;
		if ($orderno <= 0) {
			$response['message'] = 'No order number supplied.';
			$this->response->setJsonOutput($response);
			return;
		}

		$called = !empty($input['called']) ? 1 : 0;
		$userid = isset($this->session->data['userid']) ? $this->session->data['userid'] : '';

		$this->load->model('oms/orders');
		if (!$this->model_oms_orders->setCodCalled($orderno, $called, $userid)) {
			// Not a MOOV tenant — the screens never show the control there, so this is
			// only reachable by hand. Say so plainly rather than reporting a write
			// that did not happen.
			$response['message'] = 'COD call tracking is not enabled here.';
			$this->response->setJsonOutput($response);
			return;
		}

		$response['success'] = true;
		$response['orderno'] = $orderno;
		$response['cod_called'] = $called;
		$response['cod_called_by'] = $userid;
		$response['cod_called_at'] = date('Y-m-d H:i:s');

		$this->response->setJsonOutput($response);
	}

	public function updatestatus()
	{
		$input = $this->request->input;
		$response['success'] = false;

		if (!empty($this->session->data['issalesman'])) {
			$this->response->setJsonOutput($response);
			return;
		}

		if (!empty($input)) {
			$orderno = $input['trans']['orderno'];

			if ($orderno) {
				switch ($input['type']) {
					//logic actions
					case '90':
						$this->db->query("update salesorderdetails set completed = 1 where orderno ='$orderno'");
						$this->db->query("delete from pickingfreeze where orderno ='$orderno'");
						$this->shop->getUrlContentNoResponse(LOCAL_SOCKET . 'push/dispatch');
						break;
					//not picked status
					case '1':
					case '2':
					case '5':
					case '20':

						$this->db->query("delete from dispatch where invoice = '$orderno' ");
						$this->load->controller('weberp/item/pickorder/freezeorder', array($orderno));
						$this->db->query("update pickingfreeze set picked = 0 , completed = 0 where orderno = '$orderno' ");
						$this->db->query("update salesorders set printedpackingslip = 0 where orderno = '$orderno' ");

						$this->shop->getUrlContentNoResponse(LOCAL_SOCKET . 'push/dispatch');
						//requires socket call
						break;

					//picked / loading / loaded
					case '21':
					case '24':
					case '29':
					case '40':
					case '22':
						// 21	1	Picked
						$this->load->controller('weberp/item/pickorder/freezeorder', array($orderno));
						$this->db->query("update pickingfreeze set picked = pick , pickstatus='23' where orderno = '$orderno' ");
						//reshuffle 781165
						$this->shop->getUrlContentNoResponse(LOCAL_SOCKET . 'push/dispatch');

						break;
				}

				if ($input['type'] && $input['trans']['orderno']) {
					$this->db->query("update salesorders set orderstatusid = '{$input['type']}' where orderno = '$orderno'");
					
						if(isset($input['removepo']) && $input['removepo']){
							$this->load->model('weberp/purchasing');
							$po = $this->model_weberp_purchasing->getdropshippo($orderno);


							if(isset($po) && $po){
								$data = ['header'=>['purchase_orderno'=>$po]];
								$this->model_weberp_purchasing->cleanupdropship($data);
							}

						}

					$response['success'] = true;
				}
			}
		}
		$this->response->setJsonOutput($response);
	}
	public function getstatuses()
	{
		$q = $this->db->query("select * from order_status order by order_status_id asc");
		$this->response->setJsonOutput($q->rows);
	}

	public function loadorder(){
		$this->load->model('weberp/cashreceipts');
		$this->load->model('marketing/coupon');
		$this->load->model('weberp/intropromo');

		$data = file_get_contents("php://input");
		$data = json_decode($data, true);
		$input['userid'] = $this->request->get['userid'];
		$orderno = $this->request->get['oid'];
		$reverse = ($this->request->get['reverse'] == 'true') ? true : false;

		//todo! DONT DO THIS. Undu custom and make toggle
		switch ($this->config->get('db_database')) {
			case 'aa':
				$order_details = $this->db->query("SELECT
					salesorderdetails.*,
					stockmaster.lineid AS truebin,
					CASE
						WHEN salesorderdetails.parentlineno IS NULL THEN stockmaster.stockid
						ELSE stockmaster.lineid
					END AS sortbin
				FROM
					salesorderdetails
				LEFT JOIN
					stockmaster
				ON
					salesorderdetails.stkcode = stockmaster.stockid
				WHERE
					salesorderdetails.orderno = '$orderno'
					AND salesorderdetails.completed = 0
				ORDER BY
					/* Sort by sortbin to group similar bins */
					sortbin ASC,
					/* Group parent and child lines */
					COALESCE(salesorderdetails.parentlineno, salesorderdetails.orderlineno) ASC,
					/* Parent lines first */
					CASE
						WHEN salesorderdetails.parentlineno IS NULL THEN 0
						ELSE 1
					END ASC,
					/* Within parent and child lines, sort by stockid */
					CASE
						WHEN salesorderdetails.parentlineno IS NOT NULL THEN
							CONCAT('0', sortbin, salesorderdetails.stkcode, salesorderdetails.orderlineno)
						ELSE '1'
					END ASC,
					/* Sort by stockid for consistent grouping */
					salesorderdetails.stkcode ASC,
					/* Finally, sort by orderlineno */
					salesorderdetails.orderlineno ASC");
				break;
			case 'hairlines':
				$order_details = $this->db->query("SELECT
					salesorderdetails.*,
					(SELECT
						CASE
						  WHEN bin IS NULL OR bin = '' OR bin = 'NEED' THEN '' ELSE bin
						END
					 FROM locstock
					 WHERE stockid = salesorderdetails.stkcode
					   AND loccode = sa.fromstkloc
					  AND (bin <> '' OR bin = 'NEED') limit 1) AS bin
				FROM
					salesorderdetails
						join  salesorders sa on sa.orderno = salesorderdetails.orderno
				WHERE
					salesorderdetails.orderno = '$orderno'
					AND completed = 0
				ORDER BY
					bin ASC, orderlineno ASC
					");
				break;
			default:
				$order_details = $this->db->query("select * from salesorderdetails where orderno='$orderno' and completed = 0 ");
				break;
		}

		$header = $this->db->query("select s.*, t.*, c.contactname from salesorders s left join custbranch c on s.debtorno = c.debtorno and c.branchcode = s.branchcode left join trackinginfo t on s.orderno = t.transno where orderno='$orderno' ");
		$order_header = $header->row;
		$find = array(
			'{brname}',
			'{address1}',
			'{address2}',
			'{address3}',
			'{address4}',
			'{address5}',
			//'{address6}'
		);
		$format = '{brname}' . "\n" . '{address1}' . "\n" . '{address2}' . "\n" . '{address3}, {address4} {address5}';

		$order_header['deliverto'] = $this->shop->stripslashes_deep($order_header['deliverto']);


		$replace = array(
			'name' => $this->shop->stripslashes_deep($order_header['deliverto']),
			'address1' => $this->shop->stripslashes_deep($order_header['deladd1']),
			'address2' => $this->shop->stripslashes_deep($order_header['deladd2']),
			'address3' => $this->shop->stripslashes_deep($order_header['deladd3']),
			'address4' => $this->shop->stripslashes_deep($order_header['deladd4']),
			'address5' => $this->shop->stripslashes_deep($order_header['deladd5']),
			'address6' => $this->shop->stripslashes_deep($order_header['deladd6']),
		);

		$order_header['notes'] = $this->shop->stripslashes_deep(stripcslashes($order_header['notes']));
		$order_header['comments'] = $this->shop->stripslashes_deep(stripcslashes($order_header['comments']));

		$order_header['shipto'] = str_replace(array("\r\n", "\r", "\n"), '<br />', preg_replace(array("/\s\s+/", "/\r\r+/", "/\n\n+/"), '<br />', trim(str_replace($find, $replace, $format))));


		$sessionid = $order_header['debtorno'] . $input['userid'] . $order_header['branchcode'];
		//clear cart first and reload data
		$this->db->query("delete from cart where customer_id = '{$order_header['debtorno']}' and branch='{$order_header['branchcode']}' and session_id='$sessionid' ");
		//get tracking
		$trackingnumbers = $this->db->query("select trackingnumber as number ,type_id as shipper, shippername, bind from trackinginfo join shippers on type_id = shipper_id where transno ='".$orderno."'")->rows;
		if(!isset($trackingnumbers)){
			$trackingnumbers = [];
		}
		//rebuild cart

		$this->load->model('marketing/coupon');
		$cart_items = array();

		// events_history.option/orderlineno only exist on tenants running education/event
		// items (AA).  Elsewhere (MOOV) the table is the base schema and an unguarded
		// select fatals 1054, which kills loadorder and takes order edit down entirely.
		$has_event_options = (bool) $this->db->query(
			"SHOW COLUMNS FROM events_history LIKE 'option'"
		)->num_rows;

		foreach ($order_details->rows as $line) {
			if ($this->config->get('BackOrdersAllowed')) {
				//this is going to be an issue
// 				$line['quantity'] = $line['quantity'] - $line['qtyinvoiced'];
//
// 				if($line['quantity'] < 0) {
// 					$line['quantity'] = 0;
// 				}
			}
			$taxes = '';

			$decodedTaxOverride = json_decode($line['taxoverride'], true);

			if ($line['taxoverride'] && $decodedTaxOverride) {
				// Deduplicate taxes by taxauthid to prevent duplicate tax entries
				$uniqueTaxes = array();
				foreach($decodedTaxOverride as $tax) {
					if (!isset($uniqueTaxes[$tax['taxauthid']])) {
						$uniqueTaxes[$tax['taxauthid']] = $tax;
					}
				}
				$taxes_decoded = array_values($uniqueTaxes);
				$taxes = json_encode($taxes_decoded);
			}


			// This will be true if `taxoverride` is a valid JSON array, even if it's empty.
			if ($line['taxoverride'] !== null && $decodedTaxOverride !== null && is_array($decodedTaxOverride)) {
				// Deduplicate taxes by taxauthid to prevent duplicate tax entries
				$uniqueTaxes = array();
				foreach($decodedTaxOverride as $tax) {
					if (!isset($uniqueTaxes[$tax['taxauthid']])) {
						$uniqueTaxes[$tax['taxauthid']] = $tax;
					}
				}
				$taxes = json_encode(array_values($uniqueTaxes));
			}

			$item = array(
				//'api_id' => $data['customer']['debtorno'],
				'customer_id' => $order_header['debtorno'],
				'branch' => $order_header['branchcode'],
				'session_id' => $order_header['debtorno'] . $input['userid'] . $order_header['branchcode'],
				'stockid' => $this->db->escape($line['stkcode']),
				'quantity' => $line['quantity'],
				'price' => $line['unitprice'],
				'discount' => ($line['discountpercent']),
				'date_added' => date('Y-m-d H:i:s'),
				'userid' => $this->db->escape($data['userid']),
				'orderno' => $line['orderno'],
				'orderlineno' => $line['orderlineno'],
				'userid' => $this->db->escape($input['userid']),
				'porequired' => $line['porequired'],
				'po_options' => $this->db->escape($line['po_options']),
				'description' => $this->db->escape($line['itemdesc']),
				'notes' => $this->db->escape($this->shop->stripslashes_deep($line['narrative'])),
				'coupon_used'=> $order_header['coupon_used'],
				'parent_id' => ($cart_items[$line['parentlineno']] !== null && $line['parentlineno']) ? $cart_items[$line['parentlineno']] : null,
				'taxoveride' => $taxes,
				'tracking' => json_encode($trackingnumbers)
			);


			if ($has_event_options) {
				$isevent = $this->db->query("select `option` from events_history where orderlineno = '".$line['orderlineno']."' and order_id = '".$line['orderno']."' limit 1");
				if(isset($isevent->row['option'])){
					$item['option'] = $this->db->escape($isevent->row['option']);
				}
			}

			if ($item['stockid'] == 'COUPON') {
				$item['option'] = $this->model_marketing_coupon->getCouponByCode($order_header['coupon_used']);
				$coupon = $this->model_marketing_coupon->getCouponByCode($order_header['coupon_used']);
				$taxable = $coupon['taxable'];
				if($taxable){
					$item['taxoveride'] = '';
				}
			}

			$ispromo = $this->model_weberp_intropromo->getPromo($line['stkcode']);

			if ($ispromo) {
				$item['option'] = $this->db->escape($ispromo['options']);
			}

			$parent_id = $this->shop->cartInsertItem($item);
			$cart_items[$line['orderlineno']] = $parent_id;
		}

		$cart = $this->getCart($order_header['debtorno'], $order_header['branchcode']);

		if ($reverse) {
			$cart = $this->posreversecart($cart);
		}

		$preselected = $this->model_weberp_cashreceipts->getOrderSelectedPaymentMethod($order_header['orderno']);
		$order_header['conname'] = $this->shop->stripslashes_deep($order_header['contactname']);

		$this->load->model('weberp/vehicles');
		$vehicle = $this->model_weberp_vehicles->getVehicleFromOrder($orderno);
		if(isset($vehicle['id'])){
			$order_header['vehicle'] = $vehicle;
		}

		require_once DIR_SYSTEM . 'library/tpl3pl/OrderEditPolicy.php';
		require_once DIR_SYSTEM . 'library/tpl3pl/Tpl3plPgsStatusMassSync.php';
		Tpl3plPgsStatusMassSync::refreshActiveQueueRowFromPgsForOrderHeader($this->db, $this->registry, $orderno);
		Tpl3plOrderEditPolicy::attachTpl3plFieldsToOrderHeader($this->db, $order_header);

		$result = array(
			'header' => $order_header,
			'details' => $order_details->rows,
			'cart' => $cart,
			'preselected' => $preselected,
		);

		$this->response->setJsonOutput($result);
	}

	public function ispreorder()
	{

		$input = $this->request->input;
		$orderno = $input['oid'];

		$pc = $this->db->query("select orderstatusid from salesorders where orderno = '" . $orderno . "'");
		$status = $pc->row['orderstatusid'];

		if ($status == 3) {
			$po = true;
		} else {
			$po = false;
		}
		$res = array('ispo' => $po);
		$this->response->setJsonOutput($res);
	}

	public function tracking($data = false) {

		if($data){
			$orderno = $data['orderno'];
			$tracknum = $data['tracking'];
			$submit = $data['submit'];
			$update = $data['update'];
			$remove = $data['remove'];
			$bind = isset($data['bind']) ? $data['bind'] : 0;
			$orig_track = $data['originaltrack'];
			$tracking_type = $data['tracktype'];
			$url = isset($data['url']) ? (string)$data['url'] : '';
		}else{

			$orderno = $this->request->input['orderno'];
			$tracknum = $this->request->input['tracking'];
			$submit = $this->request->input['submit'];
			$update = $this->request->input['update'];
			$remove = $this->request->input['remove'];
			$orig_track = $this->request->input['originaltrack'];
			$tracking_type = $this->request->input['tracktype']; //shipper id
			$url = isset($this->request->input['url']) ? (string)$this->request->input['url'] : '';
			$bind = isset($this->request->input['bind']) ? $this->request->input['bind'] : 0;
		}
		$results = array(
			"success" => true
		);

		//get order details
		if(isset($orderno)){
			$this->load->model('oms/orders');
			$this->load->model('weberp/items');

			$orderinfo = $this->model_oms_orders->getOrder($orderno);

			if(isset($orderinfo) && count($orderinfo)){
				$results['orderInfo'] = $orderinfo;

				$items = $this->model_oms_orders->getOrderDetails($orderno);
				$results['items'] = $items;

				$totals = $this->model_oms_orders->getOrderTotals($orderno);
				$results['totals'] = $totals;

				$branch = $this->model_weberp_items->getBranch($orderinfo['debtorno'], $orderinfo['branchcode']);
				$results['branchInfo'] = $branch;
			}else{
				$results['message'] = 'Order not found';
				$results['success'] = false;
			}
		}

		//real order and submitting tracking #
		if(isset($orderinfo) && isset($tracknum) && $submit){
			$check = $this->db->query("select * from trackinginfo where transno = '".$this->db->escape($orderno)."' and trackingnumber = '".$this->db->escape($tracknum)."'")->row;
			if(!isset($check['trackingnumber'])){
				$now = date('Y-m-d H:i:s');
				$today = date('Y-m-d');
				$trackrow = array(
					'trackingnumber' => $this->db->escape($tracknum),
					'transno'=> $this->db->escape($orderno),
					'shipstatus' => 0,
					'url' => $this->db->escape($url),
					'datemon' => $now,
					'lasttime' => $now,
					'linkdate'=>$today,
					'estdeldate' => $today,
					'type_id' => $this->db->escape($tracking_type),
					'bind' => (int)$bind
				);
				$keys = implode(', ', array_keys($trackrow));
				$vals = "'" . implode("', '", array_values($trackrow)) . "'";
				$sql = "insert into trackinginfo ($keys) values($vals)";
				$results['sql'] =$sql;
				$this->db->query($sql);
			}else{
				$results['success'] = false;
			}
		}

		//real order and updating tracking #
		if(isset($orderinfo) && isset($orig_track) && isset($tracknum) && $update){
			$sql = "update trackinginfo set trackingnumber = '".$this->db->escape($tracknum)."', type_id = '".$this->db->escape($tracking_type)."', url = '".$this->db->escape($url)."' where transno = '".$this->db->escape($orderno)."' and trackingnumber = '".$this->db->escape($orig_track['trackingnumber'])."'";
			$results['sql'] =$sql;
			$this->db->query($sql);
		}

		//real order and removing tracking #
		if(isset($orderinfo) && isset($orig_track) && $remove){
			$sql = "delete from trackinginfo where trackingnumber = '".$this->db->escape($orig_track['trackingnumber'])."' and transno = '".$this->db->escape($orig_track['transno'])."' and id = '".$this->db->escape($orig_track['id'])."'";
			$results['sql'] = $sql;
			$this->db->query($sql);
		}

		//most recent tracking #s
		if(isset($orderinfo)){
			$tracking = $this->model_oms_orders->getOrderTrackingNumber($orderno);
			$results['orderInfo']['tracking'] = $tracking;
		}

		if($data){
			return $results;
		}else{
			$this->response->setJsonOutput($results);
		}
	}

	//preorder func
	public function loadpreorder()
	{
		$this->load->model('weberp/cashreceipts');
		$this->load->model('marketing/coupon');
		$this->load->model('weberp/intropromo');
		$this->load->model('weberp/item/specialprice');
		$this->load->model('weberp/items');
		$this->load->model('oms/orders');

		$input = $this->request->input;

		$input['userid'] = $this->request->get['userid'];
		$orderno = $input['oid'];
		$customer = $input['customer'];
		$user = $input['userid'];
		$reverse = ($input['reverse'] == 'true') ? true : false;


		$order_details = $this->db->query("select * from salesorderdetails where orderno='$orderno' and completed = 0 ");
		$header = $this->db->query("select * from salesorders where orderno='$orderno'");
		$order_header = $header->row;
		$orderlines = $order_details->rows;

		$find = array(
			'{brname}',
			'{address1}',
			'{address2}',
			'{address3}',
			'{address4}',
			'{address5}',
			//'{address6}'
		);

		$format = '{brname}' . "\n" . '{address1}' . "\n" . '{address2}' . "\n" . '{address3}, {address4} {address5}';
		$replace = array(
			'name' => $order_header['deliverto'],
			'address1' => $order_header['deladd1'],
			'address2' => $order_header['deladd2'],
			'address3' => $order_header['deladd3'],
			'address4' => $order_header['deladd4'],
			'address5' => $order_header['deladd5'],
			'address6' => $order_header['deladd6'],
		);

		$order_header['notes'] = stripcslashes($order_header['notes']);
		$order_header['comments'] = stripcslashes($order_header['comments']);

		$order_header['shipto'] = str_replace(array("\r\n", "\r", "\n"), '<br />', preg_replace(array("/\s\s+/", "/\r\r+/", "/\n\n+/"), '<br />', trim(str_replace($find, $replace, $format))));


		$sessionid = $order_header['debtorno'] . $user . $order_header['branchcode'];
		//clear cart first and reload data
		$this->db->query("delete from cart where customer_id = '{$order_header['debtorno']}' and branch='{$order_header['branchcode']}' and session_id='$sessionid' ");
		//rebuild cart


		$order_header['salestype'] = $order_header['ordertype'];

		$itemSearch = array(
			"customer" => $order_header,
			"stockid" => ''
		);

		$reprice = array(
			'items' => array(),
			'promos' => array()
		);

		$children_passed = array(
		);

		$this->load->model('weberp/batchupload');
		$cart_items = array();
		$saleprice = false;

		foreach ($orderlines as $line) {

			$originalPrice = $line['unitprice'];

			$itemSearch['stockid'] = $line['stkcode'];



			$decodedTaxOverride = json_decode($line['taxoverride'], true);

			if ($line['taxoverride'] && $decodedTaxOverride) {
				// Deduplicate taxes by taxauthid to prevent duplicate tax entries
				$uniqueTaxes = array();
				foreach($decodedTaxOverride as $tax) {
					if (!isset($uniqueTaxes[$tax['taxauthid']])) {
						$uniqueTaxes[$tax['taxauthid']] = $tax;
					}
				}
				$taxes_decoded = array_values($uniqueTaxes);
				$taxes = json_encode($taxes_decoded);
			}


			// This will be true if `taxoverride` is a valid JSON array, even if it's empty.
			if ($line['taxoverride'] !== null && $decodedTaxOverride !== null && is_array($decodedTaxOverride)) {
				// Deduplicate taxes by taxauthid to prevent duplicate tax entries
				$uniqueTaxes = array();
				foreach($decodedTaxOverride as $tax) {
					if (!isset($uniqueTaxes[$tax['taxauthid']])) {
						$uniqueTaxes[$tax['taxauthid']] = $tax;
					}
				}
				$taxes = json_encode(array_values($uniqueTaxes));
			}


			$itemData = $this->model_oms_orders->getOrderEntryItem($itemSearch);

			if (!$itemData['stockid']) {
				$itemData['stockid'] = $line['stkcode'];
			}

			$ispromo = $this->shop->getPromo($itemData['stockid']);

			if (isset($ispromo) && $ispromo !== false) {
				if ($ispromo['options']) {
					$promo = array(
						'branchcode' => $order_header['branchcode'],
						'customer' => $order_header['debtorno'],
						'promo' => array(
							'itemdesc' => array(
								'stockid' => $itemData['stockid']
							),
							'qty' => $line['quantity']
						)
					);

					$opts = json_decode($ispromo['options'], true);

					if (isset($opts)) {
						foreach ($opts as $option) {
							$item_id = '';

							if (isset($option['item'])) {
								$item_id = $option['item'];
							}

							if (isset($option['stockid'])) {
								$item_id = $option['stockid'];
							}

							array_push($children_passed, $item_id);
						}
					}
					$reprice['promos'][] = $promo;
					//continue;
				}
				$itemData['quantity'] = $line['quantity'];
			}
			//Get salestype since $customer isn't always set for some goddessforsaken reason.
			$st = $this->db->query("select salestype from debtorsmaster where debtorno = '" . $order_header['debtorno'] . "'");
			$custsalestype = $st->row['salestype'];
			$specials = $this->model_weberp_item_specialprice->getSpecialPrice($itemData['stockid'], $custsalestype); // SALON placeholder, replace with customer

			if (isset($specials) && $specials) {
				if (in_array($itemData['stockid'], $children_passed)) {
					unset($children_passed[array_search($itemData['stockid'], $children_passed)]);
					// Instead of continue, use else to handle remaining logic
				} else {
					//get special price
					$itemData['quantity'] = $line['quantity'];
					$itemData['price'] = $specials;
					$reprice['items'][] = $itemData;
				}
				if ($originalPrice == 0) {
					$itemData['price'] = $originalPrice;
				}
			}


			$this->load->model('weberp/storesale');

			$saleprice = $this->model_weberp_storesale->getStoreSalePrice($itemData['stockid'], $order_header['debtorno'], $order_header['fromstkloc']);

			if ($saleprice && number_format($saleprice['price'], 2, '.', '') != '0.00') {
				$itemData['originalPrice'] = $item['price'];
				$itemData['price'] = $saleprice['price'];
				$itemData['specialflag'] = 1;
				$itemData['quantity'] = $line['quantity'];
				if ($originalPrice == 0) {
					$itemData['price'] = $originalPrice;
				}

			}


			if (!$specials && !$ispromo && !$saleprice) {

				//todo come back and .. from order entry
				$nodiscount = array('EDF1');

				if (!in_array($itemData['stockid'], $nodiscount)) {
					if ($order_header['ordertype'] == 'M') {
						$matrix = $this->shop->getMatrixPriceForCustomer($order_header['debtorno'], $itemData['stockid']);
						$changeprice = $this->db->query("SELECT price FROM prices WHERE stockid='{$itemData['stockid']}' AND typeabbrev ='{$matrix['pricing_level']}' LIMIT 1");

						if ($matrix['discount']) {
							$discount = ($matrix['discount'] / 100) * $changeprice->row['price'];
							$itemData['price'] = $changeprice->row['price'] - number_format($discount, 2, '.', '');
						}
					} elseif ($order_header['ordertype'] == 'O') {
						$altprice = $this->shop->getAlternateCustPrice($order_header['debtorno'], $itemData['stockid']);
						$itemData['price'] = $altprice;
					} else {
						$plmprice = $this->model_weberp_batchupload->getPLMPrice($itemData['stockid'], $order_header['ordertype']);

						if ($plmprice != '0.00') {
							$itemData['price'] = $plmprice;
						} else {
							$itemData['price'] = $originalPrice;
						}
					}
				} else {
					$plmprice = $this->model_weberp_batchupload->getPLMPrice($itemData['stockid'], $order_header['ordertype']);

					if ($plmprice != '0.00') {
						$itemData['price'] = $plmprice;
					} else {
						$itemData['price'] = $originalPrice;
					}
				}

				if (!$itemData['price']) {
				}

				if (($cart_items[$line['parentlineno']] !== null && $line['parentlineno'] !== null)) {

					$ispromo = $this->shop->getPromo($cart_items[$line['parentlineno']]['stockid']);
					if ($originalPrice == 0 || !$ispromo) {
						$itemData['price'] = $originalPrice;
					}
				}

				$itemData['quantity'] = $line['quantity'];
				$itemData['orderno'] = $order_header['orderno'];
			}


			$item = array(
				//'api_id' => $data['customer']['debtorno'],
				'customer_id' => $order_header['debtorno'],
				'branch' => $order_header['branchcode'],
				'session_id' => $order_header['debtorno'] . $input['userid'] . $order_header['branchcode'],
				'stockid' => $this->db->escape($line['stkcode']),
				'quantity' => $itemData['quantity'],
				'price' => $itemData['price'],
				'discount' => ($line['discountpercent']),
				'date_added' => date('Y-m-d H:i:s'),
				'userid' => $this->db->escape($data['userid']),
				'orderno' => $line['orderno'],
				'orderlineno' => $line['orderlineno'],
				'userid' => $this->db->escape($input['userid']),
				'porequired' => $line['porequired'],
				'po_options' => $this->db->escape($line['po_options']),
				'description' => $this->db->escape($line['itemdesc']),
				'notes' => $this->db->escape($line['narrative']),
				'parent_id' => ($cart_items[$line['parentlineno']] !== null) ? $cart_items[$line['parentlineno']] : null,
				'taxoveride' => $taxes,
			);

			if ($item['stockid'] == 'COUPON') {
				$item['coupon_used'] = $this->model_marketing_coupon->getOrderCoupon($orderno, $line['orderlineno']);
			}

			$ispromo = $this->model_weberp_intropromo->getPromo($line['stkcode']);

			if ($ispromo) {
				$item['option'] = $this->db->escape($ispromo['options']);
			}

			$parent_id = $this->shop->cartInsertItem($item);

			$cart_items[$line['orderlineno']] = $parent_id;
		}

		$cart = $this->getCart($order_header['debtorno'], $order_header['branchcode']);

		if ($reverse) {
			$cart = array_reverse($cart);
		}

		$preselected = $this->model_weberp_cashreceipts->getOrderSelectedPaymentMethod($order_header['orderno']);

		$result = array(
			'header' => $order_header,
			'details' => $order_details->rows,
			'cart' => $cart,
			'cart_items' => $cart_items,
			'preselected' => $preselected,
		);


		$this->response->setJsonOutput($result);
	}


	public function getCart($customer, $branch, $addon = array())
	{

		if ($addon['location'] && $addon['location'] != '') {
			$this->session->data['location'] = $addon['location'];
		}

		$results = array();
		$user = $this->session->data['userid'];
		$sessionid = $customer . $user . $branch;
		$sessionsql = " and session_id='$sessionid'  ";
		if ($addon['cartcard']) {
			$sessionsql = '';
		}

		$this->session->data['debtor'] = $customer;
		$this->session->data['branch'] = $branch;

		$items = $this->db->query("select * from cart where customer_id='$customer' and branch='$branch' $sessionsql order by cart_id asc, parent_id asc");

		$this->load->model('catalog/product');
		$this->load->model('weberp/items');
		$this->load->model('tool/image');
		$this->load->model('marketing/coupon');
		$results = array();

		foreach ($items->rows as $item) {
			$details = $this->model_catalog_product->getProduct($item['stockid']);

			//$onhand = $this->model_catalog_product->getCostAndStock($item['stockid']);
			$minqty = 0;
			$onhand = $this->shop->getStock($item['stockid'], $minqty);

			if ($this->config->get('non_stock_item') == $item['stockid']) {
				$options = json_decode($item['po_options'], true);
				if (!empty($options) && $options) {
					if (isset($options['brand']['name']) && $options['brand']['name'] != '') {
						$item['description'] = $options['brand']['name'] . ' ' . $item['description'];
					}

					if (isset($options['barcode']) && $options['barcode'] != '') {
						$bar_desc = $options['barcode'];
						if (isset($options['partno']) && $options['partno'] != '') {
							$item['notes'] = $item['notes'] . ' #' . $options['partno'] . ' Barcode: ' . $bar_desc;
						} else {
							// Only add the barcode if partno is not available
							$item['notes'] = $item['notes'] . ' Barcode: ' . $bar_desc;
						}
					} elseif (isset($options['partno']) && $options['partno'] != '') {
						// Only add the partno if barcode is not available
						$item['notes'] = $item['notes'] . ' #' . $options['partno'];
					}
				}
			}


			$item['taxcatid'] = $details['tax_class_id'];
			$newTaxes = array();
			$alltaxauths = array();

			if ($item['taxoveride'] && json_decode($item['taxoveride'], true) && !empty(json_decode($item['taxoveride'], true))) {
				$line_tax_rates = json_decode($item['taxoveride'], true);
			} else {
				$line_tax_rates = $this->sales->getItemTaxes(array(
					'stockid' => $item['stockid'],
					'taxcatid' => $details['tax_class_id'],
				));
			}

			foreach ($line_tax_rates as $tax) {
				$tax['taxrate'] = (float) $tax['taxrate'];
				$alltaxauths[$tax['taxauthid']] = $tax;
			}

			foreach ($line_tax_rates as $tax) {
				$newTaxes[] = $tax;
			}

			$total_available = 0;

			foreach ($onhand as $qh) {
				$total_available += $qh['quantity'];
			}

			$qos = $this->shop->getOnSalesOrder($item['stockid'], $qh['loccode']);
			$avail = $total_available - $qos;
			$total_available = ($avail < 0) ? 0 : $avail;

			$product_type = $this->model_weberp_items->getProductType($details['mbflag']);

			$qohtwo = array();

			foreach ($onhand as $ob) {
				$qohtwo[] = $ob;
			}

			$image_f = $details['image'];

			if ($image_f && $image_f != '') {
				$thumb = $this->model_tool_image->resize('' . $image_f, 100, 100);
			} else {
				$thumb = $this->model_tool_image->resize('no_image.png', 100, 100);
			}

			$mstitem = $this->config->get('non_stock_item');

			if ($mstitem == $item['stockid']) {
				$options = is_array($item['po_options']) ? $item['po_options'] : json_decode($item['po_options'], true);

				//check to see if po options have been previously modified and skips the formatting if so.
				//double formatting drops a bunch of fields on reloading / updating of an order causing for required po creation to break
				if ($options['supplier']['supplierid']) {
					$item['po_options'] = array(
						'supplierno' => isset($options['supplier']['supplierid']) ? $options['supplier']['supplierid'] : $options['supplier'],
						'stockid' => $options['stockid'],
						'price' => number_format($options['cost'], 2, '.', ''),
						'suppliersuom' => 'each',
						'conversionfactor' => 1,
						'supplierdescription' => $this->db->escape($options['description']),
						'preferred' => 1,
						'effectivefrom' => date('Y-m-d'),
						'suppliers_partno' => $this->db->escape($options['partno']),
						'preferred_sort' => 1,
						'supplierid' => $this->db->escape($options['supplier']['supplierid']),
						'suppname' => $this->db->escape($options['supplier']['suppname']),
						'address1' => $this->db->escape($options['supplier']['address1']),
						'address2' => $this->db->escape($options['supplier']['address2']),
						'address3' => $this->db->escape($options['supplier']['address3']),
						'address4' => $this->db->escape($options['supplier']['address4']),
						'address5' => $this->db->escape($options['supplier']['address5']),
						'address6' => $this->db->escape($options['supplier']['address6']),
						'currcode' => $this->db->escape($options['supplier']['currcode']),
						'remote' => false,
						'brand' => $options['brand'] ?? null,
						'productline' => $options['productline'] ?? '',
					);
				}

				$details['description'] = $item['description'];
				$details['name'] = $item['description'];
				$details['stockid'] = $item['stockid'];
				$item['porequired'] = true;
			}

			if ($this->config->get("discounts")) {
				if ($item['discount']) {
					$item['hasdiscount'] = true;
					$item['discount'] = $item['discount'];
					$item['discountpercent'] = $item['discount'] * 100;
				}
			} elseif (!empty($item['discount']) && (float) $item['discount'] != 0) {
				$item['hasdiscount'] = true;
				$item['discountpercent'] = (float) $item['discount'] * 100;
			} else {
				$item['hasdiscount'] = false;
				$item['discountpercent'] = 0;
				$item['discount'] = 0;
			}

			$total = (($item['price'] - ($item['price'] * $item['discount'])) * $item['quantity']);
			$total = number_format($total, 2, '.', '');

			$taxable = 1;
			if ($item['stockid'] == 'COUPON') {
				$coupon = $this->model_marketing_coupon->getCouponByCartId($item['cart_id']);
				$taxable = (int) $coupon['taxable'];
			}

			$tax_total = 0.00;
			if ($taxable) {
				if ((int) $this->config->get('ny_b2b_tax_engine') === 1) {
					if (!class_exists('Weberp_OrderTaxEngine')) {
						require_once DIR_SYSTEM . 'library/weberp/tax/order_tax_engine.php';
					}
					$pot_product = array(
						'quantity' => (float) $item['quantity'],
						'thc_mg_per_unit' => isset($details['thc_mg_per_unit']) ? (float) $details['thc_mg_per_unit'] : 0.0,
					);
					foreach (Weberp_OrderTaxEngine::allocateLineTaxRows($this->sales, (float) $total, $line_tax_rates, $pot_product) as $r) {
						$tax_total += (float) $r['value'];
					}
				} else {
					foreach ($line_tax_rates as $taxrate) {
						$tax_total += ((float) $taxrate['taxrate'] > 0) ? number_format((float) $total * (float) $taxrate['taxrate'], 2, '.', '') : 0.00;
					}
				}
				$tax_total = (float) number_format($tax_total, 2, '.', '');
			}

			//matrix removed?
			//if($matrix['discount']) {
			// $item['hasdiscount'] = false;
			// $item['discountpercent'] = 0;
			// $item['discount'] = 0;
			//$item['price'] =(float) number_format( ($item['price'] - ($item['price'] * $item['discountpercent'])) ,2 ,'.','');
			//}


			$details['description'] = ($details['name']) ? $details['name'] : 'N/A';
			//second attempt maybe third?
			if ($details['description'] == 'N/A') {
				$dec = $item['description'];

				if ($dec != '') {
					$details['description'] = $dec;
				}
			}
			if($details['stockid'] == 'COUPON'){
				$details['description'] = $item['description'];
			}
			if ($addon['forceupdate']) {
				$details['forceupdate'] = true;
			}
			$details['authorities'] = $this->sales->array_sort($alltaxauths, 'taxauthid', SORT_ASC);
			$details['notes'] = ($item['notes']) ? $item['notes'] : '';
			$details['manufacture_name'] = $details['manufacturer'];
			$details['thumb'] = $thumb;
			$details['price'] = (float) number_format($item['price'], 2, '.', '');
			$details['startprice'] = (float) number_format($item['price'], 2, '.', '');
			$details['quantity'] = (float) $item['quantity'];
			$details['discount'] = number_format(($item['discount'] * 100), 2, '.', '');
			$details['discountpercent'] = $item['discountpercent'];
			$details['cart_id'] = $item['cart_id'];
			$details['cartid'] = $item['cart_id'];
			$details['coupon_used'] = $item['coupon_used'];
			$details['parent_id'] = ($item['parent_id']) ? $item['parent_id'] : false; //explicit declaration of false or number.
			$details['qoh'] = (float) $total_available;
			$details['total'] = (float) $total;
			$details['orderno'] = $item['orderno'];
			$details['quoteno'] = $item['quoteno'];
			$details['orderlineno'] = $item['orderlineno'];
			$details['product_type'] = $product_type;
			$details['option'] = json_decode($item['option'], true);
			$details['overcredit_override'] = $item['override'];
			$details['flagged'] = $item['flagged'];
			$details['porequired'] = $item['porequired'];
			$details['po_options'] = is_array($item['po_options']) ? json_encode($item['po_options'], JSON_UNESCAPED_SLASHES) : $item['po_options'];
			$details['trackingnumbers'] = json_decode($item['tracking'], true);
			if($taxable){
				$details['taxdetails'] = $line_tax_rates;
				$details['taxes'] = $this->sales->array_sort($newTaxes, 'taxauthid', SORT_ASC);
				$details['taxtotal'] = $tax_total;
			}else{
				// $details['taxdetails'] = [];
				// $details['taxes'] = [];
				$details['taxtotal'] = 0;
			}

			if($item['stockid'] !== 'MST' && $item['stockid'] !== 'MISC'){
				$locs =  $this->shop->getStock($item['stockid']);
				$details['locunits'] = isset($locs) ? $locs : [];
			}

			$results[] = $details;
		}

		return $results;
	}

	public function inqsearch()
	{


		$json = $this->request->input;

		if (!is_array($json)) { $json = []; }

		// Merge in case caller is still sending some values via GET/POST
		$input = array_merge($json, $this->request->get, $this->request->post);
		$this->request->input = is_array($this->request->input) ? array_merge($input, $this->request->input) : $input;

		$results = [];

		// -------- Filters --------
		// Keywords
		$kw = isset($this->request->input['keywords']) ? (string)$this->request->input['keywords'] : '';
		$keywords = $this->db->escape(str_replace(' ', '%', $kw));

		// Salesman filter (when viewing as salesman)
		$salesman = '';
		if (!empty($this->request->input['issalesman'])) {
			$sales = $this->db->escape($this->request->input['salesman']);
			$salesman = " AND salesperson = '{$sales}' ";
		}

		// Order type filter
		if (isset($this->request->input['orderType']) && $this->request->input['orderType'] === 'O') {
			$otype = " AND (s.orderstatusid <= 29 OR s.orderstatusid >= 51) ";
		} else if (isset($this->request->input['orderType']) && $this->request->input['orderType'] === 'I') {
			$otype = " AND s.orderstatusid > 29 AND s.orderstatusid < 51 ";
		} else {
			$otype = '';
		}

		$custFilter = '';
		if (!empty($this->request->input['customerNumber'])) {
			$custRaw = trim((string)$this->request->input['customerNumber']);

			if (strpos($custRaw, '.') !== false) {
				// Split on decimal point into debtorno and branchcode
				list($custDebtor, $custBranch) = explode('.', $custRaw, 2);
				$custDebtor = $this->db->escape(trim($custDebtor));
				$custBranch = $this->db->escape(trim($custBranch));
				$custFilter = " AND s.debtorno = '{$custDebtor}' AND s.branchcode = '{$custBranch}' ";
			} else {
				// No decimal – just search by debtorno starts-with
				$cust = $this->db->escape($custRaw);
				$custFilter = " AND s.debtorno LIKE '{$cust}%' ";
			}
		}

		$dateFilter = '';
		$dateStart = isset($this->request->input['dateStart']) ? trim((string)$this->request->input['dateStart']) : '';
		$dateEnd   = isset($this->request->input['dateEnd'])   ? trim((string)$this->request->input['dateEnd'])   : '';

		if ($dateStart !== '' && $dateEnd !== '') {
			$ds = $this->db->escape($dateStart);
			$de = $this->db->escape($dateEnd);
			$dateFilter = " AND DATE(s.date_created) BETWEEN '{$ds}' AND '{$de}' ";
		} else if ($dateStart !== '') {
			$ds = $this->db->escape($dateStart);
			$dateFilter = " AND DATE(s.date_created) >= '{$ds}' ";
		} else if ($dateEnd !== '') {
			$de = $this->db->escape($dateEnd);
			$dateFilter = " AND DATE(s.date_created) <= '{$de}' ";
		}

		// -------- Queries --------
		// MAIN (salesorders)
		$sql = "
			SELECT
				s.*,
				CASE
					WHEN (
						SELECT order_ FROM debtortrans
						WHERE order_ = s.orderno AND type = 10
						LIMIT 1
					) IS NOT NULL THEN 'Invoiced'
					ELSE o.name
				END AS statusname
			FROM salesorders s
			JOIN order_status o ON s.orderstatusid = o.order_status_id
			WHERE
				(
					s.orderno    LIKE '%{$keywords}%'
					OR s.customerref LIKE '%{$keywords}%'
					OR s.deliverto   LIKE '%{$keywords}%'
					OR s.invoiceno   LIKE '%{$keywords}%'
				)
				{$salesman}
				{$otype}
				{$custFilter}
				{$dateFilter}
			ORDER BY s.date_created DESC
			LIMIT 50
		";

		$q = $this->db->query($sql);

		// CREDITS (creditorders)
		$sql2 = "
			SELECT
				s.*,
				CASE
					WHEN (
						SELECT order_ FROM debtortrans
						WHERE order_ = s.orderno AND type = 10
						LIMIT 1
					) IS NOT NULL THEN 'Invoiced'
					ELSE o.name
				END AS statusname,
				'credit' AS type
			FROM creditorders s
			JOIN order_status o ON s.orderstatusid = o.order_status_id
			WHERE
				(
					s.orderno    LIKE '%{$keywords}%'
					OR s.customerref LIKE '%{$keywords}%'
					OR s.deliverto   LIKE '%{$keywords}%'
					OR s.invoiceno   LIKE '%{$keywords}%'
				)
				{$salesman}
				{$otype}
				{$custFilter}
				{$dateFilter}
			ORDER BY s.date_created DESC
			LIMIT 50
		";

		$q2 = $this->db->query($sql2);

		$merged = array_merge($q->rows, $q2->rows);

		$results = [];
		foreach ($merged as $row) {
			$credit = (isset($row['type']) && $row['type'] === 'credit');
			$orderno = $row['orderno'];
			$ordernoEsc = $this->db->escape($orderno);

			// dispatch
			$dispatch = $this->db->query("SELECT * FROM dispatch WHERE invoice = '{$ordernoEsc}'");
			$dispatch_details = !empty($dispatch->row) ? $dispatch->row : [];

			// details + credit status fix (QOH: sum locstock at order fromstkloc — same as orders/open, not raw line rows)
			$location = isset($row['fromstkloc']) ? $row['fromstkloc'] : '';
			$locEsc = $this->db->escape($location);

			if ($credit) {
				$det = $this->db->query("
					SELECT *, creditorderdetails.quantity AS ordered, SUM(locstock.quantity) AS qoh,
						creditorderdetails.quantity - creditorderdetails.qtyinvoiced AS dispatch, locstock.bin
					FROM creditorderdetails
					LEFT JOIN locstock ON locstock.stockid = creditorderdetails.stkcode AND locstock.loccode = '{$locEsc}'
					LEFT JOIN stockmaster ON creditorderdetails.stkcode = stockmaster.stockid
					WHERE creditorderdetails.orderno = '{$ordernoEsc}'
					GROUP BY creditorderdetails.orderno, creditorderdetails.orderlineno
				");
				$invoiced = true;
				foreach ($det->rows as $line) {
					if ($line['qtyinvoiced'] != $line['ordered'] || empty($line['completed'])) {
						$invoiced = false;
						break;
					}
				}
				if ($invoiced) {
					$row['statusname'] = 'Credited';
					$row['orderstatusid'] = 'credited';
				}
			} else {
				$det = $this->db->query("
					SELECT *, salesorderdetails.quantity AS ordered, SUM(locstock.quantity) AS qoh,
						salesorderdetails.quantity - salesorderdetails.qtyinvoiced AS dispatch, locstock.bin
					FROM salesorderdetails
					LEFT JOIN locstock ON locstock.stockid = salesorderdetails.stkcode AND locstock.loccode = '{$locEsc}'
					LEFT JOIN stockmaster ON salesorderdetails.stkcode = stockmaster.stockid
					WHERE salesorderdetails.orderno = '{$ordernoEsc}'
					GROUP BY salesorderdetails.orderno, salesorderdetails.orderlineno
				");
			}

			// debtortrans id lookup (invoice/credit id)
			if (!empty($row['invoiceno'])) {
				if ($credit) {
					$qi = $this->db->query("SELECT id FROM debtortrans WHERE transno = '{$row['invoiceno']}' AND type = '11' LIMIT 1");
				} else {
					$qi = $this->db->query("SELECT id FROM debtortrans WHERE transno = '{$row['invoiceno']}' AND type = '10' LIMIT 1");
				}
				if (!empty($qi->row['id'])) {
					$row['id'] = $qi->row['id'];
				}
			}

			$row['transno'] = $row['invoiceno'];
			$row['payment_details'] = json_decode($row['payment_details'], true);
			$row['details'] = $det->rows;
			$row['dispatch'] = $dispatch_details;

			$results[] = $row;
		}

		$this->response->setJsonOutput($results);
	}

	public function search()
	{
		$data = file_get_contents("php://input");
		$data = json_decode($data, true) ?: array();
		$input = array_merge($data, $this->request->get, $this->request->post);
		$result = array();

		$keywords = $this->db->escape(str_replace(' ', '%', $this->request->get['keywords']));

		$q = $this->db->query("select * from salesorders
			where
				orderno like '$keywords%'
				or customerref like '%$keywords%'
				or deliverto like '%$keywords%'
			");

		$results = $q->rows;

		$this->response->setJsonOutput($results);
	}

	public function gettransaction()
	{
		$data = file_get_contents("php://input");
		$data = json_decode($data, true) ?: array();
		$input = array_merge($data, $this->request->get, $this->request->post);
		$result = array();

		$id = $this->request->get['id'];
		$q = $this->db->query("select * from debtortrans where id='$id'");
		$result = $q->row;

		$this->response->setJsonOutput($result);
	}

	public function getauths(){
		$data = file_get_contents("php://input");
		$data = json_decode($data, true) ?: array();		
		$input = array_merge($data, $this->request->get, $this->request->post);

		$this->load->model('weberp/orders');
		$results = [
			'bucket1'=> $this->model_weberp_orders->getPendingAuths('<', [3]),
			'bucket2'=> $this->model_weberp_orders->getPendingAuths('between', [3,6]),
			'bucket3'=> $this->model_weberp_orders->getPendingAuths('>=', [7])
		];

		$this->response->setJsonOutput($results);
	}

	public function getquotes()
	{
		$data = file_get_contents("php://input");
		$data = json_decode($data, true) ?: array();
		$input = array_merge($data, $this->request->get, $this->request->post);
		$results = array();
		$this->load->model('weberp/salesman');
		$this->load->model('oms/orders');
		$this->load->model('weberp/groups');
		$filters = array();

		if (isset($this->request->get['debtorno']) && $this->request->get['debtorno'] != '') {
			$filters['debtorno'] = $this->request->get['debtorno'];
		}

		if (isset($this->request->get['userid']) && $this->request->get['userid'] != '') {
			$salesman_check = $this->model_weberp_salesman->getSalesmanCodeByUser($this->request->get['userid']);

			if ($salesman_check != '' && isset($salesman_check)) {
				$filters['salesman'] = $salesman_check;
			}

			$pos_check = $this->model_weberp_groups->isPosUser($this->request->get['userid']);

			if ($pos_check != '' && isset($pos_check)) {
				$filters['posuser'] = $this->request->get['userid'];
			}
		}


		$quotes = $this->model_oms_orders->getOpenQuotes($filters);
		$this->response->setJsonOutput($quotes);
	}


	public function openmstsalesorders()
	{
		$this->load->model('weberp/items');
		$items = $this->model_weberp_items->getMstItemsFromOpenSalesOrder();
		$this->response->setJsonOutput($items);
	}

	public function openmstpurchorders()
	{
		$input = $this->request->input;
		$this->load->model('weberp/items');
		$items = $this->model_weberp_items->getMstItemsFromOpenPurchOrder($input['vendor']);
		$this->response->addHeader('Access-Control-Allow-Origin: *');
		$this->response->addHeader('Access-Control-Allow-Methods: GET, PUT, POST, DELETE, OPTIONS');
		$this->response->addHeader('Access-Control-Max-Age: 1000');
		$this->response->addHeader('Access-Control-Allow-Headers: Content-Type, Authorization, X-Requested-With');
		$this->response->addHeader('Content-Type: application/json');
		$this->response->setJsonOutput($items);
	}

	public function reservedorderitems()
	{
		$data = file_get_contents("php://input");
		$data = json_decode($data, true);

		// $data['stockid'] = 'TBC-SLR08';
		// $data['oid'] = 762467;

		$this->load->model('weberp/items');

		$items = $this->model_weberp_items->getItemsFromReservedOrder($data['stockid'], $data['oid']);


		$this->response->setJsonOutput($items);
	}

	public function getdetails()
	{
		$data = file_get_contents("php://input");
		$data = json_decode($data, true) ?: array();
		$input = array_merge($data, $this->request->get, $this->request->post);
		$results = array();
		$transno = $this->request->get['oid'];

		$sql = "SELECT
			*, debtortrans.id AS taxtransid
		FROM
			debtortrans
		INNER JOIN debtorsmaster ON debtorsmaster.debtorno = debtortrans.debtorno
		INNER JOIN salesorders ON debtortrans.order_ = salesorders.orderno
		INNER JOIN locations ON salesorders.fromstkloc = locations.loccode
		INNER JOIN custbranch ON debtortrans.debtorno = custbranch.debtorno AND debtortrans.branchcode = custbranch.branchcode
		LEFT JOIN shippers ON debtortrans.shipvia = shippers.shipper_id
		INNER JOIN salesman ON custbranch.salesman = salesman.salesmancode
		LEFT JOIN paymentterms ON debtortrans.payterms = paymentterms.termsindicator
		WHERE
			transno = '$transno'
			and type = 10
		";


		$lsql = "SELECT DISTINCT
			*,
			salesorderdetails.narrative as salesnar
		FROM
			debtortrans
		INNER JOIN salesorders ON debtortrans.order_ = salesorders.orderno
		INNER JOIN salesorderdetails ON debtortrans.order_ = salesorderdetails.orderno
		LEFT JOIN stockmoves ON debtortrans.transno = stockmoves.transno AND stockmoves.orderlineref = salesorderdetails.orderlineno and stockmoves.type=debtortrans.type
		INNER JOIN stockmaster ON salesorderdetails.stkcode = stockmaster.stockid
		LEFT OUTER JOIN shippers ON debtortrans.shipvia = shippers.shipper_id
		WHERE
			debtortrans.transno ='" . $transno . "'
			AND ( stockmoves.show_on_inv_crds ='1' OR stockmoves.show_on_inv_crds is NULL)";

		$results = array();
		$header = $this->db->query($sql);
		$header = $header->row;

		$lines = $this->db->query($lsql);
		$lines = $lines->rows;

		//TODO: need to clean up the postings before grabbing payments
		$psql = "select * from debtortrans
		join banktrans on debtortrans.transno = banktrans.transno and debtortrans.type = banktrans.type
		where id in (select transid_allocfrom from custallocns where transid_allocto ='{$header['id']}')
		";

		$payments = $this->db->query($psql);
		$payments = $payments->rows;

		$results = array(
			'header' => $header,
			'lines' => $lines,
			'payments' => $payments,
			'totals' => json_decode($header['payment_details']),
		);


		$this->response->setJsonOutput($results);
	}

	public function cancel()
	{
		$input = $this->request->input;

		$results = ['success' => false, 'input' => $input, 'force'=>$this->request->get['force']];
		$this->load->model('weberp/items');

		if (!$input || !isset($input['orderno'])) {
			$this->response->setJsonOutput(['success' => false, 'error' => 'Missing orderno']);
			return;
		}

		// Normalize payload
		$isPayload = is_array($input['orderno']);
		$data = $isPayload ? $input['orderno'] : [
			'orderno' => $input['orderno'],
			'reason' => '',
			'notes' => '',
			'timestamp' => date('c')
		];

		$orderno = $this->db->escape($data['orderno']);

		// Handle PO cancel confirmation requirement
		if ($this->config->get('haultcancelifpo') && !isset($this->request->get['force'])) {
			$po = $this->db->query("SELECT * FROM salesordertopurchaseorder WHERE sales_orderno = '{$orderno}'");
			if (!empty($po->row['purchase_orderno'])) {
				$results['requires_confirmation'] = true;
				$results['message'] = 'This order is linked to a purchase order and requires confirmation to cancel.';
				$results['purchase_orderno'] = $po->row['purchase_orderno'];
				$this->response->setJsonOutput($results);
				return;
			}
		}

		$pgsPreCancel = $this->tpl3plOrdersApiPgsCancelBeforeErpCancelIfNeeded($data['orderno']);
		if (!empty($pgsPreCancel['blocked'])) {
			$out = [
				'success' => false,
				'error' => 'pgs_cancel_failed',
				'message' => isset($pgsPreCancel['message']) ? $pgsPreCancel['message'] : '3PL cancellation failed',
			];
			if (isset($pgsPreCancel['pgs_cancel'])) {
				$out['pgs_cancel'] = $pgsPreCancel['pgs_cancel'];
			}
			$this->response->setJsonOutput($out);
			return;
		}

		if(PACKAGE == 'tires'){
			$orderhasauth = $this->db->query("select orderno from authorizations where orderno = '".$data['orderno']."' and status = 'Approved' limit 1")->row['orderno'];
		}

		// Handle PreAuth void
		if ($this->config->get('PreAuth') || (PACKAGE == 'tires' && $orderhasauth)) {
			$existingauths = $this->payment->getExistingAuths($data['orderno']);
			if (!empty($existingauths)) {
				foreach ($existingauths as $void) {
					$datavoid = [
						'transid' => $void['transid'],
						'uid' => $void['uid'],
						'amount' => $void['amount'],
						'orderno' => $data['orderno']
					];

					if(PACKAGE == 'tires'){
						$datavoid['tokenid'] = $void['tokenid'];
						$results['preauth'][] = $this->payment->voidPaymentAuth($datavoid);
					}else{
						$results['preauth'][] = $this->payment->voidPayment($datavoid);
					}
				}
			}
		}

		//clear cdn of shipping_label
		$cdn_row = $this->db->query("select debtorno from salesorders where orderno = '".$data['orderno']."'")->row;
		$debtorno = isset($cdn_row['debtorno']) ? (string)$cdn_row['debtorno'] : '';
		if ($debtorno !== '') {
			try {
				$results['cdn_removal'] = $this->cdn->deleteFile(
					(string)$this->config->get('db_database'),
					$debtorno,
					$data['orderno'] . '-' . $debtorno . '-label.pdf'
				);
			} catch (\Throwable $e) {
				error_log('CDN label cleanup failed on cancel ' . $data['orderno'] . ': ' . $e->getMessage());
			}
		}

		$rawOrdernoForTpl = trim((string)$data['orderno']);
		$tpl3plCfg = $this->tpl3plOrdersApiGetConfig();
		if (
			$rawOrdernoForTpl !== ''
			&& $tpl3plCfg
			&& !empty($tpl3plCfg['enabled'])
			&& isset($tpl3plCfg['integration_type'])
			&& $tpl3plCfg['integration_type'] === 'PGS'
		) {
			$escTpl = $this->db->escape($rawOrdernoForTpl);
			$tplCols = $this->tpl3plQueueColumns();
			$tplStateCol = isset($tplCols['state']) ? ', state' : '';
			$q3pl = $this->db->query(
				"SELECT export_status" . $tplStateCol . " FROM tpl3pl_order_queue WHERE orderno = '" . $escTpl . "' AND archived_at IS NULL ORDER BY id DESC LIMIT 1"
			);
			if ($q3pl->row) {
				$qState = (isset($tplCols['state']) && isset($q3pl->row['state'])) ? (string)$q3pl->row['state'] : '';
				$qExp = isset($q3pl->row['export_status']) ? (string)$q3pl->row['export_status'] : '';
				$qTerminal = in_array($qState, ['Cancelled', 'Shipped', 'Invoiced'], true);
				if (!$qTerminal && $qExp === 'exported') {
					$this->load->library('tpl3pl');
					$tpl3pl = new Tpl3pl($tpl3plCfg['config_json'], $this->registry);
					$tpl3plCancel = $tpl3pl->cancelOutboundOrder($rawOrdernoForTpl);
					$results['tpl3pl_cancel'] = $tpl3plCancel;
					if (empty($tpl3plCancel['success'])) {
						$pgsMsg = isset($tpl3plCancel['message']) ? trim((string)$tpl3plCancel['message']) : '';
						if ($qState === 'Dispatched') {
							$results['tpl3pl_dispatched'] = true;
							$results['tpl3pl_cancel_warning'] = 'ORDER DISPATCHED — This shipment has already left the PGS warehouse. Call PGS to intercept before delivery.' . ($pgsMsg !== '' ? ' ' . $pgsMsg : '');
						} else {
							$results['tpl3pl_cancel_warning'] = $pgsMsg !== ''
								? ('Could not cancel at 3PL — the order may be picking or shipped. ' . $pgsMsg)
								: 'Could not cancel at 3PL — the order may be picking or shipped.';
						}
					}
				}
			}
		}

		// Mark order as completed
		$this->db->query("UPDATE salesorderdetails SET completed = '1' WHERE orderno = '{$orderno}'");
		$this->db->query("UPDATE salesorders SET orderstatusid = '90', takenby = '{$this->session->data['userid']}' WHERE orderno = '{$orderno}'");

		// Archive any pending/failed 3PL queue rows — the order is cancelled, no export needed.
		$tbl3 = $this->db->query("SHOW TABLES LIKE 'tpl3pl_order_queue'");
		if ($tbl3->num_rows) {
			$this->db->query(
				"UPDATE tpl3pl_order_queue
				 SET export_status = 'failed',
				     error_message = 'Order cancelled in ERP',
				     archived_at   = NOW()
				 WHERE orderno     = '{$orderno}'
				   AND archived_at IS NULL
				   AND export_status IN ('pending', 'failed')"
			);
		}

		// METRC: clean up sync row + queued jobs for pre-transfer orders, or
		// flag transfer_confirmed rows for manual void in METRC's UI. Hook is
		// no-op when METRC isn't enabled or the order has no sync row.
		if ((int) $this->config->get('metrc_enabled') === 1) {
			require_once DIR_SYSTEM . 'library/metrc/order_hook.php';
			MetrcOrderHook::onOrderCancelled($this->registry, (int) $orderno);
		}

		// Clean up picking freeze
		$items = $this->db->query("SELECT * FROM pickingfreeze WHERE orderno = '{$orderno}'");
		$this->db->query("DELETE FROM pickingfreeze WHERE orderno = '{$orderno}'");

		foreach ($items->rows as $item) {
			$orders = $this->model_weberp_items->getOpenOrders($item['stockid']);
			foreach ($orders as $o) {
				if ($o['orderno'] != $data['orderno']) {
					$this->load->controller('weberp/item/pickorder/freezeorder', [$o['orderno']]);
				}
			}
		}

		// Cancel PO if exists and force is set
		$po = $this->db->query("SELECT * FROM salesordertopurchaseorder WHERE sales_orderno = '{$orderno}'");
		if($this->request->get['force'] === 'yes'){
			if (!empty($po->row['purchase_orderno'])) {
				$ponumber = $po->row['purchase_orderno'];
				$this->load->model('weberp/purchasing');
				
				
				//check if dropship
				$dropship = $this->model_weberp_purchasing->checkifdropship($ponumber);
				if($dropship){
					$order_data = [ 'header' => ['purchase_orderno' => $ponumber]];
					//delete grn and stockmoves created - false param leaves po data intact instead of removing it
					$this->model_weberp_purchasing->cleanupdropship($order_data,false);
				}
				
				
				$this->model_weberp_purchasing->updatePoStatus([
					'orderno' => $ponumber,
					'status_id' => '140'
				]);

			}
		}


		// Trigger downstream pushinput
		$this->shop->getUrlContentNoResponse(LOCAL_SOCKET . 'push/dispatch');

		$results['success'] = true;
		if (isset($pgsPreCancel['pgs_cancel'])) {
			$results['pgs_cancel'] = $pgsPreCancel['pgs_cancel'];
		} elseif (isset($pgsPreCancel['skipped'])) {
			$results['pgs_cancel_skip'] = $pgsPreCancel['skipped'];
		}

		if ((int) $this->config->get('metrc_enabled') === 1) {
			require_once DIR_SYSTEM . 'library/moov/order_fee_accrual.php';
			$cancelReason = '';
			if ($isPayload && !empty($data['reason']) && is_array($input['orderno']['reason'])) {
				$cancelReason = (string) ($input['orderno']['reason']['code'] ?? '');
			}
			$feeAccrual = MoovOrderFeeAccrual::annotateCancelledFees(
				$this->registry,
				(int) $data['orderno'],
				$cancelReason,
				'cancelled'
			);
			if ($feeAccrual) {
				$results['fee_accrual'] = $feeAccrual;
			}
		}

		// If full payload, store cancellation reason
		if ($isPayload && !empty($data['reason']) && is_array($input['orderno']['reason'])) {
			$reasonCode = $input['orderno']['reason']['code'] ?? '';
			$cancelInsert = [
				'orderno'         => $this->db->escape($data['orderno']),
				'userid'          => $this->db->escape($this->session->data['userid']),
				'reason'          => $this->db->escape($reasonCode),
				'notes'           => $this->db->escape($data['notes']),
				'force_cancel_po' => (isset($this->request->get['force']) && $this->request->get['force'] === 'yes') ? 1 : 0,
				'po_number'       => $this->db->escape($po->row['purchase_orderno'] ?? ''),
				'created_at'      => 'NOW()'
			];

			$columns = implode(',', array_keys($cancelInsert));
			$values = implode(',', array_map(function ($v) {
				return (strtoupper($v) === 'NOW()' || is_numeric($v)) ? $v : "'$v'";
			}, array_values($cancelInsert)));

			$this->db->query("INSERT INTO order_cancellations ($columns) VALUES ($values)");
		}

		$this->response->setJsonOutput($results);
	}

	/**
	 * Reverse ERP cancel (orders/cancel): status 90 → Pending (1), reopen lines, refreeze picking.
	 * Only for sales orders in status 90 with no posted invoice (debtortrans type 10).
	 */
	public function uncancel()
	{
		$input = $this->request->input;
		$results = ['success' => false];

		if (!$input || !isset($input['orderno']) || $input['orderno'] === '' || is_array($input['orderno'])) {
			$this->response->setJsonOutput(['success' => false, 'error' => 'Missing orderno']);
			return;
		}

		$rawOrderno = trim((string)$input['orderno']);
		if ($rawOrderno === '') {
			$this->response->setJsonOutput(['success' => false, 'error' => 'Missing orderno']);
			return;
		}

		$orderno = $this->db->escape($rawOrderno);

		$hdr = $this->db->query("SELECT orderno, orderstatusid FROM salesorders WHERE orderno = '{$orderno}' LIMIT 1");
		if (empty($hdr->row['orderno'])) {
			$this->response->setJsonOutput(['success' => false, 'error' => 'Order not found']);
			return;
		}

		if ((int)$hdr->row['orderstatusid'] !== 90) {
			$this->response->setJsonOutput([
				'success' => false,
				'message' => 'Only orders in Cancelled status can be restored.',
			]);
			return;
		}

		$inv = $this->db->query("SELECT transno FROM debtortrans WHERE type = 10 AND order_ = '{$orderno}' LIMIT 1");
		if (!empty($inv->row)) {
			$this->response->setJsonOutput([
				'success' => false,
				'message' => 'This order has invoice history and cannot be restored here.',
			]);
			return;
		}

		$this->db->query(
			"UPDATE salesorderdetails SET completed = IF(quantity > qtyinvoiced, 0, 1) WHERE orderno = '{$orderno}'"
		);

		$uid = $this->db->escape($this->session->data['userid']);
		$this->db->query("UPDATE salesorders SET orderstatusid = '1', takenby = '{$uid}' WHERE orderno = '{$orderno}'");

		$this->load->controller('weberp/item/pickorder/freezeorder', array($rawOrderno));
		$this->shop->getUrlContentNoResponse(LOCAL_SOCKET . 'push/dispatch');

		if ((int) $this->config->get('metrc_enabled') === 1) {
			require_once DIR_SYSTEM . 'library/moov/order_fee_accrual.php';
			$feeAccrual = MoovOrderFeeAccrual::restoreAccrualOnUncancel($this->registry, (int) $rawOrderno);
			if ($feeAccrual) {
				$results['fee_accrual'] = $feeAccrual;
			}
		}

		$results['success'] = true;
		$this->response->setJsonOutput($results);
	}

	public function getquote()
	{
		$orderno = $this->request->input['orderno'];

		//$sql = "SELECT * FROM quotes WHERE orderno = '$orderno'";
		$sqlUpdate = "UPDATE quotes SET printedpackingslip = printedpackingslip + 1 WHERE orderno = '$orderno'";
		$this->db->query($sqlUpdate);

		$input = $this->request->input;
		$result = array();
		$result['success'] = false;

		if (isset($input['orderno']) && $input['orderno'] != '') {
			$data = file_get_contents("php://input");
			$data = json_decode($data, true);
			$input['userid'] = $this->session->data['userid'];
			$orderno = $input['orderno'];


			$order_details = $this->db->query("select * from quote_details where orderno='$orderno' and completed = 0 ");
			$header = $this->db->query("select * from quotes left join shippers on shippers.shipper_id = quotes.shipvia where orderno='$orderno'");
			$order_header = $header->row;

			// build shipto html block
			$shipto = "<div>
						<span>" . $order_header['deladd1'] . "</span>
						<span>" . $order_header['deladd2'] . "</span>
						<span>" . $order_header['deladd3'] . ",</span>
						<span>" . $order_header['deladd4'] . "</span>
						<span>" . $order_header['deladd5'] . ",</span>
						<span>" . $order_header['deladd6'] . "</span>
					</div>";

			$order_header['shipto'] = $shipto;

			$order_dispatch_details = $this->db->query("SELECT
				*,
				d.lat as clat,
				d.lng as clng
			FROM
				dispatch d
				inner join trucks t on d.truckid = t.truck_id
			WHERE
				invoice = '$orderno'");

			$order_header['dispatch_details'] = $order_dispatch_details->row;
			$order_header['dispatch_details']['nextlast'] = false;

			if (
				$order_header['dispatch_details']
				&& !$order_header['cleared']
				&& !$order_header['oclater']
			) {
				$did = $order_header['dispatch_details']['dispatch_id'];
				$tid = $order_header['dispatch_details']['truckid'];

				$totalprevtime = $this->db->query("select sum(esttime) as esttime, count(dispatch_id) as nextlast from dispatch where truckid = '$tid' and cleared = 0 and sort <= (select sort from dispatch where dispatch_id = '$did' )  and status_id <> 31 order by sort asc");

				$order_header['dispatch_details']['nextlast'] = $totalprevtime->row['nextlast'];

				$totaltime = $totalprevtime->row['esttime'];
				$minutes = floor($totaltime);
				$zero = new DateTime('@0');
				$offset = new DateTime('@' . $minutes);
				$diff = $zero->diff($offset);
				$order_header['dispatch_details']['nextlast'] = $diff->format('%h Hours, %i Minutes');
			}


			if (!empty($order_header['dispatch_details'])) {
				$order_header['dispatch_details']['lastlat'] = (float) $result['dispatch_details']['lastlat'];
				$order_header['dispatch_details']['lastlng'] = (float) $result['dispatch_details']['lastlng'];
				$order_header['dispatch_details']['clat'] = (float) $result['dispatch_details']['clat'];
				$order_header['dispatch_details']['clng'] = (float) $result['dispatch_details']['clng'];
			}
			$ddetails = array();

			foreach ($order_details->rows as $k => $line) {
				$ddetails[$k] = $line;
				$discountprice = $line['unitprice'] - number_format($line['unitprice'] * $line['discountpercent'], 2, '.', '');
				$discountprice = number_format($discountprice, 2, '.', '');

				$ddetails[$k]['discountprice'] = (float) $discountprice;
				$ddetails[$k]['unitprice'] = (float) $line['unitprice'];

				$ddetails[$k]['ordered'] = (float) $line['ordered'];
				$ddetails[$k]['quantity'] = (float) $line['quantity'];

				$loaded_checked = $this->getLineLoaded($line['orderlineno'], $line['orderno']);

				if ($line['qtyinvoiced']) {
					$loaded_checked = $line['dispatch'];
				}

				$ddetails[$k]['qtyremain'] = (float) ($line['ordered'] - $line['qtyinvoiced']);
				$this->load->model('oms/orders');
				$ddetails[$k]['dispatch'] = (float) $this->model_oms_orders->resolveInvoiceDispatchQty($line, $loaded_checked);

				$ddetails[$k]['qoh'] = (float) $line['qoh'];
				$ddetails[$k]['discountpercent'] = (float) $line['discountpercent'];
				$ddetails[$k]['dspdiscount'] = (float) ($line['discountpercent'] * 100) . '%';
				$ddetails[$k]['qtyinvoiced'] = (float) $line['qtyinvoiced'];
				$ddetails[$k]['price'] = (float) $line['price'];
				$ddetails[$k]['linetax'] = 0.00;
				$ddetails[$k]['linetotal'] = 0.00;
				$ddetails[$k]['taxtotal'] = 0.00;
				$ddetails[$k]['linesubtotal'] = 0.00;
			}
		}

		$result = array(
			'header' => $order_header,
			'details' => $ddetails,
			'cart' => $cart,
		);

		$this->response->setJsonOutput($result);
	}

	public function getLineLoaded($lineno, $orderno)
	{
		$q = $this->db->query("select sum(loaded) as total from pickingfreeze where linenumber ='$lineno' and orderno='$orderno'");

		return $q->row['total'];
	}

	public function savequote()
	{
		$now = new DateTime();
		$now_short = $now->format("Y-m-d");
		$now = $now->format("Y-m-d h:i:s");
		$data = file_get_contents("php://input");

		$data = json_decode($data, true);

		$results = array();
		$raw_input = $data;



		if ($data) {
			$taxrecords = array();
			$totaltax = 0;
			$ovamount = 0;
			$invoicetotal = 0;
			$ovgst = 0;

			foreach ($data['totals'] as $total) {
				switch ($total['code']) {
					case 'tax':
						$taxrecords[] = array(
							'auth' => $total['sort_order'],
							'amount' => $total['text'],
						);
						$totaltax += $total['text'];
						break;

					case 'sub_total':
						$ovamount = $total['text'];
						break;

					case 'shipping':
						$ovgst = $total['text'];
						break;

					case 'total':
						$invoicetotal = $total['text'];
						$allocationallowed = $total['text'];
						break;
				}
			}


			//TODO RE-WORK TOTALS
			$timestamp = strtotime($data['order']['datewanted']);
			$wanted = date('Y-m-d', $timestamp);
			$items = $this->getCart($data['customer']['debtorno'], $data['customer']['branchcode']);

			foreach ($items as $item) {
				if ($item['quoteno'] != '' && isset($item['quoteno'])) {
					$removal = $item['quoteno'];
				}
			}


			//quotes to close
			$order = array(
				'customer_info' => $data['customer'],
				'placedfrom' => 1,
				'shipping_address' => $data['customer'],
				'salesperson' => $data['customer']['salesman'],
				'payment_details' => json_encode($data['totals']),
				'shipvia' => $data['order']['defaultshipvia'],
				'freightcost' => number_format($ovgst, 2, '.', ''),
				'location' => $data['location']['loccode'],
				'tagname' => "Order",
				'totals' => $data['totals'],
				//'invoice' => true,
				'quote_date' => $now,
				'deliver_date' => $wanted,
				'reference' => $data['order']['reference'],
				'terms' => $data['customer']['paymentterms'],
				'takenby' => $data['customer']['takenby'],
				'enteredby' => $data['order']['enteredby'],
				'comments' => $data['order']['comments'],
				'notes' => $data['order']['notes'],
				//not relying on sent items - call in actual cart?
				//$this->getCart($data['customer']['debtorno'], $data['customer']['branchcode'])
				'items' => $items,
				//$data['items'],
			);

			//pulling in cash receipts
			$order_details = $order;

			$results = $this->shop->createQuote($order);

			if (isset($removal)) {
				$this->shop->removeQuote($removal);
			}

			$results['testing'] = $data;
			//Update printedpackslip in quote
			$orderno = $results['orderno'];

			$sqlUpdate = "UPDATE quotes SET printedpackingslip = printedpackingslip + 1 WHERE orderno = '$orderno'";
			$this->db->query($sqlUpdate);
		}

		$this->response->setJsonOutput($results);
	}


	public function productlinesbymfg()
	{ //change to get request
		$this->load->model('weberp/orders');
		$results = array();

		$manufacturers = $this->db->query("SELECT * FROM manufacturer order by name asc")->rows;

		foreach ($manufacturers as $mfg) {
			$manufacturer_id = $mfg['manufacturer_id'];
			$manufacturer_name = $mfg['name'];
			$manufacturer_pls = $this->model_weberp_orders->productlinesByMfgId($manufacturer_id);



			if (count($manufacturer_pls) > 0) {
				$results[$manufacturer_name] = array();

				foreach ($manufacturer_pls as $pl) {
					$pl_id = $pl['line_field'];
					$product_line = $this->model_weberp_orders->productlineByLineId($pl_id);
					$pl_description = $product_line['line_description'];

					if ($pl_id != '' and $pl_description) {
						$product_line_out = "($pl_id) $pl_description";
						array_push($results[$manufacturer_name], $product_line_out);
					}
				}
			}
		}

		$this->response->setJsonOutput($results);
	}

	public function productlinereport()
	{
		ini_set('memory_limit', -1);
		ini_set('max_execution_time', -1);
		$input = $this->request->input;
		$locations = $this->db->query("select * from locations")->rows;
		$product_lines = $input['product_lines'];
		$startdate = date('Y-m-d', strtotime($input['range']['start']));
		$enddate = date('Y-m-d', strtotime($input['range']['end']));
		$season = $input['prod_options']['season'];
		$designation = $input['prod_options']['designation'];
		$custType = $input['prod_options']['custtype'];
		$limit = $input['option'];
		$user = $this->request->get['userid'];

		$this->load->model('weberp/orders');
		$this->load->model('weberp/salesman');
		$this->load->model('reports/report');

		$salesman_check = $this->model_weberp_salesman->getSalesmanCodeByUser($user);

		if (isset($salesman_check) && $salesman_check != '') {
			$user = $salesman_check;
			$canviewlocs = $this->model_reports_report->userCanViewLocs($this->request->get['userid']);
			$locs = array();

			foreach ($canviewlocs as $row) {
				array_push($locs, $row['loccode']);
			}
			$loc_limit = $locs;
		} else {
			$user = false;
			$loc_limit = false;
		}


		$results = array();

		foreach ($product_lines as $pl) {
			//blow out input into lineid & mfg
			$prod_mfg = $pl['mfg'];
			$prod_line_name = explode(') ', $pl['pl'])[1];
			$prod_line_id = explode("(", $pl['pl'])[1];
			$prod_line_id = explode(")", $prod_line_id)[0];

			//if mfg not present -> put it in
			if (!$results[$prod_mfg]) {
				$results[$prod_mfg]['pls'] = array();
				$results[$prod_mfg]['name'] = $prod_mfg;
				$resuts[$prod_mfg]['totalunits'] = 0;
				$results[$prod_mfg]['sales'] = 0;
			}

			//if pl not under mfg put it in
			if (!$results[$prod_mfg]['pls'][$prod_line_id]) {
				$results[$prod_mfg]['pls'][$prod_line_id]['prods'] = array();
				$results[$prod_mfg]['pls'][$prod_line_id]['name'] = $prod_line_name;
				$results[$prod_mfg]['pls'][$prod_line_id]['id'] = $prod_line_id;
				$results[$prod_mfg]['pls'][$prod_line_id]['totalunits'] = 0;
			}

			//PL prods
			$prod_line_name = $this->db->escape($prod_line_name);

			if (PACKAGE == 'tires') {
				$pl_items = $this->model_weberp_orders->getProductLineItems($prod_line_name, $season, $designation);
			} else {
				$pl_items = $this->model_weberp_orders->getProductLineItems($prod_line_name);
			}


			$lookup = [];
			$stockids = [];
			foreach ($pl_items as $item) {
				$lookup[$item['stockid']] = $item;
				array_push($stockids, $this->db->escape($item['stockid']));
			}
			$stockids = "'" . implode("','", $stockids) . "'";




			$sales_by_cust = $this->model_weberp_orders->customerSalesByStockId($startdate, $enddate, $stockids, $custType, $user, $loc_limit);
			$sales_overall = $this->model_weberp_orders->salesOverallByStockId($startdate, $enddate, $stockids, $custType, $user, $loc_limit);


			$rows = [];
			//build prod array for pl
			foreach ($sales_overall as $prod_row) {
				$prod_stockid = $prod_row['stkcode'];
				$prod_desc = $lookup[$prod_stockid]['description'];
				$row = array(
					"total" => '',
					"qtysold" => '',
					"stockid" => $prod_stockid,
					"description" => $prod_desc,
					"custs" => [], //add customer type to custs payload
					"designation" => $lookup[$prod_stockid]['designation'],
					"season" => $lookup[$prod_stockid]['season'],
				);
				foreach ($locations as $loc) {
					$loc_formatted = "Loc " . $loc['loccode'];
					$row[$loc_formatted] = "";
				}
				$rows[$prod_stockid] = $row;
			}


			//update prod array with order information and mfg array with totals
			foreach ($sales_by_cust as &$sale_row) {
				$stockid_t = $sale_row['stkcode'];
				unset($sale_row['stkcode']);

				$rows[$stockid_t]['total'] += $sale_row['sales'];
				$rows[$stockid_t]['qtysold'] += $sale_row['qty'];

				$ords = "'" . implode("','", explode(',', $sale_row['orders'])) . "'";
				$ords = $this->model_weberp_orders->customerOrders($ords, $stockid_t);

				foreach ($ords as $o) {
					$rows[$stockid_t]["Loc " . $o['fromstkloc']] += $o['quantity'];
					foreach ($locations as $loc) {
						$loc_formatted = "Loc " . $loc['loccode'];
						if (!isset($sale_row[$loc_formatted])) {
							$sale_row[$loc_formatted] = "";
						}
					}
					$sale_row["Loc " . $o['fromstkloc']] += $o['quantity'];
					$results[$prod_mfg]['pls'][$prod_line_id]['totalunits'] += $o['quantity'];
					$results[$prod_mfg]['pls'][$prod_line_id]['sales'] += $o['total'];
				}

				$sale_row['orders'] = $ords;
				array_push($rows[$stockid_t]['custs'], $sale_row);
			}

			$results[$prod_mfg]['pls'][$prod_line_id]['prods'] = array_values($rows);

			if (is_null($results[$prod_mfg]['pls'][$prod_line_id]['sales'])) {
				$results[$prod_mfg]['pls'][$prod_line_id]['sales'] = 0;
			}

		}

		$this->response->setJsonOutput($results);
	}

	public function merge()
	{
		$results = array('success' => false);

		if (is_array($this->request->input) && !empty($this->request->input)) {
			foreach ($this->request->input as $group) {
				$debtorno = $group['debtorno'];
				$orders = $group['orders'];

				if (count($orders) < 2) {
					continue; // Skip if there's only one order, nothing to merge
				}

				$maxOrderNo = max($orders);

				$orderNos = array_filter($orders, function ($value) use ($maxOrderNo) {
					return $value != $maxOrderNo;
				});

				$orderNosList = "'" . implode("','", $orderNos) . "'";

				// Step 1: Get the maximum orderlineno in the maxOrderNo
				$maxLineNoQuery = "SELECT MAX(orderlineno) as maxline FROM salesorderdetails WHERE orderno = '$maxOrderNo'";
				$result = $this->db->query($maxLineNoQuery);

				if ($result->num_rows > 0) {
					$maxLineNo = $result->row['maxline'];
				} else {
					$results['error'] = 'Error fetching maximum order line number for order ' . $maxOrderNo;
					$this->response->setJsonOutput($results);

					return;
				}

				// Step 2: Get all items from the orders to be merged
				$itemsQuery = "SELECT orderno, orderlineno FROM salesorderdetails WHERE orderno IN ($orderNosList) AND completed = 0";
				$itemsResult = $this->db->query($itemsQuery);

				if ($itemsResult && $itemsResult->num_rows > 0) {
					$updates = array();

					foreach ($itemsResult->rows as $item) {
						$maxLineNo++;
						$updates[] = array(
							'oldOrderNo' => $item['orderno'],
							'oldOrderLineNo' => $item['orderlineno'],
							'newOrderNo' => $maxOrderNo,
							'newOrderLineNo' => $maxLineNo,
						);
					}

					// Step 3: Update items in the database
					foreach ($updates as $update) {
						$updateQuery = "UPDATE salesorderdetails SET orderno = '{$update['newOrderNo']}', orderlineno = '{$update['newOrderLineNo']}'
										WHERE orderno = '{$update['oldOrderNo']}' AND orderlineno = '{$update['oldOrderLineNo']}' and completed = 0 ";

						if (!$this->db->query($updateQuery)) {
							$results['error'] = 'Error updating order ' . $update['oldOrderNo'] . '-' . $update['oldOrderLineNo'] . ': ' . $this->db->error;
							$this->response->setJsonOutput($results);

							return;
						}
					}

					$results['success'] = true;
				} else {
					$results['error'] = 'Error fetching items to merge or no items to merge for debtor ' . $debtorno;
				}
			}
		} else {
			$results['error'] = 'Invalid input';
		}

		$this->response->setJsonOutput($results);
	}



	public function getlocs(){
		$this->load->model('weberp/orders');
		$result = $this->model_weberp_orders->getlocations();
		$this->response->setJsonOutput($result);
	}

	public function refreshcartitems()
	{
		$customer = $this->request->get['customer'];
		$branch = $this->request->get['branch'];
		$result = $this->getCart($customer, $branch);
		$this->response->setJsonOutput($result);
	}

	public function exampleduedate(){
		$input = $this->request->input;
		$tdate = new DateTime($input['trandate']);
		$tdate = $tdate->format("Y-m-d");

		$agefrom = isset($input['agefrom']) && $input['agefrom'] !== null && $input['agefrom'] !== '' ? intval($input['agefrom']) : 1;
		$ageto = isset($input['ageto']) && $input['ageto'] !== null && $input['ageto'] !== '' ? intval($input['ageto']) : 25;

		$duedate = $this->sales->getDueDate($tdate, $input['dayinfollowingmonth'], $input['daysbeforedue'], 'Y-m-d', $input['statementperiods'], $agefrom, $ageto);

		$result = array(
			'trandate'=> $tdate,
			'duedate' => $duedate
		);
		$this->response->setJsonOutput($result);
	}


	public function fixduedates(){

		$invoices = $this->db->query("select * from debtortrans inner join salesorders on debtortrans.order_ = salesorders.orderno where settled = 0 and type = '10'");
		foreach($invoices->rows as $inv){
			$payterms = $inv['payterms'];
			$termdata = $this->db->query("select * from paymentterms where termsindicator = '".$payterms."'");
			$term = $termdata->row;
			$tdate = new DateTime($inv['trandate']);
			$tdate = $tdate->format('Y-m-d');
		
		 $duedate = $this->sales->getDueDate($tdate, $term['dayinfollowingmonth'], $term['daysbeforedue'], 'Y-m-d', $term['statementperiods'], '','');
		
		$due = new Datetime($duedate);
		$due = $due->format('Y-m-d');
		$this->db->query("update debtortrans set duedate = '".$due."' where id = '".$inv['id']."'");
		 print_r($inv['transno'].' '.$due);
		}
	}
	public function saveshipviacontainer(){
		$this->load->model('weberp/orders');
		$input = $this->request->input;
		$result = ['success'=>false, 'shipvia' => $input['shipvia']];


		if(isset($input['shipvia']) && $input['containerid'] !== ''){
			$shipvia = $input['shipvia'];
			$container_id = $input['containerid'];
			$container_data = [
				'shipper_id' => $shipvia['shipper_id'],
				'container_id' => $container_id
			];
			$container = $this->model_weberp_orders->saveShipviaContainer($container_data);
			if(isset($container)){
				$shipvia['container'] = $container;
				$shipvia['container_id'] = $container['container_id'];
				$result['data'] = $shipvia;
				$result['success'] = true;
			}
			$other_shipvia_containers = $this->model_weberp_orders->getShipperContainers($container_data['shipper_id']);
			if(isset($other_shipvia_containers)){
				$result['containers'] = $other_shipvia_containers;
			}
		}

		$this->response->setJsonOutput($result);
	}


	public function getstickershipvias(){
		$this->load->model('weberp/orders');
		$getcontainers = $this->request->get['containers'];
		$results = [];
		$shipvias = $this->model_weberp_orders->stickerShipVias();
		if(count($shipvias)){
			if($getcontainers){
				foreach($shipvias as &$shipper){
					$containers = $this->model_weberp_orders->getShipperContainers($shipper['shipper_id']) ?? [];
					$shipper['containers'] = $containers;
					if(count($shipper['containers'])){
						$activecontainers = 0;
						foreach($shipper['containers'] as $container){
							if($container['ordercount'] > 0){
								$activecontainers++;
							}
						}
						$shipper['activecontainers'] = $activecontainers;
					}
					$shipper['liveload'] = $this->model_weberp_orders->getShipperLiveLoads($shipper['shipper_id']) ?? [];
				}
			}
			$results['data'] = $shipvias;
		}
		$this->response->setJsonOutput($results);
	}

	public function getshipviacontainers(){
		$shipper_id = $this->request->get['id'];

		$result = [];
		if(isset($shipper_id) && $shipper_id !== ''){
			$this->load->model('weberp/orders');
			$containers = $this->model_weberp_orders->getShipperContainers($shipper_id);
			$result['containers'] = count($containers) ? $containers : [];
		}

		$this->response->setJsonOutput($result);

	}


	public function getpackedstickerorders(){
		$this->load->model('weberp/items');
		$this->load->model('weberp/orders');

		//need to make this dynamic
		//isdispatch = 2?
		//25 = RTS (first)
		//24 = loaded (second)
		// $statuses = $this->model_weberp_orders->getStickerOrderStatuses();


		$res = $this->model_weberp_orders->getOpenStickerOrders([24,25]);

		foreach($res as &$order){
			$order['so'] = $this->model_weberp_items->getOpenOrderByorder($order['orderno']);
			$order['so'][0]['tracking'] = $this->model_weberp_orders->getTrackingNumbers($order['orderno']);
			$order['so'][0]['requiresBinding'] = false;
			foreach($order['so'][0]['tracking'] as $track){
				if($track['bind'] == 1){
					$order['so'][0]['requiresBinding'] = true;
				}
			}

			$order['shippedtime'] = $order['shippedtime'] ? date_format(new DateTime($order['shippedtime']), 'm/d/Y H:i') : '';
			$order['stagedtime'] =  $order['stagedtime'] ? date_format(new DateTime($order['stagedtime']), 'm/d/Y H:i') : '';
			$order['packedtime'] =  $order['packedtime'] ? date_format(new DateTime($order['packedtime']), 'm/d/Y H:i') : '';
		}

		$this->response->setJsonOutput($res);
	}

	public function getpackedstickerorder(){
		$this->load->model('weberp/items');
		$this->load->model('weberp/orders');

		$orderno = $this->request->get['orderno'];
		$res = $this->model_weberp_orders->getSingleStickerOrder($orderno);

		foreach($res as &$order){
			$order['so'] = $this->model_weberp_items->getOpenOrderByorder($order['orderno']);
			$order['shippedtime'] = $order['shippedtime'] ? date_format(new DateTime($order['shippedtime']), 'm/d/Y H:i') : '';
			$order['stagedtime'] =  $order['stagedtime'] ? date_format(new DateTime($order['stagedtime']), 'm/d/Y H:i') : '';
			$order['packedtime'] =  $order['packedtime'] ? date_format(new DateTime($order['packedtime']), 'm/d/Y H:i') : '';
		}

		$this->response->setJsonOutput($res);
	}

	public function stickerorderpacked(){
		$input = $this->request->input;
		$user = $input['user'];
		$order = $input['order'];
		$this->load->model('weberp/orders');
		$this->load->model('oms/orders');
		$this->load->model('weberp/picking');

		$result = [
			'input' => $input,
			'document' => false,
			'status' => false
		];

		if(isset($user) && isset($order)){
			$packtime = Date('Y-m-d H:i:s');
			$statusid = 25;
			$orddata = [
				'orderno' => $order['orderno'],
				'name' => $order['deliverto'],
				'stockid' => $order['details'][0]['stkcode'],
				'quantity' => $order['details'][0]['quantity'],
				'statusid' => $statusid,
				'packedtime' => $packtime,
				'shippedtime' => '',
				'trackingnumber' => $order['tracking'][0]['trackingnumber'],
				'packeduserid' => $input['user'],
				'shipper_id' => $order['shipvia']
			];

			$keys = implode(', ', array_keys($orddata));
			$vals = "'" . implode("', '", array_values($orddata)) . "'";
			$exists = $this->model_weberp_orders->stickerOrderExists($order['orderno']);

			$repack = false;
			if(isset($exists['orderno'])){
				$this->model_weberp_orders->updateStickerOrderStatus($statusid, $order['orderno'], $order['shipvia']);
				$repack = true;
			}else{
				$this->db->query("insert into stickerscanout ($keys) VALUES ($vals) ");
			}


			if(!$exists || $repack){
				$document_data = [
					'orderno' => $order['orderno'],
					'buyername' => $order['buyername'] . ' ' . $order['deliverto'],
					'stockid' => $order['details'][0]['stkcode'],
					'itemdesc' => $order['details'][0]['itemdesc'],'statusid' => 'PACKED',
					'quantity' => $order['details'][0]['quantity'],
					'packedtime' => $packtime,
					'trackingnumber' => $order['tracking'][0]['trackingnumber'],
					'packeduserid' => $input['user'],
				];

				$this->request->get['document_data'] = $document_data;
				$doc = $this->load->controller('apir/documents/stickerlabel');

				if(isset($doc)){
					$result['document'] = $doc;
					$result['status'] = true;
					$this->db->query("update stickerscanout set confirmed=1, printed=1 where orderno = '".$order['orderno']."'");
					$result['packedtime'] = date_format(new DateTime($packtime), "m/d/Y H:i");
					$result['packeduserid'] = $input['user'];
				}
			}


			$this->model_oms_orders->setOrderStatus(25, $order['orderno']);
		}

		$this->response->setJsonOutput($result);
	}

	public function unpackstickerorder(){
		$input = $this->request->input;
		$order = $input['order'];
		$orderno = $order['orderno'];
		$result = ['success'=>false];

		if(isset($orderno) && $orderno !== ''){
			$this->load->model('oms/orders');

			//99 order in stickerscanout table to leave data trail, but not show it anymore
			$this->db->query("update stickerscanout set statusid = 99 where orderno = '".$orderno."'");

			//reset back to pending in so
			$this->model_oms_orders->setOrderStatus(21, $orderno);

			//refreeze order
			$this->load->controller('weberp/item/pickorder/freezeorder', array($orderno));
			$result['success'] = true;
			$result['order'] = $order;
		}

		$this->response->setJsonOutput($result);

	}

	public function stickerorderstaged(){
		$this->load->model('weberp/items');
		$this->load->model('weberp/orders');
		$this->load->model('oms/orders');

		$input = $this->request->input;
		$user = $input['user'];
		$order = $input['order'];
		$type = $input['type'];
		//batchid, scanid, orderno, name, stockid, quantity, statusid, packedtime, shippedtime, trackingnumber, userid,

		$result = [
			'status' => false
		];

		if(isset($user) && isset($order)){
			$stagedtime = Date('Y-m-d H:i:s');
			$statusid = 24;
			$updated = $this->model_weberp_orders->setStickerOrderStaged($statusid, $stagedtime, $user, $order['orderno'], $type);
			$updated['so'] = $this->model_weberp_items->getOpenOrderByorder($order['orderno']);

			if(isset($updated) && $updated['statusid'] == $statusid){
				$result['status'] = true;
				$this->model_oms_orders->setOrderStatus(24, $order['orderno']);
				$updated['shippedtime'] = $updated['shippedtime'] ? date_format(new DateTime($updated['shippedtime']), 'm/d/Y H:i') : '';
				$updated['stagedtime'] =  $updated['stagedtime'] ? date_format(new DateTime($updated['stagedtime']), 'm/d/Y H:i') : '';
				$updated['packedtime'] =  $updated['packedtime'] ? date_format(new DateTime($updated['packedtime']), 'm/d/Y H:i') : '';
				$result['order'] = $updated;

			}

		}
		$this->response->setJsonOutput($result);
	}



	public function ocstagedstickerorders(){
		$input = $this->request->input;
		$result = [ 'success' => false ];
		$user = $input['user'];
		$shipvia = $input['shipvia'];
		$mainprinter = $input['mainprinter'];

		$this->load->model('oms/orders');
		$this->load->model('weberp/orders');
		$this->load->model('weberp/routes');
		$this->load->model('weberp/cashreceipts');

		//format orders for invoiceandpay
		$formatted = array();
		$ordernos_formatted = array();
		foreach($input['orders'] as $ord){

			$customer = array(
				'debtorno' => $ord['debtorno'],
				'branchcode' => $ord['branchcode'],
				'payterms' => $ord['terms']
			);


			$has_been_confirmed = $this->model_weberp_orders->stickerOrderHasBeenConfirmed($ord['orderno']);
			if(in_array($ord['orderno'], $ordernos_formatted) || $has_been_confirmed){
				continue;
			}else {
				$ordernos_formatted[] = $ord['orderno'];
			}
			$orderdetails = $this->model_oms_orders->getOrderDetails($ord['orderno']);
			$orderdetails['header']['orderno'] = $ord['orderno'];
			$orderdetails['header']['debtorno'] = $ord['debtorno'];
			$orderdetails['header']['branchcode'] = $ord['branchcode'];

			$lines = $this->model_oms_orders->getOrder($ord['orderno']);
			$orderdetails['header']['fromstkloc'] = $lines['fromstkloc'];
			$orderdetails['header']['deliverto'] = $lines['deliverto'];
			$totalqty = $this->model_oms_orders->getSalesOrderTotalQty($ord['orderno']);

			$mustpay = false;
			$mustpayterms = $this->model_weberp_orders->getPaymentRequiredTerms();
			if(in_array($ord['payterms'],$mustpayterms)) {
				if(!$orderdetails['header']['payment_status']) {
					//if must pay terms && invoice not paid && card attached
					$pays = $this->model_weberp_cashreceipts->getOrderSelectedPaymentMethodDispatch($ord['orderno']);
					if(isset($pays) && !empty($pays)){
						$mustpay = true;
						$pays['cc_charge'] = ['card_id' => $pays['card_id']];
						$pays['payment'] = $orderdetails['total'];
						$pays['payment_method'] = $pays['paymentmethodid'];
						$orderdetails['payments'] = [$pays];
					}else{
						// card not found - on account
						$orderdetails['payments'] = [];
					}
				}
			}else{
				//on account
				$orderdetails['payments'] = [];
			}



			$formatted[] = array(
				'invoice' => $ord['orderno'],
				'orderno' => $ord['orderno'],
				'lines' => $lines,
				'qty' => count($orderdetails),
				'qtyordered' => $totalqty,
				'ordertype' => 10,
				'dtype' => 10,
				'ispoint' => false,
				'customerno' => $ord['debtorno'],
				'branchcode' => $ord['branchcode'],
				'details' => $orderdetails,
				'mustpay' => $mustpay,
				'paid' => ($orderdetails['header']['payment_status']) ? true : false,
				'default_paymentmehod' => 0,
				'trackingnumber' => $ord['trackingnumber'],
				'header'=>$orderdetails['header'],
				'packeduser' => $ord['packeduserid'],
				'packedtime' => $ord['packedtime'],
				'stageduser' => $ord['stageduserid'],
				'stagedtime' => $ord['stagedtime'],
				'preauth_capture' => $ord['authstatus']
			);
		}


		$result['formatted'] = $formatted;
		//invoice for on account
		if(count($formatted)){
			$this->request->input['origin'] = 'sticker';
			$this->request->input['dispatches'] = $formatted;


	

			$result['invoices'] = $this->load->controller('apir/oms/invoiceandpay/index');

			// print run invoices - going to manifest only; - leaving for now;
			$printinvoices = false;
			if($printinvoices){
				foreach($result['invoices'] as &$ord){
					$ord['invoice'] = $ord['transno'];
				}
				$this->request->input['user'] = ['userid'=>$input['user'], 'usePrintConnect'=>true];
				$this->request->input['column'] = true;
				$this->request->input['servercall'] = true;
				$this->request->input['suppliedorders'] = $formatted;
				$prints = $this->load->controller('apir/documents/delivery/printruninvoices');
				$result['prints'] = $prints;
			}else{
				$result['prints'] = [];
			}


			//if order is ccn need to get the invoice/reciept allocated - need to make a receipt on capture
			$del_array = [];
			$total_units = 0;
			foreach($formatted as $ord){
				$now = Date('Y-m-d H:i:s');
				$header = $ord['header'];
				$del_array[] = array(
					'shippedtime' => $now,
					'orderno' => $header['orderno'],
					'trackingnumber' => $ord['trackingnumber'],
					'buyername' => $header['buyername'],
					'packedtime' => $ord['packedtime'],
					'packeduser' => $ord['packeduser'],
					'stagedtime' => $ord['stagedtime'],
					'stageduser' => $ord['stageduser'],
					'transno' => '',
					'preauth_capture' => $ord['preauth_capture'] ?? ''
				);
				$total_units += (float) $ord['qtyordered'];
			}

			//get new batch #
			$batchid = $this->model_weberp_orders->getLastStickerBatch();
			$batchid++;

			//insert to batch table
			$result['auth_capture'] = [];
			$orders = [];
			foreach($formatted as $ord){
				$this->db->query("insert into stickerscanoutbatch values ('".$batchid."', '".$ord['orderno']."', null, '".$shipvia['shipper_id']."', '".$shipvia['container']."', '')");
				//update salesorder and stickerscanout to shipped
				array_push($orders, $ord['orderno']);
			}

			$this->request->input['pickupdata'] = ['batchid' => $batchid, 'ocuser' => $input['user'], 'signature'=> $input['signature'], 'mainprinter'=> $mainprinter];
			$result['deliverysheet'] = $this->load->controller('apir/documents/stickerlabel/deliverysheet');
			$timeshipped = $result['deliverysheet']['timeshipped'];

			//save signature to file using batch #
			$dir = DIR_SIGNATURES;
			$filename = 'stickerbatch-' . $batchid . '.txt';
			$fullpath = $dir.'/'.$filename;
			$fp = fopen($fullpath, 'w+');
			fwrite($fp, $input['signature']);
			fclose($fp);

			//update salesorders and stickerscanout & stickerscanoutbatch

			$data = [
				'timeshipped'=> $timeshipped,
				'fullpath' => $fullpath,
				'user' => $user,
				'batchid' => $batchid
			];
			$this->model_weberp_orders->setStickerOrdersShipped($orders, $data);

			//go through and email invoices, capture & allocate payments
			foreach($result['invoices']['data'] as $invoice){

				
				
				$email = $this->getstickeremail($invoice['debtorno']);
				if(isset($email['invoice'])){
					$emails = implode(',',$email['invoice']);
					$emdata = array('transno' => $invoice['id'], 'display' => 'email', 'email' => $emails);
					$emres = $this->load->controller('apir/documents/invoice/display',$emdata);
				}

				if(isset($email['sticker'])){
					$emdata =  array('singleorder' => $invoice['orderno'], 'display' => 'email', 'email' => $email['sticker'], 'debtorno' => $invoice['debtorno'], 'batchid'=>$batchid);
					$this->request->input['pickupdata'] = $emdata;
					$emres = $this->load->controller('apir/documents/stickerlabel/deliverysheet',$emdata);
				}

				//capture
				$existingauths = $this->payment->getExistingAuths($invoice['orderno']);
				if(count($existingauths)){
					foreach($existingauths as $auth){
						$capturedata = array(
							'transid' => $auth['transid'],
							"amount" => $auth['amount'],
							"tokenid" => $auth['tokenid'],
							'orderno' => $invoice['orderno'],
							'debtorno' => $invoice['debtorno'],
							'branchcode' => $invoice['branchcode'],
							'invoice' => $invoice,
							'auth' => $auth
						);

						$capture = $this->payment->capture($capturedata);
						if($capture->status == 'Approved'){
							$invoice['authstatus'] = 'Approved';
						}else{
							$invoice['authstatus'] = $capture->status;
						}


						$result['auth_capture'][] = $capture;

						if($capture->status == 'Approved'){
							//update sso batch preauth
							$this->db->query("update stickerscanoutbatch set preauthstatus = '".$capture->result."' where orderno = '". $invoice['orderno'] . "' limit 1");

							//payment vars
							$today = Date('y-m-d');
							$postdate = $today;
							$periodno = $this->model_weberp_cashreceipts->getPeriod($today);
							$accts = $this->model_weberp_cashreceipts->getCompanyDiscAccount();
							$bankaccount = $this->config->get('DefaultBankAccount');
							$payment_transno = $this->pos->getNextTransNo('12');
							$paymenttype = 'MasterCard'; //need to make this dynamic
							$journal_selected= '';
							$reference = 'STICKER: ' . $capture->status == 'Approved' ? $capture->historyid : $capture->result;

							$banktrans = array(
								'type' => '12',
								'transno' => $payment_transno,
								'bankact' => $bankaccount,
								'amountcleared' => 0,
								'exrate' => 1,
								'functionalexrate' => 1,
								'transdate' => $today,
								'banktranstype' => $paymenttype,
								'amount' => number_format($auth['amount'], 2, '.', ''),
								'currcode' => $this->config->get('config_currency'),
								'journal_no' => '',
								'ref' => $reference,
								'placedfrom' => 1,
								'placedby' => $this->session->data['userid'],
								'paylocation' => $data['user']['user']['defaultlocation']['loccode'],
							);

							$bankcheck = $this->db->query("select skipbanktrans, payment_code from paymentmethods where paymentname='$paymenttype'");

							if ($bankcheck->row['skipbanktrans']) {
								//Get Sales GL accts
								$salesGL =	$this->shop->getSalesGLAccount($data['user']['user']['defaultlocation']['loccode'], $this->sales->getSingleConfig('GiftCardItem'), 'AN');
								$banktrans['bankact'] = $salesGL['salesglcode'];
								$bankaccount = $salesGL['salesglcode'];
							}

							$this->model_weberp_cashreceipts->addBankTrans($banktrans);

							$total_payment = number_format($auth['amount'], 2, '.', '');

							$debtortrans = array(
								'transno' => $payment_transno,
								'type' => 12,
								'debtorno' => $invoice['debtorno'],
								'branchcode' => $invoice['branchcode'],
								'trandate' => $postdate." 00:00:00",
								'inputdate' => date('Y-m-d H:i:s'),
								'prd' => $periodno,
								'settled' => 0,
								'reference' => $paymenttype,
								'tpe' => '',
								'order_' => $invoice['orderno'],
								'rate' => 1,
								'ovamount' => number_format((-1* $auth['amount']), 2, '.', ''),
								'ovgst' => 0,
								'ovfreight' => 0,
								'ovdiscount' =>  number_format((-1 * $discount), 2, '.', ''),
								'diffonexch' => 0,
								'alloc' => 0,
								'invtext' => $journal_selected.' - '.$narrative,
								'checknum' => $reference,
								'payterms' => 'CCN', //need to make this dynamic
								'payby_comments' => $capture->orderid ?? '',
								'change_given' => '0.00',
							);
							$changegiven = 0;

							$from_allocation = $this->model_weberp_cashreceipts->addDebtorTrans($debtortrans);

							$glamount = array(
								'type' => 12,
								'typeno' => $payment_transno,
								'chequeno' => $reference,
								'trandate' => $postdate,
								'periodno' => $periodno,
								'account' => $bankaccount,
								'narrative' => $notes,
								'amount' => number_format($auth['amount'], 2, '.', ''),
								'posted' => 0,
								'tag' => $this->request->post['tag'],
							);

							//if($total) {
							$this->model_weberp_cashreceipts->addGlTrans($glamount);
							//}

							$gldiscount = $glamount;
							$gldiscount['account'] = $accts['pytdiscountact'];
							$gldiscount['amount'] = $discount;
							$bal = $auth['amount'];

							if ($gldiscount['amount']) {
								$this->model_weberp_cashreceipts->addGlTrans($gldiscount);
							}

							$glbalance = $glamount;
							$glbalance['account'] = $accts['debtorsact'];
							$glbalance['amount'] = number_format(-1 * $auth['amount'], 2, '.', '');
							//if($glbalance['amount']) {
							$this->model_weberp_cashreceipts->addGlTrans($glbalance);
							//}

							//todo need math here to make sure not overallocation

							$allocation_amount = number_format(abs($auth['amount']), 2, '.', '');
							$allocationallowed = $allocation_amount;
							$invoicetotal = number_format($auth['amount'], 2, '.', '');

							//allows payment to be open.
							if (number_format($allocationallowed, 2, '.', '') < '0.00') {
								//math check
								//remaining balance of invoice
								$allocation_amount = number_format($allocation_amount + $allocationallowed, 2, '.', '');
							}

							$id = $this->db->query("select id from debtortrans where type = 10 and order_ = '".$invoice['orderno']."'")->row['id'];

							$allocations[] = array(
								'datealloc' => date('Y-m-d'),
								'amt' => $allocation_amount,
								'transid_allocfrom' => $from_allocation,
								'transid_allocto' => $id,
							);



							foreach ($allocations as $alloc) {
								$this->model_weberp_cashreceipts->addAllocation($alloc);

								$totalfromalloc = $this->model_weberp_cashreceipts->getTotalAllocationsFrom($alloc['transid_allocfrom']);
								//update payment
								//NEED THE ORIGINAL AMOUNT TO SETTLE
								$updaterecord = array(
									'id' => $alloc['transid_allocfrom'],
									'amount' => number_format($totalfromalloc * -1, 2, '.', ''),
									'settled' => (abs($alloc['amt']) == $totalfromalloc) ? 1 : 0,
								);

								$this->model_weberp_cashreceipts->updateTransaction($updaterecord);
							}

							//total invoice alloc
							$totaltoalloc =$this->model_weberp_cashreceipts->getTotalAllocationsTo($id);

							//update order
							$updaterecord = array(
								'id' => $id,
								'amount' => number_format($totaltoalloc, 2, '.', ''),
								'settled' => ($invoicetotal == $totaltoalloc) ? 1 : 0,
							);

							$this->model_weberp_cashreceipts->updateTransaction($updaterecord);
							//first version handles left over over payments
							if ($allocationallowed) {
								$change = number_format(abs($allocationallowed), 2, '.', '');
								$this->db->query("update debtortrans set change_given = '$change' where id ='{$id}' ");
							}
							//second version handles total delta only
							if ($changegiven) {
								$change = number_format(abs($changegiven), 2, '.', '');
								$this->db->query("update debtortrans set change_given = '$change' where id ='{$id}' ");
							}

						}else{
							//update sticker scan out to have preauth capture status failed
							$this->db->query("update stickerscanoutbatch set preauthstatus = '".$capture->result."' where orderno = '". $invoice['orderno'] . "' limit 1");
						}

					}
				}


				//check for shipping label in CDN and remove if found - sticker feed api orders
				$debtorno = $this->db->escape($invoice['debtorno']);
				$orderno = $this->db->escape($invoice['orderno']);
				if (!empty($debtorno) && $debtorno !== '' && !empty($orderno) && $orderno !== '') {
					$files = $this->cdn->fetchCustomerFiles($this->config->get('db_database'), $debtorno);
					if (!count($files)) {
						
					} else {
						foreach ($files as $f) {
							if (strpos($f['filename'], $debtorno) && strpos($f['filename'], $orderno) && strpos($f['filename'], 'label')) {
								$this->cdn->deleteFile((string)$this->config->get('db_database'), $debtorno, $f['link']);
							}
						}
					}
				}

			}
		}


		$this->response->setJsonOutput($result);
	}



	public function getstickeremail($debtorno){
		$sticker = [];
		$invoice = [];
		$contacts = $this->db->query("SELECT * FROM custcontacts WHERE debtorno = '".$debtorno."'")->rows;

		foreach($contacts as $cont){
			$serialized = json_decode($cont['serialized_roles'], true);
			if(in_array('STICKER', $serialized)){
				$sticker[] = $cont['email'];
			}
			if(in_array('INVOICE', $serialized)){
				$invoice[] = $cont['email'];
			}
		}

		return array(
			'sticker' => $sticker,
			'invoice' => $invoice
		);

	}


	public function getlaststickerorders(){
		$this->load->model('weberp/orders');
		$result = $this->model_weberp_orders->getLastStickerOrders();
		$this->response->setJsonOutput($result);
	}


	public function getstickerbatchhistory(){
		$data = $this->request->input;
		$this->load->model('weberp/orders');
		$results = $this->model_weberp_orders->getStickerBatchHistory($data);
		$this->response->setJsonOutput($results);
	}

	public function getstickerpaymentreqs(){
		$this->load->model('weberp/orders');
		$results = $this->model_weberp_orders->getStickerBatchHistory("needspayment");
		$this->response->setJsonOutput($results);
	}

	public function getordercounts(){
		$debtorno = isset($this->request->get['debtorno']) ? $this->request->get['debtorno'] : '';
		$date_from = isset($this->request->get['date_from']) ? $this->request->get['date_from'] : '';
		$date_to = isset($this->request->get['date_to']) ? $this->request->get['date_to'] : '';
		$branchcode = isset($this->request->get['branchcode']) ? $this->request->get['branchcode'] : '';
		$connection_type = isset($this->request->get['connection_type']) ? $this->request->get['connection_type'] : 'api';
		$this->load->model('oms/orders');
		$results = $debtorno !== '' ? $this->model_oms_orders->getOrderCounts($debtorno, $date_from, $date_to, $branchcode, $connection_type) : array('ytd' => 0, 'mtd' => 0, 'week' => 0);
		$this->response->setJsonOutput($results);
	}

	public function gethistoricalorders(){
		$debtorno = isset($this->request->get['debtorno']) ? $this->request->get['debtorno'] : '';
		$branchcode = isset($this->request->get['branchcode']) ? $this->request->get['branchcode'] : '';
		$connection_type = isset($this->request->get['connection_type']) ? $this->request->get['connection_type'] : 'api';
		$date_from = isset($this->request->get['date_from']) ? $this->request->get['date_from'] : '';
		$date_to = isset($this->request->get['date_to']) ? $this->request->get['date_to'] : '';
		$page = isset($this->request->get['page']) ? (int) $this->request->get['page'] : 1;
		$page_size = isset($this->request->get['page_size']) ? (int) $this->request->get['page_size'] : 25;
		$search = isset($this->request->get['search']) ? $this->request->get['search'] : '';
		$this->load->model('oms/orders');
		$results = $debtorno !== '' ? $this->model_oms_orders->getStickerHistoricalOrders($debtorno, $branchcode, $connection_type, $date_from, $date_to, $page, $page_size, $search) : array('rows' => array(), 'total' => 0);
		$this->response->setJsonOutput($results);
	}

	public function getopenpurchaseorders(){
		$debtorno = isset($this->request->get['debtorno']) ? $this->request->get['debtorno'] : '';
		$this->load->model('oms/orders');
		$results = $debtorno !== '' ? $this->model_oms_orders->getOpenPurchaseOrders($debtorno) : array();
		$this->response->setJsonOutput($results);
	}

	public function getorderlines(){
		$orderno = isset($this->request->get['orderno']) ? $this->request->get['orderno'] : '';
		$this->load->model('oms/orders');
		$results = $orderno !== '' ? $this->model_oms_orders->getOrderLines($orderno) : array();
		$this->response->setJsonOutput($results);
	}

	public function getordertotals(){
		$orderno = isset($this->request->get['orderno']) ? $this->request->get['orderno'] : '';
		$this->load->model('oms/orders');
		$results = $orderno !== '' ? $this->model_oms_orders->getOrderTotals($orderno) : array('total' => 0, 'subtotal' => 0, 'taxtotal' => 0, 'freighttotal' => 0);
		$this->response->setJsonOutput($results);
	}

	public function reprintstickerbatch(){
		$input = $this->request->get;
		$this->load->model('weberp/orders');
		$this->load->model('oms/orders');

		$results = array();
		$batchid = $input['id'];

		if(boolval($input['invoices'])){

			$ordernos = $this->db->query("SELECT * from stickerscanoutbatch join stickerscanout on stickerscanout.orderno = stickerscanoutbatch.orderno where batchid = '$batchid' ")->rows;
			$formatted = array();

			foreach($ordernos as $ord){
				$orderdetails = $this->model_oms_orders->getOpenOrder($ord['orderno']);
				$lines = $this->model_oms_orders->getOrder($ord['orderno']);

				$formatted[] = array(
					'invoice' => $ord['orderno'],
					'orderno' => $ord['orderno'],
					'lines' => $lines,
					'qty' => count($orderdetails),
					'ordertype' => 10,
					'dtype' => 10,
					'ispoint' => false,
					'customerno' => $ord['debtorno'],
					'branchcode' => $ord['branchcode'],
					'details' => $orderdetails,
					'paid' => ($orderdetails['header']['payment_status']) ? true : false,
					'default_paymentmehod' => 0,
					'trackingnumber' => $ord['trackingnumber'],
					'header'=>$orderdetails['header'],
					'packeduser' => $ord['packeduserid'],
					'packedtime' => $ord['packedtime'],
					'stageduser' => $ord['stageduserid'],
					'stagedtime' => $ord['stagedtime'],
				);
			}




			$this->request->input['user'] = ['userid'=>$input['user'], 'usePrintConnect'=>true];
			$this->request->input['column'] = true;
			$this->request->input['servercall'] = true;
			$this->request->input['suppliedorders'] = $formatted;
			$this->request->input['reprint'] = true;
			$prints = $this->load->controller('apir/documents/delivery/printruninvoices');
			$results['prints'] = $prints;
		}else{
			$this->request->input['pickupdata'] = array(
				"signature" =>$this->model_weberp_orders->getBatchSignature($batchid),
				"ocuser" => $this->model_weberp_orders->getBatchOcUser($batchid),
				"batchid" => $batchid,
				"reprint" => true
			);
			$prints = $this->load->controller('apir/documents/stickerlabel/deliverysheet');
			$results['prints'] = $prints;
		}



		$this->response->setJsonOutput($results);
	}


	public function paystickercc(){
		$input = $this->request->input;
		$order = $input['order'];

		$this->load->model('weberp/cashreceipts');
		$preselected_method = $this->model_weberp_cashreceipts->getOrderSelectedPaymentMethod($order['orderno']);
		$input['preselected_payment'] = $preselected_method;
		$response = array();
		$ccfail = false;
		if (isset($preselected_method)) {
			$payment_values =  array(
				[
					'newcard' => false,
					'order_number' => $order['orderno'],
					'debtorno' => $order['debtorno'],
					'branchcode' => $order['branchcode'],
					'card_id' => $preselected_method['card_id'],
					'amount' =>  $order['ovamount'],
					'comments' => ''
				]
			);

			$payment = $this->payment->makeProfilePayment($payment_values);

			if (!$payment['success']) {
				//failed cc
				$ccfail = true;
				$message = $payment['response']->message.' '.$payment['response']->ActionCode;
				$payment['message'] = $message;
				$response['message'] = $message;
			}


			$allocation_from = array();
			$ccfail = false;

			$payment = array(
				'id' => '1231231231',
				'reference' => 'rewerwe'
			);

			//test
			$input['userid'] = 'PHIL';


			if (!$ccfail) { //added ccfail check to stop hitting of gltrans on failed cc transactions 12/1/23
				foreach ($payment_values as &$pay) {
					$pay['reference'] = $payment['id'];
					$pay['payby_comments'] = $payment['id'];
					$pay['userid'] = $input['userid'];
					$pay['paylocation'] = $order['fromstkloc'];
					$pay['placedfrom'] = $order['placedfrom'];
					$pay['actualref'] = $payment['reference'];


					//faking for now
					$pay['payment_type'] = 'Visa';
					$pay['orderno'] = $order['orderno'];

					/*
						    [newcard] =>
							[order_number] => 853514
							[debtorno] => 4124
							[branchcode] => 00
							[card_id] => 1327003271
							[amount] => 161.22
							[comments] =>
							[reference] => 1231231231
							[payby_comments] => 1231231231
							[userid] => PHIL
							[paylocation] => 01
							[placedfrom] => 1
							[actualref] => rewerwe
					*/
					$allocation_from[] = $this->shop->createPayment($pay);
				}


				$payments_to_allocate = $allocation_from;

				$invoice_total = $order['ovamount'];

				foreach ($payments_to_allocate as $alloc) {
					if ($alloc['amount'] >= $invoice_total) {
						$alloc['amount'] = $invoice_total;
					} else {
						//under paid invoice
						if ($alloc['amount'] < $invoice_total) {
							$alloc['amount'] = $alloc['amount'];
						} else {
							//remainder - should be zero
							$alloc['amount'] = $invoice_total;
						}
					}

					$allocation = array(
						'datealloc' => date('Y-m-d'),
						'amt' => $alloc['amount'],
						'transid_allocfrom' => $alloc['id'],
						'transid_allocto' => $order['id'],
					);

					$this->model_weberp_cashreceipts->addAllocation($allocation);

					$total_alloc = $this->model_weberp_cashreceipts->getTotalAllocationsFrom($alloc['id']);

					$updaterecord = array(
						'id' => $alloc['id'],
						'amount' => number_format(-1 * $total_alloc, 2, '.', ''),
					);

					$this->model_weberp_cashreceipts->updateTransaction($updaterecord);
					$invoice_total = $invoice_total - $alloc['amount'];

					$total_alloc = $this->model_weberp_cashreceipts->getTotalAllocationsTo($order['id']);

					$updaterecord = array(
						'id' => $order['id'],
						'amount' => number_format($total_alloc, 2, '.', ''),
					);

					$this->model_weberp_cashreceipts->updateTransaction($updaterecord);
				}
			}
		}


		// $res = [
		// 	'succes' => $cc,
		// 	'payment_response' => $payment
		// ];

		$this->response->setJsonOutput($res);
	}


	public function testfirstmile(){
		$input = $this->request->input;
		if(isset($input) && isset($input['acctid'])){
			$response = $this->payment->testing($input);
		}
		$this->response->setJsonOutput($response);
	}


	public function orderentrylog(){
		$input = $this->request->input;
		$userid = $this->request->get['userid'];

		

		$response = ['success'=>false];
		$this->load->model('weberp/orders');

		if(isset($input)){
			$filters = [];
			$filters['userid'] = $userid;

			if(isset($input['salesman']) && $input['salesman']){
				$filters['salesman'] = $input['salesman'];
			}
		
			if(isset($input['allowedlocations']) && count($input['allowedlocations'])){
				$filters['allowedlocations'] = array_map(function($location){
					return $location['loccode'];
				}, $input['allowedlocations']);
			}

			if(isset($input['pos']) && $input['pos']){
				$filters['pos'] = $input['pos'];
			}

			if(isset($input['locations']) && count($input['locations'])){
				$filters['locations'] = $input['locations'];
			}

			if(isset($input['sources']) && count($input['sources'])){
				$filters['sources'] = $input['sources'];
			}

			if(isset($input['salespeople']) && count($input['salespeople'])){
				$filters['salespeople'] = $input['salespeople'];
			}

			$from = $input['start_d'];
			$to = $input['end_d'];



			$data = $this->model_weberp_orders->getOrderEntryHistory($from, $to, $filters);

			$this->load->model('oms/orders');
			$this->load->model('weberp/customer');
			foreach($data as &$ord){
				if(!isset($ord['transno'])){
					$totals = $this->model_oms_orders->getOrderTotals($ord['orderno']);
					$totals = $totals['total'];
				}else{
					$totals = $this->model_weberp_customer->getInvoiceTotal($ord['orderno'], $ord['type']);
				}
				$units = $this->model_weberp_orders->getOrderUnitCount($ord['orderno']);
				
				$ord['units'] = $units;
				$ord['total'] = $totals;
			}

			$response['success'] = true;
			if(count($data)){
				$response['data'] = $data;
				$response['msg'] = false;
			}else{
				$response['msg'] = 'No data for entered filters';
				$response['sql'] = $data;
			}			
		}else{
			$response['msg'] = 'Input error';
		}

		
		$this->response->setJsonOutput($response);
	}


	/**
	 * PGS cancel (optional) then assign next sales order number for 3PL re-export.
	 * Implemented here so route apir/orders/bumpOrdernoFor3plResubmit works without apir/tpl3pl/orders.
	 */
	public function bumpOrdernoFor3plResubmit()
	{
		$this->session->data['userid'] = isset($this->session->data['userid']) ? $this->session->data['userid'] : 'RO';

		if (!empty($this->session->data['issalesman'])) {
			$this->response->setJsonOutput(['success' => false, 'message' => 'Not permitted']);
			return;
		}

		$input = isset($this->request->input) && is_array($this->request->input) ? $this->request->input : [];
		$orderno = isset($input['orderno']) ? trim((string)$input['orderno']) : '';
		if ($orderno === '') {
			$this->response->setJsonOutput(['success' => false, 'message' => 'Order number required']);
			return;
		}

		$config = $this->tpl3plOrdersApiGetConfig();
		if (!$config || !$config['enabled']) {
			$this->response->setJsonOutput(['success' => false, 'message' => '3PL integration not configured or disabled']);
			return;
		}
		$cj = isset($config['config_json']) && is_array($config['config_json']) ? $config['config_json'] : [];
		if (!Tpl3pl::isConfigFlagEnabled($cj, 'split_partial_backorder_enabled') && !Tpl3pl::isConfigFlagEnabled($cj, 'bump_so_resubmit_enabled')) {
			$this->response->setJsonOutput(['success' => false, 'message' => 'Enable "bump_so_resubmit_enabled" (or partial backorder split) in 3PL config to use SO renumber', 'error_code' => 'disabled']);
			return;
		}

		$this->load->library('tpl3pl');
		$tpl3pl = new Tpl3pl($config['config_json'], $this->registry);
		$result = $tpl3pl->bumpSalesOrderNumberFor3plResubmit($orderno, [
			'cancel_pgs_first' => empty($input['skip_pgs_cancel']),
			'ignore_pgs_cancel_failure' => !empty($input['ignore_pgs_cancel_failure']),
		]);
		$this->response->setJsonOutput($result);
	}

	/**
	 * Same shape as ControllerApirTpl3plOrders::getConfig (for bump / future order-level 3PL helpers).
	 *
	 * @return array|false
	 */
	private $tpl3plQueueColCache = null;

	private function tpl3plQueueColumns()
	{
		if ($this->tpl3plQueueColCache === null) {
			$q = $this->db->query(
				"SELECT COLUMN_NAME FROM INFORMATION_SCHEMA.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'tpl3pl_order_queue' AND COLUMN_NAME IN ('client_id','environment','state')"
			);
			$this->tpl3plQueueColCache = [];
			foreach ($q->rows as $r) { $this->tpl3plQueueColCache[$r['COLUMN_NAME']] = true; }
		}
		return $this->tpl3plQueueColCache;
	}

	private function tpl3plOrdersApiGetConfig()
	{
		$clientId = defined('CLIENT_ID') ? CLIENT_ID : 'default';
		$environment = isset($this->request->input['environment']) ? $this->request->input['environment'] : 'default';

		return $this->tpl3plOrdersApiGetConfigForClientEnv($clientId, $environment);
	}

	/**
	 * @param string $clientId
	 * @param string $environment
	 * @return array|false
	 */
	private function tpl3plOrdersApiGetConfigForClientEnv($clientId, $environment)
	{
		$clientId = trim((string)$clientId) !== '' ? $clientId : 'default';
		$environment = trim((string)$environment) !== '' ? $environment : 'default';
		$config = $this->db->query("SELECT * FROM tpl3pl_config WHERE client_id = '" . $this->db->escape($clientId) . "' AND environment = '" . $this->db->escape($environment) . "'");

		if ($config->row) {
			$result = $config->row;
			$configJson = json_decode($result['config_json'], true);
			if (!is_array($configJson)) {
				$configJson = [];
			}
			$this->load->library('tpl3pl');
			$helper = new Tpl3pl([], $this->registry);
			$configJson = $helper->decryptSensitiveConfig($configJson);
			$integrationType = trim((string)$result['integration_type']);
			if ($integrationType === '' || !in_array($integrationType, ['API','PGS','EDI','SFTP','FTP'], true)) {
				$integrationType = $this->tpl3plOrdersApiInferIntegrationType($configJson);
				$this->db->query("UPDATE tpl3pl_config SET integration_type = '" . $this->db->escape($integrationType) . "' WHERE id = " . (int)$result['id']);
			}
			$configJson['integrationType'] = $integrationType;
			$configJson['testMode'] = (bool)$result['test_mode'];
			$configJson['warehouseCode'] = $result['warehouse_code'];
			$configJson = \Tpl3pl::mergeRuleColumnsFromRow($configJson, $result);
			$result['config_json'] = $configJson;
			$result['integration_type'] = $integrationType;
			return $result;
		}

		return false;
	}

	/**
	 * @param array $queueRow Row from tpl3pl_order_queue
	 * @return array|false
	 */
	private function tpl3plOrdersApiGetConfigForQueueRow(array $queueRow)
	{
		$cid = (defined('CLIENT_ID') ? CLIENT_ID : 'default');
		$env = 'default';
		if (isset($queueRow['client_id']) && trim((string)$queueRow['client_id']) !== '') {
			$cid = trim((string)$queueRow['client_id']);
		}
		if (isset($queueRow['environment']) && trim((string)$queueRow['environment']) !== '') {
			$env = trim((string)$queueRow['environment']);
		}

		return $this->tpl3plOrdersApiGetConfigForClientEnv($cid, $env);
	}

	/**
	 * If the order was exported to PGS, send warehouse DELETE before ERP cancel (open orders / standard cancel).
	 * Queue ERP updates remain the responsibility of the caller.
	 *
	 * @param string $orderno
	 * @return array{blocked: bool, message?: string, skipped?: string, pgs_cancel?: array}
	 */
	private function tpl3plOrdersApiPgsCancelBeforeErpCancelIfNeeded($orderno)
	{
		$orderno = trim((string)$orderno);
		if ($orderno === '') {
			return ['blocked' => false];
		}
		$esc = $this->db->escape($orderno);
		$tbl = $this->db->query("SHOW TABLES LIKE 'tpl3pl_order_queue'");
		if (!$tbl->num_rows) {
			return ['blocked' => false, 'skipped' => 'no_queue_table'];
		}
		$cols = $this->tpl3plQueueColumns();
		$extraCols = (isset($cols['client_id']) && isset($cols['environment'])) ? ', client_id, environment' : '';
		$hasStateCol = isset($cols['state']);
		$stateCol = $hasStateCol ? ', state' : '';
		$q = $this->db->query(
			"SELECT export_status, archived_at" . $stateCol . $extraCols . " FROM tpl3pl_order_queue WHERE orderno = '{$esc}' ORDER BY id DESC LIMIT 1"
		);
		if (!$q->row) {
			return ['blocked' => false, 'skipped' => 'no_queue_row'];
		}
		$row = $q->row;
		if (trim((string)$row['export_status']) !== 'exported') {
			return ['blocked' => false, 'skipped' => 'not_exported'];
		}
		$st = $hasStateCol ? trim((string)$row['state']) : '';
		if (in_array($st, ['Cancelled', 'Shipped', 'Invoiced'], true)) {
			return ['blocked' => false, 'skipped' => 'terminal_queue_state'];
		}
		if (!empty($row['archived_at'])) {
			return ['blocked' => false, 'skipped' => 'queue_archived'];
		}

		$configRow = $this->tpl3plOrdersApiGetConfigForQueueRow($row);
		if (!$configRow || empty($configRow['enabled'])) {
			return ['blocked' => false, 'skipped' => 'tpl3pl_disabled'];
		}
		$it = isset($configRow['integration_type']) ? trim((string)$configRow['integration_type']) : '';
		if ($it !== 'PGS') {
			return ['blocked' => false, 'skipped' => 'not_pgs'];
		}
		$this->load->library('tpl3pl');
		$cj = isset($configRow['config_json']) && is_array($configRow['config_json']) ? $configRow['config_json'] : [];
		$tpl3pl = new Tpl3pl($cj, $this->registry);
		if (!$tpl3pl->getPgsAdapter()) {
			return ['blocked' => false, 'skipped' => 'no_pgs_adapter'];
		}
		$result = $tpl3pl->cancelOutboundOrder($orderno, ['sync_erp_cancel' => false]);
		if (!empty($result['success'])) {
			return ['blocked' => false, 'pgs_cancel' => $result];
		}
		$msg = isset($result['message']) ? (string)$result['message'] : '';
		$blob = strtolower($msg . ' ' . json_encode(isset($result['pgs_result']) ? $result['pgs_result'] : []));
		$benign = (
			strpos($blob, '404') !== false
			|| strpos($blob, 'not found') !== false
			|| strpos($blob, 'unknown') !== false
			|| strpos($blob, 'already') !== false
		);
		if ($benign) {
			return ['blocked' => false, 'pgs_cancel' => $result, 'skipped' => 'pgs_cancel_benign_failure'];
		}

		return [
			'blocked' => true,
			'message' => 'PGS cancellation failed: ' . ($msg !== '' ? $msg : 'warehouse may still have this order open') . ' — cancel at 3PL or retry.',
			'pgs_cancel' => $result,
		];
	}

	private function tpl3plOrdersApiInferIntegrationType($configJson)
	{
		if (!is_array($configJson)) {
			return 'API';
		}
		$pgs = isset($configJson['pgs']) ? $configJson['pgs'] : [];
		if (!empty($pgs['baseUrl']) && !empty($pgs['userId']) && isset($pgs['passwd']) && $pgs['passwd'] !== '') {
			return 'PGS';
		}
		$api = isset($configJson['api']) ? $configJson['api'] : [];
		if (!empty($api['baseUrl'])) {
			return 'API';
		}
		$edi = isset($configJson['edi']) ? $configJson['edi'] : [];
		if (!empty($edi['tradingPartnerId'])) {
			return 'EDI';
		}
		$sftp = isset($configJson['sftp']) ? $configJson['sftp'] : [];
		if (!empty($sftp['host']) && !empty($sftp['username'])) {
			return 'SFTP';
		}
		return 'API';
	}

	private function posreversecart($cart) {
		
		$cart = array_reverse($cart);
		
		$groups = array();
		foreach ($cart as $item) {
			$gk = $item['parent_id'] ? $item['parent_id'] : $item['cart_id'];
			$groups[$gk][] = $item;
		}

		$groups = array_reverse($groups);
	
		foreach ($groups as $gk => &$items) {
			usort($items, function($a, $b) {
				return (int)$a['cart_id'] - (int)$b['cart_id'];
			});
		}
		unset($items);
		
		// reverse sory by key)
		krsort($groups);
		$result = array();
		foreach ($groups as $items) {
			foreach ($items as $item) {
				$result[] = $item;
			}
		}
		return $result;
	}

}
