/* aios::hal::Connection/Listener/net_connect/net_listen for ARM64 bare
 * metal, backed by this substrate's own real TCP state machine (net.cpp:
 * net_tcp_connect/send/recv/close - already used by cmd_tcp at the shell).
 * This is the piece Kernel::fleet_join() actually needs to dial a hub: one
 * outbound TCP connection is the whole transport a RelayClient attach()
 * requires, everything past that (the roster, circuits, the per-node
 * ChaCha20-Poly1305 handshake) is bytes over this Connection, unchanged. */
#include "aios/hal/net.hpp"
#include "aios/hal/tty.hpp"
#include "aios/hal/account.hpp"
#include "net.hpp"
#include "timer.hpp"
#include <cstring>

namespace aios::hal {

namespace {

// "a.b.c.d" only - this fleet's hub is given as a literal LAN address, not a
// hostname, so a real resolver is not today's blocker. Documented rather
// than silently wrong: a non-numeric host fails net_connect() with a clear
// error instead of being misparsed as 0.0.0.0.
bool parse_ipv4(const std::string& host, uint8_t out[4]) {
  unsigned v[4]; int n = 0;
  const char* p = host.c_str();
  for (int i = 0; i < 4; ++i) {
    unsigned val = 0; int digits = 0;
    while (*p >= '0' && *p <= '9') { val = val * 10 + static_cast<unsigned>(*p - '0'); ++p; ++digits; }
    if (digits == 0 || val > 255) return false;
    v[i] = val; ++n;
    if (i < 3) { if (*p != '.') return false; ++p; }
  }
  if (*p != '\0' || n != 4) return false;
  for (int i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>(v[i]);
  return true;
}

class Arm64TcpConnection : public Connection {
 public:
  explicit Arm64TcpConnection(int sock) : sock_(sock) {}
  ~Arm64TcpConnection() override { close(); }

  bool send_all(ByteSpan data) override {
    if (sock_ < 0) return false;
    return net_tcp_send_all(sock_, data.data(), data.size());
  }
  bool recv_all(MutableByteSpan data, int timeout_ms) override {
    if (sock_ < 0) return false;
    return net_tcp_recv_all(sock_, data.data(), data.size(), static_cast<uint32_t>(timeout_ms < 0 ? 0 : timeout_ms));
  }
  std::string peer() const override {
    if (sock_ < 0) return "";
    uint8_t ip[4]; uint16_t port = 0;
    if (!net_tcp_peer(sock_, ip, port)) return "";
    char buf[24];
    ip_to_str(ip, buf, sizeof buf);
    return std::string(buf) + ":" + std::to_string(port);
  }
  void close() override {
    if (sock_ >= 0) { net_tcp_close(sock_); sock_ = -1; }
  }
  /* Gone once the peer has closed its side and everything it sent has been
   * read - what the hosted HAL reports too (its recv sees EOF). CLOSE_WAIT
   * used to count as alive, so a reader never learned the peer was done. */
  bool alive() const override {
    if (sock_ < 0 || !net_tcp_is_alive(sock_)) return false;
    const TcpState st = net_tcp_state(sock_);
    const bool peer_done = st == TcpState::CloseWait || st == TcpState::LastAck || st == TcpState::Closing || st == TcpState::TimeWait;
    return !peer_done || net_tcp_pending(sock_) > 0;
  }
  long recv_some(MutableByteSpan data, int timeout_ms) override {
    if (sock_ < 0) return -1;
    if (data.size() == 0) return 0;
    /* net_tcp_recv answers 0 for both "nothing yet" and end of stream;
     * alive() tells the caller which. */
    return net_tcp_recv(sock_, data.data(), data.size(), static_cast<uint32_t>(timeout_ms < 0 ? 0 : timeout_ms));
  }
  size_t pending() const override { return sock_ >= 0 ? net_tcp_pending(sock_) : 0; }

 private:
  int sock_;
};

class Arm64TcpListener : public Listener {
 public:
  explicit Arm64TcpListener(uint16_t port) : port_(port) {}
  ~Arm64TcpListener() override { close(); }

  Result<std::unique_ptr<Connection>> accept(int timeout_ms) override {
    int listen_sock = net_tcp_listen(port_);
    if (listen_sock < 0) return Result<std::unique_ptr<Connection>>(Status::IoError, "listen failed");
    int c = net_tcp_accept(listen_sock, static_cast<uint32_t>(timeout_ms < 0 ? 0 : timeout_ms));
    if (c < 0) return Result<std::unique_ptr<Connection>>(Status::Busy, "no connection pending");
    return std::unique_ptr<Connection>(new Arm64TcpConnection(c));
  }
  uint16_t port() const override { return port_; }
  void close() override { net_tcp_unlisten(port_); }

 private:
  uint16_t port_;
};

class Arm64UdpSocket : public UdpSocket {
 public:
  explicit Arm64UdpSocket(int sock) : sock_(sock) {}
  ~Arm64UdpSocket() override { close(); }
  bool send_to(const std::string& host, uint16_t port, ByteSpan data, std::string& error) override {
    uint8_t ip[4];
    if (!parse_ipv4(host, ip)) { error = "'" + host + "' is not a dotted-decimal IPv4 address (no resolver on this substrate yet)"; return false; }
    const int r = net_udp_send(sock_, ip, port, data.data(), data.size());
    if (r >= 0) return true;
    error = r == -2 ? "datagram larger than one frame (1472 bytes)" : r == -3 ? "no network link" : r == -4 ? "no ARP answer from " + host : "socket is closed";
    return false;
  }
  Result<Datagram> recv_from(size_t max_len, int timeout_ms) override {
    if (sock_ < 0) return Result<Datagram>(Status::Invalid, "socket is closed");
    std::string buf(max_len ? max_len : 1, '\0');
    uint8_t ip[4] = {};
    uint16_t port = 0;
    const int n = net_udp_recv(sock_, buf.data(), buf.size(), ip, port, static_cast<uint32_t>(timeout_ms < 0 ? 0 : timeout_ms));
    if (n < 0) return Result<Datagram>(Status::Invalid, "socket is closed");
    if (n == 0 && port == 0) return Result<Datagram>(Status::Busy, "nothing arrived");
    buf.resize(static_cast<size_t>(n));
    char ipbuf[24];
    ip_to_str(ip, ipbuf, sizeof ipbuf);
    Datagram d;
    d.data = std::move(buf);
    d.host = ipbuf;
    d.port = port;
    return d;
  }
  uint16_t port() const override { return net_udp_port(sock_); }
  void close() override { if (sock_ >= 0) { net_udp_close(sock_); sock_ = -1; } }
 private:
  int sock_;
};

}  // namespace

void net_service(int timeout_ms) {
  const uint64_t until = metal_now_ns() + static_cast<uint64_t>(timeout_ms < 0 ? 0 : timeout_ms) * 1000000ull;
  do { net_poll(); } while (metal_now_ns() < until);
}

Result<std::unique_ptr<UdpSocket>> udp_open(const std::string& bind_host, uint16_t port) {
  /* One interface here: "", "0.0.0.0" and this board's own address all mean it. */
  if (!bind_host.empty() && bind_host != "0.0.0.0" && bind_host != "127.0.0.1") {
    uint8_t ip[4];
    if (!parse_ipv4(bind_host, ip)) return Result<std::unique_ptr<UdpSocket>>(Status::Invalid, "'" + bind_host + "' is not an IPv4 address");
  }
  const int s = net_udp_open(port);
  if (s == -2) return Result<std::unique_ptr<UdpSocket>>(Status::IoError, "port " + std::to_string(port) + " is in use (by the kernel or another socket)");
  if (s < 0) return Result<std::unique_ptr<UdpSocket>>(Status::IoError, "no free UDP socket (8 at most)");
  return std::unique_ptr<UdpSocket>(new Arm64UdpSocket(s));
}

Result<std::unique_ptr<Connection>> net_connect(const std::string& host, uint16_t port, int timeout_ms) {
  uint8_t ip[4];
  if (!parse_ipv4(host, ip)) {
    return Result<std::unique_ptr<Connection>>(Status::Invalid, "net_connect: '" + host + "' is not a dotted-decimal IPv4 address (no resolver on this substrate yet)");
  }
  int sock = net_tcp_connect(ip, port, static_cast<uint32_t>(timeout_ms < 0 ? 8000 : timeout_ms));
  if (sock < 0) {
    return Result<std::unique_ptr<Connection>>(Status::IoError, "net_connect: TCP connect to " + host + ":" + std::to_string(port) + " failed");
  }
  return std::unique_ptr<Connection>(new Arm64TcpConnection(sock));
}

Result<std::unique_ptr<Listener>> net_listen(uint16_t port, bool loopback_only) {
  (void)loopback_only;  // this substrate has one interface set; there is no loopback to prefer over it
  return std::unique_ptr<Listener>(new Arm64TcpListener(port));
}

// No filesystem sockets on this substrate. src/ calls these (the ASQL wire
// server's unix socket, the FastCGI client's unix:/path), so they must link.
Result<std::unique_ptr<Listener>> net_listen_unix(const std::string&) {
  return Result<std::unique_ptr<Listener>>(Status::Unsupported, "no unix sockets on the arm64 metal substrate");
}
// No controlling terminal to put a question to on this substrate; ssh treats this as nobody to ask.
// No accounts and no host name: one machine, one owner, nothing to look a person up in.
Result<LocalAccount> local_account() {
  return Result<LocalAccount>(Status::Unsupported, "no accounts on the arm64 metal substrate");
}
std::string local_hostname() { return {}; }
Result<std::string> tty_ask(const std::string&, bool) {
  return Result<std::string>(Status::Unsupported, "no terminal to ask on the arm64 metal substrate");
}
Result<std::unique_ptr<Connection>> net_connect_unix(const std::string&, int) {
  return Result<std::unique_ptr<Connection>>(Status::Unsupported, "no unix sockets on the arm64 metal substrate");
}
// Named pipes are a Windows facility; nothing on this substrate has one.
Result<std::unique_ptr<Connection>> net_connect_pipe(const std::string&, int) {
  return Result<std::unique_ptr<Connection>>(Status::Unsupported, "no named pipes on the arm64 metal substrate");
}

}  // namespace aios::hal
