#undef NDEBUG
// A generated column added or restated by ALTER TABLE (ADD COLUMN, MODIFY, CHANGE) is a generated column, as one
// declared in CREATE TABLE is: the rows already there are worked out when it is added, later INSERTs and UPDATEs
// keep it current, SHOW CREATE TABLE prints its expression, and it stays generated when the store is opened again.
// Before, ALTER read past `AS (...) STORED` and added a plain column that stayed NULL in every row.
#include <iostream>
#include <memory>
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

std::string create_text(SqlDatabase& db, const std::string& t) {
  SqlQueryResult r = run(db, "SHOW CREATE TABLE " + t + ";");
  return r.success && !r.rows.empty() && r.rows[0].values.size() > 1 ? r.rows[0].values[1] : "<" + r.error_message + ">";
}

bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

}  // namespace

int main() {
  std::cout << "=== asql generated columns through ALTER TABLE ===\n";
  char root_dir[] = "/tmp/aios-genalter-XXXXXX";
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
  auto db_ptr = std::make_unique<SqlDatabase>(kernel, &ledger);
  db_ptr->init("sql_db");
  SqlDatabase& db = *db_ptr;

  std::cout << "\n[1] ADD COLUMN ... AS (...) STORED over rows already there\n";
  check(run(db, "CREATE TABLE j (id INT PRIMARY KEY, doc JSON, n INT);").success, "table created");
  check(run(db, "INSERT INTO j VALUES (1, '{\"cust\": \"a\"}', 5), (2, '{\"cust\": \"b\"}', 6), (3, '{}', 7);").success, "three rows");
  SqlQueryResult add = run(db, "ALTER TABLE j ADD COLUMN cust VARCHAR(20) AS (JSON_VALUE(doc, '$.cust')) STORED;");
  check(add.success, "ADD COLUMN ... STORED runs (" + add.error_message + ")");
  check(cell(db, "SELECT cust FROM j WHERE id = 1;") == "a" && cell(db, "SELECT cust FROM j WHERE id = 2;") == "b",
        "the rows already there are worked out (got " + cell(db, "SELECT cust FROM j WHERE id = 1;") + ")");
  check(cell(db, "SELECT COUNT(*) FROM j WHERE cust IS NULL;") == "1", "a document without the key gives NULL");
  check(cell(db, "SELECT COUNT(*) FROM j WHERE cust = 'b';") == "1", "a WHERE on it finds the row");
  check(run(db, "INSERT INTO j (id, doc, n) VALUES (4, '{\"cust\": \"c\"}', 8);").success, "a later INSERT");
  check(cell(db, "SELECT cust FROM j WHERE id = 4;") == "c", "is worked out on insert");
  check(run(db, "UPDATE j SET doc = '{\"cust\": \"z\"}' WHERE id = 2;").success, "an UPDATE of the source column");
  check(cell(db, "SELECT cust FROM j WHERE id = 2;") == "z", "is worked out again");
  check(has(create_text(db, "j"), "GENERATED ALWAYS AS (JSON_VALUE(doc, '$.cust')) STORED"), "SHOW CREATE TABLE prints it as STORED");

  std::cout << "\n[2] GENERATED ALWAYS AS (...) VIRTUAL, placed AFTER a column\n";
  check(run(db, "ALTER TABLE j ADD COLUMN n10 INT GENERATED ALWAYS AS (n * 10) VIRTUAL AFTER n;").success, "ADD ... VIRTUAL AFTER n runs");
  check(cell(db, "SELECT n10 FROM j WHERE id = 3;") == "70", "the rows already there are worked out");
  {
    SqlQueryResult r = run(db, "SELECT * FROM j WHERE id = 1;");
    check(r.success && r.columns.size() == 5 && r.columns[3] == "n10", "and it sits right after n");
  }
  check(has(create_text(db, "j"), "GENERATED ALWAYS AS (n * 10) VIRTUAL"), "SHOW CREATE TABLE prints it as VIRTUAL");
  check(run(db, "ALTER TABLE j ADD COLUMN n11 INT AS (n10 + 1);").success, "a bare AS (...) building on another generated column");
  check(cell(db, "SELECT n11 FROM j WHERE id = 3;") == "71", "reads the earlier one's value");

  std::cout << "\n[3] an expression that cannot be worked out leaves the table as it was\n";
  const std::string before = create_text(db, "j");
  SqlQueryResult bad = run(db, "ALTER TABLE j ADD COLUMN broken INT AS (no_such_column + 1) STORED;");
  check(!bad.success, "ADD with an unknown column in the expression fails (" + (bad.success ? std::string("succeeded") : bad.error_message) + ")");
  check(create_text(db, "j") == before, "the table's definition is unchanged");
  check(cell(db, "SELECT cust FROM j WHERE id = 4;") == "c", "and its rows still read");

  std::cout << "\n[4] MODIFY makes a plain column generated, and back\n";
  check(run(db, "CREATE TABLE m (id INT PRIMARY KEY, a INT, b INT);").success, "table created");
  check(run(db, "INSERT INTO m VALUES (1, 2, NULL), (2, 3, 99);").success, "two rows, b holding its own values");
  check(run(db, "ALTER TABLE m MODIFY b INT AS (a * a) STORED;").success, "MODIFY b ... AS (a * a) STORED runs");
  check(cell(db, "SELECT b FROM m WHERE id = 1;") == "4" && cell(db, "SELECT b FROM m WHERE id = 2;") == "9", "every row's b is worked out");
  check(run(db, "UPDATE m SET a = 5 WHERE id = 1;").success && cell(db, "SELECT b FROM m WHERE id = 1;") == "25", "and follows a");
  check(run(db, "ALTER TABLE m MODIFY b INT;").success, "MODIFY b INT restates it as a plain column");
  check(!has(create_text(db, "m"), "GENERATED"), "SHOW CREATE TABLE no longer says GENERATED");
  check(cell(db, "SELECT b FROM m WHERE id = 1;") == "25", "the values it had are kept");
  check(run(db, "UPDATE m SET a = 6 WHERE id = 1;").success && cell(db, "SELECT b FROM m WHERE id = 1;") == "25", "and no longer follow a");
  const std::string m_before = create_text(db, "m");
  check(!run(db, "ALTER TABLE m MODIFY b INT AS (nope) STORED;").success, "a MODIFY whose expression fails is refused");
  check(create_text(db, "m") == m_before && cell(db, "SELECT b FROM m WHERE id = 2;") == "9", "and puts the column and its values back");

  std::cout << "\n[5] CHANGE renames and makes it generated\n";
  check(run(db, "ALTER TABLE m CHANGE b a_plus INT GENERATED ALWAYS AS (a + 100) VIRTUAL;").success, "CHANGE b a_plus ... runs");
  check(cell(db, "SELECT a_plus FROM m WHERE id = 2;") == "103", "the renamed column is worked out");

  std::cout << "\n[6] after the store is opened again\n";
  db_ptr.reset();
  auto db2 = std::make_unique<SqlDatabase>(kernel, &ledger);
  db2->init("sql_db");
  check(cell(*db2, "SELECT cust FROM j WHERE id = 2;") == "z" && cell(*db2, "SELECT n10 FROM j WHERE id = 3;") == "70",
        "the values are there");
  check(run(*db2, "INSERT INTO j (id, doc, n) VALUES (5, '{\"cust\": \"d\"}', 9);").success, "an INSERT after reopening");
  check(cell(*db2, "SELECT cust FROM j WHERE id = 5;") == "d" && cell(*db2, "SELECT n11 FROM j WHERE id = 5;") == "91",
        "is still worked out");
  check(cell(*db2, "SELECT a_plus FROM m WHERE id = 1;") == "106", "the CHANGEd column too");

  std::cout << "\n=== " << (g_failures == 0 ? "ALL CHECKS PASSED" : "FAILURES: " + std::to_string(g_failures)) << " ===\n";
  return g_failures == 0 ? 0 : 1;
}
