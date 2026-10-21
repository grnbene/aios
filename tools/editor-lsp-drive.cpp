// Drives the editor's language-server support through the compositor's own
// input path, against whatever REAL server is on PATH, and checks each answer.
//
//   aios-editor-lsp-drive [out.ppm]
//
// It used to talk to an in-process stand-in that made every answer up, set its
// own "diagnostics" by hand, and printed PROVEN whatever happened. Now:
//   * with no server for a language, the editor must say so (and offer nothing);
//   * with pylsp on PATH, a real Python file is opened in the editor and the
//     drive checks real diagnostics, a real hover, a real go-to-definition, and
//     that an edit reaches the server (didChange) and changes what it reports.
// Exit 0 only if every check that could run passed AND a real server was
// exercised; 1 if a check failed; 2 if no server was available to prove anything.
#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"
#include "aios/userland/compositor.hpp"
#include "aios/userland/lsp_client.hpp"
#include "aios/hal/keys.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

using namespace aios;
using namespace aios::userland;

namespace {

int g_fail = 0, g_pass = 0;
void check(bool ok, const std::string& what, const std::string& detail = "") {
  std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.empty() ? "" : " -- ", detail.c_str());
  (ok ? g_pass : g_fail)++;
}

void save_frame(HardwareCompositor& desk, Kernel& k, const std::string& path) {
  desk.render_desktop(k);
  const auto& fb = desk.framebuffer();
  std::ofstream out(path, std::ios::binary);
  out << "P6\n1280 800\n255\n";
  for (size_t i = 0; i < fb.size(); ++i) {
    uint32_t p = fb[i];
    char rgb[3] = {static_cast<char>((p >> 16) & 0xFF), static_cast<char>((p >> 8) & 0xFF), static_cast<char>(p & 0xFF)};
    out.write(rgb, 3);
  }
}

// Render frames (the editor polls its server once per frame) until `done`.
template <typename F>
bool pump(HardwareCompositor& desk, Kernel& k, F done, int max_ms) {
  for (int t = 0; t < max_ms; t += 20) {
    desk.render_desktop(k);
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string out_ppm = argc > 1 ? argv[1] : "editor_lsp_drive_output.ppm";
  std::printf("=== AIOS editor language-server drive ===\n");

  char root_tmpl[] = "/tmp/aios-lsp-drive-XXXXXX";
  if (!mkdtemp(root_tmpl)) { std::perror("mkdtemp"); return 1; }
  const std::string root = root_tmpl;

  KernelConfig cfg;
  cfg.root = root + "/root";
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::fprintf(stderr, "Kernel boot failed: %s\n", k.error().str().c_str()); return 1; }
  Kernel& kernel = **k;
  AuthorityProfile profile;
  auto me = kernel.resident("lsp-drive", profile);
  if (!me.ok()) { std::fprintf(stderr, "resident: %s\n", me.error().str().c_str()); return 1; }
  Surface console = kernel.as(*me);

  HardwareCompositor desk(1280, 800);
  desk.set_fullscreen(false);
  desk.set_console(&console);
  desk.init_default_workspace(kernel);
  // Raise the editor the way F2 does: this also leaves the canvas face, where
  // no pane is painted (and so no pane would ever poll its server).
  desk.run_ui_action(UiAction::PaneEditor);
  uint32_t win_id = 0;
  for (const auto& w : desk.windows()) if (w.type == WindowType::Editor) win_id = w.window_id;
  if (!win_id) { std::fprintf(stderr, "no editor window\n"); return 1; }
  desk.bring_to_front(win_id);
  auto W = [&]() { return desk.find_window(win_id); };

  // --- 1. A language with no server on PATH: the editor says so -------------
  std::printf("\n[1] C++ with %s\n", LspClientManager::server_for(SourceLang::Cpp).c_str());
  {
    const std::string cpp = root + "/a.cpp";
    std::ofstream(cpp) << "int add(int a, int b) { return a + b; }\nint main() { return add(1, 2); }\n";
    desk.editor_open(*W(), kernel, cpp);
    desk.render_desktop(kernel);
    const bool have = LspClientManager::instance().get_or_start_client(SourceLang::Cpp, root) != nullptr;
    W()->cursor_line = 1; W()->cursor_col = 22;
    desk.input_key(hal::KeyF1);
    const std::string st = W()->status_text;
    if (!have) {
      check(!W()->lsp_client, "no C++ client is attached when no server exists");
      check(st.find("no language server for") != std::string::npos, "F1 says there is no language server", st);
      check(!W()->lsp_hover_active && W()->diagnostics.empty(), "nothing is offered in its place");
    } else {
      std::printf("  (clangd is on PATH; the no-server case is shown with Python below if pylsp is absent)\n");
    }
  }

  // --- 2. Python against a real pylsp ----------------------------------------
  const std::string py_server = LspClientManager::server_for(SourceLang::Python);
  bool proved_real = false;
  std::printf("\n[2] Python with %s\n", py_server.c_str());
  {
    const std::string py = root + "/m.py";
    std::ofstream(py) << "import os\n"
                         "\n"
                         "def greet(name):\n"
                         "    \"\"\"Say hello to NAME, politely.\"\"\"\n"
                         "    return 'hello ' + name\n"
                         "\n"
                         "greet('x')\n";
    desk.editor_open(*W(), kernel, py);
    desk.render_desktop(kernel);
    const bool have_py = LspClientManager::instance().get_or_start_client(SourceLang::Python, root) != nullptr;
    if (!have_py) {
      W()->cursor_line = 6; W()->cursor_col = 1;
      desk.input_key(hal::KeyF12);
      const std::string st = W()->status_text;
      check(st.find("no language server for") != std::string::npos, "F12 says there is no language server", st);
      check(W()->cursor_line == 6, "the cursor did not move to an invented definition");
    } else {
      proved_real = true;
      bool attached = pump(desk, kernel, [&] { return W()->lsp_client != nullptr; }, 3000);
      check(attached, "the editor attaches to the server when the file is opened (no keypress needed)");
      // Real diagnostics: pyflakes says `os` is imported and unused.
      bool got = pump(desk, kernel, [&] { return !W()->diagnostics.empty(); }, 15000);
      std::string first = got ? W()->diagnostics[0].message : "";
      check(got && first.find("os") != std::string::npos, "server published a diagnostic for the unused import", first);

      // Hover on `greet` in the call: the docstring comes back from the server.
      W()->cursor_line = 6; W()->cursor_col = 2;
      desk.input_key(hal::KeyF1);
      // Open more windows while the answer is in flight: windows_ reallocates,
      // and an answer written through a reference taken before would land in
      // freed memory. The answer must find the editor by its id.
      for (int i = 0; i < 12; ++i) (void)desk.create_window("filler", 10 + i, 10 + i, 200, 120, WindowType::Terminal);
      desk.bring_to_front(win_id);
      got = pump(desk, kernel, [&] { return W()->lsp_hover_active; }, 10000);
      check(got && W()->lsp_hover_text.find("Say hello to NAME") != std::string::npos,
            "F1 hover shows greet's real docstring", W()->lsp_hover_text.substr(0, 80));

      // Go to definition: from the call on line 7 to `def greet` on line 3.
      W()->lsp_hover_active = false;
      W()->cursor_line = 6; W()->cursor_col = 2;
      desk.input_key(hal::KeyF12);
      got = pump(desk, kernel, [&] { return W()->cursor_line == 2; }, 10000);
      check(got, "F12 jumps to `def greet` (line 3)", "cursor line " + std::to_string(W()->cursor_line + 1) + ", status: " + W()->status_text);

      // An edit reaches the server: type a use of an undefined name on a new
      // last line; the server can only report it if didChange was sent.
      W()->cursor_line = (int)W()->lines.size() - 1;
      W()->cursor_col = (int)W()->lines.back().size();
      desk.input_key(hal::KeyEnter);
      for (char c : std::string("undefined_thing")) desk.input_key(c);
      W()->lsp_completion_active = false;
      got = pump(desk, kernel, [&] {
        for (const auto& d : W()->diagnostics)
          if (d.message.find("undefined_thing") != std::string::npos) return true;
        return false;
      }, 15000);
      check(got, "typing reaches the server: it now reports `undefined_thing`");

      // Opening another file detaches from this one: no stale client, no
      // stale diagnostics carried into a file the server never saw.
      desk.editor_open(*W(), kernel, root + "/a.cpp");
      desk.render_desktop(kernel);
      check(!W()->lsp_client && W()->diagnostics.empty(),
            "opening a.cpp drops m.py's client and diagnostics",
            std::to_string(W()->diagnostics.size()) + " diagnostics left");
    }
  }

  save_frame(desk, kernel, out_ppm);
  std::printf("\nframe: %s\n", out_ppm.c_str());
  LspClientManager::instance().shutdown_all();
  std::printf("%d passed, %d failed\n", g_pass, g_fail);
  if (g_fail) { std::printf("FAILED\n"); return 1; }
  if (!proved_real) {
    std::printf("NOT PROVEN: no language server on PATH, so only the no-server behaviour was checked.\n"
                "Put clangd or pylsp on PATH to prove the protocol against a real server.\n");
    return 2;
  }
  std::printf("PROVEN against a real %s\n", py_server.c_str());
  return 0;
}
