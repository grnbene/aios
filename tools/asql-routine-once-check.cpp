// Checks that a stored function called from inside a stored program runs once, and that its error reaches the
// caller. The routine's fast expression path ran a function the moment it read it; when the path then gave up (a
// builtin further along it does not have) or the function failed, the whole expression was run again as a SELECT:
// every write the function made happened twice, and a function that failed the first time could take another
// branch the second time (its own first writes now there) and succeed, so the error was lost - Pick Rush's tic
// created an order, failed on its lines, and went on as if nothing had happened.
#undef NDEBUG
#include <cstdio>
#include <cstdlib>
#include <string>

#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"
#include "check-root.hpp"

using namespace aios;
using namespace aios::userland;

namespace {

int failures = 0;
int checks = 0;

void exec(SqlDatabase& db, const std::string& sql) {
  SqlQueryResult r = db.execute_local_query(sql);
  if (!r.success) { std::printf("  statement failed: %s\n  %s\n", sql.c_str(), r.error_message.c_str()); std::exit(1); }
}

std::string one(SqlDatabase& db, const std::string& sql) {
  SqlQueryResult r = db.execute_local_query(sql);
  if (!r.success) { std::printf("  query failed: %s\n  %s\n", sql.c_str(), r.error_message.c_str()); std::exit(1); }
  return r.rows.empty() || r.rows[0].values.empty() ? "<none>" : r.rows[0].values[0];
}

void expect_eq(const std::string& what, const std::string& got, const std::string& want) {
  ++checks;
  if (got != want) { ++failures; std::printf("  FAIL %s\n    want %s, got %s\n", what.c_str(), want.c_str(), got.c_str()); }
}

// The statement runs; `calls` rows were logged by it.
void expect_calls(SqlDatabase& db, const std::string& sql, const std::string& calls) {
  exec(db, "DELETE FROM calls");
  exec(db, sql);
  expect_eq(sql + " (calls)", one(db, "SELECT COUNT(*) FROM calls"), calls);
}

// The statement fails with `want_in` in its message.
void expect_error(SqlDatabase& db, const std::string& sql, const std::string& want_in) {
  SqlQueryResult r = db.execute_local_query(sql);
  ++checks;
  if (r.success) { ++failures; std::printf("  FAIL %s\n    want an error (%s), it succeeded\n", sql.c_str(), want_in.c_str()); }
  else if (r.error_message.find(want_in) == std::string::npos) {
    ++failures;
    std::printf("  FAIL %s\n    want an error with \"%s\", got: %s\n", sql.c_str(), want_in.c_str(), r.error_message.c_str());
  }
}

}  // namespace

int main() {
  std::printf("=== ASQL routine function-runs-once check ===\n");
  const CheckRoot check_root("/tmp/aios-asql-routine-once");
  KernelConfig cfg;
  cfg.root = check_root.path;
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::printf("boot failed\n"); return 1; }
  SqlDatabase db(**k);
  db.init("sql_db");
  exec(db, "CREATE DATABASE IF NOT EXISTS oncedb");
  exec(db, "USE oncedb");
  exec(db, "CREATE TABLE calls (n INT)");
  exec(db, "CREATE TABLE flag (id INT PRIMARY KEY, done INT)");

  // logs a call and returns 2
  exec(db, "CREATE FUNCTION logged() RETURNS INT BEGIN INSERT INTO calls VALUES (1); RETURN 2; END");
  // fails the first time it is called (BIGINT overflow), then - its flag set - succeeds
  exec(db, "CREATE FUNCTION fails_once() RETURNS INT BEGIN DECLARE d INT DEFAULT 0; "
           "INSERT INTO calls VALUES (1); SELECT COUNT(*) INTO d FROM flag WHERE id = 1; "
           "INSERT INTO flag VALUES (1, 1) ON DUPLICATE KEY UPDATE done = 1; "
           "IF d = 0 THEN INSERT INTO calls SELECT 9223372036854775807 * 2; END IF; RETURN 1; END");

  exec(db, "CREATE FUNCTION via_do() RETURNS INT BEGIN DO logged(); RETURN 1; END");
  exec(db, "CREATE FUNCTION via_set_plain() RETURNS INT BEGIN DECLARE v INT; SET v = logged() + 1; RETURN v; END");
  // SIN is not on the fast path's list: the expression goes to the SELECT path, which must be the only run
  exec(db, "CREATE FUNCTION via_set_builtin() RETURNS INT BEGIN DECLARE v DOUBLE; SET v = SIN(logged()); RETURN 1; END");
  exec(db, "CREATE FUNCTION via_return() RETURNS INT RETURN logged() * 10");
  exec(db, "CREATE FUNCTION via_if() RETURNS INT BEGIN IF logged() = 2 THEN RETURN 1; END IF; RETURN 0; END");
  exec(db, "CREATE FUNCTION do_fails_once() RETURNS INT BEGIN DO fails_once(); RETURN 1; END");
  exec(db, "CREATE FUNCTION set_fails_once() RETURNS INT BEGIN DECLARE v INT; SET v = fails_once(); RETURN v; END");

  expect_calls(db, "SELECT via_do()", "1");
  expect_calls(db, "SELECT via_set_plain()", "1");
  expect_eq("via_set_plain() value", one(db, "SELECT via_set_plain()"), "3");
  expect_calls(db, "SELECT via_set_builtin()", "1");
  expect_calls(db, "SELECT via_return()", "1");
  expect_eq("via_return() value", one(db, "SELECT via_return()"), "20");
  expect_calls(db, "SELECT via_if()", "1");

  // The failure is the caller's error; nothing ran it a second time to get past it.
  exec(db, "DELETE FROM calls");
  exec(db, "DELETE FROM flag");
  expect_error(db, "SELECT do_fails_once()", "out of range");
  expect_eq("do_fails_once() calls", one(db, "SELECT COUNT(*) FROM calls"), "1");
  exec(db, "DELETE FROM calls");
  exec(db, "DELETE FROM flag");
  expect_error(db, "SELECT set_fails_once()", "out of range");
  expect_eq("set_fails_once() calls", one(db, "SELECT COUNT(*) FROM calls"), "1");

  std::printf("  %d checked\n", checks);
  if (failures) { std::printf("=== %d FAILURE(S) ===\n", failures); return 1; }
  std::printf("=== ALL ROUTINE RUNS-ONCE CHECKS PASSED ===\n");
  return 0;
}
