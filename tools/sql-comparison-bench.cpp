// Comprehensive benchmark comparing Host MySQL, AIOS MySQL Wire Protocol, and AIOS Native ASQL.
#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <fstream>
#include <chrono>
#include <thread>
#include <sstream>

#include <unistd.h>
#include <cmath>

#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"
#include "aios/userland/mysql_wire.hpp"

using namespace aios;
using namespace aios::userland;

struct BenchMetric {
  std::string name;
  double host_mysql_ms = 0.0;
  double aios_wire_ms = 0.0;
  double aios_asql_ms = 0.0;
  double asql_vs_host_speedup = 0.0;
  double asql_vs_wire_speedup = 0.0;
};

static uint64_t now_micros() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
  std::cout << "========================================================================\n";
  std::cout << "  AIOS ASQL vs Host MySQL vs AIOS MySQL Wire Protocol Benchmark Suite   \n";
  std::cout << "========================================================================\n\n";

  // 1. Prepare schema and test data
  const std::string ddl = 
    "CREATE TABLE IF NOT EXISTS stockmaster_copy1 ("
    "  stockid VARCHAR(32) PRIMARY KEY,"
    "  categoryid VARCHAR(16),"
    "  lineid VARCHAR(16),"
    "  description VARCHAR(128),"
    "  units VARCHAR(16),"
    "  mbflag VARCHAR(4),"
    "  actualcost DOUBLE,"
    "  lastcost DOUBLE,"
    "  materialcost DOUBLE,"
    "  labourcost DOUBLE,"
    "  overheadcost DOUBLE,"
    "  discontinued INT,"
    "  barcode VARCHAR(32),"
    "  netweight DOUBLE"
    ");";

  // Read raw lines from /repos/ss.sql
  std::ifstream ss_file("/repos/ss.sql");
  if (!ss_file.is_open()) {
    std::cerr << "Failed to open /repos/ss.sql\n";
    return 1;
  }

  std::vector<std::string> raw_inserts;
  std::vector<std::string> sample_stockids;
  std::string line;
  size_t max_rows = 5000;
  if (argc > 1) max_rows = std::stoul(argv[1]);

  while (std::getline(ss_file, line) && raw_inserts.size() < max_rows) {
    if (line.rfind("INSERT", 0) == 0) {
      raw_inserts.push_back(line);
      // Extract stockid: VALUES ('ABC03HCS033'
      auto pos = line.find("VALUES ('");
      if (pos != std::string::npos) {
        auto end_pos = line.find("'", pos + 9);
        if (end_pos != std::string::npos) {
          sample_stockids.push_back(line.substr(pos + 9, end_pos - (pos + 9)));
        }
      }
    }
  }
  std::cout << "Loaded " << raw_inserts.size() << " real ERP stock records from /repos/ss.sql\n\n";

  // 2. Setup AIOS Native ASQL Kernel & DB
  char aios_root[] = "/tmp/aios-sqlbench-root-XXXXXX";
  if (!mkdtemp(aios_root)) { perror("mkdtemp"); return 1; }
  KernelConfig kcfg;
  kcfg.root = aios_root;
  auto k_res = Kernel::boot(kcfg);
  if (!k_res.ok()) { std::cerr << "Kernel boot failed: " << k_res.error().message << "\n"; return 1; }
  Kernel& kernel = **k_res;

  SqlDatabase asql_db(kernel);
  asql_db.init("sql_db");
  asql_db.execute_query("DROP TABLE IF EXISTS stockmaster_copy1;");
  asql_db.execute_query(ddl);

  // 3. Setup AIOS Wire Protocol Server
  MysqlWireServer wire_server(kernel, asql_db, 0, "127.0.0.1");
  if (!wire_server.start().ok()) { std::cerr << "Wire server start failed\n"; return 1; }
  uint16_t wire_port = wire_server.port();
  wire_server.start_background();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  MariaWireSession aios_wire_client("127.0.0.1", wire_port);
  if (!aios_wire_client.connect("root", "", "aios_db").ok()) {
    std::cerr << "AIOS wire client connection failed\n"; return 1;
  }

  // 4. Setup Host MySQL Client
  MariaWireSession host_mysql_client("127.0.0.1", 3306);
  bool host_available = host_mysql_client.connect("bench", "bench", "bench_db").ok();
  if (!host_available) {
    std::cout << "[Note] Host MySQL (127.0.0.1:3306) connection failed with bench/bench; trying root...\n";
    host_available = host_mysql_client.connect("root", "", "bench_db").ok();
  }
  if (!host_available) {
    std::cerr << "Failed to connect to Host MySQL server\n";
    return 1;
  }
  host_mysql_client.query("DROP TABLE IF EXISTS stockmaster_copy1;");
  host_mysql_client.query(ddl);

  std::vector<BenchMetric> metrics;

  // --------------------------------------------------------------------------
  // BENCHMARK 1: Ingestion / Bulk INSERTs
  // --------------------------------------------------------------------------
  std::cout << "[1/6] Benchmarking Record Ingestion (" << raw_inserts.size() << " INSERT statements)...\n";
  
  // Host MySQL
  uint64_t t0 = now_micros();
  host_mysql_client.query("START TRANSACTION;");
  for (const auto& ins : raw_inserts) {
    host_mysql_client.query(ins);
  }
  host_mysql_client.query("COMMIT;");
  uint64_t t1 = now_micros();
  double host_insert_ms = (t1 - t0) / 1000.0;

  // AIOS Wire Protocol
  // Reset table in asql
  asql_db.execute_query("DROP TABLE IF EXISTS stockmaster_copy1;");
  asql_db.execute_query(ddl);
  t0 = now_micros();
  for (const auto& ins : raw_inserts) {
    aios_wire_client.query(ins);
  }
  t1 = now_micros();
  double wire_insert_ms = (t1 - t0) / 1000.0;

  // AIOS Native ASQL In-Process (RowIndex accelerated)
  asql_db.execute_query("DROP TABLE IF EXISTS stockmaster_copy1;");
  asql_db.execute_query(ddl);
  t0 = now_micros();
  for (const auto& ins : raw_inserts) {
    asql_db.execute_query(ins);
  }
  t1 = now_micros();
  double asql_insert_ms = (t1 - t0) / 1000.0;

  metrics.push_back({
    "Bulk Ingestion (" + std::to_string(raw_inserts.size()) + " rows)",
    host_insert_ms,
    wire_insert_ms,
    asql_insert_ms,
    host_insert_ms / asql_insert_ms,
    wire_insert_ms / asql_insert_ms
  });

  // --------------------------------------------------------------------------
  // BENCHMARK 2: Point Lookups by Primary Key (Indexed)
  // --------------------------------------------------------------------------
  const size_t POINT_QUERIES = std::min((size_t)1000, sample_stockids.size());
  std::cout << "[2/6] Benchmarking Point Primary Key Lookups (" << POINT_QUERIES << " queries)...\n";

  t0 = now_micros();
  for (size_t i = 0; i < POINT_QUERIES; ++i) {
    std::string q = "SELECT stockid, description, materialcost, lastcost FROM stockmaster_copy1 WHERE stockid = '" + sample_stockids[i] + "';";
    auto r = host_mysql_client.query(q);
  }
  t1 = now_micros();
  double host_pk_ms = (t1 - t0) / 1000.0;

  t0 = now_micros();
  for (size_t i = 0; i < POINT_QUERIES; ++i) {
    std::string q = "SELECT stockid, description, materialcost, lastcost FROM stockmaster_copy1 WHERE stockid = '" + sample_stockids[i] + "';";
    auto r = aios_wire_client.query(q);
  }
  t1 = now_micros();
  double wire_pk_ms = (t1 - t0) / 1000.0;

  t0 = now_micros();
  for (size_t i = 0; i < POINT_QUERIES; ++i) {
    std::string q = "SELECT stockid, description, materialcost, lastcost FROM stockmaster_copy1 WHERE stockid = '" + sample_stockids[i] + "';";
    auto r = asql_db.execute_query(q);
  }
  t1 = now_micros();
  double asql_pk_ms = (t1 - t0) / 1000.0;

  metrics.push_back({
    "PK Point Lookup (" + std::to_string(POINT_QUERIES) + " queries)",
    host_pk_ms,
    wire_pk_ms,
    asql_pk_ms,
    host_pk_ms / asql_pk_ms,
    wire_pk_ms / asql_pk_ms
  });

  // --------------------------------------------------------------------------
  // BENCHMARK 3: Range Scan with Predicate Filter
  // --------------------------------------------------------------------------
  const size_t SCAN_ROUNDS = 50;
  std::cout << "[3/6] Benchmarking Range Scans (" << SCAN_ROUNDS << " iterations)...\n";
  std::string range_q = "SELECT stockid, description, materialcost FROM stockmaster_copy1 WHERE materialcost >= 10.0 AND materialcost <= 30.0;";

  t0 = now_micros();
  for (size_t i = 0; i < SCAN_ROUNDS; ++i) {
    auto r = host_mysql_client.query(range_q);
  }
  t1 = now_micros();
  double host_range_ms = (t1 - t0) / 1000.0;

  t0 = now_micros();
  for (size_t i = 0; i < SCAN_ROUNDS; ++i) {
    auto r = aios_wire_client.query(range_q);
  }
  t1 = now_micros();
  double wire_range_ms = (t1 - t0) / 1000.0;

  t0 = now_micros();
  for (size_t i = 0; i < SCAN_ROUNDS; ++i) {
    auto r = asql_db.execute_query(range_q);
  }
  t1 = now_micros();
  double asql_range_ms = (t1 - t0) / 1000.0;

  metrics.push_back({
    "Range Scan / Filter (" + std::to_string(SCAN_ROUNDS) + " runs)",
    host_range_ms,
    wire_range_ms,
    asql_range_ms,
    host_range_ms / asql_range_ms,
    wire_range_ms / asql_range_ms
  });

  // --------------------------------------------------------------------------
  // BENCHMARK 4: Aggregation & GROUP BY
  // --------------------------------------------------------------------------
  const size_t AGG_ROUNDS = 50;
  std::cout << "[4/6] Benchmarking Aggregation / GROUP BY (" << AGG_ROUNDS << " iterations)...\n";
  std::string agg_q = "SELECT categoryid, COUNT(*), SUM(materialcost), AVG(lastcost) FROM stockmaster_copy1 GROUP BY categoryid;";

  t0 = now_micros();
  for (size_t i = 0; i < AGG_ROUNDS; ++i) {
    auto r = host_mysql_client.query(agg_q);
  }
  t1 = now_micros();
  double host_agg_ms = (t1 - t0) / 1000.0;

  t0 = now_micros();
  for (size_t i = 0; i < AGG_ROUNDS; ++i) {
    auto r = aios_wire_client.query(agg_q);
  }
  t1 = now_micros();
  double wire_agg_ms = (t1 - t0) / 1000.0;

  t0 = now_micros();
  for (size_t i = 0; i < AGG_ROUNDS; ++i) {
    auto r = asql_db.execute_query(agg_q);
  }
  t1 = now_micros();
  double asql_agg_ms = (t1 - t0) / 1000.0;

  metrics.push_back({
    "Aggregation & GROUP BY (" + std::to_string(AGG_ROUNDS) + " runs)",
    host_agg_ms,
    wire_agg_ms,
    asql_agg_ms,
    host_agg_ms / asql_agg_ms,
    wire_agg_ms / asql_agg_ms
  });

  // --------------------------------------------------------------------------
  // BENCHMARK 5: In-Place Point UPDATEs
  // --------------------------------------------------------------------------
  const size_t UPDATE_ROUNDS = std::min((size_t)500, sample_stockids.size());
  std::cout << "[5/6] Benchmarking Point UPDATEs (" << UPDATE_ROUNDS << " updates)...\n";

  t0 = now_micros();
  host_mysql_client.query("START TRANSACTION;");
  for (size_t i = 0; i < UPDATE_ROUNDS; ++i) {
    std::string uq = "UPDATE stockmaster_copy1 SET materialcost = materialcost + 1.0 WHERE stockid = '" + sample_stockids[i] + "';";
    host_mysql_client.query(uq);
  }
  host_mysql_client.query("COMMIT;");
  t1 = now_micros();
  double host_upd_ms = (t1 - t0) / 1000.0;

  t0 = now_micros();
  aios_wire_client.query("START TRANSACTION;");
  for (size_t i = 0; i < UPDATE_ROUNDS; ++i) {
    std::string uq = "UPDATE stockmaster_copy1 SET materialcost = 25.5 WHERE stockid = '" + sample_stockids[i] + "';";
    aios_wire_client.query(uq);
  }
  aios_wire_client.query("COMMIT;");
  t1 = now_micros();
  double wire_upd_ms = (t1 - t0) / 1000.0;

  t0 = now_micros();
  asql_db.execute_query("START TRANSACTION;");
  for (size_t i = 0; i < UPDATE_ROUNDS; ++i) {
    std::string uq = "UPDATE stockmaster_copy1 SET materialcost = 25.5 WHERE stockid = '" + sample_stockids[i] + "';";
    asql_db.execute_query(uq);
  }
  asql_db.execute_query("COMMIT;");
  t1 = now_micros();
  double asql_upd_ms = (t1 - t0) / 1000.0;

  metrics.push_back({
    "Point UPDATE (" + std::to_string(UPDATE_ROUNDS) + " updates)",
    host_upd_ms,
    wire_upd_ms,
    asql_upd_ms,
    host_upd_ms / asql_upd_ms,
    wire_upd_ms / asql_upd_ms
  });

  // --------------------------------------------------------------------------
  // BENCHMARK 6: Full Table Scan & ORDER BY
  // --------------------------------------------------------------------------
  const size_t ORDER_ROUNDS = 20;
  std::cout << "[6/6] Benchmarking Full Scan with ORDER BY (" << ORDER_ROUNDS << " runs)...\n";
  std::string order_q = "SELECT stockid, description, materialcost FROM stockmaster_copy1 ORDER BY materialcost DESC LIMIT 50;";

  t0 = now_micros();
  for (size_t i = 0; i < ORDER_ROUNDS; ++i) {
    auto r = host_mysql_client.query(order_q);
  }
  t1 = now_micros();
  double host_order_ms = (t1 - t0) / 1000.0;

  t0 = now_micros();
  for (size_t i = 0; i < ORDER_ROUNDS; ++i) {
    auto r = aios_wire_client.query(order_q);
  }
  t1 = now_micros();
  double wire_order_ms = (t1 - t0) / 1000.0;

  t0 = now_micros();
  for (size_t i = 0; i < ORDER_ROUNDS; ++i) {
    auto r = asql_db.execute_query(order_q);
  }
  t1 = now_micros();
  double asql_order_ms = (t1 - t0) / 1000.0;

  metrics.push_back({
    "Scan + ORDER BY + LIMIT (" + std::to_string(ORDER_ROUNDS) + " runs)",
    host_order_ms,
    wire_order_ms,
    asql_order_ms,
    host_order_ms / asql_order_ms,
    wire_order_ms / asql_order_ms
  });

  // --------------------------------------------------------------------------
  // Summary Table Output
  // --------------------------------------------------------------------------
  std::cout << "\n========================================================================================\n";
  std::cout << "                             BENCHMARK RESULTS SUMMARY                                  \n";
  std::cout << "========================================================================================\n";
  std::cout << std::left << std::setw(36) << "Workload / Operation"
            << std::right << std::setw(15) << "Host MySQL"
            << std::setw(15) << "AIOS Wire"
            << std::setw(15) << "Native ASQL"
            << std::setw(15) << "ASQL Speedup"
            << "\n";
  std::cout << std::string(96, '-') << "\n";

  for (const auto& m : metrics) {
    std::cout << std::left << std::setw(36) << m.name
              << std::right << std::setw(13) << std::fixed << std::setprecision(2) << m.host_mysql_ms << " ms"
              << std::setw(13) << std::fixed << std::setprecision(2) << m.aios_wire_ms << " ms"
              << std::setw(13) << std::fixed << std::setprecision(2) << m.aios_asql_ms << " ms"
              << std::setw(13) << std::fixed << std::setprecision(2) << m.asql_vs_host_speedup << "x"
              << "\n";
  }
  std::cout << "========================================================================================\n";

  return 0;
}
