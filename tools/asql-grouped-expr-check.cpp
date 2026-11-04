// Checks aggregates over boolean expressions per group, and the names of
// expression columns. On the live mirror `SUM(startdate<='2026-10-23' AND
// enddate>='2026-10-23') ... GROUP BY pricebook` gave 0 for every group while
// the same SUM without GROUP BY (and COUNT(*) under the same WHERE) counted
// 52761 rows; IF() and CASE over the AND gave 0 too. And `SUM(a AND b)` came
// back named `SUM()`, so two such columns collided in an associative fetch;
// MySQL names an unaliased column by its expression text as written.
#undef NDEBUG
#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
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

// km's thisoverlast: 36 `SUM(CASE WHEN month = k THEN <a view's CAST(.. AS DECIMAL)> ELSE 0 END)` columns, the
// same CAST under each month. Each column must be what that month's rows sum to on their own (WHERE m = k), and
// a CAST over pb's columns is not the same CAST over pa's (the tables have one layout: the same column numbers).
// pb lacks every 9th id (LEFT JOIN padding: NULL) and pa's d is NULL on every 11th row; 60,000 rows cross the
// lanes' 16,384-row chunks.
void check_pivot(SqlDatabase& db) {
  exec(db, "DROP TABLE IF EXISTS pa");
  exec(db, "DROP TABLE IF EXISTS pb");
  exec(db, "CREATE TABLE pa (id INT PRIMARY KEY, g INT, m INT, p DOUBLE, q DOUBLE, d DOUBLE)");
  exec(db, "CREATE TABLE pb (id INT PRIMARY KEY, g INT, m INT, p DOUBLE, q DOUBLE, d DOUBLE)");
  const int n = 60000;
  for (int b = 0; b < n; b += 2000) {
    std::string va, vb;
    for (int i = b; i < b + 2000; ++i) {
      char x[160];
      std::snprintf(x, sizeof x, "%s(%d,%d,%d,%.2f,%d,%s)", va.empty() ? "" : ",", i, i % 3, 1 + (i * 7) % 4,
                    (double)(i % 997) * 0.37 + 0.05, -1 - (i % 13), i % 11 == 0 ? "NULL" : (i % 5 == 0 ? "0.15" : "0.3"));
      va += x;
      if (i % 9 == 0) continue;
      std::snprintf(x, sizeof x, "%s(%d,%d,%d,%.2f,%d,0)", vb.empty() ? "" : ",", i, i % 3, 1, (double)(i % 89) * 1.75 + 0.25, 1 + (i % 7));
      vb += x;
    }
    exec(db, "INSERT INTO pa VALUES " + va);
    if (!vb.empty()) exec(db, "INSERT INTO pb VALUES " + vb);
  }
  const std::string dlt = "CAST(pa.p * pa.q - CAST(pa.p * pa.q * pa.d AS DECIMAL(10,2)) AS DECIMAL(10,2))";
  const std::string bcast = "CAST(pb.p * pb.q AS DECIMAL(10,2))";
  const std::string from = " FROM pa LEFT JOIN pb ON pb.id = pa.id";
  std::string sel = "SELECT pa.g";
  for (int k = 1; k <= 5; ++k)   // month 5 has no rows: 0.00
    sel += ", -1*SUM(CASE WHEN pa.m = " + std::to_string(k) + " THEN " + dlt + " ELSE 0 END), SUM(CASE WHEN pa.m = " +
           std::to_string(k) + " THEN " + bcast + " ELSE 0 END)";
  const SqlQueryResult piv = run(db, sel + from + " GROUP BY pa.g ORDER BY pa.g");
  expect(piv.rows.size() == 3, "pivot: 3 groups");
  for (int k = 1; k <= 5; ++k) {
    const SqlQueryResult ref = run(db, "SELECT pa.g, -1*SUM(" + dlt + "), SUM(" + bcast + ")" + from + " WHERE pa.m = " +
                                       std::to_string(k) + " GROUP BY pa.g ORDER BY pa.g");
    for (size_t g = 0; g < piv.rows.size(); ++g) {
      std::string want_a = "0.00", want_b = "0.00";
      for (const auto& r : ref.rows)
        if (sql_display(r.values[0]) == sql_display(piv.rows[g].values[0])) {
          want_a = sql_display(r.values[1]);
          want_b = sql_display(r.values[2]);
        }
      const std::string got_a = sql_display(piv.rows[g].values[(size_t)(2 * k - 1)]);
      const std::string got_b = sql_display(piv.rows[g].values[(size_t)(2 * k)]);
      expect(got_a == want_a, "pivot month " + std::to_string(k) + " group " + std::to_string(g) + " pa CAST: " + got_a + ", alone " + want_a);
      expect(got_b == want_b, "pivot month " + std::to_string(k) + " group " + std::to_string(g) + " pb CAST: " + got_b + ", alone " + want_b);
    }
  }
}

// The pivot shapes lane_group_by folds in one pass (PivotLanes): every SUM in one statement must print what it
// prints in a statement of its own, where it is no pivot and its own CASE lane is folded (the per-aggregate way).
// No ELSE (NULL), an ELSE other than 0, IF(), CASE k WHEN, `lit = k`, a k and an x with NULLs, a DOUBLE x (its
// sums added in row order, as its own lane's fold adds them, to the last bit), a lit no integer
// equals (1.5), a lit no row has, an x from the other table - over 200,000 joined rows, so the fold runs in parts;
// then with a text lit, which sends the statement to the interpreter's pivot.
void check_pivot_shapes(SqlDatabase& db) {
  exec(db, "DROP TABLE IF EXISTS pc");
  exec(db, "DROP TABLE IF EXISTS pk");
  exec(db, "CREATE TABLE pk (k INT PRIMARY KEY, w INT NOT NULL)");
  exec(db, "INSERT INTO pk VALUES (0, 1), (1, 2), (2, 3)");
  exec(db, "CREATE TABLE pc (id INT PRIMARY KEY, g INT NOT NULL, j INT NOT NULL, k INT, x DECIMAL(10,2), q INT NOT NULL, d DOUBLE)");
  const int n = 200000;
  for (int b = 0; b < n; b += 4000) {
    std::string v;
    for (int i = b; i < b + 4000; ++i) {
      char k[16], x[24], d[32], row[160];
      if (i % 17 == 0) std::snprintf(k, sizeof k, "NULL"); else std::snprintf(k, sizeof k, "%d", 1 + (i * 7) % 4);
      if (i % 13 == 0) std::snprintf(x, sizeof x, "NULL"); else std::snprintf(x, sizeof x, "%.2f", (double)(i % 997) * 0.37 - 90.0);
      // a DOUBLE no binary fraction holds (0.1 steps): its sums depend on the order they are added in
      if (i % 19 == 0) std::snprintf(d, sizeof d, "NULL"); else std::snprintf(d, sizeof d, "%.1f", (double)(i % 101) * 0.1 - 3.3);
      std::snprintf(row, sizeof row, "%s(%d,%d,%d,%s,%s,%d,%s)", v.empty() ? "" : ",", i, i % 5, i % 3, k, x, i % 11 - 5, d);
      v += row;
    }
    exec(db, "INSERT INTO pc VALUES " + v);
  }
  std::vector<std::string> sums;
  for (int l = 1; l <= 4; ++l) sums.push_back("SUM(CASE WHEN pc.k = " + std::to_string(l) + " THEN pc.x ELSE 0 END)");
  for (int l = 1; l <= 3; ++l) sums.push_back("SUM(CASE WHEN pc.k = " + std::to_string(l) + " THEN pc.x END)");
  for (int l = 1; l <= 3; ++l) sums.push_back("SUM(CASE WHEN pc.k = " + std::to_string(l) + " THEN pc.q ELSE 7 END)");
  for (int l = 1; l <= 3; ++l) sums.push_back("SUM(IF(pc.k = " + std::to_string(l) + ", pc.x * pc.q, 0))");
  for (int l = 1; l <= 3; ++l) sums.push_back("SUM(CASE pc.k WHEN " + std::to_string(l) + " THEN pc.q ELSE -2 END)");
  for (int l = 1; l <= 2; ++l) sums.push_back("SUM(CASE WHEN pc.k = " + std::to_string(l) + " THEN pc.d ELSE 0 END)");
  for (int l = 1; l <= 2; ++l) sums.push_back("SUM(CASE WHEN pc.k = " + std::to_string(l) + " THEN pc.d END)");
  for (int l = 1; l <= 2; ++l) sums.push_back("SUM(CASE WHEN pc.k = " + std::to_string(l) + " THEN pc.d ELSE 3 END)");
  sums.push_back("SUM(CASE WHEN 2 = pc.k THEN pc.x ELSE 0 END)");
  sums.push_back("SUM(CASE WHEN pc.k = 1.5 THEN pc.x ELSE 0 END)");
  sums.push_back("SUM(CASE WHEN pc.k = 2.0 THEN pc.x ELSE 0 END)");
  sums.push_back("SUM(CASE WHEN pc.k = 2.0000000000000000001 THEN pc.x ELSE 0 END)");   // 2.0 as a double: no k equals it
  sums.push_back("SUM(CASE WHEN pc.k = 9 THEN pc.x ELSE 0 END)");
  sums.push_back("-1*SUM(CASE WHEN pc.k = 4 THEN pc.x ELSE 0 END)");
  for (int l = 1; l <= 2; ++l) sums.push_back("SUM(CASE WHEN pc.k = " + std::to_string(l) + " THEN pk.w ELSE 0 END)");
  const std::string from = " FROM pc JOIN pk ON pk.k = pc.j GROUP BY pc.g ORDER BY pc.g";
  auto compare = [&](const std::vector<std::string>& list, const std::string& what) {
    std::string sel = "SELECT pc.g";
    for (const std::string& s : list) sel += ", " + s;
    const SqlQueryResult all = run(db, sel + from);
    expect(all.rows.size() == 5, what + ": 5 groups, got " + std::to_string(all.rows.size()));
    for (size_t c = 0; c < list.size(); ++c) {
      const SqlQueryResult one = run(db, "SELECT pc.g, " + list[c] + from);
      expect(one.rows.size() == all.rows.size(), what + ": " + list[c] + " alone, groups");
      for (size_t g = 0; g < all.rows.size() && g < one.rows.size(); ++g) {
        const std::string got = sql_display(all.rows[g].values[c + 1]), want = sql_display(one.rows[g].values[1]);
        expect(got == want, what + " group " + std::to_string(g) + " " + list[c] + ": " + got + ", alone " + want);
      }
    }
    return sel + from;
  };
  // every SUM has a lane: the statement is answered over lanes, the pivot's sums in one fold
  const std::string sel = compare(sums, "pivot shapes");
  const SqlQueryResult ex = run(db, "EXPLAIN ANALYZE " + sel);
  std::string plan;
  for (const auto& r : ex.rows) for (const auto& v : r.values) plan += sql_display(v) + "\n";
  if (ex.rows.empty()) plan = ex.formatted_table;
  expect(plan.find("group by [lanes") != std::string::npos, "pivot shapes: grouped over lanes");
  // a text lit, and a DECIMAL times another table's INT (no lane for either: the interpreter answers the
  // statement, with its own pivot pass)
  // (not the 2.0000000000000000001 lit: the interpreter's pivot compares k with it as a double, where it is 2)
  std::vector<std::string> with_text;
  for (const std::string& x : sums) if (x.find("2.0000000000000000001") == std::string::npos) with_text.push_back(x);
  with_text.push_back("SUM(CASE WHEN pc.k = '2' THEN pc.x ELSE 0 END)");
  with_text.push_back("SUM(CASE WHEN pc.k = 4 THEN pc.x * pk.w ELSE 0 END)");
  compare(with_text, "pivot shapes with a text lit");
}

// text_col [NOT] IN (SELECT ..) decided a column at a time (vp sub_in, its set run first by conjunct_truths): every
// statement must keep the rows the interpreter keeps, which it does for CONCAT(col, '') - the same text, a form no
// program takes. Sets with and without a NULL, an empty one, NULL cells, entries that meet only by collation
// ('ABC', 'abc  '), and parts over two tables (OR with the other's column), so they are decided after the join.
void check_in_subquery_lanes(SqlDatabase& db) {
  for (const char* t : {"ia", "ib", "ic"}) exec(db, std::string("DROP TABLE IF EXISTS ") + t);
  exec(db, "CREATE TABLE ic (k INT PRIMARY KEY, w INT NOT NULL)");
  exec(db, "INSERT INTO ic VALUES (0, 1), (1, 5), (2, 7), (3, 5)");
  exec(db, "CREATE TABLE ib (id INT PRIMARY KEY, code VARCHAR(12), kind INT NOT NULL)");
  exec(db, "INSERT INTO ib VALUES (1, 'abc', 1), (2, 'DEF', 1), (3, 'g7', 1), (4, 'abc', 2), (5, NULL, 2), (6, 'q1', 2), (7, 'ABC', 3), (8, 'zz', 3)");
  exec(db, "CREATE TABLE ia (id INT PRIMARY KEY, g INT NOT NULL, j INT NOT NULL, code VARCHAR(12), v INT NOT NULL)");
  const char* codes[] = {"abc", "ABC", "abc  ", "def", "g7", "q1", "zz", "other", "x9"};
  const int n = 150000;
  for (int b = 0; b < n; b += 5000) {
    std::string v;
    for (int i = b; i < b + 5000; ++i) {
      char row[96];
      const std::string c = i % 23 == 0 ? std::string("NULL") : "'" + std::string(codes[(i / 3) % 9]) + "'";
      std::snprintf(row, sizeof row, "%s(%d,%d,%d,%s,%d)", v.empty() ? "" : ",", i, i % 7, i % 4, c.c_str(), i % 101);
      v += row;
    }
    exec(db, "INSERT INTO ia VALUES " + v);
  }
  const std::vector<std::string> conds = {
      "ia.code IN (SELECT code FROM ib WHERE kind = 1)",
      "NOT ia.code IN (SELECT code FROM ib WHERE kind = 1)",
      "ia.code NOT IN (SELECT code FROM ib WHERE kind = 1)",
      "ia.code NOT IN (SELECT code FROM ib WHERE kind = 2)",   // a NULL in the set: no row
      "ia.code IN (SELECT code FROM ib WHERE kind = 2)",
      "ia.code IN (SELECT code FROM ib WHERE kind = 99)",      // no rows in the set
      "ia.code NOT IN (SELECT code FROM ib WHERE kind = 99)",  // every row, a NULL code too
      "(ia.code IN (SELECT code FROM ib WHERE kind = 3) OR ic.w = 7)",
      "(NOT ia.code IN (SELECT code FROM ib WHERE kind = 1) OR ic.w = 5)",
      "(ia.code NOT IN (SELECT code FROM ib WHERE kind = 2) OR ic.w = 1)",
  };
  for (const std::string& c : conds) {
    std::string oracle = c;
    for (size_t at = 0; (at = oracle.find("ia.code", at)) != std::string::npos; at += 20) oracle.replace(at, 7, "CONCAT(ia.code, '')");
    const std::string q = "SELECT ia.g, COUNT(*), SUM(ia.v), SUM(ic.w) FROM ia JOIN ic ON ic.k = ia.j WHERE ia.v >= 0 AND ";
    const SqlQueryResult got = run(db, q + c + " GROUP BY ia.g ORDER BY ia.g");
    const SqlQueryResult want = run(db, q + oracle + " GROUP BY ia.g ORDER BY ia.g");
    std::string gt, wt;
    for (const auto& r : got.rows) { for (const auto& v : r.values) gt += sql_display(v) + ","; gt += ";"; }
    for (const auto& r : want.rows) { for (const auto& v : r.values) wt += sql_display(v) + ","; wt += ";"; }
    expect(!want.rows.empty() || c.find("NOT IN (SELECT code FROM ib WHERE kind = 2)") != std::string::npos || c.find("99)") != std::string::npos,
           "in subquery: the interpreter keeps rows for " + c);
    expect(gt == wt, "in subquery: " + c + ": " + gt.substr(0, 160) + " | interpreter " + wt.substr(0, 160));
  }
}

// A star join with a one-to-many link (as stocktrans's debtortrans: a line's key matches several header rows) whose
// whole WHERE is decided over its arrays: the arrays, expanded, go to the grouping as its rows (no joined row made).
// Each statement must group what the same statement does when a WHERE part only the interpreter takes
// (CONCAT(.., '')) sends it through joined rows.
void check_star_multi_grouped(SqlDatabase& db) {
  for (const char* t : {"ml", "mh", "mp", "mu"}) exec(db, std::string("DROP TABLE IF EXISTS ") + t);
  exec(db, "CREATE TABLE mp (pid INT PRIMARY KEY, brand VARCHAR(16))");
  exec(db, "INSERT INTO mp VALUES (0, 'Acme'), (1, 'acme'), (2, NULL), (3, 'Zed'), (4, 'Bolt  '), (5, 'bolt')");
  exec(db, "CREATE TABLE mu (id INT PRIMARY KEY, loc VARCHAR(8), ok INT NOT NULL)");
  exec(db, "INSERT INTO mu VALUES (1, 'L1', 1), (2, 'L3', 1), (3, 'L4', 0)");
  exec(db, "CREATE TABLE mh (hid INT PRIMARY KEY, k INT NOT NULL, typ INT NOT NULL, w INT NOT NULL)");
  {
    std::string v;
    for (int h = 0; h < 6000; ++h) {   // 1 to 3 header rows a key
      const int k = h / 2 - (h % 7 == 0 ? 1 : 0);
      v += (v.empty() ? "" : ",") + std::string("(") + std::to_string(h) + "," + std::to_string(k < 0 ? 0 : k) + "," +
           std::to_string(h % 4) + "," + std::to_string(1 + h % 9) + ")";
    }
    exec(db, "INSERT INTO mh VALUES " + v);
  }
  exec(db, "CREATE TABLE ml (id INT PRIMARY KEY, k INT NOT NULL, pid INT NOT NULL, loc VARCHAR(8), qty DOUBLE NOT NULL, price DOUBLE NOT NULL, d DATE NOT NULL)");
  const int n = 160000;
  for (int b = 0; b < n; b += 5000) {
    std::string v;
    for (int i = b; i < b + 5000; ++i) {
      char row[160];
      std::snprintf(row, sizeof row, "%s(%d,%d,%d,'L%d',%.1f,%.2f,'2025-%02d-%02d')", v.empty() ? "" : ",", i, (i * 13) % 3000, i % 6,
                    1 + i % 5, (double)(i % 7) - 2.5, (double)(i % 211) * 1.37 + 0.05, 1 + i % 12, 1 + i % 28);
      v += row;
    }
    exec(db, "INSERT INTO ml VALUES " + v);
  }
  const std::string from = " FROM ml JOIN mh ON mh.k = ml.k JOIN mp ON mp.pid = ml.pid WHERE ml.d BETWEEN '2025-01-01' AND '2025-12-31' AND ";
  const std::vector<std::pair<std::string, std::string>> shapes = {
      {"SELECT COALESCE(mp.brand, 'NONE') AS b, CAST(SUM(ml.qty * -1) AS DECIMAL(16, 0)) AS q, CAST(SUM(CAST(ml.price AS DECIMAL(20,6)) * mh.w) AS DECIMAL(16, 2)) AS t",
       " GROUP BY b ORDER BY t DESC, b ASC"},
      {"SELECT YEAR(ml.d) AS y, MONTH(ml.d) AS m, COUNT(*), SUM(mh.w), SUM(CASE WHEN mh.typ = 1 THEN ml.qty ELSE 0 END), SUM(CASE WHEN mh.typ = 2 THEN ml.qty ELSE 0 END)",
       " GROUP BY YEAR(ml.d), MONTH(ml.d) ORDER BY y, m"},
  };
  const std::vector<std::pair<std::string, std::string>> wheres = {
      {"ml.loc IN (SELECT loc FROM mu WHERE ok = 1) AND NOT mh.typ = 3", "CONCAT(ml.loc, '') IN (SELECT loc FROM mu WHERE ok = 1) AND NOT mh.typ = 3"},
      {"mh.typ <> 0 AND ml.qty <> 0 AND ml.loc NOT IN (SELECT loc FROM mu WHERE ok = 0)",
       "mh.typ <> 0 AND ml.qty <> 0 AND CONCAT(ml.loc, '') NOT IN (SELECT loc FROM mu WHERE ok = 0)"},
  };
  for (const auto& sh : shapes)
    for (const auto& w : wheres) {
      const SqlQueryResult got = run(db, sh.first + from + w.first + sh.second);
      const SqlQueryResult want = run(db, sh.first + from + w.second + sh.second);
      std::string gt, wt;
      for (const auto& r : got.rows) { for (const auto& v : r.values) gt += sql_display(v) + ","; gt += ";"; }
      for (const auto& r : want.rows) { for (const auto& v : r.values) wt += sql_display(v) + ","; wt += ";"; }
      expect(!want.rows.empty(), "star multi grouped: rows for " + w.second);
      expect(gt == wt, "star multi grouped: " + sh.first.substr(0, 40) + " WHERE " + w.first + ": " + gt.substr(0, 200) + " | rows " + wt.substr(0, 200));
    }
}

// Run-length columns of a table much larger than the rows a grouped statement keeps: the lanes split the 150,000
// rows kept into 8 parts, each too small beside the 400,000-row table to decode its columns whole, so each part
// reads the runs through a cursor (Lanes::rle_cursor) - stepping run to run, and searching on a jump past many
// short runs. Every month pivot, tag sum and short-run count must be what the generated rows add up to. The rows
// are joined to a small parent, as the ERP's stocktrans view joins stockmoves to its parents, so the grouping is
// lane_group_by's over joined rows (a single table's is answered by slot, its own way).
void check_rle_parts(SqlDatabase& db) {
  exec(db, "DROP TABLE IF EXISTS rl");
  exec(db, "DROP TABLE IF EXISTS rk");
  // a parent each row joins (as stocktrans joins stockmoves to its parents): the grouping is then over joined rows
  exec(db, "CREATE TABLE rk (k INT PRIMARY KEY, w INT NOT NULL)");
  exec(db, "INSERT INTO rk VALUES (0, 10), (1, 20), (2, 30), (3, 40)");
  exec(db, "CREATE TABLE rl (id INT PRIMARY KEY, d DATE NOT NULL, tag VARCHAR(4) NOT NULL, r5 INT NOT NULL, amt INT NOT NULL, keep INT NOT NULL)");
  const int n = 400000;
  struct R { int y, m, day; char tag; int r5, amt, keep; };
  auto row = [](int i) {
    const int block = i / 1000;   // dates in runs of 1,000 rows, 2024-01 .. 2027-12
    R r;
    r.y = 2024 + block / 100;
    r.m = 1 + (block % 100) / 9;
    r.day = 1 + (block % 9) * 3;
    r.tag = "abc"[(i / 700) % 3];   // runs of 700
    r.r5 = (i / 5) % 4;             // runs of 5: a kept block of 50 skips 50 of them
    r.amt = (int)(((long long)i * 37) % 101) - 50;
    r.keep = (i / 50) % 8 < 3 ? 1 : 0;   // 3 blocks of 50 in 8
    return r;
  };
  for (int b = 0; b < n; b += 4000) {
    std::string v;
    for (int i = b; i < b + 4000; ++i) {
      const R r = row(i);
      char x[96];
      std::snprintf(x, sizeof x, "%s(%d,'%04d-%02d-%02d','%c',%d,%d,%d)", v.empty() ? "" : ",", i, r.y, r.m, r.day, r.tag, r.r5, r.amt, r.keep);
      v += x;
    }
    exec(db, "INSERT INTO rl VALUES " + v);
  }
  db.optimize_all_tables("gexdb");
  const SqlQueryResult st = run(db, "SHOW ASQL STORAGE rl");
  for (const char* c : {"d", "tag", "r5"}) {
    std::string enc = "(none)";
    for (const auto& r : st.rows) if (r.values.size() > 3 && sql_display(r.values[1]) == c) enc = sql_display(r.values[3]);
    expect(enc == "RLE", std::string("rle parts: column ") + c + " is RLE (" + enc + ")");
  }
  // the answers, from the rows as generated
  std::map<int, std::array<long long, 16>> want;   // year -> 12 months, tag b, r5 = 2 count, COUNT(*), SUM(rk.w)
  for (int i = 0; i < n; ++i) {
    const R r = row(i);
    if (!r.keep) continue;
    auto& w = want[r.y];
    w[(size_t)r.m - 1] += r.amt;
    if (r.tag == 'b') w[12] += r.amt;
    if (r.r5 == 2) w[13] += 1;
    w[14] += 1;
    w[15] += 10 * (r.r5 + 1);
  }
  std::string sel = "SELECT YEAR(rl.d) AS y";
  for (int m = 1; m <= 12; ++m) sel += ", SUM(CASE WHEN MONTH(rl.d) = " + std::to_string(m) + " THEN rl.amt ELSE 0 END)";
  sel += ", SUM(CASE WHEN rl.tag = 'b' THEN rl.amt ELSE 0 END), SUM(CASE WHEN rl.r5 = 2 THEN 1 ELSE 0 END), COUNT(*), SUM(rk.w) FROM rl JOIN rk ON rk.k = rl.r5 WHERE rl.keep = 1 GROUP BY YEAR(rl.d) ORDER BY y";
  const SqlQueryResult ex = run(db, "EXPLAIN ANALYZE " + sel);
  std::string plan;
  for (const auto& r : ex.rows) for (const auto& v : r.values) plan += sql_display(v) + "\n";
  if (ex.rows.empty()) plan = ex.formatted_table;
  expect(plan.find("group by [lanes") != std::string::npos, "rle parts: grouped over lanes");
  for (int pass = 0; pass < 2; ++pass) {   // twice: the second run over columns already read once
    const SqlQueryResult got = run(db, sel);
    expect(got.rows.size() == want.size(), "rle parts: " + std::to_string(want.size()) + " years, got " + std::to_string(got.rows.size()));
    for (const auto& r : got.rows) {
      const int y = std::atoi(sql_display(r.values[0]).c_str());
      auto it = want.find(y);
      if (it == want.end()) { expect(false, "rle parts: year " + std::to_string(y) + " not in the rows"); continue; }
      for (size_t k = 0; k < 16; ++k) {
        const std::string g = sql_display(r.values[k + 1]), w = std::to_string(it->second[k]);
        expect(g == w, "rle parts pass " + std::to_string(pass) + " year " + std::to_string(y) + " item " + std::to_string(k + 1) + ": " + g + ", rows give " + w);
      }
    }
  }
}

// summitholding's stocktrans view: its money columns are ROUND()s of CAST(.. AS DECIMAL(20,6)) - wider than 18
// digits, and ROUND, neither of which the exact lanes took, so `SUM(discountedlinetotal)` over 10M rows ran in the
// interpreter (17.7 s of a 23 s report). Each grouped SUM over lanes must be what the rows' own values (the row
// path, one value a row) add up to exactly; and the grouping must be over lanes (EXPLAIN: "group by [lanes").
// Also ROUND to more places than a value has (a decimal is padded, an integer is not), to negative places, of an
// INT column, and TRUNCATE; NULL prices every 13th row, negative quantities, prices on half-cents.
namespace {
// "-12.30" -> (-1230, 2)
bool parse_scaled(const std::string& s, __int128* v, int* sc) {
  if (s.empty() || s == "NULL") return false;
  __int128 x = 0;
  int scale = -1;
  bool neg = false;
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (i == 0 && c == '-') { neg = true; continue; }
    if (c == '.') { scale = 0; continue; }
    if (c < '0' || c > '9') return false;
    x = x * 10 + (c - '0');
    if (scale >= 0) ++scale;
  }
  *v = neg ? -x : x;
  *sc = scale < 0 ? 0 : scale;
  return true;
}
std::string scaled_text(__int128 v, int sc) {
  const bool neg = v < 0;
  if (neg) v = -v;
  std::string d;
  do { d.insert(d.begin(), (char)('0' + (int)(v % 10))); v /= 10; } while (v > 0);
  while ((int)d.size() <= sc) d.insert(d.begin(), '0');
  if (sc > 0) d.insert(d.end() - sc, '.');
  return (neg ? "-" : "") + d;
}
bool plan_has(SqlDatabase& db, const std::string& sql, const std::string& needle) {
  SqlQueryResult r = db.execute_local_query("EXPLAIN " + sql);
  if (!r.success) return false;
  for (const auto& row : r.rows)
    if (!row.values.empty() && sql_display(row.values[0]).find(needle) != std::string::npos) return true;
  for (const auto& row : r.rows) if (!row.values.empty()) std::printf("      plan: %s\n", sql_display(row.values[0]).c_str());
  return false;
}
}  // namespace

// --no-star (AIOS_SQL_STAR_JOIN=0): the join is not a star join, as the view's composite debtortrans join is not,
// and the grouped SUMs go to the lanes (lane_group_by) - which this run also requires of them.
bool g_no_star = false;

void check_round_lanes(SqlDatabase& db) {
  exec(db, "DROP VIEW IF EXISTS rvv");
  exec(db, "DROP TABLE IF EXISTS rv");
  exec(db, "DROP TABLE IF EXISTS rg");
  // a dimension the rows join, as the view's tables do: the grouped SUM is then over the joined rows' lanes
  // (two rows a g, one active: not a unique link, so no star join - the view's debtortrans join is not one either)
  exec(db, "CREATE TABLE rg (id INT PRIMARY KEY, g INT, active INT, label VARCHAR(10))");
  exec(db, "INSERT INTO rg VALUES (1,0,1,'a'),(2,1,1,'b'),(3,2,1,'c'),(4,3,1,'d'),(5,4,1,'e'),(6,0,0,'x'),(7,1,0,'x'),(8,2,0,'x'),(9,3,0,'x'),(10,4,0,'x')");
  exec(db, "CREATE TABLE rv (id INT PRIMARY KEY, g INT, m INT, price DOUBLE, qty DOUBLE, disc DOUBLE, ic INT)");
  for (int b = 0; b < 60000; b += 2000) {
    std::string v;
    for (int i = b; i < b + 2000; ++i) {
      char x[200];
      const char* disc = i % 4 == 0 ? "0.15" : i % 4 == 1 ? "0.3" : i % 4 == 2 ? "0.125" : "0";
      if (i % 13 == 0)
        std::snprintf(x, sizeof x, "%s(%d,%d,%d,NULL,%d,%s,%d)", v.empty() ? "" : ",", i, i % 5, 1 + i % 3, (i % 2 ? -1 : 1) * (1 + i % 9), disc, i * 7 - 200000);
      else
        std::snprintf(x, sizeof x, "%s(%d,%d,%d,%.3f,%d,%s,%d)", v.empty() ? "" : ",", i, i % 5, 1 + i % 3, (double)(i % 997) * 0.37 + 0.005,
                      (i % 2 ? -1 : 1) * (1 + i % 9), disc, i * 7 - 200000);
      v += x;
    }
    exec(db, "INSERT INTO rv VALUES " + v);
  }
  const std::string dlt =
      "CAST(ROUND(ROUND(ROUND(CAST(price AS DECIMAL(20,6)),2) - ROUND(ROUND(CAST(price AS DECIMAL(20,6)),2) * "
      "ROUND(CAST(disc AS DECIMAL(20,10)),4),2),2) * CAST(qty AS DECIMAL(20,6)),2) AS DECIMAL(10,2))";
  const std::vector<std::string> exprs = {
      dlt,
      "ROUND(CAST(price AS DECIMAL(20,6)), -1)",          // to tens: an integer
      "ROUND(CAST(price AS DECIMAL(10,2)), 4)",           // more places than it has: padded to 4
      "ROUND(ic, 2)",                                     // an integer stays one
      "TRUNCATE(CAST(price AS DECIMAL(20,6)) * CAST(qty AS DECIMAL(20,6)), 1)",
      "ROUND(CAST(price AS DECIMAL(20,6)) * CAST(disc AS DECIMAL(20,10)), 3)"};
  for (const std::string& ex : exprs) {
    // the rows' own values, summed here exactly
    const SqlQueryResult rows = run(db, "SELECT g, " + ex + " FROM rv");
    std::map<std::string, std::pair<__int128, int>> want;
    for (const auto& r : rows.rows) {
      __int128 v = 0;
      int sc = 0;
      if (!parse_scaled(sql_display(r.values[1]), &v, &sc)) continue;   // NULL
      auto& w = want[sql_display(r.values[0])];
      w.first += v;
      w.second = sc;
    }
    const std::string q = "SELECT rv.g, SUM(" + ex + ") FROM rv JOIN rg ON rg.g = rv.g AND rg.active = 1 GROUP BY rv.g ORDER BY rv.g";
    const SqlQueryResult got = run(db, q);
    expect(got.rows.size() == want.size(), "round lanes: " + std::to_string(want.size()) + " groups for " + ex);
    for (const auto& r : got.rows) {
      const auto it = want.find(sql_display(r.values[0]));
      const std::string w = it == want.end() ? "?" : scaled_text(it->second.first, it->second.second);
      expect(sql_display(r.values[1]) == w, "round lanes: group " + sql_display(r.values[0]) + " SUM(" + ex.substr(0, 60) + ") = " +
                                                 sql_display(r.values[1]) + ", the rows add up to " + w);
    }
    if (g_no_star) expect(plan_has(db, q, "group by [lanes"), "round lanes: grouped over lanes: SUM(" + ex.substr(0, 60) + ")");
  }
  // through a view, as the report reads it: SUM(col * -1) and a month pivot
  exec(db, "CREATE VIEW rvv AS SELECT rv.g AS g, m, qty, rg.label AS label, " + dlt + " AS dlt FROM rv JOIN rg ON rg.g = rv.g AND rg.active = 1");
  const SqlQueryResult per = run(db, "SELECT g, m, dlt FROM rvv");
  std::map<std::string, __int128> by_g, by_g_m1;
  for (const auto& r : per.rows) {
    __int128 v = 0;
    int sc = 0;
    if (!parse_scaled(sql_display(r.values[2]), &v, &sc)) continue;
    by_g[sql_display(r.values[0])] -= v;
    if (sql_display(r.values[1]) == "1") by_g_m1[sql_display(r.values[0])] += v;
  }
  const std::string qv = "SELECT g, SUM(dlt * -1), SUM(CASE WHEN m = 1 THEN dlt ELSE 0 END) FROM rvv GROUP BY g ORDER BY g";
  const SqlQueryResult gv = run(db, qv);
  for (const auto& r : gv.rows) {
    const std::string g = sql_display(r.values[0]);
    expect(sql_display(r.values[1]) == scaled_text(by_g[g], 2), "round lanes: view group " + g + " SUM(dlt * -1) = " + sql_display(r.values[1]) +
                                                                     ", rows " + scaled_text(by_g[g], 2));
    expect(sql_display(r.values[2]) == scaled_text(by_g_m1[g], 2), "round lanes: view group " + g + " month 1 = " + sql_display(r.values[2]) +
                                                                       ", rows " + scaled_text(by_g_m1[g], 2));
  }
  if (g_no_star) expect(plan_has(db, qv, "group by [lanes"), "round lanes: the view's SUMs grouped over lanes");
}


// A statement's plan holds no row-at-a-time join step: the joins ran as a star join (or were pruned away).
bool plan_has_none(SqlDatabase& db, const std::string& sql, const std::string& needle) {
  SqlQueryResult r = db.execute_local_query("EXPLAIN " + sql);
  if (!r.success) return false;
  bool found = false;
  for (const auto& row : r.rows)
    if (!row.values.empty() && sql_display(row.values[0]).find(needle) != std::string::npos) found = true;
  if (found) for (const auto& row : r.rows) if (!row.values.empty()) std::printf("      plan: %s\n", sql_display(row.values[0]).c_str());
  return !found;
}

// A report view's shape (stocktrans): a four-part key into a header table (INT parts and text parts; one key twice,
// so the link is one-to-many), a text key into a table with several rows per key, and LEFT JOINs the report never
// reads - on a primary key (dropped: the star join takes at most 8 sources, and the view brings 9 here), and on a key
// that is not unique (kept: it multiplies the rows). Every figure is worked out here from the rows inserted.
void check_star_links(SqlDatabase& db, const char* when) {
  struct F { int id, tno, ty; std::string dno, br, sid; int qty; };
  struct H { int tno, ty; std::string dno, br; int amt; };
  const int N = 6000;
  std::vector<F> fact;
  for (int i = 0; i < N; ++i)
    fact.push_back({i, 1000 + i / 3, 10 + i % 2, "D" + std::to_string(i % 37), "B" + std::to_string(i % 5), "S" + std::to_string(i % 50), 1 + i % 7});
  std::vector<H> hdr;
  for (const F& f : fact) if (f.id % 11 != 0) hdr.push_back({f.tno, f.ty, f.dno, f.br, 100 + f.id % 13});
  hdr.push_back({fact[7].tno, fact[7].ty, fact[7].dno, fact[7].br, 5000});   // fact row 7's key twice
  std::vector<std::pair<std::string, int>> prod;
  for (int k = 0; k < 40; ++k) prod.push_back({"S" + std::to_string(k), 100 + k});
  for (int k = 10; k < 15; ++k) prod.push_back({"S" + std::to_string(k), 900 + k});   // S10..S14: two rows each; S40..S49: none
  exec(db, "DROP VIEW IF EXISTS slv");
  for (const char* t : {"sl_fact", "sl_hdr", "sl_prod", "sl_deadu", "sl_deadm"}) exec(db, std::string("DROP TABLE IF EXISTS ") + t);
  exec(db, "CREATE TABLE sl_fact (id INT PRIMARY KEY, tno INT, ty SMALLINT, dno VARCHAR(10), br VARCHAR(10), sid VARCHAR(10), qty INT)");
  exec(db, "CREATE TABLE sl_hdr (tno INT, ty SMALLINT, dno VARCHAR(10), br VARCHAR(10), amt INT)");
  exec(db, "CREATE TABLE sl_prod (sid VARCHAR(10), mid INT)");
  exec(db, "CREATE TABLE sl_deadu (id INT PRIMARY KEY, z INT)");
  exec(db, "CREATE TABLE sl_deadm (id INT, z INT)");
  auto q = [](const std::string& v) { return "'" + v + "'"; };
  for (int b = 0; b < N; b += 1000) {
    std::string v;
    for (int i = b; i < b + 1000; ++i) {
      const F& f = fact[(size_t)i];
      v += std::string(v.empty() ? "" : ",") + "(" + std::to_string(f.id) + "," + std::to_string(f.tno) + "," + std::to_string(f.ty) + "," + q(f.dno) + "," +
           q(f.br) + "," + q(f.sid) + "," + std::to_string(f.qty) + ")";
    }
    exec(db, "INSERT INTO sl_fact VALUES " + v);
  }
  std::string v;
  for (const H& h : hdr)
    v += std::string(v.empty() ? "" : ",") + "(" + std::to_string(h.tno) + "," + std::to_string(h.ty) + "," + q(h.dno) + "," + q(h.br) + "," + std::to_string(h.amt) + ")";
  exec(db, "INSERT INTO sl_hdr VALUES " + v);
  v.clear();
  for (const auto& p : prod) v += std::string(v.empty() ? "" : ",") + "(" + q(p.first) + "," + std::to_string(p.second) + ")";
  exec(db, "INSERT INTO sl_prod VALUES " + v);
  v.clear();
  for (int i = 0; i < N / 2; ++i) v += std::string(v.empty() ? "" : ",") + "(" + std::to_string(i) + "," + std::to_string(i * 3) + ")";
  exec(db, "INSERT INTO sl_deadu VALUES " + v);
  v.clear();
  for (int i = 0; i < 100; ++i) v += std::string(v.empty() ? "" : ",") + "(" + std::to_string(i) + ",1),(" + std::to_string(i) + ",2)";
  exec(db, "INSERT INTO sl_deadm VALUES " + v);
  db.optimize_all_tables("gexdb");   // the columns as a loaded table has them

  std::string from = "FROM sl_fact f JOIN sl_hdr h ON h.tno = f.tno AND h.ty = f.ty AND h.dno = f.dno AND h.br = f.br "
                     "LEFT JOIN sl_prod p ON p.sid = f.sid";
  for (int d = 1; d <= 6; ++d) from += " LEFT JOIN sl_deadu d" + std::to_string(d) + " ON d" + std::to_string(d) + ".id = f.id";
  from += " LEFT JOIN sl_deadm m ON m.id = f.id";
  exec(db, "CREATE VIEW slv AS SELECT p.mid AS mid, f.qty AS qty, h.amt AS amt " + from);
  // per mid: rows, SUM(qty), SUM(amt) - each fact row against every header, product and dead-m row it meets
  std::map<std::string, std::array<long long, 3>> want;
  for (const F& f : fact) {
    std::vector<int> amts, mids;
    for (const H& h : hdr) if (h.tno == f.tno && h.ty == f.ty && h.dno == f.dno && h.br == f.br) amts.push_back(h.amt);
    for (const auto& p : prod) if (p.first == f.sid) mids.push_back(p.second);
    const size_t nm = f.id < 100 ? 2 : 1;
    for (int a : amts)
      for (size_t pi = 0; pi < std::max<size_t>(mids.size(), 1); ++pi) {
        auto& w = want[mids.empty() ? "NULL" : std::to_string(mids[pi])];
        w[0] += (long long)nm; w[1] += (long long)nm * f.qty; w[2] += (long long)nm * a;
      }
  }
  const std::string qry = "SELECT mid, COUNT(*), SUM(qty), SUM(amt) FROM slv GROUP BY mid ORDER BY mid";
  const SqlQueryResult got = run(db, qry);
  const std::string tag = std::string("star links (") + when + "): ";
  expect(got.rows.size() == want.size(), tag + std::to_string(got.rows.size()) + " groups, expected " + std::to_string(want.size()));
  for (const auto& r : got.rows) {
    std::string k = sql_is_null(r.values[0]) ? "NULL" : sql_display(r.values[0]);
    const auto it = want.find(k);
    if (it == want.end()) { expect(false, tag + "unexpected group " + k); continue; }
    expect(sql_display(r.values[1]) == std::to_string(it->second[0]) && sql_display(r.values[2]) == std::to_string(it->second[1]) &&
               sql_display(r.values[3]) == std::to_string(it->second[2]),
           tag + "group " + k + ": " + sql_display(r.values[1]) + "/" + sql_display(r.values[2]) + "/" + sql_display(r.values[3]) + ", expected " +
               std::to_string(it->second[0]) + "/" + std::to_string(it->second[1]) + "/" + std::to_string(it->second[2]));
  }
  // the unread joins on a primary key are gone from the plan, the one that multiplies stays - and no join is probed a row at a time
  expect(plan_has_none(db, qry, "source d1 "), tag + "the unread joins on a primary key are dropped");
  expect(plan_has(db, qry, "source m "), tag + "the unread join that multiplies rows is kept");
  expect(plan_has_none(db, qry, "index probe"), tag + "the joins ran as a star join");
  // and a join nothing reads on a unique key changes no count
  const SqlQueryResult cnt = run(db, "SELECT COUNT(*) FROM sl_fact f LEFT JOIN sl_deadu d ON d.id = f.id");
  expect(sql_display(cnt.rows[0].values[0]) == std::to_string(N), tag + "COUNT(*) over a LEFT JOIN on a primary key");
  const SqlQueryResult cnt2 = run(db, "SELECT COUNT(*) FROM sl_fact f LEFT JOIN sl_deadm d ON d.id = f.id");
  expect(sql_display(cnt2.rows[0].values[0]) == std::to_string(N + 100), tag + "COUNT(*) over a LEFT JOIN that multiplies");
}

int main(int argc, char** argv) {
  g_no_star = argc > 1 && std::string(argv[1]) == "--no-star";
  if (g_no_star) setenv("AIOS_SQL_STAR_JOIN", "0", 1);   // before the first statement reads it
  std::printf("=== ASQL grouped expression aggregate check%s ===\n", g_no_star ? " (no star join)" : "");
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
  if (g_no_star) {
    check_round_lanes(db);
    std::printf("  %d checks\n", checks);
    if (failures) { std::printf("=== %d FAILURE(S) ===\n", failures); return 1; }
    std::printf("=== ALL GROUPED EXPRESSION CHECKS PASSED ===\n");
    return 0;
  }
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
  check_pivot(db);
  check_pivot_shapes(db);
  check_in_subquery_lanes(db);
  check_star_multi_grouped(db);
  check_rle_parts(db);
  check_round_lanes(db);
  check_star_links(db, "after load");
  check_star_links(db, "again");
  std::printf("  %d checks\n", checks);
  if (failures) { std::printf("=== %d FAILURE(S) ===\n", failures); return 1; }
  std::printf("=== ALL GROUPED EXPRESSION CHECKS PASSED ===\n");
  return 0;
}
