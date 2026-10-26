#include "aios/node/secure.hpp"
#include "aios/core/bytes.hpp"
#include "aios/net/deflate.hpp"
#include "protocol.hpp"
#include "aios/hal/host.hpp"
#include <cstring>

namespace aios::crypto {

namespace {
inline uint32_t rotl(uint32_t v, int c) { return (v << c) | (v >> (32 - c)); }
inline uint32_t ld32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
inline void st32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
#define QR(a, b, c, d) a += b; d ^= a; d = rotl(d, 16); c += d; b ^= c; b = rotl(b, 12); a += b; d ^= a; d = rotl(d, 8); c += d; b ^= c; b = rotl(b, 7);
void chacha20_block(const Key& key, const Nonce& nonce, uint32_t counter, uint8_t out[64]) {
  uint32_t s[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
  for (int i = 0; i < 8; ++i) s[4 + i] = ld32(key.data() + 4 * i);
  s[12] = counter; s[13] = ld32(nonce.data()); s[14] = ld32(nonce.data() + 4); s[15] = ld32(nonce.data() + 8);
  uint32_t x[16]; std::memcpy(x, s, sizeof x);
  for (int i = 0; i < 10; ++i) {
    QR(x[0], x[4], x[8], x[12]) QR(x[1], x[5], x[9], x[13]) QR(x[2], x[6], x[10], x[14]) QR(x[3], x[7], x[11], x[15])
    QR(x[0], x[5], x[10], x[15]) QR(x[1], x[6], x[11], x[12]) QR(x[2], x[7], x[8], x[13]) QR(x[3], x[4], x[9], x[14])
  }
  for (int i = 0; i < 16; ++i) st32(out + 4 * i, x[i] + s[i]);
}
#undef QR
}  // namespace

void chacha20_xor(const Key& key, const Nonce& nonce, uint32_t counter, uint8_t* data, size_t n) {
  uint8_t block[64];
  for (size_t off = 0; off < n; off += 64, ++counter) {
    chacha20_block(key, nonce, counter, block);
    size_t m = n - off < 64 ? n - off : 64;
    for (size_t i = 0; i < m; ++i) data[off + i] ^= block[i];
  }
}

// Poly1305 with 26-bit limbs (the classic portable implementation).
std::array<uint8_t, 16> poly1305(const std::array<uint8_t, 32>& key, ByteSpan msg) {
  uint32_t r0 = ld32(key.data() + 0) & 0x3ffffff, r1 = (ld32(key.data() + 3) >> 2) & 0x3ffff03, r2 = (ld32(key.data() + 6) >> 4) & 0x3ffc0ff, r3 = (ld32(key.data() + 9) >> 6) & 0x3f03fff, r4 = (ld32(key.data() + 12) >> 8) & 0x00fffff;
  uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
  uint32_t h0 = 0, h1 = 0, h2 = 0, h3 = 0, h4 = 0;
  size_t n = msg.size(); const uint8_t* m = msg.data();
  uint8_t last[16];
  while (n > 0) {
    uint32_t hibit = 1u << 24;
    if (n < 16) { std::memset(last, 0, 16); std::memcpy(last, m, n); last[n] = 1; m = last; hibit = 0; n = 16; }
    h0 += ld32(m + 0) & 0x3ffffff; h1 += (ld32(m + 3) >> 2) & 0x3ffffff; h2 += (ld32(m + 6) >> 4) & 0x3ffffff; h3 += (ld32(m + 9) >> 6) & 0x3ffffff; h4 += (ld32(m + 12) >> 8) | hibit;
    uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
    uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
    uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
    uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
    uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;
    uint32_t c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff; d1 += c; c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff; d2 += c; c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff; d3 += c; c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff; d4 += c; c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff; h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
    if (m == last) break;
    m += 16; n -= 16;
  }
  uint32_t c = h1 >> 26; h1 &= 0x3ffffff; h2 += c; c = h2 >> 26; h2 &= 0x3ffffff; h3 += c; c = h3 >> 26; h3 &= 0x3ffffff; h4 += c; c = h4 >> 26; h4 &= 0x3ffffff; h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
  uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff; uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff; uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff; uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff; uint32_t g4 = h4 + c - (1u << 26);
  uint32_t mask = (g4 >> 31) - 1;
  g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask; mask = ~mask;
  h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2; h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;
  h0 = (h0 | (h1 << 26)) & 0xffffffff; h1 = ((h1 >> 6) | (h2 << 20)) & 0xffffffff; h2 = ((h2 >> 12) | (h3 << 14)) & 0xffffffff; h3 = ((h3 >> 18) | (h4 << 8)) & 0xffffffff;
  uint64_t f = (uint64_t)h0 + ld32(key.data() + 16); h0 = (uint32_t)f; f = (uint64_t)h1 + ld32(key.data() + 20) + (f >> 32); h1 = (uint32_t)f; f = (uint64_t)h2 + ld32(key.data() + 24) + (f >> 32); h2 = (uint32_t)f; f = (uint64_t)h3 + ld32(key.data() + 28) + (f >> 32); h3 = (uint32_t)f;
  std::array<uint8_t, 16> tag; st32(tag.data(), h0); st32(tag.data() + 4, h1); st32(tag.data() + 8, h2); st32(tag.data() + 12, h3);
  return tag;
}

static void pad16(ByteBuffer& b) { while (b.size() % 16) b.push_back(0); }
static void put64(ByteBuffer& b, uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back((uint8_t)(v >> (8 * i))); }

ByteBuffer aead_encrypt(const Key& key, const Nonce& nonce, ByteSpan aad, ByteSpan pt) {
  uint8_t otk[64]; chacha20_block(key, nonce, 0, otk);
  std::array<uint8_t, 32> pk; std::memcpy(pk.data(), otk, 32);
  ByteBuffer ct(pt.begin(), pt.end());
  chacha20_xor(key, nonce, 1, ct.data(), ct.size());
  ByteBuffer mac(aad.begin(), aad.end()); pad16(mac); mac.insert(mac.end(), ct.begin(), ct.end()); pad16(mac); put64(mac, aad.size()); put64(mac, ct.size());
  auto tag = poly1305(pk, mac);
  ct.insert(ct.end(), tag.begin(), tag.end());
  return ct;
}

Result<ByteBuffer> aead_decrypt(const Key& key, const Nonce& nonce, ByteSpan aad, ByteSpan in) {
  if (in.size() < 16) return Result<ByteBuffer>(Status::Corrupt, "ciphertext too short");
  ByteSpan ct = in.subspan(0, in.size() - 16), tag = in.subspan(in.size() - 16);
  uint8_t otk[64]; chacha20_block(key, nonce, 0, otk);
  std::array<uint8_t, 32> pk; std::memcpy(pk.data(), otk, 32);
  ByteBuffer mac(aad.begin(), aad.end()); pad16(mac); mac.insert(mac.end(), ct.begin(), ct.end()); pad16(mac); put64(mac, aad.size()); put64(mac, ct.size());
  auto want = poly1305(pk, mac);
  unsigned diff = 0; for (int i = 0; i < 16; ++i) diff |= want[i] ^ tag[i];
  if (diff) return Result<ByteBuffer>(Status::Denied, "authentication tag mismatch");
  ByteBuffer pt(ct.begin(), ct.end());
  chacha20_xor(key, nonce, 1, pt.data(), pt.size());
  return pt;
}

Key derive_key(const std::string& token, ByteSpan salt) {
  Key k{}; Nonce n{};
  for (size_t i = 0; i < token.size(); ++i) k[i % 32] ^= (uint8_t)token[i] + (uint8_t)(i / 32);
  for (size_t i = 0; i < salt.size() && i < 12; ++i) n[i] = salt[i];
  // Stretch: 4096 rounds of keystream feedback (a deliberate cost per guess).
  for (int r = 0; r < 4096; ++r) { uint8_t block[64]; chacha20_block(k, n, (uint32_t)r, block); for (int i = 0; i < 32; ++i) k[i] ^= block[i]; }
  return k;
}
}  // namespace aios::crypto

namespace aios {

namespace {
class PlainLink final : public Link {
 public:
  explicit PlainLink(std::shared_ptr<hal::Connection> c) : c_(std::move(c)) {}
  bool send(uint8_t t, ByteSpan p) override { return nodeproto::write_frame(*c_, t, p); }
  bool recv(uint8_t& t, ByteBuffer& p, int to) override { return nodeproto::read_frame(*c_, t, p, to); }
  std::string peer() const override { return c_->peer(); }
 private:
  std::shared_ptr<hal::Connection> c_;
};
constexpr uint8_t ENCRYPTED = 0x7E;
constexpr uint8_t ENCRYPTED_DEFLATED = 0x7D;   // the plaintext is u32 raw size + raw-deflate of (type byte + payload)
constexpr size_t kDeflateFrom = 32u << 10;
constexpr uint64_t kInflateMax = 2ull << 30;   // a frame claiming more than 2 GB raw is refused
// The tree's own DEFLATE (net/deflate.cpp): nothing linked. Level 1: the link's job is to go fast, and JSON still
// shrinks to a fraction at it.
bool deflate_frame(const ByteBuffer& pt, ByteBuffer* out) {
  out->clear();
  out->reserve(pt.size() / 4 + 64);
  const uint32_t raw = (uint32_t)pt.size();
  for (int i = 0; i < 4; ++i) out->push_back((uint8_t)(raw >> (8 * i)));
  const size_t n = net::deflate_raw_append(ByteSpan(pt.data(), pt.size()), 1, *out);
  return n > 0 && n + 4 < pt.size() - pt.size() / 10;   // not worth it unless a tenth smaller: sent as it is
}
bool inflate_frame(ByteSpan z, ByteBuffer* out) {
  if (z.size() < 4) return false;
  uint64_t raw = 0;
  for (int i = 0; i < 4; ++i) raw |= (uint64_t)z[i] << (8 * i);
  if (raw == 0 || raw > kInflateMax) return false;
  out->clear();
  size_t end = 0;
  std::string why;
  return net::inflate_raw_tree(z, 4, *out, &end, (size_t)raw, &why) && out->size() == raw && end == z.size();
}
class SecureLink final : public Link {
 public:
  SecureLink(std::shared_ptr<hal::Connection> c, const crypto::Key& k, bool initiator) : c_(std::move(c)), key_(k), send_dir_(initiator ? 1 : 2), recv_dir_(initiator ? 2 : 1) {}
  bool send(uint8_t t, ByteSpan p) override {
    crypto::Nonce n{}; n[0] = send_dir_; uint64_t seq = ++send_seq_; for (int i = 0; i < 8; ++i) n[1 + i] = (uint8_t)(seq >> (8 * i));
    ByteBuffer pt; pt.push_back(t); pt.insert(pt.end(), p.begin(), p.end());
    // A large frame to a peer that reads them deflated goes deflated: an 11 MB JSON reply is ~1 MB on the wire.
    uint8_t ft = ENCRYPTED;
    if (compress_ && pt.size() >= kDeflateFrom) {
      ByteBuffer z;
      if (deflate_frame(pt, &z)) { pt.swap(z); ft = ENCRYPTED_DEFLATED; }
    }
    uint8_t aad[9]; aad[0] = send_dir_; for (int i = 0; i < 8; ++i) aad[1 + i] = (uint8_t)(seq >> (8 * i));
    ByteBuffer ct = crypto::aead_encrypt(key_, n, ByteSpan(aad, 9), pt);
    ByteBuffer frame; frame.reserve(8 + ct.size()); for (int i = 0; i < 8; ++i) frame.push_back((uint8_t)(seq >> (8 * i))); frame.insert(frame.end(), ct.begin(), ct.end());
    return nodeproto::write_frame(*c_, ft, frame);
  }
  bool recv(uint8_t& t, ByteBuffer& p, int to) override {
    uint8_t ft; ByteBuffer frame;
    if (!nodeproto::read_frame(*c_, ft, frame, to) || (ft != ENCRYPTED && ft != ENCRYPTED_DEFLATED) || frame.size() < 8 + 17) return false;
    uint64_t seq = 0; for (int i = 0; i < 8; ++i) seq |= (uint64_t)frame[i] << (8 * i);
    if (seq <= recv_seq_) return false;                       // replay or reorder: the link is dead
    crypto::Nonce n{}; n[0] = recv_dir_; for (int i = 0; i < 8; ++i) n[1 + i] = frame[i];
    uint8_t aad[9]; aad[0] = recv_dir_; std::memcpy(aad + 1, frame.data(), 8);
    auto pt = crypto::aead_decrypt(key_, n, ByteSpan(aad, 9), ByteSpan(frame.data() + 8, frame.size() - 8));
    if (!pt.ok()) return false;
    recv_seq_ = seq;
    if (ft == ENCRYPTED_DEFLATED) {
      ByteBuffer raw;
      if (!inflate_frame(ByteSpan(pt->data(), pt->size()), &raw) || raw.empty()) return false;
      t = raw[0]; p.assign(raw.begin() + 1, raw.end());
      return true;
    }
    t = (*pt)[0]; p.assign(pt->begin() + 1, pt->end());
    return true;
  }
  void set_compress(bool on) override { compress_ = on; }
  std::string peer() const override { return c_->peer(); }
 private:
  std::shared_ptr<hal::Connection> c_;
  crypto::Key key_;
  uint8_t send_dir_, recv_dir_;
  uint64_t send_seq_ = 0, recv_seq_ = 0;
  bool compress_ = false;
};
constexpr uint8_t HS_INIT = 0x70, HS_RESP = 0x71;
const char AAD_INIT[] = "aios-node-hello-initiator", AAD_RESP[] = "aios-node-hello-responder";
crypto::Key session_key(const crypto::Key& base, ByteSpan ri, ByteSpan rr) {
  crypto::Nonce n{}; for (int i = 0; i < 12; ++i) n[i] = ri[i] ^ rr[i];
  crypto::Key k = base; crypto::chacha20_xor(base, n, 0x53455353u, k.data(), 32);   // "SESS"
  return k;
}
}  // namespace

std::unique_ptr<Link> make_plain_link(std::shared_ptr<hal::Connection> c) { return std::make_unique<PlainLink>(std::move(c)); }

ByteBuffer link_feature_payload() {
  ByteBuffer b; Writer w(b);
  w.u32(nodeproto::kFeatureMagic);
  w.u32(nodeproto::kFeatDeflate);
  return b;
}

bool link_peer_deflates(ByteSpan payload) {
  // The feature word is the payload's last 8 bytes when present ('FEAT' + bits): after a device list, or alone.
  if (payload.size() < 8) return false;
  const uint8_t* e = payload.data() + payload.size() - 8;
  uint32_t magic = 0, bits = 0;
  for (int i = 0; i < 4; ++i) { magic |= (uint32_t)e[i] << (8 * i); bits |= (uint32_t)e[4 + i] << (8 * i); }
  return magic == nodeproto::kFeatureMagic && (bits & nodeproto::kFeatDeflate);
}

std::unique_ptr<Link> make_secure_link(std::shared_ptr<hal::Connection> c, const crypto::Key& k, bool initiator) { return std::make_unique<SecureLink>(std::move(c), k, initiator); }

// Initiator: random ri; sends ri || AEAD_K(nonce ri[0..12], aad, my_kernel || payload).
// Responder: verifies, sends rr || AEAD_K(nonce rr[0..12], aad, my_kernel || payload). Both derive the session key from K, ri, rr.
Result<crypto::Key> secure_handshake_initiator(Link& plain, const std::string& token, hal::Host& host, const Id128& my_kernel, Id128& peer_kernel, ByteBuffer& peer_payload, ByteSpan my_payload) {
  uint8_t salt[16] = {'a', 'i', 'o', 's', '-', 'n', 'o', 'd', 'e', '-', 'k', 'e', 'y', '-', 'v', '1'};
  crypto::Key K = crypto::derive_key(token, ByteSpan(salt, 16));
  std::array<uint8_t, 16> ri; host.random(MutableByteSpan(ri.data(), 16));
  crypto::Nonce n{}; std::memcpy(n.data(), ri.data(), 12);
  ByteBuffer pt; { Writer w(pt); w.id(my_kernel); w.raw(my_payload); }
  ByteBuffer msg(ri.begin(), ri.end()); auto ct = crypto::aead_encrypt(K, n, span_of(std::string(AAD_INIT)), pt); msg.insert(msg.end(), ct.begin(), ct.end());
  if (!plain.send(HS_INIT, msg)) return Result<crypto::Key>(Status::IoError, "peer closed during handshake");
  uint8_t t; ByteBuffer reply;
  if (!plain.recv(t, reply, 15000)) return Result<crypto::Key>(Status::IoError, "no handshake reply");
  if (t == nodeproto::Error) { Reader r(reply); Status st = (Status)r.u8(); return Result<crypto::Key>(st, "node: " + r.str()); }
  if (t != HS_RESP || reply.size() < 16 + 16 + 16) return Result<crypto::Key>(Status::Denied, "malformed handshake reply");
  crypto::Nonce nr{}; std::memcpy(nr.data(), reply.data(), 12);
  auto dec = crypto::aead_decrypt(K, nr, span_of(std::string(AAD_RESP)), ByteSpan(reply.data() + 16, reply.size() - 16));
  if (!dec.ok()) return Result<crypto::Key>(Status::Denied, "node did not prove the token");
  Reader r(*dec); peer_kernel = r.id(); peer_payload.assign(dec->begin() + 16, dec->end());
  return session_key(K, ByteSpan(ri.data(), 16), ByteSpan(reply.data(), 16));
}

Result<crypto::Key> secure_handshake_responder(Link& plain, const std::string& token, hal::Host& host, const Id128& my_kernel, Id128& peer_kernel, ByteBuffer& peer_payload, ByteSpan my_payload) {
  return secure_handshake_responder_any(plain, std::vector<std::string>{token}, host, my_kernel, peer_kernel, peer_payload, my_payload);
}

// The same, for a node that accepts more than one token (its own `node serve`
// token and the fleet's cluster key): the initiator's sealed hello opens under
// exactly one of them, and that one keys the session.
Result<crypto::Key> secure_handshake_responder_any(Link& plain, const std::vector<std::string>& tokens, hal::Host& host, const Id128& my_kernel, Id128& peer_kernel, ByteBuffer& peer_payload, ByteSpan my_payload) {
  uint8_t salt[16] = {'a', 'i', 'o', 's', '-', 'n', 'o', 'd', 'e', '-', 'k', 'e', 'y', '-', 'v', '1'};
  uint8_t t; ByteBuffer msg;
  if (!plain.recv(t, msg, 15000) || t != HS_INIT || msg.size() < 16 + 16 + 16) return Result<crypto::Key>(Status::Denied, "no valid handshake");
  crypto::Nonce n{}; std::memcpy(n.data(), msg.data(), 12);
  crypto::Key K{};
  Result<ByteBuffer> dec = Result<ByteBuffer>(Status::Denied, "no token");
  for (const auto& token : tokens) {
    K = crypto::derive_key(token, ByteSpan(salt, 16));
    dec = crypto::aead_decrypt(K, n, span_of(std::string(AAD_INIT)), ByteSpan(msg.data() + 16, msg.size() - 16));
    if (dec.ok()) break;
  }
  if (!dec.ok()) return Result<crypto::Key>(Status::Denied, "peer did not prove the token");
  Reader r(*dec); peer_kernel = r.id(); peer_payload.assign(dec->begin() + 16, dec->end());
  std::array<uint8_t, 16> rr; host.random(MutableByteSpan(rr.data(), 16));
  crypto::Nonce nr{}; std::memcpy(nr.data(), rr.data(), 12);
  ByteBuffer pt; { Writer w(pt); w.id(my_kernel); w.raw(my_payload); }
  ByteBuffer reply(rr.begin(), rr.end()); auto ct = crypto::aead_encrypt(K, nr, span_of(std::string(AAD_RESP)), pt); reply.insert(reply.end(), ct.begin(), ct.end());
  if (!plain.send(HS_RESP, reply)) return Result<crypto::Key>(Status::IoError, "peer closed during handshake");
  return session_key(K, ByteSpan(msg.data(), 16), ByteSpan(rr.data(), 16));
}

}  // namespace aios
