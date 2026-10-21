/* The real AIOS desktop on AArch64 bare metal (metal/arm64/desktop_shell.cpp).
 *
 * metal/arm64/desktop.cpp is a bespoke diagnostic dashboard built for bring-up
 * ("dash" command) - it never touches aios::userland::HardwareCompositor. This
 * file is the actual port: the same windowed compositor a hosted build and
 * x86 bare metal both run (metal/desktop.cpp), fed through this board's own
 * input backend (USB HID keyboard/mouse instead of PS/2) and its own
 * framebuffer (fb_blit instead of a VGA-mode blit).
 *
 * Compiles under STLFLAGS (see build-arm64.sh), same as entity_shell.cpp /
 * fleet_shell.cpp - real STL, real aios::kernel::Kernel. A single extern "C"
 * function is the seam back into main.cpp's freestanding world.
 *
 * No disk, no virtio fabric on this board yet: Kernel::boot() runs with an
 * in-memory root, same as x86 metal falls back to with no virtio disk
 * attached. The command palette is intentionally small - it does not route
 * through aios/cli's ~9000-line command table (that pulls in fabric/ELF-load/
 * disk commands this substrate has no backing for yet); it is a fresh,
 * minimal dispatcher shaped for what this port can actually do today. */
#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"
#include <memory>
#include "aios/userland/compositor.hpp"
#include "aios/hal/keys.hpp"
#include "fb.hpp"
#include "gic.hpp"
#include "timer.hpp"
#include "kernel_boot.hpp"
#include "kio.hpp"
#include "net.hpp"
#include "usb_hid.hpp"
#include "timer.hpp"
#include "aios/metal/trap.hpp"

void screenshot_server_poll();
extern "C" void crashlog(const char* line);

extern "C" uint64_t metal_ticks();
extern "C" uint32_t metal_tick_hz();
extern "C" void metal_halt_until_interrupt();

using aios::Kernel;
using aios::userland::HardwareCompositor;

namespace {

/* usb_get_key()'s F1-F6 (0x91..0x96, see usb_hid.hpp) predate this port and
 * don't share aios::hal::Key's F-key numbering (KeyF1 = 0x1010 up) - every
 * other input backend (PS/2 included) already speaks that numbering, so
 * translate at the one seam that needs it instead of teaching the compositor
 * a second convention. */
int translate_key(int k) {
  if (k >= USB_KEY_F1 && k <= USB_KEY_F6) return aios::hal::KeyF1 + (k - USB_KEY_F1);
  return k;
}

bool g_exit_requested = false;

aios::userland::CommandDispatcher desktop_dispatcher(Kernel& k) {
  return [&k](const std::string& cmd, aios::userland::OutputCallback emit) {
    if (cmd == "help") {
      emit("commands: help, info, ps, exit");
      return;
    }
    if (cmd == "info") {
      emit("AIOS AArch64 bare metal - kernel " + k.self().short_str());
      /* No GIC (the Pi 3): the timer interrupt is never delivered, so "ticks: 0
       * @ 100 Hz" read as a stopped clock. Say what the board actually has. */
      if (metal_gic_version() > 0)
        emit("ticks: " + std::to_string(metal_ticks()) + " @ " + std::to_string(metal_tick_hz()) + " Hz");
      else
        emit("uptime " + std::to_string(metal_now_ns() / 1000000ull) + " ms from the counter; no tick interrupts on this board (no GIC)");
      return;
    }
    if (cmd == "ps") {
      emit("(process listing not wired into this dispatcher yet - use the serial 'ps' command)");
      return;
    }
    if (cmd == "exit" || cmd == "quit") {
      /* Escape never leaves the desktop (on purpose - a stray Escape closes
       * a menu, it does not drop you out), so it must not say it does. */
      emit("leaving the desktop");
      g_exit_requested = true;
      return;
    }
    emit("unknown command: " + cmd + " (try: help, info, ps)");
  };
}


}  // namespace

/* The only way out of compositor_run()'s loop besides Escape on a physical
 * keyboard this board does not reliably have attached. Console command
 * `desktop exit` (main.cpp) sets this; the loop below checks it once per
 * frame, same as it checks wm.input_key()'s own return. */
extern "C" void compositor_request_exit() { g_exit_requested = true; }

/* Compositor step trace (see AIOS_TRACE in compositor.cpp): a budget of
 * breadcrumbs opened by each mouse click, sent immediately, so the last line
 * received names where a stall is. */
namespace { int g_ctrace_budget = 0; }
extern "C" void aios_trace(const char* step) {
  if (g_ctrace_budget <= 0) return;
  --g_ctrace_budget;
  char line[96] = "ctrace ";
  size_t n = 7;
  while (*step && n + 1 < sizeof(line)) line[n++] = *step++;
  line[n] = 0;
  net_breadcrumb(line);
  net_breadcrumb_flush();
}

extern "C" int smp_thread_reserve();
extern "C" void smp_thread_dispatch(int slot, void (*fn)(void*), void* ctx);
extern "C" int smp_thread_done(int slot);
extern "C" void smp_thread_reap(int slot);
void smp_start();
unsigned smp_online_count();

namespace {
/* A frame is painting on another core: whatever might read or change the
 * kernel on core 0 meanwhile (a console command, a fleet peer's request)
 * waits for it - desktop_render_wait(), called from the network path. */
volatile bool g_rendering = false;

/* The pointer, on the glass: drawn straight onto the scanout the moment the
 * mouse reports, with what was under it saved and put back when it moves.
 * It no longer waits for a frame - painting the whole desktop is ~33 ms, and
 * the pointer used to move once per paint. */
struct Pointer {
  const char* const* shape = nullptr;
  int rows = 0, cols = 0;
  int x = 0, y = 0;
  bool shown = false;
  uint32_t save[32 * 16];
  uint32_t* scan = nullptr;
  uint32_t stride = 0, w = 0, h = 0;
  void hide() {
    if (!shown || !scan) return;
    for (int r = 0; r < rows; ++r) for (int c = 0; c < cols; ++c) {
      const int px = x + c, py = y + r;
      if (px < 0 || py < 0 || px >= static_cast<int>(w) || py >= static_cast<int>(h)) continue;
      scan[static_cast<uint64_t>(py) * stride + px] = save[r * cols + c];
    }
    shown = false;
  }
  void show(int nx, int ny) {
    if (!scan) return;
    x = nx; y = ny;
    for (int r = 0; r < rows; ++r) for (int c = 0; c < cols; ++c) {
      const int px = x + c, py = y + r;
      if (px < 0 || py < 0 || px >= static_cast<int>(w) || py >= static_cast<int>(h)) continue;
      uint32_t* p = &scan[static_cast<uint64_t>(py) * stride + px];
      save[r * cols + c] = *p;
      const char ch = shape[r][c];
      if (ch == 'X') *p = 0x000F1117u;
      else if (ch == '.') *p = 0x00FFFFFFu;
    }
    shown = true;
  }
  void move(int nx, int ny) { hide(); show(nx, ny); }
};

struct RenderJob { HardwareCompositor* wm; Kernel* k; };
void render_job(void* ctx) {
  auto* j = static_cast<RenderJob*>(ctx);
  j->wm->render_desktop(*j->k);
  /* Cleared HERE, by the painting core: desktop_render_wait() spins on
   * core 0 inside net_poll(), which is inside the frame loop - a loop that
   * cleared it itself would never get the chance (it deadlocked on the
   * first console command sent during a paint). */
  __atomic_store_n(&g_rendering, false, __ATOMIC_RELEASE);
}
}  // namespace

extern "C" void desktop_render_wait() {
  while (__atomic_load_n(&g_rendering, __ATOMIC_ACQUIRE)) asm volatile("yield");
}

extern "C" bool compositor_run() {
  if (!fb_live()) {
    kputs("  desktop: no framebuffer online - nothing to composite onto\n");
    return false;
  }

  crashlog("compositor_run: entered");
  Kernel& k = aios_arm64::kernel();  /* the one kernel this board runs - shared with `entity`, `fleet` and node_shell.cpp */
  crashlog("compositor_run: kernel ready");

  const uint32_t w = fb_width(), h = fb_height();
  HardwareCompositor wm(w, h);
  crashlog("compositor_run: HardwareCompositor constructed");
  /* The desktop's own resident and surface, as every hosted build and drive
   * tool sets up. Without it the compositor had no console: face apps ran
   * with no host bindings - no machine() readings, no kernel store - so the
   * System Monitor drew blanks on this board. */
  static std::unique_ptr<aios::Surface> console;
  if (!console) {
    aios::AuthorityProfile profile;
    auto me = k.resident("desktop", profile);
    if (me.ok()) console = std::make_unique<aios::Surface>(k.as(*me));
    else crashlog("compositor_run: resident refused - apps run unbound");
  }
  if (console) wm.set_console(console.get());
  wm.set_dispatcher(desktop_dispatcher(k));
  wm.set_async_commands(false);  /* no threads here either - a command runs on the frame loop */
  wm.set_canvas_mode(false);
  wm.init_default_workspace(k);
  crashlog("compositor_run: init_default_workspace done, entering frame loop");

  kputs("  desktop: compositor online, framebuffer ");
  kprintf("%ux%u", w, h);
  kputs(", USB HID keyboard/mouse\n");
  kputs("  desktop: Power (top right) > Dashboard, `exit` in its terminal, or `desktop exit` on the console leaves; F1-F4 raise a pane\n");

  /* The desktop owns the glass. The framebuffer console kept drawing every
   * console command's output onto it - and scrolling it: past the bottom
   * line it moves the whole framebuffer up a text row, which dragged the
   * desktop up with it until the next repaint. */
  const bool con_was_live = fbcon_is_live();
  fbcon_suspend();

  /* Paint on another core, input and the pointer on this one. */
  if (smp_online_count() < 2) smp_start();
  Pointer ptr;
  ptr.rows = HardwareCompositor::pointer_shape(&ptr.shape);
  for (int r = 0; r < ptr.rows; ++r) { int n = 0; while (ptr.shape[r][n]) ++n; if (n > ptr.cols) ptr.cols = n; }
  if (ptr.rows > 32) ptr.rows = 32;
  if (ptr.cols > 16) ptr.cols = 16;
  ptr.scan = fb_scanout(&ptr.stride);
  ptr.w = w; ptr.h = h;
  if (ptr.scan) wm.set_software_pointer(false);
  RenderJob job{&wm, &k};
  int render_slot = -1;
  bool rendering = false;
  uint64_t t_paint0 = 0;
  /* Input that arrives while a frame paints waits here: the compositor is
   * the painting core's until the frame is done. */
  int pend_keys[64]; int n_pend_keys = 0;
  bool pend_motion = false;
  struct PendBtn { int b; bool down; int32_t x, y; } pend_btn[16]; int n_pend_btn = 0;

  g_exit_requested = false;
  wm.set_leave_action("Dashboard", "back to the bring-up dashboard", [] { g_exit_requested = true; });
  bool running = true;
  bool dirty = true;
  uint64_t next_frame_ns = 0, next_tick_ns = 0;
  const uint64_t hz = metal_tick_hz() ? metal_tick_hz() : 100;
  int32_t last_x = -1, last_y = -1;
  uint8_t last_buttons = 0;

  uint64_t hb_last = 0; uint32_t hb_frames = 0;
  /* Where each second went: painting, presenting, the rest of the loop
   * (network, screenshots, USB input), and SD crash-log writes. */
  uint64_t acc_paint = 0, acc_blit = 0, acc_poll = 0, acc_sd = 0, acc_net = 0, acc_shot = 0, acc_usb = 0;
  while (running) {
    /* Once-a-second heartbeat on udp/42425, like the dashboard's. */
    if (metal_now_ns() - hb_last > 1000000000ull) {
      hb_last = metal_now_ns();
      char hb[160] = "hb desktop frames=";
      size_t n = 18;
      auto num = [&](uint64_t v) { char d[24]; int nd = 0; do { d[nd++] = static_cast<char>('0' + v % 10); v /= 10; } while (v); while (nd > 0 && n + 1 < sizeof(hb)) hb[n++] = d[--nd]; };
      auto put = [&](const char* t) { while (*t && n + 1 < sizeof(hb)) hb[n++] = *t++; };
      const uint64_t f = hb_frames ? hb_frames : 1;
      num(hb_frames);
      put(" paint_ms="); num(acc_paint / f / 1000000ull);
      put(" blit_ms="); num(acc_blit / f / 1000000ull);
      put(" poll_ms/s="); num(acc_poll / 1000000ull);
      put(" sd_ms/s="); num(acc_sd / 1000000ull);
      put(" [net="); num(acc_net / 1000000ull); put(" shot="); num(acc_shot / 1000000ull); put(" usb="); num(acc_usb / 1000000ull); put("]");
      hb[n] = 0;
      net_breadcrumb(hb);
      hb_frames = 0;
      acc_paint = acc_blit = acc_poll = acc_sd = acc_net = acc_shot = acc_usb = 0;
    }
    const uint64_t t_poll0 = metal_now_ns();
    /* Without these two, the UDP console and screenshot server both go
     * completely dead the instant `desktop` starts - confirmed on real
     * hardware: the very first `desktop` run left the board with no way to
     * see it or get out of it remotely (no physical keyboard attached to
     * press Escape either), needing a power-cycle to recover. desktop.cpp's
     * dash frame loop already calls net_poll() from inside its own
     * blocking loop and console commands dispatched from there work fine
     * (proven all session) - same safe, already-established pattern. */
    net_poll();
    const uint64_t t_net = metal_now_ns();
    screenshot_server_poll();
    const uint64_t t_shot = metal_now_ns();
    if (g_exit_requested) break;
    usb_hid_poll();
    const uint64_t t_usb = metal_now_ns();
    acc_net += t_net - t_poll0;
    acc_shot += t_shot - t_net;
    acc_usb += t_usb - t_shot;
    acc_poll += t_usb - t_poll0;

    for (int drained = 0; drained < 32; ++drained) {
      int key = usb_get_key();
      if (key < 0) break;
      if (rendering) { if (n_pend_keys < 64) pend_keys[n_pend_keys++] = key; continue; }
      dirty = true;
      if (!wm.input_key(translate_key(key))) { running = false; break; }
    }
    if (!running) break;

    int32_t mx = 0, my = 0;
    uint8_t buttons = 0;
    if (usb_get_mouse_state(mx, my, buttons)) {
      if (mx != last_x || my != last_y) {
        if (ptr.scan) ptr.move(mx, my);          /* on the glass now, whatever is painting */
        if (rendering) pend_motion = true;
        else { wm.input_mouse_motion(mx, my); dirty = true; }
        last_x = mx; last_y = my;
      }
      for (int b : {aios::hal::MouseLeft, aios::hal::MouseRight}) {
        bool was_down = (last_buttons & b) != 0;
        bool is_down = (buttons & b) != 0;
        if (is_down != was_down) {
          /* Step tracing (aios_trace) stays wired but off: set
           * g_ctrace_budget here to hunt a stall that follows a click. */
          if (rendering) { if (n_pend_btn < 16) pend_btn[n_pend_btn++] = {b, is_down, mx, my}; }
          else { wm.input_mouse_button(mx, my, is_down, b); dirty = true; }
        }
      }
      last_buttons = buttons;
    }

    /* The free-running counter, not timer-IRQ ticks: the Pi 3 has no GIC,
     * so the tick interrupt is never unmasked and metal_ticks() stays 0.
     * Frame pacing on ticks drew frame 0 and then waited forever. */
    (void)hz;

    /* A frame finished painting on the other core: present it here, and put
     * the pointer back on top (the present covered it). */
    if (rendering && smp_thread_done(render_slot)) {
      smp_thread_reap(render_slot);
      rendering = false;
      __atomic_store_n(&g_rendering, false, __ATOMIC_RELEASE);
      const uint64_t t_paint1 = metal_now_ns();
      fb_blit(wm.framebuffer().data(), wm.width(), wm.height());
      if (ptr.scan) { ptr.shown = false; ptr.show(last_x < 0 ? 0 : last_x, last_y < 0 ? 0 : last_y); }
      const uint64_t t_present = metal_now_ns();
      wm.note_frame_timing(static_cast<float>(t_paint1 - t_paint0) / 1e6f, static_cast<float>(t_present - t_paint1) / 1e6f);
      wm.note_frame_presented(t_present / 1000000ull);
      acc_paint += t_paint1 - t_paint0;
      acc_blit += t_present - t_paint1;
      ++hb_frames;
    }

    if (!rendering) {
      /* The compositor is this core's again: hand it what arrived meanwhile. */
      for (int q = 0; q < n_pend_keys && running; ++q) if (!wm.input_key(translate_key(pend_keys[q]))) running = false;
      if (n_pend_keys) dirty = true;
      n_pend_keys = 0;
      if (!running) break;
      if (pend_motion) { wm.input_mouse_motion(last_x, last_y); dirty = true; pend_motion = false; }
      for (int q = 0; q < n_pend_btn; ++q) { wm.input_mouse_button(pend_btn[q].x, pend_btn[q].y, pend_btn[q].down, pend_btn[q].b); dirty = true; }
      n_pend_btn = 0;

      /* The free-running counter, not timer-IRQ ticks: the Pi 3 has no GIC,
       * so the tick interrupt is never unmasked and metal_ticks() stays 0. */
      const uint64_t now_ns = metal_now_ns();
      if ((dirty && now_ns >= next_frame_ns) || now_ns >= next_tick_ns) {
        next_frame_ns = now_ns + 16000000ull;   /* ~60fps cap */
        next_tick_ns = now_ns + 250000000ull;   /* clock/monitor refresh */
        static uint32_t frame_count = 0;
        /* Every frame for the first 10 (an early crash gets logged precisely),
         * then every 600th: an SD write at every 20th cost up to ~150 ms of a
         * second, and the live heartbeat (udp/42425) already dates a stop. */
        const uint64_t t_sd0 = metal_now_ns();
        if (frame_count < 10 || frame_count % 600 == 0) {
          char msg[48] = "compositor_run: pre-render frame ";
          size_t len = 33;
          char digits[10]; int nd = 0;
          uint32_t v = frame_count;
          do { digits[nd++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
          while (nd > 0) msg[len++] = digits[--nd];
          msg[len] = '\0';
          crashlog(msg);
        }
        acc_sd += metal_now_ns() - t_sd0;
        ++frame_count;
        dirty = false;
        t_paint0 = metal_now_ns();
        render_slot = smp_thread_reserve();
        if (render_slot >= 0) {
          __atomic_store_n(&g_rendering, true, __ATOMIC_RELEASE);
          rendering = true;
          smp_thread_dispatch(render_slot, render_job, &job);
        } else {
          /* No other core up: paint here, as before. */
          wm.render_desktop(k);
          const uint64_t t_paint1 = metal_now_ns();
          fb_blit(wm.framebuffer().data(), wm.width(), wm.height());
          if (ptr.scan) { ptr.shown = false; ptr.show(last_x < 0 ? 0 : last_x, last_y < 0 ? 0 : last_y); }
          const uint64_t t_present = metal_now_ns();
          wm.note_frame_timing(static_cast<float>(t_paint1 - t_paint0) / 1e6f, static_cast<float>(t_present - t_paint1) / 1e6f);
          wm.note_frame_presented(t_present / 1000000ull);
          acc_paint += t_paint1 - t_paint0;
          acc_blit += t_present - t_paint1;
          ++hb_frames;
        }
      } else if (!dirty) {
        /* wfi only returns on an interrupt, and on the Pi none is enabled -
         * that parked the loop for good (the watchdog then rebooted the
         * board). A short delay keeps input, network and the clock live. */
        if (metal_interrupts_live() && metal_ticks() > 0) metal_halt_until_interrupt();
        else metal_delay_us(1000);
      }
    } else {
      metal_delay_us(200);   /* painting elsewhere: keep polling input */
    }
  }

  /* Never leave with a frame still painting into a compositor that is about
   * to go away. */
  if (rendering) { while (!smp_thread_done(render_slot)) asm volatile("yield"); smp_thread_reap(render_slot); }
  __atomic_store_n(&g_rendering, false, __ATOMIC_RELEASE);
  if (con_was_live) fbcon_resume();
  kputs("  desktop: left; the command surface has the screen back\n");
  return true;
}
