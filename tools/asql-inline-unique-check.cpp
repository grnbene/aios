#undef NDEBUG
// A column-level UNIQUE (`k VARCHAR(10) UNIQUE`, `UNIQUE KEY`) is a unique key like the table-level `UNIQUE KEY (k)`:
// a duplicate insert fails with "Duplicate entry ... for key ..." (MariaDB error 1062), ON DUPLICATE KEY UPDATE
// updates the row, INSERT IGNORE skips it, NULLs repeat, an UPDATE into a duplicate fails. Each case also runs on a
// table-level twin, which already worked, so the two are held to the same answers.
#include <iostream>
#include <string>
#include <unistd.h>

#include "aios/kernel/kernel.hpp"
#include "aios/storage/aql.hpp"
#include "aios/userland/sql_database.hpp"
#include "aios/userland/task_ledger.hpp"

using namespace aios;
using namespace aios::userland;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& label) {
  std::cout << (ok ? "  [ok] " : "  [FAIL] ") << label << "\n";
  if (!ok) g_failures++;
}

SqlQueryResult run(SqlDatabase& db, const std::string& sql) { return db.execute_gateway_query(sql); }

std::string cell(SqlDatabase& db, const std::string& sql) {
  SqlQueryResult r = run(db, sql);
  return r.success && !r.rows.empty() && !r.rows[0].values.empty() ? r.rows[0].values[0] : "<" + r.error_message + ">";
}

// The whole sequence on one table; `key` is the name MariaDB gives the key in its error.
void scenario(SqlDatabase& db, const std::string& table, const std::string& key) {
  const std::string t = table;
  check(run(db, "INSERT INTO " + t + " (k, v) VALUES ('a', 1);").success, t + ": a first insert works");
  SqlQueryResult dup = run(db, "INSERT INTO " + t + " (k, v) VALUES ('a', 2);");
  check(!dup.success && dup.error_message == "Duplicate entry 'a' for key '" + key + "'",
        t + ": a duplicate fails with error 1062's text (got: " + (dup.success ? std::string("success") : dup.error_message) + ")");
  check(cell(db, "SELECT COUNT(*) FROM " + t + ";") == "1", t + ": and no row was added");

  check(run(db, "INSERT INTO " + t + " (k, v) VALUES ('a', 9) ON DUPLICATE KEY UPDATE v = 9;").success, t + ": ON DUPLICATE KEY UPDATE runs");
  check(cell(db, "SELECT v FROM " + t + " WHERE k = 'a';") == "9" && cell(db, "SELECT COUNT(*) FROM " + t + ";") == "1",
        t + ": and updated the existing row, adding none");

  check(run(db, "INSERT IGNORE INTO " + t + " (k, v) VALUES ('a', 5);").success, t + ": INSERT IGNORE of a duplicate succeeds");
  check(cell(db, "SELECT v FROM " + t + " WHERE k = 'a';") == "9" && cell(db, "SELECT COUNT(*) FROM " + t + ";") == "1",
        t + ": and skipped it");

  check(run(db, "INSERT INTO " + t + " (k, v) VALUES (NULL, 1);").success && run(db, "INSERT INTO " + t + " (k, v) VALUES (NULL, 2);").success,
        t + ": NULL repeats");
  check(cell(db, "SELECT COUNT(*) FROM " + t + " WHERE k IS NULL;") == "2", t + ": both NULL rows are there");

  check(run(db, "INSERT INTO " + t + " (k, v) VALUES ('b', 3);").success, t + ": a second distinct key");
  SqlQueryResult upd = run(db, "UPDATE " + t + " SET k = 'a' WHERE k = 'b';");
  check(!upd.success && upd.error_message.rfind("Duplicate entry 'a'", 0) == 0,
        t + ": an UPDATE into a duplicate fails (got: " + (upd.success ? std::string("success") : upd.error_message) + ")");
  check(cell(db, "SELECT COUNT(*) FROM " + t + " WHERE k = 'b';") == "1", t + ": and the row kept its value");
}

}  // namespace

int main() {
  std::cout << "=== asql column-level UNIQUE ===\n";
  char root_dir[] = "/tmp/aios-inlineunique-XXXXXX";
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

  check(run(db, "CREATE TABLE inline_u (id INT AUTO_INCREMENT PRIMARY KEY, k VARCHAR(10) UNIQUE, v INT);").success, "inline UNIQUE table created");
  check(run(db, "CREATE TABLE table_u (id INT AUTO_INCREMENT PRIMARY KEY, k VARCHAR(10), v INT, UNIQUE KEY uk (k));").success, "table-level twin created");

  std::cout << "\n[1] inline UNIQUE\n";
  scenario(db, "inline_u", "k");
  std::cout << "\n[2] the table-level twin gives the same answers\n";
  scenario(db, "table_u", "uk");

  std::cout << "\n[3] other spellings\n";
  check(run(db, "CREATE TABLE inline_key (id INT PRIMARY KEY, k VARCHAR(10) NOT NULL UNIQUE KEY, n INT UNIQUE);").success, "UNIQUE KEY and a second unique column in one table");
  run(db, "INSERT INTO inline_key (id, k, n) VALUES (1, 'x', 10);");
  check(!run(db, "INSERT INTO inline_key (id, k, n) VALUES (2, 'x', 11);").success, "UNIQUE KEY on a NOT NULL column is enforced");
  check(!run(db, "INSERT INTO inline_key (id, k, n) VALUES (3, 'y', 10);").success, "and so is the second column's");
  check(run(db, "INSERT INTO inline_key (id, k, n) VALUES (4, 'y', 11);").success, "rows distinct in both are accepted");
  check(run(db, "CREATE TABLE in_comment (id INT PRIMARY KEY, x INT COMMENT 'not UNIQUE at all');").success, "a COMMENT that says UNIQUE");
  check(run(db, "INSERT INTO in_comment (id, x) VALUES (1, 7);").success && run(db, "INSERT INTO in_comment (id, x) VALUES (2, 7);").success,
        "makes no key: the same value repeats");
  check(run(db, "CREATE TABLE pk_unique (id INT PRIMARY KEY UNIQUE);").success, "PRIMARY KEY UNIQUE is accepted");
  const std::string shown = [&] { SqlQueryResult r = run(db, "SHOW CREATE TABLE inline_u;"); return r.success && !r.rows.empty() && r.rows[0].values.size() > 1 ? r.rows[0].values[1] : std::string(); }();
  check(shown.find("UNIQUE KEY `k` (`k`)") != std::string::npos, "SHOW CREATE TABLE lists UNIQUE KEY `k` (`k`)");

  std::cout << "\n=== " << (g_failures == 0 ? "ALL CHECKS PASSED" : "FAILURES: " + std::to_string(g_failures)) << " ===\n";
  return g_failures == 0 ? 0 : 1;
}
