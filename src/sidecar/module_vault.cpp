#include "aios/sidecar/module_vault.hpp"
#include "aios/crypto/sha256.hpp"
#include "aios/hal/sync.hpp"
#include <chrono>
#include <queue>
#include <unordered_set>

namespace aios::sidecar {

std::string ModuleVault::compute_sha256(const void* data, size_t len) {
  aios::ByteSpan span{static_cast<const uint8_t*>(data), len};
  auto hash_arr = aios::crypto::sha256(span);
  return aios::crypto::bytes_to_hex(aios::ByteSpan{hash_arr.data(), hash_arr.size()});
}

std::string ModuleVault::store_blob(const void* data, size_t len, const std::string& mime_type) {
  std::string hash = compute_sha256(data, len);

  std::unique_lock<aios::hal::SharedMutex> lock(mutex_);
  total_uncompressed_inputs_ += len;

  auto it = blobs_.find(hash);
  if (it != blobs_.end()) {
    // Already stored, deduplicated!
    return hash;
  }

  auto blob = std::make_shared<ModuleBlob>();
  blob->sha256_hex = hash;
  blob->data.resize(len);
  if (len > 0 && data != nullptr) {
    std::memcpy(blob->data.data(), data, len);
  }
  blob->mime_type = mime_type;
  blob->size_bytes = len;
  blob->created_at_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());

  total_stored_bytes_ += len;
  blobs_[hash] = std::move(blob);
  return hash;
}

std::optional<std::span<const uint8_t>> ModuleVault::get_blob(const std::string& sha256_hex) const {
  std::shared_lock<aios::hal::SharedMutex> lock(mutex_);
  auto it = blobs_.find(sha256_hex);
  if (it == blobs_.end()) return std::nullopt;
  return std::span<const uint8_t>{it->second->data.data(), it->second->data.size()};
}

bool ModuleVault::register_package(const PackageManifest& pkg) {
  std::unique_lock<aios::hal::SharedMutex> lock(mutex_);
  std::string key = pkg.name + "@" + pkg.version;
  packages_[key] = pkg;
  return true;
}

std::optional<PackageManifest> ModuleVault::get_package(const std::string& name, const std::string& version) const {
  std::shared_lock<aios::hal::SharedMutex> lock(mutex_);
  std::string key = name + "@" + version;
  auto it = packages_.find(key);
  if (it == packages_.end()) return std::nullopt;
  return it->second;
}

std::optional<std::span<const uint8_t>> ModuleVault::resolve_package_file(const std::string& pkg_name,
                                                                         const std::string& version,
                                                                         const std::string& relative_path) const {
  std::shared_lock<aios::hal::SharedMutex> lock(mutex_);
  std::string key = pkg_name + "@" + version;
  auto pkg_it = packages_.find(key);
  if (pkg_it == packages_.end()) return std::nullopt;

  auto file_it = pkg_it->second.file_to_blob_hash.find(relative_path);
  if (file_it == pkg_it->second.file_to_blob_hash.end()) return std::nullopt;

  auto blob_it = blobs_.find(file_it->second);
  if (blob_it == blobs_.end()) return std::nullopt;

  return std::span<const uint8_t>{blob_it->second->data.data(), blob_it->second->data.size()};
}

std::vector<std::pair<std::string, std::string>> ModuleVault::resolve_dependency_tree(
    const std::string& root_pkg, const std::string& root_version) const {
  std::shared_lock<aios::hal::SharedMutex> lock(mutex_);
  std::vector<std::pair<std::string, std::string>> resolved;
  std::unordered_set<std::string> visited;
  std::queue<std::pair<std::string, std::string>> q;

  q.push({root_pkg, root_version});
  visited.insert(root_pkg + "@" + root_version);

  while (!q.empty()) {
    auto [name, ver] = q.front();
    q.pop();
    resolved.push_back({name, ver});

    std::string key = name + "@" + ver;
    auto it = packages_.find(key);
    if (it != packages_.end()) {
      for (const auto& [dep_name, dep_ver] : it->second.dependencies) {
        std::string dep_key = dep_name + "@" + dep_ver;
        if (visited.insert(dep_key).second) {
          q.push({dep_name, dep_ver});
        }
      }
    }
  }

  return resolved;
}

size_t ModuleVault::total_unique_blobs() const {
  std::shared_lock<aios::hal::SharedMutex> lock(mutex_);
  return blobs_.size();
}

uint64_t ModuleVault::total_stored_bytes() const {
  std::shared_lock<aios::hal::SharedMutex> lock(mutex_);
  return total_stored_bytes_;
}

uint64_t ModuleVault::total_dedup_savings_bytes() const {
  std::shared_lock<aios::hal::SharedMutex> lock(mutex_);
  if (total_uncompressed_inputs_ >= total_stored_bytes_) {
    return total_uncompressed_inputs_ - total_stored_bytes_;
  }
  return 0;
}

} // namespace aios::sidecar
