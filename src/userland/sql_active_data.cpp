#include "aios/userland/sql_active_data.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "aios/hal/host.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"

namespace aios::userland {

namespace {

constexpr size_t kNoRow = (size_t)-1;
constexpr uint32_t kNone = UINT32_MAX;
constexpr size_t kPerGroupWrites = 64;      // more dirty groups than this: the view table is written whole
constexpr size_t kTierRowCap = 25000;       // larger tables are re-encoded only by maintenance/boot, not live idle ticks

uint64_t now_ns() {
  return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string lower(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace((unsigned char)s[a])) ++a;
  while (b > a && (std::isspace((unsigned char)s[b - 1]) || s[b - 1] == ';')) --b;
  return s.substr(a, b - a);
}

std::string db_or_default(const std::string& db) { return db.empty() ? std::string("aios_db") : db; }

// The key the engine files tables under (make_table_key): database.table, lower case.
std::string tkey(const SqlTable& t) { return lower(db_or_default(t.database)) + "." + lower(t.name); }

std::string quote_ident(const std::string& s) {
  std::string o = "`";
  for (char c : s) { if (c == '`') o += '`'; o += c; }
  return o + "`";
}

std::string quote_literal(const std::string& v) {
  if (sql_is_null(v)) return "NULL";
  std::string o = "'";
  for (char c : v) {
    if (c == '\\' || c == '\'') o += '\\';
    o += c;
  }
  return o + "'";
}

SqlQueryResult error_result(const std::string& msg) {
  SqlQueryResult r;
  r.success = false;
  r.error_message = msg;
  return r;
}

SqlQueryResult ok_result(const std::string& msg) {
  SqlQueryResult r;
  r.formatted_table = msg;
  return r;
}

// A SELECT's text as a key: runs of white space are one space and letters are lower case, except inside
// quotes, where a literal's case is part of what is asked ('Open' is not 'open').
std::string normalize_sql(const std::string& sql) {
  std::string out;
  char quote = 0;
  bool space = false;
  for (size_t i = 0; i < sql.size(); ++i) {
    const char c = sql[i];
    if (quote) {
      out += c;
      if (c == '\\' && i + 1 < sql.size()) out += sql[++i];
      else if (c == quote) quote = 0;
      continue;
    }
    if (c == '\'' || c == '"' || c == '`') { if (space && !out.empty()) out += ' '; space = false; quote = c; out += c; continue; }
    if (std::isspace((unsigned char)c)) { space = true; continue; }
    if (space && !out.empty() && out.back() != '(' && c != ')' && c != ',' && out.back() != ',') out += ' ';
    space = false;
    out += (char)std::tolower((unsigned char)c);
  }
  while (!out.empty() && (out.back() == ';' || out.back() == ' ')) out.pop_back();
  return out;
}

// ---- a small reader over the statement text ----
struct Cur {
  const std::string& s;
  size_t i = 0;
  explicit Cur(const std::string& str) : s(str) {}
  void ws() { while (i < s.size() && std::isspace((unsigned char)s[i])) ++i; }
  bool word(const char* kw) {
    ws();
    const size_t n = std::strlen(kw);
    if (i + n > s.size()) return false;
    for (size_t k = 0; k < n; ++k)
      if (std::tolower((unsigned char)s[i + k]) != std::tolower((unsigned char)kw[k])) return false;
    if (i + n < s.size() && (std::isalnum((unsigned char)s[i + n]) || s[i + n] == '_')) return false;
    i += n;
    return true;
  }
  bool peek_word(const char* kw) { const size_t save = i; const bool r = word(kw); i = save; return r; }
  bool ch(char c) { ws(); if (i < s.size() && s[i] == c) { ++i; return true; } return false; }
  bool at_end() { ws(); return i >= s.size() || s[i] == ';'; }
  // name or db.name, each part bare or `quoted`
  bool qname(std::vector<std::string>* parts) {
    parts->clear();
    for (;;) {
      ws();
      std::string p;
      if (i < s.size() && s[i] == '`') {
        ++i;
        while (i < s.size()) {
          if (s[i] == '`') { if (i + 1 < s.size() && s[i + 1] == '`') { p += '`'; i += 2; continue; } break; }
          p += s[i++];
        }
        if (i >= s.size()) return false;
        ++i;
      } else {
        while (i < s.size() && (std::isalnum((unsigned char)s[i]) || s[i] == '_' || s[i] == '$')) p += s[i++];
      }
      if (p.empty()) return false;
      parts->push_back(p);
      if (i < s.size() && s[i] == '.') { ++i; continue; }
      return true;
    }
  }
  std::string rest() { ws(); return trim(s.substr(i)); }
};

// The first top-level (outside quotes and parentheses) occurrence of the word `kw` at or after `from`.
size_t find_kw(const std::string& s, const char* kw, size_t from = 0) {
  const size_t n = std::strlen(kw);
  int depth = 0;
  char quote = 0;
  for (size_t i = from; i < s.size(); ++i) {
    const char c = s[i];
    if (quote) { if (c == '\\') ++i; else if (c == quote) quote = 0; continue; }
    if (c == '\'' || c == '"' || c == '`') { quote = c; continue; }
    if (c == '(') { ++depth; continue; }
    if (c == ')') { --depth; continue; }
    if (depth != 0 || i + n > s.size()) continue;
    if (i > 0 && (std::isalnum((unsigned char)s[i - 1]) || s[i - 1] == '_')) continue;
    bool eq = true;
    for (size_t k = 0; k < n && eq; ++k) eq = std::tolower((unsigned char)s[i + k]) == std::tolower((unsigned char)kw[k]);
    if (eq && (i + n == s.size() || !(std::isalnum((unsigned char)s[i + n]) || s[i + n] == '_'))) return i;
  }
  return std::string::npos;
}

std::vector<std::string> split_commas(const std::string& s) {
  std::vector<std::string> out;
  int depth = 0;
  char quote = 0;
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size()) { out.push_back(trim(s.substr(start))); break; }
    const char c = s[i];
    if (quote) { if (c == '\\') ++i; else if (c == quote) quote = 0; continue; }
    if (c == '\'' || c == '"' || c == '`') { quote = c; continue; }
    if (c == '(') ++depth;
    else if (c == ')') --depth;
    else if (c == ',' && depth == 0) { out.push_back(trim(s.substr(start, i - start))); start = i + 1; }
  }
  return out;
}

// ---- exact decimals: values are summed in units of 1e-9 ----
using I128 = AIOS_INT128;
const I128 kUnit = 1000000000;

// Plain decimal text (sign, digits, at most nine after the point, no exponent); false when it is not.
bool parse_scaled(const std::string& s, I128* out, int* frac) {
  size_t i = 0;
  bool neg = false;
  if (i < s.size() && (s[i] == '-' || s[i] == '+')) neg = s[i++] == '-';
  I128 ip = 0;
  size_t digits = 0;
  while (i < s.size() && std::isdigit((unsigned char)s[i])) {
    ip = ip * 10 + (s[i++] - '0');
    if (++digits > 18) return false;
  }
  I128 fp = 0;
  int fd = 0;
  if (i < s.size() && s[i] == '.') {
    ++i;
    while (i < s.size() && std::isdigit((unsigned char)s[i])) {
      if (fd == 9) return false;
      fp = fp * 10 + (s[i++] - '0');
      ++fd;
    }
  }
  if (i != s.size() || (digits == 0 && fd == 0)) return false;
  for (int k = fd; k < 9; ++k) fp *= 10;
  I128 v = ip * kUnit + fp;
  *out = neg ? -v : v;
  *frac = fd;
  return true;
}

std::string i128_text(I128 v) {
  if (v == 0) return "0";
  const bool neg = v < 0;
  if (neg) v = -v;
  std::string s;
  while (v > 0) { s += (char)('0' + (int)(v % 10)); v /= 10; }
  if (neg) s += '-';
  std::reverse(s.begin(), s.end());
  return s;
}

// An integer q read as q / 10^scale.
std::string scaled_int_text(I128 q, int scale) {
  const bool neg = q < 0;
  if (neg) q = -q;
  std::string digits = i128_text(q);
  if (scale > 0) {
    if ((int)digits.size() <= scale) digits.insert(0, (size_t)(scale + 1 - (int)digits.size()), '0');
    digits.insert(digits.size() - (size_t)scale, ".");
  }
  return (neg && q != 0 ? "-" : "") + digits;
}

I128 pow10_i128(int n) { I128 p = 1; while (n-- > 0) p *= 10; return p; }

// v, in units of 1e-9, to `scale` places (half away from zero).
std::string fixed_text(I128 v, int scale) {
  if (scale > 9) scale = 9;
  const I128 unit = pow10_i128(9 - scale);
  const bool neg = v < 0;
  I128 mag = neg ? -v : v;
  mag = (mag + unit / 2) / unit;
  return scaled_int_text(neg ? -mag : mag, scale);
}

double number_of(const SqlRowExpr::Value& v) { return std::strtod(v.text.c_str(), nullptr); }

// MIN / MAX order: numbers by value, anything else by collation.
int compare_extreme(bool an, double ad, const std::string& at, bool bn, double bd, const std::string& bt) {
  if (an && bn) return ad < bd ? -1 : ad > bd ? 1 : 0;
  const std::string ka = sql_collate_key(at), kb = sql_collate_key(bt);
  return ka < kb ? -1 : ka > kb ? 1 : 0;
}

// An integer join key: 1 = a number, 0 = NULL, -1 = text that is not an integer.
int int_key(const std::string& s, int64_t* out) {
  if (sql_is_null(s)) return 0;
  if (s.empty() || s.size() > 19) return -1;
  size_t i = 0;
  bool neg = false;
  if (s[0] == '-') { neg = true; i = 1; }
  if (i >= s.size()) return -1;
  int64_t v = 0;
  for (; i < s.size(); ++i) {
    if (!std::isdigit((unsigned char)s[i])) return -1;
    v = v * 10 + (s[i] - '0');
  }
  *out = neg ? -v : v;
  return 1;
}

std::string file_part(const std::string& s) {
  std::string o;
  for (char c : lower(s)) o += (std::isalnum((unsigned char)c) || c == '_') ? c : '_';
  return o;
}

}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle and persistence
// ---------------------------------------------------------------------------

ActiveDataEngine::ActiveDataEngine(SqlDatabase& db) : db_(db) {}
ActiveDataEngine::~ActiveDataEngine() = default;

void ActiveDataEngine::persist(const std::string& kind, const std::string& db, const std::string& name,
                               const std::string& sql) {
  auto& host = db_.kernel_.host();
  const std::string dir = db_.root_path_ + "/active";
  host.make_dir(dir);
  const std::string body = db + "\n" + sql;
  host.write_file(dir + "/" + kind + "_" + file_part(db) + "_" + file_part(name) + ".active",
                  ByteSpan((const uint8_t*)body.data(), body.size()));
}

void ActiveDataEngine::unpersist(const std::string& kind, const std::string& db, const std::string& name) {
  db_.kernel_.host().remove(db_.root_path_ + "/active/" + kind + "_" + file_part(db) + "_" + file_part(name) + ".active");
}

void ActiveDataEngine::load() {
  auto& host = db_.kernel_.host();
  const std::string dir = db_.root_path_ + "/active";
  if (!host.exists(dir)) return;
  auto names = host.list_dir(dir);
  if (!names.ok()) return;
  std::vector<std::string> files(names->begin(), names->end());
  std::sort(files.begin(), files.end());
  // Links and filters before caches before views: a view's table may be what another object reads.
  auto rank = [](const std::string& n) { return n.rfind("flt_", 0) == 0 ? 0 : n.rfind("link_", 0) == 0 ? 1 : n.rfind("cache_", 0) == 0 ? 2 : 3; };
  std::stable_sort(files.begin(), files.end(), [&](const std::string& a, const std::string& b) { return rank(a) < rank(b); });
  for (const std::string& n : files) {
    if (n.size() < 8 || n.compare(n.size() - 7, 7, ".active") != 0) continue;
    auto f = host.read_file(dir + "/" + n);
    if (!f.ok()) continue;
    const std::string body(f->begin(), f->end());
    const size_t nl = body.find('\n');
    if (nl == std::string::npos) continue;
    load_db_ = body.substr(0, nl);
    const std::string sql = body.substr(nl + 1);
    Cur c(sql);
    c.word("CREATE"); c.word("ACTIVE");
    SqlQueryResult r;
    if (c.word("FILTER")) r = create_filter(sql, false);
    else if (c.word("CACHE")) r = create_cache(sql, false);
    else if (c.word("LINK")) r = create_link(sql, false);
    else if (c.word("VIEW")) r = create_view(sql, false);
    else continue;
    if (!r.success) std::fprintf(stderr, "[active] could not restore %s: %s\n", n.c_str(), r.error_message.c_str());
  }
  load_db_.clear();
}

void ActiveDataEngine::recompute_watched() {
  watched_.clear();
  for (auto& kv : filters_) watched_.insert(kv.second->table_key);
  for (auto& kv : caches_) watched_.insert(kv.second->table_key);
  for (auto& kv : links_) { watched_.insert(kv.second->child_key); watched_.insert(kv.second->parent_key); }
  for (auto& kv : views_) watched_.insert(kv.second->table_key);
}

void ActiveDataEngine::rebuild_all() {
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  for (auto& kv : filters_) kv.second->stale = true;
  for (auto& kv : caches_) kv.second->stale = true;
  for (auto& kv : links_) kv.second->stale = true;
  for (auto& kv : views_) {
    kv.second->all_dirty = true;
    kv.second->state_stale = true;
    if (!kv.second->pending) { kv.second->pending = true; pending_views_.fetch_add(1); }
  }
}

bool ActiveDataEngine::handles(const std::string& sql) const {
  Cur c(sql);
  if (c.word("CREATE") || c.word("DROP")) {
    if (!c.word("ACTIVE")) return false;
    return c.peek_word("FILTER") || c.peek_word("CACHE") || c.peek_word("LINK") || c.peek_word("VIEW");
  }
  if (c.word("SHOW") && c.word("ACTIVE"))
    return c.peek_word("FILTERS") || c.peek_word("CACHES") || c.peek_word("LINKS") || c.peek_word("VIEWS") ||
           c.peek_word("STATUS") || c.peek_word("TIERING");
  return false;
}

SqlQueryResult ActiveDataEngine::execute(const std::string& sql) {
  Cur c(sql);
  if (c.word("CREATE")) {
    c.word("ACTIVE");
    if (c.word("FILTER")) return create_filter(sql, true);
    if (c.word("CACHE")) return create_cache(sql, true);
    if (c.word("LINK")) return create_link(sql, true);
    if (c.word("VIEW")) return create_view(sql, true);
  } else if (c.word("DROP")) {
    c.word("ACTIVE");
    if (c.word("FILTER")) return drop_object(sql, 'F');
    if (c.word("CACHE")) return drop_object(sql, 'C');
    if (c.word("LINK")) return drop_object(sql, 'L');
    if (c.word("VIEW")) return drop_object(sql, 'V');
  } else if (c.word("SHOW")) {
    c.word("ACTIVE");
    if (c.word("FILTERS")) return show('F');
    if (c.word("CACHES")) return show('C');
    if (c.word("LINKS")) return show('L');
    if (c.word("VIEWS")) return show('V');
    if (c.word("STATUS")) return show_status();
    if (c.word("TIERING")) return show_tiering();
  }
  return error_result("unknown active data statement: " + sql);
}

bool ActiveDataEngine::watches(const SqlTable& t) const {
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  return !watched_.empty() && watched_.count(tkey(t)) != 0;
}

// ---------------------------------------------------------------------------
// Aggregates
// ---------------------------------------------------------------------------

void ActiveDataEngine::agg_apply(std::vector<AggSpec>& specs, GroupAcc& g, const SqlRow& row, int sign) {
  g.rows += sign;
  if (g.acc.size() != specs.size()) g.acc.resize(specs.size());
  for (size_t i = 0; i < specs.size(); ++i) {
    AggSpec& s = specs[i];
    Acc& a = g.acc[i];
    if (s.kind == AggSpec::CountStar) { a.nn += sign; continue; }
    SqlRowExpr::Value v;
    std::string err;
    if (!s.expr || !s.expr->eval(row, &v, &err) || v.null) continue;
    a.nn += sign;
    switch (s.kind) {
      case AggSpec::Count: break;
      case AggSpec::Sum:
      case AggSpec::Avg: {
        I128 sc;
        int frac = 0;
        if (v.numeric && parse_scaled(v.text, &sc, &frac)) {
          a.isum += sign > 0 ? sc : -sc;
          if (frac > s.scale) s.scale = frac;
        } else {
          a.inexact = true;
        }
        a.dsum += sign * number_of(v);
        break;
      }
      case AggSpec::Min:
      case AggSpec::Max: {
        const bool is_min = s.kind == AggSpec::Min;
        bool& has = is_min ? a.has_min : a.has_max;
        bool& stale = is_min ? a.min_stale : a.max_stale;
        Extreme& e = is_min ? a.min : a.max;
        const double d = v.numeric ? number_of(v) : 0.0;
        if (sign > 0) {
          if (stale) break;   // the table is read again anyway
          if (!has || (is_min ? compare_extreme(v.numeric, d, v.text, e.numeric, e.num, e.text) < 0
                              : compare_extreme(v.numeric, d, v.text, e.numeric, e.num, e.text) > 0)) {
            e.numeric = v.numeric; e.num = d; e.text = v.text; has = true;
          }
        } else if (has && compare_extreme(v.numeric, d, v.text, e.numeric, e.num, e.text) == 0) {
          stale = true;   // the extreme left: only the table says what is next
        }
        break;
      }
      case AggSpec::CountStar: break;
    }
    if (a.nn == 0) {
      a.isum = 0; a.dsum = 0; a.inexact = false;
      a.has_min = a.has_max = false;
      a.min_stale = a.max_stale = false;
    }
  }
}

std::string ActiveDataEngine::agg_render(const AggSpec& s, const GroupAcc& g, const Acc& a) {
  switch (s.kind) {
    case AggSpec::CountStar: return std::to_string(g.rows);
    case AggSpec::Count: return std::to_string(a.nn);
    case AggSpec::Sum:
      if (a.nn == 0) return sql_null();
      return a.inexact ? sql_format_double(a.dsum, 0) : fixed_text(a.isum, s.scale);
    case AggSpec::Avg: {
      if (a.nn == 0) return sql_null();
      if (a.inexact) return sql_format_double(a.dsum / (double)a.nn, 0);
      // The mean to four more places than the values have, as MariaDB's DECIMAL AVG reads.
      const int rs = s.scale + 4;
      I128 num = 0;
      const I128 den = (I128)a.nn * kUnit;
      if (!__builtin_mul_overflow(a.isum, pow10_i128(rs), &num)) {
        const bool neg = num < 0;
        I128 mag = neg ? -num : num;
        I128 q = (mag + den / 2) / den;
        return scaled_int_text(neg ? -q : q, rs);
      }
      return sql_format_double((double)((long double)a.dsum / (long double)a.nn), 0);
    }
    case AggSpec::Min: return a.nn == 0 || !a.has_min ? sql_null() : a.min.text;
    case AggSpec::Max: return a.nn == 0 || !a.has_max ? sql_null() : a.max.text;
  }
  return sql_null();
}

namespace {

struct ParsedItem {
  std::string text;      // as written, without the alias
  std::string alias;
};

// "expr [AS alias]" -> its parts.
ParsedItem split_alias(const std::string& item) {
  ParsedItem p;
  p.text = item;
  size_t at = std::string::npos, from = 0;
  for (;;) {
    const size_t k = find_kw(item, "AS", from);
    if (k == std::string::npos) break;
    at = k;
    from = k + 2;
  }
  if (at != std::string::npos && at > 0) {
    std::string a = trim(item.substr(at + 2));
    if (a.size() >= 2 && a.front() == '`' && a.back() == '`') a = a.substr(1, a.size() - 2);
    if (!a.empty()) { p.text = trim(item.substr(0, at)); p.alias = a; }
  }
  return p;
}

// COUNT(*), COUNT(x), SUM(x), AVG(x), MIN(x), MAX(x) -> the kind and the argument; false when it is not one.
bool parse_aggregate(const std::string& text, int* kind, std::string* arg, std::string* err) {
  static const struct { const char* name; int kind; } kinds[] = {{"COUNT", 1}, {"SUM", 2}, {"AVG", 3}, {"MIN", 4}, {"MAX", 5}};
  const size_t open = text.find('(');
  if (open == std::string::npos || text.back() != ')') return false;
  std::string fn = trim(text.substr(0, open));
  for (char& c : fn) c = (char)std::toupper((unsigned char)c);
  for (const auto& k : kinds) {
    if (fn != k.name) continue;
    int depth = 0;
    for (size_t i = open; i < text.size(); ++i) {
      if (text[i] == '(') ++depth;
      else if (text[i] == ')' && --depth == 0 && i + 1 != text.size()) { *err = "unsupported expression: " + text; return false; }
    }
    *arg = trim(text.substr(open + 1, text.size() - open - 2));
    *kind = k.kind;
    if (arg->empty()) { *err = fn + "() needs an argument"; return false; }
    if (*kind == 1 && *arg == "*") { *kind = 0; return true; }
    if (*arg == "*") { *err = fn + "(*) is not valid"; return false; }
    Cur c(*arg);
    if (c.word("DISTINCT")) { *err = "DISTINCT aggregates are not supported: " + text; return false; }
    return true;
  }
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// The shared front of a SELECT: items, table, WHERE, GROUP BY
// ---------------------------------------------------------------------------

namespace {

struct SelectParts {
  std::vector<std::string> items;
  std::vector<std::string> table;
  std::string where;
  std::string group_by;
  bool has_group = false;
};

bool parse_select_parts(const std::string& select_sql, bool want_group, SelectParts* out, std::string* err) {
  Cur c(select_sql);
  if (!c.word("SELECT")) { *err = "expected SELECT"; return false; }
  const size_t items_at = c.i;
  const size_t from = find_kw(select_sql, "FROM", items_at);
  if (from == std::string::npos) { *err = "the query needs a FROM clause"; return false; }
  const std::string items = trim(select_sql.substr(items_at, from - items_at));
  if (items.empty()) { *err = "the query selects nothing"; return false; }
  {
    Cur d(items);
    if (d.word("DISTINCT")) { *err = "DISTINCT is not supported"; return false; }
  }
  out->items = split_commas(items);
  Cur t(select_sql);
  t.i = from + 4;
  if (!t.qname(&out->table)) { *err = "expected a table name after FROM"; return false; }
  const size_t after = t.i;
  const size_t where_at = find_kw(select_sql, "WHERE", after);
  const size_t group_at = find_kw(select_sql, "GROUP", after);
  const size_t first = std::min(where_at, group_at);
  {
    // Whatever stands between the table and the next clause is an alias, a JOIN, a comma: not supported.
    const std::string between = trim(select_sql.substr(after, (first == std::string::npos ? select_sql.size() : first) - after));
    if (!between.empty()) { *err = "only a single table is supported (found '" + between + "')"; return false; }
  }
  if (where_at != std::string::npos && (group_at == std::string::npos || where_at < group_at)) {
    const size_t end = group_at == std::string::npos ? select_sql.size() : group_at;
    out->where = trim(select_sql.substr(where_at + 5, end - where_at - 5));
    if (out->where.empty()) { *err = "WHERE needs a condition"; return false; }
  }
  if (group_at != std::string::npos) {
    Cur g(select_sql);
    g.i = group_at;
    if (!g.word("GROUP") || !g.word("BY")) { *err = "expected GROUP BY"; return false; }
    out->has_group = true;
    out->group_by = g.rest();
    if (out->group_by.empty()) { *err = "GROUP BY needs a column"; return false; }
    for (const char* kw : {"HAVING", "ORDER", "LIMIT", "WITH"}) {
      if (find_kw(out->group_by, kw) != std::string::npos) { *err = std::string(kw) + " is not supported"; return false; }
    }
  } else {
    const std::string tail = where_at == std::string::npos ? std::string() : out->where;
    for (const char* kw : {"HAVING", "ORDER", "LIMIT", "UNION", "FOR"}) {
      if (find_kw(tail, kw) != std::string::npos) { *err = std::string(kw) + " is not supported"; return false; }
    }
  }
  if (want_group && !out->has_group) { *err = "an active view needs a GROUP BY"; return false; }
  if (!want_group && out->has_group) { *err = "an active cache has one row: use an active view for GROUP BY"; return false; }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. Active filters
// ---------------------------------------------------------------------------

void ActiveDataEngine::rebuild_filter(ActiveFilter& f, const SqlTable& t) {
  std::string err;
  f.expr = SqlRowExpr::compile(db_, t, f.where_sql, &err);
  const size_t n = t.row_count();
  f.words.assign((n + 63) / 64 + 1, 0);
  f.count = 0;
  if (f.expr) {
    for (size_t r = 0; r < n; ++r) {
      const SqlRow row = t.get_row(r);
      std::string e;
      if (f.expr->holds(row, &e)) { f.words[r >> 6] |= 1ULL << (r & 63); ++f.count; }
    }
  }
  f.synced_rows = n;
  f.layout_seq = t.layout_seq;
  f.stale = !f.expr;
}

SqlQueryResult ActiveDataEngine::create_filter(const std::string& sql, bool persist_it) {
  Cur c(sql);
  c.word("CREATE"); c.word("ACTIVE"); c.word("FILTER");
  const bool if_not_exists = c.word("IF") && c.word("NOT") && c.word("EXISTS");
  std::vector<std::string> nm, tn;
  if (!c.qname(&nm) || nm.size() != 1) return error_result("expected CREATE ACTIVE FILTER name ON table WHERE condition");
  if (!c.word("ON") || !c.qname(&tn)) return error_result("expected ON <table>");
  if (!c.word("WHERE")) return error_result("expected WHERE <condition>");
  const std::string where = c.rest();
  if (where.empty()) return error_result("expected WHERE <condition>");

  SqlTable* t = tn.size() >= 2 ? db_.find_table(tn.back(), tn[tn.size() - 2]) : db_.find_table(tn[0], load_db_);
  if (!t) return error_result("table not found: " + tn.back());
  std::string err;
  auto probe = SqlRowExpr::compile(db_, *t, where, &err);
  if (!probe) return error_result("invalid filter condition: " + err);

  auto f = std::make_shared<ActiveFilter>();
  f->name = nm[0];
  f->db = db_or_default(t->database);
  f->table_key = tkey(*t);
  f->table_name = t->name;
  f->where_sql = where;
  f->where_norm = probe->signature();
  const std::string key = lower(f->db) + "." + lower(f->name);

  std::lock_guard<aios::hal::Mutex> lk(mu_);
  if (filters_.count(key)) return if_not_exists ? ok_result("filter already exists") : error_result("active filter already exists: " + f->name);
  rebuild_filter(*f, *t);
  filters_[key] = f;
  recompute_watched();
  if (persist_it) persist("flt", f->db, f->name, sql);
  return ok_result("active filter '" + f->name + "' on " + t->name + ": " + std::to_string(f->count) + " of " +
                   std::to_string(t->row_count()) + " rows match");
}

const std::vector<uint64_t>* ActiveDataEngine::get_filter_bits(const SqlTable& t, const std::string& where_norm,
                                                               size_t* out_count) {
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  if (filters_.empty()) return nullptr;
  const std::string tk = tkey(t);
  for (auto& kv : filters_) {
    ActiveFilter& f = *kv.second;
    if (f.table_key != tk || f.where_norm != where_norm) continue;
    if (f.stale || f.synced_rows != t.row_count() || f.layout_seq != t.layout_seq) rebuild_filter(f, t);
    if (f.stale) return nullptr;
    ++f.hits;
    if (out_count) *out_count = f.count;
    return &f.words;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// 2. Active caches
// ---------------------------------------------------------------------------

void ActiveDataEngine::rebuild_cache(ActiveCache& c, const SqlTable& t) {
  std::string err;
  if (!c.where_sql.empty()) c.where = SqlRowExpr::compile(db_, t, c.where_sql, &err);
  for (AggSpec& s : c.specs)
    if (s.kind != AggSpec::CountStar) s.expr = SqlRowExpr::compile(db_, t, s.expr_text, &err);
  c.total = GroupAcc();
  c.total.acc.resize(c.specs.size());
  const size_t n = t.row_count();
  for (size_t r = 0; r < n; ++r) {
    const SqlRow row = t.get_row(r);
    std::string e;
    if (c.where && !c.where->holds(row, &e)) continue;
    agg_apply(c.specs, c.total, row, +1);
  }
  c.stale = false;
}

SqlQueryResult ActiveDataEngine::create_cache(const std::string& sql, bool persist_it) {
  Cur c(sql);
  c.word("CREATE"); c.word("ACTIVE"); c.word("CACHE");
  const bool if_not_exists = c.word("IF") && c.word("NOT") && c.word("EXISTS");
  std::vector<std::string> nm;
  if (!c.qname(&nm) || nm.size() != 1) return error_result("expected CREATE ACTIVE CACHE name AS SELECT ...");
  if (!c.word("AS")) return error_result("expected AS SELECT ...");
  const std::string select_sql = c.rest();

  SelectParts parts;
  std::string err;
  if (!parse_select_parts(select_sql, false, &parts, &err)) return error_result("active cache: " + err);
  SqlTable* t = parts.table.size() >= 2 ? db_.find_table(parts.table.back(), parts.table[parts.table.size() - 2])
                                        : db_.find_table(parts.table[0], load_db_);
  if (!t) return error_result("table not found: " + parts.table.back());

  auto cache = std::make_shared<ActiveCache>();
  cache->name = nm[0];
  cache->db = db_or_default(t->database);
  cache->table_key = tkey(*t);
  cache->table_name = t->name;
  cache->query_sql = select_sql;
  cache->query_norm = normalize_sql(select_sql);
  cache->qualified = parts.table.size() >= 2;
  cache->where_sql = parts.where;
  for (const std::string& raw : parts.items) {
    const ParsedItem p = split_alias(raw);
    int kind = -1;
    std::string arg, perr;
    if (!parse_aggregate(p.text, &kind, &arg, &perr))
      return error_result("active cache: " + (perr.empty() ? "only COUNT, SUM, AVG, MIN and MAX are supported: " + raw : perr));
    AggSpec s;
    s.kind = (AggSpec::Kind)kind;
    s.label = p.alias.empty() ? p.text : p.alias;
    if (kind != 0) {
      s.expr_text = arg;
      std::string cerr;
      if (!SqlRowExpr::compile(db_, *t, arg, &cerr)) return error_result("active cache: " + cerr);
    }
    cache->specs.push_back(std::move(s));
  }
  if (!cache->where_sql.empty()) {
    std::string cerr;
    if (!SqlRowExpr::compile(db_, *t, cache->where_sql, &cerr)) return error_result("active cache: invalid WHERE: " + cerr);
  }

  const std::string key = lower(cache->db) + "." + lower(cache->name);
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  if (caches_.count(key)) return if_not_exists ? ok_result("cache already exists") : error_result("active cache already exists: " + cache->name);
  for (auto& kv : caches_)
    if (kv.second->db == cache->db && kv.second->query_norm == cache->query_norm)
      return error_result("active cache '" + kv.second->name + "' already answers that query");
  rebuild_cache(*cache, *t);
  caches_[key] = cache;
  cache_count_.store(caches_.size());
  recompute_watched();
  if (persist_it) persist("cache", cache->db, cache->name, sql);
  return ok_result("active cache '" + cache->name + "' on " + t->name + ": " + std::to_string(cache->total.rows) + " rows counted");
}

bool ActiveDataEngine::try_eval_cache(const std::string& sql, SqlQueryResult* out) {
  if (cache_count_.load(std::memory_order_relaxed) == 0) return false;
  {
    size_t i = 0;
    while (i < sql.size() && (std::isspace((unsigned char)sql[i]) || sql[i] == '(')) ++i;
    static const char kSelect[] = "select";
    if (sql.size() < i + 6) return false;
    for (size_t k = 0; k < 6; ++k)
      if (std::tolower((unsigned char)sql[i + k]) != kSelect[k]) return false;
  }
  const std::string norm = normalize_sql(sql);
  const std::string session_db = lower(db_or_default(db_.effective_database()));
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  for (auto& kv : caches_) {
    ActiveCache& c = *kv.second;
    if (c.query_norm != norm || (!c.qualified && lower(c.db) != session_db)) continue;
    auto it = db_.tables_.find(c.table_key);
    if (it == db_.tables_.end()) return false;
    for (const Acc& a : c.total.acc) if (a.min_stale || a.max_stale) c.stale = true;
    if (c.stale) rebuild_cache(c, it->second);
    ++c.hits;
    if (!out) return true;
    SqlQueryResult r;
    r.is_result_set = true;
    SqlRow row;
    for (size_t i = 0; i < c.specs.size(); ++i) {
      r.columns.push_back(c.specs[i].label);
      row.values.push_back(agg_render(c.specs[i], c.total, c.total.acc[i]));
      const AggSpec::Kind k = c.specs[i].kind;
      r.column_types.push_back(k == AggSpec::CountStar || k == AggSpec::Count ? SqlDataType::Int
                               : k == AggSpec::Sum || k == AggSpec::Avg ? SqlDataType::Decimal : SqlDataType::Text);
    }
    r.rows.push_back(std::move(row));
    r.rows_affected = 1;
    r.execution_route = "ACTIVE_CACHE";
    r.primary_table = c.table_name;
    *out = std::move(r);
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// 3. Active links
// ---------------------------------------------------------------------------

void ActiveDataEngine::link_attach_child(ActiveLink& l, size_t row, const std::string& key_text) {
  int64_t k;
  if (int_key(key_text, &k) != 1) return;
  auto p = l.parent_by_key.find(k);
  if (p != l.parent_by_key.end()) {
    l.child_to_parent[row] = p->second;
    auto& v = l.parent_to_children[p->second];
    v.insert(std::lower_bound(v.begin(), v.end(), (uint32_t)row), (uint32_t)row);
  } else {
    auto& v = l.orphans[k];
    v.insert(std::lower_bound(v.begin(), v.end(), (uint32_t)row), (uint32_t)row);
  }
}

void ActiveDataEngine::link_detach_child(ActiveLink& l, size_t row, const std::string& key_text) {
  int64_t k;
  const uint32_t pr = row < l.child_to_parent.size() ? l.child_to_parent[row] : kNone;
  if (pr != kNone) {
    auto& v = l.parent_to_children[pr];
    auto it = std::lower_bound(v.begin(), v.end(), (uint32_t)row);
    if (it != v.end() && *it == row) v.erase(it);
    l.child_to_parent[row] = kNone;
  } else if (int_key(key_text, &k) == 1) {
    auto o = l.orphans.find(k);
    if (o != l.orphans.end()) {
      auto it = std::lower_bound(o->second.begin(), o->second.end(), (uint32_t)row);
      if (it != o->second.end() && *it == row) o->second.erase(it);
      if (o->second.empty()) l.orphans.erase(o);
    }
  }
}

void ActiveDataEngine::link_attach_parent(ActiveLink& l, size_t row, const std::string& key_text) {
  int64_t k;
  if (int_key(key_text, &k) != 1) return;
  auto dup = l.parent_by_key.find(k);
  if (dup != l.parent_by_key.end() && dup->second != row) { l.usable = false; l.why_not = "the parent key is not unique"; l.stale = true; return; }
  l.parent_by_key[k] = (uint32_t)row;
  auto o = l.orphans.find(k);
  if (o != l.orphans.end()) {
    for (uint32_t ch : o->second) l.child_to_parent[ch] = (uint32_t)row;
    l.parent_to_children[row] = std::move(o->second);
    l.orphans.erase(o);
  }
}

void ActiveDataEngine::link_detach_parent(ActiveLink& l, size_t row, const std::string& key_text) {
  int64_t k;
  if (int_key(key_text, &k) != 1) return;
  auto it = l.parent_by_key.find(k);
  if (it == l.parent_by_key.end() || it->second != row) return;
  l.parent_by_key.erase(it);
  auto& kids = l.parent_to_children[row];
  if (!kids.empty()) {
    for (uint32_t ch : kids) l.child_to_parent[ch] = kNone;
    l.orphans[k] = std::move(kids);
    kids.clear();
  }
}

void ActiveDataEngine::rebuild_link(ActiveLink& l, const SqlTable& child, const SqlTable& parent) {
  // The columns by name: an ALTER may have moved them.
  const int ci = child.get_column_index(l.child_col_name), pi = parent.get_column_index(l.parent_col_name);
  l.child_to_parent.clear();
  l.parent_to_children.clear();
  l.parent_by_key.clear();
  l.orphans.clear();
  l.stale = false;
  if (ci < 0 || pi < 0 || child.columns[(size_t)ci].type != SqlDataType::Int || parent.columns[(size_t)pi].type != SqlDataType::Int) {
    l.usable = false;
    l.why_not = "a key column was dropped or is no longer an integer";
    return;
  }
  l.child_col = (size_t)ci;
  l.parent_col = (size_t)pi;
  l.child_to_parent.assign(child.row_count(), kNone);
  l.parent_to_children.assign(parent.row_count(), {});
  l.parent_by_key.clear();
  l.orphans.clear();
  l.usable = true;
  l.why_not.clear();
  l.child_rows = child.row_count();
  l.parent_rows = parent.row_count();
  l.child_layout = child.layout_seq;
  l.parent_layout = parent.layout_seq;
  l.stale = false;
  l.parent_by_key.reserve(parent.row_count());
  for (size_t r = 0; r < parent.row_count() && l.usable; ++r) {
    int64_t k;
    const int st = int_key(parent.cell(r, l.parent_col), &k);
    if (st < 0) { l.usable = false; l.why_not = "the parent key holds a value that is not an integer"; break; }
    if (st == 0) continue;
    if (!l.parent_by_key.emplace(k, (uint32_t)r).second) { l.usable = false; l.why_not = "the parent key is not unique"; }
  }
  for (size_t r = 0; r < child.row_count() && l.usable; ++r) {
    int64_t k;
    const int st = int_key(child.cell(r, l.child_col), &k);
    if (st < 0) { l.usable = false; l.why_not = "the child key holds a value that is not an integer"; break; }
    if (st == 0) continue;
    auto p = l.parent_by_key.find(k);
    if (p != l.parent_by_key.end()) {
      l.child_to_parent[r] = p->second;
      l.parent_to_children[p->second].push_back((uint32_t)r);
    } else {
      l.orphans[k].push_back((uint32_t)r);
    }
  }
  if (!l.usable) {
    l.child_to_parent.clear();
    l.parent_to_children.clear();
    l.parent_by_key.clear();
    l.orphans.clear();
  }
}

SqlQueryResult ActiveDataEngine::create_link(const std::string& sql, bool persist_it) {
  Cur c(sql);
  c.word("CREATE"); c.word("ACTIVE"); c.word("LINK");
  const bool if_not_exists = c.word("IF") && c.word("NOT") && c.word("EXISTS");
  std::vector<std::string> nm, ct, cc, pt, pc;
  if (!c.qname(&nm) || nm.size() != 1) return error_result("expected CREATE ACTIVE LINK name ON child(col) REFERENCES parent(col)");
  if (!c.word("ON") || !c.qname(&ct) || !c.ch('(') || !c.qname(&cc) || !c.ch(')'))
    return error_result("expected ON <child>(<column>)");
  if (!c.word("REFERENCES") || !c.qname(&pt) || !c.ch('(') || !c.qname(&pc) || !c.ch(')'))
    return error_result("expected REFERENCES <parent>(<column>)");
  if (!c.at_end()) return error_result("unexpected text after the link: " + c.rest());

  SqlTable* child = ct.size() >= 2 ? db_.find_table(ct.back(), ct[ct.size() - 2]) : db_.find_table(ct[0], load_db_);
  if (!child) return error_result("child table not found: " + ct.back());
  SqlTable* parent = pt.size() >= 2 ? db_.find_table(pt.back(), pt[pt.size() - 2]) : db_.find_table(pt[0], child->database);
  if (!parent) return error_result("parent table not found: " + pt.back());
  const int ci = child->get_column_index(cc.back());
  const int pi = parent->get_column_index(pc.back());
  if (ci < 0) return error_result("no column " + cc.back() + " in " + child->name);
  if (pi < 0) return error_result("no column " + pc.back() + " in " + parent->name);
  for (const auto& [tbl, idx] : {std::pair<SqlTable*, int>{child, ci}, std::pair<SqlTable*, int>{parent, pi}}) {
    const SqlColumn& col = tbl->columns[(size_t)idx];
    if (col.type != SqlDataType::Int || col.temporal)
      return error_result("an active link joins integer columns; " + tbl->name + "." + col.name + " is not one");
  }

  auto l = std::make_shared<ActiveLink>();
  l->name = nm[0];
  l->db = db_or_default(child->database);
  l->child_key = tkey(*child);
  l->parent_key = tkey(*parent);
  l->child_name = child->name;
  l->parent_name = parent->name;
  l->child_col_name = child->columns[(size_t)ci].name;
  l->parent_col_name = parent->columns[(size_t)pi].name;
  l->child_col = (size_t)ci;
  l->parent_col = (size_t)pi;

  const std::string key = lower(l->db) + "." + lower(l->name);
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  if (links_.count(key)) return if_not_exists ? ok_result("link already exists") : error_result("active link already exists: " + l->name);
  rebuild_link(*l, *child, *parent);
  if (!l->usable) return error_result("active link: " + l->why_not);
  links_[key] = l;
  recompute_watched();
  if (persist_it) persist("link", l->db, l->name, sql);
  return ok_result("active link '" + l->name + "': " + child->name + "." + l->child_col_name + " -> " + parent->name + "." +
                   l->parent_col_name + " (" + std::to_string(child->row_count()) + " child rows, " +
                   std::to_string(parent->row_count()) + " parent rows)");
}

ActiveDataEngine::LinkLookup ActiveDataEngine::find_link(const SqlTable& base, size_t base_col, const SqlTable& joined,
                                                        size_t joined_col) {
  LinkLookup none;
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  if (links_.empty()) return none;
  const std::string bk = tkey(base), jk = tkey(joined);
  for (auto& kv : links_) {
    ActiveLink& l = *kv.second;
    const bool to_parent = bk == l.child_key && jk == l.parent_key && base_col == l.child_col && joined_col == l.parent_col;
    const bool to_kids = bk == l.parent_key && jk == l.child_key && base_col == l.parent_col && joined_col == l.child_col;
    if (!to_parent && !to_kids) continue;
    auto ci = db_.tables_.find(l.child_key);
    auto pi = db_.tables_.find(l.parent_key);
    if (ci == db_.tables_.end() || pi == db_.tables_.end()) continue;
    const SqlTable& ct = ci->second;
    const SqlTable& pt = pi->second;
    if (l.stale || l.child_rows != ct.row_count() || l.parent_rows != pt.row_count() ||
        l.child_layout != ct.layout_seq || l.parent_layout != pt.layout_seq)
      rebuild_link(l, ct, pt);
    if (!l.usable) continue;
    ++l.hits;
    LinkLookup r;
    r.found = true;
    if (to_parent) r.to_parent = &l.child_to_parent;
    else r.to_children = &l.parent_to_children;
    return r;
  }
  return none;
}

// ---------------------------------------------------------------------------
// 4. Active views
// ---------------------------------------------------------------------------

namespace {

std::string group_key_of(const SqlRow& row, const std::vector<size_t>& cols) {
  std::string k;
  for (size_t i = 0; i < cols.size(); ++i) {
    const std::string& v = cols[i] < row.values.size() ? row.values[cols[i]] : sql_null();
    if (i) k += '\x1f';
    if (sql_is_null(v)) k += '\x01';
    else k += sql_collate_key(v);
  }
  return k;
}

}  // namespace

void ActiveDataEngine::rebuild_view_state(ActiveView& v, const SqlTable& t) {
  v.groups.clear();
  v.stale_groups.clear();
  v.broken = false;
  for (size_t k = 0; k < v.group_cols.size(); ++k) {
    const int gi = t.get_column_index(v.group_col_names[k]);
    if (gi < 0) { v.broken = true; v.state_stale = false; v.all_dirty = false; return; }
    v.group_cols[k] = (size_t)gi;
  }
  std::string err;
  if (!v.where_sql.empty()) v.where = SqlRowExpr::compile(db_, t, v.where_sql, &err);
  for (AggSpec& s : v.specs)
    if (s.kind != AggSpec::CountStar) s.expr = SqlRowExpr::compile(db_, t, s.expr_text, &err);
  v.groups.clear();
  const size_t n = t.row_count();
  for (size_t r = 0; r < n; ++r) {
    const SqlRow row = t.get_row(r);
    std::string e;
    if (v.where && !v.where->holds(row, &e)) continue;
    GroupAcc& g = v.groups[group_key_of(row, v.group_cols)];
    if (g.key_vals.empty())
      for (size_t c : v.group_cols) g.key_vals.push_back(c < row.values.size() ? row.values[c] : sql_null());
    agg_apply(v.specs, g, row, +1);
  }
  v.stale_groups.clear();
  v.state_stale = false;
  v.all_dirty = true;
}

SqlQueryResult ActiveDataEngine::create_view(const std::string& sql, bool persist_it) {
  Cur c(sql);
  c.word("CREATE"); c.word("ACTIVE"); c.word("VIEW");
  const bool if_not_exists = c.word("IF") && c.word("NOT") && c.word("EXISTS");
  std::vector<std::string> nm;
  if (!c.qname(&nm) || nm.size() > 2) return error_result("expected CREATE ACTIVE VIEW name AS SELECT ... GROUP BY ...");
  if (!c.word("AS")) return error_result("expected AS SELECT ...");
  const std::string select_sql = c.rest();

  SelectParts parts;
  std::string err;
  if (!parse_select_parts(select_sql, true, &parts, &err)) return error_result("active view: " + err);
  SqlTable* t = parts.table.size() >= 2 ? db_.find_table(parts.table.back(), parts.table[parts.table.size() - 2])
                                        : db_.find_table(parts.table[0], load_db_);
  if (!t) return error_result("table not found: " + parts.table.back());

  auto v = std::make_shared<ActiveView>();
  v->name = nm.back();
  v->db = nm.size() == 2 ? db_or_default(nm[0]) : db_or_default(t->database);
  v->table_key = tkey(*t);
  v->table_name = t->name;
  v->select_sql = select_sql;
  v->where_sql = parts.where;
  if (!v->where_sql.empty()) {
    std::string cerr;
    if (!SqlRowExpr::compile(db_, *t, v->where_sql, &cerr)) return error_result("active view: invalid WHERE: " + cerr);
  }
  // GROUP BY columns: plain columns of the table.
  for (const std::string& g : split_commas(parts.group_by)) {
    Cur gc(g);
    std::vector<std::string> gp;
    if (!gc.qname(&gp) || !gc.at_end()) return error_result("active view: GROUP BY takes plain columns, not '" + g + "'");
    const int gi = t->get_column_index(gp.back());
    if (gi < 0) return error_result("active view: no column " + gp.back() + " in " + t->name);
    v->group_cols.push_back((size_t)gi);
    v->group_col_names.push_back(t->columns[(size_t)gi].name);
  }
  // The select list: the group columns and aggregates.
  std::vector<std::string> col_types;
  std::vector<std::string> col_names;
  std::vector<bool> grouped_seen(v->group_cols.size(), false);
  for (const std::string& raw : parts.items) {
    const ParsedItem p = split_alias(raw);
    int kind = -1;
    std::string arg, perr;
    if (parse_aggregate(p.text, &kind, &arg, &perr)) {
      if (p.alias.empty())
        return error_result("active view: name the column of " + p.text + " (" + p.text + " AS name)");
      AggSpec s;
      s.kind = (AggSpec::Kind)kind;
      s.label = p.alias;
      if (kind != 0) {
        s.expr_text = arg;
        std::string cerr;
        if (!SqlRowExpr::compile(db_, *t, arg, &cerr)) return error_result("active view: " + cerr);
        Cur ac(arg);
        std::vector<std::string> ap;
        if (ac.qname(&ap) && ac.at_end()) {
          const int ai = t->get_column_index(ap.back());
          if (ai >= 0) s.plain_column = t->columns[(size_t)ai].name;
        }
      }
      const SqlColumn* pc = s.plain_column.empty() ? nullptr : &t->columns[(size_t)t->get_column_index(s.plain_column)];
      std::string type;
      switch (s.kind) {
        case AggSpec::CountStar: case AggSpec::Count: type = "BIGINT"; break;
        case AggSpec::Sum:
          type = !pc ? "DECIMAL(38,6)" : pc->type == SqlDataType::Int ? "DECIMAL(38,0)"
               : pc->type == SqlDataType::Decimal ? "DECIMAL(38," + std::to_string((int)pc->scale) + ")" : "DOUBLE";
          break;
        case AggSpec::Avg:
          type = !pc ? "DECIMAL(38,10)" : pc->type == SqlDataType::Int ? "DECIMAL(38,4)"
               : pc->type == SqlDataType::Decimal ? "DECIMAL(38," + std::to_string((int)pc->scale + 4) + ")" : "DOUBLE";
          break;
        case AggSpec::Min: case AggSpec::Max: type = pc ? sql_engine_column_type(*pc) : "VARCHAR(255)"; break;
      }
      col_types.push_back(type);
      col_names.push_back(p.alias);
      v->outs.push_back({false, v->specs.size(), p.alias});
      v->specs.push_back(std::move(s));
    } else {
      if (!perr.empty()) return error_result("active view: " + perr);
      Cur gc(p.text);
      std::vector<std::string> gp;
      if (!gc.qname(&gp) || !gc.at_end()) return error_result("active view: '" + raw + "' is neither a GROUP BY column nor an aggregate");
      const int gi = t->get_column_index(gp.back());
      size_t at = v->group_cols.size();
      for (size_t k = 0; k < v->group_cols.size(); ++k) if ((int)v->group_cols[k] == gi) at = k;
      if (at == v->group_cols.size()) return error_result("active view: " + gp.back() + " is not in the GROUP BY");
      grouped_seen[at] = true;
      col_types.push_back(sql_engine_column_type(t->columns[(size_t)gi]));
      col_names.push_back(p.alias.empty() ? t->columns[(size_t)gi].name : p.alias);
      v->outs.push_back({true, at, col_names.back()});
    }
  }
  for (size_t k = 0; k < grouped_seen.size(); ++k)
    if (!grouped_seen[k]) return error_result("active view: GROUP BY column " + t->columns[v->group_cols[k]].name + " must be selected");
  {
    std::set<std::string> seen;
    for (const std::string& n : col_names) if (!seen.insert(lower(n)).second) return error_result("active view: duplicate column " + n);
  }

  const std::string key = lower(v->db) + "." + lower(v->name);
  {
    std::lock_guard<aios::hal::Mutex> lk(mu_);
    if (views_.count(key)) return if_not_exists ? ok_result("active view already exists") : error_result("active view already exists: " + v->name);
  }
  // The output table. One that is already there is ours only when restoring after a restart.
  const std::string qual = quote_ident(v->db) + "." + quote_ident(v->name);
  if (db_.find_table(v->name, v->db) || db_.find_view(v->name))
    if (persist_it) return error_result("a table or view named " + v->name + " already exists");
  db_.execute_query("DROP TABLE IF EXISTS " + qual, v->db);
  std::string ddl = "CREATE TABLE " + qual + " (";
  for (size_t i = 0; i < col_names.size(); ++i) ddl += (i ? ", " : "") + quote_ident(col_names[i]) + " " + col_types[i];
  ddl += ")";
  SqlQueryResult made = db_.execute_query(ddl, v->db);
  if (!made.success) return error_result("active view: could not create its table: " + made.error_message);

  {
    std::lock_guard<aios::hal::Mutex> lk(mu_);
    rebuild_view_state(*v, *t);
    views_[key] = v;
    recompute_watched();
    if (!v->pending) { v->pending = true; pending_views_.fetch_add(1); }
  }
  // Written now, not at the statement's end: the caller reads the view next.
  settle();
  if (persist_it) persist("view", v->db, v->name, sql);
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  return ok_result("active view '" + v->name + "': " + std::to_string(v->groups.size()) + " groups");
}

void ActiveDataEngine::note_view_dirty(ActiveView& v, const std::string& gkey) {
  v.dirty.insert(gkey);
  if (!v.pending) { v.pending = true; pending_views_.fetch_add(1); }
}

void ActiveDataEngine::settle() {
  for (int round = 0; round < 8; ++round) {
    std::vector<std::shared_ptr<ActiveView>> due;
    {
      std::lock_guard<aios::hal::Mutex> lk(mu_);
      for (auto& kv : views_) if (kv.second->pending) due.push_back(kv.second);
    }
    if (due.empty()) return;
    for (auto& v : due) settle_view(*v);
  }
}

void ActiveDataEngine::settle_view(ActiveView& v) {
  std::vector<std::string> stmts;
  const std::string qual = quote_ident(v.db) + "." + quote_ident(v.name);
  {
    std::lock_guard<aios::hal::Mutex> lk(mu_);
    auto ti = db_.tables_.find(v.table_key);
    if (ti == db_.tables_.end()) { v.pending = false; pending_views_.fetch_sub(1); return; }
    const SqlTable& base = ti->second;
    if (v.state_stale) rebuild_view_state(v, base);
    if (v.broken) { v.pending = false; pending_views_.fetch_sub(1); return; }
    // A group whose MIN / MAX left is worked out again from the table.
    if (!v.stale_groups.empty() && !v.all_dirty) {
      for (const std::string& k : v.stale_groups) {
        auto g = v.groups.find(k);
        if (g == v.groups.end()) continue;
        g->second.rows = 0;
        g->second.acc.assign(v.specs.size(), Acc());
      }
      const size_t n = base.row_count();
      for (size_t r = 0; r < n; ++r) {
        const SqlRow row = base.get_row(r);
        std::string e;
        if (v.where && !v.where->holds(row, &e)) continue;
        const std::string k = group_key_of(row, v.group_cols);
        if (!v.stale_groups.count(k)) continue;
        agg_apply(v.specs, v.groups[k], row, +1);
      }
      for (const std::string& k : v.stale_groups) {
        v.dirty.insert(k);
        auto g = v.groups.find(k);
        if (g != v.groups.end() && g->second.rows <= 0) v.groups.erase(g);
      }
      v.stale_groups.clear();
    }
    auto row_values = [&](const GroupAcc& g) {
      std::string vals;
      for (size_t i = 0; i < v.outs.size(); ++i) {
        const ActiveView::Out& o = v.outs[i];
        vals += (i ? ", " : "") + quote_literal(o.is_group ? (o.idx < g.key_vals.size() ? g.key_vals[o.idx] : sql_null())
                                                           : agg_render(v.specs[o.idx], g, g.acc[o.idx]));
      }
      return vals;
    };
    auto delete_where = [&](const std::vector<std::string>& shown) {
      std::string w;
      for (size_t i = 0; i < v.outs.size(); ++i) {
        const ActiveView::Out& o = v.outs[i];
        if (!o.is_group) continue;
        const std::string& val = o.idx < shown.size() ? shown[o.idx] : sql_null();
        w += (w.empty() ? "" : " AND ") + quote_ident(o.label) + (sql_is_null(val) ? std::string(" IS NULL") : " = " + quote_literal(val));
      }
      return "DELETE FROM " + qual + " WHERE " + w;
    };
    if (v.all_dirty || v.dirty.size() > kPerGroupWrites) {
      stmts.push_back("DELETE FROM " + qual);
      v.shown.clear();
      std::string batch;
      size_t in_batch = 0;
      for (auto& kv : v.groups) {
        if (kv.second.rows <= 0) continue;
        batch += (in_batch ? ", (" : "(") + row_values(kv.second) + ")";
        v.shown[kv.first] = kv.second.key_vals;
        if (++in_batch == 500) { stmts.push_back("INSERT INTO " + qual + " VALUES " + batch); batch.clear(); in_batch = 0; }
      }
      if (in_batch) stmts.push_back("INSERT INTO " + qual + " VALUES " + batch);
    } else {
      for (const std::string& k : v.dirty) {
        // The row is removed by its group key whether or not this view believes it is shown: a rollback
        // puts the table's rows back without telling the view, and a second INSERT would double the group.
        auto s = v.shown.find(k);
        auto g = v.groups.find(k);
        const std::vector<std::string>* key_vals = s != v.shown.end() ? &s->second : g != v.groups.end() ? &g->second.key_vals : nullptr;
        if (key_vals) stmts.push_back(delete_where(*key_vals));
        if (s != v.shown.end()) v.shown.erase(s);
        if (g == v.groups.end()) continue;
        if (g->second.rows <= 0) { v.groups.erase(g); continue; }
        stmts.push_back("INSERT INTO " + qual + " VALUES (" + row_values(g->second) + ")");
        v.shown[k] = g->second.key_vals;
      }
    }
    v.dirty.clear();
    v.all_dirty = false;
    v.updates += 1;
    v.pending = false;
    pending_views_.fetch_sub(1);
  }
  // Written outside mu_: each statement is an ordinary write, and one into a table another object
  // watches reports back through on_row_change.
  for (const std::string& s : stmts) {
    SqlQueryResult r = db_.execute_query(s, v.db);
    if (!r.success) {
      std::fprintf(stderr, "[active] view %s: %s\n", v.name.c_str(), r.error_message.c_str());
      std::lock_guard<aios::hal::Mutex> lk(mu_);
      v.all_dirty = true;   // the table is written whole the next time
      if (!v.pending) { v.pending = true; pending_views_.fetch_add(1); }
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// Row changes
// ---------------------------------------------------------------------------

void ActiveDataEngine::on_row_change(const SqlTable& t, const SqlRow* before, const SqlRow* after, size_t row) {
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  const std::string tk = tkey(t);
  if (!watched_.count(tk)) return;

  for (auto& kv : filters_) {
    ActiveFilter& f = *kv.second;
    if (f.table_key != tk || f.stale) continue;
    ++f.mutations;
    if ((before && !after) || row == kNoRow || !f.expr) { f.stale = true; continue; }   // rows after a delete move
    std::string err;
    const bool match = f.expr->holds(*after, &err);
    if (!before) {   // INSERT
      if (row != f.synced_rows) { f.stale = true; continue; }
      if ((row >> 6) >= f.words.size()) f.words.resize((row >> 6) + 2, 0);
      if (match) { f.words[row >> 6] |= 1ULL << (row & 63); ++f.count; }
      ++f.synced_rows;
    } else {         // UPDATE
      if (row >= f.synced_rows) { f.stale = true; continue; }
      const uint64_t bit = 1ULL << (row & 63);
      const bool was = (f.words[row >> 6] & bit) != 0;
      if (match && !was) { f.words[row >> 6] |= bit; ++f.count; }
      else if (!match && was) { f.words[row >> 6] &= ~bit; --f.count; }
    }
    f.layout_seq = t.layout_seq;
  }

  for (auto& kv : caches_) {
    ActiveCache& c = *kv.second;
    if (c.table_key != tk || c.stale) continue;
    ++c.mutations;
    std::string err;
    if (before && (!c.where || c.where->holds(*before, &err))) agg_apply(c.specs, c.total, *before, -1);
    if (after && (!c.where || c.where->holds(*after, &err))) agg_apply(c.specs, c.total, *after, +1);
    if (c.total.rows < 0) c.stale = true;
  }

  for (auto& kv : links_) {
    ActiveLink& l = *kv.second;
    const bool is_child = l.child_key == tk, is_parent = l.parent_key == tk;
    if ((!is_child && !is_parent) || l.stale) continue;
    ++l.mutations;
    if ((before && !after) || row == kNoRow) { l.stale = true; continue; }
    if (is_child) {
      const std::string akey = after->values.size() > l.child_col ? after->values[l.child_col] : sql_null();
      if (!before) {
        if (row != l.child_rows) { l.stale = true; continue; }
        l.child_to_parent.push_back(kNone);
        ++l.child_rows;
        int64_t k;
        if (int_key(akey, &k) < 0) { l.usable = false; l.why_not = "the child key holds a value that is not an integer"; l.stale = true; continue; }
        link_attach_child(l, row, akey);
      } else {
        const std::string bkey = before->values.size() > l.child_col ? before->values[l.child_col] : sql_null();
        if (bkey != akey) {
          int64_t k;
          if (int_key(akey, &k) < 0) { l.usable = false; l.why_not = "the child key holds a value that is not an integer"; l.stale = true; continue; }
          link_detach_child(l, row, bkey);
          link_attach_child(l, row, akey);
        }
      }
    }
    if (is_parent && !l.stale) {
      const std::string akey = after->values.size() > l.parent_col ? after->values[l.parent_col] : sql_null();
      if (!before) {
        if (row != l.parent_rows) { l.stale = true; continue; }
        l.parent_to_children.emplace_back();
        ++l.parent_rows;
        int64_t k;
        if (int_key(akey, &k) < 0) { l.usable = false; l.why_not = "the parent key holds a value that is not an integer"; l.stale = true; continue; }
        link_attach_parent(l, row, akey);
      } else {
        const std::string bkey = before->values.size() > l.parent_col ? before->values[l.parent_col] : sql_null();
        if (bkey != akey) {
          int64_t k;
          if (int_key(akey, &k) < 0) { l.usable = false; l.why_not = "the parent key holds a value that is not an integer"; l.stale = true; continue; }
          link_detach_parent(l, row, bkey);
          link_attach_parent(l, row, akey);
        }
      }
    }
    if (!l.stale) {
      // A change that moved no encoding leaves the layout as it was; one that did is picked up on the next read.
      l.child_layout = is_child ? t.layout_seq : l.child_layout;
      l.parent_layout = is_parent ? t.layout_seq : l.parent_layout;
    }
  }

  for (auto& kv : views_) {
    ActiveView& v = *kv.second;
    if (v.table_key != tk || v.broken) continue;
    if (v.state_stale) {   // its groups are read from the table at settle
      if (!v.pending) { v.pending = true; pending_views_.fetch_add(1); }
      continue;
    }
    std::string err;
    for (int pass = 0; pass < 2; ++pass) {
      const SqlRow* img = pass == 0 ? before : after;
      if (!img || (v.where && !v.where->holds(*img, &err))) continue;
      const std::string gk = group_key_of(*img, v.group_cols);
      GroupAcc& g = v.groups[gk];
      if (pass == 1 || g.key_vals.empty()) {
        g.key_vals.clear();
        for (size_t c : v.group_cols) g.key_vals.push_back(c < img->values.size() ? img->values[c] : sql_null());
      }
      agg_apply(v.specs, g, *img, pass == 0 ? -1 : +1);
      note_view_dirty(v, gk);
      for (const Acc& a : g.acc)
        if (a.min_stale || a.max_stale) { v.stale_groups.insert(gk); break; }
    }
  }
}

void ActiveDataEngine::on_table_reset(const SqlTable& t) {
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  const std::string tk = tkey(t);
  for (auto& kv : filters_) if (kv.second->table_key == tk) kv.second->stale = true;
  for (auto& kv : caches_) if (kv.second->table_key == tk) kv.second->stale = true;
  for (auto& kv : links_) if (kv.second->child_key == tk || kv.second->parent_key == tk) kv.second->stale = true;
  for (auto& kv : views_) {
    ActiveView& v = *kv.second;
    if (v.table_key != tk) continue;
    v.state_stale = true;
    v.all_dirty = true;
    if (!v.pending) { v.pending = true; pending_views_.fetch_add(1); }
  }
}

void ActiveDataEngine::on_table_dropped(const SqlTable& t) {
  {
    std::lock_guard<aios::hal::Mutex> lk(mu_);
    const std::string tk = tkey(t);
    bool ended = false;
    // A view's own table dropped: the view is over. (A source table dropped leaves its objects waiting:
    // restoring a dump drops and creates the table again, and they are rebuilt from it.)
    for (auto it = views_.begin(); it != views_.end();) {
      if (lower(it->second->db) + "." + lower(it->second->name) == tk) {
        if (it->second->pending) pending_views_.fetch_sub(1);
        unpersist("view", it->second->db, it->second->name);
        it = views_.erase(it);
        ended = true;
      } else ++it;
    }
    if (ended) recompute_watched();
  }
  on_table_reset(t);
}

void ActiveDataEngine::on_schema_change() {
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  for (auto& kv : filters_) kv.second->stale = true;
  for (auto& kv : caches_) kv.second->stale = true;
  for (auto& kv : links_) kv.second->stale = true;
  for (auto& kv : views_) {
    kv.second->state_stale = true;
    kv.second->all_dirty = true;
    if (!kv.second->pending) { kv.second->pending = true; pending_views_.fetch_add(1); }
  }
}

// ---------------------------------------------------------------------------
// DROP and SHOW
// ---------------------------------------------------------------------------

SqlQueryResult ActiveDataEngine::drop_object(const std::string& sql, char kind) {
  Cur c(sql);
  c.word("DROP"); c.word("ACTIVE");
  c.word(kind == 'F' ? "FILTER" : kind == 'C' ? "CACHE" : kind == 'L' ? "LINK" : "VIEW");
  const bool if_exists = c.word("IF") && c.word("EXISTS");
  std::vector<std::string> nm;
  if (!c.qname(&nm) || nm.size() > 2 || !c.at_end()) return error_result("expected DROP ACTIVE <kind> [IF EXISTS] name");
  const std::string db = nm.size() == 2 ? nm[0] : db_.effective_database();
  const std::string key = lower(db_or_default(db)) + "." + lower(nm.back());
  std::string view_db, view_name;
  {
    std::lock_guard<aios::hal::Mutex> lk(mu_);
    bool found = false;
    switch (kind) {
      case 'F': { auto it = filters_.find(key); if ((found = it != filters_.end())) { unpersist("flt", it->second->db, it->second->name); filters_.erase(it); } break; }
      case 'C': { auto it = caches_.find(key); if ((found = it != caches_.end())) { unpersist("cache", it->second->db, it->second->name); caches_.erase(it); cache_count_.store(caches_.size()); } break; }
      case 'L': { auto it = links_.find(key); if ((found = it != links_.end())) { unpersist("link", it->second->db, it->second->name); links_.erase(it); } break; }
      case 'V': {
        auto it = views_.find(key);
        if ((found = it != views_.end())) {
          unpersist("view", it->second->db, it->second->name);
          view_db = it->second->db; view_name = it->second->name;
          if (it->second->pending) pending_views_.fetch_sub(1);
          views_.erase(it);
        }
        break;
      }
    }
    if (!found) return if_exists ? ok_result("") : error_result("active object not found: " + nm.back());
    recompute_watched();
  }
  if (!view_name.empty()) db_.execute_query("DROP TABLE IF EXISTS " + quote_ident(view_db) + "." + quote_ident(view_name), view_db);
  return ok_result("dropped active " + std::string(kind == 'F' ? "filter" : kind == 'C' ? "cache" : kind == 'L' ? "link" : "view") +
                   " '" + nm.back() + "'");
}

SqlQueryResult ActiveDataEngine::show(char kind) {
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  SqlQueryResult r;
  r.is_result_set = true;
  auto add = [&](std::vector<std::string> v) { SqlRow row; row.values = std::move(v); r.rows.push_back(std::move(row)); };
  if (kind == 'F') {
    r.columns = {"Filter", "Database", "Table", "Condition", "Matching Rows", "Table Rows", "State", "Reads", "Row Changes"};
    for (auto& kv : filters_) {
      const ActiveFilter& f = *kv.second;
      auto ti = db_.tables_.find(f.table_key);
      add({f.name, f.db, f.table_name, f.where_sql, std::to_string(f.count),
           ti == db_.tables_.end() ? "0" : std::to_string(ti->second.row_count()), f.stale ? "stale" : "current",
           std::to_string(f.hits), std::to_string(f.mutations)});
    }
  } else if (kind == 'C') {
    r.columns = {"Cache", "Database", "Table", "Query", "Rows Counted", "State", "Reads", "Row Changes"};
    for (auto& kv : caches_) {
      const ActiveCache& c = *kv.second;
      add({c.name, c.db, c.table_name, c.query_sql, std::to_string(c.total.rows), c.stale ? "stale" : "current",
           std::to_string(c.hits), std::to_string(c.mutations)});
    }
  } else if (kind == 'L') {
    r.columns = {"Link", "Database", "Child", "Parent", "State", "Joins Served", "Row Changes"};
    for (auto& kv : links_) {
      const ActiveLink& l = *kv.second;
      add({l.name, l.db, l.child_name + "." + l.child_col_name, l.parent_name + "." + l.parent_col_name,
           !l.usable ? "unusable: " + l.why_not : l.stale ? "stale" : "current", std::to_string(l.hits), std::to_string(l.mutations)});
    }
  } else {
    r.columns = {"View", "Database", "Source Table", "Query", "Groups", "Pending", "Applied Batches"};
    for (auto& kv : views_) {
      const ActiveView& v = *kv.second;
      add({v.name, v.db, v.table_name, v.select_sql, std::to_string(v.groups.size()), v.pending ? "yes" : "no", std::to_string(v.updates)});
    }
  }
  r.formatted_table = db_.format_result_table(r.columns, r.rows);
  return r;
}

SqlQueryResult ActiveDataEngine::show_status() {
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  SqlQueryResult r;
  r.is_result_set = true;
  r.columns = {"Kind", "Objects", "Reads Served"};
  auto add = [&](const char* k, size_t n, uint64_t hits) { SqlRow row; row.values = {k, std::to_string(n), std::to_string(hits)}; r.rows.push_back(std::move(row)); };
  uint64_t fh = 0, ch = 0, lh = 0, vu = 0;
  for (auto& kv : filters_) fh += kv.second->hits;
  for (auto& kv : caches_) ch += kv.second->hits;
  for (auto& kv : links_) lh += kv.second->hits;
  for (auto& kv : views_) vu += kv.second->updates;
  add("filter", filters_.size(), fh);
  add("cache", caches_.size(), ch);
  add("link", links_.size(), lh);
  add("view", views_.size(), vu);
  r.formatted_table = db_.format_result_table(r.columns, r.rows);
  return r;
}

// ---------------------------------------------------------------------------
// Tiering: what nothing has written for a while is folded and re-encoded
// ---------------------------------------------------------------------------

bool ActiveDataEngine::tick_due() {
  const uint64_t now = now_ns();
  uint64_t last = last_tick_ns_.load(std::memory_order_relaxed);
  if (now - last < 1000000000ull) return false;
  return last_tick_ns_.compare_exchange_strong(last, now);
}

std::vector<ActiveDataEngine::TierWork> ActiveDataEngine::tick_scan() {
  std::vector<TierWork> work;
  const uint64_t now = now_ns();
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  for (auto& kv : db_.tables_) {
    const SqlTable& t = kv.second;
    TableTelemetry& tel = telemetry_[kv.first];
    if (tel.changed_ns == 0 || tel.seen_seq != t.mutation_seq) {
      tel.seen_seq = t.mutation_seq;
      tel.changed_ns = now;
      tel.reencoded = false;
      tel.slack_released = false;
      continue;
    }
    const uint64_t idle = now - tel.changed_ns;
    TierWork w;
    w.key = kv.first;
    if (idle > 5000000000ull && t.delta_buffer.count.load(std::memory_order_relaxed) > 0) w.fold_delta = true;
    if (idle > 60000000000ull && !tel.reencoded && t.row_count() > 0 && t.row_count() <= kTierRowCap) w.reencode = true;
    if (idle > 30000000000ull && !tel.slack_released && t.row_count() > 0) w.release_slack = true;
    if (w.fold_delta || w.reencode || w.release_slack) work.push_back(std::move(w));
  }
  for (auto it = telemetry_.begin(); it != telemetry_.end();) it = db_.tables_.count(it->first) ? std::next(it) : telemetry_.erase(it);
  return work;
}

void ActiveDataEngine::tick_apply(const std::vector<TierWork>& work) {
  for (const TierWork& w : work) {
    auto it = db_.tables_.find(w.key);
    if (it == db_.tables_.end()) continue;
    SqlTable& t = it->second;
    if (w.fold_delta) t.compact_delta_buffer();
    if (w.reencode) t.optimize_storage(false);   // false: rows keep their positions
    if (w.release_slack) t.release_slack();
    std::lock_guard<aios::hal::Mutex> lk(mu_);
    TableTelemetry& tel = telemetry_[w.key];
    tel.seen_seq = t.mutation_seq;   // this was maintenance, not a write
    if (w.reencode) tel.reencoded = true;
    if (w.release_slack) tel.slack_released = true;
  }
}

void ActiveDataEngine::note_reencoded(const std::string& key, uint64_t seq) {
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  TableTelemetry& tel = telemetry_[key];
  tel.seen_seq = seq;   // this was maintenance, not a write
  tel.reencoded = true;
}

SqlQueryResult ActiveDataEngine::show_tiering() {
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  SqlQueryResult r;
  r.is_result_set = true;
  r.columns = {"Table", "Tier", "Rows", "Delta Buffer Rows", "Idle Seconds", "Storage"};
  const uint64_t now = now_ns();
  std::vector<std::string> keys;
  for (auto& kv : telemetry_) keys.push_back(kv.first);
  std::sort(keys.begin(), keys.end());
  for (const std::string& k : keys) {
    auto ti = db_.tables_.find(k);
    if (ti == db_.tables_.end()) continue;
    const TableTelemetry& tel = telemetry_[k];
    const uint64_t idle = tel.changed_ns ? (now - tel.changed_ns) / 1000000000ull : 0;
    SqlRow row;
    row.values = {k, idle > 60 ? "COLD" : idle > 5 ? "WARM" : "HOT", std::to_string(ti->second.row_count()),
                  std::to_string(ti->second.delta_buffer.count.load(std::memory_order_relaxed)), std::to_string(idle),
                  tel.reencoded ? "re-encoded" : "as written"};
    r.rows.push_back(std::move(row));
  }
  r.formatted_table = db_.format_result_table(r.columns, r.rows);
  return r;
}

}  // namespace aios::userland
