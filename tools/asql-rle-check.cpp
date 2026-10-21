// Comprehensive test suite for ColumnStore Compact RLE
#include <iostream>
#include <cstdio>
#include <chrono>
#include <string>
#include <vector>
#include <cstdlib>
#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"

#define TEST_ASSERT(cond) do { \
  if (!(cond)) { \
    std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", #cond, __FILE__, __LINE__); \
    std::abort(); \
  } \
} while(0)

using namespace aios;
using namespace aios::userland;

void test_single_run() {
  std::printf("--- Test 1: Single-run column ---\n");
  ColumnStore cs;
  const size_t N = 100000;
  for (size_t i = 0; i < N; ++i) {
    cs.push_back("STATUS_ACTIVE");
  }
  cs.optimize_encoding(SqlDataType::Text);
  TEST_ASSERT(cs.encoding == ColumnEncoding::Constant);
  TEST_ASSERT(cs.get(0) == "STATUS_ACTIVE");
  TEST_ASSERT(cs.get(N - 1) == "STATUS_ACTIVE");

  ColumnStore cs2;
  for (size_t i = 0; i < N; ++i) {
    cs2.push_back(i < 99990 ? "STATUS_ACTIVE" : "STATUS_PENDING");
  }
  cs2.optimize_encoding(SqlDataType::Text);
  TEST_ASSERT(cs2.encoding == ColumnEncoding::RLE);
  TEST_ASSERT(cs2.rle_num_runs == 2);
  TEST_ASSERT(cs2.get(0) == "STATUS_ACTIVE");
  TEST_ASSERT(cs2.get(99989) == "STATUS_ACTIVE");
  TEST_ASSERT(cs2.get(99990) == "STATUS_PENDING");
  TEST_ASSERT(cs2.get(N - 1) == "STATUS_PENDING");
  std::printf("  [PASS] Single run / 2 runs verified (rows=%zu, mem=%zu bytes)\n", N, cs2.estimate_memory_bytes());
}

void test_runs_longer_than_65535() {
  std::printf("--- Test 2: Runs longer than 65535 (escape handling) ---\n");
  ColumnStore cs;
  const size_t R0 = 70000;
  const size_t R1 = 50;
  const size_t R2 = 80000;
  const size_t R3 = 10;
  const size_t total = R0 + R1 + R2 + R3;

  for (size_t i = 0; i < R0; ++i) cs.push_back("STAGE_1");
  for (size_t i = 0; i < R1; ++i) cs.push_back("STAGE_2");
  for (size_t i = 0; i < R2; ++i) cs.push_back("STAGE_3");
  for (size_t i = 0; i < R3; ++i) cs.push_back("STAGE_1");

  cs.optimize_encoding(SqlDataType::Text);
  TEST_ASSERT(cs.encoding == ColumnEncoding::RLE);
  TEST_ASSERT(cs.rle_num_runs == 4);
  TEST_ASSERT(cs.rle_escape_runs.size() >= 2);

  TEST_ASSERT(cs.get(0) == "STAGE_1");
  TEST_ASSERT(cs.get(R0 - 1) == "STAGE_1");
  TEST_ASSERT(cs.get(R0) == "STAGE_2");
  TEST_ASSERT(cs.get(R0 + R1 - 1) == "STAGE_2");
  TEST_ASSERT(cs.get(R0 + R1) == "STAGE_3");
  TEST_ASSERT(cs.get(R0 + R1 + R2 - 1) == "STAGE_3");
  TEST_ASSERT(cs.get(R0 + R1 + R2) == "STAGE_1");
  TEST_ASSERT(cs.get(total - 1) == "STAGE_1");

  TEST_ASSERT(cs.rle_get_length(0) == R0);
  TEST_ASSERT(cs.rle_get_length(1) == R1);
  TEST_ASSERT(cs.rle_get_length(2) == R2);
  TEST_ASSERT(cs.rle_get_length(3) == R3);

  std::printf("  [PASS] Runs > 65535 verified (total_rows=%zu, mem=%zu bytes)\n", total, cs.estimate_memory_bytes());
}

void test_run_boundaries() {
  std::printf("--- Test 3: Run boundaries and random access ---\n");
  ColumnStore cs;
  const size_t num_runs = 500;
  const size_t run_len = 20;
  std::vector<std::string> codes = {"VAL_A", "VAL_B", "VAL_C", "VAL_D", "VAL_E"};

  for (size_t r = 0; r < num_runs; ++r) {
    const std::string& val = codes[r % codes.size()];
    for (size_t j = 0; j < run_len; ++j) {
      cs.push_back(val);
    }
  }

  cs.optimize_encoding(SqlDataType::Text);
  TEST_ASSERT(cs.encoding == ColumnEncoding::RLE);
  TEST_ASSERT(cs.rle_num_runs == num_runs);

  for (size_t r = 0; r < num_runs; ++r) {
    const std::string& expected = codes[r % codes.size()];
    size_t start = r * run_len;
    size_t end = start + run_len;
    TEST_ASSERT(cs.get(start) == expected);
    TEST_ASSERT(cs.get(start + 1) == expected);
    TEST_ASSERT(cs.get(end - 1) == expected);
    TEST_ASSERT(cs.rle_find_run(start) == r);
    TEST_ASSERT(cs.rle_find_run(end - 1) == r);
  }
  std::printf("  [PASS] All 500 run boundaries verified successfully\n");
}

void test_all_distinct_column() {
  std::printf("--- Test 4: All-distinct column (RLE threshold check) ---\n");
  ColumnStore cs;
  const size_t N = 1000;
  for (size_t i = 0; i < N; ++i) {
    cs.push_back("UNIQUE_VAL_" + std::to_string(i));
  }
  cs.optimize_encoding(SqlDataType::Text);
  TEST_ASSERT(cs.encoding != ColumnEncoding::RLE);
  std::printf("  [PASS] All-distinct column correctly avoided RLE (encoding=%d)\n", (int)cs.encoding);
}

void test_memory_reduction_and_profitability() {
  std::printf("--- Test 5: Memory comparison on low-cardinality clustered columns ---\n");
  ColumnStore cs;
  const size_t N = 10000;
  std::vector<std::string> vals = {"CAT", "DOG", "FISH", "BIRD", "HORSE"};
  for (size_t i = 0; i < N; ++i) {
    cs.push_back(vals[(i / 10) % vals.size()]);
  }

  size_t pre_mem = cs.estimate_memory_bytes();
  cs.optimize_encoding(SqlDataType::Text);
  TEST_ASSERT(cs.encoding == ColumnEncoding::RLE);
  size_t post_mem = cs.estimate_memory_bytes();

  std::printf("  10,000 rows, 1,000 runs of 5 values:\n");
  std::printf("    DictBitPacked / pre-opt mem: %zu bytes\n", pre_mem);
  std::printf("    Compact RLE mem: %zu bytes\n", post_mem);
  std::printf("    RLE code bits: %u, num runs: %u\n", (unsigned)cs.rle_code_bits, (unsigned)cs.rle_num_runs);
  TEST_ASSERT(post_mem < pre_mem);

  ColumnStore cs_thresh;
  for (size_t i = 0; i < N; ++i) {
    cs_thresh.push_back(vals[(i / 2) % vals.size()]);
  }
  cs_thresh.optimize_encoding(SqlDataType::Text);
  std::printf("  Threshold test (5,000 runs / 10,000 rows = 50%%): encoding=%s, mem=%zu bytes\n",
              cs_thresh.encoding == ColumnEncoding::RLE ? "RLE" : "DictBitPacked",
              cs_thresh.estimate_memory_bytes());
  std::printf("  [PASS] Memory reduction & profitability verified\n");
}

void test_mutation_materialization() {
  std::printf("--- Test 6: Mutation materialization back to DictBitPacked (set / erase) ---\n");
  ColumnStore cs;
  const size_t N = 1000;
  for (size_t i = 0; i < N; ++i) {
    cs.push_back(i < 500 ? "RED" : "BLUE");
  }
  cs.optimize_encoding(SqlDataType::Text);
  TEST_ASSERT(cs.encoding == ColumnEncoding::RLE);

  cs.set(250, "GREEN");
  TEST_ASSERT(cs.encoding == ColumnEncoding::DictBitPacked);
  TEST_ASSERT(cs.get(250) == "GREEN");
  TEST_ASSERT(cs.get(0) == "RED");
  TEST_ASSERT(cs.get(499) == "RED");
  TEST_ASSERT(cs.get(500) == "BLUE");

  cs.optimize_encoding(SqlDataType::Text);
  TEST_ASSERT(cs.encoding == ColumnEncoding::RLE);

  std::vector<size_t> rows_to_erase = {10, 20, 30};
  cs.erase_rows(rows_to_erase);
  TEST_ASSERT(cs.encoding == ColumnEncoding::DictBitPacked);
  TEST_ASSERT(cs.row_count_ == N - 3);

  std::printf("  [PASS] set() and erase_rows() cleanly materialize to DictBitPacked\n");
}

void test_scan_and_get_speed() {
  std::printf("--- Test 7: Scan and random get() speed benchmark ---\n");
  ColumnStore cs;
  const size_t N = 100000;
  for (size_t i = 0; i < N; ++i) {
    cs.push_back((i / 50) % 2 == 0 ? "YES" : "NO");
  }
  cs.optimize_encoding(SqlDataType::Text);
  TEST_ASSERT(cs.encoding == ColumnEncoding::RLE);

  auto t0 = std::chrono::high_resolution_clock::now();
  size_t yes_count = 0;
  for (size_t i = 0; i < N; ++i) {
    if (cs.get_view(i) == "YES") yes_count++;
  }
  auto t1 = std::chrono::high_resolution_clock::now();
  double scan_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  TEST_ASSERT(yes_count == 50000);

  auto t2 = std::chrono::high_resolution_clock::now();
  size_t rand_hits = 0;
  for (size_t i = 0; i < 10000; ++i) {
    size_t r = (i * 7919) % N;
    if (cs.get_view(r) == "YES") rand_hits++;
  }
  auto t3 = std::chrono::high_resolution_clock::now();
  double rand_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();
  TEST_ASSERT(rand_hits > 0);

  std::printf("  Sequential scan 100k rows: %.2f ms (%.1f M rows/s)\n", scan_ms, (N / 1e6) / (scan_ms / 1000.0));
  std::printf("  Random lookup 10k rows:    %.2f ms (%.1f M lookups/s)\n", rand_ms, (10000 / 1e6) / (rand_ms / 1000.0));
  std::printf("  [PASS] Speed benchmark completed\n");
}

void test_query_and_checkpoint() {
  std::printf("--- Test 8: SQL queries (WHERE, GROUP BY) & checkpoint restore ---\n");
  KernelConfig cfg;
  cfg.root = "/tmp/aios-asql-rle-test";
  auto k_res = Kernel::boot(cfg);
  TEST_ASSERT(k_res.ok());
  SqlDatabase db(**k_res);
  db.init("sql_db");

  db.execute_local_query("DROP TABLE IF EXISTS orders;");
  db.execute_local_query("CREATE TABLE orders (id INT, status VARCHAR(20), priority VARCHAR(10));");

  std::vector<std::string> statuses = {"OPEN", "PROCESSING", "SHIPPED", "DELIVERED"};
  for (int i = 0; i < 2000; ++i) {
    char sql[256];
    const char* st = statuses[(i / 50) % statuses.size()].c_str();
    const char* pr = (i % 2 == 0) ? "HIGH" : "LOW";
    std::snprintf(sql, sizeof(sql), "INSERT INTO orders VALUES (%d, '%s', '%s');", i, st, pr);
    db.execute_local_query(sql);
  }

  SqlTable* tbl = db.resolve_table("orders", "aios_db");
  TEST_ASSERT(tbl != nullptr);

  for (size_t c = 0; c < tbl->col_data.size(); ++c) {
    tbl->col_data[c].optimize_encoding(tbl->columns[c].type);
  }

  TEST_ASSERT(tbl->col_data[1].encoding == ColumnEncoding::RLE);
  std::printf("  status column encoded as: RLE (%u runs, %u bits)\n",
              tbl->col_data[1].rle_num_runs, (unsigned)tbl->col_data[1].rle_code_bits);

  auto res_where = db.execute_local_query("SELECT count(*) as cnt FROM orders WHERE status = 'SHIPPED';");
  TEST_ASSERT(!res_where.rows.empty());
  TEST_ASSERT(res_where.rows[0].values[0] == "500");

  auto res_grp = db.execute_local_query("SELECT status, count(*) as cnt FROM orders GROUP BY status;");
  TEST_ASSERT(res_grp.rows.size() == 4);
  for (const auto& row : res_grp.rows) {
    TEST_ASSERT(row.values[1] == "500");
  }

  auto save_res = db.flush_now();
  TEST_ASSERT(save_res.ok());
  std::printf("  Flushed checkpoint v4 successfully\n");

  SqlDatabase db2(**k_res);
  db2.init("sql_db");
  bool load_ok = db2.load_tables_v4();
  TEST_ASSERT(load_ok);
  SqlTable* tbl2 = db2.resolve_table("orders", "aios_db");
  TEST_ASSERT(tbl2 != nullptr);
  std::printf("  tbl2 row_count: %zu, encoding: %d\n", tbl2->row_count(), (int)tbl2->col_data[1].encoding);
  TEST_ASSERT(tbl2->row_count() == 2000);
  TEST_ASSERT(tbl2->col_data[1].encoding == ColumnEncoding::RLE);
  TEST_ASSERT(tbl2->col_data[1].rle_num_runs == 40);

  for (int i = 0; i < 2000; ++i) {
    const std::string& expected_st = statuses[(i / 50) % statuses.size()];
    TEST_ASSERT(tbl2->col_data[1].get(i) == expected_st);
  }

  auto res_after = db2.execute_local_query("SELECT status, count(*) FROM orders WHERE status = 'PROCESSING' GROUP BY status;");
  TEST_ASSERT(!res_after.rows.empty());
  TEST_ASSERT(res_after.rows[0].values[1] == "500");

  std::printf("  [PASS] Queries and checkpoint save/restore verified successfully\n");
}

int main() {
  std::printf("=== Running ColumnStore Compact RLE Suite ===\n");
  test_single_run();
  test_runs_longer_than_65535();
  test_run_boundaries();
  test_all_distinct_column();
  test_memory_reduction_and_profitability();
  test_mutation_materialization();
  test_scan_and_get_speed();
  test_query_and_checkpoint();
  std::printf("=== ALL COMPACT RLE TESTS PASSED! ===\n");
  return 0;
}
