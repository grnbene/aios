// Write cost of the engine alone - no socket, no client: sysbench's write_only shapes run through the call the
// MySQL wire server makes for a statement (execute_gateway_query), one session, against a fresh table of `rows`
// rows. Each phase prints microseconds per statement; a transaction phase counts BEGIN .. COMMIT as one.
//
//   aios-asql-write-bench <root-dir> [rows=100000] [statements=20000] [seed=1]
//
// Phases (the same random keys for every run with the same seed, so two builds compare like for like):
//   update_index      UPDATE sbtest SET k = k + 1 WHERE id = ?
//   update_non_index  UPDATE sbtest SET c = '<120 chars>' WHERE id = ?
//   delete_insert     DELETE FROM sbtest WHERE id = ?; INSERT INTO sbtest VALUES (?, ?, '<c>', '<pad>')
//   insert            INSERT INTO sbtest VALUES (<new id>, ...)
//   write_txn         BEGIN; 2 updates; delete; insert; COMMIT  (sysbench write_only's transaction)
// The root directory is the store: start from an empty one for a clean number (the table is created there).
#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <vector>

using namespace aios;
using namespace aios::userland;

namespace {

std::string chars(std::mt19937& rng, size_t n) {
  static const char* digits = "0123456789";
  std::string s;
  s.reserve(n);
  for (size_t i = 0; i < n; ++i) s.push_back((i % 12 == 11) ? '-' : digits[rng() % 10]);
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <root-dir> [rows=100000] [statements=20000] [seed=1]\n", argv[0]);
    return 2;
  }
  const long rows = argc > 2 ? std::atol(argv[2]) : 100000;
  const long n = argc > 3 ? std::atol(argv[3]) : 20000;
  const unsigned seed = argc > 4 ? (unsigned)std::atol(argv[4]) : 1u;
  KernelConfig cfg;
  cfg.root = argv[1];
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::fprintf(stderr, "boot: %s\n", k.error().message.c_str()); return 1; }
  SqlDatabase db(**k);
  if (!db.init("sql_db").ok()) { std::fprintf(stderr, "init failed\n"); return 1; }
  sql_set_wire_results(true);
  const std::string dbn = "bench";

  auto run = [&](const std::string& q) {
    auto r = db.execute_gateway_query(q, dbn);
    if (!r.success) { std::fprintf(stderr, "failed: %s: %s\n", q.substr(0, 120).c_str(), r.error_message.c_str()); std::exit(1); }
    return r;
  };

  run("CREATE DATABASE IF NOT EXISTS bench");
  run("DROP TABLE IF EXISTS sbtest");
  run("CREATE TABLE sbtest (id INT NOT NULL PRIMARY KEY, k INT NOT NULL DEFAULT 0, c CHAR(120) NOT NULL DEFAULT '', "
      "pad CHAR(60) NOT NULL DEFAULT '', KEY k_1 (k))");
  std::mt19937 rng(seed);
  {
    const long batch = 1000;
    for (long at = 1; at <= rows; at += batch) {
      std::string q = "INSERT INTO sbtest VALUES ";
      for (long id = at; id < at + batch && id <= rows; ++id) {
        if (id != at) q += ",";
        q += "(" + std::to_string(id) + "," + std::to_string(rng() % rows) + ",'" + chars(rng, 119) + "','" + chars(rng, 59) + "')";
      }
      run(q);
    }
  }
  std::printf("loaded %ld rows; %ld statements a phase, seed %u\n", rows, n, seed);

  // The keys and texts each phase uses, made before its clock starts.
  std::vector<long> keys((size_t)n);
  for (auto& key : keys) key = 1 + (long)(rng() % (unsigned)rows);
  std::vector<std::string> cs((size_t)n), pads((size_t)n);
  for (long i = 0; i < n; ++i) { cs[(size_t)i] = chars(rng, 119); pads[(size_t)i] = chars(rng, 59); }

  auto phase = [&](const char* name, long count, const std::function<void(long)>& body) {
    for (long i = 0; i < std::min(count, 200L); ++i) body(i);   // warm-up: plans, caches
    const auto t0 = std::chrono::steady_clock::now();
    for (long i = 0; i < count; ++i) body(i);
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / (double)count;
    std::printf("%-17s %9.2f us/statement\n", name, us);
  };

  phase("update_index", n, [&](long i) { run("UPDATE sbtest SET k = k + 1 WHERE id = " + std::to_string(keys[(size_t)i])); });
  phase("update_non_index", n, [&](long i) {
    run("UPDATE sbtest SET c = '" + cs[(size_t)i] + "' WHERE id = " + std::to_string(keys[(size_t)i]));
  });
  phase("delete_insert", n, [&](long i) {
    const std::string id = std::to_string(keys[(size_t)i]);
    run("DELETE FROM sbtest WHERE id = " + id);
    run("INSERT INTO sbtest VALUES (" + id + "," + std::to_string(i % rows) + ",'" + cs[(size_t)i] + "','" + pads[(size_t)i] + "')");
  });
  long next_id = rows + 1;
  phase("insert", n, [&](long i) {
    run("INSERT INTO sbtest VALUES (" + std::to_string(next_id++) + "," + std::to_string(i % rows) + ",'" + cs[(size_t)i] + "','" +
        pads[(size_t)i] + "')");
  });
  phase("write_txn", n / 4, [&](long i) {
    const long a = keys[(size_t)i], b = keys[(size_t)((i * 7 + 3) % n)], d = keys[(size_t)((i * 13 + 5) % n)];
    run("BEGIN");
    run("UPDATE sbtest SET k = k + 1 WHERE id = " + std::to_string(a));
    run("UPDATE sbtest SET c = '" + cs[(size_t)i] + "' WHERE id = " + std::to_string(b));
    run("DELETE FROM sbtest WHERE id = " + std::to_string(d));
    run("INSERT INTO sbtest VALUES (" + std::to_string(d) + "," + std::to_string(i % rows) + ",'" + cs[(size_t)i] + "','" +
        pads[(size_t)i] + "')");
    run("COMMIT");
  });
  auto cnt = run("SELECT COUNT(*) FROM sbtest");
  std::printf("rows at the end: %s (want %ld)\n", cnt.rows.empty() || cnt.rows[0].values.empty() ? "?" : cnt.rows[0].values[0].c_str(),
              rows + n + std::min(n, 200L));
  return 0;
}
