// Verifies the ASQL kernel enhancements against a booted kernel on a throwaway root.

#undef NDEBUG
#include <iostream>
#include <cassert>
#include <unistd.h>
#include <thread>
#include <chrono>
#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"
#include "aios/userland/task_ledger.hpp"
#include "aios/storage/aql.hpp"

using namespace aios;
using namespace aios::userland;

int main() {
  std::cout << "=== AIOS ASQL Kernel Enhancements Verification Suite ===\n";

  char root_dir[] = "/tmp/aios-asql-enhancements-XXXXXX";
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

  std::cout << "[Step 1] Multi-Row Bulk Insert via ASQL put_many...\n";
  {
    std::string multi_insert = R"(
      INSERT INTO OBJECTS (KEY, KIND, PAYLOAD) VALUES
        ('sensor:1', 'telemetry', 'val1'),
        ('sensor:2', 'telemetry', 'val2'),
        ('sensor:3', 'telemetry', 'val3'),
        ('sensor:4', 'telemetry', 'val4');
    )";
    auto res = db.execute_gateway_query(multi_insert);
    if (!res.success) {
      std::cerr << "Multi-row insert failed: " << res.error_message << "\n";
      return 1;
    }
    assert(res.rows_affected == 4);
    std::cout << "  -> Multi-row insert inserted 4 objects successfully: " << res.formatted_table << "\n";
  }

  std::cout << "[Step 2] Hex / Binary Literal Syntax (0x... and X'...')...\n";
  {
    std::string hex_insert = "INSERT INTO OBJECTS (KEY, KIND, PAYLOAD) VALUES ('hex:1', 'binary', 0x48656c6c6f20576f726c6421), ('hex:2', 'binary', X'54657374');";
    auto res = db.execute_gateway_query(hex_insert);
    if (!res.success) {
      std::cerr << "Hex insert failed: " << res.error_message << "\n";
      return 1;
    }
    assert(res.rows_affected == 2);

    auto query_res = db.execute_gateway_query("SELECT * FROM objects WHERE meta.key = \"hex:1\"");
    assert(query_res.success);
    assert(query_res.rows.size() == 1);
    // Payload column is index 2
    assert(query_res.rows[0].values[2] == "Hello World!");

    auto query_res2 = db.execute_gateway_query("SELECT * FROM objects WHERE meta.key = \"hex:2\"");
    assert(query_res2.success);
    assert(query_res2.rows.size() == 1);
    assert(query_res2.rows[0].values[2] == "Test");
    std::cout << "  -> Hex literals 0x... and X'...' decoded directly to binary payloads: " << query_res.rows[0].values[2] << " & " << query_res2.rows[0].values[2] << "\n";
  }

  std::cout << "[Step 3] Ephemeral Objects & TTL Lifecycle Expiration...\n";
  {
    std::string ttl_insert = "INSERT INTO OBJECTS (KEY, KIND, PAYLOAD, TTL) VALUES ('ephemeral:1', 'cache', 'temp_value', 1);";
    auto res = db.execute_gateway_query(ttl_insert);
    assert(res.success);

    // Immediate lookup: should be found
    auto query_res1 = db.execute_gateway_query("SELECT * FROM objects WHERE meta.key = \"ephemeral:1\"");
    assert(query_res1.success);
    assert(query_res1.rows.size() == 1);
    std::cout << "  -> Immediate lookup found ephemeral object: " << query_res1.rows[0].values[0] << "\n";

    // Wait for TTL (1 second) to expire
    std::cout << "  -> Sleeping 1.2s for TTL expiration...\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    // Post-expiry lookup: should return 0 rows
    auto query_res2 = db.execute_gateway_query("SELECT * FROM objects WHERE meta.key = \"ephemeral:1\"");
    assert(query_res2.success);
    assert(query_res2.rows.size() == 0);
    std::cout << "  -> Post-expiry lookup cleanly returned 0 rows (expired).\n";
  }

  std::cout << "[Step 4] Compaction / VACUUM (Tombstones, Superseded Revisions, and Expired Pruning)...\n";
  {
    // Update sensor:1 to a new revision
    auto upd = db.execute_gateway_query("INSERT INTO OBJECTS (KEY, KIND, PAYLOAD) VALUES ('sensor:1', 'telemetry', 'val1_updated');");
    assert(upd.success);

    // Delete sensor:2 via tombstone
    auto del = db.execute_gateway_query("INSERT INTO OBJECTS (KEY, KIND, PAYLOAD, STATUS) VALUES ('sensor:2', 'tombstone', '', '__aios_tombstone__');");
    assert(del.success);

    // Run VACUUM
    auto vac = db.execute_gateway_query("VACUUM OBJECTS;");
    if (!vac.success) {
      std::cerr << "VACUUM failed: " << vac.error_message << "\n";
      return 1;
    }
    std::cout << "  -> VACUUM result table:\n" << vac.formatted_table << "\n";

    // Verify sensor:1 holds updated value
    auto q1 = db.execute_gateway_query("SELECT * FROM objects WHERE meta.key = \"sensor:1\"");
    assert(q1.success);
    assert(q1.rows.size() == 1);
    assert(q1.rows[0].values[2] == "val1_updated");

    // Verify sensor:2 is deleted
    auto q2 = db.execute_gateway_query("SELECT * FROM objects WHERE meta.key = \"sensor:2\"");
    assert(q2.success);
    assert(q2.rows.size() == 0);

    // Verify expired item was pruned
    auto q_eph = db.execute_gateway_query("SELECT * FROM objects WHERE meta.key = \"ephemeral:1\"");
    assert(q_eph.success);
    assert(q_eph.rows.size() == 0);
    std::cout << "  -> Post-compaction store parity confirmed!\n";
  }

  std::cout << "[Step 5] Composite Predicate Pushdown & Index Intersection...\n";
  {
    std::string bulk_turns = R"(
      INSERT INTO OBJECTS (KEY, KIND, TURN_STEP, PAYLOAD) VALUES
        ('turn:1', 'agent_turn', '1', 'turn 1 payload'),
        ('turn:2', 'agent_turn', '2', 'turn 2 payload'),
        ('turn:3', 'agent_turn', '3', 'turn 3 payload'),
        ('turn:4', 'agent_turn', '4', 'turn 4 payload'),
        ('turn:5', 'other_kind', '5', 'other 5 payload'),
        ('turn:6', 'agent_turn', '6', 'turn 6 payload');
    )";
    auto res = db.execute_gateway_query(bulk_turns);
    assert(res.success);

    // Query intersecting equality index (kind = 'agent_turn') and range index (turn_step >= 3)
    auto query_res = db.execute_gateway_query("SELECT * FROM objects WHERE meta.kind = \"agent_turn\" AND meta.turn_step >= 3");
    if (!query_res.success) {
      std::cerr << "Composite query failed: " << query_res.error_message << "\n";
      return 1;
    }
    // Matching should be turn:3, turn:4, turn:6 (turn:5 has other_kind, turn:1,2 have step < 3)
    assert(query_res.rows.size() == 3);
    std::cout << "  -> Composite predicate intersection query returned " << query_res.rows.size() << " objects as expected:\n" << query_res.formatted_table << "\n";
  }

  std::cout << "[Step 6] Stored Procedures, Functions, Triggers...\n";
  {
    auto cp = db.execute_gateway_query("CREATE PROCEDURE my_proc(IN x VARCHAR(255), OUT y VARCHAR(255)) BEGIN SET y = x; END");
    assert(cp.success);
    auto cl = db.execute_gateway_query("CALL my_proc('hello_proc', @my_out);");
    assert(cl.success);
    auto sel = db.execute_gateway_query("SELECT @my_out;");
    assert(sel.success && sel.rows.size() == 1 && sel.rows[0].values[0] == "hello_proc");
    std::cout << "  -> Procedure created and CALL returned correct OUT variable.\n";
  }

  std::cout << "\n=== All ASQL Kernel Enhancements Verified Successfully ===\n";
  return 0;
}
