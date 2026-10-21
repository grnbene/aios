-- Open sales orders, open credits, and invoiced orders still on dispatch: one
-- Thing per order, kept by the rows.
--
-- "Open" is the v8 ERP's own open-orders board (catalog/model/oms/orders.php):
--   getOpenOrdersInvDispatch - salesorderdetails.completed = '0', the header's
--     orderstatusid not in config OpenOrdersExcludedStatuses (default
--     29,30,40,50,90,93), and the line's item, customer and branch exist.
--   getInvoicedOnDispatch - the orders dispatch still holds (cleared = 0,
--     oclater = 0), with their customer and branch.
--   getOpenCredits - the same as open over creditorders / creditorderdetails,
--     with no status exclusion.
-- The board (catalog/model/oms/openorders_thing.php) lists orders from these
-- rows, sorts them by created / confirmed as the queries do, filters on
-- loccode / debtorno / branchcode / placedfrom / salesperson / takenby, and
-- re-renders an order only when its row's version moves; snooze_end is in the
-- row so a snooze moves it. Nothing here knows what a cancel, an invoice, a
-- back order or a credit is: each is rows changing, and each order's row is
-- re-read from the tables.

CREATE THING open_orders KEY so.orderno PARTITION BY so.orderno
  READS salesorders, salesorderdetails, stockmaster, debtorsmaster, custbranch, config, oms_snoozed
  AS SELECT so.orderno AS orderno, so.debtorno AS debtorno, so.branchcode AS branchcode,
       so.fromstkloc AS loccode, so.orderstatusid AS orderstatusid, so.quotation AS quotation,
       so.placedfrom AS placedfrom, so.salesperson AS salesperson, so.takenby AS takenby,
       so.orddate AS orddate, so.deliverydate AS deliverydate,
       so.date_created AS created, so.confirmeddate AS confirmed,
       COUNT(*) AS open_lines,
       SUM(sd.quantity) AS ordered, SUM(sd.qtyinvoiced) AS invoiced,
       SUM(sd.quantity - sd.qtyinvoiced) AS open_units,
       SUM(sd.unitprice * (sd.quantity - sd.qtyinvoiced)) AS open_value,
       MIN(sd.itemdue) AS first_due,
       (SELECT MAX(sz.date_end) FROM oms_snoozed sz WHERE sz.orderno = so.orderno) AS snooze_end
     FROM salesorders so
     JOIN salesorderdetails sd ON sd.orderno = so.orderno
     JOIN stockmaster sm ON sm.stockid = sd.stkcode
     JOIN debtorsmaster dm ON dm.debtorno = so.debtorno
     JOIN custbranch cb ON cb.debtorno = so.debtorno AND cb.branchcode = so.branchcode
     WHERE sd.completed = 0
       AND NOT FIND_IN_SET(so.orderstatusid, COALESCE(
             (SELECT confvalue FROM config WHERE confname = 'OpenOrdersExcludedStatuses'),
             '29,30,40,50,90,93'))
     GROUP BY so.orderno;

CREATE THING on_dispatch KEY so.orderno PARTITION BY so.orderno
  READS dispatch, salesorders, debtorsmaster, custbranch
  AS SELECT so.orderno AS orderno, so.debtorno AS debtorno, so.branchcode AS branchcode,
       so.fromstkloc AS loccode, so.salesperson AS salesperson, so.takenby AS takenby,
       so.placedfrom AS placedfrom, so.date_created AS created, so.confirmeddate AS confirmed,
       COUNT(*) AS dispatches
     FROM salesorders so
     JOIN dispatch d ON d.invoice = so.orderno
     JOIN debtorsmaster dm ON dm.debtorno = so.debtorno
     JOIN custbranch cb ON cb.debtorno = so.debtorno AND cb.branchcode = so.branchcode
     WHERE d.cleared = 0 AND d.oclater = 0
     GROUP BY so.orderno;

CREATE THING open_credits KEY co.orderno PARTITION BY co.orderno
  READS creditorders, creditorderdetails, stockmaster, debtorsmaster, custbranch
  AS SELECT co.orderno, co.debtorno, co.branchcode, co.fromstkloc AS loccode, co.orderstatusid,
       co.orddate,
       COUNT(*) AS open_lines,
       SUM(cd.quantity) AS ordered, SUM(cd.qtyinvoiced) AS invoiced,
       SUM(cd.quantity - cd.qtyinvoiced) AS open_units,
       SUM(cd.unitprice * (cd.quantity - cd.qtyinvoiced)) AS open_value
     FROM creditorders co
     JOIN creditorderdetails cd ON cd.orderno = co.orderno
     JOIN stockmaster sm ON sm.stockid = cd.stkcode
     JOIN debtorsmaster dm ON dm.debtorno = co.debtorno
     JOIN custbranch cb ON cb.debtorno = co.debtorno AND cb.branchcode = co.branchcode
     WHERE cd.completed = 0
     GROUP BY co.orderno;
