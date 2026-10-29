// Grain in a browser: the language compiled to WebAssembly with Emscripten, driven from JavaScript.
//
// One entry point runs a program and answers with JSON: what it printed, what its last expression was, or the
// compile or run error with its line. The VM is the one the `grain` program runs - the same sources - with the
// portable standard library and no host that a page does not have (no processes, sockets or database driver).
#include "browser_vm.hpp"

using namespace browser;

extern "C" {

// Runs `source` (named `origin` in errors); `step_limit` 0 is no limit. The answer is a JSON object:
//   {"ok": true,  "output": [printed lines], "value": "last expression as text"}
//   {"ok": false, "output": [...], "error": "...", "line": n, "column": n, "stage": "compile" | "run"}
// The caller frees it with grain_free.
EMSCRIPTEN_KEEPALIVE char* grain_run(const char* source, const char* origin, double step_limit) {
  std::string out;
  std::vector<std::string> printed;
  auto lines = [&]() {
    out += "\"output\":[";
    for (size_t i = 0; i < printed.size(); ++i) { if (i) out += ','; json_string(out, printed[i]); }
    out += ']';
  };
  auto compiled = compile_source(source ? source : "", origin && *origin ? origin : "program", true);
  if (!compiled.ok()) {
    const auto& d = compiled.errors.empty() ? Diagnostic(0, "compile error") : compiled.errors[0];
    out = "{\"ok\":false,\"stage\":\"compile\",";
    lines();
    out += ",\"error\":";
    json_string(out, d.message);
    out += ",\"line\":" + std::to_string(d.line) + ",\"column\":" + std::to_string(d.column) + "}";
    return give(out);
  }
  Vm vm;
  set_up(vm, &printed);
  vm.step_limit = step_limit > 0 ? (uint64_t)step_limit : 0;
  auto res = vm.run(compiled.script);
  if (!res.ok) {
    out = "{\"ok\":false,\"stage\":\"run\",";
    lines();
    out += ",\"error\":";
    json_string(out, res.error);
    out += ",\"line\":" + std::to_string(res.line) + ",\"column\":" + std::to_string(res.column) + "}";
    return give(out);
  }
  out = "{\"ok\":true,";
  lines();
  out += ",\"value\":";
  json_string(out, res.value.to_text());
  out += "}";
  return give(out);
}

EMSCRIPTEN_KEEPALIVE void grain_free(char* p) { std::free(p); }

}  // extern "C"
