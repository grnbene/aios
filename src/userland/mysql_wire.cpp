// AIOS — dependency-free MariaDB/MySQL native wire-protocol client and server.
//
// Implements the MySQL/MariaDB client and server wire protocols natively:
// 1. MariaWireSession: TCP/UNIX socket client supporting HandshakeV10,
//    mysql_native_password, caching_sha2_password, COM_QUERY, resultsets,
//    and error packets.
// 2. MysqlWireServer: High-performance wire protocol server allowing standard
//    MySQL clients (CLI, JDBC, Python, Node, DBeaver) to connect to AIOS,
//    execute SQL / ASQL queries, and receive standard MySQL result sets.

#include "aios/userland/mysql_wire.hpp"
#include "aios/userland/asql_binlog.hpp"
#include "aios/core/bytes.hpp"
#include "aios/hal/net.hpp"
#include "aios/hal/sync.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/sidecar/capability_guard.hpp"
#include "aios/userland/asql_capability.hpp"
#include "aios/net/tls.hpp"
#include "aios/net/tls_conn.hpp"
#include "aios/userland/sql_database.hpp"

#if defined(AIOS_HAVE_LIBDEFLATE)
#include <libdeflate.h>
#elif defined(AIOS_HAVE_ZLIB)
#include <zlib.h>
#endif
#if defined(AIOS_HAVE_ZSTD)
#include <zstd.h>
#endif
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <memory>
#include <random>
#include <sstream>
#include <cctype>
#include <string>
#include <unordered_map>
#include <vector>

namespace aios::userland {

namespace {

// Minimum-length integer helpers
[[maybe_unused]] uint64_t u24(const uint8_t* p) {
  return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16);
}

uint16_t get_u16(const uint8_t* p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

uint32_t get_u32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint64_t get_u64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= ((uint64_t)p[i] << (8 * i));
  }
  return v;
}

uint64_t get_lenenc_u64(const uint8_t* p, size_t max_len, size_t* bytes_read) {
  if (max_len == 0) {
    if (bytes_read) *bytes_read = 0;
    return 0;
  }
  uint8_t first = p[0];
  if (first < 0xfb) {
    if (bytes_read) *bytes_read = 1;
    return first;
  }
  if (first == 0xfc) {
    if (max_len < 3) {
      if (bytes_read) *bytes_read = max_len;
      return 0;
    }
    if (bytes_read) *bytes_read = 3;
    return get_u16(p + 1);
  }
  if (first == 0xfd) {
    if (max_len < 4) {
      if (bytes_read) *bytes_read = max_len;
      return 0;
    }
    if (bytes_read) *bytes_read = 4;
    return u24(p + 1);
  }
  if (first == 0xfe) {
    if (max_len < 9) {
      if (bytes_read) *bytes_read = max_len;
      return 0;
    }
    if (bytes_read) *bytes_read = 9;
    return get_u64(p + 1);
  }
  if (bytes_read) *bytes_read = 1;
  return 0;
}

void put_u24(std::vector<uint8_t>* v, uint64_t x) {
  v->push_back((uint8_t)(x & 0xff));
  v->push_back((uint8_t)((x >> 8) & 0xff));
  v->push_back((uint8_t)((x >> 16) & 0xff));
}

void put_u16(std::vector<uint8_t>* v, uint64_t x) {
  v->push_back((uint8_t)(x & 0xff));
  v->push_back((uint8_t)((x >> 8) & 0xff));
}

void put_u32(std::vector<uint8_t>* v, uint64_t x) {
  v->push_back((uint8_t)(x & 0xff));
  v->push_back((uint8_t)((x >> 8) & 0xff));
  v->push_back((uint8_t)((x >> 16) & 0xff));
  v->push_back((uint8_t)((x >> 24) & 0xff));
}

void put_u64(std::vector<uint8_t>* v, uint64_t x) {
  for (int i = 0; i < 8; ++i) {
    v->push_back((uint8_t)((x >> (8 * i)) & 0xff));
  }
}

void put_lenenc_u64(std::vector<uint8_t>* v, uint64_t x) {
  if (x < 0xfb) {
    v->push_back((uint8_t)x);
  } else if (x <= 0xffff) {
    v->push_back(0xfc);
    put_u16(v, x);
  } else if (x <= 0xffffff) {
    v->push_back(0xfd);
    put_u24(v, x);
  } else {
    v->push_back(0xfe);
    put_u64(v, x);
  }
}

// Appended with memcpy: inserting a char range into a byte vector went
// through the element-by-element path (the types differ), a few percent of
// a result-set-heavy workload.
inline void put_raw(std::vector<uint8_t>* v, const void* p, size_t n) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  v->insert(v->end(), b, b + n);   // same type: one memmove, no zero fill first
}
void put_bytes(std::vector<uint8_t>* v, std::string_view s) { put_raw(v, s.data(), s.size()); }

// Whether a value is a number as the text protocol prints one. kind: 1 an
// integer, 2 with a fraction, 3 with an exponent; frac: digits after the point.
inline bool wire_number(const std::string& v, int* kind, int* frac) {
  size_t i = 0;
  const size_t n = v.size();
  if (n == 0) return false;
  if (v[i] == '-' || v[i] == '+') ++i;
  const size_t d0 = i;
  while (i < n && v[i] >= '0' && v[i] <= '9') ++i;
  const size_t int_digits = i - d0;
  size_t frac_digits = 0;
  bool point = false;
  if (i < n && v[i] == '.') {
    point = true;
    ++i;
    const size_t f0 = i;
    while (i < n && v[i] >= '0' && v[i] <= '9') ++i;
    frac_digits = i - f0;
  }
  if (int_digits == 0 && frac_digits == 0) return false;
  int k = point ? 2 : 1;
  if (i < n && (v[i] == 'e' || v[i] == 'E')) {
    ++i;
    if (i < n && (v[i] == '-' || v[i] == '+')) ++i;
    const size_t e0 = i;
    while (i < n && v[i] >= '0' && v[i] <= '9') ++i;
    if (i == e0) return false;
    k = 3;
  }
  if (i != n) return false;
  *kind = k;
  *frac = (int)frac_digits;
  return true;
}

// 'YYYY-MM-DD' (1), 'YYYY-MM-DD HH:MM:SS[.f]' (2), '[-]H[H[H]]:MM:SS[.f]' (3).
inline bool wire_temporal(const std::string& v, uint8_t kind) {
  auto dig = [&](size_t i) { return i < v.size() && v[i] >= '0' && v[i] <= '9'; };
  auto date = [&] { return v.size() >= 10 && dig(0) && dig(1) && dig(2) && dig(3) && v[4] == '-' && dig(5) && dig(6) && v[7] == '-' && dig(8) && dig(9); };
  auto mmss = [&](size_t o) { return v.size() >= o + 6 && v[o] == ':' && dig(o + 1) && dig(o + 2) && v[o + 3] == ':' && dig(o + 4) && dig(o + 5); };
  auto frac_tail = [&](size_t o) {
    if (v.size() == o) return true;
    if (v[o] != '.' || v.size() == o + 1) return false;
    for (size_t i = o + 1; i < v.size(); ++i) if (!dig(i)) return false;
    return true;
  };
  if (kind == 1) return date() && v.size() == 10;
  if (kind == 2) return date() && v.size() >= 19 && v[10] == ' ' && dig(11) && dig(12) && mmss(13) && frac_tail(19);
  if (kind == 3) {
    size_t o = (!v.empty() && v[0] == '-') ? 1 : 0;
    size_t h = 0;
    while (dig(o + h)) ++h;
    return h >= 1 && h <= 3 && mmss(o + h) && frac_tail(o + h + 6);
  }
  return false;
}

void put_lenenc_str(std::vector<uint8_t>* v, std::string_view s) {
  put_lenenc_u64(v, s.size());
  put_bytes(v, s);
}

// --- connection charset (MariaDB's latin1 is cp1252) ---
// The store is UTF-8. A client on a latin1 connection is sent cp1252 bytes and
// its SQL is read as cp1252; MariaDB does exactly this for any utf8 column, and
// PHP's default connection is latin1.
constexpr uint16_t kCp1252High[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

bool has_high_byte(std::string_view s) {
  for (unsigned char ch : s) if (ch >= 0x80) return true;
  return false;
}

bool collation_is_latin1(uint8_t id) {
  return id == 5 || id == 8 || id == 15 || id == 31 || id == 47 || id == 48 || id == 49 || id == 94;
}

std::string utf8_to_cp1252(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = (unsigned char)s[i];
    if (c < 0x80) { out.push_back((char)c); ++i; continue; }
    uint32_t cp = 0xFFFFFFFFu;
    size_t len = 1;
    if (c >= 0xC2 && c <= 0xDF && i + 1 < s.size()) { cp = ((c & 0x1Fu) << 6) | ((unsigned char)s[i + 1] & 0x3Fu); len = 2; }
    else if (c >= 0xE0 && c <= 0xEF && i + 2 < s.size()) {
      cp = ((c & 0x0Fu) << 12) | (((unsigned char)s[i + 1] & 0x3Fu) << 6) | ((unsigned char)s[i + 2] & 0x3Fu);
      len = 3;
    } else if (c >= 0xF0 && i + 3 < s.size()) len = 4;
    char b = '?';
    if (cp != 0xFFFFFFFFu) {
      if (cp >= 0xA0 && cp <= 0xFF) b = (char)cp;
      else for (int k = 0; k < 32; ++k) if (kCp1252High[k] == cp) { b = (char)(0x80 + k); break; }
    }
    out.push_back(b);
    i += len;
  }
  return out;
}

std::string cp1252_to_utf8(std::string_view s) {
  std::string out;
  out.reserve(s.size() + s.size() / 4);
  for (unsigned char c : s) {
    uint32_t cp = c;
    if (c >= 0x80 && c < 0xA0) cp = kCp1252High[c - 0x80];
    if (cp < 0x80) out.push_back((char)cp);
    else if (cp < 0x800) { out.push_back((char)(0xC0 | (cp >> 6))); out.push_back((char)(0x80 | (cp & 0x3F))); }
    else { out.push_back((char)(0xE0 | (cp >> 12))); out.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); out.push_back((char)(0x80 | (cp & 0x3F))); }
  }
  return out;
}

// `SET NAMES x`, `SET character_set_client|results|connection = x`: what the
// session now sends and expects. Returns false when the statement is not one.
bool parse_charset_set(const std::string& q, bool* in_latin1, bool* out_latin1) {
  size_t i = 0;
  auto ws = [&] { while (i < q.size() && (q[i] == ' ' || q[i] == '\t' || q[i] == '\r' || q[i] == '\n')) ++i; };
  auto word = [&]() {
    std::string w;
    while (i < q.size() && (std::isalnum((unsigned char)q[i]) || q[i] == '_' || q[i] == '@' || q[i] == '.')) w.push_back((char)std::tolower((unsigned char)q[i++]));
    return w;
  };
  ws();
  if (word() != "set") return false;
  bool any = false;
  for (;;) {
    ws();
    std::string name = word();
    if (name.empty()) return any;
    std::string val;
    if (name == "names") {
      ws();
      val = word();
    } else {
      while (name.rfind("@@", 0) == 0) name = name.substr(2);
      if (name.rfind("session.", 0) == 0) name = name.substr(8);
      ws();
      if (i < q.size() && q[i] == ':') ++i;
      if (i >= q.size() || q[i] != '=') return any;
      ++i;
      ws();
      if (i < q.size() && (q[i] == '\'' || q[i] == '"')) ++i;
      val = word();
    }
    const bool lat = val == "latin1" || val == "cp1252";
    if (name == "names") { *in_latin1 = lat; *out_latin1 = lat; any = true; }
    else if (name == "character_set_client" || name == "character_set_connection") { *in_latin1 = lat; any = true; }
    else if (name == "character_set_results") { *out_latin1 = lat; any = true; }
    else return any;
    ws();
    if (i < q.size() && q[i] == ',') { ++i; continue; }
    return any;
  }
}

// Packet/session protocol params from MySQL/MariaDB client-server protocol.
constexpr uint32_t CAP_LONG_PASSWORD = 1u << 0;
constexpr uint32_t CAP_FOUND_ROWS = 1u << 1;
constexpr uint32_t CAP_LONG_FLAG = 1u << 2;
constexpr uint32_t CAP_CONNECT_WITH_DB = 1u << 3;
constexpr uint32_t CAP_PROTOCOL_41 = 1u << 9;
constexpr uint32_t CAP_TRANSACTIONS = 1u << 13;
constexpr uint32_t CAP_SECURE_CONNECTION = 1u << 15;
constexpr uint32_t CAP_MULTI_STATEMENTS = 1u << 16;
constexpr uint32_t CAP_MULTI_RESULTS = 1u << 17;
constexpr uint32_t CAP_PLUGIN_AUTH = 1u << 19;
constexpr uint32_t CAP_PLUGIN_AUTH_LENENC_CLIENT_DATA = 1u << 21;

constexpr uint8_t HANDSHAKE_PROTOCOL = 0x0a;

uint64_t g_wire_started_ns = 0;   // when the first listener came up, for COM_STATISTICS' uptime

// MySQL Command constants
constexpr uint8_t COM_QUIT = 0x01;
constexpr uint8_t COM_INIT_DB = 0x02;
constexpr uint8_t COM_QUERY = 0x03;
constexpr uint8_t COM_FIELD_LIST = 0x04;
constexpr uint8_t COM_STATISTICS = 0x09;
constexpr uint8_t COM_PROCESS_INFO = 0x0a;
constexpr uint8_t COM_PROCESS_KILL = 0x0c;
constexpr uint8_t COM_DEBUG = 0x0d;
constexpr uint8_t COM_PING = 0x0e;
constexpr uint8_t COM_CHANGE_USER = 0x11;
constexpr uint8_t COM_BINLOG_DUMP = 0x12;
constexpr uint8_t COM_REGISTER_SLAVE = 0x15;
constexpr uint8_t COM_STMT_PREPARE = 0x16;
constexpr uint8_t COM_STMT_EXECUTE = 0x17;
constexpr uint8_t COM_STMT_SEND_LONG_DATA = 0x18;
constexpr uint8_t COM_BINLOG_DUMP_GTID = 0x1e;
constexpr uint8_t COM_STMT_CLOSE   = 0x19;
constexpr uint8_t COM_STMT_RESET   = 0x1a;
constexpr uint8_t COM_SET_OPTION   = 0x1b;
constexpr uint8_t COM_STMT_FETCH   = 0x1c;
constexpr uint8_t COM_RESET_CONNECTION = 0x1f;

// Server status flags carried by OK and EOF packets.
constexpr uint16_t SERVER_STATUS_AUTOCOMMIT = 0x0002;
constexpr uint16_t SERVER_MORE_RESULTS_EXISTS = 0x0008;

// MySQL Column Type constants
constexpr uint8_t MYSQL_TYPE_TINY = 0x01;
constexpr uint8_t MYSQL_TYPE_SHORT = 0x02;
constexpr uint8_t MYSQL_TYPE_LONG = 0x03;
constexpr uint8_t MYSQL_TYPE_FLOAT = 0x04;
constexpr uint8_t MYSQL_TYPE_DOUBLE = 0x05;
constexpr uint8_t MYSQL_TYPE_NULL = 0x06;
constexpr uint8_t MYSQL_TYPE_TIMESTAMP = 0x07;
constexpr uint8_t MYSQL_TYPE_LONGLONG = 0x08;
constexpr uint8_t MYSQL_TYPE_INT24 = 0x09;
constexpr uint8_t MYSQL_TYPE_DATE = 0x0a;
constexpr uint8_t MYSQL_TYPE_TIME = 0x0b;
constexpr uint8_t MYSQL_TYPE_DATETIME = 0x0c;
constexpr uint8_t MYSQL_TYPE_YEAR = 0x0d;
constexpr uint8_t MYSQL_TYPE_NEWDECIMAL = 0xf6;
constexpr uint8_t MYSQL_TYPE_BLOB = 0xfc;
constexpr uint8_t MYSQL_TYPE_VAR_STRING = 0xfd;
constexpr uint8_t MYSQL_TYPE_STRING = 0xfe;

struct WirePreparedStatement {
  uint32_t id = 0;
  std::string sql;
  size_t num_params = 0;
  std::vector<uint8_t> param_types;
  std::vector<bool> param_unsigned;
  // COM_STMT_SEND_LONG_DATA chunks per parameter (a mysqli `b` bind); a
  // parameter that has any is absent from the next execute's value block.
  std::vector<std::string> long_data;
  std::vector<bool> has_long_data;
};

// Split a multi-statement COM_QUERY into its statements, respecting quotes,
// backslash escapes and the three comment forms. Only called when the text
// has a ';' at all, so a single statement pays one memchr. A trailing ';'
// (or ';' followed by only whitespace and comments) does not add an empty
// statement, which is what MariaDB does with `SELECT 1;`.
std::vector<std::string> split_statements(const std::string& q) {
  std::vector<std::string> out;
  size_t start = 0;
  char quote = 0;
  // A CREATE PROCEDURE / FUNCTION / TRIGGER / EVENT holds its own ';'s inside
  // BEGIN ... END (and IF / CASE / LOOP / WHILE / REPEAT ... END): those are
  // not statement ends. Tracked only inside such a CREATE, so `BEGIN; ...;
  // COMMIT;` still splits as transaction statements.
  std::vector<std::string> head;   // the statement's first words, upper-cased
  bool compound = false;
  int depth = 0;
  std::string prev_word;
  auto word_at = [&](size_t i, size_t* end) {
    size_t j = i;
    while (j < q.size() && (std::isalnum((unsigned char)q[j]) || q[j] == '_' || q[j] == '$')) ++j;
    *end = j;
    std::string w = q.substr(i, j - i);
    for (auto& c : w) c = (char)std::toupper((unsigned char)c);
    return w;
  };
  for (size_t i = 0; i < q.size(); ++i) {
    const char c = q[i];
    if (quote) {
      if (c == '\\' && quote != '`') { ++i; continue; }
      if (c == quote) quote = 0;
      continue;
    }
    if (c == '\'' || c == '"' || c == '`') { quote = c; continue; }
    if (c == '#' || (c == '-' && i + 2 < q.size() && q[i + 1] == '-' && (q[i + 2] == ' ' || q[i + 2] == '\t'))) {
      while (i < q.size() && q[i] != '\n') ++i;
      continue;
    }
    if (c == '/' && i + 1 < q.size() && q[i + 1] == '*' && !(i + 2 < q.size() && (q[i + 2] == '!' || q[i + 2] == 'M'))) {
      const size_t e = q.find("*/", i + 2);
      if (e == std::string::npos) break;
      i = e + 1;
      continue;
    }
    if ((std::isalpha((unsigned char)c) || c == '_') && (i == 0 || !(std::isalnum((unsigned char)q[i - 1]) || q[i - 1] == '_'))) {
      size_t e;
      const std::string w = word_at(i, &e);
      if (head.size() < 8) {
        head.push_back(w);
        if (head[0] == "CREATE" && (w == "PROCEDURE" || w == "FUNCTION" || w == "TRIGGER" || w == "EVENT")) compound = true;
      }
      if (compound) {
        size_t k = e;
        while (k < q.size() && std::isspace((unsigned char)q[k])) ++k;
        const bool call = k < q.size() && q[k] == '(';
        if (w == "END") --depth;
        else if (prev_word != "END" && (w == "BEGIN" || w == "LOOP" || w == "WHILE" || w == "CASE" ||
                                        (w == "REPEAT" && !call) || (w == "IF" && !call))) {
          // IF EXISTS / IF NOT EXISTS (DROP TABLE IF EXISTS inside a body) is not a block.
          const std::string next = k < q.size() ? word_at(k, &k) : std::string();
          if (!(w == "IF" && (next == "EXISTS" || next == "NOT"))) ++depth;
        }
      }
      prev_word = w;
      i = e - 1;
      continue;
    }
    if (c == ';') prev_word.clear();   // `END; IF ...` is a new IF, not END IF
    if (c == ';' && depth <= 0) {
      out.push_back(q.substr(start, i - start));
      start = i + 1;
      head.clear();
      compound = false;
      depth = 0;
    }
  }
  std::string tail = q.substr(start);
  bool blank = true;
  for (char c : tail) if (c != ' ' && c != '\t' && c != '\r' && c != '\n') { blank = false; break; }
  if (!blank) out.push_back(std::move(tail));
  if (out.empty()) out.push_back(q);
  return out;
}

// The MySQL error code and SQLSTATE a client expects for an engine message.
// Applications branch on the code (1062 is how a PHP page tells "already
// there" from "broken"), and drivers show the SQLSTATE; every error used to
// be 1064 "syntax error", so a duplicate key looked like a typo.
void map_engine_error(const std::string& msg, uint16_t* code, const char** sqlstate) {
  auto starts = [&](const char* p) { return msg.compare(0, std::strlen(p), p) == 0; };
  auto has = [&](const char* p) { return msg.find(p) != std::string::npos; };
  *code = 1064; *sqlstate = "42000";
  // Stored programs, with MariaDB's codes.
  static thread_local std::string signal_state;
  if (sql_signal_state(msg, &signal_state)) {
    *sqlstate = signal_state.c_str();
    *code = signal_state.rfind("01", 0) == 0 ? 1642 : signal_state.rfind("02", 0) == 0 ? 1643 : 1644;   // ER_SIGNAL_*
    return;
  }
  const bool program = starts("PROCEDURE ") || starts("FUNCTION ") || starts("TRIGGER ") || starts("EVENT ") || starts("Trigger ");
  if (program && has("does not exist")) {
    if (starts("TRIGGER")) { *code = 1360; *sqlstate = "HY000"; }
    else if (starts("EVENT")) { *code = 1539; *sqlstate = "HY000"; }
    else { *code = 1305; *sqlstate = "42000"; }
    return;
  }
  if (program && has("already exists")) {
    if (starts("TRIGGER") || starts("Trigger")) { *code = 1359; *sqlstate = "HY000"; }
    else if (starts("EVENT")) { *code = 1537; *sqlstate = "HY000"; }
    else { *code = 1304; *sqlstate = "42000"; }
    return;
  }
  if (starts("SAVEPOINT ") && has("does not exist")) { *code = 1305; *sqlstate = "42000"; return; }
  if (starts("Deadlock found when trying to get lock")) { *code = 1213; *sqlstate = "40001"; return; }
  if (starts("Lock wait timeout exceeded")) { *code = 1205; *sqlstate = "HY000"; return; }
  if (starts("Statement aborted because lock(s) could not be acquired immediately")) { *code = 3572; *sqlstate = "HY000"; return; }
  if (starts("Incorrect number of arguments for")) { *code = 1318; *sqlstate = "42000"; return; }
  if (starts("Can't update table") && has("stored function/trigger")) { *code = 1442; *sqlstate = "HY000"; return; }
  if (starts("Result consisted of more than one row")) { *code = 1172; *sqlstate = "42000"; return; }
  if (starts("Subquery returns more than 1 row")) { *code = 1242; *sqlstate = "21000"; return; }
  if (has("is a GLOBAL variable and should be set with SET GLOBAL")) { *code = 1229; *sqlstate = "HY000"; return; }
  if (starts("Incorrect argument type to variable")) { *code = 1232; *sqlstate = "42000"; return; }
  if (starts("Can't group on '")) { *code = 1056; *sqlstate = "42000"; return; }
  if (starts("Recursive limit")) { *code = 1456; *sqlstate = "HY000"; return; }
  if (starts("Recursive stored functions and triggers")) { *code = 1424; *sqlstate = "HY000"; return; }
  if (starts("No RETURN found in FUNCTION")) { *code = 1321; *sqlstate = "2F005"; return; }
  if (starts("Case not found for CASE statement")) { *code = 1339; *sqlstate = "20000"; return; }
  if (starts("table not found") || starts("Table '") || has("doesn't exist")) { *code = 1146; *sqlstate = "42S02"; }
  else if (starts("no such column") || starts("Unknown column") || has("unknown column")) { *code = 1054; *sqlstate = "42S22"; }
  else if (starts("duplicate value for primary key") || starts("Duplicate entry") || has("duplicate key")) { *code = 1062; *sqlstate = "23000"; }
  else if (starts("Cannot add or update a child row")) { *code = 1452; *sqlstate = "23000"; }
  else if (starts("Cannot delete or update a parent row")) { *code = 1451; *sqlstate = "23000"; }
  else if (starts("Cannot truncate a table referenced in a foreign key constraint")) { *code = 1701; *sqlstate = "42000"; }
  else if (starts("Query execution was interrupted")) { *code = 1317; *sqlstate = "70100"; }
  else if (starts("Unknown thread id: ")) { *code = 1094; *sqlstate = "HY000"; }
  else if (has("already exists")) { *code = 1050; *sqlstate = "42S01"; }
  else if (starts("database not found") || starts("Unknown database") || has("no such database")) { *code = 1049; *sqlstate = "42000"; }
  else if (has("ran out of memory")) { *code = 1037; *sqlstate = "HY001"; }
  else if (starts("Access denied") || starts("access denied") || has("read-only") || has("read only")) { *code = 1044; *sqlstate = "42000"; }
  else if (has("ambiguous")) { *code = 1052; *sqlstate = "23000"; }
  else if (has("division by zero") || has("Division by 0")) { *code = 1365; *sqlstate = "22012"; }
  else if (starts("CONSTRAINT `") && has("` failed for `")) { *code = 4025; *sqlstate = "23000"; }
  else if (starts("Unknown prepared statement handler")) { *code = 1243; *sqlstate = "HY000"; }
  else if (starts("BIGINT value is out of range")) { *code = 1690; *sqlstate = "22003"; }
  else if (starts("Cannot execute statement in a READ ONLY transaction")) { *code = 1792; *sqlstate = "25006"; }
  else if (starts("Result consisted of more than one row")) { *code = 1172; *sqlstate = "42000"; }
  else if (starts("The value specified for generated column")) { *code = 1906; *sqlstate = "HY000"; }
  else if (has("cannot be null") || has("NOT NULL")) { *code = 1048; *sqlstate = "23000"; }
  else if (has("doesn't yet support")) { *code = 1235; *sqlstate = "42000"; }   // ER_NOT_SUPPORTED_YET
}

// A bound parameter as a literal the engine reads: quoted, with the bytes
// that would end or bend the literal escaped the way MariaDB's own client
// escapes them. A NUL byte goes out as \0 and the store keeps it.
std::string quote_literal(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('\'');
  for (unsigned char c : s) {
    switch (c) {
      case 0: out += "\\0"; break;
      case '\'': out += "\\'"; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case 0x1a: out += "\\Z"; break;
      default: out.push_back((char)c);
    }
  }
  out.push_back('\'');
  return out;
}

}  // namespace

std::string cp1252_bytes_to_utf8(std::string_view s) { return cp1252_to_utf8(s); }

// --- SHA-256 Implementation --------------------------------------------------
namespace {

struct Sha256Ctx {
  uint32_t h[8] = {
      0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint8_t buf[64];
  size_t buf_len = 0;
  uint64_t total_len = 0;
};

uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

void sha256_process(Sha256Ctx* c, const uint8_t block[64]) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i)
    w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
           ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
  for (int i = 16; i < 64; ++i) {
    uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
  uint32_t e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
  static const uint32_t K[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
      0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
      0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
      0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
      0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
      0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
      0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
      0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  for (int i = 0; i < 64; ++i) {
    uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    uint32_t ch = (e & f) ^ ((~e) & g);
    uint32_t temp1 = h + S1 + ch + K[i] + w[i];
    uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
    uint32_t temp2 = S0 + maj;
    h = g; g = f; f = e; e = d + temp1;
    d = cc; cc = b; b = a; a = temp1 + temp2;
  }
  c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
  c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void sha256_update(Sha256Ctx* c, const uint8_t* data, size_t n) {
  c->total_len += n;
  while (n > 0) {
    size_t take = std::min(n, 64 - c->buf_len);
    memcpy(c->buf + c->buf_len, data, take);
    c->buf_len += take;
    data += take;
    n -= take;
    if (c->buf_len == 64) {
      sha256_process(c, c->buf);
      c->buf_len = 0;
    }
  }
}

std::vector<uint8_t> sha256_final(Sha256Ctx* c) {
  uint64_t bit_len = c->total_len * 8;
  uint8_t pad = 0x80;
  sha256_update(c, &pad, 1);
  uint8_t zero = 0;
  while (c->buf_len != 56) sha256_update(c, &zero, 1);
  uint8_t len_buf[8];
  for (int i = 0; i < 8; ++i) len_buf[i] = (uint8_t)(bit_len >> (56 - 8 * i));
  sha256_update(c, len_buf, 8);
  std::vector<uint8_t> out(32);
  for (int i = 0; i < 8; ++i) {
    out[i * 4] = (uint8_t)(c->h[i] >> 24);
    out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
    out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
    out[i * 4 + 3] = (uint8_t)(c->h[i]);
  }
  return out;
}

std::vector<uint8_t> sha256(const uint8_t* data, size_t n) {
  Sha256Ctx c;
  sha256_update(&c, data, n);
  return sha256_final(&c);
}

// SHA-1 (FIPS 180-1), for mysql_native_password.
struct Sha1Ctx {
  uint32_t h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
  uint8_t buf[64];
  size_t buf_len = 0;
  uint64_t total_len = 0;
};

uint32_t left_rot(uint32_t x, uint32_t n) { return (x << n) | (x >> (32 - n)); }

void sha1_process(Sha1Ctx* c, const uint8_t block[64]) {
  uint32_t w[80];
  for (int i = 0; i < 16; ++i)
    w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
           ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
  for (int i = 16; i < 80; ++i) {
    w[i] = left_rot(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  }
  uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
  for (int i = 0; i < 80; ++i) {
    uint32_t f, k;
    if (i < 20) { f = (b & cc) | ((~b) & d); k = 0x5a827999; }
    else if (i < 40) { f = b ^ cc ^ d; k = 0x6ed9eba1; }
    else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8f1bbcdc; }
    else { f = b ^ cc ^ d; k = 0xca62c1d6; }
    uint32_t temp = left_rot(a, 5) + f + e + k + w[i];
    e = d; d = cc; cc = left_rot(b, 30); b = a; a = temp;
  }
  c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

void sha1_update(Sha1Ctx* c, const uint8_t* data, size_t n) {
  c->total_len += n;
  while (n > 0) {
    size_t take = std::min(n, 64 - c->buf_len);
    memcpy(c->buf + c->buf_len, data, take);
    c->buf_len += take;
    data += take;
    n -= take;
    if (c->buf_len == 64) { sha1_process(c, c->buf); c->buf_len = 0; }
  }
}

std::vector<uint8_t> sha1_final(Sha1Ctx* c) {
  uint64_t bit_len = c->total_len * 8;
  uint8_t pad = 0x80;
  sha1_update(c, &pad, 1);
  uint8_t zero = 0;
  while (c->buf_len != 56) sha1_update(c, &zero, 1);
  uint8_t len_buf[8];
  for (int i = 0; i < 8; ++i) len_buf[i] = (uint8_t)(bit_len >> (56 - 8 * i));
  sha1_update(c, len_buf, 8);
  std::vector<uint8_t> out(20);
  for (int i = 0; i < 5; ++i) {
    out[i * 4] = (uint8_t)(c->h[i] >> 24);
    out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
    out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
    out[i * 4 + 3] = (uint8_t)(c->h[i]);
  }
  return out;
}

std::vector<uint8_t> sha1_digest(const uint8_t* data, size_t n) {
  Sha1Ctx c;
  sha1_update(&c, data, n);
  return sha1_final(&c);
}

}  // namespace

std::vector<uint8_t> mysql_native_scramble(const std::string& password,
                                           const uint8_t* salt, size_t salt_len) {
  std::vector<uint8_t> out(20, 0);
  if (password.empty() || salt_len < 20) return out;
  auto stage1 = sha1_digest(reinterpret_cast<const uint8_t*>(password.data()), password.size());
  auto stage2 = sha1_digest(stage1.data(), stage1.size());
  std::vector<uint8_t> seed(salt, salt + 20);
  seed.insert(seed.end(), stage2.begin(), stage2.end());
  auto shake = sha1_digest(seed.data(), seed.size());
  for (int i = 0; i < 20; ++i) out[i] = (uint8_t)(stage1[i] ^ shake[i]);
  return out;
}

std::vector<uint8_t> caching_sha2_scramble(const std::string& password,
                                           const uint8_t* salt, size_t salt_len) {
  std::vector<uint8_t> out(32, 0);
  if (password.empty() || salt_len < 32) return out;
  auto p_sha = sha256(reinterpret_cast<const uint8_t*>(password.data()), password.size());
  auto p_sha2 = sha256(p_sha.data(), p_sha.size());
  std::vector<uint8_t> seed(salt, salt + 32);
  seed.insert(seed.end(), p_sha2.begin(), p_sha2.end());
  auto shake = sha256(seed.data(), seed.size());
  for (int i = 0; i < 32; ++i) out[i] = (uint8_t)(p_sha[i] ^ shake[i]);
  return out;
}

}  // namespace aios::userland

namespace aios {
std::vector<uint8_t> Sha256::digest(const uint8_t* data, size_t len) {
  return aios::userland::sha256(data, len);
}
}  // namespace aios

namespace aios::userland {

// --- MariaWireSession Implementation ------------------------------------------

namespace {
// The compressed protocol's codecs, for both ends (MysqlWireServer's sessions and MariaWireSession).
constexpr uint8_t kWireZlib = 1, kWireZstd = 2;
constexpr uint32_t kCapCompress = 0x00000020, kCapZstd = 1u << 26;   // CLIENT_COMPRESS, CLIENT_ZSTD_COMPRESSION_ALGORITHM
constexpr size_t kWireMinCompress = 50;    // smaller pieces go stored, as MySQL's MIN_COMPRESS_LENGTH

// `n` bytes compressed onto `out`; 0 when the algorithm could not (the piece then goes stored).
size_t wire_compress(uint8_t algo, int level, const uint8_t* src, size_t n, std::vector<uint8_t>& out) {
  const size_t at = out.size();
#if defined(AIOS_HAVE_ZSTD)
  if (algo == kWireZstd) {
    struct C { ZSTD_CCtx* c = ZSTD_createCCtx(); ~C() { ZSTD_freeCCtx(c); } };
    thread_local C cc;
    out.resize(at + ZSTD_compressBound(n));
    const size_t got = ZSTD_compressCCtx(cc.c, out.data() + at, out.size() - at, src, n, level);
    if (ZSTD_isError(got)) { out.resize(at); return 0; }
    out.resize(at + got);
    return got;
  }
#endif
  if (algo != kWireZlib) return 0;
#if defined(AIOS_HAVE_LIBDEFLATE)
  struct C {
    int level = -1;
    libdeflate_compressor* c = nullptr;
    ~C() { if (c) libdeflate_free_compressor(c); }
  };
  thread_local C cc;
  if (!cc.c || cc.level != level) {
    if (cc.c) libdeflate_free_compressor(cc.c);
    cc.c = libdeflate_alloc_compressor(std::clamp(level, 1, 12));
    cc.level = level;
  }
  if (!cc.c) return 0;
  out.resize(at + libdeflate_zlib_compress_bound(cc.c, n));
  const size_t got = libdeflate_zlib_compress(cc.c, src, n, out.data() + at, out.size() - at);
  out.resize(at + got);
  return got;
#elif defined(AIOS_HAVE_ZLIB)
  uLongf len = compressBound((uLong)n);
  out.resize(at + len);
  if (compress2(out.data() + at, &len, src, (uLong)n, std::clamp(level, 1, 9)) != Z_OK) { out.resize(at); return 0; }
  out.resize(at + len);
  return len;
#else
  (void)level; (void)src; (void)n;
  return 0;
#endif
}

// A frame's payload inflated to exactly `plain` bytes into `dst`.
bool wire_inflate(uint8_t algo, const uint8_t* src, size_t n, uint8_t* dst, size_t plain) {
#if defined(AIOS_HAVE_ZSTD)
  if (algo == kWireZstd) {
    struct D { ZSTD_DCtx* d = ZSTD_createDCtx(); ~D() { ZSTD_freeDCtx(d); } };
    thread_local D dc;
    const size_t got = ZSTD_decompressDCtx(dc.d, dst, plain, src, n);
    return !ZSTD_isError(got) && got == plain;
  }
#endif
  if (algo != kWireZlib) return false;
#if defined(AIOS_HAVE_LIBDEFLATE)
  struct D {
    libdeflate_decompressor* d = libdeflate_alloc_decompressor();
    ~D() { if (d) libdeflate_free_decompressor(d); }
  };
  thread_local D dc;
  size_t got = 0;
  return dc.d && libdeflate_zlib_decompress(dc.d, src, n, dst, plain, &got) == LIBDEFLATE_SUCCESS && got == plain;
#elif defined(AIOS_HAVE_ZLIB)
  uLongf got = (uLongf)plain;
  return uncompress(dst, &got, src, (uLong)n) == Z_OK && got == plain;
#else
  (void)src; (void)n; (void)dst; (void)plain;
  return false;
#endif
}

void put_u24(uint8_t* p, size_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); }
size_t get_u24(const uint8_t* p) { return (size_t)p[0] | ((size_t)p[1] << 8) | ((size_t)p[2] << 16); }

// `n` bytes of the packet stream as frames onto `out`: at most 16 MB of it each, compressed where that saves bytes.
void wire_frames(uint8_t algo, int level, uint8_t* seq, const uint8_t* p, size_t n, std::vector<uint8_t>& out) {
  for (size_t left = n; left > 0;) {
    const size_t piece = std::min(left, (size_t)0xFFFFFF);
    const size_t head = out.size();
    out.resize(head + 7);
    size_t clen = piece >= kWireMinCompress ? wire_compress(algo, level, p, piece, out) : 0;
    size_t plain = piece;
    if (clen == 0 || clen >= piece) {
      out.resize(head + 7);
      out.insert(out.end(), p, p + piece);
      clen = piece;
      plain = 0;
    }
    put_u24(out.data() + head, clen);
    out[head + 3] = (*seq)++;
    put_u24(out.data() + head + 4, plain);
    p += piece;
    left -= piece;
  }
}

}  // namespace

MariaWireSession::MariaWireSession(const std::string& host, uint16_t port)
    : port_(port ? port : 3306), host_(host.empty() ? "127.0.0.1" : host) {}

MariaWireSession::~MariaWireSession() { close(); }

void MariaWireSession::close() {
  comp_algo_ = 0;
  comp_seq_ = 0;
  comp_in_.clear();
  comp_in_at_ = 0;
  if (conn_) {
    conn_->close();
    conn_.reset();
  }
}

Result<void> MariaWireSession::read_exact(uint8_t* buf, size_t n) {
  if (!conn_ || !conn_->alive()) {
    return Result<void>(Status::IoError, "no open MySQL connection");
  }
  if (!comp_algo_) {
    if (!conn_->recv_all(MutableByteSpan(buf, n), read_timeout_ms_)) {
      return Result<void>(Status::IoError, "timed out reading from MySQL/MariaDB server");
    }
    return Result<void>();
  }
  while (n > 0) {
    if (comp_in_at_ == comp_in_.size()) {
      uint8_t h[7];
      if (!conn_->recv_all(MutableByteSpan(h, 7), read_timeout_ms_)) {
        return Result<void>(Status::IoError, "timed out reading from MySQL/MariaDB server");
      }
      const size_t clen = get_u24(h), plain = get_u24(h + 4);
      comp_seq_ = (uint8_t)(h[3] + 1);
      comp_frame_.resize(clen);
      if (clen && !conn_->recv_all(MutableByteSpan(comp_frame_.data(), clen), read_timeout_ms_)) {
        return Result<void>(Status::IoError, "timed out reading from MySQL/MariaDB server");
      }
      comp_in_at_ = 0;
      if (plain == 0) {
        comp_in_.swap(comp_frame_);
      } else {
        comp_in_.resize(plain);
        if (!wire_inflate(comp_algo_, comp_frame_.data(), clen, comp_in_.data(), plain)) {
          return Result<void>(Status::Corrupt, "a compressed packet from the server does not inflate");
        }
      }
      continue;
    }
    const size_t take = std::min(n, comp_in_.size() - comp_in_at_);
    std::memcpy(buf, comp_in_.data() + comp_in_at_, take);
    comp_in_at_ += take;
    buf += take;
    n -= take;
  }
  return Result<void>();
}

bool MariaWireSession::send_raw(const uint8_t* data, size_t n) {
  if (!comp_algo_) return conn_->send_all(ByteSpan(data, n));
  comp_out_.clear();
  wire_frames(comp_algo_, comp_level_, &comp_seq_, data, n, comp_out_);
  return conn_->send_all(ByteSpan(comp_out_.data(), comp_out_.size()));
}

// The three-byte length field caps one packet's payload; longer values are
// carried by a run of full-sized packets ending in a short one.
constexpr size_t kMaxClientPacketPayload = 0xFFFFFF;

Result<size_t> MariaWireSession::read_packet(std::vector<uint8_t>* out, uint8_t* seq) {
  out->clear();
  uint8_t hdr[4];
  Result<void> r = read_exact(hdr, 4);
  if (!r.ok()) return Result<size_t>(r.status(), r.error().message);
  size_t len = (size_t)hdr[0] | ((size_t)hdr[1] << 8) | ((size_t)hdr[2] << 16);
  if (seq) *seq = hdr[3];
  seq_ = hdr[3];
  if (len > 0) {
    out->resize(len);
    r = read_exact(out->data(), len);
    if (!r.ok()) return Result<size_t>(r.status(), r.error().message);
  }
  // A full-sized packet is a value continued in the next one; stopping here
  // returned a row cut off at 16 MB and left the rest to be read as headers.
  while (len == kMaxClientPacketPayload) {
    r = read_exact(hdr, 4);
    if (!r.ok()) return Result<size_t>(r.status(), r.error().message);
    len = (size_t)hdr[0] | ((size_t)hdr[1] << 8) | ((size_t)hdr[2] << 16);
    seq_ = hdr[3];
    if (seq) *seq = hdr[3];
    if (len > 0) {
      size_t at = out->size();
      out->resize(at + len);
      r = read_exact(out->data() + at, len);
      if (!r.ok()) return Result<size_t>(r.status(), r.error().message);
    }
  }
  return Result<size_t>(out->size());
}

Result<void> MariaWireSession::write_packet(const uint8_t* data, size_t n, uint8_t seq) {
  if (!conn_ || !conn_->alive()) return Result<void>(Status::IoError, "no open MySQL connection");
  if (seq == 0) comp_seq_ = 0;   // a new command: its frames count from 0 as well
  // Same three-byte length limit the server side has: a statement this big --
  // a wide multi-row INSERT is the usual one -- goes out as several packets,
  // and an exact multiple needs an empty one to end it.
  if (n >= kMaxClientPacketPayload) {
    size_t sent = 0;
    while (true) {
      size_t chunk = std::min(kMaxClientPacketPayload, n - sent);
      std::vector<uint8_t> buf(4 + chunk);
      buf[0] = (uint8_t)(chunk & 0xff);
      buf[1] = (uint8_t)((chunk >> 8) & 0xff);
      buf[2] = (uint8_t)((chunk >> 16) & 0xff);
      buf[3] = seq++;
      if (chunk && data) std::memcpy(buf.data() + 4, data + sent, chunk);
      if (!send_raw(buf.data(), buf.size())) {
        return Result<void>(Status::IoError, "send error to MySQL/MariaDB server");
      }
      sent += chunk;
      if (chunk < kMaxClientPacketPayload) break;
      if (sent == n) {
        uint8_t tail[4] = {0, 0, 0, seq};
        if (!send_raw(tail, 4)) {
          return Result<void>(Status::IoError, "send error to MySQL/MariaDB server");
        }
        break;
      }
    }
    return Result<void>();
  }
  if (n <= 4096) {
    uint8_t stack_buf[4096 + 4];
    stack_buf[0] = (uint8_t)(n & 0xff);
    stack_buf[1] = (uint8_t)((n >> 8) & 0xff);
    stack_buf[2] = (uint8_t)((n >> 16) & 0xff);
    stack_buf[3] = seq;
    if (n > 0 && data) {
      std::memcpy(stack_buf + 4, data, n);
    }
    if (!send_raw(stack_buf, 4 + n)) {
      return Result<void>(Status::IoError, "send error to MySQL/MariaDB server");
    }
    return Result<void>();
  }
  std::vector<uint8_t> buf;
  buf.resize(4 + n);
  buf[0] = (uint8_t)(n & 0xff);
  buf[1] = (uint8_t)((n >> 8) & 0xff);
  buf[2] = (uint8_t)((n >> 16) & 0xff);
  buf[3] = seq;
  if (n > 0 && data) {
    std::memcpy(buf.data() + 4, data, n);
  }
  if (!send_raw(buf.data(), buf.size())) {
    return Result<void>(Status::IoError, "send error to MySQL/MariaDB server");
  }
  return Result<void>();
}

Result<void> MariaWireSession::send_command(uint8_t cmd, const std::string& body) {
  if (body.size() <= 4095) {
    uint8_t stack_cmd[4096];
    stack_cmd[0] = cmd;
    if (!body.empty()) {
      std::memcpy(stack_cmd + 1, body.data(), body.size());
    }
    return write_packet(stack_cmd, 1 + body.size(), 0);
  }
  std::vector<uint8_t> pkt;
  pkt.reserve(1 + body.size());
  pkt.push_back(cmd);
  pkt.insert(pkt.end(), body.begin(), body.end());
  return write_packet(pkt.data(), pkt.size(), 0);
}

Result<void> MariaWireSession::parse_handshake(const std::vector<uint8_t>& pkt) {
  if (pkt.empty() || pkt[0] == 0xff) {
    std::string err;
    if (pkt.size() >= 3) err.assign((const char*)&pkt[1], pkt.size() - 1);
    return Result<void>(Status::Denied, "server returned error during handshake: " + err);
  }
  if (pkt[0] != HANDSHAKE_PROTOCOL)
    return Result<void>(Status::Invalid, "unsupported server handshake protocol " + std::to_string(pkt[0]));

  size_t pos = 1;
  // server version (NUL-terminated)
  size_t ver_start = pos;
  while (pos < pkt.size() && pkt[pos] != 0) ++pos;
  if (pos >= pkt.size()) return Result<void>(Status::Invalid, "malformed handshake (server version)");
  server_version_.assign((const char*)&pkt[ver_start], pos - ver_start);
  ++pos;  // skip NUL

  if (pos + 4 > pkt.size()) return Result<void>(Status::Invalid, "malformed handshake (thread id)");
  thread_id_ = pkt[pos] | ((uint32_t)pkt[pos+1] << 8) | ((uint32_t)pkt[pos+2] << 16) | ((uint32_t)pkt[pos+3] << 24);
  pos += 4;

  std::array<uint8_t, 32> salt{};
  size_t salt_len = 0;
  if (pos + 8 > pkt.size()) return Result<void>(Status::Invalid, "malformed handshake (salt1)");
  for (int i = 0; i < 8; ++i) salt[i] = pkt[pos + i];
  pos += 8;
  salt_len = 8;

  if (pos >= pkt.size()) return Result<void>(Status::Invalid, "malformed handshake (filler)");
  ++pos;  // filler (0x00)

  if (pos + 2 > pkt.size()) return Result<void>(Status::Invalid, "malformed handshake (caps lower)");
  uint32_t caps = pkt[pos] | ((uint32_t)pkt[pos + 1] << 8);
  pos += 2;

  if (pos >= pkt.size()) return Result<void>(Status::Invalid, "malformed handshake (charset)");
  ++pos;  // charset

  if (pos + 2 > pkt.size()) return Result<void>(Status::Invalid, "malformed handshake (status)");
  pos += 2;  // status flags

  if (pos + 2 <= pkt.size()) {
    caps |= ((uint32_t)pkt[pos] | ((uint32_t)pkt[pos + 1] << 8)) << 16;
    pos += 2;
  }

  uint8_t auth_len = 0;
  if (pos < pkt.size()) { auth_len = pkt[pos]; ++pos; }
  pos += 10;  // reserved 10 bytes

  if (caps & CAP_SECURE_CONNECTION) {
    size_t avail = auth_len > 1 ? (size_t)(auth_len - 1 - 8) : 12;
    size_t take = std::min<size_t>(avail, 12);
    for (size_t i = 0; i < take && pos + i < pkt.size(); ++i) {
      salt[8 + i] = pkt[pos + i];
    }
    salt_len = 8 + take;
    pos += avail;
    if (pos < pkt.size() && pkt[pos] == 0) ++pos;
  }

  std::string auth_plugin = "mysql_native_password";
  if ((caps & CAP_PLUGIN_AUTH) && pos < pkt.size()) {
    const uint8_t* start = pkt.data() + pos;
    std::string plugin((const char*)start, pkt.size() - pos);
    size_t nul = plugin.find('\0');
    if (nul != std::string::npos) plugin = plugin.substr(0, nul);
    if (!plugin.empty()) auth_plugin = plugin;
  }

  auth_plugin_data_.assign(salt.begin(), salt.begin() + salt_len);
  server_auth_plugin_ = auth_plugin;
  negotiated_capabilities_ = caps;

  return Result<void>();
}

Result<void> MariaWireSession::connect(const std::string& user, const std::string& password,
                                       const std::string& database) {
  close();
  user_ = user;
  pass_ = password;
  db_ = database;

  // 1. Establish connection via hal::net_connect. A host that is a path
  // (/opt/aios/run/asql.sock) is a unix-domain socket, as for mysql -S.
  const bool unix_sock = !host_.empty() && host_[0] == '/';
  auto c = unix_sock ? hal::net_connect_unix(host_, 5000) : hal::net_connect(host_, port_, 5000);
  if (!c.ok()) {
    return Result<void>(c.status(), "failed to connect to MySQL " + (unix_sock ? "socket " + host_ : "host " + host_ + ":" + std::to_string(port_)) + ": " + c.error().message);
  }
  conn_ = std::move(*c);

  // 2. Read server handshake
  std::vector<uint8_t> pkt;
  auto r = read_packet(&pkt);
  if (!r.ok()) { close(); return Result<void>(r.status(), r.error().message); }

  auto ph = parse_handshake(pkt);
  if (!ph.ok()) { close(); return ph; }

  // 3. Build HandshakeResponse41
  uint32_t client_caps = CAP_LONG_PASSWORD | CAP_FOUND_ROWS | CAP_LONG_FLAG |
                         CAP_PROTOCOL_41 | CAP_TRANSACTIONS | CAP_SECURE_CONNECTION |
                         CAP_MULTI_RESULTS | CAP_PLUGIN_AUTH |
                         CAP_PLUGIN_AUTH_LENENC_CLIENT_DATA;
  if (!database.empty() && (negotiated_capabilities_ & CAP_CONNECT_WITH_DB)) {
    client_caps |= CAP_CONNECT_WITH_DB;
  }
  if (negotiated_capabilities_ & CAP_MULTI_STATEMENTS) client_caps |= CAP_MULTI_STATEMENTS;
  uint8_t comp = 0;
  if (comp_want_ == kWireZstd && (negotiated_capabilities_ & kCapZstd)) comp = kWireZstd;
  else if (comp_want_ && (negotiated_capabilities_ & kCapCompress)) comp = kWireZlib;
#if !defined(AIOS_HAVE_ZSTD)
  if (comp == kWireZstd) comp = (negotiated_capabilities_ & kCapCompress) ? kWireZlib : 0;
#endif
  if (comp == kWireZstd) client_caps |= kCapZstd;
  else if (comp == kWireZlib) client_caps |= kCapCompress;

  std::vector<uint8_t> auth_resp;
  if (!password.empty()) {
    if (server_auth_plugin_ == "caching_sha2_password") {
      auth_resp = caching_sha2_scramble(password, auth_plugin_data_.data(), auth_plugin_data_.size());
    } else {
      auth_resp = mysql_native_scramble(password, auth_plugin_data_.data(), auth_plugin_data_.size());
    }
  }

  std::vector<uint8_t> resp;
  put_u32(&resp, client_caps);
  put_u32(&resp, 0x01000000);  // max packet size: 16MB
  resp.push_back(0x21);         // charset: utf8_general_ci
  resp.resize(resp.size() + 23, 0);  // 23 reserved zero bytes
  resp.insert(resp.end(), user.begin(), user.end());
  resp.push_back(0);            // NUL-terminated user
  resp.push_back((uint8_t)auth_resp.size());
  resp.insert(resp.end(), auth_resp.begin(), auth_resp.end());

  if (client_caps & CAP_CONNECT_WITH_DB) {
    resp.insert(resp.end(), database.begin(), database.end());
    resp.push_back(0);
  }
  if (client_caps & CAP_PLUGIN_AUTH) {
    resp.insert(resp.end(), server_auth_plugin_.begin(), server_auth_plugin_.end());
    resp.push_back(0);
  }
  const int zstd_level = comp_want_level_ > 0 ? std::min(comp_want_level_, 22) : 3;
  if (comp == kWireZstd) resp.push_back((uint8_t)zstd_level);   // after the attributes, which this client sends none of
  // From the OK on, both ways go through compressed frames.
  auto compress_on = [&]() {
    if (!comp) return;
    comp_algo_ = comp;
    comp_level_ = comp == kWireZstd ? zstd_level : (comp_want_level_ > 0 ? comp_want_level_ : 6);
    comp_seq_ = 0;
  };

  auto wr = write_packet(resp.data(), resp.size(), 1);
  if (!wr.ok()) { close(); return wr; }

  // 4. Read auth result
  std::vector<uint8_t> reply;
  uint8_t auth_seq = 0;
  auto rr = read_packet(&reply, &auth_seq);
  if (!rr.ok()) { close(); return Result<void>(rr.status(), rr.error().message); }

  if (!reply.empty() && reply[0] == 0x00) {
    have_auth_ = true;
    compress_on();
    return Result<void>();  // OK
  }
  if (!reply.empty() && reply[0] == 0xff) {
    close();
    std::string err;
    if (reply.size() >= 3) err.assign((const char*)&reply[1], reply.size() - 1);
    return Result<void>(Status::Denied, "authentication failed: " + err);
  }
  if (!reply.empty() && reply[0] == 0xfe) {
    // AuthSwitchRequest
    size_t pos = 1;
    std::string plugin;
    while (pos < reply.size() && reply[pos] != 0) { plugin += (char)reply[pos++]; }
    if (pos < reply.size()) ++pos;
    std::vector<uint8_t> new_salt(reply.begin() + pos, reply.end());

    std::vector<uint8_t> switch_resp;
    if (plugin == "caching_sha2_password") {
      switch_resp = caching_sha2_scramble(password, new_salt.data(), new_salt.size());
    } else {
      switch_resp = mysql_native_scramble(password, new_salt.data(), new_salt.size());
    }
    auto sw_r = write_packet(switch_resp.data(), switch_resp.size(), auth_seq + 1);
    if (!sw_r.ok()) { close(); return sw_r; }

    std::vector<uint8_t> final_reply;
    auto fr = read_packet(&final_reply);
    if (!fr.ok()) { close(); return Result<void>(fr.status(), fr.error().message); }
    if (!final_reply.empty() && final_reply[0] == 0x00) {
      have_auth_ = true;
      compress_on();
      return Result<void>();
    }
    close();
    return Result<void>(Status::Denied, "auth switch failed");
  }

  close();
  return Result<void>(Status::Invalid, "unexpected auth reply: 0x" + std::to_string(reply.empty() ? 0 : (int)reply[0]));
}

Result<void> MariaWireSession::select_db(const std::string& db) {
  auto r = send_command(COM_INIT_DB, db);
  if (!r.ok()) return r;
  std::vector<uint8_t> pkt;
  auto rp = read_packet(&pkt);
  if (!rp.ok()) return Result<void>(rp.status(), rp.error().message);
  if (!pkt.empty() && pkt[0] == 0x00) {
    db_ = db;
    return Result<void>();
  }
  if (!pkt.empty() && pkt[0] == 0xff) {
    std::string err;
    if (pkt.size() >= 3) err.assign((const char*)&pkt[1], pkt.size() - 1);
    return Result<void>(Status::Denied, "select_db failed: " + err);
  }
  return Result<void>();
}

Result<void> MariaWireSession::ping() {
  auto r = send_command(COM_PING, "");
  if (!r.ok()) return r;
  std::vector<uint8_t> pkt;
  auto rp = read_packet(&pkt);
  if (!rp.ok()) return Result<void>(rp.status(), rp.error().message);
  if (!pkt.empty() && pkt[0] == 0x00) return Result<void>();
  return Result<void>(Status::IoError, "ping failed");
}

Result<MariaResult> MariaWireSession::query(const std::string& sql, const RowSink* sink) {
  MariaResult out;
  bool sink_stopped = false;
  if (!conn_ || !conn_->alive()) return Result<MariaResult>(Status::IoError, "no open MySQL session");

  Result<void> r = send_command(COM_QUERY, sql);
  if (!r.ok()) return Result<MariaResult>(r.status(), r.error().message);

  std::vector<uint8_t> pkt;
  auto rp = read_packet(&pkt);
  if (!rp.ok()) return Result<MariaResult>(rp.status(), rp.error().message);

  if (pkt.empty()) {
    out.ok = true;
    return Result<MariaResult>(out);
  }

  uint8_t first = pkt[0];
  if (first == 0x00) {
    // OK packet
    size_t pos = 1, pkt_len = pkt.size();
    auto parse_len = [&](size_t from) -> std::pair<uint64_t, size_t> {
      if (from >= pkt_len) return {0, from};
      uint8_t b = pkt[from];
      if (b < 0xfb) return {b, from + 1};
      if (b == 0xfc && from + 2 < pkt_len) return {(uint64_t)pkt[from+1] | ((uint64_t)pkt[from+2] << 8), from + 3};
      if (b == 0xfd && from + 3 < pkt_len) return {(uint64_t)pkt[from+1] | ((uint64_t)pkt[from+2] << 8) | ((uint64_t)pkt[from+3] << 16), from + 4};
      if (b == 0xfe && from + 8 < pkt_len) {
        uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= ((uint64_t)pkt[from+1+i]) << (8*i); return {v, from + 9};
      }
      return {0, from};
    };
    auto aff = parse_len(pos); out.affected_rows = aff.first; pos = aff.second;
    auto liid = parse_len(pos); out.last_insert_id = liid.first; pos = liid.second;
    out.ok = true;
    out.has_result_set = false;
    return Result<MariaResult>(out);
  }

  if (first == 0xff) {
    // ERR packet
    size_t pos = 1;
    if (pos + 2 <= pkt.size()) pos += 2;  // skip error code
    if (pos < pkt.size() && pkt[pos] == '#') pos += 6;  // skip SQLSTATE marker and 5-char code
    if (pos < pkt.size()) out.error_message.assign((const char*)&pkt[pos], pkt.size() - pos);
    out.ok = false;
    return Result<MariaResult>(out);
  }

  // Result Set: column count
  size_t col_count = first;
  out.has_result_set = true;
  out.ok = true;

  // Read column definition packets
  for (size_t i = 0; i < col_count; ++i) {
    std::vector<uint8_t> colpkt;
    auto cr = read_packet(&colpkt);
    if (!cr.ok()) return Result<MariaResult>(cr.status(), cr.error().message);
    if (!colpkt.empty()) {
      size_t p = 0;
      auto read_lenenc_field = [&](std::string* dest) {
        if (p >= colpkt.size()) return;
        uint8_t b = colpkt[p++];
        uint64_t len = b;
        if (b == 0xfc && p + 1 < colpkt.size()) { len = colpkt[p] | ((uint64_t)colpkt[p+1] << 8); p += 2; }
        else if (b == 0xfd && p + 2 < colpkt.size()) { len = colpkt[p] | ((uint64_t)colpkt[p+1] << 8) | ((uint64_t)colpkt[p+2] << 16); p += 3; }
        if (p + len <= colpkt.size()) {
          dest->assign((const char*)&colpkt[p], len);
          p += len;
        }
      };
      MariaColumn col;
      read_lenenc_field(&col.catalog);
      read_lenenc_field(&col.schema);
      read_lenenc_field(&col.table);
      read_lenenc_field(&col.org_table);
      read_lenenc_field(&col.name);
      read_lenenc_field(&col.org_name);
      out.columns.push_back(col);
    }
  }

  // Read intermediate EOF packet (0xFE)
  std::vector<uint8_t> eofpkt;
  auto er = read_packet(&eofpkt);
  if (!er.ok()) return Result<MariaResult>(er.status(), er.error().message);

  // Read rows until terminating EOF packet (0xFE) or OK packet (0x00)
  while (true) {
    std::vector<uint8_t> rowpkt;
    auto rr = read_packet(&rowpkt);
    if (!rr.ok()) return Result<MariaResult>(rr.status(), rr.error().message);
    if (rowpkt.empty()) break;

    uint8_t b = rowpkt[0];
    if (b == 0xfe && rowpkt.size() < 9) {
      break;  // EOF packet
    }
    if (b == 0xff) {
      out.ok = false;
      if (rowpkt.size() >= 3) out.error_message.assign((const char*)&rowpkt[1], rowpkt.size() - 1);
      break;
    }

    // Text Row: parse each column
    std::vector<MariaValue> row;
    size_t p = 0;
    for (size_t c = 0; c < out.columns.size(); ++c) {
      if (p >= rowpkt.size()) { row.push_back({true, ""}); continue; }
      uint8_t fb = rowpkt[p];
      if (fb == 0xfb) {
        row.push_back({true, ""});
        p += 1;
        continue;
      }
      uint64_t slen = fb;
      size_t adv = 1;
      if (fb == 0xfc && p + 2 < rowpkt.size()) { slen = rowpkt[p+1] | ((uint64_t)rowpkt[p+2] << 8); adv = 3; }
      else if (fb == 0xfd && p + 3 < rowpkt.size()) { slen = rowpkt[p+1] | ((uint64_t)rowpkt[p+2] << 8) | ((uint64_t)rowpkt[p+3] << 16); adv = 4; }
      // 0xfe carries an 8-byte length, which is the form anything over 16 MB
      // uses. Leaving it out meant the marker itself was read as the length
      // and a 20 MB value came back as 254 bytes.
      else if (fb == 0xfe && p + 8 < rowpkt.size()) {
        slen = 0;
        for (int k = 0; k < 8; ++k) slen |= (uint64_t)rowpkt[p + 1 + k] << (8 * k);
        adv = 9;
      }
      p += adv;
      if (p + slen <= rowpkt.size()) {
        row.push_back({false, std::string((const char*)&rowpkt[p], slen)});
        p += slen;
      } else {
        row.push_back({true, ""});
      }
    }
    // Streamed: each row to the caller as it arrives, nothing kept - a table
    // of millions of rows passes through in constant memory. A caller that
    // stops early still has the rest drained, so the session stays in step.
    if (sink) {
      if (!sink_stopped && !(*sink)(out.columns, row)) sink_stopped = true;
    } else {
      out.rows.push_back(std::move(row));
    }
  }

  return Result<MariaResult>(out);
}

// --- MysqlWireServer Implementation -------------------------------------------

struct MysqlWireServer::ClientSession {
  std::shared_ptr<hal::Connection> conn;
  hal::Thread thread;
  std::atomic<bool> finished{false};
};

MysqlWireServer::MysqlWireServer(Kernel& kernel, SqlDatabase& db, uint16_t port,
                                 const std::string& bind_addr)
    : kernel_(kernel), db_(db), port_(port), bind_addr_(bind_addr) {}

MysqlWireServer::~MysqlWireServer() {
  stop();
  db_.set_user_lister(nullptr);   // it reads this server's accounts
}

Result<void> MysqlWireServer::start() {
  stop();

  if (!tcp_ && unix_path_.empty()) return Result<void>(Status::Invalid, "MySQL wire server: no TCP and no unix socket to listen on");
  if (tcp_) {
    bool loopback_only = (bind_addr_ == "127.0.0.1" || bind_addr_ == "localhost");
    auto l = hal::net_listen(port_, loopback_only);
    if (!l.ok()) {
      return Result<void>(l.status(), "failed to listen for MySQL wire server: " + l.error().message);
    }
    listener_ = std::move(*l);
    port_ = listener_->port();
  }
  if (!g_wire_started_ns) g_wire_started_ns = kernel_.host().monotonic_ns();
  if (!unix_path_.empty()) {
    auto u = hal::net_listen_unix(unix_path_);
    if (!u.ok()) {
      if (listener_) listener_->close();
      listener_.reset();
      return Result<void>(u.status(), "failed to listen for MySQL wire server: " + u.error().message);
    }
    unix_listener_ = std::move(*u);
  }
  running_ = true;
  return Result<void>();
}

void MysqlWireServer::stop() {
  running_ = false;
  stop_background();
  if (listener_) {
    listener_->close();
    listener_.reset();
  }
  if (unix_listener_) {
    unix_listener_->close();
    unix_listener_.reset();
  }
  {
    hal::LockGuard lock(clients_mu_);
    for (auto& s : sessions_) {
      if (s->conn) s->conn->close();
    }
    for (auto& s : sessions_) {
      if (s->thread.joinable()) s->thread.join();
    }
    sessions_.clear();
  }
}

void MysqlWireServer::start_background() {
  if ((bg_thread_ && bg_thread_->joinable()) || (unix_thread_ && unix_thread_->joinable())) return;
  if (!is_running()) {
    auto r = start();
    if (!r.ok()) return;
  }
  running_ = true;
  if (listener_) {
    bg_thread_ = std::make_unique<hal::Thread>([this]() {
      while (running_) {
        poll_and_serve(50);
      }
    });
  }
  if (unix_listener_) {
    // Its own accept loop: one poll cannot wait on both listeners through
    // the Listener interface, and a session thread is the same either way.
    unix_thread_ = std::make_unique<hal::Thread>([this]() {
      while (running_) {
        if (unix_listener_) poll_and_serve_on(*unix_listener_, 50);
      }
    });
  }
}

void MysqlWireServer::stop_background() {
  running_ = false;
  if (bg_thread_ && bg_thread_->joinable()) {
    bg_thread_->join();
    bg_thread_.reset();
  }
  if (unix_thread_ && unix_thread_->joinable()) {
    unix_thread_->join();
    unix_thread_.reset();
  }
}

Result<void> MysqlWireServer::poll_and_serve(int timeout_ms) {
  if (!listener_) return Result<void>(Status::IoError, "server not running");
  return poll_and_serve_on(*listener_, timeout_ms);
}

void MysqlWireServer::set_max_connections(size_t max_conns) {
  db_.set_max_connections(max_conns);
}

size_t MysqlWireServer::max_connections() const {
  return db_.max_connections();
}

Result<void> MysqlWireServer::poll_and_serve_on(hal::Listener& listener, int timeout_ms) {
  auto reap_finished = [this]() {
    hal::LockGuard lock(clients_mu_);
    for (auto it = sessions_.begin(); it != sessions_.end();) {
      if ((*it)->finished.load()) {
        if ((*it)->thread.joinable()) (*it)->thread.join();
        it = sessions_.erase(it);
      } else {
        ++it;
      }
    }
  };

  // Clean up finished sessions
  reap_finished();

  auto ac = listener.accept(timeout_ms);
  if (!ac.ok()) {
    if (ac.error().status == Status::Busy) return Result<void>();
    return Result<void>(ac.status(), ac.error().message);
  }

  std::shared_ptr<hal::Connection> conn = std::move(*ac);
  const size_t max_c = max_connections();
  if (max_c > 0 && (size_t)active_clients_.load(std::memory_order_relaxed) >= max_c) {
    // Brief backlog wait (up to 50 ms) in case short-lived connections are closing.
    for (int wait_step = 0; wait_step < 10 && running_.load(std::memory_order_relaxed); ++wait_step) {
      if ((size_t)active_clients_.load(std::memory_order_relaxed) < max_c) break;
      hal::sleep_ms(5);
    }
    reap_finished();
    if ((size_t)active_clients_.load(std::memory_order_relaxed) >= max_c) {
      db_.metrics().rejected_connections.fetch_add(1, std::memory_order_relaxed);
      send_err(*conn, 0, 1040, "08004", "Too many connections");
      wire_flush(*conn);
      conn->close();
      return Result<void>();
    }
  }

  {
    hal::LockGuard lock(clients_mu_);
    auto sess = std::make_unique<ClientSession>();
    sess->conn = conn;
    ClientSession* sp = sess.get();
    sess->thread = hal::Thread([this, conn, sp]() {
      serve_client(*conn);
      wire_flush(*conn);            // a final error or OK still reaches the client
      conn->close();
      sp->finished.store(true);
    });
    sessions_.push_back(std::move(sess));
  }
  return Result<void>();
}

// A packet's length field is three bytes, so 0xFFFFFF is the most one packet
// can carry. Anything longer travels as full-sized packets until what is left
// is short, and a payload that is an exact multiple needs a trailing empty
// packet or the client keeps waiting for the rest. Without this the length
// silently lost its high bits and the client read row bytes as a header.
constexpr size_t kMaxPacketPayload = 0xFFFFFF;

// Closes the packet whose header sits at `header_idx`, once the payload after
// it is complete. The common case writes the three length bytes in place; an
// oversized payload is lifted out and re-framed as a chain.
static void close_stream_packet(std::vector<uint8_t>& buf, size_t header_idx, uint8_t& seq) {
  size_t payload_len = buf.size() - header_idx - 4;
  if (payload_len < kMaxPacketPayload) {
    buf[header_idx] = (uint8_t)(payload_len & 0xff);
    buf[header_idx + 1] = (uint8_t)((payload_len >> 8) & 0xff);
    buf[header_idx + 2] = (uint8_t)((payload_len >> 16) & 0xff);
    buf[header_idx + 3] = seq++;
    return;
  }
  std::vector<uint8_t> payload(buf.begin() + (long)header_idx + 4, buf.end());
  buf.resize(header_idx);
  size_t sent = 0;
  while (true) {
    size_t chunk = std::min(kMaxPacketPayload, payload.size() - sent);
    size_t at = buf.size();
    buf.insert(buf.end(), 4, 0);
    buf.insert(buf.end(), payload.begin() + (long)sent, payload.begin() + (long)(sent + chunk));
    buf[at] = (uint8_t)(chunk & 0xff);
    buf[at + 1] = (uint8_t)((chunk >> 8) & 0xff);
    buf[at + 2] = (uint8_t)((chunk >> 16) & 0xff);
    buf[at + 3] = seq++;
    sent += chunk;
    if (chunk < kMaxPacketPayload) break;
    if (sent == payload.size()) {
      size_t tail = buf.size();
      buf.insert(buf.end(), 4, 0);
      buf[tail + 3] = seq++;
      break;
    }
  }
}

namespace {

// What each column is, as a client reads it from the definition. A column is
// advertised as a number or a date only if every value in it reads as one:
// this engine keeps what was written (a legacy '' in an INT column), and a
// strict driver parsing "an INT" would fail on it where MariaDB never
// produces one. Anything else stays a string. The binary protocol encodes
// each cell by this same type, so the two paths cannot disagree.
struct WireCol { uint8_t type; uint8_t charset; uint32_t length; uint16_t flags; uint8_t decimals; };

std::vector<WireCol> wire_columns(const SqlQueryResult& res, size_t num_cols) {
  const uint8_t def_charset = 45;
  std::vector<WireCol> wcols(num_cols, WireCol{MYSQL_TYPE_VAR_STRING, def_charset, 255, 0, 0});
  for (size_t c = 0; c < num_cols; ++c) {
    const SqlDataType dt = c < res.column_types.size() ? res.column_types[c] : SqlDataType::Text;
    const SqlColumnOrigin* org = c < res.column_origins.size() ? &res.column_origins[c] : nullptr;
    const uint8_t temporal = org ? org->temporal : 0;
    const bool numeric_col = sql_type_numeric(dt);
    // Declared BLOB / BINARY: bytes to the client, as MariaDB reports them.
    if (!numeric_col && org && org->binary == 1) { wcols[c] = {MYSQL_TYPE_BLOB, 63, 65535, (uint16_t)(0x10 | 0x80), 0}; continue; }
    if (!numeric_col && org && org->binary == 2) { wcols[c] = {MYSQL_TYPE_VAR_STRING, 63, 255, 0x80, 0}; continue; }
    if (!numeric_col && !(temporal >= 1 && temporal <= 3)) {
      if (org && org->mysql_type == 13) {   // YEAR: a number to the client, four digits
        wcols[c] = {MYSQL_TYPE_YEAR, 63, 4, 0x8060, 0};
        continue;
      }
      if (org && org->mysql_type == 252) {
        wcols[c] = {MYSQL_TYPE_BLOB, def_charset, 65535, 0x0010, 0};
      } else if (org && org->mysql_type == 254) {
        wcols[c] = {MYSQL_TYPE_STRING, def_charset, 255, 0, 0};
      } else {
        wcols[c] = {MYSQL_TYPE_VAR_STRING, def_charset, 255, 0, 0};
      }
      continue;
    }
    bool ok = true;
    int max_frac = 0;
    for (const auto& row : res.rows) {
      if (c >= row.values.size() || sql_is_null(row.values[c])) continue;
      const std::string& v = row.values[c];
      if (numeric_col) {
        int kind = 0, frac = 0;
        if (!wire_number(v, &kind, &frac)) { ok = false; break; }
        if (dt == SqlDataType::Int && kind != 1) { ok = false; break; }
        if (dt == SqlDataType::Decimal && kind == 3) { ok = false; break; }
        if (frac > max_frac) max_frac = frac;
      } else if (!wire_temporal(v, temporal)) { ok = false; break; }
    }
    if (!ok) continue;
    if (numeric_col) {
      // NUM_FLAG only: MariaDB does not mark a number BINARY (it does a date).
      if (dt == SqlDataType::Int) {
        // The declared width as MariaDB reports it (display digits plus a sign, none when UNSIGNED).
        const uint8_t mtype = (org && org->mysql_type != 0) ? org->mysql_type : MYSQL_TYPE_LONGLONG;
        const bool uns = org && org->is_unsigned;
        uint32_t len = 20;
        if (mtype == MYSQL_TYPE_TINY) len = uns ? 3 : 4;
        else if (mtype == MYSQL_TYPE_SHORT) len = uns ? 5 : 6;
        else if (mtype == MYSQL_TYPE_INT24) len = uns ? 8 : 9;
        else if (mtype == MYSQL_TYPE_LONG) len = uns ? 10 : 11;
        wcols[c] = {mtype, 63, len, (uint16_t)(0x8000 | (uns ? 0x0020 : 0)), 0};
      } else if (dt == SqlDataType::Real) {
        if (org && org->mysql_type == 4) wcols[c] = {MYSQL_TYPE_FLOAT, 63, 12, 0x8000, 31};
        else wcols[c] = {MYSQL_TYPE_DOUBLE, 63, 22, 0x8000, 31};
      } else {
        const int scale = std::max(max_frac, org && org->scale >= 0 ? org->scale : 0);
        wcols[c] = {MYSQL_TYPE_NEWDECIMAL, 63, 67, 0x8000, (uint8_t)std::min(scale, 30)};
      }
    } else if (temporal == 1) wcols[c] = {MYSQL_TYPE_DATE, 63, 10, 0x80, 0};
    else {
      // DATETIME(n) / TIMESTAMP(n) / TIME(n): the declared places, which a client formats the value by
      // (decimals 0 made mysqlnd drop the .678 of a DATETIME(3)).
      const uint8_t fsp = (uint8_t)std::min(org && org->scale > 0 ? org->scale : 0, 6);
      if (temporal == 2) wcols[c] = {org && org->mysql_type == 7 ? MYSQL_TYPE_TIMESTAMP : MYSQL_TYPE_DATETIME, 63, (uint32_t)(19 + (fsp ? fsp + 1 : 0)), 0x80, fsp};
      else wcols[c] = {MYSQL_TYPE_TIME, 63, (uint32_t)(10 + (fsp ? fsp + 1 : 0)), 0x80, fsp};
    }
  }
  // What the column is in its table: NOT NULL (a client that sizes "NULL" into a nullable column padded every
  // primary key to four), PRI_KEY, AUTO_INCREMENT - as MariaDB flags them.
  for (size_t c = 0; c < num_cols; ++c) {
    const SqlColumnOrigin* org = c < res.column_origins.size() ? &res.column_origins[c] : nullptr;
    if (!org) continue;
    if (org->not_null) wcols[c].flags |= 0x0001;
    if (org->org_table.empty()) continue;
    if (org->primary) wcols[c].flags |= 0x0002;
    if (org->auto_inc) wcols[c].flags |= 0x0200;
  }
  return wcols;
}

inline unsigned two_digits(std::string_view v, size_t i) {
  if (i + 1 >= v.size()) return 0;
  return (unsigned)(v[i] - '0') * 10u + (unsigned)(v[i + 1] - '0');
}

// Fraction digits after `i` (which is past the '.') as microseconds.
inline uint32_t micros_from(std::string_view v, size_t i) {
  uint32_t us = 0;
  int n = 0;
  for (; i < v.size() && n < 6; ++i, ++n) us = us * 10u + (uint32_t)(v[i] - '0');
  for (; n < 6; ++n) us *= 10u;
  return us;
}

inline long long parse_wire_i64(std::string_view v) {
  if (v.empty()) return 0;
  long long x = 0;
  bool neg = false;
  size_t i = 0;
  if (v[0] == '-') { neg = true; i = 1; }
  else if (v[0] == '+') { i = 1; }
  for (; i < v.size(); ++i) {
    char c = v[i];
    if (c >= '0' && c <= '9') x = x * 10 + (c - '0');
    else break;
  }
  return neg ? -x : x;
}

// A cell in the binary row format, by the type its column definition
// carries. The value already passed wire_number / wire_temporal for that
// type (wire_columns only advertises a type every value satisfies).
void put_binary_cell(std::vector<uint8_t>* b, std::string_view v, const WireCol& wc) {
  switch (wc.type) {
    case MYSQL_TYPE_TINY: {
      const long long x = parse_wire_i64(v);
      b->push_back((uint8_t)x);
      return;
    }
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_YEAR: {
      const long long x = parse_wire_i64(v);
      put_u16(b, (uint16_t)x);
      return;
    }
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG: {
      const long long x = parse_wire_i64(v);
      put_u32(b, (uint32_t)x);
      return;
    }
    case MYSQL_TYPE_LONGLONG: {
      if (wc.flags & 0x0020) {   // UNSIGNED: past INT64_MAX as well
        uint64_t x = 0;
        for (char ch : v) { if (ch < '0' || ch > '9') break; x = x * 10 + (uint64_t)(ch - '0'); }
        put_u64(b, x);
        return;
      }
      put_u64(b, (uint64_t)parse_wire_i64(v));
      return;
    }
    case MYSQL_TYPE_FLOAT: {
      char buf[64];
      size_t len = std::min(v.size(), sizeof(buf) - 1);
      std::memcpy(buf, v.data(), len);
      buf[len] = '\0';
      const float f = std::strtof(buf, nullptr);
      uint32_t bits;
      std::memcpy(&bits, &f, 4);
      put_u32(b, bits);
      return;
    }
    case MYSQL_TYPE_DOUBLE: {
      char buf[64];
      size_t len = std::min(v.size(), sizeof(buf) - 1);
      std::memcpy(buf, v.data(), len);
      buf[len] = '\0';
      const double d = std::strtod(buf, nullptr);
      uint64_t bits;
      std::memcpy(&bits, &d, 8);
      put_u64(b, bits);
      return;
    }
    case MYSQL_TYPE_DATE: {
      const unsigned y = (unsigned)two_digits(v, 0) * 100u + two_digits(v, 2), m = two_digits(v, 5), d = two_digits(v, 8);
      if (y == 0 && m == 0 && d == 0) { b->push_back(0); return; }
      b->push_back(4);
      put_u16(b, y);
      b->push_back((uint8_t)m);
      b->push_back((uint8_t)d);
      return;
    }
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_DATETIME: {
      const unsigned y = (unsigned)two_digits(v, 0) * 100u + two_digits(v, 2), m = two_digits(v, 5), d = two_digits(v, 8);
      const unsigned hh = two_digits(v, 11), mi = two_digits(v, 14), ss = two_digits(v, 17);
      const uint32_t us = v.size() > 20 ? micros_from(v, 20) : 0;
      if (y == 0 && m == 0 && d == 0 && hh == 0 && mi == 0 && ss == 0 && us == 0) { b->push_back(0); return; }
      b->push_back(us ? 11 : ((hh | mi | ss) ? 7 : 4));
      put_u16(b, y);
      b->push_back((uint8_t)m);
      b->push_back((uint8_t)d);
      if (!(hh | mi | ss) && !us) return;
      b->push_back((uint8_t)hh);
      b->push_back((uint8_t)mi);
      b->push_back((uint8_t)ss);
      if (us) put_u32(b, us);
      return;
    }
    case MYSQL_TYPE_TIME: {
      size_t i = 0;
      const bool neg = !v.empty() && v[0] == '-';
      if (neg) i = 1;
      unsigned hours = 0;
      while (i < v.size() && v[i] != ':') hours = hours * 10u + (unsigned)(v[i++] - '0');
      const unsigned mi = two_digits(v, i + 1), ss = two_digits(v, i + 4);
      const uint32_t us = v.size() > i + 7 ? micros_from(v, i + 7) : 0;
      if (hours == 0 && mi == 0 && ss == 0 && us == 0) { b->push_back(0); return; }
      b->push_back(us ? 12 : 8);
      b->push_back(neg ? 1 : 0);
      put_u32(b, hours / 24u);
      b->push_back((uint8_t)(hours % 24u));
      b->push_back((uint8_t)mi);
      b->push_back((uint8_t)ss);
      if (us) put_u32(b, us);
      return;
    }
    default:
      put_lenenc_str(b, v);
  }
}

// One ColumnDefinition41 packet body (no packet header) for column `c` of
// `res`, appended to `out`. Shared by the result-set stream and the
// COM_STMT_PREPARE reply, so a client sees the same definition at prepare and
// at execute.
inline void put_column_def(std::vector<uint8_t>& out, const SqlQueryResult& res, size_t c, const WireCol& wc,
                           const std::string& name, const std::string& eff_db) {
  static const std::string kDef = "def";
  static const std::string kNone;
  const SqlColumnOrigin* org = c < res.column_origins.size() ? &res.column_origins[c] : nullptr;
  const bool from_table = org && !org->org_table.empty();
  put_lenenc_str(&out, kDef);
  put_lenenc_str(&out, from_table ? (org->schema.empty() ? eff_db : org->schema) : kNone);
  put_lenenc_str(&out, from_table ? org->table : kNone);
  put_lenenc_str(&out, from_table ? org->org_table : kNone);
  put_lenenc_str(&out, name);
  put_lenenc_str(&out, from_table ? org->org_name : kNone);
  const uint8_t fixed[13] = {0x0c,                                  // len of fixed fields
                             wc.charset, 0,                         // 33 utf8_general_ci, 63 binary
                             (uint8_t)(wc.length & 0xff), (uint8_t)((wc.length >> 8) & 0xff),
                             (uint8_t)((wc.length >> 16) & 0xff), (uint8_t)((wc.length >> 24) & 0xff),
                             wc.type,
                             (uint8_t)(wc.flags & 0xff), (uint8_t)(wc.flags >> 8),
                             wc.decimals,
                             0, 0};                                 // filler
  out.insert(out.end(), fixed, fixed + sizeof(fixed));
}

// The column definitions a COM_STMT_PREPARE reply announces for `sql`, each
// as a packet body; empty when the statement's columns are only known when it
// runs (SHOW, CALL, ...) or the probe fails. MariaDB describes a prepared
// SELECT at prepare time and clients lean on it: sysbench decides from the
// prepare-time column count whether to read a result set at all, libmysqlclient
// binds result buffers against it. The columns come from running the
// statement with every placeholder NULL and LIMIT 0, which touches no row on
// an indexed probe and costs a scan with a NULL predicate otherwise; the
// answer is kept per (database, statement) until the next DDL.
struct PreparedColumns {
  uint64_t schema_gen = 0;
  std::vector<std::vector<uint8_t>> defs;
};
inline bool prepare_describes(const std::string& sql) {
  size_t i = 0;
  for (;;) {
    while (i < sql.size() && (std::isspace((unsigned char)sql[i]) || sql[i] == '(')) ++i;
    if (i + 1 < sql.size() && sql[i] == '/' && sql[i + 1] == '*') {
      const size_t e = sql.find("*/", i + 2);
      if (e == std::string::npos) return false;
      i = e + 2;
      continue;
    }
    break;
  }
  size_t j = i;
  while (j < sql.size() && std::isalpha((unsigned char)sql[j])) ++j;
  std::string w = sql.substr(i, j - i);
  for (auto& ch : w) ch = (char)std::toupper((unsigned char)ch);
  return w == "SELECT" || w == "WITH" || w == "VALUES";
}
inline std::vector<std::vector<uint8_t>> prepared_result_columns(SqlDatabase& db, const std::string& sql,
                                                                 const std::string& session_db, const std::string& wire_db) {
  static hal::Mutex cache_mu;
  static std::unordered_map<std::string, PreparedColumns> cache;
  constexpr size_t kCacheCap = 8192;
  if (!prepare_describes(sql)) return {};
  const uint64_t gen = db.schema_generation();
  const std::string key = session_db + '\0' + sql;
  {
    hal::LockGuard lk(cache_mu);
    auto it = cache.find(key);
    if (it != cache.end() && it->second.schema_gen == gen) return it->second.defs;
  }
  // Every placeholder NULL, quote-aware, the way execute binds values.
  std::string null_sql;
  null_sql.reserve(sql.size() + 16);
  {
    bool in_quote = false;
    char quote_char = 0;
    for (size_t i = 0; i < sql.size(); ++i) {
      const char c = sql[i];
      if (in_quote) {
        null_sql.push_back(c);
        if (c == '\\' && quote_char != '`' && i + 1 < sql.size()) { null_sql.push_back(sql[++i]); continue; }
        if (c == quote_char) in_quote = false;
      } else if (c == '\'' || c == '"' || c == '`') {
        in_quote = true;
        quote_char = c;
        null_sql.push_back(c);
      } else if (c == '?') {
        null_sql += "NULL";
      } else {
        null_sql.push_back(c);
      }
    }
  }
  while (!null_sql.empty() && (std::isspace((unsigned char)null_sql.back()) || null_sql.back() == ';')) null_sql.pop_back();
  // Appended LIMIT 0 keeps the origins (schema, table, original name) a
  // client shows; a statement that has its own LIMIT, locks rows or selects
  // INTO is probed as a derived table instead.
  std::string up;
  up.reserve(null_sql.size());
  for (char c : null_sql) up.push_back((char)std::toupper((unsigned char)c));
  auto has_word = [&](const char* w) {
    const size_t n = std::strlen(w);
    for (size_t pos = up.find(w); pos != std::string::npos; pos = up.find(w, pos + 1)) {
      const bool l = pos == 0 || !(std::isalnum((unsigned char)up[pos - 1]) || up[pos - 1] == '_');
      const bool r = pos + n >= up.size() || !(std::isalnum((unsigned char)up[pos + n]) || up[pos + n] == '_');
      if (l && r) return true;
    }
    return false;
  };
  std::vector<std::string> probes;
  if (!has_word("LIMIT") && !has_word("UPDATE") && !has_word("SHARE") && !has_word("INTO"))
    probes.push_back(null_sql + " LIMIT 0");
  probes.push_back("SELECT * FROM (" + null_sql + ") AS asql_prepare_probe LIMIT 0");
  std::vector<std::vector<uint8_t>> defs;
  for (const auto& probe : probes) {
    sql_set_wire_results(true);
    const std::shared_ptr<const SqlQueryResult> res = db.execute_gateway_query_shared(probe, session_db);
    sql_set_wire_results(false);
    if (!res || !res->success || !res->is_result_set || res->columns.empty()) continue;
    const size_t n = res->columns.size();
    const std::vector<WireCol> wcols = wire_columns(*res, n);
    defs.reserve(n);
    for (size_t c = 0; c < n; ++c) {
      std::vector<uint8_t> body;
      put_column_def(body, *res, c, wcols[c], res->columns[c], wire_db);
      defs.push_back(std::move(body));
    }
    break;
  }
  if (defs.empty()) return {};
  hal::LockGuard lk(cache_mu);
  if (cache.size() >= kCacheCap) cache.clear();
  cache[key] = PreparedColumns{gen, defs};
  return defs;
}

// One result set onto the wire, column count through terminating EOF, in the
// text (COM_QUERY) or binary (COM_STMT_EXECUTE) row format. `queue` takes
// each chunk as it fills; `status` is the server-status word of the closing
// EOF (SERVER_MORE_RESULTS_EXISTS when another statement follows). When
// `capture` is given the whole stream is also kept there, up to a cap;
// *captured says whether it holds all of it. Returns false on a send error.
constexpr size_t kWireChunkBufferSize = 64 * 1024;
constexpr size_t kWireCaptureCap = 4u << 20;

thread_local std::vector<uint8_t> tl_stream_buf;
thread_local std::vector<uint8_t> tl_sink_buf;

template <class Queue, class FlushNow = std::nullptr_t>
bool stream_result_set(const SqlQueryResult& res, bool binary, bool cs_out_latin1, const std::string& eff_db,
                       uint16_t status, uint8_t& seq, std::string* capture, bool* captured, Queue&& queue,
                       FlushNow&& flush_now = nullptr) {
  // The answer may be the result cache's own entry (shared with every other
  // session): read, never changed. A result with no column at all is sent
  // with one, named "result".
  static const std::string kResultCol = "result";
  size_t num_cols = res.columns.size();
  const bool unnamed = num_cols == 0;
  if (unnamed) num_cols = 1;
  auto column_name = [&](size_t c) -> const std::string& { return unnamed ? kResultCol : res.columns[c]; };
  // Column count, column definitions and EOF are written straight into the
  // buffer the rows stream through: a vector per definition and a copy per
  // packet cost more than the query for a wide cached row.
  // One buffer per thread, kept between statements: 68 KB allocated and
  // freed per result set was page-faulted afresh each time.
  std::vector<uint8_t>& stream_buf = tl_stream_buf;
  stream_buf.clear();
  if (stream_buf.capacity() < kWireChunkBufferSize + 4096) stream_buf.reserve(kWireChunkBufferSize + 4096);
  bool capturing = capture != nullptr;
  if (capture) capture->clear();
  auto flush_stream = [&]() -> bool {
    if (stream_buf.empty()) return true;
    if (capturing) {
      if (capture->size() + stream_buf.size() > kWireCaptureCap) {
        capturing = false;
        capture->clear();
        capture->shrink_to_fit();
      } else {
        capture->append(reinterpret_cast<const char*>(stream_buf.data()), stream_buf.size());
      }
    }
    const bool ok = queue(stream_buf.data(), stream_buf.size());
    if (!ok) capturing = false;
    stream_buf.clear();
    return ok;
  };
  auto open_packet = [&]() {
    const size_t h = stream_buf.size();
    stream_buf.insert(stream_buf.end(), 4, 0);
    return h;
  };
  auto put_eof = [&]() {
    const size_t h = open_packet();
    const uint8_t eof[] = {0xfe, 0x00, 0x00, (uint8_t)(status & 0xff), (uint8_t)(status >> 8)};
    stream_buf.insert(stream_buf.end(), eof, eof + sizeof(eof));
    close_stream_packet(stream_buf, h, seq);
  };

  {
    const size_t h = open_packet();
    put_lenenc_u64(&stream_buf, num_cols);
    close_stream_packet(stream_buf, h, seq);
  }
  const std::vector<WireCol> wcols = wire_columns(res, num_cols);
  for (size_t c = 0; c < num_cols; ++c) {
    const size_t h = open_packet();
    put_column_def(stream_buf, res, c, wcols[c], column_name(c), eff_db);
    close_stream_packet(stream_buf, h, seq);
    if (stream_buf.size() >= kWireChunkBufferSize && !flush_stream()) return false;
  }
  // The EOF after the definitions carries no "more results" bit; only the
  // one that ends the rows does.
  {
    const size_t h = open_packet();
    const uint8_t eof[] = {0xfe, 0x00, 0x00, 0x02, 0x00};
    stream_buf.insert(stream_buf.end(), eof, eof + sizeof(eof));
    close_stream_packet(stream_buf, h, seq);
    if (!flush_stream()) return false;
    if constexpr (!std::is_same_v<std::decay_t<FlushNow>, std::nullptr_t>) {
      if (!flush_now()) return false;
    }
  }

  const size_t null_bytes = (num_cols + 7 + 2) / 8;
  for (const auto& row : res.rows) {
    const size_t header_idx = open_packet();
    if (binary) {
      // Binary row: 0x00, a NULL bitmap with a two-bit offset, then each
      // non-NULL cell in its column's binary form.
      stream_buf.push_back(0x00);
      const size_t bitmap_at = stream_buf.size();
      stream_buf.insert(stream_buf.end(), null_bytes, 0);
      for (size_t c = 0; c < num_cols; ++c) {
        if (c >= row.values.size() || sql_is_null(row.values[c])) {
          stream_buf[bitmap_at + (c + 2) / 8] |= (uint8_t)(1u << ((c + 2) % 8));
          continue;
        }
        const std::string& val = row.values[c];
        if (cs_out_latin1 && wcols[c].charset != 63 && has_high_byte(val)) put_lenenc_str(&stream_buf, utf8_to_cp1252(val));
        else put_binary_cell(&stream_buf, val, wcols[c]);
      }
    } else {
      for (size_t c = 0; c < num_cols; ++c) {
        if (c >= row.values.size() || sql_is_null(row.values[c])) {
          stream_buf.push_back(0xfb);  // SQL NULL marker
          continue;
        }
        const std::string& val = row.values[c];
        if (cs_out_latin1 && wcols[c].charset != 63 && has_high_byte(val)) put_lenenc_str(&stream_buf, utf8_to_cp1252(val));
        else put_lenenc_str(&stream_buf, val);
      }
    }
    close_stream_packet(stream_buf, header_idx, seq);
    if (stream_buf.size() >= kWireChunkBufferSize && !flush_stream()) return false;
  }
  put_eof();
  const bool ok = flush_stream();
  if (tl_stream_buf.capacity() > kWireChunkBufferSize + 4096) {
    std::vector<uint8_t>().swap(tl_stream_buf);
  }
  if (captured) *captured = ok && capturing;
  return ok;
}

// The wire's SqlRowSink: the same framing as stream_result_set, fed a row
// at a time by the engine, cells as views of the column's own bytes. One
// copy per cell, into the packet; no row of strings built and freed.
class WireRowSink final : public SqlRowSink {
 public:
  WireRowSink(bool binary, bool latin1, std::string eff_db, uint16_t status, uint8_t& seq,
              std::function<bool(const uint8_t*, size_t)> queue,
              std::function<bool()> flush_now = nullptr)
      : binary_(binary), latin1_(latin1), eff_db_(std::move(eff_db)), status_(status), seq_(seq),
        queue_(std::move(queue)), flush_now_(std::move(flush_now)) {}
  ~WireRowSink() override {
    if (tl_sink_buf.capacity() > kWireChunkBufferSize + 4096) {
      std::vector<uint8_t>().swap(tl_sink_buf);
    }
  }
  bool used() const { return used_; }
  bool ok() const { return ok_; }

  bool begin(const SqlQueryResult& header) override {
    used_ = true;
    buf_ = &tl_sink_buf;
    buf_->clear();
    if (buf_->capacity() < kWireChunkBufferSize + 4096) buf_->reserve(kWireChunkBufferSize + 4096);
    ncols_ = header.columns.size();
    const bool unnamed = ncols_ == 0;
    if (unnamed) ncols_ = 1;
    wcols_ = wire_columns(header, ncols_);
    {
      const size_t h = open();
      put_lenenc_u64(buf_, ncols_);
      close_stream_packet(*buf_, h, seq_);
    }
    static const std::string kResultCol = "result";
    for (size_t c = 0; c < ncols_; ++c) {
      const size_t h = open();
      put_column_def(*buf_, header, c, wcols_[c], unnamed ? kResultCol : header.columns[c], eff_db_);
      close_stream_packet(*buf_, h, seq_);
    }
    {
      const size_t h = open();
      const uint8_t eof[] = {0xfe, 0x00, 0x00, 0x02, 0x00};
      buf_->insert(buf_->end(), eof, eof + sizeof(eof));
      close_stream_packet(*buf_, h, seq_);
    }
    null_bytes_ = (ncols_ + 7 + 2) / 8;
    flush();
    if (flush_now_) flush_now_();
    return ok_;
  }

  bool row(const std::string_view* cells, const uint8_t* null, size_t n) override {
    if (!ok_) return false;
    const size_t h = open();
    if (binary_) {
      buf_->push_back(0x00);
      const size_t bitmap_at = buf_->size();
      buf_->insert(buf_->end(), null_bytes_, 0);
      for (size_t c = 0; c < ncols_; ++c) {
        if (c >= n || null[c]) { (*buf_)[bitmap_at + (c + 2) / 8] |= (uint8_t)(1u << ((c + 2) % 8)); continue; }
        put_cell(cells[c], wcols_[c], true);
      }
    } else {
      for (size_t c = 0; c < ncols_; ++c) {
        if (c >= n || null[c]) { buf_->push_back(0xfb); continue; }
        put_cell(cells[c], wcols_[c], false);
      }
    }
    close_stream_packet(*buf_, h, seq_);
    if (buf_->size() >= kWireChunkBufferSize) flush();
    return ok_;
  }

  bool end() override {
    if (!ok_) return false;
    const size_t h = open();
    const uint8_t eof[] = {0xfe, 0x00, 0x00, (uint8_t)(status_ & 0xff), (uint8_t)(status_ >> 8)};
    buf_->insert(buf_->end(), eof, eof + sizeof(eof));
    close_stream_packet(*buf_, h, seq_);
    flush();
    if (tl_sink_buf.capacity() > kWireChunkBufferSize + 4096) {
      std::vector<uint8_t>().swap(tl_sink_buf);
    }
    return ok_;
  }

 private:
  size_t open() {
    const size_t h = buf_->size();
    buf_->insert(buf_->end(), 4, 0);
    return h;
  }
  void flush() {
    if (buf_->empty()) return;
    if (!queue_(buf_->data(), buf_->size())) ok_ = false;
    buf_->clear();
  }
  void put_cell(std::string_view v, const WireCol& wc, bool binary) {
    const bool text_type = wc.type == MYSQL_TYPE_VAR_STRING || wc.type == MYSQL_TYPE_BLOB || wc.type == MYSQL_TYPE_STRING ||
                           wc.type == MYSQL_TYPE_NEWDECIMAL;
    if (binary && !text_type) { put_binary_cell(buf_, v, wc); return; }
    if (latin1_ && wc.charset != 63 && has_high_byte(v)) {
      put_lenenc_str(buf_, utf8_to_cp1252(v));
      return;
    }
    put_lenenc_u64(buf_, v.size());
    put_raw(buf_, v.data(), v.size());
  }

  bool binary_, latin1_;
  std::string eff_db_;
  uint16_t status_;
  uint8_t& seq_;
  std::function<bool(const uint8_t*, size_t)> queue_;
  std::function<bool()> flush_now_;
  std::vector<uint8_t>* buf_ = nullptr;
  std::vector<WireCol> wcols_;
  size_t ncols_ = 0, null_bytes_ = 0;
  bool used_ = false, ok_ = true;
};

aios::hal::Mutex& slave_registry_mu() {
  static aios::hal::Mutex mu;
  return mu;
}

std::unordered_map<uint32_t, MysqlWireServer::SlaveInfo>& slave_registry_map() {
  static std::unordered_map<uint32_t, MysqlWireServer::SlaveInfo> map;
  return map;
}
}  // namespace

void MysqlWireServer::register_slave(const SlaveInfo& s) {
  std::lock_guard<aios::hal::Mutex> lk(slave_registry_mu());
  slave_registry_map()[s.server_id] = s;
}

void MysqlWireServer::unregister_slave(uint32_t server_id) {
  std::lock_guard<aios::hal::Mutex> lk(slave_registry_mu());
  slave_registry_map().erase(server_id);
}

std::vector<MysqlWireServer::SlaveInfo> MysqlWireServer::get_registered_slaves() {
  std::lock_guard<aios::hal::Mutex> lk(slave_registry_mu());
  std::vector<SlaveInfo> res;
  res.reserve(slave_registry_map().size());
  for (const auto& kv : slave_registry_map()) {
    res.push_back(kv.second);
  }
  std::sort(res.begin(), res.end(), [](const SlaveInfo& a, const SlaveInfo& b) {
    return a.server_id < b.server_id;
  });
  return res;
}

// A reply leaves in one write. It used to go out as one write per packet -
// column count, each column definition, EOF, rows - and through anything
// that re-segments the stream (an SSH tunnel, which is how DbStudio reaches
// a mirror) each became its own WAN packet held back behind the previous
// one's delayed ACK: ~3 round trips a query where MariaDB takes 1 (on st via
// a tunnel from arch, 120 ms against 43 ms for a COUNT(*)). Packets queue per
// client thread; recv_packet flushes before it waits, so every answer is
// complete, and anything past 256 KB streams out as it fills.
namespace {
thread_local hal::Connection* tl_wire_conn = nullptr;
thread_local std::vector<uint8_t> tl_wire_out;

// The compressed protocol, once a session that asked for it has logged in
// (CLIENT_COMPRESS: zlib; CLIENT_ZSTD_COMPRESSION_ALGORITHM: zstd). Every
// frame is a 7-byte header - compressed length (3), the compressed sequence
// id, the plain length (3; 0: the payload is stored as it is) - over a piece
// of the ordinary packet stream, cut wherever the frame ends. The id runs on
// from the client's last frame, which starts each command at 0.
struct WireCompress {
  const hal::Connection* conn = nullptr;   // the session it is on; nullptr: off
  uint8_t algo = 0;                        // kWireZlib / kWireZstd
  int level = 6;
  uint8_t seq = 0;                         // the next frame's id
  std::vector<uint8_t> in;                 // inflated, not yet read
  size_t in_at = 0;
  std::vector<uint8_t> out, frame;         // scratch
};
thread_local WireCompress tl_wz;
// The packet stream's next `n` bytes into `dst`, through the compressed frames when the session has them. The
// first frame header waits `timeout_ms` (recv_packet's poll); the rest of a frame is already on its way.
bool wire_read(hal::Connection& conn, uint8_t* dst, size_t n, int timeout_ms, std::atomic<uint64_t>& rx) {
  WireCompress& z = tl_wz;
  if (z.conn != &conn) return conn.recv_all(MutableByteSpan(dst, n), timeout_ms);
  while (n > 0) {
    if (z.in_at == z.in.size()) {
      uint8_t h[7];
      if (!conn.recv_all(MutableByteSpan(h, 7), timeout_ms)) return false;
      const size_t clen = get_u24(h), plain = get_u24(h + 4);
      z.seq = (uint8_t)(h[3] + 1);
      z.frame.resize(clen);
      if (clen && !conn.recv_all(MutableByteSpan(z.frame.data(), clen), 10000)) return false;
      rx.fetch_add(7 + clen, std::memory_order_relaxed);
      z.in_at = 0;
      if (plain == 0) {
        z.in.swap(z.frame);
      } else {
        z.in.resize(plain);
        if (!wire_inflate(z.algo, z.frame.data(), clen, z.in.data(), plain)) { conn.close(); return false; }
      }
      if (z.frame.capacity() > (64u << 10)) std::vector<uint8_t>().swap(z.frame);
      timeout_ms = 10000;
      continue;
    }
    const size_t take = std::min(n, z.in.size() - z.in_at);
    std::memcpy(dst, z.in.data() + z.in_at, take);
    z.in_at += take;
    dst += take;
    n -= take;
  }
  if (z.in_at >= z.in.size() && z.in.capacity() > (64u << 10)) {
    std::vector<uint8_t>().swap(z.in);
    z.in_at = 0;
  }
  return true;
}

void wire_shrink_statement() {
  constexpr size_t kSmallCap = 64 * 1024;
  if (tl_wire_out.capacity() > kSmallCap) std::vector<uint8_t>().swap(tl_wire_out);
  WireCompress& z = tl_wz;
  if (z.out.capacity() > kSmallCap) std::vector<uint8_t>().swap(z.out);
  if (z.frame.capacity() > kSmallCap) std::vector<uint8_t>().swap(z.frame);
  if (z.in_at >= z.in.size() && z.in.capacity() > kSmallCap) {
    std::vector<uint8_t>().swap(z.in);
    z.in_at = 0;
  }
  if (tl_stream_buf.capacity() > kWireChunkBufferSize + 4096) std::vector<uint8_t>().swap(tl_stream_buf);
  if (tl_sink_buf.capacity() > kWireChunkBufferSize + 4096) std::vector<uint8_t>().swap(tl_sink_buf);
}

void wire_shrink_idle() {
  if (tl_wire_out.capacity() > 0) std::vector<uint8_t>().swap(tl_wire_out);
  WireCompress& z = tl_wz;
  if (z.out.capacity() > 0) std::vector<uint8_t>().swap(z.out);
  if (z.frame.capacity() > 0) std::vector<uint8_t>().swap(z.frame);
  if (z.in_at >= z.in.size()) {
    std::vector<uint8_t>().swap(z.in);
    z.in_at = 0;
  }
  if (tl_stream_buf.capacity() > 0) std::vector<uint8_t>().swap(tl_stream_buf);
  if (tl_sink_buf.capacity() > 0) std::vector<uint8_t>().swap(tl_sink_buf);
}
}  // namespace

bool MysqlWireServer::wire_flush(hal::Connection& conn) {
  if (tl_wire_conn != &conn || tl_wire_out.empty()) return true;
  bool ok;
  WireCompress& z = tl_wz;
  if (z.conn == &conn) {
    z.out.clear();
    wire_frames(z.algo, z.level, &z.seq, tl_wire_out.data(), tl_wire_out.size(), z.out);
    db_.metrics().bytes_sent_compressed.fetch_add(z.out.size(), std::memory_order_relaxed);
    ok = conn.send_all(ByteSpan(z.out.data(), z.out.size()));
    if (z.out.capacity() > (64u << 10)) { std::vector<uint8_t>().swap(z.out); }
  } else {
    ok = conn.send_all(ByteSpan(tl_wire_out.data(), tl_wire_out.size()));
  }
  tl_wire_out.clear();
  if (tl_wire_out.capacity() > (64u << 10)) { std::vector<uint8_t>().swap(tl_wire_out); }
  return ok;
}

bool MysqlWireServer::wire_queue(hal::Connection& conn, ByteSpan bytes) {
  if (tl_wire_conn != &conn) {
    if (tl_wire_conn && !tl_wire_out.empty()) tl_wire_out.clear();   // a previous client's leftovers: its thread is gone
    tl_wire_conn = &conn;
  }
  tl_wire_out.insert(tl_wire_out.end(), bytes.begin(), bytes.end());
  if (tl_wire_out.size() >= (64u << 10)) return wire_flush(conn);
  return true;
}

Result<void> MysqlWireServer::send_packet(hal::Connection& conn, const uint8_t* data, size_t n, uint8_t seq) {
  db_.metrics().bytes_sent.fetch_add(4 + n, std::memory_order_relaxed);
  if (n >= kMaxPacketPayload) {
    size_t sent = 0;
    while (true) {
      size_t chunk = std::min(kMaxPacketPayload, n - sent);
      std::vector<uint8_t> buf(4 + chunk);
      buf[0] = (uint8_t)(chunk & 0xff);
      buf[1] = (uint8_t)((chunk >> 8) & 0xff);
      buf[2] = (uint8_t)((chunk >> 16) & 0xff);
      buf[3] = seq++;
      if (chunk && data) std::memcpy(buf.data() + 4, data + sent, chunk);
      if (!wire_queue(conn, ByteSpan(buf.data(), buf.size()))) {
        return Result<void>(Status::IoError, "send error");
      }
      sent += chunk;
      if (chunk < kMaxPacketPayload) break;
      if (sent == n) {
        uint8_t tail[4] = {0, 0, 0, seq};
        if (!wire_queue(conn, ByteSpan(tail, 4))) {
          return Result<void>(Status::IoError, "send error");
        }
        break;
      }
    }
    return Result<void>();
  }
  if (n <= 4096) {
    uint8_t stack_buf[4096 + 4];
    stack_buf[0] = (uint8_t)(n & 0xff);
    stack_buf[1] = (uint8_t)((n >> 8) & 0xff);
    stack_buf[2] = (uint8_t)((n >> 16) & 0xff);
    stack_buf[3] = seq;
    if (n > 0 && data) {
      std::memcpy(stack_buf + 4, data, n);
    }
    if (!wire_queue(conn, ByteSpan(stack_buf, 4 + n))) {
      return Result<void>(Status::IoError, "send error");
    }
    return Result<void>();
  }
  std::vector<uint8_t> buf;
  buf.resize(4 + n);
  buf[0] = (uint8_t)(n & 0xff);
  buf[1] = (uint8_t)((n >> 8) & 0xff);
  buf[2] = (uint8_t)((n >> 16) & 0xff);
  buf[3] = seq;
  if (n > 0 && data) {
    std::memcpy(buf.data() + 4, data, n);
  }
  if (!wire_queue(conn, ByteSpan(buf.data(), buf.size()))) {
    return Result<void>(Status::IoError, "send error");
  }
  return Result<void>();
}

Result<size_t> MysqlWireServer::recv_packet(hal::Connection& conn, std::vector<uint8_t>* out, uint8_t* seq,
                                            const std::function<void()>& on_idle) {
  // Whatever answer is queued goes out whole before we wait for the client.
  if (!wire_flush(conn)) return Result<size_t>(Status::IoError, "send error");
  out->clear();
  if (out->capacity() > (64u << 10)) {
    std::vector<uint8_t>().swap(*out);
  }
  uint8_t hdr[4];
  // The wait for the next request blocks: stop() and KILL wake it by shutting
  // the socket. A short timeout here woke every idle session ten times a
  // second (2,000 idle connections: 16% of a core). After a second idle the
  // session gives back the stack its last statement touched.
  bool idle = false;
  while (running_ && conn.alive()) {
    if (wire_read(conn, hdr, 4, idle ? 30000 : 1000, db_.metrics().bytes_received_compressed)) {
      size_t len = (size_t)hdr[0] | ((size_t)hdr[1] << 8) | ((size_t)hdr[2] << 16);
      db_.metrics().bytes_received.fetch_add(4 + len, std::memory_order_relaxed);
      if (seq) *seq = hdr[3];
      if (len > 0) {
        out->resize(len);
        if (!wire_read(conn, out->data(), len, 10000, db_.metrics().bytes_received_compressed)) {
          return Result<size_t>(Status::IoError, "client disconnected during payload");
        }
      }
      // A full-sized packet means the statement continues in the next one.
      // We advertise a 64 MB max_allowed_packet, so a client may legitimately
      // send a multi-row INSERT bigger than one packet; reading only the first
      // chunk handed the parser a statement cut off mid-value.
      while (len == kMaxPacketPayload && running_ && conn.alive()) {
        if (!wire_read(conn, hdr, 4, 10000, db_.metrics().bytes_received_compressed)) {
          return Result<size_t>(Status::IoError, "client disconnected between packets");
        }
        len = (size_t)hdr[0] | ((size_t)hdr[1] << 8) | ((size_t)hdr[2] << 16);
        db_.metrics().bytes_received.fetch_add(4 + len, std::memory_order_relaxed);
        if (len > 0) {
          size_t at = out->size();
          out->resize(at + len);
          if (!wire_read(conn, out->data() + at, len, 10000, db_.metrics().bytes_received_compressed)) {
            return Result<size_t>(Status::IoError, "client disconnected during payload");
          }
        }
      }
      return Result<size_t>(out->size());
    }
    if (!idle) {
      idle = true;
      if (out->capacity() > 0) std::vector<uint8_t>().swap(*out);
      wire_shrink_idle();
      if (on_idle) on_idle();
      db_.shrink_session_buffers(true);
      hal::release_idle_stack();
    }
  }
  return Result<size_t>(Status::IoError, "client disconnected or server stopped");
}

Result<void> MysqlWireServer::send_ok(hal::Connection& conn, uint8_t seq, uint64_t affected_rows,
                                      uint64_t last_insert_id, uint16_t status,
                                      const std::string& msg) {
  std::vector<uint8_t> pkt;
  pkt.push_back(0x00);  // OK header
  put_lenenc_u64(&pkt, affected_rows);
  put_lenenc_u64(&pkt, last_insert_id);
  put_u16(&pkt, status);    // server status: 0x0002 = AUTOCOMMIT
  put_u16(&pkt, 0);         // warning count: 0
  if (!msg.empty()) {
    put_lenenc_str(&pkt, msg);
  }
  return send_packet(conn, pkt.data(), pkt.size(), seq);
}

Result<void> MysqlWireServer::send_err(hal::Connection& conn, uint8_t seq, uint16_t err_code,
                                       const std::string& sqlstate, const std::string& msg) {
  std::vector<uint8_t> pkt;
  pkt.push_back(0xff);  // ERR header
  put_u16(&pkt, err_code);
  pkt.push_back('#');
  std::string st = sqlstate;
  while (st.size() < 5) st += '0';
  pkt.insert(pkt.end(), st.begin(), st.begin() + 5);
  pkt.insert(pkt.end(), msg.begin(), msg.end());
  return send_packet(conn, pkt.data(), pkt.size(), seq);
}

// A statement as words, for the few statements the wire layer itself answers:
// whitespace, ';' and '=' separate, and a quoted string is one word marked with
// a leading \x01 so it cannot be mistaken for a keyword.
static std::vector<std::string> sql_words(const std::string& q) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < q.size()) {
    const unsigned char c = (unsigned char)q[i];
    if (std::isspace(c) || c == ';' || c == '=') { ++i; continue; }
    if (c == '\'' || c == '"') {
      const size_t e = q.find((char)c, i + 1);
      if (e == std::string::npos) { out.push_back("\x01"); break; }
      out.push_back("\x01" + q.substr(i + 1, e - i - 1));
      i = e + 1;
      continue;
    }
    size_t j = i;
    while (j < q.size() && !std::isspace((unsigned char)q[j]) && q[j] != ';' && q[j] != '=' && q[j] != '\'' && q[j] != '"') ++j;
    out.push_back(q.substr(i, j - i));
    i = j;
  }
  return out;
}

// A prepared statement's text with each ? outside quotes read as NULL, so the
// statement tokenizes and can be checked before any value is bound.
static std::string sql_params_as_null(const std::string& sql) {
  std::string out;
  out.reserve(sql.size() + 16);
  bool in_quote = false;
  char quote = 0;
  for (size_t i = 0; i < sql.size(); ++i) {
    const char c = sql[i];
    if (in_quote) {
      out.push_back(c);
      if (c == '\\' && quote != '`' && i + 1 < sql.size()) { out.push_back(sql[++i]); continue; }
      if (c == quote) in_quote = false;
    } else if (c == '\'' || c == '"' || c == '`') {
      in_quote = true;
      quote = c;
      out.push_back(c);
    } else if (c == '?') {
      out += "NULL";
    } else {
      out.push_back(c);
    }
  }
  return out;
}

void MysqlWireServer::add_account(const std::string& user, const std::string& password) {
  accounts_[user] = password;
  install_user_lister();
}

// mysql.user: the operator accounts (every privilege) and the named users,
// each with the privileges its credential's rights stand for.
void MysqlWireServer::install_user_lister() {
  db_.set_user_lister([this] {
    SqlDatabase::UserRows rows;
    bool root = false;
    for (const auto& kv : accounts_) {
      rows.push_back({kv.first, "%", asql_password_hash(kv.second), "*"});
      root |= kv.first == "root";
    }
    if (!root) rows.push_back({"root", "localhost", "", "*"});
    if (cap_users_)
      for (const auto& u : cap_users_->list()) {
        std::string rights;
        AsqlAuthority au;
        std::string err;
        if (cap_guard_ && !u.credential.empty() && asql_verify(*cap_guard_, u.credential, &au, &err)) rights = asql_rights_text(au.rights);
        rows.push_back({u.name, "%", u.password_hash, rights});
      }
    return rows;
  });
}

// Constant-time so a wrong password cannot be found a byte at a time by
// timing the refusals.
static bool const_time_equal(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  if (a.size() != b.size()) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < a.size(); ++i) diff = (uint8_t)(diff | (a[i] ^ b[i]));
  return diff == 0;
}

bool MysqlWireServer::auth_ok(const std::string& user, const std::vector<uint8_t>& response,
                              const uint8_t* salt, bool from_loopback) const {
  // No accounts configured is a closed door, not an open one. The old
  // behaviour -- send OK and never look -- is what let `-u nobody -pWRONG`
  // read customer records from the internet.
  if (accounts_.empty()) return trust_loopback_ && from_loopback;

  auto it = accounts_.find(user);
  if (it == accounts_.end()) return false;

  const std::string& want_pass = it->second;
  if (want_pass.empty()) return response.empty();
  // Once an account carries a password it is always checked, local or not:
  // being on the box is not the same as knowing the password.
  if (response.empty()) return false;

  std::vector<uint8_t> expect = mysql_native_scramble(want_pass, salt, 20);
  return const_time_equal(expect, response);
}

namespace {
// The client's own socket, lent to the TLS layer for the life of this session
// (net::TlsServer owns what it reads through; the socket stays the caller's).
class BorrowedConnection : public hal::Connection {
 public:
  explicit BorrowedConnection(hal::Connection& c) : c_(c) {}
  bool send_all(ByteSpan d) override { return c_.send_all(d); }
  bool recv_all(MutableByteSpan d, int timeout_ms) override { return c_.recv_all(d, timeout_ms); }
  long recv_some(MutableByteSpan d, int timeout_ms) override { return c_.recv_some(d, timeout_ms); }
  std::string peer() const override { return c_.peer(); }
  void close() override { c_.close(); }
  bool alive() const override { return c_.alive(); }
  size_t pending() const override { return c_.pending(); }
 private:
  hal::Connection& c_;
};
}  // namespace

void MysqlWireServer::serve_client(hal::Connection& raw_conn) {
  uint32_t conn_id = next_conn_id_++;
  // However this connection ends - closed, killed, timed out - a transaction
  // it left open is rolled back. It holds the server's one writer lock, and a
  // thread that exits with it taken leaves every later BEGIN waiting forever.
  struct TxnGuard {
    SqlDatabase& db;
    ~TxnGuard() { db.abandon_open_transaction(); }
  } txn_guard{db_};
  db_.metrics().total_connections.fetch_add(1, std::memory_order_relaxed);
  uint64_t cur_conns = (uint64_t)++active_clients_;
  const size_t max_c = max_connections();
  if (max_c > 0 && cur_conns > max_c) {
    db_.metrics().rejected_connections.fetch_add(1, std::memory_order_relaxed);
    send_err(raw_conn, 0, 1040, "08004", "Too many connections");
    wire_flush(raw_conn);
    --active_clients_;
    db_.metrics().current_connections.store(active_clients_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return;
  }
  uint64_t max_conns = db_.metrics().max_used_connections.load(std::memory_order_relaxed);
  while (cur_conns > max_conns && !db_.metrics().max_used_connections.compare_exchange_weak(max_conns, cur_conns, std::memory_order_relaxed));
  db_.metrics().current_connections.store(cur_conns, std::memory_order_relaxed);

  // Step 1: Send Protocol::HandshakeV10
  // The salt must be unpredictable: it is the only thing stopping a replay of
  // a scramble captured off the wire. rand() seeded from the clock is not that.
  std::array<uint8_t, 20> salt;
  {
    std::random_device rd;
    for (int i = 0; i < 20; ++i) salt[i] = (uint8_t)((rd() % 255) + 1);
  }

  std::vector<uint8_t> hs;
  hs.push_back(HANDSHAKE_PROTOCOL);  // protocol version: 10
  std::string server_ver = "5.7.44-aios-asql";
  hs.insert(hs.end(), server_ver.begin(), server_ver.end());
  hs.push_back(0);  // NUL terminator
  put_u32(&hs, conn_id);
  hs.insert(hs.end(), salt.begin(), salt.begin() + 8);  // salt part 1 (8 bytes)
  hs.push_back(0);  // filler

  uint32_t caps = CAP_LONG_PASSWORD | CAP_FOUND_ROWS | CAP_LONG_FLAG |
                  CAP_CONNECT_WITH_DB | CAP_PROTOCOL_41 | CAP_TRANSACTIONS |
                  CAP_SECURE_CONNECTION | CAP_MULTI_STATEMENTS | CAP_MULTI_RESULTS |
                  CAP_PLUGIN_AUTH;
  if (tls_enabled()) caps |= 0x00000800;   // CLIENT_SSL
#if defined(AIOS_HAVE_LIBDEFLATE) || defined(AIOS_HAVE_ZLIB)
  caps |= kCapCompress;
#endif
#if defined(AIOS_HAVE_ZSTD)
  caps |= kCapZstd;
#endif
  put_u16(&hs, caps & 0xffff);  // capability flags lower 16 bits
  // Clients that take their connection charset from the greeting (PHP's mysqlnd
  // before 7.4) follow this byte. A charset-aware server stands in for a
  // MariaDB whose default is latin1, so it announces latin1_swedish_ci (8) and
  // those clients arrive as latin1 sessions; clients that ask for utf8 still get it.
  hs.push_back(charset_aware_ ? 0x08 : 0x2d);  // default collation: latin1_swedish_ci (8) / utf8mb4_general_ci (45)
  put_u16(&hs, 0x0002);         // status: AUTOCOMMIT (0x0002)
  put_u16(&hs, (caps >> 16) & 0xffff);  // capability flags upper 16 bits
  hs.push_back(21);             // auth_plugin_data_len (20 + 1 NUL)
  hs.resize(hs.size() + 10, 0); // reserved 10 zero bytes
  hs.insert(hs.end(), salt.begin() + 8, salt.begin() + 20);  // salt part 2 (12 bytes)
  hs.push_back(0);              // filler
  std::string plugin = "mysql_native_password";
  hs.insert(hs.end(), plugin.begin(), plugin.end());
  hs.push_back(0);              // NUL-terminated plugin name

  if (!send_packet(raw_conn, hs.data(), hs.size(), 0).ok()) {
    --active_clients_;
    db_.metrics().current_connections.store(active_clients_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return;
  }

  // Step 2: Read HandshakeResponse41
  std::vector<uint8_t> auth_pkt;
  uint8_t seq = 0;
  if (!recv_packet(raw_conn, &auth_pkt, &seq).ok()) {
    --active_clients_;
    db_.metrics().current_connections.store(active_clients_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return;
  }
  // SSLRequest: the 32 bytes of a handshake response with CLIENT_SSL and no
  // user. The TLS handshake runs on the socket right after it, and the real
  // handshake response arrives encrypted.
  std::unique_ptr<net::TlsServerConnection> tls_conn;
  if (tls_enabled() && auth_pkt.size() == 32 && (auth_pkt[1] & 0x08)) {
    auto server = std::make_unique<net::TlsServer>(kernel_.host(), tls_cert_, tls_key_);
    auto acc = server->accept(std::make_unique<BorrowedConnection>(raw_conn), 15000);
    if (!acc.ok()) {
      fprintf(stderr, "compute mysql wire: TLS handshake with %s failed: %s\n", raw_conn.peer().c_str(), acc.error().message.c_str());
      --active_clients_;
      db_.metrics().current_connections.store(active_clients_.load(std::memory_order_relaxed), std::memory_order_relaxed);
      return;
    }
    tls_conn = std::make_unique<net::TlsServerConnection>(std::move(server), raw_conn.peer());
    if (!recv_packet(*tls_conn, &auth_pkt, &seq).ok()) {
      --active_clients_;
      db_.metrics().current_connections.store(active_clients_.load(std::memory_order_relaxed), std::memory_order_relaxed);
      return;
    }
  }
  // From here on the session's connection is the encrypted one when it asked.
  hal::Connection& client_conn = tls_conn ? static_cast<hal::Connection&>(*tls_conn) : raw_conn;
  // Whatever is still queued goes out through it on every way out (destroyed
  // before tls_conn): the caller flushes the raw socket, which under TLS
  // dropped a refused login's error with the session.
  struct FlushAtEnd {
    MysqlWireServer* s; hal::Connection& c;
    ~FlushAtEnd() { s->wire_flush(c); }
  } flush_at_end{this, client_conn};
  if (tls_require_ && !tls_conn) {
    const std::string pr = raw_conn.peer();
    const bool local = pr.rfind("127.", 0) == 0 || pr.rfind("::1", 0) == 0 || pr.rfind("localhost", 0) == 0 || pr.rfind("unix:", 0) == 0;
    if (!local) {
      send_err(client_conn, (uint8_t)(seq + 1), 3159, "08004", "Connections using insecure transport are prohibited while --require_secure_transport=ON.");
      wire_flush(client_conn);
      --active_clients_;
      db_.metrics().current_connections.store(active_clients_.load(std::memory_order_relaxed), std::memory_order_relaxed);
      return;
    }
  }

  std::string session_database = "aios_db";
  std::string username;
  std::vector<uint8_t> auth_response;
  // What this session sends and expects (see set_charset_aware): text cells go
  // out as cp1252 and SQL text comes in as cp1252 while it is on a latin1
  // connection. Byte 8 of the handshake response is the client's collation.
  bool cs_in_latin1 = false, cs_out_latin1 = false;
  uint32_t client_flags = 0;
  int zstd_level = 3;   // the client's, when it sends one
  if (auth_pkt.size() >= 32) {
    // A client that asks for latin1 gets latin1 whatever the server announced as
    // its default (mysql_charset=1 only decides the announcement).
    if (collation_is_latin1(auth_pkt[8])) cs_in_latin1 = cs_out_latin1 = true;
    SqlDatabase::set_session_latin1(cs_in_latin1, cs_out_latin1);
    client_flags = (uint32_t)auth_pkt[0] | ((uint32_t)auth_pkt[1] << 8) |
                   ((uint32_t)auth_pkt[2] << 16) | ((uint32_t)auth_pkt[3] << 24);
    size_t offset = 32;
    // username (null terminated)
    size_t user_start = offset;
    while (offset < auth_pkt.size() && auth_pkt[offset] != 0) offset++;
    username.assign((const char*)&auth_pkt[user_start], offset - user_start);
    if (offset < auth_pkt.size()) offset++; // skip null
    // auth data
    if (client_flags & 0x00200000) { // CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA
      if (offset < auth_pkt.size()) {
        uint64_t len = auth_pkt[offset++];
        if (offset + len <= auth_pkt.size()) {
          auth_response.assign(auth_pkt.begin() + offset, auth_pkt.begin() + offset + len);
        }
        offset += len;
      }
    } else if (client_flags & 0x00008000) { // CLIENT_SECURE_CONNECTION
      if (offset < auth_pkt.size()) {
        uint8_t len = auth_pkt[offset++];
        if (offset + len <= auth_pkt.size()) {
          auth_response.assign(auth_pkt.begin() + offset, auth_pkt.begin() + offset + len);
        }
        offset += len;
      }
    } else {
      size_t ar_start = offset;
      while (offset < auth_pkt.size() && auth_pkt[offset] != 0) offset++;
      auth_response.assign(auth_pkt.begin() + ar_start, auth_pkt.begin() + offset);
      if (offset < auth_pkt.size()) offset++;
    }
    // database
    if ((client_flags & 0x00000008) && offset < auth_pkt.size()) { // CLIENT_CONNECT_WITH_DB
      size_t db_start = offset;
      while (offset < auth_pkt.size() && auth_pkt[offset] != 0) offset++;
      if (offset > db_start) {
        session_database = std::string((const char*)&auth_pkt[db_start], offset - db_start);
      }
      if (offset < auth_pkt.size()) offset++;
    }
    // The zstd level closes the response, after the plugin name and the connection attributes.
    if ((client_flags & kCapZstd) && (caps & kCapZstd)) {
      // Only what both sides announced is in it: a client keeps CLIENT_CONNECT_ATTRS set, and sends no attributes,
      // when this server does not offer it.
      if (client_flags & caps & CAP_PLUGIN_AUTH) {
        while (offset < auth_pkt.size() && auth_pkt[offset] != 0) offset++;
        if (offset < auth_pkt.size()) offset++;
      }
      if ((client_flags & caps & 0x00100000) && offset < auth_pkt.size()) {   // CLIENT_CONNECT_ATTRS
        uint64_t alen = auth_pkt[offset++];
        if (alen == 0xfc && offset + 2 <= auth_pkt.size()) { alen = auth_pkt[offset] | (auth_pkt[offset + 1] << 8); offset += 2; }
        else if (alen == 0xfd && offset + 3 <= auth_pkt.size()) { alen = get_u24(&auth_pkt[offset]); offset += 3; }
        offset += (size_t)alen;
      }
      if (offset < auth_pkt.size()) zstd_level = std::clamp((int)auth_pkt[offset], 1, 22);
    }
  }

  std::string peer = client_conn.peer();
  bool from_loopback = peer.rfind("127.", 0) == 0 || peer.rfind("::1", 0) == 0 ||
                       peer.rfind("localhost", 0) == 0 || peer.rfind("unix:", 0) == 0;
  // A session that logged in with a capability credential: what it may do is
  // that credential's authority and nothing else (see asql_capability.hpp).
  bool cap_session = false;
  AsqlAuthority authority;
  std::string credential;
  uint64_t rows_charged = 0;
  // The answer to the handshake response: 2, or 3 after an SSLRequest (packet 1).
  uint8_t ok_seq = (uint8_t)(seq + 1);
  const uint8_t auth_seq = ok_seq;
  auto refuse_login = [&](uint8_t at_seq, const std::string& why) {
    send_err(client_conn, at_seq, 1045, "28000", why);
    --active_clients_;
    db_.metrics().current_connections.store(active_clients_.load(std::memory_order_relaxed), std::memory_order_relaxed);
  };
  // With no database asked for, a capability session starts in the one its
  // grant names, when the grant is in one place.
  auto settle_session_db = [&]() {
    if (authority.names_db(session_database) || session_database != "aios_db") return;
    std::string only;
    bool one = true;
    for (const auto& s : authority.scopes) {
      if (only.empty()) only = s.db;
      else if (only != s.db) one = false;
    }
    if (one && !only.empty() && only != "*") session_database = only;
  };
  AsqlUser named_user;
  const bool named_login = cap_guard_ && cap_users_ && username != "cap" && accounts_.find(username) == accounts_.end() &&
                           cap_users_->find(username, &named_user);
  if (named_login) {
    // A named user is a password in front of a credential: the password is
    // checked the ordinary scrambled way, so any client can send it.
    if (!asql_password_matches(named_user.password_hash, salt.data(), auth_response)) {
      refuse_login(auth_seq, "Access denied for user '" + username + "' (using password: " + (auth_response.empty() ? "NO" : "YES") + ")");
      return;
    }
    std::string verr;
    if (named_user.credential.empty()) {   // CREATE USER with nothing GRANTed yet
      refuse_login(auth_seq, "Access denied for user '" + username + "': no privileges have been granted to it");
      return;
    }
    if (!asql_verify(*cap_guard_, named_user.credential, &authority, &verr)) {
      refuse_login(auth_seq, "Access denied for user '" + username + "': " + verr);
      return;
    }
    cap_session = true;
    credential = named_user.credential;
    settle_session_db();
  } else if (cap_guard_ && username == "cap") {
    if (!(client_flags & 0x00080000)) {
      refuse_login(auth_seq, "Access denied for user 'cap' (client cannot switch authentication plugin)");
      return;
    }
    if (!from_loopback && !cap_remote_) {
      refuse_login(auth_seq, "Access denied for user 'cap' (capability credentials are accepted from this host only)");
      return;
    }
    // The credential is verified by its signature, so the server cannot
    // scramble-compare it: the client is asked to send it as clear text.
    std::vector<uint8_t> sw;
    sw.push_back(0xfe);
    static const char kClear[] = "mysql_clear_password";
    sw.insert(sw.end(), kClear, kClear + sizeof(kClear));
    if (!send_packet(client_conn, sw.data(), sw.size(), auth_seq).ok()) { refuse_login(auth_seq, ""); return; }
    std::vector<uint8_t> reply;
    uint8_t rseq = 0;
    if (!recv_packet(client_conn, &reply, &rseq).ok()) {
      --active_clients_;
      db_.metrics().current_connections.store(active_clients_.load(std::memory_order_relaxed), std::memory_order_relaxed);
      return;
    }
    ok_seq = (uint8_t)(rseq + 1);
    std::string presented(reply.begin(), reply.end());
    while (!presented.empty() && presented.back() == '\0') presented.pop_back();
    std::string verr;
    if (!asql_verify(*cap_guard_, presented, &authority, &verr)) {
      refuse_login(ok_seq, "Access denied for user 'cap': " + verr);
      return;
    }
    cap_session = true;
    credential = presented;
    settle_session_db();
  } else if (!auth_ok(username, auth_response, salt.data(), from_loopback)) {
    send_err(client_conn, auth_seq, 1045, "28000",
             "Access denied for user '" + username + "'");
    --active_clients_;
    db_.metrics().current_connections.store(active_clients_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return;
  }

  if (!send_ok(client_conn, ok_seq, 0, 0, 0x0002).ok()) {
    --active_clients_;
    db_.metrics().current_connections.store(active_clients_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return;
  }
  // The compressed protocol starts after the login's OK, which goes plain: zstd where both sides have it, else zlib.
  struct CompressOff {   // what is still queued goes out compressed, before the session's thread forgets it
    MysqlWireServer* s; hal::Connection& c;
    ~CompressOff() { s->wire_flush(c); tl_wz = WireCompress(); SqlDatabase::set_session_compression(nullptr, 0); }
  } compress_off{this, client_conn};
  if (const uint32_t both = client_flags & caps; both & (kCapCompress | kCapZstd)) {
    if (!wire_flush(client_conn)) return;
    tl_wz = WireCompress();
    tl_wz.conn = &client_conn;
    tl_wz.algo = (both & kCapZstd) ? kWireZstd : kWireZlib;
    // zlib's level is the server's (the protocol carries none): AIOS_WIRE_ZLIB_LEVEL, 6 by default as in MariaDB.
    static const int zlib_level = [] {
      const char* e = std::getenv("AIOS_WIRE_ZLIB_LEVEL");
      return e && *e ? std::clamp(std::atoi(e), 1, 12) : 6;
    }();
    tl_wz.level = tl_wz.algo == kWireZstd ? zstd_level : zlib_level;
    SqlDatabase::set_session_compression(tl_wz.algo == kWireZstd ? "zstd" : "zlib", tl_wz.level);
  }

  // This session owns its thread for the rest of its life, so tagging once here
  // attributes every statement it goes on to run. A monitor connecting over the
  // wire is then distinguishable from the application connecting over RPC.
  SqlDatabase::set_client_tag("wire:" + username + "@" + peer);
  SqlDatabase::set_current_session_id(conn_id);
  SqlDatabase::register_active_session(conn_id, username, peer, session_database, "Sleep", "idle");
  // KILL CONNECTION from another session: shutting the socket wakes this
  // thread wherever it waits on the client, and the loop below ends.
  SqlDatabase::set_session_closer(conn_id, [&client_conn]() { client_conn.close(); });

  // Step 3: Command Loop
  std::unordered_map<uint32_t, WirePreparedStatement> prepared_stmts;
  uint32_t next_stmt_id = 1;
  // What COM_RESET_CONNECTION and COM_CHANGE_USER put back: the charset the
  // handshake chose and whether the client asked for multi-statements.
  const bool handshake_latin1 = cs_in_latin1;
  const bool handshake_multi = (client_flags & CAP_MULTI_STATEMENTS) != 0;
  bool multi_statements = handshake_multi;

  auto shrink_prepared_stmts = [&](bool idle) {
    for (auto& kv : prepared_stmts) {
      auto& ps = kv.second;
      for (size_t p = 0; p < ps.num_params; ++p) {
        if (!ps.has_long_data[p]) {
          if (ps.long_data[p].capacity() > (idle ? 0 : 256)) {
            std::string().swap(ps.long_data[p]);
          }
        } else if (idle && ps.long_data[p].capacity() > ps.long_data[p].size() + 1024) {
          ps.long_data[p].shrink_to_fit();
        }
      }
      if (ps.param_types.capacity() > ps.num_params) ps.param_types.shrink_to_fit();
      if (ps.long_data.capacity() > ps.num_params) ps.long_data.shrink_to_fit();
      if (ps.has_long_data.capacity() > ps.num_params) ps.has_long_data.shrink_to_fit();
    }
    if (idle && prepared_stmts.empty()) {
      prepared_stmts.rehash(0);
    }
  };

  auto statement_cleanup = [&]() {
    wire_flush(client_conn);
    wire_shrink_statement();
    shrink_prepared_stmts(false);
    db_.shrink_session_buffers(false);
  };

  // Sends a MySQL error for an engine message, with the code and SQLSTATE a
  // client branches on.
  auto send_engine_error = [&](uint8_t at_seq, const std::string& msg) {
    uint16_t code = 1064;
    const char* state = "42000";
    map_engine_error(msg, &code, &state);
    send_err(client_conn, at_seq, code, state, msg);
  };

  // Under a capability the credential is verified again for every statement,
  // so a revocation or an expiry cuts a session off at its next statement, and
  // the statement is read against the authority before anything runs.
  auto capability_refusal = [&](const std::string& sql) -> std::string {
    if (!cap_session) return std::string();
    AsqlAuthority fresh;
    std::string verr;
    if (!asql_verify(*cap_guard_, credential, &fresh, &verr)) return verr;
    authority = std::move(fresh);
    if (authority.rows_cap && rows_charged >= authority.rows_cap) return "row budget exhausted (RESOURCE_LIMIT_EXCEEDED)";
    return db_.authorize_statement(sql, authority, session_database);
  };
  // Rows returned or changed count against the credential's budget. A result
  // that would pass it is not sent at all.
  auto charge_rows = [&](uint64_t rows) -> bool {
    if (!cap_session || authority.rows_cap == 0) return true;
    if (rows > authority.rows_cap - std::min(rows_charged, authority.rows_cap)) {
      rows_charged = authority.rows_cap;
      return false;
    }
    rows_charged += rows;
    return true;
  };
  // A small text result set the wire layer answers itself.
  auto send_text_rows = [&](const std::vector<std::string>& columns, const std::vector<std::vector<std::string>>& rows) {
    uint8_t at = 1;
    std::vector<uint8_t> count;
    put_lenenc_u64(&count, columns.size());
    send_packet(client_conn, count.data(), count.size(), at++);
    for (const auto& column : columns) {
      std::vector<uint8_t> def;
      put_lenenc_str(&def, "def");
      put_lenenc_str(&def, "");
      put_lenenc_str(&def, "");
      put_lenenc_str(&def, "");
      put_lenenc_str(&def, column);
      put_lenenc_str(&def, column);
      def.push_back(0x0c);
      put_u16(&def, 45);
      put_u32(&def, 1u << 16);
      def.push_back(MYSQL_TYPE_VAR_STRING);
      put_u16(&def, 0);
      def.push_back(0);
      put_u16(&def, 0);
      send_packet(client_conn, def.data(), def.size(), at++);
    }
    const uint8_t eof[] = {0xfe, 0x00, 0x00, 0x02, 0x00};
    send_packet(client_conn, eof, sizeof(eof), at++);
    for (const auto& r : rows) {
      std::vector<uint8_t> row;
      for (const auto& v : r) put_lenenc_str(&row, v);
      send_packet(client_conn, row.data(), row.size(), at++);
    }
    send_packet(client_conn, eof, sizeof(eof), at);
  };
  auto send_one_value = [&](const std::string& column, const std::string& value) {
    send_text_rows({column}, {{value}});
  };
  // CAPABILITY MINT / CAPABILITY REVOKE (operator sessions) and SET SESSION
  // capability (any session). True when the statement was one of them.
  auto capability_statement = [&](const std::string& sql) -> bool {
    const auto w = sql_words(sql);
    if (w.empty()) return false;
    auto up = [](std::string s) { for (char& c : s) c = (char)::toupper((unsigned char)c); return s; };
    auto quoted = [](const std::string& s) { return !s.empty() && s[0] == '\x01'; };
    auto fail = [&](const std::string& why) { send_err(client_conn, 1, 1227, "42000", "ASQL capability: " + why); return true; };
    if (w.size() >= 3 && up(w[0]) == "SET" && up(w[1]) == "SESSION" && up(w[2]) == "CAPABILITY") {
      if (!cap_guard_) return fail("capabilities are not enabled on this server");
      if (w.size() != 4 || !quoted(w[3])) return fail("SET SESSION capability = '<credential>'");
      AsqlAuthority fresh;
      std::string verr;
      if (!asql_verify(*cap_guard_, w[3].substr(1), &fresh, &verr)) return fail(verr);
      authority = std::move(fresh);
      credential = w[3].substr(1);
      cap_session = true;
      rows_charged = 0;
      send_ok(client_conn, 1);
      return true;
    }
    // SHOW GRANTS [FOR 'user'[@'host']]: the user's credential read back as
    // the privileges it stands for (it used to answer ALL PRIVILEGES for
    // anyone). An operator session with no user named is root.
    if (w.size() >= 2 && up(w[0]) == "SHOW" && up(w[1]) == "GRANTS" && cap_users_ && cap_guard_) {
      std::string name, host = "%";
      if (w.size() >= 4 && up(w[2]) == "FOR") {
        name = quoted(w[3]) ? w[3].substr(1) : w[3].substr(0, w[3].find('@'));
        if (w.size() >= 6 && w[4] == "@") host = quoted(w[5]) ? w[5].substr(1) : w[5];
        else if (!quoted(w[3]) && w[3].find('@') != std::string::npos) host = w[3].substr(w[3].find('@') + 1);
      } else if (cap_session) {
        name = username;
      } else {
        return false;   // the engine answers for the operator (root)
      }
      AsqlUser user;
      if (!cap_users_->find(name, &user)) { send_err(client_conn, 1, 1141, "42000", "There is no such grant defined for user '" + name + "' on host '" + host + "'"); return true; }
      const std::string who = "'" + name + "'@'" + host + "'";
      std::vector<std::vector<std::string>> rows;
      rows.push_back({"GRANT USAGE ON *.* TO " + who});
      AsqlAuthority a;
      std::string verr;
      if (!user.credential.empty() && asql_verify(*cap_guard_, user.credential, &a, &verr)) {
        using R = aios::sidecar::CapabilityRight;
        const uint32_t r = a.rights;
        std::string privs;
        auto add = [&](const char* p) { privs += (privs.empty() ? "" : ", ") + std::string(p); };
        if ((r & 0x2F) == 0x2F) privs = "ALL PRIVILEGES";
        else {
          if (r & (uint32_t)R::READ) add("SELECT");
          if (r & (uint32_t)R::WRITE) { add("INSERT"); add("UPDATE"); }
          if (r & (uint32_t)R::DELETE_OP) add("DELETE");
          if (r & (uint32_t)R::EXECUTE) add("EXECUTE");
          if (r & (uint32_t)R::ADMIN) { add("CREATE"); add("DROP"); add("ALTER"); add("INDEX"); add("REFERENCES"); add("CREATE VIEW"); add("TRIGGER"); add("EVENT"); add("LOCK TABLES"); }
        }
        const std::string opt = (r & (uint32_t)R::DELEGATE) ? " WITH GRANT OPTION" : "";
        for (const auto& sc : a.scopes) {
          std::string on = sc.db == "*" ? "*.*" : "`" + sc.db + "`." + (sc.table == "*" ? "*" : "`" + sc.table + "`");
          if (privs.empty()) continue;
          if (on == "*.*") rows[0][0] = "GRANT " + privs + " ON *.* TO " + who + opt;
          else rows.push_back({"GRANT " + privs + " ON " + on + " TO " + who + opt});
        }
      }
      send_text_rows({"Grants for " + name + "@" + host}, rows);
      return true;
    }
    // CREATE USER / ALTER USER / DROP USER / SHOW USERS: named users in front
    // of credentials (AsqlUserStore). Operator sessions only.
    const bool user_stmt = w.size() >= 3 && up(w[1]) == "USER" && (up(w[0]) == "CREATE" || up(w[0]) == "ALTER" || up(w[0]) == "DROP");
    const bool show_users = w.size() == 2 && up(w[0]) == "SHOW" && up(w[1]) == "USERS";
    if (user_stmt || show_users) {
      if (cap_session) return fail("not available under a capability");
      if (!cap_guard_ || !cap_users_) return fail("capabilities are not enabled on this server");
      auto expires_text = [](uint64_t ns) -> std::string {
        if (!ns) return "never";
        const time_t t = (time_t)(ns / 1000000000ull);
        char buf[32];
        struct tm tmv{};
        ::localtime_r(&t, &tmv);
        ::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tmv);
        return buf;
      };
      if (show_users) {
        std::vector<std::vector<std::string>> rows;
        for (const auto& u : cap_users_->list()) {
          AsqlAuthority au;
          std::string verr;
          if (!asql_verify(*cap_guard_, u.credential, &au, &verr)) {
            rows.push_back({u.name, "", "", "", "", verr});
            continue;
          }
          std::string tables;
          for (const auto& s : au.scopes) tables += (tables.empty() ? "" : ",") + s.db + "." + s.table;
          rows.push_back({u.name, tables, asql_rights_text(au.rights), expires_text(au.expires_at_ns),
                          au.rows_cap ? std::to_string(au.rows_cap) : "unlimited", "ok"});
        }
        send_text_rows({"User", "Tables", "Rights", "Expires", "Rows", "Status"}, rows);
        return true;
      }
      const std::string verb = up(w[0]);
      const std::string usage = verb == "CREATE"
          ? "CREATE USER 'name' IDENTIFIED BY 'password' TABLES 'db.*,db.table' [RIGHTS 'read,write,delete,execute,admin'] [TTL <seconds>] [ROWS <n>]"
          : verb == "ALTER" ? "ALTER USER 'name' [IDENTIFIED BY 'password'] [TABLES '...'] [RIGHTS '...'] [TTL <seconds>] [ROWS <n>]"
                            : "DROP USER 'name'";
      size_t i = 2;
      // IF [NOT] EXISTS, and a MySQL-style @'host', are accepted and mean nothing here.
      bool if_exists = false;
      if (i + 1 < w.size() && up(w[i]) == "IF" && up(w[i + 1]) == "EXISTS") { if_exists = true; i += 2; }
      else if (i + 2 < w.size() && up(w[i]) == "IF" && up(w[i + 1]) == "NOT" && up(w[i + 2]) == "EXISTS") { if_exists = true; i += 3; }
      if (i >= w.size()) return fail(usage);
      std::string name = quoted(w[i]) ? w[i].substr(1) : w[i].substr(0, w[i].find('@'));
      ++i;
      if (i < w.size() && w[i] == "@") i += 2;
      else if (i < w.size() && w[i][0] == '@') ++i;
      if (!asql_valid_user_name(name)) return fail("a user name is 1-32 letters, digits, _ - . and is not 'cap'");
      AsqlUser existing;
      const bool exists = cap_users_->find(name, &existing);
      std::string err;
      if (verb == "DROP") {
        if (i != w.size()) return fail(usage);
        if (!exists) return if_exists ? (send_ok(client_conn, 1), true) : fail("no user '" + name + "'");
        AsqlAuthority au;
        if (asql_verify(*cap_guard_, existing.credential, &au, &err) && !asql_revoke(*cap_guard_, existing.credential, &err)) return fail(err);
        if (!cap_users_->erase(name, &err)) return fail(err);
        send_ok(client_conn, 1);
        return true;
      }
      std::string password;
      bool have_password = false;
      AsqlGrant g;
      bool regrant = false;
      for (; i < w.size(); i += 2) {
        const std::string key = up(w[i]);
        if (key == "IDENTIFIED") {
          if (i + 2 >= w.size() || up(w[i + 1]) != "BY" || !quoted(w[i + 2])) return fail(usage);
          password = w[i + 2].substr(1);
          have_password = true;
          ++i;
          continue;
        }
        if (i + 1 >= w.size()) return fail(usage);
        const std::string val = quoted(w[i + 1]) ? w[i + 1].substr(1) : w[i + 1];
        char* end = nullptr;
        if (key == "TABLES") {
          if (!asql_parse_scopes(val, &g.scopes, &err)) return fail(err);
        } else if (key == "RIGHTS") {
          if (!asql_parse_rights(val, &g.rights, &err)) return fail(err);
        } else if (key == "TTL") {
          const unsigned long long s = std::strtoull(val.c_str(), &end, 10);
          if (val.empty() || *end) return fail("TTL is a number of seconds (0 = never expires)");
          g.ttl_ns = s * 1000000000ull;
        } else if (key == "ROWS") {
          g.rows_cap = std::strtoull(val.c_str(), &end, 10);
          if (val.empty() || *end) return fail("ROWS is a number");
        } else {
          return fail("unknown option " + w[i] + "; " + usage);
        }
        regrant = true;
      }
      if (have_password && password.empty()) return fail("a user needs a password");
      AsqlUser user = exists ? existing : AsqlUser{name, "", ""};
      if (verb == "CREATE") {
        if (exists) return if_exists ? (send_ok(client_conn, 1), true) : fail("user '" + name + "' exists; ALTER USER changes it, DROP USER removes it");
        if (!have_password) return fail("a user needs a password: " + usage);
        // CREATE USER 'u'@'h' IDENTIFIED BY 'pw' as MySQL has it: no
        // privileges until a GRANT gives some.
        if (g.scopes.empty() && !regrant) {
          user.password_hash = asql_password_hash(password);
          if (!cap_users_->put(user, &err)) return fail(err);
          send_ok(client_conn, 1);
          return true;
        }
        if (g.scopes.empty()) return fail("a user needs TABLES: " + usage);
        if (!g.rights) g.rights = 1;
      } else {
        if (!exists) return fail("no user '" + name + "'");
        if (!have_password && !regrant) return fail(usage);
      }
      if (regrant) {
        // A changed grant is a new credential; the old one is revoked so a
        // session still holding it is cut off at its next statement.
        AsqlAuthority old;
        if (exists && !g.scopes.empty() && asql_verify(*cap_guard_, existing.credential, &old, &err)) {
          if (!asql_revoke(*cap_guard_, existing.credential, &err)) return fail(err);
        } else if (exists && g.scopes.empty()) {
          // Only rights/ttl/rows changed: keep the scope the user already has.
          if (!asql_verify(*cap_guard_, existing.credential, &old, &err)) return fail("the user's credential is " + err + "; give TABLES to grant afresh");
          g.scopes = old.scopes;
          if (!g.rights) g.rights = old.rights;
          if (!asql_revoke(*cap_guard_, existing.credential, &err)) return fail(err);
        }
        if (!g.rights) g.rights = 1;
        if (!asql_mint(*cap_guard_, g, &user.credential, &err)) return fail(err);
      }
      if (have_password) user.password_hash = asql_password_hash(password);
      if (!cap_users_->put(user, &err)) return fail(err);
      send_ok(client_conn, 1);
      return true;
    }
    // GRANT privileges ON db.table | db.* | *.* TO 'user'[@'host'] [IDENTIFIED BY 'pw'] [WITH GRANT OPTION]
    // REVOKE privileges ON ... FROM 'user'[@'host']
    // A named user's credential is the tables it may touch and one set of
    // rights over all of them: a GRANT adds its tables and rights, a REVOKE
    // ALL takes its tables away, a REVOKE of some privileges takes those rights.
    // SELECT is read; INSERT and UPDATE write; DELETE delete; EXECUTE execute;
    // CREATE, ALTER, DROP, INDEX and the other DDL admin; GRANT OPTION delegate.
    if ((up(w[0]) == "GRANT" || up(w[0]) == "REVOKE") && w.size() >= 4) {
      const bool grant = up(w[0]) == "GRANT";
      if (cap_session) return fail("not available under a capability");
      if (!cap_guard_ || !cap_users_) return fail("capabilities are not enabled on this server");
      size_t i = 1;
      std::string privs;
      while (i < w.size() && up(w[i]) != "ON") privs += " " + w[i++];
      if (i + 2 >= w.size()) return fail(grant ? "GRANT <privileges> ON <db>.<table> TO 'user'" : "REVOKE <privileges> ON <db>.<table> FROM 'user'");
      ++i;
      if (up(w[i]) == "TABLE" || up(w[i]) == "FUNCTION" || up(w[i]) == "PROCEDURE") ++i;
      std::string target = w[i++];
      target.erase(std::remove(target.begin(), target.end(), '`'), target.end());
      if (target == "*") target = session_database.empty() ? "*.*" : session_database + ".*";
      else if (target.find('.') == std::string::npos) target = (session_database.empty() ? db_.effective_database() : session_database) + "." + target;
      if (i >= w.size() || up(w[i]) != (grant ? "TO" : "FROM")) return fail(std::string("expected ") + (grant ? "TO" : "FROM"));
      ++i;
      if (i >= w.size()) return fail("expected a user");
      std::string name = quoted(w[i]) ? w[i].substr(1) : w[i].substr(0, w[i].find('@'));
      ++i;
      if (i < w.size() && w[i] == "@") i += 2;
      else if (i < w.size() && w[i][0] == '@') ++i;
      std::string password;
      bool have_password = false, grant_option = false;
      for (; i < w.size(); ++i) {
        if (up(w[i]) == "IDENTIFIED" && i + 2 < w.size() && up(w[i + 1]) == "BY" && quoted(w[i + 2])) { password = w[i + 2].substr(1); have_password = true; i += 2; }
        else if (up(w[i]) == "WITH" && i + 2 < w.size() && up(w[i + 1]) == "GRANT" && up(w[i + 2]) == "OPTION") { grant_option = true; i += 2; }
      }
      // The privileges as rights.
      std::string text = up(privs);
      for (char& c : text) if (c == ',' || c == '(' || c == ')') c = ' ';
      std::vector<std::string> pw;
      { std::istringstream in(text); std::string x; while (in >> x) pw.push_back(x); }
      std::string rights;
      auto add = [&](const char* r) { if (rights.find(r) == std::string::npos) rights += (rights.empty() ? "" : ",") + std::string(r); };
      bool all = false;
      for (size_t k = 0; k < pw.size(); ++k) {
        const std::string& x = pw[k];
        if (x == "ALL") { all = true; add("read"); add("write"); add("delete"); add("execute"); add("admin"); }
        else if (x == "PRIVILEGES" || x == "USAGE" || x == "TABLES" || x == "TEMPORARY" || x == "VIEW" || x == "ROUTINE" || x == "USER") {}
        else if (x == "SELECT" || x == "SHOW") add("read");
        else if (x == "INSERT" || x == "UPDATE") add("write");
        else if (x == "DELETE") add("delete");
        else if (x == "EXECUTE") add("execute");
        else if (x == "GRANT" && k + 1 < pw.size() && pw[k + 1] == "OPTION") { add("delegate"); ++k; }
        else if (x == "CREATE" || x == "DROP" || x == "ALTER" || x == "INDEX" || x == "REFERENCES" || x == "TRIGGER" ||
                 x == "EVENT" || x == "LOCK" || x == "RELOAD" || x == "PROCESS" || x == "FILE" || x == "SUPER" || x == "REPLICATION" ||
                 x == "CLIENT" || x == "SLAVE" || x == "SHUTDOWN") add("admin");
        else if (!quoted(x)) { /* a column list's names */ }
      }
      if (grant_option) add("delegate");
      uint32_t bits = 0;
      std::string err;
      if (!rights.empty() && !asql_parse_rights(rights, &bits, &err)) return fail(err);
      std::vector<AsqlScopeEntry> scope;
      if (!asql_parse_scopes(target, &scope, &err)) return fail(err);
      // Read, change and write one user's credential as a unit: two GRANTs at once would each start from the old
      // credential and the later write would lose the earlier one.
      static aios::hal::Mutex grant_mu;
      std::lock_guard<aios::hal::Mutex> grant_lock(grant_mu);
      AsqlUser user;
      const bool exists = cap_users_->find(name, &user);
      if (!exists) {
        if (!grant || !have_password) return fail("no user '" + name + "' (CREATE USER it first)");
        if (!asql_valid_user_name(name)) return fail("a user name is 1-32 letters, digits, _ - . and is not 'cap'");
        user = AsqlUser{name, "", ""};
      }
      AsqlAuthority old;
      std::vector<AsqlScopeEntry> scopes;
      uint32_t have = 0;
      if (!user.credential.empty() && asql_verify(*cap_guard_, user.credential, &old, &err)) { scopes = old.scopes; have = old.rights; }
      auto same = [](const AsqlScopeEntry& a, const AsqlScopeEntry& b) { return a.db == b.db && a.table == b.table; };
      if (grant) {
        for (const auto& e : scope) {
          bool dup = false;
          for (const auto& h : scopes) dup |= same(h, e);
          if (!dup) scopes.push_back(e);
        }
        have |= bits;
      } else if (all) {
        std::vector<AsqlScopeEntry> kept;
        for (const auto& h : scopes) {
          bool gone = false;
          for (const auto& e : scope) gone |= same(h, e);
          if (!gone) kept.push_back(h);
        }
        scopes = std::move(kept);
      } else {
        have &= ~bits;
      }
      // The new credential is minted first and the old one revoked only once it is saved: a mint that fails leaves the
      // user as it was (it used to be revoked by then, and the user locked out). The limits the operator put on the user
      // - how long it lasts, how many rows it may read or change - carry over; they were dropped on every GRANT.
      std::string fresh;
      if (!scopes.empty() && have) {
        AsqlGrant g;
        g.scopes = scopes;
        g.rights = have;
        g.ttl_ns = asql_remaining_ttl_ns(old);
        g.rows_cap = old.rows_cap;
        if (!asql_mint(*cap_guard_, g, &fresh, &err)) return fail(err);
      }
      const std::string previous = user.credential;
      user.credential = fresh;
      if (have_password) user.password_hash = asql_password_hash(password);
      if (!cap_users_->put(user, &err)) {
        std::string undo_err;
        if (!fresh.empty()) asql_revoke(*cap_guard_, fresh, &undo_err);
        return fail(err);
      }
      if (!previous.empty() && !old.scopes.empty()) {
        std::string revoke_err;
        asql_revoke(*cap_guard_, previous, &revoke_err);
      }
      send_ok(client_conn, 1);
      return true;
    }
    if (up(w[0]) != "CAPABILITY") return false;
    if (cap_session) return fail("not available under a capability");
    if (!cap_guard_) return fail("capabilities are not enabled on this server");
    if (w.size() >= 3 && up(w[1]) == "REVOKE" && quoted(w[2]) && w.size() == 3) {
      std::string err;
      if (!asql_revoke(*cap_guard_, w[2].substr(1), &err)) return fail(err);
      send_ok(client_conn, 1);
      return true;
    }
    if (w.size() >= 3 && up(w[1]) == "MINT" && quoted(w[2])) {
      AsqlGrant g;
      std::string err;
      if (!asql_parse_scopes(w[2].substr(1), &g.scopes, &err)) return fail(err);
      g.ttl_ns = 3600ull * 1000000000ull;
      uint32_t rights = 1;
      for (size_t i = 3; i < w.size(); i += 2) {
        if (i + 1 >= w.size()) return fail("CAPABILITY MINT '<db.table,...>' RIGHTS '<list>' [TTL <seconds>] [ROWS <n>]");
        const std::string key = up(w[i]);
        const std::string val = quoted(w[i + 1]) ? w[i + 1].substr(1) : w[i + 1];
        char* end = nullptr;
        if (key == "RIGHTS") {
          if (!asql_parse_rights(val, &rights, &err)) return fail(err);
        } else if (key == "TTL") {
          const unsigned long long s = std::strtoull(val.c_str(), &end, 10);
          if (val.empty() || *end) return fail("TTL is a number of seconds (0 = never expires)");
          g.ttl_ns = s * 1000000000ull;
        } else if (key == "ROWS") {
          g.rows_cap = std::strtoull(val.c_str(), &end, 10);
          if (val.empty() || *end) return fail("ROWS is a number");
        } else {
          return fail("unknown option " + w[i]);
        }
      }
      g.rights = rights;
      std::string cred;
      if (!asql_mint(*cap_guard_, g, &cred, &err)) return fail(err);
      send_one_value("credential", cred);
      return true;
    }
    return fail("CAPABILITY MINT '<db.table,...>' RIGHTS '<list>' [TTL <seconds>] [ROWS <n>] | CAPABILITY REVOKE '<credential>'");
  };

  while (running_ && client_conn.alive()) {
    std::vector<uint8_t> cmd_pkt;
    uint8_t cmd_seq = 0;
    auto cr = recv_packet(client_conn, &cmd_pkt, &cmd_seq, [&]() { shrink_prepared_stmts(true); });
    if (!cr.ok() || cmd_pkt.empty()) break;
    struct StmtCleanupGuard {
      std::function<void()> fn;
      ~StmtCleanupGuard() { if (fn) fn(); }
    } stmt_guard{statement_cleanup};

    uint8_t cmd = cmd_pkt[0];
    // A capability session gets statements and its own housekeeping, nothing
    // that reads the schema, replicates, or becomes another user.
    if (cap_session && cmd != COM_QUIT && cmd != COM_PING && cmd != COM_INIT_DB && cmd != COM_QUERY &&
        cmd != COM_STMT_PREPARE && cmd != COM_STMT_EXECUTE && cmd != COM_STMT_SEND_LONG_DATA &&
        cmd != COM_STMT_CLOSE && cmd != COM_STMT_RESET && cmd != COM_SET_OPTION && cmd != COM_RESET_CONNECTION &&
        cmd != COM_PROCESS_INFO) {
      send_err(client_conn, 1, 1227, "42000", "ASQL capability: command not available under a capability");
      continue;
    }
    // The engine is still loading, seeding or starting replication: the client is told so, statement by
    // statement, instead of finding the port shut. Nothing here reaches the engine.
    if (starting()) {
      if (cmd == COM_QUIT) break;
      if (cmd == COM_PING) { send_ok(client_conn, 1); continue; }
      if (cmd == COM_INIT_DB) {
        session_database.assign((const char*)&cmd_pkt[1], cmd_pkt.size() - 1);   // checked by the first query that uses it
        send_ok(client_conn, 1);
        continue;
      }
      if (cmd == COM_QUERY) {
        std::string q((const char*)&cmd_pkt[1], cmd_pkt.size() - 1);
        for (char& ch : q) ch = (char)std::toupper((unsigned char)ch);
        while (!q.empty() && (q.back() == ';' || q.back() == ' ' || q.back() == '\n')) q.pop_back();
        if (q == "SELECT @@VERSION_COMMENT" || q == "SELECT @@VERSION_COMMENT LIMIT 1") {   // what the mysql client asks on connecting
          send_one_value("@@version_comment", "AIOS ASQL / MySQL Native Wire Server (starting: " + db_.startup_status() + ")");
          continue;
        }
      }
      send_err(client_conn, 1, 3168, "HY000", "ASQL is starting: " + db_.startup_status() + ". It answers queries once it is up.");
      continue;
    }
    if (cmd == COM_PROCESS_INFO) {
      // The old command form of SHOW PROCESSLIST: answered as that statement.
      static const char kShow[] = "SHOW PROCESSLIST";
      cmd_pkt.assign(kShow, kShow + sizeof(kShow) - 1);
      cmd_pkt.insert(cmd_pkt.begin(), COM_QUERY);
      cmd = COM_QUERY;
    }
    if (cmd == COM_QUIT) {
      break;
    } else if (cmd == COM_PING) {
      send_ok(client_conn, 1);
    } else if (cmd == COM_INIT_DB) {
      std::string new_db((const char*)&cmd_pkt[1], cmd_pkt.size() - 1);
      if (cap_session) {
        std::string low = new_db;
        for (char& c : low) c = (char)::tolower((unsigned char)c);
        if (!authority.names_db(low)) {
          send_err(client_conn, 1, 1044, "42000", "ASQL capability: no capability for database " + low);
          continue;
        }
      }
      session_database = new_db;
      SqlDatabase::update_active_session(conn_id, "Sleep", "idle", "", session_database);
      send_ok(client_conn, 1);
    } else if (cmd == COM_REGISTER_SLAVE) {
      if (cmd_pkt.size() >= 5) {
        SlaveInfo slave;
        slave.server_id = get_u32(&cmd_pkt[1]);
        size_t off = 5;
        if (off < cmd_pkt.size()) {
          uint8_t host_len = cmd_pkt[off++];
          if (off + host_len <= cmd_pkt.size()) {
            slave.host.assign((const char*)&cmd_pkt[off], host_len);
            off += host_len;
          }
        }
        if (off < cmd_pkt.size()) {
          uint8_t user_len = cmd_pkt[off++];
          if (off + user_len <= cmd_pkt.size()) {
            slave.user.assign((const char*)&cmd_pkt[off], user_len);
            off += user_len;
          }
        }
        if (off < cmd_pkt.size()) {
          uint8_t pass_len = cmd_pkt[off++];
          off += pass_len;
        }
        if (off + 2 <= cmd_pkt.size()) {
          slave.port = get_u16(&cmd_pkt[off]);
          off += 2;
        }
        if (off + 8 <= cmd_pkt.size()) {
          off += 4;
          slave.master_id = get_u32(&cmd_pkt[off]);
        }
        slave.registered_time_ns = kernel_.host().monotonic_ns();
        register_slave(slave);
      }
      send_ok(client_conn, 1);
    } else if (cmd == COM_BINLOG_DUMP || cmd == COM_BINLOG_DUMP_GTID) {
      // A replica reading this server's own binlog (SqlDatabase::enable_binlog):
      // an artificial ROTATE naming where it starts, the file's
      // FORMAT_DESCRIPTION, then every event from there on, following the
      // files into the next one and waiting at the end of the newest, with
      // a heartbeat each idle second.
      AsqlBinlog* bl = db_.binlog_writer();
      if (!bl) {
        send_err(client_conn, 1, 1236, "HY000", "Binary logging is not enabled on this server (start aios-engine with --log-bin)");
        continue;
      }
      if (cmd == COM_BINLOG_DUMP_GTID) {
        send_err(client_conn, 1, 1236, "HY000", "This server has no GTIDs: replicate from a file and position (MASTER_AUTO_POSITION=0)");
        continue;
      }
      uint64_t binlog_pos = 4;
      uint16_t flags = 0;
      std::string binlog_file;
      if (cmd_pkt.size() >= 11) {
        binlog_pos = get_u32(&cmd_pkt[1]);
        flags = get_u16(&cmd_pkt[5]);
        if (cmd_pkt.size() > 11) binlog_file.assign((const char*)&cmd_pkt[11], cmd_pkt.size() - 11);
      }
      while (!binlog_file.empty() && binlog_file.back() == '\0') binlog_file.pop_back();
      if (binlog_file.empty()) {   // the oldest file there is
        const auto fl = bl->files();
        if (!fl.empty()) binlog_file = fl.front().name;
      }
      if (binlog_pos < 4) binlog_pos = 4;
      uint64_t end = 0;
      bool active = false;
      std::string next;
      if (!bl->readable_end(binlog_file, &end, &active, &next)) {
        send_err(client_conn, 1, 1236, "HY000", "Could not find first log file name in binary log index file");
        continue;
      }
      if (binlog_pos > end) {
        send_err(client_conn, 1, 1236, "HY000", "Client requested master to start replication from position > file size");
        continue;
      }
      SqlDatabase::update_active_session(conn_id, "Binlog Dump", "streaming", binlog_file + ":" + std::to_string(binlog_pos), session_database);
      uint8_t rep_seq = 1;
      bool gone = false;
      auto send_event = [&](const uint8_t* ev, size_t n) {
        std::vector<uint8_t> pkt(1 + n);
        pkt[0] = 0x00;
        std::memcpy(pkt.data() + 1, ev, n);
        if (!send_packet(client_conn, pkt.data(), pkt.size(), rep_seq).ok()) gone = true;
        rep_seq = (uint8_t)(rep_seq + (pkt.size() >= 0xffffff ? pkt.size() / 0xffffff + 1 : 1));
      };
      auto artificial = [&](uint8_t type, const std::string& body, uint32_t log_pos) {
        std::vector<uint8_t> ev;
        put_u32(&ev, 0);
        ev.push_back(type);
        put_u32(&ev, bl->server_id());
        put_u32(&ev, 19 + (uint32_t)body.size());
        put_u32(&ev, log_pos);
        put_u16(&ev, 0x20);   // LOG_EVENT_ARTIFICIAL_F
        ev.insert(ev.end(), body.begin(), body.end());
        send_event(ev.data(), ev.size());
      };
      auto rotate_to = [&](const std::string& file, uint64_t pos) {
        std::string body(8, '\0');
        for (int i = 0; i < 8; ++i) body[i] = (char)((pos >> (8 * i)) & 0xff);
        artificial(4, body + file, 0);
      };
      std::string file = binlog_file;
      uint64_t pos = binlog_pos;
      int fd = ::open((bl->dir() + "/" + file).c_str(), O_RDONLY | O_CLOEXEC);
      if (fd < 0) {
        send_err(client_conn, 1, 1236, "HY000", "Could not open log file " + file);
        continue;
      }
      rotate_to(file, pos);
      if (pos > 4) {
        // Its FORMAT_DESCRIPTION first, with no position: the replica does not move on it.
        uint8_t h[19];
        if (::pread(fd, h, 19, 4) == 19) {
          const uint32_t len = get_u32(h + 9);
          std::vector<uint8_t> fde(len);
          if (len >= 19 && ::pread(fd, fde.data(), len, 4) == (ssize_t)len) {
            std::memset(fde.data() + 13, 0, 4);
            send_event(fde.data(), fde.size());
          }
        }
      }
      std::vector<uint8_t> buf;
      uint32_t idle_ms = 0;
      while (running_ && client_conn.alive() && !gone) {
        if (!bl->readable_end(file, &end, &active, &next)) {   // purged under the reader: it reads its open file to the end
          end = (uint64_t)::lseek(fd, 0, SEEK_END);
          active = false;
          next.clear();
          // On to the oldest file still there after it, not to nowhere: with no next file the reader at the end of
          // its purged file waited on a file that never grows and went round this loop without sleeping, heartbeating.
          for (const auto& f : bl->files())
            if (f.name > file && (next.empty() || f.name < next)) next = f.name;
        }
        if (pos + 19 <= end) {
          uint8_t h[19];
          if (::pread(fd, h, 19, (off_t)pos) != 19) break;
          const uint32_t len = get_u32(h + 9);
          if (len < 19 || pos + len > end) break;
          buf.resize(len);
          if (::pread(fd, buf.data(), len, (off_t)pos) != (ssize_t)len) break;
          send_event(buf.data(), len);
          pos += len;
          idle_ms = 0;
          if (h[4] == 4 /* ROTATE */ && !active && !next.empty()) {   // on into the next file, from its start
            ::close(fd);
            file = next;
            pos = 4;
            fd = ::open((bl->dir() + "/" + file).c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) break;
          }
          continue;
        }
        if (!active && !next.empty()) {   // a file left without its ROTATE: the replica is told
          ::close(fd);
          file = next;
          pos = 4;
          rotate_to(file, pos);
          fd = ::open((bl->dir() + "/" + file).c_str(), O_RDONLY | O_CLOEXEC);
          if (fd < 0) break;
          continue;
        }
        if (flags & 0x01) {   // BINLOG_DUMP_NON_BLOCK: what there is, then EOF
          std::vector<uint8_t> eof = {0xfe, 0x00, 0x00, 0x02, 0x00};
          send_packet(client_conn, eof.data(), eof.size(), rep_seq);
          break;
        }
        // Caught up: what was queued goes out now (the queue otherwise waits
        // for 64 KB or the end of a command, and a dump never ends).
        if (!wire_flush(client_conn)) break;
        bl->wait_past(file, pos, 250);
        idle_ms += 250;
        if (idle_ms >= 1000) {
          idle_ms = 0;
          artificial(0x1b /* HEARTBEAT */, file, (uint32_t)pos);
          if (!wire_flush(client_conn)) break;
        }
        SqlDatabase::update_active_session(conn_id, "Binlog Dump", "streaming", file + ":" + std::to_string(pos), session_database);
      }
      if (fd >= 0) ::close(fd);
      SqlDatabase::update_active_session(conn_id, "Sleep", "idle", "", session_database);
    } else if (cmd == COM_FIELD_LIST) {
      size_t null_pos = 1;
      while (null_pos < cmd_pkt.size() && cmd_pkt[null_pos] != 0) null_pos++;
      std::string tbl_name((const char*)&cmd_pkt[1], null_pos > 1 ? (null_pos - 1) : 0);
      std::string wildcard;
      if (null_pos + 1 < cmd_pkt.size()) {
        wildcard.assign((const char*)&cmd_pkt[null_pos + 1], cmd_pkt.size() - (null_pos + 1));
      }
      std::string eff_db = session_database.empty() ? db_.effective_database() : session_database;
      const SqlTable* tbl = db_.resolve_table(tbl_name, eff_db);
      if (!tbl) {
        send_err(client_conn, 1, 1146, "42S02", "Table '" + eff_db + "." + tbl_name + "' doesn't exist");
        continue;
      }

      uint8_t curr_seq = 1;
      for (const auto& c : tbl->columns) {
        if (!wildcard.empty() && !like_match(c.name, wildcard)) {
          continue;
        }
        std::vector<uint8_t> cdef;
        put_lenenc_str(&cdef, "def");
        put_lenenc_str(&cdef, eff_db);
        put_lenenc_str(&cdef, tbl_name);
        put_lenenc_str(&cdef, tbl_name);
        put_lenenc_str(&cdef, c.name);
        put_lenenc_str(&cdef, c.name);
        cdef.push_back(0x0c);
        put_u16(&cdef, 45);
        const bool is_dec = c.type == SqlDataType::Decimal;
        uint32_t col_len = c.type == SqlDataType::Int ? 11 : (c.type == SqlDataType::Real ? 22 : (is_dec ? (uint32_t)c.precision + 2 : 65535));
        put_u32(&cdef, col_len);
        uint8_t mysql_type = c.type == SqlDataType::Int ? MYSQL_TYPE_LONG : (c.type == SqlDataType::Real ? MYSQL_TYPE_DOUBLE : (is_dec ? 0xf6 /* NEWDECIMAL */ : MYSQL_TYPE_VAR_STRING));
        cdef.push_back(mysql_type);
        uint16_t flags = (c.not_null ? 0x0001 : 0x0000) | (c.is_primary_key ? 0x0002 : 0x0000) | (c.auto_increment ? 0x0200 : 0x0000);
        put_u16(&cdef, flags);
        cdef.push_back(c.type == SqlDataType::Real ? 31 : (is_dec ? c.scale : 0));
        put_u16(&cdef, 0);
        if (column_has_default(c.default_value)) {
          put_lenenc_str(&cdef, column_default_text(c.default_value));
        }
        send_packet(client_conn, cdef.data(), cdef.size(), curr_seq++);
      }
      std::vector<uint8_t> eof = {0xfe, 0x00, 0x00, 0x02, 0x00};
      send_packet(client_conn, eof.data(), eof.size(), curr_seq++);
    } else if (cmd == COM_STMT_PREPARE) {
      std::string query_text((const char*)&cmd_pkt[1], cmd_pkt.size() - 1);
      if (cs_in_latin1 && has_high_byte(query_text)) query_text = cp1252_to_utf8(query_text);
      if (const std::string why = capability_refusal(sql_params_as_null(query_text)); !why.empty()) {
        send_err(client_conn, 1, 1227, "42000", "ASQL capability: " + why);
        continue;
      }
      size_t num_params = 0;
      bool in_quote = false;
      char quote_char = 0;
      for (size_t i = 0; i < query_text.size(); ++i) {
        char c = query_text[i];
        if (in_quote) {
          if (c == '\\' && quote_char != '`') { ++i; continue; }
          if (c == quote_char) in_quote = false;
        } else {
          if (c == '\'' || c == '"' || c == '`') {
            in_quote = true;
            quote_char = c;
          } else if (c == '?') {
            num_params++;
          }
        }
      }

      uint32_t stmt_id = next_stmt_id++;
      WirePreparedStatement stmt;
      stmt.id = stmt_id;
      stmt.sql = std::move(query_text);
      stmt.num_params = num_params;
      stmt.long_data.assign(num_params, std::string());
      stmt.has_long_data.assign(num_params, false);
      prepared_stmts[stmt_id] = std::move(stmt);

      // COM_STMT_PREPARE_OK: [0x00] [4b stmt_id] [2b num_columns] [2b num_params] [1b 0x00] [2b warnings].
      // A SELECT's columns are announced here as MariaDB does (sysbench and
      // libmysqlclient decide from this count whether a result set follows);
      // a statement whose columns are only known when it runs (SHOW, CALL)
      // announces none, as MariaDB does for a prepared SHOW, and the client
      // reads the definitions from the execute response.
      const std::string prep_db = session_database.empty() ? db_.effective_database() : session_database;
      const std::vector<std::vector<uint8_t>> prep_cols =
          prepared_result_columns(db_, prepared_stmts[stmt_id].sql, session_database, prep_db);
      std::vector<uint8_t> prep_ok;
      prep_ok.push_back(0x00);
      put_u32(&prep_ok, stmt_id);
      put_u16(&prep_ok, (uint16_t)prep_cols.size());
      put_u16(&prep_ok, (uint16_t)num_params);
      prep_ok.push_back(0x00);
      put_u16(&prep_ok, 0);
      send_packet(client_conn, prep_ok.data(), prep_ok.size(), 1);

      uint8_t cur_seq = 2;
      if (num_params > 0) {
        // Each placeholder as MariaDB describes it: an untyped NULL column named "?".
        for (size_t p = 0; p < num_params; ++p) {
          std::vector<uint8_t> pdef;
          put_lenenc_str(&pdef, "def");
          put_lenenc_str(&pdef, "");
          put_lenenc_str(&pdef, "");
          put_lenenc_str(&pdef, "");
          put_lenenc_str(&pdef, "?");
          put_lenenc_str(&pdef, "");
          pdef.push_back(0x0c);
          put_u16(&pdef, 63);
          put_u32(&pdef, 0);
          pdef.push_back(MYSQL_TYPE_NULL);
          put_u16(&pdef, 0x80);
          pdef.push_back(0);
          put_u16(&pdef, 0);
          send_packet(client_conn, pdef.data(), pdef.size(), cur_seq++);
        }
        std::vector<uint8_t> eof = {0xfe, 0x00, 0x00, 0x02, 0x00};
        send_packet(client_conn, eof.data(), eof.size(), cur_seq++);
      }
      if (!prep_cols.empty()) {
        for (const auto& def : prep_cols) send_packet(client_conn, def.data(), def.size(), cur_seq++);
        std::vector<uint8_t> eof = {0xfe, 0x00, 0x00, 0x02, 0x00};
        send_packet(client_conn, eof.data(), eof.size(), cur_seq++);
      }
    } else if (cmd == COM_STMT_SEND_LONG_DATA) {
      // [4b stmt_id] [2b param] [bytes], no reply; the chunks are the
      // parameter's value at the next execute.
      if (cmd_pkt.size() >= 7) {
        auto it = prepared_stmts.find(get_u32(&cmd_pkt[1]));
        const size_t p = get_u16(&cmd_pkt[5]);
        if (it != prepared_stmts.end() && p < it->second.num_params) {
          it->second.long_data[p].append((const char*)&cmd_pkt[7], cmd_pkt.size() - 7);
          it->second.has_long_data[p] = true;
        }
      }
    } else if (cmd == COM_STMT_EXECUTE) {
      if (cmd_pkt.size() < 10) {
        send_err(client_conn, 1, 1064, "42000", "malformed COM_STMT_EXECUTE packet");
        continue;
      }
      uint32_t stmt_id = get_u32(&cmd_pkt[1]);
      auto it = prepared_stmts.find(stmt_id);
      if (it == prepared_stmts.end()) {
        send_err(client_conn, 1, 1243, "HY000", "Unknown prepared statement handler (" + std::to_string(stmt_id) + ") given to mysqld_stmt_execute");
        continue;
      }
      auto& stmt = it->second;
      size_t offset = 10;
      std::vector<std::string> bound_values;
      bound_values.reserve(stmt.num_params);
      bool malformed = false;

      if (stmt.num_params > 0) {
        size_t null_bitmap_len = (stmt.num_params + 7) / 8;
        if (offset + null_bitmap_len + 1 > cmd_pkt.size()) {
          send_err(client_conn, 1, 1064, "42000", "truncated COM_STMT_EXECUTE payload");
          continue;
        }
        const uint8_t* null_bitmap = &cmd_pkt[offset];
        offset += null_bitmap_len;
        uint8_t new_params_bind_flag = cmd_pkt[offset++];
        if (new_params_bind_flag != 0) {
          if (offset + 2 * stmt.num_params > cmd_pkt.size()) {
            send_err(client_conn, 1, 1064, "42000", "truncated type flags in COM_STMT_EXECUTE");
            continue;
          }
          stmt.param_types.resize(stmt.num_params);
          stmt.param_unsigned.resize(stmt.num_params);
          for (size_t p = 0; p < stmt.num_params; ++p) {
            stmt.param_types[p] = cmd_pkt[offset];
            stmt.param_unsigned[p] = (cmd_pkt[offset + 1] & 0x80) != 0;
            offset += 2; // type + flags
          }
        }

        auto need = [&](size_t n) {
          if (offset + n <= cmd_pkt.size()) return true;
          malformed = true;
          return false;
        };
        char num[64];
        for (size_t p = 0; p < stmt.num_params && !malformed; ++p) {
          // Long data wins over the null bit, as on MariaDB: mysqlnd binds a
          // `b` parameter to a NULL value, sends the bytes ahead with
          // COM_STMT_SEND_LONG_DATA, and sets the null bit at execute.
          if (stmt.has_long_data[p]) {
            // Sent ahead; not in the value block.
            bound_values.push_back(quote_literal(stmt.long_data[p]));
            continue;
          }
          const bool is_null = (null_bitmap[p / 8] & (1 << (p % 8))) != 0;
          if (is_null) { bound_values.push_back("NULL"); continue; }
          const uint8_t ptype = p < stmt.param_types.size() ? stmt.param_types[p] : MYSQL_TYPE_VAR_STRING;
          const bool uns = p < stmt.param_unsigned.size() && stmt.param_unsigned[p];
          switch (ptype) {
            case MYSQL_TYPE_NULL:
              bound_values.push_back("NULL");
              break;
            case MYSQL_TYPE_TINY:
              if (!need(1)) break;
              bound_values.push_back(std::to_string(uns ? (long long)cmd_pkt[offset] : (long long)(int8_t)cmd_pkt[offset]));
              offset += 1;
              break;
            case MYSQL_TYPE_SHORT:
            case MYSQL_TYPE_YEAR:
              if (!need(2)) break;
              bound_values.push_back(std::to_string(uns ? (long long)get_u16(&cmd_pkt[offset]) : (long long)(int16_t)get_u16(&cmd_pkt[offset])));
              offset += 2;
              break;
            case MYSQL_TYPE_LONG:
            case MYSQL_TYPE_INT24:
              if (!need(4)) break;
              bound_values.push_back(std::to_string(uns ? (long long)get_u32(&cmd_pkt[offset]) : (long long)(int32_t)get_u32(&cmd_pkt[offset])));
              offset += 4;
              break;
            case MYSQL_TYPE_LONGLONG:
              if (!need(8)) break;
              bound_values.push_back(uns ? std::to_string(get_u64(&cmd_pkt[offset])) : std::to_string((long long)get_u64(&cmd_pkt[offset])));
              offset += 8;
              break;
            case MYSQL_TYPE_FLOAT: {
              if (!need(4)) break;
              float v;
              std::memcpy(&v, &cmd_pkt[offset], 4);
              offset += 4;
              std::snprintf(num, sizeof num, "%.9g", (double)v);
              bound_values.push_back(num);
              break;
            }
            case MYSQL_TYPE_DOUBLE: {
              if (!need(8)) break;
              double v;
              std::memcpy(&v, &cmd_pkt[offset], 8);
              offset += 8;
              std::snprintf(num, sizeof num, "%.17g", v);
              bound_values.push_back(num);
              break;
            }
            case MYSQL_TYPE_DATE:
            case MYSQL_TYPE_DATETIME:
            case MYSQL_TYPE_TIMESTAMP: {
              // [len] [year 2] [month] [day] [hour] [min] [sec] [micro 4]; len 0 is the zero date.
              if (!need(1)) break;
              const uint8_t len = cmd_pkt[offset++];
              if (!need(len)) break;
              unsigned y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
              uint32_t us = 0;
              if (len >= 4) { y = get_u16(&cmd_pkt[offset]); mo = cmd_pkt[offset + 2]; d = cmd_pkt[offset + 3]; }
              if (len >= 7) { h = cmd_pkt[offset + 4]; mi = cmd_pkt[offset + 5]; s = cmd_pkt[offset + 6]; }
              if (len >= 11) us = get_u32(&cmd_pkt[offset + 7]);
              offset += len;
              if (ptype == MYSQL_TYPE_DATE) std::snprintf(num, sizeof num, "'%04u-%02u-%02u'", y, mo, d);
              else if (us) std::snprintf(num, sizeof num, "'%04u-%02u-%02u %02u:%02u:%02u.%06u'", y, mo, d, h, mi, s, (unsigned)us);
              else std::snprintf(num, sizeof num, "'%04u-%02u-%02u %02u:%02u:%02u'", y, mo, d, h, mi, s);
              bound_values.push_back(num);
              break;
            }
            case MYSQL_TYPE_TIME: {
              // [len] [neg] [days 4] [hour] [min] [sec] [micro 4]; len 0 is 00:00:00.
              if (!need(1)) break;
              const uint8_t len = cmd_pkt[offset++];
              if (!need(len)) break;
              bool neg = false;
              uint32_t days = 0, us = 0;
              unsigned h = 0, mi = 0, s = 0;
              if (len >= 8) { neg = cmd_pkt[offset] != 0; days = get_u32(&cmd_pkt[offset + 1]); h = cmd_pkt[offset + 5]; mi = cmd_pkt[offset + 6]; s = cmd_pkt[offset + 7]; }
              if (len >= 12) us = get_u32(&cmd_pkt[offset + 8]);
              offset += len;
              const unsigned hours = (unsigned)days * 24u + h;
              if (us) std::snprintf(num, sizeof num, "'%s%02u:%02u:%02u.%06u'", neg ? "-" : "", hours, mi, s, (unsigned)us);
              else std::snprintf(num, sizeof num, "'%s%02u:%02u:%02u'", neg ? "-" : "", hours, mi, s);
              bound_values.push_back(num);
              break;
            }
            default: {
              // String / decimal / blob / anything else: a length-encoded string.
              size_t bytes_read = 0;
              const uint64_t str_len = get_lenenc_u64(&cmd_pkt[offset], cmd_pkt.size() - offset, &bytes_read);
              offset += bytes_read;
              if (!need(str_len)) break;
              std::string raw((const char*)&cmd_pkt[offset], str_len);
              offset += str_len;
              if (cs_in_latin1 && has_high_byte(raw)) raw = cp1252_to_utf8(raw);
              bound_values.push_back(quote_literal(raw));
              break;
            }
          }
        }
        // Long data is for one execute; the next one sends it again.
        for (size_t p = 0; p < stmt.num_params; ++p) {
          if (stmt.has_long_data[p]) { std::string().swap(stmt.long_data[p]); stmt.has_long_data[p] = false; }
        }
      }
      if (malformed) {
        send_err(client_conn, 1, 1210, "HY000", "Incorrect arguments to mysqld_stmt_execute");
        continue;
      }

      std::string bound_sql;
      bound_sql.reserve(stmt.sql.size() + 64);
      {
        size_t pidx = 0;
        bool in_quote = false;
        char quote_char = 0;
        for (size_t i = 0; i < stmt.sql.size(); ++i) {
          const char c = stmt.sql[i];
          if (in_quote) {
            bound_sql.push_back(c);
            if (c == '\\' && quote_char != '`' && i + 1 < stmt.sql.size()) { bound_sql.push_back(stmt.sql[++i]); continue; }
            if (c == quote_char) in_quote = false;
          } else if (c == '\'' || c == '"' || c == '`') {
            in_quote = true;
            quote_char = c;
            bound_sql.push_back(c);
          } else if (c == '?') {
            bound_sql += pidx < bound_values.size() ? bound_values[pidx++] : std::string("NULL");
          } else {
            bound_sql.push_back(c);
          }
        }
      }

      // A cached answer already encoded for the binary protocol goes out as
      // the stored bytes, the same way a COM_QUERY hit does.
      if (const std::string why = capability_refusal(bound_sql); !why.empty()) {
        send_err(client_conn, 1, 1227, "42000", "ASQL capability: " + why);
        continue;
      }
      const std::string wire_db = session_database.empty() ? db_.effective_database() : session_database;
      std::shared_ptr<const std::string> packets;
      if (!cap_session) packets = db_.cached_wire_packets(bound_sql, session_database, wire_db, true, cs_out_latin1);
      if (packets) {
        db_.metrics().bytes_sent.fetch_add(packets->size(), std::memory_order_relaxed);
        wire_queue(client_conn, ByteSpan(reinterpret_cast<const uint8_t*>(packets->data()), packets->size()));
        continue;
      }

      SqlDatabase::update_active_session(conn_id, "Execute", "executing", bound_sql, session_database);
      const uint64_t cache_gen = db_.query_cache_generation();
      sql_take_statement_insert_id();
      sql_set_wire_results(true);
      uint8_t stream_seq = 1;
      WireRowSink row_sink(true, cs_out_latin1, wire_db, SERVER_STATUS_AUTOCOMMIT, stream_seq,
                           [&](const uint8_t* p, size_t n) {
                             db_.metrics().bytes_sent.fetch_add(n, std::memory_order_relaxed);
                             return wire_queue(client_conn, ByteSpan(p, n));
                           },
                           [&]() { return wire_flush(client_conn); });
      sql_set_row_sink(cap_session ? nullptr : &row_sink);
      std::shared_ptr<const SqlQueryResult> res_shared = db_.execute_gateway_query_shared(bound_sql, session_database);
      sql_set_row_sink(nullptr);
      sql_set_wire_results(false);
      const SqlQueryResult& res = *res_shared;
      if (cap_session && res.success && !charge_rows(res.is_result_set ? res.rows.size() : res.rows_affected)) {
        res_shared.reset();
        send_err(client_conn, 1, 1227, "42000", "ASQL capability: row budget exhausted (RESOURCE_LIMIT_EXCEEDED)");
        SqlDatabase::update_active_session(conn_id, "Sleep", "idle", "", session_database);
        continue;
      }
      if (res.streamed) {
        res_shared.reset();
        SqlDatabase::update_active_session(conn_id, "Sleep", "idle", "", session_database);
        continue;
      }
      if (!res.success) {
        send_engine_error(1, res.error_message.empty() ? "execution error" : res.error_message);
        res_shared.reset();
        SqlDatabase::update_active_session(conn_id, "Sleep", "idle", "", session_database);
        continue;
      }
      if (!res.is_result_set) {
        send_ok(client_conn, 1, (client_flags & CAP_FOUND_ROWS) && res.rows_matched >= 0 ? (uint64_t)res.rows_matched : res.rows_affected,
                sql_take_statement_insert_id(), SERVER_STATUS_AUTOCOMMIT, res.ok_info);
        res_shared.reset();
        SqlDatabase::update_active_session(conn_id, "Sleep", "idle", "", session_database);
        continue;
      }

      uint8_t curr_seq = 1;
      std::string capture;
      bool captured = false;
      const bool sent_ok = stream_result_set(res, true, cs_out_latin1, wire_db, SERVER_STATUS_AUTOCOMMIT, curr_seq,
                                             &capture, &captured,
                                             [&](const uint8_t* p, size_t n) {
                                               db_.metrics().bytes_sent.fetch_add(n, std::memory_order_relaxed);
                                               return wire_queue(client_conn, ByteSpan(p, n));
                                             },
                                             [&]() { return wire_flush(client_conn); });
      res_shared.reset();
      if (sent_ok && captured) db_.attach_cached_wire_packets(bound_sql, session_database, wire_db, std::move(capture), cache_gen, true, cs_out_latin1);
      SqlDatabase::update_active_session(conn_id, "Sleep", "idle", "", session_database);
      wire_flush(client_conn);
    } else if (cmd == COM_STMT_CLOSE) {
      if (cmd_pkt.size() >= 5) {
        uint32_t stmt_id = get_u32(&cmd_pkt[1]);
        prepared_stmts.erase(stmt_id);
      }
      // No response packet for COM_STMT_CLOSE
    } else if (cmd == COM_STMT_RESET) {
      if (cmd_pkt.size() >= 5) {
        auto rit = prepared_stmts.find(get_u32(&cmd_pkt[1]));
        if (rit == prepared_stmts.end()) {
          send_err(client_conn, 1, 1243, "HY000", "Unknown prepared statement handler given to mysqld_stmt_reset");
          continue;
        }
        for (size_t p = 0; p < rit->second.num_params; ++p) {
          std::string().swap(rit->second.long_data[p]);
          rit->second.has_long_data[p] = false;
        }
      }
      send_ok(client_conn, 1);
    } else if (cmd == COM_STMT_FETCH) {
      // No statement here is ever opened as a cursor (execute streams every row).
      send_err(client_conn, 1, 1235, "42000", "This version of ASQL doesn't yet support 'cursor fetch'");
    } else if (cmd == COM_QUERY) {
      std::string query_text((const char*)&cmd_pkt[1], cmd_pkt.size() - 1);
      {
        // SET NAMES / character_set_*: decided on the client's own bytes, which
        // are ASCII for any statement that names a charset. Honoured whatever
        // the announced default: mysqli's set_charset('latin1') is this statement.
        if (parse_charset_set(query_text, &cs_in_latin1, &cs_out_latin1)) SqlDatabase::set_session_latin1(cs_in_latin1, cs_out_latin1);
        if (cs_in_latin1 && has_high_byte(query_text)) query_text = cp1252_to_utf8(query_text);
      }
      if (capability_statement(query_text)) continue;
      if (const std::string why = capability_refusal(query_text); !why.empty()) {
        send_err(client_conn, 1, 1227, "42000", "ASQL capability: " + why);
        continue;
      }
      // Only the short client-init statements below are compared whole, so only
      // those are upper-cased; every other statement was copied and
      // upper-cased in full just to fail these checks.
      std::string upper_q;
      if (query_text.size() <= 40) {
        upper_q = query_text;
        for (auto& c : upper_q) c = (char)::toupper((unsigned char)c);
      }
      size_t first_non_ws = 0;
      while (first_non_ws < query_text.size() && (query_text[first_non_ws] == ' ' || query_text[first_non_ws] == '\t' || query_text[first_non_ws] == '\r' || query_text[first_non_ws] == '\n')) {
        first_non_ws++;
      }
      if (first_non_ws + 4 <= query_text.size() && ::toupper((unsigned char)query_text[first_non_ws]) == 'U' &&
          ::toupper((unsigned char)query_text[first_non_ws + 1]) == 'S' &&
          ::toupper((unsigned char)query_text[first_non_ws + 2]) == 'E' && query_text[first_non_ws + 3] == ' ') {
        size_t p = first_non_ws + 4;
        while (p < query_text.size() && (query_text[p] == ' ' || query_text[p] == '\t' || query_text[p] == '`' || query_text[p] == '\'' || query_text[p] == '"')) p++;
        size_t end = p;
        while (end < query_text.size() && query_text[end] != ' ' && query_text[end] != '\t' && query_text[end] != ';' && query_text[end] != '`' && query_text[end] != '\'' && query_text[end] != '"') end++;
        if (end > p) {
          session_database = query_text.substr(p, end - p);
        }
      }

      // Intercept standard MySQL client init/system variables queries
      if (upper_q == "SELECT @@VERSION_COMMENT" || upper_q == "SELECT @@VERSION_COMMENT;" ||
          upper_q == "SELECT @@VERSION_COMMENT LIMIT 1" || upper_q == "SELECT @@VERSION_COMMENT LIMIT 1;") {
        SqlQueryResult r;
        r.columns = {"@@version_comment"};
        r.rows.push_back(SqlRow{{"AIOS ASQL / MySQL Native Wire Server"}});
        r.is_result_set = true;
        r.success = true;

        // Send Column Count
        std::vector<uint8_t> ccp;
        put_lenenc_u64(&ccp, 1);
        send_packet(client_conn, ccp.data(), ccp.size(), 1);

        // Send Column Def
        std::vector<uint8_t> cdef;
        put_lenenc_str(&cdef, "def");
        put_lenenc_str(&cdef, "");
        put_lenenc_str(&cdef, "");
        put_lenenc_str(&cdef, "");
        put_lenenc_str(&cdef, "@@version_comment");
        put_lenenc_str(&cdef, "@@version_comment");
        cdef.push_back(0x0c);  // len of fixed fields (12)
        put_u16(&cdef, 45);    // charset utf8mb4
        put_u32(&cdef, 255);   // col length
        cdef.push_back(MYSQL_TYPE_VAR_STRING);
        put_u16(&cdef, 0);     // flags
        cdef.push_back(0);     // decimals
        put_u16(&cdef, 0);     // filler
        send_packet(client_conn, cdef.data(), cdef.size(), 2);

        // Send intermediate EOF
        std::vector<uint8_t> eof = {0xfe, 0x00, 0x00, 0x02, 0x00};
        send_packet(client_conn, eof.data(), eof.size(), 3);

        // Send Row
        std::vector<uint8_t> rowpkt;
        put_lenenc_str(&rowpkt, "AIOS ASQL / MySQL Native Wire Server");
        send_packet(client_conn, rowpkt.data(), rowpkt.size(), 4);

        // Send terminating EOF
        send_packet(client_conn, eof.data(), eof.size(), 5);
        continue;
      }

      if (upper_q == "SELECT DATABASE()" || upper_q == "SELECT DATABASE();" ||
          upper_q == "SELECT DATABASE() LIMIT 1" || upper_q == "SELECT DATABASE() LIMIT 1;") {
        std::string cur_db = session_database.empty() ? db_.effective_database() : session_database;
        SqlQueryResult r;
        r.columns = {"DATABASE()"};
        r.rows.push_back(SqlRow{{cur_db}});
        r.is_result_set = true;
        r.success = true;

        std::vector<uint8_t> ccp;
        put_lenenc_u64(&ccp, 1);
        send_packet(client_conn, ccp.data(), ccp.size(), 1);

        std::vector<uint8_t> cdef;
        put_lenenc_str(&cdef, "def");
        put_lenenc_str(&cdef, "");
        put_lenenc_str(&cdef, "");
        put_lenenc_str(&cdef, "");
        put_lenenc_str(&cdef, "DATABASE()");
        put_lenenc_str(&cdef, "DATABASE()");
        cdef.push_back(0x0c);
        put_u16(&cdef, 45);
        put_u32(&cdef, 255);
        cdef.push_back(MYSQL_TYPE_VAR_STRING);
        put_u16(&cdef, 0);
        cdef.push_back(0);
        put_u16(&cdef, 0);
        send_packet(client_conn, cdef.data(), cdef.size(), 2);

        std::vector<uint8_t> eof = {0xfe, 0x00, 0x00, 0x02, 0x00};
        send_packet(client_conn, eof.data(), eof.size(), 3);

        std::vector<uint8_t> rowpkt;
        put_lenenc_str(&rowpkt, cur_db);
        send_packet(client_conn, rowpkt.data(), rowpkt.size(), 4);

        send_packet(client_conn, eof.data(), eof.size(), 5);
        continue;
      }

      // Several statements in one COM_QUERY, when the client asked for that:
      // each answer follows the last, its closing packet flagged
      // SERVER_MORE_RESULTS_EXISTS, sequence numbers running on through the
      // whole reply. The engine itself takes one statement at a time.
      std::vector<std::string> stmts;
      if (multi_statements && std::memchr(query_text.data(), ';', query_text.size())) stmts = split_statements(query_text);
      const bool multi = stmts.size() > 1;
      if (!multi) { stmts.clear(); stmts.push_back(query_text); }
      const std::string wire_db = session_database.empty() ? db_.effective_database() : session_database;
      uint8_t curr_seq = 1;
      for (size_t si = 0; si < stmts.size(); ++si) {
        const std::string& stmt_text = multi ? stmts[si] : query_text;
        const bool last = si + 1 == stmts.size();
        const uint16_t status = SERVER_STATUS_AUTOCOMMIT | (last ? 0 : SERVER_MORE_RESULTS_EXISTS);

        // A statement whose answer is cached AND already encoded goes out as
        // the stored bytes: no lock, no copy of the rows, no per-row encoding.
        // The stored bytes are UTF-8 with sequence numbers from 1; a latin1
        // session needs its own encoding, and must not leave cp1252 bytes
        // behind for a UTF-8 session to be served.
        if (!multi && !cap_session) {
          std::shared_ptr<const std::string> packets;
          packets = db_.cached_wire_packets(stmt_text, session_database, wire_db, false, cs_out_latin1);
          if (packets) {
            db_.metrics().bytes_sent.fetch_add(packets->size(), std::memory_order_relaxed);
            wire_queue(client_conn, ByteSpan(reinterpret_cast<const uint8_t*>(packets->data()), packets->size()));
            break;
          }
        }

        SqlDatabase::update_active_session(conn_id, "Query", "executing", stmt_text, session_database);
        // Execute SQL / ASQL query on AIOS database engine (gateway / write-back aware)
        const uint64_t cache_gen = db_.query_cache_generation();
        static const bool wire_trace = std::getenv("AIOS_VX_TRACE") != nullptr;
        const auto wt0 = wire_trace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
        sql_take_statement_insert_id();
        sql_set_wire_results(true);
        WireRowSink row_sink(false, cs_out_latin1, wire_db, status, curr_seq,
                             [&](const uint8_t* p, size_t n) {
                               db_.metrics().bytes_sent.fetch_add(n, std::memory_order_relaxed);
                               return wire_queue(client_conn, ByteSpan(p, n));
                             },
                             [&]() { return wire_flush(client_conn); });
        sql_set_row_sink(cap_session ? nullptr : &row_sink);
        std::shared_ptr<const SqlQueryResult> res_shared = db_.execute_gateway_query_shared(stmt_text, session_database);
        sql_set_row_sink(nullptr);
        sql_set_wire_results(false);
        const auto wt1 = wire_trace ? std::chrono::steady_clock::now() : wt0;
        const SqlQueryResult& res = *res_shared;
        if (cap_session && res.success && !charge_rows(res.is_result_set ? res.rows.size() : res.rows_affected)) {
          res_shared.reset();
          send_err(client_conn, curr_seq, 1227, "42000", "ASQL capability: row budget exhausted (RESOURCE_LIMIT_EXCEEDED)");
          SqlDatabase::update_active_session(conn_id, "Sleep", "idle", "", session_database);
          break;
        }

        if (res.streamed) {
          // The rows went out as they were produced; nothing is left to send.
          if (wire_trace) fprintf(stderr, "[wire] execute+stream=%lldus rows=%llu\n",
                                  (long long)std::chrono::duration_cast<std::chrono::microseconds>(wt1 - wt0).count(),
                                  (unsigned long long)res.rows_affected);
          res_shared.reset();
          SqlDatabase::update_active_session(conn_id, "Sleep", "idle", "", session_database);
          wire_flush(client_conn);
          wire_shrink_statement();
          db_.shrink_session_buffers(false);
          if (!row_sink.ok()) break;
          continue;
        }
        if (!res.success) {
          // An error ends the whole reply, as it does on MariaDB.
          send_engine_error(curr_seq, res.error_message.empty() ? "syntax or execution error" : res.error_message);
          res_shared.reset();
          SqlDatabase::update_active_session(conn_id, "Sleep", "idle", "", session_database);
          wire_flush(client_conn);
          break;
        }

        if (!res.is_result_set) {
          // OK Packet for DDL / DML
          // UPDATE: the rows changed, or - a client that asked for found rows (JDBC, CLIENT_FOUND_ROWS) - matched.
          send_ok(client_conn, curr_seq++,
                  (client_flags & CAP_FOUND_ROWS) && res.rows_matched >= 0 ? (uint64_t)res.rows_matched : res.rows_affected,
                  sql_take_statement_insert_id(), status, res.ok_info);
          res_shared.reset();
          SqlDatabase::update_active_session(conn_id, "Sleep", "idle", "", session_database);
          wire_flush(client_conn);
          wire_shrink_statement();
          db_.shrink_session_buffers(false);
          continue;
        }

        // The whole response is kept (up to a cap) to store beside the cached
        // answer; a bigger one, or one of several, just streams.
        std::string capture;
        bool captured = false;
        const bool sent_ok = stream_result_set(res, false, cs_out_latin1, wire_db, status, curr_seq,
                                               multi ? nullptr : &capture, &captured,
                                               [&](const uint8_t* p, size_t n) {
                                                 db_.metrics().bytes_sent.fetch_add(n, std::memory_order_relaxed);
                                                 return wire_queue(client_conn, ByteSpan(p, n));
                                               },
                                               [&]() { return wire_flush(client_conn); });
        const auto wt2 = wire_trace ? std::chrono::steady_clock::now() : wt0;
        const size_t res_cols_sz = res.columns.size();
        const size_t res_rows_sz = res.rows.size();
        res_shared.reset();
        if (sent_ok && captured) db_.attach_cached_wire_packets(stmt_text, session_database, wire_db, std::move(capture), cache_gen, false, cs_out_latin1);
        if (wire_trace) {
          const auto wt3 = std::chrono::steady_clock::now();
          auto us = [](auto a, auto b) { return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count(); };
          fprintf(stderr, "[wire] execute=%lldus encode=%lldus attach=%lldus cols=%zu rows=%zu\n", (long long)us(wt0, wt1),
                  (long long)us(wt1, wt2), (long long)us(wt2, wt3), res_cols_sz, res_rows_sz);
        }
        SqlDatabase::update_active_session(conn_id, "Sleep", "idle", "", session_database);
        wire_flush(client_conn);
        wire_shrink_statement();
        db_.shrink_session_buffers(false);
        if (!sent_ok) break;
      }
    } else if (cmd == COM_STATISTICS) {
      // `mysqladmin status` and the CLI's \s: one plain-text packet. Each
      // figure is the server's own count; MariaDB's Opens / Flush tables
      // have no counterpart here and are left out rather than invented.
      const uint64_t up_s = (kernel_.host().monotonic_ns() - g_wire_started_ns) / 1000000000ull;
      const uint64_t questions = db_.metrics().total_queries.load(std::memory_order_relaxed);
      char line[256];
      std::snprintf(line, sizeof line, "Uptime: %llu  Threads: %llu  Questions: %llu  Slow queries: %llu  Open tables: %zu  Queries per second avg: %.3f",
                    (unsigned long long)up_s, (unsigned long long)active_clients_.load(std::memory_order_relaxed),
                    (unsigned long long)questions, (unsigned long long)db_.metrics().total_slow_queries.load(std::memory_order_relaxed),
                    db_.list_tables().size(), up_s ? (double)questions / (double)up_s : 0.0);
      send_packet(client_conn, reinterpret_cast<const uint8_t*>(line), std::strlen(line), 1);
    } else if (cmd == COM_SET_OPTION) {
      // MYSQL_OPTION_MULTI_STATEMENTS_ON (0) / _OFF (1); the reply is an EOF packet.
      const uint16_t opt = cmd_pkt.size() >= 3 ? get_u16(&cmd_pkt[1]) : 0xffff;
      if (opt > 1) {
        send_err(client_conn, 1, 1047, "08S01", "Unknown command");
        continue;
      }
      multi_statements = opt == 0;
      const uint8_t eof[] = {0xfe, 0x00, 0x00, 0x02, 0x00};
      send_packet(client_conn, eof, sizeof(eof), 1);
    } else if (cmd == COM_RESET_CONNECTION) {
      // Back to a fresh session on the same connection: prepared statements
      // gone, the handshake's charset and multi-statement setting back, the
      // user and database kept. mysqlnd sends this on every reuse of a
      // persistent (p:) connection when it may skip COM_CHANGE_USER.
      prepared_stmts.clear();
      prepared_stmts.rehash(0);
      cs_in_latin1 = cs_out_latin1 = handshake_latin1;
      SqlDatabase::set_session_latin1(cs_in_latin1, cs_out_latin1);
      multi_statements = handshake_multi;
      db_.reset_session();
      send_ok(client_conn, 1);
    } else if (cmd == COM_CHANGE_USER) {
      // [user \0] [auth: len byte + bytes, or \0-terminated] [db \0] [charset 2] [plugin \0] ...
      // A new scramble is issued (an AuthSwitchRequest), the same as a fresh
      // handshake; a bad password leaves the old identity in place.
      size_t off = 1;
      std::string new_user;
      while (off < cmd_pkt.size() && cmd_pkt[off] != 0) new_user.push_back((char)cmd_pkt[off++]);
      if (off < cmd_pkt.size()) ++off;
      std::vector<uint8_t> auth_data;
      if (client_flags & CAP_SECURE_CONNECTION) {
        if (off < cmd_pkt.size()) {
          const size_t n = cmd_pkt[off++];
          if (off + n <= cmd_pkt.size()) auth_data.assign(cmd_pkt.begin() + (long)off, cmd_pkt.begin() + (long)(off + n));
          off += n;
        }
      } else {
        while (off < cmd_pkt.size() && cmd_pkt[off] != 0) auth_data.push_back(cmd_pkt[off++]);
        if (off < cmd_pkt.size()) ++off;
      }
      std::string new_db;
      while (off < cmd_pkt.size() && cmd_pkt[off] != 0) new_db.push_back((char)cmd_pkt[off++]);
      if (off < cmd_pkt.size()) ++off;
      uint16_t new_charset = 0;
      if (off + 2 <= cmd_pkt.size()) { new_charset = get_u16(&cmd_pkt[off]); off += 2; }
      bool ok;
      uint8_t at_seq = 1;
      if (accounts_.empty()) {
        ok = trust_loopback_ && from_loopback;
      } else {
        std::array<uint8_t, 20> new_salt;
        {
          std::random_device rd;
          for (int i = 0; i < 20; ++i) new_salt[i] = (uint8_t)((rd() % 255) + 1);
        }
        std::vector<uint8_t> sw;
        sw.push_back(0xfe);
        static const char kPlugin[] = "mysql_native_password";
        sw.insert(sw.end(), kPlugin, kPlugin + sizeof(kPlugin));   // with its NUL
        sw.insert(sw.end(), new_salt.begin(), new_salt.end());
        sw.push_back(0);
        if (!send_packet(client_conn, sw.data(), sw.size(), at_seq++).ok()) break;
        std::vector<uint8_t> reply;
        uint8_t rseq = 0;
        if (!recv_packet(client_conn, &reply, &rseq).ok()) break;
        at_seq = (uint8_t)(rseq + 1);
        ok = auth_ok(new_user, reply, new_salt.data(), from_loopback);
      }
      if (!ok) {
        send_err(client_conn, at_seq, 1045, "28000", "Access denied for user '" + new_user + "'");
        continue;
      }
      username = new_user;
      session_database = new_db.empty() ? std::string("aios_db") : new_db;
      prepared_stmts.clear();
      prepared_stmts.rehash(0);
      cs_in_latin1 = cs_out_latin1 = new_charset != 0 && collation_is_latin1((uint8_t)new_charset);
      SqlDatabase::set_session_latin1(cs_in_latin1, cs_out_latin1);
      multi_statements = handshake_multi;
      db_.reset_session();
      SqlDatabase::set_client_tag("wire:" + username + "@" + peer);
      SqlDatabase::update_active_session(conn_id, "Sleep", "idle", "", session_database);
      send_ok(client_conn, at_seq);
    } else if (cmd == COM_PROCESS_KILL) {
      const uint32_t victim = cmd_pkt.size() >= 5 ? get_u32(&cmd_pkt[1]) : 0;
      if (SqlDatabase::kill_session(victim, false)) send_ok(client_conn, 1);
      else send_err(client_conn, 1, 1094, "HY000", "Unknown thread id: " + std::to_string(victim));
    } else if (cmd == COM_DEBUG) {
      const uint8_t eof[] = {0xfe, 0x00, 0x00, 0x02, 0x00};
      send_packet(client_conn, eof, sizeof(eof), 1);
    } else {
      // MariaDB's answer to a command it does not know; an OK here left a
      // client waiting on a result set that never came.
      send_err(client_conn, 1, 1047, "08S01", "Unknown command");
    }
  }

  --active_clients_;
  db_.metrics().current_connections.store(active_clients_.load(std::memory_order_relaxed), std::memory_order_relaxed);
  SqlDatabase::unregister_active_session(conn_id);
  SqlDatabase::set_current_session_id(0);
}

}  // namespace aios::userland
