// aios-unit-diff: what changed between two versions of a source file, unit by unit -- the operations the agent stream
// (scripts/stream) records instead of text diffs.
//
//   aios-unit-diff <path> <old-file> <new-file>      (old-file may be /dev/null: everything is added)
//
// <path> picks the grammar (its extension, as git_structural_language reads it). Prints one JSON object:
//   {"language":"cpp","units":N,"changes":[{"scope":..,"kind":..,"how":"+|-|~","line":L,"end_line":E,
//     "before":"<old text>","after":"<new text>"}...]}
// A function or method is one unit, body and all (a statement inside it is the function's change, not a unit of its
// own); a class, struct or namespace is a unit by its own text (its members are units of their own), so a method
// changed in a class changes the method, not the class. Text outside every unit (includes, top-level statements) is
// scope "". A file no grammar reads, or that does not parse, is one unit: the whole file (scope "", kind "file").
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "aios/userland/git_structural.hpp"

using namespace aios::userland;

namespace {

std::string slurp(const char* p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream s;
  s << f.rdbuf();
  return s.str();
}

std::string esc(std::string_view s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += static_cast<char>(c);
    }
  }
  return o;
}

bool atomic(const std::string& kind) {
  for (const char* k : {"function", "method", "constructor", "lambda", "closure"})
    if (kind.find(k) != std::string::npos) return true;
  return false;
}

struct Unit {
  const GitSyntaxUnit* u;
  bool whole;            // atomic: compared and reported with everything under it
  std::string text;
};

// The units at definition granularity: everything down to the first function-like unit on each path.
std::map<std::string, Unit> units_of(const GitSyntaxOutline& o, const std::string& text) {
  std::map<std::string, Unit> out;
  std::vector<std::string> stop;   // scopes of atomic units: anything inside them belongs to them
  for (const auto& u : o.units) {
    while (!stop.empty() && !git_scopes_overlap(stop.back(), u.scope)) stop.pop_back();
    if (!stop.empty()) continue;
    if (git_syntax_is_import(u.kind)) continue;   // the file's text: no one owns an include
    const bool whole = atomic(u.kind);
    out.emplace(u.scope, Unit{&u, whole, text.substr(u.start, u.end - u.start)});
    if (whole) stop.push_back(u.scope);
  }
  return out;
}

void change(std::ostringstream& js, bool& first, const std::string& scope, const std::string& kind, char how, uint32_t line,
            uint32_t end_line, std::string_view before, std::string_view after) {
  js << (first ? "" : ",") << "{\"scope\":\"" << esc(scope) << "\",\"kind\":\"" << esc(kind) << "\",\"how\":\"" << how
     << "\",\"line\":" << line << ",\"end_line\":" << end_line << ",\"before\":\"" << esc(before) << "\",\"after\":\""
     << esc(after) << "\"}";
  first = false;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "usage: aios-unit-diff <path> <old-file> <new-file>\n";
    return 2;
  }
  const std::string path = argv[1];
  const std::string a = slurp(argv[2]), b = slurp(argv[3]);
  const std::string lang = git_structural_language(path);
  std::optional<GitSyntaxOutline> oa, ob;
  // Outlined even where tree-sitter finds a local error (a macro it cannot read in a 2 MB file is common): an
  // operation names the units around the edit, and a misread region only makes its units coarser, never wrong text.
  if (!lang.empty()) {
    oa = git_structural_outline_lenient(path, a);
    ob = git_structural_outline_lenient(path, b, a);
  }
  std::ostringstream js;
  bool first = true;
  if (!oa || !ob) {
    if (a != b) change(js, first, "", "file", a.empty() ? '+' : b.empty() ? '-' : '~', 1, 0, a, b);
    std::cout << "{\"language\":\"" << esc(lang) << "\",\"units\":0,\"changes\":[" << js.str() << "]}\n";
    return 0;
  }
  const auto ua = units_of(*oa, a), ub = units_of(*ob, b);
  for (const auto& [scope, x] : ua) {
    auto it = ub.find(scope);
    if (it == ub.end()) { change(js, first, scope, x.u->kind, '-', x.u->line, x.u->end_line, x.text, ""); continue; }
    const auto& y = it->second;
    const bool same = x.whole ? x.u->digest == y.u->digest : x.u->shell == y.u->shell;
    if (!same) change(js, first, scope, y.u->kind, '~', y.u->line, y.u->end_line, x.text, y.text);
  }
  for (const auto& [scope, y] : ub)
    if (!ua.count(scope)) change(js, first, scope, y.u->kind, '+', y.u->line, y.u->end_line, "", y.text);
  if (oa->shell != ob->shell) change(js, first, "", "file", '~', 1, 0, "", "");
  std::cout << "{\"language\":\"" << esc(lang) << "\",\"units\":" << ub.size() << ",\"changes\":[" << js.str() << "]}\n";
  return 0;
}
