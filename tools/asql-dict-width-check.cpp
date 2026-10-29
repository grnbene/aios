// A write that an encoded column cannot take in place must not turn the whole column into 32-bit dictionary codes.
//   - an UPDATE of one cell in a run-length column: the column becomes a dictionary at the width its entries need;
//   - a DELETE of one row from a dictionary column keeps a byte-rounded width (8 or 16), never 32;
//   - a DELETE of one row from an integer (FOR) column: the same;
//   - INSERTs of new values into a narrow dictionary widen it a bit at a time.
// Every row is checked against a model after each statement, so the narrow forms answer what the wide one did.
// Before the fix every one of these left the column at 32 bits a code (4 B a row for a dictionary of five values).
#include <cstdio>
#include <string>
#include <vector>

#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"

using namespace aios;
using namespace aios::userland;

static int failures = 0;
#define OK(c, ...) do { if (!(c)) { std::printf("  [FAIL] "); ++failures; } else std::printf("  [ok] "); std::printf(__VA_ARGS__); std::printf("\n"); } while (0)

static const char* enc_name(ColumnEncoding e) {
  switch (e) {
    case ColumnEncoding::DictBitPacked: return "Dict";
    case ColumnEncoding::RLE: return "RLE";
    case ColumnEncoding::FOR: return "FOR";
    case ColumnEncoding::Constant: return "Constant";
    default: return "other";
  }
}

// Every row of column `c` equals the model.
static bool rows_match(const SqlTable& t, size_t c, const std::vector<std::string>& model, const char* what) {
  const ColumnStore& cs = t.col_data[c];
  if (cs.size() != model.size()) { std::printf("  [FAIL] %s: %zu rows, model %zu\n", what, cs.size(), model.size()); ++failures; return false; }
  for (size_t r = 0; r < model.size(); ++r) {
    const std::string got = cs.is_null(r) ? std::string("NULL") : cs.get(r);
    if (got != model[r]) { std::printf("  [FAIL] %s: row %zu is '%s', model '%s'\n", what, r, got.c_str(), model[r].c_str()); ++failures; return false; }
  }
  return true;
}

int main() {
  KernelConfig cfg;
  cfg.root = "/tmp/aios-asql-dict-width-test";
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::printf("kernel boot failed\n"); return 1; }
  SqlDatabase db(**k);
  db.init("sql_db");

  const size_t N = 20000;
  db.execute_local_query("DROP TABLE IF EXISTS t");
  db.execute_local_query("CREATE TABLE t (id INT NOT NULL, state VARCHAR(10), n INT, PRIMARY KEY(id))");
  std::vector<std::string> state(N), n(N);
  static const char* kStates[] = {"NEW", "PICKED", "LOADED", "SHIPPED", "DONE"};
  {
    std::string sql;
    for (size_t i = 0; i < N; ++i) {
      state[i] = kStates[(i / 2500) % 5];   // long runs: RLE territory
      n[i] = std::to_string(1000 + i);      // sequential: FOR territory
      if (sql.empty()) sql = "INSERT INTO t VALUES ";
      else sql += ",";
      sql += "(" + std::to_string(i) + ",'" + state[i] + "'," + n[i] + ")";
      if ((i + 1) % 2000 == 0) { db.execute_local_query(sql); sql.clear(); }
    }
    if (!sql.empty()) db.execute_local_query(sql);
  }
  SqlTable* t = db.resolve_table("t", "aios_db");
  if (!t || t->row_count() != N) { std::printf("table not loaded (%zu rows)\n", t ? t->row_count() : 0); return 1; }
  const int c_state = t->get_column_index("state"), c_n = t->get_column_index("n");
  for (size_t c = 0; c < t->col_data.size(); ++c)
    t->col_data[c].optimize_encoding(t->columns[c].type, t->columns[c].scale, t->columns[c].temporal);
  ColumnStore& cs = t->col_data[(size_t)c_state];
  ColumnStore& cn = t->col_data[(size_t)c_n];
  std::printf("encoded: state %s (%u bits), n %s (%u bits)\n", enc_name(cs.encoding), (unsigned)cs.bit_width_, enc_name(cn.encoding), (unsigned)cn.bit_width_);
  if (cs.encoding != ColumnEncoding::RLE || cn.encoding != ColumnEncoding::FOR) { std::printf("precondition: expected RLE and FOR\n"); return 1; }

  // 1. One cell of the run-length column rewritten.
  db.execute_local_query("UPDATE t SET state='HELD' WHERE id=100");
  state[100] = "HELD";
  rows_match(*t, (size_t)c_state, state, "after UPDATE");
  OK(cs.encoding == ColumnEncoding::DictBitPacked && cs.bit_width_ <= 8,
     "UPDATE of a run-length cell: state is %s at %u bits (6 entries need 3; 32 is the whole column widened)", enc_name(cs.encoding), (unsigned)cs.bit_width_);

  // 2. One row deleted: the dictionary column and the integer column.
  db.execute_local_query("DELETE FROM t WHERE id=200");
  state.erase(state.begin() + 200);
  n.erase(n.begin() + 200);
  rows_match(*t, (size_t)c_state, state, "state after DELETE");
  rows_match(*t, (size_t)c_n, n, "n after DELETE");
  OK(cs.encoding == ColumnEncoding::DictBitPacked && cs.bit_width_ <= 8,
     "DELETE of one row: state is %s at %u bits", enc_name(cs.encoding), (unsigned)cs.bit_width_);
  OK(cn.bit_width_ <= 16,
     "DELETE of one row: n is %s at %u bits (20k values need 15)", enc_name(cn.encoding), (unsigned)cn.bit_width_);

  // 3. New values appended to the narrow dictionary: a bit at a time.
  for (int i = 0; i < 12; ++i) {
    const std::string v = "X" + std::to_string(i);
    db.execute_local_query("INSERT INTO t VALUES (" + std::to_string(N + (size_t)i) + ",'" + v + "'," + std::to_string(5) + ")");
    state.push_back(v);
    n.push_back("5");
  }
  rows_match(*t, (size_t)c_state, state, "state after INSERTs");
  OK(cs.encoding == ColumnEncoding::DictBitPacked && cs.bit_width_ <= 8,
     "12 new values appended: state is %s at %u bits (18 entries need 5)", enc_name(cs.encoding), (unsigned)cs.bit_width_);

  // 4. The memory of it: the state column at 4 B a row is the failure this guards against.
  const size_t bytes = cs.estimate_memory_bytes();
  OK(bytes < N * 2, "state column holds %zu rows in %zu bytes (under 2 B a row; 4 B a row before)", cs.size(), bytes);

  db.execute_local_query("DROP TABLE t");
  std::printf("%s\n", failures ? "FAILED" : "PASSED");
  return failures ? 1 : 0;
}
