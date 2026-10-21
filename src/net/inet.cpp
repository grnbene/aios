#include "aios/net/inet.hpp"
#include <cstring>
#include <algorithm>

namespace aios::net {

// ---- small helpers -------------------------------------------------------
namespace {
inline void w16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
inline void w32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
inline uint16_t r16(const uint8_t* p) { return ((uint16_t)p[0] << 8) | p[1]; }
inline uint32_t r32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

uint16_t ones_complement(const uint8_t* data, size_t len, uint32_t seed) {
  uint32_t sum = seed;
  size_t i = 0;
  for (; i + 1 < len; i += 2) sum += r16(data + i);
  if (i < len) sum += (uint32_t)data[i] << 8;
  while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
  return (uint16_t)~sum;
}
uint16_t ip_checksum(const uint8_t* hdr, size_t len) { return ones_complement(hdr, len, 0); }

constexpr uint16_t ET_IPV4 = 0x0800, ET_ARP = 0x0806;
constexpr uint8_t IPPROTO_ICMP = 1, IPPROTO_TCP = 6, IPPROTO_UDP = 17;
const Mac BCAST = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// The passive-open states are here too: this stack could only ever place a
// call, never answer one, so a machine running it could reach a peer and no
// peer could reach it.
enum TcpState { CLOSED, LISTEN, SYN_SENT, SYN_RECEIVED, ESTABLISHED,
                FIN_WAIT_1, FIN_WAIT_2, CLOSING, CLOSE_WAIT, LAST_ACK, TIME_WAIT };
constexpr uint8_t TH_FIN = 1, TH_SYN = 2, TH_RST = 4, TH_PSH = 8, TH_ACK = 16;

struct Tcb {
  uint32_t id = 0;
  TcpState state = CLOSED;
  Ipv4 remote_ip; uint16_t remote_port = 0, local_port = 0;
  uint32_t iss = 0, snd_una = 0, snd_nxt = 0, snd_wnd = 0;
  uint32_t irs = 0, rcv_nxt = 0;
  uint16_t mss = 536;
  std::deque<uint8_t> rx;          // received, in-order, not yet read by the app
  std::vector<uint8_t> tx;         // sent-but-unacked bytes, starting at snd_una
  bool fin_sent = false, peer_fin = false, reset = false;
  uint64_t last_tx_ns = 0; uint32_t rto_ms = 500; int retries = 0;
  bool syn_acked = false;
  // A listener holds no connection of its own: it holds the port and the
  // connections that have completed their handshake on it.
  bool listening = false;
  std::deque<uint32_t> backlog;    // ids of connections ready to be accepted
  size_t backlog_limit = 8;
};
}  // namespace

std::string Ipv4::str() const {
  auto d = [](uint8_t v) { char b[4]; int n = 0; if (v >= 100) b[n++] = '0' + v / 100; if (v >= 10) b[n++] = '0' + (v / 10) % 10; b[n++] = '0' + v % 10; return std::string(b, n); };
  return d(b[0]) + "." + d(b[1]) + "." + d(b[2]) + "." + d(b[3]);
}
bool parse_ipv4(const std::string& s, Ipv4& out) {
  int part = 0, val = 0, digits = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    char c = i < s.size() ? s[i] : '.';
    if (c == '.') { if (!digits || part > 3 || val > 255) return false; out.b[part++] = (uint8_t)val; val = 0; digits = 0; }
    else if (c >= '0' && c <= '9') { val = val * 10 + (c - '0'); if (++digits > 3) return false; }
    else return false;
  }
  return part == 4;
}

// ---- the stack -----------------------------------------------------------
struct NetStack::Impl {
  NetStack& owner;
  FrameDevice& dev;
  uint64_t (*now_ns)();
  uint32_t (*rand32)();
  Mac mac{};
  Ipv4 ip, mask, gw, dns;
  Mac gw_mac{}; bool gw_mac_known = false;
  struct ArpEnt { Ipv4 ip; Mac mac; };
  std::vector<ArpEnt> arp;
  std::vector<std::shared_ptr<Tcb>> tcbs;
  uint32_t next_id = 1;
  uint16_t next_port = 49152;
  // UDP waiter: DNS/DHCP replies land here by local port.
  struct UdpIn { uint16_t port; Ipv4 src; std::vector<uint8_t> data; };
  std::deque<UdpIn> udp_in;
  // ICMP echo reply flag
  bool icmp_reply = false; uint16_t icmp_id = 0, icmp_seq = 0;
  uint16_t ip_id = 1;

  Impl(NetStack& o, FrameDevice& d, uint64_t (*n)(), uint32_t (*r)()) : owner(o), dev(d), now_ns(n), rand32(r) {}

  uint64_t now_ms() { return now_ns() / 1000000ull; }
  uint16_t eph_port() { uint16_t p = next_port++; if (next_port == 0 || next_port < 49152) next_port = 49152; return p; }
  bool on_subnet(const Ipv4& t) { for (int i = 0; i < 4; ++i) if ((ip.b[i] & mask.b[i]) != (t.b[i] & mask.b[i])) return false; return true; }
  Ipv4 next_hop(const Ipv4& dst) { return on_subnet(dst) ? dst : gw; }

  const Mac* arp_lookup(const Ipv4& t) { for (auto& e : arp) if (e.ip == t) return &e.mac; return nullptr; }
  void arp_learn(const Ipv4& t, const Mac& m) { for (auto& e : arp) if (e.ip == t) { e.mac = m; return; } arp.push_back({t, m}); }

  // ---- link ----
  void eth_send(const Mac& dst, uint16_t ethertype, const uint8_t* payload, size_t len) {
    uint8_t f[1600]; if (14 + len > sizeof f) return;
    memcpy(f, dst.data(), 6); memcpy(f + 6, mac.data(), 6); w16(f + 12, ethertype);
    memcpy(f + 14, payload, len);
    size_t total = 14 + len; if (total < 60) { memset(f + total, 0, 60 - total); total = 60; }
    if (dev.send(f, total)) owner.stats_.tx_frames++;
  }

  // ---- ARP ----
  void arp_send(uint16_t op, const Mac& target_mac, const Ipv4& target_ip) {
    uint8_t a[28];
    w16(a, 1); w16(a + 2, ET_IPV4); a[4] = 6; a[5] = 4; w16(a + 6, op);
    memcpy(a + 8, mac.data(), 6); memcpy(a + 14, ip.b, 4);
    memcpy(a + 18, target_mac.data(), 6); memcpy(a + 24, target_ip.b, 4);
    eth_send(op == 1 ? BCAST : target_mac, ET_ARP, a, 28);
    owner.stats_.arp_tx++;
  }
  void arp_input(const uint8_t* p, size_t len) {
    if (len < 28) return;
    owner.stats_.arp_rx++;
    uint16_t op = r16(p + 6);
    Ipv4 sip; memcpy(sip.b, p + 14, 4); Mac smac; memcpy(smac.data(), p + 8, 6);
    if (!sip.is_zero()) arp_learn(sip, smac);
    if (op == 1) { Ipv4 tip; memcpy(tip.b, p + 24, 4); if (tip == ip) arp_send(2, smac, sip); }   // reply for us
  }
  bool arp_resolve(const Ipv4& t, Mac& out, int timeout_ms) {
    if (const Mac* m = arp_lookup(t)) { out = *m; return true; }
    uint64_t end = now_ms() + timeout_ms; int sent = 0; uint64_t last = 0;
    while (now_ms() < end) {
      if (now_ms() - last > 500) { arp_send(1, BCAST, t); last = now_ms(); sent++; }
      pump_once();
      if (const Mac* m = arp_lookup(t)) { out = *m; return true; }
    }
    (void)sent; return false;
  }

  // ---- IPv4 ----
  void ip_send(const Ipv4& dst, uint8_t proto, const uint8_t* payload, size_t len) {
    Mac dm; if (const Mac* m = arp_lookup(next_hop(dst))) dm = *m; else { if (!arp_resolve(next_hop(dst), dm, 1000)) { owner.stats_.dropped++; return; } }
    uint8_t pkt[1600]; size_t hl = 20; if (hl + len > sizeof pkt) return;
    pkt[0] = 0x45; pkt[1] = 0; w16(pkt + 2, (uint16_t)(hl + len)); w16(pkt + 4, ip_id++); w16(pkt + 6, 0x4000); // DF
    pkt[8] = 64; pkt[9] = proto; w16(pkt + 10, 0); memcpy(pkt + 12, ip.b, 4); memcpy(pkt + 16, dst.b, 4);
    w16(pkt + 10, ip_checksum(pkt, hl));
    memcpy(pkt + hl, payload, len);
    eth_send(dm, ET_IPV4, pkt, hl + len);
    owner.stats_.ip_tx++;
  }
  uint32_t pseudo_seed(const Ipv4& src, const Ipv4& dst, uint8_t proto, uint16_t tlen) {
    uint32_t s = 0; s += r16(src.b) + r16(src.b + 2); s += r16(dst.b) + r16(dst.b + 2); s += proto; s += tlen; return s;
  }
  void ip_input(const uint8_t* p, size_t len) {
    if (len < 20 || (p[0] >> 4) != 4) return;
    size_t hl = (p[0] & 0x0F) * 4; if (hl < 20 || hl > len) return;
    uint16_t total = r16(p + 2); if (total > len || total < hl) return;
    uint8_t proto = p[9];
    Ipv4 src; memcpy(src.b, p + 12, 4); Ipv4 dst; memcpy(dst.b, p + 16, 4);
    bool bcast = (dst.b[0] == 255 && dst.b[1] == 255 && dst.b[2] == 255 && dst.b[3] == 255);
    if (!(dst == ip) && !bcast && !ip.is_zero()) return;   // not for us (accept broadcast, and anything while unconfigured for DHCP)
    owner.stats_.ip_rx++;
    const uint8_t* payload = p + hl; size_t plen = total - hl;
    if (proto == IPPROTO_ICMP) icmp_input(src, payload, plen);
    else if (proto == IPPROTO_UDP) udp_input(src, payload, plen);
    else if (proto == IPPROTO_TCP) tcp_input(src, payload, plen);
  }

  // ---- ICMP ----
  void icmp_input(const Ipv4& src, const uint8_t* p, size_t len) {
    if (len < 8) return; owner.stats_.icmp_rx++;
    if (p[0] == 0) { icmp_reply = true; icmp_id = r16(p + 4); icmp_seq = r16(p + 6); }     // echo reply
    else if (p[0] == 8) {                                                                  // echo request → reply
      std::vector<uint8_t> r(p, p + len); r[0] = 0; w16(&r[2], 0); w16(&r[2], ip_checksum(r.data(), r.size()));
      ip_send(src, IPPROTO_ICMP, r.data(), r.size());
    }
  }

  // ---- UDP ----
  void udp_send(const Ipv4& dst, uint16_t sport, uint16_t dport, const uint8_t* data, size_t len) {
    std::vector<uint8_t> seg(8 + len);
    w16(&seg[0], sport); w16(&seg[2], dport); w16(&seg[4], (uint16_t)(8 + len)); w16(&seg[6], 0);
    memcpy(&seg[8], data, len);
    uint32_t seed = pseudo_seed(ip, dst, IPPROTO_UDP, (uint16_t)(8 + len));
    uint16_t ck = ones_complement(seg.data(), seg.size(), seed); if (ck == 0) ck = 0xFFFF; w16(&seg[6], ck);
    ip_send(dst, IPPROTO_UDP, seg.data(), seg.size());
  }
  void udp_input(const Ipv4& src, const uint8_t* p, size_t len) {
    if (len < 8) return; owner.stats_.udp_rx++;
    uint16_t dport = r16(p + 2); size_t dlen = r16(p + 4); if (dlen < 8 || dlen > len) dlen = len;
    udp_in.push_back({dport, src, std::vector<uint8_t>(p + 8, p + dlen)});
    if (udp_in.size() > 16) udp_in.pop_front();
  }
  bool udp_wait(uint16_t port, Ipv4& src, std::vector<uint8_t>& out, int timeout_ms) {
    uint64_t end = now_ms() + timeout_ms;
    for (;;) {
      for (size_t i = 0; i < udp_in.size(); ++i) if (udp_in[i].port == port) { src = udp_in[i].src; out = udp_in[i].data; udp_in.erase(udp_in.begin() + i); return true; }
      if (now_ms() >= end) return false;
      pump_once();
    }
  }

  // ---- DHCP (client) ----
  bool dhcp() {
    uint16_t sport = 68, dport = 67; uint32_t xid = rand32();
    auto build = [&](uint8_t type, Ipv4 reqip, Ipv4 server) {
      std::vector<uint8_t> m(240, 0);
      m[0] = 1; m[1] = 1; m[2] = 6; w32(&m[4], xid); w16(&m[10], 0x8000);   // broadcast
      memcpy(&m[28], mac.data(), 6);
      w32(&m[236], 0x63825363);   // magic cookie at 236
      auto opt = [&](uint8_t code, std::initializer_list<uint8_t> v) { m.push_back(code); m.push_back((uint8_t)v.size()); for (uint8_t x : v) m.push_back(x); };
      opt(53, {type});
      if (!reqip.is_zero()) opt(50, {reqip.b[0], reqip.b[1], reqip.b[2], reqip.b[3]});
      if (!server.is_zero()) opt(54, {server.b[0], server.b[1], server.b[2], server.b[3]});
      opt(55, {1, 3, 6});   // request subnet, router, dns
      m.push_back(255);
      return m;
    };
    // DISCOVER
    auto disc = build(1, {}, {});
    Ipv4 bip = {255, 255, 255, 255};
    // send to broadcast MAC directly (no ARP; source ip 0.0.0.0)
    Ipv4 save = ip; ip = {0, 0, 0, 0};
    auto raw_udp_bcast = [&](std::vector<uint8_t>& payload) {
      std::vector<uint8_t> seg(8 + payload.size());
      w16(&seg[0], sport); w16(&seg[2], dport); w16(&seg[4], (uint16_t)seg.size()); w16(&seg[6], 0);
      memcpy(&seg[8], payload.data(), payload.size());
      uint8_t pkt[1600]; size_t hl = 20; pkt[0] = 0x45; pkt[1] = 0; w16(pkt + 2, (uint16_t)(hl + seg.size())); w16(pkt + 4, ip_id++); w16(pkt + 6, 0);
      pkt[8] = 64; pkt[9] = IPPROTO_UDP; w16(pkt + 10, 0); memcpy(pkt + 12, ip.b, 4); memcpy(pkt + 16, bip.b, 4); w16(pkt + 10, ip_checksum(pkt, hl));
      memcpy(pkt + hl, seg.data(), seg.size()); eth_send(BCAST, ET_IPV4, pkt, hl + seg.size());
    };
    raw_udp_bcast(disc);
    Ipv4 offer_ip, server_id, gwo, dnso, masko; bool got = false;
    uint64_t end = now_ms() + 4000;
    while (now_ms() < end && !got) {
      Ipv4 src; std::vector<uint8_t> resp;
      if (udp_wait(68, src, resp, 500) && resp.size() >= 240 && r32(&resp[236]) == 0x63825363) {
        memcpy(offer_ip.b, &resp[16], 4);   // yiaddr
        // parse options
        size_t i = 240; while (i + 1 < resp.size()) { uint8_t c = resp[i], l = resp[i + 1]; if (c == 255) break; const uint8_t* v = &resp[i + 2];
          if (c == 54 && l == 4) memcpy(server_id.b, v, 4); else if (c == 1 && l == 4) memcpy(masko.b, v, 4);
          else if (c == 3 && l >= 4) memcpy(gwo.b, v, 4); else if (c == 6 && l >= 4) memcpy(dnso.b, v, 4);
          i += 2 + l; }
        got = true;
      }
    }
    if (!got) { ip = save; return false; }
    auto req = build(3, offer_ip, server_id); raw_udp_bcast(req);
    end = now_ms() + 4000; bool ack = false;
    while (now_ms() < end && !ack) {
      Ipv4 src; std::vector<uint8_t> resp;
      if (udp_wait(68, src, resp, 500) && resp.size() >= 240) {
        size_t i = 240; uint8_t type = 0; while (i + 1 < resp.size()) { uint8_t c = resp[i], l = resp[i + 1]; if (c == 255) break; if (c == 53 && l == 1) type = resp[i + 2]; i += 2 + l; }
        if (type == 5) ack = true;   // DHCPACK
      }
    }
    if (!ack) { ip = save; return false; }
    ip = offer_ip; mask = masko; gw = gwo; dns = dnso;
    return true;
  }

  // ---- DNS ----
  bool dns_query(const std::string& host, Ipv4& out, int timeout_ms) {
    if (dns.is_zero()) return false;
    uint16_t id = (uint16_t)rand32(), sport = eph_port();
    std::vector<uint8_t> q(12, 0); w16(&q[0], id); w16(&q[2], 0x0100); w16(&q[4], 1);
    size_t s = 0; for (size_t i = 0; i <= host.size(); ++i) { if (i == host.size() || host[i] == '.') { q.push_back((uint8_t)(i - s)); for (size_t j = s; j < i; ++j) q.push_back((uint8_t)host[j]); s = i + 1; } }
    q.push_back(0); q.push_back(0); q.push_back(1); q.push_back(0); q.push_back(1);   // QTYPE A, QCLASS IN
    udp_send(dns, sport, 53, q.data(), q.size());
    uint64_t end = now_ms() + timeout_ms;
    while (now_ms() < end) {
      Ipv4 src; std::vector<uint8_t> r;
      if (udp_wait(sport, src, r, 500) && r.size() >= 12 && r16(&r[0]) == id) {
        uint16_t ancount = r16(&r[6]); size_t i = 12;
        while (i < r.size() && r[i] != 0) { if ((r[i] & 0xC0) == 0xC0) { i += 2; goto qdone; } i += r[i] + 1; } i += 1;
      qdone: i += 4;   // skip QTYPE+QCLASS
        for (uint16_t a = 0; a < ancount && i + 12 <= r.size(); ++a) {
          if ((r[i] & 0xC0) == 0xC0) i += 2; else { while (i < r.size() && r[i] != 0) i += r[i] + 1; i += 1; }
          uint16_t type = r16(&r[i]); uint16_t rdlen = r16(&r[i + 8]); i += 10;
          if (type == 1 && rdlen == 4 && i + 4 <= r.size()) { memcpy(out.b, &r[i], 4); return true; }
          i += rdlen;
        }
      }
    }
    return false;
  }

  // ---- TCP ----
  std::shared_ptr<Tcb> tcb_of(uint32_t id) { for (auto& t : tcbs) if (t->id == id) return t; return nullptr; }
  void tcp_out(Tcb& t, uint8_t flags, const uint8_t* data, size_t len, bool with_mss = false) {
    size_t hl = with_mss ? 24 : 20; std::vector<uint8_t> seg(hl + len);
    w16(&seg[0], t.local_port); w16(&seg[2], t.remote_port); w32(&seg[4], t.snd_nxt);
    w32(&seg[8], (flags & TH_ACK) ? t.rcv_nxt : 0);
    seg[12] = (uint8_t)((hl / 4) << 4); seg[13] = flags;
    uint32_t rwnd = 65535; if (rwnd > 65535) rwnd = 65535; w16(&seg[14], (uint16_t)rwnd);
    w16(&seg[16], 0); w16(&seg[18], 0);
    if (with_mss) { seg[20] = 2; seg[21] = 4; w16(&seg[22], 1460); }
    if (len) memcpy(&seg[hl], data, len);
    uint32_t seed = pseudo_seed(ip, t.remote_ip, IPPROTO_TCP, (uint16_t)seg.size());
    w16(&seg[16], ones_complement(seg.data(), seg.size(), seed));
    ip_send(t.remote_ip, IPPROTO_TCP, seg.data(), seg.size());
    t.last_tx_ns = now_ns();
  }
  void tcp_input(const Ipv4& src, const uint8_t* p, size_t len) {
    if (len < 20) return; owner.stats_.tcp_rx++;
    uint16_t sport = r16(p), dport = r16(p + 2);
    uint32_t seq = r32(p + 4), ack = r32(p + 8);
    size_t hl = (p[12] >> 4) * 4; if (hl < 20 || hl > len) return;
    uint8_t flags = p[13]; uint16_t wnd = r16(p + 14);
    const uint8_t* data = p + hl; size_t dlen = len - hl;
    std::shared_ptr<Tcb> tp;
    for (auto& t : tcbs) if (!t->listening && t->local_port == dport && t->remote_port == sport && t->remote_ip == src) { tp = t; break; }
    if (!tp) {
      // Nothing has this conversation. A SYN to a port something is listening
      // on opens one; anything else to a port nobody holds gets a reset, which
      // is what tells the far end at once rather than letting it retry.
      std::shared_ptr<Tcb> listener;
      for (auto& t : tcbs) if (t->listening && t->local_port == dport) { listener = t; break; }
      if (!listener || !(flags & TH_SYN) || (flags & TH_ACK)) return;
      if (listener->backlog.size() >= listener->backlog_limit) return;

      auto c = std::make_shared<Tcb>();
      c->id = next_id++;
      c->state = SYN_RECEIVED;
      c->remote_ip = src;
      c->remote_port = sport;
      c->local_port = dport;
      c->irs = seq;
      c->rcv_nxt = seq + 1;
      c->iss = rand32();
      c->snd_una = c->iss;
      c->snd_nxt = c->iss;
      c->snd_wnd = wnd;
      for (size_t o = 20; o + 1 < hl; ) {
        uint8_t k = p[o]; if (k == 0) break; if (k == 1) { o++; continue; }
        uint8_t l = p[o + 1]; if (l < 2) break;
        if (k == 2 && l == 4) c->mss = r16(p + o + 2);
        o += l;
      }
      tcbs.push_back(c);
      tcp_out(*c, TH_SYN | TH_ACK, nullptr, 0, true);
      c->snd_nxt = c->iss + 1;
      return;
    }
    Tcb& t = *tp;
    if (flags & TH_RST) { t.reset = true; t.state = CLOSED; return; }
    t.snd_wnd = wnd;
    if (t.state == SYN_SENT) {
      if ((flags & TH_SYN) && (flags & TH_ACK)) {
        t.irs = seq; t.rcv_nxt = seq + 1; t.snd_una = ack; t.snd_nxt = ack; t.syn_acked = true;
        // parse MSS
        for (size_t o = 20; o + 1 < hl; ) { uint8_t k = p[o]; if (k == 0) break; if (k == 1) { o++; continue; } uint8_t l = p[o + 1]; if (l < 2) break; if (k == 2 && l == 4) t.mss = r16(p + o + 2); o += l; }
        t.state = ESTABLISHED; tcp_out(t, TH_ACK, nullptr, 0);
      }
      return;
    }
    if (t.state == SYN_RECEIVED) {
      // The third leg. Its ACK must cover our SYN, or it belongs to some other
      // conversation and this one is not open yet.
      if (!(flags & TH_ACK) || (int32_t)(ack - t.iss) <= 0) return;
      t.snd_una = ack;
      t.snd_nxt = ack;
      t.syn_acked = true;
      t.state = ESTABLISHED;
      for (auto& l : tcbs) {
        if (l->listening && l->local_port == t.local_port) { l->backlog.push_back(t.id); break; }
      }
      // The ACK may carry data; fall through so it is not dropped.
    }

    // established-ish: accept in-order data
    if ((flags & TH_ACK) && (int32_t)(ack - t.snd_una) > 0) {
      uint32_t adv = ack - t.snd_una;
      if (adv <= t.tx.size()) t.tx.erase(t.tx.begin(), t.tx.begin() + adv); else t.tx.clear();
      t.snd_una = ack; t.retries = 0; t.rto_ms = 500;
    }
    if (dlen && seq == t.rcv_nxt) {
      for (size_t i = 0; i < dlen; ++i) t.rx.push_back(data[i]);
      t.rcv_nxt += dlen; tcp_out(t, TH_ACK, nullptr, 0);
    } else if (dlen) { tcp_out(t, TH_ACK, nullptr, 0); }   // out of order → dup ack
    if (flags & TH_FIN) {
      if (seq == t.rcv_nxt || (dlen && seq + dlen == t.rcv_nxt)) { t.rcv_nxt++; t.peer_fin = true; tcp_out(t, TH_ACK, nullptr, 0);
        if (t.state == ESTABLISHED) t.state = CLOSE_WAIT; else if (t.state == FIN_WAIT_1 || t.state == FIN_WAIT_2) t.state = TIME_WAIT; }
    }
    if (t.fin_sent && t.snd_una == t.snd_nxt) { if (t.state == FIN_WAIT_1) t.state = FIN_WAIT_2; }
  }
  void tcp_push(Tcb& t, const uint8_t* data, size_t len) {
    size_t off = 0;
    while (off < len) { size_t chunk = std::min<size_t>(t.mss, len - off);
      for (size_t i = 0; i < chunk; ++i) t.tx.push_back(data[off + i]);
      tcp_out(t, TH_PSH | TH_ACK, data + off, chunk); t.snd_nxt += chunk; off += chunk; }
  }
  void tcp_timers() {
    uint64_t now = now_ns();
    for (auto& tp : tcbs) { Tcb& t = *tp;
      if ((t.state == SYN_SENT) && now - t.last_tx_ns > (uint64_t)t.rto_ms * 1000000ull) {
        if (++t.retries > 8) { t.reset = true; t.state = CLOSED; continue; }
        t.rto_ms = std::min<uint32_t>(t.rto_ms * 2, 4000); t.snd_nxt = t.iss; tcp_out(t, TH_SYN, nullptr, 0, true); owner.stats_.tcp_retransmits++;
      } else if (!t.tx.empty() && now - t.last_tx_ns > (uint64_t)t.rto_ms * 1000000ull) {
        if (++t.retries > 8) { t.reset = true; t.state = CLOSED; continue; }
        t.rto_ms = std::min<uint32_t>(t.rto_ms * 2, 4000);
        size_t chunk = std::min<size_t>(t.mss, t.tx.size());
        std::vector<uint8_t> d(t.tx.begin(), t.tx.begin() + chunk);
        uint32_t save = t.snd_nxt; t.snd_nxt = t.snd_una; tcp_out(t, TH_PSH | TH_ACK, d.data(), d.size()); t.snd_nxt = save;
        owner.stats_.tcp_retransmits++;
      }
    }
  }

  // ---- pump ----
  void pump_once() {
    uint8_t f[1600]; int n = dev.recv(f, sizeof f);
    if (n >= 14) {
      owner.stats_.rx_frames++;
      uint16_t et = r16(f + 12);
      if (et == ET_ARP) arp_input(f + 14, n - 14);
      else if (et == ET_IPV4) ip_input(f + 14, n - 14);
    }
    tcp_timers();
  }
};

// ---- NetStack facade -----------------------------------------------------
NetStack::NetStack(FrameDevice& dev, uint64_t (*now_ns)(), uint32_t (*rand32)())
  : d_(new Impl(*this, dev, now_ns, rand32)), dev_(dev) { d_->mac = dev.mac(); }
NetStack::~NetStack() = default;

Result<void> NetStack::configure(const NetConfig& cfg, int dhcp_timeout_ms) {
  if (cfg.ip.is_zero()) {
    if (!d_->dhcp()) return Result<void>(Status::IoError, "DHCP got no lease (no DHCP server on this link?)");
  } else { d_->ip = cfg.ip; d_->mask = cfg.netmask; d_->gw = cfg.gateway; d_->dns = cfg.dns; }
  cfg_.ip = d_->ip; cfg_.netmask = d_->mask; cfg_.gateway = d_->gw; cfg_.dns = d_->dns;
  // Resolve the gateway MAC up front so steady-state sends never block.
  if (!d_->gw.is_zero()) { Mac m; if (d_->arp_resolve(d_->gw, m, dhcp_timeout_ms)) { d_->gw_mac = m; d_->gw_mac_known = true; } }
  up_ = true;
  return Ok();
}

void NetStack::poll() { if (up_) d_->pump_once(); }

Result<Ipv4> NetStack::resolve(const std::string& host, int timeout_ms) {
  Ipv4 lit; if (parse_ipv4(host, lit)) return lit;
  Ipv4 out; if (d_->dns_query(host, out, timeout_ms)) return out;
  return Result<Ipv4>(Status::NotFound, "DNS could not resolve " + host);
}

Result<uint64_t> NetStack::ping(const Ipv4& dst, int timeout_ms) {
  uint8_t icmp[64]; memset(icmp, 0, sizeof icmp); icmp[0] = 8; w16(icmp + 4, 0x4149); w16(icmp + 6, 1);
  for (int i = 8; i < 64; ++i) icmp[i] = (uint8_t)i;
  w16(icmp + 2, ip_checksum(icmp, 64));
  d_->icmp_reply = false; uint64_t t0 = d_->now_ns();
  d_->ip_send(dst, IPPROTO_ICMP, icmp, 64);
  uint64_t end = d_->now_ms() + timeout_ms;
  while (d_->now_ms() < end) { d_->pump_once(); if (d_->icmp_reply) return (d_->now_ns() - t0) / 1000; }
  return Result<uint64_t>(Status::IoError, "no ICMP echo reply from " + dst.str());
}

Result<void> NetStack::listen(uint16_t port, size_t backlog) {
  if (port == 0) return Result<void>(Status::Invalid, "port 0 is not a port");
  for (auto& t : d_->tcbs) {
    if (t->listening && t->local_port == port) return Result<void>(Status::Busy, "already listening on that port");
    if (!t->listening && t->local_port == port) return Result<void>(Status::Busy, "that port is in a connection");
  }
  auto l = std::make_shared<Tcb>();
  l->id = d_->next_id++;
  l->state = LISTEN;
  l->listening = true;
  l->local_port = port;
  l->backlog_limit = backlog ? backlog : 1;
  d_->tcbs.push_back(l);
  return Ok();
}

Result<void> NetStack::unlisten(uint16_t port) {
  for (size_t i = 0; i < d_->tcbs.size(); ++i) {
    if (d_->tcbs[i]->listening && d_->tcbs[i]->local_port == port) {
      d_->tcbs.erase(d_->tcbs.begin() + i);
      return Ok();
    }
  }
  return Result<void>(Status::NotFound, "nothing is listening on that port");
}

bool NetStack::listening(uint16_t port) const {
  for (auto& t : d_->tcbs) if (t->listening && t->local_port == port) return true;
  return false;
}

Result<std::unique_ptr<TcpSocket>> NetStack::accept(uint16_t port, int timeout_ms) {
  std::shared_ptr<Tcb> listener;
  for (auto& t : d_->tcbs) if (t->listening && t->local_port == port) { listener = t; break; }
  if (!listener) return Result<std::unique_ptr<TcpSocket>>(Status::NotFound, "nothing is listening on that port");

  uint64_t end = d_->now_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);
  for (;;) {
    d_->pump_once();
    while (!listener->backlog.empty()) {
      uint32_t id = listener->backlog.front();
      listener->backlog.pop_front();
      if (auto c = d_->tcb_of(id); c && c->state == ESTABLISHED) {
        return std::unique_ptr<TcpSocket>(new TcpSocket(*this, id));
      }
      // The caller hung up between the handshake and this call. Not an error.
    }
    if (d_->now_ms() >= end) break;
  }
  return Result<std::unique_ptr<TcpSocket>>(Status::NotFound, "nobody called");
}

Result<std::unique_ptr<TcpSocket>> NetStack::connect(const std::string& host, uint16_t port, int timeout_ms) {
  if (!up_) return Result<std::unique_ptr<TcpSocket>>(Status::Unsupported, "network is not configured");
  auto rip = resolve(host, std::min(timeout_ms, 5000));
  if (!rip.ok()) return rip.error();
  auto t = std::make_shared<Tcb>();
  t->id = d_->next_id++; t->remote_ip = *rip; t->remote_port = port; t->local_port = d_->eph_port();
  t->iss = d_->rand32(); t->snd_una = t->iss; t->snd_nxt = t->iss; t->state = SYN_SENT;
  d_->tcbs.push_back(t);
  d_->tcp_out(*t, TH_SYN, nullptr, 0, true); t->snd_nxt = t->iss + 1;
  uint64_t end = d_->now_ms() + timeout_ms;
  while (d_->now_ms() < end) { d_->pump_once(); if (t->state == ESTABLISHED) return std::unique_ptr<TcpSocket>(new TcpSocket(*this, t->id)); if (t->reset || t->state == CLOSED) break; }
  for (size_t i = 0; i < d_->tcbs.size(); ++i) if (d_->tcbs[i]->id == t->id) { d_->tcbs.erase(d_->tcbs.begin() + i); break; }
  return Result<std::unique_ptr<TcpSocket>>(Status::IoError, t->reset ? "connection refused by " + rip->str() : "connect timed out to " + rip->str() + ":" + std::to_string(port));
}

// ---- TcpSocket -----------------------------------------------------------
TcpSocket::~TcpSocket() { close(); }
bool TcpSocket::send_all(ByteSpan data) {
  auto t = stack_.d_->tcb_of(id_); if (!t || t->state != ESTABLISHED) return false;
  stack_.d_->tcp_push(*t, data.data(), data.size());
  // drain a little so we don't unbounded-buffer
  uint64_t end = stack_.d_->now_ms() + 8000;
  while (!t->tx.empty() && stack_.d_->now_ms() < end && !t->reset) stack_.d_->pump_once();
  return !t->reset;
}
bool TcpSocket::recv_all(MutableByteSpan data, int timeout_ms) {
  auto t = stack_.d_->tcb_of(id_); if (!t) return false;
  size_t need = data.size(), got = 0;
  uint64_t end = stack_.d_->now_ms() + (timeout_ms < 0 ? 3600000 : (uint64_t)timeout_ms);
  while (got < need) {
    while (got < need && !t->rx.empty()) { data.data()[got++] = t->rx.front(); t->rx.pop_front(); }
    if (got == need) break;
    if (t->reset || (t->peer_fin && t->rx.empty())) return false;
    if (stack_.d_->now_ms() >= end) return false;
    stack_.d_->pump_once();
  }
  return true;
}
std::string TcpSocket::peer() const { auto t = stack_.d_->tcb_of(id_); return t ? t->remote_ip.str() + ":" + std::to_string(t->remote_port) : std::string("closed"); }
void TcpSocket::close() {
  auto t = stack_.d_->tcb_of(id_); if (!t) return;
  if (t->state == ESTABLISHED || t->state == CLOSE_WAIT) { stack_.d_->tcp_out(*t, TH_FIN | TH_ACK, nullptr, 0); t->snd_nxt++; t->fin_sent = true; t->state = (t->state == CLOSE_WAIT) ? LAST_ACK : FIN_WAIT_1; }
  for (size_t i = 0; i < stack_.d_->tcbs.size(); ++i) if (stack_.d_->tcbs[i]->id == id_) { stack_.d_->tcbs.erase(stack_.d_->tcbs.begin() + i); break; }
}
bool TcpSocket::alive() const { auto t = stack_.d_->tcb_of(id_); return t && !t->reset && !t->peer_fin; }
size_t TcpSocket::pending() const { auto t = stack_.d_->tcb_of(id_); return t ? t->rx.size() : 0; }

}  // namespace aios::net
