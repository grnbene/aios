// Checks that the result cache keeps a long read's answer when the writes that landed while it ran were to tables
// it does not read, and only then.
//
// An answer was refused if any write anywhere landed between the read's start and its store: one global generation
// stood for every table. On a busy ERP something is always being written - prices, audittrail, the replication
// applier - so a report of a second or more was almost never kept, however unrelated the writes. The check is now
// per table the answer depends on.
//
// 1. A long read of `a` while another session writes to `b`, which it does not read: kept - the next run is a hit,
//    with the same answer.
// 2. The same read while the other session writes to `a`: not kept, and the next run's answer is the table as it
//    now is (checked against SQL_NO_CACHE).
// 3. A kept answer, then a write to `a`: the next run sees the write.
// 4. A read of a view over `a`, while `b` is written: kept (a view's answer depends on the tables it reads); then a
//    write to `a`: the next run sees it.
#undef NDEBUG
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"
#include "check-root.hpp"

using namespace aios;
using namespace aios::userland;

namespace {

int failures = 0;
int checks = 0;
void expect(bool ok, const std::string& what) {
  ++checks;
  if (!ok) { ++failures; std::printf("  [FAIL] %s\n", what.c_str()); }
  else std::printf("  [ok] %s\n", what.c_str());
}
SqlQueryResult run(SqlDatabase& db, const std::string& sql) {
  SqlQueryResult r = db.execute_gateway_query(sql);
  if (!r.success) { std::printf("  query failed: %s\n  %s\n", sql.c_str(), r.error_message.c_str()); std::exit(1); }
  return r;
}
std::string answer(SqlDatabase& db, const std::string& sql) {
  SqlQueryResult r = run(db, sql);
  std::string s;
  for (const auto& row : r.rows)
    for (const auto& v : row.values) s += sql_display(v) + ",";
  return s;
}
long long hits(SqlDatabase& db) {
  SqlQueryResult r = db.execute_gateway_query("SHOW GLOBAL STATUS LIKE 'Asql_cache_hits'");
  return r.success && !r.rows.empty() && r.rows[0].values.size() > 1 ? std::atoll(sql_display(r.rows[0].values[1]).c_str()) : -1;
}
double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// Another session writing `sql_of(i)` until stopped; how many writes it made.
struct OtherSession {
  std::atomic<bool> stop{false};
  std::atomic<int> made{0};
  std::thread t;
  OtherSession(SqlDatabase& db, std::string (*sql_of)(int)) {
    t = std::thread([this, &db, sql_of] {
      for (int i = 0; !stop.load(); ++i) {
        SqlQueryResult r = db.execute_gateway_query(sql_of(i));
        if (!r.success) { std::printf("  writer failed: %s\n", r.error_message.c_str()); std::exit(1); }
        made.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    });
  }
  ~OtherSession() { stop.store(true); t.join(); }
};
std::atomic<int> next_b{100000};   // b's next id, across every writer
std::string write_b(int) { return "INSERT INTO b VALUES (" + std::to_string(next_b.fetch_add(1)) + ", 'x')"; }
std::string write_a(int i) { return "UPDATE a SET v = v + 1 WHERE id = " + std::to_string(1 + (i % 50)); }

constexpr int kRows = 1500000;

}  // namespace

int main() {
  std::printf("=== ASQL result cache: a long read kept unless a table it reads was written ===\n");
  const CheckRoot check_root("/tmp/aios-asql-cache-deps");
  KernelConfig cfg;
  cfg.root = check_root.path;
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::printf("boot failed\n"); return 1; }
  {
    SqlDatabase db(**k);
    db.init("sql_db");
    run(db, "CREATE TABLE a (id INT PRIMARY KEY, v INT, g VARCHAR(8))");
    run(db, "CREATE TABLE b (id INT PRIMARY KEY, note VARCHAR(8))");
    for (int s = 1; s <= kRows; s += 5000) {
      std::string sql = "INSERT INTO a VALUES ";
      for (int i = s; i < s + 5000 && i <= kRows; ++i)
        sql += (i == s ? "" : ",") + std::string("(") + std::to_string(i) + "," + std::to_string((i * 7919) % 100003) + ",'g" +
               std::to_string(i % 13) + "')";
      run(db, sql);
    }
    run(db, "CREATE VIEW av AS SELECT g, v FROM a");
    // A read that takes a while: grouped, over a computed expression, no index for it.
    const std::string q = "SELECT g, COUNT(*), SUM(v * 3 + 1), MAX(CONCAT(g, '-', v % 997)) FROM a WHERE (v * 7) % 11 <> 4 GROUP BY g ORDER BY g";
    const std::string qv = "SELECT g, SUM(v % 1013), COUNT(*) FROM av WHERE (v * 5) % 17 <> 2 GROUP BY g ORDER BY g";
    const std::string q_nc = "SELECT SQL_NO_CACHE" + q.substr(6);
    const std::string qv_nc = "SELECT SQL_NO_CACHE" + qv.substr(6);
    auto t0 = std::chrono::steady_clock::now();
    const std::string first = answer(db, q);   // the first sighting: the cache admits a statement seen twice
    const double q_ms = ms_since(t0);
    std::printf("  the read takes %.0f ms\n", q_ms);
    expect(q_ms >= 50, "the read is long enough for writes to land while it runs");

    // 1. writes to b while a is read
    {
      long long made = 0;
      {
        OtherSession w(db, write_b);
        while (w.made.load() < 3) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        answer(db, q);
        made = w.made.load();
      }
      std::printf("  %lld writes to b landed during or around the read\n", made);
      const long long h0 = hits(db);
      const std::string again = answer(db, q);
      expect(hits(db) == h0 + 1, "1. a read of a, b written while it ran: the next run is a cache hit");
      expect(again == first, "1. ... and its answer is the same");
    }

    // 2. writes to a while a is read
    {
      {
        OtherSession w(db, write_a);
        while (w.made.load() < 3) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        answer(db, q);
      }
      const std::string fresh = answer(db, q_nc);
      expect(answer(db, q) == fresh, "2. a read of a, a written while it ran: the next run answers the table as it is now");
    }

    // 3. a kept answer, then a write to a
    {
      answer(db, q);
      const long long h0 = hits(db);
      const std::string kept = answer(db, q);
      expect(hits(db) == h0 + 1, "3. the read is kept");
      run(db, "UPDATE a SET v = v + 100 WHERE id <= 1000");
      const std::string after = answer(db, q);
      expect(after == answer(db, q_nc), "3. after a write to a, the next run answers the table as it is now");
      expect(after != kept, "3. ... which is another answer than the kept one");
    }

    // 4. a view over a
    {
      const std::string vfirst = answer(db, qv);
      {
        OtherSession w(db, write_b);
        while (w.made.load() < 3) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        answer(db, qv);
      }
      const long long h0 = hits(db);
      expect(answer(db, qv) == vfirst, "4. a read of a view over a, b written while it ran: the same answer");
      expect(hits(db) == h0 + 1, "4. ... from the cache");
      run(db, "UPDATE a SET v = v + 7 WHERE id <= 5000");
      const std::string vafter = answer(db, qv);
      expect(vafter == answer(db, qv_nc), "4. after a write to a, the view's read answers the table as it is now");
      expect(vafter != vfirst, "4. ... which is another answer");
    }
  }
  std::printf("  %d checks\n", checks);
  if (failures) { std::printf("=== %d CACHE DEPENDENCY CHECKS FAILED ===\n", failures); return 1; }
  std::printf("=== ALL CACHE DEPENDENCY CHECKS PASSED ===\n");
  return 0;
}
