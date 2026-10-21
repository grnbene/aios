// The canvas: what this machine looks like when it is not pretending to be a
// desktop.
//
// There are no windows here and nothing to click. It is a readout of the things
// this operating system is actually made of — identities as they are minted,
// the authority each one holds and how much of its ceiling it has spent, the
// standing directives counting down to their next run, the tasks holding the
// CPU, and what the machine has come to know. Walk up to it and you can see
// what it is doing; that is the whole purpose.
//
// It draws with the compositor's primitives but creates no window: the
// compositor is used here as a surface and a typeface, nothing more.
#include <algorithm>
#include <string>
#include <vector>

#include "aios/userland/compositor.hpp"
#include "aios/userland/directive.hpp"
#include "aios/userland/horizon.hpp"
#include "aios/hal/keys.hpp"

extern "C" bool metal_display_describe(uint32_t* width, uint32_t* height, uint32_t* bpp, uint32_t* text_cols, uint32_t* text_rows);
extern "C" void metal_display_blit(const uint32_t* argb, uint32_t w, uint32_t h);
extern "C" void metal_display_graphics(bool on);
extern "C" void metal_vga_clear();
extern "C" int metal_key_poll();
extern "C" uint64_t metal_monotonic_ms();
extern "C" void metal_console_write(const char* s, size_t n);

#include "aios/userland/canvas.hpp"
using aios::userland::CanvasFeed;
// The bare-metal scanout for the shared canvas painter: ask the display what it
// is, paint for exactly that, blit. Every pixel above this line is the same
// code a hosted screen shows.
// Which scene the policy head asked for, if it has asked. On bare metal the
// display IS this process, so a_t[2] reaches the canvas directly here rather
// than through a file some other process polls.
static volatile int g_metal_scene = -1;
extern "C" void metal_set_display_scene(uint32_t scene) {
  g_metal_scene = (int)(scene > 3 ? 3 : scene);
}

aios::Result<void> metal_run_canvas(aios::Kernel& k, aios::Surface& console, CanvasFeed (*feed)(), void (*idle)()) {
  using namespace aios;
  uint32_t w = 0, h = 0, bpp = 0, tc = 0, tr = 0;
  if (!metal_display_describe(&w, &h, &bpp, &tc, &tr))
    return Result<void>(Status::Unsupported, "no linear framebuffer on this machine: there is nothing to paint a canvas onto");
  userland::HardwareCompositor surface(w, h);
  std::vector<uint32_t> buf((size_t)w * h, 0xFF0A0A0A);
  userland::CanvasState st;
  st.sweep(k, metal_monotonic_ms());
  metal_display_graphics(true);
  metal_console_write("[metal] canvas: the machine's own lifecycle; Esc leaves\n", 55);
  bool leaving = false;
  while (!leaving) {
    if (idle) idle();
    for (int key = metal_key_poll(); key >= 0; key = metal_key_poll())
      if (key == 27) leaving = true;
    if (leaving) break;
    uint64_t now = metal_monotonic_ms();
    st.sweep(k, now);
    CanvasFeed f = feed ? feed() : CanvasFeed{};
    if (g_metal_scene >= 0) st.set_every_scene(g_metal_scene);
    userland::paint_canvas(surface, buf, w, h, k, console, f, now, st);
    metal_display_blit(buf.data(), w, h);
  }
  metal_display_graphics(false);
  metal_vga_clear();
  return Ok();
}
