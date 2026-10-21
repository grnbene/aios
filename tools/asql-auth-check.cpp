#undef NDEBUG
// Drives the 3307 door: a wrong password, a wrong user, an empty password and
// the right one, against a server with an account configured. The port used to
// send OK without reading the auth response at all, so `-u nobody -pWRONG`
// opened a session and could read customer records.
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

#include "aios/kernel/kernel.hpp"
#include "aios/storage/object_store.hpp"
#include "aios/userland/mysql_wire.hpp"
#include "aios/userland/sql_database.hpp"

using namespace aios;
using namespace aios::userland;

namespace {

int g_failures = 0;
int g_checks = 0;

void expect_login(uint16_t port, const std::string& label, const std::string& user,
                  const std::string& pass, bool want_in) {
  g_checks++;
  MariaWireSession session("127.0.0.1", port);
  auto r = session.connect(user, pass, "aios_db");
  bool got_in = r.ok();
  if (got_in) {
    auto q = session.query("SELECT stockid FROM stockmaster");
    if (!q.ok() || !q->ok) got_in = false;
  }
  session.close();

  if (got_in != want_in) {
    std::cerr << "  [FAIL] " << label << ": " << (got_in ? "let in" : "refused")
              << ", expected " << (want_in ? "let in" : "refused") << "\n";
    g_failures++;
    return;
  }
  std::cout << "  [ok] " << label << " -> " << (got_in ? "let in" : "refused") << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  // Probe mode: point it at a live engine and report whether that user and
  // password get in. This is how the deployed door gets driven rather than a
  // freshly built one in this process.
  if (argc >= 5) {
    std::string host = argv[1];
    uint16_t port = (uint16_t)std::atoi(argv[2]);
    std::string user = argv[3];
    std::string pass = argv[4];
    std::string sql = argc >= 6 ? argv[5] : "SELECT 1";
    MariaWireSession s(host, port);
    auto r = s.connect(user, pass, "aios_db");
    if (!r.ok()) {
      std::cout << "REFUSED  " << user << "@" << host << ":" << port << "  " << r.error().message << "\n";
      return 2;
    }
    auto q = s.query(sql);
    std::cout << "LET IN   " << user << "@" << host << ":" << port << "  query "
              << ((q.ok() && q->ok) ? "ok" : "failed");
    if (q.ok() && q->ok && !q->rows.empty() && !q->rows[0].empty()) {
      std::cout << ", first cell = " << q->rows[0][0].bytes;
    }
    std::cout << "\n";
    s.close();
    return 0;
  }

  char root_dir[] = "/tmp/aios-auth-XXXXXX";
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
  SqlDatabase db(kernel);
  db.init("sql_db");
  db.execute_query("CREATE TABLE stockmaster (stockid VARCHAR(20), description VARCHAR(50))");
  db.execute_query("INSERT INTO stockmaster VALUES ('T1','Customer record')");

  std::cout << "=== 3307 authentication ===\n";

  {
    std::cout << "\n-- with an account configured --\n";
    MysqlWireServer server(kernel, db, 0, "127.0.0.1");
    server.add_account("erp", "correct horse battery");
    server.set_trust_loopback(false);
    auto sr = server.start();
    if (!sr.ok()) {
      std::cerr << "server failed to start: " << sr.error().message << "\n";
      return 1;
    }
    server.start_background();
    uint16_t port = server.port();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    expect_login(port, "right user, right password", "erp", "correct horse battery", true);
    expect_login(port, "right user, wrong password", "erp", "WRONGPASSWORD", false);
    expect_login(port, "unknown user", "nobody", "WRONGPASSWORD", false);
    expect_login(port, "right user, no password", "erp", "", false);
    expect_login(port, "no user at all", "", "", false);

    server.stop_background();
    server.stop();
  }

  {
    std::cout << "\n-- with no account, loopback trusted --\n";
    MysqlWireServer server(kernel, db, 0, "127.0.0.1");
    server.set_trust_loopback(true);
    auto sr = server.start();
    if (!sr.ok()) return 1;
    server.start_background();
    uint16_t port = server.port();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    expect_login(port, "local client with no account set", "root", "", true);
    server.stop_background();
    server.stop();
  }

  {
    std::cout << "\n-- with no account and loopback not trusted --\n";
    MysqlWireServer server(kernel, db, 0, "127.0.0.1");
    server.set_trust_loopback(false);
    auto sr = server.start();
    if (!sr.ok()) return 1;
    server.start_background();
    uint16_t port = server.port();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    expect_login(port, "closed door refuses everyone", "root", "anything", false);
    server.stop_background();
    server.stop();
  }

  std::cout << "\n=== " << (g_checks - g_failures) << "/" << g_checks << " auth checks pass ===\n";
  return g_failures == 0 ? 0 : 1;
}
