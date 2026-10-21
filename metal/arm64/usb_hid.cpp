/* USB HID Keyboard, Mouse & Hub Driver Implementation (metal/arm64/usb_hid.cpp).
 */
#include "usb_hid.hpp"
#include "fdt.hpp"

#include "dwc2.hpp"
#include "fb.hpp"
#include "kio.hpp"
#include "lan9514.hpp"
#include "timer.hpp"
#include "net.hpp"

namespace {

UsbHidStatus g_status = {};
uint64_t g_last_eth_init_ns = 0;

/* USB Keyboard Translation Tables */
const char HID_UNSHIFTED[256] = {
  0, 0, 0, 0,
  'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
  'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
  '\r', 27, '\b', '\t', ' ', '-', '=', '[', ']', '\\', '#', ';', '\'',
  '`', ',', '.', '/', 0, /* CapsLock */
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* F1-F12 */
};

const char HID_SHIFTED[256] = {
  0, 0, 0, 0,
  'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
  'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
  '\r', 27, '\b', '\t', ' ', '_', '+', '{', '}', '|', '~', ':', '"',
  '~', '<', '>', '?', 0,
};

/* Keyboard input ring buffer */
constexpr size_t KEY_BUF_SIZE = 64;
char g_key_ring[KEY_BUF_SIZE];
size_t g_key_head = 0;
size_t g_key_tail = 0;
uint8_t g_prev_keys[6] = {};

/* Mouse State */
int32_t g_mouse_px = 640;
int32_t g_mouse_py = 360;
uint8_t g_mouse_btns = 0;
int32_t g_mouse_wheel = 0;   /* accumulated notches, + = away from the user */

/* Registered HID Endpoints (Keyboards, Mice, Logitech Receivers, Hubs) */
struct HidEndpoint {
  uint8_t dev_addr;
  uint8_t ep_addr;
  uint8_t max_pkt;
  UsbSpeed speed;
  uint8_t hub_addr;
  uint8_t hub_port;
  uint8_t interface_num;
  uint8_t protocol;
  uint8_t data_toggle; /* 0 = DATA0, 2 = DATA1 */
  /* How often the device wants to be asked (its bInterval), and when it
   * last was. Polling every loop iteration instead - an interrupt-IN
   * transfer through the hub per endpoint per pass - cost the desktop
   * ~130 ms a second of USB time on the same bus as the Ethernet chip. */
  uint32_t interval_us;
  uint64_t last_poll_ns;
  bool is_mouse;
  bool is_keyboard;
  /* true: interface was put in Boot protocol (SET_PROTOCOL succeeded) and
   * reports use the fixed boot layout. false: decode strictly by the
   * layout parsed from the interface's HID report descriptor below. */
  bool boot;
  bool uses_report_ids;
  /* Mouse fields (bit offsets within the report, AFTER any report-ID byte) */
  uint8_t m_id;
  uint16_t m_btn_bit;  uint8_t m_btn_count;
  uint16_t m_x_bit;    uint8_t m_x_size;
  uint16_t m_y_bit;    uint8_t m_y_size;
  uint16_t m_w_bit;    uint8_t m_w_size;
  bool has_mouse_xy;
  /* Keyboard fields */
  uint8_t k_id;
  uint16_t k_mod_bit;  bool has_mods;
  uint16_t k_arr_bit;  uint8_t k_arr_count;
  bool has_key_array;
};

constexpr size_t MAX_HID_ENDPOINTS = 8;
HidEndpoint g_hid_endpoints[MAX_HID_ENDPOINTS] = {};
size_t g_num_endpoints = 0;
uint32_t ep_reports[MAX_HID_ENDPOINTS] = {};
/* Last few raw reports from any endpoint, for crash breadcrumbs. */
struct RawReport { uint8_t ep; uint8_t len; uint8_t b[8]; };
RawReport g_recent[6];
unsigned g_recent_head = 0;
uint8_t g_next_addr = 2;
uint64_t g_last_hub_poll_ns = 0;
uint8_t g_port_configured[6] = {};
uint8_t g_hub_ports = 5;
/* The hub is re-probed every 100 ms for hotplug; only the first pass narrates
 * itself, or the retained boot log would be nothing but rescan noise. */
bool g_enum_verbose = false;
/* Set by usb_hid_reinit_ethernet() to force ONE fully-narrated rescan pass
 * on demand, regardless of whether this is the first boot pass. Without
 * this, every retry after boot - the 100ms background rescan included -
 * ran with g_enum_verbose permanently false and printed nothing at all
 * about why a port kept failing, which made "net up" indistinguishable
 * from doing nothing. */
bool g_force_verbose_scan = false;

void enqueue_key(char c) {
  size_t next = (g_key_head + 1) % KEY_BUF_SIZE;
  if (next != g_key_tail) {
    g_key_ring[g_key_head] = c;
    g_key_head = next;
    ++g_status.key_events;
  }
}

/* Decode standard 8-byte USB HID Keyboard Boot Report */
bool g_caps_lock = false;

void decode_keyboard_report(const uint8_t report[8]) {
  uint8_t modifiers = report[0];
  bool shift = (modifiers & 0x22) != 0; /* bit 1 LeftShift, bit 5 RightShift */

  for (int i = 2; i < 8; ++i) {
    uint8_t key = report[i];
    if (key == 0) continue;

    /* Check if already pressed in previous frame (debounce) */
    bool was_pressed = false;
    for (int p = 0; p < 6; ++p) {
      if (g_prev_keys[p] == key) {
        was_pressed = true;
        break;
      }
    }

    if (!was_pressed) {
      /* Handle Function Keys F1-F6 */
      if (key >= 0x3A && key <= 0x41) {
        /* HID usages 0x3A..0x41 are F1..F8 (F7 the GPIO tab, F8 the Fleet tab) */
        enqueue_key(static_cast<char>(USB_KEY_F1 + (key - 0x3A)));
      }
      /* Handle Arrow Keys */
      else if (key == 0x4F) { /* Right Arrow */
        enqueue_key('d');
      } else if (key == 0x50) { /* Left Arrow */
        enqueue_key('a');
      } else if (key == 0x51) { /* Down Arrow */
        enqueue_key('s');
      } else if (key == 0x52) { /* Up Arrow */
        enqueue_key('w');
      } else if (key == 0x39) {
        /* CapsLock: toggle, and affect letters only - not digits or
         * punctuation, which is what Shift is for. */
        g_caps_lock = !g_caps_lock;
      } else {
        bool upper = shift;
        if (key >= 0x04 && key <= 0x1D && g_caps_lock) upper = !upper;  /* a-z */
        char ascii = upper ? HID_SHIFTED[key] : HID_UNSHIFTED[key];
        if (ascii != 0) {
          enqueue_key(ascii);
        }
      }
    }
  }

  for (int i = 0; i < 6; ++i) {
    g_prev_keys[i] = report[i + 2];
  }
}

void apply_mouse(uint8_t btns, int32_t dx, int32_t dy);

/* Decode standard 3-byte / 4-byte / 5-byte USB HID Mouse Report */
void decode_mouse_report(const uint8_t* report, size_t len) {
  /* Boot protocol only: buttons, dx, dy (int8), [wheel]. Boot reports
   * never carry a report ID. The old "a leading 1 or 2 is a report ID"
   * guess misread every boot report taken with the left or right button
   * held - and report-protocol reports were pushed through this same
   * layout, reading one byte of a 16-bit X as X and the other as Y, which
   * is what flung the pointer to the screen edge. */
  if (len < 3 || !report) return;
  /* Byte 3, when the mouse sends it, is the wheel - most do, even in boot
   * protocol (this board's mouse sends 4-byte reports). */
  if (len >= 4) g_mouse_wheel += static_cast<int8_t>(report[3]);
  apply_mouse(report[0] & 0x07, static_cast<int8_t>(report[1]), static_cast<int8_t>(report[2]));
}

void apply_mouse(uint8_t btns, int32_t dx, int32_t dy) {
  g_mouse_btns = btns & 0x07;
  g_mouse_px += dx;
  g_mouse_py += dy;

  /* Clamp to current framebuffer dimensions */
  int32_t max_x = fb_live() ? static_cast<int32_t>(fb_width() - 1) : 1279;
  int32_t max_y = fb_live() ? static_cast<int32_t>(fb_height() - 1) : 719;

  if (g_mouse_px < 0) g_mouse_px = 0;
  if (g_mouse_px > max_x) g_mouse_px = max_x;
  if (g_mouse_py < 0) g_mouse_py = 0;
  if (g_mouse_py > max_y) g_mouse_py = max_y;

  ++g_status.mouse_events;
}

/* ---- HID report descriptor parsing (HID 1.11 section 6.2.2) ----
 * Only what a keyboard or mouse needs: where the buttons, X, Y, modifier
 * bits and key array sit in an Input report, per report ID. */
uint32_t hid_bits(const uint8_t* r, size_t len, uint32_t bit, uint32_t size) {
  uint32_t v = 0;
  for (uint32_t i = 0; i < size && i < 32; ++i) {
    const uint32_t b = bit + i;
    if (b / 8 >= len) break;
    if ((r[b / 8] >> (b % 8)) & 1u) v |= (1u << i);
  }
  return v;
}

int32_t hid_signed(uint32_t v, uint32_t size) {
  if (size == 0 || size >= 32) return static_cast<int32_t>(v);
  const uint32_t sign = 1u << (size - 1);
  return (v & sign) ? static_cast<int32_t>(v | ~((1u << size) - 1u)) : static_cast<int32_t>(v);
}

void parse_report_descriptor(const uint8_t* d, size_t n, HidEndpoint& h) {
  uint16_t usage_page = 0;
  uint32_t report_size = 0, report_count = 0;
  int32_t logical_min = 0;
  uint8_t report_id = 0;
  uint16_t usages[16]; unsigned n_usages = 0;
  uint16_t usage_min = 0, usage_max = 0; bool have_range = false;
  struct IdPos { uint8_t id; uint16_t bit; } pos[8] = {}; unsigned n_pos = 1;   /* pos[0]: id 0 */
  auto bitpos = [&](uint8_t id) -> uint16_t& {
    for (unsigned i = 0; i < n_pos; ++i) if (pos[i].id == id) return pos[i].bit;
    if (n_pos < 8) { pos[n_pos].id = id; pos[n_pos].bit = 0; return pos[n_pos++].bit; }
    return pos[7].bit;
  };

  size_t i = 0;
  while (i < n) {
    const uint8_t prefix = d[i++];
    if (prefix == 0xFE) {                      /* long item: skip */
      if (i + 1 >= n) break;
      i += 2 + d[i];
      continue;
    }
    const uint32_t sz = (prefix & 3u) == 3 ? 4u : (prefix & 3u);
    if (i + sz > n) break;
    uint32_t data = 0;
    for (uint32_t k = 0; k < sz; ++k) data |= static_cast<uint32_t>(d[i + k]) << (8 * k);
    const int32_t sdata = hid_signed(data, sz * 8);
    i += sz;
    const uint8_t tag = prefix & 0xFCu;
    switch (tag) {
      case 0x04: usage_page = static_cast<uint16_t>(data); break;       /* Usage Page */
      case 0x14: logical_min = sdata; break;                            /* Logical Min */
      case 0x74: report_size = data; break;                             /* Report Size */
      case 0x84: report_id = static_cast<uint8_t>(data); h.uses_report_ids = true; break;
      case 0x94: report_count = data; break;                            /* Report Count */
      case 0x08: if (n_usages < 16) usages[n_usages++] = static_cast<uint16_t>(data); break;
      case 0x18: usage_min = static_cast<uint16_t>(data); have_range = true; break;
      case 0x28: usage_max = static_cast<uint16_t>(data); have_range = true; break;
      case 0x80: {                                                      /* Input */
        uint16_t& bp = bitpos(report_id);
        const bool constant = data & 0x01u;
        const bool variable = data & 0x02u;
        if (!constant) {
          if (usage_page == 0x09 && report_size == 1 && !h.m_btn_count) {        /* Buttons */
            h.m_id = report_id; h.m_btn_bit = bp;
            h.m_btn_count = static_cast<uint8_t>(report_count > 8 ? 8 : report_count);
          } else if (usage_page == 0x01 && variable) {                            /* Generic Desktop */
            for (uint32_t f = 0; f < report_count; ++f) {
              uint16_t u = have_range ? static_cast<uint16_t>(usage_min + f)
                                      : (n_usages ? usages[f < n_usages ? f : n_usages - 1] : 0);
              const uint16_t fbit = static_cast<uint16_t>(bp + f * report_size);
              if (u == 0x30 && !h.m_x_size) { h.m_id = report_id; h.m_x_bit = fbit; h.m_x_size = static_cast<uint8_t>(report_size); }
              if (u == 0x31 && !h.m_y_size) { h.m_y_bit = fbit; h.m_y_size = static_cast<uint8_t>(report_size); }
              if (u == 0x38 && !h.m_w_size) { h.m_w_bit = fbit; h.m_w_size = static_cast<uint8_t>(report_size); }
            }
            (void)logical_min;
          } else if (usage_page == 0x07) {                                        /* Keyboard */
            if (variable && report_size == 1 && have_range && usage_min == 0xE0 && !h.has_mods) {
              h.k_id = report_id; h.k_mod_bit = bp; h.has_mods = true;
            } else if (!variable && report_size == 8 && !h.has_key_array) {
              h.k_id = report_id; h.k_arr_bit = bp;
              h.k_arr_count = static_cast<uint8_t>(report_count > 6 ? 6 : report_count);
              h.has_key_array = true;
            }
          }
        }
        bp = static_cast<uint16_t>(bp + report_size * report_count);
        n_usages = 0; have_range = false;
        break;
      }
      case 0x90: case 0xB0: case 0xA0: case 0xC0:                       /* Output/Feature/Collection */
        n_usages = 0; have_range = false;
        break;
      default: break;
    }
  }
  h.has_mouse_xy = h.m_x_size > 0 && h.m_y_size > 0;
}

/* Decode a report-protocol report strictly by the parsed layout. */
void decode_by_layout(const HidEndpoint& ep, const uint8_t* buf, size_t bytes) {
  const uint8_t id = ep.uses_report_ids && bytes ? buf[0] : 0;
  const uint8_t* r = ep.uses_report_ids ? buf + 1 : buf;
  const size_t len = ep.uses_report_ids ? (bytes ? bytes - 1 : 0) : bytes;
  if (ep.is_mouse && ep.has_mouse_xy && id == ep.m_id) {
    const uint8_t btns = static_cast<uint8_t>(hid_bits(r, len, ep.m_btn_bit, ep.m_btn_count));
    const int32_t dx = hid_signed(hid_bits(r, len, ep.m_x_bit, ep.m_x_size), ep.m_x_size);
    const int32_t dy = hid_signed(hid_bits(r, len, ep.m_y_bit, ep.m_y_size), ep.m_y_size);
    if (ep.m_w_size) g_mouse_wheel += hid_signed(hid_bits(r, len, ep.m_w_bit, ep.m_w_size), ep.m_w_size);
    apply_mouse(btns, dx, dy);
  }
  if (ep.is_keyboard && ep.has_key_array && id == ep.k_id) {
    uint8_t boot8[8] = {};
    if (ep.has_mods) boot8[0] = static_cast<uint8_t>(hid_bits(r, len, ep.k_mod_bit, 8));
    for (uint8_t k = 0; k < ep.k_arr_count; ++k) {
      boot8[2 + k] = static_cast<uint8_t>(hid_bits(r, len, ep.k_arr_bit + k * 8u, 8));
    }
    decode_keyboard_report(boot8);
  }
}

/* Configure a device that has already been given an address.
 *
 * hub_addr/hub_port are 0/0 for a device sitting directly on the DWC2 root
 * port, or the internal hub's address and downstream port number for
 * anything behind it. Factoring this out of the hub-port walk is what lets a
 * root-port device be claimed at all: on a Pi 3 every USB jack is behind the
 * soldered-on LAN9514 hub, but the root port is where a device lands under
 * emulation and on boards without that hub, and it used to be enumerated,
 * addressed, and then silently dropped. */
bool configure_device(uint8_t dev_addr, UsbSpeed dev_speed,
                      uint8_t hub_addr, uint8_t hub_port) {
  /* Learn this device's EP0 max packet size BEFORE anything else.
   *
   * Every transfer below used to hardcode 8. That is only correct for a
   * Low/Full-Speed device: a High-Speed device - which is exactly what the
   * onboard LAN9514 Ethernet is - has a 64-byte EP0, and driving a 64-byte
   * endpoint with an 8-byte max packet size makes the controller break each
   * stage into the wrong number of packets, so every LAN9514 register access
   * failed and eth0 could never come up. 8 bytes is the one length the USB
   * spec guarantees a device in Default state will answer with. */
  /* USB 2.0 mandates a 64-byte EP0 on a High-Speed device; for Full/Low
   * speed, 8 is the only size guaranteed to work before the descriptor has
   * been read. */
  uint32_t probe_mps = (dev_speed == UsbSpeed::High) ? 64u : 8u;
  UsbDeviceDescriptor dev = {};
  if (!dwc2_control_msg(dev_addr, dev_speed, 0x80, 0x06 /* GET_DESCRIPTOR */, 0x0100, 0,
                        &dev, 8, probe_mps, hub_addr, hub_port)) {
    kprintf("[usb] addr %u port %u: device descriptor read failed\n",
            static_cast<uint64_t>(dev_addr), static_cast<uint64_t>(hub_port));
    return false;
  }
  uint32_t ep0_mps = dev.bMaxPacketSize0;
  if (ep0_mps != 8 && ep0_mps != 16 && ep0_mps != 32 && ep0_mps != 64) ep0_mps = 8;

  /* Now the full descriptor, at the real EP0 size */
  dwc2_control_msg(dev_addr, dev_speed, 0x80, 0x06, 0x0100, 0, &dev, sizeof(dev),
                   ep0_mps, hub_addr, hub_port);

  /* Only narrate on the first enumeration pass; the 100ms hotplug rescan
   * would otherwise reprint this for every device, forever. */
  if (g_enum_verbose) {
    kprintf("[usb] addr %u port %u: %x:%x class %x ep0 %u bytes\n",
            static_cast<uint64_t>(dev_addr), static_cast<uint64_t>(hub_port),
            static_cast<uint64_t>(dev.idVendor), static_cast<uint64_t>(dev.idProduct),
            static_cast<uint64_t>(dev.bDeviceClass), static_cast<uint64_t>(ep0_mps));
  }

  /* SMSC LAN9514 10/100 Ethernet (VID 0x0424) */
  if (dev.idVendor == 0x0424 &&
      (dev.idProduct == 0xEC00 || dev.idProduct == 0x9514 || dev.idProduct == 0x9512)) {
    dwc2_control_msg(dev_addr, dev_speed, 0x00, 0x09 /* SET_CONFIGURATION */, 1, 0,
                     nullptr, 0, ep0_mps, hub_addr, hub_port);
    metal_delay_us(10000);

    /* Back off between attempts.
     *
     * usb_hid_poll() re-walks the hub ports every 100ms, and a port whose
     * device failed to configure never gets marked done - so a failing
     * Ethernet init was retried ten times a second, forever. That buried
     * every other line of the kernel log under "init FAILED" (including the
     * [eth] lines saying WHY it failed) and made the on-screen log useless.
     * Retry, but at a rate a human can read. */
    const uint64_t now = metal_now_ns();
    if (g_last_eth_init_ns != 0 && (now - g_last_eth_init_ns) < 5000000000ull) {
      return false;  /* quietly - we already said why, 5 seconds ago */
    }
    g_last_eth_init_ns = now;

    if (lan9514_init(dev_addr, dev_speed, hub_addr, hub_port)) {
      g_status.ethernet_detected = true;
      g_status.ethernet_addr = dev_addr;
      kputs("[usb] lan9514 ethernet initialized\n");
      return true;
    }
    kputs("[usb] lan9514 ethernet init FAILED (retry in 5s)\n");
    return false;
  }

  /* Configuration descriptor: header first for the real total length */
  UsbConfigDescriptor cfg_hdr = {};
  if (!dwc2_control_msg(dev_addr, dev_speed, 0x80, 0x06, 0x0200, 0, &cfg_hdr, 9,
                        ep0_mps, hub_addr, hub_port)) {
    kputs("[usb] config descriptor header read failed\n");
    return false;
  }
  uint16_t total_len = cfg_hdr.wTotalLength;
  if (total_len > 512 || total_len < 9) total_len = 256;

  uint8_t cfg_buf[512] = {};
  if (!dwc2_control_msg(dev_addr, dev_speed, 0x80, 0x06, 0x0200, 0, cfg_buf, total_len,
                        ep0_mps, hub_addr, hub_port)) {
    kputs("[usb] config descriptor read failed\n");
    return false;
  }

  /* Activate Configuration 1 */
  dwc2_control_msg(dev_addr, dev_speed, 0x00, 0x09 /* SET_CONFIGURATION */, 1, 0,
                   nullptr, 0, ep0_mps, hub_addr, hub_port);
  metal_delay_us(10000);

  /* Parse Interfaces and Endpoints */
  uint32_t offset = 0;
  int cur_if_class = -1;
  int cur_if_protocol = -1;
  int cur_if_num = -1;
  int cur_if_subclass = -1;
  bool cur_if_boot = false;
  uint16_t cur_rdesc_len = 0;
  size_t claimed = 0;

  while (offset + 2 <= total_len) {
    uint8_t len = cfg_buf[offset];
    uint8_t type = cfg_buf[offset + 1];
    if (len == 0 || offset + len > total_len) break;

    if (type == 0x04 && len >= sizeof(UsbInterfaceDescriptor)) {
      auto* iface = reinterpret_cast<const UsbInterfaceDescriptor*>(&cfg_buf[offset]);
      cur_if_class = iface->bInterfaceClass;
      cur_if_protocol = iface->bInterfaceProtocol;
      cur_if_num = iface->bInterfaceNumber;
      cur_if_subclass = iface->bInterfaceSubClass;
      cur_if_boot = false;
      cur_rdesc_len = 0;

      if (cur_if_class == 0x03) { /* HID */
        /* SET_PROTOCOL(0) selects the Boot protocol, whose report layout is
         * the fixed 8-byte keyboard / 3-byte mouse format decoded below. It
         * must come before SET_IDLE, and it only exists on a boot-capable
         * interface (bInterfaceSubClass == 1). */
        if (iface->bInterfaceSubClass == 1 &&
            (iface->bInterfaceProtocol == 1 || iface->bInterfaceProtocol == 2)) {
          /* Boot layout is only trusted if the device ACCEPTED the switch. */
          cur_if_boot = dwc2_control_msg(dev_addr, dev_speed, 0x21, 0x0B /* SET_PROTOCOL */, 0,
                                         cur_if_num, nullptr, 0, ep0_mps, hub_addr, hub_port);
        }
        /* SET_IDLE(0) = report only on change */
        dwc2_control_msg(dev_addr, dev_speed, 0x21, 0x0A /* SET_IDLE */, 0,
                         cur_if_num, nullptr, 0, ep0_mps, hub_addr, hub_port);
      }
    } else if (type == 0x21 && len >= 9 && cur_if_class == 0x03) {
      /* HID descriptor: first class descriptor is normally the report
       * descriptor (type 0x22) and its length. */
      if (cfg_buf[offset + 6] == 0x22) {
        cur_rdesc_len = static_cast<uint16_t>(cfg_buf[offset + 7] | (cfg_buf[offset + 8] << 8));
      }
    } else if (type == 0x05 && len >= sizeof(UsbEndpointDescriptor)) {
      auto* ep = reinterpret_cast<const UsbEndpointDescriptor*>(&cfg_buf[offset]);
      bool is_in = (ep->bEndpointAddress & 0x80) != 0;
      bool is_interrupt = (ep->bmAttributes & 0x03) == 0x03;
      /* Only claim interrupt IN endpoints that belong to a HID interface.
       * Keying off the endpoint ADDRESS (0x81 == keyboard, 0x82 == mouse)
       * is meaningless - those are just the first and second IN endpoint
       * numbers on any device - and claiming a non-HID interrupt endpoint
       * feeds arbitrary device data into the keyboard decoder. */
      if (is_in && is_interrupt && cur_if_class == 0x03 &&
          g_num_endpoints < MAX_HID_ENDPOINTS) {
        HidEndpoint cand = {};
        cand.boot = cur_if_boot;
        if (cur_if_boot) {
          cand.is_keyboard = (cur_if_protocol == 1);
          cand.is_mouse = (cur_if_protocol == 2);
        } else {
          /* Report protocol: read the report descriptor and decode only
           * what it declares. Interfaces that carry neither keyboard keys
           * nor mouse X/Y (multimedia keys, a receiver's vendor channel)
           * are not claimed at all - their reports used to be guessed at by
           * length and fed into the cursor and keyboard as garbage. */
          static uint8_t rdesc[512];
          uint16_t rlen = cur_rdesc_len ? cur_rdesc_len : 256;
          if (rlen > sizeof(rdesc)) rlen = sizeof(rdesc);
          if (dwc2_control_msg(dev_addr, dev_speed, 0x81, 0x06, 0x2200, static_cast<uint16_t>(cur_if_num),
                               rdesc, rlen, ep0_mps, hub_addr, hub_port)) {
            parse_report_descriptor(rdesc, rlen, cand);
          }
          cand.is_mouse = cand.has_mouse_xy;
          cand.is_keyboard = cand.has_key_array;
        }
        if (!cand.is_mouse && !cand.is_keyboard) {
          kprintf("[usb]   hid ep %x if %u sub %u proto %u: not a keyboard or mouse - not claimed\n",
                  static_cast<uint64_t>(ep->bEndpointAddress), static_cast<uint64_t>(cur_if_num),
                  static_cast<uint64_t>(cur_if_subclass), static_cast<uint64_t>(cur_if_protocol));
          offset += len;
          continue;
        }
        auto& h = g_hid_endpoints[g_num_endpoints++];
        h = cand;
        h.dev_addr = dev_addr;
        h.ep_addr = ep->bEndpointAddress;
        uint16_t mps = ep->wMaxPacketSize & 0x7FFu;
        h.max_pkt = static_cast<uint8_t>((mps > 0 && mps <= 64) ? mps : 8);
        {
          /* Low/full speed: bInterval is milliseconds. High speed:
           * 2^(bInterval-1) microframes of 125 us. Clamped to 1..32 ms. */
          uint32_t us = 8000;
          const uint8_t bi = ep->bInterval;
          if (dev_speed == UsbSpeed::High) { if (bi >= 1 && bi <= 16) us = 125u << (bi - 1); }
          else if (bi > 0) us = static_cast<uint32_t>(bi) * 1000u;
          h.interval_us = us < 1000 ? 1000 : (us > 32000 ? 32000 : us);
          h.last_poll_ns = 0;
        }
        h.speed = dev_speed;
        h.hub_addr = hub_addr;
        h.hub_port = hub_port;
        h.interface_num = cur_if_num >= 0 ? cur_if_num : 0;
        h.protocol = cur_if_protocol >= 0 ? cur_if_protocol : 0;
        h.data_toggle = 0; /* Starts at DATA0 */

        /* bInterfaceProtocol on a HID boot interface: 1 = keyboard,
         * 2 = mouse. Protocol 0 means the report layout is described only
         * by the report descriptor, so accept either shape at decode time
         * rather than guessing here. */
        if (h.is_keyboard) {
          g_status.keyboard_detected = true;
          g_status.keyboard_addr = dev_addr;
        }
        if (h.is_mouse) {
          g_status.mouse_detected = true;
          g_status.mouse_addr = dev_addr;
        }
        ++claimed;
        kprintf("[usb]   hid ep %x if %u proto %u mps %u -> %s (%s)\n",
                static_cast<uint64_t>(ep->bEndpointAddress),
                static_cast<uint64_t>(cur_if_num),
                static_cast<uint64_t>(cur_if_protocol),
                static_cast<uint64_t>(h.max_pkt),
                h.is_keyboard && h.is_mouse ? "keyboard+mouse"
                                            : (h.is_keyboard ? "keyboard" : "mouse"),
                h.boot ? "boot protocol" : "report descriptor");
      }
    }
    offset += len;
  }

  if (claimed == 0) {
    kputs("[usb]   no HID interrupt IN endpoints on this device\n");
  }
  return claimed > 0;
}

/* Probe & configure a single downstream port of the internal hub */
bool probe_and_configure_hub_port(uint16_t port) {
  /* If already configured on this port, skip redundant re-enumeration */
  if (g_port_configured[port] > 0) return true;

  uint32_t port_status = 0;
  if (!dwc2_control_msg(1, UsbSpeed::High, 0xA3, 0x00 /* GET_STATUS */, 0, port, &port_status, 4)) {
    return false;
  }

  /* If no device connected */
  if ((port_status & 1) == 0) {
    return false;
  }

  if (g_enum_verbose) {
    kprintf("[usb] port %u: device present (status 0x%04x)\n",
            static_cast<uint64_t>(port), static_cast<uint64_t>(port_status & 0xFFFFu));
  }

  /* Reset the port to put the device into Default State */
  dwc2_control_msg(1, UsbSpeed::High, 0x23, 0x03 /* SET_FEATURE */, 4 /* PORT_RESET */, port, nullptr, 0);
  metal_delay_us(50000);
  dwc2_control_msg(1, UsbSpeed::High, 0x23, 0x01 /* CLEAR_FEATURE */, 20 /* C_PORT_RESET */, port, nullptr, 0);
  dwc2_control_msg(1, UsbSpeed::High, 0x23, 0x01 /* CLEAR_FEATURE */, 16 /* C_PORT_CONNECTION */, port, nullptr, 0);
  metal_delay_us(10000);

  /* Speed is only valid AFTER the reset completes */
  UsbSpeed dev_speed = UsbSpeed::Full;
  dwc2_control_msg(1, UsbSpeed::High, 0xA3, 0x00, 0, port, &port_status, 4);
  if (port_status & (1 << 9)) dev_speed = UsbSpeed::Low;
  else if (port_status & (1 << 10)) dev_speed = UsbSpeed::High;
  /* wPortStatus bit 1 is PORT_ENABLE. The hub sets it when the reset
   * completes; if it is clear the device is not on the bus yet and every
   * transaction to it will fail for reasons that have nothing to do with
   * the host controller. */
  bool port_enabled = (port_status & (1u << 1)) != 0;
  if (g_enum_verbose) {
    kprintf("[usb] port %u: reset done, status 0x%04x, %s, speed %s\n",
            static_cast<uint64_t>(port), static_cast<uint64_t>(port_status & 0xFFFFu),
            port_enabled ? "ENABLED" : "NOT ENABLED",
            dev_speed == UsbSpeed::High ? "HIGH" : (dev_speed == UsbSpeed::Full ? "FULL" : "LOW"));
  }
  if (!port_enabled) {
    /* Give the hub a little longer, then re-read once before giving up. */
    metal_delay_us(50000);
    dwc2_control_msg(1, UsbSpeed::High, 0xA3, 0x00, 0, port, &port_status, 4);
    port_enabled = (port_status & (1u << 1)) != 0;
    if (!port_enabled && g_enum_verbose) {
      kprintf("[usb] port %u: hub never enabled the port (0x%04x)\n",
              static_cast<uint64_t>(port), static_cast<uint64_t>(port_status & 0xFFFFu));
    }
  }

  /* Assign a unique device address. USB device addresses are a 7-bit field
   * (1-127); this hub is re-probed every 100ms and a run of enumeration
   * failures increments g_next_addr every time with nothing ever freeing
   * one, so an unbounded counter reaches 128+ within seconds of failures
   * and starts writing out-of-range values into HCCHAR. Wrap it. */
  if (g_next_addr > 120) g_next_addr = 2;
  uint8_t dev_addr = g_next_addr++;
  if (!dwc2_control_msg(0, dev_speed, 0x00, 0x05 /* SET_ADDRESS */, dev_addr, 0,
                        nullptr, 0, 8, 1, port)) {
    kprintf("[usb] port %u: SET_ADDRESS failed\n", static_cast<uint64_t>(port));
    return false;
  }
  metal_delay_us(20000);

  if (!configure_device(dev_addr, dev_speed, 1 /* hub addr */, static_cast<uint8_t>(port))) {
    return false;
  }

  /* Only record the port as configured once something actually claimed the
   * device. Marking it unconditionally made a device that failed to
   * enumerate invisible forever; not marking it at all made the 100 ms
   * rescan re-address a working device over and over. */
  g_port_configured[port] = dev_addr;
  return true;
}

}  /* namespace */

bool usb_hid_init() {
  if (!board_has_bcm_peripherals()) return false;   /* DWC2 lives in the BCM peripheral window only */
  /* Idempotent. desktop_init() also calls this, and re-running it meant a
   * full DWC2 core soft reset, a root-port power cycle and a complete
   * re-enumeration happening a second time moments after boot had already
   * succeeded - throwing away every endpoint that had just been found and
   * forcing the internal hub and everything behind it back through
   * addressing while the devices were no longer in Default state. */
  if (g_status.initialized) return true;

  g_status = {};
  g_num_endpoints = 0;
  g_next_addr = 2;
  for (int i = 0; i < 6; ++i) g_port_configured[i] = 0;

  if (!dwc2_host_init()) {
    return false;
  }
  const Dwc2HostInfo& host = dwc2_host_info();
  if (!host.port_connected || !host.port_enabled) {
    kputs("[usb] root port has no enabled device\n");
    return false;
  }

  /* The device on the root port is in Default State at address 0. Its EP0
   * max packet size is not known yet, and the only length the spec
   * guarantees it will answer in that state is 8 bytes. */
  UsbSpeed root_speed = host.speed;
  UsbDeviceDescriptor dev_desc = {};
  if (!dwc2_control_msg(0, root_speed, 0x80, 0x06 /* GET_DESCRIPTOR */, 0x0100, 0,
                        &dev_desc, 8, 8)) {
    kputs("[usb] root device did not answer GET_DESCRIPTOR\n");
    return false;
  }
  uint32_t root_ep0 = dev_desc.bMaxPacketSize0;
  if (root_ep0 != 8 && root_ep0 != 16 && root_ep0 != 32 && root_ep0 != 64) root_ep0 = 8;

  /* Address 1 */
  if (!dwc2_control_msg(0, root_speed, 0x00, 0x05 /* SET_ADDRESS */, 1, 0, nullptr, 0, 8)) {
    kputs("[usb] root device SET_ADDRESS failed\n");
    return false;
  }
  metal_delay_us(20000);

  if (!dwc2_control_msg(1, root_speed, 0x80, 0x06, 0x0100, 0, &dev_desc, sizeof(dev_desc), root_ep0)) {
    kputs("[usb] root device descriptor read failed after addressing\n");
    return false;
  }
  kprintf("[usb] root device: %x:%x class %x ep0 %u bytes\n",
          static_cast<uint64_t>(dev_desc.idVendor), static_cast<uint64_t>(dev_desc.idProduct),
          static_cast<uint64_t>(dev_desc.bDeviceClass), static_cast<uint64_t>(root_ep0));

  if (dev_desc.bDeviceClass == 0x09) {
    /* Hub - on a Pi 3 this is the soldered-on LAN9514, which carries the
     * Ethernet MAC on downstream port 1 and the four external jacks on
     * ports 2-5. */
    g_status.hub_detected = true;

    if (!dwc2_control_msg(1, root_speed, 0x00, 0x09 /* SET_CONFIGURATION */, 1, 0, nullptr, 0, root_ep0)) {
      kputs("[usb] hub SET_CONFIGURATION failed\n");
    }
    metal_delay_us(20000);

    /* How many downstream ports the hub really has, from its own hub
     * descriptor (class descriptor type 0x29), rather than assuming 5. */
    uint8_t hub_desc[8] = {};
    uint8_t num_ports = 5;
    if (dwc2_control_msg(1, root_speed, 0xA0, 0x06 /* GET_DESCRIPTOR */, 0x2900, 0,
                         hub_desc, sizeof(hub_desc), root_ep0) &&
        hub_desc[2] >= 1 && hub_desc[2] <= 5) {
      num_ports = hub_desc[2];
    }
    kprintf("[usb] hub: %u downstream ports\n", static_cast<uint64_t>(num_ports));
    g_hub_ports = num_ports;

    for (uint16_t port = 1; port <= num_ports; ++port) {
      dwc2_control_msg(1, root_speed, 0x23, 0x03 /* SET_FEATURE */, 8 /* PORT_POWER */, port, nullptr, 0);
    }
    /* bPwrOn2PwrGood is in 2 ms units; be generous, this only runs once. */
    metal_delay_us((hub_desc[5] ? hub_desc[5] : 50u) * 2000u + 100000u);

    g_enum_verbose = true;
    for (uint16_t port = 1; port <= num_ports; ++port) {
      if (!probe_and_configure_hub_port(port)) {
        kprintf("[usb] port %u: nothing claimed\n", static_cast<uint64_t>(port));
      }
    }
    g_enum_verbose = false;
  } else {
    /* A device wired straight to the root port. */
    configure_device(1, root_speed, 0 /* no hub */, 0);
  }

  g_status.initialized = true;
  return true;
}

namespace {
int32_t g_trace_budget = 0;
uint32_t g_trace_reports = 0;
void trace_open() {
  if (g_trace_reports < 6) { ++g_trace_reports; g_trace_budget = 80; }
}
}  /* namespace */

/* Notes from inside a USB transfer are only recorded: queueing or sending a
 * frame there would perturb the transaction being traced. */
char g_notes[16][20];
unsigned g_note_count = 0;
void usb_hid_trace_note(const char* step) {
  if (g_trace_budget <= 0 || g_note_count >= 16) return;
  size_t n = 0;
  while (*step && n + 1 < sizeof(g_notes[0])) { g_notes[g_note_count][n] = *step++; ++n; }
  g_notes[g_note_count][n] = 0;
  ++g_note_count;
}

void usb_hid_trace(const char* step) {
  if (g_trace_budget <= 0) return;
  --g_trace_budget;
  char line[200] = "trace ";
  size_t n = 6;
  for (unsigned k = 0; k < g_note_count; ++k) {
    const char* t = g_notes[k];
    while (*t && n + 2 < sizeof(line)) line[n++] = *t++;
    if (n + 2 < sizeof(line)) line[n++] = ' ';
  }
  g_note_count = 0;
  if (n + 2 < sizeof(line)) { line[n++] = '|'; line[n++] = ' '; }
  while (*step && n + 1 < sizeof(line)) line[n++] = *step++;
  line[n] = 0;
  net_breadcrumb(line);
  /* Sent now, not at the next net_poll(): the step before a hang is the
   * one that matters, and it would otherwise die in the TX ring. */
  net_breadcrumb_flush();
}

void usb_hid_inject_mouse(int32_t dx, int32_t dy, uint8_t btns) {
  apply_mouse(btns, dx, dy);
}

void usb_hid_poll() {
  if (!g_status.initialized) return;

  uint64_t now = metal_now_ns();
  /* Periodic Hub Downstream Port Scanning (Hotplug Support every 500 ms) */
  const bool due_for_scan = (now - g_last_hub_poll_ns >= 500000000ull);
  if (g_status.hub_detected && (due_for_scan || g_force_verbose_scan)) {
    g_last_hub_poll_ns = now;
    const bool was_verbose = g_enum_verbose;
    if (g_force_verbose_scan) g_enum_verbose = true;
    for (uint16_t port = 1; port <= g_hub_ports; ++port) {
      if (g_port_configured[port] == 0) {
        probe_and_configure_hub_port(port);
      }
    }
    g_enum_verbose = was_verbose;
    g_force_verbose_scan = false;
  }

  /* Poll each registered HID Interrupt IN endpoint when it is due. */
  for (size_t i = 0; i < g_num_endpoints; ++i) {
    auto& ep = g_hid_endpoints[i];
    if (ep.last_poll_ns != 0 && now - ep.last_poll_ns < static_cast<uint64_t>(ep.interval_us) * 1000ull) continue;
    ep.last_poll_ns = now;
    uint8_t buf[64] = {};
    uint8_t next_pid = ep.data_toggle;
    {
      char s[24] = "ep0 poll t0";
      s[2] = static_cast<char>('0' + i);
      s[10] = static_cast<char>('0' + (ep.data_toggle & 3));
      usb_hid_trace(s);
    }
    int32_t bytes = dwc2_interrupt_in(ep.dev_addr, ep.speed, ep.ep_addr, buf,
                                      ep.max_pkt > 0 ? ep.max_pkt : 8,
                                      ep.max_pkt > 0 ? ep.max_pkt : 8,
                                      ep.hub_addr, ep.hub_port,
                                      ep.data_toggle,
                                      &next_pid);
    ep.data_toggle = next_pid;

    {
      char s[24] = "ep0 rc=x t0";
      s[2] = static_cast<char>('0' + i);
      s[7] = bytes < 0 ? '-' : static_cast<char>('0' + (bytes > 9 ? 9 : bytes));
      s[10] = static_cast<char>('0' + (ep.data_toggle & 3));
      usb_hid_trace(s);
    }
    if (bytes > 0) {
      ++ep_reports[i];
      {
        /* The last few raw reports, kept for diagnosis (usb report). They
         * are no longer broadcast one datagram per report - a moving mouse
         * is ~125 a second, each a USB transfer to the Ethernet chip on the
         * same bus as the mouse - and a burst of step traces no longer
         * opens on the first reports of every boot. */
        RawReport& rr = g_recent[g_recent_head % 6];
        rr.ep = static_cast<uint8_t>(i);
        rr.len = static_cast<uint8_t>(bytes);
        for (int k = 0; k < 8; ++k) rr.b[k] = k < bytes ? buf[k] : 0;
        ++g_recent_head;
      }
      if (ep.boot) {
        if (ep.is_keyboard && bytes >= 8) decode_keyboard_report(buf);
        else if (ep.is_mouse && bytes >= 3) decode_mouse_report(buf, static_cast<size_t>(bytes));
      } else {
        decode_by_layout(ep, buf, static_cast<size_t>(bytes));
      }
      usb_hid_trace("decoded");
    }
  }
  usb_hid_trace("poll done");
}

int usb_get_key() {
  if (g_key_head == g_key_tail) return -1;
  char c = g_key_ring[g_key_tail];
  g_key_tail = (g_key_tail + 1) % KEY_BUF_SIZE;
  return static_cast<unsigned char>(c);
}

void usb_set_mouse_pos(int32_t x, int32_t y) { g_mouse_px = x; g_mouse_py = y; }

int32_t usb_take_mouse_wheel() {
  const int32_t w = g_mouse_wheel;
  g_mouse_wheel = 0;
  return w;
}

bool usb_get_mouse_state(int32_t& out_x, int32_t& out_y, uint8_t& out_buttons) {
  out_x = g_mouse_px;
  out_y = g_mouse_py;
  out_buttons = g_mouse_btns;
  return g_status.mouse_detected;
}

uint8_t usb_hub_port_count() {
  return g_hub_ports;
}

bool usb_has_keyboard() {
  return g_status.keyboard_detected;
}

bool usb_has_mouse() {
  return g_status.mouse_detected;
}

const UsbHidStatus& usb_hid_status() {
  return g_status;
}

bool usb_hid_reinit_ethernet() {
  /* This used to bail out and do NOTHING if ethernet_detected was already
   * false - which is exactly the state a LAN9514 that never enumerated in
   * the first place sits in forever. That made "net up" a no-op for the
   * single case it was written to fix: a port stuck failing since boot,
   * silently, with g_enum_verbose long since false. Now it always clears
   * whatever port state exists and always forces one narrated rescan,
   * whether or not eth0 was ever seen before. */
  for (int port = 0; port < 6; ++port) {
    if (g_status.ethernet_detected && g_port_configured[port] == g_status.ethernet_addr) {
      g_port_configured[port] = 0;
    }
  }
  g_status.ethernet_detected = false;
  g_last_eth_init_ns = 0;       /* do not wait out the lan9514_init() backoff */
  g_force_verbose_scan = true;  /* do not wait out the 100ms->500ms rescan either */
  kputs("[usb] forcing a fully-narrated eth0 re-probe...\n");
  return true;
}

void usb_hid_report() {
  kputs("  usb hid & hub subsystem:\n");
  kprintf("    subsystem   : %s\n", g_status.initialized ? "initialized & listening" : "offline");
  kprintf("    hub state   : %s\n", g_status.hub_detected ? "hub enumerated on the root port"
                                                             : "no hub enumerated (root port device, or nothing attached)");
  kprintf("    endpoints   : %u active HID endpoints\n", static_cast<uint64_t>(g_num_endpoints));
  for (size_t i = 0; i < g_num_endpoints; ++i) {
    const auto& e = g_hid_endpoints[i];
    kprintf("      [%u] dev %u if %u ep %x: %s%s%s via %s, %u reports",
            static_cast<uint64_t>(i), static_cast<uint64_t>(e.dev_addr), static_cast<uint64_t>(e.interface_num),
            static_cast<uint64_t>(e.ep_addr), e.is_keyboard ? "keyboard" : "",
            (e.is_keyboard && e.is_mouse) ? "+" : "", e.is_mouse ? "mouse" : "",
            e.boot ? "boot protocol" : "report descriptor", static_cast<uint64_t>(ep_reports[i]));
    if (!e.boot && e.is_mouse) {
      kprintf(" (id %u btn@%u x@%u/%u y@%u/%u)", static_cast<uint64_t>(e.m_id), static_cast<uint64_t>(e.m_btn_bit),
              static_cast<uint64_t>(e.m_x_bit), static_cast<uint64_t>(e.m_x_size),
              static_cast<uint64_t>(e.m_y_bit), static_cast<uint64_t>(e.m_y_size));
    }
    kputs("\n");
  }
  kprintf("    keyboard    : %s (events: %u)\n",
          g_status.keyboard_detected ? "connected & active" : "not detected",
          static_cast<uint64_t>(g_status.key_events));
  kprintf("    mouse       : %s at (%d, %d) (events: %u)\n",
          g_status.mouse_detected ? "connected & active" : "not detected",
          static_cast<uint64_t>(g_mouse_px),
          static_cast<uint64_t>(g_mouse_py),
          static_cast<uint64_t>(g_status.mouse_events));
}

/* "ep2:8:01fe0300 ep1:3:..." - the most recent raw HID reports, newest last. */
size_t usb_hid_recent_reports(char* out, size_t cap) {
  size_t n = 0;
  auto put = [&](char c) { if (n + 1 < cap) out[n++] = c; };
  const char* hx = "0123456789abcdef";
  const unsigned count = g_recent_head < 6 ? g_recent_head : 6;
  for (unsigned k = 0; k < count; ++k) {
    const RawReport& rr = g_recent[(g_recent_head - count + k) % 6];
    put('e'); put('p'); put(static_cast<char>('0' + rr.ep)); put(':');
    put(static_cast<char>('0' + (rr.len % 10))); put(':');
    for (int b = 0; b < rr.len && b < 8; ++b) { put(hx[rr.b[b] >> 4]); put(hx[rr.b[b] & 15]); }
    put(' ');
  }
  if (cap) out[n] = 0;
  return n;
}
