// Checks the module vault: blobs stored and resolved by their SHA-256, with the same content
// always giving the same hash.

#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "aios/sidecar/module_vault.hpp"

using namespace aios;
using namespace aios::sidecar;

static int failures = 0;

static void check(bool condition, const std::string& name) {
  if (condition) {
    std::cout << "  [PASS] " << name << "\n";
  } else {
    std::cout << "  [FAIL] " << name << "\n";
    failures++;
  }
}

int main() {
  std::cout << "======================================================================\n";
  std::cout << "    AIOS MODULE VAULT CONTENT-ADDRESSED STORAGE BENCHMARK & PROOF     \n";
  std::cout << "      'SOFTWARE BECOMES AN OBJECT' - REAL SHA256 CAS & RESOLUTION     \n";
  std::cout << "======================================================================\n\n";

  ModuleVault vault;

  // 1. Content-Addressed Blob Storage & Deterministic SHA-256
  std::cout << "─── [1] Content-Addressed Blob Storage & Deterministic Hash ──────────\n";
  std::string shared_lib_code = "/* AIOS Native Storage Bridge v2.0 - Optimized SIMD */\n"
                                "void aios_bridge_init() { asm volatile(\"nop\"); }\n";

  std::string hash1 = vault.store_blob(shared_lib_code.data(), shared_lib_code.size(), "application/octet-stream");
  check(!hash1.empty() && hash1.length() == 64, "Stored blob and received valid 64-char SHA256 hash");
  std::cout << "  Stored Blob SHA256 : " << hash1 << "\n";

  auto blob_opt = vault.get_blob(hash1);
  check(blob_opt.has_value(), "Retrieved blob via SHA256 hash lookup");
  check(blob_opt->size() == shared_lib_code.size(), "Retrieved blob matches exact byte size");
  check(std::memcmp(blob_opt->data(), shared_lib_code.data(), shared_lib_code.size()) == 0, "Retrieved blob content is bit-identical");

  // 2. High-Throughput Ingestion & Deduplication
  std::cout << "\n─── [2] Deduplication Ratio & Multi-Version Blob Ingestion ───────────\n";
  const int NUM_COPIES = 1000;
  auto t0 = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < NUM_COPIES; ++i) {
    vault.store_blob(shared_lib_code.data(), shared_lib_code.size(), "application/octet-stream");
  }
  auto t1 = std::chrono::high_resolution_clock::now();
  double ms_ingest = std::chrono::duration<double, std::milli>(t1 - t0).count();

  check(vault.total_unique_blobs() == 1, "1000 duplicate blob ingestions deduplicated into exactly 1 unique CAS entry");
  uint64_t savings = vault.total_dedup_savings_bytes();
  check(savings == shared_lib_code.size() * NUM_COPIES, "Deduplication savings matches 100% of redundant byte volume");

  std::cout << "  Ingested 1,000 duplicate modules in " << std::fixed << std::setprecision(2) << ms_ingest << " ms ("
            << int(1000.0 / (ms_ingest / 1000.0)) << " ingests/sec)\n";
  std::cout << "  Total Unique Stored Bytes : " << vault.total_stored_bytes() << " bytes\n";
  std::cout << "  Deduplicated Savings      : " << savings << " bytes ("
            << (savings / 1024) << " KB saved)\n";

  // 3. Complex Package Manifests & File Paths
  std::cout << "\n─── [3] Multi-File Package Manifest Registration & File Resolution ───\n";
  std::string file_server = "export function start() { console.log('server running'); }";
  std::string file_client = "export function connect() { return true; }";
  std::string file_types = "export type Config = { port: number; };";

  std::string hash_server = vault.store_blob(file_server.data(), file_server.size(), "application/javascript");
  std::string hash_client = vault.store_blob(file_client.data(), file_client.size(), "application/javascript");
  std::string hash_types = vault.store_blob(file_types.data(), file_types.size(), "application/typescript");

  PackageManifest pkg_core;
  pkg_core.name = "@aios/storage-core";
  pkg_core.version = "1.0.0";
  pkg_core.file_to_blob_hash["lib/server.js"] = hash_server;
  pkg_core.file_to_blob_hash["lib/client.js"] = hash_client;
  pkg_core.file_to_blob_hash["lib/types.d.ts"] = hash_types;
  vault.register_package(pkg_core);

  auto res_file = vault.resolve_package_file("@aios/storage-core", "1.0.0", "lib/server.js");
  check(res_file.has_value(), "Resolved package file 'lib/server.js' from CAS");
  check(res_file->size() == file_server.size(), "Resolved file length matches original source");

  // 4. Transitive Dependency DAG Resolution
  std::cout << "\n─── [4] Transitive Dependency Tree Resolution & Graph Traversal ──────\n";
  PackageManifest pkg_asql;
  pkg_asql.name = "@aios/asql-engine";
  pkg_asql.version = "2.1.0";
  pkg_asql.dependencies["@aios/storage-core"] = "1.0.0";
  vault.register_package(pkg_asql);

  PackageManifest pkg_app;
  pkg_app.name = "@aios/erp-sidecar";
  pkg_app.version = "3.0.0";
  pkg_app.dependencies["@aios/asql-engine"] = "2.1.0";
  vault.register_package(pkg_app);

  auto t_dep0 = std::chrono::high_resolution_clock::now();
  auto deps = vault.resolve_dependency_tree("@aios/erp-sidecar", "3.0.0");
  auto t_dep1 = std::chrono::high_resolution_clock::now();
  double us_dep = std::chrono::duration<double, std::micro>(t_dep1 - t_dep0).count();

  check(deps.size() == 3, "Transitive dependency resolution walked full 3-tier DAG");
  check(deps[0].first == "@aios/erp-sidecar", "Root package is @aios/erp-sidecar");
  check(deps[1].first == "@aios/asql-engine", "Second package is @aios/asql-engine");
  check(deps[2].first == "@aios/storage-core", "Terminal dependency is @aios/storage-core");
  std::cout << "  Dependency DAG resolution latency : " << std::fixed << std::setprecision(2) << us_dep << " µs\n";

  std::cout << "\n======================================================================\n";
  std::cout << "  SUMMARY: " << (failures == 0 ? "ALL MODULE VAULT CHECKS PASSED ✅" : "FAILURES DETECTED ❌") << "\n";
  std::cout << "======================================================================\n";

  return failures == 0 ? 0 : 1;
}
