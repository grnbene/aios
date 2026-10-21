#pragma once
// AOS's own TCP/IP stack. It rides a hal::FrameDevice (a NIC's raw Ethernet
// send/recv) directly — Ethernet II, ARP, IPv4, ICMP, UDP, DNS, DHCP and a
// client-side TCP — with NO host network stack underneath. This is what lets a
// bare-metal AOS reach a real internet host (a Bitcoin peer, an RPC node)
// without borrowing anyone's sockets. The AOS fabric (ethertype 0x88B5) keeps
// running on the same NIC; this stack owns 0x0800 (IPv4) and 0x0806 (ARP).
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <deque>
#include "aios/core/result.hpp"
#include "aios/core/bytes.hpp"
#include "aios/hal/fabric.hpp"   // FrameDevice, Mac
#include "aios/hal/net.hpp"      // Connection

namespace aios::net {

using aios::hal::Mac;
using aios::hal::FrameDevice;

struct Ipv4 {
  uint8_t b[4] = {0, 0, 0, 0};
  bool operator==(const Ipv4& o) const { for (int i = 0; i < 4; ++i) if (b[i] != o.b[i]) return false; return true; }
  bool is_zero() const { return b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0; }
  uint32_t u32() const { return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3]; }
  std::string str() const;
};
bool parse_ipv4(const std::string& s, Ipv4& out);

struct NetConfig {
  Ipv4 ip, netmask, gateway, dns;   // if ip is zero, DHCP fills these in
};

struct NetStats {
  uint64_t rx_frames = 0, tx_frames = 0, arp_rx = 0, arp_tx = 0, ip_rx = 0, ip_tx = 0,
           icmp_rx = 0, udp_rx = 0, tcp_rx = 0, tcp_retransmits = 0, dropped = 0;
};

class NetStack;

// A TCP connection, presented as a hal::Connection so the existing transports
// (http_client, the Bitcoin P2P wire) use it unchanged.
class TcpSocket : public aios::hal::Connection {
 public:
  ~TcpSocket() override;
  bool send_all(ByteSpan data) override;
  bool recv_all(MutableByteSpan data, int timeout_ms) override;
  std::string peer() const override;
  void close() override;
  bool alive() const override;
  size_t pending() const override;
 private:
  friend class NetStack;
  TcpSocket(NetStack& s, uint32_t id) : stack_(s), id_(id) {}
  NetStack& stack_;
  uint32_t id_;
};

class NetStack {
 public:
  // now_ns and rand32 come from the substrate (metal: TSC + RDRAND).
  NetStack(FrameDevice& dev, uint64_t (*now_ns)(), uint32_t (*rand32)());
  ~NetStack();

  // Bring the interface up. With a zero ip in cfg, runs DHCP (timeout ms).
  Result<void> configure(const NetConfig& cfg, int dhcp_timeout_ms = 8000);
  bool up() const { return up_; }
  const NetConfig& config() const { return cfg_; }
  const NetStats& stats() const { return stats_; }

  // Drives the stack: receive pending frames, run TCP/ARP/retransmit timers.
  // Called from whoever owns the CPU (the console idle loop on metal).
  void poll();

  // Resolve an A record via DNS (blocking up to timeout).
  Result<Ipv4> resolve(const std::string& host, int timeout_ms = 5000);
  // ICMP echo; returns round-trip time in microseconds.
  Result<uint64_t> ping(const Ipv4& dst, int timeout_ms = 2000);
  // Open a TCP connection to host:port. host may be a dotted IP or a name.
  Result<std::unique_ptr<TcpSocket>> connect(const std::string& host, uint16_t port, int timeout_ms = 8000);

  // Answer calls on a port. This stack could only ever place one, so a machine
  // running it could reach a peer and no peer could reach it.
  Result<void> listen(uint16_t port, size_t backlog = 8);
  Result<void> unlisten(uint16_t port);
  // The next connection whose handshake has completed on that port, or nothing
  // when none has within the timeout. Not an error: nobody called.
  Result<std::unique_ptr<TcpSocket>> accept(uint16_t port, int timeout_ms = 0);
  bool listening(uint16_t port) const;

  // internal, used by TcpSocket
  struct Impl;
 private:
  friend class TcpSocket;
  std::unique_ptr<Impl> d_;
  FrameDevice& dev_;
  NetConfig cfg_;
  NetStats stats_;
  bool up_ = false;
};

}  // namespace aios::net
