// Checks the column conversions a background checkpoint leaves to upkeep_column (sql_database.cpp): a column of
// 65,536 rows or more that is due to be typed (here a DOUBLE column a write turned into dictionary codes - its
// scaled form could not take the value in place; asql-warm-check has INT columns a restart loads as codes) is
// converted off the exclusive lock and put in place - the same values, now typed - and a
// write that lands while it is being converted keeps the column as it is (the conversion is dropped, counted in
// Asql_upkeep_dropped) with every row right, until a later checkpoint converts it. FLUSH TABLES, which the caller
// waits for, still converts in place before it returns. The first write after a restart used to wait for all of
// these under the lock (1 s for 4 x 2M-row columns).
// And a restart's primary-key row map, built off the lock by the warm-up (warm_row_index_off_lock): keyed writes
// find their rows through it, and a write while it is being built makes it go unused, the write standing.
#undef NDEBUG
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"
#include "check-root.hpp"

using namespace aios;
using namespace aios::userland;

namespace {

int failures = 0;
int checks = 0;
void expect(bool ok, const std::string& what) {
  ++checks;
  if (!ok) { ++failures; std::printf("  [FAIL] %s\n", what.c_str()); }
  else std::printf("  [ok] %s\n", what.c_str());
}
SqlQueryResult run(SqlDatabase& db, const std::string& sql) {
  SqlQueryResult r = db.execute_local_query(sql);
  if (!r.success) { std::printf("  query failed: %s\n  %s\n", sql.c_str(), r.error_message.c_str()); std::exit(1); }
  return r;
}
std::string cell(SqlDatabase& db, const std::string& sql, size_t col = 0) {
  SqlQueryResult r = run(db, sql);
  return r.rows.empty() || col >= r.rows[0].values.size() ? std::string("?") : sql_display(r.rows[0].values[col]);
}
long long status(SqlDatabase& db, const std::string& name) {
  SqlQueryResult r = run(db, "SHOW GLOBAL STATUS LIKE '" + name + "'");
  return r.rows.empty() || r.rows[0].values.size() < 2 ? -1 : std::atoll(sql_display(r.rows[0].values[1]).c_str());
}
// The encoding of t.c, read with no conversion under way (upkeep_wait_idle) and no statement running.
ColumnEncoding encoding_of(SqlDatabase& db, const std::string& t, const std::string& c) {
  db.upkeep_wait_idle();
  const SqlTable* tb = db.find_table(t, "updb");
  if (!tb) return ColumnEncoding::Constant;
  const int ci = tb->get_column_index(c);
  return ci < 0 ? ColumnEncoding::Constant : tb->col_data[(size_t)ci].encoding;
}
bool typed(ColumnEncoding e) {
  return e == ColumnEncoding::TypedInt32 || e == ColumnEncoding::TypedInt64 || e == ColumnEncoding::FOR ||
         e == ColumnEncoding::SegmentFOR || e == ColumnEncoding::DecimalScaled;
}
// rows 1..n: v = id % 97, d = (id % 1000) / 100, by INSERT ... VALUES as replication writes them; then one d changed,
// which leaves d as dictionary codes due to be scaled again at the next checkpoint
void fill(SqlDatabase& db, const std::string& t, int n) {
  run(db, "CREATE TABLE " + t + " (id INT PRIMARY KEY, v INT, d DOUBLE)");
  for (int b = 1; b <= n; b += 2000) {
    std::string sql = "INSERT INTO " + t + " VALUES ";
    char x[96];
    for (int id = b; id < b + 2000 && id <= n; ++id) {
      std::snprintf(x, sizeof x, "%s(%d,%d,%d.%02d)", id == b ? "" : ",", id, id % 97, (id % 1000) / 100, (id % 1000) % 100);
      sql += x;
    }
    run(db, sql);
  }
  run(db, "UPDATE " + t + " SET d = 3.75 WHERE id = 5");
}
struct Figures { std::string count, sum_v, sum_d, max_v, at; };
Figures figures(SqlDatabase& db, const std::string& t) {
  return {cell(db, "SELECT COUNT(*) FROM " + t), cell(db, "SELECT SUM(v) FROM " + t), cell(db, "SELECT SUM(d) FROM " + t),
          cell(db, "SELECT MAX(v) FROM " + t), cell(db, "SELECT CONCAT(v, '/', d) FROM " + t + " WHERE id = 77777")};
}
bool same(const Figures& a, const Figures& b) { return a.count == b.count && a.sum_v == b.sum_v && a.sum_d == b.sum_d && a.max_v == b.max_v && a.at == b.at; }
std::string show(const Figures& f) { return f.count + " rows, SUM(v) " + f.sum_v + ", SUM(d) " + f.sum_d + ", MAX(v) " + f.max_v + ", row 77777 " + f.at; }

}  // namespace

int main() {
  std::printf("=== ASQL column conversions off the lock ===\n");
  setenv("AIOS_SQL_UPKEEP_DELAY_MS", "400", 1);   // the window the third part's write lands in
  const CheckRoot check_root("/tmp/aios-asql-upkeep");
  KernelConfig cfg;
  cfg.root = check_root.path;
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::printf("boot failed\n"); return 1; }
  const int n = 131072;
  {
  SqlDatabase db(**k);
  db.init("sql_db");
  run(db, "CREATE DATABASE IF NOT EXISTS updb");
  run(db, "USE updb");

  // 1. a background checkpoint: the columns are converted off the lock, the same values typed
  fill(db, "ua", n);
  const Figures a0 = figures(db, "ua");
  expect(!typed(encoding_of(db, "ua", "d")), "ua.d is dictionary codes after the write");
  const long long conv0 = status(db, "Asql_upkeep_converted");
  db.flush_off_lock();
  db.upkeep_wait_idle();
  expect(typed(encoding_of(db, "ua", "d")), "ua.d scaled off the lock");
  expect(status(db, "Asql_upkeep_converted") - conv0 == 1, "one conversion counted: " + std::to_string(status(db, "Asql_upkeep_converted") - conv0));
  const Figures a1 = figures(db, "ua");
  expect(same(a0, a1), "ua answers as before: " + show(a1) + " (before: " + show(a0) + ")");

  // 2. FLUSH TABLES, which its caller waits for, converts in place before it returns
  fill(db, "ub", n);
  const Figures b0 = figures(db, "ub");
  expect(!typed(encoding_of(db, "ub", "d")), "ub.d is dictionary codes after the write");
  const long long conv_b = status(db, "Asql_upkeep_converted");
  run(db, "FLUSH TABLES");
  const SqlTable* tb = db.find_table("ub", "updb");
  expect(tb && typed(tb->col_data[(size_t)tb->get_column_index("d")].encoding), "ub.d scaled when FLUSH TABLES returns");
  db.upkeep_wait_idle();
  expect(status(db, "Asql_upkeep_converted") == conv_b, "FLUSH TABLES left nothing to the worker");
  expect(same(b0, figures(db, "ub")), "ub answers as before after FLUSH TABLES");

  // 3. a write while a column is being converted: the conversion is dropped, the write and every row stand
  fill(db, "uc", n);
  const long long drop0 = status(db, "Asql_upkeep_dropped");
  db.flush_off_lock();   // queues uc.d (and ub.d, ud.d are not yet written); it waits 400 ms before it would be put in place
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  run(db, "UPDATE uc SET v = 1000, d = 9.5 WHERE id = 77777");
  run(db, "INSERT INTO uc VALUES (" + std::to_string(n + 1) + ", 5, 0.25)");
  db.upkeep_wait_idle();
  expect(status(db, "Asql_upkeep_dropped") - drop0 >= 1, "a conversion the write landed in was dropped: " + std::to_string(status(db, "Asql_upkeep_dropped") - drop0));
  const Figures c1 = figures(db, "uc");
  expect(c1.at == "1000/9.5" && c1.count == std::to_string(n + 1) && c1.max_v == "1000", "uc keeps the write: " + show(c1));
  // its sums against the same table built again without any conversion under way
  fill(db, "ud", n);
  run(db, "UPDATE ud SET v = 1000, d = 9.5 WHERE id = 77777");
  run(db, "INSERT INTO ud VALUES (" + std::to_string(n + 1) + ", 5, 0.25)");
  const Figures d1 = figures(db, "ud");
  expect(same(c1, d1), "uc answers as the same rows never converted do: " + show(c1) + " / " + show(d1));
  // a later checkpoint, with nothing written meanwhile, converts it
  expect(!typed(encoding_of(db, "uc", "d")), "uc.d still dictionary codes after the dropped conversion");
  db.flush_off_lock();
  db.upkeep_wait_idle();
  expect(typed(encoding_of(db, "uc", "d")), "uc.d scaled by the next checkpoint");
  expect(same(c1, figures(db, "uc")), "uc answers the same once typed");

  // 4. a table with a text key, given a row map by a keyed write, then a clean stop (the list of maps is saved)
  run(db, "CREATE TABLE ue (id VARCHAR(24) PRIMARY KEY, v INT)");
  for (int b = 0; b < 200000; b += 4000) {
    std::string sql = "INSERT INTO ue VALUES ";
    for (int i = b; i < b + 4000; ++i) sql += std::string(i == b ? "" : ",") + "('k" + std::to_string(i) + "',1)";
    run(db, sql);
  }
  expect(db.execute_local_query("UPDATE ue SET v = 2 WHERE id = 'k777'").rows_affected == 1, "ue: a keyed write before the stop");
  run(db, "FLUSH TABLES");
  }
  auto keyed = [&](SqlDatabase& db, const std::string& sql) { return db.execute_local_query(sql).rows_affected; };
  {
    // restarted, nothing written during the warm-up: the map is built off the lock and keyed writes go through it
    SqlDatabase db(**k);
    db.init("sql_db");
    run(db, "USE updb");
    db.wait_warm_up();
    expect(status(db, "Asql_row_index_mem_bytes") > 0, "the warm-up built ue's row map: " + std::to_string(status(db, "Asql_row_index_mem_bytes")) + " bytes");
    expect(keyed(db, "UPDATE ue SET v = v + 1 WHERE id = 'k777'") == 1, "a keyed write after the warm-up finds its row");
    expect(cell(db, "SELECT v FROM ue WHERE id = 'k777'") == "3", "k777 is 3: " + cell(db, "SELECT v FROM ue WHERE id = 'k777'"));
    expect(keyed(db, "UPDATE ue SET v = 7 WHERE id = 'k199999'") == 1 && cell(db, "SELECT v FROM ue WHERE id = 'k199999'") == "7", "the last key's row");
    expect(keyed(db, "UPDATE ue SET v = 9 WHERE id = 'nope'") == 0, "a key no row has changes nothing");
    expect(cell(db, "SELECT COUNT(*) FROM ue WHERE v > 1") == "2", "two rows changed in all: " + cell(db, "SELECT COUNT(*) FROM ue WHERE v > 1"));
    run(db, "FLUSH TABLES");
  }
  {
    // restarted, a keyed write while the map is built (400 ms before it would go in): the write builds its own, the
    // warm-up's is not put in, and every write stands - read once the warm-up is done (a read during it can miss a
    // write just made: a separate fault, older than the map built off the lock)
    SqlDatabase db(**k);
    db.init("sql_db");
    run(db, "USE updb");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    expect(keyed(db, "UPDATE ue SET v = 10 WHERE id = 'k1234'") == 1, "a keyed write during the warm-up finds its row");
    db.wait_warm_up();
    expect(cell(db, "SELECT v FROM ue WHERE id = 'k1234'") == "10", "the write during the warm-up stands: " + cell(db, "SELECT v FROM ue WHERE id = 'k1234'"));
    expect(keyed(db, "UPDATE ue SET v = v + 1 WHERE id = 'k777'") == 1 && cell(db, "SELECT v FROM ue WHERE id = 'k777'") == "4", "k777 is 4 after it");
    expect(cell(db, "SELECT COUNT(*) FROM ue WHERE v > 1") == "3", "three rows changed in all: " + cell(db, "SELECT COUNT(*) FROM ue WHERE v > 1"));
    expect(cell(db, "SELECT COUNT(*) FROM ue") == "200000", "200000 rows");
  }

  std::printf("  %d checks\n", checks);
  if (failures) { std::printf("=== %d FAILURE(S) ===\n", failures); return 1; }
  std::printf("=== ALL UPKEEP CHECKS PASSED ===\n");
  return 0;
}
