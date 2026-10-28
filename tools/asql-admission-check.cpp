// Checks admission over the MySQL wire, the way the ERP's clients meet it:
//  - max_connections: connections past the limit get error 1040 and are
//    closed; one closing makes room for the next.
//  - query memory: eight sessions sending a large report at once, with a
//    budget one report's intermediates fill, run it one after another (the
//    rest wait) and all get the right answer; the reservations never pass
//    the budget by more than the one statement allowed over it; a point
//    lookup meanwhile reserves nothing and never waits; KILL reaches a
//    waiting statement; a statement bigger than the whole budget still runs.
//  - SET GLOBAL / SET SESSION and @@ of both variables.
#undef NDEBUG
#include <filesystem>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "aios/kernel/kernel.hpp"
#include "aios/userland/mysql_wire.hpp"
#include "aios/userland/sql_database.hpp"

using namespace aios;
using namespace aios::userland;

namespace {

int failures = 0;
#define CHECK(cond, ...)                          \
  do {                                            \
    if (!(cond)) {                                \
      ++failures;                                 \
      std::printf("  [FAIL] " __VA_ARGS__);       \
      std::printf("\n");                          \
    }                                             \
  } while (0)

void exec(SqlDatabase& db, const std::string& sql) {
  SqlQueryResult r = db.execute_local_query(sql);
  if (!r.success) { std::printf("  statement failed: %s\n  %s\n", sql.substr(0, 120).c_str(), r.error_message.c_str()); std::exit(1); }
}

std::string first_cell(const Result<MariaResult>& r) {
  if (!r.ok()) return "<io error: " + r.error().message + ">";
  if (!r->ok) return "<error: " + r->error_message + ">";
  if (r->rows.empty() || r->rows[0].empty()) return "<none>";
  return r->rows[0][0].is_null ? "NULL" : r->rows[0][0].bytes;
}

std::string all_cells(const Result<MariaResult>& r) {
  if (!r.ok() || !r->ok) return first_cell(r);
  std::string out;
  for (const auto& row : r->rows) {
    for (const auto& v : row) { out += v.is_null ? "NULL" : v.bytes; out += '\t'; }
    out += '\n';
  }
  return out;
}

double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

int main() {
  std::printf("=== ASQL admission check (max_connections, query memory) ===\n");
  const std::string root = "/tmp/aios-asql-admission";
  { std::error_code fs_err; std::filesystem::remove_all(root, fs_err); }
  KernelConfig cfg;
  cfg.root = root;
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::printf("boot failed\n"); return 1; }
  setenv("AIOS_SQL_CACHE_MB", "0", 1);   // every report really runs: none answered from the result cache
  SqlDatabase db(**k);
  db.init("sql_db");
  exec(db, "CREATE DATABASE IF NOT EXISTS admdb");
  exec(db, "USE admdb");
  // A large table (the reports' input) and a small one.
  exec(db, "CREATE TABLE moves (k INT NOT NULL PRIMARY KEY, g INT, item VARCHAR(16), qty DOUBLE)");
  const int kRows = 400000;
  std::string batch;
  for (int i = 0; i < kRows; ++i) {
    batch += (batch.empty() ? "" : ",") + std::string("(") + std::to_string(i) + "," + std::to_string(i % 60000) + ",'IT" +
             std::to_string(i % 9000) + "'," + std::to_string((i % 97) - 40) + ")";
    if (batch.size() > 200000 || i + 1 == kRows) { exec(db, "INSERT INTO moves VALUES " + batch); batch.clear(); }
  }
  exec(db, "CREATE TABLE locs (code VARCHAR(8) NOT NULL PRIMARY KEY, name VARCHAR(32))");
  exec(db, "INSERT INTO locs VALUES ('MAIN','Main'),('WEST','West'),('EAST','East')");
  exec(db, "CREATE VIEW move_totals AS SELECT item, SUM(qty) AS q FROM moves GROUP BY item");
  db.optimize_all_tables("admdb");

  MysqlWireServer server(**k, db, 0, "127.0.0.1");
  if (!server.start().ok()) { std::printf("server failed to start\n"); return 1; }
  const uint16_t port = server.port();
  server.start_background();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // --- max_connections ----------------------------------------------------------
  {
    server.set_max_connections(4);
    std::vector<std::unique_ptr<MariaWireSession>> open;
    for (int i = 0; i < 4; ++i) {
      open.push_back(std::make_unique<MariaWireSession>("127.0.0.1", port));
      auto r = open.back()->connect("root", "", "admdb");
      CHECK(r.ok(), "connection %d of 4 refused: %s", i + 1, r.ok() ? "" : r.error().message.c_str());
    }
    MariaWireSession extra("127.0.0.1", port);
    auto r5 = extra.connect("root", "", "admdb");
    CHECK(!r5.ok(), "a fifth connection was let in past max_connections=4");
    if (!r5.ok()) {
      const std::string& m = r5.error().message;
      CHECK(m.find("1040") != std::string::npos || m.find("Too many connections") != std::string::npos,
            "the refusal did not say why: %s", m.c_str());
    }
    open.back()->close();
    open.pop_back();
    bool admitted = false;
    for (int attempt = 0; attempt < 50 && !admitted; ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      MariaWireSession again("127.0.0.1", port);
      admitted = again.connect("root", "", "admdb").ok();
    }
    CHECK(admitted, "no room was made when a connection closed");
    CHECK(db.metrics().rejected_connections.load() >= 1, "the refusal was not counted");
    open.clear();
    server.set_max_connections(151);
    std::printf("  [ok] max_connections: the fifth of 4 refused with 1040, admitted once one closed\n");
  }

  // --- SET / @@ ------------------------------------------------------------------
  {
    MariaWireSession s("127.0.0.1", port);
    CHECK(s.connect("root", "", "admdb").ok(), "connect");
    CHECK(first_cell(s.query("SELECT @@max_connections")) == "151", "@@max_connections");
    CHECK(first_cell(s.query("SELECT @@asql_query_memory_mb")) == "2048", "@@asql_query_memory_mb default");
    auto g = s.query("SET GLOBAL asql_query_memory_mb = 64");
    CHECK(g.ok() && g->ok, "SET GLOBAL asql_query_memory_mb failed");
    CHECK(first_cell(s.query("SELECT @@asql_query_memory_mb")) == "64", "@@asql_query_memory_mb after SET GLOBAL");
    CHECK(db.query_memory_budget() == ((size_t)64 << 20), "the budget was not set");
    auto gs = s.query("SET SESSION asql_query_memory_mb = 5");
    CHECK(gs.ok() && !gs->ok, "SET SESSION asql_query_memory_mb was accepted");
    auto sess = s.query("SET SESSION max_connections = 5");
    CHECK(sess.ok() && !sess->ok && sess->error_message.find("GLOBAL variable") != std::string::npos,
          "SET SESSION max_connections should be error 1229, got %s", sess.ok() ? sess->error_message.c_str() : "io error");
    auto bad = s.query("SET GLOBAL max_connections = 0");
    CHECK(bad.ok() && !bad->ok, "SET GLOBAL max_connections = 0 was accepted");
    CHECK(first_cell(s.query("SELECT @@max_connections")) == "151", "max_connections changed by a refused SET");
    std::printf("  [ok] SET GLOBAL / SET SESSION / @@ for both variables\n");
  }

  // --- eight reports at once, one budget ------------------------------------------
  const std::string report =
      "SELECT m.item, SUM(m.qty), COUNT(*) FROM moves m JOIN moves n ON n.k = m.g GROUP BY m.item ORDER BY 2 DESC, 1";
  size_t one = 0;   // what one report reserves
  std::string want;
  {
    db.set_query_memory_budget(0);
    MariaWireSession s("127.0.0.1", port);
    s.set_read_timeout_ms(120000);
    s.connect("root", "", "admdb");
    const size_t before = db.query_memory_peak();
    want = all_cells(s.query(report));
    one = db.query_memory_peak() - before;
    CHECK(db.query_memory_reserved() == 0, "%zu bytes still reserved after the statement ended", db.query_memory_reserved());
    CHECK(want.size() > 100, "the report answered nothing: %s", want.substr(0, 80).c_str());
    CHECK(one >= ((size_t)8 << 20), "one report reserved only %zu bytes", one);
    std::printf("  [ok] one report reserves %.1f MB\n", one / 1048576.0);
  }
  {
    const size_t budget = one + one / 2;   // one fits, two do not
    db.set_query_memory_budget(budget);
    std::atomic<bool> running{true};
    std::atomic<size_t> max_reserved{0}, max_waiting{0};
    std::thread watch([&] {
      while (running.load()) {
        const size_t r = db.query_memory_reserved(), w = db.query_memory_waiting();
        if (r > max_reserved.load()) max_reserved.store(r);
        if (w > max_waiting.load()) max_waiting.store(w);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
      }
    });
    std::atomic<double> worst_lookup_ms{0};
    std::atomic<int> lookups{0};
    const uint64_t waits_before = db.metrics().query_memory_waits.load();
    std::thread lookup([&] {
      MariaWireSession s("127.0.0.1", port);
      s.connect("root", "", "admdb");
      while (running.load()) {
        const auto t0 = std::chrono::steady_clock::now();
        const std::string v = first_cell(s.query("SELECT qty FROM moves WHERE k = 31337"));
        const double ms = ms_since(t0);
        if (v != std::to_string((31337 % 97) - 40)) { std::printf("  [FAIL] point lookup answered %s\n", v.c_str()); ++failures; }
        if (ms > worst_lookup_ms.load()) worst_lookup_ms.store(ms);
        lookups++;
      }
    });
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> clients;
    std::vector<std::string> got(8);
    for (int i = 0; i < 8; ++i)
      clients.emplace_back([&, i] {
        MariaWireSession s("127.0.0.1", port);
        s.set_read_timeout_ms(120000);
        s.connect("root", "", "admdb");
        got[i] = all_cells(s.query(report));
      });
    for (auto& c : clients) c.join();
    const double wall = ms_since(t0);
    running = false;
    watch.join();
    lookup.join();
    for (int i = 0; i < 8; ++i) CHECK(got[i] == want, "report %d answered differently", i);
    // The one statement allowed past the budget is the oldest holder: at most
    // one report's worth over it.
    CHECK(max_reserved.load() <= budget + one, "%.1f MB reserved at once against a %.1f MB budget", max_reserved.load() / 1048576.0,
          budget / 1048576.0);
    CHECK(max_waiting.load() >= 1, "no report ever waited (the budget did nothing)");
    CHECK(db.metrics().query_memory_waits.load() > waits_before, "the waits were not counted");
    CHECK(db.query_memory_reserved() == 0 && db.query_memory_waiting() == 0, "left reserved: %zu bytes, %zu waiting",
          db.query_memory_reserved(), db.query_memory_waiting());
    CHECK(lookups.load() > 0, "no point lookup ran");
    std::printf("  [ok] 8 reports on a %.1f MB budget: at most %.1f MB reserved at once, up to %zu waiting, all answers right, "
                "%.0f ms wall; %d point lookups alongside, the slowest %.1f ms\n",
                budget / 1048576.0, max_reserved.load() / 1048576.0, max_waiting.load(), wall, lookups.load(), worst_lookup_ms.load());
  }

  // --- a statement over the whole budget runs alone; KILL reaches a waiting one ---------
  {
    db.set_query_memory_budget(one / 2);
    MariaWireSession first("127.0.0.1", port), second("127.0.0.1", port), admin("127.0.0.1", port);
    first.set_read_timeout_ms(120000);
    second.set_read_timeout_ms(120000);
    first.connect("root", "", "admdb");
    second.connect("root", "", "admdb");
    admin.connect("root", "", "admdb");
    // The first reserves its whole projection (400k rows) up front and then
    // works through it - a SHA2 per row, a sort - so it holds more than the
    // whole budget for a while; the second is sent once it does, and cannot fit.
    const std::string slow = "SELECT k, SHA2(CONCAT(item, '-', k), 256) AS h FROM moves ORDER BY h";
    std::string first_answer, second_answer;
    std::thread t1([&] { first_answer = all_cells(first.query(slow)); });
    for (int i = 0; i < 5000 && db.query_memory_reserved() <= db.query_memory_budget(); ++i)
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    std::thread t2([&] {
      auto r = second.query(report);
      second_answer = r.ok() ? (r->ok ? std::string("answered") : r->error_message) : r.error().message;
    });
    for (int i = 0; i < 5000 && db.query_memory_waiting() == 0; ++i) std::this_thread::sleep_for(std::chrono::microseconds(200));
    const bool second_waited = db.query_memory_waiting() == 1;
    if (second_waited) {
      auto k2 = admin.query("KILL QUERY " + std::to_string(second.thread_id()));
      CHECK(k2.ok() && k2->ok, "KILL QUERY failed: %s", k2.ok() ? k2->error_message.c_str() : "io error");
    }
    t2.join();
    t1.join();
    CHECK(second_waited, "the second report did not wait behind the first");
    CHECK(second_answer.find("interrupted") != std::string::npos, "the waiting report was not interrupted: %s", second_answer.c_str());
    CHECK(first_answer.size() > 100, "the report over the whole budget did not run alone: %s", first_answer.substr(0, 80).c_str());
    CHECK(db.query_memory_reserved() == 0 && db.query_memory_waiting() == 0, "left reserved after KILL");
    std::printf("  [ok] a report over the whole budget ran alone; KILL QUERY reached the one waiting (%s)\n", second_answer.c_str());
  }

  server.stop();
  if (failures) { std::printf("=== %d FAILURE(S) ===\n", failures); return 1; }
  std::printf("=== ALL ADMISSION CHECKS PASSED ===\n");
  return 0;
}
