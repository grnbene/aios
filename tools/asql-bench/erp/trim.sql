-- MariaDB's PAD SPACE collation matches 'GKCLGTC208015 ' to 'GKCLGTC208015'; DuckDB compares text exactly. Trimmed
-- here, in the DuckDB copy only, so the view's exact joins match the rows the ERP's view matches (9,999,676 rows of
-- stocktrans, as ASQL gives, instead of 9,998,831). Only these keys had trailing spaces in summitholding on 2026-11-04.
UPDATE stockmoves SET stockid = rtrim(stockid) WHERE stockid <> rtrim(stockid);
UPDATE stockmaster SET stockid = rtrim(stockid) WHERE stockid <> rtrim(stockid);
UPDATE product_to_manufacturer SET stockid = rtrim(stockid) WHERE stockid <> rtrim(stockid);
UPDATE debtortrans SET salesperson = rtrim(salesperson) WHERE salesperson <> rtrim(salesperson);
UPDATE salesman SET salesmancode = rtrim(salesmancode) WHERE salesmancode <> rtrim(salesmancode);
CHECKPOINT;
