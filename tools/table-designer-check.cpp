#undef NDEBUG
// Proves the backend AIOS-DB-02 (the visual table designer) needs: real
// FOREIGN KEY metadata capture (was silently discarded before today - the
// struct existed, nothing ever filled it in), ALTER TABLE MODIFY/CHANGE
// COLUMN and index/FK add-drop (none of which existed at all), and
// apply_table_design()/generate_alter_table_ddl() - the designer's "Apply
// Changes" path - actually preserving existing row data instead of the
// dead alter_table_structure() this replaced, which would have silently
// wiped every row by swapping in a bare new SqlTable with no col_data.
#include <cassert>
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

void check(bool cond, const std::string& label) {
  if (cond) { std::cout << "  [ok] " << label << "\n"; return; }
  std::cerr << "  [FAIL] " << label << "\n";
  g_failures++;
}

void run_ok(SqlDatabase& db, const std::string& sql) {
  SqlQueryResult r = db.execute_query(sql);
  check(r.success, sql + (r.success ? "" : (" -> " + r.error_message)));
}
}  // namespace

int main() {
  std::cout << "=== AIOS-DB-02 table designer backend ===\n";

  char root_dir[] = "/tmp/aios-tabledesign-XXXXXX";
  if (!mkdtemp(root_dir)) { std::perror("mkdtemp"); return 1; }

  KernelConfig cfg;
  cfg.root = root_dir;
  cfg.residency = test_config;
  auto k_res = Kernel::boot(cfg);
  if (!k_res.ok()) { std::cerr << "boot failed: " << k_res.error().message << "\n"; return 1; }
  Kernel& kernel = *k_res.value();

  TaskLedger ledger(kernel);
  ledger.init("tasks");
  SqlDatabase db(kernel, &ledger);
  db.init("sql_db");

  std::cout << "--- FOREIGN KEY metadata capture (table-level clause) ---\n";
  run_ok(db, "CREATE TABLE customers (id INT PRIMARY KEY, name TEXT)");
  run_ok(db,
    "CREATE TABLE orders (id INT PRIMARY KEY, customer_id INT, amount REAL, "
    "FOREIGN KEY (customer_id) REFERENCES customers(id) ON DELETE CASCADE)");
  {
    SqlTable* t = db.resolve_table("orders");
    check(t != nullptr, "orders table exists");
    if (t) {
      check(t->foreign_keys.size() == 1, "one foreign key captured (was always 0 before today)");
      if (!t->foreign_keys.empty()) {
        auto& fk = t->foreign_keys[0];
        check(fk.column == "customer_id", "fk.column == customer_id");
        check(fk.ref_table == "customers", "fk.ref_table == customers");
        check(fk.ref_column == "id", "fk.ref_column == id");
        check(fk.on_delete == "CASCADE", "fk.on_delete == CASCADE");
      }
    }
  }

  std::cout << "--- FOREIGN KEY metadata capture (inline column clause) ---\n";
  run_ok(db, "CREATE TABLE line_items (id INT PRIMARY KEY, order_id INT REFERENCES orders(id))");
  {
    SqlTable* t = db.resolve_table("line_items");
    check(t && t->foreign_keys.size() == 1, "inline REFERENCES captured");
  }

  std::cout << "--- ALTER TABLE ADD INDEX / DROP INDEX ---\n";
  run_ok(db, "ALTER TABLE orders ADD INDEX idx_amount (amount)");
  {
    SqlTable* t = db.resolve_table("orders");
    check(t && t->indices.size() >= 1, "index added");
  }
  run_ok(db, "ALTER TABLE orders DROP INDEX idx_amount");
  {
    SqlTable* t = db.resolve_table("orders");
    bool found = false;
    if (t) for (auto& ix : t->indices) if (ix.name == "idx_amount") found = true;
    check(!found, "index dropped");
  }

  std::cout << "--- ALTER TABLE ADD CONSTRAINT FOREIGN KEY / DROP FOREIGN KEY ---\n";
  run_ok(db, "CREATE TABLE payments (id INT PRIMARY KEY, order_id INT)");
  run_ok(db, "ALTER TABLE payments ADD CONSTRAINT fk_pay_order FOREIGN KEY (order_id) REFERENCES orders(id)");
  {
    SqlTable* t = db.resolve_table("payments");
    check(t && t->foreign_keys.size() == 1, "FK added via ALTER TABLE");
  }
  run_ok(db, "ALTER TABLE payments DROP FOREIGN KEY fk_pay_order");
  {
    SqlTable* t = db.resolve_table("payments");
    check(t && t->foreign_keys.empty(), "FK dropped via ALTER TABLE");
  }

  std::cout << "--- ALTER TABLE MODIFY / CHANGE COLUMN ---\n";
  run_ok(db, "CREATE TABLE widgets (id INT PRIMARY KEY, price TEXT)");
  run_ok(db, "INSERT INTO widgets VALUES (1, '9.99')");
  run_ok(db, "ALTER TABLE widgets MODIFY COLUMN price REAL NOT NULL");
  {
    SqlTable* t = db.resolve_table("widgets");
    int ci = t ? t->get_column_index("price") : -1;
    check(ci >= 0 && t->columns[(size_t)ci].type == SqlDataType::Real, "MODIFY changed declared type");
    check(ci >= 0 && t->columns[(size_t)ci].not_null, "MODIFY set NOT NULL");
  }
  run_ok(db, "ALTER TABLE widgets CHANGE COLUMN price unit_price REAL");
  {
    SqlTable* t = db.resolve_table("widgets");
    check(t && t->get_column_index("unit_price") >= 0, "CHANGE renamed the column");
    check(t && t->get_column_index("price") < 0, "old name gone");
  }
  {
    SqlQueryResult r = db.execute_query("SELECT id, unit_price FROM widgets");
    check(r.success && r.rows.size() == 1 && r.rows[0].values[1] == "9.99",
          "existing row survived MODIFY+CHANGE with its value intact");
  }

  std::cout << "--- apply_table_design(): the designer's Apply Changes path ---\n";
  run_ok(db, "CREATE TABLE parts (id INT PRIMARY KEY, sku TEXT, qty INT)");
  run_ok(db, "INSERT INTO parts VALUES (1, 'A100', 5)");
  run_ok(db, "INSERT INTO parts VALUES (2, 'B200', 12)");
  {
    std::vector<ColumnEdit> edits;
    // id: unchanged (kept as-is, identity preserved)
    { ColumnEdit e; e.original_name = "id"; e.col.name = "id"; e.col.type = SqlDataType::Int; e.col.is_primary_key = true; edits.push_back(e); }
    // sku: renamed to part_number (identity tracked, not a drop+add)
    { ColumnEdit e; e.original_name = "sku"; e.col.name = "part_number"; e.col.type = SqlDataType::Text; edits.push_back(e); }
    // qty: dropped (no edit row references it)
    // location: brand new column
    { ColumnEdit e; e.col.name = "location"; e.col.type = SqlDataType::Text; edits.push_back(e); }

    auto ddl = db.generate_alter_table_ddl("parts", edits);
    check(ddl.ok(), "generate_alter_table_ddl succeeded");
    std::cout << "  generated DDL:\n" << (ddl.ok() ? *ddl : "") << "\n";

    auto applied = db.apply_table_design("parts", edits);
    check(applied.ok(), "apply_table_design succeeded" + (applied.ok() ? std::string() : (": " + applied.error().message)));
  }
  {
    SqlTable* t = db.resolve_table("parts");
    check(t && t->get_column_index("part_number") >= 0, "sku renamed to part_number, not dropped+recreated");
    check(t && t->get_column_index("sku") < 0, "old name sku gone");
    check(t && t->get_column_index("qty") < 0, "qty dropped as intended");
    check(t && t->get_column_index("location") >= 0, "location added as intended");
    SqlQueryResult r = db.execute_query("SELECT id, part_number FROM parts ORDER BY id");
    check(r.success && r.rows.size() == 2, "both original rows survived the whole design change");
    if (r.success && r.rows.size() == 2) {
      check(r.rows[0].values[1] == "A100" && r.rows[1].values[1] == "B200",
            "renamed column kept its DATA, not just its name (the bug the dead alter_table_structure() had)");
    }
  }

  std::cout << (g_failures == 0 ? "\n>>> ALL PASS\n" : "\n>>> " + std::to_string(g_failures) + " FAILURE(S)\n");
  return g_failures == 0 ? 0 : 1;
}
