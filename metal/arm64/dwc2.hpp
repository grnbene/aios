#pragma once
/* Synopsys DWC2 USB 2.0 Host Controller Driver (metal/arm64/dwc2.hpp).
 *
 * Implements hardware registers, host channel allocation, root port management,
 * and low-level USB transfer protocols (Control, Interrupt, Bulk) for Raspberry Pi 3
 * (BCM2837 @ 0x3F980000) and Raspberry Pi 4 (BCM2711 @ 0xFE980000).
 */
#include <cstddef>
#include <cstdint>

#pragma pack(push, 1)

struct UsbDeviceRequest {
  uint8_t  bmRequestType;
  uint8_t  bRequest;
  uint16_t wValue;
  uint16_t wIndex;
  uint16_t wLength;
};

struct UsbDeviceDescriptor {
  uint8_t  bLength;
  uint8_t  bDescriptorType;    /* 0x01 */
  uint16_t bcdUSB;
  uint8_t  bDeviceClass;
  uint8_t  bDeviceSubClass;
  uint8_t  bDeviceProtocol;
  uint8_t  bMaxPacketSize0;
  uint16_t idVendor;
  uint16_t idProduct;
  uint16_t bcdDevice;
  uint8_t  iManufacturer;
  uint8_t  iProduct;
  uint8_t  iSerialNumber;
  uint8_t  bNumConfigurations;
};

struct UsbConfigDescriptor {
  uint8_t  bLength;
  uint8_t  bDescriptorType;    /* 0x02 */
  uint16_t wTotalLength;
  uint8_t  bNumInterfaces;
  uint8_t  bConfigurationValue;
  uint8_t  iConfiguration;
  uint8_t  bmAttributes;
  uint8_t  bMaxPower;
};

struct UsbInterfaceDescriptor {
  uint8_t bLength;
  uint8_t bDescriptorType;     /* 0x04 */
  uint8_t bInterfaceNumber;
  uint8_t bAlternateSetting;
  uint8_t bNumEndpoints;
  uint8_t bInterfaceClass;
  uint8_t bInterfaceSubClass;
  uint8_t bInterfaceProtocol;
  uint8_t iInterface;
};

struct UsbEndpointDescriptor {
  uint8_t  bLength;
  uint8_t  bDescriptorType;    /* 0x05 */
  uint8_t  bEndpointAddress;
  uint8_t  bmAttributes;
  uint16_t wMaxPacketSize;
  uint8_t  bInterval;
};

#pragma pack(pop)

enum class UsbSpeed : uint8_t {
  High = 0,  /* 480 Mbps */
  Full = 1,  /* 12 Mbps */
  Low  = 2   /* 1.5 Mbps */
};

struct Dwc2HostInfo {
  bool initialized;
  bool port_connected;
  bool port_enabled;
  UsbSpeed speed;
  uint32_t num_channels;
  uint64_t total_transfers;
  uint64_t failed_transfers;
  /* Why transfers failed. "N failed" on its own cannot distinguish a device
   * that is not answering from a protocol bug, and this kernel's only other
   * diagnostic channel is a UART most people have no cable for. */
  uint32_t fail_stall;
  uint32_t fail_xact;
  uint32_t fail_timeout;
  uint32_t fail_nak;
  uint32_t fail_direct;
  uint32_t fail_split;
  /* Raw HCINT of the most recent split phase, and which phase it was
   * (1 = start-split, 2 = complete-split). The bit pattern names the exact
   * hardware response when a split goes wrong. */
  /* Interrupt-IN poll outcomes. A keyboard that enumerated but never
   * delivers a report is a completely different problem from one that was
   * never found, and the two are indistinguishable without this. */
  uint32_t int_ok;
  uint32_t int_fail;
  uint32_t last_split_hcint;
  uint32_t last_split_stage;
};

/* Initialize DWC2 host controller hardware and power up root hub port */
bool dwc2_host_init();

/* Returns true if DWC2 hardware is initialized and ready */
bool dwc2_is_ready();

/* Execute USB Control Transfer */
bool dwc2_control_msg(uint8_t dev_addr, UsbSpeed speed,
                      uint8_t req_type, uint8_t request,
                      uint16_t value, uint16_t index,
                      void* data, uint16_t length,
                      uint32_t max_packet_size = 64,
                      uint8_t hub_addr = 0, uint8_t hub_port = 0);

/* Execute USB Interrupt IN Transfer (for Keyboards & Mice) */
int32_t dwc2_interrupt_in(uint8_t dev_addr, UsbSpeed speed,
                          uint8_t ep_addr, void* buf,
                          uint32_t len, uint32_t max_packet_size = 8,
                          uint8_t hub_addr = 0, uint8_t hub_port = 0,
                          uint8_t pid_override = 0,
                          uint8_t* next_pid = nullptr);

/* Execute USB Bulk IN Transfer (for Ethernet & Mass Storage).
 *
 * toggle is the caller's persistent DATA0/DATA1 state for this endpoint:
 * read as the PID to use, written back with whatever the hardware actually
 * ended up at. USB 2.0 data toggle synchronization (S8.6) requires this to
 * alternate every successful transfer - a caller that always passes 0
 * hardcodes DATA0 forever, which is a protocol violation the device is
 * required to react to: everything after the first successful transfer
 * looks like a retransmitted duplicate of that first one and gets silently
 * discarded rather than processed as new data. */
int32_t dwc2_bulk_in(uint8_t dev_addr, UsbSpeed speed,
                     uint8_t ep_addr, void* buf,
                     uint32_t len, uint32_t max_packet_size,
                     uint8_t hub_addr, uint8_t hub_port,
                     uint8_t& toggle);

/* Execute USB Bulk OUT Transfer (for Ethernet & Mass Storage). See
 * dwc2_bulk_in()'s toggle parameter - same requirement, same reason. */
int32_t dwc2_bulk_out(uint8_t dev_addr, UsbSpeed speed,
                      uint8_t ep_addr, const void* buf,
                      uint32_t len, uint32_t max_packet_size,
                      uint8_t hub_addr, uint8_t hub_port,
                      uint8_t& toggle);

/* Get controller telemetry */
const Dwc2HostInfo& dwc2_host_info();

/* Shell report */
void dwc2_report();
