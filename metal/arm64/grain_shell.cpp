/* grain, reachable from this board's own console (metal/arm64/grain_shell.cpp).
 *
 * grain (src/userland/grain_{lexer,parser,compiler,vm,builtins}.cpp) is
 * already compiled into this image - it is part of the ordinary src/ sweep
 * build-arm64.sh already does, same as compositor.cpp was before
 * desktop_shell.cpp gave it a way in. cli/grain_cmd.cpp is the seam
 * that reaches it from the hosted CLI; this is the equivalent seam for
 * this board's own UDP console. Files are the SD card's FAT32 (8.3 names);
 * sockets are the kernel's own UDP table and TCP stack through hal::net;
 * sys_exec runs a console command. `grain repl` keeps one VM alive between
 * commands - how tools/grain_remote.py codes on the board remotely.
 *
 * The point: everything after this file exists needs a rebuild+reflash+
 * power-cycle to change. A grain script sent as `grain <source...>` over
 * the UDP console does not - it compiles and runs against the live
 * aios::Kernel in well under a second, and the loop that used to take
 * minutes (edit -> build-arm64.sh -> flash-rpi3.sh -> reseat the card ->
 * power-cycle -> wait for DHCP) is gone for anything expressible here. It
 * is not a replacement for that loop - nothing here can touch a driver, the
 * boot sequence, or main.cpp itself - but a lot of what actually got
 * iterated on today (app-level logic, one-off diagnostics, poking at
 * Kernel state) would have. */
#include "aios/kernel/kernel.hpp"
#include "aios/userland/grain.hpp"
#include "aios/userland/machine_sql.hpp"
#include "kernel_boot.hpp"
#include "kio.hpp"
#include "fat32.hpp"
#include "shell.hpp"
#include <memory>
#include <string>
#include <vector>

using aios::Kernel;
namespace grain = aios::userland::grain;

namespace {

/* Same shape as grain_cmd.cpp's install_machine() - sys.arch/cpus/memory/
 * kernel/uptime_ms/entities, read once so a script does not see the clock
 * move mid-comparison. */
void install_sys(grain::Vm& vm, Kernel& k) {
  auto sys = std::make_shared<grain::MapObj>();
  const auto& topo = k.host().topology();
  sys->put("arch", grain::Value::string(topo.arch));
  sys->put("host", grain::Value::string(topo.host_name));
  sys->put("cpus", grain::Value::integer((int64_t)topo.logical_cpus));
  sys->put("memory", grain::Value::integer((int64_t)topo.total_memory));
  sys->put("kernel", grain::Value::string(k.self().str()));
  sys->put("uptime_ms", grain::Value::real((double)k.uptime_ns() / 1e6));
  sys->put("entities", grain::Value::integer((int64_t)k.entities().count()));
  vm.define_global("sys", grain::Value::object(grain::Type::Map, sys));
}

/* grain's file builtins on the Pi: the SD card's FAT32 volume. Names are 8.3
 * (the card is written with short names only), paths may name existing
 * subdirectories, and a write replaces the file's whole cluster chain. */
struct SdFiles final : grain::Files {
  static bool ready(std::string& error) {
    if (fat32_status().mounted) return true;
    error = "no SD card mounted";
    return false;
  }
  bool read(const std::string& path, std::string& out, std::string& error) override {
    if (!ready(error)) return false;
    size_t size = 0;
    if (!fat32_file_size(path.c_str(), size)) { error = "no such file"; return false; }
    out.assign(size, '\0');
    size_t got = 0;
    if (!fat32_read_file(path.c_str(), out.data(), size, got) || got != size) {
      error = "SD card read failed";
      return false;
    }
    return true;
  }
  bool write(const std::string& path, const std::string& data, bool append, std::string& error) override {
    if (!ready(error)) return false;
    std::string all;
    if (append && exists(path)) {
      if (!read(path, all, error)) return false;
    }
    all += data;
    if (!fat32_write_file(path.c_str(), all.data(), all.size())) {
      error = "SD card write failed (8.3 name? existing directory? card full?)";
      return false;
    }
    return true;
  }
  bool list(const std::string& path, std::vector<grain::FileEntry>& out, std::string& error) override {
    if (!ready(error)) return false;
    static Fat32FileInfo entries[512];
    const size_t n = fat32_list(path.c_str(), entries, 512);
    for (size_t i = 0; i < n; ++i) out.push_back({entries[i].name, static_cast<int64_t>(entries[i].size_bytes), entries[i].is_directory});
    return true;
  }
  bool remove(const std::string& path, std::string& error) override {
    if (!ready(error)) return false;
    if (!exists(path)) { error = "no such file"; return false; }
    if (!fat32_delete_file(path.c_str())) { error = "SD card delete failed"; return false; }
    return true;
  }
  bool exists(const std::string& path) override {
    size_t size = 0;
    return fat32_status().mounted && fat32_file_size(path.c_str(), size);
  }
};

SdFiles g_sd_files;

/* sys_exec on the Pi: there are no processes, so a "command" is a console
 * command (`fs ls`, `cpu`, `net`, ...), run here with its output captured.
 * code is 127 for a command the console does not know, else 0. */
grain::Value pi_sys_exec(grain::Vm&, std::vector<grain::Value>& args, std::string& error) {
  if (args[0].type != grain::Type::Str) { error = std::string("sys_exec() wants a string, got a ") + grain::type_name(args[0].type); return grain::Value::nil(); }
  std::vector<char> buf(64 * 1024);
  kconsole_capture_start(buf.data(), buf.size());
  dispatch_shell_command(args[0].str().c_str());
  const size_t n = kconsole_capture_stop();
  std::string output(buf.data(), n < buf.size() ? n : buf.size() - 1);
  const bool unknown = output.size() >= 13 && output.compare(output.size() - 13, 13, "? try 'help'\n") == 0;
  auto out = std::make_shared<grain::MapObj>();
  out->put("code", grain::Value::integer(unknown ? 127 : 0));
  out->put("output", grain::Value::string(std::move(output)));
  out->put("ok", grain::Value::boolean(!unknown));
  return grain::Value::object(grain::Type::Map, out);
}

}  // namespace

namespace {
/* `grain jit on|off`: native code (default) or the interpreter, so the two can
 * be compared on the board itself. */
bool g_grain_jit = true;

/* Everything a grain VM on this board gets. */
void setup_vm(grain::Vm& vm) {
  grain::install_builtins(vm);
  grain::install_socket_builtins(vm);
  grain::install_window_builtins(vm);
  vm.define_native("sys_exec", 1, 1, pi_sys_exec);
  install_sys(vm, aios_arm64::kernel());
  aios::userland::install_sql_builtins(vm, aios_arm64::kernel());
  vm.files = &g_sd_files;
  vm.jit_enabled = g_grain_jit;
}


template <class Emit>
void run_on(grain::Vm& vm, const grain::CompileResult& compiled, const char* name, Emit&& emit) {
  std::vector<std::string> out_lines;
  vm.sink = &out_lines;
  auto r = vm.run(compiled.script);
  vm.sink = nullptr;
  for (const auto& line : out_lines) emit(line);
  if (!r.ok) {
    emit(std::string(name) + ":" + std::to_string(r.line) + ": " + r.error);
    for (size_t i = 0; i < r.trace.size(); ++i) emit(std::string(i == 0 ? "  in " : "  from ") + r.trace[i]);
  } else if (r.value.type != grain::Type::Nil) {
    emit(r.value.repr());
  }
}

/* The session: one VM that outlives a console command, so `let`, `fn` and
 * everything a loaded file defined are still there on the next line. (A
 * function a line defines is its own FnObj, held by its closure - the line's
 * top-level chunk can go once it has run, so a long session does not grow.)
 * `grain reset` starts over. */
struct Session {
  grain::Vm vm;
  unsigned lines = 0;
};
std::unique_ptr<Session> g_session;

Session& session() {
  if (!g_session) {
    g_session = std::make_unique<Session>();
    setup_vm(g_session->vm);
  }
  return *g_session;
}

void session_run(const std::string& source, const char* name, uint64_t step_limit) {
  Session& S = session();
  auto compiled = grain::compile_source(source, name, /*keep_last_value=*/true);
  auto emit = [](const std::string& line) { kprintf("  %s\n", line.c_str()); };
  if (!compiled.ok()) {
    for (const auto& d : compiled.errors) emit(std::string(name) + ":" + std::to_string(d.line) + ": " + d.message);
    return;
  }
  install_sys(S.vm, aios_arm64::kernel());        /* a fresh sys.uptime_ms each line */
  S.vm.jit_enabled = g_grain_jit;
  S.vm.step_limit = step_limit;
  S.vm.steps = 0;
  ++S.lines;
  run_on(S.vm, compiled, name, emit);
}

/* Compile and run one grain program against the live Kernel. Output lines go
 * to the console and, when out_name is given, to that file on the SD card
 * (so a long run's output cannot be lost in the UDP reply). */
void run_grain(const std::string& source, const char* name, const char* out_name) {
  auto compiled = grain::compile_source(source, name, /*keep_last_value=*/true);
  std::string out;
  auto emit = [&](const std::string& line) { kprintf("  %s\n", line.c_str()); out += line; out += '\n'; };
  if (!compiled.ok()) {
    for (const auto& d : compiled.errors) emit(std::string(name) + ":" + std::to_string(d.line) + ": " + d.message);
  } else {
    grain::Vm vm;
    setup_vm(vm);
    /* A script must not be able to hang the board. Files get more room than
     * a one-line console command. */
    vm.step_limit = out_name ? 200000000 : 10000000;
    run_on(vm, compiled, name, emit);
  }
  if (out_name && *out_name) {
    const bool ok = fat32_write_file(out_name, out.data(), out.size());
    kprintf("  [output %s %s, %u bytes]\n", ok ? "saved to" : "NOT saved to", out_name, static_cast<uint64_t>(out.size()));
  }
}
}  // namespace

extern "C" void grain_eval(const char* source) {
  while (*source == ' ') ++source;
  if (*source == '\0') {
    kputs("  grain: this machine's own language, live against the running Kernel - no rebuild, no reflash.\n"
          "  usage: grain <source...>        e.g. grain print(sys.uptime_ms)\n"
          "         grain run FILE [OUTFILE] run a program from the SD card (upload with `fs b64`)\n"
          "         grain repl <source>     run in the persistent session (globals survive)\n"
          "         grain load FILE         run a file in the session;  grain reset  starts it over\n"
          "         grain jit on|off / prof native code or interpreter / what falls back\n"
          "  let / fn / if / else / while / for..in / return / break / continue\n"
          "  int real string bool nil list map, closures, first-class functions\n"
          "  read_file/write_file/append_file/list_dir/delete_file/file_exists work on the SD card (8.3 names)\n"
          "  udp_*/tcp_* are the kernel's sockets; sys_exec(\"cmd\") runs a console command\n"
          "  `sys` holds what this board is: sys.arch, sys.cpus, sys.memory, sys.uptime_ms, sys.entities\n");
    return;
  }
  if (source[0] == 'p' && source[1] == 'r' && source[2] == 'o' && source[3] == 'f') {
    auto& p = grain::g_jit_profile;
    auto line = [](const char* what, uint64_t cyc, uint64_t n) {
      kprintf("  %-12s %u calls, %u cycles each\n", what, n, n ? cyc / n : 0);
    };
    kprintf("  %-12s %u (natives, first calls, arity/depth errors)\n", "slow calls", p.slow_calls);
    line("return", p.ret, p.ret_n);
    line("get_global", p.get_global, p.get_global_n);
    line("step", p.step, p.step_n);
    for (int op = 0; op < 64; ++op)
      if (p.step_by_op[op]) kprintf("    %-10s %u\n", grain::op_name(static_cast<grain::Op>(op)), p.step_by_op[op]);
    p = grain::JitProfile{};
    return;
  }
  if (source[0] == 'j' && source[1] == 'i' && source[2] == 't' && (source[3] == ' ' || source[3] == '\0')) {
    const char* p = source + 3;
    while (*p == ' ') ++p;
    if (p[0] == 'o' && p[1] == 'n') g_grain_jit = true;
    else if (p[0] == 'o' && p[1] == 'f') g_grain_jit = false;
    kprintf("  grain: %s\n", g_grain_jit ? "native code (AArch64 JIT)" : "interpreter");
    return;
  }
  auto word = [&](const char* w) {
    size_t i = 0;
    for (; w[i]; ++i) if (source[i] != w[i]) return false;
    return source[i] == ' ' || source[i] == '\0';
  };
  if (word("repl")) {                               /* grain repl <source>: in the session */
    const char* p = source + 4;
    while (*p == ' ') ++p;
    if (!*p) { kprintf("  grain session: %u lines run, %s\n", g_session ? g_session->lines : 0u, g_session ? "live" : "not started"); return; }
    session_run(p, "<repl>", 10000000);
    return;
  }
  if (word("load")) {                               /* grain load FILE: run a file in the session */
    const char* p = source + 4;
    while (*p == ' ') ++p;
    std::string file;
    while (*p && *p != ' ') file += *p++;
    std::string text, why;
    if (!g_sd_files.read(file, text, why)) { kprintf("  grain load %s: %s\n", file.c_str(), why.c_str()); return; }
    session_run(text, file.c_str(), 200000000);
    return;
  }
  if (word("reset")) {
    g_session.reset();
    kputs("  grain session: reset\n");
    return;
  }
  if (source[0] == 'r' && source[1] == 'u' && source[2] == 'n' && source[3] == ' ') {
    const char* p = source + 4;
    while (*p == ' ') ++p;
    std::string file, outf;
    while (*p && *p != ' ') file += *p++;
    while (*p == ' ') ++p;
    while (*p && *p != ' ') outf += *p++;
    static char buf[256 * 1024];
    size_t n = 0;
    if (!fat32_status().mounted) { kputs("  grain run: no SD card mounted\n"); return; }
    if (!fat32_read_file(file.c_str(), buf, sizeof(buf) - 1, n)) { kprintf("  grain run: cannot read %s\n", file.c_str()); return; }
    run_grain(std::string(buf, n), file.c_str(), outf.empty() ? nullptr : outf.c_str());
    return;
  }
  run_grain(source, "<console>", nullptr);
}

#include "timer.hpp"

/* `bench`: the calibration kernels in metal/bench_core.inc, the same source
 * metal/tools/bench-host.sh runs on the host. */
namespace {
uint64_t bench_now_ns() { return metal_now_ns(); }
void bench_report(const char* name, uint64_t ns, long check) {
  kprintf("  %-22s %u.%u ms  (check %d)\n", name, ns / 1000000, (ns / 100000) % 10, static_cast<long long>(check));
}
}  // namespace
#include "../bench_core.inc"
extern "C" void bench_run() { bench_core::run_all(); }
