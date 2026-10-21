// AIOS ASQL Console — an interactive SQL client for a local or remote ASQL
// MySQL-wire port (:3307), speaking the wire protocol directly through
// MariaWireSession. Dependency-free: no libmariadb, no mysql/mariadb CLI, no
// Node, no Electron. Line editing (history, tab completion) is optional —
// see AIOS_HAVE_EDITLINE below — so this still builds and runs on a host
// with no libedit/readline dev headers, just without those niceties.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "aios/userland/mysql_wire.hpp"
#include "asql-conf.hpp"

#if defined(AIOS_HAVE_EDITLINE)
#if AIOS_EDITLINE_VARIANT == 1
#include <editline/readline.h>
#else
#include <readline/history.h>
#include <readline/readline.h>
#endif
#endif

using namespace aios;
using namespace aios::userland;

namespace {

const char* kHelpText =
    "Commands:\n"
    "  <sql>;                 run a statement (spans multiple lines until ';')\n"
    "  USE <database>;        switch the active database (updates the prompt)\n"
    "  \\d                     show the active database\n"
    "  \\h or help             this help\n"
    "  \\q, exit, or quit      leave the console\n"
    "\n"
    "Any other statement (SELECT/SHOW/INSERT/UPDATE/DELETE/DESCRIBE/...) goes\n"
    "straight to the engine as a real COM_QUERY over the MySQL wire protocol.\n";

void print_usage(const char* prog) {
  std::cout << "Usage: " << prog << " [options] [-e \"<sql>\"]\n\n"
            << "Connection defaults come from /etc/aios/asql.conf (or $ASQL_CONF, or\n"
            << "--conf FILE): the socket, user, password file and database the engine\n"
            << "on this machine was started with. On an installed host no flag is needed.\n\n"
            << "Options:\n"
            << "  -S, --socket <path>     ASQL unix socket (conf: MYSQL_SOCKET)\n"
            << "  -h, --host <host>       ASQL host (default: 127.0.0.1; a /path is a socket)\n"
            << "  -P, --port <port>       ASQL MySQL-wire port (conf: MYSQL_PORT, else 3307)\n"
            << "  -u, --user <user>       User (conf: MYSQL_USER, else root)\n"
            << "  -p, --password <pass>   Password (conf: first line of MYSQL_PASS_FILE)\n"
            << "  -d, --database <db>     Initial database (conf: MYSQL_DB or CDC_DB)\n"
            << "  --conf <file>           Read defaults from this conf instead\n"
            << "  --no-conf               Ignore every conf file\n"
            << "  -e, --execute <sql>     Run one statement, print result, exit (no REPL)\n"
            << "  --help                  Show this help message\n\n"
            << "With no -e, starts an interactive console. Statements end with ';'.\n\n"
            << kHelpText;
}

std::string trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  size_t e = s.find_last_not_of(" \t\r\n");
  return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}

// A value as it should print: NULL literally, everything else verbatim.
std::string cell_text(const MariaValue& v) { return v.is_null ? "NULL" : v.bytes; }

void print_result(const MariaResult& res) {
  if (!res.ok) {
    std::cerr << "\033[1;31mERROR: " << res.error_message << "\033[0m\n";
    return;
  }
  if (!res.has_result_set) {
    std::cout << "\033[1;32mOK\033[0m";
    if (res.affected_rows > 0 || !res.message.empty())
      std::cout << " (" << res.affected_rows << " row(s) affected)";
    std::cout << "\n";
    return;
  }
  const auto& cols = res.columns;
  std::vector<std::vector<std::string>> text_rows;
  for (const auto& row : res.rows) {
    std::vector<std::string> tr;
    for (size_t i = 0; i < row.size(); ++i) tr.push_back(cell_text(row[i]));
    text_rows.push_back(std::move(tr));
  }
  std::vector<size_t> widths(cols.size());
  for (size_t i = 0; i < cols.size(); ++i) widths[i] = cols[i].name.size();
  for (const auto& tr : text_rows)
    for (size_t i = 0; i < tr.size() && i < widths.size(); ++i)
      widths[i] = std::max(widths[i], tr[i].size());

  auto print_sep = [&]() {
    std::cout << "+";
    for (auto w : widths) std::cout << std::string(w + 2, '-') << "+";
    std::cout << "\n";
  };
  auto print_row = [&](const std::vector<std::string>& vals) {
    std::cout << "|";
    for (size_t i = 0; i < widths.size(); ++i) {
      std::string v = i < vals.size() ? vals[i] : "";
      std::cout << " " << v << std::string(widths[i] - v.size(), ' ') << " |";
    }
    std::cout << "\n";
  };

  print_sep();
  std::vector<std::string> header;
  for (auto& c : cols) header.push_back(c.name);
  print_row(header);
  print_sep();
  for (auto& tr : text_rows) print_row(tr);
  print_sep();
  std::cout << text_rows.size() << " row(s) in set\n";
}

const char* kSqlKeywords[] = {
    "SELECT", "FROM", "WHERE", "AND", "OR", "NOT", "IN", "IS", "NULL", "LIKE",
    "ORDER", "BY", "GROUP", "HAVING", "LIMIT", "OFFSET", "JOIN", "LEFT",
    "RIGHT", "INNER", "OUTER", "ON", "AS", "DISTINCT", "INSERT", "INTO",
    "VALUES", "UPDATE", "SET", "DELETE", "CREATE", "TABLE", "DROP", "ALTER",
    "USE", "SHOW", "TABLES", "DATABASES", "DESCRIBE", "EXPLAIN", "COUNT",
    "SUM", "AVG", "MIN", "MAX", "UNION", "ALL", "EXISTS", "BETWEEN", "ASC",
    "DESC", "ENGINE", "STATUS", "GLOBAL", "PROCESSLIST",
};

#if defined(AIOS_HAVE_EDITLINE)
// Rebuilt after connect and after every USE, so Tab-completion offers this
// database's real table names, not just SQL keywords.
std::vector<std::string> g_completion_words;

void rebuild_completion_words(const std::vector<std::string>& tables) {
  g_completion_words.clear();
  for (auto* kw : kSqlKeywords) g_completion_words.push_back(kw);
  for (auto& t : tables) g_completion_words.push_back(t);
}

char* completion_generator(const char* text, int state) {
  static size_t index;
  static size_t len;
  if (state == 0) {
    index = 0;
    len = std::strlen(text);
  }
  while (index < g_completion_words.size()) {
    const std::string& w = g_completion_words[index++];
    if (w.size() >= len && strncasecmp(w.c_str(), text, len) == 0) {
      return strdup(w.c_str());
    }
  }
  return nullptr;
}

char** aios_completion(const char* text, int /*start*/, int /*end*/) {
  rl_attempted_completion_over = 1;  // don't fall back to filename completion
  return rl_completion_matches(text, completion_generator);
}

std::string history_file_path() {
  const char* home = std::getenv("HOME");
  return std::string(home ? home : ".") + "/.aios_asql_history";
}
#endif

// Fetch this database's table names for completion. Best-effort: a failure
// here just means Tab won't offer them, not a console error.
std::vector<std::string> fetch_table_names(MariaWireSession& session) {
  std::vector<std::string> names;
  auto res = session.query("SHOW TABLES");
  if (res.ok() && res->ok && res->has_result_set) {
    for (auto& row : res->rows)
      if (!row.empty() && !row[0].is_null) names.push_back(row[0].bytes);
  }
  return names;
}

}  // namespace

int main(int argc, char** argv) {
  // Flags first, so --conf / --no-conf are known before the defaults are read.
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
  std::string db = conf.db;  // empty = no initial database; connect, then SHOW DATABASES / USE
  std::string one_shot_sql;
  bool have_one_shot = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if ((arg == "-h" || arg == "--host") && i + 1 < argc) {
      host = argv[++i];
    } else if ((arg == "-S" || arg == "--socket") && i + 1 < argc) {
      host = argv[++i];
    } else if ((arg == "-P" || arg == "--port") && i + 1 < argc) {
      port = static_cast<uint16_t>(std::atoi(argv[++i]));
      // An explicit port means TCP: drop a socket the conf would have chosen.
      if (!host.empty() && host[0] == '/') host = !conf.host.empty() ? conf.host : "127.0.0.1";
    } else if ((arg == "-u" || arg == "--user") && i + 1 < argc) {
      user = argv[++i];
    } else if ((arg == "-p" || arg == "--password") && i + 1 < argc) {
      pass = argv[++i];
    } else if ((arg == "-d" || arg == "--database") && i + 1 < argc) {
      db = argv[++i];
    } else if ((arg == "-e" || arg == "--execute") && i + 1 < argc) {
      one_shot_sql = argv[++i];
      have_one_shot = true;
    } else if (arg == "--conf" && i + 1 < argc) {
      ++i;
    } else if (arg == "--no-conf") {
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      print_usage(argv[0]);
      return 1;
    }
  }
  const bool via_socket = !host.empty() && host[0] == '/';
  const std::string where = via_socket ? host : host + ":" + std::to_string(port);

  MariaWireSession session(host, port);
  auto c_res = session.connect(user, pass, db);
  if (!c_res.ok()) {
    std::cerr << "\033[1;31m[!] Connection to " << where << " as " << user << " failed: "
              << c_res.error().message << "\033[0m\n";
    if (!conf.conf_path.empty()) std::cerr << "    (defaults read from " << conf.conf_path << ")\n";
    return 1;
  }

  if (have_one_shot) {
    auto res = session.query(one_shot_sql);
    if (!res.ok()) {
      std::cerr << "\033[1;31mERROR: " << res.error().message << "\033[0m\n";
      return 1;
    }
    print_result(*res);
    return res->ok ? 0 : 1;
  }

  std::cout << "AIOS ASQL Console — connected to " << where
            << " (" << session.server_version() << ") as " << user << "\n";
  if (!db.empty()) std::cout << "Database: " << db << ". ";
  else std::cout << "No database selected — SHOW DATABASES; then USE <db>; to pick one. ";
  std::cout << "Statements end with ';'. \\h for help, \\q to leave.\n\n";

#if defined(AIOS_HAVE_EDITLINE)
  rl_attempted_completion_function = aios_completion;
  read_history(history_file_path().c_str());
  rebuild_completion_words(fetch_table_names(session));
#else
  std::cerr << "\033[2m(no libedit/readline at build time: no history, no tab completion)\033[0m\n";
#endif

  std::string active_db = db;
  std::string buffer;
  for (;;) {
    std::string db_tag = active_db.empty() ? "(no db)" : active_db;
    std::string prompt = buffer.empty() ? ("asql [" + db_tag + "]> ") : std::string("    -> ");
    std::string line;
#if defined(AIOS_HAVE_EDITLINE)
    char* raw = readline(prompt.c_str());
    if (!raw) break;  // EOF (Ctrl-D)
    line = raw;
    free(raw);
#else
    std::cout << prompt << std::flush;
    if (!std::getline(std::cin, line)) break;
#endif
    std::string trimmed = trim(line);
    if (buffer.empty() && (trimmed == "exit" || trimmed == "quit" || trimmed == "\\q")) break;
    if (buffer.empty() && trimmed == "\\d") {
      std::cout << "active database: " << (active_db.empty() ? "(none selected)" : active_db) << "\n";
      continue;
    }
    if (buffer.empty() && (trimmed == "\\h" || trimmed == "help")) {
      std::cout << kHelpText;
      continue;
    }
    if (trimmed.empty() && buffer.empty()) continue;

#if defined(AIOS_HAVE_EDITLINE)
    if (!trimmed.empty()) add_history(trimmed.c_str());
#endif

    buffer += (buffer.empty() ? "" : " ") + line;
    if (trim(buffer).back() != ';') continue;

    std::string sql = trim(buffer);
    sql.pop_back();  // drop trailing ';'
    buffer.clear();
    if (sql.empty()) continue;

    if (!session.connected()) {
      std::cerr << "\033[1;31m[!] Connection lost. Reconnecting...\033[0m\n";
      auto rc = session.connect(user, pass, active_db);
      if (!rc.ok()) {
        std::cerr << "\033[1;31m[!] Reconnect failed: " << rc.error().message << "\033[0m\n";
        continue;
      }
    }

    auto res = session.query(sql);
    if (!res.ok()) {
      std::cerr << "\033[1;31mERROR: " << res.error().message << "\033[0m\n";
      continue;
    }
    print_result(*res);

    // Keep the prompt's [db] tag — and completion's table list — honest
    // across USE statements.
    std::string upper;
    for (char c : sql) upper += static_cast<char>(std::toupper((unsigned char)c));
    size_t use_pos = upper.find("USE ");
    if (use_pos == 0) {
      std::string rest = trim(sql.substr(4));
      if (!rest.empty()) {
        active_db = rest;
#if defined(AIOS_HAVE_EDITLINE)
        rebuild_completion_words(fetch_table_names(session));
#endif
      }
    }
  }
  std::cout << "Bye\n";
#if defined(AIOS_HAVE_EDITLINE)
  write_history(history_file_path().c_str());
#endif
  session.close();
  return 0;
}
