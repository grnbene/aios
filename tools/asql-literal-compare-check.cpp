// Checks `column <op> literal` against what MySQL answers, through every
// path the engine has for it: a small table (the single-column fast paths -
// the aggregate one behind COUNT(*), the scan behind a row SELECT) and a big
// one (the vectorized selection), dictionary-coded and re-encoded. INT, DOUBLE
// and VARCHAR columns against quoted numbers, plain numbers, and text with
// other case and trailing spaces. `id > '5'` on a small INT table matched no
// row at all on the live mirror (the fast path had only = and <> for text).
#undef NDEBUG
#include <filesystem>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"

using namespace aios;
using namespace aios::userland;

namespace {

int failures = 0;
int checks = 0;

std::string q1(SqlDatabase& db, const std::string& sql) {
  SqlQueryResult r = db.execute_local_query(sql);
  if (!r.success) { std::printf("  query failed: %s\n  %s\n", sql.c_str(), r.error_message.c_str()); std::exit(1); }
  if (r.rows.empty() || r.rows[0].values.empty()) return "<none>";
  return r.rows[0].values[0];
}

size_t row_count(SqlDatabase& db, const std::string& sql) {
  SqlQueryResult r = db.execute_local_query(sql);
  if (!r.success) { std::printf("  query failed: %s\n  %s\n", sql.c_str(), r.error_message.c_str()); std::exit(1); }
  return r.rows.size();
}

void exec(SqlDatabase& db, const std::string& sql) {
  SqlQueryResult r = db.execute_local_query(sql);
  if (!r.success) { std::printf("  statement failed: %s\n  %s\n", sql.substr(0, 160).c_str(), r.error_message.c_str()); std::exit(1); }
}

// MySQL's reading of text as a number: its leading numeric part, 0 if none.
double numeric_prefix(const std::string& s) {
  size_t i = 0;
  while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
  const char* p = s.c_str() + i;
  char* end = nullptr;
  double d = std::strtod(p, &end);
  return end == p ? 0.0 : d;
}

// utf8_general_ci on ASCII with PAD SPACE: case folded, trailing spaces ignored.
int ci_pad_compare(const std::string& a, const std::string& b) {
  auto trim = [](std::string s) { while (!s.empty() && s.back() == ' ') s.pop_back(); return s; };
  const std::string x = trim(a), y = trim(b);
  for (size_t i = 0; i < x.size() && i < y.size(); ++i) {
    const int c = std::toupper((unsigned char)x[i]), d = std::toupper((unsigned char)y[i]);
    if (c != d) return c < d ? -1 : 1;
  }
  return x.size() == y.size() ? 0 : (x.size() < y.size() ? -1 : 1);
}

bool apply(const std::string& op, int c) {
  if (op == "=") return c == 0;
  if (op == "<>") return c != 0;
  if (op == ">") return c > 0;
  if (op == ">=") return c >= 0;
  if (op == "<") return c < 0;
  return c <= 0;
}

struct Lit { std::string sql; bool quoted; std::string text; };

void run_table(SqlDatabase& db, const std::string& table, size_t rows, const char* step) {
  // id INT, amt DOUBLE, code VARCHAR: the values as the model holds them.
  std::vector<long long> ids;
  std::vector<double> amts;
  std::vector<std::string> codes;
  static const char* words[] = {"apple", "Apple ", "banana", "B", "b", "10", "5", "zeta", "Zebra", "  lead"};
  for (size_t i = 0; i < rows; ++i) {
    ids.push_back((long long)(i % 23) - 3);
    amts.push_back((double)((long long)(i % 37) - 10) / 4.0);
    codes.push_back(words[i % 10]);
  }
  const std::vector<std::string> ops = {"=", "<>", ">", ">=", "<", "<="};
  const std::vector<Lit> num_lits = {{"'5'", true, "5"}, {"5", false, "5"}, {"'10'", true, "10"}, {"'4.5'", true, "4.5"},
                                     {"'-2'", true, "-2"}, {"'abc'", true, "abc"}, {"' 7'", true, " 7"}, {"'2.25'", true, "2.25"}};
  const std::vector<Lit> text_lits = {{"'apple'", true, "apple"}, {"'APPLE'", true, "APPLE"}, {"'apple  '", true, "apple  "},
                                      {"'b'", true, "b"}, {"'10'", true, "10"}, {"'5'", true, "5"}, {"'Zeta'", true, "Zeta"}};
  auto check = [&](const std::string& col, const std::string& op, const Lit& lit, size_t want) {
    const std::string where = " WHERE " + col + " " + op + " " + lit.sql;
    const std::string cnt = q1(db, "SELECT COUNT(*) FROM " + table + where);
    const size_t got_rows = row_count(db, "SELECT id FROM " + table + where);
    ++checks;
    if (cnt != std::to_string(want) || got_rows != want) {
      ++failures;
      std::printf("  [FAIL] %s %s%s: COUNT(*) %s, rows %zu, want %zu\n", step, table.c_str(), where.c_str(), cnt.c_str(), got_rows, want);
    }
  };
  for (const auto& op : ops) {
    for (const auto& lit : num_lits) {
      const double v = lit.quoted ? numeric_prefix(lit.text) : std::strtod(lit.text.c_str(), nullptr);
      size_t want_id = 0, want_amt = 0;
      for (size_t i = 0; i < rows; ++i) {
        want_id += apply(op, ids[i] < v ? -1 : ids[i] > v ? 1 : 0);
        want_amt += apply(op, amts[i] < v ? -1 : amts[i] > v ? 1 : 0);
      }
      check("id", op, lit, want_id);
      check("amt", op, lit, want_amt);
    }
    for (const auto& lit : text_lits) {
      size_t want = 0;
      for (size_t i = 0; i < rows; ++i) want += apply(op, ci_pad_compare(codes[i], lit.text));
      check("code", op, lit, want);
    }
    // A text column against a plain number compares the cells' numeric prefixes.
    size_t want = 0;
    for (size_t i = 0; i < rows; ++i) {
      const double d = numeric_prefix(codes[i]);
      want += apply(op, d < 5 ? -1 : d > 5 ? 1 : 0);
    }
    check("code", op, {"5", false, "5"}, want);
  }
}

void fill(SqlDatabase& db, const std::string& table, size_t rows) {
  static const char* words[] = {"apple", "Apple ", "banana", "B", "b", "10", "5", "zeta", "Zebra", "  lead"};
  exec(db, "CREATE TABLE " + table + " (id INT, amt DOUBLE, code VARCHAR(20))");
  std::string batch;
  for (size_t i = 0; i < rows; ++i) {
    char amt[32];
    std::snprintf(amt, sizeof amt, "%.2f", (double)((long long)(i % 37) - 10) / 4.0);
    batch += (batch.empty() ? "" : ",") + std::string("(") + std::to_string((long long)(i % 23) - 3) + "," + amt + ",'" + words[i % 10] + "')";
    if (batch.size() > 100000 || i + 1 == rows) { exec(db, "INSERT INTO " + table + " VALUES " + batch); batch.clear(); }
  }
}

}  // namespace

int main() {
  std::printf("=== ASQL column-vs-literal comparison check ===\n");
  const std::string root = "/tmp/aios-asql-literal-compare";
  { std::error_code fs_err; std::filesystem::remove_all(root, fs_err); }
  KernelConfig cfg;
  cfg.root = root;
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::printf("boot failed\n"); return 1; }
  SqlDatabase db(**k);
  db.init("sql_db");
  exec(db, "CREATE DATABASE IF NOT EXISTS cmpdb");
  exec(db, "USE cmpdb");
  fill(db, "small", 40);
  fill(db, "big", 30000);
  run_table(db, "small", 40, "small, as inserted");
  run_table(db, "big", 30000, "big, as inserted");
  db.optimize_all_tables("cmpdb");
  run_table(db, "small", 40, "small, re-encoded");
  run_table(db, "big", 30000, "big, re-encoded");
  std::printf("  %d comparisons checked\n", checks);
  if (failures) { std::printf("=== %d FAILURE(S) ===\n", failures); return 1; }
  std::printf("=== ALL LITERAL COMPARISON CHECKS PASSED ===\n");
  return 0;
}
