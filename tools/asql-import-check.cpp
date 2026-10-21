// Verifies importing MySQL/MariaDB data into ASQL, from SQL text and from a file, then
// querying it back.

#undef NDEBUG
#include <iostream>
#include <cassert>
#include <unistd.h>
#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"
#include "aios/userland/task_ledger.hpp"
#include "aios/storage/aql.hpp"

using namespace aios;
using namespace aios::userland;

int main() {
  std::cout << "=== AIOS ASQL / MySQL / MariaDB Import Verification Suite ===\n";

  char root_dir[] = "/tmp/aios-asql-import-XXXXXX";
  if (!mkdtemp(root_dir)) {
    std::perror("mkdtemp");
    return 1;
  }

  KernelConfig cfg;
  cfg.root = root_dir;
  cfg.residency = test_config;
  auto k_res = Kernel::boot(cfg);
  if (!k_res.ok()) {
    std::cerr << "Failed to boot kernel: " << k_res.error().message << "\n";
    return 1;
  }
  Kernel& kernel = *k_res.value();

  TaskLedger ledger(kernel);
  ledger.init("tasks");
  SqlDatabase db(kernel, &ledger);
  db.init("sql_db");

  std::cout << "[Step 1] Testing MySQL / MariaDB dump import from raw SQL string...\n";

  const std::string mysql_dump_sql = R"(
-- MySQL dump 10.13  Distrib 8.0.32, for Linux (x86_64)
-- Host: 127.0.0.1    Database: erp_fleet
-- ------------------------------------------------------
# Local MariaDB server dump header comment

/*!40101 SET @OLD_CHARACTER_SET_CLIENT=@@CHARACTER_SET_CLIENT */;
/*!40101 SET NAMES utf8mb4 */;
/*!40014 SET @OLD_FOREIGN_KEY_CHECKS=@@FOREIGN_KEY_CHECKS, FOREIGN_KEY_CHECKS=0 */;

CREATE DATABASE IF NOT EXISTS `erp_fleet`;
USE `erp_fleet`;

DROP TABLE IF EXISTS `fleet_routes`;
CREATE TABLE `fleet_routes` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `route_code` varchar(64) NOT NULL,
  `driver_name` varchar(255) DEFAULT NULL,
  `truck_units` int(11) DEFAULT '1',
  `status` enum('dispatched','pending','completed') DEFAULT 'pending',
  `created_at` timestamp NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  KEY `idx_route_code` (`route_code`)
) ENGINE=InnoDB AUTO_INCREMENT=101 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

LOCK TABLES `fleet_routes` WRITE;
/*!40000 ALTER TABLE `fleet_routes` DISABLE KEYS */;
INSERT INTO `fleet_routes` (`id`, `route_code`, `driver_name`, `truck_units`, `status`) VALUES
  (101, 'RT-CLE-001', 'Bob Smith', 2, 'dispatched'),
  (102, 'RT-CLE-002', 'Alice Vance', 1, 'pending'),
  (103, 'RT-DET-009', 'Charlie Brown', 4, 'completed');
/*!40000 ALTER TABLE `fleet_routes` ENABLE KEYS */;
UNLOCK TABLES;
)";

  SqlQueryResult import_res = db.import_mysql_sql(mysql_dump_sql);
  if (!import_res.success) {
    std::cerr << "Import failed: " << import_res.error_message << "\n";
    return 1;
  }
  std::cout << "  Import result:\n" << import_res.formatted_table;
  assert(import_res.rows_affected == 3);

  std::cout << "[Step 2] Verifying imported tables and rows via SELECT query...\n";
  SqlQueryResult select_res = db.execute_query("SELECT * FROM fleet_routes WHERE status = 'dispatched';");
  if (!select_res.success) {
    std::cerr << "SELECT query failed: " << select_res.error_message << "\n";
    return 1;
  }
  std::cout << "  Query result:\n" << select_res.formatted_table;
  assert(select_res.rows.size() == 1);
  assert(select_res.rows[0].values[1] == "RT-CLE-001");
  assert(select_res.rows[0].values[2] == "Bob Smith");

  std::cout << "[Step 3] Testing import_sql_file from disk...\n";
  std::string dump_path = cfg.root + "/dump_test.sql";
  const std::string file_dump_sql = R"(
-- Dump file test for asql
DROP TABLE IF EXISTS `inventory`;
CREATE TABLE `inventory` (
  `sku` varchar(32) PRIMARY KEY,
  `qty` int NOT NULL,
  `location` varchar(64) DEFAULT 'Warehouse A'
);
INSERT INTO `inventory` (`sku`, `qty`, `location`) VALUES
  ('SKU-100', 50, 'Rack-1'),
  ('SKU-200', 120, 'Rack-4');
)";

  auto write_res = kernel.host().write_file(dump_path, ByteSpan(reinterpret_cast<const uint8_t*>(file_dump_sql.data()), file_dump_sql.size()));
  if (!write_res.ok()) {
    std::cerr << "Failed to write dump file: " << write_res.error().message << "\n";
    return 1;
  }

  SqlQueryResult file_import_res = db.import_sql_file(dump_path);
  if (!file_import_res.success) {
    std::cerr << "File import failed: " << file_import_res.error_message << "\n";
    return 1;
  }
  std::cout << "  File import result:\n" << file_import_res.formatted_table;
  assert(file_import_res.rows_affected == 2);

  SqlQueryResult inv_query = db.execute_query("SELECT sku, qty, location FROM inventory ORDER BY qty DESC;");
  if (!inv_query.success) {
    std::cerr << "Inventory SELECT failed: " << inv_query.error_message << "\n";
    return 1;
  }
  std::cout << "  Inventory query result:\n" << inv_query.formatted_table;
  assert(inv_query.rows.size() == 2);
  assert(inv_query.rows[0].values[0] == "SKU-200");
  assert(inv_query.rows[0].values[1] == "120");

  std::cout << "\n[Step 4] Testing ASQL CTE (Common Table Expressions)...\n";

  // Test 4.1: Basic CTE with calculation
  SqlQueryResult cte_basic = db.execute_query(
    "WITH calc AS (SELECT 42 AS answer, 10 AS multiplier) SELECT answer * multiplier AS result FROM calc;"
  );
  if (!cte_basic.success) {
    std::cerr << "CTE basic failed: " << cte_basic.error_message << "\n";
    return 1;
  }
  std::cout << "  CTE basic result:\n" << cte_basic.formatted_table;
  assert(cte_basic.rows.size() == 1);
  assert(cte_basic.rows[0].values[0] == "420");

  // Test 4.2: CTE with explicit column list
  SqlQueryResult cte_cols = db.execute_query(
    "WITH numbered(a, b) AS (SELECT 10, 25) SELECT a + b AS sum_ab FROM numbered;"
  );
  if (!cte_cols.success) {
    std::cerr << "CTE with explicit columns failed: " << cte_cols.error_message << "\n";
    return 1;
  }
  std::cout << "  CTE columns result:\n" << cte_cols.formatted_table;
  assert(cte_cols.rows.size() == 1);
  assert(cte_cols.rows[0].values[0] == "35");

  // Test 4.3: Multiple chained CTEs referencing each other
  SqlQueryResult cte_chained = db.execute_query(
    "WITH "
    "  step1 AS (SELECT 100 AS base_val), "
    "  step2 AS (SELECT base_val * 2 AS doubled FROM step1) "
    "SELECT doubled + 5 AS total FROM step2;"
  );
  if (!cte_chained.success) {
    std::cerr << "CTE chained failed: " << cte_chained.error_message << "\n";
    return 1;
  }
  std::cout << "  CTE chained result:\n" << cte_chained.formatted_table;
  assert(cte_chained.rows.size() == 1);
  assert(cte_chained.rows[0].values[0] == "205");

  // Test 4.4: CTE joined with an actual database table
  SqlQueryResult cte_join = db.execute_query(
    "WITH active_routes AS ( "
    "  SELECT id, route_code, driver_name FROM fleet_routes WHERE status = 'dispatched' "
    ") "
    "SELECT r.route_code, r.driver_name, f.truck_units "
    "FROM active_routes r "
    "JOIN fleet_routes f ON r.id = f.id;"
  );
  if (!cte_join.success) {
    std::cerr << "CTE join failed: " << cte_join.error_message << "\n";
    return 1;
  }
  std::cout << "  CTE JOIN result:\n" << cte_join.formatted_table;
  assert(cte_join.rows.size() == 1);
  assert(cte_join.rows[0].values[0] == "RT-CLE-001");
  assert(cte_join.rows[0].values[1] == "Bob Smith");
  assert(cte_join.rows[0].values[2] == "2");

  // Test 4.5: Recursive CTE (sequence generation)
  SqlQueryResult cte_rec = db.execute_query(
    "WITH RECURSIVE seq(n) AS ( "
    "  SELECT 1 "
    "  UNION ALL "
    "  SELECT n + 1 FROM seq WHERE n < 5 "
    ") "
    "SELECT n FROM seq ORDER BY n ASC;"
  );
  if (!cte_rec.success) {
    std::cerr << "CTE recursive failed: " << cte_rec.error_message << "\n";
    return 1;
  }
  std::cout << "  CTE recursive result:\n" << cte_rec.formatted_table;
  assert(cte_rec.rows.size() == 5);
  assert(cte_rec.rows[0].values[0] == "1");
  assert(cte_rec.rows[1].values[0] == "2");
  assert(cte_rec.rows[2].values[0] == "3");
  assert(cte_rec.rows[3].values[0] == "4");
  assert(cte_rec.rows[4].values[0] == "5");

  // Step 5: Micro-threshold boundary residency check
  std::cout << "[Step 5] Testing ASQL / ObjectStore Micro-Threshold Boundary Invariants...\n";
  assert(kernel.store().residency_config().block_objects == 8);
  assert(kernel.store().residency_config().max_active == 16);
  assert(kernel.store().residency_config().hot_budget == 64 * 1024);

  Surface self_surf = kernel.as(kernel.self());
  for (int i = 1; i <= 25; ++i) {
    StoredObject obj;
    obj.metadata.emplace_back("kind", "asql.sensor_reading");
    obj.metadata.emplace_back("sensor_id", std::to_string(i));
    obj.metadata.emplace_back("status", (i % 2 == 0) ? "active" : "calibrating");
    obj.payload = ByteBuffer(1024, static_cast<uint8_t>('A' + (i % 10)));
    auto id_r = self_surf.store(obj);
    assert(id_r.ok());
  }

  assert(kernel.store().cold_blocks_count() > 0);

  auto aql_res = storage::execute_aql(self_surf, "SELECT * FROM objects WHERE meta.kind = \"asql.sensor_reading\" AND meta.status = \"active\"");
  assert(aql_res.success);
  assert(aql_res.count == 12);

  assert(kernel.store().count() == kernel.store().active_count() + kernel.store().cold_objects_count());
  assert(kernel.store().list().size() == kernel.store().count());
  assert(kernel.store().hot_bytes() <= kernel.store().hot_budget());

  std::cout << "  [PASS] ASQL import micro-threshold boundary verified (cold_blocks_count="
            << kernel.store().cold_blocks_count() << " > 0, count identity and budget integrity held).\n";

  std::cout << "\nALL ASQL MYSQL / MARIADB IMPORT & CTE CHECKS PASSED SUCCESSFULLY!\n";
  return 0;
}
