// name-rev: the symbolic name of commits relative to the refs that reach them.
#include <array>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <unordered_map>

#include "hist_util.hpp"

namespace aios_git {

using namespace aios;
using namespace aios::userland;

namespace {

const char* kNameRevUsage =
    "usage: git name-rev [<options>] <commit>...\n"
    "   or: git name-rev [<options>] --all\n"
    "   or: git name-rev [<options>] --annotate-stdin\n"
    "\n"
    "    --[no-]name-only      print only ref-based names (no object names)\n"
    "    --[no-]tags           only use tags to name the commits\n"
    "    --[no-]refs <pattern> only use refs matching <pattern>\n"
    "    --[no-]exclude <pattern>\n"
    "                          ignore refs matching <pattern>\n"
    "\n"
    "    --[no-]all            list all commits reachable from all refs\n"
    "    --[no-]annotate-stdin annotate text from stdin\n"
    "    --[no-]undefined      allow to print `undefined` names (default)\n"
    "    --[no-]always         show abbreviated commit object as fallback\n"
    "\n";

constexpr int kMergeTraversalWeight = 65535;

bool starts(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// The offset of the shortest-to-longest trailing run of folders of `path` that the glob matches, -1 for none.
int subpath_matches(const std::string& path, const std::string& filter) {
  size_t at = 0;
  while (true) {
    if (wildmatch(filter, path.substr(at), false, false)) return (int)at;
    size_t slash = path.find('/', at);
    if (slash == std::string::npos) return -1;
    at = slash + 1;
  }
}

// refs.c shorten_unambiguous_ref in its non-strict mode.
std::string shorten_ref(Ctx& c, const std::string& refname) {
  static const char* rules[] = {"", "refs/", "refs/tags/", "refs/heads/", "refs/remotes/", "refs/remotes/"};
  for (int i = 5; i > 0; --i) {
    std::string pre = rules[i];
    std::string suffix = i == 5 ? "/HEAD" : "";
    if (refname.compare(0, pre.size(), pre) != 0) continue;
    if (!suffix.empty()) {
      if (refname.size() < pre.size() + suffix.size() || refname.compare(refname.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
    }
    std::string shortn = refname.substr(pre.size(), refname.size() - pre.size() - suffix.size());
    bool clash = false;
    for (int j = 0; j < i && !clash; ++j) {
      if (j == i) continue;
      std::string cand = std::string(rules[j]) + shortn + (j == 5 ? "/HEAD" : "");
      if (read_ref(c, cand)) clash = true;
    }
    if (!clash) return shortn;
  }
  return refname;
}

std::string strip_ref(const std::string& path, bool abbreviate, Ctx& c) {
  if (abbreviate) return shorten_ref(c, path);
  if (starts(path, "refs/heads/")) return path.substr(11);
  if (starts(path, "refs/")) return path.substr(5);
  return path;
}

}  // namespace

bool read_tag_object(Ctx& c, const std::string& oid, TagInfo& out) {
  auto o = c.repo->read_object(oid);
  if (!o.ok() || o->type != "tag") return false;
  std::string body((const char*)o->payload.data(), o->payload.size());
  size_t pos = 0;
  while (pos < body.size()) {
    size_t nl = body.find('\n', pos);
    if (nl == std::string::npos) nl = body.size();
    std::string line = body.substr(pos, nl - pos);
    pos = nl + 1;
    if (line.empty()) break;
    if (line.rfind("object ", 0) == 0) out.target = line.substr(7);
    else if (line.rfind("type ", 0) == 0) out.target_type = line.substr(5);
    else if (line.rfind("tag ", 0) == 0) out.name = line.substr(4);
    else if (line.rfind("tagger ", 0) == 0) {
      size_t gt = line.rfind('>');
      if (gt != std::string::npos) {
        out.date = std::strtoll(line.c_str() + gt + 1, nullptr, 10);
        out.has_date = true;
      }
    }
  }
  return true;
}

// git 2.43's is_better_name: distances count a merge hop's weight once the name has gone down a first-parent chain
// (effective_distance); between two tags the nearer wins, a tag beats a non-tag, and between two non-tags the
// shorter way, then the older tip.
// git's object hash table (object.c) replayed: name-rev --all prints the commits in its slot order. An object goes at
// the first 4 bytes of its id (as an unsigned int, git's oidhash) masked by the size, probing on; the table doubles when
// size - 1 <= 2 * count, re-inserting in slot order; a lookup that probed past its first slot swaps the object there.
// The creations and lookups are git's own, in its order: name_ref's parse_object of every ref (a tag's tagged object,
// a commit's tree then its parents), then name_rev's repo_parse_commit of each parent it meets.
struct NameRev::ObjectTable {
  struct Slot { std::array<uint8_t, 20> id{}; bool used = false; bool parsed = false; std::string type; };
  std::vector<Slot> slots;
  size_t count = 0;
  bool graph = false;   // a commit-graph: parents come from it, no tree is looked up
  static unsigned int hash_of(const std::array<uint8_t, 20>& id) {
    unsigned int h;
    std::memcpy(&h, id.data(), sizeof h);
    return h;
  }
  long lookup(const std::array<uint8_t, 20>& id) {
    if (slots.empty()) return -1;
    const size_t n = slots.size();
    size_t first = hash_of(id) & (n - 1), i = first;
    while (slots[i].used) {
      if (slots[i].id == id) break;
      if (++i == n) i = 0;
    }
    if (!slots[i].used) return -1;
    if (i != first) std::swap(slots[i], slots[first]);
    return (long)first;
  }
  static void insert(std::vector<Slot>& t, Slot&& s) {
    const size_t n = t.size();
    size_t j = hash_of(s.id) & (n - 1);
    while (t[j].used) if (++j >= n) j = 0;
    t[j] = std::move(s);
  }
  long create(const std::array<uint8_t, 20>& id, const std::string& type) {
    if (slots.size() <= count * 2 + 1) {
      std::vector<Slot> bigger(slots.size() < 32 ? 32 : 2 * slots.size());
      for (auto& sl : slots) if (sl.used) insert(bigger, std::move(sl));
      slots = std::move(bigger);
    }
    Slot s;
    s.id = id;
    s.used = true;
    s.type = type;
    insert(slots, std::move(s));
    ++count;
    return lookup_quiet(id);
  }
  long lookup_quiet(const std::array<uint8_t, 20>& id) const {   // where it is, without git's swap (not a git lookup)
    const size_t n = slots.size();
    size_t i = hash_of(id) & (n - 1);
    while (slots[i].used) { if (slots[i].id == id) return (long)i; if (++i == n) i = 0; }
    return -1;
  }
  // lookup_commit / lookup_tree / lookup_tag / lookup_blob: found, or made.
  long lookup_or_create(const std::array<uint8_t, 20>& id, const std::string& type) {
    long at = lookup(id);
    return at >= 0 ? at : create(id, type);
  }
};

namespace {
bool raw_id(const std::string& hex, std::array<uint8_t, 20>& out) {
  if (hex.size() != 40) return false;
  auto nib = [](char ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1; };
  for (int i = 0; i < 20; ++i) {
    int hi = nib(hex[2 * i]), lo = nib(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)(hi << 4 | lo);
  }
  return true;
}
}  // namespace

void NameRev::track_objects() {
  table_ = std::make_shared<ObjectTable>();
  if (c_.config.get_bool("core.commitgraph", true)) {
    const std::string dir = c_.repo->common_dir() + "/objects/info/";
    table_->graph = c_.host().exists(dir + "commit-graph") || c_.host().exists(dir + "commit-graphs/commit-graph-chain");
  }
}

void NameRev::replay_parse_commit(const std::string& oid) {
  std::array<uint8_t, 20> id;
  if (!table_ || !raw_id(oid, id)) return;
  ObjectTable& t = *table_;
  long at = t.lookup_quiet(id);
  if (at >= 0 && t.slots[(size_t)at].parsed) return;
  auto cm = c_.repo->read_commit(oid);
  if (!cm.ok()) return;
  if (!t.graph) {
    std::array<uint8_t, 20> tree;
    if (raw_id(cm->tree, tree)) t.lookup_or_create(tree, "tree");
  }
  for (auto& p : cm->parents) {
    std::array<uint8_t, 20> pid;
    if (raw_id(p, pid)) t.lookup_or_create(pid, "commit");
  }
  if (long me = t.lookup_quiet(id); me >= 0) t.slots[(size_t)me].parsed = true;
}

void NameRev::replay_parse_object(const std::string& oid) {
  std::array<uint8_t, 20> id;
  if (!table_ || !raw_id(oid, id)) return;
  ObjectTable& t = *table_;
  long at = t.lookup(id);
  if (at >= 0 && t.slots[(size_t)at].parsed) return;
  auto o = c_.repo->read_object(oid);
  if (!o.ok()) return;
  auto mark = [&]() { if (long me = t.lookup_quiet(id); me >= 0) t.slots[(size_t)me].parsed = true; };
  if (o->type == "blob") {
    t.lookup_or_create(id, "blob");
    mark();
    t.lookup(id);
    return;
  }
  t.lookup_or_create(id, o->type);   // parse_object_buffer's lookup_<type>
  long me = t.lookup_quiet(id);
  if (me >= 0 && t.slots[(size_t)me].parsed) return;
  if (o->type == "commit") {
    GitCommit cm;
    auto r = c_.repo->read_commit(oid);
    if (!r.ok()) return;
    cm = *r;
    std::array<uint8_t, 20> tree;
    if (raw_id(cm.tree, tree)) t.lookup_or_create(tree, "tree");
    for (auto& p : cm.parents) {
      std::array<uint8_t, 20> pid;
      if (raw_id(p, pid)) t.lookup_or_create(pid, "commit");
    }
  } else if (o->type == "tag") {
    TagInfo ti;
    if (read_tag_object(c_, oid, ti) && !ti.target.empty()) {
      std::array<uint8_t, 20> tid;
      const std::string& ty = ti.target_type;
      if (raw_id(ti.target, tid) && (ty == "commit" || ty == "tree" || ty == "blob" || ty == "tag")) t.lookup_or_create(tid, ty);
    }
  }
  mark();
}

std::vector<std::string> NameRev::commits_in_table_order() const {
  std::vector<std::string> out;
  if (!table_) return out;
  static const char* hx = "0123456789abcdef";
  for (auto& s : table_->slots) {
    if (!s.used || s.type != "commit") continue;
    std::string h(40, '0');
    for (int i = 0; i < 20; ++i) { h[2 * i] = hx[s.id[i] >> 4]; h[2 * i + 1] = hx[s.id[i] & 15]; }
    out.push_back(std::move(h));
  }
  return out;
}

bool NameRev::create_or_update(const std::string& commit, const std::string& tip, int64_t taggerdate, int gen, int dist, bool from_tag) {
  auto it = names_.find(commit);
  if (it != names_.end()) {
    const Rev& n = it->second;
    auto effective = [](int distance, int generation) { return distance + (generation > 0 ? kMergeTraversalWeight : 0); };
    const int name_distance = effective(n.distance, n.generation), new_distance = effective(dist, gen);
    bool better;
    if (from_tag && n.from_tag) better = name_distance > new_distance;
    else if (n.from_tag != from_tag) better = from_tag;
    else if (name_distance != new_distance) better = name_distance > new_distance;
    else if (n.taggerdate != taggerdate) better = n.taggerdate > taggerdate;
    else better = false;
    if (!better) return false;
  }
  Rev& n = names_[commit];
  n.tip = tip;
  n.taggerdate = taggerdate;
  n.generation = gen;
  n.distance = dist;
  n.from_tag = from_tag;
  return true;
}

namespace {
struct Tip {
  std::string oid, refname, commit;
  int64_t taggerdate = 0;
  int from_tag = 0;
  bool deref = false;
};
// name-rev names the older tags first, and tags before anything else: the order its qsort leaves them in. The keys
// are sorted, not the tips: qsort moves its elements as bytes, and a Tip's strings are not bytes to move (a short
// string points into itself; the copy freed someone else's buffer - `name-rev` aborted in free()).
struct TipKey {
  int from_tag;
  int64_t taggerdate;
  size_t index;
};
int tip_order(const void* x, const void* y) {
  const TipKey* a = (const TipKey*)x;
  const TipKey* b = (const TipKey*)y;
  int cmp = b->from_tag - a->from_tag;
  if (cmp) return cmp;
  if (a->taggerdate != b->taggerdate) return a->taggerdate < b->taggerdate ? -1 : 1;
  return 0;
}
}  // namespace

void NameRev::run(int64_t cutoff) {
  cutoff_ = cutoff;
  std::unordered_map<std::string, std::pair<int64_t, std::vector<std::string>>> meta;
  auto commit_meta = [&](const std::string& oid) -> const std::pair<int64_t, std::vector<std::string>>& {
    auto f = meta.find(oid);
    if (f != meta.end()) return f->second;
    GitCommit cm = read_commit(c_, oid);
    return meta.emplace(oid, std::make_pair((int64_t)cm.committer_time, cm.parents)).first->second;
  };
  std::vector<Tip> tips;
  for (auto& [path, oid] : all_refs(c_)) {
    if (table_) replay_parse_object(oid);   // git parses the ref's object before its filters
    if (o_.tags_only && !starts(path, "refs/tags/")) continue;
    bool skip = false;
    for (auto& ex : o_.excludes) if (subpath_matches(path, ex) >= 0) skip = true;
    if (skip) continue;
    bool abbreviate = o_.tags_only && o_.name_only;
    if (!o_.refs.empty()) {
      bool matched = false;
      for (auto& pat : o_.refs) {
        int r = subpath_matches(path, pat);
        if (r < 0) continue;
        matched = true;
        if (r > 0) abbreviate = true;
      }
      if (!matched) continue;
    }
    auto obj = c_.repo->read_object(oid);
    if (!obj.ok()) continue;
    Tip t;
    t.oid = oid;
    t.refname = strip_ref(path, abbreviate, c_);
    std::string cur = oid;
    int64_t taggerdate = INT64_MAX;
    std::string type = obj->type;
    for (int guard = 0; type == "tag" && guard < 16; ++guard) {
      TagInfo ti;
      if (!read_tag_object(c_, cur, ti) || ti.target.empty()) break;
      t.deref = true;
      taggerdate = ti.has_date ? ti.date : 0;
      cur = ti.target;
      if (table_) replay_parse_object(cur);
      auto nx = c_.repo->read_object(cur);
      if (!nx.ok()) { type = ""; break; }
      type = nx->type;
    }
    if (type == "commit") {
      t.commit = cur;
      t.from_tag = starts(path, "refs/tags/") ? 1 : 0;
      if (taggerdate == INT64_MAX) taggerdate = commit_meta(cur).first;
    }
    t.taggerdate = taggerdate;
    tips.push_back(std::move(t));
  }
  for (auto& t : tips) if (!tips_.count(t.oid)) tips_[t.oid] = t.refname;
  if (!tips.empty()) {
    std::vector<TipKey> keys(tips.size());
    for (size_t i = 0; i < tips.size(); ++i) keys[i] = TipKey{tips[i].from_tag, tips[i].taggerdate, i};
    std::qsort(keys.data(), keys.size(), sizeof(TipKey), tip_order);
    std::vector<Tip> sorted;
    sorted.reserve(tips.size());
    for (auto& k : keys) sorted.push_back(std::move(tips[k.index]));
    tips = std::move(sorted);
  }
  for (auto& t : tips) {
    if (t.commit.empty()) continue;
    const bool from_tag = t.from_tag != 0;
    const auto& m = commit_meta(t.commit);
    if (m.first < cutoff_) continue;
    std::string start_tip = t.deref ? t.refname + "^0" : t.refname;
    if (!create_or_update(t.commit, start_tip, t.taggerdate, 0, 0, from_tag)) continue;
    std::vector<std::string> stack = {t.commit};
    while (!stack.empty()) {
      std::string commit = stack.back();
      stack.pop_back();
      Rev name = names_[commit];
      int parent_number = 1;
      std::vector<std::string> pushed;
      for (auto& p : commit_meta(commit).second) {
        std::string new_name;
        int generation, distance;
        if (parent_number > 1) {
          std::string base = name.tip;
          if (base.size() >= 2 && base.compare(base.size() - 2, 2, "^0") == 0) base.resize(base.size() - 2);
          new_name = name.generation > 0 ? base + "~" + std::to_string(name.generation) + "^" + std::to_string(parent_number)
                                         : base + "^" + std::to_string(parent_number);
          generation = 0;
          distance = name.distance + kMergeTraversalWeight;
        } else {
          new_name = name.tip;
          generation = name.generation + 1;
          distance = name.distance + 1;
        }
        ++parent_number;
        if (table_) replay_parse_commit(p);
        if (commit_meta(p).first < cutoff_) continue;
        if (create_or_update(p, new_name, t.taggerdate, generation, distance, from_tag)) pushed.push_back(p);
      }
      for (size_t k = pushed.size(); k-- > 0;) stack.push_back(pushed[k]);
    }
  }
}

std::string NameRev::commit_name(const std::string& oid) const {
  auto it = names_.find(oid);
  if (it == names_.end()) return "";
  const Rev& n = it->second;
  if (!n.generation) return n.tip;
  std::string base = n.tip;
  if (base.size() >= 2 && base.compare(base.size() - 2, 2, "^0") == 0) base.resize(base.size() - 2);
  return base + "~" + std::to_string(n.generation);
}

std::string NameRev::exact_ref(const std::string& oid) const {
  auto it = tips_.find(oid);
  return it == tips_.end() ? "" : it->second;
}

int cmd_name_rev(Ctx& c) {
  std::vector<std::string> args(c.args.begin() + 1, c.args.end());
  NameRevOptions opt;
  bool all = false, annotate = false, allow_undefined = true, always = false, deprecated_stdin = false;
  std::vector<std::string> revs;
  auto need = [&](size_t& i, const std::string& name) {
    if (i + 1 >= args.size()) { c.err << "error: option `" << name << "' requires a value\n" << kNameRevUsage; throw Exit{129}; }
    return args[++i];
  };
  bool dashdash = false;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (dashdash) { revs.push_back(a); continue; }
    if (a == "--") { dashdash = true; continue; }
    if (a == "-h") { c.out << kNameRevUsage; throw Exit{129}; }
    if (a == "--name-only") opt.name_only = true;
    else if (a == "--no-name-only") opt.name_only = false;
    else if (a == "--tags") opt.tags_only = true;
    else if (a == "--no-tags") opt.tags_only = false;
    else if (a == "--refs") opt.refs.push_back(need(i, "refs"));
    else if (a.rfind("--refs=", 0) == 0) opt.refs.push_back(a.substr(7));
    else if (a == "--no-refs") opt.refs.clear();
    else if (a == "--exclude") opt.excludes.push_back(need(i, "exclude"));
    else if (a.rfind("--exclude=", 0) == 0) opt.excludes.push_back(a.substr(10));
    else if (a == "--no-exclude") opt.excludes.clear();
    else if (a == "--all") all = true;
    else if (a == "--no-all") all = false;
    else if (a == "--annotate-stdin") annotate = true;
    else if (a == "--no-annotate-stdin") annotate = false;
    else if (a == "--stdin") { annotate = true; deprecated_stdin = true; }
    else if (a == "--undefined") allow_undefined = true;
    else if (a == "--no-undefined") allow_undefined = false;
    else if (a == "--always") always = true;
    else if (a == "--no-always") always = false;
    else if (a.size() > 1 && a[0] == '-') {
      c.err << "error: unknown option `" << (a[1] == '-' ? a.substr(2) : a.substr(1, 1)) << "'\n" << kNameRevUsage;
      throw Exit{129};
    } else revs.push_back(a);
  }
  c.open_repo(false);
  if (deprecated_stdin)
    c.err << "warning: --stdin is deprecated. Please use --annotate-stdin instead, which is functionally equivalent.\n"
             "This option will be removed in a future release.\n";
  if (revs.empty() && !all && !annotate) return 0;
  if ((!revs.empty() ? 1 : 0) + (all ? 1 : 0) + (annotate ? 1 : 0) > 1) {
    c.err << "error: Specify either a list, or --all, not both!\n" << kNameRevUsage;
    return 129;
  }
  struct Item { std::string name; std::string oid; std::string type; };
  std::vector<Item> items;
  int64_t cutoff = INT64_MAX;
  bool any_cutoff = false;
  if (all) {
    cutoff = 0;   // the commits are those the walk meets, listed below once it has run
  } else if (!annotate) {
    for (auto& r : revs) {
      auto oid = resolve(c, r);
      if (!oid) { c.err << "Could not get sha1 for " << r << ". Skipping.\n"; continue; }
      std::string type = type_of(c, *oid);
      if (type.empty()) { c.err << "Could not get object for " << r << ". Skipping.\n"; continue; }
      std::string peeled = peel_to(c, *oid, "commit");
      if (!peeled.empty()) {
        int64_t d = (int64_t)read_commit(c, peeled).committer_time;
        if (d < cutoff) cutoff = d;
        any_cutoff = true;
      }
      items.push_back({r, *oid, type});
    }
  }
  if (!all && any_cutoff && cutoff != INT64_MAX && cutoff > 86400) cutoff -= 86400;
  else if (!all) cutoff = 0;
  NameRev nr(c, opt);
  if (all) nr.track_objects();
  nr.run(cutoff);
  // git lists every commit its walk parsed or met, in its object table's order
  if (all) for (auto& oid : nr.commits_in_table_order()) items.push_back({"", oid, "commit"});
  auto name_for = [&](const std::string& oid, const std::string& type) -> std::string {
    if (type != "commit") return nr.exact_ref(oid);
    return nr.commit_name(oid);
  };
  auto show = [&](const Item& it) {
    if (!opt.name_only) c.out << (it.name.empty() ? it.oid : it.name) << " ";
    std::string n = name_for(it.oid, it.type);
    if (!n.empty()) c.out << n << "\n";
    else if (allow_undefined) c.out << "undefined\n";
    else if (always) c.out << c.abbrev(it.oid) << "\n";
    else die(c, "cannot describe '" + it.oid + "'");
  };
  if (annotate) {
    std::string line;
    while (std::getline(c.in, line)) {
      bool had_nl = !c.in.eof();
      std::string out;
      size_t i = 0;
      while (i < line.size()) {
        if (!std::isxdigit((unsigned char)line[i])) { out += line[i++]; continue; }
        size_t j = i;
        while (j < line.size() && std::isxdigit((unsigned char)line[j])) ++j;
        std::string run = line.substr(i, j - i);
        std::string name;
        if (run.size() == 40) {
          std::string type = type_of(c, run);
          if (!type.empty()) name = name_for(run, type);
        }
        if (name.empty()) out += run;
        else if (opt.name_only) out += name;
        else out += run + " (" + name + ")";
        i = j;
      }
      (void)had_nl;
      c.out << out << "\n";
    }
    return 0;
  }
  for (auto& it : items) show(it);
  return 0;
}

}  // namespace aios_git
