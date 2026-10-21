#pragma once

#include <cstdint>
#include "aios/hal/sync.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#if !defined(AIOS_WASI_NO_THREADS)  // (wasm without threads: aios/core/wasi_threads.hpp declares the lock types)
#include <shared_mutex>
#endif
#include <optional>
#include <span>

namespace aios::sidecar {

struct ModuleBlob {
  std::string sha256_hex;
  std::vector<uint8_t> data;
  std::string mime_type;
  uint64_t created_at_ns{0};
  uint64_t size_bytes{0};

  // Optional pre-parsed token / AST representation
  std::vector<uint32_t> token_stream;
};

struct PackageManifest {
  std::string name;
  std::string version;
  std::unordered_map<std::string, std::string> file_to_blob_hash; // "index.js" -> "sha256..."
  std::unordered_map<std::string, std::string> dependencies;      // "lodash" -> "^4.17.21"
  uint64_t total_size_bytes{0};
};

class ModuleVault {
public:
  ModuleVault() = default;
  ~ModuleVault() = default;

  // Store a file/module blob deduplicated by content hash
  std::string store_blob(const void* data, size_t len, const std::string& mime_type = "application/octet-stream");

  // Retrieve a blob view directly from memory without disk IO
  std::optional<std::span<const uint8_t>> get_blob(const std::string& sha256_hex) const;

  // Register a package version and its file mappings in RAM
  bool register_package(const PackageManifest& pkg);

  // Retrieve a package manifest
  std::optional<PackageManifest> get_package(const std::string& name, const std::string& version) const;

  // Resolve a file inside a package
  std::optional<std::span<const uint8_t>> resolve_package_file(const std::string& pkg_name,
                                                               const std::string& version,
                                                               const std::string& relative_path) const;

  // Solve dependency graph in RAM
  std::vector<std::pair<std::string, std::string>> resolve_dependency_tree(const std::string& root_pkg,
                                                                           const std::string& root_version) const;

  size_t total_unique_blobs() const;
  uint64_t total_stored_bytes() const;
  uint64_t total_dedup_savings_bytes() const;

  static std::string compute_sha256(const void* data, size_t len);

private:
  mutable aios::hal::SharedMutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<ModuleBlob>> blobs_;
  std::unordered_map<std::string, PackageManifest> packages_; // key = "name@version"
  uint64_t total_stored_bytes_{0};
  uint64_t total_uncompressed_inputs_{0};
};

} // namespace aios::sidecar
