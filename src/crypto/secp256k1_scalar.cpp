#include "secp256k1_math.hpp"

namespace aios::crypto::k1 {

namespace {

// The group order n has no Solinas shape worth exploiting, so wide values are
// reduced by a fixed 512-step shift-and-conditional-subtract instead. It runs
// in constant time, costs microseconds, and has a fraction of the correctness
// surface of a Barrett or Montgomery reduction.
const uint64_t N[4] = {0xBFD25E8CD0364141ULL, 0xBAAEDCE6AF48A03BULL,
                       0xFFFFFFFFFFFFFFFEULL, 0xFFFFFFFFFFFFFFFFULL};
const uint64_t HALF_N[4] = {0xDFE92F46681B20A0ULL, 0x5D576E7357A4501DULL,
                            0xFFFFFFFFFFFFFFFFULL, 0x7FFFFFFFFFFFFFFFULL};
const uint64_t N_MINUS_2[4] = {0xBFD25E8CD036413FULL, 0xBAAEDCE6AF48A03BULL,
                               0xFFFFFFFFFFFFFFFEULL, 0xFFFFFFFFFFFFFFFFULL};

// Subtracts n when the 257-bit value (top:limbs) is at least n. The borrow out
// of the low 256 bits is exactly cancelled by a set top bit, so the same
// subtraction serves both cases.
void cond_sub_n(uint64_t a[4], uint64_t top) {
  uint64_t d[4];
  u128 borrow = 0;
  for (int i = 0; i < 4; ++i) {
    u128 t = (u128)a[i] - N[i] - borrow;
    d[i] = (uint64_t)t;
    borrow = (t >> 64) & 1;
  }
  uint64_t mask = (uint64_t)0 - (((uint64_t)1 - (uint64_t)borrow) | top);
  for (int i = 0; i < 4; ++i) a[i] = (a[i] & ~mask) | (d[i] & mask);
}

void reduce_wide(Scalar& r, const uint64_t t[8]) {
  uint64_t acc[4] = {0, 0, 0, 0};
  for (int i = 511; i >= 0; --i) {
    uint64_t bit = (t[i >> 6] >> (i & 63)) & 1;
    uint64_t carry = bit;
    for (int j = 0; j < 4; ++j) {
      uint64_t next = acc[j] >> 63;
      acc[j] = (acc[j] << 1) | carry;
      carry = next;
    }
    cond_sub_n(acc, carry);
  }
  for (int i = 0; i < 4; ++i) r.n[i] = acc[i];
}

bool geq_n(const uint64_t a[4]) {
  for (int i = 3; i >= 0; --i) {
    if (a[i] < N[i]) return false;
    if (a[i] > N[i]) return true;
  }
  return true;
}

void pow_n(Scalar& r, const Scalar& a, const uint64_t e[4]) {
  Scalar base = a, acc;
  sc_set_u64(acc, 1);
  for (int i = 0; i < 256; ++i) {
    uint64_t bit = (e[i >> 6] >> (i & 63)) & 1;
    Scalar mult;
    sc_mul(mult, acc, base);
    sc_cmov(acc, mult, (uint64_t)0 - bit);
    sc_mul(base, base, base);
  }
  r = acc;
}

}  // namespace

void sc_zero(Scalar& r) { r.n[0] = r.n[1] = r.n[2] = r.n[3] = 0; }

void sc_set_u64(Scalar& r, uint64_t v) {
  r.n[0] = v;
  r.n[1] = r.n[2] = r.n[3] = 0;
}

bool sc_set_b32(Scalar& r, const uint8_t b[32]) {
  for (int i = 0; i < 4; ++i) {
    uint64_t v = 0;
    for (int j = 0; j < 8; ++j) v = (v << 8) | b[i * 8 + j];
    r.n[3 - i] = v;
  }
  bool over = geq_n(r.n);
  cond_sub_n(r.n, 0);
  return over;
}

void sc_get_b32(uint8_t b[32], const Scalar& a) {
  for (int i = 0; i < 4; ++i) {
    uint64_t v = a.n[3 - i];
    for (int j = 0; j < 8; ++j) b[i * 8 + j] = (uint8_t)(v >> (56 - 8 * j));
  }
}

void sc_add(Scalar& r, const Scalar& a, const Scalar& b) {
  u128 s = 0;
  for (int i = 0; i < 4; ++i) {
    s += (u128)a.n[i] + b.n[i];
    r.n[i] = (uint64_t)s;
    s >>= 64;
  }
  cond_sub_n(r.n, (uint64_t)s);
}

void sc_mul(Scalar& r, const Scalar& a, const Scalar& b) {
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
  reduce_wide(r, t);
}

void sc_neg(Scalar& r, const Scalar& a) {
  uint64_t d[4];
  u128 borrow = 0;
  for (int i = 0; i < 4; ++i) {
    u128 t = (u128)N[i] - a.n[i] - borrow;
    d[i] = (uint64_t)t;
    borrow = (t >> 64) & 1;
  }
  uint64_t nonzero = a.n[0] | a.n[1] | a.n[2] | a.n[3];
  uint64_t mask = (uint64_t)0 - (uint64_t)((nonzero | (~nonzero + 1)) >> 63);
  for (int i = 0; i < 4; ++i) r.n[i] = d[i] & mask;
}

void sc_inv(Scalar& r, const Scalar& a) { pow_n(r, a, N_MINUS_2); }

bool sc_is_zero(const Scalar& a) { return (a.n[0] | a.n[1] | a.n[2] | a.n[3]) == 0; }

bool sc_is_high(const Scalar& a) {
  for (int i = 3; i >= 0; --i) {
    if (a.n[i] < HALF_N[i]) return false;
    if (a.n[i] > HALF_N[i]) return true;
  }
  return false;
}

bool sc_equal(const Scalar& a, const Scalar& b) {
  uint64_t d = 0;
  for (int i = 0; i < 4; ++i) d |= a.n[i] ^ b.n[i];
  return d == 0;
}

void sc_reduce_b64(Scalar& r, const uint8_t b[64]) {
  uint64_t t[8];
  for (int i = 0; i < 8; ++i) {
    uint64_t v = 0;
    for (int j = 0; j < 8; ++j) v = (v << 8) | b[i * 8 + j];
    t[7 - i] = v;
  }
  reduce_wide(r, t);
}

int sc_bit(const Scalar& a, unsigned i) {
  if (i >= 256) return 0;
  return (int)((a.n[i >> 6] >> (i & 63)) & 1);
}

void sc_cmov(Scalar& r, const Scalar& a, uint64_t mask) {
  for (int i = 0; i < 4; ++i) r.n[i] = (r.n[i] & ~mask) | (a.n[i] & mask);
}

void fe_to_scalar(Scalar& r, const Fe& a, bool& overflowed) {
  uint8_t b[32];
  fe_get_b32(b, a);
  overflowed = sc_set_b32(r, b);
}

}  // namespace aios::crypto::k1
