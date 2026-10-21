#pragma once
/* Raspberry Pi VideoCore Mailbox Property Interface (metal/arm64/rpi_mailbox.hpp).
 *
 * Supports BCM2837 (Pi 3, MMIO 0x3F00B880) and BCM2711 (Pi 4, MMIO 0xFE00B880).
 */
#include <cstddef>
#include <cstdint>

struct RpiFbInfo {
  uint32_t* pixels = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t pitch = 0;
  uint32_t bpp = 32;
  uint64_t bus_addr = 0;
  uint64_t arm_addr = 0;
  uint64_t fb_size = 0;
  /* What the firmware GRANTED for 0x00048006 (0 = "BGR", 1 = "RGB"). */
  uint32_t pixel_order = 0;
};

bool rpi_mailbox_available();
bool rpi_mailbox_call(uint32_t channel, volatile uint32_t* msg_buffer);
bool rpi_fb_init(uint32_t width, uint32_t height, uint32_t bpp, RpiFbInfo& out);
bool rpi_get_mac_address(uint8_t mac[6]);
/* Firmware board revision code (e.g. 0xa02082 = Pi 3 Model B rev 1.2, 1 GB). */
bool rpi_get_board_revision(uint32_t& out_rev);
/* ARM (0x00010005) or VideoCore (0x00010006) memory: base and size in bytes. */
bool rpi_get_memory_split(uint32_t tag, uint32_t& base, uint32_t& size);
/* The mode the firmware drove the display at (the monitor's native size when
 * it read EDID), queried before we allocate a framebuffer. */
bool rpi_get_display_size(uint32_t& w, uint32_t& h);
bool rpi_power_set(uint32_t dev_id, bool on);

/* VideoCore GPIO expander pins. These are NOT SoC GPIOs and cannot be
 * reached through GPFSEL/GPSET directly - only the VideoCore firmware can drive them,
 * via SET_GPIO_CONFIG (0x00038043) / SET_GPIO_STATE (0x00038041), with the pin numbered from 128.
 * On Raspberry Pi 3:
 *   - Pin 0: LAN_RUN (SMSC LAN9514 USB/Ethernet controller hardware reset, active high)
 *   - Pin 1: WL_REG_ON (BCM43438 / BCM43455 Wi-Fi power regulator enable, active high)
 *   - Pin 2: BT_REG_ON (BCM43438 Bluetooth power regulator enable, active high)
 */
/* Pi 3B expander lines, from bcm2837-rpi-3-b.dts gpio-line-names:
 * 0 BT_ON, 1 WL_ON, 2 PWR_LED_OFF, 3 GLOBAL_RESET, 4 VDD_SD_IO_SEL,
 * 5 CAM_GPIO, 6 SD_PWR_ON. There is no LAN_RUN on this expander. */
constexpr uint32_t RPI_EXPGPIO_BT_ON       = 0;
constexpr uint32_t RPI_EXPGPIO_WL_ON       = 1;
constexpr uint32_t RPI_EXPGPIO_PWR_LED_OFF = 2;
constexpr uint32_t RPI_EXPGPIO_SD_PWR_ON   = 6;

/* Firmware clock manager. RPI_CLOCK_EMMC is the Arasan SDHCI's base clock,
 * which every SD clock divisor divides from; it is a board/firmware fact,
 * not a constant. */
constexpr uint32_t RPI_CLOCK_EMMC = 1;
constexpr uint32_t RPI_CLOCK_ARM  = 3;
constexpr uint32_t RPI_CLOCK_CORE = 4;
bool rpi_get_clock_rate(uint32_t clock_id, uint32_t& out_hz);
bool rpi_get_max_clock_rate(uint32_t clock_id, uint32_t& out_hz);
/* SoC temperature in millidegrees C, and the firmware's throttle word
 * (bit 0 under-voltage now, 1 ARM frequency capped now, 2 throttled now,
 * 3 soft temperature limit; bits 16-19 the same, "has happened"). */
bool rpi_get_temperature(uint32_t& out_millic);
bool rpi_get_throttled(uint32_t& out_bits);
bool rpi_set_clock_rate(uint32_t clock_id, uint32_t hz);
bool rpi_set_expander_gpio(uint32_t pin, bool on);
/* Ask the firmware what it thinks the pin's level actually is right now -
 * proof the write landed, not just that the mailbox call returned success
 * (which only means the message was well-formed, not that the expander
 * chip actually toggled). */
bool rpi_get_expander_gpio(uint32_t pin, bool& out_level);

