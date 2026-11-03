// Checks that a stored function's answer over a column is never kept past a write to a table it reads.
//
// A dictionary-coded column lets the engine evaluate an expression once per distinct value, and some of those
// per-value answers are kept across statements, keyed by the column's version alone (a WHERE's truth per code,
// an aggregate over f(col)). That is right for an expression of the column and nothing else. A stored function
// is not that: `RETURN (SELECT price FROM prices WHERE stockid = s)` reads another table, so after
// `UPDATE prices` the same column gives other answers. SUM(getp(stockid)) kept returning the sums from before
// the update - in every statement, with the result cache off, until the column itself changed.
//
// Each case is run, the read table written, and run again; the expected answers are worked out here from the
// rows written. Also checked: a function redefined under the same name (DROP + CREATE) is not answered with the
// old body's values, and a function of its parameters alone still gives the right answers per value.
#undef NDEBUG
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"
#include "check-root.hpp"

using namespace aios;
using namespace aios::userland;

namespace {

int failures = 0;
int checks = 0;
void expect_eq(const std::string& got, const std::string& want, const std::string& what) {
  ++checks;
  if (got != want) { ++failures; std::printf("  [FAIL] %s: got %s, want %s\n", what.c_str(), got.c_str(), want.c_str()); }
  else std::printf("  [ok] %s: %s\n", what.c_str(), got.c_str());
}
SqlQueryResult run(SqlDatabase& db, const std::string& sql) {
  SqlQueryResult r = db.execute_local_query(sql);
  if (!r.success) { std::printf("  query failed: %s\n  %s\n", sql.c_str(), r.error_message.c_str()); std::exit(1); }
  return r;
}
std::string cell(SqlDatabase& db, const std::string& sql) {
  SqlQueryResult r = run(db, sql);
  return r.rows.empty() || r.rows[0].values.empty() ? std::string("?") : sql_display(r.rows[0].values[0]);
}
// Every row's cells joined, rows by '|': a GROUP BY's whole answer as one string.
std::string all(SqlDatabase& db, const std::string& sql) {
  SqlQueryResult r = run(db, sql);
  std::string s;
  for (const auto& row : r.rows) {
    if (!s.empty()) s += '|';
    for (size_t i = 0; i < row.values.size(); ++i) s += (i ? "," : "") + sql_display(row.values[i]);
  }
  return s;
}
std::string money(long long cents) {
  char b[64];
  std::snprintf(b, sizeof b, "%lld.%02lld", cents / 100, cents % 100);
  return b;
}

constexpr int kRows = 20000;
constexpr int kCodes = 50;   // distinct stockids: S0 .. S49, row i has S(i % kCodes)

// The price of each code in cents, as written so far.
std::map<int, long long> price;

long long sum_all() { long long s = 0; for (int i = 1; i <= kRows; ++i) s += price[i % kCodes]; return s; }
long long min_all() { long long m = -1; for (auto& [k, p] : price) if (m < 0 || p < m) m = p; return m; }
int count_at_least(long long cents) { int n = 0; for (int i = 1; i <= kRows; ++i) if (price[i % kCodes] >= cents) ++n; return n; }
std::string per_code_rows(int first, int last) {   // GROUP BY stockid over codes [first, last], as all() prints it
  std::string s;
  for (int k = first; k <= last; ++k) {
    long long n = 0;
    for (int i = 1; i <= kRows; ++i) if (i % kCodes == k) ++n;
    if (!s.empty()) s += '|';
    s += "S" + std::to_string(k) + "," + money(price[k] * n);
  }
  return s;
}

void check_all(SqlDatabase& db, const std::string& when) {
  expect_eq(cell(db, "SELECT SUM(getp(stockid)) FROM items"), money(sum_all()), when + ": SUM(getp(stockid))");
  expect_eq(cell(db, "SELECT MIN(getp(stockid)) FROM items"), money(min_all()), when + ": MIN(getp(stockid))");
  expect_eq(cell(db, "SELECT SUM(getp(stockid)) FROM items WHERE id <= 10"),
            money([] { long long s = 0; for (int i = 1; i <= 10; ++i) s += price[i % kCodes]; return s; }()),
            when + ": SUM(getp(stockid)) over 10 rows");
  expect_eq(cell(db, "SELECT COUNT(*) FROM items WHERE getp(stockid) >= 30.00"), std::to_string(count_at_least(3000)),
            when + ": WHERE getp(stockid) >= 30");
  expect_eq(cell(db, "SELECT COUNT(*) FROM items WHERE twice(stockid) >= 60.00"), std::to_string(count_at_least(3000)),
            when + ": WHERE twice(stockid) >= 60 (a function calling getp)");
  expect_eq(all(db, "SELECT stockid, SUM(getp(stockid)) FROM items WHERE stockid IN ('S1','S2','S3') GROUP BY stockid ORDER BY stockid"),
            per_code_rows(1, 3), when + ": GROUP BY stockid, SUM(getp(stockid))");
  expect_eq(cell(db, "SELECT SUM(p) FROM item_prices"), money(sum_all()), when + ": SUM over a view of getp(stockid)");
  expect_eq(cell(db, "SELECT getp('S1')"), money(price[1]), when + ": getp('S1') at top level");
  // The same aggregate run inside a routine: a procedure's statement, and a function's subquery.
  run(db, "CALL sum_prices(@t)");
  expect_eq(cell(db, "SELECT @t"), money(sum_all()), when + ": SUM(getp(stockid)) in a procedure");
  expect_eq(cell(db, "SELECT total_prices()"), money(sum_all()), when + ": SUM(getp(stockid)) in a function's subquery");
}

}  // namespace

int main() {
  std::printf("=== ASQL stored functions over a column, after writes to what they read ===\n");
  const CheckRoot check_root("/tmp/aios-asql-routine-freshness");
  KernelConfig cfg;
  cfg.root = check_root.path;
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::printf("boot failed\n"); return 1; }
  {
    SqlDatabase db(**k);
    db.init("sql_db");
    run(db, "CREATE DATABASE IF NOT EXISTS rfdb");
    run(db, "USE rfdb");
    run(db, "CREATE TABLE prices (stockid VARCHAR(10) PRIMARY KEY, price DECIMAL(10,2))");
    std::string ins = "INSERT INTO prices VALUES ";
    for (int c = 0; c < kCodes; ++c) {
      price[c] = (c + 1) * 100;   // S0 1.00, S1 2.00, ...
      ins += (c ? "," : "") + std::string("('S") + std::to_string(c) + "'," + money(price[c]) + ")";
    }
    run(db, ins);
    run(db, "CREATE TABLE items (id INT PRIMARY KEY, stockid VARCHAR(10))");
    for (int b = 1; b <= kRows; b += 2000) {
      std::string sql = "INSERT INTO items VALUES ";
      for (int i = b; i < b + 2000 && i <= kRows; ++i)
        sql += (i == b ? "" : ",") + std::string("(") + std::to_string(i) + ",'S" + std::to_string(i % kCodes) + "')";
      run(db, sql);
    }
    run(db, "CREATE FUNCTION getp(s VARCHAR(10)) RETURNS DECIMAL(10,2) RETURN (SELECT price FROM prices WHERE stockid = s)");
    run(db, "CREATE FUNCTION twice(s VARCHAR(10)) RETURNS DECIMAL(10,2) RETURN getp(s) * 2");
    run(db, "CREATE VIEW item_prices AS SELECT id, getp(stockid) AS p FROM items");
    run(db, "CREATE PROCEDURE sum_prices(OUT t DECIMAL(12,2)) BEGIN SELECT SUM(getp(stockid)) INTO t FROM items; END");
    run(db, "CREATE FUNCTION total_prices() RETURNS DECIMAL(12,2) RETURN (SELECT SUM(getp(stockid)) FROM items)");

    check_all(db, "as loaded");

    run(db, "UPDATE prices SET price = 0.50 WHERE stockid IN ('S1', 'S3')");
    price[1] = 50; price[3] = 50;
    check_all(db, "after an UPDATE");

    run(db, "UPDATE prices SET price = 99.00 WHERE stockid = 'S2'");
    price[2] = 9900;
    check_all(db, "after a second UPDATE");

    run(db, "DELETE FROM prices WHERE stockid = 'S0'");
    run(db, "INSERT INTO prices VALUES ('S0', 7.00)");
    price[0] = 700;
    check_all(db, "after a DELETE and an INSERT");

    // A function of its parameters alone, redefined: the second body's answers, not the first's.
    run(db, "CREATE FUNCTION tag(s VARCHAR(10)) RETURNS VARCHAR(20) RETURN CONCAT(s, '-a')");
    expect_eq(cell(db, "SELECT COUNT(*) FROM items WHERE tag(stockid) = 'S7-a'"), std::to_string(kRows / kCodes),
              "tag(stockid) = 'S7-a' (first body)");
    expect_eq(cell(db, "SELECT MAX(tag(stockid)) FROM items"), "S9-a", "MAX(tag(stockid)) (first body)");
    run(db, "DROP FUNCTION tag");
    run(db, "CREATE FUNCTION tag(s VARCHAR(10)) RETURNS VARCHAR(20) RETURN CONCAT(s, '-b')");
    expect_eq(cell(db, "SELECT COUNT(*) FROM items WHERE tag(stockid) = 'S7-a'"), "0", "tag(stockid) = 'S7-a' (second body)");
    expect_eq(cell(db, "SELECT COUNT(*) FROM items WHERE tag(stockid) = 'S7-b'"), std::to_string(kRows / kCodes),
              "tag(stockid) = 'S7-b' (second body)");
    expect_eq(cell(db, "SELECT MAX(tag(stockid)) FROM items"), "S9-b", "MAX(tag(stockid)) (second body)");
  }
  std::printf("  %d checks\n", checks);
  if (failures) { std::printf("=== %d ROUTINE FRESHNESS CHECKS FAILED ===\n", failures); return 1; }
  std::printf("=== ALL ROUTINE FRESHNESS CHECKS PASSED ===\n");
  return 0;
}
