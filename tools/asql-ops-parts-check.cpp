// A table whose rows are erased or changed is saved by a checkpoint as a part of those writes on its file
// (kOpsPartMagic), not written whole; a reopened store must read every row back exactly as the table held it.
// The cases: deletes and updates on rows the file holds; writes to rows that an earlier append part added (at
// load those are still in the delta buffer); deletes, updates and inserts in one checkpoint; a write the journal
// does not record (the table is then written whole); and many checkpoints in a row, past the part bounds.
#include <algorithm>
#include <cstdlib>
#include <dirent.h>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "aios/kernel/kernel.hpp"
#include "aios/storage/object_store.hpp"
#include "aios/userland/sql_database.hpp"

using namespace aios;
using namespace aios::userland;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const std::string& what) {
  g_checks++;
  if (!ok) { std::cerr << "  [FAIL] " << what << "\n"; g_failures++; }
  else std::cout << "  [ok] " << what << "\n";
}

void must(SqlDatabase& db, const std::string& sql) {
  auto r = db.execute_query(sql);
  if (!r.success) { std::cerr << "  [FAIL] " << sql.substr(0, 80) << ": " << r.error_message << "\n"; g_failures++; }
}

std::string contents(SqlDatabase& db) {
  auto r = db.execute_query("SELECT id, name, qty, note FROM t ORDER BY id");
  if (!r.success) { std::cerr << "  [FAIL] reading t: " << r.error_message << "\n"; g_failures++; return "ERROR " + r.error_message; }
  std::string out;
  for (const auto& row : r.rows) {
    for (const auto& v : row.values) out += v + "\t";
    out += "\n";
  }
  return out;
}

// The table files in the store: a part keeps the name, a table written whole gets a new generation's name.
std::set<std::string> table_files(const std::string& dir) {
  std::set<std::string> out;
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) {
      const std::string n = e->d_name;
      if (n.size() > 4 && n.compare(n.size() - 4, 4, ".tbl") == 0) out.insert(n);
    }
    closedir(d);
  }
  return out;
}

void insert_rows(SqlDatabase& db, int from, int to) {
  for (int at = from; at < to; at += 500) {
    std::string sql = "INSERT INTO t VALUES ";
    for (int i = at; i < std::min(to, at + 500); ++i)
      sql += std::string(i == at ? "" : ",") + "(" + std::to_string(i) + ", 'name-" + std::to_string(i % 977) + "', " +
             std::to_string(i % 41) + ", " + (i % 9 == 0 ? std::string("NULL") : "'note " + std::to_string(i) + " / é'") + ")";
    must(db, sql);
  }
}

}  // namespace

// Run again by main with AIOS_STORE_NO_OPS_PARTS=1 on the store it left: a table whose file holds parts of writes
// is written whole at the next checkpoint even though nothing changed it (so an older binary can read the store),
// and reads back the same.
int fold_mode(const char* root) {
  KernelConfig cfg;
  cfg.root = root;
  cfg.residency = test_config;
  auto k_res = Kernel::boot(cfg);
  if (!k_res.ok()) { std::cerr << "boot: " << k_res.error().message << "\n"; return 1; }
  Kernel& kernel = *k_res.value();
  const std::string dir = std::string(root) + "/sql_db/tables";
  SqlDatabase db(kernel);
  db.init("sql_db");
  must(db, "USE aios_db");
  const std::string want = contents(db);
  const auto files = table_files(dir);
  check(db.flush_now().ok(), "parts of writes turned off: checkpoint");
  check(table_files(dir) != files, "parts of writes turned off: the table holding them was written whole");
  SqlDatabase again(kernel);
  again.init("sql_db");
  must(again, "USE aios_db");
  check(contents(again) == want, "parts of writes turned off: the rewritten table reads back the same");
  std::cout << (g_checks - g_failures) << "/" << g_checks << " checks passed (folded)\n";
  return g_failures ? 1 : 0;
}

// Run by main on the store it left, as a restart: the first checkpoint after a start adds the writes to the files as
// parts too (the journal starts at load).
int restart_mode(const char* root) {
  KernelConfig cfg;
  cfg.root = root;
  cfg.residency = test_config;
  auto k_res = Kernel::boot(cfg);
  if (!k_res.ok()) { std::cerr << "boot: " << k_res.error().message << "\n"; return 1; }
  Kernel& kernel = *k_res.value();
  const std::string dir = std::string(root) + "/sql_db/tables";
  SqlDatabase db(kernel);
  db.init("sql_db");
  must(db, "USE aios_db");
  const auto files = table_files(dir);
  must(db, "DELETE FROM t WHERE id BETWEEN 2700 AND 2710");
  check(db.flush_now().ok(), "after a start, deletes: checkpoint");
  check(table_files(dir) == files, "after a start, deletes: the file was added to");
  const std::string want = contents(db);
  SqlDatabase again(kernel);
  again.init("sql_db");
  must(again, "USE aios_db");
  check(contents(again) == want, "after a start: a reopened store reads every row back");
  std::cout << (g_checks - g_failures) << "/" << g_checks << " checks passed (restart)\n";
  return g_failures ? 1 : 0;
}

// Zone-map segments without stats - a DELETE of several rows leaves every segment after them so, and a table written
// whole saves them so - are refreshed by each checkpoint's maintenance, 16 a column (fewer for long text: it is
// budgeted by time). That changes no value and must not make the checkpoint write the table again: chi's 30M-row
// audit log, its userid and querystring columns 14,000 segments short of stats, was written whole (320 MB under the
// lock) at every checkpoint, 2 querystring segments refreshed each time - weeks of it.
// --loose-make: a table, a DELETE near its front, written whole; --loose-check, as the restart: idle checkpoints.
int loose_mode(const char* root, bool make) {
  KernelConfig cfg;
  cfg.root = root;
  cfg.residency = test_config;
  auto k_res = Kernel::boot(cfg);
  if (!k_res.ok()) { std::cerr << "boot: " << k_res.error().message << "\n"; return 1; }
  Kernel& kernel = *k_res.value();
  const std::string dir = std::string(root) + "/sql_db/tables";
  SqlDatabase db(kernel);
  db.init("sql_db");
  must(db, "CREATE DATABASE IF NOT EXISTS loosedb");
  must(db, "USE loosedb");
  auto count = [&](const std::string& where) {
    auto r = db.execute_query("SELECT COUNT(*) FROM z WHERE " + where);
    return r.success && !r.rows.empty() ? r.rows[0].values[0] : "ERROR " + r.error_message;
  };
  if (make) {
    must(db, "CREATE TABLE z (id INT PRIMARY KEY, v INT, s VARCHAR(20))");
    for (int at = 0; at < 100000; at += 1000) {
      std::string sql = "INSERT INTO z VALUES ";
      for (int i = at; i < at + 1000; ++i) sql += std::string(i == at ? "" : ",") + "(" + std::to_string(i) + "," + std::to_string(i) + ",'s-" + std::to_string(i % 5000) + "')";
      must(db, sql);
    }
    check(db.flush_now().ok(), "loose: the table written");
    must(db, "DELETE FROM z WHERE id BETWEEN 10 AND 20");   // every segment from the first holds shifted rows: no stats
    check(db.flush_now().ok(), "loose: the writes checkpointed");
    std::cout << (g_checks - g_failures) << "/" << g_checks << " checks passed (loose, made)\n";
    return g_failures ? 1 : 0;
  }
  const SqlTable* t = db.find_table("z", "loosedb");
  size_t no_stats = 0, segs = 0;
  if (t) for (const auto& zm : t->col_data[1].zone_maps) { ++segs; no_stats += !zm.has_stats; }
  check(no_stats > 16, "loose: read back, " + std::to_string(no_stats) + " of z.v's " + std::to_string(segs) + " segments have no stats (more than one checkpoint refreshes)");
  const auto files = table_files(dir);
  for (int round = 0; round < 4; ++round) check(db.flush_now().ok(), "loose: an idle checkpoint");
  check(table_files(dir) == files, "loose: idle checkpoints refreshing segment stats do not write the table again");
  size_t still = 0;
  if ((t = db.find_table("z", "loosedb"))) for (const auto& zm : t->col_data[1].zone_maps) still += !zm.has_stats;
  check(still < no_stats, "loose: the checkpoints did refresh segments (" + std::to_string(no_stats) + " -> " + std::to_string(still) + " without stats)");
  // what the segments answer: v = id, ids 10-20 gone
  check(count("v BETWEEN 1000 AND 5000") == "4001", "loose: a range over refreshed segments and ones without stats (" + count("v BETWEEN 1000 AND 5000") + ")");
  check(count("v BETWEEN 5 AND 25") == "10", "loose: around the deleted rows (" + count("v BETWEEN 5 AND 25") + ")");
  check(count("v > 99000") == "999", "loose: the last segments (" + count("v > 99000") + ")");
  std::cout << (g_checks - g_failures) << "/" << g_checks << " checks passed (loose, after a restart)\n";
  return g_failures ? 1 : 0;
}

// Cases 1-8, in one process (a store is open by one process at a time: the restart and the fold run after it).
int main_mode(const char* root_dir) {
  setenv("AIOS_STORE_COLD_SECS", "0", 1);   // a still table is deflated at the next checkpoint (case 8)
  KernelConfig cfg;
  cfg.root = root_dir;
  cfg.residency = test_config;
  auto k_res = Kernel::boot(cfg);
  if (!k_res.ok()) { std::cerr << "boot: " << k_res.error().message << "\n"; return 1; }
  Kernel& kernel = *k_res.value();
  SqlDatabase db(kernel);
  db.init("sql_db");
  std::string dir;
  {
    // where this store keeps its table files
    const std::string base = std::string(root_dir);
    for (const char* sub : {"/sql_db/tables", "/sql_db/sql_db/tables", "/tables"}) {
      if (DIR* d = opendir((base + sub).c_str())) { closedir(d); dir = base + sub; }
    }
  }

  must(db, "CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(40), qty INT, note TEXT)");
  insert_rows(db, 0, 60000);
  check(db.flush_now().ok(), "the table written whole once");
  if (dir.empty()) {
    for (const char* sub : {"/sql_db/tables", "/sql_db/sql_db/tables", "/tables"})
      if (DIR* d = opendir((std::string(root_dir) + sub).c_str())) { closedir(d); dir = std::string(root_dir) + sub; }
  }
  check(!dir.empty(), "found the store's table files (" + dir + ")");
  const auto first_files = table_files(dir);

  auto reopened_matches = [&](const std::string& what) {
    const std::string want = contents(db);
    SqlDatabase again(kernel);
    again.init("sql_db");
    const std::string got = contents(again);
    check(got == want, what + ": a reopened store reads every row back (" + std::to_string(std::count(want.begin(), want.end(), '\n')) + " rows)");
  };

  // 1. deletes only
  must(db, "DELETE FROM t WHERE id % 7 = 3");
  check(db.flush_now().ok(), "deletes: checkpoint");
  check(table_files(dir) == first_files, "deletes: the file was added to, not written again");
  reopened_matches("deletes");

  // 2. updates only
  must(db, "UPDATE t SET qty = qty + 100, note = 'changed' WHERE id BETWEEN 1000 AND 1600");
  must(db, "UPDATE t SET name = NULL WHERE id = 42");
  check(db.flush_now().ok(), "updates: checkpoint");
  check(table_files(dir) == first_files, "updates: the file was added to");
  reopened_matches("updates");

  // 3. deletes, updates and inserts in one checkpoint
  must(db, "DELETE FROM t WHERE id BETWEEN 20000 AND 20500");
  insert_rows(db, 60000, 61000);
  must(db, "UPDATE t SET qty = -1 WHERE id BETWEEN 60100 AND 60200");
  must(db, "DELETE FROM t WHERE id BETWEEN 60300 AND 60310");
  check(db.flush_now().ok(), "mixed: checkpoint");
  check(table_files(dir) == first_files, "mixed: the file was added to");
  reopened_matches("mixed");

  // 4. an append-only checkpoint (an append part), then writes to those very rows: at load they are in the delta
  //    buffer when the part of writes is replayed
  insert_rows(db, 70000, 71500);
  check(db.flush_now().ok(), "append part: checkpoint");
  must(db, "UPDATE t SET note = 'set in delta' WHERE id BETWEEN 70100 AND 70120");
  must(db, "DELETE FROM t WHERE id BETWEEN 70500 AND 70600");
  must(db, "DELETE FROM t WHERE id IN (5, 6, 70001)");
  check(db.flush_now().ok(), "writes to appended rows: checkpoint");
  check(table_files(dir) == first_files, "writes to appended rows: the file was added to");
  reopened_matches("writes to rows an append part added");

  // 5. a write the journal does not record: written whole, and still read back
  must(db, "TRUNCATE TABLE t");
  insert_rows(db, 0, 3000);
  must(db, "DELETE FROM t WHERE id < 10");
  check(db.flush_now().ok(), "after an unrecorded write: checkpoint");
  check(table_files(dir) != first_files, "after an unrecorded write: the table was written whole");
  reopened_matches("after an unrecorded write");

  // 6. many checkpoints of writes: past the part bounds the table is written whole again, and reads back
  const auto before_many = table_files(dir);
  for (int round = 0; round < 70; ++round) {
    must(db, "UPDATE t SET qty = qty + 1 WHERE id = " + std::to_string(100 + round));
    must(db, "DELETE FROM t WHERE id = " + std::to_string(2000 + round));
    if (!db.flush_now().ok()) { check(false, "many checkpoints: one failed"); break; }
  }
  check(table_files(dir) != before_many, "many checkpoints: past the bound of parts the table was written whole");
  reopened_matches("many checkpoints");

  // 7. the store reopened twice over (a part read, then written on by the reopened store's own checkpoint)
  {
    must(db, "UPDATE t SET note = 'last' WHERE id BETWEEN 2500 AND 2600");
    check(db.flush_now().ok(), "last: checkpoint");
    const std::string want = contents(db);
    SqlDatabase again(kernel);
    again.init("sql_db");
    check(contents(again) == want, "reopened once");
  }

  // 8. a still table is deflated once (AIOS_STORE_COLD_SECS=0 here: at the next checkpoint); a reopened store knows
  //    its file is deflated and leaves it alone. A start forgot, and every big still table was copied whole again.
  {
    check(db.flush_now().ok(), "still: checkpoint");
    check(db.flush_now().ok(), "still: checkpoint deflating the still table");
    const auto deflated = table_files(dir);
    check(db.flush_now().ok(), "still: one more");
    check(table_files(dir) == deflated, "still: a deflated table is not written again");
    SqlDatabase again(kernel);
    again.init("sql_db");
    check(again.flush_now().ok(), "reopened: checkpoint");
    check(table_files(dir) == deflated, "reopened: the deflated table is not written again");
  }

  // 9. only_appended: appending rows that introduce novel dictionary keys alters the column encoding
  //    layout (advancing layout_seq past prev.layout), but because the change journal recorded only
  //    appends ('A'), only_appended() preserves the append part path rather than rewriting whole.
  {
    const auto files_before = table_files(dir);
    for (int i = 80000; i < 80500; ++i) {
      must(db, "INSERT INTO t VALUES (" + std::to_string(i) + ", 'novel-sym-" + std::to_string(i * 37) + "', " +
               std::to_string(i) + ", 'unique text " + std::to_string(i) + "')");
    }
    check(db.flush_now().ok(), "only_appended: checkpoint");
    check(table_files(dir) == files_before, "only_appended: appends with new dictionary keys appended a part");
    reopened_matches("only_appended: rows verified on reopen");
  }

  std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
  return g_failures ? 1 : 0;
}

int main(int argc, char** argv) {
  if (argc > 2 && std::string(argv[1]) == "--main") return main_mode(argv[2]);
  if (argc > 2 && std::string(argv[1]) == "--fold") return fold_mode(argv[2]);
  if (argc > 2 && std::string(argv[1]) == "--restart") return restart_mode(argv[2]);
  if (argc > 2 && std::string(argv[1]) == "--loose-make") return loose_mode(argv[2], true);
  if (argc > 2 && std::string(argv[1]) == "--loose-check") return loose_mode(argv[2], false);
  char root_dir[] = "/tmp/aios-opsparts-XXXXXX";
  if (!mkdtemp(root_dir)) { std::perror("mkdtemp"); return 1; }
  const std::string self = argv[0], root = root_dir;
  int failed = 0;
  // 9. a restart, then writes; 10. turned off, the parts fold into a whole file - each a process of its own, as a
  //    restart (with the setting) would be
  for (const std::string& cmd : {self + " --main " + root, self + " --restart " + root,
                                 "AIOS_STORE_NO_OPS_PARTS=1 " + self + " --fold " + root,
                                 "AIOS_STORE_NO_OPS_PARTS=1 " + self + " --loose-make " + root + "/loose", self + " --loose-check " + root + "/loose"})
    if (std::system(cmd.c_str()) != 0) { std::cerr << "[FAIL] " << cmd << "\n"; ++failed; }
  if (std::system(("rm -rf " + root).c_str()) != 0) std::cerr << "could not remove " << root << "\n";
  std::cout << (failed ? "OPS PARTS CHECK FAILED\n" : "ALL OPS PARTS CHECKS PASSED\n");
  return failed ? 1 : 0;
}
