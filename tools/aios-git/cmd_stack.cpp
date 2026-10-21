// git stack: the changes between trunk and the working copy, worked on as one unit (the change layer; see change.hpp).
//   git stack [log]            the stack, top first: each change's id, bookmark, description, and state
//   git stack up | down [n]    make the change above / below @ the working copy
//   git stack sync [--onto R]  rebase the whole stack onto trunk (or R): conflicts are recorded, never stop it
//   git stack push [remote]    push every change of the stack as a branch (its bookmark, else push/<change id>),
//                              each with a lease on what the remote had: a stack is reviewed change by change
// Trunk is change.trunk, else origin/main, origin/master, main or master: whichever exists first.
#include <algorithm>
#include <set>

#include "change.hpp"
#include "oplog.hpp"

namespace aios_git {

using namespace change;

namespace {

std::string trunk_of(Ctx& c, std::string* name = nullptr) {
  std::vector<std::string> tries;
  if (auto t = c.config.get("change.trunk")) tries.push_back(*t);
  for (const char* d : {"origin/main", "origin/master", "main", "master"}) tries.push_back(d);
  for (auto& t : tries) {
    auto r = resolve(c, t);
    if (!r) continue;
    std::string cm = peel_to(c, *r, "commit");
    if (cm.empty()) continue;
    if (name) *name = t;
    return cm;
  }
  return "";
}

// The stack: what @ has that trunk does not (bottom first), then what is built on @ in a line.
std::vector<std::string> stack_of(Ctx& c, const std::string& trunk) {
  std::string wc = wc_commit(c);
  std::vector<std::string> below;
  std::set<std::string> seen;
  std::string cur = wc;
  while (!cur.empty() && seen.insert(cur).second) {
    if (!trunk.empty() && is_ancestor(c, cur, trunk)) break;
    below.push_back(cur);
    GitCommit g = read_commit(c, cur);
    if (g.parents.empty()) break;
    cur = g.parents[0];
  }
  std::reverse(below.begin(), below.end());
  // above @: follow a single child at a time among the visible changes
  cur = wc;
  for (int guard = 0; guard < 1000; ++guard) {
    std::vector<std::string> kids;
    for (auto& v : visible_commits(c))
      for (auto& p : read_commit(c, v).parents)
        if (p == cur && std::find(kids.begin(), kids.end(), v) == kids.end()) kids.push_back(v);
    if (kids.size() != 1) break;
    cur = kids[0];
    if (!seen.insert(cur).second) break;
    below.push_back(cur);
  }
  return below;
}

std::vector<std::string> branches_at(Ctx& c, const std::string& oid) {
  std::vector<std::string> out;
  for (auto& [name, o] : all_refs(c))
    if (o == oid && name.rfind("refs/heads/", 0) == 0) out.push_back(name.substr(11));
  return out;
}

int run_change(Ctx& c, std::vector<std::string> args) {
  c.args = std::move(args);
  c.repo.reset();
  forget(c);
  return cmd_change(c);
}

}  // namespace

int cmd_stack(Ctx& c) {
  std::string sub = c.args.size() > 1 ? c.args[1] : "log";
  open_changes(c);
  std::string trunk_name;
  std::string trunk = trunk_of(c, &trunk_name);
  if (sub == "log" || sub == "show" || sub == "status") {
    auto st = stack_of(c, trunk);
    std::string wc = wc_commit(c);
    if (st.empty()) { c.out << "Nothing on top of trunk" << (trunk_name.empty() ? "" : " (" + trunk_name + ")") << ".\n"; return 0; }
    for (size_t i = st.size(); i-- > 0;) {
      GitCommit g = read_commit(c, st[i]);
      std::string mark = st[i] == wc ? "@" : !g.header("conflict-sides").empty() ? "x" : "o";
      std::string subj = subject_of(g.raw_message);
      c.out << mark << " " << (i + 1) << ". " << short_id(change_id_of(g)) << " " << st[i].substr(0, 8);
      for (auto& b : branches_at(c, st[i])) c.out << " " << b;
      c.out << "  " << (subj.empty() ? std::string("(no description set)") : subj);
      if (!g.header("conflict-sides").empty()) c.out << "  (conflict)";
      if (is_empty_change(c, g)) c.out << "  (empty)";
      c.out << "\n";
    }
    if (!trunk_name.empty()) c.out << "  on " << trunk_name << " " << trunk.substr(0, 8) << "\n";
    return 0;
  }
  if (sub == "up" || sub == "down" || sub == "next" || sub == "prev") {
    int n = c.args.size() > 2 ? std::max(1, std::atoi(c.args[2].c_str())) : 1;
    auto st = stack_of(c, trunk);
    std::string wc = wc_commit(c);
    auto it = std::find(st.begin(), st.end(), wc);
    if (it == st.end()) die(c, "the working copy is not in a stack on " + (trunk_name.empty() ? std::string("trunk") : trunk_name));
    long at = (long)(it - st.begin());
    bool up = sub == "up" || sub == "next";
    long to = up ? at + n : at - n;
    // an empty, undescribed @ on top is where new work goes: up from it has nowhere to go, down from it skips it
    if (to < 0) die(c, "already at the bottom of the stack");
    if (to >= (long)st.size()) die(c, "already at the top of the stack");
    return run_change(c, {"change", "edit", st[(size_t)to]});
  }
  if (sub == "sync" || sub == "rebase") {
    std::string onto = trunk;
    for (size_t i = 2; i + 1 < c.args.size(); ++i) if (c.args[i] == "--onto" || c.args[i] == "-d") onto = resolve_rev(c, c.args[i + 1]);
    if (onto.empty()) die(c, "no trunk to sync onto: set change.trunk, or give --onto <revision>");
    auto st = stack_of(c, trunk);
    if (st.empty()) { c.out << "Nothing to sync.\n"; return 0; }
    GitCommit bottom = read_commit(c, st.front());
    if (!bottom.parents.empty() && bottom.parents[0] == onto) { c.out << "The stack is already on " << onto.substr(0, 8) << ".\n"; return 0; }
    return run_change(c, {"change", "rebase", "-s", st.front(), "-d", onto});
  }
  if (sub == "push") {
    std::string remote = c.args.size() > 2 && c.args[2][0] != '-' ? c.args[2] : "origin";
    auto st = stack_of(c, trunk);
    std::vector<std::string> refspecs;
    for (auto& oid : st) {
      GitCommit g = read_commit(c, oid);
      if (oid == wc_commit(c) && is_empty_change(c, g) && g.raw_message.empty()) continue;   // the empty @ on top
      if (g.raw_message.empty()) die(c, "change " + short_id(change_id_of(g)) + " has no description: describe it before pushing");
      if (!g.header("conflict-sides").empty()) die(c, "change " + short_id(change_id_of(g)) + " has unresolved conflicts: resolve them before pushing");
      // the trunk's own branch is never one of the stack's: a stack is pushed for review, not onto trunk
      std::string trunk_branch = trunk_name;
      if (size_t sl = trunk_branch.find('/'); sl != std::string::npos && resolve(c, "refs/remotes/" + trunk_branch)) trunk_branch = trunk_branch.substr(sl + 1);
      std::vector<std::string> names;
      for (auto& b : branches_at(c, oid)) if (b != trunk_branch && b != "main" && b != "master") names.push_back(b);
      std::string name = names.empty() ? "push/" + short_id(change_id_of(g)) : names[0];
      if (names.empty()) {
        // a branch that was this change's before it was rewritten moves with it (same change id, push/<id>)
        if (auto r = c.repo->update_ref("refs/heads/" + name, oid); !r.ok()) die(c, r.error().message);
      }
      std::string lease;
      if (auto tracked = read_ref(c, "refs/remotes/" + remote + "/" + name)) lease = *tracked;
      refspecs.push_back("--force-with-lease=refs/heads/" + name + ":" + lease);
      refspecs.push_back("refs/heads/" + name + ":refs/heads/" + name);
      c.out << name << " <- " << short_id(change_id_of(g)) << " " << subject_of(g.raw_message) << "\n";
    }
    if (refspecs.empty()) { c.out << "Nothing to push.\n"; return 0; }
    oplog::record(c, "name the stack's changes for pushing", "normal", "", {});
    std::vector<std::string> args{"push", remote};
    for (auto& r : refspecs) if (r.rfind("--force-with-lease", 0) == 0) args.push_back(r);
    for (auto& r : refspecs) if (r.rfind("--force-with-lease", 0) != 0) args.push_back(r);
    c.args = args;
    c.repo.reset();
    forget(c);
    return cmd_push(c);
  }
  usage(c, "usage: git stack [log | up [n] | down [n] | sync [--onto <rev>] | push [<remote>]]");
}

}  // namespace aios_git
