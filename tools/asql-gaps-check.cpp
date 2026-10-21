#include <vector>
#undef NDEBUG
// Drives the ten gaps found against the live ERP on chi-il-1: derived tables in
// FROM/JOIN, correlated EXISTS, correlated subqueries against a joined table,
// DROP DATABASE, per-session transactions, and the durability path. Each case
// is the shape of a clause the ERP actually issues, not a synthetic one.
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

#include "aios/kernel/kernel.hpp"
#include "aios/storage/object_store.hpp"
#include "aios/userland/sql_database.hpp"

using namespace aios;
using namespace aios::userland;

namespace {

int g_failures = 0;
int g_checks = 0;

void run_ok(SqlDatabase& db, const std::string& sql) {
  SqlQueryResult r = db.execute_query(sql);
  if (!r.success) {
    std::cerr << "  setup FAILED: " << r.error_message << "\n    " << sql << "\n";
    g_failures++;
  }
}

void expect_rows(SqlDatabase& db, const std::string& label, const std::string& sql, size_t want) {
  g_checks++;
  SqlQueryResult r = db.execute_query(sql);
  if (!r.success) {
    std::cerr << "  [FAIL] " << label << ": " << r.error_message << "\n";
    g_failures++;
    return;
  }
  if (r.rows.size() != want) {
    std::cerr << "  [FAIL] " << label << ": got " << r.rows.size() << " rows, want " << want << "\n";
    g_failures++;
    return;
  }
  std::cout << "  [ok] " << label << " (" << r.rows.size() << " rows)\n";
}

void expect_cell(SqlDatabase& db, const std::string& label, const std::string& sql,
                 size_t col, const std::string& want) {
  g_checks++;
  SqlQueryResult r = db.execute_query(sql);
  if (!r.success) {
    std::cerr << "  [FAIL] " << label << ": " << r.error_message << "\n";
    g_failures++;
    return;
  }
  if (r.rows.empty() || col >= r.rows[0].values.size()) {
    std::cerr << "  [FAIL] " << label << ": no row/column back\n";
    g_failures++;
    return;
  }
  std::string got = sql_display(r.rows[0].values[col]);
  if (got != want) {
    std::cerr << "  [FAIL] " << label << ": got '" << got << "', want '" << want << "'\n";
    g_failures++;
    return;
  }
  std::cout << "  [ok] " << label << " = " << got << "\n";
}

void expect_error(SqlDatabase& db, const std::string& label, const std::string& sql) {
  g_checks++;
  SqlQueryResult r = db.execute_query(sql);
  if (r.success) {
    std::cerr << "  [FAIL] " << label << ": succeeded, should have refused\n";
    g_failures++;
    return;
  }
  std::cout << "  [ok] " << label << " refused: " << r.error_message << "\n";
}

void seed(SqlDatabase& db) {
  run_ok(db, "CREATE TABLE stockmaster (stockid VARCHAR(20) PRIMARY KEY, description VARCHAR(100), categoryid VARCHAR(10))");
  run_ok(db, "INSERT INTO stockmaster VALUES ('T1','Tire One','TIRE'),('T2','Tire Two','TIRE'),('O1','Oil Filter','O'),('L1','Labor','L')");

  run_ok(db, "CREATE TABLE purchdata (stockid VARCHAR(20), supplierno VARCHAR(10), price DECIMAL(10,2))");
  run_ok(db, "INSERT INTO purchdata VALUES ('T1','ATD',80.00),('T2','ATD',95.00),('O1','NAPA',12.00)");

  run_ok(db, "CREATE TABLE stockmoves (stkmoveno INT PRIMARY KEY, stockid VARCHAR(20), loccode VARCHAR(5), qty DECIMAL(10,2), trandate VARCHAR(20))");
  run_ok(db, "INSERT INTO stockmoves VALUES (1,'T1','01',-2,'2026-09-05'),(2,'T2','01',-1,'2026-09-06'),(3,'O1','02',-4,'2026-09-07'),(4,'T1','01',-3,'2026-09-08')");
}

}  // namespace

int main() {
  char root_dir[] = "/tmp/aios-gaps-XXXXXX";
  if (!mkdtemp(root_dir)) {
    std::perror("mkdtemp");
    return 1;
  }
  KernelConfig cfg;
  cfg.root = root_dir;
  cfg.residency = test_config;
  auto k_res = Kernel::boot(cfg);
  if (!k_res.ok()) {
    std::cerr << "Failed to boot kernel: " << k_res.error().message << "\n";
    return 1;
  }
  Kernel& kernel = *k_res.value();
  SqlDatabase db(kernel);
  db.init("sql_db");
  seed(db);

  std::cout << "\n-- gap 5: derived table in JOIN --\n";
  expect_rows(db, "JOIN (SELECT ...) alias ON",
              "SELECT s.stockid, x.n FROM stockmaster s "
              "JOIN (SELECT stockid, COUNT(*) AS n FROM stockmoves GROUP BY stockid) x "
              "ON x.stockid = s.stockid", 3);
  expect_rows(db, "FROM (SELECT ...) alias",
              "SELECT t.stockid FROM (SELECT stockid FROM stockmaster WHERE categoryid = 'TIRE') t", 2);
  expect_rows(db, "LEFT JOIN (SELECT ...) alias ON",
              "SELECT s.stockid, x.n FROM stockmaster s "
              "LEFT JOIN (SELECT stockid, COUNT(*) AS n FROM stockmoves GROUP BY stockid) x "
              "ON x.stockid = s.stockid", 4);
  // A count coming back out of a derived table has to still be a number.
  // Materialised as text, "9" sorted above "10" and the biggest group came
  // last.
  run_ok(db, "CREATE TABLE movecount (id INT PRIMARY KEY, stockid VARCHAR(20), qty DECIMAL(10,2))");
  {
    std::string vals;
    for (int i = 1; i <= 12; ++i) {
      vals += (i > 1 ? "," : "") + std::string("(") + std::to_string(i) + ",'BIG',-1)";
    }
    for (int i = 13; i <= 21; ++i) {
      vals += ",(" + std::to_string(i) + ",'SMALL',-1)";
    }
    run_ok(db, "INSERT INTO movecount VALUES " + vals);
  }
  expect_cell(db, "derived COUNT orders as a number, not as text",
              "SELECT x.stockid FROM (SELECT stockid, COUNT(*) AS n FROM movecount GROUP BY stockid) x "
              "ORDER BY x.n DESC LIMIT 1", 0, "BIG");
  expect_cell(db, "derived COUNT compares as a number",
              "SELECT COUNT(*) FROM (SELECT stockid, COUNT(*) AS n FROM movecount GROUP BY stockid) x "
              "WHERE x.n > 9", 0, "1");
  expect_cell(db, "derived SUM keeps its sign and magnitude",
              "SELECT x.stockid FROM (SELECT stockid, SUM(qty) AS q FROM movecount GROUP BY stockid) x "
              "ORDER BY x.q ASC LIMIT 1", 0, "BIG");

  std::cout << "\n-- gap 6: correlated EXISTS in WHERE --\n";
  expect_rows(db, "EXISTS (SELECT 1 ... WHERE p.stockid = s.stockid)",
              "SELECT s.stockid FROM stockmaster s "
              "WHERE EXISTS (SELECT 1 FROM purchdata p WHERE p.stockid = s.stockid AND p.supplierno = 'ATD')", 2);
  expect_rows(db, "NOT EXISTS correlated",
              "SELECT s.stockid FROM stockmaster s "
              "WHERE NOT EXISTS (SELECT 1 FROM purchdata p WHERE p.stockid = s.stockid)", 1);

  expect_rows(db, "EXISTS correlated to a joined alias",
              "SELECT s.stockid FROM stockmaster s JOIN stockmoves m ON m.stockid = s.stockid "
              "WHERE EXISTS (SELECT 1 FROM purchdata p WHERE p.stockid = m.stockid)", 4);
  expect_rows(db, "EXISTS correlated to a joined alias, narrowed supplier",
              "SELECT s.stockid FROM stockmaster s JOIN stockmoves m ON m.stockid = s.stockid "
              "WHERE EXISTS (SELECT 1 FROM purchdata p WHERE p.stockid = m.stockid AND p.supplierno = 'ATD')", 3);
  expect_rows(db, "EXISTS correlated, same table as outer",
              "SELECT s.stockid FROM stockmaster s "
              "WHERE EXISTS (SELECT 1 FROM stockmaster s2 WHERE s2.categoryid = s.categoryid AND s2.stockid <> s.stockid)", 2);
  expect_rows(db, "EXISTS ANDed with another predicate",
              "SELECT s.stockid FROM stockmaster s WHERE s.categoryid = 'TIRE' "
              "AND EXISTS (SELECT 1 FROM purchdata p WHERE p.stockid = s.stockid)", 2);
  expect_rows(db, "EXISTS under OR",
              "SELECT s.stockid FROM stockmaster s WHERE s.categoryid = 'L' "
              "OR EXISTS (SELECT 1 FROM purchdata p WHERE p.stockid = s.stockid AND p.supplierno = 'ATD')", 3);

  std::cout << "\n-- gap 7: correlated subquery against the JOIN's own target --\n";
  expect_rows(db, "scalar subquery correlated to joined table",
              "SELECT s.stockid, m.qty, "
              "(SELECT COUNT(*) FROM stockmoves m2 WHERE m2.stockid = m.stockid) AS n "
              "FROM stockmaster s JOIN stockmoves m ON m.stockid = s.stockid", 4);
  expect_rows(db, "subquery over the joined table itself",
              "SELECT s.stockid, "
              "(SELECT SUM(m2.qty) FROM stockmoves m2 WHERE m2.stockid = s.stockid) AS tot "
              "FROM stockmaster s JOIN stockmoves m ON m.stockid = s.stockid", 4);
  expect_rows(db, "correlated subquery inside the ON clause",
              "SELECT s.stockid FROM stockmaster s JOIN stockmoves m "
              "ON m.stockid = s.stockid AND m.qty = (SELECT MIN(m3.qty) FROM stockmoves m3 WHERE m3.stockid = s.stockid)", 3);
  expect_rows(db, "IN (SELECT) correlated to joined alias",
              "SELECT s.stockid FROM stockmaster s JOIN stockmoves m ON m.stockid = s.stockid "
              "WHERE m.stockid IN (SELECT p.stockid FROM purchdata p WHERE p.supplierno = 'ATD')", 3);

  std::cout << "\n-- gap 10: DROP DATABASE --\n";
  run_ok(db, "CREATE DATABASE tenant_b");
  run_ok(db, "DROP DATABASE tenant_b");
  {
    g_checks++;
    bool still_there = false;
    for (const auto& d : db.list_databases()) {
      if (d == "tenant_b") still_there = true;
    }
    if (still_there) {
      std::cerr << "  [FAIL] DROP DATABASE left tenant_b in SHOW DATABASES\n";
      g_failures++;
    } else {
      std::cout << "  [ok] DROP DATABASE removed the schema\n";
    }
  }

  std::cout << "\n-- gap 9: multi-tenant isolation --\n";
  run_ok(db, "CREATE DATABASE tenant_x");
  run_ok(db, "CREATE TABLE tenant_x.secrets (id INT, val VARCHAR(20))");
  run_ok(db, "INSERT INTO tenant_x.secrets VALUES (1,'private')");
  run_ok(db, "USE aios_db");
  std::cout << "  (effective database now: " << db.effective_database() << ")\n";
  expect_error(db, "unqualified name must not reach another schema",
               "SELECT * FROM secrets");
  expect_error(db, "qualified read of another schema's table still resolves",
               "SELECT * FROM tenant_b.stockmaster");

  std::cout << "\n-- gap 8: a query it cannot answer must not answer emptily --\n";
  expect_error(db, "unknown function", "SELECT NOT_A_FUNCTION(stockid) FROM stockmaster");
  expect_error(db, "unknown column in WHERE", "SELECT stockid FROM stockmaster WHERE no_such_col = 1");
  expect_error(db, "unknown column in a subquery", "SELECT stockid FROM stockmaster WHERE stockid IN (SELECT nope FROM purchdata)");
  expect_error(db, "table missing inside a scalar subquery",
               "SELECT stockid, (SELECT COUNT(*) FROM no_such_table t WHERE t.stockid = stockmaster.stockid) FROM stockmaster");
  expect_error(db, "unknown table in a JOIN", "SELECT s.stockid FROM stockmaster s JOIN nowhere n ON n.stockid = s.stockid");
  expect_error(db, "unknown column in GROUP BY", "SELECT COUNT(*) FROM stockmaster GROUP BY missing_col");
  expect_error(db, "unknown column in ORDER BY", "SELECT stockid FROM stockmaster ORDER BY missing_col");
  expect_error(db, "unknown column in a derived table", "SELECT x.a FROM (SELECT bogus AS a FROM stockmaster) x");

  std::cout << "\n-- gap 1: the store is not rewritten per statement --\n";
  {
    g_checks++;
    std::string store = std::string(root_dir) + "/sql_db/schema_tables.db";
    db.flush_now();
    struct stat sb0 {};
    ::stat(store.c_str(), &sb0);
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; ++i) {
      run_ok(db, "INSERT INTO stockmoves VALUES (" + std::to_string(1000 + i) +
                 ",'T1','01',-1,'2026-09-09')");
    }
    auto t1 = std::chrono::steady_clock::now();
    struct stat sb1 {};
    ::stat(store.c_str(), &sb1);
    bool rewritten = (sb0.st_mtime != sb1.st_mtime) || (sb0.st_size != sb1.st_size);
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::cout << "  200 inserts in " << ms << " ms, store rewritten: "
              << (rewritten ? "yes" : "no") << "\n";
    if (rewritten) {
      std::cerr << "  [FAIL] the full store was rewritten inside the run\n";
      g_failures++;
    } else {
      std::cout << "  [ok] statements only appended to the redo log\n";
    }
  }

  std::cout << "\n-- gap 3: per-session transactions --\n";
  {
    g_checks++;
    std::atomic<bool> second_began{false};
    std::string err2;
    db.execute_query("BEGIN");
    std::thread other([&]() {
      SqlQueryResult r = db.execute_query("BEGIN");
      second_began = r.success;
      if (!r.success) err2 = r.error_message;
      db.execute_query("ROLLBACK");
    });
    db.execute_query("INSERT INTO stockmaster VALUES ('TX1','In txn','TIRE')");
    db.execute_query("ROLLBACK");
    other.join();
    if (!second_began) {
      std::cerr << "  [FAIL] a second session could not BEGIN: " << err2 << "\n";
      g_failures++;
    } else {
      std::cout << "  [ok] two sessions each held their own transaction\n";
    }
  }
  expect_rows(db, "ROLLBACK undid only its own insert",
              "SELECT stockid FROM stockmaster WHERE stockid = 'TX1'", 0);
  {
    db.execute_query("BEGIN");
    db.execute_query("INSERT INTO stockmaster VALUES ('TX2','Kept','TIRE')");
    db.execute_query("COMMIT");
    expect_rows(db, "COMMIT kept its insert",
                "SELECT stockid FROM stockmaster WHERE stockid = 'TX2'", 1);
  }

  std::cout << "\n-- durability: replay after an unclean stop --\n";
  size_t live_moves = 0;
  {
    SqlQueryResult r = db.execute_query("SELECT COUNT(*) FROM stockmoves");
    if (r.success && !r.rows.empty()) live_moves = (size_t)std::strtoul(r.rows[0].values[0].c_str(), nullptr, 10);
  }
  {
    g_checks++;
    // No flush_now() and no destructor: the redo log is all that carries the
    // 200 inserts, which is exactly the crash case.
    SqlDatabase reopened(kernel);
    reopened.init("sql_db");
    SqlQueryResult r = reopened.execute_query("SELECT COUNT(*) FROM stockmoves");
    size_t got = (r.success && !r.rows.empty())
                     ? (size_t)std::strtoul(r.rows[0].values[0].c_str(), nullptr, 10) : 0;
    if (got != live_moves) {
      std::cerr << "  [FAIL] after replay: " << got << " rows, want " << live_moves << "\n";
      g_failures++;
    } else {
      std::cout << "  [ok] redo replay restored all " << got << " rows\n";
    }
    g_checks++;
    bool tenant_x_back = false;
    for (const auto& d : reopened.list_databases()) {
      if (d == "tenant_x") tenant_x_back = true;
    }
    if (!tenant_x_back) {
      std::cerr << "  [FAIL] schema tenant_x did not survive the restart\n";
      g_failures++;
    } else {
      std::cout << "  [ok] schemas survive a restart\n";
    }
    g_checks++;
    SqlQueryResult leak = reopened.execute_query("SELECT * FROM secrets");
    if (leak.success) {
      std::cerr << "  [FAIL] after restart an unqualified name reached tenant_x\n";
      g_failures++;
    } else {
      std::cout << "  [ok] schema isolation survives a restart\n";
    }
  }

  // -- engine telemetry & live status checks --
  {
    std::cout << "\n-- engine telemetry & live status --\n";
    g_checks++;
    SqlQueryResult st = db.execute_query("SHOW ENGINE ASQL STATUS");
    if (!st.success || st.rows.empty()) {
      std::cerr << "  [FAIL] SHOW ENGINE ASQL STATUS: " << st.error_message << "\n";
      g_failures++;
    } else {
      std::cout << "  [ok] SHOW ENGINE ASQL STATUS returned report (" << st.rows[0].values[2].size() << " bytes)\n";
    }

    g_checks++;
    SqlQueryResult gs = db.execute_query("SHOW GLOBAL STATUS LIKE 'Asql%'");
    if (!gs.success || gs.rows.empty()) {
      std::cerr << "  [FAIL] SHOW GLOBAL STATUS LIKE 'Asql%': " << gs.error_message << "\n";
      g_failures++;
    } else {
      std::cout << "  [ok] SHOW GLOBAL STATUS LIKE 'Asql%' returned " << gs.rows.size() << " variables\n";
    }

    g_checks++;
    SqlQueryResult s = db.execute_query("SHOW STATUS LIKE 'Queries'");
    if (!s.success || s.rows.empty()) {
      std::cerr << "  [FAIL] SHOW STATUS LIKE 'Queries': " << s.error_message << "\n";
      g_failures++;
    } else {
      std::cout << "  [ok] SHOW STATUS LIKE 'Queries' = " << s.rows[0].values[1] << "\n";
    }
  }

  std::cout << "\n-- an INT column re-encoded at a checkpoint keeps its values --\n";
  {
    // A span of exactly 32 bits (-7 .. 2147483647) kept its frame-of-reference codes in data32, which the
    // encoder emptied after writing them: every row read back as -7 once the column was re-encoded.
    struct Case { const char* name; std::vector<long long> vals; };
    const std::vector<Case> cases = {
        {"ft31", {0, 1, -7, 100, 2147483640, -1, 42}},
        {"ft32", {0, 1, -7, 100, 2147483647, -1, 42}},
        {"ft33", {0, 1, -4294967296LL, 100, 4294967296LL, -1, 42}},
    };
    for (const Case& c : cases) {
      run_ok(db, std::string("CREATE TABLE ") + c.name + " (id INT PRIMARY KEY, i BIGINT)");
      std::string ins = std::string("INSERT INTO ") + c.name + " VALUES ";
      std::string want;
      for (size_t k = 0; k < c.vals.size(); ++k) {
        ins += (k ? "," : "") + std::string("(") + std::to_string(k + 1) + "," + std::to_string(c.vals[k]) + ")";
        want += (k ? "," : "") + std::to_string(c.vals[k]);
      }
      run_ok(db, ins);
      db.flush_now();
      g_checks++;
      SqlQueryResult r = db.execute_query(std::string("SELECT GROUP_CONCAT(i ORDER BY id) FROM ") + c.name);
      const std::string got = r.success && !r.rows.empty() && !r.rows[0].values.empty() ? r.rows[0].values[0] : "?";
      if (got != want) {
        std::cerr << "  [FAIL] " << c.name << " after a checkpoint: " << got << " (want " << want << ")\n";
        g_failures++;
      } else {
        std::cout << "  [ok] " << c.name << " values survive re-encoding\n";
      }
    }
  }

  // CREATE TABLE ... LIKE copies the definition, not the rows; CREATE TABLE
  // ... AS SELECT makes the query's columns and fills them.
  {
    run_ok(db, "CREATE TABLE ctsrc (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(20) NOT NULL, qty INT, KEY k_name (name))");
    run_ok(db, "INSERT INTO ctsrc (name, qty) VALUES ('a', 1), ('b', 2), ('a', 5)");
    run_ok(db, "CREATE TABLE ctlike LIKE ctsrc");
    expect_rows(db, "CREATE TABLE ... LIKE makes an empty table", "SELECT * FROM ctlike", 0);
    run_ok(db, "INSERT INTO ctlike (name, qty) VALUES ('z', 9)");
    expect_rows(db, "CREATE TABLE ... LIKE keeps AUTO_INCREMENT", "SELECT * FROM ctlike WHERE id = 1", 1);
    run_ok(db, "CREATE TABLE ctas AS SELECT name, SUM(qty) AS total, COUNT(*) AS n FROM ctsrc GROUP BY name");
    expect_rows(db, "CREATE TABLE ... AS SELECT fills the rows", "SELECT * FROM ctas", 2);
    expect_rows(db, "CREATE TABLE ... AS SELECT keeps the values", "SELECT * FROM ctas WHERE name = 'a' AND total = 6 AND n = 2", 1);
    run_ok(db, "CREATE TABLE ctsel SELECT id, name FROM ctsrc WHERE qty > 1");
    expect_rows(db, "CREATE TABLE ... SELECT without AS", "SELECT * FROM ctsel", 2);
  }

  // Two columns of one table compared (the vectorized ColCmp leaf), a
  // derived GROUP BY with the outer WHERE / ORDER BY / LIMIT pushed in, and
  // a big-to-small join started from the filtered small table.
  {
    run_ok(db, "CREATE TABLE ccbig (id INT PRIMARY KEY, a INT, b INT, x DOUBLE, y DOUBLE, k INT, KEY kk (k))");
    run_ok(db, "CREATE TABLE ccsmall (k INT PRIMARY KEY, tag VARCHAR(10), d INT, nm VARCHAR(10))");
    const int N = 70000;
    size_t a_lt_b = 0, x_ge_y = 0, y_null = 0;
    std::string ins;
    for (int i = 1; i <= N; ++i) {
      const int a = i % 100, b = (i * 7) % 100, k = i % 3000;
      const double x = (i % 13) * 0.5;
      const bool yn = i % 10 == 0;
      const double y = i % 17;
      if (a < b) ++a_lt_b;
      if (yn) ++y_null; else if (x >= y) ++x_ge_y;
      ins += (ins.empty() ? "" : ",") + std::string("(") + std::to_string(i) + "," + std::to_string(a) + "," + std::to_string(b) + "," +
             std::to_string(x) + "," + (yn ? std::string("NULL") : std::to_string(y)) + "," + std::to_string(k) + ")";
      if (i % 5000 == 0) { run_ok(db, "INSERT INTO ccbig VALUES " + ins); ins.clear(); }
    }
    std::string sins;
    for (int k = 0; k < 3000; ++k)
      sins += (k ? "," : "") + std::string("(") + std::to_string(k) + ",'" + (k % 2 ? "odd" : "even") + "'," + std::to_string(k % 50) + ",'n" +
              std::to_string(10000 + k) + "')";
    run_ok(db, "INSERT INTO ccsmall VALUES " + sins);
    db.flush_now();
    expect_cell(db, "column < column", "SELECT COUNT(*) FROM ccbig WHERE a < b", 0, std::to_string(a_lt_b));
    expect_cell(db, "NOT (column >= column)", "SELECT COUNT(*) FROM ccbig WHERE NOT (a >= b)", 0, std::to_string(a_lt_b));
    expect_cell(db, "DOUBLE column >= column, NULLs unknown", "SELECT COUNT(*) FROM ccbig WHERE x >= y", 0, std::to_string(x_ge_y));
    expect_cell(db, "NOT over a NULL comparison stays unknown", "SELECT COUNT(*) FROM ccbig WHERE NOT (x >= y)", 0,
                std::to_string((size_t)N - y_null - x_ge_y));
    // k = i % 3000 over 70000 rows: k in [1, 1000] 24 times, every other k 23.
    expect_cell(db, "derived GROUP BY, outer WHERE/ORDER/LIMIT pushed in",
                "SELECT x.k, x.n FROM (SELECT k, COUNT(*) n FROM ccbig GROUP BY k) x WHERE x.n > 23 ORDER BY x.n DESC, x.k LIMIT 1", 0, "1");
    expect_rows(db, "derived GROUP BY pushdown keeps the filter",
                "SELECT x.k FROM (SELECT k, COUNT(*) n FROM ccbig GROUP BY k) x WHERE x.n > 23", 1000);
    expect_cell(db, "derived GROUP BY pushdown with OFFSET",
                "SELECT x.k FROM (SELECT k, COUNT(*) n FROM ccbig GROUP BY k) x ORDER BY x.n, x.k DESC LIMIT 1 OFFSET 1", 0, "2998");
    expect_cell(db, "join started from the filtered small table",
                "SELECT COUNT(*) FROM ccbig b JOIN ccsmall s ON s.k = b.k WHERE s.d = 7", 0, std::to_string(20 * 24 + 40 * 23));
    expect_cell(db, "join from the filtered small table, grouped",
                "SELECT SUM(b.a) FROM ccbig b JOIN ccsmall s ON s.k = b.k WHERE s.d = 7 AND s.tag = 'odd'", 0, "44800");
    // ORDER BY a text column of the joined table (read by its dictionary rank)
    // .. LIMIT: the key must still be read from that table afterwards.
    expect_cell(db, "join ORDER BY the joined table's text, LIMIT",
                "SELECT b.id, s.tag FROM ccbig b JOIN ccsmall s ON s.k = b.k ORDER BY s.tag DESC, b.id LIMIT 3", 0, "1");
    expect_cell(db, "join ORDER BY the joined table's text, LIMIT, OFFSET",
                "SELECT b.id, s.tag FROM ccbig b JOIN ccsmall s ON s.k = b.k ORDER BY s.tag DESC, b.id LIMIT 1 OFFSET 2", 0, "5");
    expect_cell(db, "join ORDER BY the joined table's text, its column",
                "SELECT s.tag, b.id FROM ccbig b JOIN ccsmall s ON s.k = b.k ORDER BY s.tag, b.id DESC LIMIT 1", 0, "even");
    expect_cell(db, "join GROUP BY .. ORDER BY the joined table's text, LIMIT",
                "SELECT s.nm, SUM(b.a) FROM ccbig b JOIN ccsmall s ON s.k = b.k GROUP BY s.nm ORDER BY s.nm DESC LIMIT 1", 0, "n12999");
    expect_cell(db, "join GROUP BY .. ORDER BY the joined table's text, its sum",
                "SELECT s.nm, SUM(b.a) FROM ccbig b JOIN ccsmall s ON s.k = b.k GROUP BY s.nm ORDER BY s.nm LIMIT 1 OFFSET 1", 1,
                std::to_string([] { long long t = 0; for (int i = 1; i <= 70000; ++i) if (i % 3000 == 1) t += i % 100; return t; }()));
  }

  // A scalar subquery with several rows is error 1242, uncorrelated or not;
  // LIMIT 1 picks one. A per-key batch kept across statements sees changes.
  {
    expect_error(db, "scalar subquery of several rows", "SELECT (SELECT k FROM ccbig WHERE a = 1)");
    expect_error(db, "scalar subquery of several rows in WHERE",
                 "SELECT COUNT(*) FROM ccsmall WHERE k = (SELECT k FROM ccbig WHERE a = 1)");
    expect_error(db, "correlated scalar subquery of several rows",
                 "SELECT s.k, (SELECT b.id FROM ccbig b WHERE b.k = s.k) FROM ccsmall s");
    expect_cell(db, "scalar subquery with LIMIT 1", "SELECT (SELECT id FROM ccbig WHERE a = 1 ORDER BY id LIMIT 1)", 0, "1");
    const std::string q = "SELECT s.k, (SELECT COUNT(*) FROM ccbig b WHERE b.k = s.k) FROM ccsmall s WHERE s.k = 5";
    expect_cell(db, "per-key batch", q, 1, "24");
    expect_cell(db, "per-key batch, kept", q, 1, "24");
    const std::string qa = "SELECT SUM(c) FROM (SELECT (SELECT COUNT(*) FROM ccbig b WHERE b.k = s.k) c FROM ccsmall s) x";
    expect_cell(db, "per-key batch over every outer row", qa, 0, "70000");
    expect_cell(db, "per-key batch over every outer row, kept", qa, 0, "70000");
    run_ok(db, "INSERT INTO ccbig (id, a, b, x, y, k) VALUES (900001, 0, 0, 0, 0, 5)");
    expect_cell(db, "per-key batch after an insert", q, 1, "25");
    expect_cell(db, "per-key batch over every outer row after an insert", qa, 0, "70001");
    run_ok(db, "DELETE FROM ccbig WHERE id = 900001");
    expect_cell(db, "per-key batch after a delete", q, 1, "24");
    expect_cell(db, "per-key batch over every outer row after a delete", qa, 0, "70000");
    run_ok(db, "INSERT INTO ccsmall (k, tag, d, nm) VALUES (5000, 'odd', 1, 'n5000')");
    expect_cell(db, "per-key batch after an outer insert", qa, 0, "70000");
    run_ok(db, "DELETE FROM ccsmall WHERE k = 5000");
  }

  // Views built from views, kept and refreshed group by group across writes:
  // the refresh runs the view's query (which builds the inner view on the
  // same thread) outside the view cache's lock, and patches a copy it holds.
  {
    run_ok(db, "CREATE VIEW nv_base AS SELECT k, SUM(a) s, COUNT(*) n FROM ccbig GROUP BY k");
    run_ok(db, "CREATE VIEW nv_top AS SELECT n, COUNT(*) c, SUM(s) ss FROM nv_base GROUP BY n");
    const std::string direct = "SELECT n, COUNT(*), SUM(s) FROM (SELECT k, SUM(a) s, COUNT(*) n FROM ccbig GROUP BY k) x GROUP BY n ORDER BY n";
    auto same = [&](const std::string& label) {
      g_checks++;
      SqlQueryResult a = db.execute_query("SELECT * FROM nv_top ORDER BY n");
      SqlQueryResult b = db.execute_query(direct);
      bool ok = a.success && b.success && a.rows.size() == b.rows.size();
      for (size_t i = 0; ok && i < a.rows.size(); ++i) ok = a.rows[i].values == b.rows[i].values;
      if (!ok) { std::cerr << "  [FAIL] " << label << "\n"; g_failures++; }
      else std::cout << "  [ok] " << label << " (" << a.rows.size() << " rows)\n";
    };
    same("view of a view");
    same("view of a view, kept");
    run_ok(db, "UPDATE ccbig SET a = a + 1 WHERE k = 7");
    same("view of a view after an update");
    run_ok(db, "INSERT INTO ccbig (id, a, b, x, y, k) VALUES (900002, 5, 0, 0, 0, 7), (900003, 5, 0, 0, 0, 7)");
    same("view of a view after an insert");
    run_ok(db, "DELETE FROM ccbig WHERE id IN (900002, 900003)");
    same("view of a view after a delete");
    expect_rows(db, "join of the view and its view", "SELECT b.k FROM nv_base b JOIN nv_top t ON t.n = b.n WHERE b.k < 10", 10);
  }

  // UNION .. ORDER BY sorts by the union column's own type; a DECIMAL column
  // has one scale; a correlated subquery over a page of rows is batched by
  // their keys; an UPDATE that changes nothing reports 0 changed rows.
  {
    expect_cell(db, "union of text sorted as text", "SELECT '10' x UNION SELECT '9' ORDER BY 1", 0, "10");
    expect_cell(db, "union DECIMAL column at its widest scale", "SELECT 1.5 x UNION SELECT 2.25 ORDER BY 1", 0, "1.50");
    expect_cell(db, "union DECIMAL scale without ORDER BY", "SELECT 1.5 x UNION SELECT 2.25", 0, "1.50");
    expect_cell(db, "union ORDER BY name DESC, LIMIT, OFFSET",
                "SELECT 3 a, 'x' b UNION SELECT 1, 'y' UNION SELECT 2, 'y' ORDER BY b DESC, a LIMIT 1 OFFSET 1", 0, "2");
    expect_cell(db, "union NULL first ascending", "SELECT 'b' x UNION SELECT NULL UNION SELECT 'a' ORDER BY 1", 0, "NULL");
    for (int k : {8, 1500, 2999}) {   // (k = 7 was updated above)
      long long mx = -1, cnt = 0;
      for (int i = 1; i <= 70000; ++i) if (i % 3000 == k) { mx = std::max<long long>(mx, i % 100); ++cnt; }
      const std::string q = "SELECT s.k, (SELECT MAX(b.a) FROM ccbig b WHERE b.k = s.k), (SELECT COUNT(*) FROM ccbig b WHERE b.k = s.k) "
                            "FROM ccsmall s WHERE s.k BETWEEN " + std::to_string(k - 3 < 0 ? 0 : k - 3) + " AND " + std::to_string(k + 3) +
                            " ORDER BY s.k LIMIT 1 OFFSET " + std::to_string(k - 3 < 0 ? k : 3);
      expect_cell(db, "correlated MAX over a page of rows, k=" + std::to_string(k), q, 1, std::to_string(mx));
      expect_cell(db, "correlated COUNT over a page of rows, k=" + std::to_string(k), q, 2, std::to_string(cnt));
    }
    g_checks++;
    {
      SqlQueryResult u = db.execute_query("UPDATE ccsmall SET d = d WHERE k < 100");
      if (!u.success || u.rows_affected != 0 || u.rows_matched != 100) {
        std::cerr << "  [FAIL] UPDATE to the same values: affected " << u.rows_affected << ", matched " << u.rows_matched << "\n";
        g_failures++;
      } else {
        std::cout << "  [ok] UPDATE to the same values changes 0 of 100 matched rows\n";
      }
    }
    run_ok(db, "UPDATE ccsmall SET d = d + 1 WHERE k = 1");
    expect_cell(db, "UPDATE after a no-op UPDATE", "SELECT d FROM ccsmall WHERE k = 1", 0, "2");
    run_ok(db, "UPDATE ccsmall SET d = d - 1 WHERE k = 1");
  }

  // A cached answer is dropped by an INSERT ... SELECT into a table it reads
  // (through the gateway, where the server's result cache lives).
  {
    auto gw = [&](const std::string& label, const std::string& sql, size_t col, const std::string& want) {
      g_checks++;
      SqlQueryResult r = db.execute_gateway_query(sql);
      const std::string got = r.success && !r.rows.empty() && col < r.rows[0].values.size() ? r.rows[0].values[col] : r.error_message;
      if (!r.success || got != want) { std::cerr << "  [FAIL] " << label << ": got " << got << ", want " << want << "\n"; g_failures++; }
      else std::cout << "  [ok] " << label << "\n";
    };
    auto gw_ok = [&](const std::string& sql) {
      g_checks++;
      SqlQueryResult r = db.execute_gateway_query(sql);
      if (!r.success) { std::cerr << "  [FAIL] " << sql << ": " << r.error_message << "\n"; g_failures++; }
    };
    gw_ok("CREATE TABLE qc_ord (o INT PRIMARY KEY, c INT)");
    gw_ok("CREATE TABLE qc_det (o INT, ln INT, q INT)");
    gw_ok("INSERT INTO qc_ord VALUES (1, 5), (2, 5), (3, 6)");
    gw_ok("INSERT INTO qc_det VALUES (1, 1, 2), (2, 1, 3), (3, 1, 4)");
    std::this_thread::sleep_for(std::chrono::milliseconds(120));   // past the window in which a just-written table is not cached
    const std::string q = "SELECT COUNT(*), SUM(q) FROM qc_det WHERE o IN (SELECT o FROM qc_ord WHERE c = 5)";
    for (int i = 0; i < 3; ++i) gw("IN-subquery count before the insert (" + std::to_string(i) + ")", q, 0, "2");
    gw_ok("INSERT INTO qc_det (ln, o, q) SELECT 99, o, 7 FROM qc_ord WHERE c = 5");
    gw("cached count after INSERT ... SELECT", q, 0, "4");
    gw("cached sum after INSERT ... SELECT", q, 1, "19");
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    for (int i = 0; i < 3; ++i) gw("count cached again (" + std::to_string(i) + ")", q, 0, "4");
    gw_ok("UPDATE qc_det SET q = q + 1 WHERE ln = 99");
    gw("cached sum after UPDATE", q, 1, "21");
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    for (int i = 0; i < 3; ++i) gw("sum cached again (" + std::to_string(i) + ")", q, 1, "21");
    gw_ok("DELETE FROM qc_det WHERE ln = 99");
    gw("cached count after DELETE", q, 0, "2");
  }

  std::cout << "\n=== " << (g_checks - g_failures) << "/" << g_checks << " gap checks pass ===\n";
  return g_failures == 0 ? 0 : 1;
}
