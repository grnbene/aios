#pragma once
/* USB HID (Human Interface Device) Keyboard, Mouse & Hub Subsystem (metal/arm64/usb_hid.hpp).
 *
 * Implements USB standard enumeration, Hub port powering, HID Boot Protocol Keyboard
 * (`0x03/0x01/0x01`), and HID Boot Protocol Mouse (`0x03/0x01/0x02`) packet decoding
 * for physical USB keyboards and mice connected to the Raspberry Pi USB ports.
 */
#include <cstddef>
#include <cstdint>

/* Function keys F1-F5 come out of usb_get_key() as these codes. They must be
 * distinct from the ASCII digits '1'-'5': flattening F1 to '1' meant that on
 * any tab that accepts typing (the Terminal) the advertised [F1-F5] Tabs
 * shortcut silently typed a digit instead, with no way back out. */
constexpr int USB_KEY_F1 = 0x91;
constexpr int USB_KEY_F5 = 0x95;
constexpr int USB_KEY_F6 = 0x96;

struct UsbHidStatus {
  bool initialized;
  bool hub_detected;
  bool keyboard_detected;
  bool mouse_detected;
  bool ethernet_detected;
  uint8_t keyboard_addr;
  uint8_t mouse_addr;
  uint8_t ethernet_addr;
  uint64_t key_events;
  uint64_t mouse_events;
};

/* Force the eth0 device off its hub port so the next usb_hid_poll() rescan
 * re-probes and fully reconfigures it from scratch - a real hardware retry
 * of the LAN9514/PHY bring-up, not just re-reading software state. Returns
 * false if no Ethernet device is currently tracked at all. */
bool usb_hid_reinit_ethernet();

/* Initialize USB subsystem, enumerate devices and hubs */
bool usb_hid_init();

/* Service USB HID input queues (called continuously in frame loop) */
void usb_hid_poll();

/* Non-blocking fetch of next ASCII key from physical USB keyboard (-1 if empty) */
int usb_get_key();

/* Fetch current mouse state (coordinates clamped to framebuffer, button bitmask) */
bool usb_get_mouse_state(int32_t& out_x, int32_t& out_y, uint8_t& out_buttons);
/* Wheel notches since the last call (+ = scrolled up / away from the user). */
int32_t usb_take_mouse_wheel();
void usb_set_mouse_pos(int32_t x, int32_t y);

/* Returns true if a physical USB keyboard is connected */
/* Downstream port count reported by the internal hub's own descriptor */
uint8_t usb_hub_port_count();

bool usb_has_keyboard();

/* Returns true if a physical USB mouse is connected */
bool usb_has_mouse();

/* Status and diagnostics */
const UsbHidStatus& usb_hid_status();
void usb_hid_report();

/* Most recent raw HID reports as text, for crash breadcrumbs. */
size_t usb_hid_recent_reports(char* out, size_t cap);

/* Mouse-hang tracing: the first few HID reports after boot open a budget of
 * step-by-step breadcrumbs (udp/42425) through the poll and the dashboard
 * loop, so the last step received names where the board stopped. */
void usb_hid_trace(const char* step);
void usb_hid_trace_note(const char* step);
/* Push a synthetic mouse move through the same apply path a USB report
 * takes (console `mouse DX DY [BTN]`), to split USB from what follows. */
void usb_hid_inject_mouse(int32_t dx, int32_t dy, uint8_t btns);
