// The ASQL engine daemon: serves ASQL over the MySQL wire, optionally mirroring an upstream
// MariaDB/MySQL by CDC; run with --help for the options.

#undef NDEBUG
#include <iostream>
#include <iomanip>
#include <string>
#include <sstream>
#include <vector>
#include <unordered_set>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <unistd.h>
#include <sys/stat.h>

#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"
#include "aios/userland/mysql_wire.hpp"
#include "aios/userland/mysql_binlog.hpp"
#include "aios/storage/object_store.hpp"
#include "aios/sidecar/shm_abi.hpp"
#include "aios/sidecar/fs_topology.hpp"
#include "aios/sidecar/binary_telemetry.hpp"
#include "aios/sidecar/module_vault.hpp"
#include "aios/sidecar/event_fabric.hpp"
#include "aios/sidecar/capability_guard.hpp"
#include "aios/userland/asql_capability.hpp"

using namespace aios;
using namespace aios::userland;
using namespace aios::sidecar;

static std::atomic<bool> g_shutdown{false};

static void handle_signal(int) {
  g_shutdown = true;
}

static void print_banner() {
  std::cout << R"(
  █████╗ ██╗ ██████╗ ███████╗    ███████╗███╗   ██╗ ██████╗ ██╗███╗   ██╗███████╗
 ██╔══██╗██║██╔═══██╗██╔════╝    ██╔════╝████╗  ██║██╔════╝ ██║████╗  ██║██╔════╝
 ███████║██║██║   ██║███████╗    █████╗  ██╔██╗ ██║██║  ███╗██║██╔██╗ ██║█████╗  
 ██╔══██║██║██║   ██║╚════██║    ██╔══╝  ██║╚██╗██║██║   ██║██║██║╚██╗██║██╔══╝  
 ██║  ██║██║╚██████╔╝███████║    ███████╗██║ ╚████║╚██████╔╝██║██║ ╚████║███████╗
 ╚═╝  ╚═╝╚═╝ ╚═════╝ ╚══════╝    ╚══════╝╚═╝  ╚═══╝ ╚═════╝ ╚═╝╚═╝  ╚═══╝╚══════╝
  UNIFIED ZERO-COPY DATA & INFRASTRUCTURE ELIMINATION SIDECAR (15 MB BARE METAL)
===================================================================================
)" << std::endl;
}

static void print_usage(const char* prog) {
  std::cout << "Usage: " << prog << " [options]\n\n"
            << "Options:\n"
            << "  --upstream=<target>       MariaDB/MySQL upstream for CDC (e.g. 127.0.0.1:3306 or /run/mysqld/mysqld.sock)\n"
            << "  --upstream-user=<user>    Upstream replication username (default: root)\n"
            << "  --upstream-pass=<pass>    Upstream replication password\n"
            << "  --upstream-pass-file=<f>  Upstream replication password, first line of a file\n"
            << "  --upstream-db=<db>        Upstream database name (required with --upstream)\n"
            << "  --server-id=<n>           Replica server id (default: derived from this root's identity)\n"
            << "  --tables=<t1,t2,...>      Comma-separated whitelist of tables to mirror; a listed table\n"
            << "                            not in the store yet is copied from the upstream first\n"
            << "  --port=<port>             MySQL Wire Protocol TCP port (default: 3307)\n"
            << "  --bind=<ip>               MySQL Wire Protocol bind IP (default: 0.0.0.0)\n"
            << "  --socket=<path>           Also listen on this UNIX domain socket (default: none)\n"
            << "  --sql-user=<user>         Account a client logs in with (required unless --trust-loopback)\n"
            << "  --sql-pass=<pass>         Its password (visible in ps; prefer --sql-pass-file)\n"
            << "  --sql-pass-file=<file>    Its password, first line of a file\n"
            << "  --trust-loopback          Let loopback clients in without an account\n"
            << "  --max-connections=<n>     Refuse connections past n with error 1040 (default 151, AIOS_MAX_CONNECTIONS; 0 = no limit)\n"
            << "  --query-memory-mb=<n>     Memory the statements running at once may reserve for their intermediates;\n"
            << "                            past it a read waits its turn (default 2048, AIOS_QUERY_MEMORY_MB; 0 = no limit)\n"
            << "  --shm-ring=<name>         POSIX SHM ring it creates (default: aios_ring); nothing in the engine writes to it\n"
            << "  --telemetry-slots=<N>     In-memory telemetry slots (default: 131072)\n"
            << "  --watch-fs=<dir>          Mirror this tree (every file and directory) into the in-memory FS topology; rescanned every 60 s\n"
            << "  --root=<dir>              AIOS data root directory (default: /tmp/aios-engine-data)\n"
            << "  --mode=<mode>             Storage sync mode: local (default), write_behind or write_through;\n"
            << "                            both of the latter need --downstream\n"
            << "  --downstream=<target>     Where write_behind/write_through deliver writes: host:port, a\n"
            << "                            socket path, or 'upstream' (the --upstream server and account)\n"
            << "  --downstream-user=<user>  --downstream-pass=<p>  --downstream-pass-file=<f>\n"
            << "  --downstream-db=<db>      Downstream account and database (default: the upstream's)\n"
            << "  --log-bin                 Write a binlog of every committed statement (<root>/<db>/binlog) and serve\n"
            << "                            it to MySQL/MariaDB replicas; stays on once started (--skip-log-bin)\n"
            << "  --listen-gate             Open no listener until replication is connected and caught up\n"
            << "                            (needs --upstream)\n"
            << "  --benchmark               Run internal self-diagnostic throughput benchmark on startup\n"
            << "  --help, -h                Show this help message\n\n";
}

// The capability master key: AIOS_CAPABILITY_KEY if set, else <root>/capability.key,
// else 32 random bytes written there (0600) on first start. Machines that must
// accept each other's tokens share the file (scripts/aios-setup.sh --key-from).
static std::string load_capability_key(const std::string& root_dir) {
  if (const char* env = std::getenv("AIOS_CAPABILITY_KEY"); env && *env) return env;
  const std::string path = root_dir + "/capability.key";
  {
    std::ifstream in(path);
    std::string key;
    if (in && std::getline(in, key) && !key.empty()) return key;
  }
  unsigned char raw[32];
  std::ifstream rnd("/dev/urandom", std::ios::binary);
  if (!rnd.read(reinterpret_cast<char*>(raw), sizeof raw)) return "";
  static const char* hex = "0123456789abcdef";
  std::string key;
  for (unsigned char b : raw) { key += hex[b >> 4]; key += hex[b & 15]; }
  const mode_t old_mask = ::umask(077);
  std::ofstream out(path, std::ios::trunc);
  out << key << "\n";
  ::umask(old_mask);
  if (!out) return "";
  std::cout << "  • Capability key       : generated " << path << " (keep it private; share it only between machines that must trust each other)\n";
  return key;
}

int main(int argc, char** argv) {
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  std::string upstream_target;
  std::string upstream_user = "root";
  std::string upstream_pass;
  std::string upstream_db;   // required with --upstream; aios_db otherwise
  std::string table_whitelist_str;
  uint16_t port = 3307;
  std::string bind_addr = "0.0.0.0";
  std::string socket_path;   // none unless asked for: the default used to be printed but never opened
  std::string shm_ring_name = "aios_ring";
  uint32_t telemetry_slots = 131072;
  std::string watch_fs_dir;
  std::string root_dir = "/tmp/aios-engine-data";
  // Gateway sync patterns are OPT-IN; the default is plain durable local asql.
  bool write_behind = false;
  bool write_through = false;
  bool listen_gate = false;
  bool log_bin = false, skip_log_bin = false;
  bool run_benchmark = false;
  std::string sql_user;
  std::string sql_pass;
  std::string sql_pass_file;
  bool trust_loopback = false;
  long long max_connections = -1, query_memory_mb = -1;   // -1: the default (environment, then built in)
  std::string upstream_pass_file;
  uint32_t server_id = 0;
  std::string mode = "local";
  std::string downstream_target;
  std::string downstream_user, downstream_pass, downstream_pass_file, downstream_db;
  bool downstream_user_set = false, downstream_pass_set = false;

  auto read_pass_file = [](const std::string& path, std::string* out) {
    std::ifstream pf(path);
    if (!pf) return false;
    std::getline(pf, *out);
    while (!out->empty() && (out->back() == '\n' || out->back() == '\r')) out->pop_back();
    return true;
  };

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      return 0;
    } else if (arg.rfind("--upstream=", 0) == 0) {
      upstream_target = arg.substr(11);
    } else if (arg.rfind("--upstream-user=", 0) == 0) {
      upstream_user = arg.substr(16);
    } else if (arg.rfind("--upstream-pass=", 0) == 0) {
      upstream_pass = arg.substr(16);
    } else if (arg.rfind("--upstream-pass-file=", 0) == 0) {
      upstream_pass_file = arg.substr(21);
    } else if (arg.rfind("--server-id=", 0) == 0) {
      server_id = static_cast<uint32_t>(std::stoul(arg.substr(12)));
    } else if (arg.rfind("--downstream=", 0) == 0) {
      downstream_target = arg.substr(13);
    } else if (arg.rfind("--downstream-user=", 0) == 0) {
      downstream_user = arg.substr(18); downstream_user_set = true;
    } else if (arg.rfind("--downstream-pass=", 0) == 0) {
      downstream_pass = arg.substr(18); downstream_pass_set = true;
    } else if (arg.rfind("--downstream-pass-file=", 0) == 0) {
      downstream_pass_file = arg.substr(23);
    } else if (arg.rfind("--downstream-db=", 0) == 0) {
      downstream_db = arg.substr(16);
    } else if (arg.rfind("--upstream-db=", 0) == 0) {
      upstream_db = arg.substr(14);
    } else if (arg.rfind("--tables=", 0) == 0) {
      table_whitelist_str = arg.substr(9);
    } else if (arg.rfind("--port=", 0) == 0) {
      port = static_cast<uint16_t>(std::stoi(arg.substr(7)));
    } else if (arg.rfind("--bind=", 0) == 0) {
      bind_addr = arg.substr(7);
    } else if (arg.rfind("--socket=", 0) == 0) {
      socket_path = arg.substr(9);
    } else if (arg.rfind("--shm-ring=", 0) == 0) {
      shm_ring_name = arg.substr(11);
    } else if (arg.rfind("--telemetry-slots=", 0) == 0) {
      telemetry_slots = static_cast<uint32_t>(std::stoul(arg.substr(18)));
    } else if (arg.rfind("--watch-fs=", 0) == 0) {
      watch_fs_dir = arg.substr(11);
    } else if (arg.rfind("--root=", 0) == 0) {
      root_dir = arg.substr(7);
    } else if (arg.rfind("--mode=", 0) == 0) {
      mode = arg.substr(7);
      if (mode != "local" && mode != "write_behind" && mode != "write_through") {
        std::cerr << "[!] --mode=" << mode << ": local, write_behind or write_through\n";
        return 2;
      }
      write_behind = (mode == "write_behind");
      write_through = (mode == "write_through");
    } else if (arg.rfind("--sql-user=", 0) == 0) {
      sql_user = arg.substr(11);
    } else if (arg.rfind("--sql-pass=", 0) == 0) {
      sql_pass = arg.substr(11);
    } else if (arg.rfind("--sql-pass-file=", 0) == 0) {
      sql_pass_file = arg.substr(16);
    } else if (arg == "--trust-loopback") {
      trust_loopback = true;
    } else if (arg.rfind("--max-connections=", 0) == 0) {
      max_connections = std::stoll(arg.substr(18));
      if (max_connections < 0) { std::cerr << "[!] --max-connections must be 0 or more\n"; return 2; }
    } else if (arg.rfind("--query-memory-mb=", 0) == 0) {
      query_memory_mb = std::stoll(arg.substr(18));
      if (query_memory_mb < 0) { std::cerr << "[!] --query-memory-mb must be 0 or more\n"; return 2; }
    } else if (arg == "--log-bin") {
      log_bin = true;
    } else if (arg == "--skip-log-bin") {
      skip_log_bin = true;
    } else if (arg == "--listen-gate") {
      listen_gate = true;
    } else if (arg == "--benchmark") {
      run_benchmark = true;
    } else {
      // An option this engine does not know used to be ignored without a word.
      std::cerr << "[!] unknown option: " << arg << " (--help lists them)\n";
      return 2;
    }
  }
  if (!upstream_pass_file.empty() && !read_pass_file(upstream_pass_file, &upstream_pass)) {
    std::cerr << "[!] cannot read " << upstream_pass_file << "\n";
    return 1;
  }
  if (!downstream_pass_file.empty()) {
    if (!read_pass_file(downstream_pass_file, &downstream_pass)) {
      std::cerr << "[!] cannot read " << downstream_pass_file << "\n";
      return 1;
    }
    downstream_pass_set = true;
  }
  // write_behind / write_through deliver every write somewhere. Without a
  // downstream, write_through applied each write here and then failed it, and
  // write_behind queued them in a WAL nothing ever drained.
  if ((write_behind || write_through) && downstream_target.empty()) {
    std::cerr << "[!] --mode=" << mode << " needs --downstream=<host:port|socket|upstream>: where its writes go\n";
    return 2;
  }
  if (!downstream_target.empty() && !write_behind && !write_through) {
    std::cerr << "[!] --downstream is only used by --mode=write_behind or --mode=write_through\n";
    return 2;
  }
  if (!upstream_target.empty() && upstream_db.empty()) {
    std::cerr << "[!] --upstream needs --upstream-db=<the source database to mirror>\n";
    return 2;
  }
  if (upstream_db.empty()) upstream_db = "aios_db";
  if (downstream_target == "upstream" && upstream_target.empty()) {
    std::cerr << "[!] --downstream=upstream needs --upstream\n";
    return 2;
  }
  if (listen_gate && upstream_target.empty()) {
    std::cerr << "[!] --listen-gate waits for replication to catch up, so it needs --upstream\n";
    return 2;
  }
  // host:port, socket path, or database URI (tds://..., postgres://..., mysql://...).
  auto split_target = [](const std::string& t, std::string* host, uint16_t* prt) {
    if (t.find("://") != std::string::npos) {
      auto prof = DbConnectionProfile::from_uri(t);
      if (!prof.ok()) return false;
      *host = prof->host;
      *prt = prof->port;
      return true;
    }
    if (!t.empty() && t[0] == '/') { *host = t; *prt = 3306; return true; }
    size_t colon = t.rfind(':');
    if (colon == std::string::npos) { *host = t; *prt = 3306; return !t.empty(); }
    *host = t.substr(0, colon);
    int pv = std::atoi(t.c_str() + colon + 1);
    if (pv <= 0 || pv > 65535 || host->empty()) return false;
    *prt = static_cast<uint16_t>(pv);
    return true;
  };

  // A password on the command line is readable in `ps` by anyone on the box,
  // so --sql-pass-file is the one to use for a real deployment.
  if (!sql_pass_file.empty()) {
    std::ifstream pf(sql_pass_file);
    if (!pf) {
      std::cerr << "[!] cannot read " << sql_pass_file << "\n";
      return 1;
    }
    std::getline(pf, sql_pass);
    while (!sql_pass.empty() && (sql_pass.back() == '\n' || sql_pass.back() == '\r')) sql_pass.pop_back();
  }

  print_banner();

  std::cout << "  [Configuration Summary]\n"
            << "  • Root Directory       : " << root_dir << "\n"
            << "  • MySQL Wire Server    : " << bind_addr << ":" << port << " (5.7.44-aios-asql)\n"
            << "  • Local UNIX Socket    : " << (socket_path.empty() ? "(disabled)" : socket_path) << "\n"
            << "  • Zero-Copy SHM Ring   : " << shm_ring_name << "\n"
            << "  • Binary Telemetry     : " << telemetry_slots << " 64-byte slots in RAM (Zero Disk IO)\n"
            << "  • Sync Mode            : " << (write_behind ? "Pattern B (Write-Behind WAL)"
                                                   : write_through ? "Pattern A (Write-Through)"
                                                                   : "Local-Only (write-behind OFF)") << "\n";
  if (!downstream_target.empty()) {
    std::string proto = "MariaDB/MySQL (port 3306)";
    if (downstream_target.rfind("tds://", 0) == 0 || downstream_target.rfind("mssql://", 0) == 0) proto = "MS SQL Server (TDS 7.4+, port 1433)";
    else if (downstream_target.rfind("postgres://", 0) == 0 || downstream_target.rfind("postgresql://", 0) == 0 || downstream_target.rfind("pg://", 0) == 0) proto = "PostgreSQL (v3, port 5432)";
    std::cout << "  • Downstream           : " << (downstream_target == "upstream" ? upstream_target + " (the upstream)" : downstream_target)
              << " [" << proto << "]\n";
  }
  if (listen_gate)
    std::cout << "  • Listen Gate          : no listener until replication has caught up\n";

  if (!upstream_target.empty()) {
    std::cout << "  • CDC Upstream Target  : " << upstream_target << " (DB: " << upstream_db << ")\n";
    if (!table_whitelist_str.empty()) {
      std::cout << "  • Ingress Whitelist    : " << table_whitelist_str << "\n";
    }
  }
  std::cout << "-----------------------------------------------------------------------------------\n";

  // 1. Boot Kernel
  KernelConfig kcfg;
  kcfg.root = root_dir;
  auto k_res = Kernel::boot(kcfg);
  if (!k_res.ok()) {
    std::cerr << "[!] Failed to boot AIOS Kernel: " << k_res.error().message << "\n";
    return 1;
  }
  Kernel& kernel = **k_res;
  std::cout << "  ✓ AIOS Microsecond Kernel booted successfully.\n";

  // 2. Initialize Sidecars
  std::cout << "  ✓ Initializing 50-Year Legacy Elimination Sidecar Subsystems...\n";
  const std::string cap_key = load_capability_key(root_dir);
  if (cap_key.empty()) {
    std::cerr << "[!] No capability key: set AIOS_CAPABILITY_KEY or make " << root_dir << "/capability.key readable and writable\n";
    return 1;
  }
  // The key above lasts across restarts, so revocations are kept beside it.
  CapabilityGuard cap_guard(cap_key, root_dir + "/capability-state");
  BinaryTelemetryCore telemetry(telemetry_slots);
  EventFabric event_fabric;
  event_fabric.register_topic("cdc_events", 8);
  event_fabric.register_topic("orders", 4);
  event_fabric.register_topic("telemetry", 2);

  FsTopologyMirror fs_mirror;
  ModuleVault module_vault;
  ShmRingChannel shm_ring(shm_ring_name, 4 * 1024 * 1024, true);

  // What each sidecar subsystem is actually doing. They are constructed here,
  // but outside --benchmark nothing in the engine feeds them: no query is
  // recorded in the telemetry, nothing is published on the fabric, nothing is
  // written to the ring, and no token is checked. Saying "Active" was a claim
  // about work that never happened.
  std::cout << "    - Capability Guard       : key loaded; idle (the engine mints and checks no tokens)\n";
  std::cout << "    - Binary Telemetry Core  : " << telemetry_slots << " slots allocated; idle (no query is recorded into it)\n";
  std::cout << "    - Event Fabric           : " << event_fabric.total_topics() << " topics registered; idle (nothing publishes)\n";
  std::cout << "    - SHM Ring Channel       : /" << shm_ring_name << " created (4 MB); idle (the engine writes nothing to it)\n";
  (void)module_vault;

  // 3. Mirror a directory tree into the FS topology, for real: every entry
  // under it, with its size, mode and mtime. (This used to insert the one
  // directory node and report "Watching".) Rescanned in the main loop.
  struct FsScan { uint64_t files = 0, dirs = 0, bytes = 0, removed = 0; std::string error; };
  std::unordered_set<std::string> fs_seen;
  auto scan_fs = [&]() {
    FsScan r;
    std::unordered_set<std::string> now;
    auto put = [&](const std::filesystem::path& p, const std::filesystem::directory_entry* de) {
      struct stat sb{};
      if (::lstat(p.c_str(), &sb) != 0) return;
      FsMetadata meta{};
      meta.inode = sb.st_ino;
      meta.size_bytes = (uint64_t)sb.st_size;
      meta.mode = sb.st_mode;
      meta.uid = sb.st_uid;
      meta.gid = sb.st_gid;
#if defined(__APPLE__)
      meta.mtime_ns = (uint64_t)sb.st_mtimespec.tv_sec * 1000000000ull + (uint64_t)sb.st_mtimespec.tv_nsec;
#elif defined(_WIN32)
      meta.mtime_ns = (uint64_t)sb.st_mtime * 1000000000ull;
#else
      meta.mtime_ns = (uint64_t)sb.st_mtim.tv_sec * 1000000000ull + (uint64_t)sb.st_mtim.tv_nsec;
#endif
      meta.is_directory = S_ISDIR(sb.st_mode);
      meta.is_symlink = S_ISLNK(sb.st_mode);
      (void)de;
      fs_mirror.upsert_entry(p.string(), meta);
      now.insert(p.string());
      if (meta.is_directory) r.dirs++; else { r.files++; r.bytes += meta.size_bytes; }
    };
    std::error_code ec;
    const std::filesystem::path root_path(watch_fs_dir);
    if (!std::filesystem::is_directory(root_path, ec)) { r.error = watch_fs_dir + " is not a readable directory"; return r; }
    put(root_path, nullptr);
    std::filesystem::recursive_directory_iterator it(root_path, std::filesystem::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) put(it->path(), &*it);
    if (ec) r.error = ec.message();
    for (const auto& old : fs_seen) if (!now.count(old)) { fs_mirror.remove_entry(old); r.removed++; }
    fs_seen.swap(now);
    return r;
  };
  if (!watch_fs_dir.empty()) {
    auto r = scan_fs();
    if (!r.error.empty() && r.files + r.dirs == 0) {
      std::cerr << "[!] --watch-fs=" << watch_fs_dir << ": " << r.error << "\n";
      return 2;
    }
    std::cout << "    - FS Topology Mirror     : " << r.files << " file(s), " << r.dirs << " dir(s), " << r.bytes
              << " bytes under " << watch_fs_dir << " (rescanned every 60 s)" << (r.error.empty() ? "" : "; scan stopped early: " + r.error) << "\n";
  }

  // 4. Initialize ASQL Engine
  SqlDatabase db(kernel);
  auto db_res = db.init(upstream_db);
  if (!db_res.ok()) {
    std::cerr << "[!] Failed to init SqlDatabase: " << db_res.error().message << "\n";
    return 1;
  }
  // A binlog master: on when asked, and from then on whenever its index is there.
  if (!skip_log_bin && (log_bin || ::access((db.binlog_default_dir() + "/asql-bin.index").c_str(), F_OK) == 0)) {
    auto bl = db.enable_binlog();
    if (!bl.ok()) {
      std::cerr << "[!] " << bl.error().message << "\n";
      return 1;
    }
    std::cout << "  ✓ Binlog master: " << db.binlog_master_position() << " in " << db.binlog_default_dir() << "\n";
  }
  if (!downstream_target.empty()) {
    DbConnectionProfile prof;
    prof.id = "downstream";
    prof.name = "Downstream";
    prof.is_local_embedded = false;
    const std::string t = downstream_target == "upstream" ? upstream_target : downstream_target;
    if (t.find("://") != std::string::npos) {
      auto p_res = DbConnectionProfile::from_uri(t);
      if (!p_res.ok()) {
        std::cerr << "[!] --downstream=" << t << ": " << p_res.error().message << "\n";
        return 2;
      }
      prof = *p_res;
    } else if (!split_target(t, &prof.host, &prof.port)) {
      std::cerr << "[!] --downstream=" << downstream_target << ": host:port, socket path, or database URI\n";
      return 2;
    }
    if (downstream_user_set) prof.user = downstream_user;
    else if (prof.user.empty()) prof.user = upstream_user;
    if (downstream_pass_set) prof.password = downstream_pass;
    else if (prof.password.empty()) prof.password = upstream_pass;
    if (!downstream_db.empty()) prof.database = downstream_db;
    else if (prof.database.empty()) prof.database = upstream_db;
    db.set_downstream_target(prof);
  }
  if (write_behind) {
    db.set_sync_mode(WriteSyncMode::WriteBehind);
    db.init_write_behind();
    // The drain: what the WAL holds goes to the downstream in order, and is
    // acknowledged in the WAL only once the downstream has taken it.
    db.start_wal_drain_worker(20);
  } else if (write_through) {
    db.set_sync_mode(WriteSyncMode::WriteThrough);
  } else {
    db.set_sync_mode(WriteSyncMode::LocalOnly);
  }
  std::cout << "  ✓ In-Memory Multi-Tenant ASQL Engine online.\n";

  // 5. Optional Startup Benchmark
  if (run_benchmark) {
    std::cout << "\n  [Executing Self-Diagnostic Microsecond Benchmark]\n";
    // Benchmark SHM Ring
    auto t0 = std::chrono::high_resolution_clock::now();
    for (uint32_t i = 0; i < 50000; ++i) {
      uint64_t msg = 0xDEADBEEF00000000ULL | i;
      shm_ring.push_frame(1, 1, &msg, sizeof(msg));
      shm_ring.pop_frame([](const ShmFrameHeader&, const void*, size_t){});
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double shm_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::cout << "    • SHM Ring Channel       : " << std::fixed << std::setprecision(2) 
              << (50000.0 / shm_ms) * 1000.0 << " ops/sec (Zero Copy)\n";

    // Benchmark Telemetry
    t0 = std::chrono::high_resolution_clock::now();
    for (uint64_t i = 1; i <= 50000; ++i) {
      telemetry.record(100, i, 1, 1, 200, 150, 0, "BENCHMARK_PROBE");
    }
    t1 = std::chrono::high_resolution_clock::now();
    double tel_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::cout << "    • Binary Telemetry Core  : " << std::fixed << std::setprecision(2) 
              << (50000.0 / tel_ms) * 1000.0 << " events/sec (0 disk IO)\n";

    // Benchmark Event Fabric
    t0 = std::chrono::high_resolution_clock::now();
    const char* payload = "EVENT_PAYLOAD";
    for (uint64_t i = 0; i < 50000; ++i) {
      event_fabric.publish("cdc_events", payload, 13, static_cast<uint32_t>(i));
    }
    t1 = std::chrono::high_resolution_clock::now();
    double ef_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::cout << "    • Event Fabric Pub/Sub   : " << std::fixed << std::setprecision(2) 
              << (50000.0 / ef_ms) * 1000.0 << " msgs/sec\n\n";
  }

  // 6. Upstream CDC Connection - through the engine's own replica start
  // (start_binlog_sync), which resumes from the saved binlog position and
  // each seeded table's snapshot point, and derives a replica id from this
  // root's identity. Building a BinlogConsumer here skipped all three: every
  // restart began at the master's current offset, and every aios-engine
  // registered as server 8901 and knocked the others off.
  if (!upstream_target.empty()) {
    std::cout << "  [CDC Ingress Connection]\n";
    BinlogConsumerConfig bcfg;
    bcfg.database = upstream_db;
    bcfg.user = upstream_user;
    bcfg.password = upstream_pass;
    if (server_id) bcfg.server_id = server_id;
    if (!split_target(upstream_target, &bcfg.host, &bcfg.port)) {
      std::cerr << "[!] --upstream=" << upstream_target << ": host:port or a socket path\n";
      return 2;
    }
    std::vector<std::string> want;
    if (!table_whitelist_str.empty()) {
      std::stringstream ss(table_whitelist_str);
      std::string tbl;
      while (std::getline(ss, tbl, ',')) {
        if (!tbl.empty()) { bcfg.table_whitelist.insert(tbl); want.push_back(tbl); }
      }
    }
    // A whitelisted table that was never copied in stays invisible however
    // long the stream runs (it carries row deltas): copy the missing ones
    // first, each with its binlog point, as compute serve does.
    if (!want.empty()) {
      db.set_mirror_source(SqlDatabase::MirrorSource{bcfg.host, bcfg.port, bcfg.user, bcfg.password, upstream_db, want});
      std::vector<std::string> seed_log;
      auto sd = db.seed_from_source(bcfg.host, bcfg.port, bcfg.user, bcfg.password, upstream_db, want, &seed_log);
      for (const auto& line : seed_log) std::cout << "    seed: " << line << "\n";
      if (!sd.ok()) std::cerr << "  [!] seed from the upstream failed: " << sd.error().message << "\n";
      else std::cout << "  ✓ Seeded " << *sd << " table(s) from the upstream\n";
    }
    auto cdc_res = db.start_binlog_sync(bcfg);
    if (!cdc_res.ok()) {
      std::cerr << "  [!] Upstream CDC did not start: " << cdc_res.error().message << "\n";
    } else {
      // "Attached" only once it is: start() only launches the worker thread.
      bool attached = false;
      for (int w = 0; w < 50 && !g_shutdown.load(); ++w) {
        if (db.binlog_sync_stats().is_connected) { attached = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      auto st = db.binlog_sync_stats();
      if (attached)
        std::cout << "  ✓ In-process CDC replication attached to " << upstream_target << " (" << st.current_binlog_file << ":"
                  << st.current_binlog_pos << ")\n";
      else
        std::cerr << "  [!] CDC not attached to " << upstream_target << " yet"
                  << (st.last_error.empty() ? std::string() : ": " + st.last_error) << " (retrying in the background)\n";
    }
    if (listen_gate) {
      // Caught up = applied up to where the master's binlog stood when the
      // gate began (SHOW MASTER STATUS). Seconds_Behind_Master alone never
      // reaches 0 on an idle master: it is measured from the last event's
      // timestamp, which an idle master does not move.
      std::string gate_file;
      uint64_t gate_pos = 0;
      {
        MariaWireSession ms(bcfg.host, bcfg.port);
        if (ms.connect(bcfg.user, bcfg.password, "").ok()) {
          auto r = ms.query("SHOW MASTER STATUS");
          if (r.ok() && r->ok && !r->rows.empty() && r->rows[0].size() >= 2) {
            gate_file = r->rows[0][0].bytes;
            gate_pos = std::strtoull(r->rows[0][1].bytes.c_str(), nullptr, 10);
          }
        }
      }
      std::cout << "  … listen gate: waiting for replication to reach "
                << (gate_file.empty() ? std::string("the master (position unknown: lag 0)") : gate_file + ":" + std::to_string(gate_pos)) << "\n";
      auto gate_t0 = std::chrono::steady_clock::now();
      auto last_say = gate_t0;
      while (!g_shutdown.load()) {
        auto st = db.binlog_sync_stats();
        const bool reached = !gate_file.empty() &&
            (st.current_binlog_file > gate_file || (st.current_binlog_file == gate_file && st.current_binlog_pos >= gate_pos));
        if (st.is_connected && (reached || (gate_file.empty() && st.seconds_behind_master == 0))) break;
        auto now = std::chrono::steady_clock::now();
        if (now - last_say >= std::chrono::seconds(10)) {
          last_say = now;
          std::cout << "    still gated: " << (st.is_connected ? "connected, at " + st.current_binlog_file + ":" + std::to_string(st.current_binlog_pos)
                                                              : "not connected" + (st.last_error.empty() ? std::string() : " (" + st.last_error + ")"))
                    << "\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (g_shutdown.load()) { db.stop_binlog_sync(); return 0; }
      std::cout << "  ✓ Listen gate open: replication caught up after "
                << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - gate_t0).count() << " ms\n";
    }
  }

  // 7. Start MySQL Wire Gateway Server
  MysqlWireServer server(kernel, db, port, bind_addr);
  if (!sql_user.empty()) server.add_account(sql_user, sql_pass);
  server.set_trust_loopback(trust_loopback);
  // Named users (CREATE USER / GRANT / REVOKE, mysql.user) and capability
  // credentials: the guard built above, and the users beside its key. Never
  // handed to the server before, so every CREATE USER and GRANT was refused
  // with "capabilities are not enabled on this server".
  server.set_capability_guard(&cap_guard);
  AsqlUserStore cap_users(root_dir + "/capability-users");
  server.set_capability_users(&cap_users);
  if (max_connections >= 0) server.set_max_connections((size_t)max_connections);
  if (query_memory_mb >= 0) db.set_query_memory_budget((size_t)query_memory_mb << 20);
  if (sql_user.empty() && !trust_loopback) {
    std::cerr << "[!] Refusing to open " << bind_addr << ":" << port
              << " with no account: pass --sql-user= and --sql-pass-file=, or"
                 " --trust-loopback for a local-only engine.\n";
    return 1;
  }
  if (sql_user.empty()) {
    std::cout << "  ! No SQL account set: only loopback clients will be let in.\n";
  }
  if (!socket_path.empty()) server.set_unix_socket(socket_path);
  auto srv_res = server.start();
  if (!srv_res.ok()) {
    std::cerr << "[!] Failed to bind MySQL Wire Server to " << bind_addr << ":" << port 
              << " (" << srv_res.error().message << ")\n";
    return 1;
  }
  server.start_background();

  std::cout << "  ✓ MySQL Native Wire Gateway listening on " << bind_addr << ":" << server.port() << "\n";
  if (!server.unix_socket().empty())
    std::cout << "  ✓ MySQL Native Wire Gateway listening on unix socket " << server.unix_socket() << "\n";
  std::cout << "===================================================================================\n";
  std::cout << "  AIOS ENGINE IS FULLY ONLINE & OPERATIONAL. READY FOR LIVE WORKLOADS.\n";
  std::cout << "===================================================================================\n\n";

  // Telemetry rolling loop and auto-vacuum
  auto last_stats_print = std::chrono::steady_clock::now();
  uint64_t loop_count = 0;

  while (!g_shutdown.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    if (g_shutdown.load()) break;

    loop_count++;
    auto now = std::chrono::steady_clock::now();

    // Periodic telemetry aggregation & heartbeat
    if (now - last_stats_print >= std::chrono::seconds(60)) {
      last_stats_print = now;
      if (!watch_fs_dir.empty()) {
        auto r = scan_fs();
        std::cout << "[aios-fs] " << watch_fs_dir << ": " << r.files << " file(s), " << r.dirs << " dir(s), " << r.bytes << " bytes"
                  << (r.removed ? ", " + std::to_string(r.removed) + " gone since the last scan" : std::string())
                  << (r.error.empty() ? "" : " (" + r.error + ")") << "\n";
      }
      auto stats = telemetry.compute_stats(1);
      if (stats.count > 0) {
        std::cout << "[aios-telemetry] count=" << stats.count 
                  << " p50=" << stats.p50_duration_us << "us p95=" << stats.p95_duration_us << "us p99=" << stats.p99_duration_us << "us"
                  << " errors=" << stats.error_count << "\n";
      }
      if (!upstream_target.empty()) {
        auto cstats = db.binlog_sync_stats();
        if (cstats.is_connected) {
          std::cout << "[aios-cdc] applied=" << cstats.total_events_applied
                    << " (ins=" << cstats.total_inserts << " upd=" << cstats.total_updates << " del=" << cstats.total_deletes << ")"
                    << " lag=" << cstats.seconds_behind_master << "s\n";
        } else {
          std::cout << "[aios-cdc] not connected" << (cstats.last_error.empty() ? std::string() : ": " + cstats.last_error) << "\n";
        }
      }
      // A write-behind WAL that is not draining (downstream down, or a
      // statement it refuses, which holds the ones behind it) says so.
      if (write_behind) {
        auto wb = db.write_back_stats();
        if (wb.pending_mutations > 0)
          std::cout << "[aios-write-behind] " << wb.pending_mutations << " statement(s) not yet delivered downstream"
                    << (wb.last_error.empty() ? std::string() : "; last error: " + wb.last_error) << "\n";
      }
    }
  }

  std::cout << "\n[aios-engine] Initiating graceful shutdown...\n";
  if (!upstream_target.empty()) {
    db.stop_binlog_sync();   // saves the binlog position the next start resumes from
    std::cout << "  ✓ CDC stream stopped.\n";
  }
  server.stop();
  std::cout << "  ✓ MySQL Wire Server stopped.\n";
  db.stop_wal_drain_worker();
  if (auto fw = db.flush_write_behind(); !fw.ok())   // no-op unless write-behind was turned on
    std::cerr << "  [!] write-behind WAL not fully delivered (kept for the next start): " << fw.error().message << "\n";
  db.flush_now();
  std::cout << "  ✓ Store checkpointed to disk.\n";
  std::cout << "[aios-engine] Shutdown complete. Goodbye.\n";

  return 0;
}
