// diff-tree: compares the contents and mode of trees (raw, name-only, name-status, patch and stat forms).
#include <algorithm>
#include <cctype>

#include "aios/hal/host.hpp"
#include "hist_util.hpp"

namespace aios_git {

using namespace aios;
using namespace aios::userland;

namespace {

const char* kDiffTreeUsage =
    "usage: git diff-tree [--stdin] [-m] [-s] [-v] [--no-commit-id] [--pretty]\n"
    "              [-t] [-r] [-c | --cc] [--combined-all-paths] [--root] [--merge-base]\n"
    "              [<common-diff-options>] <tree-ish> [<tree-ish>] [<path>...]\n"
    "\n"
    "  -r            diff recursively\n"
    "  -c            show combined diff for merge commits\n"
    "  --cc          show combined diff for merge commits removing uninteresting hunks\n"
    "  --combined-all-paths\n"
    "                show name of file in all parents for combined diffs\n"
    "  --root        include the initial commit as diff against /dev/null\n"
    "\n"
    "common diff options:\n"
    "  -z            output diff-raw with lines terminated with NUL.\n"
    "  -p            output patch format.\n"
    "  -u            synonym for -p.\n"
    "  --patch-with-raw\n"
    "                output both a patch and the diff-raw format.\n"
    "  --stat        show diffstat instead of patch.\n"
    "  --numstat     show numeric diffstat instead of patch.\n"
    "  --patch-with-stat\n"
    "                output a patch and prepend its diffstat.\n"
    "  --name-only   show only names of changed files.\n"
    "  --name-status show names and status of changed files.\n"
    "  --full-index  show full object name on index lines.\n"
    "  --abbrev=<n>  abbreviate object names in diff-tree header and diff-raw.\n"
    "  -R            swap input file pairs.\n"
    "  -B            detect complete rewrites.\n"
    "  -M            detect renames.\n"
    "  -C            detect copies.\n"
    "  --find-copies-harder\n"
    "                try unchanged files as candidate for copy detection.\n"
    "  -l<n>         limit rename attempts up to <n> paths.\n"
    "  -O<file>      reorder diffs according to the <file>.\n"
    "  -S<string>    find filepair whose only one side contains the string.\n"
    "  --pickaxe-all\n"
    "                show all files diff when -S is used and hit is found.\n"
    "  -a  --text    treat all files as text.\n"
    "\n";

bool is_hex_oid(const std::string& s) {
  if (s.size() != 40) return false;
  for (char ch : s) if (!std::isxdigit((unsigned char)ch)) return false;
  return true;
}

std::string mode6(uint32_t m) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%06o", m);
  return buf;
}

// git's tree order: a directory's name sorts as if it ended in '/'.
int entry_order(const GitTreeEntry& a, const GitTreeEntry& b) {
  std::string x = a.name + (a.is_dir() ? "/" : ""), y = b.name + (b.is_dir() ? "/" : "");
  return x < y ? -1 : x > y ? 1 : 0;
}

class DiffTree {
 public:
  explicit DiffTree(Ctx& c) : c_(c) {}
  int run();

 private:
  Ctx& c_;
  bool recursive_ = false, show_trees_ = false, root_ = false, no_commit_id_ = false, merges_ = false, quiet_ = false;
  bool stdin_ = false, nul_ = false, full_index_ = false, name_only_ = false, name_status_ = false, raw_ = false;
  bool patch_ = false, stat_ = false, numstat_ = false, shortstat_ = false, summary_ = false, renames_ = false;
  int abbrev_ = 0;     // 0: full names
  int rename_threshold_ = 50;
  std::string filter_;
  Pathspec ps_;
  DiffOptions dopt_;

  void collect(const std::string& a, const std::string& b, const std::string& base, std::vector<DiffPair>& out);
  void entry_pair(const GitTreeEntry* a, const GitTreeEntry* b, const std::string& base, std::vector<DiffPair>& out);
  void whole(const GitTreeEntry& e, const std::string& base, bool added, std::vector<DiffPair>& out);
  bool interesting(const std::string& path, bool is_tree) const;
  std::vector<DiffPair> diff_trees(const std::string& a, const std::string& b);
  bool show_diff(const std::vector<DiffPair>& pairs, const std::string& header);
  void commit_diff(const std::string& commit, const std::vector<std::string>* fake_parents);
  void tree_pair(const std::string& a, const std::string& b);
  std::string oid_text(const FileState& f) const;
  void print_raw(const std::vector<DiffPair>& pairs);
};

bool DiffTree::interesting(const std::string& path, bool is_tree) const {
  if (ps_.empty()) return true;
  if (ps_.match(path)) return true;
  return is_tree && ps_.may_match_below(path);
}

std::string DiffTree::oid_text(const FileState& f) const {
  if (!f.present) return std::string(abbrev_ ? (size_t)abbrev_ : 40, '0');
  return abbrev_ ? c_.abbrev(f.oid, abbrev_) : f.oid;
}

void DiffTree::whole(const GitTreeEntry& e, const std::string& base, bool added, std::vector<DiffPair>& out) {
  std::string path = base + e.name;
  if (!interesting(path, e.is_dir())) return;
  if (e.is_dir() && recursive_) {
    if (show_trees_) {
      DiffPair p;
      p.status = added ? 'A' : 'D';
      p.old_path = p.new_path = path;
      (added ? p.b : p.a) = FileState{e.oid, e.mode, true};
      out.push_back(p);
    }
    auto ents = c_.repo->read_tree(e.oid);
    if (!ents.ok()) die(c_, ents.error().message);
    for (auto& s : *ents) whole(s, path + "/", added, out);
    return;
  }
  if (!e.is_dir() && !ps_.empty() && !ps_.match(path)) return;
  DiffPair p;
  p.status = added ? 'A' : 'D';
  p.old_path = p.new_path = path;
  (added ? p.b : p.a) = FileState{e.oid, e.mode, true};
  out.push_back(p);
}

void DiffTree::entry_pair(const GitTreeEntry* a, const GitTreeEntry* b, const std::string& base, std::vector<DiffPair>& out) {
  if (!a) { whole(*b, base, true, out); return; }
  if (!b) { whole(*a, base, false, out); return; }
  std::string path = base + a->name;
  if (a->oid == b->oid && a->mode == b->mode) return;
  if (!interesting(path, a->is_dir())) return;
  if (a->is_dir()) {
    if (recursive_) {
      if (show_trees_) {
        DiffPair p;
        p.status = 'M';
        p.old_path = p.new_path = path;
        p.a = FileState{a->oid, a->mode, true};
        p.b = FileState{b->oid, b->mode, true};
        out.push_back(p);
      }
      collect(a->oid, b->oid, path + "/", out);
      return;
    }
  } else if (!ps_.empty() && !ps_.match(path)) {
    return;
  }
  DiffPair p;
  const bool link_a = (a->mode & 0170000) == 0120000, link_b = (b->mode & 0170000) == 0120000;
  p.status = (!a->is_dir() && link_a != link_b) ? 'T' : 'M';
  p.old_path = p.new_path = path;
  p.a = FileState{a->oid, a->mode, true};
  p.b = FileState{b->oid, b->mode, true};
  out.push_back(p);
}

void DiffTree::collect(const std::string& a_tree, const std::string& b_tree, const std::string& base, std::vector<DiffPair>& out) {
  std::vector<GitTreeEntry> ea, eb;
  if (!a_tree.empty()) { auto r = c_.repo->read_tree(a_tree); if (r.ok()) ea = *r; }
  if (!b_tree.empty()) { auto r = c_.repo->read_tree(b_tree); if (r.ok()) eb = *r; }
  size_t i = 0, j = 0;
  while (i < ea.size() || j < eb.size()) {
    if (j >= eb.size()) { entry_pair(&ea[i++], nullptr, base, out); continue; }
    if (i >= ea.size()) { entry_pair(nullptr, &eb[j++], base, out); continue; }
    int cmp = entry_order(ea[i], eb[j]);
    if (cmp < 0) entry_pair(&ea[i++], nullptr, base, out);
    else if (cmp > 0) entry_pair(nullptr, &eb[j++], base, out);
    else { entry_pair(&ea[i], &eb[j], base, out); ++i; ++j; }
  }
}

std::vector<DiffPair> DiffTree::diff_trees(const std::string& a, const std::string& b) {
  std::vector<DiffPair> pairs;
  if (renames_ && recursive_) {
    DiffOptions o = dopt_;
    o.renames = true;
    o.rename_threshold = rename_threshold_;
    pairs = aios_git::diff_trees(c_, a, b, ps_, o);
    if (show_trees_) {
      std::vector<DiffPair> all;
      collect(a, b, "", all);
      for (auto& p : all) if (p.a.mode == 040000 || p.b.mode == 040000) pairs.push_back(p);
      std::stable_sort(pairs.begin(), pairs.end(), [](const DiffPair& x, const DiffPair& y) {
        const std::string& px = x.status == 'D' ? x.old_path : x.new_path;
        const std::string& py = y.status == 'D' ? y.old_path : y.new_path;
        return px < py;
      });
    }
    return pairs;
  }
  collect(a, b, "", pairs);
  return pairs;
}

void DiffTree::print_raw(const std::vector<DiffPair>& pairs) {
  const char term = nul_ ? '\0' : '\n';
  auto shown = [&](const std::string& p) { return nul_ ? p : quote_path(p); };
  for (auto& p : pairs) {
    if (name_only_) { c_.out << shown(p.status == 'D' ? p.old_path : p.new_path) << term; continue; }
    if (name_status_) {
      if (p.status == 'R' || p.status == 'C') {
        char sc[8];
        std::snprintf(sc, sizeof sc, "%c%03d", p.status, p.score);
        c_.out << sc << (nul_ ? '\0' : '\t') << shown(p.old_path) << (nul_ ? '\0' : '\t') << shown(p.new_path) << term;
      } else {
        c_.out << p.status << (nul_ ? '\0' : '\t') << shown(p.status == 'D' ? p.old_path : p.new_path) << term;
      }
      continue;
    }
    c_.out << ":" << mode6(p.a.present ? p.a.mode : 0) << " " << mode6(p.b.present ? p.b.mode : 0) << " " << oid_text(p.a) << " " << oid_text(p.b) << " ";
    if (p.status == 'R' || p.status == 'C') {
      char sc[8];
      std::snprintf(sc, sizeof sc, "%c%03d", p.status, p.score);
      c_.out << sc << (nul_ ? '\0' : '\t') << shown(p.old_path) << (nul_ ? '\0' : '\t') << shown(p.new_path) << term;
    } else {
      c_.out << p.status << (nul_ ? '\0' : '\t') << shown(p.status == 'D' ? p.old_path : p.new_path) << term;
    }
  }
}

bool DiffTree::show_diff(const std::vector<DiffPair>& all, const std::string& header) {
  std::vector<DiffPair> pairs;
  for (auto& p : all) {
    bool keep = true;
    if (!filter_.empty()) {
      bool any_upper = false, hit_upper = false, hit_lower = false;
      for (char ch : filter_) {
        if (std::isupper((unsigned char)ch)) { any_upper = true; if (ch == p.status) hit_upper = true; }
        else if (std::toupper((unsigned char)ch) == p.status) hit_lower = true;
      }
      keep = (!any_upper || hit_upper) && !hit_lower;
    }
    if (keep) pairs.push_back(p);
  }
  if (pairs.empty()) return false;
  if (!header.empty() && !no_commit_id_) c_.out << header << (nul_ ? '\0' : '\n');
  if (quiet_) return true;
  const bool want_patch = patch_ || stat_ || numstat_ || shortstat_ || summary_;
  if (raw_ || name_only_ || name_status_ || !want_patch) print_raw(pairs);
  if (want_patch) {
    DiffOptions o = dopt_;
    o.full_index = full_index_;
    o.z = nul_;
    o.renames = false;
    std::vector<DiffOptions::Format> chosen;
    if (stat_) chosen.push_back(DiffOptions::Stat);
    if (numstat_) chosen.push_back(DiffOptions::NumStat);
    if (shortstat_) chosen.push_back(DiffOptions::ShortStat);
    if (summary_) chosen.push_back(DiffOptions::Summary);
    if (patch_) chosen.push_back(DiffOptions::Patch);
    o.format = chosen[0];
    o.extra.assign(chosen.begin() + 1, chosen.end());
    if ((raw_ || name_only_ || name_status_) && (patch_ || stat_)) c_.out << "\n";
    print_diff(c_, pairs, o, c_.out);
  }
  return true;
}

void DiffTree::tree_pair(const std::string& a, const std::string& b) {
  auto pairs = diff_trees(a, b);
  show_diff(pairs, "");
}

void DiffTree::commit_diff(const std::string& oid, const std::vector<std::string>* fake_parents) {
  GitCommit cm = read_commit(c_, oid);
  std::vector<std::string> parents = fake_parents ? *fake_parents : cm.parents;
  if (parents.empty()) {
    if (!root_) return;
    show_diff(diff_trees("", cm.tree), oid);
    return;
  }
  if (parents.size() > 1) {
    if (!merges_) return;
    for (auto& p : parents) {
      GitCommit pc = read_commit(c_, p);
      show_diff(diff_trees(pc.tree, cm.tree), oid);
    }
    return;
  }
  GitCommit pc = read_commit(c_, parents[0]);
  show_diff(diff_trees(pc.tree, cm.tree), oid);
}

int DiffTree::run() {
  std::vector<std::string> args(c_.args.begin() + 1, c_.args.end());
  std::vector<std::string> operands;
  bool dashdash = false, abbrev_auto = false;
  std::vector<std::string> paths_after;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (dashdash) { paths_after.push_back(a); continue; }
    if (a == "--") { dashdash = true; continue; }
    if (a == "-h") { c_.out << kDiffTreeUsage; throw Exit{129}; }
    if (a == "-r") recursive_ = true;
    else if (a == "-t") { show_trees_ = true; recursive_ = true; }
    else if (a == "--root") root_ = true;
    else if (a == "--no-root") root_ = false;
    else if (a == "--no-commit-id") no_commit_id_ = true;
    else if (a == "--stdin") stdin_ = true;
    else if (a == "-m") merges_ = true;
    else if (a == "-s" || a == "--no-patch") quiet_ = true;
    else if (a == "-z") nul_ = true;
    else if (a == "--raw") raw_ = true;
    else if (a == "-p" || a == "-u" || a == "--patch") patch_ = true;
    else if (a == "--patch-with-raw") { patch_ = true; raw_ = true; }
    else if (a == "--patch-with-stat") { patch_ = true; stat_ = true; }
    else if (a == "--stat" || a.rfind("--stat=", 0) == 0) stat_ = true;
    else if (a == "--numstat") numstat_ = true;
    else if (a == "--shortstat") shortstat_ = true;
    else if (a == "--summary") summary_ = true;
    else if (a == "--name-only") name_only_ = true;
    else if (a == "--name-status") name_status_ = true;
    else if (a == "--full-index") full_index_ = true;
    else if (a == "--binary") { dopt_.binary = true; full_index_ = true; }
    else if (a == "--no-abbrev") abbrev_ = 0;
    else if (a == "--abbrev") abbrev_auto = true;
    else if (a.rfind("--abbrev=", 0) == 0) { int v = std::atoi(a.c_str() + 9); abbrev_ = v < 4 ? 4 : v > 40 ? 40 : v; }
    else if (a.rfind("--diff-filter=", 0) == 0) filter_ = a.substr(14);
    else if (a == "-M" || a == "--find-renames") renames_ = true;
    else if (a.rfind("-M", 0) == 0 || a.rfind("--find-renames=", 0) == 0) {
      renames_ = true;
      std::string v = a[1] == 'M' ? a.substr(2) : a.substr(15);
      int n = std::atoi(v.c_str());
      if (v.find('%') != std::string::npos || n >= 10) rename_threshold_ = n > 100 ? 100 : n;
      else rename_threshold_ = n * 10;
    }
    else if (a == "--no-renames") renames_ = false;
    else if (a == "-c" || a == "--cc" || a == "--combined-all-paths") unsupported(c_, "diff-tree " + a + " (combined diffs)");
    else if (a == "-v" || a == "--verbose-header" || a == "--pretty" || a.rfind("--pretty=", 0) == 0 || a.rfind("--format=", 0) == 0 ||
             a == "--oneline" || a == "--no-commit-id=")
      unsupported(c_, "diff-tree " + a + " (commit message headers)");
    else if (a == "--merge-base") unsupported(c_, "diff-tree --merge-base");
    else if (a == "-C" || a.rfind("-C", 0) == 0 || a == "--find-copies" || a == "--find-copies-harder" || a == "-R" || a == "-B" || a == "-a" ||
             a == "--text" || a.rfind("-S", 0) == 0 || a.rfind("-G", 0) == 0 || a.rfind("-O", 0) == 0 || a.rfind("-l", 0) == 0 ||
             a == "--pickaxe-all" || a.rfind("-U", 0) == 0 || a.rfind("--unified", 0) == 0)
      unsupported(c_, "diff-tree " + a);
    else if (a.size() > 1 && a[0] == '-') {
      c_.err << kDiffTreeUsage;
      throw Exit{129};
    } else operands.push_back(a);
  }
  c_.open_repo(false);
  if (abbrev_auto) abbrev_ = c_.default_abbrev();
  dopt_.renames = false;
  if (patch_ || stat_ || numstat_ || shortstat_ || summary_) recursive_ = true;
  std::vector<std::string> trees;
  std::vector<std::string> path_args = paths_after;
  size_t nrev = 0;
  std::vector<std::string> revs;
  if (!stdin_) {
    for (auto& a : operands) {
      if (nrev < 2) {
        std::string why;
        auto o = resolve(c_, a, &why);
        if (o) { revs.push_back(*o); ++nrev; continue; }
        auto st = c_.host().stat(c_.abs_path(a));
        if (nrev >= 1 && st.ok()) { path_args.push_back(a); continue; }
        if (!st.ok() && !dashdash) {
          c_.err << "fatal: ambiguous argument '" << a << "': unknown revision or path not in the working tree.\n"
                 << "Use '--' to separate paths from revisions, like this:\n"
                 << "'git <command> [<revision>...] -- [<file>...]'\n";
          return 128;
        }
        path_args.push_back(a);
      } else {
        path_args.push_back(a);
      }
    }
    if (revs.empty()) { c_.err << kDiffTreeUsage; return 129; }
  } else {
    for (auto& a : operands) path_args.push_back(a);
  }
  if (!path_args.empty()) ps_ = make_pathspec(c_, path_args);
  auto peel_tree = [&](const std::string& oid) {
    std::string t = peel_to(c_, oid, "tree");
    if (t.empty()) die(c_, "unable to read tree " + oid);
    return t;
  };
  if (!stdin_) {
    if (revs.size() == 2) {
      tree_pair(peel_tree(revs[0]), peel_tree(revs[1]));
      return 0;
    }
    std::string kind = type_of(c_, revs[0]);
    if (kind != "commit" && kind != "tag") {
      c_.err << "error: object " << revs[0] << " is a " << kind << ", not a commit\n";
      return 0;
    }
    std::string commit = peel_to(c_, revs[0], "commit");
    if (commit.empty()) die(c_, "unable to read commit " + revs[0]);
    commit_diff(commit, nullptr);
    return 0;
  }
  std::string line;
  while (std::getline(c_.in, line)) {
    const bool had_nl = !c_.in.eof();
    std::string text = line;
    if (!is_hex_oid(text.substr(0, 40)) || (text.size() > 40 && text[40] != ' ')) {
      c_.out << line << (had_nl ? "\n" : "");
      c_.out.flush();
      continue;
    }
    if (!had_nl) continue;
    std::vector<std::string> ids;
    size_t pos = 0;
    while (pos + 40 <= text.size()) {
      std::string id = text.substr(pos, 40);
      if (!is_hex_oid(id)) break;
      ids.push_back(id);
      pos += 40;
      if (pos < text.size() && text[pos] == ' ') ++pos;
    }
    std::string first_type = type_of(c_, ids[0]);
    if (first_type == "commit") {
      std::vector<std::string> parents(ids.begin() + 1, ids.end());
      commit_diff(ids[0], parents.empty() ? nullptr : &parents);
    } else if (first_type == "tree") {
      if (ids.size() != 2) { c_.err << "error: Need exactly two trees, separated by a space\n"; continue; }
      show_diff(diff_trees(ids[0], ids[1]), ids[0] + " " + ids[1]);
    } else {
      c_.err << "error: Object " << ids[0] << " is a " << first_type << ", not a commit or tree\n";
    }
    c_.out.flush();
  }
  return 0;
}

}  // namespace

int cmd_diff_tree(Ctx& c) {
  DiffTree d(c);
  return d.run();
}

}  // namespace aios_git
