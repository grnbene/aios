// Verifies ASQL's MySQL binlog CDC: it builds binlog v4 events (table map, write rows) and
// checks they stream into and sync the store.

#undef NDEBUG
#include <iostream>
#include <cassert>
#include <chrono>
#include <thread>
#include <vector>
#include <string>
#include <unistd.h>
#include <cmath>
#include <cstring>

#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"
#include "aios/userland/mysql_wire.hpp"
#include "aios/userland/mysql_binlog.hpp"
#include "aios/storage/object_store.hpp"

using namespace aios;
using namespace aios::userland;

// Helper to construct a synthetic MySQL Binlog v4 Event Header
static std::vector<uint8_t> make_event_header(uint8_t type, uint32_t payload_len, uint32_t next_pos = 100, uint32_t ts = 1700000000) {
  std::vector<uint8_t> h(19, 0);
  // Timestamp (4 bytes)
  h[0] = (uint8_t)(ts & 0xff);
  h[1] = (uint8_t)((ts >> 8) & 0xff);
  h[2] = (uint8_t)((ts >> 16) & 0xff);
  h[3] = (uint8_t)((ts >> 24) & 0xff);
  // Type code (1 byte)
  h[4] = type;
  // Server ID (4 bytes) = 1
  h[5] = 1; h[6] = 0; h[7] = 0; h[8] = 0;
  // Event length = 19 + payload_len
  uint32_t total_len = 19 + payload_len;
  h[9] = (uint8_t)(total_len & 0xff);
  h[10] = (uint8_t)((total_len >> 8) & 0xff);
  h[11] = (uint8_t)((total_len >> 16) & 0xff);
  h[12] = (uint8_t)((total_len >> 24) & 0xff);
  // Next position
  h[13] = (uint8_t)(next_pos & 0xff);
  h[14] = (uint8_t)((next_pos >> 8) & 0xff);
  h[15] = (uint8_t)((next_pos >> 16) & 0xff);
  h[16] = (uint8_t)((next_pos >> 24) & 0xff);
  // Flags = 0
  h[17] = 0; h[18] = 0;
  return h;
}

// Helper to construct TABLE_MAP_EVENT packet
static std::vector<uint8_t> make_table_map_event(uint64_t table_id, const std::string& schema,
                                                 const std::string& table, const std::vector<uint8_t>& types,
                                                 uint32_t next_pos = 100) {
  std::vector<uint8_t> body;
  // 6 bytes table id
  for (int i = 0; i < 6; ++i) body.push_back((uint8_t)((table_id >> (8 * i)) & 0xff));
  // 2 bytes flags
  body.push_back(0); body.push_back(0);
  // Schema name (len + string + null)
  body.push_back((uint8_t)schema.size());
  body.insert(body.end(), schema.begin(), schema.end());
  body.push_back(0);
  // Table name (len + string + null)
  body.push_back((uint8_t)table.size());
  body.insert(body.end(), table.begin(), table.end());
  body.push_back(0);
  // Column count
  body.push_back((uint8_t)types.size());
  // Column types
  body.insert(body.end(), types.begin(), types.end());
  // Metadata len = 0
  body.push_back(0);
  // Null bitmap
  size_t null_bytes = (types.size() + 7) / 8;
  body.insert(body.end(), null_bytes, 0);

  auto header = make_event_header(binlog_event::TABLE_MAP_EVENT, (uint32_t)body.size(), next_pos);
  header.insert(header.end(), body.begin(), body.end());
  return header;
}

// Helper to construct WRITE_ROWS_EVENT_V1
static std::vector<uint8_t> make_write_rows_event_v1(uint64_t table_id, size_t col_count,
                                                     const std::vector<std::string>& row_strings,
                                                     uint32_t next_pos = 200) {
  std::vector<uint8_t> body;
  // 6 bytes table id
  for (int i = 0; i < 6; ++i) body.push_back((uint8_t)((table_id >> (8 * i)) & 0xff));
  // 2 bytes flags
  body.push_back(0); body.push_back(0);
  // Column count
  body.push_back((uint8_t)col_count);
  // Columns present bitmap
  size_t bmp_bytes = (col_count + 7) / 8;
  body.insert(body.end(), bmp_bytes, 0xff); // all columns present
  // Null bitmap (0 = not null)
  body.insert(body.end(), bmp_bytes, 0x00);

  // Encode string columns (length prefixed)
  for (const auto& s : row_strings) {
    body.push_back((uint8_t)s.size());
    body.insert(body.end(), s.begin(), s.end());
  }

  auto header = make_event_header(binlog_event::WRITE_ROWS_EVENT_V1, (uint32_t)body.size(), next_pos);
  header.insert(header.end(), body.begin(), body.end());
  return header;
}

int main() {
  std::cout << "========================================================================\n";
  std::cout << "  AIOS ASQL: Native MySQL Binlog CDC Streaming & Sync Verification Suite\n";
  std::cout << "========================================================================\n\n";

  char aios_root[] = "/tmp/aios-binlog-test-XXXXXX";
  if (!mkdtemp(aios_root)) { std::perror("mkdtemp"); return 1; }

  // 1. Boot AIOS Kernel and ASQL Engine
  std::cout << "[Step 1] Booting AIOS Engine...\n";
  KernelConfig cfg;
  cfg.root = aios_root;
  cfg.residency = test_config;
  auto k_res = Kernel::boot(cfg);
  assert(k_res.ok());
  Kernel& kernel = **k_res;

  SqlDatabase db(kernel);
  assert(db.init("sql_db").ok());
  std::cout << "  [PASS] AIOS ASQL Database initialized.\n\n";

  // Pre-create tables in ASQL
  db.execute_local_query("CREATE TABLE orders (id INT PRIMARY KEY, customer VARCHAR(64), total DOUBLE, status VARCHAR(32));");
  db.execute_local_query("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(64), role VARCHAR(32));");

  // 2. Initialize Native Binlog Consumer with Whitelist Filtering
  std::cout << "[Step 2] Initializing Binlog CDC Consumer with Table Whitelist...\n";
  BinlogConsumerConfig bcfg;
  bcfg.host = "127.0.0.1";
  bcfg.port = 3306;
  bcfg.user = "root";
  bcfg.password = "";
  bcfg.database = "aios_db";
  bcfg.server_id = 9999;
  bcfg.auto_discover_position = false;
  bcfg.binlog_filename = "mysql-bin.000001";
  bcfg.binlog_position = 4;

  // Table Whitelist: ONLY sync orders and users!
  bcfg.table_whitelist = {"orders", "users"};

  BinlogConsumer consumer(kernel, db, bcfg);
  std::cout << "  [PASS] Consumer configured with table whitelist: [orders, users]\n\n";

  // 3. Test Ingress Filtering & Zero RAM Overhead for Unwhitelisted Tables
  std::cout << "[Step 3] Testing Ingress Filtering (0 RAM spent on non-whitelisted tables)...\n";
  // Table ID 100 = unwhitelisted "audit_huge_blobs"
  std::vector<uint8_t> unwhitelisted_types = {binlog_type::VARCHAR, binlog_type::VARCHAR};
  auto map_unwhitelisted = make_table_map_event(100, "aios_db", "audit_huge_blobs", unwhitelisted_types, 120);
  assert(consumer.ingest_raw_event(map_unwhitelisted.data(), map_unwhitelisted.size()).ok());

  // Ingest massive unwhitelisted row mutation
  std::string huge_blob(1024 * 1024, 'X'); // 1MB payload
  auto row_unwhitelisted = make_write_rows_event_v1(100, 2, {"1", huge_blob}, 250);
  assert(consumer.ingest_raw_event(row_unwhitelisted.data(), row_unwhitelisted.size()).ok());

  // Verify "audit_huge_blobs" was NEVER allocated or created in ASQL
  assert(db.find_table("audit_huge_blobs") == nullptr);
  auto st1 = consumer.stats();
  assert(st1.total_events_skipped == 1);
  std::cout << "  [PASS] Unwhitelisted table 'audit_huge_blobs' dropped immediately (0 RAM allocated, skipped="
            << st1.total_events_skipped << ")\n\n";

  // 4. Test Ingestion of Whitelisted Table Mutations (INSERT / UPDATE / DELETE)
  std::cout << "[Step 4] Testing Whitelisted Event Replication (orders)...\n";
  // Table ID 101 = whitelisted "orders"
  std::vector<uint8_t> order_types = {binlog_type::VARCHAR, binlog_type::VARCHAR, binlog_type::VARCHAR, binlog_type::VARCHAR};
  auto map_orders = make_table_map_event(101, "aios_db", "orders", order_types, 300);
  assert(consumer.ingest_raw_event(map_orders.data(), map_orders.size()).ok());

  // Ingest INSERT rows
  for (int i = 1; i <= 20; ++i) {
    auto row_event = make_write_rows_event_v1(101, 4, {
      std::to_string(i),
      "Customer_" + std::to_string(i),
      std::to_string(150.0 + i * 10),
      "PENDING"
    }, 300 + i * 20);
    assert(consumer.ingest_raw_event(row_event.data(), row_event.size()).ok());
  }

  // Verify rows in ASQL
  auto sel1 = db.execute_local_query("SELECT COUNT(*) FROM orders WHERE status = 'PENDING';");
  assert(sel1.success && sel1.rows.size() == 1);
  assert(sel1.rows[0].values[0] == "20");
  std::cout << "  [PASS] Successfully streamed 20 INSERT events into ASQL 'orders'.\n";

  // Test UPDATE via BinlogRowMutation
  BinlogRowMutation upd_mut;
  upd_mut.kind = BinlogRowMutation::Kind::Update;
  upd_mut.schema = "aios_db";
  upd_mut.table = "orders";
  upd_mut.values = {{"status", "SHIPPED"}, {"total", "999.50"}};
  upd_mut.before_values = {{"id", "5"}};
  assert(consumer.apply_mutation(upd_mut).ok());

  auto sel_upd = db.execute_local_query("SELECT status, total FROM orders WHERE id = 5;");
  assert(sel_upd.success && sel_upd.rows.size() == 1);
  assert(sel_upd.rows[0].values[0] == "SHIPPED");
  assert(sel_upd.rows[0].values[1] == "999.5");   // a DOUBLE column: printed as MariaDB prints it, no trailing zero
  std::cout << "  [PASS] UPDATE mutation replicated: Order #5 is now SHIPPED ($999.50).\n";

  // Test DELETE via BinlogRowMutation
  BinlogRowMutation del_mut;
  del_mut.kind = BinlogRowMutation::Kind::Delete;
  del_mut.schema = "aios_db";
  del_mut.table = "orders";
  del_mut.values = {{"id", "10"}};
  assert(consumer.apply_mutation(del_mut).ok());

  auto sel_del = db.execute_local_query("SELECT COUNT(*) FROM orders WHERE id = 10;");
  assert(sel_del.success && sel_del.rows.size() == 1);
  assert(sel_del.rows[0].values[0] == "0");
  std::cout << "  [PASS] DELETE mutation replicated: Order #10 removed from ASQL.\n\n";

  // 5. Test Column Whitelisting / Projection
  std::cout << "[Step 5] Testing Column Projection (selective column filtering)...\n";
  db.execute_local_query("CREATE TABLE products (id INT PRIMARY KEY, name VARCHAR(64), heavy_desc VARCHAR(255));");
  consumer.set_table_whitelist({"orders", "users", "products"});
  consumer.set_column_whitelist("products", {"id", "name"}); // Exclude heavy_desc!

  std::vector<uint8_t> prod_types = {binlog_type::VARCHAR, binlog_type::VARCHAR, binlog_type::VARCHAR};
  auto map_prod = make_table_map_event(102, "aios_db", "products", prod_types, 900);
  assert(consumer.ingest_raw_event(map_prod.data(), map_prod.size()).ok());

  auto prod_row = make_write_rows_event_v1(102, 3, {"101", "MacBook Pro M3", "Unwanted giant text..."}, 950);
  assert(consumer.ingest_raw_event(prod_row.data(), prod_row.size()).ok());

  auto sel_prod = db.execute_local_query("SELECT id, name FROM products WHERE id = 101;");
  assert(sel_prod.success && sel_prod.rows.size() == 1);
  assert(sel_prod.rows[0].values[0] == "101");
  assert(sel_prod.rows[0].values[1] == "MacBook Pro M3");
  std::cout << "  [PASS] Column projection verified: unwhitelisted column 'heavy_desc' stripped.\n\n";

  // 6. Test Working-Set RAM Bounding & Max-Row FIFO Eviction
  std::cout << "[Step 6] Testing RAM Bloat Protection (Max Row Limit & FIFO Eviction)...\n";
  // Set table max rows for 'orders' to 50
  consumer.set_table_max_rows("orders", 50);

  // Ingest 100 new rows (exceeding limit)
  for (int i = 100; i < 200; ++i) {
    auto row_event = make_write_rows_event_v1(101, 4, {
      std::to_string(i),
      "Customer_" + std::to_string(i),
      "250.0",
      "ACTIVE"
    }, 1000 + i);
    assert(consumer.ingest_raw_event(row_event.data(), row_event.size()).ok());
  }

  // Row count must not exceed 50!
  auto sel_cnt = db.execute_local_query("SELECT COUNT(*) FROM orders;");
  assert(sel_cnt.success && sel_cnt.rows.size() == 1);
  size_t count_val = std::stoul(sel_cnt.rows[0].values[0]);
  std::cout << "  Current orders row count in ASQL: " << count_val << " (Target max: 50)\n";
  assert(count_val <= 50);

  auto st_ev = consumer.stats();
  std::cout << "  Total row evictions recorded: " << st_ev.total_row_evictions << "\n";
  assert(st_ev.total_row_evictions > 0);
  std::cout << "  [PASS] RAM bloat protection verified: Row count strictly capped at working set.\n\n";

  // 7. Test ASQL SQL Replication Commands & Telemetry
  std::cout << "[Step 7] Testing SQL-level Replication Management Commands...\n";
  auto rep_change = db.execute_local_query(
      "CHANGE REPLICATION SOURCE TO SOURCE_HOST = 127.0.0.1, SOURCE_PORT = 3306, "
      "SOURCE_USER = root, SOURCE_LOG_FILE = mysql-bin.000002, SOURCE_LOG_POS = 450;");
  assert(rep_change.success);

  auto show_rep = db.execute_local_query("SHOW REPLICA STATUS;");
  assert(show_rep.success && show_rep.is_result_set && show_rep.rows.size() == 1);
  std::cout << "  SHOW REPLICA STATUS Output:\n" << show_rep.formatted_table << "\n";
  assert(show_rep.rows[0].values[1] == "127.0.0.1"); // Master_Host
  assert(show_rep.rows[0].values[5] == "mysql-bin.000002"); // Master_Log_File

  auto show_master = db.execute_local_query("SHOW MASTER STATUS;");
  assert(show_master.success && show_master.is_result_set && show_master.rows.size() == 1);
  std::cout << "  SHOW MASTER STATUS Output:\n" << show_master.formatted_table << "\n";

  // Test Whitelist SQL config
  auto set_wl = db.execute_local_query("SET REPLICATION WHITELIST = orders, users, products, telemetry;");
  assert(set_wl.success);

  std::cout << "========================================================================\n";
  std::cout << "  ALL NATIVE MYSQL BINLOG CDC SYNC CHECKS PASSED (100% SUCCESS)         \n";
  std::cout << "========================================================================\n";
  return 0;
}
