// Revisions, history walks and snapshots of trees, the index and the work tree.
#include <algorithm>
#include <cctype>
#include <queue>
#include <regex>
#include <unordered_map>
#include <unordered_set>

#include "aios/hal/host.hpp"
#include "cli.hpp"

namespace aios_git {

using namespace aios;
using namespace aios::userland;

namespace {
bool all_hex(const std::string& s) {
  if (s.empty()) return false;
  for (char ch : s) if (!std::isxdigit((unsigned char)ch)) return false;
  return true;
}
std::string bytes_str(const ByteBuffer& b) { return std::string((const char*)b.data(), b.size()); }
std::string lower_hex(std::string s) {
  for (char& ch : s) if (ch >= 'A' && ch <= 'F') ch = (char)(ch - 'A' + 'a');
  return s;
}
}  // namespace

std::string type_of(Ctx& c, const std::string& oid) {
  auto o = c.repo->read_object(oid);
  if (!o.ok() && oid == empty_tree_oid()) return "tree";
  return o.ok() ? o->type : "";
}

GitCommit read_commit(Ctx& c, const std::string& oid) {
  auto r = c.repo->read_commit(oid);
  if (!r.ok()) die(c, "could not read commit " + oid + ": " + r.error().message);
  GitCommit cm = *r;
  cm.message = cm.raw_message;   // the CLI works with the message exactly as stored
  return cm;
}

std::string current_branch(Ctx& c) { return c.repo->branch(); }

std::vector<std::pair<std::string, std::string>> all_refs(Ctx& c) {
  std::vector<std::pair<std::string, std::string>> out;
  auto r = c.repo->refs();
  if (r.ok()) {
    out.reserve(r->size());
    for (auto& x : *r) out.emplace_back(std::move(x.name), std::move(x.oid));
  }
  // refs() hands them back sorted by name, and a name is never there twice
  if (!std::is_sorted(out.begin(), out.end())) std::sort(out.begin(), out.end());
  return out;
}

std::string dwim_ref(Ctx& c, const std::string& name) {
  if (name.empty()) return "";
  if (name == "HEAD" || name == "FETCH_HEAD" || name == "ORIG_HEAD" || name == "MERGE_HEAD" || name == "CHERRY_PICK_HEAD" ||
      name == "REVERT_HEAD" || name == "REBASE_HEAD") {
    return read_ref(c, name) ? name : "";
  }
  const std::vector<std::string> forms = {name, "refs/" + name, "refs/tags/" + name, "refs/heads/" + name,
                                          "refs/remotes/" + name, "refs/remotes/" + name + "/HEAD"};
  for (auto& f : forms) {
    if (f.rfind("refs/", 0) != 0) continue;
    if (f.size() > 5 && f.compare(f.size() - 5, 5, "/HEAD") == 0) {
      // refs/remotes/<r>/HEAD is symbolic
      auto t = c.host().read_file(c.repo->common_dir() + "/" + f);
      if (t.ok()) {
        std::string s = bytes_str(*t);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        if (s.rfind("ref: ", 0) == 0) return s.substr(5);
        if (s.size() == 40) return f;
      }
      continue;
    }
    if (c.repo->ref_oid(f).ok()) return f;
  }
  return "";
}

std::string upstream_of(Ctx& c, const std::string& branch) {
  auto remote = c.config.get("branch." + branch + ".remote");
  auto merge = c.config.get("branch." + branch + ".merge");
  if (!remote || !merge) return "";
  if (*remote == ".") return *merge;
  std::string m = *merge;
  if (m.rfind("refs/heads/", 0) == 0) m = m.substr(11);
  // map through the remote's fetch refspec (the usual +refs/heads/*:refs/remotes/<r>/*)
  for (auto& spec : c.config.get_all("remote." + *remote + ".fetch")) {
    std::string s = spec[0] == '+' ? spec.substr(1) : spec;
    size_t colon = s.find(':');
    if (colon == std::string::npos) continue;
    std::string src = s.substr(0, colon), dst = s.substr(colon + 1);
    size_t star = src.find('*');
    if (star == std::string::npos) { if (src == *merge) return dst; continue; }
    std::string pre = src.substr(0, star), post = src.substr(star + 1);
    if (merge->rfind(pre, 0) == 0 && merge->size() >= pre.size() + post.size() &&
        merge->compare(merge->size() - post.size(), post.size(), post) == 0) {
      std::string mid = merge->substr(pre.size(), merge->size() - pre.size() - post.size());
      size_t dstar = dst.find('*');
      return dstar == std::string::npos ? dst : dst.substr(0, dstar) + mid + dst.substr(dstar + 1);
    }
  }
  return "refs/remotes/" + *remote + "/" + m;
}

std::string peel_to(Ctx& c, const std::string& oid_in, const std::string& type) {
  std::string oid = oid_in;
  if (oid == empty_tree_oid() && !c.repo->read_object(oid).ok())
    return (type.empty() || type == "tree" || type == "object") ? oid : "";
  for (int i = 0; i < 32; ++i) {
    auto o = c.repo->read_object(oid);
    if (!o.ok()) return "";
    if (type.empty() || o->type == type || type == "object") return o->oid;
    if (o->type == "tag") {
      std::string body = bytes_str(o->payload);
      if (body.rfind("object ", 0) != 0) return "";
      oid = body.substr(7, 40);
      continue;
    }
    if (o->type == "commit" && type == "tree") {
      auto cm = c.repo->read_commit(o->oid);
      return cm.ok() ? cm->tree : "";
    }
    return "";
  }
  return "";
}

namespace {
std::optional<std::string> reflog_entry(Ctx& c, const std::string& ref, long n) {
  std::string path = (ref == "HEAD" ? c.repo->git_dir() : c.repo->common_dir()) + "/logs/" + ref;
  auto r = c.host().read_file(path);
  if (!r.ok()) return std::nullopt;
  std::vector<std::string> lines;
  std::string cur;
  for (char ch : bytes_str(*r)) { if (ch == '\n') { if (!cur.empty()) lines.push_back(cur); cur.clear(); } else cur += ch; }
  if (!cur.empty()) lines.push_back(cur);
  if (n < 0 || (size_t)n >= lines.size() + 1) return std::nullopt;
  if (n == 0) return lines.empty() ? std::nullopt : std::optional<std::string>(lines.back().substr(41, 40));
  const std::string& l = lines[lines.size() - (size_t)n];
  return l.substr(0, 40);   // the old value of the n-th newest update
}
}  // namespace

std::optional<std::string> resolve(Ctx& c, const std::string& spec_in, std::string* why) {
  std::string spec = spec_in;
  if (spec.empty()) return std::nullopt;
  // <rev>:<path>, :<path>, :<n>:<path>
  size_t colon = spec.find(':');
  if (spec == empty_tree_oid()) return spec;   // git knows the empty tree without storing it
  if (colon != std::string::npos && spec[0] != '@') {
    std::string left = spec.substr(0, colon), path = spec.substr(colon + 1);
    if (left.empty()) {
      int stage = 0;
      if (path.size() >= 2 && path[1] == ':' && path[0] >= '0' && path[0] <= '3') { stage = path[0] - '0'; path = path.substr(2); }
      if (!path.empty() && path[0] == '/') { if (why) *why = "searching commit messages is not supported"; return std::nullopt; }
      if (path.rfind("./", 0) == 0 || path.rfind("../", 0) == 0) path = c.to_repo_path(path);
      if (stage == 0) {
        for (auto& e : c.repo->index()) if (e.path == path) return e.oid;
      } else {
        for (auto& e : c.repo->unmerged()) if (e.path == path && e.stage == stage) return e.oid;
      }
      if (why) *why = "path '" + path + "' does not exist (neither on disk nor in the index)";
      return std::nullopt;
    }
    auto base = resolve(c, left, why);
    if (!base) return std::nullopt;
    std::string tree = peel_to(c, *base, "tree");
    if (tree.empty()) { if (why) *why = "'" + left + "' is not a tree-ish"; return std::nullopt; }
    if (path.rfind("./", 0) == 0 || path.rfind("../", 0) == 0) path = c.to_repo_path(path);
    while (!path.empty() && path.back() == '/') path.pop_back();
    if (path.empty()) return tree;
    std::string at = tree;
    size_t i = 0;
    while (i < path.size()) {
      size_t s = path.find('/', i);
      std::string comp = path.substr(i, s == std::string::npos ? std::string::npos : s - i);
      auto ents = c.repo->read_tree(at);
      if (!ents.ok()) return std::nullopt;
      bool found = false;
      for (auto& e : *ents) if (e.name == comp) { at = e.oid; found = true; break; }
      if (!found) { if (why) *why = "path '" + path + "' does not exist in '" + left + "'"; return std::nullopt; }
      if (s == std::string::npos) break;
      i = s + 1;
    }
    return at;
  }
  // base name, then ^, ~ and @{...} operators
  size_t op = spec.find_first_of("^~");
  size_t at_brace = spec.find("@{");
  if (at_brace != std::string::npos && (op == std::string::npos || at_brace < op)) op = at_brace;
  std::string base = spec.substr(0, op == std::string::npos ? spec.size() : op);
  std::string rest = op == std::string::npos ? "" : spec.substr(op);
  std::optional<std::string> cur;
  std::string cur_ref;
  if (base.empty() || base == "@") {
    cur_ref = "HEAD";
    if (rest.rfind("@{", 0) == 0) {
      // @{u}, @{1}: of the current branch (HEAD's own reflog for numbers)
      std::string b = c.repo->branch();
      if (rest.rfind("@{u", 0) == 0 || rest.rfind("@{push", 0) == 0) {
        if (b.empty()) { if (why) *why = "HEAD does not point to a branch"; return std::nullopt; }
        cur_ref = "refs/heads/" + b;
      }
    }
    cur = read_ref(c, "HEAD");
    if (!cur && rest.rfind("@{", 0) != 0) { if (why) *why = "ambiguous argument '" + spec + "': unknown revision or path not in the working tree."; return std::nullopt; }
  } else if (base.size() == 40 && all_hex(base) && c.repo->has_object(lower_hex(base))) {
    // A whole object id is that object (git's get_oid_basic): refs are not searched for it, which cost a cat-file
    // --batch five failed opens per line.
    cur = lower_hex(base);
  } else {
    std::string full = dwim_ref(c, base);
    if (!full.empty()) {
      cur_ref = full;
      cur = read_ref(c, full);
    } else if (all_hex(base) && base.size() >= 4 && base.size() <= 40) {
      auto o = c.repo->read_object(base);
      if (o.ok()) cur = o->oid;
      else if (o.status() != Status::NotFound && why) *why = "short object ID " + base + " is ambiguous";
    }
  }
  if (!cur && rest.rfind("@{", 0) != 0) {
    if (why && why->empty()) *why = "ambiguous argument '" + spec + "': unknown revision or path not in the working tree.";
    return std::nullopt;
  }
  size_t i = 0;
  while (i < rest.size()) {
    char ch = rest[i];
    if (ch == '@' && i + 1 < rest.size() && rest[i + 1] == '{') {
      size_t close = rest.find('}', i);
      if (close == std::string::npos) return std::nullopt;
      std::string inner = rest.substr(i + 2, close - i - 2);
      i = close + 1;
      if (inner == "u" || inner == "upstream" || inner == "push") {
        std::string b = cur_ref.rfind("refs/heads/", 0) == 0 ? cur_ref.substr(11) : "";
        std::string up = b.empty() ? "" : upstream_of(c, b);
        if (up.empty()) { if (why) *why = "no upstream configured for branch '" + b + "'"; return std::nullopt; }
        cur_ref = up;
        cur = read_ref(c, up);
        if (!cur) { if (why) *why = "upstream branch '" + up + "' not stored as a remote-tracking branch"; return std::nullopt; }
        continue;
      }
      if (!inner.empty() && (std::isdigit((unsigned char)inner[0]))) {
        auto e = reflog_entry(c, cur_ref.empty() ? "HEAD" : cur_ref, std::atol(inner.c_str()));
        if (!e) { if (why) *why = "log for '" + cur_ref + "' only has fewer entries"; return std::nullopt; }
        cur = *e;
        continue;
      }
      if (why) *why = "reflog selector @{" + inner + "} is not supported";
      return std::nullopt;
    }
    if (ch == '^') {
      ++i;
      if (i < rest.size() && rest[i] == '{') {
        size_t close = rest.find('}', i);
        if (close == std::string::npos) return std::nullopt;
        std::string t = rest.substr(i + 1, close - i - 1);
        i = close + 1;
        std::string want = t.empty() ? "" : t;
        if (t.empty()) {
          // ^{}: peel tags to whatever is underneath
          std::string o = *cur;
          for (int k = 0; k < 32; ++k) {
            if (type_of(c, o) != "tag") break;
            o = peel_to(c, o, "commit");
            if (o.empty()) return std::nullopt;
          }
          cur = o;
          continue;
        }
        if (t[0] == '/') { if (why) *why = "^{/text} is not supported"; return std::nullopt; }
        std::string p = peel_to(c, *cur, t);
        if (p.empty()) { if (why) *why = "'" + spec + "' does not peel to a " + t; return std::nullopt; }
        cur = p;
        continue;
      }
      long n = 1;
      if (i < rest.size() && std::isdigit((unsigned char)rest[i])) {
        n = 0;
        while (i < rest.size() && std::isdigit((unsigned char)rest[i])) n = n * 10 + (rest[i++] - '0');
      }
      std::string cm = peel_to(c, *cur, "commit");
      if (cm.empty()) return std::nullopt;
      if (n == 0) { cur = cm; continue; }
      auto commit = c.repo->read_commit(cm);
      if (!commit.ok() || (size_t)n > commit->parents.size()) {
        if (why) *why = "ambiguous argument '" + spec + "': unknown revision or path not in the working tree.";
        return std::nullopt;
      }
      cur = commit->parents[(size_t)n - 1];
      continue;
    }
    if (ch == '~') {
      ++i;
      long n = 1;
      if (i < rest.size() && std::isdigit((unsigned char)rest[i])) {
        n = 0;
        while (i < rest.size() && std::isdigit((unsigned char)rest[i])) n = n * 10 + (rest[i++] - '0');
      }
      std::string cm = peel_to(c, *cur, "commit");
      if (cm.empty()) return std::nullopt;
      for (long k = 0; k < n; ++k) {
        auto commit = c.repo->read_commit(cm);
        if (!commit.ok() || commit->parents.empty()) {
          if (why) *why = "ambiguous argument '" + spec + "': unknown revision or path not in the working tree.";
          return std::nullopt;
        }
        cm = commit->parents[0];
      }
      cur = cm;
      continue;
    }
    return std::nullopt;
  }
  return cur;
}

std::string must_resolve(Ctx& c, const std::string& spec) {
  std::string why;
  auto r = resolve(c, spec, &why);
  if (!r) {
    if (why.empty()) why = "ambiguous argument '" + spec + "': unknown revision or path not in the working tree.";
    if (why.rfind("ambiguous argument", 0) == 0)
      c.err << "fatal: " << why << "\nUse '--' to separate paths from revisions, like this:\n'git <command> [<revision>...] -- [<file>...]'\n";
    else
      c.err << "fatal: " << why << "\n";
    throw Exit{128};
  }
  return *r;
}

std::string commit_of(Ctx& c, const std::string& spec) {
  std::string o = must_resolve(c, spec);
  std::string cm = peel_to(c, o, "commit");
  if (cm.empty()) die(c, spec + ": not a commit");
  return cm;
}

bool is_ancestor(Ctx& c, const std::string& anc, const std::string& of) {
  auto r = git_is_ancestor(*c.repo, anc, of);
  return r.ok() && *r;
}

std::vector<std::string> merge_bases(Ctx& c, const std::string& a, const std::string& b) {
  auto r = git_merge_bases(*c.repo, a, b);
  if (!r.ok()) return {};
  return *r;
}

// ---------------------------------------------------------------------------
// revision arguments
// ---------------------------------------------------------------------------
std::vector<std::string> parse_revisions(Ctx& c, const std::vector<std::string>& args, WalkOptions& w, bool default_head,
                                         bool paths_after) {
  (void)paths_after;
  std::vector<std::string> rest;
  bool negate = false;
  bool any_rev = false;
  auto add_tip = [&](const std::string& oid, bool hide) {
    std::string cm = peel_to(c, oid, "commit");
    if (cm.empty()) return;
    (hide ? w.hide : w.tips).push_back(cm);
  };
  auto refs_with = [&](const std::string& prefix) {
    for (auto& [n, o] : all_refs(c)) if (n.rfind(prefix, 0) == 0) add_tip(o, negate);
    any_rev = true;
  };
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "--not") { negate = !negate; continue; }
    if (a == "--all") { refs_with("refs/"); if (auto h = read_ref(c, "HEAD")) add_tip(*h, negate); continue; }
    if (a == "--branches") { refs_with("refs/heads/"); continue; }
    if (a == "--tags") { refs_with("refs/tags/"); continue; }
    if (a == "--remotes") { refs_with("refs/remotes/"); continue; }
    if (!a.empty() && a[0] == '-' && a != "-") { rest.push_back(a); continue; }
    size_t dots = a.find("...");
    if (dots != std::string::npos) {
      std::string l = a.substr(0, dots), r = a.substr(dots + 3);
      auto lo = resolve(c, l.empty() ? "HEAD" : l), ro = resolve(c, r.empty() ? "HEAD" : r);
      if (lo && ro) {
        std::string lc = peel_to(c, *lo, "commit"), rc = peel_to(c, *ro, "commit");
        add_tip(lc, negate);
        add_tip(rc, negate);
        for (auto& b : merge_bases(c, lc, rc)) add_tip(b, !negate);
        w.symmetric = true;
        w.left_tips.push_back(lc);
        any_rev = true;
        continue;
      }
    }
    size_t dd = a.find("..");
    if (dd != std::string::npos && dots == std::string::npos) {
      std::string l = a.substr(0, dd), r = a.substr(dd + 2);
      auto lo = resolve(c, l.empty() ? "HEAD" : l), ro = resolve(c, r.empty() ? "HEAD" : r);
      if (lo && ro) {
        add_tip(*lo, !negate);
        add_tip(*ro, negate);
        any_rev = true;
        continue;
      }
    }
    if (a.size() > 1 && a[0] == '^') {
      if (auto o = resolve(c, a.substr(1))) { add_tip(*o, !negate); any_rev = true; continue; }
    }
    if (auto o = resolve(c, a)) {
      // a revision that names something that is also a file is ambiguous for git; revisions win here
      // only when the object is a commit-ish
      if (!peel_to(c, *o, "commit").empty()) { add_tip(*o, negate); any_rev = true; continue; }
    }
    rest.push_back(a);
  }
  if (default_head && !any_rev && w.tips.empty()) {
    if (auto h = read_ref(c, "HEAD")) w.tips.push_back(*h);
  }
  return rest;
}

// ---------------------------------------------------------------------------
// walking history
// ---------------------------------------------------------------------------
namespace {
// Whether two trees agree on every path the pathspec selects.
bool trees_same_under(Ctx& c, const std::string& a, const std::string& b, const Pathspec& ps, const std::string& prefix) {
  if (a == b) return true;
  std::vector<GitTreeEntry> ea, eb;
  if (!a.empty()) { auto r = c.repo->read_tree(a); if (r.ok()) ea = *r; }
  if (!b.empty()) { auto r = c.repo->read_tree(b); if (r.ok()) eb = *r; }
  std::map<std::string, const GitTreeEntry*> ma, mb;
  for (auto& e : ea) ma[e.name] = &e;
  for (auto& e : eb) mb[e.name] = &e;
  std::set<std::string> names;
  for (auto& [n, e] : ma) names.insert(n);
  for (auto& [n, e] : mb) names.insert(n);
  for (auto& n : names) {
    std::string path = prefix.empty() ? n : prefix + "/" + n;
    auto ia = ma.find(n), ib = mb.find(n);
    const GitTreeEntry* x = ia == ma.end() ? nullptr : ia->second;
    const GitTreeEntry* y = ib == mb.end() ? nullptr : ib->second;
    bool dir = (x && x->is_dir()) || (y && y->is_dir());
    if (dir) {
      if (!ps.may_match_below(path)) continue;
      if (x && y && x->oid == y->oid && x->mode == y->mode) continue;
      if (ps.match(path)) return false;   // a whole directory selected and it changed
      if (!trees_same_under(c, x && x->is_dir() ? x->oid : "", y && y->is_dir() ? y->oid : "", ps, path)) return false;
      // a file on one side and a directory on the other
      if ((x && !x->is_dir() && ps.match(path)) || (y && !y->is_dir() && ps.match(path))) return false;
      continue;
    }
    if (!ps.match(path)) continue;
    if (!x || !y || x->oid != y->oid || x->mode != y->mode) return false;
  }
  return true;
}
}  // namespace

namespace {
bool has_commit_graph(Ctx& c) {
  if (!c.config.get_bool("core.commitgraph", true)) return false;
  std::string dir = c.repo->common_dir() + "/objects/info/";
  return c.host().exists(dir + "commit-graph") || c.host().exists(dir + "commit-graphs/commit-graph-chain");
}
}  // namespace

std::vector<GitCommit> walk(Ctx& c, WalkOptions& w) {
  std::vector<GitCommit> out;
  walk_each(c, w, [&](const GitCommit& cm) { out.push_back(cm); });
  return out;
}

namespace {
// What a commit changes, reduced the way `git patch-id` does: file names, modes and the changed lines with every
// blank removed, so two commits that apply the same change compare equal. Merges have none.
std::string patch_id_of(Ctx& c, const GitCommit& cm) {
  if (cm.parents.size() > 1) return "";
  std::string from = cm.parents.empty() ? "" : read_commit(c, cm.parents[0]).tree;
  DiffOptions o;
  o.renames = false;
  auto pairs = diff_trees(c, from, cm.tree, Pathspec{}, o);
  std::string id;
  auto squeeze = [&](const std::string& line) {
    for (char ch : line) if (!std::isspace((unsigned char)ch)) id += ch;
    id += '\n';
  };
  for (auto& p : pairs) {
    squeeze("diff--git a/" + p.old_path + " b/" + p.new_path);
    if (!p.a.present) squeeze("newfilemode" + std::to_string(p.b.mode));
    if (!p.b.present) squeeze("deletedfilemode" + std::to_string(p.a.mode));
    std::string ta = pair_text(c, p, false), tb = pair_text(c, p, true);
    if (is_binary(ta) || is_binary(tb)) { squeeze("Binary:" + p.a.oid + ".." + p.b.oid); continue; }
    std::string h = git_unified_hunks(ta, tb, 3);
    size_t i = 0;
    while (i < h.size()) {
      size_t nl = h.find('\n', i);
      if (nl == std::string::npos) nl = h.size();
      std::string line = h.substr(i, nl - i);
      i = nl + 1;
      if (line.rfind("@@", 0) == 0 || line.rfind("\\ No newline", 0) == 0) continue;
      squeeze(line);
    }
  }
  return id;
}
}  // namespace

namespace {
// A full id is its own hash: sixteen of its digits, mixed (std::hash ran all 40 bytes through murmur, several times
// per commit of a walk).
struct HexIdHash {
  size_t operator()(const std::string& s) const noexcept {
    if (s.size() < 16) return std::hash<std::string>{}(s);
    uint64_t a, b;
    std::memcpy(&a, s.data(), 8);
    std::memcpy(&b, s.data() + 8, 8);
    uint64_t h = a ^ (b * 0x9e3779b97f4a7c15ull);
    h ^= h >> 32;
    h *= 0xd6e8feb86659fd93ull;
    return (size_t)(h ^ (h >> 29));
  }
};
using IdSet = std::unordered_set<std::string, HexIdHash>;
}  // namespace

void walk_each(Ctx& c, WalkOptions& w, const std::function<void(const GitCommit&)>& emit) {
  // One entry per commit met: the commit, and what the walk knows of it (a set per flag was a hash of the id each).
  struct Entry {
    GitCommit cm;
    bool light = false;    // known from the header or a block file's commit columns only (id, tree, parents, time)
    bool queued = false;   // pushed onto the walk's queue
    bool shown = false;    // handed to emit
  };
  std::unordered_map<std::string, Entry, HexIdHash> cache;
  auto slot = [&](const std::string& oid) -> std::pair<const std::string, Entry>* {
    auto it = cache.find(oid);
    if (it != cache.end()) return &*it;
    Entry e;
    // The header alone when the walk never shows more than ids (rev-list, counts, ancestry); a walk that prints or
    // filters on the rest reads each commit once, whole (reading the header first inflated every commit twice).
    if ((w.ids_only || c.repo->has_blocks()) && c.repo->commit_meta(oid, e.cm.tree, e.cm.parents, e.cm.committer_time)) {
      e.cm.oid = oid;
      e.light = true;
      return &*cache.emplace(oid, std::move(e)).first;
    }
    auto r = c.repo->read_commit(oid);
    if (!r.ok()) return nullptr;   // the edge of a shallow history
    e.cm = std::move(*r);
    e.cm.message = e.cm.raw_message;
    return &*cache.emplace(oid, std::move(e)).first;
  };
  auto get = [&](const std::string& oid) -> const GitCommit* {
    auto* s = slot(oid);
    return s ? &s->second.cm : nullptr;
  };
  auto full = [&](const std::string& oid) -> const GitCommit* {
    auto* s = slot(oid);
    if (!s) return nullptr;
    if (s->second.light) {
      auto r = c.repo->read_commit(oid);
      if (!r.ok()) return nullptr;
      s->second.cm = std::move(*r);
      s->second.cm.message = s->second.cm.raw_message;
      s->second.light = false;
    }
    return &s->second.cm;
  };
  // Everything reachable from a hidden tip is hidden - as far as it matters. git's limit_list: the shown and the
  // hidden tips are walked together, newest first, a hidden commit's mark passed down to its parents (and on through
  // any already met), until nothing but hidden commits is left in the queue (and a few more after that, git's
  // "slop", for clocks that ran backwards). Every hidden commit was marked before: `git status` on a branch even with
  // its upstream walked all 13k commits of redis' history, twice.
  IdSet hidden;
  if (!w.hide.empty()) {
    struct HItem { int64_t date; uint64_t seq; std::string oid; };
    auto newer = [](const HItem& x, const HItem& y) { return x.date != y.date ? x.date < y.date : x.seq > y.seq; };
    std::priority_queue<HItem, std::vector<HItem>, decltype(newer)> hq(newer);
    IdSet met, inq;
    uint64_t hseq = 0;
    long live = 0;   // queued and not hidden
    auto queue = [&](const std::string& o) {
      if (!met.insert(o).second) return;
      const GitCommit* cm = get(o);
      if (!cm) return;
      hq.push(HItem{(int64_t)cm->committer_time, hseq++, o});
      inq.insert(o);
      if (!hidden.count(o)) ++live;
    };
    auto hide = [&](const std::string& start) {
      std::vector<std::string> todo{start};
      while (!todo.empty()) {
        std::string o = std::move(todo.back());
        todo.pop_back();
        if (!hidden.insert(o).second) continue;
        if (inq.count(o)) --live;
        if (!met.count(o)) continue;   // its parents are marked when it is reached
        if (const GitCommit* cm = get(o)) for (auto& p : cm->parents) if (!hidden.count(p)) todo.push_back(p);
      }
    };
    for (auto& h : w.hide) hide(h);
    for (auto& h : w.hide) queue(h);
    for (auto& t : w.tips) queue(t);
    int slop = 5;
    while (!hq.empty()) {
      HItem it = hq.top();
      hq.pop();
      inq.erase(it.oid);
      const bool h = hidden.count(it.oid) > 0;
      if (!h) --live;
      if (const GitCommit* cm = get(it.oid))
        for (auto& p : cm->parents) {
          if (h) hide(p);
          queue(p);
        }
      if (live <= 0) {
        if (--slop <= 0) break;
      } else {
        slop = 5;
      }
    }
  }
  if (w.hidden_out) { w.hidden_out->clear(); w.hidden_out->insert(hidden.begin(), hidden.end()); }
  std::vector<std::regex> author_res, grep_res;
  auto flags = std::regex::ECMAScript | (w.grep_icase ? std::regex::icase : std::regex::ECMAScript);
  for (auto& a : w.authors) author_res.emplace_back(a, flags);
  for (auto& g : w.greps) grep_res.emplace_back(g, flags);
  const bool limit_paths = !w.paths.empty();

  // History simplification: the parents a commit keeps once a parent that carries the same paths is chosen,
  // and whether it changed the paths at all.
  struct Simplified { std::vector<std::string> parents, listed; bool treesame = false; };
  std::unordered_map<std::string, Simplified, HexIdHash> simplified;
  auto simplify = [&](const std::string& oid) -> const Simplified& {
    auto it = simplified.find(oid);
    if (it != simplified.end()) return it->second;
    Simplified s;
    if (const GitCommit* cm = get(oid)) {
      s.parents = s.listed = cm->parents;
      if (w.first_parent && s.parents.size() > 1) s.parents.resize(1);
      if (limit_paths) {
        if (cm->parents.empty()) {
          s.treesame = trees_same_under(c, "", cm->tree, w.paths, "");
        } else {
          int relevant_parents = 0;
          bool relevant_change = false, irrelevant_change = false, cut = false;
          for (auto& p : s.parents) {
            bool relevant = !hidden.count(p);
            if (relevant) ++relevant_parents;
            auto pc = get(p);
            if (!pc) continue;
            if (trees_same_under(c, pc->tree, cm->tree, w.paths, "")) {
              if (!relevant) continue;
              s.parents = s.listed = {p};
              s.treesame = cut = true;
              break;
            }
            (relevant ? relevant_change : irrelevant_change) = true;
          }
          if (!cut && (relevant_parents ? !relevant_change : !irrelevant_change)) s.treesame = true;
        }
      }
    }
    return simplified.emplace(oid, std::move(s)).first->second;
  };
  auto passes = [&](const std::string& oid) -> bool {
    if (hidden.count(oid)) return false;
    const GitCommit* cm = get(oid);
    if (!cm) return false;
    if (limit_paths && simplify(oid).treesame) return false;
    int np = (int)cm->parents.size();
    if (np < w.min_parents) return false;
    if (w.max_parents >= 0 && np > w.max_parents) return false;
    if ((int64_t)cm->committer_time < w.since) return false;
    if ((int64_t)cm->committer_time > w.until) return false;
    if (author_res.empty() && grep_res.empty()) return true;
    cm = full(oid);
    if (!cm) return false;
    if (!author_res.empty()) {
      bool hit = false;
      for (auto& re : author_res) if (std::regex_search(cm->author, re)) hit = true;
      if (!hit) return false;
    }
    if (!grep_res.empty()) {
      int hits = 0;
      for (auto& re : grep_res) if (std::regex_search(cm->message, re)) ++hits;
      bool ok = w.all_match ? hits == (int)grep_res.size() : hits > 0;
      if (ok == w.invert_grep) return false;
    }
    return true;
  };
  w.interesting = [&](const std::string& oid) {
    auto* s = slot(oid);
    return s && !s->second.shown && passes(oid);
  };

  // The parents a shown commit has in the simplified history: a parent that changed nothing is replaced by the
  // nearest ancestor that did, and one that reaches back to nothing is dropped.
  auto rewritten_parents = [&](const std::string& oid) {
    std::vector<std::string> out;
    for (auto& start : simplify(oid).listed) {
      std::string q = start;
      bool dropped = false;
      while (limit_paths && !hidden.count(q)) {
        const Simplified& sq = simplify(q);
        if (!sq.treesame) break;
        if (sq.listed.empty()) { dropped = true; break; }
        std::string next;
        if (sq.listed.size() == 1 || w.first_parent) next = sq.listed[0];
        else {
          int rel = 0;
          for (auto& p : sq.listed) if (!hidden.count(p) && ++rel == 1) next = p;
          if (rel != 1) break;
        }
        q = next;
      }
      if (!dropped && std::find(out.begin(), out.end(), q) == out.end()) out.push_back(q);
    }
    return out;
  };

  if (w.symmetric) {
    std::vector<std::string> todo = w.left_tips;
    while (!todo.empty()) {
      std::string o = todo.back();
      todo.pop_back();
      if (hidden.count(o) || !w.left.insert(o).second) continue;
      if (auto cm = get(o)) for (auto& p : cm->parents) todo.push_back(p);
    }
  }

  struct Item { int64_t date; uint64_t seq; const std::string* oid; };   // the id is the cache's own key
  auto cmp = [](const Item& x, const Item& y) { return x.date != y.date ? x.date < y.date : x.seq > y.seq; };
  std::priority_queue<Item, std::vector<Item>, decltype(cmp)> q(cmp);
  uint64_t seq = 0;
  auto push = [&](const std::string& oid) {
    if (hidden.count(oid)) return;
    auto* s = slot(oid);
    if (!s || s->second.queued) return;
    s->second.queued = true;
    q.push(Item{(int64_t)s->second.cm.committer_time, seq++, &s->first});
  };
  for (auto& t : w.tips) push(t);

  // Without a sort, --reverse or --cherry-pick, a commit's place is known when it is popped, so it goes out
  // then: a reader that stops early (`git log | head`) stops the walk too. The others need every commit first.
  const bool by_graph = w.topo && !w.date_order && !w.author_date_order;
  const bool ordered = w.topo || w.date_order || w.author_date_order;
  const bool stream = !ordered && !w.reverse && !w.cherry_pick && !w.cherry_mark && !w.want_children;
  long long skipped = 0, emitted = 0;
  std::vector<GitCommit> out;
  bool done = w.max_count == 0;
  auto deliver = [&](const std::string& oid) {
    if (!passes(oid)) return;
    if (skipped < w.skip) { ++skipped; return; }
    const GitCommit* cm = (w.ids_only ? get(oid) : full(oid));
    if (!cm) return;
    // a copy only for parents rewritten by the simplification; otherwise the cached commit itself goes out
    GitCommit copy;
    if (w.rewrite_parents) {
      copy = *cm;
      copy.parents = rewritten_parents(oid);
      cm = &copy;
    }
    if (w.post_filter && !w.post_filter(*cm)) return;
    ++emitted;
    if (stream) { slot(oid)->second.shown = true; emit(*cm); }
    else out.push_back(*cm);
    if (w.max_count >= 0 && emitted >= w.max_count) done = true;
  };
  std::vector<std::string> order;
  const bool simplifies = limit_paths || w.first_parent;   // otherwise a commit's parents are its own
  while (!q.empty() && !done) {
    Item it = q.top();
    q.pop();
    const std::string& oid = *it.oid;
    const GitCommit* cm = get(oid);
    if (!cm) continue;
    if ((int64_t)cm->committer_time < w.since) continue;
    for (auto& p : simplifies ? simplify(oid).parents : cm->parents) push(p);
    if (stream) deliver(oid);
    else order.push_back(oid);
  }
  if (stream) return;

  if (w.cherry_pick || w.cherry_mark) {
    // A commit on one side of A...B whose change also sits on the other side is the same work: omitted, or marked.
    std::vector<std::string> left_side, right_side;
    for (auto& o : order) (w.left.count(o) ? left_side : right_side).push_back(o);
    if (!left_side.empty() && !right_side.empty()) {
      const bool left_small = left_side.size() <= right_side.size();
      auto& first = left_small ? left_side : right_side;
      auto& second = left_small ? right_side : left_side;
      std::map<std::string, std::vector<std::string>> by_id;
      for (auto& o : first) {
        std::string id = patch_id_of(c, *full(o));
        if (!id.empty()) by_id[id].push_back(o);
      }
      std::set<std::string> twins;
      for (auto& o : second) {
        std::string id = patch_id_of(c, *full(o));
        auto hit = id.empty() ? by_id.end() : by_id.find(id);
        if (hit == by_id.end()) continue;
        twins.insert(o);
        for (auto& f : hit->second) twins.insert(f);
      }
      if (w.cherry_mark) w.patchsame = twins;
      else order.erase(std::remove_if(order.begin(), order.end(), [&](const std::string& o) { return twins.count(o) > 0; }), order.end());
    }
  }
  // Without a commit-graph the sorted orders walk everything first and leave the commits newer than --until out of the
  // list, so they take no part in the order.
  if (ordered && w.until != INT64_MAX && !has_commit_graph(c))
    order.erase(std::remove_if(order.begin(), order.end(), [&](const std::string& o) { return (int64_t)get(o)->committer_time > w.until; }), order.end());
  if (ordered) {
    // sort_in_topological_order: no commit before all of its children. The commits with no child in the list go
    // first. --topo-order takes them from a stack, so a commit whose last child just went out comes next (the last
    // parent first) and a merged branch follows its merge; --date-order and --author-date-order take the newest.
    std::unordered_set<std::string> in(order.begin(), order.end());
    // With a commit-graph git walks incrementally and counts only first-parent edges under --first-parent; without
    // one it sorts the whole list, which counts every parent.
    const bool first_edges = w.first_parent && has_commit_graph(c);
    auto edges = [&](const std::string& o) -> const std::vector<std::string>& {
      return first_edges ? simplify(o).parents : simplify(o).listed;
    };
    std::unordered_map<std::string, int> kids;
    for (auto& o : order) for (auto& p : edges(o)) if (in.count(p)) kids[p]++;
    auto key = [&](const std::string& o) -> int64_t {
      if (!w.author_date_order) return (int64_t)get(o)->committer_time;
      return (int64_t)full(o)->author_time;
    };
    std::vector<std::string> sorted;
    if (by_graph) {
      std::vector<std::string> ready;
      for (auto it = order.rbegin(); it != order.rend(); ++it) if (!kids[*it]) ready.push_back(*it);
      while (!ready.empty()) {
        std::string o = ready.back();
        ready.pop_back();
        sorted.push_back(o);
        for (auto& p : edges(o)) if (in.count(p) && --kids[p] == 0) ready.push_back(p);
      }
    } else {
      struct Ready { int64_t date; uint64_t seq; std::string oid; };
      auto lower = [](const Ready& x, const Ready& y) { return x.date != y.date ? x.date < y.date : x.seq > y.seq; };
      std::priority_queue<Ready, std::vector<Ready>, decltype(lower)> ready(lower);
      uint64_t n = 0;
      for (auto& o : order) if (!kids[o]) ready.push(Ready{key(o), n++, o});
      while (!ready.empty()) {
        std::string o = ready.top().oid;
        ready.pop();
        sorted.push_back(o);
        for (auto& p : edges(o)) if (in.count(p) && --kids[p] == 0) ready.push(Ready{key(p), n++, p});
      }
    }
    order = std::move(sorted);
  }
  // a child is noted as its commit goes out, so the newest comes first in a parent's list
  if (w.want_children)
    for (auto& o : order) for (auto& p : simplify(o).parents) w.children[p].insert(w.children[p].begin(), o);
  for (auto& o : order) {
    if (done) break;
    deliver(o);
  }
  if (w.reverse) std::reverse(out.begin(), out.end());
  for (auto& cm : out) {
    if (auto* s = slot(cm.oid)) s->second.shown = true;
    emit(cm);
  }
}

// ---------------------------------------------------------------------------
// snapshots
// ---------------------------------------------------------------------------
Snapshot snapshot_tree(Ctx& c, const std::string& tree_oid) {
  Snapshot s;
  if (tree_oid.empty() || tree_oid == empty_tree_oid()) return s;
  std::vector<GitIndexEntry> ents;
  auto r = c.repo->flatten_tree(tree_oid, "", ents);
  if (!r.ok()) die(c, "could not read tree " + tree_oid + ": " + r.error().message);
  for (auto& e : ents) s[e.path] = FileState{e.oid, e.mode, true};
  return s;
}

// Two trees as snapshots of only what differs between them: a subtree with the same id on both sides is not read
// (git's diff-tree does the same), where flattening both whole trees read every tree object of each (150 for redis).
// Identical files are left out of both; a directory on one side and a file on the other is flattened on its side.
void snapshot_tree_pair(Ctx& c, const std::string& ta, const std::string& tb, Snapshot& sa, Snapshot& sb) {
  auto norm = [&](const std::string& t) { return t == empty_tree_oid() ? std::string() : t; };
  const std::string a = norm(ta), b = norm(tb);
  if (a == b) return;
  std::function<void(const std::string&, const std::string&, const std::string&)> walk =
      [&](const std::string& x, const std::string& y, const std::string& prefix) {
        std::vector<GitTreeEntry> ex, ey;
        if (!x.empty()) { auto r = c.repo->read_tree(x); if (!r.ok()) die(c, "could not read tree " + x + ": " + r.error().message); ex = std::move(*r); }
        if (!y.empty()) { auto r = c.repo->read_tree(y); if (!r.ok()) die(c, "could not read tree " + y + ": " + r.error().message); ey = std::move(*r); }
        std::map<std::string, const GitTreeEntry*> by;
        for (auto& e : ey) by[e.name] = &e;
        auto put = [&](const GitTreeEntry& e, Snapshot& into) {
          const std::string path = prefix + e.name;
          if (!e.is_dir()) { into[path] = FileState{e.oid, e.mode, true}; return; }
          std::vector<GitIndexEntry> ents;
          if (auto r = c.repo->flatten_tree(e.oid, path, ents); !r.ok()) die(c, "could not read tree " + e.oid + ": " + r.error().message);
          for (auto& f : ents) into[f.path] = FileState{f.oid, f.mode, true};
        };
        for (auto& e : ex) {
          auto it = by.find(e.name);
          const GitTreeEntry* other = it == by.end() ? nullptr : it->second;
          if (other) by.erase(it);
          if (other && other->oid == e.oid && other->mode == e.mode) continue;   // the same file, or the same subtree
          if (other && e.is_dir() && other->is_dir()) { walk(e.oid, other->oid, prefix + e.name + "/"); continue; }
          put(e, sa);
          if (other) put(*other, sb);
        }
        for (auto& [name, e] : by) put(*e, sb);
      };
  walk(a, b, "");
}

std::vector<DiffPair> diff_trees(Ctx& c, const std::string& from, const std::string& to, const Pathspec& ps, const DiffOptions& o) {
  // -C -C looks for copy sources among the unchanged files too: those need the whole old tree
  if (o.copies_harder) return diff_snapshots(c, snapshot_tree(c, from), snapshot_tree(c, to), ps, o);
  Snapshot a, b;
  snapshot_tree_pair(c, from, to, a, b);
  return diff_snapshots(c, a, b, ps, o);
}

Snapshot snapshot_index(Ctx& c) {
  Snapshot s;
  for (auto& e : c.repo->index()) if (!e.intent_to_add) s[e.path] = FileState{e.oid, e.mode, true};   // `add -N` names a path, stages nothing
  return s;
}

std::string read_worktree_file(Ctx& c, const std::string& repo_path, bool* ok) {
  std::string full = join(c.top, repo_path);
  auto& h = c.host();
  auto st = h.stat(full);
  if (st.ok() && st->is_link && c.repo->settings().symlinks) {
    auto t = h.read_symlink(full);
    if (ok) *ok = t.ok();
    return t.ok() ? *t : "";
  }
  auto r = h.read_file(full);
  if (ok) *ok = r.ok();
  return r.ok() ? bytes_str(*r) : "";
}

Snapshot snapshot_worktree(Ctx& c, bool with_untracked, std::map<std::string, std::string>* texts) {
  Snapshot s = snapshot_index(c);
  for (auto& u : c.repo->unmerged()) {
    // an unmerged path is whatever the file holds now
    bool ok = false;
    std::string t = read_worktree_file(c, u.path, &ok);
    if (!ok) continue;
    std::string oid = hash_blob(t);
    s[u.path] = FileState{oid, u.mode ? u.mode : 0100644, true};
    if (texts) (*texts)[u.path] = t;
  }
  auto changes = c.repo->worktree_changes();
  if (!changes.ok()) die(c, changes.error().message);
  auto& h = c.host();
  for (auto& ch : *changes) {
    if (ch.change == GitChange::Deleted) { s.erase(ch.path); continue; }
    bool ok = false;
    std::string t = read_worktree_file(c, ch.path, &ok);
    if (!ok) { s.erase(ch.path); continue; }
    uint32_t mode = s[ch.path].mode;
    auto st = h.stat(join(c.top, ch.path));
    if (st.ok() && st->is_link && c.repo->settings().symlinks) mode = 0120000;
    else if (mode == 0120000) mode = c.repo->settings().filemode && st.ok() && st->is_exec ? 0100755 : 0100644;
    else if (c.repo->settings().filemode && st.ok()) mode = st->is_exec ? 0100755 : 0100644;
    s[ch.path] = FileState{hash_blob(t), mode, true};
    if (texts) (*texts)[ch.path] = t;
  }
  if (with_untracked) {
    auto un = c.repo->untracked(false);
    if (un.ok()) {
      for (auto& p : *un) {
        bool ok = false;
        std::string t = read_worktree_file(c, p, &ok);
        if (!ok) continue;
        uint32_t mode = 0100644;
        auto st = h.stat(join(c.top, p));
        if (st.ok() && st->is_link && c.repo->settings().symlinks) mode = 0120000;
        else if (c.repo->settings().filemode && st.ok() && st->is_exec) mode = 0100755;
        s[p] = FileState{hash_blob(t), mode, true};
        if (texts) (*texts)[p] = t;
      }
    }
  }
  return s;
}

std::string blob_text(Ctx& c, const std::string& oid) {
  if (oid.empty() || oid == std::string(40, '0')) return "";
  auto o = c.repo->read_object(oid);
  if (!o.ok()) return "";
  return bytes_str(o->payload);
}

}  // namespace aios_git
