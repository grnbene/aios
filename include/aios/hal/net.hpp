#pragma once
// The network face of the hardware host: byte streams between nodes. The
// kernel's distribution layer speaks its native framing over these; nothing
// above knows about sockets.
#include <memory>
#include <string>
#include <cstdint>
#include <climits>
#include "aios/core/result.hpp"
#include "aios/core/bytes.hpp"

namespace aios::hal {
class Connection {
 public:
  virtual ~Connection() = default;
  virtual bool send_all(ByteSpan data) = 0;
  virtual bool recv_all(MutableByteSpan data, int timeout_ms) = 0;
  virtual std::string peer() const = 0;
  virtual void close() = 0;
  virtual bool alive() const { return true; }   // false once the peer is known to be gone
  virtual size_t pending() const { return SIZE_MAX; }   // bytes readable without waiting (unknown = SIZE_MAX)
  // Whatever has arrived, up to data.size(): bytes read, 0 = nothing within
  // timeout_ms (check alive() to tell that from the peer having closed),
  // -1 = error. The default makes do with recv_all over what pending() knows
  // is there (one byte when it does not know); a real socket overrides it.
  virtual long recv_some(MutableByteSpan data, int timeout_ms) {
    if (data.size() == 0) return 0;
    size_t want = pending();
    if (want == 0 || want == SIZE_MAX) want = 1;
    if (want > data.size()) want = data.size();
    return recv_all(MutableByteSpan(data.data(), want), timeout_ms) ? static_cast<long>(want) : 0;
  }
};
class Listener {
 public:
  virtual ~Listener() = default;
  virtual Result<std::unique_ptr<Connection>> accept(int timeout_ms) = 0;   // Busy on timeout
  virtual uint16_t port() const = 0;
  virtual void close() = 0;
};
// loopback_only binds the local interface alone: a service meant for the
// processes on this machine and nothing beyond it.
Result<std::unique_ptr<Listener>> net_listen(uint16_t port, bool loopback_only = false);
// A unix-domain socket at `path` (replaced if a stale one is there), open to
// every local user: a client on the same machine skips the TCP stack - the
// ERP's PHP reaches MariaDB this way and was 30 us a statement ahead of us.
// port() is 0. Not supported off Linux.
Result<std::unique_ptr<Listener>> net_listen_unix(const std::string& path);
// The client side of the same: connect to the unix-domain socket at `path`.
Result<std::unique_ptr<Connection>> net_connect_unix(const std::string& path, int timeout_ms);

// Datagrams. A UdpSocket is bound to a local port (0 = any free one) and
// sends to / receives from any IPv4 host; "255.255.255.255" broadcasts.
struct Datagram {
  std::string data;
  std::string host;   // sender, dotted quad
  uint16_t port = 0;
};
class UdpSocket {
 public:
  virtual ~UdpSocket() = default;
  virtual bool send_to(const std::string& host, uint16_t port, ByteSpan data, std::string& error) = 0;
  virtual Result<Datagram> recv_from(size_t max_len, int timeout_ms) = 0;   // Busy on timeout
  virtual uint16_t port() const = 0;
  virtual void close() = 0;
};
// Let the network make progress for up to timeout_ms. Hosted kernels move
// packets on their own, so this just waits; a polled kernel (the Pi) runs its
// receive/transmit loop - what a caller spinning on "has it arrived yet" must
// do there, or nothing ever arrives.
void net_service(int timeout_ms);
// How a hosted connection moves its bytes: "io_uring" where the kernel gave us
// a ring, "poll" otherwise. Probed, not configured.
const char* net_backend();

// bind_host "" or "0.0.0.0" = every interface.
Result<std::unique_ptr<UdpSocket>> udp_open(const std::string& bind_host, uint16_t port);
Result<std::unique_ptr<Connection>> net_connect(const std::string& host, uint16_t port, int timeout_ms);

// The client side of a named pipe: the Windows OpenSSH agent answers at
// \\.\pipe\openssh-ssh-agent, and nothing else this machine talks to lives
// behind a pipe. Only a Windows substrate has pipes: every other one answers
// Status::Unsupported.
Result<std::unique_ptr<Connection>> net_connect_pipe(const std::string& name, int timeout_ms);
}  // namespace aios::hal
