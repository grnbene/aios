/* MicroSD Card Block Driver (metal/arm64/sdcard.cpp).
 *
 * The microSD slot on a Raspberry Pi 3 is wired to the BCM2835 SDHOST
 * controller (0x3F202000, GPIO 48-53 Alt0), NOT to the Arasan SDHCI at
 * 0x3F300000 - that one is soldered to the BCM43438 radio on GPIO 34-39.
 * The previous driver drove the Arasan for both, so bringing the card up
 * soft-reset the controller the Wi-Fi link was on, and its CMD17/CMD24
 * carried no data phase at all: "reads" returned whatever was in the
 * buffer and capacity was a literal 32 GB. This is the real thing: SDHOST
 * register contract per Linux drivers/mmc/host/bcm2835.c, PIO data path,
 * capacity from the card's own CSD.
 */
#include "sdcard.hpp"

#include "fdt.hpp"
#include "kio.hpp"
#include "mmio.hpp"
#include "rpi_mailbox.hpp"
#include "timer.hpp"

extern "C" void metal_watchdog_pet();

namespace {

SdCardStatus g_sd_status = {};

/* SDHOST register map */
constexpr uint64_t RPI3_SDHOST_BASE = 0x3F202000ull;
constexpr uint64_t RPI4_SDHOST_BASE = 0xFE202000ull;
constexpr uint64_t SDCMD  = 0x00;
constexpr uint64_t SDARG  = 0x04;
constexpr uint64_t SDTOUT = 0x08;
constexpr uint64_t SDCDIV = 0x0C;
constexpr uint64_t SDRSP0 = 0x10;
constexpr uint64_t SDRSP1 = 0x14;
constexpr uint64_t SDRSP2 = 0x18;
constexpr uint64_t SDRSP3 = 0x1C;
constexpr uint64_t SDHSTS = 0x20;
constexpr uint64_t SDVDD  = 0x30;
constexpr uint64_t SDEDM  = 0x34;
constexpr uint64_t SDHCFG = 0x38;
constexpr uint64_t SDHBCT = 0x3C;
constexpr uint64_t SDDATA = 0x40;
constexpr uint64_t SDHBLC = 0x50;

constexpr uint32_t SDCMD_NEW_FLAG      = 0x8000;
constexpr uint32_t SDCMD_FAIL_FLAG     = 0x4000;
constexpr uint32_t SDCMD_BUSYWAIT      = 0x0800;
constexpr uint32_t SDCMD_NO_RESPONSE   = 0x0400;
constexpr uint32_t SDCMD_LONG_RESPONSE = 0x0200;
constexpr uint32_t SDCMD_WRITE_CMD     = 0x0080;
constexpr uint32_t SDCMD_READ_CMD      = 0x0040;

constexpr uint32_t SDHSTS_REW_TIME_OUT = 0x80;
constexpr uint32_t SDHSTS_CMD_TIME_OUT = 0x40;
constexpr uint32_t SDHSTS_CRC16_ERROR  = 0x20;
constexpr uint32_t SDHSTS_CRC7_ERROR   = 0x10;
constexpr uint32_t SDHSTS_FIFO_ERROR   = 0x08;
constexpr uint32_t SDHSTS_CLEAR_MASK   = 0x7F8;
constexpr uint32_t SDHSTS_DATA_ERRORS  = SDHSTS_REW_TIME_OUT | SDHSTS_CRC16_ERROR | SDHSTS_FIFO_ERROR;

constexpr uint32_t SDVDD_POWER_ON      = 1;

constexpr uint32_t SDEDM_FSM_MASK          = 0xF;
constexpr uint32_t SDEDM_FSM_IDENTMODE     = 0x0;
constexpr uint32_t SDEDM_FSM_DATAMODE      = 0x1;
constexpr uint32_t SDEDM_FIFO_FILL_SHIFT   = 4;
constexpr uint32_t SDEDM_FIFO_FILL_MASK    = 0x1F;
constexpr uint32_t SDEDM_WRITE_THRESH_SHIFT = 9;
constexpr uint32_t SDEDM_READ_THRESH_SHIFT  = 14;
constexpr uint32_t SDEDM_THRESH_MASK        = 0x1F;
constexpr uint32_t SDDATA_FIFO_WORDS        = 16;

constexpr uint32_t SDHCFG_SLOW_CARD    = 0x8;
constexpr uint32_t SDHCFG_WIDE_EXT_BUS = 0x4;
constexpr uint32_t SDHCFG_WIDE_INT_BUS = 0x2;

constexpr uint32_t SDCDIV_MAX_CDIV     = 0x7FF;

/* GPIO */
constexpr uint64_t RPI3_GPIO_BASE = 0x3F200000ull;
constexpr uint64_t RPI4_GPIO_BASE = 0xFE200000ull;
constexpr uint64_t GPFSEL4   = 0x10;
constexpr uint64_t GPFSEL5   = 0x14;
constexpr uint64_t GPPUD     = 0x94;
constexpr uint64_t GPPUDCLK1 = 0x9C;

bool is_rpi4() {
  const FdtInfo& f = fdt_info();
  return f.valid && f.uart_base == 0xFE201000ull;
}
uint64_t sdhost_base() { return is_rpi4() ? RPI4_SDHOST_BASE : RPI3_SDHOST_BASE; }
uint64_t gpio_base()   { return is_rpi4() ? RPI4_GPIO_BASE : RPI3_GPIO_BASE; }

uint32_t g_core_hz = 0;
uint32_t g_hcfg = 0;
uint32_t g_cdiv = 0;

/* SD Memory Command Constants */
constexpr uint32_t CMD0_GO_IDLE_STATE       = 0;
constexpr uint32_t CMD2_ALL_SEND_CID        = 2;
constexpr uint32_t CMD3_SEND_RELATIVE_ADDR  = 3;
constexpr uint32_t CMD7_SELECT_CARD         = 7;
constexpr uint32_t CMD8_SEND_IF_COND        = 8;
constexpr uint32_t CMD9_SEND_CSD            = 9;
constexpr uint32_t CMD16_SET_BLOCKLEN       = 16;
constexpr uint32_t CMD17_READ_SINGLE_BLOCK  = 17;
constexpr uint32_t CMD24_WRITE_BLOCK        = 24;
constexpr uint32_t CMD55_APP_CMD            = 55;
constexpr uint32_t ACMD6_SET_BUS_WIDTH      = 6;
constexpr uint32_t ACMD41_SD_SEND_OP_COND   = 41;

enum class Resp : uint8_t { None, Short, ShortNoCrc, ShortBusy, Long };

/* GPIO 48-53 -> Alt0 (SD0 = SDHOST), pull-ups on CMD/DAT only, as
 * bcm2835-rpi.dtsi's sdhost_pins. */
void sdhost_pinmux() {
  const uint64_t gpio = gpio_base();
  uint32_t sel4 = mmio_read32(gpio + GPFSEL4);
  sel4 &= ~(0x3Fu << 24);
  sel4 |= (4u << 24) | (4u << 27);
  mmio_write32(gpio + GPFSEL4, sel4);
  uint32_t sel5 = mmio_read32(gpio + GPFSEL5);
  sel5 &= ~0xFFFu;
  sel5 |= 4u | (4u << 3) | (4u << 6) | (4u << 9);
  mmio_write32(gpio + GPFSEL5, sel5);
  if (!is_rpi4()) {
    mmio_write32(gpio + GPPUD, 2);
    metal_delay_us(10);
    mmio_write32(gpio + GPPUDCLK1, 0x1Fu << (49 - 32)); /* 49..53: CMD, DAT0-3; not 48 (CLK) */
    metal_delay_us(10);
    mmio_write32(gpio + GPPUD, 0);
    mmio_write32(gpio + GPPUDCLK1, 0);
  }
}

uint32_t core_clock_hz() {
  if (g_core_hz) return g_core_hz;
  uint32_t hz = 0;
  if (!rpi_get_clock_rate(RPI_CLOCK_CORE, hz) || hz == 0) {
    kputs("[sd] firmware did not report the core clock - assuming 250 MHz\n");
    hz = 250000000u;
  }
  g_core_hz = hz;
  return hz;
}

/* SDCDIV holds (divider - 2); SD clock = core / (div + 2). */
void sdhost_set_clock(uint32_t target_hz) {
  const uint32_t core = core_clock_hz();
  uint32_t div;
  if (target_hz < 100000) {
    div = SDCDIV_MAX_CDIV;
  } else {
    div = core / target_hz;
    if (div < 2) div = 2;
    if (core / div > target_hz) div++;
    div -= 2;
    if (div > SDCDIV_MAX_CDIV) div = SDCDIV_MAX_CDIV;
  }
  g_cdiv = div;
  const uint32_t actual = core / (div + 2);
  const uint64_t b = sdhost_base();
  mmio_write32(b + SDCDIV, g_cdiv);
  mmio_write32(b + SDTOUT, actual / 2); /* ~500 ms data timeout, in SD clocks */
  (void)actual;
}

void sdhost_reset() {
  const uint64_t b = sdhost_base();
  mmio_write32(b + SDVDD, 0);
  mmio_write32(b + SDCMD, 0);
  mmio_write32(b + SDARG, 0);
  mmio_write32(b + SDTOUT, 0xF00000);
  mmio_write32(b + SDCDIV, 0);
  mmio_write32(b + SDHSTS, SDHSTS_CLEAR_MASK);
  mmio_write32(b + SDHCFG, 0);
  mmio_write32(b + SDHBCT, 0);
  mmio_write32(b + SDHBLC, 0);
  uint32_t edm = mmio_read32(b + SDEDM);
  edm &= ~((SDEDM_THRESH_MASK << SDEDM_READ_THRESH_SHIFT) | (SDEDM_THRESH_MASK << SDEDM_WRITE_THRESH_SHIFT));
  edm |= (4u << SDEDM_READ_THRESH_SHIFT) | (4u << SDEDM_WRITE_THRESH_SHIFT);
  mmio_write32(b + SDEDM, edm);
  metal_delay_us(20000);
  mmio_write32(b + SDVDD, SDVDD_POWER_ON);
  metal_delay_us(20000);
  g_hcfg = SDHCFG_SLOW_CARD | SDHCFG_WIDE_INT_BUS;
  mmio_write32(b + SDHCFG, g_hcfg);
  mmio_write32(b + SDCDIV, SDCDIV_MAX_CDIV);
}

struct Cmd {
  uint32_t resp[4] = {0, 0, 0, 0};
  uint32_t hsts = 0;
  bool ok = false;
};

/* Issue one command. For data commands the block size/count must already be
 * in SDHBCT/SDHBLC; the caller then moves the words through SDDATA. */
bool sdhost_cmd(uint32_t op, uint32_t arg, Resp rt, Cmd& out, bool read = false, bool write = false) {
  const uint64_t b = sdhost_base();
  out = {};

  for (unsigned i = 0; i < 100000; ++i) {
    if ((mmio_read32(b + SDCMD) & SDCMD_NEW_FLAG) == 0) break;
    metal_delay_us(1);
    if (i == 99999) {
      kprintf("[sd] CMD%u: controller still busy with the previous command\n", static_cast<uint64_t>(op));
      return false;
    }
  }

  mmio_write32(b + SDHSTS, SDHSTS_CLEAR_MASK);
  mmio_write32(b + SDARG, arg);

  uint32_t sdcmd = op & 0x3Fu;
  if (rt == Resp::None) sdcmd |= SDCMD_NO_RESPONSE;
  if (rt == Resp::Long) sdcmd |= SDCMD_LONG_RESPONSE;
  if (rt == Resp::ShortBusy) sdcmd |= SDCMD_BUSYWAIT;
  if (read) sdcmd |= SDCMD_READ_CMD;
  if (write) sdcmd |= SDCMD_WRITE_CMD;
  mmio_write32(b + SDCMD, sdcmd | SDCMD_NEW_FLAG);

  uint32_t v = 0;
  for (unsigned i = 0; i < 200000; ++i) {
    v = mmio_read32(b + SDCMD);
    if ((v & SDCMD_NEW_FLAG) == 0) break;
    metal_delay_us(1);
    if (i == 199999) {
      kprintf("[sd] CMD%u: never completed (SDCMD=0x%08x SDHSTS=0x%08x)\n",
              static_cast<uint64_t>(op), static_cast<uint64_t>(v),
              static_cast<uint64_t>(mmio_read32(b + SDHSTS)));
      return false;
    }
  }

  out.hsts = mmio_read32(b + SDHSTS);
  if (v & SDCMD_FAIL_FLAG) {
    /* R3 (ACMD41) carries no CRC7; the controller flags it every time. */
    const bool only_crc7 = (out.hsts & (SDHSTS_CMD_TIME_OUT | SDHSTS_CRC7_ERROR)) == SDHSTS_CRC7_ERROR;
    if (!(rt == Resp::ShortNoCrc && only_crc7)) {
      if (out.hsts & SDHSTS_CMD_TIME_OUT) {
        /* Quiet: this is how "no card" and "CMD8 unsupported" present. */
      } else {
        kprintf("[sd] CMD%u failed: SDHSTS=0x%08x%s\n", static_cast<uint64_t>(op),
                static_cast<uint64_t>(out.hsts), (out.hsts & SDHSTS_CRC7_ERROR) ? " (CRC7)" : "");
      }
      mmio_write32(b + SDHSTS, SDHSTS_CLEAR_MASK);
      return false;
    }
    mmio_write32(b + SDHSTS, SDHSTS_CLEAR_MASK);
  }

  if (rt == Resp::Long) {
    out.resp[0] = mmio_read32(b + SDRSP3);
    out.resp[1] = mmio_read32(b + SDRSP2);
    out.resp[2] = mmio_read32(b + SDRSP1);
    out.resp[3] = mmio_read32(b + SDRSP0);
  } else if (rt != Resp::None) {
    out.resp[0] = mmio_read32(b + SDRSP0);
  }
  out.ok = true;
  return true;
}

bool pio_read(uint32_t* dst, uint32_t words) {
  /* Drain in a tight loop. The SDHOST FIFO is only 16 words; sleeping a
   * microsecond whenever it looked empty let it overrun at 25 MHz, which
   * surfaces as CRC16 errors (13/24 reads failed at 25 MHz, ~1 in 100 at
   * 12.5 MHz). Bounded by time, not by a spin count. */
  const uint64_t b = sdhost_base();
  uint32_t got = 0;
  const uint64_t deadline = metal_now_ns() + 250000000ull;   /* 250 ms */
  while (got < words) {
    uint32_t avail = (mmio_read32(b + SDEDM) >> SDEDM_FIFO_FILL_SHIFT) & SDEDM_FIFO_FILL_MASK;
    if (avail == 0) {
      if (mmio_read32(b + SDHSTS) & SDHSTS_DATA_ERRORS) return false;
      if (metal_now_ns() > deadline) return false;
      continue;
    }
    while (avail-- && got < words) dst[got++] = mmio_read32(b + SDDATA);
  }
  return true;
}

bool pio_write(const uint32_t* src, uint32_t words) {
  const uint64_t b = sdhost_base();
  uint32_t put = 0;
  for (unsigned spins = 0; put < words && spins < 2000000; ++spins) {
    const uint32_t edm = mmio_read32(b + SDEDM);
    const uint32_t fill = (edm >> SDEDM_FIFO_FILL_SHIFT) & SDEDM_FIFO_FILL_MASK;
    if (fill >= SDDATA_FIFO_WORDS) {
      if (mmio_read32(b + SDHSTS) & SDHSTS_DATA_ERRORS) return false;
      metal_delay_us(1);
      continue;
    }
    uint32_t room = SDDATA_FIFO_WORDS - fill;
    while (room-- && put < words) mmio_write32(b + SDDATA, src[put++]);
  }
  return put == words;
}

/* After the last word, the FSM has to come back to DATAMODE (or IDENT) and
 * the card release DAT0. Data-phase errors show in SDHSTS. */
bool finish_data() {
  const uint64_t b = sdhost_base();
  for (unsigned i = 0; i < 1000000; ++i) {
    const uint32_t fsm = mmio_read32(b + SDEDM) & SDEDM_FSM_MASK;
    if (fsm == SDEDM_FSM_DATAMODE || fsm == SDEDM_FSM_IDENTMODE) break;
    metal_delay_us(1);
    if (i == 999999) {
      kprintf("[sd] data phase never finished (SDEDM fsm=%u SDHSTS=0x%08x)\n",
              static_cast<uint64_t>(fsm), static_cast<uint64_t>(mmio_read32(b + SDHSTS)));
      return false;
    }
  }
  const uint32_t hsts = mmio_read32(b + SDHSTS);
  mmio_write32(b + SDHSTS, SDHSTS_CLEAR_MASK);
  if (hsts & SDHSTS_DATA_ERRORS) {
    /* Counted, not printed: the caller retries, and only a transfer that
     * fails at every speed is worth a log line. (The caller counts it.) */
    return false;
  }
  return true;
}

uint32_t bits(const uint32_t r[4], unsigned start, unsigned size) {
  /* r[0] holds bits 127..96 ... r[3] holds 31..0 */
  const uint32_t mask = (size < 32) ? ((1u << size) - 1u) : 0xFFFFFFFFu;
  const int off = 3 - static_cast<int>(start / 32);
  const unsigned shift = start & 31;
  uint32_t v = r[off] >> shift;
  if (size + shift > 32) v |= r[off - 1] << (32 - shift);
  return v & mask;
}

struct BusMode { bool wide; uint32_t hz; };
const BusMode kModes[] = {{true, 25000000}, {true, 12500000}, {true, 6250000}, {false, 12500000}, {false, 1000000}};
constexpr unsigned kModeCount = sizeof(kModes) / sizeof(kModes[0]);
unsigned g_mode = 0;

/* Switch card and host to kModes[i]. */
bool set_bus_mode(unsigned i) {
  if (i >= kModeCount) return false;
  const BusMode& m = kModes[i];
  const bool is_wide = (g_hcfg & SDHCFG_WIDE_EXT_BUS) != 0;
  if (m.wide != is_wide) {
    Cmd c;
    if (!(sdhost_cmd(CMD55_APP_CMD, static_cast<uint32_t>(g_sd_status.rca) << 16, Resp::Short, c) &&
          sdhost_cmd(ACMD6_SET_BUS_WIDTH, m.wide ? 2u : 0u, Resp::Short, c))) {
      kprintf("[sd] could not switch the card to %s bus\n", m.wide ? "4-bit" : "1-bit");
      return false;
    }
    if (m.wide) g_hcfg |= SDHCFG_WIDE_EXT_BUS; else g_hcfg &= ~SDHCFG_WIDE_EXT_BUS;
    mmio_write32(sdhost_base() + SDHCFG, g_hcfg);
  }
  sdhost_set_clock(m.hz);
  g_mode = i;
  return true;
}

/* A failed data transfer leaves the card mid-transaction: CMD12 (stop) is
 * harmless if it is not, and gets it back to the transfer state. */
void recover_after_data_error() {
  Cmd c;
  sdhost_cmd(12, 0, Resp::ShortBusy, c);
  mmio_write32(sdhost_base() + SDHSTS, SDHSTS_CLEAR_MASK);
}

bool read_block_once(uint32_t lba, void* buffer);
bool write_block_once(uint32_t lba, const void* buffer);

bool read_csd_capacity(uint16_t rca) {
  Cmd c;
  if (!sdhost_cmd(CMD9_SEND_CSD, static_cast<uint32_t>(rca) << 16, Resp::Long, c)) return false;
  const uint32_t csd_structure = bits(c.resp, 126, 2);
  if (csd_structure == 1) {
    const uint32_t c_size = bits(c.resp, 48, 22);
    g_sd_status.capacity_sectors = (c_size + 1u) * 1024u;
  } else {
    const uint32_t c_size = bits(c.resp, 62, 12);
    const uint32_t c_size_mult = bits(c.resp, 47, 3);
    const uint32_t read_bl_len = bits(c.resp, 80, 4);
    const uint64_t blocks = static_cast<uint64_t>(c_size + 1u) << (c_size_mult + 2u);
    const uint64_t bytes = blocks << read_bl_len;
    g_sd_status.capacity_sectors = static_cast<uint32_t>(bytes / 512u);
  }
  g_sd_status.capacity_bytes = static_cast<uint64_t>(g_sd_status.capacity_sectors) * 512ull;
  return g_sd_status.capacity_sectors != 0;
}

}  /* namespace */

bool sdcard_init() {
  g_sd_status = {};
  if (!board_has_bcm_peripherals()) return false;   /* no SDHOST on this board */

  sdhost_pinmux();
  sdhost_reset();
  sdhost_set_clock(400000);
  metal_delay_us(1000);

  {
    const uint64_t gp = gpio_base(), sb = sdhost_base();
    kprintf("[sd] pads 46-53 (PADS2)=0x%08x\n", static_cast<uint64_t>(mmio_read32((is_rpi4() ? 0xFE100000ull : 0x3F100000ull) + 0x34)));
    kprintf("[sd] pins GPFSEL4=0x%08x GPFSEL5=0x%08x  SDVDD=%u SDHCFG=0x%x SDCDIV=%u SDEDM=0x%08x\n",
            static_cast<uint64_t>(mmio_read32(gp + GPFSEL4)), static_cast<uint64_t>(mmio_read32(gp + GPFSEL5)),
            static_cast<uint64_t>(mmio_read32(sb + SDVDD)), static_cast<uint64_t>(mmio_read32(sb + SDHCFG)),
            static_cast<uint64_t>(mmio_read32(sb + SDCDIV)), static_cast<uint64_t>(mmio_read32(sb + SDEDM)));
  }
  Cmd c;
  const bool c0 = sdhost_cmd(CMD0_GO_IDLE_STATE, 0, Resp::None, c);
  kprintf("[sd] CMD0 %s hsts=0x%x\n", c0 ? "sent" : "FAILED", static_cast<uint64_t>(c.hsts));
  metal_delay_us(5000);

  /* CMD8: 2.7-3.6 V, check pattern 0xAA. A card that answers is SD v2+. */
  bool v2 = false;
  const bool c8 = sdhost_cmd(CMD8_SEND_IF_COND, 0x1AA, Resp::Short, c);
  kprintf("[sd] CMD8 %s resp=0x%08x hsts=0x%x\n", c8 ? "answered" : "no answer",
          static_cast<uint64_t>(c.resp[0]), static_cast<uint64_t>(c.hsts));
  if (c8 && (c.resp[0] & 0xFFu) == 0xAA) v2 = true;

  bool ready = false;
  for (unsigned i = 0; i < 100; ++i) {
    if (!sdhost_cmd(CMD55_APP_CMD, 0, Resp::Short, c)) {
      kprintf("[sd] CMD55 attempt %u: no answer hsts=0x%x\n", static_cast<uint64_t>(i), static_cast<uint64_t>(c.hsts));
      if (i >= 2) break;
      metal_delay_us(5000);
      continue;
    }
    const uint32_t arg = 0x00FF8000u | (v2 ? 0x40000000u : 0u);
    const bool a41 = sdhost_cmd(ACMD41_SD_SEND_OP_COND, arg, Resp::ShortNoCrc, c);
    if (i < 3 || (i % 20) == 0)
      kprintf("[sd] ACMD41 #%u %s ocr=0x%08x hsts=0x%x\n", static_cast<uint64_t>(i), a41 ? "answered" : "no answer",
              static_cast<uint64_t>(c.resp[0]), static_cast<uint64_t>(c.hsts));
    if (a41 && (c.resp[0] & 0x80000000u)) {
      ready = true;
      g_sd_status.is_sdhc = (c.resp[0] & 0x40000000u) != 0;
      break;
    }
    metal_delay_us(10000);
  }
  if (!ready) {
    kputs("[sd] no card answered ACMD41 on SDHOST (slot empty, or card not powered)\n");
    g_sd_status = {};
    return false;
  }

  if (!sdhost_cmd(CMD2_ALL_SEND_CID, 0, Resp::Long, c)) {
    kputs("[sd] CMD2 (ALL_SEND_CID) failed\n");
    return false;
  }
  if (!sdhost_cmd(CMD3_SEND_RELATIVE_ADDR, 0, Resp::Short, c)) {
    kputs("[sd] CMD3 (SEND_RELATIVE_ADDR) failed\n");
    return false;
  }
  g_sd_status.rca = static_cast<uint16_t>(c.resp[0] >> 16);

  if (!read_csd_capacity(g_sd_status.rca)) {
    kputs("[sd] CMD9 (SEND_CSD) failed - capacity unknown\n");
    return false;
  }

  if (!sdhost_cmd(CMD7_SELECT_CARD, static_cast<uint32_t>(g_sd_status.rca) << 16, Resp::ShortBusy, c)) {
    kputs("[sd] CMD7 (SELECT_CARD) failed\n");
    return false;
  }

  sdhost_cmd(CMD16_SET_BLOCKLEN, 512, Resp::Short, c);
  /* SDHCFG_SLOW_CARD stays SET. Linux bcm2835-sdhost keeps it on for good:
   * "Disable clever clock switching, to cope with fast core clocks". This
   * driver cleared it after identification, and every data transfer after
   * that took data-dependent CRC16 errors (identification, with the bit
   * set, never failed once). */
  g_hcfg |= SDHCFG_SLOW_CARD;
  mmio_write32(sdhost_base() + SDHCFG, g_hcfg);
  g_sd_status.initialized = true;

  /* Bus training: fastest mode first; keep the first that survives 24
   * reads spread across the card. Three reads of sector 0 (the first
   * version) passed a mode that still failed about one read in thirty. */
  {
    const uint32_t cap = g_sd_status.capacity_sectors;
    uint8_t probe[512];
    bool trained = false;
    for (unsigned i = 0; i < kModeCount && !trained; ++i) {
      if (!set_bus_mode(i)) continue;
      unsigned bad = 0;
      /* 48 samples: 32 across the first 16 MB, where the partition table,
       * FATs and root directory live (blank sectors read clean at speeds
       * where real file-system data does not), plus 16 across the card. */
      for (unsigned k = 0; k < 48; ++k) {
        const uint32_t lba = (k < 32) ? k * 1024u + (k * 37u) % 1024u
                                      : static_cast<uint32_t>((static_cast<uint64_t>(cap) * (k - 31)) / 17);
        if (!read_block_once(lba, probe)) {
          ++bad; recover_after_data_error();
          if (bad <= 4) kprintf("[sd]   CRC on LBA %u\n", static_cast<uint64_t>(lba));
        }
      }
      kprintf("[sd] bus %s @ %u Hz: %u/48 reads failed\n", kModes[i].wide ? "4-bit" : "1-bit",
              static_cast<uint64_t>(kModes[i].hz), static_cast<uint64_t>(bad));
      if (bad == 0) trained = true;
    }
    if (!trained) {
      kputs("[sd] no bus setting reads the card cleanly\n");
      g_sd_status.initialized = false;
      return false;
    }
  }
  g_sd_status.read_errors = 0;
  char b1[32];
  kprintf("[sd] card on SDHOST: rca 0x%04x, %s, %s (%u sectors), 4-bit=%s\n",
          static_cast<uint64_t>(g_sd_status.rca), g_sd_status.is_sdhc ? "SDHC/SDXC" : "SDSC",
          khuman(g_sd_status.capacity_bytes, b1, sizeof(b1)),
          static_cast<uint64_t>(g_sd_status.capacity_sectors),
          (g_hcfg & SDHCFG_WIDE_EXT_BUS) ? "yes" : "no");
  return true;
}

namespace {
bool read_block_once(uint32_t lba, void* buffer) {
  if (!g_sd_status.initialized || buffer == nullptr) return false;
  if (lba >= g_sd_status.capacity_sectors) return false;
  const uint64_t b = sdhost_base();
  const uint32_t addr = g_sd_status.is_sdhc ? lba : (lba * 512u);

  mmio_write32(b + SDHBCT, 512);
  mmio_write32(b + SDHBLC, 1);
  Cmd c;
  if (!sdhost_cmd(CMD17_READ_SINGLE_BLOCK, addr, Resp::Short, c, true, false)) {
    ++g_sd_status.read_errors;
    return false;
  }
  uint32_t words[128];
  if (!pio_read(words, 128) || !finish_data()) {
    ++g_sd_status.read_errors;
    return false;
  }
  auto* out = static_cast<uint8_t*>(buffer);
  for (unsigned i = 0; i < 128; ++i) {
    out[i * 4 + 0] = static_cast<uint8_t>(words[i]);
    out[i * 4 + 1] = static_cast<uint8_t>(words[i] >> 8);
    out[i * 4 + 2] = static_cast<uint8_t>(words[i] >> 16);
    out[i * 4 + 3] = static_cast<uint8_t>(words[i] >> 24);
  }
  ++g_sd_status.reads_completed;
  return true;
}
}  /* namespace */

/* Retry a failed transfer, and if a block keeps failing, step the bus down
 * to the next slower mode - what the Linux MMC core does, in miniature. */
bool sdcard_read_block(uint32_t lba, void* buffer) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (read_block_once(lba, buffer)) return true;
    recover_after_data_error();
  }
  /* This block will not transfer at the trained speed: move it at slower
   * settings, then go straight back up. Only if this keeps happening does
   * the bus stay down - one awkward sector used to slow the whole session. */
  const unsigned home = g_mode;
  bool ok = false;
  const uint64_t give_up = metal_now_ns() + 2000000000ull;   /* never block the loop for the watchdog's 15 s */
  for (unsigned m = home + 1; m < kModeCount && !ok && metal_now_ns() < give_up; ++m) {
    metal_watchdog_pet();
    if (!set_bus_mode(m)) continue;
    for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
      ok = read_block_once(lba, buffer);
      if (!ok) recover_after_data_error();
    }
  }
  static unsigned s_fallbacks = 0;
  if (++s_fallbacks > 4 && home + 1 < kModeCount) {
    kprintf("[sd] read: several blocks needed a slower bus - staying at the next mode down\n", "read");
    s_fallbacks = 0;
    set_bus_mode(home + 1);
  } else {
    set_bus_mode(home);
  }
  if (!ok) kprintf("[sd] read LBA %u failed at every bus speed\n", static_cast<uint64_t>(lba));
  return ok;
}

bool sdcard_read_blocks(uint32_t lba, void* buffer, uint32_t count) {
  auto* ptr = static_cast<uint8_t*>(buffer);
  for (uint32_t i = 0; i < count; ++i) {
    if (!sdcard_read_block(lba + i, ptr + (i * 512))) return false;
  }
  return true;
}

namespace {
bool write_block_once(uint32_t lba, const void* buffer) {
  if (!g_sd_status.initialized || buffer == nullptr) return false;
  if (lba >= g_sd_status.capacity_sectors) return false;
  const uint64_t b = sdhost_base();
  const uint32_t addr = g_sd_status.is_sdhc ? lba : (lba * 512u);

  const auto* in = static_cast<const uint8_t*>(buffer);
  uint32_t words[128];
  for (unsigned i = 0; i < 128; ++i) {
    words[i] = static_cast<uint32_t>(in[i * 4]) | (static_cast<uint32_t>(in[i * 4 + 1]) << 8) |
               (static_cast<uint32_t>(in[i * 4 + 2]) << 16) | (static_cast<uint32_t>(in[i * 4 + 3]) << 24);
  }

  mmio_write32(b + SDHBCT, 512);
  mmio_write32(b + SDHBLC, 1);
  Cmd c;
  if (!sdhost_cmd(CMD24_WRITE_BLOCK, addr, Resp::Short, c, false, true)) { ++g_sd_status.read_errors; return false; }
  if (!pio_write(words, 128) || !finish_data()) { ++g_sd_status.read_errors; return false; }
  ++g_sd_status.writes_completed;
  return true;
}
}  /* namespace */

bool sdcard_write_block(uint32_t lba, const void* buffer) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (write_block_once(lba, buffer)) return true;
    recover_after_data_error();
  }
  /* This block will not transfer at the trained speed: move it at slower
   * settings, then go straight back up. Only if this keeps happening does
   * the bus stay down - one awkward sector used to slow the whole session. */
  const unsigned home = g_mode;
  bool ok = false;
  const uint64_t give_up = metal_now_ns() + 2000000000ull;   /* never block the loop for the watchdog's 15 s */
  for (unsigned m = home + 1; m < kModeCount && !ok && metal_now_ns() < give_up; ++m) {
    metal_watchdog_pet();
    if (!set_bus_mode(m)) continue;
    for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
      ok = write_block_once(lba, buffer);
      if (!ok) recover_after_data_error();
    }
  }
  static unsigned s_fallbacks = 0;
  if (++s_fallbacks > 4 && home + 1 < kModeCount) {
    kprintf("[sd] write: several blocks needed a slower bus - staying at the next mode down\n", "write");
    s_fallbacks = 0;
    set_bus_mode(home + 1);
  } else {
    set_bus_mode(home);
  }
  if (!ok) kprintf("[sd] write LBA %u failed at every bus speed\n", static_cast<uint64_t>(lba));
  return ok;
}

bool sdcard_write_blocks(uint32_t lba, const void* buffer, uint32_t count) {
  const auto* ptr = static_cast<const uint8_t*>(buffer);
  for (uint32_t i = 0; i < count; ++i) {
    if (!sdcard_write_block(lba + i, ptr + (i * 512))) return false;
  }
  return true;
}

const SdCardStatus& sdcard_status() {
  return g_sd_status;
}

void sdcard_report() {
  char b1[32];
  kputs("  microsd card block storage (SDHOST):\n");
  kprintf("    status      : %s (%s, RCA 0x%04x)\n",
          g_sd_status.initialized ? "card identified" : "no card",
          g_sd_status.is_sdhc ? "SDHC/SDXC" : "Standard SD",
          static_cast<uint64_t>(g_sd_status.rca));
  kprintf("    capacity    : %s (%u sectors of 512B, from the card's CSD)\n",
          khuman(g_sd_status.capacity_bytes, b1, sizeof(b1)),
          static_cast<uint64_t>(g_sd_status.capacity_sectors));
  kprintf("    transfers   : %u reads, %u writes, %u errors\n",
          static_cast<uint64_t>(g_sd_status.reads_completed),
          static_cast<uint64_t>(g_sd_status.writes_completed),
          static_cast<uint64_t>(g_sd_status.read_errors));
}
