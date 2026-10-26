// A replicated stream that writes to several big tables must not rebuild a table's primary-key row index for
// every statement. The row indices share a memory budget; when the tables a stream touches do not all fit, the
// least recently used index used to be dropped to make room and rebuilt, whole, by the next insert into that
// table (stockmoves 20 s, stockmovestaxes 11 s on a 12M / 20M row mirror).
//
// Three tables with a primary key, a budget too small for all three indices, and one insert into each table per
// round - what completing an order does. After a warm-up the engine's own counter of rows walked into a row index
// (Asql_row_index_rows_indexed) must grow by the inserted rows, not by the table sizes. Then a table nobody has
// touched for longer than the hot window must still be evicted when a new index needs the room: the budget holds.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

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
bool fill(SqlDatabase& db, const std::string& table, size_t rows) {
  if (!run(db, "CREATE TABLE " + table + " (id INT NOT NULL PRIMARY KEY, v VARCHAR(24))")) return false;
  for (size_t from = 0; from < rows; from += 2000) {
    std::string sql = "INSERT INTO " + table + " VALUES ";
    for (size_t i = from; i < from + 2000 && i < rows; ++i)
      sql += (i == from ? "(" : ",(") + std::to_string(i) + ",'row" + std::to_string(i) + "')";
    if (!run(db, sql)) return false;
  }
  return true;
}
}  // namespace

int main() {
  // Read once, at the first use: the budget is smaller than the three indices together (each is 4-8 MB at 100k
  // rows), and a table untouched for a second is cold.
  setenv("AIOS_SQL_ROWINDEX_MB", "6", 1);
  setenv("AIOS_SQL_ROWINDEX_HOT_S", "1", 1);

  const std::filesystem::path root = std::filesystem::temp_directory_path() / ("rowindex-churn-" + std::to_string(::getpid()));
  std::filesystem::create_directories(root);
  KernelConfig cfg;
  cfg.root = root.string();
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::fprintf(stderr, "boot failed: %s\n", k.error().message.c_str()); return 1; }
  SqlDatabase db(**k);

  const size_t kRows = 100000;
  for (const char* t : {"t1", "t2", "t3"})
    if (!fill(db, t, kRows)) return 1;

  auto order = [&](size_t n) {
    bool ok = true;
    for (const char* t : {"t1", "t2", "t3"})
      ok &= run(db, std::string("INSERT INTO ") + t + " VALUES (" + std::to_string(kRows + n) + ",'new')");
    return ok;
  };

  const size_t kWarm = 5, kRounds = 40;
  for (size_t n = 0; n < kWarm; ++n)
    if (!order(n)) return 1;
  const uint64_t before = status(db, "Asql_row_index_rows_indexed");
  const uint64_t evict_before = status(db, "Asql_row_index_evictions");
  for (size_t n = kWarm; n < kWarm + kRounds; ++n)
    if (!order(n)) return 1;
  const uint64_t walked = status(db, "Asql_row_index_rows_indexed") - before;
  const uint64_t evicted = status(db, "Asql_row_index_evictions") - evict_before;
  std::printf("after warm-up: %zu rounds x 3 inserts walked %llu rows into row indices, %llu evictions (tables hold %zu rows each)\n",
              kRounds, (unsigned long long)walked, (unsigned long long)evicted, kRows);
  check(walked < kRounds * 3 * 16, "inserts into tables that are all being written walk only the new rows into their row index");
  check(evicted == 0, "no index in use by the stream is evicted");

  // Correctness: the index the stream kept still answers for the key.
  SqlQueryResult dup = db.execute_local_query("INSERT INTO t2 VALUES (7,'again')");
  check(!dup.success && dup.error_message.find("uplicate") != std::string::npos, "a duplicate primary key is still refused");

  // The budget still holds: after the hot window, a new table's index makes room by dropping the cold ones.
  std::this_thread::sleep_for(std::chrono::milliseconds(2200));
  const uint64_t evict_mid = status(db, "Asql_row_index_evictions");
  if (!fill(db, "t4", kRows)) return 1;
  if (!run(db, "INSERT INTO t4 VALUES (" + std::to_string(kRows + 1) + ",'new')")) return 1;
  const uint64_t evicted_cold = status(db, "Asql_row_index_evictions") - evict_mid;
  std::printf("cold tables: %llu evictions\n", (unsigned long long)evicted_cold);
  check(evicted_cold >= 1, "an index nobody touched for longer than the hot window is evicted to make room");

  std::filesystem::remove_all(root);
  std::printf("%s\n", failures ? "FAILED" : "all passed");
  return failures ? 1 : 0;
}
