// BlockCompressed keeps its row lengths inside the compressed payload (no offset array), a block written before that
// still reads, and FSST's row boundaries are 16-bit distances from a base every 16th boundary (wide when one does not
// fit). Rows read back exactly across writes, a checkpoint and a reopen.
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

const char* enc_name(ColumnEncoding e) {
  switch (e) {
    case ColumnEncoding::BlockCompressed: return "BlockCompressed";
    case ColumnEncoding::FSST: return "FSST";
    case ColumnEncoding::RawArena: return "RawArena";
    case ColumnEncoding::DictBitPacked: return "DictBitPacked";
    case ColumnEncoding::RLE: return "RLE";
    default: return "other";
  }
}

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

std::string text(size_t i, size_t len) {
  std::string s = "row-" + std::to_string(i) + "-";
  uint64_t x = i * 0x9E3779B97F4A7C15ull + 12345;
  while (s.size() < len) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; s.push_back("abcdefghijklmnopqrstuvwxyz0123456789 ./:-"[x % 40]); }
  return s;
}

// A column as FSST holds it, built the way optimize_encoding's Rule 8 builds one (which Rule 7 now pre-empts for any
// text it would reach: FSST columns come from older checkpoints).
ColumnStore make_fsst(const std::vector<std::string>& model) {
  ColumnStore cs;
  for (const auto& v : model) cs.push_back(v);
  cs.optimize_encoding(SqlDataType::Text);
  std::vector<std::string> samples;
  for (const auto& v : model) if (!sql_is_null(v) && !v.empty() && samples.size() < 2048) samples.push_back(v);
  cs.fsst_table.train(samples);
  cs.fsst_data.clear();
  std::vector<uint32_t> bounds(model.size() + 1, 0);
  for (size_t r = 0; r < model.size(); ++r) {
    if (!sql_is_null(model[r])) {
      std::vector<uint8_t> comp = cs.fsst_table.compress(model[r]);
      cs.fsst_data.insert(cs.fsst_data.end(), comp.begin(), comp.end());
    }
    bounds[r + 1] = (uint32_t)cs.fsst_data.size();
  }
  cs.encoding = ColumnEncoding::FSST;
  cs.fsst_set_boundaries(std::move(bounds));
  cs.fsst_table.drop_index();
  cs.dict.clear();
  cs.id_index.clear(); cs.id_tag.clear(); cs.id_index_count = 0;
  cs.packed_data.clear(); cs.data32.clear(); cs.bit_width_ = 0;
  cs.compressed_blocks.clear();
  cs.seg_packed_ = false; cs.seg_hdr.clear(); cs.seg_tab.clear();
  cs.build_zone_maps();
  return cs;
}

struct Pattern {
  const char* name;
  size_t n;
  std::function<std::string(size_t)> value;
};

void fsst_column_tests() {
  const std::string nul = sql_null();
  for (int variant = 0; variant < 2; ++variant) {
    const size_t n = 1500;
    std::vector<std::string> model(n);
    for (size_t i = 0; i < n; ++i) {
      // variant 1: every 16th row a 250 KB string (still over 64 KB once FSST has coded it), so a boundary distance cannot fit 16 bits and the column goes wide
      model[i] = (variant == 1 && i % 16 == 5) ? text(i, 250000) : (i % 11 == 0 ? nul : (i % 13 == 0 ? std::string() : text(i, 30 + i % 40)));
    }
    ColumnStore cs = make_fsst(model);
    std::string why;
    const std::string tag = std::string("fsst/") + (variant ? "wide" : "compact");
    std::cout << "  " << tag << ": " << n << " rows, " << (cs.fsst_offsets.empty() ? "compact" : "wide") << " boundaries, " << cs.estimate_memory_bytes() << " bytes\n";
    check(cs.fsst_offsets.empty() == (variant == 0), tag + ": the boundary form the data calls for");
    check(same(cs, model, &why), tag + ": reads back (" + why + ")");
    {
      ColumnStore w = cs;
      std::vector<std::string> m = model;
      for (size_t r : {size_t(0), size_t(15), size_t(16), n - 1, n / 2}) { const std::string v = "set-" + std::to_string(r); w.set(r, v); m[r] = v; }
      check(same(w, m, &why), tag + ": set() (" + why + ")");
    }
    for (const std::vector<size_t>& del : std::vector<std::vector<size_t>>{{0}, {n - 1}, {14, 15, 16, 17}, {1, 5, 100, n - 1}}) {
      ColumnStore w = cs;
      std::vector<std::string> m;
      size_t k = 0;
      for (size_t r = 0; r < n; ++r) { if (k < del.size() && del[k] == r) { ++k; continue; } m.push_back(model[r]); }
      w.erase_rows(del);
      check(same(w, m, &why), tag + ": erase_rows from " + std::to_string(del[0]) + " (" + why + ")");
    }
    {
      ColumnStore w = cs;
      std::vector<std::string> m = model;
      for (size_t i = 0; i < 300; ++i) {
        const std::string v = i % 7 == 0 ? nul : (i % 29 == 0 ? text(i, 250000) : text(900000 + i, 25 + i % 30));
        w.push_back(v);
        m.push_back(v);
      }
      check(w.encoding == ColumnEncoding::FSST, tag + ": appends keep the encoding");
      check(same(w, m, &why), tag + ": append (the symbol index is rebuilt for compress) (" + why + ")");
    }
  }
}

void column_tests() {
  const std::string nul = sql_null();
  const std::vector<Pattern> patterns = {
      {"long-unique", 3000, [](size_t i) { return text(i, 60 + i % 40); }},
      {"long-with-nulls-and-empties", 2600, [&](size_t i) { return i % 11 == 0 ? nul : (i % 13 == 0 ? std::string() : text(i, 40 + i % 90)); }},
      {"short-unique", 70000, [](size_t i) { return "k" + std::to_string(i * 7919) + "z"; }},
      {"very-long-rows", 700, [](size_t i) { return text(i, 300 + (i % 7) * 900); }},
  };
  for (const Pattern& p : patterns) {
    std::vector<std::string> model(p.n);
    for (size_t i = 0; i < p.n; ++i) model[i] = p.value(i);
    ColumnStore cs;
    for (const auto& v : model) cs.push_back(v);
    cs.optimize_encoding(SqlDataType::Text);
    std::string why;
    size_t offset_bytes = 0;
    for (const auto& b : cs.compressed_blocks) offset_bytes += b.offsets.capacity() * 4;
    offset_bytes += cs.fsst_offsets.capacity() * 4 + cs.fsst_off_base.capacity() * 4 + cs.fsst_off_rel.capacity() * 2;
    std::cout << "  " << p.name << ": " << p.n << " rows, " << enc_name(cs.encoding) << ", " << cs.estimate_memory_bytes() << " bytes, "
              << offset_bytes << " bytes of offsets (" << (double)offset_bytes / (double)p.n << " per row; was 4)\n";
    check(same(cs, model, &why), std::string(p.name) + ": reads back after optimize (" + why + ")");
    if (cs.encoding == ColumnEncoding::BlockCompressed) {
      bool none = true;
      for (const auto& b : cs.compressed_blocks) if (!b.offsets.empty() || b.raw_size == 0) none = false;
      check(none, std::string(p.name) + ": no block keeps an offset array");
    }
    if (cs.encoding == ColumnEncoding::FSST) check(cs.fsst_boundaries() == p.n + 1, std::string(p.name) + ": one boundary per row plus one");

    // set at the block edges, erase across them, append
    {
      ColumnStore w = cs;
      std::vector<std::string> m = model;
      const size_t rows[] = {0, 511, 512, 1023, p.n - 1, p.n / 2};
      for (size_t r : rows) {
        if (r >= p.n) continue;
        const std::string v = "set-" + std::to_string(r) + std::string(r % 3 ? "" : "-padded-padded-padded");
        w.set(r, v);
        m[r] = v;
      }
      w.set(p.n / 3, nul);
      m[p.n / 3] = nul;
      check(same(w, m, &why), std::string(p.name) + ": set() after optimize (" + why + ")");
      w.optimize_encoding(SqlDataType::Text);
      check(same(w, m, &why), std::string(p.name) + ": re-optimize after set (" + why + ")");
    }
    {
      const std::vector<std::vector<size_t>> sets = {{0}, {p.n - 1}, {510, 511, 512, 513}, {1, 5, 1023, p.n - 1}};
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
      }
    }
    {
      ColumnStore w = cs;
      std::vector<std::string> m = model;
      for (size_t i = 0; i < 700; ++i) {
        const std::string v = i % 5 == 0 ? model[i % p.n] : (i % 7 == 0 ? nul : text(100000 + i, 30 + i % 50));
        w.push_back(v);
        m.push_back(v);
      }
      check(same(w, m, &why), std::string(p.name) + ": append after optimize (" + why + ")");
    }
  }
}

// FSST's boundary forms, directly: compact while every distance fits, wide the moment one does not, and the same
// answers either way.
void boundary_tests() {
  {
    ColumnStore cs;
    std::vector<uint32_t> want = {0};
    cs.fsst_push_boundary(0);
    uint32_t off = 0;
    for (size_t i = 1; i <= 5000; ++i) { off += 1 + (i * 37) % 200; want.push_back(off); cs.fsst_push_boundary(off); }
    bool ok = cs.fsst_boundaries() == want.size() && cs.fsst_offsets.empty();
    for (size_t i = 0; ok && i < want.size(); ++i) ok = cs.fsst_off(i) == want[i];
    check(ok, "boundaries stay compact while distances fit");
    check(cs.fsst_off_rel.size() * 2 + cs.fsst_off_base.size() * 4 < want.size() * 3, "compact form is under 3 bytes a row");
    // one gap past 16 bits widens the column, and nothing earlier changes
    off += 70000; want.push_back(off); cs.fsst_push_boundary(off);
    for (size_t i = 0; i < 40; ++i) { off += 9; want.push_back(off); cs.fsst_push_boundary(off); }
    ok = cs.fsst_boundaries() == want.size() && !cs.fsst_offsets.empty() && cs.fsst_off_rel.empty();
    for (size_t i = 0; ok && i < want.size(); ++i) ok = cs.fsst_off(i) == want[i];
    check(ok, "one gap past 16 bits widens the column and every boundary still reads");
    // set_boundaries picks the form from the data
    std::vector<uint32_t> narrow = {0, 5, 9, 9, 30, 31};
    cs.fsst_set_boundaries(std::vector<uint32_t>(narrow));
    ok = cs.fsst_offsets.empty() && cs.fsst_boundaries() == narrow.size();
    for (size_t i = 0; ok && i < narrow.size(); ++i) ok = cs.fsst_off(i) == narrow[i];
    check(ok, "set_boundaries takes the compact form when it fits");
    std::vector<uint32_t> wide = {0, 10, 70000, 70010, 70011};
    cs.fsst_set_boundaries(std::vector<uint32_t>(wide));
    ok = !cs.fsst_offsets.empty() && cs.fsst_all_boundaries() == wide;
    check(ok, "set_boundaries takes the wide form when a distance does not fit");
    cs.fsst_set_boundaries({});
    check(cs.fsst_boundaries() == 0, "no boundaries");
  }
}

// A block written before the lengths moved into the payload: its rows' bytes alone, cut by CSR offsets. Built here as a
// literal-only LZ4 block, which any LZ4 decoder takes.
void legacy_block_test() {
  const std::vector<std::string> rows = {"alpha", "", "gamma gamma gamma", "delta"};
  std::string raw;
  ColumnStore::CompressedBlock blk;
  blk.start_row = 0;
  blk.num_rows = rows.size();
  blk.offsets.push_back(0);
  for (const auto& r : rows) { raw += r; blk.offsets.push_back((uint32_t)raw.size()); }
  size_t n = raw.size();
  std::vector<uint8_t> lz;
  lz.push_back((uint8_t)((n >= 15 ? 15 : n) << 4));
  if (n >= 15) { size_t rest = n - 15; while (rest >= 255) { lz.push_back(255); rest -= 255; } lz.push_back((uint8_t)rest); }
  lz.insert(lz.end(), raw.begin(), raw.end());
  blk.compressed_bytes = lz;
  blk.raw_size = (uint32_t)raw.size();
  ColumnStore cs;
  for (const auto& r : rows) cs.push_back(r);
  cs.optimize_encoding(SqlDataType::Text);
  // put the legacy block in place of whatever the column chose
  cs.encoding = ColumnEncoding::BlockCompressed;
  cs.compressed_blocks.clear();
  cs.compressed_blocks.push_back(blk);
  cs.row_count_ = rows.size();
  cs.dict.clear();
  cs.block_lru_cache.clear();
  bool ok = true;
  for (size_t r = 0; r < rows.size(); ++r) ok = ok && cs.get(r) == rows[r];
  check(ok, "a block with CSR offsets still reads");
}

void table_tests() {
  char root_dir[] = "/tmp/aios-offsets-XXXXXX";
  if (!mkdtemp(root_dir)) { std::perror("mkdtemp"); g_failures++; return; }
  KernelConfig cfg;
  cfg.root = root_dir;
  cfg.residency = test_config;
  auto k_res = Kernel::boot(cfg);
  if (!k_res.ok()) { std::cerr << "Failed to boot kernel\n"; g_failures++; return; }
  Kernel& kernel = *k_res.value();
  const size_t N = 70000;   // past 65536 distinct values: a dictionary no longer pays and the text goes to blocks
  {
    SqlDatabase db(kernel);
    db.init("sql_db");
    check(db.execute_query("CREATE TABLE notes (id INT PRIMARY KEY, body VARCHAR(255), tag VARCHAR(255), old_fsst VARCHAR(255))").success, "create");
    std::string ins;
    for (size_t i = 1; i <= N; ++i) {
      ins += (ins.empty() ? "INSERT INTO notes VALUES " : ",");
      ins += "(" + std::to_string(i) + ",'" + text(i, 70 + i % 50) + "'," + (i % 9 == 0 ? std::string("NULL") : "'" + text(i + 77777, 20 + i % 30) + "'") + ",'" + text(i + 5, 18 + i % 20) + "')";
      if (i % 500 == 0) { check(db.execute_query(ins).success, "insert"); ins.clear(); }
    }
    SqlTable* t = db.resolve_table("notes", "aios_db");
    check(t != nullptr, "table resolves");
    if (t) {
      t->optimize_storage();
      // the third text column is re-encoded as FSST, as a store written by an older build holds it
      std::vector<std::string> fs(N);
      for (size_t r = 0; r < N; ++r) fs[r] = t->col_data[3].is_null(r) ? sql_null() : t->col_data[3].get(r);
      t->col_data[3] = make_fsst(fs);
      t->mutation_seq = sql_next_mutation_seq();
      t->layout_seq = t->mutation_seq;
      for (size_t c = 1; c < 4; ++c) std::cout << "  column " << t->columns[c].name << ": " << enc_name(t->col_data[c].encoding) << ", " << t->col_data[c].estimate_memory_bytes() << " bytes\n";
      check(t->col_data[1].encoding == ColumnEncoding::BlockCompressed, "the body column is blocks");
    }
    check(db.flush_now().ok(), "flush");
  }
  {
    SqlDatabase db(kernel);
    db.init("sql_db");
    auto r = db.execute_query("SELECT body FROM notes WHERE id = 1234");
    check(r.success && !r.rows.empty() && r.rows[0].values[0] == text(1234, 70 + 1234 % 50), "a row reads back after reopen");
    auto c = db.execute_query("SELECT COUNT(*), COUNT(tag) FROM notes");
    check(c.success && c.rows[0].values[0] == std::to_string(N) && c.rows[0].values[1] == std::to_string(N - N / 9), "counts after reopen");
    auto t = db.execute_query("SELECT tag FROM notes WHERE id = 2999");
    check(t.success && !t.rows.empty() && t.rows[0].values[0] == text(2999 + 77777, 20 + 2999 % 30), "a second column reads back after reopen");
    auto f = db.execute_query("SELECT old_fsst FROM notes WHERE id = 54321");
    check(f.success && !f.rows.empty() && f.rows[0].values[0] == text(54321 + 5, 18 + 54321 % 20), "an FSST column reads back after reopen");
    SqlTable* t2 = db.resolve_table("notes", "aios_db");
    if (t2) std::cout << "  after reopen: body " << enc_name(t2->col_data[1].encoding) << ", old_fsst " << enc_name(t2->col_data[3].encoding)
                      << (t2->col_data[3].fsst_offsets.empty() ? " (compact boundaries)" : " (wide boundaries)") << "\n";
    check(t2 && t2->col_data[3].encoding == ColumnEncoding::FSST, "the FSST column is still FSST after reopen");
    check(db.execute_query("INSERT INTO notes VALUES (90001, 'late row', 'late tag', 'late fsst')").success, "insert after reopen");
    auto l = db.execute_query("SELECT body, tag, old_fsst FROM notes WHERE id = 90001");
    check(l.success && !l.rows.empty() && l.rows[0].values[0] == "late row" && l.rows[0].values[1] == "late tag" && l.rows[0].values[2] == "late fsst", "the appended row reads back");
  }
}

}  // namespace


// Zone maps refreshed while a dictionary's numeric cache is stale (the dictionary grew since it was read as numbers)
// read the segment's own values, not the whole dictionary: the stats must be the same as through the cache.
void zone_refresh_tests() {
  const std::string nul = sql_null();
  const std::vector<std::string> odd = {"12", "-7", "3.25", "1e3", "12abc", " 42", "abc", "", nul, "0x1F", "-0", "9223372036854775807",
                                        "1.5e-3", "  -2.75 ", "NULL", "7.000", "+8"};
  ColumnStore cs;
  for (size_t i = 0; i < 2048 * 3; ++i) cs.push_back(odd[i % odd.size()] + (i % 5 == 0 ? std::string() : std::to_string(i % 97)));
  cs.optimize_encoding(SqlDataType::Text);
  check(cs.encoding == ColumnEncoding::DictBitPacked, std::string("zone refresh: column is dictionary-coded (") + enc_name(cs.encoding) + ")");
  cs.build_zone_maps();
  double d0 = 0;
  cs.get_double(0, &d0);   // the numeric cache built, as reads build it
  // rows written in place, again and again: the old values stay in the dictionary until a checkpoint compacts it,
  // and compacting drops the numeric cache - the state a checkpoint's refresh met on chi
  for (int round = 0; round < 3; ++round)
    for (size_t r = 0; r < cs.size(); ++r)
      cs.set(r, r % 4 == 0 ? std::to_string(round * 100000 + (int)r) + ".25" : (r % 4 == 1 ? "v" + std::to_string(round) + "-" + std::to_string(r) : odd[(r + round) % odd.size()] + std::to_string(round)));
  check(cs.compact_dictionary(), "zone refresh: the dictionary was compacted");
  for (auto& z : cs.zone_maps) z.has_stats = false;   // what a write leaves for the checkpoint to refresh
  const size_t to = cs.size() - 1;
  ColumnStore cached = cs;
  double dd = 0; int64_t ii = 0;
  cached.get_double(0, &dd); cached.get_int64(0, &ii);   // the caches made whole: the path the refresh took before
  check(cs.dict_doubles.size() < cs.dict.size(), "zone refresh: the dictionary has outgrown its numeric cache");
  cs.refresh_zone_maps(0, to);
  cached.refresh_zone_maps(0, to);
  check(cs.dict_doubles.size() < cs.dict.size(), "zone refresh: refreshing did not build the whole cache");
  check(cs.zone_maps.size() == cached.zone_maps.size(), "zone refresh: as many segments");
  for (size_t sg = 0; sg < cs.zone_maps.size() && sg < cached.zone_maps.size(); ++sg) {
    const SegmentZoneMap &a = cs.zone_maps[sg], &b = cached.zone_maps[sg];
    const bool same_stats = a.min_int64 == b.min_int64 && a.max_int64 == b.max_int64 && a.min_double == b.min_double &&
                            a.max_double == b.max_double && a.min_str == b.min_str && a.max_str == b.max_str &&
                            a.bloom_filter == b.bloom_filter && a.null_count == b.null_count && a.all_null == b.all_null &&
                            a.has_stats == b.has_stats && a.str_cut == b.str_cut;
    check(same_stats, "zone refresh: segment " + std::to_string(sg) + " has the same stats either way");
  }
}

int main() {
  std::cout << "-- zone maps refreshed past a stale dictionary cache --\n";
  zone_refresh_tests();
  std::cout << "-- BlockCompressed: columns --\n";
  column_tests();
  std::cout << "-- FSST: columns --\n";
  fsst_column_tests();
  std::cout << "-- FSST row boundaries --\n";
  boundary_tests();
  std::cout << "-- a block from before --\n";
  legacy_block_test();
  std::cout << "-- a table across a checkpoint and a reopen --\n";
  table_tests();
  std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
  return g_failures ? 1 : 0;
}
