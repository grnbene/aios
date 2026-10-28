// asql-erp-bench: side-by-side ERP query benchmarker
//   MariaDB (direct CLI) vs AIOS ASQL (local socket port 7311)
//
// Usage:
//   asql-erp-bench --user U [--pass P] [--runs N] [--host HOST] [--port PORT]
//                  [--db DB] [--asql-host H] [--asql-port P]
//                  [--query-file FILE | --query SQL | --userid ERP_USER]
//
// The MariaDB password comes from --pass or, better (it stays out of `ps`),
// the MYSQL_PWD environment variable.
//
// Without a query argument, runs the built-in ERP suite (stocktrans 7-day,
// getOpenOrdersInvDispatch-style, etc.) as the ERP user named by --userid.
//
// Connects to MariaDB over the mysql command-line client (no libmysqlclient
// dependency needed in the build).  Connects to ASQL over the raw 4-byte
// big-endian length-prefixed JSON socket (same wire format as asql.php).
//
// Build target: aios-asql-erp-bench  (added to CMakeLists.txt)

#include "aios/hal/net.hpp"
#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <netdb.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>
#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// Tiny JSON helpers (just enough to extract "rows", "columns", "error")
// ---------------------------------------------------------------------------

// Count top-level array elements in a JSON array starting at pos.
// Returns -1 on error.  We only need the count for parity checks.
static int json_array_count(const std::string& s, size_t pos) {
    if (pos >= s.size() || s[pos] != '[') return -1;
    int depth = 0, count = 0;
    bool in_str = false;
    for (size_t i = pos; i < s.size(); ++i) {
        char c = s[i];
        if (in_str) {
            if (c == '\\') { ++i; continue; }
            if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') { in_str = true; continue; }
        if (c == '[' || c == '{') {
            if (depth == 0 && c == '[') { depth++; continue; }
            depth++;
        } else if (c == ']' || c == '}') {
            depth--;
            if (depth == 0) { if (count == 0 && i == pos+1) return 0; return count; }
        } else if (c == ',' && depth == 1) {
            count++;
        }
    }
    return -1;
}

static size_t json_find_key(const std::string& s, const std::string& key, size_t from = 0) {
    std::string needle = "\"" + key + "\"";
    auto p = s.find(needle, from);
    if (p == std::string::npos) return std::string::npos;
    // skip whitespace then ':'
    p += needle.size();
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) ++p;
    if (p >= s.size() || s[p] != ':') return std::string::npos;
    ++p;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) ++p;
    return p;
}

static std::string json_extract_string(const std::string& s, size_t pos) {
    if (pos >= s.size() || s[pos] != '"') return "";
    std::string out;
    for (size_t i = pos + 1; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) { out += s[++i]; continue; }
        if (s[i] == '"') break;
        out += s[i];
    }
    return out;
}

// ---------------------------------------------------------------------------
// ASQL socket interface
// ---------------------------------------------------------------------------

struct AsqlConn {
    int fd = -1;
    std::string host;
    int port;

    bool connect(const std::string& h, int p) {
        host = h; port = p;
        auto addr = aios::hal::resolve_ipv4(h);   // the engine's resolver: a static build has no NSS
        if (!addr.ok()) return false;
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons((uint16_t)p);
        to.sin_addr.s_addr = *addr;
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;
        if (::connect(fd, reinterpret_cast<sockaddr*>(&to), sizeof to) != 0) { close(fd); fd = -1; return false; }
        return true;
    }

    // Send 4-byte BE length + JSON payload.
    bool send_msg(const std::string& json) {
        uint32_t len = htonl((uint32_t)json.size());
        if (write(fd, &len, 4) != 4) return false;
        size_t sent = 0;
        while (sent < json.size()) {
            ssize_t n = write(fd, json.data() + sent, json.size() - sent);
            if (n <= 0) return false;
            sent += n;
        }
        return true;
    }

    // Read 4-byte BE length + that many bytes.
    std::string recv_msg() {
        uint32_t nlen = 0;
        size_t got = 0;
        while (got < 4) {
            ssize_t n = read(fd, (char*)&nlen + got, 4 - got);
            if (n <= 0) return "";
            got += n;
        }
        uint32_t len = ntohl(nlen);
        if (len == 0 || len > 128 * 1024 * 1024) return "";
        std::string buf(len, '\0');
        got = 0;
        while (got < len) {
            ssize_t n = read(fd, buf.data() + got, len - got);
            if (n <= 0) break;
            got += n;
        }
        return buf;
    }

    // Run an SQL query, return raw JSON response and elapsed milliseconds.
    // Re-opens connection each time so we get a clean session per call.
    struct Result {
        double ms = 0.0;
        int    rows = -1;      // -1 = error
        std::string error;
        std::string raw;
    };

    Result query(const std::string& sql) {
        Result out;
        // reconnect
        if (fd >= 0) { close(fd); fd = -1; }
        if (!connect(host, port)) {
            out.error = "connection refused";
            return out;
        }
        // {"op":"sql","query":"..."}
        std::string escaped;
        for (char c : sql) {
            if (c == '"')       escaped += "\\\"";
            else if (c == '\\') escaped += "\\\\";
            else if (c == '\n') escaped += "\\n";
            else if (c == '\r') escaped += "\\r";
            else if (c == '\t') escaped += "\\t";
            else                escaped += c;
        }
        std::string msg = "{\"op\":\"sql\",\"query\":\"" + escaped + "\"}";

        auto t0 = std::chrono::steady_clock::now();
        if (!send_msg(msg)) { out.error = "send failed"; return out; }
        out.raw = recv_msg();
        auto t1 = std::chrono::steady_clock::now();
        out.ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        // ASQL wire format: {"ok":true,"op":"sql","columns":[...],"rows":[[v,v],[v,v]],"row_count":N}
        // Rows are positional string arrays (not objects). Use row_count directly — always present.
        auto rc_pos = json_find_key(out.raw, "row_count");
        if (rc_pos != std::string::npos && rc_pos < out.raw.size()) {
            out.rows = std::atoi(out.raw.c_str() + rc_pos);
        }

        // Check ok:false or non-empty error string
        auto ok_pos = json_find_key(out.raw, "ok");
        if (ok_pos != std::string::npos && out.raw.size() > ok_pos + 4
                && out.raw.substr(ok_pos, 5) == "false") {
            out.rows = -1;
        }
        auto err_pos = json_find_key(out.raw, "error");
        if (err_pos != std::string::npos) {
            std::string e = json_extract_string(out.raw, err_pos);
            if (!e.empty() && e != "null") {
                out.error = e;
                out.rows = -1;
            }
        }
        // Fallback: if row_count not found, count inner arrays in rows field
        if (rc_pos == std::string::npos) {
            auto rows_pos = json_find_key(out.raw, "rows");
            if (rows_pos != std::string::npos && out.raw[rows_pos] == '[') {
                out.rows = 0;
                int depth = 0;
                bool in_str = false;
                for (size_t i = rows_pos; i < out.raw.size(); ++i) {
                    char c = out.raw[i];
                    if (in_str) {
                        if (c == '\\') { ++i; continue; }
                        if (c == '"')  in_str = false;
                        continue;
                    }
                    if (c == '"')  { in_str = true; continue; }
                    if (c == '[') {
                        if (depth == 1) ++out.rows; // each inner [ at depth 1 = one row
                        ++depth;
                    } else if (c == ']') {
                        --depth;
                        if (depth == 0) break;
                    }
                }
            }
        }
        return out;
    }

    ~AsqlConn() { if (fd >= 0) close(fd); }
};

// ---------------------------------------------------------------------------
// MariaDB via mysql CLI
// ---------------------------------------------------------------------------

struct MariaResult {
    double ms = 0.0;
    int    rows = -1;
    std::string error;
};

static MariaResult run_mariadb(const std::string& host,
                                const std::string& user,
                                const std::string& pass,
                                const std::string& db,
                                const std::string& sql) {
    MariaResult out;
    // Write query to temp file
    char qpath[] = "/tmp/asql-bench-query-XXXXXX";
    char opath[] = "/tmp/asql-bench-out-XXXXXX";
    {
        int fd = mkstemp(qpath);
        if (fd < 0) { out.error = "mkstemp"; return out; }
        write(fd, sql.c_str(), sql.size());
        write(fd, "\n", 1);
        close(fd);
    }
    {
        int fd = mkstemp(opath);
        if (fd >= 0) close(fd);
    }

    // Use MYSQL_PWD to avoid the "insecure password" warning that causes exit 1 (256 in shell).
    // -ss: silent mode, no column headers, no row count lines — pure data output.
    std::string cmd = "MYSQL_PWD=" + pass + " mysql -h " + host + " -u " + user
                    + " " + db + " -ss --connect-timeout=10"
                    + " < " + qpath + " > " + opath + " 2>/dev/null";

    auto t0 = std::chrono::steady_clock::now();
    int rc = system(cmd.c_str());
    auto t1 = std::chrono::steady_clock::now();
    out.ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (rc != 0) {
        out.error = "mysql exit " + std::to_string(rc);
    } else {
        // Count lines in output
        std::ifstream f(opath);
        out.rows = 0;
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty()) ++out.rows;
        }
    }
    unlink(qpath);
    unlink(opath);
    return out;
}

// ---------------------------------------------------------------------------
// Benchmark suite definition
// ---------------------------------------------------------------------------

struct BenchQuery {
    std::string name;
    std::string sql;
};

static std::vector<BenchQuery> builtin_suite(const std::string& userid) {
    return {
        {
            "stocktrans 7-day line sales (VIEW + nested IN subquery)",
            "SELECT lineid AS label,"
            "       SUM(discountedlinetotal * -1) AS total,"
            "       SUM(qty * -1) AS qty"
            " FROM stocktrans"
            " WHERE trandate BETWEEN (DATE(NOW() - INTERVAL 7 DAY)) AND DATE(NOW())"
            "   AND debtorno IN ("
            "     SELECT debtorno FROM custbranch WHERE defaultlocation IN ("
            "       SELECT loccode FROM locationusers WHERE userid ='" + userid + "'"
            "     )"
            "   )"
            " GROUP BY lineid"
            " ORDER BY total DESC"
        },
        {
            "stocktrans 30-day line sales",
            "SELECT lineid AS label,"
            "       SUM(discountedlinetotal * -1) AS total,"
            "       SUM(qty * -1) AS qty"
            " FROM stocktrans"
            " WHERE trandate BETWEEN (DATE(NOW() - INTERVAL 30 DAY)) AND DATE(NOW())"
            "   AND debtorno IN ("
            "     SELECT debtorno FROM custbranch WHERE defaultlocation IN ("
            "       SELECT loccode FROM locationusers WHERE userid ='" + userid + "'"
            "     )"
            "   )"
            " GROUP BY lineid"
            " ORDER BY total DESC"
        },
        {
            "dispatch uncleared order count",
            "SELECT COUNT(*) FROM dispatch WHERE cleared = 0 AND oclater = 0"
        },
        {
            "salesorders open count by status",
            "SELECT orderstatusid, COUNT(*) AS cnt"
            " FROM salesorders"
            " WHERE orderstatusid > 0"
            " GROUP BY orderstatusid"
            " ORDER BY cnt DESC"
            " LIMIT 20"
        },
        {
            "locstock fillable top items",
            "SELECT stockid, SUM(quantity) AS total_qty"
            " FROM locstock"
            " GROUP BY stockid"
            " ORDER BY total_qty DESC"
            " LIMIT 25"
        },
        {
            "pickingfreeze pending by order",
            "SELECT pf.document, COUNT(*) AS line_count, SUM(pf.pick) AS total_pick"
            " FROM pickingfreeze pf"
            " GROUP BY pf.document"
            " ORDER BY total_pick DESC"
            " LIMIT 20"
        },
        {
            "debtorsmaster customer list",
            "SELECT debtorno, name, creditlimit, currcode"
            " FROM debtorsmaster"
            " ORDER BY creditlimit DESC"
            " LIMIT 50"
        },
    };
}

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------

static std::string bar(double speedup) {
    // ██ style bar capped at 20x
    int filled = (int)std::min(speedup / 1.0, 20.0);
    return std::string(filled, '#');
}

static void print_separator(int w1, int w2, int w3, int w4, int w5) {
    std::cout << "+"
              << std::string(w1+2, '-') << "+"
              << std::string(w2+2, '-') << "+"
              << std::string(w3+2, '-') << "+"
              << std::string(w4+2, '-') << "+"
              << std::string(w5+2, '-') << "+\n";
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    // Defaults
    std::string db_host   = "127.0.0.1";
    std::string db_user;
    std::string db_pass   = std::getenv("MYSQL_PWD") ? std::getenv("MYSQL_PWD") : "";
    std::string db_name   = "versioneight";
    std::string asql_host = "127.0.0.1";
    int         asql_port = 7311;
    int         runs      = 3;
    std::string query_override;
    std::string query_name = "custom";
    std::string userid;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { std::cerr << "missing value for " << a << "\n"; exit(1); }
            return argv[++i];
        };
        if      (a == "--host")      db_host   = next();
        else if (a == "--user")      db_user   = next();
        else if (a == "--pass")      db_pass   = next();
        else if (a == "--db")        db_name   = next();
        else if (a == "--asql-host") asql_host = next();
        else if (a == "--asql-port") asql_port = std::stoi(next());
        else if (a == "--runs")      runs      = std::stoi(next());
        else if (a == "--userid")    userid    = next();
        else if (a == "--query")     { query_override = next(); }
        else if (a == "--query-file"){
            std::ifstream f(next());
            if (!f) { std::cerr << "cannot open query file\n"; return 1; }
            std::stringstream ss; ss << f.rdbuf();
            query_override = ss.str();
        }
        else if (a.rfind("--", 0) != 0) {
            // positional: treat as userid override for built-in suite
            userid = a;
        }
    }

    if (db_user.empty()) {
        std::cerr << "--user is required (the MariaDB account to time against)\n";
        return 1;
    }
    if (query_override.empty() && userid.empty()) {
        std::cerr << "the built-in suite needs --userid (an ERP user whose locations it reads), or pass --query/--query-file\n";
        return 1;
    }

    std::vector<BenchQuery> suite;
    if (!query_override.empty()) {
        suite.push_back({query_name, query_override});
    } else {
        suite = builtin_suite(userid);
    }

    // Connect ASQL
    AsqlConn asql;
    bool asql_ok = asql.connect(asql_host, asql_port);

    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║          AIOS ASQL vs MariaDB — ERP Query Benchmark                         ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
    std::cout << "║  MariaDB  : " << db_host << " / " << db_name << std::setw(49 - (int)db_host.size() - (int)db_name.size()) << "" << "║\n";
    std::cout << "║  ASQL     : " << asql_host << ":" << asql_port << (asql_ok ? "  [CONNECTED]" : "  [UNREACHABLE]") << std::setw(37 - (int)asql_host.size() - (int)std::to_string(asql_port).size()) << "" << "║\n";
    std::cout << "║  Runs/query: " << runs << std::setw(64) << "" << "║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════════════════════╝\n\n";

    if (!asql_ok) {
        std::cerr << "WARNING: ASQL socket unreachable at " << asql_host << ":" << asql_port << "\n";
        std::cerr << "         MariaDB timings will still run.\n\n";
    }

    struct Summary {
        std::string name;
        double maria_best_ms = 0.0;
        double asql_best_ms  = 0.0;
        int    maria_rows    = -1;
        int    asql_rows     = -1;
        bool   parity        = false;
        double speedup       = 0.0;
    };
    std::vector<Summary> summaries;

    for (const auto& q : suite) {
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
        std::cout << "  " << q.name << "\n";
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
        // Print query (truncated)
        std::string disp = q.sql;
        if (disp.size() > 200) disp = disp.substr(0, 197) + "...";
        std::cout << "  SQL: " << disp << "\n\n";

        // --- MariaDB runs ---
        std::vector<double> maria_ms;
        int maria_rows = -1;
        for (int r = 0; r < runs; ++r) {
            auto res = run_mariadb(db_host, db_user, db_pass, db_name, q.sql);
            if (res.rows >= 0) {
                maria_ms.push_back(res.ms);
                maria_rows = res.rows;
                std::cout << "  MariaDB  run " << (r+1) << ": " << std::fixed << std::setprecision(1)
                          << res.ms << " ms  (" << res.rows << " rows)\n";
            } else {
                std::cout << "  MariaDB  run " << (r+1) << ": ERROR — " << res.error << "\n";
            }
        }

        // --- ASQL runs ---
        std::vector<double> asql_ms;
        int asql_rows = -1;
        if (asql_ok) {
            for (int r = 0; r < runs; ++r) {
                auto res = asql.query(q.sql);
                if (res.rows >= 0) {
                    asql_ms.push_back(res.ms);
                    asql_rows = res.rows;
                    std::cout << "  ASQL     run " << (r+1) << ": " << std::fixed << std::setprecision(1)
                              << res.ms << " ms  (" << res.rows << " rows)\n";
                } else {
                    std::cout << "  ASQL     run " << (r+1) << ": ERROR — " << res.error << "\n";
                }
            }
        }

        // Best run (min latency — avoids OS scheduling noise)
        double maria_best = maria_ms.empty() ? 0.0 : *std::min_element(maria_ms.begin(), maria_ms.end());
        double asql_best  = asql_ms.empty()  ? 0.0 : *std::min_element(asql_ms.begin(), asql_ms.end());
        double speedup    = (asql_best > 0.0 && maria_best > 0.0) ? maria_best / asql_best : 0.0;
        bool   parity     = (maria_rows >= 0 && asql_rows >= 0 && maria_rows == asql_rows);

        std::cout << "\n";
        std::cout << "  ┌─────────────────────────────────────────────────────┐\n";
        std::cout << "  │  MariaDB best : " << std::setw(9) << std::fixed << std::setprecision(1) << maria_best << " ms   rows=" << std::setw(6) << maria_rows << "         │\n";
        std::cout << "  │  ASQL    best : " << std::setw(9) << std::fixed << std::setprecision(1) << asql_best  << " ms   rows=" << std::setw(6) << asql_rows  << "         │\n";
        if (speedup > 0.0) {
            std::cout << "  │  Speedup      : " << std::setw(6) << std::fixed << std::setprecision(2) << speedup << "x              " << bar(speedup) << std::string(std::max(0, 20 - (int)std::min(speedup / 1.0, 20.0)), ' ') << "│\n";
        }
        std::cout << "  │  Parity       : " << (parity ? "✓ MATCH" : (maria_rows < 0 || asql_rows < 0 ? "? (error)" : "✗ MISMATCH")) << std::string(35, ' ') << "│\n";
        std::cout << "  └─────────────────────────────────────────────────────┘\n\n";

        summaries.push_back({q.name, maria_best, asql_best, maria_rows, asql_rows, parity, speedup});
    }

    // Summary table
    std::cout << "\n╔══════════════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                          BENCHMARK SUMMARY                                  ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════════════════════╝\n\n";
    std::cout << std::left << std::setw(45) << "Query"
              << std::right << std::setw(10) << "MariaDB"
              << std::setw(10) << "ASQL"
              << std::setw(9)  << "Speedup"
              << std::setw(8)  << "Parity"
              << "\n";
    std::cout << std::string(82, '-') << "\n";
    for (const auto& s : summaries) {
        std::string name = s.name;
        if (name.size() > 44) name = name.substr(0, 41) + "...";
        std::cout << std::left  << std::setw(45) << name
                  << std::right << std::setw(9)  << std::fixed << std::setprecision(0) << s.maria_best_ms << "ms"
                  << std::setw(8)                               << std::setprecision(0) << s.asql_best_ms  << "ms"
                  << std::setw(8)                               << std::setprecision(2) << s.speedup       << "x"
                  << std::setw(7) << (s.parity ? "✓" : (s.maria_rows < 0 || s.asql_rows < 0 ? "?" : "✗"))
                  << "\n";
    }
    std::cout << std::string(82, '-') << "\n";

    return 0;
}
