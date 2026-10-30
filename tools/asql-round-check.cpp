// ROUND, FORMAT and CAST of a DOUBLE round as PHP and grain do - the decimal the double is written as, to 15
// significant digits, then half away from zero - not as MariaDB does (the binary value, half to even). On aa a view
// of ROUND(ROUND(price,2) * ROUND(discountpercent,4),2) ... over DOUBLE columns put 26 of 9245 invoices a cent off
// the totals the ERP's PHP wrote; this rule puts none off. The lines below are aa's own (invoices 130442126 and
// 130442531, 25% and 10% off). Exact values round exactly, as before; MOD and FORMAT of a DECIMAL are exact.
#undef NDEBUG
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"

using namespace aios;
using namespace aios::userland;

namespace {
int failures = 0, checks = 0;

std::string q1(SqlDatabase& db, const std::string& sql) {
  SqlQueryResult r = db.execute_local_query(sql);
  if (!r.success) { std::printf("  query failed: %s\n  %s\n", sql.c_str(), r.error_message.c_str()); std::exit(1); }
  if (r.rows.empty() || r.rows[0].values.empty()) return "<none>";
  return r.rows[0].values[0];
}
void exec(SqlDatabase& db, const std::string& sql) {
  SqlQueryResult r = db.execute_local_query(sql);
  if (!r.success) { std::printf("  failed: %s\n  %s\n", sql.c_str(), r.error_message.c_str()); std::exit(1); }
}
void expect(SqlDatabase& db, const std::string& expr, const std::string& want, const char* why) {
  ++checks;
  const std::string got = q1(db, "SELECT " + expr);
  const bool ok = got == want;
  if (!ok) ++failures;
  std::printf("  %s %-58s %-14s %s\n", ok ? "ok  " : "FAIL", expr.c_str(), (got + (ok ? "" : " want " + want)).c_str(), why);
}
}  // namespace

int main() {
  std::printf("=== ASQL rounding of DOUBLEs and DECIMALs ===\n");
  const std::string root = "/tmp/aios-asql-round-check";
  { std::error_code e; std::filesystem::remove_all(root, e); }
  KernelConfig cfg;
  cfg.root = root;
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::printf("boot failed\n"); return 1; }
  SqlDatabase db(**k);
  db.init("sql_db");
  exec(db, "CREATE DATABASE IF NOT EXISTS rdb");
  exec(db, "USE rdb");
  exec(db, "CREATE TABLE sm (id INT PRIMARY KEY, price DOUBLE, discountpercent DOUBLE, qty DOUBLE)");
  exec(db, "INSERT INTO sm VALUES (1, 6.5, 0.25, -1), (2, 9.09, 0.25, -1), (3, 35.79, 0.1, -1), (4, 28.25, 0.1, 1), (5, 1.55, 0.3, 1)");

  // DOUBLE halves: MariaDB says 1.62, 8.62, 2, -2, 0.46 (binary, half to even); PHP and grain say these
  expect(db, "ROUND(1.625e0, 2)", "1.63", "an exact binary half rounds away from zero");
  expect(db, "ROUND(8.625e0, 2)", "8.63", "");
  expect(db, "ROUND(2.5e0)", "3", "");
  expect(db, "ROUND(-2.5e0)", "-3", "");
  expect(db, "ROUND(1.55e0 * 0.3e0, 2)", "0.47", "0.46499999999999997: the product's noise does not decide it");
  expect(db, "ROUND(-0.004e0, 2)", "0.00", "a rounded zero has no sign");
  expect(db, "ROUND(1234.5e0, -2)", "1200", "places below 0");
  expect(db, "ROUND(2.675e0, 2)", "2.68", "");
  // aa's lines, through the view's own expression (no casts)
  const std::string line = "ROUND(ROUND(ROUND(price,2) - ROUND(ROUND(price,2) * ROUND(discountpercent,4),2),2) * qty,2)";
  expect(db, line + " FROM sm WHERE id = 1", "-4.87", "6.50 at 25%: 1.625 off is 1.63 (MariaDB 1.62, -4.88)");
  expect(db, line + " FROM sm WHERE id = 2", "-6.82", "9.09 at 25%");
  const std::string cast = "CAST(" + line + " AS DECIMAL(10,2))";   // the view casts each line, so its sums are exact
  expect(db, "SUM(" + cast + ") FROM sm WHERE id IN (1, 2)", "-11.69", "invoice 130442126's 11.69, as the ERP wrote it");
  expect(db, line + " FROM sm WHERE id = 4", "25.42", "28.25 at 10%: 2.825 off is 2.83");
  expect(db, "SUM(" + cast + ") FROM sm WHERE id IN (3, 4)", "-6.79", "invoice 130442531's 6.79");
  expect(db, cast + " FROM sm WHERE id = 5", "1.08", "1.55 at 30%: 0.465 off is 0.47 (MariaDB 0.46, 1.09)");
  // FORMAT and CAST round the same way
  expect(db, "FORMAT(1.0050, 2)", "1.01", "a DECIMAL: exactly (it went through a double: 1.00)");
  expect(db, "FORMAT(99999999999999999999, 0)", "99,999,999,999,999,999,999", "every digit");
  expect(db, "FORMAT(1.55e0 * 0.3e0, 2)", "0.47", "a DOUBLE as written");
  expect(db, "FORMAT(-1234.565, 2)", "-1,234.57", "");
  expect(db, "CAST(1.55e0 * 0.3e0 AS DECIMAL(10,2))", "0.47", "");
  expect(db, "CAST(2.5e0 AS SIGNED)", "3", "");
  expect(db, "CAST(-2.5e0 AS SIGNED)", "-3", "");
  // exact values, as before
  expect(db, "ROUND(2.675, 2)", "2.68", "a DECIMAL rounds exactly");
  expect(db, "ROUND(-2.5)", "-3", "");
  expect(db, "MOD(2.6750, 2)", "0.6750", "MOD of a DECIMAL: exact (it was 0.6749999999999998)");
  expect(db, "MOD(1.00, 0.3)", "0.10", "");
  // the rest of the local MariaDB sweep's differences, each now as MariaDB answers
  expect(db, "99999999999999999999 + 0", "99999999999999999999", "an integer past UNSIGNED BIGINT is a DECIMAL: exact, no error 1690");
  expect(db, "999999999999999999999999999999 * 1", "999999999999999999999999999999", "");
  exec(db, "CREATE TABLE v (g INT, i BIGINT, d2 DECIMAL(12,2), d4 DECIMAL(18,4), x DOUBLE)");
  exec(db, "INSERT INTO v VALUES (1, 2147483647, -2.50, -2.5000, 2.675), (1, -2147483648, 1.00, 1.0050, 1.005), (1, 3, 4.35, 4.3500, 0.1)");
  expect(db, "VARIANCE(i) FROM v", "3074457344186603000.0000", "a big DOUBLE in full from its shortest digits (not ...3333.3333 of the binary)");
  expect(db, "COUNT(*) FROM (SELECT d2 FROM v UNION SELECT d4 FROM v) u", "4", "UNION: -2.50 and -2.5000 are one row");
  expect(db, "MIN(d2) FROM (SELECT d2 FROM v UNION ALL SELECT x FROM v) u", "-2.5", "DECIMAL beside DOUBLE is a DOUBLE");
  std::printf("%d of %d checks %s\n", checks - failures, checks, failures ? "passed - FAILED" : "passed");
  { std::error_code e; std::filesystem::remove_all(root, e); }
  return failures ? 1 : 0;
}
