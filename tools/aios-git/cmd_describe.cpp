// describe: a commit named by the nearest tag that reaches it.
#include <algorithm>
#include <cctype>
#include <unordered_map>

#include "hist_util.hpp"

namespace aios_git {

using namespace aios;
using namespace aios::userland;

namespace {

const char* kDescribeUsage =
    "usage: git describe [--all] [--tags] [--contains] [--abbrev=<n>] [<commit-ish>...]\n"
    "   or: git describe [--all] [--tags] [--contains] [--abbrev=<n>] --dirty[=<mark>]\n"
    "   or: git describe <blob>\n"
    "\n"
    "    --[no-]contains       find the tag that comes after the commit\n"
    "    --[no-]debug          debug search strategy on stderr\n"
    "    --[no-]all            use any ref\n"
    "    --[no-]tags           use any tag, even unannotated\n"
    "    --[no-]long           always use long format\n"
    "    --[no-]first-parent   only follow first parent\n"
    "    --[no-]abbrev[=<n>]   use <n> digits to display object names\n"
    "    --[no-]exact-match    only output exact matches\n"
    "    --[no-]candidates <n> consider <n> most recent tags (default: 10)\n"
    "    --[no-]match <pattern>\n"
    "                          only consider tags matching <pattern>\n"
    "    --[no-]exclude <pattern>\n"
    "                          do not consider tags matching <pattern>\n"
    "    --[no-]always         show abbreviated commit object as fallback\n"
    "    --dirty[=<mark>]      append <mark> on dirty working tree (default: \"-dirty\")\n"
    "    --broken[=<mark>]     append <mark> on broken working tree (default: \"-broken\")\n"
    "\n";

struct Named { std::string name; int prio = 0; std::string oid; int64_t tag_date = 0; bool tag_known = false; };
struct Possible { Named* name = nullptr; int depth = 0; uint32_t flag_within = 0; int found_order = 0; };

bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

class Describe {
 public:
  explicit Describe(Ctx& c) : c_(c) {}
  int run();

 private:
  Ctx& c_;
  bool all_ = false, tags_ = false, longformat_ = false, first_parent_ = false, always_ = false, contains_ = false, debug_ = false;
  int abbrev_ = -1;
  bool abbrev_given_ = false;
  int max_candidates_ = 10;
  std::vector<std::string> match_, exclude_;
  std::map<std::string, Named> names_;     // by peeled commit
  std::string suffix_;
  struct CommitInfo { int64_t date = 0; std::vector<std::string> parents; };
  std::unordered_map<std::string, CommitInfo> commits_;
  std::unordered_map<std::string, uint32_t> flags_;
  std::set<std::string> seen_;

  const CommitInfo& commit(const std::string& oid);
  void gather();
  bool replace(Named& e, int prio, const std::string& oid, int64_t date, bool date_ok);
  void describe_commit(const std::string& oid, const std::string& arg);
  std::string suffix_for(int depth, const std::string& oid);
  void insert_by_date(std::vector<std::string>& list, const std::string& oid);
  int finish_depth(std::vector<std::string>& list, Possible& best);
  int run_contains(const std::vector<std::string>& args);
};

const Describe::CommitInfo& Describe::commit(const std::string& oid) {
  auto it = commits_.find(oid);
  if (it != commits_.end()) return it->second;
  GitCommit cm = read_commit(c_, oid);
  CommitInfo ci;
  ci.date = (int64_t)cm.committer_time;
  ci.parents = cm.parents;
  return commits_.emplace(oid, std::move(ci)).first->second;
}

bool Describe::replace(Named& e, int prio, const std::string& oid, int64_t date, bool date_ok) {
  if (e.prio < prio) return true;
  if (e.prio == 2 && prio == 2) {
    if (!e.tag_known) {
      TagInfo ti;
      if (!read_tag_object(c_, e.oid, ti)) return true;
      e.tag_date = ti.has_date ? ti.date : 0;
      e.tag_known = true;
    }
    if (!date_ok) return false;
    if (e.tag_date < date) return true;
  }
  (void)oid;
  return false;
}

void Describe::gather() {
  for (auto& [path, oid] : all_refs(c_)) {
    const bool is_tag = starts_with(path, "refs/tags/");
    if (!all_ && !is_tag) continue;
    // patterns are matched against the name without its refs/tags/ (or heads/, remotes/) prefix
    std::string shown = path;
    if (is_tag) shown = path.substr(10);
    else if (starts_with(path, "refs/heads/")) shown = path.substr(11);
    else if (starts_with(path, "refs/remotes/")) shown = path.substr(13);
    else if (all_) shown = "";
    if (!match_.empty() || !exclude_.empty()) {
      if (shown.empty() && !is_tag) continue;
    }
    if (!match_.empty()) {
      bool hit = false;
      for (auto& m : match_) if (wildmatch(m, shown, false, false)) hit = true;
      if (!hit) continue;
    }
    bool excluded = false;
    for (auto& e : exclude_) if (wildmatch(e, shown, false, false)) excluded = true;
    if (excluded) continue;
    auto obj = c_.repo->read_object(oid);
    if (!obj.ok()) continue;
    std::string peeled = oid;
    bool annotated = false;
    int64_t tdate = 0;
    bool tdate_ok = false;
    if (obj->type == "tag") {
      annotated = true;
      std::string cur = oid;
      for (int g = 0; g < 16; ++g) {
        TagInfo ti;
        if (!read_tag_object(c_, cur, ti)) break;
        if (g == 0) { tdate = ti.has_date ? ti.date : 0; tdate_ok = true; }
        cur = ti.target;
        if (ti.target_type != "tag") break;
      }
      peeled = cur;
    }
    if (!all_ && !tags_ && !annotated) continue;
    if (type_of(c_, peeled) != "commit") continue;
    int prio = annotated ? 2 : is_tag ? 1 : 0;
    std::string name = all_ ? path.substr(5) : path.substr(10);
    auto it = names_.find(peeled);
    if (it == names_.end() || replace(it->second, prio, oid, tdate, tdate_ok)) {
      Named n;
      n.name = name;
      n.prio = prio;
      n.oid = oid;
      if (annotated && tdate_ok) { n.tag_date = tdate; n.tag_known = true; }
      names_[peeled] = n;
    }
  }
}

std::string Describe::suffix_for(int depth, const std::string& oid) {
  return "-" + std::to_string(depth) + "-g" + c_.abbrev(oid, abbrev_);
}

void Describe::insert_by_date(std::vector<std::string>& list, const std::string& oid) {
  const int64_t d = commit(oid).date;
  auto pos = list.begin();
  while (pos != list.end() && commit(*pos).date >= d) ++pos;
  list.insert(pos, oid);
}

int Describe::finish_depth(std::vector<std::string>& list, Possible& best) {
  int seen = 0;
  while (!list.empty()) {
    std::string cur = list.front();
    list.erase(list.begin());
    ++seen;
    if (flags_[cur] & best.flag_within) {
      bool all_within = true;
      for (auto& i : list) if (!(flags_[i] & best.flag_within)) { all_within = false; break; }
      if (all_within) break;
    } else {
      ++best.depth;
    }
    for (auto& p : commit(cur).parents) {
      if (!seen_.count(p)) insert_by_date(list, p);
      seen_.insert(p);
      flags_[p] |= flags_[cur];
      if (first_parent_) break;
    }
  }
  return seen;
}

void Describe::describe_commit(const std::string& oid, const std::string& arg) {
  (void)arg;
  auto ex = names_.find(oid);
  if (debug_) c_.err << "describe " << arg << "\n";
  if (ex != names_.end() && (tags_ || all_ || ex->second.prio == 2)) {
    c_.out << ex->second.name;
    if (longformat_) {
      std::string target = oid;
      c_.out << suffix_for(0, target);
    }
    c_.out << suffix_ << "\n";
    return;
  }
  if (!max_candidates_) die(c_, "no tag exactly matches '" + oid + "'");
  if (debug_) c_.err << "No exact match on refs or tags, searching to describe\n";
  if (names_.empty() && !always_) die(c_, "No names found, cannot describe anything.");

  flags_.clear();
  seen_.clear();
  std::vector<Possible> matches;
  std::vector<std::string> list;
  seen_.insert(oid);
  list.push_back(oid);
  int seen_commits = 0, annotated_cnt = 0, unannotated_cnt = 0;
  std::string gave_up_on;
  while (!list.empty()) {
    std::string cur = list.front();
    list.erase(list.begin());
    ++seen_commits;
    auto nit = names_.find(cur);
    if (nit != names_.end()) {
      if (!tags_ && !all_ && nit->second.prio < 2) {
        ++unannotated_cnt;
      } else if ((int)matches.size() < max_candidates_) {
        Possible t;
        t.name = &nit->second;
        t.depth = seen_commits - 1;
        t.flag_within = 1u << (matches.size() + 1);
        t.found_order = (int)matches.size() + 1;
        matches.push_back(t);
        flags_[cur] |= t.flag_within;
        if (nit->second.prio == 2) ++annotated_cnt;
      } else {
        gave_up_on = cur;
        break;
      }
    }
    for (auto& t : matches) if (!(flags_[cur] & t.flag_within)) ++t.depth;
    if (annotated_cnt && list.empty()) {
      if (debug_) c_.err << "finished search at " << cur << "\n";
      break;
    }
    for (auto& p : commit(cur).parents) {
      if (!seen_.count(p)) insert_by_date(list, p);
      seen_.insert(p);
      flags_[p] |= flags_[cur];
      if (first_parent_) break;
    }
  }
  if (matches.empty()) {
    if (always_) {
      c_.out << c_.abbrev(oid, abbrev_) << suffix_ << "\n";
      return;
    }
    if (unannotated_cnt)
      die(c_, "No annotated tags can describe '" + oid + "'.\nHowever, there were unannotated tags: try --tags.");
    die(c_, "No tags can describe '" + oid + "'.\nTry --always, or create some tags.");
  }
  std::stable_sort(matches.begin(), matches.end(), [](const Possible& a, const Possible& b) {
    if (a.depth != b.depth) return a.depth < b.depth;
    return a.found_order < b.found_order;
  });
  if (!gave_up_on.empty()) {
    insert_by_date(list, gave_up_on);
    --seen_commits;
  }
  seen_commits += finish_depth(list, matches[0]);
  if (debug_) {
    static const char* labels[] = {"head", "lightweight", "annotated"};
    for (auto& t : matches) {
      char buf[256];
      std::snprintf(buf, sizeof buf, " %-11s %8d %s\n", labels[t.name->prio], t.depth, t.name->name.c_str());
      c_.err << buf;
    }
    c_.err << "traversed " << seen_commits << " commits\n";
    if (!gave_up_on.empty())
      c_.err << "more than " << max_candidates_ << " tags found; listed " << max_candidates_ << " most recent\ngave up search at " << gave_up_on << "\n";
  }
  c_.out << matches[0].name->name;
  if (abbrev_ != 0) c_.out << suffix_for(matches[0].depth, oid);
  c_.out << suffix_ << "\n";
}

int Describe::run_contains(const std::vector<std::string>& args) {
  NameRevOptions o;
  o.name_only = true;
  o.tags_only = !all_;
  for (auto& m : match_) o.refs.push_back((all_ ? "refs/heads/" : "refs/tags/") + m);
  for (auto& e : exclude_) o.excludes.push_back((all_ ? "refs/heads/" : "refs/tags/") + e);
  int64_t cutoff = INT64_MAX;
  struct Item { std::string arg, oid; };
  std::vector<Item> items;
  for (auto& a : args) {
    auto oid = resolve(c_, a);
    if (!oid) { c_.err << "Could not get sha1 for " << a << ". Skipping.\n"; continue; }
    std::string peeled = peel_to(c_, *oid, "commit");
    if (!peeled.empty()) cutoff = std::min<int64_t>(cutoff, (int64_t)read_commit(c_, peeled).committer_time);
    items.push_back({a, *oid});
  }
  if (cutoff != INT64_MAX && cutoff > 86400) cutoff -= 86400;
  else cutoff = 0;
  NameRev nr(c_, o);
  nr.run(cutoff);
  for (auto& it : items) {
    std::string type = type_of(c_, it.oid);
    std::string name = type == "commit" ? nr.commit_name(it.oid) : nr.exact_ref(it.oid);
    if (!name.empty()) c_.out << name << "\n";
    else if (always_) c_.out << c_.abbrev(it.oid) << "\n";
    else die(c_, "cannot describe '" + it.oid + "'");
  }
  return 0;
}

int Describe::run() {
  std::vector<std::string> args(c_.args.begin() + 1, c_.args.end());
  std::vector<std::string> revs;
  bool dirty = false, broken = false;
  std::string dirty_mark = "-dirty", broken_mark = "-broken";
  bool dashdash = false;
  auto need = [&](size_t& i, const std::string& name) {
    if (i + 1 >= args.size()) { c_.err << "error: option `" << name << "' requires a value\n"; throw Exit{129}; }
    return args[++i];
  };
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (dashdash) { revs.push_back(a); continue; }
    if (a == "--") { dashdash = true; continue; }
    if (a == "-h") { c_.out << kDescribeUsage; throw Exit{129}; }
    if (a == "--contains") contains_ = true;
    else if (a == "--no-contains") contains_ = false;
    else if (a == "--debug") debug_ = true;
    else if (a == "--no-debug") debug_ = false;
    else if (a == "--all") all_ = true;
    else if (a == "--no-all") all_ = false;
    else if (a == "--tags") tags_ = true;
    else if (a == "--no-tags") tags_ = false;
    else if (a == "--long") longformat_ = true;
    else if (a == "--no-long") longformat_ = false;
    else if (a == "--first-parent") first_parent_ = true;
    else if (a == "--no-first-parent") first_parent_ = false;
    else if (a == "--always") always_ = true;
    else if (a == "--no-always") always_ = false;
    else if (a == "--exact-match") max_candidates_ = 0;
    else if (a == "--no-exact-match") max_candidates_ = 10;
    else if (a == "--abbrev") { abbrev_given_ = true; abbrev_ = -1; }
    else if (a.rfind("--abbrev=", 0) == 0) {
      abbrev_given_ = true;
      char* end = nullptr;
      long v = std::strtol(a.c_str() + 9, &end, 10);
      if (a.size() == 9 || (end && *end)) { c_.err << "error: option `abbrev' expects a numerical value\n"; return 129; }
      if (v && v < 4) v = 4;
      else if (v > 40) v = 40;
      abbrev_ = (int)v;
    }
    else if (a == "--no-abbrev") { abbrev_given_ = true; abbrev_ = 40; }
    else if (a == "--candidates") max_candidates_ = std::atoi(need(i, "candidates").c_str());
    else if (a.rfind("--candidates=", 0) == 0) max_candidates_ = std::atoi(a.c_str() + 13);
    else if (a == "--match") match_.push_back(need(i, "match"));
    else if (a.rfind("--match=", 0) == 0) match_.push_back(a.substr(8));
    else if (a == "--no-match") match_.clear();
    else if (a == "--exclude") exclude_.push_back(need(i, "exclude"));
    else if (a.rfind("--exclude=", 0) == 0) exclude_.push_back(a.substr(10));
    else if (a == "--no-exclude") exclude_.clear();
    else if (a == "--dirty") dirty = true;
    else if (a.rfind("--dirty=", 0) == 0) { dirty = true; dirty_mark = a.substr(8); }
    else if (a == "--broken") broken = true;
    else if (a.rfind("--broken=", 0) == 0) { broken = true; broken_mark = a.substr(9); }
    else if (a.size() > 1 && a[0] == '-') {
      c_.err << "error: unknown option `" << (a[1] == '-' ? a.substr(2) : a.substr(1, 1)) << "'\n" << kDescribeUsage;
      return 129;
    } else revs.push_back(a);
  }
  c_.open_repo(false);
  if (max_candidates_ < 0) max_candidates_ = 0;
  if (max_candidates_ > 10 && false) max_candidates_ = 10;
  if (abbrev_ < 0) abbrev_ = c_.default_abbrev();
  if (always_ && contains_) {}
  if (contains_) {
    if (longformat_) die(c_, "options '--long' and '--contains' cannot be used together");
    if (dirty || broken) die(c_, "options '--contains' and '--dirty' cannot be used together");
    if (revs.empty()) revs.push_back("HEAD");
    return run_contains(revs);
  }
  if (longformat_ && abbrev_ == 0) die(c_, "options '--long' and '--abbrev=0' cannot be used together");
  if (!revs.empty() && (dirty || broken)) die(c_, std::string("option '") + (dirty ? "--dirty" : "--broken") + "' and commit-ishes cannot be used together");
  gather();
  auto describe_arg = [&](const std::string& arg) {
    std::string why;
    auto o = resolve(c_, arg, &why);
    if (!o) die(c_, "Not a valid object name " + arg);
    std::string type = type_of(c_, *o);
    if (type == "blob") unsupported(c_, "describe <blob>");
    std::string commit_oid = peel_to(c_, *o, "commit");
    if (commit_oid.empty()) die(c_, arg + " is neither a commit nor blob");
    describe_commit(commit_oid, arg);
  };
  if (revs.empty()) {
    if (dirty || broken) {
      auto head = read_ref(c_, "HEAD");
      bool is_dirty = false;
      if (head) {
        Snapshot a = snapshot_tree(c_, read_commit(c_, *head).tree);
        Snapshot b = snapshot_worktree(c_, false, nullptr);
        if (a.size() != b.size()) is_dirty = true;
        else for (auto& [p, fs] : a) { auto it = b.find(p); if (it == b.end() || it->second.oid != fs.oid || it->second.mode != fs.mode) { is_dirty = true; break; } }
      }
      if (is_dirty) suffix_ = dirty ? dirty_mark : broken_mark;
    }
    describe_arg("HEAD");
  } else {
    for (auto& r : revs) describe_arg(r);
  }
  return 0;
}

}  // namespace

int cmd_describe(Ctx& c) {
  Describe d(c);
  return d.run();
}

}  // namespace aios_git
