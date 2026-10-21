// git change ... and git new: working with changes (see change.hpp).
#include <algorithm>
#include <functional>
#include <set>
#include <sstream>

#include "aios/hal/host.hpp"
#include "change.hpp"
#include "oplog.hpp"

namespace aios_git {

using namespace change;

namespace {

std::vector<std::string> sub_args(Ctx& c, size_t start) {
  return std::vector<std::string>(c.args.begin() + (long)std::min(start, c.args.size()), c.args.end());
}

std::string message_from(const Parsed& p) {
  std::string msg;
  for (auto& m : p.all("message")) {
    if (!msg.empty()) msg += "\n";
    msg += m;
    if (msg.back() != '\n') msg += "\n";
  }
  return cleanup_message(msg, false);
}

GitCommit cm(Ctx& c, const std::string& oid) { return read_commit(c, oid); }

std::string tree_with_paths(Ctx& c, const std::string& base_tree, const std::string& from_tree, const Pathspec& ps) {
  Snapshot base = snapshot_tree(c, base_tree), from = snapshot_tree(c, from_tree);
  std::map<std::string, FileState> out = base;
  for (auto& [p, s] : base) if (ps.match(p) && !from.count(p)) out.erase(p);
  for (auto& [p, s] : from) if (ps.match(p)) out[p] = s;
  std::vector<GitIndexEntry> ents;
  for (auto& [p, s] : out) ents.push_back(GitIndexEntry{p, s.oid, s.mode});
  return write_tree_of(c, ents);
}

void print_change(Ctx& c, const std::string& label, const std::string& oid) {
  c.out << label << describe_line(c, cm(c, oid)) << "\n";
}

std::vector<std::string> children_of(Ctx& c, const std::string& oid) {
  std::vector<std::string> out;
  for (auto& v : visible_commits(c))
    for (auto& p : cm(c, v).parents)
      if (p == oid) { out.push_back(v); break; }
  return out;
}

void say_rebased(Ctx& c, size_t n) {
  if (n) c.out << "Rebased " << n << " descendant change" << (n == 1 ? "" : "s") << ".\n";
}

std::string wc_line(Ctx& c) {
  std::string wc = wc_commit(c);
  return wc.empty() ? "" : describe_line(c, cm(c, wc));
}

// ---------------------------------------------------------------------------------------------------------------
// What @ changed from its parent, then @ and its parent: git change status, and plain git status with the layer on.
int print_working_copy(Ctx& c) {
  std::string wc = wc_commit(c);
  GitCommit w = cm(c, wc);
  MergedTree pt = parents_tree(c, w.parents);
  DiffOptions o;
  o.format = DiffOptions::NameStatus;
  auto pairs = diff_trees(c, pt.tree, w.tree, Pathspec{}, o);
  if (pairs.empty()) c.out << "The working copy has no changes.\n";
  else {
    c.out << "Working copy changes:\n";
    for (auto& d : pairs) c.out << d.status << " " << (d.new_path.empty() ? d.old_path : d.new_path) << "\n";
  }
  c.out << "Working copy  (@) : " << describe_line(c, w) << "\n";
  for (auto& p : w.parents) c.out << "Parent commit (@-): " << describe_line(c, cm(c, p)) << "\n";
  auto cs = read_conflicts(c, w);
  if (!cs.empty()) {
    c.out << "There are unresolved conflicts at these paths:\n";
    for (auto& [p, k] : cs) c.out << "  " << p << "    " << k.kind << " (" << k.ours_label << " / " << k.theirs_label << ")\n";
    c.out << "Edit them to remove the markers (the next command records the resolution), or: git change resolve --ours|--theirs <path>\n";
  }
  return 0;
}

int change_status(Ctx& c, size_t start) {
  (void)start;
  open_changes(c);
  return print_working_copy(c);
}

// The log graph: one lane per line of history being followed, commits newest first with each one's children above it.
int change_log(Ctx& c, size_t start) {
  auto p = parse_options(c, c.args, start, {{'r', "revisions", Arg::Required}, {'n', "limit", Arg::Required}, {0, "no-graph"}, {0, "all"}});
  open_changes(c);
  long long limit = p.has("limit") ? std::atoll(p.get("limit").c_str()) : 50;
  std::vector<std::string> shown;
  if (p.has("revisions")) {
    for (auto& r : p.all("revisions")) for (auto& o : resolve_revs(c, r)) shown.push_back(o);
  } else {
    shown = visible_commits(c, p.has("all") ? 0 : (size_t)std::max<long long>(1, limit));
  }
  std::set<std::string> in(shown.begin(), shown.end());
  // children before parents, newest first among what is free to place
  std::map<std::string, int> kids;
  for (auto& o : shown) for (auto& par : cm(c, o).parents) if (in.count(par)) kids[par]++;
  std::vector<std::string> order, ready;
  for (auto it = shown.rbegin(); it != shown.rend(); ++it) if (!kids[*it]) ready.push_back(*it);
  std::set<std::string> placed;
  while (!ready.empty()) {
    // the newest ready commit next
    auto best = std::max_element(ready.begin(), ready.end(), [&](const std::string& a, const std::string& b) {
      return cm(c, a).committer_time < cm(c, b).committer_time;
    });
    std::string x = *best;
    ready.erase(best);
    if (!placed.insert(x).second) continue;
    order.push_back(x);
    for (auto& par : cm(c, x).parents) if (in.count(par) && --kids[par] == 0) ready.push_back(par);
  }
  std::string wc = wc_commit(c);
  std::vector<std::string> lanes;
  bool graph = !p.has("no-graph");
  for (auto& x : order) {
    GitCommit g = cm(c, x);
    auto lane = std::find(lanes.begin(), lanes.end(), x);
    size_t col;
    if (lane == lanes.end()) { lanes.push_back(x); col = lanes.size() - 1; }
    else col = (size_t)(lane - lanes.begin());
    // later lanes waiting for the same commit join this one
    size_t joined = 0;
    for (size_t i = lanes.size(); i-- > col + 1;) if (lanes[i] == x) { lanes.erase(lanes.begin() + (long)i); ++joined; }
    if (joined && graph) {
      std::string t;
      for (size_t i = 0; i < lanes.size(); ++i) t += i == col ? "|/" : "| ";
      c.out << t << "\n";
    }
    char sym = x == wc ? '@' : !g.header("conflict-sides").empty() ? 'x' : is_immutable(c, x) ? '#' : 'o';
    std::string prefix, cont;
    for (size_t i = 0; i < lanes.size(); ++i) { prefix += (i == col ? sym : '|'); prefix += ' '; }
    std::string who = g.author.substr(g.author.find('<') == std::string::npos ? 0 : g.author.find('<') + 1);
    if (!who.empty() && who.back() == '>') who.pop_back();
    std::string line1 = short_id(change_id_of(g)) + " " + who + " " + format_date((int64_t)g.committer_time, g.committer_tz, "short") + " " +
                        x.substr(0, 8);
    for (auto& [name, oid] : all_refs(c)) {
      if (oid != x) continue;
      if (name.rfind("refs/heads/", 0) == 0) line1 += " " + name.substr(11);
      else if (name.rfind("refs/remotes/", 0) == 0) line1 += " " + name.substr(13);
      else if (name.rfind("refs/tags/", 0) == 0) line1 += " tag:" + name.substr(10);
    }
    if (!g.header("conflict-sides").empty()) line1 += " conflict";
    std::string subj = subject_of(g.raw_message);
    std::string line2 = (is_empty_change(c, g) ? "(empty) " : "") + (subj.empty() ? std::string("(no description set)") : subj);
    // what the lanes become below this commit
    std::vector<std::string> parents;
    for (auto& par : g.parents) if (in.count(par)) parents.push_back(par);
    std::vector<std::string> next = lanes;
    if (parents.empty()) next.erase(next.begin() + (long)col);
    else {
      next[col] = parents[0];
      size_t at = col + 1;
      for (size_t k = 1; k < parents.size(); ++k)
        if (std::find(next.begin(), next.end(), parents[k]) == next.end()) next.insert(next.begin() + (long)(at++), parents[k]);
    }
    for (size_t i = 0; i < next.size(); ++i) cont += "| ";
    if (cont.empty()) cont = "  ";
    if (graph) {
      c.out << prefix << " " << line1 << "\n";
      std::string mid;
      for (size_t i = 0; i < lanes.size(); ++i) mid += "| ";
      c.out << mid << " " << line2 << "\n";
      if (next.size() > lanes.size()) {
        std::string t;
        for (size_t i = 0; i < lanes.size(); ++i) t += i == col ? "|\\" : "| ";
        c.out << t << "\n";
      } else if (next.size() < lanes.size() && col < next.size()) {
        std::string t;
        for (size_t i = 0; i < next.size(); ++i) t += i == col ? "|/" : "| ";
        c.out << t << "\n";
      }
    } else {
      c.out << (x == wc ? "@ " : "  ") << line1 << "\n    " << line2 << "\n";
    }
    lanes = next;
  }
  if (!p.has("revisions") && !p.has("all") && (long long)shown.size() >= limit) c.out << "(more: -n <count> or --all)\n";
  return 0;
}

// git new [-m MSG] [-A REV | -B REV] [REV...]: a new, empty change on top of REV (default @), or on several (a merge
// that always completes), made the working copy.
int change_new(Ctx& c, size_t start) {
  auto p = parse_options(c, c.args, start, {{'m', "message", Arg::Required}, {'A', "insert-after", Arg::Required},
                                             {'B', "insert-before", Arg::Required}, {0, "no-edit"}});
  open_changes(c);
  Tx tx(c, "new empty change");
  std::vector<std::string> parents;
  std::string before;
  if (p.has("insert-after")) parents = {resolve_rev(c, p.get("insert-after"))};
  else if (p.has("insert-before")) { before = resolve_rev(c, p.get("insert-before")); parents = cm(c, before).parents; }
  else {
    std::vector<std::string> revs = p.pos;
    if (revs.empty()) revs.push_back("@");
    for (auto& r : revs) for (auto& o : resolve_revs(c, r)) if (std::find(parents.begin(), parents.end(), o) == parents.end()) parents.push_back(o);
  }
  CommitSpec s;
  s.parents = parents;
  MergedTree pt = parents_tree(c, parents);
  s.tree = pt.tree;
  s.conflict_sides = write_conflicts(c, pt.conflicts);
  s.message = message_from(p);
  std::string fresh = write_commit(c, s);
  tx.add_head(fresh);
  if (p.has("insert-after")) {
    for (auto& ch : children_of(c, parents[0])) {
      if (ch == fresh) continue;
      GitCommit g = cm(c, ch);
      std::vector<std::string> np;
      for (auto& q : g.parents) np.push_back(q == parents[0] ? fresh : q);
      tx.rewrite(ch, rebase_commit(c, g, np));
    }
  }
  if (!before.empty()) tx.rewrite(before, rebase_commit(c, cm(c, before), {fresh}));
  if (!p.has("no-edit")) tx.set_wc(fresh);
  // the working copy it leaves stays a change of its own, unless it was nothing at all
  std::string old = wc_commit(c);
  GitCommit og = cm(c, old);
  if (!p.has("no-edit") && is_empty_change(c, og) && og.raw_message.empty() && !children_of(c, old).size() &&
      std::find(parents.begin(), parents.end(), old) == parents.end() && !is_immutable(c, old))
    tx.abandon(old);
  size_t n = tx.finish();
  c.out << "Working copy now at: " << wc_line(c) << "\n";
  if (!pt.conflicts.empty()) {
    c.out << "The merge left " << pt.conflicts.size() << " conflicted path" << (pt.conflicts.size() == 1 ? "" : "s")
          << " in the change (recorded in it; resolve them as an edit):\n";
    for (auto& [path, k] : pt.conflicts) c.out << "  " << path << "  " << k.kind << "\n";
  }
  say_rebased(c, n);
  return 0;
}

int change_describe(Ctx& c, size_t start) {
  auto p = parse_options(c, c.args, start, {{'r', "revision", Arg::Required}, {'m', "message", Arg::Required}, {0, "stdin"},
                                             {0, "reset-author"}, {0, "ignore-immutable"}});
  open_changes(c);
  if (!p.has("message") && !p.has("stdin")) usage(c, "git change describe [-r REV] -m <message> (or --stdin)");
  std::vector<std::string> targets;
  if (p.has("revision")) for (auto& r : p.all("revision")) targets.push_back(resolve_rev(c, r));
  for (auto& r : p.pos) targets.push_back(resolve_rev(c, r));
  if (targets.empty()) targets.push_back(wc_commit(c));
  std::string msg = p.has("stdin") ? cleanup_message(read_all(c.in), true) : message_from(p);
  Tx tx(c, "describe " + short_id(change_id_of(cm(c, targets[0]))));
  tx.allow_immutable = p.has("ignore-immutable");
  for (auto& t : targets) {
    CommitSpec s = spec_of(c, cm(c, t));
    s.message = msg;
    if (p.has("reset-author")) s.author.clear();
    tx.rewrite(t, write_commit(c, s));
  }
  size_t n = tx.finish();
  c.out << "Working copy now at: " << wc_line(c) << "\n";
  say_rebased(c, n);
  return 0;
}

// git change commit [-m MSG] [paths]: what @ holds (or the named paths of it) becomes a described change, and a new
// empty @ starts on top. A branch that pointed at @'s parent moves up to the committed change, as git commit moves it.
int change_commit(Ctx& c, size_t start) {
  auto p = parse_options(c, c.args, start, {{'m', "message", Arg::Required}, {'a', "all"}, {'q', "quiet"}, {0, "allow-empty"}});
  open_changes(c);
  std::string wc = wc_commit(c);
  GitCommit w = cm(c, wc);
  std::string msg = p.has("message") ? message_from(p) : w.raw_message;
  if (msg.empty()) die(c, "the change has no description: git change commit -m <message>");
  std::vector<std::string> paths = p.pos;
  paths.insert(paths.end(), p.after_dashdash.begin(), p.after_dashdash.end());
  Tx tx(c, "commit " + short_id(change_id_of(w)));
  std::string done, fresh;
  if (paths.empty()) {
    CommitSpec s = spec_of(c, w);
    s.message = msg;
    done = write_commit(c, s);
    CommitSpec n;
    n.parents = {done};
    n.tree = w.tree;
    n.conflict_sides = s.conflict_sides;
    fresh = write_commit(c, n);
  } else {
    Pathspec ps = make_pathspec(c, paths);
    MergedTree pt = parents_tree(c, w.parents);
    CommitSpec s = spec_of(c, w);
    s.message = msg;
    s.tree = tree_with_paths(c, pt.tree, w.tree, ps);
    s.conflict_sides = write_conflicts(c, still_conflicted(c, s.tree, read_conflicts(c, w)));
    done = write_commit(c, s);
    CommitSpec n;
    n.parents = {done};
    n.tree = w.tree;
    n.conflict_sides = write_conflicts(c, still_conflicted(c, w.tree, read_conflicts(c, w)));
    fresh = write_commit(c, n);
  }
  // branches at @'s parent advance, as git commit moves the branch it is on; not one a remote has too (main beside
  // origin/main): work on top of a remote's branch is a stack for review (git stack push), not a move of that branch
  std::set<std::string> remote_names;
  for (auto& [name, oid] : all_refs(c))
    if (name.rfind("refs/remotes/", 0) == 0) {
      size_t sl = name.find('/', 13);
      if (sl != std::string::npos) remote_names.insert(name.substr(sl + 1));
    }
  for (auto& [name, oid] : all_refs(c)) {
    if (name.rfind("refs/heads/", 0) != 0) continue;
    if (remote_names.count(name.substr(11))) continue;
    if (std::find(w.parents.begin(), w.parents.end(), oid) == w.parents.end()) continue;
    if (auto r = c.repo->update_ref(name, done); !r.ok()) die(c, r.error().message);
    append_reflog(c, name, oid, done, "commit: " + subject_of(msg));
  }
  if (w.parents.empty()) {
    if (auto b = c.config.get("change.branch"); b && !b->empty() && !c.repo->ref_oid("refs/heads/" + *b).ok()) {
      (void)c.repo->update_ref("refs/heads/" + *b, done);
      append_reflog(c, "refs/heads/" + *b, "", done, "commit (initial): " + subject_of(msg));
    }
  }
  tx.rewrite(wc, done);
  tx.add_head(fresh);
  tx.set_wc(fresh);
  tx.finish();
  if (!p.has("quiet")) {
    c.out << "Committed: " << describe_line(c, cm(c, done)) << "\n";
    c.out << "Working copy now at: " << wc_line(c) << "\n";
  }
  return 0;
}

int change_edit(Ctx& c, size_t start) {
  auto p = parse_options(c, c.args, start, {{0, "ignore-immutable"}});
  open_changes(c);
  if (p.pos.size() != 1) usage(c, "git change edit <revision>");
  std::string target = resolve_rev(c, p.pos[0]);
  if (!p.has("ignore-immutable") && is_immutable(c, target)) die(c, "commit " + c.abbrev(target) + " is immutable: 'git new " + p.pos[0] + "' starts a change on top of it");
  std::string old = wc_commit(c);
  if (target == old) { c.out << "Already editing that change.\n"; return 0; }
  Tx tx(c, "edit " + short_id(change_id_of(cm(c, target))));
  tx.add_head(target);
  tx.set_wc(target);
  GitCommit og = cm(c, old);
  if (is_empty_change(c, og) && og.raw_message.empty() && children_of(c, old).empty() && !is_ancestor(c, old, target)) tx.abandon(old);
  tx.finish();
  c.out << "Working copy now at: " << wc_line(c) << "\n";
  return 0;
}

int change_abandon(Ctx& c, size_t start) {
  auto p = parse_options(c, c.args, start, {{'r', "revision", Arg::Required}, {0, "ignore-immutable"}});
  open_changes(c);
  std::vector<std::string> targets;
  for (auto& r : p.all("revision")) for (auto& o : resolve_revs(c, r)) targets.push_back(o);
  for (auto& r : p.pos) for (auto& o : resolve_revs(c, r)) targets.push_back(o);
  if (targets.empty()) targets.push_back(wc_commit(c));
  Tx tx(c, "abandon " + std::to_string(targets.size()) + " change" + (targets.size() == 1 ? "" : "s"));
  tx.allow_immutable = p.has("ignore-immutable");
  for (auto& t : targets) { c.out << "Abandoned: " << describe_line(c, cm(c, t)) << "\n"; tx.abandon(t); }
  size_t n = tx.finish();
  say_rebased(c, n);
  c.out << "Working copy now at: " << wc_line(c) << "\n";
  return 0;
}

int change_squash(Ctx& c, size_t start) {
  auto p = parse_options(c, c.args, start, {{'r', "revision", Arg::Required}, {'f', "from", Arg::Required}, {'t', "into", Arg::Required},
                                             {0, "to", Arg::Required}, {'m', "message", Arg::Required}, {0, "ignore-immutable"},
                                             {'k', "keep-emptied"}});
  open_changes(c);
  std::string src = resolve_rev(c, p.has("revision") ? p.get("revision") : p.has("from") ? p.get("from") : "@");
  GitCommit s = cm(c, src);
  std::string dst;
  if (p.has("into") || p.has("to")) dst = resolve_rev(c, p.has("into") ? p.get("into") : p.get("to"));
  else {
    if (s.parents.size() != 1) die(c, "the change has " + std::to_string(s.parents.size()) + " parents: name where to squash it with --into");
    dst = s.parents[0];
  }
  if (dst == src) die(c, "cannot squash a change into itself");
  std::vector<std::string> paths = p.pos;
  paths.insert(paths.end(), p.after_dashdash.begin(), p.after_dashdash.end());
  Pathspec ps = make_pathspec(c, paths);
  MergedTree sp = parents_tree(c, s.parents);
  std::string moved = paths.empty() ? s.tree : tree_with_paths(c, sp.tree, s.tree, ps);   // parent + the moved part
  std::string left = paths.empty() ? sp.tree : tree_with_paths(c, s.tree, sp.tree, ps);   // what stays in the source
  GitCommit d = cm(c, dst);
  Tx tx(c, "squash " + short_id(change_id_of(s)) + " into " + short_id(change_id_of(d)));
  tx.allow_immutable = p.has("ignore-immutable");
  MergedTree into = merge_trees(c, sp.tree, d.tree, moved, short_id(change_id_of(d)), short_id(change_id_of(s)));
  CommitSpec ds = spec_of(c, d);
  ds.tree = into.tree;
  auto carried = read_conflicts(c, d);
  for (auto& [k, v] : into.conflicts) carried[k] = v;
  ds.conflict_sides = write_conflicts(c, still_conflicted(c, into.tree, carried));
  bool emptied = left == sp.tree;
  bool abandon_src = emptied && !p.has("keep-emptied") && src != wc_commit(c);
  if (p.has("message")) ds.message = message_from(p);
  else if (!s.raw_message.empty() && d.raw_message.empty()) ds.message = s.raw_message;
  else if (!s.raw_message.empty() && abandon_src) ds.message = d.raw_message + (d.raw_message.empty() || d.raw_message.back() == '\n' ? "" : "\n") + "\n" + s.raw_message;
  std::string dnew = write_commit(c, ds);
  tx.rewrite(dst, dnew);
  if (abandon_src) {
    tx.abandon(src);
  } else {
    CommitSpec ss = spec_of(c, s);
    ss.tree = left;
    ss.conflict_sides = write_conflicts(c, still_conflicted(c, left, read_conflicts(c, s)));
    if (emptied && src == wc_commit(c) && !p.has("message") && ds.message == s.raw_message) ss.message.clear();
    std::string snew = write_commit(c, ss);
    // the source sits on the destination (or below it): put it on the new version
    auto np = tx.new_parents(cm(c, snew).parents);
    tx.rewrite(src, np == cm(c, snew).parents ? snew : rebase_commit(c, cm(c, snew), np));
  }
  // a destination that descends from the source goes onto the source's new version
  {
    GitCommit dn = cm(c, dnew);
    auto np = tx.new_parents(dn.parents);
    if (np != dn.parents) tx.rewrite(dst, rebase_commit(c, dn, np));
  }
  size_t n = tx.finish();
  say_rebased(c, n);
  c.out << "Working copy now at: " << wc_line(c) << "\n";
  return 0;
}

int change_split(Ctx& c, size_t start) {
  auto p = parse_options(c, c.args, start, {{'r', "revision", Arg::Required}, {'m', "message", Arg::Required}, {0, "ignore-immutable"}});
  open_changes(c);
  std::vector<std::string> paths = p.pos;
  paths.insert(paths.end(), p.after_dashdash.begin(), p.after_dashdash.end());
  if (paths.empty()) usage(c, "git change split [-r REV] [-m MSG] <path>...: the named paths become the first change, the rest the second");
  std::string src = resolve_rev(c, p.has("revision") ? p.get("revision") : "@");
  GitCommit s = cm(c, src);
  Pathspec ps = make_pathspec(c, paths);
  MergedTree sp = parents_tree(c, s.parents);
  Tx tx(c, "split " + short_id(change_id_of(s)));
  tx.allow_immutable = p.has("ignore-immutable");
  CommitSpec a = spec_of(c, s);
  a.tree = tree_with_paths(c, sp.tree, s.tree, ps);
  if (p.has("message")) a.message = message_from(p);
  a.conflict_sides = write_conflicts(c, still_conflicted(c, a.tree, read_conflicts(c, s)));
  std::string first = write_commit(c, a);
  CommitSpec b = spec_of(c, s);
  b.change_id.clear();
  b.parents = {first};
  if (!p.has("message")) b.message.clear();
  std::string second = write_commit(c, b);
  tx.rewrite(src, second);
  size_t n = tx.finish();
  c.out << "First part:  " << describe_line(c, cm(c, first)) << "\n";
  c.out << "Second part: " << describe_line(c, cm(c, second)) << "\n";
  say_rebased(c, n);
  return 0;
}

int change_rebase(Ctx& c, size_t start) {
  auto p = parse_options(c, c.args, start, {{'r', "revision", Arg::Required}, {'s', "source", Arg::Required}, {'b', "branch", Arg::Required},
                                             {'d', "destination", Arg::Required}, {'o', "onto", Arg::Required}, {0, "ignore-immutable"}});
  open_changes(c);
  std::vector<std::string> dests;
  for (auto& d : p.all("destination")) for (auto& o : resolve_revs(c, d)) dests.push_back(o);
  for (auto& d : p.all("onto")) for (auto& o : resolve_revs(c, d)) dests.push_back(o);
  if (dests.empty()) usage(c, "git change rebase (-r REV | -s REV | -b REV) -d DEST");
  Tx tx(c, "rebase");
  tx.allow_immutable = p.has("ignore-immutable");
  auto check_cycle = [&](const std::string& x) {
    for (auto& d : dests) if (is_ancestor(c, x, d)) die(c, "cannot rebase " + c.abbrev(x) + " onto its own descendant " + c.abbrev(d));
  };
  std::vector<std::string> roots;
  if (p.has("revision")) {
    for (auto& r : p.all("revision")) {
      std::string x = resolve_rev(c, r);
      for (auto& d : dests) if (d == x) die(c, "cannot rebase a change onto itself");
      tx.move_out(x, rebase_commit(c, cm(c, x), dests));
    }
  } else {
    if (p.has("source")) for (auto& r : p.all("source")) roots.push_back(resolve_rev(c, r));
    std::vector<std::string> branches;
    if (p.has("branch")) for (auto& r : p.all("branch")) branches.push_back(resolve_rev(c, r));
    if (!p.has("source") && !p.has("branch")) branches.push_back(wc_commit(c));
    for (auto& b : branches) {
      // the roots of what b has and the destination does not
      std::set<std::string> only;
      std::vector<std::string> stack{b};
      while (!stack.empty()) {
        std::string x = stack.back();
        stack.pop_back();
        if (only.count(x)) continue;
        bool shared = false;
        for (auto& d : dests) if (is_ancestor(c, x, d)) shared = true;
        if (shared) continue;
        only.insert(x);
        for (auto& q : cm(c, x).parents) stack.push_back(q);
      }
      for (auto& x : only) {
        bool root = true;
        for (auto& q : cm(c, x).parents) if (only.count(q)) root = false;
        if (root) roots.push_back(x);
      }
    }
    for (auto& r : roots) {
      check_cycle(r);
      GitCommit g = cm(c, r);
      // keep the parents that are not part of what moves (the others come along by themselves)
      tx.rewrite(r, rebase_commit(c, g, dests));
    }
  }
  size_t n = tx.finish();
  c.out << "Rebased " << (roots.empty() ? p.all("revision").size() : roots.size()) << " change" << ((roots.size() == 1 || (roots.empty() && p.all("revision").size() == 1)) ? "" : "s");
  if (n) c.out << " and " << n << " descendant" << (n == 1 ? "" : "s");
  c.out << ".\nWorking copy now at: " << wc_line(c) << "\n";
  return 0;
}

int change_restore(Ctx& c, size_t start) {
  auto p = parse_options(c, c.args, start, {{0, "from", Arg::Required}, {0, "to", Arg::Required}, {'r', "revision", Arg::Required},
                                             {0, "ignore-immutable"}});
  open_changes(c);
  std::string to = resolve_rev(c, p.has("to") ? p.get("to") : p.has("revision") ? p.get("revision") : "@");
  GitCommit t = cm(c, to);
  std::string from_tree = p.has("from") ? cm(c, resolve_rev(c, p.get("from"))).tree : parents_tree(c, t.parents).tree;
  std::vector<std::string> paths = p.pos;
  paths.insert(paths.end(), p.after_dashdash.begin(), p.after_dashdash.end());
  Pathspec ps = make_pathspec(c, paths);
  std::string tree = paths.empty() ? from_tree : tree_with_paths(c, t.tree, from_tree, ps);
  if (tree == t.tree) { c.out << "Nothing changed.\n"; return 0; }
  Tx tx(c, "restore into " + short_id(change_id_of(t)));
  tx.allow_immutable = p.has("ignore-immutable");
  CommitSpec s = spec_of(c, t);
  s.tree = tree;
  s.conflict_sides = write_conflicts(c, still_conflicted(c, tree, read_conflicts(c, t)));
  tx.rewrite(to, write_commit(c, s));
  size_t n = tx.finish();
  c.out << "Restored. Working copy now at: " << wc_line(c) << "\n";
  say_rebased(c, n);
  return 0;
}

int change_diff(Ctx& c, size_t start, bool show) {
  DiffOptions o;
  bool any = false;
  auto rest = take_diff_options(c, sub_args(c, start), o, &any);
  std::vector<std::string> args{"x"};
  args.insert(args.end(), rest.begin(), rest.end());
  auto p = parse_options(c, args, 1, {{'r', "revision", Arg::Required}, {0, "from", Arg::Required}, {0, "to", Arg::Required}});
  open_changes(c);
  std::string a_tree, b_tree;
  std::vector<std::string> paths = p.after_dashdash;
  std::string rev;
  if (show) {
    rev = p.has("revision") ? p.get("revision") : !p.pos.empty() ? p.pos[0] : "@";
    if (!p.pos.empty() && !p.has("revision")) paths.insert(paths.end(), p.pos.begin() + 1, p.pos.end());
    else paths.insert(paths.end(), p.pos.begin(), p.pos.end());
  } else {
    paths.insert(paths.end(), p.pos.begin(), p.pos.end());
  }
  if (p.has("from") || p.has("to")) {
    a_tree = p.has("from") ? cm(c, resolve_rev(c, p.get("from"))).tree : cm(c, wc_commit(c)).tree;
    b_tree = p.has("to") ? cm(c, resolve_rev(c, p.get("to"))).tree : cm(c, wc_commit(c)).tree;
  } else {
    std::string x = resolve_rev(c, show ? rev : p.has("revision") ? p.get("revision") : "@");
    GitCommit g = cm(c, x);
    if (show) {
      c.out << "Change ID: " << change_id_of(g) << "\nCommit ID: " << g.oid << "\nAuthor   : " << g.author << " ("
            << format_date((int64_t)g.author_time, g.author_tz, "default") << ")\n";
      auto cs = read_conflicts(c, g);
      if (!cs.empty()) {
        c.out << "Conflicts:";
        for (auto& [path, k] : cs) c.out << " " << path << " (" << k.kind << ")";
        c.out << "\n";
      }
      c.out << "\n";
      std::string msg = g.raw_message.empty() ? "(no description set)\n" : g.raw_message;
      std::istringstream ss(msg);
      std::string line;
      while (std::getline(ss, line)) c.out << "    " << line << "\n";
      c.out << "\n";
    }
    a_tree = parents_tree(c, g.parents).tree;
    b_tree = g.tree;
  }
  auto pairs = diff_trees(c, a_tree, b_tree, make_pathspec(c, paths), o);
  print_diff(c, pairs, o, c.out);
  return 0;
}

int change_resolve(Ctx& c, size_t start) {
  auto p = parse_options(c, c.args, start, {{'r', "revision", Arg::Required}, {'l', "list"}, {0, "ours"}, {0, "theirs"}, {0, "base"}});
  open_changes(c);
  std::string x = resolve_rev(c, p.has("revision") ? p.get("revision") : "@");
  GitCommit g = cm(c, x);
  Conflicts cs = read_conflicts(c, g);
  std::vector<std::string> paths = p.pos;
  paths.insert(paths.end(), p.after_dashdash.begin(), p.after_dashdash.end());
  Pathspec ps = make_pathspec(c, paths);
  int side = p.has("ours") ? 1 : p.has("theirs") ? 2 : p.has("base") ? 3 : 0;
  if (cs.empty()) { c.out << "No conflicts in " << describe_line(c, g) << "\n"; return 0; }
  if (side == 0 || p.has("list")) {
    for (auto& [path, k] : cs) {
      if (!ps.match(path)) continue;
      c.out << path << "    " << k.kind << "  ours: " << (k.ours.present ? k.ours_label : "(deleted)") << "  theirs: "
            << (k.theirs.present ? k.theirs_label : "(deleted)") << "\n";
    }
    return 0;
  }
  Snapshot snap = snapshot_tree(c, g.tree);
  size_t done = 0;
  for (auto& [path, k] : cs) {
    if (!ps.match(path)) continue;
    const Side& pick = side == 1 ? k.ours : side == 2 ? k.theirs : k.base;
    if (pick.present) snap[path] = FileState{pick.oid, pick.mode, true};
    else snap.erase(path);
    ++done;
  }
  if (!done) die(c, "no conflicted path matches");
  std::vector<GitIndexEntry> ents;
  for (auto& [path, s] : snap) ents.push_back(GitIndexEntry{path, s.oid, s.mode});
  std::string tree = write_tree_of(c, ents);
  CommitSpec s = spec_of(c, g);
  s.tree = tree;
  s.conflict_sides = write_conflicts(c, still_conflicted(c, tree, cs));
  Tx tx(c, "resolve " + std::to_string(done) + " conflict" + (done == 1 ? "" : "s") + " in " + short_id(change_id_of(g)));
  tx.rewrite(x, write_commit(c, s));
  size_t n = tx.finish();
  c.out << "Resolved " << done << " path" << (done == 1 ? "" : "s") << ". " << describe_line(c, cm(c, tx.mapped(x))) << "\n";
  say_rebased(c, n);
  return 0;
}

int change_evolog(Ctx& c, size_t start) {
  auto p = parse_options(c, c.args, start, {{'r', "revision", Arg::Required}});
  open_changes(c);
  std::string x = resolve_rev(c, p.has("revision") ? p.get("revision") : !p.pos.empty() ? p.pos[0] : "@");
  std::map<std::string, std::pair<std::string, std::string>> prev;   // new -> (old, operation description)
  auto h = oplog::head_op(c);
  std::string cur = h ? h->id : "";
  while (!cur.empty()) {
    oplog::Op op = oplog::read_op(c, cur);
    for (auto& [o, n] : op.rewrites) if (!n.empty() && !prev.count(n)) prev[n] = {o, op.description};
    cur = op.parent;
  }
  std::set<std::string> seen;
  std::string why;
  while (!x.empty() && seen.insert(x).second) {
    c.out << (why.empty() ? "@ " : "o ") << describe_line(c, cm(c, x)) << "\n";
    auto it = prev.find(x);
    if (it == prev.end()) break;
    c.out << "|   (" << it->second.second << ")\n";
    x = it->second.first;
    why = it->second.second;
  }
  return 0;
}

int change_bookmark(Ctx& c, size_t start) {
  std::string sub = c.args.size() > start ? c.args[start] : "list";
  auto p = parse_options(c, c.args, start + 1, {{'r', "revision", Arg::Required}, {0, "to", Arg::Required}, {'B', "allow-backwards"}});
  open_changes(c);
  if (sub == "list" || sub == "l") {
    for (auto& [name, oid] : all_refs(c))
      if (name.rfind("refs/heads/", 0) == 0) c.out << name.substr(11) << ": " << describe_line(c, cm(c, oid)) << "\n";
    return 0;
  }
  if (p.pos.empty()) usage(c, "git change bookmark (list | set|create|move <name> [-r REV] | delete <name>)");
  if (sub == "delete" || sub == "d") {
    for (auto& n : p.pos) {
      auto r = c.repo->ref_oid("refs/heads/" + n);
      if (!r.ok()) die(c, "no bookmark " + n);
      (void)c.repo->delete_ref("refs/heads/" + n);
    }
    oplog::record(c, "delete bookmark " + p.pos[0], "normal", "", {});
    return 0;
  }
  if (sub == "set" || sub == "create" || sub == "move" || sub == "s" || sub == "c" || sub == "m") {
    std::string to = resolve_rev(c, p.has("to") ? p.get("to") : p.has("revision") ? p.get("revision") : "@");
    for (auto& n : p.pos) {
      std::string ref = "refs/heads/" + n;
      if (!Git::valid_ref_name(ref)) die(c, "'" + n + "' is not a valid bookmark name");
      auto cur = c.repo->ref_oid(ref);
      if ((sub == "create" || sub == "c") && cur.ok()) die(c, "bookmark " + n + " already exists");
      if ((sub == "move" || sub == "m") && !cur.ok()) die(c, "no bookmark " + n);
      if (cur.ok() && !p.has("allow-backwards") && !is_ancestor(c, *cur, to))
        die(c, "moving bookmark " + n + " backwards or sideways: --allow-backwards (-B) does it anyway");
      if (auto r = c.repo->update_ref(ref, to); !r.ok()) die(c, r.error().message);
      append_reflog(c, ref, cur.ok() ? *cur : "", to, "bookmark: set to " + c.abbrev(to));
    }
    oplog::record(c, "point bookmark " + p.pos[0] + " to " + c.abbrev(to), "normal", "", {});
    c.out << "Bookmark " << p.pos[0] << " now at " << describe_line(c, cm(c, to)) << "\n";
    return 0;
  }
  usage(c, "git change bookmark (list | set|create|move <name> [-r REV] | delete <name>)");
}

int change_track(Ctx& c, size_t start) {
  open_changes(c);
  auto paths = sub_args(c, start);
  if (paths.empty()) usage(c, "git change track <path>...");
  std::map<std::string, GitIndexEntry> idx;
  for (auto& e : c.repo->index()) idx[e.path] = e;
  for (auto& u : paths) {
    std::string rp = c.to_repo_path(u);
    auto s = c.repo->stage_entry(rp, idx.count(rp) ? &idx[rp] : nullptr);
    if (!s.ok()) die(c, "cannot track " + u + ": " + s.error().message);
    idx[rp] = *s;
  }
  std::vector<GitIndexEntry> next;
  for (auto& [k, e] : idx) next.push_back(e);
  set_wc_index(c, next, "track " + paths[0]);
  c.out << "Working copy now at: " << wc_line(c) << "\n";
  return 0;
}

}  // namespace

int cmd_new(Ctx& c) { return change_new(c, 1); }

int cmd_change(Ctx& c) {
  std::string sub = c.args.size() > 1 ? c.args[1] : "status";
  if (sub == "init" || sub == "enable") { change::enable(c); return 0; }
  if (sub == "disable" || sub == "off") {
    std::string branch;
    for (size_t i = 2; i < c.args.size(); ++i) {
      if (c.args[i] == "--branch" && i + 1 < c.args.size()) branch = c.args[++i];
      else if (c.args[i].rfind("--branch=", 0) == 0) branch = c.args[i].substr(9);
      else usage(c, "usage: git change disable [--branch NAME]");
    }
    change::disable(c, branch);
    return 0;
  }
  if (sub == "status" || sub == "st") return change_status(c, 2);
  if (sub == "log") return change_log(c, 2);
  if (sub == "new") return change_new(c, 2);
  if (sub == "describe" || sub == "desc") return change_describe(c, 2);
  if (sub == "commit" || sub == "ci") return change_commit(c, 2);
  if (sub == "edit") return change_edit(c, 2);
  if (sub == "abandon") return change_abandon(c, 2);
  if (sub == "squash") return change_squash(c, 2);
  if (sub == "split") return change_split(c, 2);
  if (sub == "rebase") return change_rebase(c, 2);
  if (sub == "restore") return change_restore(c, 2);
  if (sub == "diff") return change_diff(c, 2, false);
  if (sub == "show") return change_diff(c, 2, true);
  if (sub == "resolve") return change_resolve(c, 2);
  if (sub == "evolog" || sub == "obslog") return change_evolog(c, 2);
  if (sub == "bookmark" || sub == "b") return change_bookmark(c, 2);
  if (sub == "track") return change_track(c, 2);
  if (sub == "id") {
    open_changes(c);
    c.out << change_id_of(read_commit(c, resolve_rev(c, c.args.size() > 2 ? c.args[2] : "@"))) << "\n";
    return 0;
  }
  if (sub == "snapshot") {
    // --auto: the fsmonitor daemon's, after edits settle; nothing to do (and nothing said) outside the change layer
    bool automatic = c.args.size() > 2 && c.args[2] == "--auto";
    if (automatic) {
      c.open_repo();
      if (!change::enabled(c) || change::wc_commit(c).empty() || !c.config.get_bool("change.autosnapshot", true)) return 0;
      // lock-free when it can be; otherwise the lock, without waiting (the daemon tries again later)
      if (change::snapshot_unlocked(c)) return 0;
      if (!take_repo_lock(c, false)) return 75;
    }
    open_changes(c);
    return 0;
  }
  usage(c, "usage: git change <command>\n"
           "  init                          turn the change layer on: the working tree becomes the change @\n"
           "  disable [--branch NAME]       turn it off: HEAD back on the branch, @'s edits left uncommitted\n"
           "  status | log | show | diff     look at changes (@ the working copy, @- its parent, change ids)\n"
           "  new [-m] [REV...]             start a new change on REV (several: a merge that always completes)\n"
           "  describe -m | commit -m       describe a change; describe @ and start a new one on top\n"
           "  edit | abandon | squash | split | restore | rebase -r/-s/-b -d   rewrite (descendants follow)\n"
           "  resolve [--ours|--theirs]     conflicts recorded in a change\n"
           "  evolog | bookmark | track | id");
}

namespace change {
int status_of_workspace(Ctx& c) {
  if (!ensure_wc(c)) return -1;
  // the files as they are now, without the repository lock (git status takes none): the fsmonitor's snapshot
  snapshot_unlocked(c);
  forget(c);
  c.out << "On workspace " << workspace(c) << " (the change layer records your edits in @ as you save them)\n";
  return print_working_copy(c);
}

int intercept(Ctx& c) {
  if (c.args.empty()) return -1;
  const std::string& cmd = c.args[0];
  if (cmd != "commit" && cmd != "stash" && cmd != "add" && cmd != "merge") return -1;
  // looking at the stashes made before the change layer changes nothing: only making or applying one is refused
  if (cmd == "stash" && c.args.size() > 1 && (c.args[1] == "list" || c.args[1] == "show")) return -1;
  if (!c.try_open_repo() || !enabled(c) || !ensure_wc(c)) { c.repo.reset(); return -1; }
  if (cmd == "stash") {
    c.err << "fatal: the working copy is a change of its own here: there is nothing to stash.\n"
             "hint: 'git new @-' starts another change beside it; 'git change edit <change>' comes back to it.\n";
    return 128;
  }
  if (cmd == "merge") {
    // a merge always completes here: a new change on @ (or, @ being empty, on what @ is on) and what is named
    std::vector<std::string> args{"change", "new"};
    std::string msg;
    std::vector<std::string> revs;
    for (size_t i = 1; i < c.args.size(); ++i) {
      const std::string& b = c.args[i];
      if (b == "-m" || b == "--message") { if (i + 1 < c.args.size()) msg = c.args[++i]; continue; }
      if (b == "--abort" || b == "--continue" || b == "--quit") {
        c.err << "fatal: a merge here never stops part way: there is nothing to " << b.substr(2) << "\n";
        return 128;
      }
      if (!b.empty() && b[0] == '-') continue;   // --no-ff, --no-edit, -q...: nothing to choose here
      revs.push_back(b);
    }
    if (revs.empty()) usage(c, "git merge <revision>...");
    std::string wc = wc_commit(c);
    GitCommit w = read_commit(c, wc);
    if (is_empty_change(c, w) && w.raw_message.empty()) for (auto& p : w.parents) args.push_back(p);
    else args.push_back(wc);
    for (auto& r : revs) args.push_back(r);
    if (msg.empty()) msg = "Merge " + revs[0];
    args.push_back("-m");
    args.push_back(msg);
    c.args = args;
    c.repo.reset();
    return change_new(c, 2);
  }
  if (cmd == "add") {
    // nothing is staged in the change layer: the next command snapshots every file into @
    snapshot(c);
    return 0;
  }
  // git commit -m MSG [paths] (and -a, which changes nothing here) is git change commit
  for (auto& a : c.args) if (a == "--amend") {
    // amend the change below the working copy: what @ holds moves into @- (with a new message, when given)
    std::vector<std::string> args{"change", "squash"};
    for (size_t i = 1; i < c.args.size(); ++i) {
      const std::string& b = c.args[i];
      if (b == "-m" || b == "--message") { if (i + 1 < c.args.size()) { args.push_back("-m"); args.push_back(c.args[++i]); } }
      else if (b.rfind("--message=", 0) == 0) { args.push_back("-m"); args.push_back(b.substr(10)); }
      else if (b.rfind("-m", 0) == 0 && b.size() > 2) { args.push_back("-m"); args.push_back(b.substr(2)); }
    }
    c.args = args;
    c.repo.reset();
    return change_squash(c, 2);
  }
  c.args.insert(c.args.begin(), "change");
  c.repo.reset();
  return change_commit(c, 2);
}
}  // namespace change

}  // namespace aios_git
