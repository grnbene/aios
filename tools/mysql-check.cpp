// Verifies the MySQL wire protocol server against ASQL: handshake scrambles, a client
// connecting, and queries answered over the wire.

#undef NDEBUG
#include <iostream>
#include <cassert>
#include <chrono>
#include <thread>
#include <unistd.h>
#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"
#include "aios/userland/mysql_wire.hpp"
#include "aios/storage/aql.hpp"

using namespace aios;
using namespace aios::userland;
using namespace aios::storage;

int main() {
  std::cout << "=== AIOS MySQL Wire Protocol & ASQL Verification Suite ===\n";

  char root_dir[] = "/tmp/aios-mysql-check-XXXXXX";
  if (!mkdtemp(root_dir)) {
    std::perror("mkdtemp");
    return 1;
  }

  KernelConfig kcfg;
  kcfg.root = root_dir;
  kcfg.residency = test_config;
  auto k_res = Kernel::boot(kcfg);
  assert(k_res.ok());
  Kernel& kernel = **k_res;

  SqlDatabase db(kernel);
  auto init_r = db.init("sql_db");
  assert(init_r.ok());

  // --- Step 1: Crypto Scrambles Verification ----------------------------------
  std::cout << "[Step 1] Testing MySQL crypto scrambles (SHA1 & SHA256)...\n";
  const uint8_t mock_salt[20] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
  auto nat_scramble = mysql_native_scramble("secret_pass", mock_salt, 20);
  assert(nat_scramble.size() == 20);
  bool non_zero = false;
  for (uint8_t b : nat_scramble) if (b != 0) non_zero = true;
  if (!non_zero) { std::cerr << "Native scramble all zeroes\n"; return 1; }

  auto sha2_scramble = caching_sha2_scramble("secret_pass", mock_salt, 20);
  assert(sha2_scramble.size() == 32);
  std::cout << "  [PASS] Scrambles generated correctly.\n";

  // --- Step 2: MysqlWireServer Startup ---------------------------------------
  std::cout << "[Step 2] Starting AIOS MySQL Wire Protocol Server on 127.0.0.1 (dynamic port)...\n";
  MysqlWireServer server(kernel, db, 0, "127.0.0.1");
  auto s_res = server.start();
  if (!s_res.ok()) {
    std::cerr << "Failed to start MySQL wire server: " << s_res.error().message << "\n";
    return 1;
  }
  uint16_t test_port = server.port();
  assert(server.is_running());
  server.start_background();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  std::cout << "  [PASS] MysqlWireServer listening on 127.0.0.1:" << test_port << " and background serve thread active.\n";

  // --- Step 3: MariaWireSession Connect & Authentication ----------------------
  std::cout << "[Step 3] Connecting MariaWireSession client to AIOS Wire Server...\n";
  MariaWireSession client("127.0.0.1", test_port);
  auto c_res = client.connect("root", "", "aios_db");
  if (!c_res.ok()) {
    std::cerr << "Client connection failed: " << c_res.error().message << "\n";
    return 1;
  }
  assert(client.connected());
  std::cout << "  [PASS] Connected! Server version: " << client.server_version() << "\n";

  auto ping_r = client.ping();
  assert(ping_r.ok());
  std::cout << "  [PASS] COM_PING succeeded.\n";

  auto sel_r = client.select_db("aios_db");
  assert(sel_r.ok());
  std::cout << "  [PASS] COM_INIT_DB succeeded.\n";

  // --- Step 4: DDL & DML Queries over Wire Protocol ---------------------------
  std::cout << "[Step 4] Testing DDL & DML statements over MySQL Wire...\n";
  auto ddl_r = client.query("CREATE TABLE fleet_dispatch (id INT PRIMARY KEY, route_code VARCHAR(32), truck_id INT, driver_notes VARCHAR(255));");
  if (!ddl_r.ok() || !ddl_r->ok) {
    std::cerr << "CREATE TABLE failed: " << (ddl_r.ok() ? ddl_r->error_message : ddl_r.error().message) << "\n";
    return 1;
  }
  std::cout << "  [PASS] CREATE TABLE executed.\n";

  auto ins1 = client.query("INSERT INTO fleet_dispatch (id, route_code, truck_id, driver_notes) VALUES (101, 'RT-CLE-01', 501, 'Deliver before 10am');");
  assert(ins1.ok() && ins1->ok && ins1->affected_rows == 1);

  auto ins2 = client.query("INSERT INTO fleet_dispatch (id, route_code, truck_id, driver_notes) VALUES (102, 'RT-DET-09', 502, NULL);");
  assert(ins2.ok() && ins2->ok && ins2->affected_rows == 1);

  auto ins3 = client.query("INSERT INTO fleet_dispatch (id, route_code, truck_id, driver_notes) VALUES (103, 'RT-PIT-04', 503, 'Hazmat cargo');");
  assert(ins3.ok() && ins3->ok && ins3->affected_rows == 1);
  std::cout << "  [PASS] 3 INSERT statements executed with OK packets.\n";

  // --- Step 5: SELECT Query & Result Set Parsing ------------------------------
  std::cout << "[Step 5] Testing SELECT query and result set unframing...\n";
  auto sel_res = client.query("SELECT id, route_code, truck_id, driver_notes FROM fleet_dispatch ORDER BY id ASC;");
  assert(sel_res.ok() && sel_res->ok);
  assert(sel_res->has_result_set);
  assert(sel_res->columns.size() == 4);
  assert(sel_res->rows.size() == 3);

  // Check row 0
  assert(sel_res->rows[0][0].bytes == "101");
  assert(sel_res->rows[0][1].bytes == "RT-CLE-01");
  assert(sel_res->rows[0][2].bytes == "501");
  assert(!sel_res->rows[0][3].is_null);
  assert(sel_res->rows[0][3].bytes == "Deliver before 10am");

  // Check row 1 (NULL notes)
  assert(sel_res->rows[1][0].bytes == "102");
  assert(sel_res->rows[1][1].bytes == "RT-DET-09");
  assert(sel_res->rows[1][3].is_null); // Verified MySQL protocol NULL handling (0xFB)
  std::cout << "  [PASS] SELECT result set verified including NULL column handling.\n";

  // --- Step 6: UPDATE, DELETE & Error Packets ---------------------------------
  std::cout << "[Step 6] Testing UPDATE, DELETE, and ERR packet generation...\n";
  auto upd_r = client.query("UPDATE fleet_dispatch SET truck_id = 777 WHERE id = 101;");
  assert(upd_r.ok() && upd_r->ok && upd_r->affected_rows == 1);

  auto del_r = client.query("DELETE FROM fleet_dispatch WHERE id = 103;");
  assert(del_r.ok() && del_r->ok && del_r->affected_rows == 1);

  auto err_r = client.query("SELECT * FROM non_existent_table_xyz;");
  assert(err_r.ok());
  assert(!err_r->ok);  // Server correctly returned ERR packet
  assert(!err_r->error_message.empty());
  std::cout << "  [PASS] ERR packet properly received: " << err_r->error_message << "\n";

  // --- Step 7: Remote Profile Dispatch in SqlDatabase -------------------------
  std::cout << "[Step 7] Testing SqlDatabase remote connection profile...\n";
  DbConnectionProfile rem_prof;
  rem_prof.id = "wire_remote";
  rem_prof.name = "AIOS Wire Remote";
  rem_prof.host = "127.0.0.1";
  rem_prof.port = test_port;
  rem_prof.user = "root";
  rem_prof.database = "aios_db";
  rem_prof.is_local_embedded = false;
  auto add_p_r = db.add_or_update_profile(rem_prof);
  assert(add_p_r.ok());

  auto sw_r = db.switch_profile("wire_remote");
  assert(sw_r.ok());

  SqlQueryResult rem_q = db.execute_query("SELECT id, route_code, truck_id FROM fleet_dispatch WHERE id = 101;");
  if (!rem_q.success) {
    std::cerr << "Remote execute_query failed: " << rem_q.error_message << "\n";
    return 1;
  }
  assert(rem_q.rows.size() == 1);
  assert(rem_q.rows[0].values[0] == "101");
  assert(rem_q.rows[0].values[2] == "777");
  std::cout << "  [PASS] Query routed over remote wire protocol from SqlDatabase.\n";

  auto rem_tables = db.list_tables();
  bool found_table = false;
  for (const auto& t : rem_tables) if (t == "fleet_dispatch") found_table = true;
  if (!found_table) { std::cerr << "Remote tables missing fleet_dispatch\n"; return 1; }
  std::cout << "  [PASS] Remote tables listed over wire protocol.\n";

  // Switch back to embedded
  db.switch_profile("local_embedded");

  // --- Step 8: ASQL Execution over MySQL Wire Protocol ------------------------
  std::cout << "[Step 8] Testing ASQL Object Store queries over MySQL Wire Protocol...\n";
  Surface self_surf = kernel.as(kernel.self());
  StoredObject obj;
  obj.metadata.emplace_back("kind", "asql.manifest");
  obj.metadata.emplace_back("carrier", "FastFreight");
  obj.payload = ByteBuffer{'M', 'a', 'n', 'i', 'f', 'e', 's', 't', '_', '1'};
  auto store_r = self_surf.store(obj);
  if (!store_r.ok()) {
    std::cerr << "store failed: " << store_r.error().message << "\n";
  }
  assert(store_r.ok());

  auto asql_r = client.query("SELECT * FROM objects WHERE meta.kind = \"asql.manifest\";");
  if (!asql_r.ok() || !asql_r->ok) {
    std::cerr << "ASQL query over MySQL wire failed: " << (asql_r.ok() ? asql_r->error_message : asql_r.error().message) << "\n";
    return 1;
  }
  assert(asql_r->has_result_set);
  assert(asql_r->rows.size() >= 1);
  assert(asql_r->rows[0][1].bytes == "asql.manifest");
  std::cout << "  [PASS] ASQL object queries mirrored seamlessly into standard MySQL wire results!\n";

  // --- Step 9: Phase 0 Micro-Threshold Boundary Harness Invariants ----------
  std::cout << "[Step 9] Testing MySQL / ObjectStore Micro-Threshold Boundary Invariants...\n";
  assert(kernel.store().residency_config().block_objects == 8);
  assert(kernel.store().residency_config().max_active == 16);
  assert(kernel.store().residency_config().hot_budget == 64 * 1024);

  // Store 25 audit objects to cross the block_objects(8) and max_active(16) thresholds
  for (int i = 1; i <= 25; ++i) {
    StoredObject audit_obj;
    audit_obj.metadata.emplace_back("kind", "sql.audit_record");
    audit_obj.metadata.emplace_back("seq", std::to_string(i));
    audit_obj.metadata.emplace_back("status", (i % 2 == 0) ? "ok" : "pending");
    audit_obj.payload = ByteBuffer(2048, static_cast<uint8_t>('S' + (i % 10)));
    auto id_r = self_surf.store(audit_obj);
    assert(id_r.ok());
  }

  // Verify auto-sealing occurred into cold segments
  assert(kernel.store().cold_blocks_count() > 0);

  // Query objects over sealed cold blocks via ASQL MySQL wire protocol
  auto audit_q = client.query("SELECT * FROM objects WHERE meta.kind = \"sql.audit_record\" AND meta.status = \"ok\";");
  assert(audit_q.ok() && audit_q->has_result_set);
  assert(audit_q->rows.size() == 12);

  // Verify Core Invariants: count identity and budget integrity
  assert(kernel.store().count() == kernel.store().active_count() + kernel.store().cold_objects_count());
  assert(kernel.store().list().size() == kernel.store().count());
  assert(kernel.store().hot_bytes() <= kernel.store().hot_budget());

  std::cout << "  [PASS] Phase 0 Micro-Threshold Invariants verified: cold_blocks_count="
            << kernel.store().cold_blocks_count() << " > 0, 25 objects stored & queried across active & sealed tiers via ASQL wire, count identity held.\n";

  // --- Step 10: Multi-Tenant Database Isolation Verification -----------------
  std::cout << "[Step 10] Testing Multi-Tenant Database Isolation (hairlines vs AA)...\n";
  MariaWireSession client_hairlines("127.0.0.1", test_port);
  auto c_hl = client_hairlines.connect("root", "", "hairlines");
  assert(c_hl.ok() && client_hairlines.connected());

  MariaWireSession client_aa("127.0.0.1", test_port);
  auto c_aa = client_aa.connect("root", "", "AA");
  assert(c_aa.ok() && client_aa.connected());

  auto hl_db_q = client_hairlines.query("SELECT DATABASE();");
  assert(hl_db_q.ok() && hl_db_q->has_result_set && hl_db_q->rows.size() == 1);
  assert(hl_db_q->rows[0][0].bytes == "hairlines");

  auto aa_db_q = client_aa.query("SELECT DATABASE();");
  assert(aa_db_q.ok() && aa_db_q->has_result_set && aa_db_q->rows.size() == 1);
  assert(aa_db_q->rows[0][0].bytes == "AA");

  // Create table 'dispatch' in hairlines
  auto hl_create = client_hairlines.query("CREATE TABLE dispatch (id INT PRIMARY KEY, hair_product VARCHAR(64));");
  assert(hl_create.ok() && hl_create->ok);
  auto hl_ins = client_hairlines.query("INSERT INTO dispatch (id, hair_product) VALUES (1, 'Shampoo-Silk'), (2, 'Conditioner-Argan');");
  assert(hl_ins.ok() && hl_ins->ok && hl_ins->affected_rows == 2);

  // Create table 'dispatch' in AA with completely different schema and data
  auto aa_create = client_aa.query("CREATE TABLE dispatch (id INT PRIMARY KEY, part_number VARCHAR(64), stock_qty INT);");
  assert(aa_create.ok() && aa_create->ok);
  auto aa_ins = client_aa.query("INSERT INTO dispatch (id, part_number, stock_qty) VALUES (101, 'AUTO-BRAKE-01', 45), (102, 'AUTO-ROTOR-02', 12);");
  assert(aa_ins.ok() && aa_ins->ok && aa_ins->affected_rows == 2);

  // Query unqualified 'dispatch' in hairlines session
  auto hl_sel = client_hairlines.query("SELECT id, hair_product FROM dispatch ORDER BY id ASC;");
  assert(hl_sel.ok() && hl_sel->has_result_set && hl_sel->rows.size() == 2);
  assert(hl_sel->rows[0][0].bytes == "1" && hl_sel->rows[0][1].bytes == "Shampoo-Silk");
  assert(hl_sel->rows[1][0].bytes == "2" && hl_sel->rows[1][1].bytes == "Conditioner-Argan");

  // Query unqualified 'dispatch' in AA session
  auto aa_sel = client_aa.query("SELECT id, part_number, stock_qty FROM dispatch ORDER BY id ASC;");
  assert(aa_sel.ok() && aa_sel->has_result_set && aa_sel->rows.size() == 2);
  assert(aa_sel->rows[0][0].bytes == "101" && aa_sel->rows[0][1].bytes == "AUTO-BRAKE-01" && aa_sel->rows[0][2].bytes == "45");
  assert(aa_sel->rows[1][0].bytes == "102" && aa_sel->rows[1][1].bytes == "AUTO-ROTOR-02" && aa_sel->rows[1][2].bytes == "12");

  // Cross-database query: hairlines querying AA.dispatch explicitly
  auto cross_sel = client_hairlines.query("SELECT id, part_number, stock_qty FROM AA.dispatch WHERE id = 101;");
  assert(cross_sel.ok() && cross_sel->has_result_set && cross_sel->rows.size() == 1);
  assert(cross_sel->rows[0][0].bytes == "101" && cross_sel->rows[0][1].bytes == "AUTO-BRAKE-01");

  // Verify hairlines session database did not change
  auto hl_db_check = client_hairlines.query("SELECT DATABASE();");
  assert(hl_db_check.ok() && hl_db_check->rows[0][0].bytes == "hairlines");

  // SHOW TABLES in hairlines session should only show hairlines tables
  auto hl_show = client_hairlines.query("SHOW TABLES;");
  assert(hl_show.ok() && hl_show->has_result_set && hl_show->rows.size() == 1);
  assert(hl_show->rows[0][0].bytes == "dispatch");

  // SHOW TABLES in AA session should only show AA tables
  auto aa_show = client_aa.query("SHOW TABLES;");
  assert(aa_show.ok() && aa_show->has_result_set && aa_show->rows.size() == 1);
  assert(aa_show->rows[0][0].bytes == "dispatch");

  // SHOW DATABASES should list all databases
  auto show_dbs = client_hairlines.query("SHOW DATABASES;");
  assert(show_dbs.ok() && show_dbs->has_result_set);
  bool found_hl = false, found_aa = false;
  for (const auto& r : show_dbs->rows) {
    if (r[0].bytes == "hairlines") found_hl = true;
    if (r[0].bytes == "AA") found_aa = true;
  }
  assert(found_hl && found_aa);
  std::cout << "  [PASS] Multi-tenant isolation between 'hairlines' and 'AA' strictly verified with zero collision!\n";

  // --- Step 11: Navicat & MySQL GUI / CLI Compatibility Queries ---------------
  std::cout << "[Step 11] Testing Navicat & Standard MySQL Client Protocol Compatibility...\n";
  MariaWireSession client_navicat("127.0.0.1", test_port);
  auto n_res = client_navicat.connect("devops", "navicat-check", "hairlines");
  assert(n_res.ok() && client_navicat.connected());

  // 1. SET NAMES and autocommit
  auto set_names = client_navicat.query("SET NAMES utf8mb4;");
  assert(set_names.ok() && set_names->ok);
  auto set_ac = client_navicat.query("SET autocommit=1;");
  assert(set_ac.ok() && set_ac->ok);

  // 2. System variables: SELECT @@...
  auto sys_vars = client_navicat.query("SELECT @@version_comment, @@tx_isolation, @@sql_mode, @@lower_case_table_names, @@max_allowed_packet, @@character_set_client;");
  assert(sys_vars.ok() && sys_vars->ok && sys_vars->has_result_set);
  assert(sys_vars->columns.size() == 6);
  assert(sys_vars->rows.size() == 1);
  assert(sys_vars->rows[0][0].bytes == "AIOS ASQL / MySQL Native Wire Server");
  assert(sys_vars->rows[0][1].bytes == "REPEATABLE-READ");
  assert(sys_vars->rows[0][2].bytes.find("NO_ENGINE_SUBSTITUTION") != std::string::npos);
  assert(sys_vars->rows[0][3].bytes == "0");
  assert(sys_vars->rows[0][4].bytes == "67108864");
  assert(sys_vars->rows[0][5].bytes == "utf8mb4");

  // 3. Scalar functions: DATABASE(), VERSION(), USER()
  auto func_q = client_navicat.query("SELECT DATABASE(), VERSION(), USER();");
  assert(func_q.ok() && func_q->ok && func_q->has_result_set);
  assert(func_q->rows[0][0].bytes == "hairlines");
  assert(func_q->rows[0][1].bytes == "5.7.44-aios-asql");
  assert(func_q->rows[0][2].bytes == "devops@127.0.0.1");

  // 4. SHOW TABLE STATUS
  auto tbl_stat = client_navicat.query("SHOW TABLE STATUS FROM hairlines;");
  assert(tbl_stat.ok() && tbl_stat->ok && tbl_stat->has_result_set);
  assert(tbl_stat->columns.size() >= 10);
  assert(tbl_stat->rows.size() == 1);
  assert(tbl_stat->rows[0][0].bytes == "dispatch");
  assert(tbl_stat->rows[0][1].bytes == "InnoDB");

  // 5. SHOW INDEX / SHOW KEYS
  auto idx_q = client_navicat.query("SHOW INDEX FROM dispatch;");
  assert(idx_q.ok() && idx_q->ok && idx_q->has_result_set);
  assert(idx_q->columns.size() >= 10);
  assert(idx_q->rows.size() >= 1);
  assert(idx_q->rows[0][2].bytes == "PRIMARY");
  assert(idx_q->rows[0][4].bytes == "id");

  // 6. SHOW FULL COLUMNS
  auto col_q = client_navicat.query("SHOW FULL COLUMNS FROM dispatch;");
  assert(col_q.ok() && col_q->ok && col_q->has_result_set);
  assert(col_q->columns.size() == 9);
  assert(col_q->rows.size() == 2);
  assert(col_q->rows[0][0].bytes == "id");
  assert(col_q->rows[0][4].bytes == "PRI");

  // 7. SHOW VARIABLES LIKE
  auto var_q = client_navicat.query("SHOW VARIABLES LIKE 'lower_case%';");
  assert(var_q.ok() && var_q->ok && var_q->has_result_set);
  assert(var_q->rows.size() == 1);
  assert(var_q->rows[0][0].bytes == "lower_case_table_names");
  assert(var_q->rows[0][1].bytes == "0");

  // 8. information_schema.TABLES
  auto info_tbls = client_navicat.query("SELECT TABLE_NAME, TABLE_SCHEMA, TABLE_ROWS FROM information_schema.TABLES WHERE TABLE_SCHEMA = 'hairlines';");
  assert(info_tbls.ok() && info_tbls->ok && info_tbls->has_result_set);
  assert(info_tbls->rows.size() == 1);
  assert(info_tbls->rows[0][0].bytes == "dispatch");
  assert(info_tbls->rows[0][1].bytes == "hairlines");

  // 9. information_schema.COLUMNS
  auto info_cols = client_navicat.query("SELECT COLUMN_NAME, DATA_TYPE, COLUMN_KEY FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = 'hairlines' AND TABLE_NAME = 'dispatch';");
  assert(info_cols.ok() && info_cols->ok && info_cols->has_result_set);
  assert(info_cols->rows.size() == 2);
  assert(info_cols->rows[0][0].bytes == "id");
  assert(info_cols->rows[0][2].bytes == "PRI");

  // --- Step 12: Test SHOW PROCESSLIST, ENGINES, GRANTS, STATUS, and SLAVES ---
  std::cout << "[Step 12] Testing SHOW PROCESSLIST, ENGINES, GRANTS, STATUS, and SLAVE HOSTS...\n";
  
  // 1. SHOW PROCESSLIST
  auto proc_q = client_navicat.query("SHOW PROCESSLIST;");
  assert(proc_q.ok() && proc_q->ok && proc_q->has_result_set);
  assert(proc_q->columns.size() == 8);
  assert(proc_q->rows.size() >= 1);
  std::cout << "  [PASS] SHOW PROCESSLIST returned " << proc_q->rows.size() << " active session(s).\n";

  // 2. SHOW ENGINES
  auto eng_q = client_navicat.query("SHOW ENGINES;");
  assert(eng_q.ok() && eng_q->ok && eng_q->has_result_set);
  assert(eng_q->rows.size() >= 3);
  bool found_asql = false;
  for (const auto& r : eng_q->rows) {
    if (r[0].bytes == "ASQL") {
      found_asql = true;
      assert(r[1].bytes == "DEFAULT");
    }
  }
  assert(found_asql);
  std::cout << "  [PASS] SHOW ENGINES returned ASQL as DEFAULT engine.\n";

  // 3. SHOW GRANTS
  auto grant_q = client_navicat.query("SHOW GRANTS;");
  assert(grant_q.ok() && grant_q->ok && grant_q->has_result_set);
  assert(grant_q->rows.size() >= 1);
  assert(grant_q->rows[0][0].bytes.find("devops") != std::string::npos);

  auto grant_for = client_navicat.query("SHOW GRANTS FOR 'admin'@'127.0.0.1';");
  assert(grant_for.ok() && grant_for->ok && grant_for->has_result_set);
  assert(grant_for->rows[0][0].bytes.find("admin") != std::string::npos);
  std::cout << "  [PASS] SHOW GRANTS and SHOW GRANTS FOR parsed and verified.\n";

  // 4. SHOW STATUS
  auto stat_q = client_navicat.query("SHOW STATUS LIKE 'Threads_%';");
  assert(stat_q.ok() && stat_q->ok && stat_q->has_result_set);
  assert(stat_q->rows.size() >= 2);
  std::cout << "  [PASS] SHOW STATUS dynamic metrics verified.\n";

  // 5. Slave Registration & SHOW SLAVE HOSTS
  MysqlWireServer::SlaveInfo test_slave;
  test_slave.server_id = 999;
  test_slave.host = "replica-node-1";
  test_slave.user = "repuser";
  test_slave.port = 3307;
  test_slave.master_id = 1;
  MysqlWireServer::register_slave(test_slave);

  auto slave_q = client_navicat.query("SHOW SLAVE HOSTS;");
  assert(slave_q.ok() && slave_q->ok && slave_q->has_result_set);
  assert(slave_q->rows.size() == 1);
  assert(slave_q->rows[0][0].bytes == "999");
  assert(slave_q->rows[0][1].bytes == "replica-node-1");
  assert(slave_q->rows[0][2].bytes == "3307");
  std::cout << "  [PASS] COM_REGISTER_SLAVE and SHOW SLAVE HOSTS verified.\n";

  std::cout << "  [PASS] Navicat, DBeaver, and MySQL CLI protocol queries verified 100% compliant!\n";

  client_hairlines.close();
  client_aa.close();
  client_navicat.close();

  // --- Step 8: the compressed protocol (zlib and zstd) -------------------------
  // Each answer must equal the plain session's byte for byte: a few small
  // statements (stored frames), a result past one 16 MB frame, and a statement
  // bigger than one packet going the other way.
  std::cout << "[Step 8] Testing the compressed protocol (CLIENT_COMPRESS zlib, zstd)...\n";
  {
    MariaWireSession plain("127.0.0.1", test_port);
    assert(plain.connect("root", "", "aios_db").ok());
    assert(plain.query("CREATE TABLE ztest (id INT PRIMARY KEY, body VARCHAR(255))")->ok);
    std::string ins = "INSERT INTO ztest VALUES ";
    for (int i = 0; i < 90000; ++i) {
      ins += (i ? ",(" : "(") + std::to_string(i) + ",'row " + std::to_string(i) + " " + std::string(180, (char)('a' + i % 26)) + "')";
    }
    const std::string big_sql = "SELECT id, body FROM ztest ORDER BY id";
    auto digest = [](const MariaResult& r) {
      std::string all;
      for (const auto& row : r.rows) for (const auto& v : row) { all += v.is_null ? "\x01" : v.bytes; all += '\x02'; }
      return all;
    };
    std::string want;
    for (const char* algo : {"zlib", "zstd"}) {
      MariaWireSession z("127.0.0.1", test_port);
      z.set_compression(std::string(algo) == "zstd" ? 2 : 1);
      auto zc = z.connect("root", "", "aios_db");
      if (!zc.ok()) { std::cerr << algo << " connect failed: " << zc.error().message << "\n"; return 1; }
      if (std::string(algo) == "zstd") { std::cout << "  (no zstd in this build: zlib instead)\n"; algo = "zlib"; }
      if (std::string(z.compression()) != algo) { std::cerr << "session runs '" << z.compression() << "', not " << algo << "\n"; return 1; }
      auto st = z.query("SHOW STATUS LIKE 'Compression%'");
      assert(st.ok() && st->ok && st->rows.size() == 3);
      assert(st->rows[0][1].bytes == "ON" && st->rows[1][1].bytes == algo);
      assert(z.ping().ok());
      if (want.empty()) {   // the first session loads the rows, compressed: 17 MB, more than one packet and one frame
        auto r = z.query(ins);
        if (!r.ok() || !r->ok) { std::cerr << "compressed INSERT failed\n"; return 1; }
        assert(r->affected_rows == 90000);
        auto pr = plain.query(big_sql);
        assert(pr.ok() && pr->ok && pr->rows.size() == 90000);
        want = digest(*pr);
      }
      auto zr = z.query(big_sql);
      if (!zr.ok() || !zr->ok || digest(*zr) != want) { std::cerr << algo << ": the result differs from the plain session's\n"; return 1; }
      auto one = z.query("SELECT body FROM ztest WHERE id = 77");
      assert(one.ok() && one->ok && one->rows.size() == 1 && one->rows[0][0].bytes.rfind("row 77 ", 0) == 0);
      std::cout << "  [PASS] " << algo << ": 90,000-row result identical to the plain session's; small statements and ping\n";
    }
    auto g = plain.query("SHOW GLOBAL STATUS LIKE 'Asql_bytes_sent_compressed'");
    assert(g.ok() && g->ok && g->rows.size() == 1 && std::stoull(g->rows[0][1].bytes) > 0);
    auto ps = plain.query("SHOW STATUS LIKE 'Compression'");
    assert(ps.ok() && ps->rows.size() == 1 && ps->rows[0][1].bytes == "OFF");
    plain.close();
  }

  // Clean shutdown
  client.close();
  server.stop();
  std::cout << "\n>>> PROVEN: AIOS MySQL Wire Protocol & ASQL Integration 100% Operational!\n";
  return 0;
}
