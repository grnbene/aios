#include "aios/crypto/nistp.hpp"

#include <cstring>

#include "aios/crypto/sha256.hpp"
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
// A fixed-width unsigned integer, wide enough for P-521 with room to hold a
// product. Correctness is chosen over speed throughout: reduction is long
// division rather than Montgomery or a curve-specific fast path, because a
// verification that is a few milliseconds slower is invisible next to a network
// round trip, and a reduction that is subtly wrong is a machine that trusts the
// wrong certificate.
// ---------------------------------------------------------------------------
constexpr int kLimbs = 18;            // 1152 bits: a 521-bit product (1042 bits) fits

struct Num {
  uint64_t v[kLimbs];
  Num() { std::memset(v, 0, sizeof(v)); }
};

int cmp(const Num& a, const Num& b) {
  for (int i = kLimbs - 1; i >= 0; --i) {
    if (a.v[i] != b.v[i]) return a.v[i] > b.v[i] ? 1 : -1;
  }
  return 0;
}

bool is_zero(const Num& a) {
  for (int i = 0; i < kLimbs; ++i)
    if (a.v[i]) return false;
  return true;
}

// a += b, discarding any carry past the top limb (callers keep values in range).
void add_into(Num& a, const Num& b) {
  AIOS_U128 carry = 0;
  for (int i = 0; i < kLimbs; ++i) {
    AIOS_U128 t = (AIOS_U128)a.v[i] + b.v[i] + carry;
    a.v[i] = (uint64_t)t;
    carry = t >> 64;
  }
}

void sub_into(Num& a, const Num& b) {
  AIOS_U128 borrow = 0;
  for (int i = 0; i < kLimbs; ++i) {
    AIOS_U128 t = (AIOS_U128)a.v[i] - b.v[i] - borrow;
    a.v[i] = (uint64_t)t;
    borrow = (t >> 64) ? 1 : 0;
  }
}

void shl1(Num& a) {
  uint64_t carry = 0;
  for (int i = 0; i < kLimbs; ++i) {
    uint64_t next = a.v[i] >> 63;
    a.v[i] = (a.v[i] << 1) | carry;
    carry = next;
  }
}

int bit_at(const Num& a, int i) { return (int)((a.v[i / 64] >> (i % 64)) & 1); }

int top_bit(const Num& a) {
  for (int i = kLimbs * 64 - 1; i >= 0; --i)
    if (bit_at(a, i)) return i;
  return -1;
}

Num from_bytes(ByteSpan b) {
  Num n;
  // Big-endian, as every wire format here writes integers.
  for (size_t i = 0; i < b.size(); ++i) {
    size_t from_end = b.size() - 1 - i;
    if (from_end / 8 >= (size_t)kLimbs) continue;
    n.v[from_end / 8] |= (uint64_t)b[i] << (8 * (from_end % 8));
  }
  return n;
}

void to_bytes(const Num& n, uint8_t* out, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    size_t from_end = len - 1 - i;
    out[i] = (from_end / 8 < (size_t)kLimbs) ? (uint8_t)(n.v[from_end / 8] >> (8 * (from_end % 8))) : 0;
  }
}

// wide = a * b, schoolbook.
Num mul_wide(const Num& a, const Num& b) {
  uint64_t acc[kLimbs * 2];
  std::memset(acc, 0, sizeof(acc));
  for (int i = 0; i < kLimbs; ++i) {
    if (!a.v[i]) continue;
    AIOS_U128 carry = 0;
    for (int j = 0; j + i < kLimbs * 2 && j < kLimbs; ++j) {
      AIOS_U128 t = (AIOS_U128)a.v[i] * b.v[j] + acc[i + j] + carry;
      acc[i + j] = (uint64_t)t;
      carry = t >> 64;
    }
    for (int k = i + kLimbs; carry && k < kLimbs * 2; ++k) {
      AIOS_U128 t = (AIOS_U128)acc[k] + carry;
      acc[k] = (uint64_t)t;
      carry = t >> 64;
    }
  }
  Num out;
  // Everything this code multiplies stays under 2^1152 once reduced, so the
  // upper half is only ever carry space; the reduction below consumes it.
  for (int i = 0; i < kLimbs; ++i) out.v[i] = acc[i];
  Num high;
  for (int i = 0; i < kLimbs; ++i) high.v[i] = acc[i + kLimbs];
  // A product wider than the low half cannot happen for our operand sizes
  // (both < 2^521, so the product is < 2^1042 and fits exactly).
  (void)high;
  return out;
}

// a mod m, by long division: the schoolbook method, one bit at a time.
Num mod_reduce(const Num& a, const Num& m) {
  if (cmp(a, m) < 0) return a;
  Num rem;
  int top = top_bit(a);
  for (int i = top; i >= 0; --i) {
    shl1(rem);
    rem.v[0] |= (uint64_t)bit_at(a, i);
    if (cmp(rem, m) >= 0) sub_into(rem, m);
  }
  return rem;
}

Num mod_add(const Num& a, const Num& b, const Num& m) {
  Num r = a;
  add_into(r, b);
  if (cmp(r, m) >= 0) sub_into(r, m);
  return r;
}

Num mod_sub(const Num& a, const Num& b, const Num& m) {
  Num r = a;
  if (cmp(a, b) < 0) add_into(r, m);
  sub_into(r, b);
  return r;
}

Num mod_mul_slow(const Num& a, const Num& b, const Num& m) { return mod_reduce(mul_wide(a, b), m); }

// ---------------------------------------------------------------------------
// Montgomery multiplication, for the odd prime moduli every curve here uses
// (the field primes and the group orders). The long division above costs a
// 1152-bit compare, shift and subtract per BIT of the product; a point
// multiplication does thousands of field multiplications, and a TLS handshake
// does several point multiplications, so a page that opens a dozen connections
// kept a core busy for seconds. Word-wise Montgomery does the same product in a
// few dozen limb operations.
//
// The result is the same number: the context is built with the slow reduction,
// and anything that does not fit the method (an even or oversized modulus)
// still takes the slow path.
// ---------------------------------------------------------------------------
constexpr int kMontMaxLimbs = 9;   // 576 bits: P-521 is the widest modulus

struct MontCtx {
  Num m;
  Num r2;                 // R^2 mod m, with R = 2^(64 * n)
  uint64_t mprime = 0;    // -m^-1 mod 2^64
  int n = 0;              // limbs in m
  bool valid = false;
};

MontCtx make_mont(const Num& m) {
  MontCtx c;
  int n = kLimbs;
  while (n > 0 && m.v[n - 1] == 0) --n;
  if (n == 0 || n > kMontMaxLimbs || !(m.v[0] & 1)) return c;
  uint64_t inv = m.v[0];   // correct to 3 bits for an odd number; each step doubles that
  for (int i = 0; i < 6; ++i) inv *= 2 - m.v[0] * inv;
  c.m = m;
  c.n = n;
  c.mprime = (uint64_t)0 - inv;
  Num r;                   // R mod m
  r.v[n] = 1;              // 2^(64 n); n <= 9 so this is inside the 18 limbs
  r = mod_reduce(r, m);
  c.r2 = mod_mul_slow(r, r, m);
  c.valid = true;
  return c;
}

// a * b * R^-1 mod m, for a, b < m.
Num mont_mul(const MontCtx& c, const Num& a, const Num& b) {
  const int n = c.n;
  uint64_t t[kMontMaxLimbs + 2] = {0};
  for (int i = 0; i < n; ++i) {
    AIOS_U128 carry = 0;
    for (int j = 0; j < n; ++j) {
      AIOS_U128 s = (AIOS_U128)a.v[j] * b.v[i] + t[j] + carry;
      t[j] = (uint64_t)s;
      carry = s >> 64;
    }
    AIOS_U128 s = (AIOS_U128)t[n] + carry;
    t[n] = (uint64_t)s;
    t[n + 1] = (uint64_t)(s >> 64);
    const uint64_t q = t[0] * c.mprime;
    s = (AIOS_U128)q * c.m.v[0] + t[0];
    carry = s >> 64;
    for (int j = 1; j < n; ++j) {
      s = (AIOS_U128)q * c.m.v[j] + t[j] + carry;
      t[j - 1] = (uint64_t)s;
      carry = s >> 64;
    }
    s = (AIOS_U128)t[n] + carry;
    t[n - 1] = (uint64_t)s;
    t[n] = t[n + 1] + (uint64_t)(s >> 64);
  }
  Num out;
  for (int i = 0; i < n; ++i) out.v[i] = t[i];
  out.v[n] = t[n];
  if (cmp(out, c.m) >= 0) sub_into(out, c.m);
  return out;
}

Num mod_mul(const Num& a, const Num& b, const Num& m) {
  // The context is rebuilt only when the modulus changes: a handshake alternates
  // between a field prime and a group order, so a few slots are plenty.
  thread_local MontCtx cache[4];
  thread_local int next = 0;
  MontCtx* ctx = nullptr;
  for (auto& c : cache)
    if (c.valid && cmp(c.m, m) == 0) { ctx = &c; break; }
  if (!ctx) {
    MontCtx made = make_mont(m);
    if (!made.valid) return mod_mul_slow(a, b, m);
    cache[next] = made;
    ctx = &cache[next];
    next = (next + 1) % 4;
  }
  const Num ar = cmp(a, m) < 0 ? a : mod_reduce(a, m);
  const Num br = cmp(b, m) < 0 ? b : mod_reduce(b, m);
  return mont_mul(*ctx, mont_mul(*ctx, ar, br), ctx->r2);
}

Num mod_pow(const Num& base, const Num& exp, const Num& m) {
  Num result;
  result.v[0] = 1;
  Num b = mod_reduce(base, m);
  int top = top_bit(exp);
  for (int i = top; i >= 0; --i) {
    result = mod_mul(result, result, m);
    if (bit_at(exp, i)) result = mod_mul(result, b, m);
  }
  return result;
}

// 1/a mod m for prime m, by Fermat's little theorem.
Num mod_inv(const Num& a, const Num& m) {
  Num e = m;
  Num two;
  two.v[0] = 2;
  sub_into(e, two);
  return mod_pow(a, e, m);
}

// ---------------------------------------------------------------------------
// The curves
// ---------------------------------------------------------------------------
struct Curve {
  Num p, n, b, gx, gy;
  size_t bytes;
};

Num parse(const char* hex) {
  ByteBuffer raw;
  auto val = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
  for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) raw.push_back((uint8_t)(val(hex[i]) * 16 + val(hex[i + 1])));
  return from_bytes(ByteSpan(raw.data(), raw.size()));
}

const Curve& curve_of(NistCurve which) {
  static Curve p256, p384, p521;
  static bool ready = false;
  if (!ready) {
    p256.p  = parse("ffffffff00000001000000000000000000000000ffffffffffffffffffffffff");
    p256.n  = parse("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551");
    p256.b  = parse("5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b");
    p256.gx = parse("6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296");
    p256.gy = parse("4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5");
    p256.bytes = 32;
    p384.p  = parse("fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff");
    p384.n  = parse("ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973");
    p384.b  = parse("b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef");
    p384.gx = parse("aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7");
    p384.gy = parse("3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f");
    p384.bytes = 48;
    p521.p  = parse("01ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    p521.n  = parse("01fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffa51868783bf2f966b7fcc0148f709a5d03bb5c9b8899c47aebb6fb71e91386409");
    p521.b  = parse("0051953eb9618e1c9a1f929a21a0b68540eea2da725b99b315f3b8b489918ef109e156193951ec7e937b1652c0bd3bb1bf073573df883d2c34f1ef451fd46b503f00");
    p521.gx = parse("00c6858e06b70404e9cd9e3ecb662395b4429c648139053fb521f828af606b4d3dbaa14b5e77efe75928fe1dc127a2ffa8de3348b3c1856a429bf97e7e31c2e5bd66");
    p521.gy = parse("011839296a789a3bc0045c8a5fb42c7d1bd998f54449579b446817afbd17273e662c97ee72995ef42640c550b9013fad0761353c7086a272c24088be94769fd16650");
    p521.bytes = 66;
    ready = true;
  }
  switch (which) {
    case NistCurve::P256: return p256;
    case NistCurve::P384: return p384;
    case NistCurve::P521: return p521;
  }
  return p256;
}

// Jacobian coordinates: (X, Y, Z) is the affine (X/Z^2, Y/Z^3). One inversion
// at the end instead of one per addition.
struct Point {
  Num x, y, z;      // z == 0 is the point at infinity
};

bool at_infinity(const Point& p) { return is_zero(p.z); }

Point double_point(const Point& P, const Curve& c) {
  if (at_infinity(P)) return P;
  const Num& p = c.p;
  Num A = mod_mul(P.y, P.y, p);                       // Y^2
  Num B = mod_mul(P.x, A, p);
  B = mod_add(B, B, p); B = mod_add(B, B, p);         // 4XY^2
  Num C = mod_mul(A, A, p);
  C = mod_add(C, C, p); C = mod_add(C, C, p); C = mod_add(C, C, p);   // 8Y^4
  Num z2 = mod_mul(P.z, P.z, p);
  Num z4 = mod_mul(z2, z2, p);
  // a = -3, so D = 3(X - Z^2)(X + Z^2)
  Num D = mod_mul(mod_sub(P.x, z2, p), mod_add(P.x, z2, p), p);
  D = mod_add(mod_add(D, D, p), D, p);
  Point R;
  R.x = mod_sub(mod_mul(D, D, p), mod_add(B, B, p), p);
  R.y = mod_sub(mod_mul(D, mod_sub(B, R.x, p), p), C, p);
  R.z = mod_mul(mod_add(P.y, P.y, p), P.z, p);
  (void)z4;
  return R;
}

Point add_points(const Point& P, const Point& Q, const Curve& c) {
  if (at_infinity(P)) return Q;
  if (at_infinity(Q)) return P;
  const Num& p = c.p;
  Num z1z1 = mod_mul(P.z, P.z, p);
  Num z2z2 = mod_mul(Q.z, Q.z, p);
  Num u1 = mod_mul(P.x, z2z2, p);
  Num u2 = mod_mul(Q.x, z1z1, p);
  Num s1 = mod_mul(P.y, mod_mul(z2z2, Q.z, p), p);
  Num s2 = mod_mul(Q.y, mod_mul(z1z1, P.z, p), p);
  if (cmp(u1, u2) == 0) {
    if (cmp(s1, s2) != 0) { Point inf; return inf; }    // P + (-P)
    return double_point(P, c);
  }
  Num h = mod_sub(u2, u1, p);
  Num i = mod_mul(mod_add(h, h, p), mod_add(h, h, p), p);
  Num j = mod_mul(h, i, p);
  Num r = mod_sub(s2, s1, p);
  r = mod_add(r, r, p);
  Num v = mod_mul(u1, i, p);
  Point R;
  R.x = mod_sub(mod_sub(mod_mul(r, r, p), j, p), mod_add(v, v, p), p);
  Num t = mod_mul(s1, j, p);
  R.y = mod_sub(mod_mul(r, mod_sub(v, R.x, p), p), mod_add(t, t, p), p);
  R.z = mod_mul(mod_mul(mod_add(P.z, Q.z, p), mod_add(P.z, Q.z, p), p), h, p);
  R.z = mod_sub(R.z, mod_mul(mod_add(z1z1, z2z2, p), h, p), p);
  return R;
}

Point scalar_mult(const Num& k, const Point& P, const Curve& c) {
  Point acc;                       // infinity
  int top = top_bit(k);
  for (int i = top; i >= 0; --i) {
    acc = double_point(acc, c);
    if (bit_at(k, i)) acc = add_points(acc, P, c);
  }
  return acc;
}

// The affine (x, y) of a Jacobian point.
bool affine_point(const Point& P, const Curve& c, Num& out_x, Num& out_y) {
  if (at_infinity(P)) return false;
  Num zi = mod_inv(P.z, c.p);
  Num zi2 = mod_mul(zi, zi, c.p);
  Num zi3 = mod_mul(zi2, zi, c.p);
  out_x = mod_mul(P.x, zi2, c.p);
  out_y = mod_mul(P.y, zi3, c.p);
  return true;
}

// The affine x of a Jacobian point, which is all a verification compares.
bool affine_x(const Point& P, const Curve& c, Num& out) {
  if (at_infinity(P)) return false;
  Num zi = mod_inv(P.z, c.p);
  Num zi2 = mod_mul(zi, zi, c.p);
  out = mod_mul(P.x, zi2, c.p);
  return true;
}

bool on_curve(const Num& x, const Num& y, const Curve& c) {
  const Num& p = c.p;
  Num lhs = mod_mul(y, y, p);
  Num rhs = mod_mul(mod_mul(x, x, p), x, p);           // x^3
  Num three_x = mod_add(mod_add(x, x, p), x, p);
  rhs = mod_sub(rhs, three_x, p);                      // - 3x
  rhs = mod_add(rhs, c.b, p);
  return cmp(lhs, rhs) == 0;
}

bool split_pub(NistCurve which, ByteSpan pub, Num& x, Num& y) {
  const Curve& c = curve_of(which);
  if (pub.size() != 1 + 2 * c.bytes || pub[0] != 0x04) return false;   // uncompressed only
  x = from_bytes(pub.subspan(1, c.bytes));
  y = from_bytes(pub.subspan(1 + c.bytes, c.bytes));
  return true;
}

bool asn1_tlv(ByteSpan der, size_t& p, uint8_t want, ByteSpan& body) {
  if (p + 2 > der.size() || der[p] != want) return false;
  ++p;
  size_t len = der[p++];
  if (len & 0x80) {
    size_t n = len & 0x7f;
    if (n == 0 || n > 4 || p + n > der.size()) return false;
    len = 0;
    for (size_t i = 0; i < n; ++i) len = (len << 8) | der[p++];
  }
  if (p + len > der.size()) return false;
  body = der.subspan(p, len);
  p += len;
  return true;
}

void der_length(ByteBuffer& out, size_t len) {
  if (len < 0x80) { out.push_back((uint8_t)len); return; }
  if (len < 0x100) { out.push_back(0x81); out.push_back((uint8_t)len); return; }
  out.push_back(0x82);
  out.push_back((uint8_t)(len >> 8));
  out.push_back((uint8_t)len);
}

ByteBuffer encode_der_integer(const Num& n, size_t max_bytes) {
  ByteBuffer raw_bytes(max_bytes);
  to_bytes(n, raw_bytes.data(), max_bytes);
  size_t start = 0;
  while (start + 1 < raw_bytes.size() && raw_bytes[start] == 0) ++start;
  ByteBuffer int_bytes;
  if (raw_bytes[start] & 0x80) int_bytes.push_back(0x00);
  int_bytes.insert(int_bytes.end(), raw_bytes.begin() + (long)start, raw_bytes.end());

  ByteBuffer out;
  out.push_back(0x02);
  der_length(out, int_bytes.size());
  out.insert(out.end(), int_bytes.begin(), int_bytes.end());
  return out;
}

// The bit length of the group order: 256, 384 or 521.
int order_bits(const Curve& c) { return top_bit(c.n) + 1; }

// RFC 6979 bits2int: the leftmost order_bits bits of a hash (or of the nonce generator's output), as an integer.
// A digest shorter than the order is taken whole; never reduced bit-misaligned by whole bytes.
Num bits2int(ByteSpan b, const Curve& c) {
  Num v = from_bytes(b.size() > (size_t)kLimbs * 8 ? b.subspan(0, (size_t)kLimbs * 8) : b);
  int excess = (int)(b.size() * 8) - order_bits(c);
  if (excess > 0) {
    // shift right by `excess` bits
    int limbs = excess / 64, bits = excess % 64;
    for (int i = 0; i < kLimbs; ++i) {
      uint64_t lo = i + limbs < kLimbs ? v.v[i + limbs] : 0;
      uint64_t hi = i + limbs + 1 < kLimbs ? v.v[i + limbs + 1] : 0;
      v.v[i] = bits ? (lo >> bits) | (hi << (64 - bits)) : lo;
    }
  }
  return v;
}

// The hash RFC 6979 runs HMAC over: the one each curve's signatures are made with in SSH and TLS.
struct Hmac {
  NistCurve curve;
  size_t out_len() const { return curve == NistCurve::P256 ? 32 : curve == NistCurve::P384 ? 48 : 64; }
  size_t block() const { return curve == NistCurve::P256 ? 64 : 128; }
  ByteBuffer hash(const ByteBuffer& m) const {
    ByteSpan s(m.data(), m.size());
    if (curve == NistCurve::P256) { auto h = sha256(s); return ByteBuffer(h.begin(), h.end()); }
    if (curve == NistCurve::P384) { auto h = sha384(s); return ByteBuffer(h.begin(), h.end()); }
    auto h = sha512(s);
    return ByteBuffer(h.begin(), h.end());
  }
  ByteBuffer mac(const ByteBuffer& key, const ByteBuffer& msg) const {
    ByteBuffer k = key.size() > block() ? hash(key) : key;
    k.resize(block(), 0);
    ByteBuffer inner(block()), outer(block());
    for (size_t i = 0; i < block(); ++i) { inner[i] = k[i] ^ 0x36; outer[i] = k[i] ^ 0x5c; }
    inner.insert(inner.end(), msg.begin(), msg.end());
    ByteBuffer ih = hash(inner);
    outer.insert(outer.end(), ih.begin(), ih.end());
    return hash(outer);
  }
};

// RFC 6979 section 3.2: k from the private key and the message hash alone, so a signature never depends on a
// random source that could repeat or be predicted (either one hands out the private key).
Num rfc6979_nonce(const Curve& c, NistCurve which, const Num& x, ByteSpan digest) {
  const Hmac h{which};
  const size_t rlen = c.bytes;
  ByteBuffer xo(rlen), ho(rlen);
  to_bytes(x, xo.data(), rlen);
  Num h1 = mod_reduce(bits2int(digest, c), c.n);
  to_bytes(h1, ho.data(), rlen);
  ByteBuffer V(h.out_len(), 0x01), K(h.out_len(), 0x00);
  auto seeded = [&](uint8_t sep) {
    ByteBuffer m = V;
    m.push_back(sep);
    m.insert(m.end(), xo.begin(), xo.end());
    m.insert(m.end(), ho.begin(), ho.end());
    return m;
  };
  K = h.mac(K, seeded(0x00));
  V = h.mac(K, V);
  K = h.mac(K, seeded(0x01));
  V = h.mac(K, V);
  const size_t qlen = (size_t)order_bits(c);
  for (;;) {
    ByteBuffer T;
    while (T.size() * 8 < qlen) {
      V = h.mac(K, V);
      T.insert(T.end(), V.begin(), V.end());
    }
    Num k = bits2int(ByteSpan(T.data(), T.size()), c);
    if (!is_zero(k) && cmp(k, c.n) < 0) return k;
    ByteBuffer m = V;
    m.push_back(0x00);
    K = h.mac(K, m);
    V = h.mac(K, V);
  }
}

}  // namespace

bool EcPrivateKey::valid() const {
  if (d.empty()) return false;
  const Curve& c = curve_of(curve);
  Num num_d = from_bytes(d);
  return !is_zero(num_d) && cmp(num_d, c.n) < 0;
}

Result<EcPrivateKey> ec_parse_private_key(ByteSpan der) {
  size_t p = 0;
  ByteSpan seq;
  if (!asn1_tlv(der, p, 0x30, seq)) return Result<EcPrivateKey>(Status::Corrupt, "not an EC private key sequence");

  size_t sp = 0;
  ByteSpan v_elem;
  if (!asn1_tlv(seq, sp, 0x02, v_elem)) return Result<EcPrivateKey>(Status::Corrupt, "missing version in sequence");

  // Check if PKCS#8 PrivateKeyInfo: version 0, AlgorithmIdentifier sequence, OCTET STRING
  ByteSpan next_elem;
  size_t check_p = sp;
  if (asn1_tlv(seq, check_p, 0x30, next_elem)) {
    // Check parameters in AlgorithmIdentifier for curve OID
    NistCurve curve = NistCurve::P256;
    size_t aid_p = 0;
    ByteSpan alg_oid;
    if (asn1_tlv(next_elem, aid_p, 0x06, alg_oid)) {
      ByteSpan param_oid;
      if (asn1_tlv(next_elem, aid_p, 0x06, param_oid)) {
        nist_curve_from_oid(param_oid, curve);
      }
    }
    ByteSpan octets;
    if (!asn1_tlv(seq, check_p, 0x04, octets))
      return Result<EcPrivateKey>(Status::Corrupt, "PKCS#8 missing EC private key octet string");
    auto res = ec_parse_private_key(octets);
    if (res.ok()) res->curve = curve;
    return res;
  }

  // SEC1 ECPrivateKey: version 1, OCTET STRING privateKey, [0] parameters OPTIONAL, [1] publicKey OPTIONAL
  ByteSpan priv_octets;
  if (!asn1_tlv(seq, sp, 0x04, priv_octets))
    return Result<EcPrivateKey>(Status::Corrupt, "missing EC private key octet string");

  EcPrivateKey key;
  key.d.assign(priv_octets.begin(), priv_octets.end());
  key.curve = key.d.size() > 48 ? NistCurve::P521 : key.d.size() > 32 ? NistCurve::P384 : NistCurve::P256;

  // Check optional tagged parameters [0] = 0xA0
  ByteSpan tag0;
  if (asn1_tlv(seq, sp, 0xA0, tag0)) {
    size_t tag0_p = 0;
    ByteSpan curve_oid;
    if (asn1_tlv(tag0, tag0_p, 0x06, curve_oid)) {
      nist_curve_from_oid(curve_oid, key.curve);
    }
  }

  if (!key.valid()) return Result<EcPrivateKey>(Status::Invalid, "invalid EC private key scalar");
  return key;
}

Result<ByteBuffer> ecdsa_public_key(NistCurve which, ByteSpan private_key_scalar) {
  const Curve& c = curve_of(which);
  Num d = from_bytes(private_key_scalar);
  if (is_zero(d) || cmp(d, c.n) >= 0) return Result<ByteBuffer>(Status::Invalid, "invalid private key scalar");

  Point G;
  G.x = c.gx; G.y = c.gy; G.z = Num(); G.z.v[0] = 1;
  Point Q = scalar_mult(d, G, c);

  Num qx, qy;
  if (!affine_point(Q, c, qx, qy)) return Result<ByteBuffer>(Status::Invalid, "point at infinity");

  ByteBuffer pub;
  pub.reserve(1 + 2 * c.bytes);
  pub.push_back(0x04);
  ByteBuffer xb(c.bytes, 0), yb(c.bytes, 0);
  to_bytes(qx, xb.data(), c.bytes);
  to_bytes(qy, yb.data(), c.bytes);
  pub.insert(pub.end(), xb.begin(), xb.end());
  pub.insert(pub.end(), yb.begin(), yb.end());
  return pub;
}

const char* nist_curve_name(NistCurve c) {
  switch (c) {
    case NistCurve::P256: return "P-256";
    case NistCurve::P384: return "P-384";
    case NistCurve::P521: return "P-521";
  }
  return "P-256";
}

Result<ByteBuffer> ecdh_shared_x(NistCurve which, ByteSpan private_key_scalar, ByteSpan peer_pub) {
  const Curve& c = curve_of(which);
  Num d = from_bytes(private_key_scalar);
  if (is_zero(d) || cmp(d, c.n) >= 0) return Result<ByteBuffer>(Status::Invalid, "invalid private key scalar");
  Num px, py;
  // The peer's point must be on the curve, or the "shared" secret leaks d (invalid-curve attack).
  if (!split_pub(which, peer_pub, px, py) || !on_curve(px, py, c))
    return Result<ByteBuffer>(Status::Invalid, "the peer's key share is not a point on the curve");
  Point Q;
  Q.x = px; Q.y = py; Q.z = Num(); Q.z.v[0] = 1;
  Num sx;
  if (!affine_x(scalar_mult(d, Q, c), c, sx)) return Result<ByteBuffer>(Status::Invalid, "shared point at infinity");
  ByteBuffer out(c.bytes, 0);
  to_bytes(sx, out.data(), c.bytes);
  return out;
}

bool nist_curve_from_oid(ByteSpan oid, NistCurve& out) {
  // prime256v1 = 1.2.840.10045.3.1.7, secp384r1 = 1.3.132.0.34, secp521r1 = 1.3.132.0.35
  static const uint8_t p256[] = {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07};
  static const uint8_t p384[] = {0x2b, 0x81, 0x04, 0x00, 0x22};
  static const uint8_t p521[] = {0x2b, 0x81, 0x04, 0x00, 0x23};
  if (oid.size() == sizeof(p256) && std::memcmp(oid.data(), p256, sizeof(p256)) == 0) { out = NistCurve::P256; return true; }
  if (oid.size() == sizeof(p384) && std::memcmp(oid.data(), p384, sizeof(p384)) == 0) { out = NistCurve::P384; return true; }
  if (oid.size() == sizeof(p521) && std::memcmp(oid.data(), p521, sizeof(p521)) == 0) { out = NistCurve::P521; return true; }
  return false;
}

bool nist_curve_from_params(ByteSpan prime, ByteSpan b, ByteSpan base_point, ByteSpan order, NistCurve& out) {
  for (NistCurve which : {NistCurve::P256, NistCurve::P384, NistCurve::P521}) {
    const Curve& c = curve_of(which);
    Num gx, gy;
    if (cmp(from_bytes(prime), c.p) != 0 || cmp(from_bytes(b), c.b) != 0 || cmp(from_bytes(order), c.n) != 0) continue;
    if (!split_pub(which, base_point, gx, gy) || cmp(gx, c.gx) != 0 || cmp(gy, c.gy) != 0) continue;
    out = which;
    return true;
  }
  return false;
}

bool nist_point_valid(NistCurve which, ByteSpan pub) {
  Num x, y;
  if (!split_pub(which, pub, x, y)) return false;
  const Curve& c = curve_of(which);
  if (cmp(x, c.p) >= 0 || cmp(y, c.p) >= 0) return false;
  return on_curve(x, y, c);
}

bool ecdsa_verify(NistCurve which, ByteSpan pub, ByteSpan digest, ByteSpan sig_r, ByteSpan sig_s) {
  const Curve& c = curve_of(which);
  Num qx, qy;
  if (!split_pub(which, pub, qx, qy)) return false;
  if (!on_curve(qx, qy, c)) return false;

  Num r = from_bytes(sig_r), s = from_bytes(sig_s);
  if (is_zero(r) || is_zero(s) || cmp(r, c.n) >= 0 || cmp(s, c.n) >= 0) return false;

  // The digest is taken leftmost-bits-first: a hash longer than the order is
  // truncated, never wrapped.
  Num e = mod_reduce(bits2int(digest, c), c.n);

  Num w = mod_inv(s, c.n);
  Num u1 = mod_mul(e, w, c.n);
  Num u2 = mod_mul(r, w, c.n);

  Point G;
  G.x = c.gx; G.y = c.gy; G.z = Num(); G.z.v[0] = 1;
  Point Q;
  Q.x = qx; Q.y = qy; Q.z = Num(); Q.z.v[0] = 1;

  Point sum = add_points(scalar_mult(u1, G, c), scalar_mult(u2, Q, c), c);
  Num x;
  if (!affine_x(sum, c, x)) return false;
  x = mod_reduce(x, c.n);
  return cmp(x, r) == 0;
}

Result<ByteBuffer> ecdsa_sign(NistCurve which, ByteSpan private_key_scalar, ByteSpan digest, ByteSpan k_nonce) {
  const Curve& c = curve_of(which);
  Num d = from_bytes(private_key_scalar);
  if (is_zero(d) || cmp(d, c.n) >= 0) return Result<ByteBuffer>(Status::Invalid, "invalid private key scalar");
  Num e = mod_reduce(bits2int(digest, c), c.n);

  Point G;
  G.x = c.gx; G.y = c.gy; G.z = Num(); G.z.v[0] = 1;

  Num k;
  if (!k_nonce.empty()) {
    k = from_bytes(k_nonce);
    if (is_zero(k) || cmp(k, c.n) >= 0) return Result<ByteBuffer>(Status::Invalid, "invalid k nonce");
  } else {
    k = rfc6979_nonce(c, which, d, digest);
  }

  Point R = scalar_mult(k, G, c);
  Num rx;
  // RFC 6979's k for this key and digest is in [1, n-1], so neither r nor s can come out zero except with
  // probability ~2^-256; a caller's own k that does is refused rather than retried with another.
  if (!affine_x(R, c, rx)) return Result<ByteBuffer>(Status::Invalid, "k scalar yielded point at infinity");
  Num r = mod_reduce(rx, c.n);
  if (is_zero(r)) return Result<ByteBuffer>(Status::Invalid, "r is zero");
  Num kinv = mod_inv(k, c.n);
  Num s = mod_mul(kinv, mod_add(e, mod_mul(r, d, c.n), c.n), c.n);
  if (is_zero(s)) return Result<ByteBuffer>(Status::Invalid, "s is zero");

  // DER SEQUENCE { INTEGER r, INTEGER s }
  ByteBuffer r_der = encode_der_integer(r, c.bytes);
  ByteBuffer s_der = encode_der_integer(s, c.bytes);
  ByteBuffer sig;
  sig.push_back(0x30);
  der_length(sig, r_der.size() + s_der.size());
  sig.insert(sig.end(), r_der.begin(), r_der.end());
  sig.insert(sig.end(), s_der.begin(), s_der.end());
  return sig;
}

}  // namespace aios::crypto
