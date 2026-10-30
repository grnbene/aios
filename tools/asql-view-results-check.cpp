// Views and nested sub-queries answer what MariaDB answers (differential comparison, 2026-10-26):
// - COUNT(*) over a view that aggregates with no GROUP BY is one row, not the rows the view read
//   (moving_avg_supplier answered 52303 where MariaDB answers 1);
// - col IN (SELECT ...) on a reopened table reads each row's dictionary code where the segment holds it
//   (subquery items: x IN (SELECT ... WHERE loc IN (SELECT ...)) lost about half its matches).
#undef NDEBUG
#include <cstdio>
#include <string>

#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"
#include "check-root.hpp"

using namespace aios;
using namespace aios::userland;

namespace {
int failures = 0;
std::string one(SqlDatabase& db, const std::string& sql) {
  auto r = db.execute_local_query(sql);
  if (!r.success) return "ERROR " + r.error_message;
  return r.rows.empty() || r.rows[0].values.empty() ? "<none>" : r.rows[0].values[0];
}
void expect(SqlDatabase& db, const std::string& sql, const std::string& want) {
  const std::string got = one(db, sql);
  std::printf("  [%s] %s -> %s (want %s)\n", got == want ? "ok" : "FAIL", sql.c_str(), got.c_str(), want.c_str());
  if (got != want) ++failures;
}
void insert_many(SqlDatabase& db, const std::string& table, int n, std::string (*row)(int)) {
  for (int at = 0; at < n; at += 1000) {
    std::string sql = "INSERT INTO " + table + " VALUES ";
    for (int i = at; i < n && i < at + 1000; ++i) sql += (i > at ? "," : "") + row(i);
    if (!db.execute_local_query(sql).success) { std::printf("  insert into %s failed\n", table.c_str()); ++failures; return; }
  }
}
// part numbers as the suppliers write them: text that reads as a number, zero-padded, some with a ".1" suffix
std::string pn_of(int i) {
  char b[24];
  std::snprintf(b, sizeof b, i % 3 == 0 ? "%05d.1" : "%05d", i);
  return b;
}
}  // namespace

int main() {
  const CheckRoot check_root("/tmp/aios-view-results-check");   // this run's own (see check-root.hpp)
  const std::string& root = check_root.path;
  KernelConfig cfg;
  cfg.root = root;
  cfg.residency = test_config;
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::printf("boot failed\n"); return 1; }
  {
    SqlDatabase db(**k);
    db.init("sql_db");
    db.execute_local_query("CREATE DATABASE m");
    db.execute_local_query("USE m");

    // part numbers, and stock rows naming them at three warehouses (and at others the inner set leaves out)
    db.execute_local_query("CREATE TABLE parts (pn VARCHAR(15))");
    db.execute_local_query("CREATE TABLE stock (sid VARCHAR(25), loc VARCHAR(5), qty INT)");
    db.execute_local_query("CREATE TABLE maps (warehouse VARCHAR(5))");
    insert_many(db, "parts", 24000, [](int i) { return "('" + pn_of(i) + "')"; });
    // part i is stocked at warehouse 03, 60 or 99 when i % 4 != 0; at 77 (not mapped) when i % 4 == 0. Rows past the
    // first 24000 name the same parts at other unmapped warehouses, so the table is past the vectorized scan's size.
    insert_many(db, "stock", 24000, [](int i) {
      static const char* locs[] = {"77", "03", "60", "99"};
      return "('" + pn_of(i) + "','" + locs[i % 4] + "'," + std::to_string(i % 13 - 6) + ")";
    });
    insert_many(db, "stock", 140000, [](int i) {
      return "('" + pn_of(i % 24000) + "','" + std::to_string(10 + i % 50) + "',0)";
    });
    db.execute_local_query("INSERT INTO maps VALUES ('03'),('60'),('99')");
    db.execute_local_query("CREATE VIEW avg_all AS SELECT sid AS s, AVG(qty) AS a, SUM(qty) AS t, COUNT(sid) AS n FROM stock WHERE loc <> '77'");
    if (!db.flush_now().ok()) { std::printf("checkpoint failed\n"); return 1; }
  }
  // reopened: a reopened table's dictionary codes are packed per segment (the IN sub-query's per-code path read them
  // as one array: `loc IN (SELECT '03')` found no row)
  SqlDatabase db(**k);
  db.init("sql_db");
  db.execute_local_query("USE m");

  std::printf("IN sub-queries, nested and not\n");
  expect(db, "SELECT COUNT(*) FROM stock WHERE loc IN (SELECT '03')", "6000");
  expect(db, "SELECT COUNT(*) FROM stock WHERE loc IN (SELECT '03' UNION SELECT '60')", "12000");
  expect(db, "SELECT COUNT(*) FROM parts WHERE pn IN (SELECT sid FROM stock WHERE loc IN ('03','60','99'))", "18000");
  expect(db, "SELECT COUNT(*) FROM parts WHERE pn IN (SELECT sid FROM stock WHERE loc IN (SELECT warehouse FROM maps))", "18000");
  expect(db, "SELECT COUNT(*) FROM parts WHERE pn NOT IN (SELECT sid FROM stock WHERE loc IN (SELECT warehouse FROM maps))", "6000");
  expect(db, "SELECT COUNT(*) FROM parts WHERE !(pn IN (SELECT sid FROM stock WHERE loc IN (SELECT '03' UNION SELECT '60' UNION SELECT '99')))", "6000");

  std::printf("a view that aggregates with no GROUP BY\n");
  expect(db, "SELECT COUNT(*) FROM avg_all", "1");
  expect(db, "SELECT COUNT(*) FROM (SELECT * FROM avg_all) x", "1");
  expect(db, "SELECT n FROM avg_all", "158000");

  std::printf("%s\n", failures ? "FAILED" : "all ok");
  return failures ? 1 : 0;
}
