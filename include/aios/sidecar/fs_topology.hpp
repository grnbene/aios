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
#include <functional>

namespace aios::sidecar {

struct alignas(32) FsMetadata {
  uint64_t inode{0};
  uint64_t size_bytes{0};
  uint32_t mode{0};
  uint32_t uid{0};
  uint32_t gid{0};
  uint64_t mtime_ns{0};
  bool is_directory{false};
  bool is_symlink{false};
  uint8_t sha256_prefix[8]{0}; // First 8 bytes of content hash if available
};

struct FsRadixNode {
  std::string name;
  FsMetadata meta;
  std::unordered_map<std::string, std::unique_ptr<FsRadixNode>> children;
  FsRadixNode* parent{nullptr};

  uint64_t cached_recursive_size{0};
  uint64_t cached_recursive_count{0};
  bool size_dirty{true};
};

class FsTopologyMirror {
public:
  FsTopologyMirror();
  ~FsTopologyMirror() = default;

  // Insert or update an entry in the topology mirror (e.g. from fanotify / inotify event)
  bool upsert_entry(const std::string& path, const FsMetadata& meta);

  // Remove an entry
  bool remove_entry(const std::string& path);

  // Sub-microsecond lstat equivalent in RAM
  std::optional<FsMetadata> fast_lstat(const std::string& path) const;

  // Fast sub-millisecond directory size & file count rollup (du)
  struct DuResult {
    uint64_t total_size_bytes{0};
    uint64_t total_files{0};
    uint64_t total_directories{0};
  };
  DuResult fast_du(const std::string& root_path = "/");

  // In-memory pattern search / glob (find)
  std::vector<std::string> fast_find(const std::string& root_path,
                                     const std::string& pattern_suffix,
                                     size_t max_results = 1000) const;

  // Ingest an actual host directory tree recursively into the in-memory mirror
  size_t ingest_host_tree(const std::string& host_dir);

  size_t total_nodes() const;

private:
  mutable aios::hal::SharedMutex mutex_;
  std::unique_ptr<FsRadixNode> root_;
  size_t total_nodes_{0};

  static std::vector<std::string> split_path(const std::string& path);
  FsRadixNode* find_node_locked(const std::vector<std::string>& segments) const;
  void mark_dirty_upwards(FsRadixNode* node);
  void compute_size_recursive(FsRadixNode* node, DuResult& res);
};

} // namespace aios::sidecar
