// Drives the append-only checkpoint on a root and says what it saw, so a
// restart can be checked against it:
//
//   aios-asql-append-check seed <root> <db.table> <rows>
//       Creates the table (TEXT_CODEC=DEFLATE, a text column long enough to
//       be block-compressed), fills it, OPTIMIZE TABLE, checkpoint: the whole
//       image. Prints the table's row count and a hash of every cell.
//   aios-asql-append-check append <root> <db.table> <rows>
//       Inserts <rows> more and checkpoints: with AIOS_STORE_TRACE=1 the
//       store says whether it appended a part or rewrote the file.
//   aios-asql-append-check update <root> <db.table>
//       Changes one stored row and checkpoints: the file must be rewritten.
//   aios-asql-append-check verify <root> <db.table>
//       Opens the root in a fresh process and prints the row count, the hash
//       and SHOW ASQL STORAGE for the table.
// Every mode ends by printing "rows=<n> hash=<h>" for the table.
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>

#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"

using namespace aios;
using namespace aios::userland;

namespace {
uint64_t hash_table(SqlDatabase& db, const std::string& key) {
  const size_t dot = key.find('.');
  const SqlTable* t = db.find_table(key.substr(dot + 1), dot == std::string::npos ? "" : key.substr(0, dot));
  if (!t) return 0;
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t r = 0; r < t->row_count(); ++r)
    for (size_t c = 0; c < t->columns.size(); ++c) {
      const std::string& v = t->cell(r, c);
      for (unsigned char ch : v) { h ^= ch; h *= 0x100000001b3ull; }
      h ^= 0x9e3779b97f4a7c15ull; h *= 0x100000001b3ull;
    }
  return h;
}
size_t rows_of(SqlDatabase& db, const std::string& key) {
  const size_t dot = key.find('.');
  const SqlTable* t = db.find_table(key.substr(dot + 1), dot == std::string::npos ? "" : key.substr(0, dot));
  return t ? t->row_count() : 0;
}
bool run(SqlDatabase& db, const std::string& sql, bool show = false) {
  SqlQueryResult r = db.execute_local_query(sql);
  if (!r.success) { std::fprintf(stderr, "failed: %s\n  %s\n", sql.substr(0, 120).c_str(), r.error_message.c_str()); return false; }
  if (show) std::printf("%s", r.formatted_table.c_str());
  return true;
}
std::string row_text(size_t i) {
  char b[256];
  std::snprintf(b, sizeof(b), "2026-10-22 09:%02zu:%02zu user%zu updated order %zu status=%s {\"id\":%zu,\"note\":\"line %zu of the audit log, carried over\"}",
                (i / 60) % 60, i % 60, i % 37, i * 7919 % 100003, i % 3 ? "shipped" : "held", i, i);
  return b;
}
bool insert_rows(SqlDatabase& db, const std::string& key, size_t from, size_t n) {
  for (size_t i = from; i < from + n;) {
    std::string sql = "INSERT INTO " + key + " (id, who, what) VALUES ";
    const size_t end = std::min(from + n, i + 200);
    for (size_t k = i; k < end; ++k) {
      if (k > i) sql += ",";
      sql += "(" + std::to_string(k) + ",'user" + std::to_string(k % 37) + "','";
      for (char ch : row_text(k)) { if (ch == '\'' || ch == '\\') sql += '\\'; sql += ch; }
      sql += "')";
    }
    if (!run(db, sql)) return false;
    i = end;
  }
  return true;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) { std::fprintf(stderr, "usage: aios-asql-append-check seed|append|update|verify <root> <db.table> [rows]\n"); return 2; }
  const std::string mode = argv[1], root = argv[2], key = argv[3];
  const size_t n = argc > 4 ? (size_t)std::strtoull(argv[4], nullptr, 10) : 0;
  KernelConfig cfg;
  cfg.root = root;
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::fprintf(stderr, "boot failed: %s\n", k.error().message.c_str()); return 1; }
  SqlDatabase db(**k);
  db.init("sql_db");
  const size_t dot = key.find('.');
  const std::string dbname = dot == std::string::npos ? "aios_db" : key.substr(0, dot);
  if (mode == "seed") {
    if (!run(db, "CREATE DATABASE IF NOT EXISTS " + dbname)) return 1;
    if (!run(db, "DROP TABLE IF EXISTS " + key)) return 1;
    if (!run(db, "CREATE TABLE " + key + " (id INT NOT NULL PRIMARY KEY, who VARCHAR(40), what TEXT) TEXT_CODEC=DEFLATE")) return 1;
    if (!insert_rows(db, key, 0, n)) return 1;
    if (!run(db, "OPTIMIZE TABLE " + key)) return 1;
    auto r = db.flush_now();
    if (!r.ok()) { std::fprintf(stderr, "checkpoint failed: %s\n", r.error().message.c_str()); return 1; }
  } else if (mode == "append") {
    const size_t from = rows_of(db, key);
    if (!insert_rows(db, key, from, n)) return 1;
    auto r = db.flush_now();
    if (!r.ok()) { std::fprintf(stderr, "checkpoint failed: %s\n", r.error().message.c_str()); return 1; }
  } else if (mode == "update") {
    if (!run(db, "UPDATE " + key + " SET who='changed' WHERE id=1")) return 1;
    auto r = db.flush_now();
    if (!r.ok()) { std::fprintf(stderr, "checkpoint failed: %s\n", r.error().message.c_str()); return 1; }
  } else if (mode != "verify") {
    std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
    return 2;
  }
  run(db, "SHOW CREATE TABLE " + key, true);
  run(db, "SHOW ASQL STORAGE " + key.substr(dot + 1), true);
  std::printf("rows=%zu hash=%016" PRIx64 "\n", rows_of(db, key), hash_table(db, key));
  return 0;
}
