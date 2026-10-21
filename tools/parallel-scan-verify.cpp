// Checks that ASQL's partitioned scans return the right answers, alone and with
// many clients scanning at once.
//
// 20,000 rows are loaded from a formula, and every expected number below is
// computed here from that same formula - never read back from the engine. The
// parallel path is forced (AIOS_SQL_MIN_PAR_ROWS=1) and the pool's own dispatch
// counter must move, so a run that quietly went serial is a failure, not a pass.
// Then kClients threads run the aggregate and the filtered scan kRounds times
// each at the same moment; every result is compared to the reference, which is
// what catches one query's lanes being mixed up with another's.
//
// Exit status is the verdict: 0 only if every check held. Nothing here uses
// assert(), which a Release build compiles out.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"

using namespace aios;
using namespace aios::userland;

namespace {

std::atomic<int> g_failures{0};

void check(bool ok, const std::string& what) {
  if (ok) return;
  ++g_failures;
  std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}

// A mkdtemp directory that is removed (recursively) when it goes out of scope.
// Declared before the Kernel that uses it, so the kernel is gone first.
struct TempRoot {
  char path[64];
  explicit TempRoot(const char* tmpl) {
    std::snprintf(path, sizeof path, "%s", tmpl);
    if (!mkdtemp(path)) { std::perror("mkdtemp"); std::exit(1); }
  }
  ~TempRoot() { std::error_code ec; std::filesystem::remove_all(path, ec); }
  TempRoot(const TempRoot&) = delete;
  TempRoot& operator=(const TempRoot&) = delete;
};

constexpr int kRows = 20000;
constexpr int kCategories = 50;
constexpr int kClients = 8;
constexpr int kRounds = 25;

// Row i: category i % 50, amount i * 1.5, qty i % 100.
struct CategoryRef {
  long long count = 0;
  double sum = 0;
  int min_qty = 1 << 30, max_qty = -1;
};
struct Reference {
  CategoryRef cat[kCategories];
  long long filtered_count = 0;   // qty > 50
  double filtered_sum = 0;
};

Reference build_reference() {
  Reference ref;
  for (int i = 0; i < kRows; ++i) {
    const int qty = i % 100;
    const double amount = i * 1.5;
    CategoryRef& c = ref.cat[i % kCategories];
    ++c.count;
    c.sum += amount;
    if (qty < c.min_qty) c.min_qty = qty;
    if (qty > c.max_qty) c.max_qty = qty;
    if (qty > 50) { ++ref.filtered_count; ref.filtered_sum += amount; }
  }
  return ref;
}

bool same(const std::string& got, double want) {
  char* end = nullptr;
  const double v = std::strtod(got.c_str(), &end);
  if (end == got.c_str()) return false;
  return std::fabs(v - want) <= 1e-6 * std::max(1.0, std::fabs(want));
}

// The GROUP BY result against the reference. Returns an empty string when it
// matches, else what differed.
std::string compare_groups(const SqlQueryResult& r, const Reference& ref) {
  if (!r.success) return "query failed: " + r.error_message;
  if (r.rows.size() != (size_t)kCategories) return "expected 50 groups, got " + std::to_string(r.rows.size());
  // Each group is looked up by its name, so the order the engine sorts them in does not matter.
  std::vector<bool> seen(kCategories, false);
  for (const auto& row : r.rows) {
    if (row.values.size() < 6) return "short row";
    const std::string& name = row.values[0];
    if (name.rfind("CAT_", 0) != 0) return "unexpected group '" + name + "'";
    const int k = std::atoi(name.c_str() + 4);
    if (k < 0 || k >= kCategories) return "unexpected group '" + name + "'";
    if (seen[(size_t)k]) return "group " + name + " appears twice";
    seen[(size_t)k] = true;
    const CategoryRef& c = ref.cat[k];
    if (!same(row.values[1], (double)c.count)) return name + " COUNT " + row.values[1] + " want " + std::to_string(c.count);
    if (!same(row.values[2], c.sum)) return name + " SUM " + row.values[2] + " want " + std::to_string(c.sum);
    if (!same(row.values[3], c.sum / (double)c.count)) return name + " AVG " + row.values[3];
    if (!same(row.values[4], (double)c.min_qty)) return name + " MIN " + row.values[4];
    if (!same(row.values[5], (double)c.max_qty)) return name + " MAX " + row.values[5];
  }
  return "";
}

std::string compare_filtered(const SqlQueryResult& r, const Reference& ref) {
  if (!r.success) return "query failed: " + r.error_message;
  if (r.rows.size() != 1 || r.rows[0].values.size() < 2) return "expected one row of two values";
  if (!same(r.rows[0].values[0], (double)ref.filtered_count)) return "COUNT " + r.rows[0].values[0] + " want " + std::to_string(ref.filtered_count);
  if (!same(r.rows[0].values[1], ref.filtered_sum)) return "SUM " + r.rows[0].values[1] + " want " + std::to_string(ref.filtered_sum);
  return "";
}

const char* kGroupSql =
    "SELECT category, COUNT(*), SUM(amount), AVG(amount), MIN(qty), MAX(qty) FROM test_scan GROUP BY category ORDER BY category;";
const char* kFilterSql = "SELECT COUNT(*), SUM(amount) FROM test_scan WHERE qty > 50;";

}  // namespace

int main() {
  // Read once, on the pool's first use: the parallel path must be taken even though 20,000 rows is small.
  setenv("AIOS_SQL_MIN_PAR_ROWS", "1", 1);

  TempRoot tmp("/tmp/aios-par-verify-XXXXXX");
  KernelConfig kcfg;
  kcfg.root = tmp.path;
  auto k_res = Kernel::boot(kcfg);
  if (!k_res.ok()) { std::cerr << "Kernel boot failed\n"; return 1; }
  Kernel& kernel = **k_res;

  SqlDatabase db(kernel);
  db.init("par_db");

  auto ddl = db.execute_query("CREATE TABLE test_scan (id INT, category VARCHAR(16), amount DOUBLE, qty INT);");
  check(ddl.success, "CREATE TABLE: " + ddl.error_message);
  for (int batch = 0; batch < kRows / 1000; ++batch) {
    std::string sql = "INSERT INTO test_scan VALUES ";
    for (int i = 0; i < 1000; ++i) {
      const int idx = batch * 1000 + i;
      if (i > 0) sql += ", ";
      sql += "(" + std::to_string(idx) + ", 'CAT_" + std::to_string(idx % kCategories) + "', " + std::to_string(idx * 1.5) + ", " + std::to_string(idx % 100) + ")";
    }
    auto ins = db.execute_query(sql + ";");
    check(ins.success, "INSERT batch " + std::to_string(batch) + ": " + ins.error_message);
  }
  const auto count_row = db.execute_query("SELECT COUNT(*) FROM test_scan;");
  check(count_row.success && !count_row.rows.empty() && same(count_row.rows[0].values[0], (double)kRows), "table holds the 20,000 rows loaded");
  if (g_failures) { std::fprintf(stderr, "setup failed; not going on\n"); return 1; }

  const Reference ref = build_reference();
  SqlThreadPool& pool = SqlThreadPool::instance();
  check(pool.num_workers() > 0, "the pool has worker threads");

  // 1. One client, both scans, against the reference.
  const uint64_t before_single = pool.dispatches();
  const auto t0 = std::chrono::steady_clock::now();
  std::string diff = compare_groups(db.execute_gateway_query(kGroupSql), ref);
  check(diff.empty(), "GROUP BY, one client: " + diff);
  diff = compare_filtered(db.execute_gateway_query(kFilterSql), ref);
  check(diff.empty(), "filtered scan, one client: " + diff);
  const double single_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  check(pool.dispatches() > before_single, "the single-client scans went through the pool (dispatch counter moved)");

  // 2. Many clients at once, every answer checked.
  const uint64_t before_many = pool.dispatches();
  std::atomic<int> wrong{0};
  std::string first_wrong;
  std::atomic<bool> have_first{false};
  const auto t1 = std::chrono::steady_clock::now();
  std::vector<std::thread> clients;
  for (int c = 0; c < kClients; ++c) {
    clients.emplace_back([&, c] {
      for (int round = 0; round < kRounds; ++round) {
        const bool group = ((c + round) % 2) == 0;
        const SqlQueryResult r = db.execute_gateway_query(group ? kGroupSql : kFilterSql);
        const std::string d = group ? compare_groups(r, ref) : compare_filtered(r, ref);
        if (!d.empty()) {
          ++wrong;
          bool expected = false;
          if (have_first.compare_exchange_strong(expected, true)) first_wrong = "client " + std::to_string(c) + " round " + std::to_string(round) + ": " + d;
        }
      }
    });
  }
  for (auto& t : clients) t.join();
  const double many_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
  check(wrong.load() == 0, std::to_string(wrong.load()) + " of " + std::to_string(kClients * kRounds) + " concurrent results were wrong; first: " + first_wrong);
  check(pool.dispatches() > before_many, "the concurrent scans went through the pool (dispatch counter moved)");

  std::printf("workers=%zu  single-client %.1f ms  |  %d clients x %d rounds %.1f ms  |  pool dispatches %llu, serial fallbacks %llu\n",
              pool.num_workers(), single_ms, kClients, kRounds, many_ms,
              (unsigned long long)pool.dispatches(), (unsigned long long)pool.serial_fallbacks());
  if (g_failures) { std::printf("%d CHECK(S) FAILED\n", g_failures.load()); return 1; }
  std::printf("all checks held\n");
  return 0;
}
