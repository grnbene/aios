// branch, tag, checkout, switch, merge, cherry-pick, revert, stash: moving between and joining lines of history.
#include <algorithm>
#include <cctype>
#include <sstream>

#include "aios/hal/host.hpp"
#include "cli_rewrite.hpp"
#include "color.hpp"

namespace aios_git {

using namespace aios;
using namespace aios::userland;

namespace {

std::string repo_config_path(Ctx& c) { return c.repo->common_dir() + "/config"; }

void set_config(Ctx& c, const std::string& key, const std::string& value) {
  std::string err;
  if (!edit_config_file(c.host(), repo_config_path(c), key, value, ConfigEdit::Set, &err)) die(c, err);
  c.load_config();
}

void set_tracking(Ctx& c, const std::string& branch, const std::string& upstream_ref, bool quiet) {
  // refs/remotes/<remote>/<b> -> remote <remote>, merge refs/heads/<b>; a local branch tracks with remote "."
  std::string remote, merge;
  if (upstream_ref.rfind("refs/remotes/", 0) == 0) {
    std::string rest = upstream_ref.substr(13);
    // the remote is the longest configured remote name that prefixes it
    std::string best;
    for (auto& e : c.config.entries()) {
      if (e.key.rfind("remote.", 0) != 0 || e.key.size() < 12 || e.key.compare(e.key.size() - 4, 4, ".url") != 0) continue;
      std::string r = e.key.substr(7, e.key.size() - 11);
      if (rest.rfind(r + "/", 0) == 0 && r.size() > best.size()) best = r;
    }
    if (best.empty()) best = rest.substr(0, rest.find('/'));
    remote = best;
    merge = "refs/heads/" + rest.substr(best.size() + 1);
  } else if (upstream_ref.rfind("refs/heads/", 0) == 0) {
    remote = ".";
    merge = upstream_ref;
  } else {
    die(c, "the requested upstream branch '" + upstream_ref + "' does not exist");
  }
  set_config(c, "branch." + branch + ".remote", remote);
  set_config(c, "branch." + branch + ".merge", merge);
  if (!quiet) {
    std::string shown = remote == "." ? merge.substr(11) : remote + "/" + merge.substr(11);
    c.out << "branch '" << branch << "' set up to track '" << shown << "'.\n";
  }
}

bool valid_branch_name(const std::string& n) {
  if (n.empty() || n[0] == '-' || n == "HEAD") return false;
  return Git::valid_ref_name("refs/heads/" + n);
}

// --contains and its kind take the next argument as their commit when given none ("--contains feat").
void take_optional_commit(Parsed& p, std::initializer_list<const char*> keys) {
  for (const char* k : keys)
    if (p.has(k) && p.get(k).empty() && !p.pos.empty()) {
      p.values[k].back() = p.pos[0];
      p.pos.erase(p.pos.begin());
    }
}

std::string short_name(const std::string& full) {
  for (const char* p : {"refs/heads/", "refs/remotes/", "refs/tags/"})
    if (full.rfind(p, 0) == 0) return full.substr(std::strlen(p));
  return full;
}

// The remote-tracking branch `name` could be (for checkout's guess): exactly one remote has it.
std::string guess_remote_branch(Ctx& c, const std::string& name) {
  std::string found;
  int n = 0;
  for (auto& [ref, oid] : all_refs(c)) {
    if (ref.rfind("refs/remotes/", 0) != 0) continue;
    std::string rest = ref.substr(13);
    size_t slash = rest.find('/');
    if (slash == std::string::npos) continue;
    if (rest.substr(slash + 1) == name) { found = ref; ++n; }
  }
  return n == 1 ? found : "";
}

void print_local_changes(Ctx& c) {
  auto wt = c.repo->worktree_changes();
  if (!wt.ok()) return;
  std::set<std::string> shown;
  Snapshot head;
  if (auto h = read_ref(c, "HEAD")) head = snapshot_tree(c, read_commit(c, *h).tree);
  std::map<std::string, char> lines;
  for (auto& e : c.repo->index()) {
    auto it = head.find(e.path);
    if (it == head.end()) lines[e.path] = 'A';
    else if (it->second.oid != e.oid || it->second.mode != e.mode) lines[e.path] = 'M';
  }
  for (auto& [p, f] : head) {
    bool in = false;
    for (auto& e : c.repo->index()) if (e.path == p) { in = true; break; }
    if (!in) lines[p] = 'D';
  }
  for (auto& ch : *wt) lines[ch.path] = ch.change == GitChange::Deleted ? 'D' : 'M';
  for (auto& [p, ch] : lines) c.out << ch << "\t" << p << "\n";
}

void tracking_info(Ctx& c) {
  std::string b = current_branch(c);
  if (b.empty()) return;
  std::string up = upstream_of(c, b);
  if (up.empty()) return;
  auto mine = read_ref(c, "refs/heads/" + b);
  auto theirs = read_ref(c, up);
  std::string name = short_name(up);
  if (!theirs) {
    c.out << "Your branch is based on '" << name << "', but the upstream is gone.\n  (use \"git branch --unset-upstream\" to fixup)\n";
    return;
  }
  if (!mine) return;
  WalkOptions a; a.tips = {*mine}; a.hide = {*theirs};
  WalkOptions b2; b2.tips = {*theirs}; b2.hide = {*mine};
  long ahead = (long)walk(c, a).size(), behind = (long)walk(c, b2).size();
  if (!ahead && !behind) c.out << "Your branch is up to date with '" << name << "'.\n";
  else if (ahead && !behind) c.out << "Your branch is ahead of '" << name << "' by " << ahead << " commit" << (ahead == 1 ? "" : "s") << ".\n  (use \"git push\" to publish your local commits)\n";
  else if (!ahead) c.out << "Your branch is behind '" << name << "' by " << behind << " commit" << (behind == 1 ? "" : "s") << ", and can be fast-forwarded.\n  (use \"git pull\" to update your local branch)\n";
  else c.out << "Your branch and '" << name << "' have diverged,\nand have " << ahead << " and " << behind << " different commits each, respectively.\n  (use \"git pull\" if you want to integrate the remote branch with yours)\n";
}

std::string previous_branch(Ctx& c) {
  auto log = c.host().read_file(c.repo->git_dir() + "/logs/HEAD");
  if (!log.ok()) return "";
  std::string text((const char*)log->data(), log->size());
  size_t pos = text.size();
  while (true) {
    size_t at = text.rfind("checkout: moving from ", pos);
    if (at == std::string::npos) return "";
    size_t from = at + 22;
    size_t to = text.find(" to ", from);
    size_t nl = text.find('\n', from);
    if (to != std::string::npos && (nl == std::string::npos || to < nl)) return text.substr(from, to - from);
    if (at == 0) return "";
    pos = at - 1;
  }
}

// Moves HEAD (and the work tree) to a branch or a commit. `branch` empty: detach at `oid`.
enum class NewBranch { No, Created, Reset };
int move_to(Ctx& c, const std::string& branch, const std::string& oid, bool force, bool quiet, const std::string& shown_name,
            NewBranch is_new = NewBranch::No, bool explicit_detach = false) {
  auto old_head = read_ref(c, "HEAD");
  bool leaving_detached = current_branch(c).empty() && old_head;
  std::string old_name = current_branch(c);
  if (old_name.empty() && old_head) old_name = *old_head;
  // git's skip_merge_working_tree: a new branch made at the commit HEAD is on, nothing forced or detached, no sparse
  // checkout - the index and the files stay as they are (and no local changes are listed). The whole tree was read,
  // compared with itself and the index written again: switch -c took 11 ms where git takes 4.
  const bool skip = is_new == NewBranch::Created && !force && !explicit_detach && old_head && *old_head == oid && !c.repo->sparse();
  if (!skip) {
    std::string tree = read_commit(c, oid).tree;
    auto r = c.repo->checkout_tree(tree, force);
    if (!r.ok()) {
      std::string m = r.error().message;
      c.err << "error: " << m << "\nAborting\n";
      return 1;
    }
  }
  std::string msg = "checkout: moving from " + (current_branch(c).empty() ? (old_head ? *old_head : std::string("HEAD")) : current_branch(c)) +
                    " to " + (branch.empty() ? oid : branch);
  if (!branch.empty()) {
    auto s = c.repo->set_head("refs/heads/" + branch);
    if (!s.ok()) die(c, s.error().message);
    append_reflog(c, "HEAD", old_head.value_or(""), oid, msg);
    if (!quiet) {
      if (!skip) print_local_changes(c);
      if (leaving_detached && *old_head != oid)
        c.err << "Previous HEAD position was " << c.abbrev(*old_head) << " " << subject_of(read_commit(c, *old_head).message) << "\n";
      if (is_new == NewBranch::Created) c.err << "Switched to a new branch '" << branch << "'\n";
      else if (is_new == NewBranch::Reset) c.err << "Reset branch '" << branch << "'\n";
      else c.err << "Switched to branch '" << branch << "'\n";
      if (is_new == NewBranch::No) tracking_info(c);
    }
  } else {
    bool was_detached = current_branch(c).empty();
    auto s = c.repo->set_head_detached(oid);
    if (!s.ok()) die(c, s.error().message);
    append_reflog(c, "HEAD", old_head.value_or(""), oid, msg);
    if (!quiet) {
      print_local_changes(c);
      if (!was_detached || !old_head || *old_head != oid) {
        if (!was_detached && !explicit_detach) {
          detached_advice(c, shown_name);
        } else if (was_detached && old_head && *old_head != oid) {
          c.err << "Previous HEAD position was " << c.abbrev(*old_head) << " " << subject_of(read_commit(c, *old_head).message) << "\n";
        }
      }
      c.err << "HEAD is now at " << c.abbrev(oid) << " " << subject_of(read_commit(c, oid).message) << "\n";
    }
  }
  return 0;
}

// Restores paths from the index (or a tree, which also updates the index).
int checkout_paths(Ctx& c, const std::string& rev, const std::vector<std::string>& paths, bool ours, bool theirs, bool report = false) {
  int written = 0;
  auto report_count = [&](const std::string& from) {
    if (report) c.err << "Updated " << written << (written == 1 ? " path from " : " paths from ") << from << "\n";
  };
  Pathspec ps = make_pathspec(c, paths);
  std::vector<bool> matched(ps.items.size(), false);
  auto mark = [&](const std::string& p) { int i = ps.matched_index(p); if (i >= 0) matched[(size_t)i] = true; };
  std::vector<GitIndexEntry> index = c.repo->index();
  if (!rev.empty()) {
    std::string tree = peel_to(c, must_resolve(c, rev), "tree");
    if (tree.empty()) die(c, "reference is not a tree: " + rev);
    Snapshot t = snapshot_tree(c, tree);
    std::map<std::string, size_t> at;
    for (size_t i = 0; i < index.size(); ++i) at[index[i].path] = i;
    std::vector<GitIndexEntry> still;
    for (auto& u : c.repo->unmerged()) if (!ps.match(u.path)) still.push_back(u);
    for (auto& [p, f] : t) {
      if (!ps.match(p)) continue;
      mark(p);
      auto w = c.repo->write_worktree_file(p, f.oid, f.mode);
      if (!w.ok()) die(c, w.error().message);
      ++written;
      auto it = at.find(p);
      if (it != at.end()) index[it->second] = *w;
      else { at[p] = index.size(); index.push_back(*w); }
    }
    for (size_t i = 0; i < matched.size(); ++i)
      if (!matched[i]) { c.err << "error: pathspec '" << ps.raw_of(i) << "' did not match any file(s) known to git\n"; return 1; }
    c.repo->set_unmerged(still);
    c.repo->set_index(index);
    if (auto w = c.repo->write_index(); !w.ok()) die(c, w.error().message);
    report_count(c.abbrev(tree));
    return 0;
  }
  for (auto& u : c.repo->unmerged()) {
    if (!ps.match(u.path)) continue;
    mark(u.path);
    if (!ours && !theirs) { c.err << "error: path '" << u.path << "' is unmerged\n"; return 1; }
  }
  for (auto& u : c.repo->unmerged()) {
    if (!ps.match(u.path)) continue;
    if ((ours && u.stage == 2) || (theirs && u.stage == 3)) {
      auto w = c.repo->write_worktree_file(u.path, u.oid, u.mode);
      if (!w.ok()) die(c, w.error().message);
    }
  }
  for (auto& e : index) {
    if (!ps.match(e.path)) continue;
    mark(e.path);
    if (c.repo->worktree_matches(e)) continue;
    auto w = c.repo->write_worktree_file(e.path, e.oid, e.mode);
    if (!w.ok()) die(c, w.error().message);
    ++written;
    e.mtime_ns = w->mtime_ns;
    e.size = w->size;
  }
  for (size_t i = 0; i < matched.size(); ++i)
    if (!matched[i]) { c.err << "error: pathspec '" << ps.raw_of(i) << "' did not match any file(s) known to git\n"; return 1; }
  c.repo->set_index(index);
  if (auto w = c.repo->write_index(); !w.ok()) die(c, w.error().message);
  report_count("the index");
  return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// merging into the work tree
// ---------------------------------------------------------------------------
// Writes a tree merge into the index and work tree. Refuses (returns false, prints git's error) when
// local changes or untracked files are in the way, before touching anything.
bool apply_tree_merge(Ctx& c, const std::string& base_tree, const std::string& ours_tree, const std::string& theirs_tree,
                      const GitTreeMerge& tm, const std::string& ours_label, const std::string& theirs_label, MergeApply& out,
                      const char* what) {
  Snapshot base = snapshot_tree(c, base_tree), ours = snapshot_tree(c, ours_tree), theirs = snapshot_tree(c, theirs_tree);
  std::map<std::string, const GitPathConflict*> conf;
  for (auto& k : tm.conflicts) conf[k.path] = &k;
  // what each path should hold afterwards
  std::map<std::string, FileState> want;
  for (auto& e : tm.entries) want[e.path] = FileState{e.oid, e.mode, true};
  std::map<std::string, std::string> conflict_text;
  for (auto& [path, k] : conf) {
    if (k->kind == "content" || k->kind == "add/add" || (k->kind == "binary" && false)) {
      conflict_text[path] = k->file.merged;
    } else if (k->kind == "modify/delete" || k->kind == "rename/delete") {
      auto o = ours.find(path), t = theirs.find(path);
      if (o != ours.end()) want[path] = o->second;
      else if (t != theirs.end()) want[path] = t->second;
    } else {
      auto o = ours.find(path), t = theirs.find(path);
      if (o != ours.end()) want[path] = o->second;
      else if (t != theirs.end()) want[path] = t->second;
    }
  }
  // paths whose content changes in the work tree
  std::set<std::string> touched;
  for (auto& [p, f] : want) {
    auto o = ours.find(p);
    if (o == ours.end() || o->second.oid != f.oid || o->second.mode != f.mode || conflict_text.count(p)) touched.insert(p);
  }
  for (auto& [p, f] : ours) if (!want.count(p) && !conflict_text.count(p)) touched.insert(p);
  for (auto& [p, t] : conflict_text) touched.insert(p);
  // local changes in the way
  std::map<std::string, const GitIndexEntry*> idx;
  for (auto& e : c.repo->index()) idx[e.path] = &e;
  std::vector<std::string> dirty, in_way;
  for (auto& p : touched) {
    auto i = idx.find(p);
    auto o = ours.find(p);
    if (i == idx.end()) {
      if (o != ours.end()) { dirty.push_back(p); continue; }   // staged deletion
      auto st = c.host().stat(join(c.top, p));
      if (st.ok() && !st->is_dir) {
        auto w = want.find(p);
        if (w == want.end() || hash_blob(read_worktree_file(c, p)) != w->second.oid) in_way.push_back(p);
      }
      continue;
    }
    if (o == ours.end() || o->second.oid != i->second->oid || o->second.mode != i->second->mode) { dirty.push_back(p); continue; }
    if (!c.repo->worktree_matches(*i->second)) dirty.push_back(p);
  }
  if (!dirty.empty()) {
    c.err << "error: Your local changes to the following files would be overwritten by " << what << ":\n";
    for (auto& d : dirty) c.err << "\t" << d << "\n";
    c.err << "Please commit your changes or stash them before you " << what << ".\nAborting\n";
    return false;
  }
  if (!in_way.empty()) {
    c.err << "error: The following untracked working tree files would be overwritten by " << what << ":\n";
    for (auto& d : in_way) c.err << "\t" << d << "\n";
    c.err << "Please move or remove them before you " << what << ".\nAborting\n";
    return false;
  }
  // write
  auto& h = c.host();
  std::map<std::string, GitIndexEntry> written;
  for (auto& p : touched) {
    if (conflict_text.count(p)) {
      const std::string& t = conflict_text[p];
      std::string full = join(c.top, p);
      (void)h.make_dir(dirname_of(full));
      auto r = h.write_file(full, ByteSpan((const uint8_t*)t.data(), t.size()));
      if (!r.ok()) die(c, r.error().message);
      continue;
    }
    auto w = want.find(p);
    if (w == want.end()) {
      (void)h.remove(join(c.top, p));
      for (std::string d = dirname_of(p); !d.empty(); d = dirname_of(d)) {
        auto names = h.list_dir(join(c.top, d));
        if (!names.ok()) break;
        bool empty = true;
        for (auto& n : *names) if (n != "." && n != "..") empty = false;
        if (!empty || !h.remove(join(c.top, d)).ok()) break;
      }
      continue;
    }
    auto e = c.repo->write_worktree_file(p, w->second.oid, w->second.mode);
    if (!e.ok()) die(c, e.error().message);
    written[p] = *e;
  }
  // index: untouched entries stay; touched take what was written; conflicts get stages
  std::vector<GitIndexEntry> next, unmerged;
  for (auto& e : c.repo->index()) if (!touched.count(e.path)) next.push_back(e);
  for (auto& p : touched) {
    if (conf.count(p)) continue;
    auto w = written.find(p);
    if (w != written.end()) next.push_back(w->second);
  }
  for (auto& [p, k] : conf) {
    auto stage = [&](const Snapshot& s, uint8_t n) {
      auto it = s.find(p);
      if (it == s.end()) return;
      GitIndexEntry e{p, it->second.oid, it->second.mode};
      e.stage = n;
      unmerged.push_back(e);
    };
    stage(base, 1);
    stage(ours, 2);
    stage(theirs, 3);
    // remove a stage-0 entry of the path (the touched loop skipped it)
    next.erase(std::remove_if(next.begin(), next.end(), [&](const GitIndexEntry& e) { return e.path == p; }), next.end());
  }
  c.repo->set_index(next);
  c.repo->set_unmerged(unmerged);
  if (auto w = c.repo->write_index(); !w.ok()) die(c, w.error().message);
  // messages
  std::set<std::string> mentioned(tm.auto_merged.begin(), tm.auto_merged.end());
  for (auto& [p, k] : conf) mentioned.insert(p);
  for (auto& p : mentioned) {
    auto k = conf.find(p);
    if (k == conf.end()) { out.messages.push_back("Auto-merging " + p); continue; }
    out.clean = false;
    out.conflicted.push_back(p);
    const std::string& kind = k->second->kind;
    if (kind == "content" || kind == "add/add") {
      if (k->second->file.binary) out.messages.push_back("warning: Cannot merge binary files: " + p + " (" + ours_label + " vs. " + theirs_label + ")");
      out.messages.push_back("Auto-merging " + p);
      out.messages.push_back("CONFLICT (" + kind + "): Merge conflict in " + p);
    } else if (kind == "binary") {
      out.messages.push_back("warning: Cannot merge binary files: " + p + " (" + ours_label + " vs. " + theirs_label + ")");
      out.messages.push_back("Auto-merging " + p);
      out.messages.push_back("CONFLICT (content): Merge conflict in " + p);
    } else if (kind == "modify/delete") {
      bool ours_has = ours.count(p) > 0;
      std::string del = ours_has ? theirs_label : ours_label, mod = ours_has ? ours_label : theirs_label;
      out.messages.push_back("CONFLICT (modify/delete): " + p + " deleted in " + del + " and modified in " + mod + ".  Version " + mod + " of " + p + " left in tree.");
    } else {
      out.messages.push_back("CONFLICT (" + kind + "): " + p);
    }
  }
  if (!tm.clean) out.clean = false;
  return true;
}

GitTreeMerge run_tree_merge(Ctx& c, const std::string& base, const std::string& ours, const std::string& theirs,
                            const std::string& ours_label, const std::string& theirs_label, const std::string& strategy_opt) {
  GitTreeMergeOptions mo;
  mo.ours_label = ours_label;
  mo.theirs_label = theirs_label;
  // merge.structural: a conflict the line merge finds is tried again on the files' syntax trees (not git's behaviour,
  // so off unless asked for; -X structural asks for it once)
  mo.structural = c.config.get_bool("merge.structural", false) || strategy_opt == "structural";
  if (strategy_opt == "structural") {
  } else if (strategy_opt == "ours") mo.favor = GitMergeFavor::Ours;
  else if (strategy_opt == "theirs") mo.favor = GitMergeFavor::Theirs;
  else if (!strategy_opt.empty()) unsupported(c, "the merge strategy option -X " + strategy_opt);
  auto r = git_merge_trees(*c.repo, base == empty_tree_oid() ? "" : base, ours, theirs, mo);
  if (!r.ok()) die(c, "merge failed: " + r.error().message);
  return *r;
}

namespace {

std::string merge_base_tree(Ctx& c, const std::string& a, const std::string& b, bool& unrelated) {
  auto bases = merge_bases(c, a, b);
  unrelated = bases.empty();
  if (bases.empty()) return "";
  if (bases.size() == 1) return read_commit(c, bases[0]).tree;
  // several bases (criss-cross): the library merges them into a virtual base itself
  return "";
}

}  // namespace

// ---------------------------------------------------------------------------
int cmd_branch(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{'d', "delete"}, {'D', ""}, {'f', "force"}, {'m', "move"}, {'M', ""}, {'c', "copy"},
                                         {'C', ""}, {'a', "all"}, {'r', "remotes"}, {'v', "verbose"}, {'q', "quiet"},
                                         {'l', "list"}, {0, "show-current"}, {'t', "track", Arg::Optional}, {0, "no-track"},
                                         {'u', "set-upstream-to", Arg::Required}, {0, "unset-upstream"}, {0, "contains", Arg::Optional},
                                         {0, "no-contains", Arg::Optional}, {0, "merged", Arg::Optional}, {0, "no-merged", Arg::Optional},
                                         {0, "format", Arg::Required}, {0, "sort", Arg::Required}, {0, "color", Arg::Optional},
                                         {0, "column", Arg::Optional}, {0, "abbrev", Arg::Optional}, {0, "points-at", Arg::Required},
                                         {'i', "ignore-case"}, {0, "create-reflog"}, {0, "edit-description"}, {0, "omit-empty"}});
  c.open_repo(false);
  if (p.has("copy") || p.has("C")) unsupported(c, "branch --copy");
  if (p.has("edit-description")) unsupported(c, "branch --edit-description");
  if (p.has("column") && p.get("column") != "never" && p.get("column") != "no") unsupported(c, "branch --column");
  bool quiet = p.has("quiet");
  if (p.has("show-current")) {
    std::string b = current_branch(c);
    if (!b.empty()) c.out << b << "\n";
    return 0;
  }
  if (p.has("unset-upstream")) {
    std::string b = p.pos.empty() ? current_branch(c) : p.pos[0];
    if (b.empty()) die(c, "could not unset upstream of HEAD when it does not point to any branch.");
    std::string err;
    edit_config_file(c.host(), repo_config_path(c), "branch." + b + ".remote", "", ConfigEdit::UnsetAll, &err);
    edit_config_file(c.host(), repo_config_path(c), "branch." + b + ".merge", "", ConfigEdit::UnsetAll, &err);
    return 0;
  }
  if (p.has("set-upstream-to")) {
    std::string up = p.get("set-upstream-to");
    std::string b = p.pos.empty() ? current_branch(c) : p.pos[0];
    if (b.empty()) die(c, "could not set upstream of HEAD to " + up + " when it does not point to any branch.");
    std::string full = dwim_ref(c, up);
    if (full.empty()) die(c, "the requested upstream branch '" + up + "' does not exist");
    set_tracking(c, b, full, quiet);
    return 0;
  }
  if (p.has("delete") || p.has("D")) {
    bool force = p.has("D") || p.has("force");
    if (p.pos.empty()) die(c, "branch name required");
    int rc = 0;
    for (auto& name : p.pos) {
      std::string ref = (p.has("remotes") ? "refs/remotes/" : "refs/heads/") + name;
      auto oid = read_ref(c, ref);
      if (!oid) {
        c.err << "error: " << (p.has("remotes") ? "remote-tracking branch '" : "branch '") << name << "' not found\n";
        rc = 1;
        continue;
      }
      if (!p.has("remotes") && name == current_branch(c)) {
        c.err << "error: cannot delete branch '" << name << "' used by worktree at '" << c.top << "'\n";
        rc = 1;
        continue;
      }
      if (!force && !p.has("remotes")) {
        std::string up = upstream_of(c, name);
        std::string against;
        if (!up.empty() && read_ref(c, up)) against = *read_ref(c, up);
        else if (auto h = read_ref(c, "HEAD")) against = *h;
        if (!against.empty() && !is_ancestor(c, *oid, against)) {
          // git 2.43's words: an error, then plain advice
          c.err << "error: the branch '" << name << "' is not fully merged.\n"
                   "If you are sure you want to delete it, run 'git branch -D " << name << "'\n";
          rc = 1;
          continue;
        }
      }
      auto r = c.repo->delete_ref(ref);
      if (!r.ok()) die(c, r.error().message);
      if (!p.has("remotes")) {
        remove_config_section(c.host(), repo_config_path(c), "branch." + name);
        (void)c.host().remove(c.repo->common_dir() + "/logs/" + ref);
      }
      if (!quiet) c.out << "Deleted " << (p.has("remotes") ? "remote-tracking branch " : "branch ") << name << " (was " << c.abbrev(*oid) << ").\n";
    }
    return rc;
  }
  if (p.has("move") || p.has("M")) {
    bool force = p.has("M") || p.has("force");
    std::string from, to;
    if (p.pos.size() == 1) { from = current_branch(c); to = p.pos[0]; }
    else if (p.pos.size() == 2) { from = p.pos[0]; to = p.pos[1]; }
    else usage(c, "branch name required");
    if (from.empty()) die(c, "cannot rename the current branch while not on any");
    if (!valid_branch_name(to)) die(c, "'" + to + "' is not a valid branch name");
    auto oid = read_ref(c, "refs/heads/" + from);
    bool unborn = !oid && from == current_branch(c);
    if (!oid && !unborn) die(c, "no branch named '" + from + "'");
    if (from != to && read_ref(c, "refs/heads/" + to) && !force) die(c, "a branch named '" + to + "' already exists");
    if (oid) {
      update_ref_checked(c, "refs/heads/" + to, *oid, std::nullopt, "Branch: renamed refs/heads/" + from + " to refs/heads/" + to);
      if (from != to) (void)c.repo->delete_ref("refs/heads/" + from);
    }
    if (from == current_branch(c)) (void)c.repo->set_head("refs/heads/" + to);
    if (from != to) rename_config_section(c.host(), repo_config_path(c), "branch." + from, "branch." + to);
    return 0;
  }
  take_optional_commit(p, {"contains", "no-contains", "merged", "no-merged"});
  bool listing = p.has("list") || p.pos.empty() || p.has("all") || p.has("remotes") || p.has("contains") || p.has("merged") ||
                 p.has("no-merged") || p.has("no-contains") || p.has("points-at") || p.has("format");
  if (!listing || (p.pos.size() >= 1 && !p.has("list") && !p.has("all") && !p.has("remotes") && !p.has("format"))) {
    // create
    std::string name = p.pos[0];
    std::string start = p.pos.size() > 1 ? p.pos[1] : "HEAD";
    if (!valid_branch_name(name)) die(c, "'" + name + "' is not a valid branch name");
    auto existing = read_ref(c, "refs/heads/" + name);
    if (existing && !p.has("force")) die(c, "a branch named '" + name + "' already exists");
    if (existing && name == current_branch(c)) die(c, "cannot force update the branch '" + name + "' used by worktree at '" + c.top + "'");
    auto oid = resolve(c, start);
    if (!oid) die(c, "not a valid object name: '" + start + "'");
    std::string cm = peel_to(c, *oid, "commit");
    if (cm.empty()) die(c, "not a valid branch point: '" + start + "'");
    update_ref_checked(c, "refs/heads/" + name, cm, std::nullopt, "branch: Created from " + start);
    std::string full = dwim_ref(c, start);
    bool track = p.has("track") || (!p.negated.count("track") && !p.has("no-track") && full.rfind("refs/remotes/", 0) == 0 &&
                                    c.config.get("branch.autosetupmerge").value_or("true") != "false");
    if (p.has("track") && full.empty()) die(c, "cannot set up tracking information; starting point '" + start + "' is not a branch");
    if (track && !full.empty() && (full.rfind("refs/remotes/", 0) == 0 || p.has("track"))) set_tracking(c, name, full, quiet);
    return 0;
  }
  // list
  c.color = want_color(c, p.has("color") ? std::optional<std::string>(p.get("color")) : std::nullopt, p.negated.count("color") > 0, "branch");
  std::vector<std::string> patterns = p.pos;
  const bool icase = p.has("ignore-case");
  auto head = read_ref(c, "HEAD");
  std::string cur = current_branch(c);
  const bool want_local = p.has("all") || !p.has("remotes"), want_remote = p.has("all") || p.has("remotes");
  auto name_matches = [&](const std::string& name) {
    if (patterns.empty()) return true;
    for (auto& pat : patterns) if (wildmatch(pat, name, icase, false)) return true;
    return false;
  };
  std::vector<RefRow> rows;
  bool detached = cur.empty() && head;
  if (detached && want_local && (patterns.empty() || name_matches("HEAD"))) rows.push_back({"HEAD", *head, true});
  for (auto& [ref, oid] : all_refs(c)) {
    bool local = ref.rfind("refs/heads/", 0) == 0, remote = ref.rfind("refs/remotes/", 0) == 0;
    if (!((local && want_local) || (remote && want_remote))) continue;
    if (!name_matches(short_name(ref))) continue;
    rows.push_back({ref, oid});
  }
  RefSelect sel;
  sel.contains = p.all("contains");
  sel.no_contains = p.all("no-contains");
  sel.merged = p.all("merged");
  sel.no_merged = p.all("no-merged");
  sel.points_at = p.all("points-at");
  select_refs(c, rows, sel);
  std::vector<std::string> sorts = p.all("sort");
  if (sorts.empty()) sorts = c.config.get_all("branch.sort");
  // the detached HEAD row stays first whatever the sort
  std::vector<RefRow> head_row;
  if (!rows.empty() && rows[0].detached) { head_row.push_back(rows[0]); rows.erase(rows.begin()); }
  sort_refs(c, rows, sorts, icase);
  rows.insert(rows.begin(), head_row.begin(), head_row.end());
  std::string format;
  if (p.has("format")) {
    format = p.get("format");
  } else {
    const int verbose = p.count("verbose");
    // `branch -a` names remote-tracking branches under remotes/
    const std::string remote_prefix = want_local && want_remote ? "remotes/" : "";
    size_t width = 0;
    if (verbose) {
      for (auto& row : rows) {
        size_t w;
        if (row.detached) w = head_description(c).size();
        else {
          std::string desc = row.ref;
          for (const char* pre : {"refs/heads/", "refs/remotes/"}) if (desc.rfind(pre, 0) == 0) { desc = desc.substr(std::strlen(pre)); break; }
          w = desc.size();
          if (row.ref.rfind("refs/remotes/", 0) == 0) w += remote_prefix.size();
        }
        width = std::max(width, w);
      }
    }
    std::string obname = p.negated.count("abbrev") || (p.has("abbrev") && p.get("abbrev") == "0") ? "%(objectname)"
                         : !p.has("abbrev") ? "%(objectname:short)" : "%(objectname:short=" + p.get("abbrev") + ")";
    // builtin/branch.c's build_format, its colors (color.branch.<slot> over git's) written into the format
    auto bcolor = [&](const char* slot, const char* def) -> std::string {
      if (!c.color) return "";
      if (auto v = c.config.get(std::string("color.branch.") + slot))
        if (auto col = parse_git_color(*v)) return *col;
      return def;
    };
    const std::string reset = bcolor("reset", "\033[m"), current = bcolor("current", "\033[32m"), local_c = bcolor("local", ""),
                      remote_c = bcolor("remote", "\033[31m"), upstream_c = bcolor("upstream", "\033[34m"),
                      worktree_c = bcolor("worktree", "\033[36m");
    std::string local = "%(if)%(HEAD)%(then)* " + current + "%(else)%(if)%(worktreepath)%(then)+ " + worktree_c + "%(else)  " +
                        local_c + "%(end)%(end)";
    std::string remote = "  " + remote_c;
    if (verbose) {
      local += "%(align:" + std::to_string(width) + ",left)%(refname:lstrip=2)%(end)" + reset + " " + obname + " ";
      if (verbose > 1)
        local += "%(if:notequals=*)%(HEAD)%(then)%(if)%(worktreepath)%(then)(" + worktree_c + "%(worktreepath)" + reset +
                 ") %(end)%(end)%(if)%(upstream)%(then)[" + upstream_c + "%(upstream:short)" + reset +
                 "%(if)%(upstream:track)%(then): %(upstream:track,nobracket)%(end)] %(end)%(contents:subject)";
      else
        local += "%(if)%(upstream:track)%(then)%(upstream:track) %(end)%(contents:subject)";
      remote += "%(align:" + std::to_string(width) + ",left)" + remote_prefix + "%(refname:lstrip=2)%(end)" + reset +
                "%(if)%(symref)%(then) -> %(symref:short)%(else) " + obname + " %(contents:subject)%(end)";
    } else {
      local += "%(refname:lstrip=2)" + reset + "%(if)%(symref)%(then) -> %(symref:short)%(end)";
      remote += remote_prefix + "%(refname:lstrip=2)" + reset + "%(if)%(symref)%(then) -> %(symref:short)%(end)";
    }
    format = "%(if:notequals=refs/remotes)%(refname:rstrip=-2)%(then)" + local + "%(else)" + remote + "%(end)";
  }
  RefFormat fmt(c, format);
  for (auto& row : rows) {
    std::string line = fmt.expand(row);
    if (p.has("omit-empty") && line.empty()) continue;
    c.out << line << "\n";
  }
  return 0;
}

// ---------------------------------------------------------------------------
// git's detach_advice, unless advice.detachedHead is off
void detached_advice(Ctx& c, const std::string& name) {
  if (!c.config.get_bool("advice.detachedhead", true)) return;
  c.err << "Note: switching to '" << name << "'.\n\n"
           "You are in 'detached HEAD' state. You can look around, make experimental\n"
           "changes and commit them, and you can discard any commits you make in this\n"
           "state without impacting any branches by switching back to a branch.\n\n"
           "If you want to create a new branch to retain commits you create, you may\n"
           "do so (now or later) by using -c with the switch command. Example:\n\n"
           "  git switch -c <new-branch-name>\n\n"
           "Or undo this operation with:\n\n"
           "  git switch -\n\n"
           "Turn off this advice by setting config variable advice.detachedHead to false\n\n";
}

int cmd_tag(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{'a', "annotate"}, {'m', "message", Arg::Required}, {'F', "file", Arg::Required},
                                         {'d', "delete"}, {'f', "force"}, {'l', "list"}, {'n', "", Arg::Optional},
                                         {0, "contains", Arg::Optional}, {0, "points-at", Arg::Optional}, {0, "sort", Arg::Required},
                                         {'s', "sign"}, {'u', "local-user", Arg::Required}, {'v', "verify"}, {0, "merged", Arg::Optional},
                                         {0, "no-merged", Arg::Optional}, {0, "no-contains", Arg::Optional}, {0, "format", Arg::Required},
                                         {'i', "ignore-case"},
                                         {0, "cleanup", Arg::Required}, {'e', "edit"}, {0, "create-reflog"}, {0, "color", Arg::Optional}});
  c.open_repo(false);
  // ref-filter's use_color: --color[=<when>], else color.ui
  c.color = want_color(c, p.has("color") ? std::optional<std::string>(p.get("color")) : std::nullopt, p.negated.count("color") > 0, "ui");
  if (p.has("sign") || p.has("local-user") || p.has("verify")) unsupported(c, "signed tags");
  if (p.has("delete")) {
    int rc = 0;
    for (auto& name : p.pos) {
      auto oid = read_ref(c, "refs/tags/" + name);
      if (!oid) { c.err << "error: tag '" << name << "' not found.\n"; rc = 1; continue; }
      (void)c.repo->delete_ref("refs/tags/" + name);
      c.out << "Deleted tag '" << name << "' (was " << c.abbrev(*oid) << ")\n";
    }
    return rc;
  }
  take_optional_commit(p, {"contains", "no-contains", "merged", "no-merged", "points-at"});
  bool list = p.has("list") || p.pos.empty() || p.has("contains") || p.has("no-contains") || p.has("points-at") || p.has("merged") ||
              p.has("no-merged") || p.has("n") || p.has("format");
  if (list) {
    const bool icase = p.has("ignore-case");
    std::vector<RefRow> rows;
    for (auto& [ref, oid] : all_refs(c)) {
      if (ref.rfind("refs/tags/", 0) != 0) continue;
      std::string nm = ref.substr(10);
      if (!p.pos.empty()) {
        bool any = false;
        for (auto& pat : p.pos) if (wildmatch(pat, nm, icase, false)) any = true;
        if (!any) continue;
      }
      rows.push_back({ref, oid});
    }
    RefSelect sel;
    sel.contains = p.all("contains");
    sel.no_contains = p.all("no-contains");
    sel.merged = p.all("merged");
    sel.no_merged = p.all("no-merged");
    sel.points_at = p.all("points-at");
    select_refs(c, rows, sel);
    std::vector<std::string> sorts = p.all("sort");
    if (sorts.empty()) sorts = c.config.get_all("tag.sort");
    sort_refs(c, rows, sorts, icase);
    std::string format = p.has("format") ? p.get("format") : "%(refname:strip=2)";
    if (p.has("n") && !p.has("format")) {
      std::string n = p.get("n").empty() ? "1" : p.get("n");
      format = "%(align:15)%(refname:strip=2)%(end) %(contents:lines=" + n + ")";
    }
    RefFormat fmt(c, format);
    for (auto& row : rows) c.out << fmt.expand(row) << "\n";
    return 0;
  }
  std::string name = p.pos[0];
  std::string target = p.pos.size() > 1 ? p.pos[1] : "HEAD";
  if (!Git::valid_ref_name("refs/tags/" + name)) die(c, "'" + name + "' is not a valid tag name.");
  auto existing = read_ref(c, "refs/tags/" + name);
  if (existing && !p.has("force")) die(c, "tag '" + name + "' already exists");
  std::string oid = must_resolve(c, target);
  bool annotated = p.has("annotate") || p.has("message") || p.has("file");
  std::string final_oid = oid;
  if (annotated) {
    std::string msg;
    for (auto& m : p.all("message")) { if (!msg.empty()) msg += "\n\n"; msg += m; }
    if (p.has("file")) {
      std::string f = p.get("file");
      if (f == "-") msg = read_all(c.in);
      else {
        auto r = c.host().read_file(c.abs_path(f));
        if (!r.ok()) die(c, "could not open or read '" + f + "'");
        msg.assign((const char*)r->data(), r->size());
      }
    }
    if (!p.has("message") && !p.has("file")) die(c, "no tag message given, and aios-git does not start an editor: use -m or -F");
    std::string cleanup = p.get("cleanup", "strip");
    if (cleanup == "strip") msg = cleanup_message(msg, true);
    else if (cleanup == "whitespace") msg = cleanup_message(msg, false);
    std::string body = "object " + oid + "\ntype " + type_of(c, oid) + "\ntag " + name + "\ntagger " + c.ident(true) + "\n\n" + msg;
    auto w = c.repo->write_object("tag", ByteSpan((const uint8_t*)body.data(), body.size()));
    if (!w.ok()) die(c, w.error().message);
    final_oid = *w;
  }
  update_ref_checked(c, "refs/tags/" + name, final_oid, std::nullopt);
  if (existing && *existing != final_oid) c.out << "Updated tag '" << name << "' (was " << c.abbrev(*existing) << ")\n";
  return 0;
}

// ---------------------------------------------------------------------------
namespace {
int checkout_impl(Ctx& c);
}

// The checkout of branches and paths, and the post-checkout hook after it.
int cmd_checkout(Ctx& c) {
  std::string before;
  if (c.try_open_repo()) before = read_ref(c, "HEAD").value_or("");
  const std::vector<std::string> args = c.args;
  int rc = checkout_impl(c);
  if (rc != 0 || !c.repo) return rc;
  // files (after "--", or a path that is no revision) are not a branch checkout
  size_t dd = std::find(args.begin(), args.end(), "--") - args.begin();
  std::vector<std::string> pos;
  bool makes_branch = false;
  for (size_t i = 1; i < std::min(dd, args.size()); ++i) {
    const std::string& a = args[i];
    if (a == "-b" || a == "-B" || a == "--detach" || a == "--orphan" || a == "-c" || a == "-C") makes_branch = true;
    if (!a.empty() && a[0] != '-') pos.push_back(a);
  }
  bool files = dd + 1 < args.size();
  if (!files && !makes_branch && !pos.empty()) files = pos.size() > 1 || !resolve(c, pos[0]).has_value();
  return run_post_checkout_hook(c, before, read_ref(c, "HEAD").value_or(""), !files);
}

namespace {
int checkout_impl(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{'b', "", Arg::Required}, {'B', "", Arg::Required}, {'f', "force"}, {'q', "quiet"},
                                         {0, "detach"}, {'t', "track", Arg::Optional}, {0, "no-track"}, {0, "ours"}, {0, "theirs"},
                                         {'m', "merge"}, {'p', "patch"}, {0, "orphan", Arg::Required}, {0, "guess"},
                                         {0, "overwrite-ignore"}, {0, "ignore-other-worktrees"}, {0, "progress"},
                                         {0, "conflict", Arg::Required}, {0, "recurse-submodules", Arg::Optional},
                                         {0, "pathspec-from-file", Arg::Required}, {'l', ""}, {0, "overlay"}});
  c.open_repo();
  if (p.has("merge") || p.has("patch") || p.has("conflict") || p.has("pathspec-from-file")) unsupported(c, "checkout -m/-p/--conflict/--pathspec-from-file");
  bool force = p.has("force"), quiet = p.has("quiet");
  if (p.has("orphan")) {
    std::string name = p.get("orphan");
    if (!valid_branch_name(name)) die(c, "'" + name + "' is not a valid branch name");
    auto s = c.repo->set_head("refs/heads/" + name);
    if (!s.ok()) die(c, s.error().message);
    if (!quiet) c.err << "Switched to a new branch '" << name << "'\n";
    return 0;
  }
  std::vector<std::string> pos = p.pos;
  std::vector<std::string> paths = p.after_dashdash;
  std::string new_branch = p.has("b") ? p.get("b") : p.get("B");
  bool reset_branch = p.has("B");
  // checkout [<tree-ish>] -- <paths>
  if (p.dashdash) {
    if (pos.size() > 1) usage(c, "only one reference expected");
    if (!new_branch.empty() && !paths.empty()) die(c, "'-b' cannot be used with updating paths");
    if (!paths.empty()) return checkout_paths(c, pos.empty() ? "" : pos[0], paths, p.has("ours"), p.has("theirs"));
  }
  std::string target = pos.empty() ? "" : pos[0];
  if (target == "-") {
    target = previous_branch(c);
    if (target.empty()) die(c, "no previous branch");
  }
  if (!target.empty() && new_branch.empty() && !p.has("detach")) {
    bool is_rev = resolve(c, target).has_value();
    bool is_branch = read_ref(c, "refs/heads/" + target).has_value();
    bool remote_guess = !is_branch && !is_rev && !guess_remote_branch(c, target).empty();
    if (!is_rev && !remote_guess) {
      // not a revision: paths from the index
      if (pos.size() >= 1) return checkout_paths(c, "", pos, p.has("ours"), p.has("theirs"), !quiet);
    }
    if (pos.size() > 1) {
      // checkout <rev> <paths...> without --
      std::vector<std::string> rest(pos.begin() + 1, pos.end());
      return checkout_paths(c, target, rest, false, false, !quiet);
    }
    if (remote_guess) {
      std::string rref = guess_remote_branch(c, target);
      std::string oid = *read_ref(c, rref);
      update_ref_checked(c, "refs/heads/" + target, oid, std::nullopt, "branch: Created from " + short_name(rref));
      if (!p.has("no-track")) set_tracking(c, target, rref, quiet);
      return move_to(c, target, oid, force, quiet, target, NewBranch::Created);
    }
    if (is_branch) {
      std::string oid = *read_ref(c, "refs/heads/" + target);
      if (target == current_branch(c)) {
        auto r = c.repo->checkout_tree(read_commit(c, oid).tree, force);
        if (!r.ok()) { c.err << "error: " << r.error().message << "\nAborting\n"; return 1; }
        if (!quiet) { print_local_changes(c); c.err << "Already on '" << target << "'\n"; tracking_info(c); }
        return 0;
      }
      return move_to(c, target, oid, force, quiet, target);
    }
    std::string oid = commit_of(c, target);
    return move_to(c, "", oid, force, quiet, target);
  }
  if (p.has("detach")) {
    std::string oid = commit_of(c, target.empty() ? "HEAD" : target);
    return move_to(c, "", oid, force, quiet, target.empty() ? "HEAD" : target, NewBranch::No, true);
  }
  if (!new_branch.empty()) {
    if (!valid_branch_name(new_branch)) die(c, "'" + new_branch + "' is not a valid branch name");
    if (read_ref(c, "refs/heads/" + new_branch) && !reset_branch) die(c, "a branch named '" + new_branch + "' already exists");
    bool existed_before = read_ref(c, "refs/heads/" + new_branch).has_value();
    std::string start = target.empty() ? "HEAD" : target;
    auto head = read_ref(c, "HEAD");
    if (start == "HEAD" && !head) {
      // a new branch on an unborn one: just rename where HEAD points
      auto s = c.repo->set_head("refs/heads/" + new_branch);
      if (!s.ok()) die(c, s.error().message);
      if (!quiet) c.err << "Switched to a new branch '" << new_branch << "'\n";
      return 0;
    }
    std::string oid = commit_of(c, start);
    // check the move first, then create the branch
    std::string tree = read_commit(c, oid).tree;
    if (head && read_commit(c, *head).tree != tree) {
      auto r = c.repo->checkout_tree(tree, force);
      if (!r.ok()) { c.err << "error: " << r.error().message << "\nAborting\n"; return 1; }
    }
    update_ref_checked(c, "refs/heads/" + new_branch, oid, std::nullopt, "branch: Created from " + start);
    std::string full = dwim_ref(c, start);
    bool track = p.has("track") || (!p.has("no-track") && full.rfind("refs/remotes/", 0) == 0 &&
                                    c.config.get("branch.autosetupmerge").value_or("true") != "false");
    if (track && !full.empty()) set_tracking(c, new_branch, full, quiet);
    if (p.has("track") && full.empty()) die(c, "cannot set up tracking information; starting point '" + start + "' is not a branch");
    return move_to(c, new_branch, oid, force, quiet, new_branch, existed_before ? NewBranch::Reset : NewBranch::Created);
  }
  if (target.empty()) {
    // "git checkout" alone: report local changes and tracking
    print_local_changes(c);
    tracking_info(c);
    return 0;
  }
  return 0;
}
}  // namespace

int cmd_switch(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{'c', "create", Arg::Required}, {'C', "force-create", Arg::Required}, {'d', "detach"},
                                         {'f', "force"}, {0, "discard-changes"}, {'q', "quiet"}, {'t', "track", Arg::Optional},
                                         {0, "no-track"}, {0, "guess"}, {0, "orphan", Arg::Required}, {'m', "merge"},
                                         {0, "conflict", Arg::Required}, {0, "progress"}, {0, "ignore-other-worktrees"},
                                         {0, "recurse-submodules", Arg::Optional}});
  c.open_repo();
  if (p.has("merge") || p.has("conflict")) unsupported(c, "switch -m/--conflict");
  bool force = p.has("force") || p.has("discard-changes"), quiet = p.has("quiet");
  std::vector<std::string> args{"checkout"};
  if (force) args.push_back("-f");
  if (quiet) args.push_back("-q");
  if (p.has("no-track")) args.push_back("--no-track");
  if (p.has("track")) args.push_back("--track");
  if (p.has("orphan")) {
    c.args = {"checkout", "--orphan", p.get("orphan")};
    int rc = cmd_checkout(c);
    if (rc == 0) {
      // switch --orphan also empties the index and the tracked files
      for (auto& e : c.repo->index()) (void)c.host().remove(join(c.top, e.path));
      c.repo->set_index({});
      (void)c.repo->write_index();
    }
    return rc;
  }
  if (p.has("create") || p.has("force-create")) {
    args.push_back(p.has("create") ? "-b" : "-B");
    args.push_back(p.has("create") ? p.get("create") : p.get("force-create"));
    if (!p.pos.empty()) args.push_back(p.pos[0]);
    c.args = args;
    return cmd_checkout(c);
  }
  if (p.pos.empty()) die(c, "missing branch or commit argument");
  std::string target = p.pos[0];
  if (p.has("detach")) {
    args.push_back("--detach");
    args.push_back(target);
    c.args = args;
    return cmd_checkout(c);
  }
  if (target != "-" && !read_ref(c, "refs/heads/" + target)) {
    if (!guess_remote_branch(c, target).empty() && !p.negated.count("guess")) {
      args.push_back(target);
      c.args = args;
      return cmd_checkout(c);
    }
    if (resolve(c, target))
      die(c, "a branch is expected, got " + std::string(read_ref(c, "refs/remotes/" + target) ? "remote branch '" : "commit '") + target + "'\n"
             "hint: If you want to detach HEAD at the commit, try again with the --detach option.");
    die(c, "invalid reference: " + target);
  }
  args.push_back(target);
  c.args = args;
  return cmd_checkout(c);
}

// ---------------------------------------------------------------------------
int cmd_merge(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{0, "no-edit"}, {'e', "edit"}, {'m', "message", Arg::Required}, {'F', "file", Arg::Required},
                                         {0, "ff"}, {0, "no-ff"}, {0, "ff-only"}, {0, "squash"}, {0, "commit"}, {0, "no-commit"},
                                         {0, "abort"}, {0, "continue"}, {0, "quit"}, {'q', "quiet"}, {'v', "verbose"},
                                         {'X', "strategy-option", Arg::Required}, {'s', "strategy", Arg::Required},
                                         {0, "allow-unrelated-histories"}, {0, "stat"}, {'n', ""}, {0, "summary"}, {0, "log", Arg::Optional},
                                         {0, "verify"}, {0, "no-verify"}, {0, "progress"}, {0, "autostash"}, {0, "signoff"},
                                         {'S', "gpg-sign", Arg::Optional}, {0, "verify-signatures"}, {0, "rerere-autoupdate"},
                                         {0, "cleanup", Arg::Required}, {0, "into-name", Arg::Required}, {0, "overwrite-ignore"}});
  c.open_repo();
  if (!c.hasenv("GIT_REFLOG_ACTION")) {
    c.env["GIT_REFLOG_ACTION"] = "merge";
    for (auto& a : p.pos) c.env["GIT_REFLOG_ACTION"] += " " + a;
  }
  take_sign_option(c, p);
  if (p.has("log") || p.has("autostash") || p.has("verify-signatures") || p.has("signoff") || p.has("into-name"))
    unsupported(c, "merge --log/--autostash/--signoff/--into-name/--verify-signatures");
  if (p.has("strategy")) {
    std::string s = p.get("strategy");
    if (s != "ort" && s != "recursive") unsupported(c, "the merge strategy '" + s + "'");
  }
  bool quiet = p.has("quiet");
  if (p.has("abort")) {
    if (!c.read_git_file("MERGE_HEAD")) die(c, "There is no merge to abort (MERGE_HEAD missing).");
    c.args = {"reset", "--merge"};
    return cmd_reset(c);
  }
  if (p.has("quit")) {
    for (const char* f : {"MERGE_HEAD", "MERGE_MSG", "MERGE_MODE", "AUTO_MERGE"}) c.remove_git_file(f);
    return 0;
  }
  if (p.has("continue")) {
    if (!c.read_git_file("MERGE_HEAD")) die(c, "There is no merge in progress (MERGE_HEAD missing).");
    c.args = {"commit", "--no-edit"};
    return cmd_commit(c);
  }
  if (!c.repo->unmerged().empty()) {
    c.err << "error: Merging is not possible because you have unmerged files.\n"
             "hint: Fix them up in the work tree, and then use 'git add/rm <file>'\n"
             "hint: as appropriate to mark resolution and make a commit.\n"
             "fatal: Exiting because of an unresolved conflict.\n";
    return 128;
  }
  if (c.read_git_file("MERGE_HEAD")) {
    c.err << "fatal: You have not concluded your merge (MERGE_HEAD exists).\nPlease, commit your changes before you merge.\n";
    return 128;
  }
  std::vector<std::string> targets = p.pos;
  if (targets.empty()) {
    std::string b = current_branch(c);
    std::string up = b.empty() ? "" : upstream_of(c, b);
    if (up.empty()) die(c, "No remote for the current branch.");
    targets.push_back(up);
  }
  if (targets.size() > 1) unsupported(c, "octopus merges (more than one branch at once)");
  std::string name = targets[0];
  std::string theirs = commit_of(c, name);
  auto head = read_ref(c, "HEAD");
  // describe what is merged, as fmt-merge-msg does
  std::string desc;
  std::string full = dwim_ref(c, name);
  if (name == "FETCH_HEAD") {
    auto fh = c.read_git_file("FETCH_HEAD");
    std::string line = fh ? fh->substr(0, fh->find('\n')) : "";
    size_t tab2 = line.find('\t', 41);
    std::string what = tab2 == std::string::npos ? "" : line.substr(tab2 + 1);
    desc = what.empty() ? "commit '" + theirs.substr(0, 40) + "'" : what;
  } else if (full.rfind("refs/heads/", 0) == 0) desc = "branch '" + full.substr(11) + "'";
  else if (full.rfind("refs/remotes/", 0) == 0) desc = "remote-tracking branch '" + full.substr(13) + "'";
  else if (full.rfind("refs/tags/", 0) == 0) desc = "tag '" + full.substr(10) + "'";
  else desc = "commit '" + name + "'";
  std::string msg = "Merge " + desc;
  std::string cur = current_branch(c);
  if (!cur.empty()) {
    std::vector<std::string> suppress = c.config.get_all("merge.suppressdest");
    bool suppressed = suppress.empty() ? (cur == "main" || cur == "master") : false;
    for (auto& s : suppress) if (s.empty() ? false : git_wildmatch(s, cur)) suppressed = true;
    if (!suppressed) msg += " into " + cur;
  }
  if (p.has("message")) {
    msg.clear();
    for (auto& m : p.all("message")) { if (!msg.empty()) msg += "\n\n"; msg += m; }
  }
  if (p.has("file")) {
    auto r = c.host().read_file(c.abs_path(p.get("file")));
    if (!r.ok()) die(c, "could not read file '" + p.get("file") + "'");
    msg.assign((const char*)r->data(), r->size());
  }
  msg = cleanup_message(msg, false);
  if (!head) {
    // merging into an unborn branch: take theirs
    auto r = c.repo->checkout_tree(read_commit(c, theirs).tree, false);
    if (!r.ok()) { c.err << "error: " << r.error().message << "\nAborting\n"; return 1; }
    update_ref_checked(c, "HEAD", theirs, std::nullopt, "initial pull");
    return 0;
  }
  c.write_git_file("ORIG_HEAD", *head + "\n");
  if (*head == theirs || is_ancestor(c, theirs, *head)) {
    if (!quiet) c.out << "Already up to date.\n";
    return 0;
  }
  bool ff_ok = is_ancestor(c, *head, theirs);
  std::string ff_mode = p.has("ff-only") ? "only" : p.has("no-ff") ? "no" : p.has("ff") ? "yes" : c.config.get("merge.ff").value_or("true");
  if (ff_mode == "only" && !ff_ok) {
    c.err << "hint: Diverging branches can't be fast-forwarded, you need to either:\n"
             "hint: \n"
             "hint: \tgit merge --no-ff\n"
             "hint: \n"
             "hint: or:\n"
             "hint: \n"
             "hint: \tgit rebase\n"
             "hint: \n"
             "hint: Disable this message with \"git config advice.diverging false\"\n";
    die(c, "Not possible to fast-forward, aborting.");
  }
  bool stat = !p.has("n") && !p.negated.count("stat") && !quiet && c.config.get_bool("merge.stat", true);
  if (ff_ok && ff_mode != "no" && ff_mode != "false" && !p.has("squash")) {
    std::string from_tree = read_commit(c, *head).tree, to_tree = read_commit(c, theirs).tree;
    if (!quiet) c.out << "Updating " << c.abbrev(*head) << ".." << c.abbrev(theirs) << "\n";
    auto r = c.repo->checkout_tree(to_tree, false);
    if (!r.ok()) {
      std::string m = r.error().message;
      size_t at = m.find("by checkout");
      if (at != std::string::npos) m.replace(at, 11, "by merge");
      size_t sw = m.find("before you switch branches");
      if (sw != std::string::npos) m.replace(sw, 26, "before you merge");
      c.err << "error: " << m << "\nAborting\n";
      return 1;
    }
    if (!quiet) c.out << "Fast-forward\n";
    update_ref_checked(c, "HEAD", theirs, std::nullopt, "merge " + name + ": Fast-forward");
    if (stat) {
      DiffOptions o;
      o.format = DiffOptions::Stat;
      o.extra = {DiffOptions::Summary};
      auto pairs = diff_trees(c, from_tree, to_tree, Pathspec{}, o);
      print_diff(c, pairs, o, c.out);
    }
    run_post_merge_hook(c, false);
    return 0;
  }
  bool unrelated = false;
  auto bases = merge_bases(c, *head, theirs);
  unrelated = bases.empty();
  if (unrelated && !p.has("allow-unrelated-histories")) die(c, "refusing to merge unrelated histories");
  std::string ours_tree = read_commit(c, *head).tree, theirs_tree = read_commit(c, theirs).tree;
  std::string base_tree;
  GitTreeMerge tm;
  if (bases.size() > 1) {
    // criss-cross: let the library build the virtual base (git_merge_commits does), then use its tree merge
    GitTreeMergeOptions mo;
    mo.ours_label = "HEAD";
    mo.theirs_label = name;
    std::string xo = p.get("strategy-option");
    mo.structural = c.config.get_bool("merge.structural", false) || xo == "structural";
    if (xo == "ours") mo.favor = GitMergeFavor::Ours;
    else if (xo == "theirs") mo.favor = GitMergeFavor::Theirs;
    auto cm = git_merge_commits(*c.repo, *head, theirs, msg, false, mo);
    if (!cm.ok()) die(c, "merge failed: " + cm.error().message);
    tm = cm->tree;
    base_tree = cm->base.empty() ? "" : read_commit(c, cm->base).tree;
  } else {
    base_tree = unrelated ? "" : read_commit(c, bases[0]).tree;
    tm = run_tree_merge(c, base_tree, ours_tree, theirs_tree, "HEAD", name, p.get("strategy-option"));
  }
  MergeApply ap;
  if (!apply_tree_merge(c, base_tree, ours_tree, theirs_tree, tm, "HEAD", name, ap)) {
    c.err << "Merge with strategy ort failed.\n";
    return 2;
  }
  for (auto& m : ap.messages) c.out << m << "\n";
  if (p.has("squash")) {
    c.write_git_file("SQUASH_MSG", "Squashed commit of the following:\n");
    if (!ap.clean) { c.out << "Squash commit -- not updating HEAD\nAutomatic merge failed; fix conflicts and then commit the result.\n"; return 1; }
    c.out << "Squash commit -- not updating HEAD\n";
    run_post_merge_hook(c, true);
    return 0;
  }
  if (!ap.clean) {
    c.write_git_file("MERGE_HEAD", theirs + "\n");
    std::string mm = msg;
    mm += "\n# Conflicts:\n";
    for (auto& f : ap.conflicted) mm += "#\t" + f + "\n";
    c.write_git_file("MERGE_MSG", mm);
    c.write_git_file("MERGE_MODE", ff_mode == "no" ? "no-ff" : "");
    c.out << "Automatic merge failed; fix conflicts and then commit the result.\n";
    return 1;
  }
  if (p.has("no-commit")) {
    c.write_git_file("MERGE_HEAD", theirs + "\n");
    c.write_git_file("MERGE_MSG", msg);
    c.write_git_file("MERGE_MODE", ff_mode == "no" ? "no-ff" : "");
    c.out << "Automatic merge went well; stopped before committing as requested\n";
    return 0;
  }
  if (run_pre_commit_hook(c, "pre-merge-commit", p.has("no-verify")) != 0) return 1;
  {
    MessageHooks mh = run_message_hooks(c, "MERGE_MSG", msg, "merge", "", p.has("no-verify"));
    if (mh.code != 0) return 1;
    if (mh.message != msg) msg = cleanup_message(mh.message, false);
  }
  if (!p.has("no-verify") && !find_hook(c, "pre-merge-commit").empty()) {
    c.repo.reset();
    c.open_repo();
  }
  std::string tree = write_tree_of(c, c.repo->index());
  std::string oid = make_commit(c, tree, {*head, theirs}, msg, {}, {}, true);
  update_ref_checked(c, "HEAD", oid, std::nullopt, "merge " + name + ": Merge made by the 'ort' strategy.");
  if (!quiet) {
    c.out << "Merge made by the 'ort' strategy.\n";
    if (stat) {
      DiffOptions o;
      o.format = DiffOptions::Stat;
      o.extra = {DiffOptions::Summary};
      auto pairs = diff_trees(c, ours_tree, tree, Pathspec{}, o);
      print_diff(c, pairs, o, c.out);
    }
  }
  c.remove_git_file("MERGE_MSG");
  run_post_merge_hook(c, false);
  return 0;
}

// ---------------------------------------------------------------------------
namespace {
int pick(Ctx& c, bool revert, const std::vector<std::string>& args_in) {
  auto p = parse_options(c, args_in, 1, {{'n', "no-commit"}, {'e', "edit"}, {0, "no-edit"}, {'x', ""}, {'m', "mainline", Arg::Required},
                                          {0, "abort"}, {0, "continue"}, {0, "skip"}, {0, "quit"}, {0, "allow-empty"},
                                          {0, "allow-empty-message"}, {0, "keep-redundant-commits"}, {'s', "signoff"},
                                          {0, "ff"}, {'X', "strategy-option", Arg::Required}, {0, "strategy", Arg::Required},
                                          {'S', "gpg-sign", Arg::Optional}, {0, "empty", Arg::Required}, {0, "rerere-autoupdate"}});
  c.open_repo();
  const char* head_file = revert ? "REVERT_HEAD" : "CHERRY_PICK_HEAD";
  const char* cmdname = revert ? "revert" : "cherry-pick";
  take_sign_option(c, p);
  if (p.has("signoff") || p.has("skip") || p.has("empty") || p.has("ff")) unsupported(c, std::string(cmdname) + " --skip/--signoff/--ff/--empty");
  if (p.has("abort")) {
    if (!c.read_git_file(head_file)) die(c, std::string("no ") + cmdname + " in progress");
    c.args = {"reset", "--merge"};
    int rc = cmd_reset(c);
    c.remove_git_file(head_file);
    return rc;
  }
  if (p.has("quit")) { c.remove_git_file(head_file); c.remove_git_file("MERGE_MSG"); return 0; }
  if (p.has("continue")) {
    if (!c.read_git_file(head_file)) die(c, std::string("no ") + cmdname + " in progress");
    if (!c.repo->unmerged().empty()) {
      c.err << "error: Committing is not possible because you have unmerged files.\n";
      return 1;
    }
    std::string picked = c.read_git_file(head_file)->substr(0, 40);
    auto msg = c.read_git_file("MERGE_MSG");
    auto head = read_ref(c, "HEAD");
    GitCommit orig = read_commit(c, picked);
    std::string tree = write_tree_of(c, c.repo->index());
    std::string author = revert ? "" : orig.author + " " + std::to_string(orig.author_time) + " " + orig.author_tz;
    std::string oid = make_commit(c, tree, {*head}, cleanup_message(msg.value_or(orig.message), true), author, {}, true);
    update_ref_checked(c, "HEAD", oid, std::nullopt, std::string(cmdname) + ": " + subject_of(msg.value_or(orig.message)));
    c.remove_git_file(head_file);
    c.remove_git_file("MERGE_MSG");
    std::string b = current_branch(c);
    c.out << "[" << (b.empty() ? "detached HEAD" : b) << " " << c.abbrev(oid) << "] " << subject_of(msg.value_or(orig.message)) << "\n";
    print_commit_summary(c, read_commit(c, *head).tree, tree, c.out);
    return 0;
  }
  if (c.read_git_file(head_file)) die(c, std::string(cmdname) + " is already in progress\nhint: try \"git " + cmdname + " (--continue | --abort | --quit)\"");
  if (p.pos.empty()) usage(c, std::string("usage: git ") + cmdname + " [<options>] <commit-ish>...");
  // the commits, oldest first (a range A..B walks in order)
  std::vector<std::string> commits;
  for (auto& a : p.pos) {
    if (a.find("..") != std::string::npos) {
      WalkOptions w;
      parse_revisions(c, {a}, w, false);
      w.reverse = !revert;
      for (auto& cm : walk(c, w)) commits.push_back(cm.oid);
    } else {
      commits.push_back(commit_of(c, a));
    }
  }
  int mainline = p.has("mainline") ? std::atoi(p.get("mainline").c_str()) : 0;
  for (auto& oid : commits) {
    auto head = read_ref(c, "HEAD");
    if (!head) die(c, "your current branch does not have any commits yet");
    GitCommit cm = read_commit(c, oid);
    std::string parent;
    if (cm.parents.size() > 1) {
      if (!mainline) die(c, "commit " + oid + " is a merge but no -m option was given.");
      if ((size_t)mainline > cm.parents.size()) die(c, "commit " + oid + " does not have parent " + std::to_string(mainline));
      parent = cm.parents[(size_t)mainline - 1];
    } else {
      if (mainline) die(c, "mainline was specified but commit " + oid + " is not a merge.");
      if (!cm.parents.empty()) parent = cm.parents[0];
    }
    std::string parent_tree = parent.empty() ? "" : read_commit(c, parent).tree;
    std::string ours_tree = read_commit(c, *head).tree;
    std::string subj = subject_of(cm.message);
    std::string short_id = c.abbrev(oid);
    std::string base = revert ? cm.tree : parent_tree, theirs = revert ? parent_tree : cm.tree;
    std::string theirs_label = revert ? "parent of " + short_id + " (" + subj + ")" : short_id + " (" + subj + ")";
    GitTreeMerge tm = run_tree_merge(c, base, ours_tree, theirs.empty() ? empty_tree_oid() : theirs, "HEAD", theirs_label, p.get("strategy-option"));
    std::string msg;
    if (revert) {
      msg = "Revert \"" + subj + "\"\n\nThis reverts commit " + oid + ".\n";
      if (cm.parents.size() > 1) msg = "Revert \"" + subj + "\"\n\nThis reverts commit " + oid + ", reversing\nchanges made to " + parent + ".\n";
    } else {
      msg = cm.message;
      if (p.has("x")) {
        if (!msg.empty() && msg.back() != '\n') msg += "\n";
        msg += "\n(cherry picked from commit " + oid + ")\n";
      }
    }
    MergeApply ap;
    if (!apply_tree_merge(c, base, ours_tree, theirs, tm, "HEAD", theirs_label, ap, revert ? "revert" : "cherry-pick")) {
      c.err << "fatal: " << cmdname << " failed\n";
      return 128;
    }
    for (auto& m : ap.messages) c.out << m << "\n";
    if (!ap.clean) {
      c.write_git_file(head_file, oid + "\n");
      std::string mm = msg + "\n# Conflicts:\n";
      for (auto& f : ap.conflicted) mm += "#\t" + f + "\n";
      c.write_git_file("MERGE_MSG", mm);
      c.err << "error: could not " << (revert ? "revert " : "apply ") << short_id << "... " << subj << "\n"
            << "hint: After resolving the conflicts, mark them with\n"
               "hint: \"git add/rm <pathspec>\", then run\n"
               "hint: \"git " << cmdname << " --continue\".\n"
               "hint: You can instead skip this commit with \"git " << cmdname << " --skip\".\n"
               "hint: To abort and get back to the state before \"git " << cmdname << "\",\n"
               "hint: run \"git " << cmdname << " --abort\".\n";
      return 1;
    }
    if (p.has("no-commit")) continue;
    std::string tree = write_tree_of(c, c.repo->index());
    if (tree == ours_tree && !p.has("allow-empty") && !p.has("keep-redundant-commits")) {
      c.err << "On branch " << current_branch(c) << "\nYou are currently " << (revert ? "reverting" : "cherry-picking") << " commit " << short_id << ".\n\n"
            << "nothing to commit, working tree clean\n"
            << "The previous cherry-pick is now empty, possibly due to conflict resolution.\n"
               "If you wish to commit it anyway, use:\n\n    git commit --allow-empty\n\n"
               "Otherwise, please use 'git cherry-pick --skip'\n";
      c.write_git_file(head_file, oid + "\n");
      return 1;
    }
    std::string author = revert ? "" : cm.author + " " + std::to_string(cm.author_time) + " " + cm.author_tz;
    {
      MessageHooks mh = run_message_hooks(c, "COMMIT_EDITMSG", msg, "message", "", true);
      if (mh.code != 0) return 1;
      msg = mh.message;
    }
    std::string new_oid = make_commit(c, tree, {*head}, msg, author, {}, true);
    update_ref_checked(c, "HEAD", new_oid, std::nullopt, std::string(cmdname) + ": " + subject_of(msg));
    run_post_commit_hooks(c, false, "", new_oid);
    std::string b = current_branch(c);
    (void)b;
    print_new_commit(c, new_oid, true, false);
  }
  return 0;
}
}  // namespace

int cmd_cherry_pick(Ctx& c) { return pick(c, false, c.args); }
int cmd_revert(Ctx& c) { return pick(c, true, c.args); }

// ---------------------------------------------------------------------------
namespace {
struct StashEntry { std::string old_oid, oid, message; };
std::vector<StashEntry> stash_list(Ctx& c) {
  std::vector<StashEntry> out;
  auto r = c.host().read_file(c.repo->common_dir() + "/logs/refs/stash");
  if (!r.ok()) {
    if (auto s = read_ref(c, "refs/stash")) out.push_back({std::string(40, '0'), *s, read_commit(c, *s).message});
    return out;
  }
  std::string text((const char*)r->data(), r->size());
  std::istringstream ss(text);
  std::string line;
  while (std::getline(ss, line)) {
    if (line.size() < 82) continue;
    size_t tab = line.find('\t');
    out.push_back({line.substr(0, 40), line.substr(41, 40), tab == std::string::npos ? "" : line.substr(tab + 1)});
  }
  std::reverse(out.begin(), out.end());
  return out;
}
void stash_write_list(Ctx& c, const std::vector<StashEntry>& newest_first) {
  std::string path = c.repo->common_dir() + "/logs/refs/stash";
  if (newest_first.empty()) {
    (void)c.host().remove(path);
    (void)c.repo->delete_ref("refs/stash");
    return;
  }
  std::string text;
  std::string who = c.ident(true);
  for (size_t i = newest_first.size(); i-- > 0;) {
    auto& e = newest_first[i];
    std::string prev = i + 1 < newest_first.size() ? newest_first[i + 1].oid : std::string(40, '0');
    text += prev + " " + e.oid + " " + who + "\t" + e.message + "\n";
  }
  (void)c.host().make_dir(dirname_of(path));
  c.host().write_file(path, ByteSpan((const uint8_t*)text.data(), text.size()));
  auto r = c.repo->update_ref("refs/stash", newest_first[0].oid);
  if (!r.ok()) die(c, r.error().message);
}
size_t stash_index(Ctx& c, const std::string& spec) {
  if (spec.empty()) return 0;
  std::string s = spec;
  if (s.rfind("stash@{", 0) == 0 && s.back() == '}') return (size_t)std::atol(s.substr(7, s.size() - 8).c_str());
  bool digits = !s.empty();
  for (char ch : s) if (!std::isdigit((unsigned char)ch)) digits = false;
  if (digits) return (size_t)std::atol(s.c_str());
  die(c, "'" + spec + "' is not a stash-like commit");
}
}  // namespace

int cmd_stash(Ctx& c) {
  std::vector<std::string> args(c.args.begin() + 1, c.args.end());
  std::string sub = "push";
  if (!args.empty() && args[0][0] != '-') { sub = args[0]; args.erase(args.begin()); }
  std::vector<std::string> pargs{"stash"};
  pargs.insert(pargs.end(), args.begin(), args.end());
  c.open_repo();
  if (sub == "list") {
    std::string fmt;
    for (auto& a : args) if (!reflog_user_format(a).empty()) fmt = reflog_user_format(a);
    auto l = stash_list(c);
    for (size_t i = 0; i < l.size(); ++i) {
      std::string gd = "stash@{" + std::to_string(i) + "}";
      if (fmt.empty()) c.out << gd << ": " << l[i].message << "\n";
      else c.out << format_reflog_entry(c, fmt, l[i].oid, gd, l[i].message) << "\n";
    }
    return 0;
  }
  if (sub == "clear") { stash_write_list(c, {}); return 0; }
  if (sub == "drop") {
    auto p = parse_options(c, pargs, 1, {{'q', "quiet"}});
    auto l = stash_list(c);
    size_t i = stash_index(c, p.pos.empty() ? "" : p.pos[0]);
    if (i >= l.size()) die(c, p.pos.empty() ? "No stash entries found." : "stash@{" + std::to_string(i) + "} is not a valid reference");
    std::string oid = l[i].oid;
    l.erase(l.begin() + (long)i);
    stash_write_list(c, l);
    if (!p.has("quiet")) c.out << "Dropped " << (p.pos.empty() ? "refs/stash@{0}" : "stash@{" + std::to_string(i) + "}") << " (" << oid << ")\n";
    return 0;
  }
  if (sub == "show") {
    std::vector<std::string> rest(args.begin(), args.end());
    DiffOptions o;
    bool any = false;
    rest = take_diff_options(c, rest, o, &any);
    if (!any) { o.format = DiffOptions::Stat; }
    auto l = stash_list(c);
    size_t i = stash_index(c, rest.empty() ? "" : rest[0]);
    if (i >= l.size()) die(c, "No stash entries found.");
    GitCommit w = read_commit(c, l[i].oid);
    auto pairs = diff_trees(c, read_commit(c, w.parents[0]).tree, w.tree, Pathspec{}, o);
    print_diff(c, pairs, o, c.out);
    return 0;
  }
  if (sub == "apply" || sub == "pop") {
    auto p = parse_options(c, pargs, 1, {{'q', "quiet"}, {0, "index"}});
    auto l = stash_list(c);
    size_t i = stash_index(c, p.pos.empty() ? "" : p.pos[0]);
    if (i >= l.size()) die(c, l.empty() ? "No stash entries found." : "stash@{" + std::to_string(i) + "} is not a valid reference");
    GitCommit w = read_commit(c, l[i].oid);
    if (w.parents.empty()) die(c, l[i].oid + " is not a stash-like commit");
    if (!c.repo->unmerged().empty()) die(c, "Cannot apply a stash in the middle of a merge");
    std::string base_tree = read_commit(c, w.parents[0]).tree;
    std::string itree = w.parents.size() > 1 ? read_commit(c, w.parents[1]).tree : base_tree;
    std::string ours_tree = write_tree_of(c, c.repo->index());
    // untracked files first: they must not be in the way
    if (w.parents.size() > 2) {
      GitCommit u = read_commit(c, w.parents[2]);
      for (auto& [path, f] : snapshot_tree(c, u.tree)) {
        if (c.host().exists(join(c.top, path))) {
          c.err << path << " already exists, no checkout\nerror: could not restore untracked files from stash\n";
          return 1;
        }
      }
    }
    GitTreeMerge tm = run_tree_merge(c, base_tree, ours_tree, w.tree, "Updated upstream", "Stashed changes", "");
    MergeApply ap;
    std::vector<GitIndexEntry> before = c.repo->index();
    if (!apply_tree_merge(c, base_tree, ours_tree, w.tree, tm, "Updated upstream", "Stashed changes", ap)) {
      c.err << "The stash entry is kept in case you need it again.\n";
      return 1;
    }
    for (auto& m : ap.messages) if (m.rfind("CONFLICT", 0) == 0 || !ap.clean) c.out << m << "\n";
    if (ap.clean) {
      // the changes come back unstaged (new files stay added), unless --index
      std::vector<GitIndexEntry> idx;
      std::map<std::string, GitIndexEntry> was;
      for (auto& e : before) was[e.path] = e;
      Snapshot is = snapshot_tree(c, itree);
      for (auto& e : c.repo->index()) {
        auto it = was.find(e.path);
        if (p.has("index")) {
          auto s = is.find(e.path);
          if (s != is.end()) { GitIndexEntry n = e; if (n.oid != s->second.oid) { n.oid = s->second.oid; n.mode = s->second.mode; n.mtime_ns = 0; } idx.push_back(n); continue; }
        }
        if (it != was.end()) idx.push_back(it->second);
        else idx.push_back(e);   // added by the stash
      }
      // a path the stash deleted stays deleted in the work tree, and staged as removed only with --index
      if (!p.has("index")) {
        for (auto& e : before) {
          bool present = false;
          for (auto& n : idx) if (n.path == e.path) { present = true; break; }
          if (!present) idx.push_back(e);
        }
      }
      c.repo->set_index(idx);
      if (auto wr = c.repo->write_index(); !wr.ok()) die(c, wr.error().message);
    }
    if (w.parents.size() > 2) {
      GitCommit u = read_commit(c, w.parents[2]);
      for (auto& [path, f] : snapshot_tree(c, u.tree)) {
        auto wr = c.repo->write_worktree_file(path, f.oid, f.mode);
        if (!wr.ok()) die(c, wr.error().message);
      }
    }
    if (!p.has("quiet")) {
      std::vector<std::string> st_args{"status"};
      Ctx sc(c.out, c.err, c.in);
      c.args = st_args;
      cmd_status(c);
    }
    if (!ap.clean) {
      c.out << "The stash entry is kept in case you need it again.\n";
      return 1;
    }
    if (sub == "pop") {
      std::string oid = l[i].oid;
      l.erase(l.begin() + (long)i);
      stash_write_list(c, l);
      if (!p.has("quiet")) c.out << "Dropped " << (p.pos.empty() ? "refs/stash@{0}" : "stash@{" + std::to_string(i) + "}") << " (" << oid << ")\n";
    }
    return 0;
  }
  if (sub == "branch" || sub == "create" || sub == "store") unsupported(c, "stash " + sub);
  if (sub != "push" && sub != "save") die(c, "unknown subcommand: " + sub);
  auto p = parse_options(c, pargs, 1, {{'m', "message", Arg::Required}, {'u', "include-untracked"}, {'k', "keep-index"},
                                        {'q', "quiet"}, {'a', "all"}, {'p', "patch"}, {'S', "staged"}, {0, "pathspec-from-file", Arg::Required}});
  if (p.has("patch") || p.has("all") || p.has("staged") || p.has("pathspec-from-file")) unsupported(c, "stash -p/-a/--staged");
  if (!p.after_dashdash.empty() || (sub == "push" && !p.pos.empty())) unsupported(c, "stash push with pathspecs");
  std::string user_msg = p.get("message");
  if (sub == "save" && !p.pos.empty()) { for (auto& x : p.pos) user_msg += (user_msg.empty() ? "" : " ") + x; }
  auto head = read_ref(c, "HEAD");
  if (!head) die(c, "You do not have the initial commit yet");
  if (!c.repo->unmerged().empty()) {
    c.err << "error: could not write index\n";
    return 1;
  }
  GitCommit hc = read_commit(c, *head);
  std::string itree = write_tree_of(c, c.repo->index());
  // the work tree's tracked files
  std::vector<GitIndexEntry> wentries;
  auto wt = c.repo->worktree_changes();
  if (!wt.ok()) die(c, wt.error().message);
  std::map<std::string, GitChange> changed;
  for (auto& ch : *wt) changed[ch.path] = ch.change;
  for (auto& e : c.repo->index()) {
    auto it = changed.find(e.path);
    if (it == changed.end()) { wentries.push_back(e); continue; }
    if (it->second == GitChange::Deleted) continue;
    auto s = c.repo->stage_entry(e.path, &e);
    if (!s.ok()) die(c, s.error().message);
    wentries.push_back(*s);
  }
  std::string wtree = write_tree_of(c, wentries);
  std::vector<std::string> untracked;
  if (p.has("include-untracked")) {
    auto un = c.repo->untracked(false, true);
    if (un.ok())
      for (auto& u : *un) {
        if (!u.empty() && u.back() == '/') { c.err << "Ignoring path " << u << "\n"; continue; }   // a nested repository
        untracked.push_back(u);
      }
  }
  if (itree == hc.tree && wtree == hc.tree && untracked.empty()) {
    if (!p.has("quiet")) c.out << "No local changes to save\n";
    return 0;
  }
  std::string b = current_branch(c);
  std::string where = (b.empty() ? "(no branch)" : b) + ": " + c.abbrev(*head) + " " + subject_of(hc.message);
  std::string icommit = make_commit(c, itree, {*head}, "index on " + where + "\n");
  std::vector<std::string> parents{*head, icommit};
  if (!untracked.empty()) {
    std::vector<GitIndexEntry> uents;
    for (auto& u : untracked) {
      auto s = c.repo->stage_entry(u, nullptr);
      if (!s.ok()) die(c, s.error().message);
      uents.push_back(*s);
    }
    std::string utree = write_tree_of(c, uents);
    parents.push_back(make_commit(c, utree, {}, "untracked files on " + where + "\n"));
  }
  std::string wmsg = user_msg.empty() ? "WIP on " + where : "On " + (b.empty() ? std::string("(no branch)") : b) + ": " + user_msg;
  std::string wcommit = make_commit(c, wtree, parents, wmsg);   // git writes this one without a newline
  auto l = stash_list(c);
  l.insert(l.begin(), StashEntry{read_ref(c, "refs/stash").value_or(std::string(40, '0')), wcommit, wmsg});
  stash_write_list(c, l);
  if (!p.has("quiet")) c.out << "Saved working directory and index state " << wmsg << "\n";
  // back to HEAD (keeping the index's content with --keep-index), and the untracked files go
  auto r = c.repo->checkout_tree(hc.tree, true);
  if (!r.ok()) die(c, r.error().message);
  if (p.has("keep-index")) {
    for (auto& [path, f] : snapshot_tree(c, itree)) {
      auto w = c.repo->write_worktree_file(path, f.oid, f.mode);
      if (!w.ok()) die(c, w.error().message);
    }
    std::vector<GitIndexEntry> idx;
    for (auto& [path, f] : snapshot_tree(c, itree)) {
      GitIndexEntry e{path, f.oid, f.mode};
      auto st = c.host().stat(join(c.top, path));
      if (st.ok()) { e.mtime_ns = st->mtime_ns; e.size = st->size; }
      idx.push_back(e);
    }
    for (auto& [path, f] : snapshot_tree(c, hc.tree)) {
      bool in = false;
      for (auto& e : idx) if (e.path == path) in = true;
      if (!in) (void)c.host().remove(join(c.top, path));
    }
    c.repo->set_index(idx);
    (void)c.repo->write_index();
  }
  for (auto& u : untracked) (void)c.host().remove(join(c.top, u));
  return 0;
}

}  // namespace aios_git
