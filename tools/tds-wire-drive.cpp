// AIOS — Driver utility for TdsWireSession (Microsoft SQL Server TDS 7.4+).
// Exercises native TDS packet framing, PRELOGIN handshake, LOGIN7 auth, and query execution.
#include "aios/userland/tds_wire.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace aios::userland;

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  uint16_t port = 1433;
  std::string user = "sa";
  std::string pass = "Password123!";
  std::string db = "master";
  std::string query_sql = "SELECT @@VERSION AS [version], DB_NAME() AS [database], GETDATE() AS [current_time];";

  std::string sslmode = "prefer", ca;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg.rfind("--host=", 0) == 0) host = arg.substr(7);
    else if (arg.rfind("--port=", 0) == 0) port = static_cast<uint16_t>(std::atoi(arg.substr(7).c_str()));
    else if (arg.rfind("--user=", 0) == 0) user = arg.substr(7);
    else if (arg.rfind("--pass=", 0) == 0) pass = arg.substr(7);
    else if (arg.rfind("--db=", 0) == 0) db = arg.substr(5);
    else if (arg.rfind("--query=", 0) == 0) query_sql = arg.substr(8);
    else if (arg.rfind("--sslmode=", 0) == 0) sslmode = arg.substr(10);
    else if (arg.rfind("--ca=", 0) == 0) ca = arg.substr(5);
  }

  printf("=== AIOS Native TDS Wire Driver (MS SQL Server) ===\n");
  printf("Target: tds://%s:***@%s:%u/%s\n\n", user.c_str(), host.c_str(), port, db.c_str());

  TdsWireSession session(host, port);
  session.set_tls(sslmode, ca);

  printf("[1] Connecting and executing TDS 7.4 Handshake (PRELOGIN + LOGIN7)...\n");
  auto t0 = std::chrono::high_resolution_clock::now();
  auto cr = session.connect(user, pass, db);
  auto t1 = std::chrono::high_resolution_clock::now();
  double connect_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

  if (!cr.ok()) {
    printf("[-] Connection / Handshake failed (%.2f ms): %s\n", connect_ms, cr.error().message.c_str());
    return 1;
  }

  printf("[+] CONNECTED in %.2f ms!\n", connect_ms);
  printf("    Server version : %s\n", session.server_version().c_str());
  printf("    Encryption     : %s (%s)\n", session.tls_active() ? "TLS" : "none", session.tls_note().c_str());
  printf("    Active database: %s\n\n", session.database().c_str());

  printf("[2] Verifying connection health via PING (SELECT 1)...\n");
  auto pr = session.ping();
  if (pr.ok()) {
    printf("[+] PING OK\n\n");
  } else {
    printf("[-] PING FAILED: %s\n\n", pr.error().message.c_str());
  }

  printf("[3] Executing query: %s\n", query_sql.c_str());
  auto qt0 = std::chrono::high_resolution_clock::now();
  auto qr = session.query(query_sql);
  auto qt1 = std::chrono::high_resolution_clock::now();
  double query_ms = std::chrono::duration<double, std::milli>(qt1 - qt0).count();

  if (!qr.ok() || !qr->ok) {
    std::string err = !qr.ok() ? qr.error().message : qr->error_message;
    printf("[-] Query failed in %.2f ms: %s\n", query_ms, err.c_str());
    session.close();
    return 1;
  }

  printf("[+] Query succeeded in %.2f ms! Columns: %zu, Rows: %zu\n\n",
         query_ms, qr->columns.size(), qr->rows.size());

  for (size_t c = 0; c < qr->columns.size(); ++c) {
    printf("  Col %zu: %s (%s)\n", c, qr->columns[c].name.c_str(), qr->columns[c].type.c_str());
  }
  printf("\n");

  for (size_t r = 0; r < qr->rows.size(); ++r) {
    printf("  Row %zu: [", r);
    for (size_t c = 0; c < qr->rows[r].size(); ++c) {
      if (c > 0) printf(", ");
      printf("%s", qr->rows[r][c].is_null ? "NULL" : qr->rows[r][c].bytes.c_str());
    }
    printf("]\n");
  }

  session.close();
  printf("\n[+] Clean session termination.\n");
  return 0;
}
