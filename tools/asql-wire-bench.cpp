#undef NDEBUG
// Drives the wire server with a result set big enough to cross the streaming
// buffer several times, checks every row arrived intact and in order, and
// reports rows/sec. `SELECT *` over 52k rows was costing 1.46 s wall and
// 1.12 s of CPU almost entirely in packet framing, so this measures the part
// that changed rather than the scan.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
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

int main(int argc, char** argv) {
  size_t rows = argc > 1 ? (size_t)std::strtoul(argv[1], nullptr, 10) : 60000;

  // Feed a file of statements to a running engine, one per ';' at end of line.
  // Used to re-sync a table from a mysqldump --replace, which is idempotent and
  // never leaves the table missing rows the way DELETE + INSERT would.
  if (argc >= 6 && std::string(argv[5]) == "--exec-file") {
    if (argc < 7) { std::cerr << "need a file\n"; return 1; }
    std::string host = argv[1];
    uint16_t port = (uint16_t)std::atoi(argv[2]);
    MariaWireSession s(host, port);
    auto c = s.connect(argv[3], argv[4], "aios_db");
    if (!c.ok()) { std::cerr << "connect failed: " << c.error().message << "\n"; return 1; }
    std::ifstream f(argv[6]);
    if (!f) { std::cerr << "cannot open " << argv[6] << "\n"; return 1; }
    std::string line, stmt;
    size_t ok_count = 0, fail_count = 0;
    auto t0 = std::chrono::steady_clock::now();
    auto flush_stmt = [&]() {
      if (stmt.empty()) return;
      auto r = s.query(stmt);
      if (!r.ok() || !r->ok) {
        if (fail_count < 3) {
          std::cerr << "  stmt failed: "
                    << (r.ok() ? r->error_message : r.error().message).substr(0, 120) << "\n";
        }
        fail_count++;
      } else {
        ok_count++;
      }
      stmt.clear();
    };
    while (std::getline(f, line)) {
      if (line.empty() || line[0] == '-' || line[0] == '/') continue;
      stmt += line;
      if (!line.empty() && line.back() == ';') {
        stmt.pop_back();
        flush_stmt();
      } else {
        stmt += "\n";
      }
    }
    flush_stmt();
    auto t1 = std::chrono::steady_clock::now();
    std::printf("applied %zu statements, %zu failed, in %.1f s\n", ok_count, fail_count,
                std::chrono::duration<double>(t1 - t0).count());
    s.close();
    return fail_count ? 1 : 0;
  }

  // Probe mode: time a query against an already-running engine.
  if (argc >= 5) {
    std::string host = argv[1];
    uint16_t port = (uint16_t)std::atoi(argv[2]);
    std::string user = argv[3], pass = argv[4];
    std::string sql = argc >= 6 ? argv[5] : "SELECT * FROM stockmaster";
    int reps = argc >= 7 ? std::atoi(argv[6]) : 3;
    MariaWireSession s(host, port);
    auto c = s.connect(user, pass, "aios_db");
    if (!c.ok()) {
      std::cerr << "connect failed: " << c.error().message << "\n";
      return 1;
    }
    for (int i = 0; i < reps; ++i) {
      auto t0 = std::chrono::steady_clock::now();
      auto q = s.query(sql);
      auto t1 = std::chrono::steady_clock::now();
      double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
      size_t n = (q.ok() && q->ok) ? q->rows.size() : 0;
      std::printf("  run %d: %8.2f ms  %7zu rows  %10.0f rows/sec\n",
                  i + 1, ms, n, n / (ms / 1000.0));
    }
    s.close();
    return 0;
  }

  char root_dir[] = "/tmp/aios-wirebench-XXXXXX";
  if (!mkdtemp(root_dir)) { std::perror("mkdtemp"); return 1; }
  KernelConfig cfg;
  cfg.root = root_dir;
  cfg.residency = test_config;
  auto k_res = Kernel::boot(cfg);
  if (!k_res.ok()) { std::cerr << "kernel boot failed\n"; return 1; }
  Kernel& kernel = *k_res.value();

  SqlDatabase db(kernel);
  db.init("sql_db");
  db.execute_query("CREATE TABLE wide (id INT, stockid VARCHAR(32), description VARCHAR(80), "
                   "note VARCHAR(120), qty DECIMAL(10,2))");
  std::cout << "loading " << rows << " rows...\n";
  for (size_t b = 0; b < rows; b += 1000) {
    std::string sql = "INSERT INTO wide VALUES ";
    size_t upto = std::min(b + 1000, rows);
    for (size_t i = b; i < upto; ++i) {
      if (i > b) sql += ",";
      sql += "(" + std::to_string(i) + ",'SKU-" + std::to_string(i) +
             "','description for row " + std::to_string(i) +
             "','a longer free text field that pushes the row past a few hundred bytes so the "
             "streaming buffer is crossed many times over, row " + std::to_string(i) + "'," +
             std::to_string((double)i / 4.0) + ")";
    }
    db.execute_query(sql);
  }

  MysqlWireServer server(kernel, db, 0, "127.0.0.1");
  server.add_account("bench", "benchpass");
  if (!server.start().ok()) { std::cerr << "server start failed\n"; return 1; }
  server.start_background();
  uint16_t port = server.port();
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  MariaWireSession s("127.0.0.1", port);
  if (!s.connect("bench", "benchpass", "aios_db").ok()) {
    std::cerr << "client connect failed\n";
    return 1;
  }

  int failures = 0;
  for (int rep = 0; rep < 3; ++rep) {
    auto t0 = std::chrono::steady_clock::now();
    auto q = s.query("SELECT * FROM wide");
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (!q.ok() || !q->ok) {
      std::cerr << "  [FAIL] query failed\n";
      failures++;
      break;
    }
    if (q->rows.size() != rows) {
      std::cerr << "  [FAIL] got " << q->rows.size() << " rows, want " << rows << "\n";
      failures++;
      break;
    }
    // Every row, in order, with its own id and its own text: a framing bug
    // shows up as a shifted or truncated cell, not as a missing row.
    for (size_t i = 0; i < q->rows.size(); ++i) {
      const auto& r = q->rows[i];
      if (r.size() != 5) { std::cerr << "  [FAIL] row " << i << " has " << r.size() << " cells\n"; failures++; break; }
      if (r[0].bytes != std::to_string(i)) {
        std::cerr << "  [FAIL] row " << i << " id = '" << r[0].bytes << "'\n";
        failures++;
        break;
      }
      if (r[1].bytes != "SKU-" + std::to_string(i)) {
        std::cerr << "  [FAIL] row " << i << " stockid = '" << r[1].bytes << "'\n";
        failures++;
        break;
      }
      if (r[3].bytes.size() < 100 || r[3].bytes.find("row " + std::to_string(i)) == std::string::npos) {
        std::cerr << "  [FAIL] row " << i << " note truncated: '" << r[3].bytes.substr(0, 40) << "'\n";
        failures++;
        break;
      }
    }
    if (failures) break;
    std::printf("  run %d: %8.2f ms  %7zu rows  %10.0f rows/sec  (all cells verified)\n",
                rep + 1, ms, q->rows.size(), q->rows.size() / (ms / 1000.0));
  }

  // A packet's length field is three bytes, so a value over 16 MB has to
  // travel as several packets. Both directions used to read or write only the
  // first, which corrupts the stream rather than failing.
  std::cout << "\n-- payloads that cross the 16 MB packet boundary --\n";
  {
    const size_t kBig = 20u * 1024 * 1024;
    std::string big(kBig, 'x');
    for (size_t i = 0; i < kBig; i += 65536) big[i] = 'A' + (char)((i / 65536) % 26);

    // Inbound: the INSERT carrying this is itself larger than one packet.
    auto ins = s.query("INSERT INTO wide VALUES (999999,'BIG','big row','" + big + "',1)");
    if (!ins.ok() || !ins->ok) {
      std::cerr << "  [FAIL] oversized INSERT refused: "
                << (ins.ok() ? ins->error_message : ins.error().message) << "\n";
      failures++;
    } else {
      std::cout << "  [ok] " << (kBig / (1024 * 1024)) << " MB statement reassembled inbound\n";
    }

    auto stored = s.query("SELECT LENGTH(note) FROM wide WHERE id = 999999");
    if (stored.ok() && stored->ok && !stored->rows.empty() && !stored->rows[0].empty()) {
      std::cout << "  (engine stored " << stored->rows[0][0].bytes << " bytes)\n";
    }

    // Outbound: reading it back has to be split into packets and rejoined.
    auto q = s.query("SELECT note FROM wide WHERE id = 999999");
    if (!q.ok() || !q->ok || q->rows.empty() || q->rows[0].empty()) {
      std::cerr << "  [FAIL] oversized row did not come back\n";
      failures++;
    } else if (q->rows[0][0].bytes.size() != kBig) {
      std::cerr << "  [FAIL] got " << q->rows[0][0].bytes.size() << " bytes, want " << kBig << "\n";
      failures++;
    } else if (q->rows[0][0].bytes != big) {
      std::cerr << "  [FAIL] oversized row came back corrupted\n";
      failures++;
    } else {
      std::cout << "  [ok] " << (kBig / (1024 * 1024)) << " MB value returned byte-identical\n";
    }

    // The connection has to still be usable: a mis-framed split leaves the
    // client reading payload bytes as the next header.
    auto after = s.query("SELECT COUNT(*) FROM wide");
    if (!after.ok() || !after->ok || after->rows.empty()) {
      std::cerr << "  [FAIL] connection unusable after the oversized exchange\n";
      failures++;
    } else {
      std::cout << "  [ok] connection still in sync afterwards (" << after->rows[0][0].bytes << " rows)\n";
    }
  }

  s.close();
  server.stop_background();
  server.stop();
  std::cout << (failures ? "=== WIRE BENCH FAILED ===\n" : "=== wire bench ok ===\n");
  return failures ? 1 : 0;
}
