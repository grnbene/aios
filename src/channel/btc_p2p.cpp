#include "aios/channel/btc_p2p.hpp"
#include <cstring>
#include "aios/crypto/sha256.hpp"
#include "aios/hal/sync.hpp"

namespace aios::channel {

using namespace aios::crypto;

namespace {

const int32_t PROTOCOL_VERSION = 70016;
const char* SUBVERSION = "/aios:0.1/";

void put_u16be(ByteBuffer& b, uint16_t v) {
  b.push_back((uint8_t)(v >> 8));
  b.push_back((uint8_t)v);
}

void put_u32(ByteBuffer& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8 * i)));
}

void put_u64(ByteBuffer& b, uint64_t v) {
  for (int i = 0; i < 8; ++i) b.push_back((uint8_t)(v >> (8 * i)));
}

void put_varint(ByteBuffer& b, uint64_t v) {
  if (v < 0xfd) {
    b.push_back((uint8_t)v);
  } else if (v <= 0xffff) {
    b.push_back(0xfd);
    b.push_back((uint8_t)v);
    b.push_back((uint8_t)(v >> 8));
  } else if (v <= 0xffffffffull) {
    b.push_back(0xfe);
    put_u32(b, (uint32_t)v);
  } else {
    b.push_back(0xff);
    put_u64(b, v);
  }
}

void put_varstr(ByteBuffer& b, const std::string& s) {
  put_varint(b, s.size());
  b.insert(b.end(), s.begin(), s.end());
}

void put_netaddr(ByteBuffer& b, uint64_t services, uint16_t port) {
  put_u64(b, services);
  for (int i = 0; i < 10; ++i) b.push_back(0);
  b.push_back(0xff);
  b.push_back(0xff);
  for (int i = 0; i < 4; ++i) b.push_back(0);
  put_u16be(b, port);
}

class Cursor {
 public:
  explicit Cursor(ByteSpan d) : data_(d) {}
  bool ok() const { return ok_; }
  size_t left() const { return ok_ ? data_.size() - pos_ : 0; }
  uint8_t u8() {
    if (left() < 1) { ok_ = false; return 0; }
    return data_[pos_++];
  }
  uint16_t u16() { return (uint16_t)((uint16_t)u8() | ((uint16_t)u8() << 8)); }
  uint32_t u32() {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= (uint32_t)u8() << (8 * i);
    return v;
  }
  uint64_t u64() {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)u8() << (8 * i);
    return v;
  }
  uint64_t varint() {
    uint8_t tag = u8();
    if (tag < 0xfd) return tag;
    if (tag == 0xfd) return u16();
    if (tag == 0xfe) return u32();
    return u64();
  }
  std::string varstr() {
    uint64_t n = varint();
    if (n > left()) { ok_ = false; return {}; }
    std::string s(reinterpret_cast<const char*>(data_.data() + pos_), (size_t)n);
    pos_ += (size_t)n;
    return s;
  }
  void skip(size_t n) {
    if (left() < n) { ok_ = false; return; }
    pos_ += n;
  }
  bool array32(std::array<uint8_t, 32>& out) {
    if (left() < 32) { ok_ = false; return false; }
    std::memcpy(out.data(), data_.data() + pos_, 32);
    pos_ += 32;
    return true;
  }

 private:
  ByteSpan data_;
  size_t pos_ = 0;
  bool ok_ = true;
};

// nBits is a compact float: an exponent byte and a 23-bit mantissa. The digest
// is compared against the 256-bit target it expands to, little-endian, which is
// why the comparison walks the bytes from the top down.
bool digest_meets_target(const std::array<uint8_t, 32>& digest, uint32_t bits) {
  uint32_t exponent = bits >> 24;
  uint32_t mantissa = bits & 0x007fffffu;
  if (mantissa == 0) return false;
  if (bits & 0x00800000u) return false;
  if (exponent > 34) return false;

  uint8_t target[32];
  std::memset(target, 0, 32);
  if (exponent <= 3) {
    uint32_t shifted = mantissa >> (8 * (3 - exponent));
    target[0] = (uint8_t)shifted;
    target[1] = (uint8_t)(shifted >> 8);
    target[2] = (uint8_t)(shifted >> 16);
  } else {
    size_t offset = exponent - 3;
    if (offset > 29) return false;
    target[offset] = (uint8_t)mantissa;
    if (offset + 1 < 32) target[offset + 1] = (uint8_t)(mantissa >> 8);
    if (offset + 2 < 32) target[offset + 2] = (uint8_t)(mantissa >> 16);
  }

  for (int i = 31; i >= 0; --i) {
    if (digest[(size_t)i] < target[i]) return true;
    if (digest[(size_t)i] > target[i]) return false;
  }
  return true;
}

}  // namespace

ByteBuffer BlockHeader::serialize() const {
  ByteBuffer b;
  put_u32(b, (uint32_t)version);
  b.insert(b.end(), prev_block.begin(), prev_block.end());
  b.insert(b.end(), merkle_root.begin(), merkle_root.end());
  put_u32(b, time);
  put_u32(b, bits);
  put_u32(b, nonce);
  return b;
}

std::array<uint8_t, 32> BlockHeader::hash() const {
  auto raw = serialize();
  return sha256d(ByteSpan(raw.data(), raw.size()));
}

bool BlockHeader::work_is_valid() const { return digest_meets_target(hash(), bits); }

BitcoinPeer::~BitcoinPeer() { close(); }

void BitcoinPeer::close() {
  if (conn_) {
    conn_->close();
    conn_.reset();
  }
}

Result<void> BitcoinPeer::connect(const std::string& host, uint16_t port, uint32_t magic, int timeout_ms) {
  close();
  auto c = hal::net_connect(host, port, timeout_ms);
  if (!c.ok()) return Result<void>(c.error());
  conn_ = std::move(*c);
  magic_ = magic;
  info_ = PeerInfo{};
  info_.address = conn_->peer();
  return Ok();
}

Result<void> BitcoinPeer::send(const std::string& command, ByteSpan payload) {
  if (!conn_) return Result<void>(Status::Invalid, "not connected");
  if (command.size() > 12) return Result<void>(Status::Invalid, "command name too long");

  ByteBuffer frame;
  put_u32(frame, magic_);
  for (size_t i = 0; i < 12; ++i) frame.push_back(i < command.size() ? (uint8_t)command[i] : 0);
  put_u32(frame, (uint32_t)payload.size());
  auto check = sha256d(payload);
  frame.insert(frame.end(), check.begin(), check.begin() + 4);
  frame.insert(frame.end(), payload.begin(), payload.end());

  if (!conn_->send_all(ByteSpan(frame.data(), frame.size()))) {
    return Result<void>(Status::IoError, "peer closed while sending " + command);
  }
  return Ok();
}

Result<P2pMessage> BitcoinPeer::receive(int timeout_ms) {
  if (!conn_) return Result<P2pMessage>(Status::Invalid, "not connected");

  uint8_t head[24];
  if (!conn_->recv_all(MutableByteSpan(head, 24), timeout_ms)) {
    return Result<P2pMessage>(Status::Busy, "no message within the timeout");
  }

  uint32_t magic = 0;
  for (int i = 0; i < 4; ++i) magic |= (uint32_t)head[i] << (8 * i);
  if (magic != magic_) return Result<P2pMessage>(Status::Corrupt, "message magic is not this network's");

  P2pMessage message;
  for (int i = 4; i < 16 && head[i] != 0; ++i) message.command.push_back((char)head[i]);

  uint32_t length = 0;
  for (int i = 16; i < 20; ++i) length |= (uint32_t)head[i] << (8 * (i - 16));
  if (length > 4u * 1024 * 1024) return Result<P2pMessage>(Status::Exhausted, "message larger than the limit");

  message.payload.resize(length);
  if (length > 0 && !conn_->recv_all(MutableByteSpan(message.payload.data(), length), timeout_ms)) {
    return Result<P2pMessage>(Status::IoError, "payload truncated for " + message.command + " (" +
                                                   std::to_string(length) + " bytes declared)");
  }

  auto check = sha256d(ByteSpan(message.payload.data(), message.payload.size()));
  for (int i = 0; i < 4; ++i) {
    if (check[(size_t)i] != head[20 + i]) {
      return Result<P2pMessage>(Status::Corrupt, "checksum mismatch on " + message.command);
    }
  }
  return message;
}

Result<P2pMessage> BitcoinPeer::await_command(const std::vector<std::string>& wanted, int timeout_ms) {
  for (int i = 0; i < 64; ++i) {
    auto message = receive(timeout_ms);
    if (!message.ok()) return message;
    for (const auto& w : wanted) {
      if (message->command == w) return message;
    }
    if (message->command == "ping") {
      (void)send("pong", ByteSpan(message->payload.data(), message->payload.size()));
    }
  }
  return Result<P2pMessage>(Status::Busy, "peer never sent what was asked for");
}

Result<PeerInfo> BitcoinPeer::handshake(int32_t start_height, int timeout_ms) {
  if (!conn_) return Result<PeerInfo>(Status::Invalid, "not connected");

  nonce_ = 0;
  for (int i = 0; i < 8; ++i) nonce_ = (nonce_ << 8) | (uint64_t)((i * 37 + 11) & 0xff);

  ByteBuffer payload;
  put_u32(payload, (uint32_t)PROTOCOL_VERSION);
  put_u64(payload, 0);
  put_u64(payload, 0);
  put_netaddr(payload, 0, 0);
  put_netaddr(payload, 0, 0);
  put_u64(payload, nonce_);
  put_varstr(payload, SUBVERSION);
  put_u32(payload, (uint32_t)start_height);
  payload.push_back(0x00);   // do not relay transactions to us

  auto sent = send("version", ByteSpan(payload.data(), payload.size()));
  if (!sent.ok()) return Result<PeerInfo>(sent.error());

  auto peer_version = await_command({"version"}, timeout_ms);
  if (!peer_version.ok()) return Result<PeerInfo>(peer_version.error());

  Cursor c(ByteSpan(peer_version->payload.data(), peer_version->payload.size()));
  info_.version = (int32_t)c.u32();
  info_.services = c.u64();
  c.skip(8);    // timestamp
  c.skip(26);   // addr_recv
  if (info_.version >= 106) {
    c.skip(26);   // addr_from
    c.skip(8);    // nonce
    info_.subversion = c.varstr();
    info_.start_height = (int32_t)c.u32();
  }
  if (!c.ok()) return Result<PeerInfo>(Status::Corrupt, "malformed version message");

  auto acked = send("verack", ByteSpan());
  if (!acked.ok()) return Result<PeerInfo>(acked.error());

  auto verack = await_command({"verack"}, timeout_ms);
  if (!verack.ok()) return Result<PeerInfo>(verack.error());

  info_.address = conn_->peer();
  return info_;
}

Result<std::vector<BlockHeader>> BitcoinPeer::get_headers(
    const std::vector<std::array<uint8_t, 32>>& locator, int timeout_ms) {
  ByteBuffer payload;
  put_u32(payload, (uint32_t)PROTOCOL_VERSION);
  put_varint(payload, locator.size());
  for (const auto& h : locator) payload.insert(payload.end(), h.begin(), h.end());
  for (int i = 0; i < 32; ++i) payload.push_back(0);

  auto sent = send("getheaders", ByteSpan(payload.data(), payload.size()));
  if (!sent.ok()) return Result<std::vector<BlockHeader>>(sent.error());

  auto message = await_command({"headers"}, timeout_ms);
  if (!message.ok()) return Result<std::vector<BlockHeader>>(message.error());

  Cursor c(ByteSpan(message->payload.data(), message->payload.size()));
  uint64_t count = c.varint();
  if (count > 2000) return Result<std::vector<BlockHeader>>(Status::Invalid, "peer sent too many headers");

  std::vector<BlockHeader> headers;
  headers.reserve((size_t)count);
  for (uint64_t i = 0; i < count; ++i) {
    BlockHeader h;
    h.version = (int32_t)c.u32();
    c.array32(h.prev_block);
    c.array32(h.merkle_root);
    h.time = c.u32();
    h.bits = c.u32();
    h.nonce = c.u32();
    c.varint();   // transaction count, always zero in a headers message
    if (!c.ok()) return Result<std::vector<BlockHeader>>(Status::Corrupt, "truncated header");
    headers.push_back(h);
  }
  return headers;
}

Result<bool> BitcoinPeer::offer_transaction(ByteSpan raw_tx, const std::array<uint8_t, 32>& txid,
                                            const std::array<uint8_t, 32>& wtxid, int timeout_ms) {
  ByteBuffer inv;
  put_varint(inv, 1);
  put_u32(inv, INV_WITNESS_TX);
  inv.insert(inv.end(), wtxid.begin(), wtxid.end());

  auto sent = send("inv", ByteSpan(inv.data(), inv.size()));
  if (!sent.ok()) return Result<bool>(sent.error());

  for (int attempt = 0; attempt < 32; ++attempt) {
    auto message = receive(timeout_ms);
    if (!message.ok()) {
      if (message.status() == Status::Busy) return false;
      return Result<bool>(message.error());
    }
    if (message->command == "ping") {
      (void)send("pong", ByteSpan(message->payload.data(), message->payload.size()));
      continue;
    }
    if (message->command == "reject") {
      Cursor c(ByteSpan(message->payload.data(), message->payload.size()));
      std::string rejected = c.varstr();
      uint8_t code = c.u8();
      std::string reason = c.varstr();
      return Result<bool>(Status::Denied, "peer rejected " + rejected + " (" + std::to_string(code) + "): " + reason);
    }
    if (message->command != "getdata") continue;

    Cursor c(ByteSpan(message->payload.data(), message->payload.size()));
    uint64_t count = c.varint();
    for (uint64_t i = 0; i < count && c.ok(); ++i) {
      c.u32();
      std::array<uint8_t, 32> hash{};
      c.array32(hash);
      if (hash == wtxid || hash == txid) {
        auto handed = send("tx", raw_tx);
        if (!handed.ok()) return Result<bool>(handed.error());
        return true;
      }
    }
  }
  return false;
}

Result<bool> BitcoinPeer::has_transaction(const std::array<uint8_t, 32>& txid, int timeout_ms) {
  ByteBuffer request;
  put_varint(request, 1);
  put_u32(request, INV_WITNESS_TX);
  request.insert(request.end(), txid.begin(), txid.end());

  auto sent = send("getdata", ByteSpan(request.data(), request.size()));
  if (!sent.ok()) return Result<bool>(sent.error());

  for (int attempt = 0; attempt < 16; ++attempt) {
    auto message = receive(timeout_ms);
    if (!message.ok()) {
      if (message.status() == Status::Busy) return false;
      return Result<bool>(message.error());
    }
    if (message->command == "tx") return true;
    if (message->command == "notfound") return false;
    if (message->command == "ping") {
      (void)send("pong", ByteSpan(message->payload.data(), message->payload.size()));
    }
  }
  return false;
}

double difficulty_of(uint32_t bits) {
  // difficulty = easiest_target / this_target, both read out of their compact
  // form. Done in floating point because the ratio is what is wanted, not the
  // exact integer.
  auto expand = [](uint32_t compact) {
    double mantissa = (double)(compact & 0x007fffffu);
    int exponent = (int)(compact >> 24);
    double scale = 1.0;
    for (int i = 0; i < exponent - 3; ++i) scale *= 256.0;
    for (int i = 0; i > exponent - 3; --i) scale /= 256.0;
    return mantissa * scale;
  };
  double target = expand(bits);
  if (target <= 0.0) return 0.0;
  return expand(0x1d00ffffu) / target;
}

HashrateSample measure_hashrate(const BlockHeader& header, uint32_t threads, uint32_t seconds) {
  if (threads == 0) threads = 1;
  ByteBuffer base = header.serialize();
  if (base.size() != 80) return HashrateSample{};

  std::vector<uint64_t> counts(threads, 0);
  std::vector<std::array<uint8_t, 32>> best(threads);
  for (auto& b : best) b.fill(0xff);

  volatile bool running = true;
  std::vector<hal::Thread> workers;
  workers.reserve(threads);

  for (uint32_t t = 0; t < threads; ++t) {
    workers.emplace_back([&, t]() {
      uint8_t block[80];
      std::memcpy(block, base.data(), 80);
      uint32_t nonce = t * 0x10000000u;
      uint64_t done = 0;
      std::array<uint8_t, 32> local;
      local.fill(0xff);
      while (running) {
        for (int batch = 0; batch < 4096; ++batch) {
          block[76] = (uint8_t)nonce;
          block[77] = (uint8_t)(nonce >> 8);
          block[78] = (uint8_t)(nonce >> 16);
          block[79] = (uint8_t)(nonce >> 24);
          auto digest = sha256d(ByteSpan(block, 80));
          ++nonce;
          ++done;
          for (int i = 31; i >= 0; --i) {
            if (digest[(size_t)i] < local[(size_t)i]) { local = digest; break; }
            if (digest[(size_t)i] > local[(size_t)i]) break;
          }
        }
      }
      counts[t] = done;
      best[t] = local;
    });
  }

  hal::sleep_ms(seconds * 1000);
  running = false;
  for (auto& w : workers) w.join();

  HashrateSample sample;
  std::array<uint8_t, 32> overall;
  overall.fill(0xff);
  for (uint32_t t = 0; t < threads; ++t) {
    sample.hashes += counts[t];
    for (int i = 31; i >= 0; --i) {
      if (best[t][(size_t)i] < overall[(size_t)i]) { overall = best[t]; break; }
      if (best[t][(size_t)i] > overall[(size_t)i]) break;
    }
  }
  sample.hashes_per_second = seconds > 0 ? (double)sample.hashes / (double)seconds : 0.0;

  std::array<uint8_t, 32> display{};
  for (int i = 0; i < 32; ++i) display[(size_t)i] = overall[31 - (size_t)i];
  sample.best_hash = bytes_to_hex(ByteSpan(display.data(), 32));
  return sample;
}

std::vector<std::string> dns_seeds(uint32_t magic) {
  if (magic == 0x0709110bu) {
    return {"testnet-seed.bitcoin.jonasschnelli.ch", "seed.tbtc.petertodd.net",
            "testnet-seed.bluematt.me"};
  }
  if (magic == 0x40cf030au) {
    return {"seed.signet.bitcoin.sprovoost.nl", "178.128.221.177"};
  }
  return {"seed.bitcoin.sipa.be", "dnsseed.bluematt.me", "seed.bitcoinstats.com",
          "seed.bitcoin.jonasschnelli.ch", "dnsseed.emzy.de", "seed.bitcoin.wiz.biz"};
}

}  // namespace aios::channel
