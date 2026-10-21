#include "aios/crypto/aes_gcm.hpp"

#include <cstring>

namespace aios::crypto {
namespace {

constexpr uint8_t gf_mul(uint8_t a, uint8_t b) {
  uint8_t p = 0;
  for (int i = 0; i < 8; ++i) {
    if (b & 1) p ^= a;
    uint8_t hi = a & 0x80;
    a = uint8_t(a << 1);
    if (hi) a ^= 0x1b;
    b >>= 1;
  }
  return p;
}

constexpr uint8_t rotl8(uint8_t x, int n) { return uint8_t((x << n) | (x >> (8 - n))); }

// FIPS-197 S-box: multiplicative inverse in GF(2^8) followed by the affine map.
constexpr std::array<uint8_t, 256> make_sbox() {
  std::array<uint8_t, 256> s{};
  for (int x = 0; x < 256; ++x) {
    uint8_t inv = 0;
    if (x) {
      uint8_t r = 1, base = uint8_t(x);
      for (int e = 254; e; e >>= 1) {   // x^254 == x^-1
        if (e & 1) r = gf_mul(r, base);
        base = gf_mul(base, base);
      }
      inv = r;
    }
    s[size_t(x)] = uint8_t(inv ^ rotl8(inv, 1) ^ rotl8(inv, 2) ^ rotl8(inv, 3) ^ rotl8(inv, 4) ^ 0x63);
  }
  return s;
}

constexpr std::array<uint8_t, 256> SBOX = make_sbox();
static_assert(SBOX[0x00] == 0x63 && SBOX[0x01] == 0x7c && SBOX[0x53] == 0xed && SBOX[0xff] == 0x16);

inline uint8_t xtime(uint8_t x) { return uint8_t((x << 1) ^ ((x >> 7) * 0x1b)); }

constexpr std::array<uint8_t, 256> make_inv_sbox() {
  std::array<uint8_t, 256> inv{};
  for (int i = 0; i < 256; ++i) inv[SBOX[(size_t)i]] = (uint8_t)i;
  return inv;
}
constexpr std::array<uint8_t, 256> INV_SBOX = make_inv_sbox();
static_assert(INV_SBOX[0x63] == 0x00 && INV_SBOX[0x16] == 0xff);

// AES with an Nk-word key: Nk=4 is AES-128 (10 rounds), Nk=8 is AES-256 (14).
template <int Nk>
struct Aes {
  static constexpr int kRounds = Nk + 6;
  uint8_t rk[16 * (kRounds + 1)];

  explicit Aes(const std::array<uint8_t, 4 * Nk>& key) {
    std::memcpy(rk, key.data(), 4 * Nk);
    uint8_t rcon = 1;
    for (int w = Nk; w < 4 * (kRounds + 1); ++w) {   // FIPS-197 5.2, one word at a time
      uint8_t t[4] = {rk[4 * w - 4], rk[4 * w - 3], rk[4 * w - 2], rk[4 * w - 1]};
      if (w % Nk == 0) {
        uint8_t first = t[0];
        t[0] = uint8_t(SBOX[t[1]] ^ rcon);
        t[1] = SBOX[t[2]];
        t[2] = SBOX[t[3]];
        t[3] = SBOX[first];
        rcon = xtime(rcon);
      } else if (Nk > 6 && w % Nk == 4) {
        for (auto& x : t) x = SBOX[x];
      }
      for (int j = 0; j < 4; ++j) rk[4 * w + j] = uint8_t(rk[4 * (w - Nk) + j] ^ t[j]);
    }
  }

  void encrypt(const uint8_t in[16], uint8_t out[16]) const {
    uint8_t s[16];
    for (int i = 0; i < 16; ++i) s[i] = uint8_t(in[i] ^ rk[i]);
    for (int round = 1; round <= kRounds; ++round) {
      uint8_t t[16];
      for (int c = 0; c < 4; ++c)          // SubBytes + ShiftRows (state is column-major)
        for (int r = 0; r < 4; ++r) t[r + 4 * c] = SBOX[s[r + 4 * ((c + r) & 3)]];
      if (round != kRounds) {
        for (int c = 0; c < 4; ++c) {      // MixColumns
          uint8_t a0 = t[4 * c], a1 = t[4 * c + 1], a2 = t[4 * c + 2], a3 = t[4 * c + 3];
          uint8_t all = uint8_t(a0 ^ a1 ^ a2 ^ a3);
          t[4 * c] = uint8_t(a0 ^ all ^ xtime(uint8_t(a0 ^ a1)));
          t[4 * c + 1] = uint8_t(a1 ^ all ^ xtime(uint8_t(a1 ^ a2)));
          t[4 * c + 2] = uint8_t(a2 ^ all ^ xtime(uint8_t(a2 ^ a3)));
          t[4 * c + 3] = uint8_t(a3 ^ all ^ xtime(uint8_t(a3 ^ a0)));
        }
      }
      for (int i = 0; i < 16; ++i) s[i] = uint8_t(t[i] ^ rk[16 * round + i]);
    }
    std::memcpy(out, s, 16);
  }

  // The inverse cipher (FIPS-197 5.3), for CBC: the same round keys, applied last to first.
  void decrypt(const uint8_t in[16], uint8_t out[16]) const {
    uint8_t s[16];
    for (int i = 0; i < 16; ++i) s[i] = uint8_t(in[i] ^ rk[16 * kRounds + i]);
    for (int round = kRounds - 1; round >= 0; --round) {
      uint8_t t[16];
      for (int c = 0; c < 4; ++c)          // InvShiftRows + InvSubBytes
        for (int r = 0; r < 4; ++r) t[r + 4 * ((c + r) & 3)] = INV_SBOX[s[r + 4 * c]];
      for (int i = 0; i < 16; ++i) t[i] = uint8_t(t[i] ^ rk[16 * round + i]);
      if (round != 0) {
        for (int c = 0; c < 4; ++c) {      // InvMixColumns
          uint8_t a0 = t[4 * c], a1 = t[4 * c + 1], a2 = t[4 * c + 2], a3 = t[4 * c + 3];
          t[4 * c] = uint8_t(gf_mul(a0, 14) ^ gf_mul(a1, 11) ^ gf_mul(a2, 13) ^ gf_mul(a3, 9));
          t[4 * c + 1] = uint8_t(gf_mul(a0, 9) ^ gf_mul(a1, 14) ^ gf_mul(a2, 11) ^ gf_mul(a3, 13));
          t[4 * c + 2] = uint8_t(gf_mul(a0, 13) ^ gf_mul(a1, 9) ^ gf_mul(a2, 14) ^ gf_mul(a3, 11));
          t[4 * c + 3] = uint8_t(gf_mul(a0, 11) ^ gf_mul(a1, 13) ^ gf_mul(a2, 9) ^ gf_mul(a3, 14));
        }
      }
      std::memcpy(s, t, 16);
    }
    std::memcpy(out, s, 16);
  }
};
using Aes128 = Aes<4>;
using Aes256 = Aes<8>;

struct Block128 { uint64_t hi = 0, lo = 0; };

Block128 load(const uint8_t b[16]) {
  Block128 v;
  for (int i = 0; i < 8; ++i) v.hi = (v.hi << 8) | b[i];
  for (int i = 8; i < 16; ++i) v.lo = (v.lo << 8) | b[i];
  return v;
}

void store(const Block128& v, uint8_t b[16]) {
  for (int i = 0; i < 8; ++i) b[i] = uint8_t(v.hi >> (56 - 8 * i));
  for (int i = 0; i < 8; ++i) b[8 + i] = uint8_t(v.lo >> (56 - 8 * i));
}

// GF(2^128) multiply in GCM's bit-reflected convention; branch-free on secret bits.
Block128 gf128_mul(const Block128& x, const Block128& y) {
  Block128 z, v = y;
  for (int i = 0; i < 128; ++i) {
    uint64_t bit = i < 64 ? (x.hi >> (63 - i)) & 1 : (x.lo >> (127 - i)) & 1;
    uint64_t take = 0 - bit;
    z.hi ^= v.hi & take;
    z.lo ^= v.lo & take;
    uint64_t reduce = 0 - (v.lo & 1);
    v.lo = (v.lo >> 1) | (v.hi << 63);
    v.hi = (v.hi >> 1) ^ (0xe100000000000000ULL & reduce);
  }
  return z;
}

struct Ghash {
  Block128 h, acc;
  explicit Ghash(const Block128& key) : h(key) {}
  void block(const uint8_t b[16]) {
    Block128 x = load(b);
    acc.hi ^= x.hi;
    acc.lo ^= x.lo;
    acc = gf128_mul(acc, h);
  }
  void padded(ByteSpan data) {
    size_t full = data.size() / 16 * 16;
    for (size_t i = 0; i < full; i += 16) block(data.data() + i);
    if (full < data.size()) {
      uint8_t last[16] = {0};
      std::memcpy(last, data.data() + full, data.size() - full);
      block(last);
    }
  }
};

void inc32(uint8_t ctr[16]) {
  for (int i = 15; i >= 12; --i)
    if (++ctr[i]) break;
}

template <class Cipher>
void ctr_xor(const Cipher& aes, const uint8_t j0[16], const uint8_t* in, uint8_t* out, size_t n) {
  uint8_t ctr[16], ks[16];
  std::memcpy(ctr, j0, 16);
  for (size_t off = 0; off < n; off += 16) {
    inc32(ctr);
    aes.encrypt(ctr, ks);
    size_t m = n - off < 16 ? n - off : 16;
    for (size_t i = 0; i < m; ++i) out[off + i] = uint8_t(in[off + i] ^ ks[i]);
  }
}

template <class Cipher>
void compute_tag(const Cipher& aes, const uint8_t j0[16], ByteSpan aad, ByteSpan ct, uint8_t tag[16]) {
  uint8_t zero[16] = {0}, hbytes[16];
  aes.encrypt(zero, hbytes);
  Ghash g(load(hbytes));
  g.padded(aad);
  g.padded(ct);
  Block128 lens{uint64_t(aad.size()) * 8, uint64_t(ct.size()) * 8};
  uint8_t lb[16];
  store(lens, lb);
  g.block(lb);
  uint8_t s[16], ek[16];
  store(g.acc, s);
  aes.encrypt(j0, ek);
  for (int i = 0; i < 16; ++i) tag[i] = uint8_t(s[i] ^ ek[i]);
}

void make_j0(const GcmNonce& nonce, uint8_t j0[16]) {
  std::memcpy(j0, nonce.data(), 12);
  j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
}

}  // namespace

namespace {

template <class Cipher>
ByteBuffer gcm_seal(const Cipher& aes, const GcmNonce& nonce, ByteSpan aad, ByteSpan plaintext) {
  uint8_t j0[16];
  make_j0(nonce, j0);
  ByteBuffer out(plaintext.size() + 16);
  ctr_xor(aes, j0, plaintext.data(), out.data(), plaintext.size());
  compute_tag(aes, j0, aad, ByteSpan(out.data(), plaintext.size()), out.data() + plaintext.size());
  return out;
}

template <class Cipher>
bool gcm_open(const Cipher& aes, const GcmNonce& nonce, ByteSpan aad, ByteSpan sealed, ByteBuffer& out) {
  if (sealed.size() < 16) return false;
  uint8_t j0[16];
  make_j0(nonce, j0);
  ByteSpan ct = sealed.subspan(0, sealed.size() - 16);
  uint8_t want[16];
  compute_tag(aes, j0, aad, ct, want);
  unsigned diff = 0;
  for (int i = 0; i < 16; ++i) diff |= unsigned(want[i] ^ sealed[ct.size() + size_t(i)]);
  if (diff) return false;
  out.resize(ct.size());
  ctr_xor(aes, j0, ct.data(), out.data(), ct.size());
  return true;
}

}  // namespace

ByteBuffer aes128_gcm_seal(const Aes128Key& key, const GcmNonce& nonce, ByteSpan aad, ByteSpan plaintext) {
  return gcm_seal(Aes128(key), nonce, aad, plaintext);
}

bool aes128_gcm_open(const Aes128Key& key, const GcmNonce& nonce, ByteSpan aad, ByteSpan sealed, ByteBuffer& out) {
  return gcm_open(Aes128(key), nonce, aad, sealed, out);
}

ByteBuffer aes256_gcm_seal(const Aes256Key& key, const GcmNonce& nonce, ByteSpan aad, ByteSpan plaintext) {
  return gcm_seal(Aes256(key), nonce, aad, plaintext);
}

bool aes256_gcm_open(const Aes256Key& key, const GcmNonce& nonce, ByteSpan aad, ByteSpan sealed, ByteBuffer& out) {
  return gcm_open(Aes256(key), nonce, aad, sealed, out);
}

void aes_ctr_crypt(ByteSpan key, ByteSpan iv, ByteSpan in, MutableByteSpan out) {
  uint8_t ctr[16];
  std::memset(ctr, 0, 16);
  size_t iv_take = iv.size() < 16 ? iv.size() : 16;
  std::memcpy(ctr, iv.data(), iv_take);
  auto run_ctr = [&](const auto& aes) {
    uint8_t ks[16];
    size_t n = in.size();
    for (size_t off = 0; off < n; off += 16) {
      aes.encrypt(ctr, ks);
      size_t m = (n - off < 16) ? (n - off) : 16;
      for (size_t i = 0; i < m; ++i) {
        out[off + i] = uint8_t(in[off + i] ^ ks[i]);
      }
      for (int i = 15; i >= 0; --i) {
        if (++ctr[i] != 0) break;
      }
    }
  };
  if (key.size() == 16) {
    std::array<uint8_t, 16> k{};
    std::memcpy(k.data(), key.data(), 16);
    run_ctr(Aes128(k));
  } else if (key.size() == 24) {
    std::array<uint8_t, 24> k{};
    std::memcpy(k.data(), key.data(), 24);
    run_ctr(Aes<6>(k));
  } else if (key.size() == 32) {
    std::array<uint8_t, 32> k{};
    std::memcpy(k.data(), key.data(), 32);
    run_ctr(Aes256(k));
  }
}

namespace {
template <typename F>
bool with_aes(ByteSpan key, F&& f) {
  if (key.size() == 16) { std::array<uint8_t, 16> k{}; std::memcpy(k.data(), key.data(), 16); f(Aes128(k)); return true; }
  if (key.size() == 24) { std::array<uint8_t, 24> k{}; std::memcpy(k.data(), key.data(), 24); f(Aes<6>(k)); return true; }
  if (key.size() == 32) { std::array<uint8_t, 32> k{}; std::memcpy(k.data(), key.data(), 32); f(Aes256(k)); return true; }
  return false;
}
}  // namespace

bool aes_cbc_decrypt(ByteSpan key, ByteSpan iv, ByteSpan in, MutableByteSpan out) {
  if (iv.size() != 16 || in.size() % 16 != 0 || out.size() < in.size()) return false;
  return with_aes(key, [&](const auto& aes) {
    uint8_t prev[16], cur[16], plain[16];
    std::memcpy(prev, iv.data(), 16);
    for (size_t off = 0; off < in.size(); off += 16) {
      std::memcpy(cur, in.data() + off, 16);
      aes.decrypt(cur, plain);
      for (int i = 0; i < 16; ++i) out[off + (size_t)i] = uint8_t(plain[i] ^ prev[i]);
      std::memcpy(prev, cur, 16);
    }
  });
}

bool aes_cbc_encrypt(ByteSpan key, ByteSpan iv, ByteSpan in, MutableByteSpan out) {
  if (iv.size() != 16 || in.size() % 16 != 0 || out.size() < in.size()) return false;
  return with_aes(key, [&](const auto& aes) {
    uint8_t chain[16], block[16];
    std::memcpy(chain, iv.data(), 16);
    for (size_t off = 0; off < in.size(); off += 16) {
      for (int i = 0; i < 16; ++i) block[i] = uint8_t(in[off + (size_t)i] ^ chain[i]);
      aes.encrypt(block, chain);
      std::memcpy(out.data() + off, chain, 16);
    }
  });
}

}  // namespace aios::crypto
