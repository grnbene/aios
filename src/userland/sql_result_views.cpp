#include "aios/userland/sql_result_views.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "aios/hal/sync.hpp"
#include "aios/net/gzip.hpp"
#include "aios/userland/sql_database.hpp"

namespace aios::userland {

namespace {

constexpr size_t kChunkBytes = 1u << 20;

// MariaDB's latin1 is cp1252; the same mapping mysql_wire.cpp sends a latin1 session.
constexpr uint16_t kCp1252High[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

std::string utf8_to_cp1252(const std::string& s) {
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

std::string cp1252_to_utf8(const std::string& s) {
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

struct JVal {
  enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<JVal> arr;
  std::vector<std::pair<std::string, JVal>> obj;
  const JVal* get(const char* key) const {
    for (const auto& kv : obj) if (kv.first == key) return &kv.second;
    return nullptr;
  }
};

void put_utf8(std::string* out, uint32_t cp) {
  if (cp < 0x80) out->push_back((char)cp);
  else if (cp < 0x800) { out->push_back((char)(0xC0 | (cp >> 6))); out->push_back((char)(0x80 | (cp & 0x3F))); }
  else if (cp < 0x10000) {
    out->push_back((char)(0xE0 | (cp >> 12)));
    out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back((char)(0x80 | (cp & 0x3F)));
  } else {
    out->push_back((char)(0xF0 | (cp >> 18)));
    out->push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back((char)(0x80 | (cp & 0x3F)));
  }
}

struct JParser {
  const std::string& t;
  size_t i = 0;
  std::string err;
  explicit JParser(const std::string& text) : t(text) {}
  void ws() { while (i < t.size() && std::isspace((unsigned char)t[i])) ++i; }
  bool fail(const char* m) { if (err.empty()) err = m; return false; }
  bool hex4(uint32_t* v) {
    if (i + 4 > t.size()) return fail("short \\u escape");
    *v = 0;
    for (int k = 0; k < 4; ++k) {
      const char c = t[i++];
      *v <<= 4;
      if (c >= '0' && c <= '9') *v |= (uint32_t)(c - '0');
      else if (c >= 'a' && c <= 'f') *v |= (uint32_t)(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') *v |= (uint32_t)(c - 'A' + 10);
      else return fail("bad \\u escape");
    }
    return true;
  }
  bool str(std::string* out) {
    if (i >= t.size() || t[i] != '"') return fail("expected a string");
    ++i;
    while (i < t.size() && t[i] != '"') {
      char c = t[i++];
      if (c != '\\') { out->push_back(c); continue; }
      if (i >= t.size()) return fail("dangling escape");
      c = t[i++];
      switch (c) {
        case 'n': out->push_back('\n'); break;
        case 't': out->push_back('\t'); break;
        case 'r': out->push_back('\r'); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'u': {
          uint32_t cp;
          if (!hex4(&cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < t.size() && t[i] == '\\' && t[i + 1] == 'u') {
            i += 2;
            uint32_t lo;
            if (!hex4(&lo)) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          put_utf8(out, cp);
          break;
        }
        default: out->push_back(c);
      }
    }
    if (i >= t.size()) return fail("unterminated string");
    ++i;
    return true;
  }
  bool value(JVal* v, int depth = 0) {
    if (depth > 32) return fail("nested too deep");
    ws();
    if (i >= t.size()) return fail("unexpected end");
    const char c = t[i];
    if (c == '{') {
      v->kind = JVal::Obj;
      ++i;
      ws();
      if (i < t.size() && t[i] == '}') { ++i; return true; }
      for (;;) {
        ws();
        std::string k;
        if (!str(&k)) return false;
        ws();
        if (i >= t.size() || t[i] != ':') return fail("expected ':'");
        ++i;
        JVal child;
        if (!value(&child, depth + 1)) return false;
        v->obj.emplace_back(std::move(k), std::move(child));
        ws();
        if (i < t.size() && t[i] == ',') { ++i; continue; }
        if (i < t.size() && t[i] == '}') { ++i; return true; }
        return fail("expected ',' or '}'");
      }
    }
    if (c == '[') {
      v->kind = JVal::Arr;
      ++i;
      ws();
      if (i < t.size() && t[i] == ']') { ++i; return true; }
      for (;;) {
        JVal child;
        if (!value(&child, depth + 1)) return false;
        v->arr.push_back(std::move(child));
        ws();
        if (i < t.size() && t[i] == ',') { ++i; continue; }
        if (i < t.size() && t[i] == ']') { ++i; return true; }
        return fail("expected ',' or ']'");
      }
    }
    if (c == '"') { v->kind = JVal::Str; return str(&v->s); }
    if (t.compare(i, 4, "true") == 0) { v->kind = JVal::Bool; v->b = true; i += 4; return true; }
    if (t.compare(i, 5, "false") == 0) { v->kind = JVal::Bool; i += 5; return true; }
    if (t.compare(i, 4, "null") == 0) { i += 4; return true; }
    char* end = nullptr;
    const double d = std::strtod(t.c_str() + i, &end);
    if (end == t.c_str() + i) return fail("unexpected character");
    v->kind = JVal::Num;
    v->n = d;
    i = (size_t)(end - t.c_str());
    return true;
  }
};

bool valid_utf8(const std::string& s) {
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = (unsigned char)s[i];
    if (c < 0x80) { ++i; continue; }
    size_t n = 0;
    if (c >= 0xC2 && c <= 0xDF) n = 1;
    else if (c >= 0xE0 && c <= 0xEF) n = 2;
    else if (c >= 0xF0 && c <= 0xF4) n = 3;
    else return false;
    if (i + n >= s.size()) return false;
    for (size_t k = 1; k <= n; ++k) if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
    if (c == 0xE0 && (unsigned char)s[i + 1] < 0xA0) return false;
    if (c == 0xED && (unsigned char)s[i + 1] > 0x9F) return false;
    if (c == 0xF0 && (unsigned char)s[i + 1] < 0x90) return false;
    if (c == 0xF4 && (unsigned char)s[i + 1] > 0x8F) return false;
    i += n + 1;
  }
  return true;
}

bool has_high(const std::string& s) {
  for (unsigned char c : s) if (c >= 0x80) return true;
  return false;
}

// How stored text is shown, always as valid UTF-8:
//   Utf8        the stored text (bytes that are not UTF-8 read as cp1252)
//   Latin1      what a latin1 session reads, taken as UTF-8 when it is (murken's
//               double-encoded text comes out right), else as cp1252
//   Latin1Iso   what a latin1 session reads, each byte as ISO-8859-1 - exactly
//               PHP's utf8_encode(), which is what PhpReports' pages show today
enum class TextMode { Utf8 = 0, Latin1 = 1, Latin1Iso = 2 };
constexpr int kTextModes = 3;

std::string iso_to_utf8(const std::string& s) {
  std::string out;
  out.reserve(s.size() + s.size() / 4);
  for (unsigned char c : s) {
    if (c < 0x80) out.push_back((char)c);
    else { out.push_back((char)(0xC0 | (c >> 6))); out.push_back((char)(0x80 | (c & 0x3F))); }
  }
  return out;
}

std::string display_text(const std::string& v, TextMode mode) {
  if (!has_high(v)) return v;
  if (mode == TextMode::Latin1Iso) return iso_to_utf8(utf8_to_cp1252(v));
  if (mode == TextMode::Latin1) {
    std::string b = utf8_to_cp1252(v);
    return valid_utf8(b) ? b : cp1252_to_utf8(b);
  }
  return valid_utf8(v) ? v : cp1252_to_utf8(v);
}

const char* mode_name(TextMode m) { return m == TextMode::Latin1 ? "latin1" : m == TextMode::Latin1Iso ? "latin1_iso" : "utf8"; }

void put_json_str(std::string* out, const std::string& s) {
  out->push_back('"');
  for (size_t i = 0; i < s.size(); ++i) {
    const unsigned char c = (unsigned char)s[i];
    if (c == '"' || c == '\\') { out->push_back('\\'); out->push_back((char)c); continue; }
    if (c < 0x20) {
      static const char kHex[] = "0123456789abcdef";
      switch (c) {
        case '\n': out->append("\\n"); break;
        case '\r': out->append("\\r"); break;
        case '\t': out->append("\\t"); break;
        default: out->append("\\u00"); out->push_back(kHex[c >> 4]); out->push_back(kHex[c & 15]);
      }
      continue;
    }
    // U+2028/U+2029 end a line in a JavaScript string literal.
    if (c == 0xE2 && i + 2 < s.size() && (unsigned char)s[i + 1] == 0x80 &&
        ((unsigned char)s[i + 2] == 0xA8 || (unsigned char)s[i + 2] == 0xA9)) {
      out->append((unsigned char)s[i + 2] == 0xA8 ? "\\u2028" : "\\u2029");
      i += 2;
      continue;
    }
    out->push_back((char)c);
  }
  out->push_back('"');
}

bool parses_as_number(const std::string& v, double* out) {
  if (v.empty() || v.size() > 64) return false;
  const char* b = v.c_str();
  char* end = nullptr;
  const double d = std::strtod(b, &end);
  if (end != b + v.size() || !std::isfinite(d)) return false;
  if (std::isspace((unsigned char)v[0])) return false;
  *out = d;
  return true;
}

uint64_t fnv(uint64_t h, const void* p, size_t n) {
  const unsigned char* b = (const unsigned char*)p;
  for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
  return h;
}

std::string lower_ascii(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

struct ViewEntry {
  std::string key;
  std::string sql;
  std::shared_ptr<const SqlQueryResult> res;
  std::vector<bool> numeric;   // per column: every non-null value reads as a number
  std::string version;
  std::vector<std::string> deps;
  bool all = false;
  size_t bytes = 0;
  std::atomic<uint64_t> hits{0};
  uint64_t built_at_ms = 0;
  double build_ms = 0;

  hal::Mutex mu;   // the lazily built pieces below
  std::map<std::string, std::shared_ptr<const std::string>> full;   // "<text>|<gzip>" -> payload
  std::vector<std::shared_ptr<const std::vector<double>>> num_keys;
  std::vector<std::shared_ptr<const std::vector<std::string>>> text_keys;
  std::shared_ptr<const std::vector<std::string>> haystack[kTextModes];   // per text mode: lower-cased row text
};

uint64_t now_ms() {
  return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

struct ResultViewStore {
  hal::Mutex mu;
  std::list<std::shared_ptr<ViewEntry>> lru;
  std::unordered_map<std::string, std::list<std::shared_ptr<ViewEntry>>::iterator> by_key;
  std::unordered_map<std::string, std::unordered_set<std::string>> by_dep;   // table -> keys
  std::unordered_set<std::string> all_keys;
  std::unordered_map<std::string, uint64_t> dropped_at;   // table -> seq of its last write
  uint64_t dropped_all_at = 0;
  uint64_t seq = 1;
  size_t bytes = 0;
  size_t budget = 0;
  uint64_t builds = 0, hits = 0, drops = 0;
  // Read by every write without the lock: with no view kept and none being
  // built, a write has nothing to drop and no build to warn (one that starts
  // later reads the data as the write left it). The lock and a map insert per
  // write statement were contended by every writing session.
  std::atomic<int> building{0};
  std::atomic<bool> any_stored{false};

  void erase_locked(std::list<std::shared_ptr<ViewEntry>>::iterator it) {
    const auto& e = *it;
    for (const auto& d : e->deps) {
      auto b = by_dep.find(d);
      if (b != by_dep.end()) { b->second.erase(e->key); if (b->second.empty()) by_dep.erase(b); }
    }
    all_keys.erase(e->key);
    bytes -= std::min(bytes, e->bytes);
    by_key.erase(e->key);
    lru.erase(it);
  }
};

std::shared_ptr<ResultViewStore> sql_result_views_make() {
  auto s = std::make_shared<ResultViewStore>();
  const char* e = std::getenv("AIOS_RESULT_VIEW_MB");
  const unsigned long mb = e && *e ? std::strtoul(e, nullptr, 10) : 256;
  s->budget = (size_t)(mb ? mb : 256) << 20;
  return s;
}

void sql_result_views_invalidate(ResultViewStore* store, const std::vector<std::string>& tables) {
  if (!store) return;
  if (store->building.load(std::memory_order_seq_cst) == 0 && !store->any_stored.load(std::memory_order_seq_cst)) return;
  hal::LockGuard lock(store->mu);
  const uint64_t seq = ++store->seq;
  // The drop marks are kept even with nothing stored: a view being built
  // across this write finds out from them. The set of keys to drop is only
  // built when there is something to drop (every write statement runs this).
  for (const auto& t : tables) store->dropped_at[lower_ascii(t)] = seq;
  if (store->by_key.empty()) return;
  std::unordered_set<std::string> keys(store->all_keys.begin(), store->all_keys.end());
  for (const auto& t : tables) {
    auto b = store->by_dep.find(lower_ascii(t));
    if (b != store->by_dep.end()) keys.insert(b->second.begin(), b->second.end());
  }
  for (const auto& k : keys) {
    auto it = store->by_key.find(k);
    if (it != store->by_key.end()) { store->erase_locked(it->second); ++store->drops; }
  }
}

void sql_result_views_invalidate_all(ResultViewStore* store) {
  if (!store) return;
  hal::LockGuard lock(store->mu);
  store->dropped_all_at = ++store->seq;
  store->drops += store->lru.size();
  store->lru.clear();
  store->by_key.clear();
  store->by_dep.clear();
  store->all_keys.clear();
  store->bytes = 0;
}

bool sql_is_result_view(const std::string& sql) {
  size_t i = 0;
  auto word = [&](const char* w) {
    while (i < sql.size() && std::isspace((unsigned char)sql[i])) ++i;
    const size_t n = std::strlen(w);
    if (i + n > sql.size()) return false;
    for (size_t k = 0; k < n; ++k) if (std::toupper((unsigned char)sql[i + k]) != w[k]) return false;
    if (i + n < sql.size() && std::isalnum((unsigned char)sql[i + n])) return false;
    i += n;
    return true;
  };
  return word("CALL") && word("VIEW");
}

namespace {

SqlQueryResult view_error(const std::string& m) {
  SqlQueryResult r;
  r.success = false;
  r.error_message = m;
  return r;
}

// The SQL string literal at `i` (quote ' or "), unescaped the way MariaDB reads it.
bool read_sql_literal(const std::string& s, size_t* i, std::string* out) {
  while (*i < s.size() && std::isspace((unsigned char)s[*i])) ++*i;
  if (*i >= s.size() || (s[*i] != '\'' && s[*i] != '"')) return false;
  const char q = s[(*i)++];
  while (*i < s.size()) {
    char c = s[(*i)++];
    if (c == q) {
      if (*i < s.size() && s[*i] == q) { out->push_back(q); ++*i; continue; }
      return true;
    }
    if (c == '\\' && *i < s.size()) {
      c = s[(*i)++];
      switch (c) {
        case '0': out->push_back('\0'); break;
        case 'n': out->push_back('\n'); break;
        case 'r': out->push_back('\r'); break;
        case 't': out->push_back('\t'); break;
        case 'Z': out->push_back('\x1a'); break;
        case 'b': out->push_back('\b'); break;
        case '%': case '_': out->push_back('\\'); out->push_back(c); break;   // MariaDB keeps these two
        default: out->push_back(c);
      }
      continue;
    }
    out->push_back(c);
  }
  return false;
}

// Every name the statement mentions, lower-cased: a superset of the tables it
// reads, the rule the query cache uses (a column sharing a table's name only
// costs an extra rebuild).
std::vector<std::string> statement_names(const std::string& sql) {
  std::vector<std::string> out;
  std::unordered_set<std::string> seen;
  for (size_t i = 0; i < sql.size();) {
    const char c = sql[i];
    if (c == '\'' || c == '"') {
      const char q = c;
      ++i;
      while (i < sql.size() && sql[i] != q) i += (sql[i] == '\\' && i + 1 < sql.size()) ? 2 : 1;
      ++i;
      continue;
    }
    if (c == '`') {
      const size_t e = sql.find('`', i + 1);
      const std::string n = lower_ascii(sql.substr(i + 1, (e == std::string::npos ? sql.size() : e) - i - 1));
      if (!n.empty() && seen.insert(n).second) out.push_back(n);
      i = e == std::string::npos ? sql.size() : e + 1;
      continue;
    }
    if (std::isalpha((unsigned char)c) || c == '_') {
      size_t e = i;
      while (e < sql.size() && (std::isalnum((unsigned char)sql[e]) || sql[e] == '_' || sql[e] == '$')) ++e;
      const std::string n = lower_ascii(sql.substr(i, e - i));
      if (seen.insert(n).second) out.push_back(n);
      i = e;
      continue;
    }
    ++i;
  }
  return out;
}

std::string first_word_upper(const std::string& s) {
  size_t i = 0;
  while (i < s.size() && (std::isspace((unsigned char)s[i]) || s[i] == '(')) ++i;
  size_t e = i;
  while (e < s.size() && std::isalpha((unsigned char)s[e])) ++e;
  std::string w = s.substr(i, e - i);
  for (auto& c : w) c = (char)std::toupper((unsigned char)c);
  return w;
}

std::shared_ptr<ViewEntry> build_entry(const std::string& key, const std::string& sql, SqlQueryResult&& res) {
  auto e = std::make_shared<ViewEntry>();
  e->key = key;
  e->sql = sql;
  const size_t ncol = res.columns.size();
  e->numeric.assign(ncol, true);
  std::vector<bool> any(ncol, false);
  uint64_t h = 1469598103934665603ull;
  size_t bytes = 0;
  for (const auto& c : res.columns) { h = fnv(h, c.data(), c.size()); h = fnv(h, "\x1f", 1); }
  for (const auto& row : res.rows) {
    for (size_t c = 0; c < ncol; ++c) {
      const std::string& v = c < row.values.size() ? row.values[c] : sql_null();
      bytes += v.size() + 32;
      if (sql_is_null(v)) { h = fnv(h, "\x00N", 2); continue; }
      h = fnv(h, v.data(), v.size());
      h = fnv(h, "\x1f", 1);
      if (e->numeric[c]) {
        double d;
        if (parses_as_number(v, &d)) any[c] = true;
        else e->numeric[c] = false;
      }
    }
    h = fnv(h, "\x1e", 1);
  }
  for (size_t c = 0; c < ncol; ++c) if (!any[c]) e->numeric[c] = false;
  char hex[17];
  std::snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)h);
  e->version = hex;
  e->bytes = bytes * 2;   // room for the payloads and sort keys built later
  e->num_keys.resize(ncol);
  e->text_keys.resize(ncol);
  e->res = std::make_shared<const SqlQueryResult>(std::move(res));
  e->built_at_ms = now_ms();
  return e;
}

void write_header(std::string* out, const ViewEntry& e, const JVal* draw, size_t total, size_t filtered) {
  out->append("{\"version\":\"");
  out->append(e.version);
  out->append("\"");
  if (draw && draw->kind == JVal::Num) out->append(",\"draw\":" + std::to_string((long long)draw->n));
  out->append(",\"columns\":[");
  for (size_t c = 0; c < e.res->columns.size(); ++c) {
    if (c) out->push_back(',');
    put_json_str(out, e.res->columns[c]);
  }
  out->append("],\"recordsTotal\":" + std::to_string(total) + ",\"recordsFiltered\":" + std::to_string(filtered) +
              ",\"rows\":[");
}

void write_row(std::string* out, const SqlRow& row, size_t ncol, TextMode mode, bool first) {
  if (!first) out->push_back(',');
  out->push_back('[');
  for (size_t c = 0; c < ncol; ++c) {
    if (c) out->push_back(',');
    const std::string& v = c < row.values.size() ? row.values[c] : sql_null();
    if (sql_is_null(v)) out->append("null");
    else put_json_str(out, display_text(v, mode));
  }
  out->push_back(']');
}

std::shared_ptr<const std::vector<double>> num_keys_of(ViewEntry& e, size_t c) {
  if (e.num_keys[c]) return e.num_keys[c];
  auto k = std::make_shared<std::vector<double>>();
  k->reserve(e.res->rows.size());
  for (const auto& row : e.res->rows) {
    const std::string& v = c < row.values.size() ? row.values[c] : sql_null();
    double d = -HUGE_VAL;   // NULL sorts first, as in MariaDB
    if (!sql_is_null(v)) parses_as_number(v, &d);
    k->push_back(d);
  }
  e.num_keys[c] = k;
  return k;
}

std::shared_ptr<const std::vector<std::string>> text_keys_of(ViewEntry& e, size_t c) {
  if (e.text_keys[c]) return e.text_keys[c];
  auto k = std::make_shared<std::vector<std::string>>();
  k->reserve(e.res->rows.size());
  for (const auto& row : e.res->rows) {
    const std::string& v = c < row.values.size() ? row.values[c] : sql_null();
    k->push_back(sql_is_null(v) ? std::string() : std::string(1, '\x01') + sql_collate_key(v));
  }
  e.text_keys[c] = k;
  return k;
}

std::shared_ptr<const std::vector<std::string>> haystack_of(ViewEntry& e, TextMode mode) {
  auto& slot = e.haystack[(int)mode];
  if (slot) return slot;
  auto hs = std::make_shared<std::vector<std::string>>();
  hs->reserve(e.res->rows.size());
  const size_t ncol = e.res->columns.size();
  for (const auto& row : e.res->rows) {
    std::string line;
    for (size_t c = 0; c < ncol; ++c) {
      const std::string& v = c < row.values.size() ? row.values[c] : sql_null();
      if (sql_is_null(v)) continue;
      line += lower_ascii(display_text(v, mode));
      line.push_back('\x1f');
    }
    hs->push_back(std::move(line));
  }
  slot = hs;
  return slot;
}

std::string window_payload(ViewEntry& e, const JVal& win, const JVal* draw, TextMode mode) {
  const auto& rows = e.res->rows;
  const size_t ncol = e.res->columns.size();
  std::vector<uint32_t> idx(rows.size());
  for (size_t r = 0; r < rows.size(); ++r) idx[r] = (uint32_t)r;

  std::vector<std::string> terms;
  if (const JVal* s = win.get("search"); s && s->kind == JVal::Str) {
    const std::string q = lower_ascii(s->s);
    for (size_t i = 0; i < q.size();) {
      while (i < q.size() && q[i] == ' ') ++i;
      size_t j = i;
      while (j < q.size() && q[j] != ' ') ++j;
      if (j > i) terms.push_back(q.substr(i, j - i));
      i = j;
    }
  }
  struct Key { size_t col; bool desc; std::shared_ptr<const std::vector<double>> num; std::shared_ptr<const std::vector<std::string>> text; };
  std::vector<Key> keys;
  {
    hal::LockGuard lock(e.mu);
    if (!terms.empty()) {
      const auto hs = haystack_of(e, mode);
      std::vector<uint32_t> kept;
      for (uint32_t r : idx) {
        bool ok = true;
        for (const auto& t : terms) if ((*hs)[r].find(t) == std::string::npos) { ok = false; break; }
        if (ok) kept.push_back(r);
      }
      idx.swap(kept);
    }
    if (const JVal* o = win.get("order"); o && o->kind == JVal::Arr) {
      for (const auto& spec : o->arr) {
        if (spec.kind != JVal::Arr || spec.arr.empty() || spec.arr[0].kind != JVal::Num) continue;
        const double cd = spec.arr[0].n;
        if (cd < 0 || cd >= (double)ncol) continue;
        const size_t c = (size_t)cd;
        const bool desc = spec.arr.size() > 1 && spec.arr[1].kind == JVal::Str && lower_ascii(spec.arr[1].s) == "desc";
        Key k{c, desc, nullptr, nullptr};
        if (e.numeric[c]) k.num = num_keys_of(e, c);
        else k.text = text_keys_of(e, c);
        keys.push_back(std::move(k));
      }
    }
  }
  if (!keys.empty()) {
    std::stable_sort(idx.begin(), idx.end(), [&](uint32_t a, uint32_t b) {
      for (const auto& k : keys) {
        int cmp;
        if (k.num) cmp = (*k.num)[a] < (*k.num)[b] ? -1 : (*k.num)[a] > (*k.num)[b] ? 1 : 0;
        else cmp = (*k.text)[a].compare((*k.text)[b]);
        if (cmp) return k.desc ? cmp > 0 : cmp < 0;
      }
      return false;
    });
  }
  size_t start = 0, length = idx.size();
  if (const JVal* s = win.get("start"); s && s->kind == JVal::Num && s->n > 0) start = (size_t)s->n;
  if (const JVal* l = win.get("length"); l && l->kind == JVal::Num && l->n >= 0) length = (size_t)l->n;
  if (start > idx.size()) start = idx.size();
  const size_t end = std::min(idx.size(), start + length);

  std::string out;
  write_header(&out, e, draw, rows.size(), idx.size());
  for (size_t p = start; p < end; ++p) write_row(&out, rows[idx[p]], ncol, mode, p == start);
  out.append("]}");
  return out;
}

std::shared_ptr<const std::string> full_payload(ViewEntry& e, TextMode mode, bool gzip) {
  const std::string slot = std::string(mode_name(mode)) + (gzip ? "|gz" : "");
  {
    hal::LockGuard lock(e.mu);
    auto it = e.full.find(slot);
    if (it != e.full.end()) return it->second;
  }
  std::shared_ptr<const std::string> plain;
  if (gzip) plain = full_payload(e, mode, false);
  std::string out;
  if (gzip) {
    const ByteBuffer z = net::gzip_compress_whole(ByteSpan(reinterpret_cast<const uint8_t*>(plain->data()), plain->size()));
    out.assign(reinterpret_cast<const char*>(z.data()), z.size());
  } else {
    const auto& rows = e.res->rows;
    const size_t ncol = e.res->columns.size();
    out.reserve(e.bytes / 2 + rows.size() * ncol * 4 + 256);
    write_header(&out, e, nullptr, rows.size(), rows.size());
    for (size_t r = 0; r < rows.size(); ++r) write_row(&out, rows[r], ncol, mode, r == 0);
    out.append("]}");
  }
  auto p = std::make_shared<const std::string>(std::move(out));
  hal::LockGuard lock(e.mu);
  return e.full.emplace(slot, p).first->second;
}

SqlQueryResult answer(const std::string& status, const std::string& version, size_t total, size_t filtered,
                      const std::string& encoding, const std::string* payload) {
  SqlQueryResult r;
  r.is_result_set = true;
  r.columns = {"status", "version", "rows_total", "rows_filtered", "encoding", "chunk"};
  r.column_origins.resize(r.columns.size());
  r.column_origins[5].binary = 1;   // bytes to the client: no latin1 re-encoding
  auto row = [&](std::string chunk) {
    r.rows.push_back(SqlRow{{status, version, std::to_string(total), std::to_string(filtered), encoding, std::move(chunk)}});
  };
  if (!payload || payload->empty()) { row(std::string()); return r; }
  for (size_t off = 0; off < payload->size(); off += kChunkBytes) row(payload->substr(off, kChunkBytes));
  return r;
}

SqlQueryResult status_answer(ResultViewStore& st) {
  SqlQueryResult r;
  r.is_result_set = true;
  r.columns = {"statement", "rows", "columns", "version", "mbytes", "hits", "age_s", "build_ms"};
  const uint64_t now = now_ms();
  hal::LockGuard lock(st.mu);
  for (const auto& e : st.lru) {
    r.rows.push_back(SqlRow{{e->sql.substr(0, 200), std::to_string(e->res->rows.size()),
                             std::to_string(e->res->columns.size()), e->version,
                             std::to_string(e->bytes >> 20), std::to_string(e->hits.load()),
                             std::to_string((now - e->built_at_ms) / 1000), std::to_string((long long)e->build_ms)}});
  }
  r.rows.push_back(SqlRow{{"(total)", std::to_string(st.lru.size()), "", "",
                           std::to_string(st.bytes >> 20) + " of " + std::to_string(st.budget >> 20),
                           std::to_string(st.hits), "builds " + std::to_string(st.builds), "drops " + std::to_string(st.drops)}});
  return r;
}

}  // namespace

SqlQueryResult sql_run_result_view(SqlDatabase& db, const std::string& sql, const std::string& session_db) {
  ResultViewStore* st = db.result_views_.get();
  if (!st) return view_error("result views are not available in this engine");
  size_t i = 0;
  while (i < sql.size() && std::isspace((unsigned char)sql[i])) ++i;
  i += 4;   // CALL
  while (i < sql.size() && std::isspace((unsigned char)sql[i])) ++i;
  i += 4;   // VIEW
  {
    size_t k = i;
    while (k < sql.size() && std::isspace((unsigned char)sql[k])) ++k;
    std::string w;
    while (k < sql.size() && std::isalpha((unsigned char)sql[k])) w.push_back((char)std::toupper((unsigned char)sql[k++]));
    if (w == "STATUS") return status_answer(*st);
  }
  std::string opts_text;
  if (!read_sql_literal(sql, &i, &opts_text))
    return view_error("CALL VIEW '<options JSON>' SELECT ... (the options go in a quoted string, '{}' for none)");
  while (i < sql.size() && std::isspace((unsigned char)sql[i])) ++i;
  std::string inner = sql.substr(i);
  while (!inner.empty() && (std::isspace((unsigned char)inner.back()) || inner.back() == ';')) inner.pop_back();
  const std::string verb = first_word_upper(inner);
  if (verb != "SELECT" && verb != "WITH") return view_error("CALL VIEW: the statement after the options must be a SELECT");

  JVal opts;
  opts.kind = JVal::Obj;
  if (!opts_text.empty()) {
    JParser p(opts_text);
    if (!p.value(&opts) || opts.kind != JVal::Obj) return view_error("CALL VIEW options: " + (p.err.empty() ? std::string("not a JSON object") : p.err));
  }
  const JVal* tv = opts.get("text");
  TextMode mode = TextMode::Utf8;
  if (tv && tv->kind == JVal::Str) {
    const std::string m = lower_ascii(tv->s);
    if (m == "latin1") mode = TextMode::Latin1;
    else if (m == "latin1_iso") mode = TextMode::Latin1Iso;
    else if (m != "utf8") return view_error("CALL VIEW options: text is utf8, latin1 or latin1_iso");
  }
  const JVal* gz = opts.get("gzip");
  const bool gzip = gz && gz->kind == JVal::Bool && gz->b;
  const JVal* ifv = opts.get("if_version");
  const JVal* draw = opts.get("draw");
  const JVal* win = opts.get("window");

  const std::string sdb = session_db.empty() ? db.effective_database() : session_db;
  const std::string key = sdb + "::" + inner;

  std::shared_ptr<ViewEntry> e;
  {
    hal::LockGuard lock(st->mu);
    auto it = st->by_key.find(key);
    if (it != st->by_key.end()) {
      st->lru.splice(st->lru.begin(), st->lru, it->second);
      e = *it->second;
      ++st->hits;
    }
  }
  if (e) {
    e->hits.fetch_add(1, std::memory_order_relaxed);
  } else {
    struct Building {
      ResultViewStore* st;
      explicit Building(ResultViewStore* s) : st(s) { st->building.fetch_add(1, std::memory_order_seq_cst); }
      ~Building() { st->building.fetch_sub(1, std::memory_order_seq_cst); }
    } building(st);
    uint64_t seq_before;
    {
      hal::LockGuard lock(st->mu);
      seq_before = st->seq;
    }
    const auto t0 = std::chrono::steady_clock::now();
    SqlQueryResult res = db.execute_gateway_query_unguarded(inner, session_db);
    if (!res.success) return res;
    if (!res.is_result_set) return view_error("CALL VIEW: the statement returned no rows to keep");
    res.formatted_table.clear();
    e = build_entry(key, inner, std::move(res));
    e->build_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    e->deps = statement_names(inner);
    {
      const auto views = db.view_names_lower();
      for (const auto& n : e->deps) if (views->count(n)) { e->all = true; break; }
    }
    hal::LockGuard lock(st->mu);
    ++st->builds;
    // A write that landed while this was being read would outlive its answer.
    bool stale = st->dropped_all_at > seq_before;
    for (const auto& d : e->deps) {
      auto w = st->dropped_at.find(d);
      if (w != st->dropped_at.end() && w->second > seq_before) { stale = true; break; }
    }
    if (!stale && e->bytes <= st->budget) {
      auto old = st->by_key.find(key);
      if (old != st->by_key.end()) st->erase_locked(old->second);
      while (!st->lru.empty() && st->bytes + e->bytes > st->budget) st->erase_locked(std::prev(st->lru.end()));
      st->lru.push_front(e);
      st->by_key[key] = st->lru.begin();
      st->any_stored.store(true, std::memory_order_seq_cst);
      for (const auto& d : e->deps) st->by_dep[d].insert(key);
      if (e->all) st->all_keys.insert(key);
      st->bytes += e->bytes;
    }
  }

  const size_t total = e->res->rows.size();
  if (ifv && ifv->kind == JVal::Str && ifv->s == e->version) return answer("not_modified", e->version, total, total, "identity", nullptr);
  if (win && win->kind == JVal::Obj) {
    std::string body = window_payload(*e, *win, draw, mode);
    const std::string head = body.substr(0, 400);
    size_t filtered = total;
    const size_t fp = head.find("\"recordsFiltered\":");
    if (fp != std::string::npos) filtered = (size_t)std::strtoull(head.c_str() + fp + 18, nullptr, 10);
    if (gzip) {
      const ByteBuffer z = net::gzip_compress_whole(ByteSpan(reinterpret_cast<const uint8_t*>(body.data()), body.size()));
      body.assign(reinterpret_cast<const char*>(z.data()), z.size());
    }
    return answer("ok", e->version, total, filtered, gzip ? "gzip" : "identity", &body);
  }
  if (draw && draw->kind == JVal::Num) {
    JVal all_rows;
    all_rows.kind = JVal::Obj;
    std::string body = window_payload(*e, all_rows, draw, mode);
    if (gzip) {
      const ByteBuffer z = net::gzip_compress_whole(ByteSpan(reinterpret_cast<const uint8_t*>(body.data()), body.size()));
      body.assign(reinterpret_cast<const char*>(z.data()), z.size());
    }
    return answer("ok", e->version, total, total, gzip ? "gzip" : "identity", &body);
  }
  const auto payload = full_payload(*e, mode, gzip);
  return answer("ok", e->version, total, total, gzip ? "gzip" : "identity", payload.get());
}

}  // namespace aios::userland
