#pragma once
/* Bare-metal SDIO Host Controller Interface & Protocol Engine (metal/arm64/sdio.hpp).
 * Supports BCM2837 (Pi 3) and BCM2711 (Pi 4) Arasan SDHCI / EMMC controllers.
 * Implements CMD52 (byte I/O), CMD53 (block/FIFO multi-byte I/O), SDIO function
 * enumeration (F0, F1, F2), and interrupt management.
 */
#include <cstddef>
#include <cstdint>

struct SdioCmdResp {
  uint32_t resp[4];
  uint32_t status;
  bool ok;
};

struct SdioCardInfo {
  bool present;
  uint16_t rca;
  uint32_t ocr;
  uint8_t num_functions;
  bool memory_present;
  uint8_t cccr_version;
  uint8_t sdio_version;
  uint8_t sd_version;
  uint16_t f1_block_size;
  uint16_t f2_block_size;
};

/* Host controller initialization and status */
/* init_clk_divisor: the SD clock divisor used during identification, before
 * any card has been found (default 0x80). A caller retrying a card that
 * never responds at the default speed can pass a different divisor instead
 * of repeating the exact same attempt. */
/* On by default. A caller doing a retry ladder that already knows its
 * first attempt failed a specific way can turn this off for the repeat
 * attempts (still going through the exact same code, so a DIFFERENT
 * failure would still need investigating some other way) rather than
 * reprinting the identical multi-line CMD5/bus-busy diagnostic every
 * time. Suppresses everywhere kprintf goes (UART included) - that trade
 * is deliberate: a retry ladder that repeated identical multi-KB output
 * five times is what made a single "wifi init" console reply require an
 * unreliable multi-datagram UDP transport this kernel has not proven
 * solid yet. */
void sdio_set_verbose(bool on);
/* init_hz is the identification clock: the SDIO spec caps it at 400 kHz. The
 * divisor is computed from the base clock the firmware reports, not assumed. */
bool sdio_host_init(uint32_t init_hz = 400000);
bool sdio_is_present();
uint64_t sdio_get_base();

/* Low-level command execution */
bool sdio_send_cmd(uint32_t cmd, uint32_t arg, SdioCmdResp& out_resp);

/* Standard SDIO commands */
bool sdio_cmd0_idle();
bool sdio_cmd5_op_cond(uint32_t ocr, SdioCmdResp& out_resp);
bool sdio_cmd3_rca(uint16_t& out_rca);
bool sdio_cmd7_select(uint16_t rca);

/* SDIO Direct Register Access (CMD52) */
bool sdio_cmd52(uint8_t fn, uint32_t reg_addr, uint8_t val, bool write, bool raw, uint8_t* out_val);
bool sdio_cmd52_read(uint8_t fn, uint32_t reg_addr, uint8_t& out_val);
bool sdio_cmd52_write(uint8_t fn, uint32_t reg_addr, uint8_t val, bool raw = false, uint8_t* out_val = nullptr);

/* SDIO Extended Multi-Byte / Block Data Transfer (CMD53) */
bool sdio_cmd53(uint8_t fn, uint32_t reg_addr, void* buf, uint32_t byte_count, bool write, bool block_mode, bool inc_addr);
bool sdio_cmd53_read(uint8_t fn, uint32_t reg_addr, void* dst, uint32_t byte_count, bool block_mode = false, bool inc_addr = true);
bool sdio_cmd53_write(uint8_t fn, uint32_t reg_addr, const void* src, uint32_t byte_count, bool block_mode = false, bool inc_addr = true);

/* For any transfer smaller than the function's own negotiated block size:
 * on real BCM2837 Arasan hardware, non-block ("byte mode") CMD53 with a
 * count under 512 never raises Transfer Complete - confirmed on a real
 * Pi 3 (a 4-byte backplane write timed out with STATUS busy and zero
 * interrupt bits, and left the bus wedged for the next command). These pad
 * up to exactly one block of the function's registered size (f1_block_size
 * for fn 1, f2_block_size for fn 2) and transfer that in true block mode.
 *
 * FIXED-ADDRESS (inc_addr=false) TRANSFERS ONLY. This is safe for F2's
 * SDPCM FIFO (address 0, extra padding bytes are just extra FIFO pushes or
 * pops the framing already ignores). It is NOT safe for an incrementing
 * backplane access: padding a short write near the end of its 32KB window
 * walks the address past the window's top into the chip's own
 * SBADDRLOW/MID/HIGH/CHIPCLKCSR registers and corrupts them mid-transfer -
 * tried on real hardware, confirmed to break the very next command. Small
 * incrementing backplane accesses use a CMD52-per-byte loop instead (see
 * bcm_read_backplane/bcm_write_backplane). byte_count must not exceed the
 * function's block size. */
/* One CMD53 attempt, no retry - for diagnostics that must see the raw
 * behaviour of a single transfer. */
bool sdio_cmd53_once(uint8_t fn, uint32_t reg_addr, void* buf, uint32_t byte_count, bool write, bool block_mode, bool inc_addr);

bool sdio_cmd53_read_padded(uint8_t fn, uint32_t reg_addr, void* dst, uint32_t byte_count, bool inc_addr = true);
bool sdio_cmd53_write_padded(uint8_t fn, uint32_t reg_addr, const void* src, uint32_t byte_count, bool inc_addr = true);

/* Card enumeration & configuration */
bool sdio_card_init(uint32_t init_hz = 400000);
const SdioCardInfo& sdio_card_info();
bool sdio_set_block_size(uint8_t fn, uint16_t block_size);

/* Interrupt handling */
void sdio_enable_card_interrupt(bool enable);
bool sdio_is_interrupt_pending();
void sdio_clear_interrupt();
void sdio_handle_interrupt();

/* Shell / Diagnostic report */
void sdio_report();
