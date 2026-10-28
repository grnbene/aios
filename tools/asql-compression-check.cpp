// Verifies ASQL's columnar compression: data round-trips intact and the compressed form is
// smaller.

#include <iostream>
#include <cstdio>
#include <chrono>
#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"

using namespace aios;
using namespace aios::userland;

int main() {
  std::printf("=== AIOS ASQL Columnar Compression Verification Suite ===\n");

  KernelConfig cfg;
  cfg.root = "/tmp/aios-asql-comp-test";
  auto k_res = Kernel::boot(cfg);
  if (!k_res.ok()) {
    std::printf("Failed to boot kernel\n");
    return 1;
  }
  SqlDatabase db(**k_res);
  db.init("sql_db");

  // Create synthetic salesorders-like ERP table
  db.execute_local_query("DROP TABLE IF EXISTS salesorders;");
  db.execute_local_query(
    "CREATE TABLE salesorders ("
    "  orderno INT NOT NULL,"
    "  debtorno VARCHAR(20) NOT NULL,"
    "  branchcode VARCHAR(20) NOT NULL,"
    "  customerref VARCHAR(50),"
    "  buyername VARCHAR(50),"
    "  comments VARCHAR(200),"
    "  orddate DATE NOT NULL,"
    "  ordertype VARCHAR(2) NOT NULL,"
    "  shipvia INT NOT NULL,"
    "  deladd1 VARCHAR(100),"
    "  contactphone VARCHAR(30),"
    "  contactemail VARCHAR(100),"
    "  deliverto VARCHAR(100),"
    "  freightcost DECIMAL(10,2),"
    "  fromstkloc VARCHAR(5) NOT NULL,"
    "  deliverydate DATE,"
    "  payment_details TEXT"
    ");"
  );

  std::printf("[Step 1] Populating 10,000 synthetic ERP salesorder rows...\n");
  const size_t N = 10000;
  for (size_t i = 1; i <= N; ++i) {
    char sql[1024];
    // orderno (high cardinality int)
    // debtorno (ndv ~ 200, low cardinality)
    // fromstkloc (ndv == 1, constant 'MAIN')
    // ordertype (ndv == 2, 'A' or 'B')
    // payment_details (fat text > 150 chars)
    std::string fat_payment = "CARD: VISA-4111222233334444 AUTH: 981273 REF: TXN-" + std::to_string(i) +
                              " GATEWAY: STRIPE STATUS: SETTLED AT 2026-09-17T13:00:00Z NOTES: Standard 30-day net terms with batch invoice delivery";
    std::snprintf(sql, sizeof(sql),
      "INSERT INTO salesorders VALUES ("
      "  %zu, 'CUST-%zu', 'BR-%zu', 'PO-%zu', 'Buyer %zu', 'Standard Delivery', '2026-09-17', '%s', 1, "
      "  '123 Industrial Pkwy', '555-0199', 'billing@customer.com', 'Warehouse Dock 4', 15.50, 'MAIN', '2026-09-18', '%s'"
      ");",
      i, i % 200, i % 50, i, i % 1000, (i % 2 == 0 ? "A" : "B"), fat_payment.c_str()
    );
    db.execute_local_query(sql);
  }

  std::printf("  -> Inserted %zu rows successfully.\n", N);

  // Introspect table and apply optimize_encoding
  SqlTable* tbl = db.resolve_table("salesorders", "aios_db");
  if (!tbl) {
    std::printf("FAILED: could not resolve salesorders table!\n");
    return 1;
  }

  std::printf("\n[Step 2] Measuring Uncompressed vs Compressed Column Footprint...\n");
  size_t pre_mem = 0;
  for (const auto& cs : tbl->col_data) pre_mem += cs.estimate_memory_bytes();
  std::printf("  Pre-optimization estimated column memory: %zu KB (%zu bytes/row)\n", pre_mem / 1024, pre_mem / N);

  for (size_t c = 0; c < tbl->col_data.size(); ++c) {
    SqlDataType t = (c < tbl->columns.size()) ? tbl->columns[c].type : SqlDataType::Text;
    tbl->col_data[c].optimize_encoding(t, 0, c < tbl->columns.size() ? tbl->columns[c].temporal : 0);
  }

  size_t post_mem = 0;
  std::printf("\n[Step 3] Per-Column Encodings Applied:\n");
  for (size_t c = 0; c < tbl->col_data.size(); ++c) {
    const auto& cs = tbl->col_data[c];
    size_t col_mem = cs.estimate_memory_bytes();
    post_mem += col_mem;
    const char* enc_name = "Unknown";
    switch (cs.encoding) {
      case ColumnEncoding::Constant: enc_name = "Constant (0 bits/row)"; break;
      case ColumnEncoding::DictBitPacked: enc_name = "DictBitPacked"; break;
      case ColumnEncoding::TypedInt32: enc_name = "TypedInt32"; break;
      case ColumnEncoding::TypedInt64: enc_name = "TypedInt64"; break;
      case ColumnEncoding::TypedDouble: enc_name = "TypedDouble"; break;
      case ColumnEncoding::RawArena: enc_name = "RawArena"; break;
      case ColumnEncoding::BlockCompressed: enc_name = "BlockCompressed (LZ4 64KB)"; break;
      case ColumnEncoding::FOR: enc_name = "FOR (Frame-of-Ref Deltas)"; break;
      case ColumnEncoding::RLE: enc_name = "RLE (Run-Length Encoded)"; break;
      case ColumnEncoding::DecimalScaled: enc_name = "DecimalScaled (Fixed-Pt FOR)"; break;
      case ColumnEncoding::FSST: enc_name = "FSST Symbol Table (256-dict)"; break;
      case ColumnEncoding::SegmentFOR: enc_name = "SegmentFOR (per-segment FOR)"; break;
      case ColumnEncoding::ALP: enc_name = "ALP (decimal doubles)"; break;
      case ColumnEncoding::TemporalDays: enc_name = "TemporalDays (dates)"; break;
      case ColumnEncoding::TemporalSeconds: enc_name = "TemporalSeconds (datetimes)"; break;
    }
    std::printf("  Col %2zu: %-16s | Encoding: %-28s | Width: %2u-bit | Mem: %5zu KB\n",
                c, tbl->columns[c].name.c_str(), enc_name, (unsigned)cs.bit_width_, col_mem / 1024);
  }

  std::printf("\n  Post-optimization estimated column memory: %zu KB (%zu bytes/row)\n", post_mem / 1024, post_mem / N);
  double ratio = post_mem > 0 ? ((double)pre_mem / (double)post_mem) : 1.0;
  std::printf("  Compression Ratio: %.2fx Reduction!\n", ratio);

  std::printf("\n[Step 4] Verifying Data Parity & Query Execution Across Compressed Columns...\n");
  auto res = db.execute_local_query("SELECT count(*) as cnt, min(orderno) as min_o, max(orderno) as max_o FROM salesorders;");
  std::printf("  Aggregate result: %s\n", res.formatted_table.c_str());
  if (res.rows.empty() || res.rows[0].values[0] != "10000") {
    std::printf("FAILED: expected 10000 rows!\n");
    return 1;
  }

  auto res_sample = db.execute_local_query("SELECT orderno, debtorno, fromstkloc, ordertype, payment_details FROM salesorders WHERE orderno = 42;");
  std::printf("  Sample Row 42: %s\n", res_sample.formatted_table.c_str());
  if (res_sample.rows.empty() || res_sample.rows[0].values[2] != "MAIN" || res_sample.rows[0].values[3] != "A") {
    std::printf("FAILED: row data parity mismatch!\n");
    return 1;
  }

  std::printf("\n[Step 5] Direct DictArena Unit Verification...\n");
  // Test A: Empty strings & mixed transitions
  {
    DictArena da;
    da.push_back(sql_null()); // entry 0
    da.push_back("");         // entry 1
    da.push_back("alpha");    // entry 2
    da.push_back("alphabet"); // entry 3
    da.push_back("");         // entry 4
    da.push_back("beta");     // entry 5
    da.front_code(nullptr, 16);
    if (da.size() != 6 || da.view(0) != sql_null() || da.view(1) != "" || da.view(2) != "alpha" ||
        da.view(3) != "alphabet" || da.view(4) != "" || da.view(5) != "beta") {
      std::printf("FAILED: DictArena empty string test!\n");
      return 1;
    }
    std::printf("  [PASS] Empty strings and transitions round-trip accurately.\n");
  }

  // Test B: Bucket boundaries (15, 16, 17, 31, 32, 33, 48, 64)
  for (size_t count : {15, 16, 17, 31, 32, 33, 48, 64}) {
    DictArena da;
    da.push_back(sql_null());
    std::vector<std::string> expected;
    expected.push_back(sql_null());
    for (size_t i = 1; i <= count; ++i) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "boundary_item_%04zu", i);
      da.push_back(buf);
      expected.push_back(buf);
    }
    da.front_code(nullptr, 16);
    if (da.size() != count + 1) {
      std::printf("FAILED: boundary size mismatch for count %zu\n", count);
      return 1;
    }
    for (size_t i = 0; i <= count; ++i) {
      if (da.view(i) != expected[i] || da[i] != expected[i]) {
        std::printf("FAILED: boundary mismatch for count %zu at idx %zu: got %.*s, expected %s\n",
                    count, i, (int)da.view(i).size(), da.view(i).data(), expected[i].c_str());
        return 1;
      }
    }
  }
  std::printf("  [PASS] Bucket boundaries (15, 16, 17, 31, 32, 33, 48, 64) verified intact.\n");

  // Test C: Prefix-heavy sets (deep paths / URLs)
  {
    DictArena da;
    da.push_back(sql_null());
    for (size_t i = 0; i < 500; ++i) {
      char buf[128];
      std::snprintf(buf, sizeof(buf), "/api/v1/customers/enterprise/region/us-east-1/tenant-%04zu/orders", i);
      da.push_back(buf);
    }
    size_t uncomp_mem = da.memory_bytes();
    da.front_code(nullptr, 16);
    size_t comp_mem = da.memory_bytes();
    std::printf("  Prefix-heavy: uncompressed = %zu bytes, front-coded = %zu bytes (%.2fx reduction)\n",
                uncomp_mem, comp_mem, (double)uncomp_mem / (double)comp_mem);
    if (comp_mem >= uncomp_mem) {
      std::printf("FAILED: prefix-heavy did not compress!\n");
      return 1;
    }
    for (size_t i = 0; i < 500; ++i) {
      char buf[128];
      std::snprintf(buf, sizeof(buf), "/api/v1/customers/enterprise/region/us-east-1/tenant-%04zu/orders", i);
      if (da.view(i + 1) != buf) {
        std::printf("FAILED: prefix-heavy view mismatch at %zu\n", i);
        return 1;
      }
    }
    std::printf("  [PASS] Prefix-heavy compression verified (%.2fx reduction).\n", (double)uncomp_mem / (double)comp_mem);
  }

  // Test D: Prefix-free sets (hex hashes / random strings)
  {
    DictArena da;
    da.push_back(sql_null());
    for (size_t i = 0; i < 300; ++i) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%c_hex_%06zx_%zu", (char)('A' + (i % 26)), (i * 999983) & 0xffffff, i);
      da.push_back(buf);
    }
    size_t uncomp_mem = da.memory_bytes();
    da.front_code(nullptr, 16);
    size_t comp_mem = da.memory_bytes();
    std::printf("  Prefix-free: uncompressed = %zu bytes, front-coded = %zu bytes\n", uncomp_mem, comp_mem);
    for (size_t i = 0; i < 300; ++i) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%c_hex_%06zx_%zu", (char)('A' + (i % 26)), (i * 999983) & 0xffffff, i);
      if (da.view(i + 1) != buf) {
        std::printf("FAILED: prefix-free view mismatch at %zu\n", i);
        return 1;
      }
    }
    std::printf("  [PASS] Prefix-free sets round-trip accurately.\n");
  }

  // Test E: Point lookup timing & find_sorted binary search
  {
    DictArena da;
    da.push_back(sql_null());
    for (size_t i = 0; i < 1000; ++i) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "sku_%05zu", i);
      da.push_back(buf);
    }
    da.front_code(nullptr, 16);

    auto t0 = std::chrono::high_resolution_clock::now();
    size_t check_len = 0;
    const size_t lookups = 100000;
    for (size_t k = 0; k < lookups; ++k) {
      size_t idx = (k % 1000) + 1;
      check_len += da.view(idx).size();
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double ns_per_lookup = std::chrono::duration<double, std::nano>(t1 - t0).count() / lookups;
    std::printf("  Point lookup performance: %.1f ns / lookup (checksum %zu)\n", ns_per_lookup, check_len);

    // Test find_sorted
    for (size_t i = 0; i < 1000; ++i) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "sku_%05zu", i);
      uint32_t found = da.find_sorted(buf);
      if (found != i + 1) {
        std::printf("FAILED: find_sorted on %s returned %u, expected %zu\n", buf, found, i + 1);
        return 1;
      }
    }
    if (da.find_sorted("nonexistent") != UINT32_MAX) {
      std::printf("FAILED: find_sorted returned non-null for nonexistent\n");
      return 1;
    }
    std::printf("  [PASS] find_sorted binary search exact match verified.\n");
  }

  // Test F: ORDER BY execution & timing
  {
    auto t_start = std::chrono::high_resolution_clock::now();
    auto order_res = db.execute_local_query("SELECT customerref FROM salesorders ORDER BY customerref LIMIT 50;");
    auto t_end = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();
    std::printf("  ORDER BY customerref LIMIT 50 query time: %.2f ms (returned %zu rows)\n", ms, order_res.rows.size());
    if (order_res.rows.size() != 50) {
      std::printf("FAILED: ORDER BY query returned %zu rows, expected 50\n", order_res.rows.size());
      return 1;
    }
    for (size_t i = 1; i < order_res.rows.size(); ++i) {
      if (order_res.rows[i].values[0] < order_res.rows[i - 1].values[0]) {
        std::printf("FAILED: ORDER BY rows not in sorted order: %s < %s\n",
                    order_res.rows[i].values[0].c_str(), order_res.rows[i - 1].values[0].c_str());
        return 1;
      }
    }
    std::printf("  [PASS] ORDER BY query returned strictly sorted rows without regression.\n");
  }

  std::printf("=== ALL COLUMNAR COMPRESSION TESTS PASSED SUCCESSFULLY ===\n");
  return 0;
}
