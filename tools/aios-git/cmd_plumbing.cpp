// Plumbing and repository tools: config, cat-file, hash-object, mktree, write-tree, read-tree,
// update-index, commit-tree, update-ref, symbolic-ref, ls-files, ls-tree, notes, show-ref,
// for-each-ref, check-ignore, var, fsck, gc.
#include <algorithm>
#include <cctype>
#include <ctime>
#include <functional>
#include <limits>
#include <regex>
#include <sstream>
#include <tuple>

#include "aios/crypto/sha1.hpp"
#include "aios/hal/host.hpp"
#include "cli.hpp"
#include "color.hpp"

namespace aios_git {

using namespace aios;
using namespace aios::userland;

namespace {
std::string mode6(uint32_t m) {
  char b[16];
  std::snprintf(b, sizeof b, "%06o", m);
  return b;
}
std::vector<std::string> lines_of(const std::string& s, char sep = '\n') {
  std::vector<std::string> out;
  std::string cur;
  for (char ch : s) {
    if (ch == sep) { if (sep == '\n' && !cur.empty() && cur.back() == '\r') cur.pop_back(); out.push_back(cur); cur.clear(); }
    else cur += ch;
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}
bool is_hex40(const std::string& s) {
  if (s.size() != 40) return false;
  for (char ch : s) if (!std::isxdigit((unsigned char)ch)) return false;
  return true;
}
}  // namespace

// ---------------------------------------------------------------------------
int cmd_config(Ctx& c) {
  std::vector<std::string> args(c.args.begin(), c.args.end());
  // git 2.46's subcommands: config get|set|unset|list ...
  std::string sub;
  if (args.size() > 1 && (args[1] == "get" || args[1] == "set" || args[1] == "unset" || args[1] == "list" ||
                          args[1] == "rename-section" || args[1] == "remove-section")) {
    sub = args[1];
    args.erase(args.begin() + 1);
  }
  auto p = parse_options(c, args, 1, {{0, "global"}, {0, "local"}, {0, "system"}, {0, "worktree"}, {'f', "file", Arg::Required},
                                       {0, "get"}, {0, "get-all"}, {0, "get-regexp"}, {0, "unset"}, {0, "unset-all"},
                                       {0, "add"}, {0, "replace-all"}, {'l', "list"}, {0, "bool"}, {0, "int"}, {0, "path"},
                                       {0, "bool-or-int"}, {0, "expiry-date"},
                                       {0, "type", Arg::Required}, {0, "default", Arg::Required}, {0, "name-only"}, {'z', "null"},
                                       {0, "remove-section"}, {0, "rename-section"}, {0, "show-origin"}, {0, "show-scope"},
                                       {0, "includes"}, {'e', "edit"}, {0, "all"}, {0, "value", Arg::Required}, {0, "fixed-value"},
                                       {0, "get-urlmatch"}, {0, "get-color"}, {0, "get-colorbool"}, {0, "blob", Arg::Required}},
                       sub.empty());   // git stops at the first non-option: "config --get a.b --show-origin" names a value pattern
  if (p.has("edit") || p.has("get-urlmatch") || p.has("get-color") || p.has("get-colorbool") || p.has("blob") || p.has("value") ||
      p.has("fixed-value"))
    unsupported(c, "config --edit/--get-urlmatch/--get-color/--blob/--value");
  bool have_repo = c.try_open_repo();
  if (!have_repo) c.load_config();
  const int scopes = (int)p.has("global") + (int)p.has("local") + (int)p.has("system") + (int)p.has("worktree") + (int)p.has("file");
  if (scopes > 1) die(c, "only one config file at a time");
  auto system_path = [&]() {
    return c.hasenv("GIT_CONFIG_SYSTEM") ? slashes(c.getenv("GIT_CONFIG_SYSTEM")) : std::string("/etc/gitconfig");
  };
  auto worktree_path = [&]() {
    if (!have_repo) die(c, "--worktree cannot be used outside a repository");
    return c.config.get_bool("extensions.worktreeconfig", false) ? c.repo->git_dir() + "/config.worktree" : c.repo->common_dir() + "/config";
  };
  std::string file, file_scope;
  if (p.has("file")) { file = c.abs_path(p.get("file")); file_scope = "command"; }
  else if (p.has("global")) {
    if (c.global_config_path.empty()) die(c, "$HOME not set");
    file = c.global_config_path;
    file_scope = "global";
  } else if (p.has("system")) {
    file = system_path();
    file_scope = "system";
  } else if (p.has("worktree")) {
    file = worktree_path();
    file_scope = c.config.get_bool("extensions.worktreeconfig", false) ? "worktree" : "local";
  } else if (have_repo) {
    file = c.repo->common_dir() + "/config";
    file_scope = "local";
  }
  // reading: from one file when one was named (its includes are not followed), else everything
  std::vector<ConfigEntry> view;
  if (scopes) {
    if (p.has("local") && !have_repo) die(c, "--local can only be used inside a git repository");
    auto r = c.host().read_file(file);
    if (r.ok()) {
      Config one;
      one.add_text(std::string((const char*)r->data(), r->size()), file_scope, p.has("file") ? "file:" + p.get("file") : c.config_origin(file));
      view = one.entries();
    } else if (p.has("file") && (p.has("list") || sub == "list")) {
      die(c, "unable to read config file '" + p.get("file") + "': No such file or directory");
    }
  } else {
    view = c.config.entries();
  }
  std::string type = p.get("type");
  for (const char* t : {"bool", "int", "path", "bool-or-int", "expiry-date"}) if (p.has(t)) type = t;
  if (!type.empty() && type != "bool" && type != "int" && type != "bool-or-int" && type != "path" && type != "expiry-date" && type != "color")
    die(c, "unrecognized --type argument, " + type);
  if (type == "color") unsupported(c, "config --type=color");
  if (type == "expiry-date") unsupported(c, "config --type=expiry-date");
  auto format_value = [&](const std::string& key, const std::string& v, bool has_value, const std::string& origin) -> std::string {
    auto where = [&]() { return origin.empty() ? std::string() : " in " + (origin.rfind("file:", 0) == 0 ? "file " + origin.substr(5) : origin); };
    if (type == "bool" || type == "bool-or-int") {
      if (type == "bool-or-int" && has_value) {
        char* end = nullptr;
        long long n = std::strtoll(v.c_str(), &end, 10);
        if (end != v.c_str() && !*end) return std::to_string(n);
      }
      bool ok = true;
      bool b = Config::parse_bool(v, has_value, &ok);
      if (!ok) {
        // a number with a unit (4k) reads as its value
        char* end = nullptr;
        long long n = std::strtoll(v.c_str(), &end, 10);
        std::string unit = end ? end : "";
        if (end == v.c_str() || (unit != "k" && unit != "K" && unit != "m" && unit != "M" && unit != "g" && unit != "G"))
          die(c, "bad boolean config value '" + v + "' for '" + key + "'");
        b = n != 0;
      }
      return b ? "true" : "false";
    }
    if (type == "int") {
      std::string s = v;
      while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
      size_t lead = 0;
      while (lead < s.size() && std::isspace((unsigned char)s[lead])) ++lead;
      s = s.substr(lead);
      char* end = nullptr;
      long long n = std::strtoll(s.c_str(), &end, 10);
      if (!has_value || s.empty() || end == s.c_str()) die(c, "bad numeric config value '" + v + "' for '" + key + "'" + where() + ": invalid unit");
      long long mult = 1;
      std::string unit = end ? end : "";
      if (unit == "k" || unit == "K") mult = 1024;
      else if (unit == "m" || unit == "M") mult = 1024 * 1024;
      else if (unit == "g" || unit == "G") mult = 1024LL * 1024 * 1024;
      else if (!unit.empty()) die(c, "bad numeric config value '" + v + "' for '" + key + "'" + where() + ": invalid unit");
      return std::to_string(n * mult);
    }
    if (type == "path") {
      if (!has_value) die(c, "missing value for '" + key + "'");
      std::string home = slashes(c.getenv("HOME"));
      if (v == "~") return home;
      if (v.rfind("~/", 0) == 0) return home + v.substr(1);
      if (v.rfind("%(prefix)/", 0) == 0) die(c, "%(prefix)/ is not supported");
      return v;
    }
    if (type == "expiry-date") {
      int64_t t = 0;
      std::string tz;
      if (!has_value || !parse_date(v, t, tz)) die(c, "'" + v + "' for '" + key + "' is not a valid timestamp");
      return std::to_string(t);
    }
    return has_value ? v : "";
  };
  const bool show_origin = p.has("show-origin"), show_scope = p.has("show-scope");
  char term = p.has("null") ? '\0' : '\n';
  const char sep = p.has("null") ? '\0' : '\t';
  auto prefix_of = [&](const ConfigEntry& e) {
    std::string pre;
    if (show_scope) pre += e.scope + sep;
    if (show_origin) pre += e.origin + sep;
    return pre;
  };
  std::vector<std::string> pos = p.pos;
  auto write = [&](const std::string& key, const std::string& value, ConfigEdit how) -> int {
    if (file.empty()) die(c, "not in a git directory");
    std::string err;
    if (!edit_config_file(c.host(), file, key, value, how, &err)) {
      if (err.empty()) return 5;     // nothing to unset
      c.err << "error: " << err << "\n";
      return err.find("multiple values") != std::string::npos ? 5 : 1;
    }
    return 0;
  };
  if (sub == "list" || p.has("list")) {
    for (auto& e : view) {
      c.out << prefix_of(e);
      if (p.has("name-only")) c.out << e.key << term;
      else if (e.has_value) c.out << e.key << (p.has("null") ? "\n" : "=") << e.value << term;
      else c.out << e.key << term;
    }
    return 0;
  }
  if (p.has("remove-section") || sub == "remove-section") {
    if (pos.size() != 1) usage(c, "wrong number of arguments, should be 1");
    remove_config_section(c.host(), file, Config::canon(pos[0] + ".x").substr(0, Config::canon(pos[0] + ".x").size() - 2));
    return 0;
  }
  if (p.has("rename-section") || sub == "rename-section") {
    if (pos.size() != 2) usage(c, "wrong number of arguments, should be 2");
    rename_config_section(c.host(), file, pos[0], pos[1]);
    return 0;
  }
  if (p.has("unset") || p.has("unset-all") || sub == "unset") {
    if (pos.empty()) usage(c, "wrong number of arguments, should be 1");
    return write(pos[0], "", (p.has("unset-all") || p.has("all")) ? ConfigEdit::UnsetAll : ConfigEdit::Unset);
  }
  if (p.has("get-regexp")) {
    if (pos.empty()) usage(c, "wrong number of arguments");
    std::regex re(pos[0], std::regex::extended | std::regex::icase);
    int found = 0;
    for (auto& e : view) {
      if (!std::regex_search(e.key, re)) continue;
      if (pos.size() > 1 && !std::regex_search(e.value, std::regex(pos[1], std::regex::extended))) continue;
      ++found;
      c.out << prefix_of(e) << e.key;
      if (!p.has("name-only") && (e.has_value || !type.empty())) c.out << (p.has("null") ? '\n' : ' ') << format_value(e.key, e.value, e.has_value, e.origin);
      c.out << term;
    }
    return found ? 0 : 1;
  }
  if (p.has("get") || p.has("get-all") || sub == "get" || (pos.size() == 1 && !p.has("add") && !p.has("replace-all") && sub != "set")) {
    if (pos.empty()) usage(c, "wrong number of arguments, should be 1");
    std::string key = pos[0];
    if (key.find('.') == std::string::npos || key.back() == '.') {
      c.err << "error: key does not contain a section: " << key << "\n";
      return 1;
    }
    std::string ck = Config::canon(key);
    std::vector<const ConfigEntry*> hits;
    for (auto& e : view) if (e.key == ck) hits.push_back(&e);
    if (pos.size() > 1) {
      std::regex re(pos[1], std::regex::extended);
      std::vector<const ConfigEntry*> f;
      for (auto* h : hits) if (std::regex_search(h->value, re)) f.push_back(h);
      hits = f;
    }
    if (hits.empty()) {
      if (p.has("default")) { c.out << format_value(ck, p.get("default"), true, "") << term; return 0; }
      return 1;
    }
    auto show = [&](const ConfigEntry& h) { c.out << prefix_of(h) << format_value(ck, h.value, h.has_value, h.origin) << term; };
    if (p.has("get-all") || p.has("all")) for (auto* h : hits) show(*h);
    else show(*hits.back());
    return 0;
  }
  if (pos.size() < 2) usage(c, "wrong number of arguments");
  std::string key = pos[0], value = pos[1];
  if (key.find('.') == std::string::npos || key.back() == '.') {
    c.err << "error: key does not contain a section: " << key << "\n";
    return 2;
  }
  if (type == "bool" || type == "int" || type == "bool-or-int" || type == "path" || type == "expiry-date") value = format_value(key, value, true, "");
  if (p.has("add")) return write(key, value, ConfigEdit::Add);
  if (p.has("replace-all") || p.has("all")) return write(key, value, ConfigEdit::ReplaceAll);
  if (pos.size() > 2) unsupported(c, "config <key> <value> <value-pattern>");
  return write(key, value, ConfigEdit::Set);
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
int cmd_hash_object(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{'w', ""}, {'t', "", Arg::Required}, {0, "stdin"}, {0, "stdin-paths"}, {0, "path", Arg::Required},
                                         {0, "no-filters"}, {0, "literally"}});
  std::string type = p.get("t", "blob");
  bool write = p.has("w");
  if (write) c.open_repo(false);
  else if (!c.try_open_repo()) {}
  auto hash = [&](const std::string& data) -> std::string {
    std::string header = type + " " + std::to_string(data.size());
    if (write) {
      if (type == "tree" && !data.empty()) {
        // a tree must parse
      }
      auto w = c.repo->write_object(type, ByteSpan((const uint8_t*)data.data(), data.size()));
      if (!w.ok()) die(c, w.error().message);
      return *w;
    }
    if (type == "blob") return hash_blob(data);
    // other types: hash the same way
    std::string all = header;
    all.push_back('\0');
    all += data;
    Git tmp(*c.kernel);
    (void)tmp;
    auto sum = crypto::sha1(ByteSpan((const uint8_t*)all.data(), all.size()));
    static const char* hx = "0123456789abcdef";
    std::string o;
    for (uint8_t b : sum) { o += hx[b >> 4]; o += hx[b & 15]; }
    return o;
  };
  if (p.has("stdin")) c.out << hash(read_all(c.in)) << "\n";
  std::vector<std::string> files = p.pos;
  files.insert(files.end(), p.after_dashdash.begin(), p.after_dashdash.end());
  if (p.has("stdin-paths")) for (auto& l : lines_of(read_all(c.in))) files.push_back(l);
  for (auto& f : files) {
    std::string data;
    if (f != "/dev/null" && f != "NUL" && f != "nul") {
      auto r = c.host().read_file(c.abs_path(f));
      if (!r.ok()) die(c, "could not open '" + f + "' for reading: No such file or directory");
      data.assign((const char*)r->data(), r->size());
    }
    c.out << hash(data) << "\n";
  }
  return 0;
}

// ---------------------------------------------------------------------------
int cmd_mktree(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{'z', ""}, {0, "missing"}, {0, "batch"}});
  if (p.has("batch")) unsupported(c, "mktree --batch");
  c.open_repo(false);
  std::string input = read_all(c.in);
  std::vector<GitTreeEntry> ents;
  for (auto& line : lines_of(input, p.has("z") ? '\0' : '\n')) {
    if (line.empty()) continue;
    size_t tab = line.find('\t');
    if (tab == std::string::npos) die(c, "input format error: " + line);
    std::string meta = line.substr(0, tab), name = line.substr(tab + 1);
    std::istringstream ms(meta);
    std::string mode, type, oid;
    ms >> mode >> type >> oid;
    if (oid.size() != 40) die(c, "input format error: " + line);
    uint32_t m = (uint32_t)std::stoul(mode, nullptr, 8);
    if (!p.has("missing") && type != "commit") {
      auto o = c.repo->read_object(oid);
      if (!o.ok()) die(c, "entry '" + name + "' object " + oid + " is unavailable");
      if (o->type != type) die(c, "entry '" + name + "' object " + oid + " is a " + o->type + " but specified type was (" + type + ")");
    }
    ents.push_back(GitTreeEntry{name, oid, m});
  }
  std::sort(ents.begin(), ents.end(), [](const GitTreeEntry& a, const GitTreeEntry& b) {
    return (a.is_dir() ? a.name + "/" : a.name) < (b.is_dir() ? b.name + "/" : b.name);
  });
  ByteBuffer payload;
  for (auto& e : ents) {
    char m[16];
    std::snprintf(m, sizeof m, "%o", e.mode);
    std::string head = std::string(m) + " " + e.name;
    payload.insert(payload.end(), head.begin(), head.end());
    payload.push_back(0);
    for (size_t i = 0; i < 40; i += 2) payload.push_back((uint8_t)std::stoi(e.oid.substr(i, 2), nullptr, 16));
  }
  auto w = c.repo->write_object("tree", ByteSpan(payload.data(), payload.size()));
  if (!w.ok()) die(c, w.error().message);
  c.out << *w << "\n";
  return 0;
}

// ---------------------------------------------------------------------------
// checkout-index: the work tree's files from the index (-a: all of them; -f: over files that differ).
int cmd_checkout_index(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{'a', "all"}, {'f', "force"}, {'q', "quiet"}, {'u', "index"}, {'n', "no-create"}, {'z', ""},
                                         {0, "prefix", Arg::Required}, {0, "stdin"}, {0, "temp"}, {0, "ignore-skip-worktree-bits"}});
  c.open_repo(false);
  if (p.has("temp") || p.has("stdin")) unsupported(c, "checkout-index --temp/--stdin");
  if (c.top.empty()) die(c, "this operation must be run in a work tree");
  const std::string pre = p.has("prefix") ? p.get("prefix") : "";
  std::set<std::string> want;
  if (!p.has("all")) for (auto& u : p.pos) want.insert(c.to_repo_path(u));
  if (!p.has("all") && want.empty()) return 0;
  std::vector<GitIndexEntry> idx = c.repo->index();
  int status = 0;
  std::set<std::string> found;
  for (auto& e : idx) {
    if (e.mode == 0160000) continue;
    if (!p.has("all") && !want.count(e.path)) continue;
    found.insert(e.path);
    if (p.has("no-create")) continue;
    if (pre.empty() && !p.has("force") && c.host().exists(join(c.top, e.path)) && !c.repo->worktree_matches(e)) {
      if (!p.has("quiet")) c.err << "git checkout-index: " << e.path << " already exists\n";
      status = 1;
      continue;
    }
    if (!pre.empty()) {
      auto blob = c.repo->read_object(e.oid);
      if (!blob.ok()) { status = 1; continue; }
      std::string to = join(c.top, pre + e.path);
      (void)c.host().make_dir(dirname_of(to));
      if (!c.host().write_file(to, ByteSpan(blob->payload.data(), blob->payload.size())).ok()) status = 1;
      continue;
    }
    auto w = c.repo->write_worktree_file(e.path, e.oid, e.mode);
    if (!w.ok()) { c.err << "error: " << w.error().str() << "\n"; status = 1; }
  }
  for (auto& w : want) if (!found.count(w) && !p.has("quiet")) { c.err << "git checkout-index: " << w << " is not in the cache\n"; status = 1; }
  return status;
}

int cmd_write_tree(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{0, "missing-ok"}, {0, "prefix", Arg::Required}});
  c.open_repo(false);
  if (!c.repo->unmerged().empty()) {
    std::set<std::string> seen;
    for (auto& u : c.repo->unmerged()) if (seen.insert(u.path).second) c.err << "error: " << u.path << ": unmerged (" << u.oid << ")\n";
    die(c, "git-write-tree: error building trees");
  }
  std::vector<GitIndexEntry> entries = c.repo->index();
  if (!p.has("missing-ok"))
    for (auto& e : entries)
      if (e.mode != 0160000 && !c.repo->read_object(e.oid).ok()) {
        c.err << "error: invalid object " << mode6(e.mode) << " " << e.oid << " for '" << e.path << "'\n";
        die(c, "git-write-tree: error building trees");
      }
  if (p.has("prefix")) {
    std::string pre = p.get("prefix");
    while (!pre.empty() && pre.back() == '/') pre.pop_back();
    std::vector<GitIndexEntry> sub;
    for (auto& e : entries)
      if (e.path.rfind(pre + "/", 0) == 0) { GitIndexEntry n = e; n.path = e.path.substr(pre.size() + 1); sub.push_back(n); }
    if (sub.empty()) die(c, "git-write-tree: prefix " + pre + " not found");
    entries = sub;
  }
  c.out << write_tree_of(c, entries) << "\n";
  return 0;
}

// ---------------------------------------------------------------------------
int cmd_read_tree(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{'m', ""}, {0, "reset"}, {'u', ""}, {'i', ""}, {'n', "dry-run"}, {'v', ""},
                                         {0, "prefix", Arg::Required}, {0, "empty"}, {'q', ""}, {0, "trivial"}, {0, "aggressive"},
                                         {0, "index-output", Arg::Required}, {0, "no-sparse-checkout"}, {0, "exclude-per-directory", Arg::Required},
                                         {0, "recurse-submodules"}});
  c.open_repo(false);
  if (p.has("index-output") || p.has("trivial") || p.has("aggressive")) unsupported(c, "read-tree --index-output/--trivial/--aggressive");
  if (p.pos.size() > 1) unsupported(c, "read-tree with more than one tree (a merge)");
  bool update = p.has("u");
  if (update && c.top.empty()) die(c, "--prefix/-u needs a work tree");
  std::vector<GitIndexEntry> old = c.repo->index();
  std::vector<GitIndexEntry> next;
  if (p.has("empty")) {
    if (!p.pos.empty()) usage(c, "passing trees as arguments contradicts --empty");
  } else {
    if (p.pos.empty()) usage(c, "usage: git read-tree [<options>] <tree-ish>");
    std::string tree = peel_to(c, must_resolve(c, p.pos[0]), "tree");
    if (tree.empty()) die(c, "failed to unpack tree object " + p.pos[0]);
    Snapshot t = snapshot_tree(c, tree);
    if (p.has("prefix")) {
      std::string pre = p.get("prefix");
      if (!pre.empty() && pre.back() != '/') pre += "/";
      std::set<std::string> have;
      for (auto& e : old) have.insert(e.path);
      for (auto& [path, f] : t) {
        if (have.count(pre + path)) {
          c.err << "error: Entry '" << pre + path << "' overlaps with '" << pre + path << "'.  Cannot bind.\n";
          throw Exit{128};
        }
      }
      next = old;
      for (auto& [path, f] : t) next.push_back(GitIndexEntry{pre + path, f.oid, f.mode, 0, 0});
    } else {
      std::map<std::string, const GitIndexEntry*> was;
      for (auto& e : old) was[e.path] = &e;
      for (auto& [path, f] : t) {
        auto it = was.find(path);
        // keep the stat cache for an entry that did not change (what -m and --reset do)
        if (it != was.end() && it->second->oid == f.oid && it->second->mode == f.mode && (p.has("m") || p.has("reset") || update))
          next.push_back(*it->second);
        else
          next.push_back(GitIndexEntry{path, f.oid, f.mode, 0, 0});
      }
    }
  }
  if (p.has("dry-run")) return 0;
  if (update) {
    // the work tree follows the new index; without --reset, local changes block it
    std::map<std::string, const GitIndexEntry*> now;
    for (auto& e : next) now[e.path] = &e;
    if (!p.has("reset")) {
      for (auto& e : old) {
        auto it = now.find(e.path);
        bool changes = it == now.end() || it->second->oid != e.oid || it->second->mode != e.mode;
        if (changes && !c.repo->worktree_matches(e) && c.host().exists(join(c.top, e.path)))
          die(c, "Entry '" + e.path + "' not uptodate. Cannot merge.");
      }
    }
    for (auto& e : old) if (!now.count(e.path)) (void)c.host().remove(join(c.top, e.path));
    std::map<std::string, const GitIndexEntry*> was;
    for (auto& e : old) was[e.path] = &e;
    for (auto& e : next) {
      auto it = was.find(e.path);
      if (it != was.end() && it->second->oid == e.oid && it->second->mode == e.mode && c.repo->worktree_matches(*it->second)) continue;
      auto w = c.repo->write_worktree_file(e.path, e.oid, e.mode);
      if (!w.ok()) die(c, w.error().message);
      e.mtime_ns = w->mtime_ns;
      e.size = w->size;
    }
  }
  c.repo->set_unmerged({});
  c.repo->set_index(next);
  if (auto w = c.repo->write_index(); !w.ok()) die(c, "unable to write new index file: " + w.error().message);
  return 0;
}

// ---------------------------------------------------------------------------
int cmd_update_index(Ctx& c) {
  c.open_repo(false);
  std::vector<GitIndexEntry> index = c.repo->index();
  std::vector<GitIndexEntry> unmerged = c.repo->unmerged();
  auto find = [&](const std::string& path) -> GitIndexEntry* {
    for (auto& e : index) if (e.path == path) return &e;
    return nullptr;
  };
  auto drop = [&](const std::string& path) {
    index.erase(std::remove_if(index.begin(), index.end(), [&](const GitIndexEntry& e) { return e.path == path; }), index.end());
    unmerged.erase(std::remove_if(unmerged.begin(), unmerged.end(), [&](const GitIndexEntry& e) { return e.path == path; }), unmerged.end());
  };
  auto set_entry = [&](const GitIndexEntry& e) {
    drop(e.path);
    index.push_back(e);
  };
  bool add = false, remove = false, force_remove = false, quiet = false, ignore_missing = false, info_only = false, z = false, verbose = false;
  std::string chmod;
  enum class Flag { None, SkipOn, SkipOff, AssumeOn, AssumeOff } flag = Flag::None;
  std::vector<std::string> args(c.args.begin() + 1, c.args.end());
  auto update_path = [&](const std::string& user_path) {
    std::string path = c.top.empty() ? user_path : c.to_repo_path(user_path);
    if (path == "..") die(c, "'" + user_path + "' is outside repository");
    GitIndexEntry* e = find(path);
    if (flag != Flag::None) {
      if (!e) die(c, "Unable to mark file " + user_path);
      if (flag == Flag::SkipOn) e->skip_worktree = true;
      if (flag == Flag::SkipOff) e->skip_worktree = false;
      if (flag == Flag::AssumeOn) e->assume_valid = true;
      if (flag == Flag::AssumeOff) e->assume_valid = false;
      return;
    }
    if (!chmod.empty() && e && !c.host().exists(join(c.top, path))) {
      e->mode = chmod == "+x" ? 0100755 : 0100644;
      return;
    }
    if (force_remove) { drop(path); if (verbose) c.out << "remove '" << path << "'\n"; return; }
    auto st = c.host().stat(join(c.top, path));
    if (!st.ok()) {
      if (remove) { drop(path); if (verbose) c.out << "remove '" << path << "'\n"; return; }
      if (ignore_missing) return;
      c.err << "error: " << path << ": does not exist and --remove not passed\n";
      die(c, "Unable to process path " + path);
    }
    if (st->is_dir && !st->is_link) {
      c.err << "error: " << path << ": is a directory - add files inside instead\n";
      die(c, "Unable to process path " + path);
    }
    if (!e && !add) {
      c.err << "error: " << path << ": cannot add to the index - missing --add option?\n";
      die(c, "Unable to process path " + path);
    }
    auto s = c.repo->stage_entry(path, e);
    if (!s.ok()) die(c, "Unable to process path " + path + ": " + s.error().message);
    GitIndexEntry n = *s;
    if (info_only) { /* the object was written anyway; harmless */ }
    if (!chmod.empty()) n.mode = chmod == "+x" ? 0100755 : 0100644;
    set_entry(n);
    if (verbose) c.out << "add '" << path << "'\n";
  };
  auto cacheinfo = [&](const std::string& mode, const std::string& oid, const std::string& path) {
    uint32_t m = (uint32_t)std::stoul(mode, nullptr, 8);
    if (!is_hex40(oid)) die(c, "git update-index: --cacheinfo cannot add " + path);
    if (!find(path) && !add) {
      c.err << "error: " << path << ": cannot add to the index - missing --add option?\n";
      die(c, "git update-index: --cacheinfo cannot add " + path);
    }
    set_entry(GitIndexEntry{path, oid, m, 0, 0});
  };
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "--add") { add = true; continue; }
    if (a == "--remove") { remove = true; continue; }
    if (a == "--force-remove") { force_remove = true; continue; }
    if (a == "-q") { quiet = true; continue; }
    if (a == "--ignore-missing") { ignore_missing = true; continue; }
    if (a == "--info-only") { info_only = true; continue; }
    if (a == "-z") { z = true; continue; }
    if (a == "--verbose") { verbose = true; continue; }
    if (a == "--replace" || a == "--unmerged" || a == "--no-fsmonitor") continue;
    if (a == "--skip-worktree") { flag = Flag::SkipOn; continue; }
    if (a == "--no-skip-worktree") { flag = Flag::SkipOff; continue; }
    if (a == "--assume-unchanged") { flag = Flag::AssumeOn; continue; }
    if (a == "--no-assume-unchanged") { flag = Flag::AssumeOff; continue; }
    if (a.rfind("--chmod=", 0) == 0) { chmod = a.substr(8); if (chmod != "+x" && chmod != "-x") die(c, "git update-index: --chmod param '" + chmod + "' must be either -x or +x"); continue; }
    if (a == "--refresh" || a == "--really-refresh") {
      // re-read stat data of unchanged files (a later status is faster)
      for (auto& e : index) {
        if (e.assume_valid || e.skip_worktree) continue;
        if (c.repo->worktree_matches(e)) {
          auto st = c.host().stat(join(c.top, e.path));
          if (st.ok()) { e.mtime_ns = st->mtime_ns; e.size = st->size; }
        } else if (!quiet) {
          c.out << e.path << ": needs update\n";
        }
      }
      continue;
    }
    if (a == "--cacheinfo") {
      if (i + 1 >= args.size()) usage(c, "option 'cacheinfo' expects <mode>,<sha1>,<path>");
      std::string v = args[++i];
      size_t c1 = v.find(','), c2 = c1 == std::string::npos ? std::string::npos : v.find(',', c1 + 1);
      if (c2 != std::string::npos) cacheinfo(v.substr(0, c1), v.substr(c1 + 1, c2 - c1 - 1), v.substr(c2 + 1));
      else {
        if (i + 2 >= args.size()) usage(c, "option 'cacheinfo' expects <mode>,<sha1>,<path>");
        std::string mode = v, oid = args[++i], path = args[++i];
        cacheinfo(mode, oid, path);
      }
      continue;
    }
    if (a == "--index-info") {
      std::string input = read_all(c.in);
      for (auto& line : lines_of(input, z ? '\0' : '\n')) {
        if (line.empty()) continue;
        size_t tab = line.find('\t');
        if (tab == std::string::npos) die(c, "malformed index info " + line);
        std::string meta = line.substr(0, tab), path = line.substr(tab + 1);
        if (!z && !path.empty() && path[0] == '"') {
          // C-quoted path
          std::string un;
          for (size_t k = 1; k + 1 < path.size(); ++k) {
            if (path[k] == '\\' && k + 1 < path.size() - 1) {
              char e = path[++k];
              if (e >= '0' && e <= '7') { int v = (e - '0') * 64 + (path[k + 1] - '0') * 8 + (path[k + 2] - '0'); un += (char)v; k += 2; }
              else un += e == 'n' ? '\n' : e == 't' ? '\t' : e == 'a' ? '\a' : e == 'b' ? '\b' : e == 'f' ? '\f' : e == 'r' ? '\r' : e == 'v' ? '\v' : e;
            } else un += path[k];
          }
          path = un;
        }
        std::istringstream ms(meta);
        std::vector<std::string> f;
        std::string w;
        while (ms >> w) f.push_back(w);
        uint32_t m = 0;
        std::string oid;
        int stage = 0;
        if (f.size() == 2) { m = (uint32_t)std::stoul(f[0], nullptr, 8); oid = f[1]; }
        else if (f.size() == 3 && is_hex40(f[1])) { m = (uint32_t)std::stoul(f[0], nullptr, 8); oid = f[1]; stage = std::atoi(f[2].c_str()); }
        else if (f.size() == 3) { m = (uint32_t)std::stoul(f[0], nullptr, 8); oid = f[2]; }
        else die(c, "malformed index info " + line);
        if (m == 0) { drop(path); continue; }
        if (!is_hex40(oid)) die(c, "malformed index info " + line);
        if (stage) {
          index.erase(std::remove_if(index.begin(), index.end(), [&](const GitIndexEntry& e) { return e.path == path; }), index.end());
          GitIndexEntry e{path, oid, m};
          e.stage = (uint8_t)stage;
          unmerged.erase(std::remove_if(unmerged.begin(), unmerged.end(), [&](const GitIndexEntry& x) { return x.path == path && x.stage == stage; }), unmerged.end());
          unmerged.push_back(e);
        } else {
          set_entry(GitIndexEntry{path, oid, m, 0, 0});
        }
      }
      continue;
    }
    if (a == "--stdin") {
      std::string input = read_all(c.in);
      for (auto& line : lines_of(input, z ? '\0' : '\n')) if (!line.empty()) update_path(line);
      continue;
    }
    if (a == "--") {
      for (size_t j = i + 1; j < args.size(); ++j) update_path(args[j]);
      break;
    }
    if (a.size() > 1 && a[0] == '-') {
      if (a == "--again" || a == "-g" || a == "--split-index" || a == "--untracked-cache" || a == "--index-version" ||
          a == "--test-untracked-cache" || a == "--fsmonitor" || a == "--show-index-version")
        unsupported(c, "update-index " + a);
      usage(c, "unknown option '" + a + "'");
    }
    update_path(a);
  }
  c.repo->set_index(index);
  c.repo->set_unmerged(unmerged);
  if (auto w = c.repo->write_index(); !w.ok()) die(c, "Unable to write new index file: " + w.error().message);
  return 0;
}

// ---------------------------------------------------------------------------
int cmd_commit_tree(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{'p', "", Arg::Required}, {'m', "", Arg::Required}, {'F', "", Arg::Required},
                                         {'S', "gpg-sign", Arg::Optional}, {0, "no-gpg-sign"}});
  // commit-tree signs only when asked (-S), whatever commit.gpgsign says
  c.sign = 0;
  take_sign_option(c, p);
  c.open_repo(false);
  if (p.pos.size() != 1) usage(c, "usage: git commit-tree <tree> [(-p <parent>)...] [-m <message>] [-F <file>]");
  std::string tree = peel_to(c, must_resolve(c, p.pos[0]), "tree");
  if (tree.empty()) die(c, p.pos[0] + " is not a valid 'tree' object");
  std::vector<std::string> parents;
  for (auto& pr : p.all("p")) {
    std::string cm = commit_of(c, pr);
    if (std::find(parents.begin(), parents.end(), cm) != parents.end()) { c.err << "error: duplicate parent " << cm << " ignored\n"; continue; }
    parents.push_back(cm);
  }
  std::string msg;
  bool have = false;
  for (auto& a : c.args) (void)a;
  // -m and -F in the order given, as paragraphs
  for (size_t i = 1; i < c.args.size(); ++i) {
    const std::string& a = c.args[i];
    std::string val;
    bool is_m = false, is_f = false;
    if (a == "-m" && i + 1 < c.args.size()) { val = c.args[++i]; is_m = true; }
    else if (a.rfind("-m", 0) == 0 && a.size() > 2 && a[1] == 'm') { val = a.substr(2); is_m = true; }
    else if (a == "-F" && i + 1 < c.args.size()) { val = c.args[++i]; is_f = true; }
    else if (a == "-p") { ++i; continue; }
    if (!is_m && !is_f) continue;
    if (have) msg += "\n";
    if (is_m) { msg += val; if (msg.empty() || msg.back() != '\n') msg += "\n"; }
    else {
      std::string t;
      if (val == "-") t = read_all(c.in);
      else {
        auto r = c.host().read_file(c.abs_path(val));
        if (!r.ok()) die(c, "git commit-tree: failed to read '" + val + "'");
        t.assign((const char*)r->data(), r->size());
      }
      msg += t;
    }
    have = true;
  }
  if (!have) msg = read_all(c.in);
  c.out << make_commit(c, tree, parents, msg, {}, {}, true) << "\n";
  return 0;
}

// ---------------------------------------------------------------------------
int cmd_update_ref(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{'d', ""}, {'m', "", Arg::Required}, {0, "no-deref"}, {0, "create-reflog"}, {0, "stdin"},
                                         {'z', ""}});
  c.open_repo(false);
  std::string reason = p.get("m");
  if (p.has("stdin")) {
    std::string input = read_all(c.in);
    struct Op { std::string kind, ref, newv, oldv; bool has_old = false; };
    std::vector<Op> ops;
    for (auto& line : lines_of(input, p.has("z") ? '\0' : '\n')) {
      if (line.empty()) continue;
      std::istringstream ss(line);
      std::vector<std::string> f;
      std::string w;
      while (ss >> w) f.push_back(w);
      if (f.empty()) continue;
      Op o;
      o.kind = f[0];
      if (o.kind == "start" || o.kind == "prepare" || o.kind == "commit") continue;
      if (o.kind == "abort") return 0;
      if (f.size() < 2) die(c, "missing arguments in: " + line);
      o.ref = f[1];
      if (o.kind == "update") { if (f.size() < 3) die(c, "update " + o.ref + ": missing <new-oid>"); o.newv = f[2]; if (f.size() > 3) { o.oldv = f[3]; o.has_old = true; } }
      else if (o.kind == "create") { if (f.size() < 3) die(c, "create " + o.ref + ": missing <new-oid>"); o.newv = f[2]; o.has_old = true; o.oldv = std::string(40, '0'); }
      else if (o.kind == "delete") { if (f.size() > 2) { o.oldv = f[2]; o.has_old = true; } }
      else if (o.kind == "verify") { o.oldv = f.size() > 2 ? f[2] : std::string(40, '0'); o.has_old = true; }
      else unsupported(c, "update-ref --stdin command " + o.kind);
      ops.push_back(o);
    }
    // check everything first: all or nothing
    for (auto& o : ops) {
      if (!o.has_old) continue;
      auto cur = read_ref(c, o.ref);
      bool want_absent = o.oldv.empty() || o.oldv == std::string(40, '0');
      if (want_absent && cur) die(c, "cannot lock ref '" + o.ref + "': reference already exists");
      if (!want_absent) {
        std::string expect = must_resolve(c, o.oldv);
        if (!cur) die(c, "cannot lock ref '" + o.ref + "': reference is missing but expected " + expect);
        if (*cur != expect) die(c, "cannot lock ref '" + o.ref + "': is at " + *cur + " but expected " + expect);
      }
    }
    for (auto& o : ops) {
      if (o.kind == "verify") continue;
      if (o.kind == "delete") { auto r = c.repo->delete_ref(o.ref); if (!r.ok()) die(c, r.error().message); continue; }
      // the old value checked again under the ref's lock: nothing slips in after the check above
      std::optional<std::string> old;
      if (o.has_old) old = o.oldv.empty() || o.oldv == std::string(40, '0') ? std::string() : must_resolve(c, o.oldv);
      update_ref_checked(c, o.ref, must_resolve(c, o.newv), old, reason);
    }
    return 0;
  }
  if (p.has("d")) {
    if (p.pos.empty() || p.pos.size() > 2) usage(c, "usage: git update-ref -d <refname> [<old-oid>]");
    std::string ref = p.pos[0];
    if (ref == "HEAD" && !p.has("no-deref") && !c.repo->head_ref().empty()) ref = c.repo->head_ref();
    auto cur = read_ref(c, ref);
    if (p.pos.size() == 2) {
      std::string expect = must_resolve(c, p.pos[1]);
      if (!cur) fail(c, "cannot lock ref '" + ref + "': unable to resolve reference '" + ref + "'");
      if (*cur != expect) fail(c, "cannot lock ref '" + ref + "': is at " + *cur + " but expected " + expect);
    }
    if (ref == "HEAD") die(c, "refusing to delete HEAD");
    auto r = c.repo->delete_ref(ref);
    if (!r.ok()) die(c, r.error().message);
    return 0;
  }
  if (p.pos.size() < 2 || p.pos.size() > 3) usage(c, "usage: git update-ref [<options>] <refname> <new-oid> [<old-oid>]");
  std::string ref = p.pos[0];
  std::string newv = must_resolve(c, p.pos[1]);
  std::optional<std::string> oldv;
  if (p.pos.size() == 3) oldv = p.pos[2].empty() ? std::string() : (p.pos[2] == std::string(40, '0') ? std::string() : must_resolve(c, p.pos[2]));
  if (ref == "HEAD" && p.has("no-deref")) {
    auto cur = read_ref(c, "HEAD");
    if (oldv && ((oldv->empty() && cur) || (!oldv->empty() && (!cur || *cur != *oldv))))
      die(c, "cannot lock ref 'HEAD': is at " + cur.value_or("") + " but expected " + *oldv);
    auto r = c.repo->set_head_detached(newv);
    if (!r.ok()) die(c, r.error().message);
    return 0;
  }
  if (ref != "HEAD" && ref.rfind("refs/", 0) != 0 && ref != "FETCH_HEAD" && ref != "ORIG_HEAD" && ref != "MERGE_HEAD") {
    // a bare name is taken as given (git does too)
  }
  if (ref == "ORIG_HEAD" || ref == "MERGE_HEAD" || ref == "FETCH_HEAD" || ref == "CHERRY_PICK_HEAD") {
    c.write_git_file(ref, newv + "\n");
    return 0;
  }
  if (oldv) {
    // update-ref words a failed lease its own way
    std::string full = ref == "HEAD" && !c.repo->head_ref().empty() ? c.repo->head_ref() : ref;
    auto cur = read_ref(c, full);
    std::string pre = "update_ref failed for ref '" + full + "': cannot lock ref '" + full + "': ";
    if (oldv->empty() && cur) die(c, pre + "reference already exists");
    if (!oldv->empty() && !cur) die(c, pre + "reference is missing but expected " + *oldv);
    if (!oldv->empty() && *cur != *oldv) die(c, pre + "is at " + *cur + " but expected " + *oldv);
  }
  update_ref_checked(c, ref, newv, oldv, reason);   // with the old value: swapped under the ref's lock
  return 0;
}

// ---------------------------------------------------------------------------
int cmd_symbolic_ref(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{'q', "quiet"}, {0, "short"}, {'d', "delete"}, {'m', "", Arg::Required}, {0, "no-recurse"}, {0, "recurse"}});
  c.open_repo(false);
  if (p.pos.empty()) usage(c, "usage: git symbolic-ref [-m <reason>] <name> <ref>");
  std::string name = p.pos[0];
  if (p.has("delete")) {
    if (name == "HEAD") die(c, "deleting 'HEAD' is not allowed");
    (void)c.host().remove(c.repo->common_dir() + "/" + name);
    return 0;
  }
  if (p.pos.size() == 2) {
    std::string target = p.pos[1];
    if (target.rfind("refs/", 0) != 0) die(c, "Refusing to point " + name + " outside of refs/");
    if (name == "HEAD") {
      auto r = c.repo->set_head(target);
      if (!r.ok()) die(c, r.error().message);
    } else {
      std::string path = c.repo->common_dir() + "/" + name;
      (void)c.host().make_dir(dirname_of(path));
      std::string body = "ref: " + target + "\n";
      c.host().write_file(path, ByteSpan((const uint8_t*)body.data(), body.size()));
    }
    return 0;
  }
  std::string target;
  if (name == "HEAD") target = c.repo->head_ref();
  else {
    auto r = c.host().read_file((name.rfind("refs/", 0) == 0 ? c.repo->common_dir() : c.repo->git_dir()) + "/" + name);
    if (r.ok()) {
      std::string s((const char*)r->data(), r->size());
      while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
      if (s.rfind("ref: ", 0) == 0) target = s.substr(5);
    }
  }
  if (target.empty()) {
    if (p.has("quiet")) return 1;
    die(c, "ref " + name + " is not a symbolic ref");
  }
  if (p.has("short")) {
    for (const char* pre : {"refs/heads/", "refs/tags/", "refs/remotes/"})
      if (target.rfind(pre, 0) == 0) { target = target.substr(std::strlen(pre)); break; }
  }
  c.out << target << "\n";
  return 0;
}

// ---------------------------------------------------------------------------
int cmd_ls_files(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{'c', "cached"}, {'o', "others"}, {'d', "deleted"}, {'m', "modified"}, {'i', "ignored"},
                                         {'s', "stage"}, {'u', "unmerged"}, {'k', "killed"}, {'z', ""}, {0, "exclude-standard"},
                                         {'x', "exclude", Arg::Required}, {'X', "exclude-from", Arg::Required}, {0, "full-name"},
                                         {0, "error-unmatch"}, {0, "directory"}, {0, "no-empty-directory"}, {'t', ""}, {'v', ""},
                                         {0, "eol"}, {0, "deduplicate"}, {0, "abbrev", Arg::Optional}, {0, "format", Arg::Required},
                                         {0, "recurse-submodules"}, {0, "sparse"}, {0, "exclude-per-directory", Arg::Required},
                                         {0, "with-tree", Arg::Required}, {'f', ""}, {0, "debug"}});
  c.open_repo(false);
  if (p.has("killed") || p.has("t") || p.has("v") || p.has("eol") || p.has("format") || p.has("recurse-submodules") ||
      p.has("with-tree") || p.has("f") || p.has("debug") || p.has("exclude-from") || p.has("exclude-per-directory"))
    unsupported(c, "this ls-files option");
  bool z = p.has("z");
  bool cached = p.has("cached"), others = p.has("others"), deleted = p.has("deleted"), modified = p.has("modified");
  bool stage = p.has("stage"), unmerged_only = p.has("unmerged"), ignored = p.has("ignored");
  if (unmerged_only) stage = true;
  if (!others && !deleted && !modified && !stage && !ignored) cached = true;
  std::vector<std::string> paths = p.pos;
  paths.insert(paths.end(), p.after_dashdash.begin(), p.after_dashdash.end());
  Pathspec ps = make_pathspec(c, paths);
  // without paths, only what is under the current directory
  bool full = p.has("full-name");
  auto in_scope = [&](const std::string& path) {
    if (!ps.empty()) return ps.match(path);
    return c.prefix.empty() || path.rfind(c.prefix, 0) == 0;
  };
  auto shown = [&](const std::string& path) {
    std::string s = full ? path : c.to_display_path(path);
    return z ? s : quote_path(s);
  };
  char term = z ? '\0' : '\n';
  std::vector<bool> matched(ps.items.size(), false);
  auto mark = [&](const std::string& path) { int i = ps.matched_index(path); if (i >= 0) matched[(size_t)i] = true; };
  std::vector<std::string> excludes = p.all("exclude");
  if (others || ignored) {
    std::vector<std::string> list;
    if (ignored) {
      // tracked or untracked paths that the excludes match
      if (!p.has("exclude-standard") && excludes.empty()) die(c, "ls-files -i must be used with either -o or -c, and with --exclude-standard or -x");
      std::function<void(const std::string&)> walkdir = [&](const std::string& d) {
        auto names = c.host().list_dir(join(c.top, d));
        if (!names.ok()) return;
        std::vector<std::string> sorted = *names;
        std::sort(sorted.begin(), sorted.end());
        for (auto& n : sorted) {
          if (n == "." || n == ".." || n == ".git") continue;
          std::string rp = d.empty() ? n : d + "/" + n;
          auto st = c.host().stat(join(c.top, rp));
          if (!st.ok()) continue;
          bool dir = st->is_dir && !st->is_link;
          bool ig = c.repo->is_ignored(rp, dir);
          for (auto& x : excludes) if (git_wildmatch(x, basename_of(rp))) ig = true;
          if (dir) {
            if (ig && p.has("directory")) { if (others) list.push_back(rp + "/"); continue; }
            walkdir(rp);
            continue;
          }
          bool tracked = false;
          for (auto& e : c.repo->index()) if (e.path == rp) { tracked = true; break; }
          if (ig && ((others && !tracked) || (cached && tracked))) list.push_back(rp);
        }
      };
      walkdir("");
    } else if (p.has("exclude-standard") || !excludes.empty()) {
      auto un = c.repo->untracked(p.has("directory"), true);
      if (!un.ok()) die(c, un.error().message);
      for (auto& u : *un) {
        bool ex = false;
        for (auto& x : excludes) if (git_wildmatch(x, basename_of(u))) ex = true;
        if (!ex) list.push_back(u);
      }
    } else {
      // every file not in the index, ignored ones included
      std::set<std::string> tracked;
      for (auto& e : c.repo->index()) tracked.insert(e.path);
      std::function<void(const std::string&)> walkdir = [&](const std::string& d) {
        auto names = c.host().list_dir(join(c.top, d));
        if (!names.ok()) return;
        std::vector<std::string> sorted = *names;
        std::sort(sorted.begin(), sorted.end());
        for (auto& n : sorted) {
          if (n == "." || n == ".." || n == ".git") continue;
          std::string rp = d.empty() ? n : d + "/" + n;
          auto st = c.host().stat(join(c.top, rp));
          if (!st.ok()) continue;
          if (st->is_dir && !st->is_link) {
            if (c.host().exists(join(c.top, rp) + "/.git") && !tracked.count(rp)) { list.push_back(rp + "/"); continue; }
            walkdir(rp);
          } else if (!tracked.count(rp)) {
            list.push_back(rp);
          }
        }
      };
      walkdir("");
    }
    for (auto& u : list) {
      std::string probe = u.back() == '/' ? u.substr(0, u.size() - 1) : u;
      if (!in_scope(probe)) continue;
      mark(probe);
      c.out << shown(u) << term;
    }
    if (ignored && !others) return 0;
  }
  if (stage) {
    std::vector<GitIndexEntry> all = unmerged_only ? std::vector<GitIndexEntry>{} : c.repo->index();
    for (auto& u : c.repo->unmerged()) all.push_back(u);
    std::sort(all.begin(), all.end(), [](const GitIndexEntry& a, const GitIndexEntry& b) { return a.path != b.path ? a.path < b.path : a.stage < b.stage; });
    for (auto& e : all) {
      if (!in_scope(e.path)) continue;
      mark(e.path);
      std::string oid = p.has("abbrev") ? c.abbrev(e.oid, p.get("abbrev").empty() ? 0 : std::atoi(p.get("abbrev").c_str())) : e.oid;
      c.out << mode6(e.mode) << " " << oid << " " << (int)e.stage << "\t" << shown(e.path) << term;
    }
  } else if (cached || deleted || modified) {
    std::set<std::string> changed, gone;
    if (deleted || modified) {
      auto wt = c.repo->worktree_changes();
      if (!wt.ok()) die(c, wt.error().message);
      for (auto& ch : *wt) { changed.insert(ch.path); if (ch.change == GitChange::Deleted) gone.insert(ch.path); }
    }
    std::vector<std::string> names;
    for (auto& e : c.repo->index()) names.push_back(e.path);
    for (auto& u : c.repo->unmerged()) names.push_back(u.path);
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    for (auto& path : names) {
      if (!in_scope(path)) continue;
      mark(path);
      if (cached) c.out << shown(path) << term;
      if (deleted && gone.count(path)) c.out << shown(path) << term;
      if (modified && changed.count(path)) c.out << shown(path) << term;
    }
  }
  if (p.has("error-unmatch")) {
    for (size_t i = 0; i < matched.size(); ++i)
      if (!matched[i]) {
        c.err << "error: pathspec '" << ps.raw_of(i) << "' did not match any file(s) known to git\nDid you forget to 'git add'?\n";
        return 1;
      }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
int cmd_notes(Ctx& c) {
  std::vector<std::string> args(c.args.begin() + 1, c.args.end());
  std::string ref = c.getenv("GIT_NOTES_REF");
  std::vector<std::string> rest;
  for (size_t i = 0; i < args.size(); ++i) {
    if (args[i].rfind("--ref=", 0) == 0) { ref = args[i].substr(6); continue; }
    if (args[i] == "--ref" && i + 1 < args.size()) { ref = args[++i]; continue; }
    rest.push_back(args[i]);
  }
  c.open_repo(false);
  if (ref.empty()) ref = c.config.get("core.notesref").value_or("refs/notes/commits");
  if (ref.rfind("refs/notes/", 0) != 0) ref = ref.rfind("notes/", 0) == 0 ? "refs/" + ref : "refs/notes/" + ref;
  std::string sub = rest.empty() ? "list" : rest[0];
  std::vector<std::string> sargs{"notes"};
  if (!rest.empty()) sargs.insert(sargs.end(), rest.begin() + 1, rest.end());
  if (sub == "list") {
    auto p = parse_options(c, sargs, 1, {});
    auto list = c.repo->list_notes(ref);
    if (!list.ok()) return 0;
    if (!p.pos.empty()) {
      std::string target = must_resolve(c, p.pos[0]);
      for (auto& [blob, obj] : *list) if (obj == target) { c.out << blob << "\n"; return 0; }
      fail(c, "no note found for object " + target + ".", 1);
    }
    auto sorted = *list;
    std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.second < b.second; });
    for (auto& [blob, obj] : sorted) c.out << blob << " " << obj << "\n";
    return 0;
  }
  if (sub == "show") {
    auto p = parse_options(c, sargs, 1, {});
    std::string target = must_resolve(c, p.pos.empty() ? "HEAD" : p.pos[0]);
    auto note = c.repo->read_note(ref, target);
    if (!note.ok()) fail(c, "no note found for object " + target + ".", 1);
    c.out << *note;
    return 0;
  }
  if (sub == "add" || sub == "append") {
    auto p = parse_options(c, sargs, 1, {{'m', "message", Arg::Required}, {'F', "file", Arg::Required}, {'f', "force"},
                                          {'C', "reuse-message", Arg::Required}, {'c', "reedit-message", Arg::Required}, {0, "allow-empty"}});
    std::string target = must_resolve(c, p.pos.empty() ? "HEAD" : p.pos[0]);
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
    if (p.has("reuse-message")) msg = blob_text(c, must_resolve(c, p.get("reuse-message")));
    else {
      if (!p.has("message") && !p.has("file")) die(c, "no note message given, and aios-git does not start an editor: use -m or -F");
      msg = cleanup_message(msg, true);
    }
    auto existing = c.repo->read_note(ref, target);
    if (sub == "append" && existing.ok()) {
      std::string prev = *existing;
      msg = prev + (prev.empty() || prev.back() == '\n' ? "" : "\n") + "\n" + msg;
    } else if (existing.ok() && !p.has("force")) {
      c.err << "error: Cannot add notes. Found existing notes for object " << target << ". Use '-f' to overwrite existing notes\n";
      return 1;
    }
    if (msg.empty() && !p.has("allow-empty")) {
      c.err << "Removing note for object " << target << "\n";
      unsupported(c, "removing a note by adding an empty one");
    }
    if (existing.ok() && p.has("force") && sub == "add") c.err << "Overwriting existing notes for object " << target << "\n";
    // the notes tree: one blob per annotated object, named by its id (flat, or fanned out "ab/cdef..")
    auto blob = c.repo->write_object("blob", ByteSpan((const uint8_t*)msg.data(), msg.size()));
    if (!blob.ok()) die(c, blob.error().message);
    auto parent = read_ref(c, ref);
    std::vector<GitIndexEntry> entries;
    if (parent) {
      for (auto& [path, f] : snapshot_tree(c, read_commit(c, *parent).tree)) {
        std::string flat = path;
        flat.erase(std::remove(flat.begin(), flat.end(), '/'), flat.end());
        if (flat == target) continue;
        entries.push_back(GitIndexEntry{path, f.oid, f.mode});
      }
    }
    entries.push_back(GitIndexEntry{target, *blob, 0100644});
    std::string tree = write_tree_of(c, entries);
    std::string note_msg = std::string("Notes added by 'git notes ") + sub + "'\n";
    std::string oid = make_commit(c, tree, parent ? std::vector<std::string>{*parent} : std::vector<std::string>{}, note_msg);
    update_ref_checked(c, ref, oid, std::nullopt, "notes: " + note_msg.substr(0, note_msg.size() - 1));
    return 0;
  }
  unsupported(c, "notes " + sub);
}

// ---------------------------------------------------------------------------
int cmd_show_ref(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{0, "heads"}, {0, "branches"}, {0, "tags"}, {'s', "hash", Arg::Optional}, {0, "verify"},
                                         {'q', "quiet"}, {'d', "dereference"}, {0, "head"}, {0, "abbrev", Arg::Optional},
                                         {0, "exists"}, {0, "exclude-existing", Arg::Optional}});
  c.open_repo(false);
  if (p.has("exclude-existing")) unsupported(c, "show-ref --exclude-existing");
  auto line = [&](const std::string& oid, const std::string& ref) {
    if (p.has("quiet")) return;
    std::string id = p.has("abbrev") ? c.abbrev(oid) : oid;
    if (p.has("hash")) c.out << id << "\n";
    else c.out << id << " " << ref << "\n";
    if (p.has("dereference") && type_of(c, oid) == "tag") c.out << peel_to(c, oid, "commit") << " " << ref << "^{}\n";
  };
  if (p.has("exists")) {
    if (p.pos.size() != 1) usage(c, "--exists requires exactly one reference");
    return read_ref(c, p.pos[0]) ? 0 : 2;
  }
  if (p.has("verify")) {
    int rc = 0;
    for (auto& r : p.pos) {
      auto o = read_ref(c, r);
      if (!o || (r != "HEAD" && r.rfind("refs/", 0) != 0)) {
        if (p.has("quiet")) return 1;
        die(c, "'" + r + "' - not a valid ref");
      }
      line(*o, r);
    }
    return rc;
  }
  int found = 0;
  if (p.has("head")) if (auto h = read_ref(c, "HEAD")) { line(*h, "HEAD"); ++found; }
  for (auto& [ref, oid] : all_refs(c)) {
    if ((p.has("heads") || p.has("branches") || p.has("tags")) &&
        !((p.has("heads") || p.has("branches")) && ref.rfind("refs/heads/", 0) == 0) && !(p.has("tags") && ref.rfind("refs/tags/", 0) == 0))
      continue;
    if (!p.pos.empty()) {
      bool any = false;
      for (auto& pat : p.pos)
        if (ref == pat || (ref.size() > pat.size() && ref.compare(ref.size() - pat.size(), pat.size(), pat) == 0 && ref[ref.size() - pat.size() - 1] == '/'))
          any = true;
      if (!any) continue;
    }
    line(oid, ref);
    ++found;
  }
  return found ? 0 : 1;
}

int cmd_for_each_ref(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{0, "format", Arg::Required}, {0, "sort", Arg::Required}, {0, "count", Arg::Required},
                                         {0, "shell"}, {0, "perl"}, {0, "python"}, {0, "tcl"}, {0, "points-at", Arg::Required},
                                         {0, "merged", Arg::Optional}, {0, "no-merged", Arg::Optional}, {0, "contains", Arg::Optional},
                                         {0, "no-contains", Arg::Optional}, {'i', "ignore-case"}, {0, "exclude", Arg::Required},
                                         {0, "stdin"}, {0, "omit-empty"}, {0, "color", Arg::Optional}});
  c.open_repo(false);
  // ref-filter's use_color: --color[=<when>], else color.ui
  c.color = want_color(c, p.has("color") ? std::optional<std::string>(p.get("color")) : std::nullopt, p.negated.count("color") > 0, "ui");
  int quoting = (int)p.has("shell") + (int)p.has("perl") + (int)p.has("python") + (int)p.has("tcl");
  if (quoting > 1) die(c, "options '--shell', '--perl', '--python' and '--tcl' cannot be used together");
  RefQuote quote = p.has("shell") ? RefQuote::Shell : p.has("perl") ? RefQuote::Perl : p.has("python") ? RefQuote::Python
                   : p.has("tcl") ? RefQuote::Tcl : RefQuote::None;
  long long count = -1;
  if (p.has("count")) {
    char* end = nullptr;
    count = std::strtoll(p.get("count").c_str(), &end, 10);
    if (!end || *end || p.get("count").empty() || count < 0) usage(c, "invalid --count argument: `" + p.get("count") + "'");
  }
  RefFormat format(c, p.get("format", "%(objectname) %(objecttype)\t%(refname)"), quote);
  const bool icase = p.has("ignore-case");
  std::vector<std::string> patterns = p.pos;
  if (p.has("stdin")) {
    if (!patterns.empty()) die(c, "unknown arguments supplied with --stdin");
    for (auto& l : lines_of(read_all(c.in), '\n')) if (!l.empty()) patterns.push_back(l);
  }
  auto matches = [&](const std::string& ref, const std::vector<std::string>& pats) {
    for (auto& pat : pats) {
      std::string pp = pat;
      if (pp.find_first_of("*?[") != std::string::npos) { if (wildmatch(pp, ref, icase, true)) return true; }
      else {
        if (!pp.empty() && pp.back() == '/') pp.pop_back();
        if (ref == pp || ref.rfind(pp + "/", 0) == 0) return true;
      }
    }
    return false;
  };
  std::vector<RefRow> rows;
  for (auto& [ref, oid] : all_refs(c)) {
    if (ref.rfind("refs/", 0) != 0) continue;
    if (!patterns.empty() && !matches(ref, patterns)) continue;
    if (p.has("exclude") && matches(ref, p.all("exclude"))) continue;
    rows.push_back({ref, oid});
  }
  RefSelect sel;
  for (const char* k : {"contains", "no-contains", "merged", "no-merged", "points-at"}) {
    std::vector<std::string>* dest = std::string(k) == "contains" ? &sel.contains : std::string(k) == "no-contains" ? &sel.no_contains
                                     : std::string(k) == "merged" ? &sel.merged : std::string(k) == "no-merged" ? &sel.no_merged : &sel.points_at;
    for (auto& v : p.all(k)) dest->push_back(v);
  }
  select_refs(c, rows, sel);
  sort_refs(c, rows, p.all("sort"), icase);
  for (auto& row : rows) {
    if (count == 0) break;
    if (count > 0) --count;
    std::string line = format.expand(row);
    if (p.has("omit-empty") && line.empty()) continue;
    c.out << line << "\n";
  }
  return 0;
}

int cmd_check_ignore(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{'q', "quiet"}, {'v', "verbose"}, {'n', "non-matching"}, {0, "no-index"}, {0, "stdin"}, {'z', ""}});
  c.open_repo();
  if (p.has("verbose") || p.has("non-matching")) unsupported(c, "check-ignore -v/-n");
  std::vector<std::string> paths = p.pos;
  if (p.has("stdin")) for (auto& l : lines_of(read_all(c.in), p.has("z") ? '\0' : '\n')) paths.push_back(l);
  if (paths.empty()) die(c, "no path specified");
  std::set<std::string> tracked;
  if (!p.has("no-index")) for (auto& e : c.repo->index()) tracked.insert(e.path);
  int found = 0;
  for (auto& u : paths) {
    std::string rp = c.to_repo_path(u);
    if (tracked.count(rp)) continue;
    auto st = c.host().stat(join(c.top, rp));
    bool dir = st.ok() && st->is_dir;
    if (c.repo->is_ignored(rp, dir)) {
      ++found;
      if (!p.has("quiet")) c.out << u << (p.has("z") ? '\0' : '\n');
    }
  }
  return found ? 0 : 1;
}

int cmd_var(Ctx& c) {
  if (c.args.size() != 2) usage(c, "usage: git var (-l | <variable>)");
  c.try_open_repo();
  if (!c.repo) c.load_config();
  std::string v = c.args[1];
  if (v == "GIT_AUTHOR_IDENT") c.out << c.ident(false) << "\n";
  else if (v == "GIT_COMMITTER_IDENT") c.out << c.ident(true) << "\n";
  else if (v == "GIT_EDITOR") {
    std::string e = c.getenv("GIT_EDITOR");
    if (e.empty()) e = c.config.get("core.editor").value_or("");
    if (e.empty()) e = c.getenv("VISUAL");
    if (e.empty()) e = c.getenv("EDITOR");
    if (e.empty()) e = "vi";
    c.out << e << "\n";
  } else if (v == "GIT_PAGER") c.out << "cat\n";
  else if (v == "GIT_DEFAULT_BRANCH") c.out << c.config.get("init.defaultbranch").value_or("master") << "\n";
  else usage(c, "usage: git var (-l | <variable>)");
  return 0;
}

int cmd_fsck(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{0, "full"}, {0, "strict"}, {0, "no-dangling"}, {0, "unreachable"}, {0, "connectivity-only"},
                                         {0, "name-objects"}, {0, "progress"}, {0, "lost-found"}, {0, "root"}, {0, "tags"}, {0, "cache"}});
  c.open_repo(false);
  if (p.has("unreachable") || p.has("lost-found") || p.has("root") || p.has("tags")) unsupported(c, "fsck --unreachable/--lost-found/--root/--tags");
  auto r = c.repo->fsck();
  if (!r.ok()) die(c, r.error().message);
  for (auto& b : r->broken) c.err << "error: " << b << "\n";
  // every ref must name an object this repository has
  int bad = (int)r->broken.size();
  for (auto& [ref, oid] : all_refs(c))
    if (!c.repo->read_object(oid).ok()) { c.err << "error: " << ref << ": invalid sha1 pointer " << oid << "\n"; ++bad; }
  return bad ? 1 : 0;
}

namespace {
// A path as converge writes it into alternates and objects/info/borrowers (no empty or "." segments, no trailing '/'),
// so the two lists compare equal for the same directory.
std::string store_path(const std::string& p) {
  std::string out;
  for (size_t i = 0; i <= p.size();) {
    size_t j = p.find('/', i);
    if (j == std::string::npos) j = p.size();
    const std::string seg = p.substr(i, j - i);
    if (!seg.empty() && seg != ".") out += (out.empty() && (p.empty() || p[0] != '/') ? "" : "/") + seg;
    i = j + 1;
  }
  return out.empty() && !p.empty() && p[0] == '/' ? "/" : out;
}

std::vector<std::string> path_lines(hal::Host& host, const std::string& file) {
  std::vector<std::string> out;
  auto raw = host.read_file(file);
  if (!raw.ok()) return out;
  std::istringstream in(std::string(reinterpret_cast<const char*>(raw->data()), raw->size()));
  for (std::string line; std::getline(in, line);) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (!line.empty() && line[0] != '#') out.push_back(line);
  }
  return out;
}

// The repositories that read this one's objects through their alternates and still do. git records only the
// borrower's side, so a repack -d here would delete the loose objects and packs their history lives in; converge
// (RepoCache::borrow_objects) lists each borrower's objects directory in objects/info/borrowers for that reason.
// A listed borrower counts while its objects directory exists and its alternates still name this store (the same
// test as RepoCache::live_borrowers; the list is only read here, the engine cleans it).
std::vector<std::string> live_borrowers(Ctx& c) {
  hal::Host& h = c.host();
  const std::string store = store_path(c.repo->common_dir() + "/objects");
  std::vector<std::string> live;
  for (const auto& line : path_lines(h, store + "/info/borrowers")) {
    const std::string b = store_path(line);
    if (std::find(live.begin(), live.end(), b) != live.end() || !h.exists(b)) continue;
    // (a store opened by a relative path cannot be matched against the absolute ones converge writes: listed is enough)
    if (store.empty() || store[0] != '/') { live.push_back(b); continue; }
    for (const auto& alt : path_lines(h, b + "/info/alternates")) {
      // git reads a relative alternate against the borrower's objects directory
      if (store_path(alt[0] == '/' ? alt : b + "/" + alt) == store) { live.push_back(b); break; }
    }
  }
  return live;
}

// -d (or gc) on a store others borrow from: the new pack is still written, but nothing it replaces goes.
bool keep_old_for_borrowers(Ctx& c, const char* what) {
  const auto live = live_borrowers(c);
  if (live.empty()) return false;
  c.err << "warning: " << what << ": " << live.size() << " repositor" << (live.size() == 1 ? "y reads" : "ies read")
        << " these objects through alternates (objects/info/borrowers, e.g. " << live.front()
        << "): the old packs and loose objects are kept\n";
  return true;
}
}  // namespace

// repack: this repository's objects into one pack (see git_repack). -a every object, -d remove what the new pack
// replaces; the options that only tune how hard git looks for deltas are accepted and change nothing here.
int cmd_repack(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{'a', ""}, {'A', ""}, {'d', ""}, {'q', "quiet"}, {'f', ""}, {'F', ""}, {'l', "local"}, {'n', ""},
                                         {0, "window", Arg::Required}, {0, "depth", Arg::Required}, {0, "threads", Arg::Required},
                                         {0, "window-memory", Arg::Required}, {0, "max-pack-size", Arg::Required},
                                         {'b', "write-bitmap-index"}, {0, "no-write-bitmap-index"}, {'k', "keep-unreachable"},
                                         {0, "blocks"}});
  if (p.has("max-pack-size")) unsupported(c, "repack --max-pack-size");
  c.open_repo(false);
  // aios's columnar block storage instead of a pack (git blocks)
  if (p.has("blocks")) return build_blocks(c, p.has("d") && !keep_old_for_borrowers(c, "repack -d"), p.has("quiet"));
  auto r = git_repack(*c.repo, p.has("a") || p.has("A"), p.has("d") && !keep_old_for_borrowers(c, "repack -d"));
  if (!r.ok()) die(c, "repack failed: " + r.error().message);
  return 0;
}

// ---------------------------------------------------------------------------
// reflog: the .git/logs/* files read and rewritten directly (oldest line first, one per ref update).
// ---------------------------------------------------------------------------
namespace {
const int64_t kNever = 0;                                       // no entry is older than this
const int64_t kAll = std::numeric_limits<int64_t>::max();      // every entry is older than this

struct ReflogLine {
  std::string old_oid, new_oid, text;   // text: the whole line without its newline
  std::string msg;                      // what follows the tab
  int64_t when = 0;
  bool parsed = false;
};

// The directory that holds a HEAD and its log, "" when `ref` is not a HEAD. "HEAD" is this worktree's; the others
// are named as git names them from here: "main-worktree/HEAD" (seen from a linked worktree) and "worktrees/<id>/HEAD".
std::string head_dir_of(Ctx& c, const std::string& ref) {
  if (ref == "HEAD") return c.repo->git_dir();
  if (ref == "main-worktree/HEAD") return c.repo->common_dir();
  if (ref.size() > 15 && ref.rfind("worktrees/", 0) == 0 && ref.find('/', 10) == ref.size() - 5 &&
      ref.compare(ref.size() - 5, 5, "/HEAD") == 0)
    return c.repo->common_dir() + "/worktrees/" + ref.substr(10, ref.size() - 15);
  return "";
}

std::string reflog_path(Ctx& c, const std::string& ref) {
  std::string head_dir = head_dir_of(c, ref);
  return head_dir.empty() ? c.repo->common_dir() + "/logs/" + ref : head_dir + "/logs/HEAD";
}

std::vector<ReflogLine> read_reflog(Ctx& c, const std::string& ref) {
  std::vector<ReflogLine> out;
  auto r = c.host().read_file(reflog_path(c, ref));
  if (!r.ok()) return out;
  std::istringstream ss(std::string((const char*)r->data(), r->size()));
  std::string line;
  while (std::getline(ss, line)) {
    if (line.empty()) continue;
    ReflogLine e;
    e.text = line;
    size_t tab = line.find('\t');
    std::string head = line.substr(0, tab);
    e.msg = tab == std::string::npos ? "" : line.substr(tab + 1);
    size_t gt = head.rfind('>');
    if (head.size() >= 82 && head[40] == ' ' && head[81] == ' ' && gt != std::string::npos) {
      e.old_oid = head.substr(0, 40);
      e.new_oid = head.substr(41, 40);
      e.when = std::atoll(head.c_str() + gt + 1);
      e.parsed = true;
    }
    out.push_back(e);
  }
  return out;
}

// git's refusal when another git holds the log's lock (<log>.lock). A lock left by another git, running or crashed,
// means its owner may be rewriting the log too, so the rewrite is refused instead of racing it.
void say_reflog_locked(Ctx& c, const std::string& ref, const std::string& lock) {
  c.err << "error: cannot lock ref '" << ref << "': Unable to create '" << lock << "': File exists.\n\n"
        << "Another git process seems to be running in this repository, e.g.\n"
           "an editor opened by 'git commit'. Please make sure all processes\n"
           "are terminated then try again. If it still fails, a git process\n"
           "may have crashed in this repository earlier:\n"
           "remove the file manually to continue.\n";
}

// Whether someone else holds the log's lock right now, saying so as git does. Only a cheap early refusal, so that a
// locked log is reported before any work is done for it: write_reflog takes the lock itself, atomically.
bool reflog_locked(Ctx& c, const std::string& ref) {
  std::string lock = reflog_path(c, ref) + ".lock";
  if (!c.host().exists(lock)) return false;
  say_reflog_locked(c, ref, lock);
  return true;
}

// The new log is written as <log>.lock and then renamed over the log, as git does: a reader sees the old log or
// the new one, never a half-written one. The lock is taken by creating <log>.lock exclusively, holding the new log:
// of two writers racing for it exactly one creates it, and the other is refused as git refuses it, where checking
// first and writing after would let both through, each renaming its own log over the other's.
// Returns false (having said why) when the lock is already taken.
bool write_reflog(Ctx& c, const std::string& ref, const std::vector<ReflogLine>& lines) {
  std::string text;
  for (auto& e : lines) text += e.text + "\n";
  // an emptied reflog stays as an empty file, as git leaves it
  const std::string path = reflog_path(c, ref), lock = path + ".lock";
  auto w = c.host().create_exclusive(lock, ByteSpan((const uint8_t*)text.data(), text.size()));
  if (!w.ok()) {
    if (w.error().status == Status::Conflict) { say_reflog_locked(c, ref, lock); return false; }
    // the lock is not ours (the host removes a half-written one itself), so it is not removed here
    die(c, "cannot write reflog for '" + ref + "': " + w.error().message);
  }
  auto r = c.host().rename_file(lock, path);
  if (!r.ok()) {
    (void)c.host().remove(lock);
    die(c, "cannot write reflog for '" + ref + "': " + r.error().message);
  }
  return true;
}

// The ref a name means for reflog purposes: the first of git's dwim forms that has a log (dwim_log).
std::string reflog_ref(Ctx& c, const std::string& name) {
  if (name.empty() || name == "HEAD" || name == "@") return "HEAD";
  // another worktree's HEAD, named as `reflog list --all` would name it
  if (!head_dir_of(c, name).empty()) return c.host().exists(reflog_path(c, name)) ? name : "";
  for (auto& f : {name, "refs/" + name, "refs/tags/" + name, "refs/heads/" + name, "refs/remotes/" + name,
                  "refs/remotes/" + name + "/HEAD"})
    if ((f == name ? f.rfind("refs/", 0) == 0 : true) && c.host().exists(reflog_path(c, f))) return f;
  return "";
}

bool is_symref(Ctx& c, const std::string& ref) {
  std::string head_dir = head_dir_of(c, ref);
  auto r = c.host().read_file(head_dir.empty() ? c.repo->common_dir() + "/" + ref : head_dir + "/HEAD");
  return r.ok() && r->size() >= 5 && std::string((const char*)r->data(), 5) == "ref: ";
}

// Every ref with a reflog: HEAD (this worktree's) and logs/refs/**, sorted as git lists them, then with
// `other_worktrees` the other worktrees' HEADs.
std::vector<std::string> all_reflogs(Ctx& c, bool other_worktrees) {
  std::vector<std::string> out;
  if (c.host().exists(reflog_path(c, "HEAD"))) out.push_back("HEAD");
  std::function<void(const std::string&)> walk = [&](const std::string& rel) {
    auto names = c.host().list_dir(c.repo->common_dir() + "/logs/" + rel);
    if (!names.ok()) return;
    for (auto& n : *names) {
      if (n == "." || n == "..") continue;
      std::string sub = rel + "/" + n;
      auto st = c.host().stat(c.repo->common_dir() + "/logs/" + sub);
      if (!st.ok()) continue;
      if (st->is_dir) walk(sub);
      else out.push_back(sub);
    }
  };
  walk("refs");
  std::sort(out.begin() + (out.empty() || out[0] != "HEAD" ? 0 : 1), out.end());
  if (!other_worktrees) return out;
  // The other worktrees' HEADs come after, as git's --all reaches them: each keeps its log beside its HEAD
  // (worktrees/<id>/logs/HEAD; the main worktree's in logs/HEAD), which the walk of logs/refs never sees.
  const std::string here = normalize_path(slashes(c.repo->git_dir()));
  const std::string common = c.repo->common_dir();
  if (normalize_path(slashes(common)) != here && c.host().exists(reflog_path(c, "main-worktree/HEAD")))
    out.push_back("main-worktree/HEAD");
  std::vector<std::string> ids;
  if (auto names = c.host().list_dir(common + "/worktrees"); names.ok())
    for (auto& id : *names)
      if (id != "." && id != ".." && normalize_path(slashes(common + "/worktrees/" + id)) != here) ids.push_back(id);
  std::sort(ids.begin(), ids.end());
  for (auto& id : ids)
    if (c.host().exists(reflog_path(c, "worktrees/" + id + "/HEAD"))) out.push_back("worktrees/" + id + "/HEAD");
  return out;
}

// N calendar months before `now`, as git's approxidate counts "N.months.ago": the same day of the month and time
// of day in local time, a day the month lacks running on into the next (31 March less a month is 3 March).
int64_t months_before(int64_t now, int64_t n) {
  std::time_t t = (std::time_t)now;
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  const int64_t m = (int64_t)tm.tm_mon - n;
  tm.tm_year += (int)((m >= 0 ? m : m - 11) / 12);
  tm.tm_mon = (int)(((m % 12) + 12) % 12);
  tm.tm_isdst = -1;
  return (int64_t)std::mktime(&tm);
}

// An expiry time as git's --expire takes it: "never"/"false" (keep everything), "now"/"all" (expire everything),
// "<n>.<unit>[.ago]" with '.' or ' ' between the words, or an absolute date. Returns false when it is none of those.
bool parse_expiry(const std::string& in, int64_t& out) {
  std::string s;
  for (char ch : in) s += (char)std::tolower((unsigned char)ch);
  if (s == "never" || s == "false") { out = kNever; return true; }
  if (s == "now" || s == "all") { out = kAll; return true; }
  const int64_t now = now_seconds();
  std::vector<std::string> w;
  {
    std::string cur;
    for (char ch : s) { if (ch == '.' || ch == ' ' || ch == '_') { if (!cur.empty()) w.push_back(cur); cur.clear(); } else cur += ch; }
    if (!cur.empty()) w.push_back(cur);
  }
  if (!w.empty() && w.back() == "ago") w.pop_back();
  if (w.size() == 1 && w[0] == "yesterday") { out = now - 86400; return true; }
  if (w.size() == 2 && !w[0].empty() && std::all_of(w[0].begin(), w[0].end(), [](char ch) { return std::isdigit((unsigned char)ch); })) {
    std::string u = w[1];
    if (u.size() > 1 && u.back() == 's') u.pop_back();
    static const std::map<std::string, int64_t> units = {
        {"second", 1}, {"sec", 1}, {"minute", 60}, {"min", 60}, {"hour", 3600}, {"day", 86400},
        {"week", 7 * 86400}};
    const int64_t n = std::atoll(w[0].c_str());
    // months and years are the calendar's, not a fixed number of days
    if (u == "month") { out = months_before(now, n); return true; }
    if (u == "year") { out = months_before(now, n * 12); return true; }
    auto it = units.find(u);
    if (it != units.end()) { out = now - n * it->second; return true; }
  }
  std::string tz;
  int64_t t = 0;
  if (parse_date(in, t, tz)) { out = t; return true; }
  return false;
}

int64_t must_parse_expiry(Ctx& c, const std::string& v, const std::string& what) {
  int64_t t = 0;
  if (!parse_expiry(v, t)) die(c, "invalid value for '" + what + "': '" + v + "'");
  return t;
}

struct ExpireOptions {
  std::optional<int64_t> expire, expire_unreachable;   // set when given on the command line
  bool dry_run = false, verbose = false, rewrite = false, updateref = false, stale_fix = false;
};

// The two cut-offs for one ref: the command line, else the first gc.<pattern>.reflogExpire[Unreachable] whose pattern
// matches the ref, else (refs/stash only) never, else gc.reflogExpire[Unreachable] (90 / 30 days by default).
std::pair<int64_t, int64_t> expiry_for(Ctx& c, const std::string& ref, const ExpireOptions& o) {
  std::optional<int64_t> total = o.expire, unreach = o.expire_unreachable;
  if (!total || !unreach) {
    std::vector<std::string> order;
    std::map<std::string, std::pair<std::optional<std::string>, std::optional<std::string>>> by_pattern;
    for (auto& e : c.config.entries()) {
      std::string k = Config::canon(e.key);
      if (k.rfind("gc.", 0) != 0) continue;
      size_t dot = k.rfind('.');
      if (dot <= 3) continue;   // gc.<name> without a pattern
      std::string pat = k.substr(3, dot - 3), name = k.substr(dot + 1);
      if (name != "reflogexpire" && name != "reflogexpireunreachable") continue;
      if (!by_pattern.count(pat)) order.push_back(pat);
      (name == "reflogexpire" ? by_pattern[pat].first : by_pattern[pat].second) = e.value;
    }
    bool configured = false;
    for (auto& pat : order) {
      if (!wildmatch(pat, ref, false, false)) continue;
      auto& [t, u] = by_pattern[pat];
      if (!total && t) total = must_parse_expiry(c, *t, "gc." + pat + ".reflogExpire");
      if (!unreach && u) unreach = must_parse_expiry(c, *u, "gc." + pat + ".reflogExpireUnreachable");
      configured = true;
      break;
    }
    // git keeps the stash's reflog (the stash list itself) unless told otherwise
    if (ref == "refs/stash" && !configured) {
      if (!total) total = kNever;
      if (!unreach) unreach = kNever;
    }
    if (!total) total = must_parse_expiry(c, c.config.get("gc.reflogexpire").value_or("90.days.ago"), "gc.reflogExpire");
    if (!unreach)
      unreach = must_parse_expiry(c, c.config.get("gc.reflogexpireunreachable").value_or("30.days.ago"), "gc.reflogExpireUnreachable");
  }
  return {*total, *unreach};
}

// Prunes one reflog as git's should_expire_reflog_ent does. `delete_nth` (from the newest, 0-based, counting the
// entries `reflog show` numbers) removes that one entry instead of expiring by age. Returns false (having said why)
// when another git holds the log's lock.
bool expire_one(Ctx& c, const std::string& ref, const ExpireOptions& o, std::optional<size_t> delete_nth = std::nullopt) {
  auto lines = read_reflog(c, ref);
  // a dry run writes nothing, so it needs no lock; a real one is refused up front even when nothing would go
  if (!o.dry_run && reflog_locked(c, ref)) return false;
  const size_t parsed_total = (size_t)std::count_if(lines.begin(), lines.end(), [](const ReflogLine& e) { return e.parsed; });
  size_t parsed_seen = 0;
  int64_t expire_total = kNever, expire_unreach = kNever;
  if (!delete_nth) std::tie(expire_total, expire_unreach) = expiry_for(c, ref, o);
  const std::string zero(40, '0');

  // Reachability as git's reflog_expiry_prepare judges it. A HEAD, this worktree's or another's, may have been on
  // any branch, so its entries are judged from every ref's tip and not from the HEAD itself (git's UE_HEAD: a
  // detached HEAD's own commits count only if a ref reaches them). Any other ref is judged from its own tip, and a
  // ref with no tip to reach anything from loses every old entry. The walk is done once, only when an entry needs it.
  const bool is_head = !head_dir_of(c, ref).empty();
  std::vector<std::string> tips;
  if (is_head) {
    for (auto& [name, o2] : all_refs(c)) { (void)name; std::string t = peel_to(c, o2, "commit"); if (!t.empty()) tips.push_back(t); }
  } else if (auto tip = read_ref(c, ref)) {
    std::string t = peel_to(c, *tip, "commit");
    if (!t.empty()) tips.push_back(t);
  }
  const bool always = !is_head && tips.empty();
  bool walked = false;
  std::set<std::string> reachable;
  auto unreachable = [&](const std::string& oid) {
    if (oid == zero) return false;
    if (always) return true;
    std::string cm = peel_to(c, oid, "commit");
    if (cm.empty()) return false;   // not a commit (or not here): git keeps the entry rather than guess
    if (!walked) {
      walked = true;
      std::vector<std::string> todo = tips;
      while (!todo.empty()) {
        std::string x = todo.back();
        todo.pop_back();
        if (!reachable.insert(x).second) continue;
        auto cmt = c.repo->read_commit(x);
        if (cmt.ok()) for (auto& p : cmt->parents) if (!reachable.count(p)) todo.push_back(p);
      }
    }
    return !reachable.count(cm);
  };
  auto missing = [&](const std::string& oid) { return oid != zero && !c.repo->read_object(oid).ok(); };

  std::vector<ReflogLine> kept;
  size_t pruned = 0;
  std::string last_kept = zero;
  for (size_t i = 0; i < lines.size(); ++i) {
    ReflogLine e = lines[i];
    bool prune = false;
    if (e.parsed) {
      if (delete_nth) prune = parsed_total - 1 - parsed_seen == *delete_nth;
      else if (o.stale_fix && (missing(e.old_oid) || missing(e.new_oid))) prune = true;
      else if (e.when < expire_total) prune = true;
      else if (e.when < expire_unreach) prune = unreachable(e.old_oid) || unreachable(e.new_oid);
      ++parsed_seen;
    }
    if (prune) {
      ++pruned;
      if (o.verbose) c.out << (o.dry_run ? "would prune " : "prune ") << e.msg << "\n";
      continue;
    }
    if (o.verbose) c.out << "keep " << e.msg << "\n";
    if (e.parsed && o.rewrite && e.old_oid != last_kept) {
      // the entry now follows the one kept before it: its old value is what that one set
      e.old_oid = last_kept;
      e.text = e.old_oid + e.text.substr(40);
    }
    if (e.parsed) last_kept = e.new_oid;
    kept.push_back(e);
  }
  if (o.dry_run || !pruned) return true;
  if (!write_reflog(c, ref, kept)) return false;
  if (o.updateref && last_kept != zero && !is_symref(c, ref)) {
    std::string head_dir = head_dir_of(c, ref);
    if (ref != "HEAD" && !head_dir.empty()) {
      // another worktree's detached HEAD: its file is not one the repository's ref store writes
      std::string text = last_kept + "\n";
      auto w = c.host().write_file(head_dir + "/HEAD", ByteSpan((const uint8_t*)text.data(), text.size()));
      if (!w.ok()) die(c, w.error().message);
    } else {
      auto r = c.repo->update_ref(ref, last_kept);
      if (!r.ok()) die(c, r.error().message);
    }
  }
  return true;
}

// Prunes every reflog, every worktree's HEAD included, as `git reflog expire --all` would (with the configured
// cut-offs): what gc does first. A log another git has locked fails it as the reflog step fails git's gc.
void expire_all_reflogs(Ctx& c, const ExpireOptions& o) {
  bool ok = true;
  for (auto& ref : all_reflogs(c, true)) ok = expire_one(c, ref, o) && ok;
  if (!ok) die(c, "failed to run reflog");
}
}  // namespace

// reflog [show|list|exists|expire|delete]: show lists a ref's entries newest first as <commit> <ref>@{n}: <message>.
// expire prunes by age (--expire, default gc.reflogExpire or 90 days) and, for entries that the ref's tip (for a HEAD,
// any ref's) no longer reaches, by --expire-unreachable (gc.reflogExpireUnreachable, 30 days); delete removes ref@{n},
// or ref@{<date>}: the entry in effect at that date. Both take --dry-run, --verbose, --rewrite and --updateref. expire --all covers
// logs/refs/** and every worktree's HEAD (only this one's with --single-worktree); a log another git has locked
// (<log>.lock) is left alone with an error, as git leaves it.
int cmd_reflog(Ctx& c) {
  std::string sub = c.args.size() > 1 ? c.args[1] : "show";
  const bool explicit_sub = sub == "show" || sub == "list" || sub == "exists" || sub == "expire" || sub == "delete";
  if (!explicit_sub) sub = "show";
  const size_t start = explicit_sub ? 2 : 1;
  c.open_repo(false);
  if (sub == "expire" || sub == "delete") {
    auto p = parse_options(c, c.args, start, {{0, "expire", Arg::Required}, {0, "expire-unreachable", Arg::Required},
                                              {'n', "dry-run"}, {0, "verbose"}, {0, "rewrite"}, {0, "updateref"},
                                              {0, "stale-fix"}, {0, "all"}, {0, "single-worktree"}});
    ExpireOptions o;
    o.dry_run = p.has("dry-run");
    o.verbose = p.has("verbose");
    o.rewrite = p.has("rewrite");
    o.updateref = p.has("updateref");
    int status = 0;
    if (sub == "delete") {
      if (p.pos.empty()) usage(c, "no reflog specified to delete");
      for (auto& spec : p.pos) {
        size_t at = spec.find("@{");
        if (at == std::string::npos || spec.back() != '}') { c.err << "error: not a reflog: " << spec << "\n"; status = 1; continue; }
        std::string n = spec.substr(at + 2, spec.size() - at - 3);
        std::string ref = reflog_ref(c, spec.substr(0, at));
        if (ref.empty()) { c.err << "error: " << spec << " points nowhere!\n"; status = 1; continue; }
        size_t nth = 0;
        if (!n.empty() && std::all_of(n.begin(), n.end(), [](char ch) { return std::isdigit((unsigned char)ch); })) {
          nth = (size_t)std::strtoull(n.c_str(), nullptr, 10);
        } else {
          // A date means the entry in effect then: the newest one older than it, which is what git's count of the
          // entries before the date picks out. With none that old there is nothing to delete.
          int64_t when = 0;
          if (!parse_expiry(n, when)) unsupported(c, "reflog date '" + n + "' in '" + spec + "'");
          auto lines = read_reflog(c, ref);
          size_t total = 0, before = 0;
          for (auto& e : lines)
            if (e.parsed) { ++total; if (e.when < when) ++before; }
          if (!before) continue;
          nth = total - before;
        }
        if (!expire_one(c, ref, o, nth)) status = 1;
      }
      return status;
    }
    if (p.has("expire")) o.expire = must_parse_expiry(c, p.get("expire"), "--expire");
    if (p.has("expire-unreachable")) o.expire_unreachable = must_parse_expiry(c, p.get("expire-unreachable"), "--expire-unreachable");
    o.stale_fix = p.has("stale-fix");
    std::vector<std::string> refs;
    if (p.has("all")) refs = all_reflogs(c, !p.has("single-worktree"));
    for (auto& name : p.pos) {
      std::string ref = reflog_ref(c, name);
      if (ref.empty()) { c.err << "error: reflog could not be found: '" << name << "'\n"; status = 1; continue; }
      refs.push_back(ref);
    }
    for (auto& ref : refs)
      if (!expire_one(c, ref, o)) status = 1;
    return status;
  }
  if (sub == "list") {
    parse_options(c, c.args, start, {});
    for (auto& ref : all_reflogs(c, false)) c.out << ref << "\n";   // git lists this worktree's logs only
    return 0;
  }
  if (sub == "exists") {
    auto p = parse_options(c, c.args, start, {});
    if (p.pos.size() != 1) usage(c, "usage: git reflog exists <ref>");
    const std::string& r = p.pos[0];
    return c.host().exists(reflog_path(c, r == "@" ? "HEAD" : r)) ? 0 : 1;
  }
  // show: the plain listing, or one --format/--pretty=format: line per entry
  auto p = parse_options(c, c.args, start, {{'n', "max-count", Arg::Required}, {0, "format", Arg::Required},
                                            {0, "pretty", Arg::Required}});
  std::string fmt = p.has("format") ? reflog_user_format("--format=" + p.get("format"))
                  : p.has("pretty") ? reflog_user_format("--pretty=" + p.get("pretty")) : "";
  if (p.has("pretty") && fmt.empty()) unsupported(c, "reflog --pretty=" + p.get("pretty"));
  if (p.pos.size() > 1) unsupported(c, "reflog show with more than one ref");
  std::string name = p.pos.empty() ? "HEAD" : p.pos[0];
  std::string ref = reflog_ref(c, name);
  if (ref.empty()) {
    if (!dwim_ref(c, name).empty()) return 0;   // a ref with no reflog shows nothing
    die(c, "ambiguous argument '" + name + "': unknown revision or path not in the working tree.\n"
           "Use '--' to separate paths from revisions, like this:\n'git <command> [<revision>...] -- [<file>...]'");
  }
  long max = p.has("max-count") ? std::atol(p.get("max-count").c_str()) : -1;
  auto lines = read_reflog(c, ref);
  long n = 0;
  for (size_t i = lines.size(); i-- > 0 && (max < 0 || n < max);) {
    if (!lines[i].parsed) continue;
    std::string gd = name + "@{" + std::to_string(n) + "}";
    if (fmt.empty()) {
      // log's oneline form: the id in the commit color
      if (c.color) c.out << diff_palette(c).commit << c.abbrev(lines[i].new_oid) << kColorReset;
      else c.out << c.abbrev(lines[i].new_oid);
      c.out << " " << gd << ": " << lines[i].msg << "\n";
    }
    else c.out << format_reflog_entry(c, fmt, lines[i].new_oid, gd, lines[i].msg) << "\n";
    ++n;
  }
  return 0;
}

// gc: reflogs expired (gc.reflogExpire / gc.reflogExpireUnreachable, as `reflog expire --all`), then everything into
// one pack and the files it replaces removed (repack -a -d). No object is pruned: one no ref reaches is packed with the
// rest, so gc never loses an object. --auto does nothing (there is no threshold to cross). A store other repositories
// borrow from (objects/info/borrowers) keeps what the pack replaces, as repack -d does.
int cmd_gc(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{'q', "quiet"}, {0, "auto"}, {0, "aggressive"}, {0, "prune", Arg::Optional}, {0, "no-prune"},
                                         {0, "force"}, {0, "keep-largest-pack"}, {0, "cruft"}, {0, "no-cruft"}});
  c.open_repo(false);
  if (p.has("auto")) return 0;
  // git gc starts with `reflog expire --all`, which is what keeps the logs from growing without bound
  expire_all_reflogs(c, ExpireOptions{});
  auto r = git_repack(*c.repo, true, !keep_old_for_borrowers(c, "gc"));
  if (!r.ok()) die(c, "gc failed: " + r.error().message);
  return 0;
}

}  // namespace aios_git
