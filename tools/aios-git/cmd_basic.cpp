// init, status, add, rm, mv, commit, reset, restore, clean: the work tree and the index.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <functional>
#include <sstream>
#include <optional>
#include <thread>

#include "aios/hal/host.hpp"
#include "change.hpp"
#include "cli_rewrite.hpp"
#include "color.hpp"

namespace aios_git {

using namespace aios;
using namespace aios::userland;

namespace {

std::string head_tree(Ctx& c) {
  auto h = read_ref(c, "HEAD");
  if (!h) return "";
  return read_commit(c, *h).tree;
}

struct StatusItem {
  char x = ' ', y = ' ';
  std::string path, orig;
  uint32_t mh = 0, mi = 0, mw = 0;     // modes in HEAD, the index and the work tree (0: absent)
  std::string hh, hi;                  // object ids in HEAD and the index
  int score = 0;                       // similarity of a rename
  bool unmerged = false;
  uint32_t sm[3] = {0, 0, 0};          // an unmerged path's modes at stages 1, 2, 3
  std::string so[3];
};

const char* unmerged_code(bool b, bool o, bool t) {
  if (b && o && t) return "UU";
  if (!b && o && t) return "AA";
  if (b && !o && t) return "DU";
  if (b && o && !t) return "UD";
  if (!b && o && !t) return "AU";
  if (!b && !o && t) return "UA";
  return "DD";
}

const std::string kNullOid(40, '0');

// A path as status prints it: C-quoted when it holds bytes git escapes (core.quotePath decides about non-ASCII).
std::string qp(Ctx& c, const std::string& path, bool space = false) {
  return quote_path(path, c.config.get_bool("core.quotepath", true), space);
}

// The mode the work tree file has, 0 when there is none (a directory is none either, except for a submodule).
uint32_t worktree_mode(Ctx& c, const std::string& path, uint32_t index_mode) {
  auto st = c.host().stat(join(c.top, path));
  if (!st.ok()) return 0;
  if (st->is_dir && !st->is_link) return index_mode == 0160000 ? 0160000 : 0;
  if (st->is_link && c.repo->settings().symlinks) return 0120000;
  if (c.repo->settings().filemode) return st->is_exec ? 0100755 : 0100644;
  return index_mode == 0100755 ? 0100755 : 0100644;
}

std::vector<StatusItem> tracked_status(Ctx& c, const Pathspec& ps, bool renames) {
  std::map<std::string, StatusItem> m;
  std::map<std::string, int> stages;
  std::map<std::string, std::vector<GitIndexEntry>> conflicts;
  for (auto& u : c.repo->unmerged()) { stages[u.path] |= 1 << u.stage; conflicts[u.path].push_back(u); }
  // Nothing staged when the index hashes to HEAD's own tree (no conflict, nothing added with -N): the tree of
  // HEAD is not read and no paths are compared.
  bool same_as_head = false;
  if (stages.empty()) {
    bool intent_any = false;
    for (auto& e : c.repo->index()) if (e.intent_to_add) { intent_any = true; break; }
    const std::string ht = head_tree(c);
    if (!intent_any && !ht.empty()) {
      auto it = c.repo->index_tree_oid();
      same_as_head = it.ok() && *it == ht;
    }
  }
  Snapshot head = same_as_head ? Snapshot{} : snapshot_tree(c, head_tree(c));
  Snapshot index = same_as_head ? Snapshot{} : snapshot_index(c);
  // the index entry of a path the work tree changed, looked up only when there is one
  std::unordered_map<std::string_view, const GitIndexEntry*> by_path;
  auto entry_of = [&](const std::string& path) -> const GitIndexEntry* {
    if (by_path.empty()) {
      by_path.reserve(c.repo->index().size() * 2);
      for (auto& e : c.repo->index()) by_path.emplace(std::string_view(e.path), &e);
    }
    auto it = by_path.find(std::string_view(path));
    return it == by_path.end() ? nullptr : it->second;
  };
  std::set<std::string> intent;
  for (auto& e : c.repo->index()) {
    if (e.intent_to_add) { intent.insert(e.path); index.erase(e.path); }
  }
  for (auto& [p, st] : stages) head.erase(p);
  DiffOptions o;
  o.renames = renames;
  for (auto& p : diff_snapshots(c, head, index, ps, o)) {
    StatusItem it;
    it.x = p.status;
    it.path = p.status == 'D' ? p.old_path : p.new_path;
    if (p.status == 'R') { it.orig = p.old_path; it.score = p.score; }
    it.mh = p.a.present ? p.a.mode : 0;
    it.mi = p.b.present ? p.b.mode : 0;
    it.hh = p.a.present ? p.a.oid : kNullOid;
    it.hi = p.b.present ? p.b.oid : kNullOid;
    m[it.path] = it;
  }
  auto wt = c.repo->worktree_changes();
  if (!wt.ok()) die(c, wt.error().message);
  for (auto& ch : *wt) {
    if (!ps.match(ch.path) || intent.count(ch.path) || stages.count(ch.path)) continue;
    const GitIndexEntry* e = entry_of(ch.path);
    auto it = m.find(ch.path);
    if (it == m.end()) {
      StatusItem n;
      n.path = ch.path;
      n.mh = n.mi = e->mode;
      n.hh = n.hi = e->oid;
      it = m.emplace(ch.path, n).first;
    }
    if (ch.change == GitChange::Deleted) { it->second.y = 'D'; it->second.mw = 0; continue; }
    it->second.mw = worktree_mode(c, ch.path, e->mode);
    const bool was_link = e->mode == 0120000;
    it->second.y = (it->second.mw == 0120000) != was_link ? 'T' : 'M';
  }
  for (auto& path : intent) {
    if (!ps.match(path)) continue;
    auto it = m.find(path);
    StatusItem n;
    if (it != m.end()) n = it->second;
    else n.hh = kNullOid;
    n.path = path;
    n.y = 'A';
    n.mi = 0;
    n.hi = kNullOid;
    n.mw = worktree_mode(c, path, 0100644);
    if (n.mw == 0) n.y = 'D';
    m[path] = n;
  }
  for (auto& [path, it] : m) {
    if (it.y == ' ') it.mw = it.mi;
  }
  for (auto& [path, list] : conflicts) {
    if (!ps.match(path)) continue;
    int s = stages[path];
    const char* code = unmerged_code(s & 2, s & 4, s & 8);
    StatusItem it;
    it.x = code[0];
    it.y = code[1];
    it.path = path;
    it.unmerged = true;
    for (auto& u : list) {
      it.sm[u.stage - 1] = u.mode;
      it.so[u.stage - 1] = u.oid;
    }
    it.mw = worktree_mode(c, path, 0100644);
    m[path] = it;
  }
  std::vector<StatusItem> out;
  for (auto& [p, it] : m) out.push_back(it);
  return out;
}

void ahead_behind(Ctx& c, const std::string& a, const std::string& b, long& ahead, long& behind) {
  if (a == b) { ahead = behind = 0; return; }   // the same commit: nothing on either side, no walk
  WalkOptions w1;
  w1.tips = {a};
  w1.hide = {b};
  ahead = (long)walk(c, w1).size();
  WalkOptions w2;
  w2.tips = {b};
  w2.hide = {a};
  behind = (long)walk(c, w2).size();
}

std::string short_ref(const std::string& r) {
  for (const char* p : {"refs/heads/", "refs/remotes/", "refs/tags/"})
    if (r.rfind(p, 0) == 0) return r.substr(std::strlen(p));
  return r;
}

size_t stash_count(Ctx& c) {
  auto log = c.host().read_file(c.repo->common_dir() + "/logs/refs/stash");
  if (!log.ok()) return 0;
  return (size_t)std::count(log->begin(), log->end(), (uint8_t)'\n');
}

// What the ignore rules hide, as `status --ignored` lists it: a directory with nothing tracked in it that is
// ignored stands for its contents ("dir/"), or with `collapse` off every file in it is named.
void collect_ignored(Ctx& c, bool collapse, std::vector<std::string>& out) {
  auto& h = c.host();
  std::set<std::string> tracked, tracked_dirs;
  auto note = [&](const GitIndexEntry& e) {
    tracked.insert(e.path);
    for (size_t s = e.path.find('/'); s != std::string::npos; s = e.path.find('/', s + 1)) tracked_dirs.insert(e.path.substr(0, s));
  };
  for (auto& e : c.repo->index()) note(e);
  for (auto& e : c.repo->unmerged()) note(e);
  std::function<size_t(const std::string&, bool, bool)> walk = [&](const std::string& dir, bool inside, bool dry) -> size_t {
    auto names = h.list_dir(dir.empty() ? c.top : join(c.top, dir));
    if (!names.ok()) return 0;
    std::vector<std::string> sorted = *names;
    std::sort(sorted.begin(), sorted.end());
    size_t found = 0;
    for (auto& n : sorted) {
      if (n == "." || n == ".." || n == ".git") continue;
      std::string rel = dir.empty() ? n : dir + "/" + n;
      auto st = h.stat(join(c.top, rel));
      if (!st.ok()) continue;
      if (st->is_dir && !st->is_link) {
        if (inside) { found += walk(rel, true, dry); continue; }
        if (c.repo->is_ignored(rel, true) && !tracked_dirs.count(rel)) {
          if (collapse) {
            if (walk(rel, true, true)) { ++found; if (!dry) out.push_back(rel + "/"); }
          } else {
            found += walk(rel, true, dry);
          }
        } else {
          found += walk(rel, false, dry);
        }
      } else if (!tracked.count(rel) && (inside || c.repo->is_ignored(rel, false))) {
        ++found;
        if (!dry) out.push_back(rel);
      }
    }
    return found;
  };
  walk("", false, false);
}

// wt-status.c's palette (color.status.<slot> over git's defaults), every slot empty when colors are off; a color is
// written with a reset after it only when it is not empty (color_print_strbuf)
struct StatusColors {
  std::string header, updated, changed, untracked, nobranch, unmerged, local, remote;
  std::string paint(const std::string& col, const std::string& text) const { return col.empty() ? text : col + text + kColorReset; }
};
StatusColors status_colors(Ctx& c) {
  StatusColors s;
  if (!c.color) return s;
  auto slot = [&](std::initializer_list<const char*> names, const char* def) -> std::string {
    for (const char* n : names)
      if (auto v = c.config.get(std::string("color.status.") + n))
        if (auto col = parse_git_color(*v)) return *col;
    return def;
  };
  s.header = slot({"header"}, "");
  s.updated = slot({"updated", "added"}, "\033[32m");
  s.changed = slot({"changed"}, "\033[31m");
  s.untracked = slot({"untracked"}, "\033[31m");
  s.nobranch = slot({"nobranch"}, "\033[31m");
  s.unmerged = slot({"unmerged"}, "\033[31m");
  s.local = slot({"localbranch"}, "\033[32m");
  s.remote = slot({"remotebranch"}, "\033[31m");
  return s;
}

void print_tracking(Ctx& c, std::ostream& out) {
  std::string b = current_branch(c);
  if (b.empty()) return;
  std::string up = upstream_of(c, b);
  if (up.empty()) return;
  auto mine = read_ref(c, "refs/heads/" + b);
  auto theirs = read_ref(c, up);
  std::string name = short_ref(up);
  if (!theirs) {
    out << "Your branch is based on '" << name << "', but the upstream is gone.\n"
        << "  (use \"git branch --unset-upstream\" to fixup)\n";
    return;
  }
  if (!mine) return;
  long ahead = 0, behind = 0;
  ahead_behind(c, *mine, *theirs, ahead, behind);
  if (!ahead && !behind) out << "Your branch is up to date with '" << name << "'.\n";
  else if (ahead && !behind)
    out << "Your branch is ahead of '" << name << "' by " << ahead << " commit" << (ahead == 1 ? "" : "s") << ".\n"
        << "  (use \"git push\" to publish your local commits)\n";
  else if (!ahead && behind)
    out << "Your branch is behind '" << name << "' by " << behind << " commit" << (behind == 1 ? "" : "s")
        << ", and can be fast-forwarded.\n  (use \"git pull\" to update your local branch)\n";
  else
    out << "Your branch and '" << name << "' have diverged,\nand have " << ahead << " and " << behind
        << " different commits each, respectively.\n  (use \"git pull\" if you want to integrate the remote branch with yours)\n";
}

void long_status_text(Ctx& c, const std::vector<StatusItem>& items, const std::vector<std::string>& untracked, std::ostream& out,
                      bool for_commit, const std::vector<std::string>* ignored, bool shows_untracked);

// advice.statusHints off: git leaves out every "  (use ...)" line and the parenthesised advice after a verdict.
void long_status(Ctx& c, const std::vector<StatusItem>& items, const std::vector<std::string>& untracked, std::ostream& out,
                 bool for_commit = false, const std::vector<std::string>* ignored = nullptr, bool shows_untracked = true) {
  if (c.config.get_bool("advice.statushints", true)) return long_status_text(c, items, untracked, out, for_commit, ignored, shows_untracked);
  std::ostringstream ss;
  long_status_text(c, items, untracked, ss, for_commit, ignored, shows_untracked);
  const std::string text = ss.str();
  size_t at = 0;
  while (at < text.size()) {
    size_t eol = text.find('\n', at);
    std::string line = text.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
    at = eol == std::string::npos ? text.size() : eol + 1;
    if (line.size() > 3 && line.compare(0, 3, "  (") == 0 && line.back() == ')') continue;
    if (line.rfind("no changes added to commit (", 0) == 0) line = "no changes added to commit";
    else if (line.rfind("nothing added to commit but untracked files present (", 0) == 0) line = "nothing added to commit but untracked files present";
    else if (line.rfind("nothing to commit (", 0) == 0) line = "nothing to commit";
    out << line << "\n";
  }
}

void long_status_text(Ctx& c, const std::vector<StatusItem>& items, const std::vector<std::string>& untracked, std::ostream& out,
                      bool for_commit, const std::vector<std::string>* ignored, bool shows_untracked) {
  // git's layout: a header, then each block (tracking, state, a section) ends with an empty line, then
  // the one-line verdict.
  const StatusColors sc = status_colors(c);
  auto head = read_ref(c, "HEAD");
  std::string b = current_branch(c);
  const bool rebasing = b.empty() && rebase_in_progress(c);
  if (rebasing) out << rebase_status_text(c, false);   // the rebase's own header, its commands and what to do next
  else if (b.empty()) {
    // "HEAD detached at" in the nobranch color, what it is detached at in the header's
    std::string d = head_description(c);
    std::string text = d.rfind("(HEAD detached", 0) == 0 ? d.substr(1, d.size() - 2) : "HEAD detached at " + (head ? c.abbrev(*head) : std::string("HEAD"));
    const size_t cut = text.rfind(' ') + 1;
    out << sc.paint(sc.nobranch, text.substr(0, cut)) << sc.paint(sc.header, text.substr(cut)) << "\n";
  }
  else out << "On branch " << b << "\n";
  if (!head) out << "\n" << (for_commit ? "Initial commit" : "No commits yet") << "\n\n";
  if (head) {
    std::ostringstream t;
    print_tracking(c, t);
    if (!t.str().empty()) out << t.str() << "\n";
  }
  if (c.repo->sparse() && !c.repo->index().empty()) {
    size_t skip = 0;
    for (auto& e : c.repo->index()) if (e.skip_worktree) ++skip;
    size_t total = c.repo->index().size();
    out << "You are in a sparse checkout with " << ((200 * (total - skip) + total) / (2 * total)) << "% of tracked files present.\n\n";
  }
  bool merging = c.read_git_file("MERGE_HEAD").has_value();
  bool picking = c.read_git_file("CHERRY_PICK_HEAD").has_value() || c.read_git_file("REVERT_HEAD").has_value();
  bool any_unmerged = false, del_conflict = false;
  for (auto& it : items) {
    std::string xy{it.x, it.y};
    if (xy == "UU" || xy == "AA" || xy == "DU" || xy == "UD" || xy == "AU" || xy == "UA" || xy == "DD") any_unmerged = true;
    if (xy == "DU" || xy == "UD") del_conflict = true;
  }
  if (merging) {
    if (any_unmerged)
      out << "You have unmerged paths.\n  (fix conflicts and run \"git commit\")\n  (use \"git merge --abort\" to abort the merge)\n\n";
    else
      out << "All conflicts fixed but you are still merging.\n  (use \"git commit\" to conclude merge)\n\n";
  }
  auto label = [](char ch) -> std::string {
    switch (ch) {
      case 'A': return "new file:   ";
      case 'D': return "deleted:    ";
      case 'R': return "renamed:    ";
      case 'T': return "typechange: ";
      case 'C': return "copied:     ";
      default: return "modified:   ";
    }
  };
  auto unmerged_label = [](const StatusItem& it) -> std::string {
    std::string xy{it.x, it.y};
    if (xy == "UU") return "both modified:   ";
    if (xy == "AA") return "both added:      ";
    if (xy == "DU") return "deleted by us:   ";
    if (xy == "UD") return "deleted by them: ";
    if (xy == "AU") return "added by us:     ";
    if (xy == "UA") return "added by them:   ";
    return "both deleted:    ";
  };
  auto is_unmerged = [](const StatusItem& it) {
    std::string xy{it.x, it.y};
    return xy == "UU" || xy == "AA" || xy == "DU" || xy == "UD" || xy == "AU" || xy == "UA" || xy == "DD";
  };
  bool staged = false, unstaged = false, has_deleted = false;
  for (auto& it : items) {
    if (is_unmerged(it)) continue;
    if (it.x != ' ') staged = true;
    if (it.y != ' ') { unstaged = true; if (it.y == 'D') has_deleted = true; }
  }
  if (staged) {
    out << "Changes to be committed:\n";
    if (!merging && !picking)
      out << (head ? "  (use \"git restore --staged <file>...\" to unstage)\n" : "  (use \"git rm --cached <file>...\" to unstage)\n");
    for (auto& it : items) {
      if (is_unmerged(it) || it.x == ' ') continue;
      std::string t = label(it.x);
      if (it.x == 'R') t += qp(c, c.to_display_path(it.orig)) + " -> ";
      t += qp(c, c.to_display_path(it.path));
      out << "\t" << sc.paint(sc.updated, t) << "\n";
    }
    out << "\n";
  }
  if (any_unmerged) {
    out << "Unmerged paths:\n";
    if (!merging && !picking) out << (head ? "  (use \"git restore --staged <file>...\" to unstage)\n" : "  (use \"git rm --cached <file>...\" to unstage)\n");
    out << (del_conflict ? "  (use \"git add/rm <file>...\" as appropriate to mark resolution)\n" : "  (use \"git add <file>...\" to mark resolution)\n");
    for (auto& it : items)
      if (is_unmerged(it)) out << "\t" << sc.paint(sc.unmerged, unmerged_label(it) + qp(c, c.to_display_path(it.path))) << "\n";
    out << "\n";
  }
  if (unstaged) {
    out << "Changes not staged for commit:\n";
    out << (has_deleted ? "  (use \"git add/rm <file>...\" to update what will be committed)\n"
                        : "  (use \"git add <file>...\" to update what will be committed)\n");
    out << "  (use \"git restore <file>...\" to discard changes in working directory)\n";
    for (auto& it : items) {
      if (is_unmerged(it) || it.y == ' ') continue;
      out << "\t" << sc.paint(sc.changed, label(it.y) + qp(c, c.to_display_path(it.path))) << "\n";
    }
    out << "\n";
  }
  if (!untracked.empty()) {
    out << "Untracked files:\n  (use \"git add <file>...\" to include in what will be committed)\n";
    for (auto& u : untracked) out << "\t" << sc.paint(sc.untracked, qp(c, c.to_display_path(u))) << "\n";
    out << "\n";
  }
  if (ignored && !ignored->empty()) {
    out << "Ignored files:\n  (use \"git add -f <file>...\" to include in what will be committed)\n";
    for (auto& u : *ignored) out << "\t" << sc.paint(sc.untracked, qp(c, c.to_display_path(u))) << "\n";
    out << "\n";
  }
  const bool hints = c.config.get_bool("advice.statushints", true);
  // -uno (wt_longstatus_print): with something to commit, a note that untracked files were not looked for
  if (!shows_untracked && staged)
    out << "Untracked files not listed" << (hints ? " (use -u option to show untracked files)" : "") << "\n";
  if (staged) {
    // nothing more
  } else if (unstaged || any_unmerged) {
    out << "no changes added to commit (use \"git add\" and/or \"git commit -a\")\n";
  } else if (!untracked.empty()) {
    out << "nothing added to commit but untracked files present (use \"git add\" to track)\n";
  } else if (!head) {
    out << "nothing to commit (create/copy files and use \"git add\" to track)\n";
  } else if (!shows_untracked) {
    out << (hints ? "nothing to commit (use -u to show untracked files)\n" : "nothing to commit\n");
  } else {
    out << "nothing to commit, working tree clean\n";
  }
  // "On branch x\n\nNo commits yet" keeps its blank line; a clean tree prints none before "nothing"
}

std::string status_text_for_commit(Ctx& c) {
  std::ostringstream ss;
  auto items = tracked_status(c, Pathspec{}, true);
  auto un = c.repo->untracked(true, true);
  std::vector<std::string> u = un.ok() ? *un : std::vector<std::string>{};
  long_status(c, items, u, ss, true);
  return ss.str();
}

// git status --porcelain=v2: "# branch.*" headers, then "1" (changed), "2" (renamed), "u" (unmerged) and "?"
// lines carrying the HEAD/index/work-tree modes and the HEAD/index object ids. Paths are from the top.
void print_status_v2(Ctx& c, const std::vector<StatusItem>& items, const std::vector<std::string>& untracked,
                     bool branch, bool z) {
  const char eol = z ? '\0' : '\n';
  const std::string zero_oid(40, '0');
  auto head = read_ref(c, "HEAD");
  if (branch) {
    std::string b = current_branch(c);
    c.out << "# branch.oid " << (head ? *head : std::string("(initial)")) << eol;
    c.out << "# branch.head " << (b.empty() ? std::string("(detached)") : b) << eol;
    std::string up = b.empty() ? "" : upstream_of(c, b);
    if (!up.empty()) {
      c.out << "# branch.upstream " << short_ref(up) << eol;
      auto theirs = read_ref(c, up);
      if (theirs && head) {
        long a = 0, bh = 0;
        ahead_behind(c, *head, *theirs, a, bh);
        c.out << "# branch.ab +" << a << " -" << bh << eol;
      }
    }
  }
  Snapshot hs = snapshot_tree(c, head_tree(c));
  Snapshot is = snapshot_index(c);
  Snapshot ws = snapshot_worktree(c, false, nullptr);
  auto mode_of = [](const Snapshot& s, const std::string& path) {
    auto it = s.find(path);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%06o", it != s.end() && it->second.present ? it->second.mode : 0u);
    return std::string(buf);
  };
  auto oid_of = [&](const Snapshot& s, const std::string& path) {
    auto it = s.find(path);
    return it != s.end() && !it->second.oid.empty() ? it->second.oid : zero_oid;
  };
  auto path_out = [&](const std::string& path) { return z ? path : quote_path(path); };
  std::map<std::string, std::map<int, const GitIndexEntry*>> stages;
  for (auto& u : c.repo->unmerged()) stages[u.path][u.stage] = &u;
  for (auto& it : items) {
    const char x = it.x == ' ' ? '.' : it.x, y = it.y == ' ' ? '.' : it.y;
    auto st = stages.find(it.path);
    if (st != stages.end()) {
      c.out << "u " << x << y << " N...";
      for (int s = 1; s <= 3; ++s) {
        auto e = st->second.find(s);
        char buf[16];
        std::snprintf(buf, sizeof buf, " %06o", e != st->second.end() ? e->second->mode : 0u);
        c.out << buf;
      }
      c.out << ' ' << mode_of(ws, it.path);
      for (int s = 1; s <= 3; ++s) {
        auto e = st->second.find(s);
        c.out << ' ' << (e != st->second.end() ? e->second->oid : zero_oid);
      }
      c.out << ' ' << path_out(it.path) << eol;
      continue;
    }
    const std::string& head_path = it.orig.empty() ? it.path : it.orig;
    c.out << (it.orig.empty() ? "1 " : "2 ") << x << y << " N... " << mode_of(hs, head_path) << ' '
          << mode_of(is, it.path) << ' ' << mode_of(ws, it.path) << ' ' << oid_of(hs, head_path) << ' '
          << oid_of(is, it.path) << ' ';
    if (!it.orig.empty()) c.out << 'R' << it.score << ' ';
    c.out << path_out(it.path);
    if (!it.orig.empty()) c.out << (z ? '\0' : '\t') << path_out(it.orig);
    c.out << eol;
  }
  for (auto& u : untracked) c.out << "? " << path_out(u) << eol;
}

}  // namespace

// ---------------------------------------------------------------------------
int cmd_init(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{'q', "quiet"}, {0, "bare"}, {'b', "initial-branch", Arg::Required},
                                         {0, "template", Arg::Required}, {0, "shared", Arg::Optional},
                                         {0, "object-format", Arg::Required}, {0, "separate-git-dir", Arg::Required}});
  if (p.has("separate-git-dir")) unsupported(c, "init --separate-git-dir");
  if (p.has("object-format") && p.get("object-format") != "sha1") unsupported(c, "init --object-format=" + p.get("object-format"));
  if (p.pos.size() > 1) usage(c, "too many arguments");
  std::string dir = p.pos.empty() ? c.cwd : c.abs_path(p.pos[0]);
  bool bare = p.has("bare") || c.getenv("GIT_DIR") != "";
  c.load_config();
  std::string branch = p.get("initial-branch");
  bool fallback = false;
  if (branch.empty()) {
    auto d = c.config.get("init.defaultbranch");
    fallback = !d;
    branch = d.value_or("master");
  }
  auto& h = c.host();
  std::string gitdir = bare ? dir : dir + "/.git";
  bool existed = h.exists(gitdir + "/HEAD");
  if (fallback && !existed) default_branch_hint(c);
  Git g(*c.kernel);
  auto r = g.init(dir, branch, bare);
  if (!r.ok()) die(c, "cannot create repository at '" + dir + "': " + r.error().message);
  if (!p.has("quiet")) {
    std::string shown = gitdir;
    c.out << (existed ? "Reinitialized existing" : "Initialized empty") << " Git repository in " << shown << "/\n";
  }
  return 0;
}

// ---------------------------------------------------------------------------
int cmd_status(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{'s', "short"}, {'b', "branch"}, {0, "porcelain", Arg::Optional}, {'z', ""},
                                         {'u', "untracked-files", Arg::Optional}, {0, "ignored", Arg::Optional},
                                         {0, "long"}, {'v', "verbose"}, {0, "show-stash"}, {0, "ahead-behind"},
                                         {0, "renames"}, {0, "find-renames", Arg::Optional}, {0, "column", Arg::Optional},
                                         {0, "ignore-submodules", Arg::Optional}, {0, "no-optional-locks"}});
  c.open_repo();
  if (p.has("verbose")) unsupported(c, "status -v");
  std::string porcelain = p.has("porcelain") ? p.get("porcelain") : "";
  if (p.has("porcelain") && !porcelain.empty() && porcelain != "v1" && porcelain != "v2" && porcelain != "1" && porcelain != "2")
    die(c, "unsupported porcelain version '" + porcelain + "'");
  const bool v2 = porcelain == "v2" || porcelain == "2";
  // color.status / color.ui; --porcelain and -z are never colored
  c.color = !p.has("porcelain") && !p.has("z") && want_color(c, std::nullopt, false, "status");
  bool z = p.has("z");
  bool short_cfg = c.config.get_bool("status.short", false);
  if (p.has("column") && p.get("column") != "never" && p.get("column") != "no") unsupported(c, "status --column");
  bool machine = p.has("porcelain") || p.has("short") || z || short_cfg;
  if (p.has("long")) {
    // the last of --long and --short on the command line decides
    bool last_long = true;
    for (auto& a : c.args) {
      if (a == "--long") last_long = true;
      else if (a == "--short" || a == "-s") last_long = false;
    }
    if (last_long && !p.has("porcelain") && !z) machine = false;
  }
  // With the change layer on the files are always @ already, so the plain comparison is always empty: the long form
  // shows this workspace's change instead. --short and --porcelain stay git's, for the tools that parse them.
  if (!machine && p.pos.empty() && p.after_dashdash.empty() && change::enabled(c)) {
    const int r = change::status_of_workspace(c);
    if (r >= 0) return r;
  }
  std::string umode = "normal";
  if (p.has("untracked-files")) {
    umode = p.get("untracked-files");
    if (umode.empty()) umode = "all";
  } else if (auto cfg = c.config.get("status.showuntrackedfiles")) {
    umode = *cfg;
  }
  if (umode != "no" && umode != "normal" && umode != "all") usage(c, "Invalid untracked files mode '" + umode + "'");
  std::string imode = "no";
  if (p.has("ignored")) {
    imode = p.get("ignored");
    if (imode.empty()) imode = "traditional";
    if (imode != "traditional" && imode != "matching" && imode != "no") die(c, "Invalid ignored mode '" + imode + "'");
  }
  std::vector<std::string> paths = p.pos;
  paths.insert(paths.end(), p.after_dashdash.begin(), p.after_dashdash.end());
  Pathspec ps = make_pathspec(c, paths);
  auto rename_cfg = [&](const char* key, bool def) {
    auto v = c.config.get(key);
    if (v && (*v == "copy" || *v == "copies")) return true;
    return c.config.get_bool(key, def);
  };
  bool renames = rename_cfg("status.renames", rename_cfg("diff.renames", true));
  if (p.negated.count("renames")) renames = false;
  const bool count_ab = !p.negated.count("ahead-behind") && c.config.get_bool("status.aheadbehind", true);
  // The untracked walk reads directories and the ignore rules, the tracked comparison the index, the work tree's
  // stats and objects: nothing the other changes. They run side by side (the index read first, by both).
  (void)c.repo->index();
  std::optional<Result<std::vector<std::string>>> un_result;
  std::thread un_thread;
  struct Join { std::thread& t; ~Join() { if (t.joinable()) t.join(); } } join_un{un_thread};
  if (umode != "no") {
    const bool collapse = umode == "normal";
    un_thread = std::thread([&, collapse] { un_result.emplace(c.repo->untracked(collapse, true)); });
  }
  auto items = tracked_status(c, ps, renames);
  if (un_thread.joinable()) un_thread.join();
  std::vector<std::string> untracked, ignored;
  auto in_spec = [&](const std::string& u) {
    std::string probe = u.size() && u.back() == '/' ? u.substr(0, u.size() - 1) : u;
    return ps.match(probe) || (u.back() == '/' && ps.may_match_below(probe));
  };
  if (umode != "no") {
    auto& un = *un_result;
    if (!un.ok()) die(c, un.error().message);
    for (auto& u : *un) if (in_spec(u)) untracked.push_back(u);
    std::sort(untracked.begin(), untracked.end());
  }
  if (imode != "no" && umode != "no") {
    std::vector<std::string> all;
    collect_ignored(c, umode != "all", all);
    for (auto& u : all) if (in_spec(u)) ignored.push_back(u);
    std::sort(ignored.begin(), ignored.end());
  }
  if (!machine) {
    long_status(c, items, untracked, c.out, false, &ignored, umode != "no");
    return 0;
  }
  if (porcelain == "v2") {
    print_status_v2(c, items, untracked, p.has("branch"), z);
    return 0;
  }
  bool rel = !p.has("porcelain");   // --short shows paths from cwd, --porcelain from the top
  auto shown = [&](const std::string& path) { return rel || v2 ? c.to_display_path(path) : path; };
  const char eol = z ? '\0' : '\n';
  auto text = [&](const std::string& path, bool space) { return z ? shown(path) : qp(c, shown(path), space); };
  const bool branch = p.has("branch") || (!p.has("porcelain") && c.config.get_bool("status.branch", false));
  if (v2) {
    if (branch) {
      auto head = read_ref(c, "HEAD");
      std::string b = current_branch(c);
      c.out << "# branch.oid " << (head ? *head : std::string("(initial)")) << eol;
      c.out << "# branch.head " << (b.empty() ? std::string("(detached)") : b) << eol;
      std::string up = b.empty() ? "" : upstream_of(c, b);
      if (!up.empty()) {
        c.out << "# branch.upstream " << short_ref(up) << eol;
        auto theirs = read_ref(c, up);
        if (theirs && head) {
          if (count_ab) {
            long a = 0, bh = 0;
            ahead_behind(c, *head, *theirs, a, bh);
            c.out << "# branch.ab +" << a << " -" << bh << eol;
          } else {
            c.out << (*head == *theirs ? "# branch.ab +0 -0" : "# branch.ab +? -?") << eol;
          }
        }
      }
    }
    if (p.has("show-stash")) {
      size_t n = stash_count(c);
      if (n) c.out << "# stash " << n << eol;
    }
    std::vector<StatusItem> ordered;
    for (auto& it : items) if (!it.unmerged) ordered.push_back(it);
    for (auto& it : items) if (it.unmerged) ordered.push_back(it);
    for (auto& it : ordered) {
      auto dot =[](char ch) { return ch == ' ' ? '.' : ch; };
      auto mode = [](uint32_t m) { char b[8]; snprintf(b, sizeof b, "%06o", m); return std::string(b); };
      const bool sub = it.mh == 0160000 || it.mi == 0160000 || it.mw == 0160000;
      const char* subs = sub ? "S..." : "N...";
      if (it.unmerged) {
        c.out << "u " << it.x << it.y << ' ' << subs << ' ' << mode(it.sm[0]) << ' ' << mode(it.sm[1]) << ' ' << mode(it.sm[2]) << ' '
              << mode(it.mw) << ' ' << (it.so[0].empty() ? kNullOid : it.so[0]) << ' ' << (it.so[1].empty() ? kNullOid : it.so[1]) << ' '
              << (it.so[2].empty() ? kNullOid : it.so[2]) << ' ' << text(it.path, false) << eol;
        continue;
      }
      c.out << (it.orig.empty() ? "1 " : "2 ") << dot(it.x) << dot(it.y) << ' ' << subs << ' ' << mode(it.mh) << ' ' << mode(it.mi) << ' '
            << mode(it.mw) << ' ' << it.hh << ' ' << it.hi << ' ';
      if (!it.orig.empty()) c.out << it.x << it.score << ' ';
      c.out << text(it.path, false);
      if (!it.orig.empty()) c.out << (z ? '\0' : '\t') << text(it.orig, false);
      c.out << eol;
    }
    for (auto& u : untracked) c.out << "? " << text(u, false) << eol;
    for (auto& u : ignored) c.out << "! " << text(u, false) << eol;
    return 0;
  }
  const StatusColors sc = status_colors(c);
  if (branch) {
    // wt_shortstatus_print_tracking: the branch in the local color, its upstream and the counts in theirs
    auto head = read_ref(c, "HEAD");
    std::string b = current_branch(c);
    c.out << sc.paint(sc.header, "## ");
    if (b.empty()) c.out << sc.paint(sc.nobranch, "HEAD (no branch)");
    else if (!head) c.out << sc.paint(sc.header, "No commits yet on ") << sc.paint(sc.local, b);
    else {
      c.out << sc.paint(sc.local, b);
      std::string up = upstream_of(c, b);
      if (!up.empty()) {
        c.out << sc.paint(sc.header, "...") << sc.paint(sc.remote, short_ref(up));
        auto theirs = read_ref(c, up);
        if (!theirs) c.out << sc.paint(sc.header, " [") << sc.paint(sc.header, "gone") << sc.paint(sc.header, "]");
        else if (!count_ab) {
          if (*head != *theirs) c.out << sc.paint(sc.header, " [") << sc.paint(sc.header, "different") << sc.paint(sc.header, "]");
        } else {
          long a = 0, bh = 0;
          ahead_behind(c, *head, *theirs, a, bh);
          if (a || bh) {
            c.out << sc.paint(sc.header, " [");
            if (!a) c.out << sc.paint(sc.header, "behind ") << sc.paint(sc.remote, std::to_string(bh));
            else if (!bh) c.out << sc.paint(sc.header, "ahead ") << sc.paint(sc.local, std::to_string(a));
            else
              c.out << sc.paint(sc.header, "ahead ") << sc.paint(sc.local, std::to_string(a)) << sc.paint(sc.header, ", behind ")
                    << sc.paint(sc.remote, std::to_string(bh));
            c.out << sc.paint(sc.header, "]");
          }
        }
      }
    }
    c.out << eol;
  }
  for (auto& it : items) {
    const std::string xy{it.x, it.y};
    const bool unmerged = xy == "UU" || xy == "AA" || xy == "DU" || xy == "UD" || xy == "AU" || xy == "UA" || xy == "DD";
    if (unmerged) c.out << sc.paint(sc.unmerged, xy);
    else c.out << (it.x != ' ' ? sc.paint(sc.updated, std::string(1, it.x)) : " ")
               << (it.y != ' ' ? sc.paint(sc.changed, std::string(1, it.y)) : " ");
    c.out << ' ';
    if (z) {
      c.out << shown(it.path) << '\0';
      if (!it.orig.empty()) c.out << shown(it.orig) << '\0';
    } else {
      if (!it.orig.empty()) c.out << text(it.orig, true) << " -> ";
      c.out << text(it.path, true) << "\n";
    }
  }
  for (auto& u : untracked) c.out << sc.paint(sc.untracked, "??") << " " << text(u, true) << eol;
  for (auto& u : ignored) c.out << sc.paint(sc.untracked, "!!") << " " << text(u, true) << eol;
  return 0;
}

// ---------------------------------------------------------------------------
int cmd_add(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{'A', "all"}, {'u', "update"}, {'f', "force"}, {'n', "dry-run"}, {'v', "verbose"},
                                         {0, "ignore-removal"}, {'N', "intent-to-add"}, {0, "chmod", Arg::Required},
                                         {'q', "quiet"}, {0, "ignore-errors"}, {0, "ignore-missing"}, {0, "renormalize"},
                                         {'p', "patch"}, {'i', "interactive"}, {'e', "edit"}, {0, "pathspec-from-file", Arg::Required}});
  c.open_repo();
  if (p.has("patch") || p.has("interactive") || p.has("edit")) unsupported(c, "interactive add");
  if (p.has("pathspec-from-file")) unsupported(c, "add --pathspec-from-file");
  std::vector<std::string> paths = p.pos;
  paths.insert(paths.end(), p.after_dashdash.begin(), p.after_dashdash.end());
  bool all = p.has("all") || (p.negated.count("all") == 0 && false);
  bool update = p.has("update");
  bool ignore_removal = p.has("ignore-removal") || p.negated.count("all");
  if (paths.empty() && !all && !update) {
    c.err << "Nothing specified, nothing added.\nhint: Maybe you wanted to say 'git add .'?\n";
    return 0;
  }
  Pathspec ps = make_pathspec(c, paths);
  if (paths.empty() && (all || update)) ps = Pathspec{};   // the whole tree, from wherever you are
  bool dry = p.has("dry-run"), verbose = p.has("verbose") || dry, force = p.has("force");
  std::vector<bool> matched(ps.items.size(), false);
  auto note_match = [&](const std::string& path) {
    int i = ps.matched_index(path);
    if (i >= 0) matched[(size_t)i] = true;
  };
  std::vector<GitIndexEntry> index = c.repo->index();
  std::map<std::string, size_t> at;
  for (size_t i = 0; i < index.size(); ++i) at[index[i].path] = i;
  std::set<std::string> remove;
  std::vector<std::string> to_stage;
  // tracked paths (and unmerged ones, which `add` resolves)
  std::set<std::string> tracked;
  for (auto& e : index) tracked.insert(e.path);
  for (auto& u : c.repo->unmerged()) tracked.insert(u.path);
  auto& h = c.host();
  // Each tracked path's stat and stat-cache check, on threads (the checks are independent and nearly all system
  // call), then taken in order.
  std::vector<std::string> named;
  for (auto& path : tracked) if (ps.match(path)) named.push_back(path);
  const bool intent_only = p.has("intent-to-add");
  std::vector<char> state(named.size(), 0);   // 1 gone, 2 unchanged, 3 to stage
  {
    std::atomic<size_t> next{0};
    auto work = [&]() {
      for (;;) {
        const size_t from = next.fetch_add(32);
        if (from >= named.size()) return;
        for (size_t i = from; i < std::min(named.size(), from + 32); ++i) {
          const std::string& path = named[i];
          auto st = h.stat(join(c.top, path));
          if (!(st.ok() && (!st->is_dir || st->is_link))) { state[i] = 1; continue; }
          if (intent_only) { state[i] = 2; continue; }
          auto it = at.find(path);
          state[i] = it != at.end() && !index[it->second].intent_to_add && c.repo->worktree_matches(index[it->second]) ? 2 : 3;
        }
      }
    };
    unsigned n = hal::hardware_threads();
    n = std::max(1u, std::min({n == 0 ? 1u : n, 20u, (unsigned)(named.size() / 500)}));
    std::vector<hal::Thread> pool;
    for (unsigned t = 1; t < n; ++t) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
  }
  for (size_t i = 0; i < named.size(); ++i) {
    const std::string& path = named[i];
    note_match(path);
    if (state[i] == 1) {
      if (!ignore_removal) remove.insert(path);
      continue;
    }
    if (state[i] == 3) to_stage.push_back(path);
  }
  if (!update) {
    auto un = c.repo->untracked(false);
    if (!un.ok()) die(c, un.error().message);
    for (auto& u : *un) {
      if (!ps.match(u)) continue;
      note_match(u);
      to_stage.push_back(u);
    }
    // a path named outright that an ignore rule hides
    std::vector<std::string> ignored_named;
    for (size_t i = 0; i < ps.items.size(); ++i) {
      if (matched[i]) continue;
      const std::string& it = ps.items[i];
      auto st = h.stat(join(c.top, it));
      if (!st.ok()) continue;
      if (st->is_dir && !st->is_link) {
        // a directory with only ignored files, or an ignored directory
        if (c.repo->is_ignored(it, true)) {
          if (force) {
            std::function<void(const std::string&)> walkdir = [&](const std::string& d) {
              auto names = h.list_dir(join(c.top, d));
              if (!names.ok()) return;
              for (auto& n : *names) {
                if (n == "." || n == ".." || n == ".git") continue;
                std::string rp = d + "/" + n;
                auto s2 = h.stat(join(c.top, rp));
                if (!s2.ok()) continue;
                if (s2->is_dir && !s2->is_link) walkdir(rp);
                else if (!tracked.count(rp)) to_stage.push_back(rp);
              }
            };
            walkdir(it);
            matched[i] = true;
          } else {
            ignored_named.push_back(it);
          }
        }
        continue;
      }
      if (c.repo->is_ignored(it, false)) {
        if (force) { to_stage.push_back(it); matched[i] = true; }
        else ignored_named.push_back(it);
      }
    }
    if (!ignored_named.empty()) {
      c.err << "The following paths are ignored by one of your .gitignore files:\n";
      for (auto& x : ignored_named) c.err << c.to_display_path(x) << "\n";
      c.err << "hint: Use -f if you really want to add them.\n"
               "hint: Turn this message off by running\n"
               "hint: \"git config advice.addIgnoredFile false\"\n";
      // what could be added still is, as in git
      for (size_t i = 0; i < ps.items.size(); ++i)
        if (std::find(ignored_named.begin(), ignored_named.end(), ps.items[i]) != ignored_named.end()) matched[i] = true;
      if (!dry) {
        // fall through to stage the rest, then exit 1
      }
      for (size_t i = 0; i < ps.items.size(); ++i) {
        if (!matched[i]) die(c, "pathspec '" + ps.raw_of(i) + "' did not match any files");
      }
      std::vector<GitIndexEntry> next = index;
      // stage the rest below and then fail
      for (auto& path : to_stage) {
        auto it = at.find(path);
        auto e = c.repo->stage_entry(path, it != at.end() ? &index[it->second] : nullptr);
        if (!e.ok()) die(c, "unable to add '" + path + "': " + e.error().message);
        if (it != at.end()) next[it->second] = *e; else next.push_back(*e);
      }
      if (!dry && !to_stage.empty()) { c.repo->set_index(next); (void)c.repo->write_index(); }
      return 1;
    }
  }
  for (size_t i = 0; i < ps.items.size(); ++i) {
    if (matched[i]) continue;
    // a path that exists but holds nothing to add (an empty directory) is fine for git only when tracked
    if (update) continue;
    die(c, "pathspec '" + ps.raw_of(i) + "' did not match any files");
  }
  std::string chmod = p.get("chmod");
  if (!chmod.empty() && chmod != "+x" && chmod != "-x") die(c, "--chmod param '" + chmod + "' must be either -x or +x");
  std::vector<GitIndexEntry> next;
  std::vector<GitIndexEntry> unmerged = c.repo->unmerged();
  std::sort(to_stage.begin(), to_stage.end());
  to_stage.erase(std::unique(to_stage.begin(), to_stage.end()), to_stage.end());
  std::map<std::string, GitIndexEntry> staged;
  if (verbose) {
    // git reports the tracked paths first (updates and removals, in path order), then new files
    std::map<std::string, bool> tracked_lines;   // path -> removed
    for (auto& path : to_stage) if (tracked.count(path)) tracked_lines[path] = false;
    for (auto& path : remove) tracked_lines[path] = true;
    for (auto& [path, removed] : tracked_lines) c.out << (removed ? "remove '" : "add '") << c.to_display_path(path) << "'\n";
    for (auto& path : to_stage) if (!tracked.count(path)) c.out << "add '" << c.to_display_path(path) << "'\n";
  }
  const bool intent = p.has("intent-to-add");
  for (auto& path : to_stage) {
    auto it = at.find(path);
    if (dry) continue;
    if (intent && !tracked.count(path)) {
      // named, not staged: the empty blob stands in until the content is added
      auto empty = c.repo->write_object("blob", ByteSpan{});
      if (!empty.ok()) die(c, empty.error().message);
      auto st = c.host().stat(join(c.top, path));
      GitIndexEntry n;
      n.path = path;
      n.oid = *empty;
      n.mode = st.ok() && st->is_link && c.repo->settings().symlinks ? 0120000 : st.ok() && st->is_exec && c.repo->settings().filemode ? 0100755 : 0100644;
      n.intent_to_add = true;
      staged[path] = n;
      continue;
    }
    auto e = c.repo->stage_entry(path, it != at.end() ? &index[it->second] : nullptr);
    if (!e.ok()) {
      if (p.has("ignore-errors")) { c.err << "error: unable to index file '" << path << "'\n"; continue; }
      die(c, "unable to index file '" + path + "': " + e.error().message);
    }
    staged[path] = *e;
  }
  if (dry) return 0;
  for (auto& e : index) {
    if (remove.count(e.path)) continue;
    auto s = staged.find(e.path);
    GitIndexEntry n = s != staged.end() ? s->second : e;
    if (!chmod.empty() && ps.match(n.path) && n.mode != 0120000 && n.mode != 0160000) n.mode = chmod == "+x" ? 0100755 : 0100644;
    next.push_back(n);
    if (s != staged.end()) staged.erase(s);
  }
  for (auto& [path, e] : staged) {
    GitIndexEntry n = e;
    if (!chmod.empty() && n.mode != 0120000) n.mode = chmod == "+x" ? 0100755 : 0100644;
    next.push_back(n);
  }
  // resolving: an unmerged path that was added or removed is no longer unmerged
  std::vector<GitIndexEntry> still;
  for (auto& u : unmerged) {
    bool resolved = false;
    for (auto& n : next) if (n.path == u.path) resolved = true;
    if (remove.count(u.path)) resolved = true;
    if (!resolved) still.push_back(u);
  }
  c.repo->set_unmerged(still);
  c.repo->set_index(next);
  auto w = c.repo->write_index();
  if (!w.ok()) die(c, "unable to write new index file: " + w.error().message);
  return 0;
}

// ---------------------------------------------------------------------------
int cmd_rm(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{'r', ""}, {'f', "force"}, {0, "cached"}, {'q', "quiet"}, {'n', "dry-run"},
                                         {0, "ignore-unmatch"}, {0, "sparse"}, {0, "pathspec-from-file", Arg::Required}});
  c.open_repo();
  if (p.has("pathspec-from-file")) unsupported(c, "rm --pathspec-from-file");
  std::vector<std::string> paths = p.pos;
  paths.insert(paths.end(), p.after_dashdash.begin(), p.after_dashdash.end());
  if (paths.empty()) usage(c, "No pathspec was given. Which files should I remove?");
  Pathspec ps = make_pathspec(c, paths);
  bool cached = p.has("cached"), force = p.has("force"), recursive = p.has("r");
  std::vector<std::string> hits;
  std::vector<bool> matched(ps.items.size(), false);
  std::set<std::string> tracked;
  for (auto& e : c.repo->index()) tracked.insert(e.path);
  for (auto& u : c.repo->unmerged()) tracked.insert(u.path);
  for (auto& t : tracked) {
    int i = ps.matched_index(t);
    if (i < 0) continue;
    // a directory pathspec needs -r
    if (!recursive && ps.items[(size_t)i] != t && !ps.items[(size_t)i].empty() && !ps.is_wild((size_t)i))
      die(c, "not removing '" + ps.raw_of((size_t)i) + "' recursively without -r");
    matched[(size_t)i] = true;
    hits.push_back(t);
  }
  for (size_t i = 0; i < matched.size(); ++i)
    if (!matched[i] && !p.has("ignore-unmatch")) die(c, "pathspec '" + ps.raw_of(i) + "' did not match any files");
  if (!force) {
    std::string head = head_tree(c);
    Snapshot hs = snapshot_tree(c, head);
    std::vector<std::string> modified, staged_diff;
    for (auto& path : hits) {
      const GitIndexEntry* e = nullptr;
      for (auto& x : c.repo->index()) if (x.path == path) e = &x;
      if (!e) continue;
      bool wt_same = c.repo->worktree_matches(*e) || !c.host().exists(join(c.top, path));
      auto hit = hs.find(path);
      bool head_same = hit != hs.end() && hit->second.oid == e->oid;
      if (!wt_same && !head_same && !cached) staged_diff.push_back(path);
      else if (!wt_same && !cached) modified.push_back(path);
      else if (!head_same && cached && !wt_same) staged_diff.push_back(path);
      else if (!head_same && !cached && hit != hs.end()) modified.push_back(path);
    }
    if (!staged_diff.empty()) {
      c.err << "error: the following file" << (staged_diff.size() > 1 ? "s have" : " has") << " staged content different from both the\nfile and the HEAD:\n";
      for (auto& x : staged_diff) c.err << "    " << x << "\n";
      c.err << "(use -f to force removal)\n";
      return 1;
    }
    if (!modified.empty()) {
      c.err << "error: the following file" << (modified.size() > 1 ? "s have" : " has") << (cached ? " changes staged in the index:\n" : " local modifications:\n");
      for (auto& x : modified) c.err << "    " << x << "\n";
      c.err << (cached ? "(use --cached to keep the file, or -f to force removal)\n" : "(use --cached to keep the file, or -f to force removal)\n");
      return 1;
    }
  }
  for (auto& path : hits) if (!p.has("quiet")) c.out << "rm '" << path << "'\n";
  if (p.has("dry-run")) return 0;
  std::set<std::string> drop(hits.begin(), hits.end());
  std::vector<GitIndexEntry> next, still;
  for (auto& e : c.repo->index()) if (!drop.count(e.path)) next.push_back(e);
  for (auto& u : c.repo->unmerged()) if (!drop.count(u.path)) still.push_back(u);
  c.repo->set_index(next);
  c.repo->set_unmerged(still);
  auto w = c.repo->write_index();
  if (!w.ok()) die(c, w.error().message);
  if (!cached) {
    auto& h = c.host();
    for (auto& path : hits) {
      (void)h.remove(join(c.top, path));
      // remove directories left empty
      for (std::string d = dirname_of(path); !d.empty(); d = dirname_of(d)) {
        auto names = h.list_dir(join(c.top, d));
        if (!names.ok()) break;
        bool empty = true;
        for (auto& n : *names) if (n != "." && n != "..") empty = false;
        if (!empty || !h.remove(join(c.top, d)).ok()) break;
      }
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
int cmd_mv(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{'f', "force"}, {'k', ""}, {'n', "dry-run"}, {'v', "verbose"}});
  c.open_repo();
  std::vector<std::string> args = p.pos;
  args.insert(args.end(), p.after_dashdash.begin(), p.after_dashdash.end());
  if (args.size() < 2) usage(c, "usage: git mv [<options>] <source>... <destination>");
  auto& h = c.host();
  std::string dst_user = args.back();
  args.pop_back();
  std::string dst = c.to_repo_path(dst_user);
  if (dst == "..") die(c, "'" + dst_user + "' is outside repository");
  auto dst_st = h.stat(join(c.top, dst));
  bool into_dir = args.size() > 1 || (dst_st.ok() && dst_st->is_dir);
  if (args.size() > 1 && !(dst_st.ok() && dst_st->is_dir)) die(c, "destination '" + dst_user + "' is not a directory");
  std::vector<GitIndexEntry> index = c.repo->index();
  std::vector<std::pair<std::string, std::string>> moves;   // index paths
  std::vector<std::pair<std::string, std::string>> fs_moves;
  for (auto& s_user : args) {
    std::string src = c.to_repo_path(s_user);
    if (src == ".." || src == ".") die(c, "bad source, source=" + s_user + ", destination=" + dst_user);
    std::string target = into_dir ? (dst == "." ? basename_of(src) : dst + "/" + basename_of(src)) : dst;
    auto st = h.stat(join(c.top, src));
    if (!st.ok()) {
      if (p.has("k")) continue;
      die(c, "bad source, source=" + s_user + ", destination=" + c.to_display_path(target));
    }
    bool any = false;
    for (auto& e : index) {
      if (e.path == src) { moves.push_back({e.path, target}); any = true; }
      else if (e.path.rfind(src + "/", 0) == 0) { moves.push_back({e.path, target + e.path.substr(src.size())}); any = true; }
    }
    if (!any) {
      if (p.has("k")) continue;
      die(c, "not under version control, source=" + s_user + ", destination=" + c.to_display_path(target));
    }
    if (h.exists(join(c.top, target)) && !p.has("force") && !(st->is_dir)) {
      if (p.has("k")) continue;
      die(c, "destination exists, source=" + s_user + ", destination=" + c.to_display_path(target));
    }
    fs_moves.push_back({src, target});
  }
  if (p.has("verbose") || p.has("dry-run"))
    for (auto& [a, b] : fs_moves) c.out << "Renaming " << a << " to " << b << "\n";
  if (p.has("dry-run")) return 0;
  for (auto& [a, b] : fs_moves) {
    std::string from = join(c.top, a), to = join(c.top, b);
    (void)h.make_dir(dirname_of(to));
    // move the files the index knows (a directory moves its whole content)
    std::function<bool(const std::string&, const std::string&)> mv = [&](const std::string& f, const std::string& t) -> bool {
      auto st = h.stat(f);
      if (!st.ok()) return false;
      if (st->is_dir && !st->is_link) {
        (void)h.make_dir(t);
        auto names = h.list_dir(f);
        if (names.ok()) for (auto& n : *names) if (n != "." && n != "..") mv(f + "/" + n, t + "/" + n);
        (void)h.remove(f);
        return true;
      }
      auto data = h.read_file(f);
      if (!data.ok()) return false;
      if (!h.write_file(t, ByteSpan(data->data(), data->size())).ok()) return false;
      if (st->is_exec) h.set_access(t, hal::Host::FileAccess::Executable);
      return h.remove(f).ok();
    };
    if (!mv(from, to)) die(c, "renaming '" + a + "' failed");
  }
  for (auto& e : index)
    for (auto& [from, to] : moves)
      if (e.path == from) { e.path = to; e.mtime_ns = 0; }
  c.repo->set_index(index);
  auto w = c.repo->write_index();
  if (!w.ok()) die(c, w.error().message);
  return 0;
}

// ---------------------------------------------------------------------------
int cmd_commit(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{'m', "message", Arg::Required}, {'F', "file", Arg::Required}, {'a', "all"},
                                         {'q', "quiet"}, {0, "amend"}, {0, "no-edit"}, {'e', "edit"}, {0, "allow-empty"},
                                         {0, "allow-empty-message"}, {'n', "no-verify"}, {0, "verify"}, {0, "author", Arg::Required},
                                         {0, "date", Arg::Required}, {'s', "signoff"}, {'v', "verbose"}, {0, "cleanup", Arg::Required},
                                         {'C', "reuse-message", Arg::Required}, {'c', "reedit-message", Arg::Required},
                                         {0, "reset-author"}, {'o', "only"}, {'i', "include"}, {0, "dry-run"}, {0, "status"},
                                         {'S', "gpg-sign", Arg::Optional}, {0, "fixup", Arg::Required}, {0, "squash", Arg::Required},
                                         {0, "porcelain"}, {0, "short"}, {'u', "untracked-files", Arg::Optional},
                                         {0, "pathspec-from-file", Arg::Required}, {'t', "template", Arg::Required},
                                         {0, "trailer", Arg::Required}});
  c.open_repo();
  take_sign_option(c, p);
  if (p.has("fixup") || p.has("squash")) unsupported(c, "commit --fixup/--squash");
  if (p.has("dry-run") || p.has("porcelain") || p.has("short")) unsupported(c, "commit --dry-run");
  if (p.has("pathspec-from-file") || p.has("template") || p.has("include")) unsupported(c, "commit --pathspec-from-file/--template/--include");
  if (p.has("trailer")) unsupported(c, "commit --trailer");
  if (!c.repo->unmerged().empty()) {
    std::set<std::string> seen;
    for (auto& u : c.repo->unmerged()) if (seen.insert(u.path).second) c.out << "U\t" << u.path << "\n";
    c.err << "error: Committing is not possible because you have unmerged files.\n"
             "hint: Fix them up in the work tree, and then use 'git add/rm <file>'\n"
             "hint: as appropriate to mark resolution and make a commit.\n"
             "fatal: Exiting because of an unresolved conflict.\n";
    return 128;
  }
  std::vector<std::string> paths = p.pos;
  paths.insert(paths.end(), p.after_dashdash.begin(), p.after_dashdash.end());
  bool amend = p.has("amend");
  auto head = read_ref(c, "HEAD");
  if (amend && !head) die(c, "You have nothing to amend.");
  auto merge_head = c.read_git_file("MERGE_HEAD");
  if (amend && merge_head) die(c, "You are in the middle of a merge -- cannot amend.");
  if (p.has("all") && !paths.empty()) die(c, "paths '" + paths[0] + " ...' with -a does not make sense");

  // what to commit
  std::vector<GitIndexEntry> index = c.repo->index();
  for (auto& e : index) {
    // Present, not read: inflating every blob the index names was 40% of a
    // commit on a 3,000-file tree.
    if (e.mode == 0160000 || c.repo->has_object(e.oid)) continue;
    char m[16];
    std::snprintf(m, sizeof m, "%06o", e.mode);
    // git reports it twice (its tree cache is checked, then built)
    for (int k = 0; k < 2; ++k) c.err << "error: invalid object " << m << " " << e.oid << " for '" << e.path << "'\n";
    c.err << "error: Error building trees\n";
    return 1;
  }
  auto restage = [&](const Pathspec& ps, bool only_tracked) {
    std::map<std::string, size_t> at;
    for (size_t i = 0; i < index.size(); ++i) at[index[i].path] = i;
    std::vector<GitIndexEntry> next;
    for (auto& e : index) {
      if (!ps.match(e.path)) { next.push_back(e); continue; }
      if (!c.host().exists(join(c.top, e.path))) continue;          // deleted
      if (c.repo->worktree_matches(e)) { next.push_back(e); continue; }
      auto s = c.repo->stage_entry(e.path, &e);
      if (!s.ok()) die(c, "unable to index file '" + e.path + "': " + s.error().message);
      next.push_back(*s);
    }
    (void)only_tracked;
    index = next;
  };
  std::string tree;
  if (p.has("all")) {
    restage(Pathspec{}, true);
    c.repo->set_index(index);
    if (auto w = c.repo->write_index(); !w.ok()) die(c, w.error().message);
    tree = write_tree_of(c, index);
  } else if (!paths.empty()) {
    // --only (the default with paths): the commit is HEAD plus these paths as the work tree has them
    if (merge_head) die(c, "cannot do a partial commit during a merge.");
    Pathspec ps = make_pathspec(c, paths);
    std::vector<bool> matched(ps.items.size(), false);
    for (auto& e : index) { int i = ps.matched_index(e.path); if (i >= 0) matched[(size_t)i] = true; }
    Snapshot hs = snapshot_tree(c, head ? read_commit(c, *head).tree : "");
    for (auto& [path, f] : hs) { int i = ps.matched_index(path); if (i >= 0) matched[(size_t)i] = true; }
    for (size_t i = 0; i < matched.size(); ++i)
      if (!matched[i]) { c.err << "error: pathspec '" << ps.raw_of(i) << "' did not match any file(s) known to git\n"; return 1; }
    restage(ps, true);
    c.repo->set_index(index);
    if (auto w = c.repo->write_index(); !w.ok()) die(c, w.error().message);
    std::vector<GitIndexEntry> partial;
    for (auto& [path, f] : hs) if (!ps.match(path)) partial.push_back(GitIndexEntry{path, f.oid, f.mode});
    for (auto& e : index) if (ps.match(e.path)) partial.push_back(e);
    tree = write_tree_of(c, partial);
  } else {
    tree = write_tree_of(c, index);
  }

  // parents
  std::vector<std::string> parents;
  GitCommit old;
  if (amend) {
    old = read_commit(c, *head);
    parents = old.parents;
  } else {
    if (head) parents.push_back(*head);
    if (merge_head) {
      std::istringstream ss(*merge_head);
      std::string line;
      while (std::getline(ss, line)) if (line.size() >= 40) parents.push_back(line.substr(0, 40));
    }
  }

  // nothing to commit?
  if (amend && !p.has("allow-empty") && !parents.empty() && parents.size() == 1 && tree == read_commit(c, parents[0]).tree) {
    std::string b = current_branch(c);
    c.out << (b.empty() ? "HEAD detached at " + c.abbrev(*head) : "On branch " + b) << "\nNo changes\n";
    c.err << "You asked to amend the most recent commit, but doing so would make\n"
             "it empty. You can repeat your command with --allow-empty, or you can\n"
             "remove the commit entirely with \"git reset HEAD^\".\n";
    return 1;
  }
  if (!amend && !merge_head && !p.has("allow-empty")) {
    std::string parent_tree = head ? read_commit(c, *head).tree : empty_tree_oid();
    if (tree == parent_tree) {
      if (!head && index.empty()) {
        c.out << status_text_for_commit(c);
        return 1;
      }
      c.out << status_text_for_commit(c);
      return 1;
    }
  }

  const bool temp_index = p.has("all") || !paths.empty();
  if (run_pre_commit_hook(c, "pre-commit", p.has("no-verify"), temp_index) != 0) return 1;
  if (!p.has("no-verify") && !find_hook(c, "pre-commit").empty()) {
    // the hook may have staged more: the commit is of the index as it leaves it
    c.repo.reset();
    c.open_repo();
    if (paths.empty()) tree = write_tree_of(c, c.repo->index());
  }

  // message
  std::string msg;
  bool have_msg = false;
  bool from_user = false;
  for (auto& m : p.all("message")) {
    if (have_msg) msg += "\n\n";
    msg += m;
    have_msg = true;
    from_user = true;
  }
  if (p.has("file")) {
    if (have_msg) die(c, "Option -m cannot be combined with -F.");
    std::string f = p.get("file");
    if (f == "-") msg = read_all(c.in);
    else {
      auto r = c.host().read_file(c.abs_path(f));
      if (!r.ok()) die(c, "could not read log file '" + f + "': " + r.error().message);
      msg.assign((const char*)r->data(), r->size());
    }
    have_msg = true;
    from_user = true;
  }
  std::string reuse = p.has("reuse-message") ? p.get("reuse-message") : p.get("reedit-message");
  std::string author;
  if (!reuse.empty()) {
    auto rc = read_commit(c, commit_of(c, reuse));
    if (!have_msg) { msg = rc.message; have_msg = true; }
    if (!p.has("reset-author")) author = rc.author + " " + std::to_string(rc.author_time) + " " + rc.author_tz;
  }
  if (amend) {
    if (!have_msg) { msg = old.message; have_msg = true; }
    if (!p.has("reset-author") && author.empty()) author = old.author + " " + std::to_string(old.author_time) + " " + old.author_tz;
  }
  if (!have_msg && merge_head) {
    auto mm = c.read_git_file("MERGE_MSG");
    if (mm) { msg = *mm; have_msg = true; }
  }
  if (!have_msg) {
    std::string editor = c.getenv("GIT_EDITOR");
    if (editor.empty()) editor = c.config.get("core.editor").value_or("");
    if (editor.empty()) editor = c.getenv("VISUAL");
    if (editor.empty()) editor = c.getenv("EDITOR");
    if (editor != "true" && editor != ":")
      die(c, "no commit message given, and aios-git does not start an editor: use -m <message> or -F <file>");
    msg.clear();
  }
  std::string cleanup = p.get("cleanup", c.config.get("commit.cleanup").value_or("default"));
  bool editing = p.has("edit") || (!from_user && !p.has("no-edit") && false);
  if (cleanup == "default") cleanup = editing ? "strip" : "whitespace";
  if (cleanup == "strip") msg = cleanup_message(msg, true);
  else if (cleanup == "whitespace") msg = cleanup_message(msg, false);
  else if (cleanup == "scissors") {
    size_t sc = msg.find("# ------------------------ >8 ------------------------");
    if (sc != std::string::npos) msg = msg.substr(0, sc);
    msg = cleanup_message(msg, false);
  } else if (cleanup != "verbatim") {
    die(c, "Invalid cleanup mode " + cleanup);
  }
  if (p.has("signoff")) {
    std::string me = c.ident(true);
    size_t gt = me.find('>');
    std::string line = "Signed-off-by: " + me.substr(0, gt + 1);
    if (msg.find(line) == std::string::npos) {
      if (msg.empty()) msg = "\n";
      // a trailer block already there is extended, otherwise a blank line comes first
      size_t last_para = msg.rfind("\n\n", msg.size() >= 2 ? msg.size() - 2 : 0);
      std::string last = last_para == std::string::npos ? msg : msg.substr(last_para + 2);
      bool trailers = last.find(": ") != std::string::npos && last_para != std::string::npos;
      msg += (trailers ? "" : "\n") + line + "\n";
    }
  }
  {
    const bool by_message = p.has("message") || p.has("file");
    const std::string source = by_message ? "message" : (!reuse.empty() || amend) ? "commit" : merge_head ? "merge" : "";
    const std::string sha = source == "commit" ? (reuse.empty() ? *head : commit_of(c, reuse)) : "";
    MessageHooks mh = run_message_hooks(c, source == "merge" ? "MERGE_MSG" : "COMMIT_EDITMSG", msg, source, sha, p.has("no-verify"), temp_index);
    if (mh.code != 0) return 1;
    if (mh.message != msg) msg = cleanup == "strip" ? cleanup_message(mh.message, true) : cleanup_message(mh.message, false);
  }
  if (msg.empty() && !p.has("allow-empty-message")) {
    c.err << "Aborting commit due to empty commit message.\n";
    return 1;
  }

  if (p.has("author")) {
    std::string a = p.get("author");
    size_t lt = a.find('<'), gt = a.find('>');
    if (lt == std::string::npos || gt == std::string::npos) die(c, "--author '" + a + "' is not 'Name <email>' and no existing author was found");
    std::string base = c.ident(false);
    size_t sp = base.rfind(' ', base.rfind(' ') - 1);
    author = a.substr(0, gt + 1) + base.substr(sp);
  }
  if (p.has("date")) {
    int64_t t;
    std::string tz;
    if (!parse_date(p.get("date"), t, tz)) die(c, "invalid date format: " + p.get("date"));
    std::string base = author.empty() ? c.ident(false) : author;
    size_t gt = base.find('>');
    author = base.substr(0, gt + 1) + " " + std::to_string(t) + " " + tz;
  }
  std::string oid = make_commit(c, tree, parents, msg, author, {}, true);
  std::string subject = msg.substr(0, msg.find('\n'));   // the reflog keeps the first line, not the folded subject
  std::string reflog = std::string("commit") + (amend ? " (amend)" : merge_head ? " (merge)" : head ? "" : " (initial)") + ": " + subject;
  update_ref_checked(c, "HEAD", oid, std::nullopt, reflog);
  for (const char* f : {"MERGE_HEAD", "MERGE_MSG", "MERGE_MODE", "CHERRY_PICK_HEAD", "REVERT_HEAD", "SQUASH_MSG", "AUTO_MERGE"})
    c.remove_git_file(f);
  if (!p.has("quiet")) print_new_commit(c, oid, amend || !reuse.empty() || p.has("date"), !amend && parents.empty());
  run_post_commit_hooks(c, amend, head ? *head : std::string(), oid);
  return 0;
}

void print_new_commit(Ctx& c, const std::string& oid, bool show_date, bool root) {
  std::string b = current_branch(c);
  auto cm = read_commit(c, oid);
  c.out << "[" << (b.empty() ? "detached HEAD" : b) << (root ? " (root-commit)" : "") << " " << c.abbrev(oid) << "] "
        << subject_of(cm.message) << "\n";
  std::string committer_who = cm.committer;
  if (cm.author != committer_who) c.out << " Author: " << cm.author << "\n";
  if (show_date) c.out << " Date: " << format_date((int64_t)cm.author_time, cm.author_tz, "default") << "\n";
  if (cm.parents.size() > 1) return;   // a merge's summary shows no diffstat
  std::string from = cm.parents.empty() ? "" : read_commit(c, cm.parents[0]).tree;
  print_commit_summary(c, from, cm.tree, c.out);
}

// ---------------------------------------------------------------------------
namespace {
// The index reset to a tree, keeping the stat data of entries that did not change.
std::vector<GitIndexEntry> index_from_tree(Ctx& c, const std::string& tree) {
  std::map<std::string, const GitIndexEntry*> old;
  for (auto& e : c.repo->index()) old[e.path] = &e;
  std::vector<GitIndexEntry> out;
  for (auto& [path, f] : snapshot_tree(c, tree)) {
    auto it = old.find(path);
    if (it != old.end() && it->second->oid == f.oid && it->second->mode == f.mode) out.push_back(*it->second);
    else out.push_back(GitIndexEntry{path, f.oid, f.mode, 0, 0});
  }
  return out;
}
void print_unstaged(Ctx& c) {
  auto wt = c.repo->worktree_changes();
  if (!wt.ok() || wt->empty()) return;
  c.out << "Unstaged changes after reset:\n";
  for (auto& ch : *wt) c.out << (ch.change == GitChange::Deleted ? "D" : "M") << "\t" << ch.path << "\n";
}
}  // namespace

int cmd_reset(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{0, "soft"}, {0, "mixed"}, {0, "hard"}, {0, "merge"}, {0, "keep"}, {'q', "quiet"},
                                         {'N', "intent-to-add"}, {0, "pathspec-from-file", Arg::Required}, {'p', "patch"},
                                         {0, "recurse-submodules", Arg::Optional}, {0, "no-refresh"}, {0, "refresh"}});
  c.open_repo();
  if (p.has("patch")) unsupported(c, "reset -p");
  if (p.has("keep")) unsupported(c, "reset --keep");
  if (p.has("pathspec-from-file") || p.has("intent-to-add")) unsupported(c, "reset --pathspec-from-file/-N");
  std::vector<std::string> pos = p.pos;
  std::string rev = "HEAD";
  std::vector<std::string> paths = p.after_dashdash;
  if (!pos.empty()) {
    auto r = resolve(c, pos[0]);
    bool is_rev = r && !peel_to(c, *r, "tree").empty();
    if (p.dashdash) {
      if (pos.size() > 1) usage(c, "reset takes one revision before --");
      rev = pos[0];
      pos.clear();
    } else if (is_rev) {
      if (pos.size() == 1 && pos[0] != "HEAD" && c.host().exists(c.abs_path(pos[0])))
        die(c, "ambiguous argument '" + pos[0] + "': both revision and filename\nUse '--' to separate paths from revisions, like this:\n'git <command> [<revision>...] -- [<file>...]'");
      rev = pos[0];
      pos.erase(pos.begin());
    }
    paths.insert(paths.begin(), pos.begin(), pos.end());
  }
  auto head = read_ref(c, "HEAD");
  if (!paths.empty()) {
    if (p.has("soft") || p.has("hard") || p.has("merge")) die(c, std::string("Cannot do ") + (p.has("soft") ? "soft" : p.has("hard") ? "hard" : "merge") + " reset with paths.");
    std::string tree = head || rev != "HEAD" ? peel_to(c, must_resolve(c, rev), "tree") : empty_tree_oid();
    if (tree.empty()) die(c, "Failed to resolve '" + rev + "' as a valid tree.");
    Pathspec ps = make_pathspec(c, paths);
    Snapshot t = snapshot_tree(c, tree);
    std::vector<GitIndexEntry> next;
    for (auto& e : c.repo->index()) if (!ps.match(e.path)) next.push_back(e);
    for (auto& [path, f] : t) {
      if (!ps.match(path)) continue;
      const GitIndexEntry* old = nullptr;
      for (auto& e : c.repo->index()) if (e.path == path) old = &e;
      if (old && old->oid == f.oid && old->mode == f.mode) next.push_back(*old);
      else next.push_back(GitIndexEntry{path, f.oid, f.mode, 0, 0});
    }
    std::vector<GitIndexEntry> still;
    for (auto& u : c.repo->unmerged()) if (!ps.match(u.path)) still.push_back(u);
    c.repo->set_unmerged(still);
    c.repo->set_index(next);
    if (auto w = c.repo->write_index(); !w.ok()) die(c, w.error().message);
    if (!p.has("quiet")) print_unstaged(c);
    return 0;
  }
  std::string target;
  if (rev == "HEAD" && !head) {
    // reset on an unborn branch: empty the index
    if (p.has("soft")) return 0;
    c.repo->set_index({});
    c.repo->set_unmerged({});
    if (auto w = c.repo->write_index(); !w.ok()) die(c, w.error().message);
    return 0;
  }
  target = commit_of(c, rev);
  std::string tree = read_commit(c, target).tree;
  if (head) c.write_git_file("ORIG_HEAD", *head + "\n");
  if (p.has("soft")) {
    if (!c.repo->unmerged().empty() || c.read_git_file("MERGE_HEAD")) die(c, "Cannot do a soft reset in the middle of a merge.");
    update_ref_checked(c, "HEAD", target, std::nullopt, "reset: moving to " + rev);
    return 0;
  }
  if (p.has("hard")) {
    auto r = c.repo->checkout_tree(tree, true);
    if (!r.ok()) die(c, "Could not reset index file to revision '" + rev + "'.\n" + r.error().message);
    update_ref_checked(c, "HEAD", target, std::nullopt, "reset: moving to " + rev);
    for (const char* f : {"MERGE_HEAD", "MERGE_MSG", "MERGE_MODE", "CHERRY_PICK_HEAD", "REVERT_HEAD", "AUTO_MERGE"}) c.remove_git_file(f);
    if (!p.has("quiet")) c.out << "HEAD is now at " << c.abbrev(target) << " " << subject_of(read_commit(c, target).message) << "\n";
    return 0;
  }
  if (p.has("merge")) {
    // Paths the index changed (a merge's results and conflicts) go back to the target; local changes
    // to anything else stay.
    Snapshot cur = snapshot_index(c);
    Snapshot t = snapshot_tree(c, tree);
    std::set<std::string> touched;
    for (auto& u : c.repo->unmerged()) touched.insert(u.path);
    Snapshot hs = snapshot_tree(c, head ? read_commit(c, *head).tree : "");
    for (auto& [path, f] : cur) {
      auto h2 = hs.find(path);
      if (h2 == hs.end() || h2->second.oid != f.oid || h2->second.mode != f.mode) touched.insert(path);
      auto t2 = t.find(path);
      if (t2 == t.end() || t2->second.oid != f.oid) touched.insert(path);
    }
    for (auto& [path, f] : hs) if (!cur.count(path)) touched.insert(path);
    for (auto& path : touched) {
      auto t2 = t.find(path);
      if (t2 == t.end()) { (void)c.host().remove(join(c.top, path)); continue; }
      auto w = c.repo->write_worktree_file(path, t2->second.oid, t2->second.mode);
      if (!w.ok()) die(c, w.error().message);
    }
    std::vector<GitIndexEntry> next = index_from_tree(c, tree);
    for (auto& e : next) {
      if (!touched.count(e.path)) continue;
      auto st = c.host().stat(join(c.top, e.path));
      if (st.ok()) { e.mtime_ns = st->mtime_ns; e.size = st->size; }
    }
    c.repo->set_unmerged({});
    c.repo->set_index(next);
    if (auto w = c.repo->write_index(); !w.ok()) die(c, w.error().message);
    update_ref_checked(c, "HEAD", target, std::nullopt, "reset: moving to " + rev);
    for (const char* f : {"MERGE_HEAD", "MERGE_MSG", "MERGE_MODE", "CHERRY_PICK_HEAD", "REVERT_HEAD", "AUTO_MERGE"}) c.remove_git_file(f);
    return 0;
  }
  // mixed
  c.repo->set_unmerged({});
  c.repo->set_index(index_from_tree(c, tree));
  if (auto w = c.repo->write_index(); !w.ok()) die(c, w.error().message);
  update_ref_checked(c, "HEAD", target, std::nullopt, "reset: moving to " + rev);
  for (const char* f : {"MERGE_HEAD", "MERGE_MSG", "MERGE_MODE", "CHERRY_PICK_HEAD", "REVERT_HEAD", "AUTO_MERGE"}) c.remove_git_file(f);
  if (!p.has("quiet")) print_unstaged(c);
  return 0;
}

// ---------------------------------------------------------------------------
int cmd_restore(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{'s', "source", Arg::Required}, {'S', "staged"}, {'W', "worktree"}, {'q', "quiet"},
                                         {0, "ours"}, {0, "theirs"}, {'p', "patch"}, {'m', "merge"}, {0, "overlay"},
                                         {0, "ignore-unmerged"}, {0, "pathspec-from-file", Arg::Required}, {0, "progress"},
                                         {0, "conflict", Arg::Required}, {0, "recurse-submodules"}});
  c.open_repo();
  if (p.has("patch") || p.has("merge") || p.has("conflict") || p.has("pathspec-from-file") || p.has("overlay"))
    unsupported(c, "restore -p/--merge/--conflict/--overlay/--pathspec-from-file");
  std::vector<std::string> paths = p.pos;
  paths.insert(paths.end(), p.after_dashdash.begin(), p.after_dashdash.end());
  if (paths.empty()) die(c, "you must specify path(s) to restore");
  bool staged = p.has("staged"), worktree = p.has("worktree") || !staged;
  Pathspec ps = make_pathspec(c, paths);
  std::vector<bool> matched(ps.items.size(), false);
  auto mark = [&](const std::string& path) { int i = ps.matched_index(path); if (i >= 0) matched[(size_t)i] = true; };
  Snapshot src;
  bool from_index = !staged && !p.has("source");
  if (from_index) src = snapshot_index(c);
  else {
    std::string rev = p.get("source", "HEAD");
    auto head = read_ref(c, "HEAD");
    std::string tree = (rev == "HEAD" && !head) ? "" : peel_to(c, must_resolve(c, rev), "tree");
    if (rev != "HEAD" && tree.empty()) die(c, "could not resolve " + rev);
    src = snapshot_tree(c, tree);
  }
  // ours/theirs pick a stage for unmerged paths
  if (p.has("ours") || p.has("theirs")) {
    int stage = p.has("ours") ? 2 : 3;
    for (auto& u : c.repo->unmerged()) if (u.stage == stage && ps.match(u.path)) src[u.path] = FileState{u.oid, u.mode, true};
  }
  std::vector<GitIndexEntry> index = c.repo->index();
  std::set<std::string> known;
  for (auto& e : index) known.insert(e.path);
  for (auto& u : c.repo->unmerged()) known.insert(u.path);
  for (auto& [path, f] : src) known.insert(path);
  for (auto& k : known) if (ps.match(k)) mark(k);
  for (size_t i = 0; i < matched.size(); ++i)
    if (!matched[i]) { c.err << "error: pathspec '" << ps.raw_of(i) << "' did not match any file(s) known to git\n"; return 1; }
  if (worktree && !p.has("ours") && !p.has("theirs") && !p.has("ignore-unmerged") && !staged) {
    for (auto& u : c.repo->unmerged())
      if (ps.match(u.path)) { c.err << "error: path '" << u.path << "' is unmerged\n"; return 1; }
  }
  if (staged) {
    std::vector<GitIndexEntry> next;
    for (auto& e : index) if (!ps.match(e.path)) next.push_back(e);
    for (auto& [path, f] : src) {
      if (!ps.match(path)) continue;
      const GitIndexEntry* old = nullptr;
      for (auto& e : index) if (e.path == path) old = &e;
      if (old && old->oid == f.oid && old->mode == f.mode) next.push_back(*old);
      else next.push_back(GitIndexEntry{path, f.oid, f.mode, 0, 0});
    }
    std::vector<GitIndexEntry> still;
    for (auto& u : c.repo->unmerged()) if (!ps.match(u.path)) still.push_back(u);
    c.repo->set_unmerged(still);
    index = next;
    c.repo->set_index(index);
  }
  if (worktree) {
    std::map<std::string, size_t> at;
    for (size_t i = 0; i < index.size(); ++i) at[index[i].path] = i;
    for (auto& k : known) {
      if (!ps.match(k)) continue;
      auto f = src.find(k);
      if (f == src.end()) {
        // not in the source: a tracked file disappears from the work tree (only with --source/--staged)
        if (!from_index) (void)c.host().remove(join(c.top, k));
        continue;
      }
      auto it = at.find(k);
      if (from_index && it != at.end() && c.repo->worktree_matches(index[it->second])) continue;
      auto w = c.repo->write_worktree_file(k, f->second.oid, f->second.mode);
      if (!w.ok()) die(c, "unable to write " + k + ": " + w.error().message);
      if (it != at.end() && index[it->second].oid == w->oid) { index[it->second].mtime_ns = w->mtime_ns; index[it->second].size = w->size; }
    }
    c.repo->set_index(index);
  }
  if (auto w = c.repo->write_index(); !w.ok()) die(c, w.error().message);
  return 0;
}

// ---------------------------------------------------------------------------
int cmd_clean(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{'n', "dry-run"}, {'f', "force"}, {'d', ""}, {'x', ""}, {'X', ""}, {'q', "quiet"},
                                         {'e', "exclude", Arg::Required}, {'i', "interactive"}});
  c.open_repo();
  if (p.has("interactive")) unsupported(c, "clean -i");
  if (p.has("exclude")) unsupported(c, "clean -e");
  int force = p.count("force");
  bool dry = p.has("dry-run");
  if (!force && !dry) {
    auto rf = c.config.get("clean.requireforce");
    if (!rf) die(c, "clean.requireForce defaults to true and neither -i, -n, nor -f given; refusing to clean");
    if (c.config.get_bool("clean.requireforce", true))
      die(c, "clean.requireForce set to true and neither -i, -n, nor -f given; refusing to clean");
  }
  bool dirs = p.has("d"), with_ignored = p.has("x"), only_ignored = p.has("X");
  std::vector<std::string> paths = p.pos;
  paths.insert(paths.end(), p.after_dashdash.begin(), p.after_dashdash.end());
  Pathspec ps = make_pathspec(c, paths);
  std::set<std::string> tracked, tracked_dirs;
  for (auto& e : c.repo->index()) {
    tracked.insert(e.path);
    for (size_t s = e.path.find('/'); s != std::string::npos; s = e.path.find('/', s + 1)) tracked_dirs.insert(e.path.substr(0, s));
  }
  for (auto& u : c.repo->unmerged()) tracked.insert(u.path);
  auto& h = c.host();
  std::vector<std::string> removed;
  std::function<void(const std::string&)> rmtree = [&](const std::string& full) {
    auto st = h.stat(full);
    if (!st.ok()) return;
    if (st->is_dir && !st->is_link) {
      auto names = h.list_dir(full);
      if (names.ok()) for (auto& n : *names) if (n != "." && n != "..") rmtree(full + "/" + n);
    }
    (void)h.remove(full);
  };
  // returns true when everything under `dir` was (or would be) removed
  std::function<bool(const std::string&, bool)> walk_dir = [&](const std::string& dir, bool in_ignored) -> bool {
    auto names = h.list_dir(join(c.top, dir));
    if (!names.ok()) return false;
    std::vector<std::string> sorted = *names;
    std::sort(sorted.begin(), sorted.end());
    bool all_gone = true;
    for (auto& n : sorted) {
      if (n == "." || n == ".." || (dir.empty() && n == ".git")) continue;
      std::string rp = dir.empty() ? n : dir + "/" + n;
      auto st = h.stat(join(c.top, rp));
      if (!st.ok()) continue;
      bool is_dir = st->is_dir && !st->is_link;
      if (!is_dir) {
        if (tracked.count(rp)) { all_gone = false; continue; }
        bool ign = in_ignored || c.repo->is_ignored(rp, false);
        bool take = only_ignored ? ign : (with_ignored || !ign);
        if (!take || !ps.match(rp)) { all_gone = false; continue; }
        removed.push_back(rp);
        if (!dry) (void)h.remove(join(c.top, rp));
        continue;
      }
      if (tracked_dirs.count(rp)) { if (!walk_dir(rp, in_ignored)) all_gone = false; continue; }
      // wholly untracked directory
      if (h.exists(join(c.top, rp) + "/.git") && force < 2) { all_gone = false; continue; }
      bool ign = in_ignored || c.repo->is_ignored(rp, true);
      if (!dirs) {
        // without -d, git leaves untracked directories alone (pathspec naming files inside aside)
        if (!ps.empty() && ps.may_match_below(rp) && !ps.match(rp)) { if (!walk_dir(rp, ign)) all_gone = false; }
        else all_gone = false;
        continue;
      }
      if (ign && !with_ignored && !only_ignored) { all_gone = false; continue; }
      if (!ps.match(rp) && !ps.may_match_below(rp)) { all_gone = false; continue; }
      if ((ign && (with_ignored || only_ignored)) || (!ign && !only_ignored && ps.match(rp))) {
        // remove it whole, unless something inside must stay
        bool keep_inside = false;
        if (!ign && !with_ignored) {
          std::function<bool(const std::string&)> has_ignored = [&](const std::string& d) -> bool {
            auto ns = h.list_dir(join(c.top, d));
            if (!ns.ok()) return false;
            for (auto& x : *ns) {
              if (x == "." || x == "..") continue;
              std::string r2 = d + "/" + x;
              auto s2 = h.stat(join(c.top, r2));
              if (!s2.ok()) continue;
              bool d2 = s2->is_dir && !s2->is_link;
              if (c.repo->is_ignored(r2, d2)) return true;
              if (d2 && has_ignored(r2)) return true;
            }
            return false;
          };
          keep_inside = has_ignored(rp);
        }
        if (!keep_inside) {
          removed.push_back(rp + "/");
          if (!dry) rmtree(join(c.top, rp));
          continue;
        }
      }
      if (!walk_dir(rp, ign)) all_gone = false;
      else if (!dry) (void)h.remove(join(c.top, rp));
    }
    return all_gone;
  };
  walk_dir("", false);
  if (!p.has("quiet") || dry)
    for (auto& r : removed) c.out << (dry ? "Would remove " : "Removing ") << qp(c, c.to_display_path(r)) << "\n";
  return 0;
}

}  // namespace aios_git
