#pragma once
// A buffered, line-oriented connection for the mail protocols (IMAP, SMTP): plain, TLS from the first
// byte (ports 993 and 465), or plain first and upgraded in place (STARTTLS, ports 143 and 587). It is
// the kernel's own TLS client over the kernel's own sockets; nothing is shelled out to.

#include <memory>
#include <string>
#include <string_view>

#include "aios/core/result.hpp"
#include "aios/hal/host.hpp"
#include "aios/hal/net.hpp"
#include "aios/net/tls.hpp"

namespace aios::net {

enum class Security : uint8_t {
  None,       // plain TCP and stay there (a loopback test server, a trusted LAN)
  Tls,        // TLS from the first byte
  StartTls,   // plain, then the protocol's STARTTLS command, then TLS
};

class LineStream {
 public:
  // `kernel_root` locates this machine's trust store; `trust_store` overrides it when non-empty.
  LineStream(hal::Host& host, std::string kernel_root, int timeout_ms = 30000, std::string trust_store = {});
  ~LineStream();

  Result<void> connect(const std::string& host, uint16_t port, Security security);
  // After the server has agreed to STARTTLS: the handshake over the open socket. Anything the peer
  // sent after its "go ahead" and before this call would be lost, and a correct peer sends none.
  Result<void> start_tls();

  Result<void> write(std::string_view data);
  // One line without its CRLF (a bare LF is accepted too). Fails past `max_line` bytes.
  Result<std::string> read_line(size_t max_line = 1 << 20);
  // Exactly `n` bytes.
  Result<std::string> read_exact(size_t n);
  void close();

  bool encrypted() const { return tls_ != nullptr; }
  const TlsInfo* tls_info() const { return tls_ ? &tls_->info() : nullptr; }
  const std::string& peer_host() const { return host_name_; }
  void set_timeout(int ms) { timeout_ms_ = ms; }

 private:
  Result<void> fill();   // at least one more byte into buf_, or an error

  hal::Host& host_;
  std::string kernel_root_;
  std::string trust_store_;
  int timeout_ms_;
  std::string host_name_;
  uint16_t port_ = 0;
  std::unique_ptr<hal::Connection> plain_;
  std::unique_ptr<TlsClient> tls_;
  std::string buf_;
  size_t pos_ = 0;
  bool eof_ = false;
};

}  // namespace aios::net
