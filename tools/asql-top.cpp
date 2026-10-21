// AIOS ASQL Real-Time Live Performance & Telemetry Monitor
// Connects to local or remote ASQL MySQL-wire port (:3307) and streams live
// metrics, query throughput, latency percentiles, data skipping efficiency,
// memory footprint, and real-time execution logs.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <map>
#include <algorithm>
#include <iomanip>
#include <strings.h>
#include <sstream>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <termios.h>

#include "aios/userland/mysql_wire.hpp"
#include "asql-conf.hpp"

using namespace aios;
using namespace aios::userland;

static volatile bool g_running = true;

static void handle_sigint(int) {
  g_running = false;
}


// --- recording -------------------------------------------------------------

static void json_str(std::string& o, const std::string& v) {
  o += '"';
  for (unsigned char c : v) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += (char)c;
    }
  }
  o += '"';
}

static void json_cell(std::string& o, const MariaValue& v) {
  if (v.is_null) o += "null"; else json_str(o, v.bytes);
}

// One result row as {"column": "value", ...}.
static void json_row(std::string& o, const MariaResult& r, const std::vector<MariaValue>& row) {
  o += '{';
  for (size_t i = 0; i < r.columns.size() && i < row.size(); ++i) {
    if (i) o += ',';
    json_str(o, r.columns[i].name);
    o += ':';
    json_cell(o, row[i]);
  }
  o += '}';
}

static int col_index(const MariaResult& r, const char* name) {
  for (size_t i = 0; i < r.columns.size(); ++i)
    if (strcasecmp(r.columns[i].name.c_str(), name) == 0) return (int)i;
  return -1;
}

// The monitor's own polling statements are not what it is there to record.
static bool is_monitor_statement(const std::string& q) {
  static const char* mine[] = {"SHOW ENGINE ASQL STATUS", "SHOW GLOBAL STATUS", "SHOW STATUS",
                               "SHOW ASQL QUERIES", "SHOW FULL PROCESSLIST"};
  for (const char* m : mine) if (q.compare(0, std::strlen(m), m) == 0) return true;
  return false;
}

static std::string iso_now_ms() {
  auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
  char b[40], z[8];
  std::tm tmv; localtime_r(&t, &tmv);
  std::strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%S", &tmv);
  std::strftime(z, sizeof(z), "%z", &tmv);
  char o[64]; std::snprintf(o, sizeof(o), "%s.%03d%s", b, (int)ms, z);
  return o;
}

static void print_usage(const char* prog) {
  std::cout << "Usage: " << prog << " [options]\n\n"
            << "Connection defaults come from /etc/aios/asql.conf (or $ASQL_CONF, or\n"
            << "--conf FILE), the file the engine on this machine was started from, so\n"
            << "on an installed host no flag is needed.\n\n"
            << "Options:\n"
            << "  -S, --socket <path>     ASQL unix socket (conf: MYSQL_SOCKET)\n"
            << "  -h, --host <host>       ASQL host (default: 127.0.0.1; a /path is a socket)\n"
            << "  -P, --port <port>       ASQL MySQL-wire port (conf: MYSQL_PORT, else 3307)\n"
            << "  -u, --user <user>       MySQL user (conf: MYSQL_USER, else root)\n"
            << "  -p, --password <pass>   MySQL password (conf: first line of MYSQL_PASS_FILE)\n"
            << "  -d, --database <db>     Initial database (conf: MYSQL_DB or CDC_DB)\n"
            << "  --conf <file>           Read defaults from this conf instead\n"
            << "  --no-conf               Ignore every conf file\n"
            << "  -i, --interval <ms>     Refresh interval in milliseconds (default: 1000)\n"
            << "  -1, --once              Print single snapshot and exit\n"
            << "  --record <file>         Append every sample to <file> as JSON lines (one line per\n"
            << "                          sample): all status counters, the sessions running right now\n"
            << "                          (SHOW FULL PROCESSLIST) and every statement that finished since\n"
            << "                          the last sample, with its full text. Nothing is truncated here;\n"
            << "                          the engine keeps the last 1024 statements, so use a short\n"
            << "                          --interval (e.g. -i 100) for a burst; a sample that missed some\n"
            << "                          says how many (\"missed_statements\"). Off unless given.\n"
            << "  --no-screen             With --record: draw nothing, just record (for nohup / no tty)\n"
            << "  --help                  Show this help message\n";
}

static std::string get_val(const std::map<std::string, std::string>& m, const std::string& key, const std::string& dflt = "0") {
  auto it = m.find(key);
  if (it == m.end() || it->second.empty()) return dflt;
  return it->second;
}

int main(int argc, char** argv) {
  std::string conf_path;
  bool use_conf = true;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--conf" && i + 1 < argc) conf_path = argv[++i];
    else if (arg == "--no-conf") use_conf = false;
    else if (arg == "--help") { print_usage(argv[0]); return 0; }
  }
  asql_conf::Defaults conf;
  if (use_conf) conf = asql_conf::load(conf_path);

  std::string host = !conf.socket.empty() ? conf.socket : (!conf.host.empty() ? conf.host : "127.0.0.1");
  uint16_t port = conf.port ? conf.port : 3307;
  std::string user = !conf.user.empty() ? conf.user : "root";
  std::string pass = conf.pass;
  std::string db = conf.db;
  int interval_ms = 1000;
  bool once_mode = false;
  std::string record_path;
  bool no_screen = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if ((arg == "-h" || arg == "--host") && i + 1 < argc) {
      host = argv[++i];
    } else if ((arg == "-S" || arg == "--socket") && i + 1 < argc) {
      host = argv[++i];
    } else if (arg == "--conf" && i + 1 < argc) {
      ++i;
    } else if (arg == "--no-conf") {
    } else if ((arg == "-P" || arg == "--port") && i + 1 < argc) {
      port = static_cast<uint16_t>(std::atoi(argv[++i]));
      if (!host.empty() && host[0] == '/') host = !conf.host.empty() ? conf.host : "127.0.0.1";
    } else if ((arg == "-u" || arg == "--user") && i + 1 < argc) {
      user = argv[++i];
    } else if ((arg == "-p" || arg == "--password") && i + 1 < argc) {
      pass = argv[++i];
    } else if ((arg == "-d" || arg == "--database") && i + 1 < argc) {
      db = argv[++i];
    } else if ((arg == "-i" || arg == "--interval") && i + 1 < argc) {
      interval_ms = std::atoi(argv[++i]);
      if (interval_ms < 50) interval_ms = 50;
    } else if (arg == "--record" && i + 1 < argc) {
      record_path = argv[++i];
    } else if (arg == "--no-screen") {
      no_screen = true;
    } else if (arg == "-1" || arg == "--once") {
      once_mode = true;
    } else if (arg == "--help") {
      print_usage(argv[0]);
      return 0;
    }
  }

  const std::string where = (!host.empty() && host[0] == '/') ? host : host + ":" + std::to_string(port);

  if (no_screen && record_path.empty()) {
    std::cerr << "--no-screen needs --record FILE (otherwise there is nothing to show)\n";
    return 2;
  }
  std::ofstream rec;
  if (!record_path.empty()) {
    rec.open(record_path, std::ios::app | std::ios::binary);
    if (!rec) { std::cerr << "cannot open " << record_path << " for writing\n"; return 1; }
    std::cerr << "recording to " << record_path << " every " << interval_ms << " ms\n";
  }
  uint64_t last_seq = 0;
  bool first_sample = true;

  signal(SIGINT, handle_sigint);
  signal(SIGTERM, handle_sigint);

  // Setup terminal alternate buffer if interactive
  if (!once_mode && !no_screen && isatty(fileno(stdout))) {
    std::cout << "\033[?1049h\033[?25l\033[H\033[2J" << std::flush;
  }

  std::unique_ptr<MariaWireSession> session;
  uint64_t last_query_count = 0;
  auto last_check_time = std::chrono::steady_clock::now();

  while (g_running) {
    if (!session || !session->connected()) {
      session = std::make_unique<MariaWireSession>(host, port);
      auto c_res = session->connect(user, pass, db);
      if (!c_res.ok()) {
        if (once_mode) {
          std::cerr << "\033[1;31m[!] Connection failed to " << where << " as " << user
                    << " (" << c_res.error().message << ")\033[0m\n";
          return 1;
        }
        std::cout << "\033[H\033[2J";
        std::cout << "\033[1;31m================================================================================\033[K\033[0m\n";
        std::cout << "\033[1;31m  ASQL LIVE MONITOR: DISCONNECTED FROM " << where << "\033[K\033[0m\n";
        std::cout << "\033[1;31m  Error: " << c_res.error().message << "\033[K\033[0m\n";
        std::cout << "\033[1;33m  Retrying connection in " << (interval_ms / 1000.0) << "s... (Press Ctrl+C to quit)\033[K\033[0m\n";
        std::cout << "\033[1;31m================================================================================\033[K\033[0m\n";
        std::cout << "\033[J" << std::flush;
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        continue;
      }
    }

    auto now = std::chrono::steady_clock::now();
    double dt_sec = std::chrono::duration<double>(now - last_check_time).count();
    if (dt_sec <= 0.0) dt_sec = 0.001;
    last_check_time = now;

    // Fetch engine status report
    std::string report_text;
    auto st_res = session->query("SHOW ENGINE ASQL STATUS;");
    if (st_res.ok() && st_res->ok) {
      if (st_res->has_result_set && !st_res->rows.empty() && st_res->rows[0].size() >= 3) {
        report_text = st_res->rows[0][2].bytes;
      }
    } else if (!st_res.ok()) {
      session->close();
      session.reset();
      continue;
    }

    // Fetch key status metrics
    std::map<std::string, std::string> status_vars;
    auto g_res = session->query("SHOW GLOBAL STATUS;");
    if (g_res.ok() && g_res->ok && g_res->has_result_set) {
      for (const auto& row : g_res->rows) {
        if (row.size() >= 2) {
          status_vars[row[0].bytes] = row[1].bytes;
        }
      }
    } else if (g_res.ok() && !g_res->ok) {
      auto s_res = session->query("SHOW STATUS;");
      if (s_res.ok() && s_res->ok && s_res->has_result_set) {
        for (const auto& row : s_res->rows) {
          if (row.size() >= 2) {
            status_vars[row[0].bytes] = row[1].bytes;
          }
        }
      }
    } else if (!g_res.ok()) {
      session->close();
      session.reset();
      continue;
    }

    uint64_t cur_queries = 0;
    if (status_vars.count("Queries")) {
      cur_queries = static_cast<uint64_t>(std::strtoull(status_vars["Queries"].c_str(), nullptr, 10));
    }
    double instant_qps = 0.0;
    if (last_query_count > 0 && cur_queries >= last_query_count) {
      instant_qps = (cur_queries - last_query_count) / dt_sec;
    }
    last_query_count = cur_queries;

    if (rec.is_open()) {
      std::string line;
      line += "{\"time\":"; json_str(line, iso_now_ms());
      line += ",\"interval_s\":" + std::to_string(dt_sec);
      line += ",\"instant_qps\":" + std::to_string(instant_qps);
      line += ",\"first_sample\":"; line += first_sample ? "true" : "false";
      line += ",\"status\":{";
      bool firstv = true;
      for (const auto& kv : status_vars) {
        if (!firstv) line += ',';
        firstv = false;
        json_str(line, kv.first); line += ':'; json_str(line, kv.second);
      }
      line += "}";

      // Sessions executing right now: the thing a burst is made of.
      line += ",\"processlist\":[";
      auto pl = session->query("SHOW FULL PROCESSLIST");
      if (pl.ok() && pl->ok && pl->has_result_set) {
        int c_info = col_index(*pl, "Info");
        bool firstp = true;
        for (const auto& row : pl->rows) {
          if (c_info >= 0 && (size_t)c_info < row.size() && is_monitor_statement(row[c_info].bytes)) continue;
          if (!firstp) line += ',';
          firstp = false;
          json_row(line, *pl, row);
        }
      }
      line += "]";

      // Every statement that finished since the last sample, oldest first, full text.
      std::string stmts;
      uint64_t missed = 0, nstmts = 0, skipped_own = 0, max_seq = last_seq;
      auto ql = session->query("SHOW ASQL QUERIES SINCE " + std::to_string(last_seq));
      if (ql.ok() && ql->ok && ql->has_result_set) {
        int c_id = col_index(*ql, "id");
        if (c_id < 0) c_id = col_index(*ql, "seq");
        int c_q = col_index(*ql, "query");
        std::vector<const std::vector<MariaValue>*> rows;
        for (const auto& row : ql->rows) rows.push_back(&row);
        std::sort(rows.begin(), rows.end(), [&](auto a, auto b) {
          return std::strtoull((*a)[c_id].bytes.c_str(), nullptr, 10) < std::strtoull((*b)[c_id].bytes.c_str(), nullptr, 10);
        });
        bool firsts = true;
        for (auto* rp : rows) {
          uint64_t id = std::strtoull((*rp)[c_id].bytes.c_str(), nullptr, 10);
          if (firsts && !first_sample && last_seq > 0 && id > last_seq + 1) missed = id - last_seq - 1;
          firsts = false;
          if (id > max_seq) max_seq = id;
          if (c_q >= 0 && is_monitor_statement((*rp)[c_q].bytes)) { ++skipped_own; continue; }
          if (nstmts++) stmts += ',';
          json_row(stmts, *ql, *rp);
        }
      } else if (!ql.ok()) {
        session->close();
        session.reset();
        continue;
      }
      last_seq = max_seq;
      line += ",\"missed_statements\":" + std::to_string(missed);
      line += ",\"monitor_statements_skipped\":" + std::to_string(skipped_own);
      line += ",\"statements\":[" + stmts + "]}\n";
      rec.write(line.data(), (std::streamsize)line.size());
      rec.flush();
      first_sample = false;
    }
    if (no_screen) {
      if (once_mode) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
      continue;
    }

    // Position cursor at top-left of screen
    if (!once_mode) {
      std::cout << "\033[H";
    }

    std::time_t t = std::time(nullptr);
    char time_buf[64];
    std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));

    std::string active_db = get_val(status_vars, "Asql_active_database", db);
    std::string uptime_s = get_val(status_vars, "Uptime", "0");
    std::string global_qps = get_val(status_vars, "Asql_qps", "0.0");
    std::string tot_queries = get_val(status_vars, "Queries", "0");
    std::string sel_cnt = get_val(status_vars, "Com_select", "0");
    std::string ins_cnt = get_val(status_vars, "Com_insert", "0");
    std::string upd_cnt = get_val(status_vars, "Com_update", "0");
    std::string del_cnt = get_val(status_vars, "Com_delete", "0");
    std::string avg_lat = get_val(status_vars, "Asql_avg_latency_us", "0");
    std::string last_lat = get_val(status_vars, "Asql_last_latency_us", "0");
    std::string slow_q = get_val(status_vars, "Slow_queries", "0");
    std::string err_cnt = get_val(status_vars, "Asql_errors", "0");

    std::string cache_hits = get_val(status_vars, "Asql_cache_hits", "0");
    std::string cache_rate = get_val(status_vars, "Asql_cache_hit_rate", "0.0%");
    std::string par_scans = get_val(status_vars, "Asql_parallel_scans", "0");
    std::string rad_joins = get_val(status_vars, "Asql_radix_joins", "0");
    std::string zm_pruned = get_val(status_vars, "Asql_zone_map_pruned", "0");
    std::string bf_pruned = get_val(status_vars, "Asql_bloom_filter_pruned", "0");

    std::string tot_tables = get_val(status_vars, "Asql_total_tables", "0");
    std::string tot_rows = get_val(status_vars, "Asql_total_rows", "0");
    size_t mem_bytes = static_cast<size_t>(std::strtoull(get_val(status_vars, "Asql_memory_bytes", "0").c_str(), nullptr, 10));
    std::string delta_rows = get_val(status_vars, "Asql_delta_buffer_rows", "0");

    std::string host_cpu_pct = get_val(status_vars, "Asql_host_cpu_pct", "0");
    std::string host_cpu_cores = get_val(status_vars, "Asql_host_cpu_cores", "0");
    std::string host_load_1m = get_val(status_vars, "Asql_host_load_1m", "0");
    std::string proc_cpu_pct = get_val(status_vars, "Asql_process_cpu_pct", "0");
    uint64_t host_mem_total = std::strtoull(get_val(status_vars, "Asql_host_mem_total_bytes", "0").c_str(), nullptr, 10);
    uint64_t host_mem_avail = std::strtoull(get_val(status_vars, "Asql_host_mem_available_bytes", "0").c_str(), nullptr, 10);
    uint64_t rss_bytes = std::strtoull(get_val(status_vars, "Asql_rss_bytes", "0").c_str(), nullptr, 10);
    uint64_t host_mem_used = host_mem_total > host_mem_avail ? host_mem_total - host_mem_avail : 0;

    std::ostringstream out;
    out << "\033[1;36m================================================================================\033[K\033[0m\n";
    out << "\033[1;37;44m  AIOS ASQL ENGINE LIVE DASHBOARD                                              \033[K\033[0m\n";
    out << "\033[1;36m================================================================================\033[K\033[0m\n";
    out << "  \033[1;32m● ONLINE\033[0m | Target: \033[1;33m" << where << "\033[0m | User: \033[1;37m" << user
        << "\033[0m | Time: \033[1;37m" << time_buf << "\033[K\033[0m\n";
    out << "  Uptime: \033[1;37m" << uptime_s << "s\033[0m | Active DB: \033[1;37m" << active_db << "\033[K\033[0m\n";
    out << "\033[1;36m--------------------------------------------------------------------------------\033[K\033[0m\n";
    out << "  \033[1;33mSYSTEM / HOST:\033[0m\033[K\n";
    out << "    Host CPU          : \033[1;" << (std::atof(host_cpu_pct.c_str()) > 80.0 ? "31" : "32") << "m"
        << host_cpu_pct << "%\033[0m (" << host_cpu_cores << " cores, load " << host_load_1m
        << ") | ASQL process: \033[1;37m" << proc_cpu_pct << "%\033[K\033[0m\n";
    out << "    Host RAM          : \033[1;37m" << (host_mem_used / (1024 * 1024)) << " MB used / "
        << (host_mem_total / (1024 * 1024)) << " MB total\033[0m | ASQL RSS: \033[1;37m"
        << (rss_bytes / (1024 * 1024)) << " MB\033[K\033[0m\n";
    out << "\033[1;36m--------------------------------------------------------------------------------\033[K\033[0m\n";
    out << "  \033[1;33mTHROUGHPUT & VELOCITY:\033[0m\033[K\n";
    out << "    Live QPS          : \033[1;32m" << std::fixed << std::setprecision(1) << instant_qps << " q/s\033[0m (Global Avg: " << global_qps << " q/s)\033[K\n";
    out << "    Total Queries     : \033[1;37m" << tot_queries << "\033[0m [Sel: " << sel_cnt
        << " | Ins: " << ins_cnt << " | Upd: " << upd_cnt
        << " | Del: " << del_cnt << "]\033[K\n";
    out << "    Avg Latency       : \033[1;32m" << (std::atof(avg_lat.c_str()) / 1000.0) << " ms\033[0m ("
        << avg_lat << " us)\033[K\n";
    out << "    Last Query Latency: \033[1;37m" << (std::atof(last_lat.c_str()) / 1000.0) << " ms\033[K\033[0m\n";
    out << "    Slow Queries (>50ms): \033[1;" << (slow_q == "0" ? "32" : "31") << "m"
        << slow_q << "\033[0m | Errors: \033[1;" << (err_cnt == "0" ? "32" : "31") << "m"
        << err_cnt << "\033[K\033[0m\n";
    out << "\033[1;36m--------------------------------------------------------------------------------\033[K\033[0m\n";
    out << "  \033[1;33mACCELERATION & DATA SKIPPING:\033[0m\033[K\n";
    out << "    Query Cache Hits  : \033[1;32m" << cache_hits << "\033[0m (Hit Rate: \033[1;32m" << cache_rate << "\033[0m)\033[K\n";
    out << "    Parallel Morsels  : \033[1;37m" << par_scans << "\033[0m | Radix Joins: \033[1;37m" << rad_joins << "\033[K\033[0m\n";
    out << "    Zone Map Pruned   : \033[1;32m" << zm_pruned << " chunks\033[0m | Bloom Pruned: \033[1;32m" << bf_pruned << " chunks\033[K\033[0m\n";
    out << "\033[1;36m--------------------------------------------------------------------------------\033[K\033[0m\n";
    out << "  \033[1;33mSTORAGE & IN-MEMORY FOOTPRINT:\033[0m\033[K\n";
    out << "    Tables Loaded     : \033[1;37m" << tot_tables << " tables\033[0m | Total Rows: \033[1;37m" << tot_rows << " rows\033[K\033[0m\n";
    out << "    RAM ColumnStore   : \033[1;32m" << (mem_bytes / 1024) << " KB (" << (mem_bytes / (1024 * 1024)) << " MB)\033[K\033[0m\n";
    out << "    Pending Delta     : \033[1;37m" << delta_rows << " rows\033[K\033[0m\n";

    if (!report_text.empty()) {
      size_t log_pos = report_text.find("RECENT QUERY LOG");
      if (log_pos != std::string::npos) {
        out << "\033[1;36m--------------------------------------------------------------------------------\033[K\033[0m\n";
        out << "  \033[1;33mLIVE STATEMENT ACTIVITY STREAM:\033[0m\033[K\n";
        std::istringstream iss(report_text.substr(log_pos));
        std::string line;
        int count = 0;
        while (std::getline(iss, line)) {
          if (line.find("==") != std::string::npos || line.find("RECENT QUERY LOG") != std::string::npos) continue;
          if (line.empty()) continue;
          if (line.find("[ERR") != std::string::npos) {
            out << "  \033[1;31m" << line << "\033[K\033[0m\n";
          } else if (line.find("[OK ") != std::string::npos) {
            out << "  \033[0;32m" << line << "\033[K\033[0m\n";
          } else {
            out << "  " << line << "\033[K\n";
          }
          if (++count >= 10) break;
        }
      }
    }

    out << "\033[1;36m================================================================================\033[K\033[0m\n";
    if (!once_mode) {
      out << "  \033[2m[Refreshing every " << interval_ms << "ms. Press Ctrl+C to stop]\033[K\033[0m\n";
      out << "\033[J"; // Erase to bottom of screen
    }

    std::cout << out.str() << std::flush;

    if (once_mode) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
  }

  if (!once_mode && !no_screen && isatty(fileno(stdout))) {
    std::cout << "\033[?25h\033[?1049l" << std::flush;
  }

  if (!no_screen) std::cout << "\n\033[1;32mASQL Monitor stopped.\033[0m\n";
  if (rec.is_open()) std::cerr << "recording stopped: " << record_path << "\n";
  return 0;
}
