/* Serving side of fleet distribution for AArch64 bare metal
 * (metal/arm64/node_shell.cpp).
 *
 * fleet_shell.cpp's `fleet` command only ever did the client half: attach to
 * a hub, list the roster. Nothing accepted a connection FROM a peer, so this
 * board could see the fleet but never actually do distributed work for it -
 * the transport (hal_net_arm64.cpp's net_listen/net_connect) and the
 * threading (hal_thread_arm64.cpp's SMP/PSCI-backed hal::Thread) it needs
 * were both already real, just never wired to aios::NodeService. This file
 * is that wiring: start_polled()+poll() (aios/node/node.hpp), the same
 * no-extra-thread pattern net_poll()/usb_hid_poll() already use, pumped from
 * main.cpp's console loop, so a peer's request runs on the console's own
 * core with no additional concurrency to reason about. */
#include "aios/node/node.hpp"
#include "kernel_boot.hpp"
#include "kio.hpp"
#include "fat32.hpp"

using aios::Kernel;
using aios::NodeService;

namespace {
NodeService* g_node = nullptr;
uint16_t g_node_port = 0;
}  // namespace

extern "C" bool node_service_start(uint16_t port, const char* token) {
  if (g_node) { kputs("  node: already serving - one NodeService per boot\n"); return false; }
  g_node = new NodeService(aios_arm64::kernel(), token);
  auto r = g_node->start_polled(port);
  if (!r.ok()) {
    kprintf("  node: listen on %u failed: %s\n", static_cast<unsigned>(port), r.error().str().c_str());
    delete g_node; g_node = nullptr;
    return false;
  }
  g_node_port = *r;
  kprintf("  node: serving fleet peers on tcp/%u - this board now accepts distributed work, not just roster visibility\n",
          static_cast<unsigned>(g_node_port));
  return true;
}

/* The token a peer must present. It used to be a constant compiled into the
 * image - the same secret on every board, readable by anyone with the source
 * or the kernel8.img - so `node start` served the board to all of them. Now it
 * comes from this boot only: `node start <token>`, `node start @FILE` (a file
 * on the SD card), or NODE.KEY on the card when no argument is given. With
 * none of those the board refuses to serve. Returns false with the reason
 * printed. `boot` marks the automatic start at DHCP time, which stays quiet
 * when the card simply has no NODE.KEY. */
extern "C" bool node_service_start_from(const char* arg, bool boot) {
  while (arg && *arg == ' ') ++arg;
  static char tok[160];
  size_t n = 0;
  const char* file = nullptr;
  if (!arg || !*arg) file = "NODE.KEY";
  else if (*arg == '@') file = arg + 1;
  if (file) {
    if (!fat32_status().mounted || !fat32_read_file(file, tok, sizeof(tok) - 1, n) || n == 0) {
      if (boot) { kputs("  node: not serving - no NODE.KEY on the card (`node start <token>` to serve by hand)\n"); return false; }
      kprintf("  node: refusing to serve without a token - %s is not on the card; use `node start <token>`, `node start @FILE`, or put one in NODE.KEY\n", file);
      return false;
    }
  } else {
    while (arg[n] && n < sizeof(tok) - 1) { tok[n] = arg[n]; ++n; }
  }
  while (n > 0 && (tok[n - 1] == '\n' || tok[n - 1] == '\r' || tok[n - 1] == ' ')) --n;
  tok[n] = 0;
  bool ok = false;
  if (n < 16) kprintf("  node: refusing a %u-character token - a peer could guess it; use at least 16\n", static_cast<unsigned>(n));
  else ok = node_service_start(7421, tok);
  for (size_t i = 0; i < sizeof(tok); ++i) tok[i] = 0;
  return ok;
}

/* Called every main-loop iteration once serving has started - a no-op
 * otherwise, same shape as net_poll()/usb_hid_poll() so it costs nothing to
 * call unconditionally from main.cpp before a peer ever dials in. */
extern "C" void node_service_poll() {
  if (g_node) g_node->poll();
}

extern "C" void node_service_report() {
  if (!g_node) { kputs("  node: not serving (fleet attach only - no inbound work accepted)\n"); return; }
  auto peers = g_node->peers();
  kprintf("  node: serving on tcp/%u, %llu peer(s) ever connected\n",
          static_cast<unsigned>(g_node_port), static_cast<unsigned long long>(peers.size()));
  for (auto& p : peers) {
    kprintf("    - %s guest=%s requests=%llu %s\n", p.address.c_str(), p.guest.short_str().c_str(),
            static_cast<unsigned long long>(p.requests), p.connected ? "connected" : "disconnected");
  }
}
