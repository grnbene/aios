// Checks the storage maintenance that shrinks dictionary columns in place:
// codes narrowed to the width the dictionary needs, and large dictionaries
// coded with a symbol table. A column of JSON-like text (few values, each
// long) and one of short statuses are written the way replication writes
// them, maintenance runs until it has nothing left, and every cell, filter,
// LIKE, GROUP BY and ORDER BY must answer as a model of the table says -
// after more writes, a dictionary compaction, and a restart from the store.
#undef NDEBUG
#include <filesystem>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"

using namespace aios;
using namespace aios::userland;

namespace {

int failures = 0;
#define CHECK(cond, ...)                                                              \
  do {                                                                                \
    if (!(cond)) {                                                                    \
      ++failures;                                                                     \
      std::printf("  [FAIL] " __VA_ARGS__);                                           \
      std::printf("\n");                                                              \
      if (failures > 20) { std::printf("too many failures\n"); std::exit(1); }        \
    }                                                                                 \
  } while (0)

struct Row {
  int id;
  std::string doc;      // NULL as "\N"
  std::string status;
};

std::string q1(SqlDatabase& db, const std::string& sql) {
  SqlQueryResult r = db.execute_local_query(sql);
  if (!r.success) { std::printf("  query failed: %s\n  %s\n", sql.c_str(), r.error_message.c_str()); std::exit(1); }
  if (r.rows.empty() || r.rows[0].values.empty()) return "<none>";
  return r.rows[0].values[0];
}

void exec(SqlDatabase& db, const std::string& sql) {
  SqlQueryResult r = db.execute_local_query(sql);
  if (!r.success) { std::printf("  statement failed: %s\n  %s\n", sql.substr(0, 200).c_str(), r.error_message.c_str()); std::exit(1); }
}

std::string quote(const std::string& s) {
  if (s == "\\N") return "NULL";
  std::string o = "'";
  for (char c : s) { if (c == '\'' || c == '\\') o += '\\'; o += c; }
  return o + "'";
}

// A purchase-order options document, as the ERP keeps them: long, few distinct.
std::string make_doc(std::mt19937_64& rng, int variant) {
  static const char* names[] = {"Chrome", "Black", "Gunmetal", "Bronze", "Silver", "Matte", "Gloss", "Satin"};
  std::string d = "{\"options\":[";
  for (int i = 0; i < 6 + variant % 5; ++i) {
    if (i) d += ",";
    d += "{\"name\":\"finish\",\"value\":\"" + std::string(names[(variant + i) % 8]) + "\",\"price\":" +
         std::to_string((variant * 37 + i * 11) % 500) + ".00,\"sku\":\"WHL-" + std::to_string(10000 + variant * 7 + i) + "\"}";
  }
  d += "],\"notes\":\"customer pickup - verify lug pattern 5x114.3 before mounting; variant " + std::to_string(variant) + "\"}";
  (void)rng;
  return d;
}

const char* kStatuses[] = {"open", "picked", "shipped", "invoiced", "cancelled", "returned"};

std::string encoding_of(SqlDatabase& db, const std::string& col) {
  SqlQueryResult r = db.execute_local_query("SHOW ASQL STORAGE dictcheck");
  for (const auto& row : r.rows)
    if (row.values.size() > 3 && row.values[1] == col) return row.values[3];
  return "?";
}

void verify(SqlDatabase& db, const std::vector<Row>& model, const char* step) {
  const SqlTable* t = db.find_table("dictcheck", "dictdb");
  CHECK(t && t->row_count() == model.size(), "%s: %zu rows, model %zu", step, t ? t->row_count() : 0, model.size());
  if (!t) return;
  size_t bad = 0;
  for (size_t r = 0; r < model.size() && r < t->row_count() && bad < 5; ++r) {
    const std::string doc = t->col_data[1].is_null(r) ? "\\N" : t->cell(r, 1);
    const std::string st = t->col_data[2].is_null(r) ? "\\N" : t->cell(r, 2);
    if (doc != model[r].doc || st != model[r].status) {
      ++bad;
      CHECK(false, "%s: row %zu is (%s, %s), model (%s, %s)", step, r, doc.substr(0, 40).c_str(), st.c_str(),
            model[r].doc.substr(0, 40).c_str(), model[r].status.c_str());
    }
  }
  // Filters, LIKE, grouping and ordering through the query engine.
  std::map<std::string, size_t> by_status;
  size_t like_gunmetal = 0, like_variant7 = 0, doc_eq = 0;
  const std::string probe = model[model.size() / 3].doc;
  for (const Row& m : model) {
    if (m.status != "\\N") by_status[m.status]++;
    if (m.doc != "\\N") {
      like_gunmetal += m.doc.find("Gunmetal") != std::string::npos;
      like_variant7 += m.doc.find("variant 7\"") != std::string::npos;
      doc_eq += m.doc == probe;
    }
  }
  CHECK(q1(db, "SELECT COUNT(*) FROM dictcheck WHERE doc LIKE '%gunmetal%'") == std::to_string(like_gunmetal), "%s: LIKE %%gunmetal%%", step);
  CHECK(q1(db, "SELECT COUNT(*) FROM dictcheck WHERE doc LIKE '%variant 7\"}'") == std::to_string(like_variant7), "%s: LIKE suffix", step);
  if (probe != "\\N")
    CHECK(q1(db, "SELECT COUNT(*) FROM dictcheck WHERE doc = " + quote(probe)) == std::to_string(doc_eq), "%s: doc = probe", step);
  SqlQueryResult g = db.execute_local_query("SELECT status, COUNT(*) FROM dictcheck WHERE status IS NOT NULL GROUP BY status ORDER BY status");
  CHECK(g.success && g.rows.size() == by_status.size(), "%s: GROUP BY status has %zu groups, model %zu", step, g.rows.size(), by_status.size());
  size_t gi = 0;
  for (const auto& kv : by_status) {
    if (gi >= g.rows.size()) break;
    CHECK(g.rows[gi].values[0] == kv.first && g.rows[gi].values[1] == std::to_string(kv.second), "%s: group %s", step, kv.first.c_str());
    ++gi;
  }
  CHECK(q1(db, "SELECT COUNT(DISTINCT doc) FROM dictcheck") ==
            std::to_string([&] { std::map<std::string, int> s; for (const Row& m : model) if (m.doc != "\\N") s[m.doc]; return s.size(); }()),
        "%s: COUNT(DISTINCT doc)", step);
  // Text order as the collation has it (ASCII here: letters folded to upper case).
  auto ci_less = [](const std::string& a, const std::string& b) {
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
      const int x = std::toupper((unsigned char)a[i]), y = std::toupper((unsigned char)b[i]);
      if (x != y) return x < y;
    }
    return a.size() < b.size();
  };
  std::string lo, hi;
  for (const Row& m : model) {
    if (m.doc == "\\N") continue;
    if (lo.empty() || ci_less(m.doc, lo)) lo = m.doc;
    if (hi.empty() || ci_less(hi, m.doc)) hi = m.doc;
  }
  CHECK(q1(db, "SELECT doc FROM dictcheck WHERE doc IS NOT NULL ORDER BY doc LIMIT 1") == lo, "%s: ORDER BY doc", step);
  CHECK(q1(db, "SELECT MIN(doc) FROM dictcheck") == lo, "%s: MIN(doc)", step);
  CHECK(q1(db, "SELECT MAX(doc) FROM dictcheck") == hi, "%s: MAX(doc)", step);
  // Ranges against a long value: the bounds kept for long text are cut short.
  size_t gt = 0, le = 0;
  for (const Row& m : model) if (m.doc != "\\N") { gt += ci_less(probe, m.doc); le += !ci_less(probe, m.doc); }
  if (probe != "\\N") {
    CHECK(q1(db, "SELECT COUNT(*) FROM dictcheck WHERE doc > " + quote(probe)) == std::to_string(gt), "%s: doc > probe", step);
    CHECK(q1(db, "SELECT COUNT(*) FROM dictcheck WHERE doc <= " + quote(probe)) == std::to_string(le), "%s: doc <= probe", step);
  }
  std::printf("  [ok] %s: %zu rows, doc %s, status %s\n", step, model.size(), encoding_of(db, "doc").c_str(), encoding_of(db, "status").c_str());
}

// Maintenance until it does no more (each checkpoint runs a pass).
void settle(SqlDatabase& db) {
  for (int i = 0; i < 200; ++i) {
    const SqlTable* t = db.find_table("dictcheck", "dictdb");
    const size_t plain = t->col_data[1].dict.plain_bytes();
    const uint8_t w = t->col_data[2].bit_width_;
    if (!db.flush_now().ok()) { std::printf("checkpoint failed\n"); std::exit(1); }
    t = db.find_table("dictcheck", "dictdb");
    if (t->col_data[1].dict.plain_bytes() == plain && t->col_data[2].bit_width_ == w) return;
  }
}

}  // namespace

// A dictionary column with 16-bit codes, each value on a few rows scattered through it: `v = x` must find every
// one (read back from the store, then rows deleted: the codes are rewritten 8, 16 or 32 bits wide). The vectorized 16-bit compare once dropped half of each 64-row block's matches (its byte packing lost the
// bits of rows 16-31 and 48-63 and mixed the rest), so most single matches read as no match.
void make_wide_codes(SqlDatabase& db) {
  exec(db, "CREATE TABLE widecodes (id INT NOT NULL, v VARCHAR(20))");
  std::string batch;
  for (int i = 0; i < 9000; ++i) {
    batch += (batch.empty() ? "" : ",") + std::string("(") + std::to_string(i) + ",'val-" + std::to_string((i * 7) % 1000) + "')";
    if (batch.size() > 50000 || i == 8999) { exec(db, "INSERT INTO widecodes VALUES " + batch); batch.clear(); }
  }
  settle(db);
}

void check_wide_codes(SqlDatabase& db) {
  // a delete with more than 4,096 rows after it rewrites the packed codes at a byte width
  exec(db, "INSERT INTO widecodes VALUES (50000, 'val-new')");
  exec(db, "DELETE FROM widecodes WHERE id < 3 OR id = 50000");
  const SqlTable* t = db.find_table("widecodes", "dictdb");
  CHECK(t && t->col_data[1].encoding == ColumnEncoding::DictBitPacked && t->col_data[1].bit_width_ == 16,
        "widecodes.v is not 16-bit dictionary codes (encoding %d, %u bits)", t ? (int)t->col_data[1].encoding : -1,
        t ? (unsigned)t->col_data[1].bit_width_ : 0u);
  int wrong = 0;
  for (int k = 0; k < 1000; ++k) {
    const std::string got = q1(db, "SELECT COUNT(*) FROM widecodes WHERE v = 'val-" + std::to_string(k) + "'");
    const std::string ids = q1(db, "SELECT COUNT(id) FROM widecodes WHERE v = 'val-" + std::to_string(k) + "'");
    const std::string want = (k == 0 || k == 7 || k == 14) ? "8" : "9";   // ids 0, 1, 2 held these and are gone
    if (got != want || ids != want) {
      if (++wrong <= 3) std::printf("  [FAIL] v = 'val-%d': COUNT(*) %s, COUNT(id) %s, want %s\n", k, got.c_str(), ids.c_str(), want.c_str());
    }
  }
  CHECK(wrong == 0, "%d of 1000 values of a 16-bit dictionary column miscounted by `v = x`", wrong);
  std::printf("  16-bit codes: %d of 1000 equality counts wrong\n", wrong);
  exec(db, "DROP TABLE widecodes");
}

int main() {
  std::printf("=== ASQL dictionary maintenance check (narrowed codes, coded dictionaries) ===\n");
  setenv("AIOS_STORE_MAINTAIN_MS", "2000", 1);
  setenv("AIOS_ZONE_VERIFY", "1", 1);   // every incremental zone-map update checked against a rescan
  const std::string root = "/tmp/aios-asql-dictcheck";
  { std::error_code fs_err; std::filesystem::remove_all(root, fs_err); }
  KernelConfig cfg;
  cfg.root = root;
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::printf("boot failed\n"); return 1; }
  std::mt19937_64 rng(77);
  std::vector<Row> model;
  {
    SqlDatabase db(**k);
    db.init("sql_db");
    exec(db, "CREATE DATABASE IF NOT EXISTS dictdb");
    exec(db, "USE dictdb");
    exec(db, "CREATE TABLE dictcheck (id INT NOT NULL, doc TEXT, status VARCHAR(20))");
    // 2,500 distinct documents (~600 KB each copy... ~1.5 MB of dictionary) over 20,000 rows.
    std::string batch;
    for (int i = 0; i < 20000; ++i) {
      Row r{i, (i % 97 == 0) ? "\\N" : make_doc(rng, (int)(rng() % 2500)), (i % 89 == 0) ? "\\N" : kStatuses[rng() % 6]};
      model.push_back(r);
      batch += (batch.empty() ? "" : ",") + std::string("(") + std::to_string(i) + "," + quote(r.doc) + "," + quote(r.status) + ")";
      if (batch.size() > 200000 || i == 19999) { exec(db, "INSERT INTO dictcheck VALUES " + batch); batch.clear(); }
    }
    // A write the status codes take only as a 32-bit dictionary (what
    // replication leaves): an UPDATE through materialize_to_dict_packed.
    exec(db, "UPDATE dictcheck SET status = 'held' WHERE id = 5");
    model[5].status = "held";
    verify(db, model, "inserted");
    const SqlTable* t = db.find_table("dictcheck", "dictdb");
    std::printf("  before: doc dictionary %zu entries, %.2f MB plain; status codes %u bits\n", t->col_data[1].dict.size(),
                t->col_data[1].dict.plain_bytes() / 1048576.0, (unsigned)t->col_data[2].bit_width_);
    settle(db);
    t = db.find_table("dictcheck", "dictdb");
    const DictArena& d = t->col_data[1].dict;
    std::printf("  after:  doc dictionary %zu coded / %zu, %.2f MB as held; status codes %u bits\n", d.coded_count(), d.size(),
                d.byte_size() / 1048576.0, (unsigned)t->col_data[2].bit_width_);
    CHECK(d.coded() && d.plain_bytes() == 0, "doc dictionary not coded (%zu of %zu)", d.coded_count(), d.size());
    CHECK(t->col_data[2].bit_width_ <= 8, "status codes still %u bits", (unsigned)t->col_data[2].bit_width_);
    verify(db, model, "maintained");
    make_wide_codes(db);

    // Writes after: an existing document (no new entry), new ones (a plain
    // tail), NULLs, appends.
    const size_t entries = d.size();
    for (int i = 0; i < 300; ++i) {
      const size_t r = rng() % model.size();
      model[r].doc = (i % 3 == 0) ? model[(r + 17) % model.size()].doc : (i % 3 == 1) ? make_doc(rng, 5000 + i) : "\\N";
      model[r].status = kStatuses[(r + i) % 6];
      exec(db, "UPDATE dictcheck SET doc = " + quote(model[r].doc) + ", status = " + quote(model[r].status) + " WHERE id = " + std::to_string(model[r].id));
    }
    for (int i = 0; i < 500; ++i) {
      Row r{30000 + i, (i % 2) ? model[(size_t)i * 7].doc : make_doc(rng, 9000 + i), kStatuses[i % 6]};
      model.push_back(r);
      exec(db, "INSERT INTO dictcheck VALUES (" + std::to_string(r.id) + "," + quote(r.doc) + "," + quote(r.status) + ")");
    }
    t = db.find_table("dictcheck", "dictdb");
    std::printf("  written: doc dictionary %zu entries (%zu before), %zu coded\n", t->col_data[1].dict.size(), entries,
                t->col_data[1].dict.coded_count());
    verify(db, model, "written after coding");
    // Deletes, then a compaction (the dictionary has far more entries than
    // rows once most rows are gone).
    exec(db, "DELETE FROM dictcheck WHERE id < 19600");
    std::vector<Row> keep;
    for (const Row& m : model) if (m.id >= 19600) keep.push_back(m);
    model.swap(keep);
    const size_t before_compaction = db.find_table("dictcheck", "dictdb")->col_data[1].dict.size();
    settle(db);
    t = db.find_table("dictcheck", "dictdb");
    std::printf("  after deletes: doc dictionary %zu entries (%zu before), %zu coded\n", t->col_data[1].dict.size(), before_compaction,
                t->col_data[1].dict.coded_count());
    CHECK(t->col_data[1].dict.size() < before_compaction, "the dictionary was not compacted");
    CHECK(t->col_data[1].dict.coded_count() > 0, "compaction dropped the coding");
    verify(db, model, "deleted and compacted");
    if (!db.flush_now().ok()) { std::printf("checkpoint failed\n"); return 1; }
  }
  {
    SqlDatabase db2(**k);
    db2.init("sql_db");
    exec(db2, "USE dictdb");
    const SqlTable* t = db2.find_table("dictcheck", "dictdb");
    CHECK(t && t->col_data[1].dict.coded(), "the coded dictionary did not come back coded from the store");
    verify(db2, model, "read back from the store");
    check_wide_codes(db2);
    Row r{99999, model[0].doc, "open"};
    model.push_back(r);
    exec(db2, "INSERT INTO dictcheck VALUES (99999," + quote(r.doc) + ",'open')");
    verify(db2, model, "appended after reload");
  }
  if (failures) { std::printf("=== %d FAILURE(S) ===\n", failures); return 1; }
  std::printf("=== ALL DICTIONARY MAINTENANCE CHECKS PASSED ===\n");
  return 0;
}
