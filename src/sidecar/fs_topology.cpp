#include "aios/hal/host.hpp"
#include "aios/sidecar/fs_topology.hpp"
#include "aios/hal/sync.hpp"
#include <sstream>
#include <queue>

namespace aios::sidecar {

FsTopologyMirror::FsTopologyMirror() {
  root_ = std::make_unique<FsRadixNode>();
  root_->name = "/";
  root_->meta.is_directory = true;
  total_nodes_ = 1;
}

std::vector<std::string> FsTopologyMirror::split_path(const std::string& path) {
  std::vector<std::string> segments;
  std::string cur;
  for (char c : path) {
    if (c == '/') {
      if (!cur.empty()) {
        segments.push_back(std::move(cur));
        cur.clear();
      }
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) {
    segments.push_back(std::move(cur));
  }
  return segments;
}

FsRadixNode* FsTopologyMirror::find_node_locked(const std::vector<std::string>& segments) const {
  FsRadixNode* cur = root_.get();
  for (const auto& seg : segments) {
    auto it = cur->children.find(seg);
    if (it == cur->children.end()) return nullptr;
    cur = it->second.get();
  }
  return cur;
}

void FsTopologyMirror::mark_dirty_upwards(FsRadixNode* node) {
  FsRadixNode* cur = node;
  while (cur != nullptr) {
    cur->size_dirty = true;
    cur = cur->parent;
  }
}

bool FsTopologyMirror::upsert_entry(const std::string& path, const FsMetadata& meta) {
  std::unique_lock<aios::hal::SharedMutex> lock(mutex_);
  auto segments = split_path(path);
  if (segments.empty()) {
    root_->meta = meta;
    root_->meta.is_directory = true;
    mark_dirty_upwards(root_.get());
    return true;
  }

  FsRadixNode* cur = root_.get();
  for (size_t i = 0; i < segments.size(); ++i) {
    const auto& seg = segments[i];
    auto it = cur->children.find(seg);
    if (it == cur->children.end()) {
      auto new_node = std::make_unique<FsRadixNode>();
      new_node->name = seg;
      new_node->parent = cur;
      if (i + 1 < segments.size()) {
        new_node->meta.is_directory = true;
      } else {
        new_node->meta = meta;
      }
      FsRadixNode* raw_ptr = new_node.get();
      cur->children[seg] = std::move(new_node);
      total_nodes_++;
      cur = raw_ptr;
    } else {
      cur = it->second.get();
      if (i + 1 == segments.size()) {
        cur->meta = meta;
      }
    }
  }

  mark_dirty_upwards(cur);
  return true;
}

bool FsTopologyMirror::remove_entry(const std::string& path) {
  std::unique_lock<aios::hal::SharedMutex> lock(mutex_);
  auto segments = split_path(path);
  if (segments.empty()) return false;

  std::string leaf = segments.back();
  segments.pop_back();

  FsRadixNode* parent = find_node_locked(segments);
  if (!parent) return false;

  auto it = parent->children.find(leaf);
  if (it == parent->children.end()) return false;

  // Subtract count
  std::queue<FsRadixNode*> q;
  q.push(it->second.get());
  size_t removed_count = 0;
  while (!q.empty()) {
    auto* n = q.front();
    q.pop();
    removed_count++;
    for (auto& child : n->children) {
      q.push(child.second.get());
    }
  }

  parent->children.erase(it);
  if (total_nodes_ >= removed_count) total_nodes_ -= removed_count;
  mark_dirty_upwards(parent);
  return true;
}

std::optional<FsMetadata> FsTopologyMirror::fast_lstat(const std::string& path) const {
  std::shared_lock<aios::hal::SharedMutex> lock(mutex_);
  auto segments = split_path(path);
  FsRadixNode* node = find_node_locked(segments);
  if (!node) return std::nullopt;
  return node->meta;
}

void FsTopologyMirror::compute_size_recursive(FsRadixNode* node, DuResult& res) {
  if (!node) return;
  if (node->meta.is_directory) {
    res.total_directories++;
  } else {
    res.total_files++;
    res.total_size_bytes += node->meta.size_bytes;
  }

  for (const auto& pair : node->children) {
    compute_size_recursive(pair.second.get(), res);
  }
}

FsTopologyMirror::DuResult FsTopologyMirror::fast_du(const std::string& root_path) {
  std::shared_lock<aios::hal::SharedMutex> lock(mutex_);
  auto segments = split_path(root_path);
  FsRadixNode* node = find_node_locked(segments);
  DuResult res;
  if (!node) return res;

  if (!node->size_dirty && node->cached_recursive_count > 0) {
    res.total_size_bytes = node->cached_recursive_size;
    res.total_files = node->cached_recursive_count;
    res.total_directories = 1;
    return res;
  }

  compute_size_recursive(node, res);
  node->cached_recursive_size = res.total_size_bytes;
  node->cached_recursive_count = res.total_files;
  node->size_dirty = false;
  return res;
}

std::vector<std::string> FsTopologyMirror::fast_find(const std::string& root_path,
                                                    const std::string& pattern_suffix,
                                                    size_t max_results) const {
  std::shared_lock<aios::hal::SharedMutex> lock(mutex_);
  std::vector<std::string> results;
  auto segments = split_path(root_path);
  FsRadixNode* start_node = find_node_locked(segments);
  if (!start_node) return results;

  struct Frame {
    FsRadixNode* node;
    std::string current_path;
  };

  std::vector<Frame> stack;
  stack.push_back({start_node, root_path == "/" ? "" : root_path});

  while (!stack.empty() && results.size() < max_results) {
    Frame f = stack.back();
    stack.pop_back();

    for (const auto& pair : f.node->children) {
      std::string child_path = f.current_path + "/" + pair.first;
      if (!pair.second->meta.is_directory) {
        if (pattern_suffix.empty() ||
            (pair.first.size() >= pattern_suffix.size() &&
             pair.first.compare(pair.first.size() - pattern_suffix.size(),
                                pattern_suffix.size(), pattern_suffix) == 0)) {
          results.push_back(child_path);
          if (results.size() >= max_results) break;
        }
      } else {
        stack.push_back({pair.second.get(), child_path});
      }
    }
  }

  return results;
}

size_t FsTopologyMirror::ingest_host_tree(const std::string& host_dir) {
  size_t ingested = 0;
  std::queue<std::string> dirs;
  dirs.push(host_dir);

  while (!dirs.empty()) {
    std::string current = dirs.front();
    dirs.pop();

    auto& host = hal::process_host();
    auto names = host.list_dir(current);
    if (!names.ok()) continue;

    for (const std::string& name : *names) {
      if (name == "." || name == "..") continue;

      std::string full_path = current + "/" + name;
      auto st = host.stat(full_path);
      if (st.ok()) {
        FsMetadata meta;
        meta.inode = st->inode;
        meta.size_bytes = st->size;
        meta.mode = st->mode;
        meta.uid = st->uid;
        meta.gid = st->gid;
        meta.mtime_ns = st->mtime_ns;
        meta.is_directory = st->is_dir;
        meta.is_symlink = st->is_link;

        upsert_entry(full_path, meta);
        ingested++;

        if (meta.is_directory && !meta.is_symlink) {
          dirs.push(full_path);
        }
      }
    }
  }

  return ingested;
}

size_t FsTopologyMirror::total_nodes() const {
  std::shared_lock<aios::hal::SharedMutex> lock(mutex_);
  return total_nodes_;
}

} // namespace aios::sidecar
