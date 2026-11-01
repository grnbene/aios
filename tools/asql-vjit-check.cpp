// Randomized differential check of the lane programs' native code (src/userland/sql_vjit_x64.inc) against the
// interpreter. With AIOS_SQL_VJIT_CHECK=1 the engine computes every program chunk both ways and aborts on any
// difference (a value or NULL byte, a bail the interpreter accepts, an answer it refuses). This tool sets that
// mode, makes tables of random and extreme values with NULLs, and runs hundreds of random expression trees over
// them - arithmetic, comparisons, three-valued logic, CASE / COALESCE / NULLIF / GREATEST / LEAST, math functions,
// casts, bit operators - in the single-table and the joined shape, as items and inside aggregates. It passes
// when no chunk disagreed and native code actually ran (the engine counts its chunks). A seed from the
// environment (AIOS_VJIT_SEED) repeats a run; the default seed changes with the date so each day's run differs.
#undef NDEBUG
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>
#include <string>
#include <vector>
#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"
#include "../src/userland/sql_engine_shared.hpp"
#include "check-root.hpp"
using namespace aios;
using namespace aios::userland;
namespace {
std::mt19937_64 rng;
size_t rnd(size_t n) { return (size_t)(rng() % n); }
std::string lit() {
  switch (rnd(12)) {
    case 0: return "0";
    case 1: return "1";
    case 2: return "-1";
    case 3: return std::to_string((long long)rnd(1000) - 500);
    case 4: return "9223372036854775807";
    case 5: return "-9223372036854775807";
    case 6: return "2.5e0";
    case 7: return "-0.0e0";
    case 8: return "1e15";
    case 9: return "3.75e0";
    case 10: return "1e300";
    default: return "NULL";
  }
}
std::string col(bool joined) {
  static const char* cols[] = {"a", "b", "c", "d", "e", "u", "z"};
  const size_t k = rnd(joined ? 7 : 6);
  if (k == 6) return "o.z";
  return std::string("t.") + cols[k];
}
// text: a dictionary-coded column or a literal, compared and passed through CASE / COALESCE / NULLIF
std::string text() {
  static const char* lits[] = {"'a'", "'B'", "'ab'", "'A '", "''", "'z'", "NULL"};
  return rnd(2) ? "t.s" : lits[rnd(7)];
}
std::string text_pred() {
  static const char* c[] = {"=", "<>", "<", "<=", ">", ">="};
  return "(" + text() + " " + c[rnd(6)] + " " + text() + ")";
}
std::string gen(int depth, bool joined) {
  if (depth <= 0 || rnd(4) == 0) return rnd(3) ? col(joined) : lit();
  auto g = [&] { return gen(depth - 1, joined); };
  switch (rnd(28)) {
    case 0: return "(" + g() + " + " + g() + ")";
    case 1: return "(" + g() + " - " + g() + ")";
    case 2: return "(" + g() + " * " + g() + ")";
    case 3: return "(" + g() + " / " + g() + ")";
    case 4: return "(" + g() + " DIV " + g() + ")";
    case 5: return "(" + g() + " % " + g() + ")";
    case 6: return "(-" + g() + ")";
    case 7: { static const char* c[] = {"=", "<>", "<", "<=", ">", ">="}; return "(" + g() + " " + c[rnd(6)] + " " + g() + ")"; }
    case 8: return "(" + g() + " AND " + g() + ")";
    case 9: return "(" + g() + " OR " + g() + ")";
    case 10: return "(NOT " + g() + ")";
    case 11: return "(" + g() + (rnd(2) ? " IS NULL)" : " IS NOT NULL)");
    case 12: return "(CASE WHEN " + g() + " THEN " + g() + (rnd(2) ? " ELSE " + g() : std::string()) + " END)";
    case 13: return "COALESCE(" + g() + ", " + g() + (rnd(2) ? ", " + g() : std::string()) + ")";
    case 14: return "NULLIF(" + g() + ", " + g() + ")";
    case 15: return (rnd(2) ? "GREATEST(" : "LEAST(") + g() + ", " + g() + (rnd(2) ? ", " + g() : std::string()) + ")";
    case 16: { static const char* f[] = {"ABS", "FLOOR", "CEIL", "SQRT", "SIN", "COS", "TAN", "ATAN", "RADIANS", "DEGREES"}; return std::string(f[rnd(10)]) + "(" + g() + ")"; }
    case 17: return "CAST(" + g() + " AS SIGNED)";
    case 18: return "CAST(" + g() + " AS DOUBLE)";
    case 19: return "CAST(" + g() + " AS UNSIGNED)";
    case 20: { static const char* b[] = {"&", "|", "^"}; return "(" + g() + " " + b[rnd(3)] + " " + g() + ")"; }
    case 21: return "(" + g() + " << " + g() + ")";
    case 22: return "(" + g() + " >> " + g() + ")";
    case 23: return "(~" + g() + ")";
    case 24: return "IF(" + g() + ", " + g() + ", " + g() + ")";
    case 25: return rnd(2) ? text_pred() : "(" + text() + (rnd(2) ? " IS NULL)" : " IS NOT NULL)");
    case 26: return "POW(" + g() + ", 2)";
    default: return "(" + g() + " + 1)";
  }
}
}  // namespace

int main() {
  setenv("AIOS_SQL_VJIT_CHECK", "1", 1);   // before the engine compiles any program
  const char* seed_env = std::getenv("AIOS_VJIT_SEED");
  const uint64_t seed = seed_env ? std::strtoull(seed_env, nullptr, 10) : (uint64_t)(std::time(nullptr) / 86400);
  rng.seed(seed);
  std::printf("=== ASQL native lane code check (seed %llu) ===\n", (unsigned long long)seed);
  const CheckRoot check_root("/tmp/aios-asql-vjitcheck");
  KernelConfig cfg;
  cfg.root = check_root.path;
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::printf("boot failed\n"); return 1; }
  SqlDatabase db(**k);
  db.init("sql_db");
  auto run = [&](const std::string& sql) {
    SqlQueryResult r = db.execute_local_query(sql);
    if (!r.success) { std::printf("  setup failed: %s\n  %s\n", sql.c_str(), r.error_message.c_str()); std::exit(1); }
    return r;
  };
  run("CREATE DATABASE IF NOT EXISTS vj");
  run("USE vj");
  run("CREATE TABLE t (id INT PRIMARY KEY, a BIGINT, b BIGINT, c INT, d DOUBLE, e DOUBLE, u BIGINT UNSIGNED, s VARCHAR(4))");
  run("CREATE TABLE one (z INT)");
  run("INSERT INTO one VALUES (3)");
  // random values with the extremes and NULLs: a wide, b small, c tiny, d any double, e near-integers, u unsigned
  std::string batch;
  const size_t rows = 3000;
  for (size_t i = 0; i < rows; ++i) {
    auto maybe_null = [&](const std::string& v) { return rnd(9) == 0 ? std::string("NULL") : v; };
    std::string a = maybe_null(rnd(20) == 0 ? (rnd(2) ? "9223372036854775807" : "-9223372036854775808") : std::to_string((long long)(rng() % 2000001) - 1000000));
    std::string b = maybe_null(std::to_string((long long)rnd(201) - 100));
    std::string c = maybe_null(std::to_string((long long)rnd(7) - 3));
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.17g", rnd(15) == 0 ? (rnd(2) ? 1e300 : -1e300) : ((double)(long long)(rng() % 2000001) - 1000000.0) / (rnd(2) ? 1.0 : 7.0));
    std::string d = maybe_null(buf);
    std::snprintf(buf, sizeof buf, "%.17g", (double)((long long)rnd(2001) - 1000) + (rnd(3) == 0 ? 0.5 : rnd(3) == 0 ? 0.4999999999 : 0.0));
    std::string e = maybe_null(buf);
    std::string u = maybe_null(rnd(10) == 0 ? "18446744073709551615" : std::to_string(rnd(100000)));
    static const char* texts[] = {"'a'", "'A'", "'b'", "'ab'", "'B '", "''", "'zz'", "'Ab'"};
    std::string sv = maybe_null(texts[rnd(8)]);
    batch += (batch.empty() ? "" : ",") + std::string("(") + std::to_string(i) + "," + a + "," + b + "," + c + "," + d + "," + e + "," + u + "," + sv + ")";
    if (batch.size() > 60000 || i + 1 == rows) { run("INSERT INTO t VALUES " + batch); batch.clear(); }
  }
  const uint64_t before = sqle::vjit_native_runs();
  size_t ok = 0, refused = 0;
  const size_t statements = 600;
  for (size_t s = 0; s < statements; ++s) {
    const bool joined = rnd(2) == 0;
    const std::string e = gen(3 + (int)rnd(3), joined);
    const std::string from = joined ? " FROM t CROSS JOIN one o" : " FROM t";
    std::string sql;
    switch (rnd(5)) {
      case 0: sql = "SELECT " + e + " AS v" + from + " ORDER BY t.id LIMIT 500"; break;
      case 4: sql = "SELECT " + (rnd(3) == 0 ? "COALESCE(" + text() + ", " + text() + ")" : rnd(2) ? "NULLIF(" + text() + ", " + text() + ")"
                               : "CASE WHEN " + e + " THEN " + text() + " WHEN " + text_pred() + " THEN " + text() + " ELSE " + text() + " END") +
                    " AS v" + from + " ORDER BY t.id LIMIT 500"; break;
      case 1: sql = "SELECT COUNT(*), SUM(" + e + "), MIN(" + e + "), MAX(" + e + ")" + from; break;
      case 2: sql = "SELECT t.id" + from + " WHERE " + e + " ORDER BY t.id LIMIT 500"; break;
      default: sql = "SELECT t.c, COUNT(*), MAX(" + e + ")" + from + " GROUP BY t.c"; break;
    }
    SqlQueryResult r = db.execute_local_query(sql);
    // an overflow or a DOUBLE out of range is a legitimate refusal of a random expression; anything else is a bug
    if (r.success) ++ok;
    else if (r.error_message.find("out of range") != std::string::npos || r.error_message.find("exact decimal") != std::string::npos) ++refused;
    else { std::printf("  [FAIL] %s\n  %s\n", sql.c_str(), r.error_message.c_str()); return 1; }
  }
  const uint64_t native = sqle::vjit_native_runs() - before;
  std::printf("%zu statements: %zu answered, %zu refused as out of range; %llu chunks computed natively and matched the interpreter\n",
              statements, ok, refused, (unsigned long long)native);
#if defined(__x86_64__) && !defined(_WIN32) && !defined(AIOS_METAL)
  if (native == 0) { std::printf("  [FAIL] no native code ran: the check exercised nothing\n"); return 1; }
#endif
  std::printf("PASSED\n");
  return 0;
}
