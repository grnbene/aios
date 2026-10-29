#pragma once
// Makes an already-accepted net::TlsServer look like a plain hal::Connection,
// so http_stream.hpp's reader/writer (and anything else written against
// hal::Connection) work unchanged whether the socket underneath is cleartext
// or TLS. TlsServer's own read()/write() return whole plaintext records
// rather than filling an exact-size buffer, so this adapter buffers the
// leftover of a record between calls — the same pattern net::ConnReader uses
// for socket data, one layer up.
#include <memory>
#include <string>

#include "aios/core/bytes.hpp"
#include "aios/hal/net.hpp"
#include "aios/net/tls.hpp"

namespace aios::net {

class TlsServerConnection : public hal::Connection {
 public:
  TlsServerConnection(std::unique_ptr<TlsServer> server, std::string peer_addr);
  bool send_all(ByteSpan data) override;
  bool recv_all(MutableByteSpan data, int timeout_ms) override;
  long recv_some(MutableByteSpan data, int timeout_ms) override;
  std::string peer() const override { return peer_; }
  void close() override;
  bool alive() const override { return !closed_; }
  // The socket under the TLS layer, so the wire's hang-up watcher sees a TLS client go away mid-statement too.
  int poll_fd() const override { const hal::Connection* t = server_ ? server_->transport() : nullptr; return t ? t->poll_fd() : -1; }

 private:
  std::unique_ptr<TlsServer> server_;
  std::string peer_;
  ByteBuffer buf_;
  size_t pos_ = 0;
  bool closed_ = false;
};

// The client side of the same: an open TlsClient as a hal::Connection, so a
// database wire session (PostgreSQL after SSLRequest, MS SQL after its
// PRELOGIN) reads and writes it exactly as it does a plain socket.
class TlsClientConnection : public hal::Connection {
 public:
  TlsClientConnection(std::unique_ptr<TlsClient> client, std::string peer_addr);
  bool send_all(ByteSpan data) override;
  bool recv_all(MutableByteSpan data, int timeout_ms) override;
  long recv_some(MutableByteSpan data, int timeout_ms) override;
  std::string peer() const override { return peer_; }
  void close() override;
  bool alive() const override { return !closed_; }
  const TlsInfo& info() const { return client_->info(); }

 private:
  std::unique_ptr<TlsClient> client_;
  std::string peer_;
  ByteBuffer buf_;
  size_t pos_ = 0;
  bool closed_ = false;
};

}  // namespace aios::net
