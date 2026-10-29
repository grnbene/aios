#pragma once
// TLS 1.3 (RFC 8446) and 1.2 (RFC 5246), client side, so this machine can talk
// to the parts of the internet that will not talk in the clear - the npm
// registry, and databases (PostgreSQL, MS SQL Server) that require encryption.
//
// TLS 1.3: ChaCha20-Poly1305, AES-128-GCM, AES-256-GCM. TLS 1.2: ECDHE with
// RSA or ECDSA certificates and ChaCha20-Poly1305, AES-GCM, or - offered last,
// for older Windows Schannel (Server 2012 R2) that has no ECDHE_RSA GCM suite -
// AES-CBC with HMAC-SHA1/256/384. Key exchange over X25519 or P-256. The
// server's certificate chain is verified against this machine's own trust
// store (and its name or IP address checked) before a single byte of
// application data is sent; a chain that does not verify ends the connection
// with the reason, never a warning.
#include <array>
#include <memory>
#include <string>
#include <vector>

#include "aios/core/bytes.hpp"
#include "aios/core/result.hpp"
#include "aios/crypto/curve25519.hpp"
#include "aios/crypto/x509.hpp"
#include "aios/hal/host.hpp"
#include "aios/hal/net.hpp"

namespace aios::net {

struct TlsInfo {
  std::string version = "TLS 1.3";
  std::string cipher = "TLS_CHACHA20_POLY1305_SHA256";
  std::string group = "x25519";
  std::string peer_signature;          // what the server signed its handshake with
  std::string subject;                 // who the certificate says it is
  std::string issuer;
  std::string trust_anchor;            // the certificate in this machine's store the chain reached
  bool resumed = false;                // the session was resumed from a ticket (no certificate exchange)
  bool client_certificate_sent = false;  // the server asked for a certificate and got this machine's
  size_t chain_length = 0;
  uint64_t bytes_in = 0, bytes_out = 0;
};

class TlsClient {
 public:
  TlsClient(hal::Host& host, std::string trust_store_path);
  ~TlsClient();

  // The certificate this machine presents when a server asks for one. Without
  // this call, `client.crt` and `client.key` beside the trust store are used
  // when they exist; otherwise an empty Certificate is sent and the server decides.
  Result<void> use_client_certificate(const std::string& chain_path, const std::string& key_path);
  // Connect, do the handshake, verify the chain covers `host_name`. A ticket
  // the server issued earlier in this process (same host and port) resumes.
  Result<void> connect(const std::string& host_name, uint16_t port, int timeout_ms = 20000);
  // The same handshake over a connection somebody else opened: how STARTTLS works, where a protocol
  // (SMTP, IMAP) speaks in the clear first and agrees to switch. `conn` is the open socket, with
  // nothing of the peer's still unread on it; `host_name` is who the certificate has to cover.
  Result<void> connect_over(std::unique_ptr<hal::Connection> conn, const std::string& host_name, uint16_t port,
                            int timeout_ms = 20000);
  Result<void> write(ByteSpan data);
  // Whatever has arrived; an empty buffer means the peer closed cleanly.
  Result<ByteBuffer> read(int timeout_ms);
  void close();
  const TlsInfo& info() const { return info_; }
  bool closed() const { return closed_; }

 private:
  struct Impl;
  // TLS 1.2, for the servers that will not speak 1.3. Same certificate check,
  // same cipher, a different way of agreeing on the keys.
  Result<void> handshake12(const std::string& host_name, const ByteBuffer& client_random,
                           const std::array<uint8_t, 32>& server_random, const crypto::Scalar32& our_secret, const ByteBuffer& p256_secret,
                           const std::vector<crypto::Certificate>& anchors, int timeout_ms);
  std::unique_ptr<Impl> impl_;
  TlsInfo info_;
  bool closed_ = false;
};

class TlsServer {
 public:
  TlsServer(hal::Host& host, std::string cert_chain_path, std::string private_key_path);
  TlsServer(hal::Host& host, std::vector<crypto::Certificate> chain, crypto::PrivateKey key);
  ~TlsServer();

  // Accept a client on an open connection and perform the server TLS handshake
  Result<void> accept(std::unique_ptr<hal::Connection> conn, int timeout_ms = 20000);
  Result<void> write(ByteSpan data);
  // Whatever has arrived; an empty buffer means the peer closed cleanly.
  Result<ByteBuffer> read(int timeout_ms);
  void close();
  const TlsInfo& info() const { return info_; }
  bool closed() const { return closed_; }
  // The connection under the TLS record layer (for its poll_fd), null before accept.
  const hal::Connection* transport() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  TlsInfo info_;
  bool closed_ = false;
};

// Where this machine keeps the certificates it trusts. A hosted substrate can
// be pointed at the one the host already maintains; the kernel never invents a
// trust decision it was not given.
std::string default_trust_store(hal::Host& host, const std::string& kernel_root);

}  // namespace aios::net
