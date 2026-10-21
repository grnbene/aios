#undef NDEBUG
// GET_LOCK waiting outside the statement (the lock's holder must be able to
// write while another connection waits for its lock), and the AUTO_INCREMENT id
// an INSERT reports for ON DUPLICATE KEY UPDATE, INSERT IGNORE, REPLACE,
// explicit ids and triggers, as MariaDB does.
#include <atomic>
#include <chrono>
#include <future>
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
using namespace std::chrono_literals;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& label) {
  std::cout << (ok ? "  [ok] " : "  [FAIL] ") << label << "\n";
  if (!ok) g_failures++;
}

SqlQueryResult run(SqlDatabase& db, const std::string& sql) {
  SqlQueryResult r = db.execute_gateway_query(sql);
  if (!r.success) {
    std::cerr << "  statement FAILED: " << r.error_message << "\n    " << sql << "\n";
    g_failures++;
  }
  return r;
}

std::string cell(const SqlQueryResult& r) {
  return r.success && !r.rows.empty() && !r.rows[0].values.empty() ? r.rows[0].values[0] : "<none>";
}

// What the client's insert_id would be after `sql`, and what LAST_INSERT_ID() reads afterwards.
void expect_ids(SqlDatabase& db, const std::string& label, const std::string& sql, uint64_t want_stmt,
                const std::string& want_last) {
  sql_take_statement_insert_id();
  run(db, sql);
  const uint64_t stmt = sql_take_statement_insert_id();
  const std::string last = cell(run(db, "SELECT LAST_INSERT_ID();"));
  check(stmt == want_stmt && last == want_last,
        label + ": insert_id " + std::to_string(stmt) + " (want " + std::to_string(want_stmt) + "), LAST_INSERT_ID() " +
            last + " (want " + want_last + ")");
}

void insert_id_checks(SqlDatabase& db) {
  std::cout << "\n[1] insert ids\n";
  run(db, "CREATE TABLE ids (id INT AUTO_INCREMENT PRIMARY KEY, k VARCHAR(10), v INT, UNIQUE KEY uk (k));");
  run(db, "CREATE TABLE audit (id INT AUTO_INCREMENT PRIMARY KEY, note VARCHAR(20));");
  run(db, "CREATE TRIGGER ids_ai AFTER INSERT ON ids FOR EACH ROW INSERT INTO audit (note) VALUES ('ins');");

  expect_ids(db, "plain insert", "INSERT INTO ids (k, v) VALUES ('a', 1);", 1, "1");
  // audit has taken id 1 in the trigger; the client's id is still ids' own.
  expect_ids(db, "second plain insert (trigger inserts too)", "INSERT INTO ids (k, v) VALUES ('b', 1);", 2, "2");
  expect_ids(db, "ODKU updating a row: no id generated, LAST_INSERT_ID() kept",
             "INSERT INTO ids (k, v) VALUES ('a', 2) ON DUPLICATE KEY UPDATE v = 2;", 1, "2");
  expect_ids(db, "ODKU with id = LAST_INSERT_ID(id)",
             "INSERT INTO ids (k, v) VALUES ('a', 3) ON DUPLICATE KEY UPDATE id = LAST_INSERT_ID(id), v = 3;", 1, "1");
  expect_ids(db, "ODKU inserting a new row", "INSERT INTO ids (k, v) VALUES ('c', 1) ON DUPLICATE KEY UPDATE v = 9;", 5,
             "5");   // the two ODKU updates above used ids 3 and 4, as InnoDB's do
  expect_ids(db, "INSERT IGNORE of a duplicate generates nothing",
             "INSERT IGNORE INTO ids (k, v) VALUES ('c', 5);", 0, "5");
  expect_ids(db, "explicit id", "INSERT INTO ids (id, k, v) VALUES (20, 'd', 1);", 20, "5");
  expect_ids(db, "REPLACE of a row by explicit id", "REPLACE INTO ids (id, k, v) VALUES (20, 'd', 2);", 20, "5");
  expect_ids(db, "INSERT ... SELECT ODKU reports no touched id",
             "INSERT INTO ids (k, v) SELECT 'a', 7 ON DUPLICATE KEY UPDATE v = 7;", 0, "5");
}

void lock_checks(SqlDatabase& db) {
  std::cout << "\n[2] GET_LOCK waits outside the statement\n";
  run(db, "CREATE TABLE lk (id INT AUTO_INCREMENT PRIMARY KEY, n INT);");

  std::atomic<bool> holder_has{false}, holder_wrote{false}, release_now{false};
  std::promise<std::string> waiter_result;
  std::future<std::string> waiter_future = waiter_result.get_future();

  std::thread holder([&] {
    SqlDatabase::set_current_session_id(9001);
    holder_has = cell(run(db, "SELECT GET_LOCK('wl', 10);")) == "1";
    while (!release_now) std::this_thread::sleep_for(5ms);
    run(db, "SELECT RELEASE_LOCK('wl');");
  });
  while (!holder_has) std::this_thread::sleep_for(5ms);

  std::chrono::steady_clock::time_point wait_start{}, wait_end{};
  std::thread waiter([&] {
    SqlDatabase::set_current_session_id(9002);
    wait_start = std::chrono::steady_clock::now();
    std::string v = cell(db.execute_gateway_query("SELECT GET_LOCK('wl', 10);"));
    wait_end = std::chrono::steady_clock::now();
    waiter_result.set_value(v);
  });
  std::this_thread::sleep_for(300ms);   // the waiter is now waiting for the lock

  // The holder writes while the waiter waits: this sat until the waiter timed out.
  std::thread writer([&] {
    SqlDatabase::set_current_session_id(9001);
    auto t0 = std::chrono::steady_clock::now();
    SqlQueryResult w = db.execute_gateway_query("INSERT INTO lk (n) VALUES (1);");
    holder_wrote = w.success && std::chrono::steady_clock::now() - t0 < 3s;
  });
  writer.join();
  check(holder_has, "holder took the lock");
  check(holder_wrote, "the holder's INSERT ran while another connection waited for its lock");
  check(waiter_future.wait_for(0ms) == std::future_status::timeout, "the waiter is still waiting");

  release_now = true;
  holder.join();
  check(waiter_future.wait_for(5s) == std::future_status::ready, "the waiter woke when the lock was released");
  const std::string got = waiter_future.get();
  waiter.join();
  check(got == "1", "the waiter got the lock (got " + got + ")");
  check(wait_end - wait_start >= 250ms, "the waiter really waited");

  // A wait that runs out answers 0, after about the timeout.
  std::thread other([&] {
    SqlDatabase::set_current_session_id(9002);
    auto t0 = std::chrono::steady_clock::now();
    std::string v = cell(db.execute_gateway_query("SELECT GET_LOCK('wl', 0.4);"));   // 9002 holds it: re-entrant
    check(v == "1", "re-entrant GET_LOCK by the holder answers 1");
    (void)t0;
  });
  other.join();
  std::thread third([&] {
    SqlDatabase::set_current_session_id(9003);
    auto t0 = std::chrono::steady_clock::now();
    std::string v = cell(db.execute_gateway_query("SELECT GET_LOCK('wl', 0.5);"));
    auto took = std::chrono::steady_clock::now() - t0;
    check(v == "0", "a wait that times out answers 0 (got " + v + ")");
    check(took >= 400ms && took < 3s, "and only after about its timeout");
  });
  third.join();

  // A statement that can't run twice errors instead of waiting.
  std::thread fourth([&] {
    SqlDatabase::set_current_session_id(9004);
    SqlQueryResult r = db.execute_gateway_query("SELECT GET_LOCK('wl', 5), SLEEP(0);");
    check(!r.success && r.error_message.find("SLEEP()") != std::string::npos,
          "GET_LOCK that must wait in a statement with SLEEP() is an error (" + r.error_message + ")");
  });
  fourth.join();
  std::thread release([&] {
    SqlDatabase::set_current_session_id(9002);
    check(cell(db.execute_gateway_query("SELECT RELEASE_LOCK('wl');")) == "1", "the lock releases once for its holder");
  });
  release.join();
}

}  // namespace

int main() {
  std::cout << "=== asql GET_LOCK wait and insert ids ===\n";
  char root_dir[] = "/tmp/aios-lockid-XXXXXX";
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

  insert_id_checks(db);
  lock_checks(db);

  std::cout << "\n=== " << (g_failures == 0 ? "ALL CHECKS PASSED" : "FAILURES: " + std::to_string(g_failures)) << " ===\n";
  return g_failures == 0 ? 0 : 1;
}
