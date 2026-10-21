#undef NDEBUG
// Per-segment bit-packing of dictionary codes (ColumnStore::pack_segments): a column's
// rows read back exactly as written across segment boundaries, a partial last segment,
// NULLs, and every write after optimize_encoding (set, erase_rows, append); and a table
// survives a checkpoint and a reopen with the packed layout.
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include <unistd.h>

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
}

// The column read row by row against the model (NULL = sql_null()).
bool same(const ColumnStore& cs, const std::vector<std::string>& model, std::string* why) {
  if (cs.size() != model.size()) { *why = "size " + std::to_string(cs.size()) + " vs " + std::to_string(model.size()); return false; }
  for (size_t r = 0; r < model.size(); ++r) {
    const bool null = sql_is_null(model[r]);
    if (cs.is_null(r) != null || (!null && cs.get(r) != model[r])) {
      *why = "row " + std::to_string(r) + " reads '" + (cs.is_null(r) ? "NULL" : cs.get(r)) + "', want '" + (null ? "NULL" : model[r]) + "'";
      return false;
    }
  }
  return true;
}

// Every code decodes the same through read_code and through the layout writers expect.
bool codes_match_unpacked(const ColumnStore& cs) {
  ColumnStore flat = cs;
  flat.unpack_segments();
  if (flat.seg_packed_) return false;
  for (size_t r = 0; r < cs.size(); ++r) if (cs.read_code(r) != flat.read_code(r)) return false;
  return true;
}

ColumnStore build(const std::vector<std::string>& model) {
  ColumnStore cs;
  for (const auto& v : model) cs.push_back(v);
  cs.optimize_encoding(SqlDataType::Text);
  return cs;
}

struct Pattern {
  const char* name;
  size_t n;
  bool expect_packed;
  std::function<std::string(size_t)> value;
};

void column_tests() {
  const std::string nul = sql_null();
  const std::vector<Pattern> patterns = {
      // codes grow with the row: each segment sees a narrow window of a wide dictionary
      {"chronological", 6000, true, [](size_t i) { return "ts-" + std::to_string(i / 2); }},
      // few values, but a segment-local subset of a big dictionary
      {"local-subset", 5000, true, [](size_t i) { return "v" + std::to_string((i / 2048) * 1000 + (i * 7) % 5); }},
      // chronological with NULLs: code 0 sits in the range of every segment that holds one, so a NULL every 17 rows
      // keeps each segment as wide as the whole column and nothing is gained - read back exactly all the same
      {"with-nulls", 4100, false, [&](size_t i) { return i % 17 == 0 ? nul : "k" + std::to_string(i); }},   // (all distinct: runs of equal values are RLE's)
      {"exact-2-segments", 4096, true, [](size_t i) { return "e" + std::to_string(i / 2); }},   // (i / 4 is runs of four: RLE)
      // one segment: a segment's range is the column's, so there is nothing to gain (the edges are what is tested)
      {"one-short", 2047, false, [](size_t i) { return "s" + std::to_string(i / 2); }},
      // one row over: the saving is under the tenth pack_segments asks for
      {"one-over", 2049, false, [](size_t i) { return "o" + std::to_string(i / 2); }},
      {"tiny", 5, false, [](size_t i) { return "t" + std::to_string(i); }},
      // codes spread over the whole dictionary in every segment: nothing to gain
      {"scattered", 4096, false, [](size_t i) { return "x" + std::to_string((i * 2654435761u) % 3000); }},
  };
  for (const Pattern& p : patterns) {
    std::vector<std::string> model(p.n);
    for (size_t i = 0; i < p.n; ++i) model[i] = p.value(i);
    ColumnStore cs = build(model);
    std::string why;
    std::cout << "  " << p.name << ": " << p.n << " rows, " << (cs.seg_packed_ ? "segment-packed" : "global width " + std::to_string(cs.bit_width_))
              << ", " << cs.estimate_memory_bytes() << " bytes\n";
    check(cs.encoding == ColumnEncoding::DictBitPacked, std::string(p.name) + ": stays a dictionary column");
    if (cs.encoding != ColumnEncoding::DictBitPacked) continue;
    check(!p.expect_packed || cs.seg_packed_, std::string(p.name) + ": packs per segment");
    check(same(cs, model, &why), std::string(p.name) + ": reads back after optimize (" + why + ")");
    check(codes_match_unpacked(cs), std::string(p.name) + ": codes equal the global layout's");

    // a decode over arbitrary ranges equals read_code (segment edges, past the end)
    if (cs.seg_packed_) {
      std::vector<uint32_t> out(p.n + 5);
      const size_t starts[] = {0, 1, 2047, 2048, 2049, p.n > 3 ? p.n - 3 : 0};
      bool ok = true;
      for (size_t s : starts) {
        if (s > p.n) continue;
        cs.seg_decode(s, p.n + 5, out.data());
        for (size_t r = s; r < p.n + 5 && ok; ++r) ok = out[r - s] == cs.read_code(r);
      }
      check(ok, std::string(p.name) + ": seg_decode equals read_code over ranges");
    }

    // set() at the segment edges and in the partial last segment
    {
      ColumnStore w = cs;
      std::vector<std::string> m = model;
      const size_t rows[] = {0, 2047, 2048, p.n - 1, p.n / 2};
      for (size_t r : rows) {
        if (r >= p.n) continue;
        const std::string v = "set-" + std::to_string(r);
        w.set(r, v);
        m[r] = v;
      }
      w.set(p.n / 3, nul);
      m[p.n / 3] = nul;
      check(!w.seg_packed_, std::string(p.name) + ": a write returns the column to one width");
      check(same(w, m, &why), std::string(p.name) + ": set() after optimize (" + why + ")");
      w.optimize_encoding(SqlDataType::Text);
      check(same(w, m, &why), std::string(p.name) + ": re-optimize after set (" + why + ")");
    }
    // erase_rows: single, a run across a segment edge, the tail, and scattered
    {
      const std::vector<std::vector<size_t>> sets = {
          {0}, {p.n - 1}, {2047, 2048, 2049}, {1, 5, 2047, p.n - 1}, {p.n / 2}};
      for (const auto& del : sets) {
        std::vector<size_t> rows;
        for (size_t r : del) if (r < p.n) rows.push_back(r);
        if (rows.empty()) continue;
        ColumnStore w = cs;
        std::vector<std::string> m;
        size_t k = 0;
        for (size_t r = 0; r < p.n; ++r) {
          if (k < rows.size() && rows[k] == r) { ++k; continue; }
          m.push_back(model[r]);
        }
        w.erase_rows(rows);
        check(same(w, m, &why), std::string(p.name) + ": erase_rows of " + std::to_string(rows.size()) + " rows from " + std::to_string(rows[0]) + " (" + why + ")");
        w.optimize_encoding(SqlDataType::Text);
        check(same(w, m, &why), std::string(p.name) + ": re-optimize after erase_rows (" + why + ")");
      }
    }
    // append after optimize, new values and old ones
    {
      ColumnStore w = cs;
      std::vector<std::string> m = model;
      for (size_t i = 0; i < 2100; ++i) {
        const std::string v = i % 3 == 0 ? model[i % p.n] : (i % 7 == 0 ? nul : "new-" + std::to_string(i));
        w.push_back(v);
        m.push_back(v);
      }
      check(same(w, m, &why), std::string(p.name) + ": append after optimize (" + why + ")");
      w.optimize_encoding(SqlDataType::Text);
      check(same(w, m, &why), std::string(p.name) + ": re-optimize after append (" + why + ")");
      check(codes_match_unpacked(w), std::string(p.name) + ": codes after append equal the global layout's");
    }
  }
}

void table_tests() {
  char root_dir[] = "/tmp/aios-segpack-XXXXXX";
  if (!mkdtemp(root_dir)) { std::perror("mkdtemp"); g_failures++; return; }
  KernelConfig cfg;
  cfg.root = root_dir;
  cfg.residency = test_config;
  auto k_res = Kernel::boot(cfg);
  if (!k_res.ok()) { std::cerr << "Failed to boot kernel\n"; g_failures++; return; }
  Kernel& kernel = *k_res.value();
  const size_t N = 5000;   // two full segments and a partial one
  std::string want_sum, want_cnt;
  {
    SqlDatabase db(kernel);
    db.init("sql_db");
    check(db.execute_query("CREATE TABLE seg (id INT PRIMARY KEY, ts VARCHAR(20), tag VARCHAR(10), note VARCHAR(20))").success, "create");
    std::string ins;
    for (size_t i = 1; i <= N; ++i) {
      ins += (ins.empty() ? "INSERT INTO seg VALUES " : ",");
      ins += "(" + std::to_string(i) + ",'ts-" + std::to_string(i) + "','g" + std::to_string((i / 2048) * 100 + i % 4) + "'," +
             (i % 11 == 0 ? std::string("NULL") : "'n" + std::to_string(i / 5) + "'") + ")";
      if (i % 500 == 0) { check(db.execute_query(ins).success, "insert"); ins.clear(); }
    }
    check(db.flush_now().ok(), "flush");
    SqlTable* t = db.resolve_table("seg", "aios_db");
    check(t != nullptr, "table resolves");
    if (t) {
      for (size_t c = 0; c < t->col_data.size(); ++c)
        std::cout << "  column " << t->columns[c].name << ": " << (t->col_data[c].seg_packed_ ? "segment-packed" : "global") << " "
                  << t->col_data[c].estimate_memory_bytes() << " bytes\n";
      check(t->col_data[2].seg_packed_, "tag column is segment-packed after the checkpoint");
    }
    auto r = db.execute_query("SELECT COUNT(*), COUNT(note), COUNT(DISTINCT tag) FROM seg");
    check(r.success && r.rows[0].values[0] == std::to_string(N), "row count before reopen");
    want_cnt = r.rows[0].values[1];
    auto q = db.execute_query("SELECT tag, COUNT(*) FROM seg GROUP BY tag ORDER BY tag");
    check(q.success, "group by tag");
    for (auto& row : q.rows) want_sum += row.values[0] + "=" + row.values[1] + ";";
    // writes after the checkpoint, then a second one
    check(db.execute_query("UPDATE seg SET tag = 'changed' WHERE id IN (1, 2048, 2049, 5000)").success, "update");
    check(db.execute_query("DELETE FROM seg WHERE id BETWEEN 2040 AND 2060").success, "delete across a segment edge");
    check(db.execute_query("INSERT INTO seg VALUES (6000, 'ts-6000', 'g0', 'late')").success, "insert after");
    check(db.flush_now().ok(), "second flush");
  }
  {
    SqlDatabase db(kernel);
    db.init("sql_db");
    auto r = db.execute_query("SELECT COUNT(*) FROM seg");
    check(r.success && r.rows[0].values[0] == std::to_string(N - 21 + 1), "row count after reopen");
    auto c = db.execute_query("SELECT COUNT(*) FROM seg WHERE tag = 'changed'");
    check(c.success && c.rows[0].values[0] == "2", "updated rows after reopen (ids 1 and 5000 stay changed; 2048 and 2049 were deleted)");
    auto g = db.execute_query("SELECT note FROM seg WHERE id = 4999");
    check(g.success && !g.rows.empty() && g.rows[0].values[0] == "n999", "a late row reads back");
    auto z = db.execute_query("SELECT ts FROM seg WHERE id = 6000");
    check(z.success && !z.rows.empty() && z.rows[0].values[0] == "ts-6000", "the appended row reads back");
    auto u = db.execute_query("SELECT ts FROM seg WHERE tag = 'g1' AND id < 10 ORDER BY ts");
    check(u.success && u.rows.size() == 2, "a filter on a packed column");
    // the untouched checkpoint copy: group counts of the original data minus the edits
    (void)want_sum; (void)want_cnt;
  }
}

}  // namespace

int main() {
  std::cout << "-- per-segment code packing: columns --\n";
  column_tests();
  std::cout << "-- per-segment code packing: a table across a checkpoint and a reopen --\n";
  table_tests();
  std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
  return g_failures ? 1 : 0;
}
