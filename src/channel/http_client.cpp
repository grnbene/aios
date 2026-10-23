#include <cctype>
#include "aios/core/numparse.hpp"
#include "aios/channel/http_client.hpp"
#include <cstring>
#include <memory>
#include "aios/core/base64.hpp"
#include "aios/hal/net.hpp"
#include "aios/net/tls.hpp"

namespace aios::channel {

namespace {

std::string lower_of(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) out.push_back((c >= 'A' && c <= 'Z') ? (char)(c + 32) : c);
  return out;
}

// One byte stream, whether the bytes are in the clear or inside TLS records:
// the HTTP above it is the same protocol either way, and there is one parser.
struct Stream {
  virtual ~Stream() = default;
  virtual bool send(ByteSpan data) = 0;
  virtual bool recv_exact(uint8_t* out, size_t n, int timeout_ms) = 0;
  // Why the last read failed, when the transport can say.
  virtual std::string last_error() const { return {}; }
};

class PlainStream final : public Stream {
 public:
  explicit PlainStream(hal::Connection& c) : c_(c) {}
  bool send(ByteSpan d) override { return c_.send_all(d); }
  bool recv_exact(uint8_t* out, size_t n, int t) override { return c_.recv_all(MutableByteSpan(out, n), t); }
 private:
  hal::Connection& c_;
};

class TlsStream final : public Stream {
 public:
  explicit TlsStream(net::TlsClient& t) : t_(t) {}
  bool send(ByteSpan d) override { return t_.write(d).ok(); }
  bool recv_exact(uint8_t* out, size_t n, int timeout_ms) override {
    while (buf_.size() < n) {
      if (t_.closed()) return false;
      auto chunk = t_.read(timeout_ms);
      if (!chunk.ok()) { err_ = chunk.error().message; return false; }
      if (chunk->empty() && t_.closed()) return false;
      buf_.insert(buf_.end(), chunk->begin(), chunk->end());
    }
    std::memcpy(out, buf_.data(), n);
    buf_.erase(buf_.begin(), buf_.begin() + (long)n);
    return true;
  }
 private:
  net::TlsClient& t_;
  ByteBuffer buf_;
  std::string err_;
 public:
  std::string last_error() const override { return err_; }
};

// hal::Connection fills a whole span or fails, so a line-oriented protocol is
// read a byte at a time and a body is read by its declared length.
class LineReader {
 public:
  LineReader(Stream& c, int timeout_ms) : conn_(c), timeout_(timeout_ms) {}

  bool line(std::string& out) {
    out.clear();
    uint8_t c = 0;
    while (true) {
      if (!conn_.recv_exact(&c, 1, timeout_)) return false;
      if (c == '\n') {
        if (!out.empty() && out.back() == '\r') out.pop_back();
        return true;
      }
      out.push_back((char)c);
      if (out.size() > 16384) return false;
    }
  }

  bool exact(ByteBuffer& out, size_t n) {
    if (n == 0) return true;
    size_t start = out.size();
    out.resize(start + n);
    return conn_.recv_exact(out.data() + start, n, timeout_);
  }

 private:
  Stream& conn_;
  int timeout_;
};

uint64_t parse_hex_length(const std::string& text) {
  uint64_t v = 0;
  for (char c : text) {
    int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else break;
    v = v * 16 + (uint64_t)d;
  }
  return v;
}

}  // namespace

std::string HttpResponse::header(const std::string& name) const {
  std::string want = lower_of(name);
  for (const auto& h : headers) {
    if (lower_of(h.first) == want) return h.second;
  }
  return {};
}

std::string HttpResponse::text() const {
  return std::string(reinterpret_cast<const char*>(body.data()), body.size());
}

Result<HttpEndpoint> parse_endpoint(const std::string& url) {
  HttpEndpoint ep;
  std::string rest = url;

  auto scheme_end = rest.find("://");
  if (scheme_end != std::string::npos) {
    std::string scheme = lower_of(rest.substr(0, scheme_end));
    if (scheme == "https") {
      ep.secure = true;
      ep.port = 443;
    } else if (scheme == "http") {
      ep.port = 80;
    } else {
      return Result<HttpEndpoint>(Status::Unsupported, "unsupported scheme: " + scheme);
    }
    rest = rest.substr(scheme_end + 3);
  }

  auto slash = rest.find('/');
  std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
  ep.path = (slash == std::string::npos) ? "/" : rest.substr(slash);
  if (ep.path.empty()) ep.path = "/";

  auto at = authority.rfind('@');
  if (at != std::string::npos) {
    std::string credentials = authority.substr(0, at);
    // Userinfo is percent-encoded in a URL (a token may contain '@', ':' or '/'); decode it.
    auto decode = [](const std::string& v) {
      std::string out;
      for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '%' && i + 2 < v.size() && std::isxdigit(static_cast<unsigned char>(v[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(v[i + 2]))) {
          out += static_cast<char>(aios::num::to_ll(v.substr(i + 1, 2), 16));
          i += 2;
        } else {
          out += v[i];
        }
      }
      return out;
    };
    authority = authority.substr(at + 1);
    auto colon = credentials.find(':');
    if (colon == std::string::npos) {
      ep.user = decode(credentials);
    } else {
      ep.user = decode(credentials.substr(0, colon));
      ep.password = decode(credentials.substr(colon + 1));
    }
  }

  auto colon = authority.rfind(':');
  if (colon != std::string::npos && authority.find(']') == std::string::npos) {
    ep.host = authority.substr(0, colon);
    uint32_t port = 0;
    for (size_t i = colon + 1; i < authority.size(); ++i) {
      char c = authority[i];
      if (c < '0' || c > '9') return Result<HttpEndpoint>(Status::Invalid, "port is not a number");
      port = port * 10 + (uint32_t)(c - '0');
    }
    if (port == 0 || port > 65535) return Result<HttpEndpoint>(Status::Invalid, "port out of range");
    ep.port = (uint16_t)port;
  } else {
    ep.host = authority;
  }

  if (ep.host.empty()) return Result<HttpEndpoint>(Status::Invalid, "no host in url");
  return ep;
}

Result<HttpResponse> http_send(const HttpRequest& request) { return http_send_noted(request, nullptr); }

Result<HttpResponse> http_send_noted(const HttpRequest& request, HttpTransportNote* note) {
  const auto& ep = request.endpoint;
  std::unique_ptr<hal::Connection> conn_holder;
  std::unique_ptr<net::TlsClient> tls;
  std::unique_ptr<Stream> stream;

  if (ep.secure) {
    if (!request.host)
      return Result<HttpResponse>(Status::Invalid,
                                  "an https request needs the machine's host boundary (for its trust store and its "
                                  "randomness); this caller gave none");
    std::string store = request.trust_store.empty()
                            ? net::default_trust_store(*request.host, request.kernel_root)
                            : request.trust_store;
    tls.reset(new net::TlsClient(*request.host, store));
    if (auto r = tls->connect(ep.host, ep.port, request.timeout_ms); !r.ok()) return Result<HttpResponse>(r.error());
    if (note) {
      note->tls_version = tls->info().version;
      note->cipher = tls->info().cipher;
      note->subject = tls->info().subject;
      note->issuer = tls->info().issuer;
      note->trust_anchor = tls->info().trust_anchor;
      note->resumed = tls->info().resumed;
      note->client_certificate_sent = tls->info().client_certificate_sent;
      note->chain_length = tls->info().chain_length;
    }
    stream.reset(new TlsStream(*tls));
  } else {
    auto conn = hal::net_connect(ep.host, ep.port, request.timeout_ms);
    if (!conn.ok()) return Result<HttpResponse>(conn.error());
    conn_holder = std::move(*conn);
    stream.reset(new PlainStream(*conn_holder));
  }
  Stream& c = *stream;

  std::string head = request.method + " " + ep.path + " HTTP/1.1\r\n";
  // A default port is not named in Host:, which some servers are strict about.
  head += "Host: " + ep.host + ((ep.secure && ep.port == 443) || (!ep.secure && ep.port == 80)
                                    ? std::string()
                                    : ":" + std::to_string(ep.port)) + "\r\n";
  head += "Connection: close\r\n";
  if (!ep.user.empty() || !ep.password.empty()) {
    std::string credentials = ep.user + ":" + ep.password;
    head += "Authorization: Basic " +
            base64_encode(ByteSpan(reinterpret_cast<const uint8_t*>(credentials.data()), credentials.size())) +
            "\r\n";
  }
  for (const auto& h : request.headers) head += h.first + ": " + h.second + "\r\n";
  if (!request.body.empty()) head += "Content-Length: " + std::to_string(request.body.size()) + "\r\n";
  head += "\r\n";

  if (!c.send(ByteSpan(reinterpret_cast<const uint8_t*>(head.data()), head.size()))) {
    return Result<HttpResponse>(Status::IoError, "could not send request head");
  }
  if (!request.body.empty() &&
      !c.send(ByteSpan(request.body.data(), request.body.size()))) {
    return Result<HttpResponse>(Status::IoError, "could not send request body");
  }

  LineReader reader(c, request.timeout_ms);
  HttpResponse response;

  std::string status_line;
  if (!reader.line(status_line))
    return Result<HttpResponse>(Status::IoError, c.last_error().empty() ? std::string("no status line") : c.last_error());
  auto first = status_line.find(' ');
  if (first == std::string::npos) return Result<HttpResponse>(Status::Invalid, "malformed status line");
  auto second = status_line.find(' ', first + 1);
  std::string code = status_line.substr(first + 1, (second == std::string::npos ? status_line.size() : second) - first - 1);
  for (char ch : code) {
    if (ch < '0' || ch > '9') return Result<HttpResponse>(Status::Invalid, "malformed status code");
    response.status = response.status * 10 + (ch - '0');
  }
  if (second != std::string::npos) response.reason = status_line.substr(second + 1);

  while (true) {
    std::string header_line;
    if (!reader.line(header_line)) return Result<HttpResponse>(Status::IoError, "headers truncated");
    if (header_line.empty()) break;
    auto colon = header_line.find(':');
    if (colon == std::string::npos) continue;
    std::string name = header_line.substr(0, colon);
    size_t start = colon + 1;
    while (start < header_line.size() && (header_line[start] == ' ' || header_line[start] == '\t')) ++start;
    response.headers.emplace_back(name, header_line.substr(start));
  }

  std::string encoding = lower_of(response.header("Transfer-Encoding"));
  std::string length_header = response.header("Content-Length");

  if (encoding.find("chunked") != std::string::npos) {
    while (true) {
      std::string size_line;
      if (!reader.line(size_line)) return Result<HttpResponse>(Status::IoError, "chunk header truncated");
      uint64_t chunk = parse_hex_length(size_line);
      if (chunk == 0) break;
      if (chunk > (uint64_t)64 * 1024 * 1024) {
        return Result<HttpResponse>(Status::Exhausted, "chunk larger than the accepted limit");
      }
      if (!reader.exact(response.body, (size_t)chunk)) {
        return Result<HttpResponse>(Status::IoError, "chunk body truncated");
      }
      std::string blank;
      if (!reader.line(blank)) return Result<HttpResponse>(Status::IoError, "chunk terminator missing");
    }
    std::string trailer;
    while (reader.line(trailer) && !trailer.empty()) {
    }
  } else if (!length_header.empty()) {
    uint64_t want = 0;
    for (char ch : length_header) {
      if (ch < '0' || ch > '9') break;
      want = want * 10 + (uint64_t)(ch - '0');
    }
    if (want > (uint64_t)256 * 1024 * 1024) {
      return Result<HttpResponse>(Status::Exhausted, "response larger than the accepted limit");
    }
    if (!reader.exact(response.body, (size_t)want)) {
      return Result<HttpResponse>(Status::IoError, "response body truncated");
    }
  } else {
    // No length and no chunking: the body ends when the peer stops talking.
    // Read in blocks — a byte at a time across a TLS record is thousands of
    // times slower, and a package tarball is megabytes.
    uint8_t block[16384];
    for (;;) {
      size_t got = 0;
      while (got < sizeof(block) && c.recv_exact(block + got, 1, request.timeout_ms)) ++got;
      response.body.insert(response.body.end(), block, block + got);
      if (got < sizeof(block)) break;
      if (response.body.size() > (size_t)64 * 1024 * 1024) break;
    }
  }

  if (conn_holder) conn_holder->close();
  return response;
}

Result<HttpResponse> http_get(const std::string& url, int timeout_ms) {
  auto ep = parse_endpoint(url);
  if (!ep.ok()) return Result<HttpResponse>(ep.error());
  HttpRequest request;
  request.endpoint = *ep;
  request.timeout_ms = timeout_ms;
  return http_send(request);
}

Result<HttpResponse> http_get_secure(const std::string& url, hal::Host& host, const std::string& kernel_root,
                                     int timeout_ms) {
  auto ep = parse_endpoint(url);
  if (!ep.ok()) return Result<HttpResponse>(ep.error());
  HttpRequest request;
  request.endpoint = *ep;
  request.timeout_ms = timeout_ms;
  request.host = &host;
  request.kernel_root = kernel_root;
  return http_send(request);
}

}  // namespace aios::channel
