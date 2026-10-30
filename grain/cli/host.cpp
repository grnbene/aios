#include "host.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <system_error>
#include <thread>

#include "aios/hal/process.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <unistd.h>
#else
#include <unistd.h>
#endif

namespace grain_cli {

namespace g = aios::userland::grain;

namespace {

struct HostFiles final : g::Files {
  bool read(const std::string& path, std::string& out, std::string& error) override {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) { error = "is a directory"; return false; }
    std::ifstream in(path, std::ios::binary);
    if (!in) { error = std::filesystem::exists(path, ec) ? "cannot open" : "no such file"; return false; }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
  }
  bool write(const std::string& path, const std::string& data, bool append, std::string& error) override {
    std::ofstream f(path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    if (!f) { error = "cannot open for writing"; return false; }
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    f.close();
    if (!f) { error = "write failed"; return false; }
    return true;
  }
  bool list(const std::string& path, std::vector<g::FileEntry>& out, std::string& error) override {
    std::error_code ec;
    std::filesystem::directory_iterator it(path.empty() ? "." : path, ec), end;
    if (ec) { error = ec.message(); return false; }
    for (; it != end; it.increment(ec)) {
      if (ec) { error = ec.message(); return false; }
      g::FileEntry e;
      e.name = it->path().filename().string();
      e.dir = it->is_directory(ec);
      e.size = e.dir ? 0 : static_cast<int64_t>(it->file_size(ec));
      out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(), [](const g::FileEntry& a, const g::FileEntry& b) { return a.name < b.name; });
    return true;
  }
  bool remove(const std::string& path, std::string& error) override {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) { error = "no such file"; return false; }
    if (!std::filesystem::remove(path, ec)) { error = ec ? ec.message() : "not removed"; return false; }
    return true;
  }
  bool exists(const std::string& path) override {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
  }
  bool make_dir(const std::string& path, bool parents, std::string& error) override {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) return true;   // already there is not a failure
    const bool made = parents ? std::filesystem::create_directories(path, ec) : std::filesystem::create_directory(path, ec);
    if (!made || ec) { error = ec ? ec.message() : "not made"; return false; }
    return true;
  }
  bool remove_dir(const std::string& path, std::string& error) override {
    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec)) { error = "no such directory"; return false; }
    if (!std::filesystem::is_empty(path, ec)) { error = ec ? ec.message() : "not empty"; return false; }
    if (!std::filesystem::remove(path, ec)) { error = ec ? ec.message() : "not removed"; return false; }
    return true;
  }
  bool is_dir(const std::string& path) override {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec);
  }
  bool absolute(const std::string& path, std::string& out, std::string& error) override {
    std::error_code ec;
    const auto p = std::filesystem::absolute(path, ec);
    if (ec) { error = ec.message(); return false; }
    out = p.generic_string();
    return true;
  }
  bool stat(const std::string& path, g::FileStat& out, std::string&) override {
    std::error_code ec;
    const auto s = std::filesystem::status(path, ec);
    if (ec || !std::filesystem::exists(s)) return false;
    out.dir = std::filesystem::is_directory(s);
    out.size = out.dir ? 0 : static_cast<int64_t>(std::filesystem::file_size(path, ec));
    const auto t = std::filesystem::last_write_time(path, ec);
    if (!ec) out.modified_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::file_clock::to_sys(t).time_since_epoch()).count();
    out.mode = static_cast<uint32_t>(s.permissions()) & 07777;
    return true;
  }
  bool rename(const std::string& from, const std::string& to, std::string& error) override {
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    if (ec) { error = ec.message(); return false; }
    return true;
  }
};

const char* os_name() {
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#elif defined(__linux__)
  return "linux";
#elif defined(__FreeBSD__)
  return "freebsd";
#else
  return "unknown";
#endif
}

const char* arch_name() {
#if defined(__aarch64__) || defined(_M_ARM64)
  return "aarch64";
#elif defined(__x86_64__) || defined(_M_X64)
  return "x86_64";
#elif defined(__arm__) || defined(_M_ARM)
  return "arm";
#elif defined(__i386__) || defined(_M_IX86)
  return "x86";
#elif defined(__riscv)
  return "riscv";
#else
  return "unknown";
#endif
}

int64_t total_memory() {
#if defined(_WIN32)
  MEMORYSTATUSEX ms;
  ms.dwLength = sizeof(ms);
  return GlobalMemoryStatusEx(&ms) ? static_cast<int64_t>(ms.ullTotalPhys) : 0;
#elif defined(__APPLE__)
  int64_t bytes = 0;
  size_t len = sizeof(bytes);
  return sysctlbyname("hw.memsize", &bytes, &len, nullptr, 0) == 0 ? bytes : 0;
#else
  const long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGE_SIZE);
  return pages > 0 && page > 0 ? static_cast<int64_t>(pages) * page : 0;
#endif
}

std::string host_name() {
#if defined(_WIN32)
  char buf[MAX_COMPUTERNAME_LENGTH + 1];
  DWORD n = sizeof(buf);
  return GetComputerNameA(buf, &n) ? std::string(buf, n) : std::string();
#else
  char buf[256] = {0};
  return gethostname(buf, sizeof(buf) - 1) == 0 ? std::string(buf) : std::string();
#endif
}

g::Value result_map(int code, std::string output) {
  auto out = std::make_shared<g::MapObj>();
  out->put("ok", g::Value::boolean(code == 0));
  out->put("code", g::Value::integer(code));
  out->put("output", g::Value::string(std::move(output)));
  return g::Value::object(g::Type::Map, out);
}

// A process may run as long as it needs: grain has no business guessing how
// long a build or a backup takes. UINT32_MAX is INFINITE to Windows'
// WaitForSingleObject and 49 days to the POSIX wait loop. (0 would be
// "already timed out" to both.)
constexpr uint32_t kNoTimeout = UINT32_MAX;

g::Value run_argv(const std::vector<std::string>& argv, const char* who, std::string& error) {
  auto r = aios::hal::run_tool(argv, kNoTimeout);
  if (!r.ok()) { error = std::string(who) + "(): " + r.error().message; return g::Value::nil(); }
  return result_map(r.value().exit_code, std::move(r.value().output));
}

g::Value bi_sys_exec(g::Vm&, std::vector<g::Value>& args, std::string& error) {
  if (args[0].type != g::Type::Str) { error = std::string("sys_exec() wants a string, got a ") + g::type_name(args[0].type); return g::Value::nil(); }
#if defined(_WIN32)
  return run_argv({"cmd.exe", "/d", "/s", "/c", args[0].str()}, "sys_exec", error);
#else
  return run_argv({"/bin/sh", "-c", args[0].str()}, "sys_exec", error);
#endif
}

g::Value bi_exec(g::Vm&, std::vector<g::Value>& args, std::string& error) {
  if (args[0].type != g::Type::List) { error = std::string("exec() wants a list, got a ") + g::type_name(args[0].type); return g::Value::nil(); }
  std::vector<std::string> argv;
  for (const auto& v : args[0].list_items()) {
    if (v.type != g::Type::Str) { error = std::string("exec() wants a list of strings, found a ") + g::type_name(v.type); return g::Value::nil(); }
    argv.push_back(v.str());
  }
  if (argv.empty()) { error = "exec() wants the program to run"; return g::Value::nil(); }
  return run_argv(argv, "exec", error);
}

}  // namespace

g::Files& host_files() {
  static HostFiles files;
  return files;
}

void install_sys(g::Vm& vm, const std::string& version) {
  auto sys = std::make_shared<g::MapObj>();
  sys->put("os", g::Value::string(os_name()));
  sys->put("arch", g::Value::string(arch_name()));
  sys->put("cpus", g::Value::integer(static_cast<int64_t>(std::max(1u, std::thread::hardware_concurrency()))));
  sys->put("memory", g::Value::integer(total_memory()));
  sys->put("host", g::Value::string(host_name()));
  sys->put("version", g::Value::string(version));
  vm.define_global("sys", g::Value::object(g::Type::Map, sys));
}

void install_args(g::Vm& vm, const std::vector<std::string>& args) {
  std::vector<g::Value> items;
  items.reserve(args.size());
  for (const auto& a : args) items.push_back(g::Value::string(a));
  vm.define_global("args", g::Value::list(std::move(items)));
}

void install_process(g::Vm& vm) {
  vm.define_native("sys_exec", 1, 1, bi_sys_exec);
  vm.define_native("exec", 1, 1, bi_exec);
}

std::string state_dir() {
  auto env = [](const char* name) -> std::string {
    const char* v = std::getenv(name);
    return v && *v ? std::string(v) : std::string();
  };
#if defined(_WIN32)
  const std::string base = env("LOCALAPPDATA");
  return base.empty() ? std::string() : base + "\\grain";
#elif defined(__APPLE__)
  const std::string home = env("HOME");
  return home.empty() ? std::string() : home + "/Library/Caches/grain";
#else
  const std::string xdg = env("XDG_CACHE_HOME");
  if (!xdg.empty()) return xdg + "/grain";
  const std::string home = env("HOME");
  return home.empty() ? std::string() : home + "/.cache/grain";
#endif
}

}  // namespace grain_cli
