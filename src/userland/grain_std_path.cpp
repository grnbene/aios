// grain's paths and directories.
//
// Pure text (the same answer everywhere, no filesystem touched):
//   path_join(a, b, ...)   the parts joined with "/", an absolute part restarting it
//   basename(path)         the last part:            "a/b/c.txt" -> "c.txt"
//   dirname(path)          everything before it:     "a/b/c.txt" -> "a/b"
//   extname(path)          the extension with its dot: "c.tar.gz" -> ".gz", "c" -> ""
//   path_normalize(path)   "." and ".." resolved, doubled separators dropped
// On Windows "\" separates as well as "/" (and a drive letter or \\server
// share is absolute); joining always writes "/", which Windows accepts.
//
// Through Vm::files, like read_file and write_file:
//   mkdir(path [, parents])   make a directory (and its parents when true)
//   rmdir(path)               remove an empty directory
//   is_dir(path)              whether path is a directory
//   abspath(path)             path against the working directory
#include <cctype>
#include <string>
#include <vector>

#include "aios/userland/grain.hpp"
#include "grain_builtins_internal.hpp"

namespace aios::userland::grain {

namespace {

bool is_sep(char c) {
#if defined(_WIN32)
  return c == '/' || c == '\\';
#else
  return c == '/';
#endif
}

// How much of p is its root: "/" , "C:/", "\\server\share\" (Windows), or nothing.
size_t root_length(const std::string& p) {
#if defined(_WIN32)
  if (p.size() >= 2 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':') return p.size() >= 3 && is_sep(p[2]) ? 3 : 2;
  if (p.size() >= 2 && is_sep(p[0]) && is_sep(p[1])) {   // \\server\share
    size_t i = 2;
    while (i < p.size() && !is_sep(p[i])) ++i;
    if (i < p.size()) ++i;
    while (i < p.size() && !is_sep(p[i])) ++i;
    return i < p.size() ? i + 1 : i;
  }
#endif
  return !p.empty() && is_sep(p[0]) ? 1 : 0;
}

std::string strip_trailing(const std::string& p) {
  std::string s = p;
  const size_t root = root_length(s);
  while (s.size() > root && is_sep(s.back())) s.pop_back();
  return s;
}

std::string base_of(const std::string& path) {
  const std::string s = strip_trailing(path);
  const size_t root = root_length(s);
  if (s.size() == root) return "";
  size_t i = s.size();
  while (i > root && !is_sep(s[i - 1])) --i;
  return s.substr(i);
}

std::string dir_of(const std::string& path) {
  const std::string s = strip_trailing(path);
  const size_t root = root_length(s);
  size_t i = s.size();
  while (i > root && !is_sep(s[i - 1])) --i;   // back over the last part
  if (i == root) return root ? s.substr(0, root) : ".";
  while (i > root && is_sep(s[i - 1])) --i;    // and the separators before it
  return s.substr(0, i);
}

std::string normalize(const std::string& path) {
  const size_t root = root_length(path);
  std::string head = path.substr(0, root);
  for (char& c : head) if (is_sep(c)) c = '/';
  std::vector<std::string> parts;
  size_t i = root;
  while (i <= path.size()) {
    size_t j = i;
    while (j < path.size() && !is_sep(path[j])) ++j;
    const std::string part = path.substr(i, j - i);
    if (part == "..") {
      if (!parts.empty() && parts.back() != "..") parts.pop_back();
      else if (!root) parts.push_back("..");   // above an absolute root there is nothing: dropped
    } else if (!part.empty() && part != ".") {
      parts.push_back(part);
    }
    i = j + 1;
  }
  std::string out = head;
  for (size_t k = 0; k < parts.size(); ++k) { if (k) out += "/"; out += parts[k]; }
  return out.empty() ? "." : out;
}

Value bi_path_join(Vm&, std::vector<Value>& args, std::string& error) {
  std::string out;
  for (const auto& a : args) {
    if (!want_str(a, error, "path_join")) return Value::nil();
    const std::string& part = a.str();
    if (part.empty()) continue;
    if (root_length(part) > 0) { out = part; continue; }   // an absolute part starts the path again
    if (!out.empty() && !is_sep(out.back())) out += "/";
    out += part;
  }
  return Value::string(out);
}

Value bi_basename(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "basename")) return Value::nil();
  return Value::string(base_of(args[0].str()));
}

Value bi_dirname(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "dirname")) return Value::nil();
  return Value::string(dir_of(args[0].str()));
}

Value bi_extname(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "extname")) return Value::nil();
  const std::string b = base_of(args[0].str());
  const size_t dot = b.rfind('.');
  // ".profile" is a name, not an extension; "a." has an empty one.
  return Value::string(dot == std::string::npos || dot == 0 ? "" : b.substr(dot));
}

Value bi_path_normalize(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "path_normalize")) return Value::nil();
  return Value::string(normalize(args[0].str()));
}

Files* files_for(Vm& vm, std::string& error, const char* who) {
  if (!vm.files) error = std::string(who) + "(): this program has no file storage";
  return vm.files;
}

Value bi_mkdir(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "mkdir")) return Value::nil();
  const bool parents = args.size() > 1 && args[1].truthy();
  Files* f = files_for(vm, error, "mkdir");
  if (!f) return Value::nil();
  std::string why;
  if (!f->make_dir(args[0].str(), parents, why)) { error = "mkdir(\"" + args[0].str() + "\"): " + why; return Value::nil(); }
  return Value::boolean(true);
}

Value bi_rmdir(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "rmdir")) return Value::nil();
  Files* f = files_for(vm, error, "rmdir");
  if (!f) return Value::nil();
  std::string why;
  if (!f->remove_dir(args[0].str(), why)) { error = "rmdir(\"" + args[0].str() + "\"): " + why; return Value::nil(); }
  return Value::boolean(true);
}

Value bi_is_dir(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "is_dir")) return Value::nil();
  Files* f = files_for(vm, error, "is_dir");
  if (!f) return Value::nil();
  return Value::boolean(f->is_dir(args[0].str()));
}

Value bi_abspath(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "abspath")) return Value::nil();
  Files* f = files_for(vm, error, "abspath");
  if (!f) return Value::nil();
  std::string out, why;
  if (!f->absolute(args[0].str(), out, why)) { error = "abspath(\"" + args[0].str() + "\"): " + why; return Value::nil(); }
  return Value::string(normalize(out));
}

}  // namespace

void install_path_builtins(Vm& vm) {
  vm.define_native("path_join", 1, -1, bi_path_join);
  vm.define_native("basename", 1, 1, bi_basename);
  vm.define_native("dirname", 1, 1, bi_dirname);
  vm.define_native("extname", 1, 1, bi_extname);
  vm.define_native("path_normalize", 1, 1, bi_path_normalize);
  vm.define_native("mkdir", 1, 2, bi_mkdir);
  vm.define_native("rmdir", 1, 1, bi_rmdir);
  vm.define_native("is_dir", 1, 1, bi_is_dir);
  vm.define_native("abspath", 1, 1, bi_abspath);
}

}  // namespace aios::userland::grain
