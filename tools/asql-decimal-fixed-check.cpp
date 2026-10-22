// DECIMAL(p,s) in DecimalScaled: every value prints byte for byte as it did
// from the string dictionary (trailing zeros kept), memory drops, and a column
// the encoding cannot hold exactly stays as it was.

#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <string>
#include <vector>
#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"

using namespace aios;
using namespace aios::userland;

static int g_fail = 0;
static void expect(bool ok, const char* what) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_fail;
}

static std::vector<std::string> dump(SqlDatabase& db, const std::string& sql) {
  std::vector<std::string> out;
  auto res = db.execute_local_query(sql);
  for (const auto& row : res.rows) {
    std::string line;
    for (const auto& v : row.values) { line += v; line += '|'; }
    out.push_back(line);
  }
  return out;
}

// A DECIMAL(15,8) column whose scaled values need all 32 bits (30.00000000 is
// 3e9 scaled), saved and read back by a fresh engine: the dictionary-to-scaled
// re-encode used to clear the 32-bit codes it had just written, and every
// weight came back 0.00000000.
static void full_width_survives_reload() {
  char root[] = "/tmp/aios-asql-decfixed-reload-XXXXXX";
  if (!mkdtemp(root)) { expect(false, "temp root for the reload"); return; }
  const std::vector<std::string> want = {"1|1.00000000|", "2|30.00000000|", "3|0.00000000|", "4|21.47483648|", "5|40.00000000|"};
  {
    KernelConfig cfg; cfg.root = root;
    auto k = Kernel::boot(cfg);
    if (!k.ok()) { expect(false, "boot for the reload"); return; }
    SqlDatabase db(**k);
    db.init("sql_db");
    db.execute_local_query("CREATE TABLE weights (id INT PRIMARY KEY, w DECIMAL(15,8) NOT NULL);");
    db.execute_local_query("INSERT INTO weights VALUES (1, 1.0), (2, 30.0), (3, 0.0), (4, 21.47483648), (5, 40.0);");
    expect(dump(db, "SELECT id, w FROM weights ORDER BY id;") == want, "DECIMAL(15,8) before the save");
    db.execute_local_query("FLUSH TABLES;");
  }
  KernelConfig cfg; cfg.root = root;
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { expect(false, "boot after the save"); return; }
  SqlDatabase db(**k);
  db.init("sql_db");
  expect(dump(db, "SELECT id, w FROM weights ORDER BY id;") == want, "DECIMAL(15,8) at full 32-bit width reads back after a reload");
}

int main() {
  full_width_survives_reload();

  KernelConfig cfg;
  cfg.root = "/tmp/aios-asql-decfixed-test";
  auto k_res = Kernel::boot(cfg);
  if (!k_res.ok()) { std::printf("Failed to boot kernel\n"); return 1; }
  SqlDatabase db(**k_res);
  db.init("sql_db");

  db.execute_local_query("DROP TABLE IF EXISTS decfix;");
  db.execute_local_query("CREATE TABLE decfix (id INT NOT NULL, price DECIMAL(10,2), qty DECIMAL(12,3), huge DECIMAL(18,2), note TEXT);");
  const size_t N = 200000;
  std::string batch;
  for (size_t i = 1; i <= N; ++i) {
    char row[256];
    const long cents = (long)((i * 7919) % 500000);            // 0.00 .. 4999.99, many ending in 0
    const long mills = (long)((i * 104729) % 90000);           // 0.000 .. 89.999
    const bool null_price = i % 97 == 0;
    char price[32];
    if (null_price) std::snprintf(price, sizeof(price), "NULL");
    else std::snprintf(price, sizeof(price), "%s%ld.%02ld", i % 11 == 0 ? "-" : "", cents / 100, cents % 100);
    std::snprintf(row, sizeof(row), "%s(%zu, %s, %ld.%03ld, %s, 'n%zu')",
                  batch.empty() ? "" : ",", i, price, mills / 1000, mills % 1000,
                  i % 5 == 0 ? "99999999999999.99" : "1.50", i % 100);
    batch += row;
    if (batch.size() > 60000 || i == N) {
      db.execute_local_query("INSERT INTO decfix VALUES " + batch + ";");
      batch.clear();
    }
  }
  db.execute_local_query("INSERT INTO decfix VALUES (900001, 0.00, 0.000, 0.00, 'z'), (900002, 100.10, 5.500, 7.00, 'z');");

  SqlTable* tbl = db.resolve_table("decfix", "aios_db");
  if (!tbl) { std::printf("no table\n"); return 1; }
  const size_t rows = tbl->row_count();

  const char* queries[] = {
    "SELECT id, price, qty, huge FROM decfix ORDER BY id;",
    "SELECT price, COUNT(*), SUM(qty), MIN(price), MAX(price) FROM decfix GROUP BY price ORDER BY price;",
    "SELECT SUM(price), SUM(qty), AVG(price), MIN(qty), MAX(qty) FROM decfix;",
    "SELECT id, price FROM decfix WHERE price > 1000.50 AND qty < 10.000 ORDER BY price DESC, id LIMIT 500;",
    "SELECT id, price FROM decfix WHERE price = 100.10 OR price = 0.00 ORDER BY id;",
    "SELECT CONCAT(price, '/', qty), price + qty, price * 2 FROM decfix WHERE id % 1000 = 0 ORDER BY id;",
  };
  std::vector<std::vector<std::string>> before;
  for (const char* q : queries) before.push_back(dump(db, q));

  size_t mem_before = tbl->col_data[1].estimate_memory_bytes() + tbl->col_data[2].estimate_memory_bytes();
  std::printf("rows=%zu  price+qty memory before: %zu bytes (%.2f bytes/row)\n", rows, mem_before, (double)mem_before / rows);

  for (size_t c = 0; c < tbl->col_data.size(); ++c)
    tbl->col_data[c].optimize_encoding(tbl->columns[c].type, tbl->columns[c].scale);

  size_t mem_after = tbl->col_data[1].estimate_memory_bytes() + tbl->col_data[2].estimate_memory_bytes();
  std::printf("price+qty memory after:  %zu bytes (%.2f bytes/row)\n", mem_after, (double)mem_after / rows);
  expect(tbl->col_data[1].encoding == ColumnEncoding::DecimalScaled && tbl->col_data[1].decimal_fixed(), "price is DecimalScaled (fixed places)");
  expect(tbl->col_data[2].encoding == ColumnEncoding::DecimalScaled && tbl->col_data[2].decimal_fixed(), "qty is DecimalScaled (fixed places)");
  expect(tbl->col_data[3].encoding != ColumnEncoding::DecimalScaled, "huge (span > 32 bits) stays on the dictionary");
  expect(mem_after < mem_before, "memory dropped");

  for (size_t i = 0; i < before.size(); ++i) {
    expect(dump(db, queries[i]) == before[i], queries[i]);
  }

  // Trailing zeros, negatives and zero, straight from get().
  bool zeros = false;
  for (size_t r = 0; r < tbl->col_data[1].size(); ++r) {
    if (tbl->col_data[1].is_null(r)) continue;
    const std::string& s = tbl->col_data[1].get(r);
    if (s.size() >= 3 && s[s.size() - 3] != '.') { zeros = false; break; }
    if (s.back() == '0') zeros = true;
  }
  expect(zeros, "price prints every value with two places, trailing zeros kept");

  // Writes after the encoding: a fitting value stays packed, others fall back safely.
  db.execute_local_query("INSERT INTO decfix VALUES (900003, 12.50, 3.100, 1.00, 'w');");
  db.execute_local_query("UPDATE decfix SET price = 7.70 WHERE id = 900003;");
  auto got = dump(db, "SELECT price, qty FROM decfix WHERE id = 900003;");
  expect(got.size() == 1 && got[0] == "7.70|3.100|", "insert + update keep fixed places");
  db.execute_local_query("INSERT INTO decfix VALUES (900004, 99999999.99, 1.000, 1.00, 'w');");
  got = dump(db, "SELECT price FROM decfix WHERE id = 900004;");
  expect(got.size() == 1 && got[0] == "99999999.99|", "value outside the 32-bit span falls back and reads back");
  got = dump(db, "SELECT price FROM decfix WHERE id = 900002;");
  expect(got.size() == 1 && got[0] == "100.10|", "earlier rows unchanged after the fallback");

  std::printf(g_fail ? "=== FAILED (%d) ===\n" : "=== DECIMAL FIXED-SCALE CHECK PASSED ===\n", g_fail);
  return g_fail ? 1 : 0;
}
