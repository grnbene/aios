#include "aios/net/ssh.hpp"

#include <algorithm>
#include <cstring>
#include <optional>

#include "aios/core/base64.hpp"
#include "aios/crypto/aes_gcm.hpp"
#include "aios/crypto/bcrypt.hpp"
#include "aios/crypto/md5.hpp"
#include "aios/crypto/rsa.hpp"
#include "aios/crypto/sha1.hpp"
#include "aios/crypto/sha256.hpp"
#include "aios/crypto/sha512.hpp"
#include "aios/hal/tty.hpp"
#include "aios/node/secure.hpp"   // crypto::chacha20_xor, crypto::poly1305

namespace aios::net {
namespace {

// SSH message numbers (RFC 4253, 4252, 4254).
enum : uint8_t {
  MSG_DISCONNECT = 1,
  MSG_IGNORE = 2,
  MSG_UNIMPLEMENTED = 3,
  MSG_DEBUG = 4,
  MSG_SERVICE_REQUEST = 5,
  MSG_SERVICE_ACCEPT = 6,
  MSG_EXT_INFO = 7,
  MSG_KEXINIT = 20,
  MSG_NEWKEYS = 21,
  MSG_KEX_ECDH_INIT = 30,
  MSG_KEX_ECDH_REPLY = 31,
  MSG_USERAUTH_REQUEST = 50,
  MSG_USERAUTH_FAILURE = 51,
  MSG_USERAUTH_SUCCESS = 52,
  MSG_USERAUTH_BANNER = 53,
  MSG_USERAUTH_PK_OK = 60,
  MSG_GLOBAL_REQUEST = 80,
  MSG_REQUEST_SUCCESS = 81,
  MSG_REQUEST_FAILURE = 82,
  MSG_CHANNEL_OPEN = 90,
  MSG_CHANNEL_OPEN_CONFIRMATION = 91,
  MSG_CHANNEL_OPEN_FAILURE = 92,
  MSG_CHANNEL_WINDOW_ADJUST = 93,
  MSG_CHANNEL_DATA = 94,
  MSG_CHANNEL_EXTENDED_DATA = 95,
  MSG_CHANNEL_EOF = 96,
  MSG_CHANNEL_CLOSE = 97,
  MSG_CHANNEL_REQUEST = 98,
  MSG_CHANNEL_SUCCESS = 99,
  MSG_CHANNEL_FAILURE = 100,
};

// SSH wire encoding: big-endian lengths, length-prefixed strings, mpints.
struct Buf {
  ByteBuffer b;
  void u8(uint8_t v) { b.push_back(v); }
  void u32(uint32_t v) {
    b.push_back((uint8_t)(v >> 24)); b.push_back((uint8_t)(v >> 16));
    b.push_back((uint8_t)(v >> 8));  b.push_back((uint8_t)v);
  }
  void raw(ByteSpan s) { b.insert(b.end(), s.begin(), s.end()); }
  void str(ByteSpan s) { u32((uint32_t)s.size()); raw(s); }
  void str(const std::string& s) { str(span_of(s)); }
  void boolean(bool v) { b.push_back(v ? 1 : 0); }
  void mpint(ByteSpan v) {
    size_t i = 0;
    while (i < v.size() && v[i] == 0) ++i;               // strip leading zeroes
    bool pad = i < v.size() && (v[i] & 0x80) != 0;       // never look negative
    u32((uint32_t)(v.size() - i + (pad ? 1 : 0)));
    if (pad) b.push_back(0);
    b.insert(b.end(), v.begin() + (long)i, v.end());
  }
  ByteSpan span() const { return ByteSpan(b.data(), b.size()); }
};

struct Cursor {
  ByteSpan d;
  size_t pos = 0;
  bool bad = false;
  uint8_t u8() { if (pos >= d.size()) { bad = true; return 0; } return d[pos++]; }
  uint32_t u32() {
    if (pos + 4 > d.size()) { bad = true; pos = d.size(); return 0; }
    uint32_t v = ((uint32_t)d[pos] << 24) | ((uint32_t)d[pos + 1] << 16) | ((uint32_t)d[pos + 2] << 8) | d[pos + 3];
    pos += 4;
    return v;
  }
  ByteSpan str() {
    uint32_t n = u32();
    if (bad || pos + n > d.size()) { bad = true; return {}; }
    ByteSpan s = d.subspan(pos, n);
    pos += n;
    return s;
  }
  std::string text() { auto s = str(); return bad ? std::string() : string_of(s); }
  bool boolean() { return u8() != 0; }
};

std::string b64_nopad(ByteSpan d) {
  std::string s = base64_encode(d);
  while (!s.empty() && s.back() == '=') s.pop_back();
  return s;
}

std::string fingerprint_of(ByteSpan blob) {
  auto h = crypto::sha256(blob);
  return "SHA256:" + b64_nopad(ByteSpan(h.data(), h.size()));
}

// The chacha20-poly1305@openssh.com key material is 64 bytes: K_2 (payload)
// followed by K_1 (the packet length).
crypto::Key key_of(const std::vector<uint8_t>& material, bool second) {
  crypto::Key k{};
  std::memcpy(k.data(), material.data() + (second ? 32 : 0), 32);
  return k;
}

crypto::Nonce nonce_of(uint32_t seq) {
  crypto::Nonce n{};
  // 8-byte big-endian sequence number in the low half of the RFC 8439 nonce,
  // which is exactly the original 64-bit chacha nonce OpenSSH specifies.
  n[4] = 0; n[5] = 0; n[6] = 0; n[7] = 0;
  n[8] = (uint8_t)(seq >> 24); n[9] = (uint8_t)(seq >> 16);
  n[10] = (uint8_t)(seq >> 8); n[11] = (uint8_t)seq;
  return n;
}

std::array<uint8_t, 32> poly_key_for(const crypto::Key& k2, uint32_t seq) {
  uint8_t block[64];
  std::memset(block, 0, sizeof(block));
  crypto::chacha20_xor(k2, nonce_of(seq), 0, block, sizeof(block));
  std::array<uint8_t, 32> pk{};
  std::memcpy(pk.data(), block, 32);
  return pk;
}

const char* kKexAlgs = "curve25519-sha256,curve25519-sha256@libssh.org,ecdh-sha2-nistp256,ecdh-sha2-nistp384,ecdh-sha2-nistp521";
// RSA host keys as RFC 8332 signs them (SHA-2). The SHA-1 "ssh-rsa" signature
// is not offered: OpenSSH itself stopped accepting it in 8.8.
const char* kHostKeyAlgs = "ssh-ed25519,ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,ecdsa-sha2-nistp521,rsa-sha2-512,rsa-sha2-256";
// AES-GCM beside ChaCha20-Poly1305, for servers that turn ChaCha off (FIPS builds do).
const char* kCiphers = "chacha20-poly1305@openssh.com,aes256-gcm@openssh.com,aes128-gcm@openssh.com";

// The client's names, in `wanted` order (or its own order when `wanted` is empty), as one name-list.
std::string offer_list(const std::vector<std::string>& wanted, const char* mine) {
  if (wanted.empty()) return mine;
  std::string out;
  for (auto& w : wanted) {
    bool ours = false;
    std::string list = mine;
    size_t i = 0;
    while (i <= list.size()) {
      size_t j = list.find(',', i);
      if (j == std::string::npos) j = list.size();
      if (list.substr(i, j - i) == w) ours = true;
      if (j == list.size()) break;
      i = j + 1;
    }
    if (ours && (","+ out + ",").find("," + w + ",") == std::string::npos) out += (out.empty() ? "" : ",") + w;
  }
  return out;
}

bool is_gcm(const std::string& cipher) { return cipher == "aes256-gcm@openssh.com" || cipher == "aes128-gcm@openssh.com"; }

// RFC 5647 / OpenSSH: the IV's last 8 bytes are an invocation counter, one per packet.
void gcm_next(std::vector<uint8_t>& iv) {
  for (int i = 11; i >= 4; --i)
    if (++iv[(size_t)i] != 0) break;
}
ByteBuffer gcm_seal(const std::vector<uint8_t>& key, const std::vector<uint8_t>& iv, ByteSpan aad, ByteSpan plain) {
  crypto::GcmNonce n{};
  std::memcpy(n.data(), iv.data(), 12);
  if (key.size() == 32) { crypto::Aes256Key k{}; std::memcpy(k.data(), key.data(), 32); return crypto::aes256_gcm_seal(k, n, aad, plain); }
  crypto::Aes128Key k{};
  std::memcpy(k.data(), key.data(), 16);
  return crypto::aes128_gcm_seal(k, n, aad, plain);
}
bool gcm_open(const std::vector<uint8_t>& key, const std::vector<uint8_t>& iv, ByteSpan aad, ByteSpan sealed, ByteBuffer& out) {
  crypto::GcmNonce n{};
  std::memcpy(n.data(), iv.data(), 12);
  if (key.size() == 32) { crypto::Aes256Key k{}; std::memcpy(k.data(), key.data(), 32); return crypto::aes256_gcm_open(k, n, aad, sealed, out); }
  crypto::Aes128Key k{};
  std::memcpy(k.data(), key.data(), 16);
  return crypto::aes128_gcm_open(k, n, aad, sealed, out);
}

// The exchange hash of a key exchange method (RFC 5656: the curve's own SHA-2; curve25519: SHA-256).
ByteBuffer kex_hash(const std::string& kex, ByteSpan data) {
  if (kex == "ecdh-sha2-nistp384") { auto h = crypto::sha384(data); return ByteBuffer(h.begin(), h.end()); }
  if (kex == "ecdh-sha2-nistp521") { auto h = crypto::sha512(data); return ByteBuffer(h.begin(), h.end()); }
  auto h = crypto::sha256(data);
  return ByteBuffer(h.begin(), h.end());
}
const char* kMacs = "hmac-sha2-256";        // implicit under the AEAD; offered for form
const char* kCompression = "none";

bool list_has(const std::string& list, const std::string& want) {
  size_t i = 0;
  while (i <= list.size()) {
    size_t j = list.find(',', i);
    if (j == std::string::npos) j = list.size();
    if (list.substr(i, j - i) == want) return true;
    if (j == list.size()) break;
    i = j + 1;
  }
  return false;
}

ByteBuffer strip_leading_zeroes(ByteSpan v) {
  size_t i = 0;
  while (i + 1 < v.size() && v[i] == 0) ++i;
  return ByteBuffer(v.begin() + (long)i, v.end());
}

}  // namespace

namespace {

// "nistp256" and the rest: the curve as an ecdsa-sha2-* key names it.
bool ecdsa_curve_of(const std::string& name, crypto::NistCurve& out) {
  if (name == "nistp256") { out = crypto::NistCurve::P256; return true; }
  if (name == "nistp384") { out = crypto::NistCurve::P384; return true; }
  if (name == "nistp521") { out = crypto::NistCurve::P521; return true; }
  return false;
}
std::string ecdsa_curve_name(crypto::NistCurve c) {
  return c == crypto::NistCurve::P521 ? "nistp521" : c == crypto::NistCurve::P384 ? "nistp384" : "nistp256";
}
// The hash each ECDSA curve signs with in SSH (RFC 5656 6.2.1).
ByteBuffer ecdsa_digest(crypto::NistCurve c, ByteSpan data) {
  if (c == crypto::NistCurve::P521) { auto h = crypto::sha512(data); return ByteBuffer(h.begin(), h.end()); }
  if (c == crypto::NistCurve::P384) { auto h = crypto::sha384(data); return ByteBuffer(h.begin(), h.end()); }
  auto h = crypto::sha256(data);
  return ByteBuffer(h.begin(), h.end());
}

// ---- DER, as much of it as private keys use ----
struct Der {
  ByteSpan d;
  size_t p = 0;
  bool bad = false;
  bool at_end() const { return p >= d.size(); }
  uint8_t peek() const { return p < d.size() ? d[p] : 0; }
  ByteSpan take(uint8_t tag) {
    if (p + 2 > d.size() || d[p] != tag) { bad = true; return {}; }
    ++p;
    size_t len = d[p++];
    if (len & 0x80) {
      size_t n = len & 0x7f;
      if (n == 0 || n > 4 || p + n > d.size()) { bad = true; return {}; }
      len = 0;
      for (size_t i = 0; i < n; ++i) len = (len << 8) | d[p++];
    }
    if (p + len > d.size()) { bad = true; return {}; }
    ByteSpan body = d.subspan(p, len);
    p += len;
    return body;
  }
  uint64_t small_int() {
    ByteSpan v = take(0x02);
    if (bad || v.empty() || v.size() > 8) { bad = true; return 0; }
    uint64_t x = 0;
    for (uint8_t b : v) x = (x << 8) | b;
    return x;
  }
};
bool oid_is(ByteSpan oid, std::initializer_list<uint8_t> want) {
  return oid.size() == want.size() && std::equal(oid.begin(), oid.end(), want.begin());
}

// ---- PEM ----
struct PemBlock {
  std::string label;                                  // "OPENSSH PRIVATE KEY", "EC PRIVATE KEY", ...
  std::vector<std::pair<std::string, std::string>> headers;   // Proc-Type, DEK-Info
  ByteBuffer der;
  bool found = false;
};
Result<PemBlock> first_pem(const std::string& text) {
  PemBlock out;
  size_t b = text.find("-----BEGIN ");
  if (b == std::string::npos) return out;
  size_t le = text.find("-----", b + 11);
  if (le == std::string::npos) return Result<PemBlock>(Status::Corrupt, "a PEM BEGIN line without its closing dashes");
  out.label = text.substr(b + 11, le - b - 11);
  size_t body_start = text.find('\n', le);
  std::string end_line = "-----END " + out.label + "-----";
  size_t e = text.find(end_line, body_start == std::string::npos ? le : body_start);
  if (body_start == std::string::npos || e == std::string::npos)
    return Result<PemBlock>(Status::Corrupt, "the " + out.label + " block has no END line");
  std::string body;
  size_t i = body_start + 1;
  while (i < e) {
    size_t nl = text.find('\n', i);
    if (nl == std::string::npos || nl > e) nl = e;
    std::string line = text.substr(i, nl - i);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    size_t colon = line.find(": ");
    if (colon != std::string::npos && body.empty()) out.headers.push_back({line.substr(0, colon), line.substr(colon + 2)});
    else for (char ch : line) if (ch != ' ' && ch != '\t') body += ch;
    i = nl + 1;
  }
  auto raw = base64_decode(body);
  if (!raw.ok()) return Result<PemBlock>(Status::Corrupt, "the " + out.label + " block is not valid base64");
  out.der = std::move(*raw);
  out.found = true;
  return out;
}
std::string header_of(const PemBlock& b, const std::string& name) {
  for (auto& [k, v] : b.headers) if (k == name) return v;
  return "";
}

ByteBuffer unhex(const std::string& s) {
  ByteBuffer v;
  auto d = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if ((c | 32) >= 'a' && (c | 32) <= 'f') return (c | 32) - 'a' + 10;
    return -1;
  };
  for (size_t i = 0; i + 1 < s.size(); i += 2) {
    int a = d(s[i]), b = d(s[i + 1]);
    if (a < 0 || b < 0) return {};
    v.push_back((uint8_t)(a * 16 + b));
  }
  return v;
}

// Removes PKCS#7 padding; false when it is not there (which is what a wrong passphrase produces).
bool strip_pkcs7(ByteBuffer& b) {
  if (b.empty() || b.size() % 16) return false;
  uint8_t n = b.back();
  if (n == 0 || n > 16 || n > b.size()) return false;
  for (size_t i = b.size() - n; i < b.size(); ++i) if (b[i] != n) return false;
  b.resize(b.size() - n);
  return true;
}

// The legacy "Proc-Type: 4,ENCRYPTED" PEM: OpenSSL's EVP_BytesToKey (MD5, one round, the IV's first 8 bytes as
// salt) gives the key, the cipher is the one DEK-Info names.
Result<ByteBuffer> open_legacy_pem(const PemBlock& b, const std::string& passphrase) {
  std::string dek = header_of(b, "DEK-Info");
  size_t comma = dek.find(',');
  if (comma == std::string::npos) return Result<ByteBuffer>(Status::Corrupt, "an encrypted PEM key without a usable DEK-Info line");
  std::string cipher = dek.substr(0, comma);
  ByteBuffer iv = unhex(dek.substr(comma + 1));
  size_t keylen = cipher == "AES-128-CBC" ? 16 : cipher == "AES-192-CBC" ? 24 : cipher == "AES-256-CBC" ? 32 : 0;
  if (!keylen)
    return Result<ByteBuffer>(Status::Unsupported, "this PEM key is encrypted with " + cipher +
                                                       "; rewrite it with `ssh-keygen -p -f <key>`, which uses the OpenSSH format");
  if (iv.size() != 16) return Result<ByteBuffer>(Status::Corrupt, "the DEK-Info IV is not 16 bytes");
  if (passphrase.empty()) return Result<ByteBuffer>(Status::Denied, "the key is encrypted (" + cipher + "); it needs its passphrase");
  ByteBuffer key, prev;
  while (key.size() < keylen) {
    ByteBuffer in = prev;
    in.insert(in.end(), passphrase.begin(), passphrase.end());
    in.insert(in.end(), iv.begin(), iv.begin() + 8);
    auto h = crypto::md5(ByteSpan(in.data(), in.size()));
    prev.assign(h.begin(), h.end());
    key.insert(key.end(), prev.begin(), prev.end());
  }
  key.resize(keylen);
  ByteBuffer out(b.der.size());
  if (!crypto::aes_cbc_decrypt(ByteSpan(key), ByteSpan(iv), ByteSpan(b.der), MutableByteSpan(out)) || !strip_pkcs7(out))
    return Result<ByteBuffer>(Status::Denied, "incorrect passphrase (the decrypted key's padding is wrong)");
  return out;
}

// HMAC and PBKDF2 over the hashes PKCS#5 v2 names as PRFs.
enum class Prf { Sha1, Sha256, Sha384, Sha512 };
ByteBuffer prf_hash(Prf f, ByteSpan m) {
  switch (f) {
    case Prf::Sha1: { auto h = crypto::sha1(m); return ByteBuffer(h.begin(), h.end()); }
    case Prf::Sha256: { auto h = crypto::sha256(m); return ByteBuffer(h.begin(), h.end()); }
    case Prf::Sha384: { auto h = crypto::sha384(m); return ByteBuffer(h.begin(), h.end()); }
    case Prf::Sha512: { auto h = crypto::sha512(m); return ByteBuffer(h.begin(), h.end()); }
  }
  return {};
}
ByteBuffer prf_hmac(Prf f, ByteSpan key, ByteSpan msg) {
  const size_t block = (f == Prf::Sha384 || f == Prf::Sha512) ? 128 : 64;
  ByteBuffer k = key.size() > block ? prf_hash(f, key) : ByteBuffer(key.begin(), key.end());
  k.resize(block, 0);
  ByteBuffer in(block), out(block);
  for (size_t i = 0; i < block; ++i) { in[i] = k[i] ^ 0x36; out[i] = k[i] ^ 0x5c; }
  in.insert(in.end(), msg.begin(), msg.end());
  ByteBuffer ih = prf_hash(f, ByteSpan(in));
  out.insert(out.end(), ih.begin(), ih.end());
  return prf_hash(f, ByteSpan(out));
}
ByteBuffer pbkdf2(Prf f, const std::string& pass, ByteSpan salt, uint64_t iterations, size_t len) {
  ByteBuffer out;
  ByteSpan pw((const uint8_t*)pass.data(), pass.size());
  for (uint32_t block = 1; out.size() < len; ++block) {
    ByteBuffer s(salt.begin(), salt.end());
    s.push_back((uint8_t)(block >> 24)); s.push_back((uint8_t)(block >> 16));
    s.push_back((uint8_t)(block >> 8)); s.push_back((uint8_t)block);
    ByteBuffer u = prf_hmac(f, pw, ByteSpan(s)), t = u;
    for (uint64_t i = 1; i < iterations; ++i) {
      u = prf_hmac(f, pw, ByteSpan(u));
      for (size_t j = 0; j < t.size(); ++j) t[j] ^= u[j];
    }
    out.insert(out.end(), t.begin(), t.end());
  }
  out.resize(len);
  return out;
}

// PKCS#8 EncryptedPrivateKeyInfo with PBES2 (PBKDF2 + AES-CBC), what `ssh-keygen -m PKCS8 -N` and OpenSSL write.
Result<ByteBuffer> open_pkcs8(ByteSpan der, const std::string& passphrase) {
  Der top{der};
  Der epki{top.take(0x30)};
  Der alg{epki.take(0x30)};
  ByteSpan alg_oid = alg.take(0x06);
  if (top.bad || epki.bad || alg.bad) return Result<ByteBuffer>(Status::Corrupt, "malformed PKCS#8 EncryptedPrivateKeyInfo");
  if (!oid_is(alg_oid, {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x05, 0x0d}))
    return Result<ByteBuffer>(Status::Unsupported, "this PKCS#8 key is encrypted with a scheme other than PBES2; "
                                                   "rewrite it with `ssh-keygen -p -f <key>`");
  Der params{alg.take(0x30)};
  Der kdf{params.take(0x30)};
  ByteSpan kdf_oid = kdf.take(0x06);
  if (!oid_is(kdf_oid, {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x05, 0x0c}))
    return Result<ByteBuffer>(Status::Unsupported, "PBES2 with a key derivation other than PBKDF2");
  Der kp{kdf.take(0x30)};
  ByteSpan salt = kp.take(0x04);
  uint64_t iterations = kp.small_int();
  if (!kp.at_end() && kp.peek() == 0x02) kp.small_int();   // keyLength: the cipher says it anyway
  Prf prf = Prf::Sha1;
  if (!kp.at_end() && kp.peek() == 0x30) {
    Der pa{kp.take(0x30)};
    ByteSpan po = pa.take(0x06);
    if (oid_is(po, {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x02, 0x07})) prf = Prf::Sha1;
    else if (oid_is(po, {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x02, 0x09})) prf = Prf::Sha256;
    else if (oid_is(po, {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x02, 0x0a})) prf = Prf::Sha384;
    else if (oid_is(po, {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x02, 0x0b})) prf = Prf::Sha512;
    else return Result<ByteBuffer>(Status::Unsupported, "PBKDF2 with an unknown PRF");
  }
  Der enc{params.take(0x30)};
  ByteSpan enc_oid = enc.take(0x06);
  ByteSpan iv = enc.take(0x04);
  ByteSpan data = epki.take(0x04);
  if (params.bad || kdf.bad || kp.bad || enc.bad || epki.bad || iterations == 0 || iterations > 10000000)
    return Result<ByteBuffer>(Status::Corrupt, "malformed PBES2 parameters");
  size_t keylen = 0;
  if (oid_is(enc_oid, {0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x01, 0x02})) keylen = 16;
  else if (oid_is(enc_oid, {0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x01, 0x16})) keylen = 24;
  else if (oid_is(enc_oid, {0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x01, 0x2a})) keylen = 32;
  else return Result<ByteBuffer>(Status::Unsupported, "PBES2 with a cipher other than AES-CBC");
  if (passphrase.empty()) return Result<ByteBuffer>(Status::Denied, "the key is encrypted (PKCS#8); it needs its passphrase");
  ByteBuffer key = pbkdf2(prf, passphrase, salt, iterations, keylen);
  ByteBuffer out(data.size());
  if (!crypto::aes_cbc_decrypt(ByteSpan(key), iv, data, MutableByteSpan(out)) || !strip_pkcs7(out))
    return Result<ByteBuffer>(Status::Denied, "incorrect passphrase (the decrypted key's padding is wrong)");
  return out;
}

Result<SshIdentity> rsa_identity(const crypto::RsaPrivateKey& k) {
  SshIdentity id;
  id.type = "ssh-rsa";
  id.rsa = k;
  id.valid = id.rsa.valid();
  if (!id.valid) return Result<SshIdentity>(Status::Corrupt, "invalid RSA private key");
  return id;
}
Result<SshIdentity> ecdsa_identity(crypto::NistCurve curve, ByteSpan d) {
  SshIdentity id;
  id.type = "ecdsa-sha2-" + ecdsa_curve_name(curve);
  id.ec_curve = curve;
  id.ec_d.assign(d.begin(), d.end());
  auto pub = crypto::ecdsa_public_key(curve, d);
  if (!pub.ok()) return Result<SshIdentity>(Status::Corrupt, "invalid ECDSA private key: " + pub.error().message);
  id.ec_pub = std::move(*pub);
  id.valid = true;
  return id;
}

// SEC1 ECPrivateKey: the curve from its parameters, or the one PKCS#8 named around it.
Result<SshIdentity> sec1_identity(ByteSpan der, const crypto::NistCurve* named) {
  auto k = crypto::ec_parse_private_key(der);
  if (!k.ok()) return Result<SshIdentity>(k.error());
  crypto::NistCurve curve = named ? *named : k->curve;
  // the scalar is the curve's width: left-pad a short one so the public point is derived from the right integer
  size_t width = curve == crypto::NistCurve::P521 ? 66 : curve == crypto::NistCurve::P384 ? 48 : 32;
  ByteBuffer d = k->d;
  while (d.size() > width && d.front() == 0) d.erase(d.begin());
  if (d.size() < width) d.insert(d.begin(), width - d.size(), 0);
  return ecdsa_identity(curve, ByteSpan(d));
}

// PKCS#8 PrivateKeyInfo: RSA, EC or Ed25519.
Result<SshIdentity> pkcs8_identity(ByteSpan der) {
  Der top{der};
  Der pki{top.take(0x30)};
  pki.small_int();
  Der alg{pki.take(0x30)};
  ByteSpan oid = alg.take(0x06);
  ByteSpan key = pki.take(0x04);
  if (top.bad || pki.bad || alg.bad) return Result<SshIdentity>(Status::Corrupt, "malformed PKCS#8 PrivateKeyInfo");
  if (oid_is(oid, {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01})) {
    auto k = crypto::rsa_parse_private_key(key);
    if (!k.ok()) return Result<SshIdentity>(k.error());
    return rsa_identity(*k);
  }
  if (oid_is(oid, {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01})) {
    crypto::NistCurve curve;
    bool known = false;
    if (alg.peek() == 0x06) {
      known = crypto::nist_curve_from_oid(alg.take(0x06), curve);
    } else if (alg.peek() == 0x30) {
      // explicit ECParameters (LibreSSL writes these): version, fieldID {prime-field, p}, curve {a, b[, seed]}, base, order
      Der params{alg.take(0x30)};
      params.small_int();
      Der field{params.take(0x30)};
      field.take(0x06);
      ByteSpan prime = field.take(0x02);
      Der coeffs{params.take(0x30)};
      coeffs.take(0x04);
      ByteSpan b = coeffs.take(0x04);
      ByteSpan base = params.take(0x04);
      ByteSpan order = params.take(0x02);
      known = !params.bad && !field.bad && !coeffs.bad && crypto::nist_curve_from_params(prime, b, base, order, curve);
    }
    if (alg.bad || !known)
      return Result<SshIdentity>(Status::Unsupported, "an EC key on a curve other than P-256, P-384 or P-521");
    return sec1_identity(key, &curve);
  }
  if (oid_is(oid, {0x2b, 0x65, 0x70})) {
    Der inner{key};
    ByteSpan seed = inner.take(0x04);
    if (inner.bad || seed.size() != 32) return Result<SshIdentity>(Status::Corrupt, "malformed Ed25519 PKCS#8 key");
    SshIdentity id;
    id.type = "ssh-ed25519";
    std::memcpy(id.seed.data(), seed.data(), 32);
    id.pub = crypto::ed25519_public_from_seed(id.seed);
    id.valid = true;
    return id;
  }
  return Result<SshIdentity>(Status::Unsupported, "a PKCS#8 key of a type this machine does not sign with");
}

// openssh-key-v1: the envelope, and its private section opened with the cipher it names.
struct OpenSshKey {
  std::string cipher, kdf;
  ByteSpan kdfopts, public_blob, sealed;
  ByteBuffer tag;          // the AEAD tag after the private section (aes*-gcm, chacha20-poly1305)
};
Result<OpenSshKey> openssh_envelope(ByteSpan raw) {
  Cursor c{raw};
  const char* magic = "openssh-key-v1";
  for (size_t i = 0; i <= std::strlen(magic); ++i)
    if (c.u8() != (uint8_t)magic[i]) return Result<OpenSshKey>(Status::Invalid, "bad openssh-key-v1 magic");
  OpenSshKey k;
  k.cipher = c.text();
  k.kdf = c.text();
  k.kdfopts = c.str();
  uint32_t nkeys = c.u32();
  if (c.bad || nkeys != 1) return Result<OpenSshKey>(Status::Corrupt, "an openssh-key-v1 file holds exactly one key");
  k.public_blob = c.str();
  k.sealed = c.str();
  if (c.bad) return Result<OpenSshKey>(Status::Corrupt, "truncated private key body");
  if (k.cipher == "aes256-gcm@openssh.com" || k.cipher == "aes128-gcm@openssh.com" || k.cipher == "chacha20-poly1305@openssh.com") {
    if (c.pos + 16 > raw.size()) return Result<OpenSshKey>(Status::Corrupt, "the private section's authentication tag is missing");
    k.tag.assign(raw.begin() + (long)c.pos, raw.begin() + (long)c.pos + 16);
  }
  return k;
}

Result<ByteBuffer> openssh_open(const OpenSshKey& k, const std::string& passphrase) {
  if (k.cipher == "none" && k.kdf == "none") return ByteBuffer(k.sealed.begin(), k.sealed.end());
  if (k.kdf != "bcrypt")
    return Result<ByteBuffer>(Status::Unsupported, "private key is encrypted with " + k.cipher + "/" + k.kdf + ": an unknown KDF");
  size_t keylen, ivlen;
  if (k.cipher == "aes256-ctr" || k.cipher == "aes256-cbc") { keylen = 32; ivlen = 16; }
  else if (k.cipher == "aes192-ctr" || k.cipher == "aes192-cbc") { keylen = 24; ivlen = 16; }
  else if (k.cipher == "aes128-ctr" || k.cipher == "aes128-cbc") { keylen = 16; ivlen = 16; }
  else if (k.cipher == "aes256-gcm@openssh.com") { keylen = 32; ivlen = 12; }
  else if (k.cipher == "aes128-gcm@openssh.com") { keylen = 16; ivlen = 12; }
  else if (k.cipher == "chacha20-poly1305@openssh.com") { keylen = 64; ivlen = 0; }
  else return Result<ByteBuffer>(Status::Unsupported, "private key is encrypted with " + k.cipher + ": an unknown cipher");
  Cursor kc{k.kdfopts};
  ByteSpan salt = kc.str();
  uint32_t rounds = kc.u32();
  if (kc.bad || salt.empty() || rounds < 1) return Result<ByteBuffer>(Status::Corrupt, "malformed bcrypt kdf options");
  if (passphrase.empty())
    return Result<ByteBuffer>(Status::Denied, "the key is encrypted (" + k.cipher + "); it needs its passphrase");
  ByteBuffer key_iv(keylen + ivlen);
  if (crypto::bcrypt_pbkdf(passphrase.c_str(), passphrase.size(), salt.data(), salt.size(), key_iv.data(), key_iv.size(), rounds) != 0)
    return Result<ByteBuffer>(Status::Corrupt, "bcrypt_pbkdf failed");
  ByteSpan key(key_iv.data(), keylen), iv(key_iv.data() + keylen, ivlen);
  ByteBuffer out(k.sealed.size());
  if (k.cipher.find("-ctr") != std::string::npos) {
    crypto::aes_ctr_crypt(key, iv, k.sealed, MutableByteSpan(out));
  } else if (k.cipher.find("-cbc") != std::string::npos) {
    if (!crypto::aes_cbc_decrypt(key, iv, k.sealed, MutableByteSpan(out)))
      return Result<ByteBuffer>(Status::Corrupt, "the private section is not whole AES blocks");
  } else if (k.cipher.find("-gcm@") != std::string::npos) {
    crypto::GcmNonce nonce{};
    std::memcpy(nonce.data(), iv.data(), 12);
    ByteBuffer sealed(k.sealed.begin(), k.sealed.end());
    sealed.insert(sealed.end(), k.tag.begin(), k.tag.end());
    bool ok;
    if (keylen == 32) { crypto::Aes256Key a{}; std::memcpy(a.data(), key.data(), 32); ok = crypto::aes256_gcm_open(a, nonce, {}, ByteSpan(sealed), out); }
    else { crypto::Aes128Key a{}; std::memcpy(a.data(), key.data(), 16); ok = crypto::aes128_gcm_open(a, nonce, {}, ByteSpan(sealed), out); }
    if (!ok) return Result<ByteBuffer>(Status::Denied, "incorrect passphrase (the private section does not authenticate)");
  } else {
    // chacha20-poly1305@openssh.com with sequence number 0 and no length to hide: K_2 encrypts, its block 0 is the
    // Poly1305 key over the ciphertext
    crypto::Key k2{};
    std::memcpy(k2.data(), key.data(), 32);
    auto pk = poly_key_for(k2, 0);
    auto tag = crypto::poly1305(pk, k.sealed);
    uint8_t diff = 0;
    for (size_t i = 0; i < 16; ++i) diff |= (uint8_t)(tag[i] ^ k.tag[i]);
    if (diff) return Result<ByteBuffer>(Status::Denied, "incorrect passphrase (the private section does not authenticate)");
    std::memcpy(out.data(), k.sealed.data(), out.size());
    crypto::chacha20_xor(k2, nonce_of(0), 1, out.data(), out.size());
  }
  return out;
}

Result<SshIdentity> openssh_identity(ByteSpan priv) {
  Cursor p{priv};
  uint32_t check1 = p.u32(), check2 = p.u32();
  if (p.bad || check1 != check2)
    return Result<SshIdentity>(Status::Denied, "incorrect passphrase or corrupted private key (check words differ)");
  std::string type = p.text();
  if (type == "ssh-ed25519") {
    ByteSpan pub = p.str();
    ByteSpan sec = p.str();      // 64 bytes: seed || public
    std::string comment = p.text();
    if (p.bad || pub.size() != 32 || sec.size() != 64) return Result<SshIdentity>(Status::Corrupt, "malformed ed25519 key body");
    SshIdentity id;
    id.type = "ssh-ed25519";
    std::memcpy(id.seed.data(), sec.data(), 32);
    std::memcpy(id.pub.data(), pub.data(), 32);
    id.comment = comment;
    id.valid = true;
    auto derived = crypto::ed25519_public_from_seed(id.seed);
    if (std::memcmp(derived.data(), id.pub.data(), 32) != 0)
      return Result<SshIdentity>(Status::Corrupt, "the key's public half is not the one its seed derives");
    return id;
  }
  if (type == "ssh-rsa") {
    ByteSpan n_raw = p.str(), e_raw = p.str(), d_raw = p.str(), iqmp_raw = p.str(), p_raw = p.str(), q_raw = p.str();
    std::string comment = p.text();
    if (p.bad || n_raw.empty() || e_raw.empty() || d_raw.empty()) return Result<SshIdentity>(Status::Corrupt, "malformed rsa key body");
    crypto::RsaPrivateKey k;
    k.modulus = strip_leading_zeroes(n_raw);
    k.public_exponent = strip_leading_zeroes(e_raw);
    k.private_exponent = strip_leading_zeroes(d_raw);
    k.coefficient = strip_leading_zeroes(iqmp_raw);
    k.prime1 = strip_leading_zeroes(p_raw);
    k.prime2 = strip_leading_zeroes(q_raw);
    auto id = rsa_identity(k);
    if (id.ok()) id->comment = comment;
    return id;
  }
  if (type.rfind("ecdsa-sha2-", 0) == 0) {
    std::string curve_name = p.text();
    ByteSpan q = p.str();
    ByteSpan d = p.str();
    std::string comment = p.text();
    crypto::NistCurve curve;
    if (p.bad || !ecdsa_curve_of(curve_name, curve) || type != "ecdsa-sha2-" + curve_name)
      return Result<SshIdentity>(Status::Corrupt, "malformed " + type + " key body");
    size_t width = curve == crypto::NistCurve::P521 ? 66 : curve == crypto::NistCurve::P384 ? 48 : 32;
    ByteBuffer dd = strip_leading_zeroes(d);
    if (dd.size() > width) return Result<SshIdentity>(Status::Corrupt, "the " + type + " scalar is wider than its curve");
    dd.insert(dd.begin(), width - dd.size(), 0);
    auto id = ecdsa_identity(curve, ByteSpan(dd));
    if (!id.ok()) return id;
    if (!std::equal(q.begin(), q.end(), id->ec_pub.begin(), id->ec_pub.end()))
      return Result<SshIdentity>(Status::Corrupt, "the key's public point is not the one its scalar derives");
    id->comment = comment;
    return id;
  }
  if (type.rfind("sk-", 0) == 0)
    return Result<SshIdentity>(Status::Unsupported, type + " is a security-key (FIDO) key: it signs on the token itself; "
                                                           "load it into ssh-agent and the agent signs for this machine");
  return Result<SshIdentity>(Status::Unsupported, "key type " + type + ": this machine signs with ssh-ed25519, ssh-rsa and ecdsa-sha2-nistp*");
}

// SSH's mpint as an unsigned big-endian integer.
ByteBuffer mpint_value(ByteSpan v) { return strip_leading_zeroes(v); }

// EMSA-PKCS1-v1_5 with SHA-1, for the legacy "ssh-rsa" signature an agent may still return.
bool rsa_verify_sha1(const crypto::RsaPublicKey& key, ByteSpan data, ByteSpan sig) {
  auto em = crypto::rsa_public_op(key, sig);
  if (!em.ok()) return false;
  static const uint8_t prefix[] = {0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e, 0x03, 0x02, 0x1a, 0x05, 0x00, 0x04, 0x14};
  auto h = crypto::sha1(data);
  size_t k = (key.bits() + 7) / 8;
  ByteBuffer want(k, 0xff);
  want[0] = 0x00;
  want[1] = 0x01;
  size_t t = sizeof(prefix) + h.size();
  if (k < t + 11) return false;
  want[k - t - 1] = 0x00;
  std::memcpy(want.data() + k - t, prefix, sizeof(prefix));
  std::memcpy(want.data() + k - h.size(), h.data(), h.size());
  ByteBuffer got = *em;
  if (got.size() < k) got.insert(got.begin(), k - got.size(), 0);
  return got == want;
}

}  // namespace

// ---------------------------------------------------------------------------
// Identities
// ---------------------------------------------------------------------------
ByteBuffer SshIdentity::public_blob() const {
  Buf b;
  if (type == "ssh-rsa") {
    b.str(std::string("ssh-rsa"));
    b.mpint(ByteSpan(rsa.public_exponent.data(), rsa.public_exponent.size()));
    b.mpint(ByteSpan(rsa.modulus.data(), rsa.modulus.size()));
  } else if (type.rfind("ecdsa-sha2-", 0) == 0) {
    b.str(type);
    b.str(ecdsa_curve_name(ec_curve));
    b.str(ByteSpan(ec_pub.data(), ec_pub.size()));
  } else {
    b.str(std::string("ssh-ed25519"));
    b.str(ByteSpan(pub.data(), pub.size()));
  }
  return b.b;
}

std::string SshIdentity::public_line() const {
  auto blob = public_blob();
  return type + " " + base64_encode(ByteSpan(blob.data(), blob.size())) + (comment.empty() ? std::string() : " " + comment);
}

std::string SshIdentity::fingerprint() const {
  auto blob = public_blob();
  return fingerprint_of(ByteSpan(blob.data(), blob.size()));
}

std::string ssh_blob_type(ByteSpan blob) {
  Cursor c{blob};
  std::string t = c.text();
  return c.bad ? std::string() : t;
}

std::string ssh_fingerprint(ByteSpan blob) { return fingerprint_of(blob); }

Result<std::pair<std::string, ByteBuffer>> ssh_parse_public_line(const std::string& line_in) {
  std::string line = line_in;
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) line.pop_back();
  size_t a = line.find_first_not_of(" \t");
  if (a == std::string::npos) return Result<std::pair<std::string, ByteBuffer>>(Status::Invalid, "an empty public key line");
  size_t sp = line.find_first_of(" \t", a);
  if (sp == std::string::npos) return Result<std::pair<std::string, ByteBuffer>>(Status::Invalid, "a public key line without its key");
  std::string type = line.substr(a, sp - a);
  size_t b = line.find_first_not_of(" \t", sp);
  size_t e = b == std::string::npos ? std::string::npos : line.find_first_of(" \t", b);
  std::string b64 = b == std::string::npos ? std::string() : line.substr(b, e == std::string::npos ? std::string::npos : e - b);
  auto blob = base64_decode(b64);
  if (!blob.ok() || blob->empty()) return Result<std::pair<std::string, ByteBuffer>>(Status::Invalid, "the key in a public key line is not base64");
  if (ssh_blob_type(ByteSpan(*blob)) != type)
    return Result<std::pair<std::string, ByteBuffer>>(Status::Invalid, "a public key line says " + type + " but holds a " +
                                                                         ssh_blob_type(ByteSpan(*blob)) + " key");
  return std::make_pair(type, std::move(*blob));
}

bool ssh_verify_signature(ByteSpan key, ByteSpan signature, ByteSpan data) {
  Cursor kc{key}, sc{signature};
  std::string ktype = kc.text();
  std::string salg = sc.text();
  ByteSpan sig = sc.str();
  if (kc.bad || sc.bad || sc.pos != signature.size()) return false;
  if (ktype == "ssh-ed25519") {
    ByteSpan pub = kc.str();
    if (kc.bad || salg != "ssh-ed25519" || pub.size() != 32 || sig.size() != 64) return false;
    crypto::Point32 p{};
    crypto::Sig64 s{};
    std::memcpy(p.data(), pub.data(), 32);
    std::memcpy(s.data(), sig.data(), 64);
    return crypto::ed25519_verify(data, s, p);
  }
  if (ktype == "ssh-rsa") {
    ByteSpan e = kc.str(), n = kc.str();
    if (kc.bad) return false;
    crypto::RsaPublicKey pk;
    pk.exponent = mpint_value(e);
    pk.modulus = mpint_value(n);
    if (!pk.valid()) return false;
    if (salg == "rsa-sha2-256") return crypto::rsa_verify_pkcs1(pk, crypto::RsaDigest::Sha256, data, sig);
    if (salg == "rsa-sha2-512") return crypto::rsa_verify_pkcs1(pk, crypto::RsaDigest::Sha512, data, sig);
    if (salg == "ssh-rsa") return rsa_verify_sha1(pk, data, sig);
    return false;
  }
  if (ktype.rfind("ecdsa-sha2-", 0) == 0) {
    std::string curve_name = kc.text();
    ByteSpan q = kc.str();
    crypto::NistCurve curve;
    if (kc.bad || salg != ktype || ktype != "ecdsa-sha2-" + curve_name || !ecdsa_curve_of(curve_name, curve)) return false;
    Cursor rs{sig};
    ByteSpan r = rs.str(), s = rs.str();
    if (rs.bad || rs.pos != sig.size()) return false;
    ByteBuffer rv = mpint_value(r), sv = mpint_value(s), h = ecdsa_digest(curve, data);
    return crypto::ecdsa_verify(curve, q, ByteSpan(h), ByteSpan(rv), ByteSpan(sv));
  }
  return false;
}

namespace {

// The SSH signature blob (string alg, string sig) a key held here makes over `data`.
Result<ByteBuffer> sign_with_identity(const SshIdentity& id, ByteSpan data, const std::string& algorithm) {
  Buf out;
  if (id.type == "ssh-rsa") {
    if (algorithm != "rsa-sha2-256" && algorithm != "rsa-sha2-512")
      return Result<ByteBuffer>(Status::Unsupported, "an RSA key signs with rsa-sha2-256 or rsa-sha2-512, not " + algorithm);
    auto sig = crypto::rsa_sign_pkcs1(id.rsa, algorithm == "rsa-sha2-512" ? crypto::RsaDigest::Sha512 : crypto::RsaDigest::Sha256, data);
    if (!sig.ok()) return Result<ByteBuffer>(sig.error());
    out.str(algorithm);
    out.str(ByteSpan(sig->data(), sig->size()));
    return out.b;
  }
  if (id.type == "ssh-ed25519") {
    auto sig = crypto::ed25519_sign(data, id.seed, id.pub);
    out.str(std::string("ssh-ed25519"));
    out.str(ByteSpan(sig.data(), sig.size()));
    return out.b;
  }
  if (id.type.rfind("ecdsa-sha2-", 0) == 0) {
    ByteBuffer h = ecdsa_digest(id.ec_curve, data);
    auto der = crypto::ecdsa_sign(id.ec_curve, ByteSpan(id.ec_d), ByteSpan(h));
    if (!der.ok()) return Result<ByteBuffer>(der.error());
    // DER SEQUENCE { INTEGER r, INTEGER s } into SSH's (mpint r, mpint s)
    Der seq{ByteSpan(*der)};
    Der rs{seq.take(0x30)};
    ByteSpan r = rs.take(0x02), s = rs.take(0x02);
    if (seq.bad || rs.bad) return Result<ByteBuffer>(Status::Corrupt, "the ECDSA signature did not encode");
    Buf inner;
    inner.mpint(r);
    inner.mpint(s);
    out.str(id.type);
    out.str(inner.span());
    return out.b;
  }
  return Result<ByteBuffer>(Status::Unsupported, "this machine cannot sign with a " + id.type + " key");
}

// A key held here: signs in process, with the algorithm the session chose.
class IdentitySigner final : public SshSigner {
 public:
  IdentitySigner(SshIdentity id, std::string where) : id_(std::move(id)), where_(std::move(where)) {}
  ByteBuffer public_blob() const override { return id_.public_blob(); }
  std::string key_type() const override { return id_.type; }
  std::string describe() const override { return where_; }
  Result<ByteBuffer> sign(ByteSpan data, const std::string& algorithm) override { return sign_with_identity(id_, data, algorithm); }

 private:
  SshIdentity id_;
  std::string where_;
};

// A key file, opened when the server says it would take the key (a passphrase is asked for then, not before).
class FileSigner final : public SshSigner {
 public:
  FileSigner(hal::Host& host, std::string path, bool batch, std::string type, ByteBuffer blob, std::optional<SshIdentity> id)
      : host_(host), path_(std::move(path)), batch_(batch), type_(std::move(type)), blob_(std::move(blob)), id_(std::move(id)) {}
  ByteBuffer public_blob() const override { return blob_; }
  std::string key_type() const override { return type_; }
  std::string describe() const override { return path_; }
  Result<ByteBuffer> sign(ByteSpan data, const std::string& algorithm) override {
    if (!id_) {
      auto opened = unlock();
      if (!opened.ok()) return Result<ByteBuffer>(opened.error());
    }
    return sign_with_identity(*id_, data, algorithm);
  }
  Result<void> unlock() {
    auto pem = host_.read_file(path_);
    if (!pem.ok()) return Result<void>(Status::NotFound, path_ + ": " + pem.error().message);
    if (batch_) return Result<void>(Status::Denied, path_ + ": the key is encrypted and BatchMode allows no passphrase to be asked for");
    for (int attempt = 0; attempt < 3; ++attempt) {
      auto pass = hal::tty_ask("Enter passphrase for key '" + path_ + "': ", false);
      if (!pass.ok()) return Result<void>(Status::Denied, path_ + ": the key is encrypted and there is no terminal to ask for its passphrase");
      if (pass->empty()) return Result<void>(Status::Denied, path_ + ": no passphrase given");
      auto id = ssh_parse_private_key(ByteSpan(pem->data(), pem->size()), *pass);
      if (id.ok()) {
        if (id->public_blob() != blob_)
          return Result<void>(Status::Corrupt, path_ + ": the private key is not the one its .pub names");
        id_ = std::move(*id);
        return Ok();
      }
      if (id.error().status != Status::Denied) return Result<void>(id.error().status, path_ + ": " + id.error().message);
    }
    return Result<void>(Status::Denied, path_ + ": three wrong passphrases");
  }

 private:
  hal::Host& host_;
  std::string path_;
  bool batch_;
  std::string type_;
  ByteBuffer blob_;
  std::optional<SshIdentity> id_;
};

}  // namespace

std::unique_ptr<SshSigner> ssh_identity_signer(SshIdentity id, std::string where) {
  return std::unique_ptr<SshSigner>(new IdentitySigner(std::move(id), std::move(where)));
}

bool ssh_private_key_encrypted(ByteSpan pem) {
  auto block = first_pem(string_of(pem));
  if (!block.ok() || !block->found) return false;
  if (block->label == "OPENSSH PRIVATE KEY") {
    auto env = openssh_envelope(ByteSpan(block->der));
    return env.ok() && (env->cipher != "none" || env->kdf != "none");
  }
  if (block->label == "ENCRYPTED PRIVATE KEY") return true;
  return header_of(*block, "Proc-Type").find("ENCRYPTED") != std::string::npos;
}

Result<std::unique_ptr<SshSigner>> ssh_file_signer(hal::Host& host, const std::string& path, bool batch) {
  using R = Result<std::unique_ptr<SshSigner>>;
  auto pem = host.read_file(path);
  if (!pem.ok()) return R(Status::NotFound, path + ": no such key file");
  ByteSpan text(pem->data(), pem->size());
  if (!ssh_private_key_encrypted(text)) {
    auto id = ssh_parse_private_key(text);
    if (!id.ok()) return R(id.error().status, path + ": " + id.error().message);
    ByteBuffer blob = id->public_blob();
    std::string type = id->type;
    return R(std::unique_ptr<SshSigner>(new FileSigner(host, path, batch, type, std::move(blob), std::move(*id))));
  }
  // encrypted: the public half from beside it, or from the OpenSSH envelope (which keeps it in the clear)
  std::optional<std::pair<std::string, ByteBuffer>> pub;
  if (auto line = host.read_file(path + ".pub"); line.ok()) {
    auto parsed = ssh_parse_public_line(string_of(ByteSpan(line->data(), line->size())));
    if (parsed.ok()) pub = std::move(*parsed);
  }
  if (!pub) {
    auto block = first_pem(string_of(text));
    if (block.ok() && block->found && block->label == "OPENSSH PRIVATE KEY") {
      auto env = openssh_envelope(ByteSpan(block->der));
      if (env.ok()) pub = std::make_pair(ssh_blob_type(env->public_blob), ByteBuffer(env->public_blob.begin(), env->public_blob.end()));
    }
  }
  if (pub) return R(std::unique_ptr<SshSigner>(new FileSigner(host, path, batch, pub->first, std::move(pub->second), std::nullopt)));
  // nothing names the public half: unlock it now, as ssh does
  if (batch) return R(Status::Denied, path + ": the key is encrypted, has no .pub beside it, and BatchMode allows no question at the terminal");
  for (int attempt = 0; attempt < 3; ++attempt) {
    auto pass = hal::tty_ask("Enter passphrase for key '" + path + "': ", false);
    if (!pass.ok()) return R(Status::Denied, path + ": the key is encrypted and there is no terminal to ask for its passphrase");
    if (pass->empty()) return R(Status::Denied, path + ": no passphrase given");
    auto id = ssh_parse_private_key(text, *pass);
    if (id.ok()) {
      ByteBuffer blob = id->public_blob();
      std::string type = id->type;
      return R(std::unique_ptr<SshSigner>(new FileSigner(host, path, batch, type, std::move(blob), std::move(*id))));
    }
    if (id.error().status != Status::Denied) return R(id.error().status, path + ": " + id.error().message);
  }
  return R(Status::Denied, path + ": three wrong passphrases");
}

Result<SshIdentity> ssh_parse_private_key(ByteSpan pem, const std::string& passphrase) {
  auto block = first_pem(string_of(pem));
  if (!block.ok()) return Result<SshIdentity>(block.error());
  if (!block->found) return Result<SshIdentity>(Status::Invalid, "not a private key file (no PEM block)");
  const std::string& label = block->label;
  const bool legacy_encrypted = header_of(*block, "Proc-Type").find("ENCRYPTED") != std::string::npos;
  auto der = [&]() -> Result<ByteBuffer> {
    if (legacy_encrypted) return open_legacy_pem(*block, passphrase);
    return block->der;
  };
  if (label == "OPENSSH PRIVATE KEY") {
    auto env = openssh_envelope(ByteSpan(block->der));
    if (!env.ok()) return Result<SshIdentity>(env.error());
    auto priv = openssh_open(*env, passphrase);
    if (!priv.ok()) return Result<SshIdentity>(priv.error());
    auto id = openssh_identity(ByteSpan(*priv));
    if (id.ok() && id->public_blob() != ByteBuffer(env->public_blob.begin(), env->public_blob.end()))
      return Result<SshIdentity>(Status::Corrupt, "the private key does not match the public key stored with it");
    return id;
  }
  if (label == "RSA PRIVATE KEY") {
    auto d = der();
    if (!d.ok()) return Result<SshIdentity>(d.error());
    auto k = crypto::rsa_parse_private_key(ByteSpan(*d));
    if (!k.ok()) return Result<SshIdentity>(legacy_encrypted ? Status::Denied : k.error().status,
                                            legacy_encrypted ? "incorrect passphrase" : k.error().message);
    return rsa_identity(*k);
  }
  if (label == "EC PRIVATE KEY") {
    auto d = der();
    if (!d.ok()) return Result<SshIdentity>(d.error());
    auto id = sec1_identity(ByteSpan(*d), nullptr);
    if (!id.ok() && legacy_encrypted) return Result<SshIdentity>(Status::Denied, "incorrect passphrase");
    return id;
  }
  if (label == "PRIVATE KEY") return pkcs8_identity(ByteSpan(block->der));
  if (label == "ENCRYPTED PRIVATE KEY") {
    auto d = open_pkcs8(ByteSpan(block->der), passphrase);
    if (!d.ok()) return Result<SshIdentity>(d.error());
    auto id = pkcs8_identity(ByteSpan(*d));
    if (!id.ok()) return Result<SshIdentity>(Status::Denied, "incorrect passphrase");
    return id;
  }
  if (label == "DSA PRIVATE KEY")
    return Result<SshIdentity>(Status::Unsupported, "a DSA key: OpenSSH removed DSA, and so does this machine");
  return Result<SshIdentity>(Status::Invalid, "not a private key this machine reads (" + label + ")");
}

SshIdentity ssh_generate_identity(hal::Host& host, const std::string& comment) {
  SshIdentity id;
  id.type = "ssh-ed25519";
  host.random(MutableByteSpan(id.seed.data(), id.seed.size()));
  id.pub = crypto::ed25519_public_from_seed(id.seed);
  id.comment = comment;
  id.valid = true;
  return id;
}

std::string ssh_write_private_key(const SshIdentity& id) {
  Buf priv;
  priv.u32(0x41494f53);        // the check words: any value, both the same
  priv.u32(0x41494f53);
  if (id.type == "ssh-rsa") {
    priv.str(std::string("ssh-rsa"));
    priv.mpint(ByteSpan(id.rsa.modulus.data(), id.rsa.modulus.size()));
    priv.mpint(ByteSpan(id.rsa.public_exponent.data(), id.rsa.public_exponent.size()));
    priv.mpint(ByteSpan(id.rsa.private_exponent.data(), id.rsa.private_exponent.size()));
    priv.mpint(ByteSpan(id.rsa.coefficient.data(), id.rsa.coefficient.size()));
    priv.mpint(ByteSpan(id.rsa.prime1.data(), id.rsa.prime1.size()));
    priv.mpint(ByteSpan(id.rsa.prime2.data(), id.rsa.prime2.size()));
    priv.str(id.comment);
    for (uint8_t pad = 1; priv.b.size() % 8; ++pad) priv.b.push_back(pad);
  } else {
    priv.str(std::string("ssh-ed25519"));
    priv.str(ByteSpan(id.pub.data(), id.pub.size()));
    {
      ByteBuffer sk(id.seed.begin(), id.seed.end());
      sk.insert(sk.end(), id.pub.begin(), id.pub.end());
      priv.str(ByteSpan(sk.data(), sk.size()));
    }
    priv.str(id.comment);
    for (uint8_t pad = 1; priv.b.size() % 8; ++pad) priv.b.push_back(pad);
  }

  Buf out;
  const char* magic = "openssh-key-v1";
  out.raw(ByteSpan((const uint8_t*)magic, std::strlen(magic)));
  out.u8(0);
  out.str(std::string("none"));
  out.str(std::string("none"));
  out.str(std::string(""));
  out.u32(1);
  {
    auto blob = id.public_blob();
    out.str(ByteSpan(blob.data(), blob.size()));
  }
  out.str(ByteSpan(priv.b.data(), priv.b.size()));

  std::string b64 = base64_encode(ByteSpan(out.b.data(), out.b.size()));
  std::string pem = "-----BEGIN OPENSSH PRIVATE KEY-----\n";
  for (size_t i = 0; i < b64.size(); i += 70) pem += b64.substr(i, 70) + "\n";
  pem += "-----END OPENSSH PRIVATE KEY-----\n";
  return pem;
}

Result<SshIdentity> ssh_load_identity(hal::Host& host, const std::string& path, bool create_if_missing,
                                      const std::string& comment,
                                      const std::string& passphrase) {
  auto pem = host.read_file(path);
  if (pem.ok()) {
    auto id = ssh_parse_private_key(ByteSpan(pem->data(), pem->size()), passphrase);
    if (!id.ok()) return Result<SshIdentity>(id.error().status, path + ": " + id.error().message);
    return id;
  }
  if (!create_if_missing)
    return Result<SshIdentity>(Status::NotFound, path + ": this machine holds no such key");
  auto id = ssh_generate_identity(host, comment);
  size_t slash = path.rfind('/');
  if (slash != std::string::npos) host.make_dir(path.substr(0, slash));
  std::string text = ssh_write_private_key(id);
  if (auto r = host.write_file(path, span_of(text)); !r.ok()) return Result<SshIdentity>(r.error());
  // A private key is this machine's alone, wherever the substrate can say so.
  if (auto r = host.set_access(path, hal::Host::FileAccess::Private); !r.ok()) return Result<SshIdentity>(r.error());
  std::string pub = id.public_line() + "\n";
  if (auto r = host.write_file(path + ".pub", span_of(pub)); !r.ok()) return Result<SshIdentity>(r.error());
  return id;
}

Result<std::vector<SshKnownHost>> ssh_known_hosts(hal::Host& host, const std::string& path) {
  std::vector<SshKnownHost> out;
  auto f = host.read_file(path);
  if (!f.ok()) return out;                    // never met anyone yet, which is not an error
  std::string text = string_of(ByteSpan(f->data(), f->size()));
  size_t i = 0;
  while (i < text.size()) {
    size_t j = text.find('\n', i);
    if (j == std::string::npos) j = text.size();
    std::string row = text.substr(i, j - i);
    i = j + 1;
    if (row.empty() || row[0] == '#') continue;
    size_t sp1 = row.find(' ');
    if (sp1 == std::string::npos) continue;
    size_t sp2 = row.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) continue;
    SshKnownHost k;
    k.host = row.substr(0, sp1);
    k.algorithm = row.substr(sp1 + 1, sp2 - sp1 - 1);
    auto blob = base64_decode(row.substr(sp2 + 1));
    k.fingerprint = blob.ok() ? fingerprint_of(ByteSpan(blob->data(), blob->size())) : std::string("unreadable");
    out.push_back(k);
  }
  return out;
}

std::string SshTarget::str() const {
  std::string s = user.empty() ? host : user + "@" + host;
  if (port != 22) s += ":" + [](uint16_t p) { std::string d; while (p) { d.insert(d.begin(), (char)('0' + p % 10)); p /= 10; } return d.empty() ? std::string("0") : d; }(port);
  return s;
}

Result<SshTarget> ssh_parse_target(const std::string& s, const std::string& default_user) {
  SshTarget t;
  t.user = default_user;
  std::string rest = s;
  if (rest.rfind("ssh://", 0) == 0) rest = rest.substr(6);
  size_t at = rest.find('@');
  if (at != std::string::npos) {
    t.user = rest.substr(0, at);
    rest = rest.substr(at + 1);
  }
  size_t colon = rest.find(':');
  if (colon != std::string::npos) {
    uint32_t p = 0;
    for (size_t i = colon + 1; i < rest.size(); ++i) {
      if (rest[i] < '0' || rest[i] > '9') return Result<SshTarget>(Status::Invalid, "bad port in " + s);
      p = p * 10 + (uint32_t)(rest[i] - '0');
    }
    if (p == 0 || p > 65535) return Result<SshTarget>(Status::Invalid, "bad port in " + s);
    t.port = (uint16_t)p;
    rest = rest.substr(0, colon);
  }
  if (rest.empty()) return Result<SshTarget>(Status::Invalid, "no host in " + s);
  t.host = rest;
  if (t.user.empty()) return Result<SshTarget>(Status::Invalid, "no user for " + s + " (use user@host)");
  return t;
}

// ---------------------------------------------------------------------------
// The session
// ---------------------------------------------------------------------------
SshSession::SshSession(hal::Host& host, std::string known_hosts)
    : host_(host), known_hosts_(std::move(known_hosts)) {}

SshSession::~SshSession() { disconnect(); }

void SshSession::disconnect() {
  if (conn_) {
    if (encrypted_) {
      Buf b;
      b.u8(MSG_DISCONNECT);
      b.u32(11);                                   // by application
      b.str(std::string("done"));
      b.str(std::string(""));
      write_packet(ByteSpan(b.b.data(), b.b.size()));
    }
    conn_->close();
    conn_.reset();
  }
  encrypted_ = false;
}

// The transport reads exactly what was asked for or fails; every call here is
// for a length the protocol has already promised, so a timeout part-way through
// a structure means the peer stopped mid-packet and the stream is no longer in
// sync — that is an error, not "nothing yet". A timeout with nothing read at
// all is the ordinary idle case and comes back Busy.
Result<ByteBuffer> SshSession::read_exact(size_t n, int timeout_ms) {
  ByteBuffer out(n);
  if (n == 0) return out;
  if (!conn_->recv_all(MutableByteSpan(out.data(), n), timeout_ms)) {
    if (!conn_->alive()) return Result<ByteBuffer>(Status::IoError, "the peer closed the connection");
    return Result<ByteBuffer>(Status::Busy, "no data from the peer");
  }
  return out;
}

Result<void> SshSession::write_packet(ByteSpan payload) {
  if (!conn_) return Result<void>(Status::IoError, "not connected");
  ByteBuffer wire;
  if (!encrypted_) {
    size_t body = 1 + payload.size();
    size_t pad = 8 - ((4 + body) % 8);
    if (pad < 4) pad += 8;
    uint32_t len = (uint32_t)(body + pad);
    wire.push_back((uint8_t)(len >> 24)); wire.push_back((uint8_t)(len >> 16));
    wire.push_back((uint8_t)(len >> 8));  wire.push_back((uint8_t)len);
    wire.push_back((uint8_t)pad);
    wire.insert(wire.end(), payload.begin(), payload.end());
    size_t at = wire.size();
    wire.resize(at + pad);
    host_.random(MutableByteSpan(wire.data() + at, pad));
  } else if (is_gcm(cipher_c2s_)) {
    // aes-gcm: the length is sent in the clear and authenticated as AAD; the rest is a whole number of blocks.
    size_t body = 1 + payload.size();
    size_t pad = 16 - (body % 16);
    if (pad < 4) pad += 16;
    ByteBuffer inner;
    inner.push_back((uint8_t)pad);
    inner.insert(inner.end(), payload.begin(), payload.end());
    size_t at = inner.size();
    inner.resize(at + pad);
    host_.random(MutableByteSpan(inner.data() + at, pad));
    uint32_t len = (uint32_t)inner.size();
    uint8_t lenb[4] = {(uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len};
    ByteBuffer sealed = gcm_seal(key_c2s_, iv_c2s_, ByteSpan(lenb, 4), ByteSpan(inner.data(), inner.size()));
    gcm_next(iv_c2s_);
    wire.insert(wire.end(), lenb, lenb + 4);
    wire.insert(wire.end(), sealed.begin(), sealed.end());
  } else {
    // The length is not covered by the padding computation under this cipher.
    size_t body = 1 + payload.size();
    size_t pad = 8 - (body % 8);
    if (pad < 4) pad += 8;
    ByteBuffer inner;
    inner.push_back((uint8_t)pad);
    inner.insert(inner.end(), payload.begin(), payload.end());
    size_t at = inner.size();
    inner.resize(at + pad);
    host_.random(MutableByteSpan(inner.data() + at, pad));

    uint32_t len = (uint32_t)inner.size();
    uint8_t lenb[4] = {(uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len};
    crypto::Key k2 = key_of(key_c2s_, false), k1 = key_of(key_c2s_, true);
    crypto::chacha20_xor(k1, nonce_of(seq_out_), 0, lenb, 4);
    crypto::chacha20_xor(k2, nonce_of(seq_out_), 1, inner.data(), inner.size());

    wire.insert(wire.end(), lenb, lenb + 4);
    wire.insert(wire.end(), inner.begin(), inner.end());
    auto tag = crypto::poly1305(poly_key_for(k2, seq_out_), ByteSpan(wire.data(), wire.size()));
    wire.insert(wire.end(), tag.begin(), tag.end());
  }
  ++seq_out_;
  bytes_out_ += wire.size();
  if (!conn_->send_all(ByteSpan(wire.data(), wire.size())))
    return Result<void>(Status::IoError, "the connection dropped while sending");
  return Ok();
}

Result<ByteBuffer> SshSession::read_packet(int timeout_ms) {
  if (!conn_) return Result<ByteBuffer>(Status::IoError, "not connected");
  if (!encrypted_) {
    auto head = read_exact(4, timeout_ms);
    if (!head.ok()) return head;
    uint32_t len = ((uint32_t)(*head)[0] << 24) | ((uint32_t)(*head)[1] << 16) | ((uint32_t)(*head)[2] << 8) | (*head)[3];
    if (len < 8 || len > (1u << 20)) return Result<ByteBuffer>(Status::Corrupt, "implausible packet length");
    auto body = read_exact(len, timeout_ms);
    if (!body.ok()) return body;
    bytes_in_ += 4 + len;
    ++seq_in_;                      // every packet counts, encrypted or not
    uint8_t pad = (*body)[0];
    if ((size_t)pad + 1 > body->size()) return Result<ByteBuffer>(Status::Corrupt, "padding longer than the packet");
    return ByteBuffer(body->begin() + 1, body->end() - pad);
  }
  auto head = read_exact(4, timeout_ms);
  if (!head.ok()) return head;
  if (is_gcm(cipher_s2c_)) {
    uint32_t len = ((uint32_t)(*head)[0] << 24) | ((uint32_t)(*head)[1] << 16) | ((uint32_t)(*head)[2] << 8) | (*head)[3];
    if (len < 16 || len > (1u << 20) || len % 16) return Result<ByteBuffer>(Status::Corrupt, "implausible packet length");
    auto rest = read_exact((size_t)len + 16, timeout_ms);
    if (!rest.ok()) return rest;
    bytes_in_ += 4 + len + 16;
    ByteBuffer inner;
    if (!gcm_open(key_s2c_, iv_s2c_, ByteSpan(head->data(), 4), ByteSpan(rest->data(), rest->size()), inner))
      return Result<ByteBuffer>(Status::Denied, "packet authentication tag mismatch");
    gcm_next(iv_s2c_);
    ++seq_in_;
    uint8_t pad = inner[0];
    if ((size_t)pad + 1 > inner.size()) return Result<ByteBuffer>(Status::Corrupt, "padding longer than the packet");
    return ByteBuffer(inner.begin() + 1, inner.end() - pad);
  }
  crypto::Key k2 = key_of(key_s2c_, false), k1 = key_of(key_s2c_, true);
  uint8_t lenb[4];
  std::memcpy(lenb, head->data(), 4);
  uint8_t plain_len[4];
  std::memcpy(plain_len, lenb, 4);
  crypto::chacha20_xor(k1, nonce_of(seq_in_), 0, plain_len, 4);
  uint32_t len = ((uint32_t)plain_len[0] << 24) | ((uint32_t)plain_len[1] << 16) | ((uint32_t)plain_len[2] << 8) | plain_len[3];
  if (len < 8 || len > (1u << 20)) return Result<ByteBuffer>(Status::Corrupt, "implausible packet length");

  auto rest = read_exact((size_t)len + 16, timeout_ms);
  if (!rest.ok()) return rest;
  bytes_in_ += 4 + len + 16;

  ByteBuffer mac_input;
  mac_input.insert(mac_input.end(), lenb, lenb + 4);
  mac_input.insert(mac_input.end(), rest->begin(), rest->begin() + (long)len);
  auto want = crypto::poly1305(poly_key_for(k2, seq_in_), ByteSpan(mac_input.data(), mac_input.size()));
  unsigned diff = 0;
  for (int i = 0; i < 16; ++i) diff |= (unsigned)(want[(size_t)i] ^ (*rest)[len + (size_t)i]);
  if (diff) return Result<ByteBuffer>(Status::Denied, "packet authentication tag mismatch");

  ByteBuffer inner(rest->begin(), rest->begin() + (long)len);
  crypto::chacha20_xor(k2, nonce_of(seq_in_), 1, inner.data(), inner.size());
  ++seq_in_;
  uint8_t pad = inner[0];
  if ((size_t)pad + 1 > inner.size()) return Result<ByteBuffer>(Status::Corrupt, "padding longer than the packet");
  return ByteBuffer(inner.begin() + 1, inner.end() - pad);
}

Result<void> SshSession::connect(const SshTarget& target, const SshIdentity& identity, int timeout_ms) {
  auto c = hal::net_connect(target.host, target.port, timeout_ms);
  if (!c.ok()) return Result<void>(c.error());
  conn_ = std::move(*c);
  if (auto r = handshake(target, timeout_ms); !r.ok()) { disconnect(); return r; }
  if (auto r = authenticate(target, identity, timeout_ms); !r.ok()) { disconnect(); return r; }
  return Ok();
}

void SshSession::configure(const SshConnectOptions& o) {
  offer_kex_ = offer_list(o.kex_algorithms, kKexAlgs);
  offer_host_keys_ = offer_list(o.host_key_algorithms, kHostKeyAlgs);
  offer_ciphers_ = offer_list(o.ciphers, kCiphers);
  verify_host_key_ = o.verify_host_key;
}

Result<void> SshSession::connect(const SshTarget& target, SshConnectOptions& options) {
  if (!options.verify_host_key)
    return Result<void>(Status::Invalid, "a session needs something to decide its host key (SshConnectOptions::verify_host_key)");
  if (target.user.empty()) return Result<void>(Status::Invalid, "no user to log in as on " + target.host);
  if (!options.jumps.empty()) {
    auto stream = options.jumps.back()->open_direct_tcpip(target.host, target.port);
    if (!stream.ok()) return Result<void>(stream.error().status, "through the ProxyJump hop: " + stream.error().message);
    jumps_ = std::move(options.jumps);
    options.jumps.clear();
    return connect_over(std::move(*stream), target, options);
  }
  auto c = hal::net_connect(target.host, target.port, options.timeout_ms);
  if (!c.ok()) return Result<void>(c.error().status, "ssh: connect to host " + target.host + " port " + std::to_string(target.port) + ": " + c.error().message);
  return connect_over(std::move(*c), target, options);
}

Result<void> SshSession::connect_over(std::unique_ptr<hal::Connection> stream, const SshTarget& target, SshConnectOptions& options) {
  if (!options.verify_host_key)
    return Result<void>(Status::Invalid, "a session needs something to decide its host key (SshConnectOptions::verify_host_key)");
  configure(options);
  conn_ = std::move(stream);
  if (auto r = handshake(target, options.timeout_ms); !r.ok()) { disconnect(); return r; }
  if (auto r = authenticate_signers(target, options.signers, options.timeout_ms); !r.ok()) { disconnect(); return r; }
  return Ok();
}

Result<void> SshSession::handshake(const SshTarget& t, int timeout_ms) {
  client_ident_ = "SSH-2.0-AIOS_1.0";
  std::string line = client_ident_ + "\r\n";
  if (!conn_->send_all(span_of(line))) return Result<void>(Status::IoError, "could not send the version string");

  // The server may send banner lines before its identification.
  for (int guard = 0; guard < 64; ++guard) {
    std::string got;
    for (;;) {
      auto b = read_exact(1, timeout_ms);
      if (!b.ok()) return Result<void>(b.error());
      char ch = (char)(*b)[0];
      if (ch == '\n') break;
      if (ch != '\r') got += ch;
      if (got.size() > 512) return Result<void>(Status::Corrupt, "version line too long");
    }
    if (got.rfind("SSH-", 0) == 0) { server_ident_ = got; break; }
  }
  if (server_ident_.rfind("SSH-2.0", 0) != 0)
    return Result<void>(Status::Unsupported, "the peer is not an SSH-2.0 server: " + server_ident_);

  return do_kex(&t, nullptr, timeout_ms);
}

Result<void> SshSession::rekey(int timeout_ms) {
  if (!conn_ || !encrypted_) return Result<void>(Status::Invalid, "not connected or not encrypted");
  return do_kex(nullptr, nullptr, timeout_ms);
}

Result<void> SshSession::do_kex(const SshTarget* target_if_initial, const ByteBuffer* server_kexinit_opt, int timeout_ms) {
  // --- KEXINIT ---
  const std::string kex_offer = offer_kex_.empty() ? kKexAlgs : offer_kex_;
  const std::string hk_offer = offer_host_keys_.empty() ? kHostKeyAlgs : offer_host_keys_;
  const std::string cipher_offer = offer_ciphers_.empty() ? kCiphers : offer_ciphers_;
  if (kex_offer.empty() || hk_offer.empty() || cipher_offer.empty())
    return Result<void>(Status::Unsupported, "the configured KexAlgorithms, HostKeyAlgorithms or Ciphers name nothing this "
                                             "client implements (it has " + std::string(kKexAlgs) + "; " + kHostKeyAlgs + "; " + kCiphers + ")");
  Buf ci;
  ci.u8(MSG_KEXINIT);
  {
    uint8_t cookie[16];
    host_.random(MutableByteSpan(cookie, 16));
    ci.raw(ByteSpan(cookie, 16));
  }
  ci.str(kex_offer);
  ci.str(hk_offer);
  ci.str(cipher_offer);                // client to server
  ci.str(cipher_offer);                // server to client
  ci.str(std::string(kMacs));
  ci.str(std::string(kMacs));
  ci.str(std::string(kCompression));
  ci.str(std::string(kCompression));
  ci.str(std::string(""));             // languages
  ci.str(std::string(""));
  ci.boolean(false);                   // no guessed packet follows
  ci.u32(0);
  if (auto r = write_packet(ByteSpan(ci.b.data(), ci.b.size())); !r.ok()) return r;

  auto read_kex_packet = [&](uint8_t expected_type) -> Result<ByteBuffer> {
    for (;;) {
      auto p = read_packet(timeout_ms);
      if (!p.ok()) return Result<ByteBuffer>(p.error());
      if (p->empty()) continue;
      uint8_t type = (*p)[0];
      if (type == expected_type) return *p;
      if (type == MSG_DEBUG || type == MSG_IGNORE || type == MSG_UNIMPLEMENTED) continue;
      if (type == MSG_DISCONNECT) {
        Cursor c{ByteSpan(p->data(), p->size())};
        c.u8(); c.u32();
        return Result<ByteBuffer>(Status::Denied, "the server disconnected: " + c.text());
      }
      if (type == MSG_GLOBAL_REQUEST) {
        Cursor c{ByteSpan(p->data(), p->size())};
        c.u8(); c.text();
        if (c.boolean()) {
          Buf b;
          b.u8(MSG_REQUEST_FAILURE);
          if (auto r = write_packet(ByteSpan(b.b.data(), b.b.size())); !r.ok()) return Result<ByteBuffer>(r.error());
        }
        continue;
      }
      if (type == MSG_CHANNEL_DATA) {
        Cursor c{ByteSpan(p->data(), p->size())};
        c.u8(); c.u32();
        auto d = c.str();
        rx_data_.insert(rx_data_.end(), d.begin(), d.end());
        if (d.size() < local_window_) local_window_ -= (uint32_t)d.size(); else local_window_ = 0;
        if (local_window_ < (1u << 19)) {
          uint32_t add = (1u << 21) - local_window_;
          Buf b;
          b.u8(MSG_CHANNEL_WINDOW_ADJUST);
          b.u32(remote_channel_);
          b.u32(add);
          if (auto r = write_packet(ByteSpan(b.b.data(), b.b.size())); !r.ok()) return Result<ByteBuffer>(r.error());
          local_window_ += add;
        }
        continue;
      }
      if (type == MSG_CHANNEL_WINDOW_ADJUST) {
        Cursor c{ByteSpan(p->data(), p->size())};
        c.u8(); c.u32();
        remote_window_ += c.u32();
        continue;
      }
      if (type == MSG_CHANNEL_EXTENDED_DATA) {
        Cursor c{ByteSpan(p->data(), p->size())};
        c.u8(); c.u32(); c.u32();
        stderr_ += c.text();
        continue;
      }
      if (type == MSG_CHANNEL_EOF) {
        channel_eof_ = true;
        continue;
      }
      if (type == MSG_CHANNEL_CLOSE) {
        channel_closed_ = channel_eof_ = true;
        continue;
      }
      if (type == MSG_CHANNEL_REQUEST) {
        Cursor c{ByteSpan(p->data(), p->size())};
        c.u8(); c.u32();
        std::string what = c.text();
        bool want_reply = c.boolean();
        if (what == "exit-status") exit_status_ = (int)c.u32();
        if (want_reply) {
          Buf b;
          b.u8(MSG_CHANNEL_FAILURE);
          b.u32(remote_channel_);
          if (auto r = write_packet(ByteSpan(b.b.data(), b.b.size())); !r.ok()) return Result<ByteBuffer>(r.error());
        }
        continue;
      }
      return Result<ByteBuffer>(Status::Corrupt, "unexpected message type " + std::to_string(type) + " during key exchange");
    }
  };

  ByteBuffer server_kexinit;
  if (server_kexinit_opt) {
    server_kexinit = *server_kexinit_opt;
  } else {
    auto p = read_kex_packet(MSG_KEXINIT);
    if (!p.ok()) return Result<void>(p.error());
    server_kexinit = std::move(*p);
  }
  {
    Cursor c{ByteSpan(server_kexinit.data(), server_kexinit.size())};
    c.u8();
    c.pos += 16;                       // cookie
    std::string kex = c.text(), hostkeys = c.text(), enc_cs = c.text(), enc_sc = c.text();
    if (c.bad) return Result<void>(Status::Corrupt, "malformed KEXINIT");
    // the first of the client's names the server also has (RFC 4253 7.1)
    auto pick = [&](const std::string& theirs, const std::string& list, const char* what) -> Result<std::string> {
      size_t i = 0;
      while (i <= list.size()) {
        size_t j = list.find(',', i);
        if (j == std::string::npos) j = list.size();
        std::string cand = list.substr(i, j - i);
        if (list_has(theirs, cand)) return cand;
        if (j == list.size()) break;
        i = j + 1;
      }
      return Result<std::string>(Status::Unsupported,
                                std::string("no ") + what + " in common with " + server_ident_ + " (it offers " + theirs + ")");
    };
    auto k = pick(kex, kex_offer, "key exchange");
    if (!k.ok()) return Result<void>(k.error());
    kex_ = *k;
    auto hk = pick(hostkeys, hk_offer, "host key algorithm");
    if (!hk.ok()) return Result<void>(hk.error());
    host_key_alg_ = *hk;
    auto e1 = pick(enc_cs, cipher_offer, "cipher");
    if (!e1.ok()) return Result<void>(e1.error());
    auto e2 = pick(enc_sc, cipher_offer, "cipher");
    if (!e2.ok()) return Result<void>(e2.error());
    cipher_ = *e1;
    cipher_c2s_pending_ = *e1;
    cipher_s2c_pending_ = *e2;
  }

  // --- the key exchange: curve25519 (RFC 8731) or ECDH over a NIST curve (RFC 5656) ---
  const bool nist = kex_.rfind("ecdh-sha2-", 0) == 0;
  crypto::NistCurve curve = crypto::NistCurve::P256;
  if (kex_ == "ecdh-sha2-nistp384") curve = crypto::NistCurve::P384;
  if (kex_ == "ecdh-sha2-nistp521") curve = crypto::NistCurve::P521;
  crypto::Scalar32 secret{};
  ByteBuffer nist_secret, q_c;
  if (nist) {
    const size_t width = curve == crypto::NistCurve::P521 ? 66 : curve == crypto::NistCurve::P384 ? 48 : 32;
    for (int tries = 0;; ++tries) {
      nist_secret.assign(width, 0);
      host_.random(MutableByteSpan(nist_secret.data(), width));
      if (curve == crypto::NistCurve::P521) nist_secret[0] &= 0x01;   // below 2^521, as the order is
      auto pub = crypto::ecdsa_public_key(curve, ByteSpan(nist_secret));
      if (pub.ok()) { q_c = std::move(*pub); break; }
      if (tries > 64) return Result<void>(Status::IoError, "the random source gave no usable ECDH scalar");
    }
  } else {
    host_.random(MutableByteSpan(secret.data(), secret.size()));
    secret[0] &= 248; secret[31] &= 127; secret[31] |= 64;
    crypto::Point32 pub = crypto::x25519_base(secret);
    q_c.assign(pub.begin(), pub.end());
  }

  Buf init;
  init.u8(MSG_KEX_ECDH_INIT);
  init.str(ByteSpan(q_c.data(), q_c.size()));
  if (auto r = write_packet(ByteSpan(init.b.data(), init.b.size())); !r.ok()) return r;

  auto rep = read_kex_packet(MSG_KEX_ECDH_REPLY);
  if (!rep.ok()) return Result<void>(rep.error());
  ByteBuffer reply = std::move(*rep);
  Cursor rc{ByteSpan(reply.data(), reply.size())};
  rc.u8();
  ByteSpan k_s = rc.str();
  ByteSpan q_s = rc.str();
  ByteSpan sig = rc.str();
  if (rc.bad) return Result<void>(Status::Corrupt, "malformed key exchange reply");

  ByteBuffer shared;
  if (nist) {
    auto x = crypto::ecdh_shared_x(curve, ByteSpan(nist_secret), q_s);
    if (!x.ok()) return Result<void>(Status::Denied, "the server's key exchange point: " + x.error().message);
    shared = std::move(*x);
  } else {
    if (q_s.size() != 32) return Result<void>(Status::Corrupt, "malformed key exchange reply");
    crypto::Point32 peer{}, out{};
    std::memcpy(peer.data(), q_s.data(), 32);
    if (!crypto::x25519(out, secret, peer))
      return Result<void>(Status::Denied, "the server's key exchange point has small order");
    shared.assign(out.begin(), out.end());
  }

  // The exchange hash covers both identifications, both KEXINITs, the host key,
  // both public values and the shared secret.
  Buf h;
  h.str(client_ident_);
  h.str(server_ident_);
  h.str(ByteSpan(ci.b.data(), ci.b.size()));
  h.str(ByteSpan(server_kexinit.data(), server_kexinit.size()));
  h.str(k_s);
  h.str(ByteSpan(q_c.data(), q_c.size()));
  h.str(q_s);
  h.mpint(ByteSpan(shared.data(), shared.size()));
  ByteBuffer H = kex_hash(kex_, ByteSpan(h.b.data(), h.b.size()));

  // The host key signs the exchange hash: this is what proves the peer is the
  // machine whose key we already trust.
  {
    std::string algo = ssh_blob_type(k_s);
    // the key must be of the kind both sides agreed on, and signed in exactly that algorithm
    const bool agreed = algo == host_key_alg_ || (algo == "ssh-rsa" && (host_key_alg_ == "rsa-sha2-512" || host_key_alg_ == "rsa-sha2-256"));
    if (!agreed) return Result<void>(Status::Denied, "the host presented a " + algo + " key after agreeing to " + host_key_alg_);
    Cursor sc{sig};
    std::string salgo = sc.text();
    if (sc.bad) return Result<void>(Status::Corrupt, "malformed host key signature");
    if (salgo != host_key_alg_) return Result<void>(Status::Denied, "the host signed with " + salgo + " after agreeing to " + host_key_alg_);
    if (!ssh_verify_signature(k_s, sig, ByteSpan(H.data(), H.size())))
      return Result<void>(Status::Denied, "the host key signature over the exchange hash does not verify");
    host_key_.algorithm = algo;
    host_key_.blob.assign(k_s.begin(), k_s.end());
    host_key_.fingerprint = fingerprint_of(k_s);
  }
  if (target_if_initial) {
    if (verify_host_key_) {
      if (auto r = verify_host_key_(host_key_); !r.ok()) return r;
    } else if (!known_hosts_.empty()) {
      if (auto r = check_host_key(*target_if_initial); !r.ok()) return r;
    } else {
      return Result<void>(Status::Denied, "nothing here decides whether " + target_if_initial->host + "'s host key (" + host_key_.fingerprint +
                                              ") is the right one: refused");
    }
  } else {
    // RFC 4253 §9: during rekeying, the host key must match the already-established host key.
    if (host_key_.blob.size() != k_s.size() || std::memcmp(host_key_.blob.data(), k_s.data(), k_s.size()) != 0) {
      return Result<void>(Status::Denied, "the server changed its host key during rekeying");
    }
  }

  if (session_id_.empty()) session_id_.assign(H.begin(), H.end());

  // NEWKEYS both ways, then the cipher is live.
  {
    Buf nk;
    nk.u8(MSG_NEWKEYS);
    if (auto r = write_packet(ByteSpan(nk.b.data(), nk.b.size())); !r.ok()) return r;
  }
  auto nk_res = read_kex_packet(MSG_NEWKEYS);
  if (!nk_res.ok()) return Result<void>(nk_res.error());

  // RFC 4253 7.2: HASH(K || H || letter || session_id), extended with HASH(K || H || what came before)
  auto derive = [&](char id, size_t need) {
    std::vector<uint8_t> out;
    while (out.size() < need) {
      Buf b;
      b.mpint(ByteSpan(shared.data(), shared.size()));
      b.raw(ByteSpan(H.data(), H.size()));
      if (out.empty()) {
        b.u8((uint8_t)id);
        b.raw(ByteSpan(session_id_.data(), session_id_.size()));
      } else {
        b.raw(ByteSpan(out.data(), out.size()));
      }
      ByteBuffer k = kex_hash(kex_, ByteSpan(b.b.data(), b.b.size()));
      out.insert(out.end(), k.begin(), k.end());
    }
    out.resize(need);
    return out;
  };
  auto key_len = [](const std::string& c) -> size_t { return c == "aes128-gcm@openssh.com" ? 16 : c == "aes256-gcm@openssh.com" ? 32 : 64; };
  cipher_c2s_ = cipher_c2s_pending_;
  cipher_s2c_ = cipher_s2c_pending_;
  key_c2s_ = derive('C', key_len(cipher_c2s_));
  key_s2c_ = derive('D', key_len(cipher_s2c_));
  iv_c2s_ = is_gcm(cipher_c2s_) ? derive('A', 12) : std::vector<uint8_t>();
  iv_s2c_ = is_gcm(cipher_s2c_) ? derive('B', 12) : std::vector<uint8_t>();
  // Sequence numbers are not reset by NEWKEYS: they run for the whole session.
  encrypted_ = true;
  rekey_bytes_ = bytes_in_ + bytes_out_;
  last_kex_ns_ = host_.monotonic_ns();
  return Ok();
}

Result<void> SshSession::check_host_key(const SshTarget& t) {
  std::string name = t.host;
  if (t.port != 22) {
    std::string p;
    uint16_t v = t.port;
    while (v) { p.insert(p.begin(), (char)('0' + v % 10)); v /= 10; }
    name = "[" + t.host + "]:" + p;
  }
  std::string line = name + " " + host_key_.algorithm + " " +
                     base64_encode(ByteSpan(host_key_.blob.data(), host_key_.blob.size()));

  auto existing = host_.read_file(known_hosts_);
  if (existing.ok()) {
    std::string text = string_of(ByteSpan(existing->data(), existing->size()));
    size_t i = 0;
    while (i < text.size()) {
      size_t j = text.find('\n', i);
      if (j == std::string::npos) j = text.size();
      std::string row = text.substr(i, j - i);
      i = j + 1;
      if (row.empty() || row[0] == '#') continue;
      size_t sp = row.find(' ');
      if (sp == std::string::npos || row.substr(0, sp) != name) continue;
      if (row == line) return Ok();                     // known, and unchanged
      return Result<void>(Status::Denied,
                         "the host key for " + name + " has CHANGED — this machine knows a different key for it (" +
                             host_key_.fingerprint + " now). Remove the line from " + known_hosts_ +
                             " if the host was legitimately rebuilt.");
    }
  }
  host_key_.first_seen = true;
  // Trust on first use, recorded so it cannot change unnoticed afterwards.
  size_t slash = known_hosts_.rfind('/');
  if (slash != std::string::npos) host_.make_dir(known_hosts_.substr(0, slash));
  std::string append = line + "\n";
  auto w = host_.append_file(known_hosts_, span_of(append));
  if (!w.ok()) return w;
  return Ok();
}

Result<void> SshSession::service_request(int timeout_ms) {
  {
    Buf b;
    b.u8(MSG_SERVICE_REQUEST);
    b.str(std::string("ssh-userauth"));
    if (auto r = write_packet(ByteSpan(b.b.data(), b.b.size())); !r.ok()) return r;
  }
  for (;;) {
    auto p = read_packet(timeout_ms);
    if (!p.ok()) return Result<void>(p.error());
    if (p->empty()) continue;
    uint8_t type = (*p)[0];
    if (type == MSG_SERVICE_ACCEPT) return Ok();
    if (type == MSG_EXT_INFO) {
      Cursor ec{ByteSpan(p->data(), p->size())};
      ec.u8();
      uint32_t nr = ec.u32();
      for (uint32_t i = 0; i < nr && !ec.bad; ++i) {
        std::string name = ec.text();
        std::string val = ec.text();
        if (name == "server-sig-algs") server_sig_algs_ = val;
      }
      continue;
    }
    if (type == MSG_DEBUG || type == MSG_IGNORE) continue;
    if (type == MSG_DISCONNECT) {
      Cursor c{ByteSpan(p->data(), p->size())};
      c.u8(); c.u32();
      return Result<void>(Status::Denied, "the server disconnected: " + c.text());
    }
    return Result<void>(Status::Corrupt, "expected SERVICE_ACCEPT");
  }
}

// Publickey authentication as OpenSSH does it: each key is offered without a signature first, and signs only
// once the server has said it would accept that key (USERAUTH_PK_OK), so a refused key never costs a passphrase.
Result<void> SshSession::authenticate_signers(const SshTarget& t, std::vector<std::unique_ptr<SshSigner>>& signers, int timeout_ms) {
  if (auto r = service_request(timeout_ms); !r.ok()) return r;
  std::string can_continue;
  std::vector<std::string> tried;
  // one reply to an authentication request: SUCCESS, FAILURE, PK_OK; banners and EXT_INFO are taken on the way
  auto answer = [&]() -> Result<uint8_t> {
    for (;;) {
      auto p = read_packet(timeout_ms);
      if (!p.ok()) return Result<uint8_t>(p.error());
      if (p->empty()) continue;
      Cursor c{ByteSpan(p->data(), p->size())};
      uint8_t type = c.u8();
      if (type == MSG_USERAUTH_SUCCESS || type == MSG_USERAUTH_PK_OK) return type;
      if (type == MSG_USERAUTH_FAILURE) { can_continue = c.text(); return type; }
      if (type == MSG_USERAUTH_BANNER) { stderr_ += c.text(); continue; }
      if (type == MSG_EXT_INFO) {
        uint32_t nr = c.u32();
        for (uint32_t i = 0; i < nr && !c.bad; ++i) {
          std::string name = c.text(), val = c.text();
          if (name == "server-sig-algs") server_sig_algs_ = val;
        }
        continue;
      }
      if (type == MSG_DEBUG || type == MSG_IGNORE || type == MSG_GLOBAL_REQUEST) continue;
      if (type == MSG_DISCONNECT) {
        c.u32();
        return Result<uint8_t>(Status::Denied, "the server disconnected: " + c.text());
      }
      return Result<uint8_t>(Status::Corrupt, "unexpected message during authentication");
    }
  };
  for (auto& signer : signers) {
    if (!signer) continue;
    if (!can_continue.empty() && !list_has(can_continue, "publickey")) break;
    const ByteBuffer blob = signer->public_blob();
    std::string alg = signer->key_type();
    if (alg == "ssh-rsa")
      alg = !server_sig_algs_.empty() && list_has(server_sig_algs_, "rsa-sha2-512") ? "rsa-sha2-512" : "rsa-sha2-256";
    if (!server_sig_algs_.empty() && !list_has(server_sig_algs_, alg)) {
      tried.push_back(signer->describe() + " (" + fingerprint_of(ByteSpan(blob)) + "): the server does not take " + alg + " signatures");
      continue;
    }
    auto request = [&](bool with_signature) {
      Buf b;
      b.u8(MSG_USERAUTH_REQUEST);
      b.str(t.user);
      b.str(std::string("ssh-connection"));
      b.str(std::string("publickey"));
      b.boolean(with_signature);
      b.str(alg);
      b.str(ByteSpan(blob.data(), blob.size()));
      return b;
    };
    {
      Buf q = request(false);
      if (auto r = write_packet(q.span()); !r.ok()) return r;
    }
    auto a = answer();
    if (!a.ok()) return Result<void>(a.error());
    if (*a == MSG_USERAUTH_FAILURE) { tried.push_back(signer->describe() + " (" + fingerprint_of(ByteSpan(blob)) + "): refused"); continue; }
    if (*a == MSG_USERAUTH_SUCCESS) { authenticated_with_ = signer->describe(); return Ok(); }
    Buf req = request(true);
    Buf signed_data;
    signed_data.str(ByteSpan(session_id_.data(), session_id_.size()));
    signed_data.raw(req.span());
    auto sig = signer->sign(signed_data.span(), alg);
    if (!sig.ok()) {
      tried.push_back(signer->describe() + " (" + fingerprint_of(ByteSpan(blob)) + "): accepted, but could not sign: " + sig.error().message);
      continue;
    }
    req.str(ByteSpan(sig->data(), sig->size()));
    if (auto r = write_packet(req.span()); !r.ok()) return r;
    auto done = answer();
    if (!done.ok()) return Result<void>(done.error());
    if (*done == MSG_USERAUTH_SUCCESS) { authenticated_with_ = signer->describe(); return Ok(); }
    tried.push_back(signer->describe() + " (" + fingerprint_of(ByteSpan(blob)) + "): its signature was refused");
  }
  std::string list;
  for (auto& x : tried) list += "\n  " + x;
  return Result<void>(Status::Denied, t.user + "@" + t.host + ": Permission denied (" + (can_continue.empty() ? std::string("publickey") : can_continue) + ")." +
                                          (tried.empty() ? std::string(" No key was offered: none was found in the agent or the key files.") : " Keys tried:" + list));
}

Result<void> SshSession::authenticate(const SshTarget& t, const SshIdentity& id, int timeout_ms) {
  if (!id.valid) return Result<void>(Status::Invalid, "no identity to authenticate with");
  if (auto r = service_request(timeout_ms); !r.ok()) return r;

  auto blob = id.public_blob();
  // An RSA identity signs with SHA-2 (RFC 8332): rsa-sha2-512 where the server
  // lists it in server-sig-algs, else rsa-sha2-256, which every server that
  // takes RSA keys since OpenSSH 7.2 accepts.
  // An ed25519 or ecdsa-sha2-nistp* identity signs under its own key type
  // name. (This used to say ssh-ed25519 for every non-RSA key, and sign with
  // ed25519 too, so an ECDSA key in authorized_keys could never log in here.)
  std::string alg = id.type;
  if (id.type == "ssh-rsa")
    alg = !server_sig_algs_.empty() && list_has(server_sig_algs_, "rsa-sha2-512") ? "rsa-sha2-512" : "rsa-sha2-256";
  std::string refused_can;   // the methods the server named when it refused

  auto attempt_auth = [&](const std::string& current_alg) -> Result<bool> {
    Buf signed_data;
    signed_data.str(ByteSpan(session_id_.data(), session_id_.size()));
    signed_data.u8(MSG_USERAUTH_REQUEST);
    signed_data.str(t.user);
    signed_data.str(std::string("ssh-connection"));
    signed_data.str(std::string("publickey"));
    signed_data.boolean(true);
    signed_data.str(current_alg);
    signed_data.str(ByteSpan(blob.data(), blob.size()));

    auto sig_blob = sign_with_identity(id, ByteSpan(signed_data.b.data(), signed_data.b.size()), current_alg);
    if (!sig_blob.ok()) return Result<bool>(sig_blob.error());

    Buf req;
    req.u8(MSG_USERAUTH_REQUEST);
    req.str(t.user);
    req.str(std::string("ssh-connection"));
    req.str(std::string("publickey"));
    req.boolean(true);
    req.str(current_alg);
    req.str(ByteSpan(blob.data(), blob.size()));
    req.str(ByteSpan(sig_blob->data(), sig_blob->size()));
    if (auto r = write_packet(ByteSpan(req.b.data(), req.b.size())); !r.ok()) return Result<bool>(r.error());

    for (;;) {
      auto p = read_packet(timeout_ms);
      if (!p.ok()) return Result<bool>(p.error());
      if (p->empty()) continue;
      uint8_t type = (*p)[0];
      if (type == MSG_USERAUTH_SUCCESS) return Result<bool>(true);
      if (type == MSG_EXT_INFO) {
        Cursor ec{ByteSpan(p->data(), p->size())};
        ec.u8();
        uint32_t nr = ec.u32();
        for (uint32_t i = 0; i < nr && !ec.bad; ++i) {
          std::string name = ec.text();
          std::string val = ec.text();
          if (name == "server-sig-algs") server_sig_algs_ = val;
        }
        continue;
      }
      if (type == MSG_USERAUTH_BANNER || type == MSG_DEBUG || type == MSG_IGNORE) continue;
      if (type == MSG_USERAUTH_FAILURE) {
        Cursor fc{ByteSpan(p->data(), p->size())};
        fc.u8();
        refused_can = fc.text();
        return Result<bool>(false);
      }
      if (type == MSG_DISCONNECT) {
        Cursor c{ByteSpan(p->data(), p->size())};
        c.u8(); c.u32();
        return Result<bool>(Status::Denied, "the server disconnected: " + c.text());
      }
      return Result<bool>(Status::Corrupt, "unexpected message during authentication");
    }
  };

  auto res = attempt_auth(alg);
  if (!res.ok()) return Result<void>(res.error());
  if (*res) return Ok();

  return Result<void>(Status::Denied,
                      "the server refused this identity for " + t.user + "@" + t.host + " (" + id.fingerprint() +
                          (refused_can.empty() ? std::string(")") : "); it will accept: " + refused_can));
}

// The session's one channel: "session" for a command, "direct-tcpip" for a forward. `extra` is what the type adds
// to CHANNEL_OPEN (RFC 4254 7.2 for direct-tcpip).
Result<void> SshSession::open_channel(const std::string& type_name, ByteSpan extra, int timeout_ms) {
  if (channel_open_) return Result<void>(Status::Invalid, "this session's channel is already open");
  local_channel_ = 0;
  local_window_ = 1u << 21;
  {
    Buf b;
    b.u8(MSG_CHANNEL_OPEN);
    b.str(type_name);
    b.u32(local_channel_);
    b.u32(local_window_);
    b.u32(32768);
    b.raw(extra);
    if (auto r = write_packet(ByteSpan(b.b.data(), b.b.size())); !r.ok()) return r;
  }
  for (;;) {
    auto p = read_packet(timeout_ms);
    if (!p.ok()) return Result<void>(p.error());
    if (p->empty()) continue;
    uint8_t type = (*p)[0];
    if (type == MSG_CHANNEL_OPEN_CONFIRMATION) {
      Cursor c{ByteSpan(p->data(), p->size())};
      c.u8();
      c.u32();                        // our channel
      remote_channel_ = c.u32();
      remote_window_ = c.u32();
      remote_max_packet_ = c.u32();
      if (remote_max_packet_ == 0 || remote_max_packet_ > 262144) remote_max_packet_ = 32768;
      channel_open_ = true;
      return Ok();
    }
    if (type == MSG_CHANNEL_OPEN_FAILURE) {
      Cursor c{ByteSpan(p->data(), p->size())};
      c.u8(); c.u32();
      uint32_t code = c.u32();
      std::string why = c.text();
      return Result<void>(Status::Denied, "the server refused a " + type_name + " channel (" + std::to_string(code) + "): " + why);
    }
    if (type == MSG_GLOBAL_REQUEST) {
      Cursor c{ByteSpan(p->data(), p->size())};
      c.u8(); c.text();
      if (c.boolean()) {
        Buf b;
        b.u8(MSG_REQUEST_FAILURE);
        if (auto r = write_packet(ByteSpan(b.b.data(), b.b.size())); !r.ok()) return r;
      }
      continue;
    }
    if (type == MSG_KEXINIT) {
      ByteBuffer sk = std::move(*p);
      if (auto r = do_kex(nullptr, &sk, timeout_ms); !r.ok()) return r;
      continue;
    }
    if (type == MSG_DEBUG || type == MSG_IGNORE) continue;
    return Result<void>(Status::Corrupt, "expected a channel confirmation");
  }
}

namespace {

// A direct-tcpip channel as a byte stream: what another session (the next ProxyJump hop) runs over.
class ChannelStream final : public hal::Connection {
 public:
  ChannelStream(SshSession& s, std::string peer) : s_(s), peer_(std::move(peer)) {}
  bool send_all(ByteSpan data) override { return !closed_ && s_.send(data).ok(); }
  bool recv_all(MutableByteSpan data, int timeout_ms) override {
    while (buf_.size() < data.size()) {
      if (!fill(timeout_ms)) return false;
    }
    std::memcpy(data.data(), buf_.data(), data.size());
    buf_.erase(buf_.begin(), buf_.begin() + (long)data.size());
    return true;
  }
  long recv_some(MutableByteSpan data, int timeout_ms) override {
    if (data.empty()) return 0;
    if (buf_.empty() && !fill(timeout_ms)) return alive() ? 0 : -1;
    size_t n = std::min(data.size(), buf_.size());
    std::memcpy(data.data(), buf_.data(), n);
    buf_.erase(buf_.begin(), buf_.begin() + (long)n);
    return (long)n;
  }
  std::string peer() const override { return peer_; }
  void close() override {
    if (closed_) return;
    closed_ = true;
    (void)s_.send_eof();
  }
  bool alive() const override { return !closed_ && (s_.channel_alive() || !buf_.empty() || s_.channel_buffered()); }
  size_t pending() const override { return buf_.size() + s_.channel_buffered(); }

 private:
  bool fill(int timeout_ms) {
    if (closed_) return false;
    auto got = s_.recv(timeout_ms);
    if (!got.ok() || got->empty()) return false;
    buf_.insert(buf_.end(), got->begin(), got->end());
    return true;
  }
  SshSession& s_;
  std::string peer_;
  ByteBuffer buf_;
  bool closed_ = false;
};

}  // namespace

Result<std::unique_ptr<hal::Connection>> SshSession::open_direct_tcpip(const std::string& host, uint16_t port) {
  Buf extra;
  extra.str(host);
  extra.u32(port);
  extra.str(std::string("127.0.0.1"));   // originator: what OpenSSH's ssh -W reports
  extra.u32(0);
  if (auto r = open_channel("direct-tcpip", extra.span(), 20000); !r.ok())
    return Result<std::unique_ptr<hal::Connection>>(r.error());
  return std::unique_ptr<hal::Connection>(new ChannelStream(*this, host + ":" + std::to_string(port) + " via " + server_ident_));
}

Result<void> SshSession::exec(const std::string& command, const std::vector<std::pair<std::string, std::string>>& env) {
  if (auto r = open_channel("session", ByteSpan(), 20000); !r.ok()) return r;

  for (const auto& [name, value] : env) {
    Buf e;
    e.u8(MSG_CHANNEL_REQUEST);
    e.u32(remote_channel_);
    e.str(std::string("env"));
    e.boolean(false);
    e.str(name);
    e.str(value);
    if (auto r = write_packet(ByteSpan(e.b.data(), e.b.size())); !r.ok()) return r;
  }

  Buf req;
  req.u8(MSG_CHANNEL_REQUEST);
  req.u32(remote_channel_);
  req.str(std::string("exec"));
  req.boolean(true);
  req.str(command);
  if (auto r = write_packet(ByteSpan(req.b.data(), req.b.size())); !r.ok()) return r;

  for (;;) {
    auto p = read_packet(20000);
    if (!p.ok()) return Result<void>(p.error());
    if (p->empty()) continue;
    uint8_t type = (*p)[0];
    if (type == MSG_CHANNEL_SUCCESS) return Ok();
    if (type == MSG_CHANNEL_FAILURE)
      return Result<void>(Status::Denied, "the server refused to run: " + command);
    if (type == MSG_KEXINIT) {
      ByteBuffer sk = std::move(*p);
      if (auto r = do_kex(nullptr, &sk, 20000); !r.ok()) return r;
      continue;
    }
    // Anything else that arrives first (data, window adjust) is handled the
    // normal way and we keep waiting for the reply.
    if (type == MSG_CHANNEL_DATA || type == MSG_CHANNEL_EXTENDED_DATA || type == MSG_CHANNEL_WINDOW_ADJUST ||
        type == MSG_CHANNEL_EOF || type == MSG_CHANNEL_CLOSE || type == MSG_DEBUG || type == MSG_IGNORE ||
        type == MSG_GLOBAL_REQUEST || type == MSG_CHANNEL_REQUEST) {
      // fall through to the dispatcher below
      ByteBuffer copy = *p;
      Cursor c{ByteSpan(copy.data(), copy.size())};
      uint8_t t2 = c.u8();
      if (t2 == MSG_CHANNEL_DATA) {
        c.u32();
        auto d = c.str();
        rx_data_.insert(rx_data_.end(), d.begin(), d.end());
      } else if (t2 == MSG_CHANNEL_EXTENDED_DATA) {
        c.u32(); c.u32();
        stderr_ += c.text();
      } else if (t2 == MSG_CHANNEL_WINDOW_ADJUST) {
        c.u32();
        remote_window_ += c.u32();
      } else if (t2 == MSG_CHANNEL_EOF) {
        channel_eof_ = true;
      } else if (t2 == MSG_CHANNEL_CLOSE) {
        channel_closed_ = true;
        return Ok();
      }
      continue;
    }
    return Result<void>(Status::Corrupt, "unexpected reply to the exec request");
  }
}

Result<void> SshSession::pump(int timeout_ms) {
  auto p = read_packet(timeout_ms);
  if (!p.ok()) return Result<void>(p.error());
  if (p->empty()) return Ok();
  Cursor c{ByteSpan(p->data(), p->size())};
  uint8_t type = c.u8();
  switch (type) {
    case MSG_CHANNEL_DATA: {
      c.u32();
      auto d = c.str();
      rx_data_.insert(rx_data_.end(), d.begin(), d.end());
      if (d.size() < local_window_) local_window_ -= (uint32_t)d.size(); else local_window_ = 0;
      if (local_window_ < (1u << 19)) {
        uint32_t add = (1u << 21) - local_window_;
        Buf b;
        b.u8(MSG_CHANNEL_WINDOW_ADJUST);
        b.u32(remote_channel_);
        b.u32(add);
        if (auto r = write_packet(ByteSpan(b.b.data(), b.b.size())); !r.ok()) return r;
        local_window_ += add;
      }
      break;
    }
    case MSG_CHANNEL_EXTENDED_DATA: {
      c.u32(); c.u32();
      stderr_ += c.text();
      break;
    }
    case MSG_CHANNEL_WINDOW_ADJUST: {
      c.u32();
      remote_window_ += c.u32();
      break;
    }
    case MSG_CHANNEL_EOF:
      channel_eof_ = true;
      break;
    case MSG_CHANNEL_CLOSE:
      channel_closed_ = true;
      channel_eof_ = true;
      break;
    case MSG_CHANNEL_REQUEST: {
      c.u32();
      std::string what = c.text();
      bool want_reply = c.boolean();
      if (what == "exit-status") exit_status_ = (int)c.u32();
      if (want_reply) {
        Buf b;
        b.u8(MSG_CHANNEL_FAILURE);
        b.u32(remote_channel_);
        write_packet(ByteSpan(b.b.data(), b.b.size()));
      }
      break;
    }
    case MSG_GLOBAL_REQUEST: {
      c.text();
      if (c.boolean()) {
        Buf b;
        b.u8(MSG_REQUEST_FAILURE);
        if (auto r = write_packet(ByteSpan(b.b.data(), b.b.size())); !r.ok()) return r;
      }
      break;
    }
    case MSG_DISCONNECT: {
      c.u32();
      std::string why = c.text();
      channel_closed_ = channel_eof_ = true;
      return Result<void>(Status::IoError, "the server disconnected: " + why);
    }
    case MSG_KEXINIT: {
      ByteBuffer sk = std::move(*p);
      if (auto r = do_kex(nullptr, &sk, timeout_ms); !r.ok()) return r;
      break;
    }
    default:
      break;   // IGNORE, DEBUG, anything we do not act on
  }
  return Ok();
}

Result<void> SshSession::send(ByteSpan data) {
  if (!channel_open_) return Result<void>(Status::Invalid, "no channel is open");
  if (encrypted_ && (bytes_in_ + bytes_out_ - rekey_bytes_ > (1ULL << 30))) {
    if (auto r = rekey(20000); !r.ok()) return r;
  }
  size_t off = 0;
  while (off < data.size()) {
    while (remote_window_ == 0) {
      if (channel_closed_) return Result<void>(Status::IoError, "the remote closed the channel");
      if (auto r = pump(20000); !r.ok()) return r;
    }
    size_t n = data.size() - off;
    if (n > remote_max_packet_ - 64) n = remote_max_packet_ - 64;
    if (n > remote_window_) n = remote_window_;
    Buf b;
    b.u8(MSG_CHANNEL_DATA);
    b.u32(remote_channel_);
    b.str(data.subspan(off, n));
    if (auto r = write_packet(ByteSpan(b.b.data(), b.b.size())); !r.ok()) return r;
    remote_window_ -= (uint32_t)n;
    off += n;
  }
  return Ok();
}

Result<void> SshSession::send_eof() {
  if (!channel_open_) return Ok();
  Buf b;
  b.u8(MSG_CHANNEL_EOF);
  b.u32(remote_channel_);
  return write_packet(ByteSpan(b.b.data(), b.b.size()));
}

Result<void> SshSession::wait_close(int timeout_ms) {
  while (!channel_closed_) {
    if (auto r = pump(timeout_ms); !r.ok()) return r;
  }
  return Ok();
}

Result<ByteBuffer> SshSession::recv(int timeout_ms) {
  if (!rx_data_.empty()) {
    ByteBuffer out;
    out.swap(rx_data_);
    return out;
  }
  if (channel_eof_ || channel_closed_) return ByteBuffer{};
  // A timeout is fatal rather than "nothing yet": the transport cannot report a
  // partial read, so retrying after one would resume in the middle of a packet.
  auto r = pump(timeout_ms);
  if (!r.ok()) return Result<ByteBuffer>(r.error());
  ByteBuffer out;
  out.swap(rx_data_);
  return out;
}

}  // namespace aios::net
