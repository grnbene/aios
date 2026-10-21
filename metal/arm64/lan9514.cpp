/* SMSC LAN9514 USB 2.0 to 10/100 Fast Ethernet Driver (metal/arm64/lan9514.cpp).
 *
 * Implements register control, MAC/PHY initialization, and high-speed
 * packet transmission/reception for Raspberry Pi 3 onboard Ethernet (VID 0x0424, PID 0xEC00).
 */
#include "lan9514.hpp"

#include "dwc2.hpp"
#include "kio.hpp"
#include "net.hpp"
#include "rpi_mailbox.hpp"
#include "timer.hpp"

namespace {

/* LAN95xx register map, as exposed over the vendor control endpoint.
 *
 * These offsets were wrong in every case that mattered. The LAN95xx USB
 * family puts its MAC and MII registers DIRECTLY in this register map at
 * 0x100 and up; the previous code used the LAN9118-style indirect
 * "MAC_CSR_CMD/MAC_CSR_DATA" sequence from a different SMSC part, aimed at
 * 0x100/0x104 - which are actually MAC_CR and ADDRH. So every MAC and PHY
 * access wrote command words into the MAC control and address registers.
 * ID_REV was read from 0x50 (reserved) instead of 0x00, and because init
 * only checked that the USB transfer succeeded rather than what came back,
 * the driver reported success having configured nothing at all. */
constexpr uint32_t ID_REV       = 0x000;
constexpr uint32_t INT_STS      = 0x008;
constexpr uint32_t RX_CFG       = 0x00C;
constexpr uint32_t TX_CFG       = 0x010;
constexpr uint32_t HW_CFG       = 0x014;
constexpr uint32_t PM_CTRL      = 0x020;
constexpr uint32_t LED_GPIO_CFG = 0x024;
constexpr uint32_t AFC_CFG      = 0x02C;
constexpr uint32_t BURST_CAP    = 0x038;
constexpr uint32_t INT_EP_CTL   = 0x068;
constexpr uint32_t BULK_IN_DLY  = 0x06C;

/* MAC / MII block - directly addressable, no indirection */
constexpr uint32_t MAC_CR       = 0x100;
constexpr uint32_t ADDRH        = 0x104;
constexpr uint32_t ADDRL        = 0x108;
constexpr uint32_t MII_ADDR     = 0x114;
constexpr uint32_t MII_DATA     = 0x118;

/* Bit Definitions */
constexpr uint32_t HW_CFG_SRST      = (1u << 0); /* full reset - NOT what init uses, see below */
constexpr uint32_t HW_CFG_LRST      = (1u << 3); /* "Lite Reset": MAC+PHY, not the whole chip */
constexpr uint32_t HW_CFG_BIR       = (1u << 12); /* Bulk-IN empty response */
constexpr uint32_t HW_CFG_RXDOFF    = (3u << 9);  /* RX data offset, in bytes */
constexpr uint32_t PM_CTL_PHY_RST   = (1u << 4);

/* LED_GPIO_CFG: selects each GPIOn pin's alternate function as an LED
 * output instead of a general-purpose GPIO. Without this, the three GPIO
 * pins wired to the RJ45's link/speed/duplex LEDs stay in their power-on
 * GPIO-input default and nothing lights, regardless of link state. */
constexpr uint32_t LED_GPIO_CFG_SPD_LED = (1u << 24); /* GPIO2 -> Speed LED */
constexpr uint32_t LED_GPIO_CFG_LNK_LED = (1u << 20); /* GPIO1 -> Link LED */
constexpr uint32_t LED_GPIO_CFG_FDX_LED = (1u << 16); /* GPIO0 -> Full-Duplex LED */

/* MAC_CR bits. Note the negative logic on some of these: RCVOWN set means
 * "do NOT receive our own transmissions", and BCAST set would DISABLE
 * broadcast reception - which DHCP and ARP both need, so it stays clear. */
constexpr uint32_t MAC_CR_RCVOWN    = (1u << 23);
constexpr uint32_t MAC_CR_RXEN      = (1u << 2);
constexpr uint32_t MAC_CR_TXEN      = (1u << 3);
constexpr uint32_t MAC_CR_MCPAS     = (1u << 19);
constexpr uint32_t MAC_CR_FDPX      = (1u << 20);

constexpr uint32_t TX_CFG_TX_ON     = (1u << 2);

constexpr uint32_t MII_BUSY         = (1u << 0);
constexpr uint32_t MII_WRITE        = (1u << 1);

/* PHY registers (internal PHY is at MII address 1) */
constexpr uint32_t PHY_ADDR         = 1;
constexpr uint32_t PHY_BMCR         = 0;
constexpr uint32_t PHY_BMSR         = 1;
constexpr uint32_t PHY_ANAR         = 4;
constexpr uint32_t PHY_ID1          = 2;
constexpr uint32_t PHY_ID2          = 3;
constexpr uint32_t BMCR_RESET       = (1u << 15);
constexpr uint32_t BMSR_ANEG_DONE   = (1u << 5);
constexpr uint32_t BMCR_ANEG_EN     = (1u << 12);
constexpr uint32_t BMCR_ANEG_RESTART= (1u << 9);
constexpr uint32_t BMCR_SPEED_100   = (1u << 13);
constexpr uint32_t BMCR_FULL_DUPLEX = (1u << 8);
constexpr uint32_t BMSR_LINK_UP     = (1u << 2);

/* RX status word bits. Anything in RX_STS_ERROR means the MAC itself
 * rejected the frame, so there is nothing useful left in it. */
constexpr uint32_t RX_STS_FF        = (1u << 30); /* filter fail */
constexpr uint32_t RX_STS_ES        = (1u << 15); /* error summary */
constexpr uint32_t RX_STS_LE        = (1u << 12); /* length error */
constexpr uint32_t RX_STS_TL        = (1u << 7);  /* frame too long */
constexpr uint32_t RX_STS_CS        = (1u << 6);  /* collision seen */
constexpr uint32_t RX_STS_RW        = (1u << 4);  /* receive watchdog */
constexpr uint32_t RX_STS_ME        = (1u << 3);  /* MII error */
constexpr uint32_t RX_STS_DB        = (1u << 2);  /* dribbling */
constexpr uint32_t RX_STS_CRC       = (1u << 1);  /* CRC error */
/* Runt (bit 11) is deliberately NOT in here: the error summary already
 * covers the cases that matter and a short frame is not by itself a
 * reason to throw the packet away. */
constexpr uint32_t RX_STS_ERROR     = RX_STS_FF | RX_STS_ES | RX_STS_LE | RX_STS_TL |
                                      RX_STS_CS | RX_STS_RW | RX_STS_ME | RX_STS_DB |
                                      RX_STS_CRC;

constexpr uint8_t EP_BULK_IN        = 0x81;
constexpr uint8_t EP_BULK_OUT       = 0x02;

Lan9514Info g_lan = {};

/* Persistent USB DATA0/DATA1 toggle state for the bulk endpoints, per USB
 * 2.0 data toggle synchronization (S8.6). This did not exist until now -
 * every bulk transfer was sent as DATA0, which is a real protocol
 * violation: the device is required to treat every transfer after the
 * first successful one as a retransmitted duplicate of that one and
 * silently discard it rather than process it as new data. That means, in
 * practice, close to nothing this driver ever sent after its first packet
 * actually reached the wire - a "sent fine, nothing ever heard back"
 * failure mode indistinguishable from a network problem.
 * IN and OUT toggle independently; both reset to DATA0 on init, matching
 * the USB spec's rule that configuring (or resetting) an endpoint resets
 * its toggle. */
uint8_t g_tx_toggle = 0;
uint8_t g_rx_toggle = 0;

/* Low-level Register Read / Write over USB Vendor Control transfers */
bool lan_read_reg(uint32_t reg, uint32_t& out_val) {
  uint32_t val = 0;
  bool ok = dwc2_control_msg(g_lan.dev_addr, g_lan.speed, 0xC0 /* Vendor Device IN */,
                             0xA1 /* Get Reg */, 0, static_cast<uint16_t>(reg),
                             &val, 4, 64, g_lan.hub_addr, g_lan.hub_port);
  out_val = val;
  return ok;
}

bool lan_write_reg(uint32_t reg, uint32_t val) {
  return dwc2_control_msg(g_lan.dev_addr, g_lan.speed, 0x40 /* Vendor Device OUT */,
                          0xA0 /* Set Reg */, 0, static_cast<uint16_t>(reg),
                          &val, 4, 64, g_lan.hub_addr, g_lan.hub_port);
}

/* MII / PHY access. MII_ADDR carries the PHY address, register index and a
 * busy flag; the data register is read or written around it. */
bool lan_mii_wait() {
  for (unsigned i = 0; i < 1000; ++i) {
    uint32_t v = 0;
    if (lan_read_reg(MII_ADDR, v) && ((v & MII_BUSY) == 0)) return true;
    metal_delay_us(100);
  }
  return false;
}

bool lan_read_phy_reg(uint32_t phy_addr, uint32_t reg_addr, uint16_t& out_val) {
  if (!lan_mii_wait()) return false;
  uint32_t cmd = ((phy_addr & 0x1Fu) << 11) | ((reg_addr & 0x1Fu) << 6) | MII_BUSY;
  if (!lan_write_reg(MII_ADDR, cmd)) return false;
  if (!lan_mii_wait()) return false;
  uint32_t data = 0;
  if (!lan_read_reg(MII_DATA, data)) return false;
  out_val = static_cast<uint16_t>(data & 0xFFFFu);
  return true;
}

bool lan_write_phy_reg(uint32_t phy_addr, uint32_t reg_addr, uint16_t val) {
  if (!lan_mii_wait()) return false;
  if (!lan_write_reg(MII_DATA, val)) return false;
  uint32_t cmd = ((phy_addr & 0x1Fu) << 11) | ((reg_addr & 0x1Fu) << 6) | MII_WRITE | MII_BUSY;
  if (!lan_write_reg(MII_ADDR, cmd)) return false;
  return lan_mii_wait();
}

}  /* namespace */

namespace {
/* Read the PHY's link bit over USB. Costs roughly eight control transfers. */
bool lan9514_link_from_hw() {
  uint16_t bmsr = 0;
  /* BMSR latches low: read twice so a link that came up since the last poll
   * is reported, not the stale latched-down bit. */
  lan_read_phy_reg(PHY_ADDR, PHY_BMSR, bmsr);
  if (lan_read_phy_reg(PHY_ADDR, PHY_BMSR, bmsr)) {
    return (bmsr & BMSR_LINK_UP) != 0;
  }
  return false;
}
uint64_t g_last_link_check_ns = 0;
}  /* namespace */

bool lan9514_check_link() {
  if (!g_lan.initialized) return false;

  /* Rate limit. net_poll() calls this from the desktop's main loop on EVERY
   * iteration, and each call is ~8 USB control transfers - roughly 24 USB
   * transactions per rendered frame, all synchronous, all ahead of the HID
   * interrupt polls in the same loop. That is what made the mouse and
   * keyboard feel sluggish: they were starved behind Ethernet PHY reads.
   * Link state cannot change meaningfully faster than this. */
  uint64_t now = metal_now_ns();
  if (g_last_link_check_ns != 0 && (now - g_last_link_check_ns) < 500000000ull) {
    return g_lan.link_up;
  }
  g_last_link_check_ns = now;

  g_lan.link_up = lan9514_link_from_hw();
  return g_lan.link_up;
}

bool lan9514_init(uint8_t dev_addr, UsbSpeed speed, uint8_t hub_addr, uint8_t hub_port, const uint8_t mac_override[6]) {
  g_lan = {};
  g_lan.dev_addr = dev_addr;
  g_lan.speed = speed;
  g_lan.hub_addr = hub_addr;
  g_lan.hub_port = hub_port;
  /* SET_CONFIGURATION (already issued by the caller before this runs)
   * resets both bulk endpoints' data toggle to DATA0 - match that here so
   * our tracked state starts in sync with what the device actually has. */
  g_tx_toggle = 0;
  g_rx_toggle = 0;

  /* 1. Read and VALIDATE the chip ID. The upper 16 bits are the chip id;
   *    LAN9512/9514 report 0xEC00. Checking only that the USB transfer
   *    succeeded let a driver pointed at the wrong register offsets report
   *    success while having configured nothing. */
  uint32_t id_rev = 0;
  if (!lan_read_reg(ID_REV, id_rev)) {
    kputs("[eth] LAN9514 ID_REV read failed\n");
    return false;
  }
  g_lan.chip_id = id_rev;
  uint32_t chip = (id_rev >> 16) & 0xFFFFu;
  if (chip == 0x0000 || chip == 0xFFFFu) {
    kprintf("[eth] LAN9514 ID_REV implausible (0x%08x) - register map wrong or device not responding\n",
            static_cast<uint64_t>(id_rev));
    return false;
  }
  kprintf("[eth] LAN95xx chip id 0x%04x rev 0x%04x\n",
          static_cast<uint64_t>(chip), static_cast<uint64_t>(id_rev & 0xFFFFu));

  /* 2. Reset, and wait for it to actually clear.
   *
   * This used to write HW_CFG_SRST (bit 0, the FULL chip reset), and on
   * real hardware the chip read a perfectly valid ID (0xec00 rev 0002) and
   * then hung right here even after the timeout budget was extended 10x.
   * That points at the wrong operation, not just a slow one: the reference
   * smsc95xx Linux
   * driver's own reset routine does NOT use SRST at all - it writes
   * HW_CFG_LRST ("Lite Reset", bit 3: MAC+PHY only), and that is what
   * actually gets used on this exact chip family in production. A full
   * SRST also resets logic SRST does not document clearing on the same
   * timescale on every silicon revision; LRST is the narrower, faster,
   * proven operation. */
  if (!lan_write_reg(HW_CFG, HW_CFG_LRST)) return false;
  bool reset_done = false;
  uint32_t hw_final = 0;
  for (unsigned i = 0; i < 1000; ++i) {
    uint32_t hw = 0;
    if (lan_read_reg(HW_CFG, hw) && ((hw & HW_CFG_LRST) == 0)) { reset_done = true; break; }
    hw_final = hw;
    metal_delay_us(1000);
  }
  if (!reset_done) {
    kprintf("[eth] LAN9514 lite reset did not complete within 1s (HW_CFG=0x%08x)\n",
            static_cast<uint64_t>(hw_final));
    return false;
  }

  /* 3. Determine MAC Address */
  if (mac_override) {
    for (int i = 0; i < 6; ++i) g_lan.mac[i] = mac_override[i];
  } else if (!rpi_get_mac_address(g_lan.mac)) {
    /* Fallback default Raspberry Pi OUI MAC */
    g_lan.mac[0] = 0xB8; g_lan.mac[1] = 0x27; g_lan.mac[2] = 0xEB;
    g_lan.mac[3] = 0x14; g_lan.mac[4] = 0x95; g_lan.mac[5] = 0x14;
  }

  /* 4. Write the MAC address. ADDRL holds bytes 0-3, ADDRH bytes 4-5. */
  uint32_t addrl = g_lan.mac[0] | (static_cast<uint32_t>(g_lan.mac[1]) << 8) |
                   (static_cast<uint32_t>(g_lan.mac[2]) << 16) | (static_cast<uint32_t>(g_lan.mac[3]) << 24);
  uint32_t addrh = g_lan.mac[4] | (static_cast<uint32_t>(g_lan.mac[5]) << 8);
  lan_write_reg(ADDRL, addrl);
  lan_write_reg(ADDRH, addrh);

  /* 5. Bulk-IN behaviour: return a zero-length packet rather than stalling
   *    when there is nothing to read, and cap the burst size. Without BIR
   *    an empty poll hangs the bulk endpoint instead of returning. */
  uint32_t hw_cfg = 0;
  lan_read_reg(HW_CFG, hw_cfg);
  /* RXDOFF must be 0: lan9514_rx_poll reads the frame at a fixed offset of 4
   * bytes (immediately after the RX status word), so any pad the hardware
   * inserts there would shift every received frame. */
  hw_cfg &= ~HW_CFG_RXDOFF;
  lan_write_reg(HW_CFG, hw_cfg | HW_CFG_BIR);
  lan_write_reg(BURST_CAP, 0);
  lan_write_reg(BULK_IN_DLY, 0x2000);
  lan_write_reg(AFC_CFG, 0x00F830A1);
  lan_write_reg(INT_EP_CTL, 0);
  lan_write_reg(INT_STS, 0xFFFFFFFFu);

  /* 6. Force PM_CTRL to its documented power-up default (0), unconditionally,
   * BEFORE trusting anything the PHY reports.
   *
   * This is a plain absolute write, not the read-modify-write that used to
   * be here - it cannot introduce garbage, because it does not depend on
   * whatever PM_CTRL currently contains. That distinction matters: this
   * board does not get a true power-on reset between test runs - a PSCI
   * warm reboot or re-running "net up"/"wifi init" resets the SoC, not the
   * LAN9514's own power rail - so if PM_CTRL's SUS_MODE (bits 6:5) or
   * WOL_EN/ED_EN (bits 3:2) got set by ANY earlier test run, including ones
   * from before this session's fixes existed, that state survives every
   * warm retry since. A suspended PHY still answers MII reads (which is
   * exactly why the MII-unreadable check below would never catch it) while
   * its actual link/PCS logic stays parked - indistinguishable from "no
   * cable" without this. Only a real power cycle, or this write, clears it. */
  lan_write_reg(PM_CTRL, 0);
  metal_delay_us(1000);

  /* Read the PHY. Deliberately do NOT reset or reconfigure it beyond the
   * PM_CTRL clear above.
   *
   * A known-good bare-metal driver for this exact chip on this exact board
   * touches the PHY only to READ link status: it never resets it, never
   * writes BMCR or ANAR, and still links and autonegotiates. That is not an
   * oversight on its part - IEEE 802.3 requires a PHY to come out of
   * power-on reset with autonegotiation already enabled, so the link
   * negotiates itself with no help from us.
   *
   * The PHY reset below is kept only as a RECOVERY step if the MII bus
   * really is unreadable, and then it writes the bare bit rather than
   * OR-ing - the original bug here was reading PM_CTRL and OR-ing in
   * PHY_RST, which could carry forward whatever garbage SUS_MODE/WOL_EN
   * bits were already sitting in the register. */
  uint16_t id1 = 0, id2 = 0;
  auto read_phy_ids = [&]() {
    id1 = 0; id2 = 0;
    const bool ok = lan_read_phy_reg(PHY_ADDR, PHY_ID1, id1) &&
                    lan_read_phy_reg(PHY_ADDR, PHY_ID2, id2);
    const bool plausible = !((id1 == 0x0000 || id1 == 0xFFFF) &&
                             (id2 == 0x0000 || id2 == 0xFFFF));
    return ok && plausible;
  };

  if (!read_phy_ids()) {
    kprintf("[eth] phy id %04x:%04x unreadable - attempting a PHY reset\n",
            static_cast<uint64_t>(id1), static_cast<uint64_t>(id2));
    /* Bare write, NOT read-modify-write: PHY_RST is self-clearing and the
     * other bits in this register must stay at their reset defaults. */
    lan_write_reg(PM_CTRL, PM_CTL_PHY_RST);
    for (unsigned i = 0; i < 5000; ++i) {
      uint32_t v = 0;
      if (lan_read_reg(PM_CTRL, v) && ((v & PM_CTL_PHY_RST) == 0)) break;
      metal_delay_us(200);
    }
    metal_delay_us(2000); /* MDIO is not ready the instant the bit clears */

    if (!read_phy_ids()) {
      kprintf("[eth] MII path still bad after reset: phy id %04x:%04x\n",
              static_cast<uint64_t>(id1), static_cast<uint64_t>(id2));
    } else {
      kprintf("[eth] phy id %04x:%04x (recovered by reset)\n",
              static_cast<uint64_t>(id1), static_cast<uint64_t>(id2));
    }
  } else {
    kprintf("[eth] phy id %04x:%04x\n",
            static_cast<uint64_t>(id1), static_cast<uint64_t>(id2));
  }

  /* 7. Enable the MAC receiver/transmitter and the TX FIFO stream.
   *
   * RCVOWN stops the MAC handing our own transmitted frames back to the RX
   * path. Duplex is deliberately NOT forced here: FDPX used to be set
   * unconditionally, which pins the MAC to full duplex regardless of what
   * autonegotiation actually settled on - a guaranteed duplex mismatch, and
   * a late-collision/CRC-error mess, against any half-duplex link partner. */
  lan_write_reg(MAC_CR, MAC_CR_RCVOWN | MAC_CR_TXEN | MAC_CR_RXEN);
  lan_write_reg(TX_CFG, TX_CFG_TX_ON);
  /* This used to write the unexplained constant 0x70070000 here - no named
   * bits, no comment, unlike every other register in this file - which
   * does not select any of the three GPIOs into LED mode. That is why the
   * jack's LEDs never lit regardless of link state: the pins were left as
   * plain GPIO inputs. */
  lan_write_reg(LED_GPIO_CFG, LED_GPIO_CFG_SPD_LED | LED_GPIO_CFG_LNK_LED | LED_GPIO_CFG_FDX_LED);

  g_lan.initialized = true;

  /* 8. Give autonegotiation a moment. Deliberately short: net_poll() keeps
   *    re-checking, so a link that settles later still comes up without a
   *    reboot, and boot is not held for two seconds waiting on a cable. */
  for (unsigned i = 0; i < 12; ++i) {
    if (lan9514_link_from_hw()) { g_lan.link_up = true; break; }
    metal_delay_us(50000);
  }

  /* Still nothing after 600ms, but the PHY itself is talking? Kick
   * autonegotiation once. This is the ONE PHY write this driver makes, and
   * it is deliberately a restart rather than a reset: restarting
   * autonegotiation is a standard, side-effect-free operation, whereas
   * resetting the PHY is what used to leave the MII bus dead. If the cable
   * is simply unplugged this changes nothing and net_poll() keeps
   * re-checking anyway. */
  if (!g_lan.link_up && id1 != 0x0000 && id1 != 0xFFFF) {
    lan_write_phy_reg(PHY_ADDR, PHY_ANAR, 0x01E1); /* 10/100, half+full, 802.3 */
    lan_write_phy_reg(PHY_ADDR, PHY_BMCR,
                      static_cast<uint16_t>(BMCR_ANEG_EN | BMCR_ANEG_RESTART));
    /* Autonegotiation takes 2-3 s against a normal switch. This waited 1 s,
     * then fell through to forcing a mode - which is why this board ran at
     * a forced 10 Mbit instead of negotiating 100 full. */
    for (unsigned i = 0; i < 80; ++i) {
      if (lan9514_link_from_hw()) { g_lan.link_up = true; break; }
      metal_delay_us(50000);
    }
  }

  /* Autonegotiation genuinely did not complete? Try forcing a mode instead
   * of giving up. A link partner that itself has autoneg disabled, or a
   * flaky negotiation on old hardware, is a real and common reason for
   * this specific failure - forcing a mode is the standard field fix, and
   * this tries every mode a link partner could realistically be fixed at,
   * from the two most common cases down. Each attempt clears BMCR's
   * autoneg-enable bit, so this is a genuinely different code path from
   * the restart above, not the same thing retried. */
  if (!g_lan.link_up && id1 != 0x0000 && id1 != 0xFFFF) {
    struct ForceMode { uint16_t bmcr; const char* name; };
    /* Half duplex first: a switch that autonegotiates falls back to
     * parallel detection against a forced partner, and parallel detection
     * can only ever pick HALF duplex. Forcing full here (the old first
     * choice) guaranteed a duplex mismatch. */
    static const ForceMode modes[] = {
      {BMCR_SPEED_100,                    "100M half"},
      {0,                                 "10M half"},
      {BMCR_SPEED_100 | BMCR_FULL_DUPLEX, "100M full"},
      {BMCR_FULL_DUPLEX,                  "10M full"},
    };
    for (const auto& m : modes) {
      lan_write_phy_reg(PHY_ADDR, PHY_BMCR, m.bmcr);
      for (unsigned i = 0; i < 6; ++i) {
        if (lan9514_link_from_hw()) { g_lan.link_up = true; break; }
        metal_delay_us(50000);
      }
      if (g_lan.link_up) {
        kprintf("[eth] link came up forced to %s (link partner has autoneg off)\n", m.name);
        net_log_eth("link came up forced, not via autoneg");
        break;
      }
    }
    /* None of them worked: restore autoneg rather than leaving the PHY
     * stuck in the last forced mode net_poll() will keep re-checking. */
    if (!g_lan.link_up) {
      lan_write_phy_reg(PHY_ADDR, PHY_BMCR,
                        static_cast<uint16_t>(BMCR_ANEG_EN | BMCR_ANEG_RESTART));
    }
  }
  {
    uint16_t bmcr = 0, bmsr = 0, anlpar = 0;
    lan_read_phy_reg(PHY_ADDR, PHY_BMCR, bmcr);
    lan_read_phy_reg(PHY_ADDR, PHY_BMSR, bmsr);
    lan_read_phy_reg(PHY_ADDR, 5 /* ANLPAR */, anlpar);
    /* Record what the link actually runs at, for the UI - it used to print
     * a constant "100 Mbps Full-Duplex" whatever the PHY said. */
    g_lan.autoneg = (bmcr & BMCR_ANEG_EN) && (bmsr & BMSR_ANEG_DONE);
    if (g_lan.autoneg) {
      uint16_t anar = 0;
      lan_read_phy_reg(PHY_ADDR, PHY_ANAR, anar);
      const uint16_t common = static_cast<uint16_t>(anar & anlpar);
      if (common & 0x0100)      { g_lan.link_mbps = 100; g_lan.full_duplex = true; }
      else if (common & 0x0080) { g_lan.link_mbps = 100; g_lan.full_duplex = false; }
      else if (common & 0x0040) { g_lan.link_mbps = 10;  g_lan.full_duplex = true; }
      else                      { g_lan.link_mbps = 10;  g_lan.full_duplex = false; }
    } else {
      g_lan.link_mbps = (bmcr & BMCR_SPEED_100) ? 100 : 10;
      g_lan.full_duplex = (bmcr & BMCR_FULL_DUPLEX) != 0;
    }
    /* The MAC's duplex must match the PHY's. It was never set, so the MAC
     * always ran half duplex - on a full-duplex link it deferred and
     * "collided" whenever the other end talked, and frames over 1024 bytes
     * never reached the wire (measured: 1024 B frames 100%, 1025 B 0%). */
    if (g_lan.link_up) {
      uint32_t mac_cr = MAC_CR_RCVOWN | MAC_CR_TXEN | MAC_CR_RXEN;
      if (g_lan.full_duplex) mac_cr |= MAC_CR_FDPX;
      lan_write_reg(MAC_CR, mac_cr);
    }
    kprintf("[eth] bmcr %04x bmsr %04x lp %04x aneg %s\n",
            static_cast<uint64_t>(bmcr), static_cast<uint64_t>(bmsr),
            static_cast<uint64_t>(anlpar),
            (bmsr & BMSR_ANEG_DONE) ? "COMPLETE" : "incomplete");
  }
  kprintf("[eth] mac %02x:%02x:%02x:%02x:%02x:%02x link %s\n",
          static_cast<uint64_t>(g_lan.mac[0]), static_cast<uint64_t>(g_lan.mac[1]),
          static_cast<uint64_t>(g_lan.mac[2]), static_cast<uint64_t>(g_lan.mac[3]),
          static_cast<uint64_t>(g_lan.mac[4]), static_cast<uint64_t>(g_lan.mac[5]),
          g_lan.link_up ? "UP" : "DOWN");

  /* Stamp the rate limiter's clock here, now, with the link state this
   * function just spent several hundred ms determining directly (bypassing
   * lan9514_check_link()'s 500ms cache, deliberately, to poll fast during
   * bring-up). Without this, the very next call to lan9514_check_link() -
   * which is exactly what net_init_eth_only() makes moments later, to
   * decide whether to auto-start DHCP - could still be inside a stale
   * 500ms window left over from an unrelated background poll made BEFORE
   * this init even started, while the link was still down. That returns
   * the old cached "down" and skips the auto-DHCP trigger, even though the
   * link is now genuinely up. This is exactly why DHCP needed a manual
   * kick after a successful "net up". */
  g_last_link_check_ns = metal_now_ns();
  return true;
}

bool lan9514_is_up() {
  return g_lan.initialized && g_lan.link_up;
}

const Lan9514Info& lan9514_info() {
  return g_lan;
}

bool lan9514_tx_frame(const void* frame, size_t len) {
  if (!g_lan.initialized || !frame || len < 14 || len > 1518) {
    ++g_lan.stats.tx_errors;
    return false;
  }

  /* Construct LAN9514 TX Buffer: 8-byte Command Header + Payload */
  uint8_t tx_buf[1600] __attribute__((aligned(8)));
  uint32_t tx_cmd_a = (len & 0x7FFu) | (1u << 13) /* First Segment */ | (1u << 12) /* Last Segment */;
  uint32_t tx_cmd_b = (len & 0x7FFu);

  *reinterpret_cast<uint32_t*>(&tx_buf[0]) = tx_cmd_a;
  *reinterpret_cast<uint32_t*>(&tx_buf[4]) = tx_cmd_b;

  const auto* src = static_cast<const uint8_t*>(frame);
  for (size_t i = 0; i < len; ++i) {
    tx_buf[8 + i] = src[i];
  }

  /* This used to pad the transfer out to a 4-byte boundary with trailing
   * zero bytes. A known-working bare-metal driver for this exact chip
   * sends exactly header(8) + payload(len) bytes and nothing more - no
   * alignment padding at all. TX_CMD_A/B both declare the true, unpadded
   * length, so those 1-3 extra bytes we used to send were never accounted
   * for by anything the chip parses against; they were just appended past
   * where the chip's own command header said the frame ended. Every
   * odd-length Ethernet frame - which is most of them, DHCP included -
   * hit this. */
  const size_t total_len = 8 + len;

  int32_t sent = dwc2_bulk_out(g_lan.dev_addr, g_lan.speed, EP_BULK_OUT,
                               tx_buf, static_cast<uint32_t>(total_len), 512,
                               g_lan.hub_addr, g_lan.hub_port, g_tx_toggle);
  if (sent > 0) {
    ++g_lan.stats.tx_packets;
    g_lan.stats.tx_bytes += len;
    return true;
  }

  ++g_lan.stats.tx_errors;
  return false;
}

bool lan9514_rx_poll(void* out_frame, size_t max_len, size_t& out_len) {
  out_len = 0;
  if (!g_lan.initialized || !out_frame) return false;

  uint8_t rx_buf[2048] __attribute__((aligned(8)));
  int32_t bytes = dwc2_bulk_in(g_lan.dev_addr, g_lan.speed, EP_BULK_IN,
                               rx_buf, sizeof(rx_buf), 512,
                               g_lan.hub_addr, g_lan.hub_port, g_rx_toggle);
  if (bytes < 4) return false;

  uint32_t rx_status = *reinterpret_cast<const uint32_t*>(&rx_buf[0]);

  /* Drop frames the MAC already flagged as bad. Without this every CRC
   * error, runt, and filter-fail got handed to the IP stack as if it were
   * a real packet, to be parsed as whatever its corrupted headers claimed. */
  if (rx_status & RX_STS_ERROR) {
    ++g_lan.stats.rx_errors;
    return false;
  }

  /* Frame length in bits 29..16 */
  uint32_t frame_len = (rx_status >> 16) & 0x3FFFu;
  if (frame_len < 14 || frame_len > 1536 || frame_len + 4 > static_cast<uint32_t>(bytes)) {
    return false;
  }

  /* Exclude 4-byte Ethernet FCS checksum if present */
  if (frame_len > 4) frame_len -= 4;

  size_t copy_len = (frame_len < max_len) ? frame_len : max_len;
  auto* dst = static_cast<uint8_t*>(out_frame);
  for (size_t i = 0; i < copy_len; ++i) {
    dst[i] = rx_buf[4 + i];
  }
  out_len = copy_len;

  ++g_lan.stats.rx_packets;
  g_lan.stats.rx_bytes += copy_len;
  return true;
}

/* Burst test: n UDP-sized frames back to back, with the chip's interrupt
 * status (TX data FIFO overrun/underrun, TX error) read before and after -
 * the evidence for where back-to-back frames go missing. */
void lan9514_burst_test(const uint8_t* frame, size_t len, unsigned n) {
  uint32_t before = 0, after = 0;
  lan_write_reg(INT_STS, 0xFFFFFFFFu);
  lan_read_reg(INT_STS, before);
  unsigned ok = 0;
  const uint8_t t0 = g_tx_toggle;
  for (unsigned i = 0; i < n; ++i) if (lan9514_tx_frame(frame, len)) ++ok;
  metal_delay_us(20000);
  lan_read_reg(INT_STS, after);
  kprintf("  burst: %u/%u accepted by USB, toggle %u->%u, INT_STS before 0x%08x after 0x%08x%s%s%s\n",
          static_cast<uint64_t>(ok), static_cast<uint64_t>(n), static_cast<uint64_t>(t0), static_cast<uint64_t>(g_tx_toggle),
          static_cast<uint64_t>(before), static_cast<uint64_t>(after),
          (after & 0x1000u) ? " TDFO(tx fifo overrun)" : "", (after & 0x2000u) ? " TDFU" : "", (after & 0x4000u) ? " TXE" : "");
}

void lan9514_report() {
  kputs("  smsc lan9514 10/100 fast ethernet (eth0):\n");
  kprintf("    status      : %s (link %s)\n",
          g_lan.initialized ? "initialized" : "offline",
          g_lan.link_up ? "UP" : "DOWN");
  kprintf("    mac addr    : %02x:%02x:%02x:%02x:%02x:%02x\n",
          static_cast<uint64_t>(g_lan.mac[0]), static_cast<uint64_t>(g_lan.mac[1]),
          static_cast<uint64_t>(g_lan.mac[2]), static_cast<uint64_t>(g_lan.mac[3]),
          static_cast<uint64_t>(g_lan.mac[4]), static_cast<uint64_t>(g_lan.mac[5]));
  kprintf("    chip id     : 0x%08x\n", static_cast<uint64_t>(g_lan.chip_id));
  kprintf("    packets     : %u tx (%u err), %u rx\n",
          static_cast<uint64_t>(g_lan.stats.tx_packets),
          static_cast<uint64_t>(g_lan.stats.tx_errors),
          static_cast<uint64_t>(g_lan.stats.rx_packets));
}
