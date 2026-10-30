// Replication must not stall readers. The binlog consumer applies each row event with execute_local_query, which
// holds the exclusive statement lock for the whole statement; the first insert into a big table whose key index is
// not in memory used to walk every row of the table to build it - 12.5M rows of stockmoves was 19 s - and every
// read of every table waited behind it (a counter POS order read ASQL for rows MariaDB had just committed).
//
// A 2M-row table with a primary key; FLUSH MEMORY drops its row index (what a restart does); a thread applies the
// next replicated rows the way the consumer does while another reads an unrelated table and times each read.
// The longest read must stay short, and the apply must not walk the table.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <unistd.h>

#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"

using namespace aios;
using namespace aios::userland;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++failures;
}
bool run(SqlDatabase& db, const std::string& sql) {
  SqlQueryResult r = db.execute_local_query(sql);
  if (!r.success) std::fprintf(stderr, "failed: %s\n  %s\n", sql.substr(0, 120).c_str(), r.error_message.c_str());
  return r.success;
}
uint64_t status(SqlDatabase& db, const char* name) {
  SqlQueryResult r = db.execute_local_query(std::string("SHOW STATUS LIKE '") + name + "'");
  const size_t p = r.formatted_table.find(name);
  if (!r.success || p == std::string::npos) return UINT64_MAX;
  const size_t bar = r.formatted_table.find('|', p);
  return bar == std::string::npos ? UINT64_MAX : std::strtoull(r.formatted_table.c_str() + bar + 1, nullptr, 10);
}
double ms_since(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}
}  // namespace

int main() {
  const std::filesystem::path root = std::filesystem::temp_directory_path() / ("cdc-stall-" + std::to_string(::getpid()));
  std::filesystem::create_directories(root);
  KernelConfig cfg;
  cfg.root = root.string();
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::fprintf(stderr, "boot failed: %s\n", k.error().message.c_str()); return 1; }
  SqlDatabase db(**k);

  const size_t kRows = 2000000;
  if (!run(db, "CREATE TABLE big (stkmoveno INT NOT NULL AUTO_INCREMENT PRIMARY KEY, stockid VARCHAR(20), qty DECIMAL(12,3))")) return 1;
  for (size_t from = 0; from < kRows; from += 5000) {
    std::string sql = "INSERT INTO big VALUES ";
    for (size_t i = from; i < from + 5000; ++i) sql += (i == from ? "(" : ",(") + std::to_string(i + 1) + ",'S" + std::to_string(i % 500) + "',1.000)";
    if (!run(db, sql)) return 1;
  }
  if (!run(db, "CREATE TABLE small (id INT NOT NULL PRIMARY KEY, v VARCHAR(10))") || !run(db, "INSERT INTO small VALUES (1,'a'),(2,'b')")) return 1;
  std::printf("filled %zu rows\n", kRows);

  // A restart: the row indices are gone, the tables are loaded.
  if (!run(db, "FLUSH MEMORY")) return 1;
  // FLUSH MEMORY also drops the column indices a restart warms again before serving ("warmed .. primary-key
  // indexes"); one keyed read puts this table's back, so what is left to measure is the row index alone.
  if (!run(db, "SELECT qty FROM big WHERE stkmoveno = 1000")) return 1;
  const uint64_t walked_before = status(db, "Asql_row_index_rows_indexed");

  std::atomic<bool> stop{false};
  std::atomic<double> worst_read_ms{0};
  std::atomic<uint64_t> reads{0};
  std::thread reader([&] {
    while (!stop.load()) {
      const auto t = std::chrono::steady_clock::now();
      SqlQueryResult r = db.execute_local_query("SELECT v FROM small WHERE id = 1");
      const double ms = ms_since(t);
      if (r.success) {
        reads++;
        double w = worst_read_ms.load();
        while (ms > w && !worst_read_ms.compare_exchange_weak(w, ms)) {}
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });

  // The consumer's insert for the next order's rows, as flush_pending_inserts builds it.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  sql_set_cdc_thread(true);
  const auto t0 = std::chrono::steady_clock::now();
  const bool applied = run(db, "INSERT INTO big (stkmoveno, stockid, qty) VALUES (" + std::to_string(kRows + 1) +
                                   ", 'S1', 1.000) ON DUPLICATE KEY UPDATE stockid = VALUES(stockid), qty = VALUES(qty)");
  const double apply_ms = ms_since(t0);
  sql_set_cdc_thread(false);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  stop = true;
  reader.join();

  const uint64_t walked = status(db, "Asql_row_index_rows_indexed") - walked_before;
  std::printf("the first replicated insert after the index was dropped took %.0f ms and walked %llu rows; the reader's worst read of another table: %.0f ms over %llu reads\n",
              apply_ms, (unsigned long long)walked, worst_read_ms.load(), (unsigned long long)reads.load());
  check(applied, "the replicated insert applies");
  check(worst_read_ms.load() < 100, "a read of an unrelated table never waits more than 100 ms behind the apply");
  check(walked < 100000, "the apply does not walk the whole table to build its key index");

  // While the key map is still partial the duplicate check must be as exact as with a whole one.
  auto cell = [&](const std::string& sql) {
    SqlQueryResult r = db.execute_local_query(sql);
    return r.success && !r.rows.empty() && !r.rows[0].values.empty() ? r.rows[0].values[0] : std::string("<none>");
  };
  check(run(db, "INSERT INTO big (stkmoveno, stockid, qty) VALUES (5, 'UPSERT', 9.000) ON DUPLICATE KEY UPDATE qty = VALUES(qty)"),
        "an upsert on an existing key applies while the key map is partial");
  check(cell("SELECT COUNT(*) FROM big") == std::to_string(kRows + 1), "... and updated that row instead of adding one");
  check(cell("SELECT qty FROM big WHERE stkmoveno = 5") == "9.000", "... and the row carries the updated value");
  check(cell("SELECT stockid FROM big WHERE stkmoveno = 5") == "S4", "... and keeps the column the upsert did not name");
  check(!db.execute_local_query("INSERT INTO big (stkmoveno, stockid, qty) VALUES (6, 'DUP', 1.000)").success,
        "a plain duplicate key is refused while the key map is partial");

  // Keep applying, as replication does: every apply stays short and the map fills in.
  const uint64_t walked_mid = status(db, "Asql_row_index_rows_indexed");
  double worst_apply_ms = 0;
  sql_set_cdc_thread(true);
  for (size_t n = 2; n < 150; ++n) {
    const auto t = std::chrono::steady_clock::now();
    if (!run(db, "INSERT INTO big (stkmoveno, stockid, qty) VALUES (" + std::to_string(kRows + n) +
                     ", 'S2', 1.000) ON DUPLICATE KEY UPDATE stockid = VALUES(stockid), qty = VALUES(qty)")) return 1;
    worst_apply_ms = std::max(worst_apply_ms, ms_since(t));
  }
  sql_set_cdc_thread(false);
  const uint64_t walked_total = status(db, "Asql_row_index_rows_indexed") - walked_before;
  std::printf("149 more applies: slowest %.0f ms; the key map has walked %llu of %zu rows\n", worst_apply_ms,
              (unsigned long long)walked_total, kRows);
  check(worst_apply_ms < 100, "no apply while the map fills in holds the lock for 100 ms");
  check(walked_total >= kRows, "the key map becomes whole through the applies");
  (void)walked_mid;
  check(!db.execute_local_query("INSERT INTO big (stkmoveno, stockid, qty) VALUES (7, 'DUP', 1.000)").success,
        "a plain duplicate key is refused once the map is whole");
  check(cell("SELECT COUNT(*) FROM big") == std::to_string(kRows + 149), "no row was added or lost along the way");

  std::filesystem::remove_all(root);
  std::printf("%s\n", failures ? "FAILED" : "all passed");
  return failures ? 1 : 0;
}
