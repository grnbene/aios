// The `grain` command: the language, reachable from the machine's own prompt.
//
// The language itself knows nothing about AOS -- that is the point of keeping
// it in src/userland with no kernel include. This file is the seam: it hands a
// running VM the `sys` map, so a grain program can ask the machine what it is
// without grain having to depend on a kernel to exist at all.
#include "grain_cmd.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include "aios/userland/grain.hpp"
#include "aios/userland/machine_sql.hpp"
#if !defined(AIOS_METAL)
#include "grain_host.hpp"   // popen, BSD sockets: hosted only
#endif
#include "grain_net.hpp"

namespace aios::cli {

namespace {

using namespace aios::userland;

std::string read_file(const std::string& path, bool& ok) {
  std::ifstream in(path, std::ios::binary);
  if (!in) { ok = false; return ""; }
  std::ostringstream ss;
  ss << in.rdbuf();
  ok = true;
  return ss.str();
}

// grain's file builtins on a hosted machine: ordinary paths, relative to where
// `aios grain` was started. (On the Pi the same builtins land on the SD card -
// metal/arm64/grain_shell.cpp.)
struct HostFiles final : grain::Files {
  bool read(const std::string& path, std::string& out, std::string& error) override {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) { error = "is a directory"; return false; }
    bool ok = false;
    out = read_file(path, ok);
    if (!ok) error = std::filesystem::exists(path, ec) ? "cannot open" : "no such file";
    return ok;
  }
  bool write(const std::string& path, const std::string& data, bool append, std::string& error) override {
    std::ofstream f(path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    if (!f) { error = "cannot open for writing"; return false; }
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    f.close();
    if (!f) { error = "write failed"; return false; }
    return true;
  }
  bool list(const std::string& path, std::vector<grain::FileEntry>& out, std::string& error) override {
    std::error_code ec;
    std::filesystem::directory_iterator it(path.empty() ? "." : path, ec), end;
    if (ec) { error = ec.message(); return false; }
    for (; it != end; it.increment(ec)) {
      if (ec) { error = ec.message(); return false; }
      grain::FileEntry e;
      e.name = it->path().filename().string();
      e.dir = it->is_directory(ec);
      e.size = e.dir ? 0 : static_cast<int64_t>(it->file_size(ec));
      out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(), [](const grain::FileEntry& a, const grain::FileEntry& b) { return a.name < b.name; });
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
};

HostFiles g_host_files;

// What this machine is, as values a program can branch on. Read once at start:
// a script that asks twice should not see the uptime move underneath it in the
// middle of a comparison.
void install_machine(grain::Vm& vm, Kernel& k) {
  userland::install_sql_builtins(vm, k);   // sql() / sql_on(): this machine's engine, and peers' over the node link
  auto sys = std::make_shared<grain::MapObj>();
  const auto& topo = k.host().topology();
  sys->put("arch", grain::Value::string(topo.arch));
  sys->put("host", grain::Value::string(topo.host_name));
  sys->put("cpus", grain::Value::integer((int64_t)topo.logical_cpus));
  sys->put("memory", grain::Value::integer((int64_t)topo.total_memory));
  sys->put("kernel", grain::Value::string(k.self().str()));
  sys->put("uptime_ms", grain::Value::real((double)k.uptime_ns() / 1e6));
  sys->put("entities", grain::Value::integer((int64_t)k.entities().count()));
  auto ms = k.memory().stats();
  sys->put("memory_objects", grain::Value::integer((int64_t)ms.objects));
  sys->put("memory_bytes", grain::Value::integer((int64_t)ms.bytes));
  vm.define_global("sys", grain::Value::object(grain::Type::Map, sys));
}

void report_errors(Out& o, const std::string& origin, const std::vector<grain::Diagnostic>& errs, const std::string& source = "") {
  for (const auto& d : errs)
    o << d.format_snippet(source, origin);
}

// One path for "compile this text and run it", because the file, the one-liner
// and the REPL line must not be able to disagree about what the language does.
bool compile_and_run(Kernel& k, Out& o, const std::string& source,
                     const std::string& origin, bool show_value, bool serving = false) {
  auto compiled = grain::compile_source(source, origin, show_value);
  if (!compiled.ok()) { report_errors(o, origin, compiled.errors, source); return false; }

  grain::Vm vm;
  grain::install_builtins(vm);
  install_machine(vm, k);
  vm.files = &g_host_files;
#if !defined(AIOS_METAL)
  install_grain_host(vm);
  grain::set_http_trust_root(k.config().root);   // https verifies against the machine's own store first
  grain::install_http_builtins(vm);
  vm.allow_process = true;   // a script run from the console may start programs (process_run)
#else
  grain::install_socket_builtins(vm);   // x86 metal: sockets through its own hal::net
#endif
  // A program at a console must not be able to hang the console. Ten million
  // instructions is far past any sensible script and reached in well under a
  // second, so a runaway says so instead of needing a kill.
  //
  // A SERVER is the one program whose whole job is to never finish, so `serve`
  // lifts the limit -- and is also the only mode that hands out sockets, so
  // `grain run` cannot quietly open a port.
  vm.step_limit = serving ? 0 : 10000000;
  if (serving) install_grain_net(vm);

  // A finite program's output is collected and handed over at the end, so a
  // caller that is not a terminal gets the text. A SERVER never reaches the
  // end, so collecting would mean it never says anything at all -- its log
  // goes straight out as it happens.
  std::vector<std::string> out_lines;
  if (!serving) vm.sink = &out_lines;
  auto r = vm.run(compiled.script);
  for (const auto& line : out_lines) o << line << "\n";

  if (!r.ok) {
    grain::Diagnostic d{r.line, r.column, r.error};
    o << d.format_snippet(source, origin);
    for (size_t i = 0; i < r.trace.size(); ++i)
      o << (i == 0 ? "  in " : "  from ") << r.trace[i] << "\n";
    return false;
  }
  if (show_value && r.value.type != grain::Type::Nil) o << r.value.repr() << "\n";
  return true;
}

}  // namespace

// What a hosted app's VMs get inside aios (`aios serve`): everything `grain serve` gives a program here - the
// machine's SQL (sql(), sql_on() across the fleet), `sys`, files, http, sockets - with no step limit.
void setup_app_vm(Kernel& k, grain::Vm& vm) {
  grain::install_builtins(vm);
  // grain's own database client (params, records, options), as a grain host gives it: an app is the same program
  // on either host. aios's older db_query (two arguments, rows only) stays for the console's own scripts.
  grain::install_db_builtins(vm);
  install_machine(vm, k);
  vm.files = &g_host_files;
#if !defined(AIOS_METAL)
  install_grain_host(vm);
  grain::set_http_trust_root(k.config().root);
  grain::install_http_builtins(vm);
#else
  grain::install_socket_builtins(vm);
#endif
  install_grain_net(vm);
  vm.step_limit = 0;
}

// `grain` with no arguments says what it is rather than failing, because the
// first thing anyone types at a new command is its own name.
Result<void> run_grain_command(Kernel& k, Out& o, const std::vector<std::string>& a) {
  const std::string sub = a.size() > 1 ? a[1] : "help";

  if (sub == "help" || sub == "-h") {
    o << "grain — this machine's own language: lexer, parser, bytecode compiler, VM\n"
      << "  grain run <file.gr>        compile a file and run it\n"
      << "  grain eval <source...>     compile one line of source and run it\n"
      << "  grain dis <file.gr>        show the bytecode the compiler emitted\n"
      << "  grain serve <file.gr>      run it with sockets and no step limit\n"
      << "  grain repl                 read lines until EOF or .quit\n"
      << "\n"
      << "  let / fn / if / else / while / for..in / return / break / continue\n"
      << "  int real string bool nil list map, closures, first-class functions\n"
      << "  `sys` holds what this machine is: sys.arch, sys.cpus, sys.memory\n";
    return Ok();
  }

  if (sub == "run" || sub == "dis" || sub == "serve") {
    if (a.size() < 3) return Result<void>(Status::Invalid, "grain " + sub + " <file>");
    bool ok = false;
    const std::string source = read_file(a[2], ok);
    if (!ok) return Result<void>(Status::NotFound, "cannot read " + a[2]);
    if (sub == "dis") {
      auto compiled = grain::compile_source(source, a[2]);
      if (!compiled.ok()) { report_errors(o, a[2], compiled.errors); return Result<void>(Status::Invalid, "did not compile"); }
      o << grain::disassemble(*compiled.script);
      return Ok();
    }
    if (!compile_and_run(k, o, source, a[2], false, sub == "serve"))
      return Result<void>(Status::Invalid, a[2] + " did not run to completion");
    return Ok();
  }

  if (sub == "eval") {
    if (a.size() < 3) return Result<void>(Status::Invalid, "grain eval <source>");
    std::string source;
    for (size_t i = 2; i < a.size(); ++i) { if (i > 2) source += " "; source += a[i]; }
    if (!compile_and_run(k, o, source, "<eval>", true))
      return Result<void>(Status::Invalid, "did not run to completion");
    return Ok();
  }

  if (sub == "repl") {
    o << "grain — end with .quit or EOF\n";
    // The REPL keeps ONE vm, so `let x = 1` on one line is still there on the
    // next. Each line is its own script compiled against the globals the
    // previous ones left behind.
    grain::Vm vm;
    grain::install_builtins(vm);
    install_machine(vm, k);
    vm.files = &g_host_files;
  #if !defined(AIOS_METAL)
  install_grain_host(vm);
  grain::set_http_trust_root(k.config().root);   // https verifies against the machine's own store first
  grain::install_http_builtins(vm);
  vm.allow_process = true;   // a script run from the console may start programs (process_run)
#else
  grain::install_socket_builtins(vm);   // x86 metal: sockets through its own hal::net
#endif
    vm.step_limit = 10000000;
    std::string line;
    while (std::getline(std::cin, line)) {
      if (line == ".quit" || line == ".exit") break;
      if (line.empty()) continue;
      auto compiled = grain::compile_source(line, "<repl>", true);
      if (!compiled.ok()) { report_errors(o, "<repl>", compiled.errors, line); continue; }
      std::vector<std::string> out_lines;
      vm.sink = &out_lines;
      auto r = vm.run(compiled.script);
      for (const auto& l : out_lines) o << l << "\n";
      if (!r.ok) {
        grain::Diagnostic d{r.line, r.column, r.error};
        o << d.format_snippet(line, "<repl>");
        for (size_t i = 0; i < r.trace.size(); ++i)
          o << (i == 0 ? "  in " : "  from ") << r.trace[i] << "\n";
      } else if (r.value.type != grain::Type::Nil) {
        o << r.value.repr() << "\n";
      }
    }
    return Ok();
  }

  return Result<void>(Status::Invalid, "grain run|serve|eval|dis|repl — `grain help` lists them");
}

}  // namespace aios::cli
