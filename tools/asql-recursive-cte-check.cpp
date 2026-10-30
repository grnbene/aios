#undef NDEBUG
// GROUP_CONCAT's SEPARATOR and the recursive CTE's iteration limit, as MariaDB 10.11 has them. Every expected value
// below is what mariadb:10.11 answered for the same statement.
//
// SEPARATOR '' joins with nothing (it joined with ',', so a frame packed from bytes came out with a comma between
// every pixel). A recursive CTE stops after max_recursive_iterations (default 1000, settable per session) with
// warning 1931, which SHOW WARNINGS and @@warning_count report - it used to stop at 10000 without a word, a short
// answer that looked whole. Under strict sql_mode a statement that changes data, or a stored function or trigger,
// fails with error 1931 instead and writes nothing.
//
// A recursive CTE is reached through a view where it has to sit inside another statement (a function, a procedure,
// UPDATE's SET, a scalar subquery): a WITH inside a subquery is not parsed yet.
//
// Statements go through execute_gateway_query, the path a client's statement takes (the answer cache included).
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

#include "aios/kernel/kernel.hpp"
#include "aios/storage/aql.hpp"
#include "aios/userland/sql_database.hpp"
#include "aios/userland/task_ledger.hpp"

using namespace aios;
using namespace aios::userland;

namespace {

int g_failures = 0;
const std::string kWarn100 =
    "Query execution was interrupted. The query exceeded max_recursive_iterations = 100. The query result may be incomplete";

void fail(const std::string& label, const std::string& why) {
  std::cerr << "  [FAIL] " << label << ": " << why << "\n";
  g_failures++;
}

SqlQueryResult q(SqlDatabase& db, const std::string& sql) { return db.execute_gateway_query(sql, "d"); }

void run_ok(SqlDatabase& db, const std::string& sql) {
  SqlQueryResult r = q(db, sql);
  if (!r.success) fail("setup", r.error_message + "\n    " + sql);
}

// The first row's first cell.
void expect_cell(SqlDatabase& db, const std::string& label, const std::string& sql, const std::string& want) {
  SqlQueryResult r = q(db, sql);
  if (!r.success) return fail(label, r.error_message);
  if (r.rows.empty() || r.rows[0].values.empty()) return fail(label, "no row back");
  const std::string got = sql_display(r.rows[0].values[0]);
  if (got != want) return fail(label, "got '" + got + "', want '" + want + "'");
  std::cout << "  [ok] " << label << " = " << got << "\n";
}

void expect_error(SqlDatabase& db, const std::string& label, const std::string& sql, const std::string& want_part) {
  SqlQueryResult r = q(db, sql);
  if (r.success) return fail(label, "succeeded, want an error containing '" + want_part + "'");
  if (r.error_message.find(want_part) == std::string::npos)
    return fail(label, "error '" + r.error_message + "', want one containing '" + want_part + "'");
  std::cout << "  [ok] " << label << ": " << r.error_message << "\n";
}

// SHOW WARNINGS, each row as "Level|Code|Message", joined by '\n'.
std::string warnings(SqlDatabase& db) {
  SqlQueryResult r = q(db, "SHOW WARNINGS");
  if (!r.success) return "error: " + r.error_message;
  std::string out;
  for (const SqlRow& row : r.rows) {
    if (!out.empty()) out += "\n";
    for (size_t k = 0; k < row.values.size(); ++k) out += (k ? "|" : "") + sql_display(row.values[k]);
  }
  return out;
}

void expect_warnings(SqlDatabase& db, const std::string& label, const std::string& want) {
  const std::string got = warnings(db);
  if (got != want) return fail(label, "SHOW WARNINGS gave\n      '" + got + "'\n    want\n      '" + want + "'");
  std::cout << "  [ok] " << label << (want.empty() ? ": no warnings" : ": " + want.substr(0, 12) + "...") << "\n";
}

}  // namespace

int main() {
  std::cout << "=== asql GROUP_CONCAT SEPARATOR and recursive CTE limit ===\n";

  char root_dir[] = "/tmp/aios-recursive-cte-XXXXXX";
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
  TaskLedger ledger(kernel);
  ledger.init("tasks");
  SqlDatabase db(kernel, &ledger);
  db.init("sql_db");

  run_ok(db, "CREATE DATABASE d");
  run_ok(db, "CREATE TABLE t (a INT)");
  run_ok(db, "CREATE TABLE log (c INT)");
  run_ok(db, "CREATE TABLE g (k INT, v INT)");
  run_ok(db, "INSERT INTO g VALUES (1, 3), (1, 1), (1, 2), (2, 9), (2, 8)");
  const std::string s3 = "WITH RECURSIVE s(n) AS (SELECT 0 UNION ALL SELECT n+1 FROM s WHERE n<3) ";
  const std::string s500 = "WITH RECURSIVE s(n) AS (SELECT 0 UNION ALL SELECT n+1 FROM s WHERE n<500) ";

  std::cout << "\n[1] GROUP_CONCAT ... SEPARATOR\n";
  expect_cell(db, "SEPARATOR ''", s3 + "SELECT GROUP_CONCAT(n SEPARATOR '') FROM s", "0123");
  expect_cell(db, "ORDER BY ... SEPARATOR ''", s3 + "SELECT GROUP_CONCAT(n ORDER BY n DESC SEPARATOR '') FROM s", "3210");
  expect_cell(db, "SEPARATOR \"\"", "SELECT GROUP_CONCAT(x SEPARATOR \"\") FROM (SELECT 1 x UNION SELECT 2) t", "12");
  expect_cell(db, "SEPARATOR 0x00", s3 + "SELECT HEX(GROUP_CONCAT(n SEPARATOR 0x00)) FROM s", "30003100320033");
  expect_cell(db, "SEPARATOR X'2D2D'", "SELECT HEX(GROUP_CONCAT(x SEPARATOR X'2D2D')) FROM (SELECT 1 x UNION SELECT 2) t", "312D2D32");
  expect_cell(db, "no SEPARATOR is ','", "SELECT GROUP_CONCAT(x) FROM (SELECT 1 x UNION SELECT 2) t", "1,2");
  expect_cell(db, "DISTINCT ... SEPARATOR ''", "SELECT GROUP_CONCAT(DISTINCT v ORDER BY v SEPARATOR '') FROM (SELECT 1 v UNION ALL SELECT 1 UNION ALL SELECT 2) t", "12");
  expect_cell(db, "per group, SEPARATOR ''", "SELECT GROUP_CONCAT(v ORDER BY v SEPARATOR '') FROM g GROUP BY k ORDER BY k DESC", "89");
  expect_cell(db, "in a scalar subquery", "SELECT (SELECT GROUP_CONCAT(v ORDER BY v SEPARATOR '') FROM g WHERE k = 1)", "123");
  // The same text again, and a third time: the answer cache keeps the separator it was computed with.
  for (int i = 0; i < 3; ++i)
    expect_cell(db, "repeated (cache) #" + std::to_string(i + 1), "SELECT GROUP_CONCAT(v ORDER BY v SEPARATOR '') FROM g WHERE k = 2", "89");
  run_ok(db, "CREATE VIEW vsep AS " + s3 + "SELECT GROUP_CONCAT(n SEPARATOR '') c FROM s");
  expect_cell(db, "in a view", "SELECT c FROM vsep", "0123");
  run_ok(db, "CREATE FUNCTION gsep() RETURNS TEXT RETURN (SELECT GROUP_CONCAT(v ORDER BY v DESC SEPARATOR '') FROM g WHERE k = 1)");
  expect_cell(db, "in a stored function", "SELECT gsep()", "321");
  expect_error(db, "SEPARATOR takes a string literal", "SELECT GROUP_CONCAT(x SEPARATOR CONCAT('a','b')) FROM (SELECT 1 x UNION SELECT 2) t", "SEPARATOR");

  std::cout << "\n[2] max_recursive_iterations\n";
  expect_cell(db, "default", "SELECT @@max_recursive_iterations", "1000");
  expect_cell(db, "64000 rows at the default", "WITH RECURSIVE s(n) AS (SELECT 0 UNION ALL SELECT n+1 FROM s WHERE n<63999) SELECT COUNT(*) FROM s", "1001");
  expect_warnings(db, "warned at the default",
                  "Warning|1931|Query execution was interrupted. The query exceeded max_recursive_iterations = 1000. The query result may be incomplete");
  run_ok(db, "SET SESSION max_recursive_iterations = 1000000");
  expect_cell(db, "set", "SELECT @@max_recursive_iterations", "1000000");
  expect_cell(db, "all 64000 rows", "WITH RECURSIVE s(n) AS (SELECT 0 UNION ALL SELECT n+1 FROM s WHERE n<63999) SELECT SUM(n) FROM s", "2047968000");
  expect_warnings(db, "a whole answer", "");

  run_ok(db, "SET SESSION max_recursive_iterations = 100");
  expect_cell(db, "n<99: 99 iterations", "WITH RECURSIVE s(n) AS (SELECT 0 UNION ALL SELECT n+1 FROM s WHERE n<99) SELECT COUNT(*) FROM s", "100");
  expect_cell(db, "... no warning", "SELECT @@warning_count", "0");
  expect_cell(db, "n<100: the 100th still made rows", "WITH RECURSIVE s(n) AS (SELECT 0 UNION ALL SELECT n+1 FROM s WHERE n<100) SELECT COUNT(*) FROM s", "101");
  expect_cell(db, "... warned", "SELECT @@warning_count", "1");
  expect_cell(db, "n<500", s500 + "SELECT COUNT(*) FROM s", "101");
  expect_warnings(db, "the warning", "Warning|1931|" + kWarn100);
  expect_cell(db, "SHOW COUNT(*) WARNINGS", "SHOW COUNT(*) WARNINGS", "1");
  expect_warnings(db, "SHOW WARNINGS keeps them", "Warning|1931|" + kWarn100);
  expect_cell(db, "a statement reading a table", "SELECT COUNT(*) FROM t", "0");
  expect_warnings(db, "... clears them", "");
  // Repeated, so the second and third come from wherever the answer cache would serve them: each still warns.
  for (int i = 0; i < 3; ++i) {
    expect_cell(db, "repeated #" + std::to_string(i + 1), s500 + "SELECT COUNT(*) FROM s", "101");
    expect_warnings(db, "repeated #" + std::to_string(i + 1) + " warned", "Warning|1931|" + kWarn100);
  }
  expect_cell(db, "UNION (distinct) cycle ends by itself", "WITH RECURSIVE s(n) AS (SELECT 0 UNION SELECT (n+1)%5 FROM s) SELECT COUNT(*) FROM s", "5");
  expect_warnings(db, "... no warning", "");
  expect_cell(db, "UNION (distinct) past the limit", "WITH RECURSIVE s(n) AS (SELECT 0 UNION SELECT n+1 FROM s WHERE n<500) SELECT COUNT(*) FROM s", "101");
  expect_warnings(db, "... warned", "Warning|1931|" + kWarn100);
  expect_cell(db, "two CTEs past it",
              "WITH RECURSIVE a(n) AS (SELECT 0 UNION ALL SELECT n+1 FROM a WHERE n<500), b(n) AS (SELECT 0 UNION ALL SELECT n+1 FROM b WHERE n<500) "
              "SELECT (SELECT COUNT(*) FROM a) + (SELECT COUNT(*) FROM b)", "202");
  expect_warnings(db, "... one warning each", "Warning|1931|" + kWarn100 + "\nWarning|1931|" + kWarn100);
  run_ok(db, "CREATE VIEW vrec AS " + s500 + "SELECT COUNT(*) c FROM s");
  expect_cell(db, "in a view", "SELECT c FROM vrec", "101");
  expect_warnings(db, "... warned", "Warning|1931|" + kWarn100);
  expect_cell(db, "a view in a scalar subquery", "SELECT (SELECT c FROM vrec) + 0", "101");
  expect_warnings(db, "... warned", "Warning|1931|" + kWarn100);

  // Another session with its own limit, after this one's answers: none of them is served to it.
  {
    std::string got_whole, got_cut, warn_cut;
    std::thread other([&] {
      run_ok(db, "SET SESSION max_recursive_iterations = 1000000");
      SqlQueryResult r = q(db, s500 + "SELECT COUNT(*) FROM s");
      got_whole = r.success && !r.rows.empty() ? sql_display(r.rows[0].values[0]) : r.error_message;
      SqlQueryResult v = q(db, "SELECT c FROM vrec");
      got_whole += "/" + (v.success && !v.rows.empty() ? sql_display(v.rows[0].values[0]) : v.error_message);
    });
    other.join();
    if (got_whole == "501/501") std::cout << "  [ok] a session with a higher limit gets the whole answer = " << got_whole << "\n";
    else fail("another session's limit", "got '" + got_whole + "', want '501/501'");
    // And back here, at 100, after the other session's whole answers: still cut, still warned.
    expect_cell(db, "this session after it", s500 + "SELECT COUNT(*) FROM s", "101");
    expect_warnings(db, "... warned", "Warning|1931|" + kWarn100);
  }

  run_ok(db, "SET SESSION max_recursive_iterations = 0");
  expect_cell(db, "limit 0: the anchor rows", "WITH RECURSIVE s(n) AS (SELECT 0 UNION ALL SELECT n+1 FROM s WHERE n<5) SELECT COUNT(*) FROM s", "1");
  expect_warnings(db, "... warned",
                  "Warning|1931|Query execution was interrupted. The query exceeded max_recursive_iterations = 0. The query result may be incomplete");
  run_ok(db, "SET SESSION max_recursive_iterations = -1");
  expect_warnings(db, "-1 is cut to 0", "Warning|1292|Truncated incorrect max_recursive_iterations value: '-1'");
  expect_cell(db, "... to 0", "SELECT @@max_recursive_iterations", "0");
  expect_error(db, "'abc' is refused", "SET SESSION max_recursive_iterations = 'abc'", "Incorrect argument type to variable 'max_recursive_iterations'");
  expect_error(db, "'7' is refused", "SET SESSION max_recursive_iterations = '7'", "Incorrect argument type to variable 'max_recursive_iterations'");
  expect_error(db, "1.5 is refused", "SET SESSION max_recursive_iterations = 1.5", "Incorrect argument type to variable 'max_recursive_iterations'");
  run_ok(db, "SET SESSION max_recursive_iterations = 5000000000");
  expect_warnings(db, "past the maximum", "Warning|1292|Truncated incorrect max_recursive_iterations value: '5000000000'");
  expect_cell(db, "... is the maximum", "SELECT @@max_recursive_iterations", "4294967295");
  run_ok(db, "SET GLOBAL max_recursive_iterations = 50");
  expect_cell(db, "SET GLOBAL leaves the session's", "SELECT @@max_recursive_iterations", "4294967295");
  expect_cell(db, "... sets the global", "SELECT @@global.max_recursive_iterations", "50");
  expect_cell(db, "... @@session. reads the session's", "SELECT @@session.max_recursive_iterations", "4294967295");
  run_ok(db, "SET SESSION max_recursive_iterations = DEFAULT");
  expect_cell(db, "DEFAULT is the global", "SELECT @@max_recursive_iterations", "50");
  expect_cell(db, "... and is used", "WITH RECURSIVE s(n) AS (SELECT 0 UNION ALL SELECT n+1 FROM s WHERE n<5000) SELECT COUNT(*) FROM s", "51");
  expect_cell(db, "@@warning_count", "SELECT @@warning_count", "1");
  expect_cell(db, "@@error_count", "SELECT @@error_count", "0");
  expect_warnings(db, "SHOW ERRORS has none", "Warning|1931|Query execution was interrupted. The query exceeded max_recursive_iterations = 50. The query result may be incomplete");
  {
    SqlQueryResult r = q(db, "SHOW ERRORS");
    if (r.success && r.rows.empty()) std::cout << "  [ok] SHOW ERRORS: none\n";
    else fail("SHOW ERRORS", r.success ? std::to_string(r.rows.size()) + " row(s)" : r.error_message);
  }
  run_ok(db, "SET GLOBAL max_recursive_iterations = DEFAULT");
  run_ok(db, "SET SESSION max_recursive_iterations = DEFAULT");
  expect_cell(db, "both DEFAULT", "SELECT @@max_recursive_iterations", "1000");

  std::cout << "\n[3] where the limit is an error\n";
  run_ok(db, "SET SESSION max_recursive_iterations = 100");
  run_ok(db, "CREATE FUNCTION frec() RETURNS INT RETURN (SELECT c FROM vrec)");
  run_ok(db, "CREATE PROCEDURE prec() BEGIN SELECT c FROM vrec; END");
  // (Not checked: a function reading the recursive view without strict mode. MariaDB fails it with 1931 there, yet
  // only warns for the same CTE written inline in the function - which this engine cannot parse yet.)
  run_ok(db, "SET SESSION sql_mode = ''");
  run_ok(db, "INSERT INTO log " + s500 + "SELECT n FROM s");
  expect_warnings(db, "not strict: INSERT ... SELECT warns", "Warning|1931|" + kWarn100);
  expect_cell(db, "... and inserts what there is", "SELECT COUNT(*) FROM log", "101");
  run_ok(db, "DELETE FROM log");

  run_ok(db, "SET SESSION sql_mode = 'STRICT_TRANS_TABLES'");
  expect_error(db, "strict: a function fails", "SELECT frec()", kWarn100);
  expect_error(db, "strict: INSERT ... SELECT fails", "INSERT INTO log " + s500 + "SELECT n FROM s", kWarn100);
  expect_cell(db, "... and inserts nothing", "SELECT COUNT(*) FROM log", "0");
  run_ok(db, "INSERT INTO t VALUES (1)");
  expect_error(db, "strict: UPDATE fails", "UPDATE t SET a = (SELECT c FROM vrec)", kWarn100);
  expect_cell(db, "... and changes nothing", "SELECT a FROM t", "1");
  run_ok(db, "DELETE FROM t WHERE a < (SELECT c FROM vrec)");
  expect_warnings(db, "strict: DELETE only warns", "Warning|1931|" + kWarn100);
  expect_cell(db, "... and deletes", "SELECT COUNT(*) FROM t", "0");
  expect_error(db, "strict: CREATE TABLE ... SELECT fails", "CREATE TABLE c2 AS " + s500 + "SELECT n FROM s", kWarn100);
  expect_error(db, "... and makes no table", "SELECT COUNT(*) FROM c2", "c2");
  expect_cell(db, "strict: a plain SELECT warns", s500 + "SELECT COUNT(*) FROM s", "101");
  expect_warnings(db, "... warned", "Warning|1931|" + kWarn100);
  run_ok(db, "SELECT c INTO @v FROM vrec");
  expect_warnings(db, "strict: SELECT ... INTO @v warns", "Warning|1931|" + kWarn100);
  expect_cell(db, "... and sets it", "SELECT @v", "101");
  {
    SqlQueryResult r = q(db, "CALL prec()");
    if (r.success && !r.rows.empty() && sql_display(r.rows[0].values[0]) == "101") std::cout << "  [ok] strict: CALL of a procedure = 101\n";
    else fail("strict: CALL of a procedure", r.success ? (r.rows.empty() ? "no row" : sql_display(r.rows[0].values[0])) : r.error_message);
  }
  expect_warnings(db, "... warns", "Warning|1931|" + kWarn100);

  std::error_code ec;
  std::filesystem::remove_all(root_dir, ec);
  std::cout << "\n=== " << (g_failures == 0 ? "ALL CHECKS PASSED" : "FAILURES: " + std::to_string(g_failures)) << " ===\n";
  return g_failures == 0 ? 0 : 1;
}
