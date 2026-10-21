-- Reference data for drive-wire.php: load into MariaDB (mysql < setup-wdrv.sql),
-- dump it (mysqldump wdrv), load the dump into ASQL through the wire, then
-- run drive-wire.php against both and diff the two outputs.
CREATE DATABASE IF NOT EXISTS wdrv;
USE wdrv;
DROP TABLE IF EXISTS orders; DROP TABLE IF EXISTS olines; DROP TABLE IF EXISTS notes;
CREATE TABLE orders (orderno INT PRIMARY KEY, debtorno VARCHAR(10) NOT NULL, orddate DATE, total DECIMAL(12,2), status TINYINT, comment TEXT, blob_col VARBINARY(16), created DATETIME);
CREATE TABLE olines (orderlineno INT, orderno INT, stockid VARCHAR(20), qty DECIMAL(10,3), price DOUBLE, PRIMARY KEY (orderlineno, orderno));
CREATE TABLE notes (id INT AUTO_INCREMENT PRIMARY KEY, txt VARCHAR(80) CHARACTER SET utf8mb4);
INSERT INTO orders SELECT seq, CONCAT('D', seq % 37), DATE_ADD('2026-01-01', INTERVAL seq % 300 DAY), seq * 1.25, seq % 4, IF(seq % 5 = 0, NULL, CONCAT('note ', seq)), UNHEX(LPAD(HEX(seq), 8, '0')), '2026-09-30 12:00:00' FROM seq_1_to_5000;
INSERT INTO olines SELECT seq % 7 + 1, seq % 5000 + 1, CONCAT('SKU', seq % 250), seq % 13 + 0.5, seq * 0.01 FROM seq_1_to_20000;
INSERT INTO notes (txt) VALUES ('plain'), (_utf8mb4 'café résumé'), (_utf8mb4 '€ 5'), (_utf8mb4 '😀 emoji');
