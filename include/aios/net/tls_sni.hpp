#pragma once
// SNI-based vhost dispatch for one process terminating TLS for many domains
// on a single port — what nginx's per-server_name cert selection does, and
// what a naive single-cert TlsServer cannot: TlsServer::accept() is
// constructed with one chain+key before it ever sees a ClientHello, so
// picking the right one means reading enough of the ClientHello to find the
// server_name extension *before* handing the connection to TlsServer, then
// replaying those same bytes so TlsServer's own handshake parse sees them
// again from the start.
//
// Limitation: only handles a ClientHello that arrives in a single TLS record,
// which is effectively every real client (browsers, curl, PHP/Node HTTP
// clients) — a ClientHello fragmented across multiple records is rejected
// rather than misparsed.
#include <memory>
#include <string>

#include "aios/core/result.hpp"
#include "aios/hal/net.hpp"

namespace aios::net {

struct SniPeekResult {
  std::string server_name;             // empty if the ClientHello had no SNI extension
  std::unique_ptr<hal::Connection> conn;  // replays the peeked bytes, then reads live
};

Result<SniPeekResult> peek_client_hello_sni(std::unique_ptr<hal::Connection> conn, int timeout_ms);

}  // namespace aios::net
