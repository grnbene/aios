// aios/sidecar/vault_workspace.hpp
#pragma once

#include "aios/core/result.hpp"
#include "aios/crypto/sha256.hpp"
#include "aios/hal/sync.hpp"
#include "aios/hal/host.hpp"
#include "aios/sidecar/module_vault.hpp"
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aios::sidecar {

struct MergeReport {
  bool clean{true};
  std::vector<std::string> conflict_paths;
  std::string new_commit_oid;
  // Set (with clean = false) when the commit could not be journaled: nothing
  // was merged, and the caller must not treat the work as saved.
  std::string error;
};

// Represents branch state across concurrent agents in a shared repo
struct WorkspaceBranchState {
  std::string head_commit_oid;
  std::unordered_map<std::string, std::string> files; // rel_path -> sha256_hex
};

class VaultWorkspace {
public:
  VaultWorkspace(ModuleVault& vault,
                 const std::string& repo_name,
                 const std::string& base_branch = "main")
      : vault_(vault),
        repo_name_(repo_name),
        branch_(base_branch) {
    init_from_branch(base_branch);
  }

  // Zero-copy read from staged working set or base commit in RAM
  std::optional<std::span<const uint8_t>> read(const std::string& rel_path) const {
    std::shared_lock<aios::hal::SharedMutex> lock(mutex_);
    auto it = staged_files_.find(rel_path);
    if (it != staged_files_.end()) {
      return vault_.get_blob(it->second);
    }
    auto base_it = base_files_.find(rel_path);
    if (base_it != base_files_.end()) {
      return vault_.get_blob(base_it->second);
    }
    return std::nullopt;
  }

  // Stage file modification into ModuleVault (zero disk I/O)
  void write(const std::string& rel_path, const void* data, size_t len) {
    std::string sha = vault_.store_blob(data, len);
    std::unique_lock<aios::hal::SharedMutex> lock(mutex_);
    staged_files_[rel_path] = sha;
  }

  // Check if a path is currently staged
  bool is_staged(const std::string& rel_path) const {
    std::shared_lock<aios::hal::SharedMutex> lock(mutex_);
    return staged_files_.find(rel_path) != staged_files_.end();
  }

  // 3-way tree merge against the target branch
  MergeReport commit_and_merge(const std::string& target_branch, const std::string& msg) {
    std::unique_lock<aios::hal::SharedMutex> local_lock(mutex_);
    auto& branch_map = get_shared_branch_registry(repo_name_);
    std::unique_lock<aios::hal::SharedMutex> global_lock(get_shared_registry_mutex());

    auto& bstate = branch_map[target_branch];
    MergeReport report;
    report.clean = true;

    // Check for 3-way merge conflicts across all staged files
    for (const auto& [path, staged_hash] : staged_files_) {
      auto base_it = base_files_.find(path);
      auto cur_it = bstate.files.find(path);

      std::string base_hash = (base_it != base_files_.end()) ? base_it->second : "";
      std::string cur_hash = (cur_it != bstate.files.end()) ? cur_it->second : "";

      // If target branch moved since our base snapshot and differs from our edit:
      if (cur_hash != base_hash && cur_hash != staged_hash) {
        report.clean = false;
        report.conflict_paths.push_back(path);
      }
    }

    if (!report.clean) {
      return report;
    }

    // Clean merge: the tree the branch will have once the staged changes land.
    std::unordered_map<std::string, std::string> merged = bstate.files;
    for (const auto& [path, staged_hash] : staged_files_) {
      merged[path] = staged_hash;
    }

    // Generate new commit OID (SHA-256 over parent OID, message, and manifest tree)
    std::string commit_body = "parent:" + bstate.head_commit_oid + "\nmsg:" + msg + "\n";
    for (const auto& [path, hash] : merged) {
      commit_body += path + ":" + hash + "\n";
    }
    std::string new_oid = ModuleVault::compute_sha256(commit_body.data(), commit_body.size());

    // Write-ahead: the commit and every blob it names reach the journal before
    // the branch moves. A crash after this point replays it; a failure here
    // leaves the branch exactly as it was and says so.
    if (!journal_commit(target_branch, msg, new_oid, merged)) {
      report.clean = false;
      report.error = "journal write failed; commit not applied";
      return report;
    }

    bstate.files = std::move(merged);
    bstate.head_commit_oid = new_oid;

    // Update local base snapshot
    base_commit_oid_ = new_oid;
    base_files_ = bstate.files;
    staged_files_.clear();

    report.new_commit_oid = new_oid;
    return report;
  }

  const std::string& repo_name() const { return repo_name_; }
  const std::string& branch() const { return branch_; }
  const std::string& base_commit_oid() const { return base_commit_oid_; }

private:
  // ---- the journal --------------------------------------------------------
  // Off unless AIOS_GIT_WAL_DIR names a directory. One file per repo, appended
  // to and never rewritten. A record is "AVJ1", a u32 length, the payload, and
  // the SHA-256 of the payload: a record cut short by a crash, or altered, fails
  // its check and ends the replay there. Payloads are length-prefixed fields, so
  // a commit message or path may hold any bytes without being able to forge a
  // record. A blob record carries the file's bytes (ModuleVault is memory-only,
  // so a head that only names a hash would recover to content that is gone).
  static std::string journal_path_for_repo(const std::string& repo) {
    const char* env_dir = std::getenv("AIOS_GIT_WAL_DIR");
    if (!env_dir || !*env_dir) return "";
    std::string base_dir(env_dir);
    (void)aios::hal::process_host().make_dir(base_dir);
    static const char* hex = "0123456789abcdef";
    std::string name;   // the repo name is never used raw in a path
    for (unsigned char c : repo) { name += hex[c >> 4]; name += hex[c & 15]; }
    return base_dir + "/repo_" + name + ".journal";
  }

  static void put_u32(std::string& o, uint32_t v) { for (int i = 0; i < 4; ++i) o.push_back((char)((v >> (8 * i)) & 0xFF)); }
  static void put_str(std::string& o, const std::string& v) { put_u32(o, (uint32_t)v.size()); o += v; }

  struct JournalReader {
    const std::string& b;
    size_t p = 0;
    bool ok = true;
    uint32_t u32() {
      if (p + 4 > b.size()) { ok = false; return 0; }
      uint32_t v = 0;
      for (int i = 0; i < 4; ++i) v |= (uint32_t)(uint8_t)b[p + i] << (8 * i);
      p += 4;
      return v;
    }
    std::string str() {
      uint32_t n = u32();
      if (!ok || n > b.size() - p) { ok = false; return {}; }
      std::string s = b.substr(p, n);
      p += n;
      return s;
    }
  };

  static void append_record(std::string& out, const std::string& payload) {
    out += "AVJ1";
    put_u32(out, (uint32_t)payload.size());
    out += payload;
    auto h = aios::crypto::sha256(aios::ByteSpan{reinterpret_cast<const uint8_t*>(payload.data()), payload.size()});
    out.append(reinterpret_cast<const char*>(h.data()), h.size());
  }

  // Blob hashes already in the journal, per repo: a blob is written once.
  static std::mutex& journaled_mu() { static std::mutex m; return m; }
  static std::unordered_map<std::string, std::unordered_set<std::string>>& journaled_blobs() {
    static std::unordered_map<std::string, std::unordered_set<std::string>> m;
    return m;
  }

  bool journal_commit(const std::string& target_branch, const std::string& msg, const std::string& new_oid,
                      const std::unordered_map<std::string, std::string>& files) {
    const std::string jpath = journal_path_for_repo(repo_name_);
    if (jpath.empty()) return true;   // no journal configured
    std::string out;
    std::vector<std::string> fresh;
    {
      std::lock_guard<std::mutex> g(journaled_mu());
      const auto& have = journaled_blobs()[repo_name_];
      std::unordered_set<std::string> seen;
      for (const auto& [path, hash] : files) {
        if (have.count(hash) || !seen.insert(hash).second) continue;
        auto blob = vault_.get_blob(hash);
        if (!blob) return false;   // content not in the vault: this commit could not be recovered
        std::string payload(1, 'B');
        put_str(payload, hash);
        put_str(payload, std::string(reinterpret_cast<const char*>(blob->data()), blob->size()));
        append_record(out, payload);
        fresh.push_back(hash);
      }
    }
    std::string commit(1, 'C');
    put_str(commit, target_branch);
    put_str(commit, new_oid);
    put_str(commit, msg);
    put_u32(commit, (uint32_t)files.size());
    for (const auto& [path, hash] : files) { put_str(commit, path); put_str(commit, hash); }
    append_record(out, commit);
    if (!aios::hal::process_host().append_file(jpath, aios::ByteSpan(reinterpret_cast<const uint8_t*>(out.data()), out.size())).ok()) return false;
    std::lock_guard<std::mutex> g(journaled_mu());
    for (const auto& h : fresh) journaled_blobs()[repo_name_].insert(h);
    return true;
  }

  // Replays the journal into the shared registry and this workspace's vault,
  // once per repo per process. A commit is applied only if every blob it names
  // came back; the replay stops at the first record that fails its check.
  void recover_repo_once() {
    static std::mutex mu;
    static std::unordered_set<std::string> recovered;
    { std::lock_guard<std::mutex> g(mu); if (!recovered.insert(repo_name_).second) return; }
    const std::string jpath = journal_path_for_repo(repo_name_);
    if (jpath.empty()) return;
    auto read_res = aios::hal::process_host().read_file(jpath);
    if (!read_res.ok() || read_res->empty()) return;
    const std::string content(reinterpret_cast<const char*>(read_res->data()), read_res->size());

    struct Commit { std::string branch, oid; std::unordered_map<std::string, std::string> files; };
    std::vector<Commit> commits;
    std::vector<std::pair<std::string, std::string>> blobs;   // hash, bytes
    size_t pos = 0;
    while (pos + 8 <= content.size() && content.compare(pos, 4, "AVJ1") == 0) {
      JournalReader head{content, pos + 4};
      const uint32_t len = head.u32();
      if (!head.ok || len > content.size() - head.p || content.size() - head.p - len < 32) break;
      const std::string payload = content.substr(head.p, len);
      auto h = aios::crypto::sha256(aios::ByteSpan{reinterpret_cast<const uint8_t*>(payload.data()), payload.size()});
      if (std::memcmp(h.data(), content.data() + head.p + len, 32) != 0) break;   // torn or altered: replay ends here
      pos = head.p + len + 32;
      if (payload.empty()) break;
      JournalReader r{payload, 1};
      if (payload[0] == 'B') {
        std::string hash = r.str(), bytes = r.str();
        if (!r.ok) break;
        blobs.emplace_back(std::move(hash), std::move(bytes));
      } else if (payload[0] == 'C') {
        Commit c;
        c.branch = r.str(); c.oid = r.str(); (void)r.str();
        const uint32_t n = r.u32();
        for (uint32_t i = 0; r.ok && i < n; ++i) { std::string path = r.str(); c.files[path] = r.str(); }
        if (!r.ok) break;
        commits.push_back(std::move(c));
      } else {
        break;
      }
    }

    std::unordered_set<std::string> restored;
    for (const auto& [hash, bytes] : blobs) {
      if (vault_.store_blob(bytes.data(), bytes.size()) == hash) restored.insert(hash);
    }
    {
      std::lock_guard<std::mutex> g(journaled_mu());
      auto& have = journaled_blobs()[repo_name_];
      for (const auto& h : restored) have.insert(h);
    }
    auto& branch_map = get_shared_branch_registry(repo_name_);
    std::unique_lock<aios::hal::SharedMutex> global_lock(get_shared_registry_mutex());
    for (auto& c : commits) {
      bool whole = true;
      for (const auto& [path, hash] : c.files) if (!vault_.get_blob(hash)) { whole = false; break; }
      if (!whole) continue;   // a blob did not come back: do not resurrect a head that names missing content
      auto& st = branch_map[c.branch];
      st.head_commit_oid = c.oid;
      st.files = std::move(c.files);
    }
  }

  void init_from_branch(const std::string& branch_name) {
    recover_repo_once();
    auto& branch_map = get_shared_branch_registry(repo_name_);
    std::shared_lock<aios::hal::SharedMutex> lock(get_shared_registry_mutex());
    auto it = branch_map.find(branch_name);
    if (it != branch_map.end()) {
      base_commit_oid_ = it->second.head_commit_oid;
      base_files_ = it->second.files;
    } else {
      base_commit_oid_ = "0000000000000000000000000000000000000000000000000000000000000000";
    }
  }

  static aios::hal::SharedMutex& get_shared_registry_mutex() {
    static aios::hal::SharedMutex s_mutex;
    return s_mutex;
  }

  static std::unordered_map<std::string, WorkspaceBranchState>& get_shared_branch_registry(const std::string& repo) {
    static std::unordered_map<std::string, std::unordered_map<std::string, WorkspaceBranchState>> s_repos;
    static std::mutex s_repos_mu;   // operator[] inserts; guard it (the returned node-based map stays valid)
    std::lock_guard<std::mutex> g(s_repos_mu);
    return s_repos[repo];
  }

  ModuleVault& vault_;
  std::string repo_name_;
  std::string branch_;
  std::string base_commit_oid_;

  mutable aios::hal::SharedMutex mutex_;
  std::unordered_map<std::string, std::string> base_files_;   // path -> sha256
  std::unordered_map<std::string, std::string> staged_files_; // path -> sha256
};

} // namespace aios::sidecar
