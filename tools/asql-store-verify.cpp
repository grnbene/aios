// Opens an ASQL store and says what it holds, so two opens of the same data
// can be compared: before and after a re-encode, or before and after the
// store was written out and read back.
//
//   aios-asql-store-verify digest <root> <out.tsv>
//       One line per column: its rows, encoding and in-memory bytes, a hash
//       of every cell's text, a hash of every cell's numeric reading, and the
//       answers of an equality and a range probe through the query engine;
//       one line per table with the answers of a COUNT/MIN/MAX/SUM query over
//       all its columns. Two digests of the same data must be equal in every
//       field but the encoding and byte columns.
//   aios-asql-store-verify checkpoint <root> [optimize]
//       Re-encodes every column first when asked, then writes the store out.
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"

using namespace aios;
using namespace aios::userland;

namespace {

uint64_t mix(uint64_t h, uint64_t v) {
  v *= 0xbf58476d1ce4e5b9ull;
  v ^= v >> 31;
  h = (h ^ v) * 0x94d049bb133111ebull;
  return h ^ (h >> 29);
}

uint64_t hash_bytes(const char* p, size_t n) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; ++i) { h ^= (unsigned char)p[i]; h *= 0x100000001b3ull; }
  return h;
}

std::string quote_ident(const std::string& s) {
  std::string o = "`";
  for (char c : s) { if (c == '`') o += '`'; o += c; }
  return o + "`";
}

std::string quote_text(const std::string& s) {
  std::string o = "'";
  for (char c : s) {
    if (c == '\'' || c == '\\') o += '\\';
    o += c;
  }
  return o + "'";
}

uint64_t hash_result(const SqlQueryResult& r) {
  uint64_t h = r.success ? 1 : 2;
  if (!r.success) return mix(h, hash_bytes(r.error_message.data(), r.error_message.size()));
  for (const auto& row : r.rows)
    for (const auto& v : row.values) h = mix(h, hash_bytes(v.data(), v.size()));
  return h;
}

bool numeric_type(SqlDataType t) { return t == SqlDataType::Int || t == SqlDataType::Real; }

int digest(SqlDatabase& db, const char* out_path) {
  FILE* out = std::fopen(out_path, "w");
  if (!out) { std::perror(out_path); return 1; }
  SqlQueryResult tables = db.execute_local_query("SHOW ASQL STORAGE");
  if (!tables.success) { std::fprintf(stderr, "SHOW ASQL STORAGE failed: %s\n", tables.error_message.c_str()); return 1; }
  std::vector<std::string> keys;
  std::map<std::pair<std::string, std::string>, std::string> encodings;   // (table, column) -> encoding
  for (const auto& row : tables.rows) {
    if (row.values.size() < 4) continue;
    if (keys.empty() || keys.back() != row.values[0]) keys.push_back(row.values[0]);
    encodings[{row.values[0], row.values[1]}] = row.values[3];
  }
  std::map<std::string, std::pair<size_t, size_t>> by_encoding;   // encoding -> columns, bytes
  size_t total_bytes = 0, columns = 0;
  for (const std::string& key : keys) {
    const size_t dot = key.find('.');
    const std::string dbn = dot == std::string::npos ? std::string() : key.substr(0, dot);
    const std::string tn = dot == std::string::npos ? key : key.substr(dot + 1);
    const SqlTable* t = db.find_table(tn, dbn);
    if (!t) { std::fprintf(out, "T\t%s\tMISSING\n", key.c_str()); continue; }
    const std::string from = (dbn.empty() ? std::string() : quote_ident(dbn) + ".") + quote_ident(tn);
    const size_t rows = t->row_count();
    std::string agg = "SELECT COUNT(*)";
    for (size_t c = 0; c < t->columns.size() && c < t->col_data.size(); ++c) {
      const SqlColumn& col = t->columns[c];
      const ColumnStore& cs = t->col_data[c];
      uint64_t text_h = 0, num_h = 0;
      for (size_t r = 0; r < rows; ++r) {
        const std::string& v = t->cell(r, c);
        text_h = mix(text_h, cs.is_null(r) ? 0x6e756c6cull : hash_bytes(v.data(), v.size()));
        double d = 0;
        int64_t i = 0;
        const bool dok = cs.get_double(r, &d), iok = cs.get_int64(r, &i);
        uint64_t db_bits = 0;
        std::memcpy(&db_bits, &d, 8);
        num_h = mix(num_h, dok ? db_bits : 0x1111);
        num_h = mix(num_h, iok ? (uint64_t)i : 0x2222);
      }
      const std::string qc = quote_ident(col.name);
      // Probes through the query engine with the value of the middle row:
      // zone-map pruning, code comparisons and typed fast paths answer these.
      uint64_t eq_h = 0, range_h = 0;
      if (rows > 0) {
        const size_t mid = rows / 2;
        if (!cs.is_null(mid)) {
          const std::string v = t->cell(mid, c);
          const std::string lit = numeric_type(col.type) ? v : quote_text(v);
          eq_h = hash_result(db.execute_local_query("SELECT COUNT(*) FROM " + from + " WHERE " + qc + " = " + lit));
          range_h = hash_result(db.execute_local_query("SELECT COUNT(*) FROM " + from + " WHERE " + qc + " > " + lit));
        }
      }
      const size_t bytes = cs.estimate_memory_bytes();
      auto e = encodings.find({key, col.name});
      const std::string encoding = e == encodings.end() ? std::string("?") : e->second;
      by_encoding[encoding].first++;
      by_encoding[encoding].second += bytes;
      total_bytes += bytes;
      ++columns;
      std::fprintf(out, "C\t%s\t%s\t%zu\t%016" PRIx64 "\t%016" PRIx64 "\t%016" PRIx64 "\t%016" PRIx64 "\t%s\t%zu\n",
                   key.c_str(), col.name.c_str(), rows, text_h, num_h, eq_h, range_h, encoding.c_str(), bytes);
      agg += ", COUNT(" + qc + "), MIN(" + qc + "), MAX(" + qc + ")";
      if (numeric_type(col.type)) agg += ", SUM(" + qc + ")";
    }
    SqlQueryResult a = db.execute_local_query(agg + " FROM " + from);
    std::fprintf(out, "T\t%s\t%zu\t%016" PRIx64 "\n", key.c_str(), rows, hash_result(a));
  }
  std::fclose(out);
  std::printf("%zu tables, %zu columns, %.1f MB in memory\n", keys.size(), columns, total_bytes / 1048576.0);
  for (const auto& kv : by_encoding)
    std::printf("  %-14s %6zu columns %10.1f MB\n", kv.first.c_str(), kv.second.first, kv.second.second / 1048576.0);
  return 0;
}

// Where a dictionary column's bytes are: the entries' text, their offsets,
// the write index, the numeric caches, the sorted view, the codes.
int breakdown(SqlDatabase& db) {
  SqlQueryResult tables = db.execute_local_query("SHOW ASQL STORAGE");
  std::vector<std::string> keys;
  for (const auto& row : tables.rows)
    if (!row.values.empty() && (keys.empty() || keys.back() != row.values[0])) keys.push_back(row.values[0]);
  size_t codes32 = 0, codes32_fit24 = 0;
  size_t coded_plain[5] = {}, coded_held[5] = {}, coded_cols[5] = {};
  size_t text = 0, offs = 0, index = 0, caches = 0, sorted = 0, codes = 0, zone = 0, zone_str = 0, other = 0, cols = 0;
  size_t null_rows = 0, rows_all = 0, sparse_cols = 0, sparse_bytes = 0, all_bytes = 0;
  for (const std::string& key : keys) {
    const size_t dot = key.find('.');
    const SqlTable* t = db.find_table(dot == std::string::npos ? key : key.substr(dot + 1), dot == std::string::npos ? "" : key.substr(0, dot));
    if (!t) continue;
    for (const ColumnStore& cs : t->col_data) {
      size_t nulls = 0;
      all_bytes += cs.estimate_memory_bytes();
      for (size_t r = 0; r < cs.size(); ++r) nulls += cs.is_null(r);
      null_rows += nulls;
      rows_all += cs.size();
      if (cs.size() >= 2048 && nulls * 10 >= cs.size() * 9) { ++sparse_cols; sparse_bytes += cs.estimate_memory_bytes(); }
      for (const auto& z : cs.zone_maps) {
        if (z.min_str.capacity() > 15) zone_str += z.min_str.capacity();   // past the inline buffer
        if (z.max_str.capacity() > 15) zone_str += z.max_str.capacity();
      }
      zone += cs.zone_maps.capacity() * sizeof(SegmentZoneMap);
      if (cs.encoding != ColumnEncoding::DictBitPacked && cs.encoding != ColumnEncoding::RLE) { other += cs.estimate_memory_bytes(); continue; }
      ++cols;
      const DictArena& d = cs.shared_dict ? cs.shared_dict->dict : cs.dict;
      text += d.byte_size();
      if (d.coded()) {   // what coding saved, by the entries' average length
        size_t plain = 0;
        for (size_t k = 0; k < d.coded_count(); ++k) plain += d.view(k).size();
        const size_t avg = plain / std::max<size_t>(1, d.coded_count());
        const int b = avg < 16 ? 0 : avg < 24 ? 1 : avg < 32 ? 2 : avg < 64 ? 3 : 4;
        coded_plain[b] += plain;
        coded_held[b] += d.coded_bytes().size();
        coded_cols[b]++;
      }
      offs += d.memory_bytes() - d.byte_size();
      index += cs.id_index.capacity() * sizeof(uint32_t);
      caches += cs.dict_doubles.capacity() * 8 + cs.dict_doubles_valid.capacity() + cs.dict_ints.capacity() * 8 + cs.dict_ints_valid.capacity();
      sorted += cs.sorted_dict_indices.capacity() * 4;
      codes += cs.data32.capacity() * 4 + cs.packed_data.capacity() + cs.rle_codes_data.capacity() + cs.rle_run_lengths.capacity() * 2 +
               (cs.rle_escape_runs.capacity() + cs.rle_escape_lengths.capacity() + cs.rle_seg_base_rows.capacity()) * 4;
      if (cs.encoding == ColumnEncoding::DictBitPacked && cs.bit_width_ == 32) {
        codes32 += cs.data32.capacity() * 4;
        if (d.size() <= (1u << 24)) codes32_fit24 += cs.data32.capacity() * 4;
      }
    }
  }
  auto mb = [](size_t b) { return b / 1048576.0; };
  std::printf("all columns %.1f MB (estimate_memory_bytes, as loaded)\n", mb(all_bytes));
  std::printf("dictionary columns: %zu\n  entry text %.1f MB\n  offsets/slack %.1f MB\n  write index %.1f MB\n"
              "  numeric caches %.1f MB\n  sorted view %.1f MB\n  codes %.1f MB\nother columns %.1f MB\n"
              "zone maps %.1f MB + %.1f MB of heap strings\nNULL cells %zu of %zu; columns >= 90%% NULL (2048+ rows): %zu holding %.1f MB\n",
              cols, mb(text), mb(offs), mb(index), mb(caches), mb(sorted), mb(codes), mb(other), mb(zone), mb(zone_str), null_rows,
              rows_all, sparse_cols, mb(sparse_bytes));
  const char* bucket[5] = {"<16", "16-23", "24-31", "32-63", "64+"};
  for (int b = 0; b < 5; ++b)
    if (coded_cols[b])
      std::printf("coded dictionaries, entries averaging %s bytes: %zu columns, %.1f MB plain held in %.1f MB\n", bucket[b], coded_cols[b],
                  mb(coded_plain[b]), mb(coded_held[b]));
  std::printf("32-bit dictionary codes %.1f MB, of them %.1f MB for dictionaries of at most 2^24 entries\n", mb(codes32), mb(codes32_fit24));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: aios-asql-store-verify digest <root> <out.tsv> | checkpoint <root> [optimize]\n");
    return 2;
  }
  const std::string mode = argv[1];
  KernelConfig cfg;
  cfg.root = argv[2];
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::fprintf(stderr, "boot failed: %s\n", k.error().message.c_str()); return 1; }
  SqlDatabase db(**k);
  if (auto r = db.init("sql_db"); !r.ok()) { std::fprintf(stderr, "init failed: %s\n", r.error().message.c_str()); return 1; }
  if (mode == "digest") {
    if (argc < 4) { std::fprintf(stderr, "digest needs an output file\n"); return 2; }
    return digest(db, argv[3]);
  }
  if (mode == "breakdown") return breakdown(db);
  // bench <root> <file>: each query of the file (one a line) once to warm,
  // then three times; the best time and a hash of the answer.
  if (mode == "bench") {
    if (argc < 4) return 2;
    FILE* f = std::fopen(argv[3], "r");
    if (!f) { std::perror(argv[3]); return 1; }
    char line[8192];
    while (std::fgets(line, sizeof line, f)) {
      std::string q(line);
      while (!q.empty() && (q.back() == '\n' || q.back() == '\r')) q.pop_back();
      if (q.empty() || q[0] == '#') continue;
      uint64_t h = hash_result(db.execute_local_query(q));
      double best = 1e300;
      for (int i = 0; i < 3; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        SqlQueryResult r = db.execute_local_query(q);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ms < best) best = ms;
        if (hash_result(r) != h) h = 0;   // not the same answer twice
      }
      std::printf("%10.2f ms  %016" PRIx64 "  %s\n", best, h, q.substr(0, 110).c_str());
      std::fflush(stdout);
    }
    std::fclose(f);
    return 0;
  }
  if (mode == "query") {   // aios-asql-store-verify query <root> <sql>: its rows, tab-separated
    if (argc < 4) return 2;
    SqlQueryResult r = db.execute_local_query(argv[3]);
    if (!r.success) { std::printf("ERROR %s\n", r.error_message.c_str()); return 1; }
    for (const auto& row : r.rows) {
      for (size_t i = 0; i < row.values.size(); ++i) std::printf("%s%s", i ? "\t" : "", row.values[i].c_str());
      std::printf("\n");
    }
    return 0;
  }
  // Checkpoints until the storage maintenance they run (narrowing codes,
  // coding dictionaries) has nothing left: the store as it ends up.
  if (mode == "converge") {
    size_t prev = SIZE_MAX;
    for (int i = 0; i < 2000; ++i) {
      auto r = db.flush_now();
      if (!r.ok()) { std::fprintf(stderr, "checkpoint failed: %s\n", r.error().message.c_str()); return 1; }
      size_t total = 0;
      SqlQueryResult st = db.execute_local_query("SHOW ASQL STORAGE");
      for (const auto& row : st.rows) if (row.values.size() > 6) total += std::strtoull(row.values[6].c_str(), nullptr, 10);
      std::printf("pass %d: %.1f MB in memory\n", i + 1, total / 1048576.0);
      std::fflush(stdout);
      if (total >= prev) break;
      prev = total;
    }
    return 0;
  }
  if (mode == "checkpoint") {
    if (argc > 3 && std::string(argv[3]) == "optimize") db.optimize_all_tables("");
    auto r = db.flush_now();
    if (!r.ok()) { std::fprintf(stderr, "checkpoint failed: %s\n", r.error().message.c_str()); return 1; }
    std::printf("checkpoint written\n");
    return 0;
  }
  std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
  return 2;
}
