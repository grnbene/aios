// git outline [REV:]PATH...: a file's named units (functions, classes, methods, keys, tables...) as the
// language-aware merge sees them, one per line: its lines, its kind and its scope. REV:PATH reads the file at a
// revision (from the block files' outlines when they keep it); a bare PATH reads the work tree.
//
//   git outline --as PATH FILE              FILE (anywhere) read as a file at PATH (whose name picks the grammar)
//   git outline --changes OLD NEW --as PATH the scopes a change from OLD to NEW touched, one per line: "+ scope"
//                                           added, "- scope" removed, "~ scope" changed in itself, "~ " for text
//                                           outside every unit (imports included). Exit 3 when NEW does not parse, 4
//                                           when OLD does not.
// The last two need no repository: aios' agent runtime asks them whether a write keeps to a scope claim.
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "aios/userland/git_structural.hpp"
#include "cli.hpp"

namespace aios_git {

using namespace aios::userland;

namespace {
std::string read_any(Ctx& c, const std::string& file) {
  std::ifstream in(c.abs_path(file), std::ios::binary);
  if (!in) die(c, "cannot read " + file);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void print_units(Ctx& c, const GitSyntaxOutline& o) {
  for (auto& u : o.units) c.out << u.line << "-" << u.end_line << "\t" << u.kind << "\t" << u.scope << "\n";
}
}  // namespace

int cmd_outline(Ctx& c) {
  std::vector<std::string> specs;
  std::string as, old_file, new_file;
  bool changes = false;
  for (size_t i = 1; i < c.args.size(); ++i) {
    const std::string& a = c.args[i];
    if (a == "--as" && i + 1 < c.args.size()) as = c.args[++i];
    else if (a == "--changes" && i + 2 < c.args.size()) { changes = true; old_file = c.args[++i]; new_file = c.args[++i]; }
    else specs.push_back(a);
  }
  if (changes) {
    if (as.empty()) usage(c, "usage: git outline --changes <old> <new> --as <path>");
    if (git_structural_language(as).empty()) { c.err << "no grammar reads " << as << "\n"; return 2; }
    auto a = git_structural_outline(as, read_any(c, old_file));
    if (!a) { c.err << old_file << " does not parse as " << as << "\n"; return 4; }
    auto b = git_structural_outline(as, read_any(c, new_file), read_any(c, old_file));
    if (!b) { c.err << new_file << " does not parse as " << as << "\n"; return 3; }
    // imports count as the file's own text (""), as converge's lease checks count them
    std::map<std::string, std::string> kind;
    for (const auto* o : {&*a, &*b}) for (auto& u : o->units) kind[u.scope] = u.kind;
    std::set<std::string> seen;
    for (auto& ch : git_structural_scope_changes(*a, *b)) {
      std::string line = git_syntax_is_import(kind[ch.scope]) ? std::string("~ ") : std::string(1, ch.how) + " " + ch.scope;
      if (seen.insert(line).second) c.out << line << "\n";
    }
    return 0;
  }
  if (!as.empty()) {
    if (specs.size() != 1) usage(c, "usage: git outline --as <path> <file>");
    auto o = git_structural_outline(as, read_any(c, specs[0]));
    if (!o) { c.err << (git_structural_language(as).empty() ? "no grammar reads " + as : specs[0] + " does not parse") << "\n"; return 3; }
    print_units(c, *o);
    return 0;
  }
  c.open_repo(false);
  if (specs.empty()) usage(c, "usage: git outline [<rev>:]<path>...");
  for (auto& spec : specs) {
    std::optional<GitSyntaxOutline> o;
    std::string path = spec;
    size_t colon = spec.find(':');
    if (colon != std::string::npos) {
      path = spec.substr(colon + 1);
      std::string oid = must_resolve(c, spec);
      o = c.repo->outline(oid, path);
    } else {
      bool ok = false;
      std::string text = read_worktree_file(c, path, &ok);
      if (!ok) die(c, "cannot read " + path);
      o = git_structural_outline(path, text);
    }
    if (specs.size() > 1) c.out << "==> " << spec << " <==\n";
    if (!o) {
      c.out << (git_structural_language(path).empty() ? "(no grammar reads " + path + ")\n" : "(" + path + " does not parse)\n");
      continue;
    }
    print_units(c, *o);
  }
  return 0;
}

}  // namespace aios_git
