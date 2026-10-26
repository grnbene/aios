// DELETE ... MATCHING (what the mirror applies a keyless table's replicated deletes with): on a table the vectorized
// selection takes (100k rows or more), the rows are found through the probe column's IN, not a walk from the first
// row. Every image must delete exactly the row a walk would: one row per image, duplicates one at a time, an image
// the table does not hold deletes nothing - with zero dates, decimals written two ways, text matched as `=` matches
// it (case, trailing spaces) and NULLs in the other columns. The table's contents are checked against a model after.
#undef NDEBUG
#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"

using namespace aios;
using namespace aios::userland;

namespace {
int failures = 0;
void check(bool ok, const std::string& what) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what.c_str());
  if (!ok) ++failures;
}
std::string one(SqlDatabase& db, const std::string& sql) {
  auto r = db.execute_local_query(sql);
  if (!r.success) return "ERROR " + r.error_message;
  return r.rows.empty() || r.rows[0].values.empty() ? "<none>" : r.rows[0].values[0];
}
struct Row { std::string at, who, amount, note; };   // note "" is NULL
std::string q(const std::string& v) { return v.empty() ? "NULL" : "'" + v + "'"; }
std::string at_of(int i) {
  if (i % 1000 == 7) return "0000-00-00 00:00:00";
  char b[32];
  std::snprintf(b, sizeof b, "2026-%02d-%02d %02d:%02d:%02d", 4 + (i / 30000) % 6, 1 + (i / 1000) % 28, (i / 60) % 24, i % 60, (i * 7) % 60);
  return b;
}
}  // namespace

int main() {
  const std::string root = "/tmp/aios-delete-matching-check";
  if (std::system(("rm -rf " + root).c_str()) != 0) std::printf("could not remove %s\n", root.c_str());
  KernelConfig cfg;
  cfg.root = root;
  cfg.residency = test_config;
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::printf("boot failed\n"); return 1; }
  SqlDatabase db(**k);
  db.init("sql_db");
  db.execute_local_query("CREATE DATABASE m");
  db.execute_local_query("USE m");
  check(db.execute_local_query("CREATE TABLE log (at DATETIME NOT NULL, who VARCHAR(20), amount DECIMAL(10,2), note TEXT)").success, "a keyless table");
  std::vector<Row> model;
  for (int i = 0; i < 120000; ++i)
    model.push_back({at_of(i), (i % 5 == 0) ? "Ann" : "bob" + std::to_string(i % 300), std::to_string(i % 977) + "." + (i % 2 ? "50" : "05"),
                     (i % 11 == 0) ? "" : "note " + std::to_string(i)});
  // a few rows twice, the same image
  for (int i = 0; i < 50; ++i) model.push_back(model[(size_t)i * 1999]);
  for (size_t at = 0; at < model.size(); at += 2000) {
    std::string sql = "INSERT INTO log VALUES ";
    for (size_t i = at; i < std::min(model.size(), at + 2000); ++i)
      sql += std::string(i == at ? "" : ",") + "(" + q(model[i].at) + "," + q(model[i].who) + "," + model[i].amount + "," + q(model[i].note) + ")";
    if (!db.execute_local_query(sql).success) { std::printf("insert failed\n"); return 1; }
  }
  check(db.flush_now().ok(), "checkpointed (typed and encoded as a stored table is)");

  // The images, as the binlog gives them: the stored text, but `who` in another case with trailing spaces (text is
  // matched as = matches it), an amount written with one decimal where it ends in 0 (a number, as = compares).
  auto image = [&](const Row& r, bool vary) {
    std::string who = r.who, amount = r.amount;
    if (vary && !who.empty()) { for (char& ch : who) ch = (char)std::toupper((unsigned char)ch); who += "  "; }
    if (vary && amount.size() > 1 && amount.back() == '0') amount.pop_back();
    return "(" + q(r.at) + "," + q(who) + "," + amount + "," + q(r.note) + ")";
  };
  std::vector<size_t> gone;
  std::string sql = "DELETE FROM log MATCHING ";
  bool first = true;
  auto add = [&](const std::string& img) { sql += (first ? "" : ", ") + img; first = false; };
  for (size_t i = 0; i < model.size(); i += 97) { add(image(model[i], i % 2)); gone.push_back(i); }
  for (int i = 0; i < 50; ++i) { add(image(model[(size_t)i * 1999], false)); }   // the duplicated rows: one copy each
  for (int i = 0; i < 2000; ++i) add("('2025-01-0" + std::to_string(1 + i % 9) + " 00:00:00','nobody'," + std::to_string(i) + ".00,NULL)");   // not in the table
  add("('0000-00-00 00:00:00','nobody',1.00,NULL)");   // a zero date the table holds, with values it does not
  const auto t0 = std::chrono::steady_clock::now();
  auto r = db.execute_local_query(sql);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  check(r.success, "DELETE ... MATCHING ran" + (r.success ? std::string() : ": " + r.error_message));
  // the model: the rows named, each once; one copy of each duplicated image (index 50 rows: the copies at the end)
  std::vector<bool> drop(model.size(), false);
  for (size_t i : gone) drop[i] = true;
  std::map<std::string, int> dup_left;
  for (int i = 0; i < 50; ++i) dup_left[image(model[(size_t)i * 1999], false)]++;
  // a duplicated image whose first copy was also named above (i % 97 == 0) is deleted twice: both copies
  size_t expect_gone = gone.size() + 50;
  check(r.rows_affected == expect_gone, "one row per image held, none for the 2,001 it does not hold (" + std::to_string(r.rows_affected) + " of " +
                                            std::to_string(expect_gone) + ", " + std::to_string((int)ms) + " ms)");
  check(one(db, "SELECT COUNT(*) FROM log") == std::to_string(model.size() - expect_gone), "rows left: " + one(db, "SELECT COUNT(*) FROM log"));
  // every row named is gone; a row not named is still there
  size_t wrong = 0;
  for (size_t i = 0; i < 120000; i += 97 * 13) {
    const std::string c = one(db, "SELECT COUNT(*) FROM log WHERE at = " + q(model[i].at) + " AND who = " + q(model[i].who) + " AND amount = " + model[i].amount);
    const bool dup = i % 1999 == 0 && i / 1999 < 50;
    const std::string want = drop[i] ? (dup ? "0" : "0") : (dup ? "1" : "1");
    if (c != want) ++wrong;
  }
  for (size_t i = 1; i < 120000; i += 977) {
    if (drop[i]) continue;
    const std::string c = one(db, "SELECT COUNT(*) FROM log WHERE at = " + q(model[i].at) + " AND who = " + q(model[i].who) + " AND amount = " + model[i].amount);
    const bool dup = i % 1999 == 0 && i / 1999 < 50;
    if (c != (dup ? "1" : "1")) ++wrong;
  }
  check(wrong == 0, "the rows named are gone, the others stay (" + std::to_string(wrong) + " wrong)");
  check(one(db, "SELECT COUNT(*) FROM log WHERE at = '0000-00-00 00:00:00'") ==
            std::to_string([&] { size_t n = 0; for (size_t i = 0; i < model.size(); ++i) if (!drop[i] && model[i].at[0] == '0') ++n; return n; }() -
                           [&] { size_t n = 0; for (int i = 0; i < 50; ++i) if (model[(size_t)i * 1999].at[0] == '0' && !drop[(size_t)i * 1999]) ++n; return n; }()),
        "zero-date rows: the named ones gone (" + one(db, "SELECT COUNT(*) FROM log WHERE at = '0000-00-00 00:00:00'") + " left)");
  if (std::system(("rm -rf " + root).c_str()) != 0) std::printf("could not remove %s\n", root.c_str());
  if (failures) { std::printf("=== %d FAILURE(S) ===\n", failures); return 1; }
  std::printf("=== DELETE MATCHING CHECKS PASSED ===\n");
  return 0;
}
