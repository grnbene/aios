// Checks the per-segment encodings (SegmentFOR for integers, ALP for
// doubles) against a model of what the table holds: the column is re-encoded,
// then written to, NULLed, deleted from and appended to past segment
// boundaries, written out and read back - and after every step each cell,
// each aggregate and each filtered count must be what the model says.
#undef NDEBUG
#include <filesystem>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "aios/userland/portable_int128.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"
#include "check-root.hpp"

using namespace aios;
using namespace aios::userland;

namespace {

struct Row {
  int64_t id;
  std::optional<int64_t> ts;       // epoch milliseconds, spanning far more than 32 bits
  std::optional<int64_t> seq;      // sequential with rare huge outliers
  std::optional<std::string> amt;  // a double's text as the engine prints it
};

int failures = 0;
#define CHECK(cond, ...)                                  \
  do {                                                    \
    if (!(cond)) {                                        \
      ++failures;                                         \
      std::printf("  [FAIL] " __VA_ARGS__);               \
      std::printf("\n");                                  \
      if (failures > 20) { std::printf("too many failures\n"); std::exit(1); } \
    }                                                     \
  } while (0)

std::string q1(SqlDatabase& db, const std::string& sql) {
  SqlQueryResult r = db.execute_local_query(sql);
  if (!r.success) { std::printf("  query failed: %s\n  %s\n", sql.c_str(), r.error_message.c_str()); std::exit(1); }
  if (r.rows.empty() || r.rows[0].values.empty()) return "<none>";
  return r.rows[0].values[0];
}

void exec(SqlDatabase& db, const std::string& sql) {
  SqlQueryResult r = db.execute_local_query(sql);
  if (!r.success) { std::printf("  statement failed: %s\n  %s\n", sql.c_str(), r.error_message.c_str()); std::exit(1); }
}

// The text the engine prints for a double (what a typed double column reads back as).
std::string dtext(SqlDatabase& db, double d) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.17g", d);
  return q1(db, std::string("SELECT CAST(") + buf + " AS DOUBLE)");
}

std::string lit(const std::optional<int64_t>& v) { return v ? std::to_string(*v) : "NULL"; }
std::string lit(const std::optional<std::string>& v) { return v ? *v : "NULL"; }

std::string encoding_of_table(SqlDatabase& db, const std::string& table, const std::string& col) {
  SqlQueryResult r = db.execute_local_query("SHOW ASQL STORAGE " + table);
  for (const auto& row : r.rows)
    if (row.values.size() > 3 && row.values[1] == col) return row.values[3];
  return "?";
}

std::string encoding_of(SqlDatabase& db, const std::string& col) {
  SqlQueryResult r = db.execute_local_query("SHOW ASQL STORAGE segcheck");
  for (const auto& row : r.rows)
    if (row.values.size() > 3 && row.values[1] == col) return row.values[3];
  return "?";
}

void verify(SqlDatabase& db, const std::vector<Row>& model, const char* step) {
  const SqlTable* t = db.find_table("segcheck", "segdb");
  CHECK(t != nullptr, "%s: table missing", step);
  if (!t) return;
  CHECK(t->row_count() == model.size(), "%s: %zu rows, model has %zu", step, t->row_count(), model.size());
  const size_t n = std::min(t->row_count(), model.size());
  size_t bad = 0;
  for (size_t r = 0; r < n && bad < 5; ++r) {
    const Row& m = model[r];
    const std::string want[4] = {std::to_string(m.id), m.ts ? std::to_string(*m.ts) : "NULL",
                                 m.seq ? std::to_string(*m.seq) : "NULL", m.amt ? *m.amt : "NULL"};
    for (size_t c = 0; c < 4; ++c) {
      const bool null = t->col_data[c].is_null(r);
      const std::string got = null ? "NULL" : t->cell(r, c);
      if (got != want[c]) {
        ++bad;
        CHECK(false, "%s: row %zu col %zu is %s, model %s", step, r, c, got.c_str(), want[c].c_str());
      }
    }
  }
  // Aggregates and filters through the query engine.
  AIOS_INT128 ts_sum = 0, seq_sum = 0;
  int64_t ts_min = INT64_MAX, ts_max = INT64_MIN, seq_max = INT64_MIN;
  size_t ts_n = 0, amt_n = 0, ts_gt = 0, seq_le = 0;
  double amt_min = 1e300, amt_max = -1e300;
  const int64_t ts_pivot = model.empty() || !model[model.size() / 2].ts ? 0 : *model[model.size() / 2].ts;
  const int64_t seq_pivot = 25000;
  for (const Row& m : model) {
    if (m.ts) { ts_sum += *m.ts; ts_n++; ts_min = std::min(ts_min, *m.ts); ts_max = std::max(ts_max, *m.ts); ts_gt += *m.ts > ts_pivot; }
    if (m.seq) { seq_sum += *m.seq; seq_max = std::max(seq_max, *m.seq); seq_le += *m.seq <= seq_pivot; }
    if (m.amt) { amt_n++; double d = std::strtod(m.amt->c_str(), nullptr); amt_min = std::min(amt_min, d); amt_max = std::max(amt_max, d); }
  }
  auto i128 = [](AIOS_INT128 v) {
    bool neg = v < 0; AIOS_UINT128 u = neg ? -(AIOS_UINT128)v : v; std::string s;
    do { s.insert(s.begin(), char('0' + (int)(u % 10))); u /= 10; } while (u);
    return neg ? "-" + s : s;
  };
  CHECK(q1(db, "SELECT COUNT(ts) FROM segcheck") == std::to_string(ts_n), "%s: COUNT(ts)", step);
  if (ts_n) {
    // An exact SUM only while it fits a BIGINT: past that the engine sums
    // as a double (as it did before any of these encodings).
    // Past 2^53 the engine sums BIGINTs as doubles (it did before these
    // encodings too): compared to a relative 1e-12 there, exactly below.
    const std::string got = q1(db, "SELECT SUM(ts) FROM segcheck");
    const long double g = std::strtold(got.c_str(), nullptr), want = (long double)ts_sum;
    CHECK(got == i128(ts_sum) || std::fabs((double)((g - want) / want)) < 1e-12, "%s: SUM(ts) %s want %s", step, got.c_str(),
          i128(ts_sum).c_str());
    CHECK(q1(db, "SELECT MIN(ts) FROM segcheck") == std::to_string(ts_min), "%s: MIN(ts)", step);
    CHECK(q1(db, "SELECT MAX(ts) FROM segcheck") == std::to_string(ts_max), "%s: MAX(ts)", step);
    CHECK(q1(db, "SELECT COUNT(*) FROM segcheck WHERE ts > " + std::to_string(ts_pivot)) == std::to_string(ts_gt),
          "%s: ts > pivot %s want %zu", step, q1(db, "SELECT COUNT(*) FROM segcheck WHERE ts > " + std::to_string(ts_pivot)).c_str(), ts_gt);
  }
  {
    AIOS_INT128 small = 0;
    for (const Row& m : model) if (m.seq && *m.seq < 1000000) small += *m.seq;
    const std::string got = q1(db, "SELECT SUM(seq) FROM segcheck WHERE seq < 1000000");
    CHECK(got == i128(small), "%s: SUM(seq) of the small ones %s want %s", step, got.c_str(), i128(small).c_str());
    const std::string all = q1(db, "SELECT SUM(seq) FROM segcheck");
    const long double g = std::strtold(all.c_str(), nullptr), want = (long double)seq_sum;
    CHECK(all == i128(seq_sum) || std::fabs((double)((g - want) / want)) < 1e-12, "%s: SUM(seq) %s want %s", step, all.c_str(),
          i128(seq_sum).c_str());
  }
  CHECK(q1(db, "SELECT COUNT(*) FROM segcheck WHERE seq <= " + std::to_string(seq_pivot)) == std::to_string(seq_le), "%s: seq <= pivot", step);
  CHECK(q1(db, "SELECT COUNT(amt) FROM segcheck") == std::to_string(amt_n), "%s: COUNT(amt)", step);
  if (amt_n) {
    CHECK(q1(db, "SELECT MIN(amt) FROM segcheck") == dtext(db, amt_min), "%s: MIN(amt) %s want %s", step,
          q1(db, "SELECT MIN(amt) FROM segcheck").c_str(), dtext(db, amt_min).c_str());
    CHECK(q1(db, "SELECT MAX(amt) FROM segcheck") == dtext(db, amt_max), "%s: MAX(amt)", step);
  }
  // A point lookup of every 997th row by its wide value.
  for (size_t r = 0; r < model.size(); r += 997) {
    if (!model[r].ts) continue;
    const std::string c = q1(db, "SELECT COUNT(*) FROM segcheck WHERE ts = " + std::to_string(*model[r].ts));
    size_t want = 0;
    for (const Row& m : model) want += m.ts && *m.ts == *model[r].ts;
    CHECK(c == std::to_string(want), "%s: ts = row %zu's value counts %s, want %zu", step, r, c.c_str(), want);
  }
  std::printf("  [ok] %s: %zu rows, ts %s, seq %s, amt %s\n", step, model.size(), encoding_of(db, "ts").c_str(),
              encoding_of(db, "seq").c_str(), encoding_of(db, "amt").c_str());
}

}  // namespace

int main() {
  std::printf("=== ASQL per-segment encoding check (SegmentFOR, ALP) ===\n");
  const CheckRoot check_root("/tmp/aios-asql-segcheck");   // this run's own (see check-root.hpp)
  const std::string& root = check_root.path;
  KernelConfig cfg;
  cfg.root = root;
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::printf("boot failed\n"); return 1; }
  std::mt19937_64 rng(20261016);
  std::vector<Row> model;
  {
    SqlDatabase db(**k);
    db.init("sql_db");
    exec(db, "CREATE DATABASE IF NOT EXISTS segdb");
    exec(db, "USE segdb");
    exec(db, "CREATE TABLE segcheck (id BIGINT NOT NULL, ts BIGINT, seq BIGINT, amt DOUBLE)");
    // 9,000 rows: four full segments and a partial one.
    const int64_t t0 = 1760000000000LL;   // 2025 in milliseconds
    std::string batch;
    for (int64_t i = 0; i < 9000; ++i) {
      Row r;
      r.id = i;
      if (i % 37 != 0) r.ts = t0 + i * 1000003 + (int64_t)(rng() % 997);
      if (i % 53 != 0) r.seq = (i == 4100) ? 4000000000000000000LL : i;   // one huge outlier in segment 2
      if (i % 41 != 0) {
        double d;
        // Decimals as text gives them: k / 10^places is the double nearest
        // the decimal (what reading "12.3456" yields).
        if (i % 9 == 0) d = std::ldexp((double)(rng() >> 11), -53) * 100.0;           // full precision: exceptions
        else if (i % 7 == 0) d = (double)((int64_t)(rng() % 2000000000) - 1000000000) / 1000000.0;   // six places
        else d = (double)((int64_t)(rng() % 20000000) - 10000000) / 10000.0;          // four places
        r.amt = dtext(db, d);
      }
      model.push_back(r);
      batch += (batch.empty() ? "" : ",") + std::string("(") + std::to_string(r.id) + "," + lit(r.ts) + "," + lit(r.seq) + "," + lit(r.amt) + ")";
      if (batch.size() > 60000 || i == 8999) { exec(db, "INSERT INTO segcheck VALUES " + batch); batch.clear(); }
    }
    verify(db, model, "inserted (dictionary)");
    db.optimize_all_tables("segdb");
    verify(db, model, "re-encoded");
    CHECK(encoding_of(db, "ts") == "SegFOR", "ts is %s, want SegFOR", encoding_of(db, "ts").c_str());
    CHECK(encoding_of(db, "seq") == "SegFOR", "seq is %s, want SegFOR", encoding_of(db, "seq").c_str());
    CHECK(encoding_of(db, "amt") == "ALP", "amt is %s, want ALP", encoding_of(db, "amt").c_str());

    // Writes the segments take where they are: a value inside the segment's
    // range, NULL and back, a decimal of the segment's places, a value no
    // decimal gives back (an exception, in a segment that has them).
    for (int k2 = 0; k2 < 300; ++k2) {
      const size_t r = rng() % 8192;   // the full segments
      Row& m = model[r];
      const size_t seg0 = r / 2048 * 2048;
      switch (k2 % 5) {
        case 0:
          if (model[seg0].ts && model[seg0 + 1].ts) m.ts = std::min(*model[seg0].ts, *model[seg0 + 1].ts) + 1;
          break;
        case 1: m.ts.reset(); break;
        case 2: m.ts = model[seg0 + 5].ts; break;
        case 3: m.amt = dtext(db, (double)((int64_t)(rng() % 2000000) - 1000000) / 10000.0); break;
        case 4: m.amt = (k2 & 1) ? std::optional<std::string>() : dtext(db, std::ldexp((double)(rng() >> 11), -53) * 100.0); break;
      }
      exec(db, "UPDATE segcheck SET ts = " + lit(m.ts) + ", amt = " + lit(m.amt) + " WHERE id = " + std::to_string(m.id));
    }
    verify(db, model, "updated within the segments");
    CHECK(encoding_of(db, "ts") == "SegFOR", "ts became %s on writes it can take", encoding_of(db, "ts").c_str());
    CHECK(encoding_of(db, "amt") == "ALP", "amt became %s on writes it can take", encoding_of(db, "amt").c_str());

    // Writes that fit the segment, ones that widen it, NULLs both ways.
    for (int k2 = 0; k2 < 400; ++k2) {
      const size_t r = rng() % model.size();
      Row& m = model[r];
      switch (k2 % 6) {
        case 0: m.ts = m.ts ? *m.ts + 1 : t0; break;
        case 1: m.ts = (int64_t)(rng() >> 1) * ((rng() & 1) ? 1 : -1); break;   // anywhere in int64
        case 2: m.ts.reset(); break;
        case 3: m.seq = (int64_t)r + 1; break;
        case 4: m.amt = dtext(db, (double)(int64_t)(rng() % 1000000) / 100.0); break;
        case 5: m.amt = (rng() & 1) ? std::optional<std::string>() : dtext(db, std::ldexp((double)(rng() >> 11), -40)); break;
      }
      exec(db, "UPDATE segcheck SET ts = " + lit(m.ts) + ", seq = " + lit(m.seq) + ", amt = " + lit(m.amt) +
                   " WHERE id = " + std::to_string(m.id));
    }
    verify(db, model, "updated in place");
    // Deletes: one row, then a range spanning a segment boundary.
    {
      const int64_t id = model[5000].id;
      exec(db, "DELETE FROM segcheck WHERE id = " + std::to_string(id));
      model.erase(model.begin() + 5000);
      verify(db, model, "one row deleted");
      exec(db, "DELETE FROM segcheck WHERE id >= 2000 AND id < 2300");
      std::vector<Row> keep;
      for (const Row& m : model) if (m.id < 2000 || m.id >= 2300) keep.push_back(m);
      model.swap(keep);
      verify(db, model, "range deleted");
    }
    // Appends: into the last segment and past it, with values that widen it.
    for (int64_t i = 0; i < 3000; ++i) {
      Row r;
      r.id = 100000 + i;
      if (i % 11) r.ts = t0 + (int64_t)(rng() % 4000000000000LL);
      if (i % 13) r.seq = 9000 + i;
      if (i % 17) r.amt = dtext(db, (double)(int64_t)(rng() % 100000) / 1000.0);
      if (i == 1500) r.ts = INT64_MIN + 5;
      model.push_back(r);
      exec(db, "INSERT INTO segcheck VALUES (" + std::to_string(r.id) + "," + lit(r.ts) + "," + lit(r.seq) + "," + lit(r.amt) + ")");
    }
    verify(db, model, "appended");
    db.optimize_all_tables("segdb");
    verify(db, model, "re-encoded again");
    // Appends to encoded columns: into the partial last segment, then new
    // ones, widening as they go (a value below the base, a huge one, NULLs,
    // exceptions).
    for (int64_t i = 0; i < 2500; ++i) {
      Row r;
      r.id = 200000 + i;
      if (i % 19) r.ts = t0 + 9000000000000LL + i * 7919 + ((i == 700) ? -5000000000000LL : 0) + ((i == 1900) ? 3000000000000000000LL : 0);
      if (i % 23) r.seq = 20000 + i;
      if (i % 29) r.amt = (i % 31 == 0) ? dtext(db, std::ldexp((double)(rng() >> 11), -53)) : dtext(db, (double)(int64_t)(rng() % 100000) / 100.0);
      model.push_back(r);
      exec(db, "INSERT INTO segcheck VALUES (" + std::to_string(r.id) + "," + lit(r.ts) + "," + lit(r.seq) + "," + lit(r.amt) + ")");
    }
    verify(db, model, "appended to the encoded columns");
    CHECK(encoding_of(db, "ts") == "SegFOR", "ts became %s on appends", encoding_of(db, "ts").c_str());
    CHECK(encoding_of(db, "seq") == "SegFOR", "seq became %s on appends", encoding_of(db, "seq").c_str());
    CHECK(encoding_of(db, "amt") == "ALP", "amt became %s on appends", encoding_of(db, "amt").c_str());
    // Digits in a VARCHAR stay text: wide ones (phone numbers) are not
    // made SegmentFOR, and a comparison with a quoted literal is a text one.
    exec(db, "CREATE TABLE phones (id INT NOT NULL, phone VARCHAR(20))");
    std::vector<std::string> phones;
    std::string pb;
    for (int i = 0; i < 6000; ++i) {
      phones.push_back(std::to_string(15550000000LL + (int64_t)(rng() % 9000000000LL)));
      pb += (pb.empty() ? "" : ",") + std::string("(") + std::to_string(i) + ",'" + phones.back() + "')";
    }
    exec(db, "INSERT INTO phones VALUES " + pb);
    db.optimize_all_tables("segdb");
    {
      SqlQueryResult r = db.execute_local_query("SHOW ASQL STORAGE phones");
      std::string enc;
      for (const auto& row : r.rows) if (row.values.size() > 3 && row.values[1] == "phone") enc = row.values[3];
      CHECK(enc != "SegFOR", "a VARCHAR of digits became %s", enc.c_str());
      size_t gt = 0;
      for (const auto& p : phones) gt += p > "2";
      CHECK(q1(db, "SELECT COUNT(*) FROM phones WHERE phone > '2'") == std::to_string(gt), "VARCHAR digits compared as numbers");
      std::printf("  [ok] VARCHAR digits stay text (%s)\n", enc.c_str());
    }
    // Small digits in a VARCHAR (what FOR used to take): '10' sorts before
    // '5' as text, and a re-encode must not change that.
    exec(db, "CREATE TABLE codes (id INT NOT NULL, code VARCHAR(8))");
    std::vector<std::string> codes;
    std::string cb;
    for (int i = 0; i < 5000; ++i) {
      codes.push_back(std::to_string(1 + i % 120));
      cb += (cb.empty() ? "" : ",") + std::string("(") + std::to_string(i) + ",'" + codes.back() + "')";
    }
    exec(db, "INSERT INTO codes VALUES " + cb);
    db.optimize_all_tables("segdb");
    {
      SqlQueryResult r = db.execute_local_query("SHOW ASQL STORAGE codes");
      std::string enc;
      for (const auto& row : r.rows) if (row.values.size() > 3 && row.values[1] == "code") enc = row.values[3];
      CHECK(enc != "FOR" && enc != "SegFOR", "a VARCHAR of small digits became %s", enc.c_str());
      auto count = [&](auto pred) { size_t n = 0; for (const auto& c : codes) n += pred(c); return std::to_string(n); };
      CHECK(q1(db, "SELECT COUNT(*) FROM codes WHERE code > '5'") == count([](const std::string& c) { return c > "5"; }), "code > '5'");
      CHECK(q1(db, "SELECT COUNT(*) FROM codes WHERE code > '10'") == count([](const std::string& c) { return c > "10"; }), "code > '10'");
      CHECK(q1(db, "SELECT COUNT(*) FROM codes WHERE code = '7'") == count([](const std::string& c) { return c == "7"; }), "code = '7'");
      CHECK(q1(db, "SELECT COUNT(*) FROM codes WHERE code BETWEEN '2' AND '4'") ==
                count([](const std::string& c) { return c >= "2" && c <= "4"; }), "code BETWEEN '2' AND '4'");
      CHECK(q1(db, "SELECT code FROM codes ORDER BY code DESC LIMIT 1") == "99", "ORDER BY code DESC");
      std::printf("  [ok] VARCHAR small digits keep text comparison (%s)\n", enc.c_str());
    }
    // The shape found on the live mirror: a VARCHAR key of '1'..'8'.
    exec(db, "CREATE TABLE migrations (id VARCHAR(64) NOT NULL PRIMARY KEY, applied_at VARCHAR(32))");
    for (int i = 1; i <= 8; ++i) exec(db, "INSERT INTO migrations VALUES ('" + std::to_string(i) + "', '2026-10-0" + std::to_string(i) + "')");
    db.optimize_all_tables("segdb");
    CHECK(q1(db, "SELECT COUNT(*) FROM migrations WHERE id > '5'") == "3", "migrations id > '5' is %s, want 3",
          q1(db, "SELECT COUNT(*) FROM migrations WHERE id > '5'").c_str());
    CHECK(q1(db, "SELECT COUNT(*) FROM migrations WHERE id > '10'") == "7", "migrations id > '10' is %s, want 7",
          q1(db, "SELECT COUNT(*) FROM migrations WHERE id > '10'").c_str());
    std::printf("  [ok] VARCHAR key '1'..'8' compares as text after a re-encode\n");
    // An INT key compared with a quoted number (what PHP sends): numeric.
    // With a PRIMARY KEY on it (the row index answers) and without.
    for (const char* tbl : {"intkeys", "intplain"}) {
      const std::string T = tbl;
      exec(db, "CREATE TABLE " + T + " (id INT NOT NULL" + (T == "intkeys" ? " PRIMARY KEY" : "") + ", v VARCHAR(8))");
      for (int i = 1; i <= 8; ++i) exec(db, "INSERT INTO " + T + " VALUES (" + std::to_string(i) + ", 'x')");
      db.optimize_all_tables("segdb");
      for (const char* q : {"'5'", "5", "'10'", "10", "'4.5'"}) {
        const std::string sql = "SELECT COUNT(*) FROM " + T + " WHERE id > " + q;
        const std::string want = std::string(q) == "'10'" || std::string(q) == "10" ? "0" : std::string(q) == "'4.5'" ? "4" : "3";
        CHECK(q1(db, sql) == want, "%s id > %s is %s, want %s", tbl, q, q1(db, sql).c_str(), want.c_str());
      }
      for (const char* q : {"'5'", "5"}) {
        const std::string sql = "SELECT COUNT(*) FROM " + T + " WHERE id < " + q;
        CHECK(q1(db, sql) == "4", "%s id < %s is %s, want 4", tbl, q, q1(db, sql).c_str());
      }
      CHECK(q1(db, "SELECT COUNT(*) FROM " + T + " WHERE id = '7'") == "1", "%s id = '7'", tbl);
      CHECK(q1(db, "SELECT COUNT(*) FROM " + T + " WHERE id BETWEEN '2' AND '4'") == "3", "%s BETWEEN '2' AND '4' is %s", tbl,
            q1(db, "SELECT COUNT(*) FROM " + T + " WHERE id BETWEEN '2' AND '4'").c_str());
      std::printf("  [ok] INT %s against quoted numbers (%s)\n", tbl, encoding_of_table(db, T, "id").c_str());
    }
    auto f = db.flush_now();
    CHECK(f.ok(), "checkpoint failed");
  }
  {
    SqlDatabase db2(**k);
    db2.init("sql_db");
    exec(db2, "USE segdb");
    verify(db2, model, "written out and read back");
    exec(db2, "DELETE FROM segcheck WHERE id < 50");
    std::vector<Row> keep;
    for (const Row& m : model) if (m.id >= 50) keep.push_back(m);
    model.swap(keep);
    verify(db2, model, "deleted after reload");
  }
  if (failures) { std::printf("=== %d FAILURE(S) ===\n", failures); return 1; }
  std::printf("=== ALL SEGMENT ENCODING CHECKS PASSED ===\n");
  return 0;
}
