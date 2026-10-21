#undef NDEBUG
// Verifies the SQL the ERP's open-order board actually issues: NOW() and
// DATEDIFF(), CASE WHEN, a correlated scalar subquery in the SELECT list, and
// IN (SELECT ...). Each check is the shape of a real clause in
// getInvoicedOnDispatch(), not a synthetic one.
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

void run_ok(SqlDatabase& db, const std::string& sql) {
  SqlQueryResult r = db.execute_query(sql);
  if (!r.success) {
    std::cerr << "  setup FAILED: " << r.error_message << "\n    " << sql << "\n";
    g_failures++;
  }
}

// Asserts the first row's named column equals `want`.
void expect_cell(SqlDatabase& db, const std::string& label, const std::string& sql,
                 size_t col, const std::string& want) {
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

void expect_rows(SqlDatabase& db, const std::string& label, const std::string& sql, size_t want) {
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
  std::cout << "  [ok] " << label << " = " << want << " rows\n";
}

}  // namespace

int main() {
  std::cout << "=== asql SQL features the ERP board needs ===\n";

  char root_dir[] = "/tmp/aios-sqlfeatures-XXXXXX";
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

  // --- the shapes the real query joins over ---------------------------------
  run_ok(db, "CREATE TABLE salesorders (orderno INT, deliverydate TEXT, cleared INT);");
  run_ok(db, "CREATE TABLE oms_snoozed (orderno INT, date_end TEXT);");
  run_ok(db, "CREATE TABLE dispatch (invoice INT, cleared INT, oclater INT);");
  run_ok(db, "CREATE TABLE pickingfreeze (orderno INT, pick INT);");

  run_ok(db, "INSERT INTO salesorders (orderno, deliverydate, cleared) VALUES "
             "(1001, '2099-01-01', 0), (1002, '2000-01-01', 0), (1003, '2099-01-01', 0);");
  // 1001 is snoozed into the future; 1002's snooze has expired; 1003 has none.
  run_ok(db, "INSERT INTO oms_snoozed (orderno, date_end) VALUES "
             "(1001, '2099-12-31'), (1002, '2000-01-01');");
  run_ok(db, "INSERT INTO dispatch (invoice, cleared, oclater) VALUES "
             "(1001, 0, 0), (1002, 1, 0), (1003, 0, 0);");
  run_ok(db, "INSERT INTO pickingfreeze (orderno, pick) VALUES "
             "(1001, 2), (1001, 3), (1003, 5);");

  std::cout << "\n[1] NOW() / CURDATE() / DATEDIFF()\n";
  expect_cell(db, "DATEDIFF same day", "SELECT DATEDIFF('2026-09-15','2026-09-15');", 0, "0");
  expect_cell(db, "DATEDIFF one day", "SELECT DATEDIFF('2026-09-16','2026-09-15');", 0, "1");
  expect_cell(db, "DATEDIFF negative", "SELECT DATEDIFF('2026-09-15','2026-09-16');", 0, "-1");
  // Across a month and a leap-year boundary, where a naive 30-day month fails.
  expect_cell(db, "DATEDIFF across Feb 2024", "SELECT DATEDIFF('2024-03-01','2024-02-28');", 0, "2");
  expect_cell(db, "DATEDIFF ignores time", "SELECT DATEDIFF('2026-09-16 00:01:00','2026-09-15 23:59:00');", 0, "1");
  expect_cell(db, "NOW() is not null", "SELECT CASE WHEN NOW() IS NULL THEN 'null' ELSE 'set' END;", 0, "set");
  // Two NOW()s in one statement must be the same instant.
  expect_cell(db, "NOW() stable in statement", "SELECT CASE WHEN NOW() = NOW() THEN 'same' ELSE 'drifted' END;", 0, "same");

  std::cout << "\n[2] CASE WHEN\n";
  expect_cell(db, "searched CASE true", "SELECT CASE WHEN 1=1 THEN 'yes' ELSE 'no' END;", 0, "yes");
  expect_cell(db, "searched CASE false", "SELECT CASE WHEN 1=2 THEN 'yes' ELSE 'no' END;", 0, "no");
  expect_cell(db, "CASE with no ELSE is NULL",
              "SELECT CASE WHEN CASE WHEN 1=2 THEN 'x' END IS NULL THEN 'null' ELSE 'notnull' END;", 0, "null");
  expect_cell(db, "simple CASE", "SELECT CASE 3 WHEN 1 THEN 'one' WHEN 3 THEN 'three' ELSE 'other' END;", 0, "three");
  expect_cell(db, "CASE first match wins",
              "SELECT CASE WHEN 1=1 THEN 'first' WHEN 1=1 THEN 'second' END;", 0, "first");

  std::cout << "\n[2b] NULLIF\n";
  expect_cell(db, "NULLIF equal -> NULL",
              "SELECT CASE WHEN NULLIF('a','a') IS NULL THEN 'null' ELSE 'notnull' END;", 0, "null");
  expect_cell(db, "NULLIF differ -> left", "SELECT NULLIF('a','b');", 0, "a");
  expect_cell(db, "NULLIF empty-sentinel idiom",
              "SELECT CASE WHEN NULLIF(TRIM(IFNULL('','')), '') IS NULL THEN 'absent' ELSE 'present' END;", 0, "absent");
  expect_cell(db, "NULLIF keeps a real value",
              "SELECT CASE WHEN NULLIF(TRIM(IFNULL('  4111  ','')), '') IS NULL THEN 'absent' ELSE 'present' END;", 0, "present");
  expect_cell(db, "NULLIF(NULL,x) -> NULL",
              "SELECT CASE WHEN NULLIF(NULL,'a') IS NULL THEN 'null' ELSE 'notnull' END;", 0, "null");
  expect_cell(db, "NULLIF(a,NULL) -> a", "SELECT NULLIF('a',NULL);", 0, "a");

  std::cout << "\n[3] correlated scalar subquery in the SELECT list\n";
  // This is the `snoozed` column, verbatim in shape.
  expect_cell(db, "snoozed order 1001",
              "SELECT CASE WHEN (SELECT orderno FROM oms_snoozed "
              "WHERE orderno = salesorders.orderno AND date_end > NOW() LIMIT 1) IS NOT NULL "
              "THEN 1 ELSE 0 END FROM salesorders WHERE orderno = 1001;", 0, "1");
  expect_cell(db, "expired snooze 1002 is not snoozed",
              "SELECT CASE WHEN (SELECT orderno FROM oms_snoozed "
              "WHERE orderno = salesorders.orderno AND date_end > NOW() LIMIT 1) IS NOT NULL "
              "THEN 1 ELSE 0 END FROM salesorders WHERE orderno = 1002;", 0, "0");
  expect_cell(db, "never-snoozed 1003",
              "SELECT CASE WHEN (SELECT orderno FROM oms_snoozed "
              "WHERE orderno = salesorders.orderno AND date_end > NOW() LIMIT 1) IS NOT NULL "
              "THEN 1 ELSE 0 END FROM salesorders WHERE orderno = 1003;", 0, "0");
  // An aggregate inside the correlated subquery -- the `fillable` shape.
  expect_cell(db, "correlated SUM subquery",
              "SELECT (SELECT SUM(pick) FROM pickingfreeze WHERE pickingfreeze.orderno = salesorders.orderno) "
              "FROM salesorders WHERE orderno = 1001;", 0, "5");
  // The inner name must win over the outer one of the same name.
  expect_cell(db, "inner scope shadows outer",
              "SELECT (SELECT COUNT(*) FROM dispatch WHERE dispatch.cleared = 1) "
              "FROM salesorders WHERE orderno = 1001;", 0, "1");

  std::cout << "\n[4] IN (SELECT ...) and EXISTS\n";
  // The dispatch predicate, verbatim in shape. 1001 and 1003 qualify.
  expect_rows(db, "IN (SELECT ...)",
              "SELECT orderno FROM salesorders WHERE orderno IN "
              "(SELECT invoice FROM dispatch WHERE cleared = 0 AND oclater = 0);", 2);
  expect_rows(db, "NOT IN (SELECT ...)",
              "SELECT orderno FROM salesorders WHERE orderno NOT IN "
              "(SELECT invoice FROM dispatch WHERE cleared = 0 AND oclater = 0);", 1);
  expect_rows(db, "correlated EXISTS",
              "SELECT orderno FROM salesorders WHERE EXISTS "
              "(SELECT 1 FROM pickingfreeze WHERE pickingfreeze.orderno = salesorders.orderno);", 2);
  expect_rows(db, "NOT EXISTS",
              "SELECT orderno FROM salesorders WHERE NOT EXISTS "
              "(SELECT 1 FROM pickingfreeze WHERE pickingfreeze.orderno = salesorders.orderno);", 1);

  // An INSERT ... SELECT that anti-joins against the table it fills, while that
  // table has never held a row (no column data yet), once segfaulted the engine
  // (fast_in_subquery_scan indexed the missing column).
  run_ok(db, "CREATE TABLE seed_notes (codebase VARCHAR(64) PRIMARY KEY, body LONGTEXT NOT NULL);");
  run_ok(db, "INSERT INTO seed_notes VALUES ('p1', 'one'), ('p2', 'two');");
  run_ok(db, "CREATE TABLE note_items (id BIGINT PRIMARY KEY AUTO_INCREMENT, codebase VARCHAR(64) NOT NULL, body LONGTEXT);");
  run_ok(db, "INSERT INTO note_items (codebase, body) SELECT n.codebase, n.body FROM seed_notes n "
             "WHERE n.codebase NOT IN (SELECT codebase FROM note_items);");
  expect_rows(db, "NOT IN into an empty table it reads", "SELECT id FROM note_items;", 2);
  run_ok(db, "DELETE FROM note_items;");
  run_ok(db, "INSERT INTO note_items (codebase, body) SELECT n.codebase, n.body FROM seed_notes n "
             "WHERE NOT EXISTS (SELECT 1 FROM note_items i WHERE i.codebase = n.codebase);");
  expect_rows(db, "NOT EXISTS into an emptied table it reads", "SELECT id FROM note_items;", 2);

  std::cout << "\n[5] all four together, as the board asks for them\n";
  expect_rows(db, "combined board query",
              "SELECT DISTINCT salesorders.orderno, "
              "DATEDIFF(salesorders.deliverydate, NOW()) AS daystilldue, "
              "CASE WHEN (SELECT orderno FROM oms_snoozed WHERE orderno = salesorders.orderno "
              "           AND date_end > NOW() LIMIT 1) IS NOT NULL THEN 1 ELSE 0 END AS snoozed "
              "FROM salesorders "
              "WHERE salesorders.orderno IN "
              "  (SELECT invoice FROM dispatch WHERE cleared = 0 AND oclater = 0) "
              "GROUP BY salesorders.orderno "
              "ORDER BY salesorders.orderno;", 2);

  std::cout << "\n[6] SQL VIEW support (CREATE VIEW, inlining, SHOW, DROP)\n";
  auto cv1 = db.execute_local_query(
      "CREATE VIEW v_active_orders AS "
      "SELECT orderno, deliverydate FROM salesorders WHERE cleared = 0;");
  if (!cv1.success) {
    std::cout << "  [FAIL] CREATE VIEW: " << cv1.error_message << "\n";
    g_failures++;
  } else {
    std::cout << "  [ok] CREATE VIEW v_active_orders created\n";
  }

  expect_rows(db, "SELECT from simple VIEW",
              "SELECT * FROM v_active_orders;", 3);
  expect_rows(db, "SELECT from VIEW with outer filter",
              "SELECT orderno FROM v_active_orders WHERE orderno = 1001;", 1);

  // Multi-table join VIEW matching stocktrans pattern
  auto cv2 = db.execute_local_query(
      "CREATE OR REPLACE ALGORITHM=UNDEFINED DEFINER=`root`@`localhost` SQL SECURITY DEFINER VIEW v_order_dispatch AS "
      "SELECT salesorders.orderno, salesorders.deliverydate, dispatch.cleared AS dispatch_cleared "
      "FROM salesorders "
      "JOIN dispatch ON salesorders.orderno = dispatch.invoice "
      "WHERE dispatch.cleared = 0;");
  if (!cv2.success) {
    std::cout << "  [FAIL] CREATE VIEW multi-table: " << cv2.error_message << "\n";
    g_failures++;
  } else {
    std::cout << "  [ok] CREATE VIEW v_order_dispatch (multi-table join) created\n";
  }

  expect_rows(db, "SELECT from multi-table join VIEW",
              "SELECT orderno, dispatch_cleared FROM v_order_dispatch WHERE orderno = 1001;", 1);

  // SHOW FULL TABLES test
  auto show_ft = db.execute_local_query("SHOW FULL TABLES;");
  bool found_view = false;
  if (show_ft.success && show_ft.is_result_set) {
    for (const auto& r : show_ft.rows) {
      if (r.values.size() >= 2 && r.values[0] == "v_active_orders" && r.values[1] == "VIEW") {
        found_view = true;
        break;
      }
    }
  }
  if (found_view) {
    std::cout << "  [ok] SHOW FULL TABLES identified v_active_orders as VIEW\n";
  } else {
    std::cout << "  [FAIL] SHOW FULL TABLES did not list VIEW\n";
    g_failures++;
  }

  // SHOW CREATE VIEW test
  auto scv = db.execute_local_query("SHOW CREATE VIEW v_active_orders;");
  if (scv.success && scv.is_result_set && scv.rows.size() == 1) {
    std::cout << "  [ok] SHOW CREATE VIEW returned DDL\n";
  } else {
    std::cout << "  [FAIL] SHOW CREATE VIEW failed: " << scv.error_message << "\n";
    g_failures++;
  }

  // Verify DDL / DML operations on views do not treat views as base tables
  auto dt_view = db.execute_local_query("DROP TABLE v_order_dispatch;");
  if (!dt_view.success && (dt_view.error_message.find("table not found") != std::string::npos ||
                           dt_view.error_message.find("Unknown table") != std::string::npos)) {
    std::cout << "  [ok] DROP TABLE on VIEW rejected as table not found / Unknown table\n";
  } else {
    std::cout << "  [FAIL] DROP TABLE on VIEW: expected table not found, got " << dt_view.error_message << "\n";
    g_failures++;
  }

  auto dt_ie_view = db.execute_local_query("DROP TABLE IF EXISTS v_order_dispatch;");
  if (dt_ie_view.success) {
    std::cout << "  [ok] DROP TABLE IF EXISTS on VIEW is no-op success\n";
  } else {
    std::cout << "  [FAIL] DROP TABLE IF EXISTS on VIEW failed: " << dt_ie_view.error_message << "\n";
    g_failures++;
  }

  auto trunc_view = db.execute_local_query("TRUNCATE TABLE v_order_dispatch;");
  if (!trunc_view.success && trunc_view.error_message.find("is not BASE TABLE") != std::string::npos) {
    std::cout << "  [ok] TRUNCATE TABLE on VIEW rejected\n";
  } else {
    std::cout << "  [FAIL] TRUNCATE TABLE on VIEW: " << trunc_view.error_message << "\n";
    g_failures++;
  }

  auto alter_view = db.execute_local_query("ALTER TABLE v_order_dispatch ADD COLUMN dummy INT;");
  if (!alter_view.success && alter_view.error_message.find("is not BASE TABLE") != std::string::npos) {
    std::cout << "  [ok] ALTER TABLE on VIEW rejected\n";
  } else {
    std::cout << "  [FAIL] ALTER TABLE on VIEW: " << alter_view.error_message << "\n";
    g_failures++;
  }

  auto ins_view = db.execute_local_query("INSERT INTO v_order_dispatch VALUES (1, '2026-01-01', 0);");
  if (!ins_view.success && ins_view.error_message.find("not insertable-into") != std::string::npos) {
    std::cout << "  [ok] INSERT into VIEW rejected\n";
  } else {
    std::cout << "  [FAIL] INSERT into VIEW: " << ins_view.error_message << "\n";
    g_failures++;
  }

  auto upd_view = db.execute_local_query("UPDATE v_order_dispatch SET dispatch_cleared = 1;");
  if (!upd_view.success && upd_view.error_message.find("not updatable") != std::string::npos) {
    std::cout << "  [ok] UPDATE on VIEW rejected\n";
  } else {
    std::cout << "  [FAIL] UPDATE on VIEW: " << upd_view.error_message << "\n";
    g_failures++;
  }

  auto del_view = db.execute_local_query("DELETE FROM v_order_dispatch;");
  if (!del_view.success && del_view.error_message.find("not updatable") != std::string::npos) {
    std::cout << "  [ok] DELETE on VIEW rejected\n";
  } else {
    std::cout << "  [FAIL] DELETE on VIEW: " << del_view.error_message << "\n";
    g_failures++;
  }

  auto ct_conflict = db.execute_local_query("CREATE TABLE v_order_dispatch (id INT);");
  if (!ct_conflict.success && ct_conflict.error_message.find("already exists") != std::string::npos) {
    std::cout << "  [ok] CREATE TABLE conflicting with existing VIEW rejected\n";
  } else {
    std::cout << "  [FAIL] CREATE TABLE conflicting with VIEW: " << ct_conflict.error_message << "\n";
    g_failures++;
  }

  // DROP VIEW test
  auto dv = db.execute_local_query("DROP VIEW v_active_orders;");
  if (dv.success) {
    std::cout << "  [ok] DROP VIEW v_active_orders\n";
  } else {
    std::cout << "  [FAIL] DROP VIEW: " << dv.error_message << "\n";
    g_failures++;
  }


  // TO_DAYS scalar function tests
  expect_cell(db, "TO_DAYS('2026-09-16')", "SELECT TO_DAYS('2026-09-16');", 0, "740240");
  expect_cell(db, "TO_DAYS('2000-02-29')", "SELECT TO_DAYS('2000-02-29');", 0, "730544");
  expect_cell(db, "TO_DAYS diff", "SELECT TO_DAYS('2026-09-16') - TO_DAYS('2026-09-01');", 0, "15");

  // REGEXP_REPLACE tests
  run_ok(db, "CREATE TABLE taxgroups (taxgroupid INT, taxgroupdescription TEXT);");
  run_ok(db, "INSERT INTO taxgroups VALUES (1, '100 No Tax Code Assigned');");
  run_ok(db, "INSERT INTO taxgroups VALUES (2, '200 Standard Tax');");
  expect_cell(db, "REGEXP_REPLACE scalar", "SELECT REGEXP_REPLACE('100 No Tax Code Assigned', '[0-9]+', '');", 0, " No Tax Code Assigned");
  expect_cell(db, "ORDER BY REGEXP_REPLACE", "SELECT taxgroupid FROM taxgroups ORDER BY REGEXP_REPLACE(taxgroupdescription, '[0-9]+', '') ASC, taxgroupid ASC LIMIT 1;", 0, "1");

  // A view of several tables joined AFTER another table, as st's report does:
  // `productlinepoints p JOIN stocktrans s ON p.lineid = s.lineid`, where
  // lineid is stocktrans's stockmaster.lineid, not its first table's. Merged,
  // the ON was tried before stockmaster joined: "no such column:
  // stockmaster.lineid (in scope: productlinepoints AS p, stockmoves)".
  std::cout << "\n[6b] a several-table VIEW joined after another table\n";
  run_ok(db, "CREATE TABLE vj_lines (lineid INT PRIMARY KEY, name VARCHAR(20))");
  run_ok(db, "CREATE TABLE vj_master (stockid VARCHAR(10) PRIMARY KEY, lineid INT)");
  run_ok(db, "CREATE TABLE vj_moves (stkmoveno INT PRIMARY KEY, stockid VARCHAR(10), amt INT)");
  run_ok(db, "INSERT INTO vj_lines VALUES (1,'Hair'),(2,'Nails')");
  run_ok(db, "INSERT INTO vj_master VALUES ('A',1),('B',2),('C',1)");
  run_ok(db, "INSERT INTO vj_moves VALUES (1,'A',10),(2,'A',5),(3,'B',7),(4,'C',1),(5,'Z',100)");
  run_ok(db, "CREATE VIEW vj_trans AS SELECT vj_moves.stkmoveno, vj_moves.amt, vj_master.lineid "
             "FROM vj_moves JOIN vj_master ON vj_master.stockid = vj_moves.stockid");
  // Its columns named bare, as `SELECT lineid ... FROM productlines` does.
  run_ok(db, "CREATE VIEW vj_points AS SELECT lineid, name FROM vj_lines");
  expect_cell(db, "table JOIN view on the view's second table (st's report)",
              "SELECT p.name, SUM(s.amt) FROM vj_lines p JOIN vj_trans s ON p.lineid = s.lineid "
              "GROUP BY p.name ORDER BY p.name", 1, "16");
  expect_rows(db, "two views, the second's columns named bare",
              "SELECT s.stkmoveno, p.name FROM vj_trans s JOIN vj_points p ON p.lineid = s.lineid", 4);
  expect_rows(db, "table, table, then the view",
              "SELECT m.stockid FROM vj_lines p JOIN vj_master m ON m.lineid = p.lineid "
              "JOIN vj_trans s ON s.lineid = p.lineid AND s.amt > 6", 3);
  {
    // SELECT * is every source's columns, not the view's alone.
    SqlQueryResult r = db.execute_query(
        "SELECT * FROM vj_lines p JOIN vj_trans s ON p.lineid = s.lineid WHERE s.stkmoveno = 1");
    if (!r.success || r.rows.size() != 1 || r.rows[0].values.size() != 5) {
      std::cerr << "  [FAIL] SELECT * over table JOIN view: "
                << (r.success ? std::to_string(r.rows.empty() ? 0 : r.rows[0].values.size()) + " columns, want 5"
                              : r.error_message) << "\n";
      g_failures++;
    } else {
      std::cout << "  [ok] SELECT * over table JOIN view = 5 columns\n";
    }
  }

  std::cout << "\n[7] Stored Procedures (CREATE PROCEDURE, getNextTransNo from ERP dump, IN/OUT/INOUT)\n";
  run_ok(db, "CREATE TABLE systypes (typeid SMALLINT, typeno INT);");
  run_ok(db, "INSERT INTO systypes (typeid, typeno) VALUES (10, 100);");

  // getNextTransNo exactly as defined in the ERP schema (/repos/aa.sql)
  run_ok(db,
         "CREATE DEFINER=`api`@`%` PROCEDURE `getNextTransNo`(IN `typeidno` SMALLINT(6), OUT `newtypeno` INT(11))\n"
         "    MODIFIES SQL DATA\n"
         "    COMMENT 'Replace stupid lock crap'\n"
         "BEGIN\n"
         "    DECLARE curr_typeno INT;\n"
         "    START TRANSACTION;\n"
         "    SELECT typeno INTO curr_typeno FROM systypes WHERE typeid = typeidno;\n"
         "    SET newtypeno=curr_typeno + 1;\n"
         "    UPDATE systypes SET typeno=newtypeno WHERE typeid=typeidno;\n"
         "    COMMIT;\n"
         "END");

  run_ok(db, "CALL getNextTransNo(10, @nextid);");
  expect_cell(db, "getNextTransNo first call @nextid", "SELECT @nextid;", 0, "101");
  expect_cell(db, "systypes table typeno updated", "SELECT typeno FROM systypes WHERE typeid = 10;", 0, "101");

  run_ok(db, "CALL getNextTransNo(10, @nextid);");
  expect_cell(db, "getNextTransNo second call @nextid", "SELECT @nextid;", 0, "102");
  expect_cell(db, "systypes table typeno incremented again", "SELECT typeno FROM systypes WHERE typeid = 10;", 0, "102");

  run_ok(db, "CALL getNextTransNo(999, @nextid);");
  expect_cell(db, "getNextTransNo nonexistent type produces NULL", "SELECT CASE WHEN @nextid IS NULL THEN 'null' ELSE 'notnull' END;", 0, "null");

  // Stored procedure with IN, INOUT, OUT and local DECLARE with DEFAULT
  run_ok(db,
         "CREATE PROCEDURE test_calc(IN a INT, INOUT b INT, OUT c INT)\n"
         "BEGIN\n"
         "    DECLARE factor INT DEFAULT 2;\n"
         "    SET c = a + b;\n"
         "    SET b = b * factor;\n"
         "END");
  run_ok(db, "SET @inout_b = 5;");
  run_ok(db, "CALL test_calc(10, @inout_b, @out_c);");
  expect_cell(db, "test_calc OUT c", "SELECT @out_c;", 0, "15");
  expect_cell(db, "test_calc INOUT b", "SELECT @inout_b;", 0, "10");

  // SHOW PROCEDURE STATUS test
  auto sp_status = db.execute_local_query("SHOW PROCEDURE STATUS;");
  bool found_sp = false;
  if (sp_status.success && sp_status.is_result_set) {
    for (const auto& r : sp_status.rows) {
      if (r.values.size() >= 2 && r.values[1] == "getNextTransNo") {
        found_sp = true;
        break;
      }
    }
  }
  if (found_sp) {
    std::cout << "  [ok] SHOW PROCEDURE STATUS found getNextTransNo\n";
  } else {
    std::cout << "  [FAIL] SHOW PROCEDURE STATUS did not list getNextTransNo\n";
    g_failures++;
  }

  // SHOW CREATE PROCEDURE test
  auto scp = db.execute_local_query("SHOW CREATE PROCEDURE getNextTransNo;");
  if (scp.success && scp.is_result_set && !scp.rows.empty()) {
    std::cout << "  [ok] SHOW CREATE PROCEDURE getNextTransNo succeeded\n";
  } else {
    std::cout << "  [FAIL] SHOW CREATE PROCEDURE failed: " << scp.error_message << "\n";
    g_failures++;
  }

  // DROP PROCEDURE test
  run_ok(db, "DROP PROCEDURE test_calc;");
  auto drop_call = db.execute_local_query("CALL test_calc(1, @inout_b, @out_c);");
  if (!drop_call.success) {
    std::cout << "  [ok] CALL dropped procedure failed as expected\n";
  } else {
    std::cout << "  [FAIL] CALL dropped procedure should have failed\n";
    g_failures++;
  }

  // Derived table with UNION ((SELECT ...) UNION (SELECT ...))
  expect_rows(db, "union inside derived table ((SELECT ...) UNION (SELECT ...))",
              "SELECT * FROM ((SELECT 1 AS a) UNION (SELECT 2 AS a)) AS u;", 2);
  expect_rows(db, "union inside derived table ((SELECT ...) UNION ALL (SELECT ...))",
              "SELECT * FROM ((SELECT 1 AS a) UNION ALL (SELECT 2 AS a)) AS u;", 2);
  expect_rows(db, "derived table ((SELECT 1 AS a UNION SELECT 2 AS a))",
              "SELECT * FROM ((SELECT 1 AS a UNION SELECT 2 AS a)) AS u;", 2);
  expect_rows(db, "derived table (((SELECT 1 AS a) UNION (SELECT 2 AS a)))",
              "SELECT * FROM (((SELECT 1 AS a) UNION (SELECT 2 AS a))) AS u;", 2);
  expect_rows(db, "join with union derived table",
              "SELECT * FROM salesorders o JOIN ((SELECT 1001 AS orderno) UNION (SELECT 1002 AS orderno)) u ON o.orderno = u.orderno;", 2);
  expect_rows(db, "parenthesised join group with derived tables",
              "SELECT * FROM ((SELECT 1 AS a) AS dt1 JOIN (SELECT 1 AS a) AS dt2 ON dt1.a = dt2.a);", 1);

  std::cout << "\n=== " << (g_failures == 0 ? "ALL CHECKS PASSED" : "FAILURES: " + std::to_string(g_failures))
            << " ===\n";
  return g_failures == 0 ? 0 : 1;
}
