// Per-statement cost of the engine alone - no socket, no client: the exact
// call the MySQL wire server makes for a query (execute_gateway_query with
// wire results on), looped. Used to find where a one-row key lookup's time
// goes; run it under a sampler with `seconds` > 0 to hold the loop open.
//
//   aios-asql-stmt-bench <root-dir> [iterations=100000] [seconds=0] [extra_tables=0]
//   extra_tables: other (empty) tables in the database, as a real mirror has
//   aios-asql-stmt-bench <root-dir> <iterations> --db <database> <queries-file>
//     loads the store at root-dir (a COPY of a live one) and times each line of
//     queries-file; prints timings and row counts only.
#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"

#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace aios;
using namespace aios::userland;

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: %s <root-dir> [iterations] [seconds]\n", argv[0]); return 2; }
  const long iters = argc > 2 ? std::atol(argv[2]) : 100000;
  const double hold_s = argc > 3 ? std::atof(argv[3]) : 0.0;
  const int extra = argc > 4 ? std::atoi(argv[4]) : 0;
  const bool live = argc > 5 && std::string(argv[3]) == "--db";
  KernelConfig cfg;
  cfg.root = argv[1];
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::fprintf(stderr, "boot: %s\n", k.error().message.c_str()); return 1; }
  SqlDatabase db(**k);
  if (!db.init("sql_db").ok()) { std::fprintf(stderr, "init failed\n"); return 1; }
  if (live) {
    FILE* f = std::fopen(argv[5], "r");
    if (!f) { std::fprintf(stderr, "cannot read %s\n", argv[5]); return 1; }
    char line[8192];
    sql_set_wire_results(true);
    while (std::fgets(line, sizeof line, f)) {
      std::string q(line);
      while (!q.empty() && (q.back() == '\n' || q.back() == '\r')) q.pop_back();
      if (q.empty()) continue;
      auto check = db.execute_gateway_query(q, argv[4]);
      // AIOS_BENCH_UNIQUE=1: every execution's text differs by a trailing
      // comment, so none is answered from the result cache - the path an ERP
      // statement with new literal values takes, without SQL_NO_CACHE.
      static const bool unique = std::getenv("AIOS_BENCH_UNIQUE") != nullptr;
      std::vector<std::string> uq;
      if (unique) {
        uq.reserve((size_t)iters + 200);
        for (long i = 0; i < iters + 200; ++i) uq.push_back(q + " /* " + std::to_string(i) + " */");
      }
      // Key indexes are built in the background after boot; a number taken
      // while that runs is the scan's. AIOS_BENCH_SETTLE_S waits it out once.
      static bool settled = false;
      if (!settled) {
        settled = true;
        if (const char* e = std::getenv("AIOS_BENCH_SETTLE_S")) {
          const double sec = std::atof(e);
          if (sec > 0) std::this_thread::sleep_for(std::chrono::duration<double>(sec));
        }
      }
      for (int i = 0; i < 200; ++i) db.execute_gateway_query(unique ? uq[(size_t)i] : q, argv[4]);
      auto t0 = std::chrono::steady_clock::now();
      for (long i = 0; i < iters; ++i) db.execute_gateway_query(unique ? uq[(size_t)i + 200] : q, argv[4]);
      const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / (double)iters;
      std::printf("%9.2f us/statement  rows=%zu ok=%d  %.50s\n", us, check.rows.size(), (int)check.success, q.c_str());
    }
    std::fclose(f);
    // Optional 7th argument: keep running the file's first query this many
    // seconds, for a sampler to attach to.
    if (argc > 6) {
      FILE* g = std::fopen(argv[5], "r");
      std::string first;
      if (g && std::fgets(line, sizeof line, g)) first = line;
      if (g) std::fclose(g);
      while (!first.empty() && (first.back() == '\n' || first.back() == '\r')) first.pop_back();
      const double hold = std::atof(argv[6]);
      auto t0 = std::chrono::steady_clock::now();
      while (!first.empty() && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < hold)
        db.execute_gateway_query(first, argv[4]);
    }
    return 0;
  }
  db.execute_gateway_query("CREATE TABLE cfg (confname VARCHAR(40) PRIMARY KEY, confvalue VARCHAR(40))");
  std::string ins = "INSERT INTO cfg VALUES ";
  for (int i = 0; i < 300; ++i) ins += (i ? "," : "") + std::string("('Name") + std::to_string(i) + "','" + std::to_string(i) + "')";
  db.execute_gateway_query(ins);
  db.execute_gateway_query("CREATE TABLE loc (loccode VARCHAR(5) PRIMARY KEY, taxprovinceid INT, name VARCHAR(40))");
  ins = "INSERT INTO loc VALUES ";
  for (int i = 0; i < 100; ++i) ins += (i ? "," : "") + std::string("('") + (i < 10 ? "0" : "") + std::to_string(i) + "'," + std::to_string(i % 7) + ",'L')";
  db.execute_gateway_query(ins);
  for (int i = 0; i < extra; ++i)
    db.execute_gateway_query("CREATE TABLE filler_" + std::to_string(i) + " (id INT PRIMARY KEY, v VARCHAR(10))");

  const char* qs[][2] = {
    {"config by name", "SELECT SQL_NO_CACHE confvalue FROM cfg WHERE confname = 'Name7'"},
    {"location by key", "SELECT taxprovinceid FROM loc WHERE loccode='00'"},
    {"select 1", "SELECT 1"},
  };
  sql_set_wire_results(true);
  for (auto& q : qs) {
    auto check = db.execute_gateway_query(q[1]);
    for (int i = 0; i < 2000; ++i) db.execute_gateway_query(q[1]);
    auto t0 = std::chrono::steady_clock::now();
    for (long i = 0; i < iters; ++i) db.execute_gateway_query(q[1]);
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / (double)iters;
    std::printf("%-16s %7.2f us/statement  (rows=%zu ok=%d)\n", q[0], us, check.rows.size(), (int)check.success);
  }
  if (hold_s > 0) {
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < hold_s)
      db.execute_gateway_query(qs[0][1]);
  }
  return 0;
}
