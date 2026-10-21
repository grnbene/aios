#pragma once
/* MicroSD Card Block Storage Driver (metal/arm64/sdcard.hpp).
 *
 * Implements low-level SD card protocol (CMD0, CMD8, ACMD41, CMD2, CMD3, CMD7, CMD16, CMD17, CMD24)
 * over BCM2837 Arasan EMMC / SDHOST hardware controller for raw 512-byte sector block access.
 */
#include <cstddef>
#include <cstdint>

struct SdCardStatus {
  bool initialized;
  bool is_sdhc;
  uint16_t rca;
  uint32_t capacity_sectors;
  uint64_t capacity_bytes;
  uint64_t reads_completed;
  uint64_t writes_completed;
  uint64_t read_errors;
};

/* Initialize SD card interface and power up card */
bool sdcard_init();

/* Read a single 512-byte block from sector LBA */
bool sdcard_read_block(uint32_t lba, void* buffer);

/* Read multiple contiguous 512-byte blocks */
bool sdcard_read_blocks(uint32_t lba, void* buffer, uint32_t count);

/* Write a single 512-byte block to sector LBA */
bool sdcard_write_block(uint32_t lba, const void* buffer);

/* Write multiple contiguous 512-byte blocks */
bool sdcard_write_blocks(uint32_t lba, const void* buffer, uint32_t count);

/* Status and diagnostics */
const SdCardStatus& sdcard_status();
void sdcard_report();
