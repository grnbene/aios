#include "aios/net/tls_sni.hpp"

#include <algorithm>

namespace aios::net {
namespace {

class ReplayConnection : public hal::Connection {
 public:
  ReplayConnection(std::unique_ptr<hal::Connection> inner, ByteBuffer prefix)
      : inner_(std::move(inner)), prefix_(std::move(prefix)) {}

  bool send_all(ByteSpan data) override { return inner_->send_all(data); }

  bool recv_all(MutableByteSpan data, int timeout_ms) override {
    size_t off = from_prefix(data);
    if (off == data.size()) return true;
    return inner_->recv_all(MutableByteSpan(data.data() + off, data.size() - off), timeout_ms);
  }

  long recv_some(MutableByteSpan data, int timeout_ms) override {
    if (pos_ < prefix_.size()) return long(from_prefix(data));
    return inner_->recv_some(data, timeout_ms);
  }

  std::string peer() const override { return inner_->peer(); }
  void close() override { inner_->close(); }
  bool alive() const override { return inner_->alive(); }
  size_t pending() const override {
    size_t left = prefix_.size() - pos_;
    if (left > 0) return left;
    return inner_->pending();
  }

 private:
  size_t from_prefix(MutableByteSpan data) {
    if (pos_ >= prefix_.size()) return 0;
    size_t n = std::min(data.size(), prefix_.size() - pos_);
    std::copy(prefix_.begin() + pos_, prefix_.begin() + pos_ + n, data.begin());
    pos_ += n;
    return n;
  }
  std::unique_ptr<hal::Connection> inner_;
  ByteBuffer prefix_;
  size_t pos_ = 0;
};

// Parses just far enough into a ClientHello handshake message (the TLS
// record body, handshake header included) to find extension 0x0000
// (server_name) and pull out the first hostname in it.
std::string parse_sni(ByteSpan body) {
  if (body.size() < 4 || body[0] != 0x01) return {};
  size_t hs_len = (size_t(body[1]) << 16) | (size_t(body[2]) << 8) | body[3];
  size_t pos = 4;
  size_t end = std::min(body.size(), pos + hs_len);

  auto need = [&](size_t n) { return pos + n <= end; };
  if (!need(2)) return {};
  pos += 2;  // client_version
  if (!need(32)) return {};
  pos += 32;  // random
  if (!need(1)) return {};
  uint8_t sess_len = body[pos]; pos += 1;
  if (!need(sess_len)) return {};
  pos += sess_len;
  if (!need(2)) return {};
  uint16_t cs_len = (uint16_t(body[pos]) << 8) | body[pos + 1]; pos += 2;
  if (!need(cs_len)) return {};
  pos += cs_len;
  if (!need(1)) return {};
  uint8_t comp_len = body[pos]; pos += 1;
  if (!need(comp_len)) return {};
  pos += comp_len;
  if (!need(2)) return {};  // no extensions present at all
  uint16_t ext_total = (uint16_t(body[pos]) << 8) | body[pos + 1]; pos += 2;
  size_t ext_end = std::min(end, pos + ext_total);

  while (pos + 4 <= ext_end) {
    uint16_t ext_type = (uint16_t(body[pos]) << 8) | body[pos + 1];
    uint16_t ext_len = (uint16_t(body[pos + 2]) << 8) | body[pos + 3];
    pos += 4;
    if (pos + ext_len > ext_end) break;
    if (ext_type == 0x0000) {
      size_t p = pos, list_end = pos + ext_len;
      if (p + 2 <= list_end) {
        uint16_t list_len = (uint16_t(body[p]) << 8) | body[p + 1]; p += 2;
        list_end = std::min(list_end, p + list_len);
        while (p + 3 <= list_end) {
          uint8_t name_type = body[p];
          uint16_t name_len = (uint16_t(body[p + 1]) << 8) | body[p + 2];
          p += 3;
          if (p + name_len > list_end) break;
          if (name_type == 0) return std::string(reinterpret_cast<const char*>(body.data() + p), name_len);
          p += name_len;
        }
      }
    }
    pos += ext_len;
  }
  return {};
}

}  // namespace

Result<SniPeekResult> peek_client_hello_sni(std::unique_ptr<hal::Connection> conn, int timeout_ms) {
  ByteBuffer header(5);
  if (!conn->recv_all(MutableByteSpan(header.data(), header.size()), timeout_ms))
    return Result<SniPeekResult>(Status::IoError, "failed to read TLS record header");
  if (header[0] != 0x16) return Result<SniPeekResult>(Status::Invalid, "first record is not a TLS handshake");
  size_t len = (size_t(header[3]) << 8) | header[4];
  if (len == 0 || len > 16384) return Result<SniPeekResult>(Status::Invalid, "implausible ClientHello record length");

  ByteBuffer body(len);
  if (!conn->recv_all(MutableByteSpan(body.data(), body.size()), timeout_ms))
    return Result<SniPeekResult>(Status::IoError, "failed to read ClientHello record body");

  std::string sni = parse_sni(ByteSpan(body.data(), body.size()));

  ByteBuffer prefix = header;
  prefix.insert(prefix.end(), body.begin(), body.end());

  SniPeekResult res;
  res.server_name = sni;
  res.conn = std::make_unique<ReplayConnection>(std::move(conn), std::move(prefix));
  return res;
}

}  // namespace aios::net
