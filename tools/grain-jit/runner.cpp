// qemu-aarch64 test runner: grain with sys, files and the Pi's step limit;
// GRAIN_NOJIT=1 interprets everything, GRAIN_STEPS overrides the limit.
#include "aios/userland/grain.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <filesystem>
using namespace aios::userland::grain;
// Live heap allocations, to show the VM + compiled code tear down to nothing.
static long g_live = 0;
void* operator new(size_t n) { ++g_live; void* p = std::malloc(n ? n : 1); if (!p) std::abort(); return p; }
void operator delete(void* p) noexcept { if (p) { --g_live; std::free(p); } }
void operator delete(void* p, size_t) noexcept { if (p) { --g_live; std::free(p); } }
struct F : Files {
  bool read(const std::string& p, std::string& o, std::string& e) override { std::ifstream i(p, std::ios::binary); if (!i) { e = "no such file"; return false; } std::stringstream s; s << i.rdbuf(); o = s.str(); return true; }
  bool write(const std::string& p, const std::string& d, bool a, std::string& e) override { std::ofstream f(p, std::ios::binary | (a ? std::ios::app : std::ios::trunc)); if (!f) { e = "cannot open"; return false; } f << d; return true; }
  bool list(const std::string& p, std::vector<FileEntry>& out, std::string&) override { for (auto& d : std::filesystem::directory_iterator(p.empty() ? "." : p)) out.push_back({d.path().filename().string(), d.is_directory() ? 0 : (int64_t)d.file_size(), d.is_directory()}); return true; }
  bool remove(const std::string& p, std::string& e) override { if (!std::filesystem::remove(p)) { e = "no such file"; return false; } return true; }
  bool exists(const std::string& p) override { return std::filesystem::is_regular_file(p); }
} files;
static int run_one(char** argv);
int main(int argc, char** argv) {
  const long before = g_live;
  int rc = run_one(argv);
  std::cout.flush();
  if (std::getenv("GRAIN_LEAKS")) std::fprintf(stderr, "live allocations after teardown: %ld\n", g_live - before);
  return rc;
}
static int run_one(char** argv) {
  std::ifstream in(argv[1]); std::stringstream ss; ss << in.rdbuf();
  auto c = compile_source(ss.str(), argv[1]);
  if (!c.ok()) { for (auto& d : c.errors) std::cerr << d.line << ": " << d.message << "\n"; return 1; }
  Vm vm; install_builtins(vm);
  if (std::getenv("GRAIN_NOJIT")) vm.jit_enabled = false;
  vm.step_limit = std::getenv("GRAIN_STEPS") ? std::strtoull(std::getenv("GRAIN_STEPS"), nullptr, 10) : 200000000;
  vm.files = &files;
  auto sys = std::make_shared<MapObj>();
  sys->put("arch", Value::string("aarch64")); sys->put("host", Value::string("qemu")); sys->put("cpus", Value::integer(4));
  sys->put("memory", Value::integer(1 << 30)); sys->put("kernel", Value::string("k")); sys->put("uptime_ms", Value::real(1.0)); sys->put("entities", Value::integer(0));
  vm.define_global("sys", Value::object(Type::Map, sys));
  std::vector<std::string> out; vm.sink = &out;
  auto r = vm.run(c.script);
  for (auto& l : out) std::cout << l << "\n";
  if (!r.ok) { std::cout << argv[1] << ":" << r.line << ": " << r.error << "\n"; for (auto& t : r.trace) std::cout << "  in " << t << "\n"; return 2; }
  return 0;
}
