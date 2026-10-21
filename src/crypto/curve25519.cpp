#include "aios/crypto/curve25519.hpp"

#include <cstring>

#include "aios/crypto/sha512.hpp"

#if defined(_MSC_VER) && !defined(__clang__)
#include "aios/crypto/portable_uint128.hpp"
#define AIOS_U128 aios::crypto::portable::U128
#else
#define AIOS_U128 unsigned __int128
#endif

namespace aios::crypto {
namespace {

// ---------------------------------------------------------------------------
// GF(2^255 - 19), five 51-bit limbs
// ---------------------------------------------------------------------------
using fe = uint64_t[5];
constexpr uint64_t kMask51 = 0x7ffffffffffffULL;

void fe_copy(fe h, const fe f) { for (int i = 0; i < 5; ++i) h[i] = f[i]; }
void fe_zero(fe h) { for (int i = 0; i < 5; ++i) h[i] = 0; }
void fe_one(fe h) { fe_zero(h); h[0] = 1; }

void fe_add(fe h, const fe f, const fe g) { for (int i = 0; i < 5; ++i) h[i] = f[i] + g[i]; }

void fe_sub(fe h, const fe f, const fe g) {
  // f - g + 2p, so every limb stays positive; the caller's next multiply carries.
  h[0] = f[0] + 0xfffffffffffdaULL - g[0];
  h[1] = f[1] + 0xffffffffffffeULL - g[1];
  h[2] = f[2] + 0xffffffffffffeULL - g[2];
  h[3] = f[3] + 0xffffffffffffeULL - g[3];
  h[4] = f[4] + 0xffffffffffffeULL - g[4];
}

void fe_carry(fe h, AIOS_U128 t0, AIOS_U128 t1, AIOS_U128 t2,
              AIOS_U128 t3, AIOS_U128 t4) {
  uint64_t r0, r1, r2, r3, r4, c;
  t1 += (uint64_t)(t0 >> 51); r0 = (uint64_t)t0 & kMask51;
  t2 += (uint64_t)(t1 >> 51); r1 = (uint64_t)t1 & kMask51;
  t3 += (uint64_t)(t2 >> 51); r2 = (uint64_t)t2 & kMask51;
  t4 += (uint64_t)(t3 >> 51); r3 = (uint64_t)t3 & kMask51;
  c = (uint64_t)(t4 >> 51);   r4 = (uint64_t)t4 & kMask51;
  r0 += c * 19;
  c = r0 >> 51; r0 &= kMask51;
  r1 += c;
  c = r1 >> 51; r1 &= kMask51;
  r2 += c;
  h[0] = r0; h[1] = r1; h[2] = r2; h[3] = r3; h[4] = r4;
}

void fe_mul(fe h, const fe f, const fe g) {
  using u128 = AIOS_U128;
  const u128 f0 = f[0], f1 = f[1], f2 = f[2], f3 = f[3], f4 = f[4];
  const uint64_t g0 = g[0], g1 = g[1], g2 = g[2], g3 = g[3], g4 = g[4];
  const u128 f1_19 = 19 * f1, f2_19 = 19 * f2, f3_19 = 19 * f3, f4_19 = 19 * f4;
  u128 t0 = f0 * g0 + f1_19 * g4 + f2_19 * g3 + f3_19 * g2 + f4_19 * g1;
  u128 t1 = f0 * g1 + f1 * g0 + f2_19 * g4 + f3_19 * g3 + f4_19 * g2;
  u128 t2 = f0 * g2 + f1 * g1 + f2 * g0 + f3_19 * g4 + f4_19 * g3;
  u128 t3 = f0 * g3 + f1 * g2 + f2 * g1 + f3 * g0 + f4_19 * g4;
  u128 t4 = f0 * g4 + f1 * g3 + f2 * g2 + f3 * g1 + f4 * g0;
  fe_carry(h, t0, t1, t2, t3, t4);
}

void fe_sq(fe h, const fe f) { fe_mul(h, f, f); }

void fe_mul_small(fe h, const fe f, uint64_t n) {
  using u128 = AIOS_U128;
  fe_carry(h, (u128)f[0] * n, (u128)f[1] * n, (u128)f[2] * n, (u128)f[3] * n, (u128)f[4] * n);
}

void fe_neg(fe h, const fe f) { fe z; fe_zero(z); fe_sub(h, z, f); }

void fe_cmov(fe h, const fe g, uint64_t bit) {
  uint64_t mask = 0 - (bit & 1);
  for (int i = 0; i < 5; ++i) h[i] ^= mask & (h[i] ^ g[i]);
}

void fe_cswap(fe a, fe b, uint64_t bit) {
  uint64_t mask = 0 - (bit & 1);
  for (int i = 0; i < 5; ++i) {
    uint64_t t = mask & (a[i] ^ b[i]);
    a[i] ^= t;
    b[i] ^= t;
  }
}

void fe_frombytes(fe h, const uint8_t* s) {
  auto ld = [&](int off) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | s[off + i];
    return v;
  };
  h[0] = ld(0) & kMask51;
  h[1] = (ld(6) >> 3) & kMask51;
  h[2] = (ld(12) >> 6) & kMask51;
  h[3] = (ld(19) >> 1) & kMask51;
  h[4] = (ld(24) >> 12) & kMask51;
}

void fe_tobytes(uint8_t* s, const fe f) {
  uint64_t h0 = f[0], h1 = f[1], h2 = f[2], h3 = f[3], h4 = f[4], c;
  c = h1 >> 51; h1 &= kMask51; h2 += c;
  c = h2 >> 51; h2 &= kMask51; h3 += c;
  c = h3 >> 51; h3 &= kMask51; h4 += c;
  c = h4 >> 51; h4 &= kMask51; h0 += c * 19;
  c = h0 >> 51; h0 &= kMask51; h1 += c;

  // Fully reduce: add 19, and if that overflowed past 2^255 the value was >= p.
  uint64_t q = (h0 + 19) >> 51;
  q = (h1 + q) >> 51;
  q = (h2 + q) >> 51;
  q = (h3 + q) >> 51;
  q = (h4 + q) >> 51;
  h0 += 19 * q;
  c = h0 >> 51; h0 &= kMask51; h1 += c;
  c = h1 >> 51; h1 &= kMask51; h2 += c;
  c = h2 >> 51; h2 &= kMask51; h3 += c;
  c = h3 >> 51; h3 &= kMask51; h4 += c;
  h4 &= kMask51;

  uint64_t w[4];
  w[0] = h0 | (h1 << 51);
  w[1] = (h1 >> 13) | (h2 << 38);
  w[2] = (h2 >> 26) | (h3 << 25);
  w[3] = (h3 >> 39) | (h4 << 12);
  for (int i = 0; i < 4; ++i)
    for (int b = 0; b < 8; ++b) s[i * 8 + b] = (uint8_t)(w[i] >> (8 * b));
}

bool fe_is_zero(const fe f) {
  uint8_t s[32];
  fe_tobytes(s, f);
  uint8_t acc = 0;
  for (int i = 0; i < 32; ++i) acc |= s[i];
  return acc == 0;
}

bool fe_is_negative(const fe f) {
  uint8_t s[32];
  fe_tobytes(s, f);
  return (s[0] & 1) != 0;
}

bool fe_equal(const fe a, const fe b) {
  fe d;
  fe_sub(d, a, b);
  return fe_is_zero(d);
}

// z^(p-2), the ref10 addition chain.
void fe_invert(fe out, const fe z) {
  fe t0, t1, t2, t3;
  fe_sq(t0, z);
  fe_sq(t1, t0); fe_sq(t1, t1);
  fe_mul(t1, z, t1);
  fe_mul(t0, t0, t1);
  fe_sq(t2, t0);
  fe_mul(t1, t1, t2);
  fe_sq(t2, t1);
  for (int i = 1; i < 5; ++i) fe_sq(t2, t2);
  fe_mul(t1, t2, t1);
  fe_sq(t2, t1);
  for (int i = 1; i < 10; ++i) fe_sq(t2, t2);
  fe_mul(t2, t2, t1);
  fe_sq(t3, t2);
  for (int i = 1; i < 20; ++i) fe_sq(t3, t3);
  fe_mul(t2, t3, t2);
  fe_sq(t2, t2);
  for (int i = 1; i < 10; ++i) fe_sq(t2, t2);
  fe_mul(t1, t2, t1);
  fe_sq(t2, t1);
  for (int i = 1; i < 50; ++i) fe_sq(t2, t2);
  fe_mul(t2, t2, t1);
  fe_sq(t3, t2);
  for (int i = 1; i < 100; ++i) fe_sq(t3, t3);
  fe_mul(t2, t3, t2);
  fe_sq(t2, t2);
  for (int i = 1; i < 50; ++i) fe_sq(t2, t2);
  fe_mul(t1, t2, t1);
  fe_sq(t1, t1);
  for (int i = 1; i < 5; ++i) fe_sq(t1, t1);
  fe_mul(out, t1, t0);
}

// z^((p-5)/8), for the square root in point decompression.
void fe_pow22523(fe out, const fe z) {
  fe t0, t1, t2;
  fe_sq(t0, z);
  fe_sq(t1, t0); fe_sq(t1, t1);
  fe_mul(t1, z, t1);
  fe_mul(t0, t0, t1);
  fe_sq(t0, t0);
  fe_mul(t0, t1, t0);
  fe_sq(t1, t0);
  for (int i = 1; i < 5; ++i) fe_sq(t1, t1);
  fe_mul(t0, t1, t0);
  fe_sq(t1, t0);
  for (int i = 1; i < 10; ++i) fe_sq(t1, t1);
  fe_mul(t1, t1, t0);
  fe_sq(t2, t1);
  for (int i = 1; i < 20; ++i) fe_sq(t2, t2);
  fe_mul(t1, t2, t1);
  fe_sq(t1, t1);
  for (int i = 1; i < 10; ++i) fe_sq(t1, t1);
  fe_mul(t0, t1, t0);
  fe_sq(t1, t0);
  for (int i = 1; i < 50; ++i) fe_sq(t1, t1);
  fe_mul(t1, t1, t0);
  fe_sq(t2, t1);
  for (int i = 1; i < 100; ++i) fe_sq(t2, t2);
  fe_mul(t1, t2, t1);
  fe_sq(t1, t1);
  for (int i = 1; i < 50; ++i) fe_sq(t1, t1);
  fe_mul(t0, t1, t0);
  fe_sq(t0, t0); fe_sq(t0, t0);
  fe_mul(out, t0, z);
}

// ---------------------------------------------------------------------------
// The twisted Edwards group, extended coordinates (X:Y:Z:T), a = -1
// ---------------------------------------------------------------------------
struct Ge { fe X, Y, Z, T; };

struct Consts {
  fe d, d2, sqrtm1;
  Ge B;
  bool ready = false;
};

void ge_zero(Ge& p) { fe_zero(p.X); fe_one(p.Y); fe_one(p.Z); fe_zero(p.T); }

void ge_copy(Ge& r, const Ge& p) { fe_copy(r.X, p.X); fe_copy(r.Y, p.Y); fe_copy(r.Z, p.Z); fe_copy(r.T, p.T); }

void ge_cmov(Ge& r, const Ge& p, uint64_t bit) {
  fe_cmov(r.X, p.X, bit); fe_cmov(r.Y, p.Y, bit);
  fe_cmov(r.Z, p.Z, bit); fe_cmov(r.T, p.T, bit);
}

const Consts& consts();

// add-2008-hwcd-3
void ge_add(Ge& r, const Ge& p, const Ge& q) {
  fe a, b, c, d, e, f, g, h, t;
  fe_sub(a, p.Y, p.X);
  fe_sub(t, q.Y, q.X);
  fe_mul(a, a, t);
  fe_add(b, p.Y, p.X);
  fe_add(t, q.Y, q.X);
  fe_mul(b, b, t);
  fe_mul(c, p.T, q.T);
  fe_mul(c, c, consts().d2);
  fe_mul(d, p.Z, q.Z);
  fe_add(d, d, d);
  fe_sub(e, b, a);
  fe_sub(f, d, c);
  fe_add(g, d, c);
  fe_add(h, b, a);
  fe_mul(r.X, e, f);
  fe_mul(r.Y, g, h);
  fe_mul(r.T, e, h);
  fe_mul(r.Z, f, g);
}

// dbl-2008-hwcd
void ge_dbl(Ge& r, const Ge& p) {
  fe a, b, c, d, e, f, g, h, t;
  fe_sq(a, p.X);
  fe_sq(b, p.Y);
  fe_sq(c, p.Z);
  fe_add(c, c, c);
  fe_neg(d, a);
  fe_add(t, p.X, p.Y);
  fe_sq(t, t);
  fe_sub(e, t, a);
  fe_sub(e, e, b);
  fe_add(g, d, b);
  fe_sub(f, g, c);
  fe_sub(h, d, b);
  fe_mul(r.X, e, f);
  fe_mul(r.Y, g, h);
  fe_mul(r.T, e, h);
  fe_mul(r.Z, f, g);
}

void ge_tobytes(uint8_t* s, const Ge& p) {
  fe zi, x, y;
  fe_invert(zi, p.Z);
  fe_mul(x, p.X, zi);
  fe_mul(y, p.Y, zi);
  fe_tobytes(s, y);
  s[31] = (uint8_t)(s[31] ^ (fe_is_negative(x) ? 0x80 : 0x00));
}

bool ge_frombytes(Ge& p, const uint8_t* s) {
  const Consts& k = consts();
  uint8_t y_bytes[32];
  std::memcpy(y_bytes, s, 32);
  uint8_t sign = (uint8_t)(y_bytes[31] >> 7);
  y_bytes[31] &= 0x7f;

  fe y, y2, u, v, v3, vxx, check, x;
  fe_frombytes(y, y_bytes);
  fe_sq(y2, y);
  fe one;
  fe_one(one);
  fe_sub(u, y2, one);              // u = y^2 - 1
  fe_mul(v, y2, k.d);
  fe_add(v, v, one);               // v = d*y^2 + 1

  fe_sq(v3, v);
  fe_mul(v3, v3, v);               // v^3
  fe_sq(x, v3);
  fe_mul(x, x, v);                 // v^7
  fe_mul(x, x, u);                 // u * v^7
  fe_pow22523(x, x);
  fe_mul(x, x, v3);
  fe_mul(x, x, u);                 // x = u * v^3 * (u v^7)^((p-5)/8)

  fe_sq(vxx, x);
  fe_mul(vxx, vxx, v);
  fe_sub(check, vxx, u);
  if (!fe_is_zero(check)) {
    fe_add(check, vxx, u);
    if (!fe_is_zero(check)) return false;      // not on the curve
    fe_mul(x, x, k.sqrtm1);
  }
  if (fe_is_negative(x) != (sign != 0)) fe_neg(x, x);

  fe_copy(p.X, x);
  fe_copy(p.Y, y);
  fe_one(p.Z);
  fe_mul(p.T, x, y);
  return true;
}

const Consts& consts() {
  static Consts k;
  if (!k.ready) {
    // d = -121665 / 121666, derived rather than transcribed.
    fe num, den;
    fe_zero(num); num[0] = 121665;
    fe_neg(num, num);
    fe_zero(den); den[0] = 121666;
    fe_invert(den, den);
    fe_mul(k.d, num, den);
    fe_add(k.d2, k.d, k.d);
    // sqrt(-1) = 2^((p-1)/4): computed as (2)^((p-1)/4) via the pow chain on 2.
    {
      fe two, t;
      fe_zero(two); two[0] = 2;
      // 2^((p-1)/4) == 2^((p-5)/8 * 2 + ...): reuse pow22523 on 2 then square.
      fe_pow22523(t, two);      // 2^((p-5)/8)
      fe_sq(t, t);              // 2^((p-5)/4)
      fe_mul(t, t, two);        // 2^((p-5)/4 + 1) = 2^((p-1)/4)
      fe_copy(k.sqrtm1, t);
    }
    // The base point, decompressed from its standard encoding.
    k.ready = true;   // ge_frombytes needs d and sqrtm1, which are set
    uint8_t b[32];
    b[0] = 0x58;
    for (int i = 1; i < 32; ++i) b[i] = 0x66;
    ge_frombytes(k.B, b);
  }
  return k;
}

// r = [scalar] p, scalar little-endian, 256 bits. Selection is a conditional
// move: the bit chooses a value, not a branch.
void ge_scalarmult(Ge& r, const uint8_t* scalar, const Ge& p) {
  Ge acc, sum;
  ge_zero(acc);
  for (int i = 255; i >= 0; --i) {
    ge_dbl(acc, acc);
    ge_add(sum, acc, p);
    ge_cmov(acc, sum, (uint64_t)((scalar[i >> 3] >> (i & 7)) & 1));
  }
  ge_copy(r, acc);
}

void ge_scalarmult_base(Ge& r, const uint8_t* scalar) { ge_scalarmult(r, scalar, consts().B); }

// ---------------------------------------------------------------------------
// Scalars mod L = 2^252 + 27742317777372353535851937790883648493
// ---------------------------------------------------------------------------
const uint8_t kL[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7,
                        0xa2, 0xde, 0xf9, 0xde, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10};

// Long division by L, bit by bit: the remainder never exceeds 33 bytes and the
// loop count is fixed by the input width, so this is simple to read and to
// check against the reference vectors.
void sc_reduce_bytes(uint8_t out[32], const uint8_t* in, size_t n) {
  uint8_t rem[33];
  std::memset(rem, 0, sizeof(rem));
  auto ge_l = [&]() {
    for (int i = 32; i >= 0; --i) {
      uint8_t l = (i == 32) ? 0 : kL[i];
      if (rem[i] != l) return rem[i] > l;
    }
    return true;   // equal
  };
  auto sub_l = [&]() {
    int borrow = 0;
    for (int i = 0; i < 33; ++i) {
      int l = (i == 32) ? 0 : kL[i];
      int v = (int)rem[i] - l - borrow;
      borrow = v < 0;
      rem[i] = (uint8_t)(v & 0xff);
    }
  };
  for (size_t bit = n * 8; bit-- > 0;) {
    // rem <<= 1
    uint8_t carry = 0;
    for (int i = 0; i < 33; ++i) {
      uint8_t nc = (uint8_t)(rem[i] >> 7);
      rem[i] = (uint8_t)((rem[i] << 1) | carry);
      carry = nc;
    }
    rem[0] |= (uint8_t)((in[bit >> 3] >> (bit & 7)) & 1);
    if (ge_l()) sub_l();
  }
  std::memcpy(out, rem, 32);
}

// out = (a*b + c) mod L, all little-endian 32-byte scalars.
void sc_muladd(uint8_t out[32], const uint8_t a[32], const uint8_t b[32], const uint8_t c[32]) {
  uint32_t prod[64];
  std::memset(prod, 0, sizeof(prod));
  for (int i = 0; i < 32; ++i) {
    uint32_t carry = 0;
    for (int j = 0; j < 32; ++j) {
      uint32_t v = prod[i + j] + (uint32_t)a[i] * b[j] + carry;
      prod[i + j] = v & 0xff;
      carry = v >> 8;
    }
    int k = i + 32;
    while (carry) {
      uint32_t v = prod[k] + carry;
      prod[k] = v & 0xff;
      carry = v >> 8;
      ++k;
    }
  }
  uint8_t wide[65];
  std::memset(wide, 0, sizeof(wide));
  uint32_t carry = 0;
  for (int i = 0; i < 64; ++i) {
    uint32_t v = prod[i] + (i < 32 ? (uint32_t)c[i] : 0u) + carry;
    wide[i] = (uint8_t)(v & 0xff);
    carry = v >> 8;
  }
  wide[64] = (uint8_t)carry;
  sc_reduce_bytes(out, wide, 65);
}

}  // namespace

// ---------------------------------------------------------------------------
// X25519
// ---------------------------------------------------------------------------
bool x25519(Point32& out, const Scalar32& scalar, const Point32& peer) {
  uint8_t e[32];
  std::memcpy(e, scalar.data(), 32);
  e[0] &= 248;
  e[31] &= 127;
  e[31] |= 64;

  uint8_t u_bytes[32];
  std::memcpy(u_bytes, peer.data(), 32);
  u_bytes[31] &= 127;

  fe x1, x2, z2, x3, z3, t0, t1;
  fe_frombytes(x1, u_bytes);
  fe_one(x2); fe_zero(z2);
  fe_copy(x3, x1); fe_one(z3);

  uint64_t swap = 0;
  for (int pos = 254; pos >= 0; --pos) {
    uint64_t b = (uint64_t)((e[pos >> 3] >> (pos & 7)) & 1);
    swap ^= b;
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);
    swap = b;

    fe a, aa, bb, bmin, da, cb, e_;
    fe_add(a, x2, z2);
    fe_sq(aa, a);
    fe_sub(bmin, x2, z2);
    fe_sq(bb, bmin);
    fe_sub(e_, aa, bb);
    fe_add(t0, x3, z3);
    fe_sub(t1, x3, z3);
    fe_mul(da, t1, a);
    fe_mul(cb, t0, bmin);
    fe_add(t0, da, cb);
    fe_sq(x3, t0);
    fe_sub(t1, da, cb);
    fe_sq(t1, t1);
    fe_mul(z3, t1, x1);
    fe_mul(x2, aa, bb);
    fe_mul_small(t0, e_, 121665);
    fe_add(t0, t0, aa);          // z2 = E * (AA + a24*E), RFC 7748 5.2
    fe_mul(z2, e_, t0);
  }
  fe_cswap(x2, x3, swap);
  fe_cswap(z2, z3, swap);

  fe_invert(z2, z2);
  fe_mul(x2, x2, z2);
  fe_tobytes(out.data(), x2);
  uint8_t acc = 0;
  for (int i = 0; i < 32; ++i) acc |= out[(size_t)i];
  return acc != 0;
}

Point32 x25519_base(const Scalar32& scalar) {
  Point32 base{};
  base[0] = 9;
  Point32 out{};
  x25519(out, scalar, base);
  return out;
}

// ---------------------------------------------------------------------------
// Ed25519
// ---------------------------------------------------------------------------
namespace {

void clamp_seed_hash(const Scalar32& seed, uint8_t a[32], uint8_t prefix[32]) {
  auto h = sha512(ByteSpan(seed.data(), seed.size()));
  std::memcpy(a, h.data(), 32);
  a[0] &= 248;
  a[31] &= 63;
  a[31] |= 64;
  std::memcpy(prefix, h.data() + 32, 32);
}

}  // namespace

Point32 ed25519_public_from_seed(const Scalar32& seed) {
  uint8_t a[32], prefix[32];
  clamp_seed_hash(seed, a, prefix);
  Ge A;
  ge_scalarmult_base(A, a);
  Point32 out{};
  ge_tobytes(out.data(), A);
  return out;
}

Sig64 ed25519_sign(ByteSpan message, const Scalar32& seed, const Point32& pub) {
  uint8_t a[32], prefix[32];
  clamp_seed_hash(seed, a, prefix);

  Sha512 h1;
  h1.update(ByteSpan(prefix, 32));
  h1.update(message);
  auto rh = h1.finish();
  uint8_t r[32];
  sc_reduce_bytes(r, rh.data(), 64);

  Ge R;
  ge_scalarmult_base(R, r);
  Sig64 sig{};
  ge_tobytes(sig.data(), R);

  Sha512 h2;
  h2.update(ByteSpan(sig.data(), 32));
  h2.update(ByteSpan(pub.data(), 32));
  h2.update(message);
  auto kh = h2.finish();
  uint8_t k[32];
  sc_reduce_bytes(k, kh.data(), 64);

  uint8_t s[32];
  sc_muladd(s, k, a, r);
  std::memcpy(sig.data() + 32, s, 32);
  return sig;
}

bool ed25519_verify(ByteSpan message, const Sig64& sig, const Point32& pub) {
  // S must already be reduced: an unreduced S is a malleated signature.
  {
    uint8_t reduced[32];
    sc_reduce_bytes(reduced, sig.data() + 32, 32);
    if (std::memcmp(reduced, sig.data() + 32, 32) != 0) return false;
  }
  Ge A;
  if (!ge_frombytes(A, pub.data())) return false;
  fe_neg(A.X, A.X);            // -A, so the check becomes [S]B + [k](-A) == R
  fe_neg(A.T, A.T);

  Sha512 h;
  h.update(ByteSpan(sig.data(), 32));
  h.update(ByteSpan(pub.data(), 32));
  h.update(message);
  auto kh = h.finish();
  uint8_t k[32];
  sc_reduce_bytes(k, kh.data(), 64);

  Ge sB, kA, sum;
  ge_scalarmult_base(sB, sig.data() + 32);
  ge_scalarmult(kA, k, A);
  ge_add(sum, sB, kA);

  uint8_t check[32];
  ge_tobytes(check, sum);
  uint8_t diff = 0;
  for (int i = 0; i < 32; ++i) diff |= (uint8_t)(check[i] ^ sig[(size_t)i]);
  return diff == 0;
}

}  // namespace aios::crypto
