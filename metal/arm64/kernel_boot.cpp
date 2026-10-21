/* The one aios::Kernel instance this board runs (metal/arm64/kernel_boot.cpp).
 *
 * entity_shell.cpp and desktop_shell.cpp used to each boot their own throwaway
 * Kernel - fine for proving a real STL Kernel::boot() links and runs on this
 * hardware, wrong for anything that needs to be the SAME machine: a peer that
 * joins this board over the fleet (node_shell.cpp's NodeService) spawns guest
 * entities INTO a kernel, and the desktop (desktop_shell.cpp's compositor)
 * renders a kernel's device/entity state - those only agree with each other,
 * and with what `entity`/`fleet` report at the console, if they all share one
 * instance. Lazily booted on first use so a board that never opens the
 * desktop or joins the fleet never pays for it. */
#include "aios/kernel/kernel.hpp"
#include "kio.hpp"

extern "C" void crashlog(const char* line);

using aios::Kernel;
using aios::KernelConfig;

namespace {
Kernel* g_kernel = nullptr;
}

extern "C" bool arm64_kernel_ready() { return g_kernel != nullptr; }

/* metal/arm64/app_blobs.S */
extern "C" const char app_order_desk_start[], app_order_desk_end[], app_task_tracker_start[], app_task_tracker_end[],
                      app_system_monitor_start[], app_system_monitor_end[], app_notes_start[], app_notes_end[];

namespace aios_arm64 {

Kernel& kernel() {
  if (!g_kernel) {
    /* Checkpoints, not decoration: Kernel::boot() has never actually been
     * proven to RUN on this hardware, only to LINK (0 undefined symbols is
     * a link-time fact, not a runtime one - conflating the two is exactly
     * what froze the board the first three times something finally called
     * this). kputs/kprintf write straight to fbcon synchronously, not
     * queued - so whatever printed before a hang is still on the glass
     * after it, readable over screenshot_server_poll() even with the
     * console itself dead. */
    kputs("  kernel: booting aios::Kernel for the first time on this board...\n");
    crashlog("kernel_boot: calling Kernel::boot()");
    KernelConfig cfg;
    cfg.root = "/aios/root";  /* in-memory: no AOS volume/virtio disk on this board yet */
    auto kr = Kernel::boot(cfg);
    kputs("  kernel: Kernel::boot() returned\n");
    crashlog("kernel_boot: Kernel::boot() returned");
    if (!kr.ok()) {
      kprintf("  kernel: boot failed: %s - halting\n", kr.error().str().c_str());
      crashlog("kernel_boot: boot() failed - halting");
      for (;;) asm volatile("wfi");
    }
    g_kernel = kr->release();
    /* The bundled apps, into the in-memory root the desktop reads them from. */
    {
      struct Blob { const char* name; const char* start; const char* end; };
      const Blob blobs[] = {
        {"order_desk.aos", app_order_desk_start, app_order_desk_end},
        {"task_tracker.aos", app_task_tracker_start, app_task_tracker_end},
        {"system_monitor.aos", app_system_monitor_start, app_system_monitor_end},
        {"notes.aos", app_notes_start, app_notes_end},
      };
      auto& host = g_kernel->host();
      (void)host.make_dir(cfg.root + "/apps");
      unsigned ok = 0;
      for (const Blob& b : blobs) {
        const auto* p = reinterpret_cast<const uint8_t*>(b.start);
        if (host.write_file(cfg.root + "/apps/" + b.name, aios::ByteSpan(p, static_cast<size_t>(b.end - b.start))).ok()) ++ok;
      }
      kprintf("  kernel: installed %u bundled apps into %s/apps\n", static_cast<uint64_t>(ok), cfg.root.c_str());
    }
    kputs("  kernel: ready\n");
    crashlog("kernel_boot: ready");
  }
  return *g_kernel;
}

}  // namespace aios_arm64
