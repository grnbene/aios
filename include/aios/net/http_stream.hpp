#pragma once
// A minimal HTTP/1.1 server-side reader/writer built around one rule: nothing
// here ever requires the full request or response body to exist in memory at
// once. That is the actual bug being fixed (nginx's default fastcgi/proxy
// buffering does hold a full body before it moves on, and that's what turns
// a large base64 payload or a slow upstream into a stalled or dropped
// connection). Everything below reads/writes in bounded chunks.
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "aios/core/bytes.hpp"
#include "aios/core/result.hpp"
#include "aios/hal/net.hpp"

namespace aios::net {

struct HttpHeader { std::string name, value; };

struct HttpRequestHead {
  std::string method;
  std::string target;    // raw request-target, including any query string
  std::string version;   // "HTTP/1.1" etc
  std::vector<HttpHeader> headers;
  std::string host;
  int64_t content_length = -1;  // -1 = absent
  bool chunked = false;
  bool keep_alive = true;

  std::optional<std::string> header(const std::string& name) const;
};

// Reads leftover socket bytes before hitting the wire again, so a single
// recv() that lands past the header/body boundary (very common — clients
// pipeline the headers and the first body bytes in one write()) never loses
// anything.
class ConnReader {
 public:
  explicit ConnReader(hal::Connection& conn) : conn_(conn) {}
  // Up to max_len bytes, waiting up to timeout_ms for at least one. Empty
  // return + conn.alive()==false means the peer closed; empty + alive==true
  // means the timeout elapsed with nothing to read.
  ByteBuffer read_some(size_t max_len, int timeout_ms);
  void unread(ByteBuffer bytes);  // push back onto the front of the buffer
  hal::Connection& conn() { return conn_; }

 private:
  hal::Connection& conn_;
  ByteBuffer buf_;
  size_t pos_ = 0;
};

// Reads the request line + headers (bounded to 64KiB — a header block past
// that is almost certainly not a browser or a real API client). Any body
// bytes that arrived in the same read are retained in `reader` for the body
// reader below to pick up first.
Result<HttpRequestHead> read_request_head(ConnReader& reader, int timeout_ms);

// Streams the body in bounded chunks regardless of framing (Content-Length or
// chunked Transfer-Encoding). next() returns an empty buffer at end of body;
// conn.alive()==false + non-empty return never happens (a fault mid-body
// surfaces as ok()==false).
class RequestBodyReader {
 public:
  RequestBodyReader(ConnReader& reader, const HttpRequestHead& head);
  Result<ByteBuffer> next(size_t max_chunk, int timeout_ms);
  bool done() const { return done_; }

 private:
  ConnReader& reader_;
  bool chunked_;
  int64_t remaining_;      // for Content-Length framing; -1 = read until EOF (no framing given)
  int64_t chunk_remaining_ = 0;  // for chunked framing: bytes left in the current chunk
  bool done_ = false;
};

struct HttpResponseHead {
  int status = 200;
  std::string status_text = "OK";
  std::vector<HttpHeader> headers;
};

// Writes the status line + headers. If content_length is given, the body is
// written with write_body_chunk() up to exactly that many bytes and the
// connection may be kept alive after. If content_length is absent, the
// response is sent chunked (Transfer-Encoding: chunked is added here) so the
// body can still be streamed without knowing its length up front — this is
// what lets a PHP response start reaching the client before PHP has finished
// generating it, instead of nginx's default of buffering the whole thing.
class ResponseWriter {
 public:
  ResponseWriter(hal::Connection& conn, HttpResponseHead head, std::optional<int64_t> content_length);
  bool write_head();
  bool write_body_chunk(ByteSpan data);
  bool finish();  // writes the terminating chunk if chunked; no-op otherwise
  bool chunked() const { return chunked_; }
  // For a HEAD request: headers only, body bytes are dropped. 1xx/204/304
  // are set this way automatically — any body byte after them is read by
  // the client as the start of the next response on a keep-alive connection.
  void suppress_body() { no_body_ = true; }
  static bool status_has_no_body(int status) { return (status >= 100 && status < 200) || status == 204 || status == 304; }

 private:
  hal::Connection& conn_;
  HttpResponseHead head_;
  bool chunked_;
  int64_t remaining_ = -1;
  bool no_body_ = false;
};

}  // namespace aios::net
