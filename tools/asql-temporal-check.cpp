// Verifies the temporal column encodings: DATE / DATETIME / TIMESTAMP text packed as
// days / seconds (TemporalDays / TemporalSeconds) reads back byte for byte through every
// path, a written value is stored as MariaDB 10.11 stores it (checked against it), a
// value with no point in time (the zero date) keeps the column off the encoding, and the
// packed column is smaller than the dictionary it replaces.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"

using namespace aios;
using namespace aios::userland;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
  if (!ok) { std::printf("  FAIL: %s\n", what.c_str()); ++g_fail; }
}
const char* enc_name(ColumnEncoding e) {
  switch (e) {
    case ColumnEncoding::TemporalDays: return "TemporalDays";
    case ColumnEncoding::TemporalSeconds: return "TemporalSeconds";
    case ColumnEncoding::DictBitPacked: return "Dict";
    case ColumnEncoding::RLE: return "RLE";
    case ColumnEncoding::Constant: return "Constant";
    default: return "other";
  }
}
// The i-th timestamp of a log that ticks forward a few seconds a row.
std::string stamp(size_t i) {
  const size_t t = 1780000000 + i * 7 + (i * 31) % 5;   // seconds since epoch
  const size_t days = t / 86400, sod = t % 86400;
  // civil from days (1970-based)
  long z = (long)days + 719468;
  const long era = z / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  const long y = (long)yoe + era * 400 + (m <= 2);
  char b[32];
  std::snprintf(b, sizeof(b), "%04ld-%02u-%02u %02zu:%02zu:%02zu", y, m, d, sod / 3600, sod / 60 % 60, sod % 60);
  return b;
}
std::string day_of(size_t i) { return stamp(i * 3000).substr(0, 10); }
}  // namespace

int main() {
  KernelConfig cfg;
  cfg.root = "/tmp/aios-asql-temporal-test";
  auto k_res = Kernel::boot(cfg);
  if (!k_res.ok()) { std::printf("Failed to boot kernel\n"); return 1; }
  SqlDatabase db(**k_res);
  db.init("sql_db");

  const size_t N = 6000;
  db.execute_local_query("DROP TABLE IF EXISTS tlog;");
  db.execute_local_query("CREATE TABLE tlog (id INT NOT NULL, inputdate DATETIME, created_at TIMESTAMP, d DATE, note VARCHAR(10));");
  std::vector<std::string> want_dt(N), want_d(N);
  for (size_t i = 0; i < N; ++i) {
    want_dt[i] = stamp(i);
    want_d[i] = day_of(i);
    db.execute_local_query("INSERT INTO tlog VALUES (" + std::to_string(i) + ", '" + want_dt[i] + "', '" + want_dt[i] + "', '" + want_d[i] + "', 'x');");
  }
  // NULL cells ride along
  db.execute_local_query("UPDATE tlog SET inputdate = NULL, d = NULL WHERE id = 17;");
  want_dt[17] = "NULL"; want_d[17] = "NULL";

  SqlTable* tbl = db.resolve_table("tlog", "aios_db");
  if (!tbl) { std::printf("FAILED: no table\n"); return 1; }
  auto mem = [&](size_t c) { return tbl->col_data[c].estimate_memory_bytes(); };
  const size_t before_dt = mem(1), before_ts = mem(2), before_d = mem(3);
  std::printf("[before] inputdate %zu B (%s), created_at %zu B, d %zu B\n", before_dt, enc_name(tbl->col_data[1].encoding), before_ts, before_d);

  std::printf("[1] optimize_storage -> temporal encodings\n");
  tbl->optimize_storage();
  std::printf("[after]  inputdate %zu B (%s %u-bit), created_at %zu B (%s), d %zu B (%s %u-bit)\n", mem(1),
              enc_name(tbl->col_data[1].encoding), (unsigned)tbl->col_data[1].bit_width_, mem(2), enc_name(tbl->col_data[2].encoding),
              mem(3), enc_name(tbl->col_data[3].encoding), (unsigned)tbl->col_data[3].bit_width_);
  check(tbl->col_data[1].encoding == ColumnEncoding::TemporalSeconds, "inputdate is TemporalSeconds");
  check(tbl->col_data[2].encoding == ColumnEncoding::TemporalSeconds, "created_at is TemporalSeconds");
  check(tbl->col_data[3].encoding == ColumnEncoding::TemporalDays, "d is TemporalDays");
  check(mem(1) < before_dt / 2, "inputdate is less than half its dictionary form: " + std::to_string(mem(1)) + " of " + std::to_string(before_dt));
  check(mem(3) < before_d, "d is smaller");
  check(tbl->col_data[1].dict.size() == 0, "no dictionary left on inputdate");

  std::printf("[2] get() is byte for byte, NULLs stay NULL\n");
  for (size_t i = 0; i < N; ++i) {
    const bool null_dt = i == 17;
    check(tbl->col_data[1].is_null(i) == null_dt, "null flag inputdate row " + std::to_string(i));
    if (!null_dt) {
      check(tbl->col_data[1].get(i) == want_dt[i], "inputdate row " + std::to_string(i) + " got " + tbl->col_data[1].get(i) + " want " + want_dt[i]);
      check(tbl->col_data[3].get(i) == want_d[i], "d row " + std::to_string(i));
    }
    check(tbl->col_data[2].get(i) == (i == 17 ? stamp(17) : want_dt[i]), "created_at row " + std::to_string(i));
  }

  std::printf("[3] SQL readers: scan, range filter, equality, ORDER BY, GROUP BY, MIN/MAX, join\n");
  auto q = [&](const std::string& sql) { return db.execute_local_query(sql); };
  {
    auto r = q("SELECT id, inputdate FROM tlog WHERE id = 5;");
    check(!r.rows.empty() && r.rows[0].values[1] == want_dt[5], "point read");
    r = q("SELECT count(*) FROM tlog WHERE inputdate >= '" + want_dt[1000] + "' AND inputdate < '" + want_dt[2000] + "';");
    size_t cnt = 0;
    for (size_t i = 0; i < N; ++i) if (i != 17 && want_dt[i] >= want_dt[1000] && want_dt[i] < want_dt[2000]) ++cnt;
    check(!r.rows.empty() && r.rows[0].values[0] == std::to_string(cnt), "range count " + (r.rows.empty() ? "" : r.rows[0].values[0]) + " want " + std::to_string(cnt));
    r = q("SELECT id FROM tlog WHERE inputdate = '" + want_dt[4321] + "';");
    check(!r.rows.empty() && r.rows[0].values[0] == "4321", "equality");
    r = q("SELECT id FROM tlog WHERE inputdate = '1999-01-01 00:00:00';");
    check(r.rows.empty(), "equality, no match");
    r = q("SELECT inputdate FROM tlog ORDER BY inputdate DESC LIMIT 3;");
    check(r.rows.size() == 3 && r.rows[0].values[0] == want_dt[N - 1] && r.rows[1].values[0] == want_dt[N - 2], "ORDER BY DESC");
    r = q("SELECT inputdate FROM tlog ORDER BY inputdate ASC LIMIT 2;");
    check(r.rows.size() == 2 && (sql_is_null(r.rows[0].values[0]) || r.rows[0].values[0] == want_dt[0]), "ORDER BY ASC");   // NULLs sort first
    r = q("SELECT min(inputdate), max(inputdate) FROM tlog;");
    check(!r.rows.empty() && r.rows[0].values[0] == want_dt[0] && r.rows[0].values[1] == want_dt[N - 1], "MIN/MAX");
    r = q("SELECT d, count(*) FROM tlog GROUP BY d ORDER BY d LIMIT 1;");
    check(!r.rows.empty(), "GROUP BY runs");
    size_t groups = 0;
    {
      std::vector<std::string> seen;
      for (size_t i = 0; i < N; ++i) if (i != 17) { bool f = false; for (auto& s : seen) if (s == want_d[i]) { f = true; break; } if (!f) seen.push_back(want_d[i]); }
      groups = seen.size();
    }
    r = q("SELECT count(*) FROM (SELECT d FROM tlog WHERE d IS NOT NULL GROUP BY d) g;");
    check(!r.rows.empty() && r.rows[0].values[0] == std::to_string(groups), "GROUP BY distinct dates");
    r = q("SELECT count(*) FROM tlog a JOIN tlog b ON a.inputdate = b.inputdate;");
    check(!r.rows.empty() && r.rows[0].values[0] == std::to_string(N - 1), "self-join on the temporal column");
    r = q("SELECT count(*) FROM tlog WHERE inputdate IS NULL;");
    check(!r.rows.empty() && r.rows[0].values[0] == "1", "IS NULL");
  }

  std::printf("[4] writes keep the encoding or fall back exactly\n");
  q("INSERT INTO tlog VALUES (9001, '2030-01-02 03:04:05', '2030-01-02 03:04:05', '2030-01-02', 'y');");
  check(tbl->col_data[1].encoding == ColumnEncoding::TemporalSeconds, "append of a canonical value stays temporal");
  check(tbl->col_data[1].get(N) == "2030-01-02 03:04:05", "appended value reads back");
  q("INSERT INTO tlog VALUES (9002, '1999-12-31 23:59:59', '1999-12-31 23:59:59', '1999-12-31', 'y');");
  check(tbl->col_data[1].get(N + 1) == "1999-12-31 23:59:59" && tbl->col_data[1].get(0) == want_dt[0], "append below the base rebases, others intact");
  q("UPDATE tlog SET inputdate = '2001-02-03 04:05:06' WHERE id = 100;");
  check(tbl->col_data[1].get(100) == "2001-02-03 04:05:06", "set in place");
  want_dt[100] = "2001-02-03 04:05:06";
  q("UPDATE tlog SET inputdate = '0000-00-00 00:00:00' WHERE id = 101;");
  check(tbl->col_data[1].get(101) == "0000-00-00 00:00:00", "zero date is stored as written");
  check(tbl->col_data[1].get(100) == "2001-02-03 04:05:06" && tbl->col_data[1].get(102) == want_dt[102], "neighbours intact after the fallback");
  want_dt[101] = "0000-00-00 00:00:00";
  q("INSERT INTO tlog VALUES (9003, '2026-06-01 12:34:56.789', '2026-06-01 12:34:56', '2026-06-01', 'z');");
  // a DATETIME without a precision holds whole seconds: MariaDB cuts the fraction (it does not round)
  check(tbl->col_data[1].get(N + 2) == "2026-06-01 12:34:56", "fractional seconds cut to the column's precision, got " + tbl->col_data[1].get(N + 2));
  q("DELETE FROM tlog WHERE id IN (3, 4, 2500);");
  check(tbl->col_data[3].get(3) == want_d[5] || tbl->col_data[3].encoding != ColumnEncoding::TemporalDays, "erase_rows keeps the rest");
  {
    auto r = q("SELECT count(*) FROM tlog;");
    check(!r.rows.empty() && r.rows[0].values[0] == std::to_string(N + 3 - 3), "row count after DELETE");
    r = q("SELECT inputdate FROM tlog WHERE id = 5;");
    check(!r.rows.empty() && r.rows[0].values[0] == want_dt[5], "row after the deleted ones");
    r = q("SELECT d FROM tlog WHERE id = 2501;");
    check(!r.rows.empty() && r.rows[0].values[0] == want_d[2501], "row after a mid-table delete");
  }

  std::printf("[5] a written value is stored as MariaDB stores it; the zero date keeps the column off the encoding\n");
  // What MariaDB 10.11 stores for each into a DATETIME, sql_mode '' (the mirror's): the zero date and text that is
  // no date at all ('', February 30) are the zero date-time, with a warning; the rest is normalised.
  struct Odd { const char* in; const char* stored; };
  const Odd odd[] = {{"0000-00-00", "0000-00-00 00:00:00"}, {"2026-02-30", "0000-00-00 00:00:00"}, {"2026-6-1", "2026-06-01 00:00:00"},
                     {"", "0000-00-00 00:00:00"}, {"2026-06-01T12:00:00", "2026-06-01 12:00:00"},
                     {"2026-06-01 12:00:00.5", "2026-06-01 12:00:00"}, {"2026-06-01", "2026-06-01 00:00:00"}};
  for (const Odd& o : odd) {
    db.execute_local_query("DROP TABLE IF EXISTS todd;");
    db.execute_local_query("CREATE TABLE todd (id INT, d DATETIME);");
    for (size_t i = 0; i < 700; ++i) db.execute_local_query("INSERT INTO todd VALUES (" + std::to_string(i) + ", '" + stamp(i) + "');");
    db.execute_local_query(std::string("INSERT INTO todd VALUES (9999, '") + o.in + "');");
    SqlTable* t2 = db.resolve_table("todd", "aios_db");
    t2->optimize_storage();
    const ColumnStore& cs = t2->col_data[1];
    const bool zero = std::string(o.stored).rfind("0000-00-00", 0) == 0;
    if (zero) check(cs.encoding != ColumnEncoding::TemporalSeconds && cs.encoding != ColumnEncoding::TemporalDays, std::string("'") + o.in + "' (the zero date) keeps the column off the encoding");
    check(cs.get(700) == o.stored, std::string("'") + o.in + "' is stored as '" + o.stored + "', got '" + cs.get(700) + "'");
    check(cs.get(699) == stamp(699), "neighbour of the odd value");
  }

  std::printf("[6] extreme canonical values\n");
  {
    db.execute_local_query("DROP TABLE IF EXISTS text1;");
    db.execute_local_query("CREATE TABLE text1 (id INT, d DATE);");
    const char* ds[] = {"0001-01-01", "1969-12-31", "1970-01-01", "2000-02-29", "2100-02-28", "9999-12-31"};
    for (size_t i = 0; i < 400; ++i) db.execute_local_query("INSERT INTO text1 VALUES (" + std::to_string(i) + ", '" + day_of(i) + "');");
    for (size_t i = 0; i < 6; ++i) db.execute_local_query(std::string("INSERT INTO text1 VALUES (") + std::to_string(1000 + i) + ", '" + ds[i] + "');");
    SqlTable* t4 = db.resolve_table("text1", "aios_db");
    t4->optimize_storage();
    for (size_t i = 0; i < 6; ++i) check(t4->col_data[1].get(400 + i) == ds[i], std::string("extreme ") + ds[i] + ", got '" + t4->col_data[1].get(400 + i) + "'");
    std::printf("  span 0001..9999 -> %s\n", enc_name(t4->col_data[1].encoding));
    // narrow the span: leap days and pre-epoch dates inside 32 bits of days
    db.execute_local_query("DROP TABLE IF EXISTS text2;");
    db.execute_local_query("CREATE TABLE text2 (id INT, t DATETIME);");
    const char* ts[] = {"1969-12-31 23:59:59", "1970-01-01 00:00:00", "2000-02-29 12:00:00", "2038-01-19 03:14:08", "2106-02-07 06:28:14"};   // (:15 is 2^32 - 1 seconds after 1970; with the pre-epoch -1 the span would be 2^32, one past 32 bits)
    for (size_t i = 0; i < 400; ++i) db.execute_local_query("INSERT INTO text2 VALUES (" + std::to_string(i) + ", '" + stamp(i) + "');");
    for (size_t i = 0; i < 5; ++i) db.execute_local_query(std::string("INSERT INTO text2 VALUES (") + std::to_string(1000 + i) + ", '" + ts[i] + "');");
    SqlTable* t5 = db.resolve_table("text2", "aios_db");
    t5->optimize_storage();
    std::printf("  pre-epoch .. 2106 -> %s\n", enc_name(t5->col_data[1].encoding));
    check(t5->col_data[1].encoding == ColumnEncoding::TemporalSeconds, "pre-epoch to 2106 fits");
    for (size_t i = 0; i < 5; ++i) check(t5->col_data[1].get(400 + i) == ts[i], std::string("extreme ") + ts[i]);
  }

  std::printf("[7] checkpoint save / reload\n");
  {
    db.execute_local_query("DROP TABLE IF EXISTS tsave;");
    db.execute_local_query("CREATE TABLE tsave (id INT, t DATETIME);");
    for (size_t i = 0; i < 3000; ++i) db.execute_local_query("INSERT INTO tsave VALUES (" + std::to_string(i) + ", '" + stamp(i) + "');");
    (void)db.flush_now();
    SqlTable* t6 = db.resolve_table("tsave", "aios_db");
    std::printf("  after checkpoint: %s\n", enc_name(t6->col_data[1].encoding));
    check(t6->col_data[1].encoding == ColumnEncoding::TemporalSeconds, "checkpoint promotes an INSERT-built column");
    for (size_t i = 0; i < 3000; ++i) if (t6->col_data[1].get(i) != stamp(i)) { check(false, "checkpointed row " + std::to_string(i)); break; }
  }

  if (g_fail) { std::printf("=== %d CHECK(S) FAILED ===\n", g_fail); return 1; }
  std::printf("=== ALL TEMPORAL ENCODING CHECKS PASSED ===\n");
  return 0;
}
