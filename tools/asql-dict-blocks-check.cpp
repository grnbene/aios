// A dictionary of long entries (JSON options, notes, request bodies) is held in compressed blocks: atl's
// salesorderdetails.po_options was 27 MB as symbol-coded entries and 4 MB as 64 KB deflate blocks of the same text.
//   - after encoding, such a column's dictionary is under a quarter of its plain bytes;
//   - every row reads back exactly; a filter and an ORDER BY over it answer what the plain column did;
//   - a point read costs a block decode at most (and the block stays decoded for the next one);
//   - the blocked form survives a checkpoint and a reload;
//   - new values appended after blocking stay plain until the next blocking, and are found.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"

using namespace aios;
using namespace aios::userland;

static int failures = 0;
#define OK(c, ...) do { if (!(c)) { std::printf("  [FAIL] "); ++failures; } else std::printf("  [ok] "); std::printf(__VA_ARGS__); std::printf("\n"); } while (0)

// An options payload as the ERP writes one: the same keys every time, a few values varying.
static std::string payload(std::mt19937_64& rng, size_t k) {
  static const char* kWheels[] = {"Alloy 17\\\" Silver", "Steel 16\\\" Black", "Alloy 18\\\" Gunmetal", "Alloy 19\\\" Machined"};
  static const char* kNotes[] = {"Customer will call for pickup", "Deliver to rear dock after 2pm", "Leave with office", "Call before delivery", ""};
  std::string s = "{\"items\":[";
  const int n = 1 + (int)(rng() % 4);
  for (int i = 0; i < n; ++i) {
    if (i) s += ",";
    s += "{\"sku\":\"" + std::to_string(100000 + (k * 7 + (size_t)i) % 90000) + "\",\"qty\":" + std::to_string(1 + rng() % 8) +
         ",\"wheel\":\"" + kWheels[rng() % 4] + "\",\"tpms\":" + (rng() % 2 ? "true" : "false") +
         ",\"balance\":{\"type\":\"road-force\",\"weights\":[" + std::to_string(rng() % 60) + "," + std::to_string(rng() % 60) + "]}"
         ",\"mount\":{\"bay\":" + std::to_string(1 + rng() % 6) + ",\"tech\":\"T" + std::to_string(rng() % 20) + "\"}}";
  }
  s += "],\"delivery\":{\"window\":\"" + std::to_string(8 + rng() % 8) + ":00-" + std::to_string(12 + rng() % 6) + ":00\",\"note\":\"" + kNotes[rng() % 5] +
       "\",\"signature\":" + (rng() % 2 ? "true" : "false") + "},\"order_ref\":\"PO-" + std::to_string(k) + "\",\"version\":3}";
  return s;
}

int main() {
  KernelConfig cfg;
  cfg.root = "/tmp/aios-asql-dict-blocks-test";
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::printf("kernel boot failed\n"); return 1; }
  std::mt19937_64 rng(7);
  const size_t N = 60000, D = 30000;   // rows; distinct payloads
  std::vector<std::string> vals(D);
  size_t plain = 0;
  for (size_t i = 0; i < D; ++i) { vals[i] = payload(rng, i); plain += vals[i].size(); }
  std::vector<std::string> model(N);
  {
    SqlDatabase db(**k);
    db.init("sql_db");
    db.execute_local_query("DROP TABLE IF EXISTS sod");
    db.execute_local_query("CREATE TABLE sod (id INT NOT NULL, orderno INT NOT NULL, po_options TEXT, PRIMARY KEY(id))");
    std::string sql;
    for (size_t i = 0; i < N; ++i) {
      model[i] = vals[i < D ? i : rng() % D];   // every distinct value once, then repeats
      std::string esc;
      for (char ch : model[i]) { if (ch == '\'' || ch == '\\') esc += '\\'; esc += ch; }
      sql += sql.empty() ? "INSERT INTO sod VALUES " : ",";
      sql += "(" + std::to_string(i + 1) + "," + std::to_string(1000 + i / 3) + ",'" + esc + "')";
      if ((i + 1) % 500 == 0) { db.execute_local_query(sql); sql.clear(); }
    }
    if (!sql.empty()) db.execute_local_query(sql);
    SqlTable* t = db.resolve_table("sod", "aios_db");
    if (!t || t->row_count() != N) { std::printf("table not loaded (%zu)\n", t ? t->row_count() : 0); return 1; }
    const int c = t->get_column_index("po_options");
    SqlTable::encode_columns(t->col_data, t->columns);
    ColumnStore& cs = t->col_data[(size_t)c];
    const size_t dict_bytes = cs.dict.memory_bytes();
    std::printf("po_options: %zu rows, %zu distinct, plain %zu KB, dictionary held in %zu KB (%.2f of plain), column %zu KB\n",
                N, cs.dict.size() - 1, plain >> 10, dict_bytes >> 10, (double)dict_bytes / plain, cs.estimate_memory_bytes() >> 10);
    OK(dict_bytes * 4 < plain, "the dictionary is under a quarter of its plain text (%zu KB of %zu KB)", dict_bytes >> 10, plain >> 10);

    size_t bad = 0;
    for (size_t r = 0; r < N; ++r) if (cs.get(r) != model[r]) ++bad;
    OK(bad == 0, "every row reads back as written (%zu wrong)", bad);

    // Point reads in a random order: a decode per block miss, nothing per hit.
    std::vector<size_t> order(20000);
    for (auto& o : order) o = rng() % N;
    const auto t0 = std::chrono::steady_clock::now();
    size_t sum = 0;
    for (size_t r : order) sum += cs.get(r).size();
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / order.size();
    OK(sum > 0 && us < 40.0, "20k random point reads across every block: %.2f us each (a block decode on each miss)", us);

    // The SQL over it answers what the plain column did.
    size_t expect_rf = 0, expect_sig = 0;
    for (size_t r = 0; r < N; ++r) { if (model[r].find("\"tech\":\"T7\"") != std::string::npos) ++expect_rf; if (model[r].find("\"signature\":true") != std::string::npos) ++expect_sig; }
    auto count = [&](const std::string& q) -> long long {
      auto res = db.execute_local_query(q);
      if (!res.success || res.rows.empty() || res.rows[0].values.empty()) return -1;
      return std::stoll(res.rows[0].values[0]);
    };
    const long long rf = count("SELECT COUNT(*) FROM sod WHERE po_options LIKE '%\"tech\":\"T7\"%'");
    const long long sig = count("SELECT COUNT(*) FROM sod WHERE po_options LIKE '%\"signature\":true%'");
    OK(rf == (long long)expect_rf && sig == (long long)expect_sig, "LIKE filters: %lld and %lld rows (model %zu, %zu)", rf, sig, expect_rf, expect_sig);
    auto res = db.execute_local_query("SELECT po_options FROM sod ORDER BY po_options LIMIT 3");
    std::vector<std::string> sorted(model);
    std::sort(sorted.begin(), sorted.end());
    OK(res.success && res.rows.size() == 3 && res.rows[0].values[0] == sorted[0] && res.rows[2].values[0] == sorted[2], "ORDER BY over the column: the model's first three");

    // New values after blocking: plain tail, found, and read back.
    for (int i = 0; i < 50; ++i) {
      const std::string v = "{\"late\":" + std::to_string(i) + "}";
      db.execute_local_query("INSERT INTO sod VALUES (" + std::to_string(N + (size_t)i + 1) + ",1,'" + v + "')");
      model.push_back(v);
    }
    bad = 0;
    for (size_t r = 0; r < model.size(); ++r) if (cs.get(r) != model[r]) ++bad;
    OK(bad == 0 && count("SELECT COUNT(*) FROM sod WHERE po_options = '{\"late\":7}'") == 1, "50 values appended after blocking: all read back, the new one found by =");

    auto fl = db.flush_now();
    OK(fl.ok(), "checkpoint written");
  }
  {
    SqlDatabase db2(**k);
    db2.init("sql_db");
    SqlTable* t = db2.resolve_table("sod", "aios_db");
    if (!t) { std::printf("  [FAIL] table gone after reload\n"); ++failures; }
    else {
      ColumnStore& cs = t->col_data[(size_t)t->get_column_index("po_options")];
      size_t bad = 0;
      for (size_t r = 0; r < model.size() && r < cs.size(); ++r) if (cs.get(r) != model[r]) ++bad;
      OK(cs.size() == model.size() && bad == 0, "reloaded: %zu rows, every value as written (%zu wrong)", cs.size(), bad);
      OK(cs.dict.memory_bytes() * 4 < plain, "reloaded dictionary still held in blocks: %zu KB", cs.dict.memory_bytes() >> 10);
      db2.execute_local_query("DROP TABLE sod");
    }
  }
  std::printf("%s\n", failures ? "FAILED" : "PASSED");
  return failures ? 1 : 0;
}
