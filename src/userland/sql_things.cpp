// Things: live objects kept inside the SQL engine. See sql_things.hpp.
#if defined(_WIN32)
// Before anything transitively drags in <windows.h>.
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#endif
#include "aios/userland/sql_things.hpp"
#include "aios/hal/sync.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <new>
#if !defined(AIOS_METAL) && !defined(_WIN32)
#include <unistd.h>
#endif
#if defined(_WIN32)
// POSIX localtime_r(const time_t*, struct tm*) -> struct tm*, as Windows' own
// (reversed-argument, errno_t-returning) localtime_s.
static inline struct tm* localtime_r(const std::time_t* t, struct tm* buf) {
  return ::localtime_s(buf, t) == 0 ? buf : nullptr;
}
#endif

#include "aios/hal/net.hpp"
#include "aios/userland/grain.hpp"
#include "aios/userland/sql_database.hpp"
#include "aios/userland/mysql_wire.hpp"
#include "aios/userland/sat_solver.hpp"
#include "aios/userland/sql_sat.hpp"
#if defined(AIOS_METAL) && defined(__aarch64__)
void kputs(const char* s);   // the board's serial console (metal/arm64/krt.cpp)
#endif

namespace aios::userland {

namespace {

constexpr size_t kHistoryKeep = 32;
constexpr size_t kPartitionChunk = 1000;
const char* const kNullKey = "\x01NULL";

std::string lower(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}
std::string upper(std::string s) {
  for (char& c : s) c = (char)std::toupper((unsigned char)c);
  return s;
}
std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace((unsigned char)s[a])) ++a;
  while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
  return s.substr(a, b - a);
}
std::string unquote_ident(std::string s) {
  s = trim(s);
  if (s.size() >= 2 && s.front() == '`' && s.back() == '`') s = s.substr(1, s.size() - 2);
  return s;
}

// A partition value in one canonical spelling: the executor prints the view's
// column, the notice carries the table's cell - for an integer both are the
// same digits, but a stray sign or leading zero must not split one partition.
std::string canon(const std::string& v) {
  std::string s = trim(v);
  size_t i = 0;
  bool neg = false;
  if (i < s.size() && (s[i] == '-' || s[i] == '+')) { neg = s[i] == '-'; ++i; }
  if (i == s.size()) return s;
  for (size_t j = i; j < s.size(); ++j)
    if (!std::isdigit((unsigned char)s[j])) return s;
  while (i + 1 < s.size() && s[i] == '0') ++i;
  std::string d = s.substr(i);
  return (neg && d != "0") ? "-" + d : d;
}
bool all_digits(const std::string& s) {
  if (s.empty()) return false;
  size_t i = (s[0] == '-') ? 1 : 0;
  if (i == s.size()) return false;
  for (; i < s.size(); ++i)
    if (!std::isdigit((unsigned char)s[i])) return false;
  return true;
}
std::string sql_lit(const std::string& v) {
  if (sql_is_null(v)) return "NULL";
  std::string o = "'";
  for (char ch : v) {
    if (ch == '\'') o += "''";
    else if (ch == '\\') o += "\\\\";
    else o += ch;
  }
  return o + "'";
}
std::string ident(const std::string& n) {
  std::string o = "`";
  for (char ch : n) { if (ch == '`') o += "``"; else o += ch; }
  return o + "`";
}

// Exact decimal text -> scaled integer. False for anything that is not a
// plain decimal numeral (an exponent, text): such a value cannot be summed
// across partitions to exactly what the query prints.
bool parse_dec(const std::string& t, ThingDec* out) {
  size_t i = 0;
  bool neg = false;
  if (i < t.size() && (t[i] == '-' || t[i] == '+')) { neg = t[i] == '-'; ++i; }
  AIOS_INT128 acc = 0;
  int frac = -1, digits = 0;
  bool any = false;
  for (; i < t.size(); ++i) {
    char c = t[i];
    if (c == '.' && frac < 0) { frac = 0; continue; }
    if (c < '0' || c > '9') return false;
    if (frac >= 0) ++frac;
    if (acc != 0 || c != '0') ++digits;
    if (digits > 36) return false;
    acc = acc * 10 + (c - '0');
    any = true;
  }
  if (!any) return false;
  out->v = neg ? -acc : acc;
  out->scale = frac < 0 ? 0 : frac;
  return true;
}
AIOS_INT128 pow10i(int n) {
  AIOS_INT128 p = 1;
  while (n-- > 0) p *= 10;
  return p;
}
void dec_add(ThingDec* a, const ThingDec& b, int sign) {
  AIOS_INT128 bv = b.v;
  if (b.scale > a->scale) {
    a->v *= pow10i(b.scale - a->scale);
    a->scale = b.scale;
  } else if (b.scale < a->scale) {
    bv *= pow10i(a->scale - b.scale);
  }
  a->v += sign > 0 ? bv : -bv;
}
std::string dec_text(const ThingDec& d, int scale) {
  AIOS_INT128 v = d.v;
  if (scale > d.scale) v *= pow10i(scale - d.scale);
  // (scale never drops below d.scale: it is the widest seen)
  const bool neg = v < 0;
  if (neg) v = -v;
  std::string digits;
  do { digits.insert(digits.begin(), (char)('0' + (int)(v % 10))); v /= 10; } while (v > 0);
  if (scale > 0) {
    while ((int)digits.size() <= scale) digits.insert(digits.begin(), '0');
    digits.insert(digits.end() - scale, '.');
  }
  bool zero = true;
  for (char c : digits) if (c != '0' && c != '.') zero = false;
  return (neg && !zero) ? "-" + digits : digits;
}

// Top-level scanning of SQL text: outside quotes and parentheses.
struct Scan {
  const std::string& s;
  explicit Scan(const std::string& str) : s(str) {}
  // Position of the whole word `kw` (case-insensitive, may contain one space
  // as in "GROUP BY") at depth 0 at or after `from`; npos when absent.
  size_t find_kw(const std::string& kw, size_t from = 0) const {
    int depth = 0;
    char q = 0;
    const std::string K = upper(kw);
    for (size_t i = from; i < s.size(); ++i) {
      char c = s[i];
      if (q) {
        if (c == q) {
          if (i + 1 < s.size() && s[i + 1] == q) { ++i; continue; }
          q = 0;
        } else if (c == '\\' && q != '`') {
          ++i;
        }
        continue;
      }
      if (c == '\'' || c == '"' || c == '`') { q = c; continue; }
      if (c == '(') { ++depth; continue; }
      if (c == ')') { --depth; continue; }
      if (depth != 0) continue;
      if (i > 0 && (std::isalnum((unsigned char)s[i - 1]) || s[i - 1] == '_')) continue;
      if (i + K.size() > s.size()) continue;
      bool m = true;
      for (size_t k = 0; k < K.size() && m; ++k) {
        char a = (char)std::toupper((unsigned char)s[i + k]);
        if (K[k] == ' ') m = std::isspace((unsigned char)s[i + k]) != 0;
        else m = a == K[k];
      }
      if (!m) continue;
      size_t e = i + K.size();
      if (e < s.size() && (std::isalnum((unsigned char)s[e]) || s[e] == '_')) continue;
      return i;
    }
    return std::string::npos;
  }
  std::vector<std::string> split_commas(size_t from, size_t to) const {
    std::vector<std::string> out;
    int depth = 0;
    char q = 0;
    size_t start = from;
    for (size_t i = from; i < to; ++i) {
      char c = s[i];
      if (q) {
        if (c == q) {
          if (i + 1 < to && s[i + 1] == q) { ++i; continue; }
          q = 0;
        } else if (c == '\\' && q != '`') {
          ++i;
        }
        continue;
      }
      if (c == '\'' || c == '"' || c == '`') { q = c; continue; }
      if (c == '(') ++depth;
      else if (c == ')') --depth;
      else if (c == ',' && depth == 0) { out.push_back(trim(s.substr(start, i - start))); start = i + 1; }
    }
    out.push_back(trim(s.substr(start, to - start)));
    return out;
  }
};

// The whole text is one call FUNC(...) (balanced), returning FUNC upper-cased.
bool whole_call(const std::string& t, std::string* func, std::string* inner) {
  size_t p = t.find('(');
  if (p == std::string::npos || t.back() != ')') return false;
  std::string f = trim(t.substr(0, p));
  for (char c : f) if (!std::isalnum((unsigned char)c) && c != '_') return false;
  int depth = 0;
  char q = 0;
  for (size_t i = p; i < t.size(); ++i) {
    char c = t[i];
    if (q) { if (c == q) q = 0; continue; }
    if (c == '\'' || c == '"' || c == '`') { q = c; continue; }
    if (c == '(') ++depth;
    else if (c == ')') { if (--depth == 0 && i + 1 != t.size()) return false; }
  }
  *func = upper(f);
  *inner = t.substr(p + 1, t.size() - p - 2);
  return true;
}
std::string strip_parens(std::string t) {
  t = trim(t);
  while (t.size() >= 2 && t.front() == '(' && t.back() == ')') {
    int depth = 0;
    bool wraps = true;
    for (size_t i = 0; i < t.size(); ++i) {
      if (t[i] == '(') ++depth;
      else if (t[i] == ')') { if (--depth == 0 && i + 1 != t.size()) { wraps = false; break; } }
    }
    if (!wraps) break;
    t = trim(t.substr(1, t.size() - 2));
  }
  return t;
}
bool is_number(const std::string& t) {
  ThingDec d;
  return parse_dec(trim(t), &d);
}
// Linear in SUM/COUNT: partitions' values add up to the whole's exactly.
bool linear(const std::string& text, bool* count_like, bool* sum_like) {
  std::string t = strip_parens(text);
  if (t.empty()) return false;
  if (t[0] == '-') return linear(t.substr(1), count_like, sum_like);
  std::string f, inner;
  if (whole_call(t, &f, &inner)) {
    if (upper(trim(inner)).rfind("DISTINCT", 0) == 0) return false;
    if (f == "COUNT") { *count_like = true; return true; }
    if (f == "SUM") { *sum_like = true; return true; }
    return false;
  }
  Scan sc(t);
  // a * b, one side a number
  int depth = 0;
  char q = 0;
  for (size_t i = 0; i < t.size(); ++i) {
    char c = t[i];
    if (q) { if (c == q) q = 0; continue; }
    if (c == '\'' || c == '"' || c == '`') { q = c; continue; }
    if (c == '(') ++depth;
    else if (c == ')') --depth;
    else if (c == '*' && depth == 0) {
      std::string a = t.substr(0, i), b = t.substr(i + 1);
      if (is_number(strip_parens(b))) return linear(a, count_like, sum_like);
      if (is_number(strip_parens(a))) return linear(b, count_like, sum_like);
      return false;
    }
  }
  (void)sc;
  return false;
}

bool time_function(const std::string& select_upper) {
  static const char* fns[] = {"CURDATE", "NOW(", "CURRENT_DATE", "CURRENT_TIMESTAMP", "SYSDATE", "UTC_DATE",
                              "LOCALTIME", "UNIX_TIMESTAMP(", "CURTIME"};
  for (const char* f : fns)
    if (select_upper.find(f) != std::string::npos) return true;
  return false;
}

std::string esc(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (char c : s) {
    if (c == '\\') o += "\\\\";
    else if (c == '\t') o += "\\t";
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else if (c == '\0') o += "\\0";
    else o += c;
  }
  return o;
}
std::string unesc(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      char n = s[++i];
      if (n == 't') o += '\t';
      else if (n == 'n') o += '\n';
      else if (n == 'r') o += '\r';
      else if (n == '0') o += '\0';
      else o += n;
    } else {
      o += s[i];
    }
  }
  return o;
}
std::vector<std::string> split_tabs(const std::string& line) {
  std::vector<std::string> f;
  size_t a = 0;
  for (size_t i = 0; i <= line.size(); ++i)
    if (i == line.size() || line[i] == '\t') { f.push_back(line.substr(a, i - a)); a = i + 1; }
  return f;
}
std::string val_field(const std::string& v) { return sql_is_null(v) ? std::string("N") : "V" + esc(v); }
std::string field_val(const std::string& f) { return (f.empty() || f[0] == 'N') ? sql_null() : unesc(f.substr(1)); }

std::string wall_text(uint64_t ns) {
  if (ns == 0) return sql_null();
  std::time_t t = (std::time_t)(ns / 1000000000ull);
  std::tm tm_buf{};
  localtime_r(&t, &tm_buf);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
  return buf;
}

SqlQueryResult error_result(const std::string& m) {
  SqlQueryResult r;
  r.success = false;
  r.error_message = m;
  return r;
}

}  // namespace

// ---- grain Things: the runtime --------------------------------------------
//
// One VM per grain kind, holding the compiled program between reactions. It
// is built with the language's builtins minus everything that could make a
// reaction depend on anything but the tables: no files, clock, randomness,
// environment, network, canvas, sidecars or SQL writes. read() is the one way
// out, and it goes through the engine's executor with the tables it touches
// checked against READS.
struct ThingGrain {
  grain::Vm vm;
  std::shared_ptr<grain::FnObj> script;
  grain::Value shared;
  bool have_shared = false;
  bool shared_fresh = false;   // shared was just recomputed: a full pass may take it as is
  bool has_touched = false;
  bool day_safe = false;   // the program says when its Things next change with the date (wake_at): the day turning is no reason to read them all again
  std::vector<std::string> printed;
  struct Notice {
    std::string table, op;
    grain::Value before, after;
  };
  std::vector<Notice> notices;
};

namespace {

struct GrainCtx {
  SqlDatabase* db = nullptr;
  ThingEngine* eng = nullptr;
  std::string dbname;
  const std::vector<std::string>* reads = nullptr;
  const std::map<std::string, std::string>* links = nullptr;
  const std::vector<std::string>* writes = nullptr;   // set only while an event fires
  MariaWireSession* target = nullptr;   // WRITES ... TO <profile>: writes (and read_target) go here
  uint64_t queries = 0;
  uint64_t writes_done = 0;
};
thread_local GrainCtx* g_grain = nullptr;
// True while this thread holds the engine's mutex (settle, tick, CREATE,
// SHOW, load): claims() then reads directly instead of locking again.
thread_local bool g_mu_held = false;
struct MuHeld {
  bool prev;
  MuHeld() : prev(g_mu_held) { g_mu_held = true; }
  ~MuHeld() { g_mu_held = prev; }
};

struct GrainScope {
  GrainCtx ctx;
  GrainCtx* prev;
  GrainScope(SqlDatabase& db, ThingEngine* eng, const std::string& dbname, const std::vector<std::string>* reads,
             const std::map<std::string, std::string>* links)
      : prev(g_grain) {
    ctx.db = &db;
    ctx.eng = eng;
    ctx.dbname = dbname;
    ctx.reads = reads;
    ctx.links = links;
    g_grain = &ctx;
  }
  ~GrainScope() { g_grain = prev; }
};
grain::Value gb_claims(grain::Vm&, std::vector<grain::Value>& args, std::string& error);

// The shortest text that reads back as the same double (what ASQL and PHP's
// json_encode both print).
std::string real_text(double d) {
  if (std::isnan(d)) return "NaN";
  if (std::isinf(d)) return d > 0 ? "INF" : "-INF";
  char b[40];
  for (int p = 1; p <= 17; ++p) {
    std::snprintf(b, sizeof b, "%.*g", p, d);
    if (std::strtod(b, nullptr) == d) return b;
  }
  return b;
}

std::string gval_text(const grain::Value& v) {
  switch (v.type) {
    case grain::Type::Nil: return sql_null();
    case grain::Type::Bool: return v.b ? "1" : "0";
    case grain::Type::Int: return std::to_string(v.i);
    case grain::Type::Real: return real_text(v.d);
    case grain::Type::Str: return v.str();
    default: return v.repr();
  }
}

bool grain_lit(const grain::Value& v, std::string* out, std::string* err) {
  switch (v.type) {
    case grain::Type::Nil: *out = "NULL"; return true;
    case grain::Type::Bool: *out = v.b ? "1" : "0"; return true;
    case grain::Type::Int: *out = std::to_string(v.i); return true;
    case grain::Type::Real: *out = real_text(v.d); return true;
    case grain::Type::Str: *out = sql_lit(v.str()); return true;
    case grain::Type::List: {
      // A list binds as the inside of IN (...); an empty one as NULL, which matches nothing.
      std::string s;
      for (const auto& e : v.list_items()) {
        if (e.type == grain::Type::List || e.type == grain::Type::Map) { *err = "read(): a list argument must hold plain values"; return false; }
        std::string one;
        if (!grain_lit(e, &one, err)) return false;
        s += (s.empty() ? "" : ",") + one;
      }
      *out = s.empty() ? "NULL" : s;
      return true;
    }
    default: *err = std::string("read(): cannot bind a ") + grain::type_name(v.type); return false;
  }
}

grain::Value cell_value(const std::string& v, SqlDataType t) {
  if (sql_is_null(v)) return grain::Value::nil();
  if (t == SqlDataType::Int) {
    char* e = nullptr;
    long long n = std::strtoll(v.c_str(), &e, 10);
    if (e && *e == 0 && !v.empty()) return grain::Value::integer(n);
  } else if (t == SqlDataType::Real || t == SqlDataType::Decimal) {
    char* e = nullptr;
    double d = std::strtod(v.c_str(), &e);
    if (e && *e == 0 && !v.empty()) return grain::Value::real(d);
  }
  return grain::Value::string(v);
}

grain::Value row_value(const SqlTable& t, const SqlRow* r) {
  if (!r) return grain::Value::nil();
  auto m = std::make_shared<grain::MapObj>();
  for (size_t i = 0; i < t.columns.size() && i < r->values.size(); ++i)
    m->put(t.columns[i].name, cell_value(r->values[i], t.columns[i].type));
  return grain::Value::object(grain::Type::Map, m);
}

// '?' outside quotes takes the next argument, as a quoted literal.
bool bind_args(const std::vector<grain::Value>& args, const char* who, std::string* out, std::string& error) {
  const std::string& tpl = args[0].str();
  std::string& sql = *out;
  size_t next = 1;
  char q = 0;
  for (size_t i = 0; i < tpl.size(); ++i) {
    char c = tpl[i];
    if (q) {
      sql += c;
      if (c == '\\' && q != '`' && i + 1 < tpl.size()) { sql += tpl[++i]; continue; }
      if (c == q) q = 0;
      continue;
    }
    if (c == '\'' || c == '"' || c == '`') { q = c; sql += c; continue; }
    if (c == ';') { error = std::string(who) + "(): one statement only"; return false; }
    if (c == '?') {
      if (next >= args.size()) { error = std::string(who) + "(): more ? than arguments"; return false; }
      std::string lit;
      if (!grain_lit(args[next++], &lit, &error)) return false;
      sql += lit;
      continue;
    }
    sql += c;
  }
  if (next != args.size()) { error = std::string(who) + "(): more arguments than ?"; return false; }
  return true;
}

// write(sql, args...): only while an event Thing fires, only INSERT / UPDATE /
// DELETE / REPLACE / DROP TABLE on its WRITES tables; returns rows affected.
grain::Value gb_write(grain::Vm&, std::vector<grain::Value>& args, std::string& error) {
  if (!g_grain || !g_grain->writes) { error = "write() only runs while an event Thing fires"; return grain::Value::nil(); }
  if (args[0].type != grain::Type::Str) { error = "write() wants a statement"; return grain::Value::nil(); }
  std::string sql;
  if (!bind_args(args, "write", &sql, error)) return grain::Value::nil();
  // the table it writes
  std::vector<std::string> w;
  {
    std::string cur;
    for (char c : sql) {
      if (std::isspace((unsigned char)c) || c == '(') { if (!cur.empty()) { w.push_back(cur); cur.clear(); } if (w.size() > 6) break; if (c == '(') break; continue; }
      cur += c;
    }
    if (!cur.empty()) w.push_back(cur);
  }
  auto W = [&](size_t i) { return i < w.size() ? upper(w[i]) : std::string(); };
  size_t ti = std::string::npos;
  if (W(0) == "INSERT" || W(0) == "REPLACE") ti = W(1) == "INTO" ? 2 : (W(2) == "INTO" ? 3 : std::string::npos);
  else if (W(0) == "UPDATE") ti = 1;
  else if (W(0) == "DELETE") ti = W(1) == "FROM" ? 2 : std::string::npos;
  else if (W(0) == "DROP" && W(1) == "TABLE") ti = (W(2) == "IF" && W(3) == "EXISTS") ? 4 : 2;
  if (ti == std::string::npos || ti >= w.size()) { error = "write(): INSERT, UPDATE, DELETE, REPLACE or DROP TABLE only"; return grain::Value::nil(); }
  std::string t = lower(unquote_ident(w[ti]));
  if (t.find('.') != std::string::npos) t = lower(unquote_ident(t.substr(t.find('.') + 1)));
  if (!t.empty() && t.front() == '`') t = t.substr(1);
  if (!t.empty() && t.back() == '`') t.pop_back();
  if (std::find(g_grain->writes->begin(), g_grain->writes->end(), t) == g_grain->writes->end()) {
    error = "write(): " + t + " is not in this Thing's WRITES";
    return grain::Value::nil();
  }
  if (g_grain->target) {
    auto r = g_grain->target->query(sql);
    g_grain->writes_done++;
    if (!r.ok()) { error = "write(): " + r.error().message; return grain::Value::nil(); }
    if (!r->ok) { error = "write(): " + r->error_message; return grain::Value::nil(); }
    return grain::Value::integer((int64_t)r->affected_rows);
  }
  SqlQueryResult r = g_grain->db->execute_local_query(sql, g_grain->dbname);
  g_grain->writes_done++;
  if (!r.success) { error = "write(): " + r.error_message; return grain::Value::nil(); }
  return grain::Value::integer((int64_t)r.rows_affected);
}

// read_target(sql, args...): a SELECT on the database an event writes to
// (WRITES ... TO <profile>), inside its transaction - what fire() is about to
// change, and what verify() finds actually landed.
grain::Value gb_read_target(grain::Vm&, std::vector<grain::Value>& args, std::string& error) {
  if (!g_grain || !g_grain->target) { error = "read_target() only runs while an event that writes TO a database fires"; return grain::Value::nil(); }
  if (args[0].type != grain::Type::Str) { error = "read_target() wants a SELECT statement"; return grain::Value::nil(); }
  std::string sql;
  if (!bind_args(args, "read_target", &sql, error)) return grain::Value::nil();
  const std::string head = upper(trim(sql)).substr(0, 6);
  if (head.rfind("SELECT", 0) != 0 && head.rfind("WITH", 0) != 0) { error = "read_target() runs SELECT only"; return grain::Value::nil(); }
  auto r = g_grain->target->query(sql);
  g_grain->queries++;
  if (!r.ok()) { error = "read_target(): " + r.error().message; return grain::Value::nil(); }
  if (!r->ok) { error = "read_target(): " + r->error_message; return grain::Value::nil(); }
  std::vector<grain::Value> rows;
  for (const auto& row : r->rows) {
    auto m = std::make_shared<grain::MapObj>();
    for (size_t i = 0; i < r->columns.size() && i < row.size(); ++i) {
      if (row[i].is_null) { m->put(r->columns[i].name, grain::Value::nil()); continue; }
      const std::string& v = row[i].bytes;
      char* e = nullptr;
      const long long n = std::strtoll(v.c_str(), &e, 10);
      if (!v.empty() && e && *e == 0) { m->put(r->columns[i].name, grain::Value::integer(n)); continue; }
      const double d = std::strtod(v.c_str(), &e);
      if (!v.empty() && e && *e == 0 && v.find_first_not_of("0123456789.-+eE") == std::string::npos) m->put(r->columns[i].name, grain::Value::real(d));
      else m->put(r->columns[i].name, grain::Value::string(v));
    }
    rows.push_back(grain::Value::object(grain::Type::Map, m));
  }
  return grain::Value::list(std::move(rows));
}

// read(sql, args...): '?' outside quotes takes the next argument; the rows
// come back as a list of maps keyed by column name.
grain::Value read_impl(std::vector<grain::Value>& args, std::string& error, bool raw) {
  if (!g_grain) { error = "read() only runs inside a Thing's reaction"; return grain::Value::nil(); }
  if (args[0].type != grain::Type::Str) { error = "read() wants a SELECT statement"; return grain::Value::nil(); }
  const std::string& tpl = args[0].str();
  std::string sql;
  size_t next = 1;
  char q = 0;
  for (size_t i = 0; i < tpl.size(); ++i) {
    char c = tpl[i];
    if (q) {
      sql += c;
      if (c == '\\' && q != '`' && i + 1 < tpl.size()) { sql += tpl[++i]; continue; }
      if (c == q) q = 0;
      continue;
    }
    if (c == '\'' || c == '"' || c == '`') { q = c; sql += c; continue; }
    if (c == ';') { error = "read(): one statement only"; return grain::Value::nil(); }
    if (c == '?') {
      if (next >= args.size()) { error = "read(): more ? than arguments"; return grain::Value::nil(); }
      std::string lit;
      if (!grain_lit(args[next++], &lit, &error)) return grain::Value::nil();
      sql += lit;
      continue;
    }
    sql += c;
  }
  if (next != args.size()) { error = "read(): more arguments than ?"; return grain::Value::nil(); }
  const std::string head = upper(trim(sql)).substr(0, 6);
  if (head.rfind("SELECT", 0) != 0 && head.rfind("WITH", 0) != 0) {
    error = "read() runs SELECT (or WITH ... SELECT) only: a Thing does not write";
    return grain::Value::nil();
  }
  std::unordered_set<const SqlTable*> touched;
  sql_set_read_capture(&touched);
  static const bool trace_reads = std::getenv("AIOS_THINGS_TRACE_READS") != nullptr;
  const auto read_t0 = trace_reads ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
  SqlQueryResult r = g_grain->db->execute_local_query(sql, g_grain->dbname);
  sql_set_read_capture(nullptr);
  g_grain->queries++;
  if (trace_reads) {   // AIOS_THINGS_TRACE_READS=1: what a Thing's reads cost, one line each (a refresh is ~25 of them)
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - read_t0).count();
    if (ms >= 0.5) std::fprintf(stderr, "[things-read] %.2f ms %zu row(s) %.70s\n", ms, r.rows.size(), sql.c_str());
  }
  if (!r.success) { error = "read(): " + r.error_message; return grain::Value::nil(); }
  for (const SqlTable* t : touched) {
    if (!g_grain->db->is_stored_table(t)) continue;
    const std::string nm = lower(t->name);
    if (std::find(g_grain->reads->begin(), g_grain->reads->end(), nm) == g_grain->reads->end()) {
      error = "read() touched " + t->database + "." + t->name + ", which is not in the Thing's READS list";
      return grain::Value::nil();
    }
  }
  std::vector<grain::Value> rows;
  rows.reserve(r.rows.size());
  for (const auto& row : r.rows) {
    auto m = std::make_shared<grain::MapObj>();
    for (size_t i = 0; i < r.columns.size() && i < row.values.size(); ++i) {
      if (raw) m->put(r.columns[i], sql_is_null(row.values[i]) ? grain::Value::nil() : grain::Value::string(row.values[i]));
      else m->put(r.columns[i], cell_value(row.values[i], i < r.column_types.size() ? r.column_types[i] : SqlDataType::Text));
    }
    rows.push_back(grain::Value::object(grain::Type::Map, m));
  }
  return grain::Value::list(std::move(rows));
}

grain::Value gb_read(grain::Vm&, std::vector<grain::Value>& args, std::string& error) { return read_impl(args, error, false); }

// read_raw(sql, args...): read(), with every value as the text the wire would carry (NULL stays nil), for a
// program that is a port of code written against the wire: a price "0.0000" is a string there, and a
// string, not a number, is what that code's rules are about.
grain::Value gb_read_raw(grain::Vm&, std::vector<grain::Value>& args, std::string& error) { return read_impl(args, error, true); }

// read_with(sql, params) / read_raw_with(sql, params): the same, the ? values given as one list (nil: none) - for a
// caller holding its parameters in a list. A list among them still binds as IN (...), as it does in read().
grain::Value read_with_impl(std::vector<grain::Value>& args, std::string& error, bool raw) {
  if (args[1].type != grain::Type::List && args[1].type != grain::Type::Nil) {
    error = std::string(raw ? "read_raw_with" : "read_with") + "() wants its parameters as a list";
    return grain::Value::nil();
  }
  std::vector<grain::Value> flat;
  flat.push_back(args[0]);
  if (args[1].type == grain::Type::List)
    for (const auto& v : args[1].list_items()) flat.push_back(v);
  return read_impl(flat, error, raw);
}
grain::Value gb_read_with(grain::Vm&, std::vector<grain::Value>& args, std::string& error) { return read_with_impl(args, error, false); }
grain::Value gb_read_raw_with(grain::Vm&, std::vector<grain::Value>& args, std::string& error) { return read_with_impl(args, error, true); }

bool gcall(ThingGrain& g, const char* name, const std::vector<grain::Value>& args, grain::Value* out, std::string* err) {
  grain::Value* f = g.vm.find_global(name);
  if (!f) { *err = std::string("the program defines no ") + name + "()"; return false; }
  g.vm.steps = 0;
  std::string e;
#if !defined(AIOS_METAL) && defined(__cpp_exceptions)
  try {
    *out = g.vm.call(*f, args, e);
  } catch (const std::bad_alloc&) {
    // Caught here, not by the statement: a Thing that fails must not stay
    // half-declared (the kind is taken back out by the caller).
    *err = std::string(name) + "(): ran out of memory";
    return false;
  }
#else
  *out = g.vm.call(*f, args, e);   // (the bare-metal image and wasm builds have no exceptions)
#endif
  if (!e.empty()) { *err = std::string(name) + "(): " + e; return false; }
  return true;
}

// What a Thing's program may name: pure functions of their arguments.
const std::vector<std::string>& grain_allowed() {
  static const std::vector<std::string> names = {
      "print", "len", "type", "str", "repr", "int", "real", "push", "pop", "range", "sort", "reverse", "map",
      "filter", "reduce", "each", "for_each", "any", "all", "find_if", "flatten", "concat", "insert", "remove_at",
      "zip", "enumerate", "take", "drop", "count", "unique", "dedup", "compact", "keys", "values", "has", "del", "merge",
      "entries", "from_entries", "split", "join", "upper", "lower", "find", "replace", "slice", "ord", "chr", "trim",
      "ltrim", "rtrim", "starts_with", "ends_with", "contains", "index_of", "last_index_of", "pad_left", "pad_right",
      "repeat", "char_at", "chars", "is_digit", "is_alpha", "is_alnum", "is_space", "abs", "min", "max", "floor",
      "ceil", "sqrt", "pow", "round", "trunc", "sign", "clamp", "log", "log2", "log10", "exp", "assert", "try_call",
      "pcall", "error", "panic", "is_err", "is_ok", "json_parse", "json_stringify",
      // exact decimals and text layout: pure functions of their arguments
      "decimal", "dec_div", "scale", "decimal_prefix", "round_sig", "number_format", "is_numeric", "to_number", "format", "sort_by", "group_by", "key_by",
      "sum_by", "idiv", "upper_rtrim",
      // patterns: the same text and pattern give the same answer anywhere
      "regex_test", "regex_match", "regex_find_all", "regex_replace", "regex_split",
      "sat"};   // deterministic: a reaction replayed after a crash gets the same answer
  return names;
}

}  // namespace

namespace {
ThingFleetFn& fleet_fn() { static ThingFleetFn f; return f; }

// "... ON <machine>" / "... ON ALL" at the very end of a statement that can
// be sent (SHOW, CALL and the controls - never CREATE, whose query may end in
// a JOIN's ON). Returns the statement without it.
bool split_target(const std::string& sql, std::string* body, std::string* target) {
  std::string u = upper(trim(sql));
  static const char* verbs[] = {"SHOW", "CALL", "PAUSE", "RESUME", "REFRESH", "FIRE", "RETRY", "SET"};
  bool ok = false;
  for (const char* v : verbs) if (u.rfind(v, 0) == 0) ok = true;
  if (!ok) return false;
  std::string t = trim(sql);
  if (!t.empty() && t.back() == ';') t = trim(t.substr(0, t.size() - 1));
  size_t e = t.size();
  size_t b = e;
  while (b > 0 && (std::isalnum((unsigned char)t[b - 1]) || t[b - 1] == '_' || t[b - 1] == '-')) --b;
  if (b == e || b == 0 || !std::isspace((unsigned char)t[b - 1])) return false;
  size_t w = b;
  while (w > 0 && std::isspace((unsigned char)t[w - 1])) --w;
  if (w < 2 || upper(t.substr(w - 2, 2)) != "ON" || (w > 2 && !std::isspace((unsigned char)t[w - 3]))) return false;
  // the ON must sit outside quotes and parentheses
  Scan sc(t);
  if (sc.find_kw("ON", w - 2) != w - 2) return false;
  *body = trim(t.substr(0, w - 2));
  *target = t.substr(b, e - b);
  return true;
}

// Every row of a SHOW answer carries the machine it came from.
void tag_machine(SqlQueryResult* r, const std::string& machine) {
  if (!r->is_result_set) return;
  size_t at = r->columns.size();
  for (size_t i = 0; i < r->columns.size(); ++i) if (r->columns[i] == "machine") at = i;
  if (at == r->columns.size()) {
    r->columns.push_back("machine");
    for (auto& row : r->rows) row.values.push_back(machine);
  } else {
    for (auto& row : r->rows) if (at < row.values.size() && (row.values[at].empty() || sql_is_null(row.values[at]))) row.values[at] = machine;
  }
}
}  // namespace

void set_things_fleet(ThingFleetFn fn) { fleet_fn() = std::move(fn); }

// Why a mirror does not pull on the Pi (yet): a pull waits on fleet peers, and
// on the board every wait - the hub's open, the node handshake, one polled
// read of up to 8 s - runs on its one loop, which the hardware watchdog resets
// after ~15 s without a pass. A MACHINES pull there rebooted the board
// (2026-09-27, reproduced on demand). The board is watched from the servers
// (they mirror its Things); pulling from it needs a pull that never waits -
// ask on one tick, collect on a later one.
[[maybe_unused]] constexpr const char* kBoardNoPull =
    "not on the board yet: a pull waits on fleet peers, and on the Pi those waits run on its one loop, which the "
    "watchdog resets after ~15 s - mirror the board's Things from a server instead";

ThingEngine::ThingEngine(SqlDatabase& db) : db_(db) {}

std::string ThingEngine::machine_id() const { return db_.kernel_.self().short_str(); }

namespace {
// What people call this machine: AIOS_MACHINE_NAME, else its host name.
std::string host_name() {
  if (const char* n = std::getenv("AIOS_MACHINE_NAME")) if (*n) return n;
#if defined(AIOS_METAL)
  return "aios-metal";
#else
  char b[256] = {0};
  if (gethostname(b, sizeof b - 1) == 0 && b[0]) {
    std::string h(b);
    return h.substr(0, h.find('.'));
  }
  return "unknown";
#endif
}
}  // namespace

std::string ThingEngine::table_key(const SqlTable& t) const { return lower(t.database + "." + t.name); }

std::string ThingEngine::things_dir() const { return db_.root_path_ + "/things"; }

bool ThingEngine::watches(const SqlTable& t) const {
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  return !watched_.empty() && watched_.count(table_key(t)) != 0;
}

bool ThingEngine::has_pending() const {
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  for (const auto& kv : kinds_)
    if (kv.second.pending_full || !kv.second.pending.empty() || !kv.second.pending_res.empty() ||
        !kv.second.pending_part_keys.empty() || !kv.second.pending_part_full.empty() ||
        !kv.second.dirty_queues.empty() || kv.second.pending_shared ||
        (kv.second.grain && !kv.second.grain->notices.empty()))
      return true;
  return false;
}

void ThingEngine::on_row_change(const SqlTable& t, const SqlRow* before, const SqlRow* after) {
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  const std::string key = table_key(t);
  if (!watched_.count(key)) return;
  const char* op = before && after ? "update" : (after ? "insert" : "delete");
  static const bool trace = std::getenv("AIOS_THINGS_TRACE") != nullptr;
  if (trace) fprintf(stderr, "[things-trace] row %s %s\n", key.c_str(), op);
  for (auto& kv : kinds_) {
    Kind& k = kv.second;
    if (k.type == Kind::Grain) {
      if (std::find(k.reads.begin(), k.reads.end(), lower(t.name)) != k.reads.end() && lower(t.database) == lower(k.db))
        grain_notice(k, t, op, before, after);
      continue;
    }
    if (k.type == Kind::Resource) {
      if (key != k.cap_table) continue;
      for (const SqlRow* r : {before, after}) {
        if (!r) continue;
        std::vector<std::string> vals;
        bool ok = true;
        for (const auto& kc : k.key_cols) {
          int c = t.get_column_index(kc);
          if (c < 0 || (size_t)c >= r->values.size()) { ok = false; break; }
          vals.push_back(r->values[(size_t)c]);
        }
        if (!ok) { k.pending_full = true; k.full_cause = t.name + " " + op; break; }
        const std::string rk = reskey_of(vals);
        // Only stock something claims is held; the rest changes nothing.
        if (k.res.count(rk)) {
          std::string label;
          for (size_t i = vals.size(); i-- > 0;) label += (label.empty() ? "" : " at ") + vals[i];
          k.pending_res[rk] = t.name + " " + op + " " + label;
        }
      }
      continue;
    }
    if (k.deps_partition.count(key)) {
      int c = t.get_column_index(k.part_col);
      if (c < 0) { k.pending_full = true; k.full_cause = t.name + " " + op; continue; }
      for (const SqlRow* r : {before, after}) {
        if (!r || (size_t)c >= r->values.size() || sql_is_null(r->values[(size_t)c])) continue;
        const std::string p = canon(r->values[(size_t)c]);
        k.pending[p] = t.name + " " + op + " " + k.part_col + " " + p;
      }
    } else if (k.deps_full.count(key)) {
      k.pending_full = true;
      k.full_cause = t.name + " " + op;
    }
    // The parts: a row of a part's key table moves that key's rows; any other
    // table a part reads moves the part for every key.
    for (const auto& p : k.part_defs) {
      if (p.key_dep == key) {
        int c = t.get_column_index(p.key_col);
        if (c < 0) { k.pending_part_full.insert(p.name); continue; }
        for (const SqlRow* r : {before, after}) {
          if (!r || (size_t)c >= r->values.size() || sql_is_null(r->values[(size_t)c])) continue;
          k.pending_part_keys[p.name].insert(canon(r->values[(size_t)c]));
          if (trace) fprintf(stderr, "[things-trace]   %s part %s key %s\n", k.name.c_str(), p.name.c_str(), canon(r->values[(size_t)c]).c_str());
        }
      } else if (p.deps_full.count(key)) {
        k.pending_part_full.insert(p.name);
      }
    }
  }
}

void ThingEngine::on_table_reset(const SqlTable& t) {
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  const std::string key = table_key(t);
  for (auto& kv : kinds_) {
    Kind& k = kv.second;
    const bool grain_reads = k.type == Kind::Grain && lower(t.database) == lower(k.db) &&
                             std::find(k.reads.begin(), k.reads.end(), lower(t.name)) != k.reads.end();
    if (k.deps_partition.count(key) || k.deps_full.count(key) || (k.type == Kind::Resource && key == k.cap_table) || grain_reads) {
      k.pending_full = true;
      k.full_cause = t.name + " emptied";
    }
  }
}

std::string ThingEngine::current_day() {
  SqlQueryResult r = db_.execute_local_query("SELECT CURDATE()");
  if (!r.success || r.rows.empty() || r.rows[0].values.empty()) return "";
  return r.rows[0].values[0];
}

static void from_names(const std::string& text, std::vector<std::string>* out);

bool ThingEngine::parse_create(const std::string& sql, Kind* k, std::string* err) const {
  // CREATE THING name KEY col PARTITION BY col READS t1, t2 AS SELECT ...
  Scan sc(sql);
  {
    const size_t p_key = sc.find_kw("KEY");
    const size_t p_res = sc.find_kw("RESOURCE"), p_cl = sc.find_kw("CLAIMS");
    if (p_res != std::string::npos && p_res < p_key) return parse_resource(sql, k, err);
    if (p_cl != std::string::npos && p_cl < p_key) return parse_claims(sql, k, err);
    const size_t p_gr = sc.find_kw("GRAIN");
    if (p_key == std::string::npos && p_gr == std::string::npos &&
        (sc.find_kw("MIRROR") != std::string::npos || sc.find_kw("MACHINES") != std::string::npos))
      return parse_mirror(sql, k, err);
    const size_t p_at = sc.find_kw("AT");
    if (p_at != std::string::npos && p_gr != std::string::npos && p_at < p_gr) return parse_event(sql, k, err);
    if (p_gr != std::string::npos && p_gr < p_key) return parse_grain(sql, k, err);
  }
  // CREATE THING name KEY <expr> [PARTITION BY <col>] [READS t1, ..] AS SELECT ...
  //   [PART <name> AS SELECT ... WHERE <expr> = KEY ...]...
  // PARTITION BY defaults to the KEY; READS to every table the queries name.
  size_t p_thing = sc.find_kw("THING");
  size_t p_key = sc.find_kw("KEY");
  size_t p_part = sc.find_kw("PARTITION BY");
  size_t p_reads = sc.find_kw("READS");
  size_t p_as = sc.find_kw("AS", p_reads != std::string::npos ? p_reads : p_part != std::string::npos ? p_part : p_key);
  // SOURCE BY <column>, anywhere between KEY and AS: taken out here so the clauses around it parse as before
  {
    const size_t p_src = sc.find_kw("SOURCE BY");
    if (p_src != std::string::npos && p_key != std::string::npos && p_as != std::string::npos && p_key < p_src && p_src < p_as) {
      size_t end = p_as;
      for (size_t q : {p_part, p_reads}) if (q != std::string::npos && q > p_src && q < end) end = q;
      k->source_by = unquote_ident(trim(sql.substr(p_src + 9, end - p_src - 9)));
      std::string rest = sql.substr(0, p_src) + std::string(end - p_src, ' ') + sql.substr(end);
      const std::string by = k->source_by;
      const bool ok = parse_create(rest, k, err);
      if (ok) { k->source_by = by; k->declaration = trim(sql); }
      if (ok && !k->row_mode) { *err = "SOURCE BY: a row Thing only (KEY and PARTITION BY the same column)"; return false; }
      if (ok && source_index(*k) < 0) { *err = "SOURCE BY " + k->source_by + ": no such value column in the query"; return false; }
      return ok;
    }
  }
  if (p_thing == std::string::npos || p_key == std::string::npos || p_as == std::string::npos || !(p_thing < p_key && p_key < p_as) ||
      (p_part != std::string::npos && !(p_key < p_part && p_part < p_as)) ||
      (p_reads != std::string::npos && !(p_key < p_reads && p_reads < p_as))) {
    *err = "CREATE THING <name> KEY <column> [PARTITION BY <column>] [READS <table>[, ...]] AS SELECT ... [PART <name> AS SELECT ... WHERE <col> = KEY]";
    return false;
  }
  const size_t key_end = p_part != std::string::npos ? p_part : p_reads != std::string::npos ? p_reads : p_as;
  k->name = lower(unquote_ident(sql.substr(p_thing + 5, p_key - p_thing - 5)));
  k->key_col = unquote_ident(sql.substr(p_key + 3, key_end - p_key - 3));
  if (p_part != std::string::npos) {
    const size_t part_end = p_reads != std::string::npos ? p_reads : p_as;
    k->part_expr = trim(sql.substr(p_part + 12, part_end - p_part - 12));
  } else {
    k->part_expr = k->key_col;
  }
  {
    // `so.orderno` or `so`.`orderno`: the queries say it as declared; change
    // notices find it in each table by its bare name.
    std::string bare = k->part_expr;
    const size_t dot = bare.rfind('.');
    if (dot != std::string::npos) bare = bare.substr(dot + 1);
    k->part_col = unquote_ident(bare);
    if (dot == std::string::npos) k->part_expr = k->part_col;
  }
  if (p_reads != std::string::npos) {
    for (const std::string& r : sc.split_commas(p_reads + 5, p_as)) {
      std::string t = lower(unquote_ident(r));
      size_t dot = t.find('.');
      if (dot != std::string::npos) t = t.substr(dot + 1);
      if (!t.empty()) k->reads.push_back(t);
    }
  }
  if (k->name.empty() || k->key_col.empty() || k->part_col.empty()) {
    *err = "CREATE THING needs a name and a KEY";
    return false;
  }
  // The head query ends where the first PART begins; each PART runs to the next.
  std::string body = trim(sql.substr(p_as + 2));
  if (!body.empty() && body.back() == ';') body.pop_back();
  std::vector<std::pair<std::string, std::string>> part_texts;   // (name, query)
  std::string select;
  {
    // WHEN <expr> DO <statements> TO <profile> at the very end: the last WHEN at depth 0 that a
    // DO follows with no THEN between (a CASE WHEN in a select list has its THEN).
    Scan ws(body);
    size_t wp = std::string::npos;
    for (size_t at = ws.find_kw("WHEN"); at != std::string::npos; at = ws.find_kw("WHEN", at + 4)) {
      const size_t dp = ws.find_kw("DO", at + 4), tp = ws.find_kw("THEN", at + 4);
      if (dp != std::string::npos && (tp == std::string::npos || tp > dp)) wp = at;
    }
    if (wp != std::string::npos) {
      const size_t dp = ws.find_kw("DO", wp + 4);
      std::string clause = body.substr(dp + 2);
      Scan cs(clause);
      size_t top = std::string::npos;
      for (size_t at = cs.find_kw("TO"); at != std::string::npos; at = cs.find_kw("TO", at + 2)) top = at;
      if (top == std::string::npos) { *err = "WHEN <condition> DO <statement>[; ...] TO <connection profile>"; return false; }
      k->when_expr = trim(body.substr(wp + 4, dp - wp - 4));
      k->do_sql = trim(clause.substr(0, top));
      k->do_target = unquote_ident(trim(clause.substr(top + 2)));
      if (k->when_expr.empty() || k->do_sql.empty() || k->do_target.empty()) { *err = "WHEN <condition> DO <statement>[; ...] TO <connection profile>"; return false; }
      body = trim(body.substr(0, wp));
    }
  }
  {
    Scan bs(body);
    size_t first = bs.find_kw("PART");
    select = trim(first == std::string::npos ? body : body.substr(0, first));
    size_t at = first;
    while (at != std::string::npos) {
      size_t next = bs.find_kw("PART", at + 4);
      std::string clause = trim(body.substr(at + 4, next == std::string::npos ? std::string::npos : next - at - 4));
      Scan cs(clause);
      size_t cas = cs.find_kw("AS");
      if (cas == std::string::npos) { *err = "PART <name> AS SELECT ..."; return false; }
      std::string pname = lower(unquote_ident(trim(clause.substr(0, cas))));
      std::string psql = trim(clause.substr(cas + 2));
      if (pname.empty() || psql.empty()) { *err = "PART <name> AS SELECT ..."; return false; }
      part_texts.emplace_back(pname, psql);
      at = next;
    }
  }
  Scan ss(select);
  size_t s_sel = ss.find_kw("SELECT");
  size_t s_from = ss.find_kw("FROM");
  size_t s_where = ss.find_kw("WHERE");
  size_t s_group = ss.find_kw("GROUP BY");
  if (s_sel != 0 || s_from == std::string::npos || s_group == std::string::npos) {
    *err = "the Thing's query must be SELECT <key>, <aggregates> FROM <source> [WHERE ...] GROUP BY <key>";
    return false;
  }
  for (const char* bad : {"HAVING", "ORDER BY", "LIMIT", "UNION", "WITH ROLLUP"}) {
    if (ss.find_kw(bad) != std::string::npos) {
      *err = std::string("a Thing's query cannot use ") + bad + ": partitions could not add up to it";
      return false;
    }
  }
  if (s_where != std::string::npos && (s_where < s_from || s_where > s_group)) {
    *err = "WHERE must come between FROM and GROUP BY";
    return false;
  }
  const size_t src_end = s_where == std::string::npos ? s_group : s_where;
  k->source = trim(select.substr(s_from + 4, src_end - s_from - 4));
  if (s_where != std::string::npos) k->where_text = trim(select.substr(s_where + 5, s_group - s_where - 5));
  k->group_text = trim(select.substr(s_group + 8));
  {
    // The KEY leads the GROUP BY; a row kind may group by more (the ERP's
    // open-orders query groups by orderno, debtorno - one row per order).
    std::string lead = k->group_text;
    const size_t comma = lead.find(',');
    if (comma != std::string::npos) lead = trim(lead.substr(0, comma));
    if (lower(unquote_ident(lead)) != lower(k->key_col)) {
      *err = "GROUP BY must start with the KEY column (" + k->key_col + ")";
      return false;
    }
  }
  k->row_mode = lower(unquote_ident(k->key_col)) == lower(unquote_ident(k->part_expr)) ||
                lower(unquote_ident(k->key_col)) == lower(k->part_col);
  std::vector<std::string> items = ss.split_commas(s_sel + 6, s_from);
  if (items.size() < 2) {
    *err = "the Thing's query needs the key and at least one value";
    return false;
  }
  for (size_t i = 0; i < items.size(); ++i) {
    Item it;
    it.text = items[i];
    Scan is(it.text);
    size_t p = is.find_kw("AS");
    std::string expr = p == std::string::npos ? it.text : trim(it.text.substr(0, p));
    it.label = p == std::string::npos ? expr : unquote_ident(it.text.substr(p + 2));
    if (i == 0) {
      if (lower(unquote_ident(expr)) != lower(k->key_col)) {
        *err = "the first column must be the KEY column (" + k->key_col + ")";
        return false;
      }
      // Named by its column (orderno), not the expression it was declared by
      // (so.orderno): that is the name a row of the table carries.
      if (p == std::string::npos) {
        const size_t dot = it.label.rfind('.');
        if (dot != std::string::npos) it.label = unquote_ident(it.label.substr(dot + 1));
      }
    } else if (k->row_mode) {
      // One row per key, read whole from its one partition: any expression,
      // `t.*` included (its names come from the first run).
      bool cl = false, sl = false;
      it.count_like = expr.find('*') == std::string::npos && linear(expr, &cl, &sl) && cl && !sl;
    } else {
      bool cl = false, sl = false;
      if (!linear(expr, &cl, &sl) || (cl && sl)) {
        *err = "value '" + expr + "' is not a SUM or COUNT (optionally times a number): a Thing's partitions "
               "must add up to exactly what its query returns";
        return false;
      }
      it.count_like = cl;
    }
    k->items.push_back(it);
  }
  k->time_dependent = time_function(upper(select));
  // The parts: where the KEY stands in each, which table that is, and the
  // text around it, so a run can read any set of keys in one query.
  for (const auto& pt : part_texts) {
    if (!k->row_mode) { *err = "PART " + pt.first + ": parts belong to a row kind (KEY and PARTITION BY the same column)"; return false; }
    Kind::PartDef p;
    p.name = pt.first;
    const std::string& q = pt.second;
    Scan qs(q);
    if (qs.find_kw("SELECT") != 0) { *err = "PART " + p.name + " must be a SELECT"; return false; }
    // KEY at any depth (a subquery may compare to it), outside quotes.
    size_t kp = std::string::npos;
    {
      char quote = 0;
      for (size_t i = 0; i + 3 <= q.size(); ++i) {
        const char c = q[i];
        if (quote) { if (c == quote) quote = 0; continue; }
        if (c == '\'' || c == '"' || c == '`') { quote = c; continue; }
        if (std::toupper((unsigned char)c) == 'K' && std::toupper((unsigned char)q[i + 1]) == 'E' && std::toupper((unsigned char)q[i + 2]) == 'Y' &&
            (i == 0 || !(std::isalnum((unsigned char)q[i - 1]) || q[i - 1] == '_' || q[i - 1] == '.')) &&
            (i + 3 == q.size() || !(std::isalnum((unsigned char)q[i + 3]) || q[i + 3] == '_'))) { kp = i; break; }
      }
    }
    if (kp == std::string::npos) { *err = "PART " + p.name + ": the query must compare a column to KEY (WHERE <col> = KEY)"; return false; }
    {
      // KEY in the part's own WHERE, not a subquery's: the rows come back with
      // the compared column in front, which a subquery cannot provide.
      int depth = 0;
      char quote = 0;
      for (size_t i = 0; i < kp; ++i) {
        const char c = q[i];
        if (quote) { if (c == quote) quote = 0; continue; }
        if (c == '\'' || c == '"' || c == '`') quote = c;
        else if (c == '(') ++depth;
        else if (c == ')') --depth;
      }
      if (depth != 0) { *err = "PART " + p.name + ": compare to KEY in the part's own WHERE, not inside a subquery (join the table instead)"; return false; }
    }
    auto ident_chars = [](char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '.' || c == '`' || c == '$'; };
    std::string before = q.substr(0, kp), after = q.substr(kp + 3);
    std::string lhs = trim(before), rhs = trim(after);
    if (!lhs.empty() && lhs.back() == '=') {
      // <expr> = KEY
      lhs.pop_back();
      lhs = trim(lhs);
      size_t e = lhs.size();
      while (e > 0 && ident_chars(lhs[e - 1])) --e;
      p.key_expr = lhs.substr(e);
      p.head = lhs.substr(0, e);
      p.tail = after;
    } else if (!rhs.empty() && rhs.front() == '=') {
      // KEY = <expr>
      rhs = trim(rhs.substr(1));
      size_t e = 0;
      while (e < rhs.size() && ident_chars(rhs[e])) ++e;
      p.key_expr = rhs.substr(0, e);
      p.head = before;
      p.tail = " " + rhs.substr(e);
    } else {
      *err = "PART " + p.name + ": KEY must be one side of an = (WHERE <col> = KEY)";
      return false;
    }
    if (p.key_expr.empty()) { *err = "PART " + p.name + ": nothing is compared to KEY"; return false; }
    // head starts after SELECT: the key goes in front of the select list
    {
      Scan hs(p.head);
      const size_t sel = hs.find_kw("SELECT");
      if (sel != 0) { *err = "PART " + p.name + ": the comparison to KEY must be in the SELECT's own WHERE"; return false; }
      p.head = p.head.substr(6);
    }
    std::string qual, col = p.key_expr;
    const size_t dot = col.rfind('.');
    if (dot != std::string::npos) { qual = lower(unquote_ident(col.substr(0, dot))); col = col.substr(dot + 1); }
    p.key_col = unquote_ident(col);
    // alias -> table over the FROM/JOIN list; the first table when unqualified
    std::vector<std::string> names;
    from_names(q, &names);
    std::map<std::string, std::string> alias;
    {
      std::string flat;
      for (char c : q) flat += (c == '(' || c == ')' || c == ',') ? ' ' : c;
      Scan fs(flat);
      for (const char* kw : {"FROM", "JOIN"}) {
        size_t at = 0;
        while ((at = fs.find_kw(kw, at)) != std::string::npos) {
          at += std::strlen(kw);
          size_t i = at;
          auto word = [&]() { while (i < flat.size() && std::isspace((unsigned char)flat[i])) ++i; size_t b = i; while (i < flat.size() && (ident_chars(flat[i]))) ++i; return flat.substr(b, i - b); };
          std::string t = word();
          if (t.empty()) continue;
          size_t save = i;
          std::string a = word();
          if (upper(a) == "AS") a = word();
          static const std::set<std::string> kws = {"ON", "WHERE", "LEFT", "RIGHT", "INNER", "JOIN", "CROSS", "GROUP", "ORDER", "LIMIT", "USING", "STRAIGHT_JOIN", "NATURAL", "SELECT", "AND", "OR"};
          if (a.empty() || kws.count(upper(a))) { i = save; a.clear(); }
          std::string bare = lower(unquote_ident(t));
          const size_t d2 = bare.find('.');
          if (d2 != std::string::npos) bare = bare.substr(d2 + 1);
          alias[bare] = bare;
          if (!a.empty()) alias[lower(unquote_ident(a))] = bare;
        }
      }
    }
    for (const std::string& n : names) {
      std::string bare = lower(unquote_ident(n));
      const size_t d2 = bare.find('.');
      if (d2 != std::string::npos) bare = bare.substr(d2 + 1);
      if (std::find(p.tables.begin(), p.tables.end(), bare) == p.tables.end()) p.tables.push_back(bare);
    }
    if (p.tables.empty()) { *err = "PART " + p.name + ": no table in its FROM"; return false; }
    if (!qual.empty()) {
      auto it = alias.find(qual);
      if (it == alias.end()) { *err = "PART " + p.name + ": " + p.key_expr + " names no table of its FROM"; return false; }
      p.key_table = it->second;
    } else {
      p.key_table.clear();   // unqualified: the table of the part's FROM that has the column (resolved at the first run)
    }
    for (const char* bad : {"LIMIT"}) {
      if (qs.find_kw(bad) != std::string::npos) { *err = "PART " + p.name + ": " + bad + " has no meaning over a set of keys"; return false; }
    }
    k->part_defs.push_back(std::move(p));
  }
  // READS not declared: every table the head and the parts name.
  if (k->reads.empty()) {
    std::vector<std::string> names;
    from_names("FROM " + k->source, &names);   // the source text follows a FROM the scanner needs to see
    if (!k->where_text.empty()) from_names(k->where_text, &names);
    for (const auto& it2 : k->items) from_names(it2.text, &names);
    for (const auto& p : k->part_defs) for (const auto& t : p.tables) names.push_back(t);
    for (const std::string& n : names) {
      std::string bare = lower(unquote_ident(n));
      const size_t d2 = bare.find('.');
      if (d2 != std::string::npos) bare = bare.substr(d2 + 1);
      if (!bare.empty() && std::find(k->reads.begin(), k->reads.end(), bare) == k->reads.end()) k->reads.push_back(bare);
    }
    if (k->reads.empty()) { *err = "the Thing's query names no table"; return false; }
  }
  k->declaration = trim(sql);
  return true;
}

std::string ThingEngine::full_sql(const Kind& k) const {
  std::string items;
  for (size_t i = 0; i < k.items.size(); ++i) items += (i ? ", " : "") + k.items[i].text;
  std::string sql = "SELECT " + k.part_expr + ", " + items + " FROM " + k.source;
  if (!k.where_text.empty()) sql += " WHERE (" + k.where_text + ")";
  sql += " GROUP BY " + k.part_expr + ", " + k.group_text;
  return sql;
}

std::string ThingEngine::partition_sql(const Kind& k, const std::vector<std::string>& parts) const {
  std::string list;
  for (size_t i = 0; i < parts.size(); ++i) list += (i ? "," : "") + (all_digits(parts[i]) ? parts[i] : sql_lit(parts[i]));
  std::string items;
  for (size_t i = 0; i < k.items.size(); ++i) items += (i ? ", " : "") + k.items[i].text;
  std::string sql = "SELECT " + k.part_expr + ", " + items + " FROM " + k.source + " WHERE ";
  if (!k.where_text.empty()) sql += "(" + k.where_text + ") AND ";
  sql += k.part_expr + (parts.size() == 1 ? " = " + list : " IN (" + list + ")") +
         " GROUP BY " + k.part_expr + ", " + k.group_text;
  return sql;
}

std::string ThingEngine::value_text(const Kind& k, const Instance& in, size_t item) const {
  const size_t v = item - 1;
  if (k.row_mode) return v < in.row.size() ? in.row[v] : sql_null();
  if (v >= in.sums.size() || in.nonnull[v] == 0) return k.items[item].count_like ? std::string("0") : sql_null();
  return dec_text(in.sums[v], in.scale[v]);
}

std::string ThingEngine::value_label(const Kind& k, size_t v) const {
  if (!k.out_labels.empty()) return v < k.out_labels.size() ? k.out_labels[v] : std::to_string(v);
  return v + 1 < k.items.size() ? k.items[v + 1].label : std::to_string(v);
}

int ThingEngine::source_index(const Kind& k) const {
  if (k.source_by.empty()) return -1;
  const size_t n = !k.out_labels.empty() ? k.out_labels.size() : (k.items.empty() ? 0 : k.items.size() - 1);
  for (size_t v = 0; v < n; ++v) {
    std::string l = value_label(k, v);
    const size_t dot = l.rfind('.');
    if (lower(unquote_ident(l)) == lower(k.source_by) || (dot != std::string::npos && lower(unquote_ident(l.substr(dot + 1))) == lower(k.source_by)))
      return (int)v;
  }
  return -1;
}

void ThingEngine::merge_sides(const Kind& k, Instance& in) const {
  const int src = source_index(k);
  in.conflicts.clear();
  in.ruled.clear();
  if (in.sides.empty()) { in.row.clear(); return; }
  size_t width = 0;
  for (const auto& sd : in.sides) width = std::max(width, sd.second.size());
  in.row.assign(width, sql_null());
  std::map<std::string, std::string> keep_picks;
  for (size_t v = 0; v < width; ++v) {
    if ((int)v == src) {
      std::string all;
      for (const auto& sd : in.sides) all += (all.empty() ? "" : ",") + sd.first;
      in.row[v] = all;
      continue;
    }
    const std::string label = value_label(k, v);
    std::set<std::string> distinct;
    for (const auto& sd : in.sides) distinct.insert(v < sd.second.size() ? sd.second[v] : sql_null());
    if (distinct.size() == 1) { in.row[v] = *distinct.begin(); continue; }   // they agree (a stale pick goes)
    in.conflicts.push_back(label);
    auto pk = in.picks.find(label);
    if (pk != in.picks.end()) {
      for (const auto& sd : in.sides)
        if (sd.first == pk->second) { in.row[v] = v < sd.second.size() ? sd.second[v] : sql_null(); keep_picks[label] = pk->second; break; }
    }
    if (keep_picks.count(label)) continue;
    // no pick by hand: a PREFER rule for this field (else the * rule) takes the first of its sources reporting here
    const std::vector<std::string>* order = nullptr;
    for (const auto& r : k.prefer) {
      if (lower(r.first) == lower(label)) { order = &r.second; break; }
      if (r.first == "*" && !order) order = &r.second;
    }
    if (!order) continue;
    for (const std::string& want : *order) {
      bool hit = false;
      for (const auto& sd : in.sides)
        if (sd.first == want) { in.row[v] = v < sd.second.size() ? sd.second[v] : sql_null(); in.ruled[label] = want; hit = true; break; }
      if (hit) break;
    }
  }
  in.picks = std::move(keep_picks);   // a choice whose source is gone, or whose sides now agree, is dropped
}

std::string ThingEngine::display(const Kind& k, const Instance& in) const {
  std::string s;
  if (!in.conflicts.empty()) {
    // both sides of every conflicted field, so history records a conflict arising and settling
    for (size_t v = 0; v < in.row.size(); ++v) {
      const std::string label = value_label(k, v);
      s += (v ? " | " : "") + label + "=";
      if (std::find(in.conflicts.begin(), in.conflicts.end(), label) == in.conflicts.end()) {
        s += sql_is_null(in.row[v]) ? std::string("NULL") : in.row[v];
        continue;
      }
      std::string both;
      for (const auto& sd : in.sides) {
        const std::string x = v < sd.second.size() ? sd.second[v] : sql_null();
        both += (both.empty() ? "" : "; ") + sd.first + ": " + (sql_is_null(x) ? std::string("NULL") : x);
      }
      auto pk = in.picks.find(label);
      auto rl = in.ruled.find(label);
      s += "{" + both + "}" + (pk != in.picks.end() ? " -> " + pk->second
                               : rl != in.ruled.end() ? " -> " + rl->second + " (by rule)" : std::string(" (conflict)"));
    }
    return s;
  }
  if (k.row_mode && !k.out_labels.empty()) {
    for (size_t v = 0; v < in.row.size(); ++v)
      s += (v ? " | " : "") + (v < k.out_labels.size() ? k.out_labels[v] : std::to_string(v)) + "=" +
           (sql_is_null(in.row[v]) ? std::string("NULL") : in.row[v]);
    return s;
  }
  for (size_t i = 1; i < k.items.size(); ++i) {
    std::string v = value_text(k, in, i);
    s += (i > 1 ? " | " : "") + k.items[i].label + "=" + (sql_is_null(v) ? std::string("NULL") : v);
  }
  return s;
}

void ThingEngine::apply_partition(Kind& k, const std::string& part, std::vector<Partial> fresh,
                                  std::map<std::string, std::string>* before_display) {
  const size_t nv = k.items.size() - 1;
  auto touch = [&](const Partial& pr) -> Instance& {
    const std::string ik = pr.key_null ? std::string(kNullKey) : pr.key;
    auto it = k.things.find(ik);
    if (it == k.things.end()) {
      Instance in;
      in.key = pr.key;
      in.key_null = pr.key_null;
      in.sums.assign(nv, ThingDec{});
      in.nonnull.assign(nv, 0);
      in.scale.assign(nv, 0);
      it = k.things.emplace(ik, std::move(in)).first;
    }
    if (!before_display->count(ik)) (*before_display)[ik] = display(k, it->second);
    return it->second;
  };
  auto fold = [&](const Partial& pr, int sign) {
    Instance& in = touch(pr);
    for (size_t i = 0; i < nv && i < pr.vals.size(); ++i) {
      if (sql_is_null(pr.vals[i])) continue;
      ThingDec d;
      if (!parse_dec(pr.vals[i], &d)) continue;   // checked when the partial was read
      dec_add(&in.sums[i], d, sign);
      in.nonnull[i] += sign;
      if (d.scale > in.scale[i]) in.scale[i] = d.scale;
    }
    in.parts += sign;
  };
  auto old = k.parts.find(part);
  if (k.row_mode) {
    // The partition's rows ARE its keys' values: replaced whole, and a key the
    // fresh read no longer returns is no longer a Thing.
    std::set<std::string> was, now;
    if (old != k.parts.end()) {
      for (const Partial& pr : old->second) was.insert(pr.key_null ? std::string(kNullKey) : pr.key);
      k.parts.erase(old);
    }
    const int src = source_index(k);
    if (src >= 0) {
      // every source's row kept; the Thing's values merged from them (no row overwrites another)
      std::map<std::string, std::vector<std::pair<std::string, std::vector<std::string>>>> by_key;
      for (const Partial& pr : fresh) {
        Instance& in = touch(pr);
        const std::string ik = pr.key_null ? std::string(kNullKey) : pr.key;
        const std::string from = (size_t)src < pr.vals.size() ? pr.vals[(size_t)src] : sql_null();
        by_key[ik].emplace_back(sql_is_null(from) ? std::string("(unnamed)") : from, pr.vals);
        in.parts = 1;
        now.insert(ik);
      }
      for (auto& kv : by_key) {
        Instance& in = k.things[kv.first];
        std::stable_sort(kv.second.begin(), kv.second.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        in.sides = std::move(kv.second);
        merge_sides(k, in);
      }
    } else {
      for (const Partial& pr : fresh) {
        Instance& in = touch(pr);
        in.row = pr.vals;
        in.parts = 1;
        now.insert(pr.key_null ? std::string(kNullKey) : pr.key);
      }
    }
    for (const std::string& ik : was) {
      if (now.count(ik)) continue;
      k.things.erase(ik);
      before_display->erase(ik);
      k.left++;
      k.version++;
      k.departed[ik] = k.version;
      if (k.departed.size() > 50000) k.departed.clear();   // a client that old reloads anyway
      dirty_ = true;
    }
    if (!fresh.empty()) k.parts.emplace(part, std::move(fresh));
    return;
  }
  if (old != k.parts.end()) {
    for (const Partial& pr : old->second) fold(pr, -1);
    k.parts.erase(old);
  }
  for (const Partial& pr : fresh) fold(pr, +1);
  if (!fresh.empty()) k.parts.emplace(part, std::move(fresh));
}

void ThingEngine::finish_changes(Kind& k, const std::map<std::string, std::string>& before_display,
                                 const std::string& cause) {
  const uint64_t now = db_.kernel_.host().wall_ns();
  for (const auto& kv : before_display) {
    auto it = k.things.find(kv.first);
    if (it == k.things.end()) continue;
    Instance& in = it->second;
    std::string after = display(k, in);
    if (after == kv.second) continue;
    in.version++;
    in.updated_ns = now;
    in.history.push_back(History{now, cause, kv.second, after});
    if (in.history.size() > kHistoryKeep) in.history.erase(in.history.begin());
    k.version++;
    in.stamp = k.version;
    dirty_ = true;
  }
}

void ThingEngine::stamp_instance(Kind& k, Instance& in) {
  in.version++;
  in.updated_ns = db_.kernel_.host().wall_ns();
  k.version++;
  in.stamp = k.version;
  dirty_ = true;
}

// The part's query over a set of keys: SELECT <expr> AS the key, <its own
// list> ... WHERE <expr> IN (keys) ... - what was written, with KEY filled in.
std::string ThingEngine::part_sql(const Kind& k, const Kind::PartDef& p, const std::vector<std::string>& keys) const {
  (void)k;
  std::string list;
  for (size_t i = 0; i < keys.size(); ++i) list += (i ? "," : "") + (all_digits(keys[i]) ? keys[i] : sql_lit(keys[i]));
  // A GROUP BY in the part groups per key: the key leads it.
  std::string tail = p.tail;
  {
    Scan ts(tail);
    const size_t g = ts.find_kw("GROUP BY");
    if (g != std::string::npos) tail = tail.substr(0, g + 8) + " " + p.key_expr + "," + tail.substr(g + 8);
  }
  return "SELECT " + p.key_expr + " AS `__thing_key`, " + p.head + p.key_expr +
         (keys.size() == 1 ? " = " + list : " IN (" + list + ")") + tail;
}

bool ThingEngine::resolve_part_deps(Kind& k, std::string* err) {
  for (auto& p : k.part_defs) {
    if (p.key_table.empty()) {
      // An unqualified column: the one table of the part's FROM that has it.
      std::vector<std::string> owners;
      for (const auto& t : p.tables) {
        const SqlTable* tt = db_.resolve_table(t, k.db);
        if (tt && db_.is_stored_table(tt) && tt->get_column_index(p.key_col) >= 0) owners.push_back(t);
      }
      if (owners.empty()) { *err = "PART " + p.name + ": no table of its FROM has a column " + p.key_col; return false; }
      if (owners.size() > 1) { *err = "PART " + p.name + ": " + p.key_col + " is ambiguous (" + owners[0] + ", " + owners[1] + "): qualify it"; return false; }
      p.key_table = owners[0];
    }
    const SqlTable* kt = db_.resolve_table(p.key_table, k.db);
    if (!kt || !db_.is_stored_table(kt)) { *err = "PART " + p.name + ": no table " + p.key_table; return false; }
    if (kt->get_column_index(p.key_col) < 0) { *err = "PART " + p.name + ": " + p.key_table + " has no column " + p.key_col; return false; }
    p.key_dep = table_key(*kt);
    p.deps_full.clear();
    for (const auto& t : p.tables) {
      if (t == p.key_table) continue;
      const SqlTable* tt = db_.resolve_table(t, k.db);
      if (tt && db_.is_stored_table(tt)) p.deps_full.insert(table_key(*tt));
    }
  }
  return true;
}

// One part, for the keys given (every Thing when null): the rows per key
// replace the old ones, and a key whose rows differ is stamped as changed.
bool ThingEngine::run_part(Kind& k, Kind::PartDef& p, const std::vector<std::string>* keys, const std::string& cause,
                           std::string* err) {
  std::vector<std::string> all;
  if (!keys) {
    for (const auto& kv : k.things) if (!kv.second.key_null) all.push_back(kv.second.key);
    keys = &all;
  }
  constexpr size_t kBatch = 400;
  for (size_t at = 0; at < keys->size(); at += kBatch) {
    std::vector<std::string> batch(keys->begin() + (long)at, keys->begin() + (long)std::min(keys->size(), at + kBatch));
    SqlQueryResult r = db_.execute_local_query(part_sql(k, p, batch), k.db);
    if (!r.success) { *err = "PART " + p.name + ": " + r.error_message; return false; }
    if (p.cols.empty() && r.columns.size() > 1) p.cols.assign(r.columns.begin() + 1, r.columns.end());
    std::map<std::string, std::vector<std::vector<std::string>>> fresh;
    for (const SqlRow& row : r.rows) {
      if (row.values.empty()) continue;
      std::vector<std::string> vals(row.values.begin() + 1, row.values.end());
      fresh[canon(row.values[0])].push_back(std::move(vals));
    }
    for (const std::string& key : batch) {
      auto it = k.things.find(key);
      if (it == k.things.end()) {
        // keys arrive canonical from a change notice, as declared from the head
        bool found = false;
        for (auto& kv : k.things) if (!kv.second.key_null && canon(kv.second.key) == canon(key)) { it = k.things.find(kv.first); found = true; break; }
        if (!found) continue;
      }
      Instance& in = it->second;
      auto f = fresh.find(canon(key));
      std::vector<std::vector<std::string>> rows = f == fresh.end() ? std::vector<std::vector<std::string>>{} : std::move(f->second);
      auto old = in.part_rows.find(p.name);
      const bool same = old != in.part_rows.end() ? old->second == rows : rows.empty();
      if (same) continue;
      const std::string before = "part " + p.name + ": " + std::to_string(old == in.part_rows.end() ? 0 : old->second.size()) + " rows";
      in.part_rows[p.name] = std::move(rows);
      in.history.push_back(History{db_.kernel_.host().wall_ns(), cause, before,
                                   "part " + p.name + ": " + std::to_string(in.part_rows[p.name].size()) + " rows"});
      if (in.history.size() > kHistoryKeep) in.history.erase(in.history.begin());
      stamp_instance(k, in);
    }
  }
  return true;
}

// Does WHEN hold for this Thing: the engine's own SQL over the row's columns, a derived
// table of its values (the key and every output column; a later duplicate name wins, as a
// row read by name would).
bool ThingEngine::condition_holds(const Kind& k, const Instance& in, bool* holds, std::string* err) {
  std::vector<std::string> labels;
  std::vector<std::string> values;
  labels.push_back(k.items[0].label);
  values.push_back(in.key_null ? sql_null() : in.key);
  if (k.row_mode && !k.out_labels.empty()) {
    for (size_t v = 0; v < k.out_labels.size(); ++v) { labels.push_back(k.out_labels[v]); values.push_back(v < in.row.size() ? in.row[v] : sql_null()); }
  } else {
    for (size_t i = 1; i < k.items.size(); ++i) { labels.push_back(k.items[i].label); values.push_back(value_text(k, in, i)); }
  }
  std::string cols;
  for (size_t i = 0; i < labels.size(); ++i) {
    bool later = false;
    for (size_t j = i + 1; j < labels.size() && !later; ++j) later = lower(labels[j]) == lower(labels[i]);
    if (later) continue;
    cols += (cols.empty() ? "" : ", ") + std::string(sql_is_null(values[i]) ? "NULL" : sql_lit(values[i])) + " AS `" + labels[i] + "`";
  }
  const std::string sql = "SELECT CASE WHEN (" + k.when_expr + ") THEN 1 ELSE 0 END AS ok FROM (SELECT " + cols + ") AS thing";
  SqlQueryResult r = db_.execute_local_query(sql, k.db);
  if (!r.success) { *err = "WHEN " + k.when_expr + ": " + r.error_message; return false; }
  *holds = !r.rows.empty() && !r.rows[0].values.empty() && r.rows[0].values[0] == "1";
  return true;
}

// The DO statements, KEY filled in, in one transaction on the profile.
bool ThingEngine::run_act(const Kind& k, const Instance& in, std::string* err) {
  DbConnectionProfile prof;
  bool found = false;
  for (const auto& p : db_.list_profiles()) if (p.id == k.do_target) { prof = p; found = true; }
  if (!found) { *err = "DO .. TO " + k.do_target + ": no such connection profile"; return false; }
  if (!prof.things_write) { *err = "DO .. TO " + k.do_target + ": the profile is not marked things_write=1"; return false; }
  const std::string key = in.key_null ? std::string("NULL") : (all_digits(in.key) ? in.key : sql_lit(in.key));
  // KEY as a word outside quotes -> the key, unless it is SQL's own KEY (ON DUPLICATE KEY
  // UPDATE, PRIMARY KEY ..); ';' outside quotes splits the statements
  std::vector<std::string> stmts;
  {
    std::string cur;
    char quote = 0;
    const std::string& q = k.do_sql;
    auto prev_word = [&](size_t end) {
      size_t e = end;
      while (e > 0 && std::isspace((unsigned char)q[e - 1])) --e;
      size_t b = e;
      while (b > 0 && (std::isalnum((unsigned char)q[b - 1]) || q[b - 1] == '_')) --b;
      return upper(q.substr(b, e - b));
    };
    for (size_t i = 0; i < q.size(); ++i) {
      const char c = q[i];
      if (quote) { cur += c; if (c == quote) quote = 0; continue; }
      if (c == '\'' || c == '"' || c == '`') { quote = c; cur += c; continue; }
      if (c == ';') { if (!trim(cur).empty()) stmts.push_back(trim(cur)); cur.clear(); continue; }
      if ((c == 'K' || c == 'k') && i + 3 <= q.size() && std::toupper((unsigned char)q[i + 1]) == 'E' && std::toupper((unsigned char)q[i + 2]) == 'Y' &&
          (i == 0 || !(std::isalnum((unsigned char)q[i - 1]) || q[i - 1] == '_' || q[i - 1] == '.')) &&
          (i + 3 == q.size() || !(std::isalnum((unsigned char)q[i + 3]) || q[i + 3] == '_'))) {
        const std::string pw = prev_word(i);
        if (pw == "DUPLICATE" || pw == "PRIMARY" || pw == "UNIQUE" || pw == "FOREIGN" || pw == "ADD" || pw == "DROP" || pw == "INDEX") { cur += "KEY"; i += 2; continue; }
        cur += key; i += 2; continue;
      }
      cur += c;
    }
    if (!trim(cur).empty()) stmts.push_back(trim(cur));
  }
  if (stmts.empty()) { *err = "DO: nothing to run"; return false; }
  if (auto t = sql_profile_transport_ok(prof); !t.ok()) { *err = "DO .. TO " + k.do_target + ": " + t.error().message; return false; }
  MariaWireSession target(sql_profile_dial_host(prof), prof.port);
  auto c = target.connect(prof.user, prof.password, prof.database);
  if (!c.ok()) { *err = "DO .. TO " + k.do_target + ": cannot connect: " + c.error().message; return false; }
  auto one = [&](const std::string& stmt) -> bool {
    auto r = target.query(stmt);
    if (!r.ok()) { *err = stmt + " on " + k.do_target + ": " + r.error().message; return false; }
    if (!r->ok) { *err = stmt + " on " + k.do_target + ": " + r->error_message; return false; }
    return true;
  };
  if (!one("START TRANSACTION")) return false;
  for (const auto& s : stmts) if (!one(s)) { auto r = target.query("ROLLBACK"); (void)r; *err += " (rolled back)"; return false; }
  return one("COMMIT");
}

// Every Thing that changed since the last pass: WHEN holds and it has not acted -> DO; WHEN
// does not hold -> armed again. Each act is a history line and a change of its own.
void ThingEngine::act_due(Kind& k) {
  if (k.do_sql.empty()) return;
  const uint64_t upto = k.version;
  bool did_act = false;
  for (auto& kv : k.things) {
    Instance& in = kv.second;
    if (in.stamp <= k.act_clock && k.act_clock != 0) continue;
    bool holds = false;
    std::string err;
    if (!condition_holds(k, in, &holds, &err)) { fprintf(stderr, "[things] %s: %s\n", k.name.c_str(), err.c_str()); continue; }
    if (!holds) { if (in.acted) { in.acted = false; dirty_ = true; } continue; }
    if (in.acted) continue;
    const uint64_t now = db_.kernel_.host().wall_ns();
    if (run_act(k, in, &err)) {
      in.acted = true;
      in.history.push_back(History{now, "acted", "WHEN " + k.when_expr, "DO " + k.do_sql + " TO " + k.do_target});
      fprintf(stderr, "[things] %s %s: acted (%s)\n", k.name.c_str(), in.key.c_str(), k.do_sql.c_str());
    } else {
      in.history.push_back(History{now, "act failed", "WHEN " + k.when_expr, err});
      fprintf(stderr, "[things] %s %s: act failed: %s\n", k.name.c_str(), in.key.c_str(), err.c_str());
    }
    if (in.history.size() > kHistoryKeep) in.history.erase(in.history.begin());
    k.version++;
    in.stamp = k.version;
    dirty_ = true;
    did_act = true;
  }
  k.act_clock = std::max(upto, k.version);
  // An act is a side effect in another database: its record goes to disk now, not at the
  // next periodic save, or a crash in between would do the work twice on restart.
  if (did_act) {
    auto& host = db_.kernel_.host();
    host.make_dir(things_dir());
    const std::string text = serialize(k);
    host.write_file(things_dir() + "/" + k.name + ".thing", ByteSpan((const uint8_t*)text.data(), text.size()));
  }
}

void ThingEngine::settle_parts(Kind& k) {
  if (k.part_defs.empty()) return;
  std::string err;
  for (auto& p : k.part_defs) {
    const bool full = k.pending_part_full.count(p.name) != 0;
    auto pk = k.pending_part_keys.find(p.name);
    if (!full && pk == k.pending_part_keys.end()) continue;
    std::vector<std::string> keys;
    if (!full && pk != k.pending_part_keys.end()) keys.assign(pk->second.begin(), pk->second.end());
    const std::string cause = full ? "a change to a table " + p.name + " reads" : p.key_table + " change for " + std::to_string(keys.size()) + " key(s)";
    if (std::getenv("AIOS_THINGS_TRACE")) fprintf(stderr, "[things-trace] settle_parts %s part %s full=%d keys=%zu\n", k.name.c_str(), p.name.c_str(), (int)full, keys.size());
    if (!run_part(k, p, full ? nullptr : &keys, cause, &err))
      fprintf(stderr, "[things] %s: part %s failed: %s\n", k.name.c_str(), p.name.c_str(), err.c_str());
  }
  k.pending_part_full.clear();
  k.pending_part_keys.clear();
}

bool ThingEngine::run_partitions(Kind& k, const std::vector<std::string>& parts, const std::string& cause,
                                 std::string* err) {
  SqlQueryResult r = db_.execute_local_query(partition_sql(k, parts), k.db);
  if (!r.success) { *err = r.error_message; return false; }
  std::unordered_map<std::string, std::vector<Partial>> fresh;
  for (const SqlRow& row : r.rows) {
    if (row.values.size() < k.items.size() + 1) continue;
    Partial pr;
    pr.key_null = sql_is_null(row.values[1]);
    pr.key = pr.key_null ? std::string() : row.values[1];
    for (size_t i = 2; i < row.values.size(); ++i) {
      ThingDec d;
      if (!k.row_mode && !sql_is_null(row.values[i]) && !parse_dec(row.values[i], &d)) {
        *err = "value '" + row.values[i] + "' of " + k.items[i - 1].label + " is not an exact number";
        return false;
      }
      pr.vals.push_back(row.values[i]);
    }
    fresh[canon(row.values[0])].push_back(std::move(pr));
  }
  std::map<std::string, std::string> before;
  for (const std::string& p : parts) {
    auto f = fresh.find(p);
    apply_partition(k, p, f == fresh.end() ? std::vector<Partial>{} : std::move(f->second), &before);
  }
  k.reactions += parts.size();
  finish_changes(k, before, cause);
  // A row kind's partitions are its keys: their parts are re-read too.
  if (k.row_mode && !k.part_defs.empty()) {
    for (auto& p : k.part_defs)
      if (!run_part(k, p, &parts, cause, err)) return false;
  }
  return true;
}

// The names a FROM/JOIN clause (or a WHERE's subquery) reads from: every
// identifier that follows FROM or JOIN outside quotes, "db.name" or "name",
// backticks removed. A "(" after the keyword is a derived table: skipped.
static void from_names(const std::string& text, std::vector<std::string>* out) {
  Scan sc(text);
  auto ident_at = [&](size_t i) -> std::pair<std::string, size_t> {
    std::string id;
    while (i < text.size() && std::isspace((unsigned char)text[i])) ++i;
    for (int part = 0; part < 2 && i < text.size(); ++part) {
      if (text[i] == '`') {
        ++i;
        while (i < text.size() && text[i] != '`') id += text[i++];
        if (i < text.size()) ++i;
      } else {
        while (i < text.size() && (std::isalnum((unsigned char)text[i]) || text[i] == '_' || text[i] == '$')) id += text[i++];
      }
      if (i < text.size() && text[i] == '.' && part == 0) { id += '.'; ++i; continue; }
      break;
    }
    return {id, i};
  };
  // Every FROM/JOIN at any depth: the scanner only finds depth-0 words, so
  // walk the parenthesised pieces too by scanning the text with depth ignored.
  std::string flat;
  flat.reserve(text.size());
  {
    char q = 0;
    for (size_t i = 0; i < text.size(); ++i) {
      char c = text[i];
      if (q) {
        if (c == q) { if (i + 1 < text.size() && text[i + 1] == q) { flat += c; flat += c; ++i; continue; } q = 0; }
        else if (c == '\\' && q != '`') { flat += c; if (i + 1 < text.size()) flat += text[++i]; continue; }
        flat += c;
        continue;
      }
      if (c == '\'' || c == '"' || c == '`') { q = c; flat += c; continue; }
      flat += (c == '(' || c == ')') ? ' ' : c;
    }
  }
  Scan fs(flat);
  for (const char* kw : {"FROM", "JOIN"}) {
    size_t p = 0;
    while ((p = fs.find_kw(kw, p)) != std::string::npos) {
      p += std::strlen(kw);
      std::pair<std::string, size_t> id = ident_at(p);
      if (!id.first.empty()) {
        // a comma list after FROM: "FROM a, b"
        out->push_back(id.first);
        size_t j = id.second;
        for (;;) {
          while (j < flat.size() && std::isspace((unsigned char)flat[j])) ++j;
          // skip an alias: an identifier that is not a keyword
          std::pair<std::string, size_t> nx = ident_at(j);
          if (!nx.first.empty()) {
            const std::string u = upper(nx.first);
            if (u == "AS") { nx = ident_at(nx.second); }
            else if (u == "ON" || u == "WHERE" || u == "LEFT" || u == "RIGHT" || u == "INNER" || u == "JOIN" || u == "CROSS" ||
                     u == "GROUP" || u == "ORDER" || u == "LIMIT" || u == "UNION" || u == "HAVING" || u == "USING" || u == "STRAIGHT_JOIN" || u == "NATURAL")
              break;
            j = nx.second;
          }
          while (j < flat.size() && std::isspace((unsigned char)flat[j])) ++j;
          if (j >= flat.size() || flat[j] != ',') break;
          std::pair<std::string, size_t> more = ident_at(j + 1);
          if (more.first.empty()) break;
          out->push_back(more.first);
          j = more.second;
        }
      }
    }
  }
}

void ThingEngine::view_tables(const std::vector<std::string>& texts, const std::string& db,
                              std::set<std::string>* tables, std::vector<std::string>* via) const {
  std::set<std::string> seen;
  std::vector<std::pair<std::string, std::string>> todo;   // (view name, database)
  auto note = [&](const std::string& raw, const std::string& in_db) {
    std::string name = raw, ndb = in_db;
    size_t dot = name.find('.');
    if (dot != std::string::npos) { ndb = name.substr(0, dot); name = name.substr(dot + 1); }
    if (const SqlView* v = db_.find_view(name, ndb)) {
      const std::string key = lower(v->database + "." + v->name);
      if (seen.insert(key).second) todo.push_back({v->name, v->database});
    }
  };
  for (const std::string& t : texts) {
    std::vector<std::string> names;
    from_names(t, &names);
    for (const std::string& n : names) note(n, db);
    // a READS entry is a bare name, not a clause
    if (names.empty()) note(unquote_ident(t), db);
  }
  while (!todo.empty()) {
    std::pair<std::string, std::string> v = todo.back();
    todo.pop_back();
    const SqlView* view = db_.find_view(v.first, v.second);
    if (!view) continue;
    std::vector<std::string> names;
    from_names(view->query_sql, &names);
    for (const std::string& n : names) {
      std::string name = n, ndb = view->database;
      size_t dot = name.find('.');
      if (dot != std::string::npos) { ndb = name.substr(0, dot); name = name.substr(dot + 1); }
      if (db_.find_view(name, ndb)) { note(n, view->database); continue; }
      if (const SqlTable* t = db_.resolve_table(name, ndb)) {
        if (!db_.is_stored_table(t)) continue;
        if (tables->insert(table_key(*t)).second) via->push_back(lower(t->name) + " (via " + lower(view->name) + ")");
      }
    }
  }
}

bool ThingEngine::may_read(const std::vector<std::string>& reads, const std::vector<std::string>& texts, const std::string& db,
                           const SqlTable& t, std::set<std::string>* view_tabs, std::vector<std::string>* via) const {
  const std::string nm = lower(t.name);
  if (std::find(reads.begin(), reads.end(), nm) != reads.end()) return true;
  if (via->empty() && view_tabs->empty()) {
    std::vector<std::string> all = texts;
    for (const std::string& r : reads) all.push_back(r);
    view_tables(all, db, view_tabs, via);
  }
  return view_tabs->count(table_key(t)) != 0;
}

bool ThingEngine::run_full(Kind& k, const std::string& cause, std::string* err) {
  if (k.time_dependent) k.day = current_day();
  std::unordered_set<const SqlTable*> read;
  sql_set_read_capture(&read);
  SqlQueryResult r = db_.execute_local_query(full_sql(k), k.db);
  sql_set_read_capture(nullptr);
  if (!r.success) { *err = r.error_message; return false; }
  // Links: the tables the executor actually read, all within READS - or
  // behind a view the query names: a view's definition is the source's, and
  // when it gains a join (stocktrans gained purchdata and suppliers) the
  // Thing must read those too, or it can never recompute again.
  std::set<std::string> view_tabs;
  std::vector<std::string> via;
  for (const SqlTable* t : read) {
    if (!db_.is_stored_table(t)) continue;
    if (!may_read(k.reads, {k.source, k.where_text}, k.db, *t, &view_tabs, &via)) {
      *err = "Thing '" + k.name + "' would read " + t->database + "." + t->name + ", which is not in its READS list";
      return false;
    }
    const std::string key = table_key(*t);
    if (t->get_column_index(k.part_col) >= 0) k.deps_partition.insert(key);
    else k.deps_full.insert(key);
  }
  // Only the view tables the executor actually read: a declared read that a
  // view no longer joins is not a link.
  k.view_reads.clear();
  for (const std::string& v : via) {
    const std::string tn = v.substr(0, v.find(' '));
    for (const SqlTable* t : read)
      if (db_.is_stored_table(t) && lower(t->name) == tn &&
          std::find(k.reads.begin(), k.reads.end(), tn) == k.reads.end()) { k.view_reads.push_back(v); break; }
  }
  std::unordered_map<std::string, std::vector<Partial>> fresh;
  for (const SqlRow& row : r.rows) {
    if (row.values.size() < k.items.size() + 1) continue;
    Partial pr;
    pr.key_null = sql_is_null(row.values[1]);
    pr.key = pr.key_null ? std::string() : row.values[1];
    for (size_t i = 2; i < row.values.size(); ++i) {
      ThingDec d;
      if (!k.row_mode && !sql_is_null(row.values[i]) && !parse_dec(row.values[i], &d)) {
        *err = "value '" + row.values[i] + "' of " + k.items[i - 1].label + " is not an exact number";
        return false;
      }
      pr.vals.push_back(row.values[i]);
    }
    fresh[canon(row.values[0])].push_back(std::move(pr));
  }
  // A `*` in the head: the names are the query's own, learnt here.
  if (k.row_mode && r.columns.size() > 2) {
    bool star = false;
    for (const auto& it : k.items) if (it.text.find('*') != std::string::npos) star = true;
    if (star || r.columns.size() != k.items.size() + 1) k.out_labels.assign(r.columns.begin() + 2, r.columns.end());
  }
  std::map<std::string, std::string> before;
  std::vector<std::string> gone;
  for (const auto& kv : k.parts)
    if (!fresh.count(kv.first)) gone.push_back(kv.first);
  for (const std::string& p : gone) apply_partition(k, p, {}, &before);
  for (auto& kv : fresh) apply_partition(k, kv.first, std::move(kv.second), &before);
  k.full_recomputes++;
  finish_changes(k, before, cause);
  if (!k.part_defs.empty()) {
    if (!resolve_part_deps(k, err)) return false;
    for (auto& p : k.part_defs)
      if (!run_part(k, p, nullptr, cause, err)) return false;
    k.pending_part_full.clear();
    k.pending_part_keys.clear();
  }
  rebuild_watched();
  dirty_ = true;
  return true;
}

void ThingEngine::settle_kind(Kind& k) {
  if (k.paused) return;   // its notices wait (and RESUME re-reads it whole)
  if (k.type == Kind::Grain) { settle_grain(k); return; }
  if (k.type != Kind::Aggregate) return;   // resources and claims settle together (settle_links)
  if (k.time_dependent && !k.pending_full) {
    std::string day = current_day();
    if (!day.empty() && day != k.day) { k.pending_full = true; k.full_cause = "the day turned: " + day; }
  }
  std::string err;
  if (k.pending_full) {
    const std::string cause = k.full_cause.empty() ? std::string("full recompute") : k.full_cause;
    k.pending_full = false;
    k.pending.clear();
    k.full_cause.clear();
    if (!run_full(k, cause, &err)) fprintf(stderr, "[things] %s: full recompute failed: %s\n", k.name.c_str(), err.c_str());
    else act_due(k);
    return;
  }
  if (k.pending.empty()) { settle_parts(k); act_due(k); return; }
  std::vector<std::pair<std::string, std::string>> todo(k.pending.begin(), k.pending.end());
  k.pending.clear();
  std::sort(todo.begin(), todo.end());
  for (size_t at = 0; at < todo.size(); at += kPartitionChunk) {
    const size_t end = std::min(todo.size(), at + kPartitionChunk);
    std::vector<std::string> parts;
    for (size_t i = at; i < end; ++i) parts.push_back(todo[i].first);
    const std::string cause = end - at == 1 ? todo[at].second
                                            : todo[at].second + " (+" + std::to_string(end - at - 1) + " more)";
    if (!run_partitions(k, parts, cause, &err)) {
      fprintf(stderr, "[things] %s: reaction failed (%s); recomputing in full\n", k.name.c_str(), err.c_str());
      k.pending_full = true;
      k.full_cause = "after a failed reaction";
      settle_kind(k);
      return;
    }
  }
  settle_parts(k);
  act_due(k);
}

void ThingEngine::settle_all_locked() {
  MuHeld held;
  for (auto& kv : kinds_)
    if (kv.second.type != Kind::Grain) settle_kind(kv.second);
  settle_links();
  // Grain kinds last: they may read the queues just shared out (LINKS).
  for (auto& kv : kinds_)
    if (kv.second.type == Kind::Grain) settle_kind(kv.second);
}

namespace {
// Every kind's clock added up: it moves when any Thing changed, so waiters are woken only then.
template <class M> uint64_t clocks_of(const M& kinds) {
  uint64_t s = 0;
  for (const auto& kv : kinds) s += kv.second.version;
  return s;
}
}  // namespace

void ThingEngine::wake_waiters() {
  {
    std::lock_guard<std::mutex> g(wait_mu_);
    ++wait_epoch_;
  }
  wait_cv_.notify_all();
}

void ThingEngine::settle() {
  bool moved = false;
  {
    std::lock_guard<aios::hal::Mutex> lk(mu_);
    const uint64_t before = clocks_of(kinds_);
    settle_all_locked();
    moved = clocks_of(kinds_) != before;
  }
  if (moved) wake_waiters();
}

void ThingEngine::tick() {
  bool moved = false;
  {
    std::lock_guard<aios::hal::Mutex> lk(mu_);
    MuHeld held;
    const uint64_t before = clocks_of(kinds_);
    for (auto& kv : kinds_)
      if (kv.second.type != Kind::Grain &&
          (kv.second.time_dependent || kv.second.pending_full || !kv.second.pending.empty() || kv.second.pending_shared ||
           !kv.second.pending_part_keys.empty() || !kv.second.pending_part_full.empty()))
        settle_kind(kv.second);
    settle_links();
    for (auto& kv : kinds_)
      if (kv.second.type == Kind::Grain) settle_kind(kv.second);
    moved = clocks_of(kinds_) != before;
  }
  if (moved) wake_waiters();
}

std::string ThingEngine::waiting_for(const Kind& k) const {
  std::string w;
  if (k.type == Kind::Grain) {
    if (!k.grain_error.empty()) w = "ERROR (last run): " + k.grain_error + "; ";
    w += "a change to ";
    for (size_t i = 0; i < k.reads.size(); ++i) w += (i ? ", " : "") + k.reads[i];
    for (const auto& l : k.links) w += "; " + l.first + "'s queues";
    // Things that wait: the soonest wake_on a program gave any of them.
    std::string next;
    size_t waking = 0;
    for (const auto& g : k.gthings)
      for (const auto& f : g.second.vals)
        if (f.first == "wake_on" && !sql_is_null(f.second)) {
          if (next.empty() || f.second < next) { next = f.second; waking = 0; }
          if (f.second == next) ++waking;
        }
    if (!next.empty()) w += "; next wake " + next + " (" + std::to_string(waking) + " thing" + (waking == 1 ? "" : "s") + ")";
    if (!k.wakes.empty())
      w += "; next wake_at " + k.wakes.begin()->first + " (" + std::to_string(k.wakes.begin()->second.size()) + " thing" +
           (k.wakes.begin()->second.size() == 1 ? "" : "s") + ")";
    char ms[48];
    std::snprintf(ms, sizeof ms, "; the day after %s (full pass %.0f ms)", k.day.empty() ? "today" : k.day.c_str(), k.last_full_ms);
    return w + ms;
  }
  if (!k.deps_partition.empty()) {
    w += "a change to ";
    bool first = true;
    for (const auto& d : k.deps_partition) { w += (first ? "" : ", ") + d.substr(d.find('.') + 1); first = false; }
    w += " (the " + k.part_col + " it touches)";
  }
  if (!k.deps_full.empty()) {
    w += w.empty() ? "a change to " : "; any change to ";
    bool first = true;
    for (const auto& d : k.deps_full) { w += (first ? "" : ", ") + d.substr(d.find('.') + 1); first = false; }
  }
  if (k.time_dependent) w += (w.empty() ? "" : "; ") + std::string("the day after ") + (k.day.empty() ? "today" : k.day);
  return w;
}

// Asked of every statement the gateway sees, so it reads only the first two
// words, upper-cased in place: it copied and upper-cased the whole statement,
// then copied the rest again for each verb it tried - ~26 KB of copies for a
// 2 KB SELECT, to learn it was not a Thing command.
bool ThingEngine::handles(const std::string& sql) const {
  auto word = [&](size_t& i, char* out, size_t cap) {
    while (i < sql.size() && std::isspace((unsigned char)sql[i])) ++i;
    size_t n = 0;
    while (i < sql.size() && std::isalnum((unsigned char)sql[i])) {
      if (n + 1 < cap) out[n++] = (char)std::toupper((unsigned char)sql[i]);
      else return (size_t)0;   // longer than any word asked about
      ++i;
    }
    out[n] = 0;
    return n;
  };
  size_t i = 0;
  char a[8], b[8];
  if (!word(i, a, sizeof a) || !word(i, b, sizeof b)) return false;
  static const char* const verbs[] = {"CREATE", "DROP", "SHOW", "REPLAY", "CALL", "PAUSE", "RESUME",
                                      "REFRESH", "FIRE", "RETRY", "SET", "RESOLVE", "CHECK", "WAIT"};
  bool verb = false;
  for (const char* v : verbs) if (std::strcmp(a, v) == 0) { verb = true; break; }
  if (!verb) return false;
  if (std::strcmp(b, "THING") == 0) return true;
  if (std::strcmp(a, "CREATE") == 0 && std::strcmp(b, "OR") == 0) {   // CREATE OR REPLACE THING
    char c3[8], d4[8];
    return word(i, c3, sizeof c3) && std::strcmp(c3, "REPLACE") == 0 && word(i, d4, sizeof d4) && std::strcmp(d4, "THING") == 0;
  }
  return std::strcmp(a, "SHOW") == 0 && std::strcmp(b, "THINGS") == 0;
}

SqlQueryResult ThingEngine::show(const std::string& name) {
  SqlQueryResult res;
  res.is_result_set = true;
  if (name.empty()) {
    res.columns = {"thing", "things", "version", "reactions", "full_recomputes", "partitions", "links", "waiting_for", "declared", "state", "host"};
    const std::string host = host_name();
    for (const auto& kv : kinds_) {
      const Kind& k = kv.second;
      std::string links;
      for (const auto& d : k.deps_partition) links += (links.empty() ? "" : ", ") + d.substr(d.find('.') + 1) + "(" + k.part_col + ")";
      for (const auto& d : k.deps_full) links += (links.empty() ? "" : ", ") + d.substr(d.find('.') + 1);
      const size_t count = k.type == Kind::Resource ? k.res.size() : k.type == Kind::Claims ? k.orders.size()
                         : (k.type == Kind::Grain || k.type == Kind::Mirror) ? k.gthings.size() : k.things.size();
      if (k.type == Kind::Mirror) {
        const uint64_t now_ns = db_.kernel_.host().wall_ns();
        const long long in_s = k.next_pull_ns > now_ns ? (long long)((k.next_pull_ns - now_ns) / 1000000000ull) : 0;
        std::string w = (k.mirror_error.empty() ? std::string() : "UNREACHABLE: " + k.mirror_error + " (values as last seen " +
                         (k.mirror_ok_ns ? wall_text(k.mirror_ok_ns) : std::string("never")) + "); ") +
                        "the next pull in " + std::to_string(in_s) + " s (every " + std::to_string(k.every_s) + " s)";
        res.rows.push_back(SqlRow{{k.name, std::to_string(count), std::to_string(k.version), std::to_string(k.reactions), "0", "0",
                                   k.mirror_kind.empty() ? std::string("the fleet's machines")
                                                         : "mirror of " + k.mirror_kind + " on " + k.mirror_machine,
                                   w, wall_text(k.created_ns),
                                   k.paused ? std::string("paused") : k.mirror_error.empty() ? std::string("active") : std::string("stale"), host}});
        continue;
      }
      if (k.type == Kind::Resource) links = k.cap_table.substr(k.cap_table.find('.') + 1) + "(" + [&] {
        std::string c; for (const auto& kc : k.key_cols) c += (c.empty() ? "" : ", ") + kc; return c; }() + ")" +
        (k.claimed_by.empty() ? "" : "; claimed by " + k.claimed_by);
      if (k.type == Kind::Claims) links += "; claims " + k.resource;
      if (k.type == Kind::Event) {
        res.rows.push_back(SqlRow{{k.name, "1", std::to_string(k.version), "0", "0", "0",
                                   "writes " + [&] { std::string w; for (const auto& t : k.writes) w += (w.empty() ? "" : ", ") + t; return w; }(),
                                   k.state == "failed" ? "FAILED: " + k.fire_error + " (RETRY THING to arm it again)"
                                                       : (k.state == "firing" ? std::string("firing (resumes after a crash)") : "fires at " + k.at),
                                   wall_text(k.created_ns), k.paused ? std::string("paused") : k.state, host}});
        continue;
      }
      if (k.type == Kind::Grain) {
        links.clear();
        for (const auto& r : k.reads) {
          auto w = k.watch.find(r);
          links += (links.empty() ? "" : ", ") + r + (w == k.watch.end() ? "" : "(" + w->second + ")");
        }
      }
      for (const auto& p : k.part_defs) links += std::string(links.empty() ? "" : "; ") + "part " + p.name + ": " + p.key_table + "(" + p.key_col + ")";
      if (!k.do_sql.empty()) links += std::string(links.empty() ? "" : "; ") + "when " + k.when_expr + " do " + k.do_sql + " on " + k.do_target;
      res.rows.push_back(SqlRow{{k.name, std::to_string(count), std::to_string(k.version),
                                 std::to_string(k.reactions), std::to_string(k.full_recomputes),
                                 std::to_string(k.parts.size()), links, waiting_for(k), wall_text(k.created_ns),
                                 k.paused ? "paused" : "active", host}});
    }
  } else {
    auto it = kinds_.find(lower(name));
    if (it == kinds_.end()) return error_result("no Thing kind named " + name);
    const Kind& k = it->second;
    if (k.type == Kind::Resource) return show_resource(k);
    if (k.type == Kind::Claims) return show_claims(k);
    if (k.type == Kind::Grain || k.type == Kind::Mirror) return show_grain(k);
    res.columns.push_back(k.items[0].label);
    const bool labelled = k.row_mode && !k.out_labels.empty();
    if (labelled) for (const auto& l : k.out_labels) res.columns.push_back(l);
    else for (size_t i = 1; i < k.items.size(); ++i) res.columns.push_back(k.items[i].label);
    if (!k.source_by.empty()) res.columns.push_back("conflicts");
    for (const char* c : {"version", "partitions", "updated_at", "last_change", "waiting_for", "may_read"})
      res.columns.push_back(c);
    std::string reads;
    for (const auto& r : k.reads) reads += (reads.empty() ? "" : ", ") + r;
    for (const auto& r : k.view_reads) reads += ", " + r;
    const std::string wf = waiting_for(k);
    for (const auto& kv : k.things) {
      const Instance& in = kv.second;
      SqlRow row;
      row.values.push_back(in.key_null ? sql_null() : in.key);
      if (labelled) for (size_t v = 0; v < k.out_labels.size(); ++v) row.values.push_back(v < in.row.size() ? in.row[v] : sql_null());
      else for (size_t i = 1; i < k.items.size(); ++i) row.values.push_back(value_text(k, in, i));
      if (!k.source_by.empty()) {
        // each conflicted field with every side's value, and the choice when one was made
        std::string c;
        for (const std::string& f : in.conflicts) {
          size_t v = 0;
          while (v < in.row.size() && value_label(k, v) != f) ++v;
          std::string both;
          for (const auto& sd : in.sides) both += (both.empty() ? "" : ", ") + sd.first + "=" + (v < sd.second.size() && !sql_is_null(sd.second[v]) ? sd.second[v] : std::string("NULL"));
          auto pk = in.picks.find(f);
          auto rl = in.ruled.find(f);
          c += (c.empty() ? "" : "; ") + f + ": " + both + (pk != in.picks.end() ? " (took " + pk->second + ")"
                                                            : rl != in.ruled.end() ? " (took " + rl->second + " by rule)" : "");
        }
        row.values.push_back(c.empty() ? sql_null() : c);
      }
      row.values.push_back(std::to_string(in.version));
      row.values.push_back(std::to_string(in.parts));
      row.values.push_back(wall_text(in.updated_ns));
      row.values.push_back(in.history.empty() ? sql_null() : in.history.back().cause);
      row.values.push_back(wf);
      row.values.push_back(reads);
      res.rows.push_back(std::move(row));
    }
  }
  if (!sql_wire_results()) res.formatted_table = db_.format_result_table(res.columns, res.rows);
  return res;
}

namespace {
// A statement's words: quoted ones whole (and marked quoted), commas and ';' as spaces.
struct Word { std::string text; bool quoted = false; };
std::vector<Word> thing_words(const std::string& sql) {
  std::vector<Word> w;
  size_t i = 0;
  auto space = [&](char c) { return std::isspace((unsigned char)c) || c == ';' || c == ','; };
  while (i < sql.size()) {
    while (i < sql.size() && space(sql[i])) ++i;
    if (i >= sql.size()) break;
    if (sql[i] == '\'' || sql[i] == '"' || sql[i] == '`') {
      const char q = sql[i++];
      std::string t;
      while (i < sql.size() && sql[i] != q) t += sql[i++];
      ++i;
      w.push_back(Word{t, true});
      continue;
    }
    std::string t;
    while (i < sql.size() && !space(sql[i])) t += sql[i++];
    w.push_back(Word{t, false});
  }
  return w;
}

// "email={crm: a; erp: b} (conflict)" is how display() writes a conflicted field: is `label` one in `shown`?
bool shows_conflict(const std::string& shown, const std::string& label) {
  return shown.rfind(label + "={", 0) == 0 || shown.find(" | " + label + "={") != std::string::npos;
}
}  // namespace

// RESOLVE THING kind 'key' field TAKE 'source' | UNRESOLVE           one Thing's conflict
// RESOLVE THING kind * field|* TAKE 'source' | UNRESOLVE              every Thing's
// RESOLVE THING kind * field|* PREFER 'source'[, ...] | NOPREFER      a standing rule
SqlQueryResult ThingEngine::resolve(const std::string& sql) {
  const char* usage =
      "RESOLVE THING <kind> '<key>' <field> TAKE '<source>' | UNRESOLVE, or for every Thing: "
      "RESOLVE THING <kind> * <field>|* TAKE '<source>' | UNRESOLVE | PREFER '<source>'[, ...] | NOPREFER";
  const std::vector<Word> w = thing_words(sql);
  if (w.size() < 6 || upper(w[1].text) != "THING") return error_result(usage);
  const std::string verb = upper(w[5].text);
  const bool all = w[3].text == "*" && !w[3].quoted;
  const bool take = verb == "TAKE" && w.size() == 7, unresolve = verb == "UNRESOLVE" && w.size() == 6;
  const bool prefer = verb == "PREFER" && w.size() >= 7, noprefer = verb == "NOPREFER" && w.size() == 6;
  if (!(take || unresolve || ((prefer || noprefer) && all))) return error_result(usage);
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  settle_all_locked();
  auto it = kinds_.find(lower(w[2].text));
  if (it == kinds_.end()) return error_result("no Thing kind named " + w[2].text);
  Kind& k = it->second;
  if (k.source_by.empty()) return error_result(k.name + " has no SOURCE BY: its rows never conflict");
  const std::string want_field = w[4].text;
  const bool any_field = want_field == "*" && !w[4].quoted;
  // the field named, as the Thing labels it ("" when it has no such value column)
  std::string field;
  if (!any_field) {
    const int src = source_index(k);
    const size_t n = !k.out_labels.empty() ? k.out_labels.size() : (k.items.empty() ? 0 : k.items.size() - 1);
    for (size_t v = 0; v < n; ++v)
      if ((int)v != src && lower(value_label(k, v)) == lower(want_field)) field = value_label(k, v);
  }
  SqlQueryResult res;
  res.is_result_set = true;
  auto value_of = [&](const Instance& in, const std::string& f) {
    size_t v = 0;
    while (v < in.row.size() && value_label(k, v) != f) ++v;
    return v < in.row.size() ? in.row[v] : sql_null();
  };

  if (!all) {
    if (any_field) return error_result("RESOLVE THING one Thing names one field; * is for every Thing (RESOLVE THING " + k.name + " * ...)");
    const std::string ik = upper(w[3].text) == "NULL" && !w[3].quoted ? std::string(kNullKey) : w[3].text;
    auto ti = k.things.find(ik);
    if (ti == k.things.end()) return error_result("no " + k.name + " Thing with key " + w[3].text);
    Instance& in = ti->second;
    std::string f;
    for (const std::string& c : in.conflicts) if (lower(c) == lower(want_field)) f = c;
    if (f.empty()) return error_result(want_field + " is not in conflict on " + k.name + " " + w[3].text + " (its sources agree, or there is no such field)");
    if (take) {
      bool present = false;
      for (const auto& sd : in.sides) present = present || sd.first == w[6].text;
      if (!present) {
        std::string have;
        for (const auto& sd : in.sides) have += (have.empty() ? "" : ", ") + sd.first;
        return error_result(w[6].text + " does not report " + k.name + " " + w[3].text + " (its sources: " + have + ")");
      }
    }
    std::map<std::string, std::string> before{{ik, display(k, in)}};
    if (take) in.picks[f] = w[6].text;
    else in.picks.erase(f);
    merge_sides(k, in);
    finish_changes(k, before, take ? "resolved " + f + ": took " + w[6].text : "unresolved " + f);
    dirty_ = true;
    res.columns = {"thing", "key", "field", "value", "conflicts_left"};
    res.rows.push_back(SqlRow{{k.name, w[3].text, f, value_of(in, f),
                               std::to_string(in.conflicts.size() - in.picks.size() - in.ruled.size())}});
    if (!sql_wire_results()) res.formatted_table = db_.format_result_table(res.columns, res.rows);
    return res;
  }

  if (!any_field && field.empty()) return error_result(k.name + " has no field " + want_field + " that can conflict");
  const std::string fname = any_field ? std::string("*") : field;
  std::map<std::string, std::string> before;
  for (auto& kv : k.things) before[kv.first] = display(k, kv.second);
  std::string cause;
  res.columns = {"thing", "key", "field", "value", "result"};
  if (take || unresolve) {
    const std::string src = take ? w[6].text : std::string();
    for (auto& kv : k.things) {
      Instance& in = kv.second;
      for (const std::string& f : in.conflicts) {
        if (!any_field && f != field) continue;
        if (unresolve) {
          if (in.picks.erase(f)) res.rows.push_back(SqlRow{{k.name, in.key, f, sql_null(), "unresolved"}});
          continue;
        }
        bool present = false;
        for (const auto& sd : in.sides) present = present || sd.first == src;
        if (present) in.picks[f] = src;
        res.rows.push_back(SqlRow{{k.name, in.key, f, sql_null(), present ? "took " + src : src + " does not report it"}});
      }
      merge_sides(k, in);
    }
    cause = take ? "resolved " + fname + " on every Thing: took " + src : "unresolved " + fname + " on every Thing";
  } else {
    // the rule replaces this field's rule; it settles every conflict on the field without a pick by hand, now and
    // whenever one arises
    for (size_t i = 0; i < k.prefer.size(); ++i)
      if (lower(k.prefer[i].first) == lower(fname)) { k.prefer.erase(k.prefer.begin() + (long)i); break; }
    std::string order;
    if (prefer) {
      std::vector<std::string> srcs;
      for (size_t i = 6; i < w.size(); ++i) { srcs.push_back(w[i].text); order += (order.empty() ? "" : ", ") + w[i].text; }
      k.prefer.emplace_back(fname, std::move(srcs));
    }
    for (auto& kv : k.things) {
      Instance& in = kv.second;
      merge_sides(k, in);
      for (const std::string& f : in.conflicts) {
        if (!any_field && f != field) continue;
        auto rl = in.ruled.find(f);
        auto pk = in.picks.find(f);
        res.rows.push_back(SqlRow{{k.name, in.key, f, value_of(in, f),
                                   pk != in.picks.end() ? "took " + pk->second + " by hand (the rule yields to it)"
                                   : rl != in.ruled.end() ? "took " + rl->second + " by rule"
                                   : prefer ? "no preferred source reports it" : "in conflict"}});
      }
    }
    cause = prefer ? "rule: " + fname + " prefers " + order : "rule dropped: " + fname;
  }
  for (auto& r : res.rows) {   // the value each Thing now has
    if (!sql_is_null(r.values[3])) continue;
    auto ti = k.things.find(r.values[1]);
    if (ti != k.things.end()) r.values[3] = value_of(ti->second, r.values[2]);
  }
  finish_changes(k, before, cause);
  dirty_ = true;
  if (!sql_wire_results()) res.formatted_table = db_.format_result_table(res.columns, res.rows);
  return res;
}

// SHOW THING CONFLICT GROUPS kind [BY CHANGE]: the conflicts that share a cause, one row per group. By field and by
// which sources side together ("billing,crm | erp": the same disagreement on many Things), or with BY CHANGE by the
// change that raised them (the history entry where the field became a conflict).
SqlQueryResult ThingEngine::conflict_groups(const std::string& sql) {
  const std::vector<Word> w = thing_words(sql);
  // SHOW THING CONFLICT GROUPS <kind> [BY CHANGE]
  if (w.size() < 5 || upper(w[3].text) != "GROUPS") return error_result("SHOW THING CONFLICT GROUPS <kind> [BY CHANGE]");
  const bool by_change = w.size() >= 7 && upper(w[5].text) == "BY" && upper(w[6].text) == "CHANGE";
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  settle_all_locked();
  auto it = kinds_.find(lower(w[4].text));
  if (it == kinds_.end()) return error_result("no Thing kind named " + w[4].text);
  const Kind& k = it->second;
  if (k.source_by.empty()) return error_result(k.name + " has no SOURCE BY: its rows never conflict");
  struct Group { std::set<std::string> fields, causes, camps; std::vector<std::string> keys; size_t n = 0, open = 0, hand = 0, rule = 0; };
  std::map<std::string, Group> groups;
  std::vector<std::string> order;
  for (const auto& kv : k.things) {
    const Instance& in = kv.second;
    for (const std::string& f : in.conflicts) {
      size_t v = 0;
      while (v < in.row.size() && value_label(k, v) != f) ++v;
      // the sides that agree, together: value -> sources
      std::map<std::string, std::vector<std::string>> by_value;
      for (const auto& sd : in.sides) by_value[v < sd.second.size() ? sd.second[v] : sql_null()].push_back(sd.first);
      std::vector<std::string> camps;
      for (auto& bv : by_value) {
        std::sort(bv.second.begin(), bv.second.end());
        std::string c;
        for (const auto& s : bv.second) c += (c.empty() ? "" : ",") + s;
        camps.push_back(c);
      }
      std::sort(camps.begin(), camps.end());
      std::string camp;
      for (const auto& c : camps) camp += (camp.empty() ? "" : " | ") + c;
      // the change that made it a conflict: the newest history entry where it became one
      std::string cause = "(before history)";
      for (size_t h = in.history.size(); h-- > 0;)
        if (shows_conflict(in.history[h].after, f) && !shows_conflict(in.history[h].before, f)) { cause = in.history[h].cause; break; }
      const std::string id = by_change ? cause : f + "\x1f" + camp;
      auto gi = groups.find(id);
      if (gi == groups.end()) { gi = groups.emplace(id, Group{}).first; order.push_back(id); }
      Group& g = gi->second;
      g.fields.insert(f);
      g.causes.insert(cause);
      g.camps.insert(camp);
      g.n++;
      if (in.picks.count(f)) g.hand++;
      else if (in.ruled.count(f)) g.rule++;
      else g.open++;
      if (std::find(g.keys.begin(), g.keys.end(), in.key) == g.keys.end()) g.keys.push_back(in.key);
    }
  }
  auto list = [](const auto& xs, size_t cap) {
    std::string s;
    size_t i = 0;
    for (const auto& x : xs) {
      if (i++ == cap) { s += ", ... (" + std::to_string(xs.size()) + ")"; break; }
      s += (s.empty() ? "" : ", ") + x;
    }
    return s;
  };
  SqlQueryResult res;
  res.is_result_set = true;
  if (by_change) res.columns = {"raised_by", "conflicts", "open", "by_hand", "by_rule", "fields", "keys"};
  else res.columns = {"field", "sides", "conflicts", "open", "by_hand", "by_rule", "keys", "raised_by"};
  std::stable_sort(order.begin(), order.end(), [&](const std::string& a, const std::string& b) { return groups[a].n > groups[b].n; });
  for (const auto& id : order) {
    const Group& g = groups[id];
    if (by_change)
      res.rows.push_back(SqlRow{{id, std::to_string(g.n), std::to_string(g.open), std::to_string(g.hand), std::to_string(g.rule),
                                 list(g.fields, 8), list(g.keys, 10)}});
    else
      res.rows.push_back(SqlRow{{*g.fields.begin(), *g.camps.begin(), std::to_string(g.n), std::to_string(g.open), std::to_string(g.hand),
                                 std::to_string(g.rule), list(g.keys, 10), list(g.causes, 3)}});
  }
  if (!sql_wire_results()) res.formatted_table = db_.format_result_table(res.columns, res.rows);
  return res;
}

// CHECK THING kind ADD UNIQUE field | ADD TOGETHER f1, f2[, ...] | DROP ... : constraints over the merged values.
// CHECK THING kind [APPLY]: are the picks (by hand and by rule) consistent with them, and is there a choice for every
// open conflict on a constrained field that keeps them so? A SAT problem: a variable per (Thing, field, side), one
// side each, the picks as units, UNIQUE as "not both" between Things that would share a value, TOGETHER as "the
// side taken for one field brings that source's value for the other". SAT: a choice for each open conflict (model
// checked against every clause; APPLY takes it). UNSAT: the minimal set of picks, rules and constraints that clash.
SqlQueryResult ThingEngine::check(const std::string& sql) {
  const char* usage = "CHECK THING <kind> [APPLY] | CHECK THING <kind> ADD|DROP UNIQUE <field> | ADD|DROP TOGETHER <field>, <field>[, ...]";
  const std::vector<Word> w = thing_words(sql);
  if (w.size() < 3 || upper(w[1].text) != "THING") return error_result(usage);
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  settle_all_locked();
  auto it = kinds_.find(lower(w[2].text));
  if (it == kinds_.end()) return error_result("no Thing kind named " + w[2].text);
  Kind& k = it->second;
  if (k.source_by.empty()) return error_result(k.name + " has no SOURCE BY: its rows never conflict");
  const int src_col = source_index(k);
  const size_t width = !k.out_labels.empty() ? k.out_labels.size() : (k.items.empty() ? 0 : k.items.size() - 1);
  auto label_of = [&](const std::string& name) {
    for (size_t v = 0; v < width; ++v)
      if ((int)v != src_col && lower(value_label(k, v)) == lower(name)) return value_label(k, v);
    return std::string();
  };
  auto describe = [](const Kind::Constraint& c) {
    std::string f;
    for (const auto& x : c.fields) f += (f.empty() ? "" : ", ") + x;
    return std::string(c.unique ? "UNIQUE " : "TOGETHER ") + f;
  };
  SqlQueryResult res;
  res.is_result_set = true;
  const std::string op = w.size() > 3 ? upper(w[3].text) : std::string();
  if (op == "ADD" || op == "DROP") {
    if (w.size() < 6) return error_result(usage);
    Kind::Constraint c;
    c.unique = upper(w[4].text) == "UNIQUE";
    if (!c.unique && upper(w[4].text) != "TOGETHER") return error_result(usage);
    for (size_t i = 5; i < w.size(); ++i) {
      const std::string l = label_of(w[i].text);
      if (l.empty()) return error_result(k.name + " has no field " + w[i].text + " (the source column " + k.source_by + " is not one)");
      c.fields.push_back(l);
    }
    if (c.unique ? c.fields.size() != 1 : c.fields.size() < 2) return error_result(c.unique ? "UNIQUE takes one field" : "TOGETHER takes two fields or more");
    bool found = false;
    for (size_t i = 0; i < k.constraints.size(); ++i)
      if (describe(k.constraints[i]) == describe(c)) {
        found = true;
        if (op == "DROP") { k.constraints.erase(k.constraints.begin() + (long)i); break; }
      }
    if (op == "DROP" && !found) return error_result(k.name + " has no constraint " + describe(c));
    if (op == "ADD" && !found) k.constraints.push_back(c);
    dirty_ = true;
    res.columns = {"thing", "constraint"};
    for (const auto& x : k.constraints) res.rows.push_back(SqlRow{{k.name, describe(x)}});
    if (!sql_wire_results()) res.formatted_table = db_.format_result_table(res.columns, res.rows);
    return res;
  }
  const bool apply = op == "APPLY";
  if (!op.empty() && !apply) return error_result(usage);

  // ---- the problem ----
  std::set<std::string> constrained;
  for (const auto& c : k.constraints) constrained.insert(c.fields.begin(), c.fields.end());
  std::vector<std::vector<int>> cls;
  std::vector<std::string> why;
  int vars = 0;
  auto add = [&](std::vector<int> c, const std::string& reason) { cls.push_back(std::move(c)); why.push_back(reason); };
  // a field of a Thing: its fixed value, or a variable per side
  struct Slot { bool open = false; std::string value; std::vector<std::pair<std::string, int>> side_var; std::vector<std::string> side_val; };
  std::map<std::string, std::map<std::string, Slot>> slots;   // thing key -> field -> slot
  auto keyname = [](const Instance& in) { return in.key_null ? std::string("NULL") : in.key; };
  for (auto& kv : k.things) {
    Instance& in = kv.second;
    for (const std::string& f : constrained) {
      size_t v = 0;
      while (v < in.row.size() && value_label(k, v) != f) ++v;
      Slot s;
      if (std::find(in.conflicts.begin(), in.conflicts.end(), f) == in.conflicts.end()) {
        s.value = v < in.row.size() ? in.row[v] : sql_null();
      } else {
        s.open = true;
        std::vector<int> one;
        for (const auto& sd : in.sides) {
          s.side_var.emplace_back(sd.first, ++vars);
          s.side_val.push_back(v < sd.second.size() ? sd.second[v] : sql_null());
          one.push_back(vars);
        }
        const std::string who = k.name + " " + keyname(in) + " " + f;
        add(one, who + " takes one side");
        for (size_t a = 0; a < one.size(); ++a)
          for (size_t b = a + 1; b < one.size(); ++b) add({-one[a], -one[b]}, who + " takes one side");
        auto pk = in.picks.find(f);
        auto rl = in.ruled.find(f);
        for (const auto& sv : s.side_var) {
          if (pk != in.picks.end() && pk->second == sv.first) add({sv.second}, "pick: " + who + " took " + sv.first);
          else if (pk == in.picks.end() && rl != in.ruled.end() && rl->second == sv.first)
            add({sv.second}, "rule: " + f + " prefers " + sv.first + " (" + k.name + " " + keyname(in) + ")");
        }
      }
      slots[kv.first][f] = std::move(s);
    }
  }
  for (const auto& c : k.constraints) {
    const std::string cname = describe(c);
    if (c.unique) {
      const std::string& f = c.fields[0];
      // value -> (thing, literal; 0 = the Thing has the value whatever is chosen)
      std::map<std::string, std::vector<std::pair<std::string, int>>> holders;
      for (const auto& ts : slots) {
        const Slot& s = ts.second.at(f);
        if (!s.open) { if (!sql_is_null(s.value)) holders[s.value].emplace_back(ts.first, 0); continue; }
        for (size_t i = 0; i < s.side_var.size(); ++i)
          if (!sql_is_null(s.side_val[i])) holders[s.side_val[i]].emplace_back(ts.first, s.side_var[i].second);
      }
      for (const auto& h : holders)
        for (size_t a = 0; a < h.second.size(); ++a)
          for (size_t b = a + 1; b < h.second.size(); ++b) {
            const auto& x = h.second[a];
            const auto& y = h.second[b];
            if (x.first == y.first) continue;
            auto kn = [&](const std::string& ik) { auto t = k.things.find(ik); return t == k.things.end() ? ik : keyname(t->second); };
            const std::string r = cname + ": " + k.name + " " + kn(x.first) + " and " + kn(y.first) + " cannot both have " + f + " " + h.first;
            std::vector<int> cl;
            if (x.second) cl.push_back(-x.second);
            if (y.second) cl.push_back(-y.second);
            if (cl.empty()) { ++vars; add({vars}, r + " (they already do)"); add({-vars}, r + " (they already do)"); }
            else add(cl, r);
          }
    } else {
      for (const auto& ts : slots) {
        auto kn = k.things.find(ts.first);
        const std::string who = k.name + " " + (kn == k.things.end() ? ts.first : keyname(kn->second));
        for (const auto& fi : c.fields)
          for (const auto& fj : c.fields) {
            if (fi == fj) continue;
            const Slot& a = ts.second.at(fi);
            const Slot& b = ts.second.at(fj);
            if (!a.open || !b.open) continue;   // one value already: any side of the other agrees with it
            for (size_t i = 0; i < a.side_var.size(); ++i) {
              // taking source s for fi brings s's value of fj: some side of fj with that value is taken
              std::string want;
              for (size_t j = 0; j < b.side_var.size(); ++j) if (b.side_var[j].first == a.side_var[i].first) want = b.side_val[j];
              std::vector<int> cl{-a.side_var[i].second};
              for (size_t j = 0; j < b.side_var.size(); ++j) if (b.side_val[j] == want) cl.push_back(b.side_var[j].second);
              add(cl, cname + ": " + who + " takes " + fi + " and " + fj + " from one source");
            }
          }
      }
    }
  }

  // ---- solved, and checked ----
  const SatAnswer ans = solve_verified(vars, cls, 200000);
  res.columns = {"status", "thing", "key", "field", "source", "value", "why"};
  const std::string status = ans.status == 10 ? "CONSISTENT" : ans.status == 20 ? "CLASH" : "UNKNOWN";
  if (k.constraints.empty()) {
    res.rows.push_back(SqlRow{{"CONSISTENT", k.name, sql_null(), sql_null(), sql_null(), sql_null(),
                               "no constraints (CHECK THING " + k.name + " ADD UNIQUE <field> | ADD TOGETHER <field>, <field>)"}});
  } else if (ans.status == 10) {
    std::vector<bool> val((size_t)vars + 1, false);
    for (int l : ans.model) if (l > 0 && l <= vars) val[(size_t)l] = true;
    std::map<std::string, std::string> before;
    size_t open_rows = 0;
    for (const auto& ts : slots) {
      auto ti = k.things.find(ts.first);
      if (ti == k.things.end()) continue;
      Instance& in = ti->second;
      for (const auto& fs : ts.second) {
        const Slot& s = fs.second;
        if (!s.open || in.picks.count(fs.first) || in.ruled.count(fs.first)) continue;
        for (size_t i = 0; i < s.side_var.size(); ++i) {
          if (!val[(size_t)s.side_var[i].second]) continue;
          res.rows.push_back(SqlRow{{status, k.name, keyname(in), fs.first, s.side_var[i].first,
                                     s.side_val[i], apply ? "taken: consistent with every constraint" : "a choice consistent with every constraint"}});
          ++open_rows;
          if (apply) {
            if (!before.count(ts.first)) before[ts.first] = display(k, in);
            in.picks[fs.first] = s.side_var[i].first;
          }
          break;
        }
      }
    }
    if (apply) {
      for (auto& b : before) merge_sides(k, k.things[b.first]);
      std::string cs;
      for (const auto& c : k.constraints) cs += (cs.empty() ? "" : "; ") + describe(c);
      finish_changes(k, before, "check: took a choice consistent with " + cs);
      dirty_ = true;
    }
    res.rows.insert(res.rows.begin(), SqlRow{{status, k.name, sql_null(), sql_null(), sql_null(), sql_null(),
                                               std::to_string(open_rows) + " open conflict(s); the picks and rules hold together (" + ans.evidence + ")"}});
  } else if (ans.status == 20) {
    std::set<std::string> seen;
    std::vector<std::string> reasons;
    for (size_t c : ans.core) if (c < why.size() && seen.insert(why[c]).second) reasons.push_back(why[c]);
    res.rows.push_back(SqlRow{{status, k.name, sql_null(), sql_null(), sql_null(), sql_null(),
                               "these cannot all hold (" + ans.core_evidence + "):"}});
    for (const auto& r : reasons) res.rows.push_back(SqlRow{{status, k.name, sql_null(), sql_null(), sql_null(), sql_null(), r}});
  } else {
    res.rows.push_back(SqlRow{{status, k.name, sql_null(), sql_null(), sql_null(), sql_null(), "the solver stopped first: " + ans.evidence}});
  }
  if (!sql_wire_results()) res.formatted_table = db_.format_result_table(res.columns, res.rows);
  return res;
}

// SHOW THING CONFLICTS kind ['key']: one row per side of every conflicted field
SqlQueryResult ThingEngine::show_conflicts(const std::string& name, const std::string& key) {
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  settle_all_locked();
  auto it = kinds_.find(lower(name));
  if (it == kinds_.end()) return error_result("no Thing kind named " + name);
  const Kind& k = it->second;
  if (k.source_by.empty()) return error_result(k.name + " has no SOURCE BY: its rows never conflict");
  SqlQueryResult res;
  res.is_result_set = true;
  res.columns = {"key", "field", "source", "value", "taken", "how"};
  for (const auto& kv : k.things) {
    const Instance& in = kv.second;
    if (!key.empty() && in.key != key) continue;
    for (const std::string& f : in.conflicts) {
      size_t v = 0;
      while (v < in.row.size() && value_label(k, v) != f) ++v;
      auto pk = in.picks.find(f);
      auto rl = in.ruled.find(f);
      for (const auto& sd : in.sides) {
        const bool hand = pk != in.picks.end() && pk->second == sd.first, rule = rl != in.ruled.end() && rl->second == sd.first;
        res.rows.push_back(SqlRow{{in.key_null ? sql_null() : in.key, f, sd.first, v < sd.second.size() ? sd.second[v] : sql_null(),
                                   hand || rule ? "1" : "0", hand ? "hand" : rule ? "rule" : sql_null()}});
      }
    }
  }
  if (!sql_wire_results()) res.formatted_table = db_.format_result_table(res.columns, res.rows);
  return res;
}

// SHOW THING name JSON [SINCE n]: one row per Thing - key, stamp, state and
// the whole Thing as JSON (the head's columns, then each part as a list of
// rows); with SINCE, only the Things whose stamp moved past n, and the keys
// that left since (state "gone", json NULL). SHOW THING name CLOCK: the
// stamp a client holds after reading everything.
static std::string thing_json_str(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
  }
  return o + "\"";
}

SqlQueryResult ThingEngine::show_json(const Kind& k, uint64_t since, bool clock_only) const {
  SqlQueryResult res;
  res.is_result_set = true;
  if (clock_only) {
    res.columns = {"clock", "things"};
    res.rows.push_back(SqlRow{{std::to_string(k.version), std::to_string(k.things.size())}});
    return res;
  }
  res.columns = {"key", "stamp", "state", "json"};
  std::vector<std::string> labels;
  if (k.row_mode && !k.out_labels.empty()) labels = k.out_labels;
  else for (size_t i = 1; i < k.items.size(); ++i) labels.push_back(k.items[i].label);
  auto val = [](const std::string& v) { return sql_is_null(v) ? std::string("null") : thing_json_str(v); };
  // A label a `*` expansion repeats (so.orderno, salesorderdetails.orderno ..) is written once, as the
  // last occurrence - the value any JSON reader keeps for a repeated key - so the document is not
  // paid for three times. The key's label is never repeated after it.
  std::vector<bool> last_of(labels.size(), true);
  bool key_first = true;   // the key opens the document unless a column of its name follows (that one wins)
  for (size_t v = 0; v < labels.size(); ++v) {
    if (lower(labels[v]) == lower(k.items[0].label)) key_first = false;
    for (size_t w = v + 1; w < labels.size(); ++w) if (lower(labels[w]) == lower(labels[v])) { last_of[v] = false; break; }
  }
  res.rows.reserve(k.things.size());
  for (const auto& kv : k.things) {
    const Instance& in = kv.second;
    if (since && in.stamp <= since) continue;
    if (in.json_valid && in.json_stamp == in.stamp && in.json_version == in.version) {
      res.rows.push_back(SqlRow{{in.key_null ? sql_null() : in.key, std::to_string(in.stamp), "live", in.json_doc}});
      continue;
    }
    std::string j;
    j.reserve(16384);
    j += "{";
    if (key_first) j += thing_json_str(k.items[0].label) + ":" + (in.key_null ? std::string("null") : thing_json_str(in.key));
    for (size_t v = 0; v < labels.size(); ++v) {
      if (!last_of[v]) continue;
      std::string text = k.row_mode ? (v < in.row.size() ? in.row[v] : sql_null()) : value_text(k, in, v + 1);
      j += (j.size() > 1 ? "," : "") + thing_json_str(labels[v]) + ":" + val(text);
    }
    for (const auto& p : k.part_defs) {
      j += (j.size() > 1 ? "," : "") + thing_json_str(p.name) + ":[";
      auto pr = in.part_rows.find(p.name);
      if (pr != in.part_rows.end()) {
        bool first_row = true;
        for (const auto& row : pr->second) {
          j += first_row ? "{" : ",{";
          first_row = false;
          for (size_t c = 0; c < row.size(); ++c)
            j += (c ? "," : "") + thing_json_str(c < p.cols.size() ? p.cols[c] : std::to_string(c)) + ":" + val(row[c]);
          j += "}";
        }
      }
      j += "]";
    }
    if (!k.source_by.empty()) {
      // _conflicts: [{field, sides: {source: value}, took: source|null}]
      j += (j.size() > 1 ? "," : "") + std::string("\"_conflicts\":[");
      for (size_t c = 0; c < in.conflicts.size(); ++c) {
        const std::string& f = in.conflicts[c];
        size_t v = 0;
        while (v < in.row.size() && value_label(k, v) != f) ++v;
        j += (c ? ",{" : "{") + std::string("\"field\":") + thing_json_str(f) + ",\"sides\":{";
        for (size_t i = 0; i < in.sides.size(); ++i)
          j += (i ? "," : "") + thing_json_str(in.sides[i].first) + ":" + val(v < in.sides[i].second.size() ? in.sides[i].second[v] : sql_null());
        auto pk = in.picks.find(f);
        auto rl = in.ruled.find(f);
        j += "},\"took\":" + (pk != in.picks.end() ? thing_json_str(pk->second) : rl != in.ruled.end() ? thing_json_str(rl->second) : std::string("null")) +
             (pk == in.picks.end() && rl != in.ruled.end() ? ",\"by_rule\":true}" : "}");
      }
      j += "]";
    }
    j += (j.size() > 1 ? "," : "") + std::string("\"_version\":") + std::to_string(in.version) + ",\"_updated\":" + thing_json_str(wall_text(in.updated_ns)) + "}";
    in.json_doc = j;
    in.json_stamp = in.stamp;
    in.json_version = in.version;
    in.json_valid = true;
    SqlRow row;
    row.values.reserve(4);
    row.values.push_back(in.key_null ? sql_null() : in.key);
    row.values.push_back(std::to_string(in.stamp));
    row.values.push_back("live");
    row.values.push_back(std::move(j));
    res.rows.push_back(std::move(row));
  }
  if (since)
    for (const auto& d : k.departed)
      if (d.second > since && !k.things.count(d.first))
        res.rows.push_back(SqlRow{{d.first == kNullKey ? sql_null() : d.first, std::to_string(d.second), "gone", sql_null()}});
  return res;
}

SqlQueryResult ThingEngine::show_history(const std::string& name, const std::string& key) {
  auto it = kinds_.find(lower(name));
  if (it == kinds_.end()) return error_result("no Thing kind named " + name);
  const std::vector<History>* hist = nullptr;
  if (it->second.type == Kind::Resource) {
    // 'v1,v2' - the key columns' values in order.
    std::vector<std::string> vals;
    size_t a = 0;
    for (size_t i = 0; i <= key.size(); ++i)
      if (i == key.size() || key[i] == ',') { vals.push_back(trim(key.substr(a, i - a))); a = i + 1; }
    auto ri = it->second.res.find(reskey_of(vals));
    if (ri == it->second.res.end()) return error_result("Thing " + name + " has no instance " + key + " (held only while claimed)");
    hist = &ri->second.history;
  } else if (it->second.type == Kind::Claims) {
    auto oi = it->second.orders.find(canon(key));
    if (oi == it->second.orders.end()) return error_result("Thing " + name + " has no instance " + key + " (closed or never open)");
    hist = &oi->second.history;
  } else if (it->second.type == Kind::Grain || it->second.type == Kind::Mirror) {
    auto gi = it->second.gthings.find(sql_collate_key(key));
    if (gi == it->second.gthings.end()) return error_result("Thing " + name + " has no instance " + key);
    hist = &gi->second.history;
  } else {
    auto ti = it->second.things.find(key);
    if (ti == it->second.things.end()) return error_result("Thing " + name + " has no instance " + key);
    hist = &ti->second.history;
  }
  SqlQueryResult res;
  res.is_result_set = true;
  res.columns = {"at", "cause", "before", "after"};
  for (const History& h : *hist)
    res.rows.push_back(SqlRow{{wall_text(h.wall_ns), h.cause, h.before, h.after}});
  if (!sql_wire_results()) res.formatted_table = db_.format_result_table(res.columns, res.rows);
  return res;
}

// The proof harness: take a Kind's partitions out of its partition-linked
// tables and put them back day by day, through the executor's own INSERTs -
// the statements the binlog consumer applies. Only in a process started with
// AIOS_THING_REPLAY=1: it deletes rows, so it never runs on a live mirror.
SqlQueryResult ThingEngine::replay(const std::string& name, const std::string& action, const std::string& arg) {
  const char* gate = std::getenv("AIOS_THING_REPLAY");
  if (!gate || std::string(gate) != "1")
    return error_result("REPLAY THING only runs in a process started with AIOS_THING_REPLAY=1 (it deletes and re-inserts rows)");
  std::unique_lock<aios::hal::Mutex> lk(mu_);
  auto it = kinds_.find(lower(name));
  if (it == kinds_.end()) return error_result("no Thing kind named " + name);
  Kind& k = it->second;
  const std::string file = things_dir() + "/" + k.name + ".replay";
  SqlQueryResult res;
  res.is_result_set = true;

  if (action == "RESET") {
    if (arg.empty()) return error_result("REPLAY THING <name> RESET BY <day expression>");
    if (!k.pending.empty() || k.pending_full) settle_kind(k);
    // Each partition's day, by the given expression over the Thing's source.
    std::string day_sql = "SELECT " + k.part_col + ", MIN(" + arg + ") FROM " + k.source +
                          (k.where_text.empty() ? "" : " WHERE (" + k.where_text + ")") + " GROUP BY " + k.part_col;
    SqlQueryResult dr = db_.execute_local_query(day_sql, k.db);
    if (!dr.success) return error_result("day query: " + dr.error_message);
    std::map<std::string, std::vector<std::string>> by_day;   // day -> partitions
    for (const SqlRow& r : dr.rows)
      if (r.values.size() >= 2 && k.parts.count(canon(r.values[0]))) by_day[r.values[1]].push_back(canon(r.values[0]));
    std::vector<std::string> all;
    for (auto& d : by_day) {
      std::sort(d.second.begin(), d.second.end(), [](const std::string& a, const std::string& b) {
        return a.size() != b.size() ? a.size() < b.size() : a < b;
      });
      for (const auto& p : d.second) all.push_back(p);
    }
    // Capture every row of those partitions from each partition-linked table.
    std::map<std::string, std::vector<std::string>> stmts_by_part;
    size_t rows_captured = 0;
    std::vector<std::string> tables(k.deps_partition.begin(), k.deps_partition.end());
    lk.unlock();
    for (const std::string& tk : tables) {
      const size_t dot = tk.find('.');
      const SqlTable* t = db_.resolve_table(tk.substr(dot + 1), tk.substr(0, dot));
      if (!t) continue;
      const std::string tname = t->name, tdb = t->database;
      for (size_t at = 0; at < all.size(); at += kPartitionChunk) {
        std::string list;
        for (size_t i = at; i < std::min(all.size(), at + kPartitionChunk); ++i)
          list += (i > at ? "," : "") + (all_digits(all[i]) ? all[i] : sql_lit(all[i]));
        SqlQueryResult rr = db_.execute_local_query("SELECT * FROM " + ident(tname) + " WHERE " + ident(k.part_col) +
                                                    " IN (" + list + ")", tdb);
        if (!rr.success) return error_result("capture " + tname + ": " + rr.error_message);
        int pc = -1;
        for (size_t c = 0; c < rr.columns.size(); ++c)
          if (lower(rr.columns[c]) == lower(k.part_col)) pc = (int)c;
        if (pc < 0) continue;
        for (const SqlRow& row : rr.rows) {
          // As the binlog consumer applies a row: every column, upsert on the key.
          std::string cols, vals, upd;
          for (size_t c = 0; c < rr.columns.size() && c < row.values.size(); ++c) {
            if (c) { cols += ", "; vals += ", "; upd += ", "; }
            cols += ident(rr.columns[c]);
            vals += sql_lit(row.values[c]);
            upd += ident(rr.columns[c]) + " = " + sql_lit(row.values[c]);
          }
          stmts_by_part[canon(row.values[(size_t)pc])].push_back(
              "INSERT INTO " + ident(tname) + " (" + cols + ") VALUES (" + vals + ") ON DUPLICATE KEY UPDATE " + upd);
          rows_captured++;
        }
      }
    }
    // Take them out - through DELETE, so the Things hear it and empty.
    for (const std::string& tk : tables) {
      const size_t dot = tk.find('.');
      const SqlTable* t = db_.resolve_table(tk.substr(dot + 1), tk.substr(0, dot));
      if (!t) continue;
      const std::string tname = t->name, tdb = t->database;
      for (size_t at = 0; at < all.size(); at += kPartitionChunk) {
        std::string list;
        for (size_t i = at; i < std::min(all.size(), at + kPartitionChunk); ++i)
          list += (i > at ? "," : "") + (all_digits(all[i]) ? all[i] : sql_lit(all[i]));
        sql_things_defer(+1);
        SqlQueryResult d = db_.execute_local_query("DELETE FROM " + ident(tname) + " WHERE " + ident(k.part_col) +
                                                   " IN (" + list + ")", tdb);
        sql_things_defer(-1);
        if (!d.success) return error_result("remove " + tname + ": " + d.error_message);
      }
    }
    lk.lock();
    settle_kind(k);
    k.replay_days.clear();
    for (auto& d : by_day) {
      std::vector<std::string> stmts;
      for (const auto& p : d.second) {
        stmts.push_back("-- " + p);
        for (auto& s : stmts_by_part[p]) stmts.push_back(s);
      }
      k.replay_days.emplace_back(d.first, std::move(stmts));
    }
    k.replay_cursor = 0;
    std::string out = "AIOSREPLAY 1\ncursor\t0\n";
    for (const auto& d : k.replay_days) {
      out += "day\t" + esc(d.first) + "\n";
      for (const auto& s : d.second) out += "s\t" + esc(s) + "\n";
    }
    db_.kernel_.host().make_dir(things_dir());
    db_.kernel_.host().write_file(file, ByteSpan((const uint8_t*)out.data(), out.size()));
    dirty_ = true;
    res.columns = {"partitions", "rows", "days", "first_day", "last_day"};
    res.rows.push_back(SqlRow{{std::to_string(all.size()), std::to_string(rows_captured),
                               std::to_string(k.replay_days.size()),
                               k.replay_days.empty() ? sql_null() : k.replay_days.front().first,
                               k.replay_days.empty() ? sql_null() : k.replay_days.back().first}});
  } else if (action == "NEXT") {
    if (k.replay_days.empty()) {
      // After a restart: the captured days and the cursor are on disk.
      auto f = db_.kernel_.host().read_file(file);
      if (!f.ok()) return error_result("nothing to replay: REPLAY THING " + k.name + " RESET BY <day expression> first");
      std::string text(f->begin(), f->end());
      size_t at = 0;
      while (at < text.size()) {
        size_t nl = text.find('\n', at);
        std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = nl == std::string::npos ? text.size() : nl + 1;
        auto fl = split_tabs(line);
        if (fl.size() >= 2 && fl[0] == "cursor") k.replay_cursor = (size_t)std::strtoull(fl[1].c_str(), nullptr, 10);
        else if (fl.size() >= 2 && fl[0] == "day") k.replay_days.emplace_back(unesc(fl[1]), std::vector<std::string>{});
        else if (fl.size() >= 2 && fl[0] == "s" && !k.replay_days.empty()) k.replay_days.back().second.push_back(unesc(fl[1]));
      }
    }
    if (k.replay_cursor >= k.replay_days.size()) {
      res.columns = {"day", "invoices", "rows", "remaining_days"};
      res.rows.push_back(SqlRow{{sql_null(), "0", "0", "0"}});
      return res;
    }
    const auto day = k.replay_days[k.replay_cursor];
    lk.unlock();
    // One partition at a time, settled after each - as a binlog transaction
    // commits: an invoice's header and lines, then the reaction.
    size_t parts = 0, rows = 0;
    std::vector<std::string> group;
    auto flush_group = [&]() -> std::string {
      if (group.empty()) return "";
      sql_things_defer(+1);
      sql_redo_group(+1);
      for (const std::string& s : group) {
        SqlQueryResult r = db_.execute_local_query(s, k.db);
        if (!r.success) { sql_redo_group(-1); db_.flush_redo_pending(); sql_things_defer(-1); return r.error_message; }
        rows++;
      }
      sql_redo_group(-1);
      db_.flush_redo_pending();
      sql_things_defer(-1);
      group.clear();
      std::lock_guard<aios::hal::Mutex> g(mu_);
      settle_kind(k);
      parts++;
      return "";
    };
    for (const std::string& s : day.second) {
      if (s.rfind("-- ", 0) == 0) {
        std::string e = flush_group();
        if (!e.empty()) return error_result("replay insert: " + e);
        continue;
      }
      group.push_back(s);
    }
    std::string e = flush_group();
    if (!e.empty()) return error_result("replay insert: " + e);
    lk.lock();
    k.replay_cursor++;
    // Advance the cursor on disk: a restart resumes at the next day.
    auto f = db_.kernel_.host().read_file(file);
    if (f.ok()) {
      std::string text(f->begin(), f->end());
      size_t p = text.find("\ncursor\t");
      if (p != std::string::npos) {
        size_t e2 = text.find('\n', p + 1);
        text = text.substr(0, p) + "\ncursor\t" + std::to_string(k.replay_cursor) + text.substr(e2);
        db_.kernel_.host().write_file(file, ByteSpan((const uint8_t*)text.data(), text.size()));
      }
    }
    res.columns = {"day", "invoices", "rows", "remaining_days"};
    res.rows.push_back(SqlRow{{day.first, std::to_string(parts), std::to_string(rows),
                               std::to_string(k.replay_days.size() - k.replay_cursor)}});
  } else {
    return error_result("REPLAY THING <name> RESET BY <day expression> | NEXT");
  }
  if (!sql_wire_results()) res.formatted_table = db_.format_result_table(res.columns, res.rows);
  return res;
}

// A Thing statement, here or on another machine: `... ON <machine>` runs it
// on the fleet member whose identity starts so, `SHOW ... ON ALL` gathers
// every member's answer (unreachable ones listed, not skipped). Every SHOW
// row names the machine it came from.
SqlQueryResult ThingEngine::execute(const std::string& sql) {
  {
    size_t i = 0;
    while (i < sql.size() && std::isspace((unsigned char)sql[i])) ++i;
    if (sql.size() - i >= 4 && upper(sql.substr(i, 4)) == "WAIT")
      return error_result("WAIT THING waits outside a transaction (inside one it would hold the statement lock the "
                          "change it waits for needs)");
  }
  struct Wake { ThingEngine* e; ~Wake() { e->wake_waiters(); } } wake{this};   // RESOLVE, REFRESH .. may change a Thing
  std::string body, target;
  if (!split_target(sql, &body, &target)) {
    SqlQueryResult r = execute_local(sql);
    if (r.success && r.is_result_set) {
      tag_machine(&r, machine_id());
      if (!sql_wire_results()) r.formatted_table = db_.format_result_table(r.columns, r.rows);
    }
    return r;
  }
  const std::string self = machine_id();
  const std::string ut = upper(target);
  const std::string lt = lower(target);
  if (ut == "LOCAL" || (!lt.empty() && self.compare(0, lt.size(), lt) == 0 && ut != "ALL")) return execute(body);
  if (!fleet_fn()) return error_result("ON " + target + ": this machine is not in a fleet");
  const bool all = ut == "ALL";
  if (all && upper(body).rfind("SHOW", 0) != 0) return error_result("ON ALL is for SHOW; a control names its machine");
  std::vector<ThingFleetAnswer> answers;
  fleet_fn()(all ? std::string("ALL") : lt, body, &answers);
  if (!all) {
    if (answers.empty()) return error_result("no fleet member starts with " + target);
    if (answers.size() > 1) return error_result("more than one fleet member starts with " + target);
    if (!answers[0].ok) return error_result(answers[0].machine + ": " + answers[0].error);
    SqlQueryResult r = *answers[0].result;
    tag_machine(&r, answers[0].machine);
    if (r.is_result_set && !sql_wire_results()) r.formatted_table = db_.format_result_table(r.columns, r.rows);
    return r;
  }
  // ON ALL: this machine first, then every member, columns merged by name.
  std::vector<std::pair<std::string, SqlQueryResult>> parts;
  {
    SqlQueryResult mine = execute_local(body);
    if (mine.success) { tag_machine(&mine, self); parts.emplace_back(self, std::move(mine)); }
  }
  SqlQueryResult res;
  res.is_result_set = true;
  std::vector<std::pair<std::string, std::string>> failed;
  for (auto& a : answers) {
    if (!a.ok || !a.result) { failed.emplace_back(a.machine, a.error); continue; }
    SqlQueryResult r = *a.result;
    if (!r.success) { failed.emplace_back(a.machine, r.error_message); continue; }
    tag_machine(&r, a.machine);
    parts.emplace_back(a.machine, std::move(r));
  }
  std::unordered_map<std::string, size_t> col_at;
  for (auto& p : parts)
    for (auto& c : p.second.columns)
      if (col_at.emplace(c, res.columns.size()).second) res.columns.push_back(c);
  if (!failed.empty() && !col_at.count("unreachable")) { col_at["unreachable"] = res.columns.size(); res.columns.push_back("unreachable"); }
  if (!col_at.count("machine")) { col_at["machine"] = res.columns.size(); res.columns.push_back("machine"); }
  for (auto& p : parts)
    for (auto& row : p.second.rows) {
      SqlRow out;
      out.values.assign(res.columns.size(), sql_null());
      for (size_t i = 0; i < p.second.columns.size() && i < row.values.size(); ++i) out.values[col_at[p.second.columns[i]]] = row.values[i];
      res.rows.push_back(std::move(out));
    }
  for (auto& f : failed) {
    SqlRow out;
    out.values.assign(res.columns.size(), sql_null());
    out.values[col_at["machine"]] = f.first;
    out.values[col_at["unreachable"]] = f.second;
    res.rows.push_back(std::move(out));
  }
  if (!sql_wire_results()) res.formatted_table = db_.format_result_table(res.columns, res.rows);
  return res;
}

// PAUSE / RESUME / REFRESH / FIRE ... NOW / RETRY: a Thing's own state, never a table.
SqlQueryResult ThingEngine::control(const std::string& verb, const std::string& name) {
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  MuHeld held;
  auto it = kinds_.find(lower(name));
  if (it == kinds_.end()) return error_result("no Thing kind named " + name);
  Kind& k = it->second;
  std::string msg;
  if (verb == "PAUSE") {
    k.paused = true;
    msg = "Thing " + k.name + " paused: changes are noted, nothing reacts" + (k.type == Kind::Event ? " or fires" : "") + " until RESUME.";
  } else if (verb == "RESUME") {
    k.paused = false;
    if (k.type == Kind::Claims) {
      k.pending_full = true;
      auto rit = kinds_.find(k.resource);
      if (rit != kinds_.end()) rit->second.pending_full = true;
    } else if (k.type == Kind::Resource) {
      k.pending_full = true;
      if (!k.claimed_by.empty()) { auto cit = kinds_.find(k.claimed_by); if (cit != kinds_.end()) cit->second.pending_full = true; }
    } else if (k.type == Kind::Mirror) {
      k.next_pull_ns = 0;
    } else if (k.type != Kind::Event) {
      // what happened while paused is re-read whole: nothing is assumed
      k.pending_full = true;
      k.full_cause = "resumed";
    }
    msg = "Thing " + k.name + " resumed.";
  } else if (verb == "REFRESH" && k.type == Kind::Mirror) {
    k.next_pull_ns = 0;
    msg = "Thing " + k.name + " pulls again now.";
  } else if (verb == "REFRESH") {
    if (k.type == Kind::Event) return error_result(k.name + " acts once; FIRE or RETRY it instead");
    k.pending_full = true;
    k.full_cause = "refreshed by hand";
    if (k.type == Kind::Resource && !k.claimed_by.empty()) { auto cit = kinds_.find(k.claimed_by); if (cit != kinds_.end()) cit->second.pending_full = true; }
    if (k.type == Kind::Claims) { auto rit = kinds_.find(k.resource); if (rit != kinds_.end()) rit->second.pending_full = true; }
    msg = "Thing " + k.name + " refreshed.";
  } else if (verb == "FIRE") {
    if (k.type != Kind::Event) return error_result(k.name + " is not a Thing that acts once");
    if (k.state == "firing") return error_result(k.name + " is firing now");
    k.at = [] { std::time_t t = std::time(nullptr); std::tm tm_buf{}; localtime_r(&t, &tm_buf); char b[32];
                std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm_buf); return std::string(b); }();
    k.state = "armed";
    k.fire_error.clear();
    msg = "Thing " + k.name + " fires now (" + k.at + ").";
  } else if (verb == "RETRY") {
    if (k.type != Kind::Event) return error_result(k.name + " is not a Thing that acts once");
    if (k.state != "failed") return error_result(k.name + " has not failed (state " + k.state + ")");
    k.state = "armed";
    k.fire_error.clear();
    alert(k.name, "", "retried", "armed again for " + k.at);
    msg = "Thing " + k.name + " armed again; it fires at " + k.at + " (now, if that has passed).";
  }
  dirty_ = true;
  save_locked();
  if (verb == "RESUME" || verb == "REFRESH") settle_all_locked();
  SqlQueryResult res;
  res.formatted_table = msg;
  return res;
}

SqlQueryResult ThingEngine::execute_local(const std::string& sql) {
  const std::string u = upper(trim(sql));
  if (u.rfind("SET", 0) == 0) return set_alerts(sql);
  for (const char* v : {"PAUSE", "RESUME", "REFRESH", "FIRE", "RETRY"}) {
    if (u.rfind(v, 0) != 0) continue;
    Scan sc(sql);
    std::string rest = trim(sql.substr(sc.find_kw("THING") + 5));
    if (!rest.empty() && rest.back() == ';') rest = trim(rest.substr(0, rest.size() - 1));
    std::string name = rest;
    if (std::string(v) == "FIRE") {
      Scan rs(rest);
      const size_t now = rs.find_kw("NOW");
      if (now == std::string::npos) return error_result("FIRE THING <name> NOW");
      name = trim(rest.substr(0, now));
    }
    name = unquote_ident(name);
    if (name.empty()) return error_result(std::string(v) + " THING <name>");
    return control(v, name);
  }
  Scan sc(sql);
  if (u.rfind("CREATE", 0) == 0) {
    // CREATE OR REPLACE THING: a grain Thing that exists is built anew beside the old one, which answers meanwhile
    std::string stmt = sql;
    bool or_replace = false;
    {
      Scan cs(sql);
      const size_t p_or = cs.find_kw("OR REPLACE"), p_th = cs.find_kw("THING");
      if (p_or != std::string::npos && p_th != std::string::npos && p_or < p_th) {
        or_replace = true;
        stmt = trim(sql.substr(0, p_or)) + " " + trim(sql.substr(p_or + 10));
      }
    }
    Kind k;
    std::string err;
    if (!parse_create(stmt, &k, &err)) return error_result(err);
    k.db = db_.effective_database();
    k.created_ns = db_.kernel_.host().wall_ns();
    if (k.type == Kind::Resource && k.cap_table.find('.') == std::string::npos) k.cap_table = lower(k.db) + "." + k.cap_table;
    std::unique_lock<aios::hal::Mutex> lk(mu_);
    MuHeld held;
    if (or_replace && kinds_.count(k.name)) {
      if (k.type != Kind::Grain || kinds_.at(k.name).type != Kind::Grain)
        return error_result("CREATE OR REPLACE THING replaces a grain Thing with a grain Thing; DROP " + k.name + " first");
      return replace_grain(std::move(k), lk);
    }
    if (kinds_.count(k.name)) return error_result("a Thing kind named " + k.name + " already exists");
    if (k.type == Kind::Resource) {
      const size_t dot = k.cap_table.find('.');
      const SqlTable* t = db_.resolve_table(k.cap_table.substr(dot + 1), k.cap_table.substr(0, dot));
      if (!t) return error_result("no table " + k.cap_table);
      for (const auto& c : k.key_cols)
        if (t->get_column_index(c) < 0) return error_result(k.cap_table + " has no column " + c);
      if (t->get_column_index(k.cap_col) < 0) return error_result(k.cap_table + " has no column " + k.cap_col);
      const std::string name = k.name;
      kinds_.emplace(name, std::move(k));
      rebuild_watched();
      dirty_ = true;
      save_locked();
      SqlQueryResult res;
      res.formatted_table = "Resource Thing " + name + " created: it holds what is claimed of it.";
      return res;
    }
    if (k.type == Kind::Claims) {
      auto rit = kinds_.find(k.resource);
      if (rit == kinds_.end() || rit->second.type != Kind::Resource)
        return error_result("no resource Thing named " + k.resource + " (CREATE THING ... RESOURCE first)");
      if (!rit->second.claimed_by.empty())
        return error_result(k.resource + " is already claimed by " + rit->second.claimed_by);
      const size_t want = 2 + rit->second.key_cols.size() + 1;
      if (k.items.size() != want)
        return error_result("the claims query must return " + std::to_string(want) + " columns: key, line id, " +
                            std::to_string(rit->second.key_cols.size()) + " resource key column(s), quantity needed");
      const std::string name = k.name;
      auto it = kinds_.emplace(name, std::move(k)).first;
      rit = kinds_.find(it->second.resource);
      rit->second.claimed_by = name;
      if (!load_claims(it->second, {}, true, "created", &err)) {
        rit->second.claimed_by.clear();
        rit->second.res.clear();
        kinds_.erase(it);
        return error_result(err);
      }
      std::vector<std::string> keys;
      for (const auto& e : rit->second.res) keys.push_back(e.first);
      rit->second.pending_res.clear();
      if (!load_capacity(rit->second, keys, "created", &err)) {
        rit->second.claimed_by.clear();
        rit->second.res.clear();
        kinds_.erase(it);
        return error_result(err);
      }
      for (const auto& kk : keys) it->second.dirty_queues.insert(kk);
      reallocate(it->second, rit->second, "created", false);
      rebuild_watched();
      dirty_ = true;
      save_locked();
      size_t ready = 0;
      for (const auto& o : it->second.orders) {
        bool ok = true;
        for (const auto& ln : o.second.lines) { ThingDec d = ln.need; dec_add(&d, ln.alloc, -1); if (d.v > 0) ok = false; }
        if (ok) ++ready;
      }
      SqlQueryResult res;
      res.rows_affected = it->second.orders.size();
      res.formatted_table = "Thing " + name + " created: " + std::to_string(it->second.orders.size()) + " claimants (" +
                            std::to_string(ready) + " ready) over " + std::to_string(rit->second.res.size()) + " " +
                            rit->second.name + " things.";
      return res;
    }
    if (k.type == Kind::Mirror) {
#if defined(AIOS_METAL)
      return error_result(kBoardNoPull);
#endif
      if (!fleet_fn()) return error_result("a mirror needs the fleet, and this engine has no fleet link");
      const std::string name = k.name;
      auto it = kinds_.emplace(name, std::move(k)).first;
      it->second.next_pull_ns = 0;   // the clock pulls it straight away, outside this statement
      dirty_ = true;
      save_locked();
      SqlQueryResult res;
      res.formatted_table = "Thing " + name + " created: " +
                            (it->second.mirror_kind.empty() ? std::string("the fleet's machines")
                                                            : it->second.mirror_kind + " on " + it->second.mirror_machine) +
                            ", pulled every " + std::to_string(it->second.every_s) + " s (the first pull runs now).";
      return res;
    }
    if (k.type == Kind::Event) {
      const char* gate = std::getenv("AIOS_THING_WRITES");
      if (!gate || std::string(gate) != "1")
        return error_result("an event Thing writes: it only runs in a process started with AIOS_THING_WRITES=1");
      if (!k.write_target.empty()) {
        bool found = false, may = false;
        for (const auto& p : db_.list_profiles()) if (p.id == k.write_target) { found = true; may = p.things_write; }
        if (!found) return error_result("WRITES TO " + k.write_target + ": no such connection profile");
        if (!may) return error_result("WRITES TO " + k.write_target + ": the profile is not marked things_write=1 (a Thing writes only where someone said it may)");
      }
      const std::string name = k.name;
      auto it = kinds_.emplace(name, std::move(k)).first;
      if (!grain_boot(it->second, &err)) { kinds_.erase(it); return error_result(err); }
      dirty_ = true;
      save_locked();
      SqlQueryResult res;
      res.formatted_table = "Thing " + name + " armed: fires at " + it->second.at + ".";
      return res;
    }
    if (k.type == Kind::Grain) {
      for (const auto& l : k.links) {
        auto lit = kinds_.find(l.first);
        if (lit == kinds_.end() || lit->second.type != Kind::Claims) return error_result("LINKS " + l.first + ": no claims Thing by that name");
        auto rit = kinds_.find(lit->second.resource);
        bool found = false;
        if (rit != kinds_.end())
          for (const auto& kc : rit->second.key_cols) found = found || lower(kc) == lower(l.second);
        if (!found) return error_result("LINKS " + l.first + "(" + l.second + "): not a key column of " + lit->second.resource);
      }
      const std::string name = k.name;
      auto it = kinds_.emplace(name, std::move(k)).first;
      if (!grain_boot(it->second, &err) || !grain_full(it->second, "created", &err)) {
        kinds_.erase(it);
        return error_result(err);
      }
      rebuild_watched();
      dirty_ = true;
      save_locked();
      SqlQueryResult res;
      res.rows_affected = it->second.gthings.size();
      char ms[32];
      std::snprintf(ms, sizeof ms, "%.0f", it->second.last_full_ms);
      res.formatted_table = "Thing " + name + " created: " + std::to_string(it->second.gthings.size()) + " things (" +
                            ms + " ms).";
      return res;
    }
    if (!k.do_sql.empty()) {
      if (!k.row_mode) return error_result("WHEN .. DO: only a row Thing (one row per key) acts");
      bool found = false, may = false;
      for (const auto& p : db_.list_profiles()) if (p.id == k.do_target) { found = true; may = p.things_write; }
      if (!found) return error_result("DO .. TO " + k.do_target + ": no such connection profile");
      if (!may) return error_result("DO .. TO " + k.do_target + ": the profile is not marked things_write=1 (a Thing writes only where someone said it may)");
    }
    auto it = kinds_.emplace(k.name, std::move(k)).first;
    if (!run_full(it->second, "created", &err)) {
      kinds_.erase(it);
      return error_result(err);
    }
    act_due(it->second);
    // On disk before the statement returns: a declared Thing is not
    // something a restart can rebuild from the tables. (Saved only at the
    // next checkpoint, a quiet mirror restarted without it.)
    dirty_ = true;
    save_locked();
    SqlQueryResult res;
    res.rows_affected = it->second.things.size();
    res.formatted_table = "Thing " + it->second.name + " created: " + std::to_string(it->second.things.size()) +
                          " things over " + std::to_string(it->second.parts.size()) + " partitions.";
    return res;
  }
  if (u.rfind("DROP", 0) == 0) {
    std::string name = lower(unquote_ident(trim(sql.substr(sc.find_kw("THING") + 5))));
    std::lock_guard<aios::hal::Mutex> lk(mu_);
    auto dit = kinds_.find(name);
    if (dit == kinds_.end()) return error_result("no Thing kind named " + name);
    if (dit->second.type == Kind::Resource && !dit->second.claimed_by.empty())
      return error_result(name + " is claimed by " + dit->second.claimed_by + ": drop that first");
    if (dit->second.building) return error_result(name + " is a replacement still being built: it takes its name, or goes, by itself");
    if (dit->second.type == Kind::Claims) {
      auto rit = kinds_.find(dit->second.resource);
      if (rit != kinds_.end()) { rit->second.claimed_by.clear(); rit->second.res.clear(); }
    }
    kinds_.erase(dit);
    rebuild_watched();
    // Nothing is left to be alarmed about.
    for (const auto& al : std::map<std::string, std::pair<std::string, std::string>>(alert_last_)) {
      const size_t sep = al.first.find('\x01');
      if (sep != std::string::npos && al.first.compare(0, sep, name) == 0 && sep == name.size())
        alert(name, al.first.substr(sep + 1), "cleared", "the Thing was dropped");
    }
    db_.kernel_.host().remove(things_dir() + "/" + name + ".thing");
    db_.kernel_.host().remove(things_dir() + "/" + name + ".replay");
    dirty_ = true;
    SqlQueryResult res;
    res.formatted_table = "Thing " + name + " dropped.";
    return res;
  }
  if (u.rfind("CALL", 0) == 0) return call_report(sql);
  if (u.rfind("REPLAY", 0) == 0) {
    // REPLAY THING name RESET BY <expr> | REPLAY THING name NEXT
    size_t p = sc.find_kw("THING") + 5;
    std::string rest = trim(sql.substr(p));
    size_t sp = 0;
    while (sp < rest.size() && !std::isspace((unsigned char)rest[sp])) ++sp;
    std::string name = unquote_ident(rest.substr(0, sp));
    std::string tail = trim(rest.substr(sp));
    std::string ut = upper(tail);
    {
      std::unique_lock<aios::hal::Mutex> lk(mu_);
      auto kit = kinds_.find(lower(name));
      if (kit != kinds_.end() && kit->second.type == Kind::Claims) {
        const char* gate = std::getenv("AIOS_THING_REPLAY");
        if (!gate || std::string(gate) != "1")
          return error_result("REPLAY THING only runs in a process started with AIOS_THING_REPLAY=1 (it deletes and re-inserts rows)");
        if (ut == "RESET") return replay_claims(kit->second, "RESET", 0);
        if (ut.rfind("NEXT", 0) == 0) {
          size_t n = 1;
          std::string num = trim(tail.substr(4));
          if (!num.empty()) n = (size_t)std::strtoull(num.c_str(), nullptr, 10);
          return replay_claims(kit->second, "NEXT", n ? n : 1);
        }
        return error_result("REPLAY THING <claims> RESET | NEXT [n]");
      }
    }
    if (ut.rfind("RESET", 0) == 0) {
      Scan ts(tail);
      size_t by = ts.find_kw("BY");
      return replay(name, "RESET", by == std::string::npos ? "" : trim(tail.substr(by + 2)));
    }
    if (ut == "NEXT") return replay(name, "NEXT", "");
    return error_result("REPLAY THING <name> RESET BY <day expression> | NEXT");
  }
  if (u.rfind("RESOLVE", 0) == 0) return resolve(sql);
  if (u.rfind("CHECK", 0) == 0) return check(sql);
  // SHOW THING CONFLICT GROUPS name [BY CHANGE]
  if (sc.find_kw("GROUPS") != std::string::npos && sc.find_kw("CONFLICT") != std::string::npos) return conflict_groups(sql);
  // SHOW THING CONFLICTS name ['key']
  {
    const size_t cp = sc.find_kw("CONFLICTS");
    if (cp != std::string::npos) {
      std::string rest = trim(sql.substr(cp + 9));
      if (!rest.empty() && rest.back() == ';') rest = trim(rest.substr(0, rest.size() - 1));
      size_t sp = 0;
      while (sp < rest.size() && !std::isspace((unsigned char)rest[sp])) ++sp;
      std::string key = trim(rest.substr(sp));
      if (key.size() >= 2 && (key.front() == '\'' || key.front() == '"') && key.back() == key.front()) key = key.substr(1, key.size() - 2);
      return show_conflicts(unquote_ident(rest.substr(0, sp)), key);
    }
  }
  // SHOW THING ALERTS [name]: newest first.
  size_t ap = sc.find_kw("ALERTS");
  if (ap != std::string::npos) {
    std::string name = unquote_ident(trim(sql.substr(ap + 6)));
    if (!name.empty() && name.back() == ';') name = unquote_ident(trim(name.substr(0, name.size() - 1)));
    return show_alerts(name);
  }
  // SHOW THINGS [name] | SHOW THING HISTORY name 'key'
  std::unique_lock<aios::hal::Mutex> lk(mu_);
  settle_all_locked();
  if (sc.find_kw("RECEIPTS") != std::string::npos) return show_receipts();
  size_t alp = sc.find_kw("ALLOCATION");
  if (alp != std::string::npos) {
    std::string name = unquote_ident(trim(sql.substr(alp + 10)));
    if (!name.empty() && name.back() == ';') name.pop_back();
    auto it = kinds_.find(lower(name));
    if (it == kinds_.end() || it->second.type != Kind::Claims) return error_result("no claims Thing named " + name);
    return show_allocation(it->second);
  }
  size_t lp = sc.find_kw("LINES");
  if (lp != std::string::npos) {
    std::string name = unquote_ident(trim(sql.substr(lp + 5)));
    if (!name.empty() && name.back() == ';') name.pop_back();
    auto it = kinds_.find(lower(name));
    if (it == kinds_.end() || it->second.type != Kind::Claims) return error_result("no claims Thing named " + name);
    return show_lines(it->second);
  }
  size_t hp = sc.find_kw("HISTORY");
  if (hp != std::string::npos) {
    std::string rest = trim(sql.substr(hp + 7));
    size_t sp = 0;
    while (sp < rest.size() && !std::isspace((unsigned char)rest[sp])) ++sp;
    std::string name = unquote_ident(rest.substr(0, sp));
    std::string key = trim(rest.substr(sp));
    if (key.size() >= 2 && (key.front() == '\'' || key.front() == '"') && key.back() == key.front())
      key = key.substr(1, key.size() - 2);
    if (upper(key) == "NULL") key = kNullKey;
    return show_history(name, key);
  }
  size_t tp = sc.find_kw("THINGS");
  if (tp == std::string::npos) tp = sc.find_kw("THING");
  std::string name = trim(sql.substr(tp + (sc.find_kw("THINGS") != std::string::npos ? 6 : 5)));
  if (!name.empty() && name.back() == ';') name.pop_back();
  {
    // SHOW THING name JSON [SINCE n] | SHOW THING name CLOCK
    Scan ns(name);
    const size_t jp = ns.find_kw("JSON"), cp = ns.find_kw("CLOCK");
    if (jp != std::string::npos || cp != std::string::npos) {
      const size_t cut = std::min(jp, cp);
      const std::string kind = lower(unquote_ident(trim(name.substr(0, cut))));
      auto it = kinds_.find(kind);
      if (it == kinds_.end()) return error_result("no Thing kind named " + kind);
      if (it->second.type != Kind::Aggregate) return error_result("SHOW THING .. JSON: " + kind + " is not a row Thing");
      uint64_t since = 0;
      const size_t sp = ns.find_kw("SINCE");
      if (sp != std::string::npos) since = std::strtoull(trim(name.substr(sp + 5)).c_str(), nullptr, 10);
      SqlQueryResult r = show_json(it->second, since, cp != std::string::npos && jp == std::string::npos);
      if (!sql_wire_results()) r.formatted_table = db_.format_result_table(r.columns, r.rows);
      return r;
    }
  }
  {
    // SHOW THING name KEYS ('a', 'b', ...): only those Things of a grain kind
    Scan ns(name);
    const size_t kp = ns.find_kw("KEYS");
    if (kp != std::string::npos) {
      const std::string kind = lower(unquote_ident(trim(name.substr(0, kp))));
      auto it = kinds_.find(kind);
      if (it == kinds_.end()) return error_result("no Thing kind named " + kind);
      if (it->second.type != Kind::Grain && it->second.type != Kind::Mirror)
        return error_result("SHOW THING .. KEYS: " + kind + " is not a grain Thing");
      std::vector<std::string> keys;
      std::string list = trim(name.substr(kp + 4));
      if (list.size() < 2 || list.front() != '(' || list.back() != ')')
        return error_result("SHOW THING <name> KEYS ('key', ...)");
      list = list.substr(1, list.size() - 2);
      for (size_t i = 0; i < list.size();) {
        while (i < list.size() && (std::isspace((unsigned char)list[i]) || list[i] == ',')) ++i;
        if (i >= list.size()) break;
        std::string v;
        if (list[i] == '\'' || list[i] == '"') {
          const char q = list[i++];
          for (; i < list.size(); ++i) {
            if (list[i] == '\\' && i + 1 < list.size()) { v += list[++i]; continue; }
            if (list[i] == q) {
              if (i + 1 < list.size() && list[i + 1] == q) { v += q; ++i; continue; }   // 'it''s'
              ++i;
              break;
            }
            v += list[i];
          }
        } else {
          while (i < list.size() && list[i] != ',' && !std::isspace((unsigned char)list[i])) v += list[i++];
          if (upper(v) == "NULL") v = kNullKey;
        }
        keys.push_back(std::move(v));
      }
      SqlQueryResult r = show_grain(it->second, &keys);
      return r;
    }
  }
  {
    // SHOW THING <grain kind>: the rows as of the kind's version are taken under the lock and copied after it is
    // let go. Built under the lock and copied there, a 50,000-item kind held every other reader (and every
    // reaction) for the whole copy and its formatting, so concurrent full reads ran one at a time.
    auto it = kinds_.find(lower(unquote_ident(name)));
    if (it != kinds_.end() && (it->second.type == Kind::Grain || it->second.type == Kind::Mirror)) {
      SqlQueryResult r;
      r.is_result_set = true;
      r.columns = grain_columns(it->second);
      std::shared_ptr<const std::vector<SqlRow>> rows = grain_rows_locked(it->second);
      lk.unlock();
      r.rows = *rows;
      if (!sql_wire_results()) r.formatted_table = db_.format_result_table(r.columns, r.rows);
      return r;
    }
  }
  return show(unquote_ident(name));
}

// ---- two kinds that know about each other: a resource and its claimants ------
//
//   CREATE THING stock RESOURCE KEY loccode, stockid CAPACITY quantity FROM locstock READS locstock
//   CREATE THING open_order CLAIMS stock KEY orderno PRIORITY orddate, orderno, orderlineno
//     READS salesorderdetails, salesorders
//     AS SELECT d.orderno, d.orderlineno, so.fromstkloc, d.stkcode, d.quantity - d.qtyinvoiced
//        FROM salesorderdetails d JOIN salesorders so ON so.orderno = d.orderno
//        WHERE d.completed = 0 AND so.quotation = 0 AND d.quantity > d.qtyinvoiced
//
// The claims query's columns are: claimant key, line id, the resource's key
// columns, the quantity needed. Each resource key's claims form a queue in
// PRIORITY order; a line gets what is left after the claims ahead of it
// (min(need, max(0, capacity - need ahead))), and a claimant is Ready when
// every line has all it needs. A change to a claimant re-reads its lines and
// re-queues the resources they touch; a change to a resource's rows re-reads
// its capacity and re-queues it - and every claimant in those queues sees
// the result. Claims never depend on readiness, so this cannot oscillate.
// With ALLOCATE READY (after PRIORITY) the plan instead makes the most
// claimants whole - see plan_ready - and still cannot oscillate: the plan is a
// pure function of the claims and capacities, not of the last plan.

bool ThingEngine::parse_resource(const std::string& sql, Kind* k, std::string* err) const {
  Scan sc(sql);
  size_t p_thing = sc.find_kw("THING"), p_res = sc.find_kw("RESOURCE"), p_key = sc.find_kw("KEY"),
         p_cap = sc.find_kw("CAPACITY"), p_from = sc.find_kw("FROM"), p_reads = sc.find_kw("READS");
  if (p_thing == std::string::npos || p_res == std::string::npos || p_key == std::string::npos ||
      p_cap == std::string::npos || p_from == std::string::npos ||
      !(p_thing < p_res && p_res < p_key && p_key < p_cap && p_cap < p_from)) {
    *err = "CREATE THING <name> RESOURCE KEY <col>[, ...] CAPACITY <col> FROM <table> [READS <table>]";
    return false;
  }
  k->type = Kind::Resource;
  k->name = lower(unquote_ident(sql.substr(p_thing + 5, p_res - p_thing - 5)));
  for (const std::string& c : sc.split_commas(p_key + 3, p_cap)) k->key_cols.push_back(unquote_ident(c));
  k->cap_col = unquote_ident(sql.substr(p_cap + 8, p_from - p_cap - 8));
  const size_t from_end = p_reads == std::string::npos ? sql.size() : p_reads;
  k->cap_table = lower(unquote_ident(sql.substr(p_from + 4, from_end - p_from - 4)));
  if (p_reads != std::string::npos)
    for (const std::string& r : sc.split_commas(p_reads + 5, sql.size())) k->reads.push_back(lower(unquote_ident(r)));
  else
    k->reads.push_back(k->cap_table);
  if (std::find(k->reads.begin(), k->reads.end(), k->cap_table) == k->reads.end()) {
    *err = "a resource must be allowed to read its own table (" + k->cap_table + ")";
    return false;
  }
  if (k->name.empty() || k->key_cols.empty() || k->cap_col.empty() || k->cap_table.empty()) {
    *err = "a resource needs a name, KEY columns, a CAPACITY column and a FROM table";
    return false;
  }
  k->declaration = trim(sql);
  return true;
}

bool ThingEngine::parse_claims(const std::string& sql, Kind* k, std::string* err) const {
  Scan sc(sql);
  size_t p_thing = sc.find_kw("THING"), p_cl = sc.find_kw("CLAIMS"), p_key = sc.find_kw("KEY"),
         p_prio = sc.find_kw("PRIORITY"), p_reads = sc.find_kw("READS"), p_alloc = sc.find_kw("ALLOCATE");
  size_t p_as = p_reads == std::string::npos ? std::string::npos : sc.find_kw("AS", p_reads);
  if (p_alloc != std::string::npos && p_reads != std::string::npos && p_alloc > p_reads) p_alloc = std::string::npos;
  if (p_thing == std::string::npos || p_cl == std::string::npos || p_key == std::string::npos ||
      p_prio == std::string::npos || p_reads == std::string::npos || p_as == std::string::npos ||
      !(p_thing < p_cl && p_cl < p_key && p_key < p_prio && p_prio < p_reads)) {
    *err = "CREATE THING <name> CLAIMS <resource> KEY <col> PRIORITY <expr>[, ...] [ALLOCATE FIFO|READY] READS <table>[, ...] "
           "AS SELECT <key>, <line id>, <resource key columns>, <quantity needed> FROM ... [WHERE ...]";
    return false;
  }
  if (p_alloc != std::string::npos) {
    if (p_alloc < p_prio) { *err = "ALLOCATE comes after PRIORITY"; return false; }
    const std::string mode = upper(trim(sql.substr(p_alloc + 8, p_reads - p_alloc - 8)));
    if (mode == "READY") k->alloc_ready = true;
    else if (mode != "FIFO") { *err = "ALLOCATE FIFO (first come, first served) or ALLOCATE READY (the most claimants complete)"; return false; }
  }
  k->type = Kind::Claims;
  k->name = lower(unquote_ident(sql.substr(p_thing + 5, p_cl - p_thing - 5)));
  k->resource = lower(unquote_ident(sql.substr(p_cl + 6, p_key - p_cl - 6)));
  k->key_col = unquote_ident(sql.substr(p_key + 3, p_prio - p_key - 3));
  k->part_col = k->key_col;
  for (const std::string& e : sc.split_commas(p_prio + 8, p_alloc != std::string::npos ? p_alloc : p_reads)) k->prio_exprs.push_back(e);
  for (const std::string& r : sc.split_commas(p_reads + 5, p_as)) {
    std::string t = lower(unquote_ident(r));
    size_t dot = t.find('.');
    if (dot != std::string::npos) t = t.substr(dot + 1);
    if (!t.empty()) k->reads.push_back(t);
  }
  const std::string select = trim(sql.substr(p_as + 2));
  Scan ss(select);
  size_t s_sel = ss.find_kw("SELECT"), s_from = ss.find_kw("FROM"), s_where = ss.find_kw("WHERE");
  if (s_sel != 0 || s_from == std::string::npos) { *err = "the claims query must be SELECT ... FROM ..."; return false; }
  for (const char* bad : {"GROUP BY", "HAVING", "ORDER BY", "LIMIT", "UNION"})
    if (ss.find_kw(bad) != std::string::npos) { *err = std::string("a claims query cannot use ") + bad + " (its order is PRIORITY)"; return false; }
  std::vector<std::string> items = ss.split_commas(s_sel + 6, s_from);
  k->claim_key_expr = items.empty() ? std::string() : items[0];
  k->claim_items = trim(select.substr(s_sel + 6, s_from - s_sel - 6));
  k->claim_from = trim(select.substr(s_from + 4, (s_where == std::string::npos ? select.size() : s_where) - s_from - 4));
  if (s_where != std::string::npos) k->claim_where = trim(select.substr(s_where + 5));
  // (the item count is checked against the resource's key when created)
  k->items.clear();
  Item key_item;
  key_item.text = k->claim_key_expr;
  key_item.label = k->key_col;
  k->items.push_back(key_item);
  for (size_t i = 1; i < items.size(); ++i) { Item it; it.text = items[i]; it.label = items[i]; k->items.push_back(it); }
  k->declaration = trim(sql);
  return true;
}

std::string ThingEngine::reskey_of(const std::vector<std::string>& vals) const {
  // As MariaDB's collation compares them: 'ABC' and 'abc' are one item.
  std::string k;
  for (size_t i = 0; i < vals.size(); ++i) {
    if (i) k.push_back('\x1f');
    k += sql_is_null(vals[i]) ? std::string("\x01NULL") : sql_collate_key(vals[i]);
  }
  return k;
}

namespace {
// Priority order: NULL first (as ORDER BY), numbers as numbers, text as text.
int prio_cmp(const std::vector<std::string>& a, const std::vector<std::string>& b) {
  for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
    const bool an = sql_is_null(a[i]), bn = sql_is_null(b[i]);
    if (an || bn) { if (an != bn) return an ? -1 : 1; continue; }
    ThingDec x, y;
    if (parse_dec(a[i], &x) && parse_dec(b[i], &y)) {
      ThingDec d = x;
      dec_add(&d, y, -1);
      if (d.v != 0) return d.v < 0 ? -1 : 1;
      continue;
    }
    int c = a[i].compare(b[i]);
    if (c != 0) return c < 0 ? -1 : 1;
  }
  return 0;
}
ThingDec dec_min(const ThingDec& a, const ThingDec& b) {
  ThingDec d = a;
  dec_add(&d, b, -1);
  return d.v <= 0 ? a : b;
}
ThingDec dec_max0(const ThingDec& a) { return a.v < 0 ? ThingDec{0, a.scale} : a; }
std::string dec_plain(const ThingDec& d) {
  std::string t = dec_text(d, d.scale);
  if (t.find('.') != std::string::npos) {
    while (!t.empty() && t.back() == '0') t.pop_back();
    if (!t.empty() && t.back() == '.') t.pop_back();
  }
  if (t == "-0") t = "0";
  return t;
}
}  // namespace

bool ThingEngine::load_capacity(Kind& r, const std::vector<std::string>& keys, const std::string& cause, std::string* err) {
  if (keys.empty()) return true;
  const size_t nk = r.key_cols.size();
  const size_t dot = r.cap_table.find('.');
  const std::string tname = dot == std::string::npos ? r.cap_table : r.cap_table.substr(dot + 1);
  std::string cols;
  for (size_t i = 0; i < nk; ++i) cols += (i ? ", " : "") + ident(r.key_cols[i]);
  // Keys asked for, found or not: a key with no row has no capacity (0).
  std::unordered_set<std::string> asked(keys.begin(), keys.end());
  std::unordered_map<std::string, ThingDec> got;
  for (size_t at = 0; at < keys.size(); at += 200) {
    std::string where;
    for (size_t i = at; i < std::min(keys.size(), at + 200); ++i) {
      auto it = r.res.find(keys[i]);
      if (it == r.res.end()) continue;
      std::string c;
      for (size_t j = 0; j < nk; ++j)
        c += (j ? " AND " : "") + ident(r.key_cols[j]) + " = " + sql_lit(it->second.keyvals[j]);
      where += (where.empty() ? "" : " OR ") + std::string("(") + c + ")";
    }
    if (where.empty()) continue;
    SqlQueryResult q = db_.execute_local_query("SELECT " + cols + ", SUM(" + ident(r.cap_col) + ") FROM " + ident(tname) +
                                               " WHERE " + where + " GROUP BY " + cols, r.db);
    if (!q.success) { *err = q.error_message; return false; }
    for (const SqlRow& row : q.rows) {
      if (row.values.size() < nk + 1) continue;
      std::vector<std::string> kv(row.values.begin(), row.values.begin() + (long)nk);
      ThingDec d;
      if (!sql_is_null(row.values[nk]) && !parse_dec(row.values[nk], &d)) {
        *err = "capacity '" + row.values[nk] + "' is not an exact number";
        return false;
      }
      got[reskey_of(kv)] = d;
    }
  }
  const uint64_t now = db_.kernel_.host().wall_ns();
  Kind* c = r.claimed_by.empty() ? nullptr : &kinds_[r.claimed_by];
  for (const std::string& key : keys) {
    auto it = r.res.find(key);
    if (it == r.res.end()) continue;
    auto g = got.find(key);
    ThingDec cap = g == got.end() ? ThingDec{} : g->second;
    const bool known = g != got.end();
    if (it->second.cap_known == known && dec_plain(it->second.cap) == dec_plain(cap)) continue;
    if (!r.res_pre.count(key)) r.res_pre[key] = res_display(it->second);
    it->second.cap = cap;
    it->second.cap_known = known;
    if (c) {
      c->dirty_queues.insert(key);
      if (c->wave_cause.empty()) c->wave_cause = cause;
    }
    (void)now;
  }
  return true;
}

bool ThingEngine::load_claims(Kind& c, const std::vector<std::string>& orders, bool all, const std::string& cause,
                              std::string* err) {
  Kind& r = kinds_[c.resource];
  const size_t nk = r.key_cols.size();
  std::string prio;
  for (const auto& e : c.prio_exprs) prio += ", " + e;
  std::string sql = "SELECT " + c.claim_items + prio + " FROM " + c.claim_from;
  std::string where = c.claim_where;
  if (!all) {
    std::string list;
    for (size_t i = 0; i < orders.size(); ++i) list += (i ? "," : "") + (all_digits(orders[i]) ? orders[i] : sql_lit(orders[i]));
    where = (where.empty() ? "" : "(" + where + ") AND ") + c.claim_key_expr + " IN (" + list + ")";
  }
  if (!where.empty()) sql += " WHERE " + where;
  std::unordered_set<const SqlTable*> read;
  if (all) sql_set_read_capture(&read);
  SqlQueryResult q = db_.execute_local_query(sql, c.db);
  if (all) sql_set_read_capture(nullptr);
  if (!q.success) { *err = q.error_message; return false; }
  if (all) {
    std::set<std::string> view_tabs;
    std::vector<std::string> via;
    for (const SqlTable* t : read) {
      if (!t || db_.resolve_table(t->name, t->database) != t) continue;
      if (!may_read(c.reads, {c.claim_from, c.claim_where}, c.db, *t, &view_tabs, &via)) {
        *err = "Thing '" + c.name + "' would read " + t->database + "." + t->name + ", which is not in its READS list";
        return false;
      }
      if (t->get_column_index(c.key_col) >= 0) c.deps_partition.insert(table_key(*t));
      else c.deps_full.insert(table_key(*t));
    }
  }
  const size_t want = 2 + nk + 1 + c.prio_exprs.size();
  std::map<std::string, std::vector<Kind::Line>> fresh;
  for (const SqlRow& row : q.rows) {
    if (row.values.size() != want) {
      *err = "the claims query returned " + std::to_string(row.values.size()) + " columns; expected key, line id, " +
             std::to_string(nk) + " resource key column(s), quantity and " + std::to_string(c.prio_exprs.size()) + " priorities";
      return false;
    }
    if (sql_is_null(row.values[0])) continue;
    Kind::Line ln;
    ln.lineid = row.values[1];
    ln.resvals.assign(row.values.begin() + 2, row.values.begin() + 2 + (long)nk);
    ln.reskey = reskey_of(ln.resvals);
    const std::string& need = row.values[2 + nk];
    if (sql_is_null(need)) continue;
    if (!parse_dec(need, &ln.need)) { *err = "quantity '" + need + "' is not an exact number"; return false; }
    ln.prio.assign(row.values.begin() + 3 + (long)nk, row.values.end());
    fresh[canon(row.values[0])].push_back(std::move(ln));
  }
  // Replace the orders asked about (all of them, or the partitions touched).
  std::vector<std::string> targets;
  if (all) {
    for (const auto& kv : c.orders) targets.push_back(kv.first);
    for (const auto& kv : fresh) if (!c.orders.count(kv.first)) targets.push_back(kv.first);
  } else {
    targets = orders;
  }
  for (const std::string& o : targets) {
    auto old = c.orders.find(o);
    if (!c.order_pre.count(o)) c.order_pre[o] = old == c.orders.end() ? std::string("status=Open") : order_display(old->second);
    if (old != c.orders.end())
      for (const auto& ln : old->second.lines) c.dirty_queues.insert(ln.reskey);
    auto f = fresh.find(o);
    if (f == fresh.end()) {
      if (old != c.orders.end()) {
        // Gone (invoiced, completed, cancelled): its claims leave the queues.
        old->second.lines.clear();
      }
      continue;
    }
    Kind::OrderInst& oi = c.orders[o];
    oi.lines = std::move(f->second);
    for (const auto& ln : oi.lines) {
      c.dirty_queues.insert(ln.reskey);
      if (!r.res.count(ln.reskey)) {
        Kind::ResInst ri;
        ri.keyvals = ln.resvals;
        r.res.emplace(ln.reskey, std::move(ri));
        r.pending_res[ln.reskey] = "claimed";
      }
    }
  }
  if (c.wave_cause.empty()) c.wave_cause = cause;
  return true;
}

std::string ThingEngine::res_display(const Kind::ResInst& ri) const {
  ThingDec avail = dec_max0(ri.cap), freeq = avail;
  dec_add(&freeq, ri.promised, -1);
  return "on_hand=" + (ri.cap_known ? dec_plain(ri.cap) : std::string("none")) + " | demand=" + dec_plain(ri.demand) +
         " | promised=" + dec_plain(ri.promised) + " | free=" + dec_plain(freeq);
}

std::string ThingEngine::order_display(const Kind::OrderInst& oi) const {
  size_t short_lines = 0;
  for (const auto& ln : oi.lines) {
    ThingDec d = ln.need;
    dec_add(&d, ln.alloc, -1);
    if (d.v > 0) ++short_lines;
  }
  if (oi.lines.empty()) return "status=Closed";
  return std::string("status=") + (short_lines ? "Waiting" : "Ready") + " | lines=" + std::to_string(oi.lines.size()) +
         " | short=" + std::to_string(short_lines);
}

std::string ThingEngine::order_waiting(const Kind& c, const Kind::OrderInst& oi) const {
  std::string w;
  size_t n = 0, more = 0;
  for (const auto& ln : oi.lines) {
    ThingDec d = ln.need;
    dec_add(&d, ln.alloc, -1);
    if (d.v <= 0) continue;
    if (n >= 3) { ++more; continue; }
    std::string where;
    for (size_t i = ln.resvals.size(); i-- > 0;) where += (where.empty() ? "" : " at ") + ln.resvals[i];
    w += (w.empty() ? "" : "; ") + where + ": short " + dec_plain(d) + " (" + std::to_string(ln.pos) + " of " +
         std::to_string(ln.qlen) + " in line)";
    ++n;
  }
  if (more) w += "; +" + std::to_string(more) + " more";
  if (w.empty()) w = oi.lines.empty() ? std::string("nothing (closed)") : "a change to its lines or to the stock it holds";
  (void)c;
  return w;
}

namespace {
// sum w_i * [l_i] <= cap as clauses: a BDD over the terms, heaviest first,
// nodes shared by (term, room left) - MiniSat+'s encoding. node(i, rem) true
// means "the terms from i on add up to at most rem"; only the direction that
// forbids going over is needed. 0 = constant true, -1 = constant false.
struct PbAtMost {
  std::vector<std::pair<int, int64_t>> terms;
  std::vector<int64_t> suffix;
  std::map<std::pair<size_t, int64_t>, int> memo;
  int* next_var;
  std::vector<std::vector<int>>* out;
  int node(size_t i, int64_t rem) {
    if (rem < 0) return -1;
    if (suffix[i] <= rem) return 0;
    const auto key = std::make_pair(i, rem);
    auto it = memo.find(key);
    if (it != memo.end()) return it->second;
    const int hi = node(i + 1, rem - terms[i].second), lo = node(i + 1, rem);
    const int v = ++*next_var, l = terms[i].first;
    if (hi == -1) out->push_back({-v, -l}); else if (hi != 0) out->push_back({-v, -l, hi});
    if (lo == -1) out->push_back({-v}); else if (lo != 0) out->push_back({-v, lo});
    memo[key] = v;
    return v;
  }
};
void pb_at_most(std::vector<std::pair<int, int64_t>> terms, int64_t cap, int* next_var, std::vector<std::vector<int>>* out) {
  std::sort(terms.begin(), terms.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
  PbAtMost e;
  e.terms = std::move(terms);
  e.suffix.assign(e.terms.size() + 1, 0);
  for (size_t i = e.terms.size(); i-- > 0;) e.suffix[i] = e.suffix[i + 1] + e.terms[i].second;
  e.next_var = next_var;
  e.out = out;
  const int root = e.node(0, cap);
  if (root == -1) out->push_back({});
  else if (root != 0) out->push_back({root});
}
bool dec_le(ThingDec a, const ThingDec& b) { dec_add(&a, b, -1); return a.v <= 0; }
}  // namespace

// ALLOCATE READY: the claimants to make whole. A claimant can be whole only
// if each of its lines fits the resource at all; a resource is contested when
// the claimants that could be whole need more than it holds. Claimants that
// touch nothing contested are whole outright; the rest fall into groups joined
// by the contested resources they share, and each group is solved exactly:
// every resource's capacity a pseudo-boolean constraint, "at least K whole" a
// cardinality one, K raised from the greedy count until the solver proves
// K + 1 impossible (that proof checked by the independent RUP/RAT checker);
// then, among the plans with K whole, the one that favours PRIORITY order
// (each claimant in turn kept if a plan with it still reaches K). A pure
// function of the current claims and capacities - the solver is
// deterministic - so a replay reaches the same plan and nothing oscillates.
void ThingEngine::plan_ready(Kind& c, Kind& r) {
  c.ready_plan.clear();
  auto cap_of = [&](const std::string& key) {
    auto it = r.res.find(key);
    return it == r.res.end() ? ThingDec{} : dec_max0(it->second.cap);
  };
  auto order_prio = [&](const Kind::OrderInst& oi) -> const std::vector<std::string>* {
    const std::vector<std::string>* best = nullptr;
    for (const auto& ln : oi.lines) if (!best || prio_cmp(ln.prio, *best) < 0) best = &ln.prio;
    return best;
  };
  // each claimant's need per resource, and whether it could be whole at all
  std::map<std::string, std::map<std::string, ThingDec>> need;   // order -> reskey -> need
  std::set<std::string> feasible;
  for (const auto& kv : c.orders) {
    if (kv.second.lines.empty()) continue;
    auto& m = need[kv.first];
    for (const auto& ln : kv.second.lines) dec_add(&m[ln.reskey], ln.need, +1);
    bool ok = true;
    for (const auto& kn : m) ok &= dec_le(kn.second, cap_of(kn.first));
    if (ok) feasible.insert(kv.first);
  }
  std::map<std::string, ThingDec> demand;
  for (const auto& o : feasible) for (const auto& kn : need[o]) dec_add(&demand[kn.first], kn.second, +1);
  std::set<std::string> contested;
  for (const auto& kd : demand) if (!dec_le(kd.second, cap_of(kd.first))) contested.insert(kd.first);
  // outright whole, and the groups (union-find over contested resources)
  std::map<std::string, std::string> parent;
  std::function<std::string(const std::string&)> find = [&](const std::string& x) -> std::string {
    auto it = parent.find(x);
    if (it == parent.end() || it->second == x) return x;
    return it->second = find(it->second);
  };
  std::vector<std::string> contenders;
  for (const auto& o : feasible) {
    std::string first;
    for (const auto& kn : need[o]) {
      if (!contested.count(kn.first)) continue;
      if (!parent.count(kn.first)) parent[kn.first] = kn.first;
      if (first.empty()) first = kn.first; else parent[find(kn.first)] = find(first);
    }
    if (first.empty()) c.ready_plan.insert(o); else contenders.push_back(o);
  }
  std::map<std::string, std::vector<std::string>> groups;   // root -> orders
  for (const auto& o : contenders) {
    std::string root;
    for (const auto& kn : need[o]) if (contested.count(kn.first)) { root = find(kn.first); break; }
    groups[root].push_back(o);
  }
  int64_t proven = 0;
  std::string evidence;
  size_t checked_lemmas = 0;
  for (auto& g : groups) {
    std::vector<std::string>& os = g.second;
    std::sort(os.begin(), os.end(), [&](const std::string& a, const std::string& b) {
      const int p = prio_cmp(*order_prio(c.orders[a]), *order_prio(c.orders[b]));
      return p != 0 ? p < 0 : a < b;
    });
    // weights as integers at the group's widest scale
    int scale = 0;
    std::set<std::string> keys;
    for (const auto& o : os) for (const auto& kn : need[o]) if (contested.count(kn.first)) { keys.insert(kn.first); scale = std::max(scale, kn.second.scale); }
    for (const auto& k : keys) scale = std::max(scale, cap_of(k).scale);
    auto units = [&](const ThingDec& d) -> int64_t { return (int64_t)(d.v * pow10i(scale - d.scale)); };
    const int n = (int)os.size();
    // greedy in PRIORITY order: the count to beat
    std::vector<uint8_t> greedy(n, 0);
    {
      std::map<std::string, int64_t> room;
      for (const auto& k : keys) room[k] = units(cap_of(k));
      for (int i = 0; i < n; ++i) {
        bool fits = true;
        for (const auto& kn : need[os[i]]) if (keys.count(kn.first)) fits &= units(kn.second) <= room[kn.first];
        if (!fits) continue;
        greedy[i] = 1;
        for (const auto& kn : need[os[i]]) if (keys.count(kn.first)) room[kn.first] -= units(kn.second);
      }
    }
    int best = 0;
    for (uint8_t b : greedy) best += b;
    std::vector<std::vector<int>> base;
    int nv = n;
    for (const auto& k : keys) {
      std::vector<std::pair<int, int64_t>> terms;
      for (int i = 0; i < n; ++i) { auto it = need[os[i]].find(k); if (it != need[os[i]].end()) terms.push_back({i + 1, units(it->second)}); }
      pb_at_most(std::move(terms), units(cap_of(k)), &nv, &base);
    }
    auto with_at_least = [&](int K, int* vars) {
      std::vector<std::vector<int>> f = base;
      *vars = nv;
      std::vector<std::pair<int, int64_t>> t;
      for (int i = 0; i < n; ++i) t.push_back({-(i + 1), 1});
      pb_at_most(std::move(t), n - K, vars, &f);   // at most n - K left out
      return f;
    };
    // raise K until K + 1 is proven impossible
    std::vector<uint8_t> chosen = greedy;
    bool optimal = false;
    for (int K = best + 1; K <= n + 1; ++K) {
      int vars = 0;
      const auto f = with_at_least(K, &vars);
      if (K == n + 1) { optimal = true; break; }   // everyone whole
      const SatAnswer a = solve_verified(vars, f, 200000);
      if (a.status == 10 && a.verified) {
        best = 0;
        for (int i = 0; i < n; ++i) { chosen[i] = a.model[(size_t)i] > 0; best += chosen[i]; }
        K = best;   // (the loop's ++K asks for one more than this model has)
        continue;
      }
      if (a.status == 20 && a.verified) { optimal = true; checked_lemmas += (size_t)a.conflicts; }
      break;
    }
    if (optimal) ++proven;
    // among plans with `best` whole: each claimant in PRIORITY order kept if
    // a plan with it (and the choices before it) still reaches `best`
    {
      int vars = 0;
      const auto f = with_at_least(best, &vars);
      SatSolver s;
      s.set_elimination(false);
      s.reserve_vars(vars);
      bool ok = true;
      for (const auto& cl : f) if (!s.add_clause(cl)) { ok = false; break; }
      std::vector<int> assume;
      bool clean = ok;
      for (int i = 0; i < n && clean; ++i) {
        assume.push_back(i + 1);
        s.set_conflict_limit(200000);
        const auto res = s.solve(assume);
        if (res == SatSolver::Result::Unsat) assume.back() = -(i + 1);
        else if (res == SatSolver::Result::Unknown) clean = false;
      }
      if (clean) {
        s.set_conflict_limit(0);
        if (s.solve(assume) == SatSolver::Result::Sat) {
          std::vector<std::vector<int>> orig = f;
          std::vector<bool> model = s.model();
          if (SatSolver::check_model(orig, model))
            for (int i = 0; i < n; ++i) chosen[i] = model[(size_t)i + 1];
        }
      }
    }
    for (int i = 0; i < n; ++i) if (chosen[i]) c.ready_plan.insert(os[i]);
  }
  // what first come, first served would have made whole, for comparison
  int64_t fifo = 0;
  {
    std::map<std::string, std::vector<std::pair<std::string, size_t>>> queues;
    for (const auto& kv : c.orders) for (size_t i = 0; i < kv.second.lines.size(); ++i) queues[kv.second.lines[i].reskey].emplace_back(kv.first, i);
    std::set<std::string> shortq;
    for (auto& kq : queues) {
      auto& q = kq.second;
      std::sort(q.begin(), q.end(), [&](const auto& a, const auto& b) {
        const auto& la = c.orders[a.first].lines[a.second];
        const auto& lb = c.orders[b.first].lines[b.second];
        int p = prio_cmp(la.prio, lb.prio);
        if (p != 0) return p < 0;
        if (a.first != b.first) return a.first < b.first;
        return la.lineid < lb.lineid;
      });
      ThingDec ahead{};
      const ThingDec avail = cap_of(kq.first);
      for (const auto& e : q) {
        const auto& ln = c.orders[e.first].lines[e.second];
        ThingDec left = avail;
        dec_add(&left, ahead, -1);
        if (!dec_le(ln.need, dec_max0(left))) shortq.insert(e.first);
        dec_add(&ahead, ln.need, +1);
      }
    }
    for (const auto& kv : c.orders) if (!kv.second.lines.empty() && !shortq.count(kv.first)) ++fifo;
  }
  c.alloc_orders = (int64_t)need.size();
  c.alloc_ready_n = (int64_t)c.ready_plan.size();
  c.alloc_fifo_n = fifo;
  c.alloc_components = (int64_t)groups.size();
  c.alloc_proven = proven;
  c.alloc_evidence = std::to_string(contested.size()) + " contested resources in " + std::to_string(groups.size()) +
                     " groups; " + (proven == (int64_t)groups.size() ? std::string("every group optimal") : std::to_string(proven) + " proven optimal") +
                     (groups.empty() ? std::string() : " (one more whole claimant proven impossible, each proof checked)");
  (void)checked_lemmas;
}

SqlQueryResult ThingEngine::show_allocation(const Kind& c) const {
  SqlQueryResult res;
  res.is_result_set = true;
  res.columns = {"thing", "allocate", "claimants", "whole", "whole_if_fifo", "groups", "proven_optimal", "evidence"};
  SqlRow row;
  row.values = {c.name, c.alloc_ready ? "READY" : "FIFO", std::to_string(c.alloc_orders), std::to_string(c.alloc_ready_n),
                std::to_string(c.alloc_fifo_n), std::to_string(c.alloc_components), std::to_string(c.alloc_proven),
                c.alloc_ready ? c.alloc_evidence : std::string("first come, first served")};
  res.rows.push_back(std::move(row));
  if (!sql_wire_results()) res.formatted_table = db_.format_result_table(res.columns, res.rows);
  return res;
}

void ThingEngine::reallocate(Kind& c, Kind& r, const std::string& cause, bool quiet) {
  if (c.dirty_queues.empty()) return;
  // Grain kinds that LINK these claims hear which items' queues are shared
  // out again (not at load: what they hold was saved with these queues).
  auto notify_links = [&](const std::set<std::string>& keys) {
    if (quiet) return;
    for (auto& gk : kinds_) {
      Kind& g = gk.second;
      if (g.type != Kind::Grain) continue;
      auto l = g.links.find(c.name);
      if (l == g.links.end()) continue;
      size_t col = r.key_cols.size();
      for (size_t i = 0; i < r.key_cols.size(); ++i)
        if (lower(r.key_cols[i]) == lower(l->second)) col = i;
      if (col == r.key_cols.size()) continue;
      for (const std::string& key : keys) {
        const std::vector<std::string>* vals = nullptr;
        auto rit = r.res.find(key);
        if (rit != r.res.end()) vals = &rit->second.keyvals;
        if (!vals)
          for (const auto& o : c.orders) {
            for (const auto& ln : o.second.lines)
              if (ln.reskey == key) { vals = &ln.resvals; break; }
            if (vals) break;
          }
        if (vals && col < vals->size() && !sql_is_null((*vals)[col]))
          g.pending[(*vals)[col]] = c.name + " queue shared out again: " + (*vals)[col] + (cause.empty() ? "" : " (" + cause + ")");
      }
    }
  };
  // ALLOCATE READY: one change can reshape the whole plan, so every queue is
  // shared out again - and linked kinds hear of the items whose allocation
  // actually moved (and of the ones that changed to begin with).
  std::unordered_map<std::string, std::string> alloc_before;
  auto alloc_sig = [&](std::unordered_map<std::string, std::string>* m) {
    for (const auto& kv : c.orders)
      for (const auto& ln : kv.second.lines) {
        std::string& sgn = (*m)[ln.reskey];
        sgn += kv.first; sgn += '\x1f'; sgn += ln.lineid; sgn += '\x1f'; sgn += dec_plain(ln.alloc); sgn += '\x1e';
      }
  };
  std::set<std::string> first_dirty;
  if (c.alloc_ready) {
    first_dirty = c.dirty_queues;
    alloc_sig(&alloc_before);
    for (const auto& kv : c.orders) for (const auto& ln : kv.second.lines) c.dirty_queues.insert(ln.reskey);
    for (const auto& q : c.queue_index) c.dirty_queues.insert(q.first);
    plan_ready(c, r);
  } else {
    notify_links(c.dirty_queues);
  }
  // Rebuild the queue index for the dirty keys from the current lines.
  std::map<std::string, std::string> order_before = std::move(c.order_pre), res_before = std::move(r.res_pre);
  c.order_pre.clear();
  r.res_pre.clear();
  for (const std::string& key : c.dirty_queues) c.queue_index[key].clear();
  for (auto& kv : c.orders)
    for (size_t i = 0; i < kv.second.lines.size(); ++i) {
      const auto& ln = kv.second.lines[i];
      if (c.dirty_queues.count(ln.reskey)) c.queue_index[ln.reskey].emplace_back(kv.first, i);
    }
  for (const std::string& key : c.dirty_queues) {
    auto& q = c.queue_index[key];
    std::sort(q.begin(), q.end(), [&](const auto& a, const auto& b) {
      const auto& la = c.orders[a.first].lines[a.second];
      const auto& lb = c.orders[b.first].lines[b.second];
      int p = prio_cmp(la.prio, lb.prio);
      if (p != 0) return p < 0;
      if (a.first != b.first) return a.first < b.first;
      return la.lineid < lb.lineid;
    });
    auto rit = r.res.find(key);
    ThingDec avail = rit == r.res.end() ? ThingDec{} : dec_max0(rit->second.cap);
    ThingDec ahead{}, demand{}, promised{};
    int64_t pos = 0;
    if (c.alloc_ready) {
      // the plan's claimants whole first, then what is left in PRIORITY order
      for (const auto& e : q)
        if (!order_before.count(e.first)) order_before[e.first] = order_display(c.orders[e.first]);
      ThingDec left = avail;
      for (const auto& e : q) {
        if (!c.ready_plan.count(e.first)) continue;
        Kind::Line& ln = c.orders[e.first].lines[e.second];
        ln.alloc = ln.need;
        dec_add(&left, ln.need, -1);
      }
      for (const auto& e : q) {
        if (c.ready_plan.count(e.first)) continue;
        Kind::Line& ln = c.orders[e.first].lines[e.second];
        ln.alloc = dec_min(ln.need, dec_max0(left));
        dec_add(&left, ln.alloc, -1);
      }
    }
    for (const auto& e : q) {
      Kind::OrderInst& oi = c.orders[e.first];
      if (!order_before.count(e.first)) order_before[e.first] = order_display(oi);
      Kind::Line& ln = oi.lines[e.second];
      if (!c.alloc_ready) {
        ThingDec left = avail;
        dec_add(&left, ahead, -1);
        ln.alloc = dec_min(ln.need, dec_max0(left));
      }
      dec_add(&ahead, ln.need, +1);
      dec_add(&demand, ln.need, +1);
      dec_add(&promised, ln.alloc, +1);
      ln.pos = ++pos;
      ln.qlen = (int64_t)q.size();
    }
    if (rit != r.res.end()) {
      if (!res_before.count(key)) res_before[key] = res_display(rit->second);
      rit->second.demand = demand;
      rit->second.promised = promised;
      rit->second.claims = (int64_t)q.size();
    }
    if (q.empty()) c.queue_index.erase(key);
  }
  // Orders whose lines were cleared (gone) also report.
  for (auto& kv : c.orders)
    if (kv.second.lines.empty() && !order_before.count(kv.first)) order_before[kv.first] = "status=Open";
  c.dirty_queues.clear();
  if (c.alloc_ready) {
    std::unordered_map<std::string, std::string> alloc_after;
    alloc_sig(&alloc_after);
    std::set<std::string> moved = first_dirty;
    for (const auto& kv : alloc_after) { auto b = alloc_before.find(kv.first); if (b == alloc_before.end() || b->second != kv.second) moved.insert(kv.first); }
    for (const auto& kv : alloc_before) if (!alloc_after.count(kv.first)) moved.insert(kv.first);
    notify_links(moved);
  }
  const uint64_t now = db_.kernel_.host().wall_ns();
  for (const auto& kv : order_before) {
    auto it = c.orders.find(kv.first);
    if (it == c.orders.end()) continue;
    const std::string after = order_display(it->second);
    if (after == kv.second) continue;
    if (!quiet) {
      it->second.version++;
      it->second.updated_ns = now;
      it->second.history.push_back(History{now, cause, kv.second, after});
      if (it->second.history.size() > kHistoryKeep) it->second.history.erase(it->second.history.begin());
      c.version++;
    }
  }
  for (const auto& kv : res_before) {
    auto it = r.res.find(kv.first);
    if (it == r.res.end()) continue;
    const std::string after = res_display(it->second);
    if (after == kv.second) continue;
    if (!quiet) {
      it->second.version++;
      it->second.updated_ns = now;
      it->second.history.push_back(History{now, cause, kv.second, after});
      if (it->second.history.size() > kHistoryKeep) it->second.history.erase(it->second.history.begin());
      r.version++;
    }
  }
  // Closed orders and unclaimed stock are not kept: nothing can change them
  // until they are claimed or reopened, and then they are read afresh.
  for (auto it = c.orders.begin(); it != c.orders.end();) {
    if (it->second.lines.empty()) it = c.orders.erase(it); else ++it;
  }
  for (auto it = r.res.begin(); it != r.res.end();) {
    if (it->second.claims == 0 && !c.queue_index.count(it->first)) it = r.res.erase(it); else ++it;
  }
  dirty_ = true;
}

void ThingEngine::settle_links() {
  for (auto& kv : kinds_) {
    Kind& c = kv.second;
    if (c.type != Kind::Claims) continue;
    auto rit = kinds_.find(c.resource);
    if (rit == kinds_.end()) continue;
    Kind& r = rit->second;
    if (c.paused || r.paused) continue;
    std::string err;
    std::string cause;
    // 1. Claimants whose rows changed re-read their lines.
    if (c.pending_full) {
      cause = c.full_cause.empty() ? std::string("full recompute") : c.full_cause;
      c.pending_full = false;
      c.pending.clear();
      c.full_cause.clear();
      if (!load_claims(c, {}, true, cause, &err)) fprintf(stderr, "[things] %s: %s\n", c.name.c_str(), err.c_str());
      c.full_recomputes++;
    } else if (!c.pending.empty()) {
      std::vector<std::pair<std::string, std::string>> todo(c.pending.begin(), c.pending.end());
      c.pending.clear();
      std::sort(todo.begin(), todo.end());
      cause = todo.size() == 1 ? todo[0].second : todo[0].second + " (+" + std::to_string(todo.size() - 1) + " more)";
      for (size_t at = 0; at < todo.size(); at += kPartitionChunk) {
        std::vector<std::string> part;
        for (size_t i = at; i < std::min(todo.size(), at + kPartitionChunk); ++i) part.push_back(todo[i].first);
        if (!load_claims(c, part, false, cause, &err)) fprintf(stderr, "[things] %s: %s\n", c.name.c_str(), err.c_str());
      }
      c.reactions += todo.size();
    }
    // 2. Resources whose rows changed (or newly claimed) re-read their capacity.
    if (r.pending_full) {
      std::vector<std::string> keys;
      for (const auto& e : r.res) keys.push_back(e.first);
      r.pending_full = false;
      r.pending_res.clear();
      const std::string rc = r.full_cause.empty() ? std::string("full recompute") : r.full_cause;
      r.full_cause.clear();
      if (cause.empty()) cause = rc;
      if (!load_capacity(r, keys, rc, &err)) fprintf(stderr, "[things] %s: %s\n", r.name.c_str(), err.c_str());
      for (const auto& k : keys) c.dirty_queues.insert(k);
      r.full_recomputes++;
    } else if (!r.pending_res.empty()) {
      std::vector<std::string> keys;
      std::string rc;
      for (const auto& e : r.pending_res) { keys.push_back(e.first); if (rc.empty() || e.second != "claimed") rc = e.second; }
      r.pending_res.clear();
      if (cause.empty()) cause = rc;
      if (!load_capacity(r, keys, rc, &err)) fprintf(stderr, "[things] %s: %s\n", r.name.c_str(), err.c_str());
      for (const auto& k : keys) c.dirty_queues.insert(k);
      r.reactions += keys.size();
    }
    // 3. The queues touched are shared out again; everyone in them hears it.
    if (!c.dirty_queues.empty()) {
      if (cause.empty()) cause = c.wave_cause.empty() ? std::string("re-queued") : c.wave_cause;
      reallocate(c, r, cause, false);
    }
    c.wave_cause.clear();
  }
}

void ThingEngine::rebuild_watched() {
  watched_.clear();
  for (const auto& kv : kinds_) {
    for (const auto& d : kv.second.deps_partition) watched_.insert(d);
    for (const auto& d : kv.second.deps_full) watched_.insert(d);
    for (const auto& p : kv.second.part_defs) {
      if (!p.key_dep.empty()) watched_.insert(p.key_dep);
      for (const auto& d : p.deps_full) watched_.insert(d);
    }
    if (kv.second.type == Kind::Resource && !kv.second.cap_table.empty()) watched_.insert(kv.second.cap_table);
    if (kv.second.type == Kind::Grain)
      for (const auto& r : kv.second.reads) watched_.insert(lower(kv.second.db) + "." + r);
  }
}

SqlQueryResult ThingEngine::show_resource(const Kind& k) {
  SqlQueryResult res;
  res.is_result_set = true;
  for (const auto& c : k.key_cols) res.columns.push_back(c);
  for (const char* c : {"on_hand", "demand", "promised", "free", "short", "claims", "version", "updated_at", "last_change", "waiting_for", "may_read"})
    res.columns.push_back(c);
  std::string reads;
  for (const auto& r : k.reads) reads += (reads.empty() ? "" : ", ") + r;
  for (const auto& r : k.view_reads) reads += ", " + r;
  for (const auto& kv : k.res) {
    const auto& ri = kv.second;
    SqlRow row;
    for (const auto& v : ri.keyvals) row.values.push_back(v);
    ThingDec avail = dec_max0(ri.cap), freeq = avail, shortq = ri.demand;
    dec_add(&freeq, ri.promised, -1);
    dec_add(&shortq, ri.promised, -1);
    row.values.push_back(ri.cap_known ? dec_plain(ri.cap) : sql_null());
    row.values.push_back(dec_plain(ri.demand));
    row.values.push_back(dec_plain(ri.promised));
    row.values.push_back(dec_plain(freeq));
    row.values.push_back(dec_plain(shortq));
    row.values.push_back(std::to_string(ri.claims));
    row.values.push_back(std::to_string(ri.version));
    row.values.push_back(wall_text(ri.updated_ns));
    row.values.push_back(ri.history.empty() ? sql_null() : ri.history.back().cause);
    row.values.push_back("a change to its " + k.cap_table.substr(k.cap_table.find('.') + 1) + " row(s) or to the claims on it");
    row.values.push_back(reads);
    res.rows.push_back(std::move(row));
  }
  if (!sql_wire_results()) res.formatted_table = db_.format_result_table(res.columns, res.rows);
  return res;
}

SqlQueryResult ThingEngine::show_claims(const Kind& k) {
  SqlQueryResult res;
  res.is_result_set = true;
  res.columns = {k.key_col, "status", "lines", "short_lines", "version", "updated_at", "last_change", "waiting_for", "may_read"};
  std::string reads;
  for (const auto& r : k.reads) reads += (reads.empty() ? "" : ", ") + r;
  for (const auto& r : k.view_reads) reads += ", " + r;
  for (const auto& kv : k.orders) {
    const auto& oi = kv.second;
    size_t short_lines = 0;
    for (const auto& ln : oi.lines) { ThingDec d = ln.need; dec_add(&d, ln.alloc, -1); if (d.v > 0) ++short_lines; }
    res.rows.push_back(SqlRow{{kv.first, short_lines ? "Waiting" : "Ready", std::to_string(oi.lines.size()),
                               std::to_string(short_lines), std::to_string(oi.version), wall_text(oi.updated_ns),
                               oi.history.empty() ? sql_null() : oi.history.back().cause, order_waiting(k, oi), reads}});
  }
  if (!sql_wire_results()) res.formatted_table = db_.format_result_table(res.columns, res.rows);
  return res;
}

SqlQueryResult ThingEngine::show_lines(const Kind& k) {
  SqlQueryResult res;
  res.is_result_set = true;
  res.columns = {k.key_col, "line"};
  auto rit = kinds_.find(k.resource);
  if (rit != kinds_.end()) for (const auto& c : rit->second.key_cols) res.columns.push_back(c);
  for (const char* c : {"need", "allocated", "position", "queue"}) res.columns.push_back(c);
  for (const auto& kv : k.orders)
    for (const auto& ln : kv.second.lines) {
      SqlRow row;
      row.values.push_back(kv.first);
      row.values.push_back(ln.lineid);
      for (const auto& v : ln.resvals) row.values.push_back(v);
      row.values.push_back(dec_plain(ln.need));
      row.values.push_back(dec_plain(ln.alloc));
      row.values.push_back(std::to_string(ln.pos));
      row.values.push_back(std::to_string(ln.qlen));
      res.rows.push_back(std::move(row));
    }
  if (!sql_wire_results()) res.formatted_table = db_.format_result_table(res.columns, res.rows);
  return res;
}

// REPLAY for claimants: take every current claimant's rows out of the tables
// it is keyed in, then put them back one claimant at a time in PRIORITY
// order - orders arriving as they did - reacting after each.
SqlQueryResult ThingEngine::replay_claims(Kind& k, const std::string& action, size_t n) {
  const std::string file = things_dir() + "/" + k.name + ".replay";
  SqlQueryResult res;
  res.is_result_set = true;
  if (action == "RESET") {
    std::vector<std::pair<std::vector<std::string>, std::string>> order_prio;   // (first line's priority, order)
    for (const auto& kv : k.orders) {
      std::vector<std::string> best;
      for (const auto& ln : kv.second.lines) if (best.empty() || prio_cmp(ln.prio, best) < 0) best = ln.prio;
      order_prio.emplace_back(best, kv.first);
    }
    std::sort(order_prio.begin(), order_prio.end(), [](const auto& a, const auto& b) {
      int p = prio_cmp(a.first, b.first);
      return p != 0 ? p < 0 : a.second < b.second;
    });
    std::vector<std::string> all;
    for (const auto& e : order_prio) all.push_back(e.second);
    std::vector<std::string> tables(k.deps_partition.begin(), k.deps_partition.end());
    std::map<std::string, std::vector<std::string>> stmts;
    size_t rows_captured = 0;
    mu_.unlock();
    auto relock = [&]() { mu_.lock(); };
    for (const std::string& tk : tables) {
      const size_t dot = tk.find('.');
      const SqlTable* t = db_.resolve_table(tk.substr(dot + 1), tk.substr(0, dot));
      if (!t) continue;
      const std::string tname = t->name;
      for (size_t at = 0; at < all.size(); at += kPartitionChunk) {
        std::string list;
        for (size_t i = at; i < std::min(all.size(), at + kPartitionChunk); ++i)
          list += (i > at ? "," : "") + (all_digits(all[i]) ? all[i] : sql_lit(all[i]));
        SqlQueryResult rr = db_.execute_local_query("SELECT * FROM " + ident(tname) + " WHERE " + ident(k.key_col) + " IN (" + list + ")", k.db);
        if (!rr.success) { relock(); return error_result("capture " + tname + ": " + rr.error_message); }
        int pc = -1;
        for (size_t c = 0; c < rr.columns.size(); ++c) if (lower(rr.columns[c]) == lower(k.key_col)) pc = (int)c;
        if (pc < 0) continue;
        for (const SqlRow& row : rr.rows) {
          std::string cols, vals, upd;
          for (size_t c = 0; c < rr.columns.size() && c < row.values.size(); ++c) {
            if (c) { cols += ", "; vals += ", "; upd += ", "; }
            cols += ident(rr.columns[c]);
            vals += sql_lit(row.values[c]);
            upd += ident(rr.columns[c]) + " = " + sql_lit(row.values[c]);
          }
          stmts[canon(row.values[(size_t)pc])].push_back("INSERT INTO " + ident(tname) + " (" + cols + ") VALUES (" + vals +
                                                         ") ON DUPLICATE KEY UPDATE " + upd);
          rows_captured++;
        }
      }
    }
    // Take them out - through DELETE, so the Things hear it.
    for (const std::string& tk : tables) {
      const size_t dot = tk.find('.');
      const SqlTable* t = db_.resolve_table(tk.substr(dot + 1), tk.substr(0, dot));
      if (!t) continue;
      const std::string tname = t->name;
      for (size_t at = 0; at < all.size(); at += kPartitionChunk) {
        std::string list;
        for (size_t i = at; i < std::min(all.size(), at + kPartitionChunk); ++i)
          list += (i > at ? "," : "") + (all_digits(all[i]) ? all[i] : sql_lit(all[i]));
        sql_things_defer(+1);
        SqlQueryResult d = db_.execute_local_query("DELETE FROM " + ident(tname) + " WHERE " + ident(k.key_col) + " IN (" + list + ")", k.db);
        sql_things_defer(-1);
        if (!d.success) { relock(); return error_result("remove " + tname + ": " + d.error_message); }
      }
    }
    relock();
    settle_links();
    k.replay_days.clear();
    for (size_t i = 0; i < order_prio.size(); ++i) {
      std::string label = order_prio[i].second;
      for (const auto& p : order_prio[i].first) label += "\t" + (sql_is_null(p) ? std::string("NULL") : p);
      // (Statement order within a claimant does not matter: it reacts once
      // all of them are in.)
      k.replay_days.emplace_back(label, stmts[order_prio[i].second]);
    }
    k.replay_cursor = 0;
    std::string out = "AIOSREPLAY 1\ncursor\t0\n";
    for (const auto& d : k.replay_days) {
      out += "day\t" + esc(d.first) + "\n";
      for (const auto& s : d.second) out += "s\t" + esc(s) + "\n";
    }
    db_.kernel_.host().make_dir(things_dir());
    db_.kernel_.host().write_file(file, ByteSpan((const uint8_t*)out.data(), out.size()));
    dirty_ = true;
    res.columns = {"claimants", "rows"};
    res.rows.push_back(SqlRow{{std::to_string(all.size()), std::to_string(rows_captured)}});
  } else {
    if (k.replay_days.empty()) {
      auto f = db_.kernel_.host().read_file(file);
      if (!f.ok()) return error_result("nothing to replay: REPLAY THING " + k.name + " RESET first");
      std::string text(f->begin(), f->end());
      size_t at = 0;
      while (at < text.size()) {
        size_t nl = text.find('\n', at);
        std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = nl == std::string::npos ? text.size() : nl + 1;
        auto fl = split_tabs(line);
        if (fl.size() >= 2 && fl[0] == "cursor") k.replay_cursor = (size_t)std::strtoull(fl[1].c_str(), nullptr, 10);
        else if (fl.size() >= 2 && fl[0] == "day") k.replay_days.emplace_back(unesc(line.substr(4)), std::vector<std::string>{});
        else if (fl.size() >= 2 && fl[0] == "s" && !k.replay_days.empty()) k.replay_days.back().second.push_back(unesc(fl[1]));
      }
    }
    size_t applied = 0, rows = 0;
    std::string last;
    while (applied < n && k.replay_cursor < k.replay_days.size()) {
      const auto day = k.replay_days[k.replay_cursor];
      mu_.unlock();
      sql_things_defer(+1);
      sql_redo_group(+1);
      for (const std::string& s : day.second) {
        SqlQueryResult r = db_.execute_local_query(s, k.db);
        if (!r.success) { sql_redo_group(-1); db_.flush_redo_pending(); sql_things_defer(-1); mu_.lock(); return error_result("replay insert: " + r.error_message); }
        rows++;
      }
      sql_redo_group(-1);
      db_.flush_redo_pending();   // one fdatasync per order, as a binlog transaction's commit
      sql_things_defer(-1);
      mu_.lock();
      settle_links();
      last = day.first;
      k.replay_cursor++;
      applied++;
    }
    auto f = db_.kernel_.host().read_file(file);
    if (f.ok()) {
      std::string text(f->begin(), f->end());
      size_t p = text.find("\ncursor\t");
      if (p != std::string::npos) {
        size_t e2 = text.find('\n', p + 1);
        text = text.substr(0, p) + "\ncursor\t" + std::to_string(k.replay_cursor) + text.substr(e2);
        db_.kernel_.host().write_file(file, ByteSpan((const uint8_t*)text.data(), text.size()));
      }
    }
    res.columns = {"last", "claimants", "rows", "remaining"};
    res.rows.push_back(SqlRow{{last.empty() ? sql_null() : last, std::to_string(applied), std::to_string(rows),
                               std::to_string(k.replay_days.size() - k.replay_cursor)}});
  }
  if (!sql_wire_results()) res.formatted_table = db_.format_result_table(res.columns, res.rows);
  return res;
}

// ---- persistence -------------------------------------------------------------

std::string ThingEngine::serialize(const Kind& k) const {
  std::string o = "AIOSTHING 1\n";
  o += "declaration\t" + esc(k.declaration) + "\n";
  o += "db\t" + esc(k.db) + "\n";
  o += "created\t" + std::to_string(k.created_ns) + "\n";
  o += "version\t" + std::to_string(k.version) + "\n";
  o += "reactions\t" + std::to_string(k.reactions) + "\n";
  o += "full\t" + std::to_string(k.full_recomputes) + "\n";
  o += "day\t" + esc(k.day) + "\n";
  if (k.paused) o += "paused\t1\n";
  for (const auto& r : k.prefer) {
    o += "prefer\t" + esc(r.first);
    for (const auto& x : r.second) o += "\t" + esc(x);
    o += "\n";
  }
  for (const auto& c : k.constraints) {
    o += std::string("constraint\t") + (c.unique ? "UNIQUE" : "TOGETHER");
    for (const auto& x : c.fields) o += "\t" + esc(x);
    o += "\n";
  }
  for (const auto& d : k.deps_partition) o += "dep_part\t" + esc(d) + "\n";
  for (const auto& d : k.deps_full) o += "dep_full\t" + esc(d) + "\n";
  for (const auto& kv : k.parts) {
    o += "part\t" + esc(kv.first) + "\n";
    for (const Partial& pr : kv.second) {
      o += "p\t" + std::string(pr.key_null ? "N" : "V" + esc(pr.key));
      for (const auto& v : pr.vals) o += "\t" + val_field(v);
      o += "\n";
    }
  }
  for (const auto& kv : k.things) {
    const Instance& in = kv.second;
    o += "inst\t" + std::string(in.key_null ? "N" : "V" + esc(in.key)) + "\t" + std::to_string(in.version) + "\t" +
         std::to_string(in.updated_ns) + "\t" + std::to_string(in.stamp) + "\t" + (in.acted ? "1" : "0") + "\n";
    for (const History& h : in.history)
      o += "hist\t" + std::to_string(h.wall_ns) + "\t" + esc(h.cause) + "\t" + esc(h.before) + "\t" + esc(h.after) + "\n";
    for (const auto& pk : in.picks) o += "pick\t" + esc(pk.first) + "\t" + esc(pk.second) + "\n";
    for (const auto& pr : in.part_rows)
      for (const auto& row : pr.second) {
        o += "pr\t" + esc(pr.first) + "\t" + std::string(in.key_null ? "N" : "V" + esc(in.key)) + "\t" + std::to_string(row.size());
        for (const auto& v : row) o += "\t" + val_field(v);
        o += "\n";
      }
  }
  for (const auto& p : k.part_defs) {
    o += "pcols\t" + esc(p.name);
    for (const auto& c : p.cols) o += "\t" + esc(c);
    o += "\n";
  }
  if (!k.out_labels.empty()) {
    o += "labels";
    for (const auto& c : k.out_labels) o += "\t" + esc(c);
    o += "\n";
  }
  for (const auto& d : k.departed) o += "gone\t" + esc(d.first) + "\t" + std::to_string(d.second) + "\n";
  if (!k.do_sql.empty()) o += "act_clock\t" + std::to_string(k.act_clock) + "\n";
  if (k.type == Kind::Resource) {
    if (!k.claimed_by.empty()) o += "claimed_by\t" + esc(k.claimed_by) + "\n";
    for (const auto& kv : k.res) {
      const auto& ri = kv.second;
      o += "res\t" + std::to_string(ri.keyvals.size());
      for (const auto& v : ri.keyvals) o += "\t" + val_field(v);
      o += "\t" + std::string(ri.cap_known ? "V" + dec_text(ri.cap, ri.cap.scale) : "N") + "\t" + std::to_string(ri.version) +
           "\t" + std::to_string(ri.updated_ns) + "\n";
      for (const History& h : ri.history)
        o += "hist\t" + std::to_string(h.wall_ns) + "\t" + esc(h.cause) + "\t" + esc(h.before) + "\t" + esc(h.after) + "\n";
    }
  }
  if (k.type == Kind::Claims) {
    for (const auto& kv : k.orders) {
      const auto& oi = kv.second;
      o += "order\t" + esc(kv.first) + "\t" + std::to_string(oi.version) + "\t" + std::to_string(oi.updated_ns) + "\n";
      for (const auto& ln : oi.lines) {
        o += "line\t" + esc(ln.lineid) + "\t" + dec_text(ln.need, ln.need.scale) + "\t" + std::to_string(ln.resvals.size());
        for (const auto& v : ln.resvals) o += "\t" + val_field(v);
        o += "\t" + std::to_string(ln.prio.size());
        for (const auto& v : ln.prio) o += "\t" + val_field(v);
        o += "\n";
      }
      for (const History& h : oi.history)
        o += "hist\t" + std::to_string(h.wall_ns) + "\t" + esc(h.cause) + "\t" + esc(h.before) + "\t" + esc(h.after) + "\n";
    }
  }
  if (k.type == Kind::Grain || k.type == Kind::Mirror) o += grain_serialize(k);
  if (k.type == Kind::Mirror) o += "mkey\t" + esc(k.key_col) + "\t" + std::to_string(k.mirror_ok_ns) + "\n";
  if (k.type == Kind::Event) o += "estate\t" + esc(k.state) + "\t" + esc(k.fire_error) + "\n";
  o += "end\n";
  return o;
}

bool ThingEngine::deserialize(const std::string& text, Kind* k, std::string* err) {
  std::vector<std::string> lines;
  size_t at = 0;
  while (at < text.size()) {
    size_t nl = text.find('\n', at);
    lines.push_back(text.substr(at, nl == std::string::npos ? std::string::npos : nl - at));
    at = nl == std::string::npos ? text.size() : nl + 1;
  }
  if (lines.empty() || lines[0] != "AIOSTHING 1") { *err = "not a Thing file"; return false; }
  if (lines.back() != "end") { *err = "truncated Thing file"; return false; }
  std::string decl;
  for (const auto& l : lines) {
    auto f = split_tabs(l);
    if (f.size() >= 2 && f[0] == "declaration") decl = unesc(f[1]);
  }
  if (!parse_create(decl, k, err)) return false;
  std::string cur_part;
  Instance* cur_inst = nullptr;
  Kind::OrderInst* cur_order = nullptr;
  std::vector<History>* last_hist = nullptr;
  std::vector<std::pair<std::string, std::vector<Partial>>> parts;
  struct Meta { bool null; std::string key; uint64_t version, updated; std::vector<History> hist; uint64_t stamp = 0;
                std::map<std::string, std::vector<std::vector<std::string>>> part_rows; bool acted = false;
                std::map<std::string, std::string> picks; };
  std::vector<Meta> metas;
  for (size_t i = 1; i + 1 < lines.size(); ++i) {
    auto f = split_tabs(lines[i]);
    if (f.empty()) continue;
    if (f[0] == "db" && f.size() >= 2) k->db = unesc(f[1]);
    else if (f[0] == "created" && f.size() >= 2) k->created_ns = std::strtoull(f[1].c_str(), nullptr, 10);
    else if (f[0] == "version" && f.size() >= 2) k->version = std::strtoull(f[1].c_str(), nullptr, 10);
    else if (f[0] == "reactions" && f.size() >= 2) k->reactions = std::strtoull(f[1].c_str(), nullptr, 10);
    else if (f[0] == "full" && f.size() >= 2) k->full_recomputes = std::strtoull(f[1].c_str(), nullptr, 10);
    else if (f[0] == "day" && f.size() >= 2) k->day = unesc(f[1]);
    else if (f[0] == "paused" && f.size() >= 2) k->paused = f[1] == "1";
    else if (f[0] == "prefer" && f.size() >= 3) {
      std::vector<std::string> srcs;
      for (size_t j = 2; j < f.size(); ++j) srcs.push_back(unesc(f[j]));
      k->prefer.emplace_back(unesc(f[1]), std::move(srcs));
    } else if (f[0] == "constraint" && f.size() >= 3) {
      Kind::Constraint c;
      c.unique = f[1] == "UNIQUE";
      for (size_t j = 2; j < f.size(); ++j) c.fields.push_back(unesc(f[j]));
      k->constraints.push_back(std::move(c));
    }
    else if (f[0] == "dep_part" && f.size() >= 2) k->deps_partition.insert(unesc(f[1]));
    else if (f[0] == "dep_full" && f.size() >= 2) k->deps_full.insert(unesc(f[1]));
    else if (f[0] == "part" && f.size() >= 2) parts.emplace_back(unesc(f[1]), std::vector<Partial>{});
    else if (f[0] == "p" && f.size() >= 2 && !parts.empty()) {
      Partial pr;
      pr.key_null = f[1] == "N";
      pr.key = pr.key_null ? std::string() : unesc(f[1].substr(1));
      for (size_t j = 2; j < f.size(); ++j) pr.vals.push_back(field_val(f[j]));
      parts.back().second.push_back(std::move(pr));
    } else if (k->type == Kind::Resource && f[0] == "claimed_by" && f.size() >= 2) {
      k->claimed_by = unesc(f[1]);
    } else if (k->type == Kind::Resource && f[0] == "res" && f.size() >= 2) {
      const size_t nkv = (size_t)std::strtoull(f[1].c_str(), nullptr, 10);
      if (f.size() < 2 + nkv + 3) continue;
      Kind::ResInst ri;
      for (size_t j = 0; j < nkv; ++j) ri.keyvals.push_back(field_val(f[2 + j]));
      const std::string& cf = f[2 + nkv];
      ri.cap_known = !cf.empty() && cf[0] == 'V';
      if (ri.cap_known) parse_dec(cf.substr(1), &ri.cap);
      ri.version = std::strtoull(f[3 + nkv].c_str(), nullptr, 10);
      ri.updated_ns = std::strtoull(f[4 + nkv].c_str(), nullptr, 10);
      const std::string rk = reskey_of(ri.keyvals);
      last_hist = &k->res.emplace(rk, std::move(ri)).first->second.history;
    } else if (k->type == Kind::Claims && f[0] == "order" && f.size() >= 4) {
      Kind::OrderInst oi;
      oi.version = std::strtoull(f[2].c_str(), nullptr, 10);
      oi.updated_ns = std::strtoull(f[3].c_str(), nullptr, 10);
      cur_order = &k->orders.emplace(unesc(f[1]), std::move(oi)).first->second;
      last_hist = &cur_order->history;
    } else if (k->type == Kind::Claims && f[0] == "line" && f.size() >= 4 && cur_order) {
      Kind::Line ln;
      ln.lineid = unesc(f[1]);
      parse_dec(f[2], &ln.need);
      const size_t nr = (size_t)std::strtoull(f[3].c_str(), nullptr, 10);
      if (f.size() < 4 + nr + 1) continue;
      for (size_t j = 0; j < nr; ++j) ln.resvals.push_back(field_val(f[4 + j]));
      const size_t np = (size_t)std::strtoull(f[4 + nr].c_str(), nullptr, 10);
      for (size_t j = 0; j < np && 5 + nr + j < f.size(); ++j) ln.prio.push_back(field_val(f[5 + nr + j]));
      ln.reskey = reskey_of(ln.resvals);
      cur_order->lines.push_back(std::move(ln));
    } else if (k->type == Kind::Event && f[0] == "estate" && f.size() >= 2) {
      k->state = unesc(f[1]);
      if (f.size() >= 3) k->fire_error = unesc(f[2]);
    } else if (k->type == Kind::Grain && f[0] == "gshared" && f.size() >= 2) {
      k->shared_json = unesc(f[1]);
    } else if (k->type == Kind::Mirror && f[0] == "mkey" && f.size() >= 3) {
      k->key_col = unesc(f[1]);
      k->mirror_ok_ns = std::strtoull(f[2].c_str(), nullptr, 10);
    } else if ((k->type == Kind::Grain || k->type == Kind::Mirror) && f[0] == "gcols") {
      for (size_t j = 1; j < f.size(); ++j) k->gcols.push_back(unesc(f[j]));
    } else if ((k->type == Kind::Grain || k->type == Kind::Mirror) && f[0] == "g" && f.size() >= 5) {
      Kind::GInst gi;
      gi.key = unesc(f[1]);
      gi.version = std::strtoull(f[2].c_str(), nullptr, 10);
      gi.updated_ns = std::strtoull(f[3].c_str(), nullptr, 10);
      const size_t n = (size_t)std::strtoull(f[4].c_str(), nullptr, 10);
      for (size_t j = 0; j < n && 6 + 2 * j < f.size(); ++j) gi.vals.emplace_back(unesc(f[5 + 2 * j]), field_val(f[6 + 2 * j]));
      const std::string ck = sql_collate_key(gi.key);
      last_hist = &k->gthings.emplace(ck, std::move(gi)).first->second.history;
    } else if (k->type != Kind::Aggregate && f[0] == "hist" && f.size() >= 5 && last_hist) {
      last_hist->push_back(History{std::strtoull(f[1].c_str(), nullptr, 10), unesc(f[2]), unesc(f[3]), unesc(f[4])});
    } else if (f[0] == "inst" && f.size() >= 4) {
      Meta m;
      m.null = f[1] == "N";
      m.key = m.null ? std::string() : unesc(f[1].substr(1));
      m.version = std::strtoull(f[2].c_str(), nullptr, 10);
      m.updated = std::strtoull(f[3].c_str(), nullptr, 10);
      if (f.size() >= 5) m.stamp = std::strtoull(f[4].c_str(), nullptr, 10);
      if (f.size() >= 6) m.acted = f[5] == "1";
      metas.push_back(std::move(m));
    } else if (f[0] == "hist" && f.size() >= 5 && !metas.empty()) {
      metas.back().hist.push_back(History{std::strtoull(f[1].c_str(), nullptr, 10), unesc(f[2]), unesc(f[3]), unesc(f[4])});
    } else if (f[0] == "pick" && f.size() >= 3 && !metas.empty()) {
      metas.back().picks[unesc(f[1])] = unesc(f[2]);
    } else if (f[0] == "pr" && f.size() >= 4 && !metas.empty()) {
      const size_t n = (size_t)std::strtoull(f[3].c_str(), nullptr, 10);
      std::vector<std::string> row;
      for (size_t j = 0; j < n && 4 + j < f.size(); ++j) row.push_back(field_val(f[4 + j]));
      metas.back().part_rows[unesc(f[1])].push_back(std::move(row));
    } else if (f[0] == "pcols" && f.size() >= 2) {
      for (auto& p : k->part_defs)
        if (p.name == unesc(f[1])) { p.cols.clear(); for (size_t j = 2; j < f.size(); ++j) p.cols.push_back(unesc(f[j])); }
    } else if (f[0] == "labels") {
      k->out_labels.clear();
      for (size_t j = 1; j < f.size(); ++j) k->out_labels.push_back(unesc(f[j]));
    } else if (f[0] == "gone" && f.size() >= 3) {
      k->departed[unesc(f[1])] = std::strtoull(f[2].c_str(), nullptr, 10);
    } else if (f[0] == "act_clock" && f.size() >= 2) {
      k->act_clock = std::strtoull(f[1].c_str(), nullptr, 10);
    }
  }
  (void)cur_part;
  (void)cur_inst;
  // As at CREATE: a resource's table named with its database, the key its
  // change notices arrive under.
  if (k->type == Kind::Resource && k->cap_table.find('.') == std::string::npos) k->cap_table = lower(k->db) + "." + k->cap_table;
  if (k->type == Kind::Grain) {
    if (!grain_boot(*k, err)) return false;
    grain_restore(*k);
    return true;
  }
  if (k->type == Kind::Event) return grain_boot(*k, err);
  if (k->type == Kind::Mirror) return true;
  // The values are rebuilt from the partitions - the file's one source of truth.
  std::map<std::string, std::string> scratch;
  for (auto& p : parts) apply_partition(*k, p.first, std::move(p.second), &scratch);
  for (auto& m : metas) {
    const std::string ik = m.null ? std::string(kNullKey) : m.key;
    auto it = k->things.find(ik);
    if (it == k->things.end() && k->row_mode) continue;   // its row is the partition's; none, no Thing
    if (it == k->things.end()) {
      Instance in;
      in.key = m.key;
      in.key_null = m.null;
      const size_t nv = k->items.size() - 1;
      in.sums.assign(nv, ThingDec{});
      in.nonnull.assign(nv, 0);
      in.scale.assign(nv, 0);
      it = k->things.emplace(ik, std::move(in)).first;
    }
    it->second.version = m.version;
    it->second.updated_ns = m.updated;
    it->second.history = std::move(m.hist);
    it->second.stamp = m.stamp;
    it->second.part_rows = std::move(m.part_rows);
    it->second.acted = m.acted;
    if (!m.picks.empty()) {   // RESOLVE THING's choices, applied to the sides the partitions gave back
      it->second.picks = std::move(m.picks);
      merge_sides(*k, it->second);
    }
  }
  return true;
}

void ThingEngine::save() {
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  save_locked();
}

void ThingEngine::save_locked() {
  if (!dirty_) return;
  auto& host = db_.kernel_.host();
  host.make_dir(things_dir());
  for (const auto& kv : kinds_) {
    if (kv.second.building) continue;   // (a crash mid-build leaves the Thing it was to replace)
    const std::string text = serialize(kv.second);
    host.write_file(things_dir() + "/" + kv.first + ".thing", ByteSpan((const uint8_t*)text.data(), text.size()));
  }
  dirty_ = false;
}

void ThingEngine::load() {
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  MuHeld held;
  auto& host = db_.kernel_.host();
  if (!host.exists(things_dir())) return;
  load_alerts();
  auto names = host.list_dir(things_dir());
  if (!names.ok()) return;
  for (const std::string& n : *names) {
    if (n.size() < 7 || n.compare(n.size() - 6, 6, ".thing") != 0) continue;
    auto f = host.read_file(things_dir() + "/" + n);
    if (!f.ok()) continue;
    Kind k;
    std::string err;
    if (!deserialize(std::string(f->begin(), f->end()), &k, &err)) {
      fprintf(stderr, "[things] %s: not loaded: %s\n", n.c_str(), err.c_str());
      continue;
    }
    const std::string name = k.name;
    kinds_.emplace(name, std::move(k));
  }
  // Claims and resources back together: the shares are derived, so they are
  // worked out again from the saved claims and capacities (no history).
  for (auto& kv : kinds_) {
    Kind& c = kv.second;
    if (c.type != Kind::Claims) continue;
    auto rit = kinds_.find(c.resource);
    if (rit == kinds_.end()) continue;
    rit->second.claimed_by = c.name;
    for (const auto& o : c.orders)
      for (const auto& ln : o.second.lines) c.dirty_queues.insert(ln.reskey);
    reallocate(c, rit->second, "loaded", true);
  }
  // A part's tables are resolved against the schema, not stored: a loaded Thing
  // resolves them again here, or its part tables are never watched until the
  // next full recompute (a restart used to leave every part deaf).
  for (auto& kv : kinds_) {
    Kind& k = kv.second;
    if (k.part_defs.empty()) continue;
    std::string e;
    if (!resolve_part_deps(k, &e)) {
      fprintf(stderr, "[things] %s: parts not resolved at load (%s); recomputing in full\n", k.name.c_str(), e.c_str());
      k.pending_full = true;
      k.full_cause = "parts unresolved at load";
    }
  }
  rebuild_watched();
  if (!kinds_.empty()) {
    size_t things = 0;
    for (const auto& kv : kinds_)
      things += kv.second.things.size() + kv.second.res.size() + kv.second.orders.size() + kv.second.gthings.size();
    fprintf(stderr, "asql: %zu Thing kind(s), %zu things back from disk\n", kinds_.size(), things);
    fflush(stderr);
  }
}

// ---- grain Things ------------------------------------------------------------
//
//   CREATE THING reorder_daily GRAIN KEY stockid
//     READS stockmoves, debtortrans, locstock, ...
//     WATCH stockmoves(stockid), locstock(stockid), salesorderdetails(stkcode), ...
//     AS '<grain program>'
//
// The program, compiled once into the kind's own VM:
//   enroll()                -> the keys that may be Things (run on a full pass)
//   shared()                -> anything every key's answer depends on
//   things(keys, shared)    -> map key -> map of values; a key it leaves out is
//                              not a Thing (nothing to say about it)
//   touched(table, op, before, after, shared)   (optional)
//                           -> what a change means: nil (nothing), a list of
//                              keys, {keys: [...], shared: true} (shared() may
//                              have moved: rerun it, and if it did, every key),
//                              or "all"
// A row written to a WATCH table names its key by the column given; every
// change to a READS table also goes to touched(). Without touched(), a change
// to an unwatched table means a full pass. The key's answer replaces the old
// one, never adds to it, and the program sees only the tables - so replaying
// notices after a crash lands on the same values.

bool ThingEngine::parse_grain(const std::string& sql, Kind* k, std::string* err) const {
  Scan sc(sql);
  const size_t p_thing = sc.find_kw("THING"), p_gr = sc.find_kw("GRAIN"), p_key = sc.find_kw("KEY"),
               p_reads = sc.find_kw("READS"), p_watch = sc.find_kw("WATCH"), p_links = sc.find_kw("LINKS");
  const size_t p_as = p_reads == std::string::npos ? std::string::npos : sc.find_kw("AS", p_reads);
  if (p_thing == std::string::npos || p_gr == std::string::npos || p_key == std::string::npos ||
      p_reads == std::string::npos || p_as == std::string::npos || !(p_thing < p_gr && p_gr < p_key && p_key < p_reads) ||
      (p_watch != std::string::npos && !(p_reads < p_watch && p_watch < p_as)) ||
      (p_links != std::string::npos && !(p_reads < p_links && p_links < p_as && (p_watch == std::string::npos || p_watch < p_links)))) {
    *err = "CREATE THING <name> GRAIN KEY <column> READS <table>[, ...] [WATCH <table>(<column>)[, ...]] "
           "[LINKS <claims thing>(<key column>)[, ...]] AS '<grain program>'";
    return false;
  }
  k->type = Kind::Grain;
  k->name = lower(unquote_ident(sql.substr(p_thing + 5, p_gr - p_thing - 5)));
  k->key_col = unquote_ident(sql.substr(p_key + 3, p_reads - p_key - 3));
  const size_t links_at = p_links == std::string::npos ? p_as : p_links;
  const size_t reads_end = p_watch == std::string::npos ? links_at : p_watch;
  for (const std::string& r : sc.split_commas(p_reads + 5, reads_end)) {
    std::string t = lower(unquote_ident(r));
    const size_t dot = t.find('.');
    if (dot != std::string::npos) t = t.substr(dot + 1);
    if (!t.empty()) k->reads.push_back(t);
  }
  if (p_watch != std::string::npos) {
    for (const std::string& w : sc.split_commas(p_watch + 5, links_at)) {
      const size_t lp = w.find('('), rp = w.rfind(')');
      if (lp == std::string::npos || rp == std::string::npos || rp < lp) { *err = "WATCH wants <table>(<key column>), got " + w; return false; }
      std::string t = lower(unquote_ident(w.substr(0, lp)));
      const size_t dot = t.find('.');
      if (dot != std::string::npos) t = t.substr(dot + 1);
      if (std::find(k->reads.begin(), k->reads.end(), t) == k->reads.end()) { *err = "WATCH " + t + ": not in READS"; return false; }
      k->watch[t] = unquote_ident(w.substr(lp + 1, rp - lp - 1));
    }
  }
  if (p_links != std::string::npos) {
    for (const std::string& w : sc.split_commas(p_links + 5, p_as)) {
      const size_t lp = w.find('('), rp = w.rfind(')');
      if (lp == std::string::npos || rp == std::string::npos || rp < lp) { *err = "LINKS wants <claims thing>(<key column>), got " + w; return false; }
      k->links[lower(unquote_ident(w.substr(0, lp)))] = unquote_ident(w.substr(lp + 1, rp - lp - 1));
    }
  }
  std::string src = trim(sql.substr(p_as + 2));
  if (!src.empty() && src.back() == ';') src = trim(src.substr(0, src.size() - 1));
  if (src.size() < 2 || src.front() != '\'' || src.back() != '\'') { *err = "the program goes in one quoted string: AS '...'"; return false; }
  std::string body;
  for (size_t i = 1; i + 1 < src.size(); ++i) {
    if (src[i] == '\'' && i + 2 < src.size() && src[i + 1] == '\'') { body += '\''; ++i; continue; }
    if (src[i] == '\\' && i + 2 < src.size()) {   // MySQL's escapes, as a client's quoting writes them
      const char n = src[++i];
      body += n == 'n' ? '\n' : n == 't' ? '\t' : n == 'r' ? '\r' : n == '0' ? '\0' : n == 'Z' ? '\x1a' : n == 'b' ? '\b' : n;
      continue;
    }
    body += src[i];
  }
  if (k->name.empty() || k->key_col.empty() || k->reads.empty() || trim(body).empty()) {
    *err = "CREATE THING ... GRAIN needs a name, a KEY, a READS list and a program";
    return false;
  }
  k->grain_src = body;
  k->time_dependent = true;   // a program may read CURDATE(); a new day is a full pass
  k->declaration = trim(sql);
  return true;
}

bool ThingEngine::grain_boot(Kind& k, std::string* err) {
  auto g = std::make_shared<ThingGrain>();
  grain::install_builtins(g->vm);
  g->vm.retain_globals(grain_allowed());
  g->vm.define_native("read", 1, -1, gb_read);
  g->vm.define_native("read_raw", 1, -1, gb_read_raw);
  g->vm.define_native("read_with", 2, 2, gb_read_with);
  g->vm.define_native("read_raw_with", 2, 2, gb_read_raw_with);
  g->vm.define_native("claims", 2, 2, gb_claims);
  g->vm.define_native("write", 1, -1, gb_write);
  g->vm.define_native("read_target", 1, -1, gb_read_target);
  g->vm.sink = &g->printed;
  g->vm.files = nullptr;
  auto cr = grain::compile_source(k.grain_src, "thing " + k.name);
  if (!cr.ok()) {
    *err = "grain did not compile";
    for (const auto& d : cr.errors) *err += "; line " + std::to_string(d.line) + ": " + d.message;
    return false;
  }
  g->script = cr.script;
  if (!k.params_json.empty()) {
    grain::Value* jp = g->vm.find_global("json_parse");
    std::string e;
    grain::Value pv = g->vm.call(*jp, {grain::Value::string(k.params_json)}, e);
    if (!e.empty()) { *err = "PARAMS is not JSON: " + e; return false; }
    g->vm.define_global("params", pv);
  }
  GrainScope scope(db_, this, k.db, &k.reads, &k.links);
  auto rr = g->vm.run(g->script);
  if (!rr.ok) { *err = "grain: line " + std::to_string(rr.line) + ": " + rr.error; return false; }
  if (k.type == Kind::Event) {
    grain::Value* f = g->vm.find_global("fire");
    if (!f || f->type != grain::Type::Fn) { *err = "an event Thing's program must define fire(ctx)"; return false; }
  } else {
    for (const char* fn : {"enroll", "shared", "things"}) {
      grain::Value* f = g->vm.find_global(fn);
      if (!f || (f->type != grain::Type::Fn)) {
        *err = "the program must define enroll(), shared() and things(keys, shared)";
        return false;
      }
    }
  }
  g->has_touched = g->vm.find_global("touched") != nullptr;
  if (grain::Value* ds = g->vm.find_global("DAY_SAFE")) g->day_safe = ds->truthy();
  k.time_dependent = !g->day_safe;   // a program may read CURDATE(); unless it names its own wakes, a new day is a full pass
  k.grain = g;
  return true;
}

void ThingEngine::grain_notice(Kind& k, const SqlTable& t, const char* op, const SqlRow* before, const SqlRow* after) {
  const std::string tn = lower(t.name);
  auto w = k.watch.find(tn);
  if (w != k.watch.end()) {
    const int c = t.get_column_index(w->second);
    if (c < 0) {
      k.pending_full = true;
      k.full_cause = t.name + " " + op + " (no column " + w->second + ")";
    } else {
      for (const SqlRow* r : {before, after}) {
        if (!r || (size_t)c >= r->values.size() || sql_is_null(r->values[(size_t)c])) continue;
        k.pending[r->values[(size_t)c]] = t.name + " " + op + " " + w->second + " " + r->values[(size_t)c];
      }
    }
  }
  if (k.grain && k.grain->has_touched) {
    k.grain->notices.push_back(ThingGrain::Notice{t.name, op, row_value(t, before), row_value(t, after)});
  } else if (w == k.watch.end()) {
    k.pending_full = true;
    k.full_cause = t.name + " " + op;
  }
}

// Keeps a Thing's place in its kind's wake schedule: out of it, and back in
// at its wake_at if it has one.
void ThingEngine::note_wake(Kind& k, const std::string& ck, const Kind::GInst* g) {
  auto w = k.wake_of.find(ck);
  if (w != k.wake_of.end()) {
    auto t = k.wakes.find(w->second);
    if (t != k.wakes.end()) { t->second.erase(ck); if (t->second.empty()) k.wakes.erase(t); }
    k.wake_of.erase(w);
  }
  if (!g) return;
  for (const auto& f : g->vals)
    if (f.first == "wake_at" && !sql_is_null(f.second) && f.second.size() >= 10) {
      std::string at = f.second.size() == 10 ? f.second + " 00:00:00" : f.second.substr(0, 19);
      k.wakes[at].insert(ck);
      k.wake_of[ck] = at;
    }
}

std::string ThingEngine::grain_display(const Kind::GInst& g) const {
  std::string s;
  for (const auto& kv : g.vals)
    s += (s.empty() ? "" : " | ") + kv.first + "=" + (sql_is_null(kv.second) ? std::string("NULL") : kv.second);
  return s;
}

namespace {
// A Thing's `alert` field, or "" when it has none up.
std::string alert_field(const std::vector<std::pair<std::string, std::string>>& vals) {
  for (const auto& f : vals)
    if (f.first == "alert" && !sql_is_null(f.second) && !f.second.empty() && f.second != "false" && f.second != "0") return f.second;
  return std::string();
}
}  // namespace

bool ThingEngine::put_ginst(Kind& k, const std::string& ck, Kind::GInst fresh, const std::string& cause, uint64_t now) {
  auto it = k.gthings.find(ck);
  const std::string before = it == k.gthings.end() ? std::string() : grain_display(it->second);
  const std::string after = grain_display(fresh);
  if (it != k.gthings.end() && before == after) return false;
  // `alert`: raised when it appears or changes, cleared when it goes.
  const std::string was = it == k.gthings.end() ? std::string() : alert_field(it->second.vals);
  const std::string now_alert = alert_field(fresh.vals);
  if (k.building) {}   // a replacement being built says nothing until it is the Thing
  else if (!now_alert.empty() && now_alert != was) alert(k.name, fresh.key, "raised", now_alert);
  else if (now_alert.empty() && !was.empty()) alert(k.name, fresh.key, "cleared", was);
  if (it == k.gthings.end()) it = k.gthings.emplace(ck, Kind::GInst{}).first;
  Kind::GInst& gi = it->second;
  gi.key = fresh.key;
  gi.vals = std::move(fresh.vals);
  note_wake(k, ck, &gi);
  gi.version++;
  gi.updated_ns = now;
  gi.history.push_back(History{now, cause, before.empty() ? std::string("(not a Thing)") : before, after});
  if (gi.history.size() > kHistoryKeep) gi.history.erase(gi.history.begin());
  k.version++;
  gi.stamp = k.version;
  dirty_ = true;
  return true;
}

void ThingEngine::drop_ginst(Kind& k, const std::string& ck, const std::string& why) {
  note_wake(k, ck, nullptr);
  auto gone = k.gthings.find(ck);
  if (gone == k.gthings.end()) return;
  if (!alert_field(gone->second.vals).empty()) alert(k.name, gone->second.key, "cleared", why);
  k.gthings.erase(gone);
  k.version++;
  dirty_ = true;
}

// AIOS_THINGS_TRACE_TIME=1: each things() and touched() call's time, to stderr
static bool things_trace_time() { static const bool on = [] { const char* e = getenv("AIOS_THINGS_TRACE_TIME"); return e && e[0] == '1'; }(); return on; }

bool ThingEngine::grain_keys(Kind& k, const std::vector<std::string>& keys, const std::string& cause, std::string* err) {
  ThingGrain& g = *k.grain;
  GrainScope scope(db_, this, k.db, &k.reads, &k.links);
  std::vector<grain::Value> kl;
  kl.reserve(keys.size());
  for (const auto& key : keys) kl.push_back(grain::Value::string(key));
  grain::Value out;
  const auto t0 = std::chrono::steady_clock::now();
  if (!gcall(g, "things", {grain::Value::list(std::move(kl)), g.shared}, &out, err)) return false;
  if (things_trace_time())
    fprintf(stderr, "[things-time] %s: things(%zu key(s)) %.3f ms (%s)\n", k.name.c_str(), keys.size(),
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), cause.c_str());
  if (out.type != grain::Type::Map) { *err = "things() must return a map of key -> values"; return false; }
  auto* m = static_cast<grain::MapObj*>(out.obj.get());
  const uint64_t now = db_.kernel_.host().wall_ns();
  std::set<std::string> answered;
  for (const auto& e : m->entries) {
    const std::string ck = sql_collate_key(e.first);
    if (e.second.type == grain::Type::Nil) continue;
    if (e.second.type != grain::Type::Map) { *err = "things(): the value for " + e.first + " is not a map"; return false; }
    answered.insert(ck);
    Kind::GInst fresh;
    fresh.key = e.first;
    for (const auto& f : static_cast<grain::MapObj*>(e.second.obj.get())->entries) {
      fresh.vals.emplace_back(f.first, gval_text(f.second));
      if (std::find(k.gcols.begin(), k.gcols.end(), f.first) == k.gcols.end()) k.gcols.push_back(f.first);
    }
    put_ginst(k, ck, std::move(fresh), cause, now);
  }
  // Asked about and left out: nothing to say about it any more.
  for (const auto& key : keys) {
    const std::string ck = sql_collate_key(key);
    if (answered.count(ck)) continue;
    drop_ginst(k, ck, "no longer a Thing");
  }
  k.reactions += keys.size();
  return true;
}

bool ThingEngine::try_wait_unlocked(const std::string& sql, SqlQueryResult* out) {
  Scan sc(sql);
  const size_t p_w = sc.find_kw("WAIT"), p_th = sc.find_kw("THING"), p_si = sc.find_kw("SINCE"), p_to = sc.find_kw("TIMEOUT");
  if (p_w != 0 || p_th == std::string::npos) return false;
  if (p_si == std::string::npos || !(p_th < p_si) || (p_to != std::string::npos && p_to < p_si)) {
    *out = error_result("WAIT THING <name> SINCE <clock> [TIMEOUT <seconds>]");
    return true;
  }
  const std::string kind = lower(unquote_ident(trim(sql.substr(p_th + 5, p_si - p_th - 5))));
  std::string since_s = trim(sql.substr(p_si + 5, (p_to == std::string::npos ? sql.size() : p_to) - p_si - 5));
  if (!since_s.empty() && since_s.back() == ';') since_s.pop_back();
  const uint64_t since = std::strtoull(since_s.c_str(), nullptr, 10);
  double timeout_s = 30;
  if (p_to != std::string::npos) timeout_s = std::strtod(sql.c_str() + p_to + 7, nullptr);
  timeout_s = std::max(0.0, std::min(timeout_s, 3600.0));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds((int64_t)(timeout_s * 1000));
  for (;;) {
    uint64_t epoch;
    {
      std::lock_guard<std::mutex> g(wait_mu_);
      epoch = wait_epoch_;
    }
    {
      // the clock and what moved, read under the engine's lock (taken for this look only)
      std::lock_guard<aios::hal::Mutex> lk(mu_);
      auto it = kinds_.find(kind);
      if (it == kinds_.end()) { *out = error_result("no Thing kind named " + kind); return true; }
      const Kind& k = it->second;
      if (k.version != since) {
        SqlQueryResult r;
        r.is_result_set = true;
        r.columns = {"clock", "changed", "whole"};
        // A clock behind the one asked about is a new version of the kind (CREATE OR REPLACE): every Thing may differ.
        const bool whole = k.version < since || since == 0 || k.type != Kind::Grain;
        std::string keys = "[";
        if (!whole) {
          bool first = true;
          for (const auto& g : k.gthings)
            if (g.second.stamp > since) { keys += (first ? "" : ",") + thing_json_str(g.second.key); first = false; }
        }
        keys += "]";
        r.rows.push_back(SqlRow{{std::to_string(k.version), keys, whole ? "1" : "0"}});
        if (!sql_wire_results()) r.formatted_table = db_.format_result_table(r.columns, r.rows);
        *out = r;
        return true;
      }
    }
    std::unique_lock<std::mutex> g(wait_mu_);
    if (wait_epoch_ == epoch && !wait_cv_.wait_until(g, deadline, [&] { return wait_epoch_ != epoch; })) {
      SqlQueryResult r;   // the time ran out with nothing changed: the same clock back
      r.is_result_set = true;
      r.columns = {"clock", "changed", "whole"};
      r.rows.push_back(SqlRow{{std::to_string(since), "[]", "0"}});
      if (!sql_wire_results()) r.formatted_table = db_.format_result_table(r.columns, r.rows);
      *out = r;
      return true;
    }
  }
}

bool ThingEngine::try_replace_unlocked(const std::string& sql, SqlQueryResult* out) {
  struct Wake { ThingEngine* e; ~Wake() { e->wake_waiters(); } } wake{this};   // the new version takes the name
  Scan cs(sql);
  const size_t p_cr = cs.find_kw("CREATE"), p_or = cs.find_kw("OR REPLACE"), p_th = cs.find_kw("THING"), p_gr = cs.find_kw("GRAIN");
  if (p_cr != 0 || p_or == std::string::npos || p_th == std::string::npos || p_gr == std::string::npos || !(p_or < p_th && p_th < p_gr))
    return false;
  const std::string stmt = trim(sql.substr(0, p_or)) + " " + trim(sql.substr(p_or + 10));
  Kind k;
  std::string err;
  if (!parse_create(stmt, &k, &err)) { *out = error_result(err); return true; }
  if (k.type != Kind::Grain) return false;
  k.db = db_.effective_database();
  k.created_ns = db_.kernel_.host().wall_ns();
  std::unique_lock<aios::hal::Mutex> lk(mu_);
  auto ex = kinds_.find(k.name);
  if (ex == kinds_.end() || ex->second.type != Kind::Grain) return false;   // a new one: the ordinary CREATE, under its lock
  MuHeld held;
  *out = replace_grain(std::move(k), lk);
  return true;
}

// CREATE OR REPLACE THING for a grain Thing that exists. The new version goes in as <name>~next: paused (nothing
// reacts) but watching, so every change from here on is noted for it. Its first pass runs with the engine's lock
// let go - the old Thing answers, and writes do not wait - and then, under the lock again, it takes the name and
// the changes noted meanwhile go through its touched() like any others: nothing is missed, nothing waited.
SqlQueryResult ThingEngine::replace_grain(Kind&& k, std::unique_lock<aios::hal::Mutex>& lk) {
  const std::string name = k.name, next = name + "~next";
  if (kinds_.count(next)) return error_result("a new version of " + name + " is already being built");
  std::string err;
  for (const auto& l : k.links)
    if (!kinds_.count(l.first)) return error_result("LINKS " + l.first + ": no claims Thing by that name");
  k.declaration = trim(k.declaration);
  k.name = next;
  k.paused = true;
  k.building = true;
  const uint64_t t0 = db_.kernel_.host().wall_ns();
  const bool held_before = g_mu_held;
  // Compiled and its top level run with the lock let go (that may read tables, and a read waits for a writer
  // that may be waiting for this lock); then in, watching, before its first pass reads anything.
  g_mu_held = false;
  lk.unlock();
  const bool booted = grain_boot(k, &err);
  lk.lock();
  if (!booted) { g_mu_held = held_before; return error_result("the new version of " + name + " does not load (the old one stays): " + err); }
  if (kinds_.count(next)) { g_mu_held = held_before; return error_result("a new version of " + name + " is already being built"); }
  auto it = kinds_.emplace(next, std::move(k)).first;
  rebuild_watched();
  Kind* nk = &it->second;   // map nodes stay put; a building kind cannot be dropped
  lk.unlock();
  const bool ok = grain_full(*nk, "replaced", &err, true);
  lk.lock();
  g_mu_held = held_before;
  if (!ok) {
    kinds_.erase(next);
    rebuild_watched();
    return error_result("the new version of " + name + " did not build (the old one stays): " + err);
  }
  auto node = kinds_.extract(next);
  Kind& nkind = node.mapped();
  nkind.name = name;
  nkind.paused = false;
  nkind.building = false;
  // what the old one had come to: its counters carry on
  if (auto old = kinds_.find(name); old != kinds_.end()) {
    nkind.reactions += old->second.reactions;
    nkind.full_recomputes += old->second.full_recomputes;
    nkind.created_ns = old->second.created_ns;
    kinds_.erase(old);
  }
  node.key() = name;
  auto placed = kinds_.insert(std::move(node));
  rebuild_watched();
  dirty_ = true;
  save_locked();
  SqlQueryResult res;
  res.rows_affected = placed.position->second.gthings.size();
  char ms[32];
  std::snprintf(ms, sizeof ms, "%.0f", (double)(db_.kernel_.host().wall_ns() - t0) / 1e6);
  res.formatted_table = "Thing " + name + " replaced: " + std::to_string(placed.position->second.gthings.size()) + " things (" + ms +
                        " ms), the old version answering until now; " + std::to_string(placed.position->second.grain->notices.size()) +
                        " change(s) made meanwhile to follow.";
  return res;
}

bool ThingEngine::grain_full(Kind& k, const std::string& cause, std::string* err, bool keep_notices) {
  if (!k.grain) { *err = "the program is not loaded"; return false; }
  ThingGrain& g = *k.grain;
  const uint64_t t0 = db_.kernel_.host().wall_ns();
  k.day = current_day();
  GrainScope scope(db_, this, k.db, &k.reads, &k.links);
  if (!g.shared_fresh) {
    grain::Value sh;
    if (!gcall(g, "shared", {}, &sh, err)) return false;
    g.shared = sh;
  }
  g.shared_fresh = false;
  g.have_shared = true;
  grain::Value en;
  if (!gcall(g, "enroll", {}, &en, err)) return false;
  if (en.type != grain::Type::List) { *err = "enroll() must return a list of keys"; return false; }
  std::vector<std::string> keys;
  std::set<std::string> enrolled;
  for (const auto& v : en.list_items()) {
    keys.push_back(gval_text(v));
    enrolled.insert(sql_collate_key(keys.back()));
  }
  std::vector<std::string> gone;
  for (const auto& e : k.gthings)
    if (!enrolled.count(e.first)) gone.push_back(e.second.key);
  keys.insert(keys.end(), gone.begin(), gone.end());
  if (!grain_keys(k, keys, cause, err)) return false;
  if (!keep_notices) {
    k.pending.clear();
    k.pending_shared = false;
    g.notices.clear();
  }
  k.full_recomputes++;
  if (!k.grain_error.empty()) alert(k.name, "", "recovered", "a full pass ran clean");
  k.grain_error.clear();
  k.last_full_ms = (double)(db_.kernel_.host().wall_ns() - t0) / 1e6;
  dirty_ = true;
  return true;
}

void ThingEngine::settle_grain(Kind& k) {
  if (!k.grain) return;
  ThingGrain& g = *k.grain;
  std::string err;
  // Things whose moment has come are read again.
  if (!k.wakes.empty()) {
    const std::string now = [] { std::time_t t = std::time(nullptr); std::tm tm_buf{}; localtime_r(&t, &tm_buf); char b[32];
                                 std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm_buf); return std::string(b); }();
    while (!k.wakes.empty() && k.wakes.begin()->first <= now) {
      const std::string at = k.wakes.begin()->first;
      for (const auto& ck : std::set<std::string>(k.wakes.begin()->second)) {
        auto gi = k.gthings.find(ck);
        if (gi != k.gthings.end()) k.pending[gi->second.key] = "woke: " + at;
        note_wake(k, ck, nullptr);
      }
      k.wakes.erase(at);
    }
  }
  if (!k.pending_full) {
    const std::string day = current_day();
    if (!day.empty() && day != k.day) {
      if (g.day_safe) k.day = day;
      else { k.pending_full = true; k.full_cause = "the day turned: " + day; }
    }
  }
  // touched() hears every change, also once a full pass is coming: a program that keeps what it read (an
  // item's inputs, a table it reads whole) learns from it what to read again, in that pass too.
  if (!g.notices.empty()) {
    std::vector<ThingGrain::Notice> ns;
    ns.swap(g.notices);
    GrainScope scope(db_, this, k.db, &k.reads, &k.links);
    for (const auto& n : ns) {
      grain::Value r;
      const std::string cause = n.table + " " + n.op;
      const auto t0 = std::chrono::steady_clock::now();
      const bool ok = gcall(g, "touched", {grain::Value::string(n.table), grain::Value::string(n.op), n.before, n.after, g.shared}, &r, &err);
      if (things_trace_time())
        fprintf(stderr, "[things-time] %s: touched(%s) %.3f ms\n", k.name.c_str(), cause.c_str(),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
      if (!ok) {
        k.pending_full = true;
        k.full_cause = "after touched() failed: " + err;
        break;
      }
      if (k.pending_full || r.type == grain::Type::Nil) continue;
      if (r.type == grain::Type::Str && r.str() == "all") { k.pending_full = true; k.full_cause = cause; continue; }
      grain::Value keys = r;
      if (r.type == grain::Type::Map) {
        auto* m = static_cast<grain::MapObj*>(r.obj.get());
        grain::Value* sh = m->find("shared");
        if (sh && sh->truthy()) k.pending_shared = true;
        grain::Value* kv = m->find("keys");
        keys = kv ? *kv : grain::Value::nil();
      }
      if (keys.type == grain::Type::List) {
        for (const auto& v : keys.list_items()) k.pending[gval_text(v)] = cause;
      } else if (keys.type != grain::Type::Nil) {
        k.pending_full = true;
        k.full_cause = "touched() answered neither nil, keys, {keys, shared} nor \"all\"";
      }
    }
  }
  if (!k.pending_full && k.pending_shared) {
    k.pending_shared = false;
    GrainScope scope(db_, this, k.db, &k.reads, &k.links);
    grain::Value sh;
    if (!gcall(g, "shared", {}, &sh, &err)) {
      k.pending_full = true;
      k.full_cause = "after shared() failed: " + err;
    } else if (!g.have_shared || !sh.equals(g.shared)) {
      g.shared = sh;
      g.shared_fresh = true;
      k.pending_full = true;
      k.full_cause = "what every item shares moved";
    }
  }
  if (k.pending_full) {
    const std::string cause = k.full_cause.empty() ? std::string("full recompute") : k.full_cause;
    k.pending_full = false;
    k.full_cause.clear();
    if (!grain_full(k, cause, &err)) {
      k.grain_error = err;
      alert(k.name, "", "error", err);
      fprintf(stderr, "[things] %s: full pass failed: %s\n", k.name.c_str(), err.c_str());
    }
    return;
  }
  if (k.pending.empty()) return;
  std::vector<std::pair<std::string, std::string>> todo(k.pending.begin(), k.pending.end());
  k.pending.clear();
  std::sort(todo.begin(), todo.end());
  for (size_t at = 0; at < todo.size(); at += kPartitionChunk) {
    const size_t end = std::min(todo.size(), at + kPartitionChunk);
    std::vector<std::string> keys;
    for (size_t i = at; i < end; ++i) keys.push_back(todo[i].first);
    const std::string cause = end - at == 1 ? todo[at].second
                                            : todo[at].second + " (+" + std::to_string(end - at - 1) + " more)";
    if (!grain_keys(k, keys, cause, &err)) {
      k.grain_error = err;
      alert(k.name, "", "error", err);
      fprintf(stderr, "[things] %s: reaction failed (%s); a full pass next\n", k.name.c_str(), err.c_str());
      k.pending_full = true;
      k.full_cause = "after a failed reaction";
      return;
    }
  }
}

// SHOW THING's columns for a grain kind: its key, every field seen, then the bookkeeping.
std::vector<std::string> ThingEngine::grain_columns(const Kind& k) {
  std::vector<std::string> cols;
  cols.push_back(k.key_col);
  for (const auto& c : k.gcols) cols.push_back(c);
  for (const char* c : {"version", "updated_at", "last_change"}) cols.push_back(c);
  return cols;
}

// One Thing's row. Most Things were written in the same second (a pass writes them all): each second is
// formatted once, not per row (last_sec / last_text carry it from row to row).
SqlRow ThingEngine::grain_row(const Kind& k, const Kind::GInst& gi, uint64_t* last_sec, std::string* last_text) {
  SqlRow row;
  row.values.reserve(k.gcols.size() + 4);
  row.values.push_back(gi.key);
  for (const auto& c : k.gcols) {
    std::string v = sql_null();
    for (const auto& f : gi.vals)
      if (f.first == c) { v = f.second; break; }
    row.values.push_back(std::move(v));
  }
  row.values.push_back(std::to_string(gi.version));
  const uint64_t sec = gi.updated_ns / 1000000000ull;
  if (gi.updated_ns == 0) row.values.push_back(sql_null());
  else {
    if (sec != *last_sec || last_text->empty()) { *last_sec = sec; *last_text = wall_text(gi.updated_ns); }
    row.values.push_back(*last_text);
  }
  row.values.push_back(gi.history.empty() ? sql_null() : gi.history.back().cause);
  return row;
}

std::shared_ptr<const std::vector<SqlRow>> ThingEngine::grain_rows_locked(Kind& k) {
  // Every change to a grain kind's Things (put_ginst, drop_ginst) moves k.version; a replacement is a new Kind.
  if (k.shown_rows && k.shown_version == k.version) return k.shown_rows;
  auto rows = std::make_shared<std::vector<SqlRow>>();
  rows->reserve(k.gthings.size());
  uint64_t last_sec = ~0ull;
  std::string last_text;
  for (const auto& kv : k.gthings) rows->push_back(grain_row(k, kv.second, &last_sec, &last_text));
  k.shown_rows = std::move(rows);
  k.shown_version = k.version;
  return k.shown_rows;
}

SqlQueryResult ThingEngine::show_grain(const Kind& k, const std::vector<std::string>* keys) {
  SqlQueryResult res;
  res.is_result_set = true;
  res.columns = grain_columns(k);
  if (!keys) {
    res.rows = *grain_rows_locked(const_cast<Kind&>(k));
  } else {
    // KEYS: a lookup per key asked (a key named twice is one row, a key that is no Thing is none), not a walk over all
    std::set<std::string> seen;
    uint64_t last_sec = ~0ull;
    std::string last_text;
    for (const auto& want : *keys) {
      const std::string ck = sql_collate_key(want);
      if (!seen.insert(ck).second) continue;
      auto f = k.gthings.find(ck);
      if (f != k.gthings.end()) res.rows.push_back(grain_row(k, f->second, &last_sec, &last_text));
    }
  }
  if (!sql_wire_results()) res.formatted_table = db_.format_result_table(res.columns, res.rows);
  return res;
}

std::string ThingEngine::grain_serialize(const Kind& k) const {
  std::string o;
  if (k.grain && k.grain->have_shared) {
    grain::Value* js = k.grain->vm.find_global("json_stringify");
    std::string e;
    grain::Value txt = js ? k.grain->vm.call(*js, {k.grain->shared}, e) : grain::Value::nil();
    if (e.empty() && txt.type == grain::Type::Str) o += "gshared\t" + esc(txt.str()) + "\n";
  }
  o += "gcols";
  for (const auto& c : k.gcols) o += "\t" + esc(c);
  o += "\n";
  for (const auto& kv : k.gthings) {
    const Kind::GInst& gi = kv.second;
    o += "g\t" + esc(gi.key) + "\t" + std::to_string(gi.version) + "\t" + std::to_string(gi.updated_ns) + "\t" +
         std::to_string(gi.vals.size());
    for (const auto& f : gi.vals) o += "\t" + esc(f.first) + "\t" + val_field(f.second);
    o += "\n";
    for (const History& h : gi.history)
      o += "hist\t" + std::to_string(h.wall_ns) + "\t" + esc(h.cause) + "\t" + esc(h.before) + "\t" + esc(h.after) + "\n";
  }
  return o;
}

void ThingEngine::grain_restore(Kind& k) {
  for (auto& g : k.gthings) note_wake(k, g.first, &g.second);
  // shared() as it was saved: the redo replayed at boot brings notices on top
  // of it. Without it (an older file), a full pass puts it back.
  ThingGrain& g = *k.grain;
  grain::Value* jp = g.vm.find_global("json_parse");
  std::string e;
  if (!k.shared_json.empty() && jp) {
    grain::Value v = g.vm.call(*jp, {grain::Value::string(k.shared_json)}, e);
    if (e.empty()) { g.shared = v; g.have_shared = true; }
  }
  k.shared_json.clear();
  if (!g.have_shared) { k.pending_full = true; k.full_cause = "loaded without its shared state"; }
}

// CALL THING kind('<json>'): the program's report(input) over the tables as
// they are now, for inputs the live Things do not hold - the ERP's report
// form passes its $input as JSON. It runs in a VM of its own, compiled for
// this call, so it shares nothing with the live Things or another call, and
// holds no engine lock while it reads.
SqlQueryResult ThingEngine::call_report(const std::string& sql) {
  Scan sc(sql);
  std::string rest = trim(sql.substr(sc.find_kw("THING") + 5));
  if (!rest.empty() && rest.back() == ';') rest = trim(rest.substr(0, rest.size() - 1));
  const size_t lp = rest.find('(');
  if (lp == std::string::npos || rest.empty() || rest.back() != ')')
    return error_result("CALL THING <name>('<json input>')");
  std::string name = lower(unquote_ident(rest.substr(0, lp)));
  // CALL THING kind.fn(...) calls another of the program's functions; report by default.
  std::string fn = "report";
  const size_t dot = name.find('.');
  if (dot != std::string::npos) {
    fn = name.substr(dot + 1);
    name = name.substr(0, dot);
    for (char ch : fn)
      if (!std::isalnum((unsigned char)ch) && ch != '_') return error_result("CALL THING " + name + ": bad function name " + fn);
    for (const char* hook : {"enroll", "shared", "things", "touched", "fire", "cleanup"})
      if (fn == hook) return error_result("CALL THING " + name + "." + fn + ": that is the live Thing's own hook");
  }
  std::string arg = trim(rest.substr(lp + 1, rest.size() - lp - 2));
  std::string json;
  if (!arg.empty()) {
    if (arg.size() < 2 || (arg.front() != '\'' && arg.front() != '"') || arg.back() != arg.front())
      return error_result("CALL THING " + name + ": the input goes in one quoted string");
    const char q = arg.front();
    for (size_t i = 1; i + 1 < arg.size(); ++i) {
      if (arg[i] == q && i + 2 < arg.size() && arg[i + 1] == q) { json += q; ++i; continue; }
      if (arg[i] == '\\' && i + 2 < arg.size()) {
        const char n = arg[++i];
        json += n == 'n' ? '\n' : n == 't' ? '\t' : n == 'r' ? '\r' : n == '0' ? '\0' : n;
        continue;
      }
      json += arg[i];
    }
  }
  Kind tmp;
  {
    std::lock_guard<aios::hal::Mutex> lk(mu_);
    auto it = kinds_.find(name);
    if (it == kinds_.end() || (it->second.type != Kind::Grain && it->second.type != Kind::Event))
      return error_result("no grain Thing named " + name);
    tmp.type = it->second.type;
    tmp.params_json = it->second.params_json;
    tmp.name = it->second.name;
    tmp.grain_src = it->second.grain_src;
    tmp.db = it->second.db;
    tmp.reads = it->second.reads;
    tmp.links = it->second.links;
  }
  std::string err;
  if (!grain_boot(tmp, &err)) return error_result(err);
  ThingGrain& g = *tmp.grain;
  grain::Value* rf = g.vm.find_global(fn);
  if (!rf || rf->type != grain::Type::Fn) return error_result("Thing " + name + " defines no " + fn + "(input)");
  GrainScope scope(db_, this, tmp.db, &tmp.reads, &tmp.links);
  grain::Value input;
  if (!json.empty()) {
    grain::Value* jp = g.vm.find_global("json_parse");
    std::string e;
    input = g.vm.call(*jp, {grain::Value::string(json)}, e);
    if (!e.empty()) return error_result("CALL THING " + name + ": the input is not JSON: " + e);
  }
  grain::Value out;
  const uint64_t t0 = db_.kernel_.host().wall_ns();
  if (!gcall(g, fn.c_str(), {input}, &out, &err)) return error_result(err);
  if (out.type != grain::Type::List) return error_result(fn + "() must return a list of rows (maps)");
  SqlQueryResult res;
  res.is_result_set = true;
  std::unordered_map<std::string, size_t> col_at;
  for (const auto& row : out.list_items()) {
    if (row.type != grain::Type::Map) return error_result("report(): every row must be a map");
    for (const auto& f : static_cast<grain::MapObj*>(row.obj.get())->entries)
      if (col_at.emplace(f.first, res.columns.size()).second) res.columns.push_back(f.first);
  }
  for (const auto& row : out.list_items()) {
    SqlRow r;
    r.values.assign(res.columns.size(), sql_null());
    for (const auto& f : static_cast<grain::MapObj*>(row.obj.get())->entries) r.values[col_at[f.first]] = gval_text(f.second);
    res.rows.push_back(std::move(r));
  }
  fprintf(stderr, "[things] CALL THING %s.%s: %zu rows in %.0f ms, %llu reads\n", name.c_str(), fn.c_str(), res.rows.size(),
          (double)(db_.kernel_.host().wall_ns() - t0) / 1e6, (unsigned long long)scope.ctx.queries);
  if (!sql_wire_results()) res.formatted_table = db_.format_result_table(res.columns, res.rows);
  return res;
}

// ---- grain reading another Thing -------------------------------------------
//
// claims(kind, keys): a LINKed claims Thing's queues for the resource keys
// given (each a list of the resource's key values, in its key order). One
// queue per key, in the order asked:
//   {k, cap (nil when no row), lines: [{order, line, need, alloc, ahead, prio}]}
// lines in queue order (ahead = what the lines before it need), and
//   orders: {orderno: [{k, line, need, alloc}]} - every line of every order
// in those queues, so a caller can tell which orders a change would complete.
struct ThingGrainAccess {
  static grain::Value dec_value(const ThingDec& d) { return grain::Value::real(std::strtod(dec_plain(d).c_str(), nullptr)); }
  static grain::Value claims(ThingEngine& e, const std::string& name, const grain::Value& keys, std::string& error) {
    std::unique_lock<aios::hal::Mutex> lk(e.mu_, std::defer_lock);
    if (!g_mu_held) lk.lock();
    auto it = e.kinds_.find(lower(name));
    if (it == e.kinds_.end() || it->second.type != ThingEngine::Kind::Claims) { error = "claims(): no claims Thing named " + name; return grain::Value::nil(); }
    const ThingEngine::Kind& c = it->second;
    auto rit = e.kinds_.find(c.resource);
    if (rit == e.kinds_.end()) { error = "claims(): " + name + " has lost its resource"; return grain::Value::nil(); }
    const ThingEngine::Kind& r = rit->second;
    std::vector<grain::Value> queues;
    std::set<std::string> order_ids;
    for (const auto& kv : keys.list_items()) {
      if (kv.type != grain::Type::List || kv.list_items().size() != r.key_cols.size()) {
        error = "claims(): each key is a list of " + std::to_string(r.key_cols.size()) + " value(s)";
        return grain::Value::nil();
      }
      std::vector<std::string> vals;
      for (const auto& v : kv.list_items()) vals.push_back(gval_text(v));
      const std::string rk = e.reskey_of(vals);
      auto q = std::make_shared<grain::MapObj>();
      q->put("k", grain::Value::string(rk));
      auto ri = r.res.find(rk);
      q->put("cap", ri != r.res.end() && ri->second.cap_known ? dec_value(ri->second.cap) : grain::Value::nil());
      std::vector<grain::Value> lines;
      auto qi = c.queue_index.find(rk);
      if (qi != c.queue_index.end()) {
        ThingDec ahead{};
        for (const auto& ent : qi->second) {
          auto oi = c.orders.find(ent.first);
          if (oi == c.orders.end() || ent.second >= oi->second.lines.size()) continue;
          const auto& ln = oi->second.lines[ent.second];
          auto m = std::make_shared<grain::MapObj>();
          m->put("order", grain::Value::string(ent.first));
          m->put("line", grain::Value::string(ln.lineid));
          m->put("need", dec_value(ln.need));
          m->put("alloc", dec_value(ln.alloc));
          m->put("ahead", dec_value(ahead));
          std::vector<grain::Value> prio;
          for (const auto& p : ln.prio) prio.push_back(sql_is_null(p) ? grain::Value::nil() : grain::Value::string(p));
          m->put("prio", grain::Value::list(std::move(prio)));
          lines.push_back(grain::Value::object(grain::Type::Map, m));
          dec_add(&ahead, ln.need, +1);
          order_ids.insert(ent.first);
        }
      }
      q->put("lines", grain::Value::list(std::move(lines)));
      queues.push_back(grain::Value::object(grain::Type::Map, q));
    }
    auto orders = std::make_shared<grain::MapObj>();
    for (const auto& id : order_ids) {
      auto oi = c.orders.find(id);
      if (oi == c.orders.end()) continue;
      std::vector<grain::Value> ls;
      for (const auto& ln : oi->second.lines) {
        auto m = std::make_shared<grain::MapObj>();
        m->put("k", grain::Value::string(ln.reskey));
        m->put("line", grain::Value::string(ln.lineid));
        m->put("need", dec_value(ln.need));
        m->put("alloc", dec_value(ln.alloc));
        ls.push_back(grain::Value::object(grain::Type::Map, m));
      }
      orders->put(id, grain::Value::list(std::move(ls)));
    }
    auto out = std::make_shared<grain::MapObj>();
    out->put("queues", grain::Value::list(std::move(queues)));
    out->put("orders", grain::Value::object(grain::Type::Map, orders));
    return grain::Value::object(grain::Type::Map, out);
  }
};

namespace {
grain::Value gb_claims(grain::Vm&, std::vector<grain::Value>& args, std::string& error) {
  if (!g_grain || !g_grain->eng) { error = "claims() only runs inside a Thing"; return grain::Value::nil(); }
  if (args[0].type != grain::Type::Str || args[1].type != grain::Type::List) { error = "claims(kind, [[key values], ...])"; return grain::Value::nil(); }
  if (!g_grain->links || !g_grain->links->count(lower(args[0].str()))) {
    error = "claims(): " + args[0].str() + " is not in this Thing's LINKS";
    return grain::Value::nil();
  }
  return ThingGrainAccess::claims(*g_grain->eng, args[0].str(), args[1], error);
}
}  // namespace

// ---- Things that act once ----------------------------------------------------
//
//   CREATE THING post_prices_oct AT '2026-10-01 00:00:00' GRAIN
//     READS temp_pb_..., stockmaster, prices, pricebook_staging_session
//     WRITES prices, pricebook_match_cache, temp_pb_..., pricebook_staging_session
//     PARAMS '{"table": "temp_pb_...", ...}'
//     AS '<grain: fire(ctx), and optionally preview(input), verify(ctx, fired), cleanup(ctx, fired)>'
//
// Until its time it is a plan anyone can look at (CALL THING name.preview()).
// At its time the engine's clock fires it: state firing (saved first), fire,
// verify, cleanup, a receipt under things/receipts, and the Thing removes
// itself. A crash mid-fire leaves it firing: the next start fires it again,
// so fire() must be safe to repeat (the ERP's posted_at marks make the price
// post so). Time passed while the machine was down: it fires at the start.
bool ThingEngine::parse_event(const std::string& sql, Kind* k, std::string* err) const {
  Scan sc(sql);
  const size_t p_thing = sc.find_kw("THING"), p_at = sc.find_kw("AT"), p_gr = sc.find_kw("GRAIN"),
               p_reads = sc.find_kw("READS"), p_writes = sc.find_kw("WRITES"), p_params = sc.find_kw("PARAMS");
  const size_t p_as = p_reads == std::string::npos ? std::string::npos : sc.find_kw("AS", p_reads);
  if (p_thing == std::string::npos || p_at == std::string::npos || p_gr == std::string::npos || p_reads == std::string::npos ||
      p_writes == std::string::npos || p_as == std::string::npos ||
      !(p_thing < p_at && p_at < p_gr && p_gr < p_reads && p_reads < p_writes && p_writes < p_as) ||
      (p_params != std::string::npos && !(p_writes < p_params && p_params < p_as))) {
    *err = "CREATE THING <name> AT '<YYYY-MM-DD HH:MM:SS>' GRAIN READS <table>[, ...] WRITES <table>[, ...] [PARAMS '<json>'] AS '<grain program>'";
    return false;
  }
  auto quoted = [](std::string t, std::string* out) {
    t = trim(t);
    if (t.size() < 2 || (t.front() != '\'' && t.front() != '"') || t.back() != t.front()) return false;
    const char q = t.front();
    out->clear();
    for (size_t i = 1; i + 1 < t.size(); ++i) {
      if (t[i] == q && i + 2 < t.size() && t[i + 1] == q) { *out += q; ++i; continue; }
      if (t[i] == '\\' && i + 2 < t.size()) { const char n = t[++i]; *out += n == 'n' ? '\n' : n == 't' ? '\t' : n; continue; }
      *out += t[i];
    }
    return true;
  };
  k->type = Kind::Event;
  k->name = lower(unquote_ident(sql.substr(p_thing + 5, p_at - p_thing - 5)));
  if (!quoted(sql.substr(p_at + 2, p_gr - p_at - 2), &k->at)) { *err = "AT wants a quoted 'YYYY-MM-DD HH:MM:SS'"; return false; }
  if (k->at.size() == 10) k->at += " 00:00:00";
  if (k->at.size() != 19 || k->at[4] != '-' || k->at[7] != '-' || k->at[10] != ' ' || k->at[13] != ':' || k->at[16] != ':') {
    *err = "AT wants 'YYYY-MM-DD HH:MM:SS' (local time), got " + k->at;
    return false;
  }
  auto names = [&](size_t from, size_t to, std::vector<std::string>* out) {
    for (const std::string& r : sc.split_commas(from, to)) {
      std::string t = lower(unquote_ident(r));
      const size_t dot = t.find('.');
      if (dot != std::string::npos) t = t.substr(dot + 1);
      if (!t.empty()) out->push_back(t);
    }
  };
  names(p_reads + 5, p_writes, &k->reads);
  size_t writes_end = p_params == std::string::npos ? p_as : p_params;
  // WRITES t1, t2 TO <profile>: the tables are a real database's, reached
  // through that connection profile.
  const size_t p_to = sc.find_kw("TO", p_writes);
  if (p_to != std::string::npos && p_to < writes_end) {
    k->write_target = unquote_ident(trim(sql.substr(p_to + 2, writes_end - p_to - 2)));
    if (k->write_target.empty()) { *err = "WRITES ... TO <connection profile id>"; return false; }
    writes_end = p_to;
  }
  names(p_writes + 6, writes_end, &k->writes);
  if (k->write_target.empty())
    for (const auto& w : k->writes)
      if (std::find(k->reads.begin(), k->reads.end(), w) == k->reads.end()) k->reads.push_back(w);
  if (p_params != std::string::npos && !quoted(sql.substr(p_params + 6, p_as - p_params - 6), &k->params_json)) {
    *err = "PARAMS wants one quoted JSON string";
    return false;
  }
  std::string body;
  if (!quoted(sql.substr(p_as + 2), &body)) {
    std::string src = trim(sql.substr(p_as + 2));
    if (!src.empty() && src.back() == ';') src = trim(src.substr(0, src.size() - 1));
    if (!quoted(src, &body)) { *err = "the program goes in one quoted string: AS '...'"; return false; }
  }
  if (k->name.empty() || k->writes.empty() || trim(body).empty()) { *err = "an event Thing needs a name, WRITES and a program"; return false; }
  k->grain_src = body;
  k->declaration = trim(sql);
  return true;
}

namespace {
std::string local_now_text() {
  std::time_t t = std::time(nullptr);
  std::tm tm_buf{};
  localtime_r(&t, &tm_buf);
  char b[32];
  std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm_buf);
  return b;
}
}  // namespace

bool ThingEngine::due_now() {
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  if (kinds_.empty()) return false;
  const std::string now = local_now_text();
  const std::string today = now.substr(0, 10);
  for (const auto& kv : kinds_) {
    const Kind& k = kv.second;
    if (k.paused) continue;
    if (k.type == Kind::Event && k.state != "failed" && k.at <= now && !firing_now_.count(k.name)) return true;
    if (k.type == Kind::Grain && !k.wakes.empty() && k.wakes.begin()->first <= now) return true;
    if (k.type == Kind::Mirror && k.next_pull_ns <= db_.kernel_.host().wall_ns()) return true;
    // the ones that wait on the date: a new day is theirs to see even when
    // nothing is written (a quiet mirror, a scratch copy with no binlog)
    if (k.time_dependent && !k.day.empty() && k.day < today) return true;
  }
  return false;
}

void ThingEngine::fire_due() {
  struct Due { std::string name; Kind copy; };
  std::vector<Due> due;
  const std::string now = local_now_text();
  {
    std::lock_guard<aios::hal::Mutex> lk(mu_);
    MuHeld held;
    for (auto& kv : kinds_) {
      Kind& k = kv.second;
      if (k.type != Kind::Event || k.paused || k.state == "failed" || k.at > now || firing_now_.count(k.name)) continue;
      const char* gate = std::getenv("AIOS_THING_WRITES");
      if (!gate || std::string(gate) != "1") {
        k.state = "failed";
        k.fire_error = "this process was not started with AIOS_THING_WRITES=1";
        alert(k.name, "", "failed", k.fire_error);
        dirty_ = true;
        continue;
      }
      k.state = "firing";   // on disk before a single write: a crash fires it again
      firing_now_.insert(k.name);
      dirty_ = true;
      Due d;
      d.name = k.name;
      d.copy.type = Kind::Event;
      d.copy.name = k.name;
      d.copy.db = k.db;
      d.copy.reads = k.reads;
      d.copy.writes = k.writes;
      d.copy.write_target = k.write_target;
      d.copy.params_json = k.params_json;
      d.copy.grain_src = k.grain_src;
      d.copy.at = k.at;
      due.push_back(std::move(d));
    }
    save_locked();
  }
  for (auto& d : due) {
    std::string err;
    grain::Value fired, verified, cleaned;
    const uint64_t t0 = db_.kernel_.host().wall_ns();
    bool ok = grain_boot(d.copy, &err);
    uint64_t writes = 0;
    // WRITES ... TO <profile>: one connection to that database, and fire() as
    // one transaction on it - an error rolls it all back, and a crash leaves
    // it uncommitted (the server drops it with the connection), so the
    // firing that follows at start begins from what was there before.
    std::unique_ptr<MariaWireSession> target;
    auto target_sql = [&](const char* stmt) -> bool {
      auto r = target->query(stmt);
      if (!r.ok()) { err = std::string(stmt) + " on " + d.copy.write_target + ": " + r.error().message; return false; }
      if (!r->ok) { err = std::string(stmt) + " on " + d.copy.write_target + ": " + r->error_message; return false; }
      return true;
    };
    if (ok && !d.copy.write_target.empty()) {
      DbConnectionProfile prof;
      bool found = false;
      for (const auto& p : db_.list_profiles()) if (p.id == d.copy.write_target) { prof = p; found = true; }
      if (!found) { ok = false; err = "WRITES TO " + d.copy.write_target + ": no such connection profile"; }
      else if (!prof.things_write) { ok = false; err = "WRITES TO " + d.copy.write_target + ": the profile is not marked things_write=1"; }
      else if (auto t = sql_profile_transport_ok(prof); !t.ok()) { ok = false; err = "WRITES TO " + d.copy.write_target + ": " + t.error().message; }
      else {
        target = std::make_unique<MariaWireSession>(sql_profile_dial_host(prof), prof.port);
        auto c = target->connect(prof.user, prof.password, prof.database);
        if (!c.ok()) { ok = false; err = "WRITES TO " + d.copy.write_target + ": cannot connect: " + c.error().message; }
        else ok = target_sql("START TRANSACTION");
      }
    }
    if (ok) {
      ThingGrain& g = *d.copy.grain;
      GrainScope scope(db_, this, d.copy.db, &d.copy.reads, &d.copy.links);
      scope.ctx.writes = &d.copy.writes;
      scope.ctx.target = target.get();
      auto ctx = std::make_shared<grain::MapObj>();
      ctx->put("at", grain::Value::string(d.copy.at));
      ctx->put("now", grain::Value::string(now));
      ctx->put("today", grain::Value::string(now.substr(0, 10)));
      grain::Value cv = grain::Value::object(grain::Type::Map, ctx);
      ok = gcall(g, "fire", {cv}, &fired, &err);
      if (target) {
        if (ok) ok = target_sql("COMMIT");
        else { std::string keep = err; target_sql("ROLLBACK"); err = keep + " (rolled back on " + d.copy.write_target + ")"; }
      }
      if (ok && g.vm.find_global("verify")) {
        scope.ctx.writes = nullptr;   // verify only reads
        ok = gcall(g, "verify", {cv, fired}, &verified, &err);
        if (ok && verified.type == grain::Type::Map) {
          grain::Value* okv = static_cast<grain::MapObj*>(verified.obj.get())->find("ok");
          if (okv && !okv->truthy()) { ok = false; err = "verify() found the result wrong: " + verified.repr(); }
        }
      }
      if (ok && g.vm.find_global("cleanup")) {
        scope.ctx.writes = &d.copy.writes;
        if (target) ok = target_sql("START TRANSACTION");
        if (ok) ok = gcall(g, "cleanup", {cv, fired}, &cleaned, &err);
        if (target) {
          if (ok) ok = target_sql("COMMIT");
          else { std::string keep = err; target_sql("ROLLBACK"); err = keep + " (cleanup rolled back)"; }
        }
      }
      writes = scope.ctx.writes_done;
    }
    const double ms = (double)(db_.kernel_.host().wall_ns() - t0) / 1e6;
    std::lock_guard<aios::hal::Mutex> lk(mu_);
    MuHeld held;
    firing_now_.erase(d.name);
    auto it = kinds_.find(d.name);
    if (it == kinds_.end()) continue;
    if (!ok) {
      it->second.state = "failed";
      it->second.fire_error = err;
      alert(d.name, "", "failed", err);
      dirty_ = true;
      save_locked();
      fprintf(stderr, "[things] %s: firing failed: %s\n", d.name.c_str(), err.c_str());
      continue;
    }
    // The receipt, then the Thing is gone.
    auto& host = db_.kernel_.host();
    host.make_dir(things_dir());
    host.make_dir(things_dir() + "/receipts");
    std::string r = "AIOSRECEIPT 1\nthing\t" + esc(d.name) + "\nat\t" + esc(d.copy.at) + "\nfired\t" + esc(now) +
                    "\nms\t" + std::to_string((long long)ms) + "\nwrites\t" + std::to_string(writes) +
                    (d.copy.write_target.empty() ? std::string() : "\ntarget\t" + esc(d.copy.write_target)) +
                    "\nfire\t" + esc(fired.repr()) + "\nverify\t" + esc(verified.repr()) + "\ncleanup\t" + esc(cleaned.repr()) +
                    "\ndeclaration\t" + esc(it->second.declaration) + "\nend\n";
    host.write_file(things_dir() + "/receipts/" + d.name + ".receipt", ByteSpan((const uint8_t*)r.data(), r.size()));
    alert(d.name, "", "fired", "due " + d.copy.at + ", fired " + now + ": " + std::to_string(writes) + " writes" +
          (d.copy.write_target.empty() ? std::string() : " on " + d.copy.write_target) + " in " +
          std::to_string((long long)ms) + " ms" + (verified.type == grain::Type::Nil ? "" : ", verified"));
    kinds_.erase(it);
    host.remove(things_dir() + "/" + d.name + ".thing");
    rebuild_watched();
    dirty_ = true;
    save_locked();
    fprintf(stderr, "[things] %s fired at %s (due %s): %llu writes in %.0f ms; receipt kept, Thing removed\n", d.name.c_str(),
            now.c_str(), d.copy.at.c_str(), (unsigned long long)writes, ms);
  }
}

SqlQueryResult ThingEngine::show_receipts() {
  SqlQueryResult res;
  res.is_result_set = true;
  res.columns = {"thing", "at", "fired", "ms", "writes", "target", "fire", "verify", "cleanup"};
  auto& host = db_.kernel_.host();
  const std::string dir = things_dir() + "/receipts";
  if (host.exists(dir)) {
    auto names = host.list_dir(dir);
    if (names.ok()) {
      for (const std::string& n : *names) {
        auto f = host.read_file(dir + "/" + n);
        if (!f.ok()) continue;
        std::map<std::string, std::string> kv;
        std::string text(f->begin(), f->end());
        size_t at = 0;
        while (at < text.size()) {
          size_t nl = text.find('\n', at);
          std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
          at = nl == std::string::npos ? text.size() : nl + 1;
          auto fs = split_tabs(line);
          if (fs.size() >= 2) kv[fs[0]] = unesc(fs[1]);
        }
        res.rows.push_back(SqlRow{{kv["thing"], kv["at"], kv["fired"], kv["ms"], kv["writes"], kv.count("target") ? kv["target"] : std::string("ASQL"), kv["fire"], kv["verify"], kv["cleanup"]}});
      }
    }
  }
  if (!sql_wire_results()) res.formatted_table = db_.format_result_table(res.columns, res.rows);
  return res;
}


// ---- Alerts -----------------------------------------------------------------

namespace {
constexpr size_t kAlertsKeep = 1000;   // in memory and, after a rewrite, on disk
bool alert_active(const std::string& state) {
  return state == "raised" || state == "changed" || state == "error" || state == "failed";
}
std::string json_str(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else if (c == '\t') o += "\\t";
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += (char)c;
  }
  return o + "\"";
}
}  // namespace

// state: "raised" (an alert field appeared or changed - recorded as "changed"
// when one was already up), "cleared", "error" / "recovered" (the kind's
// program), "failed" (an event), "fired", "retried". A repeat of what is
// already recorded is nothing new: a reaction replayed from the redo log after
// a crash re-raises exactly what it raised the first time, and is dropped here.
void ThingEngine::alert(const std::string& thing, const std::string& key, const std::string& state_in, const std::string& text) {
  const std::string lk = thing + "\x01" + key;
  auto last = alert_last_.find(lk);
  const bool up = last != alert_last_.end() && alert_active(last->second.first);
  std::string state = state_in;
  if (state == "raised" || state == "error" || state == "failed") {
    if (up && last->second.second == text) return;
    if (up && state == "raised") state = "changed";
  } else if (state == "cleared" || state == "recovered" || state == "retried") {
    if (!up) return;
  }
  Alert a;
  a.seq = ++alert_seq_;
  a.at = local_now_text();
  a.thing = thing;
  a.key = key;
  a.state = state;
  a.text = text;
  alert_last_[lk] = {state, text};
  auto& host = db_.kernel_.host();
  host.make_dir(things_dir());
  const std::string line = std::to_string(a.seq) + "\t" + esc(a.at) + "\t" + esc(a.thing) + "\t" + esc(a.key) + "\t" +
                           esc(a.state) + "\t" + esc(a.text) + "\n";
  alerts_.push_back(a);
  if (alerts_.size() > kAlertsKeep) alerts_.erase(alerts_.begin(), alerts_.begin() + (alerts_.size() - kAlertsKeep));
  if (++alert_log_lines_ > 2 * kAlertsKeep) {
    // The log is kept to the alerts still held.
    std::string all;
    for (const auto& x : alerts_)
      all += std::to_string(x.seq) + "\t" + esc(x.at) + "\t" + esc(x.thing) + "\t" + esc(x.key) + "\t" + esc(x.state) + "\t" + esc(x.text) + "\n";
    host.write_file(things_dir() + "/alerts.log", ByteSpan((const uint8_t*)all.data(), all.size()));
    alert_log_lines_ = alerts_.size();
  } else {
    host.append_file(things_dir() + "/alerts.log", ByteSpan((const uint8_t*)line.data(), line.size()));
  }
  // On the machine's own console, where whoever is at it sees it.
  std::string say = "[things] ALERT " + a.state + " " + a.thing + (a.key.empty() ? "" : " " + a.key) +
                    (a.text.empty() ? "" : ": " + a.text) + "\n";
#if defined(AIOS_METAL) && defined(__aarch64__)
  kputs(say.c_str());
#else
  std::fputs(say.c_str(), stderr);
#endif
}

void ThingEngine::load_alerts() {
  auto& host = db_.kernel_.host();
  alerts_.clear();
  alert_last_.clear();
  alert_log_lines_ = 0;
  auto f = host.read_file(things_dir() + "/alerts.log");
  if (f.ok()) {
    std::string text(f->begin(), f->end());
    size_t at = 0;
    while (at < text.size()) {
      size_t nl = text.find('\n', at);
      if (nl == std::string::npos) break;   // a line cut off by a crash was never recorded
      auto fs = split_tabs(text.substr(at, nl - at));
      at = nl + 1;
      if (fs.size() < 6) continue;
      Alert a;
      a.seq = std::strtoull(fs[0].c_str(), nullptr, 10);
      a.at = unesc(fs[1]); a.thing = unesc(fs[2]); a.key = unesc(fs[3]); a.state = unesc(fs[4]); a.text = unesc(fs[5]);
      if (a.seq <= alert_seq_) continue;
      alert_seq_ = a.seq;
      alert_last_[a.thing + "\x01" + a.key] = {a.state, a.text};
      alerts_.push_back(a);
      alert_log_lines_++;
    }
    if (alerts_.size() > kAlertsKeep) alerts_.erase(alerts_.begin(), alerts_.begin() + (alerts_.size() - kAlertsKeep));
  }
  auto c = host.read_file(things_dir() + "/alerts.cfg");
  if (c.ok()) {
    std::string text(c->begin(), c->end());
    size_t at = 0;
    while (at < text.size()) {
      size_t nl = text.find('\n', at);
      auto fs = split_tabs(text.substr(at, nl == std::string::npos ? std::string::npos : nl - at));
      at = nl == std::string::npos ? text.size() : nl + 1;
      if (fs.size() < 2) continue;
      if (fs[0] == "url") alert_url_ = unesc(fs[1]);
      else if (fs[0] == "sent") alert_sent_ = std::strtoull(fs[1].c_str(), nullptr, 10);
    }
  }
}

void ThingEngine::save_alert_config() {
  auto& host = db_.kernel_.host();
  host.make_dir(things_dir());
  const std::string text = "url\t" + esc(alert_url_) + "\nsent\t" + std::to_string(alert_sent_) + "\n";
  host.write_file(things_dir() + "/alerts.cfg", ByteSpan((const uint8_t*)text.data(), text.size()));
}

// SET THING ALERTS TO 'http://host[:port]/path' | NONE. Alerts raised from
// then on are sent; the history before it stays here (SHOW THING ALERTS).
SqlQueryResult ThingEngine::set_alerts(const std::string& sql) {
  SqlQueryResult res;
  std::string t = trim(sql);
  if (!t.empty() && t.back() == ';') t = trim(t.substr(0, t.size() - 1));
  const std::string u = upper(t);
  size_t to = u.find(" TO ");
  if (u.find("ALERTS") == std::string::npos || to == std::string::npos) {
    res.success = false;
    res.error_message = "SET THING ALERTS TO 'http://host[:port]/path' | NONE";
    return res;
  }
  std::string v = trim(t.substr(to + 4));
  if (upper(v) == "NONE" || upper(v) == "NULL" || v == "''") v.clear();
  else if (v.size() >= 2 && (v.front() == '\'' || v.front() == '"') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
  if (!v.empty() && v.compare(0, 7, "http://") != 0) {
    res.success = false;
    res.error_message = "alerts go to an http:// address (this engine has no TLS; an https receiver needs a relay in front)";
    return res;
  }
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  alert_url_ = v;
  alert_sent_ = alert_seq_;
  alert_error_.clear();
  alert_backoff_s_ = 0;
  alert_retry_ns_ = 0;
  save_alert_config();
  res.formatted_table = v.empty() ? "Thing alerts are kept here only." : "Thing alerts go to " + v + " from now on.";
  return res;
}

SqlQueryResult ThingEngine::show_alerts(const std::string& name) {
  SqlQueryResult res;
  res.is_result_set = true;
  res.columns = {"seq", "at", "thing", "key", "state", "text", "sent"};
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  for (auto it = alerts_.rbegin(); it != alerts_.rend(); ++it) {
    const Alert& a = *it;
    if (!name.empty() && lower(a.thing) != lower(name)) continue;
    std::string sent = alert_url_.empty() ? "kept here" : a.seq <= alert_sent_ ? "sent" : "waiting" + (alert_error_.empty() ? std::string() : " (" + alert_error_ + ")");
    res.rows.push_back(SqlRow{{std::to_string(a.seq), a.at, a.thing, a.key, a.state, a.text, sent}});
  }
  if (!sql_wire_results()) res.formatted_table = db_.format_result_table(res.columns, res.rows);
  return res;
}

bool ThingEngine::alerts_waiting() {
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  return !alert_url_.empty() && alert_sent_ < alert_seq_ && db_.kernel_.host().wall_ns() >= alert_retry_ns_;
}

namespace {
// One POST; true on a 2xx.
bool http_post(const std::string& url, const std::string& body, std::string* err) {
  std::string rest = url.substr(7);
  size_t slash = rest.find('/');
  std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
  std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
  std::string host = hostport;
  uint16_t port = 80;
  size_t colon = hostport.rfind(':');
  if (colon != std::string::npos) { host = hostport.substr(0, colon); port = (uint16_t)std::atoi(hostport.c_str() + colon + 1); }
  auto c = hal::net_connect(host, port, 5000);
  if (!c.ok()) { *err = "cannot connect to " + hostport + ": " + c.error().message; return false; }
  auto& conn = *c.value();
  const std::string req = "POST " + path + " HTTP/1.1\r\nHost: " + hostport +
                          "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
                          std::to_string(body.size()) + "\r\n\r\n" + body;
  if (!conn.send_all(ByteSpan((const uint8_t*)req.data(), req.size()))) { *err = "the connection failed while sending"; return false; }
  std::string got;
  for (int i = 0; i < 50 && got.find("\r\n") == std::string::npos; ++i) {
    uint8_t buf[512];
    long n = conn.recv_some(MutableByteSpan(buf, sizeof buf), 200);
    if (n < 0) break;
    if (n == 0) { if (!conn.alive()) break; continue; }
    got.append((const char*)buf, (size_t)n);
  }
  conn.close();
  size_t sp = got.find(' ');
  if (got.compare(0, 5, "HTTP/") != 0 || sp == std::string::npos) { *err = "no HTTP answer from " + hostport; return false; }
  const int status = std::atoi(got.c_str() + sp + 1);
  if (status < 200 || status > 299) { *err = hostport + " answered " + std::to_string(status); return false; }
  return true;
}
}  // namespace

void ThingEngine::deliver_alerts() {
  std::unique_lock<aios::hal::Mutex> one(deliver_mu_, std::try_to_lock);
  if (!one.owns_lock()) return;
  const std::string machine = machine_id(), host = host_name();
  for (int n = 0; n < 50; ++n) {
    Alert a;
    std::string url;
    {
      std::lock_guard<aios::hal::Mutex> lk(mu_);
      if (alert_url_.empty() || alert_sent_ >= alert_seq_ || db_.kernel_.host().wall_ns() < alert_retry_ns_) return;
      const Alert* next = nullptr;
      for (const auto& x : alerts_) if (x.seq > alert_sent_) { next = &x; break; }
      if (!next) { alert_sent_ = alert_seq_; save_alert_config(); return; }   // older than what is kept
      a = *next;
      url = alert_url_;
    }
    const std::string body = "{\"machine\":" + json_str(machine) + ",\"host\":" + json_str(host) + ",\"seq\":" + std::to_string(a.seq) +
                             ",\"at\":" + json_str(a.at) + ",\"thing\":" + json_str(a.thing) + ",\"key\":" + json_str(a.key) +
                             ",\"state\":" + json_str(a.state) + ",\"text\":" + json_str(a.text) + "}";
    std::string err;
    const bool ok = http_post(url, body, &err);
    std::lock_guard<aios::hal::Mutex> lk(mu_);
    if (url != alert_url_) return;   // the address changed while it was sending
    if (!ok) {
      alert_error_ = err;
      alert_backoff_s_ = alert_backoff_s_ ? std::min(alert_backoff_s_ * 2, 300) : 2;
      alert_retry_ns_ = db_.kernel_.host().wall_ns() + (uint64_t)alert_backoff_s_ * 1000000000ull;
      return;
    }
    alert_sent_ = a.seq;
    alert_error_.clear();
    alert_backoff_s_ = 0;
    alert_retry_ns_ = 0;
    save_alert_config();
  }
}


// ---- Mirrors: Things about another machine ------------------------------------


// CREATE THING name MIRROR kind ON machine [EVERY n SECONDS]
// CREATE THING name MACHINES [EVERY n SECONDS]
bool ThingEngine::parse_mirror(const std::string& sql, Kind* k, std::string* err) const {
  const char* usage = "CREATE THING <name> MIRROR <kind> ON <machine> [EVERY <n> SECONDS] | CREATE THING <name> MACHINES [EVERY <n> SECONDS]";
  std::string t = trim(sql);
  if (!t.empty() && t.back() == ';') t = trim(t.substr(0, t.size() - 1));
  std::vector<std::string> w;
  {
    std::string cur;
    for (char c : t) {
      if (std::isspace((unsigned char)c)) { if (!cur.empty()) { w.push_back(cur); cur.clear(); } }
      else cur += c;
    }
    if (!cur.empty()) w.push_back(cur);
  }
  auto W = [&](size_t i) { return i < w.size() ? upper(w[i]) : std::string(); };
  if (W(0) != "CREATE" || W(1) != "THING" || w.size() < 4) { *err = usage; return false; }
  k->type = Kind::Mirror;
  k->name = lower(unquote_ident(w[2]));
  size_t i = 3;
  if (W(i) == "MIRROR") {
    if (w.size() < i + 4 || W(i + 2) != "ON") { *err = usage; return false; }
    k->mirror_kind = lower(unquote_ident(w[i + 1]));
    k->mirror_machine = lower(unquote_ident(w[i + 3]));
    if (k->mirror_kind.empty() || k->mirror_machine.empty() || k->mirror_machine == "all" || k->mirror_machine == "local") {
      *err = "a mirror names one kind on one other machine (not ALL or LOCAL)";
      return false;
    }
    i += 4;
  } else if (W(i) == "MACHINES") {
    k->key_col = "machine";
    i += 1;
  } else {
    *err = usage;
    return false;
  }
  if (W(i) == "EVERY") {
    if (w.size() < i + 2) { *err = usage; return false; }
    k->every_s = std::atoi(w[i + 1].c_str());
    if (k->every_s < 1 || k->every_s > 86400) { *err = "EVERY: 1 to 86400 seconds"; return false; }
    i += 2;
    if (W(i) == "SECONDS" || W(i) == "SECOND") ++i;
  }
  if (i != w.size()) { *err = std::string("unexpected ") + w[i] + ": " + usage; return false; }
  k->declaration = t;
  return true;
}

namespace {
// A member that answered, but not with Things: it is there.
bool fleet_unreachable(const std::string& e) {
  return e.find("no reply") != std::string::npos || e.find("timed out") != std::string::npos ||
         e.find("cannot connect") != std::string::npos || e.find("not reachable") != std::string::npos ||
         e.find("offline") != std::string::npos || e.find("no fleet member") != std::string::npos;
}
}  // namespace

void ThingEngine::pull_mirrors() {
#if defined(AIOS_METAL)
  // Never on the board (kBoardNoPull); one declared before this image says so.
  {
    std::lock_guard<aios::hal::Mutex> lk(mu_);
    for (auto& kv : kinds_)
      if (kv.second.type == Kind::Mirror && kv.second.mirror_error != kBoardNoPull) { kv.second.mirror_error = kBoardNoPull; dirty_ = true; }
  }
  return;
#endif
  struct Job { std::string name, kind, machine; };
  std::vector<Job> jobs;
  size_t self_kinds = 0;
  {
    std::lock_guard<aios::hal::Mutex> lk(mu_);
    const uint64_t now = db_.kernel_.host().wall_ns();
    for (auto& kv : kinds_) {
      Kind& k = kv.second;
      if (k.type != Kind::Mirror) { ++self_kinds; continue; }
      ++self_kinds;
      if (k.paused || k.next_pull_ns > now) continue;
#if defined(AIOS_METAL)
      const int every = std::max(k.every_s, 30);   // the board's one loop also carries its hub link
#else
      const int every = k.every_s;
#endif
      k.next_pull_ns = now + (uint64_t)every * 1000000000ull;   // one pull at a time, however slow
      jobs.push_back(Job{k.name, k.mirror_kind, k.mirror_machine});
    }
  }
  const std::string self_host = host_name();
  for (const auto& j : jobs) {
    std::vector<ThingFleetAnswer> answers;
    if (fleet_fn()) fleet_fn()(j.kind.empty() ? "ALL" : j.machine, j.kind.empty() ? "SHOW THINGS" : "SHOW THINGS " + j.kind, &answers);
    std::lock_guard<aios::hal::Mutex> lk(mu_);
    MuHeld held;
    auto it = kinds_.find(j.name);
    if (it == kinds_.end() || it->second.type != Kind::Mirror) continue;   // dropped while it pulled
    if (!fleet_fn()) {
      it->second.mirror_error = "this engine has no fleet link";
    } else {
      apply_mirror(it->second, answers, self_kinds, self_host);
    }
    save_locked();
  }
}

void ThingEngine::apply_mirror(Kind& k, std::vector<ThingFleetAnswer>& answers, size_t self_kinds, const std::string& self_host) {
  const uint64_t now = db_.kernel_.host().wall_ns();
  k.reactions++;
  if (!k.mirror_kind.empty()) {
    // One machine's kind.
    std::string e;
    if (answers.empty()) e = "no fleet member " + k.mirror_machine;
    else if (answers.size() > 1) e = k.mirror_machine + " names " + std::to_string(answers.size()) + " machines";
    else if (!answers[0].ok) e = answers[0].error;
    else if (!answers[0].result || !answers[0].result->success) e = answers[0].result ? answers[0].result->error_message : "no answer";
    if (!e.empty()) {
      if (k.mirror_error.empty()) dirty_ = true;
      k.mirror_error = e;
      alert(k.name, "", "error", "cannot read " + k.mirror_kind + " on " + k.mirror_machine + ": " + e);
      return;
    }
    const SqlQueryResult& r = *answers[0].result;
    if (!k.mirror_error.empty()) alert(k.name, "", "recovered", k.mirror_machine + " answers again");
    k.mirror_error.clear();
    k.mirror_ok_ns = now;
    if (r.columns.empty()) return;
    k.key_col = r.columns[0];
    static const std::set<std::string> meta = {"version", "updated_at", "last_change", "waiting_for", "may_read", "machine", "partitions"};
    size_t vcol = std::string::npos, ccol = std::string::npos;
    for (size_t c = 0; c < r.columns.size(); ++c) {
      if (r.columns[c] == "version") vcol = c;
      if (r.columns[c] == "last_change") ccol = c;
    }
    std::set<std::string> seen;
    for (const auto& row : r.rows) {
      if (row.values.empty()) continue;
      Kind::GInst fresh;
      fresh.key = row.values[0];
      for (size_t c = 1; c < r.columns.size() && c < row.values.size(); ++c) {
        if (meta.count(r.columns[c])) continue;
        fresh.vals.emplace_back(r.columns[c], row.values[c]);
        if (std::find(k.gcols.begin(), k.gcols.end(), r.columns[c]) == k.gcols.end()) k.gcols.push_back(r.columns[c]);
      }
      std::string cause = "from " + answers[0].machine;
      if (vcol < row.values.size()) cause += " v" + row.values[vcol];
      if (ccol < row.values.size() && !sql_is_null(row.values[ccol])) cause += ": " + row.values[ccol];
      const std::string ck = sql_collate_key(fresh.key);
      seen.insert(ck);
      put_ginst(k, ck, std::move(fresh), cause, now);
    }
    std::vector<std::string> gone;
    for (const auto& g : k.gthings) if (!seen.count(g.first)) gone.push_back(g.first);
    for (const auto& ck : gone) drop_ginst(k, ck, "gone on " + answers[0].machine);
    return;
  }
  // MACHINES: every member, this one included; one that drops out of the
  // roster stays, as not there.
  k.mirror_error.clear();
  k.mirror_ok_ns = now;
  std::set<std::string> seen;
  auto put = [&](const std::string& machine, const std::string& host, bool there, const std::string& serves,
                 const std::string& kinds, const std::string& note) {
    Kind::GInst fresh;
    fresh.key = machine;
    fresh.vals = {{"host", host}, {"reachable", there ? "yes" : "no"}, {"serves", serves}, {"thing_kinds", kinds},
                  {"note", note}};
    if (!there) fresh.vals.emplace_back("alert", "unreachable: " + note);
    for (const auto& f : fresh.vals)
      if (std::find(k.gcols.begin(), k.gcols.end(), f.first) == k.gcols.end()) k.gcols.push_back(f.first);
    const std::string ck = sql_collate_key(machine);
    seen.insert(ck);
    put_ginst(k, ck, std::move(fresh), there ? "answered" : "no answer", now);
  };
  put(machine_id(), self_host, true, "things", std::to_string(self_kinds), "this machine");
  for (const auto& a : answers) {
    if (a.ok && a.result && a.result->success) {
      std::string host = sql_null();
      size_t hcol = std::string::npos;
      for (size_t c = 0; c < a.result->columns.size(); ++c) if (a.result->columns[c] == "host") hcol = c;
      if (hcol != std::string::npos && !a.result->rows.empty() && hcol < a.result->rows[0].values.size()) host = a.result->rows[0].values[hcol];
      put(a.machine, host, true, "things", std::to_string(a.result->rows.size()), "");
    } else {
      const std::string e = a.ok ? (a.result ? a.result->error_message : std::string("no answer")) : a.error;
      if (e.compare(0, 7, "skipped") == 0) {
        // Not asked this round: as last seen.
        seen.insert(sql_collate_key(a.machine));
        continue;
      }
      if (fleet_unreachable(e)) {
        // What it was last seen as stays; only its reachability moves.
        auto old = k.gthings.find(sql_collate_key(a.machine));
        std::string host = sql_null(), serves = "?", kinds = sql_null();
        if (old != k.gthings.end())
          for (const auto& f : old->second.vals) {
            if (f.first == "host") host = f.second;
            if (f.first == "serves") serves = f.second;
            if (f.first == "thing_kinds") kinds = f.second;
          }
        put(a.machine, host, false, serves, kinds, e);
      } else {
        const bool sql = e.find("does not serve SQL") == std::string::npos && e.find("unknown node request") == std::string::npos;
        put(a.machine, sql_null(), true, sql ? "sql" : "none", sql_null(), e.substr(0, 120));
      }
    }
  }
  for (auto& g : k.gthings) {
    if (seen.count(g.first)) continue;
    Kind::GInst fresh = g.second;
    for (auto& f : fresh.vals) {
      if (f.first == "reachable") f.second = "no";
      if (f.first == "note") f.second = "not in the fleet roster";
    }
    bool has_alert = false;
    for (auto& f : fresh.vals) if (f.first == "alert") { f.second = "unreachable: not in the fleet roster"; has_alert = true; }
    if (!has_alert) fresh.vals.emplace_back("alert", "unreachable: not in the fleet roster");
    const std::string ck = g.first;
    put_ginst(k, ck, std::move(fresh), "left the roster", now);
  }
}

}  // namespace aios::userland
