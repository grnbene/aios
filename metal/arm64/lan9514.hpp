#pragma once
/* SMSC LAN9514 USB 2.0 to 10/100 Fast Ethernet Driver (metal/arm64/lan9514.hpp).
 *
 * Implements hardware register control, MAC/PHY initialization, and high-speed
 * packet transmission/reception for Raspberry Pi 3 onboard Ethernet (VID 0x0424, PID 0xEC00).
 */
#include <cstddef>
#include <cstdint>
#include "dwc2.hpp"

struct Lan9514Stats {
  uint64_t tx_packets;
  uint64_t rx_packets;
  uint64_t tx_bytes;
  uint64_t rx_bytes;
  uint64_t tx_errors;
  uint64_t rx_errors;
};

struct Lan9514Info {
  bool initialized;
  bool link_up;
  uint8_t dev_addr;
  UsbSpeed speed;
  uint8_t hub_addr;
  uint8_t hub_port;
  uint8_t mac[6];
  uint32_t chip_id;
  /* Link mode as the PHY reports it (0 = unknown): */
  uint16_t link_mbps;
  bool full_duplex;
  bool autoneg;
  Lan9514Stats stats;
};

/* Subsystem Lifecycle */
bool lan9514_init(uint8_t dev_addr, UsbSpeed speed, uint8_t hub_addr, uint8_t hub_port, const uint8_t mac_override[6] = nullptr);
bool lan9514_is_up();
bool lan9514_check_link();
const Lan9514Info& lan9514_info();

/* Packet Transfer */
bool lan9514_tx_frame(const void* frame, size_t len);
bool lan9514_rx_poll(void* out_frame, size_t max_len, size_t& out_len);

/* Telemetry & Shell */
void lan9514_report();
void lan9514_burst_test(const uint8_t* frame, size_t len, unsigned n);
