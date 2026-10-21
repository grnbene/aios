// The graphical desktop, on the metal.
//
// There is no display server here and nothing to ask for a window: the machine's
// framebuffer — whatever mode the loader put the hardware in — IS the display,
// and this file is the whole path from the PS/2 controller to a composited
// frame in video memory. The window manager itself is the same
// aios::userland::HardwareCompositor a hosted build runs; it is fed through the
// backend-neutral input calls, so the desktop a person drives here behaves
// exactly like the one on a hosted machine, mouse, keyboard and all.
#include "aios/userland/compositor.hpp"
#include "aios/hal/keys.hpp"

extern "C" bool metal_display_describe(uint32_t* width, uint32_t* height, uint32_t* bpp, uint32_t* text_cols, uint32_t* text_rows);
extern "C" void metal_display_blit(const uint32_t* argb, uint32_t w, uint32_t h);
extern "C" void metal_display_graphics(bool on);
extern "C" void metal_vga_clear();
extern "C" bool metal_input_init(uint32_t bound_w, uint32_t bound_h);
extern "C" bool metal_mouse_present();
extern "C" bool metal_mouse_poll(int* x, int* y, int* buttons, int* wheel);
extern "C" int metal_key_poll();
extern "C" bool metal_key_word_modifier();
extern "C" uint64_t metal_monotonic_ms();
// `hlt` on x86, `wfi` on AArch64 — the idle wait is per-machine, so the desktop
// asks for it by name instead of writing one architecture's instruction inline.
extern "C" void metal_halt_until_interrupt();
extern "C" void metal_console_write(const char* s, size_t n);

namespace {
void say(const std::string& s) { metal_console_write(s.data(), s.size()); }
}

// Runs the desktop until the operator leaves it (Escape, or the last window
// closed with nothing left to show). `idle` is the substrate's pump — the
// network and the kernel's services must keep advancing while a human is
// looking at a window, exactly as they do between prompts on the console.
aios::Result<void> metal_run_desktop(aios::Kernel& k,
                                    aios::userland::CommandDispatcher dispatcher,
                                    void (*idle)()) {
  // A command run from a desktop terminal can be `desktop` itself; one screen
  // means one desktop.
  static bool active = false;
  if (active) return aios::Result<void>(aios::Status::Denied, "the desktop is already on the screen");

  uint32_t w = 0, h = 0, bpp = 0, tc = 0, tr = 0;
  if (!metal_display_describe(&w, &h, &bpp, &tc, &tr))
    return aios::Result<void>(aios::Status::Unsupported,
                              "no linear framebuffer on this machine: the loader left it in VGA text mode, "
                              "so there is nothing to composite onto (boot with a graphical mode, or use the console)");
  if (w < 640 || h < 480)
    return aios::Result<void>(aios::Status::Unsupported, "the framebuffer is too small for a desktop");

  aios::userland::HardwareCompositor wm(w, h);
  wm.set_dispatcher(std::move(dispatcher));
  // Without threads, a command runs on the frame loop: the desktop pauses for
  // exactly as long as the command takes, and nothing races the window state.
  wm.set_async_commands(false);
  // This loop composites the WINDOWED workspace — it calls render_desktop, and
  // nothing here ever paints the start page. The compositor's click routing is
  // switched on the same flag, so leaving it on the canvas default sent every
  // click down the canvas' hit test while the screen showed the workspace: the
  // top bar drew its controls at their measured widths and a click was tested
  // against the canvas' hardcoded ones. Input now agrees with what is drawn.
  wm.set_canvas_mode(false);

  bool mouse = metal_input_init(w, h);
  say(std::string("[metal] desktop: framebuffer ") + std::to_string(w) + "x" + std::to_string(h) + "x" +
      std::to_string(bpp) + ", PS/2 mouse " + (mouse ? "online" : "ABSENT (keyboard only)") +
      ", PS/2 keyboard online\n");
  say("[metal] desktop: Esc leaves, F1-F4 raise a pane, the mouse drags and resizes\n");

  wm.init_default_workspace(k);
  metal_display_graphics(true);
  active = true;

  int last_x = -1, last_y = -1, last_buttons = 0;
  uint64_t next_frame = 0, next_tick = 0;
  bool dirty = true;
  bool running = true;
  while (running) {
    // Input first, so a frame always shows the newest state of the machine.
    for (int drained = 0; drained < 32; ++drained) {
      int key = metal_key_poll();
      if (key < 0) break;
      dirty = true;
      wm.set_word_modifier(metal_key_word_modifier());
      if (!wm.input_key(key)) { running = false; break; }
    }
    if (!running) break;

    if (mouse) {
      int mx = 0, my = 0, buttons = 0, wheel = 0;
      metal_mouse_poll(&mx, &my, &buttons, &wheel);
      if (mx != last_x || my != last_y) {
        wm.input_mouse_motion(mx, my);
        last_x = mx;
        last_y = my;
        dirty = true;
      }
      // Both buttons drive the window manager. The right one used to be read
      // off the PS/2 packet and thrown away, so this machine had a pointer with
      // half of it wired up; it opens the context menu now, as it does on a
      // hosted screen.
      for (int b : {aios::hal::MouseLeft, aios::hal::MouseRight}) {
        bool was_down = (last_buttons & b) != 0;
        bool is_down = (buttons & b) != 0;
        if (is_down == was_down) continue;
        wm.input_mouse_button(mx, my, is_down, b);
        dirty = true;
      }
      last_buttons = buttons;
      if (wheel) {
        wm.input_mouse_wheel(wheel);
        dirty = true;
      }
    }

    if (idle) idle();

    // Render immediately when dirty (up to 60fps for snappy mouse/typing response),
    // or periodically every 250ms for the clock and resource monitors.
    uint64_t now = metal_monotonic_ms();
    if ((dirty && now >= next_frame) || now >= next_tick) {
      next_frame = now + 16;
      next_tick = now + 250;
      wm.render_desktop(k);
      metal_display_blit(wm.framebuffer().data(), wm.width(), wm.height());
      dirty = false;
    } else if (!dirty) {
      metal_halt_until_interrupt();
    }
  }

  active = false;
  metal_display_graphics(false);
  metal_vga_clear();
  say("[metal] desktop: left; the command surface has the screen back\n");
  return aios::Ok();
}
