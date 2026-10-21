#include "aios/net/fastcgi_client.hpp"

namespace aios::net {
namespace {

constexpr uint8_t kVersion = 1;
constexpr uint8_t kTypeBeginRequest = 1;
constexpr uint8_t kTypeEndRequest = 3;
constexpr uint8_t kTypeParams = 4;
constexpr uint8_t kTypeStdin = 5;
constexpr uint8_t kTypeStdout = 6;
constexpr uint8_t kTypeStderr = 7;
constexpr uint16_t kRoleResponder = 1;
constexpr uint16_t kRequestId = 1;
constexpr size_t kMaxRecordBody = 65535;  // contentLength is a 16-bit field

void put_be16(ByteBuffer& b, uint16_t v) { b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v)); }
void put_be32(ByteBuffer& b, uint32_t v) {
  b.push_back(uint8_t(v >> 24)); b.push_back(uint8_t(v >> 16)); b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v));
}

// One FastCGI record header + whatever payload it carries, no padding (padding
// exists in the spec purely for alignment on servers that want it; nothing on
// either end of this connection requires it).
void write_record(ByteBuffer& out, uint8_t type, ByteSpan content) {
  out.push_back(kVersion);
  out.push_back(type);
  put_be16(out, kRequestId);
  put_be16(out, uint16_t(content.size()));
  out.push_back(0);  // paddingLength
  out.push_back(0);  // reserved
  out.insert(out.end(), content.begin(), content.end());
}

// A stream (STDIN/PARAMS) is terminated by an empty record of its own type.
void write_stream_chunk(ByteBuffer& out, uint8_t type, ByteSpan data) {
  size_t off = 0;
  if (data.empty()) { write_record(out, type, {}); return; }
  while (off < data.size()) {
    size_t n = std::min(kMaxRecordBody, data.size() - off);
    write_record(out, type, data.subspan(off, n));
    off += n;
  }
}

// FastCGI name/value length encoding: high bit clear = 1 byte, high bit set =
// 4 bytes big-endian with the high bit stripped back off.
void put_fcgi_len(ByteBuffer& b, uint32_t len) {
  if (len < 128) {
    b.push_back(uint8_t(len));
  } else {
    put_be32(b, len | 0x80000000u);
  }
}

ByteBuffer encode_params(const FcgiParams& params) {
  ByteBuffer body;
  for (const auto& [name, value] : params) {
    put_fcgi_len(body, uint32_t(name.size()));
    put_fcgi_len(body, uint32_t(value.size()));
    body.insert(body.end(), name.begin(), name.end());
    body.insert(body.end(), value.begin(), value.end());
  }
  return body;
}

}  // namespace

FastCgiClient::FastCgiClient(std::string host, uint16_t port) : host_(std::move(host)), port_(port) {}

FastCgiClient::Result_ FastCgiClient::run(const FcgiParams& params, const std::function<ByteBuffer()>& next_body_chunk,
                                           const FcgiOutputSink& on_output, int timeout_ms, const std::function<bool()>& should_abort) {
  Result_ res;
  // "unix:/path" is php-fpm's unix socket: no IP stack between us at all -
  // on loopback TCP every segment went through tcp_sendmsg, the softirq and
  // ip_rcv on both sides, ~45% of this server's CPU on a small PHP request.
  auto conn_r = host_.rfind("unix:", 0) == 0 ? hal::net_connect_unix(host_.substr(5), timeout_ms)
                                              : hal::net_connect(host_, port_, timeout_ms);
  if (!conn_r.ok()) { res.error = "connect to php-fpm failed: " + conn_r.error().str(); return res; }
  auto conn = std::move(*conn_r);

  // The request goes out in as few writes as the body allows: BEGIN_REQUEST
  // (RESPONDER, KEEP_CONN=0 - php-fpm closes after this one request), every
  // PARAMS record and the empty one that ends them, and the first STDIN chunk
  // all share one buffer. A request without a body (every GET) is one send;
  // it was three, each its own TCP segment under TCP_NODELAY.
  ByteBuffer wire;
  {
    ByteBuffer begin_body;
    put_be16(begin_body, kRoleResponder);
    begin_body.push_back(0);  // flags
    begin_body.resize(8, 0);
    write_record(wire, kTypeBeginRequest, ByteSpan(begin_body.data(), begin_body.size()));
    ByteBuffer encoded = encode_params(params);
    write_stream_chunk(wire, kTypeParams, ByteSpan(encoded.data(), encoded.size()));
    write_stream_chunk(wire, kTypeParams, {});
  }

  // STDIN: the request body, streamed straight from the caller's source
  // (typically the client socket) one chunk at a time — this call never
  // holds more than one chunk in memory regardless of how large the body is.
  for (;;) {
    ByteBuffer chunk = next_body_chunk();
    if (should_abort && should_abort()) { res.error = "request aborted: the body was refused"; return res; }
    write_stream_chunk(wire, kTypeStdin, ByteSpan(chunk.data(), chunk.size()));
    if (!conn->send_all(ByteSpan(wire.data(), wire.size()))) { res.error = "write to php-fpm failed"; return res; }
    wire.clear();
    if (chunk.empty()) break;  // that was the terminating record
  }

  // STDOUT/STDERR/END_REQUEST: relay STDOUT to the caller as records arrive.
  uint8_t head[8];
  ByteBuffer body;
  uint8_t pad_buf[256];
  for (;;) {
    if (!conn->recv_all(MutableByteSpan(head, 8), timeout_ms)) {
      res.error = conn->alive() ? "timed out waiting on php-fpm" : "php-fpm closed the connection early";
      return res;
    }
    uint8_t type = head[1];
    uint16_t content_len = uint16_t(head[4]) << 8 | head[5];
    uint8_t padding_len = head[6];
    body.resize(content_len);
    if (content_len > 0 && !conn->recv_all(MutableByteSpan(body.data(), content_len), timeout_ms)) {
      res.error = "php-fpm closed mid-record";
      return res;
    }
    if (padding_len > 0) {
      if (!conn->recv_all(MutableByteSpan(pad_buf, padding_len), timeout_ms)) { res.error = "php-fpm closed mid-padding"; return res; }
    }
    if (type == kTypeStdout) {
      if (!body.empty() && on_output) on_output(ByteSpan(body.data(), body.size()));
    } else if (type == kTypeStderr) {
      // Surfaced by the caller's logging, not treated as a protocol error —
      // PHP notices/warnings land here and shouldn't abort a working response.
    } else if (type == kTypeEndRequest) {
      if (body.size() >= 8) {
        res.app_status = uint32_t(body[0]) << 24 | uint32_t(body[1]) << 16 | uint32_t(body[2]) << 8 | uint32_t(body[3]);
      }
      res.ok = true;
      break;
    }
  }
  conn->close();
  return res;
}

}  // namespace aios::net
