/* AArch64 bare-metal entry for AIOS (metal/arm64/main.cpp).
 *
 * Everything this prints is something it measured. This file itself has no
 * libc and no libstdc++ under it — aarch64-elf-gcc ships neither, so it
 * builds under strict freestanding flags (see build-arm64.sh's SRCS) — but
 * the AIOS core in src/ IS linked into this image now, through a second,
 * STL-linked compile pass (build-arm64.sh's STLSRCS_METAL) that entity_shell/
 * fleet_shell/desktop_shell are the extern "C" seams into.
 */
#include "bcm43438.hpp"
#include "bcm_irq.hpp"
#include "desktop.hpp"
#include "dhd.hpp"
#include "dwc2.hpp"
#include "fat32.hpp"
#include "fb.hpp"
#include "fdt.hpp"
#include "gic.hpp"
#include "kio.hpp"
#include "mmu.hpp"
#include "net.hpp"
#include "psci.hpp"
#include "sdcard.hpp"
#include "sdio.hpp"
#include "shell.hpp"
#include "timer.hpp"
#include "uart.hpp"
#include "usb_hid.hpp"
#include "rpi_mailbox.hpp"
#include "mmio.hpp"
#include "gpio.hpp"

#include "aios/metal/trap.hpp"

struct tm { int tm_sec; int tm_min; int tm_hour; int tm_mday; int tm_mon; int tm_year; int tm_wday; int tm_yday; int tm_isdst; };
extern "C" {
long time(long* t);
struct tm* localtime_r(const long* t, struct tm* out);
size_t strftime(char* s, size_t max, const char* fmt, const struct tm* t);
bool metal_wall_clock_set();
bool metal_set_timezone(const char* name);
const char* metal_timezone_name();
}


extern "C" {
uint64_t metal_ticks();
uint32_t metal_tick_hz();
bool metal_interrupts_live();
uint64_t metal_spurious();
uint64_t metal_fault_count();
void metal_idt_init();
void metal_halt_until_interrupt();
[[noreturn]] void metal_power_off();
[[noreturn]] void metal_power_reboot();
[[noreturn]] void metal_panic(const char* what);
}

/* Provided by the subsystems that own them. */
bool neon_selftest();
extern "C" void metal_watchdog_pet();
extern "C" char __kernel_start[];   /* link.ld */
void blk_init();
void blk_report();
void proc_init();
void proc_report();
extern "C" void entity_test_command();
extern "C" void fleet_join_test(const char* args);
extern "C" void grain_eval(const char* source);
extern "C" void bench_run();
extern "C" bool compositor_run();
extern "C" void compositor_request_exit();
extern "C" bool node_service_start(uint16_t port, const char* token);
extern "C" bool node_service_start_from(const char* arg, bool boot);
extern "C" void node_service_poll();
extern "C" void things_frame_tick();
extern "C" void node_service_report();
extern "C" bool console_debug_enabled();
extern "C" void console_debug_set(bool on);

bool screenshot_server_start();
void screenshot_server_poll();
bool proc_run(unsigned index);
void smp_start();
void smp_report();
void smp_test();
uint64_t smp_online_mask();
unsigned smp_online_count();

namespace {

char g_line[256];

bool streq(const char* a, const char* b) {
  while (*a && *a == *b) { ++a; ++b; }
  return *a == *b;
}

/* ---- shell -------------------------------------------------------------- */

void cmd_info() {
  const FdtInfo& f = fdt_info();
  char b1[32], b2[32];
  kprintf("  machine    : %s\n", f.model[0] ? f.model : "(unnamed)");
  kprintf("  substrate  : AArch64 EL1, 4 KiB granule, 39-bit VA, TTBR0 only\n");
  kprintf("  dram       : %p .. %p (%s)\n", f.ram_base, f.ram_base + f.ram_size,
          khuman(f.ram_size, b1, sizeof(b1)));
  kprintf("  frames     : %s in use of %s\n",
          khuman(metal_heap_used(), b1, sizeof(b1)),
          khuman(metal_heap_total(), b2, sizeof(b2)));
  kprintf("  mmu        : %s\n", metal_mmu_live() ? "on" : "off");
  kprintf("  gic        : v%d at %p\n", metal_gic_version(), f.gicd_base);
  /* The generic timer is programmed at the tick rate on every board, but its
   * interrupt is only delivered through a GIC. The Pi 3 has none (its BCM2836
   * core-local controller is not wired to the timer here), so the tick count
   * there stays 0 by design: time comes from the counter, never from ticks,
   * and nothing on this board is preempted. Saying "100 Hz tick" beside
   * "0 ticks" read as a broken timer; say which it is. */
  if (metal_gic_version() > 0)
    kprintf("  timer      : %u Hz tick, %u Hz counter, intid %u\n",
            static_cast<uint64_t>(metal_tick_hz()), metal_cntfrq(),
            static_cast<uint64_t>(metal_timer_intid()));
  else
    kprintf("  timer      : %u Hz counter; no tick interrupts (no GIC on this board - time is read from the counter, nothing is preempted)\n",
            metal_cntfrq());
  /* Secondary cores are started on demand (`smp`, or the desktop, which paints
   * on another core), not at boot, so "1 online" after a plain boot is right. */
  kprintf("  cpus       : %u in the device tree, %u online%s\n",
          static_cast<uint64_t>(f.cpu_count), static_cast<uint64_t>(smp_online_count()),
          smp_online_count() < f.cpu_count ? " (the others start with `smp` or `desktop`)" : "");
  if (fb_live())
    kprintf("  display    : %u x %u, %s\n", fb_width(), fb_height(), fb_source_name());
  else
    kputs("  display    : none\n");
  if (!f.psci_present) {
    kputs("  psci       : absent\n");
  } else {
    const uint32_t v = metal_psci_version();
    kprintf("  psci       : v%u.%u over %s\n", static_cast<uint64_t>(v >> 16),
            static_cast<uint64_t>(v & 0xFFFF), f.psci_use_hvc ? "hvc" : "smc");
  }
  if (metal_gic_version() > 0)
    kprintf("  uptime     : %u ms over %u ticks\n", metal_now_ns() / 1000000ull, metal_ticks());
  else
    kprintf("  uptime     : %u ms (counter)\n", metal_now_ns() / 1000000ull);
}

void cmd_mem() {
  char b1[32], b2[32];
  kprintf("  frames: %s in use, %s total, %u frames free\n",
          khuman(metal_heap_used(), b1, sizeof(b1)),
          khuman(metal_heap_total(), b2, sizeof(b2)), metal_heap_frames_free());

  /* Exercise the allocator and the live tables rather than describe them. */
  const uint64_t free_before = metal_heap_frames_free();
  void* a = metal_alloc_pages(4096 * 4);
  void* b = metal_alloc_pages(4096);
  if (!a || !b) { kputs("  allocation FAILED\n"); return; }
  *reinterpret_cast<volatile uint32_t*>(a) = 0x12345678u;
  *reinterpret_cast<volatile uint32_t*>(b) = 0x87654321u;
  metal_free_pages(b, 4096);
  metal_free_pages(a, 4096 * 4);
  const uint64_t free_after = metal_heap_frames_free();
  if (free_before != free_after) {
    kprintf("  LEAK: free was %u frames, now %u frames\n", free_before, free_after);
  } else {
    kputs("  allocator: 5-frame alloc + write + free cleanly recovered\n");
  }
}

void cmd_dt() {
  const FdtInfo& f = fdt_info();
  char b[32];
  kprintf("  fdt at %p (%u bytes)\n", f.dtb, static_cast<uint64_t>(f.dtb_size));
  kprintf("  model: %s\n", f.model[0] ? f.model : "(none)");
  kprintf("  ram:   %p .. %p (%s)\n", f.ram_base, f.ram_base + f.ram_size,
          khuman(f.ram_size, b, sizeof(b)));
  kprintf("  gicd:  %p\n", f.gicd_base);
  kprintf("  gicr:  %p\n", f.gicr_base);
  kprintf("  uart:  %p\n", f.uart_base);
  kprintf("  cpus:  %u\n", static_cast<uint64_t>(f.cpu_count));
}

void cmd_irq() {
  kprintf("  irq: %u Hz configured, %s\n",
          static_cast<uint64_t>(metal_tick_hz()),
          metal_interrupts_live() ? "live" : "offline");
  kprintf("  ticks (cpu0): %u\n", metal_ticks());
  kprintf("  spurious:     %u\n", metal_spurious());
  kprintf("  fault count:  %u\n", metal_fault_count());
}

void cmd_time() {
  const uint64_t t0 = metal_now_ns();
  metal_delay_us(100000);
  const uint64_t elapsed_ns = metal_now_ns() - t0;
  kprintf("  delay(100ms) measured: %u us (%u ns)\n", elapsed_ns / 1000, elapsed_ns);
}

void cmd_fault() {
  kputs("  taking a deliberate data abort from EL1...\n");
  metal_console_flush();
  volatile uint32_t* p = reinterpret_cast<volatile uint32_t*>(0x00000000DEAD0000ull);
  *p = 42;
}

void cmd_wifi(const char* args) {
  while (*args == ' ') ++args;
  if (!*args || streq(args, "status")) {
    bcm43438_report();
    dhd_report();
    return;
  }
  if (streq(args, "scan")) {
    static NetScanEntry aps[32];
    size_t count = 0;
    net_scan(aps, 32, count);
    kprintf("  wifi scan: found %u access points\n", static_cast<uint64_t>(count));
    for (size_t i = 0; i < count; ++i) {
      kprintf("    [%u] SSID: %s | CH: %u | RSSI: %d dBm\n",
              static_cast<uint64_t>(i + 1), aps[i].ssid,
              static_cast<uint64_t>(aps[i].channel),
              static_cast<uint64_t>(aps[i].rssi));
    }
    return;
  }
  if (streq(args, "probe")) {
    kputs("  probing BCM43438 backplane write mechanisms...\n");
    g_probe_writes = true;
    bcm43438_init();
    g_probe_writes = false;
    return;
  }
  if (streq(args, "init")) {
    kputs("  reinitializing BCM43438 wireless...\n");
    bcm43438_init();
    dhd_init();
    bcm43438_report();
    return;
  }
  if (streq(args, "tx")) {
    kputs("  dispatching ARP probe frame over Function 2...\n");
    net_send_test_frame();
    return;
  }
  if (streq(args, "disassoc")) {
    net_disassociate();
    return;
  }
  if (args[0] == 'c' && args[1] == 'o' && args[2] == 'n' && args[3] == 'n' && args[4] == 'e' && args[5] == 'c' && args[6] == 't') {
    /* wifi connect SSID [PASSPHRASE]  |  wifi connect "SSID WITH SPACES" [PASSPHRASE]
     * No passphrase = open network. */
    const char* p = args + 7;
    while (*p == ' ') ++p;
    if (!*p) {
      kputs("  usage: wifi connect <SSID> [passphrase]   (quote an SSID containing spaces)\n");
      return;
    }
    static char ssid[33];
    static char psk[65];
    size_t n = 0;
    if (*p == '"') {
      ++p;
      while (*p && *p != '"' && n < 32) ssid[n++] = *p++;
      if (*p == '"') ++p;
    } else {
      while (*p && *p != ' ' && n < 32) ssid[n++] = *p++;
    }
    ssid[n] = '\0';
    while (*p == ' ') ++p;
    size_t m = 0;
    while (*p && m < 64) psk[m++] = *p++;
    while (m && psk[m - 1] == ' ') --m;
    psk[m] = '\0';
    kprintf("  associating with '%s' (%s)...\n", ssid, m ? "WPA2-PSK" : "open");
    const bool ok = net_associate(ssid, m ? psk : nullptr);
    kprintf("  association %s\n", ok ? "complete - DHCP started" : "FAILED - see events above / wifi log");
    return;
  }
  if (streq(args, "log")) {
    const char* lines[32];
    const size_t k = net_wifi_log(lines, 32);
    for (size_t i = 0; i < k; ++i) kprintf("  %s\n", lines[i]);
    return;
  }
  kputs("  usage: wifi [status | scan | connect <ssid> [psk] | disassoc | log | tx | init | probe]\n");
}

void cmd_net(const char* args) {
  while (*args == ' ') ++args;
  if (!*args || streq(args, "status")) {
    net_report();
    return;
  }
  if (streq(args, "scan")) {
    cmd_wifi("scan");
    return;
  }
  if (streq(args, "tx")) {
    net_send_test_frame();
    return;
  }
  if (streq(args, "up")) {
    /* eth0 ONLY - deliberately net_init_eth_only(), not net_init(). This
     * used to call net_init(), which always ALSO runs dhd_init()'s five-
     * variation WiFi retry ladder: dozens of [wifi]/[sdio] lines that push
     * the much shorter eth0 output out of the log ring, making "net up"
     * useless for looking at eth0 specifically. Use "wifi init" for wlan0.
     *
     * This also used to just call net_init(), which only re-reads software
     * state (lan9514_info().initialized) - it never touched the hardware,
     * so "net up" could not actually recover a LAN9514 that failed to come
     * up at boot. Force it off its USB hub port first, so the next
     * usb_hid_poll() rescan runs the real PHY bring-up from scratch. */
    usb_hid_reinit_ethernet();
    kputs("  forcing eth0 back through USB re-enumeration...\n");
    for (unsigned i = 0; i < 20 && !usb_hid_status().ethernet_detected; ++i) {
      usb_hid_poll();
      metal_delay_us(200000);
    }
    net_init_eth_only();
    return;
  }
  if (streq(args, "dhcp")) {
    kputs("  triggering DHCP discovery on eth0 and wlan0...\n");
    net_start_dhcp(NetIfType::Ethernet);
    net_start_dhcp(NetIfType::Wifi);
    return;
  }
  kputs("  usage: net [status | scan | tx | rx | up | dhcp]\n");
}

void cmd_ping(const char* args) {
  while (*args == ' ') ++args;
  if (!*args) {
    kputs("  usage: ping <IP address> (e.g. ping 192.168.1.1)\n");
    return;
  }
  uint8_t target_ip[4] = {};
  unsigned oct = 0, val = 0;
  const char* s = args;
  while (*s && oct < 4) {
    if (*s >= '0' && *s <= '9') {
      val = val * 10 + (*s - '0');
    } else if (*s == '.') {
      target_ip[oct++] = static_cast<uint8_t>(val);
      val = 0;
    } else break;
    ++s;
  }
  if (oct < 4) target_ip[oct] = static_cast<uint8_t>(val);

  char ip_s[20];
  ip_to_str(target_ip, ip_s, sizeof(ip_s));
  kprintf("  PING %s (64 bytes ICMP echo)...\n", ip_s);
  uint32_t rtt_us = 0;
  if (net_ping(target_ip, 1000, rtt_us)) {
    kprintf("  64 bytes from %s: rtt=%u.%u ms\n", ip_s, rtt_us / 1000, (rtt_us % 1000) / 10);
  } else {
    kputs("  request timed out (no reply received)\n");
  }
}

void cmd_tcp(const char* args) {
  while (*args == ' ') ++args;
  if (!*args || streq(args, "status")) {
    TcpSocketInfo socks[TCP_MAX_SOCKETS];
    size_t n_socks = net_tcp_get_all_info(socks, TCP_MAX_SOCKETS);
    if (n_socks == 0) {
      kputs("  no active TCP sockets (use 'tcp listen <port>' or 'tcp connect <ip> <port>')\n");
    } else {
      kprintf("  active TCP sockets (%u):\n", static_cast<uint64_t>(n_socks));
      for (size_t i = 0; i < n_socks; ++i) {
        char rip_s[20];
        ip_to_str(socks[i].remote_ip, rip_s, sizeof(rip_s));
        kprintf("    [%d] :%u -> %s:%u (%s, rx: %u, tx: %u)\n",
                socks[i].id, socks[i].local_port, rip_s, socks[i].remote_port,
                tcp_state_str(socks[i].state), socks[i].rx_pending, socks[i].tx_pending);
      }
    }
    return;
  }
  if (args[0] == 'l' && args[1] == 'i' && args[2] == 's' && args[3] == 't' && args[4] == 'e' && args[5] == 'n') {
    const char* p = args + 6;
    while (*p == ' ') ++p;
    uint16_t port = 0;
    while (*p >= '0' && *p <= '9') { port = port * 10 + static_cast<uint16_t>(*p - '0'); ++p; }
    if (port == 0) { kputs("  usage: tcp listen <port>\n"); return; }
    int id = net_tcp_listen(port);
    if (id > 0) kprintf("  TCP listener started on port %u (sock %d)\n", static_cast<uint64_t>(port), id);
    else kputs("  failed to start TCP listener\n");
    return;
  }
  if (args[0] == 'c' && args[1] == 'l' && args[2] == 'o' && args[3] == 's' && args[4] == 'e') {
    const char* p = args + 5;
    while (*p == ' ') ++p;
    int id = 0;
    while (*p >= '0' && *p <= '9') { id = id * 10 + (*p - '0'); ++p; }
    if (id <= 0) { kputs("  usage: tcp close <sock-id>\n"); return; }
    net_tcp_close(id);
    kprintf("  closed TCP socket %d\n", id);
    return;
  }
  kputs("  usage: tcp [status | listen <port> | close <sock-id>]\n");
}

void cmd_ls() {
  Fat32FileInfo entries[16];
  size_t count = fat32_list("/", entries, 16);
  kprintf("  listing directory [/sd/] (%u entries):\n", static_cast<uint64_t>(count));
  for (size_t i = 0; i < count; ++i) {
    char sz[32];
    if (entries[i].is_directory) {
      kprintf("    [DIR]  %-16s (cluster %u)\n", entries[i].name, static_cast<uint64_t>(entries[i].first_cluster));
    } else {
      khuman(entries[i].size_bytes, sz, sizeof(sz));
      kprintf("    [FILE] %-16s %8s (cluster %u)\n", entries[i].name, sz, static_cast<uint64_t>(entries[i].first_cluster));
    }
  }
}

/* fs [cat NAME | write NAME TEXT | rm NAME] - the FAT32 volume, driven for
 * real from the console so a write can be checked against the card by
 * another machine afterwards. */
void cmd_fs(const char* args) {
  while (*args == ' ') ++args;
  if (!*args) { fat32_report(); return; }
  auto starts = [](const char* s, const char* pfx) {
    while (*pfx) { if (*s++ != *pfx++) return false; }
    return true;
  };
  if (starts(args, "cat ")) {
    const char* name = args + 4;
    while (*name == ' ') ++name;
    static uint8_t buf[4096];
    size_t n = 0;
    if (!fat32_read_file(name, buf, sizeof(buf) - 1, n)) { kprintf("  fs: cannot read '%s'\n", name); return; }
    buf[n] = 0;
    kprintf("  %s (%u bytes):\n", name, static_cast<uint64_t>(n));
    kputs(reinterpret_cast<const char*>(buf));
    if (n && buf[n - 1] != '\n') kputc('\n');
    return;
  }
  if (starts(args, "write ")) {
    const char* name = args + 6;
    while (*name == ' ') ++name;
    const char* text = name;
    while (*text && *text != ' ') ++text;
    char fname[64];
    size_t fl = 0;
    while (name + fl < text && fl + 1 < sizeof(fname)) { fname[fl] = name[fl]; ++fl; }
    fname[fl] = 0;
    while (*text == ' ') ++text;
    size_t tl = 0;
    while (text[tl]) ++tl;
    const bool ok = fat32_write_file(fname, text, tl);
    kprintf("  fs: write '%s' (%u bytes): %s\n", fname, static_cast<uint64_t>(tl), ok ? "ok" : "FAILED");
    return;
  }
  if (starts(args, "b64 ")) {
    /* fs b64 NAME [new] BASE64: append decoded bytes to NAME (or start it
     * fresh with `new`). How a script larger than one console line gets
     * onto the card - metal/tools/pi-grain.py drives it. */
    const char* q = args + 4;
    char fname[16]; size_t fl = 0;
    while (*q && *q != ' ' && fl + 1 < sizeof(fname)) fname[fl++] = *q++;
    fname[fl] = 0;
    while (*q == ' ') ++q;
    bool fresh = false;
    if (q[0] == 'n' && q[1] == 'e' && q[2] == 'w' && q[3] == ' ') { fresh = true; q += 4; }
    if (!fresh && *q == '\0') {                    /* no data: a size query - read only */
      size_t size = 0;
      const bool exists = fat32_file_size(fname, size);
      kprintf("  fs: b64 %s: %s, %u bytes\n", fname, exists ? "ok" : "absent", static_cast<uint64_t>(size));
      return;
    }
    static uint8_t buf[256 * 1024];
    size_t have = 0;
    if (!fresh) { size_t got = 0; if (fat32_read_file(fname, buf, sizeof(buf), got)) have = got; }
    auto val = [](char c) -> int {
      if (c >= 'A' && c <= 'Z') return c - 'A'; if (c >= 'a' && c <= 'z') return c - 'a' + 26;
      if (c >= '0' && c <= '9') return c - '0' + 52; if (c == '+') return 62; if (c == '/') return 63; return -1; };
    uint32_t acc = 0; int bits = 0;
    bool overflow = false;
    for (; *q; ++q) {
      const int v = val(*q);
      if (v < 0) continue;
      acc = (acc << 6) | static_cast<uint32_t>(v); bits += 6;
      if (bits >= 8) {
        bits -= 8;
        if (have < sizeof(buf)) buf[have++] = static_cast<uint8_t>(acc >> bits);
        else overflow = true;
      }
    }
    /* Past 256 KB the bytes used to be dropped and the file written short
     * with "ok" - a truncated program that then failed somewhere strange. */
    if (overflow) { kprintf("  fs: b64 %s: FAILED, file would pass %u bytes\n", fname, static_cast<uint64_t>(sizeof(buf))); return; }
    const bool ok = fat32_write_file(fname, buf, have);
    kprintf("  fs: b64 %s: %s, %u bytes\n", fname, ok ? "ok" : "FAILED", static_cast<uint64_t>(have));
    return;
  }
  if (starts(args, "put ")) {
    /* fs put NAME new [B64] / fs put NAME B64 / fs put NAME end: an upload
     * staged in RAM and written to the card once, at `end`. (fs b64 rewrites
     * the whole file for every chunk - quadratic, 36 s for 100 KB.) `fs put
     * NAME` alone reports what is staged. tools/grain_remote.py drives it. */
    static char staged_name[16] = "";
    static uint8_t staged[256 * 1024];
    static size_t staged_len = 0;
    const char* q = args + 4;
    char fname[16]; size_t fl = 0;
    while (*q && *q != ' ' && fl + 1 < sizeof(fname)) fname[fl++] = *q++;
    fname[fl] = 0;
    while (*q == ' ') ++q;
    const bool fresh = q[0] == 'n' && q[1] == 'e' && q[2] == 'w' && (q[3] == ' ' || q[3] == '\0');
    const bool end = q[0] == 'e' && q[1] == 'n' && q[2] == 'd' && q[3] == '\0';
    if (fresh) { q += 3; while (*q == ' ') ++q; for (size_t i = 0; i <= fl; ++i) staged_name[i] = fname[i]; staged_len = 0; }
    if (!streq(staged_name, fname)) { kprintf("  fs: put %s: FAILED, nothing staged under that name (start with `new`)\n", fname); return; }
    if (end) {
      const bool ok = fat32_write_file(fname, staged, staged_len);
      size_t on_card = 0;
      const bool sized = fat32_file_size(fname, on_card);
      kprintf("  fs: put %s: %s, %u bytes written\n", fname, ok && sized && on_card == staged_len ? "ok" : "FAILED",
              static_cast<uint64_t>(staged_len));
      staged_name[0] = '\0';
      staged_len = 0;
      return;
    }
    auto val = [](char c) -> int {
      if (c >= 'A' && c <= 'Z') return c - 'A'; if (c >= 'a' && c <= 'z') return c - 'a' + 26;
      if (c >= '0' && c <= '9') return c - '0' + 52; if (c == '+') return 62; if (c == '/') return 63; return -1; };
    uint32_t acc = 0; int bits = 0;
    for (; *q; ++q) {
      const int v = val(*q);
      if (v < 0) continue;
      acc = (acc << 6) | static_cast<uint32_t>(v); bits += 6;
      if (bits >= 8) {
        bits -= 8;
        if (staged_len >= sizeof(staged)) {
          kprintf("  fs: put %s: FAILED, larger than %u bytes\n", fname, static_cast<uint64_t>(sizeof(staged)));
          staged_name[0] = '\0'; staged_len = 0;
          return;
        }
        staged[staged_len++] = static_cast<uint8_t>(acc >> bits);
      }
    }
    kprintf("  fs: put %s: staged %u bytes\n", fname, static_cast<uint64_t>(staged_len));
    return;
  }
  if (starts(args, "get ")) {
    /* fs get NAME [OFFSET]: up to 36 KB of NAME from OFFSET as base64, with
     * the file's total size - how a client pulls a file back exactly
     * (tools/grain_remote.py), in as many calls as it takes. */
    const char* q = args + 4;
    char fname[16]; size_t fl = 0;
    while (*q && *q != ' ' && fl + 1 < sizeof(fname)) fname[fl++] = *q++;
    fname[fl] = 0;
    while (*q == ' ') ++q;
    size_t off = 0; while (*q >= '0' && *q <= '9') off = off * 10 + static_cast<size_t>(*q++ - '0');
    static uint8_t buf[256 * 1024];
    size_t got = 0;
    size_t size = 0;
    if (!fat32_file_size(fname, size) || !fat32_read_file(fname, buf, sizeof(buf), got)) { kprintf("  fs: get %s: no such file\n", fname); return; }
    if (size > sizeof(buf)) { kprintf("  fs: get %s: larger than %u bytes\n", fname, static_cast<uint64_t>(sizeof(buf))); return; }
    constexpr size_t kSlice = 36 * 1024;
    const size_t start = off < got ? off : got;
    const size_t n = got - start < kSlice ? got - start : kSlice;
    static char enc[kSlice / 3 * 4 + 8];
    static const char* const tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t e = 0;
    for (size_t i = 0; i < n; i += 3) {
      const uint32_t b0 = buf[start + i], b1 = i + 1 < n ? buf[start + i + 1] : 0, b2 = i + 2 < n ? buf[start + i + 2] : 0;
      const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
      enc[e++] = tbl[(v >> 18) & 63];
      enc[e++] = tbl[(v >> 12) & 63];
      enc[e++] = i + 1 < n ? tbl[(v >> 6) & 63] : '=';
      enc[e++] = i + 2 < n ? tbl[v & 63] : '=';
    }
    enc[e] = 0;
    kprintf("  fs: get %s size=%u off=%u len=%u b64=%s\n", fname, static_cast<uint64_t>(got), static_cast<uint64_t>(start),
            static_cast<uint64_t>(n), enc);
    return;
  }
  if (starts(args, "fill ")) {
    /* fs fill NAME BYTES: write a known pattern (multi-cluster test). */
    const char* q = args + 5;
    char fname[16]; size_t fl = 0;
    while (*q && *q != ' ' && fl + 1 < sizeof(fname)) fname[fl++] = *q++;
    fname[fl] = 0;
    while (*q == ' ') ++q;
    size_t n = 0; while (*q >= '0' && *q <= '9') n = n * 10 + static_cast<size_t>(*q++ - '0');
    if (n == 0 || n > 262144) { kputs("  fs fill: size must be 1..262144\n"); return; }
    auto* buf = static_cast<uint8_t*>(metal_alloc_pages((n + 4095) & ~static_cast<size_t>(4095)));
    if (!buf) { kputs("  fs fill: out of memory\n"); return; }
    uint32_t sum = 0;
    for (size_t i = 0; i < n; ++i) { buf[i] = static_cast<uint8_t>((i * 31 + (i >> 9)) & 0xFF); sum = sum * 33u + buf[i]; }
    const bool ok = fat32_write_file(fname, buf, n);
    metal_free_pages(buf, (n + 4095) & ~static_cast<size_t>(4095));
    kprintf("  fs: fill '%s' %u bytes: %s, checksum %08x\n", fname, static_cast<uint64_t>(n), ok ? "ok" : "FAILED", static_cast<uint64_t>(sum));
    return;
  }
  if (starts(args, "sum ")) {
    /* fs sum NAME: read the whole file back and checksum it. */
    const char* fname = args + 4;
    const size_t cap = 262144;
    auto* buf = static_cast<uint8_t*>(metal_alloc_pages(cap));
    if (!buf) { kputs("  fs sum: out of memory\n"); return; }
    size_t got = 0;
    const bool ok = fat32_read_file(fname, buf, cap, got);
    uint32_t sum = 0;
    for (size_t i = 0; i < got; ++i) sum = sum * 33u + buf[i];
    metal_free_pages(buf, cap);
    kprintf("  fs: sum '%s': %s, %u bytes, checksum %08x\n", fname, ok ? "read" : "FAILED", static_cast<uint64_t>(got), static_cast<uint64_t>(sum));
    return;
  }
  if (starts(args, "rm ")) {
    const char* name = args + 3;
    while (*name == ' ') ++name;
    kprintf("  fs: rm '%s': %s\n", name, fat32_delete_file(name) ? "ok" : "FAILED");
    return;
  }
  kputs("  fs | fs cat NAME | fs write NAME TEXT | fs rm NAME\n");
}

void cmd_gpio(const char* args) {
  while (*args == ' ') ++args;

  if (!*args || streq(args, "layout") || streq(args, "pinout") || streq(args, "render")) {
    gpio_render_pinout_ansi();
    return;
  }
  if (streq(args, "list") || streq(args, "status")) {
    gpio_report_list();
    return;
  }
  if (streq(args, "ack") || streq(args, "inspect") || streq(args, "audit")) {
    gpio_inspect_and_acknowledge();
    return;
  }
  if (streq(args, "scan") || streq(args, "i2c")) {
    kputs("  scanning I2C Bus 1 (GPIO 2 SDA / GPIO 3 SCL)...\n");
    GpioI2cDevice devs[16];
    size_t count = gpio_i2c_scan_bus(2, 3, devs, 16);
    if (count == 0) {
      kputs("  no I2C slave devices responded with ACK on bus 1\n");
    } else {
      kprintf("  found %u I2C device(s):\n", static_cast<uint64_t>(count));
      for (size_t i = 0; i < count; ++i) {
        kprintf("    [0x%02X] %-18s: %s\n", devs[i].addr, devs[i].name, devs[i].description);
      }
    }
    return;
  }

  auto parse_pin = [](const char*& p) -> int {
    while (*p == ' ') ++p;
    if (!*p) return -1;
    bool is_phys = false;
    if (*p == 'p' || *p == 'P') { is_phys = true; ++p; }
    int val = 0;
    bool any = false;
    while (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); ++p; any = true; }
    if (!any) return -1;
    if (is_phys) return gpio_phys_to_bcm(static_cast<uint8_t>(val));
    return val;
  };

  if (args[0] == 'r' && args[1] == 'e' && args[2] == 'a' && args[3] == 'd') {
    const char* p = args + 4;
    int pin = parse_pin(p);
    if (pin < 0 || pin > 53) { kputs("  usage: pin read <bcm-pin | p<phys-pin>>\n"); return; }
    bool val = gpio_read(static_cast<unsigned>(pin));
    GpioMode mode = gpio_get_mode(static_cast<unsigned>(pin));
    kprintf("  GPIO %d: logic level %s (%d), mode %u\n", pin, val ? "HIGH" : "LOW", val ? 1 : 0, static_cast<uint32_t>(mode));
    return;
  }

  if (args[0] == 'w' && args[1] == 'r' && args[2] == 'i' && args[3] == 't' && args[4] == 'e') {
    const char* p = args + 5;
    int pin = parse_pin(p);
    while (*p == ' ') ++p;
    if (pin < 0 || pin > 53 || (*p != '0' && *p != '1')) {
      kputs("  usage: pin write <pin> <0 | 1>\n");
      return;
    }
    bool val = (*p == '1');
    gpio_set_mode(static_cast<unsigned>(pin), GpioMode::Output);
    gpio_write(static_cast<unsigned>(pin), val);
    kprintf("  GPIO %d: set to OUTPUT, written %s (%d)\n", pin, val ? "HIGH" : "LOW", val ? 1 : 0);
    return;
  }

  if (args[0] == 's' && args[1] == 'e' && args[2] == 't') {
    const char* p = args + 3;
    int pin = parse_pin(p);
    if (pin < 0 || pin > 53) { kputs("  usage: pin set <pin>\n"); return; }
    gpio_set_mode(static_cast<unsigned>(pin), GpioMode::Output);
    gpio_write(static_cast<unsigned>(pin), true);
    kprintf("  GPIO %d: set to OUTPUT HIGH (1)\n", pin);
    return;
  }

  if (args[0] == 'c' && args[1] == 'l' && args[2] == 'r') {
    const char* p = args + 3;
    int pin = parse_pin(p);
    if (pin < 0 || pin > 53) { kputs("  usage: pin clr <pin>\n"); return; }
    gpio_set_mode(static_cast<unsigned>(pin), GpioMode::Output);
    gpio_write(static_cast<unsigned>(pin), false);
    kprintf("  GPIO %d: set to OUTPUT LOW (0)\n", pin);
    return;
  }

  if (args[0] == 't' && args[1] == 'o' && args[2] == 'g' && args[3] == 'g' && args[4] == 'l' && args[5] == 'e') {
    const char* p = args + 6;
    int pin = parse_pin(p);
    if (pin < 0 || pin > 53) { kputs("  usage: pin toggle <pin>\n"); return; }
    gpio_set_mode(static_cast<unsigned>(pin), GpioMode::Output);
    gpio_toggle(static_cast<unsigned>(pin));
    bool val = gpio_read(static_cast<unsigned>(pin));
    kprintf("  GPIO %d: toggled -> now %s (%d)\n", pin, val ? "HIGH" : "LOW", val ? 1 : 0);
    return;
  }

  if (args[0] == 'm' && args[1] == 'o' && args[2] == 'd' && args[3] == 'e') {
    const char* p = args + 4;
    int pin = parse_pin(p);
    while (*p == ' ') ++p;
    GpioMode mode = GpioMode::Input;
    if (streq(p, "in") || streq(p, "input")) mode = GpioMode::Input;
    else if (streq(p, "out") || streq(p, "output")) mode = GpioMode::Output;
    else if (streq(p, "alt0")) mode = GpioMode::Alt0;
    else if (streq(p, "alt1")) mode = GpioMode::Alt1;
    else if (streq(p, "alt2")) mode = GpioMode::Alt2;
    else if (streq(p, "alt3")) mode = GpioMode::Alt3;
    else if (streq(p, "alt4")) mode = GpioMode::Alt4;
    else if (streq(p, "alt5")) mode = GpioMode::Alt5;
    else {
      kputs("  usage: pin mode <pin> <in | out | alt0..alt5>\n");
      return;
    }
    gpio_set_mode(static_cast<unsigned>(pin), mode);
    kprintf("  GPIO %d: mode configured to %s\n", pin, p);
    return;
  }

  if (args[0] == 'p' && args[1] == 'u' && args[2] == 'l' && args[3] == 'l') {
    const char* p = args + 4;
    int pin = parse_pin(p);
    while (*p == ' ') ++p;
    GpioPull pull = GpioPull::None;
    if (streq(p, "up")) pull = GpioPull::Up;
    else if (streq(p, "down")) pull = GpioPull::Down;
    else if (streq(p, "none") || streq(p, "off")) pull = GpioPull::None;
    else {
      kputs("  usage: pin pull <pin> <none | up | down>\n");
      return;
    }
    gpio_set_pull(static_cast<unsigned>(pin), pull);
    kprintf("  GPIO %d: pull resistor set to %s\n", pin, p);
    return;
  }

  if (args[0] == 'p' && args[1] == 'r' && args[2] == 'o' && args[3] == 'b' && args[4] == 'e') {
    const char* p = args + 5;
    int pin = parse_pin(p);
    if (pin < 0 || pin > 53) { kputs("  usage: pin probe <pin>\n"); return; }
    gpio_report_pin(static_cast<unsigned>(pin));
    return;
  }

  /* Direct single pin report */
  const char* p = args;
  int pin = parse_pin(p);
  if (pin >= 0 && pin <= 53) {
    gpio_report_pin(static_cast<unsigned>(pin));
    return;
  }

  kputs("  usage: pin [layout | list | ack | scan | read <pin> | write <pin> <0|1> | toggle <pin> | mode <pin> <in|out|alt0..5> | pull <pin> <none|up|down> | probe <pin>]\n");
}

void cmd_help() {
  kputs("  desktop the real AIOS windowed desktop (HardwareCompositor) on the framebuffer\n"
        "  gui     alias for desktop\n"
        "  dash    the bring-up diagnostic dashboard (hardware tabs, not app windows)\n"
        "  pin     Raspberry Pi 40-pin header layout, live voltages & hardware pin control\n"
        "  gpio    alias for pin (layout | list | ack | scan | read | write | toggle | mode | pull)\n"
        "  pinout  render ASCII/ANSI 40-pin expansion header layout\n"
        "  info    what this machine is, measured\n"
        "  mem     frame allocator and page tables, exercised\n"
        "  dt      what the device tree actually said\n"
        "  irq     timer interrupts counted against the clock\n"
        "  time    generic timer\n"
        "  neon    NEON kernels checked against scalar\n"
        "  fb      what the display is and where its pixels live\n"
        "  gfx     paint a test pattern and time the flush to the glass\n"
        "  cls     clear the screen\n"
        "  blk     virtio-mmio block device: identify, write, read back\n"
        "  sdio    SDIO host controller and card function report\n"
        "  wifi    BCM43438 DHD wireless management and telemetry\n"
        "  net     bare-metal network device and packet ring buffers\n"
        "  ping    ICMP echo ping diagnostic probe\n"
        "  tcp     TCP state machine, socket horizon, and listeners\n"
        "  dhcp    broadcast DHCPDISCOVER leases on active interfaces\n"
        "  usb     Synopsys DWC2 USB host controller and HID devices\n"
        "  sd      MicroSD block storage driver status\n"
        "  fat32   FAT32 filesystem and volume status\n"
        "  ls      list files in root directory on SD card (/sd/)\n"
        "  fs      FAT32 volume report; fs cat NAME | fs write NAME TEXT | fs rm NAME\n"
        "  ps      embedded EL0 programs and process table\n"
        "  run N   run embedded EL0 program N\n"
        "  entity  real STL src/entity::EntityManager smoke test\n"
        "  grain <source...>   this board's own language, live against the running Kernel - no rebuild\n"
        "  fleet H P K   attach to a fleet hub at H:P with cluster key K (stays attached)\n"
        "  fleet         (no args) report this boot's attach status and roster\n"
        "  node          report NodeService status - peers actually serving distributed work here\n"
        "  node start [<token>|@FILE]  serve peers that present the token (none given: NODE.KEY on the card; refuses without one)\n"
        "  console debug on|off   unlock/lock the open UDP console (locked by default each boot - use\n"
        "                         `fleet run <peer> ...` or this physical console to reach it either way)\n"
        "  smp     bring up the secondary cores (PSCI, or the Pi's spin table)\n"
        "  cpus    which cores are up; `cpus test` runs a job on each at once\n"
        "  fault   take a deliberate data abort\n"
        "  halt    PSCI SYSTEM_OFF\n"
        "  reset   PSCI SYSTEM_RESET\n");
}

}  /* namespace */

/* Not in the anonymous namespace above - net.cpp's UDP console calls this
 * too, via shell.hpp. It can still see every cmd_* function: they were
 * declared earlier in this same translation unit, anonymous namespace or
 * not. */
/* The core's real speed, measured: PMU cycle counter against the fixed
 * 19.2 MHz system counter over 20 ms - not the firmware's word for it. */
uint32_t measure_cpu_mhz() {
  uint64_t pmcr;
  asm volatile("mrs %0, pmcr_el0" : "=r"(pmcr));
  asm volatile("msr pmcr_el0, %0" :: "r"(pmcr | 1ull));           /* E: counters on */
  asm volatile("msr pmcntenset_el0, %0" :: "r"(1ull << 31));      /* cycle counter */
  asm volatile("isb");
  uint64_t c0, c1;
  const uint64_t t0 = metal_cntpct();
  asm volatile("isb; mrs %0, pmccntr_el0" : "=r"(c0));
  metal_delay_us(20000);
  asm volatile("isb; mrs %0, pmccntr_el0" : "=r"(c1));
  const uint64_t ticks = metal_cntpct() - t0;
  if (!ticks) return 0;
  return static_cast<uint32_t>((c1 - c0) * metal_cntfrq() / ticks / 1000000ull);
}

/* The firmware starts the ARM cores at their minimum clock (600 MHz on a
 * 3B) and leaves raising it to the OS - Linux's cpufreq does. Nothing here
 * did, so every instruction this kernel ran, ran at half speed. This asks
 * for the firmware's own maximum (arm_freq), never more. */
void cpu_clock_to_max() {
  uint32_t cur = 0, max = 0, now = 0;
  rpi_get_clock_rate(RPI_CLOCK_ARM, cur);
  if (!rpi_get_max_clock_rate(RPI_CLOCK_ARM, max) || max == 0) {
    kprintf("[boot] arm clock: firmware gave no maximum; staying at %u MHz\n", static_cast<uint64_t>(cur / 1000000));
    return;
  }
  if (max > cur) rpi_set_clock_rate(RPI_CLOCK_ARM, max);
  rpi_get_clock_rate(RPI_CLOCK_ARM, now);
  kprintf("[boot] arm clock: %u -> %u MHz (firmware max %u), measured %u MHz\n", static_cast<uint64_t>(cur / 1000000),
          static_cast<uint64_t>(now / 1000000), static_cast<uint64_t>(max / 1000000), static_cast<uint64_t>(measure_cpu_mhz()));
}

void cmd_cpu() {
  uint32_t cur = 0, max = 0, temp = 0, thr = 0;
  rpi_get_clock_rate(RPI_CLOCK_ARM, cur);
  rpi_get_max_clock_rate(RPI_CLOCK_ARM, max);
  kprintf("  arm clock   : %u MHz firmware (max %u), %u MHz measured\n", static_cast<uint64_t>(cur / 1000000),
          static_cast<uint64_t>(max / 1000000), static_cast<uint64_t>(measure_cpu_mhz()));
  if (rpi_get_temperature(temp)) kprintf("  soc temp    : %u.%u C\n", static_cast<uint64_t>(temp / 1000), static_cast<uint64_t>(temp % 1000 / 100));
  if (rpi_get_throttled(thr)) {
    kprintf("  throttle    : 0x%x%s%s%s%s\n", static_cast<uint64_t>(thr), (thr & 1) ? " UNDER-VOLTAGE" : "", (thr & 2) ? " FREQ-CAPPED" : "",
            (thr & 4) ? " THROTTLED" : "", (thr & 0xF0000) ? " (has happened since boot)" : "");
  }
}

void dispatch_shell_command(const char* cmd) {
  while (*cmd == ' ') ++cmd;

  if (streq(cmd, "desktop") || streq(cmd, "gui")) compositor_run();
  else if (streq(cmd, "desktop exit")) compositor_request_exit();
  else if (streq(cmd, "dash")) desktop_run();
  else if (streq(cmd, "help") || streq(cmd, "?")) cmd_help();
  else if (streq(cmd, "pinout")) gpio_render_pinout_ansi();
  else if (cmd[0] == 'p' && cmd[1] == 'i' && cmd[2] == 'n' && (cmd[3] == ' ' || cmd[3] == '\0')) cmd_gpio(cmd + 3);
  else if (cmd[0] == 'g' && cmd[1] == 'p' && cmd[2] == 'i' && cmd[3] == 'o' && (cmd[4] == ' ' || cmd[4] == '\0')) cmd_gpio(cmd + 4);
  else if (streq(cmd, "info")) cmd_info();
  else if (streq(cmd, "cpu")) cmd_cpu();
  else if (streq(cmd, "bench")) bench_run();
  else if (streq(cmd, "mem")) cmd_mem();
  else if (streq(cmd, "dt")) cmd_dt();
  else if (streq(cmd, "irq")) cmd_irq();
  else if (streq(cmd, "time")) cmd_time();
  else if (streq(cmd, "neon")) neon_selftest();
  else if (streq(cmd, "fb")) fb_report();
  else if (streq(cmd, "gfx")) fb_demo();
  else if (streq(cmd, "cls")) fbcon_reset();
  else if (streq(cmd, "blk")) blk_report();
  else if (streq(cmd, "sdio")) sdio_report();
  else if (cmd[0] == 'w' && cmd[1] == 'i' && cmd[2] == 'f' && cmd[3] == 'i') cmd_wifi(cmd + 4);
  else if (cmd[0] == 'p' && cmd[1] == 'i' && cmd[2] == 'n' && cmd[3] == 'g') cmd_ping(cmd + 4);
  else if (cmd[0] == 't' && cmd[1] == 'c' && cmd[2] == 'p') cmd_tcp(cmd + 3);
  else if (cmd[0] == 'n' && cmd[1] == 'e' && cmd[2] == 't' && cmd[3] == ' ' && cmd[4] == 'b' && cmd[5] == 'u') {
    /* net burst N [LEN] */
    const char* q = cmd + 9; unsigned n = 0; size_t len = 0;
    while (*q == ' ') ++q; while (*q >= '0' && *q <= '9') n = n * 10 + static_cast<unsigned>(*q++ - '0');
    while (*q == ' ') ++q; while (*q >= '0' && *q <= '9') len = len * 10 + static_cast<size_t>(*q++ - '0');
    net_burst_test(n ? n : 20, len ? len : 1400);
  }
  else if (streq(cmd, "dhcp")) {
    kputs("  triggering DHCP discovery on eth0 and wlan0...\n");
    net_start_dhcp(NetIfType::Ethernet);
    net_start_dhcp(NetIfType::Wifi);
  }
  else if (cmd[0] == 'n' && cmd[1] == 'e' && cmd[2] == 't') cmd_net(cmd + 3);
  else if (streq(cmd, "usb")) { dwc2_report(); usb_hid_report(); }
  else if (cmd[0] == 't' && cmd[1] == 'z' && (cmd[2] == 0 || cmd[2] == ' ')) {
    /* tz [NAME]: show or set the timezone (UTC, US/Eastern, US/Central,
     * US/Mountain, US/Pacific, US/Arizona); saved to the SD card. */
    const char* name = cmd[2] ? cmd + 3 : nullptr;
    if (name && *name) {
      if (!metal_set_timezone(name)) { kprintf("  tz: unknown zone '%s'\n", name); return; }
      const bool saved = fat32_status().mounted && fat32_write_file("TZ.CFG", name, [&]{ size_t n = 0; while (name[n]) ++n; return n; }());
      kprintf("  timezone %s%s\n", metal_timezone_name(), saved ? " (saved to SD)" : " (not saved: no SD card)");
    }
    const long t = time(nullptr);
    struct tm lt; localtime_r(&t, &lt);
    char buf[48]; strftime(buf, sizeof(buf), "%a %F %T %Z", &lt);
    kprintf("  %s  [%s]\n", buf, metal_wall_clock_set() ? "set by NTP" : "NOT SET - waiting for NTP");
  }
  else if (streq(cmd, "board")) {
    uint32_t rev = 0;
    const bool ok = rpi_get_board_revision(rev);
    kprintf("  board revision query: %s, rev=0x%08x\n", ok ? "ok" : "FAILED", static_cast<uint64_t>(rev));
    uint32_t ab = 0, as = 0, vb = 0, vs = 0;
    const bool am = rpi_get_memory_split(0x00010005, ab, as), vm = rpi_get_memory_split(0x00010006, vb, vs);
    kprintf("  ARM memory: %s base 0x%08x size 0x%08x (%u MiB)\n", am ? "ok" : "FAILED", static_cast<uint64_t>(ab), static_cast<uint64_t>(as), static_cast<uint64_t>(as >> 20));
    kprintf("  VC  memory: %s base 0x%08x size 0x%08x (%u MiB)\n", vm ? "ok" : "FAILED", static_cast<uint64_t>(vb), static_cast<uint64_t>(vs), static_cast<uint64_t>(vs >> 20));
    kprintf("  kernel assumes RAM 0x%08x..0x%08x\n", static_cast<uint64_t>(fdt_info().ram_base), static_cast<uint64_t>(fdt_info().ram_base + fdt_info().ram_size));
  }
  else if (cmd[0] == 'm' && cmd[1] == 'o' && cmd[2] == 'u' && cmd[3] == 's' && cmd[4] == 'e' && cmd[5] == ' ') {
    /* mouse DX DY [BTN]: a synthetic move through the USB report path. */
    int32_t v[3] = {0, 0, 0};
    const char* q = cmd + 6;
    for (int k = 0; k < 3 && *q; ++k) {
      while (*q == ' ') ++q;
      bool neg = (*q == '-'); if (neg) ++q;
      int32_t x = 0; while (*q >= '0' && *q <= '9') x = x * 10 + (*q++ - '0');
      v[k] = neg ? -x : x;
    }
    usb_hid_inject_mouse(v[0], v[1], static_cast<uint8_t>(v[2]));
    kputs("  injected\n");
  }
  else if (cmd[0] == 's' && cmd[1] == 'd' && cmd[2] == ' ' && cmd[3] == 'p' && cmd[4] == 'a' && cmd[5] == 'd' && cmd[6] == 's' && cmd[7] == ' ') {
    /* sd pads HEX: set GPIO 46-53 pad control (drive/slew/hysteresis), then
     * re-run card init + bus training to measure it. */
    uint32_t v = 0; const char* q = cmd + 8;
    if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X')) q += 2;
    for (; *q; ++q) { const char ch = *q; v = v * 16 + static_cast<uint32_t>(ch >= 'a' ? ch - 'a' + 10 : (ch >= 'A' ? ch - 'A' + 10 : ch - '0')); }
    mmio_write32(0x3F100034ull, 0x5A000000u | (v & 0x1Fu));
    kprintf("  PADS 46-53 now 0x%08x; retraining...\n", static_cast<uint64_t>(mmio_read32(0x3F100034ull)));
    kprintf("  sdcard_init: %s\n", sdcard_init() ? "OK" : "FAILED");
  }
  else if (streq(cmd, "sd init")) {
    kputs("  initializing the SD card on SDHOST and mounting FAT32...\n");
    const bool m = fat32_mount();
    kprintf("  fat32_mount: %s\n", m ? "mounted" : "FAILED");
    sdcard_report();
  }
  else if (streq(cmd, "sd")) sdcard_report();
  else if (cmd[0] == 'l' && cmd[1] == 'o' && cmd[2] == 'g' && cmd[3] == ' ') {
    /* log PREFIX: retained kernel log lines starting with PREFIX, oldest first. */
    const char* lines[60];
    const size_t n = kconsole_log(lines, 60, 0, cmd + 4);
    for (size_t i = n; i > 0; --i) { kputs(lines[i - 1]); kputs("\n"); }
    if (n == 0) kputs("  (no matching lines)\n");
  }
  else if (streq(cmd, "fat32")) fat32_report();
  else if (streq(cmd, "ls")) cmd_ls();
  else if (cmd[0] == 'f' && cmd[1] == 's' && (cmd[2] == ' ' || cmd[2] == '\0')) cmd_fs(cmd + 2);
  else if (streq(cmd, "ps")) proc_report();
  else if (streq(cmd, "entity")) entity_test_command();
  else if (cmd[0] == 'g' && cmd[1] == 'r' && cmd[2] == 'a' && cmd[3] == 'i' && cmd[4] == 'n' && (cmd[5] == ' ' || cmd[5] == '\0')) grain_eval(cmd + 5);
  else if (streq(cmd, "node start") || (cmd[0] == 'n' && cmd[1] == 'o' && cmd[2] == 'd' && cmd[3] == 'e' && cmd[4] == ' ' && cmd[5] == 's' && cmd[6] == 't' && cmd[7] == 'a' && cmd[8] == 'r' && cmd[9] == 't' && cmd[10] == ' ')) node_service_start_from(cmd + 10, false);
  else if (streq(cmd, "node")) node_service_report();
  else if (streq(cmd, "console debug on")) { console_debug_set(true); kputs("  console: UNLOCKED - the open UDP console now answers anyone on the LAN, no authentication\n"); }
  else if (streq(cmd, "console debug off")) { console_debug_set(false); kputs("  console: locked - only `fleet run` (cluster key, encrypted) and the physical console reach this board now\n"); }
  else if (streq(cmd, "console debug")) kprintf("  console: %s\n", console_debug_enabled() ? "UNLOCKED (anyone on the LAN)" : "locked (fleet run / physical console only)");
  else if (cmd[0] == 'f' && cmd[1] == 'l' && cmd[2] == 'e' && cmd[3] == 'e' && cmd[4] == 't') fleet_join_test(cmd + 5);
  else if (streq(cmd, "smp")) smp_start();
  else if (streq(cmd, "cpus")) smp_report();
  else if (streq(cmd, "cpus test")) smp_test();
  else if (streq(cmd, "fault")) cmd_fault();
  else if (streq(cmd, "halt") || streq(cmd, "exit")) {
    kputs("  PSCI SYSTEM_OFF\n");
    metal_console_flush();
    metal_power_off();
  } else if (streq(cmd, "reset")) {
    metal_console_flush();
    metal_power_reboot();
  } else if (cmd[0] == 'r' && cmd[1] == 'u' && cmd[2] == 'n') {
    const char* arg = cmd + 3;
    while (*arg == ' ') ++arg;
    unsigned idx = 0;
    bool any = false;
    while (*arg >= '0' && *arg <= '9') {
      idx = idx * 10 + static_cast<unsigned>(*arg - '0');
      ++arg;
      any = true;
    }
    if (!any) kputs("  usage: run N\n");
    else if (!proc_run(idx)) kputs("  no such program\n");
  } else if (cmd[0]) {
    kprintf("  %s? try 'help'\n", cmd);
  }
}

extern "C" char __tdata_start[], __tdata_end[], __tbss_start[], __tbss_end[];

/* AArch64 static TLS, variant I: TPIDR_EL0 points at a 16-byte thread
 * control block and the TLS image follows it (offset 16; the segment's
 * alignment is 16 - see link.ld). One block per core. */
/* Build one core's TLS block (on the calling core - the page allocator has
 * no lock, so core 0 builds the secondaries' before waking them). */
extern "C" void* metal_make_tls_block() {
  const size_t image = static_cast<size_t>(__tdata_end - __tdata_start);
  const size_t total = static_cast<size_t>(__tbss_end - __tdata_start);
  const size_t bytes = (16 + total + 4095) & ~static_cast<size_t>(4095);
  auto* block = static_cast<uint8_t*>(metal_alloc_pages(bytes));   /* zeroed: covers .tbss */
  if (!block) metal_panic("no memory for a TLS block");
  for (size_t i = 0; i < image; ++i) block[16 + i] = static_cast<uint8_t>(__tdata_start[i]);
  return block;
}
extern "C" void metal_setup_tls() {
  void* block = metal_make_tls_block();
  asm volatile("msr tpidr_el0, %0" :: "r"(block) : "memory");
}

extern "C" void (*__init_array_start[])();
extern "C" void (*__init_array_end[])();

extern "C" void arm64_main(uint64_t dtb_ptr) {
  FdtInfo fdt;
  fdt_parse(dtb_ptr, fdt);

  /* Initialize UART early based on detected machine */
  if (fdt.uart_base) {
    metal_uart_init(fdt.uart_base);
  } else {
    metal_uart_init(0x3F201000ull);
  }
  /* Build stamp, printed before anything else can fail.
   *
   * If this line does not change after a rebuild+reflash, the board is
   * running a stale image and nothing below it means anything - which is
   * worth one line of output to be able to rule out instantly. */
  kputs("\nAIOS - AArch64 bare metal [build " __DATE__ " " __TIME__ "]\n");

  char buf[32];
  kprintf("[boot] %s: %s of DRAM at %p, %u cpu(s), gic v%d\n",
          fdt.model[0] ? fdt.model : "unnamed machine",
          khuman(fdt.ram_size, buf, sizeof(buf)), fdt.ram_base,
          static_cast<uint64_t>(fdt.cpu_count), fdt.gic_version);

  uint64_t el;
  asm volatile("mrs %0, CurrentEL" : "=r"(el));
  kprintf("[boot] entered at EL%u\n", (el >> 2) & 3);

  metal_mmu_init(dtb_ptr);
  kprintf("[boot] mmu on: %s of free frames, image VA %p maps to PA %p\n",
          khuman(metal_heap_total(), buf, sizeof(buf)), reinterpret_cast<uint64_t>(&__kernel_start),
          mmu_translate(reinterpret_cast<uint64_t>(&__kernel_start)));   /* where it was linked: 0x80000 on the Pi, 0x40080000 on virt */

  /* Thread-local storage for this core, before anything C++ can touch a
   * thread_local (constructors included). */
  metal_setup_tls();

  /* C++ static constructors. The x86 kernel has always run .init_array
   * (metal/main.cpp); this one never did, so on the Pi every global with a
   * constructor sat zero-filled. The desktop's gamma tables are one - all
   * zeros made every glyph blend to nothing, so the compositor drew no text
   * at all. Run them now: the heap exists (constructors may allocate) and
   * nothing past this point has touched a C++ global yet. */
  {
    size_t n = 0;
    for (auto** f = __init_array_start; f < __init_array_end; ++f, ++n) (*f)();
    kprintf("[boot] ran %u static constructors\n", static_cast<uint64_t>(n));
  }

  metal_idt_init();
  /* The Broadcom blocks (interrupt controller, GPIO, the VideoCore clock
   * mailbox, SDHOST, DWC2 USB) are at fixed addresses in the BCM peripheral
   * window; on any other board - QEMU virt - those addresses are external
   * aborts, so they are only touched where they exist. */
  const bool bcm_board = board_has_bcm_peripherals();
  if (bcm_board) {
    bcm_irq_init();
    gpio_init();
    cpu_clock_to_max();
  }

  const uint64_t t0 = metal_ticks();
  metal_delay_us(50000);
  if (metal_gic_version() > 0) {
    kprintf("[boot] timer intid %u on gic v%d: %u interrupts in the first 50 ms\n",
            static_cast<uint64_t>(metal_timer_intid()), metal_gic_version(),
            metal_ticks() - t0);
  } else {
    kprintf("[boot] system timer initialized at %u Hz via cntpct_el0\n",
            static_cast<uint64_t>(metal_tick_hz()));
  }

  kputs("[boot] initializing framebuffer...\n");
  fb_init();
  blk_init();
  proc_init();
  if (bcm_board) {
    kputs("[boot] initializing usb host, ethernet & hid (DWC2 + LAN9514)...\n");
    usb_hid_init();
  }
  /* Print what USB actually found, at boot, on the serial console. A
   * headless operator cannot read the framebuffer panels over a UART cable,
   * and "initializing usb host..." followed by silence is indistinguishable
   * between a working stack and a totally dead one. */
  dwc2_report();
  usb_hid_report();
  /* Storage BEFORE networking: net_init() reads /sd/wifi.cfg for the saved
   * SSID and regulatory country, which it could never find while the mount
   * happened after it. The card is on SDHOST, so this no longer touches the
   * Arasan the radio uses. */
  /* SDHOST is a Broadcom block. On a board whose device tree puts its UART
   * anywhere but the BCM283x/2711 peripheral window (QEMU virt: PL011 at
   * 0x09000000) there is nothing at 0x3F202000, and touching its GPIO pads was
   * an external abort - virt panicked here before reaching a prompt. */
  if (bcm_board) kputs("[boot] probing sd storage (SDHOST, FAT32)...\n");
  else kputs("[boot] no BCM283x peripherals in the device tree - no SD card to probe\n");
  if (bcm_board && fat32_mount()) {
    const Fat32FsStatus& fs = fat32_status();
    char b1[32];
    kprintf("[fs] mounted '%s' at LBA %u: %s, %u root entries\n",
            fs.volume_label[0] ? fs.volume_label : "(no label)", static_cast<uint64_t>(fs.lba_start),
            khuman(static_cast<uint64_t>(fs.total_clusters) * fs.bytes_per_cluster, b1, sizeof(b1)),
            static_cast<uint64_t>(fs.total_files_indexed));
    Fat32FileInfo ents[16];
    const size_t n = fat32_list("/", ents, 16);
    for (size_t i = 0; i < n; ++i) {
      kprintf("[fs]   %s%s (%u bytes)\n", ents[i].name, ents[i].is_directory ? "/" : "",
              static_cast<uint64_t>(ents[i].size_bytes));
    }
  } else {
    kputs("[fs] /sd/ unavailable this boot\n");
  }
  kputs("[boot] initializing network stack & dhcp (eth0 + wlan0)...\n");
  net_init();

  /* Fleet-join/node-serve/screenshot-server bootstrap fires from INSIDE
   * net_poll() itself the moment eth0 gets a DHCP lease (see net.cpp's
   * try_fleet_bootstrap()) - not from a wait here, and not from the outer
   * console loop below. Both of those were tried and both missed real
   * boots: a short bounded wait can still be running when the lease lands
   * a beat later, and desktop_run() (the bring-up dashboard, launched just
   * below) BLOCKS on its own frame loop until someone presses Escape on a
   * physical keyboard - which never happens on this board's actual
   * deployment, so nothing placed after it in the outer loop ever ran.
   * net_poll() is the one thing both loops already call on every
   * iteration, so it is the one place this is guaranteed to actually run. */
  /* Re-enabled: Kernel::boot() is now proven to actually complete on this
   * hardware (`grain print(sys.arch, sys.cpus, sys.uptime_ms)` returned
   * "aarch64 4 ..." over the console, board stayed alive afterward) - the
   * make_dir()-too-strict and fat32-mount-dependent-bookkeeping bugs that
   * halted every previous attempt are both fixed (hal_host_arm64.cpp). */
  if (fb_live()) {
    kputs("[boot] display online: auto-booting into the bring-up dashboard ('dash')...\n");
    desktop_run();
  }

  kputs("\nsubstrate ready - 'help' for commands ('desktop' for the AIOS windowed desktop, 'dash' for the bring-up dashboard)\n\n");

  unsigned n = 0;
  kputs("aos> ");
  for (;;) {
    metal_watchdog_pet();
    /* Poll network and USB subsystems in the background */
    net_poll();
    usb_hid_poll();
    if (bcm_board) bcm_irq_dispatch();
    node_service_poll();  /* no-op until net_poll()'s own bootstrap has started it */
    things_frame_tick();  /* the Things' clock, outside net_poll (fleet_shell.cpp) */
    screenshot_server_poll();  /* likewise */

    int c = usb_get_key();
    if (c < 0) {
      c = metal_console_read();
    }

    if (c < 0) {
      /* About to sleep: push whatever is drawn but unflushed onto the glass. */
      fbcon_flush();
      metal_halt_until_interrupt();
      continue;
    }

    if (c == '\r' || c == '\n') {
      kputs("\n");
      g_line[n] = '\0';
      const char* cmd = g_line;
      while (*cmd == ' ') ++cmd;

      /* `gfx` leaves its pattern on the glass with the console suspended; the
       * next command is the signal to give the screen back to the shell. */
      if (!streq(cmd, "gfx")) fbcon_resume();

      dispatch_shell_command(cmd);

      n = 0;
      kputs("aos> ");
      continue;
    }

    if (c == 0x7F || c == 0x08) {
      if (n) { --n; kputs("\b \b"); }
      continue;
    }
    if (c >= 32 && c < 127 && n + 1 < sizeof(g_line)) {
      g_line[n++] = static_cast<char>(c);
      kputc(static_cast<char>(c));
    }
  }
}
