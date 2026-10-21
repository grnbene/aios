// The operation log (see oplog.hpp): views, recording, undo/redo as reverts, and `git op`.
#include "oplog.hpp"

#include <algorithm>
#include <set>
#include <sstream>

#include "aios/hal/host.hpp"
#include "change.hpp"

namespace aios_git::oplog {

using namespace aios;

namespace {

const char* kOpRef = "refs/changes/op";

ByteSpan span(const std::string& s) { return ByteSpan((const uint8_t*)s.data(), s.size()); }

std::string join_args(const std::vector<std::string>& a) {
  std::string out = "git";
  for (auto& x : a) {
    out += " ";
    bool plain = !x.empty() && x.find_first_of(" \t\n'\"") == std::string::npos;
    out += plain ? x : "'" + x + "'";
  }
  for (auto& ch : out) if (ch == '\n') ch = ' ';
  return out;
}

std::string encode_view(const View& v) {
  std::string out = "head " + v.head + "\n";
  if (!v.worktree.empty()) out += "worktree " + v.worktree + "\n";
  if (!v.index.empty()) out += "index " + v.index + "\n";
  for (auto& [n, o] : v.refs) out += "ref " + n + " " + o + "\n";
  return out;
}

View decode_view(const std::string& t) {
  View v;
  std::istringstream ss(t);
  std::string line;
  while (std::getline(ss, line)) {
    if (line.rfind("head ", 0) == 0) v.head = line.substr(5);
    else if (line.rfind("worktree ", 0) == 0) v.worktree = line.substr(9);
    else if (line.rfind("index ", 0) == 0) v.index = line.substr(6);
    else if (line.rfind("ref ", 0) == 0) {
      size_t sp = line.rfind(' ');
      if (sp > 4) v.refs[line.substr(4, sp - 4)] = line.substr(sp + 1);
    }
  }
  return v;
}

bool excluded_ref(const std::string& name) {
  if (name == kOpRef) return true;
  if (name.rfind("refs/remotes/", 0) == 0 && name.size() > 5 && name.compare(name.size() - 5, 5, "/HEAD") == 0) return true;
  return false;
}

std::string read_head_text(Ctx& c) {
  auto t = c.read_git_file("HEAD");
  if (!t) return "";
  std::string s = *t;
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
  return s;
}

// The work tree as a tree: the index's entries with each changed file's content, plus untracked files git would
// list (not ignored ones, and not ones bigger than change.maxNewFileSize).
std::string worktree_tree(Ctx& c) {
  std::map<std::string, GitIndexEntry> idx;
  for (auto& e : c.repo->index()) idx[e.path] = e;
  for (auto& u : c.repo->unmerged()) if (!idx.count(u.path)) { GitIndexEntry e = u; e.stage = 0; idx[u.path] = e; }
  auto changes = c.repo->worktree_changes();
  if (changes.ok()) {
    for (auto& ch : *changes) {
      if (ch.change == aios::userland::GitChange::Deleted) { idx.erase(ch.path); continue; }
      auto it = idx.find(ch.path);
      auto s = c.repo->stage_entry(ch.path, it == idx.end() ? nullptr : &it->second);
      if (s.ok()) idx[ch.path] = *s; else idx.erase(ch.path);
    }
  }
  for (auto& u : c.repo->unmerged()) {
    auto s = c.repo->stage_entry(u.path, nullptr);
    if (s.ok()) idx[u.path] = *s; else idx.erase(u.path);
  }
  long long max_new = c.config.get_int("change.maxnewfilesize", 1 << 20);
  auto un = c.repo->untracked(false);
  if (un.ok()) {
    for (auto& p : *un) {
      auto st = c.host().stat(join(c.top, p));
      if (max_new > 0 && st.ok() && !st->is_link && (long long)st->size > max_new) continue;
      auto s = c.repo->stage_entry(p, nullptr);
      if (s.ok()) idx[p] = *s;
    }
  }
  std::vector<GitIndexEntry> v;
  for (auto& [p, e] : idx) v.push_back(e);
  return write_tree_of(c, v);
}

std::string index_tree(Ctx& c) {
  std::vector<GitIndexEntry> v = c.repo->index();
  return write_tree_of(c, v);
}

bool is_change_command(const std::string& cmd) {
  return cmd == "change" || cmd == "new" || cmd == "op" || cmd == "undo" || cmd == "redo" || cmd == "stack";
}

// The listing forms of commands that can also write: `remote` / `remote -v` / `remote get-url`, `branch` with no
// name or with --list/--show-current/..., `tag -l`, `stash list`, `worktree list`, `notes list`. Editors and
// prompts run these every few seconds; taken as writes, each one locked the repository, snapshotted and, with the
// change layer on, started a new @ on top of a HEAD that real git had moved - detaching it again.
bool lists_only(const std::vector<std::string>& args) {
  const std::string& cmd = args[0];
  const std::string sub = args.size() > 1 ? args[1] : "";
  auto has = [&](std::initializer_list<const char*> flags) {
    for (size_t i = 1; i < args.size(); ++i)
      for (const char* f : flags)
        if (args[i] == f || (args[i].rfind(std::string(f) + "=", 0) == 0)) return true;
    return false;
  };
  auto positional = [&]() {
    size_t n = 0;
    for (size_t i = 1; i < args.size(); ++i)
      if (!args[i].empty() && args[i][0] != '-') ++n;
    return n;
  };
  if (cmd == "remote") return args.size() == 1 || sub == "-v" || sub == "--verbose" || sub == "show" || sub == "get-url";
  if (cmd == "stash") return sub == "list" || sub == "show";
  if (cmd == "worktree") return sub == "list";
  if (cmd == "notes") return sub == "list" || sub == "show";
  if (cmd == "symbolic-ref") return positional() <= 1 && !has({"-d", "--delete"});
  if (cmd == "branch") {
    if (has({"-d", "-D", "--delete", "-m", "-M", "--move", "-c", "-C", "--copy", "-u", "--set-upstream-to",
             "--unset-upstream", "--edit-description", "-f", "--force", "-t", "--track", "--no-track"}))
      return false;
    return positional() == 0 || has({"-l", "--list", "--contains", "--no-contains", "--merged", "--no-merged",
                                     "--points-at"});
  }
  if (cmd == "tag") {
    if (has({"-d", "--delete", "-a", "--annotate", "-s", "--sign", "-u", "--local-user", "-m", "--message", "-F",
             "--file", "-f", "--force"}))
      return false;
    return positional() == 0 || has({"-l", "--list", "-n", "--contains", "--no-contains", "--merged", "--no-merged",
                                     "--points-at"});
  }
  return false;
}

// Commands that never change a repository: nothing is recorded around them (and outside the change layer, no
// snapshot is taken either).
bool read_only(const std::vector<std::string>& args, bool change_mode) {
  static const std::set<std::string> never = {"cat-file", "hash-object", "rev-parse", "var", "check-ignore", "ls-remote",
                                              "upload-pack", "git-upload-pack", "receive-pack", "git-receive-pack",
                                              "http-backend", "merge-base", "show-ref", "for-each-ref", "rev-list", "fsck",
                                              "config", "bundle", "archive", "fsmonitor--daemon", "blocks", "repack", "gc",
                                              "outline", "describe", "name-rev", "count-objects", "check-attr",
                                              "check-ref-format", "cherry", "show-branch", "verify-commit", "verify-tag",
                                              "version", "help"};
  static const std::set<std::string> looking = {"log", "show", "diff", "status", "grep", "ls-files", "ls-tree", "reflog",
                                                "blame", "shortlog", "whatchanged"};
  if (args.empty()) return true;
  if (never.count(args[0]) || lists_only(args)) return true;
  if (looking.count(args[0])) return !change_mode;   // in the change layer, looking snapshots first
  return false;
}

}  // namespace

bool enabled(Ctx& c) {
  if (!c.repo || c.repo->bare()) return false;
  return c.config.get_bool("core.operationlog", false) || change::enabled(c);
}

View capture(Ctx& c, bool with_trees) {
  View v;
  v.head = read_head_text(c);
  for (auto& [n, o] : all_refs(c)) if (!excluded_ref(n)) v.refs[n] = o;
  if (with_trees && !c.repo->bare()) {
    v.index = index_tree(c);
    v.worktree = worktree_tree(c);
  }
  return v;
}

Op read_op(Ctx& c, const std::string& id) {
  Op op;
  op.id = id;
  auto cm = c.repo->read_commit(id);
  if (!cm.ok()) die(c, "no operation " + id);
  if (!cm->parents.empty()) op.parent = cm->parents[0];
  std::istringstream ss(cm->raw_message);
  std::string line;
  std::getline(ss, op.description);
  while (std::getline(ss, line)) {
    if (line.rfind("kind: ", 0) == 0) op.kind = line.substr(6);
    else if (line.rfind("target: ", 0) == 0) op.target = line.substr(8);
    else if (line.rfind("args: ", 0) == 0) op.args = line.substr(6);
    else if (line.rfind("user: ", 0) == 0) op.user = line.substr(6);
    else if (line.rfind("time: ", 0) == 0) op.time = std::atoll(line.c_str() + 6);
    else if (line.rfind("rewrite ", 0) == 0) {
      std::string rest = line.substr(8);
      size_t sp = rest.find(' ');
      std::string n = sp == std::string::npos ? "" : rest.substr(sp + 1);
      op.rewrites.push_back({rest.substr(0, sp), n == "-" ? "" : n});
    }
  }
  auto ents = c.repo->read_tree(cm->tree);
  if (ents.ok()) for (auto& e : *ents) if (e.name == "view") op.view = decode_view(blob_text(c, e.oid));
  return op;
}

std::string record_once(Ctx& c, const std::string& description, const std::string& kind, const std::string& target,
                        const std::vector<std::pair<std::string, std::string>>& rewrites);

std::optional<Op> head_op(Ctx& c) {
  auto r = c.repo->ref_oid(kOpRef);
  if (!r.ok()) return std::nullopt;
  return read_op(c, *r);
}

std::string resolve_op(Ctx& c, const std::string& spec_in) {
  std::string spec = spec_in;
  size_t back = 0;
  while (!spec.empty() && spec.back() == '-') { spec.pop_back(); ++back; }
  std::string id;
  if (spec == "@" || spec.empty()) {
    auto h = c.repo->ref_oid(kOpRef);
    if (!h.ok()) die(c, "the operation log is empty");
    id = *h;
  } else {
    auto h = c.repo->ref_oid(kOpRef);
    if (!h.ok()) die(c, "the operation log is empty");
    std::string cur = *h;
    while (!cur.empty()) {
      if (cur.rfind(spec, 0) == 0) { id = cur; break; }
      auto cm = c.repo->read_commit(cur);
      if (!cm.ok() || cm->parents.empty()) break;
      cur = cm->parents[0];
    }
    if (id.empty()) die(c, "no operation " + spec);
  }
  for (size_t i = 0; i < back; ++i) {
    Op op = read_op(c, id);
    if (op.parent.empty()) die(c, "operation " + spec_in + " goes past the first one");
    id = op.parent;
  }
  return id;
}

std::string record(Ctx& c, const std::string& description, const std::string& kind, const std::string& target,
                   const std::vector<std::pair<std::string, std::string>>& rewrites) {
  if (!enabled(c)) return "";
  // Appended optimistically: the operation is written on the head it read, and the log's ref swapped from that head;
  // when another process appended first (the fsmonitor daemon's snapshot takes no lock), it is written again on top.
  for (int attempt = 0;; ++attempt) {
    std::string id = record_once(c, description, kind, target, rewrites);
    if (id != "\x01") return id;
    if (attempt >= 20) die(c, "could not record the operation: the operation log kept moving under it");
  }
}

std::string record_once(Ctx& c, const std::string& description, const std::string& kind, const std::string& target,
                        const std::vector<std::pair<std::string, std::string>>& rewrites) {
  // the files and the index are kept until there is a working-copy change holding them (the first operation
  // `git change init` records comes before @ does: undoing the switch puts the index back from it)
  View v = capture(c, !change::enabled(c) || !c.repo->ref_oid(change::wc_ref(c)).ok());
  auto prev = head_op(c);
  if (prev && prev->view == v && rewrites.empty() && kind != "undo" && kind != "redo") return "";
  auto blob = c.repo->write_object("blob", span(encode_view(v)));
  if (!blob.ok()) die(c, blob.error().message);
  GitIndexEntry e;
  e.path = "view";
  e.oid = *blob;
  e.mode = 0100644;
  std::string tree = write_tree_of(c, {e});
  std::string msg = description + "\n\nkind: " + kind + "\n";
  if (!target.empty()) msg += "target: " + target + "\n";
  msg += "time: " + std::to_string(now_seconds()) + "\n";
  msg += "user: " + c.ident(true, false) + "\n";
  msg += "args: " + join_args(c.args) + "\n";
  for (auto& [o, n] : rewrites) msg += "rewrite " + o + " " + (n.empty() ? "-" : n) + "\n";
  std::vector<std::string> parents;
  if (prev) parents.push_back(prev->id);
  std::string id = make_commit(c, tree, parents, msg, c.ident(false, false), c.ident(true, false));
  if (auto r = c.repo->compare_and_swap_ref(kOpRef, prev ? prev->id : "", id); !r.ok()) {
    if (r.error().status == Status::Conflict) return "\x01";   // another operation came first: again, on it
    die(c, "could not record the operation: " + r.error().message);
  }
  return id;
}

void ensure_started(Ctx& c, const std::string& description) {
  if (c.repo->ref_oid(kOpRef).ok()) return;
  if (!enabled(c)) return;
  record(c, description, "normal", "", {});
}

void before_command(Ctx& c) {
  if (c.args.empty() || is_change_command(c.args[0]) || read_only(c.args, true)) return;
  if (!c.try_open_repo() || !enabled(c)) return;
  bool cm = change::enabled(c);
  if (read_only(c.args, cm)) return;
  if (cm) {
    if (change::ensure_wc(c)) change::snapshot(c);
    return;
  }
  auto prev = head_op(c);
  View v = capture(c, true);
  if (prev && prev->view == v) return;
  record(c, prev && !prev->view.same_refs(v) ? "refs changed outside the operation log" : "snapshot working copy", "snapshot", "", {});
}

bool needs_lock(Ctx& c) {
  if (c.args.empty() || !c.try_open_repo() || !enabled(c)) return false;
  if (is_change_command(c.args[0])) return true;
  return !read_only(c.args, change::enabled(c));
}

void after_command(Ctx& c) {
  if (c.args.empty() || is_change_command(c.args[0]) || read_only(c.args, true)) return;
  // Neither the operation log nor the change layer is on, and only `config` could have turned one on: nothing to
  // record, and reading the repository afresh (dropping everything it had read) was a millisecond of `status`.
  if (c.repo && !enabled(c) && c.args[0] != "config") return;
  // what the command left on disk, read afresh
  c.repo.reset();
  change::forget(c);
  if (!c.try_open_repo() || !enabled(c)) return;
  bool cm = change::enabled(c);
  if (read_only(c.args, cm)) return;
  if (cm && change::ensure_wc(c)) change::snapshot(c);
  record(c, join_args(c.args), "normal", "", {});
}

// ---------------------------------------------------------------------------------------------------------------
// putting state back
// ---------------------------------------------------------------------------------------------------------------
void apply_tree_to_disk(Ctx& c, const std::string& from, const std::string& to) {
  if (from == to) return;
  Snapshot a = snapshot_tree(c, from), b = snapshot_tree(c, to);
  auto& h = c.host();
  for (auto& [p, s] : a) {
    if (b.count(p)) continue;
    (void)h.remove(join(c.top, p));
    for (size_t sl = p.rfind('/'); sl != std::string::npos && sl > 0; sl = p.rfind('/', sl - 1)) {
      auto names = h.list_dir(join(c.top, p.substr(0, sl)));
      bool empty = names.ok();
      if (names.ok()) for (auto& n : *names) if (n != "." && n != "..") { empty = false; break; }
      if (!empty || !h.remove(join(c.top, p.substr(0, sl))).ok()) break;
    }
  }
  for (auto& [p, s] : b) {
    auto it = a.find(p);
    if (it != a.end() && it->second.oid == s.oid && it->second.mode == s.mode) continue;
    if (s.mode == 0160000) continue;
    auto w = c.repo->write_worktree_file(p, s.oid, s.mode);
    if (!w.ok()) die(c, "could not write " + p + ": " + w.error().message);
  }
}

namespace {

void set_index_to_tree(Ctx& c, const std::string& tree) {
  std::vector<GitIndexEntry> ents;
  if (!tree.empty() && tree != empty_tree_oid()) {
    auto r = c.repo->flatten_tree(tree, "", ents);
    if (!r.ok()) die(c, r.error().message);
  }
  // keep what the index knew about files it already had as they are (their stat information stays good)
  std::map<std::string, GitIndexEntry> now;
  for (auto& e : c.repo->index()) now[e.path] = e;
  for (auto& e : ents) {
    auto it = now.find(e.path);
    if (it != now.end() && it->second.oid == e.oid && it->second.mode == e.mode) e = it->second;
  }
  c.repo->set_index(ents);
  c.repo->set_unmerged({});
  if (auto r = c.repo->write_index(); !r.ok()) die(c, r.error().message);
}

void set_head_text(Ctx& c, const std::string& head) {
  if (head.rfind("ref: ", 0) == 0) {
    if (auto r = c.repo->set_head(head.substr(5)); !r.ok()) die(c, r.error().message);
  } else if (head.size() == 40) {
    if (auto r = c.repo->set_head_detached(head); !r.ok()) die(c, r.error().message);
  }
}

void set_ref(Ctx& c, const std::string& name, const std::string& oid, const std::string& why) {
  auto cur = c.repo->ref_oid(name);
  std::string before = cur.ok() ? *cur : "";
  if (oid.empty()) {
    if (cur.ok()) (void)c.repo->delete_ref(name);
  } else if (before != oid) {
    if (auto r = c.repo->update_ref(name, oid); !r.ok()) die(c, r.error().message);
  } else {
    return;
  }
  if (name.rfind("refs/heads/", 0) == 0) append_reflog(c, name, before, oid, why);
}

// One change's three versions: the version an operation left (base), the one there is now (ours) and the one before
// the operation (theirs). What changed between base and theirs is applied onto ours: the description, the parents,
// the content.
std::string merge_versions(Ctx& c, const std::string& base, const std::string& ours, const std::string& theirs) {
  GitCommit b = read_commit(c, base), o = read_commit(c, ours), t = read_commit(c, theirs);
  change::CommitSpec s = change::spec_of(c, o);
  if (o.raw_message == b.raw_message) s.message = t.raw_message;
  if (o.parents == b.parents) s.parents = t.parents;
  auto m = change::merge_trees(c, b.tree, o.tree, t.tree, "now", "before the operation");
  s.tree = m.tree;
  auto carried = change::read_conflicts(c, o);
  for (auto& [p, k] : change::read_conflicts(c, t)) carried[p] = k;
  for (auto& [p, k] : m.conflicts) carried[p] = k;
  s.conflict_sides = change::write_conflicts(c, change::still_conflicted(c, m.tree, carried));
  return change::write_commit(c, s);
}

// What one command did: its operation and the snapshots the same command recorded right after it (`git change init`
// records the switch, then snapshots what was edited into @). Undone apart, the operation's refs have all moved on by
// the time it is reverted and nothing goes back; as one, the view it is reverted from is the last snapshot's.
Op command_unit(Ctx& c, const Op& t) {
  auto h = head_op(c);
  std::vector<std::string> newer;   // newest first, down to t
  for (std::string cur = h ? h->id : ""; !cur.empty() && cur != t.id;) {
    newer.push_back(cur);
    cur = read_op(c, cur).parent;
  }
  Op unit = t;
  for (auto it = newer.rbegin(); it != newer.rend(); ++it) {
    Op next = read_op(c, *it);
    if (next.kind != "snapshot" || next.args != t.args) break;
    unit.view = next.view;
  }
  return unit;
}

bool layer_on(const View& v) {
  for (auto& [n, o] : v.refs) if (n.rfind("refs/changes/wc/", 0) == 0) return true;
  return false;
}

// Reverting an operation that turned the change layer on (git change init) or off (git change disable): the layer
// goes back the other way too. Off again: HEAD back on what it named before, the index as it was staged then (when @
// still holds just what the operation left, else the branch's commit, as `git change disable` leaves it), the files
// as they are. On again: HEAD back on the working-copy change. Changes worked on since keep their refs.
void revert_layer_switch(Ctx& c, const Op& t, const Op& p, const std::string& kind, const std::string& description) {
  const bool turn_on = layer_on(p.view);
  View cur = capture(c, false);
  std::set<std::string> names;
  for (auto& [n, o] : t.view.refs) names.insert(n);
  for (auto& [n, o] : p.view.refs) names.insert(n);
  bool wc_untouched = true;
  for (auto& n : names) {
    auto get = [&](const View& v) { auto it = v.refs.find(n); return it == v.refs.end() ? std::string() : it->second; };
    std::string had = get(t.view), want = get(p.view), now = get(cur);
    if (n.rfind("refs/changes/wc/", 0) == 0 && now != had) wc_untouched = false;
    if (had == want || now == want) continue;
    if (now == had) set_ref(c, n, want, description);
    else if (n.rfind("refs/changes/wc/", 0) == 0 && !turn_on) (void)c.repo->delete_ref(n);   // the layer is off: no @
    else if (n.rfind("refs/changes/", 0) != 0) c.err << "warning: " << n << " has moved since that operation; left where it is\n";
  }
  if (turn_on) {
    if (auto r = c.repo->set_config("change.enabled", "true"); !r.ok()) die(c, r.error().message);
    c.config.add("change.enabled", "true");
    // HEAD on @ and the index its tree, as `git change init` leaves them; the files are @'s next snapshot
    change::forget(c);
    std::string wc = change::wc_commit(c);
    if (wc.empty()) set_head_text(c, p.view.head);
    else {
      (void)c.repo->set_head_detached(wc);
      set_index_to_tree(c, read_commit(c, wc).tree);
    }
  } else {
    set_head_text(c, p.view.head);
    std::string tree = wc_untouched ? p.view.index : "";
    if (tree.empty()) {
      auto h = read_ref(c, "HEAD");
      tree = h ? read_commit(c, peel_to(c, *h, "commit")).tree : empty_tree_oid();
    }
    set_index_to_tree(c, tree);
    if (auto r = c.repo->set_config("change.enabled", "false"); !r.ok()) die(c, r.error().message);
    c.config.add("change.enabled", "false");
    // the log may have been on only through the layer: this undo is still recorded (redo needs it), nothing after it
    if (!c.config.get_bool("core.operationlog", false)) c.config.add("core.operationlog", "true");
  }
  change::forget(c);
  record(c, description, kind, t.id, {});
}

// Reverts what operation `t` did (relative to the operation before it) onto the present.
void revert_op(Ctx& c, const Op& t_in, const std::string& kind, const std::string& description) {
  if (t_in.parent.empty()) die(c, "operation " + t_in.id.substr(0, 12) + " is the first one: there is nothing before it to go back to");
  const Op t = command_unit(c, t_in);
  Op p = read_op(c, t.parent);
  if (layer_on(p.view) != layer_on(t.view)) { revert_layer_switch(c, t, p, kind, description); return; }
  bool cm = change::enabled(c);
  View cur = capture(c, !cm);
  std::string wc_before = cm ? change::wc_commit(c) : "";
  std::set<std::string> names;
  for (auto& [n, o] : t.view.refs) names.insert(n);
  for (auto& [n, o] : p.view.refs) names.insert(n);
  std::vector<std::string> kept;
  std::map<std::string, std::string> merged_memo;
  for (auto& n : names) {
    auto get = [&](const View& v) { auto it = v.refs.find(n); return it == v.refs.end() ? std::string() : it->second; };
    std::string had = get(t.view), want = get(p.view), now = get(cur);
    if (had == want || now == want) continue;
    if (now == had) { set_ref(c, n, want, description); continue; }
    auto is_commit = [&](const std::string& o) { return !o.empty() && type_of(c, o) == "commit"; };
    if (cm && is_commit(now) && is_commit(had)) {
      std::string cid = change::change_id_of(read_commit(c, now));
      if (cid == change::change_id_of(read_commit(c, had))) {
        // the ref holds a later version of what the operation left: that change was worked on since
        if (want.empty()) continue;   // what the operation added stays, being in use
        if (is_commit(want) && change::change_id_of(read_commit(c, want)) == cid) {
          std::string key = had + now + want;
          if (!merged_memo.count(key)) merged_memo[key] = merge_versions(c, had, now, want);
          set_ref(c, n, merged_memo[key], description);
          continue;
        }
        // the ref goes back to another change; the later version stays visible on its own
        (void)c.repo->update_ref("refs/changes/heads/" + cid, now);
        set_ref(c, n, want, description);
        continue;
      }
    }
    kept.push_back(n);
  }
  if (!cm && t.view.head != p.view.head && cur.head == t.view.head) set_head_text(c, p.view.head);
  for (auto& n : kept) c.err << "warning: " << n << " has moved since that operation; left where it is\n";
  if (cm) {
    change::forget(c);
    std::string wc_after = change::wc_commit(c);
    if (wc_after != wc_before && !wc_after.empty()) {
      (void)c.repo->set_head_detached(wc_after);
      append_reflog(c, "HEAD", wc_before, wc_after, description);
      std::string from = wc_before.empty() ? empty_tree_oid() : read_commit(c, wc_before).tree;
      change::check_out_tree(c, from, read_commit(c, wc_after).tree);
      wc_before = wc_after;
    }
    record(c, description, kind, t.id, {});
    change::evolve(c, "rebase onto the changes '" + description + "' put back");
    change::forget(c);
    wc_after = change::wc_commit(c);
    if (wc_after != wc_before && !wc_after.empty()) {
      (void)c.repo->set_head_detached(wc_after);
      append_reflog(c, "HEAD", wc_before, wc_after, description);
      std::string from = wc_before.empty() ? empty_tree_oid() : read_commit(c, wc_before).tree;
      change::check_out_tree(c, from, read_commit(c, wc_after).tree);
    }
  } else {
    auto three = [&](const std::string& base, const std::string& ours, const std::string& theirs) {
      if (base.empty() || theirs.empty() || base == theirs) return ours;
      auto m = change::merge_trees(c, base, ours, theirs, "now", "before the operation");
      for (auto& [path, k] : m.conflicts) c.err << "warning: " << path << " conflicts with what changed since; markers left in it\n";
      return m.tree;
    };
    std::string wt = three(t.view.worktree, cur.worktree, p.view.worktree);
    std::string ix = three(t.view.index, cur.index, p.view.index);
    apply_tree_to_disk(c, cur.worktree, wt);
    set_index_to_tree(c, ix);
  }
  change::forget(c);
  if (!cm) record(c, description, kind, t.id, {});
}

std::string undo_target(Ctx& c) {
  auto h = head_op(c);
  if (!h) die(c, "the operation log is empty: nothing to undo");
  std::set<std::string> undone;
  std::string cur = h->id;
  while (!cur.empty()) {
    Op op = read_op(c, cur);
    if (undone.count(op.id)) { cur = op.parent; continue; }
    if (op.kind == "snapshot") { cur = op.parent; continue; }
    if (op.kind == "undo") { undone.insert(op.target); cur = op.parent; continue; }
    if (op.parent.empty()) die(c, "nothing left to undo");
    return op.id;   // a normal operation, or a redo
  }
  die(c, "nothing left to undo");
}

std::string redo_target(Ctx& c) {
  auto h = head_op(c);
  if (!h) die(c, "the operation log is empty: nothing to redo");
  std::set<std::string> undone;
  std::string cur = h->id;
  while (!cur.empty()) {
    Op op = read_op(c, cur);
    if (undone.count(op.id) || op.kind == "snapshot") { cur = op.parent; continue; }
    if (op.kind == "redo") { undone.insert(op.target); cur = op.parent; continue; }
    if (op.kind == "undo") return op.id;
    break;
  }
  die(c, "nothing to redo: the last operation was not an undo");
}

void open_for_ops(Ctx& c) {
  c.open_repo();
  if (!enabled(c)) die(c, "the operation log is off here: 'git op enable' (or 'git change init') turns it on");
  // what is on disk now is part of the present an undo applies to
  if (change::enabled(c)) { if (change::ensure_wc(c)) change::snapshot(c); }
  else {
    auto prev = head_op(c);
    View v = capture(c, true);
    if (!prev || !(prev->view == v)) record(c, "snapshot working copy", "snapshot", "", {});
  }
}

std::string when_text(int64_t t) { return format_date(t, "+0000", "iso"); }

void print_op_line(Ctx& c, const Op& op, bool current) {
  c.out << (current ? "@ " : "o ") << op.id.substr(0, 12) << " " << when_text(op.time) << " " << op.kind << "\n";
  c.out << "|  " << op.description << "\n";
  if (op.args != "git" && !op.args.empty() && op.args != op.description) c.out << "|  args: " << op.args << "\n";
}

}  // namespace

int cmd_undo(Ctx& c) {
  open_for_ops(c);
  std::string t = c.args.size() > 1 ? resolve_op(c, c.args[1]) : undo_target(c);
  Op op = read_op(c, t);
  revert_op(c, op, "undo", "undo: " + op.description);
  c.out << "Undid operation " << t.substr(0, 12) << ": " << op.description << "\n";
  return 0;
}

int cmd_redo(Ctx& c) {
  open_for_ops(c);
  std::string t = redo_target(c);
  Op op = read_op(c, t);
  revert_op(c, op, "redo", "redo: " + (op.description.rfind("undo: ", 0) == 0 ? op.description.substr(6) : op.description));
  c.out << "Redid operation " << op.target.substr(0, 12) << "\n";
  return 0;
}

int cmd_op(Ctx& c) {
  std::string sub = c.args.size() > 1 ? c.args[1] : "log";
  if (sub == "enable") {
    c.open_repo();
    if (auto r = c.repo->set_config("core.operationLog", "true"); !r.ok()) die(c, r.error().message);
    c.config.add("core.operationlog", "true");
    ensure_started(c, "start the operation log");
    c.out << "The operation log is on: every command is recorded, and 'git undo' takes any of them back.\n";
    return 0;
  }
  if (sub == "disable") {
    c.open_repo();
    (void)c.repo->set_config("core.operationLog", "false");
    c.out << "The operation log is off (what it recorded is kept under refs/changes/op).\n";
    return 0;
  }
  c.open_repo();
  if (!enabled(c) && !c.repo->ref_oid(kOpRef).ok()) die(c, "the operation log is off here: 'git op enable' turns it on");
  if (sub == "log") {
    long long limit = 30;
    for (size_t i = 2; i < c.args.size(); ++i) {
      if ((c.args[i] == "-n" || c.args[i] == "--limit") && i + 1 < c.args.size()) limit = std::atoll(c.args[++i].c_str());
      else if (c.args[i].rfind("-n", 0) == 0 && c.args[i].size() > 2) limit = std::atoll(c.args[i].c_str() + 2);
      else if (c.args[i] == "--all") limit = -1;
    }
    auto h = c.repo->ref_oid(kOpRef);
    std::string cur = h.ok() ? *h : "";
    bool first = true;
    for (long long n = 0; !cur.empty() && (limit < 0 || n < limit); ++n) {
      Op op = read_op(c, cur);
      print_op_line(c, op, first);
      first = false;
      cur = op.parent;
    }
    return 0;
  }
  if (sub == "show") {
    std::string id = resolve_op(c, c.args.size() > 2 ? c.args[2] : "@");
    Op op = read_op(c, id);
    c.out << "operation " << op.id << "\n";
    c.out << "kind:  " << op.kind << (op.target.empty() ? "" : " of " + op.target.substr(0, 12)) << "\n";
    c.out << "when:  " << when_text(op.time) << "\n";
    c.out << "by:    " << op.user << "\n";
    c.out << "ran:   " << op.args << "\n\n    " << op.description << "\n\n";
    View before;
    if (!op.parent.empty()) before = read_op(c, op.parent).view;
    if (before.head != op.view.head) c.out << "HEAD: " << (before.head.empty() ? "(none)" : before.head) << " -> " << op.view.head << "\n";
    std::set<std::string> names;
    for (auto& [n, o] : before.refs) names.insert(n);
    for (auto& [n, o] : op.view.refs) names.insert(n);
    for (auto& n : names) {
      auto a = before.refs.count(n) ? before.refs.at(n) : "";
      auto b = op.view.refs.count(n) ? op.view.refs.at(n) : "";
      if (a == b) continue;
      c.out << (a.empty() ? "+ " : b.empty() ? "- " : "~ ") << n << " " << (a.empty() ? "" : a.substr(0, 12)) << (a.empty() || b.empty() ? "" : " -> ")
            << (b.empty() ? "" : b.substr(0, 12)) << "\n";
    }
    if (!op.view.worktree.empty() && before.worktree != op.view.worktree && !before.worktree.empty()) {
      DiffOptions o;
      o.format = DiffOptions::NameStatus;
      auto pairs = diff_trees(c, before.worktree, op.view.worktree, Pathspec{}, o);
      if (!pairs.empty()) { c.out << "work tree:\n"; print_diff(c, pairs, o, c.out); }
    }
    for (auto& [o, n] : op.rewrites) c.out << "rewrote " << o.substr(0, 12) << " -> " << (n.empty() ? "(abandoned)" : n.substr(0, 12)) << "\n";
    return 0;
  }
  if (sub == "undo" || sub == "revert") {
    open_for_ops(c);
    std::string id = c.args.size() > 2 ? resolve_op(c, c.args[2]) : undo_target(c);
    Op op = read_op(c, id);
    revert_op(c, op, "undo", "undo: " + op.description);
    c.out << "Undid operation " << id.substr(0, 12) << ": " << op.description << "\n";
    return 0;
  }
  if (sub == "restore") {
    if (c.args.size() < 3) usage(c, "usage: git op restore <operation>");
    open_for_ops(c);
    std::string id = resolve_op(c, c.args[2]);
    Op op = read_op(c, id);
    bool cm = change::enabled(c);
    View cur = capture(c, !cm);
    std::string wc_before = cm ? change::wc_commit(c) : "";
    std::set<std::string> names;
    for (auto& [n, o] : cur.refs) names.insert(n);
    for (auto& [n, o] : op.view.refs) names.insert(n);
    for (auto& n : names) set_ref(c, n, op.view.refs.count(n) ? op.view.refs.at(n) : "", "op restore " + id.substr(0, 12));
    if (cm) {
      change::forget(c);
      std::string wc_after = change::wc_commit(c);
      if (!wc_after.empty()) {
        (void)c.repo->set_head_detached(wc_after);
        std::string from = wc_before.empty() ? empty_tree_oid() : read_commit(c, wc_before).tree;
        change::check_out_tree(c, from, read_commit(c, wc_after).tree);
      }
    } else {
      set_head_text(c, op.view.head);
      if (!op.view.worktree.empty()) apply_tree_to_disk(c, cur.worktree, op.view.worktree);
      if (!op.view.index.empty()) set_index_to_tree(c, op.view.index);
    }
    change::forget(c);
    record(c, "restore to operation " + id.substr(0, 12), "normal", id, {});
    c.out << "Restored the repository to operation " << id.substr(0, 12) << ": " << op.description << "\n";
    return 0;
  }
  usage(c, "usage: git op (log [-n N] | show [<op>] | undo [<op>] | restore <op> | enable | disable)");
}

}  // namespace aios_git::oplog
