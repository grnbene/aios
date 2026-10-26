// The standalone `grain` program: the language without aios around it.
//
// The same lexer, parser, compiler, VM and standard library `aios grain` runs,
// with the host operating system where aios would be: files on the host's own
// filesystem, its processes, its sockets, and a `sys` map describing it. What
// only exists inside aios (the sidecar, ASQL, the SAT solver) is not here.
//
// Exit status: 0 the program ran to its end, 1 it did not compile, 2 it stopped
// on a runtime error, 64 the command line was wrong.
#include <cstdlib>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "aios/userland/grain.hpp"
#include "bundle.hpp"
#include "tools.hpp"
#include "grain_net.hpp"
#include "ffi.hpp"
#include "host.hpp"
#include "plugin.hpp"
#include "open_client.hpp"
#include "serve.hpp"
#if defined(GRAIN_HAVE_WIN32)
#include "display_win32.hpp"
#elif defined(GRAIN_HAVE_X11)
#include "display_x11.hpp"
#elif defined(GRAIN_HAVE_SDL2)
#include "display_sdl.hpp"
#endif

#ifndef GRAIN_VERSION
#define GRAIN_VERSION "0.0.0-dev"
#endif

namespace g = aios::userland::grain;

namespace {

constexpr int kExitOk = 0, kExitCompile = 1, kExitRuntime = 2, kExitUsage = 64;

struct Options {
  uint64_t steps = 0;     // 0: run until the program ends
  bool jit = true;
  bool safe = false;      // --safe / --no-native: nothing that can touch memory grain does not own
};

void usage(std::ostream& o) {
  o << "grain " GRAIN_VERSION " - a small, fast, embeddable language\n"
    << "\n"
    << "usage:\n"
    << "  grain <file.gr> [args...]         run a program (same as grain run)\n"
    << "  grain run <file.gr> [args...]     run a program; `args` holds what follows it\n"
    << "  grain eval <source...>            run one line and print its value\n"
    << "  grain repl                        read and run lines until EOF or .quit\n"
    << "  grain serve <file.gr> [--port N] [--max-body 10m]  the program's ui.pane in a browser, not a window\n"
    << "  grain open <url>                  a ui.pane program running on a server, in a native window (no HTML)\n"
    << "  grain check <file.gr>...          compile only and report every error\n"
    << "  grain dis <file.gr>               show the bytecode the compiler emitted\n"
    << "  grain build <file.gr> [-o out]    make one executable of the program (see grain build)\n"
    << "  grain test [file | dir]...        run the tests in *_test.gr files\n"
    << "  grain fmt [--check] <file | dir>  lay source out the one grain way\n"
    << "  grain lsp                         a language server for editors, on stdin/stdout\n"
    << "  grain host                        this machine's app host: runs, restarts and serves its apps\n"
    << "  grain app deploy|status|logs|...  apps, on this machine or another (`grain app help`)\n"
    << "  grain pkg init|add|list|vendor    package and dependency management (`grain pkg help`)\n"
    << "  grain debug <file.gr>             run in interactive step-by-step debugger\n"
    << "  grain version                     print the version\n"
    << "\n"
    << "options (before the subcommand):\n"
    << "  --steps <n>                       stop a program after n instructions\n"
    << "  --no-jit                          interpret everything\n"
    << "  --safe                            native fn only on bytes: no ptr, extern fn, ffi or plugins\n"
    << "  --no-native                       refuse native fn altogether\n";
}

bool read_source(const std::string& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

// Each problem as file:line:column: message, then the line and a caret under the column when one is known.
void report_compile(const std::string& origin, const std::vector<g::Diagnostic>& errs, const std::string& source = "") {
  for (const auto& d : errs) std::cerr << d.format_snippet(source, origin);
}

void report_runtime(const std::string& origin, const g::Vm::RunResult& r, const std::string& source = "") {
  std::string src = source;
  if (src.empty() && !origin.empty() && origin[0] != '<') {
    read_source(origin, src);
  }
  g::Diagnostic d{r.line, r.column, r.error};
  std::cerr << d.format_snippet(src, origin);
  for (size_t i = 0; i < r.trace.size(); ++i) std::cerr << (i == 0 ? "  in " : "  from ") << r.trace[i] << "\n";
}

// A VM's panes leave the registry with it: the registry outlives every VM.
struct DropPanes {
  g::Vm& vm;
  bool done = false;
  void now() { if (!done) { done = true; g::drop_panes(vm); } }
  ~DropPanes() { now(); }
};

// One VM set up the same way for a file, a one-liner and the REPL, so the three
// cannot disagree about what the language has.
void prepare(g::Vm& vm, const Options& opt, const std::vector<std::string>& script_args) {
  g::install_core_builtins(vm);
  g::install_socket_builtins(vm);
  g::install_window_builtins(vm);
  aios::cli::install_grain_net(vm);
  g::install_http_builtins(vm);
  g::install_db_builtins(vm);
  grain_cli::install_process(vm);
  vm.allow_process = !opt.safe;   // a script the user runs may start programs (process_run), as exec() does
  if (!opt.safe) {
    grain_cli::install_ffi(vm);
    grain_cli::install_plugins(vm);
  }
  grain_cli::install_sys(vm, GRAIN_VERSION);
  grain_cli::install_args(vm, script_args);
  vm.files = &grain_cli::host_files();
  vm.step_limit = opt.steps;
  vm.jit_enabled = opt.jit;
}

}  // namespace

namespace grain_cli {
void prepare_vm(g::Vm& vm, const std::vector<std::string>& script_args) { prepare(vm, Options{}, script_args); }
}  // namespace grain_cli

namespace {

// Where a program's imports start: its own directory.
std::string dir_of(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? "" : path.substr(0, slash);
}

int run_file(const Options& opt, const std::string& path, const std::vector<std::string>& script_args) {
  std::string source;
  if (!read_source(path, source)) { std::cerr << "grain: cannot read " << path << "\n"; return kExitUsage; }
  auto compiled = g::compile_source(source, path);
  if (!compiled.ok()) { report_compile(path, compiled.errors, source); return kExitCompile; }
  g::Vm vm;
  DropPanes drop{vm};
  prepare(vm, opt, script_args);
  vm.base_dir = dir_of(path);
  auto r = vm.run(compiled.script);
  if (!r.ok) { report_runtime(path, r, source); return kExitRuntime; }
  drop.now();
  vm.retire_heap();   // the program is over; freeing its heap object by object is time spent before the process exits anyway
  return kExitOk;
}

int eval_line(const Options& opt, const std::string& source) {
  auto compiled = g::compile_source(source, "<eval>", true);
  if (!compiled.ok()) { report_compile("<eval>", compiled.errors, source); return kExitCompile; }
  g::Vm vm;
  DropPanes drop{vm};
  prepare(vm, opt, {});
  auto r = vm.run(compiled.script);
  if (!r.ok) { report_runtime("<eval>", r, source); return kExitRuntime; }
  if (r.value.type != g::Type::Nil) std::cout << r.value.repr() << "\n";
  return kExitOk;
}

// One VM for the whole session: `let x = 1` on one line is still there on the
// next. Each line is its own script, compiled against what earlier lines left.
int repl(const Options& opt) {
  const bool tty = isatty(fileno(stdin)) != 0;
  if (tty) std::cout << "grain " GRAIN_VERSION " - .quit or EOF to leave\n";
  g::Vm vm;
  DropPanes drop{vm};
  prepare(vm, opt, {});
  std::string line;
  int status = kExitOk;
  for (;;) {
    if (tty) std::cout << "> " << std::flush;
    if (!std::getline(std::cin, line)) break;
    if (line == ".quit" || line == ".exit") break;
    if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
    auto compiled = g::compile_source(line, "<repl>", true);
    if (!compiled.ok()) { report_compile("<repl>", compiled.errors, line); status = kExitCompile; continue; }
    auto r = vm.run(compiled.script);
    if (!r.ok) { report_runtime("<repl>", r, line); status = kExitRuntime; continue; }
    if (r.value.type != g::Type::Nil) std::cout << r.value.repr() << "\n";
  }
  if (tty) std::cout << "\n";
  // Piped in, a REPL is a script read line by line: its status is whether every line ran.
  return tty ? kExitOk : status;
}

// A program packed onto this executable (grain build): it gets every argument.
// serve [--port N] [--host H] and `--web [port]`: the program in a browser, not a window.
int serve_program(const std::string& name, std::function<bool(std::string&, std::string&, std::string&)> load,
                  std::function<void(g::Vm&)> setup, const std::vector<std::string>& opts) {
  grain_cli::ServeOptions so;
  if (const char* env_host = std::getenv("GRAIN_HOST")) so.host = env_host;
  if (const char* env_port = std::getenv("GRAIN_PORT")) so.port = std::atoi(env_port);
  // 4096, 64k, 10M, 1g: bytes with an optional k/m/g suffix
  auto size_arg = [](const std::string& t) -> size_t {
    char* end = nullptr;
    size_t n = (size_t)std::strtoull(t.c_str(), &end, 10);
    switch (end && *end ? std::tolower((unsigned char)*end) : 0) { case 'k': n <<= 10; break; case 'm': n <<= 20; break; case 'g': n <<= 30; break; default: break; }
    return n;
  };
  if (const char* env_body = std::getenv("GRAIN_MAX_BODY")) so.max_body = size_arg(env_body);
  for (size_t k = 0; k < opts.size(); ++k) {
    if (opts[k] == "--max-body" && k + 1 < opts.size()) so.max_body = size_arg(opts[++k]);
    else if (opts[k] == "--port" && k + 1 < opts.size()) so.port = std::atoi(opts[++k].c_str());
    else if (opts[k] == "--host" && k + 1 < opts.size()) so.host = opts[++k];
    else if (opts[k] == "--all" || opts[k] == "--all-ips") so.host = "0.0.0.0";
    else if (opts[k] == "--localhost" || opts[k] == "--local") so.host = "127.0.0.1";
    else if (opts[k] == "--hide-errors") so.show_errors = false;
    else if (!opts[k].empty() && std::isdigit((unsigned char)opts[k][0])) so.port = std::atoi(opts[k].c_str());
    else { std::cerr << "grain: unknown serve option " << opts[k] << "\n"; return kExitUsage; }
  }
  return grain_cli::serve_ui(name, std::move(load), std::move(setup), so);
}

int run_pack(const grain_cli::Pack& pack, const std::vector<std::string>& script_args) {
  const std::string& name = pack.main_name;
  if (!script_args.empty() && script_args[0] == "--web") {   // the same program, in a browser
    return serve_program(name,
        [&pack](std::string& source, std::string& version, std::string&) { source = pack.files.at(pack.main_name); version = "pack"; return true; },
        [&pack](g::Vm& vm) { prepare(vm, Options{}, {}); grain_cli::install_bundle_builtins(vm, pack); },
        std::vector<std::string>(script_args.begin() + 1, script_args.end()));
  }
  auto compiled = g::compile_source(pack.files.at(name), name);
  if (!compiled.ok()) { report_compile(name, compiled.errors); return kExitCompile; }
  g::Vm vm;
  DropPanes drop{vm};
  prepare(vm, Options{}, script_args);
  grain_cli::install_bundle_builtins(vm, pack);
  auto r = vm.run(compiled.script);
  if (!r.ok) { report_runtime(name, r, pack.files.at(name)); return kExitRuntime; }
  return kExitOk;
}

int check_files(const std::vector<std::string>& paths) {
  int status = kExitOk;
  for (const auto& path : paths) {
    std::string source;
    if (!read_source(path, source)) { std::cerr << "grain: cannot read " << path << "\n"; status = kExitUsage; continue; }
    auto compiled = g::compile_source(source, path);
    if (!compiled.ok()) { report_compile(path, compiled.errors, source); if (status == kExitOk) status = kExitCompile; }
  }
  return status;
}

int disassemble(const std::string& path) {
  std::string source;
  if (!read_source(path, source)) { std::cerr << "grain: cannot read " << path << "\n"; return kExitUsage; }
  auto compiled = g::compile_source(source, path);
  if (!compiled.ok()) { report_compile(path, compiled.errors, source); return kExitCompile; }
  std::cout << g::disassemble(*compiled.script);
  return kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(__GLIBC__)
  // A thread's heap grows a page at a time unless told otherwise, and a full feed spent a tenth of its time in
  // mprotect doing it (measured on chi-il-1): grow in 16 MB steps. The pad is also what each thread's heap keeps
  // when it is done (64 MB held 1.4 GB on chi's app host after load, 16 MB 240 MB, for feeds 10% slower; see
  // cli/main.cpp). MALLOC_TOP_PAD_ in the environment still wins.
  if (!std::getenv("MALLOC_TOP_PAD_")) mallopt(M_TOP_PAD, 16 << 20);
#endif
  g::set_http_trust_root(grain_cli::state_dir());
  g::set_native_policy({/*allow=*/true, /*unsafe=*/true});   // the standalone grain trusts its own programs
#if defined(GRAIN_HAVE_WIN32)
  g::set_display(&grain_cli::win32_display());   // run_panes() opens real windows
#elif defined(GRAIN_HAVE_X11)
  g::set_display(&grain_cli::x11_display());   // run_panes() opens real windows
#elif defined(GRAIN_HAVE_SDL2)
  g::set_display(&grain_cli::sdl_display());   // run_panes() opens real windows
#endif
  std::vector<std::string> a(argv + 1, argv + argc);
  {
    grain_cli::Pack pack;
    std::string why;
    if (grain_cli::find_own_pack(pack, why)) return run_pack(pack, a);
    if (!why.empty()) { std::cerr << argv[0] << ": " << why << "\n"; return kExitRuntime; }
  }
  Options opt;
  size_t i = 0;
  for (; i < a.size() && a[i].rfind("--", 0) == 0; ++i) {
    if (a[i] == "--steps" && i + 1 < a.size()) {
      char* end = nullptr;
      opt.steps = std::strtoull(a[++i].c_str(), &end, 10);
      if (!end || *end) { std::cerr << "grain: --steps wants a number\n"; return kExitUsage; }
    } else if (a[i] == "--no-jit") {
      opt.jit = false;
    } else if (a[i] == "--safe") {
      g::set_native_policy({/*allow=*/true, /*unsafe=*/false});
      opt.safe = true;
    } else if (a[i] == "--no-native") {
      g::set_native_policy({/*allow=*/false, /*unsafe=*/false});
      opt.safe = true;
    } else if (a[i] == "--help") {
      usage(std::cout);
      return kExitOk;
    } else if (a[i] == "--version") {
      std::cout << "grain " GRAIN_VERSION "\n";
      return kExitOk;
    } else {
      std::cerr << "grain: unknown option " << a[i] << "\n";
      usage(std::cerr);
      return kExitUsage;
    }
  }
  if (i >= a.size()) { usage(std::cerr); return kExitUsage; }
  const std::string sub = a[i];
  const std::vector<std::string> rest(a.begin() + static_cast<long>(i) + 1, a.end());

  if (sub == "help" || sub == "-h") { usage(std::cout); return kExitOk; }
  if (sub == "version") { std::cout << "grain " GRAIN_VERSION "\n"; return kExitOk; }
  if (sub == "repl") return repl(opt);
  if (sub == "run") {
    if (rest.empty()) { std::cerr << "grain run <file.gr> [args...]\n"; return kExitUsage; }
    return run_file(opt, rest[0], std::vector<std::string>(rest.begin() + 1, rest.end()));
  }
  if (sub == "eval") {
    if (rest.empty()) { std::cerr << "grain eval <source...>\n"; return kExitUsage; }
    std::string source;
    for (size_t k = 0; k < rest.size(); ++k) { if (k) source += " "; source += rest[k]; }
    return eval_line(opt, source);
  }
  if (sub == "open") {   // grain open <url>: a remote ui.pane program in a native window
    if (rest.empty()) { std::cerr << "grain: open wants the URL of a grain app\n"; return kExitUsage; }
    auto compiled = g::compile_source(grain_cli::kOpenClientSource, "<open>");
    if (!compiled.ok()) { report_compile("<open>", compiled.errors); return kExitCompile; }
    g::Vm vm;
    DropPanes drop{vm};
    prepare(vm, opt, rest);
    auto r = vm.run(compiled.script);
    if (!r.ok) { report_runtime("<open>", r, grain_cli::kOpenClientSource); return kExitRuntime; }
    return kExitOk;
  }
  if (sub == "serve") {   // grain serve app.gr [--port N] [--host H]: the program's ui.pane in a browser
    if (rest.empty()) { std::cerr << "grain: serve wants a program\n"; return kExitUsage; }
    const std::string path = rest[0];
    return serve_program(path,
        [path](std::string& source, std::string& version, std::string& error) {
          if (!read_source(path, source)) { error = "cannot read " + path; return false; }
#if defined(_WIN32)
          version = std::to_string(source.size()) + ":" + std::to_string(std::hash<std::string>()(source));
#else
          struct stat st;
          version = ::stat(path.c_str(), &st) == 0 ? std::to_string((long long)st.st_mtime) + ":" + std::to_string((long long)st.st_size) : std::to_string(source.size());
#endif
          return true;
        },
        [opt, path](g::Vm& vm) { prepare(vm, opt, {}); vm.base_dir = dir_of(path); },
        std::vector<std::string>(rest.begin() + 1, rest.end()));
  }
  if (sub == "check") {
    if (rest.empty()) { std::cerr << "grain check <file.gr>...\n"; return kExitUsage; }
    return check_files(rest);
  }
  if (sub == "build") return grain_cli::build_command(rest);
  if (sub == "test") return grain_cli::test_command(rest);
  if (sub == "fmt") return grain_cli::fmt_command(rest);
  if (sub == "lsp") return grain_cli::lsp_command(rest);
  if (sub == "host") return grain_cli::host_command(rest);
  if (sub == "app") return grain_cli::app_command(rest);
  if (sub == "pkg") return grain_cli::pkg_command(rest);
  if (sub == "debug") return grain_cli::debug_command(rest);
  if (sub == "dis") {
    if (rest.size() != 1) { std::cerr << "grain dis <file.gr>\n"; return kExitUsage; }
    return disassemble(rest[0]);
  }
  // `grain prog.gr args...`: a file is run, the way an interpreter is used from a shebang line.
  if (sub.size() > 3 && sub.compare(sub.size() - 3, 3, ".gr") == 0) return run_file(opt, sub, rest);
  std::cerr << "grain: unknown command " << sub << "\n";
  usage(std::cerr);
  return kExitUsage;
}
