// grain test: runs a project's tests.
//
//   grain test [file.gr | dir]...     every *_test.gr under the given places
//                                     (the working directory when none), each file in a VM of its own
//
// A test file registers its tests and the runner runs them, each on its own:
//
//   test("adds", fn () { expect_eq(1 + 1, 2) })
//   test("rejects nil", fn () { expect(not pcall(fn () { return upper(nil) }).ok) })
//
//   expect(condition [, message])         fails the test when condition is false or nil
//   expect_eq(got, want [, message])      fails it when they differ, saying both
//   expect_error(fn [, part])             fails it unless fn raises an error (containing part)
//
// A test passes when its function returns and fails when it raises an error
// (any error, an expectation's or the program's own). A file that does not
// compile, or fails before its tests run, fails as a whole. Exit status 0 when
// everything passed, 1 otherwise.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <system_error>

#include "tools.hpp"

namespace grain_cli {

namespace g = aios::userland::grain;
namespace fs = std::filesystem;

namespace {

struct Registered {
  std::string name;
  g::Value fn;
};
// The tests a file registered, for the VM running it (one file at a time).
std::vector<Registered>* g_tests = nullptr;

g::Value bi_test(g::Vm&, std::vector<g::Value>& args, std::string& error) {
  if (args[0].type != g::Type::Str) { error = std::string("test() wants a name, got a ") + g::type_name(args[0].type); return g::Value::nil(); }
  if (args[1].type != g::Type::Fn) { error = std::string("test() wants a function, got a ") + g::type_name(args[1].type); return g::Value::nil(); }
  g_tests->push_back({args[0].str(), args[1]});
  return g::Value::nil();
}

std::string note(std::vector<g::Value>& args, size_t at) {
  return args.size() > at ? " (" + args[at].to_text() + ")" : std::string();
}

g::Value bi_expect(g::Vm&, std::vector<g::Value>& args, std::string& error) {
  if (!args[0].truthy()) error = "expected true, got " + args[0].repr() + note(args, 1);
  return g::Value::nil();
}

g::Value bi_expect_eq(g::Vm&, std::vector<g::Value>& args, std::string& error) {
  if (!args[0].equals(args[1])) error = "expected " + args[1].repr() + ", got " + args[0].repr() + note(args, 2);
  return g::Value::nil();
}

g::Value bi_expect_error(g::Vm& vm, std::vector<g::Value>& args, std::string& error) {
  std::string raised;
  vm.call(args[0], {}, raised);
  if (raised.empty()) { error = "expected an error, but the function returned"; return g::Value::nil(); }
  if (args.size() > 1) {
    const std::string part = args[1].to_text();
    if (raised.find(part) == std::string::npos) error = "expected an error containing " + args[1].repr() + ", got: " + raised;
  }
  return g::Value::nil();
}

void collect(const fs::path& p, std::vector<fs::path>& out) {
  std::error_code ec;
  if (fs::is_directory(p, ec)) {
    for (fs::recursive_directory_iterator it(p, ec), end; it != end && !ec; it.increment(ec)) {
      const std::string n = it->path().filename().string();
      if (it->is_regular_file(ec) && n.size() > 8 && n.compare(n.size() - 8, 8, "_test.gr") == 0) out.push_back(it->path());
    }
  } else {
    out.push_back(p);
  }
}

bool read_all(const fs::path& p, std::string& out) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

}  // namespace

int test_command(const std::vector<std::string>& args) {
  std::vector<fs::path> files;
  for (const auto& a : args) collect(a, files);
  if (args.empty()) collect(".", files);
  std::sort(files.begin(), files.end());
  if (files.empty()) { std::cerr << "grain test: no *_test.gr files found\n"; return 1; }

  size_t passed = 0, failed = 0;
  for (const auto& f : files) {
    const std::string origin = f.lexically_normal().generic_string();
    std::cout << origin << "\n";
    std::string src;
    if (!read_all(f, src)) { std::cout << "  FAIL cannot read the file\n"; ++failed; continue; }
    auto compiled = g::compile_source(src, origin);
    if (!compiled.ok()) {
      for (const auto& d : compiled.errors) std::cout << "  FAIL " << origin << ":" << d.line << ": " << d.message << "\n";
      ++failed;
      continue;
    }
    std::vector<Registered> tests;
    g_tests = &tests;
    g::Vm vm;
    struct Drop { g::Vm& vm; ~Drop() { g::drop_panes(vm); } } drop{vm};   // the file's panes go with its VM
    prepare_vm(vm, {});
    vm.define_native("test", 2, 2, bi_test);
    vm.define_native("expect", 1, 2, bi_expect);
    vm.define_native("expect_eq", 2, 3, bi_expect_eq);
    vm.define_native("expect_error", 1, 2, bi_expect_error);
    auto r = vm.run(compiled.script);
    if (!r.ok) {
      std::cout << "  FAIL the file stopped before its tests ran: " << origin << ":" << r.line << ": " << r.error << "\n";
      ++failed;
      g_tests = nullptr;
      continue;
    }
    if (tests.empty()) std::cout << "  (no tests registered)\n";
    for (const auto& t : tests) {
      std::string err;
      vm.call(t.fn, {}, err);
      if (err.empty()) {
        std::cout << "  ok   " << t.name << "\n";
        ++passed;
      } else {
        std::cout << "  FAIL " << t.name << ": " << err << "\n";
        ++failed;
      }
    }
    g_tests = nullptr;
  }
  std::cout << passed << " passed, " << failed << " failed\n";
  return failed ? 1 : 0;
}

}  // namespace grain_cli
