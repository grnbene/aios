// The change layer's machinery: change ids, commits with headers, conflicts recorded in commits, which commits are
// visible and mutable, transactions that rewrite and rebase, and the working copy as a commit. See change.hpp.
#include "change.hpp"

#include <algorithm>
#include <cstring>
#include <functional>
#include <queue>
#include <random>
#include <unordered_map>
#include <unordered_set>

#include "aios/crypto/sha1.hpp"
#include "aios/hal/host.hpp"
#include "oplog.hpp"

namespace aios_git::change {

using namespace aios;
using aios::userland::GitMergeConflict;

namespace {

std::string str_of(const ByteBuffer& b) { return std::string((const char*)b.data(), b.size()); }
ByteSpan span(const std::string& s) { return ByteSpan((const uint8_t*)s.data(), s.size()); }

const char* kAlphabet = "zyxwvutsrqponmlk";   // jj's: hex digit d is written as kAlphabet[d]

std::string hex_to_cid(const std::string& hex) {
  std::string out;
  for (char ch : hex) {
    int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : 0;
    out += kAlphabet[d];
  }
  return out;
}

bool is_cid_text(const std::string& s) {
  if (s.empty()) return false;
  for (char ch : s) if (ch < 'k' || ch > 'z') return false;
  return true;
}

// Per-invocation caches: a command reads the same commits many times over.
struct Cache {
  std::unordered_map<std::string, GitCommit> commits;
  bool painted = false;
  std::vector<std::string> visible;                 // newest first
  std::unordered_set<std::string> immutable_seen;   // visited by the paint and immutable
  std::unordered_set<std::string> mutable_seen;
  std::vector<std::string> imm_heads;
  bool imm_heads_loaded = false;
};
std::unordered_map<const Ctx*, Cache> g_cache;
Cache& cache(Ctx& c) { return g_cache[&c]; }
void invalidate(Ctx& c) {
  auto& k = cache(c);
  k.painted = false;
  k.visible.clear();
  k.immutable_seen.clear();
  k.mutable_seen.clear();
}

const GitCommit& commit_at(Ctx& c, const std::string& oid) {
  auto& k = cache(c);
  auto it = k.commits.find(oid);
  if (it != k.commits.end()) return it->second;
  return k.commits.emplace(oid, read_commit(c, oid)).first->second;
}

std::string tree_of_commit(Ctx& c, const std::string& oid) { return oid.empty() ? empty_tree_oid() : commit_at(c, oid).tree; }

void ensure_empty_tree(Ctx& c) {
  if (!c.repo->has_object(empty_tree_oid())) (void)c.repo->write_object("tree", ByteSpan{});
}

// One path's entry in a tree, found by walking the components (no flattening of the whole tree).
std::optional<Side> entry_at(Ctx& c, const std::string& tree, const std::string& path) {
  if (tree.empty() || tree == empty_tree_oid()) return std::nullopt;
  std::string cur = tree;
  size_t i = 0;
  while (true) {
    size_t slash = path.find('/', i);
    std::string name = path.substr(i, slash == std::string::npos ? std::string::npos : slash - i);
    auto ents = c.repo->read_tree(cur);
    if (!ents.ok()) return std::nullopt;
    const GitTreeEntry* hit = nullptr;
    for (auto& e : *ents) if (e.name == name) { hit = &e; break; }
    if (!hit) return std::nullopt;
    if (slash == std::string::npos) {
      if (hit->is_dir()) return std::nullopt;
      return Side{hit->oid, hit->mode, true};
    }
    if (!hit->is_dir()) return std::nullopt;
    cur = hit->oid;
    i = slash + 1;
  }
}

std::string enc_path(const std::string& p) {
  std::string out;
  for (char ch : p) {
    if (ch == '%') out += "%25";
    else if (ch == '/') out += "%2F";
    else out += ch;
  }
  return out;
}
std::string dec_path(const std::string& p) {
  std::string out;
  for (size_t i = 0; i < p.size(); ++i) {
    if (p[i] == '%' && i + 2 < p.size()) {
      std::string h = p.substr(i + 1, 2);
      if (h == "25") { out += '%'; i += 2; continue; }
      if (h == "2F") { out += '/'; i += 2; continue; }
    }
    out += p[i];
  }
  return out;
}

std::string ident_line(const std::string& who, uint64_t when, const std::string& tz) {
  return who + " " + std::to_string(when) + " " + tz;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// identity
// ---------------------------------------------------------------------------------------------------------------
bool enabled(Ctx& c) { return c.repo && c.config.get_bool("change.enabled", false); }

std::string new_change_id(Ctx& c) {
  static uint64_t counter = 0;
  std::random_device rd;
  std::string seed = std::to_string(c.host().wall_ns()) + "|" + std::to_string(++counter) + "|" + std::to_string(rd()) + "|" +
                     std::to_string(rd()) + "|" + c.cwd;
  auto sum = crypto::sha1(span(seed));
  static const char* hx = "0123456789abcdef";
  std::string hex;
  for (size_t i = 0; i < 16; ++i) { hex += hx[sum[i] >> 4]; hex += hx[sum[i] & 15]; }
  return hex_to_cid(hex);
}

std::string derived_change_id(const std::string& oid) {
  std::string r(oid.rbegin(), oid.rend());
  return hex_to_cid(r.substr(0, 32));
}

std::string change_id_of(const GitCommit& cm) {
  std::string h = cm.header("change-id");
  if (h.size() == 32 && is_cid_text(h)) return h;
  return derived_change_id(cm.oid);
}

std::string short_id(const std::string& cid) { return cid.substr(0, 12); }

// ---------------------------------------------------------------------------------------------------------------
// commits
// ---------------------------------------------------------------------------------------------------------------
CommitSpec spec_of(Ctx& c, const GitCommit& cm) {
  (void)c;
  CommitSpec s;
  s.tree = cm.tree;
  s.parents = cm.parents;
  s.message = cm.raw_message;
  s.author = ident_line(cm.author, cm.author_time, cm.author_tz);
  s.change_id = change_id_of(cm);
  s.conflict_sides = cm.header("conflict-sides");
  for (auto& [k, v] : cm.extra_headers)
    if (k != "change-id" && k != "conflict-sides" && k != "gpgsig" && k != "gpgsig-sha256") s.other_headers.push_back({k, v});
  return s;
}

std::string write_commit(Ctx& c, CommitSpec s) {
  if (s.tree.empty() || s.tree == empty_tree_oid()) { ensure_empty_tree(c); if (s.tree.empty()) s.tree = empty_tree_oid(); }
  if (s.change_id.empty()) s.change_id = new_change_id(c);
  std::string body = "tree " + s.tree + "\n";
  for (auto& p : s.parents) body += "parent " + p + "\n";
  body += "author " + (s.author.empty() ? c.ident(false) : s.author) + "\n";
  body += "committer " + (s.committer.empty() ? c.ident(true) : s.committer) + "\n";
  auto header = [&](const std::string& k, const std::string& v) {
    body += k + " ";
    for (char ch : v) { body += ch; if (ch == '\n') body += ' '; }
    body += "\n";
  };
  for (auto& [k, v] : s.other_headers) header(k, v);
  header("change-id", s.change_id);
  if (!s.conflict_sides.empty()) header("conflict-sides", s.conflict_sides);
  body += "\n";
  body += s.message;
  body = sign_commit_body(c, body);
  auto w = c.repo->write_object("commit", span(body));
  if (!w.ok()) die(c, "failed to write commit object: " + w.error().message);
  return *w;
}

// ---------------------------------------------------------------------------------------------------------------
// conflicts
// ---------------------------------------------------------------------------------------------------------------
bool has_markers(const std::string& t) {
  bool open = false;
  size_t i = 0;
  while (i < t.size()) {
    size_t nl = t.find('\n', i);
    size_t end = nl == std::string::npos ? t.size() : nl;
    if (end - i >= 7) {
      if (t.compare(i, 7, "<<<<<<<") == 0) open = true;
      else if (open && t.compare(i, 7, ">>>>>>>") == 0) return true;
    }
    if (nl == std::string::npos) break;
    i = nl + 1;
  }
  return false;
}

Conflicts read_conflicts(Ctx& c, const GitCommit& cm) {
  Conflicts out;
  std::string sides = cm.header("conflict-sides");
  if (sides.size() != 40) return out;
  auto top = c.repo->read_tree(sides);
  if (!top.ok()) return out;
  for (auto& d : *top) {
    if (!d.is_dir()) continue;
    PathConflict pc;
    auto ents = c.repo->read_tree(d.oid);
    if (!ents.ok()) continue;
    for (auto& e : *ents) {
      Side s{e.oid, e.mode, true};
      if (e.name == "base") pc.base = s;
      else if (e.name == "ours") pc.ours = s;
      else if (e.name == "theirs") pc.theirs = s;
      else if (e.name == "result") pc.materialized = e.oid;
      else if (e.name == "info") {
        std::string t = blob_text(c, e.oid);
        std::vector<std::string> lines;
        size_t i = 0;
        while (i <= t.size()) {
          size_t nl = t.find('\n', i);
          lines.push_back(t.substr(i, nl == std::string::npos ? std::string::npos : nl - i));
          if (nl == std::string::npos) break;
          i = nl + 1;
        }
        if (lines.size() > 0) pc.kind = lines[0];
        if (lines.size() > 1) pc.ours_label = lines[1];
        if (lines.size() > 2) pc.theirs_label = lines[2];
      }
    }
    out[dec_path(d.name)] = pc;
  }
  return out;
}

std::string write_conflicts(Ctx& c, const Conflicts& cs) {
  if (cs.empty()) return "";
  std::vector<GitIndexEntry> ents;
  auto blob = [&](const std::string& text) {
    auto w = c.repo->write_object("blob", span(text));
    if (!w.ok()) die(c, w.error().message);
    return *w;
  };
  for (auto& [p, pc] : cs) {
    std::string d = enc_path(p);
    auto add = [&](const char* name, const Side& s) {
      if (!s.present || s.mode == 0160000) return;
      GitIndexEntry e;
      e.path = d + "/" + name;
      e.oid = s.oid;
      e.mode = s.mode == 0120000 || s.mode == 0100755 ? s.mode : 0100644;
      ents.push_back(e);
    };
    add("base", pc.base);
    add("ours", pc.ours);
    add("theirs", pc.theirs);
    if (!pc.materialized.empty()) add("result", Side{pc.materialized, 0100644, true});
    add("info", Side{blob(pc.kind + "\n" + pc.ours_label + "\n" + pc.theirs_label + "\n"), 0100644, true});
  }
  std::sort(ents.begin(), ents.end(), [](const GitIndexEntry& a, const GitIndexEntry& b) { return a.path < b.path; });
  return write_tree_of(c, ents);
}

Conflicts still_conflicted(Ctx& c, const std::string& tree, const Conflicts& candidates) {
  Conflicts out;
  for (auto& [p, pc] : candidates) {
    auto e = entry_at(c, tree, p);
    if (!e) {
      if (pc.materialized.empty()) out[p] = pc;   // recorded as absent and still absent
      continue;
    }
    if (e->oid == pc.materialized || has_markers(blob_text(c, e->oid))) {
      PathConflict k = pc;
      if (k.materialized.empty()) k.materialized = e->oid;
      out[p] = k;
    }
  }
  return out;
}

MergedTree merge_trees(Ctx& c, const std::string& base_tree, const std::string& ours_tree, const std::string& theirs_tree,
                       const std::string& ours_label, const std::string& theirs_label) {
  ensure_empty_tree(c);
  MergedTree out;
  auto norm = [](const std::string& t) { return t.empty() ? empty_tree_oid() : t; };
  const std::string b = norm(base_tree), o = norm(ours_tree), t = norm(theirs_tree);
  if (o == t || b == t) { out.tree = o; return out; }
  if (b == o) { out.tree = t; return out; }
  aios::userland::GitTreeMergeOptions opt;
  opt.ours_label = ours_label;
  opt.theirs_label = theirs_label;
  opt.structural = c.config.get_bool("merge.structural", true);
  auto r = aios::userland::git_merge_trees(*c.repo, b == empty_tree_oid() ? "" : b, o, t, opt);
  if (!r.ok()) die(c, "merge failed: " + r.error().message);
  if (r->clean) { out.tree = r->tree; return out; }
  std::map<std::string, GitIndexEntry> entries;
  for (auto& e : r->entries) entries[e.path] = e;
  for (auto& pc : r->conflicts) {
    if (pc.kind == "file/directory") continue;
    PathConflict k;
    k.kind = pc.kind;
    k.ours_label = ours_label;
    k.theirs_label = theirs_label;
    if (auto s = entry_at(c, b, pc.path)) k.base = *s;
    if (auto s = entry_at(c, o, pc.path)) k.ours = *s;
    if (auto s = entry_at(c, t, pc.path)) k.theirs = *s;
    if ((pc.kind == "content" || pc.kind == "add/add") && !pc.file.binary && !pc.file.merged.empty()) {
      auto w = c.repo->write_object("blob", span(pc.file.merged));
      if (!w.ok()) die(c, w.error().message);
      uint32_t mode = k.ours.present ? k.ours.mode : k.theirs.present ? k.theirs.mode : 0100644;
      entries[pc.path] = GitIndexEntry{pc.path, *w, mode};
    } else if (pc.kind == "modify/delete" && !entries.count(pc.path)) {
      const Side& kept = k.ours.present ? k.ours : k.theirs;
      if (kept.present) entries[pc.path] = GitIndexEntry{pc.path, kept.oid, kept.mode};
    }
    auto it = entries.find(pc.path);
    k.materialized = it == entries.end() ? "" : it->second.oid;
    out.conflicts[pc.path] = k;
  }
  std::vector<GitIndexEntry> flat;
  for (auto& [p, e] : entries) flat.push_back(e);
  out.tree = write_tree_of(c, flat);
  return out;
}

MergedTree parents_tree(Ctx& c, const std::vector<std::string>& parents) {
  MergedTree out;
  if (parents.empty()) { ensure_empty_tree(c); out.tree = empty_tree_oid(); return out; }
  const GitCommit& first = commit_at(c, parents[0]);
  out.tree = first.tree;
  out.conflicts = read_conflicts(c, first);
  std::string acc_commit = parents[0];
  for (size_t i = 1; i < parents.size(); ++i) {
    const GitCommit& pi = commit_at(c, parents[i]);
    auto bases = merge_bases(c, acc_commit, parents[i]);
    std::string base_tree = bases.empty() ? empty_tree_oid() : tree_of_commit(c, bases[0]);
    auto m = merge_trees(c, base_tree, out.tree, pi.tree, short_id(change_id_of(first)), short_id(change_id_of(pi)));
    Conflicts carried = out.conflicts;
    for (auto& [p, k] : read_conflicts(c, pi)) carried[p] = k;
    for (auto& [p, k] : m.conflicts) carried[p] = k;
    out.tree = m.tree;
    out.conflicts = still_conflicted(c, m.tree, carried);
  }
  return out;
}

// ---------------------------------------------------------------------------------------------------------------
// the repository's changes
// ---------------------------------------------------------------------------------------------------------------
std::string workspace(Ctx& c) {
  std::string w = c.getenv("GIT_CHANGE_WORKSPACE");
  if (!w.empty()) return w;
  // A linked work tree (git worktree add) is a workspace of its own, named as git names it (.git/worktrees/<name>):
  // its files are not the main tree's, and one shared @ had each folder's snapshot replace the other's files in it.
  if (c.repo && c.repo->git_dir() != c.repo->common_dir()) {
    const std::string& g = c.repo->git_dir();
    const size_t slash = g.find_last_of('/');
    return slash == std::string::npos ? g : g.substr(slash + 1);
  }
  return "default";
}
std::string wc_ref(Ctx& c) { return "refs/changes/wc/" + workspace(c); }
std::string wc_commit(Ctx& c) {
  auto r = read_ref(c, wc_ref(c));
  return r ? *r : "";
}

std::map<std::string, std::string> head_refs(Ctx& c) {
  std::map<std::string, std::string> out;
  for (auto& [name, oid] : all_refs(c))
    if (name.rfind("refs/changes/heads/", 0) == 0) out[name.substr(19)] = oid;
  return out;
}

std::vector<std::string> immutable_heads(Ctx& c) {
  auto& k = cache(c);
  if (k.imm_heads_loaded) return k.imm_heads;
  k.imm_heads_loaded = true;
  std::set<std::string> out;
  std::vector<std::string> extra = c.config.get_all("change.immutable");
  // trunk on every remote (what its HEAD names, else main/master/trunk) and tags: a remote branch of one's own (a
  // pushed stack) stays rewritable, as jj's immutable_heads() has it
  std::set<std::string> remote_trunks;
  for (auto& [name, oid] : all_refs(c)) {
    if (name.rfind("refs/remotes/", 0) != 0) continue;
    std::string rest = name.substr(13);
    size_t sl = rest.find('/');
    if (sl == std::string::npos) continue;
    std::string remote = rest.substr(0, sl);
    if (auto t = c.host().read_file(c.repo->common_dir() + "/refs/remotes/" + remote + "/HEAD"); t.ok()) {
      std::string sym((const char*)t->data(), t->size());
      while (!sym.empty() && (sym.back() == '\n' || sym.back() == ' ')) sym.pop_back();
      if (sym.rfind("ref: ", 0) == 0) remote_trunks.insert(sym.substr(5));
    }
    std::string branch = rest.substr(sl + 1);
    if (branch == "main" || branch == "master" || branch == "trunk") remote_trunks.insert(name);
  }
  for (auto& [name, oid] : all_refs(c)) {
    bool imm = remote_trunks.count(name) || name.rfind("refs/tags/", 0) == 0;
    for (auto& e : extra) if (name == e || name == "refs/heads/" + e || name == "refs/tags/" + e || name == "refs/remotes/" + e) imm = true;
    if (!imm) continue;
    std::string cm = peel_to(c, oid, "commit");
    if (!cm.empty()) out.insert(cm);
  }
  k.imm_heads.assign(out.begin(), out.end());
  return k.imm_heads;
}

namespace {
std::vector<std::string> roots_of(Ctx& c) {
  std::set<std::string> roots;
  for (auto& [name, oid] : all_refs(c)) {
    if (name.rfind("refs/changes/heads/", 0) == 0 || name.rfind("refs/changes/wc/", 0) == 0 || name.rfind("refs/heads/", 0) == 0) {
      std::string cm = peel_to(c, oid, "commit");
      if (!cm.empty()) roots.insert(cm);
    }
  }
  if (auto h = read_ref(c, "HEAD")) roots.insert(*h);
  return {roots.begin(), roots.end()};
}

// Paint down from the visible roots and the immutable heads together, newest first (git's paint_down_to_common): a
// commit reached from an immutable head is immutable; the walk stops once only immutable commits are left to visit.
void paint(Ctx& c, size_t limit) {
  auto& k = cache(c);
  if (k.painted) return;
  k.painted = true;
  enum : uint8_t { VIS = 1, IMM = 2 };
  std::unordered_map<std::string, uint8_t> flags;
  using Item = std::pair<uint64_t, std::string>;
  std::priority_queue<Item> q;
  size_t pending_vis = 0;
  auto push = [&](const std::string& oid, uint8_t f) {
    uint8_t& have = flags[oid];
    bool queued = have != 0;
    uint8_t before = have;
    have |= f;
    if (have == before) return;
    bool was_vis_only = (before & VIS) && !(before & IMM);
    bool is_vis_only = (have & VIS) && !(have & IMM);
    if (!queued) {
      q.push({commit_at(c, oid).committer_time, oid});
      if (is_vis_only) ++pending_vis;
    } else if (was_vis_only && !is_vis_only) {
      --pending_vis;
    }
  };
  for (auto& r : roots_of(c)) push(r, VIS);
  for (auto& h : immutable_heads(c)) push(h, IMM);
  std::unordered_set<std::string> done;
  while (!q.empty() && pending_vis > 0) {
    auto [t, oid] = q.top();
    q.pop();
    if (!done.insert(oid).second) continue;
    uint8_t f = flags[oid];
    if ((f & VIS) && !(f & IMM)) --pending_vis;
    if (f & IMM) {
      k.immutable_seen.insert(oid);
      if (f & VIS) k.visible.push_back(oid);   // the boundary: shown, never rewritten
    } else {
      k.mutable_seen.insert(oid);
      k.visible.push_back(oid);
      if (limit && k.visible.size() >= limit) break;
    }
    for (auto& p : commit_at(c, oid).parents) push(p, (f & IMM) ? IMM : VIS);
  }
}
}  // namespace

bool is_immutable(Ctx& c, const std::string& oid) {
  auto& k = cache(c);
  paint(c, 0);
  if (k.immutable_seen.count(oid)) return true;
  if (k.mutable_seen.count(oid)) return false;
  for (auto& h : immutable_heads(c)) if (is_ancestor(c, oid, h)) return true;
  return false;
}

std::vector<std::string> visible_commits(Ctx& c, size_t limit) {
  auto& k = cache(c);
  if (limit) {
    invalidate(c);
    paint(c, limit);
    auto out = k.visible;
    invalidate(c);
    return out;
  }
  paint(c, 0);
  return k.visible;
}

std::vector<std::string> commits_of_change(Ctx& c, const std::string& cid) {
  std::vector<std::string> out;
  auto heads = head_refs(c);
  if (auto it = heads.find(cid); it != heads.end()) {
    // the head ref is authoritative for a change that has one
    out.push_back(it->second);
    return out;
  }
  for (auto& oid : visible_commits(c)) if (change_id_of(commit_at(c, oid)) == cid) out.push_back(oid);
  return out;
}

std::vector<std::string> resolve_revs(Ctx& c, const std::string& spec_in) {
  std::string spec = spec_in;
  size_t dashes = 0;
  while (!spec.empty() && spec.back() == '-' && spec.size() > 1) { spec.pop_back(); ++dashes; }
  std::vector<std::string> cur;
  if (spec == "@") {
    std::string wc = wc_commit(c);
    if (wc.empty()) die(c, "there is no working-copy change here (git change init turns the change layer on)");
    cur.push_back(wc);
  } else if (is_cid_text(spec) && spec.size() >= 1) {
    std::set<std::string> hits;
    std::string exact;
    for (auto& [cid, oid] : head_refs(c)) if (cid.rfind(spec, 0) == 0) hits.insert(cid);
    for (auto& oid : visible_commits(c)) {
      std::string cid = change_id_of(commit_at(c, oid));
      if (cid.rfind(spec, 0) == 0) hits.insert(cid);
    }
    if (hits.size() > 1) die(c, "change id prefix '" + spec + "' is ambiguous");
    if (hits.size() == 1) {
      auto commits = commits_of_change(c, *hits.begin());
      if (commits.size() > 1) die(c, "change " + short_id(*hits.begin()) + " is divergent: name one of its commits by commit id");
      if (!commits.empty()) cur.push_back(commits[0]);
    }
    if (cur.empty()) {
      auto r = resolve(c, spec);
      if (!r) die(c, "no change or revision '" + spec_in + "'");
      cur.push_back(peel_to(c, *r, "commit"));
    }
  } else {
    std::string why;
    auto r = resolve(c, spec, &why);
    if (!r) die(c, "no change or revision '" + spec_in + "'");
    std::string cm = peel_to(c, *r, "commit");
    if (cm.empty()) die(c, "'" + spec_in + "' is not a commit");
    cur.push_back(cm);
  }
  for (size_t i = 0; i < dashes; ++i) {
    std::vector<std::string> next;
    for (auto& o : cur) for (auto& p : commit_at(c, o).parents) if (std::find(next.begin(), next.end(), p) == next.end()) next.push_back(p);
    if (next.empty()) die(c, "'" + spec_in + "' goes past the root commit");
    cur = next;
  }
  return cur;
}

std::string resolve_rev(Ctx& c, const std::string& spec) {
  auto v = resolve_revs(c, spec);
  if (v.size() != 1) die(c, "'" + spec + "' names " + std::to_string(v.size()) + " commits; name one");
  return v[0];
}

// ---------------------------------------------------------------------------------------------------------------
// rewriting
// ---------------------------------------------------------------------------------------------------------------
std::string rebase_commit(Ctx& c, const GitCommit& cm, const std::vector<std::string>& new_parents) {
  if (new_parents == cm.parents) return cm.oid;
  MergedTree old_base = parents_tree(c, cm.parents);
  MergedTree new_base = parents_tree(c, new_parents);
  MergedTree merged;
  if (old_base.tree == new_base.tree) merged.tree = cm.tree;
  else merged = merge_trees(c, old_base.tree, new_base.tree, cm.tree, "rebase destination", short_id(change_id_of(cm)) + " " + subject_of(cm.raw_message));
  Conflicts carried = read_conflicts(c, cm);
  // A path still conflicted as recorded (its file untouched since) is the formal sum ours + theirs - base. When the
  // old parents held one of those sides, moving to new parents replaces that side: the path is merged again from the
  // other two and the new parents' version, so a conflict can also go away (rebased back onto what it came from).
  {
    Conflicts own = carried;
    std::map<std::string, GitIndexEntry> ents;
    bool touched = false;
    for (auto& [p, k] : own) {
      auto here = entry_at(c, cm.tree, p);
      if (!here || here->oid != k.materialized) continue;
      auto pold = entry_at(c, old_base.tree, p), pnew = entry_at(c, new_base.tree, p);
      auto same = [](const std::optional<Side>& x, const Side& y) { return x ? (y.present && x->oid == y.oid) : !y.present; };
      const Side* other = nullptr;
      if (same(pold, k.ours)) other = &k.theirs;
      else if (same(pold, k.theirs)) other = &k.ours;
      if (!other) continue;
      Side nw = pnew ? *pnew : Side{};
      std::optional<Side> result;
      std::string merged_text;
      bool clean = false;
      if (same(pnew, k.base)) { result = *other; clean = true; }
      else if (other->present == k.base.present && (!other->present || other->oid == k.base.oid)) { result = nw; clean = true; }
      else if (nw.present && other->present && nw.oid == other->oid) { result = nw; clean = true; }
      else if (nw.present && other->present) {
        aios::userland::GitMerge3Options mo;
        mo.ours_label = "rebase destination";
        mo.theirs_label = other == &k.theirs ? k.theirs_label : k.ours_label;
        auto m = aios::userland::git_merge3(k.base.present ? blob_text(c, k.base.oid) : "", blob_text(c, nw.oid), blob_text(c, other->oid), mo);
        if (!m.binary) {
          auto w = c.repo->write_object("blob", span(m.merged));
          if (!w.ok()) die(c, w.error().message);
          result = Side{*w, nw.mode, true};
          clean = m.clean;
        }
      }
      if (!result) continue;
      if (ents.empty()) {
        std::vector<GitIndexEntry> flat;
        if (merged.tree != empty_tree_oid()) (void)c.repo->flatten_tree(merged.tree, "", flat);
        for (auto& e : flat) ents[e.path] = e;
      }
      if (result->present) ents[p] = GitIndexEntry{p, result->oid, result->mode};
      else ents.erase(p);
      touched = true;
      if (clean) { carried.erase(p); merged.conflicts.erase(p); }
      else {
        PathConflict nk = k;
        if (other == &k.theirs) nk.ours = nw; else nk.theirs = nw;
        nk.ours_label = "rebase destination";
        nk.materialized = result->oid;
        carried[p] = nk;
        merged.conflicts.erase(p);
      }
    }
    if (touched) {
      std::vector<GitIndexEntry> flat;
      for (auto& [p, e] : ents) flat.push_back(e);
      merged.tree = write_tree_of(c, flat);
    }
  }
  for (auto& [p, k] : new_base.conflicts) if (!carried.count(p) || carried[p].materialized != k.materialized) carried.emplace(p, k);
  for (auto& [p, k] : merged.conflicts) carried[p] = k;
  CommitSpec s = spec_of(c, cm);
  s.parents = new_parents;
  s.tree = merged.tree;
  s.conflict_sides = write_conflicts(c, still_conflicted(c, merged.tree, carried));
  return write_commit(c, s);
}

Tx::Tx(Ctx& c, std::string description) : c_(c), description_(std::move(description)) { wc_before_ = wc_commit(c); }

void Tx::rewrite(const std::string& o, const std::string& n) { if (o != n) rewritten_[o] = n; }
void Tx::abandon(const std::string& oid) { abandoned_[oid] = commit_at(c_, oid).parents; }
void Tx::move_out(const std::string& o, const std::string& n) {
  moved_[o] = n;
  moved_parents_[o] = commit_at(c_, o).parents;
}
void Tx::add_head(const std::string& oid) { new_heads_.push_back(oid); }
void Tx::set_wc(const std::string& oid) { new_wc_ = oid; }

std::string Tx::mapped(const std::string& oid) const {
  std::string cur = oid;
  for (int guard = 0; guard < 10000; ++guard) {
    auto it = rewritten_.find(cur);
    if (it == rewritten_.end()) return cur;
    cur = it->second;
  }
  return cur;
}

std::vector<std::string> Tx::new_parents(const std::vector<std::string>& parents) const {
  std::vector<std::string> out;
  std::function<void(const std::string&, int)> add = [&](const std::string& p, int depth) {
    std::string m = mapped(p);
    auto ab = abandoned_.find(m);
    if (ab != abandoned_.end() && depth < 1000) { for (auto& q : ab->second) add(q, depth + 1); return; }
    auto mv = moved_parents_.find(m);
    if (mv != moved_parents_.end() && depth < 1000) { for (auto& q : mv->second) add(q, depth + 1); return; }
    if (std::find(out.begin(), out.end(), m) == out.end()) out.push_back(m);
  };
  for (auto& p : parents) add(p, 0);
  return out;
}

size_t Tx::finish() {
  for (auto* m : {&rewritten_}) for (auto& [o, n] : *m) {
    (void)n;
    if (!allow_immutable && is_immutable(c_, o)) die(c_, "commit " + c_.abbrev(o) + " is immutable (a remote branch or tag holds it); --ignore-immutable rewrites it anyway");
  }
  for (auto& [o, ps] : abandoned_) {
    (void)ps;
    if (!allow_immutable && is_immutable(c_, o)) die(c_, "commit " + c_.abbrev(o) + " is immutable; --ignore-immutable abandons it anyway");
  }
  // 1. what descends from anything rewritten or abandoned, among the visible commits
  std::set<std::string> olds;
  for (auto& [o, n] : rewritten_) olds.insert(o);
  for (auto& [o, ps] : abandoned_) olds.insert(o);
  for (auto& [o, n] : moved_) olds.insert(o);
  size_t rebased = 0;
  if (!olds.empty()) {
    uint64_t cutoff = UINT64_MAX;
    for (auto& o : olds) cutoff = std::min<uint64_t>(cutoff, commit_at(c_, o).committer_time);
    cutoff = cutoff > 86400 ? cutoff - 86400 : 0;   // a day of clock skew between a commit and its children
    std::vector<std::string> roots = roots_of(c_);
    for (auto& h : new_heads_) roots.push_back(h);
    // walk back from the roots, never below the cutoff
    std::unordered_set<std::string> seen;
    std::vector<std::string> order;
    std::vector<std::string> stack(roots.begin(), roots.end());
    while (!stack.empty()) {
      std::string x = stack.back();
      stack.pop_back();
      if (!seen.insert(x).second) continue;
      const GitCommit& cm = commit_at(c_, x);
      if (cm.committer_time < cutoff && !olds.count(x)) continue;
      order.push_back(x);
      if (olds.count(x)) continue;
      for (auto& p : cm.parents) stack.push_back(p);
    }
    // descends(x): a parent is old or descends
    std::unordered_map<std::string, int> memo;   // 0 unknown, 1 yes, 2 no, 3 in progress
    std::function<bool(const std::string&)> descends = [&](const std::string& x) -> bool {
      int& m = memo[x];
      if (m == 1) return true;
      if (m == 2 || m == 3) return false;
      if (!seen.count(x)) { m = 2; return false; }
      m = 3;
      bool yes = false;
      for (auto& p : commit_at(c_, x).parents) if (olds.count(p) || descends(p)) yes = true;
      memo[x] = yes ? 1 : 2;
      return yes;
    };
    std::vector<std::string> desc;
    for (auto& x : order) if (!olds.count(x) && descends(x)) desc.push_back(x);
    // parents first
    std::set<std::string> dset(desc.begin(), desc.end()), placed;
    std::vector<std::string> topo;
    std::function<void(const std::string&)> place = [&](const std::string& x) {
      if (placed.count(x) || !dset.count(x)) return;
      placed.insert(x);
      for (auto& p : commit_at(c_, x).parents) place(p);
      topo.push_back(x);
    };
    for (auto& x : desc) place(x);
    for (auto& d : topo) {
      if (rewritten_.count(d)) continue;
      const GitCommit& cm = commit_at(c_, d);
      auto np = new_parents(cm.parents);
      if (np == cm.parents) continue;
      if (!allow_immutable && is_immutable(c_, d)) continue;
      rewritten_[d] = rebase_commit(c_, cm, np);
      ++rebased;
    }
  }
  // 2. refs: branches, head refs and other workspaces' working copies follow their commit's new version
  std::string wc_after;
  auto empty_on = [&](const std::vector<std::string>& np) {
    CommitSpec s;
    s.parents = np;
    MergedTree pt = parents_tree(c_, np);
    s.tree = pt.tree;
    s.conflict_sides = write_conflicts(c_, pt.conflicts);
    std::string w = write_commit(c_, s);
    new_heads_.push_back(w);
    return w;
  };
  {
    std::string w = !new_wc_.empty() ? new_wc_ : wc_before_;
    if (!w.empty()) {
      if (moved_.count(w)) wc_after = mapped(moved_.at(w));
      else if (abandoned_.count(w)) wc_after = empty_on(new_parents(abandoned_.at(w)));
      else wc_after = mapped(w);
    }
  }
  // @ first, by compare-and-swap from what it was when this began: before any other ref moves, so a transaction that
  // loses the race has changed nothing
  const bool wc_moves = (!wc_before_.empty() || !wc_after.empty()) && wc_after != wc_before_;
  if (wc_moves) {
    auto swapped = c_.repo->compare_and_swap_ref(wc_ref(c_), wc_before_, wc_after);
    if (!swapped.ok()) {
      if (swapped.error().status == aios::Status::Conflict && retry_if_wc_moved) throw WcMoved{};
      die(c_, swapped.error().status == aios::Status::Conflict
                  ? "the working copy (@) moved while this ran (another process changed it); nothing was changed: run it again"
                  : swapped.error().message);
    }
  }
  for (auto& [name, oid] : all_refs(c_)) {
    bool branch = name.rfind("refs/heads/", 0) == 0;
    bool headref = name.rfind("refs/changes/heads/", 0) == 0;
    bool wcref = name.rfind("refs/changes/wc/", 0) == 0;
    if ((!branch && !headref && !wcref) || name == wc_ref(c_)) continue;
    std::string target;
    if (moved_.count(oid)) {
      target = mapped(moved_.at(oid));
    } else if (abandoned_.count(oid)) {
      auto np = new_parents(abandoned_.at(oid));
      if (headref) { (void)c_.repo->delete_ref(name); for (auto& p : np) if (!is_immutable(c_, p)) new_heads_.push_back(p); continue; }
      if (np.empty()) { (void)c_.repo->delete_ref(name); continue; }
      target = np[0];
    } else {
      target = mapped(oid);
    }
    if (target == oid) continue;
    if (headref) {
      std::string cid = change_id_of(commit_at(c_, target));
      if ("refs/changes/heads/" + cid != name) (void)c_.repo->delete_ref(name);
      (void)c_.repo->update_ref("refs/changes/heads/" + cid, target);
      continue;
    }
    if (auto r = c_.repo->update_ref(name, target); !r.ok()) die(c_, r.error().message);
    append_reflog(c_, name, oid, target, "change: " + description_);
  }
  for (auto& h : new_heads_) {
    std::string m = mapped(h);
    if (abandoned_.count(m)) continue;
    (void)c_.repo->update_ref("refs/changes/heads/" + change_id_of(commit_at(c_, m)), m);
  }
  // A head ref that another head (or a working copy) is built on is redundant: the other keeps it visible.
  {
    auto heads = head_refs(c_);
    std::set<std::string> parents_of_roots;
    for (auto& [cid, oid] : heads) for (auto& p : commit_at(c_, oid).parents) parents_of_roots.insert(p);
    if (!wc_after.empty()) for (auto& p : commit_at(c_, wc_after).parents) parents_of_roots.insert(p);
    for (auto& [cid, oid] : heads) if (parents_of_roots.count(oid)) (void)c_.repo->delete_ref("refs/changes/heads/" + cid);
  }
  // 3. the working copy
  if (!wc_before_.empty() || !wc_after.empty()) {
    if (wc_moves) {
      auto old_head = read_ref(c_, "HEAD");
      if (auto r = c_.repo->set_head_detached(wc_after); !r.ok()) die(c_, r.error().message);
      append_reflog(c_, "HEAD", old_head.value_or(""), wc_after, "change: " + description_);
      std::string from = wc_before_.empty() ? empty_tree_oid() : commit_at(c_, wc_before_).tree;
      std::string to = commit_at(c_, wc_after).tree;
      if (from != to || c_.repo->index().empty() != (to == empty_tree_oid())) check_out_tree(c_, from, to);
    }
  }
  invalidate(c_);
  // 4. the operation log
  std::vector<std::pair<std::string, std::string>> rw;
  for (auto& [o, n] : rewritten_) rw.push_back({o, n});
  for (auto& [o, ps] : abandoned_) rw.push_back({o, ""});
  oplog::record(c_, description_, description_ == "snapshot working copy" ? "snapshot" : kind, "", rw);
  return rebased;
}

// ---------------------------------------------------------------------------------------------------------------
// the working copy
// ---------------------------------------------------------------------------------------------------------------
void forget(Ctx& c) { g_cache.erase(&c); }

void evolve(Ctx& c, const std::string& description) {
  forget(c);
  std::map<std::string, std::string> current;   // change id -> the version a ref holds
  std::set<std::string> pinned, twice;
  for (auto& [name, oid] : all_refs(c)) {
    if (name.rfind("refs/changes/heads/", 0) != 0 && name.rfind("refs/changes/wc/", 0) != 0 && name.rfind("refs/heads/", 0) != 0) continue;
    std::string cm = peel_to(c, oid, "commit");
    if (cm.empty()) continue;
    pinned.insert(cm);
    std::string cid = change_id_of(commit_at(c, cm));
    auto it = current.find(cid);
    if (it != current.end() && it->second != cm) twice.insert(cid);
    current[cid] = cm;
  }
  for (auto& t : twice) current.erase(t);
  Tx tx(c, description);
  tx.kind = "snapshot";
  bool any = false;
  for (auto& v : visible_commits(c)) {
    for (auto& p : commit_at(c, v).parents) {
      if (pinned.count(p)) continue;
      auto it = current.find(change_id_of(commit_at(c, p)));
      if (it == current.end() || it->second == p || is_immutable(c, p)) continue;
      tx.rewrite(p, it->second);
      any = true;
    }
  }
  if (any) tx.finish();
}

void check_out_tree(Ctx& c, const std::string& from_tree, const std::string& to_tree) {
  (void)from_tree;
  ensure_empty_tree(c);
  auto r = c.repo->checkout_tree(to_tree, true);
  if (!r.ok()) die(c, "could not check out the working-copy change: " + r.error().message);
}

namespace {
// The work tree's files as a new index (the index is @'s tree): changed files hashed, new files added (unless
// ignored, or too big to have been meant), deleted ones dropped. Paths outside the sparse checkout keep @'s
// version. Returns whether anything changed.
bool stage_worktree(Ctx& c, std::vector<GitIndexEntry>& next, std::vector<std::string>* skipped_big) {
  bool changed = false;
  std::map<std::string, GitIndexEntry> idx;
  for (auto& e : c.repo->index()) idx[e.path] = e;
  for (auto& u : c.repo->unmerged()) {
    // an unmerged path (a plain git merge stopped here) is whatever its file holds now
    if (!idx.count(u.path)) { GitIndexEntry e = u; e.stage = 0; e.oid = std::string(40, '0'); idx[u.path] = e; changed = true; }
  }
  auto changes = c.repo->worktree_changes();
  if (!changes.ok()) die(c, changes.error().message);
  for (auto& ch : *changes) {
    auto it = idx.find(ch.path);
    if (ch.change == aios::userland::GitChange::Deleted) {
      if (it != idx.end() && !it->second.skip_worktree) { idx.erase(it); changed = true; }
      continue;
    }
    auto s = c.repo->stage_entry(ch.path, it == idx.end() ? nullptr : &it->second);
    if (!s.ok()) { if (it != idx.end()) idx.erase(it); changed = true; continue; }
    idx[ch.path] = *s;
    changed = true;
  }
  for (auto& u : c.repo->unmerged()) {
    auto s = c.repo->stage_entry(u.path, nullptr);
    if (s.ok()) idx[u.path] = *s; else idx.erase(u.path);
  }
  long long max_new = c.config.get_int("change.maxnewfilesize", 1 << 20);
  auto un = c.repo->untracked(false);
  if (un.ok()) {
    for (auto& p : *un) {
      if (idx.count(p)) continue;
      auto st = c.host().stat(join(c.top, p));
      if (max_new > 0 && st.ok() && !st->is_link && (long long)st->size > max_new) {
        if (skipped_big) skipped_big->push_back(p);
        continue;
      }
      auto s = c.repo->stage_entry(p, nullptr);
      if (!s.ok()) continue;
      idx[p] = *s;
      changed = true;
    }
  }
  next.clear();
  for (auto& [p, e] : idx) next.push_back(e);
  return changed;
}
}  // namespace

bool ensure_wc(Ctx& c) {
  if (!wc_commit(c).empty()) return true;
  // Only a workspace other than the repository's own starts itself: "default" without an @ means the layer was never
  // set up here, which `git change init` does on purpose.
  if (workspace(c) == "default") return false;
  auto head = read_ref(c, "HEAD");
  if (!head) return false;
  const std::string head_commit = peel_to(c, *head, "commit");
  if (head_commit.empty()) return false;
  CommitSpec s;
  s.parents = {head_commit};
  s.tree = commit_at(c, head_commit).tree;
  s.conflict_sides = write_conflicts(c, read_conflicts(c, commit_at(c, head_commit)));
  const std::string fresh = write_commit(c, s);
  // created only if still missing: two commands starting the same workspace at once make one @, not two
  if (!c.repo->compare_and_swap_refs({{wc_ref(c), "", fresh}}).ok()) {
    invalidate(c);
    return !wc_commit(c).empty();
  }
  (void)c.repo->set_head_detached(fresh);
  append_reflog(c, "HEAD", head_commit, fresh, "change: start workspace " + workspace(c));
  (void)c.repo->update_ref("refs/changes/heads/" + change_id_of(commit_at(c, fresh)), fresh);
  invalidate(c);
  oplog::record(c, "start workspace " + workspace(c) + " on " + c.abbrev(head_commit), "normal", "", {});
  return true;
}

bool snapshot(Ctx& c) {
  std::string wc = wc_commit(c);
  if (wc.empty()) return false;
  bool recorded = false;
  auto head = read_ref(c, "HEAD");
  if (head && *head != wc) {
    // A plain git command (commit, checkout, reset, merge...) moved HEAD: the working copy starts again on top of
    // where it went. The old @ stays as a change of its own unless it was empty and undescribed.
    const GitCommit& old = commit_at(c, wc);
    std::string head_commit = peel_to(c, *head, "commit");
    CommitSpec s;
    s.parents = {head_commit};
    s.tree = commit_at(c, head_commit).tree;
    s.conflict_sides = write_conflicts(c, read_conflicts(c, commit_at(c, head_commit)));
    std::string fresh = write_commit(c, s);
    if (is_empty_change(c, old) && old.raw_message.empty() && !is_ancestor(c, wc, head_commit)) {
      auto heads = head_refs(c);
      std::string cid = change_id_of(old);
      if (heads.count(cid) && heads[cid] == wc) (void)c.repo->delete_ref("refs/changes/heads/" + cid);
    }
    // the disk already holds what the git command left there: no checkout
    (void)c.repo->update_ref(wc_ref(c), fresh);
    (void)c.repo->set_head_detached(fresh);
    append_reflog(c, "HEAD", *head, fresh, "change: import git HEAD");
    (void)c.repo->update_ref("refs/changes/heads/" + change_id_of(commit_at(c, fresh)), fresh);
    invalidate(c);
    oplog::record(c, "import git HEAD " + c.abbrev(head_commit), "normal", "", {});
    wc = fresh;
    recorded = true;
  }
  // @ is swapped from the version this read (Tx::finish): when the fsmonitor daemon's lock-free snapshot moved it
  // first, the files are read again on top of it
  for (int attempt = 0;; ++attempt) {
    std::vector<GitIndexEntry> next;
    std::vector<std::string> big;
    bool changed = stage_worktree(c, next, &big);
    if (attempt == 0)
      for (auto& b : big)
        c.err << "warning: " << b << " is new and bigger than change.maxNewFileSize; left untracked (git change track " << b << " adds it)\n";
    if (!changed) return recorded;
    try {
      set_wc_index(c, next, "snapshot working copy", attempt < 4);
      return true;
    } catch (const WcMoved&) {
      invalidate(c);
    }
  }
}

void set_wc_index(Ctx& c, const std::vector<GitIndexEntry>& next, const std::string& description, bool retry_if_wc_moved) {
  std::string wc = wc_commit(c);
  std::string tree = write_tree_of(c, next);
  c.repo->set_index(next);
  c.repo->set_unmerged({});
  if (auto r = c.repo->write_index(); !r.ok()) die(c, r.error().message);
  const GitCommit& cur = commit_at(c, wc);
  if (tree == cur.tree) return;
  CommitSpec s = spec_of(c, cur);
  s.tree = tree;
  s.conflict_sides = write_conflicts(c, still_conflicted(c, tree, read_conflicts(c, cur)));
  std::string fresh = write_commit(c, s);
  Tx tx(c, description);
  tx.rewrite(wc, fresh);
  tx.allow_immutable = true;
  tx.retry_if_wc_moved = retry_if_wc_moved;
  // the disk is the new version already: the index was just written to match it, so the checkout touches nothing
  tx.finish();
}

bool snapshot_unlocked(Ctx& c) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (!repo_lock_free(c)) return true;   // a command is running: it snapshots the files itself
    invalidate(c);
    const std::string wc = wc_commit(c);
    if (wc.empty()) return true;
    auto head = read_ref(c, "HEAD");
    if (!head || *head != wc) return false;
    for (auto& [cid, oid] : head_refs(c))
      if (oid != wc && is_ancestor(c, wc, oid)) return false;
    std::vector<GitIndexEntry> next;
    std::vector<std::string> big;
    if (!stage_worktree(c, next, &big)) return true;
    const std::string tree = write_tree_of(c, next);
    const GitCommit& cur = commit_at(c, wc);
    if (tree == cur.tree) return true;
    CommitSpec s = spec_of(c, cur);
    s.tree = tree;
    s.conflict_sides = write_conflicts(c, still_conflicted(c, tree, read_conflicts(c, cur)));
    const std::string fresh = write_commit(c, s);
    std::vector<Git::RefSwap> swaps{{"HEAD", wc, fresh}, {wc_ref(c), wc, fresh}};
    const std::string cid_ref = "refs/changes/heads/" + change_id_of(cur);
    if (auto now = c.repo->ref_oid(cid_ref); now.ok() && *now == wc) swaps.push_back({cid_ref, wc, fresh});
    if (!c.repo->compare_and_swap_refs(swaps).ok()) continue;   // moved under it: read again
    // the files' stat data, for the next snapshot (only a cache: a stale one costs a re-read, never a wrong tree)
    c.repo->set_index(next);
    c.repo->set_unmerged({});
    (void)c.repo->write_index();
    append_reflog(c, "HEAD", wc, fresh, "change: snapshot working copy");
    invalidate(c);
    oplog::record(c, "snapshot working copy", "snapshot", "", {{wc, fresh}});
    return true;
  }
  return true;   // @ kept moving: the commands moving it snapshot the files
}

void open_changes(Ctx& c) {
  c.open_repo();
  if (!enabled(c)) die(c, "the change layer is off in this repository: 'git change init' turns it on");
  if (!ensure_wc(c)) die(c, "the change layer is on but there is no working-copy change: 'git change init' makes one");
  snapshot(c);
}

void enable(Ctx& c) {
  c.open_repo();
  if (enabled(c) && !wc_commit(c).empty()) { c.out << "The change layer is already on here.\n"; return; }
  if (auto r = c.repo->set_config("change.enabled", "true"); !r.ok()) die(c, r.error().message);
  c.config.add("change.enabled", "true");
  oplog::ensure_started(c, "start the operation log");
  auto head = read_ref(c, "HEAD");
  std::string branch = current_branch(c);
  // an unborn branch is made by the first git change commit
  if (!branch.empty() && !head) (void)c.repo->set_config("change.branch", branch);
  CommitSpec s;
  if (head) s.parents = {*head};
  s.tree = head ? commit_at(c, *head).tree : empty_tree_oid();
  std::string wc = write_commit(c, s);
  (void)c.repo->update_ref(wc_ref(c), wc);
  (void)c.repo->update_ref("refs/changes/heads/" + change_id_of(commit_at(c, wc)), wc);
  if (auto r = c.repo->set_head_detached(wc); !r.ok()) die(c, r.error().message);
  append_reflog(c, "HEAD", head.value_or(""), wc, "change: init");
  invalidate(c);
  oplog::record(c, "turn on the change layer", "normal", "", {});
  // what was staged or edited goes into @ now
  snapshot(c);
  std::string now = wc_commit(c);
  c.out << "Working copy is now change " << short_id(change_id_of(commit_at(c, now)));
  if (head) c.out << " on top of " << (branch.empty() ? c.abbrev(*head) : branch + " (" + c.abbrev(*head) + ")");
  c.out << ".\n";
  if (!branch.empty())
    c.out << "HEAD is detached at the working copy; " << branch << " stays where it is and 'git change commit' advances it.\n";
  c.out << "Edits are snapshotted into @ by every command: there is no staging area and nothing to stash.\n";
}

void disable(Ctx& c, const std::string& branch_named) {
  c.open_repo();
  if (!enabled(c)) { c.out << "The change layer is already off here.\n"; return; }
  // the files as they are now go into @ first: whatever follows, they are recorded in a change
  if (!wc_commit(c).empty()) snapshot(c);
  const std::string wc = wc_commit(c);
  std::vector<std::string> parents;
  if (!wc.empty()) parents = commit_at(c, wc).parents;
  else if (auto h = read_ref(c, "HEAD")) parents = {peel_to(c, *h, "commit")};
  if (parents.empty() || parents[0].empty())
    die(c, "the working copy has no parent commit to put a branch on: 'git change commit -m MSG' records it first");
  std::string base, branch;
  if (!branch_named.empty()) {
    const std::string ref = "refs/heads/" + branch_named;
    auto at = c.repo->ref_oid(ref);
    if (!at.ok()) die(c, "no branch named '" + branch_named + "'");
    base = parents[0];
    if (*at != base) {
      // behind the working copy's parent: moved forward to it, as 'git change commit' would have moved it
      if (!is_ancestor(c, *at, base))
        die(c, "'" + branch_named + "' (" + c.abbrev(*at) + ") is not under the working copy's parent " + c.abbrev(base) +
                   ": moving it there would drop commits. Nothing was changed.");
      if (auto r = c.repo->update_ref(ref, base); !r.ok()) die(c, r.error().message);
      append_reflog(c, ref, *at, base, "change: disable, fast-forward to the working copy's parent");
    }
    branch = branch_named;
  } else {
    std::vector<std::pair<std::string, std::string>> found;   // branch, the parent it points at
    for (auto& [name, oid] : all_refs(c)) {
      if (name.rfind("refs/heads/", 0) != 0) continue;
      if (std::find(parents.begin(), parents.end(), oid) != parents.end()) found.push_back({name.substr(11), oid});
    }
    std::string preferred = c.config.get("change.branch").value_or("");
    for (auto& want : {preferred, std::string("main"), std::string("master")})
      for (auto& f : found)
        if (branch.empty() && !want.empty() && f.first == want) { branch = f.first; base = f.second; }
    if (branch.empty() && found.size() == 1) { branch = found[0].first; base = found[0].second; }
    if (branch.empty()) {
      std::string names;
      for (auto& f : found) names += (names.empty() ? "" : ", ") + f.first;
      die(c, found.empty()
                 ? "no branch points at the working copy's parent " + c.abbrev(parents[0]) +
                       ": name one with --branch NAME (moved forward to it when it is behind). Nothing was changed."
                 : "several branches point at the working copy's parents (" + names + "): name one with --branch NAME. Nothing was changed.");
    }
  }
  // the index becomes the branch's commit, so what @ held on top of it shows as uncommitted changes in the work tree,
  // as git would show them; the stat data of entries that did not change is kept
  Snapshot target = snapshot_tree(c, tree_of_commit(c, base));
  std::map<std::string, GitIndexEntry> was;
  for (auto& e : c.repo->index()) was[e.path] = e;
  std::vector<GitIndexEntry> next;
  for (auto& [path, f] : target) {
    auto it = was.find(path);
    if (it != was.end() && it->second.oid == f.oid && it->second.mode == f.mode) next.push_back(it->second);
    else next.push_back(GitIndexEntry{path, f.oid, f.mode, 0, 0});
  }
  c.repo->set_index(next);
  c.repo->set_unmerged({});
  if (auto r = c.repo->write_index(); !r.ok()) die(c, r.error().message);
  const std::string head_before = read_ref(c, "HEAD").value_or("");
  if (auto r = c.repo->set_head("refs/heads/" + branch); !r.ok()) die(c, r.error().message);
  append_reflog(c, "HEAD", head_before, base, "change: disable, back on " + branch);
  if (!wc.empty()) (void)c.repo->delete_ref(wc_ref(c));
  if (auto r = c.repo->set_config("change.enabled", "false"); !r.ok()) die(c, r.error().message);
  invalidate(c);
  oplog::record(c, "turn off the change layer", "normal", "", {});
  c.out << "The change layer is off. HEAD is on " << branch << " (" << c.abbrev(base) << ")";
  if (!wc.empty() && commit_at(c, wc).tree != tree_of_commit(c, base)) c.out << "; what the working copy held is in the work tree as uncommitted changes";
  c.out << ".\nIts changes stay under refs/changes; 'git change init' turns the layer on again.\n";
}

// ---------------------------------------------------------------------------------------------------------------
// showing
// ---------------------------------------------------------------------------------------------------------------
bool is_empty_change(Ctx& c, const GitCommit& cm) { return parents_tree(c, cm.parents).tree == cm.tree; }

std::string describe_line(Ctx& c, const GitCommit& cm) {
  std::string out = short_id(change_id_of(cm)) + " " + cm.oid.substr(0, 8);
  std::vector<std::string> names;
  for (auto& [name, oid] : all_refs(c)) {
    if (oid != cm.oid) continue;
    if (name.rfind("refs/heads/", 0) == 0) names.push_back(name.substr(11));
    else if (name.rfind("refs/remotes/", 0) == 0) names.push_back(name.substr(13));
    else if (name.rfind("refs/tags/", 0) == 0) names.push_back("tag:" + name.substr(10));
  }
  for (auto& n : names) out += " " + n;
  std::string subj = subject_of(cm.raw_message);
  out += " " + (subj.empty() ? std::string("(no description set)") : subj);
  if (!cm.header("conflict-sides").empty()) out += " (conflict)";
  if (is_empty_change(c, cm)) out += " (empty)";
  return out;
}

}  // namespace aios_git::change
