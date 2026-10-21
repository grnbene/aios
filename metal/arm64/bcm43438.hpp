#pragma once
/* Broadcom / Cypress BCM43438 WiFi Chip Initialization & Firmware Loader (metal/arm64/bcm43438.hpp).
 * Manages the backplane window over SDIO Function 1, uploads embedded firmware and NVRAM blobs,
 * resets and brings up the ARM Cortex-M3 WLAN core, and activates Function 2 data transfer.
 */
#include <cstddef>
#include <cstdint>

struct Bcm43438Info {
  bool initialized;
  uint16_t chip_id;
  uint8_t chip_rev;
  uint32_t ram_size;
  uint32_t firmware_size;
  uint32_t nvram_size;
  bool f2_ready;
};

/* Embedded blob symbols (defined in bcm43438_blobs.S) */
extern "C" {
extern const uint8_t bcm43438_firmware_start[];
extern const uint8_t bcm43438_firmware_end[];
extern const char bcm43438_nvram_start[];
extern const uint8_t bcm43438_clm_start[];
extern const uint8_t bcm43438_clm_end[];
extern const char bcm43438_nvram_end[];
}

/* BCM43438 Backplane Accessors */
void bcm_set_backplane_window(uint32_t addr);
uint8_t bcm_read8(uint32_t addr);
void bcm_write8(uint32_t addr, uint8_t val);
uint32_t bcm_read32(uint32_t addr);
void bcm_write32(uint32_t addr, uint32_t val);
bool bcm_read_backplane(uint32_t addr, void* dst, size_t len);
bool bcm_write_backplane(uint32_t addr, const void* src, size_t len);

/* BCM43438 Chip Bringup & Sequence */
bool bcm43438_init();
/* Set before bcm43438_init() to run the write-mechanism probe (console: wifi probe). */
extern bool g_probe_writes;
const Bcm43438Info& bcm43438_info();
/* Backplane address of the chip's SDIO device core registers (intstatus at
 * +0x20); 0 before bring-up. */
uint32_t bcm43438_sdio_core_regs();
void bcm43438_report();
