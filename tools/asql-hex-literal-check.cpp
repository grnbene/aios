// Checks a 0x.. literal going into a column, against what MariaDB 10.11 stores for the same statements: in a
// numeric column it is the number its bytes spell (MariaDB's hex hybrid), in a text column the bytes. ASQL stored
// the bytes in an INT column - `INSERT INTO parts (color) VALUES (0x9a7444)` kept three bytes that every later read
// returned as text - through VALUES, UPDATE .. SET and the literal ON DUPLICATE KEY UPDATE path alike, and read an
// odd number of digits with the pad nibble at the end (0xABC as 0xAB0C).
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

void expect(SqlDatabase& db, const std::string& sql, const std::string& want) {
  SqlQueryResult r = db.execute_local_query(sql);
  if (!r.success) { std::printf("  query failed: %s\n  %s\n", sql.c_str(), r.error_message.c_str()); std::exit(1); }
  const std::string got = r.rows.empty() || r.rows[0].values.empty() ? "<none>" : r.rows[0].values[0];
  ++checks;
  if (got != want) {
    ++failures;
    std::printf("  FAIL %s\n    want %s, got %s\n", sql.c_str(), want.c_str(), got.c_str());
  }
}

}  // namespace

int main() {
  std::printf("=== ASQL hex literal check ===\n");
  const CheckRoot check_root("/tmp/aios-asql-hex-literal");
  KernelConfig cfg;
  cfg.root = check_root.path;
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::printf("boot failed\n"); return 1; }
  SqlDatabase db(**k);
  db.init("sql_db");
  exec(db, "CREATE DATABASE IF NOT EXISTS hexdb");
  exec(db, "USE hexdb");
  exec(db, "CREATE TABLE h (k INT PRIMARY KEY, a INT, d DOUBLE, b BIGINT, m DECIMAL(10,2), v VARCHAR(10))");

  // Every expected value below is MariaDB 10.11's answer to the same statements.
  exec(db, "INSERT INTO h VALUES (1, 0x9a7444, 0x10, 0xABC, 0x41, 0x41)");
  expect(db, "SELECT a FROM h WHERE k = 1", "10122308");
  expect(db, "SELECT d FROM h WHERE k = 1", "16");
  expect(db, "SELECT b FROM h WHERE k = 1", "2748");
  expect(db, "SELECT m FROM h WHERE k = 1", "65.00");
  expect(db, "SELECT v FROM h WHERE k = 1", "A");
  expect(db, "SELECT a >> 16 FROM h WHERE k = 1", "154");
  expect(db, "SELECT a & 255 FROM h WHERE k = 1", "68");

  exec(db, "INSERT INTO h (k, a) VALUES (2, 0x0000000000000041)");
  expect(db, "SELECT a FROM h WHERE k = 2", "65");

  exec(db, "INSERT INTO h (k, a) VALUES (4, 5)");
  exec(db, "UPDATE h SET a = 0x10 WHERE k = 4");
  expect(db, "SELECT a FROM h WHERE k = 4", "16");

  exec(db, "INSERT INTO h (k, a) VALUES (5, 0x41) ON DUPLICATE KEY UPDATE a = 0x42");
  expect(db, "SELECT a FROM h WHERE k = 5", "65");
  exec(db, "INSERT INTO h (k, a) VALUES (5, 0x41) ON DUPLICATE KEY UPDATE a = 0x42");
  expect(db, "SELECT a FROM h WHERE k = 5", "66");

  // INSERT .. SELECT: an item that is a bare 0x literal, aliased or not, through a WITH and a join as well.
  exec(db, "INSERT INTO h (k, a) SELECT 6, 0x9a7444");
  expect(db, "SELECT a FROM h WHERE k = 6", "10122308");
  exec(db, "INSERT INTO h (k, a, v) WITH RECURSIVE n (i) AS (SELECT 0 UNION ALL SELECT i + 1 FROM n WHERE i < 2) "
           "SELECT 10 + n.i, 0x2a5db0 AS c, 0x41 FROM n CROSS JOIN (SELECT 1 AS one) o");
  expect(db, "SELECT COUNT(*) FROM h WHERE k BETWEEN 10 AND 12 AND a = 2776496", "3");
  expect(db, "SELECT v FROM h WHERE k = 11", "A");

  // An odd number of digits has its pad nibble in front: 0xABC is 0x0ABC.
  expect(db, "SELECT HEX(0xABC)", "0ABC");
  expect(db, "SELECT HEX(0x1)", "01");

  std::printf("  %d values checked\n", checks);
  if (failures) { std::printf("=== %d FAILURE(S) ===\n", failures); return 1; }
  std::printf("=== ALL HEX LITERAL CHECKS PASSED ===\n");
  return 0;
}
