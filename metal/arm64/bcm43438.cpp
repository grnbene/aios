/* Broadcom / Cypress BCM43438 WiFi Chip Initialization & Firmware Loader (metal/arm64/bcm43438.cpp).
 * Implements backplane window access, TCM firmware upload, NVRAM formatting and injection,
 * and ARM Cortex-M3 WLAN core reset release.
 */
#include "bcm43438.hpp"

#include "fdt.hpp"
#include "kio.hpp"
#include "mmio.hpp"
#include "net.hpp"
#include "rpi_mailbox.hpp"
#include "sdio.hpp"
#include "timer.hpp"

namespace {

/* Broadcom SDIO Function 1 Registers */
constexpr uint32_t SBSDIO_WATERMARK        = 0x10000;
constexpr uint32_t SBSDIO_DEVICE_CTL       = 0x10001;
constexpr uint32_t SBSDIO_FUNC1_INT_STATUS = 0x10004;
constexpr uint32_t SBSDIO_FUNC1_INT_ENABLE = 0x10005;
constexpr uint32_t SBSDIO_FUNC1_SBADDRLOW  = 0x1000A;
constexpr uint32_t SBSDIO_FUNC1_SBADDRMID  = 0x1000B;
constexpr uint32_t SBSDIO_FUNC1_SBADDRHIGH = 0x1000C;
constexpr uint32_t SBSDIO_FUNC1_FRAMECTRL  = 0x1000D;
constexpr uint32_t SBSDIO_FUNC1_CHIPCLKCSR = 0x1000E;
constexpr uint32_t SBSDIO_FUNC1_PULLUPS    = 0x1000F;

/* Clock control bits */
constexpr uint8_t SBSDIO_FORCE_ALP         = 0x01;
constexpr uint8_t SBSDIO_FORCE_HT          = 0x02;
constexpr uint8_t SBSDIO_FORCE_ILP         = 0x04;
constexpr uint8_t SBSDIO_ALP_AVAIL_REQ     = 0x08;
constexpr uint8_t SBSDIO_HT_AVAIL_REQ      = 0x10;
constexpr uint8_t SBSDIO_NOHWREQ           = 0x20;
constexpr uint8_t SBSDIO_ALP_AVAIL         = 0x40;
constexpr uint8_t SBSDIO_HT_AVAIL          = 0x80;

/* Broadcom Backplane Architecture */
constexpr uint32_t SB_WINDOW_SIZE          = 0x8000;   /* 32 KiB window */
constexpr uint32_t SB_WINDOW_MASK          = 0x7FFF;
constexpr uint32_t SB_WINDOW_OFFSET        = 0x8000;   /* Function 1 address offset for backplane */

constexpr uint32_t CHIPCOMMON_BASE         = 0x18000000; /* enumeration base */
constexpr uint32_t AI_IOCTRL               = 0x0408;
constexpr uint32_t AI_RESETCTRL            = 0x0800;

/* Core IDs in the chip's enumeration ROM. The wrapper addresses for these
 * are NOT fixed across chips or revisions - they have to be discovered by
 * walking the ROM. This driver used to hardcode 0x18103000 for the ARM
 * wrapper and never located the SOCRAM or D11 cores at all, which is why
 * the firmware could never actually be started. */
constexpr uint32_t CORE_CHIPCOMMON         = 0x800;
constexpr uint32_t CORE_D11                = 0x812;
constexpr uint32_t CORE_ARM7TDMI           = 0x825;
constexpr uint32_t CORE_SDIOD              = 0x829;
constexpr uint32_t CORE_ARM_CM3            = 0x82A;
constexpr uint32_t CORE_SOCRAM             = 0x80E;
constexpr uint32_t CORE_ARM_CR4            = 0x83E;
constexpr uint32_t CR4_CPUHALT             = 0x20;
constexpr uint32_t CORESCAN_SZ             = 512;

/* SOCRAM core registers */
constexpr uint32_t SOCRAM_COREINFO         = 0x00;
constexpr uint32_t SOCRAM_BANKIDX          = 0x10;
constexpr uint32_t SOCRAM_BANKINFO         = 0x40;
constexpr uint32_t SOCRAM_BANKPDA          = 0x44;

/* ChipCommon core registers */
constexpr uint32_t CC_GPIOPULLUP           = 0x58;
constexpr uint32_t CC_GPIOPULLDOWN         = 0x5C;

/* SDIO device core registers - the firmware's side of the mailbox */
constexpr uint32_t SD_INTSTATUS            = 0x20;
constexpr uint32_t SD_INTMASK              = 0x24;
constexpr uint32_t SD_SBMBOXDATA           = 0x48;
constexpr uint32_t SD_HOSTMBOXDATA         = 0x4C;
constexpr uint32_t SD_FWREADY              = 0x80;
constexpr uint32_t SD_FCCHANGE             = (1u << 5);
constexpr uint32_t SD_FRAMEINT             = (1u << 6);
constexpr uint32_t SD_MAILBOXINT           = (1u << 7);

/* The BCM43438 reports itself as chip 43430 (Cypress CYW43430). */
constexpr uint32_t BCM_CHIPID_43430        = 43430;

constexpr uint8_t SICF_CLOCK_EN            = 0x01;
constexpr uint8_t SICF_FGC                 = 0x02;
constexpr uint8_t AIRC_RESET               = 0x01;

constexpr uint32_t BCM43438_RAM_BASE       = 0x00000000;
constexpr uint32_t BCM43438_RAM_SIZE       = 0x80000;  /* 512 KiB TCM RAM */

uint32_t g_current_window = 0xFFFFFFFFu;
Bcm43438Info g_info = {};
uint8_t g_nvram_formatted[2048];

/* ---- 32.768 kHz LPO clock (GPCLK2 on GPIO 43) ----
 *
 * The BCM43438 needs a 32.768 kHz low-power oscillator input to run its
 * internal always-on domain. Without it the chip does not answer ANY SDIO
 * command - not CMD5, not anything - which looks exactly like "nothing on
 * the bus" and is indistinguishable from a wiring or controller fault.
 * On a Pi the LPO is not a crystal; the SoC generates it on GPCLK2 and
 * routes it to GPIO 43 (Alt0). Under Linux the GPU firmware and device
 * tree set this up; from bare metal nothing had. */
constexpr uint64_t RPI3_CM_BASE   = 0x3F101000ull;
constexpr uint64_t RPI4_CM_BASE   = 0xFE101000ull;
constexpr uint64_t CM_GP2CTL      = 0x80;
constexpr uint64_t CM_GP2DIV      = 0x84;
constexpr uint32_t CM_PASSWD      = 0x5A000000u;
constexpr uint32_t CM_CTL_ENAB    = (1u << 4);
constexpr uint32_t CM_CTL_BUSY    = (1u << 7);
constexpr uint32_t CM_SRC_OSC     = 1;          /* 19.2 MHz crystal */
constexpr uint32_t CM_MASH1       = (1u << 9);

constexpr uint64_t RPI3_GPIO_BASE_L = 0x3F200000ull;
constexpr uint64_t RPI4_GPIO_BASE_L = 0xFE200000ull;
constexpr uint64_t GPFSEL4          = 0x10;

bool is_rpi4() {
  const FdtInfo& f = fdt_info();
  return f.valid && f.uart_base == 0xFE201000ull;
}

void bcm43438_start_lpo_clock() {
  const uint64_t cm   = is_rpi4() ? RPI4_CM_BASE : RPI3_CM_BASE;
  const uint64_t gpio = is_rpi4() ? RPI4_GPIO_BASE_L : RPI3_GPIO_BASE_L;

  /* 19.2 MHz / 32768 Hz = 585.9375 -> DIVI 585, DIVF 0.9375 * 4096 = 3840,
   * with MASH 1 to dither the fractional part. */
  constexpr uint32_t divi = 585;
  constexpr uint32_t divf = 3840;

  /* Stop the clock before reprogramming it, and wait for it to go quiet -
   * writing DIV while BUSY is set corrupts the generator. */
  mmio_write32(cm + CM_GP2CTL, CM_PASSWD | CM_SRC_OSC);
  for (unsigned i = 0; i < 1000; ++i) {
    if ((mmio_read32(cm + CM_GP2CTL) & CM_CTL_BUSY) == 0) break;
    metal_delay_us(10);
  }

  mmio_write32(cm + CM_GP2DIV, CM_PASSWD | (divi << 12) | divf);
  mmio_write32(cm + CM_GP2CTL, CM_PASSWD | CM_MASH1 | CM_SRC_OSC);
  mmio_write32(cm + CM_GP2CTL, CM_PASSWD | CM_MASH1 | CM_SRC_OSC | CM_CTL_ENAB);

  /* GPIO 43 -> Alt0 (GPCLK2), BEFORE relying on the clock being on the pin.
   * Order matters: the clock generator was enabled above, but nothing
   * connects it to pin 43 until the alt-function is set here. */
  uint32_t sel4 = mmio_read32(gpio + GPFSEL4);
  sel4 &= ~(7u << 9);
  sel4 |=  (4u << 9); /* Alt0 */
  mmio_write32(gpio + GPFSEL4, sel4);

  metal_delay_us(10000); /* let the LPO settle before the chip needs it */

  /* Prove it, rather than assume it: ENAB should read back set, and BUSY
   * set means the generator is actively running (not an error once ENAB is
   * on - it is only a "don't reprogram DIV yet" signal beforehand). GPFSEL4
   * bits [11:9] should read back 4 (Alt0). */
  const uint32_t ctl_final = mmio_read32(cm + CM_GP2CTL);
  const uint32_t sel4_final = (mmio_read32(gpio + GPFSEL4) >> 9) & 7u;
  kprintf("[wifi] LPO: CM_GP2CTL=0x%08x (enab=%u busy=%u) GPIO43 altfunc=%u (want 4)\n",
          static_cast<uint64_t>(ctl_final),
          static_cast<uint64_t>((ctl_final >> 4) & 1u),
          static_cast<uint64_t>((ctl_final >> 7) & 1u),
          static_cast<uint64_t>(sel4_final));
}

}  /* namespace */

/* This had two bugs, and together they meant every single backplane
 * access this driver makes - the chip-ID read, the ARM core reset, the
 * firmware upload, the NVRAM upload - landed at the wrong address:
 *
 *  1. SBADDRMID/SBADDRHIGH were shifted by 15/23, not 16/24. The window
 *     address is split into three bytes at bit offsets 8, 16 and 24
 *     (bits 0-7 come from the CMD52/53 register offset itself, not from
 *     this window); shifting by 15/23 instead reads across a byte
 *     boundary that was never there.
 *  2. SBADDRLOW was masked with 0x7F instead of 0xFF, which - because the
 *     window is 32 KiB-aligned so bits 0-14 of `win` are always zero -
 *     chopped off exactly the one bit that register carries (bit 15 of
 *     the address). Any window whose bit 15 was set silently wrote 0
 *     there instead.
 *
 * Checked against CHIPCOMMON_BASE (0x18000000): the old code produced
 * bytes that decode back to 0x30000000 - not almost right, a completely
 * different address. That is consistent with "no dongle firmware" as a
 * root cause independent of anything else in this file. */
void bcm_set_backplane_window(uint32_t addr) {
  const uint32_t win = addr & ~SB_WINDOW_MASK;
  if (win == g_current_window) return;

  sdio_cmd52_write(1, SBSDIO_FUNC1_SBADDRLOW,  static_cast<uint8_t>((win >> 8)  & 0xFFu));
  sdio_cmd52_write(1, SBSDIO_FUNC1_SBADDRMID,  static_cast<uint8_t>((win >> 16) & 0xFFu));
  sdio_cmd52_write(1, SBSDIO_FUNC1_SBADDRHIGH, static_cast<uint8_t>((win >> 24) & 0xFFu));
  g_current_window = win;
}

uint8_t bcm_read8(uint32_t addr) {
  bcm_set_backplane_window(addr);
  uint8_t v = 0;
  sdio_cmd52_read(1, SB_WINDOW_OFFSET | (addr & SB_WINDOW_MASK), v);
  return v;
}

void bcm_write8(uint32_t addr, uint8_t val) {
  bcm_set_backplane_window(addr);
  sdio_cmd52_write(1, SB_WINDOW_OFFSET | (addr & SB_WINDOW_MASK), val);
}

/* 32-bit backplane registers are accessed as ONE 4-byte CMD53 (byte mode,
 * incrementing), which the SDIO core turns into a single 32-bit backplane
 * access (the 0x8000 "Sb32bit" flag in the F1 address). This is what Linux
 * brcmfmac, Plan 9 and Circle all do.
 *
 * For a while these went through four CMD52 byte accesses instead, after
 * 4-byte CMD53 appeared "broken on this controller". That conclusion was
 * drawn while the host/card bus width was mismatched (a dropped CONTROL0
 * write - see emmc_wr in sdio.cpp), when EVERY DAT-line transfer failed.
 * Byte writes are fine for RAM but not for core wrapper registers: four
 * byte-lane writes to IOCTRL/RESETCTRL are not one 32-bit write, so every
 * sb_disable/sb_reset silently failed and SOCRAM was left in reset - on
 * the real board, even single-byte CMD52 RAM accesses were then refused.
 * CMD52 stays as a read fallback only. */
uint32_t bcm_read32(uint32_t addr) {
  bcm_set_backplane_window(addr);
  uint32_t v = 0;
  if (sdio_cmd53(1, SB_WINDOW_OFFSET | (addr & SB_WINDOW_MASK), &v, 4, false, false, true)) return v;
  v = 0;
  for (unsigned i = 0; i < 4; ++i) {
    uint8_t b = 0;
    if (!sdio_cmd52_read(1, SB_WINDOW_OFFSET | ((addr + i) & SB_WINDOW_MASK), b)) return 0;
    v |= static_cast<uint32_t>(b) << (8u * i);
  }
  return v;
}

bool bcm_write32_checked(uint32_t addr, uint32_t val) {
  bcm_set_backplane_window(addr);
  return sdio_cmd53(1, SB_WINDOW_OFFSET | (addr & SB_WINDOW_MASK), &val, 4, true, false, true);
}

void bcm_write32(uint32_t addr, uint32_t val) {
  if (!bcm_write32_checked(addr, val)) {
    kprintf("[wifi] 32-bit backplane write %08x <- %08x FAILED\n",
            static_cast<uint64_t>(addr), static_cast<uint64_t>(val));
  }
}

/* Bulk backplane transfer. Per 32 KB window: whole Function-1 blocks in
 * block-mode CMD53, at most 511 blocks per command (the count field is
 * 9 bits), then any sub-block tail one byte at a time over CMD52.
 *
 * Why CMD52 for the tail and not a padded block: this address INCREMENTS,
 * so padding a short access near the top of the window walks the F1
 * register address past 0xFFFF into the chip's own SBADDRLOW/MID/HIGH and
 * CHIPCLKCSR registers and corrupts them mid-transfer - tried on real
 * hardware, it broke the very next command. CMD52 touches exactly the
 * bytes asked for. */
namespace {

bool backplane_xfer(uint32_t addr, uint8_t* ptr, size_t len, bool write) {
  const uint16_t neg = sdio_card_info().f1_block_size;
  const uint32_t blk = neg ? neg : 64;
  while (len > 0) {
    bcm_set_backplane_window(addr);
    const uint32_t offset_in_win = addr & SB_WINDOW_MASK;
    const uint32_t max_in_win = SB_WINDOW_SIZE - offset_in_win;
    const uint32_t chunk = static_cast<uint32_t>(len < max_in_win ? len : max_in_win);

    uint32_t done = 0;
    while (chunk - done >= blk) {
      uint32_t blocks = (chunk - done) / blk;
      if (blocks > 511) blocks = 511;
      const uint32_t bytes = blocks * blk;
      const uint32_t reg = SB_WINDOW_OFFSET | (offset_in_win + done);
      const bool ok = write ? sdio_cmd53_write(1, reg, ptr + done, bytes, true, true)
                            : sdio_cmd53_read(1, reg, ptr + done, bytes, true, true);
      if (!ok) return false;
      done += bytes;
    }
    for (; done < chunk; ++done) {
      const uint32_t reg = SB_WINDOW_OFFSET | (offset_in_win + done);
      if (write) {
        if (!sdio_cmd52_write(1, reg, ptr[done])) return false;
      } else {
        uint8_t b = 0;
        if (!sdio_cmd52_read(1, reg, b)) return false;
        ptr[done] = b;
      }
    }
    addr += chunk;
    ptr += chunk;
    len -= chunk;
  }
  return true;
}

}  /* namespace */

bool bcm_read_backplane(uint32_t addr, void* dst, size_t len) {
  return backplane_xfer(addr, static_cast<uint8_t*>(dst), len, false);
}

bool bcm_write_backplane(uint32_t addr, const void* src, size_t len) {
  return backplane_xfer(addr, const_cast<uint8_t*>(static_cast<const uint8_t*>(src)), len, true);
}

namespace {

/* Addresses discovered by walking the chip's core enumeration ROM. */
struct SbCores {
  uint32_t chipcommon;
  uint32_t armcore;   /* CORE_ARM_CM3 / CORE_ARM_CR4 / CORE_ARM7TDMI */
  uint32_t armctl;
  uint32_t armregs;
  uint32_t socramctl;
  uint32_t socramregs;
  uint32_t socramrev;
  uint32_t sdregs;
  uint32_t d11ctl;
  uint32_t rambase;
  uint32_t ramsize;
};
SbCores g_cores = {};

/* Take a core out of service.
 *
 * The read-back after every Ioctrl write is not decoration: this bus posts
 * writes, and the core will act on a stale value if the write has not been
 * pushed through before the next step looks at Resetctrl. The previous
 * implementation wrote Ioctrl/Resetctrl once each with no handshake and no
 * read-back at all, so the core was never actually held in reset. */
void sb_disable(uint32_t regs, uint32_t pre, uint32_t ioctl) {
  if ((bcm_read32(regs + AI_RESETCTRL) & AIRC_RESET) != 0) {
    bcm_write32(regs + AI_IOCTRL, SICF_CLOCK_EN | SICF_FGC | ioctl);
    (void)bcm_read32(regs + AI_IOCTRL);
    return;
  }
  bcm_write32(regs + AI_IOCTRL, SICF_CLOCK_EN | SICF_FGC | pre);
  (void)bcm_read32(regs + AI_IOCTRL);
  bcm_write32(regs + AI_RESETCTRL, AIRC_RESET);
  metal_delay_us(10);
  for (unsigned i = 0; i < 10000; ++i) {
    if ((bcm_read32(regs + AI_RESETCTRL) & AIRC_RESET) != 0) break;
    metal_delay_us(10);
  }
  bcm_write32(regs + AI_IOCTRL, SICF_CLOCK_EN | SICF_FGC | ioctl);
  (void)bcm_read32(regs + AI_IOCTRL);
}

/* Reset a core and bring it back up running. */
void sb_reset(uint32_t regs, uint32_t pre, uint32_t ioctl) {
  sb_disable(regs, pre, ioctl);
  for (unsigned i = 0; i < 10000; ++i) {
    if ((bcm_read32(regs + AI_RESETCTRL) & AIRC_RESET) == 0) break;
    bcm_write32(regs + AI_RESETCTRL, 0);
    metal_delay_us(40);
  }
  bcm_write32(regs + AI_IOCTRL, SICF_CLOCK_EN | ioctl);
  (void)bcm_read32(regs + AI_IOCTRL);
}

/* Walk the chip's core enumeration ROM to find where each core's registers
 * actually live. The ROM is a list of 4-byte descriptors: low nibble 0x1 is
 * a core-info record (carrying the core id), 0x5 is an address record, 0xF
 * terminates. Bit 0xC0 of an address record distinguishes a core's control
 * wrapper from its register block. */
bool corescan() {
  uint8_t buf[CORESCAN_SZ];
  const uint32_t rom = bcm_read32(CHIPCOMMON_BASE + 63 * 4);
  if (!bcm_read_backplane(rom, buf, sizeof(buf))) return false;

  uint32_t coreid = 0;
  uint32_t corerev = 0;
  for (uint32_t i = 0; i + 4 <= CORESCAN_SZ; i += 4) {
    const uint8_t tag = buf[i] & 0xFu;
    if (tag == 0xF) break;

    if (tag == 0x1) {
      if (i + 8 > CORESCAN_SZ) break;
      if ((buf[i + 4] & 0xFu) != 0x1) continue;
      coreid = (static_cast<uint32_t>(buf[i + 1]) |
                (static_cast<uint32_t>(buf[i + 2]) << 8)) & 0xFFFu;
      i += 4;
      corerev = buf[i + 3];
      continue;
    }
    if (tag != 0x5) continue;

    uint32_t addr = (static_cast<uint32_t>(buf[i + 1]) << 8) |
                    (static_cast<uint32_t>(buf[i + 2]) << 16) |
                    (static_cast<uint32_t>(buf[i + 3]) << 24);
    addr &= ~0xFFFu;
    const bool is_ctl = (buf[i] & 0xC0u) != 0;

    switch (coreid) {
      case CORE_CHIPCOMMON:
        if (!is_ctl) g_cores.chipcommon = addr;
        break;
      case CORE_ARM_CM3:
      case CORE_ARM7TDMI:
      case CORE_ARM_CR4:
        g_cores.armcore = coreid;
        if (is_ctl) { if (g_cores.armctl == 0) g_cores.armctl = addr; }
        else        { if (g_cores.armregs == 0) g_cores.armregs = addr; }
        break;
      case CORE_SOCRAM:
        if (is_ctl) g_cores.socramctl = addr;
        else if (g_cores.socramregs == 0) g_cores.socramregs = addr;
        g_cores.socramrev = corerev;
        break;
      case CORE_SDIOD:
        if (!is_ctl) g_cores.sdregs = addr;
        break;
      case CORE_D11:
        if (is_ctl) g_cores.d11ctl = addr;
        break;
      default:
        break;
    }
  }
  return true;
}

/* Work out how much on-chip RAM there is and where it starts, by asking the
 * SOCRAM core about each of its banks. The firmware image and the NVRAM
 * blob are placed relative to this, so a hardcoded guess puts both in the
 * wrong place. */
bool ramscan() {
  if (g_cores.armcore == CORE_ARM_CR4) {
    const uint32_t r = g_cores.armregs;
    const uint32_t cap = bcm_read32(r + 0x04 /* Cr4Cap */);
    const uint32_t banks = ((cap >> 4) & 0xF) + (cap & 0xF);
    uint32_t size = 0;
    for (uint32_t i = 0; i < banks; ++i) {
      bcm_write32(r + 0x40 /* Cr4Bankidx */, i);
      const uint32_t bi = bcm_read32(r + 0x44 /* Cr4Bankinfo */);
      size += 8192u * ((bi & 0x3Fu) + 1u);
    }
    g_cores.ramsize = size;
    g_cores.rambase = 0x198000;
    return true;
  }

  if (g_cores.socramrev <= 7 || g_cores.socramrev == 12) {
    kprintf("[wifi] SOCRAM rev %u not supported\n",
            static_cast<uint64_t>(g_cores.socramrev));
    return false;
  }

  sb_reset(g_cores.socramctl, 0, 0);

  const uint32_t r = g_cores.socramregs;
  const uint32_t info = bcm_read32(r + SOCRAM_COREINFO);
  const uint32_t banks = (info >> 4) & 0xFu;
  uint32_t size = 0;
  for (uint32_t i = 0; i < banks; ++i) {
    bcm_write32(r + SOCRAM_BANKIDX, i);
    const uint32_t bi = bcm_read32(r + SOCRAM_BANKINFO);
    size += 8192u * ((bi & 0x3Fu) + 1u);
  }
  g_cores.ramsize = size;
  g_cores.rambase = 0;

  /* On the 43430, bank 3 comes up with a protected data area that covers
   * where the firmware has to go. Clearing it is required before upload. */
  if (g_info.chip_id == BCM_CHIPID_43430) {
    bcm_write32(r + SOCRAM_BANKIDX, 3);
    bcm_write32(r + SOCRAM_BANKPDA, 0);
  }
  return true;
}

/* Wait for one of the clock-available bits in CHIPCLKCSR. */
bool wait_clk(uint8_t want, unsigned tries) {
  for (unsigned i = 0; i < tries; ++i) {
    uint8_t csr = 0;
    if (sdio_cmd52_read(1, SBSDIO_FUNC1_CHIPCLKCSR, csr) && (csr & want)) return true;
    metal_delay_us(1000);
  }
  return false;
}

/* Parse and format raw NVRAM configuration text into Broadcom binary token block */
uint32_t format_nvram_blob(const char* src, size_t src_len, uint8_t* dst, size_t dst_max) {
  size_t out_idx = 0;
  size_t in_idx = 0;

  while (in_idx < src_len && out_idx + 128 < dst_max) {
    /* Skip leading whitespace / newlines */
    while (in_idx < src_len && (src[in_idx] == ' ' || src[in_idx] == '\t' || src[in_idx] == '\r' || src[in_idx] == '\n')) {
      ++in_idx;
    }
    if (in_idx >= src_len) break;

    /* Skip comment lines */
    if (src[in_idx] == '#') {
      while (in_idx < src_len && src[in_idx] != '\n') ++in_idx;
      continue;
    }

    /* Extract line */
    size_t line_start = in_idx;
    while (in_idx < src_len && src[in_idx] != '\r' && src[in_idx] != '\n') ++in_idx;
    size_t line_end = in_idx;

    /* Trim trailing whitespace */
    while (line_end > line_start && (src[line_end - 1] == ' ' || src[line_end - 1] == '\t')) {
      --line_end;
    }

    if (line_end > line_start) {
      for (size_t i = line_start; i < line_end && out_idx < dst_max - 1; ++i) {
        dst[out_idx++] = static_cast<uint8_t>(src[i]);
      }
      dst[out_idx++] = '\0';
    }
  }

  /* Add trailing double-null */
  if (out_idx < dst_max) dst[out_idx++] = '\0';

  /* Pad with zeros to 4-byte boundary */
  while ((out_idx % 4) != 0 && out_idx < dst_max) {
    dst[out_idx++] = '\0';
  }

  return static_cast<uint32_t>(out_idx);
}

}  /* namespace */

bool g_probe_writes = false;

/* `wifi probe`: every write mechanism against scratch SOCRAM, before any
 * firmware is loaded, in one boot. Pattern-write then read back through
 * CMD52 (the proven path) and compare, so each line says whether that
 * transfer shape actually lands on this chip. */
void probe_backplane_writes() {
  uint8_t csr = 0;
  sdio_cmd52_read(1, SBSDIO_FUNC1_CHIPCLKCSR, csr);
  kprintf("[probe] CHIPCLKCSR=0x%02x (ALP %s, HT %s) rambase=%08x ramsize=%u\n",
          static_cast<uint64_t>(csr), (csr & SBSDIO_ALP_AVAIL) ? "avail" : "NOT avail",
          (csr & SBSDIO_HT_AVAIL) ? "avail" : "not avail",
          static_cast<uint64_t>(g_cores.rambase), static_cast<uint64_t>(g_cores.ramsize));

  kprintf("[probe] socram wrapper: IOCTRL=%08x RESETCTRL=%08x | arm wrapper: IOCTRL=%08x RESETCTRL=%08x\n",
          static_cast<uint64_t>(bcm_read32(g_cores.socramctl + AI_IOCTRL)),
          static_cast<uint64_t>(bcm_read32(g_cores.socramctl + AI_RESETCTRL)),
          static_cast<uint64_t>(bcm_read32(g_cores.armctl + AI_IOCTRL)),
          static_cast<uint64_t>(bcm_read32(g_cores.armctl + AI_RESETCTRL)));
  {
    const uint32_t a = g_cores.rambase + 0x800;
    const bool w = bcm_write32_checked(a, 0xA5C3F00Du);
    const uint32_t r = bcm_read32(a);
    kprintf("[probe] CMD53 4-byte RAM write/read: write=%s read=%08x (%s)\n", w ? "ok" : "FAIL",
            static_cast<uint64_t>(r), r == 0xA5C3F00Du ? "MATCH" : "mismatch");
  }
  static uint8_t pat[4096];
  static uint8_t back[4096];
  const uint32_t base = g_cores.rambase + 0x1000;   /* scratch, window-aligned-ish */

  auto readback_cmd52 = [&](uint32_t addr, uint32_t n) -> bool {
    bcm_set_backplane_window(addr);
    for (uint32_t i = 0; i < n; ++i) {
      bcm_set_backplane_window(addr + i);
      uint8_t b = 0;
      if (!sdio_cmd52_read(1, SB_WINDOW_OFFSET | ((addr + i) & SB_WINDOW_MASK), b)) return false;
      back[i] = b;
    }
    return true;
  };
  auto report = [&](const char* what, bool io_ok, uint32_t addr, uint32_t n, uint8_t seed) {
    uint32_t bad = 0, first_bad = 0xFFFFFFFFu;
    const bool rb = readback_cmd52(addr, n);
    for (uint32_t i = 0; rb && i < n; ++i) {
      if (back[i] != static_cast<uint8_t>(seed + i * 7)) { if (!bad) first_bad = i; ++bad; }
    }
    kprintf("[probe] %-26s io=%s readback=%s mismatches=%u/%u first_bad=%d\n", what,
            io_ok ? "ok" : "FAIL", rb ? "ok" : "FAIL", static_cast<uint64_t>(bad),
            static_cast<uint64_t>(n), static_cast<int64_t>(first_bad == 0xFFFFFFFFu ? -1 : static_cast<int64_t>(first_bad)));
  };
  auto fill = [&](uint32_t n, uint8_t seed) { for (uint32_t i = 0; i < n; ++i) pat[i] = static_cast<uint8_t>(seed + i * 7); };
  auto abort_f1 = [&]() { sdio_cmd52_write(0, 0x06, 1); metal_delay_us(100); };

  /* E1: CMD52 bytes */
  fill(16, 0x11);
  bool ok = true;
  bcm_set_backplane_window(base);
  for (uint32_t i = 0; i < 16 && ok; ++i) ok = sdio_cmd52_write(1, SB_WINDOW_OFFSET | ((base + i) & SB_WINDOW_MASK), pat[i]);
  report("CMD52 x16", ok, base, 16, 0x11);

  /* E2..: CMD53 block writes of increasing length, single attempt each */
  const uint32_t blks[] = {1, 2, 4, 8, 64};
  uint8_t seed = 0x21;
  for (uint32_t nb : blks) {
    const uint32_t n = nb * 64;
    const uint32_t addr = base + 0x100;
    fill(n, seed);
    bcm_set_backplane_window(addr);
    const bool io = sdio_cmd53_once(1, SB_WINDOW_OFFSET | (addr & SB_WINDOW_MASK), pat, n, true, true, true);
    if (!io) abort_f1();
    char label[40];
    const char* pre = "CMD53 write ";
    unsigned p = 0; for (; pre[p]; ++p) label[p] = pre[p];
    unsigned v = nb, d = 0; char tmp[8]; do { tmp[d++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
    while (d) label[p++] = tmp[--d];
    const char* post = " blk"; for (unsigned k = 0; post[k]; ++k) label[p++] = post[k];
    label[p] = 0;
    report(label, io, addr, n > 256 ? 256 : n, seed);
    seed = static_cast<uint8_t>(seed + 0x10);
  }

  /* E7: same 8 blocks as byte-mode-free single-block commands in a loop */
  {
    const uint32_t addr = base + 0x100;
    fill(512, 0x77);
    bool io = true;
    for (uint32_t b = 0; b < 8 && io; ++b) {
      bcm_set_backplane_window(addr + b * 64);
      io = sdio_cmd53_once(1, SB_WINDOW_OFFSET | ((addr + b * 64) & SB_WINDOW_MASK), pat + b * 64, 64, true, true, true);
      if (!io) abort_f1();
    }
    report("CMD53 8x single-blk cmds", io, addr, 256, 0x77);
  }

  /* E8: CMD53 read of 8 blocks back, compared against E7's pattern */
  {
    const uint32_t addr = base + 0x100;
    bcm_set_backplane_window(addr);
    const bool io = sdio_cmd53_once(1, SB_WINDOW_OFFSET | (addr & SB_WINDOW_MASK), back, 512, false, true, true);
    if (!io) abort_f1();
    uint32_t bad = 0;
    for (uint32_t i = 0; io && i < 512; ++i) if (back[i] != static_cast<uint8_t>(0x77 + i * 7)) ++bad;
    kprintf("[probe] %-26s io=%s mismatches=%u/512\n", "CMD53 read 8 blk", io ? "ok" : "FAIL", static_cast<uint64_t>(bad));
  }
}

bool bcm43438_init() {
  g_info = {};

  /* 0a. Start the 32.768 kHz LPO before anything else. The chip needs it
   *     running to come out of reset at all, so it has to be live while
   *     WL_REG_ON is still low. */
  bcm43438_start_lpo_clock();
  kputs("[wifi] 32.768kHz LPO started on GPCLK2/GPIO43\n");

  /* 0b/1. Power the radio and bring up the SDIO card, trying several real
   * variations instead of one attempt.
   *
   * WL_REG_ON is the BCM43438's regulator enable. On a Pi 3 it is NOT a SoC
   * GPIO - it hangs off the VideoCore GPIO expander, so the only way to
   * drive it is the firmware mailbox, and nothing here ever did before this.
   * What is actually wired to WL_REG_ON (which expander pin, how long the
   * regulator takes to ramp) is a board fact, not something derivable from
   * a datasheet alone, so this cycles through the combinations that a real
   * Pi 3 is known to use rather than gambling on one of them:
   *   - expander pin 1 (documented WL_ON) AND pin 0 (documented BT_ON, but
   *     board wiring mistakes/variants exist and it costs nothing to try),
   *   - the datasheet's minimum ~100ms regulator ramp, and twice that in
   *     case this board's regulator is slower than typical,
   *   - the standard ~400kHz identification clock divisor, and a slower one
   *     in case the core clock this divides from is not exactly what the
   *     divisor math assumes.
   * Each combination gets a full host-controller reset (sdio_card_init does
   * this unconditionally) so a bad attempt cannot wedge the one after it. */
  /* An earlier version of this ladder had a first attempt that skipped
   * WL_REG_ON entirely, trusting the boot firmware's own "best-effort"
   * assertion. Pulled after the very next reflash came up with no display
   * and no network activity at all (confirmed via a live packet capture on
   * the switch - zero traffic from the board's MAC, not even ARP) and no
   * UART attached to see why. Nothing in this function's own bounded loops
   * explained a hang under review, but it was the one new, unverified code
   * path in that build and there was no way to clear it without a serial
   * console. Reverting to the five power-cycling attempts that were already
   * proven on real hardware (HDMI + eth0 console came up, WiFi failed
   * cleanly rather than silently) until a UART is available to actually
   * instrument a "don't touch power" attempt instead of guessing at it. */
  /* The clock column is a target frequency; the divisor is derived from the
   * base clock the firmware reports. The old table carried raw 8-bit
   * divisors that, against the Pi 3's 250 MHz EMMC clock, were 977 kHz and
   * 490 kHz - both above the 400 kHz identification ceiling, so no attempt
   * ever probed the chip at a legal speed. The BT_ON row is gone: pin 0 is
   * Bluetooth's regulator, not a wiring variant of WL_ON. */
  struct Attempt { uint32_t wl_pin; uint32_t power_delay_us; uint32_t init_hz; };
  static const Attempt kAttempts[] = {
    {RPI_EXPGPIO_WL_ON, 150000, 400000},
    {RPI_EXPGPIO_WL_ON, 300000, 400000},
    {RPI_EXPGPIO_WL_ON, 150000, 200000},
    {RPI_EXPGPIO_WL_ON, 500000, 100000},
  };
  constexpr size_t kNumAttempts = sizeof(kAttempts) / sizeof(kAttempts[0]);

  bool card_up = false;
  for (size_t attempt = 0; attempt < kNumAttempts && !card_up; ++attempt) {
    const Attempt& a = kAttempts[attempt];
    /* Full per-command SDIO diagnostics (CMD5 error decode, bus-busy, etc)
     * only for the first attempt. Every attempt goes through the exact
     * same sdio_card_init() code, so a SECOND attempt failing differently
     * from the first would still be worth knowing about - but on real
     * hardware all five have been observed to fail identically, and
     * reprinting that identical multi-line diagnostic five times over is
     * what made a single "wifi init" reply need 6+ UDP datagrams, most of
     * which were getting lost. */
    sdio_set_verbose(attempt == 0);

    kprintf("[wifi] power/probe attempt %u/%u: expander pin %u, %ums power delay, %u kHz init clock\n",
            static_cast<uint64_t>(attempt + 1), static_cast<uint64_t>(kNumAttempts),
            static_cast<uint64_t>(a.wl_pin), static_cast<uint64_t>(a.power_delay_us / 1000),
            static_cast<uint64_t>(a.init_hz / 1000));

    /* Full off/on cycle rather than just asserting on, so the chip gets a
     * clean power-on reset regardless of what state it was already in or
     * what the previous attempt left it in. */
    rpi_set_expander_gpio(a.wl_pin, false);
    metal_delay_us(50000);
    const bool powered = rpi_set_expander_gpio(a.wl_pin, true);
    metal_delay_us(a.power_delay_us);

    bool level = false;
    const bool read_ok = rpi_get_expander_gpio(a.wl_pin, level);
    kprintf("[wifi]   set=%s readback=%s\n", powered ? "ok" : "FAILED",
            read_ok ? (level ? "HIGH" : "LOW") : "FAILED");

    if (sdio_card_init(a.init_hz)) {
      card_up = true;
      kprintf("[wifi] SDIO card answered on attempt %u/%u\n",
              static_cast<uint64_t>(attempt + 1), static_cast<uint64_t>(kNumAttempts));
      net_log_wifi("SDIO card answered");
    }
  }
  sdio_set_verbose(true); /* restore the default for everything after this */

  if (!card_up) {
    kprintf("[wifi] SDIO card did not answer after %u power/clock variations - "
            "chip is not responding at the electrical level, not a register bug\n",
            static_cast<uint64_t>(kNumAttempts));
    net_log_wifi("SDIO card unreachable after all retries");
    return false;
  }

  /* 2. Bring up the ALP (active low-power) clock.
   *
   * Order matters and the old code got it wrong: it asked for ALP and
   * forced it in a single write, with no Nohwreq. Clear the register
   * first, request ALP, wait for the chip to say a clock is actually
   * available, and only then force it. Nohwreq stops the chip's own
   * hardware request logic fighting the setting. */
  sdio_cmd52_write(1, SBSDIO_FUNC1_CHIPCLKCSR, 0);
  metal_delay_us(10);
  sdio_cmd52_write(1, SBSDIO_FUNC1_CHIPCLKCSR, SBSDIO_NOHWREQ | SBSDIO_ALP_AVAIL_REQ);
  if (!wait_clk(SBSDIO_ALP_AVAIL | SBSDIO_HT_AVAIL, 500)) {
    kputs("[wifi] ALP clock never came ready (chip not answering CHIPCLKCSR)\n");
    net_log_wifi("ALP clock never came ready");
    return false;
  }
  sdio_cmd52_write(1, SBSDIO_FUNC1_CHIPCLKCSR, SBSDIO_NOHWREQ | SBSDIO_FORCE_ALP);
  metal_delay_us(65);

  /* 3. Identify the chip from the enumeration base, and VALIDATE it. The
   *    old code stored whatever came back without ever checking it, so a
   *    dead bus looked identical to a healthy one. */
  const uint32_t chip_ident = bcm_read32(CHIPCOMMON_BASE);
  g_info.chip_id  = static_cast<uint16_t>(chip_ident & 0xFFFFu);
  g_info.chip_rev = static_cast<uint8_t>((chip_ident >> 16) & 0xFu);
  kprintf("[wifi] chip %u rev %u type %u\n",
          static_cast<uint64_t>(g_info.chip_id),
          static_cast<uint64_t>(g_info.chip_rev),
          static_cast<uint64_t>((chip_ident >> 28) & 0xFu));
  if (g_info.chip_id != BCM_CHIPID_43430) {
    kprintf("[wifi] unexpected chip id %u (raw 0x%08x) - backplane not reading correctly\n",
            static_cast<uint64_t>(g_info.chip_id), static_cast<uint64_t>(chip_ident));
    net_log_wifi("unexpected chip id - backplane bad");
    return false;
  }

  /* 4. Find where the cores actually live. */
  g_cores = {};
  if (!corescan()) {
    kputs("[wifi] core enumeration ROM unreadable\n");
    net_log_wifi("core enumeration ROM unreadable");
    return false;
  }
  kprintf("[wifi] cores: arm %08x/%08x (id %03x) socram %08x/%08x d11 %08x sdio %08x cc %08x\n",
          static_cast<uint64_t>(g_cores.armctl), static_cast<uint64_t>(g_cores.armregs),
          static_cast<uint64_t>(g_cores.armcore),
          static_cast<uint64_t>(g_cores.socramctl), static_cast<uint64_t>(g_cores.socramregs),
          static_cast<uint64_t>(g_cores.d11ctl), static_cast<uint64_t>(g_cores.sdregs),
          static_cast<uint64_t>(g_cores.chipcommon));
  if (g_cores.armctl == 0 || g_cores.d11ctl == 0 ||
      (g_cores.armcore == CORE_ARM_CM3 &&
       (g_cores.socramctl == 0 || g_cores.socramregs == 0))) {
    kputs("[wifi] core scan did not find the essential cores\n");
    net_log_wifi("core scan missing essential cores");
    return false;
  }

  /* 5. Hold the WLAN CPU while its RAM is loaded, and reset the MAC core. */
  if (g_cores.armcore == CORE_ARM_CR4) {
    sb_reset(g_cores.armctl, CR4_CPUHALT, CR4_CPUHALT);
  } else {
    sb_disable(g_cores.armctl, 0, 0);
  }
  sb_reset(g_cores.d11ctl, 8 | 4, 4);

  /* 6. Size the on-chip RAM (this also resets the SOCRAM core). */
  if (!ramscan()) {
    net_log_wifi("SOCRAM scan failed");
    return false;
  }
  g_info.ram_size = g_cores.ramsize;
  kprintf("[wifi] socram %u bytes at %08x\n",
          static_cast<uint64_t>(g_cores.ramsize), static_cast<uint64_t>(g_cores.rambase));
  if (g_cores.ramsize == 0) {
    kputs("[wifi] socram reported zero size\n");
    net_log_wifi("socram reported zero size");
    return false;
  }

  /* 7. Quiet the chip's GPIO pulls before loading firmware. */
  sdio_cmd52_write(1, SBSDIO_FUNC1_PULLUPS, 0);
  if (g_cores.chipcommon) {
    bcm_write32(g_cores.chipcommon + CC_GPIOPULLUP, 0);
    bcm_write32(g_cores.chipcommon + CC_GPIOPULLDOWN, 0);
  }

  /* 8. Upload firmware and NVRAM.
   *
   * The NVRAM goes at the TOP of RAM with a length token in the last word,
   * and that token must be cleared BEFORE the firmware image is written -
   * otherwise a stale token from a previous boot is what the firmware
   * reads. Both are placed relative to the discovered rambase/ramsize, not
   * a hardcoded guess. */
  sdio_cmd52_write(1, SBSDIO_FUNC1_CHIPCLKCSR, SBSDIO_ALP_AVAIL_REQ);
  const bool alp_ok = wait_clk(SBSDIO_ALP_AVAIL, 200);
  if (!alp_ok) kputs("[wifi] ALP clock not available before firmware upload\n");
  if (g_probe_writes) probe_backplane_writes();

  const uint32_t token_addr = g_cores.rambase + g_cores.ramsize - 4;
  uint32_t zero = 0;
  bcm_write_backplane(token_addr, &zero, 4);

  const uint32_t fw_len = static_cast<uint32_t>(bcm43438_firmware_end - bcm43438_firmware_start);
  g_info.firmware_size = fw_len;
  if (fw_len == 0) {
    kputs("[wifi] no firmware blob embedded in the image\n");
    net_log_wifi("no firmware blob embedded");
    return false;
  }
  if (!bcm_write_backplane(g_cores.rambase, bcm43438_firmware_start, fw_len)) {
    kputs("[wifi] firmware upload to TCM RAM failed\n");
    net_log_wifi("firmware upload to TCM RAM failed");
    return false;
  }

  const uint32_t nvram_raw_len = static_cast<uint32_t>(bcm43438_nvram_end - bcm43438_nvram_start);
  const uint32_t nvram_len = format_nvram_blob(bcm43438_nvram_start, nvram_raw_len,
                                               g_nvram_formatted, sizeof(g_nvram_formatted));
  g_info.nvram_size = nvram_len;
  if (nvram_len == 0) {
    kputs("[wifi] NVRAM blob formatted to nothing\n");
    net_log_wifi("NVRAM blob empty");
    return false;
  }
  const uint32_t nvram_dest = g_cores.rambase + g_cores.ramsize - nvram_len - 4;
  if (!bcm_write_backplane(nvram_dest, g_nvram_formatted, nvram_len)) {
    kputs("[wifi] NVRAM upload to TCM RAM failed\n");
    net_log_wifi("NVRAM upload to TCM RAM failed");
    return false;
  }
  {
    const uint32_t words = nvram_len / 4;
    const uint32_t token = ((~words & 0xFFFFu) << 16) | (words & 0xFFFFu);
    bcm_write_backplane(token_addr, &token, 4);
  }

  /* 9. Start the WLAN CPU. Everything above this point was preparation;
   *    this is the instruction that makes the radio's firmware run. */
  if (g_cores.armcore == CORE_ARM_CR4) {
    if (g_cores.sdregs) bcm_write32(g_cores.sdregs + SD_INTSTATUS, 0xFFFFFFFFu);
    sb_reset(g_cores.armctl, CR4_CPUHALT, 0);
  } else {
    sb_reset(g_cores.armctl, 0, 0);
  }

  /* 10. Switch to the HT clock now that firmware is running. */
  sdio_cmd52_write(1, SBSDIO_FUNC1_CHIPCLKCSR, 0);
  metal_delay_us(1000);
  sdio_cmd52_write(1, SBSDIO_FUNC1_CHIPCLKCSR, SBSDIO_HT_AVAIL_REQ);
  if (!wait_clk(SBSDIO_HT_AVAIL, 5000)) {
    kputs("[wifi] HT clock never came ready - firmware did not start\n");
    net_log_wifi("HT clock never came ready");
    return false;
  }
  {
    uint8_t csr = 0;
    sdio_cmd52_read(1, SBSDIO_FUNC1_CHIPCLKCSR, csr);
    sdio_cmd52_write(1, SBSDIO_FUNC1_CHIPCLKCSR, static_cast<uint8_t>(csr | SBSDIO_FORCE_HT));
  }
  metal_delay_us(10000);

  /* 11. Hand the firmware its mailbox protocol version and unmask the
   *     interrupts it uses to talk back. */
  if (g_cores.sdregs) {
    bcm_write32(g_cores.sdregs + SD_SBMBOXDATA, 4u << 16); /* protocol version 4 */
    bcm_write32(g_cores.sdregs + SD_INTMASK, SD_FRAMEINT | SD_MAILBOXINT | SD_FCCHANGE);
  }

  /* 12. Only now can Function 2 (the WLAN data path) come ready - it is the
   *     running firmware that brings it up, which is why every previous
   *     attempt to enable it before starting the CPU timed out. */
  uint8_t io_en = 0;
  sdio_cmd52_read(0, 0x02, io_en);
  sdio_cmd52_write(0, 0x02, static_cast<uint8_t>(io_en | 0x04u));

  bool f2_ready = false;
  for (unsigned i = 0; i < 500; ++i) {
    uint8_t io_ready = 0;
    if (sdio_cmd52_read(0, 0x03, io_ready) && (io_ready & 0x04u)) { f2_ready = true; break; }
    metal_delay_us(2000);
  }
  g_info.f2_ready = f2_ready;
  if (!f2_ready) {
    kputs("[wifi] BCM43438 Function 2 (WLAN data) never came ready\n");
    net_log_wifi("Function 2 (WLAN data) never came ready");
    return false;
  }

  /* 13. Enable the card's interrupt sources for F1 and F2. */
  sdio_cmd52_write(0, 0x04, static_cast<uint8_t>((1u << 1) | (1u << 2) | 1u));

  if (g_cores.sdregs) {
    const uint32_t mbox = bcm_read32(g_cores.sdregs + SD_HOSTMBOXDATA);
    kprintf("[wifi] firmware mailbox 0x%08x%s\n", static_cast<uint64_t>(mbox),
            (mbox & SD_FWREADY) ? " (FWREADY)" : "");
  }
  kputs("[wifi] BCM43438 core + firmware + F2 up\n");
  net_log_wifi("BCM43438 core + firmware + F2 up");

  /* 14. Configure Function 2 block size to 512 bytes and enable SDIO interrupts */
  sdio_set_block_size(2, 512);
  sdio_enable_card_interrupt(true);

  g_info.initialized = true;
  return true;
}

uint32_t bcm43438_sdio_core_regs() {
  return g_cores.sdregs;
}

const Bcm43438Info& bcm43438_info() {
  return g_info;
}

void bcm43438_report() {
  char b1[32], b2[32], b3[32];
  kputs("  bcm43438 wireless:\n");
  kprintf("    chip status : %s\n", g_info.initialized ? "initialized & online" : "not initialized");
  if (g_info.initialized) {
    kprintf("    chip id     : 0x%04x (rev %u)\n",
            static_cast<uint64_t>(g_info.chip_id),
            static_cast<uint64_t>(g_info.chip_rev));
    kprintf("    tcm ram     : %s at %p\n",
            khuman(g_info.ram_size, b1, sizeof(b1)),
            BCM43438_RAM_BASE);
    kprintf("    firmware    : %s uploaded\n",
            khuman(g_info.firmware_size, b2, sizeof(b2)));
    kprintf("    nvram       : %s uploaded (token at %p)\n",
            khuman(g_info.nvram_size, b3, sizeof(b3)),
            BCM43438_RAM_BASE + BCM43438_RAM_SIZE - 4);
    kprintf("    wlan core   : %s (Function 2 ready)\n",
            g_info.f2_ready ? "active" : "pending");
  }
}
