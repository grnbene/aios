#include "aios/net/tls.hpp"
#include "aios/crypto/sha1.hpp"

#include <map>
#include <algorithm>
#include <cstring>
#include <string>

#include "aios/crypto/aes_gcm.hpp"
#include "aios/crypto/curve25519.hpp"
#include "aios/crypto/hmac_sha256.hpp"
#include "aios/crypto/nistp.hpp"
#include "aios/crypto/rsa.hpp"
#include "aios/crypto/sha256.hpp"
#include "aios/crypto/sha512.hpp"
#include <mutex>
#include "aios/node/secure.hpp"   // chacha20_xor, poly1305
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")
#endif

namespace aios::net {
namespace {

enum : uint8_t {
  REC_CHANGE_CIPHER_SPEC = 20,
  REC_ALERT = 21,
  REC_HANDSHAKE = 22,
  REC_APPLICATION = 23,
};
enum : uint8_t {
  HS_CLIENT_HELLO = 1,
  HS_SERVER_HELLO = 2,
  HS_NEW_SESSION_TICKET = 4,
  HS_ENCRYPTED_EXTENSIONS = 8,
  HS_CERTIFICATE = 11,
  HS_CERTIFICATE_VERIFY = 15,
  HS_FINISHED = 20,
};

void be16(ByteBuffer& b, uint16_t v) { b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)v); }
void be24(ByteBuffer& b, uint32_t v) { b.push_back((uint8_t)(v >> 16)); b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)v); }
void raw(ByteBuffer& b, ByteSpan s) { b.insert(b.end(), s.begin(), s.end()); }
void raw(ByteBuffer& b, const std::string& s) { b.insert(b.end(), s.begin(), s.end()); }

using Hash = std::array<uint8_t, 32>;

Hash hmac(ByteSpan key, ByteSpan msg) {
  auto v = crypto::hmac_sha256(key, msg);
  Hash out{};
  std::memcpy(out.data(), v.data(), 32);
  return out;
}

Hash hkdf_extract(ByteSpan salt, ByteSpan ikm) { return hmac(salt, ikm); }

ByteBuffer hkdf_expand(ByteSpan prk, ByteSpan info, size_t len) {
  ByteBuffer out;
  ByteBuffer t;
  uint8_t counter = 1;
  while (out.size() < len) {
    ByteBuffer in(t.begin(), t.end());
    in.insert(in.end(), info.begin(), info.end());
    in.push_back(counter++);
    auto block = hmac(prk, ByteSpan(in.data(), in.size()));
    t.assign(block.begin(), block.end());
    out.insert(out.end(), block.begin(), block.end());
  }
  out.resize(len);
  return out;
}

// HKDF-Expand-Label, the shape RFC 8446 gives every key in the schedule.
ByteBuffer expand_label(ByteSpan secret, const std::string& label, ByteSpan context, size_t len) {
  ByteBuffer info;
  be16(info, (uint16_t)len);
  std::string full = "tls13 " + label;
  info.push_back((uint8_t)full.size());
  raw(info, full);
  info.push_back((uint8_t)context.size());
  raw(info, context);
  return hkdf_expand(secret, ByteSpan(info.data(), info.size()), len);
}

ByteBuffer derive_secret(ByteSpan secret, const std::string& label, ByteSpan transcript_hash) {
  return expand_label(secret, label, transcript_hash, 32);
}

// The same schedule over either hash a suite names: SHA-256 for the
// *_SHA256 suites, SHA-384 for TLS_AES_256_GCM_SHA384 and the 1.2 *_SHA384
// ones. `s384` picks; outputs are 32 or 48 bytes accordingly.
ByteBuffer hash_h(bool s384, ByteSpan data) {
  if (s384) { auto d = crypto::sha384(data); return ByteBuffer(d.begin(), d.end()); }
  auto d = crypto::sha256(data);
  return ByteBuffer(d.begin(), d.end());
}

ByteBuffer hmac_h(bool s384, ByteSpan key, ByteSpan msg) {
  if (!s384) { auto v = crypto::hmac_sha256(key, msg); return ByteBuffer(v.begin(), v.end()); }
  // HMAC-SHA384 (RFC 2104 with a 128-byte block).
  uint8_t block[128] = {0};
  if (key.size() > 128) { auto d = crypto::sha384(key); std::memcpy(block, d.data(), d.size()); }
  else if (!key.empty()) std::memcpy(block, key.data(), key.size());
  ByteBuffer inner(128 + msg.size()), outer(128 + 48);
  for (int i = 0; i < 128; ++i) { inner[(size_t)i] = (uint8_t)(block[i] ^ 0x36); outer[(size_t)i] = (uint8_t)(block[i] ^ 0x5c); }
  if (!msg.empty()) std::memcpy(inner.data() + 128, msg.data(), msg.size());
  auto ih = crypto::sha384(ByteSpan(inner.data(), inner.size()));
  std::memcpy(outer.data() + 128, ih.data(), 48);
  auto oh = crypto::sha384(ByteSpan(outer.data(), outer.size()));
  return ByteBuffer(oh.begin(), oh.end());
}

ByteBuffer hkdf_expand_h(bool s384, ByteSpan prk, ByteSpan info, size_t len) {
  ByteBuffer out, t;
  uint8_t counter = 1;
  while (out.size() < len) {
    ByteBuffer in(t.begin(), t.end());
    in.insert(in.end(), info.begin(), info.end());
    in.push_back(counter++);
    t = hmac_h(s384, prk, ByteSpan(in.data(), in.size()));
    out.insert(out.end(), t.begin(), t.end());
  }
  out.resize(len);
  return out;
}

ByteBuffer expand_label_h(bool s384, ByteSpan secret, const std::string& label, ByteSpan context, size_t len) {
  ByteBuffer info;
  be16(info, (uint16_t)len);
  std::string full = "tls13 " + label;
  info.push_back((uint8_t)full.size());
  raw(info, full);
  info.push_back((uint8_t)context.size());
  raw(info, context);
  return hkdf_expand_h(s384, secret, ByteSpan(info.data(), info.size()), len);
}

// One direction's keys, and the sequence number that makes each record's nonce
// different from every other.
struct Keys {
  crypto::Key key{};            // AES-128-GCM uses the first 16 bytes, AES-256-GCM all 32
  std::array<uint8_t, 12> iv{};
  ByteBuffer secret;
  uint64_t seq = 0;
  bool live = false;
  bool gcm = false;             // AES-GCM instead of ChaCha20-Poly1305
  bool aes256 = false;          // with gcm: AES-256 rather than AES-128
  bool sha384 = false;          // the suite's hash, for the 1.3 key derivation
  // TLS 1.2 AES-CBC (the suites older Windows Schannel offers instead of GCM):
  // HMAC over the record, then the cipher; `mac` 1 SHA-1, 2 SHA-256, 3 SHA-384.
  bool cbc = false;
  uint8_t mac = 0;
  std::array<uint8_t, 48> mac_key{};

  void set(ByteSpan traffic_secret) {
    secret.assign(traffic_secret.begin(), traffic_secret.end());
    auto k = expand_label_h(sha384, traffic_secret, "key", {}, gcm && !aes256 ? 16 : 32);
    auto v = expand_label_h(sha384, traffic_secret, "iv", {}, 12);
    key.fill(0);
    std::memcpy(key.data(), k.data(), k.size());
    std::memcpy(iv.data(), v.data(), 12);
    seq = 0;
    live = true;
  }
  crypto::Nonce nonce() const {
    crypto::Nonce n{};
    std::memcpy(n.data(), iv.data(), 12);
    for (int i = 0; i < 8; ++i) n[11 - (size_t)i] ^= (uint8_t)(seq >> (8 * i));
    return n;
  }
};

std::array<uint8_t, 32> poly_key(const crypto::Key& k, const crypto::Nonce& n) {
  uint8_t block[64];
  std::memset(block, 0, sizeof(block));
  crypto::chacha20_xor(k, n, 0, block, sizeof(block));
  std::array<uint8_t, 32> pk{};
  std::memcpy(pk.data(), block, 32);
  return pk;
}

void pad16(ByteBuffer& b) { while (b.size() % 16) b.push_back(0); }
void put64le(ByteBuffer& b, uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back((uint8_t)(v >> (8 * i))); }

crypto::Aes128Key aes_key(const Keys& k) {
  crypto::Aes128Key a{};
  std::memcpy(a.data(), k.key.data(), 16);
  return a;
}
crypto::Aes256Key aes256_key(const Keys& k) {
  crypto::Aes256Key a{};
  std::memcpy(a.data(), k.key.data(), 32);
  return a;
}

// AEAD as RFC 8439 (or SP 800-38D for GCM) defines it, with the record header
// as associated data. `n` is the full per-record nonce.
ByteBuffer aead_seal_with(const Keys& k, const crypto::Nonce& n, ByteSpan aad, ByteSpan plaintext) {
  if (k.gcm) return k.aes256 ? crypto::aes256_gcm_seal(aes256_key(k), n, aad, plaintext)
                             : crypto::aes128_gcm_seal(aes_key(k), n, aad, plaintext);
  ByteBuffer ct(plaintext.begin(), plaintext.end());
  crypto::chacha20_xor(k.key, n, 1, ct.data(), ct.size());
  ByteBuffer mac(aad.begin(), aad.end());
  pad16(mac);
  mac.insert(mac.end(), ct.begin(), ct.end());
  pad16(mac);
  put64le(mac, aad.size());
  put64le(mac, ct.size());
  auto tag = crypto::poly1305(poly_key(k.key, n), ByteSpan(mac.data(), mac.size()));
  ct.insert(ct.end(), tag.begin(), tag.end());
  return ct;
}

ByteBuffer aead_seal(const Keys& k, ByteSpan aad, ByteSpan plaintext) { return aead_seal_with(k, k.nonce(), aad, plaintext); }

Result<ByteBuffer> aead_open_with(const Keys& k, const crypto::Nonce& n, ByteSpan aad, ByteSpan sealed) {
  if (sealed.size() < 16) return Result<ByteBuffer>(Status::Corrupt, "record too short to hold a tag");
  if (k.gcm) {
    ByteBuffer pt;
    if (!(k.aes256 ? crypto::aes256_gcm_open(aes256_key(k), n, aad, sealed, pt)
                   : crypto::aes128_gcm_open(aes_key(k), n, aad, sealed, pt)))
      return Result<ByteBuffer>(Status::Denied, "record authentication failed");
    return pt;
  }
  ByteSpan ct = sealed.subspan(0, sealed.size() - 16);
  ByteBuffer mac(aad.begin(), aad.end());
  pad16(mac);
  mac.insert(mac.end(), ct.begin(), ct.end());
  pad16(mac);
  put64le(mac, aad.size());
  put64le(mac, ct.size());
  auto want = crypto::poly1305(poly_key(k.key, n), ByteSpan(mac.data(), mac.size()));
  unsigned diff = 0;
  for (size_t i = 0; i < 16; ++i) diff |= (unsigned)(want[i] ^ sealed[ct.size() + i]);
  if (diff) return Result<ByteBuffer>(Status::Denied, "record authentication failed");
  ByteBuffer pt(ct.begin(), ct.end());
  crypto::chacha20_xor(k.key, n, 1, pt.data(), pt.size());
  return pt;
}

Result<ByteBuffer> aead_open(const Keys& k, ByteSpan aad, ByteSpan sealed) { return aead_open_with(k, k.nonce(), aad, sealed); }

size_t mac_len(uint8_t alg) { return alg == 1 ? 20 : alg == 2 ? 32 : 48; }

ByteBuffer hmac_sha1(ByteSpan key, ByteSpan msg) {
  uint8_t block[64] = {0};
  if (key.size() > 64) { auto d = crypto::sha1(key); std::memcpy(block, d.data(), d.size()); }
  else if (!key.empty()) std::memcpy(block, key.data(), key.size());
  uint8_t ipad[64], opad[64];
  for (int i = 0; i < 64; ++i) { ipad[i] = uint8_t(block[i] ^ 0x36); opad[i] = uint8_t(block[i] ^ 0x5c); }
  crypto::SHA1Context c;
  uint8_t inner[20], out[20];
  crypto::sha1_init(c);
  crypto::sha1_update(c, ipad, 64);
  if (!msg.empty()) crypto::sha1_update(c, msg.data(), msg.size());
  crypto::sha1_final(c, inner);
  crypto::sha1_init(c);
  crypto::sha1_update(c, opad, 64);
  crypto::sha1_update(c, inner, 20);
  crypto::sha1_final(c, out);
  return ByteBuffer(out, out + 20);
}

// The MAC of a TLS 1.2 CBC record: HMAC(seq || type || version || length || fragment).
ByteBuffer record_mac(const Keys& k, uint8_t type, ByteSpan fragment) {
  ByteBuffer m;
  for (int i = 7; i >= 0; --i) m.push_back((uint8_t)(k.seq >> (8 * i)));
  m.push_back(type);
  m.push_back(0x03); m.push_back(0x03);
  m.push_back((uint8_t)(fragment.size() >> 8)); m.push_back((uint8_t)fragment.size());
  m.insert(m.end(), fragment.begin(), fragment.end());
  ByteSpan key(k.mac_key.data(), mac_len(k.mac));
  if (k.mac == 1) return hmac_sha1(key, ByteSpan(m.data(), m.size()));
  return hmac_h(k.mac == 3, key, ByteSpan(m.data(), m.size()));
}

// RFC 5246 6.2.3.2: a fresh IV, then AES-CBC over fragment || MAC || padding.
ByteBuffer cbc_seal(hal::Host& host, const Keys& k, uint8_t type, ByteSpan fragment) {
  ByteBuffer data(fragment.begin(), fragment.end());
  ByteBuffer mac = record_mac(k, type, fragment);
  data.insert(data.end(), mac.begin(), mac.end());
  const uint8_t pad = (uint8_t)(15 - data.size() % 16);
  for (int i = 0; i <= pad; ++i) data.push_back(pad);
  ByteBuffer out(16 + data.size());
  host.random(MutableByteSpan(out.data(), 16));
  crypto::aes_cbc_encrypt(ByteSpan(k.key.data(), k.aes256 ? 32 : 16), ByteSpan(out.data(), 16), ByteSpan(data.data(), data.size()),
                          MutableByteSpan(out.data() + 16, data.size()));
  return out;
}

// The other way. A bad padding and a bad MAC are the same failure, and the
// MAC is computed either way (the padding check must not tell them apart).
Result<ByteBuffer> cbc_open(const Keys& k, uint8_t type, ByteSpan body) {
  const size_t ml = mac_len(k.mac);
  if (body.size() < 32 || (body.size() - 16) % 16 != 0 || body.size() - 16 < ml + 1)
    return Result<ByteBuffer>(Status::Corrupt, "a CBC record of an impossible length");
  ByteBuffer pt(body.size() - 16);
  if (!crypto::aes_cbc_decrypt(ByteSpan(k.key.data(), k.aes256 ? 32 : 16), body.subspan(0, 16), body.subspan(16), MutableByteSpan(pt.data(), pt.size())))
    return Result<ByteBuffer>(Status::Corrupt, "record decryption failed");
  const size_t pad = pt.back();
  bool good = pad + 1 + ml <= pt.size();
  const size_t cut = good ? pt.size() - pad - 1 : pt.size() - 1;
  unsigned pdiff = 0;
  for (size_t i = cut; i < pt.size(); ++i) pdiff |= (unsigned)(pt[i] ^ pad);
  if (pdiff) good = false;
  const size_t frag = cut >= ml ? cut - ml : 0;
  ByteBuffer want = record_mac(k, type, ByteSpan(pt.data(), frag));
  unsigned mdiff = 0;
  for (size_t i = 0; i < ml && frag + i < pt.size(); ++i) mdiff |= (unsigned)(want[i] ^ pt[frag + i]);
  if (!good || mdiff) return Result<ByteBuffer>(Status::Denied, "record authentication failed");
  pt.resize(frag);
  return pt;
}

}  // namespace


namespace {

// A handshake signature, whichever algorithm the server picked and whichever
// key kind its certificate carries. `named` comes back as the algorithm's own
// name, or empty when this machine never offered it.
bool verify_handshake_signature(const crypto::Certificate& leaf, uint16_t alg, ByteSpan signed_data,
                                ByteSpan signature, std::string& named) {
  named.clear();
  auto ecdsa = [&](crypto::RsaDigest which) {
    // ECDSA signatures are DER: SEQUENCE { INTEGER r, INTEGER s }.
    ByteBuffer r, sv;
    size_t p = 0;
    if (signature.size() < 8 || signature[p++] != 0x30) return false;
    size_t l = signature[p++];
    if (l & 0x80) p += (l & 0x7f);
    auto integer = [&](ByteBuffer& out) -> bool {
      if (p >= signature.size() || signature[p++] != 0x02) return false;
      size_t n = signature[p++];
      if (p + n > signature.size()) return false;
      size_t i = 0;
      while (i + 1 < n && signature[p + i] == 0) ++i;
      out.assign(signature.begin() + (long)(p + i), signature.begin() + (long)(p + n));
      p += n;
      return true;
    };
    if (!integer(r) || !integer(sv)) return false;
    if (!leaf.key_is_ec) return false;
    ByteSpan key(leaf.key_point.data(), leaf.key_point.size());
    if (which == crypto::RsaDigest::Sha256) {
      auto h = crypto::sha256(signed_data);
      return crypto::ecdsa_verify(leaf.key_curve, key, ByteSpan(h.data(), h.size()),
                                  ByteSpan(r.data(), r.size()), ByteSpan(sv.data(), sv.size()));
    }
    auto h = crypto::sha384(signed_data);
    return crypto::ecdsa_verify(leaf.key_curve, key, ByteSpan(h.data(), h.size()),
                                ByteSpan(r.data(), r.size()), ByteSpan(sv.data(), sv.size()));
  };
  switch (alg) {
    case 0x0403: named = "ecdsa_secp256r1_sha256"; return ecdsa(crypto::RsaDigest::Sha256);
    case 0x0503: named = "ecdsa_secp384r1_sha384"; return ecdsa(crypto::RsaDigest::Sha384);
    case 0x0804:
      named = "rsa_pss_rsae_sha256";
      return leaf.key_is_rsa && crypto::rsa_verify_pss(leaf.rsa_key, crypto::RsaDigest::Sha256, signed_data, signature);
    case 0x0805:
      named = "rsa_pss_rsae_sha384";
      return leaf.key_is_rsa && crypto::rsa_verify_pss(leaf.rsa_key, crypto::RsaDigest::Sha384, signed_data, signature);
    case 0x0401:
      named = "rsa_pkcs1_sha256";
      return leaf.key_is_rsa && crypto::rsa_verify_pkcs1(leaf.rsa_key, crypto::RsaDigest::Sha256, signed_data, signature);
    case 0x0501:
      named = "rsa_pkcs1_sha384";
      return leaf.key_is_rsa && crypto::rsa_verify_pkcs1(leaf.rsa_key, crypto::RsaDigest::Sha384, signed_data, signature);
    default:
      return false;                       // named stays empty: this was never offered
  }
}

}  // namespace

struct TlsClient::Impl {
  hal::Host& host;
  std::string trust_path;
  std::unique_ptr<hal::Connection> conn;
  ByteBuffer inbox;             // decrypted application data not yet handed out
  Keys send_keys, recv_keys;
  ByteBuffer transcript;        // every handshake message, in order
  std::vector<crypto::Certificate> chain;
  // 1.2 and 1.3 protect a record differently: 1.3 hides the content type inside
  // the ciphertext and always says "application data" on the outside, 1.2 keeps
  // the real type in the header and puts it in the associated data instead.
  bool tls12 = false;
  bool gcm12 = false;           // 1.2 AES-GCM: 4-byte salt + 8-byte explicit nonce per record
  bool sha384 = false;          // the negotiated suite hashes with SHA-384
  std::vector<crypto::Certificate> client_chain;   // presented on request
  crypto::PrivateKey client_key;
  ByteBuffer res_master;        // 1.3 resumption master secret, for the tickets that follow
  std::string ticket_key;       // host:port this connection's tickets are filed under
  uint16_t suite = 0;
  std::string subject, issuer, anchor;

  Impl(hal::Host& h, std::string p) : host(h), trust_path(std::move(p)) {}

  ByteBuffer transcript_hash() const { return hash_h(sha384, ByteSpan(transcript.data(), transcript.size())); }

  Result<void> send_record(uint8_t type, ByteSpan payload) {
    ByteBuffer wire;
    if (!send_keys.live || (tls12 && type == REC_CHANGE_CIPHER_SPEC)) {
      wire.push_back(type);
      be16(wire, 0x0303);
      be16(wire, (uint16_t)payload.size());
      raw(wire, payload);
    } else if (tls12 && send_keys.cbc) {
      auto sealed = cbc_seal(host, send_keys, type, payload);
      ++send_keys.seq;
      wire.push_back(type);
      be16(wire, 0x0303);
      be16(wire, (uint16_t)sealed.size());
      raw(wire, ByteSpan(sealed.data(), sealed.size()));
    } else if (tls12) {
      ByteBuffer aad;
      for (int i = 7; i >= 0; --i) aad.push_back((uint8_t)(send_keys.seq >> (8 * i)));
      aad.push_back(type);
      be16(aad, 0x0303);
      be16(aad, (uint16_t)payload.size());
      auto sealed = aead_seal(send_keys, ByteSpan(aad.data(), aad.size()), payload);
      ++send_keys.seq;
      wire.push_back(type);
      be16(wire, 0x0303);
      be16(wire, (uint16_t)(sealed.size() + (gcm12 ? 8 : 0)));
      if (gcm12) raw(wire, ByteSpan(aad.data(), 8));   // explicit nonce = sequence number
      raw(wire, ByteSpan(sealed.data(), sealed.size()));
    } else {
      ByteBuffer inner(payload.begin(), payload.end());
      inner.push_back(type);                       // the real type rides inside
      ByteBuffer header;
      header.push_back(REC_APPLICATION);
      be16(header, 0x0303);
      be16(header, (uint16_t)(inner.size() + 16));
      auto sealed = aead_seal(send_keys, ByteSpan(header.data(), header.size()), ByteSpan(inner.data(), inner.size()));
      ++send_keys.seq;
      wire = header;
      raw(wire, ByteSpan(sealed.data(), sealed.size()));
    }
    if (!conn->send_all(ByteSpan(wire.data(), wire.size())))
      return Result<void>(Status::IoError, "the connection dropped while sending");
    return Ok();
  }

  Result<ByteBuffer> read_exact(size_t n, int timeout_ms) {
    ByteBuffer out(n);
    if (n == 0) return out;
    if (!conn->recv_all(MutableByteSpan(out.data(), n), timeout_ms)) {
      if (!conn->alive()) return Result<ByteBuffer>(Status::IoError, "the peer closed the connection");
      return Result<ByteBuffer>(Status::Busy, "the peer sent nothing in time");
    }
    return out;
  }

  // One record, decrypted if the keys are live. `type` comes back as the real
  // content type.
  Result<ByteBuffer> read_record(uint8_t& type, int timeout_ms) {
    auto head = read_exact(5, timeout_ms);
    if (!head.ok()) return head;
    uint8_t rtype = (*head)[0];
    size_t len = ((size_t)(*head)[3] << 8) | (*head)[4];
    if (len > (1u << 16)) return Result<ByteBuffer>(Status::Corrupt, "implausible record length");
    auto body = read_exact(len, timeout_ms);
    if (!body.ok()) return body;
    if (rtype == REC_CHANGE_CIPHER_SPEC) { type = rtype; return ByteBuffer{}; }   // carries no meaning here
    if (tls12) {
      if (!recv_keys.live) { type = rtype; return *body; }
      if (recv_keys.cbc) {
        auto pt = cbc_open(recv_keys, rtype, ByteSpan(body->data(), body->size()));
        if (!pt.ok()) return pt;
        ++recv_keys.seq;
        type = rtype;
        return pt;
      }
      const size_t explicit_len = gcm12 ? 8 : 0;
      if (body->size() < explicit_len + 16) return Result<ByteBuffer>(Status::Corrupt, "record too short to hold a tag");
      crypto::Nonce n = recv_keys.nonce();
      if (gcm12) std::memcpy(n.data() + 4, body->data(), 8);
      ByteBuffer aad;
      for (int i = 7; i >= 0; --i) aad.push_back((uint8_t)(recv_keys.seq >> (8 * i)));
      aad.push_back(rtype);
      be16(aad, 0x0303);
      be16(aad, (uint16_t)(body->size() - explicit_len - 16));      // the PLAINTEXT length
      auto pt12 = aead_open_with(recv_keys, n, ByteSpan(aad.data(), aad.size()),
                                 ByteSpan(body->data() + explicit_len, body->size() - explicit_len));
      if (!pt12.ok()) return pt12;
      ++recv_keys.seq;
      type = rtype;
      return pt12;
    }
    if (!recv_keys.live || rtype != REC_APPLICATION) { type = rtype; return *body; }
    auto pt = aead_open(recv_keys, ByteSpan(head->data(), 5), ByteSpan(body->data(), body->size()));
    if (!pt.ok()) return pt;
    ++recv_keys.seq;
    ByteBuffer inner = *pt;
    while (!inner.empty() && inner.back() == 0) inner.pop_back();     // strip padding
    if (inner.empty()) return Result<ByteBuffer>(Status::Corrupt, "a record with no content type");
    type = inner.back();
    inner.pop_back();
    return inner;
  }
};

namespace {
// Tickets this process holds, by host:port (RFC 8446 4.6.1). One per server:
// a new ticket replaces the old, and a used one is dropped (single use).
struct Ticket {
  ByteBuffer psk, ticket;
  uint32_t age_add = 0, lifetime_s = 0;
  uint64_t received_ms = 0;
  bool sha384 = false;
  uint16_t suite = 0;
  std::string subject, issuer, anchor;
};
std::map<std::string, Ticket>& ticket_cache() { static std::map<std::string, Ticket> c; return c; }
// Connections are made from several threads at once (a page's stylesheets and scripts download together), and every
// one reads and writes this process-wide map.
std::mutex& ticket_mutex() { static std::mutex m; return m; }

// Each curve signs with its own hash (RFC 8446 4.2.3): ecdsa_secp256r1_sha256, _secp384r1_sha384, _secp521r1_sha512.
uint16_t ec_sig_alg(crypto::NistCurve c) {
  return c == crypto::NistCurve::P521 ? 0x0603 : c == crypto::NistCurve::P384 ? 0x0503 : 0x0403;
}
ByteBuffer ec_digest(crypto::NistCurve c, ByteSpan data) {
  if (c == crypto::NistCurve::P521) { auto d = crypto::sha512(data); return ByteBuffer(d.begin(), d.end()); }
  if (c == crypto::NistCurve::P384) { auto d = crypto::sha384(data); return ByteBuffer(d.begin(), d.end()); }
  auto d = crypto::sha256(data);
  return ByteBuffer(d.begin(), d.end());
}

Result<ByteBuffer> sign_with(const crypto::PrivateKey& key, uint16_t alg, ByteSpan data) {
  if (key.is_ec) {
    ByteBuffer h = ec_digest(key.ec_key.curve, data);
    return crypto::ecdsa_sign(key.ec_key.curve, ByteSpan(key.ec_key.d.data(), key.ec_key.d.size()), ByteSpan(h.data(), h.size()));
  }
  if (key.is_rsa) {
    if (alg == 0x0804) return crypto::rsa_sign_pss(key.rsa_key, crypto::RsaDigest::Sha256, data);
    return crypto::rsa_sign_pkcs1(key.rsa_key, crypto::RsaDigest::Sha256, data);
  }
  return Result<ByteBuffer>(Status::Unsupported, "no supported key type for handshake signing");
}
// The one algorithm this key produces that the server listed (an empty list accepts any).
uint16_t client_sig_alg(const crypto::PrivateKey& key, const std::vector<uint16_t>& offered, bool tls13) {
  auto ok = [&](uint16_t a) { return offered.empty() || std::find(offered.begin(), offered.end(), a) != offered.end(); };
  if (key.is_ec) { uint16_t a = ec_sig_alg(key.ec_key.curve); return ok(a) ? a : 0; }
  if (key.is_rsa) { if (ok(0x0804)) return 0x0804; if (!tls13 && ok(0x0401)) return 0x0401; }
  return 0;
}
}  // namespace

TlsClient::TlsClient(hal::Host& host, std::string trust_store_path)
    : impl_(new Impl(host, std::move(trust_store_path))) {}

Result<void> TlsClient::use_client_certificate(const std::string& chain_path, const std::string& key_path) {
  Impl& s = *impl_;
  auto c = s.host.read_file(chain_path);
  if (!c.ok()) return Result<void>(Status::NotFound, chain_path + ": " + c.error().str());
  auto chain = crypto::x509_parse_bundle(ByteSpan(c->data(), c->size()));
  if (!chain.ok()) return Result<void>(chain.error());
  if (chain->empty()) return Result<void>(Status::Invalid, chain_path + ": holds no certificate");
  auto k = s.host.read_file(key_path);
  if (!k.ok()) return Result<void>(Status::NotFound, key_path + ": " + k.error().str());
  auto key = crypto::x509_parse_private_key(ByteSpan(k->data(), k->size()));
  if (!key.ok()) return Result<void>(key.error());
  s.client_chain = std::move(*chain);
  s.client_key = std::move(*key);
  return Ok();
}
TlsClient::~TlsClient() { close(); }

void TlsClient::close() {
  if (impl_ && impl_->conn) { impl_->conn->close(); impl_->conn.reset(); }
  closed_ = true;
}

#ifdef _WIN32
namespace {
// Windows keeps its trusted roots in the certificate store, not in a file: write them out as a PEM
// bundle the TLS client reads like any other. Refreshed daily, so a root Windows adds or removes
// reaches this machine without anyone copying files.
void export_windows_roots(hal::Host& host, const std::string& path) {
  if (auto st = host.stat(path); st.ok() && st->mtime_ns + 86400ull * 1000000000ull > host.wall_ns()) return;
  std::string pem;
  for (const wchar_t* name : {L"ROOT", L"CA"}) {
    HCERTSTORE store = ::CertOpenSystemStoreW(0, name);
    if (!store) continue;
    for (PCCERT_CONTEXT ctx = nullptr; (ctx = ::CertEnumCertificatesInStore(store, ctx)) != nullptr;) {
      if (ctx->dwCertEncodingType != X509_ASN_ENCODING) continue;
      DWORD n = 0;
      if (!::CryptBinaryToStringA(ctx->pbCertEncoded, ctx->cbCertEncoded, CRYPT_STRING_BASE64HEADER, nullptr, &n)) continue;
      std::string block(n, '\0');
      if (!::CryptBinaryToStringA(ctx->pbCertEncoded, ctx->cbCertEncoded, CRYPT_STRING_BASE64HEADER, block.data(), &n)) continue;
      block.resize(n);
      pem += block;
    }
    ::CertCloseStore(store, 0);
  }
  if (pem.empty()) return;
  const size_t slash = path.rfind('/');
  if (slash != std::string::npos) (void)host.make_dir(path.substr(0, slash));
  (void)host.write_file(path, ByteSpan((const uint8_t*)pem.data(), pem.size()));
}
}  // namespace
#endif

std::string default_trust_store(hal::Host& host, const std::string& kernel_root) {
  // The machine's own store first: a kernel that was given certificates uses
  // those. Otherwise the substrate's, where there is one.
  const std::string own = kernel_root + "/ssl/ca-certificates.crt";
#ifdef _WIN32
  // unless a store was put there by hand (no marker), the system's roots are kept current in it
  if (!host.exists(own) || host.exists(kernel_root + "/ssl/.from-windows")) {
    export_windows_roots(host, own);
    std::string mark = "written from the Windows certificate store\n";
    (void)host.write_file(kernel_root + "/ssl/.from-windows", ByteSpan((const uint8_t*)mark.data(), mark.size()));
  }
#endif
  if (host.exists(own)) return own;
  for (const char* p : {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
                        "/etc/ssl/cert.pem", "/usr/local/etc/ssl/cert.pem"})
    if (host.exists(p)) return p;
  return own;      // named, so the failure says exactly which file is missing
}

Result<void> TlsClient::connect(const std::string& host_name, uint16_t port, int timeout_ms) {
  auto c = hal::net_connect(host_name, port, timeout_ms);
  if (!c.ok()) return Result<void>(c.error());
  return connect_over(std::move(*c), host_name, port, timeout_ms);
}

Result<void> TlsClient::connect_over(std::unique_ptr<hal::Connection> conn, const std::string& host_name, uint16_t port,
                                     int timeout_ms) {
  Impl& s = *impl_;

  auto store_raw = s.host.read_file(s.trust_path);
  if (!store_raw.ok())
    return Result<void>(Status::NotFound,
                       s.trust_path + ": this machine holds no certificates, so it cannot tell who it is talking to");
  auto anchors = crypto::x509_parse_bundle(ByteSpan(store_raw->data(), store_raw->size()));
  if (!anchors.ok()) return Result<void>(anchors.error());

  // This machine's own TLS identity, kept beside its trust store.
  if (s.client_chain.empty()) {
    const size_t slash = s.trust_path.rfind('/');
    const std::string dir = slash == std::string::npos ? "." : s.trust_path.substr(0, slash);
    if (s.host.exists(dir + "/client.crt") && s.host.exists(dir + "/client.key"))
      (void)use_client_certificate(dir + "/client.crt", dir + "/client.key");
  }
  s.ticket_key = host_name + ":" + std::to_string(port);
  s.res_master.clear();
  info_ = TlsInfo{};
  // A ticket from an earlier session with this server: offer it, single use.
  Ticket resume;
  bool have_ticket = false;
  {
    std::lock_guard<std::mutex> lk(ticket_mutex());
    auto it = ticket_cache().find(s.ticket_key);
    if (it != ticket_cache().end()) {
      const uint64_t age_ms = s.host.monotonic_ns() / 1000000ull - it->second.received_ms;
      if (age_ms / 1000 < it->second.lifetime_s) { resume = it->second; have_ticket = true; }
      ticket_cache().erase(it);
    }
  }

  s.conn = std::move(conn);

  // ---- ClientHello ----
  crypto::Scalar32 secret{};
  s.host.random(MutableByteSpan(secret.data(), secret.size()));
  secret[0] &= 248; secret[31] &= 127; secret[31] |= 64;
  crypto::Point32 our_share = crypto::x25519_base(secret);
  // A P-256 key too, so a server that prefers secp256r1 (or a TLS 1.2 server
  // whose certificate is P-256, which OpenSSL then requires in our groups)
  // can use it without a HelloRetryRequest.
  ByteBuffer p256_secret(32, 0), p256_share;
  for (int tries = 0; tries < 8 && p256_share.empty(); ++tries) {
    s.host.random(MutableByteSpan(p256_secret.data(), p256_secret.size()));
    auto pub = crypto::ecdsa_public_key(crypto::NistCurve::P256, ByteSpan(p256_secret.data(), 32));
    if (pub.ok()) p256_share = std::move(*pub);
  }

  ByteBuffer hello;
  hello.push_back(HS_CLIENT_HELLO);
  ByteBuffer body;
  be16(body, 0x0303);                       // legacy_version
  {
    uint8_t rnd[32];
    s.host.random(MutableByteSpan(rnd, 32));
    raw(body, ByteSpan(rnd, 32));
  }
  body.push_back(32);                       // legacy_session_id: a fresh one, as the spec advises
  {
    uint8_t sid[32];
    s.host.random(MutableByteSpan(sid, 32));
    raw(body, ByteSpan(sid, 32));
  }
  // ChaCha20-Poly1305 first (this machine's AES is portable software, not
  // AES-NI), then AES-GCM for the servers configured to take nothing else.
  // TLS 1.3 suites, then TLS 1.2 ECDHE suites for the servers that still refuse
  // 1.3 — the npm registry among them, on every edge it answers from.
  static const uint16_t kSuites[] = {
      0x1303,   // TLS_CHACHA20_POLY1305_SHA256
      0x1301,   // TLS_AES_128_GCM_SHA256
      0x1302,   // TLS_AES_256_GCM_SHA384
      0xcca9,   // ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256
      0xcca8,   // ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256
      0xc02b,   // ECDHE_ECDSA_WITH_AES_128_GCM_SHA256
      0xc02f,   // ECDHE_RSA_WITH_AES_128_GCM_SHA256
      0xc02c,   // ECDHE_ECDSA_WITH_AES_256_GCM_SHA384
      0xc030,   // ECDHE_RSA_WITH_AES_256_GCM_SHA384
      // AES-CBC last: what Windows Server 2012 R2's Schannel (SQL Server 2014/2016 on it) has
      // for ECDHE with an RSA certificate - it has no ECDHE_RSA GCM suite.
      0xc027,   // ECDHE_RSA_WITH_AES_128_CBC_SHA256
      0xc028,   // ECDHE_RSA_WITH_AES_256_CBC_SHA384
      0xc023,   // ECDHE_ECDSA_WITH_AES_128_CBC_SHA256
      0xc024,   // ECDHE_ECDSA_WITH_AES_256_CBC_SHA384
      0xc013,   // ECDHE_RSA_WITH_AES_128_CBC_SHA
      0xc014,   // ECDHE_RSA_WITH_AES_256_CBC_SHA
      0xc009,   // ECDHE_ECDSA_WITH_AES_128_CBC_SHA
      0xc00a,   // ECDHE_ECDSA_WITH_AES_256_CBC_SHA
  };
  {
    // Resuming: a PSK is bound to its hash, so only suites with that hash are offered.
    std::vector<uint16_t> suites;
    for (uint16_t cs : kSuites) {
      if (have_ticket && ((cs == 0x1302) != resume.sha384 || cs < 0x1301 || cs > 0x1303)) continue;
      suites.push_back(cs);
    }
    be16(body, (uint16_t)(2 * suites.size()));
    for (uint16_t cs : suites) be16(body, cs);
  }
  body.push_back(1); body.push_back(0);     // no compression

  ByteBuffer ext;
  {                                          // server_name
    ByteBuffer sn;
    be16(sn, (uint16_t)(host_name.size() + 3));
    sn.push_back(0);
    be16(sn, (uint16_t)host_name.size());
    raw(sn, host_name);
    be16(ext, 0);
    be16(ext, (uint16_t)sn.size());
    raw(ext, ByteSpan(sn.data(), sn.size()));
  }
  {                                          // supported_versions: 1.3 preferred, 1.2 accepted
    be16(ext, 43);
    be16(ext, 5);
    ext.push_back(4);
    be16(ext, 0x0304);
    be16(ext, 0x0303);
  }
  {                                          // ec_point_formats: uncompressed, which 1.2 servers expect to see
    be16(ext, 11);
    be16(ext, 2);
    ext.push_back(1);
    ext.push_back(0);
  }
  {                                          // supported_groups: x25519, secp256r1
    be16(ext, 10);
    be16(ext, 6);
    be16(ext, 4);
    be16(ext, 0x001d);
    be16(ext, 0x0017);
  }
  {                                          // signature_algorithms this machine can check
    ByteBuffer sa;
    be16(sa, 12);
    be16(sa, 0x0403);                        // ecdsa_secp256r1_sha256
    be16(sa, 0x0503);                        // ecdsa_secp384r1_sha384
    be16(sa, 0x0804);                        // rsa_pss_rsae_sha256
    be16(sa, 0x0805);                        // rsa_pss_rsae_sha384
    be16(sa, 0x0401);                        // rsa_pkcs1_sha256
    be16(sa, 0x0501);                        // rsa_pkcs1_sha384
    be16(ext, 13);
    be16(ext, (uint16_t)sa.size());
    raw(ext, ByteSpan(sa.data(), sa.size()));
  }
  {                                          // key_share
    ByteBuffer ks;
    be16(ks, (uint16_t)(36 + 4 + p256_share.size()));
    be16(ks, 0x001d);
    be16(ks, 32);
    raw(ks, ByteSpan(our_share.data(), our_share.size()));
    be16(ks, 0x0017);
    be16(ks, (uint16_t)p256_share.size());
    raw(ks, ByteSpan(p256_share.data(), p256_share.size()));
    be16(ext, 51);
    be16(ext, (uint16_t)ks.size());
    raw(ext, ByteSpan(ks.data(), ks.size()));
  }
  {                                          // ALPN: plain HTTP/1.1, which is what this machine speaks
    ByteBuffer al;
    be16(al, 9);
    al.push_back(8);
    raw(al, std::string("http/1.1"));
    be16(ext, 16);
    be16(ext, (uint16_t)al.size());
    raw(ext, ByteSpan(al.data(), al.size()));
  }
  size_t binder_tail = 0;                    // bytes of the hello the binder must not cover
  if (have_ticket) {
    {                                        // psk_key_exchange_modes: psk_dhe_ke
      be16(ext, 45); be16(ext, 2); ext.push_back(1); ext.push_back(1);
    }
    const size_t hl_t = resume.sha384 ? 48 : 32;
    ByteBuffer psk;
    {                                        // pre_shared_key, last; the binder is filled in below
      ByteBuffer ids;
      be16(ids, (uint16_t)resume.ticket.size());
      raw(ids, ByteSpan(resume.ticket.data(), resume.ticket.size()));
      const uint32_t age = (uint32_t)(s.host.monotonic_ns() / 1000000ull - resume.received_ms) + resume.age_add;
      ids.push_back((uint8_t)(age >> 24)); ids.push_back((uint8_t)(age >> 16)); ids.push_back((uint8_t)(age >> 8)); ids.push_back((uint8_t)age);
      be16(psk, (uint16_t)ids.size());
      raw(psk, ByteSpan(ids.data(), ids.size()));
      be16(psk, (uint16_t)(1 + hl_t));
      psk.push_back((uint8_t)hl_t);
      for (size_t i = 0; i < hl_t; ++i) psk.push_back(0);
      be16(ext, 41);
      be16(ext, (uint16_t)psk.size());
      raw(ext, ByteSpan(psk.data(), psk.size()));
      binder_tail = 2 + 1 + hl_t;
    }
  }
  be16(body, (uint16_t)ext.size());
  raw(body, ByteSpan(ext.data(), ext.size()));
  be24(hello, (uint32_t)body.size());
  raw(hello, ByteSpan(body.data(), body.size()));
  if (have_ticket) {
    // binder = HMAC(finished_key, Hash(ClientHello up to the binders)) (RFC 8446 4.2.11.2)
    const bool h384t = resume.sha384;
    const size_t hl_t = h384t ? 48 : 32;
    ByteBuffer zeros_t(hl_t, 0);
    auto early_t = hmac_h(h384t, ByteSpan(zeros_t.data(), zeros_t.size()), ByteSpan(resume.psk.data(), resume.psk.size()));
    auto empty_t = hash_h(h384t, ByteSpan());
    auto binder_key = expand_label_h(h384t, ByteSpan(early_t.data(), early_t.size()), "res binder", ByteSpan(empty_t.data(), empty_t.size()), hl_t);
    auto fin_key = expand_label_h(h384t, ByteSpan(binder_key.data(), binder_key.size()), "finished", {}, hl_t);
    auto th_t = hash_h(h384t, ByteSpan(hello.data(), hello.size() - binder_tail));
    auto binder = hmac_h(h384t, ByteSpan(fin_key.data(), fin_key.size()), ByteSpan(th_t.data(), th_t.size()));
    std::memcpy(hello.data() + hello.size() - hl_t, binder.data(), hl_t);
  }
  raw(s.transcript, ByteSpan(hello.data(), hello.size()));
  if (auto r = s.send_record(REC_HANDSHAKE, ByteSpan(hello.data(), hello.size())); !r.ok()) return r;

  // ---- ServerHello ----
  uint8_t rtype = 0;
  ByteBuffer rec;
  for (;;) {
    auto got = s.read_record(rtype, timeout_ms);
    if (!got.ok()) return Result<void>(got.error());
    if (rtype == REC_CHANGE_CIPHER_SPEC) continue;
    if (rtype == REC_ALERT) {
      // An alert here is the server's reason for refusing the hello; say which.
      std::string why = "the server refused the handshake";
      if (got->size() >= 2) {
        static const struct { uint8_t code; const char* text; } kAlerts[] = {
            {40, "handshake_failure"}, {47, "illegal_parameter"}, {50, "decode_error"},
            {70, "protocol_version"}, {109, "missing_extension"}, {112, "unrecognized_name"},
            {80, "internal_error"}, {51, "decrypt_error"}, {42, "bad_certificate"}};
        const char* text = "alert";
        for (auto& a : kAlerts) if (a.code == (*got)[1]) text = a.text;
        why += std::string(": ") + text + " (" + std::to_string((int)(*got)[1]) + ")";
      }
      return Result<void>(Status::Denied, why);
    }
    rec = *got;
    break;
  }
  if (rec.size() < 4 || rec[0] != HS_SERVER_HELLO) {
    // Say what did arrive: "not a ServerHello" is the same message whether the
    // server sent a retry request, a fragment, or nothing at all.
    std::string what = "record type " + std::to_string((int)rtype) + ", " + std::to_string(rec.size()) + " bytes";
    if (!rec.empty()) what += ", first byte " + std::to_string((int)rec[0]);
    return Result<void>(Status::Corrupt, "the server did not answer with a ServerHello (" + what + ")");
  }
  raw(s.transcript, ByteSpan(rec.data(), rec.size()));

  crypto::Point32 peer_share{};

  ByteBuffer peer_p256;   // the server's secp256r1 share, when it chose that group
  std::array<uint8_t, 32> server_random{};
  bool server_chose_13 = false;
  bool psk_accepted = false;
  uint16_t chosen_suite = 0;
  {
    size_t p = 4 + 2;                       // type/len, legacy_version
    if (p + 32 > rec.size()) return Result<void>(Status::Corrupt, "short ServerHello");
    std::memcpy(server_random.data(), rec.data() + p, 32);
    p += 32;
    size_t sid = rec[p++];
    p += sid;
    if (p + 3 > rec.size()) return Result<void>(Status::Corrupt, "short ServerHello");
    chosen_suite = (uint16_t)((rec[p] << 8) | rec[p + 1]);
    p += 3;                                  // suite + compression
    if (p + 2 <= rec.size()) {
      size_t elen = (size_t)((rec[p] << 8) | rec[p + 1]);
      p += 2;
      size_t end = p + elen;
      while (p + 4 <= end && p + 4 <= rec.size()) {
        uint16_t et = (uint16_t)((rec[p] << 8) | rec[p + 1]);
        size_t el = (size_t)((rec[p + 2] << 8) | rec[p + 3]);
        p += 4;
        if (p + el > rec.size()) break;
        if (et == 41 && el >= 2) {           // pre_shared_key: our one identity, accepted
          psk_accepted = have_ticket && rec[p] == 0 && rec[p + 1] == 0;
        } else if (et == 43 && el >= 2) {    // supported_versions: the server's real answer
          server_chose_13 = (rec[p] == 0x03 && rec[p + 1] == 0x04);
        } else if (et == 51 && el >= 4) {    // key_share
          uint16_t group = (uint16_t)((rec[p] << 8) | rec[p + 1]);
          size_t klen = (size_t)((rec[p + 2] << 8) | rec[p + 3]);
          if (group == 0x001d && klen == 32) std::memcpy(peer_share.data(), rec.data() + p + 4, 32);
          else if (group == 0x0017 && klen == 65) peer_p256.assign(rec.begin() + (long)p + 4, rec.begin() + (long)p + 4 + 65);
        }
        p += el;
      }
    }
  }

  // A server that did not choose 1.3 is speaking 1.2, and that is a different
  // handshake — not a worse one, and the npm registry answers with nothing else.
  if (!server_chose_13) {
    const char* name12 = nullptr;
    bool gcm = false, a256 = false, prf384 = false;
    uint8_t cbc_mac = 0;
    switch (chosen_suite) {
      case 0xcca9: name12 = "ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256"; break;
      case 0xcca8: name12 = "ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256"; break;
      case 0xc02b: name12 = "ECDHE_ECDSA_WITH_AES_128_GCM_SHA256"; gcm = true; break;
      case 0xc02f: name12 = "ECDHE_RSA_WITH_AES_128_GCM_SHA256"; gcm = true; break;
      case 0xc02c: name12 = "ECDHE_ECDSA_WITH_AES_256_GCM_SHA384"; gcm = a256 = true; break;
      case 0xc030: name12 = "ECDHE_RSA_WITH_AES_256_GCM_SHA384"; gcm = a256 = true; break;
      case 0xc027: name12 = "ECDHE_RSA_WITH_AES_128_CBC_SHA256"; cbc_mac = 2; break;
      case 0xc023: name12 = "ECDHE_ECDSA_WITH_AES_128_CBC_SHA256"; cbc_mac = 2; break;
      case 0xc028: name12 = "ECDHE_RSA_WITH_AES_256_CBC_SHA384"; cbc_mac = 3; a256 = prf384 = true; break;
      case 0xc024: name12 = "ECDHE_ECDSA_WITH_AES_256_CBC_SHA384"; cbc_mac = 3; a256 = prf384 = true; break;
      case 0xc013: name12 = "ECDHE_RSA_WITH_AES_128_CBC_SHA"; cbc_mac = 1; break;
      case 0xc009: name12 = "ECDHE_ECDSA_WITH_AES_128_CBC_SHA"; cbc_mac = 1; break;
      case 0xc014: name12 = "ECDHE_RSA_WITH_AES_256_CBC_SHA"; cbc_mac = 1; a256 = true; break;
      case 0xc00a: name12 = "ECDHE_ECDSA_WITH_AES_256_CBC_SHA"; cbc_mac = 1; a256 = true; break;
      default: break;
    }
    if (!name12)
      return Result<void>(Status::Unsupported,
                         "the server chose a TLS 1.2 cipher suite this machine did not offer (0x" +
                             std::string(1, "0123456789abcdef"[(chosen_suite >> 12) & 15]) +
                             std::string(1, "0123456789abcdef"[(chosen_suite >> 8) & 15]) +
                             std::string(1, "0123456789abcdef"[(chosen_suite >> 4) & 15]) +
                             std::string(1, "0123456789abcdef"[chosen_suite & 15]) + ")");
    info_.version = "TLS 1.2";
    info_.cipher = name12;
    s.gcm12 = gcm;
    // The PRF's hash: SHA-384 for the *_SHA384 suites (the AES-256 GCM ones and
    // AES_256_CBC_SHA384), SHA-256 for every other 1.2 suite - the *_CBC_SHA ones included.
    s.sha384 = cbc_mac ? prf384 : a256;
    s.send_keys.gcm = s.recv_keys.gcm = gcm;
    s.send_keys.cbc = s.recv_keys.cbc = cbc_mac != 0;
    s.send_keys.mac = s.recv_keys.mac = cbc_mac;
    s.send_keys.aes256 = s.recv_keys.aes256 = a256;
    ByteBuffer client_random_bytes(hello.begin() + 6, hello.begin() + 6 + 32);
    return handshake12(host_name, client_random_bytes, server_random, secret, p256_secret, *anchors, timeout_ms);
  }
  // TLS 1.3: the suite fixes the AEAD and the hash the key schedule runs on.
  switch (chosen_suite) {
    case 0x1303: info_.cipher = "TLS_CHACHA20_POLY1305_SHA256"; break;
    case 0x1301: info_.cipher = "TLS_AES_128_GCM_SHA256"; s.send_keys.gcm = s.recv_keys.gcm = true; break;
    case 0x1302:
      info_.cipher = "TLS_AES_256_GCM_SHA384";
      s.send_keys.gcm = s.recv_keys.gcm = true;
      s.send_keys.aes256 = s.recv_keys.aes256 = true;
      s.sha384 = true;
      break;
    default:
      return Result<void>(Status::Unsupported, "the server chose a TLS 1.3 cipher suite this machine did not offer");
  }
  s.send_keys.sha384 = s.recv_keys.sha384 = s.sha384;
  info_.version = "TLS 1.3";
  bool have_x25519 = false;
  for (uint8_t b : peer_share) if (b) have_x25519 = true;
  crypto::Point32 shared{};
  if (have_x25519) {
    if (!crypto::x25519(shared, secret, peer_share))
      return Result<void>(Status::Denied, "the server's key share has small order");
  } else if (!peer_p256.empty()) {
    auto x = crypto::ecdh_shared_x(crypto::NistCurve::P256, ByteSpan(p256_secret.data(), 32),
                                   ByteSpan(peer_p256.data(), peer_p256.size()));
    if (!x.ok()) return Result<void>(Status::Denied, "the server's secp256r1 share: " + x.error().str());
    std::memcpy(shared.data(), x->data(), 32);
  } else {
    return Result<void>(Status::Unsupported,
                       "the server agreed to neither x25519 nor secp256r1 (a HelloRetryRequest is not implemented here)");
  }

  // ---- the key schedule (RFC 8446 7.1), over the suite's hash ----
  const bool h384 = s.sha384;
  const size_t hl = h384 ? 48 : 32;
  auto bspan = [](const ByteBuffer& b) { return ByteSpan(b.data(), b.size()); };
  ByteBuffer zeros(hl, 0);
  auto early = psk_accepted ? hmac_h(h384, bspan(zeros), bspan(resume.psk)) : hmac_h(h384, bspan(zeros), bspan(zeros));
  auto empty_hash = hash_h(h384, ByteSpan());
  s.suite = chosen_suite;
  auto derived = expand_label_h(h384, bspan(early), "derived", bspan(empty_hash), hl);
  auto handshake = hmac_h(h384, bspan(derived), ByteSpan(shared.data(), 32));
  auto th = s.transcript_hash();
  auto c_hs = expand_label_h(h384, bspan(handshake), "c hs traffic", bspan(th), hl);
  auto s_hs = expand_label_h(h384, bspan(handshake), "s hs traffic", bspan(th), hl);
  s.recv_keys.set(ByteSpan(s_hs.data(), s_hs.size()));

  // ---- the server's half of the handshake ----
  ByteBuffer pending;
  ByteBuffer cert_verify;
  ByteBuffer server_finished;
  ByteBuffer th_before_cv, th_before_fin;
  bool have_certs = false, have_cv = false, done = false, cert_requested = false;
  uint16_t cv_alg = 0;
  ByteBuffer cert_req_ctx;
  std::vector<uint16_t> server_sig_algs;
  while (!done) {
    auto got = s.read_record(rtype, timeout_ms);
    if (!got.ok()) return Result<void>(got.error());
    if (rtype == REC_CHANGE_CIPHER_SPEC) continue;
    if (rtype == REC_ALERT) return Result<void>(Status::Denied, "the server sent an alert during the handshake");
    if (rtype != REC_HANDSHAKE) return Result<void>(Status::Corrupt, "unexpected record during the handshake");
    pending.insert(pending.end(), got->begin(), got->end());

    size_t at = 0;
    while (at + 4 <= pending.size()) {
      uint8_t ht = pending[at];
      size_t hlen = ((size_t)pending[at + 1] << 16) | ((size_t)pending[at + 2] << 8) | pending[at + 3];
      if (at + 4 + hlen > pending.size()) break;                 // wait for the rest
      ByteSpan msg(pending.data() + at, 4 + hlen);
      ByteSpan body_span(pending.data() + at + 4, hlen);
      if (ht == HS_CERTIFICATE_VERIFY) th_before_cv = s.transcript_hash();
      if (ht == HS_FINISHED) th_before_fin = s.transcript_hash();
      raw(s.transcript, msg);

      if (ht == HS_ENCRYPTED_EXTENSIONS) {
        // Nothing here changes what this machine does; ALPN is advisory.
      } else if (ht == 13) {                                      // CertificateRequest
        cert_requested = true;
        size_t p = 0;
        if (p >= body_span.size()) return Result<void>(Status::Corrupt, "empty CertificateRequest");
        const size_t cl = body_span[p++];
        if (p + cl + 2 > body_span.size()) return Result<void>(Status::Corrupt, "short CertificateRequest");
        cert_req_ctx.assign(body_span.begin() + (long)p, body_span.begin() + (long)(p + cl));
        p += cl;
        const size_t el = (size_t)((body_span[p] << 8) | body_span[p + 1]);
        p += 2;
        const size_t end = std::min(body_span.size(), p + el);
        while (p + 4 <= end) {
          const uint16_t et = (uint16_t)((body_span[p] << 8) | body_span[p + 1]);
          const size_t len = (size_t)((body_span[p + 2] << 8) | body_span[p + 3]);
          p += 4;
          if (p + len > end) break;
          if (et == 13 && len >= 2) {                                // signature_algorithms
            const size_t n = (size_t)((body_span[p] << 8) | body_span[p + 1]);
            for (size_t i = 0; i + 1 < n && p + 2 + i + 1 < end; i += 2)
              server_sig_algs.push_back((uint16_t)((body_span[p + 2 + i] << 8) | body_span[p + 2 + i + 1]));
          }
          p += len;
        }
      } else if (ht == HS_CERTIFICATE) {
        size_t p = 0;
        if (p >= body_span.size()) return Result<void>(Status::Corrupt, "empty Certificate message");
        p += 1 + body_span[0];                                    // request context
        if (p + 3 > body_span.size()) return Result<void>(Status::Corrupt, "short Certificate message");
        size_t list = ((size_t)body_span[p] << 16) | ((size_t)body_span[p + 1] << 8) | body_span[p + 2];
        p += 3;
        size_t end = p + list;
        while (p + 3 <= end && p + 3 <= body_span.size()) {
          size_t clen = ((size_t)body_span[p] << 16) | ((size_t)body_span[p + 1] << 8) | body_span[p + 2];
          p += 3;
          if (p + clen > body_span.size()) break;
          auto parsed = crypto::x509_parse(body_span.subspan(p, clen));
          if (!parsed.ok()) return Result<void>(parsed.error());
          s.chain.push_back(*parsed);
          p += clen;
          if (p + 2 > body_span.size()) break;
          size_t elen = (size_t)((body_span[p] << 8) | body_span[p + 1]);   // per-certificate extensions
          p += 2 + elen;
        }
        have_certs = true;
      } else if (ht == HS_CERTIFICATE_VERIFY) {
        if (body_span.size() < 4) return Result<void>(Status::Corrupt, "short CertificateVerify");
        cv_alg = (uint16_t)((body_span[0] << 8) | body_span[1]);
        size_t slen = (size_t)((body_span[2] << 8) | body_span[3]);
        if (4 + slen > body_span.size()) return Result<void>(Status::Corrupt, "short CertificateVerify signature");
        cert_verify.assign(body_span.begin() + 4, body_span.begin() + 4 + (long)slen);
        have_cv = true;
      } else if (ht == HS_FINISHED) {
        server_finished.assign(body_span.begin(), body_span.end());
        done = true;
      }
      at += 4 + hlen;
    }
    pending.erase(pending.begin(), pending.begin() + (long)at);
  }

  if (psk_accepted) {
    // The identity was proven in the session the ticket came from; the PSK is
    // the proof now (a server without it cannot produce a matching Finished).
    info_.resumed = true;
    info_.subject = resume.subject; info_.issuer = resume.issuer; info_.trust_anchor = resume.anchor;
  } else {
  if (!have_certs || s.chain.empty())
    return Result<void>(Status::Denied, "the server sent no certificate");
  if (!have_cv) return Result<void>(Status::Denied, "the server did not prove it holds the certificate's key");

  // ---- is this certificate's key the one that signed the handshake? ----
  {
    const crypto::Certificate& leaf = s.chain.front();
    if (!leaf.key_is_ec && !leaf.key_is_rsa)
      return Result<void>(Status::Unsupported,
                         "the server's certificate carries a key type this machine cannot check");
    ByteBuffer signed_data;
    for (int i = 0; i < 64; ++i) signed_data.push_back(0x20);
    raw(signed_data, std::string("TLS 1.3, server CertificateVerify"));
    signed_data.push_back(0);
    raw(signed_data, bspan(th_before_cv));

    bool ok = verify_handshake_signature(leaf, cv_alg, ByteSpan(signed_data.data(), signed_data.size()),
                                         ByteSpan(cert_verify.data(), cert_verify.size()), info_.peer_signature);
    if (!ok) {
      if (info_.peer_signature.empty())
        return Result<void>(Status::Unsupported,
                           "the server signed the handshake with an algorithm this machine did not offer");
      return Result<void>(Status::Denied, "the server's handshake signature does not verify against its certificate");
    }
  }

  // ---- and is that certificate one this machine has reason to trust? ----
  {
    auto verdict = crypto::x509_verify_chain(s.chain, *anchors, host_name, s.host.wall_ns());
    if (!verdict.trusted) return Result<void>(Status::Denied, verdict.why);
    info_.trust_anchor = verdict.anchor;
    info_.subject = s.chain.front().subject_cn;
    info_.issuer = s.chain.front().issuer_cn;
    info_.chain_length = s.chain.size();
  }
  }
  s.subject = info_.subject; s.issuer = info_.issuer; s.anchor = info_.trust_anchor;

  // ---- Finished, both ways ----
  {
    auto fin_key = expand_label_h(h384, bspan(s_hs), "finished", {}, hl);
    auto want = hmac_h(h384, bspan(fin_key), bspan(th_before_fin));
    if (server_finished.size() != hl || std::memcmp(want.data(), server_finished.data(), hl) != 0)
      return Result<void>(Status::Denied, "the server's Finished does not match the handshake this machine saw");
  }
  // The application traffic secrets are derived from the transcript through
  // the server's Finished (RFC 8446 7.1); our own Certificate, CertificateVerify
  // and Finished are not part of it.
  auto th_server_fin = s.transcript_hash();
  {
    // The change_cipher_spec a middlebox expects to see, then everything of
    // ours under the handshake keys: Certificate and CertificateVerify when
    // the server asked, then Finished.
    ByteBuffer ccs{0x01};
    s.send_keys.live = false;
    if (auto rr = s.send_record(REC_CHANGE_CIPHER_SPEC, ByteSpan(ccs.data(), 1)); !rr.ok()) return rr;
    s.send_keys.set(ByteSpan(c_hs.data(), c_hs.size()));
  }
  if (cert_requested) {
    ByteBuffer body;
    body.push_back((uint8_t)cert_req_ctx.size());
    raw(body, ByteSpan(cert_req_ctx.data(), cert_req_ctx.size()));
    ByteBuffer list;
    const uint16_t alg = client_sig_alg(s.client_key, server_sig_algs, true);
    const bool present = !s.client_chain.empty() && alg != 0;
    if (present)
      for (const auto& cert : s.client_chain) { be24(list, (uint32_t)cert.der.size()); raw(list, bspan(cert.der)); be16(list, 0); }
    be24(body, (uint32_t)list.size());
    raw(body, bspan(list));
    ByteBuffer msg;
    msg.push_back(HS_CERTIFICATE);
    be24(msg, (uint32_t)body.size());
    raw(msg, bspan(body));
    raw(s.transcript, bspan(msg));
    if (auto rr = s.send_record(REC_HANDSHAKE, bspan(msg)); !rr.ok()) return rr;
    if (present) {
      ByteBuffer content(64, 0x20);
      raw(content, std::string("TLS 1.3, client CertificateVerify"));
      content.push_back(0);
      auto th_cv = s.transcript_hash();
      raw(content, bspan(th_cv));
      auto sig = sign_with(s.client_key, alg, bspan(content));
      if (!sig.ok()) return Result<void>(sig.error());
      ByteBuffer cv;
      cv.push_back(HS_CERTIFICATE_VERIFY);
      be24(cv, (uint32_t)(4 + sig->size()));
      be16(cv, alg);
      be16(cv, (uint16_t)sig->size());
      raw(cv, bspan(*sig));
      raw(s.transcript, bspan(cv));
      if (auto rr = s.send_record(REC_HANDSHAKE, bspan(cv)); !rr.ok()) return rr;
      info_.client_certificate_sent = true;
    }
  }
  auto th_after = s.transcript_hash();
  {
    auto fin_key = expand_label_h(h384, bspan(c_hs), "finished", {}, hl);
    auto mac = hmac_h(h384, bspan(fin_key), bspan(th_after));
    ByteBuffer msg;
    msg.push_back(HS_FINISHED);
    be24(msg, (uint32_t)hl);
    raw(msg, bspan(mac));
    if (auto rr = s.send_record(REC_HANDSHAKE, ByteSpan(msg.data(), msg.size())); !rr.ok()) return rr;
    raw(s.transcript, bspan(msg));
  }

  // ---- application keys ----
  {
    auto derived2 = expand_label_h(h384, bspan(handshake), "derived", bspan(empty_hash), hl);
    auto master = hmac_h(h384, bspan(derived2), bspan(zeros));
    auto c_ap = expand_label_h(h384, bspan(master), "c ap traffic", bspan(th_server_fin), hl);
    auto s_ap = expand_label_h(h384, bspan(master), "s ap traffic", bspan(th_server_fin), hl);
    s.send_keys.set(ByteSpan(c_ap.data(), c_ap.size()));
    s.recv_keys.set(ByteSpan(s_ap.data(), s_ap.size()));
    // What a NewSessionTicket's PSK is derived from (RFC 8446 7.1, over the
    // transcript through our Finished).
    auto th_fin = s.transcript_hash();
    s.res_master = expand_label_h(h384, bspan(master), "res master", bspan(th_fin), hl);
  }
  closed_ = false;
  return Ok();
}


// ---------------------------------------------------------------------------
// TLS 1.2 (RFC 5246 + RFC 7905): ECDHE over x25519, ECDSA certificates, and
// ChaCha20-Poly1305 records. The version is older; the checking is not weaker —
// the same chain verification and the same refusal on anything unproven.
// ---------------------------------------------------------------------------
namespace {

// The TLS 1.2 PRF, P_SHA256.
ByteBuffer prf(ByteSpan secret, const std::string& label, ByteSpan seed, size_t len) {
  ByteBuffer full;
  full.insert(full.end(), label.begin(), label.end());
  full.insert(full.end(), seed.begin(), seed.end());
  ByteBuffer out;
  ByteBuffer a(full.begin(), full.end());
  while (out.size() < len) {
    auto ai = crypto::hmac_sha256(secret, ByteSpan(a.data(), a.size()));
    a.assign(ai.begin(), ai.end());
    ByteBuffer in(a.begin(), a.end());
    in.insert(in.end(), full.begin(), full.end());
    auto block = crypto::hmac_sha256(secret, ByteSpan(in.data(), in.size()));
    out.insert(out.end(), block.begin(), block.end());
  }
  out.resize(len);
  return out;
}

// The same PRF over the suite's hash: P_SHA384 for the *_SHA384 suites.
ByteBuffer prf_h(bool s384, ByteSpan secret, const std::string& label, ByteSpan seed, size_t len) {
  if (!s384) return prf(secret, label, seed, len);
  ByteBuffer full;
  full.insert(full.end(), label.begin(), label.end());
  full.insert(full.end(), seed.begin(), seed.end());
  ByteBuffer out;
  ByteBuffer a(full.begin(), full.end());
  while (out.size() < len) {
    a = hmac_h(true, secret, ByteSpan(a.data(), a.size()));
    ByteBuffer in(a.begin(), a.end());
    in.insert(in.end(), full.begin(), full.end());
    auto block = hmac_h(true, secret, ByteSpan(in.data(), in.size()));
    out.insert(out.end(), block.begin(), block.end());
  }
  out.resize(len);
  return out;
}

}  // namespace

Result<void> TlsClient::handshake12(const std::string& host_name, const ByteBuffer& client_random,
                                    const std::array<uint8_t, 32>& server_random,
                                    const crypto::Scalar32& our_secret, const ByteBuffer& p256_secret,
                                    const std::vector<crypto::Certificate>& anchors, int timeout_ms) {
  Impl& s = *impl_;
  // Everything after this point is plaintext until the change_cipher_spec, and
  // the handshake hash covers the messages, not the records that carried them.
  ByteBuffer pending;
  crypto::Point32 peer_pub{};
  ByteBuffer peer_p256;                                 // set when the server chose secp256r1
  bool have_ske = false, done = false, cert_requested = false;
  std::vector<uint16_t> server_sig_algs12;
  uint8_t rtype = 0;

  while (!done) {
    auto got = s.read_record(rtype, timeout_ms);
    if (!got.ok()) return Result<void>(got.error());
    if (rtype == REC_ALERT) return Result<void>(Status::Denied, "the server sent an alert during the handshake");
    if (rtype != REC_HANDSHAKE) continue;
    pending.insert(pending.end(), got->begin(), got->end());
    size_t at = 0;
    while (at + 4 <= pending.size()) {
      uint8_t ht = pending[at];
      size_t hlen = ((size_t)pending[at + 1] << 16) | ((size_t)pending[at + 2] << 8) | pending[at + 3];
      if (at + 4 + hlen > pending.size()) break;
      ByteSpan msg(pending.data() + at, 4 + hlen);
      ByteSpan body(pending.data() + at + 4, hlen);
      raw(s.transcript, msg);

      if (ht == HS_CERTIFICATE) {
        size_t p = 3;                                   // certificate_list length
        while (p + 3 <= body.size()) {
          size_t clen = ((size_t)body[p] << 16) | ((size_t)body[p + 1] << 8) | body[p + 2];
          p += 3;
          if (p + clen > body.size()) break;
          auto parsed = crypto::x509_parse(body.subspan(p, clen));
          if (!parsed.ok()) return Result<void>(parsed.error());
          s.chain.push_back(*parsed);
          p += clen;
        }
      } else if (ht == 12) {                            // ServerKeyExchange
        // struct { curve_type(1)=named_curve, namedcurve(2), public(1+n),
        //          signature_algorithm(2), signature(2+n) }
        if (body.size() < 4) return Result<void>(Status::Corrupt, "short ServerKeyExchange");
        if (body[0] != 0x03) return Result<void>(Status::Unsupported, "the server offered a curve form this machine does not use");
        uint16_t group = (uint16_t)((body[1] << 8) | body[2]);
        size_t plen = body[3];
        if (!((group == 0x001d && plen == 32) || (group == 0x0017 && plen == 65)))
          return Result<void>(Status::Unsupported, "the server agreed to neither x25519 nor secp256r1");
        if (4 + plen + 4 > body.size()) return Result<void>(Status::Corrupt, "short ServerKeyExchange");
        if (group == 0x001d) std::memcpy(peer_pub.data(), body.data() + 4, 32);
        else peer_p256.assign(body.begin() + 4, body.begin() + 4 + 65);
        size_t p = 4 + plen;
        uint16_t sig_alg = (uint16_t)((body[p] << 8) | body[p + 1]);
        p += 2;
        size_t slen = (size_t)((body[p] << 8) | body[p + 1]);
        p += 2;
        if (p + slen > body.size()) return Result<void>(Status::Corrupt, "short ServerKeyExchange signature");
        ByteSpan sig = body.subspan(p, slen);

        // What the server signed: both randoms, then the curve parameters
        // exactly as they appeared on the wire.
        ByteBuffer signed_data;
        raw(signed_data, ByteSpan(client_random.data(), client_random.size()));
        raw(signed_data, ByteSpan(server_random.data(), server_random.size()));
        raw(signed_data, body.subspan(0, 4 + plen));

        if (s.chain.empty()) return Result<void>(Status::Denied, "the server sent key exchange before its certificate");
        const crypto::Certificate& leaf = s.chain.front();
        if (!leaf.key_is_ec && !leaf.key_is_rsa)
          return Result<void>(Status::Unsupported,
                             "the server's certificate carries a key type this machine cannot check");
        bool ok = verify_handshake_signature(leaf, sig_alg, ByteSpan(signed_data.data(), signed_data.size()),
                                             sig, info_.peer_signature);
        if (!ok && info_.peer_signature.empty())
          return Result<void>(Status::Unsupported,
                             "the server signed its key exchange with an algorithm this machine did not offer");
        if (!ok) return Result<void>(Status::Denied, "the server's key exchange signature does not verify against its certificate");
        have_ske = true;
      } else if (ht == 13) {                            // CertificateRequest
        cert_requested = true;
        size_t p = 0;
        if (p < body.size()) p += 1 + body[p];          // certificate_types
        if (p + 2 <= body.size()) {                     // supported_signature_algorithms
          const size_t n = (size_t)((body[p] << 8) | body[p + 1]);
          p += 2;
          for (size_t i = 0; i + 1 < n && p + i + 1 < body.size(); i += 2)
            server_sig_algs12.push_back((uint16_t)((body[p + i] << 8) | body[p + i + 1]));
        }
      } else if (ht == 14) {                            // ServerHelloDone
        done = true;
      }
      at += 4 + hlen;
    }
    pending.erase(pending.begin(), pending.begin() + (long)at);
  }

  if (s.chain.empty()) return Result<void>(Status::Denied, "the server sent no certificate");
  if (!have_ske) return Result<void>(Status::Denied, "the server never proved it holds the certificate's key");
  {
    auto verdict = crypto::x509_verify_chain(s.chain, anchors, host_name, s.host.wall_ns());
    if (!verdict.trusted) return Result<void>(Status::Denied, verdict.why);
    info_.trust_anchor = verdict.anchor;
    info_.subject = s.chain.front().subject_cn;
    info_.issuer = s.chain.front().issuer_cn;
    info_.chain_length = s.chain.size();
  }

  // ---- our half: Certificate when asked, ClientKeyExchange, CertificateVerify ----
  const uint16_t cert_alg12 = cert_requested ? client_sig_alg(s.client_key, server_sig_algs12, false) : 0;
  const bool present12 = cert_requested && !s.client_chain.empty() && cert_alg12 != 0;
  if (cert_requested) {
    ByteBuffer list;
    if (present12)
      for (const auto& cert : s.client_chain) { be24(list, (uint32_t)cert.der.size()); raw(list, ByteSpan(cert.der.data(), cert.der.size())); }
    ByteBuffer msg;
    msg.push_back(HS_CERTIFICATE);
    be24(msg, (uint32_t)(3 + list.size()));
    be24(msg, (uint32_t)list.size());
    raw(msg, ByteSpan(list.data(), list.size()));
    raw(s.transcript, ByteSpan(msg.data(), msg.size()));
    if (auto r = s.send_record(REC_HANDSHAKE, ByteSpan(msg.data(), msg.size())); !r.ok()) return r;
  }
  crypto::Point32 pre_master{};
  ByteBuffer our_point;
  if (peer_p256.empty()) {
    crypto::Point32 our_pub = crypto::x25519_base(our_secret);
    our_point.assign(our_pub.begin(), our_pub.end());
    if (!crypto::x25519(pre_master, our_secret, peer_pub))
      return Result<void>(Status::Denied, "the server's key share has small order");
  } else {
    auto pub = crypto::ecdsa_public_key(crypto::NistCurve::P256, ByteSpan(p256_secret.data(), 32));
    auto x = crypto::ecdh_shared_x(crypto::NistCurve::P256, ByteSpan(p256_secret.data(), 32),
                                   ByteSpan(peer_p256.data(), peer_p256.size()));
    if (!pub.ok() || !x.ok()) return Result<void>(Status::Denied, "the server's secp256r1 share is not usable");
    our_point = std::move(*pub);
    std::memcpy(pre_master.data(), x->data(), 32);
  }
  {
    ByteBuffer cke;
    cke.push_back(16);                                  // ClientKeyExchange
    be24(cke, (uint32_t)(1 + our_point.size()));
    cke.push_back((uint8_t)our_point.size());
    raw(cke, ByteSpan(our_point.data(), our_point.size()));
    raw(s.transcript, ByteSpan(cke.data(), cke.size()));
    if (auto r = s.send_record(REC_HANDSHAKE, ByteSpan(cke.data(), cke.size())); !r.ok()) return r;
  }
  if (present12) {
    // CertificateVerify: a signature over every handshake message so far (RFC 5246 7.4.8).
    auto sig = sign_with(s.client_key, cert_alg12, ByteSpan(s.transcript.data(), s.transcript.size()));
    if (!sig.ok()) return Result<void>(sig.error());
    ByteBuffer cv;
    cv.push_back(HS_CERTIFICATE_VERIFY);
    be24(cv, (uint32_t)(4 + sig->size()));
    be16(cv, cert_alg12);
    be16(cv, (uint16_t)sig->size());
    raw(cv, ByteSpan(sig->data(), sig->size()));
    raw(s.transcript, ByteSpan(cv.data(), cv.size()));
    if (auto r = s.send_record(REC_HANDSHAKE, ByteSpan(cv.data(), cv.size())); !r.ok()) return r;
    info_.client_certificate_sent = true;
  }

  ByteBuffer seed;
  raw(seed, ByteSpan(client_random.data(), client_random.size()));
  raw(seed, ByteSpan(server_random.data(), server_random.size()));
  const bool h384 = s.sha384;
  auto master = prf_h(h384, ByteSpan(pre_master.data(), 32), "master secret", ByteSpan(seed.data(), seed.size()), 48);

  ByteBuffer key_seed;
  raw(key_seed, ByteSpan(server_random.data(), server_random.size()));
  raw(key_seed, ByteSpan(client_random.data(), client_random.size()));
  // client_write_key | server_write_key | client_write_IV | server_write_IV.
  // ChaCha20: 32-byte keys, 12-byte IVs. AES-GCM: 16/32-byte keys and a
  // 4-byte implicit salt; the other 8 nonce bytes ride in each record.
  const bool gcm = s.gcm12;
  if (s.send_keys.cbc) {
    // CBC: client_write_MAC_key | server_write_MAC_key | client_write_key | server_write_key (no IVs: each record carries one).
    const size_t ml = mac_len(s.send_keys.mac), kl = s.send_keys.aes256 ? 32 : 16;
    auto kb = prf_h(h384, ByteSpan(master.data(), master.size()), "key expansion", ByteSpan(key_seed.data(), key_seed.size()), 2 * (ml + kl));
    s.send_keys.key.fill(0);
    s.recv_keys.key.fill(0);
    std::memcpy(s.send_keys.mac_key.data(), kb.data(), ml);
    std::memcpy(s.recv_keys.mac_key.data(), kb.data() + ml, ml);
    std::memcpy(s.send_keys.key.data(), kb.data() + 2 * ml, kl);
    std::memcpy(s.recv_keys.key.data(), kb.data() + 2 * ml + kl, kl);
  }
  const size_t klen = gcm && !s.send_keys.aes256 ? 16 : 32, ivlen = gcm ? 4 : 12;
  auto block = prf_h(h384, ByteSpan(master.data(), master.size()), "key expansion",
                     ByteSpan(key_seed.data(), key_seed.size()), 2 * (klen + ivlen));
  if (!s.send_keys.cbc) {
  s.send_keys.key.fill(0);
  s.recv_keys.key.fill(0);
  s.send_keys.iv.fill(0);
  s.recv_keys.iv.fill(0);
  std::memcpy(s.send_keys.key.data(), block.data(), klen);
  std::memcpy(s.recv_keys.key.data(), block.data() + klen, klen);
  std::memcpy(s.send_keys.iv.data(), block.data() + 2 * klen, ivlen);
  std::memcpy(s.recv_keys.iv.data(), block.data() + 2 * klen + ivlen, ivlen);
  }
  s.send_keys.seq = s.recv_keys.seq = 0;
  s.tls12 = true;

  {                                                     // ChangeCipherSpec, then Finished under the new keys
    ByteBuffer ccs{0x01};
    if (auto r = s.send_record(REC_CHANGE_CIPHER_SPEC, ByteSpan(ccs.data(), 1)); !r.ok()) return r;
    s.send_keys.live = true;
    auto hh = s.transcript_hash();
    auto verify = prf_h(h384, ByteSpan(master.data(), master.size()), "client finished", ByteSpan(hh.data(), hh.size()), 12);
    ByteBuffer fin;
    fin.push_back(HS_FINISHED);
    be24(fin, 12);
    raw(fin, ByteSpan(verify.data(), verify.size()));
    raw(s.transcript, ByteSpan(fin.data(), fin.size()));
    if (auto r = s.send_record(REC_HANDSHAKE, ByteSpan(fin.data(), fin.size())); !r.ok()) return r;
  }

  {                                                     // and the server's, which must match what we saw
    auto expect_hash = s.transcript_hash();
    bool seen_ccs = false;
    for (;;) {
      auto got = s.read_record(rtype, timeout_ms);
      if (!got.ok()) return Result<void>(got.error());
      if (rtype == REC_CHANGE_CIPHER_SPEC) { s.recv_keys.live = true; seen_ccs = true; continue; }
      if (rtype == REC_ALERT) return Result<void>(Status::Denied, "the server rejected our Finished");
      if (rtype != REC_HANDSHAKE || !seen_ccs) continue;
      if (got->size() < 16 || (*got)[0] != HS_FINISHED)
        return Result<void>(Status::Corrupt, "the server did not send a Finished");
      auto want = prf_h(h384, ByteSpan(master.data(), master.size()), "server finished",
                        ByteSpan(expect_hash.data(), expect_hash.size()), 12);
      if (std::memcmp(want.data(), got->data() + 4, 12) != 0)
        return Result<void>(Status::Denied, "the server's Finished does not match the handshake this machine saw");
      break;
    }
  }
  closed_ = false;
  return Ok();
}

Result<void> TlsClient::write(ByteSpan data) {
  if (!impl_ || !impl_->conn) return Result<void>(Status::IoError, "not connected");
  size_t off = 0;
  while (off < data.size()) {
    size_t n = data.size() - off;
    if (n > 16384) n = 16384;                    // one record's worth
    if (auto r = impl_->send_record(REC_APPLICATION, data.subspan(off, n)); !r.ok()) return r;
    info_.bytes_out += n;
    off += n;
  }
  return Ok();
}

Result<ByteBuffer> TlsClient::read(int timeout_ms) {
  if (!impl_ || !impl_->conn) return Result<ByteBuffer>(Status::IoError, "not connected");
  if (!impl_->inbox.empty()) {
    ByteBuffer out;
    out.swap(impl_->inbox);
    return out;
  }
  for (;;) {
    uint8_t type = 0;
    auto rec = impl_->read_record(type, timeout_ms);
    if (!rec.ok()) {
      if (rec.status() == Status::IoError) { closed_ = true; return ByteBuffer{}; }   // clean close
      return rec;
    }
    if (type == REC_APPLICATION) {
      info_.bytes_in += rec->size();
      return rec;
    }
    if (type == REC_ALERT) {
      // close_notify is how a server says it is done; anything else is a fault,
      // named so the reason is in the message.
      closed_ = true;
      if (rec->size() >= 2 && (*rec)[1] == 0) return ByteBuffer{};
      const int code = rec->size() >= 2 ? (*rec)[1] : -1;
      if (code == 116)
        return Result<ByteBuffer>(Status::Denied, "the server requires a client certificate and this machine presented none (put client.crt and client.key beside its trust store)");
      static const struct { int c; const char* t; } kNames[] = {{40, "handshake_failure"}, {42, "bad_certificate"}, {43, "unsupported_certificate"},
          {44, "certificate_revoked"}, {45, "certificate_expired"}, {46, "certificate_unknown"}, {47, "illegal_parameter"}, {48, "unknown_ca"},
          {49, "access_denied"}, {50, "decode_error"}, {51, "decrypt_error"}, {70, "protocol_version"}, {80, "internal_error"}, {109, "missing_extension"}};
      std::string name = "alert " + std::to_string(code);
      for (auto& n : kNames) if (n.c == code) name = n.t;
      return Result<ByteBuffer>(Status::Denied, "the server sent " + name + " (" + std::to_string(code) + ")");
    }
    if (type == REC_HANDSHAKE && !impl_->tls12 && !impl_->res_master.empty()) {
      // NewSessionTicket (RFC 8446 4.6.1): kept for the next connection to this
      // server in this process. A KeyUpdate is not handled here.
      size_t at = 0;
      while (at + 4 <= rec->size()) {
        const uint8_t ht = (*rec)[at];
        const size_t hlen = ((size_t)(*rec)[at + 1] << 16) | ((size_t)(*rec)[at + 2] << 8) | (*rec)[at + 3];
        if (at + 4 + hlen > rec->size()) break;
        const uint8_t* b = rec->data() + at + 4;
        if (ht == HS_NEW_SESSION_TICKET && hlen >= 10) {
          Ticket t;
          t.lifetime_s = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
          t.age_add = ((uint32_t)b[4] << 24) | ((uint32_t)b[5] << 16) | ((uint32_t)b[6] << 8) | b[7];
          size_t p = 8;
          const size_t nl = b[p++];
          if (p + nl + 2 <= hlen) {
            ByteBuffer nonce(b + p, b + p + nl);
            p += nl;
            const size_t tl = (size_t)((b[p] << 8) | b[p + 1]);
            p += 2;
            if (p + tl <= hlen && tl > 0) {
              t.ticket.assign(b + p, b + p + tl);
              const size_t hl = impl_->sha384 ? 48 : 32;
              t.psk = expand_label_h(impl_->sha384, ByteSpan(impl_->res_master.data(), impl_->res_master.size()), "resumption",
                                     ByteSpan(nonce.data(), nonce.size()), hl);
              t.received_ms = impl_->host.monotonic_ns() / 1000000ull;
              t.sha384 = impl_->sha384; t.suite = impl_->suite;
              t.subject = impl_->subject; t.issuer = impl_->issuer; t.anchor = impl_->anchor;
              if (t.lifetime_s > 0) {
                std::lock_guard<std::mutex> lk(ticket_mutex());
                ticket_cache()[impl_->ticket_key] = std::move(t);
              }
            }
          }
        }
        at += 4 + hlen;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// TLS server: 1.3 (RFC 8446) preferred, 1.2 (RFC 5246/5288/7627/7905) for
// clients that cannot do 1.3. x25519 key exchange; ChaCha20-Poly1305 or
// AES-128-GCM; RSA or ECDSA certificates. No resumption, no client certs.
// ---------------------------------------------------------------------------
namespace {

enum : uint8_t {
  ALERT_CLOSE_NOTIFY = 0,
  ALERT_UNEXPECTED_MESSAGE = 10,
  ALERT_BAD_RECORD_MAC = 20,
  ALERT_HANDSHAKE_FAILURE = 40,
  ALERT_ILLEGAL_PARAMETER = 47,
  ALERT_DECODE_ERROR = 50,
  ALERT_DECRYPT_ERROR = 51,
  ALERT_PROTOCOL_VERSION = 70,
  ALERT_INTERNAL_ERROR = 80,
  ALERT_MISSING_EXTENSION = 109,
};

enum : uint8_t {
  HS_SERVER_KEY_EXCHANGE = 12,
  HS_SERVER_HELLO_DONE = 14,
  HS_CLIENT_KEY_EXCHANGE = 16,
  HS_KEY_UPDATE = 24,
  HS_MESSAGE_HASH = 254,
};

enum : uint16_t {
  TLS13_AES_128_GCM_SHA256 = 0x1301,
  TLS13_CHACHA20_POLY1305_SHA256 = 0x1303,
  ECDHE_ECDSA_AES_128_GCM_SHA256 = 0xc02b,
  ECDHE_RSA_AES_128_GCM_SHA256 = 0xc02f,
  ECDHE_RSA_CHACHA20_POLY1305 = 0xcca8,
  ECDHE_ECDSA_CHACHA20_POLY1305 = 0xcca9,
  GROUP_X25519 = 0x001d,
  GROUP_SECP256R1 = 0x0017,
};

// SHA-256("HelloRetryRequest"): the ServerHello.random that marks an HRR.
constexpr uint8_t HRR_RANDOM[32] = {0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C,
                                    0x02, 0x1E, 0x65, 0xB8, 0x91, 0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB,
                                    0x8C, 0x5E, 0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C};

struct HandshakeFailure {
  uint8_t alert;
  std::string why;
};

struct ClientHelloInfo {
  ByteBuffer random, session_id;
  std::vector<uint16_t> suites, sig_algs, versions;
  bool group_x25519 = false;
  bool has_x25519_share = false;
  crypto::Point32 x25519_share{};
  // secp256r1, for clients that will not do x25519 (MySQL 8's libmysqlclient
  // restricts its groups to the NIST curves).
  bool group_p256 = false;
  bool has_p256_share = false;
  ByteBuffer p256_share;   // uncompressed point, 65 bytes
  bool ems = false, alpn_http11 = false, point_formats = false, secure_reneg = false;
  bool offers(uint16_t suite) const {
    for (auto s : suites) if (s == suite) return true;
    return false;
  }
  bool offers_version(uint16_t v) const {
    for (auto x : versions) if (x == v) return true;
    return false;
  }
  bool offers_sig(uint16_t a) const {
    for (auto x : sig_algs) if (x == a) return true;
    return false;
  }
};

uint16_t get16(ByteSpan b, size_t p) { return (uint16_t)((b[p] << 8) | b[p + 1]); }

// `msg` is the whole handshake message, 4-byte header included.
bool parse_client_hello(ByteSpan msg, ClientHelloInfo& ch, std::string& why) {
  size_t p = 4, end = msg.size();
  auto need = [&](size_t n) { return p + n <= end; };
  if (!need(2 + 32 + 1)) { why = "short ClientHello"; return false; }
  uint16_t legacy_version = get16(msg, p); p += 2;
  ch.random.assign(msg.begin() + (long)p, msg.begin() + (long)(p + 32)); p += 32;
  size_t sid = msg[p++];
  if (sid > 32 || !need(sid)) { why = "malformed session id"; return false; }
  ch.session_id.assign(msg.begin() + (long)p, msg.begin() + (long)(p + sid)); p += sid;
  if (!need(2)) { why = "missing cipher suites"; return false; }
  size_t cs = get16(msg, p); p += 2;
  if (cs % 2 || !need(cs)) { why = "malformed cipher suites"; return false; }
  for (size_t i = 0; i < cs; i += 2) {
    uint16_t s = get16(msg, p + i);
    if (s == 0x00ff) ch.secure_reneg = true;     // TLS_EMPTY_RENEGOTIATION_INFO_SCSV
    ch.suites.push_back(s);
  }
  p += cs;
  if (!need(1)) { why = "missing compression methods"; return false; }
  size_t comp = msg[p++];
  if (!need(comp)) { why = "malformed compression methods"; return false; }
  p += comp;
  bool saw_versions = false;
  if (need(2)) {
    size_t ext_total = get16(msg, p); p += 2;
    if (!need(ext_total)) { why = "malformed extensions"; return false; }
    size_t ext_end = p + ext_total;
    while (p + 4 <= ext_end) {
      uint16_t type = get16(msg, p), len = get16(msg, p + 2);
      p += 4;
      if (p + len > ext_end) { why = "malformed extension"; return false; }
      ByteSpan e = msg.subspan(p, len);
      switch (type) {
        case 10:   // supported_groups
          for (size_t i = 2; e.size() >= 2 && i + 2 <= e.size() && i < 2 + size_t(get16(e, 0)); i += 2)
            if (get16(e, i) == GROUP_X25519) ch.group_x25519 = true;
            else if (get16(e, i) == GROUP_SECP256R1) ch.group_p256 = true;
          break;
        case 11: ch.point_formats = true; break;
        case 13:   // signature_algorithms
          for (size_t i = 2; e.size() >= 2 && i + 2 <= e.size() && i < 2 + size_t(get16(e, 0)); i += 2)
            ch.sig_algs.push_back(get16(e, i));
          break;
        case 16:   // ALPN
          for (size_t i = 2; e.size() >= 2 && i < e.size() && i < 2 + size_t(get16(e, 0));) {
            size_t n = e[i++];
            if (i + n > e.size()) break;
            if (n == 8 && std::memcmp(e.data() + i, "http/1.1", 8) == 0) ch.alpn_http11 = true;
            i += n;
          }
          break;
        case 23: ch.ems = true; break;
        case 43:   // supported_versions
          saw_versions = true;
          for (size_t i = 1; !e.empty() && i + 2 <= e.size() && i < 1 + size_t(e[0]); i += 2) ch.versions.push_back(get16(e, i));
          break;
        case 51:   // key_share
          for (size_t i = 2; e.size() >= 2 && i + 4 <= e.size() && i < 2 + size_t(get16(e, 0));) {
            uint16_t g = get16(e, i), kl = get16(e, i + 2);
            i += 4;
            if (i + kl > e.size()) break;
            if (g == GROUP_X25519 && kl == 32 && !ch.has_x25519_share) {
              ch.has_x25519_share = true;
              std::memcpy(ch.x25519_share.data(), e.data() + i, 32);
            } else if (g == GROUP_SECP256R1 && kl == 65 && !ch.has_p256_share) {
              ch.has_p256_share = true;
              ch.p256_share.assign(e.data() + i, e.data() + i + 65);
            }
            i += kl;
          }
          break;
        case 0xff01: ch.secure_reneg = true; break;
        default: break;
      }
      p += len;
    }
  }
  // Without supported_versions, legacy_version is the client's highest (RFC 8446 4.2.1).
  if (!saw_versions && legacy_version >= 0x0303) ch.versions.push_back(0x0303);
  return true;
}

void ext_u16(ByteBuffer& out, uint16_t type, uint16_t value) {
  be16(out, type);
  be16(out, 2);
  be16(out, value);
}

void ext_alpn_http11(ByteBuffer& out) {
  be16(out, 16);
  be16(out, 11);
  be16(out, 9);
  out.push_back(8);
  raw(out, std::string("http/1.1"));
}

ByteBuffer handshake_msg(uint8_t type, ByteSpan body) {
  ByteBuffer m;
  m.push_back(type);
  be24(m, (uint32_t)body.size());
  raw(m, body);
  return m;
}

const char* suite_name(uint16_t s) {
  switch (s) {
    case TLS13_AES_128_GCM_SHA256: return "TLS_AES_128_GCM_SHA256";
    case TLS13_CHACHA20_POLY1305_SHA256: return "TLS_CHACHA20_POLY1305_SHA256";
    case ECDHE_ECDSA_AES_128_GCM_SHA256: return "ECDHE_ECDSA_WITH_AES_128_GCM_SHA256";
    case ECDHE_RSA_AES_128_GCM_SHA256: return "ECDHE_RSA_WITH_AES_128_GCM_SHA256";
    case ECDHE_RSA_CHACHA20_POLY1305: return "ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256";
    case ECDHE_ECDSA_CHACHA20_POLY1305: return "ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256";
    default: return "unknown";
  }
}

}  // namespace

struct TlsServer::Impl {
  hal::Host& host;
  std::vector<crypto::Certificate> chain;
  crypto::PrivateKey key;
  std::unique_ptr<hal::Connection> conn;
  ByteBuffer inbox;
  Keys send_keys, recv_keys;
  ByteBuffer transcript;
  ByteBuffer hs_buf;              // handshake bytes read but not yet consumed as whole messages
  bool tls12 = false;
  bool gcm12 = false;             // 1.2 AES-GCM: 4-byte salt + 8-byte explicit nonce per record
  bool used_p256 = false;         // the key exchange was secp256r1, not x25519
  bool p256_keypair(ByteBuffer& d, ByteBuffer* pub);

  Impl(hal::Host& h, std::vector<crypto::Certificate> c, crypto::PrivateKey k)
      : host(h), chain(std::move(c)), key(std::move(k)) {}

  Hash transcript_hash() const { return crypto::sha256(ByteSpan(transcript.data(), transcript.size())); }

  Result<void> send_record(uint8_t type, ByteSpan payload) {
    ByteBuffer wire;
    if (!send_keys.live || (tls12 && type == REC_CHANGE_CIPHER_SPEC)) {
      wire.push_back(type);
      be16(wire, 0x0303);
      be16(wire, (uint16_t)payload.size());
      raw(wire, payload);
    } else if (tls12) {
      ByteBuffer aad;
      for (int i = 7; i >= 0; --i) aad.push_back((uint8_t)(send_keys.seq >> (8 * i)));
      aad.push_back(type);
      be16(aad, 0x0303);
      be16(aad, (uint16_t)payload.size());
      auto sealed = aead_seal(send_keys, ByteSpan(aad.data(), aad.size()), payload);
      wire.push_back(type);
      be16(wire, 0x0303);
      be16(wire, (uint16_t)(sealed.size() + (gcm12 ? 8 : 0)));
      if (gcm12) raw(wire, ByteSpan(aad.data(), 8));   // explicit nonce = sequence number
      raw(wire, ByteSpan(sealed.data(), sealed.size()));
      ++send_keys.seq;
    } else {
      ByteBuffer inner(payload.begin(), payload.end());
      inner.push_back(type);
      ByteBuffer header;
      header.push_back(REC_APPLICATION);
      be16(header, 0x0303);
      be16(header, (uint16_t)(inner.size() + 16));
      auto sealed = aead_seal(send_keys, ByteSpan(header.data(), header.size()), ByteSpan(inner.data(), inner.size()));
      ++send_keys.seq;
      wire = header;
      raw(wire, ByteSpan(sealed.data(), sealed.size()));
    }
    if (!conn->send_all(ByteSpan(wire.data(), wire.size())))
      return Result<void>(Status::IoError, "the connection dropped while sending");
    return Ok();
  }

  void send_alert(uint8_t desc) {
    uint8_t a[2] = {uint8_t(desc == ALERT_CLOSE_NOTIFY ? 1 : 2), desc};
    (void)send_record(REC_ALERT, ByteSpan(a, 2));
  }

  Result<ByteBuffer> read_exact(size_t n, int timeout_ms) {
    ByteBuffer out(n);
    if (n == 0) return out;
    if (!conn->recv_all(MutableByteSpan(out.data(), n), timeout_ms)) {
      if (!conn->alive()) return Result<ByteBuffer>(Status::IoError, "the peer closed the connection");
      return Result<ByteBuffer>(Status::Busy, "the peer sent nothing in time");
    }
    return out;
  }

  Result<ByteBuffer> read_record(uint8_t& type, int timeout_ms) {
    auto head = read_exact(5, timeout_ms);
    if (!head.ok()) return head;
    uint8_t rtype = (*head)[0];
    size_t len = ((size_t)(*head)[3] << 8) | (*head)[4];
    if (len > 16384 + 2048) return Result<ByteBuffer>(Status::Corrupt, "implausible record length");
    auto body = read_exact(len, timeout_ms);
    if (!body.ok()) return body;
    if (rtype == REC_CHANGE_CIPHER_SPEC) { type = rtype; return *body; }
    if (!recv_keys.live) { type = rtype; return *body; }
    if (tls12) {
      size_t explicit_len = gcm12 ? 8 : 0;
      if (body->size() < explicit_len + 16) return Result<ByteBuffer>(Status::Corrupt, "record too short to hold a tag");
      crypto::Nonce n = recv_keys.nonce();
      if (gcm12) std::memcpy(n.data() + 4, body->data(), 8);
      ByteBuffer aad;
      for (int i = 7; i >= 0; --i) aad.push_back((uint8_t)(recv_keys.seq >> (8 * i)));
      aad.push_back(rtype);
      be16(aad, 0x0303);
      be16(aad, (uint16_t)(body->size() - explicit_len - 16));
      auto pt = aead_open_with(recv_keys, n, ByteSpan(aad.data(), aad.size()),
                               ByteSpan(body->data() + explicit_len, body->size() - explicit_len));
      if (!pt.ok()) return pt;
      ++recv_keys.seq;
      type = rtype;
      return pt;
    }
    if (rtype != REC_APPLICATION) { type = rtype; return *body; }
    auto pt = aead_open(recv_keys, ByteSpan(head->data(), 5), ByteSpan(body->data(), body->size()));
    if (!pt.ok()) return pt;
    ++recv_keys.seq;
    ByteBuffer inner = std::move(*pt);
    while (!inner.empty() && inner.back() == 0) inner.pop_back();
    if (inner.empty()) return Result<ByteBuffer>(Status::Corrupt, "a record with no content type");
    type = inner.back();
    inner.pop_back();
    return inner;
  }

  // The next whole handshake message (header included), reassembled across
  // records. 1.3's compatibility ChangeCipherSpec is skipped; a 1.2 CCS turns
  // on the receive keys and is reported through `saw_ccs`.
  Result<ByteBuffer> next_handshake(int timeout_ms, bool* saw_ccs = nullptr) {
    for (;;) {
      if (hs_buf.size() >= 4) {
        size_t len = ((size_t)hs_buf[1] << 16) | ((size_t)hs_buf[2] << 8) | hs_buf[3];
        if (len > 65536) return Result<ByteBuffer>(Status::Corrupt, "handshake message too large");
        if (hs_buf.size() >= 4 + len) {
          ByteBuffer msg(hs_buf.begin(), hs_buf.begin() + (long)(4 + len));
          hs_buf.erase(hs_buf.begin(), hs_buf.begin() + (long)(4 + len));
          return msg;
        }
      }
      uint8_t rtype = 0;
      auto rec = read_record(rtype, timeout_ms);
      if (!rec.ok()) return rec;
      if (rtype == REC_HANDSHAKE) { raw(hs_buf, ByteSpan(rec->data(), rec->size())); continue; }
      if (rtype == REC_CHANGE_CIPHER_SPEC) {
        if (rec->size() != 1 || (*rec)[0] != 1 || !hs_buf.empty())
          return Result<ByteBuffer>(Status::Corrupt, "malformed ChangeCipherSpec");
        if (tls12) {
          if (!saw_ccs) return Result<ByteBuffer>(Status::Corrupt, "unexpected ChangeCipherSpec");
          recv_keys.live = true;
          *saw_ccs = true;
        }
        continue;
      }
      if (rtype == REC_ALERT) {
        std::string why = "the client aborted the handshake";
        if (rec->size() >= 2) why += " (alert " + std::to_string((*rec)[1]) + ")";
        return Result<ByteBuffer>(Status::Denied, why);
      }
      return Result<ByteBuffer>(Status::Corrupt, "unexpected record during handshake");
    }
  }

  // Signs for ServerKeyExchange (1.2) or CertificateVerify (1.3).
  Result<ByteBuffer> sign(uint16_t alg, ByteSpan data) {
    if (key.is_ec) {
      ByteBuffer h = ec_digest(key.ec_key.curve, data);
      return crypto::ecdsa_sign(key.ec_key.curve, ByteSpan(key.ec_key.d.data(), key.ec_key.d.size()), ByteSpan(h.data(), h.size()));
    }
    if (key.is_rsa) {
      if (alg == 0x0804) return crypto::rsa_sign_pss(key.rsa_key, crypto::RsaDigest::Sha256, data);
      return crypto::rsa_sign_pkcs1(key.rsa_key, crypto::RsaDigest::Sha256, data);
    }
    return Result<ByteBuffer>(Status::Unsupported, "no supported key type for handshake signing");
  }

  // The one algorithm this key can actually produce that the client accepts;
  // 0 when there is none. 1.3 forbids PKCS#1 v1.5 for handshake signatures.
  uint16_t pick_sig_alg(const ClientHelloInfo& ch, bool tls13) const {
    if (key.is_ec) {
      uint16_t want = ec_sig_alg(key.ec_key.curve);
      if (ch.offers_sig(want)) return want;
      if (!tls13 && ch.sig_algs.empty()) return want;
      return 0;
    }
    if (key.is_rsa) {
      if (ch.offers_sig(0x0804)) return 0x0804;
      if (tls13) return 0;
      if (ch.offers_sig(0x0401) || ch.sig_algs.empty()) return 0x0401;
    }
    return 0;
  }

  Result<HandshakeFailure> handshake13(ClientHelloInfo ch, ByteBuffer ch_msg, uint16_t suite, uint16_t sig_alg,
                                       TlsInfo& info, int timeout_ms);
  Result<HandshakeFailure> handshake12(const ClientHelloInfo& ch, uint16_t suite, uint16_t sig_alg, TlsInfo& info,
                                       int timeout_ms);
};

namespace {
Result<HandshakeFailure> fail(uint8_t alert, std::string why) { return HandshakeFailure{alert, std::move(why)}; }
Result<HandshakeFailure> done() { return HandshakeFailure{0, {}}; }
}  // namespace

// A fresh secp256r1 key: the scalar d (32 bytes, below the group order - a
// random value is retried, which almost never happens) and its public point.
bool TlsServer::Impl::p256_keypair(ByteBuffer& d, ByteBuffer* pub) {
  for (int tries = 0; tries < 8; ++tries) {
    host.random(MutableByteSpan(d.data(), d.size()));
    auto p = crypto::ecdsa_public_key(crypto::NistCurve::P256, ByteSpan(d.data(), 32));
    if (p.ok() && p->size() == 65) { *pub = std::move(*p); return true; }
  }
  return false;
}

Result<HandshakeFailure> TlsServer::Impl::handshake13(ClientHelloInfo ch, ByteBuffer ch_msg, uint16_t suite,
                                                      uint16_t sig_alg, TlsInfo& info, int timeout_ms) {
  raw(transcript, ByteSpan(ch_msg.data(), ch_msg.size()));
  bool compat_ccs_sent = false;
  auto compat_ccs = [&]() -> Result<void> {
    if (compat_ccs_sent || ch.session_id.empty()) return Ok();
    compat_ccs_sent = true;
    uint8_t one = 1;
    return send_record(REC_CHANGE_CIPHER_SPEC, ByteSpan(&one, 1));
  };

  // x25519 when the client sent a share for it, else secp256r1's share;
  // with neither share, a HelloRetryRequest for the group it lists.
  bool use_p256 = !ch.has_x25519_share && (ch.has_p256_share || !ch.group_x25519);
  if (!(use_p256 ? ch.has_p256_share : ch.has_x25519_share)) {
    // HelloRetryRequest: the client can do the group but did not send a share for it.
    auto h1 = transcript_hash();
    transcript.clear();
    transcript.push_back(HS_MESSAGE_HASH);
    be24(transcript, 32);
    raw(transcript, ByteSpan(h1.data(), h1.size()));
    ByteBuffer body;
    be16(body, 0x0303);
    raw(body, ByteSpan(HRR_RANDOM, 32));
    body.push_back((uint8_t)ch.session_id.size());
    raw(body, ByteSpan(ch.session_id.data(), ch.session_id.size()));
    be16(body, suite);
    body.push_back(0);
    ByteBuffer ext;
    ext_u16(ext, 43, 0x0304);
    ext_u16(ext, 51, use_p256 ? GROUP_SECP256R1 : GROUP_X25519);
    be16(body, (uint16_t)ext.size());
    raw(body, ByteSpan(ext.data(), ext.size()));
    auto hrr = handshake_msg(HS_SERVER_HELLO, ByteSpan(body.data(), body.size()));
    raw(transcript, ByteSpan(hrr.data(), hrr.size()));
    if (auto r = send_record(REC_HANDSHAKE, ByteSpan(hrr.data(), hrr.size())); !r.ok()) return Result<HandshakeFailure>(r.error());
    if (auto r = compat_ccs(); !r.ok()) return Result<HandshakeFailure>(r.error());

    auto ch2_msg = next_handshake(timeout_ms);
    if (!ch2_msg.ok()) return Result<HandshakeFailure>(ch2_msg.error());
    if ((*ch2_msg)[0] != HS_CLIENT_HELLO) return fail(ALERT_UNEXPECTED_MESSAGE, "expected a second ClientHello after HelloRetryRequest");
    ClientHelloInfo ch2;
    std::string why;
    if (!parse_client_hello(ByteSpan(ch2_msg->data(), ch2_msg->size()), ch2, why)) return fail(ALERT_DECODE_ERROR, why);
    if (!(use_p256 ? ch2.has_p256_share : ch2.has_x25519_share) || !ch2.offers(suite) || !ch2.offers_version(0x0304))
      return fail(ALERT_ILLEGAL_PARAMETER, "the second ClientHello ignored the HelloRetryRequest");
    raw(transcript, ByteSpan(ch2_msg->data(), ch2_msg->size()));
    ch2.session_id = ch.session_id;   // must be unchanged; the echo below uses the first
    ch = std::move(ch2);
  }

  send_keys.gcm = recv_keys.gcm = suite == TLS13_AES_128_GCM_SHA256;

  crypto::Scalar32 server_secret{};
  host.random(MutableByteSpan(server_secret.data(), server_secret.size()));
  server_secret[0] &= 248; server_secret[31] &= 127; server_secret[31] |= 64;
  crypto::Point32 server_pub = crypto::x25519_base(server_secret);
  crypto::Point32 shared{};
  ByteBuffer p256_pub;
  if (use_p256) {
    used_p256 = true;
    ByteBuffer d(32, 0);
    auto made = p256_keypair(d, &p256_pub);
    if (!made) return fail(ALERT_INTERNAL_ERROR, "could not make a secp256r1 key share");
    auto x = crypto::ecdh_shared_x(crypto::NistCurve::P256, ByteSpan(d.data(), 32), ByteSpan(ch.p256_share.data(), ch.p256_share.size()));
    if (!x.ok() || x->size() != 32) return fail(ALERT_ILLEGAL_PARAMETER, "client secp256r1 share invalid");
    std::memcpy(shared.data(), x->data(), 32);
  } else if (!crypto::x25519(shared, server_secret, ch.x25519_share)) {
    return fail(ALERT_ILLEGAL_PARAMETER, "client x25519 share invalid");
  }

  // ---- ServerHello ----
  {
    uint8_t server_random[32];
    host.random(MutableByteSpan(server_random, 32));
    ByteBuffer body;
    be16(body, 0x0303);
    raw(body, ByteSpan(server_random, 32));
    body.push_back((uint8_t)ch.session_id.size());
    raw(body, ByteSpan(ch.session_id.data(), ch.session_id.size()));
    be16(body, suite);
    body.push_back(0);
    ByteBuffer ext;
    ext_u16(ext, 43, 0x0304);
    be16(ext, 51);
    if (use_p256) {
      be16(ext, (uint16_t)(4 + p256_pub.size()));
      be16(ext, GROUP_SECP256R1);
      be16(ext, (uint16_t)p256_pub.size());
      raw(ext, ByteSpan(p256_pub.data(), p256_pub.size()));
    } else {
      be16(ext, 36);
      be16(ext, GROUP_X25519);
      be16(ext, 32);
      raw(ext, ByteSpan(server_pub.data(), 32));
    }
    be16(body, (uint16_t)ext.size());
    raw(body, ByteSpan(ext.data(), ext.size()));
    auto sh = handshake_msg(HS_SERVER_HELLO, ByteSpan(body.data(), body.size()));
    raw(transcript, ByteSpan(sh.data(), sh.size()));
    if (auto r = send_record(REC_HANDSHAKE, ByteSpan(sh.data(), sh.size())); !r.ok()) return Result<HandshakeFailure>(r.error());
  }
  if (auto r = compat_ccs(); !r.ok()) return Result<HandshakeFailure>(r.error());

  // ---- Key schedule (RFC 8446 7.1), SHA-256 throughout ----
  uint8_t zeros[32] = {0};
  auto empty_hash = crypto::sha256(ByteSpan());
  Hash early = hkdf_extract(ByteSpan(zeros, 32), ByteSpan(zeros, 32));
  auto derived = derive_secret(ByteSpan(early.data(), 32), "derived", ByteSpan(empty_hash.data(), 32));
  Hash hs_secret = hkdf_extract(ByteSpan(derived.data(), derived.size()), ByteSpan(shared.data(), 32));
  auto th = transcript_hash();
  auto c_hs = derive_secret(ByteSpan(hs_secret.data(), 32), "c hs traffic", ByteSpan(th.data(), 32));
  auto s_hs = derive_secret(ByteSpan(hs_secret.data(), 32), "s hs traffic", ByteSpan(th.data(), 32));
  auto derived2 = derive_secret(ByteSpan(hs_secret.data(), 32), "derived", ByteSpan(empty_hash.data(), 32));
  Hash master = hkdf_extract(ByteSpan(derived2.data(), derived2.size()), ByteSpan(zeros, 32));
  send_keys.set(ByteSpan(s_hs.data(), s_hs.size()));
  recv_keys.set(ByteSpan(c_hs.data(), c_hs.size()));

  auto send_hs = [&](uint8_t type, const ByteBuffer& body) -> Result<void> {
    auto m = handshake_msg(type, ByteSpan(body.data(), body.size()));
    raw(transcript, ByteSpan(m.data(), m.size()));
    return send_record(REC_HANDSHAKE, ByteSpan(m.data(), m.size()));
  };

  {  // EncryptedExtensions
    ByteBuffer ext;
    if (ch.alpn_http11) ext_alpn_http11(ext);
    ByteBuffer body;
    be16(body, (uint16_t)ext.size());
    raw(body, ByteSpan(ext.data(), ext.size()));
    if (auto r = send_hs(HS_ENCRYPTED_EXTENSIONS, body); !r.ok()) return Result<HandshakeFailure>(r.error());
  }
  {  // Certificate
    ByteBuffer list;
    for (const auto& c : chain) {
      be24(list, (uint32_t)c.der.size());
      raw(list, ByteSpan(c.der.data(), c.der.size()));
      be16(list, 0);
    }
    ByteBuffer body;
    body.push_back(0);   // certificate_request_context
    be24(body, (uint32_t)list.size());
    raw(body, ByteSpan(list.data(), list.size()));
    if (auto r = send_hs(HS_CERTIFICATE, body); !r.ok()) return Result<HandshakeFailure>(r.error());
  }
  {  // CertificateVerify
    ByteBuffer content(64, 0x20);
    raw(content, std::string("TLS 1.3, server CertificateVerify"));
    content.push_back(0);
    auto h = transcript_hash();
    raw(content, ByteSpan(h.data(), 32));
    auto sig = sign(sig_alg, ByteSpan(content.data(), content.size()));
    if (!sig.ok()) return fail(ALERT_INTERNAL_ERROR, "signing CertificateVerify failed: " + sig.error().str());
    ByteBuffer body;
    be16(body, sig_alg);
    be16(body, (uint16_t)sig->size());
    raw(body, ByteSpan(sig->data(), sig->size()));
    if (auto r = send_hs(HS_CERTIFICATE_VERIFY, body); !r.ok()) return Result<HandshakeFailure>(r.error());
  }
  {  // Finished
    auto fk = expand_label(ByteSpan(s_hs.data(), s_hs.size()), "finished", {}, 32);
    auto h = transcript_hash();
    auto verify = hmac(ByteSpan(fk.data(), fk.size()), ByteSpan(h.data(), 32));
    if (auto r = send_hs(HS_FINISHED, ByteBuffer(verify.begin(), verify.end())); !r.ok()) return Result<HandshakeFailure>(r.error());
  }

  auto th_sf = transcript_hash();
  auto c_ap = derive_secret(ByteSpan(master.data(), 32), "c ap traffic", ByteSpan(th_sf.data(), 32));
  auto s_ap = derive_secret(ByteSpan(master.data(), 32), "s ap traffic", ByteSpan(th_sf.data(), 32));

  // ---- Client Finished ----
  auto fin = next_handshake(timeout_ms);
  if (!fin.ok()) return Result<HandshakeFailure>(fin.error());
  if ((*fin)[0] != HS_FINISHED) return fail(ALERT_UNEXPECTED_MESSAGE, "expected client Finished");
  auto cfk = expand_label(ByteSpan(c_hs.data(), c_hs.size()), "finished", {}, 32);
  auto want = hmac(ByteSpan(cfk.data(), cfk.size()), ByteSpan(th_sf.data(), 32));
  if (fin->size() != 4 + 32 || std::memcmp(fin->data() + 4, want.data(), 32) != 0)
    return fail(ALERT_DECRYPT_ERROR, "client Finished did not verify");
  if (!hs_buf.empty()) return fail(ALERT_UNEXPECTED_MESSAGE, "handshake data after client Finished");

  send_keys.set(ByteSpan(s_ap.data(), s_ap.size()));
  recv_keys.set(ByteSpan(c_ap.data(), c_ap.size()));
  info.version = "TLS 1.3";
  info.cipher = suite_name(suite);
  return done();
}

Result<HandshakeFailure> TlsServer::Impl::handshake12(const ClientHelloInfo& ch, uint16_t suite, uint16_t sig_alg,
                                                      TlsInfo& info, int timeout_ms) {
  tls12 = true;
  bool gcm = suite == ECDHE_RSA_AES_128_GCM_SHA256 || suite == ECDHE_ECDSA_AES_128_GCM_SHA256;

  crypto::Scalar32 server_secret{};
  host.random(MutableByteSpan(server_secret.data(), server_secret.size()));
  server_secret[0] &= 248; server_secret[31] &= 127; server_secret[31] |= 64;
  crypto::Point32 server_pub = crypto::x25519_base(server_secret);
  // secp256r1 when the client does not list x25519
  const bool use_p256 = !ch.group_x25519 && ch.group_p256;
  ByteBuffer p256_d(32, 0), p256_pub;
  if (use_p256) {
    used_p256 = true;
    if (!p256_keypair(p256_d, &p256_pub)) return fail(ALERT_INTERNAL_ERROR, "could not make a secp256r1 key share");
  }

  std::array<uint8_t, 32> server_random{};
  host.random(MutableByteSpan(server_random.data(), 32));
  if (ch.offers_version(0x0304))   // RFC 8446 4.1.3 downgrade sentinel "DOWNGRD\x01"
    std::memcpy(server_random.data() + 24, "\x44\x4F\x57\x4E\x47\x52\x44\x01", 8);

  auto send_hs = [&](uint8_t type, const ByteBuffer& body) -> Result<void> {
    auto m = handshake_msg(type, ByteSpan(body.data(), body.size()));
    raw(transcript, ByteSpan(m.data(), m.size()));
    return send_record(REC_HANDSHAKE, ByteSpan(m.data(), m.size()));
  };

  {  // ServerHello. The session id is left EMPTY: echoing the client's would
     // claim a resumption that does not exist, and TLS 1.3-capable clients
     // (which always send a random compatibility id) abort on it.
    ByteBuffer body;
    be16(body, 0x0303);
    raw(body, ByteSpan(server_random.data(), 32));
    body.push_back(0);
    be16(body, suite);
    body.push_back(0);
    ByteBuffer ext;
    if (ch.secure_reneg) { be16(ext, 0xff01); be16(ext, 1); ext.push_back(0); }
    if (ch.point_formats) { be16(ext, 11); be16(ext, 2); ext.push_back(1); ext.push_back(0); }
    if (ch.ems) { be16(ext, 23); be16(ext, 0); }
    if (ch.alpn_http11) ext_alpn_http11(ext);
    if (!ext.empty()) {
      be16(body, (uint16_t)ext.size());
      raw(body, ByteSpan(ext.data(), ext.size()));
    }
    if (auto r = send_hs(HS_SERVER_HELLO, body); !r.ok()) return Result<HandshakeFailure>(r.error());
  }
  {  // Certificate
    ByteBuffer list;
    for (const auto& c : chain) {
      be24(list, (uint32_t)c.der.size());
      raw(list, ByteSpan(c.der.data(), c.der.size()));
    }
    ByteBuffer body;
    be24(body, (uint32_t)list.size());
    raw(body, ByteSpan(list.data(), list.size()));
    if (auto r = send_hs(HS_CERTIFICATE, body); !r.ok()) return Result<HandshakeFailure>(r.error());
  }
  {  // ServerKeyExchange
    ByteBuffer params;
    params.push_back(0x03);   // named_curve
    if (use_p256) {
      be16(params, GROUP_SECP256R1);
      params.push_back((uint8_t)p256_pub.size());
      raw(params, ByteSpan(p256_pub.data(), p256_pub.size()));
    } else {
      be16(params, GROUP_X25519);
      params.push_back(32);
      raw(params, ByteSpan(server_pub.data(), 32));
    }
    ByteBuffer signed_data;
    raw(signed_data, ByteSpan(ch.random.data(), ch.random.size()));
    raw(signed_data, ByteSpan(server_random.data(), 32));
    raw(signed_data, ByteSpan(params.data(), params.size()));
    auto sig = sign(sig_alg, ByteSpan(signed_data.data(), signed_data.size()));
    if (!sig.ok()) return fail(ALERT_INTERNAL_ERROR, "signing ServerKeyExchange failed: " + sig.error().str());
    ByteBuffer body = params;
    be16(body, sig_alg);
    be16(body, (uint16_t)sig->size());
    raw(body, ByteSpan(sig->data(), sig->size()));
    if (auto r = send_hs(HS_SERVER_KEY_EXCHANGE, body); !r.ok()) return Result<HandshakeFailure>(r.error());
  }
  if (auto r = send_hs(HS_SERVER_HELLO_DONE, {}); !r.ok()) return Result<HandshakeFailure>(r.error());

  // ---- ClientKeyExchange ----
  auto cke = next_handshake(timeout_ms);
  if (!cke.ok()) return Result<HandshakeFailure>(cke.error());
  if ((*cke)[0] != HS_CLIENT_KEY_EXCHANGE) return fail(ALERT_UNEXPECTED_MESSAGE, "expected ClientKeyExchange");
  crypto::Point32 pre_master{};
  if (use_p256) {
    if (cke->size() != 4 + 66 || (*cke)[4] != 65) return fail(ALERT_DECODE_ERROR, "malformed ClientKeyExchange");
    raw(transcript, ByteSpan(cke->data(), cke->size()));
    auto x = crypto::ecdh_shared_x(crypto::NistCurve::P256, ByteSpan(p256_d.data(), 32), ByteSpan(cke->data() + 5, 65));
    if (!x.ok() || x->size() != 32) return fail(ALERT_ILLEGAL_PARAMETER, "client secp256r1 share invalid");
    std::memcpy(pre_master.data(), x->data(), 32);
  } else {
    if (cke->size() != 4 + 33 || (*cke)[4] != 32) return fail(ALERT_DECODE_ERROR, "malformed ClientKeyExchange");
    crypto::Point32 client_pub{};
    std::memcpy(client_pub.data(), cke->data() + 5, 32);
    raw(transcript, ByteSpan(cke->data(), cke->size()));
    if (!crypto::x25519(pre_master, server_secret, client_pub)) return fail(ALERT_ILLEGAL_PARAMETER, "client x25519 share invalid");
  }

  ByteBuffer master;
  if (ch.ems) {   // RFC 7627: bind the master secret to the whole handshake so far
    auto session_hash = transcript_hash();
    master = prf(ByteSpan(pre_master.data(), 32), "extended master secret", ByteSpan(session_hash.data(), 32), 48);
  } else {
    ByteBuffer seed;
    raw(seed, ByteSpan(ch.random.data(), ch.random.size()));
    raw(seed, ByteSpan(server_random.data(), 32));
    master = prf(ByteSpan(pre_master.data(), 32), "master secret", ByteSpan(seed.data(), seed.size()), 48);
  }
  ByteBuffer key_seed;
  raw(key_seed, ByteSpan(server_random.data(), 32));
  raw(key_seed, ByteSpan(ch.random.data(), ch.random.size()));
  size_t klen = gcm ? 16 : 32, ivlen = gcm ? 4 : 12;
  auto block = prf(ByteSpan(master.data(), master.size()), "key expansion", ByteSpan(key_seed.data(), key_seed.size()),
                   2 * (klen + ivlen));
  send_keys.gcm = recv_keys.gcm = gcm12 = gcm;
  std::memcpy(recv_keys.key.data(), block.data(), klen);
  std::memcpy(send_keys.key.data(), block.data() + klen, klen);
  std::memcpy(recv_keys.iv.data(), block.data() + 2 * klen, ivlen);
  std::memcpy(send_keys.iv.data(), block.data() + 2 * klen + ivlen, ivlen);
  send_keys.seq = recv_keys.seq = 0;
  send_keys.secret = recv_keys.secret = master;

  // ---- ChangeCipherSpec + client Finished ----
  bool saw_ccs = false;
  auto fin = next_handshake(timeout_ms, &saw_ccs);
  if (!fin.ok()) {
    if (fin.error().status == Status::Denied && std::string(fin.error().str()).find("authentication") != std::string::npos)
      return fail(ALERT_BAD_RECORD_MAC, "client Finished record did not decrypt");
    return Result<HandshakeFailure>(fin.error());
  }
  if (!saw_ccs || (*fin)[0] != HS_FINISHED) return fail(ALERT_UNEXPECTED_MESSAGE, "expected ChangeCipherSpec then Finished");
  auto th = transcript_hash();
  auto want = prf(ByteSpan(master.data(), master.size()), "client finished", ByteSpan(th.data(), 32), 12);
  if (fin->size() != 4 + 12 || std::memcmp(fin->data() + 4, want.data(), 12) != 0)
    return fail(ALERT_DECRYPT_ERROR, "client Finished did not verify");
  raw(transcript, ByteSpan(fin->data(), fin->size()));

  {  // our ChangeCipherSpec + Finished
    uint8_t one = 1;
    if (auto r = send_record(REC_CHANGE_CIPHER_SPEC, ByteSpan(&one, 1)); !r.ok()) return Result<HandshakeFailure>(r.error());
    send_keys.live = true;
    auto hh = transcript_hash();
    auto verify = prf(ByteSpan(master.data(), master.size()), "server finished", ByteSpan(hh.data(), 32), 12);
    if (auto r = send_hs(HS_FINISHED, verify); !r.ok()) return Result<HandshakeFailure>(r.error());
  }
  info.version = "TLS 1.2";
  info.cipher = suite_name(suite);
  return done();
}

TlsServer::TlsServer(hal::Host& host, std::string cert_chain_path, std::string private_key_path) {
  std::vector<crypto::Certificate> chain;
  crypto::PrivateKey pkey;
  auto cert_raw = host.read_file(cert_chain_path);
  if (cert_raw.ok()) {
    auto parsed_chain = crypto::x509_parse_bundle(ByteSpan(cert_raw->data(), cert_raw->size()));
    if (parsed_chain.ok()) chain = std::move(*parsed_chain);
  }
  auto key_raw = host.read_file(private_key_path);
  if (key_raw.ok()) {
    auto parsed_key = crypto::x509_parse_private_key(ByteSpan(key_raw->data(), key_raw->size()));
    if (parsed_key.ok()) pkey = std::move(*parsed_key);
  }
  impl_ = std::make_unique<Impl>(host, std::move(chain), std::move(pkey));
}

TlsServer::TlsServer(hal::Host& host, std::vector<crypto::Certificate> chain, crypto::PrivateKey key)
    : impl_(std::make_unique<Impl>(host, std::move(chain), std::move(key))) {}

TlsServer::~TlsServer() { close(); }

void TlsServer::close() {
  // A raw TCP close with no close_notify first is what this code's own read()
  // treats as a fault from the other end; a real client deserves the same.
  if (impl_ && impl_->conn && impl_->send_keys.live) impl_->send_alert(ALERT_CLOSE_NOTIFY);
  if (impl_ && impl_->conn) { impl_->conn->close(); impl_->conn.reset(); }
  closed_ = true;
}

Result<void> TlsServer::accept(std::unique_ptr<hal::Connection> conn, int timeout_ms) {
  if (!impl_) return Result<void>(Status::Invalid, "server not initialized");
  Impl& s = *impl_;
  s.conn = std::move(conn);
  s.transcript.clear();
  s.hs_buf.clear();
  s.send_keys = Keys{};
  s.recv_keys = Keys{};
  s.inbox.clear();
  s.tls12 = false;
  s.gcm12 = false;
  s.used_p256 = false;

  auto refuse = [&](uint8_t alert, const std::string& why) {
    s.send_alert(alert);
    return Result<void>(Status::Denied, why);
  };

  if (s.chain.empty()) return refuse(ALERT_INTERNAL_ERROR, "server certificate chain is empty");
  if (!s.key.valid()) return refuse(ALERT_INTERNAL_ERROR, "server private key is invalid");

  auto ch_msg = s.next_handshake(timeout_ms);
  if (!ch_msg.ok()) return Result<void>(ch_msg.error());
  if ((*ch_msg)[0] != HS_CLIENT_HELLO) return refuse(ALERT_UNEXPECTED_MESSAGE, "expected ClientHello");
  ClientHelloInfo ch;
  std::string why;
  if (!parse_client_hello(ByteSpan(ch_msg->data(), ch_msg->size()), ch, why)) return refuse(ALERT_DECODE_ERROR, why);
  if (!ch.group_x25519 && !ch.has_x25519_share && !ch.group_p256 && !ch.has_p256_share)
    return refuse(ALERT_HANDSHAKE_FAILURE, "the client offers no key exchange group this server implements (x25519, secp256r1)");

  const crypto::Certificate& leaf = s.chain.front();
  Result<HandshakeFailure> outcome = fail(ALERT_PROTOCOL_VERSION, "the client offers neither TLS 1.3 nor TLS 1.2");

  // Prefer ChaCha20: this machine's AES is portable software, not AES-NI.
  uint16_t suite13 = ch.offers(TLS13_CHACHA20_POLY1305_SHA256) ? TLS13_CHACHA20_POLY1305_SHA256
                     : ch.offers(TLS13_AES_128_GCM_SHA256)     ? TLS13_AES_128_GCM_SHA256
                                                               : 0;
  uint16_t sig13 = s.pick_sig_alg(ch, true);
  if (ch.offers_version(0x0304) && suite13 && sig13) {
    outcome = s.handshake13(ch, *ch_msg, suite13, sig13, info_, timeout_ms);
  } else if (ch.offers_version(0x0303)) {
    uint16_t chacha = leaf.key_is_ec ? ECDHE_ECDSA_CHACHA20_POLY1305 : ECDHE_RSA_CHACHA20_POLY1305;
    uint16_t aes = leaf.key_is_ec ? ECDHE_ECDSA_AES_128_GCM_SHA256 : ECDHE_RSA_AES_128_GCM_SHA256;
    uint16_t suite12 = ch.offers(chacha) ? chacha : ch.offers(aes) ? aes : 0;
    uint16_t sig12 = s.pick_sig_alg(ch, false);
    if (!ch.group_x25519 && !ch.group_p256) outcome = fail(ALERT_HANDSHAKE_FAILURE, "TLS 1.2 client lists neither x25519 nor secp256r1");
    else if (!suite12) outcome = fail(ALERT_HANDSHAKE_FAILURE, "no cipher suite in common (need ECDHE with ChaCha20-Poly1305 or AES-128-GCM)");
    else if (!sig12) outcome = fail(ALERT_HANDSHAKE_FAILURE, "no signature algorithm in common for this certificate");
    else {
      raw(s.transcript, ByteSpan(ch_msg->data(), ch_msg->size()));
      outcome = s.handshake12(ch, suite12, sig12, info_, timeout_ms);
    }
  } else if (ch.offers_version(0x0304)) {
    outcome = fail(ALERT_HANDSHAKE_FAILURE, "TLS 1.3 client shares no cipher suite or signature algorithm with this server");
  }

  if (!outcome.ok()) return Result<void>(outcome.error());
  if (outcome->alert) return refuse(outcome->alert, outcome->why);

  info_.group = s.used_p256 ? "secp256r1" : "x25519";
  info_.subject = leaf.subject_cn;
  info_.issuer = leaf.issuer_cn;
  info_.chain_length = s.chain.size();
  closed_ = false;
  return Ok();
}

Result<void> TlsServer::write(ByteSpan data) {
  if (!impl_ || !impl_->conn) return Result<void>(Status::IoError, "not connected");
  size_t off = 0;
  while (off < data.size()) {
    size_t n = data.size() - off;
    if (n > 16384) n = 16384;
    if (auto r = impl_->send_record(REC_APPLICATION, data.subspan(off, n)); !r.ok()) return r;
    info_.bytes_out += n;
    off += n;
  }
  return Ok();
}

Result<ByteBuffer> TlsServer::read(int timeout_ms) {
  if (!impl_ || !impl_->conn) return Result<ByteBuffer>(Status::IoError, "not connected");
  Impl& s = *impl_;
  if (!s.inbox.empty()) {
    ByteBuffer out;
    out.swap(s.inbox);
    return out;
  }
  for (;;) {
    uint8_t type = 0;
    auto rec = s.read_record(type, timeout_ms);
    if (!rec.ok()) {
      if (rec.status() == Status::IoError) { closed_ = true; return ByteBuffer{}; }
      if (rec.status() == Status::Denied) { s.send_alert(ALERT_BAD_RECORD_MAC); closed_ = true; }
      return rec;
    }
    if (type == REC_APPLICATION) {
      if (rec->empty()) continue;
      info_.bytes_in += rec->size();
      return rec;
    }
    if (type == REC_ALERT) {
      closed_ = true;
      if (rec->size() >= 2 && (*rec)[1] == ALERT_CLOSE_NOTIFY) return ByteBuffer{};
      return Result<ByteBuffer>(Status::IoError, "the peer sent an alert");
    }
    if (type == REC_HANDSHAKE && !s.tls12) {
      // Post-handshake messages. KeyUpdate rolls the client's keys (and ours,
      // when asked); anything else (tickets are server-to-client) is ignored.
      raw(s.hs_buf, ByteSpan(rec->data(), rec->size()));
      while (s.hs_buf.size() >= 4) {
        size_t len = ((size_t)s.hs_buf[1] << 16) | ((size_t)s.hs_buf[2] << 8) | s.hs_buf[3];
        if (s.hs_buf.size() < 4 + len) break;
        uint8_t ht = s.hs_buf[0];
        bool request_update = ht == HS_KEY_UPDATE && len == 1 && s.hs_buf[4] == 1;
        s.hs_buf.erase(s.hs_buf.begin(), s.hs_buf.begin() + (long)(4 + len));
        if (ht != HS_KEY_UPDATE) continue;
        auto next_recv = expand_label(ByteSpan(s.recv_keys.secret.data(), s.recv_keys.secret.size()), "traffic upd", {}, 32);
        s.recv_keys.set(ByteSpan(next_recv.data(), next_recv.size()));
        if (request_update) {
          uint8_t ku[5] = {HS_KEY_UPDATE, 0, 0, 1, 0};
          if (auto r = s.send_record(REC_HANDSHAKE, ByteSpan(ku, 5)); !r.ok()) return Result<ByteBuffer>(r.error());
          auto next_send = expand_label(ByteSpan(s.send_keys.secret.data(), s.send_keys.secret.size()), "traffic upd", {}, 32);
          s.send_keys.set(ByteSpan(next_send.data(), next_send.size()));
        }
      }
      continue;
    }
    // 1.2 renegotiation attempts and stray CCS: not supported, ignored.
  }
}

}  // namespace aios::net
