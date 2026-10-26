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

  // What a replicated order does to a big table that stays hot: rows deleted by key and put back, rows updated.
  // None of it may drop the table's index (the next insert would walk every row of the table again).
  if (!fill(db, "d1", 120000)) return 1;
  if (!run(db, "INSERT INTO d1 VALUES (120000,'warm')")) return 1;
  const uint64_t d_before = status(db, "Asql_row_index_rows_indexed");
  bool d_ok = true;
  for (size_t n = 0; n < 20; ++n) {
    const std::string k = std::to_string(50000 + n * 3);
    d_ok &= run(db, "DELETE FROM d1 WHERE id = " + k);
    d_ok &= run(db, "INSERT INTO d1 VALUES (" + k + ",'again')");
    d_ok &= run(db, "UPDATE d1 SET v = 'upd' WHERE id = " + std::to_string(60000 + n));
    d_ok &= run(db, "INSERT INTO d1 VALUES (" + std::to_string(120001 + n) + ",'new')");
  }
  const uint64_t d_walked = status(db, "Asql_row_index_rows_indexed") - d_before;
  std::printf("delete/insert/update on a hot 120k-row table: 20 rounds walked %llu rows into its row index\n", (unsigned long long)d_walked);
  check(d_ok, "the deletes, inserts and updates succeed");
  check(d_walked < 20 * 4 * 16, "deleting, re-inserting and updating rows of a hot big table does not rebuild its row index");

  // The form the replication applies rows in: INSERT .. ON DUPLICATE KEY UPDATE, on a composite-key table
  // (stockmovestaxes) and on an auto-increment one (stockmoves), a transaction at a time.
  {
    if (!run(db, "CREATE TABLE c1 (stkmoveno INT NOT NULL, taxauthid INT NOT NULL, rate DECIMAL(10,4), PRIMARY KEY (stkmoveno, taxauthid))")) return 1;
    if (!run(db, "CREATE TABLE a1 (stkmoveno INT NOT NULL AUTO_INCREMENT PRIMARY KEY, stockid VARCHAR(20), qty DECIMAL(12,3))")) return 1;
    for (size_t from = 0; from < 120000; from += 2000) {
      std::string c = "INSERT INTO c1 VALUES ", a = "INSERT INTO a1 VALUES ";
      for (size_t i = from; i < from + 2000; ++i) {
        c += (i == from ? "(" : ",(") + std::to_string(i) + ",1,5.0000)";
        a += (i == from ? "(" : ",(") + std::to_string(i + 1) + ",'S" + std::to_string(i % 500) + "',1.000)";
      }
      if (!run(db, c) || !run(db, a)) return 1;
    }
    const uint64_t o_before = status(db, "Asql_row_index_rows_indexed");
    bool o_ok = true;
    for (size_t n = 0; n < 20; ++n) {
      o_ok &= run(db, "INSERT INTO c1 (stkmoveno, taxauthid, rate) VALUES (" + std::to_string(200000 + n) + ",1,5.0000) ON DUPLICATE KEY UPDATE rate = VALUES(rate)");
      o_ok &= run(db, "INSERT INTO a1 (stkmoveno, stockid, qty) VALUES (" + std::to_string(200000 + n) + ",'S1',1.000) ON DUPLICATE KEY UPDATE qty = VALUES(qty)");
      o_ok &= run(db, "UPDATE a1 SET qty = 2.000 WHERE stkmoveno = " + std::to_string(1000 + n));
      o_ok &= run(db, "DELETE FROM c1 WHERE stkmoveno = " + std::to_string(3000 + n) + " AND taxauthid = 1");
    }
    const uint64_t o_walked = status(db, "Asql_row_index_rows_indexed") - o_before;
    std::printf("ON DUPLICATE KEY UPDATE / UPDATE / DELETE on hot 120k-row tables: 20 rounds walked %llu rows into row indices\n", (unsigned long long)o_walked);
    check(o_ok, "the replicated-form statements succeed");
    check(o_walked < 20 * 4 * 16, "applying them does not rebuild a hot big table's row index");
  }

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
