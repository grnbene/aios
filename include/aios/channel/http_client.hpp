#pragma once
#include <string>
#include <utility>
#include <vector>
#include "aios/core/bytes.hpp"
#include "aios/core/result.hpp"
#include "aios/hal/host.hpp"

namespace aios::channel {

// HTTP/1.1 over hal::net. No shell, no curl: this runs wherever the kernel
// runs, including the host boundary that has neither.
struct HttpEndpoint {
  std::string host;
  uint16_t port = 80;
  std::string path = "/";
  bool secure = false;   // https: the request rides inside TLS
  std::string user;
  std::string password;
};

Result<HttpEndpoint> parse_endpoint(const std::string& url);

struct HttpRequest {
  std::string method = "GET";
  HttpEndpoint endpoint;
  std::vector<std::pair<std::string, std::string>> headers;
  ByteBuffer body;
  int timeout_ms = 20000;
  // https needs both: somewhere to get randomness and read the trust store, and
  // the certificates this machine trusts. Without a host an https request is
  // refused rather than quietly downgraded.
  hal::Host* host = nullptr;
  std::string trust_store;      // empty: wherever this substrate keeps them
  std::string kernel_root;      // where the machine's own store would live
};

struct HttpResponse {
  int status = 0;
  std::string reason;
  std::vector<std::pair<std::string, std::string>> headers;
  ByteBuffer body;
  std::string header(const std::string& name) const;
  std::string text() const;
};

Result<HttpResponse> http_send(const HttpRequest& request);
Result<HttpResponse> http_get(const std::string& url, int timeout_ms = 20000);
// The same, over TLS when the url says https.
Result<HttpResponse> http_get_secure(const std::string& url, hal::Host& host, const std::string& kernel_root,
                                     int timeout_ms = 60000);

// What the transport did, for a caller that wants to report it.
struct HttpTransportNote {
  std::string tls_version, cipher, subject, issuer, trust_anchor;
  bool resumed = false, client_certificate_sent = false;
  size_t chain_length = 0;
};
Result<HttpResponse> http_send_noted(const HttpRequest& request, HttpTransportNote* note);

}  // namespace aios::channel
