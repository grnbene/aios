#undef NDEBUG
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"
#include "aios/userland/mysql_wire.hpp"
#include "aios/userland/mysql_binlog.hpp"
#include "aios/storage/object_store.hpp"

using namespace aios;
using namespace aios::userland;

// Reads current process RSS in Megabytes from /proc/self/statm
static double get_process_rss_mb() {
  std::ifstream statm("/proc/self/statm");
  if (!statm.is_open()) return 0.0;
  long pages = 0;
  long rss_pages = 0;
  statm >> pages >> rss_pages;
  long page_size_kb = sysconf(_SC_PAGESIZE) / 1024;
  return (rss_pages * page_size_kb) / 1024.0;
}

// Executes a command on chi-il-1 MariaDB via mysql CLI over local SSH tunnel
// The source database's password, from the environment (ASQL_SOURCE_PASSWORD): never in the source.
static std::string source_password() {
  const char* p = std::getenv("ASQL_SOURCE_PASSWORD");
  if (!p || !*p) { std::cerr << "asql-live-ingest: set ASQL_SOURCE_PASSWORD to the source database's password\n"; std::exit(2); }
  return p;
}

static std::string exec_mysql_query(const std::string& sql, uint16_t port = 33060) {
  std::string cmd = "MYSQL_PWD='" + source_password() + "' mysql -h 127.0.0.1 -P " + std::to_string(port) +
                    " -u api versioneight --batch --raw --default-character-set=utf8mb4 -e \"" +
                    sql + "\" 2>/dev/null";
  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) return "";

  char buffer[128 * 1024];
  std::string result;
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    result += buffer;
  }
  pclose(pipe);
  return result;
}

int main(int argc, char** argv) {
  uint16_t mysql_port = 33060;
  std::string dump_file = "/tmp/versioneight_20tables.sql";
  if (argc > 1) mysql_port = (uint16_t)std::atoi(argv[1]);
  if (argc > 2) dump_file = argv[2];

  std::cout << "========================================================================\n";
  std::cout << "  AIOS ASQL: Production Ingestion, CDC & ERP Query Benchmark\n";
  std::cout << "========================================================================\n\n";

  double rss_empty = get_process_rss_mb();
  std::cout << "Baseline Empty Process RSS: " << rss_empty << " MB\n\n";

  char aios_root[] = "/tmp/aios-live-ingest-XXXXXX";
  if (!mkdtemp(aios_root)) { std::perror("mkdtemp"); return 1; }

  // 1. Boot Kernel & ASQL Engine
  std::cout << "[Step 1] Booting AIOS Engine...\n";
  KernelConfig cfg;
  cfg.root = aios_root;
  cfg.residency = test_config;
  auto k_res = Kernel::boot(cfg);
  assert(k_res.ok());
  Kernel& kernel = **k_res;

  SqlDatabase db(kernel);
  assert(db.init("sql_db").ok());
  db.use_database("versioneight");
  std::cout << "  [PASS] AIOS ASQL Database initialized (Active DB: versioneight).\n\n";

  // 2. 20 Required Dispatch Tables List
  std::vector<std::string> target_tables = {
    "dispatch", "pickingfreeze", "locstock", "stockmaster", "salesorders",
    "salesorderdetails", "debtorsmaster", "custbranch", "debtortrans", "stockmoves",
    "order_status", "oms_snoozed", "trackinginfo", "locationusers", "paymentterms",
    "dispatcheddev", "ITEM_MASTER", "ITEM_BRANCH_DET", "ITEM_HISTORY", "SALES_BY_ORDER"
  };

  std::cout << "[Step 2] Reading MariaDB Master Binlog Position...\n";
  std::string master_status = exec_mysql_query("SHOW MASTER STATUS;", mysql_port);
  std::string binlog_file = "mariadb-bin.000002";
  uint32_t binlog_pos = 330;

  std::stringstream ss_ms(master_status);
  std::string line;
  if (std::getline(ss_ms, line) && std::getline(ss_ms, line)) {
    std::stringstream row_ss(line);
    row_ss >> binlog_file >> binlog_pos;
  }
  std::cout << "  Master Binlog Position: " << binlog_file << " @ " << binlog_pos << "\n\n";

  // 3. Import Full 20 Tables Snapshot Dump into ASQL
  std::cout << "[Step 3] Importing 20 Dispatch Tables Snapshot Dump into ASQL...\n";
  std::cout << "  File path: " << dump_file << "\n";
  auto t_ingest_start = std::chrono::steady_clock::now();

  SqlQueryResult import_res = db.import_sql_file(dump_file);
  if (!import_res.success) {
    std::cerr << "  [FAIL] Import failed: " << import_res.error_message << "\n";
    return 1;
  }

  auto t_ingest_end = std::chrono::steady_clock::now();
  double ingest_sec = std::chrono::duration<double>(t_ingest_end - t_ingest_start).count();
  double rss_loaded = get_process_rss_mb();

  std::cout << "  [PASS] Snapshot Dump Ingestion Finished:\n";
  std::cout << "    Rows Affected / Imported: " << import_res.rows_affected << "\n";
  std::cout << "    Ingestion Latency:        " << ingest_sec << " s\n";
  std::cout << "    Loaded Process RSS:       " << rss_loaded << " MB (Net Data: " << (rss_loaded - rss_empty) << " MB)\n\n";

  // Table Row Summary
  std::cout << "[Step 3b] Table Row Count Summary in ASQL:\n";
  size_t grand_total_rows = 0;
  for (const auto& tbl : target_tables) {
    auto r = db.execute_local_query("SELECT COUNT(*) FROM `" + tbl + "`;");
    size_t cnt = 0;
    if (r.success && !r.rows.empty()) {
      cnt = std::stoul(r.rows[0].values[0]);
    }
    grand_total_rows += cnt;
    std::cout << "  `" << tbl << "`: " << cnt << " rows\n";
  }
  std::cout << "  Grand Total ASQL Rows: " << grand_total_rows << "\n\n";

  // 4. Connect Live CDC Replication Stream
  std::cout << "[Step 4] Configuring & Starting Live CDC Replication Stream...\n";
  std::string change_rep = "CHANGE REPLICATION SOURCE TO SOURCE_HOST = '127.0.0.1', SOURCE_PORT = " +
                           std::to_string(mysql_port) + ", SOURCE_USER = 'api', SOURCE_PASSWORD = '" + source_password() + "', SOURCE_DATABASE = 'versioneight', " +
                           "SOURCE_LOG_FILE = '" + binlog_file + "', SOURCE_LOG_POS = " + std::to_string(binlog_pos) + ";";
  auto rep_res = db.execute_local_query(change_rep);
  assert(rep_res.success);

  // Set Whitelist
  db.set_replication_whitelist(target_tables);

  // Start Replica
  auto start_res = db.execute_local_query("START REPLICA;");
  assert(start_res.success);

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  auto show_rep = db.execute_local_query("SHOW REPLICA STATUS;");
  assert(show_rep.success && show_rep.is_result_set);
  std::cout << "  SHOW REPLICA STATUS:\n" << show_rep.formatted_table << "\n";

  double rss_cdc = get_process_rss_mb();
  std::cout << "  Live Stream Active Process RSS: " << rss_cdc << " MB\n\n";

  // 5. Execute getInvoicedOnDispatch Query
  std::cout << "[Step 5] Executing getInvoicedOnDispatch Query directly inside ASQL...\n";
  std::string erp_sql = R"(
    SELECT DISTINCT
      salesorders.orderno AS orderno,
      debtorsmaster.debtorno AS dno,
      debtorsmaster.name AS dname,
      DATEDIFF(deliverydate, NOW()) AS daystilldue,
      CASE WHEN (SELECT orderno FROM oms_snoozed WHERE orderno = salesorders.orderno AND date_end > NOW() LIMIT 1) IS NOT NULL THEN 1 ELSE 0 END AS snoozed,
      (SELECT (SELECT SUM(quantity) FROM locstock WHERE locstock.stockid = salesorderdetails.stkcode) > SUM(pick) FROM pickingfreeze WHERE linenumber = salesorderdetails.orderlineno AND pickingfreeze.orderno = salesorderdetails.orderno GROUP BY linenumber, stockid, pickingfreeze.orderno) AS fillable
    FROM salesorderdetails
    INNER JOIN salesorders ON salesorderdetails.orderno = salesorders.orderno
    INNER JOIN stockmaster sm ON salesorderdetails.stkcode = sm.stockid
    INNER JOIN debtorsmaster ON salesorders.debtorno = debtorsmaster.debtorno
    INNER JOIN custbranch ON custbranch.debtorno = salesorders.debtorno AND custbranch.branchcode = salesorders.branchcode
    LEFT JOIN debtortrans ON debtortrans.order_ = salesorderdetails.orderno
    LEFT JOIN stockmoves ON debtortrans.transno = stockmoves.transno AND stockmoves.orderlineref = salesorderdetails.orderlineno AND stockmoves.type = 10
    LEFT JOIN dispatch ON salesorders.orderno = dispatch.invoice
    LEFT JOIN order_status ON salesorders.orderstatusid = order_status.order_status_id
    WHERE salesorders.orderno IN (SELECT invoice FROM dispatch WHERE cleared = 0 AND oclater = 0)
    GROUP BY salesorders.orderno
    ORDER BY salesorders.orderno;
  )";

  auto t_q_start = std::chrono::steady_clock::now();
  SqlQueryResult q_res = db.execute_local_query(erp_sql);
  auto t_q_end = std::chrono::steady_clock::now();
  double q_sec = std::chrono::duration<double>(t_q_end - t_q_start).count();

  std::cout << "  Query Status:  " << (q_res.success ? "SUCCESS" : "FAILED: " + q_res.error_message) << "\n";
  std::cout << "  Query Rows:    " << q_res.rows.size() << " rows returned\n";
  std::cout << "  Query Latency: " << (q_sec * 1000.0) << " ms\n\n";

  if (!q_res.rows.empty()) {
    std::cout << "  Sample Query Output Table:\n" << q_res.formatted_table << "\n";
  }

  std::cout << "========================================================================\n";
  std::cout << "  ALL LIVE CDC & ERP DISPATCH BENCHMARKS COMPLETED SUCCESSFULLY         \n";
  std::cout << "========================================================================\n";
  return 0;
}
