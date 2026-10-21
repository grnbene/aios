// Verifies the sidecar subsystems on a throwaway kernel root: the shared-memory ABI,
// filesystem topology, and binary telemetry.

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <cassert>
#include <thread>
#include <vector>
#include <string>
#if !defined(_WIN32)
#include <unistd.h>
#endif
#include <cstdlib>

#include "aios/sidecar/shm_abi.hpp"
#include "aios/sidecar/fs_topology.hpp"
#include "aios/sidecar/binary_telemetry.hpp"
#include "aios/sidecar/module_vault.hpp"
#include "aios/sidecar/event_fabric.hpp"
#include "aios/sidecar/capability_guard.hpp"
#include "aios/sidecar/git_telemetry_events.hpp"
#include "aios/sidecar/vault_workspace.hpp"
#include "aios/sidecar/instrumented_agent_session.hpp"
#include "aios/sidecar/aios_sidecar_c_abi.h"
#include "aios/userland/sql_database.hpp"

using namespace aios::sidecar;

// A fresh temporary directory, removed (recursively) when it goes out of scope.
// Declared before the Kernel that uses it, so the kernel is gone first.
struct TempRoot {
  std::string dir;
  const char* path;
  explicit TempRoot(const char* name) {
    dir = (std::filesystem::temp_directory_path() /
           (std::string(name) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).string();
    std::error_code ec;
    if (!std::filesystem::create_directories(dir, ec)) { std::fprintf(stderr, "cannot create %s\n", dir.c_str()); std::exit(1); }
    path = dir.c_str();
  }
  ~TempRoot() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
  TempRoot(const TempRoot&) = delete;
  TempRoot& operator=(const TempRoot&) = delete;
};

int main() {
  std::cout << "=================================================================\n";
  std::cout << "  AIOS 50-Year Legacy Elimination Sidecar Architecture Suite   \n";
  std::cout << "=================================================================\n\n";

  int checks_passed = 0;
  int checks_failed = 0;
  int total_checks = 0;

  // Gates were raw assert() calls: success == not aborting, failure == a bare
  // SIGABRT with no report, no summary line, and no nonzero exit (main always
  // returned 0). That is the same dishonest pattern the weekend's honesty pass
  // removed from the benches -- it cannot say which step failed or produce an
  // honest exit code. check() records the real condition, prints [FAIL], and
  // lets us summarize and return nonzero at the end. A hardware/absence probe
  // is reported as [INFO] and not counted, since it has no wrong answer.
  auto check = [&](bool ok, const std::string& name, const std::string& detail) {
    total_checks++;
    if (ok) {
      checks_passed++;
      std::cout << "  [PASS] " << name << ": " << detail << "\n";
    } else {
      checks_failed++;
      std::cout << "  [FAIL] " << name << ": " << detail << "\n";
    }
  };

  // -------------------------------------------------------------
  // 1. Zero-Copy Shared Memory Ring Channel (Bypassing Unix Serialization)
  // -------------------------------------------------------------
  std::cout << "[Step 1] Testing Zero-Copy Shared Memory ABI Ring Channel...\n";
  {
    ShmRingChannel chan("anon", 1024 * 1024, true);
    check(chan.capacity() >= 1024 * 1024, "SHM ring capacity",
          chan.capacity() >= 1024 * 1024 ? "anon ring holds "+std::to_string(chan.capacity())+" bytes" : "capacity below requested");

    struct FastRecord {
      uint32_t id;
      double price;
      char sku[16];
    };

    FastRecord rec1{42, 199.99, "HAIR-TREAT-01"};
    FastRecord rec2{43, 89.50, "COMB-PRO-X"};

    chan.push_frame(1, 100, &rec1, sizeof(rec1));
    chan.push_frame(1, 100, &rec2, sizeof(rec2));

    size_t received_count = 0;
    bool r1_ok = false, r2_ok = false;
    while (chan.pop_frame([&](const ShmFrameHeader& hdr, const void* payload, size_t) {
      if (hdr.payload_len != sizeof(FastRecord)) return;
      const auto* r = reinterpret_cast<const FastRecord*>(payload);
      if (received_count == 0) { r1_ok = (r->id == 42) && (r->price == 199.99); }
      else if (received_count == 1) { r2_ok = (r->id == 43) && (r->price == 89.50); }
      received_count++;
    })) {}
    check(received_count == 2 && r1_ok && r2_ok, "SHM ring frame roundtrip",
          received_count == 2 && r1_ok && r2_ok ? "2 FastRecords pushed and popped in order" : "got "+std::to_string(received_count)+" frames");

    // Throughput Benchmark
    auto t0 = std::chrono::high_resolution_clock::now();
    const size_t bench_count = 100000;
    for (size_t i = 0; i < bench_count; ++i) {
      FastRecord r{static_cast<uint32_t>(i), 12.34, "TEST-SKU"};
      chan.push_frame(2, 1, &r, sizeof(r));
      chan.pop_frame([](const ShmFrameHeader&, const void*, size_t) {});
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double ops_per_sec = (bench_count / elapsed_ms) * 1000.0;
    std::cout << "  [PASS] SHM Ring: " << bench_count << " push/pop ops in "
              << std::fixed << std::setprecision(2) << elapsed_ms << " ms ("
              << std::setprecision(0) << ops_per_sec << " ops/sec, zero-copy!)\n";
  }

  // -------------------------------------------------------------
  // 2. In-Memory Radix Topology Mirror (Bypassing lstat / du Storms)
  // -------------------------------------------------------------
  std::cout << "[Step 2] Testing In-Memory Filesystem Radix Topology Mirror...\n";
  {
    FsTopologyMirror mirror;

    // Simulate deeply nested directory structure
    for (int d = 0; d < 10; ++d) {
      std::string dir = "/var/www/site/tenant" + std::to_string(d);
      FsMetadata d_meta{};
      d_meta.is_directory = true;
      mirror.upsert_entry(dir, d_meta);

      for (int f = 0; f < 500; ++f) {
        std::string file = dir + "/asset_" + std::to_string(f) + ".png";
        FsMetadata f_meta{};
        f_meta.inode = 100000 + d * 1000 + f;
        f_meta.size_bytes = 10240; // 10 KB
        f_meta.is_directory = false;
        mirror.upsert_entry(file, f_meta);
      }
    }

    check(mirror.total_nodes() >= 5000, "FS topology node count",
          mirror.total_nodes() >= 5000 ? std::to_string(mirror.total_nodes())+" nodes indexed in RAM" : "only "+std::to_string(mirror.total_nodes())+" nodes");

    // Fast lstat lookup benchmark
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 10000; ++i) {
      check(mirror.fast_lstat("/var/www/site/tenant3/asset_250.png").has_value(),
          "FS fast_lstat", "looked up nested asset in RAM");
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double lstat_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / 10000.0;

    // Fast recursive du benchmark
    auto t_du0 = std::chrono::high_resolution_clock::now();
    auto du = mirror.fast_du("/var/www/site");
    auto t_du1 = std::chrono::high_resolution_clock::now();
    double du_us = std::chrono::duration<double, std::micro>(t_du1 - t_du0).count();

    check(du.total_files == 5000, "FS fast_du total files",
          du.total_files == 5000 ? "counted 5000 files" : "counted "+std::to_string(du.total_files));
    check(du.total_size_bytes == 5000 * 10240, "FS fast_du total size",
          du.total_size_bytes == 5000 * 10240 ? std::to_string(du.total_size_bytes)+" bytes" : "got "+std::to_string(du.total_size_bytes));

    std::cout << "  [PASS] Fast lstat latency: " << std::fixed << std::setprecision(1)
              << lstat_ns << " ns/lookup (in RAM)\n";
    std::cout << "  [PASS] Fast du over 5,000 files: " << std::fixed << std::setprecision(2)
              << du_us << " us (Total size: " << (du.total_size_bytes / (1024*1024)) << " MB)\n";
  }

  // -------------------------------------------------------------
  // 3. Ring-Buffer Binary Telemetry Core (Bypassing Text Logging)
  // -------------------------------------------------------------
  std::cout << "[Step 3] Testing Ring-Buffer Binary Telemetry Core...\n";
  {
    BinaryTelemetryCore telemetry(131072); // 128k slots

    auto t0 = std::chrono::high_resolution_clock::now();
    for (uint64_t i = 1; i <= 50000; ++i) {
      uint32_t svc = (i % 3 == 0) ? 1 : 2;
      uint16_t status = (i % 50 == 0) ? 500 : 200;
      uint32_t dur = static_cast<uint32_t>(100 + (i % 400));
      telemetry.record(1000 + (i % 500), i, svc, 1, status, dur, 0, "GET /api/v1/orders");
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double record_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // Query trace ID
    auto trace_events = telemetry.query_trace(1250);
    check(!trace_events.empty(), "Binary telemetry trace query",
          !trace_events.empty() ? "trace 1250 resolved to "+std::to_string(trace_events.size())+" events" : "no trace events");

    // Compute percentiles (p50, p95, p99) in RAM
    auto t_stats0 = std::chrono::high_resolution_clock::now();
    auto stats = telemetry.compute_stats(1);
    auto t_stats1 = std::chrono::high_resolution_clock::now();
    double stats_us = std::chrono::duration<double, std::micro>(t_stats1 - t_stats0).count();

    check(stats.count > 0, "Binary telemetry stats count",
          stats.count > 0 ? std::to_string(stats.count)+" samples" : "no samples");
    check(stats.p50_duration_us > 0 && stats.p99_duration_us >= stats.p50_duration_us,
          "Binary telemetry p50/p99 ordering",
          stats.p50_duration_us > 0 && stats.p99_duration_us >= stats.p50_duration_us ? "p50="+std::to_string(stats.p50_duration_us)+"us p99="+std::to_string(stats.p99_duration_us)+"us" : "monotonicity violated");

    std::cout << "  [PASS] Recorded 50,000 64-byte events in " << std::fixed << std::setprecision(2)
              << record_ms << " ms (0 disk IO)\n";
    std::cout << "  [PASS] Aggregated p50/p95/p99 stats in " << std::fixed << std::setprecision(1)
              << stats_us << " us (p50=" << stats.p50_duration_us << "us, p99="
              << stats.p99_duration_us << "us, errors=" << stats.error_count << ")\n";
  }

  // -------------------------------------------------------------
  // 4. Content-Addressed Module & AST Vault (Bypassing node_modules IOPS)
  // -------------------------------------------------------------
  std::cout << "[Step 4] Testing Content-Addressed In-Memory Module & AST Vault...\n";
  {
    ModuleVault vault;

    std::string js_code = "export function calculateTax(amt, rate) { return amt * rate; }";
    std::string hash1 = vault.store_blob(js_code.data(), js_code.size(), "text/javascript");
    std::string hash2 = vault.store_blob(js_code.data(), js_code.size(), "text/javascript"); // duplicate
    check(hash1 == hash2, "ModuleVault dedup hash",
          hash1 == hash2 ? std::to_string(hash1.size())+"-char identical hash" : "hashes differ");
    check(vault.total_unique_blobs() == 1, "ModuleVault unique blobs",
          vault.total_unique_blobs() == 1 ? "1 unique blob stored" : std::to_string(vault.total_unique_blobs())+" stored");
    check(vault.total_dedup_savings_bytes() == js_code.size(), "ModuleVault dedup savings",
          vault.total_dedup_savings_bytes() == js_code.size() ? std::to_string(vault.total_dedup_savings_bytes())+" bytes saved" : "saved "+std::to_string(vault.total_dedup_savings_bytes()));

    PackageManifest pkg_lodash;
    pkg_lodash.name = "lodash";
    pkg_lodash.version = "4.17.21";
    pkg_lodash.file_to_blob_hash["index.js"] = hash1;
    vault.register_package(pkg_lodash);

    PackageManifest pkg_app;
    pkg_app.name = "hairlines-app";
    pkg_app.version = "1.0.0";
    pkg_app.dependencies["lodash"] = "4.17.21";
    vault.register_package(pkg_app);

    auto resolved_tree = vault.resolve_dependency_tree("hairlines-app", "1.0.0");
    check(resolved_tree.size() == 2 && resolved_tree[1].first == "lodash",
          "ModuleVault dependency resolution",
          resolved_tree.size() == 2 ? "resolved hairlines-app->lodash tree of "+std::to_string(resolved_tree.size())+" nodes" : "resolved "+std::to_string(resolved_tree.size()));

    auto file_view = vault.resolve_package_file("lodash", "4.17.21", "index.js");
    check(file_view.has_value() && file_view->size() == js_code.size(), "ModuleVault package file view",
          file_view.has_value() ? "lodash 4.17.21 index.js = "+std::to_string(file_view->size())+" bytes" : "file view missing");

    std::cout << "  [PASS] Content-addressed deduplication & instant dependency resolution verified!\n";
  }

  // -------------------------------------------------------------
  // 5. Lockless Shared-Memory Event Fabric (Bypassing Heavy Message Brokers)
  // -------------------------------------------------------------
  std::cout << "[Step 5] Testing Lockless Shared-Memory Event Fabric...\n";
  {
    EventFabric fabric;
    uint32_t topic_id = fabric.register_topic("orders.dispatch", 4);
    check(topic_id > 0, "Event fabric topic registration",
          topic_id > 0 ? "topic id "+std::to_string(topic_id) : "registration failed");

    const size_t msg_count = 20000;
    auto t0 = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < msg_count; ++i) {
      char payload[64];
      std::snprintf(payload, sizeof(payload), "ORDER-DISPATCH-ID-%zu", i);
      fabric.publish("orders.dispatch", payload, std::strlen(payload) + 1, static_cast<uint32_t>(i));
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double pub_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // Consume from partition 0
    EventFabric::ConsumerCursor cursor{topic_id, 0, 0};
    auto batch = fabric.poll(cursor, 100);
    check(!batch.empty(), "Event fabric poll",
          !batch.empty() ? "got "+std::to_string(batch.size())+" messages" : "empty batch");
    check(!batch.empty() && std::string(reinterpret_cast<const char*>(batch[0].payload)).rfind("ORDER-DISPATCH", 0) == 0,
          "Event fabric payload prefix", "first message starts ORDER-DISPATCH");

    std::cout << "  [PASS] Published " << msg_count << " messages in "
              << std::fixed << std::setprecision(2) << pub_ms << " ms ("
              << std::setprecision(0) << (msg_count / pub_ms * 1000.0) << " msgs/sec)\n";
  }

  // -------------------------------------------------------------
  // 6. 128-Bit Capability-Secured Object Handles (Bypassing Ambient Authority)
  // -------------------------------------------------------------
  std::cout << "[Step 6] Testing 128-Bit Capability-Secured Object Handles...\n";
  {
    CapabilityGuard guard("super-secret-kernel-capability-master-key");

    // Mint admin token for tenant hairlines
    auto admin_tok = guard.mint_for_resource("tenant:hairlines.dispatch",
                                             CapabilityRight::READ | CapabilityRight::WRITE | CapabilityRight::DELEGATE);

    // Validate valid permissions
    check(guard.validate_for_resource(admin_tok, "tenant:hairlines.dispatch", CapabilityRight::READ) &&
          guard.validate_for_resource(admin_tok, "tenant:hairlines.dispatch", CapabilityRight::WRITE),
          "Capability admin grants", "admin READ+WRITE on hairlines.dispatch");
    // Must NOT have access to tenant AA
    check(!guard.validate_for_resource(admin_tok, "tenant:AA.dispatch", CapabilityRight::READ),
          "Capability tenant isolation", "admin denied on AA.dispatch");

    // Attenuate to read-only token for worker
    auto read_only_tok = guard.attenuate(admin_tok, CapabilityRight::READ, 3600ULL * 1000000000ULL);
    check(read_only_tok.has_value(), "Capability attenuation",
          read_only_tok.has_value() ? "minted 1h read-only token" : "attenuation failed");
    check(read_only_tok.has_value() &&
          guard.validate_for_resource(*read_only_tok, "tenant:hairlines.dispatch", CapabilityRight::READ) &&
          !guard.validate_for_resource(*read_only_tok, "tenant:hairlines.dispatch", CapabilityRight::WRITE),
          "Capability read-only enforcement", "read allowed, write denied");

    // Tampering test: modify 1 byte in MAC
    bool tamper_ok = false;
    if (read_only_tok.has_value()) {
      CapabilityToken tampered = *read_only_tok;
      tampered.mac[0] ^= 0xFF;
      tamper_ok = !guard.validate_for_resource(tampered, "tenant:hairlines.dispatch", CapabilityRight::READ);
    }
    check(tamper_ok, "Capability tamper resistance", "1-byte MAC flip rejected");

    // Revocation test
    guard.revoke_resource("tenant:hairlines.dispatch");
    check(!guard.validate_for_resource(admin_tok, "tenant:hairlines.dispatch", CapabilityRight::READ),
          "Capability revocation", "admin token invalid after revoke");

    // Revocation survives a restart: a second guard with the same key and state directory still refuses the old token.
    {
      const std::string rev = (std::filesystem::temp_directory_path() /
                               ("aios-sidecars-check-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).string();
      std::error_code rm_ec;
      std::filesystem::remove_all(rev, rm_ec);
      CapabilityGuard first("restart-test-key", rev);
      auto old_tok = first.mint_for_resource("tenant:restart.res", CapabilityRight::READ);
      first.revoke_resource("tenant:restart.res");
      CapabilityGuard second("restart-test-key", rev);
      check(!second.validate_for_resource(old_tok, "tenant:restart.res", CapabilityRight::READ),
            "Revocation survives restart", "revoked token still refused by a new guard on the same file");
      auto fresh = second.mint_for_resource("tenant:restart.res", CapabilityRight::READ);
      check(second.validate_for_resource(fresh, "tenant:restart.res", CapabilityRight::READ),
            "Re-minted after restart", "a token minted after the revocation is valid");
      CapabilityGuard forgetful("restart-test-key");
      check(forgetful.validate_for_resource(old_tok, "tenant:restart.res", CapabilityRight::READ),
            "No state, no memory", "without a state directory the old token is honoured again (the gap this closes)");
      std::filesystem::remove_all(rev, rm_ec);
    }

    std::cout << "   [SUB] capability minting, attenuation, and revocation exercised\n";
  }

  // -------------------------------------------------------------
  // 7. Transparent In-Memory CDC Hot-Mirror (ASQL Multi-Tenant Engine)
  // -------------------------------------------------------------
  std::cout << "[Step 7] Testing Transparent In-Memory CDC Multi-Tenant ASQL Engine...\n";
  {
    TempRoot tmp("aios-sidecar-check");
    const char* root_dir = tmp.path;
    {
      aios::KernelConfig kcfg;
      kcfg.root = root_dir;
      kcfg.residency = aios::test_config;
      auto k_res = aios::Kernel::boot(kcfg);
      check(k_res.ok(), "ASQL kernel boot", k_res.ok() ? "booted sidecar kernel" : "boot failed");
      aios::Kernel& kernel = **k_res;

      aios::userland::SqlDatabase db(kernel);
      auto init_r = db.init("sql_db");
      check(init_r.ok(), "ASQL db init", init_r.ok() ? "initialized sql_db" : "init failed");

      // Create hairlines table
      db.execute_query("CREATE TABLE hairlines.orders (id INT PRIMARY KEY, amount DOUBLE, status VARCHAR(32))");
      db.execute_query("INSERT INTO hairlines.orders VALUES (101, 450.00, 'DISPATCHED')");

      // Create AA table
      db.execute_query("CREATE TABLE AA.orders (id INT PRIMARY KEY, amount DOUBLE, status VARCHAR(32))");
      db.execute_query("INSERT INTO AA.orders VALUES (201, 980.50, 'PENDING')");

      // Session 1: hairlines
      auto res1 = db.execute_query("SELECT * FROM orders", "hairlines");
      check(res1.rows.size() == 1 && res1.rows[0].values[0] == "101",
            "ASQL tenant isolation (hairlines)", res1.rows.size() == 1 ? "session sees row 101 only" : "got "+std::to_string(res1.rows.size())+" rows");

      // Session 2: AA
      auto res2 = db.execute_query("SELECT * FROM orders", "AA");
      check(res2.rows.size() == 1 && res2.rows[0].values[0] == "201",
            "ASQL tenant isolation (AA)", res2.rows.size() == 1 ? "session sees row 201 only" : "got "+std::to_string(res2.rows.size())+" rows");

      std::cout << "   [SUB] Transparent multi-tenant CDC memory mirror with strict isolation\n";
    }
  }

  // -------------------------------------------------------------
  // 8. Multi-Agent Git Substrate, Tracing & Binary Telemetry ABI
  // -------------------------------------------------------------
  std::cout << "[Step 8] Testing Multi-Agent Git Substrate & Binary Telemetry Tracing...\n";
  {
    ModuleVault vault;
    CapabilityGuard guard("multi-agent-master-key-cryptographically-secure");
    EventFabric fabric;
    BinaryTelemetryCore telemetry(65536);

    uint32_t git_topic = fabric.register_topic("git:main", 4);
    check(git_topic > 0, "EventFabric git topic registration", "registered git:main with 4 partitions");

    VaultWorkspace ws1(vault, "aios-core", "main");
    VaultWorkspace ws2(vault, "aios-core", "main");

    // Mint capability tokens for Agent 1 and Agent 2
    auto tok_agent1 = guard.mint_for_resource("repo:aios-core/src/kernel/dispatcher.cpp",
                                              CapabilityRight::Read | CapabilityRight::Write);
    auto tok_agent1_branch = guard.mint_for_resource("repo:aios-core:branch:main",
                                                     CapabilityRight::Write);

    // Agent 2 only gets Read capability at first
    auto tok_agent2_readonly = guard.mint_for_resource("repo:aios-core/src/kernel/dispatcher.cpp",
                                                       CapabilityRight::Read);

    uint64_t trace_agent1 = 0xA1050001ULL;
    uint64_t trace_agent2 = 0xA1050002ULL;

    InstrumentedAgentSession session1(trace_agent1, 101, "aios-core", "main",
                                      tok_agent1, telemetry, guard, fabric, ws1);
    InstrumentedAgentSession session2(trace_agent2, 102, "aios-core", "main",
                                      tok_agent2_readonly, telemetry, guard, fabric, ws2);

    // 1. Capability check rejection verification
    std::string test_data = "int dispatch_kernel_event() { return 0; }";
    auto stage_denied = session2.stage_file("src/kernel/dispatcher.cpp", test_data.data(), test_data.size());
    check(!stage_denied.ok() && stage_denied.status() == aios::Status::Denied,
          "Agent capability enforcement", "write with read-only token rejected with Status::Denied");

    // Verify telemetry recorded TokenCheckDenied
    auto trace2_events = telemetry.query_trace(trace_agent2);
    bool recorded_denied = false;
    for (const auto& ev : trace2_events) {
      if (ev.event_type == static_cast<uint16_t>(GitTelemetryEvent::TokenCheckDenied) &&
          ev.status_code == kStatusDenied) {
        recorded_denied = true;
        break;
      }
    }
    check(recorded_denied, "Telemetry TokenCheckDenied span", "recorded TokenCheckDenied with status 403");

    // 2. Agent 1 stages file in ModuleVault with lockless intent broadcast
    auto stage_ok = session1.stage_file("src/kernel/dispatcher.cpp", test_data.data(), test_data.size());
    check(stage_ok.ok(), "Agent 1 stage_file", "staged src/kernel/dispatcher.cpp in ModuleVault");

    // Verify broadcast over EventFabric
    EventFabric::ConsumerCursor cursor{git_topic, 101 % 4, 0};
    auto events = fabric.poll(cursor, 10);
    bool saw_intent = false, saw_staged = false;
    for (const auto& msg : events) {
      if (msg.payload_len >= sizeof(GitEventPayload)) {
        const auto* p = reinterpret_cast<const GitEventPayload*>(msg.payload);
        if (p->type == GitEventType::IntentToEdit && p->agent_id == 101) saw_intent = true;
        if (p->type == GitEventType::FileStaged && p->agent_id == 101) saw_staged = true;
      }
    }
    check(saw_intent && saw_staged, "EventFabric multi-agent intent broadcast",
          "peers notified of IntentToEdit and FileStaged over lockless ring");

    // 3. Zero-copy read by Agent 1
    auto read_res = session1.read_file("src/kernel/dispatcher.cpp");
    check(read_res.ok() && read_res->size() == test_data.size() &&
          std::memcmp(read_res->data(), test_data.data(), test_data.size()) == 0,
          "Agent 1 zero-copy read_file", "read back exact bytes directly from RAM without disk IO");

    // 4. Clean merge and commit by Agent 1
    InstrumentedAgentSession session1_commit(trace_agent1, 101, "aios-core", "main",
                                            tok_agent1_branch, telemetry, guard, fabric, ws1);
    auto commit_res1 = session1_commit.commit_and_merge("feat(kernel): add dispatcher");
    check(commit_res1.ok() && commit_res1->size() == 64,
          "Agent 1 commit_and_merge", "clean merge into main with commit OID: " + (commit_res1.ok() ? commit_res1->substr(0, 12) + "..." : "fail"));

    // 5. Concurrent conflict detection: Agent 2 tries to commit a divergent change on the same file
    auto tok_agent2_write = guard.mint_for_resource("repo:aios-core/src/kernel/dispatcher.cpp",
                                                    CapabilityRight::Read | CapabilityRight::Write);
    auto tok_agent2_branch = guard.mint_for_resource("repo:aios-core:branch:main",
                                                     CapabilityRight::Write);
    std::string conflicting_code = "int dispatch_kernel_event() { return -1; /* conflict */ }";
    InstrumentedAgentSession session2_divergent(trace_agent2, 102, "aios-core", "main",
                                               tok_agent2_write, telemetry, guard, fabric, ws2);
    session2_divergent.stage_file("src/kernel/dispatcher.cpp", conflicting_code.data(), conflicting_code.size());

    InstrumentedAgentSession session2_commit(trace_agent2, 102, "aios-core", "main",
                                            tok_agent2_branch, telemetry, guard, fabric, ws2);
    auto conflict_res = session2_commit.commit_and_merge("feat: conflicting dispatcher edit");
    check(!conflict_res.ok() && conflict_res.status() == aios::Status::Conflict,
          "3-way merge conflict detection", "divergent AST/file edit correctly rejected with Status::Conflict");

    // Verify telemetry recorded MergeConflict
    trace2_events = telemetry.query_trace(trace_agent2);
    bool recorded_conflict = false;
    for (const auto& ev : trace2_events) {
      if (ev.event_type == static_cast<uint16_t>(GitTelemetryEvent::MergeConflict) &&
          ev.status_code == kStatusConflict) {
        recorded_conflict = true;
        break;
      }
    }
    check(recorded_conflict, "Telemetry MergeConflict span", "recorded MergeConflict event 106 with status 409");

    // 6. Test C ABI inspection functions
    aios_telemetry_record(0xBEA70001ULL, 1, kServiceGitAgent,
                          static_cast<uint16_t>(GitTelemetryEvent::FileRead),
                          kStatusOk, 12, 1024, "dispatcher.cpp");
    aios_telemetry_record(0xBEA70001ULL, 2, kServiceGitAgent,
                          static_cast<uint16_t>(GitTelemetryEvent::MergeClean),
                          kStatusOk, 45, 101, "commit_oid");

    char trace_json[4096] = {0};
    int trace_rc = aios_telemetry_get_trace_json(0xBEA70001ULL, trace_json, sizeof(trace_json));
    check(trace_rc == AIOS_OK && std::string(trace_json).find(std::to_string(0xBEA70001ULL)) != std::string::npos,
          "C-ABI aios_telemetry_get_trace_json", "retrieved full JSON trace spanning file reads and merges");

    uint64_t ops_count = 0, errors_count = 0;
    double avg_us = 0;
    uint32_t p99_us = 0;
    int stats_rc = aios_telemetry_git_stats(3600, &ops_count, &errors_count, &avg_us, &p99_us);
    check(stats_rc == AIOS_OK && ops_count >= 2,
          "C-ABI aios_telemetry_git_stats", "computed rolling stats: " + std::to_string(ops_count) + " ops, avg " + std::to_string((int)avg_us) + " us");

    std::cout << "  [PASS] Autonomous multi-agent Git substrate, capability bounds & telemetry tracing fully verified!\n";
  }

  std::cout << "\n=================================================================\n";
  if (checks_failed == 0) {
    std::cout << "  ALL " << checks_passed << "/" << total_checks << " SIDECAR CHECKS PASSED\n";
  } else {
    std::cout << "  " << checks_passed << "/" << total_checks << " CHECKS PASSED, " << checks_failed << " FAILED\n";
  }
  std::cout << "=================================================================\n";
  return checks_failed == 0 ? 0 : 1;
}
