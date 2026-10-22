// Two VMs in one process, as inside aios (a desktop app and `aios grain run`):
// each must see only its own panes, and neither may close or draw the other's.
#include <cstdio>
#include <string>

#include "aios/userland/grain.hpp"

namespace g = aios::userland::grain;

static int failures = 0;
static void check(const char* what, bool ok) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++failures;
}

static g::Vm::RunResult run(g::Vm& vm, const std::string& src) {
  auto c = g::compile_source(src, "<check>");
  if (!c.ok()) { std::printf("does not compile: %s\n", c.errors[0].message.c_str()); return {}; }
  return vm.run(c.script);
}

int main() {
  g::Vm a, b;
  g::install_core_builtins(a);
  g::install_window_builtins(a);
  g::install_core_builtins(b);
  g::install_window_builtins(b);
  run(a, "let drawn = 0\nlet pa = create_pane({\"title\": \"A\", \"width\": 10, \"height\": 10, \"on_draw\": fn (cv) { drawn = drawn + 1 }})");
  run(b, "let pb = create_pane({\"title\": \"B\", \"width\": 20, \"height\": 20})");
  const auto la = g::pane_list(a), lb = g::pane_list(b);
  check("each VM lists only its own pane", la.size() == 1 && lb.size() == 1 && la[0].title == "A" && lb[0].title == "B");
  const uint64_t ida = la[0].id;
  // B asks for A's pane by its id: not B's to close.
  auto r = run(b, "unregister_pane(" + std::to_string(ida) + ")");
  check("a VM cannot close another's pane", !r.ok && g::pane_list(a).size() == 1);
  // Drawing A's pane through B is refused; through A it runs A's callback.
  std::vector<uint32_t> px;
  int w = 0, h = 0;
  std::string err;
  check("a VM cannot draw another's pane", !g::pane_frame(b, ida, px, w, h, err));
  err.clear();
  check("its own VM draws it", g::pane_frame(a, ida, px, w, h, err) && w == 10 && h == 10);
  check("with its own callback", a.find_global("drawn") && a.find_global("drawn")->as_int() == 1);
  r = run(a, "unregister_pane(pa.id)");
  check("its own VM can close it", r.ok && g::pane_list(a).empty() && g::pane_list(b).size() == 1);
  std::printf("%s\n", failures ? "pane isolation: FAILED" : "pane isolation: all agreed");
  return failures ? 1 : 0;
}
