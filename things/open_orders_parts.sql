-- The v8 open-orders board as a Thing: one per open order, kept by the rows.
-- The board's query also LEFT JOINed debtortrans and stockmoves; those carry
-- the order under other names (order_, transno) and move with every invoice,
-- so they are parts here (trans, moves) and the consumer adds them up the way
-- the join did (sumqty, t, and the per-row multiplication of sumquantity).
-- invlines carries only the columns the consumer's sums and totals read: with
-- sd.* it was 25 of the document's 59 MB.
-- The head is the board's own query (catalog/model/oms/orders.php
-- getOpenOrdersInvDispatch) without the per-user and per-request filters,
-- which the consumer applies; each PART is one of the queries that function
-- ran per order, with the order number as KEY. The consumer
-- (catalog/model/oms/openorders_thing.php) shapes an entry from the JSON and
-- runs no query of its own.
CREATE THING open_orders KEY so.orderno
  AS SELECT so.orderno,
       salesorderdetails.*, so.*, debtorsmaster.*, custbranch.*,
       so.orderno AS orderno, debtorsmaster.debtorno AS dno, debtorsmaster.name AS dname,
       SUM(quantity) AS sumquantity, SUM(unitprice * (quantity - qtyinvoiced)) AS ordervalue,
       order_status.name AS statusname, order_status.badge_bg AS status_badge_bg, order_status.badge_fg AS status_badge_fg,
       so.orderstatusid AS orderstatusid, so.branchcode AS branchcode, sm.description,
       DATEDIFF(deliverydate, NOW()) AS daystilldue, 30 AS ordertype,
       0 AS is_cod, 0 AS cod_called, '' AS cod_called_by, NULL AS cod_called_at,
       EXISTS (SELECT 1 FROM salesorderpaymentmethod sopm WHERE sopm.orderno = so.orderno
               AND (NULLIF(TRIM(IFNULL(sopm.card_id, '')), '') IS NOT NULL
                    OR NULLIF(TRIM(IFNULL(sopm.card_number, '')), '') IS NOT NULL
                    OR NULLIF(TRIM(IFNULL(sopm.payment_method, '')), '') IS NOT NULL)) AS has_order_payment_method
     FROM salesorderdetails
     INNER JOIN salesorders so ON salesorderdetails.orderno = so.orderno
     INNER JOIN stockmaster sm ON salesorderdetails.stkcode = sm.stockid
     INNER JOIN debtorsmaster ON so.debtorno = debtorsmaster.debtorno
     INNER JOIN custbranch ON custbranch.debtorno = so.debtorno AND custbranch.branchcode = so.branchcode
     LEFT JOIN stockmaster ON salesorderdetails.stkcode = stockmaster.stockid
     LEFT JOIN order_status ON so.orderstatusid = order_status.order_status_id
     WHERE completed = '0'
       AND NOT FIND_IN_SET(so.orderstatusid, COALESCE((SELECT confvalue FROM config WHERE confname = 'OpenOrdersExcludedStatuses'), '29,30,40,50,90,93'))
     GROUP BY so.orderno, debtorsmaster.debtorno
  PART trans AS SELECT dt.transno, dt.type FROM debtortrans dt WHERE dt.order_ = KEY
  PART moves AS SELECT sm.transno, sm.orderlineref, sm.qty FROM debtortrans dt JOIN stockmoves sm ON sm.transno = dt.transno AND sm.type = 10 WHERE dt.order_ = KEY
  PART lines AS SELECT *, SUM(salesorderdetails.quantity) AS quantity, SUM(salesorderdetails.quantity) AS ordered
       FROM salesorderdetails JOIN stockmaster ON stockmaster.stockid = stkcode
       WHERE completed <> 1 AND salesorderdetails.orderno = KEY GROUP BY stkcode ORDER BY orderno, orderlineno ASC
  PART invlines AS SELECT sd.orderlineno, sd.stkcode, sd.quantity, sd.qtyinvoiced, sd.unitprice, sd.discountpercent,
       sd.taxable, sd.taxoverride, sd.completed, s.taxcatid
       FROM salesorderdetails sd JOIN stockmaster s ON sd.stkcode = s.stockid WHERE sd.orderno = KEY
  PART tracking AS SELECT ti.*, s.shippername AS shippername
       FROM trackinginfo ti LEFT JOIN shippers s ON s.shipper_id = ti.type_id WHERE ti.transno = KEY
  PART tpl AS SELECT tracking_number, carrier, ship_date, created_at FROM tpl3pl_shipments
       WHERE orderno = KEY AND tracking_number IS NOT NULL AND tracking_number <> ''
  PART picking AS SELECT * FROM pickingfreeze WHERE orderno = KEY
  PART auth AS SELECT * FROM authorizations WHERE orderno = KEY
  PART snooze AS SELECT * FROM oms_snoozed WHERE orderno = KEY
  PART stock AS SELECT sd.stkcode, (SELECT SUM(quantity) FROM locstock WHERE locstock.stockid = sd.stkcode) AS quantity
       FROM salesorderdetails sd WHERE sd.orderno = KEY GROUP BY sd.stkcode
  PART po_links AS SELECT * FROM salesordertopurchaseorder WHERE sales_orderno = KEY
  PART open_po_items AS SELECT po.orderno AS purchase_orderno
       FROM salesorderdetails sd JOIN purchorderdetails po ON po.itemcode = sd.stkcode
       WHERE sd.orderno = KEY AND sd.stkcode != 'FET' AND po.completed = 0 AND po.quantityord > po.quantityrecd
       GROUP BY po.orderno ORDER BY po.orderno ASC;
