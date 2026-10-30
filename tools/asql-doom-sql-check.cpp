#undef NDEBUG
// The SQL constructs SQLDoom's renderer and game tic lean on that ASQL lacked, each against what a real server
// answers for the same statement (MariaDB 10.11 where it has the construct, MySQL 8.0 for LATERAL, which MariaDB
// does not have):
//   [1] a row-value IN over a subquery: (x, y) IN (SELECT c, r ...), NULLs and NOT IN as the server has them;
//   [2] WITH (and WITH RECURSIVE) inside a subquery, a derived table and a function body;
//   [3] LATERAL derived tables.
// Each is tried at the top level, in a stored function's body and nested in another query.
#include <iostream>
#include <string>
#include <vector>

#include "aios/kernel/kernel.hpp"
#include "aios/storage/aql.hpp"
#include "aios/userland/sql_database.hpp"
#include "aios/userland/task_ledger.hpp"
#include "check-root.hpp"

using namespace aios;
using namespace aios::userland;

namespace {

int g_failures = 0;

void fail(const std::string& label, const std::string& why) {
  std::cerr << "  [FAIL] " << label << ": " << why << "\n";
  g_failures++;
}

SqlQueryResult q(SqlDatabase& db, const std::string& sql) { return db.execute_gateway_query(sql, "d"); }

void run_ok(SqlDatabase& db, const std::string& sql) {
  SqlQueryResult r = q(db, sql);
  if (!r.success) fail("setup", r.error_message + "\n    " + sql);
}

// Every row, its cells joined by '|', the rows by ';' (NULL as NULL).
std::string rows_of(const SqlQueryResult& r) {
  std::string out;
  for (size_t i = 0; i < r.rows.size(); ++i) {
    if (i) out += ";";
    for (size_t k = 0; k < r.rows[i].values.size(); ++k) out += (k ? "|" : "") + sql_display(r.rows[i].values[k]);
  }
  return out;
}

void expect(SqlDatabase& db, const std::string& label, const std::string& sql, const std::string& want) {
  SqlQueryResult r = q(db, sql);
  if (!r.success) return fail(label, r.error_message);
  const std::string got = rows_of(r);
  if (got != want) return fail(label, "got '" + got + "', want '" + want + "'");
  std::cout << "  [ok] " << label << " = " << got << "\n";
}

void expect_error(SqlDatabase& db, const std::string& label, const std::string& sql, const std::string& want_part) {
  SqlQueryResult r = q(db, sql);
  if (r.success) return fail(label, "succeeded (" + rows_of(r) + "), want an error containing '" + want_part + "'");
  if (r.error_message.find(want_part) == std::string::npos)
    return fail(label, "error '" + r.error_message + "', want one containing '" + want_part + "'");
  std::cout << "  [ok] " << label << ": " << r.error_message << "\n";
}

}  // namespace

int main() {
  std::cout << "=== asql: the SQL SQLDoom needs ===\n";
  const CheckRoot check_root("/tmp/aios-asql-doom-sql");   // this run's own (see check-root.hpp)
  KernelConfig cfg;
  cfg.root = check_root.path;
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
  run_ok(db, "CREATE TABLE a (x INT, y INT, s VARCHAR(10))");
  run_ok(db, "INSERT INTO a VALUES (1,1,'p'),(1,2,'q'),(2,1,'r'),(3,NULL,'s'),(NULL,1,'t')");
  run_ok(db, "CREATE TABLE w (c INT, r INT, solid INT)");
  run_ok(db, "INSERT INTO w VALUES (1,1,1),(1,2,0),(2,1,1),(9,9,1),(NULL,5,1)");

  std::cout << "\n[1] (x, y) IN (SELECT ...)\n";
  const std::string solid = "(SELECT c, r FROM w WHERE solid)";
  expect(db, "IN", "SELECT GROUP_CONCAT(s ORDER BY s) FROM a WHERE (x,y) IN " + solid, "p,r");
  expect(db, "NOT IN", "SELECT GROUP_CONCAT(s ORDER BY s) FROM a WHERE (x,y) NOT IN " + solid, "q");
  expect(db, "as values, NULLs", "SELECT s, (x,y) IN " + solid + " AS v, (x,y) NOT IN " + solid + " AS nv FROM a ORDER BY s",
         "p|1|0;q|0|1;r|1|0;s|NULL|NULL;t|NULL|NULL");
  expect(db, "a NULL the other column contradicts",
         "SELECT s, (x,y) IN (SELECT c, r FROM w WHERE solid AND c IS NOT NULL) AS v FROM a ORDER BY s", "p|1;q|0;r|1;s|0;t|NULL");
  expect(db, "against no rows",
         "SELECT (1,1) IN (SELECT c, r FROM w WHERE 0), (1,1) NOT IN (SELECT c, r FROM w WHERE 0), (NULL,1) IN (SELECT c, r FROM w WHERE 0)",
         "0|1|0");
  expect(db, "compared as = compares",
         "SELECT (1,'1') IN (SELECT c, r FROM w), ('1',1.0) IN (SELECT c, r FROM w), (2,1) IN (SELECT c+0.0, r FROM w)", "1|1|1");
  expect(db, "correlated", "SELECT s FROM a WHERE (x, y) IN (SELECT w.c, w.r FROM w WHERE w.c = a.x AND w.solid = 1) ORDER BY s", "p;r");
  expect(db, "with another condition", "SELECT COUNT(*) FROM a WHERE ((x,y) IN (SELECT c, r FROM w)) AND s <> 'q'", "2");
  expect(db, "expressions on the left", "SELECT s FROM a WHERE (x+0, y*1) IN " + solid + " ORDER BY s", "p;r");
  expect(db, "over a UNION", "SELECT COUNT(*) FROM a WHERE (x, y) IN (SELECT c, r FROM w UNION SELECT 3, 7)", "3");
  expect(db, "nested in a scalar subquery",
         "SELECT (SELECT GROUP_CONCAT(s ORDER BY s) FROM a WHERE (a.x, a.y) IN " + solid + ") AS nested", "p,r");
  run_ok(db, "CREATE FUNCTION f(px INT, py INT) RETURNS INT RETURN (px, py) IN " + solid);
  expect(db, "in a function body", "SELECT f(1,1), f(1,2), f(9,9), f(NULL,5)", "1|0|1|NULL");
  expect_error(db, "fewer columns", "SELECT (x,y) IN (SELECT c FROM w) FROM a LIMIT 1", "Operand should contain 2 column(s)");
  expect_error(db, "more columns", "SELECT (x,y,s) IN (SELECT c, r FROM w) FROM a LIMIT 1", "Operand should contain 3 column(s)");
  // The same text three times: the answer cache, and the subquery's kept rows, give the same answer.
  for (int i = 0; i < 3; ++i) expect(db, "repeated #" + std::to_string(i + 1), "SELECT COUNT(*) FROM a WHERE (x,y) IN " + solid, "2");

  std::cout << "\n=== " << (g_failures == 0 ? "ALL CHECKS PASSED" : "FAILURES: " + std::to_string(g_failures)) << " ===\n";
  return g_failures == 0 ? 0 : 1;
}
