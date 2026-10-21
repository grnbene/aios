// BigInt arithmetic: schoolbook multiplication, Knuth algorithm D division, two's-complement bitwise operators.
#include "bigint.hpp"

#include <algorithm>
#include <cstring>

#include "internal_num.hpp"

namespace aios::js::big {

namespace {

using Mag = std::vector<uint32_t>;

void trim(Mag& m) {
  while (!m.empty() && m.back() == 0) m.pop_back();
}

BigNum make(Mag m, bool neg) {
  trim(m);
  BigNum r;
  r.neg = neg && !m.empty();
  r.mag = std::move(m);
  return r;
}

int cmp_mag(const Mag& a, const Mag& b) {
  if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
  for (size_t i = a.size(); i-- > 0;)
    if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
  return 0;
}

Mag add_mag(const Mag& a, const Mag& b) {
  const Mag& x = a.size() >= b.size() ? a : b;
  const Mag& y = a.size() >= b.size() ? b : a;
  Mag r;
  r.reserve(x.size() + 1);
  uint64_t c = 0;
  for (size_t i = 0; i < x.size(); ++i) {
    uint64_t s = static_cast<uint64_t>(x[i]) + (i < y.size() ? y[i] : 0) + c;
    r.push_back(static_cast<uint32_t>(s));
    c = s >> 32;
  }
  if (c) r.push_back(static_cast<uint32_t>(c));
  return r;
}

// a >= b
Mag sub_mag(const Mag& a, const Mag& b) {
  Mag r;
  r.reserve(a.size());
  int64_t borrow = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    int64_t d = static_cast<int64_t>(a[i]) - (i < b.size() ? b[i] : 0) - borrow;
    if (d < 0) { d += (1LL << 32); borrow = 1; } else borrow = 0;
    r.push_back(static_cast<uint32_t>(d));
  }
  trim(r);
  return r;
}

Mag mul_mag(const Mag& a, const Mag& b) {
  if (a.empty() || b.empty()) return {};
  Mag r(a.size() + b.size(), 0);
  for (size_t i = 0; i < a.size(); ++i) {
    uint64_t carry = 0;
    const uint64_t ai = a[i];
    for (size_t j = 0; j < b.size(); ++j) {
      uint64_t t = ai * b[j] + r[i + j] + carry;
      r[i + j] = static_cast<uint32_t>(t);
      carry = t >> 32;
    }
    r[i + b.size()] = static_cast<uint32_t>(carry);
  }
  trim(r);
  return r;
}

// m = m * mul + add
void mul_small_add(Mag& m, uint32_t mul, uint32_t add) {
  uint64_t carry = add;
  for (uint32_t& limb : m) {
    uint64_t t = static_cast<uint64_t>(limb) * mul + carry;
    limb = static_cast<uint32_t>(t);
    carry = t >> 32;
  }
  if (carry) m.push_back(static_cast<uint32_t>(carry));
}

// m = m / d in place; returns the remainder.
uint32_t div_small(Mag& m, uint32_t d) {
  uint64_t rem = 0;
  for (size_t i = m.size(); i-- > 0;) {
    uint64_t cur = (rem << 32) | m[i];
    m[i] = static_cast<uint32_t>(cur / d);
    rem = cur % d;
  }
  trim(m);
  return static_cast<uint32_t>(rem);
}

int clz32(uint32_t x) {
  int n = 0;
  if (x == 0) return 32;
  while (!(x & 0x80000000u)) { x <<= 1; ++n; }
  return n;
}

Mag shl_mag(const Mag& a, uint64_t bits) {
  if (a.empty()) return {};
  const size_t limbs = static_cast<size_t>(bits / 32);
  const unsigned rem = static_cast<unsigned>(bits % 32);
  Mag r(limbs, 0);
  r.reserve(limbs + a.size() + 1);
  if (rem == 0) {
    r.insert(r.end(), a.begin(), a.end());
  } else {
    uint32_t carry = 0;
    for (uint32_t limb : a) {
      r.push_back((limb << rem) | carry);
      carry = limb >> (32 - rem);
    }
    if (carry) r.push_back(carry);
  }
  return r;
}

// Floor of the magnitude divided by 2^bits.
Mag shr_mag(const Mag& a, uint64_t bits) {
  const uint64_t limbs = bits / 32;
  const unsigned rem = static_cast<unsigned>(bits % 32);
  if (limbs >= a.size()) return {};
  Mag r(a.begin() + static_cast<std::ptrdiff_t>(limbs), a.end());
  if (rem) {
    for (size_t i = 0; i < r.size(); ++i) {
      uint32_t hi = i + 1 < r.size() ? r[i + 1] : 0;
      r[i] = (r[i] >> rem) | (hi << (32 - rem));
    }
  }
  trim(r);
  return r;
}

// Whether any of the low `bits` bits of the magnitude is set.
bool low_bits_set(const Mag& a, uint64_t bits) {
  const uint64_t limbs = bits / 32;
  const unsigned rem = static_cast<unsigned>(bits % 32);
  for (uint64_t i = 0; i < limbs && i < a.size(); ++i)
    if (a[i]) return true;
  if (rem && limbs < a.size() && (a[limbs] & ((1u << rem) - 1))) return true;
  return false;
}

uint64_t mag_bit_length(const Mag& a) {
  if (a.empty()) return 0;
  return static_cast<uint64_t>(a.size() - 1) * 32 + (32 - clz32(a.back()));
}

// Knuth, TAOCP vol. 2, 4.3.1 algorithm D.
void divmod_mag(const Mag& a, const Mag& b, Mag* q, Mag* r) {
  if (cmp_mag(a, b) < 0) { q->clear(); *r = a; return; }
  if (b.size() == 1) {
    Mag t = a;
    uint32_t rem = div_small(t, b[0]);
    *q = std::move(t);
    r->clear();
    if (rem) r->push_back(rem);
    return;
  }
  const int s = clz32(b.back());
  Mag bn = shl_mag(b, static_cast<uint64_t>(s));
  Mag an = shl_mag(a, static_cast<uint64_t>(s));
  const size_t n = bn.size();
  if (an.size() == a.size()) an.push_back(0);
  const size_t m = an.size() - n - 1;
  Mag quo(m + 1, 0);
  const uint64_t base = 1ull << 32;
  for (size_t j = m + 1; j-- > 0;) {
    uint64_t num = (static_cast<uint64_t>(an[j + n]) << 32) | an[j + n - 1];
    uint64_t qhat = num / bn[n - 1];
    uint64_t rhat = num % bn[n - 1];
    while (qhat >= base || qhat * bn[n - 2] > ((rhat << 32) | an[j + n - 2])) {
      --qhat;
      rhat += bn[n - 1];
      if (rhat >= base) break;
    }
    int64_t borrow = 0;
    uint64_t carry = 0;
    for (size_t i = 0; i < n; ++i) {
      uint64_t p = qhat * bn[i] + carry;
      carry = p >> 32;
      int64_t t = static_cast<int64_t>(an[i + j]) - borrow - static_cast<int64_t>(p & 0xffffffffu);
      an[i + j] = static_cast<uint32_t>(t);
      borrow = t < 0 ? 1 : 0;
    }
    int64_t t = static_cast<int64_t>(an[j + n]) - borrow - static_cast<int64_t>(carry);
    an[j + n] = static_cast<uint32_t>(t);
    if (t < 0) {
      --qhat;
      uint64_t c = 0;
      for (size_t i = 0; i < n; ++i) {
        uint64_t sum = static_cast<uint64_t>(an[i + j]) + bn[i] + c;
        an[i + j] = static_cast<uint32_t>(sum);
        c = sum >> 32;
      }
      an[j + n] += static_cast<uint32_t>(c);
    }
    quo[j] = static_cast<uint32_t>(qhat);
  }
  trim(quo);
  an.resize(n);
  trim(an);
  *q = std::move(quo);
  *r = shr_mag(an, static_cast<uint64_t>(s));
}

// Two's complement over exactly `n` limbs (n must exceed the magnitude's size so the sign bit is free).
Mag to_twos(const BigNum& x, size_t n) {
  Mag r(n, 0);
  std::copy(x.mag.begin(), x.mag.end(), r.begin());
  if (x.neg) {
    uint64_t c = 1;
    for (size_t i = 0; i < n; ++i) {
      uint64_t s = static_cast<uint64_t>(static_cast<uint32_t>(~r[i])) + c;
      r[i] = static_cast<uint32_t>(s);
      c = s >> 32;
    }
  }
  return r;
}

BigNum from_twos(Mag r) {
  const bool neg = !r.empty() && (r.back() & 0x80000000u);
  if (neg) {
    uint64_t c = 1;
    for (size_t i = 0; i < r.size(); ++i) {
      uint64_t s = static_cast<uint64_t>(static_cast<uint32_t>(~r[i])) + c;
      r[i] = static_cast<uint32_t>(s);
      c = s >> 32;
    }
  }
  return make(std::move(r), neg);
}

template <class F>
BigNum bitwise(const BigNum& a, const BigNum& b, F f) {
  const size_t n = std::max(a.mag.size(), b.mag.size()) + 1;
  Mag x = to_twos(a, n);
  const Mag y = to_twos(b, n);
  for (size_t i = 0; i < n; ++i) x[i] = f(x[i], y[i]);
  return from_twos(std::move(x));
}

// 2^bits - m, where m < 2^bits
Mag pow2_minus(uint64_t bits, const Mag& m) {
  Mag p = shl_mag(Mag{1}, bits);
  return sub_mag(p, m);
}

// The low `bits` bits of the magnitude.
Mag low_bits(const Mag& a, uint64_t bits) {
  const uint64_t limbs = bits / 32;
  const unsigned rem = static_cast<unsigned>(bits % 32);
  Mag r(a.begin(), a.begin() + static_cast<std::ptrdiff_t>(std::min<uint64_t>(a.size(), limbs + (rem ? 1 : 0))));
  if (rem && r.size() == limbs + 1) r[limbs] &= (1u << rem) - 1;
  trim(r);
  return r;
}

int digit_value(char16_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'z') return c - 'a' + 10;
  if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
  return 99;
}

}  // namespace

BigNum from_u64(uint64_t v) {
  Mag m;
  if (v) m.push_back(static_cast<uint32_t>(v));
  if (v >> 32) m.push_back(static_cast<uint32_t>(v >> 32));
  return make(std::move(m), false);
}

BigNum from_i64(int64_t v) {
  if (v >= 0) return from_u64(static_cast<uint64_t>(v));
  BigNum r = from_u64(~static_cast<uint64_t>(v) + 1);
  r.neg = true;
  return r;
}

BigNum from_double(double d) {
  uint64_t bits;
  std::memcpy(&bits, &d, 8);
  const bool neg = bits >> 63;
  const int exp = static_cast<int>((bits >> 52) & 0x7ff);
  if (exp == 0) return {};
  uint64_t mant = (bits & ((1ull << 52) - 1)) | (1ull << 52);
  const int e = exp - 1075;
  BigNum r;
  if (e < 0) {
    mant = -e >= 64 ? 0 : mant >> (-e);
    r = from_u64(mant);
  } else {
    r = make(shl_mag(from_u64(mant).mag, static_cast<uint64_t>(e)), false);
  }
  r.neg = neg && !r.mag.empty();
  return r;
}

double to_double(const BigNum& a) {
  const uint64_t bl = mag_bit_length(a.mag);
  if (bl == 0) return 0.0;
  double d;
  if (bl <= 64) {
    uint64_t u = a.mag[0];
    if (a.mag.size() > 1) u |= static_cast<uint64_t>(a.mag[1]) << 32;
    d = static_cast<double>(u);
  } else if (bl > 1024) {
    d = __builtin_huge_val();
  } else {
    const uint64_t shift = bl - 64;
    const Mag top = shr_mag(a.mag, shift);
    uint64_t u = top[0] | (static_cast<uint64_t>(top.size() > 1 ? top[1] : 0) << 32);
    if (low_bits_set(a.mag, shift)) u |= 1;   // sticky: keeps ties-to-even honest below the 53rd bit
    d = static_cast<double>(u);
    uint64_t s = shift;
    while (s >= 32) { d *= 4294967296.0; s -= 32; }
    d *= static_cast<double>(1u << s);
  }
  return a.neg ? -d : d;
}

uint64_t to_u64_wrap(const BigNum& a) {
  uint64_t u = 0;
  if (!a.mag.empty()) u = a.mag[0];
  if (a.mag.size() > 1) u |= static_cast<uint64_t>(a.mag[1]) << 32;
  return a.neg ? ~u + 1 : u;
}

uint64_t bit_length(const BigNum& a) { return mag_bit_length(a.mag); }
bool is_odd(const BigNum& a) { return !a.mag.empty() && (a.mag[0] & 1); }

int cmp(const BigNum& a, const BigNum& b) {
  if (a.neg != b.neg) return a.neg ? -1 : 1;
  int c = cmp_mag(a.mag, b.mag);
  return a.neg ? -c : c;
}

int cmp_double(const BigNum& a, double d) {
  if (d != d) return 2;
  if (d == __builtin_huge_val()) return -1;
  if (d == -__builtin_huge_val()) return 1;
  // trunc(d) without libm: values with magnitude >= 2^52 are integers already
  double t = d;
  if (d > -4503599627370496.0 && d < 4503599627370496.0) {
    long long i = static_cast<long long>(d);
    t = static_cast<double>(i);
  }
  int c = cmp(a, from_double(t));
  if (c != 0) return c;
  const double frac = d - t;
  if (frac > 0) return -1;
  if (frac < 0) return 1;
  return 0;
}

BigNum negate(const BigNum& a) {
  BigNum r = a;
  r.neg = !a.neg && !a.mag.empty();
  return r;
}

BigNum add(const BigNum& a, const BigNum& b) {
  if (a.neg == b.neg) return make(add_mag(a.mag, b.mag), a.neg);
  const int c = cmp_mag(a.mag, b.mag);
  if (c == 0) return {};
  if (c > 0) return make(sub_mag(a.mag, b.mag), a.neg);
  return make(sub_mag(b.mag, a.mag), b.neg);
}

BigNum sub(const BigNum& a, const BigNum& b) { return add(a, negate(b)); }

BigNum mul(const BigNum& a, const BigNum& b) { return make(mul_mag(a.mag, b.mag), a.neg != b.neg); }

Err divmod(const BigNum& a, const BigNum& b, BigNum* q, BigNum* r) {
  if (b.mag.empty()) return Err::DivideByZero;
  Mag qm, rm;
  divmod_mag(a.mag, b.mag, &qm, &rm);
  if (q) *q = make(std::move(qm), a.neg != b.neg);
  if (r) *r = make(std::move(rm), a.neg);
  return Err::None;
}

Err pow(const BigNum& a, const BigNum& e, BigNum* out) {
  if (e.neg) return Err::NegativeExponent;
  if (e.mag.empty()) { *out = from_u64(1); return Err::None; }
  if (a.mag.empty()) { *out = {}; return Err::None; }
  if (a.mag.size() == 1 && a.mag[0] == 1) {
    *out = make(Mag{1}, a.neg && is_odd(e));
    return Err::None;
  }
  if (e.mag.size() > 1) return Err::TooBig;
  const uint32_t exp = e.mag[0];
  if ((mag_bit_length(a.mag) - 1) * static_cast<uint64_t>(exp) > kMaxBits) return Err::TooBig;
  Mag result{1};
  Mag base = a.mag;
  for (uint32_t k = exp;;) {
    if (k & 1) result = mul_mag(result, base);
    k >>= 1;
    if (!k) break;
    base = mul_mag(base, base);
  }
  *out = make(std::move(result), a.neg && (exp & 1));
  return Err::None;
}

BigNum bit_and(const BigNum& a, const BigNum& b) { return bitwise(a, b, [](uint32_t x, uint32_t y) { return x & y; }); }
BigNum bit_or(const BigNum& a, const BigNum& b) { return bitwise(a, b, [](uint32_t x, uint32_t y) { return x | y; }); }
BigNum bit_xor(const BigNum& a, const BigNum& b) { return bitwise(a, b, [](uint32_t x, uint32_t y) { return x ^ y; }); }
BigNum bit_not(const BigNum& a) { return sub(negate(a), from_u64(1)); }   // ~a == -a - 1

Err shift_left(const BigNum& a, const BigNum& count, BigNum* out) {
  if (count.neg) return shift_right(a, negate(count), out);
  if (a.mag.empty() || count.mag.empty()) { *out = a; return Err::None; }
  if (count.mag.size() > 1 || count.mag[0] > kMaxBits || mag_bit_length(a.mag) + count.mag[0] > kMaxBits) return Err::TooBig;
  *out = make(shl_mag(a.mag, count.mag[0]), a.neg);
  return Err::None;
}

Err shift_right(const BigNum& a, const BigNum& count, BigNum* out) {
  if (count.neg) return shift_left(a, negate(count), out);
  if (a.mag.empty() || count.mag.empty()) { *out = a; return Err::None; }
  const uint64_t bl = mag_bit_length(a.mag);
  if (count.mag.size() > 1 || count.mag[0] > bl) {
    *out = a.neg ? make(Mag{1}, true) : BigNum();
    return Err::None;
  }
  const uint64_t n = count.mag[0];
  Mag q = shr_mag(a.mag, n);
  if (a.neg && low_bits_set(a.mag, n)) q = add_mag(q, Mag{1});   // floor, not truncation, for negatives
  *out = make(std::move(q), a.neg);
  return Err::None;
}

Err as_uint_n(uint64_t bits, const BigNum& a, BigNum* out) {
  if (bits == 0 || a.mag.empty()) { *out = {}; return Err::None; }
  if (!a.neg && mag_bit_length(a.mag) <= bits) { *out = a; return Err::None; }
  if (bits > kMaxBits && a.neg) return Err::TooBig;
  Mag m = low_bits(a.mag, bits);
  if (!a.neg) { *out = make(std::move(m), false); return Err::None; }
  if (m.empty()) { *out = {}; return Err::None; }
  *out = make(pow2_minus(bits, m), false);
  return Err::None;
}

Err as_int_n(uint64_t bits, const BigNum& a, BigNum* out) {
  if (bits == 0 || a.mag.empty()) { *out = {}; return Err::None; }
  if (mag_bit_length(a.mag) < bits) { *out = a; return Err::None; }   // |a| < 2^(bits-1): already in range
  // |a| >= 2^(bits-1) (so bits is at most kMaxBits-ish): reduce modulo 2^bits, then fold the top half to negatives.
  BigNum u;
  Err e = as_uint_n(bits, a, &u);
  if (e != Err::None) return e;
  const uint64_t top = bits - 1;
  const bool sign = top / 32 < u.mag.size() && (u.mag[top / 32] >> (top % 32)) & 1;
  if (!sign) { *out = u; return Err::None; }
  *out = make(pow2_minus(bits, u.mag), true);
  return Err::None;
}

std::string to_string(const BigNum& a, int radix) {
  if (a.mag.empty()) return "0";
  uint32_t chunk = static_cast<uint32_t>(radix);
  int per = 1;
  while (static_cast<uint64_t>(chunk) * radix <= 0xffffffffull) { chunk *= static_cast<uint32_t>(radix); ++per; }
  static const char kDigits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
  Mag m = a.mag;
  std::string out;   // least significant digit first
  while (!m.empty()) {
    uint32_t rem = div_small(m, chunk);
    for (int i = 0; i < per; ++i) {
      if (m.empty() && rem == 0) break;
      out.push_back(kDigits[rem % radix]);
      rem /= static_cast<uint32_t>(radix);
    }
  }
  if (a.neg) out.push_back('-');
  std::reverse(out.begin(), out.end());
  return out;
}

bool parse_digits(std::u16string_view digits, int radix, BigNum* out) {
  if (digits.empty()) return false;
  uint32_t chunk = static_cast<uint32_t>(radix);
  int per = 1;
  while (static_cast<uint64_t>(chunk) * radix <= 0xffffffffull) { chunk *= static_cast<uint32_t>(radix); ++per; }
  Mag m;
  size_t i = 0;
  while (i < digits.size()) {
    uint32_t acc = 0, mulv = 1;
    for (int k = 0; k < per && i < digits.size(); ++k, ++i) {
      int d = digit_value(digits[i]);
      if (d >= radix) return false;
      acc = acc * static_cast<uint32_t>(radix) + static_cast<uint32_t>(d);
      mulv *= static_cast<uint32_t>(radix);
    }
    mul_small_add(m, mulv, acc);
  }
  *out = make(std::move(m), false);
  return true;
}

bool string_to_bigint(std::u16string_view s, BigNum* out) {
  size_t b = 0, e = s.size();
  while (b < e && is_js_whitespace(s[b])) ++b;
  while (e > b && is_js_whitespace(s[e - 1])) --e;
  s = s.substr(b, e - b);
  if (s.empty()) { *out = {}; return true; }
  if (s.size() > 2 && s[0] == '0') {
    const char16_t p = s[1] | 0x20;
    const int radix = p == 'x' ? 16 : p == 'o' ? 8 : p == 'b' ? 2 : 0;
    if (radix) return parse_digits(s.substr(2), radix, out);
  }
  bool neg = false;
  if (s[0] == '+' || s[0] == '-') { neg = s[0] == '-'; s.remove_prefix(1); }
  if (!parse_digits(s, 10, out)) return false;
  if (neg) *out = negate(*out);
  return true;
}

size_t hash(const BigNum& a) {
  size_t h = a.neg ? 0x9e3779b97f4a7c15ull : 0x1234567ull;
  for (uint32_t limb : a.mag) h = (h ^ limb) * 1099511628211ull;
  return h;
}

}  // namespace aios::js::big
