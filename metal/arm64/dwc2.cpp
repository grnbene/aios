/* Synopsys DWC2 USB 2.0 Host Controller Driver Implementation (metal/arm64/dwc2.cpp).
 *
 * Buffer-DMA host mode. Everything here is polled: there is no USB interrupt
 * handler, so GAHBCFG's global interrupt enable stays OFF and completion is
 * observed by reading HCINT. See dwc2_exec_transaction for the four core
 * requirements this driver previously violated.
 */
#include "dwc2.hpp"
#include "usb_hid.hpp"

#include "fdt.hpp"
#include "kio.hpp"
#include "mmio.hpp"
#include "rpi_mailbox.hpp"
#include "timer.hpp"

namespace {

/* Register Offsets */
constexpr uint64_t RPI3_DWC2_BASE = 0x3F980000ull;
constexpr uint64_t RPI4_DWC2_BASE = 0xFE980000ull;

/* Core Global Registers */
constexpr uint64_t GOTGCTL        = 0x000;
constexpr uint64_t GAHBCFG        = 0x008;
constexpr uint64_t GUSBCFG        = 0x00C;
constexpr uint64_t GRSTCTL        = 0x010;
constexpr uint64_t GINTSTS        = 0x014;
constexpr uint64_t GINTMSK        = 0x018;
constexpr uint64_t GRXFSIZ        = 0x024;
constexpr uint64_t GNPTXFSIZ      = 0x028;
constexpr uint64_t HPTXFSIZ       = 0x100;

/* Host Mode Registers */
constexpr uint64_t HCFG           = 0x400;
constexpr uint64_t HFIR           = 0x404;
constexpr uint64_t HFNUM          = 0x408;
constexpr uint64_t HAINT          = 0x414;
constexpr uint64_t HAINTMSK       = 0x418;
constexpr uint64_t HPRT0          = 0x440;

/* Host Channel Registers (offset = 0x500 + ch * 0x20) */
inline uint64_t HCCHAR(uint32_t ch)   { return 0x500 + ch * 0x20; }
inline uint64_t HCSPLT(uint32_t ch)   { return 0x504 + ch * 0x20; }
inline uint64_t HCINT(uint32_t ch)    { return 0x508 + ch * 0x20; }
inline uint64_t HCINTMSK(uint32_t ch) { return 0x50C + ch * 0x20; }
inline uint64_t HCTSIZ(uint32_t ch)   { return 0x510 + ch * 0x20; }
inline uint64_t HCDMA(uint32_t ch)    { return 0x514 + ch * 0x20; }

/* Register Bit Definitions */
constexpr uint32_t GRSTCTL_CSFT_RST   = (1u << 0);
constexpr uint32_t GRSTCTL_RXF_FLUSH  = (1u << 4);
constexpr uint32_t GRSTCTL_TXF_FLUSH  = (1u << 5);
constexpr uint32_t GRSTCTL_TXF_ALL    = (0x10u << 6);
constexpr uint32_t GRSTCTL_AHB_IDLE   = (1u << 31);

constexpr uint32_t GAHBCFG_GLBL_INTR_EN = (1u << 0);
constexpr uint32_t GAHBCFG_DMA_EN       = (1u << 5);
/* HBstLen field is GAHBCFG[4:1]; 0x10 is the value Linux's bcm2835 DWC2
 * parameter block programs for this core. */
constexpr uint32_t GAHBCFG_AHBCFG_BCM   = 0x10u;

constexpr uint32_t GUSBCFG_PHY_IF16     = (1u << 3);
constexpr uint32_t GUSBCFG_ULPI_UTMI_SEL= (1u << 4);
constexpr uint32_t GUSBCFG_FORCE_HOST   = (1u << 29);

constexpr uint32_t GINTSTS_CUR_MODE_HOST = (1u << 0);

constexpr uint32_t HCFG_FSLS_PCLK_MASK  = 3u;
constexpr uint32_t HCFG_DESC_DMA        = (1u << 23);

constexpr uint32_t HPRT0_PRT_CONN_STS   = (1u << 0);
constexpr uint32_t HPRT0_PRT_CONN_DET   = (1u << 1);
constexpr uint32_t HPRT0_PRT_ENA        = (1u << 2);
constexpr uint32_t HPRT0_PRT_EN_CHG     = (1u << 3);
constexpr uint32_t HPRT0_PRT_OVRCURR_CHG= (1u << 5);
constexpr uint32_t HPRT0_PRT_RST        = (1u << 8);
constexpr uint32_t HPRT0_PRT_PWR        = (1u << 12);
/* Every one of these is write-1-to-clear; a read-modify-write of HPRT0 that
 * does not mask them off will clear whichever happen to be set, including
 * PrtEna, which DISABLES the port. */
constexpr uint32_t HPRT0_W1C_MASK       = (HPRT0_PRT_CONN_DET | HPRT0_PRT_ENA |
                                           HPRT0_PRT_EN_CHG | HPRT0_PRT_OVRCURR_CHG);

constexpr uint32_t HCCHAR_CH_ENA        = (1u << 31);
constexpr uint32_t HCCHAR_CH_DIS        = (1u << 30);
constexpr uint32_t HCCHAR_ODD_FRM       = (1u << 29);
constexpr uint32_t HCCHAR_EP_DIR_IN     = (1u << 15);
constexpr uint32_t HCCHAR_LSPD_DEV      = (1u << 17);
constexpr uint32_t HCCHAR_MULTI_CNT_1   = (1u << 20);

constexpr uint32_t HCSPLT_SPLT_ENA      = (1u << 31);
constexpr uint32_t HCSPLT_COMP_SPLT     = (1u << 16);
constexpr uint32_t HCSPLT_XACTPOS_ALL   = (3u << 14);

constexpr uint32_t HCINT_XFER_COMPL     = (1u << 0);
constexpr uint32_t HCINT_CH_HALTED      = (1u << 1);
constexpr uint32_t HCINT_AHB_ERR        = (1u << 2);
constexpr uint32_t HCINT_STALL          = (1u << 3);
constexpr uint32_t HCINT_NAK            = (1u << 4);
constexpr uint32_t HCINT_ACK            = (1u << 5);
constexpr uint32_t HCINT_NYET           = (1u << 6);
constexpr uint32_t HCINT_XACT_ERR       = (1u << 7);
constexpr uint32_t HCINT_BBL_ERR        = (1u << 8);
constexpr uint32_t HCINT_FRM_OVRUN      = (1u << 9);
constexpr uint32_t HCINT_DAT_TGL_ERR    = (1u << 10);

constexpr uint32_t HCTSIZ_XFER_SIZE_MASK = 0x7FFFFu;

uint64_t get_dwc2_base() {
  const FdtInfo& f = fdt_info();
  if (f.valid && f.uart_base == 0xFE201000ull) return RPI4_DWC2_BASE;
  return RPI3_DWC2_BASE;
}

Dwc2HostInfo g_host_info = {};

/* Per-channel DMA bounce buffers. 2 KB covers a full Ethernet frame plus the
 * LAN9514's 8-byte RX status header; must be a whole number of cache lines so
 * the invalidate below can never touch a neighbour's dirty data. */
constexpr uint32_t DMA_BUF_SIZE = 2048;
alignas(64) uint8_t g_dma_buf[16][DMA_BUF_SIZE];

inline void dma_flush_range(const void* ptr, uint32_t size) {
  uint64_t addr = reinterpret_cast<uint64_t>(ptr) & ~63ull;
  uint64_t end = reinterpret_cast<uint64_t>(ptr) + size;
  while (addr < end) {
    asm volatile("dc civac, %0" :: "r"(addr) : "memory");
    addr += 64;
  }
  asm volatile("dsb sy" ::: "memory");
}

inline void dma_inv_range(const void* ptr, uint32_t size) {
  uint64_t addr = reinterpret_cast<uint64_t>(ptr) & ~63ull;
  uint64_t end = reinterpret_cast<uint64_t>(ptr) + size;
  while (addr < end) {
    asm volatile("dc ivac, %0" :: "r"(addr) : "memory");
    addr += 64;
  }
  asm volatile("dsb sy" ::: "memory");
}

/* The DWC2 core's internal DMA master sits behind the same VideoCore
 * interconnect as every other BCM283x DMA-capable peripheral (bcm2835.dtsi
 * gives usb@7e980000 dma-ranges <0xc0000000 0 0x3f000000>): it needs the
 * 0xC0000000 L2-cached bus alias, not the raw ARM physical address. */
inline uint32_t to_bus_addr(const void* ptr) {
  return (static_cast<uint32_t>(reinterpret_cast<uint64_t>(ptr)) & 0x3FFFFFFFu) | 0xC0000000u;
}

void dwc2_flush_fifos(uint64_t base) {
  mmio_write32(base + GRSTCTL, GRSTCTL_TXF_FLUSH | GRSTCTL_TXF_ALL);
  for (unsigned i = 0; i < 10000; ++i) {
    if ((mmio_read32(base + GRSTCTL) & GRSTCTL_TXF_FLUSH) == 0) break;
    metal_delay_us(1);
  }
  mmio_write32(base + GRSTCTL, GRSTCTL_RXF_FLUSH);
  for (unsigned i = 0; i < 10000; ++i) {
    if ((mmio_read32(base + GRSTCTL) & GRSTCTL_RXF_FLUSH) == 0) break;
    metal_delay_us(1);
  }
}

bool dwc2_core_reset(uint64_t base) {
  for (unsigned i = 0; i < 10000; ++i) {
    if (mmio_read32(base + GRSTCTL) & GRSTCTL_AHB_IDLE) break;
    metal_delay_us(1);
  }
  mmio_write32(base + GRSTCTL, GRSTCTL_CSFT_RST);
  for (unsigned i = 0; i < 10000; ++i) {
    if ((mmio_read32(base + GRSTCTL) & GRSTCTL_CSFT_RST) == 0) {
      metal_delay_us(100);
      return true;
    }
    metal_delay_us(10);
  }
  return false;
}

/* Bring a host channel to a known-idle state.
 *
 * Writing ChEna to a channel that is ALREADY enabled is undefined on this
 * core. Nothing here used to disable a channel after a timeout or an error,
 * so the first failed transfer left channel 0 enabled forever and every
 * later control transfer was programmed on top of a live channel. That is
 * why a run started out working and then failed en masse. */
void chan_halt(uint64_t base, uint32_t ch) {
  uint32_t hcchar = mmio_read32(base + HCCHAR(ch));
  if (hcchar & HCCHAR_CH_ENA) {
    mmio_write32(base + HCCHAR(ch), hcchar | HCCHAR_CH_ENA | HCCHAR_CH_DIS);
    for (unsigned i = 0; i < 10000; ++i) {
      if ((mmio_read32(base + HCCHAR(ch)) & HCCHAR_CH_ENA) == 0) break;
      metal_delay_us(1);
    }
  }
  mmio_write32(base + HCINT(ch), 0xFFFFFFFFu);
}

enum class XferResult : uint8_t { Complete, Ack, Halted, Nak, Nyet, Stall, Error, Timeout };

/* Poll HCINT for a verdict.
 *
 * ChHltd is NOT a failure indication - it accompanies nearly every outcome,
 * including success - so it can only be read last, and only after checking
 * whether Ack came with it.
 *
 * This matters most for a START-SPLIT. A start-split that the hub accepts
 * asserts Ack and ChHltd and does NOT assert XferCompl, because the transfer
 * is not finished: the device's answer is collected later by the
 * complete-split. Folding that into "Error" made every single split
 * transaction fail on its first phase and retry until it gave up, so every
 * Full/Low-speed device behind the high-speed hub - keyboard, mouse, every
 * SET_ADDRESS - was unreachable, while High-Speed devices, which are never
 * split, worked perfectly. It also inflated the transaction-error count by
 * one per retry.
 *
 * Ack is only reported once the channel has also halted; Ack on its own just
 * means the first packet of a still-running transfer was acknowledged. */
/* ignore_nak: in buffer-DMA mode the core retries a NAKed non-periodic
 * transaction by itself; the NAK bit is only a report. Bailing out on it
 * aborted bulk OUT transfers mid-frame whenever the LAN9514's TX FIFO was
 * momentarily full (every burst at 10 Mbit), so frames were cut short and
 * lost - TCP then crawled at ~20 KB/s on retransmits. */
XferResult chan_wait(uint64_t base, uint32_t ch, uint32_t us_budget, bool ignore_nak = false) {
  for (uint32_t i = 0; i < us_budget; ++i) {
    uint32_t hcint = mmio_read32(base + HCINT(ch));
    if (hcint & HCINT_XFER_COMPL) return XferResult::Complete;
    if (hcint & HCINT_STALL) return XferResult::Stall;
    if (hcint & (HCINT_XACT_ERR | HCINT_BBL_ERR | HCINT_AHB_ERR |
                 HCINT_DAT_TGL_ERR | HCINT_FRM_OVRUN)) return XferResult::Error;
    if (!ignore_nak) {
      if (hcint & HCINT_NYET) return XferResult::Nyet;
      if (hcint & HCINT_NAK) return XferResult::Nak;
    }
    if (hcint & HCINT_CH_HALTED) {
      return (hcint & HCINT_ACK) ? XferResult::Ack : XferResult::Halted;
    }
    metal_delay_us(1);
  }
  return XferResult::Timeout;
}

}  /* namespace */

bool dwc2_host_init() {
  g_host_info = {};
  const uint64_t base = get_dwc2_base();

  /* Power on USB HCD via VideoCore Mailbox on Raspberry Pi */
  rpi_power_set(3 /* RPI_MBOX_DEV_USB_HCD */, true);

  /* No LAN_RUN here: on the Pi 3B the LAN9514 is reset by the USB power
   * domain above, and expander line 0 - which this used to pulse as
   * "LAN_RUN" - is BT_ON, the Bluetooth half of the BCM43438's regulator. */
  metal_delay_us(60000);


  /* 1. Core soft reset */
  if (!dwc2_core_reset(base)) {
    return false;
  }

  /* 2. Select the PHY: UTMI+, 8-bit (what the BCM283x integration wires up).
   *    Changing the PHY selection requires ANOTHER core soft reset before the
   *    core will latch it — without the second reset the core keeps running
   *    its power-on PHY configuration. */
  uint32_t usbcfg = mmio_read32(base + GUSBCFG);
  usbcfg &= ~(GUSBCFG_ULPI_UTMI_SEL | GUSBCFG_PHY_IF16);
  mmio_write32(base + GUSBCFG, usbcfg);
  if (!dwc2_core_reset(base)) {
    return false;
  }

  /* 3. Force host mode and wait for the core to actually report host mode.
   *    The spec requires up to 25 ms for the mode switch to take effect. */
  usbcfg = mmio_read32(base + GUSBCFG);
  usbcfg |= GUSBCFG_FORCE_HOST;
  mmio_write32(base + GUSBCFG, usbcfg);
  for (unsigned i = 0; i < 300; ++i) {
    if (mmio_read32(base + GINTSTS) & GINTSTS_CUR_MODE_HOST) break;
    metal_delay_us(1000);
  }

  /* 4. FIFO layout, in 32-bit words. These are the sizes Linux's bcm2835
   *    DWC2 parameter block uses; the previous 512/256/256 layout left the
   *    periodic TX FIFO overlapping nothing but was far below what the core
   *    needs for high-speed transfers. */
  mmio_write32(base + GRXFSIZ, 774);
  mmio_write32(base + GNPTXFSIZ, (256u << 16) | 774u);
  mmio_write32(base + HPTXFSIZ, (512u << 16) | (774u + 256u));
  dwc2_flush_fifos(base);

  /* 5. Host config: 30/60 MHz PHY clock, buffer DMA (descriptor DMA off) */
  uint32_t hcfg = mmio_read32(base + HCFG);
  hcfg &= ~HCFG_FSLS_PCLK_MASK;
  hcfg &= ~HCFG_DESC_DMA;
  mmio_write32(base + HCFG, hcfg);

  /* 6. Enable the internal DMA engine. Global interrupt enable stays OFF:
   *    this driver is polled and there is no DWC2 interrupt handler, so
   *    letting the core drive its IRQ line has no upside and can wedge the
   *    SoC interrupt controller. */
  mmio_write32(base + GINTMSK, 0);
  mmio_write32(base + GAHBCFG, GAHBCFG_DMA_EN | GAHBCFG_AHBCFG_BCM);
  mmio_write32(base + HAINTMSK, 0xFFFFu);

  /* Leave every channel idle and its interrupt mask fully open. The core
   *    gates some HCINT status bits on HCINTMSK in DMA mode; this register was
   *    never programmed, so completions could go unseen. */
  for (uint32_t ch = 0; ch < 16; ++ch) {
    mmio_write32(base + HCINTMSK(ch), 0xFFFFFFFFu);
    chan_halt(base, ch);
  }

  /* 7. Power the root hub port */
  uint32_t hprt0 = mmio_read32(base + HPRT0);
  hprt0 &= ~HPRT0_W1C_MASK;
  hprt0 |= HPRT0_PRT_PWR;
  mmio_write32(base + HPRT0, hprt0);
  metal_delay_us(100000);

  /* 8. Wait for a device on the root port (the LAN9514 hub is soldered on,
   *    but it comes out of power-on reset at its own pace) */
  for (unsigned i = 0; i < 100; ++i) {
    if (mmio_read32(base + HPRT0) & HPRT0_PRT_CONN_STS) break;
    metal_delay_us(10000);
  }

  hprt0 = mmio_read32(base + HPRT0);
  if (hprt0 & (HPRT0_PRT_CONN_STS | HPRT0_PRT_CONN_DET)) {
    g_host_info.port_connected = true;

    /* Port reset: assert >= 50 ms, then wait for the core to set PrtEna */
    hprt0 = mmio_read32(base + HPRT0);
    hprt0 &= ~HPRT0_W1C_MASK;
    hprt0 |= HPRT0_PRT_RST;
    mmio_write32(base + HPRT0, hprt0);
    metal_delay_us(60000);

    hprt0 = mmio_read32(base + HPRT0);
    hprt0 &= ~HPRT0_W1C_MASK;
    hprt0 &= ~HPRT0_PRT_RST;
    mmio_write32(base + HPRT0, hprt0);

    for (unsigned i = 0; i < 500; ++i) {
      if (mmio_read32(base + HPRT0) & HPRT0_PRT_ENA) break;
      metal_delay_us(1000);
    }
    /* USB 2.0 9.2.6.2: 10 ms reset recovery before the first transaction */
    metal_delay_us(20000);

    hprt0 = mmio_read32(base + HPRT0);
    if (hprt0 & HPRT0_PRT_ENA) {
      g_host_info.port_enabled = true;
      uint32_t spd = (hprt0 >> 17) & 3u;
      if (spd == 0) g_host_info.speed = UsbSpeed::High;
      else if (spd == 1) g_host_info.speed = UsbSpeed::Full;
      else g_host_info.speed = UsbSpeed::Low;

      /* Re-select the PHY clock for the negotiated speed and reload the
       * frame interval to match, or periodic scheduling runs off a clock
       * the bus is not actually using. */
      hcfg = mmio_read32(base + HCFG);
      hcfg &= ~HCFG_FSLS_PCLK_MASK;
      if (g_host_info.speed == UsbSpeed::High) {
        mmio_write32(base + HCFG, hcfg);          /* 0 = 30/60 MHz */
      } else {
        mmio_write32(base + HCFG, hcfg | 1u);     /* 1 = 48 MHz FS/LS */
      }
      /* HFIR is deliberately NOT written. The core loads the correct frame
       * interval for the negotiated speed and PHY clock at reset; a
       * hand-computed value that is even slightly off desynchronises
       * periodic and split scheduling, which is precisely the traffic that
       * depends on microframe boundaries. */
    }
  }

  g_host_info.num_channels = 16;
  g_host_info.initialized = true;
  return true;
}

bool dwc2_is_ready() {
  return g_host_info.initialized;
}

const Dwc2HostInfo& dwc2_host_info() {
  return g_host_info;
}

namespace {

/* Execute one USB transaction on a host channel, in buffer-DMA mode.
 *
 * Four core requirements the previous implementation broke, each on its own
 * sufficient to make every transfer fail:
 *
 *  1. HCCHAR.MultiCnt (bits 21:20) was left at 0. The core treats 0 as an
 *     invalid transaction count / error count and will not schedule the
 *     channel. It must be at least 1.
 *  2. HCINTMSK was never programmed. In DMA mode the core gates status
 *     reporting through it.
 *  3. For an IN transfer, buffer-DMA mode requires XferSize to be an
 *     INTEGRAL number of max-size packets (PktCnt * MaxPacketSize), not the
 *     number of bytes the caller wants. Programming a 4-byte IN against a
 *     64-byte endpoint - which is exactly what every LAN9514 register read
 *     did - is a protocol violation, and the LAN9514 is High-Speed so it
 *     never went anywhere near the split path that earlier passes blamed.
 *     The actual byte count comes back as the HCTSIZ residual.
 *  4. Channels were never disabled after a timeout or an error, so a single
 *     failure poisoned the channel for every subsequent transfer.
 *
 * NAK is also a normal, retryable response here (the device is not ready),
 * not a failure - except on an interrupt endpoint, where it means exactly
 * "no new report this interval" and must return immediately and cheaply.
 */
/* Execute one USB transfer on a host channel, in buffer-DMA mode.
 *
 * Core requirements that were previously broken, each on its own enough to
 * make transfers fail:
 *
 *  1. HCCHAR.MultiCnt (bits 21:20) must be at least 1; 0 is invalid and the
 *     core will not schedule the channel.
 *  2. HCINTMSK must be programmed - in DMA mode the core gates status
 *     reporting through it.
 *  3. An IN transfer must be programmed for an INTEGRAL number of max-size
 *     packets, not the byte count the caller wants. The real count comes
 *     back as the HCTSIZ residual.
 *  4. A channel must be disabled after a timeout or error before it is
 *     reused; writing ChEna to a live channel is undefined.
 *  5. A SPLIT transaction carries exactly ONE packet. PktCnt is 1, an IN is
 *     programmed for a full max-size packet, and the complete-split of an
 *     OUT carries no data. A multi-packet transfer through a hub has to be
 *     issued as one start/complete-split PAIR PER PACKET, carrying the data
 *     toggle forward between them. Programming a whole 18-byte descriptor
 *     read into a single split pair is invalid - which is why everything
 *     behind the internal hub failed while the hub itself, being high speed
 *     and therefore never split, worked perfectly.
 *
 * NAK is a normal retryable response, not a failure - except on an interrupt
 * endpoint, where it means "no report this interval" and must return fast.
 */
void dtrace(const char* tag, int r) {
  char t[24]; size_t n = 0;
  while (*tag && n < 16) t[n++] = *tag++;
  t[n++] = '='; t[n++] = static_cast<char>('0' + (r & 7)); t[n] = 0;
  usb_hid_trace_note(t);
}

bool dwc2_exec_transaction(uint32_t ch, uint8_t dev_addr, uint8_t ep_num,
                           bool is_in, uint8_t ep_type, uint32_t max_pkt,
                           UsbSpeed speed, uint8_t pid, void* data, uint32_t len,
                           uint8_t hub_addr, uint8_t hub_port,
                           uint32_t* out_len, uint8_t* out_pid = nullptr) {
  if (ch >= 16) return false;
  const uint64_t base = get_dwc2_base();
  const bool is_periodic = (ep_type == 1 || ep_type == 3);

  /* Split transactions exist so a HIGH-speed host can reach a Full/Low speed
   * device through a high-speed hub's transaction translator. Two conditions:
   * the device must be slower than high speed AND the hub must be running at
   * high speed. g_host_info.speed is the root port's negotiated speed, which
   * is the speed the internal hub itself is running at. */
  const bool use_split = (hub_addr > 0 && speed != UsbSpeed::High &&
                          g_host_info.speed == UsbSpeed::High);

  if (max_pkt == 0) max_pkt = 8;
  if (max_pkt > DMA_BUF_SIZE) max_pkt = DMA_BUF_SIZE;
  if (len > DMA_BUF_SIZE) len = DMA_BUF_SIZE;

  uint32_t pkt_cnt = (len + max_pkt - 1) / max_pkt;
  if (pkt_cnt == 0) pkt_cnt = 1;

  /* Requirement 3: IN transfers are programmed in whole max-size packets. */
  uint32_t prog_len = len;
  if (is_in) {
    prog_len = pkt_cnt * max_pkt;
    while (prog_len > DMA_BUF_SIZE && pkt_cnt > 1) {
      --pkt_cnt;
      prog_len = pkt_cnt * max_pkt;
    }
    if (prog_len > DMA_BUF_SIZE) prog_len = DMA_BUF_SIZE;
    if (len > prog_len) len = prog_len;
  }

  uint8_t* buf = &g_dma_buf[ch][0];

  if (!is_in) {
    for (uint32_t i = 0; i < prog_len; ++i) {
      buf[i] = (data != nullptr && i < len) ? static_cast<const uint8_t*>(data)[i] : 0;
    }
    dma_flush_range(buf, prog_len ? prog_len : 64);
  } else {
    for (uint32_t i = 0; i < prog_len; ++i) buf[i] = 0;
    dma_flush_range(buf, prog_len);
  }

  const uint32_t dma_bus = to_bus_addr(buf);

  uint32_t hcchar = ((dev_addr & 0x7Fu) << 22) |
                    HCCHAR_MULTI_CNT_1 |              /* requirement 1 */
                    ((static_cast<uint32_t>(ep_type) & 3u) << 18) |
                    ((ep_num & 0x0Fu) << 11) |
                    (is_in ? HCCHAR_EP_DIR_IN : 0u) |
                    (max_pkt & 0x7FFu);
  if (speed == UsbSpeed::Low) hcchar |= HCCHAR_LSPD_DEV;

  const uint32_t splt_base = use_split
      ? (HCSPLT_SPLT_ENA | HCSPLT_XACTPOS_ALL |
         ((hub_addr & 0x7Fu) << 7) | (hub_port & 0x7Fu))
      : 0u;

  /* An interrupt endpoint is polled regularly by the UI loop: allow sufficient
   * microframe retries for low-speed completion without indefinite lockup. */
  const uint32_t max_attempts = is_periodic ? 3u : 20u;
  const uint32_t max_csplits  = is_periodic ? 32u : 40u;
  const uint32_t ssplit_budget = is_periodic ? 1000u : 5000u;
  /* Per-attempt hardware-completion budget for a DIRECT (non-split)
   * transfer. This was a flat 20ms regardless of endpoint type, and it is
   * the dominant cost of every NAK: a High-Speed bulk transaction that
   * genuinely has data to move completes in low microseconds once
   * scheduled, so 20ms is margin for a worst case that essentially never
   * happens, paid in full on every ordinary "nothing to receive yet" poll.
   * dwc2_bulk_in() is used ONLY by lan9514_rx_poll(), called once per
   * net_poll() iteration - at up to 20 attempts x 20ms, a single empty
   * poll could cost 400ms, which starves every other net_poll()/
   * usb_hid_poll() call in the same loop and reads as the whole machine
   * being slow, not just the network. 2ms leaves over 100x margin above a
   * genuine successful completion while cutting that worst case to ~40ms. */
  const uint32_t direct_budget = is_periodic ? 20000u : 2000u;
  /* Bulk OUT waits for the core to finish on its own (NAKs retried in
   * hardware) - up to 25 ms, ~20 full frames at 10 Mbit. */
  const bool out_waits_through_nak = !is_in && !is_periodic && ep_type == 2;

  uint32_t got_total = 0;

  if (!use_split) {
    /* ---------------- Direct (non-split) transfer ---------------- */
    const uint32_t hctsiz = (static_cast<uint32_t>(pid & 3u) << 29) |
                            ((pkt_cnt & 0x3FFu) << 19) |
                            (prog_len & HCTSIZ_XFER_SIZE_MASK);
    bool ok = false;
    for (uint32_t attempt = 0; attempt < max_attempts && !ok; ++attempt) {
      chan_halt(base, ch);
      mmio_write32(base + HCINTMSK(ch), 0xFFFFFFFFu);   /* requirement 2 */

      uint32_t start_char = hcchar;
      if (is_periodic && ((mmio_read32(base + HFNUM) & 1u) == 0)) {
        start_char |= HCCHAR_ODD_FRM;
      }
      mmio_write32(base + HCSPLT(ch), 0);
      mmio_write32(base + HCTSIZ(ch), hctsiz);
      mmio_write32(base + HCDMA(ch), dma_bus);
      mmio_write32(base + HCCHAR(ch), start_char | HCCHAR_CH_ENA);

      XferResult r = out_waits_through_nak ? chan_wait(base, ch, 25000u, true)
                                           : chan_wait(base, ch, direct_budget);
      switch (r) {
        case XferResult::Complete: {
          uint32_t tsiz = mmio_read32(base + HCTSIZ(ch));
          uint32_t residual = tsiz & HCTSIZ_XFER_SIZE_MASK;
          got_total = (prog_len >= residual) ? (prog_len - residual) : 0;
          if (out_pid) *out_pid = static_cast<uint8_t>((tsiz >> 29) & 3u);
          ok = true;
          break;
        }
        case XferResult::Stall:
          ++g_host_info.fail_stall; chan_halt(base, ch); return false;
        case XferResult::Nak:
        case XferResult::Nyet:
          /* On an interrupt endpoint a NAK just means "no new report this
           * interval" - the overwhelmingly common case for an idle keyboard. */
          if (is_periodic) { chan_halt(base, ch); return false; }
          chan_halt(base, ch); metal_delay_us(100);
          break;
        case XferResult::Error: {
          uint32_t hcint = mmio_read32(base + HCINT(ch));
          if (hcint & HCINT_DAT_TGL_ERR) {
            /* Toggle mismatch: resync software toggle with device */
            if (out_pid) *out_pid = static_cast<uint8_t>((pid ^ 2u) & 3u);
          }
          ++g_host_info.fail_xact; chan_halt(base, ch); metal_delay_us(100);
          break;
        }
        case XferResult::Ack:
        case XferResult::Halted:
          /* Channel stopped without completing the transfer. Retry. */
        case XferResult::Timeout:
        default:
          ++g_host_info.fail_timeout; chan_halt(base, ch); metal_delay_us(100);
          break;
      }
    }
    if (!ok) { ++g_host_info.fail_direct; ++g_host_info.fail_nak; chan_halt(base, ch); return false; }

  } else {
    /* ---------------- Split transfer: one packet per split pair --------- */
    uint32_t done = 0;
    uint8_t cur_pid = pid & 3u;
    bool short_pkt = false;

    do {
      uint32_t chunk = len - done;
      if (chunk > max_pkt) chunk = max_pkt;

      /* Requirement 5 */
      const uint32_t ss_len = is_in ? max_pkt : chunk;
      const uint32_t cs_len = is_in ? max_pkt : 0u;
      const uint32_t chunk_dma = dma_bus + done;

      bool ok = false;
      for (uint32_t attempt = 0; attempt < max_attempts && !ok; ++attempt) {
        uint32_t ss_tsiz = (static_cast<uint32_t>(cur_pid) << 29) | (1u << 19) |
                           (ss_len & HCTSIZ_XFER_SIZE_MASK);

        chan_halt(base, ch);
        mmio_write32(base + HCINTMSK(ch), 0xFFFFFFFFu);
        uint32_t start_char = hcchar;
        if (is_periodic && ((mmio_read32(base + HFNUM) & 1u) == 0)) {
          start_char |= HCCHAR_ODD_FRM;
        }
        mmio_write32(base + HCSPLT(ch), splt_base);       /* CompSplt = 0 */
        mmio_write32(base + HCTSIZ(ch), ss_tsiz);
        mmio_write32(base + HCDMA(ch), chunk_dma);
        mmio_write32(base + HCCHAR(ch), start_char | HCCHAR_CH_ENA);

        XferResult r = chan_wait(base, ch, ssplit_budget);
        if (is_periodic) dtrace("ss", static_cast<int>(r));
        g_host_info.last_split_hcint = mmio_read32(base + HCINT(ch));
        g_host_info.last_split_stage = 1;
        if (r == XferResult::Stall) { ++g_host_info.fail_stall; chan_halt(base, ch); return false; }
        if (r == XferResult::Nak && is_periodic) { chan_halt(base, ch); return false; }
        if (r == XferResult::Error) {
          ++g_host_info.fail_xact; chan_halt(base, ch); metal_delay_us(100); continue;
        }
        if (r == XferResult::Timeout) {
          ++g_host_info.fail_timeout; chan_halt(base, ch); metal_delay_us(100); continue;
        }
        /* Ack, Halted, Complete or Nyet: the hub took the start-split.
         * Proceed to the complete-split, which is where the device's
         * answer actually arrives. */

        /* One microframe of settling before asking the hub for the result */
        metal_delay_us(50);

        for (uint32_t cs = 0; cs < max_csplits; ++cs) {
          uint32_t cs_tsiz = (static_cast<uint32_t>(cur_pid) << 29) | (1u << 19) |
                             (cs_len & HCTSIZ_XFER_SIZE_MASK);
          chan_halt(base, ch);
          mmio_write32(base + HCINTMSK(ch), 0xFFFFFFFFu);
          uint32_t cs_char = hcchar;
          if (is_periodic && ((mmio_read32(base + HFNUM) & 1u) == 0)) {
            cs_char |= HCCHAR_ODD_FRM;
          }
          mmio_write32(base + HCSPLT(ch), splt_base | HCSPLT_COMP_SPLT);
          mmio_write32(base + HCTSIZ(ch), cs_tsiz);
          mmio_write32(base + HCDMA(ch), chunk_dma);
          mmio_write32(base + HCCHAR(ch), cs_char | HCCHAR_CH_ENA);

          r = chan_wait(base, ch, 2000u);
          if (is_periodic) dtrace("cs", static_cast<int>(r));
          g_host_info.last_split_hcint = mmio_read32(base + HCINT(ch));
          g_host_info.last_split_stage = 2;

          /* An OUT complete-split carries no data, so the core signals it
           * with Ack rather than XferCompl. Treat both as done and let the
           * HCTSIZ residual say how much actually moved. */
          if (r == XferResult::Complete || r == XferResult::Ack) {
            uint32_t tsiz = mmio_read32(base + HCTSIZ(ch));
            uint32_t residual = tsiz & HCTSIZ_XFER_SIZE_MASK;
            uint32_t got = chunk;
            if (is_in) {
              got = (cs_len >= residual) ? (cs_len - residual) : 0;
              if (got > len - done) got = len - done;
              if (got < max_pkt) short_pkt = true;
              dma_inv_range(buf + done, max_pkt);
            }
            done += got;
            /* Carry the hardware's updated data toggle into the next packet */
            cur_pid = static_cast<uint8_t>((tsiz >> 29) & 3u);
            if (out_pid) *out_pid = cur_pid;
            ok = true;
            break;
          }
          if (r == XferResult::Nyet || r == XferResult::Halted) {
            /* Wait one USB 2.0 microframe interval (~100-125us) before retry */
            metal_delay_us(100);
            continue;
          }
          if (r == XferResult::Stall) { ++g_host_info.fail_stall; chan_halt(base, ch); return false; }
          if (r == XferResult::Nak) {
            if (is_periodic) { chan_halt(base, ch); return false; }
            break;                       /* device busy - redo the start-split */
          }
          if (r == XferResult::Error) {
            uint32_t hcint = mmio_read32(base + HCINT(ch));
            if (hcint & HCINT_DAT_TGL_ERR) {
              cur_pid ^= 2u;
              if (out_pid) *out_pid = cur_pid;
            }
            ++g_host_info.fail_xact;
          } else {
            ++g_host_info.fail_timeout;
          }
          break;                         /* redo the start-split */
        }
        if (!ok) { chan_halt(base, ch); metal_delay_us(100); }
      }
      if (!ok) { ++g_host_info.fail_split; chan_halt(base, ch); return false; }
    } while (done < len && !short_pkt);

    got_total = done;
  }

  if (is_periodic) dtrace("xfer ok", 0);
  if (is_in) {
    dma_inv_range(buf, prog_len);
    if (got_total > len) got_total = len;
    if (data != nullptr) {
      auto* dst = static_cast<uint8_t*>(data);
      for (uint32_t i = 0; i < got_total; ++i) dst[i] = buf[i];
    }
  } else {
    got_total = len;
  }
  if (out_len != nullptr) *out_len = got_total;
  chan_halt(base, ch);
  return true;
}

}  /* namespace */

bool dwc2_control_msg(uint8_t dev_addr, UsbSpeed speed,
                      uint8_t req_type, uint8_t request,
                      uint16_t value, uint16_t index,
                      void* data, uint16_t length,
                      uint32_t max_packet_size,
                      uint8_t hub_addr, uint8_t hub_port) {
  if (!g_host_info.initialized) return false;
  if (max_packet_size == 0) max_packet_size = 8;

  UsbDeviceRequest req;
  req.bmRequestType = req_type;
  req.bRequest = request;
  req.wValue = value;
  req.wIndex = index;
  req.wLength = length;

  /* SETUP stage: PID field value 3 = SETUP */
  if (!dwc2_exec_transaction(0, dev_addr, 0, false, 0, max_packet_size, speed,
                             3, &req, 8, hub_addr, hub_port, nullptr)) {
    ++g_host_info.failed_transfers;
    return false;
  }

  /* DATA stage: always starts at DATA1; the core toggles within the stage */
  if (length > 0 && data != nullptr) {
    bool is_in = (req_type & 0x80) != 0;
    if (!dwc2_exec_transaction(0, dev_addr, 0, is_in, 0, max_packet_size, speed,
                               2 /* DATA1 */, data, length, hub_addr, hub_port, nullptr)) {
      ++g_host_info.failed_transfers;
      return false;
    }
  }

  /* STATUS stage: opposite direction, zero length, DATA1 */
  bool status_in = (req_type & 0x80) == 0;
  if (!dwc2_exec_transaction(0, dev_addr, 0, status_in, 0, max_packet_size, speed,
                             2 /* DATA1 */, nullptr, 0, hub_addr, hub_port, nullptr)) {
    ++g_host_info.failed_transfers;
    return false;
  }

  ++g_host_info.total_transfers;
  return true;
}

int32_t dwc2_interrupt_in(uint8_t dev_addr, UsbSpeed speed,
                          uint8_t ep_addr, void* buf,
                          uint32_t len, uint32_t max_packet_size,
                          uint8_t hub_addr, uint8_t hub_port,
                          uint8_t pid_override,
                          uint8_t* next_pid) {
  if (!g_host_info.initialized) return -1;

  uint32_t got = 0;
  if (dwc2_exec_transaction(1, dev_addr, ep_addr & 0x0Fu, true, 3 /* Interrupt */,
                            max_packet_size, speed, pid_override, buf, len,
                            hub_addr, hub_port, &got, next_pid)) {
    ++g_host_info.total_transfers;
    ++g_host_info.int_ok;
    return static_cast<int32_t>(got);
  }
  ++g_host_info.int_fail;
  return -1;
}

int32_t dwc2_bulk_in(uint8_t dev_addr, UsbSpeed speed,
                     uint8_t ep_addr, void* buf,
                     uint32_t len, uint32_t max_packet_size,
                     uint8_t hub_addr, uint8_t hub_port,
                     uint8_t& toggle) {
  if (!g_host_info.initialized) return -1;

  uint32_t got = 0;
  uint8_t next_pid = toggle;
  if (dwc2_exec_transaction(2, dev_addr, ep_addr & 0x0Fu, true, 2 /* Bulk */,
                            max_packet_size, speed, toggle, buf, len,
                            hub_addr, hub_port, &got, &next_pid)) {
    toggle = next_pid;
    ++g_host_info.total_transfers;
    return static_cast<int32_t>(got);
  }
  /* A failed/timed-out transfer never reached the device, so nothing there
   * advanced its toggle either - leave ours exactly where it was so the
   * next attempt matches what the device still expects. */
  return -1;
}

int32_t dwc2_bulk_out(uint8_t dev_addr, UsbSpeed speed,
                      uint8_t ep_addr, const void* buf,
                      uint32_t len, uint32_t max_packet_size,
                      uint8_t hub_addr, uint8_t hub_port,
                      uint8_t& toggle) {
  if (!g_host_info.initialized) return -1;

  uint32_t got = 0;
  uint8_t next_pid = toggle;
  if (dwc2_exec_transaction(3, dev_addr, ep_addr & 0x0Fu, false, 2 /* Bulk */,
                            max_packet_size, speed, toggle,
                            const_cast<void*>(buf), len, hub_addr, hub_port,
                            &got, &next_pid)) {
    toggle = next_pid;
    ++g_host_info.total_transfers;
    return static_cast<int32_t>(got);
  }
  return -1;
}

void dwc2_report() {
  kputs("  synopsys dwc2 usb host controller:\n");
  kprintf("    status      : %s\n", g_host_info.initialized ? "core reset and host mode entered" : "not initialized");
  kprintf("    root port   : %s (speed: %s)\n",
          g_host_info.port_connected ? (g_host_info.port_enabled ? "connected & enabled" : "connected") : "no device",
          g_host_info.speed == UsbSpeed::High ? "High-Speed 480M" : (g_host_info.speed == UsbSpeed::Full ? "Full-Speed 12M" : "Low-Speed 1.5M"));
  kprintf("    channels    : %u host channels active\n", static_cast<uint64_t>(g_host_info.num_channels));
  kprintf("    transfers   : %u successful, %u failed\n",
          static_cast<uint64_t>(g_host_info.total_transfers),
          static_cast<uint64_t>(g_host_info.failed_transfers));
}
