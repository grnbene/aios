// A DATETIME column whose values are all canonical text, in runs (orders entered in the same second, rows in time
// order) is stored as seconds (TemporalSeconds, ~29 bits a row), not as a run-length dictionary of 19-byte strings.
// atl's salesorders.date_created (355k rows, 151k distinct, every value canonical) was RLE at 15 B a row; its
// TemporalSeconds form is a quarter of that.
#include <cstdio>
#include <string>
#include <vector>

#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"

using namespace aios;
using namespace aios::userland;

static int failures = 0;
#define OK(c, ...) do { if (!(c)) { std::printf("  [FAIL] "); ++failures; } else std::printf("  [ok] "); std::printf(__VA_ARGS__); std::printf("\n"); } while (0)

static std::string stamp(long long secs) {
  // seconds since 2013-06-26 00:00:00 UTC (1372204800), as 'YYYY-MM-DD HH:MM:SS'
  const long long t = 1372204800 + secs;
  const long long days = t / 86400, rem = t % 86400;
  // civil from days (Howard Hinnant)
  long long z = days + 719468;
  const long long era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const long long y = (long long)yoe + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  char buf[32];
  std::snprintf(buf, sizeof buf, "%04lld-%02u-%02u %02lld:%02lld:%02lld", y + (m <= 2), m, d, rem / 3600, (rem / 60) % 60, rem % 60);
  return buf;
}

int main() {
  KernelConfig cfg;
  cfg.root = "/tmp/aios-asql-temporal-choice-test";
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::printf("kernel boot failed\n"); return 1; }
  SqlDatabase db(**k);
  db.init("sql_db");

  const size_t N = 120000;
  db.execute_local_query("DROP TABLE IF EXISTS o");
  db.execute_local_query("CREATE TABLE o (orderno INT NOT NULL, date_created DATETIME NOT NULL, note VARCHAR(10), PRIMARY KEY(orderno))");
  std::vector<std::string> model(N);
  {
    // Time moves forward ~13 years over the rows; every other order shares its second with the one before (runs of 2-3).
    std::string sql;
    long long secs = 0;
    for (size_t i = 0; i < N; ++i) {
      if (i % 5 == 0 || i % 5 == 3) secs += 1200 + (long long)((i * 7919) % 5000);
      model[i] = stamp(secs);
      sql += sql.empty() ? "INSERT INTO o VALUES " : ",";
      sql += "(" + std::to_string(i + 1) + ",'" + model[i] + "','x')";
      if ((i + 1) % 2000 == 0) { db.execute_local_query(sql); sql.clear(); }
    }
    if (!sql.empty()) db.execute_local_query(sql);
  }
  SqlTable* t = db.resolve_table("o", "aios_db");
  if (!t || t->row_count() != N) { std::printf("table not loaded\n"); return 1; }
  const int c = t->get_column_index("date_created");
  for (size_t i = 0; i < t->col_data.size(); ++i)
    t->col_data[i].optimize_encoding(t->columns[i].type, t->columns[i].scale, t->columns[i].temporal);
  ColumnStore& cs = t->col_data[(size_t)c];
  size_t ndv = 1;
  for (size_t i = 1; i < N; ++i) if (model[i] != model[i - 1]) ++ndv;
  std::printf("date_created: temporal flag %u, %zu rows, %zu distinct, encoding %d, %u bits, %zu bytes (%.1f a row)\n",
              (unsigned)t->columns[(size_t)c].temporal, N, ndv, (int)cs.encoding, (unsigned)cs.bit_width_, cs.estimate_memory_bytes(),
              (double)cs.estimate_memory_bytes() / N);
  OK(cs.encoding == ColumnEncoding::TemporalSeconds, "a DATETIME column of canonical values in runs is TemporalSeconds (encoding %d)", (int)cs.encoding);
  OK(cs.estimate_memory_bytes() < N * 5, "and under 5 B a row: %zu bytes", cs.estimate_memory_bytes());
  size_t bad = 0;
  for (size_t i = 0; i < N; ++i) if (cs.get(i) != model[i]) ++bad;
  OK(bad == 0, "every value reads back as written (%zu wrong)", bad);

  // A column stored as a run-length dictionary of date strings (encoded before the temporal rule existed, as atl's
  // salesorders.date_created and debtortrans.inputdate still are) is turned into seconds at the next checkpoint,
  // as a plain dictionary of dates already was.
  cs.optimize_encoding(t->columns[(size_t)c].type, t->columns[(size_t)c].scale, /*temporal=*/0);
  std::printf("re-encoded without the temporal rule: encoding %d, %zu bytes\n", (int)cs.encoding, cs.estimate_memory_bytes());
  if (cs.encoding != ColumnEncoding::RLE) { std::printf("precondition: expected RLE\n"); return 1; }
  db.execute_local_query("UPDATE o SET note = 'y' WHERE orderno = 1");   // a change to another column, so the checkpoint has work
  auto fl = db.flush_now();
  OK(fl.ok() && cs.encoding == ColumnEncoding::TemporalSeconds, "after a checkpoint the run-length date column is TemporalSeconds (encoding %d, %zu bytes)",
     (int)cs.encoding, cs.estimate_memory_bytes());
  bad = 0;
  for (size_t i = 0; i < N; ++i) if (cs.get(i) != model[i]) ++bad;
  OK(bad == 0, "and every value still reads back as written (%zu wrong)", bad);

  // Comparisons against the seconds column answer what the text did: a datetime bound, a bare date (its midnight),
  // a range, equality, and a bound the fast path does not take (a time with a fraction).
  const std::string lo = model[N / 3], hi = model[N / 2];
  const std::string day = lo.substr(0, 10);
  auto count = [&](const std::string& where) -> long long {
    auto res = db.execute_local_query("SELECT COUNT(*) FROM o WHERE " + where);
    if (!res.success || res.rows.empty() || res.rows[0].values.empty()) return -1;
    return std::stoll(res.rows[0].values[0]);
  };
  size_t m_ge = 0, m_day = 0, m_between = 0, m_eq = 0, m_lt_frac = 0;
  for (size_t i = 0; i < N; ++i) {
    if (model[i] >= lo) ++m_ge;
    if (model[i] >= day + " 00:00:00") ++m_day;
    if (model[i] >= lo && model[i] <= hi) ++m_between;
    if (model[i] == lo) ++m_eq;
    if (model[i] < lo + ".5") ++m_lt_frac;
  }
  const long long c_ge = count("date_created >= '" + lo + "'"), c_day = count("date_created >= '" + day + "'"),
                  c_between = count("date_created BETWEEN '" + lo + "' AND '" + hi + "'"), c_eq = count("date_created = '" + lo + "'"),
                  c_lt_frac = count("date_created < '" + lo + ".5'");
  OK(c_ge == (long long)m_ge && c_day == (long long)m_day && c_between == (long long)m_between && c_eq == (long long)m_eq && c_lt_frac == (long long)m_lt_frac,
     ">=, >= a bare date, BETWEEN, =, < a fraction: %lld %lld %lld %lld %lld (model %zu %zu %zu %zu %zu)",
     c_ge, c_day, c_between, c_eq, c_lt_frac, m_ge, m_day, m_between, m_eq, m_lt_frac);
  db.execute_local_query("DROP TABLE o");
  std::printf("%s\n", failures ? "FAILED" : "PASSED");
  return failures ? 1 : 0;
}
