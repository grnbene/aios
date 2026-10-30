// A FOR integer column that grows by appends (sequential ids: how st's stockmovestaxes.stkmoveno came to be 32-bit
// FOR, 4 bytes a row) is re-laid per segment (SegmentFOR) by a checkpoint's maintenance once that is at most 60% of it
// (ColumnStore::for_to_segments). Every value reads back the same - a column with NULLs too - before and after, after
// more appends and an UPDATE, and after the store is reopened from disk.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <unistd.h>

#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"
#include "aios/userland/task_ledger.hpp"

using namespace aios;
using namespace aios::userland;

static int failures = 0;
#define OK(c, ...) do { if (!(c)) { std::printf("  [FAIL] "); ++failures; } else std::printf("  [ok] "); std::printf(__VA_ARGS__); std::printf("\n"); } while (0)

struct Col { std::string encoding; double bytes = 0; };
static Col storage(SqlDatabase& db, const std::string& table, const std::string& column) {
  auto r = db.execute_gateway_query("SHOW ASQL STORAGE");
  for (const auto& row : r.rows)
    if (row.values.size() >= 8 && row.values[0].find(table) != std::string::npos && row.values[1] == column)
      return Col{row.values[3], std::atof(row.values[6].c_str())};
  return Col{};
}
// Runs a statement whatever the build (assert() is gone under NDEBUG, and with it the statement).
static void run(SqlDatabase& db, const std::string& sql) {
  auto r = db.execute_gateway_query(sql);
  if (!r.success) { std::printf("  [FAIL] %s: %s\n", sql.substr(0, 60).c_str(), r.error_message.c_str()); std::exit(1); }
}
static std::vector<std::string> all(SqlDatabase& db, const char* sql) {
  std::vector<std::string> out;
  auto r = db.execute_gateway_query(sql);
  for (const auto& row : r.rows) { std::string line; for (const auto& v : row.values) line += v + "|"; out.push_back(line); }
  return out;
}

int main() {
  char root[] = "/tmp/aios-for-segments-XXXXXX";
  if (!mkdtemp(root)) return 1;
  std::vector<std::string> before, after_conv;
  {
    KernelConfig cfg; cfg.root = root;
    auto k = Kernel::boot(cfg);
    TaskLedger ledger(**k); ledger.init("tasks");
    SqlDatabase db(**k, &ledger); db.init("sql_db");
    run(db, "CREATE TABLE moves (id INT PRIMARY KEY, ref INT, qty INT)");
    // A small table first: its first encoding is FOR (one segment: per segment would not pay).
    std::string ins = "INSERT INTO moves VALUES ";
    for (int i = 1; i <= 1000; ++i) ins += (i > 1 ? "," : "") + std::string("(") + std::to_string(i) + "," + (i % 7 ? std::to_string(5000000 + i * 2) : "NULL") + "," + std::to_string(i % 50) + ")";
    run(db, ins);
    run(db, "FLUSH TABLES");
    const Col c0 = storage(db, "moves", "id"), r0 = storage(db, "moves", "ref");
    OK(c0.encoding == "FOR" && r0.encoding == "FOR", "1,000 rows: id %s, ref %s", c0.encoding.c_str(), r0.encoding.c_str());
    // Then appends, as replication brings them: 200k sequential ids, and a ref column of slowly rising numbers with
    // NULLs; one far-off id widens FOR to 32 bits, as st's has.
    for (int b = 0; b < 200; ++b) {
      std::string s = "INSERT INTO moves VALUES ";
      for (int i = 0; i < 1000; ++i) {
        const int id = 1001 + b * 1000 + i;
        s += (i ? "," : "") + std::string("(") + std::to_string(id) + "," + (id % 7 ? std::to_string(5000000 + id * 2) : "NULL") + "," + std::to_string(id % 50) + ")";
      }
      run(db, s);
    }
    run(db, "INSERT INTO moves VALUES (2000000000, 9, 9)");
    // Not yet maintained: the appends keep FOR.
    const Col c1 = storage(db, "moves", "id");
    before = all(db, "SELECT id, ref, qty FROM moves");
    const auto sums = all(db, "SELECT COUNT(*), SUM(id), MIN(id), MAX(id), COUNT(ref), SUM(ref), MIN(ref), MAX(ref) FROM moves");
    const auto ranges = all(db, "SELECT COUNT(*) FROM moves WHERE id BETWEEN 50000 AND 60000 AND ref > 5110000");
    run(db, "FLUSH TABLES");   // a checkpoint: maintenance runs
    const Col c2 = storage(db, "moves", "id"), r2 = storage(db, "moves", "ref");
    OK(c1.encoding == "FOR" && c2.encoding == "SegFOR", "after the appends, a checkpoint: id %s -> %s", c1.encoding.c_str(), c2.encoding.c_str());
    OK(c2.bytes * 10 <= c1.bytes * 6, "id %.0f -> %.0f bytes (%.2f -> %.2f a row)", c1.bytes, c2.bytes, c1.bytes / 201001, c2.bytes / 201001);
    OK(r2.encoding == "SegFOR", "the ref column with NULLs too: %s", r2.encoding.c_str());
    after_conv = all(db, "SELECT id, ref, qty FROM moves");
    OK(after_conv == before, "all %zu rows read back the same", before.size());
    OK(all(db, "SELECT COUNT(*), SUM(id), MIN(id), MAX(id), COUNT(ref), SUM(ref), MIN(ref), MAX(ref) FROM moves") == sums, "sums, counts, NULL counts, min and max the same");
    OK(all(db, "SELECT COUNT(*) FROM moves WHERE id BETWEEN 50000 AND 60000 AND ref > 5110000") == ranges, "a range filter the same");
    auto p = db.execute_gateway_query("SELECT ref FROM moves WHERE id = 77778");
    OK(p.success && p.rows.size() == 1 && p.rows[0].values[0] == std::to_string(5000000 + 77778 * 2), "a point lookup");
    // Writes after: appends, an UPDATE, a DELETE.
    run(db, "INSERT INTO moves VALUES (2000000001, 1, 1), (2000000002, NULL, 2)");
    run(db, "UPDATE moves SET ref = 42 WHERE id = 100000");
    run(db, "DELETE FROM moves WHERE id = 150000");
    auto u = db.execute_gateway_query("SELECT ref FROM moves WHERE id = 100000");
    auto n = db.execute_gateway_query("SELECT COUNT(*) FROM moves");
    OK(u.rows.size() == 1 && u.rows[0].values[0] == "42" && n.rows[0].values[0] == "201002", "appends, an UPDATE and a DELETE after it");
    before = all(db, "SELECT id, ref, qty FROM moves");
    run(db, "FLUSH TABLES");
  }
  {
    // Reopened from disk.
    KernelConfig cfg; cfg.root = root;
    auto k = Kernel::boot(cfg);
    TaskLedger ledger(**k); ledger.init("tasks");
    SqlDatabase db(**k, &ledger); db.init("sql_db");
    OK(all(db, "SELECT id, ref, qty FROM moves") == before, "reopened from disk: every row the same");
  }
  std::string rm = std::string("rm -rf ") + root;
  (void)!std::system(rm.c_str());
  std::printf(failures ? "for segments check: %d failure(s)\n" : "for segments check: all passed\n", failures);
  return failures ? 1 : 0;
}
