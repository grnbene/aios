// grain fmt: one layout for grain source.
//
//   grain fmt <file.gr | dir>...      rewrite the files in place
//   grain fmt --check <...>           change nothing; list the files that would
//                                     change, exit 1 if any would
//
// Each line is indented two spaces per level of open brackets - { [ ( - with
// brackets opened on one line sharing a level, a line that starts by closing
// dedented to match its opener, and a line continuing an expression (the one
// before ended in an operator) one level deeper; trailing whitespace goes,
// runs of blank lines stop at two, the file ends with one newline. Strings
// (single-line in grain) and # comments are skipped when counting brackets and
// never changed.
//
// A file is only rewritten when the formatted text compiles to exactly the
// bytecode the original did: a formatter bug can make a file look different,
// never run differently. A file that does not compile is reported and left alone.
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <system_error>

#include "tools.hpp"

namespace grain_cli {

namespace g = aios::userland::grain;
namespace fs = std::filesystem;

std::string format_source(const std::string& text) {
  std::vector<std::string> lines;
  {
    std::string cur;
    for (char c : text) {
      if (c == '\n') { lines.push_back(cur); cur.clear(); }
      else if (c != '\r') cur.push_back(c);
    }
    if (!cur.empty()) lines.push_back(cur);
  }
  // Each open bracket remembers the indent of the line that opened it. What is
  // inside sits one level deeper than the innermost opener's line - so
  // brackets opened together, `create_pane({`, indent their contents once - and
  // a line that starts by closing a bracket goes back to its opener's indent.
  std::vector<int> open_levels;
  bool continues = false;   // the previous line ended in an operator: this one hangs one level deeper
  std::string out;
  int blanks = 0;
  for (const std::string& raw : lines) {
    const size_t first = raw.find_first_not_of(" \t");
    if (first == std::string::npos) {
      if (++blanks <= 2 && !out.empty()) out += "\n";
      continue;
    }
    blanks = 0;
    std::string body = raw.substr(first);
    while (!body.empty() && (body.back() == ' ' || body.back() == '\t')) body.pop_back();

    // Closers the line starts with: it goes where the last of their openers' lines went.
    size_t i = 0;
    int closed_to = -1;
    while (i < body.size() && (body[i] == '}' || body[i] == ']' || body[i] == ')')) {
      if (!open_levels.empty()) { closed_to = open_levels.back(); open_levels.pop_back(); }
      ++i;
    }
    const int level = closed_to >= 0 ? closed_to
                                     : (open_levels.empty() ? 0 : open_levels.back() + 1) + (continues ? 1 : 0);
    out += std::string(static_cast<size_t>(level) * 2, ' ') + body + "\n";

    // The rest of the line: what it opens indents the lines after it one level past this one.
    char quote = 0;
    size_t code_end = body.size();
    for (; i < body.size(); ++i) {
      const char c = body[i];
      if (quote) {
        if (c == '\\') ++i;
        else if (c == quote) quote = 0;
        continue;
      }
      if (c == '"' || c == '\'') { quote = c; continue; }
      if (c == '#') { code_end = i; break; }
      if (c == '{' || c == '[' || c == '(') open_levels.push_back(level);
      else if ((c == '}' || c == ']' || c == ')') && !open_levels.empty()) open_levels.pop_back();
    }
    // Ends in a binary operator (comments aside): the expression goes on below.
    std::string code = body.substr(0, code_end);
    while (!code.empty() && (code.back() == ' ' || code.back() == '\t')) code.pop_back();
    auto ends_with = [&](const char* w) {
      const size_t n = std::char_traits<char>::length(w);
      if (code.size() < n || code.compare(code.size() - n, n, w) != 0) return false;
      const bool word = std::isalpha(static_cast<unsigned char>(w[0])) != 0;
      return !word || code.size() == n || !(std::isalnum(static_cast<unsigned char>(code[code.size() - n - 1])) || code[code.size() - n - 1] == '_');
    };
    continues = false;
    for (const char* op : {"+", "-", "*", "/", "%", "=", "==", "!=", "<", ">", "<=", ">=", "&", "|", "^", "<<", ">>", "and", "or", "not", "?", ":"}) {
      if (ends_with(op)) { continues = true; break; }
    }
  }
  while (out.size() >= 2 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n') out.pop_back();
  return out;
}

namespace {

bool read_all(const fs::path& p, std::string& out) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

// The same compiled program: bytecode, constants (functions compared the same
// way, recursively), arity, locals and upvalues. Line numbers may differ -
// removing blank lines moves them - and nothing else may.
bool same_program(const g::FnObj& a, const g::FnObj& b) {
  if (a.arity != b.arity || a.local_slots != b.local_slots || a.name != b.name) return false;
  if (a.upvalue_from_local != b.upvalue_from_local || a.upvalue_index != b.upvalue_index) return false;
  if (a.chunk.code != b.chunk.code || a.chunk.constants.size() != b.chunk.constants.size()) return false;
  for (size_t i = 0; i < a.chunk.constants.size(); ++i) {
    const g::Value& x = a.chunk.constants[i];
    const g::Value& y = b.chunk.constants[i];
    if (x.type != y.type) return false;
    if (x.type == g::Type::Fn) {
      const g::Obj* ox = x.obj.get();
      const g::Obj* oy = y.obj.get();
      if (!ox || !oy || ox->is_closure || oy->is_closure) return false;   // compiled constants are bare functions
      if (!same_program(*static_cast<const g::FnObj*>(ox), *static_cast<const g::FnObj*>(oy))) return false;
    } else if (!x.equals(y)) {
      return false;
    }
  }
  return true;
}

void collect(const fs::path& p, std::vector<fs::path>& out) {
  std::error_code ec;
  if (fs::is_directory(p, ec)) {
    for (fs::recursive_directory_iterator it(p, ec), end; it != end && !ec; it.increment(ec))
      if (it->is_regular_file(ec) && it->path().extension() == ".gr") out.push_back(it->path());
  } else {
    out.push_back(p);
  }
}

}  // namespace

int fmt_command(const std::vector<std::string>& args) {
  bool check = false;
  std::vector<fs::path> files;
  for (const auto& a : args) {
    if (a == "--check") check = true;
    else collect(a, files);
  }
  if (files.empty()) { std::cerr << "grain fmt [--check] <file.gr | dir>...\n"; return 64; }
  std::sort(files.begin(), files.end());
  int changed = 0, failed = 0;
  for (const auto& f : files) {
    std::string src;
    if (!read_all(f, src)) { std::cerr << f.string() << ": cannot read\n"; ++failed; continue; }
    const std::string formatted = format_source(src);
    if (formatted == src) continue;
    const auto before = g::compile_source(src, f.string());
    if (!before.ok()) {
      std::cerr << f.string() << ": not formatted - it does not compile (line " << before.errors[0].line << ": " << before.errors[0].message << ")\n";
      ++failed;
      continue;
    }
    const auto after = g::compile_source(formatted, f.string());
    if (!after.ok() || !same_program(*before.script, *after.script)) {
      std::cerr << f.string() << ": not formatted - the result would compile differently (a grain fmt bug; please report it)\n";
      ++failed;
      continue;
    }
    ++changed;
    if (check) { std::cout << f.string() << "\n"; continue; }
    std::ofstream out(f, std::ios::binary | std::ios::trunc);
    out << formatted;
    if (!out) { std::cerr << f.string() << ": cannot write\n"; ++failed; continue; }
    std::cout << "formatted " << f.string() << "\n";
  }
  if (failed) return 1;
  return check && changed ? 1 : 0;
}

}  // namespace grain_cli
