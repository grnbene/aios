#include "aios/crypto/sha512.hpp"

#include <cstring>

namespace aios::crypto {
namespace {

const uint64_t K[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};

inline uint64_t ror(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

}  // namespace

void Sha512::reset() {
  if (sha384_) {
    h_[0] = 0xcbbb9d5dc1059ed8ULL; h_[1] = 0x629a292a367cd507ULL;
    h_[2] = 0x9159015a3070dd17ULL; h_[3] = 0x152fecd8f70e5939ULL;
    h_[4] = 0x67332667ffc00b31ULL; h_[5] = 0x8eb44a8768581511ULL;
    h_[6] = 0xdb0c2e0d64f98fa7ULL; h_[7] = 0x47b5481dbefa4fa4ULL;
  } else {
  h_[0] = 0x6a09e667f3bcc908ULL; h_[1] = 0xbb67ae8584caa73bULL;
  h_[2] = 0x3c6ef372fe94f82bULL; h_[3] = 0xa54ff53a5f1d36f1ULL;
  h_[4] = 0x510e527fade682d1ULL; h_[5] = 0x9b05688c2b3e6c1fULL;
  h_[6] = 0x1f83d9abfb41bd6bULL; h_[7] = 0x5be0cd19137e2179ULL;
  }
  buffered_ = 0;
  total_ = 0;
}

void Sha512::block(const uint8_t* p) {
  uint64_t w[80];
  for (int i = 0; i < 16; ++i) {
    w[i] = 0;
    for (int b = 0; b < 8; ++b) w[i] = (w[i] << 8) | p[i * 8 + b];
  }
  for (int i = 16; i < 80; ++i) {
    uint64_t s0 = ror(w[i - 15], 1) ^ ror(w[i - 15], 8) ^ (w[i - 15] >> 7);
    uint64_t s1 = ror(w[i - 2], 19) ^ ror(w[i - 2], 61) ^ (w[i - 2] >> 6);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint64_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], hh = h_[7];
  for (int i = 0; i < 80; ++i) {
    uint64_t S1 = ror(e, 14) ^ ror(e, 18) ^ ror(e, 41);
    uint64_t ch = (e & f) ^ (~e & g);
    uint64_t t1 = hh + S1 + ch + K[i] + w[i];
    uint64_t S0 = ror(a, 28) ^ ror(a, 34) ^ ror(a, 39);
    uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint64_t t2 = S0 + maj;
    hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
  h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += hh;
}

void Sha512::update(ByteSpan data) {
  size_t i = 0;
  total_ += data.size();
  if (buffered_) {
    while (i < data.size() && buffered_ < 128) buf_[buffered_++] = data[i++];
    if (buffered_ < 128) return;
    block(buf_);
    buffered_ = 0;
  }
  while (i + 128 <= data.size()) { block(data.data() + i); i += 128; }
  while (i < data.size()) buf_[buffered_++] = data[i++];
}

std::array<uint8_t, 64> Sha512::finish() {
  uint64_t bits = total_ * 8;
  uint8_t pad[256];
  std::memset(pad, 0, sizeof(pad));
  size_t padlen = (buffered_ < 112) ? (112 - buffered_) : (240 - buffered_);
  pad[0] = 0x80;
  // 128-bit length; this machine never hashes 2^64 bits, so the high half is zero.
  for (int i = 0; i < 8; ++i) pad[padlen + 8 + i] = (uint8_t)(bits >> (56 - 8 * i));
  uint64_t keep = total_;
  update(ByteSpan(pad, padlen + 16));
  total_ = keep;
  std::array<uint8_t, 64> out{};
  for (int i = 0; i < 8; ++i)
    for (int b = 0; b < 8; ++b) out[(size_t)i * 8 + (size_t)b] = (uint8_t)(h_[i] >> (56 - 8 * b));
  return out;
}

std::array<uint8_t, 64> sha512(ByteSpan data) {
  Sha512 s;
  s.update(data);
  return s.finish();
}

std::array<uint8_t, 48> sha384(ByteSpan data) {
  Sha512 s(true);
  s.update(data);
  auto full = s.finish();
  std::array<uint8_t, 48> out{};
  for (size_t i = 0; i < 48; ++i) out[i] = full[i];
  return out;
}

}  // namespace aios::crypto
