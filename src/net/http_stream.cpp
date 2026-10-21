#include "aios/net/http_stream.hpp"

#include <algorithm>
#include "aios/core/strbuf.hpp"
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace aios::net {
namespace {

bool ieq(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
  return true;
}
std::string trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t");
  size_t e = s.find_last_not_of(" \t");
  if (b == std::string::npos) return "";
  return s.substr(b, e - b + 1);
}
bool icontains(const std::string& hay, const std::string& needle) {
  auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                         [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
  return it != hay.end();
}

// Reads a single \r\n-terminated line, keeping any bytes read past it in the
// reader for whatever comes next (chunk data, the next line, ...).
Result<std::string> read_line(ConnReader& reader, size_t max_len, int timeout_ms) {
  std::string acc;
  for (;;) {
    auto chunk = reader.read_some(256, timeout_ms);
    if (chunk.empty()) {
      if (!reader.conn().alive()) return Result<std::string>(Status::IoError, "connection closed mid-line");
      return Result<std::string>(Status::Busy, "timed out reading a line");
    }
    acc.append(chunk.begin(), chunk.end());
    auto pos = acc.find("\r\n");
    if (pos != std::string::npos) {
      if (pos + 2 < acc.size()) reader.unread(ByteBuffer(acc.begin() + pos + 2, acc.end()));
      return acc.substr(0, pos);
    }
    if (acc.size() > max_len) return Result<std::string>(Status::Invalid, "line too long");
  }
}

}  // namespace

std::optional<std::string> HttpRequestHead::header(const std::string& name) const {
  for (auto& h : headers)
    if (ieq(h.name, name)) return h.value;
  return std::nullopt;
}

ByteBuffer ConnReader::read_some(size_t max_len, int timeout_ms) {
  if (pos_ < buf_.size()) {
    size_t n = std::min(max_len, buf_.size() - pos_);
    ByteBuffer out(buf_.begin() + pos_, buf_.begin() + pos_ + n);
    pos_ += n;
    if (pos_ == buf_.size()) { buf_.clear(); pos_ = 0; }
    return out;
  }
  ByteBuffer tmp(max_len);
  long n = conn_.recv_some(MutableByteSpan(tmp.data(), tmp.size()), timeout_ms);
  if (n <= 0) return {};
  tmp.resize(size_t(n));
  return tmp;
}

void ConnReader::unread(ByteBuffer bytes) {
  if (bytes.empty()) return;
  if (pos_ > 0) { buf_.erase(buf_.begin(), buf_.begin() + pos_); pos_ = 0; }
  buf_.insert(buf_.begin(), bytes.begin(), bytes.end());
}

Result<HttpRequestHead> read_request_head(ConnReader& reader, int timeout_ms) {
  constexpr size_t kMaxHeaderBlock = 64 * 1024;
  std::string acc;
  for (;;) {
    if (acc.size() > kMaxHeaderBlock) return Result<HttpRequestHead>(Status::Invalid, "request header block too large");
    auto chunk = reader.read_some(4096, timeout_ms);
    if (chunk.empty()) {
      if (!reader.conn().alive())
        return Result<HttpRequestHead>(acc.empty() ? Status::NotFound : Status::IoError,
                                        acc.empty() ? "peer closed before sending a request" : "connection closed mid-headers");
      return Result<HttpRequestHead>(Status::Busy, "timed out reading request headers");
    }
    acc.append(chunk.begin(), chunk.end());
    auto pos = acc.find("\r\n\r\n");
    if (pos == std::string::npos) continue;
    size_t header_end = pos + 4;
    if (header_end < acc.size()) reader.unread(ByteBuffer(acc.begin() + header_end, acc.end()));
    acc.resize(pos);
    break;
  }

  HttpRequestHead head;
  size_t line_start = 0;
  bool first = true;
  while (line_start <= acc.size()) {
    auto nl = acc.find("\r\n", line_start);
    std::string line = acc.substr(line_start, nl == std::string::npos ? std::string::npos : nl - line_start);
    if (first) {
      first = false;
      size_t sp1 = line.find(' ');
      size_t sp2 = sp1 == std::string::npos ? std::string::npos : line.find(' ', sp1 + 1);
      if (sp1 == std::string::npos || sp2 == std::string::npos)
        return Result<HttpRequestHead>(Status::Invalid, "malformed request line");
      head.method = line.substr(0, sp1);
      head.target = line.substr(sp1 + 1, sp2 - sp1 - 1);
      head.version = line.substr(sp2 + 1);
    } else if (!line.empty()) {
      auto colon = line.find(':');
      if (colon != std::string::npos) head.headers.push_back({trim(line.substr(0, colon)), trim(line.substr(colon + 1))});
    }
    if (nl == std::string::npos) break;
    line_start = nl + 2;
  }

  if (auto h = head.header("Host")) head.host = *h;
  if (auto h = head.header("Content-Length")) head.content_length = aios::to_i64(h->c_str());
  if (auto h = head.header("Transfer-Encoding")) head.chunked = icontains(*h, "chunked");
  head.keep_alive = head.version == "HTTP/1.1";
  if (auto h = head.header("Connection")) {
    if (icontains(*h, "close")) head.keep_alive = false;
    else if (icontains(*h, "keep-alive")) head.keep_alive = true;
  }
  return head;
}

RequestBodyReader::RequestBodyReader(ConnReader& reader, const HttpRequestHead& head)
    : reader_(reader), chunked_(head.chunked), remaining_(head.chunked ? 0 : head.content_length) {
  if (!chunked_ && remaining_ <= 0) done_ = true;
}

Result<ByteBuffer> RequestBodyReader::next(size_t max_chunk, int timeout_ms) {
  if (done_) return ByteBuffer{};
  if (!chunked_) {
    size_t want = std::min<size_t>(max_chunk, size_t(remaining_));
    auto data = reader_.read_some(want, timeout_ms);
    if (data.empty()) {
      if (!reader_.conn().alive()) return Result<ByteBuffer>(Status::IoError, "connection closed mid-body");
      return Result<ByteBuffer>(Status::Busy, "timed out reading request body");
    }
    remaining_ -= int64_t(data.size());
    if (remaining_ <= 0) done_ = true;
    return data;
  }
  if (chunk_remaining_ == 0) {
    auto line = read_line(reader_, 64, timeout_ms);
    if (!line.ok()) return Result<ByteBuffer>(line.error());
    size_t semi = line->find(';');
    std::string size_hex = semi == std::string::npos ? *line : line->substr(0, semi);
    long long size = aios::to_i64(size_hex.c_str(), nullptr, 16);
    if (size < 0) return Result<ByteBuffer>(Status::Invalid, "malformed chunk size");
    if (size == 0) {
      // Trailer headers (if any), terminated by a blank line — discarded.
      for (;;) {
        auto trailer = read_line(reader_, 4096, timeout_ms);
        if (!trailer.ok()) return Result<ByteBuffer>(trailer.error());
        if (trailer->empty()) break;
      }
      done_ = true;
      return ByteBuffer{};
    }
    chunk_remaining_ = size;
  }
  size_t want = std::min<size_t>(max_chunk, size_t(chunk_remaining_));
  auto data = reader_.read_some(want, timeout_ms);
  if (data.empty()) {
    if (!reader_.conn().alive()) return Result<ByteBuffer>(Status::IoError, "connection closed mid-chunk");
    return Result<ByteBuffer>(Status::Busy, "timed out reading chunk data");
  }
  chunk_remaining_ -= int64_t(data.size());
  if (chunk_remaining_ == 0) {
    auto crlf = read_line(reader_, 2, timeout_ms);  // consumes the CRLF after chunk data
    if (!crlf.ok()) return Result<ByteBuffer>(crlf.error());
  }
  return data;
}

ResponseWriter::ResponseWriter(hal::Connection& conn, HttpResponseHead head, std::optional<int64_t> content_length)
    : conn_(conn), head_(std::move(head)), chunked_(!content_length.has_value()), remaining_(content_length.value_or(-1)),
      no_body_(status_has_no_body(head_.status)) {}

bool ResponseWriter::write_head() {
  char status_line[64];
  std::snprintf(status_line, sizeof(status_line), "HTTP/1.1 %d ", head_.status);
  std::string out = status_line;
  out += head_.status_text;
  out += "\r\n";
  for (auto& h : head_.headers) out += h.name + ": " + h.value + "\r\n";
  if (status_has_no_body(head_.status)) {
    // RFC 9110: no Content-Length or Transfer-Encoding on 1xx/204.
  } else if (chunked_) {
    out += "Transfer-Encoding: chunked\r\n";
  } else {
    out += "Content-Length: " + std::to_string(remaining_) + "\r\n";
  }
  out += "\r\n";
  return conn_.send_all(span_of(out));
}

bool ResponseWriter::write_body_chunk(ByteSpan data) {
  if (data.empty() || no_body_) return true;
  if (!chunked_) return conn_.send_all(data);
  char hdr[32];
  int n = std::snprintf(hdr, sizeof(hdr), "%zx\r\n", data.size());
  ByteBuffer wire;
  wire.reserve((size_t)n + data.size() + 2);
  wire.insert(wire.end(), hdr, hdr + n);
  wire.insert(wire.end(), data.begin(), data.end());
  wire.push_back('\r'); wire.push_back('\n');
  return conn_.send_all(ByteSpan(wire.data(), wire.size()));
}

bool ResponseWriter::finish() {
  if (!chunked_ || no_body_) return true;
  static const char kEnd[] = "0\r\n\r\n";
  return conn_.send_all(ByteSpan(reinterpret_cast<const uint8_t*>(kEnd), sizeof(kEnd) - 1));
}

}  // namespace aios::net
