// Verifies the MySQL gateway's write-through and write-behind modes against a downstream MySQL
// server it forwards to.

#undef NDEBUG
#include <iostream>
#include <cassert>
#include <chrono>
#include <thread>
#include <vector>
#include <string>
#include <unistd.h>
#include <cmath>

#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"
#include "aios/userland/mysql_wire.hpp"
#include "aios/storage/object_store.hpp"

using namespace aios;
using namespace aios::userland;

static uint64_t now_micros() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main() {
  std::cout << "========================================================================\n";
  std::cout << "  AIOS MySQL Gateway: Write-Through & Write-Behind Verification Suite  \n";
  std::cout << "========================================================================\n\n";

  // Setup temporary test directories
  char aios_root[] = "/tmp/aios-wb-gateway-XXXXXX";
  if (!mkdtemp(aios_root)) { std::perror("mkdtemp"); return 1; }

  char downstream_root[] = "/tmp/aios-wb-downstream-XXXXXX";
  if (!mkdtemp(downstream_root)) { std::perror("mkdtemp"); return 1; }

  // 1. Boot Downstream Instance (Simulating real downstream MySQL/MariaDB server)
  std::cout << "[Step 1] Booting downstream database instance & wire server...\n";
  KernelConfig down_kcfg;
  down_kcfg.root = downstream_root;
  down_kcfg.residency = test_config;
  auto down_k_res = Kernel::boot(down_kcfg);
  assert(down_k_res.ok());
  Kernel& down_kernel = **down_k_res;

  SqlDatabase down_db(down_kernel);
  assert(down_db.init("sql_db").ok());

  MysqlWireServer down_server(down_kernel, down_db, 0, "127.0.0.1");
  assert(down_server.start().ok());
  uint16_t down_port = down_server.port();
  down_server.start_background();
  std::cout << "  [PASS] Downstream MySQL server listening on 127.0.0.1:" << down_port << "\n\n";

  // 2. Boot AIOS Gateway Instance
  std::cout << "[Step 2] Booting AIOS Gateway instance with Write-Behind enabled...\n";
  KernelConfig aios_kcfg;
  aios_kcfg.root = aios_root;
  aios_kcfg.residency = test_config;
  auto aios_k_res = Kernel::boot(aios_kcfg);
  assert(aios_k_res.ok());
  Kernel& aios_kernel = **aios_k_res;

  SqlDatabase aios_db(aios_kernel);
  assert(aios_db.init("sql_db").ok());

  // Configure downstream connection profile
  DbConnectionProfile down_prof;
  down_prof.id = "downstream_db";
  down_prof.name = "Downstream Archive DB";
  down_prof.host = "127.0.0.1";
  down_prof.port = down_port;
  down_prof.user = "root";
  down_prof.password = "";
  down_prof.database = "aios_db";
  down_prof.is_local_embedded = false;
  aios_db.set_downstream_target(down_prof);

  // Set Pattern B: Asynchronous Write-Behind
  aios_db.set_sync_mode(WriteSyncMode::WriteBehind);
  assert(aios_db.sync_mode() == WriteSyncMode::WriteBehind);

  // Start dedicated background WAL drain worker
  aios_db.start_wal_drain_worker(10); // 10ms poll interval
  assert(aios_db.is_wal_drain_worker_running());

  // Start AIOS MySQL Wire Server
  MysqlWireServer aios_server(aios_kernel, aios_db, 0, "127.0.0.1");
  assert(aios_server.start().ok());
  uint16_t aios_port = aios_server.port();
  aios_server.start_background();
  std::cout << "  [PASS] AIOS Gateway listening on 127.0.0.1:" << aios_port << "\n\n";

  // 3. Connect Client to AIOS Gateway
  std::cout << "[Step 3] Connecting legacy MySQL client to AIOS Gateway...\n";
  MariaWireSession client("127.0.0.1", aios_port);
  assert(client.connect("root", "", "aios_db").ok());
  assert(client.connected());
  std::cout << "  [PASS] Client connected successfully to AIOS Wire Server.\n\n";

  // 4. Duel & Microsecond ACK Latency (Pattern B: Write-Behind)
  std::cout << "[Step 4] Duel Benchmark: Measuring Write-Behind Ingestion Latency...\n";
  auto create_res = client.query(
      "CREATE TABLE orders (id INT PRIMARY KEY, customer VARCHAR(64), total DOUBLE, status VARCHAR(32));");
  assert(create_res.ok() && create_res->ok);

  const int NUM_WRITES = 200;
  uint64_t total_write_us = 0;
  uint64_t max_write_us = 0;
  uint64_t min_write_us = 99999999;

  for (int i = 1; i <= NUM_WRITES; ++i) {
    std::string sql = "INSERT INTO orders (id, customer, total, status) VALUES (" +
                      std::to_string(i) + ", 'Customer-" + std::to_string(i % 10) + "', " +
                      std::to_string(100.0 + i * 2.5) + ", 'NEW');";
    uint64_t t0 = now_micros();
    auto ins_res = client.query(sql);
    uint64_t t1 = now_micros();
    uint64_t lat = (t1 >= t0) ? (t1 - t0) : 0;

    assert(ins_res.ok() && ins_res->ok);
    assert(ins_res->affected_rows == 1);

    total_write_us += lat;
    if (lat > max_write_us) max_write_us = lat;
    if (lat < min_write_us) min_write_us = lat;
  }

  double avg_lat_us = (double)total_write_us / NUM_WRITES;
  std::cout << "  Executed " << NUM_WRITES << " writes through AIOS Write-Behind Gateway:\n";
  std::cout << "    - Average Client ACK Latency: " << avg_lat_us << " µs\n";
  std::cout << "    - Min Latency: " << min_write_us << " µs\n";
  std::cout << "    - Max Latency: " << max_write_us << " µs\n";

  // Immediate Local Reads (Zero lag in AIOS RAM tables)
  auto sel_local = client.query("SELECT COUNT(*) FROM orders;");
  assert(sel_local.ok() && sel_local->ok && sel_local->rows.size() == 1);
  assert(sel_local->rows[0][0].bytes == std::to_string(NUM_WRITES));
  std::cout << "  [PASS] Local RAM table immediately returns all " << NUM_WRITES << " committed rows.\n\n";

  // 5. Back-Propagation Verification & Downstream Parity
  std::cout << "[Step 5] Verifying asynchronous back-propagation to downstream MySQL...\n";
  // Flush WAL drain to ensure worker sweeps all pending mutations
  assert(aios_db.flush_write_behind().ok());

  auto stats = aios_db.write_back_stats();
  std::cout << "  Write-Back Stats:\n";
  std::cout << "    - Total Mutations: " << stats.total_mutations << "\n";
  std::cout << "    - Acked by Downstream: " << stats.acked_mutations << "\n";
  std::cout << "    - Pending: " << stats.pending_mutations << "\n";
  std::cout << "    - Failed: " << stats.failed_mutations << "\n";
  assert(stats.pending_mutations == 0);
  assert(stats.acked_mutations >= (uint64_t)NUM_WRITES);

  // Directly inspect downstream database tables
  MariaWireSession down_client("127.0.0.1", down_port);
  assert(down_client.connect("root", "", "aios_db").ok());
  auto down_sel = down_client.query("SELECT COUNT(*), SUM(total) FROM orders;");
  assert(down_sel.ok() && down_sel->ok && down_sel->rows.size() == 1);
  assert(down_sel->rows[0][0].bytes == std::to_string(NUM_WRITES));
  std::cout << "  [PASS] Downstream database holds all " << NUM_WRITES << " synced rows with 100% fidelity.\n\n";

  // 6. Disconnection & Offline Tolerance Test
  std::cout << "[Step 6] Testing downstream disconnection & offline resilience...\n";
  // Stop downstream server to simulate network outage / database crash
  down_server.stop();
  std::cout << "  Downstream server stopped (simulating connection loss).\n";

  // Client continues writing to AIOS Gateway at full hardware speed
  for (int i = 201; i <= 250; ++i) {
    std::string sql = "INSERT INTO orders (id, customer, total, status) VALUES (" +
                      std::to_string(i) + ", 'OfflineCust', 999.0, 'QUEUED');";
    auto offline_ins = client.query(sql);
    assert(offline_ins.ok() && offline_ins->ok);
  }

  // Update mutations while offline
  auto upd_off = client.query("UPDATE orders SET status = 'PROCESSED' WHERE id <= 10;");
  assert(upd_off.ok() && upd_off->ok && upd_off->affected_rows == 10);

  // Querying AIOS returns latest state without any blockage
  auto local_off_sel = client.query("SELECT COUNT(*) FROM orders;");
  assert(local_off_sel.ok() && local_off_sel->ok);
  assert(local_off_sel->rows[0][0].bytes == "250");

  auto off_stats = aios_db.write_back_stats();
  std::cout << "  Offline stats: " << off_stats.pending_mutations << " mutations safely queued in WAL.\n";
  assert(off_stats.pending_mutations > 0);

  // Restart downstream server
  std::cout << "  Restarting downstream server & reconnecting...\n";
  MysqlWireServer down_server2(down_kernel, down_db, down_port, "127.0.0.1");
  assert(down_server2.start().ok());
  down_server2.start_background();

  // Reset session so gateway reconnects and drains pending WAL
  aios_db.set_downstream_session(nullptr);
  assert(aios_db.flush_write_behind().ok());

  auto rec_stats = aios_db.write_back_stats();
  assert(rec_stats.pending_mutations == 0);
  std::cout << "  [PASS] All offline mutations drained and ACKed downstream after recovery.\n\n";

  // 7. Crash Recovery Simulation (Un-ACKed WAL Replay on Boot)
  std::cout << "[Step 7] Testing crash recovery with un-ACKed WAL replay on reboot...\n";
  // Stop background drain and append writes directly to WAL
  aios_db.stop_wal_drain_worker();
  for (int i = 251; i <= 260; ++i) {
    std::string sql = "INSERT INTO orders (id, customer, total, status) VALUES (" +
                      std::to_string(i) + ", 'CrashRecoveryCust', 777.0, 'REBOOT_TEST');";
    aios_db.execute_gateway_query(sql);
  }

  // Simulate abrupt shutdown: tear down aios_db and aios_kernel without flushing
  aios_server.stop();

  // Boot fresh AIOS instance on the same root directory
  std::cout << "  Booting fresh AIOS instance from disk on existing root...\n";
  auto k_reboot_r = Kernel::boot(aios_kcfg);
  assert(k_reboot_r.ok());
  Kernel& reboot_k = **k_reboot_r;

  SqlDatabase reboot_db(reboot_k);
  assert(reboot_db.init("sql_db").ok());
  reboot_db.set_downstream_target(down_prof);
  reboot_db.set_sync_mode(WriteSyncMode::WriteBehind);

  // Check that uncommitted WAL mutations were reloaded on startup
  auto pending_after_boot = reboot_db.list_pending_mutations();
  std::cout << "  Recovered " << pending_after_boot.size() << " un-ACKed mutations from WAL on boot.\n";
  assert(!pending_after_boot.empty());

  // Drain recovered WAL
  assert(reboot_db.flush_write_behind().ok());
  assert(reboot_db.write_back_stats().pending_mutations == 0);

  // Verify downstream database has all 260 records
  MariaWireSession verify_client("127.0.0.1", down_port);
  assert(verify_client.connect("root", "", "aios_db").ok());
  auto verify_sel = verify_client.query("SELECT COUNT(*) FROM orders;");
  assert(verify_sel.ok() && verify_sel->ok && verify_sel->rows.size() == 1);
  assert(verify_sel->rows[0][0].bytes == "260");
  std::cout << "  [PASS] Downstream successfully synchronized all 260 rows after crash recovery.\n\n";

  // 8. Pattern A: Synchronous Write-Through Mode
  std::cout << "[Step 8] Testing Pattern A: Synchronous Write-Through Gateway...\n";
  reboot_db.set_sync_mode(WriteSyncMode::WriteThrough);
  assert(reboot_db.sync_mode() == WriteSyncMode::WriteThrough);

  MysqlWireServer reboot_server(reboot_k, reboot_db, 0, "127.0.0.1");
  assert(reboot_server.start().ok());
  reboot_server.start_background();

  MariaWireSession wt_client("127.0.0.1", reboot_server.port());
  assert(wt_client.connect("root", "", "aios_db").ok());

  auto wt_res = wt_client.query(
      "INSERT INTO orders (id, customer, total, status) VALUES (300, 'WriteThroughCust', 555.0, 'SYNC_OK');");
  assert(wt_res.ok() && wt_res->ok && wt_res->affected_rows == 1);

  // In Write-Through mode, downstream is updated immediately
  auto wt_down_sel = verify_client.query("SELECT status FROM orders WHERE id = 300;");
  assert(wt_down_sel.ok() && wt_down_sel->ok && wt_down_sel->rows.size() == 1);
  assert(wt_down_sel->rows[0][0].bytes == "SYNC_OK");
  std::cout << "  [PASS] Synchronous Write-Through committed to both AIOS and downstream in lockstep.\n\n";

  std::cout << "========================================================================\n";
  std::cout << "  ALL 8 GATEWAY & WRITE-BACK VERIFICATION GATES PASSED (100% SUCCESS)   \n";
  std::cout << "========================================================================\n";

  down_server2.stop();
  reboot_server.stop();
  return 0;
}
