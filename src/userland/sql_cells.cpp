// Cells: per-key counts, sums, extremes and bits that follow every row change
// of one table, and act when a bit flips. See sql_cells.hpp for the statements.
#include "aios/core/threads.hpp"
#include "aios/userland/sql_cells.hpp"
#include <map>
#include <mutex>
#include "aios/hal/sync.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "aios/channel/http_client.hpp"
#include "aios/hal/host.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/userland/mysql_wire.hpp"
#include "aios/userland/sql_database.hpp"

namespace aios::userland {
namespace {

std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
std::string upper(std::string s) { for (auto& c : s) c = (char)std::toupper((unsigned char)c); return s; }
std::string trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
  return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}
std::string unquote_ident(std::string s) {
  s = trim(s);
  if (s.size() >= 2 && s.front() == '`' && s.back() == '`') s = s.substr(1, s.size() - 2);
  return s;
}
bool is_ident(const std::string& s) {
  if (s.empty() || !(std::isalpha((unsigned char)s[0]) || s[0] == '_')) return false;
  for (char c : s) if (!(std::isalnum((unsigned char)c) || c == '_')) return false;
  return true;
}
std::string ident(const std::string& s) { return "`" + s + "`"; }
std::string qualified(const std::string& db, const std::string& table) { return ident(db) + "." + ident(table); }
std::string sql_lit(const std::string& v) {
  std::string o = "'";
  for (char c : v) { if (c == '\'' || c == '\\') o += '\\'; o += c; }
  return o + "'";
}
// A key in one spelling: an integer's sign and leading zeros never split it
// (the GROUP BY prints the value, a notice carries the cell).
std::string canon(const std::string& v) {
  std::string s = trim(v);
  size_t i = 0;
  bool neg = false;
  if (i < s.size() && (s[i] == '-' || s[i] == '+')) { neg = s[i] == '-'; ++i; }
  if (i == s.size()) return s;
  for (size_t j = i; j < s.size(); ++j) if (!std::isdigit((unsigned char)s[j])) return s;
  while (i + 1 < s.size() && s[i] == '0') ++i;
  std::string d = s.substr(i);
  return (neg && d != "0") ? "-" + d : d;
}
bool all_digits(const std::string& s) {
  if (s.empty()) return false;
  size_t i = s[0] == '-' ? 1 : 0;
  if (i == s.size()) return false;
  for (; i < s.size(); ++i) if (!std::isdigit((unsigned char)s[i])) return false;
  return true;
}

// Top-level (outside quotes and parentheses) helpers over a statement's text.
struct Scan {
  const std::string& s;
  // depth/quote state at each index
  std::vector<int> depth;
  std::vector<char> quoted;
  explicit Scan(const std::string& text) : s(text), depth(text.size(), 0), quoted(text.size(), 0) {
    int d = 0;
    char q = 0;
    for (size_t i = 0; i < s.size(); ++i) {
      const char c = s[i];
      if (q) {
        quoted[i] = 1;
        if (c == '\\' && q != '`' && i + 1 < s.size()) { quoted[++i] = 1; continue; }
        if (c == q) q = 0;
        depth[i] = d;
        continue;
      }
      if (c == '\'' || c == '"' || c == '`') { q = c; quoted[i] = 1; depth[i] = d; continue; }
      if (c == '(') { depth[i] = d; ++d; continue; }
      if (c == ')') { --d; depth[i] = d; continue; }
      depth[i] = d;
    }
  }
  bool top(size_t i) const { return i < s.size() && depth[i] == 0 && !quoted[i]; }
  bool word_at(size_t i, const char* w) const {
    const size_t n = std::strlen(w);
    if (i + n > s.size() || !top(i)) return false;
    for (size_t k = 0; k < n; ++k) if (std::toupper((unsigned char)s[i + k]) != w[k]) return false;
    if (i > 0 && (std::isalnum((unsigned char)s[i - 1]) || s[i - 1] == '_')) return false;
    if (i + n < s.size() && (std::isalnum((unsigned char)s[i + n]) || s[i + n] == '_')) return false;
    return true;
  }
  size_t find_word(const char* w, size_t from = 0) const {
    for (size_t i = from; i < s.size(); ++i) if (word_at(i, w)) return i;
    return std::string::npos;
  }
};

// Splits on a character that is outside quotes and at parenthesis depth `at`.
std::vector<std::string> split_top(const std::string& s, char sep, int at = 0) {
  std::vector<std::string> out;
  Scan sc(s);
  std::string cur;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == sep && !sc.quoted[i] && sc.depth[i] == at) { out.push_back(trim(cur)); cur.clear(); continue; }
    cur += s[i];
  }
  if (!trim(cur).empty()) out.push_back(trim(cur));
  return out;
}

// The leading word(s) of a statement, upper case, for handles().
std::string head_words(const std::string& sql, int n) {
  std::string out;
  size_t i = 0;
  for (int w = 0; w < n; ++w) {
    while (i < sql.size() && std::isspace((unsigned char)sql[i])) ++i;
    size_t b = i;
    while (i < sql.size() && (std::isalnum((unsigned char)sql[i]) || sql[i] == '_')) ++i;
    if (b == i) break;
    if (!out.empty()) out += ' ';
    out += upper(sql.substr(b, i - b));
  }
  return out;
}

bool numeric_text(const std::string& s, long double* out) {
  if (s.empty()) return false;
  char* end = nullptr;
  *out = std::strtold(s.c_str(), &end);
  return end && *end == '\0';
}
// MIN/MAX order: numbers as numbers, anything else as the collation does it
// - case and trailing spaces do not matter.
int compare_values(const std::string& a, const std::string& b) {
  long double x, y;
  if (numeric_text(a, &x) && numeric_text(b, &y)) return x < y ? -1 : (x > y ? 1 : 0);
  std::string p = lower(a), q = lower(b);
  while (!p.empty() && p.back() == ' ') p.pop_back();
  while (!q.empty() && q.back() == ' ') q.pop_back();
  return p < q ? -1 : (p > q ? 1 : 0);
}

SqlQueryResult error_result(const std::string& msg) {
  SqlQueryResult r;
  r.success = false;
  r.error_message = msg;
  return r;
}
SqlQueryResult ok_result(const std::string& msg) {
  SqlQueryResult r;
  r.success = true;
  r.formatted_table = msg;
  return r;
}

constexpr size_t kActLog = 64;
constexpr size_t kMaxBits = 64;

}  // namespace

// ---------------------------------------------------------------------------
// Exact decimals for SUM: a row's printed value, added and taken away again
// without drift (a double would not give the old total back after a delete).
namespace {
bool parse_dec(const std::string& t, AIOS_INT128* v, int* scale) {
  std::string s = trim(t);
  if (s.empty()) return false;
  if (s.find_first_of("eE") != std::string::npos) {
    long double d;
    if (!numeric_text(s, &d)) return false;
    *scale = 8;
    long double m = d * 100000000.0L;
    *v = aios_i128_from_ld(m < 0 ? m - 0.5L : m + 0.5L);
    return true;
  }
  size_t i = 0;
  bool neg = false;
  if (s[i] == '-' || s[i] == '+') { neg = s[i] == '-'; ++i; }
  AIOS_INT128 acc = 0;
  int sc = 0;
  bool dot = false, any = false;
  for (; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '.' && !dot) { dot = true; continue; }
    if (!std::isdigit((unsigned char)c)) return false;
    acc = acc * 10 + (c - '0');
    any = true;
    if (dot) ++sc;
  }
  if (!any) return false;
  *v = neg ? -acc : acc;
  *scale = sc;
  return true;
}
AIOS_INT128 pow10i(int n) { AIOS_INT128 p = 1; while (n-- > 0) p *= 10; return p; }
std::string dec_text(AIOS_INT128 v, int scale) {
  const bool neg = v < 0;
  AIOS_UINT128 u = neg ? (AIOS_UINT128)(-v) : (AIOS_UINT128)v;
  std::string digits;
  do { digits.insert(digits.begin(), char('0' + (int)(u % 10))); u /= 10; } while (u);
  while ((int)digits.size() <= scale) digits.insert(digits.begin(), '0');
  std::string out = scale > 0 ? digits.substr(0, digits.size() - scale) + "." + digits.substr(digits.size() - scale) : digits;
  return (neg && v != 0) ? "-" + out : out;
}
}  // namespace

CellEngine::CellEngine(SqlDatabase& db) : db_(db) {
#if !defined(AIOS_INLINE_THREADS)
  worker_ = aios::hal::Thread([this] { worker_loop(); });
#endif
}

CellEngine::~CellEngine() {
  {
    std::lock_guard<aios::hal::Mutex> lk(jobs_mu_);
    stop_ = true;
  }
  jobs_cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

std::string CellEngine::cells_dir() const { return db_.root_path_ + "/cells"; }

bool CellEngine::watches(const SqlTable& t) const {
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  return !watched_.empty() && watched_.count(lower(t.database + "." + t.name)) != 0;
}

bool CellEngine::has_pending() const {
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  for (const auto& kv : sets_) {
    const Set& s = *kv.second;
    if (s.reset || !s.dirty.empty() || !s.minmax_dirty.empty()) return true;
  }
  return false;
}

bool CellEngine::handles(const std::string& sql) const {
  const std::string h = head_words(sql, 2);
  return h == "CREATE CELLS" || h == "DROP CELLS" || h == "SHOW CELLS";
}

// ---------------------------------------------------------------------------
// CREATE CELLS name ON table KEY col ( cell, ... ) [ACT EXISTING] [ON RISE|FALL bit DO stmts TO profile] ...
bool CellEngine::parse_create(const std::string& sql, Set* s, std::string* err) {
  std::string q = trim(sql);
  while (!q.empty() && q.back() == ';') q = trim(q.substr(0, q.size() - 1));
  s->declaration = q;
  Scan sc(q);
  auto next_token = [&](size_t& i) {
    while (i < q.size() && std::isspace((unsigned char)q[i])) ++i;
    size_t b = i;
    while (i < q.size() && !std::isspace((unsigned char)q[i]) && q[i] != '(') ++i;
    return q.substr(b, i - b);
  };
  size_t i = 0;
  if (upper(next_token(i)) != "CREATE" || upper(next_token(i)) != "CELLS") { *err = "expected CREATE CELLS"; return false; }
  s->name = unquote_ident(next_token(i));
  if (!is_ident(s->name)) { *err = "CREATE CELLS needs a name (letters, digits, _)"; return false; }
  if (upper(next_token(i)) != "ON") { *err = "CREATE CELLS " + s->name + " ON <table> KEY <column> ( cells )"; return false; }
  std::string table = next_token(i);
  const size_t dot = table.find('.');
  if (dot != std::string::npos) { s->db = unquote_ident(table.substr(0, dot)); table = table.substr(dot + 1); }
  s->table = unquote_ident(table);
  if (upper(next_token(i)) != "KEY") { *err = "CREATE CELLS " + s->name + ": KEY <column> after the table"; return false; }
  s->key_col = unquote_ident(next_token(i));
  if (!is_ident(s->key_col)) { *err = "KEY needs a column name"; return false; }
  while (i < q.size() && std::isspace((unsigned char)q[i])) ++i;
  if (i >= q.size() || q[i] != '(') { *err = "CREATE CELLS " + s->name + ": the cells go in ( ... ) after KEY " + s->key_col; return false; }
  // the matching ')'
  size_t close = std::string::npos;
  for (size_t j = i + 1; j < q.size(); ++j) if (q[j] == ')' && !sc.quoted[j] && sc.depth[j] == 0) { close = j; break; }
  if (close == std::string::npos) { *err = "the cell list is not closed"; return false; }
  const std::string body = q.substr(i + 1, close - i - 1);
  for (const std::string& def : split_top(body, ',')) {
    Cell c;
    size_t k = 0;
    while (k < def.size() && !std::isspace((unsigned char)def[k])) ++k;
    c.name = unquote_ident(def.substr(0, k));
    if (!is_ident(c.name)) { *err = "a cell needs a name: '" + def + "'"; return false; }
    std::string rest = trim(def.substr(k));
    const std::string ur = upper(rest);
    auto take_when = [&](const std::string& tail) -> bool {
      std::string t = trim(tail);
      if (t.empty()) return true;
      if (upper(t.substr(0, 4)) != "WHEN" || (t.size() > 4 && !std::isspace((unsigned char)t[4]))) {
        *err = "cell " + c.name + ": expected WHEN <condition>, not '" + t + "'";
        return false;
      }
      c.when_text = trim(t.substr(4));
      return !c.when_text.empty();
    };
    if (ur == "COUNT" || ur.rfind("COUNT ", 0) == 0) {
      c.kind = Cell::Count;
      if (!take_when(rest.substr(5))) return false;
    } else if (ur.rfind("BIT ", 0) == 0) {
      std::string t = trim(rest.substr(4));
      const std::string w = upper(t.substr(0, t.find_first_of(" \t")));
      if (w == "ALL") c.kind = Cell::BitAll;
      else if (w == "ANY") c.kind = Cell::BitAny;
      else if (w == "NONE") c.kind = Cell::BitNone;
      else { *err = "cell " + c.name + ": BIT ALL|ANY|NONE <condition>"; return false; }
      c.when_text = trim(t.substr(w.size()));
      if (c.when_text.empty()) { *err = "cell " + c.name + ": the bit needs a condition"; return false; }
    } else if (ur.rfind("SUM", 0) == 0 || ur.rfind("MIN", 0) == 0 || ur.rfind("MAX", 0) == 0) {
      c.kind = ur[1] == 'U' ? Cell::Sum : (ur[1] == 'I' ? Cell::Min : Cell::Max);
      std::string t = trim(rest.substr(3));
      if (t.empty() || t[0] != '(') { *err = "cell " + c.name + ": " + ur.substr(0, 3) + "(<expression>)"; return false; }
      Scan ts(t);
      size_t e = std::string::npos;
      for (size_t j = 1; j < t.size(); ++j) if (t[j] == ')' && !ts.quoted[j] && ts.depth[j] == 0) { e = j; break; }
      if (e == std::string::npos) { *err = "cell " + c.name + ": unclosed ("; return false; }
      c.expr_text = trim(t.substr(1, e - 1));
      if (!take_when(t.substr(e + 1))) return false;
    } else {
      *err = "cell " + c.name + ": COUNT [WHEN c], SUM(e) [WHEN c], MIN(e), MAX(e) or BIT ALL|ANY|NONE c";
      return false;
    }
    for (const auto& o : s->cells) if (lower(o.name) == lower(c.name)) { *err = "two cells named " + c.name; return false; }
    s->cells.push_back(std::move(c));
  }
  if (s->cells.empty()) { *err = "CREATE CELLS " + s->name + ": no cells"; return false; }
  for (size_t ci = 0; ci < s->cells.size(); ++ci) if (s->cells[ci].is_bit()) s->bit_cells.push_back(ci);
  if (s->bit_cells.size() > kMaxBits) { *err = "at most 64 bits in a set"; return false; }

  // After the list: ACT EXISTING, ON RISE|FALL <bit> DO <stmts> TO <profile> ...
  const std::string tail = q.substr(close + 1);
  Scan ts(tail);
  std::vector<size_t> starts;
  for (size_t j = 0; j < tail.size(); ++j)
    if (ts.word_at(j, "ON")) {
      size_t k = j + 2;
      while (k < tail.size() && std::isspace((unsigned char)tail[k])) ++k;
      if (ts.word_at(k, "RISE") || ts.word_at(k, "FALL")) starts.push_back(j);
    }
  const size_t ex = ts.find_word("ACT");
  if (ex != std::string::npos && (starts.empty() || ex < starts[0])) {
    size_t k = ex + 3;
    while (k < tail.size() && std::isspace((unsigned char)tail[k])) ++k;
    if (!ts.word_at(k, "EXISTING")) { *err = "ACT EXISTING, or ON RISE|FALL <bit> DO ..."; return false; }
    s->act_existing = true;
  }
  const std::string before_acts = trim(tail.substr(0, starts.empty() ? tail.size() : starts[0]));
  if (!before_acts.empty() && upper(before_acts) != "ACT EXISTING") { *err = "unexpected '" + before_acts + "' after the cells"; return false; }
  for (size_t a = 0; a < starts.size(); ++a) {
    const std::string clause = trim(tail.substr(starts[a], (a + 1 < starts.size() ? starts[a + 1] : tail.size()) - starts[a]));
    Scan cs(clause);
    Act act;
    size_t k = 2;
    while (k < clause.size() && std::isspace((unsigned char)clause[k])) ++k;
    act.rise = cs.word_at(k, "RISE");
    k += 4;
    while (k < clause.size() && std::isspace((unsigned char)clause[k])) ++k;
    size_t b = k;
    while (k < clause.size() && (std::isalnum((unsigned char)clause[k]) || clause[k] == '_')) ++k;
    const std::string bit = clause.substr(b, k - b);
    bool found = false;
    for (size_t ci = 0; ci < s->cells.size(); ++ci)
      if (lower(s->cells[ci].name) == lower(bit)) {
        if (!s->cells[ci].is_bit()) { *err = "ON " + std::string(act.rise ? "RISE " : "FALL ") + bit + ": not a bit"; return false; }
        act.bit = ci;
        found = true;
      }
    if (!found) { *err = "ON RISE|FALL " + bit + ": no such bit"; return false; }
    const size_t nt = cs.find_word("NOTIFY", k);
    if (nt != std::string::npos) {
      std::string url = trim(clause.substr(nt + 6));
      if (url.size() < 2 || (url.front() != '\'' && url.front() != '"') || url.back() != url.front()) {
        *err = "ON ... " + bit + " NOTIFY '<http url>'";
        return false;
      }
      act.notify = url.substr(1, url.size() - 2);
      if (act.notify.rfind("http://", 0) != 0 && act.notify.rfind("https://", 0) != 0 && act.notify.rfind("local:", 0) != 0) {
        *err = "NOTIFY takes an http:// or https:// url, or local:<handler>";
        return false;
      }
      s->acts.push_back(act);
      continue;
    }
    const size_t d = cs.find_word("DO", k);
    if (d == std::string::npos) { *err = "ON ... " + bit + " DO <statement> TO <profile>, or NOTIFY '<url>'"; return false; }
    // The last top-level TO <profile> ends the clause.
    size_t to = std::string::npos;
    for (size_t j = d + 2; j < clause.size(); ++j) if (cs.word_at(j, "TO")) to = j;
    if (to == std::string::npos) { *err = "ON ... DO ... needs TO <profile>"; return false; }
    act.sql = trim(clause.substr(d + 2, to - d - 2));
    act.target = trim(clause.substr(to + 2));
    if (act.sql.empty() || !is_ident(act.target)) { *err = "ON ... DO <statement> TO <profile>"; return false; }
    s->acts.push_back(act);
  }
  return true;
}

bool CellEngine::compile(Set& s, std::string* err) {
  if (s.db.empty()) s.db = db_.effective_database();
  const SqlTable* t = db_.find_table(s.table, s.db);
  if (!t) { *err = "CREATE CELLS " + s.name + ": no table " + s.db + "." + s.table; return false; }
  s.table = t->name;
  s.db = t->database;
  s.table_key = lower(t->database + "." + t->name);
  s.key_index = t->get_column_index(s.key_col);
  if (s.key_index < 0) { *err = "CREATE CELLS " + s.name + ": " + s.table + " has no column " + s.key_col; return false; }
  int sums = 0, exts = 0;
  s.slot.assign(s.cells.size(), -1);
  for (size_t ci = 0; ci < s.cells.size(); ++ci) {
    Cell& c = s.cells[ci];
    if (!c.expr_text.empty()) {
      c.expr = SqlRowExpr::compile(db_, *t, c.expr_text, err);
      if (!c.expr) { *err = "cell " + c.name + ": " + *err; return false; }
    }
    if (!c.when_text.empty()) {
      c.when = SqlRowExpr::compile(db_, *t, c.when_text, err);
      if (!c.when) { *err = "cell " + c.name + ": " + *err; return false; }
    }
    if (c.kind == Cell::Sum) s.slot[ci] = sums++;
    if (c.kind == Cell::Min || c.kind == Cell::Max) s.slot[ci] = exts++;
  }
  return true;
}

// Every key's values from the table as it stands: one GROUP BY, the same
// conditions the row path evaluates, so the two agree from the start.
bool CellEngine::rebuild(Set& s, std::string* err) {
  std::string cols = ident(s.key_col) + ", COUNT(*)";
  for (const Cell& c : s.cells) {
    const std::string w = c.when_text.empty() ? std::string() : "(" + c.when_text + ")";
    switch (c.kind) {
      case Cell::Count:
        cols += w.empty() ? ", COUNT(*)" : ", SUM(CASE WHEN " + w + " THEN 1 ELSE 0 END)";
        break;
      case Cell::BitAll: case Cell::BitAny: case Cell::BitNone:
        cols += ", SUM(CASE WHEN " + w + " THEN 1 ELSE 0 END)";
        break;
      case Cell::Sum: case Cell::Min: case Cell::Max: {
        const std::string e = w.empty() ? "(" + c.expr_text + ")" : "CASE WHEN " + w + " THEN (" + c.expr_text + ") END";
        const char* f = c.kind == Cell::Sum ? "SUM" : (c.kind == Cell::Min ? "MIN" : "MAX");
        cols += ", COUNT(" + e + "), " + std::string(f) + "(" + e + ")";
        break;
      }
    }
  }
  const std::string sql = "SELECT " + cols + " FROM " + qualified(s.db, s.table) + " GROUP BY " + ident(s.key_col);
  // A table that is gone (DROP TABLE) holds no keys: every bit is off.
  SqlQueryResult r;
  if (db_.find_table(s.table, s.db)) {
    r = db_.execute_local_query(sql, s.db);
    if (!r.success) { *err = "CREATE CELLS " + s.name + ": " + r.error_message; return false; }
  }
  std::unordered_map<std::string, KeyState> fresh;
  for (const SqlRow& row : r.rows) {
    if (row.values.empty() || sql_is_null(row.values[0])) continue;   // a NULL key is no key
    KeyState k = blank_state(s);
    k.rows = std::strtoll(row.values[1].c_str(), nullptr, 10);
    size_t col = 2;
    for (size_t ci = 0; ci < s.cells.size(); ++ci) {
      const Cell& c = s.cells[ci];
      auto v = [&](size_t i) -> const std::string& { return row.values[i]; };
      if (c.kind == Cell::Sum || c.kind == Cell::Min || c.kind == Cell::Max) {
        k.n[ci] = std::strtoll(v(col).c_str(), nullptr, 10);
        const std::string& agg = v(col + 1);
        if (c.kind == Cell::Sum) {
          Dec d;
          if (!sql_is_null(agg)) parse_dec(agg, &d.v, &d.scale);
          k.sum[(size_t)s.slot[ci]] = d;
        } else {
          k.ext[(size_t)s.slot[ci]] = sql_is_null(agg) ? std::string() : agg;
        }
        col += 2;
      } else {
        k.n[ci] = sql_is_null(v(col)) ? 0 : std::strtoll(v(col).c_str(), nullptr, 10);
        col += 1;
      }
    }
    k.bits = bits_of(s, k);
    fresh[canon(row.values[0])] = std::move(k);
  }
  // What was acted on survives a rebuild (load, a replaced table): the acts
  // see the difference between it and the bits as they are now.
  for (auto& kv : s.keys) {
    auto it = fresh.find(kv.first);
    if (it != fresh.end()) it->second.acted = kv.second.acted;
    else if (kv.second.acted) {
      KeyState gone = blank_state(s);
      gone.acted = kv.second.acted;
      fresh[kv.first] = std::move(gone);
    }
  }
  s.keys = std::move(fresh);
  return true;
}

// A key with nothing in it yet, every slot sized: a SUM or MIN/MAX cell
// indexes its slot on the first row that arrives.
CellEngine::KeyState CellEngine::blank_state(const Set& s) const {
  KeyState k;
  k.n.assign(s.cells.size(), 0);
  size_t sums = 0, exts = 0;
  for (const Cell& c : s.cells) if (c.kind == Cell::Sum) ++sums; else if (c.kind == Cell::Min || c.kind == Cell::Max) ++exts;
  k.sum.assign(sums, Dec{});
  k.ext.assign(exts, std::string());
  return k;
}

uint64_t CellEngine::bits_of(const Set& s, const KeyState& k) const {
  uint64_t b = 0;
  for (size_t i = 0; i < s.bit_cells.size(); ++i) {
    const size_t ci = s.bit_cells[i];
    const int64_t n = ci < k.n.size() ? k.n[ci] : 0;
    bool on = false;
    switch (s.cells[ci].kind) {
      case Cell::BitAll: on = k.rows > 0 && n == k.rows; break;
      case Cell::BitAny: on = n > 0; break;
      case Cell::BitNone: on = k.rows > 0 && n == 0; break;
      default: break;
    }
    if (on) b |= uint64_t(1) << i;
  }
  return b;
}

// One row in (+1) or out (-1) of its key.
void CellEngine::apply(Set& s, const std::string& key, KeyState& k, const SqlRow& row, int sign) {
  if (k.n.size() != s.cells.size()) {
    const uint64_t acted = k.acted;
    k = blank_state(s);
    k.acted = acted;
  }
  k.rows += sign;
  std::string err;
  for (size_t ci = 0; ci < s.cells.size(); ++ci) {
    Cell& c = s.cells[ci];
    if (c.when && !c.when->holds(row, &err)) { err.clear(); continue; }
    if (c.kind == Cell::Count || c.is_bit()) { k.n[ci] += sign; continue; }
    SqlRowExpr::Value v;
    if (!c.expr->eval(row, &v, &err)) { err.clear(); continue; }
    if (v.null) continue;
    k.n[ci] += sign;
    if (c.kind == Cell::Sum) {
      Dec d;
      if (!parse_dec(v.text, &d.v, &d.scale)) continue;
      Dec& tot = k.sum[(size_t)s.slot[ci]];
      const int sc = std::max(tot.scale, d.scale);
      tot.v = tot.v * pow10i(sc - tot.scale) + (sign > 0 ? 1 : -1) * d.v * pow10i(sc - d.scale);
      tot.scale = sc;
    } else {
      std::string& ext = k.ext[(size_t)s.slot[ci]];
      const bool lo = c.kind == Cell::Min;
      if (sign > 0) {
        if (ext.empty() || (lo ? compare_values(v.text, ext) < 0 : compare_values(v.text, ext) > 0)) ext = v.text;
      } else if (!ext.empty() && compare_values(v.text, ext) == 0) {
        s.minmax_dirty.insert(key);   // its extreme left: settle reads the key again
      }
    }
  }
}

void CellEngine::on_row_change(const SqlTable& t, const SqlRow* before, const SqlRow* after) {
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  const std::string tk = lower(t.database + "." + t.name);
  for (auto& kv : sets_) {
    Set& s = *kv.second;
    if (s.table_key != tk || s.reset) continue;
    for (int pass = 0; pass < 2; ++pass) {
      const SqlRow* r = pass == 0 ? before : after;
      if (!r || s.key_index < 0 || (size_t)s.key_index >= r->values.size() || sql_is_null(r->values[(size_t)s.key_index])) continue;
      const std::string key = canon(r->values[(size_t)s.key_index]);
      apply(s, key, s.keys[key], *r, pass == 0 ? -1 : +1);
      s.dirty.insert(key);
    }
    ++s.changes;
  }
}

void CellEngine::on_table_reset(const SqlTable& t) {
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  const std::string tk = lower(t.database + "." + t.name);
  for (auto& kv : sets_) if (kv.second->table_key == tk) kv.second->reset = true;
}

// A MIN or MAX whose extreme left: read that key's again (only that key).
bool CellEngine::recompute_extremes(Set& s, const std::string& key, std::string* err) {
  auto it = s.keys.find(key);
  if (it == s.keys.end()) return true;
  std::string cols;
  std::vector<size_t> which;
  for (size_t ci = 0; ci < s.cells.size(); ++ci) {
    const Cell& c = s.cells[ci];
    if (c.kind != Cell::Min && c.kind != Cell::Max) continue;
    const std::string e = c.when_text.empty() ? "(" + c.expr_text + ")" : "CASE WHEN (" + c.when_text + ") THEN (" + c.expr_text + ") END";
    cols += std::string(cols.empty() ? "" : ", ") + (c.kind == Cell::Min ? "MIN(" : "MAX(") + e + ")";
    which.push_back(ci);
  }
  if (which.empty()) return true;
  const std::string lit = all_digits(key) ? key : sql_lit(key);
  SqlQueryResult r = db_.execute_local_query("SELECT " + cols + " FROM " + qualified(s.db, s.table) + " WHERE " + ident(s.key_col) + " = " + lit, s.db);
  if (!r.success) { *err = r.error_message; return false; }
  for (size_t w = 0; w < which.size(); ++w) {
    const std::string v = r.rows.empty() || w >= r.rows[0].values.size() ? sql_null() : r.rows[0].values[w];
    it->second.ext[(size_t)s.slot[which[w]]] = sql_is_null(v) ? std::string() : v;
  }
  return true;
}

// The bits as they stand against what was last acted on: a RISE act for a
// bit that came on, a FALL act for one that went off.
void CellEngine::queue_job(Set& s, const std::string& key, size_t i, bool rise, uint64_t id) {
  for (const Act& a : s.acts) {
    if (a.bit != s.bit_cells[i] || a.rise != rise) continue;
    Job j;
    j.set = s.name;
    j.key = key;
    j.bit = i;
    j.rise = rise;
    j.id = id;
    j.what = std::string(rise ? "RISE " : "FALL ") + s.cells[a.bit].name;
    j.sql = a.sql;
    j.target = a.target;
    j.notify = a.notify;
    {
      std::lock_guard<aios::hal::Mutex> jl(jobs_mu_);
      jobs_.push_back(std::move(j));
    }
    // Woken on every job, not only from settle: one queued at load (an act a
    // kill left uncommitted) would otherwise wait for some unrelated edge.
    jobs_cv_.notify_one();
  }
}

void CellEngine::queue_edges(Set& s, const std::string& key, KeyState& k) {
  const uint64_t was = k.acted, now = k.bits;
  if (was == now) return;
  for (size_t i = 0; i < s.bit_cells.size(); ++i) {
    const uint64_t m = uint64_t(1) << i;
    if ((was & m) == (now & m)) continue;
    const bool rise = (now & m) != 0;
    bool has_act = false;
    for (const Act& a : s.acts) if (a.bit == s.bit_cells[i] && a.rise == rise) has_act = true;
    if (!has_act) continue;
    const uint64_t id = next_job_id_++;
    s.inflight[id] = {key, {i, rise}};
    queue_job(s, key, i, rise, id);
  }
  k.acted = now;
  dirty_ = true;
}

void CellEngine::settle() {
  bool queued = false;
  {
    std::lock_guard<aios::hal::Mutex> lk(mu_);
    for (auto& kv : sets_) {
      Set& s = *kv.second;
      std::string err;
      if (s.reset) {
        s.reset = false;
        s.minmax_dirty.clear();
        // Dropped and created again, the table may have other columns: the
        // expressions are compiled against it as it is now.
        if (db_.find_table(s.table, s.db) && !compile(s, &err))
          std::fprintf(stderr, "[cells] %s: the table changed under it: %s\n", s.name.c_str(), err.c_str());
        if (!rebuild(s, &err)) std::fprintf(stderr, "[cells] %s: rebuild failed: %s\n", s.name.c_str(), err.c_str());
        for (auto& k : s.keys) s.dirty.insert(k.first);
      }
      for (const std::string& key : s.minmax_dirty)
        if (!recompute_extremes(s, key, &err)) std::fprintf(stderr, "[cells] %s %s: %s\n", s.name.c_str(), key.c_str(), err.c_str());
      s.minmax_dirty.clear();
      if (s.dirty.empty()) continue;
      size_t jobs_before;
      { std::lock_guard<aios::hal::Mutex> jl(jobs_mu_); jobs_before = jobs_.size(); }
      bool acted_moved = false;
      for (const std::string& key : s.dirty) {
        auto it = s.keys.find(key);
        if (it == s.keys.end()) continue;
        KeyState& k = it->second;
        k.bits = bits_of(s, k);
        const uint64_t was = k.acted;
        queue_edges(s, key, k);
        if (k.acted != was) acted_moved = true;
        if (k.rows <= 0 && k.acted == 0) s.keys.erase(it);
      }
      s.dirty.clear();
      { std::lock_guard<aios::hal::Mutex> jl(jobs_mu_); queued = queued || jobs_.size() > jobs_before; }
      if (acted_moved) write_set(s);   // what was acted on is on disk before the act runs
    }
  }
#if defined(AIOS_INLINE_THREADS)
  // No threads that wait here (metal, wasm): the acts run here.
  for (;;) {
    Job j;
    {
      std::lock_guard<aios::hal::Mutex> jl(jobs_mu_);
      if (jobs_.empty()) break;
      j = std::move(jobs_.front());
      jobs_.pop_front();
    }
    std::string err;
    run_job(j, &err);
  }
#else
  if (queued) jobs_cv_.notify_one();
#endif
}

// One act: its statements, KEY filled in, in one transaction on the profile.
// The connection is kept for the next one (an act is a write, not a login).
bool CellEngine::run_job(const Job& j, std::string* err) {
  static thread_local std::map<std::string, std::unique_ptr<MariaWireSession>> sessions;
  DbConnectionProfile prof;
  bool found = false;
  if (j.notify.empty())
    for (const auto& p : db_.list_profiles()) if (p.id == j.target) { prof = p; found = true; }
  bool ok = false;
  if (!j.notify.empty()) {
    ok = post_edge(j, err);
  } else if (!found) *err = "TO " + j.target + ": no such connection profile";
  else if (!prof.things_write) *err = "TO " + j.target + ": the profile is not marked things_write=1";
  else {
    // KEY as a word outside quotes is the key, except SQL's own KEY.
    const std::string key = all_digits(j.key) ? j.key : sql_lit(j.key);
    std::vector<std::string> stmts;
    {
      const std::string& q = j.sql;
      std::string cur;
      char quote = 0;
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
        if ((c == 'K' || c == 'k') && i + 3 <= q.size() && std::toupper((unsigned char)q[i + 1]) == 'E' &&
            std::toupper((unsigned char)q[i + 2]) == 'Y' && (i == 0 || !(std::isalnum((unsigned char)q[i - 1]) || q[i - 1] == '_' || q[i - 1] == '.')) &&
            (i + 3 == q.size() || !(std::isalnum((unsigned char)q[i + 3]) || q[i + 3] == '_'))) {
          const std::string pw = prev_word(i);
          if (pw == "DUPLICATE" || pw == "PRIMARY" || pw == "UNIQUE" || pw == "FOREIGN" || pw == "ADD" || pw == "DROP" || pw == "INDEX") { cur += "KEY"; i += 2; continue; }
          cur += key;
          i += 2;
          continue;
        }
        cur += c;
      }
      if (!trim(cur).empty()) stmts.push_back(trim(cur));
    }
    for (int attempt = 0; attempt < 2 && !ok; ++attempt) {
      auto& sess = sessions[j.target];
      if (!sess) {
        if (auto t = sql_profile_transport_ok(prof); !t.ok()) { *err = "TO " + j.target + ": " + t.error().message; break; }
        sess = std::make_unique<MariaWireSession>(sql_profile_dial_host(prof), prof.port);
        auto c = sess->connect(prof.user, prof.password, prof.database);
        if (!c.ok()) { *err = "TO " + j.target + ": cannot connect: " + c.error().message; sess.reset(); continue; }
      }
      bool dropped = false;
      auto one = [&](const std::string& stmt) -> bool {
        auto r = sess->query(stmt);
        if (!r.ok()) { *err = stmt + ": " + r.error().message; dropped = true; return false; }
        if (!r->ok) { *err = stmt + ": " + r->error_message; return false; }
        return true;
      };
      bool good = one("START TRANSACTION");
      for (size_t s = 0; good && s < stmts.size(); ++s) good = one(stmts[s]);
      if (good) good = one("COMMIT");
      else if (!dropped) { auto r = sess->query("ROLLBACK"); (void)r; *err += " (rolled back)"; }
      if (good) ok = true;
      else if (dropped) sess.reset();   // a dead connection: once more on a fresh one
      else break;                       // the statement itself failed: not a connection matter
    }
  }
  // The outcome, in the set's log; a failed act leaves the bit un-acted, so
  // the next change of that key tries again.
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  auto it = sets_.find(lower(j.set));
  if (it != sets_.end()) {
    Set& s = *it->second;
    ActLog l;
    l.wall_ns = db_.kernel_.host().wall_ns();
    l.key = j.key;
    l.what = j.what;
    l.result = ok ? "ok" : *err;
    s.log.push_back(std::move(l));
    while (s.log.size() > kActLog) s.log.pop_front();
    s.inflight.erase(j.id);
    if (ok) { ++s.acts_run; write_set(s); }
    else {
      ++s.acts_failed;
      auto k = s.keys.find(j.key);
      if (k != s.keys.end()) {
        const uint64_t m = uint64_t(1) << j.bit;
        k->second.acted = j.rise ? (k->second.acted & ~m) : (k->second.acted | m);
        write_set(s);
      }
      std::fprintf(stderr, "[cells] %s %s %s: %s\n", s.name.c_str(), j.key.c_str(), j.what.c_str(), err->c_str());
    }
  }
  return ok;
}

// NOTIFY: the edge and the key's values as they are now, POSTed as JSON.
namespace {
std::mutex& local_mu() { static std::mutex m; return m; }
std::map<std::string, CellEngine::LocalHandler>& local_handlers() { static std::map<std::string, CellEngine::LocalHandler> h; return h; }
}  // namespace

void CellEngine::register_local(const std::string& name, LocalHandler handler) {
  std::lock_guard<std::mutex> lk(local_mu());
  local_handlers()[name] = std::move(handler);
}

void CellEngine::unregister_local(const std::string& name) {
  std::lock_guard<std::mutex> lk(local_mu());
  local_handlers().erase(name);
}

bool CellEngine::post_edge(const Job& j, std::string* err) {
  auto js = [](const std::string& v) {
    std::string o = "\"";
    for (unsigned char c : v) {
      if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
      else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
      else o += (char)c;
    }
    return o + "\"";
  };
  std::string body;
  {
    std::lock_guard<aios::hal::Mutex> lk(mu_);
    auto it = sets_.find(lower(j.set));
    std::string values;
    if (it != sets_.end()) {
      const Set& s = *it->second;
      auto k = s.keys.find(j.key);
      if (k != s.keys.end()) {
        size_t bit_i = 0;
        for (size_t ci = 0; ci < s.cells.size(); ++ci) {
          const Cell& c = s.cells[ci];
          std::string v;
          if (c.is_bit()) v = (k->second.bits >> bit_i++) & 1 ? "1" : "0";
          else if (c.kind == Cell::Count) v = std::to_string(k->second.n[ci]);
          else if (c.kind == Cell::Sum) v = dec_text(k->second.sum[(size_t)s.slot[ci]].v, k->second.sum[(size_t)s.slot[ci]].scale);
          else v = k->second.ext[(size_t)s.slot[ci]];
          values += (values.empty() ? "" : ",") + js(c.name) + ":" + js(v);
        }
      }
    }
    const size_t sp = j.what.find(' ');
    body = "{\"cells\":" + js(j.set) + ",\"key\":" + js(j.key) + ",\"bit\":" + js(sp == std::string::npos ? j.what : j.what.substr(sp + 1)) +
           ",\"edge\":" + js(j.rise ? "rise" : "fall") + ",\"id\":" + std::to_string(j.id) + ",\"values\":{" + values + "}}";
  }
  if (j.notify.rfind("local:", 0) == 0) {
    LocalHandler h;
    {
      std::lock_guard<std::mutex> lk(local_mu());
      auto it = local_handlers().find(j.notify.substr(6));
      if (it != local_handlers().end()) h = it->second;
    }
    if (!h) { *err = "NOTIFY " + j.notify + ": no handler registered in this process"; return false; }
    std::string why;
    if (!h(body, &why)) { *err = "NOTIFY " + j.notify + ": " + (why.empty() ? "refused" : why); return false; }
    return true;
  }
  auto ep = channel::parse_endpoint(j.notify);
  if (!ep.ok()) { *err = "NOTIFY " + j.notify + ": " + ep.error().message; return false; }
  channel::HttpRequest req;
  req.method = "POST";
  req.endpoint = *ep;
  req.headers = {{"Content-Type", "application/json"}};
  req.body.assign(body.begin(), body.end());
  req.timeout_ms = 5000;
  if (ep->secure) { req.host = &db_.kernel_.host(); req.kernel_root = db_.root_path_; }
  auto r = channel::http_send(req);
  if (!r.ok()) { *err = "NOTIFY " + j.notify + ": " + r.error().message; return false; }
  if (r->status < 200 || r->status >= 300) { *err = "NOTIFY " + j.notify + ": HTTP " + std::to_string(r->status); return false; }
  return true;
}

void CellEngine::worker_loop() {
  for (;;) {
    Job j;
    {
      std::unique_lock<aios::hal::Mutex> lk(jobs_mu_);
      jobs_cv_.wait(lk, [&] { return stop_ || !jobs_.empty(); });
      if (stop_ && jobs_.empty()) return;
      j = std::move(jobs_.front());
      jobs_.pop_front();
    }
    std::string err;
    run_job(j, &err);
  }
}

size_t CellEngine::bytes_of(const Set& s) const {
  size_t b = 0;
  for (const auto& kv : s.keys) {
    b += kv.first.capacity() + sizeof(KeyState) + 32;   // the key, its state, the map node
    b += kv.second.n.capacity() * sizeof(int64_t) + kv.second.sum.capacity() * sizeof(Dec);
    for (const auto& e : kv.second.ext) b += sizeof(std::string) + e.capacity();
  }
  return b;
}

// ---------------------------------------------------------------------------
// <root>/cells/<name>.cells: the declaration, then what each key's acts saw.
// Values are not kept: they are rebuilt from the tables at load.
void CellEngine::write_set(const Set& s) {
  auto& host = db_.kernel_.host();
  host.make_dir(cells_dir());
  std::string text = "declaration\t";
  for (char c : s.declaration) text += c == '\n' ? ' ' : c;
  text += "\ndb\t" + s.db + "\n";
  text += "stats\t" + std::to_string(s.changes) + "\t" + std::to_string(s.acts_run) + "\t" + std::to_string(s.acts_failed) + "\t" + std::to_string(s.created_ns) + "\n";
  text += "edges\t" + std::to_string(next_job_id_) + "\n";
  for (const auto& f : s.inflight)
    text += "inflight\t" + std::to_string(f.first) + "\t" + f.second.first + "\t" + std::to_string(f.second.second.first) + "\t" + (f.second.second.second ? "rise" : "fall") + "\n";
  for (const auto& kv : s.keys) {
    if (!kv.second.acted) continue;
    char hex[24];
    std::snprintf(hex, sizeof hex, "%llx", (unsigned long long)kv.second.acted);
    text += "acted\t" + kv.first + "\t" + hex + "\n";
  }
  host.write_file(cells_dir() + "/" + lower(s.name) + ".cells", ByteSpan((const uint8_t*)text.data(), text.size()));
}

void CellEngine::save() {
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  if (!dirty_) return;
  for (const auto& kv : sets_) write_set(*kv.second);
  dirty_ = false;
}

void CellEngine::load() {
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  auto& host = db_.kernel_.host();
  if (!host.exists(cells_dir())) return;
  auto names = host.list_dir(cells_dir());
  if (!names.ok()) return;
  for (const std::string& n : *names) {
    if (n.size() < 7 || n.compare(n.size() - 6, 6, ".cells") != 0) continue;
    auto f = host.read_file(cells_dir() + "/" + n);
    if (!f.ok()) continue;
    const std::string text(f->begin(), f->end());
    std::string decl, dbname;
    std::vector<std::pair<std::string, uint64_t>> acted;
    struct Pend { uint64_t id; std::string key; size_t bit; bool rise; };
    std::vector<Pend> pend;
    uint64_t changes = 0, run = 0, failed = 0, created = 0;
    size_t at = 0;
    while (at < text.size()) {
      size_t nl = text.find('\n', at);
      const std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
      at = nl == std::string::npos ? text.size() : nl + 1;
      const size_t t1 = line.find('\t');
      if (t1 == std::string::npos) continue;
      const std::string tag = line.substr(0, t1), rest = line.substr(t1 + 1);
      if (tag == "declaration") decl = rest;
      else if (tag == "db") dbname = rest;
      else if (tag == "stats") std::sscanf(rest.c_str(), "%llu\t%llu\t%llu\t%llu", (unsigned long long*)&changes, (unsigned long long*)&run, (unsigned long long*)&failed, (unsigned long long*)&created);
      else if (tag == "edges") next_job_id_ = std::max(next_job_id_, (uint64_t)std::strtoull(rest.c_str(), nullptr, 10));
      else if (tag == "inflight") {   // id, key, bit, rise|fall
        std::vector<std::string> fields;
        size_t b = 0;
        for (size_t e; (e = rest.find('\t', b)) != std::string::npos; b = e + 1) fields.push_back(rest.substr(b, e - b));
        fields.push_back(rest.substr(b));
        if (fields.size() == 4)
          pend.push_back({std::strtoull(fields[0].c_str(), nullptr, 10), fields[1], (size_t)std::strtoull(fields[2].c_str(), nullptr, 10),
                          fields[3] == "rise"});
      }
      else if (tag == "acted") {
        const size_t t2 = rest.find('\t');
        if (t2 != std::string::npos) acted.push_back({rest.substr(0, t2), std::strtoull(rest.substr(t2 + 1).c_str(), nullptr, 16)});
      }
    }
    auto s = std::make_unique<Set>();
    std::string err;
    if (!parse_create(decl, s.get(), &err)) { std::fprintf(stderr, "[cells] %s: %s\n", n.c_str(), err.c_str()); continue; }
    if (s->db.empty()) s->db = dbname;
    if (!compile(*s, &err)) { std::fprintf(stderr, "[cells] %s: %s\n", n.c_str(), err.c_str()); continue; }
    for (const auto& a : acted) { KeyState k = blank_state(*s); k.acted = a.second; s->keys[a.first] = std::move(k); }
    if (!rebuild(*s, &err)) { std::fprintf(stderr, "[cells] %s: %s\n", n.c_str(), err.c_str()); continue; }
    s->changes = changes; s->acts_run = run; s->acts_failed = failed; s->created_ns = created;
    // An act queued but never committed (killed first) runs now; a bit that
    // moved while the machine was down acts now, on the data as it is.
    // The same edge id as before the kill: a NOTIFY receiver can tell a
    // repeat from a new edge.
    for (const Pend& p : pend) {
      if (p.bit >= s->bit_cells.size()) continue;
      s->inflight[p.id] = {p.key, {p.bit, p.rise}};
      next_job_id_ = std::max(next_job_id_, p.id + 1);
      queue_job(*s, p.key, p.bit, p.rise, p.id);
    }
    for (const auto& k : s->keys) s->dirty.insert(k.first);
    watched_.insert(s->table_key);
    sets_[lower(s->name)] = std::move(s);
  }
}

// ---------------------------------------------------------------------------
SqlQueryResult CellEngine::execute(const std::string& sql) {
  const std::string h = head_words(sql, 2);
  if (h == "SHOW CELLS") return show(sql);
  if (h == "DROP CELLS") {
    std::string name = lower(unquote_ident(trim(trim(sql).substr(10))));
    while (!name.empty() && (name.back() == ';' || std::isspace((unsigned char)name.back()))) name.pop_back();
    std::lock_guard<aios::hal::Mutex> lk(mu_);
    auto it = sets_.find(name);
    if (it == sets_.end()) return error_result("no cells named " + name);
    sets_.erase(it);
    watched_.clear();
    for (const auto& kv : sets_) watched_.insert(kv.second->table_key);
    db_.kernel_.host().remove(cells_dir() + "/" + name + ".cells");
    return ok_result("cells " + name + " dropped");
  }
  // CREATE CELLS
  auto s = std::make_unique<Set>();
  std::string err;
  if (!parse_create(sql, s.get(), &err)) return error_result(err);
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  if (sets_.count(lower(s->name))) return error_result("cells " + s->name + " already exist (DROP CELLS first)");
  if (!compile(*s, &err)) return error_result(err);
  for (const Act& a : s->acts) {
    if (!a.notify.empty()) continue;
    bool found = false, writable = false;
    for (const auto& p : db_.list_profiles()) if (p.id == a.target) { found = true; writable = p.things_write; }
    if (!found) return error_result("TO " + a.target + ": no such connection profile");
    if (!writable) return error_result("TO " + a.target + ": the profile is not marked things_write=1");
  }
  const uint64_t t0 = db_.kernel_.host().monotonic_ns();
  if (!rebuild(*s, &err)) return error_result(err);
  const uint64_t built_ms = (db_.kernel_.host().monotonic_ns() - t0) / 1000000;
  s->created_ns = db_.kernel_.host().wall_ns();
  size_t on = 0;
  for (auto& kv : s->keys) {
    if (s->act_existing) s->dirty.insert(kv.first);   // settle acts on what is already true
    else kv.second.acted = kv.second.bits;           // already true counts as acted on
    if (kv.second.bits) ++on;
  }
  watched_.insert(s->table_key);
  const std::string msg = "cells " + s->name + " on " + s->db + "." + s->table + ": " + std::to_string(s->keys.size()) + " keys, " +
                          std::to_string(on) + " with a bit on, built in " + std::to_string(built_ms) + " ms, " +
                          std::to_string(bytes_of(*s)) + " bytes";
  write_set(*s);
  sets_[lower(s->name)] = std::move(s);
  dirty_ = true;
  return ok_result(msg);
}

SqlQueryResult CellEngine::show(const std::string& sql) {
  std::lock_guard<aios::hal::Mutex> lk(mu_);
  std::string q = trim(sql);
  while (!q.empty() && q.back() == ';') q = trim(q.substr(0, q.size() - 1));
  const std::string rest = trim(q.substr(10));   // after SHOW CELLS
  SqlQueryResult r;
  r.success = true;
  r.is_result_set = true;
  if (rest.empty()) {
    r.columns = {"cells", "table", "key", "keys", "bits_on", "bytes", "changes", "acts", "act_failures", "declared"};
    for (const auto& kv : sets_) {
      const Set& s = *kv.second;
      std::string bits;
      for (size_t i = 0; i < s.bit_cells.size(); ++i) {
        size_t n = 0;
        for (const auto& k : s.keys) if (k.second.bits & (uint64_t(1) << i)) ++n;
        bits += (bits.empty() ? "" : " ") + s.cells[s.bit_cells[i]].name + "=" + std::to_string(n);
      }
      SqlRow row;
      size_t live = 0;
      for (const auto& k : s.keys) if (k.second.rows > 0) ++live;
      row.values = {s.name, s.db + "." + s.table, s.key_col, std::to_string(live), bits, std::to_string(bytes_of(s)),
                    std::to_string(s.changes), std::to_string(s.acts_run), std::to_string(s.acts_failed), s.declaration};
      r.rows.push_back(std::move(row));
    }
    return r;
  }
  size_t k = 0;
  while (k < rest.size() && !std::isspace((unsigned char)rest[k])) ++k;
  const std::string name = lower(unquote_ident(rest.substr(0, k)));
  auto it = sets_.find(name);
  if (it == sets_.end()) return error_result("no cells named " + name);
  const Set& s = *it->second;
  const std::string mode = trim(rest.substr(k));
  const std::string um = upper(mode);
  if (um == "ACTS") {
    r.columns = {"at", "key", "act", "result"};
    for (const ActLog& l : s.log) {
      char ts[32];
      const std::time_t secs = (std::time_t)(l.wall_ns / 1000000000ull);
      std::tm tm_buf{};
#if defined(_WIN32)
      localtime_s(&tm_buf, &secs);
#else
      localtime_r(&secs, &tm_buf);
#endif
      std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm_buf);
      SqlRow row;
      row.values = {ts, l.key, l.what, l.result};
      r.rows.push_back(std::move(row));
    }
    return r;
  }
  std::string only_key;
  int only_bit = -1;
  if (um.rfind("KEY", 0) == 0) {
    only_key = trim(mode.substr(3));
    if (only_key.size() >= 2 && (only_key.front() == '\'' || only_key.front() == '"')) only_key = only_key.substr(1, only_key.size() - 2);
    only_key = canon(only_key);
  } else if (um.rfind("WHERE", 0) == 0) {
    const std::string bit = lower(trim(mode.substr(5)));
    for (size_t i = 0; i < s.bit_cells.size(); ++i) if (lower(s.cells[s.bit_cells[i]].name) == bit) only_bit = (int)i;
    if (only_bit < 0) return error_result("SHOW CELLS " + s.name + " WHERE <bit>: no bit " + bit);
  } else if (!mode.empty()) {
    return error_result("SHOW CELLS " + s.name + " [WHERE <bit> | KEY '<key>' | ACTS]");
  }
  r.columns = {s.key_col, "rows"};
  for (const Cell& c : s.cells) r.columns.push_back(c.name);
  std::vector<std::string> keys;
  for (const auto& kv : s.keys) {
    if (kv.second.rows <= 0) continue;
    if (!only_key.empty() && kv.first != only_key) continue;
    if (only_bit >= 0 && !(kv.second.bits & (uint64_t(1) << only_bit))) continue;
    keys.push_back(kv.first);
  }
  std::sort(keys.begin(), keys.end(), [](const std::string& a, const std::string& b) {
    if (all_digits(a) && all_digits(b) && a.size() != b.size()) return a.size() < b.size() || (a[0] == '-' && b[0] != '-');
    return a < b;
  });
  for (const std::string& key : keys) {
    const KeyState& ks = s.keys.at(key);
    SqlRow row;
    row.values = {key, std::to_string(ks.rows)};
    size_t bit_i = 0;
    for (size_t ci = 0; ci < s.cells.size(); ++ci) {
      const Cell& c = s.cells[ci];
      if (c.is_bit()) { row.values.push_back((ks.bits >> bit_i) & 1 ? "1" : "0"); ++bit_i; continue; }
      if (c.kind == Cell::Count) { row.values.push_back(std::to_string(ks.n[ci])); continue; }
      if (c.kind == Cell::Sum) {
        const Dec& d = ks.sum[(size_t)s.slot[ci]];
        row.values.push_back(ks.n[ci] == 0 ? sql_null() : dec_text(d.v, d.scale));
        continue;
      }
      const std::string& e = ks.ext[(size_t)s.slot[ci]];
      row.values.push_back(ks.n[ci] == 0 || e.empty() ? sql_null() : e);
    }
    r.rows.push_back(std::move(row));
  }
  return r;
}

}  // namespace aios::userland
