// Sparse checkout and the file-watcher hint: what part of the work tree a repository keeps on disk, and which paths
// a watcher says may have changed (see git.hpp).
#include <algorithm>

#include "aios/userland/git.hpp"

namespace aios::userland {

namespace {
std::string str_of(const ByteBuffer& b) { return std::string((const char*)b.data(), b.size()); }
bool truthy(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s == "true" || s == "yes" || s == "on" || s == "1" || s.empty();
}
std::string trim_line(std::string s) {
  while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
  return s;
}
}  // namespace

void Git::reload_sparse() const {
  sparse_loaded_ = true;
  sparse_on_ = false;
  sparse_recursive_.clear();
  sparse_parents_.clear();
  sparse_patterns_.clear();
  auto on = get_config("core.sparsecheckout");
  if (!on.ok() || !truthy(*on)) return;
  auto cone = get_config("core.sparsecheckoutcone");
  sparse_cone_ = !cone.ok() || truthy(*cone);
  auto f = kernel_.host().read_file(git_ + "/info/sparse-checkout");
  if (!f.ok()) return;   // on, but no patterns yet: everything stays (git warns and checks out everything too)
  sparse_on_ = true;
  std::vector<std::string> lines;
  std::string text = str_of(*f), cur;
  for (char ch : text) {
    if (ch == '\n') { lines.push_back(trim_line(cur)); cur.clear(); } else cur += ch;
  }
  if (!cur.empty()) lines.push_back(trim_line(cur));
  if (sparse_cone_) {
    // git's cone patterns: "/*" and "!/*/" (the top level's files), "/d/" (d, recursively), and "/d/" followed by
    // "!/d/*/" (d's own files only: a parent of something included)
    std::vector<std::string> dirs, files_only;
    for (auto& l : lines) {
      if (l.empty() || l[0] == '#' || l == "/*" || l == "!/*/") continue;
      if (l[0] == '!' && l.size() > 4 && l.compare(l.size() - 3, 3, "/*/") == 0) {
        std::string d = l.substr(2, l.size() - 5);
        files_only.push_back(d);
        continue;
      }
      if (l[0] == '/' && l.back() == '/' && l.size() > 2) dirs.push_back(l.substr(1, l.size() - 2));
    }
    for (auto& d : dirs) {
      if (std::find(files_only.begin(), files_only.end(), d) != files_only.end()) sparse_parents_.push_back(d);
      else sparse_recursive_.push_back(d);
    }
    return;
  }
  for (auto& l : lines) {
    if (l.empty() || l[0] == '#') continue;
    bool neg = l[0] == '!';
    sparse_patterns_.push_back({neg ? l.substr(1) : l, neg});
  }
}

bool Git::sparse() const {
  if (!sparse_loaded_) reload_sparse();
  return sparse_on_;
}

bool Git::in_sparse(const std::string& rel) const {
  if (!sparse_loaded_) reload_sparse();
  if (!sparse_on_) return true;
  if (sparse_cone_) {
    size_t slash = rel.rfind('/');
    if (slash == std::string::npos) return true;   // the top level's files are always there
    std::string dir = rel.substr(0, slash);
    for (auto& r : sparse_recursive_) if (rel.compare(0, r.size() + 1, r + "/") == 0) return true;
    for (auto& p : sparse_parents_) if (dir == p) return true;
    return false;
  }
  // gitignore-style patterns: the last one that matches the path (or a directory above it) decides
  bool in = false;
  for (auto& [pat, neg] : sparse_patterns_) {
    std::string p = pat;
    bool dir_only = !p.empty() && p.back() == '/';
    if (dir_only) p.pop_back();
    bool anchored = !p.empty() && p[0] == '/';
    if (anchored) p = p.substr(1);
    bool hit = false;
    // the path itself, then each directory above it
    std::string cur = rel;
    bool is_dir = false;
    while (true) {
      if (!(dir_only && !is_dir)) {
        if (anchored || p.find('/') != std::string::npos) hit = git_wildmatch(p, cur);
        else {
          size_t s = cur.rfind('/');
          hit = git_wildmatch(p, s == std::string::npos ? cur : cur.substr(s + 1));
        }
      }
      if (hit) break;
      size_t s = cur.rfind('/');
      if (s == std::string::npos) break;
      cur = cur.substr(0, s);
      is_dir = true;
    }
    if (hit) in = !neg;
  }
  return in;
}

bool Git::watch_says_unchanged(const std::string& rel) const {
  if (!watch_hint_) return false;
  if (watch_hint_->count(rel)) return false;
  // a directory event (created, removed, renamed) covers everything under it
  for (size_t s = rel.find('/'); s != std::string::npos; s = rel.find('/', s + 1))
    if (watch_hint_->count(rel.substr(0, s))) return false;
  return true;
}

}  // namespace aios::userland
