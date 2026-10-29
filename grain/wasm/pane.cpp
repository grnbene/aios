// A ui.pane in a page: the program's own `view` and actions run in this VM, and what it draws comes out as the HTML
// fragment the server's page would show - the same web_pane_* calls grain_webui.cpp makes, without the HTTP.
//
//   grain_pane_open(main, query)  runs the program at `main` (its files are on the page's file system), calls the pane's
//                                 on_open with the page's query, and answers the first frame
//   grain_pane_event(body)        an action: `body` is what the page script sends (action=<id>&f.<field>=<text>...)
//   grain_pane_tick()             timers and the pane's "refresh": answers a frame when the view changed, else
//                                 {"changed": false}
//   grain_pane_close()            drops the VM
//
// Every answer is JSON. A frame is {"ok": true, "seq", "version", "html", "log": [lines printed since the last answer]},
// the first also "title", "css", "theme", "width", "refresh", "shared". A failure is {"ok": false, "error", "line"}.
// The VM is kept between calls: a pane's state is the program's variables, as on the server.
#include "browser_vm.hpp"

namespace {

using namespace browser;

constexpr uint64_t kStepsPerCall = 50'000'000;   // a runaway view or action ends with an error instead of freezing the page

struct Pane {
  std::unique_ptr<Vm> vm;
  WebPaneInfo info;
  std::vector<std::string> printed;
  std::string html;
  uint64_t seq = 0;
  double refreshed_ms = 0;
};
std::unique_ptr<Pane> g_pane;

std::string fail(const std::string& error, int line = 0) {
  std::string out = "{\"ok\":false,\"error\":";
  json_string(out, error);
  return out + ",\"line\":" + std::to_string(line) + "}";
}

// The lines the program printed since the last answer, as a JSON array.
std::string log_json() {
  std::string out = "[";
  for (size_t i = 0; i < g_pane->printed.size(); ++i) {
    if (i) out += ',';
    json_string(out, g_pane->printed[i]);
  }
  g_pane->printed.clear();
  return out + "]";
}

std::string unescape(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '+') out += ' ';
    else if (s[i] == '%' && i + 2 < s.size() + 0 && std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2])) {
      out += (char)std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
      i += 2;
    } else out += s[i];
  }
  return out;
}

// Calls the view and keeps its HTML; true when it differs from the last one (then seq moves).
bool render(std::string& error) {
  std::string html;
  uint64_t version = 0;
  g_pane->vm->steps = 0;
  if (!web_pane_render(*g_pane->vm, g_pane->info.id, html, version, error)) return false;
  if (html != g_pane->html) { g_pane->html = std::move(html); ++g_pane->seq; return true; }
  return false;
}

std::string frame(bool first) {
  std::string out = "{\"ok\":true,\"seq\":" + std::to_string(g_pane->seq) + ",\"version\":\"wasm\",\"html\":";
  json_string(out, g_pane->html);
  if (first) {
    const auto& i = g_pane->info;
    out += ",\"title\":";
    json_string(out, i.title);
    out += ",\"css\":";
    json_string(out, i.css);
    out += ",\"theme\":";
    json_string(out, i.theme);
    out += ",\"width\":" + std::to_string(i.width) + ",\"refresh\":" + std::to_string(i.refresh) + ",\"shared\":" + (i.shared ? "true" : "false");
  }
  return out + ",\"log\":" + log_json() + "}";
}

}  // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE char* grain_pane_open(const char* main_path, const char* query) {
  g_pane.reset();
  std::string source, error;
  if (!g_files.read(main_path ? main_path : "", source, error)) return give(fail(std::string(main_path ? main_path : "") + ": " + error));
  auto compiled = compile_source(source, main_path, false);
  if (!compiled.ok()) {
    const auto& d = compiled.errors.empty() ? Diagnostic(0, "compile error") : compiled.errors[0];
    return give(fail(std::string(main_path) + ": " + d.message, d.line));
  }
  auto pane = std::make_unique<Pane>();
  pane->vm = std::make_unique<Vm>();
  set_up(*pane->vm, &pane->printed);
  pane->vm->step_limit = kStepsPerCall;
  const std::string path = main_path;
  const size_t slash = path.rfind('/');
  pane->vm->base_dir = slash == std::string::npos ? "" : path.substr(0, slash);   // where `import` looks
  // A program that is also a window calls run_panes(); here the page is the window, so it returns at once.
  pane->vm->define_native("run_panes", 0, 1, [](Vm&, std::vector<Value>&, std::string&) { return Value::nil(); });
  auto run = pane->vm->run(compiled.script);
  if (!run.ok) return give(fail("the program failed: " + run.error, run.line));
  if (!web_pane_first(*pane->vm, pane->info)) return give(fail("the program made no ui.pane with a view (a pane it draws by hand is not shown in a browser)"));
  pane->refreshed_ms = wall_ms();
  g_pane = std::move(pane);
  std::string err;
  if (!web_pane_on_open(*g_pane->vm, g_pane->info.id, query ? query : "", err)) g_pane->printed.push_back("on_open: " + err);
  if (!render(err) && g_pane->html.empty()) {
    const std::string why = err.empty() ? "the view drew nothing" : err;
    g_pane.reset();
    return give(fail("the pane's first frame: " + why));
  }
  return give(frame(true));
}

// `body` as the page script sends it: urlencoded pairs, the button or field as `action`, the page's fields as `f.<id>`.
EMSCRIPTEN_KEEPALIVE char* grain_pane_event(const char* body) {
  if (!g_pane) return give(fail("no pane is open"));
  std::string action;
  std::vector<std::pair<std::string, std::string>> fields;
  const std::string text = body ? body : "";
  for (size_t at = 0; at <= text.size();) {
    size_t amp = text.find('&', at);
    if (amp == std::string::npos) amp = text.size();
    const std::string pair = text.substr(at, amp - at);
    const size_t eq = pair.find('=');
    if (eq != std::string::npos) {
      const std::string key = unescape(pair.substr(0, eq)), value = unescape(pair.substr(eq + 1));
      if (key == "action") action = value;
      else if (key.rfind("f.", 0) == 0) fields.push_back({key.substr(2), value});
    }
    at = amp + 1;
  }
  std::string err;
  g_pane->vm->steps = 0;
  const bool ok = web_pane_event(*g_pane->vm, g_pane->info.id, 0, action, fields, err);
  std::string render_err;
  render(render_err);
  std::string out = frame(false);
  if (!ok || !render_err.empty()) {
    // the answer still carries the view; what went wrong goes with it, as the server's does
    out.pop_back();
    out += ",\"error\":";
    json_string(out, ok ? render_err : err);
    out += "}";
  }
  return give(out);
}

EMSCRIPTEN_KEEPALIVE char* grain_pane_tick() {
  if (!g_pane) return give(fail("no pane is open"));
  std::string err;
  g_pane->vm->steps = 0;
  bool look = run_timers(*g_pane->vm, err);
  if (g_pane->info.refresh > 0 && wall_ms() - g_pane->refreshed_ms >= g_pane->info.refresh * 1000.0) {
    look = true;
    g_pane->refreshed_ms = wall_ms();
  }
  if (look) {
    std::string render_err;
    if (render(render_err)) return give(frame(false));
    if (!render_err.empty()) g_pane->printed.push_back("view: " + render_err);
  }
  if (!err.empty()) g_pane->printed.push_back("timer: " + err);
  std::string out = "{\"ok\":true,\"changed\":false,\"log\":" + log_json() + "}";
  return give(out);
}

EMSCRIPTEN_KEEPALIVE void grain_pane_close() { g_pane.reset(); }

}  // extern "C"
