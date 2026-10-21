/* Broadcom / Cypress BCM43438 Dongle Host Driver (DHD) Protocol Layer (metal/arm64/dhd.cpp).
 * Implements SDPCM packetization, BCDC ioctl dispatch, WLC command engine,
 * and SDIO Function 2 bidirectional data frame transport.
 */
#include "dhd.hpp"

#include "bcm43438.hpp"
#include "kio.hpp"
#include "net.hpp"
#include "sdio.hpp"
#include "timer.hpp"

namespace {

DhdInfo g_dhd = {};
uint16_t g_req_id = 1;

uint8_t g_tx_scratch[DHD_MAX_FRAME] __attribute__((aligned(8)));
uint8_t g_rx_scratch[DHD_MAX_FRAME] __attribute__((aligned(8)));
uint8_t g_ioctl_buf[DHD_MAX_FRAME] __attribute__((aligned(8)));

/* Frames that arrive on the bus while an ioctl is waiting for its reply.
 * They used to be read and thrown away, which is where E_LINK / E_SET_SSID
 * went whenever they landed during dhd_get_bssid(). */
struct PendingFrame {
  uint8_t channel;
  uint16_t len;
  uint8_t data[2048];
};
constexpr unsigned kPendingSlots = 4;
PendingFrame g_pending[kPendingSlots];
unsigned g_pending_head = 0;
unsigned g_pending_count = 0;

size_t kstrlen(const char* s) {
  size_t n = 0;
  while (s && s[n]) ++n;
  return n;
}

void kmemcpy(void* dst, const void* src, size_t n) {
  auto* d = static_cast<uint8_t*>(dst);
  const auto* s = static_cast<const uint8_t*>(src);
  for (size_t i = 0; i < n; ++i) d[i] = s[i];
}

void kmemset(void* dst, int v, size_t n) {
  auto* d = static_cast<uint8_t*>(dst);
  for (size_t i = 0; i < n; ++i) d[i] = static_cast<uint8_t>(v);
}

uint32_t align4(uint32_t v) {
  return (v + 3u) & ~3u;
}

uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

/* Transmit a raw SDPCM packet over SDIO Function 2 */
bool dhd_send_sdpcm(uint8_t channel, uint8_t data_offset, const void* payload, uint32_t payload_len) {
  const uint32_t total_len = align4(data_offset + payload_len);
  if (total_len > sizeof(g_tx_scratch)) return false;

  kmemset(g_tx_scratch, 0, data_offset);

  auto* hdr = reinterpret_cast<SdpcmHeader*>(g_tx_scratch);
  hdr->framelen       = static_cast<uint16_t>(total_len);
  hdr->framelen_check = static_cast<uint16_t>(~total_len);
  hdr->sequence       = g_dhd.tx_seq++;
  hdr->channel_flags  = channel & 0x0Fu;
  hdr->next_len       = 0;
  hdr->data_offset    = data_offset;
  hdr->flow_control   = 0;
  hdr->credit         = 0;
  hdr->reserved       = 0;

  if (payload && payload_len > 0) {
    kmemcpy(g_tx_scratch + data_offset, payload, payload_len);
  }

  /* Under one F2 block: padded single-block CMD53. A real Pi 3 confirmed
   * non-block CMD53 under 512 bytes never raises Transfer Complete and
   * wedges the bus for the next command - the same failure this drove out
   * of the backplane register path (see sdio_cmd53_read_padded). Almost
   * every SDPCM control/ioctl frame is under 512 bytes, so this is not an
   * edge case: it is the common case. */
  bool ok;
  if (total_len >= 512) {
    const uint32_t transfer_bytes = ((total_len + 511u) / 512u) * 512u;
    ok = sdio_cmd53_write(2, 0, g_tx_scratch, transfer_bytes, true, false);
  } else {
    ok = sdio_cmd53_write_padded(2, 0, g_tx_scratch, total_len, false);
  }
  if (ok) {
    g_dhd.tx_packets++;
    g_dhd.tx_bytes += total_len;
  }
  return ok;
}

/* Frames are read only when the dongle says one is waiting, as brcmfmac
 * does: it raises its Function-1 interrupt (CCCR 0x05 bit 1) and sets
 * I_HMB_FRAME_IND in the SDIO core's intstatus. This used to read F2 blindly
 * on every net_poll() pass and every ioctl-reply poll; once Wi-Fi was up,
 * those reads ran constantly inside the loop the desktop, keyboard and mouse
 * share, and the board froze. The check is one CMD52 when nothing is
 * pending. Once indicated, frames are drained until the dongle returns an
 * empty header. */
bool g_frames_indicated = false;

bool dongle_has_frame() {
  if (g_frames_indicated) return true;
  uint8_t pend = 0;
  if (!sdio_cmd52_read(0, 0x05, pend) || !(pend & 0x02u)) return false;   /* no F1 interrupt */
  const uint32_t sdregs = bcm43438_sdio_core_regs();
  if (!sdregs) return false;
  const uint32_t ist = bcm_read32(sdregs + 0x20);                            /* SD intstatus */
  if (ist) bcm_write32(sdregs + 0x20, ist);                                  /* write-1-to-clear */
  if (ist & (1u << 6)) g_frames_indicated = true;                            /* I_HMB_FRAME_IND */
  return g_frames_indicated;
}

/* Receive a raw SDPCM packet over SDIO Function 2 */
bool dhd_recv_sdpcm(void* out_buf, size_t max_len, size_t& out_len, uint8_t& out_channel) {
  out_len = 0;
  out_channel = 0xFF;

  if (!dongle_has_frame()) return false;
  if (!sdio_cmd53_read_padded(2, 0, g_rx_scratch, 512, false)) {
    g_frames_indicated = false;
    return false;
  }

  const auto* hdr = reinterpret_cast<const SdpcmHeader*>(g_rx_scratch);
  const uint16_t framelen = hdr->framelen;
  const uint16_t check    = hdr->framelen_check;

  /* framelen == 0 with an all-zero header is the dongle saying "nothing
   * pending"; anything else that fails the check is a corrupt frame. */
  if (framelen == 0) { g_frames_indicated = false; return false; }
  if (framelen > sizeof(g_rx_scratch) || (framelen ^ check) != 0xFFFFu) {
    g_dhd.rx_errors++;
    kprintf("[dhd] bad SDPCM header: framelen %u check %04x (max %u)\n",
            static_cast<uint64_t>(framelen), static_cast<uint64_t>(check),
            static_cast<uint64_t>(sizeof(g_rx_scratch)));
    return false;
  }

  if (framelen > 512) {
    const uint32_t remaining = framelen - 512;
    const uint32_t aligned_rem = align4(remaining);
    const bool rd_ok = (aligned_rem >= 512)
                           ? sdio_cmd53_read(2, 0, g_rx_scratch + 512, aligned_rem, true, false)
                           : sdio_cmd53_read_padded(2, 0, g_rx_scratch + 512, aligned_rem, false);
    if (!rd_ok) {
      g_dhd.rx_errors++;
      kprintf("[dhd] frame of %u bytes: remainder read (%u) failed\n",
              static_cast<uint64_t>(framelen), static_cast<uint64_t>(aligned_rem));
      return false;
    }
  }

  g_dhd.rx_seq = hdr->sequence;
  if (hdr->credit) {
    g_dhd.tx_credits = hdr->credit;
  }

  out_channel = hdr->channel_flags & 0x0Fu;
  const uint8_t data_offset = hdr->data_offset;

  if (data_offset >= framelen) {
    out_len = 0;
    return true;
  }

  const uint32_t payload_len = framelen - data_offset;
  const size_t copy_len = (payload_len < max_len) ? payload_len : max_len;

  if (out_buf && copy_len > 0) {
    kmemcpy(out_buf, g_rx_scratch + data_offset, copy_len);
  }
  out_len = copy_len;

  g_dhd.rx_packets++;
  g_dhd.rx_bytes += framelen;
  if (out_channel == SDPCM_CHANNEL_EVENT) {
    g_dhd.rx_events++;
  }

  return true;
}

void pending_push(uint8_t channel, const uint8_t* data, size_t len) {
  if (len == 0 || len > sizeof(PendingFrame::data)) return;
  if (g_pending_count == kPendingSlots) {
    g_dhd.rx_dropped++;
    return;
  }
  PendingFrame& f = g_pending[(g_pending_head + g_pending_count) % kPendingSlots];
  f.channel = channel;
  f.len = static_cast<uint16_t>(len);
  kmemcpy(f.data, data, len);
  g_pending_count++;
}

bool pending_pop(uint8_t* out, size_t max_len, size_t& out_len, uint8_t& out_channel) {
  if (g_pending_count == 0) return false;
  PendingFrame& f = g_pending[g_pending_head];
  g_pending_head = (g_pending_head + 1) % kPendingSlots;
  g_pending_count--;
  out_channel = f.channel;
  out_len = (f.len < max_len) ? f.len : max_len;
  if (out && out_len) kmemcpy(out, f.data, out_len);
  return true;
}

/* An event frame is an Ethernet frame (ethertype 0x886C) carrying a BCM
 * header (subtype, length, version, OUI 00:10:18, usr_subtype 1) and then
 * the event message, every multi-byte field in network order. Decode it
 * into a host-order BcmEventMsg or reject it. */
bool decode_event(const uint8_t* raw, size_t raw_len, BcmEventMsg& out) {
  constexpr size_t kEthHdr = 14;
  constexpr size_t kBcmHdr = 10;
  constexpr size_t kMsg = 48;
  if (raw_len < kEthHdr + kBcmHdr + kMsg) return false;
  if (be16(raw + 12) != 0x886Cu) return false;
  const uint8_t* bh = raw + kEthHdr;
  if (bh[5] != 0x00 || bh[6] != 0x10 || bh[7] != 0x18) return false; /* Broadcom OUI */
  if (be16(bh + 8) != 1) return false;                                /* BCMILCP_BCM_SUBTYPE_EVENT */
  const uint8_t* m = bh + kBcmHdr;
  out.version    = be16(m + 0);
  out.flags      = be16(m + 2);
  out.event_type = be32(m + 4);
  out.status     = be32(m + 8);
  out.reason     = be32(m + 12);
  out.auth_type  = be32(m + 16);
  out.datalen    = be32(m + 20);
  kmemcpy(out.addr, m + 24, 6);
  kmemcpy(out.ifname, m + 30, 16);
  out.ifidx      = m[46];
  out.bsscfgidx  = m[47];
  return true;
}

/* Tell the firmware which events to deliver. The mask defaults to nothing;
 * a host that never sets it never hears E_LINK and can only find out it is
 * associated by asking. */
void enable_events() {
  uint8_t mask[16] = {};
  const uint32_t wanted[] = {
    BCM_E_SET_SSID, BCM_E_JOIN, BCM_E_AUTH, BCM_E_DEAUTH, BCM_E_DEAUTH_IND, BCM_E_ASSOC,
    BCM_E_DISASSOC, BCM_E_DISASSOC_IND, BCM_E_LINK, BCM_E_PSK_SUP, BCM_E_SCAN_COMPLETE,
  };
  for (uint32_t e : wanted) mask[e / 8] |= static_cast<uint8_t>(1u << (e % 8));
  if (!dhd_set_var("event_msgs", mask, sizeof(mask))) {
    net_log_wifi("event_msgs rejected - link events will not arrive");
  }
}

/* Download the CLM (regulatory) blob with the "clmload" iovar, the way
 * Linux brcmfmac does (brcmf_c_process_clm_blob): chunks carrying
 * struct { le16 flag; le16 dload_type; le32 len; le32 crc; u8 data[]; },
 * flag = (DLOAD_HANDLER_VER 1 << 12) | DL_BEGIN(0x2) on the first chunk,
 * | DL_END(0x4) on the last; dload_type = DL_TYPE_CLM (2); crc unused (0).
 * Then "clmload_status" must read back 0. */
bool dhd_load_clm() {
  const uint8_t* blob = bcm43438_clm_start;
  const uint32_t total = static_cast<uint32_t>(bcm43438_clm_end - bcm43438_clm_start);
  if (total == 0) { kputs("[wifi] no CLM blob embedded\n"); return false; }

  constexpr uint32_t kChunk = 400;   /* name + header + chunk must fit dhd_set_var's 512-byte buffer */
  uint8_t buf[12 + kChunk];
  uint16_t flag = 0x0002;            /* DL_BEGIN */
  uint32_t off = 0;
  while (off < total) {
    const uint32_t n = (total - off < kChunk) ? (total - off) : kChunk;
    if (off + n >= total) flag |= 0x0004;   /* DL_END */
    const uint16_t f = static_cast<uint16_t>((1u << 12) | flag);
    buf[0] = static_cast<uint8_t>(f); buf[1] = static_cast<uint8_t>(f >> 8);
    buf[2] = 2; buf[3] = 0;                  /* DL_TYPE_CLM */
    buf[4] = static_cast<uint8_t>(n); buf[5] = static_cast<uint8_t>(n >> 8);
    buf[6] = static_cast<uint8_t>(n >> 16); buf[7] = static_cast<uint8_t>(n >> 24);
    buf[8] = buf[9] = buf[10] = buf[11] = 0; /* crc not in use */
    kmemcpy(buf + 12, blob + off, n);
    if (!dhd_set_var("clmload", buf, 12 + n)) {
      kprintf("[wifi] clmload chunk at %u/%u rejected\n", static_cast<uint64_t>(off), static_cast<uint64_t>(total));
      return false;
    }
    flag &= static_cast<uint16_t>(~0x0002u);
    off += n;
  }
  int32_t status = -1;
  const bool st_ok = dhd_get_var("clmload_status", &status, sizeof(status));
  kprintf("[wifi] CLM blob loaded: %u bytes, clmload_status=%d%s\n", static_cast<uint64_t>(total),
          static_cast<int64_t>(status), st_ok ? "" : " (status unreadable)");
  return st_ok && status == 0;
}

}  /* namespace */

bool dhd_init() {
  g_dhd = {};
  g_dhd.tx_credits = 1;
  g_pending_head = 0;
  g_pending_count = 0;

  /* 1. Ensure BCM43438 hardware core and Function 2 are initialized */
  if (!bcm43438_info().initialized || !bcm43438_info().f2_ready) {
    if (!bcm43438_init()) {
      kputs("[wifi] BCM43438 SDIO core did not come up\n");
      net_log_wifi("BCM43438 SDIO core did not come up");
      return false;
    }
  }

  metal_delay_us(10000);

  /* 2. Verify the dongle firmware is actually alive. If the firmware does
   *    not answer, say so here rather than reporting wlan0 READY. */
  uint32_t magic = 0;
  bool magic_ok = dhd_ioctl(WLC_GET_MAGIC, &magic, sizeof(magic), false);
  if (magic_ok) g_dhd.magic = magic;

  uint32_t ver = 0;
  bool ver_ok = dhd_ioctl(WLC_GET_VERSION, &ver, sizeof(ver), false);
  if (ver_ok) g_dhd.version = ver;

  if (!magic_ok && !ver_ok) {
    kputs("[wifi] dongle firmware not responding to IOCTLs - wlan0 unavailable\n");
    net_log_wifi("dongle firmware not responding to IOCTLs");
    return false;
  }
  kprintf("[wifi] dongle magic 0x%x version %u%s\n",
          static_cast<uint64_t>(g_dhd.magic), static_cast<uint64_t>(g_dhd.version),
          (magic_ok && g_dhd.magic != BCM43438_DHD_MAGIC) ? " (magic is not WLC_IOCTL_MAGIC)" : "");
  net_log_wifi("dongle firmware responding to IOCTLs");

  /* 3. No bus glomming, and the events we act on. */
  {
    char ver[128] = {};
    if (dhd_get_var("ver", ver, sizeof(ver) - 1)) {
      for (unsigned i = 0; i < sizeof(ver); ++i) if (ver[i] == '\n' || ver[i] == '\r') ver[i] = ' ';
      kprintf("[wifi] firmware: %s\n", ver);
    }
  }
  /* Regulatory data must be in place before the radio can come up. */
  dhd_load_clm();
  dhd_set_var_int("bus:txglom", 0);
  dhd_set_var_int("ampdu_ba_wsize", 8);
  enable_events();

  /* 4. Read active MAC address. No fallback: an address the dongle did not
   *    report is not this board's address. */
  bool mac_ok = dhd_get_var("cur_etheraddr", g_dhd.mac, 6);
  if (!mac_ok) kmemset(g_dhd.mac, 0, 6);

  /* 5. Bring the radio PHY up. The regulatory country is NOT set here:
   *    net_init() applies a config-provided country if wifi.cfg has one. */
  /* Radio policy before WLC_UP, as the Plan 9 ether4330 driver does on this
   * exact board: minimum-power-consumption off (with mpc on, the firmware
   * keeps the radio down while unassociated and a direct WLC_SCAN answers
   * BCME_NOTUP - seen live on firmware 7.45.98.118), power-save off,
   * infrastructure mode. */
  if (!dhd_set_var_int("mpc", 0)) kputs("[wifi] mpc=0 rejected\n");
  {
    uint32_t pm = 0;      /* PM_OFF */
    dhd_ioctl(86 /* WLC_SET_PM */, &pm, sizeof(pm), true);
    uint32_t infra = 1;
    dhd_ioctl(WLC_SET_INFRA, &infra, sizeof(infra), true);
  }
  if (!dhd_up()) kputs("[wifi] WLC_UP rejected - radio not up; scan/join will fail\n");

  /* 6. Re-read the MAC now the PHY is up - adopt it only if the query
   *    actually succeeded and returned something. */
  {
    uint8_t fresh[6] = {};
    if (dhd_get_var("cur_etheraddr", fresh, 6)) {
      bool nonzero = false;
      for (int i = 0; i < 6; ++i) if (fresh[i]) { nonzero = true; break; }
      if (nonzero) {
        for (int i = 0; i < 6; ++i) g_dhd.mac[i] = fresh[i];
        mac_ok = true;
      }
    }
  }

  kprintf("[wifi] mac %02x:%02x:%02x:%02x:%02x:%02x%s\n",
          static_cast<uint64_t>(g_dhd.mac[0]), static_cast<uint64_t>(g_dhd.mac[1]),
          static_cast<uint64_t>(g_dhd.mac[2]), static_cast<uint64_t>(g_dhd.mac[3]),
          static_cast<uint64_t>(g_dhd.mac[4]), static_cast<uint64_t>(g_dhd.mac[5]),
          mac_ok ? "" : " (cur_etheraddr iovar failed)");

  g_dhd.initialized = true;
  return true;
}

bool dhd_is_ready() {
  return g_dhd.initialized;
}

const DhdInfo& dhd_info() {
  return g_dhd;
}

bool dhd_ioctl(uint32_t cmd, void* buf, uint32_t len, bool write) {
  const uint16_t req_id = g_req_id++;
  g_dhd.ioctl_count++;

  const uint32_t bcdc_len = sizeof(BcdcHeader) + len;
  if (bcdc_len > sizeof(g_ioctl_buf)) return false;

  auto* req = reinterpret_cast<BcdcHeader*>(g_ioctl_buf);
  req->cmd    = cmd;
  req->len    = len;
  req->flags  = (static_cast<uint32_t>(req_id) << BCDC_FLAG_ID_SHIFT) | (write ? BCDC_FLAG_SET : 0);
  req->status = 0;

  if (buf && len > 0) {
    kmemcpy(g_ioctl_buf + sizeof(BcdcHeader), buf, len);
  }

  /* The request frame is capped at 1518 bytes (brcmfmac
   * BRCMF_TX_IOCTL_MAX_MSG_SIZE) while the header's len still tells the
   * dongle how big a reply buffer we have. A 4 KB WLC_SCAN_RESULTS request
   * sent whole overflowed the dongle's control receive buffer and was
   * dropped silently - no reply, no error - on the real board. */
  const uint32_t send_len = bcdc_len < 1518u ? bcdc_len : 1518u;
  if (!dhd_send_sdpcm(SDPCM_CHANNEL_CONTROL, SDPCM_HEADER_LEN, g_ioctl_buf, send_len)) {
    g_dhd.ioctl_errors++;
    kprintf("[dhd] ioctl %u: request frame (%u bytes) not sent over F2\n",
            static_cast<uint64_t>(cmd), static_cast<uint64_t>(bcdc_len));
    return false;
  }

  /* Wait for the matching BCDC reply. Anything else that arrives meanwhile
   * is queued for dhd_rx_poll, not discarded. */
  for (unsigned attempt = 0; attempt < 400; ++attempt) {
    /* Fallback while (and only while) waiting for a reply: if the F1
     * interrupt indication never fires, still look every 25 ms, so a
     * missing interrupt degrades to slow instead of to no replies. */
    if (attempt % 25 == 24) g_frames_indicated = true;
    size_t resp_len = 0;
    uint8_t resp_channel = 0xFF;

    if (dhd_recv_sdpcm(g_ioctl_buf, sizeof(g_ioctl_buf), resp_len, resp_channel)) {
      if (resp_channel == SDPCM_CHANNEL_CONTROL && resp_len >= sizeof(BcdcHeader)) {
        const auto* resp = reinterpret_cast<const BcdcHeader*>(g_ioctl_buf);
        const uint16_t resp_id = static_cast<uint16_t>(resp->flags >> BCDC_FLAG_ID_SHIFT);
        if (resp_id != req_id) continue; /* a stale reply to an earlier, timed-out request */
        if (buf && len > 0 && resp_len > sizeof(BcdcHeader)) {
          const size_t avail = resp_len - sizeof(BcdcHeader);
          kmemcpy(buf, g_ioctl_buf + sizeof(BcdcHeader), avail < len ? avail : len);
        }
        const bool ok = (resp->flags & BCDC_FLAG_ERROR) == 0 && resp->status == 0;
        if (!ok) {
          g_dhd.ioctl_errors++;
          /* The firmware's own reason (bcmutils.h BCME_*: -1 error, -2 bad
           * arg, -4 NOTUP, -5 NOTDOWN, -9 RADIOOFF, -14 BUFTOOSHORT, -16 BUSY,
           * -17 NOTASSOCIATED, -23 UNSUPPORTED, -24 BADLEN). Without this a
           * rejected command is just "false". */
          const char* var = (cmd == WLC_GET_VAR || cmd == WLC_SET_VAR) && buf
                                ? static_cast<const char*>(buf) : "";
          kprintf("[dhd] ioctl %u%s%s %s rejected: status %d flags 0x%x\n",
                  static_cast<uint64_t>(cmd), var[0] ? " " : "", var, write ? "set" : "get",
                  static_cast<int64_t>(static_cast<int32_t>(resp->status)),
                  static_cast<uint64_t>(resp->flags & 0xFFFFu));
        }
        return ok;
      }
      if (resp_channel != SDPCM_CHANNEL_CONTROL && resp_len > 0) {
        pending_push(resp_channel, g_ioctl_buf, resp_len);
      }
      continue; /* something arrived: look again immediately */
    }
    metal_delay_us(1000);
  }

  g_dhd.ioctl_errors++;
  kprintf("[dhd] ioctl %u: no reply within the wait window (rx errors so far %u)\n",
          static_cast<uint64_t>(cmd), static_cast<uint64_t>(g_dhd.rx_errors));
  return false;
}

bool dhd_get_var(const char* name, void* buf, uint32_t len) {
  if (!name) return false;
  const size_t name_len = kstrlen(name);
  uint8_t io_buf[512];
  const size_t total_len = name_len + 1 + len;
  if (total_len > sizeof(io_buf)) return false;

  kmemcpy(io_buf, name, name_len + 1);
  kmemset(io_buf + name_len + 1, 0, len);

  if (!dhd_ioctl(WLC_GET_VAR, io_buf, static_cast<uint32_t>(total_len), false)) {
    return false;
  }

  /* The reply overwrites the buffer from offset 0 with the value. */
  if (buf && len > 0) {
    kmemcpy(buf, io_buf, len);
  }
  return true;
}

bool dhd_set_var(const char* name, const void* buf, uint32_t len) {
  if (!name) return false;
  const size_t name_len = kstrlen(name);
  uint8_t io_buf[512];
  const size_t total_len = name_len + 1 + len;
  if (total_len > sizeof(io_buf)) return false;

  kmemcpy(io_buf, name, name_len + 1);
  if (buf && len > 0) {
    kmemcpy(io_buf + name_len + 1, buf, len);
  }

  return dhd_ioctl(WLC_SET_VAR, io_buf, static_cast<uint32_t>(total_len), true);
}

bool dhd_set_var_int(const char* name, int32_t val) {
  return dhd_set_var(name, &val, sizeof(val));
}

bool dhd_up() {
  return dhd_ioctl(WLC_UP, nullptr, 0, true);
}

bool dhd_get_ifname(char out[16]) {
  if (!out) return false;
  uint8_t buf[16] = {};
  if (dhd_get_var("ifname", buf, sizeof(buf)) && buf[0]) {
    kmemcpy(out, buf, sizeof(buf));
    out[15] = '\0';
    return true;
  }
  return false;
}

bool dhd_down() {
  return dhd_ioctl(WLC_DOWN, nullptr, 0, true);
}

bool dhd_get_mac(uint8_t mac[6]) {
  if (dhd_get_var("cur_etheraddr", mac, 6)) {
    kmemcpy(g_dhd.mac, mac, 6);
    return true;
  }
  kmemcpy(mac, g_dhd.mac, 6);
  return false;
}

bool dhd_set_mac(const uint8_t mac[6]) {
  if (dhd_set_var("cur_etheraddr", mac, 6)) {
    kmemcpy(g_dhd.mac, mac, 6);
    return true;
  }
  return false;
}

bool dhd_get_ssid(char ssid[33]) {
  WlcSsid s = {};
  if (!dhd_ioctl(WLC_GET_SSID, &s, sizeof(s), false)) {
    ssid[0] = '\0';
    return false;
  }
  const uint32_t len = (s.len < 32) ? s.len : 32;
  kmemcpy(ssid, s.ssid, len);
  ssid[len] = '\0';
  return true;
}

bool dhd_set_ssid(const char* ssid) {
  if (!ssid) return false;
  WlcSsid s = {};
  s.len = static_cast<uint32_t>(kstrlen(ssid));
  if (s.len > 32) s.len = 32;
  kmemcpy(s.ssid, ssid, s.len);
  return dhd_ioctl(WLC_SET_SSID, &s, sizeof(s), true);
}

bool dhd_get_bssid(uint8_t bssid[6]) {
  return dhd_ioctl(WLC_GET_BSSID, bssid, 6, false);
}

bool dhd_get_channel(uint32_t& channel) {
  /* channel_info_t: hw_channel, target_channel, scan_channel. */
  int32_t ci[3] = {};
  channel = 0;
  if (!dhd_ioctl(WLC_GET_CHANNEL, ci, sizeof(ci), false)) return false;
  channel = static_cast<uint32_t>(ci[0]);
  return true;
}

bool dhd_set_channel(uint32_t channel) {
  return dhd_ioctl(WLC_SET_CHANNEL, &channel, sizeof(channel), true);
}

bool dhd_get_rssi(int32_t& rssi) {
  /* scb_val_t: the station address (zero = the AP we are joined to) and
   * the value the firmware fills in. */
  struct __attribute__((packed)) ScbVal {
    uint8_t ea[6];
    int32_t val;
  } scb = {};
  rssi = 0;
  if (!dhd_ioctl(WLC_GET_RSSI, &scb, sizeof(scb), false)) return false;
  rssi = scb.val;
  return true;
}

bool dhd_set_promisc(bool enable) {
  uint32_t p = enable ? 1 : 0;
  return dhd_ioctl(WLC_SET_PROMISC, &p, sizeof(p), true);
}

bool dhd_set_country(const char* country_code) {
  if (!country_code) return false;
  /* wl_country_t: country_abbrev[4], rev, ccode[4]. rev -1 = firmware picks. */
  struct __attribute__((packed)) WlCountry {
    char country_abbrev[4];
    int32_t rev;
    char ccode[4];
  } cc = {};
  const size_t len = kstrlen(country_code);
  for (size_t i = 0; i < len && i < 3; ++i) {
    cc.country_abbrev[i] = country_code[i];
    cc.ccode[i] = country_code[i];
  }
  cc.rev = -1;
  return dhd_set_var("country", &cc, sizeof(cc));
}

bool dhd_disassoc() {
  return dhd_ioctl(WLC_DISASSOC, nullptr, 0, true);
}

bool dhd_scan(const char* ssid) {
  WlcScanParams params = {};
  if (ssid && ssid[0]) {
    params.ssid.len = static_cast<uint32_t>(kstrlen(ssid));
    if (params.ssid.len > 32) params.ssid.len = 32;
    kmemcpy(params.ssid.ssid, ssid, params.ssid.len);
  }
  kmemset(params.bssid, 0xFF, 6); /* any BSSID */
  params.bss_type = 2;            /* DOT11_BSSTYPE_ANY */
  params.scan_type = 0;           /* active */
  params.nprobes = -1;
  params.active_time = -1;
  params.passive_time = -1;
  params.home_time = -1;
  params.channel_num = 0;         /* every channel the country allows */

  return dhd_ioctl(WLC_SCAN, &params, sizeof(params), true);
}

bool dhd_get_scan_results(void* out_buf, size_t max_len) {
  if (!out_buf || max_len < sizeof(WlcScanResults)) return false;
  if (max_len > DHD_MAX_FRAME - SDPCM_HEADER_LEN - sizeof(BcdcHeader)) {
    max_len = DHD_MAX_FRAME - SDPCM_HEADER_LEN - sizeof(BcdcHeader);
  }
  auto* res = static_cast<WlcScanResults*>(out_buf);
  res->buflen = static_cast<uint32_t>(max_len);
  res->version = 0;
  res->count = 0;
  return dhd_ioctl(WLC_SCAN_RESULTS, out_buf, static_cast<uint32_t>(max_len), false);
}

bool dhd_set_passphrase(const char* passphrase) {
  bool ok = true;
  if (!passphrase || passphrase[0] == '\0') {
    /* Open network */
    uint32_t zero = 0;
    ok &= dhd_ioctl(WLC_SET_WSEC, &zero, sizeof(zero), true);
    zero = 0;
    ok &= dhd_ioctl(WLC_SET_WPA_AUTH, &zero, sizeof(zero), true);
    zero = 0;
    ok &= dhd_ioctl(WLC_SET_AUTH, &zero, sizeof(zero), true);
    dhd_set_var_int("sup_wpa", 0);
    return ok;
  }

  /* WPA2-PSK with the firmware's own supplicant running the 4-way
   * handshake. wsec 4 = AES/CCMP, wpa_auth 0x80 = WPA2_AUTH_PSK, auth 0 =
   * open system. The passphrase goes in through WLC_SET_WSEC_PMK as
   * wsec_pmk_t; there is no "wpa_psk" iovar. */
  uint32_t wsec = 4, wpa_auth = 0x80, auth = 0;
  ok &= dhd_ioctl(WLC_SET_AUTH, &auth, sizeof(auth), true);
  ok &= dhd_ioctl(WLC_SET_WSEC, &wsec, sizeof(wsec), true);
  ok &= dhd_ioctl(WLC_SET_WPA_AUTH, &wpa_auth, sizeof(wpa_auth), true);
  if (!dhd_set_var_int("sup_wpa", 1)) {
    net_log_wifi("sup_wpa rejected - firmware has no internal supplicant");
    ok = false;
  }

  struct __attribute__((packed)) WsecPmk {
    uint16_t key_len;
    uint16_t flags;   /* 1 = WSEC_PASSPHRASE (ASCII, 8..63 chars); 0 = 64 hex digit PMK */
    /* wsec_pmk_t: key is 2*WSEC_MAX_PSK_LEN+1 = 129 bytes (brcmfmac
     * brcmf_wsec_pmk_le); the whole struct is 132. It was 64 here, so the
     * firmware got a 68-byte structure and the passphrase was rejected. */
    uint8_t key[129];
  } pmk = {};

  size_t len = kstrlen(passphrase);
  if (len > 64) len = 64;
  for (size_t i = 0; i < len; ++i) pmk.key[i] = static_cast<uint8_t>(passphrase[i]);
  pmk.key_len = static_cast<uint16_t>(len);
  pmk.flags = (len == 64) ? 0 : 1;

  if (!dhd_ioctl(WLC_SET_WSEC_PMK, &pmk, sizeof(pmk), true)) {
    net_log_wifi("WLC_SET_WSEC_PMK rejected");
    ok = false;
  }
  return ok;
}

bool dhd_tx_frame(const void* frame, size_t len) {
  if (!frame || len == 0 || len > 1518) return false;

  uint8_t payload[1536];
  if (sizeof(BcdcDataHeader) + len > sizeof(payload)) return false;

  auto* bcdc = reinterpret_cast<BcdcDataHeader*>(payload);
  bcdc->flags = static_cast<uint8_t>(BCDC_PROTO_VER << BCDC_FLAG_VER_SHIFT);
  bcdc->priority = 0;
  bcdc->flags2 = 0; /* ifidx 0 */
  bcdc->data_offset = 0;

  kmemcpy(payload + sizeof(BcdcDataHeader), frame, len);

  return dhd_send_sdpcm(SDPCM_CHANNEL_DATA, SDPCM_DATA_OFFSET, payload, sizeof(BcdcDataHeader) + len);
}

bool dhd_rx_poll(void* out_frame, size_t max_len, size_t& out_len, uint8_t& out_channel) {
  out_len = 0;
  out_channel = 0xFF;

  static uint8_t raw_payload[DHD_MAX_FRAME] __attribute__((aligned(8)));
  size_t raw_len = 0;
  uint8_t channel = 0xFF;

  if (!pending_pop(raw_payload, sizeof(raw_payload), raw_len, channel) &&
      !dhd_recv_sdpcm(raw_payload, sizeof(raw_payload), raw_len, channel)) {
    return false;
  }

  out_channel = channel;

  if (channel == SDPCM_CHANNEL_DATA) {
    if (raw_len <= sizeof(BcdcDataHeader)) return false;
    const auto* bcdc = reinterpret_cast<const BcdcDataHeader*>(raw_payload);
    const size_t skip = sizeof(BcdcDataHeader) + static_cast<size_t>(bcdc->data_offset) * 4u;
    if (raw_len <= skip) return false;
    const size_t frame_len = raw_len - skip;
    const size_t copy_len = (frame_len < max_len) ? frame_len : max_len;
    if (out_frame && copy_len > 0) {
      kmemcpy(out_frame, raw_payload + skip, copy_len);
    }
    out_len = copy_len;
    return true;
  }

  if (channel == SDPCM_CHANNEL_EVENT) {
    /* Events also carry a BCDC data header in front of the Ethernet frame. */
    if (raw_len <= sizeof(BcdcDataHeader)) return false;
    const auto* bcdc = reinterpret_cast<const BcdcDataHeader*>(raw_payload);
    const size_t skip = sizeof(BcdcDataHeader) + static_cast<size_t>(bcdc->data_offset) * 4u;
    if (raw_len <= skip) return false;
    BcmEventMsg msg = {};
    if (!decode_event(raw_payload + skip, raw_len - skip, msg)) {
      g_dhd.rx_errors++;
      return false;
    }
    const size_t copy_len = (sizeof(msg) < max_len) ? sizeof(msg) : max_len;
    if (out_frame && copy_len > 0) kmemcpy(out_frame, &msg, copy_len);
    out_len = copy_len;
    return true;
  }

  if (channel == SDPCM_CHANNEL_CONTROL) {
    const size_t copy_len = (raw_len < max_len) ? raw_len : max_len;
    if (out_frame && copy_len > 0) {
      kmemcpy(out_frame, raw_payload, copy_len);
    }
    out_len = copy_len;
    return true;
  }

  return false;
}

void dhd_report() {
  char b1[32], b2[32];
  kputs("  dhd dongle host driver:\n");
  kprintf("    status     : %s\n", g_dhd.initialized ? "firmware answering ioctls" : "not initialized");
  if (g_dhd.initialized) {
    kprintf("    mac addr   : %02x:%02x:%02x:%02x:%02x:%02x\n",
            static_cast<uint64_t>(g_dhd.mac[0]), static_cast<uint64_t>(g_dhd.mac[1]),
            static_cast<uint64_t>(g_dhd.mac[2]), static_cast<uint64_t>(g_dhd.mac[3]),
            static_cast<uint64_t>(g_dhd.mac[4]), static_cast<uint64_t>(g_dhd.mac[5]));
    kprintf("    protocol   : SDPCM framing over SDIO F2, BCDC ioctl\n");
    kprintf("    dhd magic  : 0x%08x (seq tx=%u, rx=%u, credits=%u)\n",
            static_cast<uint64_t>(g_dhd.magic),
            static_cast<uint64_t>(g_dhd.tx_seq),
            static_cast<uint64_t>(g_dhd.rx_seq),
            static_cast<uint64_t>(g_dhd.tx_credits));
    kprintf("    ioctls     : %u dispatched, %u failed\n",
            static_cast<uint64_t>(g_dhd.ioctl_count),
            static_cast<uint64_t>(g_dhd.ioctl_errors));
    kprintf("    tx frames  : %u packets (%s)\n",
            static_cast<uint64_t>(g_dhd.tx_packets),
            khuman(g_dhd.tx_bytes, b1, sizeof(b1)));
    kprintf("    rx frames  : %u packets (%s, %u events, %u errors, %u dropped mid-ioctl)\n",
            static_cast<uint64_t>(g_dhd.rx_packets),
            khuman(g_dhd.rx_bytes, b2, sizeof(b2)),
            static_cast<uint64_t>(g_dhd.rx_events),
            static_cast<uint64_t>(g_dhd.rx_errors),
            static_cast<uint64_t>(g_dhd.rx_dropped));
  }
}
