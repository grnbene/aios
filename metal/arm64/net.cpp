/* Bare-metal Network Device & IPv4 / DHCP / ARP / ICMP Stack (metal/arm64/net.cpp).
 *
 * Implements:
 *   - Dual network interface support: eth0 (SMSC LAN9514) & wlan0 (BCM43438)
 *   - Automatic DHCP client state machine (Discover -> Offer -> Request -> Ack)
 *   - Dynamic ARP request / reply engine & peer cache
 *   - ICMP Echo (Ping) responder (sub-millisecond replies to incoming pings)
 *   - ICMP Ping diagnostic sender
 *   - Wi-Fi scanning, WPA2 association, and FAT32 credential persistence
 *   - Bidirectional lockless TX/RX ring buffers
 */
#include "net.hpp"
#include "fdt.hpp"

#include "bcm43438.hpp"
#include "dhd.hpp"
#include "fat32.hpp"
#include "kio.hpp"
#include "lan9514.hpp"
#include "rpi_mailbox.hpp"
#include "shell.hpp"
#include "timer.hpp"

extern "C" {
uint64_t metal_ticks();
}

namespace {

WifiSavedConfig g_saved_config = {};

struct NetLog {
  char lines[NET_LOG_CAPACITY][NET_LOG_LINE_LEN];
  uint32_t head = 0;
  uint32_t count = 0;

  void append(const char* msg) {
    if (!msg) return;
    char* dst = lines[head];
    size_t i = 0;
    while (msg[i] && i + 1 < NET_LOG_LINE_LEN) { dst[i] = msg[i]; ++i; }
    dst[i] = '\0';
    head = (head + 1) % NET_LOG_CAPACITY;
    if (count < NET_LOG_CAPACITY) ++count;
  }

  /* Newest first. */
  size_t copy_out(const char** out, size_t max) const {
    size_t n = count < max ? count : max;
    for (size_t i = 0; i < n; ++i) {
      uint32_t idx = (head + NET_LOG_CAPACITY - 1 - static_cast<uint32_t>(i)) % NET_LOG_CAPACITY;
      out[i] = lines[idx];
    }
    return n;
  }
};

NetLog g_eth_log;
NetLog g_wifi_log;

struct PacketRing {
  NetFrame entries[NET_RING_CAPACITY];
  uint32_t head = 0;
  uint32_t tail = 0;
  uint32_t count = 0;
  uint64_t enqueued = 0;
  uint64_t dequeued = 0;
  uint64_t dropped = 0;

  bool push(const void* data, size_t len, uint16_t flags) {
    if (!data || len == 0 || len > NET_MAX_FRAME_SIZE) return false;
    if (count >= NET_RING_CAPACITY) {
      dropped++;
      return false;
    }
    auto& slot = entries[tail];
    const auto* src = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) slot.data[i] = src[i];
    slot.len = static_cast<uint16_t>(len);
    slot.flags = flags;
    slot.timestamp_ns = metal_now_ns();

    tail = (tail + 1) % NET_RING_CAPACITY;
    count++;
    enqueued++;
    return true;
  }

  bool pop(void* out_data, size_t max_len, size_t& out_len) {
    out_len = 0;
    if (count == 0) return false;

    const auto& slot = entries[head];
    const size_t copy_len = (slot.len < max_len) ? slot.len : max_len;
    if (out_data && copy_len > 0) {
      auto* dst = static_cast<uint8_t*>(out_data);
      for (size_t i = 0; i < copy_len; ++i) dst[i] = slot.data[i];
    }
    out_len = copy_len;

    head = (head + 1) % NET_RING_CAPACITY;
    count--;
    dequeued++;
    return true;
  }

  NetRingStats stats() const {
    return {
      .capacity = static_cast<uint32_t>(NET_RING_CAPACITY),
      .depth = count,
      .enqueued = enqueued,
      .dequeued = dequeued,
      .dropped = dropped
    };
  }
};

NetInterfaceInfo g_eth_if = {};
NetInterfaceInfo g_wifi_if = {};
PacketRing g_eth_tx_ring;
PacketRing g_eth_rx_ring;
PacketRing g_wifi_tx_ring;
PacketRing g_wifi_rx_ring;

uint64_t g_last_dhcp_poll_ns = 0;
uint32_t g_dhcp_xid = 0x3914A800;

/* Automatic Wi-Fi discovery/association state - see net_poll() section 3b.
 * wlan0 used to only scan or connect when a human pressed a key in the
 * desktop Wi-Fi tab or typed "wifi scan"/"wifi connect" on the console -
 * both require a physical keyboard, which per this board's other net_poll()
 * comments "never happens on this board's actual deployment". Radio bring-up
 * (dhd_init()) already ran unattended from net_init(); nothing made it then
 * go LOOK for a network or try the saved one. */
uint64_t g_last_wifi_scan_ns = 0;
bool g_wifi_scan_complete = false;
/* Background scan/join run as state machines inside net_poll(): start, then
 * finish on a later pass. net_poll() is the loop the desktop, keyboard and
 * mouse run on; a 5 s wait inside it froze typing and let mouse motion pile
 * up into one jump, every 10 s, once Wi-Fi actually came up. */
bool g_auto_scan_pending = false;
uint64_t g_auto_scan_started_ns = 0;
bool g_auto_join_pending = false;
uint64_t g_auto_join_started_ns = 0;
char g_auto_join_ssid[33] = {};
bool wifi_scan_start();
bool wifi_scan_collect(NetScanEntry* entries, size_t max_entries, size_t& out_count);
uint64_t g_last_wifi_auto_connect_ns = 0;
constexpr size_t kAutoScanMax = 24;
NetScanEntry g_wifi_auto_scan_cache[kAutoScanMax];
size_t g_wifi_auto_scan_count = 0;
uint32_t g_wifi_scan_generation = 0;   /* bumps each time a scan result lands */

/* Ping Wait State */
struct PingWait {
  bool active;
  uint8_t target_ip[4];
  uint16_t seq;
  uint64_t send_ts_ns;
  uint64_t recv_ts_ns;
  bool replied;
};
PingWait g_ping = {};

void kmemcpy(void* dst, const void* src, size_t n) {
  auto* d = static_cast<uint8_t*>(dst);
  const auto* s = static_cast<const uint8_t*>(src);
  for (size_t i = 0; i < n; ++i) d[i] = s[i];
}

void kmemset(void* dst, int v, size_t n) {
  auto* d = static_cast<uint8_t*>(dst);
  for (size_t i = 0; i < n; ++i) d[i] = static_cast<uint8_t>(v);
}

size_t kstrlen(const char* s) {
  size_t n = 0;
  while (s && s[n]) ++n;
  return n;
}

/* Internet checksum (RFC 1071), computed by summing the buffer as NATIVE
 * uint16_t reads. On this little-endian target that is byte-swapped
 * relative to the network/big-endian words the wire actually carries - the
 * standard trick that makes this cheap on an LE machine. It is only
 * correct if the caller ALSO stores the 16-bit result back with a native
 * (little-endian) byte order: low byte at the lower address, high byte at
 * the higher one - NOT the `>> 8` then `& 0xFF` big-endian split every
 * checksum field write used to do. That manual big-endian write undid the
 * swap a second time instead of leaving it alone, so every IP/ICMP
 * checksum this stack ever sent was byte-swapped and therefore wrong -
 * silently invisible here, and silently dropped by anything upstream that
 * checks it, which is every real router, DHCP server, and OS network
 * stack. That is enough on its own to look exactly like "no network" with
 * link up and hardware fully working. */
uint16_t net_chksum(const void* data, size_t len) {
  const auto* ptr = static_cast<const uint16_t*>(data);
  uint32_t sum = 0;
  while (len > 1) {
    sum += *ptr++;
    len -= 2;
  }
  if (len == 1) {
    sum += *reinterpret_cast<const uint8_t*>(ptr);
  }
  while (sum >> 16) {
    sum = (sum & 0xFFFFu) + (sum >> 16);
  }
  return static_cast<uint16_t>(~sum);
}

/* UDP checksum over IPv4, including the pseudo-header (RFC 768). This
 * codebase has been leaving UDP checksums at 0 ("not computed") on every
 * outgoing packet, DHCP included - legal per RFC 768, but a documented,
 * real behavior of a meaningful number of routers and consumer DHCP
 * server implementations is to silently drop any UDP datagram with a zero
 * checksum as a basic sanity/anti-spoof filter. That produces exactly
 * "packet leaves cleanly, absolutely nothing ever comes back" - no error,
 * no malformed-reply, nothing to see on the receive side at all, because
 * there never was a reply to receive.
 *
 * udp_segment must be the UDP header immediately followed by its payload,
 * contiguous, with the checksum field already zeroed. src_ip is the actual
 * source (0.0.0.0 is valid and expected for an unconfigured DHCP client -
 * the pseudo-header still uses it exactly as sent). */
uint16_t net_udp_chksum(const uint8_t src_ip[4], const uint8_t dst_ip[4],
                        const uint8_t* udp_segment, size_t udp_len) {
  /* Was capped at 512 - a chunk of the multi-packet console reply protocol
   * (net.cpp's own handle_console_packet(), up to 1200 bytes of payload
   * plus an 8-byte UDP header) is bigger than that, so its checksum was
   * silently elided (returned 0, "not computed" - legal on the wire per
   * RFC 768, but real switches/NICs along a path are not obligated to
   * forward a packet with no checksum the same way they forward one with
   * a valid one, and this is exactly the "some large chunks vanish for no
   * visible reason" symptom this file's own comments describe). Sized to
   * the largest UDP segment anything in this file actually builds (the
   * 1216-byte console chunk), not a round guess. */
  uint8_t buf[12 + 1280];
  if (udp_len > sizeof(buf) - 12) return 0;
  kmemcpy(buf, src_ip, 4);
  kmemcpy(buf + 4, dst_ip, 4);
  buf[8] = 0;
  buf[9] = 17; /* UDP */
  buf[10] = static_cast<uint8_t>(udp_len >> 8);
  buf[11] = static_cast<uint8_t>(udp_len & 0xFF);
  kmemcpy(buf + 12, udp_segment, udp_len);
  const uint16_t c = net_chksum(buf, 12 + udp_len);
  /* RFC 768: a computed checksum of exactly 0 is transmitted as all-ones,
   * because 0 means "not computed" on the wire. */
  return (c == 0) ? 0xFFFFu : c;
}

const char* link_state_str(NetLinkState st) {
  switch (st) {
    case NetLinkState::Down:        return "DOWN";
    case NetLinkState::Up:          return "UP (unassociated)";
    case NetLinkState::Scanning:    return "SCANNING";
    case NetLinkState::Associating: return "ASSOCIATING";
    case NetLinkState::Associated:  return "ASSOCIATED";
    default:                        return "UNKNOWN";
  }
}

/* ----------------- Networking Helpers & Byte Conversions ----------------- */

inline void w16_be(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v & 0xFF);
}

inline void w32_be(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v & 0xFF);
}

inline uint16_t r16_be(const uint8_t* p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

inline uint32_t r32_be(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8)  |
         static_cast<uint32_t>(p[3]);
}

uint16_t tcp_checksum(const uint8_t* data, size_t len, uint32_t seed) {
  uint32_t sum = seed;
  size_t i = 0;
  for (; i + 1 < len; i += 2) sum += r16_be(data + i);
  if (i < len) sum += static_cast<uint32_t>(data[i]) << 8;
  while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
  return static_cast<uint16_t>(~sum);
}

uint32_t tcp_pseudo_seed(const uint8_t src[4], const uint8_t dst[4], uint8_t proto, uint16_t tlen) {
  uint32_t s = 0;
  s += r16_be(src) + r16_be(src + 2);
  s += r16_be(dst) + r16_be(dst + 2);
  s += proto;
  s += tlen;
  return s;
}

uint32_t net_rand32() {
  static uint32_t state = 0x87654321;
  state ^= static_cast<uint32_t>(metal_ticks() ^ metal_now_ns());
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  if (state == 0) state = 0x13579BDF;
  return state;
}

uint16_t net_alloc_ephemeral_port() {
  static uint16_t next_port = 49152;
  uint16_t p = next_port++;
  if (next_port < 49152) next_port = 49152;
  return p;
}

/* ----------------- Dynamic ARP Cache & Resolver ----------------- */

struct ArpEntry {
  uint8_t  ip[4];
  uint8_t  mac[6];
  uint64_t last_seen_ns;
  bool     valid;
};
constexpr size_t ARP_TABLE_CAPACITY = 32;
ArpEntry g_arp_table[ARP_TABLE_CAPACITY] = {};

void arp_learn(const uint8_t ip[4], const uint8_t mac[6]) {
  if (!ip || !mac) return;
  if (ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0) return;
  if (ip[0] == 255 && ip[1] == 255 && ip[2] == 255 && ip[3] == 255) return;

  uint64_t now = metal_now_ns();
  int oldest_idx = -1;
  uint64_t oldest_ts = UINT64_MAX;

  for (size_t i = 0; i < ARP_TABLE_CAPACITY; ++i) {
    if (g_arp_table[i].valid) {
      if (g_arp_table[i].ip[0] == ip[0] &&
          g_arp_table[i].ip[1] == ip[1] &&
          g_arp_table[i].ip[2] == ip[2] &&
          g_arp_table[i].ip[3] == ip[3]) {
        kmemcpy(g_arp_table[i].mac, mac, 6);
        g_arp_table[i].last_seen_ns = now;
        return;
      }
      if (g_arp_table[i].last_seen_ns < oldest_ts) {
        oldest_ts = g_arp_table[i].last_seen_ns;
        oldest_idx = static_cast<int>(i);
      }
    } else if (oldest_idx < 0) {
      oldest_idx = static_cast<int>(i);
    }
  }

  if (oldest_idx >= 0) {
    kmemcpy(g_arp_table[oldest_idx].ip, ip, 4);
    kmemcpy(g_arp_table[oldest_idx].mac, mac, 6);
    g_arp_table[oldest_idx].last_seen_ns = now;
    g_arp_table[oldest_idx].valid = true;
  }
}

const uint8_t* arp_lookup(const uint8_t ip[4]) {
  if (!ip) return nullptr;
  for (size_t i = 0; i < ARP_TABLE_CAPACITY; ++i) {
    if (g_arp_table[i].valid &&
        g_arp_table[i].ip[0] == ip[0] &&
        g_arp_table[i].ip[1] == ip[1] &&
        g_arp_table[i].ip[2] == ip[2] &&
        g_arp_table[i].ip[3] == ip[3]) {
      return g_arp_table[i].mac;
    }
  }
  return nullptr;
}

void send_arp_request(NetInterfaceInfo& iface, const uint8_t target_ip[4]) {
  uint8_t pkt[42] = {};
  kmemset(pkt + 0, 0xFF, 6);
  kmemcpy(pkt + 6, iface.mac, 6);
  pkt[12] = 0x08; pkt[13] = 0x06; /* ARP */
  pkt[14] = 0x00; pkt[15] = 0x01; /* Ethernet */
  pkt[16] = 0x08; pkt[17] = 0x00; /* IPv4 */
  pkt[18] = 0x06; pkt[19] = 0x04;
  pkt[20] = 0x00; pkt[21] = 0x01; /* Request */
  kmemcpy(pkt + 22, iface.mac, 6);
  kmemcpy(pkt + 28, iface.ip_cfg.ip, 4);
  kmemset(pkt + 32, 0x00, 6);
  kmemcpy(pkt + 38, target_ip, 4);
  net_transmit_if(iface.type, pkt, sizeof(pkt));
}

bool arp_resolve(NetInterfaceInfo& iface, const uint8_t target_ip[4], uint8_t out_mac[6], uint32_t timeout_ms = 1000) {
  const uint8_t* resolve_ip = target_ip;
  if (iface.ip_cfg.gateway[0] != 0 || iface.ip_cfg.gateway[1] != 0 || iface.ip_cfg.gateway[2] != 0 || iface.ip_cfg.gateway[3] != 0) {
    bool same_subnet = true;
    for (int i = 0; i < 4; ++i) {
      if ((iface.ip_cfg.ip[i] & iface.ip_cfg.netmask[i]) != (target_ip[i] & iface.ip_cfg.netmask[i])) {
        same_subnet = false;
        break;
      }
    }
    if (!same_subnet) {
      resolve_ip = iface.ip_cfg.gateway;
    }
  }

  const uint8_t* cached = arp_lookup(resolve_ip);
  if (cached) {
    kmemcpy(out_mac, cached, 6);
    return true;
  }

  send_arp_request(iface, resolve_ip);
  uint64_t deadline = metal_now_ns() + static_cast<uint64_t>(timeout_ms) * 1000000ull;
  uint64_t last_req = metal_now_ns();

  while (metal_now_ns() < deadline) {
    net_poll();
    cached = arp_lookup(resolve_ip);
    if (cached) {
      kmemcpy(out_mac, cached, 6);
      return true;
    }
    if (metal_now_ns() - last_req > 300000000ull) {
      send_arp_request(iface, resolve_ip);
      last_req = metal_now_ns();
    }
    metal_delay_us(500);
  }

  kmemset(out_mac, 0xFF, 6);
  return false;
}

/* ----------------- TCP Control Block & State Machine Engine ----------------- */

// The bytes live in g_socket_bytes (zero-initialised, so .bss: they cost the
// image nothing); each control block points its two rings at its own pair.
struct SocketRingBuffer {
  uint8_t* buf = nullptr;
  size_t   head = 0;
  size_t   tail = 0;
  size_t   count = 0;

  void clear() { head = 0; tail = 0; count = 0; }
  size_t available() const { return TCP_SOCKET_BUF_SIZE - count; }
  size_t size() const { return count; }

  // Copies in (at most) two pieces around the wrap, not a byte at a time.
  size_t write(const uint8_t* data, size_t len) {
    size_t to_write = (len < available()) ? len : available();
    const size_t first = (to_write < TCP_SOCKET_BUF_SIZE - tail) ? to_write : TCP_SOCKET_BUF_SIZE - tail;
    kmemcpy(buf + tail, data, first);
    if (to_write > first) kmemcpy(buf, data + first, to_write - first);
    tail = (tail + to_write) % TCP_SOCKET_BUF_SIZE;
    count += to_write;
    return to_write;
  }

  size_t read(uint8_t* out, size_t max_len) {
    const size_t to_read = peek(out, max_len, 0);
    head = (head + to_read) % TCP_SOCKET_BUF_SIZE;
    count -= to_read;
    return to_read;
  }

  size_t peek(uint8_t* out, size_t max_len, size_t offset = 0) const {
    if (offset >= count) return 0;
    size_t avail = count - offset;
    size_t to_read = (max_len < avail) ? max_len : avail;
    size_t pos = (head + offset) % TCP_SOCKET_BUF_SIZE;
    const size_t first = (to_read < TCP_SOCKET_BUF_SIZE - pos) ? to_read : TCP_SOCKET_BUF_SIZE - pos;
    kmemcpy(out, buf + pos, first);
    if (to_read > first) kmemcpy(out + first, buf, to_read - first);
    return to_read;
  }

  void drop(size_t len) {
    size_t to_drop = (len < count) ? len : count;
    head = (head + to_drop) % TCP_SOCKET_BUF_SIZE;
    count -= to_drop;
  }
};

struct TcpControlBlock {
  int              id = 0;
  bool             in_use = false;
  TcpState         state = TcpState::Closed;
  NetIfType        if_type = NetIfType::None;

  uint8_t          remote_ip[4] = {};
  uint16_t         remote_port = 0;
  uint16_t         local_port = 0;

  uint32_t         iss = 0;
  uint32_t         snd_una = 0;
  uint32_t         snd_nxt = 0;
  uint32_t         snd_wnd = 65535;

  uint32_t         irs = 0;
  uint32_t         rcv_nxt = 0;
  uint32_t         rcv_wnd = TCP_SOCKET_BUF_SIZE;

  uint16_t         mss = 1460;   /* was 400: 3.6x the packets for every transfer */
  bool             fin_sent = false;
  bool             peer_fin = false;
  bool             reset = false;
  bool             syn_acked = false;

  uint64_t         last_tx_ns = 0;
  uint32_t         rto_ms = 300;
  int              retries = 0;
  int              dup_acks = 0;

  bool             listening = false;
  int              backlog[TCP_MAX_BACKLOG] = {};
  size_t           backlog_count = 0;
  size_t           backlog_limit = TCP_MAX_BACKLOG;

  SocketRingBuffer rx_buf;
  SocketRingBuffer tx_buf;
};

TcpControlBlock g_tcbs[TCP_MAX_SOCKETS] = {};
uint8_t g_socket_bytes[TCP_MAX_SOCKETS][2][TCP_SOCKET_BUF_SIZE];
int g_next_sock_id = 1;

TcpControlBlock* tcb_find_by_id(int id) {
  if (id <= 0) return nullptr;
  for (size_t i = 0; i < TCP_MAX_SOCKETS; ++i) {
    if (g_tcbs[i].in_use && g_tcbs[i].id == id) return &g_tcbs[i];
  }
  return nullptr;
}

TcpControlBlock* tcb_allocate() {
  for (size_t i = 0; i < TCP_MAX_SOCKETS; ++i) {
    if (!g_tcbs[i].in_use || g_tcbs[i].state == TcpState::Closed) {
      g_tcbs[i] = {};
      g_tcbs[i].rx_buf.buf = g_socket_bytes[i][0];
      g_tcbs[i].tx_buf.buf = g_socket_bytes[i][1];
      g_tcbs[i].id = g_next_sock_id++;
      g_tcbs[i].in_use = true;
      g_tcbs[i].state = TcpState::Closed;
      g_tcbs[i].mss = 1460;
      g_tcbs[i].rto_ms = 300;
      g_tcbs[i].rcv_wnd = TCP_SOCKET_BUF_SIZE;
      g_tcbs[i].rx_buf.clear();
      g_tcbs[i].tx_buf.clear();
      return &g_tcbs[i];
    }
  }
  return nullptr;
}

void tcb_free(TcpControlBlock* t) {
  if (!t) return;
  t->in_use = false;
  t->state = TcpState::Closed;
  t->rx_buf.clear();
  t->tx_buf.clear();
}

void send_raw_tcp_segment(NetInterfaceInfo& iface,
                           const uint8_t dst_mac[6],
                           const uint8_t dst_ip[4],
                           uint16_t src_port,
                           uint16_t dst_port,
                           uint32_t seq,
                           uint32_t ack,
                           uint8_t flags,
                           uint16_t window,
                           const uint8_t* payload,
                           size_t payload_len,
                           bool with_mss_opt = false) {
  /* Same class of bug as send_udp() had: an unzeroed packet buffer leaves
   * every field this function does not explicitly write (IP identification,
   * flags/fragment-offset, and the MSS option's padding when
   * with_mss_opt is false but tcp_hdr_len still reserves the space) as
   * uninitialized stack garbage, transmitted as-is. */
  uint8_t pkt[NET_MAX_FRAME_SIZE] = {};
  size_t tcp_hdr_len = with_mss_opt ? 24 : 20;
  size_t total_ip_len = 20 + tcp_hdr_len + payload_len;
  size_t total_frame_len = 14 + total_ip_len;

  if (total_frame_len > sizeof(pkt)) return;

  /* Ethernet header */
  kmemcpy(pkt + 0, dst_mac, 6);
  kmemcpy(pkt + 6, iface.mac, 6);
  pkt[12] = 0x08; pkt[13] = 0x00; /* IPv4 */

  /* IPv4 header */
  uint8_t* ip = pkt + 14;
  ip[0] = 0x45; ip[1] = 0x00;
  w16_be(ip + 2, static_cast<uint16_t>(total_ip_len));
  static uint16_t s_ip_id = 1;
  w16_be(ip + 4, s_ip_id++);
  w16_be(ip + 6, 0x4000); /* Don't Fragment */
  ip[8] = 64; /* TTL */
  ip[9] = 6;  /* IPPROTO_TCP */
  ip[10] = 0; ip[11] = 0;
  kmemcpy(ip + 12, iface.ip_cfg.ip, 4);
  kmemcpy(ip + 16, dst_ip, 4);
  uint16_t ip_chk = net_chksum(ip, 20);
  ip[10] = static_cast<uint8_t>(ip_chk & 0xFF);
  ip[11] = static_cast<uint8_t>(ip_chk >> 8);

  /* TCP header */
  uint8_t* tcp = pkt + 14 + 20;
  w16_be(tcp + 0, src_port);
  w16_be(tcp + 2, dst_port);
  w32_be(tcp + 4, seq);
  w32_be(tcp + 8, (flags & TCP_FLAG_ACK) ? ack : 0);
  tcp[12] = static_cast<uint8_t>((tcp_hdr_len / 4) << 4);
  tcp[13] = flags;
  w16_be(tcp + 14, window);
  tcp[16] = 0; tcp[17] = 0;
  tcp[18] = 0; tcp[19] = 0;

  if (with_mss_opt) {
    tcp[20] = 2; /* MSS Option Kind */
    tcp[21] = 4; /* Length */
    w16_be(tcp + 22, 1460);
  }

  if (payload && payload_len > 0) {
    kmemcpy(tcp + tcp_hdr_len, payload, payload_len);
  }

  uint32_t pseudo_seed = tcp_pseudo_seed(iface.ip_cfg.ip, dst_ip, 6, static_cast<uint16_t>(tcp_hdr_len + payload_len));
  uint16_t chk = tcp_checksum(tcp, tcp_hdr_len + payload_len, pseudo_seed);
  w16_be(tcp + 16, chk);

  net_transmit_if(iface.type, pkt, total_frame_len);
}

void tcp_out_segment(TcpControlBlock& t, uint8_t flags, const uint8_t* data, size_t len, bool with_mss = false) {
  /* The sequence number this segment was meant to carry, taken before
   * arp_resolve(): that polls the network, the poll runs the TCP timers,
   * and a timer that retransmits on this same connection moves snd_nxt
   * underneath us. It did, on every connect - the first SYN went out at
   * iss+1 right behind the timer's SYN at iss, and a real peer answered two
   * different handshakes with a reset. */
  const uint32_t seq = t.snd_nxt;
  NetInterfaceInfo iface = net_primary_info();
  if (t.if_type != NetIfType::None) {
    if (t.if_type == NetIfType::Ethernet) iface = net_eth_info();
    else if (t.if_type == NetIfType::Wifi) iface = net_wifi_info();
  }

  uint8_t dst_mac[6];
  if (!arp_resolve(iface, t.remote_ip, dst_mac, 200)) {
    const uint8_t* gw_mac = arp_lookup(iface.ip_cfg.gateway);
    if (gw_mac) kmemcpy(dst_mac, gw_mac, 6);
    else kmemset(dst_mac, 0xFF, 6);
  }

  uint16_t rx_wnd = static_cast<uint16_t>(t.rx_buf.available() > 65535 ? 65535 : t.rx_buf.available());
  send_raw_tcp_segment(iface, dst_mac, t.remote_ip, t.local_port, t.remote_port,
                        seq, (flags & TCP_FLAG_ACK) ? t.rcv_nxt : 0,
                        flags, rx_wnd, data, len, with_mss);
  t.last_tx_ns = metal_now_ns();
}

void tcp_retransmit(TcpControlBlock& t, bool all);

void handle_tcp_packet(NetInterfaceInfo& iface, const uint8_t* frame, size_t len) {
  if (len < 14 + 20 + 20) return;
  const uint8_t* sender_mac = frame + 6;
  const uint8_t* ip_hdr = frame + 14;
  uint32_t ip_hl = (ip_hdr[0] & 0x0F) * 4;
  if (len < 14 + ip_hl + 20) return;

  const uint8_t* src_ip = ip_hdr + 12;
  const uint8_t* dst_ip = ip_hdr + 16;

  if (iface.ip_cfg.ip[0] != 0 &&
      (dst_ip[0] != iface.ip_cfg.ip[0] ||
       dst_ip[1] != iface.ip_cfg.ip[1] ||
       dst_ip[2] != iface.ip_cfg.ip[2] ||
       dst_ip[3] != iface.ip_cfg.ip[3])) {
    return;
  }

  arp_learn(src_ip, sender_mac);

  const uint8_t* tcp_hdr = frame + 14 + ip_hl;
  uint16_t sport = r16_be(tcp_hdr + 0);
  uint16_t dport = r16_be(tcp_hdr + 2);
  uint32_t seq = r32_be(tcp_hdr + 4);
  uint32_t ack = r32_be(tcp_hdr + 8);
  size_t tcp_hl = ((tcp_hdr[12] >> 4) & 0x0F) * 4;
  if (tcp_hl < 20 || len < 14 + ip_hl + tcp_hl) return;

  uint8_t flags = tcp_hdr[13];
  uint16_t wnd = r16_be(tcp_hdr + 14);

  const uint8_t* payload = frame + 14 + ip_hl + tcp_hl;
  size_t ip_tot_len = r16_be(ip_hdr + 2);
  size_t avail_in_frame = len - (14 + ip_hl + tcp_hl);
  size_t dlen = 0;
  if (ip_tot_len >= ip_hl + tcp_hl) {
    size_t ip_payload = ip_tot_len - ip_hl - tcp_hl;
    dlen = (ip_payload < avail_in_frame) ? ip_payload : avail_in_frame;
  } else {
    dlen = avail_in_frame;
  }

  TcpControlBlock* tp = nullptr;
  for (size_t i = 0; i < TCP_MAX_SOCKETS; ++i) {
    if (g_tcbs[i].in_use && !g_tcbs[i].listening &&
        g_tcbs[i].local_port == dport &&
        g_tcbs[i].remote_port == sport &&
        g_tcbs[i].remote_ip[0] == src_ip[0] &&
        g_tcbs[i].remote_ip[1] == src_ip[1] &&
        g_tcbs[i].remote_ip[2] == src_ip[2] &&
        g_tcbs[i].remote_ip[3] == src_ip[3]) {
      tp = &g_tcbs[i];
      break;
    }
  }

  if (!tp) {
    TcpControlBlock* listener = nullptr;
    for (size_t i = 0; i < TCP_MAX_SOCKETS; ++i) {
      if (g_tcbs[i].in_use && g_tcbs[i].listening && g_tcbs[i].local_port == dport) {
        listener = &g_tcbs[i];
        break;
      }
    }

    if (!listener) {
      if (!(flags & TCP_FLAG_RST)) {
        uint32_t rst_seq = (flags & TCP_FLAG_ACK) ? ack : 0;
        uint32_t rst_ack = seq + ((flags & (TCP_FLAG_SYN | TCP_FLAG_FIN)) ? 1 : static_cast<uint32_t>(dlen));
        uint8_t rst_flags = (flags & TCP_FLAG_ACK) ? TCP_FLAG_RST : (TCP_FLAG_RST | TCP_FLAG_ACK);
        send_raw_tcp_segment(iface, sender_mac, src_ip, dport, sport, rst_seq, rst_ack, rst_flags, 0, nullptr, 0);
      }
      return;
    }

    if ((flags & TCP_FLAG_SYN) && !(flags & TCP_FLAG_ACK)) {
      if (listener->backlog_count >= listener->backlog_limit) return;

      TcpControlBlock* c = tcb_allocate();
      if (!c) return;

      c->state = TcpState::SynReceived;
      c->if_type = iface.type;
      kmemcpy(c->remote_ip, src_ip, 4);
      c->remote_port = sport;
      c->local_port = dport;
      c->irs = seq;
      c->rcv_nxt = seq + 1;
      c->iss = net_rand32();
      c->snd_una = c->iss;
      c->snd_nxt = c->iss;
      c->snd_wnd = wnd;

      for (size_t o = 20; o + 1 < tcp_hl; ) {
        uint8_t k = tcp_hdr[o];
        if (k == 0) break;
        if (k == 1) { o++; continue; }
        uint8_t l = tcp_hdr[o + 1];
        if (l < 2) break;
        if (k == 2 && l == 4) {
          uint16_t pmss = r16_be(tcp_hdr + o + 2);
          c->mss = (pmss < 1460) ? pmss : 1460;
        }
        o += l;
      }

      tcp_out_segment(*c, TCP_FLAG_SYN | TCP_FLAG_ACK, nullptr, 0, true);
      c->snd_nxt = c->iss + 1;
      return;
    }
    return;
  }

  TcpControlBlock& t = *tp;

  if (flags & TCP_FLAG_RST) {
    t.reset = true;
    t.state = TcpState::Closed;
    return;
  }

  t.snd_wnd = wnd;

  if (t.state == TcpState::SynSent) {
    if ((flags & TCP_FLAG_SYN) && (flags & TCP_FLAG_ACK)) {
      t.irs = seq;
      t.rcv_nxt = seq + 1;
      t.snd_una = ack;
      t.snd_nxt = ack;
      t.syn_acked = true;

      for (size_t o = 20; o + 1 < tcp_hl; ) {
        uint8_t k = tcp_hdr[o];
        if (k == 0) break;
        if (k == 1) { o++; continue; }
        uint8_t l = tcp_hdr[o + 1];
        if (l < 2) break;
        if (k == 2 && l == 4) {
          uint16_t pmss = r16_be(tcp_hdr + o + 2);
          t.mss = (pmss < 1460) ? pmss : 1460;
        }
        o += l;
      }

      t.state = TcpState::Established;
      tcp_out_segment(t, TCP_FLAG_ACK, nullptr, 0);
    }
    return;
  }

  if (t.state == TcpState::SynReceived) {
    if (!(flags & TCP_FLAG_ACK) || static_cast<int32_t>(ack - t.iss) <= 0) return;
    t.snd_una = ack;
    t.snd_nxt = ack;
    t.syn_acked = true;
    t.state = TcpState::Established;

    for (size_t i = 0; i < TCP_MAX_SOCKETS; ++i) {
      if (g_tcbs[i].in_use && g_tcbs[i].listening && g_tcbs[i].local_port == t.local_port) {
        if (g_tcbs[i].backlog_count < TCP_MAX_BACKLOG) {
          g_tcbs[i].backlog[g_tcbs[i].backlog_count++] = t.id;
        }
        break;
      }
    }
  }

  if ((flags & TCP_FLAG_ACK) && static_cast<int32_t>(ack - t.snd_una) > 0) {
    uint32_t adv = ack - t.snd_una;
    t.tx_buf.drop(adv);
    t.snd_una = ack;
    t.retries = 0;
    t.rto_ms = 300;
    t.dup_acks = 0;
  } else if ((flags & TCP_FLAG_ACK) && dlen == 0 && ack == t.snd_una && t.snd_nxt != t.snd_una) {
    /* Three duplicate ACKs: the peer is missing the segment at snd_una. */
    if (++t.dup_acks == 3) tcp_retransmit(t, false);
  }

  if (dlen > 0 && seq == t.rcv_nxt) {
    t.rx_buf.write(payload, dlen);
    t.rcv_nxt += dlen;
    tcp_out_segment(t, TCP_FLAG_ACK, nullptr, 0);
  } else if (dlen > 0) {
    tcp_out_segment(t, TCP_FLAG_ACK, nullptr, 0);
  }

  if (flags & TCP_FLAG_FIN) {
    if (seq == t.rcv_nxt || (dlen > 0 && seq + dlen == t.rcv_nxt)) {
      t.rcv_nxt++;
      t.peer_fin = true;
      tcp_out_segment(t, TCP_FLAG_ACK, nullptr, 0);
      if (t.state == TcpState::Established) {
        t.state = TcpState::CloseWait;
      } else if (t.state == TcpState::FinWait1) {
        if (flags & TCP_FLAG_ACK) {
          t.state = TcpState::TimeWait;
        } else {
          t.state = TcpState::Closing;
        }
      } else if (t.state == TcpState::FinWait2) {
        t.state = TcpState::TimeWait;
      }
    }
  }

  if (t.fin_sent && t.snd_una == t.snd_nxt) {
    if (t.state == TcpState::FinWait1) {
      t.state = TcpState::FinWait2;
    } else if (t.state == TcpState::Closing || t.state == TcpState::LastAck) {
      t.state = (t.state == TcpState::Closing) ? TcpState::TimeWait : TcpState::Closed;
    }
  }
}

void tcp_poll_timers() {
  uint64_t now = metal_now_ns();
  for (size_t i = 0; i < TCP_MAX_SOCKETS; ++i) {
    if (!g_tcbs[i].in_use || g_tcbs[i].listening) continue;
    TcpControlBlock& t = g_tcbs[i];

    if (t.state == TcpState::SynSent && now - t.last_tx_ns > static_cast<uint64_t>(t.rto_ms) * 1000000ull) {
      if (++t.retries > 8) {
        t.reset = true;
        t.state = TcpState::Closed;
        continue;
      }
      t.rto_ms = (t.rto_ms * 2 < 4000) ? t.rto_ms * 2 : 4000;
      t.snd_nxt = t.iss;
      tcp_out_segment(t, TCP_FLAG_SYN, nullptr, 0, true);
      t.snd_nxt = t.iss + 1;
    } else if (t.state == TcpState::TimeWait && now - t.last_tx_ns > 2000000000ull) {
      t.state = TcpState::Closed;
    } else if (t.tx_buf.size() > 0 && now - t.last_tx_ns > static_cast<uint64_t>(t.rto_ms) * 1000000ull) {
      if (++t.retries > 8) {
        t.reset = true;
        t.state = TcpState::Closed;
        continue;
      }
      t.rto_ms = (t.rto_ms * 2 < 4000) ? t.rto_ms * 2 : 4000;
      tcp_retransmit(t, true);
    }
  }
}

/* Room left in the interface TX ring: a segment pushed into a full ring was
 * dropped silently and then cost a retransmit timeout - that, with a 400 B
 * MSS and one-segment-per-RTO recovery, held screenshots to ~2 KB/s. */
size_t tx_ring_room(const TcpControlBlock& t) {
  const PacketRing& ring = (t.if_type == NetIfType::Wifi) ? g_wifi_tx_ring : g_eth_tx_ring;
  return ring.count + 4 < NET_RING_CAPACITY ? NET_RING_CAPACITY - 4 - ring.count : 0;
}

size_t tcp_push(TcpControlBlock& t, const uint8_t* data, size_t len) {
  /* CloseWait is a half-close: the peer stopped sending, not receiving. */
  if (t.state != TcpState::Established && t.state != TcpState::CloseWait) return 0;
  size_t total_sent = 0;
  while (total_sent < len) {
    const uint32_t in_flight = t.snd_nxt - t.snd_una;
    const uint32_t wnd = t.snd_wnd ? t.snd_wnd : 1;
    if (in_flight >= wnd || tx_ring_room(t) == 0) break;
    size_t chunk = len - total_sent;
    if (chunk > t.mss) chunk = t.mss;
    if (chunk > wnd - in_flight) chunk = wnd - in_flight;
    if (chunk > t.tx_buf.available()) chunk = t.tx_buf.available();
    if (chunk == 0) break;

    t.tx_buf.write(data + total_sent, chunk);
    tcp_out_segment(t, TCP_FLAG_PSH | TCP_FLAG_ACK, data + total_sent, chunk);
    t.snd_nxt += static_cast<uint32_t>(chunk);
    total_sent += chunk;
  }
  return total_sent;
}

/* Resend unacknowledged data from snd_una: everything outstanding on a
 * timeout (go-back-N), or just the first segment for a fast retransmit.
 * The old path resent one segment per timeout, so a burst of losses took
 * seconds to recover. */
void tcp_retransmit(TcpControlBlock& t, bool all) {
  uint8_t d[1460];
  const uint32_t outstanding = t.snd_nxt - t.snd_una;
  uint32_t off = 0;
  const uint32_t save_nxt = t.snd_nxt;
  while (off < outstanding && tx_ring_room(t) > 0) {
    size_t chunk = t.mss < sizeof(d) ? t.mss : sizeof(d);
    if (chunk > outstanding - off) chunk = outstanding - off;
    const size_t got = t.tx_buf.peek(d, chunk, off);
    if (got == 0) break;
    t.snd_nxt = t.snd_una + off;
    tcp_out_segment(t, TCP_FLAG_PSH | TCP_FLAG_ACK, d, got);
    off += static_cast<uint32_t>(got);
    if (!all) break;
  }
  t.snd_nxt = save_nxt;
}

/* ----------------- IPv4 & ARP & ICMP Processing ----------------- */

void handle_arp_packet(NetInterfaceInfo& iface, const uint8_t* frame, size_t len) {
  if (len < 42) return;

  uint16_t hw_type = (frame[14] << 8) | frame[15];
  uint16_t proto_type = (frame[16] << 8) | frame[17];
  uint16_t opcode = (frame[20] << 8) | frame[21];

  if (hw_type != 0x0001 || proto_type != 0x0800) return;

  const uint8_t* sender_mac = &frame[22];
  const uint8_t* sender_ip  = &frame[28];
  const uint8_t* target_ip  = &frame[38];

  arp_learn(sender_ip, sender_mac);

  /* If ARP Request for our IP address */
  if (opcode == 1 && iface.ip_cfg.ip[0] != 0) {
    if (target_ip[0] == iface.ip_cfg.ip[0] &&
        target_ip[1] == iface.ip_cfg.ip[1] &&
        target_ip[2] == iface.ip_cfg.ip[2] &&
        target_ip[3] == iface.ip_cfg.ip[3]) {

      /* Build ARP Reply */
      uint8_t reply[42];
      /* Eth Dst = sender MAC, Eth Src = our MAC */
      kmemcpy(&reply[0], sender_mac, 6);
      kmemcpy(&reply[6], iface.mac, 6);
      reply[12] = 0x08; reply[13] = 0x06; /* ARP */

      /* HW Type (1), Proto (0x0800), HW Size (6), Proto Size (4) */
      reply[14] = 0x00; reply[15] = 0x01;
      reply[16] = 0x08; reply[17] = 0x00;
      reply[18] = 0x06; reply[19] = 0x04;
      reply[20] = 0x00; reply[21] = 0x02; /* ARP Reply */

      /* Sender MAC = our MAC, Sender IP = our IP */
      kmemcpy(&reply[22], iface.mac, 6);
      kmemcpy(&reply[28], iface.ip_cfg.ip, 4);

      /* Target MAC = requestor MAC, Target IP = requestor IP */
      kmemcpy(&reply[32], sender_mac, 6);
      kmemcpy(&reply[38], sender_ip, 4);

      net_transmit_if(iface.type, reply, sizeof(reply));
    }
  }
}

void handle_icmp_packet(NetInterfaceInfo& iface, const uint8_t* frame, size_t len) {
  if (len < 34 + 8) return; /* 14 Eth + 20 IP + 8 ICMP */

  const uint8_t* ip_hdr = frame + 14;
  uint32_t ip_hl = (ip_hdr[0] & 0x0F) * 4;
  if (len < 14 + ip_hl + 8) return;

  const uint8_t* icmp_hdr = frame + 14 + ip_hl;
  uint8_t icmp_type = icmp_hdr[0];
  uint8_t icmp_code = icmp_hdr[1];

  /* 1. Inbound Ping Reply to our diagnostic ping */
  if (icmp_type == 0 && icmp_code == 0 && g_ping.active) {
    uint16_t seq = (icmp_hdr[6] << 8) | icmp_hdr[7];
    if (seq == g_ping.seq) {
      g_ping.replied = true;
      g_ping.recv_ts_ns = metal_now_ns();
      return;
    }
  }

  /* 2. Inbound Ping Request addressed to us -> Answer Echo Reply */
  if (icmp_type == 8 && icmp_code == 0 && iface.ip_cfg.ip[0] != 0) {
    const uint8_t* dst_ip = ip_hdr + 16;
    if (dst_ip[0] == iface.ip_cfg.ip[0] &&
        dst_ip[1] == iface.ip_cfg.ip[1] &&
        dst_ip[2] == iface.ip_cfg.ip[2] &&
        dst_ip[3] == iface.ip_cfg.ip[3]) {

      uint8_t reply[NET_MAX_FRAME_SIZE];
      if (len > sizeof(reply)) return;
      kmemcpy(reply, frame, len);

      /* Swap Ethernet MACs */
      kmemcpy(reply + 0, frame + 6, 6);
      kmemcpy(reply + 6, iface.mac, 6);

      /* Swap IPv4 Src and Dst */
      uint8_t* r_ip = reply + 14;
      kmemcpy(r_ip + 12, ip_hdr + 16, 4); /* Src IP = our IP */
      kmemcpy(r_ip + 16, ip_hdr + 12, 4); /* Dst IP = sender IP */
      r_ip[10] = 0; r_ip[11] = 0;         /* Clear IP checksum */
      uint16_t new_ip_chk = net_chksum(r_ip, ip_hl);
      r_ip[10] = static_cast<uint8_t>(new_ip_chk & 0xFF);
      r_ip[11] = static_cast<uint8_t>(new_ip_chk >> 8);

      /* Set ICMP Echo Reply (Type 0) */
      uint8_t* r_icmp = reply + 14 + ip_hl;
      r_icmp[0] = 0; /* Echo Reply */
      r_icmp[2] = 0; r_icmp[3] = 0; /* Clear ICMP checksum */
      size_t icmp_len = len - (14 + ip_hl);
      uint16_t new_icmp_chk = net_chksum(r_icmp, icmp_len);
      r_icmp[2] = static_cast<uint8_t>(new_icmp_chk & 0xFF);
      r_icmp[3] = static_cast<uint8_t>(new_icmp_chk >> 8);

      net_transmit_if(iface.type, reply, len);
    }
  }
}

/* ----------------- DHCP Client Protocol ----------------- */

void send_dhcp_discover(NetInterfaceInfo& iface) {
  uint8_t pkt[512] = {};

  /* Ethernet Header: Broadcast */
  kmemset(pkt + 0, 0xFF, 6);
  kmemcpy(pkt + 6, iface.mac, 6);
  pkt[12] = 0x08; pkt[13] = 0x00; /* IPv4 */

  /* IPv4 Header: 0.0.0.0 -> 255.255.255.255, UDP (17) */
  uint8_t* ip = pkt + 14;
  ip[0] = 0x45; ip[1] = 0x00;
  /* Total length set after DHCP options */
  ip[8] = 64; /* TTL */
  ip[9] = 17; /* UDP */
  kmemset(ip + 12, 0x00, 4); /* Src IP 0.0.0.0 */
  kmemset(ip + 16, 0xFF, 4); /* Dst IP 255.255.255.255 */

  /* UDP Header: Src Port 68, Dst Port 67 */
  uint8_t* udp = pkt + 34;
  udp[0] = 0x00; udp[1] = 68;
  udp[2] = 0x00; udp[3] = 67;

  /* DHCP Payload at offset 42 */
  uint8_t* dhcp = pkt + 42;
  dhcp[0] = 1;  /* BOOTREQUEST */
  dhcp[1] = 1;  /* Ethernet */
  dhcp[2] = 6;  /* HW addr len */
  dhcp[3] = 0;  /* Hops */

  /* Transaction ID */
  uint32_t xid = g_dhcp_xid + static_cast<uint32_t>(iface.type);
  dhcp[4] = static_cast<uint8_t>(xid >> 24);
  dhcp[5] = static_cast<uint8_t>(xid >> 16);
  dhcp[6] = static_cast<uint8_t>(xid >> 8);
  dhcp[7] = static_cast<uint8_t>(xid & 0xFF);

  dhcp[10] = 0x80; /* Flags: Broadcast */
  kmemcpy(dhcp + 28, iface.mac, 6); /* chaddr */

  /* Magic Cookie: 99.130.83.99 (0x63825363) */
  dhcp[236] = 0x63; dhcp[237] = 0x82; dhcp[238] = 0x53; dhcp[239] = 0x63;

  /* Options */
  size_t opt = 240;
  /* Option 53: DHCP Message Type = 1 (Discover) */
  dhcp[opt++] = 53; dhcp[opt++] = 1; dhcp[opt++] = 1;
  /* Option 55: Parameter Request List (1=Subnet, 3=Router, 6=DNS, 15=Domain) */
  dhcp[opt++] = 55; dhcp[opt++] = 4;
  dhcp[opt++] = 1; dhcp[opt++] = 3; dhcp[opt++] = 6; dhcp[opt++] = 15;
  /* Option 255: End */
  dhcp[opt++] = 255;

  /* Pad the BOOTP/DHCP payload to the classic 300-byte BOOTP minimum
   * (236-byte fixed header + 64-byte vendor/options area). Nothing in
   * RFC 2131 requires this, but a lot of real DHCP servers and relay
   * agents - especially consumer router firmware - still enforce the
   * legacy BOOTP minimum and silently drop anything shorter. Our payload
   * was 250 bytes: 236 fixed + 4 magic cookie + 10 bytes of options,
   * 50 bytes under. That is indistinguishable, from everything this
   * driver can observe, from "sent fine, router just never replied" -
   * which is exactly the symptom being chased. pkt[] is already
   * zero-initialized, so padding is just moving `opt` forward. */
  while (opt < 300 && opt < sizeof(pkt) - 42) dhcp[opt++] = 0;

  size_t dhcp_len = opt;
  size_t udp_len = 8 + dhcp_len;
  size_t ip_len = 20 + udp_len;
  size_t frame_len = 14 + ip_len;

  ip[2] = static_cast<uint8_t>(ip_len >> 8);
  ip[3] = static_cast<uint8_t>(ip_len & 0xFF);
  uint16_t ip_chk = net_chksum(ip, 20);
  ip[10] = static_cast<uint8_t>(ip_chk & 0xFF);
  ip[11] = static_cast<uint8_t>(ip_chk >> 8);

  udp[4] = static_cast<uint8_t>(udp_len >> 8);
  udp[5] = static_cast<uint8_t>(udp_len & 0xFF);
  udp[6] = 0; udp[7] = 0; /* zeroed before computing, per RFC 768 */
  {
    const uint16_t udp_chk = net_udp_chksum(ip + 12, ip + 16, udp, udp_len);
    udp[6] = static_cast<uint8_t>(udp_chk & 0xFF);
    udp[7] = static_cast<uint8_t>(udp_chk >> 8);
  }

  iface.ip_cfg.dhcp_state = DhcpState::Selecting;
  const bool queued = net_transmit_if(iface.type, pkt, frame_len);
  kprintf("[%s] DHCP DISCOVER %s (%u bytes)\n",
          iface.type == NetIfType::Ethernet ? "eth" : "wifi",
          queued ? "queued for TX" : "FAILED TO QUEUE - TX ring full/rejected",
          static_cast<uint64_t>(frame_len));
}

void send_dhcp_request(NetInterfaceInfo& iface, const uint8_t offered_ip[4], const uint8_t server_ip[4]) {
  uint8_t pkt[512] = {};

  kmemset(pkt + 0, 0xFF, 6);
  kmemcpy(pkt + 6, iface.mac, 6);
  pkt[12] = 0x08; pkt[13] = 0x00;

  uint8_t* ip = pkt + 14;
  ip[0] = 0x45; ip[1] = 0x00;
  ip[8] = 64; ip[9] = 17;
  kmemset(ip + 12, 0x00, 4);
  kmemset(ip + 16, 0xFF, 4);

  uint8_t* udp = pkt + 34;
  udp[0] = 0x00; udp[1] = 68;
  udp[2] = 0x00; udp[3] = 67;

  uint8_t* dhcp = pkt + 42;
  dhcp[0] = 1; dhcp[1] = 1; dhcp[2] = 6;
  uint32_t xid = g_dhcp_xid + static_cast<uint32_t>(iface.type);
  dhcp[4] = static_cast<uint8_t>(xid >> 24);
  dhcp[5] = static_cast<uint8_t>(xid >> 16);
  dhcp[6] = static_cast<uint8_t>(xid >> 8);
  dhcp[7] = static_cast<uint8_t>(xid & 0xFF);
  dhcp[10] = 0x80;
  kmemcpy(dhcp + 28, iface.mac, 6);

  dhcp[236] = 0x63; dhcp[237] = 0x82; dhcp[238] = 0x53; dhcp[239] = 0x63;

  size_t opt = 240;
  /* Option 53: DHCP Request (3) */
  dhcp[opt++] = 53; dhcp[opt++] = 1; dhcp[opt++] = 3;
  /* Option 50: Requested IP */
  dhcp[opt++] = 50; dhcp[opt++] = 4;
  kmemcpy(dhcp + opt, offered_ip, 4); opt += 4;
  /* Option 54: Server Identifier */
  dhcp[opt++] = 54; dhcp[opt++] = 4;
  kmemcpy(dhcp + opt, server_ip, 4); opt += 4;
  /* Option 55: Parameter Request List */
  dhcp[opt++] = 55; dhcp[opt++] = 4;
  dhcp[opt++] = 1; dhcp[opt++] = 3; dhcp[opt++] = 6; dhcp[opt++] = 15;
  dhcp[opt++] = 255;

  /* Same 300-byte BOOTP minimum padding as send_dhcp_discover(), and for
   * the same reason - this packet was also short. */
  while (opt < 300 && opt < sizeof(pkt) - 42) dhcp[opt++] = 0;

  size_t dhcp_len = opt;
  size_t udp_len = 8 + dhcp_len;
  size_t ip_len = 20 + udp_len;
  size_t frame_len = 14 + ip_len;

  ip[2] = static_cast<uint8_t>(ip_len >> 8);
  ip[3] = static_cast<uint8_t>(ip_len & 0xFF);
  uint16_t ip_chk = net_chksum(ip, 20);
  ip[10] = static_cast<uint8_t>(ip_chk & 0xFF);
  ip[11] = static_cast<uint8_t>(ip_chk >> 8);

  udp[4] = static_cast<uint8_t>(udp_len >> 8);
  udp[5] = static_cast<uint8_t>(udp_len & 0xFF);
  udp[6] = 0; udp[7] = 0;
  {
    const uint16_t udp_chk = net_udp_chksum(ip + 12, ip + 16, udp, udp_len);
    udp[6] = static_cast<uint8_t>(udp_chk & 0xFF);
    udp[7] = static_cast<uint8_t>(udp_chk >> 8);
  }

  iface.ip_cfg.dhcp_state = DhcpState::Requesting;
  net_transmit_if(iface.type, pkt, frame_len);
}

void handle_dhcp_packet(NetInterfaceInfo& iface, const uint8_t* frame, size_t len) {
  /* This function used to say nothing at all, ever - a reply that arrived
   * but got rejected for any reason (too short, not a BOOTREPLY, bad magic
   * cookie) looked identical to a reply that never arrived at all. That
   * made "still unbound" undiagnosable from anything this driver could show. */
  void (*log_fn)(const char*) = (iface.type == NetIfType::Ethernet) ? net_log_eth : net_log_wifi;
  const uint32_t ip_hl = (frame[14] & 0x0Fu) * 4u;
  const size_t dhcp_off = 14 + ip_hl + 8;

  if (len < dhcp_off + 240) {
    kprintf("[%s] DHCP packet too short to be BOOTP (%u bytes, need %u)\n",
            iface.type == NetIfType::Ethernet ? "eth" : "wifi",
            static_cast<uint64_t>(len), static_cast<uint64_t>(dhcp_off + 240));
    log_fn("DHCP reply too short - dropped");
    return;
  }

  const uint8_t* dhcp = frame + dhcp_off;
  if (dhcp[0] != 2) {
    kprintf("[%s] DHCP packet op=%u, not a BOOTREPLY(2) - dropped\n",
            iface.type == NetIfType::Ethernet ? "eth" : "wifi",
            static_cast<uint64_t>(dhcp[0]));
    return;
  }

  /* Verify Magic Cookie */
  if (dhcp[236] != 0x63 || dhcp[237] != 0x82 || dhcp[238] != 0x53 || dhcp[239] != 0x63) {
    kprintf("[%s] DHCP magic cookie wrong (%02x%02x%02x%02x) - dropped\n",
            iface.type == NetIfType::Ethernet ? "eth" : "wifi",
            static_cast<uint64_t>(dhcp[236]), static_cast<uint64_t>(dhcp[237]),
            static_cast<uint64_t>(dhcp[238]), static_cast<uint64_t>(dhcp[239]));
    return;
  }

  const uint8_t* yiaddr = dhcp + 16;
  uint8_t msg_type = 0;
  uint8_t server_id[4] = {};
  uint8_t subnet_mask[4] = {};
  uint8_t router[4] = {};
  uint8_t dns[4] = {};
  uint32_t lease_sec = 86400;

  size_t opt = 240;
  while (opt < len - dhcp_off) {
    uint8_t tag = dhcp[opt++];
    if (tag == 255) break;
    if (tag == 0) continue;
    if (opt >= len - dhcp_off) break;
    uint8_t opt_len = dhcp[opt++];
    if (opt + opt_len > len - dhcp_off) break;

    if (tag == 53 && opt_len == 1) msg_type = dhcp[opt];
    else if (tag == 54 && opt_len == 4) kmemcpy(server_id, dhcp + opt, 4);
    else if (tag == 1 && opt_len == 4) kmemcpy(subnet_mask, dhcp + opt, 4);
    else if (tag == 3 && opt_len >= 4) kmemcpy(router, dhcp + opt, 4);
    else if (tag == 6 && opt_len >= 4) kmemcpy(dns, dhcp + opt, 4);
    else if (tag == 51 && opt_len == 4) {
      lease_sec = (dhcp[opt] << 24) | (dhcp[opt + 1] << 16) | (dhcp[opt + 2] << 8) | dhcp[opt + 3];
    }

    opt += opt_len;
  }

  /* DHCP OFFER (2) -> Send DHCP REQUEST */
  if (msg_type == 2 && iface.ip_cfg.dhcp_state == DhcpState::Selecting) {
    kmemcpy(iface.ip_cfg.server_ip, server_id, 4);
    send_dhcp_request(iface, yiaddr, server_id);
  }
  /* DHCP ACK (5) -> Commit IP configuration */
  else if (msg_type == 5 && (iface.ip_cfg.dhcp_state == DhcpState::Requesting || iface.ip_cfg.dhcp_state == DhcpState::Selecting)) {
    kmemcpy(iface.ip_cfg.ip, yiaddr, 4);
    kmemcpy(iface.ip_cfg.netmask, subnet_mask, 4);
    kmemcpy(iface.ip_cfg.gateway, router, 4);
    kmemcpy(iface.ip_cfg.dns, dns, 4);
    iface.ip_cfg.lease_time_sec = lease_sec;
    iface.ip_cfg.lease_start_ns = metal_now_ns();
    iface.ip_cfg.dhcp_bound = true;
    iface.ip_cfg.dhcp_state = DhcpState::Bound;
    char ip_s[20];
    ip_to_str(yiaddr, ip_s, sizeof(ip_s));
    char msg[NET_LOG_LINE_LEN] = "DHCP bound: ";
    size_t p = kstrlen(msg);
    for (size_t i = 0; ip_s[i] && p + 1 < sizeof(msg); ++i) msg[p++] = ip_s[i];
    msg[p] = '\0';
    (iface.type == NetIfType::Ethernet ? net_log_eth : net_log_wifi)(msg);
  } else {
    /* A structurally valid DHCP reply that we did NOT act on - either the
     * wrong message type, or the right type at a point in the state
     * machine where we were not expecting it (e.g. an OFFER after we had
     * already moved on, or a stray ACK). This used to be silently
     * swallowed with nothing to show for it, indistinguishable from no
     * reply arriving at all. */
    kprintf("[%s] DHCP reply msg_type=%u ignored (state=%u)\n",
            iface.type == NetIfType::Ethernet ? "eth" : "wifi",
            static_cast<uint64_t>(msg_type), static_cast<uint64_t>(iface.ip_cfg.dhcp_state));
  }
}

/* ----------------- Remote Console (LAN debug access) -----------------
 *
 * Minimal, unauthenticated UDP text console: send a command line as a UDP
 * datagram to this machine's IP on AIOS_CONSOLE_PORT, get the same output
 * `dispatch_shell_command()` would print locally back as one UDP reply
 * from the same port. From a laptop on the same LAN:
 *   echo -n "info" | nc -u -w1 <pi-ip> 42424
 * There is no TCP stack here (see aios-metal-arm64-qemu-drive memory /
 * build-arm64.sh header comment - this is a freestanding kernel, nothing
 * from the hosted AIOS/ASSAY substrate's own discovery or console-relay
 * protocol runs here), so this is a fresh, minimal protocol shaped for
 * what this kernel can actually do, not a port of that one.
 *
 * No authentication of any kind - anyone who can reach the LAN can run
 * `halt`/`reset`/`run N` on this machine. Fine for a bench/lab network,
 * not for anything exposed further than that. */

constexpr uint16_t AIOS_CONSOLE_PORT = 42424;
constexpr uint16_t AIOS_BEACON_PORT  = 42423;
constexpr uint64_t AIOS_BEACON_INTERVAL_NS = 5000000000ull; /* 5s */
uint64_t g_last_beacon_ns = 0;

/* Build and send Eth+IP+UDP+payload, addressed directly (no ARP - the
 * caller already has a MAC, either from an incoming frame or the LAN
 * broadcast address). */
void send_udp(NetInterfaceInfo& iface, const uint8_t dst_mac[6], const uint8_t dst_ip[4],
              uint16_t src_port, uint16_t dst_port, const void* payload, size_t payload_len) {
  /* Zero-initialized, unlike the two DHCP send functions above which
   * always were: this left the IP header's identification and
   * flags/fragment-offset fields (never explicitly written below) as
   * uninitialized stack garbage on every console reply and beacon. */
  /* Full Ethernet MTU. This was 1024 bytes, and anything larger was
   * dropped without a word - which is also what made a back-to-back
   * burst test look like the NIC lost every frame over 1 KB. */
  uint8_t pkt[NET_MAX_FRAME_SIZE] = {};
  if (14 + 20 + 8 + payload_len > 1514) return;

  kmemcpy(pkt + 0, dst_mac, 6);
  kmemcpy(pkt + 6, iface.mac, 6);
  pkt[12] = 0x08; pkt[13] = 0x00;

  uint8_t* ip = pkt + 14;
  ip[0] = 0x45; ip[1] = 0x00;
  ip[8] = 64; ip[9] = 17; /* UDP */
  kmemcpy(ip + 12, iface.ip_cfg.ip, 4);
  kmemcpy(ip + 16, dst_ip, 4);

  uint8_t* udp = pkt + 34;
  udp[0] = static_cast<uint8_t>(src_port >> 8); udp[1] = static_cast<uint8_t>(src_port & 0xFF);
  udp[2] = static_cast<uint8_t>(dst_port >> 8); udp[3] = static_cast<uint8_t>(dst_port & 0xFF);

  kmemcpy(pkt + 42, payload, payload_len);

  size_t udp_len = 8 + payload_len;
  size_t ip_len = 20 + udp_len;
  size_t frame_len = 14 + ip_len;

  ip[2] = static_cast<uint8_t>(ip_len >> 8);
  ip[3] = static_cast<uint8_t>(ip_len & 0xFF);
  uint16_t ip_chk = net_chksum(ip, 20);
  ip[10] = static_cast<uint8_t>(ip_chk & 0xFF);
  ip[11] = static_cast<uint8_t>(ip_chk >> 8);

  udp[4] = static_cast<uint8_t>(udp_len >> 8);
  udp[5] = static_cast<uint8_t>(udp_len & 0xFF);
  udp[6] = 0; udp[7] = 0;
  /* net_udp_chksum() falls back to 0 ("not computed", still RFC-legal) for
   * a segment too big for its scratch buffer - a large console reply can
   * exceed that. Real for everything DHCP-sized, which is what mattered. */
  {
    const uint16_t udp_chk = net_udp_chksum(ip + 12, ip + 16, udp, udp_len);
    udp[6] = static_cast<uint8_t>(udp_chk & 0xFF);
    udp[7] = static_cast<uint8_t>(udp_chk >> 8);
  }

  net_transmit_if(iface.type, pkt, frame_len);
}

/* ----------------- Wall clock: DNS + SNTP -----------------
 *
 * The board has no real-time clock. The desktop's top bar used to print the
 * literal string "08:26 AM" on this substrate, and time() returned a fixed
 * 2026 date. Now: once eth0 has a lease, resolve pool.ntp.org through the
 * DHCP-supplied DNS server, ask it for the time (SNTPv4, RFC 4330), and keep
 * the offset against the free-running counter. Non-blocking: every step is
 * a state change driven from net_poll(); replies arrive through the normal
 * UDP dispatch. Resynced hourly; retried every 30 s until the first success. */
constexpr uint16_t DNS_LOCAL_PORT = 53053;
constexpr uint16_t NTP_LOCAL_PORT = 12300;
struct SntpState {
  int state = 0;                 /* 0 idle, 1 waiting for DNS, 2 waiting for NTP */
  uint64_t next_ns = 0, sent_ns = 0;
  uint16_t dns_id = 0x5A17;
  uint8_t server[4] = {0, 0, 0, 0};
  unsigned attempts = 0;
  bool synced = false;
  int64_t unix_ns_at_sync = 0;
  uint64_t mono_ns_at_sync = 0;
  char server_name[32] = "";
};
SntpState g_sntp;

bool same_subnet(const NetInterfaceInfo& iface, const uint8_t ip[4]) {
  for (int i = 0; i < 4; ++i)
    if ((iface.ip_cfg.ip[i] & iface.ip_cfg.netmask[i]) != (ip[i] & iface.ip_cfg.netmask[i])) return false;
  return true;
}
/* The MAC to put on a frame for this IP, or null after asking ARP for it. */
const uint8_t* next_hop_mac(NetInterfaceInfo& iface, const uint8_t ip[4]) {
  const uint8_t* hop = same_subnet(iface, ip) ? ip : iface.ip_cfg.gateway;
  const uint8_t* mac = arp_lookup(hop);
  if (!mac) send_arp_request(iface, hop);
  return mac;
}

void sntp_send_dns(NetInterfaceInfo& iface) {
  const uint8_t* dns = iface.ip_cfg.dns;
  if (!dns[0] && !dns[1] && !dns[2] && !dns[3]) dns = iface.ip_cfg.gateway;
  const uint8_t* mac = next_hop_mac(iface, dns);
  if (!mac) return;                               /* ARP asked; the next poll retries */
  uint8_t q[64] = {};
  ++g_sntp.dns_id;
  q[0] = static_cast<uint8_t>(g_sntp.dns_id >> 8); q[1] = static_cast<uint8_t>(g_sntp.dns_id);
  q[2] = 0x01;                                   /* recursion desired */
  q[5] = 1;                                      /* one question */
  size_t n = 12;
  const char* name = "pool.ntp.org";
  while (*name) {
    const char* dot = name; while (*dot && *dot != '.') ++dot;
    q[n++] = static_cast<uint8_t>(dot - name);
    while (name < dot) q[n++] = static_cast<uint8_t>(*name++);
    if (*name == '.') ++name;
  }
  q[n++] = 0;
  q[n++] = 0; q[n++] = 1;                        /* type A */
  q[n++] = 0; q[n++] = 1;                        /* class IN */
  send_udp(iface, mac, dns, DNS_LOCAL_PORT, 53, q, n);
  g_sntp.state = 1;
  g_sntp.sent_ns = metal_now_ns();
}

void sntp_send_ntp(NetInterfaceInfo& iface) {
  const uint8_t* mac = next_hop_mac(iface, g_sntp.server);
  if (!mac) { g_sntp.state = 0; g_sntp.next_ns = metal_now_ns() + 1000000000ull; return; }
  uint8_t p[48] = {};
  p[0] = 0x23;                                   /* LI 0, version 4, mode 3 (client) */
  send_udp(iface, mac, g_sntp.server, NTP_LOCAL_PORT, 123, p, sizeof(p));
  g_sntp.state = 2;
  g_sntp.sent_ns = metal_now_ns();
}

void sntp_poll() {
  NetInterfaceInfo& iface = g_eth_if;
  if (!iface.initialized || !iface.ip_cfg.dhcp_bound) return;
  const uint64_t now = metal_now_ns();
  if (g_sntp.state != 0 && now - g_sntp.sent_ns > 3000000000ull) {   /* no answer in 3 s */
    g_sntp.state = 0;
    g_sntp.next_ns = now + 30000000000ull;
  }
  if (g_sntp.state == 0 && now >= g_sntp.next_ns) {
    ++g_sntp.attempts;
    sntp_send_dns(iface);
    /* Sent: a lost reply is retried in 30 s. Not sent (the router's MAC was
     * not in the ARP cache yet - the usual case right after boot): try again
     * in 1 s, not 30, so the clock is not blank for half a minute. */
    g_sntp.next_ns = now + (g_sntp.state == 1 ? 30000000000ull : 1000000000ull);
  }
}

void handle_dns_reply(const uint8_t* d, size_t len) {
  if (g_sntp.state != 1 || len < 12) return;
  if (((d[0] << 8) | d[1]) != g_sntp.dns_id) return;
  const unsigned qd = (d[4] << 8) | d[5], an = (d[6] << 8) | d[7];
  size_t o = 12;
  auto skip_name = [&]() {
    while (o < len) {
      const uint8_t l = d[o];
      if ((l & 0xC0) == 0xC0) { o += 2; return; }
      o += 1u + l;
      if (l == 0) return;
    }
  };
  for (unsigned i = 0; i < qd && o < len; ++i) { skip_name(); o += 4; }
  for (unsigned i = 0; i < an && o + 10 <= len; ++i) {
    skip_name();
    if (o + 10 > len) return;
    const unsigned type = (d[o] << 8) | d[o + 1];
    const unsigned rdlen = (d[o + 8] << 8) | d[o + 9];
    o += 10;
    if (type == 1 && rdlen == 4 && o + 4 <= len) {
      kmemcpy(g_sntp.server, d + o, 4);
      sntp_send_ntp(g_eth_if);
      return;
    }
    o += rdlen;
  }
}

void handle_ntp_reply(const uint8_t* d, size_t len) {
  if (g_sntp.state != 2 || len < 48) return;
  if ((d[0] & 7) != 4 || d[1] == 0) return;      /* server mode, non-zero stratum */
  const uint64_t now = metal_now_ns();
  const uint32_t secs = (static_cast<uint32_t>(d[40]) << 24) | (d[41] << 16) | (d[42] << 8) | d[43];
  const uint32_t frac = (static_cast<uint32_t>(d[44]) << 24) | (d[45] << 16) | (d[46] << 8) | d[47];
  const int64_t unix_s = static_cast<int64_t>(secs) - 2208988800ll;   /* NTP era 0 epoch is 1900 */
  const int64_t frac_ns = static_cast<int64_t>((static_cast<uint64_t>(frac) * 1000000000ull) >> 32);
  const int64_t half_rtt = static_cast<int64_t>((now - g_sntp.sent_ns) / 2);
  g_sntp.unix_ns_at_sync = unix_s * 1000000000ll + frac_ns + half_rtt;
  g_sntp.mono_ns_at_sync = now;
  const bool first = !g_sntp.synced;
  g_sntp.synced = true;
  g_sntp.state = 0;
  g_sntp.next_ns = now + 3600000000000ull;       /* hourly */
  char ip_s[20]; ip_to_str(g_sntp.server, ip_s, sizeof(ip_s));
  if (first) kprintf("[net] wall clock set by NTP from %s (pool.ntp.org), stratum %u\n", ip_s, static_cast<uint64_t>(d[1]));
}

/* Immediately transmit whatever is queued on the given interface's TX
 * ring, instead of leaving it for net_poll()'s own drain to pick up next
 * call. handle_console_packet() runs FROM INSIDE net_poll() (via the RX
 * path that dispatches this packet in the first place) - by the time it
 * queues a reply, this net_poll() call's own TX-drain phase has already
 * run for this iteration, so anything queued here would otherwise sit
 * until the NEXT net_poll() call. A multi-chunk reply that queues several
 * packets in a tight loop with nothing draining between them is exactly
 * the shape of bug that looks fine in the ring (each push succeeds, ring
 * never overflows) but only the last chunk was ever reliably observed to
 * arrive - use this after each chunk instead of trusting the next poll. */
void drain_tx_now(NetInterfaceInfo& iface) {
  PacketRing& ring = (iface.type == NetIfType::Ethernet) ? g_eth_tx_ring : g_wifi_tx_ring;
  while (ring.count > 0) {
    uint8_t frame[NET_MAX_FRAME_SIZE];
    size_t flen = 0;
    if (!ring.pop(frame, sizeof(frame), flen)) break;
    if (iface.type == NetIfType::Ethernet) {
      if (lan9514_tx_frame(frame, flen)) {
        iface.stats.tx_packets++;
        iface.stats.tx_bytes += flen;
      } else {
        iface.stats.tx_errors++;
      }
    } else {
      if (dhd_tx_frame(frame, flen)) {
        iface.stats.tx_packets++;
        iface.stats.tx_bytes += flen;
      } else {
        iface.stats.tx_errors++;
      }
    }
  }
}

extern "C" void desktop_render_wait();   /* desktop_shell.cpp: a frame painting on another core */

/* Off by default: this UDP console has no authentication of any kind, and
 * anyone on the LAN reaching it could already run `halt`/`reset`/anything
 * else this board's console understands. `fleet run` (fleet_shell.cpp)
 * reaches the SAME dispatch_shell_command() through the cluster key's
 * authenticated, end-to-end encrypted node link instead - that path calls
 * dispatch_shell_command() directly, never through here, so it is not
 * gated by this flag and stays the way in even while this is off.
 *
 * The toggle is deliberately NOT reachable from a command THIS gate
 * accepts - `console debug on` sent over the open UDP console would let
 * anyone on the LAN turn its own lock back off. Flip it with `console
 * debug on|off` over `fleet run`, or from the physical/serial console
 * (dispatch_shell_command() called directly there too, not through this
 * function) - both are unaffected. */
bool g_console_debug = false;

void handle_console_packet(NetInterfaceInfo& iface, const uint8_t* frame, size_t len) {
  const uint8_t* ip_hdr = frame + 14;
  uint32_t ip_hl = (ip_hdr[0] & 0x0F) * 4;
  if (len < 14 + ip_hl + 8) return;

  const uint8_t* udp = frame + 14 + ip_hl;
  uint16_t udp_len = static_cast<uint16_t>((udp[4] << 8) | udp[5]);
  if (udp_len < 8) return;
  size_t avail = len - (14 + ip_hl + 8);
  size_t udp_payload = static_cast<size_t>(udp_len) - 8;
  size_t payload_len = (udp_payload < avail) ? udp_payload : avail;
  /* Up to a full frame's worth of command. This was 200 bytes and anything
   * longer was dropped without a word - a long `fs write`, or an upload
   * chunk, simply never ran. */
  if (payload_len == 0) return;
  if (payload_len > 1400) {
    static const char too_long[] = "  console: command longer than 1400 bytes - not run\n";
    send_udp(iface, frame + 6, ip_hdr + 12, AIOS_CONSOLE_PORT, static_cast<uint16_t>((udp[0] << 8) | udp[1]),
             too_long, sizeof(too_long) - 1);
    return;
  }

  char cmd[1401];
  kmemcpy(cmd, udp + 8, payload_len);
  cmd[payload_len] = '\0';
  while (payload_len > 0 && (cmd[payload_len - 1] == '\n' || cmd[payload_len - 1] == '\r')) {
    cmd[--payload_len] = '\0';
  }
  if (payload_len == 0) return;

  /* Copied now: `frame` is the receive buffer, and a command that services
   * the network while it runs (sockets, sys_exec, a long program) can have
   * later packets land in it - the reply then went to whoever sent last. */
  const uint16_t sender_port = static_cast<uint16_t>((udp[0] << 8) | udp[1]);
  uint8_t sender_ip[4], sender_mac[6];
  kmemcpy(sender_ip, ip_hdr + 12, 4);
  kmemcpy(sender_mac, frame + 6, 6);

  /* Numbered chunks ("[2/5] ...") when the reply needs more than one frame -
   * and always for an "@ID" request, even "[1/1]", so that client knows the
   * moment a reply is complete instead of waiting out a quiet period. */
  bool numbered = false;
  auto send_reply = [&](const char* out, size_t out_len) {
    constexpr size_t CHUNK_DATA = 1400;
    const unsigned total_chunks = static_cast<unsigned>(out_len == 0 ? 1 : (out_len + CHUNK_DATA - 1) / CHUNK_DATA);
    metal_delay_us(2000);
    for (unsigned i = 0; i < total_chunks; ++i) {
      char piece[CHUNK_DATA + 32];
      size_t pl = 0;
      if (total_chunks > 1 || numbered) {
        auto put_num = [&](unsigned v) { char t[12]; int k = 0; do { t[k++] = static_cast<char>('0' + v % 10); v /= 10; } while (v); while (k) piece[pl++] = t[--k]; };
        piece[pl++] = '[';
        put_num(i + 1);
        piece[pl++] = '/';
        put_num(total_chunks);
        piece[pl++] = ']';
        piece[pl++] = ' ';
      }
      const size_t from = static_cast<size_t>(i) * CHUNK_DATA;
      const size_t take = (out_len - from < CHUNK_DATA) ? (out_len - from) : CHUNK_DATA;
      for (size_t k = 0; k < take; ++k) piece[pl++] = out[from + k];
      send_udp(iface, sender_mac, sender_ip, AIOS_CONSOLE_PORT, sender_port, piece, pl);
      drain_tx_now(iface);
      metal_delay_us(1000);   /* a short gap, kind to slow receivers */
    }
  };

  /* "@ID command": at-most-once. The reply is kept under its ID; the same ID
   * again (a client retrying because a reply frame was lost) gets the kept
   * reply without the command running twice, and is ignored while the
   * command is still running. Plain commands behave as before. */
  struct KeptReply { char id[24]; bool running; bool done; uint64_t used; size_t len; char data[65536]; };
  static KeptReply kept[4];
  static uint64_t kept_clock = 0;
  const char* run = cmd;
  KeptReply* entry = nullptr;
  if (cmd[0] == '@') {
    numbered = true;
    char id[24]; size_t il = 0;
    const char* q = cmd + 1;
    while (*q && *q != ' ' && il + 1 < sizeof(id)) id[il++] = *q++;
    id[il] = '\0';
    while (*q == ' ') ++q;
    run = q;
    for (auto& k : kept) {
      bool same = k.id[0] != '\0';
      for (size_t i = 0; same && i <= il; ++i) same = k.id[i] == id[i];
      if (same) { entry = &k; break; }
    }
    if (entry && entry->running) return;
    if (entry && entry->done) { entry->used = ++kept_clock; send_reply(entry->data, entry->len); return; }
    entry = &kept[0];
    for (auto& k : kept) if (!k.running && k.used < entry->used) entry = &k;
    kmemcpy(entry->id, id, il + 1);
    entry->running = true;
    entry->done = false;
    entry->used = ++kept_clock;
    if (*run == '\0') { entry->running = false; entry->id[0] = '\0'; return; }
  }

  if (!g_console_debug) {
    static const char locked[] =
        "  console: locked - this open UDP console is off by default (anyone on the LAN can\n"
        "  reach it with no authentication). Use `fleet run <peer> ...` (the cluster key,\n"
        "  end-to-end encrypted) instead, or `console debug on` from the physical console\n"
        "  or over fleet run to open this one too.\n";
    if (entry) { entry->running = false; entry->id[0] = '\0'; }
    send_reply(locked, sizeof(locked) - 1);
    return;
  }

  /* One capture buffer per nesting level: `desktop` keeps the console alive
   * from inside its frame loop, so a command can arrive while another is
   * still running - and a shared buffer let the inner reply overwrite the
   * outer one's. Deeper than two is refused, not corrupted. */
  static char outs[2][65536];
  static int depth = 0;
  if (depth >= 2) {
    static const char busy[] = "  console: busy - two commands are already running\n";
    if (entry) { entry->running = false; entry->id[0] = '\0'; }
    send_reply(busy, sizeof(busy) - 1);
    return;
  }
  char* out = outs[depth++];
  /* A command can read or change the kernel; a frame painting on another
   * core is reading it too. Let it finish (tens of ms). */
  desktop_render_wait();
  kconsole_capture_start(out, sizeof(outs[0]));
  dispatch_shell_command(run);
  size_t out_len = kconsole_capture_stop();
  --depth;
  if (entry) {
    entry->len = out_len < sizeof(entry->data) ? out_len : sizeof(entry->data);
    kmemcpy(entry->data, out, entry->len);
    entry->done = true;
    entry->running = false;
  }

  /* The network log is on the screen. Commands that carry secrets or bulk
   * payload - a fleet cluster key, a Wi-Fi password, an upload's base64 -
   * are logged by name only. */
  char logmsg[NET_LOG_LINE_LEN] = "console cmd: ";
  size_t p = kstrlen(logmsg);
  auto starts_with = [&](const char* w) { size_t i = 0; for (; w[i]; ++i) if (run[i] != w[i]) return false; return run[i] == ' ' || run[i] == '\0'; };
  const bool secret = starts_with("fleet") || starts_with("wifi") || starts_with("fs put") || starts_with("fs b64") || starts_with("node start");
  size_t upto = 0;
  if (secret) { int spaces = 0; while (run[upto] && !(run[upto] == ' ' && ++spaces == (starts_with("fs put") || starts_with("fs b64") ? 3 : starts_with("node start") ? 2 : 1))) ++upto; }
  for (size_t i = 0; run[i] && p + 1 < sizeof(logmsg) && (!secret || i < upto); ++i) logmsg[p++] = run[i];
  if (secret) { const char* r = " (arguments not logged)"; while (*r && p + 1 < sizeof(logmsg)) logmsg[p++] = *r++; }
  logmsg[p] = '\0';
  (iface.type == NetIfType::Ethernet ? net_log_eth : net_log_wifi)(logmsg);

  send_reply(out, out_len);
}

}  // namespace

/* External linkage for main.cpp's `console` shell command - g_console_debug
 * itself stays internal (unnamed-namespace, above), only handle_console_packet
 * and these two need to see it, and both are in this file. */
extern "C" bool console_debug_enabled() { return g_console_debug; }
extern "C" void console_debug_set(bool on) { g_console_debug = on; }

/* Live breadcrumbs: one UDP broadcast per line on tcp... udp/42425 from eth0.
 * Unlike the SD crash log, this needs nothing but the NIC, which keeps
 * working right up to a hang - so the last lines received are the last
 * thing the board did. Listen with `nc -ulk 42425`. */
size_t net_wifi_last_scan(NetScanEntry* out, size_t max, uint32_t* generation) {
  if (generation) *generation = g_wifi_scan_generation;
  size_t n = 0;
  for (; out && n < g_wifi_auto_scan_count && n < max; ++n) out[n] = g_wifi_auto_scan_cache[n];
  return n;
}

/* Send n numbered breadcrumb-port broadcasts straight to the chip, back to
 * back (console `net burst N [len]`); the listener counts what arrives. */
void net_burst_test(unsigned n, size_t payload) {
  if (!g_eth_if.initialized) return;
  if (payload < 16) payload = 16;
  if (payload > 1400) payload = 1400;
  char msg[1400];
  for (size_t i = 0; i < payload; ++i) msg[i] = 'x';
  const char* tag = "burst "; for (size_t i = 0; tag[i]; ++i) msg[i] = tag[i];
  const uint8_t bcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  const uint8_t bcast_ip[4] = {255, 255, 255, 255};
  drain_tx_now(g_eth_if);
  send_udp(g_eth_if, bcast_mac, bcast_ip, 42425, 42425, msg, payload);
  uint8_t frame[NET_MAX_FRAME_SIZE]; size_t flen = 0;
  if (!g_eth_tx_ring.pop(frame, sizeof(frame), flen)) return;
  lan9514_burst_test(frame, flen, n);
}

extern "C" bool metal_wall_unix_ns(int64_t* out) {
  if (!g_sntp.synced) return false;
  if (out) *out = g_sntp.unix_ns_at_sync + static_cast<int64_t>(metal_now_ns() - g_sntp.mono_ns_at_sync);
  return true;
}

void net_breadcrumb_flush() {
  if (g_eth_if.initialized && g_eth_if.link_up) drain_tx_now(g_eth_if);
}

void net_breadcrumb(const char* msg) {
  if (!msg || !g_eth_if.initialized || !g_eth_if.link_up) return;
  const uint8_t bcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  const uint8_t bcast_ip[4] = {255, 255, 255, 255};
  size_t n = 0;
  while (msg[n] && n < 900) ++n;
  send_udp(g_eth_if, bcast_mac, bcast_ip, 42425, 42425, msg, n);
}

namespace {

/* ----------------- UDP sockets (net_udp_*) -----------------
 * A small table of bound ports, each with a queue of whole datagrams. The
 * RX dispatch below hands anything addressed to a bound port here; a
 * datagram to this machine's own address never touches the wire - it goes
 * straight into the local socket's queue, so a program can talk to itself.
 * One frame per datagram (no IP fragmentation): 1472 bytes of payload. */
constexpr int UDP_MAX_SOCKETS = 8;
constexpr int UDP_QUEUE = 16;
constexpr size_t UDP_MAX_PAYLOAD = 1472;
struct UdpDatagram { uint8_t ip[4]; uint16_t port; uint16_t len; uint8_t data[UDP_MAX_PAYLOAD]; };
struct UdpSocketSlot {
  bool used = false;
  uint16_t port = 0;
  unsigned head = 0, count = 0;
  uint64_t dropped = 0;
  UdpDatagram q[UDP_QUEUE];
};
UdpSocketSlot g_udp[UDP_MAX_SOCKETS];

UdpSocketSlot* udp_slot_for_port(uint16_t port) {
  for (auto& u : g_udp) if (u.used && u.port == port) return &u;
  return nullptr;
}
void udp_enqueue(UdpSocketSlot& u, const uint8_t ip[4], uint16_t port, const uint8_t* data, size_t len) {
  if (u.count == UDP_QUEUE) { ++u.dropped; return; }           /* full: newest is dropped, and counted */
  UdpDatagram& d = u.q[(u.head + u.count) % UDP_QUEUE];
  kmemcpy(d.ip, ip, 4);
  d.port = port;
  d.len = static_cast<uint16_t>(len > UDP_MAX_PAYLOAD ? UDP_MAX_PAYLOAD : len);
  kmemcpy(d.data, data, d.len);
  ++u.count;
}

/* Periodic broadcast so a listener on the LAN (`nc -ul 42423`) and P2PMeshDiscovery
 * can find this machine without already knowing its IP. */
void send_beacon(NetInterfaceInfo& iface) {
  if (!iface.ip_cfg.dhcp_bound) return;
  char ip_s[20], mac_s[24];
  ip_to_str(iface.ip_cfg.ip, ip_s, sizeof(ip_s));
  mac_to_str(iface.mac, mac_s, sizeof(mac_s));

  char msg[128] = "AIOS-METAL-ARM64 ";
  size_t p = kstrlen(msg);
  for (size_t i = 0; ip_s[i] && p + 1 < sizeof(msg); ++i) msg[p++] = ip_s[i];
  msg[p++] = ' ';
  for (size_t i = 0; mac_s[i] && p + 1 < sizeof(msg); ++i) msg[p++] = mac_s[i];
  msg[p] = '\0';

  uint8_t bcast_mac[6]; kmemset(bcast_mac, 0xFF, 6);
  uint8_t bcast_ip[4]; kmemset(bcast_ip, 0xFF, 4);
  send_udp(iface, bcast_mac, bcast_ip, AIOS_BEACON_PORT, AIOS_BEACON_PORT, msg, kstrlen(msg));

  /* P2P mesh / substrate discovery announcement (AIOS_P2P_MDNS_v1:<node_id>:<ip>:<port>:active) */
  char mdns_msg[160] = "AIOS_P2P_MDNS_v1:rpi3-arm64-";
  size_t mp = kstrlen(mdns_msg);
  for (size_t i = 0; mac_s[i] && mp + 1 < sizeof(mdns_msg); ++i) {
    if (mac_s[i] != ':') mdns_msg[mp++] = mac_s[i];
  }
  if (mp + 1 < sizeof(mdns_msg)) mdns_msg[mp++] = ':';
  for (size_t i = 0; ip_s[i] && mp + 1 < sizeof(mdns_msg); ++i) mdns_msg[mp++] = ip_s[i];
  const char* suffix = ":7421:active";
  for (size_t i = 0; suffix[i] && mp + 1 < sizeof(mdns_msg); ++i) mdns_msg[mp++] = suffix[i];
  mdns_msg[mp] = '\0';

  send_udp(iface, bcast_mac, bcast_ip, AIOS_BEACON_PORT, AIOS_BEACON_PORT, mdns_msg, mp);
}

void parse_incoming_ethernet_frame(NetInterfaceInfo& iface, const uint8_t* frame, size_t len) {
  if (len < 14) return;
  uint16_t ethertype = (frame[12] << 8) | frame[13];

  if (ethertype == 0x0806) {
    handle_arp_packet(iface, frame, len);
  } else if (ethertype == 0x0800 && len >= 34) {
    uint8_t proto = frame[23];
    if (proto == 1) {
      handle_icmp_packet(iface, frame, len);
    } else if (proto == 6) {
      handle_tcp_packet(iface, frame, len);
    } else if (proto == 17 && len >= 34) {
      /* This assumed a fixed 20-byte IP header (dst_port always at byte
       * 36/37). handle_icmp_packet already computes ip_hl dynamically from
       * the header's own IHL field a few lines away in this same file -
       * this path never did. Most home/office routers send plain 20-byte
       * IP headers so this usually happened to work, but any DHCP server
       * that adds IP options (some do) would have its reply's UDP header
       * read from the wrong offset, computed garbage for dst_port, missed
       * the ==68 check, and been silently dropped - indistinguishable from
       * "no reply ever arrived" from everything this driver can observe. */
      const uint32_t ip_hl = (frame[14] & 0x0Fu) * 4u;
      if (len >= 14u + ip_hl + 8u) {
        const uint8_t* udp = frame + 14 + ip_hl;
        uint16_t dst_port = static_cast<uint16_t>((udp[2] << 8) | udp[3]);
        if (dst_port == 68) {
          handle_dhcp_packet(iface, frame, len);
        } else if (dst_port == AIOS_CONSOLE_PORT) {
          handle_console_packet(iface, frame, len);
        } else if (dst_port == DNS_LOCAL_PORT || dst_port == NTP_LOCAL_PORT) {
          const size_t ulen = (static_cast<size_t>(udp[4]) << 8) | udp[5];
          const size_t avail = len - (14 + ip_hl);
          const size_t plen = (ulen >= 8 && ulen <= avail) ? ulen - 8 : 0;
          if (dst_port == DNS_LOCAL_PORT) handle_dns_reply(udp + 8, plen);
          else handle_ntp_reply(udp + 8, plen);
        } else if (UdpSocketSlot* u = udp_slot_for_port(dst_port)) {
          const size_t ulen = (static_cast<size_t>(udp[4]) << 8) | udp[5];
          const size_t avail = len - (14 + ip_hl);
          const size_t plen = (ulen >= 8 && ulen <= avail) ? ulen - 8 : 0;
          udp_enqueue(*u, frame + 26, static_cast<uint16_t>((udp[0] << 8) | udp[1]), udp + 8, plen);
        }
      }
    }
  }
}

}  /* namespace */

/* ----------------- Core Subsystem Lifecycle ----------------- */

bool net_init_eth_only() {
  g_eth_if = {};
  g_eth_if.type = NetIfType::Ethernet;
  const char eth_name[] = "eth0";
  for (size_t i = 0; i < sizeof(eth_name); ++i) g_eth_if.name[i] = eth_name[i];

  if (lan9514_info().initialized) {
    g_eth_if.initialized = true;
    bool is_up = lan9514_check_link();
    g_eth_if.link_up = is_up;
    g_eth_if.link_state = is_up ? NetLinkState::Up : NetLinkState::Down;
    const auto& lan_inf = lan9514_info();
    kmemcpy(g_eth_if.mac, lan_inf.mac, 6);
    net_log_eth(is_up ? "eth0 initialized, link UP" : "eth0 initialized, no carrier");
  } else if (rpi_get_mac_address(g_eth_if.mac)) {
    g_eth_if.initialized = false;
    g_eth_if.link_up = false;
    g_eth_if.link_state = NetLinkState::Down;
    net_log_eth("LAN9514 not initialized");
  }

  if (g_eth_if.initialized && g_eth_if.link_up) {
    net_start_dhcp(NetIfType::Ethernet);
  }
  return g_eth_if.initialized;
}

bool net_init() {
  const bool eth_ok = net_init_eth_only();

  /* Initialize the wireless interface. The name is DISCOVERED from the
   * driver after bring-up rather than stamped as a literal "wlan0"; only if
   * the driver cannot report one do we fall back to the conventional name. */
  g_wifi_if = {};
  g_wifi_if.type = NetIfType::Wifi;

  /* The radio hangs off the Pi's Arasan SDIO and its clock manager: on a
   * board without the BCM peripheral window there is no radio to probe. */
  if (board_has_bcm_peripherals() && dhd_init()) {
    dhd_get_mac(g_wifi_if.mac);
    g_wifi_if.initialized = true;
    g_wifi_if.link_state = NetLinkState::Up;

    char ifname[16] = {};
    const char* resolved = nullptr;
    if (dhd_get_ifname(ifname)) {
      resolved = ifname;
    } else {
      resolved = "wlan0";  /* last-resort fallback only */
      net_log_wifi("driver did not report an interface name; using wlan0");
    }
    size_t ni = 0;
    while (resolved[ni] && ni + 1 < sizeof(g_wifi_if.name)) {
      g_wifi_if.name[ni] = resolved[ni];
      ++ni;
    }
    g_wifi_if.name[ni] = '\0';

    /* Apply a config-provided regulatory country if wifi.cfg has one.
     * Omitted entirely when absent - never hardcoded here. */
    WifiSavedConfig cfg = {};
    if (wifi_load_saved_config(cfg) && cfg.country[0]) {
      if (dhd_set_country(cfg.country)) {
        net_log_wifi("regulatory country applied from config");
      } else {
        net_log_wifi("config country rejected by dongle");
      }
    }

    net_log_wifi("BCM43438 dongle initialized");
  } else {
    net_log_wifi("dongle init FAILED - see serial log for reason");
  }

  /* net_init_eth_only() already triggered eth0's DHCP if it came up. */
  return eth_ok || g_wifi_if.initialized;
}

bool net_is_up() {
  return (g_eth_if.initialized && g_eth_if.link_up) ||
         (g_wifi_if.initialized && g_wifi_if.link_state == NetLinkState::Associated);
}

bool screenshot_server_start();

namespace {
/* Fire once, the moment eth0 gets a DHCP lease, from INSIDE net_poll() -
 * not from a one-shot bounded wait in main() before the dashboard blocks,
 * and not from the outer console loop alone. Both of those were tried and
 * both missed real boots: DHCP does not always bind inside a short fixed
 * window (see main.cpp's own comment on the wait it used to rely on), and
 * once desktop_run() (the bring-up dashboard) takes the CPU it does not
 * give it back to the console loop until someone presses Escape on a
 * physical keyboard - which never happens on this board's actual
 * deployment. net_poll() is the one thing both the dashboard's frame loop
 * and the console loop already call on every iteration, so it is the one
 * place this is guaranteed to actually run.
 *
 * What this does call, once eth0 has a lease: the screenshot server and the
 * node service - the latter only when the card carries NODE.KEY (its token;
 * there is no compiled-in one any more), so a card without one never reaches
 * Kernel::boot() from here. The history below is why the dial stays manual.
 *
 * fleet_join_test() is deliberately NOT called from here. fleet_join_test ends in net_tcp_connect(), which
 * busy-loops calling net_poll() itself for up to its timeout waiting for
 * the SYN-ACK - and this function runs from INSIDE net_poll(). Dialing an
 * external host (chi-il-1, across the WAN, possibly unreachable from this
 * LAN) synchronously from the first line of a per-frame poll function
 * froze the whole board for the full dial timeout on real hardware.
 * node_service_start() turned out to be its own separate hang risk: it is
 * the first thing to call aios_arm64::kernel() - Kernel::boot() - from an
 * AUTOMATIC path, completely untested in that context (previously only
 * reachable manually, via the `desktop` console command). Auto-starting it
 * here froze the board totally (no console response at all, not just
 * input) the moment DHCP bound - confirmed on real hardware after the
 * fleet_join_test fix alone did not stop the freeze. Both stay manual
 * console commands (`fleet <host> <port> <key>`, `node start`) - a person
 * choosing to wait for one, or to be the first to exercise Kernel::boot()
 * on this board, is the one asking for it. screenshot_server_start() stays
 * automatic: it never touches Kernel, only net_tcp_listen() (a local bind,
 * does not block). */
extern "C" bool node_service_start_from(const char* arg, bool boot);

bool g_bootstrap_tried = false;
void try_local_service_bootstrap() {
  if (g_bootstrap_tried || !net_is_dhcp_bound(NetIfType::Ethernet)) return;
  g_bootstrap_tried = true;
  kputs("\n[net] eth0 has a lease - starting screenshot server & node service (tcp/7421, token from NODE.KEY)...\n");
  screenshot_server_start();
  /* The token is this card's NODE.KEY, never one compiled into the image. */
  node_service_start_from(nullptr, true);
}
}  // namespace

extern "C" void metal_watchdog_pet();

extern "C" void fleet_poll();   /* fleet_shell.cpp: the fleet link, polled - no thread on this kernel */

void net_poll() {
  /* Nesting depth: TCP waits (and the console) call net_poll from inside
   * net_poll. The fleet link is driven only from the outermost call - its own
   * reads pump net_poll, and must not re-enter it. */
  static int depth = 0;
  ++depth;
  struct Leave { ~Leave() { --depth; } } leave;
  metal_watchdog_pet();   /* long waits (scan/join/TCP send) spin on net_poll */
  try_local_service_bootstrap();
  sntp_poll();
  /* 1. Poll SMSC LAN9514 Ethernet (eth0) */
  if (lan9514_info().initialized) {
    bool is_up = lan9514_check_link();
    bool was_up = g_eth_if.link_up;
    g_eth_if.initialized = true;
    g_eth_if.link_up = is_up;
    g_eth_if.link_state = is_up ? NetLinkState::Up : NetLinkState::Down;
    const auto& lan_inf = lan9514_info();
    kmemcpy(g_eth_if.mac, lan_inf.mac, 6);

    /* Link transitioned from Down to Up: Trigger DHCP */
    if (is_up && !was_up) {
      net_log_eth("Link UP - starting DHCP");
      net_start_dhcp(NetIfType::Ethernet);
    }
    /* Link dropped: Reset DHCP lease */
    else if (!is_up && was_up) {
      net_log_eth("Link DOWN - lease cleared");
      g_eth_if.ip_cfg.dhcp_bound = false;
      g_eth_if.ip_cfg.dhcp_state = DhcpState::Init;
      for (int i = 0; i < 4; ++i) g_eth_if.ip_cfg.ip[i] = 0;
    }

    if (is_up) {
      /* Drain TX Ring to LAN9514 */
      while (g_eth_tx_ring.count > 0) {
        uint8_t frame[NET_MAX_FRAME_SIZE];
        size_t flen = 0;
        if (g_eth_tx_ring.pop(frame, sizeof(frame), flen)) {
          if (lan9514_tx_frame(frame, flen)) {
            g_eth_if.stats.tx_packets++;
            g_eth_if.stats.tx_bytes += flen;
          } else {
            g_eth_if.stats.tx_errors++;
          }
        }
      }

      /* Poll RX from LAN9514 */
      for (unsigned i = 0; i < 8; ++i) {
        uint8_t frame[NET_MAX_FRAME_SIZE];
        size_t flen = 0;
        if (!lan9514_rx_poll(frame, sizeof(frame), flen) || flen == 0) break;

        g_eth_if.stats.rx_packets++;
        g_eth_if.stats.rx_bytes += flen;
        g_eth_rx_ring.push(frame, flen, 0);

        parse_incoming_ethernet_frame(g_eth_if, frame, flen);
      }
    }
  }

  /* 2. Poll Broadcom BCM43438 Wi-Fi (wlan0) */
  if (g_wifi_if.initialized) {
    while (g_wifi_tx_ring.count > 0) {
      uint8_t frame[NET_MAX_FRAME_SIZE];
      size_t flen = 0;
      if (g_wifi_tx_ring.pop(frame, sizeof(frame), flen)) {
        if (dhd_tx_frame(frame, flen)) {
          g_wifi_if.stats.tx_packets++;
          g_wifi_if.stats.tx_bytes += flen;
        } else {
          g_wifi_if.stats.tx_errors++;
        }
      }
    }

    for (unsigned i = 0; i < 8; ++i) {
      uint8_t rx_buf[NET_MAX_FRAME_SIZE];
      size_t rx_len = 0;
      uint8_t channel = 0xFF;

      if (!dhd_rx_poll(rx_buf, sizeof(rx_buf), rx_len, channel)) break;

      if (channel == SDPCM_CHANNEL_DATA && rx_len > 0) {
        g_wifi_if.stats.rx_packets++;
        g_wifi_if.stats.rx_bytes += rx_len;
        g_wifi_rx_ring.push(rx_buf, rx_len, 0);

        parse_incoming_ethernet_frame(g_wifi_if, rx_buf, rx_len);
      } else if (channel == SDPCM_CHANNEL_EVENT && rx_len >= sizeof(BcmEventMsg)) {
        const auto* ev = reinterpret_cast<const BcmEventMsg*>(rx_buf);
        /* Every firmware event, named, with its status/reason: a failed join
         * then says why (auth rejected, wrong key, no such SSID...). */
        {
          const char* name = "?";
          switch (ev->event_type) {
            case BCM_E_SET_SSID: name = "SET_SSID"; break;
            case BCM_E_JOIN: name = "JOIN"; break;
            case BCM_E_AUTH: name = "AUTH"; break;
            case BCM_E_DEAUTH: name = "DEAUTH"; break;
            case BCM_E_DEAUTH_IND: name = "DEAUTH_IND"; break;
            case BCM_E_ASSOC: name = "ASSOC"; break;
            case BCM_E_DISASSOC: name = "DISASSOC"; break;
            case BCM_E_DISASSOC_IND: name = "DISASSOC_IND"; break;
            case BCM_E_LINK: name = "LINK"; break;
            case BCM_E_PSK_SUP: name = "PSK_SUP"; break;
            case BCM_E_SCAN_COMPLETE: name = "SCAN_COMPLETE"; break;
            default: break;
          }
          if (ev->event_type != BCM_E_SCAN_COMPLETE) {
            kprintf("[wifi] event %s (%u) status %u reason %u flags 0x%x\n", name,
                    static_cast<uint64_t>(ev->event_type), static_cast<uint64_t>(ev->status),
                    static_cast<uint64_t>(ev->reason), static_cast<uint64_t>(ev->flags));
          }
        }
        if (ev->event_type == BCM_E_LINK) {
          if (ev->flags & 1) g_wifi_if.link_state = NetLinkState::Associated;
          else g_wifi_if.link_state = NetLinkState::Up;
        } else if (ev->event_type == BCM_E_JOIN || ev->event_type == BCM_E_SET_SSID) {
          if (ev->status == 0) {
            g_wifi_if.link_state = NetLinkState::Associated;
            kmemcpy(g_wifi_if.bssid, ev->addr, 6);
            /* Trigger DHCP on Wi-Fi connection */
            net_start_dhcp(NetIfType::Wifi);
          }
        } else if (ev->event_type == BCM_E_DISASSOC || ev->event_type == BCM_E_DEAUTH ||
                   ev->event_type == BCM_E_DISASSOC_IND || ev->event_type == BCM_E_DEAUTH_IND) {
          g_wifi_if.link_state = NetLinkState::Up;
          g_wifi_if.ssid[0] = '\0';
          kmemset(g_wifi_if.bssid, 0, 6);
          g_wifi_if.ip_cfg.dhcp_bound = false;
        } else if (ev->event_type == BCM_E_SCAN_COMPLETE) {
          g_wifi_scan_complete = true;
        } else if (ev->event_type == BCM_E_PSK_SUP) {
          /* 6 = WLC_SUP_KEYED: the 4-way handshake finished and data can
           * flow. DHCP sent before this is dropped by the firmware. */
          if (ev->status == 6 && g_wifi_if.link_state == NetLinkState::Associated) {
            net_start_dhcp(NetIfType::Wifi);
          }
        }
      }
    }
  }

  /* 3. Periodic DHCP Retry for unbound active links */
  uint64_t now = metal_now_ns();
  if (now - g_last_dhcp_poll_ns > 3000000000ull) { /* Every 3 seconds */
    g_last_dhcp_poll_ns = now;
    if (g_eth_if.initialized && g_eth_if.link_up && !g_eth_if.ip_cfg.dhcp_bound) {
      send_dhcp_discover(g_eth_if);
    }
    if (g_wifi_if.initialized && g_wifi_if.link_state == NetLinkState::Associated && !g_wifi_if.ip_cfg.dhcp_bound) {
      send_dhcp_discover(g_wifi_if);
    }
  }

  /* 3b. Automatic Wi-Fi discovery + association - no keyboard required.
   * Every 10s while the radio is up and not yet associated: re-scan, so
   * the desktop Wi-Fi tab's list (backed by the same net_scan()) is
   * already populated the moment anyone looks, without requiring the 's'
   * scan keypress or Scan button first. Trying the saved network (if any)
   * is on its own, much longer 60s timer - net_associate() blocks this
   * whole poll loop for up to 5s while it waits out a WPA2 handshake, and
   * fleet_join_test/node_service_start above were kept manual specifically
   * because an automatic blocking network call froze this board solid on
   * real hardware; retrying a failing/out-of-range AP every 10s would
   * mean the board spends a third of its time frozen. 60s keeps the same
   * blocking call but bounds it to a tolerable duty cycle. */
  /* Both are started here and finished on a later pass - nothing in this
   * loop waits on the radio. */
  if (g_auto_scan_pending && (g_wifi_scan_complete || now - g_auto_scan_started_ns > 5000000000ull)) {
    g_auto_scan_pending = false;
    if (!g_wifi_scan_complete) net_log_wifi("background scan: no E_SCAN_COMPLETE in 5s, reading results anyway");
    if (wifi_scan_collect(g_wifi_auto_scan_cache, kAutoScanMax, g_wifi_auto_scan_count)) ++g_wifi_scan_generation;
  }
  if (g_auto_join_pending) {
    if (g_wifi_if.link_state == NetLinkState::Associated) {
      g_auto_join_pending = false;
      size_t k = 0;
      while (g_auto_join_ssid[k] && k < 32) { g_wifi_if.ssid[k] = g_auto_join_ssid[k]; ++k; }
      g_wifi_if.ssid[k] = '\0';
      kprintf("[wifi] joined saved network '%s'\n", g_auto_join_ssid);
    } else if (now - g_auto_join_started_ns > 15000000000ull) {
      g_auto_join_pending = false;
      g_wifi_if.link_state = NetLinkState::Up;
      kprintf("[wifi] saved network '%s' did not associate within 15s\n", g_auto_join_ssid);
    }
  }
  if (g_wifi_if.initialized && g_wifi_if.link_state != NetLinkState::Associated &&
      g_wifi_if.link_state != NetLinkState::Associating && !g_auto_join_pending) {
    if (!g_auto_scan_pending && now - g_last_wifi_scan_ns > 10000000000ull) { /* Every 10 seconds */
      g_last_wifi_scan_ns = now;
      if (wifi_scan_start()) { g_auto_scan_pending = true; g_auto_scan_started_ns = now; }
    }
    if (!g_auto_scan_pending && now - g_last_wifi_auto_connect_ns > 60000000000ull) { /* Every 60 seconds */
      g_last_wifi_auto_connect_ns = now;
      if (!g_saved_config.valid) wifi_load_saved_config(g_saved_config);
      if (g_saved_config.valid && g_saved_config.auto_connect && g_saved_config.ssid[0]) {
        /* Start the join; the firmware's SET_SSID/LINK events finish it. */
        uint32_t infra = 1;
        dhd_ioctl(WLC_SET_INFRA, &infra, sizeof(infra), true);
        dhd_set_passphrase(g_saved_config.psk[0] ? g_saved_config.psk : nullptr);
        size_t k = 0;
        while (g_saved_config.ssid[k] && k < 32) { g_auto_join_ssid[k] = g_saved_config.ssid[k]; ++k; }
        g_auto_join_ssid[k] = '\0';
        if (dhd_set_ssid(g_auto_join_ssid)) {
          g_wifi_if.link_state = NetLinkState::Associating;
          g_auto_join_pending = true;
          g_auto_join_started_ns = now;
        }
      }
    }
  }

  /* 4. Remote console discovery beacon */
  if (now - g_last_beacon_ns > AIOS_BEACON_INTERVAL_NS) {
    g_last_beacon_ns = now;
    if (g_eth_if.initialized && g_eth_if.link_up) send_beacon(g_eth_if);
    if (g_wifi_if.initialized && g_wifi_if.link_state == NetLinkState::Associated) send_beacon(g_wifi_if);
  }

  /* 5. TCP state machine timer and retransmission poll */
  tcp_poll_timers();

  g_eth_if.tx_ring = g_eth_tx_ring.stats();
  g_eth_if.rx_ring = g_eth_rx_ring.stats();
  g_wifi_if.tx_ring = g_wifi_tx_ring.stats();
  g_wifi_if.rx_ring = g_wifi_rx_ring.stats();

  /* 6. The fleet hub link: heartbeat, roster, circuits (no-op until `fleet`).
   * A peer's request runs against the kernel - not while a frame paints. */
  if (depth == 1) { desktop_render_wait(); fleet_poll(); }
}

const NetInterfaceInfo& net_eth_info() {
  g_eth_if.tx_ring = g_eth_tx_ring.stats();
  g_eth_if.rx_ring = g_eth_rx_ring.stats();
  return g_eth_if;
}

const NetInterfaceInfo& net_wifi_info() {
  g_wifi_if.tx_ring = g_wifi_tx_ring.stats();
  g_wifi_if.rx_ring = g_wifi_rx_ring.stats();
  return g_wifi_if;
}

const NetInterfaceInfo& net_primary_info() {
  if (g_eth_if.initialized && g_eth_if.ip_cfg.dhcp_bound) return net_eth_info();
  if (g_wifi_if.initialized && g_wifi_if.ip_cfg.dhcp_bound) return net_wifi_info();
  if (g_eth_if.initialized && g_eth_if.link_up) return net_eth_info();
  return net_wifi_info();
}

NetIfType net_primary_type() {
  if (g_eth_if.initialized && g_eth_if.ip_cfg.dhcp_bound) return NetIfType::Ethernet;
  if (g_wifi_if.initialized && g_wifi_if.ip_cfg.dhcp_bound) return NetIfType::Wifi;
  if (g_eth_if.initialized && g_eth_if.link_up) return NetIfType::Ethernet;
  return NetIfType::Wifi;
}

bool net_start_dhcp(NetIfType if_type) {
  if (if_type == NetIfType::Ethernet && g_eth_if.initialized) {
    g_eth_if.ip_cfg.dhcp_enabled = true;
    g_eth_if.ip_cfg.dhcp_bound = false;
    send_dhcp_discover(g_eth_if);
    return true;
  }
  if (if_type == NetIfType::Wifi && g_wifi_if.initialized) {
    g_wifi_if.ip_cfg.dhcp_enabled = true;
    g_wifi_if.ip_cfg.dhcp_bound = false;
    send_dhcp_discover(g_wifi_if);
    return true;
  }
  return false;
}

bool net_is_dhcp_bound(NetIfType if_type) {
  if (if_type == NetIfType::Ethernet) return g_eth_if.ip_cfg.dhcp_bound;
  if (if_type == NetIfType::Wifi) return g_wifi_if.ip_cfg.dhcp_bound;
  return g_eth_if.ip_cfg.dhcp_bound || g_wifi_if.ip_cfg.dhcp_bound;
}

bool net_set_static_ip(NetIfType if_type, const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4], const uint8_t dns[4]) {
  auto* iface = (if_type == NetIfType::Ethernet) ? &g_eth_if : &g_wifi_if;
  if (!iface->initialized) return false;

  kmemcpy(iface->ip_cfg.ip, ip, 4);
  kmemcpy(iface->ip_cfg.netmask, mask, 4);
  kmemcpy(iface->ip_cfg.gateway, gw, 4);
  kmemcpy(iface->ip_cfg.dns, dns, 4);
  iface->ip_cfg.dhcp_bound = true;
  iface->ip_cfg.dhcp_enabled = false;
  return true;
}

bool net_transmit_if(NetIfType if_type, const void* frame, size_t len) {
  if (!frame || len < 14 || len > NET_MAX_FRAME_SIZE) return false;

  if (if_type == NetIfType::Ethernet) {
    return g_eth_tx_ring.push(frame, len, 0);
  } else if (if_type == NetIfType::Wifi) {
    return g_wifi_tx_ring.push(frame, len, 0);
  }
  return false;
}

bool net_transmit(const void* frame, size_t len) {
  return net_transmit_if(net_primary_type(), frame, len);
}

bool net_get_mac(uint8_t mac[6]) {
  const auto& inf = net_primary_info();
  kmemcpy(mac, inf.mac, 6);
  return true;
}

bool net_set_mac(const uint8_t mac[6]) {
  if (!mac) return false;
  if (dhd_set_mac(mac)) {
    kmemcpy(g_wifi_if.mac, mac, 6);
    return true;
  }
  return false;
}

namespace {

/* Kick off a firmware scan; the dongle reports E_SCAN_COMPLETE when done. */
bool wifi_scan_start() {
  dhd_up();   /* WLC_UP is idempotent: never scan a downed radio */
  g_wifi_scan_complete = false;
  if (!dhd_scan()) {
    kputs("[wifi] scan request rejected by dongle\n");
    return false;
  }
  return true;
}

/* Read the firmware's scan results into entries (one ioctl, milliseconds). */
bool wifi_scan_collect(NetScanEntry* entries, size_t max_entries, size_t& out_count) {
  out_count = 0;
  static uint8_t scan_buf[8000] __attribute__((aligned(8)));
  if (!dhd_get_scan_results(scan_buf, sizeof(scan_buf))) {
    kputs("[wifi] scan results unreadable\n");
    return false;
  }
  const auto* results = reinterpret_cast<const WlcScanResults*>(scan_buf);
  const uint32_t count = results->count;
  /* Once, not on every 10 s background scan - that filled the Wi-Fi log
   * with nothing but this line. */
  static bool s_truncation_logged = false;
  if (count > max_entries && !s_truncation_logged) {
    s_truncation_logged = true;
    kprintf("[wifi] firmware reported %u networks; keeping the first %u\n",
            static_cast<uint64_t>(count), static_cast<uint64_t>(max_entries));
  }
  const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&results->bss_info[0]);
  size_t parsed = 0;
  for (uint32_t i = 0; i < count && parsed < max_entries; ++i) {
    const auto* bss = reinterpret_cast<const WlcBssInfo*>(ptr);
    if (bss->length == 0 || (ptr + bss->length > scan_buf + sizeof(scan_buf))) break;
    auto& e = entries[parsed++];
    kmemcpy(e.bssid, bss->bssid, 6);
    const size_t slen = (bss->ssid_len < 32) ? bss->ssid_len : 32;
    kmemcpy(e.ssid, bss->ssid, slen);
    e.ssid[slen] = '\0';
    e.channel = bss->ctl_ch ? bss->ctl_ch : (bss->chanspec & 0xFFu);
    e.rssi = bss->rssi;
    e.capability = bss->capability;
    ptr += bss->length;
  }
  out_count = parsed;
  return true;
}

}  // namespace

/* Blocking scan for when a person asked for one (console, desktop button).
 * The background refresh in net_poll() never calls this. */
bool net_scan(NetScanEntry* entries, size_t max_entries, size_t& out_count) {
  out_count = 0;
  if (!g_wifi_if.initialized) return false;

  const NetLinkState prev_state = g_wifi_if.link_state;
  g_wifi_if.link_state = NetLinkState::Scanning;

  /* A background scan already in flight is the same scan - wait on it
   * rather than asking the firmware for a second one while it is busy. */
  if (!g_auto_scan_pending && !wifi_scan_start()) {
    g_wifi_if.link_state = prev_state;
    return false;
  }
  const uint64_t scan_start = metal_now_ns();
  while (!g_wifi_scan_complete && metal_now_ns() - scan_start < 5000000000ull) {
    net_poll();
    metal_delay_us(20000);
  }
  g_auto_scan_pending = false;
  if (!g_wifi_scan_complete) net_log_wifi("scan complete event not received within 5s; reading what there is");

  const bool ok = wifi_scan_collect(entries, max_entries, out_count);
  if (ok) {
    for (size_t i = 0; i < out_count && i < kAutoScanMax; ++i) g_wifi_auto_scan_cache[i] = entries[i];
    g_wifi_auto_scan_count = out_count < kAutoScanMax ? out_count : kAutoScanMax;
    ++g_wifi_scan_generation;
  }
  g_wifi_if.link_state = prev_state;
  return ok;
}

bool net_associate(const char* ssid, const char* psk) {
  if (!ssid || !g_wifi_if.initialized) return false;

  g_wifi_if.link_state = NetLinkState::Associating;
  {
    uint32_t infra = 1;
    dhd_ioctl(WLC_SET_INFRA, &infra, sizeof(infra), true);
  }
  if (!dhd_set_passphrase(psk)) kputs("[wifi] security setup was not fully accepted - join may fail\n");

  if (!dhd_set_ssid(ssid)) {
    g_wifi_if.link_state = NetLinkState::Up;
    return false;
  }

  /* Wait for the dongle to report association. Five seconds: 50 attempts
   * at 10 ms was half a second, which is shorter than a WPA2 handshake
   * against a real AP. */
  bool associated = false;
  for (unsigned attempt = 0; attempt < 500; ++attempt) {
    net_poll();
    if (g_wifi_if.link_state == NetLinkState::Associated) { associated = true; break; }
    metal_delay_us(10000);
  }

  /* Confirm against the dongle itself rather than taking our own state
   * variable's word for it: a non-zero BSSID means we are actually
   * associated with an AP. */
  uint8_t bssid[6] = {};
  if (!associated && dhd_get_bssid(bssid)) {
    for (int i = 0; i < 6; ++i) if (bssid[i]) { associated = true; break; }
  }

  if (!associated) {
    /* This used to force link_state to Associated and return true no matter
     * what happened, so a failed association reported success and closed
     * the dialog with no connectivity behind it. */
    kputs("[wifi] association did not complete\n");
    g_wifi_if.link_state = NetLinkState::Up;
    g_wifi_if.ssid[0] = '\0';
    return false;
  }

  const size_t slen = kstrlen(ssid);
  const size_t copy_len = (slen < 32) ? slen : 32;
  kmemcpy(g_wifi_if.ssid, ssid, copy_len);
  g_wifi_if.ssid[copy_len] = '\0';

  dhd_get_bssid(g_wifi_if.bssid);
  dhd_get_channel(g_wifi_if.channel);
  dhd_get_rssi(g_wifi_if.rssi);

  g_wifi_if.link_state = NetLinkState::Associated;
  net_start_dhcp(NetIfType::Wifi);
  return true;
}

bool net_disassociate() {
  if (!g_wifi_if.initialized) return false;
  dhd_disassoc();
  g_wifi_if.link_state = NetLinkState::Up;
  g_wifi_if.ssid[0] = '\0';
  kmemset(g_wifi_if.bssid, 0, 6);
  g_wifi_if.rssi = 0;
  g_wifi_if.ip_cfg.dhcp_bound = false;
  return true;
}

bool net_ping(const uint8_t target_ip[4], uint32_t timeout_ms, uint32_t& out_rtt_us) {
  out_rtt_us = 0;
  const auto& iface = net_primary_info();
  if (!iface.ip_cfg.dhcp_bound) return false;

  g_ping.active = true;
  kmemcpy(g_ping.target_ip, target_ip, 4);
  g_ping.seq = static_cast<uint16_t>(metal_ticks() & 0xFFFF);
  g_ping.replied = false;
  g_ping.send_ts_ns = metal_now_ns();

  /* Construct ICMP Echo Request Packet (64 bytes total) */
  uint8_t pkt[74] = {};
  /* Broadcast MAC or known gateway MAC */
  kmemset(pkt + 0, 0xFF, 6);
  kmemcpy(pkt + 6, iface.mac, 6);
  pkt[12] = 0x08; pkt[13] = 0x00; /* IPv4 */

  uint8_t* ip = pkt + 14;
  ip[0] = 0x45; ip[8] = 64; ip[9] = 1; /* ICMP */
  ip[2] = 0x00; ip[3] = 60;            /* Total length 60 */
  kmemcpy(ip + 12, iface.ip_cfg.ip, 4);
  kmemcpy(ip + 16, target_ip, 4);
  uint16_t ip_chk = net_chksum(ip, 20);
  ip[10] = static_cast<uint8_t>(ip_chk & 0xFF);
  ip[11] = static_cast<uint8_t>(ip_chk >> 8);

  uint8_t* icmp = pkt + 34;
  icmp[0] = 8; /* Echo Request */
  icmp[1] = 0;
  icmp[4] = 0x12; icmp[5] = 0x34; /* ID */
  icmp[6] = static_cast<uint8_t>(g_ping.seq >> 8);
  icmp[7] = static_cast<uint8_t>(g_ping.seq & 0xFF);
  for (int i = 8; i < 40; ++i) icmp[i] = static_cast<uint8_t>('A' + (i % 26));

  uint16_t icmp_chk = net_chksum(icmp, 40);
  icmp[2] = static_cast<uint8_t>(icmp_chk & 0xFF);
  icmp[3] = static_cast<uint8_t>(icmp_chk >> 8);

  net_transmit_if(iface.type, pkt, 74);

  /* Poll for reply */
  uint64_t deadline = metal_now_ns() + static_cast<uint64_t>(timeout_ms) * 1000000ull;
  while (metal_now_ns() < deadline) {
    net_poll();
    if (g_ping.replied) {
      out_rtt_us = static_cast<uint32_t>((g_ping.recv_ts_ns - g_ping.send_ts_ns) / 1000ull);
      g_ping.active = false;
      return true;
    }
    metal_delay_us(100);
  }

  g_ping.active = false;
  return false;
}

bool net_send_test_frame() {
  const auto& iface = net_primary_info();
  uint8_t arp_frame[42] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    iface.mac[0], iface.mac[1], iface.mac[2], iface.mac[3], iface.mac[4], iface.mac[5],
    0x08, 0x06,
    0x00, 0x01,
    0x08, 0x00,
    0x06, 0x04,
    0x00, 0x01,
    iface.mac[0], iface.mac[1], iface.mac[2], iface.mac[3], iface.mac[4], iface.mac[5],
    iface.ip_cfg.ip[0], iface.ip_cfg.ip[1], iface.ip_cfg.ip[2], iface.ip_cfg.ip[3],
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    iface.ip_cfg.gateway[0],
    iface.ip_cfg.gateway[1],
    iface.ip_cfg.gateway[2],
    iface.ip_cfg.gateway[3]
  };

  return net_transmit(arp_frame, sizeof(arp_frame));
}

bool wifi_load_saved_config(WifiSavedConfig& cfg) {
  cfg = {};
  uint8_t buf[256] = {};
  size_t read_len = 0;
  if (!fat32_read_file("/sd/wifi.cfg", buf, sizeof(buf) - 1, read_len) &&
      !fat32_read_file("wifi.cfg", buf, sizeof(buf) - 1, read_len)) {
    if (g_saved_config.valid) {
      cfg = g_saved_config;
      return true;
    }
    return false;
  }

  buf[read_len] = '\0';
  const char* str = reinterpret_cast<const char*>(buf);
  const char* p = str;
  while (*p) {
    while (*p == ' ' || *p == '\r' || *p == '\n') ++p;
    if (!*p) break;
    if (p[0] == 's' && p[1] == 's' && p[2] == 'i' && p[3] == 'd' && p[4] == '=') {
      p += 5;
      size_t idx = 0;
      while (*p && *p != '\r' && *p != '\n' && idx + 1 < sizeof(cfg.ssid)) {
        cfg.ssid[idx++] = *p++;
      }
      cfg.ssid[idx] = '\0';
    } else if (p[0] == 'p' && p[1] == 's' && p[2] == 'k' && p[3] == '=') {
      p += 4;
      size_t idx = 0;
      while (*p && *p != '\r' && *p != '\n' && idx + 1 < sizeof(cfg.psk)) {
        cfg.psk[idx++] = *p++;
      }
      cfg.psk[idx] = '\0';
    } else if (p[0] == 'c' && p[1] == 'o' && p[2] == 'u' && p[3] == 'n' && p[4] == 't' &&
               p[5] == 'r' && p[6] == 'y' && p[7] == '=') {
      /* Optional regulatory country. Read only if a value is genuinely
       * present - a blank "country=" leaves cfg.country empty so bring-up
       * omits the setting instead of hardcoding one. */
      p += 8;
      size_t idx = 0;
      while (*p && *p != '\r' && *p != '\n' && idx + 1 < sizeof(cfg.country)) {
        cfg.country[idx++] = *p++;
      }
      cfg.country[idx] = '\0';
      while (*p && *p != '\r' && *p != '\n') ++p;
    } else if (p[0] == 'a' && p[1] == 'u' && p[2] == 't' && p[3] == 'o' && p[4] == '_' &&
               p[5] == 'c' && p[6] == 'o' && p[7] == 'n' && p[8] == 'n' && p[9] == 'e' && p[10] == 'c' && p[11] == 't' && p[12] == '=') {
      p += 13;
      cfg.auto_connect = (*p == '1' || *p == 'y' || *p == 'Y' || *p == 't' || *p == 'T');
      while (*p && *p != '\r' && *p != '\n') ++p;
    } else {
      while (*p && *p != '\r' && *p != '\n') ++p;
    }
  }

  if (cfg.ssid[0]) {
    cfg.valid = true;
    g_saved_config = cfg;
    return true;
  }
  /* A country-only file (no SSID) still carries the regulatory setting, so
   * keep the parsed value; it just does not enable auto-connect. */
  if (cfg.country[0]) g_saved_config.country[0] = cfg.country[0];
  return false;
}

bool wifi_save_config(const char* ssid, const char* psk, bool auto_connect) {
  if (!ssid || !ssid[0]) return false;

  g_saved_config.valid = true;
  size_t i = 0;
  while (ssid[i] && i + 1 < sizeof(g_saved_config.ssid)) {
    g_saved_config.ssid[i] = ssid[i];
    ++i;
  }
  g_saved_config.ssid[i] = '\0';

  i = 0;
  if (psk) {
    while (psk[i] && i + 1 < sizeof(g_saved_config.psk)) {
      g_saved_config.psk[i] = psk[i];
      ++i;
    }
  }
  g_saved_config.psk[i] = '\0';
  g_saved_config.auto_connect = auto_connect;

  char out_buf[256];
  size_t p = 0;
  auto append = [&](const char* s) {
    while (*s && p + 1 < sizeof(out_buf)) out_buf[p++] = *s++;
  };

  append("ssid=");
  append(ssid);
  append("\npsk=");
  append(psk ? psk : "");
  append("\nauto_connect=");
  append(auto_connect ? "1\n" : "0\n");
  out_buf[p] = '\0';

  return fat32_write_file("/sd/wifi.cfg", out_buf, p);
}

bool wifi_clear_saved_config() {
  g_saved_config = {};
  fat32_delete_file("/sd/wifi.cfg");
  return true;
}

const WifiSavedConfig& wifi_get_saved_config() {
  return g_saved_config;
}

bool wifi_auto_connect_saved() {
  if (!g_saved_config.valid) {
    wifi_load_saved_config(g_saved_config);
  }
  if (g_saved_config.valid && g_saved_config.auto_connect && g_saved_config.ssid[0]) {
    return net_associate(g_saved_config.ssid, g_saved_config.psk);
  }
  return false;
}

void ip_to_str(const uint8_t ip[4], char* buf, size_t max_len) {
  if (!buf || max_len < 16) return;
  size_t p = 0;
  for (int i = 0; i < 4; ++i) {
    uint8_t v = ip[i];
    if (v >= 100) buf[p++] = static_cast<char>('0' + (v / 100));
    if (v >= 10)  buf[p++] = static_cast<char>('0' + ((v / 10) % 10));
    buf[p++] = static_cast<char>('0' + (v % 10));
    if (i < 3) buf[p++] = '.';
  }
  buf[p] = '\0';
}

void mac_to_str(const uint8_t mac[6], char* buf, size_t max_len) {
  static const char hexchars[] = "0123456789ABCDEF";
  if (!buf || max_len < 18) return;
  size_t idx = 0;
  for (int i = 0; i < 6; ++i) {
    buf[idx++] = hexchars[(mac[i] >> 4) & 0xF];
    buf[idx++] = hexchars[mac[i] & 0xF];
    if (i < 5) buf[idx++] = ':';
  }
  buf[idx] = '\0';
}

void net_log_eth(const char* msg) { g_eth_log.append(msg); }
void net_log_wifi(const char* msg) { g_wifi_log.append(msg); }
size_t net_eth_log(const char** out_lines, size_t max_lines) { return g_eth_log.copy_out(out_lines, max_lines); }
size_t net_wifi_log(const char** out_lines, size_t max_lines) { return g_wifi_log.copy_out(out_lines, max_lines); }

void net_report() {
  char ip_s[20], gw_s[20];
  kputs("  network subsystem:\n");

  /* eth0 report */
  ip_to_str(g_eth_if.ip_cfg.ip, ip_s, sizeof(ip_s));
  ip_to_str(g_eth_if.ip_cfg.gateway, gw_s, sizeof(gw_s));
  kprintf("    eth0 (LAN9514) : %s | IP: %s (GW: %s, DHCP: %s)\n",
          g_eth_if.link_up ? "LINK UP" : "LINK DOWN",
          g_eth_if.ip_cfg.dhcp_bound ? ip_s : "0.0.0.0 (unbound)",
          gw_s, g_eth_if.ip_cfg.dhcp_bound ? "BOUND" : "SEARCHING");
  kprintf("    eth0 MAC       : %02x:%02x:%02x:%02x:%02x:%02x (TX: %u, RX: %u)\n",
          static_cast<uint64_t>(g_eth_if.mac[0]), static_cast<uint64_t>(g_eth_if.mac[1]),
          static_cast<uint64_t>(g_eth_if.mac[2]), static_cast<uint64_t>(g_eth_if.mac[3]),
          static_cast<uint64_t>(g_eth_if.mac[4]), static_cast<uint64_t>(g_eth_if.mac[5]),
          static_cast<uint64_t>(g_eth_if.stats.tx_packets), static_cast<uint64_t>(g_eth_if.stats.rx_packets));

  /* wlan0 report */
  ip_to_str(g_wifi_if.ip_cfg.ip, ip_s, sizeof(ip_s));
  kprintf("    wlan0 (BCM43438): %s | SSID: %s | IP: %s\n",
          link_state_str(g_wifi_if.link_state),
          g_wifi_if.ssid[0] ? g_wifi_if.ssid : "(none)",
          g_wifi_if.ip_cfg.dhcp_bound ? ip_s : "0.0.0.0");
  kprintf("    wlan0 MAC      : %02x:%02x:%02x:%02x:%02x:%02x (RSSI: %d dBm)\n",
          static_cast<uint64_t>(g_wifi_if.mac[0]), static_cast<uint64_t>(g_wifi_if.mac[1]),
          static_cast<uint64_t>(g_wifi_if.mac[2]), static_cast<uint64_t>(g_wifi_if.mac[3]),
          static_cast<uint64_t>(g_wifi_if.mac[4]), static_cast<uint64_t>(g_wifi_if.mac[5]),
          static_cast<int64_t>(g_wifi_if.rssi));

  TcpSocketInfo socks[TCP_MAX_SOCKETS];
  size_t n_socks = net_tcp_get_all_info(socks, TCP_MAX_SOCKETS);
  if (n_socks > 0) {
    kprintf("    active TCP sockets (%u):\n", static_cast<uint64_t>(n_socks));
    for (size_t i = 0; i < n_socks; ++i) {
      char rip_s[20];
      ip_to_str(socks[i].remote_ip, rip_s, sizeof(rip_s));
      kprintf("      [%d] :%u -> %s:%u (%s, rx: %u, tx: %u)\n",
              socks[i].id, socks[i].local_port, rip_s, socks[i].remote_port,
              tcp_state_str(socks[i].state), socks[i].rx_pending, socks[i].tx_pending);
    }
  }
}

void wifi_report() {
  bcm43438_report();
  dhd_report();
  net_report();
}

/* ----------------- TCP Public API ----------------- */

int net_tcp_connect(const uint8_t remote_ip[4], uint16_t port, uint32_t timeout_ms) {
  if (!remote_ip || port == 0) return -1;
  TcpControlBlock* t = tcb_allocate();
  if (!t) return -1;

  NetInterfaceInfo iface = net_primary_info();
  t->if_type = iface.type;
  kmemcpy(t->remote_ip, remote_ip, 4);
  t->remote_port = port;
  t->local_port = net_alloc_ephemeral_port();
  t->state = TcpState::SynSent;
  t->iss = net_rand32();
  t->snd_una = t->iss;
  t->snd_nxt = t->iss;
  t->retries = 0;
  t->rto_ms = 500;
  t->syn_acked = false;
  t->reset = false;
  /* Now, not whenever this TCB last sent: a stale stamp made the SYN_SENT
   * timer "retransmit" before the first SYN had even gone out. */
  t->last_tx_ns = metal_now_ns();

  tcp_out_segment(*t, TCP_FLAG_SYN, nullptr, 0, true);
  t->snd_nxt = t->iss + 1;

  uint64_t deadline = metal_now_ns() + static_cast<uint64_t>(timeout_ms) * 1000000ull;
  while (metal_now_ns() < deadline) {
    net_poll();
    if (t->state == TcpState::Established) {
      return t->id;
    }
    if (t->state == TcpState::Closed || t->reset) {
      tcb_free(t);
      return -1;
    }
    metal_delay_us(200);
  }

  tcb_free(t);
  return -1;
}

int net_tcp_listen(uint16_t port, size_t backlog) {
  if (port == 0) return -1;
  for (size_t i = 0; i < TCP_MAX_SOCKETS; ++i) {
    if (g_tcbs[i].in_use && g_tcbs[i].listening && g_tcbs[i].local_port == port) {
      return g_tcbs[i].id;
    }
  }

  TcpControlBlock* t = tcb_allocate();
  if (!t) return -1;

  t->listening = true;
  t->local_port = port;
  t->state = TcpState::Listen;
  t->backlog_count = 0;
  t->backlog_limit = (backlog > TCP_MAX_BACKLOG) ? TCP_MAX_BACKLOG : backlog;
  return t->id;
}

int net_tcp_accept(int listen_sock, uint32_t timeout_ms) {
  TcpControlBlock* listener = tcb_find_by_id(listen_sock);
  if (!listener || !listener->listening) return -1;

  /* timeout 0 = "is anyone waiting?": the backlog, nothing more. Pumping the
   * network here too made every caller's poll loop run net_poll twice per
   * listener - the desktop's screenshot server alone (two listeners) spent
   * ~300 ms a second in it. The caller's loop already runs net_poll. */
  uint64_t deadline = (timeout_ms > 0) ? (metal_now_ns() + static_cast<uint64_t>(timeout_ms) * 1000000ull) : 0;
  for (;;) {
    if (timeout_ms > 0) net_poll();
    if (listener->backlog_count > 0) {
      int child_id = listener->backlog[0];
      for (size_t i = 1; i < listener->backlog_count; ++i) {
        listener->backlog[i - 1] = listener->backlog[i];
      }
      listener->backlog_count--;
      return child_id;
    }
    if (timeout_ms == 0 || metal_now_ns() >= deadline) break;
    metal_delay_us(200);
  }
  return -1;
}

bool net_tcp_unlisten(uint16_t port) {
  for (size_t i = 0; i < TCP_MAX_SOCKETS; ++i) {
    if (g_tcbs[i].in_use && g_tcbs[i].listening && g_tcbs[i].local_port == port) {
      tcb_free(&g_tcbs[i]);
      return true;
    }
  }
  return false;
}

bool net_tcp_is_listening(uint16_t port) {
  for (size_t i = 0; i < TCP_MAX_SOCKETS; ++i) {
    if (g_tcbs[i].in_use && g_tcbs[i].listening && g_tcbs[i].local_port == port) {
      return true;
    }
  }
  return false;
}

int net_tcp_send(int sock, const void* data, size_t len, uint32_t timeout_ms) {
  TcpControlBlock* t = tcb_find_by_id(sock);
  if (!t || (t->state != TcpState::Established && t->state != TcpState::CloseWait)) return -1;
  if (!data || len == 0) return 0;

  const uint8_t* ptr = reinterpret_cast<const uint8_t*>(data);
  uint64_t deadline = metal_now_ns() + static_cast<uint64_t>(timeout_ms) * 1000000ull;
  size_t total_sent = 0;

  /* Give up after 8 s without progress, not just at the deadline: a
   * receiver that vanished otherwise held the caller for the whole timeout
   * (the screenshot server passes 180 s), and since this loop keeps calling
   * net_poll() the watchdog never noticed - the desktop froze before its
   * first frame waiting on a dead screenshot connection. */
  uint64_t last_progress = metal_now_ns();
  while (total_sent < len) {
    net_poll();
    if (t->state != TcpState::Established && t->state != TcpState::CloseWait) break;
    if (t->reset) break;
    size_t pushed = tcp_push(*t, ptr + total_sent, len - total_sent);
    total_sent += pushed;
    if (total_sent >= len) break;
    const uint64_t now = metal_now_ns();
    if (pushed) last_progress = now;
    if (now >= deadline || now - last_progress > 8000000000ull) break;
    metal_delay_us(200);
  }
  return static_cast<int>(total_sent);
}

int net_tcp_recv(int sock, void* buf, size_t max_len, uint32_t timeout_ms) {
  TcpControlBlock* t = tcb_find_by_id(sock);
  if (!t) return -1;
  if (!buf || max_len == 0) return 0;

  uint64_t deadline = (timeout_ms > 0) ? (metal_now_ns() + static_cast<uint64_t>(timeout_ms) * 1000000ull) : 0;
  for (;;) {
    net_poll();
    if (t->rx_buf.size() > 0) {
      size_t read_bytes = t->rx_buf.read(reinterpret_cast<uint8_t*>(buf), max_len);
      return static_cast<int>(read_bytes);
    }
    if (t->peer_fin || t->state == TcpState::Closed || t->reset) {
      return 0; /* EOF */
    }
    if (timeout_ms == 0 || metal_now_ns() >= deadline) break;
    metal_delay_us(200);
  }
  return 0;
}

bool net_tcp_send_all(int sock, const void* data, size_t len, uint32_t timeout_ms) {
  if (!data || len == 0) return true;
  int sent = net_tcp_send(sock, data, len, timeout_ms);
  return sent == static_cast<int>(len);
}

bool net_tcp_recv_all(int sock, void* buf, size_t len, uint32_t timeout_ms) {
  if (!buf || len == 0) return true;
  uint8_t* ptr = reinterpret_cast<uint8_t*>(buf);
  size_t total_read = 0;
  uint64_t deadline = metal_now_ns() + static_cast<uint64_t>(timeout_ms) * 1000000ull;

  while (total_read < len) {
    int r = net_tcp_recv(sock, ptr + total_read, len - total_read, 100);
    if (r > 0) {
      total_read += static_cast<size_t>(r);
    } else if (r == 0) {
      TcpControlBlock* t = tcb_find_by_id(sock);
      if (!t || t->peer_fin || t->state == TcpState::Closed) break;
    }
    if (metal_now_ns() >= deadline) break;
  }
  return total_read == len;
}

void net_tcp_close(int sock) {
  TcpControlBlock* t = tcb_find_by_id(sock);
  if (!t) return;

  if (t->listening) {
    tcb_free(t);
    return;
  }

  if (t->state == TcpState::Established) {
    tcp_out_segment(*t, TCP_FLAG_FIN | TCP_FLAG_ACK, nullptr, 0);
    t->snd_nxt++;
    t->fin_sent = true;
    t->state = TcpState::FinWait1;
  } else if (t->state == TcpState::CloseWait) {
    tcp_out_segment(*t, TCP_FLAG_FIN | TCP_FLAG_ACK, nullptr, 0);
    t->snd_nxt++;
    t->fin_sent = true;
    t->state = TcpState::LastAck;
  } else {
    tcb_free(t);
  }
}

void net_tcp_abort(int sock) {
  TcpControlBlock* t = tcb_find_by_id(sock);
  if (!t) return;
  if (t->state != TcpState::Closed && !t->listening) {
    tcp_out_segment(*t, TCP_FLAG_RST | TCP_FLAG_ACK, nullptr, 0);
  }
  tcb_free(t);
}

bool net_tcp_is_alive(int sock) {
  TcpControlBlock* t = tcb_find_by_id(sock);
  return t && t->in_use && t->state != TcpState::Closed;
}

bool net_tcp_is_established(int sock) {
  TcpControlBlock* t = tcb_find_by_id(sock);
  return t && t->in_use && t->state == TcpState::Established;
}

size_t net_tcp_pending(int sock) {
  TcpControlBlock* t = tcb_find_by_id(sock);
  return t ? t->rx_buf.size() : 0;
}

TcpState net_tcp_state(int sock) {
  TcpControlBlock* t = tcb_find_by_id(sock);
  return t ? t->state : TcpState::Closed;
}

bool net_tcp_peer(int sock, uint8_t out_ip[4], uint16_t& out_port) {
  TcpControlBlock* t = tcb_find_by_id(sock);
  if (!t) return false;
  kmemcpy(out_ip, t->remote_ip, 4);
  out_port = t->remote_port;
  return true;
}

size_t net_tcp_get_all_info(TcpSocketInfo* out_info, size_t max_info) {
  if (!out_info || max_info == 0) return 0;
  size_t count = 0;
  for (size_t i = 0; i < TCP_MAX_SOCKETS && count < max_info; ++i) {
    if (g_tcbs[i].in_use) {
      auto& info = out_info[count++];
      info.id = g_tcbs[i].id;
      info.state = g_tcbs[i].state;
      kmemcpy(info.remote_ip, g_tcbs[i].remote_ip, 4);
      info.remote_port = g_tcbs[i].remote_port;
      info.local_port = g_tcbs[i].local_port;
      info.rx_pending = g_tcbs[i].rx_buf.size();
      info.tx_pending = g_tcbs[i].tx_buf.size();
      info.is_listening = g_tcbs[i].listening;
      info.is_reset = g_tcbs[i].reset;
      info.is_peer_fin = g_tcbs[i].peer_fin;
    }
  }
  return count;
}

const char* tcp_state_str(TcpState st) {
  switch (st) {
    case TcpState::Closed: return "CLOSED";
    case TcpState::Listen: return "LISTEN";
    case TcpState::SynSent: return "SYN_SENT";
    case TcpState::SynReceived: return "SYN_RCVD";
    case TcpState::Established: return "ESTABLISHED";
    case TcpState::FinWait1: return "FIN_WAIT_1";
    case TcpState::FinWait2: return "FIN_WAIT_2";
    case TcpState::Closing: return "CLOSING";
    case TcpState::CloseWait: return "CLOSE_WAIT";
    case TcpState::LastAck: return "LAST_ACK";
    case TcpState::TimeWait: return "TIME_WAIT";
    default: return "UNKNOWN";
  }
}

/* ----------------- UDP socket API ----------------- */
namespace {
bool udp_port_reserved(uint16_t port) {
  return port == 68 || port == AIOS_CONSOLE_PORT || port == DNS_LOCAL_PORT || port == NTP_LOCAL_PORT;
}
uint16_t g_udp_next_ephemeral = 49152;
}  // namespace

int net_udp_open(uint16_t port) {
  if (port == 0) {
    for (int tries = 0; tries < 16384 && port == 0; ++tries) {
      const uint16_t p = g_udp_next_ephemeral;
      g_udp_next_ephemeral = static_cast<uint16_t>(p >= 65535 ? 49152 : p + 1);
      if (!udp_slot_for_port(p)) port = p;
    }
    if (port == 0) return -1;
  } else if (udp_port_reserved(port) || udp_slot_for_port(port)) {
    return -2;                                                 /* in use (by the kernel or another socket) */
  }
  for (int i = 0; i < UDP_MAX_SOCKETS; ++i) {
    if (g_udp[i].used) continue;
    g_udp[i].used = true;
    g_udp[i].port = port;
    g_udp[i].head = g_udp[i].count = 0;
    g_udp[i].dropped = 0;
    return i;
  }
  return -3;                                                   /* table full */
}

uint16_t net_udp_port(int s) {
  return (s >= 0 && s < UDP_MAX_SOCKETS && g_udp[s].used) ? g_udp[s].port : 0;
}

void net_udp_close(int s) {
  if (s >= 0 && s < UDP_MAX_SOCKETS) g_udp[s].used = false;
}

int net_udp_send(int s, const uint8_t ip[4], uint16_t port, const void* data, size_t len) {
  if (s < 0 || s >= UDP_MAX_SOCKETS || !g_udp[s].used) return -1;
  if (len > UDP_MAX_PAYLOAD) return -2;
  NetInterfaceInfo& iface = g_eth_if;
  if (!iface.initialized || !iface.link_up) return -3;
  const uint16_t src = g_udp[s].port;
  const bool self = (ip[0] == 127) || (ip[0] == iface.ip_cfg.ip[0] && ip[1] == iface.ip_cfg.ip[1] &&
                                         ip[2] == iface.ip_cfg.ip[2] && ip[3] == iface.ip_cfg.ip[3]);
  if (self) {                                                  /* to ourselves: straight into the queue */
    UdpSocketSlot* u = udp_slot_for_port(port);
    const uint8_t* from = ip[0] == 127 ? ip : iface.ip_cfg.ip;
    if (u) udp_enqueue(*u, from, src, static_cast<const uint8_t*>(data), len);
    return static_cast<int>(len);                              /* nobody bound: gone, as on a wire */
  }
  uint8_t mac[6];
  const bool bcast = ip[0] == 255 && ip[1] == 255 && ip[2] == 255 && ip[3] == 255;
  if (bcast) { for (auto& b : mac) b = 0xFF; }
  else if (!arp_resolve(iface, ip, mac, 1000)) return -4;
  send_udp(iface, mac, ip, src, port, data, len);
  drain_tx_now(iface);
  return static_cast<int>(len);
}

int net_udp_recv(int s, void* buf, size_t max_len, uint8_t from_ip[4], uint16_t& from_port, uint32_t timeout_ms) {
  if (s < 0 || s >= UDP_MAX_SOCKETS || !g_udp[s].used) return -1;
  UdpSocketSlot& u = g_udp[s];
  const uint64_t deadline = metal_now_ns() + static_cast<uint64_t>(timeout_ms) * 1000000ull;
  for (;;) {
    if (u.count > 0) {
      UdpDatagram& d = u.q[u.head];
      const size_t n = d.len < max_len ? d.len : max_len;
      kmemcpy(buf, d.data, n);
      kmemcpy(from_ip, d.ip, 4);
      from_port = d.port;
      u.head = (u.head + 1) % UDP_QUEUE;
      --u.count;
      return static_cast<int>(n);
    }
    if (metal_now_ns() >= deadline) return 0;                  /* nothing in time */
    net_poll();
    metal_delay_us(100);
  }
}
