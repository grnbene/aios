#include "secp256k1_math.hpp"

namespace aios::crypto::k1 {

namespace {

// p = 2^256 - 2^32 - 977, so 2^256 mod p is the 33-bit constant below and a
// 512-bit product folds down with two multiplications instead of a division.
const uint64_t P[4] = {0xFFFFFFFEFFFFFC2FULL, 0xFFFFFFFFFFFFFFFFULL,
                       0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL};
const uint64_t R_FOLD = 0x1000003D1ULL;

const uint64_t P_MINUS_2[4] = {0xFFFFFFFEFFFFFC2DULL, 0xFFFFFFFFFFFFFFFFULL,
                               0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL};
const uint64_t P_PLUS_1_DIV_4[4] = {0xFFFFFFFFBFFFFF0CULL, 0xFFFFFFFFFFFFFFFFULL,
                                    0xFFFFFFFFFFFFFFFFULL, 0x3FFFFFFFFFFFFFFFULL};

// Absorbs a carry out of bit 256 by folding it back through 2^256 = R_FOLD.
// Three rounds are enough for any carry the callers can produce: the first
// drops it below 2, the second can only carry when the limbs wrap to a value
// smaller than R_FOLD, and the third cannot carry at all.
void fold_carry(uint64_t n[4], uint64_t c) {
  for (int round = 0; round < 3; ++round) {
    u128 m = (u128)c * R_FOLD;
    u128 s = (u128)n[0] + (uint64_t)m;
    n[0] = (uint64_t)s;
    s >>= 64;
    s += (u128)n[1] + (uint64_t)(m >> 64);
    n[1] = (uint64_t)s;
    s >>= 64;
    s += n[2];
    n[2] = (uint64_t)s;
    s >>= 64;
    s += n[3];
    n[3] = (uint64_t)s;
    c = (uint64_t)(s >> 64);
  }
}

// One conditional subtraction, selected by mask rather than by branch. The
// caller guarantees the input is below 2p, which every path here does.
void cond_sub_p(uint64_t n[4]) {
  uint64_t d[4];
  u128 borrow = 0;
  for (int i = 0; i < 4; ++i) {
    u128 t = (u128)n[i] - P[i] - borrow;
    d[i] = (uint64_t)t;
    borrow = (t >> 64) & 1;
  }
  uint64_t mask = (uint64_t)borrow - 1;   // all ones when n >= p
  for (int i = 0; i < 4; ++i) n[i] = (n[i] & ~mask) | (d[i] & mask);
}

void pow_p(Fe& r, const Fe& a, const uint64_t e[4]) {
  Fe base = a, acc;
  fe_set_u64(acc, 1);
  for (int i = 0; i < 256; ++i) {
    uint64_t bit = (e[i >> 6] >> (i & 63)) & 1;
    Fe mult;
    fe_mul(mult, acc, base);
    fe_cmov(acc, mult, (uint64_t)0 - bit);
    fe_sqr(base, base);
  }
  r = acc;
}

}  // namespace

void fe_zero(Fe& r) { r.n[0] = r.n[1] = r.n[2] = r.n[3] = 0; }

void fe_set_u64(Fe& r, uint64_t v) {
  r.n[0] = v;
  r.n[1] = r.n[2] = r.n[3] = 0;
}

bool fe_set_b32(Fe& r, const uint8_t b[32]) {
  for (int i = 0; i < 4; ++i) {
    uint64_t v = 0;
    for (int j = 0; j < 8; ++j) v = (v << 8) | b[i * 8 + j];
    r.n[3 - i] = v;
  }
  for (int i = 3; i >= 0; --i) {
    if (r.n[i] < P[i]) return true;
    if (r.n[i] > P[i]) { cond_sub_p(r.n); return false; }
  }
  cond_sub_p(r.n);
  return false;
}

void fe_get_b32(uint8_t b[32], const Fe& a) {
  for (int i = 0; i < 4; ++i) {
    uint64_t v = a.n[3 - i];
    for (int j = 0; j < 8; ++j) b[i * 8 + j] = (uint8_t)(v >> (56 - 8 * j));
  }
}

void fe_add(Fe& r, const Fe& a, const Fe& b) {
  u128 s = 0;
  for (int i = 0; i < 4; ++i) {
    s += (u128)a.n[i] + b.n[i];
    r.n[i] = (uint64_t)s;
    s >>= 64;
  }
  fold_carry(r.n, (uint64_t)s);
  cond_sub_p(r.n);
}

void fe_sub(Fe& r, const Fe& a, const Fe& b) {
  uint64_t t[4];
  u128 borrow = 0;
  for (int i = 0; i < 4; ++i) {
    u128 d = (u128)P[i] - b.n[i] - borrow;
    t[i] = (uint64_t)d;
    borrow = (d >> 64) & 1;
  }
  u128 s = 0;
  for (int i = 0; i < 4; ++i) {
    s += (u128)a.n[i] + t[i];
    r.n[i] = (uint64_t)s;
    s >>= 64;
  }
  fold_carry(r.n, (uint64_t)s);
  cond_sub_p(r.n);
}

void fe_neg(Fe& r, const Fe& a) {
  Fe zero;
  fe_zero(zero);
  fe_sub(r, zero, a);
}

void fe_mul(Fe& r, const Fe& a, const Fe& b) {
  uint64_t t[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  for (int i = 0; i < 4; ++i) {
    u128 carry = 0;
    for (int j = 0; j < 4; ++j) {
      u128 acc = (u128)a.n[i] * b.n[j] + t[i + j] + carry;
      t[i + j] = (uint64_t)acc;
      carry = acc >> 64;
    }
    t[i + 4] = (uint64_t)carry;
  }

  uint64_t lo[4] = {t[0], t[1], t[2], t[3]};
  uint64_t m[5];
  u128 c = 0;
  for (int i = 0; i < 4; ++i) {
    c += (u128)t[i + 4] * R_FOLD;
    m[i] = (uint64_t)c;
    c >>= 64;
  }
  m[4] = (uint64_t)c;

  u128 s = 0;
  for (int i = 0; i < 4; ++i) {
    s += (u128)lo[i] + m[i];
    lo[i] = (uint64_t)s;
    s >>= 64;
  }
  fold_carry(lo, m[4] + (uint64_t)s);
  cond_sub_p(lo);
  for (int i = 0; i < 4; ++i) r.n[i] = lo[i];
}

void fe_sqr(Fe& r, const Fe& a) { fe_mul(r, a, a); }

void fe_inv(Fe& r, const Fe& a) { pow_p(r, a, P_MINUS_2); }

bool fe_sqrt(Fe& r, const Fe& a) {
  Fe root;
  pow_p(root, a, P_PLUS_1_DIV_4);
  Fe check;
  fe_sqr(check, root);
  if (!fe_equal(check, a)) return false;
  r = root;
  return true;
}

bool fe_is_zero(const Fe& a) { return (a.n[0] | a.n[1] | a.n[2] | a.n[3]) == 0; }

bool fe_is_odd(const Fe& a) { return (a.n[0] & 1) != 0; }

bool fe_equal(const Fe& a, const Fe& b) {
  uint64_t d = 0;
  for (int i = 0; i < 4; ++i) d |= a.n[i] ^ b.n[i];
  return d == 0;
}

void fe_cmov(Fe& r, const Fe& a, uint64_t mask) {
  for (int i = 0; i < 4; ++i) r.n[i] = (r.n[i] & ~mask) | (a.n[i] & mask);
}

}  // namespace aios::crypto::k1
