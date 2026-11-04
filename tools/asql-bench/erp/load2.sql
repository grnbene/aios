LOAD mysql_scanner; ATTACH '' AS m (TYPE mysql, READ_ONLY);
CREATE OR REPLACE TABLE "manufacturer" AS SELECT * FROM mysql_query('m', 'SELECT * FROM `manufacturer`');
CREATE OR REPLACE TABLE "product_to_manufacturer" AS SELECT * FROM mysql_query('m', 'SELECT * FROM `product_to_manufacturer`');
CREATE OR REPLACE TABLE "purchdata" AS SELECT * FROM mysql_query('m', 'SELECT * FROM `purchdata`');
CREATE OR REPLACE TABLE "salesman" AS SELECT * FROM mysql_query('m', 'SELECT * FROM `salesman`');
CREATE OR REPLACE TABLE "territory" AS SELECT * FROM mysql_query('m', 'SELECT * FROM `territory`');
CREATE OR REPLACE TABLE "stockcategory" AS SELECT * FROM mysql_query('m', 'SELECT * FROM `stockcategory`');
.mode tabs
SELECT 'manufacturer', COUNT(*) FROM "manufacturer";
SELECT 'product_to_manufacturer', COUNT(*) FROM "product_to_manufacturer";
SELECT 'purchdata', COUNT(*) FROM "purchdata";
SELECT 'salesman', COUNT(*) FROM "salesman";
SELECT 'territory', COUNT(*) FROM "territory";
SELECT 'stockcategory', COUNT(*) FROM "stockcategory";
