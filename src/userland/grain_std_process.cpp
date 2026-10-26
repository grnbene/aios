// process_run: another program, by argument list (no shell: nothing in an argument is ever parsed as a command),
// through the hal's process facility (hal::run_tool: spawn, wait, process-group kill on timeout; POSIX and Windows).
// Off unless the host allows it (Vm::allow_process): a script run from the command line may; a hosted app only when
// the machine's operator names it in AIOS_APP_PROCESS_RUN; a page aios-webserver serves never. The child starts
// from a clean environment (PATH, LANG and what the call gives), so the host's own variables - keys among them - do
// not reach it unless inherit_env says so.
#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

#include "aios/hal/process.hpp"
#include "aios/userland/grain.hpp"
#include "grain_builtins_internal.hpp"

namespace aios::userland::grain {

namespace {

constexpr int64_t kDefaultTimeoutMs = 60000, kMaxTimeoutMs = 24LL * 3600 * 1000;
constexpr int64_t kDefaultMaxOutput = 16LL << 20;

bool env_name_ok(const std::string& k) {
  if (k.empty()) return false;
  for (unsigned char c : k) if (!(std::isalnum(c) || c == '_')) return false;
  return true;
}

// process_run(argv [, {cwd, env, inherit_env, timeout_ms, max_output}])
//   -> {ok, code, output, timed_out, truncated}; output is stdout and stderr together, in the order written.
Value bi_process_run(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!vm.allow_process) {
    error = "process_run(): not allowed here - a hosted app needs the machine's operator to name it in "
            "AIOS_APP_PROCESS_RUN (the app's own settings cannot)";
    return Value::nil();
  }
  if (args[0].type != Type::List) { error = "process_run() wants the program and its arguments as a list of strings"; return Value::nil(); }
  hal::ProcessSpawn spec;
  for (const Value& v : args[0].list_items()) {
    if (v.type != Type::Str) { error = std::string("process_run(): every argument is a string, found a ") + type_name(v.type); return Value::nil(); }
    if (v.str().find('\0') != std::string::npos) { error = "process_run(): an argument holds a NUL byte"; return Value::nil(); }
    spec.argv.push_back(v.str());
  }
  if (spec.argv.empty() || spec.argv[0].empty()) { error = "process_run(): name the program to run"; return Value::nil(); }

  int64_t timeout_ms = kDefaultTimeoutMs, max_output = kDefaultMaxOutput;
  bool inherit = false;
#if defined(_WIN32)
  inherit = true;   // a Windows program needs SystemRoot and friends; there is no small clean set
#endif
  std::vector<std::pair<std::string, std::string>> extra;
  if (args.size() > 1 && args[1].type != Type::Nil) {
    if (args[1].type != Type::Map) { error = "process_run(): options are a map"; return Value::nil(); }
    MapObj& o = *static_cast<MapObj*>(args[1].obj.get());
    if (Value* v = o.find("cwd")) {
      if (!want_str(*v, error, "process_run")) return Value::nil();
      spec.working_dir = v->str();
    }
    if (Value* v = o.find("inherit_env")) inherit = v->truthy();
    if (Value* v = o.find("env")) {
      if (v->type != Type::Map) { error = "process_run(): env is a map of name to value"; return Value::nil(); }
      for (const auto& [k, val] : static_cast<MapObj*>(v->obj.get())->entries) {
        if (!env_name_ok(k)) { error = "process_run(): \"" + k + "\" is not an environment variable name"; return Value::nil(); }
        extra.emplace_back(k, val.type == Type::Str ? val.str() : val.to_text());
      }
    }
    if (Value* v = o.find("timeout_ms")) {
      if (!want_number(*v, error, "process_run")) return Value::nil();
      if (v->as_int() <= 0 || v->as_int() > kMaxTimeoutMs) { error = "process_run(): timeout_ms is 1 to 86400000"; return Value::nil(); }
      timeout_ms = v->as_int();
    }
    if (Value* v = o.find("max_output")) {
      if (!want_number(*v, error, "process_run")) return Value::nil();
      if (v->as_int() <= 0) { error = "process_run(): max_output must be positive"; return Value::nil(); }
      max_output = v->as_int();
    }
  }
  if (!inherit) {
    spec.clear_env = true;
    spec.env.emplace_back("PATH", "/usr/local/bin:/usr/bin:/bin");
    spec.env.emplace_back("LANG", "C.UTF-8");
  }
  for (auto& kv : extra) {
    auto same = [&](const auto& e) { return e.first == kv.first; };
    spec.env.erase(std::remove_if(spec.env.begin(), spec.env.end(), same), spec.env.end());
    spec.env.push_back(std::move(kv));
  }

  auto out = std::make_shared<MapObj>();
  auto r = hal::run_tool(spec, static_cast<uint32_t>(timeout_ms));
  if (!r.ok()) {
    if (r.error().status == Status::Busy) {   // run_tool: it did not finish in time, and was killed
      out->put("ok", Value::boolean(false));
      out->put("code", Value::integer(-1));
      out->put("output", Value::string(""));
      out->put("timed_out", Value::boolean(true));
      out->put("truncated", Value::boolean(false));
      return Value::object(Type::Map, out);
    }
    error = "process_run(" + spec.argv[0] + "): " + r.error().message;
    return Value::nil();
  }
  std::string text = std::move(r->output);
  const bool truncated = static_cast<int64_t>(text.size()) > max_output;
  if (truncated) text.resize(static_cast<size_t>(max_output));
  out->put("ok", Value::boolean(r->exit_code == 0));
  out->put("code", Value::integer(r->exit_code));
  out->put("output", Value::string(std::move(text)));
  out->put("timed_out", Value::boolean(false));
  out->put("truncated", Value::boolean(truncated));
  return Value::object(Type::Map, out);
}

}  // namespace

void install_process_builtins(Vm& vm) {
  vm.define_native("process_run", 1, 2, bi_process_run);
}

}  // namespace aios::userland::grain
