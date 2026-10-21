// Host capabilities for grain: sockets (the shared hal::net ones,
// grain_sockets.cpp) and sys_exec - a shell command through popen.
//
// Not in install_builtins(): the sidecar's sandboxed aios_grain_run must not
// be able to run commands. `aios grain run/eval/repl/serve` on a hosted
// machine installs these - there the script is the user's own. (The Pi has
// its own sys_exec: a kernel console command - metal/arm64/grain_shell.cpp.)
#include "grain_host.hpp"

#if defined(AIOS_METAL)
namespace aios::cli {
void install_grain_host(userland::grain::Vm&) {}
}
#else
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "aios/userland/grain.hpp"

namespace aios::cli {

namespace {

using namespace aios::userland::grain;

bool want_str(const Value& v, std::string& error, const char* who) {
  if (v.type == Type::Str) return true;
  error = std::string(who) + "() wants a string, got a " + type_name(v.type);
  return false;
}

Value bi_sys_exec(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "sys_exec")) return Value::nil();
  const std::string& cmd = args[0].str();
  FILE* fp = popen(cmd.c_str(), "r");
  if (!fp) {
    error = "sys_exec(): popen failed";
    return Value::nil();
  }
  std::string output;
  char buf[4096];
  while (size_t n = fread(buf, 1, sizeof(buf), fp)) {
    output.append(buf, n);
  }
  int status = pclose(fp);
  int code = WIFEXITED(status) ? WEXITSTATUS(status) : status;
  auto out = std::make_shared<MapObj>();
  out->put("code", Value::integer(code));
  out->put("output", Value::string(std::move(output)));
  out->put("ok", Value::boolean(code == 0));
  return Value::object(Type::Map, out);
}

}  // namespace

void install_grain_host(Vm& vm) {
  install_socket_builtins(vm);             // udp_*/tcp_* over hal::net - shared with the Pi
  install_window_builtins(vm);
  vm.define_native("sys_exec", 1, 1, bi_sys_exec);
}


}  // namespace aios::cli
#endif
