/* Raspberry Pi VideoCore Mailbox implementation (metal/arm64/rpi_mailbox.cpp). */
#include "rpi_mailbox.hpp"

#include "fdt.hpp"
#include "kio.hpp"
#include "mmio.hpp"

namespace {

constexpr uint64_t RPI3_MBOX_BASE = 0x3F00B880ull;
constexpr uint64_t RPI4_MBOX_BASE = 0xFE00B880ull;

constexpr uint64_t MBOX0_READ   = 0x00;
constexpr uint64_t MBOX0_STATUS = 0x18;
constexpr uint64_t MBOX1_WRITE  = 0x20;
constexpr uint64_t MBOX1_STATUS = 0x38;

constexpr uint32_t MBOX_FULL  = 0x80000000u;
constexpr uint32_t MBOX_EMPTY = 0x40000000u;

uint64_t get_mbox_base() {
  const FdtInfo& f = fdt_info();
  if (f.valid && f.uart_base == 0xFE201000ull) return RPI4_MBOX_BASE;
  return RPI3_MBOX_BASE;
}

alignas(64) volatile uint32_t g_mbox_buf[64];

}  /* namespace */

bool rpi_mailbox_available() {
  /* Not a Broadcom board (QEMU virt): the read below would be an external
   * abort, not a 0xFFFFFFFF. */
  if (!board_has_bcm_peripherals()) return false;
  const uint64_t base = get_mbox_base();
  const uint32_t status = mmio_read32(base + MBOX0_STATUS);
  /* If MMIO unmapped or float, reading 0xFFFFFFFF usually means not present */
  return (status != 0xFFFFFFFFu);
}

bool rpi_mailbox_call(uint32_t channel, volatile uint32_t* msg_buffer) {
  if (!board_has_bcm_peripherals()) return false;
  const uint64_t base = get_mbox_base();
  const uint32_t arm_addr = static_cast<uint32_t>(reinterpret_cast<uint64_t>(msg_buffer) & 0xFFFFFFFFull);
  /* Raspberry Pi 3 VideoCore GPU expects 0xC0000000 uncached L2 bus alias */
  const uint32_t bus_addr = arm_addr | 0xC0000000u;
  const uint32_t send_val = (bus_addr & ~0xFu) | (channel & 0xFu);

  /* Clean & invalidate data cache so VideoCore reads our written buffer */
  const uint64_t line_start = reinterpret_cast<uint64_t>(msg_buffer) & ~63ull;
  const uint64_t line_end = reinterpret_cast<uint64_t>(msg_buffer) + sizeof(g_mbox_buf);
  for (uint64_t p = line_start; p < line_end; p += 64) {
    asm volatile("dc civac, %0" :: "r"(p) : "memory");
  }
  asm volatile("dsb sy; isb" ::: "memory");

  /* Drain replies nobody collected. A call that timed out leaves its reply
   * in the read FIFO; the next call then took THAT as its own answer, read a
   * buffer the GPU had not processed yet (response code 0x00000000), and
   * every later call was one reply behind - which is how the core-clock and
   * board-revision queries failed on the real board. */
  for (unsigned i = 0; i < 16 && (mmio_read32(base + MBOX0_STATUS) & MBOX_EMPTY) == 0; ++i) {
    (void)mmio_read32(base + MBOX0_READ);
  }

  /* Wait until Mailbox 1 is not full */
  for (unsigned i = 0; i < 5000000; ++i) {
    if ((mmio_read32(base + MBOX1_STATUS) & MBOX_FULL) == 0) break;
    if (i == 4999999) return false;
  }

  asm volatile("dsb sy" ::: "memory");
  mmio_write32(base + MBOX1_WRITE, send_val);

  /* Wait for response on Mailbox 0 */
  for (;;) {
    for (unsigned i = 0; i < 5000000; ++i) {
      if ((mmio_read32(base + MBOX0_STATUS) & MBOX_EMPTY) == 0) break;
      if (i == 4999999) return false;
    }

    const uint32_t resp = mmio_read32(base + MBOX0_READ);
    if ((resp & 0xFu) == (channel & 0xFu)) {
      /* Invalidate data cache so ARM CPU reads response written by GPU */
      for (uint64_t p = line_start; p < line_end; p += 64) {
        asm volatile("dc ivac, %0" :: "r"(p) : "memory");
      }
      asm volatile("dsb sy; isb" ::: "memory");
      /* Still our untouched request (code 0): a stale reply slipped past the
       * drain above. Keep waiting for the real one. */
      if (msg_buffer[1] == 0) continue;
      /* The VideoCore property protocol's request/response code is not a
       * bitmask - it is one of exactly two values:
       *   0x80000000 = every tag in the buffer was processed successfully
       *   0x80000001 = error parsing the request buffer (a PARTIAL or
       *                failed response - something in the tag was rejected)
       * Both have bit 31 set. This used to check only that bit, so an
       * explicit 0x80000001 error - the firmware telling us in as many
       * words that it did NOT do what was asked - was reported as success.
       * Every mailbox call in this kernel (WL_REG_ON, the framebuffer, the
       * MAC address, USB power) has been trusting this check since before
       * today; whether any of their apparent successes were real is now
       * only as good as this fix. */
      if (msg_buffer[1] != 0x80000000u) {
        /* This printed once per call, unconditionally - a single "wifi
         * init" retry ladder makes up to 5 of these calls per attempt x 5
         * attempts, and on hardware where they all fail identically that
         * is up to 25 near-duplicate lines, several KB of pure repetition.
         * That volume is what was breaking the console's multi-packet
         * replies (6+ UDP chunks, most of which were getting lost in
         * transit) - the fix for THAT bug is to not generate 6 chunks of
         * output in the first place. Collapse identical repeats: print
         * the first occurrence and a count, not every one. */
        static uint32_t s_last_chan = 0xFFFFFFFFu;
        static uint32_t s_last_code = 0xFFFFFFFFu;
        static uint32_t s_repeat = 0;
        if (channel == s_last_chan && msg_buffer[1] == s_last_code) {
          ++s_repeat;
        } else {
          if (s_repeat > 0) {
            kprintf("[mbox]   (^ repeated %u more time(s))\n", static_cast<uint64_t>(s_repeat));
          }
          kprintf("[mbox] chan %u response code 0x%08x (not full success)\n",
                  static_cast<uint64_t>(channel), static_cast<uint64_t>(msg_buffer[1]));
          s_last_chan = channel;
          s_last_code = msg_buffer[1];
          s_repeat = 0;
        }
      }
      return msg_buffer[1] == 0x80000000u;
    }
  }
}

bool rpi_fb_init(uint32_t width, uint32_t height, uint32_t bpp, RpiFbInfo& out) {
  g_mbox_buf[0] = 35 * 4;
  g_mbox_buf[1] = 0; /* Request */

  g_mbox_buf[2] = 0x00048003; /* set phy wh */
  g_mbox_buf[3] = 8;
  g_mbox_buf[4] = 8;
  g_mbox_buf[5] = width;
  g_mbox_buf[6] = height;

  g_mbox_buf[7] = 0x00048004; /* set virt wh */
  g_mbox_buf[8] = 8;
  g_mbox_buf[9] = 8;
  g_mbox_buf[10] = width;
  g_mbox_buf[11] = height;

  g_mbox_buf[12] = 0x00048009; /* set virt offset */
  g_mbox_buf[13] = 8;
  g_mbox_buf[14] = 8;
  g_mbox_buf[15] = 0;
  g_mbox_buf[16] = 0;

  g_mbox_buf[17] = 0x00048005; /* set depth */
  g_mbox_buf[18] = 4;
  g_mbox_buf[19] = 4;
  g_mbox_buf[20] = bpp;

  g_mbox_buf[21] = 0x00048006; /* set pixel order */
  g_mbox_buf[22] = 4;
  g_mbox_buf[23] = 4;
  /* Firmware "BGR" (0): with 32bpp this is the layout whose little-endian
   * words are 0x00RRGGBB, which is how every drawing routine here writes.
   * Requesting "RGB" (1), as this used to, put bytes R,G,B in memory - words
   * 0x00BBGGRR - so the real HDMI output had red and blue swapped (the dark
   * navy desktop showed as brown, the yellow title bars as light blue). The
   * granted value is read back below and fb.cpp compensates if refused. */
  g_mbox_buf[24] = 0;

  g_mbox_buf[25] = 0x00040001; /* allocate buffer */
  g_mbox_buf[26] = 8;
  g_mbox_buf[27] = 4; /* alignment input */
  g_mbox_buf[28] = 4096; /* align */
  g_mbox_buf[29] = 0;

  g_mbox_buf[30] = 0x00040008; /* get pitch */
  g_mbox_buf[31] = 4;
  g_mbox_buf[32] = 0;
  g_mbox_buf[33] = 0;

  g_mbox_buf[34] = 0; /* end tag */

  if (!rpi_mailbox_call(8, g_mbox_buf)) return false;

  const uint32_t bus_addr = g_mbox_buf[28];
  const uint32_t fb_size = g_mbox_buf[29];
  if (bus_addr == 0 || fb_size == 0) return false;

  const uint64_t arm_addr = bus_addr & 0x3FFFFFFFull;
  out.pixels = reinterpret_cast<uint32_t*>(arm_addr);
  out.width = g_mbox_buf[5];
  out.height = g_mbox_buf[6];
  out.pitch = g_mbox_buf[33] ? g_mbox_buf[33] : (out.width * 4);
  out.bpp = bpp;
  out.bus_addr = bus_addr;
  out.arm_addr = arm_addr;
  out.fb_size = fb_size;
  out.pixel_order = g_mbox_buf[24];

  return true;
}

bool rpi_get_mac_address(uint8_t mac[6]) {
  if (!mac) return false;
  g_mbox_buf[0] = 8 * 4;
  g_mbox_buf[1] = 0;          /* Request */
  g_mbox_buf[2] = 0x00010003; /* get mac address tag */
  g_mbox_buf[3] = 6;          /* buffer size */
  g_mbox_buf[4] = 0;          /* req/resp length */
  g_mbox_buf[5] = 0;
  g_mbox_buf[6] = 0;
  g_mbox_buf[7] = 0;          /* end tag */

  if (!rpi_mailbox_call(8, g_mbox_buf)) return false;

  const auto* p = reinterpret_cast<const volatile uint8_t*>(&g_mbox_buf[5]);
  for (int i = 0; i < 6; ++i) mac[i] = p[i];
  return true;
}

bool rpi_get_display_size(uint32_t& w, uint32_t& h) {
  g_mbox_buf[0] = 8 * 4;
  g_mbox_buf[1] = 0;
  g_mbox_buf[2] = 0x00040003; /* get physical (display) width/height */
  g_mbox_buf[3] = 8;
  g_mbox_buf[4] = 0;
  g_mbox_buf[5] = 0;
  g_mbox_buf[6] = 0;
  g_mbox_buf[7] = 0;          /* end tag */
  if (!rpi_mailbox_call(8, g_mbox_buf)) return false;
  w = g_mbox_buf[5]; h = g_mbox_buf[6];
  return w != 0 && h != 0;
}

/* tag 0x00010005 = ARM memory, 0x00010006 = VideoCore memory: {base, size}. */
bool rpi_get_memory_split(uint32_t tag, uint32_t& base, uint32_t& size) {
  g_mbox_buf[0] = 8 * 4;
  g_mbox_buf[1] = 0;
  g_mbox_buf[2] = tag;
  g_mbox_buf[3] = 8;
  g_mbox_buf[4] = 0;
  g_mbox_buf[5] = 0;
  g_mbox_buf[6] = 0;
  g_mbox_buf[7] = 0;
  if (!rpi_mailbox_call(8, g_mbox_buf)) return false;
  base = g_mbox_buf[5]; size = g_mbox_buf[6];
  return size != 0;
}

bool rpi_get_board_revision(uint32_t& out_rev) {
  g_mbox_buf[0] = 7 * 4;
  g_mbox_buf[1] = 0;
  g_mbox_buf[2] = 0x00010002; /* get board revision */
  g_mbox_buf[3] = 4;
  g_mbox_buf[4] = 0;
  g_mbox_buf[5] = 0;
  g_mbox_buf[6] = 0;          /* end tag */
  if (!rpi_mailbox_call(8, g_mbox_buf)) return false;
  out_rev = g_mbox_buf[5];
  return out_rev != 0;
}

bool rpi_set_expander_gpio(uint32_t pin, bool on) {
  /* 1. Send SET_GPIO_CONFIG (0x00038043) to set direction = OUT (1) and initial state.
   *    Matches Linux drivers/gpio/gpio-raspberrypi-exp.c structure:
   *    { u32 gpio, u32 direction, u32 polarity, u32 term_en, u32 term_pull_up, u32 state } */
  g_mbox_buf[0] = 12 * 4;
  g_mbox_buf[1] = 0;          /* Request */
  g_mbox_buf[2] = 0x00038043; /* SET_GPIO_CONFIG */
  g_mbox_buf[3] = 24;         /* Value buffer size: 6 words */
  g_mbox_buf[4] = 24;         /* Request length: 6 words */
  g_mbox_buf[5] = 128 + pin;  /* Expander GPIO pin */
  g_mbox_buf[6] = 1;          /* Direction: 1 = Output */
  g_mbox_buf[7] = 0;          /* Polarity */
  g_mbox_buf[8] = 0;          /* Term enable */
  g_mbox_buf[9] = 0;          /* Term pull-up */
  g_mbox_buf[10] = on ? 1 : 0;/* State */
  g_mbox_buf[11] = 0;         /* End tag */

  bool cfg_ok = rpi_mailbox_call(8, g_mbox_buf);

  /* 2. Also send SET_GPIO_STATE (0x00038041) to ensure the logical level is driven */
  g_mbox_buf[0] = 8 * 4;
  g_mbox_buf[1] = 0;          /* Request */
  g_mbox_buf[2] = 0x00038041; /* SET_GPIO_STATE */
  g_mbox_buf[3] = 8;          /* Value buffer size */
  g_mbox_buf[4] = 8;          /* Request length */
  g_mbox_buf[5] = 128 + pin;  /* Expander GPIOs are numbered from 128 */
  g_mbox_buf[6] = on ? 1 : 0;
  g_mbox_buf[7] = 0;          /* End tag */

  bool state_ok = rpi_mailbox_call(8, g_mbox_buf);
  return cfg_ok || state_ok;
}

bool rpi_get_expander_gpio(uint32_t pin, bool& out_level) {
  /* struct gpio_get_set_state { u32 gpio; u32 state; } - the response
   * OVERWRITES this same 2-word slot, so the value buffer is 8 bytes and
   * needs 2 words reserved (buf[5], buf[6]) regardless of the request only
   * sending gpio. The real end tag goes at buf[7]. This function used to
   * declare a 2-word value buffer but only allocate ONE word for it and
   * then reuse the second word as the end tag - an 8-word message with no
   * end tag in it at all, malformed, correctly rejected by the firmware. */
  g_mbox_buf[0] = 8 * 4;
  g_mbox_buf[1] = 0;          /* Request */
  g_mbox_buf[2] = 0x00030041; /* GET_GPIO_STATE */
  g_mbox_buf[3] = 8;          /* Value buffer size (2 words: gpio, state) */
  g_mbox_buf[4] = 4;          /* Request length (just the gpio number) */
  g_mbox_buf[5] = 128 + pin;  /* gpio */
  g_mbox_buf[6] = 0;          /* state - firmware fills this in */
  g_mbox_buf[7] = 0;          /* End tag */

  if (!rpi_mailbox_call(8, g_mbox_buf)) return false;
  out_level = (g_mbox_buf[6] & 1u) != 0;
  return true;
}

bool rpi_power_set(uint32_t dev_id, bool on) {
  g_mbox_buf[0] = 8 * 4;
  g_mbox_buf[1] = 0;          /* Request */
  g_mbox_buf[2] = 0x00028001; /* Set power state tag */
  g_mbox_buf[3] = 8;          /* Value buffer size */
  g_mbox_buf[4] = 8;          /* Request/response length */
  g_mbox_buf[5] = dev_id;
  g_mbox_buf[6] = (on ? 1 : 0) | 2; /* Bit 0: on/off, Bit 1: wait */
  g_mbox_buf[7] = 0;          /* End tag */

  if (!rpi_mailbox_call(8, g_mbox_buf)) return false;
  return (g_mbox_buf[6] & 1) == (on ? 1 : 0);
}

bool rpi_get_clock_rate(uint32_t clock_id, uint32_t& out_hz) {
  out_hz = 0;
  g_mbox_buf[0] = 8 * 4;
  g_mbox_buf[1] = 0;          /* Request */
  g_mbox_buf[2] = 0x00030002; /* GET_CLOCK_RATE */
  g_mbox_buf[3] = 8;          /* Value buffer size */
  g_mbox_buf[4] = 8;          /* Request length */
  g_mbox_buf[5] = clock_id;
  g_mbox_buf[6] = 0;          /* rate (response) */
  g_mbox_buf[7] = 0;          /* End tag */
  if (!rpi_mailbox_call(8, g_mbox_buf)) return false;
  out_hz = g_mbox_buf[6];
  return true;
}

namespace {
/* One tag, one u32 in (id), one u32 out: GET_MAX_CLOCK_RATE, GET_TEMPERATURE,
 * GET_THROTTLED all have this shape. */
bool mbox_query2(uint32_t tag, uint32_t id, uint32_t& out) {
  out = 0;
  g_mbox_buf[0] = 8 * 4;
  g_mbox_buf[1] = 0;
  g_mbox_buf[2] = tag;
  g_mbox_buf[3] = 8;
  g_mbox_buf[4] = 8;
  g_mbox_buf[5] = id;
  g_mbox_buf[6] = 0;
  g_mbox_buf[7] = 0;
  if (!rpi_mailbox_call(8, g_mbox_buf)) return false;
  out = g_mbox_buf[6];
  return true;
}
}  // namespace

bool rpi_get_max_clock_rate(uint32_t clock_id, uint32_t& out_hz) { return mbox_query2(0x00030004, clock_id, out_hz); }
bool rpi_get_temperature(uint32_t& out_millic) { return mbox_query2(0x00030006, 0, out_millic); }
bool rpi_get_throttled(uint32_t& out_bits) { return mbox_query2(0x00030046, 0, out_bits); }

bool rpi_set_clock_rate(uint32_t clock_id, uint32_t hz) {
  g_mbox_buf[0] = 9 * 4;
  g_mbox_buf[1] = 0;          /* Request */
  g_mbox_buf[2] = 0x00038002; /* SET_CLOCK_RATE */
  g_mbox_buf[3] = 12;         /* Value buffer size */
  g_mbox_buf[4] = 12;         /* Request length */
  g_mbox_buf[5] = clock_id;
  g_mbox_buf[6] = hz;
  g_mbox_buf[7] = 0;          /* skip setting turbo */
  g_mbox_buf[8] = 0;          /* End tag */
  if (!rpi_mailbox_call(8, g_mbox_buf)) return false;
  return g_mbox_buf[6] != 0;
}
