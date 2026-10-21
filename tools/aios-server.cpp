// The ASQL server: serves the MySQL wire protocol over an AIOS root and vacuums the store on
// its own when the log grows or memory runs short.

#undef NDEBUG
#include <iostream>
#include <string>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"
#include "aios/userland/mysql_wire.hpp"
#include "aios/storage/object_store.hpp"

using namespace aios;
using namespace aios::userland;

static std::atomic<bool> g_shutdown{false};

static void handle_signal(int) {
  g_shutdown = true;
}

// VACUUM used to only ever run because someone remembered to run it by hand
// (which is how objects.log grew to 2.1GB on one box before anyone noticed).
// This is the fix: check periodically and run it automatically.
//
// System memory pressure (ResidencyController's available_bytes() <=
// low_watermark, the same signal the tiering system already uses to decide
// what to evict from the hot cache) is deliberately NOT the primary trigger
// here, even though it's the signal that already exists for "am I under
// pressure": a box can have gigabytes of RAM free while objects.log grows
// unbounded on disk -- pressure never fires, and the log just keeps growing.
// That's exactly what happened on chi. The log's own on-disk size is what
// actually tracks the problem, so it's the primary trigger; pressure is
// kept as a secondary, immediate trigger for when the box really is tight.
static uint64_t env_u64(const char* name, uint64_t fallback) {
  const char* v = std::getenv(name);
  if (!v || !*v) return fallback;
  try { return std::stoull(v); } catch (...) { return fallback; }
}

static void auto_vacuum_loop(Kernel& kernel, const std::string& root_dir) {
  if (env_u64("AIOS_AUTOVACUUM_DISABLE", 0) != 0) return;

  const auto kCheckInterval = std::chrono::seconds(env_u64("AIOS_AUTOVACUUM_CHECK_SEC", 30));
  const auto kCooldown = std::chrono::seconds(env_u64("AIOS_AUTOVACUUM_COOLDOWN_SEC", 300));
  const uint64_t kSizeThresholdBytes = env_u64("AIOS_AUTOVACUUM_SIZE_MB", 100) << 20;
  // A vacuum reads and rewrites the WHOLE log. Below this share of new bytes
  // it costs more I/O than it can possibly give back, so it waits.
  const uint64_t kMinGrowthPercent = env_u64("AIOS_AUTOVACUUM_MIN_GROWTH_PCT", 25);

  const std::string log_path = root_dir + "/store/objects.log";
  auto last_vacuum = std::chrono::steady_clock::now() - kCooldown;  // allow an immediate first run
  uint64_t last_compacted_bytes = 0;

  auto last_check = std::chrono::steady_clock::now();
  while (!g_shutdown.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (g_shutdown.load()) break;

    auto now = std::chrono::steady_clock::now();
    if (now - last_check < kCheckInterval) continue;
    last_check = now;
    if (now - last_vacuum < kCooldown) continue;

    uint64_t log_bytes = 0;
    auto info = kernel.host().stat(log_path);
    if (info.ok()) log_bytes = info->size;

    // GROWTH since the last compaction, not the log's absolute size. A vacuum
    // reads and rewrites the WHOLE log, so what it costs is set by the log,
    // and what it gives back is set by the garbage -- and those are different
    // numbers. On chi they were wildly different: 1.03GB of genuinely LIVE
    // conversations, growing past the 100MB floor every few minutes, so the
    // engine rewrote a gigabyte to reclaim ~6% of it and then did it again
    // five minutes later. Forever. A store whose live set is simply large was
    // being treated as a store full of garbage.
    //
    // `grown` is the only cheap honest bound on what is reclaimable: bytes
    // appended since the last compaction. Nothing older than that pass can be
    // garbage -- it was just proved live -- so a vacuum cannot give back more
    // than this.
    const uint64_t grown = log_bytes > last_compacted_bytes ? log_bytes - last_compacted_bytes : 0;
    const bool first_run = (last_compacted_bytes == 0);
    // Cost is the whole log; the most that can come back is `grown`. Run when
    // that ratio is worth having. It scales with the store instead of against
    // it: a 100MB log compacts after 100MB of churn, a 1GB log after 250MB,
    // and neither one rewrites itself to reclaim a rounding error.
    const bool size_triggered = log_bytes >= kSizeThresholdBytes &&
        (first_run || (grown >= kSizeThresholdBytes &&
                       grown * 100 >= log_bytes * kMinGrowthPercent));
    const bool pressure_triggered = kernel.host().available_bytes() > 0 &&
        kernel.host().available_bytes() <= kernel.residency().low_watermark_bytes();
    if (!size_triggered && !pressure_triggered) continue;

    std::cout << "[auto-vacuum] triggered ("
              << (size_triggered ? "grown " + std::to_string(grown >> 20) + "MB of " +
                                   std::to_string(log_bytes >> 20) + "MB"
                                 : "memory pressure")
              << ") -- running VACUUM OBJECTS...\n";
    std::cout.flush();
    auto vac = kernel.store().vacuum();
    last_vacuum = now;
    if (!vac.ok()) {
      std::cout << "[auto-vacuum] FAILED: " << vac.error().message << "\n";
      std::cout.flush();
      continue;
    }
    last_compacted_bytes = vac->bytes_after;
    std::cout << "[auto-vacuum] scanned=" << vac->total_objects_scanned
              << " kept=" << vac->live_objects_written
              << " superseded_pruned=" << vac->dead_objects_pruned
              << " tombstones_pruned=" << vac->tombstones_pruned
              << " expired_pruned=" << vac->expired_objects_pruned
              << " bytes " << vac->bytes_before << " -> " << vac->bytes_after << "\n";
    std::cout.flush();
  }
}

int main(int argc, char** argv) {
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  uint16_t port = 3306;
  std::string bind_addr = "127.0.0.1";
  // The system temp directory (/tmp on Linux), so the default works on every host.
  std::string root_dir = (std::filesystem::temp_directory_path() / "aios-server-data").string();
  std::string downstream_host = "";
  uint16_t downstream_port = 0;
  // Write-behind and write-through are OPT-IN gateway patterns. The default is
  // a plain local asql server: durable local writes, no WAL, no drain thread.
  bool write_behind = false;
  bool write_through = false;
  // An account to require (--user, --pass-file: the password is the file's first line). Without one,
  // only clients on this machine are let in, unauthenticated; with one, every client must sign in.
  std::string account_user, account_pass_file;
  // --socket PATH: listen on a unix-domain socket there. Given alone it is the only way in (no TCP port at all,
  // for a sandbox with no network); with --port as well, both.
  std::string socket_path;
  std::string db_dir;   // --db: the database directory under the root; aios_gateway unless only sql_db is there
  bool port_given = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--port" && i + 1 < argc) {
      port = static_cast<uint16_t>(std::stoi(argv[++i]));
      port_given = true;
    } else if (arg == "--bind" && i + 1 < argc) {
      bind_addr = argv[++i];
    } else if (arg == "--socket" && i + 1 < argc) {
      socket_path = argv[++i];
    } else if (arg == "--root" && i + 1 < argc) {
      root_dir = argv[++i];
    } else if ((arg == "--db" || arg == "--db-dir") && i + 1 < argc) {
      db_dir = argv[++i];
    } else if (arg == "--user" && i + 1 < argc) {
      account_user = argv[++i];
    } else if (arg == "--pass-file" && i + 1 < argc) {
      account_pass_file = argv[++i];
    } else if (arg == "--downstream" && i + 1 < argc) {
      std::string ds = argv[++i];
      size_t colon = ds.find(':');
      if (colon != std::string::npos) {
        downstream_host = ds.substr(0, colon);
        downstream_port = static_cast<uint16_t>(std::stoi(ds.substr(colon + 1)));
      }
    } else if (arg == "--mode" && i + 1 < argc) {
      std::string m = argv[++i];
      if (m == "write_through") {
        write_through = true;
        write_behind = false;
      } else if (m == "write_behind") {
        write_behind = true;
        write_through = false;
      } else {
        // "local" / "local_only" / anything else: plain asql.
        write_behind = false;
        write_through = false;
      }
    }
  }
  // Env opt-in, for supervisors that cannot pass a flag.
  if (const char* wb = std::getenv("AIOS_WRITE_BEHIND")) {
    std::string v(wb);
    if (!v.empty() && v != "0" && v != "false" && v != "off" && v != "no") {
      write_behind = true;
      write_through = false;
    }
  }

  if (std::filesystem::path(db_dir).is_absolute() && root_dir == ".") {
    root_dir = std::filesystem::path(db_dir).parent_path().string();
    db_dir = std::filesystem::path(db_dir).filename().string();
  }

  std::cout << "========================================================================\n";
  std::cout << "  AIOS Microsecond Storage & MySQL Wire Gateway Server                  \n";
  std::cout << "========================================================================\n";
  std::cout << "  Root Dir       : " << root_dir << "\n";
  const bool tcp = socket_path.empty() || port_given;
  if (tcp) std::cout << "  Listening On   : " << bind_addr << ":" << port << "\n";
  if (!socket_path.empty()) std::cout << "  Unix Socket    : " << socket_path << "\n";
  std::cout << "  Mode           : " << (write_behind ? "Pattern B (Write-Behind WAL)"
                                         : write_through ? "Pattern A (Write-Through)"
                                                         : "Local-Only (durable local writes, write-behind OFF)") << "\n";
  if (!downstream_host.empty()) {
    std::cout << "  Downstream DB  : " << downstream_host << ":" << downstream_port << "\n";
  }
  std::cout << "------------------------------------------------------------------------\n";

  // A storage server never schedules GPU work, but booting the kernel opens
  // the OpenCL/CUDA/Vulkan runtimes anyway -- measured at 389 MB of resident
  // memory on an EMPTY store (397 MB at boot against 8 MB with them off).
  // chi-il-1 only escapes that because its unit happens to set the variable;
  // anyone who runs this binary by hand pays it for nothing. Default it off
  // here and let an explicit setting win.
#ifdef _WIN32
  if (!std::getenv("AIOS_NO_ACCEL")) _putenv_s("AIOS_NO_ACCEL", "1");
#else
  if (!std::getenv("AIOS_NO_ACCEL")) setenv("AIOS_NO_ACCEL", "1", 0);
#endif

  KernelConfig kcfg;
  kcfg.root = root_dir;
  auto k_res = Kernel::boot(kcfg);
  if (!k_res.ok()) {
    std::cerr << "Failed to boot AIOS Kernel: " << k_res.error().message << "\n";
    return 1;
  }
  Kernel& kernel = **k_res;

  if (db_dir.empty()) {   // a root `compute serve` made (sql_db) opens as it is; a new one gets the gateway's own
    const std::filesystem::path root(root_dir);
    db_dir = std::filesystem::exists(root / "sql_db") && !std::filesystem::exists(root / "aios_gateway") ? "sql_db" : "aios_gateway";
  }

  SqlDatabase db(kernel);
  auto db_res = db.init(db_dir);
  if (!db_res.ok()) {
    std::cerr << "Failed to init SqlDatabase: " << db_res.error().message << "\n";
    return 1;
  }

  if (write_behind) {
    db.set_sync_mode(WriteSyncMode::WriteBehind);
    db.init_write_behind();
  } else if (write_through) {
    db.set_sync_mode(WriteSyncMode::WriteThrough);
  } else {
    // Default: nothing gateway-shaped is armed at all.
    db.set_sync_mode(WriteSyncMode::LocalOnly);
  }

  if (!downstream_host.empty() && downstream_port > 0) {
    DbConnectionProfile prof;
    prof.id = "downstream";
    prof.name = "Downstream Server";
    prof.host = downstream_host;
    prof.port = downstream_port;
    prof.user = "root";
    prof.password = "";
    prof.database = "downstream";
    db.set_downstream_target(prof);
    if (write_behind) {
      db.start_wal_drain_worker(20);
    }
  }

  MysqlWireServer server(kernel, db, port, bind_addr);
  if (!socket_path.empty()) server.set_unix_socket(socket_path);
  server.set_tcp(tcp);
  if (!account_user.empty() || !account_pass_file.empty()) {
    std::string pass;
    std::ifstream pf(account_pass_file);
    std::getline(pf, pass);
    while (!pass.empty() && (pass.back() == '\r' || pass.back() == '\n' || pass.back() == ' ')) pass.pop_back();
    if (account_user.empty() || pass.empty()) {
      std::cerr << "--user and --pass-file go together, and the file must hold a password (" << account_pass_file << ")\n";
      return 1;
    }
    server.add_account(account_user, pass);
    std::cout << "  Account        : " << account_user << " (every client signs in)\n";
  }
  auto srv_res = server.start();
  if (!srv_res.ok()) {
    std::cerr << "Failed to start MySQL wire server (" << srv_res.error().message << ")\n";
    return 1;
  }

  server.start_background();
  if (tcp) std::cout << "  [ONLINE] AIOS MySQL Wire Gateway listening on " << bind_addr << ":" << server.port() << "\n";
  if (!socket_path.empty()) std::cout << "  [ONLINE] AIOS MySQL Wire Gateway listening on unix:" << socket_path << "\n";
  std::cout << "  Ready for harness connections, queries, and duel operations.\n\n";

  std::thread vacuum_thread(auto_vacuum_loop, std::ref(kernel), root_dir);

  while (!g_shutdown.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  std::cout << "\nShutting down AIOS Gateway Server...\n";
  vacuum_thread.join();
  server.stop();
  db.flush_write_behind();  // no-op unless write-behind was turned on
  db.flush_now();           // checkpoint the store + empty the redo log
  std::cout << "AIOS Gateway Server safely stopped.\n";
  return 0;
}
