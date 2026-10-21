// What git leaves out: .gitignore files, .git/info/exclude and core.excludesFile,
// matched with git's wildmatch rules (dir.c + wildmatch.c), not a regex guess.
#include "aios/userland/git.hpp"

#include <cctype>
#include <cstring>
#include "aios/hal/host.hpp"

namespace aios::userland {

namespace {

char fold(char c, bool icase) { return icase ? (char)std::tolower((unsigned char)c) : c; }

// One bracket expression starting just after '['. Returns the position after ']' and whether `c` matched,
// or npos when the bracket never closes (then '[' is an ordinary character, as in git).
size_t match_class(std::string_view p, size_t i, char c, bool icase, bool& matched) {
  bool negate = false;
  if (i < p.size() && (p[i] == '!' || p[i] == '^')) { negate = true; ++i; }
  bool hit = false;
  bool first = true;
  const char fc = fold(c, icase);
  while (i < p.size()) {
    char pc = p[i];
    if (pc == ']' && !first) { matched = hit != negate; return i + 1; }
    first = false;
    if (pc == '[' && i + 1 < p.size() && p[i + 1] == ':') {
      size_t end = p.find(":]", i + 2);
      if (end == std::string_view::npos) return std::string_view::npos;
      std::string_view name = p.substr(i + 2, end - i - 2);
      unsigned char u = (unsigned char)c;
      bool in = name == "alnum" ? std::isalnum(u) : name == "alpha" ? std::isalpha(u) : name == "blank" ? (c == ' ' || c == '\t')
              : name == "cntrl" ? std::iscntrl(u) : name == "digit" ? std::isdigit(u) : name == "graph" ? std::isgraph(u)
              : name == "lower" ? (std::islower(u) || (icase && std::isupper(u))) : name == "print" ? std::isprint(u)
              : name == "punct" ? std::ispunct(u) : name == "space" ? std::isspace(u)
              : name == "upper" ? (std::isupper(u) || (icase && std::islower(u))) : name == "xdigit" ? std::isxdigit(u) : false;
      if (in) hit = true;
      i = end + 2;
      continue;
    }
    if (pc == '\\' && i + 1 < p.size()) pc = p[++i];
    char lo = pc;
    if (i + 2 < p.size() && p[i + 1] == '-' && p[i + 2] != ']') {
      char hi = p[i + 2];
      size_t adv = 3;
      if (hi == '\\' && i + 3 < p.size()) { hi = p[i + 3]; adv = 4; }
      if (c >= lo && c <= hi) hit = true;
      if (icase) {
        char l = (char)std::tolower((unsigned char)c), u = (char)std::toupper((unsigned char)c);
        if ((l >= lo && l <= hi) || (u >= lo && u <= hi)) hit = true;
      }
      i += adv;
      continue;
    }
    if (fold(lo, icase) == fc) hit = true;
    ++i;
  }
  return std::string_view::npos;
}

// wildmatch with WM_PATHNAME: '*' and '?' never cross '/', and "**" between slashes (or at either end)
// spans any number of directories.
bool wild(std::string_view p, std::string_view t, bool icase) {
  size_t pi = 0, ti = 0;
  while (pi < p.size()) {
    char pc = p[pi];
    if (pc == '*') {
      size_t stars = pi;
      while (pi < p.size() && p[pi] == '*') ++pi;
      const bool doubled = pi - stars >= 2;
      const bool at_start = stars == 0 || p[stars - 1] == '/';
      if (doubled && at_start && (pi == p.size() || p[pi] == '/')) {
        if (pi == p.size()) return true;                          // "foo/**": everything below
        std::string_view rest = p.substr(pi + 1);                 // "**/rest": zero or more directories
        if (wild(rest, t.substr(ti), icase)) return true;
        for (size_t k = ti; k < t.size(); ++k)
          if (t[k] == '/' && wild(rest, t.substr(k + 1), icase)) return true;
        return false;
      }
      std::string_view rest = p.substr(pi);
      for (size_t k = ti;; ++k) {
        if (wild(rest, t.substr(k), icase)) return true;
        if (k >= t.size() || t[k] == '/') return false;
      }
    }
    if (ti >= t.size()) return false;
    char tc = t[ti];
    if (pc == '?') {
      if (tc == '/') return false;
      ++pi; ++ti;
      continue;
    }
    if (pc == '[') {
      if (tc == '/') return false;
      bool matched = false;
      size_t after = match_class(p, pi + 1, tc, icase, matched);
      if (after != std::string_view::npos) {
        if (!matched) return false;
        pi = after; ++ti;
        continue;
      }
      // an unclosed '[' is literal
    }
    if (pc == '\\' && pi + 1 < p.size()) pc = p[++pi];
    if (fold(pc, icase) != fold(tc, icase)) return false;
    ++pi; ++ti;
  }
  return ti == t.size();
}

std::string base_name(const std::string& path) {
  size_t s = path.rfind('/');
  return s == std::string::npos ? path : path.substr(s + 1);
}

}  // namespace

bool git_wildmatch(std::string_view pattern, std::string_view text, bool icase) { return wild(pattern, text, icase); }

void GitIgnore::add_patterns(const std::string& text, const std::string& base) {
  size_t at = 0;
  while (at <= text.size()) {
    size_t nl = text.find('\n', at);
    std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
    at = nl == std::string::npos ? text.size() + 1 : nl + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    // Trailing spaces go unless escaped with a backslash.
    while (!line.empty() && line.back() == ' ') {
      if (line.size() >= 2 && line[line.size() - 2] == '\\') { line.erase(line.size() - 2, 1); break; }
      line.pop_back();
    }
    if (line.empty()) continue;
    Pattern pt;
    pt.base = base;
    if (line[0] == '!') { pt.negate = true; line.erase(0, 1); }
    else if (line[0] == '\\' && line.size() > 1 && (line[1] == '!' || line[1] == '#')) line.erase(0, 1);
    if (!line.empty() && line.back() == '/') { pt.dir_only = true; line.pop_back(); }
    if (line.empty()) continue;
    pt.basename_only = line.find('/') == std::string::npos;
    if (line[0] == '/') line.erase(0, 1);
    pt.glob = line;
    patterns_.push_back(std::move(pt));
  }
}

void GitIgnore::load(hal::Host& host, const std::string& work, const std::string& git_dir,
                     const std::string& excludes_file, bool icase) {
  host_ = &host;
  work_ = work;
  icase_ = icase;
  patterns_.clear();
  loaded_.clear();
  auto read = [&](const std::string& p) -> std::string {
    auto r = host.read_file(p);
    return r.ok() ? std::string((const char*)r->data(), r->size()) : std::string();
  };
  if (!excludes_file.empty()) add_patterns(read(excludes_file), "");
  add_patterns(read(git_dir + "/info/exclude"), "");
}

void GitIgnore::enter(const std::string& rel_dir) {
  if (!host_ || loaded_.count(rel_dir)) return;
  loaded_.insert(rel_dir);
  std::string file = (rel_dir.empty() ? work_ : work_ + "/" + rel_dir) + "/.gitignore";
  auto r = host_->read_file(file);
  if (r.ok()) add_patterns(std::string((const char*)r->data(), r->size()), rel_dir);
}

int GitIgnore::verdict(const std::string& rel, bool is_dir) const {
  // The last pattern that matches decides; patterns from deeper directories were added later.
  const std::string name = base_name(rel);
  for (size_t i = patterns_.size(); i-- > 0;) {
    const Pattern& pt = patterns_[i];
    if (pt.dir_only && !is_dir) continue;
    if (!pt.base.empty() && rel.compare(0, pt.base.size() + 1, pt.base + "/") != 0) continue;
    bool hit;
    if (pt.basename_only) hit = wild(pt.glob, name, icase_);
    else hit = wild(pt.glob, pt.base.empty() ? std::string_view(rel) : std::string_view(rel).substr(pt.base.size() + 1), icase_);
    if (hit) return pt.negate ? -1 : 1;
  }
  return 0;
}

bool GitIgnore::ignored(const std::string& rel, bool is_dir) {
  // A path inside an ignored directory is ignored whatever its own patterns say: git never looks inside.
  std::string acc;
  size_t at = 0;
  enter("");
  while (true) {
    size_t slash = rel.find('/', at);
    if (slash == std::string::npos) break;
    acc = rel.substr(0, slash);
    if (verdict(acc, true) > 0) return true;
    enter(acc);
    at = slash + 1;
  }
  return verdict(rel, is_dir) > 0;
}

}  // namespace aios::userland
