// Checks two things that made a small lookup wait on something it has no part in.
// 1. The worker pool busy with another statement's long scan: a small statement's parallel lanes no worker has taken
//    are run by the statement's own thread (SqlThreadPool::wait_parallel), so it finishes in its own time, not when
//    the long scan lets the workers go. A 15 s report made every customer page wait 12 s.
// 2. A join step of few base rows against a big table, two equality keys (the semi-join path): the key that tells the
//    joined rows apart is probed, not the first one in the ON (or the first with an index). `ON mv.olr = qd.lineno
//    AND dt.transno = mv.transno` with transno NULL in every base row probed by olr (4 values, every row a
//    candidate): 2 s a customer lookup.
#undef NDEBUG
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
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
  SqlQueryResult r = db.execute_local_query(sql);
  if (!r.success) { std::printf("  query failed: %s\n  %s\n", sql.c_str(), r.error_message.c_str()); std::exit(1); }
  return r;
}
std::string cell(SqlDatabase& db, const std::string& sql) {
  SqlQueryResult r = run(db, sql);
  return r.rows.empty() || r.rows[0].values.empty() ? std::string("?") : sql_display(r.rows[0].values[0]);
}
double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
void insert_rows(SqlDatabase& db, const std::string& t, int n, const std::function<std::string(int)>& row) {
  for (int b = 1; b <= n; b += 2000) {
    std::string sql = "INSERT INTO " + t + " VALUES ";
    for (int i = b; i < b + 2000 && i <= n; ++i) sql += (i == b ? "" : ",") + row(i);
    run(db, sql);
  }
}

// Every worker busy for about `hold_ms` with a scan started on another thread; returns once they all are.
struct BusyPool {
  std::thread t;
  std::atomic<size_t> running{0};
  explicit BusyPool(int hold_ms) {
    SqlThreadPool& pool = SqlThreadPool::instance();
    const size_t lanes = pool.num_workers() + 1;
    const size_t per_lane = 10;
    t = std::thread([this, &pool, lanes, per_lane, hold_ms] {
      pool.parallel_for(lanes * per_lane, 1, [&](size_t, size_t, size_t) {
        running.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms / (int)per_lane));
      });
    });
    while (running.load() < pool.num_workers()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ~BusyPool() { t.join(); }
};

}  // namespace

int main() {
  std::printf("=== ASQL small lookups beside a long scan ===\n");
  const CheckRoot check_root("/tmp/aios-asql-lookup-latency");
  KernelConfig cfg;
  cfg.root = check_root.path;
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::printf("boot failed\n"); return 1; }
  SqlThreadPool& pool = SqlThreadPool::instance();
  std::printf("  %zu pool workers\n", pool.num_workers());
  expect(pool.num_workers() > 0, "the pool has workers");
  {
    SqlDatabase db(**k);
    db.init("sql_db");
    run(db, "CREATE DATABASE IF NOT EXISTS lldb");
    run(db, "USE lldb");

    // 1. the pool busy for 3 s with another statement's scan, once for each split
    const int hold_ms = 3000;
    {
      BusyPool busy(hold_ms);
      auto t0 = std::chrono::steady_clock::now();
      std::atomic<uint64_t> sum{0};
      pool.parallel_for(200000, 1000, [&](size_t b, size_t e, size_t) {
        uint64_t s = 0;
        for (size_t i = b; i < e; ++i) s += i;
        sum.fetch_add(s);
      });
      const double dyn_ms = ms_since(t0);
      expect(sum.load() == 200000ull * 199999ull / 2, "dynamic split: every row summed once (" + std::to_string(sum.load()) + ")");
      expect(dyn_ms < 500, "dynamic split done in its own time beside the busy pool: " + std::to_string((int)dyn_ms) + " ms");
    }
    {
      BusyPool busy(hold_ms);
      auto t0 = std::chrono::steady_clock::now();
      std::atomic<uint64_t> sum{0};
      std::atomic<size_t> calls{0};
      pool.parallel_for_static(200000, 1000, [&](size_t b, size_t e, size_t) {
        uint64_t s = 0;
        for (size_t i = b; i < e; ++i) s += i;
        sum.fetch_add(s);
        calls.fetch_add(1);
      });
      const double st_ms = ms_since(t0);
      expect(sum.load() == 200000ull * 199999ull / 2 && calls.load() == 200, "static split: 200 morsels, each once (" + std::to_string(calls.load()) + ")");
      expect(st_ms < 500, "static split done in its own time beside the busy pool: " + std::to_string((int)st_ms) + " ms");
    }

    // the same through a statement: a 200k-row filtered count (its scan goes to the pool) beside the busy pool
    run(db, "CREATE TABLE big (id INT PRIMARY KEY, v INT)");
    insert_rows(db, "big", 200000, [](int i) { return "(" + std::to_string(i) + "," + std::to_string(i % 97) + ")"; });
    const std::string idle = cell(db, "SELECT COUNT(*) FROM big WHERE v = 13");
    {
      BusyPool busy(hold_ms);
      auto t0 = std::chrono::steady_clock::now();
      const std::string got = cell(db, "SELECT COUNT(*) FROM big WHERE v = 13");
      const double q_ms = ms_since(t0);
      expect(got == idle, "the count beside the busy pool is the count: " + got + " (idle: " + idle + ")");
      expect(q_ms < 500, "a statement's scan done in its own time beside the busy pool: " + std::to_string((int)q_ms) + " ms");
    }

    // 2. two join keys over few base rows, the first in the ON telling no rows apart
    run(db, "CREATE TABLE mv (id INT PRIMARY KEY, transno INT, olr INT, type INT)");
    insert_rows(db, "mv", 200000, [](int i) {
      return "(" + std::to_string(i) + "," + std::to_string(i) + "," + std::to_string(i % 4 + 1) + ",10)";
    });
    run(db, "CREATE TABLE qd (orderno INT, lineno INT)");
    insert_rows(db, "qd", 2000, [](int i) { return "(" + std::to_string(i) + "," + std::to_string(i % 4 + 1) + ")"; });
    run(db, "CREATE TABLE dt (order_ INT, transno INT)");
    // orders 1..1000 have a transaction, transno = 2 * order (olr of row 2i is 2i % 4 + 1: matches lineno for even i)
    insert_rows(db, "dt", 1000, [](int i) { return "(" + std::to_string(i) + "," + std::to_string(2 * i) + ")"; });

    const std::string q_null =
        "SELECT COUNT(*), COUNT(mv.id) FROM qd LEFT JOIN dt ON dt.order_ = qd.orderno + 5000 "
        "LEFT JOIN mv ON mv.olr = qd.lineno AND dt.transno = mv.transno AND mv.type = 10";
    auto t0 = std::chrono::steady_clock::now();
    SqlQueryResult r = run(db, q_null);
    double j_ms = ms_since(t0);
    const std::string n_all = r.rows.empty() ? "?" : sql_display(r.rows[0].values[0]);
    const std::string n_mv = r.rows.empty() ? "?" : sql_display(r.rows[0].values[1]);
    expect(n_all == "2000" && n_mv == "0", "every transno NULL: 2000 rows, none matched (" + n_all + ", " + n_mv + ")");
    expect(j_ms < 300, "probed by the key that meets nothing: " + std::to_string((int)j_ms) + " ms");

    const std::string q_some =
        "SELECT COUNT(*), COUNT(mv.id) FROM qd LEFT JOIN dt ON dt.order_ = qd.orderno "
        "LEFT JOIN mv ON mv.olr = qd.lineno AND dt.transno = mv.transno AND mv.type = 10";
    t0 = std::chrono::steady_clock::now();
    r = run(db, q_some);
    j_ms = ms_since(t0);
    // order i <= 1000 meets mv row 2i when that row's olr (2i % 4 + 1) is the line's (i % 4 + 1)
    int want = 0;
    for (int i = 1; i <= 1000; ++i) want += (2 * i) % 4 == i % 4;
    const std::string s_all = r.rows.empty() ? "?" : sql_display(r.rows[0].values[0]);
    const std::string s_mv = r.rows.empty() ? "?" : sql_display(r.rows[0].values[1]);
    expect(s_all == "2000" && s_mv == std::to_string(want),
           "half the orders with a transaction: 2000 rows, " + std::to_string(want) + " matched (" + s_all + ", " + s_mv + ")");
    expect(j_ms < 300, "probed by transno, not olr: " + std::to_string((int)j_ms) + " ms");


    // 2b. st's open-orders count: a key with an index (transno) that gives few rows beside one without (olr, a few
    //     line numbers over 1M rows): probed by the indexed key, with no index built for the other just to compare
    run(db, "CREATE TABLE mv2 (id INT PRIMARY KEY, transno INT, olr INT, type INT)");
    insert_rows(db, "mv2", 1000000, [](int i) {
      return "(" + std::to_string(i) + "," + std::to_string(i / 4 + 1) + "," + std::to_string(i % 4 + 1) + ",10)";
    });
    run(db, "CREATE TABLE ol (orderno INT, lineno INT)");
    insert_rows(db, "ol", 40000, [](int i) { return "(" + std::to_string(i) + "," + std::to_string(i % 4 + 1) + ")"; });
    run(db, "CREATE TABLE dt2 (order_ INT, transno INT)");
    insert_rows(db, "dt2", 40000, [](int i) { return "(" + std::to_string(i) + "," + std::to_string(i * 3) + ")"; });
    // 40000 order lines against 1M rows: past the semi-join, the transno index is built and kept
    expect(cell(db, "SELECT COUNT(mv2.id) FROM ol LEFT JOIN dt2 ON dt2.order_ = ol.orderno LEFT JOIN mv2 ON dt2.transno = mv2.transno") == "160000",
           "the transno index built: " + cell(db, "SELECT COUNT(mv2.id) FROM ol LEFT JOIN dt2 ON dt2.order_ = ol.orderno LEFT JOIN mv2 ON dt2.transno = mv2.transno"));
    const std::string q_open =
        "SELECT COUNT(*), COUNT(mv2.id) FROM ol LEFT JOIN dt2 ON dt2.order_ = ol.orderno "
        "LEFT JOIN mv2 ON mv2.olr = ol.lineno AND dt2.transno = mv2.transno AND mv2.type = 10 WHERE ol.orderno <= 1500";
    // transno 3i holds rows 12i-4 .. 12i-1 (i / 4 + 1 = 3i), their olr 1..4: one of them is the line's
    t0 = std::chrono::steady_clock::now();
    r = run(db, q_open);
    j_ms = ms_since(t0);
    const std::string o_all = r.rows.empty() ? "?" : sql_display(r.rows[0].values[0]);
    const std::string o_mv = r.rows.empty() ? "?" : sql_display(r.rows[0].values[1]);
    expect(o_all == "1500" && o_mv == "1500", "1500 order lines, each its one stock move (" + o_all + ", " + o_mv + ")");
    expect(j_ms < 45, "probed by the transno index, no olr index built to compare: " + std::to_string((int)j_ms) + " ms");

    // 3. the order-entry search's price date: a correlated MAX (and MIN) over 900k prices per searched item, by a
    //    key -> rows map of prices.stockid. The search writes a price on its way out; that write is to a column the
    //    map is not made of, so the next search finds the map as it was instead of building it over all 900k rows.
    run(db, "CREATE TABLE sm (stockid VARCHAR(20) PRIMARY KEY, description VARCHAR(60))");
    insert_rows(db, "sm", 50000, [](int i) {
      return "('S" + std::to_string(i) + "','TIRE " + std::string(i % 156 == 0 ? "2256018" : "1956515") + " X" + std::to_string(i) + "')";
    });
    run(db, "CREATE TABLE pr (stockid VARCHAR(20) NOT NULL, typeabbrev CHAR(2) NOT NULL, currabrev CHAR(3) NOT NULL DEFAULT 'USD', "
            "price DECIMAL(20,4), startdate DATE NOT NULL, enddate DATE NOT NULL, "
            "PRIMARY KEY (stockid, typeabbrev, currabrev, startdate, enddate), KEY StockID (stockid))");
    static const char* types[] = {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R"};
    insert_rows(db, "pr", 900000, [](int i) {
      const int st = (i - 1) / 18, ty = (i - 1) % 18;
      return "('S" + std::to_string(st + 1) + "','" + types[ty] + "','USD'," + std::to_string(10 + st % 90) + ",'2025-0" +
             std::to_string(1 + st % 9) + "-01','9999-12-31')";
    });
    const std::string search =
        "SELECT COUNT(*), SUM(pr.price) FROM sm s JOIN pr ON s.stockid = pr.stockid AND pr.typeabbrev = 'H' AND pr.startdate = COALESCE("
        "(SELECT MAX(p2.startdate) FROM pr p2 WHERE p2.stockid = s.stockid AND p2.typeabbrev = pr.typeabbrev "
        "AND p2.startdate <= CURRENT_DATE AND (p2.enddate >= CURRENT_DATE OR p2.enddate = '0000-00-00')), "
        "(SELECT MIN(p3.startdate) FROM pr p3 WHERE p3.stockid = s.stockid AND p3.typeabbrev = pr.typeabbrev "
        "AND p3.startdate > CURRENT_DATE)) WHERE s.description LIKE '%2256018%'";
    expect(cell(db, search) == "320", "the search's 320 items each have their price row: " + cell(db, search));
    double worst = 0;
    for (int k = 0; k < 3; ++k) {
      run(db, "UPDATE pr SET price = price + 1 WHERE stockid = 'S" + std::to_string(156 * (k + 1)) + "' AND typeabbrev = 'H'");
      t0 = std::chrono::steady_clock::now();
      const std::string n = cell(db, search);
      const double ms = ms_since(t0);
      worst = std::max(worst, ms);
      expect(n == "320", "after a price write, still 320 items: " + n);
    }
    expect(worst < 200, "a search after a price write finds the map as it was: worst " + std::to_string((int)worst) + " ms");
    const std::string one = "SELECT (SELECT MAX(p2.startdate) FROM pr p2 WHERE p2.stockid = s.stockid AND p2.typeabbrev = 'H') "
                            "FROM sm s WHERE s.description LIKE '%2256018%' AND s.stockid = ";
    // the date the answer is made of, written: read from the table as it is now
    run(db, "UPDATE pr SET startdate = '2030-01-01' WHERE stockid = 'S156' AND typeabbrev = 'H'");
    expect(cell(db, one + "'S156'") == "2030-01-01", "a written date is the answer: " + cell(db, one + "'S156'"));
    // a key written: the map is made again
    run(db, "UPDATE pr SET stockid = 'S157' WHERE stockid = 'S156' AND typeabbrev = 'H'");
    expect(cell(db, one + "'S156'") == "NULL", "the key moved away: no price date (" + cell(db, one + "'S156'") + ")");
    expect(cell(db, "SELECT (SELECT MAX(p2.startdate) FROM pr p2 WHERE p2.stockid = s.stockid AND p2.typeabbrev = 'H') FROM sm s "
                    "WHERE s.stockid = 'S157'") == "2030-01-01",
           "the key moved to S157: its date with it");
    // rows deleted: the map is made again
    run(db, "DELETE FROM pr WHERE stockid = 'S312'");
    expect(cell(db, one + "'S312'") == "NULL", "deleted rows: no price date (" + cell(db, one + "'S312'") + ")");
    expect(cell(db, search) == "318", "the search after the key move and the delete: 318 items (" + cell(db, search) + ")");
    // an aggregate over no rows, answered without the nested statement: as SQL has it
    const std::string none = " FROM pr p2 WHERE p2.stockid = s.stockid AND p2.startdate > '2099-01-01') FROM sm s WHERE s.description LIKE '%2256018%' AND s.stockid = 'S468'";
    expect(cell(db, "SELECT (SELECT COUNT(*)" + none) == "0", "COUNT of no rows: 0 (" + cell(db, "SELECT (SELECT COUNT(*)" + none) + ")");
    expect(cell(db, "SELECT (SELECT MIN(p2.startdate)" + none) == "NULL", "MIN of no rows: NULL");
    expect(cell(db, "SELECT (SELECT SUM(p2.price)" + none) == "NULL", "SUM of no rows: NULL");
    expect(cell(db, "SELECT (SELECT COUNT(DISTINCT p2.typeabbrev)" + none) == "0", "COUNT(DISTINCT) of no rows: 0");
    expect(cell(db, "SELECT (SELECT COALESCE(MAX(p2.startdate), 'none')" + none) == "none",
           "an expression of an aggregate of no rows: 'none' (" + cell(db, "SELECT (SELECT COALESCE(MAX(p2.startdate), 'none')" + none) + ")");
    expect(cell(db, "SELECT SUM((SELECT COUNT(*) FROM pr p2 WHERE p2.stockid = s.stockid AND p2.typeabbrev = 'H')) FROM sm s "
                    "WHERE s.description LIKE '%2256018%'") == "318",
           "COUNT per searched item: 318 (S156's H row moved to S157, S312's rows deleted)");
  }

  std::printf("  %d checks\n", checks);
  if (failures) { std::printf("=== %d FAILURE(S) ===\n", failures); return 1; }
  std::printf("=== ALL LOOKUP LATENCY CHECKS PASSED ===\n");
  return 0;
}
