// Checks aggregates over boolean expressions per group, and the names of
// expression columns. On the live mirror `SUM(startdate<='2026-10-23' AND
// enddate>='2026-10-23') ... GROUP BY pricebook` gave 0 for every group while
// the same SUM without GROUP BY (and COUNT(*) under the same WHERE) counted
// 52761 rows; IF() and CASE over the AND gave 0 too. And `SUM(a AND b)` came
// back named `SUM()`, so two such columns collided in an associative fetch;
// MySQL names an unaliased column by its expression text as written.
#undef NDEBUG
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"
#include "check-root.hpp"

using namespace aios;
using namespace aios::userland;

namespace {

int failures = 0;
int checks = 0;

SqlQueryResult run(SqlDatabase& db, const std::string& sql) {
  SqlQueryResult r = db.execute_local_query(sql);
  if (!r.success) { std::printf("  query failed: %s\n  %s\n", sql.c_str(), r.error_message.c_str()); std::exit(1); }
  return r;
}

void exec(SqlDatabase& db, const std::string& sql) { run(db, sql); }

void expect(bool ok, const std::string& what) {
  ++checks;
  if (!ok) { ++failures; std::printf("  [FAIL] %s\n", what.c_str()); }
}

struct Row { int book; double price; std::string start, end; char type; };

std::string date_of(int day) {  // day 0 = 2026-01-01, within one year
  static const int mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int m = 0;
  while (day >= mdays[m]) { day -= mdays[m]; ++m; }
  char b[16];
  std::snprintf(b, sizeof b, "2026-%02d-%02d", m + 1, day + 1);
  return b;
}

std::vector<Row> make_rows(size_t n) {
  std::vector<Row> rows;
  for (size_t i = 0; i < n; ++i) {
    Row r;
    r.book = (int)(i % 3 == 0);
    r.price = (i % 7 == 0) ? 0.0 : (double)(i % 50) * 1.25;
    r.start = date_of((int)(i * 13 % 365));
    r.end = (i % 5 == 0) ? "9999-12-31" : date_of((int)(i * 29 % 365));
    r.type = (i % 4 == 0) ? 'R' : 'W';
    rows.push_back(r);
  }
  return rows;
}

void fill(SqlDatabase& db, const std::string& table, const std::vector<Row>& rows) {
  exec(db, "CREATE TABLE " + table + " (id INT NOT NULL, typeabbrev CHAR(2) NOT NULL, price DECIMAL(20,4) NOT NULL DEFAULT '0.0000', "
           "startdate DATE NOT NULL DEFAULT '0000-00-00', enddate DATE NOT NULL DEFAULT '9999-12-31', pricebook TINYINT(1) DEFAULT '0', "
           "PRIMARY KEY (id), KEY IsPriceBook (pricebook))");
  std::string batch;
  for (size_t i = 0; i < rows.size(); ++i) {
    char price[32];
    std::snprintf(price, sizeof price, "%.4f", rows[i].price);
    batch += (batch.empty() ? "" : ",") + std::string("(") + std::to_string(i) + ",'" + rows[i].type + "'," + price + ",'" +
             rows[i].start + "','" + rows[i].end + "'," + std::to_string(rows[i].book) + ")";
    if (batch.size() > 100000 || i + 1 == rows.size()) { exec(db, "INSERT INTO " + table + " VALUES " + batch); batch.clear(); }
  }
}

void run_table(SqlDatabase& db, const std::string& table, const std::vector<Row>& rows, const char* step) {
  const std::string day = "2026-06-15";
  // Per book: [0] start<=day AND end>=day, [1] start<=day AND price>0, [2] start<=day,
  // [3] price>0 AND price<40.
  long long want[2][4] = {};
  for (const auto& r : rows) {
    if (r.type != 'W') continue;
    const bool s = r.start <= day, e = r.end >= day, p = r.price > 0;
    want[r.book][0] += s && e;
    want[r.book][1] += s && p;
    want[r.book][2] += s;
    want[r.book][3] += p && r.price < 40;
  }
  const std::string a = "startdate<='" + day + "' AND enddate>='" + day + "'";
  const std::string b = "startdate<='" + day + "' AND price>0";
  const std::vector<std::string> forms = {
      "SUM(" + a + ")", "SUM(" + b + ")", "SUM(startdate<='" + day + "')", "SUM(price>0 AND price<40)",
      "SUM(IF(" + a + ",1,0))", "SUM(CASE WHEN " + a + " THEN 1 ELSE 0 END)", "COUNT(IF(" + a + ",1,NULL))",
      "SUM(" + b + " AND enddate>='" + day + "')"};
  const int which[] = {0, 1, 2, 3, 0, 0, 0, -1};
  // The last form: all three conditions.
  long long want_all[2] = {};
  for (const auto& r : rows)
    if (r.type == 'W' && r.start <= day && r.price > 0 && r.end >= day) ++want_all[r.book];

  for (size_t f = 0; f < forms.size(); ++f) {
    const long long w0 = which[f] < 0 ? want_all[0] : want[0][which[f]];
    const long long w1 = which[f] < 0 ? want_all[1] : want[1][which[f]];
    const std::string tail = " FROM " + table + " WHERE typeabbrev='W'";
    // Grouped: one row per book.
    SqlQueryResult g = run(db, "SELECT pricebook, " + forms[f] + tail + " GROUP BY pricebook ORDER BY pricebook");
    std::string got;
    for (const auto& row : g.rows) got += (got.empty() ? "" : ",") + row.values[0] + "=" + row.values[1];
    const std::string want_s = "0=" + std::to_string(w0) + ",1=" + std::to_string(w1);
    expect(got == want_s, std::string(step) + " grouped " + forms[f] + ": got " + got + ", want " + want_s);
    // Grouped without the key in the select list, and with a second aggregate beside it.
    SqlQueryResult g2 = run(db, "SELECT " + forms[f] + ", COUNT(*)" + tail + " GROUP BY pricebook ORDER BY pricebook");
    got.clear();
    for (const auto& row : g2.rows) got += (got.empty() ? "" : ",") + row.values[0];
    expect(got == std::to_string(w0) + "," + std::to_string(w1),
           std::string(step) + " grouped (no key) " + forms[f] + ": got " + got);
    // Ungrouped, and the same count through WHERE.
    SqlQueryResult u = run(db, "SELECT " + forms[f] + tail);
    expect(!u.rows.empty() && u.rows[0].values[0] == std::to_string(w0 + w1),
           std::string(step) + " ungrouped " + forms[f] + ": got " + (u.rows.empty() ? "<none>" : u.rows[0].values[0]));
  }
  // Sums of values, not counts (column arithmetic, two columns, a DATE's day): per group, the same as each
  // group's own total through WHERE.
  for (const std::string& f : {std::string("SUM(price*2)"), std::string("SUM(price+id)"), std::string("AVG(price*2)"),
                               std::string("SUM(IF(enddate>='" + day + "',price,0))"), std::string("SUM(DAYOFMONTH(startdate))")}) {
    SqlQueryResult g = run(db, "SELECT pricebook, " + f + " FROM " + table + " WHERE typeabbrev='W' GROUP BY pricebook ORDER BY pricebook");
    expect(g.rows.size() == 2, std::string(step) + " grouped " + f + ": " + std::to_string(g.rows.size()) + " groups");
    for (size_t i = 0; i < g.rows.size() && i < 2; ++i) {
      SqlQueryResult w = run(db, "SELECT " + f + " FROM " + table + " WHERE typeabbrev='W' AND pricebook=" + g.rows[i].values[0]);
      expect(g.rows[i].values[1] == w.rows[0].values[0],
             std::string(step) + " grouped " + f + " book " + g.rows[i].values[0] + ": " + g.rows[i].values[1] + ", WHERE gives " + w.rows[0].values[0]);
    }
  }
  // Every book through WHERE, as a cross-check on the expected numbers.
  for (int book = 0; book < 2; ++book) {
    SqlQueryResult w = run(db, "SELECT COUNT(*) FROM " + table + " WHERE typeabbrev='W' AND pricebook=" + std::to_string(book) + " AND " + a);
    expect(w.rows[0].values[0] == std::to_string(want[book][0]), std::string(step) + " WHERE count, book " + std::to_string(book));
  }
}

void check_names(SqlDatabase& db) {
  // MySQL names an unaliased expression column by its text as written.
  SqlQueryResult r = run(db,
      "SELECT pricebook, SUM(startdate<='2026-06-15' AND enddate>='2026-06-15'), SUM(startdate<='2026-06-15' AND price>0), "
      "IF(price>0,1,0), SUM(CASE WHEN price>0 THEN 1 ELSE 0 END), COUNT(*), SUM( price ) FROM small GROUP BY pricebook");
  const std::vector<std::string> want = {"pricebook",
                                         "SUM(startdate<='2026-06-15' AND enddate>='2026-06-15')",
                                         "SUM(startdate<='2026-06-15' AND price>0)",
                                         "IF(price>0,1,0)",
                                         "SUM(CASE WHEN price>0 THEN 1 ELSE 0 END)",
                                         "COUNT(*)",
                                         "SUM( price )"};
  expect(r.columns.size() == want.size(), "column count");
  for (size_t i = 0; i < want.size() && i < r.columns.size(); ++i)
    expect(r.columns[i] == want[i], "column " + std::to_string(i) + " named '" + r.columns[i] + "', want '" + want[i] + "'");
  SqlQueryResult u = run(db, "SELECT SUM(startdate<='2026-06-15' AND enddate>='2026-06-15'), IF(price>0,1,0) FROM small LIMIT 1");
  expect(u.columns.size() == 2 && u.columns[0] == want[1] && u.columns[1] == want[3],
         "ungrouped names: '" + (u.columns.empty() ? std::string() : u.columns[0]) + "', '" +
             (u.columns.size() < 2 ? std::string() : u.columns[1]) + "'");
  // The same through the gateway, the path a client's statement takes.
  SqlQueryResult w = db.execute_gateway_query(
      "SELECT pricebook, SUM(startdate<='2026-06-15' AND enddate>='2026-06-15'), SUM(startdate<='2026-06-15' AND price>0), "
      "IF(price>0,1,0), SUM(CASE WHEN price>0 THEN 1 ELSE 0 END), COUNT(*), SUM( price ) FROM small GROUP BY pricebook", "gexdb");
  expect(w.success, "gateway query: " + w.error_message);
  expect(w.columns.size() == want.size(), "gateway column count");
  for (size_t i = 0; i < want.size() && i < w.columns.size(); ++i)
    expect(w.columns[i] == want[i], "gateway column " + std::to_string(i) + " named '" + w.columns[i] + "', want '" + want[i] + "'");
}

}  // namespace

int main() {
  std::printf("=== ASQL grouped expression aggregate check ===\n");
  const CheckRoot check_root("/tmp/aios-asql-grouped-expr");   // this run's own (see check-root.hpp)
  const std::string& root = check_root.path;
  KernelConfig cfg;
  cfg.root = root;
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::printf("boot failed\n"); return 1; }
  SqlDatabase db(**k);
  db.init("sql_db");
  exec(db, "CREATE DATABASE IF NOT EXISTS gexdb");
  exec(db, "USE gexdb");
  const std::vector<Row> small = make_rows(40), big = make_rows(30000);
  fill(db, "small", small);
  fill(db, "big", big);
  run_table(db, "small", small, "small, as inserted");
  run_table(db, "big", big, "big, as inserted");
  check_names(db);
  db.optimize_all_tables("gexdb");
  run_table(db, "small", small, "small, re-encoded");
  run_table(db, "big", big, "big, re-encoded");
  check_names(db);
  std::printf("  %d checks\n", checks);
  if (failures) { std::printf("=== %d FAILURE(S) ===\n", failures); return 1; }
  std::printf("=== ALL GROUPED EXPRESSION CHECKS PASSED ===\n");
  return 0;
}
