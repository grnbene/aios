-- A Thing that does the work: an order on the pick floor, one row per order
-- being picked (Printed 20 / Picking 23, with open pickingfreeze lines). When
-- every line's picked count has reached its pick, the Thing marks the order
-- Picked (21) itself - the statement the picking screen runs after a picker's
-- last pick (catalog/model/oms/orders.php, markSalesOrderPickStatus) - on the
-- connection profile st_maria, which connections.ini marks things_write=1.
-- Once Picked the order leaves this Thing (its status is no longer 20/23); put
-- back into Picking it is a new Thing and acts again when fully picked.
-- Not mirrored: the METRC fee accrual markSalesOrderPickOrder adds when
-- metrc_enabled=1 (off on this machine).
CREATE THING picked_orders KEY so.orderno
  AS SELECT so.orderno, so.orderstatusid, so.debtorno,
       COUNT(*) AS lines,
       SUM(CASE WHEN pf.picked >= pf.pick THEN 1 ELSE 0 END) AS picked_lines,
       SUM(pf.pick) AS to_pick, SUM(pf.picked) AS picked
     FROM salesorders so
     INNER JOIN pickingfreeze pf ON pf.orderno = so.orderno
     WHERE so.orderstatusid IN (20, 23) AND pf.completed = 0
     GROUP BY so.orderno
  WHEN picked_lines = lines AND to_pick > 0
  DO UPDATE salesorders SET orderstatusid = 21 WHERE orderno = KEY AND orderstatusid < 29
  TO st_maria;
