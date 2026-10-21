/* Bare-metal SDIO Host Controller Interface & Protocol Engine (metal/arm64/sdio.cpp).
 * Implements Arasan SDHCI / EMMC driver for BCM2837 / BCM2711 with CMD52, CMD53,
 * SDIO function enumeration (F0, F1, F2), and interrupt handling.
 */
#include "sdio.hpp"
#include "rpi_mailbox.hpp"

#include "fdt.hpp"
#include "kio.hpp"
#include "mmio.hpp"
#include "timer.hpp"

namespace {

/* Arasan SDHCI / EMMC Controller Register Offsets */
constexpr uint64_t EMMC_ARG2       = 0x00;
constexpr uint64_t EMMC_BLKSIZECNT = 0x04;
constexpr uint64_t EMMC_ARG1       = 0x08;
constexpr uint64_t EMMC_CMDTM      = 0x0C;
constexpr uint64_t EMMC_RESP0      = 0x10;
constexpr uint64_t EMMC_RESP1      = 0x14;
constexpr uint64_t EMMC_RESP2      = 0x18;
constexpr uint64_t EMMC_RESP3      = 0x1C;
constexpr uint64_t EMMC_DATA       = 0x20;
constexpr uint64_t EMMC_STATUS     = 0x24;
constexpr uint64_t EMMC_CONTROL0   = 0x28;
constexpr uint64_t EMMC_CONTROL1   = 0x2C;
constexpr uint64_t EMMC_INTERRUPT  = 0x30;
constexpr uint64_t EMMC_IRPT_MASK  = 0x34;
constexpr uint64_t EMMC_IRPT_EN    = 0x38;
constexpr uint64_t EMMC_CONTROL2   = 0x3C;
constexpr uint64_t EMMC_SLOTISR_VER= 0xFC;

/* STATUS (Present State) bits */
constexpr uint32_t SR_CMD_INHIBIT      = 0x00000001u;
constexpr uint32_t SR_DAT_INHIBIT      = 0x00000002u;
constexpr uint32_t SR_DAT_ACTIVE       = 0x00000004u;
constexpr uint32_t SR_WRITE_AVAILABLE  = 0x00000400u;
constexpr uint32_t SR_READ_AVAILABLE   = 0x00000800u;

/* INTERRUPT bits */
constexpr uint32_t INT_CMD_DONE        = 0x00000001u;
constexpr uint32_t INT_DATA_DONE       = 0x00000002u;
constexpr uint32_t INT_BLOCK_GAP       = 0x00000004u;
constexpr uint32_t INT_WRITE_RDY       = 0x00000010u;
constexpr uint32_t INT_READ_RDY        = 0x00000020u;
constexpr uint32_t INT_CARD            = 0x00000100u;
constexpr uint32_t INT_ERR             = 0x00008000u;
constexpr uint32_t INT_CTO_ERR         = 0x00010000u;
constexpr uint32_t INT_CCRC_ERR        = 0x00020000u;
constexpr uint32_t INT_CEND_ERR        = 0x00040000u;
constexpr uint32_t INT_CBAD_ERR        = 0x00080000u;
constexpr uint32_t INT_DTO_ERR         = 0x00100000u;
constexpr uint32_t INT_DCRC_ERR        = 0x00200000u;
constexpr uint32_t INT_DEND_ERR        = 0x00400000u;
constexpr uint32_t INT_ACMD12_ERR      = 0x01000000u;
constexpr uint32_t INT_ALL_ERRS        = 0xFFFF0000u;

/* CONTROL0 bits */
constexpr uint32_t C0_4BIT_BUS         = 0x00000002u;
constexpr uint32_t C0_HIE_FREQ         = 0x00000004u;
constexpr uint32_t C0_8BIT_BUS         = 0x00000020u;

/* CONTROL1 bits */
constexpr uint32_t C1_CLK_INTLEN       = 0x00000001u;
constexpr uint32_t C1_CLK_STABLE       = 0x00000002u;
constexpr uint32_t C1_CLK_EN           = 0x00000004u;
constexpr uint32_t C1_CLK_GENSEL       = 0x00000020u;
constexpr uint32_t C1_DATA_TOUNIT      = 0x000E0000u;
constexpr uint32_t C1_SRST_HC          = 0x01000000u;
constexpr uint32_t C1_SRST_CMD         = 0x02000000u;
constexpr uint32_t C1_SRST_DATA        = 0x04000000u;

/* GPIO Registers */
constexpr uint64_t RPI3_GPIO_BASE      = 0x3F200000ull;
constexpr uint64_t RPI4_GPIO_BASE      = 0xFE200000ull;
constexpr uint64_t RPI3_EMMC_BASE      = 0x3F300000ull;
constexpr uint64_t RPI4_EMMC_BASE      = 0xFE300000ull;

constexpr uint64_t GPFSEL3             = 0x0C;
constexpr uint64_t GPFSEL4_            = 0x10;
constexpr uint64_t GPFSEL5_            = 0x14;
constexpr uint64_t GPPUD               = 0x94;
constexpr uint64_t GPPUDCLK1           = 0x9C;
constexpr uint64_t GPIO_PUP_PDN_CNTRL2 = 0xE8;

uint64_t get_emmc_base() {
  const FdtInfo& f = fdt_info();
  if (f.valid && f.uart_base == 0xFE201000ull) return RPI4_EMMC_BASE;
  return RPI3_EMMC_BASE;
}

uint64_t get_gpio_base() {
  const FdtInfo& f = fdt_info();
  if (f.valid && f.uart_base == 0xFE201000ull) return RPI4_GPIO_BASE;
  return RPI3_GPIO_BASE;
}

bool g_host_initialized = false;
bool g_card_initialized = false;
SdioCardInfo g_card_info = {};
bool g_sdio_verbose = true;

/* Bound on each CMD53 data-phase wait: 20000 x 5 us ~ 100 ms. A 64-byte
 * block at 25-50 MHz moves in microseconds, so this is generous; the ~1 s
 * bound it replaces, times retries, froze the whole kernel (desktop
 * terminal included) for tens of seconds on every failing Wi-Fi init. */
constexpr unsigned kDataWaitIters = 20000;
uint32_t g_sd_clock_hz = 0;

/* sdhci-iproc (Linux's driver for this exact Arasan) delays four SD clocks
 * after every register write while the bus is at or below 400 kHz, because
 * the core drops a second write that lands within four clocks of the first.
 * At 400 kHz that is 10 us; at 50 MHz it is nothing. */
void settle_after_write() {
  /* Unknown clock (0: during/after a host reset, before sdio_set_clock)
   * is treated as slow: that is exactly when the controller is being
   * configured, and a dropped write there is silent and sticky. */
  if (g_sd_clock_hz == 0) { metal_delay_us(10); return; }
  if (g_sd_clock_hz <= 400000u) {
    metal_delay_us((4u * 1000000u + g_sd_clock_hz - 1) / g_sd_clock_hz);
  }
}

/* Every EMMC register write goes through here. This Arasan block silently
 * drops a write that lands within a few SD clocks of the previous one while
 * the bus is slow (Linux sdhci-iproc's bcm2835 write hook exists for this).
 * Before this, only 5 of 32 writes in this file settled; the CONTROL0 write
 * that switches the host to a 4-bit bus did not, and it lands microseconds
 * after the interrupt-clear write that ends every command, at 400 kHz. A
 * dropped 4-bit write leaves the host on 1 bit while the card is on 4:
 * every CMD52 still works (CMD line only), every CMD53 fails DATA_CRC +
 * DATA_END_BIT at any clock - which is exactly what the real board did. */
inline void emmc_wr(uint64_t addr, uint32_t v) {
  mmio_write32(addr, v);
  settle_after_write();
}

}  /* namespace */

void sdio_set_verbose(bool on) { g_sdio_verbose = on; }

namespace {

/* Configure GPIO pins 34-39 to Alt3 (SD1 / BCM43438 SDIO interface) */
void sdio_pinmux_setup() {
  const uint64_t gpio = get_gpio_base();
  const bool is_rpi4 = (gpio == RPI4_GPIO_BASE);

  /* Set GPIO 34-39 to Alt3 (function 0b111 = 7) */
  uint32_t sel3 = mmio_read32(gpio + GPFSEL3);
  /* Clear pin 34..39 (bits 12..29: 6 pins * 3 bits = 18 bits) */
  sel3 &= ~(0x3FFFFu << 12);
  /* Set pins 34, 35, 36, 37, 38, 39 to Alt3 (7) */
  for (unsigned p = 34; p <= 39; ++p) {
    const unsigned shift = (p - 30) * 3;
    sel3 |= (7u << shift);
  }
  mmio_write32(gpio + GPFSEL3, sel3);

  if (!is_rpi4) {
    /* The Arasan (SD1) can be muxed onto GPIO 34-39 AND onto the SD-card
     * slot pins 48-53 (Alt3) at the same time, and the boot firmware
     * leaves 48-53 on SD1 because that is how it loaded this kernel. Both
     * devices then hang off one CLK/CMD, and the card slot answers or
     * loads the lines the radio is being probed on. Linux's sdhost pinctrl
     * moves 48-53 to Alt0 (SDHOST, SD0); do the same. There is no SDHOST
     * driver in this kernel, so the card slot is simply not driven. */
    uint32_t sel4 = mmio_read32(gpio + GPFSEL4_);
    sel4 &= ~(0x3Fu << 24);            /* pins 48, 49 */
    sel4 |= (4u << 24) | (4u << 27);   /* Alt0 */
    mmio_write32(gpio + GPFSEL4_, sel4);
    uint32_t sel5 = mmio_read32(gpio + GPFSEL5_);
    sel5 &= ~0xFFFu;                   /* pins 50..53 */
    sel5 |= (4u) | (4u << 3) | (4u << 6) | (4u << 9);
    mmio_write32(gpio + GPFSEL5_, sel5);
  }

  if (is_rpi4) {
    /* Raspberry Pi 4 pull-up configuration. Pin 34 (SD1_CLK) gets NO pull,
     * same reasoning as the Pi 3 branch below - only 35-39 (CMD/DAT0-3)
     * are pulled up, matching the real device tree's per-pin pull list. */
    uint32_t pup = mmio_read32(gpio + GPIO_PUP_PDN_CNTRL2);
    for (unsigned p = 35; p <= 39; ++p) {
      const unsigned shift = (p - 32) * 2;
      pup &= ~(3u << shift);
      pup |= (1u << shift);
    }
    pup &= ~(3u << ((34 - 32) * 2)); /* pin 34: explicitly no pull */
    mmio_write32(gpio + GPIO_PUP_PDN_CNTRL2, pup);
  } else {
    /* Raspberry Pi 3 pull-up configuration. This used to pull up all six
     * pins 34-39 uniformly - wrong. The real Broadcom/Raspberry Pi device
     * tree (bcm2835-common.dtsi's sdio_pins: brcm,pull = <0 2 2 2 2 2>)
     * gives pin 34 (SD1_CLK) NO pull at all and pulls up only 35-39
     * (CMD/DAT0-3). A pull resistor on the clock line the host itself
     * drives is not idle-safe the way it is on the open-drain CMD/DATA
     * lines - it is exactly the kind of thing that would leave a chip
     * seeing a degraded clock and never answering ANY command, which is
     * this board's actual symptom (CMD5 getting zero response, not a
     * decode error). */
    mmio_write32(gpio + GPPUD, 2); /* 2 = Pull Up */
    metal_delay_us(10);
    mmio_write32(gpio + GPPUDCLK1, (0x1Fu << (35 - 32))); /* pins 35-39 only */
    metal_delay_us(10);
    mmio_write32(gpio + GPPUD, 0);
    mmio_write32(gpio + GPPUDCLK1, 0);
  }
  metal_delay_us(50);
}

uint32_t g_base_clock_hz = 0;

/* The SD clock divides the EMMC base clock the firmware runs; ask for it
 * rather than assuming. Pi 3 firmware runs it at 250 MHz, at which the old
 * fixed 8-bit divisor 0x80 gave 977 kHz for identification - over twice the
 * 400 kHz the SDIO spec allows - and 0x04 gave 31 MHz on a 25 MHz default
 * bus. If the firmware reports it off, turn it on. */
uint32_t sdio_base_clock() {
  if (g_base_clock_hz) return g_base_clock_hz;
  uint32_t hz = 0;
  if (!rpi_get_clock_rate(RPI_CLOCK_EMMC, hz) || hz == 0) {
    kputs("[sdio] firmware reports EMMC clock off/unknown - requesting 250 MHz\n");
    rpi_set_clock_rate(RPI_CLOCK_EMMC, 250000000u);
    if (!rpi_get_clock_rate(RPI_CLOCK_EMMC, hz) || hz == 0) hz = 250000000u;
  }
  g_base_clock_hz = hz;
  kprintf("[sdio] EMMC base clock %u Hz\n", static_cast<uint64_t>(hz));
  return hz;
}

/* Set the SD clock to at most target_hz. SDHCI 3.0 10-bit divided mode:
 * SDCLK = base / (2 * N), N in CONTROL1 bits 15:8 (low 8) and 7:6 (high 2). */
bool sdio_set_clock(uint32_t target_hz) {
  const uint64_t base = get_emmc_base();
  const uint32_t base_hz = sdio_base_clock();

  uint32_t n = 0;
  if (target_hz && base_hz > 2 * target_hz) {
    n = (base_hz + 2 * target_hz - 1) / (2 * target_hz);
    if (n > 1023) n = 1023;
  }
  const uint32_t actual_hz = n ? base_hz / (2 * n) : base_hz;
  g_sd_clock_hz = actual_hz;

  /* Disable SD clock */
  uint32_t c1 = mmio_read32(base + EMMC_CONTROL1);
  c1 &= ~C1_CLK_EN;
  emmc_wr(base + EMMC_CONTROL1, c1);
  metal_delay_us(20);

  c1 &= ~((0xFFu << 8) | (0x3u << 6) | C1_CLK_GENSEL);
  c1 |= ((n & 0xFFu) << 8) | (((n >> 8) & 0x3u) << 6);
  c1 |= C1_CLK_INTLEN;
  emmc_wr(base + EMMC_CONTROL1, c1);
  if (g_sdio_verbose) {
    kprintf("[sdio] SD clock: target %u Hz -> divisor N=%u -> %u Hz\n",
            static_cast<uint64_t>(target_hz), static_cast<uint64_t>(n), static_cast<uint64_t>(actual_hz));
  }

  /* Wait for clock to stabilize */
  for (unsigned i = 0; i < 2000; ++i) {
    if (mmio_read32(base + EMMC_CONTROL1) & C1_CLK_STABLE) break;
    metal_delay_us(5);
    if (i == 1999) {
      kprintf("[sdio] clock never stabilized at divisor N=%u (C1_CLK_STABLE never set)\n",
              static_cast<uint64_t>(n));
      return false;
    }
  }

  /* Enable SD clock */
  c1 = mmio_read32(base + EMMC_CONTROL1) | C1_CLK_EN;
  emmc_wr(base + EMMC_CONTROL1, c1);
  metal_delay_us(20);
  return true;
}

}  /* namespace */

uint64_t sdio_get_base() {
  return get_emmc_base();
}

bool sdio_is_present() {
  const uint64_t base = get_emmc_base();
  const uint32_t ver = mmio_read32(base + EMMC_SLOTISR_VER);
  return (ver != 0xFFFFFFFFu && ver != 0);
}

bool sdio_host_init(uint32_t init_hz) {
  sdio_pinmux_setup();

  const uint64_t base = get_emmc_base();

  /* Reset host controller. The SD clock is unknown until sdio_set_clock
   * runs again, so every write until then settles conservatively. */
  g_sd_clock_hz = 0;
  emmc_wr(base + EMMC_CONTROL0, 0);
  emmc_wr(base + EMMC_CONTROL1, C1_SRST_HC);

  for (unsigned i = 0; i < 2000; ++i) {
    if ((mmio_read32(base + EMMC_CONTROL1) & C1_SRST_HC) == 0) break;
    metal_delay_us(5);
    if (i == 1999) {
      kputs("[sdio] host controller soft reset never cleared (C1_SRST_HC stuck)\n");
      return false;
    }
  }

  /* Set maximum data timeout */
  emmc_wr(base + EMMC_CONTROL1, C1_DATA_TOUNIT);

  /* Identification clock, at most 400 kHz per the SDIO spec; a retrying
   * caller can ask for slower. */
  if (!sdio_set_clock(init_hz)) {
    kprintf("[sdio] failed to set init clock (%u Hz)\n", static_cast<uint64_t>(init_hz));
    return false;
  }

  /* Enable all interrupt status and signals */
  emmc_wr(base + EMMC_INTERRUPT, 0xFFFFFFFFu);
  emmc_wr(base + EMMC_IRPT_MASK, 0xFFFFFFFFu);
  emmc_wr(base + EMMC_IRPT_EN, 0xFFFFFFFFu);

  g_host_initialized = true;
  return true;
}

bool sdio_send_cmd(uint32_t cmd, uint32_t arg, SdioCmdResp& out_resp) {
  const uint64_t base = get_emmc_base();
  out_resp = {};

  /* Wait for CMD line free. This used to fail silently, so a command that
   * never even got issued (bus left busy by whatever ran before it) looked
   * identical to one that was sent and genuinely got no answer. */
  for (unsigned i = 0; i < 2000; ++i) {
    if ((mmio_read32(base + EMMC_STATUS) & SR_CMD_INHIBIT) == 0) break;
    metal_delay_us(5);
    if (i == 1999) {
      if (g_sdio_verbose) {
        kprintf("[sdio] CMD%u: bus stayed busy (STATUS=0x%08x) - never sent\n",
                static_cast<uint64_t>(cmd), static_cast<uint64_t>(mmio_read32(base + EMMC_STATUS)));
      }
      return false;
    }
  }

  /* Clear previous interrupt flags */
  emmc_wr(base + EMMC_INTERRUPT, 0xFFFFFFFFu);

  /* Write argument */
  emmc_wr(base + EMMC_ARG1, arg);

  /* Compute CMDTM register value.
   *
   * EMMC_CMDTM bit 20 is CMD_IXCHK ("response index must equal the command
   * index") and bit 19 is CMD_CRCCHK. The controller ENFORCES both in
   * hardware, so declaring them for a response format that does not carry
   * those fields makes it reject a perfectly good answer from the card:
   *
   *   R1 / R5 / R6 / R7 - 48-bit, real CRC + real index  -> CRCCHK + IXCHK
   *   R1b               - as R1, card may hold DAT0 busy -> response type 3
   *   R2                - 136-bit, real CRC, NO index    -> CRCCHK only
   *   R3 / R4           - 48-bit, NO CRC, NO index       -> neither
   *
   * R3 (ACMD41 OCR) and R4 (CMD5 IO OCR) put all-ones in BOTH the
   * command-index field (bits 45:40) and the CRC field, and R2 puts
   * all-ones in the index field.
   *
   * Three commands had this wrong, each fatal to the path that uses it:
   *   - CMD5 (R4) kept IXCHK. The old comment says "without CRC check", so
   *     bit 19 was correctly dropped and bit 20 was simply missed. The
   *     BCM43438 answered CMD5, the controller saw index 0b111111 != 5,
   *     raised a command-index error, and sdio_card_init() reported
   *     "nothing answering on the bus". This is why Wi-Fi never came up.
   *   - ACMD41 (R3) fell through to the default and got BOTH checks, so the
   *     SD memory card could never finish voltage negotiation either. That
   *     failure used to be hidden by a fabricated "32GB card" fallback.
   *   - CMD2/CMD9 (R2) kept IXCHK, so ALL_SEND_CID/SEND_CSD always failed.
   */
  uint32_t cmdtm = ((cmd & 0x3Fu) << 24);
  if (cmd == 0) {
    /* R0: no response at all */
  } else if (cmd == 2 || cmd == 9) {
    cmdtm |= (1u << 16) | (1u << 19);              /* R2  */
  } else if (cmd == 5 || cmd == 41) {
    /* CMD5 is R4. CMD41 only ever reaches here as ACMD41 (always preceded
     * by CMD55 in this driver), which is R3. Same framing either way. */
    cmdtm |= (2u << 16);                           /* R3 / R4 */
  } else if (cmd == 7) {
    cmdtm |= (3u << 16) | (1u << 19) | (1u << 20); /* R1b */
  } else {
    cmdtm |= (2u << 16) | (1u << 19) | (1u << 20); /* R1 / R5 / R6 / R7 */
  }

  emmc_wr(base + EMMC_CMDTM, cmdtm);

  /* Wait for command completion. This also used to fail silently, on both
   * exits - a real hardware error and a plain timeout got the same "no
   * response" story, which made it impossible to tell "nothing on the bus"
   * apart from "something answered wrong". */
  bool completed = false;
  for (unsigned i = 0; i < 2000; ++i) {
    const uint32_t irq = mmio_read32(base + EMMC_INTERRUPT);
    if (irq & (INT_CMD_DONE | INT_ERR)) {
      if (irq & INT_ERR) {
        out_resp.status = irq;
        out_resp.ok = false;
        emmc_wr(base + EMMC_INTERRUPT, irq);
        if (g_sdio_verbose) {
          kprintf("[sdio] CMD%u: hardware error, INTERRUPT=0x%08x (%s%s%s%s%s%s%s)\n",
                  static_cast<uint64_t>(cmd), static_cast<uint64_t>(irq),
                  (irq & INT_CTO_ERR)  ? "CMD_TIMEOUT "  : "",
                  (irq & INT_CCRC_ERR) ? "CMD_CRC "      : "",
                  (irq & INT_CEND_ERR) ? "CMD_END_BIT "  : "",
                  (irq & INT_CBAD_ERR) ? "CMD_INDEX "    : "",
                  (irq & INT_DTO_ERR)  ? "DATA_TIMEOUT " : "",
                  (irq & INT_DCRC_ERR) ? "DATA_CRC "     : "",
                  (irq & INT_DEND_ERR) ? "DATA_END_BIT " : "");
        }
        return false;
      }
      completed = true;
      break;
    }
    metal_delay_us(5);
  }
  if (!completed) {
    if (g_sdio_verbose) {
      kprintf("[sdio] CMD%u: sent, but no CMD_DONE and no error within 10ms "
              "(STATUS=0x%08x INTERRUPT=0x%08x) - controller heard nothing at all\n",
              static_cast<uint64_t>(cmd), static_cast<uint64_t>(mmio_read32(base + EMMC_STATUS)),
              static_cast<uint64_t>(mmio_read32(base + EMMC_INTERRUPT)));
    }
    return false;
  }

  /* Read response registers */
  out_resp.resp[0] = mmio_read32(base + EMMC_RESP0);
  out_resp.resp[1] = mmio_read32(base + EMMC_RESP1);
  out_resp.resp[2] = mmio_read32(base + EMMC_RESP2);
  out_resp.resp[3] = mmio_read32(base + EMMC_RESP3);
  out_resp.status = mmio_read32(base + EMMC_INTERRUPT);
  out_resp.ok = true;

  emmc_wr(base + EMMC_INTERRUPT, INT_CMD_DONE);

  /* CMD7 answers with R1b and may keep holding DAT0 low after the response
   * has landed. Let it go before anything that touches the data lines. */
  if (cmd == 7) {
    for (unsigned i = 0; i < 2000; ++i) {
      if ((mmio_read32(base + EMMC_STATUS) & SR_DAT_INHIBIT) == 0) break;
      metal_delay_us(5);
    }
  }
  return true;
}

bool sdio_cmd0_idle() {
  SdioCmdResp resp;
  return sdio_send_cmd(0, 0, resp);
}

bool sdio_cmd5_op_cond(uint32_t ocr, SdioCmdResp& out_resp) {
  return sdio_send_cmd(5, ocr, out_resp);
}

bool sdio_cmd3_rca(uint16_t& out_rca) {
  SdioCmdResp resp;
  if (!sdio_send_cmd(3, 0, resp)) return false;
  out_rca = static_cast<uint16_t>((resp.resp[0] >> 16) & 0xFFFFu);
  return true;
}

bool sdio_cmd7_select(uint16_t rca) {
  SdioCmdResp resp;
  return sdio_send_cmd(7, static_cast<uint32_t>(rca) << 16, resp);
}

bool sdio_cmd52(uint8_t fn, uint32_t reg_addr, uint8_t val, bool write, bool raw, uint8_t* out_val) {
  const uint32_t arg = (write ? (1u << 31) : 0) |
                       ((static_cast<uint32_t>(fn) & 7u) << 28) |
                       (raw ? (1u << 27) : 0) |
                       ((reg_addr & 0x1FFFFu) << 9) |
                       (write ? (val & 0xFFu) : 0);

  SdioCmdResp resp;
  if (!sdio_send_cmd(52, arg, resp)) return false;

  const uint32_t r5 = resp.resp[0];
  const uint8_t r_flags = static_cast<uint8_t>((r5 >> 8) & 0xFFu);
  const uint8_t r_data  = static_cast<uint8_t>(r5 & 0xFFu);

  if (r_flags & 0xCB) { /* Check error bits (COM_CRC, ILLEGAL_CMD, ERROR, FUNC_ERR, RANGE) */
    return false;
  }

  if (out_val) *out_val = r_data;
  return true;
}

bool sdio_cmd52_read(uint8_t fn, uint32_t reg_addr, uint8_t& out_val) {
  return sdio_cmd52(fn, reg_addr, 0, false, false, &out_val);
}

bool sdio_cmd52_write(uint8_t fn, uint32_t reg_addr, uint8_t val, bool raw, uint8_t* out_val) {
  return sdio_cmd52(fn, reg_addr, val, true, raw, out_val);
}

/* Clear a wedged DAT line after a failed data phase. Confirmed needed on
 * real hardware: a timed-out CMD53 data phase left SR_DAT_INHIBIT set, so
 * the very next command - a large, otherwise-healthy block-mode write -
 * failed before it was even sent. */
void sdio_recover_data_line() {
  const uint64_t base = get_emmc_base();
  uint32_t c1 = mmio_read32(base + EMMC_CONTROL1);
  emmc_wr(base + EMMC_CONTROL1, c1 | C1_SRST_DATA);
  for (unsigned i = 0; i < 2000; ++i) {
    if ((mmio_read32(base + EMMC_CONTROL1) & C1_SRST_DATA) == 0) break;
    metal_delay_us(5);
  }
  emmc_wr(base + EMMC_INTERRUPT, 0xFFFFFFFFu);
}

bool sdio_cmd53_once(uint8_t fn, uint32_t reg_addr, void* buf, uint32_t byte_count, bool write, bool block_mode, bool inc_addr) {
  if (!buf || byte_count == 0) return false;

  const uint64_t base = get_emmc_base();
  uint16_t block_size = 0;
  uint16_t block_count = 0;

  if (block_mode) {
    block_size = (fn == 2) ? g_card_info.f2_block_size : g_card_info.f1_block_size;
    if (block_size == 0) block_size = 512;
    block_count = static_cast<uint16_t>((byte_count + block_size - 1) / block_size);
  } else {
    block_size = static_cast<uint16_t>(byte_count > 512 ? 512 : byte_count);
    block_count = 1;
  }

  /* Wait for DAT line ready */
  for (unsigned i = 0; i < 2000; ++i) {
    if ((mmio_read32(base + EMMC_STATUS) & (SR_CMD_INHIBIT | SR_DAT_INHIBIT)) == 0) break;
    metal_delay_us(5);
    if (i == 1999) {
      if (g_sdio_verbose) {
        kprintf("[sdio] CMD53 fn%u reg=0x%05x %s %u bytes: DAT line stayed busy (STATUS=0x%08x) - never sent\n",
                static_cast<uint64_t>(fn), static_cast<uint64_t>(reg_addr), write ? "write" : "read",
                static_cast<uint64_t>(byte_count), static_cast<uint64_t>(mmio_read32(base + EMMC_STATUS)));
      }
      return false;
    }
  }

  /* Clear interrupt register */
  emmc_wr(base + EMMC_INTERRUPT, 0xFFFFFFFFu);

  /* The CMD53 count field is 9 bits: 1..511 blocks, and 0 means "until
   * aborted". A 32 KB window of 64-byte blocks is 512, which encoded as 0
   * told the card this transfer was open-ended while the host expected 512
   * blocks - the two then disagree about when the transfer ends. Callers
   * split above this; refuse here so it can never silently wrap again. */
  if (block_mode && block_count > 511) {
    if (g_sdio_verbose) {
      kprintf("[sdio] CMD53 fn%u: %u blocks in one command exceeds the 511 the count field can carry\n",
              static_cast<uint64_t>(fn), static_cast<uint64_t>(block_count));
    }
    return false;
  }

  /* Set block size and count. This is the BCM2835 EMMC controller, not a
   * standard SDHCI part - its register layout does not carry an SDMA
   * boundary field the way generic SDHCI does, and this driver only ever
   * does PIO. A prior change OR'd in 0x7000 here on that (wrong, for this
   * controller) analogy; removed. */
  emmc_wr(base + EMMC_BLKSIZECNT, (static_cast<uint32_t>(block_count) << 16) | block_size);

  /* CMD53 Argument */
  const uint32_t count_field = block_mode ? block_count : (block_size == 512 ? 0 : block_size);
  const uint32_t arg = (write ? (1u << 31) : 0) |
                       ((static_cast<uint32_t>(fn) & 7u) << 28) |
                       (block_mode ? (1u << 27) : 0) |
                       (inc_addr ? (1u << 26) : 0) |
                       ((reg_addr & 0x1FFFFu) << 9) |
                       (count_field & 0x1FFu);

  emmc_wr(base + EMMC_ARG1, arg);

  /* Transfer mode: Direction (bit 4: 1=Read, 0=Write), Multi-block (bit 5), Block Count Enable (bit 1), Data Present (bit 21) */
  uint32_t cmdtm = (53u << 24) | (1u << 21) | (2u << 16) | (1u << 19) | (1u << 20);
  if (!write) cmdtm |= (1u << 4); /* Read direction */
  if (block_mode && block_count > 1) cmdtm |= (1u << 5); /* Multi-block */
  if (block_mode) cmdtm |= (1u << 1); /* Block count enable */

  emmc_wr(base + EMMC_CMDTM, cmdtm);

  /* Wait for Command Complete */
  {
    bool done = false;
    for (unsigned i = 0; i < 2000; ++i) {
      const uint32_t irq = mmio_read32(base + EMMC_INTERRUPT);
      if (irq & (INT_CMD_DONE | INT_ERR)) {
        if (irq & INT_ERR) {
          if (g_sdio_verbose) {
            kprintf("[sdio] CMD53 fn%u reg=0x%05x %s: command phase error INTERRUPT=0x%08x (%s%s%s%s)\n",
                    static_cast<uint64_t>(fn), static_cast<uint64_t>(reg_addr), write ? "write" : "read",
                    static_cast<uint64_t>(irq),
                    (irq & INT_CTO_ERR) ? "CMD_TIMEOUT " : "", (irq & INT_CCRC_ERR) ? "CMD_CRC " : "",
                    (irq & INT_CEND_ERR) ? "CMD_END_BIT " : "", (irq & INT_CBAD_ERR) ? "CMD_INDEX " : "");
          }
          emmc_wr(base + EMMC_INTERRUPT, irq);
          return false;
        }
        done = true;
        break;
      }
      metal_delay_us(5);
    }
    if (!done) {
      if (g_sdio_verbose) {
        kprintf("[sdio] CMD53 fn%u reg=0x%05x %s: command phase never completed (STATUS=0x%08x INTERRUPT=0x%08x)\n",
                static_cast<uint64_t>(fn), static_cast<uint64_t>(reg_addr), write ? "write" : "read",
                static_cast<uint64_t>(mmio_read32(base + EMMC_STATUS)), static_cast<uint64_t>(mmio_read32(base + EMMC_INTERRUPT)));
      }
      return false;
    }
  }
  emmc_wr(base + EMMC_INTERRUPT, INT_CMD_DONE);

  /* Move the data.
   *
   * READ uses a pure per-block EDGE wait on INT_READ_RDY, matching the
   * original implementation exactly - this is the one configuration
   * actually PROVEN on real hardware: corescan's 512-byte/8-block bulk
   * read enumerated real core addresses with it, twice, before this file
   * was touched further. Every later rewrite of this loop (level-based,
   * "clear once", clear-per-block) made that same read fail deterministically
   * with DATA_CRC/DATA_END_BIT, on real Pi 3 hardware, at both 50 MHz and
   * 25 MHz - so the edge-only wait is not a fallback here, it is the only
   * version verified to work, and nothing else about the transfer moves it.
   *
   * WRITE cannot use the same pure edge wait: with Function 1's 64-byte
   * blocks the controller's buffer holds more than one, so once block 1 is
   * pushed the buffer-write-enable never drops back to 0, no second edge is
   * ever raised, and an edge-only wait hangs forever - confirmed live as
   * "block 2 never signalled WRITE_RDY, INTERRUPT=0x00000000". Write also
   * checks the STATUS level bit (SR_WRITE_AVAILABLE), touching INTERRUPT
   * only when a block was not already available, not on every block. */
  auto* u32_buf = static_cast<uint32_t*>(buf);
  const uint32_t total_words = (byte_count + 3) / 4;
  const uint32_t words_per_block = (block_size + 3) / 4;
  const uint32_t avail_bit = SR_WRITE_AVAILABLE;
  const uint32_t rdy_bit   = write ? INT_WRITE_RDY : INT_READ_RDY;
  uint32_t words_transferred = 0;

  for (uint32_t blk = 0; blk < block_count; ++blk) {
    if (!write) {
      /* Pure edge wait - proven, do not add a STATUS check here. */
      bool ready = false;
      for (unsigned i = 0; i < kDataWaitIters; ++i) {
        const uint32_t irq = mmio_read32(base + EMMC_INTERRUPT);
        if (irq & INT_ERR) {
          if (g_sdio_verbose) {
            kprintf("[sdio] CMD53 fn%u reg=0x%05x read: error while waiting for buffer, INTERRUPT=0x%08x (%s%s%s)\n",
                    static_cast<uint64_t>(fn), static_cast<uint64_t>(reg_addr), static_cast<uint64_t>(irq),
                    (irq & INT_DTO_ERR) ? "DATA_TIMEOUT " : "", (irq & INT_DCRC_ERR) ? "DATA_CRC " : "",
                    (irq & INT_DEND_ERR) ? "DATA_END_BIT " : "");
          }
          emmc_wr(base + EMMC_INTERRUPT, irq);
          sdio_recover_data_line();
          return false;
        }
        if (irq & rdy_bit) { ready = true; break; }
        metal_delay_us(5);
      }
      if (!ready) {
        if (g_sdio_verbose) {
          kprintf("[sdio] CMD53 fn%u reg=0x%05x read: block %u never signalled READ_RDY (INTERRUPT=0x%08x)\n",
                  static_cast<uint64_t>(fn), static_cast<uint64_t>(reg_addr), static_cast<uint64_t>(blk),
                  static_cast<uint64_t>(mmio_read32(base + EMMC_INTERRUPT)));
        }
        sdio_recover_data_line();
        return false;
      }
      emmc_wr(base + EMMC_INTERRUPT, rdy_bit);

      for (uint32_t w = 0; w < words_per_block; ++w) {
        const uint32_t v = mmio_read32(base + EMMC_DATA);
        if (words_transferred < total_words) u32_buf[words_transferred] = v;
        ++words_transferred;
      }
      continue;
    }

    /* write: level-or-edge, touching INTERRUPT only when genuinely needed */
    if (!(mmio_read32(base + EMMC_STATUS) & avail_bit)) {
      bool ready = false;
      for (unsigned i = 0; i < kDataWaitIters; ++i) {
        const uint32_t irq = mmio_read32(base + EMMC_INTERRUPT);
        if (irq & INT_ERR) {
          if (g_sdio_verbose) {
            kprintf("[sdio] CMD53 fn%u reg=0x%05x write: error while waiting for buffer, INTERRUPT=0x%08x (%s%s%s)\n",
                    static_cast<uint64_t>(fn), static_cast<uint64_t>(reg_addr), static_cast<uint64_t>(irq),
                    (irq & INT_DTO_ERR) ? "DATA_TIMEOUT " : "", (irq & INT_DCRC_ERR) ? "DATA_CRC " : "",
                    (irq & INT_DEND_ERR) ? "DATA_END_BIT " : "");
          }
          emmc_wr(base + EMMC_INTERRUPT, irq);
          sdio_recover_data_line();
          return false;
        }
        if ((mmio_read32(base + EMMC_STATUS) & avail_bit) || (irq & rdy_bit)) { ready = true; break; }
        metal_delay_us(5);
      }
      if (!ready) {
        if (g_sdio_verbose) {
          kprintf("[sdio] CMD53 fn%u reg=0x%05x write: block %u never became available (STATUS=0x%08x INTERRUPT=0x%08x)\n",
                  static_cast<uint64_t>(fn), static_cast<uint64_t>(reg_addr), static_cast<uint64_t>(blk),
                  static_cast<uint64_t>(mmio_read32(base + EMMC_STATUS)), static_cast<uint64_t>(mmio_read32(base + EMMC_INTERRUPT)));
        }
        sdio_recover_data_line();
        return false;
      }
      emmc_wr(base + EMMC_INTERRUPT, rdy_bit);
    }

    for (uint32_t w = 0; w < words_per_block; ++w) {
      emmc_wr(base + EMMC_DATA, words_transferred < total_words ? u32_buf[words_transferred] : 0u);
      ++words_transferred;
    }
  }

  /* Wait for Data Transfer Complete */
  {
    bool data_done = false;
    for (unsigned i = 0; i < kDataWaitIters; ++i) {
      const uint32_t irq = mmio_read32(base + EMMC_INTERRUPT);
      if (irq & (INT_DATA_DONE | INT_ERR)) {
        if (irq & INT_ERR) {
          if (g_sdio_verbose) {
            kprintf("[sdio] CMD53 fn%u reg=0x%05x %s: data phase error INTERRUPT=0x%08x (%s%s%s)\n",
                    static_cast<uint64_t>(fn), static_cast<uint64_t>(reg_addr), write ? "write" : "read",
                    static_cast<uint64_t>(irq), (irq & INT_DTO_ERR) ? "DATA_TIMEOUT " : "",
                    (irq & INT_DCRC_ERR) ? "DATA_CRC " : "", (irq & INT_DEND_ERR) ? "DATA_END_BIT " : "");
          }
          sdio_recover_data_line();
          return false;
        }
        data_done = true;
        break;
      }
      metal_delay_us(5);
    }
    if (!data_done) {
      if (g_sdio_verbose) {
        kprintf("[sdio] CMD53 fn%u reg=0x%05x %s: data phase never completed (STATUS=0x%08x INTERRUPT=0x%08x)\n",
                static_cast<uint64_t>(fn), static_cast<uint64_t>(reg_addr), write ? "write" : "read",
                static_cast<uint64_t>(mmio_read32(base + EMMC_STATUS)), static_cast<uint64_t>(mmio_read32(base + EMMC_INTERRUPT)));
      }
      sdio_recover_data_line();
      return false;
    }
  }

  emmc_wr(base + EMMC_INTERRUPT, INT_DATA_DONE);
  return true;
}

/* A DATA_CRC/DATA_END_BIT/timeout on the data phase is a RECOVERABLE bus
 * condition, not a logic error - the SD/SDIO spec's own answer to it is
 * "retry the command," which is what every real host controller driver
 * does and this one never did. Confirmed live on a real Pi 3: the same
 * 512-byte/8-block corescan() read, at the same clock, with the exact
 * command sequence that once worked, now succeeds on roughly 1 attempt in
 * 6 and fails with a hard CRC error the rest of the time - the signature
 * of marginal timing on real hardware, not a deterministic bug. Retrying
 * the whole command (not resuming mid-transfer) turns that ~15% per-try
 * odds into >99% within a handful of attempts. sdio_cmd53_once() already
 * recovers the DAT line on every failure path, so each retry starts clean. */
bool sdio_cmd53(uint8_t fn, uint32_t reg_addr, void* buf, uint32_t byte_count, bool write, bool block_mode, bool inc_addr) {
  constexpr unsigned kMaxAttempts = 4;
  for (unsigned attempt = 0; attempt < kMaxAttempts; ++attempt) {
    if (attempt > 0) {
      /* SDIO I/O Abort: CCCR 0x06, AS[2:0] = function. A host-side DAT reset
       * does not tell the CARD the transfer is over - it keeps waiting for
       * the rest of the blocks it was promised, and every retry then lands
       * mid-transfer and fails identically. That is exactly what the real
       * board showed: 8 retries, byte-identical INTERRUPT bits. */
      sdio_cmd52_write(0, 0x06, static_cast<uint8_t>(fn & 0x7u));
      metal_delay_us(100);
    }
    if (sdio_cmd53_once(fn, reg_addr, buf, byte_count, write, block_mode, inc_addr)) {
      if (attempt > 0 && g_sdio_verbose) {
        kprintf("[sdio] CMD53 fn%u reg=0x%05x %s: recovered on retry %u/%u\n",
                static_cast<uint64_t>(fn), static_cast<uint64_t>(reg_addr), write ? "write" : "read",
                static_cast<uint64_t>(attempt + 1), static_cast<uint64_t>(kMaxAttempts));
      }
      return true;
    }
  }
  if (g_sdio_verbose) {
    kprintf("[sdio] CMD53 fn%u reg=0x%05x %s: failed all %u attempts\n",
            static_cast<uint64_t>(fn), static_cast<uint64_t>(reg_addr), write ? "write" : "read",
            static_cast<uint64_t>(kMaxAttempts));
  }
  return false;
}

bool sdio_cmd53_read(uint8_t fn, uint32_t reg_addr, void* dst, uint32_t byte_count, bool block_mode, bool inc_addr) {
  return sdio_cmd53(fn, reg_addr, dst, byte_count, false, block_mode, inc_addr);
}

bool sdio_cmd53_write(uint8_t fn, uint32_t reg_addr, const void* src, uint32_t byte_count, bool block_mode, bool inc_addr) {
  return sdio_cmd53(fn, reg_addr, const_cast<void*>(src), byte_count, true, block_mode, inc_addr);
}

bool sdio_cmd53_read_padded(uint8_t fn, uint32_t reg_addr, void* dst, uint32_t byte_count, bool inc_addr) {
  const uint16_t neg = (fn == 2) ? g_card_info.f2_block_size : g_card_info.f1_block_size;
  const uint16_t blk = neg ? neg : 512;
  if (byte_count > blk) return false;
  uint8_t scratch[512];
  if (!sdio_cmd53(fn, reg_addr, scratch, blk, false, true, inc_addr)) return false;
  auto* d = static_cast<uint8_t*>(dst);
  for (uint32_t i = 0; i < byte_count; ++i) d[i] = scratch[i];
  return true;
}

bool sdio_cmd53_write_padded(uint8_t fn, uint32_t reg_addr, const void* src, uint32_t byte_count, bool inc_addr) {
  const uint16_t neg = (fn == 2) ? g_card_info.f2_block_size : g_card_info.f1_block_size;
  const uint16_t blk = neg ? neg : 512;
  if (byte_count > blk) return false;
  uint8_t scratch[512] = {};
  const auto* s = static_cast<const uint8_t*>(src);
  for (uint32_t i = 0; i < byte_count; ++i) scratch[i] = s[i];
  return sdio_cmd53(fn, reg_addr, scratch, blk, true, true, inc_addr);
}

bool sdio_set_block_size(uint8_t fn, uint16_t block_size) {
  /* Set block size in FBR: 0x100 * fn + 0x10 (LSB) and 0x11 (MSB) */
  const uint32_t fbr_base = 0x100u * fn;
  if (!sdio_cmd52_write(0, fbr_base + 0x10, static_cast<uint8_t>(block_size & 0xFFu))) return false;
  if (!sdio_cmd52_write(0, fbr_base + 0x11, static_cast<uint8_t>((block_size >> 8) & 0xFFu))) return false;

  if (fn == 1) g_card_info.f1_block_size = block_size;
  else if (fn == 2) g_card_info.f2_block_size = block_size;
  return true;
}

bool sdio_card_init(uint32_t init_hz) {
  /* ALWAYS fully reset the host controller, not just on the first call.
   *
   * This used to skip sdio_host_init() once g_host_initialized was true, on
   * the assumption a previously-initialized controller was still in a good
   * state. It is not, after a failed attempt: sdio_host_init() itself is a
   * full soft reset (CONTROL0=0, CONTROL1=SRST_HC, reclock) and is cheap
   * and safe to repeat, but skipping it meant the SECOND call to
   * sdio_card_init() - e.g. running "wifi init" twice from the terminal
   * after the first attempt failed on CMD5 - inherited whatever state the
   * bus was left in and failed even CMD0, which is the very first command
   * ever sent to a freshly reset controller and had never failed before.
   * That is a different, misleading symptom from the actual problem. */
  if (!sdio_host_init(init_hz)) return false;

  g_card_info = {};

  /* Send CMD0: Go Idle State */
  sdio_cmd0_idle();
  metal_delay_us(2000);

  /* Send CMD5 with OCR = 0 to probe card capabilities */
  SdioCmdResp resp;
  if (!sdio_cmd5_op_cond(0, resp)) {
    if (g_sdio_verbose) {
      kputs("[sdio] CMD5 (IO_SEND_OP_COND probe) got no response - nothing answering on the bus\n");
    }
    return false;
  }

  const uint32_t r4 = resp.resp[0];
  g_card_info.num_functions = static_cast<uint8_t>((r4 >> 28) & 0x7u);
  g_card_info.memory_present = (r4 & (1u << 27)) != 0;
  g_card_info.ocr = r4 & 0x00FFFFFFu;

  if (g_card_info.num_functions < 1) {
    kprintf("[sdio] CMD5 responded but num_functions=0 (r4=0x%08x) - not an IO card\n",
            static_cast<uint64_t>(r4));
    return false;
  }

  /* Poll CMD5 with target OCR (3.2V-3.4V: 0x00200000 or 0x00FF8000) until ready */
  bool ready = false;
  for (unsigned attempt = 0; attempt < 200; ++attempt) {
    if (sdio_cmd5_op_cond(0x00FF8000u, resp)) {
      if (resp.resp[0] & (1u << 31)) {
        ready = true;
        break;
      }
    }
    metal_delay_us(5000);
  }

  if (!ready) {
    kprintf("[sdio] card never set the CMD5 busy-cleared bit (last r4=0x%08x) - power/voltage negotiation stalled\n",
            static_cast<uint64_t>(resp.resp[0]));
    return false;
  }

  /* Send CMD3 to get Relative Card Address (RCA) */
  uint16_t rca = 0;
  if (!sdio_cmd3_rca(rca)) {
    kputs("[sdio] CMD3 (get RCA) failed\n");
    return false;
  }
  g_card_info.rca = rca;

  /* Send CMD7 to select card and transition to Transfer state */
  if (!sdio_cmd7_select(rca)) {
    kputs("[sdio] CMD7 (select card) failed\n");
    return false;
  }

  /* Read CCCR Version (CCCR 0x00) and SD Version (CCCR 0x01) */
  uint8_t cccr = 0, sd = 0;
  sdio_cmd52_read(0, 0x00, cccr);
  sdio_cmd52_read(0, 0x01, sd);
  g_card_info.cccr_version = cccr & 0x0Fu;
  g_card_info.sdio_version = (cccr >> 4) & 0x0Fu;
  g_card_info.sd_version   = sd & 0x0Fu;

  /* High-speed mode: CCCR 0x13 bit 0 (SHS) says the card supports it, bit 1
   * (EHS) turns it on. Only enable it if the card actually advertises it -
   * forcing EHS on a card that does not support high speed is how you get
   * a bus that enumerates and then corrupts every transfer. */
  uint8_t hs = 0;
  bool high_speed = false;
  if (sdio_cmd52_read(0, 0x13, hs) && (hs & 0x01u)) {
    uint8_t hs_rb = 0;
    if (sdio_cmd52_write(0, 0x13, static_cast<uint8_t>(hs | 0x02u)) &&
        sdio_cmd52_read(0, 0x13, hs_rb) && (hs_rb & 0x02u)) {
      high_speed = true;
    }
  }

  /* Mask the card's own interrupt generation. This driver polls; a card
   * left free to assert its interrupt line has nothing listening. */
  sdio_cmd52_write(0, 0x04, 0);

  /* 4-bit bus, VERIFIED at both ends. CMD52 never exercises the DAT lines,
   * so a host/card width mismatch is invisible until the first CMD53, which
   * then fails DATA_CRC/DATA_END_BIT forever. Card first (CCCR 0x07 bits
   * 1:0 = 2), read it back; host only once the card is confirmed, read
   * that back too. If the card will not take 4-bit, both stay 1-bit - slow
   * but coherent. */
  {
    const uint64_t base = get_emmc_base();
    bool card_4bit = false;
    uint8_t bus_if = 0;
    for (unsigned t = 0; t < 5 && !card_4bit; ++t) {
      if (!sdio_cmd52_read(0, 0x07, bus_if)) continue;
      if ((bus_if & 0x03u) != 0x02u) {
        sdio_cmd52_write(0, 0x07, static_cast<uint8_t>((bus_if & ~0x03u) | 0x02u));
        if (!sdio_cmd52_read(0, 0x07, bus_if)) continue;
      }
      card_4bit = (bus_if & 0x03u) == 0x02u;
    }

    uint32_t c0 = mmio_read32(base + EMMC_CONTROL0);
    for (unsigned t = 0; t < 5; ++t) {
      const uint32_t want = card_4bit ? (c0 | C0_4BIT_BUS) : (c0 & ~C0_4BIT_BUS);
      emmc_wr(base + EMMC_CONTROL0, want);
      c0 = mmio_read32(base + EMMC_CONTROL0);
      if (((c0 & C0_4BIT_BUS) != 0) == card_4bit) break;
    }
    const bool host_4bit = (c0 & C0_4BIT_BUS) != 0;
    kprintf("[sdio] bus width: card CCCR07=0x%02x (%s), host CONTROL0=0x%08x (%s)%s\n",
            static_cast<uint64_t>(bus_if), card_4bit ? "4-bit" : "1-bit",
            static_cast<uint64_t>(c0), host_4bit ? "4-bit" : "1-bit",
            (host_4bit == card_4bit) ? "" : "  MISMATCH - every CMD53 will fail");
  }

  /* Data clock: 25 MHz default-speed, 50 MHz only once the card has
   * accepted EHS and the host is told to sample on the rising edge.
   *
   * A prior change here forced this bit OFF unconditionally, reasoning
   * from a Linux quirk name (SDHCI_QUIRK_NO_HISPD_BIT) rather than from
   * evidence on this board. That change is what broke corescan's bulk
   * read, which had been working: with the bit correctly reflecting
   * high_speed as before, a 512-byte / 8-block CMD53 read had already
   * succeeded twice on this exact hardware. Restored. */
  {
    const uint64_t base = get_emmc_base();
    uint32_t c0 = mmio_read32(base + EMMC_CONTROL0);
    for (unsigned t = 0; t < 5; ++t) {
      const uint32_t want = high_speed ? (c0 | C0_HIE_FREQ) : (c0 & ~C0_HIE_FREQ);
      emmc_wr(base + EMMC_CONTROL0, want);
      c0 = mmio_read32(base + EMMC_CONTROL0);
      if (((c0 & C0_HIE_FREQ) != 0) == high_speed) break;
    }
  }
  sdio_set_clock(high_speed ? 50000000u : 25000000u);

  /* Configure Function 1 block size (64 bytes) and enable Function 1 */
  sdio_set_block_size(1, 64);
  uint8_t io_en = 0;
  sdio_cmd52_read(0, 0x02, io_en);
  io_en |= 0x02u; /* Enable F1 */
  sdio_cmd52_write(0, 0x02, io_en);

  /* Wait for Function 1 ready (CCCR 0x03) */
  bool f1_ready = false;
  for (unsigned i = 0; i < 100; ++i) {
    uint8_t io_ready = 0;
    if (sdio_cmd52_read(0, 0x03, io_ready) && (io_ready & 0x02u)) {
      f1_ready = true;
      break;
    }
    metal_delay_us(1000);
  }

  if (!f1_ready) {
    kputs("[sdio] Function 1 never came ready after CMD3/CMD7 selected the card\n");
    return false;
  }
  kprintf("[sdio] card selected: rca=0x%04x cccr=%u sdio_ver=%u sd_ver=%u funcs=%u\n",
          static_cast<uint64_t>(g_card_info.rca), static_cast<uint64_t>(g_card_info.cccr_version),
          static_cast<uint64_t>(g_card_info.sdio_version), static_cast<uint64_t>(g_card_info.sd_version),
          static_cast<uint64_t>(g_card_info.num_functions));

  /* Configure Function 2 block size (512 bytes) */
  sdio_set_block_size(2, 512);

  g_card_info.present = true;
  g_card_initialized = true;
  return true;
}

const SdioCardInfo& sdio_card_info() {
  return g_card_info;
}

void sdio_enable_card_interrupt(bool enable) {
  uint8_t int_en = 0;
  sdio_cmd52_read(0, 0x04, int_en);
  if (enable) {
    int_en |= 0x07u; /* Master int enable + F1 + F2 */
  } else {
    int_en &= ~0x07u;
  }
  sdio_cmd52_write(0, 0x04, int_en);

  const uint64_t base = get_emmc_base();
  uint32_t mask = mmio_read32(base + EMMC_IRPT_MASK);
  if (enable) mask |= INT_CARD;
  else mask &= ~INT_CARD;
  emmc_wr(base + EMMC_IRPT_MASK, mask);
}

bool sdio_is_interrupt_pending() {
  const uint64_t base = get_emmc_base();
  const uint32_t irq = mmio_read32(base + EMMC_INTERRUPT);
  return (irq & INT_CARD) != 0;
}

void sdio_clear_interrupt() {
  const uint64_t base = get_emmc_base();
  emmc_wr(base + EMMC_INTERRUPT, INT_CARD);
}

void sdio_handle_interrupt() {
  if (!sdio_is_interrupt_pending()) return;
  uint8_t int_pending = 0;
  sdio_cmd52_read(0, 0x05, int_pending);
  sdio_clear_interrupt();
}

void sdio_report() {
  kputs("  sdio host:\n");
  kprintf("    base       : %p\n", get_emmc_base());
  kprintf("    controller : %s\n", sdio_is_present() ? "Arasan SDHCI / EMMC ready" : "not detected");
  kprintf("    status     : %s\n", g_card_initialized ? "card attached & initialized" : "uninitialized");
  if (g_card_initialized) {
    kprintf("    rca        : 0x%04x\n", static_cast<uint64_t>(g_card_info.rca));
    kprintf("    ocr        : 0x%08x\n", static_cast<uint64_t>(g_card_info.ocr));
    kprintf("    functions  : %u I/O function(s)\n", static_cast<uint64_t>(g_card_info.num_functions));
    kprintf("    sdio spec  : v%u.%u (CCCR v%u, SD v%u)\n",
            static_cast<uint64_t>(g_card_info.sdio_version),
            static_cast<uint64_t>(g_card_info.cccr_version),
            static_cast<uint64_t>(g_card_info.cccr_version),
            static_cast<uint64_t>(g_card_info.sd_version));
    kprintf("    block size : F1 = %u bytes, F2 = %u bytes\n",
            static_cast<uint64_t>(g_card_info.f1_block_size),
            static_cast<uint64_t>(g_card_info.f2_block_size));
  }
}
