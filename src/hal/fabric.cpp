#include "aios/hal/fabric.hpp"
#include <cstring>

namespace aios::hal {

std::string mac_str(const Mac& m) { static const char* H = "0123456789abcdef"; std::string s; for (int i = 0; i < 6; ++i) { if (i) s += ':'; s += H[m[i] >> 4]; s += H[m[i] & 15]; } return s; }
bool parse_mac(const std::string& s, Mac& out) {
  if (s.size() != 17) return false;
  for (int i = 0; i < 6; ++i) {
    auto hex = [](char c) -> int { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; if (c >= 'A' && c <= 'F') return c - 'A' + 10; return -1; };
    int a = hex(s[3 * i]), b = hex(s[3 * i + 1]); if (a < 0 || b < 0) return false; if (i < 5 && s[3 * i + 2] != ':') return false;
    out[i] = (uint8_t)(a * 16 + b);
  }
  return true;
}

namespace {
constexpr uint16_t ETHERTYPE = 0x88B5;
constexpr uint8_t F_SYN = 1, F_ACK = 2, F_FIN = 4, F_DATA = 8, F_RST = 16;
constexpr size_t HDR = 14 + 20;                 // ethernet + fabric header
constexpr size_t MAX_PAYLOAD = 1400;
constexpr uint64_t RETRANSMIT_NS = 60ull * 1000000, GIVE_UP_NS = 8ull * 1000000000;
const Mac BROADCAST = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
inline void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
inline void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
inline uint16_t get16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
inline uint32_t get32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
}  // namespace

struct Fabric::Session {
  uint32_t id = 0; uint16_t port = 0; Mac peer{};
  bool established = false, closed = false, initiator = false;
  uint32_t send_seq = 0;            // next byte offset to send
  uint32_t recv_seq = 0;            // next byte offset expected
  uint32_t acked = 0;               // peer has acknowledged up to here
  std::deque<uint8_t> rx;
  std::string peer_name;
};

class FabricConnection final : public Connection {
 public:
  FabricConnection(Fabric& f, std::shared_ptr<Fabric::Session> s) : f_(f), s_(std::move(s)) { name_ = mac_str(s_->peer) + "#" + std::to_string(s_->id); }
  ~FabricConnection() override { close(); }
  bool send_all(ByteSpan data) override {
    size_t off = 0;
    while (off < data.size()) {
      if (s_->closed) return false;
      size_t n = data.size() - off < MAX_PAYLOAD ? data.size() - off : MAX_PAYLOAD;
      uint32_t seq = s_->send_seq;
      uint64_t start = f_.now_ns(), last = 0;
      while (s_->acked != seq + n) {
        uint64_t now = f_.now_ns();
        if (now - last >= RETRANSMIT_NS) { if (last) f_.stats_.retransmits++; f_.send_frame(s_->peer, F_DATA, s_->id, s_->port, seq, s_->recv_seq, data.data() + off, n); last = now; }
        f_.pump();
        if (s_->closed) return false;
        if (now - start > GIVE_UP_NS) { s_->closed = true; return false; }
      }
      s_->send_seq = seq + (uint32_t)n;
      off += n;
    }
    return true;
  }
  // All-or-nothing: bytes leave the receive queue only when the whole request
  // can be satisfied, so a zero-timeout poll never tears a frame.
  bool recv_all(MutableByteSpan out, int timeout_ms) override {
    uint64_t start = f_.now_ns();
    for (;;) {
      if (s_->rx.size() >= out.size()) { for (size_t i = 0; i < out.size(); ++i) { out[i] = s_->rx.front(); s_->rx.pop_front(); } return true; }
      if (s_->closed) return false;
      f_.pump();
      if (s_->rx.size() >= out.size()) continue;
      if (timeout_ms >= 0 && f_.now_ns() - start >= (uint64_t)timeout_ms * 1000000ull) return false;
    }
  }
  bool alive() const override { return s_ && !s_->closed; }
  size_t pending() const override { const_cast<Fabric&>(f_).pump(); return s_->rx.size(); }
  std::string peer() const override { return name_; }
  void close() override { if (s_ && !s_->closed) { f_.send_frame(s_->peer, F_FIN, s_->id, s_->port, s_->send_seq, s_->recv_seq, nullptr, 0); s_->closed = true; f_.close(s_->id); } }
 private:
  Fabric& f_; std::shared_ptr<Fabric::Session> s_; std::string name_;
};

class FabricListener final : public Listener {
 public:
  FabricListener(Fabric& f, uint16_t port) : f_(f), port_(port) {}
  ~FabricListener() override { close(); }
  Result<std::unique_ptr<Connection>> accept(int timeout_ms) override {
    uint64_t start = f_.now_ns();
    for (;;) {
      f_.pump();
      auto& q = f_.pending_[port_];
      if (!q.empty()) { auto s = q.front(); q.pop_front(); return std::unique_ptr<Connection>(new FabricConnection(f_, s)); }
      if (f_.now_ns() - start >= (uint64_t)timeout_ms * 1000000ull) return Result<std::unique_ptr<Connection>>(Status::Busy);
    }
  }
  uint16_t port() const override { return port_; }
  void close() override { for (auto it = f_.listening_.begin(); it != f_.listening_.end(); ++it) if (*it == port_) { f_.listening_.erase(it); break; } }
 private:
  Fabric& f_; uint16_t port_;
};

Fabric::Fabric(FrameDevice& dev, uint64_t (*now_ns)()) : dev_(dev), now_(now_ns) {}

bool Fabric::send_frame(const Mac& dst, uint8_t flags, uint32_t session, uint16_t port, uint32_t seq, uint32_t ack, const uint8_t* payload, size_t len) {
  uint8_t frame[HDR + MAX_PAYLOAD];
  std::memcpy(frame, dst.data(), 6); std::memcpy(frame + 6, dev_.mac().data(), 6); put16(frame + 12, ETHERTYPE);
  uint8_t* h = frame + 14;
  h[0] = 'A'; h[1] = 'F'; h[2] = 1; h[3] = flags; put32(h + 4, session); put16(h + 8, port); put32(h + 10, seq); put32(h + 14, ack); put16(h + 18, (uint16_t)len);
  if (len) std::memcpy(frame + HDR, payload, len);
  size_t total = HDR + len; if (total < 60) { std::memset(frame + total, 0, 60 - total); total = 60; }
  stats_.frames_out++;
  bool ok = dev_.send(frame, total);
  note(std::string(ok ? "out: " : "OUT FAILED: ") + "flags " + std::to_string(flags) + " seq " + std::to_string(seq) + " ack " + std::to_string(ack) + " len " + std::to_string(len));
  return ok;
}

void Fabric::pump() {
  uint8_t frame[2048];
  for (int budget = 0; budget < 64; ++budget) {
    int n = dev_.recv(frame, sizeof frame);
    if (n <= 0) break;
    if ((size_t)n < HDR || get16(frame + 12) != ETHERTYPE) continue;
    const uint8_t* h = frame + 14;
    if (h[0] != 'A' || h[1] != 'F' || h[2] != 1) continue;
    stats_.frames_in++;
    uint8_t flags = h[3]; uint32_t sid = get32(h + 4); uint16_t port = get16(h + 8); uint32_t seq = get32(h + 10), ack = get32(h + 14); uint16_t len = get16(h + 18);
    if ((size_t)n < HDR + len) continue;
    Mac src; std::memcpy(src.data(), frame + 6, 6);
    auto it = sessions_.find(sid);
    if ((flags & F_SYN) && !(flags & F_ACK)) {
      // A connection request to one of our listening ports.
      bool listening = false; for (auto p : listening_) if (p == port) listening = true;
      if (!listening) { send_frame(src, F_RST, sid, port, 0, 0, nullptr, 0); continue; }
      if (it == sessions_.end()) {
        auto s = std::make_shared<Session>(); s->id = sid; s->port = port; s->peer = src; s->established = true; s->initiator = false;
        sessions_[sid] = s; pending_[port].push_back(s); stats_.sessions++;
      }
      send_frame(src, F_SYN | F_ACK, sid, port, 0, 0, nullptr, 0);
      continue;
    }
    if (it == sessions_.end()) { note("drop: unknown session " + std::to_string(sid) + " flags " + std::to_string(flags)); if (!(flags & F_RST)) send_frame(src, F_RST, sid, port, 0, 0, nullptr, 0); continue; }
    auto& s = it->second;
    note("in: flags " + std::to_string(flags) + " seq " + std::to_string(seq) + " ack " + std::to_string(ack) + " len " + std::to_string(len) + " (recv_seq " + std::to_string(s->recv_seq) + ", acked " + std::to_string(s->acked) + ")");
    if (flags & F_RST) { s->closed = true; continue; }
    if ((flags & F_SYN) && (flags & F_ACK)) { if (!s->established) { s->established = true; s->peer = src; } continue; }
    if (flags & F_FIN) { s->closed = true; continue; }
    if (flags & F_DATA) {
      if (seq == s->recv_seq) { s->rx.insert(s->rx.end(), frame + HDR, frame + HDR + len); s->recv_seq += len; }
      // Acknowledge what we hold (also re-acks duplicates).
      send_frame(s->peer, F_ACK, sid, port, 0, s->recv_seq, nullptr, 0);
      continue;
    }
    if (flags & F_ACK) { if (ack > s->acked) s->acked = ack; if (ack == s->send_seq) {} }
  }
}

Result<std::unique_ptr<Listener>> Fabric::listen(uint16_t port) {
  for (auto p : listening_) if (p == port) return Result<std::unique_ptr<Listener>>(Status::Busy, "port already listening");
  listening_.push_back(port);
  return std::unique_ptr<Listener>(new FabricListener(*this, port));
}

Result<std::unique_ptr<Connection>> Fabric::connect(const Mac& peer, bool broadcast, uint16_t port, int timeout_ms) {
  auto s = std::make_shared<Session>();
  uint8_t rnd[4]; uint64_t t = now_(); for (int i = 0; i < 4; ++i) rnd[i] = (uint8_t)(t >> (8 * i + 3));
  s->id = (next_session_++ << 8) ^ get32(rnd) ^ (uint32_t)dev_.mac()[5]; s->port = port; s->peer = broadcast ? BROADCAST : peer; s->initiator = true;
  sessions_[s->id] = s; stats_.sessions++;
  uint64_t start = now_(), last = 0;
  while (!s->established) {
    uint64_t now = now_();
    if (now - last >= 200ull * 1000000) { send_frame(broadcast ? BROADCAST : peer, F_SYN, s->id, port, 0, 0, nullptr, 0); last = now; }
    pump();
    if (s->closed) { sessions_.erase(s->id); return Result<std::unique_ptr<Connection>>(Status::Denied, "peer refused (no listener on that port)"); }
    if (now - start > (uint64_t)timeout_ms * 1000000ull) { sessions_.erase(s->id); return Result<std::unique_ptr<Connection>>(Status::Busy, "no fabric peer answered on port " + std::to_string(port)); }
  }
  return std::unique_ptr<Connection>(new FabricConnection(*this, s));
}

void Fabric::close(uint32_t session) { sessions_.erase(session); }

std::string Fabric::describe_sessions() const {
  std::string out;
  for (auto& e : log_) out += "  · " + e + "\n";
  for (auto& [id, s] : sessions_) out += "  session " + std::to_string(id) + " peer " + mac_str(s->peer) + " port " + std::to_string(s->port) + (s->initiator ? " (initiator)" : " (accepted)") + (s->established ? "" : " connecting") + (s->closed ? " CLOSED" : "") + " sent " + std::to_string(s->send_seq) + " acked " + std::to_string(s->acked) + " received " + std::to_string(s->recv_seq) + " queued " + std::to_string(s->rx.size()) + "\n";
  return out;
}

}  // namespace aios::hal
