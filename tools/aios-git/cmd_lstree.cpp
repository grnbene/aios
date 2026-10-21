// ls-tree: the entries of a tree object, recursively, filtered by paths, formatted as asked.
#include <algorithm>
#include <cctype>

#include "hist_util.hpp"

namespace aios_git {

using namespace aios;
using namespace aios::userland;

namespace {

const char* kLsTreeUsage =
    "usage: git ls-tree [<options>] <tree-ish> [<path>...]\n"
    "\n"
    "    -d                    only show trees\n"
    "    -r                    recurse into subtrees\n"
    "    -t                    show trees when recursing\n"
    "    -z                    terminate entries with NUL byte\n"
    "    -l, --long            include object size\n"
    "    --name-only           list only filenames\n"
    "    --name-status         list only filenames\n"
    "    --object-only         list only objects\n"
    "    --[no-]full-name      use full path names\n"
    "    --[no-]full-tree      list entire tree; not just current directory (implies --full-name)\n"
    "    --format <format>     format to use for the output\n"
    "    --[no-]abbrev[=<n>]   use <n> digits to display object names\n"
    "\n";

struct SpecItem {
  std::string path;       // from the top of the tree, no trailing slash
  bool dir = false;       // written with a trailing slash, or as "." / ".." : the folder's contents
  std::string raw;
};

class LsTree {
 public:
  explicit LsTree(Ctx& c) : c_(c) {}
  int run();

 private:
  Ctx& c_;
  bool only_trees_ = false, recursive_ = false, show_trees_ = false, nul_ = false, long_ = false, name_only_ = false;
  bool object_only_ = false, full_name_ = false, full_tree_ = false;
  int abbrev_ = -1;                 // -1: not asked; 0: full; else digits (0 with auto: see auto_abbrev_)
  bool auto_abbrev_ = false;
  std::string format_;
  bool have_format_ = false;
  std::vector<SpecItem> specs_;
  std::string prefix_;              // where the command runs, "" or "dir/"
  std::string line_end_;

  bool interesting(const std::string& path, bool is_tree) const;
  bool recurse_into(const std::string& base, const std::string& name) const;
  void walk(const std::string& tree, const std::string& base);
  void show(const GitTreeEntry& e, const std::string& path);
  std::string display(const std::string& full) const;
  std::string oid_text(const std::string& oid);
  std::string size_text(const GitTreeEntry& e, bool padded);
  void check_format(const std::string& fmt);
};

std::string mode_text(uint32_t m) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%06o", m);
  return buf;
}

[[noreturn]] void parse_error(Ctx& c, const std::string& msg) {
  c.err << "error: " << msg << "\n" << kLsTreeUsage;
  throw Exit{129};
}

// The path as the user at `prefix` would write it (git's relative_path).
std::string LsTree::display(const std::string& full) const {
  if (full_name_ || full_tree_ || prefix_.empty()) return full;
  const std::string slashed = full + "/";
  size_t common = 0;
  for (size_t i = 0;;) {
    size_t ps = prefix_.find('/', i);
    if (ps == std::string::npos) break;
    size_t fs = slashed.find('/', i);
    if (fs != ps || slashed.compare(i, ps - i, prefix_, i, ps - i) != 0) break;
    i = ps + 1;
    common = i;
  }
  std::string ups;
  for (char ch : prefix_.substr(common)) if (ch == '/') ups += "../";
  std::string rel = slashed.substr(common);
  if (!rel.empty()) rel.pop_back();
  if (ups.empty() && rel.empty()) return "./";
  return ups + rel;
}

std::string LsTree::oid_text(const std::string& oid) {
  if (abbrev_ < 0 && !auto_abbrev_) return oid;
  if (auto_abbrev_) return c_.abbrev(oid, 0);
  if (abbrev_ == 0 || abbrev_ >= 40) return oid;
  return c_.abbrev(oid, abbrev_);
}

std::string LsTree::size_text(const GitTreeEntry& e, bool padded) {
  std::string size = "-";
  if (!e.is_dir() && e.mode != 0160000) {
    auto o = c_.repo->read_object(e.oid);
    if (o.ok()) size = std::to_string(o->payload.size());
  }
  if (padded && size.size() < 7) size = std::string(7 - size.size(), ' ') + size;
  return size;
}

bool LsTree::interesting(const std::string& path, bool is_tree) const {
  if (specs_.empty()) return true;
  for (auto& s : specs_) {
    if (s.path.empty()) return true;
    if (path == s.path) { if (s.dir && !is_tree) continue; return true; }
    if (path.size() > s.path.size() && path.compare(0, s.path.size(), s.path) == 0 && path[s.path.size()] == '/') return true;
    if (is_tree && s.path.size() > path.size() && s.path.compare(0, path.size(), path) == 0 && s.path[path.size()] == '/') return true;
  }
  return false;
}

// show_recursive of ls-tree.c: does some path name something below this tree entry?
bool LsTree::recurse_into(const std::string& base, const std::string& name) const {
  if (recursive_) return true;
  for (auto& s : specs_) {
    std::string spec = s.path + (s.dir && !s.path.empty() ? "/" : "");
    if (spec.compare(0, base.size(), base) != 0 || spec.size() < base.size()) continue;
    std::string rest = spec.substr(base.size());
    if (rest.size() <= name.size()) continue;
    if (rest[name.size()] != '/') continue;
    if (rest.compare(0, name.size(), name) != 0) continue;
    return true;
  }
  return false;
}

void LsTree::check_format(const std::string& fmt) {
  for (size_t i = 0; i < fmt.size(); ++i) {
    if (fmt[i] != '%') continue;
    if (i + 1 < fmt.size() && fmt[i + 1] == '%') { ++i; continue; }
    if (i + 1 < fmt.size() && fmt[i + 1] == 'n') { ++i; continue; }
    if (i + 3 < fmt.size() + 0 && fmt[i + 1] == 'x' && std::isxdigit((unsigned char)fmt[i + 2]) && std::isxdigit((unsigned char)fmt[i + 3])) { i += 3; continue; }
    if (i + 1 >= fmt.size() || fmt[i + 1] != '(') die(c_, "bad ls-tree format: element '" + fmt.substr(i + 1) + "' does not start with '('");
    size_t e = fmt.find(')', i + 1);
    if (e == std::string::npos) die(c_, "bad ls-tree format: element '" + fmt.substr(i + 1) + "' does not end in ')'");
    std::string atom = fmt.substr(i + 2, e - i - 2);
    if (atom != "objectmode" && atom != "objecttype" && atom != "objectsize" && atom != "objectsize:padded" && atom != "objectname" && atom != "path")
      die(c_, "bad ls-tree format: %" + fmt.substr(i + 1, e - i));
    i = e;
  }
}

void LsTree::show(const GitTreeEntry& e, const std::string& path) {
  const char* type = e.is_dir() ? "tree" : e.mode == 0160000 ? "commit" : "blob";
  std::string shown = display(path);
  if (!nul_) shown = quote_path(shown);
  std::string fmt = format_;
  if (!have_format_) {
    if (name_only_) fmt = "%(path)";
    else if (object_only_) fmt = "%(objectname)";
    else if (long_) fmt = "%(objectmode) %(objecttype) %(objectname) %(objectsize:padded)%x09%(path)";
    else fmt = "%(objectmode) %(objecttype) %(objectname)%x09%(path)";
  }
  std::string out;
  for (size_t i = 0; i < fmt.size(); ++i) {
    if (fmt[i] != '%') { out += fmt[i]; continue; }
    if (fmt[i + 1] == '%') { out += '%'; ++i; continue; }
    if (fmt[i + 1] == 'n') { out += '\n'; ++i; continue; }
    if (fmt[i + 1] == 'x' && i + 3 < fmt.size() + 0 && std::isxdigit((unsigned char)fmt[i + 2]) && std::isxdigit((unsigned char)fmt[i + 3])) {
      out += (char)std::stoi(fmt.substr(i + 2, 2), nullptr, 16);
      i += 3;
      continue;
    }
    size_t end = fmt.find(')', i);
    std::string atom = fmt.substr(i + 2, end - i - 2);
    i = end;
    if (atom == "objectmode") out += mode_text(e.mode);
    else if (atom == "objecttype") out += type;
    else if (atom == "objectsize") out += size_text(e, false);
    else if (atom == "objectsize:padded") out += size_text(e, true);
    else if (atom == "objectname") out += oid_text(e.oid);
    else if (atom == "path") out += shown;
  }
  c_.out << out << line_end_;
}

void LsTree::walk(const std::string& tree, const std::string& base) {
  auto ents = c_.repo->read_tree(tree);
  if (!ents.ok()) die(c_, ents.error().message);
  for (auto& e : *ents) {
    std::string path = base + e.name;
    const bool is_tree = e.is_dir();
    if (!interesting(path, is_tree)) continue;
    if (!is_tree) {
      if (e.mode != 0160000 && only_trees_) continue;
      show(e, path);
      continue;
    }
    if (recurse_into(base, e.name)) {
      if (show_trees_) show(e, path);
      walk(e.oid, path + "/");
    } else {
      show(e, path);
    }
  }
}

int LsTree::run() {
  std::vector<std::string> args(c_.args.begin() + 1, c_.args.end());
  std::vector<std::string> pos;
  bool dashdash = false;
  int conflict_first = 0;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (dashdash) { pos.push_back(a); continue; }
    if (a == "--") { dashdash = true; continue; }
    if (a == "-h") { c_.out << kLsTreeUsage; throw Exit{129}; }
    if (a.size() > 2 && a[0] == '-' && a[1] == '-') {
      std::string name = a.substr(2), val;
      bool has = false;
      size_t eq = name.find('=');
      if (eq != std::string::npos) { val = name.substr(eq + 1); name = name.substr(0, eq); has = true; }
      static const char* longs[] = {"long", "name-only", "name-status", "object-only", "full-name", "full-tree", "format", "abbrev"};
      bool neg = false;
      std::string base = name;
      if (base.rfind("no-", 0) == 0) { neg = true; base = base.substr(3); }
      std::vector<std::string> hits;
      for (auto l : longs) { if (base == l) { hits = {l}; break; } if (std::string(l).rfind(base, 0) == 0) hits.push_back(l); }
      if (hits.size() != 1) {
        if (hits.size() > 1) parse_error(c_, "ambiguous option: " + name);
        parse_error(c_, "unknown option `" + name + "'");
      }
      const std::string full = hits[0];
      if (full == "long") { long_ = !neg; }
      else if (full == "name-only" || full == "name-status") { name_only_ = !neg; }
      else if (full == "object-only") { object_only_ = !neg; }
      else if (full == "full-name") { full_name_ = !neg; }
      else if (full == "full-tree") { full_tree_ = !neg; }
      else if (full == "format") {
        if (neg) { have_format_ = false; continue; }
        if (!has) { if (i + 1 >= args.size()) parse_error(c_, "option `format' requires a value"); val = args[++i]; }
        format_ = val;
        have_format_ = true;
      } else if (full == "abbrev") {
        if (neg) { abbrev_ = -1; auto_abbrev_ = false; continue; }
        if (!has) { auto_abbrev_ = true; abbrev_ = -1; continue; }
        char* end = nullptr;
        long v = std::strtol(val.c_str(), &end, 10);
        if (val.empty() || (end && *end)) { c_.err << "error: option `abbrev' expects a numerical value\n"; throw Exit{129}; }
        if (v && v < 4) v = 4;
        else if (v > 40) v = 40;
        abbrev_ = (int)v;
        auto_abbrev_ = false;
      }
      (void)conflict_first;
      continue;
    }
    if (a.size() > 1 && a[0] == '-') {
      for (size_t j = 1; j < a.size(); ++j) {
        switch (a[j]) {
          case 'd': only_trees_ = true; break;
          case 'r': recursive_ = true; break;
          case 't': show_trees_ = true; break;
          case 'z': nul_ = true; break;
          case 'l': long_ = true; break;
          default: parse_error(c_, std::string("unknown switch `") + a[j] + "'");
        }
      }
      continue;
    }
    pos.push_back(a);
  }
  c_.open_repo(false);
  if (long_ && name_only_) { c_.err << "error: options '--name-only' and '-l' cannot be used together\n"; return 129; }
  if (long_ && object_only_) { c_.err << "error: options '--object-only' and '-l' cannot be used together\n"; return 129; }
  if (name_only_ && object_only_) { c_.err << "error: options '--object-only' and '--name-only' cannot be used together\n"; return 129; }
  if (have_format_ && (long_ || name_only_ || object_only_)) {
    c_.err << "fatal: --format can't be combined with other format-altering options\n\n" << kLsTreeUsage;
    return 129;
  }
  if (only_trees_ && recursive_) show_trees_ = true;
  if (pos.empty()) { c_.err << kLsTreeUsage; return 129; }
  line_end_ = nul_ ? std::string(1, '\0') : "\n";
  prefix_ = full_tree_ ? "" : c_.prefix;
  std::string why;
  auto named = resolve(c_, pos[0], &why);
  if (!named) die(c_, "Not a valid object name " + pos[0]);
  std::string tree = peel_to(c_, *named, "tree");
  if (tree.empty()) die(c_, "not a tree object");
  if (have_format_) check_format(format_);

  std::vector<std::string> user;
  for (size_t i = 1; i < pos.size(); ++i) user.push_back(pos[i]);
  for (auto& s : user) {
    if (s.empty()) die(c_, "empty string is not a valid pathspec. please use . instead if you meant to match all paths");
    std::vector<std::string> bad;
    if (s.rfind(":!", 0) == 0 || s.rfind(":^", 0) == 0) bad.push_back("'exclude' (mnemonic: '!')");
    if (s.rfind(":(", 0) == 0) {
      size_t close = s.find(')');
      std::string words = close == std::string::npos ? "" : s.substr(2, close - 2);
      for (size_t b = 0; b <= words.size();) {
        size_t e = words.find(',', b);
        if (e == std::string::npos) e = words.size();
        std::string w = words.substr(b, e - b);
        b = e + 1;
        if (w == "exclude") bad.push_back("'exclude' (mnemonic: '!')");
        else if (w == "glob") bad.push_back("'glob'");
        else if (w == "icase") bad.push_back("'icase'");
        else if (w.rfind("attr:", 0) == 0) bad.push_back("'attr'");
      }
    }
    if (!bad.empty()) {
      std::string list;
      for (size_t k = 0; k < bad.size(); ++k) list += (k ? ", " : "") + bad[k];
      die(c_, s + ": pathspec magic not supported by this command: " + list);
    }
  }
  for (auto& s : user) {
    SpecItem it;
    it.raw = s;
    std::string u = s;
    if (full_tree_) {
      // pathspecs are taken from the top, not from where the command runs
      Pathspec ps = make_pathspec(c_, {u});
      (void)ps;
    }
    std::string rp;
    {
      std::string body = u;
      bool magic_top = body.rfind(":/", 0) == 0 || body.rfind(":(top)", 0) == 0;
      Pathspec ps = make_pathspec(c_, {u});
      rp = ps.items.empty() ? "" : ps.items[0];
      if (magic_top && body.size() > 2 && body.back() == '/') it.dir = true;
      if (full_tree_ && !magic_top) {
        std::string raw = body;
        rp = normalize_path(raw);
        if (rp == ".") rp = "";
      }
    }
    it.path = rp;
    size_t last = u.find_last_of('/');
    std::string tail = last == std::string::npos ? u : u.substr(last + 1);
    if (u.back() == '/' || tail == "." || tail == ".." || u == "." || u == "..") it.dir = true;
    if (it.path.empty()) it.dir = false;
    specs_.push_back(std::move(it));
  }
  if (specs_.empty() && !prefix_.empty()) {
    SpecItem it;
    it.path = prefix_.substr(0, prefix_.size() - 1);
    it.dir = true;
    specs_.push_back(std::move(it));
  }
  walk(tree, "");
  return 0;
}

}  // namespace

int cmd_ls_tree(Ctx& c) {
  LsTree t(c);
  return t.run();
}

}  // namespace aios_git
