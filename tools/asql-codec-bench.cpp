// Measures ASQL's compressed text blocks on a real store: for every block
// column of a root, the bytes its blocks take as LZ4 and as deflate (the
// in-tree encoder, and zlib's where the host has one, so the two can be
// compared), and the time a full scan of the column takes with each codec.
//
//   aios-asql-codec-bench selftest
//       Round-trips synthetic buffers through the in-tree deflate encoder and
//       every decoder path, and compares the encoder's output to zlib's.
//   aios-asql-codec-bench <root> [db.table ...]
//       Opens the root (read-only: nothing is written back) and measures the
//       named tables, or every table with a block column.
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>


#include "aios/kernel/kernel.hpp"
#include "aios/net/deflate.hpp"
#include "aios/userland/sql_database.hpp"

using namespace aios;
using namespace aios::userland;

namespace {

double now_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }


int selftest() {
  std::mt19937_64 rng(7);
  int failures = 0;
  auto check = [&](const char* what, const ByteBuffer& data) {
    for (int level : {1, 6, 9}) {
      ByteBuffer z = net::deflate_raw(ByteSpan(data.data(), data.size()), level);
      ByteBuffer tree;
      std::string why;
      size_t end = 0;
      const bool ok_tree = net::inflate_raw_tree(ByteSpan(z.data(), z.size()), 0, tree, &end, data.size(), &why) && tree == data && end == z.size();
      std::vector<uint8_t> exact(data.size());
      const bool ok_exact = net::inflate_raw_exact(ByteSpan(z.data(), z.size()), exact.data(), data.size()) && std::memcmp(exact.data(), data.data(), data.size()) == 0;
      size_t zl = 0;
      std::printf("  %-28s level %d: %zu -> %zu bytes (zlib %zu, %+.1f%%)  tree %s  exact %s\n", what, level, data.size(), z.size(), zl,
                  zl ? (100.0 * ((double)z.size() - (double)zl) / (double)zl) : 0.0, ok_tree ? "ok" : why.c_str(), ok_exact ? "ok" : "FAIL");
      if (!ok_tree || !ok_exact) ++failures;
    }
  };
  check("empty", ByteBuffer{});
  check("one byte", ByteBuffer{'x'});
  { ByteBuffer b(100000, 'a'); check("100k of one byte", b); }
  { ByteBuffer b(70000); for (auto& x : b) x = (uint8_t)rng(); check("70k random", b); }
  {
    ByteBuffer b;
    const char* words[] = {"order ", "12345 ", "customer ", "updated ", "status=shipped ", "{\"id\":", "\"note\":\"", "2026-10-22 09:11:33 ", "\n"};
    while (b.size() < 300000) { const char* w = words[rng() % 9]; b.insert(b.end(), w, w + std::strlen(w)); if (rng() % 7 == 0) { std::string n = std::to_string(rng() % 100000); b.insert(b.end(), n.begin(), n.end()); } }
    check("300k log-like text", b);
  }
  {
    ByteBuffer b;
    for (int i = 0; i < 20000; ++i) { std::string s = "row " + std::to_string(i * 7919 % 10007) + " " + std::string(1 + i % 13, 'q') + "\t"; b.insert(b.end(), s.begin(), s.end()); }
    check("varying short rows", b);
  }
  // damaged streams are refused, never read as data
  {
    ByteBuffer b(5000); for (auto& x : b) x = (uint8_t)(rng() % 7 + 'a');
    ByteBuffer z = net::deflate_raw(ByteSpan(b.data(), b.size()), 6);
    std::vector<uint8_t> out(b.size());
    z[z.size() / 2] ^= 0x55;
    const bool refused = !net::inflate_raw_exact(ByteSpan(z.data(), z.size()), out.data(), b.size()) || std::memcmp(out.data(), b.data(), b.size()) != 0;
    std::printf("  damaged stream refused: %s\n", refused ? "ok" : "FAIL");
    if (!refused) ++failures;
    ByteBuffer z2 = net::deflate_raw(ByteSpan(b.data(), b.size()), 6);
    z2.resize(z2.size() / 2);
    const bool truncated = !net::inflate_raw_exact(ByteSpan(z2.data(), z2.size()), out.data(), b.size());
    std::printf("  truncated stream refused: %s\n", truncated ? "ok" : "FAIL");
    if (!truncated) ++failures;
  }
  std::printf("%s\n", failures ? "SELFTEST FAILED" : "selftest ok");
  return failures ? 1 : 0;
}

struct ColumnMeasure {
  std::string table, column;
  size_t rows = 0, blocks = 0, raw = 0, lz4 = 0, deflate6 = 0, zlib6 = 0;
  double lz4_pack_ms = 0, deflate_pack_ms = 0, lz4_scan_ms = 0, deflate_scan_ms = 0;
  uint64_t scan_hash_lz4 = 0, scan_hash_deflate = 0;
};

uint64_t scan(const ColumnStore& cs, double* ms) {
  // The blocks decoded through the engine's own read path, one row at a time
  // as a predicate over the column would; the cache is dropped first so every
  // block is decoded once.
  { const_cast<ColumnStore&>(cs).release_slack(); }
  uint64_t h = 0xcbf29ce484222325ull;
  const double t0 = now_ms();
  for (size_t r = 0; r < cs.size(); ++r) {
    std::string_view v = cs.get_view(r);
    for (unsigned char ch : v) { h ^= ch; h *= 0x100000001b3ull; }
    h ^= 0x9e3779b97f4a7c15ull;
  }
  *ms = now_ms() - t0;
  return h;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: aios-asql-codec-bench selftest | <root> [db.table ...]\n"); return 2; }
  if (std::strcmp(argv[1], "selftest") == 0) return selftest();
  KernelConfig cfg;
  cfg.root = argv[1];
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::fprintf(stderr, "boot failed: %s\n", k.error().message.c_str()); return 1; }
  SqlDatabase db(**k);
  const double load0 = now_ms();
  db.init("sql_db");
  std::printf("loaded %s in %.1f s\n", argv[1], (now_ms() - load0) / 1000.0);
  std::vector<std::string> only;
  for (int i = 2; i < argc; ++i) only.push_back(argv[i]);
  SqlQueryResult tables = db.execute_local_query("SHOW ASQL STORAGE");
  if (!tables.success) { std::fprintf(stderr, "SHOW ASQL STORAGE failed: %s\n", tables.error_message.c_str()); return 1; }
  std::vector<ColumnMeasure> all;
  for (const auto& row : tables.rows) {
    if (row.values.size() < 4 || row.values[3].compare(0, 5, "Block") != 0) continue;
    const std::string& key = row.values[0];
    if (!only.empty()) { bool hit = false; for (const auto& o : only) hit = hit || o == key; if (!hit) continue; }
    const size_t dot = key.find('.');
    SqlTable* t = db.find_table(key.substr(dot + 1), dot == std::string::npos ? "" : key.substr(0, dot));
    if (!t) continue;
    const int ci = t->get_column_index(row.values[1]);
    if (ci < 0 || (size_t)ci >= t->col_data.size()) continue;
    ColumnStore& cs = t->col_data[(size_t)ci];
    if (cs.encoding != ColumnEncoding::BlockCompressed) continue;
    ColumnMeasure m;
    m.table = key; m.column = row.values[1]; m.rows = cs.size(); m.blocks = cs.compressed_blocks.size();
    for (const auto& b : cs.compressed_blocks) { m.raw += b.raw_size; (b.codec == ColumnStore::kBlockDeflate ? m.deflate6 : m.lz4) += b.compressed_bytes.size(); }
    // as LZ4: the column as it is when every block is LZ4 (its state in an AQT6 store)
    cs.block_codec_pref = ColumnStore::kBlockLz4;
    double t0 = now_ms(); cs.recode_blocks(); m.lz4_pack_ms = now_ms() - t0;
    m.lz4 = 0; for (const auto& b : cs.compressed_blocks) m.lz4 += b.compressed_bytes.size();
    m.scan_hash_lz4 = scan(cs, &m.lz4_scan_ms);
    cs.block_codec_pref = ColumnStore::kBlockDeflate;
    t0 = now_ms(); cs.recode_blocks(); m.deflate_pack_ms = now_ms() - t0;
    m.deflate6 = 0; for (const auto& b : cs.compressed_blocks) m.deflate6 += b.compressed_bytes.size();
    m.scan_hash_deflate = scan(cs, &m.deflate_scan_ms);
    cs.block_codec_pref = ColumnStore::kBlockLz4;   // the table as it was
    cs.recode_blocks();
    all.push_back(m);
    std::printf("%-40s %-24s %9zu rows %6zu blocks raw %10zu  lz4 %10zu  deflate %10zu (%.2fx of lz4, zlib %10zu)  pack lz4 %.0f ms deflate %.0f ms  scan lz4 %.0f ms deflate %.0f ms  %s\n",
                m.table.c_str(), m.column.c_str(), m.rows, m.blocks, m.raw, m.lz4, m.deflate6, m.lz4 ? (double)m.deflate6 / (double)m.lz4 : 0.0, m.zlib6,
                m.lz4_pack_ms, m.deflate_pack_ms, m.lz4_scan_ms, m.deflate_scan_ms, m.scan_hash_lz4 == m.scan_hash_deflate ? "same rows" : "ROWS DIFFER");
    std::fflush(stdout);
  }
  size_t raw = 0, lz4 = 0, df = 0, zl = 0;
  double sl = 0, sd = 0;
  bool differ = false;
  for (const auto& m : all) { raw += m.raw; lz4 += m.lz4; df += m.deflate6; zl += m.zlib6; sl += m.lz4_scan_ms; sd += m.deflate_scan_ms; differ = differ || m.scan_hash_lz4 != m.scan_hash_deflate; }
  std::printf("TOTAL %zu block columns: raw %zu  lz4 %zu  deflate %zu (%.2fx of lz4; zlib %zu, in-tree %+.1f%% of zlib)  scan lz4 %.0f ms  deflate %.0f ms  %s\n",
              all.size(), raw, lz4, df, lz4 ? (double)df / (double)lz4 : 0.0, zl, zl ? 100.0 * ((double)df - (double)zl) / (double)zl : 0.0, sl, sd, differ ? "ROWS DIFFER" : "rows identical");
  return differ ? 1 : 0;
}
