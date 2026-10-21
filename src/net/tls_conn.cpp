#include "aios/net/tls_conn.hpp"

#include <algorithm>

namespace aios::net {

TlsServerConnection::TlsServerConnection(std::unique_ptr<TlsServer> server, std::string peer_addr)
    : server_(std::move(server)), peer_(std::move(peer_addr)) {}

bool TlsServerConnection::send_all(ByteSpan data) {
  if (closed_) return false;
  auto r = server_->write(data);
  if (!r.ok()) { closed_ = true; return false; }
  return true;
}

bool TlsServerConnection::recv_all(MutableByteSpan data, int timeout_ms) {
  size_t off = 0;
  while (off < data.size()) {
    if (pos_ < buf_.size()) {
      size_t n = std::min(data.size() - off, buf_.size() - pos_);
      std::copy(buf_.begin() + pos_, buf_.begin() + pos_ + n, data.begin() + off);
      pos_ += n; off += n;
      if (pos_ == buf_.size()) { buf_.clear(); pos_ = 0; }
      continue;
    }
    if (closed_) return false;
    auto r = server_->read(timeout_ms);
    if (!r.ok()) { if (r.error().status != Status::Busy) closed_ = true; return false; }
    if (r->empty()) { closed_ = true; return false; }
    buf_ = std::move(*r); pos_ = 0;
  }
  return true;
}

long TlsServerConnection::recv_some(MutableByteSpan data, int timeout_ms) {
  if (data.size() == 0) return 0;
  if (pos_ < buf_.size()) {
    size_t n = std::min(data.size(), buf_.size() - pos_);
    std::copy(buf_.begin() + pos_, buf_.begin() + pos_ + n, data.begin());
    pos_ += n;
    if (pos_ == buf_.size()) { buf_.clear(); pos_ = 0; }
    return long(n);
  }
  if (closed_) return -1;
  auto r = server_->read(timeout_ms);
  if (!r.ok()) {
    if (r.error().status == Status::Busy) return 0;  // nothing within timeout, still alive
    closed_ = true;
    return -1;
  }
  if (r->empty()) { closed_ = true; return 0; }
  buf_ = std::move(*r); pos_ = 0;
  size_t n = std::min(data.size(), buf_.size());
  std::copy(buf_.begin(), buf_.begin() + n, data.begin());
  pos_ = n;
  if (pos_ == buf_.size()) { buf_.clear(); pos_ = 0; }
  return long(n);
}

void TlsServerConnection::close() {
  if (closed_) return;
  server_->close();
  closed_ = true;
}

TlsClientConnection::TlsClientConnection(std::unique_ptr<TlsClient> client, std::string peer_addr)
    : client_(std::move(client)), peer_(std::move(peer_addr)) {}

bool TlsClientConnection::send_all(ByteSpan data) {
  if (closed_) return false;
  auto r = client_->write(data);
  if (!r.ok()) { closed_ = true; return false; }
  return true;
}

bool TlsClientConnection::recv_all(MutableByteSpan data, int timeout_ms) {
  size_t off = 0;
  while (off < data.size()) {
    if (pos_ < buf_.size()) {
      size_t n = std::min(data.size() - off, buf_.size() - pos_);
      std::copy(buf_.begin() + pos_, buf_.begin() + pos_ + n, data.begin() + off);
      pos_ += n; off += n;
      if (pos_ == buf_.size()) { buf_.clear(); pos_ = 0; }
      continue;
    }
    if (closed_) return false;
    auto r = client_->read(timeout_ms);
    if (!r.ok()) { if (r.error().status != Status::Busy) closed_ = true; return false; }
    if (r->empty()) { closed_ = true; return false; }
    buf_ = std::move(*r); pos_ = 0;
  }
  return true;
}

long TlsClientConnection::recv_some(MutableByteSpan data, int timeout_ms) {
  if (data.size() == 0) return 0;
  if (pos_ < buf_.size()) {
    size_t n = std::min(data.size(), buf_.size() - pos_);
    std::copy(buf_.begin() + pos_, buf_.begin() + pos_ + n, data.begin());
    pos_ += n;
    if (pos_ == buf_.size()) { buf_.clear(); pos_ = 0; }
    return long(n);
  }
  if (closed_) return -1;
  auto r = client_->read(timeout_ms);
  if (!r.ok()) {
    if (r.error().status == Status::Busy) return 0;
    closed_ = true;
    return -1;
  }
  if (r->empty()) { closed_ = true; return 0; }
  buf_ = std::move(*r); pos_ = 0;
  size_t n = std::min(data.size(), buf_.size());
  std::copy(buf_.begin(), buf_.begin() + n, data.begin());
  pos_ = n;
  if (pos_ == buf_.size()) { buf_.clear(); pos_ = 0; }
  return long(n);
}

void TlsClientConnection::close() {
  if (closed_) return;
  client_->close();
  closed_ = true;
}

}  // namespace aios::net
