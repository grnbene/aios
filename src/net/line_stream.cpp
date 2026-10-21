// See include/aios/net/line_stream.hpp.
#include "aios/net/line_stream.hpp"

#include <cstring>

namespace aios::net {

LineStream::LineStream(hal::Host& host, std::string kernel_root, int timeout_ms, std::string trust_store)
    : host_(host), kernel_root_(std::move(kernel_root)), trust_store_(std::move(trust_store)), timeout_ms_(timeout_ms) {}

LineStream::~LineStream() { close(); }

void LineStream::close() {
  if (tls_) tls_->close();
  if (plain_) plain_->close();
  tls_.reset();
  plain_.reset();
  buf_.clear();
  pos_ = 0;
  eof_ = false;
}

Result<void> LineStream::connect(const std::string& host, uint16_t port, Security security) {
  close();
  host_name_ = host;
  port_ = port;
  const std::string store = trust_store_.empty() ? default_trust_store(host_, kernel_root_) : trust_store_;
  if (security == Security::Tls) {
    tls_.reset(new TlsClient(host_, store));
    auto r = tls_->connect(host, port, timeout_ms_);
    if (!r.ok()) { tls_.reset(); return r; }
    return Ok();
  }
  auto c = hal::net_connect(host, port, timeout_ms_);
  if (!c.ok()) return Result<void>(c.error());
  plain_ = std::move(*c);
  return Ok();
}

Result<void> LineStream::start_tls() {
  if (tls_) return Ok();
  if (!plain_) return Result<void>(Status::Invalid, "STARTTLS with no open connection");
  if (pos_ != buf_.size())
    return Result<void>(Status::Corrupt, "the server sent data after agreeing to STARTTLS and before the handshake");
  buf_.clear();
  pos_ = 0;
  const std::string store = trust_store_.empty() ? default_trust_store(host_, kernel_root_) : trust_store_;
  tls_.reset(new TlsClient(host_, store));
  auto r = tls_->connect_over(std::move(plain_), host_name_, port_, timeout_ms_);
  plain_.reset();
  if (!r.ok()) { tls_.reset(); return r; }
  return Ok();
}

Result<void> LineStream::write(std::string_view data) {
  if (tls_) return tls_->write(ByteSpan(reinterpret_cast<const uint8_t*>(data.data()), data.size()));
  if (!plain_) return Result<void>(Status::IoError, "not connected");
  if (!plain_->send_all(ByteSpan(reinterpret_cast<const uint8_t*>(data.data()), data.size())))
    return Result<void>(Status::IoError, "the connection closed while writing");
  return Ok();
}

Result<void> LineStream::fill() {
  if (eof_) return Result<void>(Status::IoError, "the server closed the connection");
  // Drop what has been consumed once it is a good part of the buffer.
  if (pos_ > 65536 && pos_ * 2 > buf_.size()) { buf_.erase(0, pos_); pos_ = 0; }
  if (tls_) {
    auto chunk = tls_->read(timeout_ms_);
    if (!chunk.ok()) return Result<void>(chunk.error());
    if (chunk->empty()) { eof_ = true; return Result<void>(Status::IoError, "the server closed the connection"); }
    buf_.append(reinterpret_cast<const char*>(chunk->data()), chunk->size());
    return Ok();
  }
  if (!plain_) return Result<void>(Status::IoError, "not connected");
  uint8_t tmp[16384];
  long n = plain_->recv_some(MutableByteSpan(tmp, sizeof tmp), timeout_ms_);
  if (n > 0) { buf_.append(reinterpret_cast<const char*>(tmp), static_cast<size_t>(n)); return Ok(); }
  if (n == 0 && plain_->alive()) return Result<void>(Status::Busy, "timed out waiting for the server");
  eof_ = true;
  return Result<void>(Status::IoError, "the server closed the connection");
}

Result<std::string> LineStream::read_line(size_t max_line) {
  size_t scan = pos_;
  for (;;) {
    size_t nl = buf_.find('\n', scan);
    if (nl != std::string::npos) {
      size_t end = nl;
      if (end > pos_ && buf_[end - 1] == '\r') --end;
      std::string line = buf_.substr(pos_, end - pos_);
      pos_ = nl + 1;
      return line;
    }
    if (buf_.size() - pos_ > max_line) return Result<std::string>(Status::Exhausted, "a line longer than the limit");
    scan = buf_.size();
    auto r = fill();
    if (!r.ok()) return Result<std::string>(r.error());
  }
}

Result<std::string> LineStream::read_exact(size_t n) {
  while (buf_.size() - pos_ < n) {
    auto r = fill();
    if (!r.ok()) return Result<std::string>(r.error());
  }
  std::string out = buf_.substr(pos_, n);
  pos_ += n;
  return out;
}

}  // namespace aios::net
