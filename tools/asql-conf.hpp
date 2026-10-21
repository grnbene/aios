// Connection defaults for the ASQL client tools, read from the host's own
// /etc/aios/asql.conf (the file asql-serve starts the engine from), so that
// on an installed machine `aios-asql-console` and `aios-asql-top` connect
// with no flags: the unix socket, the wire user and its password file are
// all already written down there. Flags still override every field.
#pragma once
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>

namespace asql_conf {

struct Defaults {
  std::string conf_path;   // the file the values came from ("" = none read)
  std::string socket;      // MYSQL_SOCKET
  std::string host;        // MYSQL_BIND when it is a real address
  uint16_t port = 0;       // MYSQL_PORT
  std::string user;        // MYSQL_USER
  std::string pass;        // first line of MYSQL_PASS_FILE
  std::string db;          // MYSQL_DB, else CDC_DB
};

inline std::string trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  size_t e = s.find_last_not_of(" \t\r\n");
  return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}

// KEY=VALUE lines, # comments, optional single or double quotes round the
// value: the subset of shell the conf actually uses.
inline std::map<std::string, std::string> read_kv(const std::string& path) {
  std::map<std::string, std::string> kv;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    if (line.compare(0, 7, "export ") == 0) line = trim(line.substr(7));
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
    kv[k] = v;
  }
  return kv;
}

inline std::string first_line(const std::string& path) {
  std::ifstream in(path);
  std::string line;
  if (!std::getline(in, line)) return "";
  return trim(line);
}

// `explicit_path` wins; else $ASQL_CONF; else /etc/aios/asql.conf. A file that
// cannot be read yields empty defaults, never an error: the flags still work.
inline Defaults load(const std::string& explicit_path = "") {
  Defaults d;
  std::string path = explicit_path;
  if (path.empty()) {
    if (const char* e = std::getenv("ASQL_CONF")) path = e;
    else path = "/etc/aios/asql.conf";
  }
  std::ifstream probe(path);
  if (!probe.good()) return d;
  probe.close();
  auto kv = read_kv(path);
  d.conf_path = path;
  auto get = [&](const char* k) { auto it = kv.find(k); return it == kv.end() ? std::string() : it->second; };
  d.socket = get("MYSQL_SOCKET");
  d.host = get("MYSQL_BIND");
  if (d.host == "0.0.0.0") d.host.clear();
  if (!get("MYSQL_PORT").empty()) d.port = (uint16_t)std::atoi(get("MYSQL_PORT").c_str());
  d.user = get("MYSQL_USER");
  const std::string pf = get("MYSQL_PASS_FILE");
  if (!pf.empty()) d.pass = first_line(pf);
  d.db = get("MYSQL_DB");
  if (d.db.empty()) d.db = get("CDC_DB");
  return d;
}

}  // namespace asql_conf
