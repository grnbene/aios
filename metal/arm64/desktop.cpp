/* Graphical Desktop Subsystem for AArch64 Bare-Metal (metal/arm64/desktop.cpp).
 *
 * Implements a full-screen, high-performance, responsive graphical desktop on the
 * framebuffer with:
 *   - Tab 1: System Dashboard & Hardware Telemetry (CPU, DRAM, Timers, Mailbox FB, USB HID)
 *   - Tab 2: Broadcom BCM43438 Wi-Fi & Network Mesh (State, Ring Buffers, AP Scanner)
 *   - Tab 3: Interactive Live Terminal & Shell Console
 *   - Tab 4: EL0 Process Table & Task Horizon
 *   - Tab 5: Storage & FAT32 SD Card File Explorer
 *
 * The desktop auto-updates in real-time, runs network & USB polling in the frame loop,
 * and accepts physical USB keyboard/mouse events alongside serial hotkeys seamlessly.
 */
#include "desktop.hpp"

#include "bcm43438.hpp"
#include "bcm_irq.hpp"
#include "dhd.hpp"
#include "dwc2.hpp"
#include "fat32.hpp"
#include "fb.hpp"
#include "fdt.hpp"
#include "gic.hpp"
#include "kio.hpp"
#include "rpi_mailbox.hpp"
#include "lan9514.hpp"
#include "mmu.hpp"
#include "net.hpp"
#include "shell.hpp"
#include "psci.hpp"
#include "sdcard.hpp"
#include "sdio.hpp"
#include "timer.hpp"
#include "uart.hpp"
#include "usb_hid.hpp"
#include "gpio.hpp"
#include "fleet_view.hpp"

extern "C" {
uint64_t metal_ticks();
uint32_t metal_tick_hz();
bool metal_interrupts_live();
uint64_t metal_spurious();
uint64_t metal_fault_count();
void metal_halt_until_interrupt();
[[noreturn]] void metal_power_off();
[[noreturn]] void metal_power_reboot();
}

bool neon_selftest();
extern "C" void crashlog(const char* line);
extern "C" void metal_watchdog_arm();
extern "C" void metal_watchdog_pet();
void blk_report();
void proc_report();
bool proc_run(unsigned index);
void smp_start();
unsigned smp_online_count();
void screenshot_server_poll();
extern "C" void node_service_poll();

struct tm { int tm_sec; int tm_min; int tm_hour; int tm_mday; int tm_mon; int tm_year; int tm_wday; int tm_yday; int tm_isdst; };
extern "C" {
long time(long* t);
struct tm* localtime_r(const long* t, struct tm* out);
size_t strftime(char* s, size_t max, const char* fmt, const struct tm* t);
bool metal_wall_clock_set();
bool metal_set_timezone(const char* name);
}

namespace {

enum class DesktopTab : uint8_t {
  Dashboard = 0,
  Lan       = 1,
  Wifi      = 2,
  Terminal  = 3,
  Processes = 4,
  Storage   = 5,
  Gpio      = 6,
  Fleet     = 7,
  Count     = 8
};

uint8_t g_selected_pin = 3; /* Physical Pin 3 (GPIO 2 SDA) by default */
GpioI2cDevice g_gpio_i2c_cache[16];
size_t g_gpio_i2c_cache_count = 0;
bool g_gpio_i2c_scanned = false;
char g_gpio_status_msg[64] = "Ready. Select pin to inspect or control.";
uint32_t g_gpio_status_col = 0x3DA9FC; /* COL_CYAN */

DesktopTab g_active_tab = DesktopTab::Dashboard;
bool g_desktop_active = false;
uint64_t g_last_render_ms = 0;

/* Colors (Modern Dark Glassmorphism Theme) */
constexpr uint32_t COL_BG_DARK     = 0x0A0D14; /* Background canvas */
constexpr uint32_t COL_PANEL_BG    = 0x131720; /* Card / Window fill */
constexpr uint32_t COL_PANEL_HDR   = 0x1B212E; /* Card titlebar */
constexpr uint32_t COL_BORDER      = 0x2B3342; /* Panel borders */
constexpr uint32_t COL_BORDER_GLOW = 0x3DA9FC; /* Active border glow */
constexpr uint32_t COL_TEXT_WHITE  = 0xF0F6FC; /* Primary text */
constexpr uint32_t COL_TEXT_MUTED  = 0x8B949E; /* Secondary text */
constexpr uint32_t COL_CYAN        = 0x3DA9FC; /* Accent cyan */
constexpr uint32_t COL_EMERALD     = 0x2CB67D; /* Success emerald */
constexpr uint32_t COL_AMBER       = 0xFFA400; /* Warning amber */
constexpr uint32_t COL_CRIMSON     = 0xEF4565; /* Danger crimson */
constexpr uint32_t COL_PURPLE      = 0x9B51E0; /* Purple accent */
constexpr uint32_t COL_TERMINAL_BG = 0x06080D; /* Terminal dark bg */

/* All dashboard text goes through here: anti-aliased Inter / JetBrains Mono
 * (fb_text_aa), laid out with fractional advances. Body is Inter 13 px,
 * the macOS body size. */
inline uint32_t ui_text(uint32_t x, uint32_t y, const char* s, uint32_t col, Face f = Face::Body) {
  return fb_text_aa(x, y, s, col, f);
}
inline uint32_t text_w(const char* s, Face f = Face::Body) { return fb_text_aa_width(s, f); }

/* Draw text clipped to max_w pixels, ending in "..." when cut. */
void text_clip(uint32_t x, uint32_t y, const char* t, uint32_t col, uint32_t max_w, Face f = Face::Body) {
  if (!t || max_w == 0) return;
  if (text_w(t, f) <= max_w) { ui_text(x, y, t, col, f); return; }
  const uint32_t dots = text_w("...", f);
  size_t n = 0; while (t[n]) ++n;
  while (n > 0 && fb_text_aa_width_n(t, n, f) + dots > max_w) --n;
  while (n > 0 && t[n - 1] == ' ') --n;
  char buf[200]; size_t k = 0;
  for (; k < n && k < sizeof(buf) - 4; ++k) buf[k] = t[k];
  buf[k++] = '.'; buf[k++] = '.'; buf[k++] = '.'; buf[k] = 0;
  ui_text(x, y, buf, col, f);
}

/* Mouse & Pointer State */
int32_t g_mouse_x = 640;
/* Tab rectangles as last drawn by the top bar, for click hit-testing. */
uint32_t g_tab_x0[8] = {}, g_tab_x1[8] = {};
unsigned g_tab_count = 0;
int32_t g_mouse_y = 360;
uint8_t g_prev_mouse_btns = 0;

/* Terminal Buffer */
constexpr size_t TERM_MAX_LINES = 300;
constexpr size_t TERM_LINE_LEN  = 128;
char g_term_lines[TERM_MAX_LINES][TERM_LINE_LEN];
size_t g_term_line_count = 0;
char g_term_input[128];
size_t g_term_input_len = 0;

/* Wi-Fi Scan Cache */
constexpr size_t kScanMax = 24;
NetScanEntry g_scan_cache[kScanMax];
size_t g_scan_count = 0;
/* AP list scrolling, and the rectangles the list was ACTUALLY drawn at, so
 * clicks hit what is on screen. The click handler used to recompute rows
 * from its own constants, which disagreed with the drawing. */
size_t g_scan_scroll = 0;
size_t g_ap_row_idx[kScanMax];
size_t g_ap_rows_drawn = 0;

/* Strongest first, one row per network name (a network broadcast by
 * several access points appeared once per AP), hidden SSIDs dropped. */
void tidy_scan_results() {
  size_t n = 0;
  for (size_t i = 0; i < g_scan_count; ++i) {
    if (!g_scan_cache[i].ssid[0]) continue;
    size_t dup = n;
    for (size_t j = 0; j < n; ++j) {
      size_t k = 0;
      while (g_scan_cache[j].ssid[k] && g_scan_cache[j].ssid[k] == g_scan_cache[i].ssid[k]) ++k;
      if (g_scan_cache[j].ssid[k] == 0 && g_scan_cache[i].ssid[k] == 0) { dup = j; break; }
    }
    if (dup < n) {
      if (g_scan_cache[i].rssi > g_scan_cache[dup].rssi) g_scan_cache[dup] = g_scan_cache[i];
    } else {
      g_scan_cache[n++] = g_scan_cache[i];
    }
  }
  for (size_t i = 1; i < n; ++i) {
    NetScanEntry e = g_scan_cache[i];
    size_t j = i;
    while (j > 0 && g_scan_cache[j - 1].rssi < e.rssi) { g_scan_cache[j] = g_scan_cache[j - 1]; --j; }
    g_scan_cache[j] = e;
  }
  g_scan_count = n;
  g_scan_scroll = 0;
}

void do_wifi_scan() {
  net_scan(g_scan_cache, kScanMax, g_scan_count);
  tidy_scan_results();
}

/* Storage File Cache */
Fat32FileInfo g_fat_files[16];
size_t g_fat_file_count = 0;

/* Wi-Fi Password & Connection Modal State */
struct WifiModalState {
  bool active;
  char ssid[33];
  char psk[64];
  size_t psk_len;
  size_t ssid_len;
  bool show_password;
  bool remember;
  uint8_t focus_field; /* 0 = Password, 1 = SSID, 2 = Show Pwd, 3 = Remember, 4 = Connect, 5 = Cancel */
  char status_msg[64];
  uint32_t status_col;
};

WifiModalState g_wifi_modal = {};


bool streq(const char* a, const char* b) {
  while (*a && *a == *b) { ++a; ++b; }
  return *a == *b;
}

/* Helper string formatters */
void int_to_str_hex(uint64_t val, char* buf, size_t max_len) {
  static const char hexd[] = "0123456789abcdef";
  if (max_len < 2) { if (max_len) buf[0] = '\0'; return; }
  char tmp[20];
  size_t i = 0;
  if (val == 0) tmp[i++] = '0';
  while (val > 0 && i < sizeof(tmp)) {
    tmp[i++] = hexd[val & 0xFu];
    val >>= 4;
  }
  size_t out_idx = 0;
  while (i > 0 && out_idx + 1 < max_len) buf[out_idx++] = tmp[--i];
  buf[out_idx] = '\0';
}

void int_to_str(uint64_t val, char* buf, size_t max_len) {
  if (max_len == 0) return;
  if (val == 0) {
    buf[0] = '0';
    buf[1] = '\0';
    return;
  }
  char tmp[24];
  size_t i = 0;
  while (val > 0 && i < sizeof(tmp)) {
    tmp[i++] = static_cast<char>('0' + (val % 10));
    val /= 10;
  }
  size_t out_idx = 0;
  while (i > 0 && out_idx + 1 < max_len) {
    buf[out_idx++] = tmp[--i];
  }
  buf[out_idx] = '\0';
}

void str_copy(char* dst, const char* src, size_t max_len) {
  size_t i = 0;
  while (src[i] && i + 1 < max_len) {
    dst[i] = src[i];
    ++i;
  }
  dst[i] = '\0';
}

size_t kstrlen(const char* s) {
  size_t len = 0;
  while (s && s[len]) ++len;
  return len;
}


void term_append_line(const char* s) {
  if (g_term_line_count < TERM_MAX_LINES) {
    str_copy(g_term_lines[g_term_line_count++], s, TERM_LINE_LEN);
  } else {
    for (size_t i = 0; i < TERM_MAX_LINES - 1; ++i) {
      str_copy(g_term_lines[i], g_term_lines[i + 1], TERM_LINE_LEN);
    }
    str_copy(g_term_lines[TERM_MAX_LINES - 1], s, TERM_LINE_LEN);
  }
}

void draw_window_frame(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const char* title, uint32_t accent_color = COL_CYAN) {
  fb_box_shadow(x, y, w, h, 3);
  fb_round_box(x, y, w, h, COL_PANEL_BG, 0x262E3D);
  fb_gradient_v(x + 1, y + 1, w - 2, 30, 0x1C2230, 0x161B26);
  fb_line_h(x + 1, y + 31, w - 2, 0x262E3D);
  /* A small status dot, not a bar: colour says state, the title says what. */
  fb_round_box(x + 14, y + 12, 7, 7, accent_color, accent_color);
  ui_text(x + 29, y + 8, title, COL_TEXT_WHITE, Face::Semibold);
}

void draw_metric_card(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const char* label, const char* val, uint32_t val_color, const char* subtext = nullptr) {
  fb_box_shadow(x, y, w, h, 2);
  fb_round_box(x, y, w, h, COL_PANEL_BG, 0x262E3D);
  ui_text(x + 14, y + 10, label, 0x7D8796, Face::Small);
  text_clip(x + 14, y + 28, val, val_color, w - 28, Face::Large);
  if (subtext) text_clip(x + 14, y + h - 22, subtext, COL_TEXT_MUTED, w - 28);
}

void draw_progress_bar(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint64_t current, uint64_t max, uint32_t fill_col) {
  fb_round_box(x, y, w, h, 0x080B10, COL_BORDER);
  if (max > 0 && w > 4) {
    uint32_t bar_w = static_cast<uint32_t>((static_cast<uint64_t>(w - 4) * (current > max ? max : current)) / max);
    if (bar_w > 0) {
      fb_fill(x + 2, y + 2, bar_w, h - 4, fill_col);
    }
  }
}

/* Wi-Fi 4-Bar Signal Indicator */
void draw_wifi_signal_bars(uint32_t x, uint32_t y, int8_t rssi) {
  int bars = 0;
  if (rssi >= -55) bars = 4;
  else if (rssi >= -67) bars = 3;
  else if (rssi >= -78) bars = 2;
  else if (rssi >= -90) bars = 1;

  for (int i = 0; i < 4; ++i) {
    uint32_t bh = (i + 1) * 3 + 2;
    uint32_t bx = x + i * 5;
    uint32_t by = y + (14 - bh);
    uint32_t col = (i < bars) ? (bars >= 3 ? COL_EMERALD : (bars == 2 ? COL_AMBER : COL_CRIMSON)) : 0x222730;
    fb_fill(bx, by, 3, bh, col);
  }
}

/* ----------------- Top Bar & Navigation ----------------- */

/* Glyph advance for fb_text at scale 1, and the padding fb_chip adds. */
constexpr uint32_t GLYPH_ADV = 8;
constexpr uint32_t CHIP_PAD  = 16;


/* Tiny text builder for status values: Txt().s("eth0 ").u(3).c() */
struct Txt {
  char b[112]; size_t n = 0;
  Txt() { b[0] = 0; }
  Txt& s(const char* t) { while (t && *t && n + 1 < sizeof(b)) b[n++] = *t++; b[n] = 0; return *this; }
  Txt& u(uint64_t v) { char t[24]; int_to_str(v, t, sizeof(t)); return s(t); }
  Txt& ip(const uint8_t a[4]) { char t[20]; ip_to_str(a, t, sizeof(t)); return s(t); }
  const char* c() const { return b; }
};

/* ---- Buttons that remember where they were drawn ----
 * Every clickable control registers its rectangle while painting and the
 * click handler walks this list, so a click can only ever hit what is on
 * screen. Hand-maintained hit-test coordinates drifted from the drawing
 * (tab clicks landed on the wrong tab) - this cannot. */
enum UiAction : int {
  UI_NONE = 0,
  UI_WIFI_SCAN, UI_WIFI_JOIN, UI_WIFI_FORGET, UI_WIFI_UP, UI_WIFI_DOWN,
  UI_LAN_DHCP, UI_LAN_PING,
  UI_MODAL_SSID, UI_MODAL_PSK, UI_MODAL_SHOW, UI_MODAL_REMEMBER, UI_MODAL_CONNECT, UI_MODAL_CANCEL,
  UI_GPIO_TOGGLE, UI_GPIO_INPUT, UI_GPIO_OUTPUT, UI_GPIO_PULLUP, UI_GPIO_PULLDOWN, UI_GPIO_SCAN,
  UI_FLEET_SURVEY, UI_FLEET_JOIN, UI_FLEET_LEAVE,
  UI_WIFI_ROW0 = 100,   /* + visible row index */
  UI_PROC_RUN0 = 200,   /* + process index */
};
struct UiButton { uint32_t x, y, w, h; int action; };
UiButton g_ui_buttons[64];
unsigned g_ui_button_count = 0;

enum class BtnStyle : uint8_t { Primary, Secondary, Success, Disabled };

uint32_t ui_button(uint32_t x, uint32_t y, const char* label, int action,
                   BtnStyle style = BtnStyle::Secondary, uint32_t min_w = 0) {
  const uint32_t tw = text_w(label, Face::Medium);
  uint32_t bw = tw + 28;
  if (bw < min_w) bw = min_w;
  const uint32_t bh = 26;
  uint32_t fill = 0x222A38, border = 0x333D50, fg = 0xDCE3EC;
  if (style == BtnStyle::Primary)  { fill = 0x1F5FAF; border = 0x2E73C7; fg = 0xFFFFFF; }
  if (style == BtnStyle::Success)  { fill = 0x16392A; border = 0x1F5A40; fg = COL_EMERALD; }
  if (style == BtnStyle::Disabled) { fill = 0x181D27; border = 0x232A37; fg = 0x5A6474; }
  fb_round_box(x, y, bw, bh, fill, border);
  if (style == BtnStyle::Primary) fb_line_h(x + 2, y + 1, bw - 4, 0x3A82D6);   /* top highlight */
  ui_text(x + (bw - tw) / 2, y + 6, label, fg, Face::Medium);
  if (action != UI_NONE && style != BtnStyle::Disabled && g_ui_button_count < 64) {
    g_ui_buttons[g_ui_button_count++] = {x, y, bw, bh, action};
  }
  return bw;
}

/* Panes the mouse wheel scrolls, registered as they are drawn. */
enum ScrollPane : int { SCR_NONE = 0, SCR_BOOT_LOG, SCR_LAN_LOG, SCR_WIFI_LOG, SCR_TERMINAL, SCR_NETWORKS };
struct ScrollRegion { uint32_t x, y, w, h; int pane; };
ScrollRegion g_scroll_regions[8];
unsigned g_scroll_region_count = 0;
void ui_scroll_region(uint32_t x, uint32_t y, uint32_t w, uint32_t h, int pane) {
  if (g_scroll_region_count < 8) g_scroll_regions[g_scroll_region_count++] = {x, y, w, h, pane};
}
uint32_t g_boot_log_scroll = 0;   /* lines back from the newest */
uint32_t g_term_scroll = 0;

void ui_region(uint32_t x, uint32_t y, uint32_t w, uint32_t h, int action) {
  if (g_ui_button_count < 64) g_ui_buttons[g_ui_button_count++] = {x, y, w, h, action};
}


void draw_top_bar(uint32_t w) {
  fb_gradient_v(0, 0, w, 40, 0x1A2130, 0x121722);
  fb_line_h(0, 40, w, 0x252D3B);

  /* Logo */
  fb_round_box(14, 10, 20, 20, COL_CYAN, COL_CYAN);
  ui_text(14 + (20 - text_w("A", Face::Semibold)) / 2, 12, "A", 0x06101C, Face::Semibold);
  ui_text(42, 12, "AIOS", COL_TEXT_WHITE, Face::Semibold);

  const NetInterfaceInfo& eth = net_eth_info();
  const NetInterfaceInfo& wifi = net_wifi_info();
  const UsbHidStatus& usb = usb_hid_status();
  const Fat32FsStatus& fs = fat32_status();

  /* ---- Status pills, packed right-to-left: a coloured dot and a label. */
  struct Pill { char text[40]; uint32_t dot; };
  Pill pills[4]; size_t n = 0;
  {
    Txt t;
    if (wifi.ip_cfg.dhcp_bound) { t.s("Wi-Fi ").ip(wifi.ip_cfg.ip); pills[n].dot = COL_EMERALD; }
    else if (wifi.link_state == NetLinkState::Associated) { t.s("Wi-Fi joined"); pills[n].dot = COL_AMBER; }
    else if (wifi.initialized) { t.s("Wi-Fi on"); pills[n].dot = COL_CYAN; }
    else { t.s("Wi-Fi off"); pills[n].dot = 0x4A5263; }
    str_copy(pills[n++].text, t.c(), sizeof(pills[0].text));
  }
  {
    Txt t;
    if (eth.ip_cfg.dhcp_bound) { t.s("Ethernet ").ip(eth.ip_cfg.ip); pills[n].dot = COL_EMERALD; }
    else if (eth.link_up) { t.s("Ethernet"); pills[n].dot = COL_AMBER; }
    else { t.s("Ethernet off"); pills[n].dot = 0x4A5263; }
    str_copy(pills[n++].text, t.c(), sizeof(pills[0].text));
  }
  if (fs.mounted) { str_copy(pills[n].text, "SD", sizeof(pills[0].text)); pills[n++].dot = COL_EMERALD; }
  if (usb.keyboard_detected || usb.mouse_detected) { str_copy(pills[n].text, "USB", sizeof(pills[0].text)); pills[n++].dot = COL_CYAN; }

  uint32_t cx = w - 14;
  for (size_t i = 0; i < n; ++i) {
    const uint32_t pw = text_w(pills[i].text, Face::Medium) + 30;
    if (cx < pw + 8) break;
    cx -= pw;
    fb_round_box(cx, 9, pw, 22, 0x1C2433, 0x2A3345);
    fb_round_box(cx + 10, 17, 7, 7, pills[i].dot, pills[i].dot);
    ui_text(cx + 22, 12, pills[i].text, 0xC9D1DC, Face::Medium);
    cx -= 8;
  }
  const uint32_t pills_left = cx;

  /* ---- Tabs: a segmented control. The click hit-test reads g_tab_x0/x1,
   * recorded here, so clicks land on the tab that is actually drawn. */
  static const char* TABS[8] = {"Dashboard", "LAN", "Wi-Fi", "Terminal", "Programs", "Storage", "GPIO", "Fleet"};
  constexpr uint32_t PAD = 14;
  uint32_t total = 0;
  for (unsigned i = 0; i < 8; ++i) total += text_w(TABS[i], Face::Medium) + 2 * PAD;
  uint32_t tx = 120;
  if (tx + total + 12 > pills_left) tx = (pills_left > total + 12) ? pills_left - total - 12 : 100;
  fb_round_box(tx - 3, 7, total + 6, 26, 0x131923, 0x262E3D);
  g_tab_count = 0;
  for (unsigned i = 0; i < 8; ++i) {
    const uint32_t tw = text_w(TABS[i], Face::Medium) + 2 * PAD;
    const bool active = static_cast<uint8_t>(g_active_tab) == i;
    if (active) fb_round_box(tx, 9, tw, 22, 0x2E3B52, 0x3B4A66);
    ui_text(tx + PAD, 12, TABS[i], active ? COL_TEXT_WHITE : 0x8F99A8, Face::Medium);
    g_tab_x0[i] = tx; g_tab_x1[i] = tx + tw;
    g_tab_count = i + 1;
    tx += tw;
  }
}

/* ----------------- Bottom Status Bar ----------------- */
void draw_bottom_bar(uint32_t w, uint32_t h) {
  fb_gradient_v(0, h - 28, w, 28, 0x11161F, 0x1A202C);
  fb_line_h(0, h - 28, w, COL_BORDER);

  /* Key hints: the key in the brighter weight, what it does beside it. */
  uint32_t hx = 16;
  static const char* hints[][2] = {{"F1-F8", "switch tab"}, {"Tab", "next tab"}, {"Space", "click"}, {"Esc", "leave dashboard"}};
  for (auto& hnt : hints) {
    hx = ui_text(hx, h - 20, hnt[0], 0xB5BFCC, Face::Small) + 5;
    hx = ui_text(hx, h - 20, hnt[1], 0x6D7786, Face::Small) + 18;
  }

  /* Uptime from the free-running counter. This used to print timer-IRQ
   * ticks, which never advance on the Pi (no GIC, IRQ never unmasked), so
   * it read "Ticks: 0" forever. */
  /* The time of day, once NTP has set it; uptime beside it either way. */
  if (metal_wall_clock_set() && w >= 900) {
    const long t = time(nullptr);
    struct tm lt; localtime_r(&t, &lt);
    char ts[40]; strftime(ts, sizeof(ts), "%a %b %e  %I:%M %p", &lt);
    ui_text(w / 2 - text_w(ts, Face::Small) / 2, h - 20, ts, 0xB5BFCC, Face::Small);
  }
  uint64_t sec = metal_now_ns() / 1000000000ull;
  char up[32] = "uptime 00:00:00";
  const uint64_t hr = sec / 3600, mn = (sec / 60) % 60; sec %= 60;
  up[7] = static_cast<char>('0' + (hr / 10) % 10); up[8] = static_cast<char>('0' + hr % 10);
  up[10] = static_cast<char>('0' + mn / 10);       up[11] = static_cast<char>('0' + mn % 10);
  up[13] = static_cast<char>('0' + sec / 10);      up[14] = static_cast<char>('0' + sec % 10);
  if (w >= 900) ui_text(w - 16 - text_w(up, Face::Small), h - 20, up, 0x7D8796, Face::Small);
}

/* The board's own name for itself, from the firmware revision code - the
 * device tree is often absent here, and its fallback model string was a
 * guess ("Raspberry Pi 3 (Hardware Substrate)"). */
const char* board_name() {
  static char name[64] = "";
  static unsigned tries = 0;
  if (name[0]) return name;
  if (tries >= 5 || fb_source() != FbSource::RpiMailbox) return nullptr;
  ++tries;
  uint32_t rev = 0;
  const bool ok = rpi_get_board_revision(rev);
  { Txt t; t.s("board revision query: ").s(ok ? "ok " : "FAILED ").s("0x"); char hx[12]; int_to_str_hex(rev, hx, sizeof(hx)); t.s(hx);
    net_breadcrumb(t.c()); }
  if (!ok || !(rev & (1u << 23))) return nullptr;
  const uint32_t type = (rev >> 4) & 0xFF, pcb = rev & 0xF, mem = (rev >> 20) & 7;
  const char* model = nullptr;
  switch (type) {
    case 0x04: model = "Raspberry Pi 2 Model B"; break;
    case 0x08: model = "Raspberry Pi 3 Model B"; break;
    case 0x0A: model = "Raspberry Pi Compute Module 3"; break;
    case 0x0C: model = "Raspberry Pi Zero W"; break;
    case 0x0D: model = "Raspberry Pi 3 Model B+"; break;
    case 0x0E: model = "Raspberry Pi 3 Model A+"; break;
    case 0x11: model = "Raspberry Pi 4 Model B"; break;
    default: return nullptr;
  }
  static const char* mems[] = {"256 MB", "512 MB", "1 GB", "2 GB", "4 GB", "8 GB", "?", "?"};
  Txt t; t.s(model).s(" rev 1.").u(pcb).s(", ").s(mems[mem]);
  str_copy(name, t.c(), sizeof(name));
  return name;
}

/* ----------------- Tab 1: Dashboard View ----------------- */
void refresh_scan_from_background();
void draw_tab_dashboard(uint32_t w, uint32_t h) {
  refresh_scan_from_background();
  const FdtInfo& f = fdt_info();
  char b1[32], b2[32];

  /* 4 Metric Top Cards */
  uint32_t card_w = (w - 64) / 4;
  uint32_t card_y = 52;
  uint32_t card_h = 72;

  draw_metric_card(20, card_y, card_w, card_h, "BOARD",
                   board_name() ? board_name() : (f.dtb_size && f.model[0] ? f.model : "Raspberry Pi 3 (model unconfirmed)"),
                   COL_CYAN, "BCM2837, 4x Cortex-A53");

  khuman(metal_heap_used(), b1, sizeof(b1));
  khuman(metal_heap_total(), b2, sizeof(b2));
  Txt ram; ram.s(b1).s(" of ").s(b2);
  draw_metric_card(20 + (card_w + 8), card_y, card_w, card_h, "KERNEL HEAP",
                   ram.c(), COL_EMERALD, "4 KiB pages, 39-bit VA");

  Txt disp; disp.u(fb_width()).s(" x ").u(fb_height()).s(", 32 bpp");
  draw_metric_card(20 + (card_w + 8) * 2, card_y, card_w, card_h, "DISPLAY",
                   disp.c(), COL_PURPLE, "HDMI via VideoCore mailbox");

  const NetInterfaceInfo& eth = net_eth_info();
  const NetInterfaceInfo& wifi = net_wifi_info();
  Txt eth_t, wifi_t;
  uint32_t net_col = COL_AMBER;
  if (eth.ip_cfg.dhcp_bound) { eth_t.s("eth0  ").ip(eth.ip_cfg.ip); net_col = COL_EMERALD; }
  else if (eth.link_up)      eth_t.s("eth0  link up, no address");
  else                       { eth_t.s("eth0  no cable"); net_col = COL_TEXT_MUTED; }
  if (wifi.ip_cfg.dhcp_bound)                          wifi_t.s("wlan0 ").ip(wifi.ip_cfg.ip);
  else if (wifi.link_state == NetLinkState::Associated) wifi_t.s("wlan0 joined, no address");
  else if (wifi.initialized)                            wifi_t.s("wlan0 not joined");
  else                                                  wifi_t.s("wlan0 offline");
  draw_metric_card(20 + (card_w + 8) * 3, card_y, card_w, card_h, "NETWORK",
                   eth_t.c(), net_col, wifi_t.c());

  /* Main System Window (Left) */
  uint32_t win1_w = (w - 52) / 2;
  uint32_t win_y = 136;
  uint32_t win_h = h - win_y - 36;
  draw_window_frame(20, win_y, win1_w, win_h, "System", COL_CYAN);

  uint32_t row_y = win_y + 42;
  const uint32_t val_x = 196;
  const uint32_t val_w = win1_w - (val_x - 20) - 16;
  auto draw_info_row = [&](const char* key, const char* val, uint32_t col) {
    ui_text(36, row_y, key, COL_TEXT_MUTED);
    text_clip(val_x, row_y, val, col, val_w);
    row_y += 24;
  };
  auto section = [&](const char* title) {
    row_y += 6;
    ui_text(36, row_y + 2, title, 0x6FA8DC, Face::Small);
    fb_line_h(36 + text_w(title, Face::Small) + 10, row_y + 8, win1_w - 52 - text_w(title, Face::Small) - 10, 0x262E3D);
    row_y += 24;
  };

  const Dwc2HostInfo& usb_host = dwc2_host_info();
  const UsbHidStatus& hid = usb_hid_status();

  section("PROCESSOR");
  draw_info_row("SoC", "Broadcom BCM2837, ARMv8-A 64-bit", COL_TEXT_WHITE);
  draw_info_row("Running at", "EL1, MMU on", metal_mmu_live() ? COL_EMERALD : COL_AMBER);
  { Txt t; t.s("generic timer, ").u(metal_cntfrq() / 1000000).s(".").u((metal_cntfrq() / 100000) % 10).s(" MHz");
    draw_info_row("Clock", t.c(), COL_TEXT_WHITE); }

  section("USB");
  draw_info_row("Host", !usb_host.initialized ? "not initialized"
                        : (usb_host.port_enabled ? "DWC2, root port up" : "DWC2, nothing attached"),
                (usb_host.initialized && usb_host.port_enabled) ? COL_EMERALD : COL_CRIMSON);
  { Txt t;
    if (hid.keyboard_detected) t.s("keyboard");
    if (hid.keyboard_detected && hid.mouse_detected) t.s(" + ");
    if (hid.mouse_detected) t.s("mouse");
    if (!hid.keyboard_detected && !hid.mouse_detected) t.s("no keyboard or mouse");
    draw_info_row("Input devices", t.c(), (hid.keyboard_detected || hid.mouse_detected) ? COL_EMERALD : COL_AMBER); }
  { Txt t; t.u(hid.key_events).s(hid.key_events == 1 ? " key, " : " keys, ").u(hid.mouse_events).s(hid.mouse_events == 1 ? " mouse report" : " mouse reports");
    draw_info_row("Input received", t.c(), COL_TEXT_WHITE); }
  { /* A NAK on an interrupt endpoint is "no new report", not an error:
     * only stalls, transaction errors and timeouts count here. */
    const uint64_t errs = static_cast<uint64_t>(usb_host.fail_stall) + usb_host.fail_xact + usb_host.fail_timeout;
    Txt t; t.u(usb_host.total_transfers).s(" transfers, ").u(errs).s(errs == 1 ? " error" : " errors");
    draw_info_row("Traffic", t.c(), errs ? COL_AMBER : COL_TEXT_WHITE); }

  section("NETWORK");
  {
    const Lan9514Info& lan = lan9514_info();
    Txt t;
    if (eth.ip_cfg.dhcp_bound) t.ip(eth.ip_cfg.ip); else t.s(eth.link_up ? "link up, no address" : "no cable");
    if (eth.link_up && lan.link_mbps) t.s(", ").u(lan.link_mbps).s(" Mbps");
    draw_info_row("Ethernet", t.c(), eth.ip_cfg.dhcp_bound ? COL_EMERALD : (eth.link_up ? COL_AMBER : COL_TEXT_MUTED));
  }
  {
    uint32_t gen = 0;
    const size_t seen = net_wifi_last_scan(nullptr, 0, &gen);
    (void)seen;
    Txt t;
    if (!wifi.initialized) t.s("radio offline");
    else if (wifi.ip_cfg.dhcp_bound) t.s(wifi.ssid).s(", ").ip(wifi.ip_cfg.ip);
    else if (wifi.link_state == NetLinkState::Associated) t.s(wifi.ssid).s(", no address yet");
    else { t.s("not joined"); if (g_scan_count) t.s(", ").u(g_scan_count).s(" networks nearby"); }
    draw_info_row("Wi-Fi", t.c(), wifi.ip_cfg.dhcp_bound ? COL_EMERALD : (wifi.initialized ? COL_CYAN : COL_CRIMSON));
  }

  section("STORAGE & MEMORY");
  {
    const Fat32FsStatus& sd_st = fat32_status();
    draw_info_row("SD card", sd_st.mounted ? "FAT32 mounted at /sd" : "no FAT32 volume (netbooted)",
                  sd_st.mounted ? COL_EMERALD : COL_AMBER);
  }
  { Txt t; t.s(b1).s(" used of ").s(b2); draw_info_row("Heap", t.c(), COL_TEXT_WHITE); }
  row_y += 2;
  draw_progress_bar(36, row_y, win1_w - 72, 12, metal_heap_used(), metal_heap_total(), COL_EMERALD);

  /* Subsystems & Drivers Window (Right) */
  uint32_t win2_x = 20 + win1_w + 12;
  draw_window_frame(win2_x, win_y, win1_w, win_h, "Devices", COL_PURPLE);

  row_y = win_y + 42;
  auto draw_subsystem_card = [&](const char* name, const char* desc, const char* state, uint32_t st_col) {
    fb_round_box(win2_x + 16, row_y, win1_w - 32, 42, 0x151A24, 0x232B39);
    ui_text(win2_x + 30, row_y + 6, name, COL_TEXT_WHITE, Face::Medium);
    ui_text(win2_x + 30, row_y + 23, desc, COL_TEXT_MUTED);
    const uint32_t sw = text_w(state, Face::Medium);
    const uint32_t sx = win2_x + win1_w - 30 - sw;
    fb_round_box(sx - 14, row_y + 17, 7, 7, st_col, st_col);
    ui_text(sx, row_y + 13, state, st_col, Face::Medium);
    row_y += 48;
  };

  /* Every row here reports the driver's OWN state. These used to be constant
   * green "Online" strings, so the panel said the Ethernet and HID stacks
   * were up while the hardware was completely dead - which is worse than no
   * panel at all, because it sends you looking at cables. */
  const Fat32FsStatus& fs_st = fat32_status();

  draw_subsystem_card("Synopsys DWC2 USB Host", "Root port and the LAN9514 hub",
                      !usb_host.initialized ? "Offline"
                        : (usb_host.port_enabled ? "Online" : "No device"),
                      (usb_host.initialized && usb_host.port_enabled) ? COL_EMERALD : COL_CRIMSON);

  draw_subsystem_card("SMSC LAN9514 Ethernet", "10/100 Ethernet, eth0",
                      !hid.ethernet_detected ? "Not found"
                        : (eth.link_up ? "Link up" : "No cable"),
                      !hid.ethernet_detected ? COL_CRIMSON
                        : (eth.link_up ? COL_EMERALD : COL_AMBER));

  draw_subsystem_card("BCM43438 Wi-Fi", "802.11n radio on SDIO, wlan0",
                      !wifi.initialized ? "Offline"
                        : (wifi.link_state == NetLinkState::Associated ? "Online" : "Not joined"),
                      !wifi.initialized ? COL_CRIMSON
                        : (wifi.link_state == NetLinkState::Associated ? COL_EMERALD : COL_AMBER));

  draw_subsystem_card("Keyboard and mouse", "Boot protocol keyboard and mouse",
                      (hid.keyboard_detected || hid.mouse_detected) ? "Active" : "No device",
                      (hid.keyboard_detected || hid.mouse_detected) ? COL_EMERALD : COL_CRIMSON);

  draw_subsystem_card("microSD card", "Card slot, /sd",
                      fs_st.mounted ? "Mounted" : "Not mounted",
                      fs_st.mounted ? COL_EMERALD : COL_CRIMSON);

  /* Kernel boot log, on the screen.
   *
   * This is the whole point: with no serial cable, no working keyboard and
   * no network there is exactly one output left, and it is this display.
   * Every line the kernel printed at boot - including each [usb] device it
   * enumerated and each step that failed - is rendered here so it can be
   * read (or photographed) without any input device at all. */
  row_y += 6;
  ui_text(win2_x + 16, row_y + 2, "BOOT LOG", 0x6FA8DC, Face::Small);
  fb_line_h(win2_x + 16 + text_w("BOOT LOG", Face::Small) + 10, row_y + 8, win1_w - 42 - text_w("BOOT LOG", Face::Small) - 10, 0x262E3D);
  row_y += 18;

  uint32_t log_bottom = win_y + win_h - 12;
  uint32_t avail_rows = (log_bottom > row_y) ? (log_bottom - row_y) / 14 : 0;
  size_t total_lines = klog_line_count();

  /* Show the diagnostic lines, not the tail of the log. The boot banner and
   * the driver reports are long enough to push every [usb]/[eth] enumeration
   * line off the bottom, and those lines are the entire reason this pane
   * exists. */
  auto is_interesting = [](const char* q) -> uint32_t {
    if (!q || !q[0]) return 0;
    while (*q == ' ') ++q;
    if (q[0] == '[' && q[1] == 'c' && q[2] == 'r' && q[3] == 'a') return 0;   /* [crashlog] notes */
    if (q[0] == '[' && q[1] == 'u' && q[2] == 's' && q[3] == 'b') return COL_CYAN;
    if (q[0] == '[' && q[1] == 'e' && q[2] == 't' && q[3] == 'h') return COL_CYAN;
    for (const char* r = q; *r; ++r) {
      if (r[0] == 'f' && r[1] == 'a' && r[2] == 'i' && r[3] == 'l' && !(r > q + 2 && r[-1] == ' ' && r[-2] == '0' && !(r[-3] >= '0' && r[-3] <= '9'))) return COL_CRIMSON;
      if (r[0] == 'F' && r[1] == 'A' && r[2] == 'I' && r[3] == 'L') return COL_CRIMSON;
      if (r[0] == 'N' && r[1] == 'O' && r[2] == 'T' && r[3] == ' ') return COL_CRIMSON;
      if (r[0] == 'p' && r[1] == 'a' && r[2] == 'n' && r[3] == 'i') return COL_CRIMSON;
    }
    return 0;
  };

  /* Count matches first so the newest ones are the ones that fit. */
  size_t matches = 0;
  for (size_t i = 0; i < total_lines; ++i) {
    if (is_interesting(klog_line(i))) ++matches;
  }
  size_t skip = (matches > avail_rows) ? (matches - avail_rows) : 0;
  if (g_boot_log_scroll > skip) g_boot_log_scroll = static_cast<uint32_t>(skip);
  skip -= g_boot_log_scroll;
  ui_scroll_region(win2_x + 8, row_y - 20, win1_w - 16, log_bottom - row_y + 20, SCR_BOOT_LOG);
  if (g_boot_log_scroll) {
    Txt t; t.s("scrolled back ").u(g_boot_log_scroll);
    ui_text(win2_x + win1_w - 16 - text_w(t.c(), Face::Small), row_y - 16, t.c(), COL_AMBER, Face::Small);
  }

  size_t seen = 0, drawn = 0;
  for (size_t i = 0; i < total_lines && drawn < avail_rows; ++i) {
    const char* ln = klog_line(i);
    uint32_t col = is_interesting(ln);
    if (!col) continue;
    if (seen++ < skip) continue;
    text_clip(win2_x + 20, row_y, ln, col, win1_w - 40, Face::Mono);
    row_y += 15;
    ++drawn;
  }
  if (drawn == 0) {
    ui_text(win2_x + 20, row_y, "(no USB diagnostics recorded)", COL_TEXT_MUTED);
  }
}

void open_wifi_connect_modal(const char* ssid = nullptr) {
  g_wifi_modal.active = true;
  g_wifi_modal.psk[0] = '\0';
  g_wifi_modal.psk_len = 0;
  g_wifi_modal.show_password = false;
  g_wifi_modal.remember = true;
  g_wifi_modal.focus_field = 0; /* Password field */
  str_copy(g_wifi_modal.status_msg, "Enter WPA2 password and press ENTER to connect", sizeof(g_wifi_modal.status_msg));
  g_wifi_modal.status_col = COL_CYAN;

  if (ssid && ssid[0]) {
    str_copy(g_wifi_modal.ssid, ssid, sizeof(g_wifi_modal.ssid));
    g_wifi_modal.ssid_len = kstrlen(g_wifi_modal.ssid);
  } else {
    g_wifi_modal.ssid[0] = '\0';
    g_wifi_modal.ssid_len = 0;
    g_wifi_modal.focus_field = 1; /* SSID field */
  }
}

void submit_wifi_connect() {
  if (!g_wifi_modal.ssid[0]) {
    str_copy(g_wifi_modal.status_msg, "[err] Please specify a valid SSID", sizeof(g_wifi_modal.status_msg));
    g_wifi_modal.status_col = COL_CRIMSON;
    return;
  }

  str_copy(g_wifi_modal.status_msg, "[*] Associating with network...", sizeof(g_wifi_modal.status_msg));
  g_wifi_modal.status_col = COL_AMBER;
  desktop_paint();

  char term_msg[96] = "[*] Wi-Fi Connecting to '";
  size_t p = 25;
  for (size_t i = 0; g_wifi_modal.ssid[i] && p < sizeof(term_msg) - 8; ++i) term_msg[p++] = g_wifi_modal.ssid[i];
  term_msg[p++] = '\''; term_msg[p++] = '.'; term_msg[p++] = '.'; term_msg[p++] = '.'; term_msg[p] = '\0';
  term_append_line(term_msg);

  bool ok = net_associate(g_wifi_modal.ssid, g_wifi_modal.psk);
  if (ok) {
    if (g_wifi_modal.remember && fat32_status().mounted) {
      wifi_save_config(g_wifi_modal.ssid, g_wifi_modal.psk, true);
      term_append_line("[ok] Wi-Fi credentials saved to /sd/wifi.cfg (Auto-Connect: ON)");
    }
    term_append_line("[ok] Wi-Fi connection established! IP stack online.");
    g_wifi_modal.active = false;
  } else {
    str_copy(g_wifi_modal.status_msg, "[err] Association timed out or rejected", sizeof(g_wifi_modal.status_msg));
    g_wifi_modal.status_col = COL_CRIMSON;
    term_append_line("[err] Wi-Fi association failed. Check passphrase.");
  }
}

void draw_wifi_modal(uint32_t screen_w, uint32_t screen_h) {
  if (!g_wifi_modal.active) return;
  /* Only the dialog is clickable while it is open. */
  g_ui_button_count = 0;

  const uint32_t mw = 560, mh = 296;
  const uint32_t mx = (screen_w > mw) ? (screen_w - mw) / 2 : 10;
  const uint32_t my = (screen_h > mh) ? (screen_h - mh) / 2 : 40;
  const bool blink = (metal_now_ns() / 500000000ull) % 2 == 0;
  const bool can_remember = fat32_status().mounted;

  draw_window_frame(mx, my, mw, mh, "Join a Wi-Fi network", COL_CYAN);

  /* Network name */
  ui_text(mx + 20, my + 44, "Network name", COL_TEXT_MUTED);
  fb_round_box(mx + 20, my + 62, mw - 40, 28, 0x080B10, g_wifi_modal.focus_field == 1 ? COL_CYAN : COL_BORDER);
  text_clip(mx + 30, my + 68, g_wifi_modal.ssid[0] ? g_wifi_modal.ssid : "type the network name",
            g_wifi_modal.ssid[0] ? COL_TEXT_WHITE : COL_TEXT_MUTED, mw - 60);
  if (g_wifi_modal.focus_field == 1 && blink) fb_fill(mx + 30 + g_wifi_modal.ssid_len * 8, my + 67, 2, 16, COL_CYAN);
  ui_region(mx + 20, my + 62, mw - 40, 28, UI_MODAL_SSID);

  /* Password */
  ui_text(mx + 20, my + 102, "Password", COL_TEXT_MUTED);
  fb_round_box(mx + 20, my + 120, mw - 40, 28, 0x080B10, g_wifi_modal.focus_field == 0 ? COL_CYAN : COL_BORDER);
  char shown[68] = {};
  if (g_wifi_modal.show_password) str_copy(shown, g_wifi_modal.psk, sizeof(shown));
  else { for (size_t i = 0; i < g_wifi_modal.psk_len && i < 64; ++i) shown[i] = '*'; shown[g_wifi_modal.psk_len < 64 ? g_wifi_modal.psk_len : 64] = 0; }
  text_clip(mx + 30, my + 126, shown[0] ? shown : "leave empty for an open network",
            shown[0] ? COL_TEXT_WHITE : COL_TEXT_MUTED, mw - 60);
  if (g_wifi_modal.focus_field == 0 && blink) fb_fill(mx + 30 + g_wifi_modal.psk_len * 8, my + 125, 2, 16, COL_CYAN);
  ui_region(mx + 20, my + 120, mw - 40, 28, UI_MODAL_PSK);

  /* Options */
  auto checkbox = [&](uint32_t x, uint32_t y, bool on, bool focus, bool enabled, const char* label, int action) {
    fb_round_box(x, y, 16, 16, on && enabled ? COL_CYAN : 0x080B10, focus ? COL_CYAN : COL_BORDER);
    if (on && enabled) { fb_line_h(x + 4, y + 8, 3, 0x000000); fb_line_v(x + 7, y + 4, 8, 0x000000); fb_fill(x + 4, y + 7, 8, 3, 0x0A0D14); }
    ui_text(x + 24, y, label, enabled ? COL_TEXT_WHITE : COL_TEXT_MUTED);
    if (enabled) ui_region(x, y - 4, 24 + static_cast<uint32_t>(kstrlen(label)) * 8, 24, action);
  };
  checkbox(mx + 20, my + 162, g_wifi_modal.show_password, g_wifi_modal.focus_field == 2, true, "Show password", UI_MODAL_SHOW);
  checkbox(mx + 240, my + 162, g_wifi_modal.remember, g_wifi_modal.focus_field == 3, can_remember,
           can_remember ? "Remember this network" : "Remember (needs an SD card)", UI_MODAL_REMEMBER);

  if (g_wifi_modal.status_msg[0]) text_clip(mx + 20, my + 196, g_wifi_modal.status_msg, g_wifi_modal.status_col, mw - 40);

  const uint32_t by = my + mh - 46;
  uint32_t bx = mx + 20;
  bx += ui_button(bx, by, "Connect  [Enter]", UI_MODAL_CONNECT, BtnStyle::Primary) + 10;
  ui_button(bx, by, "Cancel  [Esc]", UI_MODAL_CANCEL, BtnStyle::Secondary);
  ui_text(mx + mw - 20 - text_w("[Tab] next field"), by + 5, "[Tab] next field", COL_TEXT_MUTED);
}

/* Shared: a scrolling recent-events panel fed by net_log_eth()/net_log_wifi(),
 * newest first. Used at the bottom of both the LAN and Wi-Fi tabs so a
 * failure like "dongle firmware not responding" is visible on the
 * framebuffer, not only over a serial cable. */
/* Scroll offset (in lines) for the LAN and Wi-Fi log panels, adjusted with
 * ',' and '.'. Without this the panel only ever showed the newest handful of
 * lines, which is useless when the interesting failure scrolled past. */
uint32_t g_lan_log_scroll = 0;
uint32_t g_wifi_log_scroll = 0;

void draw_net_log_panel(uint32_t x, uint32_t y, uint32_t w, uint32_t h, bool is_wifi) {
  draw_window_frame(x, y, w, h, is_wifi ? "Wi-Fi log" : "Ethernet log", COL_TEXT_MUTED);
  const uint32_t scroll = is_wifi ? g_wifi_log_scroll : g_lan_log_scroll;
  size_t n = 0;
  {
    Txt hint;
    if (scroll > 0) hint.s("scrolled back ").u(scroll).s("   ");
    hint.s("scroll, or [,] older  [.] newer");
    ui_text(x + w - 16 - text_w(hint.c(), Face::Small), y + 10, hint.c(), scroll ? COL_AMBER : 0x7D8796, Face::Small);
  }

  /* The real kernel log lines for this interface, newest first. */
  constexpr size_t MAX_ROWS = 40;
  const char* lines[MAX_ROWS];
  uint32_t max_rows = (h - 44) / 15;
  if (max_rows > MAX_ROWS) max_rows = MAX_ROWS;
  ui_scroll_region(x, y, w, h, is_wifi ? SCR_WIFI_LOG : SCR_LAN_LOG);
  uint32_t sc = scroll;
  for (;;) {
    if (is_wifi) {
      n = kconsole_log(lines, max_rows, sc, "[wifi]");
      if (n == 0 && sc == 0) n = kconsole_log(lines, max_rows, 0, "[sdio]");
    } else {
      n = kconsole_log(lines, max_rows, sc, "[eth]");
      if (n == 0 && sc == 0) n = kconsole_log(lines, max_rows, 0, "[usb]");
    }
    /* Scrolled past the oldest line: settle on the oldest page. */
    if (n > 0 || sc == 0) break;
    sc = sc > 3 ? sc - 3 : 0;
  }
  if (sc != scroll) { if (is_wifi) g_wifi_log_scroll = sc; else g_lan_log_scroll = sc; }
  uint32_t row_y = y + 40;
  if (n == 0) {
    ui_text(x + 16, row_y, "Nothing logged yet.", COL_TEXT_MUTED);
    return;
  }
  for (size_t i = 0; i < n; ++i) {
    const char* ln = lines[i];
    /* Drop the "[wifi] " / "[eth] " prefix: the panel title already says it. */
    if (ln[0] == '[') { const char* q = ln; while (*q && *q != ']') ++q; if (*q == ']') { ++q; while (*q == ' ') ++q; ln = q; } }
    text_clip(x + 16, row_y, ln, i == 0 ? COL_TEXT_WHITE : COL_TEXT_MUTED, w - 32, Face::Mono);
    row_y += 15;
  }
}

/* One label / value row inside a panel. */
struct RowWriter {
  uint32_t x, y, key_w, max_w;
  void row(const char* key, const char* val, uint32_t col) {
    ui_text(x, y, key, COL_TEXT_MUTED);
    text_clip(x + key_w, y, val, col, max_w - key_w);
    y += 24;
  }
};

char g_ping_msg[64] = "";
uint32_t g_ping_col = COL_TEXT_MUTED;

void lan_ping_router() {
  const auto& eth = net_eth_info();
  uint32_t rtt = 0;
  if (!eth.ip_cfg.dhcp_bound) {
    str_copy(g_ping_msg, "No address yet - renew DHCP first.", sizeof(g_ping_msg));
    g_ping_col = COL_AMBER;
    return;
  }
  Txt t;
  if (net_ping(eth.ip_cfg.gateway, 1000, rtt)) {
    t.s("Router ").ip(eth.ip_cfg.gateway).s(" replied in ").u(rtt / 1000).s(".").u((rtt / 100) % 10).s(" ms");
    g_ping_col = COL_EMERALD;
  } else {
    t.s("No reply from ").ip(eth.ip_cfg.gateway).s(" within 1 s");
    g_ping_col = COL_CRIMSON;
  }
  str_copy(g_ping_msg, t.c(), sizeof(g_ping_msg));
}

/* ----------------- Tab 2: LAN (eth0 / SMSC LAN9514) View ----------------- */
void draw_tab_lan(uint32_t w, uint32_t h) {
  const NetInterfaceInfo& eth = net_eth_info();
  const Lan9514Info& lan = lan9514_info();

  const uint32_t top_y = 52, top_h = 300;
  const uint32_t left_w = 440;
  const uint32_t right_x = 20 + left_w + 12, right_w = w - right_x - 20;

  /* Interface panel */
  draw_window_frame(20, top_y, left_w, top_h, "Ethernet (eth0)", eth.link_up ? COL_EMERALD : COL_AMBER);
  RowWriter r{36, top_y + 44, 120, left_w - 32};
  {
    Txt t;
    if (!eth.link_up) t.s("no cable / no link");
    else {
      t.s("up");
      if (lan.link_mbps) { t.s(", ").u(lan.link_mbps).s(" Mbps ").s(lan.full_duplex ? "full duplex" : "half duplex");
                           if (!lan.autoneg) t.s(" (forced)"); }
    }
    r.row("Link", t.c(), eth.link_up ? COL_EMERALD : COL_CRIMSON);
  }
  if (eth.ip_cfg.dhcp_bound) {
    Txt a; a.ip(eth.ip_cfg.ip); r.row("Address", a.c(), COL_EMERALD);
    Txt m; m.ip(eth.ip_cfg.netmask); r.row("Netmask", m.c(), COL_TEXT_WHITE);
    Txt g; g.ip(eth.ip_cfg.gateway); r.row("Router", g.c(), COL_TEXT_WHITE);
    Txt d; d.ip(eth.ip_cfg.dns); r.row("DNS", d.c(), COL_TEXT_WHITE);
  } else {
    r.row("Address", eth.link_up ? "asking DHCP..." : "none", COL_AMBER);
    r.row("Netmask", "-", COL_TEXT_MUTED);
    r.row("Router", "-", COL_TEXT_MUTED);
    r.row("DNS", "-", COL_TEXT_MUTED);
  }
  { char mac[24]; mac_to_str(eth.mac, mac, sizeof(mac)); r.row("MAC", mac, COL_TEXT_WHITE); }

  const uint32_t by = top_y + top_h - 42;
  uint32_t bx = 36;
  bx += ui_button(bx, by, "[D] Renew DHCP", UI_LAN_DHCP, BtnStyle::Primary) + 10;
  ui_button(bx, by, "[P] Ping router", UI_LAN_PING, eth.ip_cfg.dhcp_bound ? BtnStyle::Secondary : BtnStyle::Disabled);
  if (g_ping_msg[0]) text_clip(36, by - 24, g_ping_msg, g_ping_col, left_w - 32);

  /* Traffic panel */
  draw_window_frame(right_x, top_y, right_w, top_h, "Traffic", COL_CYAN);
  RowWriter t{right_x + 16, top_y + 44, 150, right_w - 32};
  { Txt v; v.u(eth.stats.tx_packets).s(" packets, ").u(eth.stats.tx_bytes / 1024).s(" KiB"); t.row("Sent", v.c(), COL_TEXT_WHITE); }
  { Txt v; v.u(eth.stats.rx_packets).s(" packets, ").u(eth.stats.rx_bytes / 1024).s(" KiB"); t.row("Received", v.c(), COL_TEXT_WHITE); }
  { const uint64_t e = eth.stats.tx_errors + eth.stats.rx_errors;
    Txt v; v.u(eth.stats.tx_errors).s(" send, ").u(eth.stats.rx_errors).s(" receive"); t.row("Errors", v.c(), e ? COL_AMBER : COL_TEXT_WHITE); }
  { const uint64_t d = eth.stats.tx_dropped + eth.stats.rx_dropped + eth.tx_ring.dropped + eth.rx_ring.dropped;
    Txt v; v.u(d); t.row("Dropped", v.c(), d ? COL_AMBER : COL_TEXT_WHITE); }
  { Txt v; v.u(eth.tx_ring.depth).s(" of ").u(eth.tx_ring.capacity).s(" frames"); t.row("Send queue", v.c(), COL_TEXT_WHITE); }
  t.y += 4;
  draw_progress_bar(right_x + 16, t.y, right_w - 32, 10, eth.tx_ring.depth, eth.tx_ring.capacity ? eth.tx_ring.capacity : 1, COL_CYAN);
  t.y += 26;
  { Txt v; v.u(eth.rx_ring.depth).s(" of ").u(eth.rx_ring.capacity).s(" frames"); t.row("Receive queue", v.c(), COL_TEXT_WHITE); }
  t.y += 4;
  draw_progress_bar(right_x + 16, t.y, right_w - 32, 10, eth.rx_ring.depth, eth.rx_ring.capacity ? eth.rx_ring.capacity : 1, COL_EMERALD);

  const uint32_t log_y = top_y + top_h + 12;
  draw_net_log_panel(20, log_y, w - 40, h - 36 - log_y, false);
}

/* ----------------- Tab 8: Fleet ----------------- */
/* This board on chi's hub: the link, who else is on it, what each member
 * reports when asked over the node link, and the fleet's own log lines.
 * Every figure is fleet_shell.cpp's live state or a member's answer - the
 * survey is a real telemetry round trip per member, run when asked for,
 * never a cached guess dressed up as live. */
FleetMember g_fleet_members[16];
size_t g_fleet_member_count = 0;
uint64_t g_fleet_surveyed_ns = 0;
char g_fleet_msg[80] = "";
uint32_t g_fleet_msg_col = COL_TEXT_MUTED;

/* fleet_shell.cpp calls this once per roster member, from inside the
 * blocking survey loop, so a slow/dead peer repaints "surveying N/M: id"
 * instead of leaving the screen frozen with no way to tell "still working"
 * from "crashed" - the thing that made an unresponsive peer look like a
 * hang on real hardware even after the timeout math itself was fixed. */
extern "C" void fleet_survey_progress(size_t index, size_t total, const char* peer_id) {
  Txt t; t.s("Surveying ").u(index + 1).s("/").u(total).s(": ").s(peer_id).s(" ...");
  str_copy(g_fleet_msg, t.c(), sizeof(g_fleet_msg));
  g_fleet_msg_col = COL_AMBER;
  desktop_paint();
}

extern "C" void things_frame_tick();   // fleet_shell.cpp: the Things' clock, outside net_poll

void fleet_do_survey() {
  FleetStatus st; fleet_status(&st);
  if (!st.attached) { str_copy(g_fleet_msg, "Not on a hub - join first.", sizeof(g_fleet_msg)); g_fleet_msg_col = COL_AMBER; return; }
  const uint64_t t0 = metal_now_ns();
  g_fleet_member_count = fleet_survey(g_fleet_members, 16);
  g_fleet_surveyed_ns = metal_now_ns();
  size_t ok = 0; for (size_t i = 0; i < g_fleet_member_count; ++i) ok += g_fleet_members[i].reached;
  Txt t; t.s("Surveyed ").u(g_fleet_member_count).s(" member(s), ").u(ok).s(" answered, in ").u((g_fleet_surveyed_ns - t0) / 1000000).s(" ms");
  str_copy(g_fleet_msg, t.c(), sizeof(g_fleet_msg));
  g_fleet_msg_col = ok == g_fleet_member_count ? COL_EMERALD : COL_AMBER;
}

void draw_tab_fleet(uint32_t w, uint32_t h) {
  FleetStatus st; fleet_status(&st);
  const uint32_t top_y = 52, top_h = 300;
  const uint32_t left_w = 440;
  const uint32_t right_x = 20 + left_w + 12, right_w = w - right_x - 20;

  /* The link */
  const uint32_t accent = st.attached ? COL_EMERALD : (st.joining ? COL_AMBER : COL_CRIMSON);
  draw_window_frame(20, top_y, left_w, top_h, "Fleet link", accent);
  RowWriter r{36, top_y + 44, 130, left_w - 32};
  if (st.attached) r.row("Status", "on the hub", COL_EMERALD);
  else if (st.joining) {
    Txt t; t.s("joining");
    if (st.retry_in_s) t.s(" - next try in ").u(st.retry_in_s).s(" s"); else t.s(" (waiting for eth0)");
    r.row("Status", t.c(), COL_AMBER);
  } else r.row("Status", "not on a hub", COL_CRIMSON);
  r.row("Hub", st.hub[0] ? st.hub : "-", st.hub[0] ? COL_TEXT_WHITE : COL_TEXT_MUTED);
  r.row("This board", st.self, COL_CYAN);
  { Txt t; t.u(st.roster_count).s(st.roster_count == 1 ? " other member" : " other members");
    r.row("On the hub", st.attached ? t.c() : "-", COL_TEXT_WHITE); }
  { Txt t; t.u(st.served_count).s(" peer(s), ").u(st.served_requests).s(" request(s)");
    r.row("Served", t.c(), st.served_count ? COL_TEXT_WHITE : COL_TEXT_MUTED); }
  r.row("At boot", st.configured ? "joins from FLEET.CFG" : "no FLEET.CFG - manual", st.configured ? COL_TEXT_WHITE : COL_AMBER);

  const uint32_t by = top_y + top_h - 42;
  uint32_t bx = 36;
  if (st.attached) bx += ui_button(bx, by, "[S] Survey members", UI_FLEET_SURVEY, BtnStyle::Primary) + 10;
  else bx += ui_button(bx, by, "[J] Join now", UI_FLEET_JOIN, st.configured ? BtnStyle::Primary : BtnStyle::Disabled) + 10;
  if (st.attached || st.joining) ui_button(bx, by, "[L] Leave", UI_FLEET_LEAVE, BtnStyle::Secondary);
  if (g_fleet_msg[0]) text_clip(36, by - 24, g_fleet_msg, g_fleet_msg_col, left_w - 32);

  /* The members */
  draw_window_frame(right_x, top_y, right_w, top_h, "Members", COL_CYAN);
  const uint32_t cx0 = right_x + 16, cx1 = cx0 + 130, cx2 = cx1 + 90, cx3 = cx2 + 60, cx4 = cx3 + 150, cx5 = cx4 + 90;
  uint32_t ry = top_y + 44;
  if (!st.attached) {
    ui_text(cx0, ry, "Not on a hub.", COL_TEXT_MUTED);
  } else if (g_fleet_member_count == 0) {
    for (uint32_t i = 0; i < st.roster_count && i < 16 && ry + 24 < top_y + top_h; ++i) {
      ui_text(cx0, ry, st.roster[i], COL_TEXT_WHITE, Face::Mono);
      ry += 22;
    }
    ui_text(cx0, top_y + top_h - 30, "Press S to ask each member how it is doing.", COL_TEXT_MUTED, Face::Small);
  } else {
    ui_text(cx0, ry, "Member", COL_TEXT_MUTED, Face::Small);
    ui_text(cx1, ry, "Answer", COL_TEXT_MUTED, Face::Small);
    ui_text(cx2, ry, "CPUs", COL_TEXT_MUTED, Face::Small);
    if (right_w > 560) ui_text(cx3, ry, "Memory in use", COL_TEXT_MUTED, Face::Small);
    if (right_w > 650) ui_text(cx4, ry, "Jobs / slots", COL_TEXT_MUTED, Face::Small);
    if (right_w > 740) ui_text(cx5, ry, "Databases", COL_TEXT_MUTED, Face::Small);
    ry += 22;
    for (size_t i = 0; i < g_fleet_member_count && ry + 22 < top_y + top_h - 20; ++i) {
      const FleetMember& m = g_fleet_members[i];
      ui_text(cx0, ry, m.id, COL_TEXT_WHITE, Face::Mono);
      if (!m.reached) { text_clip(cx1, ry, m.note[0] ? m.note : "no answer", COL_CRIMSON, right_x + right_w - 16 - cx1); ry += 22; continue; }
      { Txt t; t.u(m.rtt_ms).s(" ms"); ui_text(cx1, ry, t.c(), m.rtt_ms < 500 ? COL_EMERALD : COL_AMBER); }
      { Txt t; t.u(m.cpus); ui_text(cx2, ry, t.c(), COL_TEXT_WHITE); }
      if (right_w > 560) { Txt t; t.u(m.memory_in_use >> 20).s(" / ").u(m.total_memory >> 20).s(" MiB"); ui_text(cx3, ry, t.c(), COL_TEXT_WHITE); }
      if (right_w > 650) { Txt t; t.u(m.jobs).s(" / ").u(m.slots); ui_text(cx4, ry, t.c(), m.jobs ? COL_CYAN : COL_TEXT_WHITE); }
      if (right_w > 740) { Txt t; t.u(m.databases); ui_text(cx5, ry, t.c(), COL_TEXT_WHITE); }
      ry += 22;
    }
    Txt a; a.s("asked ").u((metal_now_ns() - g_fleet_surveyed_ns) / 1000000000ull).s(" s ago - S to ask again");
    ui_text(cx0, top_y + top_h - 30, a.c(), COL_TEXT_MUTED, Face::Small);
  }

  /* What the fleet code has been saying: every console line about it,
   * newest first (the join, the hub link dropping and healing, peers'
   * circuits). */
  const uint32_t log_y = top_y + top_h + 12, log_h = h - 36 - log_y;
  draw_window_frame(20, log_y, w - 40, log_h, "Fleet log", COL_TEXT_MUTED);
  const char* all[256];
  const size_t n = kconsole_log(all, 256, 0, nullptr);
  uint32_t row_y = log_y + 40, shown = 0;
  for (size_t i = 0; i < n && row_y + 15 < log_y + log_h; ++i) {
    const char* ln = all[i];
    bool fleet_line = false;
    for (const char* q = ln; *q && !fleet_line; ++q)
      if (q[0] == 'f' && q[1] == 'l' && q[2] == 'e' && q[3] == 'e' && q[4] == 't' && q[5] == ':') fleet_line = true;
    if (!fleet_line) continue;
    while (*ln == ' ') ++ln;
    text_clip(36, row_y, ln, shown == 0 ? COL_TEXT_WHITE : COL_TEXT_MUTED, w - 72, Face::Mono);
    row_y += 15; ++shown;
  }
  if (shown == 0) ui_text(36, row_y, "Nothing about the fleet logged since boot.", COL_TEXT_MUTED);
}

/* Pull the newest background scan into the list, keeping the scroll spot. */
uint32_t g_scan_gen_seen = 0xFFFFFFFFu;
uint64_t g_scan_updated_ns = 0;
void refresh_scan_from_background() {
  uint32_t gen = 0;
  NetScanEntry tmp[kScanMax];
  const size_t n = net_wifi_last_scan(tmp, kScanMax, &gen);
  if (gen == g_scan_gen_seen) return;
  g_scan_gen_seen = gen;
  if (n == 0) return;
  const size_t keep_scroll = g_scan_scroll;
  for (size_t i = 0; i < n; ++i) g_scan_cache[i] = tmp[i];
  g_scan_count = n;
  tidy_scan_results();
  g_scan_scroll = keep_scroll;
  g_scan_updated_ns = metal_now_ns();
}

/* ----------------- Tab 3: Wi-Fi (wlan0 / BCM43438) View ----------------- */
void draw_tab_wifi(uint32_t w, uint32_t h) {
  const NetInterfaceInfo& wifi = net_wifi_info();
  refresh_scan_from_background();

  const uint32_t top_y = 52, top_h = 300;
  const uint32_t left_w = 400;
  const uint32_t right_x = 20 + left_w + 12, right_w = w - right_x - 20;
  const bool joined = wifi.link_state == NetLinkState::Associated;

  /* Status panel */
  draw_window_frame(20, top_y, left_w, top_h, "Wi-Fi (wlan0)",
                    !wifi.initialized ? COL_CRIMSON : (joined ? COL_EMERALD : COL_CYAN));
  RowWriter r{36, top_y + 44, 104, left_w - 32};
  if (!wifi.initialized)                             r.row("Radio", "offline (firmware did not start)", COL_CRIMSON);
  else if (joined)                                   r.row("Radio", "connected", COL_EMERALD);
  else if (wifi.link_state == NetLinkState::Scanning) r.row("Radio", "scanning...", COL_AMBER);
  else                                               r.row("Radio", "on, not connected", COL_CYAN);
  r.row("Network", wifi.ssid[0] ? wifi.ssid : "-", wifi.ssid[0] ? COL_TEXT_WHITE : COL_TEXT_MUTED);
  if (wifi.ip_cfg.dhcp_bound) { Txt a; a.ip(wifi.ip_cfg.ip); r.row("Address", a.c(), COL_EMERALD); }
  else r.row("Address", joined ? "asking DHCP..." : "-", joined ? COL_AMBER : COL_TEXT_MUTED);
  if (wifi.rssi) {
    Txt v; v.s("    ").s("-").u(static_cast<uint64_t>(wifi.rssi < 0 ? -wifi.rssi : wifi.rssi)).s(" dBm");
    r.row("Signal", v.c(), COL_TEXT_WHITE);
    draw_wifi_signal_bars(36 + 104, r.y - 26, static_cast<int8_t>(wifi.rssi));
  } else {
    r.row("Signal", "-", COL_TEXT_MUTED);
  }
  { char mac[24]; mac_to_str(wifi.mac, mac, sizeof(mac));
    r.row("MAC", wifi.initialized ? mac : "-", wifi.initialized ? COL_TEXT_WHITE : COL_TEXT_MUTED); }

  const uint32_t by = top_y + top_h - 42;
  uint32_t bx = 36;
  const BtnStyle live = wifi.initialized ? BtnStyle::Primary : BtnStyle::Disabled;
  bx += ui_button(bx, by, "[S] Scan", UI_WIFI_SCAN, live) + 10;
  bx += ui_button(bx, by, "[C] Join...", UI_WIFI_JOIN, wifi.initialized ? BtnStyle::Secondary : BtnStyle::Disabled) + 10;
  ui_button(bx, by, "[F] Forget saved", UI_WIFI_FORGET, BtnStyle::Secondary);

  /* Networks panel */
  draw_window_frame(right_x, top_y, right_w, top_h, "Networks", COL_CYAN);
  const uint32_t c_name = right_x + 16, c_sig = right_x + 330, c_ch = right_x + 450, c_sec = right_x + 510;
  const uint32_t btn_w = 112, c_btn = right_x + right_w - 16 - btn_w;
  uint32_t y = top_y + 42;
  ui_text(c_name, y, "NAME", 0x7D8796, Face::Small);
  ui_text(c_sig, y, "SIGNAL", 0x7D8796, Face::Small);
  ui_text(c_ch, y, "CH", 0x7D8796, Face::Small);
  ui_text(c_sec, y, "SECURITY", 0x7D8796, Face::Small);
  fb_line_h(right_x + 12, y + 18, right_w - 24, COL_BORDER);

  const uint32_t rows_top = y + 26, row_h = 28;
  ui_scroll_region(right_x, rows_top - 4, right_w, top_y + top_h - rows_top, SCR_NETWORKS);
  const uint32_t footer_y = top_y + top_h - 34;
  const size_t rows_fit = (footer_y - 4 - rows_top) / row_h;
  if (g_scan_scroll + rows_fit > g_scan_count) g_scan_scroll = g_scan_count > rows_fit ? g_scan_count - rows_fit : 0;

  if (g_scan_count == 0) {
    ui_text(c_name, rows_top + 6, wifi.initialized ? "No networks found yet - the radio scans every 10 s, or press [S]."
                                                   : "The Wi-Fi radio is offline.", COL_TEXT_MUTED);
  }
  for (size_t i = 0; i < rows_fit && g_scan_scroll + i < g_scan_count; ++i) {
    const NetScanEntry& ap = g_scan_cache[g_scan_scroll + i];
    const uint32_t ry = rows_top + static_cast<uint32_t>(i) * row_h;
    bool current = joined && wifi.ssid[0];
    for (size_t k = 0; current && k < 33; ++k) { if (ap.ssid[k] != wifi.ssid[k]) current = false; if (!ap.ssid[k]) break; }
    if (i % 2 == 1) fb_fill(right_x + 12, ry - 2, right_w - 24, row_h, 0x161B25);
    text_clip(c_name, ry + 6, ap.ssid, current ? COL_EMERALD : COL_TEXT_WHITE, c_sig - c_name - 12);
    draw_wifi_signal_bars(c_sig, ry + 5, static_cast<int8_t>(ap.rssi));
    { Txt v; v.s("-").u(static_cast<uint64_t>(ap.rssi < 0 ? -ap.rssi : ap.rssi)).s(" dBm");
      ui_text(c_sig + 28, ry + 6, v.c(), COL_TEXT_MUTED); }
    { Txt v; v.u(ap.channel); ui_text(c_ch, ry + 6, v.c(), COL_TEXT_WHITE); }
    ui_text(c_sec, ry + 6, (ap.capability & 0x10) ? "secured" : "open", (ap.capability & 0x10) ? COL_TEXT_WHITE : COL_AMBER);
    if (current) {
      ui_button(c_btn, ry, "Connected", UI_NONE, BtnStyle::Success, btn_w);
    } else {
      ui_button(c_btn, ry, "Connect", UI_WIFI_ROW0 + static_cast<int>(i), BtnStyle::Primary, btn_w);
    }
    if (g_ap_rows_drawn < kScanMax) { g_ap_row_idx[i] = g_scan_scroll + i; g_ap_rows_drawn = i + 1; }
  }

  /* Footer: count, freshness and scrolling. */
  fb_line_h(right_x + 12, footer_y - 4, right_w - 24, COL_BORDER);
  {
    Txt v;
    if (g_scan_count) {
      v.u(g_scan_count).s(g_scan_count == 1 ? " network" : " networks");
      if (g_scan_count > rows_fit) {
        const size_t last = g_scan_scroll + rows_fit < g_scan_count ? g_scan_scroll + rows_fit : g_scan_count;
        v.s(", showing ").u(g_scan_scroll + 1).s("-").u(last);
      }
      if (g_scan_updated_ns) v.s(", updated ").u((metal_now_ns() - g_scan_updated_ns) / 1000000000ull).s(" s ago");
    }
    ui_text(c_name, footer_y + 5, v.c(), COL_TEXT_MUTED);
  }
  if (g_scan_count > rows_fit) {
    const bool can_dn = g_scan_scroll + rows_fit < g_scan_count;
    uint32_t sx = right_x + right_w - 16;
    sx -= 76; ui_button(sx, footer_y, "Down ]", UI_WIFI_DOWN, can_dn ? BtnStyle::Secondary : BtnStyle::Disabled, 76);
    sx -= 84; ui_button(sx, footer_y, "[ Up", UI_WIFI_UP, g_scan_scroll ? BtnStyle::Secondary : BtnStyle::Disabled, 76);
  }

  const uint32_t log_y = top_y + top_h + 12;
  draw_net_log_panel(20, log_y, w - 40, h - 36 - log_y, true);
}

/* ----------------- Tab 3: Interactive Terminal View ----------------- */
void draw_tab_terminal(uint32_t w, uint32_t h) {
  uint32_t win_y = 52;
  uint32_t win_w = w - 40;
  uint32_t win_h = h - win_y - 36;

  draw_window_frame(20, win_y, win_w, win_h, "Terminal", COL_EMERALD);

  /* Terminal canvas fill */
  uint32_t term_x = 24;
  uint32_t term_y = win_y + 34;
  uint32_t term_w = win_w - 8;
  uint32_t term_h = win_h - 40;
  fb_fill(term_x, term_y, term_w, term_h, COL_TERMINAL_BG);

  /* Output history: the newest page, or further back with the wheel. */
  uint32_t line_y = term_y + 8;
  const uint32_t prompt_top = term_y + term_h - 28;
  const size_t visible = prompt_top > line_y ? (prompt_top - line_y) / 17 : 0;
  const size_t max_back = g_term_line_count > visible ? g_term_line_count - visible : 0;
  if (g_term_scroll > max_back) g_term_scroll = static_cast<uint32_t>(max_back);
  const size_t first = g_term_line_count > visible + g_term_scroll ? g_term_line_count - visible - g_term_scroll : 0;
  const size_t last = g_term_line_count > g_term_scroll ? g_term_line_count - g_term_scroll : 0;
  ui_scroll_region(term_x, term_y, term_w, term_h, SCR_TERMINAL);
  if (g_term_scroll) {
    Txt t; t.s("scrolled back ").u(g_term_scroll).s(" lines - type to return");
    ui_text(term_x + term_w - 16 - text_w(t.c(), Face::Small), term_y + 6, t.c(), COL_AMBER, Face::Small);
  }
  for (size_t i = first; i < last; ++i) {
    uint32_t col = COL_TEXT_WHITE;
    if (g_term_lines[i][0] == '$') col = COL_CYAN;
    else if (g_term_lines[i][0] == '[') {
      if (g_term_lines[i][1] == 'o' && g_term_lines[i][2] == 'k') col = COL_EMERALD;
      else if (g_term_lines[i][1] == 'e' && g_term_lines[i][2] == 'r') col = COL_CRIMSON;
      else if (g_term_lines[i][1] == '*') col = COL_AMBER;
    }
    text_clip(term_x + 12, line_y, g_term_lines[i], col, term_w - 24, Face::Mono);
    line_y += 17;
  }

  /* Render Prompt Line */
  uint32_t prompt_y = term_y + term_h - 24;
  fb_line_h(term_x, prompt_y - 4, term_w, COL_BORDER);
  const uint32_t in_x = ui_text(term_x + 12, prompt_y, "aos $ ", COL_EMERALD, Face::Mono);
  const uint32_t end_x = ui_text(in_x, prompt_y, g_term_input, COL_TEXT_WHITE, Face::Mono);

  /* Blinking block cursor, placed from the real text width. */
  if ((metal_now_ns() / 500000000ull) % 2 == 0) {
    fb_fill(end_x + 1, prompt_y + 1, 7, 15, 0x6FA8DC);
  }
}

/* ----------------- Tab 4: Processes View ----------------- */
void draw_tab_processes(uint32_t w, uint32_t h) {
  const uint32_t win_y = 52, win_w = w - 40, win_h = h - win_y - 36;
  draw_window_frame(20, win_y, win_w, win_h, "Programs", COL_PURPLE);

  ui_text(36, win_y + 42, "Built-in test programs. Each runs unprivileged (EL0) in its own address space;", COL_TEXT_MUTED);
  ui_text(36, win_y + 60, "output appears on the Terminal tab (F4).", COL_TEXT_MUTED);

  struct Prog { const char* name; const char* what; };
  static const Prog progs[] = {
    {"u_hello", "Prints a greeting through a system call"},
    {"u_spin",  "Busy loop - shows the timer taking the CPU back"},
    {"u_crash", "Touches bad memory - the kernel must survive it"},
    {"u_priv",  "Tries a privileged instruction - must be refused"},
    {"u_neon",  "128-bit NEON vector arithmetic"},
  };

  const uint32_t c_id = 36, c_name = 76, c_what = 200;
  const uint32_t btn_w = 88, c_btn = 20 + win_w - 16 - btn_w;
  uint32_t y = win_y + 92;
  ui_text(c_id, y, "#", 0x7D8796, Face::Small);
  ui_text(c_name, y, "PROGRAM", 0x7D8796, Face::Small);
  ui_text(c_what, y, "WHAT IT DOES", 0x7D8796, Face::Small);
  fb_line_h(32, y + 18, win_w - 24, COL_BORDER);
  y += 28;
  for (unsigned i = 0; i < 5; ++i) {
    if (i % 2 == 1) fb_fill(32, y - 3, win_w - 24, 34, 0x161B25);
    char id[4] = {static_cast<char>('0' + i), 0};
    ui_text(c_id, y + 6, id, COL_CYAN);
    ui_text(c_name, y + 6, progs[i].name, COL_TEXT_WHITE);
    text_clip(c_what, y + 6, progs[i].what, COL_TEXT_MUTED, c_btn - c_what - 16);
    ui_button(c_btn, y + 1, "Run", UI_PROC_RUN0 + static_cast<int>(i), BtnStyle::Primary, btn_w);
    y += 34;
  }
}

/* ----------------- Tab 5: Storage & FAT32 SD File Explorer ----------------- */
void draw_tab_storage(uint32_t w, uint32_t h) {
  const uint32_t win_y = 52, win_h = h - win_y - 36;
  const uint32_t left_w = 440;
  const uint32_t right_x = 20 + left_w + 12, right_w = w - right_x - 20;

  const SdCardStatus& sd = sdcard_status();
  const Fat32FsStatus& fs = fat32_status();

  /* Every value here is the driver's own state. This panel used to print a
   * fixed "50 MHz High-Speed 4-bit bus", "RCA 0x0001" and a "/sd" mount
   * point even with no card detected at all. */
  draw_window_frame(20, win_y, left_w, win_h, "SD card", fs.mounted ? COL_EMERALD : COL_AMBER);
  RowWriter r{36, win_y + 44, 120, left_w - 32};
  if (!sd.initialized) {
    r.row("Card", "no card answered", COL_AMBER);
    r.y += 8;
    ui_text(36, r.y, "Nothing is needed from the card to run:", COL_TEXT_MUTED); r.y += 18;
    ui_text(36, r.y, "a netbooted board loads the kernel over", COL_TEXT_MUTED); r.y += 18;
    ui_text(36, r.y, "the network.", COL_TEXT_MUTED);
  } else {
    r.row("Card", sd.is_sdhc ? "SDHC / SDXC" : "SD (standard capacity)", COL_TEXT_WHITE);
    { char cap[32]; khuman(sd.capacity_bytes, cap, sizeof(cap)); r.row("Capacity", cap, COL_TEXT_WHITE); }
    { Txt t; t.s("0x"); char hx[12]; int_to_str_hex(sd.rca, hx, sizeof(hx)); t.s(hx); r.row("Address (RCA)", t.c(), COL_TEXT_WHITE); }
    { Txt t; t.u(sd.reads_completed).s(" read, ").u(sd.writes_completed).s(" written"); r.row("Blocks", t.c(), COL_TEXT_WHITE); }
    { Txt t; t.u(sd.read_errors); r.row("Read errors", t.c(), sd.read_errors ? COL_AMBER : COL_TEXT_WHITE); }
    r.y += 8;
    if (fs.mounted) {
      r.row("File system", "FAT32, mounted at /sd", COL_EMERALD);
      r.row("Label", fs.volume_label[0] ? fs.volume_label : "(none)", COL_TEXT_WHITE);
      { Txt t; t.u(fs.bytes_per_cluster / 1024).s(" KiB"); r.row("Cluster size", t.c(), COL_TEXT_WHITE); }
      if (fs.total_clusters) {
        const uint64_t tot = static_cast<uint64_t>(fs.total_clusters) * fs.bytes_per_cluster;
        const uint64_t fre = static_cast<uint64_t>(fs.free_clusters) * fs.bytes_per_cluster;
        char a[24], b[24]; khuman(tot - fre, a, sizeof(a)); khuman(tot, b, sizeof(b));
        Txt t; t.s(a).s(" used of ").s(b); r.row("Space", t.c(), COL_TEXT_WHITE);
        r.y += 2;
        draw_progress_bar(36, r.y, left_w - 32, 10, tot - fre, tot ? tot : 1, COL_CYAN);
      }
    } else {
      r.row("File system", "no FAT32 volume found", COL_AMBER);
    }
  }

  draw_window_frame(right_x, win_y, right_w, win_h, fs.mounted ? "Files in /sd" : "Files", COL_CYAN);
  const uint32_t c_name = right_x + 16, c_size = right_x + 320, c_kind = right_x + 460;
  uint32_t y = win_y + 42;
  ui_text(c_name, y, "NAME", 0x7D8796, Face::Small);
  ui_text(c_size, y, "SIZE", 0x7D8796, Face::Small);
  ui_text(c_kind, y, "KIND", 0x7D8796, Face::Small);
  fb_line_h(right_x + 12, y + 18, right_w - 24, COL_BORDER);
  y += 28;
  if (!fs.mounted || g_fat_file_count == 0) {
    ui_text(c_name, y, fs.mounted ? "The card is empty." : "No mounted card to list.", COL_TEXT_MUTED);
  }
  const size_t rows_fit = (win_y + win_h - 16 - y) / 26;
  for (size_t i = 0; fs.mounted && i < g_fat_file_count && i < rows_fit; ++i) {
    const auto& f = g_fat_files[i];
    if (i % 2 == 1) fb_fill(right_x + 12, y - 5, right_w - 24, 26, 0x161B25);
    text_clip(c_name, y, f.name, f.is_directory ? COL_CYAN : COL_TEXT_WHITE, c_size - c_name - 12);
    char sz[32];
    if (f.is_directory) str_copy(sz, "-", sizeof(sz)); else khuman(f.size_bytes, sz, sizeof(sz));
    ui_text(c_size, y, sz, COL_TEXT_MUTED);
    ui_text(c_kind, y, f.is_directory ? "folder" : "file", COL_TEXT_MUTED);
    y += 26;
  }
}

/* One implementation for the GPIO keys (t/i/o/u/d/s) and buttons. */
void gpio_do(char c) {
  if (c == 's') {
    g_gpio_i2c_cache_count = gpio_i2c_scan_bus(2, 3, g_gpio_i2c_cache, 16);
    Txt t; t.s("I2C scan done: ").u(g_gpio_i2c_cache_count).s(g_gpio_i2c_cache_count == 1 ? " device answered." : " devices answered.");
    str_copy(g_gpio_status_msg, t.c(), sizeof(g_gpio_status_msg));
    g_gpio_status_col = COL_EMERALD;
    return;
  }
  const HeaderPinDef* def = gpio_get_header_def(g_selected_pin);
  if (!def || def->bcm_gpio < 0) {
    str_copy(g_gpio_status_msg, "That pin is a power or ground rail, not a GPIO.", sizeof(g_gpio_status_msg));
    g_gpio_status_col = COL_AMBER;
    return;
  }
  const unsigned bcm = static_cast<unsigned>(def->bcm_gpio);
  if (c == 't') {
    gpio_set_mode(bcm, GpioMode::Output);
    gpio_toggle(bcm);
    str_copy(g_gpio_status_msg, gpio_read(bcm) ? "Pin driven high (3.3 V)." : "Pin driven low (0 V).", sizeof(g_gpio_status_msg));
    g_gpio_status_col = COL_EMERALD;
  } else if (c == 'i') {
    gpio_set_mode(bcm, GpioMode::Input);
    str_copy(g_gpio_status_msg, "Pin set to input.", sizeof(g_gpio_status_msg));
    g_gpio_status_col = COL_CYAN;
  } else if (c == 'o') {
    gpio_set_mode(bcm, GpioMode::Output);
    str_copy(g_gpio_status_msg, "Pin set to output.", sizeof(g_gpio_status_msg));
    g_gpio_status_col = COL_EMERALD;
  } else if (c == 'u') {
    gpio_set_pull(bcm, GpioPull::Up);
    str_copy(g_gpio_status_msg, "Pull-up resistor on.", sizeof(g_gpio_status_msg));
    g_gpio_status_col = COL_PURPLE;
  } else if (c == 'd') {
    gpio_set_pull(bcm, GpioPull::Down);
    str_copy(g_gpio_status_msg, "Pull-down resistor on.", sizeof(g_gpio_status_msg));
    g_gpio_status_col = COL_PURPLE;
  }
}

/* ----------------- Tab 6: Raspberry Pi 40-Pin GPIO Header & Hardware Control ----------------- */
void draw_tab_gpio(uint32_t w, uint32_t h) {
  uint32_t win_y = 52;
  uint32_t win_h = h - win_y - 36;
  uint32_t win1_w = 560;
  if (w < 1200) win1_w = (w - 52) * 45 / 100;
  uint32_t win2_x = 20 + win1_w + 12;
  uint32_t win2_w = (w > win2_x + 20) ? (w - win2_x - 20) : 400;

  /* Left Panel: 40-Pin Raspberry Pi Header Visualizer */
  draw_window_frame(20, win_y, win1_w, win_h, "40-pin header (J8), live levels", COL_CYAN);

  /* Sub-header Platform Banner */
  fb_box(30, win_y + 34, win1_w - 20, 24, 0x11161F, COL_BORDER);
  ui_text(38, win_y + 40, "BOARD", COL_TEXT_MUTED);
  ui_text(96, win_y + 40, gpio_platform_name(), COL_EMERALD);

  /* Header Table Columns */
  uint32_t grid_y = win_y + 64;
  {
    const uint32_t hc = 0x7D8796;
    ui_text(32, grid_y + 1, "BCM", hc, Face::Small);
    ui_text(58, grid_y + 1, "NAME", hc, Face::Small);
    ui_text(210, grid_y + 1, "M", hc, Face::Small);
    ui_text(228, grid_y + 1, "V", hc, Face::Small);
    ui_text(254, grid_y + 1, "PIN", hc, Face::Small);
    ui_text(294, grid_y + 1, "PIN", hc, Face::Small);
    ui_text(342, grid_y + 1, "V", hc, Face::Small);
    ui_text(358, grid_y + 1, "M", hc, Face::Small);
    ui_text(374, grid_y + 1, "NAME", hc, Face::Small);
    ui_text(win1_w - 18, grid_y + 1, "BCM", hc, Face::Small);
  }
  fb_line_h(30, grid_y + 14, win1_w - 20, COL_BORDER);

  GpioPinStatus status[40];
  gpio_sample_all_pins(status);

  uint32_t row_y = grid_y + 18;
  for (uint8_t row = 0; row < 20; ++row) {
    uint8_t p_left = row * 2 + 1;
    uint8_t p_right = row * 2 + 2;
    const GpioPinStatus& l = status[p_left - 1];
    const GpioPinStatus& r = status[p_right - 1];

    bool l_sel = (g_selected_pin == p_left);
    bool r_sel = (g_selected_pin == p_right);

    /* Row Background / Selection highlight */
    if (l_sel || r_sel) {
      fb_box(28, row_y - 2, win1_w - 16, 22, 0x1A283C, COL_BORDER_GLOW);
    } else if (row % 2 == 1) {
      fb_box(28, row_y - 2, win1_w - 16, 22, 0x0E121A, 0x0E121A);
    }

    /* Left Pin Color */
    uint32_t l_col = COL_TEXT_WHITE;
    if (l.type == PinType::Power3V3) l_col = COL_CRIMSON;
    else if (l.type == PinType::Power5V) l_col = COL_AMBER;
    else if (l.type == PinType::Ground) l_col = 0x556070;
    else if (l.bcm_gpio == 2 || l.bcm_gpio == 3) l_col = COL_CYAN;
    else if (l.bcm_gpio >= 7 && l.bcm_gpio <= 11) l_col = COL_EMERALD;
    else if (l.bcm_gpio == 14 || l.bcm_gpio == 15) l_col = COL_PURPLE;

    /* Right Pin Color */
    uint32_t r_col = COL_TEXT_WHITE;
    if (r.type == PinType::Power3V3) r_col = COL_CRIMSON;
    else if (r.type == PinType::Power5V) r_col = COL_AMBER;
    else if (r.type == PinType::Ground) r_col = 0x556070;
    else if (r.bcm_gpio == 2 || r.bcm_gpio == 3) r_col = COL_CYAN;
    else if (r.bcm_gpio >= 7 && r.bcm_gpio <= 11) r_col = COL_EMERALD;
    else if (r.bcm_gpio == 14 || r.bcm_gpio == 15) r_col = COL_PURPLE;

    /* Left BCM */
    char bcm_buf[8];
    if (l.bcm_gpio >= 0) {
      int_to_str(l.bcm_gpio, bcm_buf, sizeof(bcm_buf));
      ui_text(32, row_y + 2, bcm_buf, COL_TEXT_MUTED);
    } else {
      ui_text(32, row_y + 2, "--", COL_BORDER);
    }

    /* Left Name (truncated to fit) */
    char l_name[22];
    const char* src_l = l.alt0_name ? l.alt0_name : l.name;
    size_t n_l = 0;
    while (src_l[n_l] && n_l < 18) { l_name[n_l] = src_l[n_l]; ++n_l; }
    l_name[n_l] = '\0';
    ui_text(58, row_y + 2, l_name, l_col);

    /* Left Mode */
    if (l.bcm_gpio >= 0) {
      const char* m_str = (l.mode == GpioMode::Input) ? "I" : ((l.mode == GpioMode::Output) ? "O" : "A");
      ui_text(210, row_y + 2, m_str, l.mode == GpioMode::Output ? COL_EMERALD : COL_CYAN);
    }

    /* Left Level LED & Value */
    ui_text(228, row_y + 2, l.level ? "1" : "0", l.level ? COL_EMERALD : COL_TEXT_MUTED);
    fb_fill(242, row_y + 5, 8, 8, l.level ? (l.type == PinType::Power3V3 || l.type == PinType::Power5V ? l_col : COL_EMERALD) : 0x222B38);

    /* Physical Pin Badges (Centered) */
    char pin_l_str[6], pin_r_str[6];
    int_to_str(p_left, pin_l_str, sizeof(pin_l_str));
    int_to_str(p_right, pin_r_str, sizeof(pin_r_str));

    fb_round_box(256, row_y, 24, 18, l_sel ? COL_CYAN : 0x1E2430, l_sel ? COL_TEXT_WHITE : COL_BORDER);
    ui_text(p_left < 10 ? 264 : 260, row_y + 2, pin_l_str, l_sel ? 0x000000 : COL_TEXT_WHITE);

    fb_round_box(294, row_y, 24, 18, r_sel ? COL_CYAN : 0x1E2430, r_sel ? COL_TEXT_WHITE : COL_BORDER);
    ui_text(p_right < 10 ? 302 : 298, row_y + 2, pin_r_str, r_sel ? 0x000000 : COL_TEXT_WHITE);

    /* Right Level LED & Value */
    fb_fill(326, row_y + 5, 8, 8, r.level ? (r.type == PinType::Power3V3 || r.type == PinType::Power5V ? r_col : COL_EMERALD) : 0x222B38);
    ui_text(342, row_y + 2, r.level ? "1" : "0", r.level ? COL_EMERALD : COL_TEXT_MUTED);

    /* Right Mode */
    if (r.bcm_gpio >= 0) {
      const char* m_str = (r.mode == GpioMode::Input) ? "I" : ((r.mode == GpioMode::Output) ? "O" : "A");
      ui_text(358, row_y + 2, m_str, r.mode == GpioMode::Output ? COL_EMERALD : COL_CYAN);
    }

    /* Right Name */
    char r_name[22];
    const char* src_r = r.alt0_name ? r.alt0_name : r.name;
    size_t n_r = 0;
    while (src_r[n_r] && n_r < 20) { r_name[n_r] = src_r[n_r]; ++n_r; }
    r_name[n_r] = '\0';
    ui_text(374, row_y + 2, r_name, r_col);

    /* Right BCM */
    if (r.bcm_gpio >= 0) {
      int_to_str(r.bcm_gpio, bcm_buf, sizeof(bcm_buf));
      ui_text(win1_w - 18, row_y + 2, bcm_buf, COL_TEXT_MUTED);
    } else {
      ui_text(win1_w - 18, row_y + 2, "--", COL_BORDER);
    }

    row_y += 24;
  }

  /* Right Upper Panel: Active Pin Inspector */
  uint32_t panel1_h = 290;
  draw_window_frame(win2_x, win_y, win2_w, panel1_h, "Selected pin", COL_EMERALD);

  const HeaderPinDef* cur_def = gpio_get_header_def(g_selected_pin);
  GpioPinStatus cur_status = {};
  if (cur_def) gpio_sample_pin_status(g_selected_pin, cur_status);

  uint32_t insp_y = win_y + 36;
  char sel_title[96] = "Selected: Physical Pin ";
  char p_str[8];
  int_to_str(g_selected_pin, p_str, sizeof(p_str));
  size_t tp = kstrlen(sel_title);
  for (size_t i = 0; p_str[i]; ++i) sel_title[tp++] = p_str[i];
  if (cur_status.bcm_gpio >= 0) {
    const char b_pre[] = " (BCM GPIO ";
    for (size_t i = 0; b_pre[i]; ++i) sel_title[tp++] = b_pre[i];
    char bcm_num[8];
    int_to_str(cur_status.bcm_gpio, bcm_num, sizeof(bcm_num));
    for (size_t i = 0; bcm_num[i]; ++i) sel_title[tp++] = bcm_num[i];
    sel_title[tp++] = ')';
  }
  sel_title[tp] = '\0';

  fb_box(win2_x + 16, insp_y, win2_w - 32, 26, 0x1A2534, COL_CYAN);
  ui_text(win2_x + 26, insp_y + 5, sel_title, COL_TEXT_WHITE);

  insp_y += 34;
  auto draw_insp_row = [&](const char* label, const char* val, uint32_t val_col) {
    ui_text(win2_x + 20, insp_y, label, COL_TEXT_MUTED);
    text_clip(win2_x + 160, insp_y, val, val_col, win2_w - 180);
    insp_y += 22;
  };

  draw_insp_row("Function", cur_status.alt0_name ? cur_status.alt0_name : cur_status.name, COL_CYAN);

  const char* mode_desc = "power rail";
  if (cur_status.bcm_gpio >= 0) {
    if (cur_status.mode == GpioMode::Input) mode_desc = "input";
    else if (cur_status.mode == GpioMode::Output) mode_desc = "output";
    else if (cur_status.mode == GpioMode::Alt0) mode_desc = "peripheral (alt 0)";
    else mode_desc = "peripheral (alt function)";
  }
  draw_insp_row("Mode", mode_desc, COL_EMERALD);

  draw_insp_row("Level", cur_status.level ? "high (3.3 V)" : "low (0 V)", cur_status.level ? COL_EMERALD : COL_AMBER);

  char freq_s[32];
  int_to_str(cur_status.transition_hz, freq_s, sizeof(freq_s));
  str_copy(freq_s + kstrlen(freq_s), " Hz", sizeof(freq_s) - kstrlen(freq_s));
  draw_insp_row("Toggling at", cur_status.transition_hz > 0 ? freq_s : "not toggling", COL_TEXT_WHITE);

  draw_insp_row("Device", cur_status.detected_device ? cur_status.detected_device : "none detected", COL_TEXT_WHITE);

  /* Actions */
  {
    uint32_t btn_row_y = win_y + panel1_h - 72;
    uint32_t bx = win2_x + 16;
    bx += ui_button(bx, btn_row_y, "[T] Toggle", UI_GPIO_TOGGLE, BtnStyle::Primary) + 8;
    bx += ui_button(bx, btn_row_y, "[I] Input", UI_GPIO_INPUT) + 8;
    bx += ui_button(bx, btn_row_y, "[O] Output", UI_GPIO_OUTPUT) + 8;
    bx += ui_button(bx, btn_row_y, "[U] Pull-up", UI_GPIO_PULLUP) + 8;
    ui_button(bx, btn_row_y, "[D] Pull-down", UI_GPIO_PULLDOWN);
  }

  /* Status message line */
  ui_text(win2_x + 20, win_y + panel1_h - 22, g_gpio_status_msg, g_gpio_status_col);

  /* Right Lower Panel: I2C Discovery & Attached Devices */
  uint32_t panel2_y = win_y + panel1_h + 10;
  uint32_t panel2_h = win_h - panel1_h - 10;
  draw_window_frame(win2_x, panel2_y, win2_w, panel2_h, "I2C bus 1", COL_PURPLE);

  if (!g_gpio_i2c_scanned) {
    g_gpio_i2c_cache_count = gpio_i2c_scan_bus(2, 3, g_gpio_i2c_cache, 16);
    g_gpio_i2c_scanned = true;
  }

  uint32_t i2c_y = panel2_y + 34;
  ui_text(win2_x + 20, i2c_y, "BUS:", COL_TEXT_MUTED);
  ui_text(win2_x + 60, i2c_y, "GPIO 2 (SDA), GPIO 3 (SCL)", COL_CYAN);

  char dev_cnt_s[48] = "devices: ";
  char cnt_num[8];
  int_to_str(g_gpio_i2c_cache_count, cnt_num, sizeof(cnt_num));
  str_copy(dev_cnt_s + kstrlen(dev_cnt_s), cnt_num, sizeof(dev_cnt_s) - kstrlen(dev_cnt_s));
  ui_text(win2_x + win2_w - 180, i2c_y, dev_cnt_s, COL_EMERALD);
  fb_line_h(win2_x + 16, i2c_y + 16, win2_w - 32, COL_BORDER);

  i2c_y += 22;
  ui_text(win2_x + 24, i2c_y, "ADDR", 0x7D8796, Face::Small);
  ui_text(win2_x + 80, i2c_y, "NAME", 0x7D8796, Face::Small);
  ui_text(win2_x + 260, i2c_y, "LIKELY PART", 0x7D8796, Face::Small);
  ui_text(win2_x + win2_w - 110, i2c_y, "STATUS", 0x7D8796, Face::Small);

  i2c_y += 18;
  for (size_t i = 0; i < g_gpio_i2c_cache_count && i < 5; ++i) {
    fb_box(win2_x + 16, i2c_y - 2, win2_w - 32, 24, 0x111622, COL_BORDER);
    char hex_s[12] = "0x";
    int_to_str_hex(g_gpio_i2c_cache[i].addr, hex_s + 2, sizeof(hex_s) - 2);
    ui_text(win2_x + 24, i2c_y + 3, hex_s, COL_AMBER);
    text_clip(win2_x + 80, i2c_y + 3, g_gpio_i2c_cache[i].name, COL_TEXT_WHITE, 170);
    text_clip(win2_x + 260, i2c_y + 3, g_gpio_i2c_cache[i].description, COL_TEXT_MUTED, win2_w - 260 - 120);
    ui_text(win2_x + win2_w - 110, i2c_y + 3, "answered", COL_EMERALD);
    i2c_y += 28;
  }

  if (g_gpio_i2c_cache_count == 0) {
    ui_text(win2_x + 24, i2c_y + 3, gpio_i2c_bus_stuck() ? "The bus is held low - check wiring and pull-ups."
                                                         : "No device answered on this bus.", gpio_i2c_bus_stuck() ? COL_AMBER : COL_TEXT_MUTED);
  }
  ui_button(win2_x + 16, panel2_y + panel2_h - 40, "[S] Scan bus again", UI_GPIO_SCAN, BtnStyle::Primary);
}

/* Execute command from Terminal input */
void execute_term_cmd(const char* cmd) {
  while (*cmd == ' ') ++cmd;
  if (!*cmd) return;

  char echo_buf[140] = "$ ";
  size_t p = 2;
  for (size_t i = 0; cmd[i] && p < sizeof(echo_buf) - 1; ++i) echo_buf[p++] = cmd[i];
  echo_buf[p] = '\0';
  term_append_line(echo_buf);

  if (streq(cmd, "info")) {
    /* The list of hardware this machine HAS, not what it was designed
     * around: each line is gated on the driver's own state. */
    term_append_line("AIOS AArch64 bare-metal substrate");
    term_append_line(fdt_info().valid ? fdt_info().model : "     (no device tree model string)");
    term_append_line(net_eth_info().initialized ? "     eth0  : SMSC LAN9514 up" : "     eth0  : not initialized");
    term_append_line(net_wifi_info().initialized ? "     wlan0 : BCM43438 up" : "     wlan0 : not initialized");
    term_append_line(usb_hid_status().initialized ? "     usb   : DWC2 host up" : "     usb   : not initialized");
  } else if (streq(cmd, "ip") || streq(cmd, "ifconfig") || streq(cmd, "net")) {
    const auto& eth = net_eth_info();
    const auto& wifi = net_wifi_info();
    char ip_s[20], gw_s[20], mask_s[20], mac_s[20];

    ip_to_str(eth.ip_cfg.ip, ip_s, sizeof(ip_s));
    ip_to_str(eth.ip_cfg.gateway, gw_s, sizeof(gw_s));
    ip_to_str(eth.ip_cfg.netmask, mask_s, sizeof(mask_s));
    mac_to_str(eth.mac, mac_s, sizeof(mac_s));
    char eth_l1[128] = "[*] eth0 (LAN9514): ";
    size_t p = 20;
    const char* st1 = eth.link_up ? "Link up" : "DOWN";
    for (size_t i = 0; st1[i]; ++i) eth_l1[p++] = st1[i];
    eth_l1[p++] = ' '; eth_l1[p++] = '|'; eth_l1[p++] = ' ';
    for (size_t i = 0; mac_s[i]; ++i) eth_l1[p++] = mac_s[i];
    eth_l1[p] = '\0';
    term_append_line(eth_l1);

    char eth_l2[128] = "     inet ";
    p = 10;
    for (size_t i = 0; ip_s[i]; ++i) eth_l2[p++] = ip_s[i];
    const char m_pref[] = " netmask ";
    for (size_t i = 0; m_pref[i]; ++i) eth_l2[p++] = m_pref[i];
    for (size_t i = 0; mask_s[i]; ++i) eth_l2[p++] = mask_s[i];
    const char g_pref[] = " gw ";
    for (size_t i = 0; g_pref[i]; ++i) eth_l2[p++] = g_pref[i];
    for (size_t i = 0; gw_s[i]; ++i) eth_l2[p++] = gw_s[i];
    eth_l2[p] = '\0';
    term_append_line(eth_l2);

    ip_to_str(wifi.ip_cfg.ip, ip_s, sizeof(ip_s));
    mac_to_str(wifi.mac, mac_s, sizeof(mac_s));
    char wifi_l1[128] = "[*] wlan0 (BCM43438): ";
    p = 22;
    for (size_t i = 0; mac_s[i]; ++i) wifi_l1[p++] = mac_s[i];
    const char s_pref[] = " | SSID: ";
    for (size_t i = 0; s_pref[i]; ++i) wifi_l1[p++] = s_pref[i];
    const char* sname = wifi.ssid[0] ? wifi.ssid : "(none)";
    for (size_t i = 0; sname[i]; ++i) wifi_l1[p++] = sname[i];
    wifi_l1[p] = '\0';
    term_append_line(wifi_l1);

    char wifi_l2[128] = "     inet ";
    p = 10;
    for (size_t i = 0; ip_s[i]; ++i) wifi_l2[p++] = ip_s[i];
    wifi_l2[p] = '\0';
    term_append_line(wifi_l2);
  } else if (streq(cmd, "dhcp")) {
    term_append_line("[*] Broadcasting DHCPDISCOVER on eth0 and wlan0...");
    net_start_dhcp(NetIfType::Ethernet);
    net_start_dhcp(NetIfType::Wifi);
    for (unsigned i = 0; i < 20; ++i) {
      net_poll();
      if (net_is_dhcp_bound(NetIfType::Ethernet) || net_is_dhcp_bound(NetIfType::Wifi)) break;
      metal_delay_us(50000);
    }
    const auto& prim = net_primary_info();
    if (prim.ip_cfg.dhcp_bound) {
      char ip_s[20], b_msg[96] = "[ok] DHCP Bound! Assigned IP: ";
      ip_to_str(prim.ip_cfg.ip, ip_s, sizeof(ip_s));
      size_t pl = 30;
      for (size_t i = 0; ip_s[i]; ++i) b_msg[pl++] = ip_s[i];
      b_msg[pl] = '\0';
      term_append_line(b_msg);
    } else {
      term_append_line("[*] DHCP Discover dispatched; awaiting lease response in background.");
    }
  } else if (cmd[0] == 'p' && cmd[1] == 'i' && cmd[2] == 'n' && cmd[3] == 'g') {
    const char* p_ip = cmd + 4;
    while (*p_ip == ' ') ++p_ip;
    if (!*p_ip) {
      term_append_line("usage: ping <IP Address> (e.g. ping 192.168.1.1)");
    } else {
      /* Parse target IP string */
      uint8_t target_ip[4] = {};
      unsigned oct = 0, val = 0;
      const char* s = p_ip;
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

      char p_start[80] = "[*] PING ";
      size_t p = 9;
      for (size_t i = 0; p_ip[i] && p_ip[i] != ' ' && p < sizeof(p_start) - 16; ++i) p_start[p++] = p_ip[i];
      const char p_end[] = " (64 bytes)...";
      for (size_t i = 0; p_end[i]; ++i) p_start[p++] = p_end[i];
      p_start[p] = '\0';
      term_append_line(p_start);

      uint32_t rtt_us = 0;
      if (net_ping(target_ip, 1000, rtt_us)) {
        char rep_msg[96] = "[ok] 64 bytes ICMP reply: rtt=";
        size_t pl = 30;
        char num[24];
        int_to_str(rtt_us / 1000, num, sizeof(num));
        for (size_t i = 0; num[i]; ++i) rep_msg[pl++] = num[i];
        rep_msg[pl++] = '.';
        int_to_str((rtt_us % 1000) / 10, num, sizeof(num));
        if (num[0] == '\0') rep_msg[pl++] = '0';
        else for (size_t i = 0; num[i]; ++i) rep_msg[pl++] = num[i];
        const char ms_s[] = " ms";
        for (size_t i = 0; ms_s[i]; ++i) rep_msg[pl++] = ms_s[i];
        rep_msg[pl] = '\0';
        term_append_line(rep_msg);
      } else {
        term_append_line("[err] Request timed out (no reply received).");
      }
    }
  } else if (streq(cmd, "tcp") || streq(cmd, "tcp status") || streq(cmd, "sockets")) {
    TcpSocketInfo socks[TCP_MAX_SOCKETS];
    size_t n_socks = net_tcp_get_all_info(socks, TCP_MAX_SOCKETS);
    if (n_socks == 0) {
      term_append_line("[ok] No active TCP sockets. Use 'tcp listen <port>' or 'tcp connect <ip> <port>'");
    } else {
      char hdr[64] = "[*] Active TCP Sockets: ";
      char sc[16];
      int_to_str(n_socks, sc, sizeof(sc));
      size_t hp = 24;
      for (size_t i = 0; sc[i]; ++i) hdr[hp++] = sc[i];
      hdr[hp] = '\0';
      term_append_line(hdr);
      for (size_t i = 0; i < n_socks; ++i) {
        char s_line[128];
        char rip[20], lp[12], rp[12];
        ip_to_str(socks[i].remote_ip, rip, sizeof(rip));
        int_to_str(socks[i].local_port, lp, sizeof(lp));
        int_to_str(socks[i].remote_port, rp, sizeof(rp));
        str_copy(s_line, "    sock ", sizeof(s_line));
        char id_s[8];
        int_to_str(socks[i].id, id_s, sizeof(id_s));
        size_t sp = kstrlen(s_line);
        for (size_t k = 0; id_s[k]; ++k) s_line[sp++] = id_s[k];
        const char m1[] = ": :";
        for (size_t k = 0; m1[k]; ++k) s_line[sp++] = m1[k];
        for (size_t k = 0; lp[k]; ++k) s_line[sp++] = lp[k];
        const char m2[] = " -> ";
        for (size_t k = 0; m2[k]; ++k) s_line[sp++] = m2[k];
        for (size_t k = 0; rip[k]; ++k) s_line[sp++] = rip[k];
        s_line[sp++] = ':';
        for (size_t k = 0; rp[k]; ++k) s_line[sp++] = rp[k];
        const char m3[] = " (";
        for (size_t k = 0; m3[k]; ++k) s_line[sp++] = m3[k];
        const char* st_name = tcp_state_str(socks[i].state);
        for (size_t k = 0; st_name[k]; ++k) s_line[sp++] = st_name[k];
        s_line[sp++] = ')';
        s_line[sp] = '\0';
        term_append_line(s_line);
      }
    }
  } else if (cmd[0] == 't' && cmd[1] == 'c' && cmd[2] == 'p' && cmd[3] == ' ' &&
             cmd[4] == 'l' && cmd[5] == 'i' && cmd[6] == 's' && cmd[7] == 't' && cmd[8] == 'e' && cmd[9] == 'n') {
    const char* p_port = cmd + 10;
    while (*p_port == ' ') ++p_port;
    uint16_t port = 0;
    while (*p_port >= '0' && *p_port <= '9') {
      port = port * 10 + static_cast<uint16_t>(*p_port - '0');
      ++p_port;
    }
    if (port == 0) {
      term_append_line("usage: tcp listen <port>");
    } else {
      int s = net_tcp_listen(port);
      if (s > 0) {
        term_append_line("[ok] TCP listener created successfully.");
      } else {
        term_append_line("[err] Failed to create TCP listener (table full or invalid).");
      }
    }
  } else if (streq(cmd, "mem")) {
    char b1[32], b2[32], m_msg[96];
    khuman(metal_heap_used(), b1, sizeof(b1));
    khuman(metal_heap_total(), b2, sizeof(b2));
    str_copy(m_msg, "[ok] Heap Used: ", sizeof(m_msg));
    size_t pl = 16;
    for (size_t i = 0; b1[i]; ++i) m_msg[pl++] = b1[i];
    m_msg[pl++] = ' '; m_msg[pl++] = '/'; m_msg[pl++] = ' ';
    for (size_t i = 0; b2[i]; ++i) m_msg[pl++] = b2[i];
    m_msg[pl] = '\0';
    term_append_line(m_msg);
  } else if (streq(cmd, "wifi") || streq(cmd, "wifi status")) {
    const NetInterfaceInfo& w = net_wifi_info();
    if (!w.initialized) {
      term_append_line("[err] BCM43438 Wi-Fi is not initialized (dongle never came up)");
    } else {
      term_append_line(w.link_state == NetLinkState::Associated ? "[ok] Wi-Fi radio up, associated"
                                                                 : "[ok] Wi-Fi radio up, not associated");
      do_wifi_scan(); if (g_scan_count == 0) term_append_line("[err] no networks found - see 'wifi log'");
    }
  } else if (streq(cmd, "usb")) {
    const auto& u = usb_hid_status();
    term_append_line(u.initialized ? "[ok] DWC2 USB Host Controller Active" : "[err] USB Offline");
    term_append_line(u.keyboard_detected ? "     USB Keyboard: Connected & Listening" : "     USB Keyboard: None");
    term_append_line(u.mouse_detected ? "     USB Mouse: Connected & Listening" : "     USB Mouse: None");
  } else if (streq(cmd, "sd")) {
    const auto& fs = fat32_status();
    term_append_line(fs.mounted ? "[ok] MicroSD Card Mounted as FAT32 at /sd" : "[err] SD Card Offline");
  } else if (streq(cmd, "ps")) {
    term_append_line("EL0 Embedded Tasks:");
    term_append_line("  [0] u_hello - Syscall hello world");
    term_append_line("  [1] u_spin  - Timer preemption loop");
    term_append_line("  [2] u_crash - Data abort fault trap");
    term_append_line("  [3] u_priv  - EL1 instruction trap");
    term_append_line("  [4] u_neon  - 128-bit SIMD arithmetic");
  } else if (streq(cmd, "neon")) {
    term_append_line("[*] Running 128-bit NEON vector compute self-test (details on serial)...");
    term_append_line(neon_selftest() ? "[ok] NEON kernels agree with the scalar oracle."
                                     : "[err] NEON kernels DISAGREE with the scalar oracle - see serial output.");
  } else if (cmd[0] == 'r' && cmd[1] == 'u' && cmd[2] == 'n') {
    const char* p_arg = cmd + 3;
    while (*p_arg == ' ') ++p_arg;
    unsigned idx = (*p_arg >= '0' && *p_arg <= '9') ? static_cast<unsigned>(*p_arg - '0') : 99;
    if (idx < 5) {
      char run_msg[48] = "[*] Launching EL0 Process ";
      run_msg[26] = static_cast<char>('0' + idx);
      run_msg[27] = '.'; run_msg[28] = '.'; run_msg[29] = '.';
      run_msg[30] = '\0';
      term_append_line(run_msg);
      proc_run(idx);
      term_append_line("[ok] EL0 Process completed execution.");
    } else {
      term_append_line("[err] Unknown program index (use 0..4)");
    }
  } else if (streq(cmd, "reset") || streq(cmd, "reboot")) {
    term_append_line("[*] Rebooting machine via PSCI...");
    metal_power_reboot();
  } else if (streq(cmd, "halt") || streq(cmd, "poweroff")) {
    term_append_line("[*] Powering off system via PSCI...");
    metal_power_off();
  } else {
    /* Anything this tab does not special-case goes to the REAL shell - the
     * same dispatcher the serial console uses - with everything it prints
     * captured and echoed here.
     *
     * Before this, the GUI terminal had its own small hand-written command
     * list and answered everything else with "Unknown command", so the full
     * diagnostics (wifi status, sdio, usb, eth, fat32, dt, irq ...) existed
     * ONLY over a serial cable. Anyone standing at the HDMI output could not
     * run them at all, which is no way to debug a machine you are looking
     * straight at. */
    static char out[4096];
    kconsole_capture_start(out, sizeof(out));
    dispatch_shell_command(cmd);
    const size_t n = kconsole_capture_stop();

    if (n == 0) {
      term_append_line("[err] Unknown command. Type 'help' for options.");
    } else {
      /* Split the captured output into terminal lines. */
      char line[140];
      size_t col = 0;
      for (size_t i = 0; i < n; ++i) {
        const char ch = out[i];
        if (ch == '\r') continue;
        if (ch == '\n' || col + 1 >= sizeof(line)) {
          line[col] = '\0';
          if (col > 0) term_append_line(line);
          col = 0;
          if (ch != '\n') { line[col++] = ch; }
          continue;
        }
        line[col++] = ch;
      }
      if (col > 0) { line[col] = '\0'; term_append_line(line); }
    }
  }
}

/* Click Hit Testing Dispatcher */
void desktop_handle_click(int32_t x, int32_t y) {
  uint32_t w = fb_width();
  uint32_t h = fb_height();
  (void)h;

  /* 0. The join dialog owns every click while it is open. */
  if (g_wifi_modal.active) {
    for (unsigned i = 0; i < g_ui_button_count; ++i) {
      const UiButton& bt = g_ui_buttons[i];
      if (x < static_cast<int32_t>(bt.x) || x >= static_cast<int32_t>(bt.x + bt.w) ||
          y < static_cast<int32_t>(bt.y) || y >= static_cast<int32_t>(bt.y + bt.h)) continue;
      switch (bt.action) {
        case UI_MODAL_SSID:     g_wifi_modal.focus_field = 1; break;
        case UI_MODAL_PSK:      g_wifi_modal.focus_field = 0; break;
        case UI_MODAL_SHOW:     g_wifi_modal.show_password = !g_wifi_modal.show_password; g_wifi_modal.focus_field = 2; break;
        case UI_MODAL_REMEMBER: g_wifi_modal.remember = !g_wifi_modal.remember; g_wifi_modal.focus_field = 3; break;
        case UI_MODAL_CONNECT:  submit_wifi_connect(); break;
        case UI_MODAL_CANCEL:   g_wifi_modal.active = false; break;
        default: break;
      }
      return;
    }
    return;
  }

  /* 1. Top Bar Tabs Hit Test */
  if (y >= 4 && y <= 32) {
    for (unsigned i = 0; i < g_tab_count; ++i) {
      if (x >= static_cast<int32_t>(g_tab_x0[i]) && x < static_cast<int32_t>(g_tab_x1[i])) {
        g_active_tab = static_cast<DesktopTab>(i);
        return;
      }
    }
    /* Close Button (Top-Right) */
    if (w >= 1024 && x >= static_cast<int32_t>(w - 36) && x <= static_cast<int32_t>(w - 16)) {
      g_desktop_active = false;
      return;
    }
  }

  /* 2. Registered buttons: exactly the rectangles painted last frame. */
  for (unsigned i = 0; i < g_ui_button_count; ++i) {
    const UiButton& b = g_ui_buttons[i];
    if (x < static_cast<int32_t>(b.x) || x >= static_cast<int32_t>(b.x + b.w) ||
        y < static_cast<int32_t>(b.y) || y >= static_cast<int32_t>(b.y + b.h)) continue;
    const int a = b.action;
    if (a == UI_WIFI_SCAN) do_wifi_scan();
    else if (a == UI_WIFI_JOIN) open_wifi_connect_modal();
    else if (a == UI_WIFI_FORGET) wifi_clear_saved_config();
    else if (a == UI_WIFI_UP) { if (g_scan_scroll) --g_scan_scroll; }
    else if (a == UI_WIFI_DOWN) { if (g_scan_scroll + 1 < g_scan_count) ++g_scan_scroll; }
    else if (a == UI_LAN_DHCP) net_start_dhcp(NetIfType::Ethernet);
    else if (a == UI_LAN_PING) lan_ping_router();
    else if (a == UI_FLEET_SURVEY) fleet_do_survey();
    else if (a == UI_FLEET_JOIN) { fleet_request_join(); str_copy(g_fleet_msg, "Joining from FLEET.CFG...", sizeof(g_fleet_msg)); g_fleet_msg_col = COL_AMBER; }
    else if (a == UI_FLEET_LEAVE) { fleet_leave(); g_fleet_member_count = 0; str_copy(g_fleet_msg, "Left the hub. J joins again.", sizeof(g_fleet_msg)); g_fleet_msg_col = COL_AMBER; }
    else if (a == UI_GPIO_TOGGLE) gpio_do('t');
    else if (a == UI_GPIO_INPUT) gpio_do('i');
    else if (a == UI_GPIO_OUTPUT) gpio_do('o');
    else if (a == UI_GPIO_PULLUP) gpio_do('u');
    else if (a == UI_GPIO_PULLDOWN) gpio_do('d');
    else if (a == UI_GPIO_SCAN) gpio_do('s');
    else if (a >= UI_WIFI_ROW0 && a < UI_WIFI_ROW0 + static_cast<int>(kScanMax)) {
      const size_t row = static_cast<size_t>(a - UI_WIFI_ROW0);
      if (row < g_ap_rows_drawn) open_wifi_connect_modal(g_scan_cache[g_ap_row_idx[row]].ssid);
    } else if (a >= UI_PROC_RUN0 && a < UI_PROC_RUN0 + 16) {
      const unsigned pi = static_cast<unsigned>(a - UI_PROC_RUN0);
      char msg[48] = "[*] Launching process ";
      msg[22] = static_cast<char>('0' + pi); msg[23] = '\0';
      term_append_line(msg);
      proc_run(pi);
    }
    return;
  }

  if (g_active_tab == DesktopTab::Gpio) {
    uint32_t win_y = 52;
    uint32_t win1_w = 560;
    if (w < 1200) win1_w = (w - 52) * 45 / 100;
    uint32_t grid_y = win_y + 64;
    uint32_t row_y = grid_y + 18;

    /* Hit Test Header Pins */
    for (uint8_t row = 0; row < 20; ++row) {
      uint8_t p_left = row * 2 + 1;
      uint8_t p_right = row * 2 + 2;
      if (y >= static_cast<int32_t>(row_y - 2) && y <= static_cast<int32_t>(row_y + 20)) {
        if (x >= 28 && x <= static_cast<int32_t>(256 + 24)) {
          g_selected_pin = p_left;
          return;
        } else if (x >= static_cast<int32_t>(294) && x <= static_cast<int32_t>(win1_w)) {
          g_selected_pin = p_right;
          return;
        }
      }
      row_y += 24;
    }

    (void)win1_w;
  }
}

}  /* namespace */

void desktop_init() {
  g_term_line_count = 0;
  g_term_input_len = 0;
  g_term_input[0] = '\0';
  g_mouse_x = fb_width() / 2;
  g_mouse_y = fb_height() / 2;
  usb_set_mouse_pos(g_mouse_x, g_mouse_y);
  g_wifi_modal = {};

  /* USB was already brought up during boot; this is a no-op unless the
   * desktop is entered without it. It must NOT re-reset the controller. */
  bool usb_ok = usb_hid_init();

  /* Mount MicroSD Card FAT32 Partition */
  fat32_mount();
  /* The timezone the operator chose (`tz NAME`), kept on the card. */
  {
    char tz[32] = {}; size_t tl = 0;
    if (fat32_status().mounted && fat32_read_file("TZ.CFG", tz, sizeof(tz) - 1, tl) && tl) {
      tz[tl] = 0;
      metal_set_timezone(tz);
    }
  }
  g_fat_file_count = fat32_list("/", g_fat_files, 16);

  /* Load Saved Wi-Fi Configuration from SD Card */
  WifiSavedConfig saved_cfg = {};
  if (wifi_load_saved_config(saved_cfg) && saved_cfg.auto_connect && saved_cfg.ssid[0]) {
    wifi_auto_connect_saved();
  }

  term_append_line("AIOS - AArch64, bare metal, Raspberry Pi 3 (BCM2837)");
  term_append_line("");
  {
    /* Every line below is the subsystem's own answer. This banner used to
     * print "Wireless Stack Online" and "FAT32 Mounted" unconditionally, on
     * boards where neither had come up. */
    char line[128];
    unsigned p = 0;
    auto put_u = [&](uint32_t v) {
      char d[12]; unsigned n = 0;
      do { d[n++] = static_cast<char>('0' + v % 10); v /= 10; } while (v && n < sizeof(d));
      while (n && p < sizeof(line) - 1) line[p++] = d[--n];
    };
    const char* head = "[ok] Framebuffer ";
    for (unsigned i = 0; head[i]; ++i) line[p++] = head[i];
    put_u(fb_width());
    line[p++] = 'x';
    put_u(fb_height());
    const char* tail = " 32bpp via VideoCore mailbox";
    for (unsigned i = 0; tail[i] && p < sizeof(line) - 1; ++i) line[p++] = tail[i];
    line[p] = '\0';
    term_append_line(line);

    const NetInterfaceInfo& w = net_wifi_info();
    if (!w.initialized) {
      term_append_line("[!!] BCM43438 Wi-Fi did not initialize - see 'wifi log' / serial console");
    } else if (w.link_state == NetLinkState::Associated) {
      term_append_line("[ok] BCM43438 Wi-Fi up and associated [wlan0]");
    } else {
      term_append_line("[ok] BCM43438 Wi-Fi radio up, not associated [wlan0]");
    }
  }
  if (usb_ok) {
    const UsbHidStatus& hid = usb_hid_status();
    if (hid.keyboard_detected || hid.mouse_detected) {
      term_append_line("[ok] Synopsys DWC2 USB Host & Physical Keyboard/Mouse Driver Online");
    } else {
      term_append_line("[!!] DWC2 USB Host Online but no Keyboard/Mouse enumerated on hub");
    }
  } else {
    term_append_line("[!!] DWC2 USB Host Controller FAILED to initialize (no port/device)");
  }
  term_append_line(fat32_status().mounted ? "[ok] MicroSD card mounted as FAT32 at /sd/"
                                          : "[!!] No FAT32 volume on the SD card (SDHOST) - /sd/ is unavailable");
  if (saved_cfg.valid && saved_cfg.ssid[0]) {
    term_append_line(net_wifi_info().link_state == NetLinkState::Associated
                         ? "[ok] Auto-connected to the Wi-Fi remembered in /sd/wifi.cfg"
                         : "[!!] Remembered Wi-Fi in /sd/wifi.cfg did not associate");
  }
  term_append_line("Graphical workspace up. Use the USB mouse and keyboard, or F1-F8 for tabs. Type 'help'.");
}

bool desktop_is_active() { return g_desktop_active; }

void desktop_paint() {
  if (!fb_live()) return;

  uint32_t w = fb_width();
  uint32_t h = fb_height();

  g_ui_button_count = 0;
  g_scroll_region_count = 0;
  g_ap_rows_drawn = 0;
  /* Clear / Wallpaper background */
  fb_clear(COL_BG_DARK);

  /* Subtle background grid */
  for (uint32_t x = 0; x < w; x += 40) fb_line_v(x, 37, h - 65, 0x0E121A);
  for (uint32_t y = 37; y < h - 28; y += 40) fb_line_h(0, y, w, 0x0E121A);

  /* Top & Bottom Navigation Bars */
  draw_top_bar(w);
  draw_bottom_bar(w, h);

  /* Render Active Workspace Tab */
  switch (g_active_tab) {
    case DesktopTab::Dashboard: draw_tab_dashboard(w, h); break;
    case DesktopTab::Lan:       draw_tab_lan(w, h); break;
    case DesktopTab::Wifi:      draw_tab_wifi(w, h); break;
    case DesktopTab::Terminal:  draw_tab_terminal(w, h); break;
    case DesktopTab::Processes: draw_tab_processes(w, h); break;
    case DesktopTab::Storage:   draw_tab_storage(w, h); break;
    case DesktopTab::Gpio:      draw_tab_gpio(w, h); break;
    case DesktopTab::Fleet:     draw_tab_fleet(w, h); break;
    default: break;
  }

  /* Render Wi-Fi Password / Connection Modal Dialog if active */
  if (g_wifi_modal.active) {
    draw_wifi_modal(w, h);
  }

  /* The whole screen was just redrawn, so whatever was saved under the old
   * cursor position is stale. Present the clean frame, then let the cursor
   * overlay capture its background and draw itself.
   *
   * The cursor must NOT be drawn before that capture: fb_cursor_show saves
   * whatever is underneath, so drawing it first would save the cursor's own
   * pixels as "background" and smear a ghost at this position on the next
   * pointer move. */
  fb_cursor_invalidate();
  fb_present_all();
  fb_cursor_show(static_cast<uint32_t>(g_mouse_x), static_cast<uint32_t>(g_mouse_y));
}

uint32_t g_serial_bytes = 0;

/* One SD breadcrumb per click the dashboard acts on: where it came from,
 * where it landed, which tab, and the last raw HID reports. A hang then
 * leaves its trigger in crashlog.txt (kept as CRASHOLD.TXT across the
 * watchdog reboot) instead of a blank screen. */
void crumb_click(const char* src) {
  char b[320];
  size_t n = 0;
  auto put = [&](const char* t) { while (*t && n + 1 < sizeof(b)) b[n++] = *t++; };
  auto num = [&](uint64_t v) { char t[24]; int_to_str(v, t, sizeof(t)); put(t); };
  put("click src="); put(src);
  put(" x="); num(static_cast<uint64_t>(g_mouse_x));
  put(" y="); num(static_cast<uint64_t>(g_mouse_y));
  put(" tab="); num(static_cast<uint64_t>(g_active_tab));
  put(" serial_bytes="); num(g_serial_bytes);
  put(" | ");
  b[n] = 0;
  usb_hid_recent_reports(b + n, sizeof(b) - n);
  crashlog(b);
}

void desktop_run() {
  if (!fb_live()) {
    kputs("[desktop] no framebuffer display available\n");
    return;
  }

  desktop_init();
  g_desktop_active = true;
  fbcon_suspend(); /* Desktop takes full control of the glass */

  desktop_paint();

  crashlog(fat32_status().mounted ? "desktop loop start (sd mounted)" : "desktop loop start (sd NOT mounted)");
  metal_watchdog_arm();
  bool running = true;
  uint64_t hb_last = 0, hb_loops = 0;
  while (running && g_desktop_active) {
    metal_watchdog_pet();
    /* Once-a-second heartbeat: the last one received dates a hang. */
    ++hb_loops;
    if (metal_now_ns() - hb_last > 1000000000ull) {
      hb_last = metal_now_ns();
      char hb[96]; size_t n = 0;
      auto put = [&](const char* t) { while (*t && n + 1 < sizeof(hb)) hb[n++] = *t++; };
      auto num = [&](uint64_t v) { char t[24]; int_to_str(v, t, sizeof(t)); put(t); };
      put("hb loops="); num(hb_loops); put(" mouse="); num(static_cast<uint64_t>(g_mouse_x)); put(",");
      num(static_cast<uint64_t>(g_mouse_y)); put(" tab="); num(static_cast<uint64_t>(g_active_tab));
      hb[n] = 0;
      net_breadcrumb(hb);
      hb_loops = 0;
    }
    usb_hid_trace("loop top");
    /* Poll network ring buffers and DHD packets */
    net_poll();
    usb_hid_trace("net_poll done");

    /* screenshot_server_poll()'s accept+encode+send only ever lived in
     * main.cpp's OUTER console loop - unreachable from here, since this
     * dashboard auto-launches at boot and blocks in this very while() until
     * Escape, which on a headless board never happens. The TCP handshake
     * itself still completed (net_poll() above drives that at the state-
     * machine level regardless), which is what made this look like it
     * "should" work from a `net` command's socket listing - but nothing
     * was ever calling accept() past that, so no screenshot was ever sent.
     * Same bug class as fleet_join_test/node_service_start; this one is
     * safe to actually call from a blocking context like this frame loop
     * (unlike those two) because nothing here calls it from INSIDE
     * net_poll() itself, so there is no recursive-blocking risk - it costs
     * this dashboard a pause of a second or so only while a screenshot is
     * actively being fetched, not on every frame. */
    screenshot_server_poll();
    usb_hid_trace("screenshot done");
    node_service_poll();
    usb_hid_trace("node_service done");
    things_frame_tick();

    /* Poll physical USB HID Keyboard and Mouse reports */
    usb_hid_poll();

    /* Check physical USB Mouse motion and clicks */
    int32_t umx = 0, umy = 0;
    uint8_t ubtns = 0;
    if (usb_get_mouse_state(umx, umy, ubtns)) {
      if (umx != g_mouse_x || umy != g_mouse_y) {
        const int32_t jx = umx - g_mouse_x, jy = umy - g_mouse_y;
        if (jx > 200 || jx < -200 || jy > 200 || jy < -200) {
          /* One report moving the pointer this far is the "flies off the
           * screen" symptom - record the raw reports that did it. */
          g_mouse_x = umx; g_mouse_y = umy;
          crumb_click("POINTER-JUMP(no click)");
        }
        g_mouse_x = umx;
        g_mouse_y = umy;
        /* Pointer motion is an overlay update, not a screen repaint. */
        usb_hid_trace("cursor move");
        fb_cursor_move(static_cast<uint32_t>(g_mouse_x), static_cast<uint32_t>(g_mouse_y));
        usb_hid_trace("cursor moved");
      }
      /* Left Click Detection */
      if ((ubtns & 1) && !(g_prev_mouse_btns & 1)) {
        crumb_click("usb-mouse");
        desktop_handle_click(g_mouse_x, g_mouse_y);
        desktop_paint();
      }
      g_prev_mouse_btns = ubtns;
    }

    /* Check input from USB Keyboard or Serial Console.
     *
     * Remember WHICH source it came from. An ESC typed on the USB keyboard
     * is a bare ESC key; only a serial terminal sends ESC as the lead byte
     * of a cursor sequence. Reaching for the UART to disambiguate a USB
     * keypress meant that any byte sitting in the UART receive register -
     * or a UART that is not reporting "empty" correctly on a headless board
     * - turned the ESC key into the start of an escape sequence and
     * swallowed it, which is why ESC would not close the connect pane. */
    /* Mouse wheel: scroll whichever pane is under the pointer. */
    if (const int32_t wheel = usb_take_mouse_wheel()) {
      for (unsigned i = 0; i < g_scroll_region_count; ++i) {
        const ScrollRegion& r = g_scroll_regions[i];
        if (g_mouse_x < static_cast<int32_t>(r.x) || g_mouse_x >= static_cast<int32_t>(r.x + r.w) ||
            g_mouse_y < static_cast<int32_t>(r.y) || g_mouse_y >= static_cast<int32_t>(r.y + r.h)) continue;
        const int32_t lines = wheel * 3;            /* up (+) = back in time / up the list */
        auto bump = [&](uint32_t& v, int32_t d) { const int64_t nv = static_cast<int64_t>(v) + d; v = nv < 0 ? 0 : static_cast<uint32_t>(nv); };
        if (r.pane == SCR_BOOT_LOG) bump(g_boot_log_scroll, lines);
        else if (r.pane == SCR_LAN_LOG) bump(g_lan_log_scroll, lines);
        else if (r.pane == SCR_WIFI_LOG) bump(g_wifi_log_scroll, lines);
        else if (r.pane == SCR_TERMINAL) bump(g_term_scroll, lines);
        else if (r.pane == SCR_NETWORKS) {
          const int64_t nv = static_cast<int64_t>(g_scan_scroll) - wheel;
          g_scan_scroll = nv < 0 ? 0 : static_cast<size_t>(nv);   /* clamped when drawn */
        }
        desktop_paint();
        break;
      }
    }
    usb_hid_trace("mouse handled");
    bool key_from_usb = true;
    int c = usb_get_key();
    if (c < 0) {
      key_from_usb = false;
      c = metal_console_read();
      if (c >= 0) ++g_serial_bytes;
    }

    if (c >= 0) {
      if (c == 27) { /* ESC key, or the lead byte of a serial escape sequence */
        int c2 = key_from_usb ? -1 : metal_console_read();
        if (c2 < 0) {
          /* Bare ESC key */
          if (g_wifi_modal.active) {
            g_wifi_modal.active = false;
            desktop_paint();
            continue;
          }
          running = false;
          break;
        }
        if (c2 == '[') {
          int c3 = metal_console_read();
          if (c3 == 'A') { /* Up Arrow */
            g_mouse_y = (g_mouse_y > 16) ? g_mouse_y - 16 : 0;
            desktop_paint();
            continue;
          } else if (c3 == 'B') { /* Down Arrow */
            g_mouse_y = (g_mouse_y + 16 < static_cast<int32_t>(fb_height())) ? g_mouse_y + 16 : fb_height() - 1;
            desktop_paint();
            continue;
          } else if (c3 == 'C') { /* Right Arrow */
            g_mouse_x = (g_mouse_x + 16 < static_cast<int32_t>(fb_width())) ? g_mouse_x + 16 : fb_width() - 1;
            desktop_paint();
            continue;
          } else if (c3 == 'D') { /* Left Arrow */
            g_mouse_x = (g_mouse_x > 16) ? g_mouse_x - 16 : 0;
            desktop_paint();
            continue;
          } else if (c3 == 'M') { /* X10 / ANSI Mouse Click Report */
            int b = metal_console_read();
            int mx = metal_console_read();
            int my = metal_console_read();
            if (b >= 32 && mx >= 32 && my >= 32) {
              g_mouse_x = (mx - 32) * 8;
              g_mouse_y = (my - 32) * 16;
              crumb_click("serial-x10-mouse");
              desktop_handle_click(g_mouse_x, g_mouse_y);
              desktop_paint();
              continue;
            }
          }
        }
      }

      /* An ESC that was not a recognised escape sequence still means
       * "dismiss", and must not fall through into the modal's text input. */
      if (c == 27 && g_wifi_modal.active) {
        g_wifi_modal.active = false;
        desktop_paint();
        continue;
      }

      /* Modal Active: Route Keyboard Directly to Modal Input Fields */
      if (g_wifi_modal.active) {
        if (c == '\r' || c == '\n') {
          submit_wifi_connect();
          desktop_paint();
        } else if (c == '\t') {
          g_wifi_modal.focus_field = (g_wifi_modal.focus_field + 1) % 6;
          desktop_paint();
        } else if (c == 0x7F || c == 0x08) {
          if (g_wifi_modal.focus_field == 0 && g_wifi_modal.psk_len > 0) {
            g_wifi_modal.psk[--g_wifi_modal.psk_len] = '\0';
            desktop_paint();
          } else if (g_wifi_modal.focus_field == 1 && g_wifi_modal.ssid_len > 0) {
            g_wifi_modal.ssid[--g_wifi_modal.ssid_len] = '\0';
            desktop_paint();
          }
        } else if (c >= 32 && c < 127) {
          if (g_wifi_modal.focus_field == 0 && g_wifi_modal.psk_len + 1 < sizeof(g_wifi_modal.psk)) {
            g_wifi_modal.psk[g_wifi_modal.psk_len++] = static_cast<char>(c);
            g_wifi_modal.psk[g_wifi_modal.psk_len] = '\0';
            desktop_paint();
          } else if (g_wifi_modal.focus_field == 1 && g_wifi_modal.ssid_len + 1 < sizeof(g_wifi_modal.ssid)) {
            g_wifi_modal.ssid[g_wifi_modal.ssid_len++] = static_cast<char>(c);
            g_wifi_modal.ssid[g_wifi_modal.ssid_len] = '\0';
            desktop_paint();
          } else if (g_wifi_modal.focus_field == 2 && (c == ' ' || c == 'p' || c == 'P')) {
            g_wifi_modal.show_password = !g_wifi_modal.show_password;
            desktop_paint();
          } else if (g_wifi_modal.focus_field == 3 && (c == ' ' || c == 'r' || c == 'R')) {
            g_wifi_modal.remember = !g_wifi_modal.remember;
            desktop_paint();
          } else if (g_wifi_modal.focus_field == 4 && (c == ' ')) {
            submit_wifi_connect();
            desktop_paint();
          } else if (g_wifi_modal.focus_field == 5 && (c == ' ')) {
            g_wifi_modal.active = false;
            desktop_paint();
          }
        }
        continue;
      }

      /* Tab Key: Cycle Workspace Tabs */
      if (c == '\t') {
        uint8_t next = (static_cast<uint8_t>(g_active_tab) + 1) % static_cast<uint8_t>(DesktopTab::Count);
        g_active_tab = static_cast<DesktopTab>(next);
        desktop_paint();
        continue;
      }

      /* F1-F7 switch tabs from ANY tab, including the Terminal, which
       * otherwise swallows every printable character. */
      if (c >= USB_KEY_F1 && c <= (USB_KEY_F1 + 7)) {
        g_active_tab = static_cast<DesktopTab>(c - USB_KEY_F1);
        desktop_paint();
        continue;
      }

      /* Handle typing in Terminal tab or global commands */
      if (g_active_tab == DesktopTab::Terminal) {
        g_term_scroll = 0;
        if (c == '\r' || c == '\n') {
          g_term_input[g_term_input_len] = '\0';
          execute_term_cmd(g_term_input);
          g_term_input_len = 0;
          g_term_input[0] = '\0';
          desktop_paint();
        } else if (c == 0x7F || c == 0x08) {
          if (g_term_input_len > 0) {
            --g_term_input_len;
            g_term_input[g_term_input_len] = '\0';
            desktop_paint();
          }
        } else if (c >= 32 && c < 127 && g_term_input_len + 1 < sizeof(g_term_input)) {
          g_term_input[g_term_input_len++] = static_cast<char>(c);
          g_term_input[g_term_input_len] = '\0';
          desktop_paint();
        }
      } else {
        /* Hotkeys & Pointer Navigation on other tabs */
        if (c == '1') { g_active_tab = DesktopTab::Dashboard; desktop_paint(); }
        else if (c == '2') { g_active_tab = DesktopTab::Lan; desktop_paint(); }
        else if (c == '3') { g_active_tab = DesktopTab::Wifi; desktop_paint(); }
        else if (c == '4') { g_active_tab = DesktopTab::Terminal; desktop_paint(); }
        else if (c == '5') { g_active_tab = DesktopTab::Processes; desktop_paint(); }
        else if (c == '6') { g_active_tab = DesktopTab::Storage; desktop_paint(); }
        else if (c == '7') { g_active_tab = DesktopTab::Gpio; desktop_paint(); }
        else if (c == '8') { g_active_tab = DesktopTab::Fleet; desktop_paint(); }
        else if (g_active_tab == DesktopTab::Fleet && (c == 's' || c == 'S' || c == 'j' || c == 'J' || c == 'l' || c == 'L')) {
          if (c == 's' || c == 'S') fleet_do_survey();
          else if (c == 'j' || c == 'J') { fleet_request_join(); str_copy(g_fleet_msg, "Joining from FLEET.CFG...", sizeof(g_fleet_msg)); g_fleet_msg_col = COL_AMBER; }
          else { fleet_leave(); g_fleet_member_count = 0; str_copy(g_fleet_msg, "Left the hub. J joins again.", sizeof(g_fleet_msg)); g_fleet_msg_col = COL_AMBER; }
          desktop_paint();
        }
        else if (g_active_tab == DesktopTab::Gpio) {
          const char lc = static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c);
          if (lc == 't' || lc == 'i' || lc == 'o' || lc == 'u' || lc == 'd' || lc == 's') {
            gpio_do(lc);
            desktop_paint();
          } else if (c == 'w' || c == 'W') {
            g_selected_pin = (g_selected_pin > 2) ? g_selected_pin - 2 : g_selected_pin;
            desktop_paint();
          } else if (c == 'z' || c == 'Z') {
            g_selected_pin = (g_selected_pin + 2 <= 40) ? g_selected_pin + 2 : g_selected_pin;
            desktop_paint();
          } else if (c == ' ' || c == '\r' || c == '\n') {
            crumb_click(key_from_usb ? "usb-key-space/enter" : "serial-key-space/enter");
            desktop_handle_click(g_mouse_x, g_mouse_y);
            desktop_paint();
          }
        } else if (c == ' ' || c == '\r' || c == '\n') {
          crumb_click(key_from_usb ? "usb-key-space/enter" : "serial-key-space/enter");
          desktop_handle_click(g_mouse_x, g_mouse_y);
          desktop_paint();
        } else if (c == 'w' || c == 'W') {
          g_mouse_y = (g_mouse_y > 20) ? g_mouse_y - 20 : 0;
          desktop_paint();
        } else if (c == 's' || c == 'S') {
          if (g_active_tab == DesktopTab::Wifi) {
            do_wifi_scan();
          } else {
            g_mouse_y = (g_mouse_y + 20 < static_cast<int32_t>(fb_height())) ? g_mouse_y + 20 : fb_height() - 1;
          }
          desktop_paint();
        } else if (c == 'c' || c == 'C') {
          if (g_active_tab == DesktopTab::Wifi) {
            open_wifi_connect_modal();
            desktop_paint();
          }
        } else if (c == 'f' || c == 'F') {
          if (g_active_tab == DesktopTab::Wifi) {
            wifi_clear_saved_config();
            desktop_paint();
          }
        } else if ((c == '[' || c == ']') && g_active_tab == DesktopTab::Wifi) {
          if (c == '[' && g_scan_scroll) --g_scan_scroll;
          if (c == ']' && g_scan_scroll + 1 < g_scan_count) ++g_scan_scroll;
          desktop_paint();
        } else if (c == ',' || c == '<') {
          /* Scroll the kernel log panel back into history. */
          if (g_active_tab == DesktopTab::Lan)  { g_lan_log_scroll += 5; desktop_paint(); }
          if (g_active_tab == DesktopTab::Wifi) { g_wifi_log_scroll += 5; desktop_paint(); }
        } else if (c == '.' || c == '>') {
          if (g_active_tab == DesktopTab::Lan) {
            g_lan_log_scroll = (g_lan_log_scroll > 5) ? g_lan_log_scroll - 5 : 0;
            desktop_paint();
          }
          if (g_active_tab == DesktopTab::Wifi) {
            g_wifi_log_scroll = (g_wifi_log_scroll > 5) ? g_wifi_log_scroll - 5 : 0;
            desktop_paint();
          }
        } else if (c == 'p' || c == 'P') {
          /* [P] PING ROUTER on the LAN tab. Free key everywhere else. */
          if (g_active_tab == DesktopTab::Lan) {
            lan_ping_router();
            desktop_paint();
          }
        } else if (c == 'a' || c == 'A') {
          g_mouse_x = (g_mouse_x > 20) ? g_mouse_x - 20 : 0;
          desktop_paint();
        } else if (c == 'd' || c == 'D') {
          /* [D] RENEW DHCP on the LAN tab, same overload pattern s/c/f
           * already use for the Wi-Fi tab; mouse-right everywhere else. */
          if (g_active_tab == DesktopTab::Lan) {
            net_start_dhcp(NetIfType::Ethernet);
          } else {
            g_mouse_x = (g_mouse_x + 20 < static_cast<int32_t>(fb_width())) ? g_mouse_x + 20 : fb_width() - 1;
          }
          desktop_paint();
        } else if (c == 't' || c == 'T') {
          net_send_test_frame();
          desktop_paint();
        } else if (c == 'r' || c == 'R') {
          desktop_paint();
        }
      }
    }

    usb_hid_trace("keys handled");
    uint64_t now_ms = metal_now_ns() / 1000000ull;
    if (now_ms - g_last_render_ms >= 500) {
      g_last_render_ms = now_ms;
      usb_hid_trace("paint");
      desktop_paint();
      usb_hid_trace("painted");
    } else {
      metal_delay_us(5000);
    }
  }

  g_desktop_active = false;
  fbcon_resume();
  kputs("\n[desktop] left GUI desktop mode; back to console shell\n");
}

