/* Crash-persistent logging to the physical SD card (metal/arm64/crashlog.cpp).
 *
 * Everything else this board reports (kconsole_log's ring buffer, the UDP
 * console, screenshot_server_poll()) lives only in RAM and only while the
 * board is still answering - exactly the two things a hard freeze takes
 * away first. `desktop` freezing solid with no panic text and no console
 * reply (the exact symptom a silent stack overflow produces - a corrupted
 * return address does not necessarily reach metal_panic() cleanly) is what
 * this is for: write each checkpoint straight to the SD card AS IT
 * HAPPENS, so pulling the card after a freeze and reading crashlog.txt
 * shows exactly how far execution actually got, instead of a guess.
 *
 * Deliberately NOT routed through hal_host_arm64.cpp's in-memory store
 * (that exists precisely because this board's SD card was not reliably
 * mounted when Kernel::boot() needed it) - this calls fat32_write_file()
 * directly, and reports mount status on first use so it is obvious whether
 * this diagnostic path can even work before trusting it. */
#include "fat32.hpp"
#include "kio.hpp"

namespace {
constexpr size_t kCap = 4000;  /* fat32.cpp's own per-file cap is 4096 */
char g_buf[kCap];
size_t g_len = 0;
bool g_checked_mount = false;
}  // namespace

void net_breadcrumb(const char* msg);

extern "C" void crashlog(const char* line) {
  net_breadcrumb(line);
  if (!g_checked_mount) {
    g_checked_mount = true;
    if (!fat32_status().mounted) {
      kputs("[crashlog] SD not mounted yet - retrying fat32_mount()\n");
      fat32_mount();
    }
    if (fat32_status().mounted) {
      /* Keep the previous boot's log: a watchdog reboot after a hang is
       * exactly when it matters, and the first write below would erase it. */
      static char prev[kCap];
      size_t plen = 0;
      if (fat32_read_file("crashlog.txt", prev, sizeof(prev), plen) && plen > 0) {
        fat32_write_file("CRASHOLD.TXT", prev, plen);   /* 8.3: the old "crashprev.txt" was refused */
      }
    }
    kprintf("[crashlog] SD mounted=%s - crashlog.txt %s\n",
            fat32_status().mounted ? "yes" : "no",
            fat32_status().mounted ? "will be written" : "CANNOT be written - this diagnostic path is dead");
  }
  if (!fat32_status().mounted) return;

  size_t n = 0;
  while (line[n] && g_len + n + 1 < kCap) ++n;
  for (size_t i = 0; i < n; ++i) g_buf[g_len + i] = line[i];
  g_len += n;
  if (g_len + 1 < kCap) g_buf[g_len++] = '\n';
  fat32_write_file("crashlog.txt", g_buf, g_len);
}
