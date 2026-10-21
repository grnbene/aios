// Drives the desktop's input path the way a person's hands do, and writes out
// what the screen actually showed.
//
// The compositor's input calls are the SAME ones the SDL backend and the
// bare-metal PS/2 driver feed; nothing here reaches around them. It exists
// because the alternative is asking somebody to sit in front of the machine
// and try the arrow keys.
//
//   aios-desktop-drive <root> <out-dir>
#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"
#include "aios/hal/clipboard.hpp"
#include "aios/hal/keys.hpp"
#include "aios/userland/compositor.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>
#include <string>
#include <vector>

using namespace aios;

namespace {

void write_ppm(const userland::HardwareCompositor& c, uint32_t w, uint32_t h, const std::string& path) {
  const auto& fb = c.framebuffer();
  std::ofstream out(path, std::ios::binary);
  out << "P6\n" << w << " " << h << "\n255\n";
  for (size_t i = 0; i < fb.size(); ++i) {
    uint32_t p = fb[i];
    char rgb[3] = {(char)((p >> 16) & 0xFF), (char)((p >> 8) & 0xFF), (char)(p & 0xFF)};
    out.write(rgb, 3);
  }
}

void type(userland::HardwareCompositor& c, const std::string& text) { c.input_text(text); }

// The caret blinks on a half-second cycle off the same clock the compositor
// reads, so a frame grabbed at random has a even chance of showing no caret at
// all. Wait for the lit half before painting the one we are going to look at.
void wait_for_caret() {
  for (;;) {
    uint64_t ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if ((ms / 500) % 2 == 0 && (ms % 500) < 400) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

void frame(userland::HardwareCompositor& c, Kernel& k, uint32_t w, uint32_t h, const std::string& path) {
  wait_for_caret();
  c.render_desktop(k);
  write_ppm(c, w, h, path);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: aios-desktop-drive <root> <out-dir>\n"); return 2; }
  const std::string root = argv[1];
  const std::string out = argv[2];

  KernelConfig cfg;
  cfg.root = root;
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::fprintf(stderr, "boot: %s\n", k.error().str().c_str()); return 1; }
  Kernel& kernel = **k;

  AuthorityProfile profile;
  auto me = kernel.resident("desktop-drive", profile);
  if (!me.ok()) { std::fprintf(stderr, "resident: %s\n", me.error().str().c_str()); return 1; }
  Surface console = kernel.as(*me);

  const uint32_t W = 1280, H = 800;
  userland::HardwareCompositor desk(W, H);
  desk.set_fullscreen(false);
  desk.set_console(&console);
  desk.set_dispatcher([](const std::string&, userland::OutputCallback) {});
  desk.init_default_workspace(kernel);

  // Leave the start page for the workspace — through the right-click menu it
  // now has, which is the first thing to prove.
  desk.input_mouse_button(640, 300, true, hal::MouseRight);
  desk.input_mouse_motion(660, 330);
  frame(desk, kernel, W, H, out + "/0-start-page-menu.ppm");
  // Click "Show the workspace" where it is drawn: 6px of padding, then rows of
  // 24 and separators of 9 — Paste, Copy, ---, Next scene, Show the workspace.
  desk.input_mouse_button(700, 300 + 6 + 24 + 24 + 9 + 24 + 4, true, hal::MouseLeft);
  desk.switch_workspace(userland::HardwareCompositor::CockpitWorkspace::Code);
  desk.render_desktop(kernel);

  std::printf("step 1: type the command, forgetting the word at the front\n");
  type(desk, "entity list");
  frame(desk, kernel, W, H, out + "/1-typed.ppm");

  std::printf("step 2: eleven Lefts, back to the front of the line\n");
  for (int i = 0; i < 11; ++i) desk.input_key(hal::KeyLeft);
  frame(desk, kernel, W, H, out + "/2-cursor-at-front.ppm");

  std::printf("step 3: type the missing word in at the cursor\n");
  type(desk, "ask ");
  frame(desk, kernel, W, H, out + "/3-word-inserted.ppm");

  std::printf("step 4: Ctrl+E then Ctrl+U clears the line, then paste from the clipboard\n");
  (void)hal::clipboard_set("horizon brief 3");
  desk.input_key(hal::KeyCtrlE);   // to the end, so the kill below takes it all
  desk.input_key(hal::KeyCtrlU);   // readline's kill-to-start
  desk.input_key(hal::KeyCtrlV);
  frame(desk, kernel, W, H, out + "/4-pasted.ppm");

  std::printf("step 5: Ctrl+A to the front, paste again, so paste lands AT the cursor\n");
  desk.input_key(hal::KeyCtrlA);
  (void)hal::clipboard_set("please ");
  desk.input_key(hal::KeyCtrlV);
  frame(desk, kernel, W, H, out + "/5-paste-at-cursor.ppm");

  std::printf("step 6: Ctrl+C copies the line back out to the clipboard\n");
  desk.input_key(hal::KeyCtrlC);
  auto copied = hal::clipboard_get();
  std::printf("        clipboard now holds: '%s'\n", copied.ok() ? copied->c_str() : "<failed>");

  std::printf("step 7: right-click inside the terminal pane\n");
  desk.input_mouse_button(420, 380, true, hal::MouseRight);
  desk.input_mouse_motion(430, 400);
  frame(desk, kernel, W, H, out + "/6-context-menu.ppm");

  // Stop. The dispatcher above does nothing, so this one blocks on purpose:
  // it is standing in for a mind that has set off down the wrong road.
  std::printf("step 8: a slow command, and the Stop control that ends it\n");
  {
    std::atomic<bool> asked_to_stop{false};
    std::atomic<bool> finished{false};
    desk.set_interrupt([&] { asked_to_stop = true; });
    desk.set_dispatcher([&](const std::string&, userland::OutputCallback out) {
      out("thinking...");
      for (int i = 0; i < 400 && !asked_to_stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
      out(asked_to_stop ? "stopped" : "ran to completion");
      finished = true;
    });
    // Dismiss the menu step 7 left open, or it owns the next click — which is
    // right, and is what made the first run of this look like Stop was broken.
    desk.input_mouse_button(200, 300, true, hal::MouseLeft);
    desk.submit_command("ask what is the meaning of all this");
    for (int i = 0; i < 100 && !desk.command_running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::printf("        running: %s (%s)\n", desk.command_running() ? "yes" : "no", desk.running_command().c_str());
    frame(desk, kernel, W, H, out + "/8-running.ppm");

    // Click the control where the compositor says it is, not where it looks
    // like it should be: the first attempt at this aimed 70px left of it.
    auto g = desk.stop_geom();
    std::printf("        Stop is at %d,%d %dx%d\n", g.x, g.y, g.w, g.h);
    desk.input_mouse_button(g.x + g.w / 2, g.y + g.h / 2, true, hal::MouseLeft);
    for (int i = 0; i < 200 && !finished; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::printf("        after clicking Stop: interrupt asked=%s, command finished=%s\n",
                asked_to_stop ? "yes" : "no", finished ? "yes" : "no");
    for (int i = 0; i < 100 && desk.command_running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::printf("        still running: %s\n", desk.command_running() ? "yes" : "no");
    frame(desk, kernel, W, H, out + "/9-stopped.ppm");

    // And by keyboard, which is what a person's hands reach for first.
    for (int key : {(int)hal::KeyEscape, (int)hal::KeyCtrlC}) {
      asked_to_stop = false;
      finished = false;
      desk.submit_command("ask something else");
      for (int i = 0; i < 100 && !desk.command_running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
      desk.input_key(key);
      for (int i = 0; i < 200 && !finished; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
      std::printf("        %s stopped it: %s\n", key == hal::KeyEscape ? "Escape" : "Ctrl+C",
                  (asked_to_stop && finished) ? "yes" : "NO");
      for (int i = 0; i < 100 && desk.command_running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  desk.set_dispatcher([](const std::string&, userland::OutputCallback) {});

  std::printf("step 9: right-click the bare desktop, between the panes\n");
  desk.input_mouse_button(714, 600, true, hal::MouseLeft);   // dismiss the open one
  desk.input_mouse_button(714, 600, true, hal::MouseRight);
  desk.input_mouse_motion(760, 660);
  frame(desk, kernel, W, H, out + "/7-desktop-menu.ppm");

  // ---- the drawing surface -------------------------------------------------
  // Everything below goes through the SAME three pointer calls a finger uses,
  // because that is the only path the surface has: a touch backend calls them
  // directly, a mouse backend is routed into them, and so a result here is a
  // result for both.
  std::printf("step 10: the drawing surface — a drag, the palette, undo, and the on-screen controls\n");
  desk.draw_mode_set(true);
  desk.draw_set_width(9);

  // A curve, sampled the way a hand delivers one: far apart, and not on a
  // straight line. If the smoothing is doing its job there is ink the whole way
  // along it and no gaps between the samples.
  desk.draw_select_ink("rose");
  std::printf("        ink now: %s\n", desk.draw_ink_name().c_str());
  desk.draw_pointer_down(200, 500);
  const int arc[][2] = {{260, 420}, {330, 380}, {410, 390}, {470, 440}, {510, 520}, {520, 600}};
  for (const auto& p : arc) desk.draw_pointer_move(p[0], p[1]);
  desk.draw_pointer_up(520, 600);

  // A second stroke in a DIFFERENT ink the agent side picked by name, so the
  // picture is not one hue.
  desk.draw_select_ink("lagoon");
  std::printf("        ink now: %s\n", desk.draw_ink_name().c_str());
  desk.draw_pointer_down(620, 400);
  for (int i = 1; i <= 8; ++i) desk.draw_pointer_move(620 + i * 30, 400 + (i % 2 ? 60 : -60));
  desk.draw_pointer_up(620 + 8 * 30, 400 - 60);
  frame(desk, kernel, W, H, out + "/10-drawn.ppm");
  std::printf("        strokes=%zu points=%zu\n", desk.draw_stroke_count(), desk.draw_point_count());

  // Is the curve continuous? Walk the arc's x range and count columns with no
  // ink in them. A chain of disconnected dots shows up here as gaps.
  {
    int gaps = 0;
    for (int x = 200; x <= 520; ++x) {
      bool ink = false;
      for (int y = 340; y <= 640 && !ink; ++y) if ((desk.draw_pixel(x, y) & 0x00FFFFFFu) != 0) ink = true;
      if (!ink) ++gaps;
    }
    std::printf("        columns across the curve with no ink: %d\n", gaps);
  }

  // Undo by the on-screen control, pressed as a THUMB presses it — down and up
  // inside the same rectangle, through draw_pointer_*, not a mouse-only route.
  {
    auto r = desk.draw_undo_rect();
    const size_t before = desk.draw_stroke_count();
    desk.draw_pointer_down(r.x + r.w / 2, r.y + r.h / 2);
    desk.draw_pointer_up(r.x + r.w / 2, r.y + r.h / 2);
    std::printf("        undo control at %d,%d %dx%d: strokes %zu -> %zu\n",
                r.x, r.y, r.w, r.h, before, desk.draw_stroke_count());
  }
  frame(desk, kernel, W, H, out + "/11-after-undo.ppm");

  // A press that slides OFF the control must not fire it.
  {
    auto r = desk.draw_clear_rect();
    const size_t before = desk.draw_stroke_count();
    desk.draw_pointer_down(r.x + r.w / 2, r.y + r.h / 2);
    desk.draw_pointer_move(r.x + r.w / 2, r.y + r.h + 200);
    desk.draw_pointer_up(r.x + r.w / 2, r.y + r.h + 200);
    std::printf("        press slid off clear: strokes %zu -> %zu (must not change, and must leave no stroke behind)\n",
                before, desk.draw_stroke_count());
  }

  // And the clear control, pressed properly.
  {
    auto r = desk.draw_clear_rect();
    desk.draw_pointer_down(r.x + r.w / 2, r.y + r.h / 2);
    desk.draw_pointer_up(r.x + r.w / 2, r.y + r.h / 2);
    std::printf("        clear control: strokes now %zu, pixel at 330,380 = %06x\n",
                desk.draw_stroke_count(), desk.draw_pixel(330, 380) & 0x00FFFFFFu);
  }
  frame(desk, kernel, W, H, out + "/12-cleared.ppm");

  // The agent side reaching in the way it really does: directives, as written
  // into the run directory by `aios draw palette ...`.
  {
    const std::string trouble = desk.apply_draw_directives("palette amber\nwidth 12\n");
    desk.draw_pointer_down(300, 200);
    for (int i = 1; i <= 6; ++i) desk.draw_pointer_move(300 + i * 40, 200 + i * 20);
    desk.draw_pointer_up(300 + 6 * 40, 200 + 6 * 20);
    std::printf("        after `palette amber` directive: ink=%s trouble='%s' stroke pixel=%06x\n",
                desk.draw_ink_name().c_str(), trouble.c_str(), desk.draw_pixel(340, 220) & 0x00FFFFFFu);
  }
  frame(desk, kernel, W, H, out + "/13-agent-ink.ppm");

  std::printf("done — frames in %s\n", out.c_str());
  return 0;
}
