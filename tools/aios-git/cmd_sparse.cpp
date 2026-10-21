// git sparse-checkout: which part of the tree the work tree holds (cone mode: directories; or gitignore-style
// patterns), kept in $GIT_DIR/info/sparse-checkout as git keeps it. Paths outside stay in the index marked
// skip-worktree and are not on disk; checkout, switch, reset, merge and the change layer leave them there.
#include <algorithm>
#include <set>
#include <sstream>

#include "aios/hal/host.hpp"
#include "change.hpp"
#include "oplog.hpp"

namespace aios_git {

using namespace aios;

namespace {

std::string bytes_str(const ByteBuffer& b) { return std::string((const char*)b.data(), b.size()); }

std::string sparse_file(Ctx& c) { return c.repo->git_dir() + "/info/sparse-checkout"; }

std::vector<std::string> read_lines(Ctx& c) {
  std::vector<std::string> out;
  auto f = c.host().read_file(sparse_file(c));
  if (!f.ok()) return out;
  std::istringstream ss(bytes_str(*f));
  std::string l;
  while (std::getline(ss, l)) {
    while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
    if (!l.empty()) out.push_back(l);
  }
  return out;
}

void write_lines(Ctx& c, const std::vector<std::string>& lines) {
  std::string t;
  for (auto& l : lines) t += l + "\n";
  (void)c.host().make_dir(c.repo->git_dir() + "/info");
  auto w = c.host().write_file(sparse_file(c), ByteSpan((const uint8_t*)t.data(), t.size()));
  if (!w.ok()) die(c, "could not write " + sparse_file(c) + ": " + w.error().message);
}

std::string clean_dir(std::string d) {
  d = slashes(d);
  while (!d.empty() && d.front() == '/') d.erase(d.begin());
  while (!d.empty() && d.back() == '/') d.pop_back();
  if (d.rfind("./", 0) == 0) d = d.substr(2);
  return d;
}

// The cone's directories as git writes them: the top level's files, each named directory recursively, and the files
// directly in every directory above one.
std::vector<std::string> cone_lines(std::set<std::string> dirs) {
  // a directory inside another named one adds nothing
  std::set<std::string> minimal;
  for (auto& d : dirs) {
    bool inside = false;
    for (auto& e : dirs) if (e != d && d.compare(0, e.size() + 1, e + "/") == 0) inside = true;
    if (!inside && !d.empty()) minimal.insert(d);
  }
  std::set<std::string> parents;
  for (auto& d : minimal)
    for (size_t s = d.find('/'); s != std::string::npos; s = d.find('/', s + 1)) parents.insert(d.substr(0, s));
  std::vector<std::string> out{"/*", "!/*/"};
  std::set<std::string> all = minimal;
  all.insert(parents.begin(), parents.end());
  for (auto& d : all) {
    out.push_back("/" + d + "/");
    if (parents.count(d) && !minimal.count(d)) out.push_back("!/" + d + "/*/");
  }
  return out;
}

std::set<std::string> cone_dirs(const std::vector<std::string>& lines) {
  std::set<std::string> dirs, parents;
  for (auto& l : lines) {
    if (l == "/*" || l == "!/*/") continue;
    if (l[0] == '!' && l.size() > 4) { parents.insert(l.substr(2, l.size() - 5)); continue; }
    if (l[0] == '/' && l.back() == '/') dirs.insert(l.substr(1, l.size() - 2));
  }
  for (auto& p : parents) dirs.erase(p);
  return dirs;
}

// Put the work tree in line with the patterns: what came in is written, what went out is removed (unless it holds
// changes, which stay, as git leaves them).
void reapply(Ctx& c, bool quiet) {
  c.repo->reload_sparse();
  auto& h = c.host();
  std::vector<GitIndexEntry> next;
  size_t in = 0, out = 0, kept = 0, total = 0;
  std::set<std::string> left_dirs;
  for (auto e : c.repo->index()) {
    ++total;
    bool want = c.repo->in_sparse(e.path) || e.mode == 0160000;
    if (want && e.skip_worktree) {
      e.skip_worktree = false;
      auto w = c.repo->write_worktree_file(e.path, e.oid, e.mode);
      if (!w.ok()) die(c, "could not write " + e.path + ": " + w.error().message);
      GitIndexEntry ne = *w;
      ne.assume_valid = e.assume_valid;
      e = ne;
      ++in;
    } else if (!want && !e.skip_worktree) {
      std::string full = join(c.top, e.path);
      if (h.exists(full) && !c.repo->worktree_matches(e)) {
        c.err << "warning: " << e.path << " holds changes: left on disk though outside the sparse checkout\n";
        ++kept;
        next.push_back(e);
        continue;
      }
      (void)h.remove(full);
      size_t s = e.path.rfind('/');
      if (s != std::string::npos) left_dirs.insert(e.path.substr(0, s));
      e.skip_worktree = true;
      ++out;
    }
    if (!e.skip_worktree) ++kept;
    next.push_back(e);
  }
  // directories emptied by the removals go too, deepest first
  std::vector<std::string> dirs(left_dirs.begin(), left_dirs.end());
  std::sort(dirs.begin(), dirs.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
  for (auto& d : dirs) {
    std::string cur = d;
    while (!cur.empty()) {
      auto names = h.list_dir(join(c.top, cur));
      bool empty = names.ok();
      if (names.ok()) for (auto& n : *names) if (n != "." && n != "..") { empty = false; break; }
      if (!empty || !h.remove(join(c.top, cur)).ok()) break;
      size_t s = cur.rfind('/');
      cur = s == std::string::npos ? "" : cur.substr(0, s);
    }
  }
  c.repo->set_index(next);
  if (auto r = c.repo->write_index(); !r.ok()) die(c, r.error().message);
  if (!quiet && c.repo->sparse())
    c.out << "The work tree holds " << kept << " of " << total << " tracked files (" << in << " brought in, " << out << " taken out).\n";
}

void enable(Ctx& c, bool cone) {
  if (auto r = c.repo->set_config("core.sparseCheckout", "true"); !r.ok()) die(c, r.error().message);
  (void)c.repo->set_config("core.sparseCheckoutCone", cone ? "true" : "false");
}

}  // namespace

int cmd_sparse_checkout(Ctx& c) {
  std::string sub = c.args.size() > 1 ? c.args[1] : "";
  auto p = parse_options(c, c.args, 2, {{0, "cone"}, {0, "no-cone"}, {0, "stdin"}, {0, "skip-checks"}, {0, "sparse-index"},
                                         {0, "no-sparse-index"}, {0, "rules-file", Arg::Required}});
  c.open_repo();
  if (change::enabled(c) && change::ensure_wc(c)) change::snapshot(c);
  bool cone = p.has("no-cone") ? false : p.has("cone") ? true : c.config.get_bool("core.sparsecheckoutcone", true);
  std::vector<std::string> items = p.pos;
  if (p.has("stdin")) {
    std::istringstream ss(read_all(c.in));
    std::string l;
    while (std::getline(ss, l)) if (!l.empty()) items.push_back(l);
  }
  if (sub == "list") {
    if (!c.repo->sparse()) die(c, "this worktree is not sparse");
    auto lines = read_lines(c);
    if (c.config.get_bool("core.sparsecheckoutcone", true)) for (auto& d : cone_dirs(lines)) c.out << d << "\n";
    else for (auto& l : lines) c.out << l << "\n";
    return 0;
  }
  if (sub == "init") {
    enable(c, cone);
    if (read_lines(c).empty()) write_lines(c, {"/*", "!/*/"});
    reapply(c, false);
    return 0;
  }
  if (sub == "set" || sub == "add") {
    if (sub == "add" && !c.repo->sparse()) die(c, "no sparse-checkout to add to");
    enable(c, cone);
    if (cone) {
      std::set<std::string> dirs;
      if (sub == "add") dirs = cone_dirs(read_lines(c));
      for (auto& i : items) {
        std::string d = clean_dir(c.prefix + i);
        if (d.find_first_of("*?[") != std::string::npos) die(c, "'" + i + "' is a pattern; cone mode takes directories (--no-cone for patterns)");
        if (!d.empty()) dirs.insert(d);
      }
      write_lines(c, cone_lines(dirs));
    } else {
      std::vector<std::string> lines = sub == "add" ? read_lines(c) : std::vector<std::string>{};
      for (auto& i : items) lines.push_back(i);
      if (lines.empty()) lines = {"/*", "!/*/"};
      write_lines(c, lines);
    }
    reapply(c, false);
    oplog::record(c, "git sparse-checkout " + sub, "normal", "", {});
    return 0;
  }
  if (sub == "reapply") { reapply(c, false); return 0; }
  if (sub == "disable") {
    write_lines(c, {"/*"});
    (void)c.repo->set_config("core.sparseCheckoutCone", "false");
    reapply(c, true);
    (void)c.repo->set_config("core.sparseCheckout", "false");
    c.repo->reload_sparse();
    c.out << "The work tree holds every tracked file again.\n";
    return 0;
  }
  if (sub == "check-rules") {
    std::istringstream ss(read_all(c.in));
    std::string l;
    while (std::getline(ss, l)) if (c.repo->in_sparse(l)) c.out << l << "\n";
    return 0;
  }
  usage(c, "usage: git sparse-checkout (init | list | set | add | reapply | disable | check-rules) [<options>]");
}

}  // namespace aios_git
