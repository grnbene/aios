#undef NDEBUG
// Loads a mysqldump extract into asql and runs a query against it, printing the
// result as TSV so it can be diffed byte-for-byte against what MariaDB returned
// from the same rows. Usage:
//   aios-asql-erpdiff-check <dir-with-x_*.sql> <query.sql>
#include <cassert>
#include <chrono>
#include <dirent.h>
#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <unistd.h>

#include "aios/kernel/kernel.hpp"
#include "aios/storage/aql.hpp"
#include "aios/userland/sql_database.hpp"
#include "aios/userland/task_ledger.hpp"

using namespace aios;
using namespace aios::userland;

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: " << argv[0] << " <dump-dir> <query.sql>\n";
    return 2;
  }
  const std::string dir = argv[1];
  const std::string query_path = argv[2];

  char root_dir[] = "/tmp/aios-erpdiff-XXXXXX";
  if (!mkdtemp(root_dir)) {
    std::perror("mkdtemp");
    return 1;
  }

  KernelConfig cfg;
  cfg.root = root_dir;
  cfg.residency = test_config;
  auto k_res = Kernel::boot(cfg);
  if (!k_res.ok()) {
    std::cerr << "boot failed: " << k_res.error().message << "\n";
    return 1;
  }
  Kernel& kernel = *k_res.value();
  TaskLedger ledger(kernel);
  ledger.init("tasks");
  SqlDatabase db(kernel, &ledger);
  db.init("sql_db");

  // Load every x_*.sql in the directory, in name order.
  std::vector<std::string> files;
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) {
      std::string n = e->d_name;
      if (n.rfind("x_", 0) == 0 && n.size() > 4 && n.substr(n.size() - 4) == ".sql")
        files.push_back(n);
    }
    closedir(d);
  }
  std::sort(files.begin(), files.end());
  if (files.empty()) {
    std::cerr << "no x_*.sql files in " << dir << "\n";
    return 1;
  }
  auto t_load0 = std::chrono::steady_clock::now();
  for (const std::string& f : files) {
    SqlQueryResult r = db.import_sql_file(dir + "/" + f);
    std::cerr << "  load " << f << ": " << (r.success ? "ok" : "FAILED: " + r.error_message) << "\n";
    if (!r.success) return 1;
  }
  auto t_load1 = std::chrono::steady_clock::now();
  std::cerr << "LOAD_SECONDS="
            << std::chrono::duration<double>(t_load1 - t_load0).count() << "\n";

  std::ifstream qf(query_path);
  if (!qf) {
    std::cerr << "cannot open " << query_path << "\n";
    return 1;
  }
  std::stringstream ss;
  ss << qf.rdbuf();
  std::string sql = ss.str();
  while (!sql.empty() && (sql.back() == '\n' || sql.back() == ';' || sql.back() == ' ')) sql.pop_back();

  int runs = 1;
  if (const char* rv = getenv("ASQL_RUNS")) runs = std::max(1, atoi(rv));
  SqlQueryResult r;
  for (int i = 0; i < runs; ++i) {
    auto t0 = std::chrono::steady_clock::now();
    r = db.execute_query(sql);
    auto t1 = std::chrono::steady_clock::now();
    std::cerr << "QUERY_SECONDS_run" << (i + 1) << "="
              << std::chrono::duration<double>(t1 - t0).count() << "\n";
    if (!r.success) {
      std::cerr << "QUERY FAILED: " << r.error_message << "\n";
      return 1;
    }
  }

  for (size_t i = 0; i < r.columns.size(); ++i)
    std::cout << (i ? "\t" : "") << r.columns[i];
  std::cout << "\n";
  for (const SqlRow& row : r.rows) {
    for (size_t i = 0; i < row.values.size(); ++i) {
      // MariaDB's batch mode prints NULL for a null cell; match it so the two
      // outputs can be compared without a translation step in between.
      std::cout << (i ? "\t" : "")
                << (sql_is_null(row.values[i]) ? "NULL" : row.values[i]);
    }
    std::cout << "\n";
  }
  std::cerr << "rows=" << r.rows.size() << "\n";
  return 0;
}
