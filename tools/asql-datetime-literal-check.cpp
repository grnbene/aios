// A DATETIME / DATE column compared with a literal in any shape MariaDB reads as a time: 'YYYY-MM-DD HH:MM:SS',
// 'YYYY-MM-DD HH:MM', 'YYYY-MM-DD HH', 'YYYY-MM-DD', with a T, with fractions. `d >= '2026-10-25 17:00'`
// once matched every row (the literal read as the number 2026) - a 21M-row audit log answered whole. Each count is
// checked against a model, on the table as written and again after a checkpoint and a reopen (encoded columns).
#undef NDEBUG
#include <cstdio>
#include <string>
#include <vector>
#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"
#include "check-root.hpp"
using namespace aios;
using namespace aios::userland;
namespace {
int failures = 0;
std::string one(SqlDatabase& db, const std::string& q) {
  auto r = db.execute_local_query(q);
  return !r.success ? "ERROR " + r.error_message : r.rows.empty() ? "<none>" : r.rows[0].values[0];
}
}  // namespace
int main() {
  const CheckRoot check_root("/tmp/aios-datetime-literal-check");   // this run's own (see check-root.hpp)
  const std::string& root = check_root.path;
  KernelConfig cfg; cfg.root = root; cfg.residency = test_config;
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::printf("boot failed\n"); return 1; }
  // one row a minute from 2026-10-25 00:00:00 to 23:59:00, every row at :17 seconds, and dates a day apart
  std::vector<int> secs;   // seconds since midnight, for the model
  {
    SqlDatabase db(**k);
    db.init("sql_db");
    db.execute_local_query("CREATE DATABASE dt");
    db.execute_local_query("USE dt");
    db.execute_local_query("CREATE TABLE ev (id INT PRIMARY KEY, at DATETIME NOT NULL, day DATE NOT NULL)");
    std::string sql;
    for (int m = 0; m < 1440; ++m) {
      char b[96];
      std::snprintf(b, sizeof b, "%s(%d,'2026-10-25 %02d:%02d:17','2026-%02d-%02d')", sql.empty() ? "" : ",", m, m / 60, m % 60, 1 + m % 12, 1 + m % 28);
      sql += b;
      secs.push_back(m * 60 + 17);
    }
    if (!db.execute_local_query("INSERT INTO ev VALUES " + sql).success) { std::printf("insert failed\n"); return 1; }
    if (!db.flush_now().ok()) { std::printf("checkpoint failed\n"); return 1; }
  }
  struct Case { std::string cmp, lit; int at; };   // at: the literal's seconds since midnight on 2026-10-25
  const std::vector<Case> cases = {
      {">=", "'2026-10-25 17:00:00'", 17 * 3600}, {">=", "'2026-10-25 17:00'", 17 * 3600}, {">=", "'2026-10-25 17'", 17 * 3600},
      {"<", "'2026-10-25 17:00'", 17 * 3600},     {"<", "'2026-10-25 17'", 17 * 3600},      {">", "'2026-10-25 9:05'", 9 * 3600 + 5 * 60},
      {">=", "'2026-10-25T17:00'", 17 * 3600},    {"<=", "'2026-10-25 17:00:17.5'", 17 * 3600 + 17}, {">=", "'2026-10-25'", 0},
      {"<", "'2026-10-26'", 86400},               {"=", "'2026-10-25 17:00:17'", 17 * 3600 + 17},
      // a bare number is read as the date it spells, as MariaDB reads it: YYYYMMDDhhmmss, or YYYYMMDD at midnight
      {">=", "20261025170000", 17 * 3600},        {"<", "20261025170000", 17 * 3600},       {"=", "20261025170017", 17 * 3600 + 17},
      {">=", "20261025", 0},                      {"<", "20261026", 86400},                 {">", "20261025090500", 9 * 3600 + 5 * 60},
      // digits in quotes too ('20261025170000' is that datetime)
      {">=", "'20261025170000'", 17 * 3600},
  };
  auto model = [&](const Case& c) {
    int n = 0;
    for (int s : secs) {
      const bool hit = c.cmp == ">=" ? s >= c.at : c.cmp == ">" ? s > c.at : c.cmp == "<" ? s < c.at : c.cmp == "<=" ? s <= c.at : s == c.at;
      n += hit;
    }
    return n;
  };
  for (int pass = 0; pass < 2; ++pass) {
    SqlDatabase db(**k);
    db.init("sql_db");
    db.execute_local_query("USE dt");
    for (const Case& c : cases) {
      const std::string got = one(db, "SELECT COUNT(*) FROM ev WHERE at " + c.cmp + " " + c.lit);
      const std::string rows = std::to_string(db.execute_local_query("SELECT id FROM ev WHERE at " + c.cmp + " " + c.lit).rows.size());
      const std::string want = std::to_string(model(c));
      const bool ok = got == want && rows == want;
      if (!ok) ++failures;
      std::printf("  [%s] %s at %s %s: COUNT %s, rows %s, want %s\n", ok ? "ok" : "FAIL", pass ? "reopened" : "written", c.cmp.c_str(), c.lit.c_str(), got.c_str(), rows.c_str(), want.c_str());
    }
    // a DATE column against a datetime literal: '2026-05-01 00:00' is the date 2026-05-01 at midnight
    const std::string dg = one(db, "SELECT COUNT(*) FROM ev WHERE day >= '2026-05-01 00:00'");
    const std::string dw = one(db, "SELECT COUNT(*) FROM ev WHERE day >= '2026-05-01'");
    if (dg != dw) ++failures;
    std::printf("  [%s] %s day >= '2026-05-01 00:00': %s, as day >= '2026-05-01': %s\n", dg == dw ? "ok" : "FAIL", pass ? "reopened" : "written", dg.c_str(), dw.c_str());
    // and a bare number: 20260501 is that date
    const std::string dn = one(db, "SELECT COUNT(*) FROM ev WHERE day >= 20260501");
    if (dn != dw) ++failures;
    std::printf("  [%s] %s day >= 20260501: %s, as day >= '2026-05-01': %s\n", dn == dw ? "ok" : "FAIL", pass ? "reopened" : "written", dn.c_str(), dw.c_str());
    if (pass == 0) db.flush_now();
  }
  if (failures) { std::printf("=== %d FAILURE(S) ===\n", failures); return 1; }
  std::printf("=== DATETIME LITERAL CHECKS PASSED ===\n");
  return 0;
}
