// Checks exact numbers a double cannot carry - BIGINTs from 2^53 on, ids near
// 2^63, DECIMAL(18,2) values of 18 digits - through every path the engine has
// for them: WHERE comparisons, BETWEEN, IN lists and IN subqueries, MIN / MAX /
// SUM, ORDER BY and a join, on a small table and a big keyed one, before and
// after the columns are re-encoded as typed integers (SegmentFOR). As doubles
// 9007199254740993 is 9007199254740992 and neighbouring ids near 2^63 are one
// number; MariaDB compares them exactly, and so must we. The expected answers
// are worked out here with 128-bit integers.
#undef NDEBUG
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "aios/userland/portable_int128.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"

using namespace aios;
using namespace aios::userland;

namespace {

using i128 = AIOS_INT128;

int failures = 0;
int checks = 0;

#define CHECK(cond, ...)                 \
  do {                                   \
    ++checks;                            \
    if (!(cond)) {                       \
      ++failures;                        \
      std::printf("  [FAIL] ");          \
      std::printf(__VA_ARGS__);          \
      std::printf("\n");                 \
    }                                    \
  } while (0)

SqlQueryResult run(SqlDatabase& db, const std::string& sql) {
  SqlQueryResult r = db.execute_local_query(sql);
  if (!r.success) { std::printf("  query failed: %s\n  %s\n", sql.c_str(), r.error_message.c_str()); std::exit(1); }
  return r;
}

std::string q1(SqlDatabase& db, const std::string& sql) {
  SqlQueryResult r = run(db, sql);
  if (r.rows.empty() || r.rows[0].values.empty()) return "<none>";
  return r.rows[0].values[0];
}

std::string i128_text(i128 v) {
  if (v == 0) return "0";
  const bool neg = v < 0;
  std::string s;
  while (v != 0) { const int d = (int)(v % 10); s += (char)('0' + (neg ? -d : d)); v /= 10; }
  if (neg) s += '-';
  std::reverse(s.begin(), s.end());
  return s;
}

// A scaled integer as the column prints it (DECIMAL(18,2): two places).
std::string scaled_text(i128 v, int scale) {
  if (scale == 0) return i128_text(v);
  const bool neg = v < 0;
  i128 a = neg ? -v : v;
  i128 p = 1;
  for (int i = 0; i < scale; ++i) p *= 10;
  std::string frac = i128_text(a % p);
  while ((int)frac.size() < scale) frac = "0" + frac;
  return (neg ? "-" : "") + i128_text(a / p) + "." + frac;
}

const i128 MAXV = (i128)9223372036854775807LL;   // BIGINT's largest

i128 pow10(int n) { i128 p = 1; for (int i = 0; i < n; ++i) p *= 10; return p; }

// A constant as the statement spells it, and its exact value num / 10^scale;
// approx: a DOUBLE literal (1e0 notation). MariaDB 10.11 compares even that
// with a BIGINT exactly: `b = 9.007199254740993e15` finds 9007199254740992
// (the double's own value) and not ...993.
struct Const {
  std::string sql;
  i128 num = 0;
  int scale = 0;
  bool approx = false;
  double dbl = 0;
};

Const exact(i128 num, int scale, bool quoted) {
  Const c;
  c.num = num;
  c.scale = scale;
  const std::string t = scaled_text(num, scale);
  c.sql = quoted ? "'" + t + "'" : t;
  return c;
}

Const approx(double d) {
  Const c;
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.17e", d);
  c.sql = buf;
  c.approx = true;
  c.dbl = d;
  c.num = (i128)d;   // an integral double: its exact value
  return c;
}

// The column value (v / 10^cscale) against the constant: -1, 0, 1.
int cmp(i128 v, int cscale, const Const& c) {
  const i128 a = v * pow10(c.scale), b = c.num * pow10(cscale);
  return a < b ? -1 : a > b ? 1 : 0;
}

bool apply(const std::string& op, int c) {
  if (op == "=") return c == 0;
  if (op == "<>") return c != 0;
  if (op == ">") return c > 0;
  if (op == ">=") return c >= 0;
  if (op == "<") return c < 0;
  return c <= 0;
}

struct Col {
  std::string name;
  int scale;
  std::vector<bool> null;
  std::vector<i128> v;
  std::vector<Const> consts;
};

std::string ids_of(const std::vector<size_t>& rows) {
  std::string s;
  for (size_t r : rows) { if (!s.empty()) s += ","; s += std::to_string(r); }
  return s;
}

// The ids a statement returned, in order.
std::string ids_from(SqlDatabase& db, const std::string& sql) {
  SqlQueryResult r = run(db, sql);
  std::string s;
  for (const auto& row : r.rows) { if (!s.empty()) s += ","; s += row.values.empty() ? "?" : row.values[0]; }
  return s;
}

std::string encoding_of(SqlDatabase& db, const std::string& table, const std::string& col) {
  SqlQueryResult r = db.execute_local_query("SHOW ASQL STORAGE " + table);
  for (const auto& row : r.rows)
    if (row.values.size() > 3 && row.values[1] == col) return row.values[3];
  return "?";
}

void verify(SqlDatabase& db, const std::string& T, const std::vector<Col>& cols, size_t n, const char* step) {
  const int before = failures;
  const std::vector<std::string> ops = {"=", "<>", "<", "<=", ">", ">="};
  // Big tables: counts only for the comparisons (row lists of 20k ids say no more and cost the run its seconds).
  const bool big = n > 1000;
  for (const Col& c : cols) {
    for (const Const& k : c.consts) {
      for (const std::string& op : ops) {
        std::vector<size_t> want;
        for (size_t i = 0; i < n; ++i) if (!c.null[i] && apply(op, cmp(c.v[i], c.scale, k))) want.push_back(i);
        const std::string where = " WHERE " + c.name + " " + op + " " + k.sql;
        const std::string got_n = q1(db, "SELECT COUNT(*) FROM " + T + where);
        CHECK(got_n == std::to_string(want.size()), "%s %s: COUNT(*)%s is %s, want %zu", step, T.c_str(), where.c_str(), got_n.c_str(), want.size());
        if (!big || want.size() <= 50) {
          const std::string got = ids_from(db, "SELECT id FROM " + T + where + " ORDER BY id");
          CHECK(got == ids_of(want), "%s %s: ids%s are [%s], want [%s]", step, T.c_str(), where.c_str(), got.substr(0, 200).c_str(),
                ids_of(want).substr(0, 200).c_str());
        }
      }
    }
    // BETWEEN / NOT BETWEEN over neighbouring constants
    for (size_t a = 0; a + 1 < c.consts.size(); ++a) {
      const Const& lo = c.consts[a];
      const Const& hi = c.consts[a + 1];
      for (bool neg : {false, true}) {
        size_t want = 0;
        for (size_t i = 0; i < n; ++i) {
          if (c.null[i]) continue;
          const bool in = cmp(c.v[i], c.scale, lo) >= 0 && cmp(c.v[i], c.scale, hi) <= 0;
          if (in != neg) ++want;
        }
        const std::string where = " WHERE " + c.name + (neg ? " NOT" : "") + " BETWEEN " + lo.sql + " AND " + hi.sql;
        const std::string got = q1(db, "SELECT COUNT(*) FROM " + T + where);
        CHECK(got == std::to_string(want), "%s %s: COUNT(*)%s is %s, want %zu", step, T.c_str(), where.c_str(), got.c_str(), want);
      }
    }
    // IN / NOT IN over the exact constants
    {
      std::vector<const Const*> list;
      std::string sql_list;
      for (const Const& k : c.consts) if (!k.approx) { list.push_back(&k); sql_list += (sql_list.empty() ? "" : ", ") + k.sql; }
      for (bool neg : {false, true}) {
        std::vector<size_t> want;
        for (size_t i = 0; i < n; ++i) {
          if (c.null[i]) continue;
          bool in = false;
          for (const Const* k : list) in = in || cmp(c.v[i], c.scale, *k) == 0;
          if (in != neg) want.push_back(i);
        }
        const std::string where = " WHERE " + c.name + (neg ? " NOT" : "") + " IN (" + sql_list + ")";
        const std::string got = q1(db, "SELECT COUNT(*) FROM " + T + where);
        CHECK(got == std::to_string(want.size()), "%s %s: COUNT(*)%s is %s, want %zu", step, T.c_str(), where.c_str(), got.c_str(), want.size());
        if (!neg) {
          const std::string ids = ids_from(db, "SELECT id FROM " + T + where + " ORDER BY id");
          CHECK(ids == ids_of(want), "%s %s: ids%s are [%s], want [%s]", step, T.c_str(), where.c_str(), ids.substr(0, 200).c_str(),
                ids_of(want).substr(0, 200).c_str());
        }
      }
    }
    // IN (SELECT ...): the values of a few rows, matched against the whole column
    {
      std::vector<size_t> picked;
      for (size_t i = 0; i < n && picked.size() < 5; i += 7) if (!c.null[i]) picked.push_back(i);
      std::vector<size_t> want;
      for (size_t i = 0; i < n; ++i) {
        if (c.null[i]) continue;
        bool in = false;
        for (size_t p : picked) in = in || c.v[i] == c.v[p];
        if (in) want.push_back(i);
      }
      const std::string where = " WHERE " + c.name + " IN (SELECT " + c.name + " FROM " + T + " WHERE id IN (" + ids_of(picked) + "))";
      const std::string got = q1(db, "SELECT COUNT(*) FROM " + T + where);
      CHECK(got == std::to_string(want.size()), "%s %s: COUNT(*)%s is %s, want %zu", step, T.c_str(), where.c_str(), got.c_str(), want.size());
    }
    // MIN / MAX / SUM
    {
      bool any = false;
      i128 mn = 0, mx = 0, sum = 0;
      for (size_t i = 0; i < n; ++i) {
        if (c.null[i]) continue;
        if (!any || c.v[i] < mn) mn = c.v[i];
        if (!any || c.v[i] > mx) mx = c.v[i];
        sum += c.v[i];
        any = true;
      }
      const std::string gmn = q1(db, "SELECT MIN(" + c.name + ") FROM " + T);
      const std::string gmx = q1(db, "SELECT MAX(" + c.name + ") FROM " + T);
      const std::string gsum = q1(db, "SELECT SUM(" + c.name + ") FROM " + T);
      CHECK(gmn == scaled_text(mn, c.scale), "%s %s: MIN(%s) is %s, want %s", step, T.c_str(), c.name.c_str(), gmn.c_str(), scaled_text(mn, c.scale).c_str());
      CHECK(gmx == scaled_text(mx, c.scale), "%s %s: MAX(%s) is %s, want %s", step, T.c_str(), c.name.c_str(), gmx.c_str(), scaled_text(mx, c.scale).c_str());
      CHECK(gsum == scaled_text(sum, c.scale), "%s %s: SUM(%s) is %s, want %s", step, T.c_str(), c.name.c_str(), gsum.c_str(), scaled_text(sum, c.scale).c_str());
    }
    // ORDER BY the column (ties by id), first rows and last rows
    {
      std::vector<size_t> order;
      for (size_t i = 0; i < n; ++i) order.push_back(i);
      std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) -> bool {
        if (c.null[a] != c.null[b]) return c.null[a];   // NULLs first, as MariaDB sorts them ascending
        if (c.null[a]) return false;
        return c.v[a] < c.v[b];
      });
      std::vector<size_t> head(order.begin(), order.begin() + (long)std::min<size_t>(40, order.size()));
      const std::string got = ids_from(db, "SELECT id FROM " + T + " ORDER BY " + c.name + ", id LIMIT 40");
      CHECK(got == ids_of(head), "%s %s: ORDER BY %s is [%s], want [%s]", step, T.c_str(), c.name.c_str(), got.c_str(), ids_of(head).c_str());
      std::vector<size_t> tail(order.rbegin(), order.rbegin() + (long)std::min<size_t>(40, order.size()));
      // descending: largest first, ties by id descending, as the reversed ascending order lists them
      const std::string gotd = ids_from(db, "SELECT id FROM " + T + " ORDER BY " + c.name + " DESC, id DESC LIMIT 40");
      CHECK(gotd == ids_of(tail), "%s %s: ORDER BY %s DESC is [%s], want [%s]", step, T.c_str(), c.name.c_str(), gotd.c_str(), ids_of(tail).c_str());
    }
    // window functions: ROW_NUMBER over the column (ties by id) lists the rows in its order; a RANGE frame of
    // one unit either side counts the rows whose value is within it - arithmetic on the number, not its rank
    {
      std::vector<size_t> order;
      for (size_t i = 0; i < n; ++i) order.push_back(i);
      std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) -> bool {
        if (c.null[a] != c.null[b]) return c.null[a];
        if (c.null[a]) return false;
        return c.v[a] < c.v[b];
      });
      std::vector<size_t> head(order.begin(), order.begin() + (long)std::min<size_t>(40, order.size()));
      const std::string got = ids_from(db, "SELECT id FROM (SELECT id, ROW_NUMBER() OVER (ORDER BY " + c.name + ", id) AS rn FROM " + T +
                                               ") w ORDER BY rn LIMIT 40");
      CHECK(got == ids_of(head), "%s %s: ROW_NUMBER() OVER (ORDER BY %s) is [%s], want [%s]", step, T.c_str(), c.name.c_str(), got.c_str(),
            ids_of(head).c_str());
      if (!big) {
        const i128 unit = pow10(c.scale);
        std::string want;
        for (size_t i = 0; i < n; ++i) {
          size_t cnt = 0;
          if (!c.null[i]) {
            for (size_t j = 0; j < n; ++j) if (!c.null[j] && c.v[j] - c.v[i] <= unit && c.v[i] - c.v[j] <= unit) ++cnt;
          } else {
            for (size_t j = 0; j < n; ++j) cnt += c.null[j];   // a NULL's frame is the NULL peers
          }
          want += (want.empty() ? "" : ",") + std::to_string(cnt);
        }
        // An integer key whose bound leaves BIGINT (2^63 - 1 + 1, -2^63 - 1): MariaDB 10.11 refuses the statement with
        // error 1690, "BIGINT value is out of range in '... + 1'".
        bool overflows = false;
        if (c.scale == 0)
          for (size_t i = 0; i < n; ++i) overflows = overflows || (!c.null[i] && (c.v[i] + 1 > MAXV || c.v[i] - 1 < -MAXV - 1));
        const std::string sql = "SELECT COUNT(*) OVER (ORDER BY " + c.name + " RANGE BETWEEN 1 PRECEDING AND 1 FOLLOWING) AS c FROM " + T + " ORDER BY id";
        if (overflows) {
          SqlQueryResult r = db.execute_local_query(sql);
          CHECK(!r.success && r.error_message.rfind("BIGINT value is out of range in", 0) == 0,
                "%s %s: RANGE over %s past BIGINT's range: %s, want the out-of-range error", step, T.c_str(), c.name.c_str(),
                r.success ? "answered" : r.error_message.c_str());
        } else {
          const std::string got_r = ids_from(db, sql);
          CHECK(got_r == want, "%s %s: RANGE 1 PRECEDING/FOLLOWING over %s is [%s], want [%s]", step, T.c_str(), c.name.c_str(),
                got_r.substr(0, 200).c_str(), want.substr(0, 200).c_str());
        }
      }
    }
    // a self-join on the column: the pairs with one value
    if (!big) {
      size_t want = 0;
      for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < n; ++j) if (!c.null[i] && !c.null[j] && c.v[i] == c.v[j]) ++want;
      const std::string got = q1(db, "SELECT COUNT(*) FROM " + T + " a JOIN " + T + " b ON a." + c.name + " = b." + c.name);
      CHECK(got == std::to_string(want), "%s %s: self-join on %s is %s pairs, want %zu", step, T.c_str(), c.name.c_str(), got.c_str(), want);
    }
  }
  // A join of typed integer columns of ordinary size: the direct numeric join once probed with every key but 0
  // read as NULL, and met one row of these.
  {
    const std::string got = q1(db, "SELECT COUNT(*) FROM " + T + " a JOIN " + T + " b ON a.id = b.id");
    CHECK(got == std::to_string(n), "%s %s: self-join on id is %s pairs, want %zu", step, T.c_str(), got.c_str(), n);
  }
  std::printf("  [%s] %s %s: %zu rows (%s)\n", failures == before ? "ok" : "FAIL", step, T.c_str(), n,
              [&] { std::string e; for (const Col& c : cols) e += (e.empty() ? "" : ", ") + c.name + " " + encoding_of(db, T, c.name); return e; }().c_str());
}

}  // namespace

int main() {
  std::printf("=== ASQL wide number check (past 2^53, near 2^63, 18-digit decimals) ===\n");
  const std::string root = "/tmp/aios-asql-widecheck";
  { std::error_code fs_err; std::filesystem::remove_all(root, fs_err); }
  KernelConfig cfg;
  cfg.root = root;
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::printf("boot failed\n"); return 1; }
  const i128 P53 = (i128)9007199254740992LL;
  const i128 MAX = (i128)9223372036854775807LL;
  const i128 D18 = (i128)999999999999999999LL;   // DECIMAL(18,2) 9999999999999999.99, scaled
  const i128 F18 = (i128)123456789123456789LL;   // DECIMAL(18,9) 123456789.123456789, scaled
  struct Made { std::string table; std::vector<Col> cols; size_t n; };
  std::vector<Made> made;
  {
    SqlDatabase db(**k);
    db.init("sql_db");
    run(db, "CREATE DATABASE IF NOT EXISTS widedb");
    run(db, "USE widedb");
    for (size_t n : {size_t(60), size_t(20000)}) {
      const std::string T = n > 1000 ? "wbig" : "wsmall";
      // b: a few values each side of 2^53, repeated; k: distinct ids counting down from 2^63-1 (the key);
      // d: 18-digit decimals one cent apart; s: signed, both ends of BIGINT.
      // f: DECIMAL(18,9) of 18 digits, 123456789.123456789 and its neighbours - kept as scaled integers
      Col b{"b", 0, {}, {}, {}}, kk{"k", 0, {}, {}, {}}, d{"d", 2, {}, {}, {}}, s{"s", 0, {}, {}, {}}, f{"f", 9, {}, {}, {}};
      for (size_t i = 0; i < n; ++i) {
        b.null.push_back(i % 17 == 5);
        b.v.push_back(P53 + (i128)((long long)(i % 9) - 4));
        kk.null.push_back(false);
        kk.v.push_back(MAX - (i128)(i * 3));
        // the big table's d and s spread over many values, which re-encodes them typed (DECIMAL kept scaled,
        // and one column spanning the whole BIGINT range)
        d.null.push_back(i % 23 == 7);
        d.v.push_back(D18 - (i128)(n > 1000 ? i * 3 : i % 13));
        s.null.push_back(false);
        const i128 off = (i128)(n > 1000 ? i : i % 5);
        s.v.push_back(i % 2 ? MAX - off : -MAX - 1 + off);
        f.null.push_back(i % 29 == 3);
        f.v.push_back(F18 + (i128)((long long)(i % 11) - 5));
      }
      b.consts = {exact(P53 - 1, 0, false), exact(P53, 0, false), exact(P53 + 1, 0, false), exact(P53 + 1, 0, true),
                  exact(P53 * 10 + 5, 1, false), exact(P53 * 10 + 15, 1, true), exact((P53 + 2) * pow10(16) + 1, 16, false),
                  approx(9007199254740993.0)};
      kk.consts = {exact(MAX - 30, 0, false), exact(MAX - 4, 0, false), exact(MAX - 3, 0, true), exact(MAX - 1, 0, false),
                   exact(MAX, 0, false), exact(MAX * 10 + 5, 1, false), exact(MAX + 1, 0, false)};
      d.consts = {exact(D18 - 12, 2, false), exact(D18 - 6, 2, true), exact((D18 - 5) * 10 + 5, 3, false), exact(D18 - 1, 2, false),
                  exact(D18, 2, false)};
      f.consts = {exact(F18 - 3, 9, false), exact(F18 - 1, 9, true), exact(F18 * 10 + 5, 10, false), exact(F18, 9, false),
                  exact(F18 + 4, 9, true), exact(F18 / 1000000000, 0, false)};
      s.consts = {exact(-MAX - 1, 0, false), exact(-MAX + 2, 0, false), exact(-MAX + 2, 0, true), exact(MAX - 2, 0, false), exact(MAX, 0, false)};
      run(db, "CREATE TABLE " + T + " (id INT NOT NULL PRIMARY KEY, b BIGINT, k BIGINT NOT NULL, d DECIMAL(18,2), s BIGINT NOT NULL, f DECIMAL(18,9)" +
                  std::string(n > 1000 ? ", KEY (k), KEY (b)" : "") + ")");
      std::string batch;
      for (size_t i = 0; i < n; ++i) {
        batch += (batch.empty() ? "" : ",") + std::string("(") + std::to_string(i) + "," + (b.null[i] ? "NULL" : i128_text(b.v[i])) + "," +
                 i128_text(kk.v[i]) + "," + (d.null[i] ? "NULL" : scaled_text(d.v[i], 2)) + "," + i128_text(s.v[i]) + "," +
                 (f.null[i] ? "NULL" : scaled_text(f.v[i], 9)) + ")";
        if (batch.size() > 60000 || i + 1 == n) { run(db, "INSERT INTO " + T + " VALUES " + batch); batch.clear(); }
      }
      const std::vector<Col> cols = {b, kk, d, s, f};
      verify(db, T, cols, n, "inserted");
      db.optimize_all_tables("widedb");
      verify(db, T, cols, n, "re-encoded");
      made.push_back({T, cols, n});
    }
    auto f = db.flush_now();
    CHECK(f.ok(), "checkpoint failed");
  }
  {
    // read back from the store: the encodings and values as a restart finds them
    SqlDatabase db(**k);
    db.init("sql_db");
    run(db, "USE widedb");
    for (const Made& m : made) verify(db, m.table, m.cols, m.n, "reopened");
  }
  std::printf("%d of %d checks passed\n", checks - failures, checks);
  return failures ? 1 : 0;
}
