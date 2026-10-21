#undef NDEBUG
// A connection waiting in GET_LOCK ends on KILL QUERY / KILL CONNECTION: the wait wakes at once, the statement fails
// with "Query execution was interrupted", and the locks the statement had already taken are released. Every case
// is a real second connection waiting on a lock a first one holds, killed from a third.
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

SqlQueryResult run(SqlDatabase& db, const std::string& sql) { return db.execute_gateway_query(sql); }

std::string cell(const SqlQueryResult& r) {
  return r.success && !r.rows.empty() && !r.rows[0].values.empty() ? r.rows[0].values[0] : "<" + r.error_message + ">";
}

// A connection of its own: this thread's session, registered so KILL can reach it.
void as_session(uint64_t id) {
  SqlDatabase::set_current_session_id(id);
  SqlDatabase::register_active_session(id, "root", "127.0.0.1", "", "Sleep", "idle", "");
}

struct Waiter {
  std::future<SqlQueryResult> result;
  std::chrono::steady_clock::time_point started;
};

// Starts `sql` as connection `id` and gives it a moment to be waiting.
Waiter start_waiting(SqlDatabase& db, uint64_t id, const std::string& sql) {
  auto pr = std::make_shared<std::promise<SqlQueryResult>>();
  Waiter w{pr->get_future(), std::chrono::steady_clock::now()};
  std::thread([&db, id, sql, pr] {
    as_session(id);
    pr->set_value(run(db, sql));
    SqlDatabase::unregister_active_session(id);
  }).detach();
  std::this_thread::sleep_for(300ms);
  return w;
}

}  // namespace

int main() {
  std::cout << "=== asql GET_LOCK wait ends on KILL ===\n";
  char root_dir[] = "/tmp/aios-lockkill-XXXXXX";
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

  as_session(9101);   // connection A, this thread: holds 'x'
  check(cell(run(db, "SELECT GET_LOCK('x', 0);")) == "1", "A takes x");

  std::cout << "\n[1] KILL QUERY\n";
  {
    Waiter b = start_waiting(db, 9102, "SELECT GET_LOCK('x', 30);");
    check(b.result.wait_for(0ms) == std::future_status::timeout, "B is waiting for x");
    check(cell(run(db, "KILL QUERY 9102;")).find("interrupted") == std::string::npos, "the KILL statement itself succeeds");
    check(b.result.wait_for(2s) == std::future_status::ready, "B returns within 2 s of the KILL");
    if (b.result.wait_for(0ms) == std::future_status::ready) {
      SqlQueryResult r = b.result.get();
      check(!r.success && r.error_message == "Query execution was interrupted", "B's statement fails with 'Query execution was interrupted' (got: " + r.error_message + ")");
      const auto took = std::chrono::steady_clock::now() - b.started;
      check(took < 2500ms, "and long before its 30 s timeout");
    }
    check(cell(run(db, "SELECT IS_USED_LOCK('x');")) == "9101", "A still holds x");
  }

  std::cout << "\n[2] KILL CONNECTION\n";
  {
    Waiter b = start_waiting(db, 9103, "SELECT GET_LOCK('x', 30);");
    check(b.result.wait_for(0ms) == std::future_status::timeout, "B is waiting for x");
    run(db, "KILL CONNECTION 9103;");
    check(b.result.wait_for(2s) == std::future_status::ready, "B returns within 2 s of the KILL CONNECTION");
    if (b.result.wait_for(0ms) == std::future_status::ready) {
      SqlQueryResult r = b.result.get();
      check(!r.success && r.error_message == "Query execution was interrupted", "B's statement fails with 'Query execution was interrupted' (got: " + r.error_message + ")");
    }
  }

  std::cout << "\n[3] a lock the killed statement had already taken is released\n";
  {
    Waiter b = start_waiting(db, 9104, "SELECT GET_LOCK('y', 0), GET_LOCK('x', 30);");
    check(cell(run(db, "SELECT IS_USED_LOCK('y');")) == "9104", "B holds y while it waits for x");
    run(db, "KILL QUERY 9104;");
    check(b.result.wait_for(2s) == std::future_status::ready, "B returns within 2 s");
    if (b.result.wait_for(0ms) == std::future_status::ready) b.result.get();
    check(cell(run(db, "SELECT IS_FREE_LOCK('y');")) == "1", "y is free again after the kill");
  }

  std::cout << "\n[4] a KILL QUERY does not poison the connection's next statement\n";
  {
    auto done = std::make_shared<std::promise<std::string>>();
    auto fut = done->get_future();
    std::thread([&db, done] {
      as_session(9105);
      SqlDatabase::update_active_session(9105, "Query", "executing", "x");
      SqlQueryResult w = run(db, "SELECT GET_LOCK('x', 30);");
      SqlQueryResult next = run(db, "SELECT 1;");
      done->set_value((w.success ? std::string("not killed") : w.error_message) + "|" + cell(next));
      SqlDatabase::unregister_active_session(9105);
    }).detach();
    std::this_thread::sleep_for(300ms);
    run(db, "KILL QUERY 9105;");
    check(fut.wait_for(3s) == std::future_status::ready, "B returns");
    if (fut.wait_for(0ms) == std::future_status::ready)
      check(fut.get() == "Query execution was interrupted|1", "B's next statement runs normally");
  }

  std::cout << "\n[5] without a KILL the wait still works\n";
  {
    Waiter b = start_waiting(db, 9106, "SELECT GET_LOCK('x', 30);");
    run(db, "SELECT RELEASE_LOCK('x');");
    check(b.result.wait_for(2s) == std::future_status::ready, "B wakes when A releases x");
    if (b.result.wait_for(0ms) == std::future_status::ready) check(cell(b.result.get()) == "1", "and gets it");
  }

  std::cout << "\n=== " << (g_failures == 0 ? "ALL CHECKS PASSED" : "FAILURES: " + std::to_string(g_failures)) << " ===\n";
  return g_failures == 0 ? 0 : 1;
}
