// check-attr: what gitattributes say about paths.
#include "attrs.hpp"
#include "hist_util.hpp"

namespace aios_git {

namespace {

const char* kUsage =
    "usage: git check-attr [--source <tree-ish>] [-a | --all | <attr>...] [--] <pathname>...\n"
    "   or: git check-attr --stdin [-z] [--source <tree-ish>] [-a | --all | <attr>...]\n"
    "\n"
    "    -a, --[no-]all        report all attributes set on file\n"
    "    --[no-]cached         use .gitattributes only from the index\n"
    "    --[no-]stdin          read file names from stdin\n"
    "    -z                    terminate input and output records by a NUL character\n"
    "    --[no-]source <tree-ish>\n"
    "                          which tree-ish to check attributes at\n"
    "\n";

[[noreturn]] void usage_error(Ctx& c, const std::string& msg) {
  c.err << "error: " << msg << "\n" << kUsage;
  throw Exit{129};
}

// A path as `git check-attr --stdin` reads it: C-quoted when it starts with '"'.
std::string unquote_path(const std::string& s) {
  if (s.size() < 2 || s[0] != '"' || s.back() != '"') return s;
  std::string out;
  for (size_t i = 1; i + 1 < s.size(); ++i) {
    char ch = s[i];
    if (ch != '\\' || i + 2 >= s.size() + 0) { out += ch; continue; }
    char n = s[++i];
    if (n >= '0' && n <= '7' && i + 2 < s.size()) {
      out += (char)(((n - '0') << 6) | ((s[i + 1] - '0') << 3) | (s[i + 2] - '0'));
      i += 2;
      continue;
    }
    switch (n) {
      case 'n': out += '\n'; break;
      case 't': out += '\t'; break;
      case 'a': out += '\a'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'r': out += '\r'; break;
      case 'v': out += '\v'; break;
      default: out += n; break;
    }
  }
  return out;
}

}  // namespace

int cmd_check_attr(Ctx& c) {
  std::vector<std::string> rest;
  bool all = false, cached = false, from_stdin = false, nul = false;
  std::string source;
  bool have_source = false;
  std::vector<std::string> args(c.args.begin() + 1, c.args.end());
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "--") { rest.insert(rest.end(), args.begin() + (long)i, args.end()); break; }
    if (a == "-h") { c.out << kUsage; throw Exit{129}; }
    if (a == "-a" || a == "--all") all = true;
    else if (a == "--no-all") all = false;
    else if (a == "--cached") cached = true;
    else if (a == "--no-cached") cached = false;
    else if (a == "--stdin") from_stdin = true;
    else if (a == "--no-stdin") from_stdin = false;
    else if (a == "-z") nul = true;
    else if (a == "--source") {
      if (i + 1 >= args.size()) { c.err << "error: option `source' requires a value\n" << kUsage; throw Exit{129}; }
      source = args[++i];
      have_source = true;
    } else if (a.rfind("--source=", 0) == 0) { source = a.substr(9); have_source = true; }
    else if (a == "--no-source") have_source = false;
    else if (a.size() > 1 && a[0] == '-' && a != "--") {
      bool known = true;
      for (size_t j = 1; j < a.size(); ++j) {
        if (a[j] == 'a') all = true;
        else if (a[j] == 'z') nul = true;
        else known = false;
      }
      if (!known) {
        c.err << (a[1] == '-' ? "error: unknown option `" + a.substr(2) + "'\n" : "error: unknown switch `" + std::string(1, a[1]) + "'\n") << kUsage;
        throw Exit{129};
      }
    } else rest.push_back(a);
  }
  c.open_repo(false);
  size_t doubledash = std::string::npos;
  for (size_t i = 0; i < rest.size() && doubledash == std::string::npos; ++i) if (rest[i] == "--") doubledash = i;
  std::vector<std::string> attrs, files;
  if (all) {
    if (doubledash != std::string::npos) {
      attrs.assign(rest.begin(), rest.begin() + (long)doubledash);
      files.assign(rest.begin() + (long)doubledash + 1, rest.end());
      if (!attrs.empty()) usage_error(c, "Attributes and --all both specified");
    } else {
      files = rest;
    }
  } else {
    size_t filei;
    if (doubledash == std::string::npos) {
      if (rest.empty()) usage_error(c, "No attribute specified");
      attrs.push_back(rest[0]);
      filei = 1;
    } else {
      if (doubledash < 1) usage_error(c, "No attribute specified");
      attrs.assign(rest.begin(), rest.begin() + (long)doubledash);
      filei = doubledash + 1;
    }
    files.assign(rest.begin() + (long)filei, rest.end());
  }
  if (!all) {
    for (auto& n : attrs) {
      bool ok = !n.empty() && n[0] != '-';
      for (char ch : n) if (!(ch == '-' || ch == '.' || ch == '_' || std::isalnum((unsigned char)ch))) ok = false;
      if (!ok) { c.err << "error: " << n << ": not a valid attribute name\n"; return 255; }
    }
  }
  if (from_stdin) { if (!files.empty()) usage_error(c, "Can't specify files with --stdin"); }
  else if (files.empty()) usage_error(c, "No file specified");

  std::string tree;
  if (have_source && !source.empty()) {
    auto o = resolve(c, source);
    if (o) tree = peel_to(c, *o, "tree");
    if (!o || tree.empty()) die(c, source + ": not a valid tree-ish source");
  }
  Attributes attr(c, cached, tree);

  auto check = [&](const std::string& shown) {
    std::string rel = c.to_repo_path(shown);
    if (rel == "..") die(c, "'" + shown + "' is outside repository at '" + c.top + "'");
    if (rel == ".") rel = "";
    auto emit = [&](const std::string& name, const AttrValue& v) {
      if (nul) c.out << shown << '\0' << name << '\0' << v.shown() << '\0';
      else c.out << quote_path(shown) << ": " << name << ": " << v.shown() << "\n";
    };
    if (all) {
      for (auto& [n, v] : attr.all(rel)) emit(n, v);
    } else {
      for (auto& n : attrs) emit(n, attr.get(rel, n));
    }
  };
  if (from_stdin) {
    std::string in = read_all(c.in);
    size_t pos = 0;
    const char delim = nul ? '\0' : '\n';
    while (pos < in.size()) {
      size_t e = in.find(delim, pos);
      std::string item = in.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
      pos = e == std::string::npos ? in.size() : e + 1;
      if (!nul && !item.empty() && item.back() == '\r') item.pop_back();
      if (item.empty()) continue;
      check(nul ? item : unquote_path(item));
    }
  } else {
    for (auto& f : files) check(f);
  }
  return 0;
}

}  // namespace aios_git
