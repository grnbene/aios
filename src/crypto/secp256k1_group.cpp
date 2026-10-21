#include "secp256k1_math.hpp"

namespace aios::crypto::k1 {

namespace {

const uint8_t GX[32] = {0x79, 0xBE, 0x66, 0x7E, 0xF9, 0xDC, 0xBB, 0xAC, 0x55, 0xA0, 0x62,
                        0x95, 0xCE, 0x87, 0x0B, 0x07, 0x02, 0x9B, 0xFC, 0xDB, 0x2D, 0xCE,
                        0x28, 0xD9, 0x59, 0xF2, 0x81, 0x5B, 0x16, 0xF8, 0x17, 0x98};
const uint8_t GY[32] = {0x48, 0x3A, 0xDA, 0x77, 0x26, 0xA3, 0xC4, 0x65, 0x5D, 0xA4, 0xFB,
                        0xFC, 0x0E, 0x11, 0x08, 0xA8, 0xFD, 0x17, 0xB4, 0x48, 0xA6, 0x85,
                        0x54, 0x19, 0x9C, 0x47, 0xD0, 0x8F, 0xFB, 0x10, 0xD4, 0xB8};

struct GeneratorHolder {
  Ge g;
  GeneratorHolder() {
    fe_set_b32(g.x, GX);
    fe_set_b32(g.y, GY);
    g.inf = false;
  }
};
const GeneratorHolder g_generator;

uint64_t mask_of(bool b) { return (uint64_t)0 - (uint64_t)b; }

void fe_double(Fe& r, const Fe& a) { fe_add(r, a, a); }

void fe_triple(Fe& r, const Fe& a) {
  Fe two;
  fe_add(two, a, a);
  fe_add(r, two, a);
}

}  // namespace

const Ge& generator() { return g_generator.g; }

void gej_set_infinity(Gej& r) {
  fe_set_u64(r.x, 1);
  fe_set_u64(r.y, 1);
  fe_zero(r.z);
}

bool gej_is_infinity(const Gej& a) { return fe_is_zero(a.z); }

void gej_set_ge(Gej& r, const Ge& a) {
  r.x = a.x;
  r.y = a.y;
  if (a.inf) {
    fe_zero(r.z);
  } else {
    fe_set_u64(r.z, 1);
  }
}

void ge_set_infinity(Ge& r) {
  fe_zero(r.x);
  fe_zero(r.y);
  r.inf = true;
}

void gej_neg(Gej& r, const Gej& a) {
  r.x = a.x;
  fe_neg(r.y, a.y);
  r.z = a.z;
}

void gej_double(Gej& r, const Gej& a) {
  Fe A, B, C, D, E, F, t, u;
  fe_sqr(A, a.x);
  fe_sqr(B, a.y);
  fe_sqr(C, B);
  fe_add(t, a.x, B);
  fe_sqr(t, t);
  fe_sub(t, t, A);
  fe_sub(t, t, C);
  fe_double(D, t);
  fe_triple(E, A);
  fe_sqr(F, E);

  Fe x3, y3, z3;
  fe_double(u, D);
  fe_sub(x3, F, u);
  fe_sub(t, D, x3);
  fe_mul(t, E, t);
  fe_double(u, C);
  fe_double(u, u);
  fe_double(u, u);
  fe_sub(y3, t, u);
  fe_mul(z3, a.y, a.z);
  fe_double(z3, z3);

  r.x = x3;
  r.y = y3;
  r.z = z3;
}

void gej_add(Gej& r, const Gej& a, const Gej& b) {
  if (gej_is_infinity(a)) { r = b; return; }
  if (gej_is_infinity(b)) { r = a; return; }

  Fe z1z1, z2z2, u1, u2, s1, s2, h, i, j, rr, v, t;
  fe_sqr(z1z1, a.z);
  fe_sqr(z2z2, b.z);
  fe_mul(u1, a.x, z2z2);
  fe_mul(u2, b.x, z1z1);
  fe_mul(s1, a.y, b.z);
  fe_mul(s1, s1, z2z2);
  fe_mul(s2, b.y, a.z);
  fe_mul(s2, s2, z1z1);
  fe_sub(h, u2, u1);
  fe_sub(rr, s2, s1);

  if (fe_is_zero(h)) {
    if (fe_is_zero(rr)) { gej_double(r, a); return; }
    gej_set_infinity(r);
    return;
  }

  fe_double(rr, rr);
  fe_double(i, h);
  fe_sqr(i, i);
  fe_mul(j, h, i);
  fe_mul(v, u1, i);

  Fe x3, y3, z3;
  fe_sqr(x3, rr);
  fe_sub(x3, x3, j);
  fe_double(t, v);
  fe_sub(x3, x3, t);
  fe_sub(t, v, x3);
  fe_mul(t, rr, t);
  fe_mul(u2, s1, j);
  fe_double(u2, u2);
  fe_sub(y3, t, u2);
  fe_add(z3, a.z, b.z);
  fe_sqr(z3, z3);
  fe_sub(z3, z3, z1z1);
  fe_sub(z3, z3, z2z2);
  fe_mul(z3, z3, h);

  r.x = x3;
  r.y = y3;
  r.z = z3;
}

// The branch-free counterpart of gej_add for a z == 1 operand. Every case is
// evaluated and the answer selected by mask, so the running time carries no
// information about the scalar bit that chose the operand.
void gej_add_ge_ct(Gej& r, const Gej& a, const Ge& b) {
  Fe z1z1, u2, s2, h, rr, i, j, v, t, u;
  fe_sqr(z1z1, a.z);
  fe_mul(u2, b.x, z1z1);
  fe_mul(s2, b.y, a.z);
  fe_mul(s2, s2, z1z1);
  fe_sub(h, u2, a.x);
  fe_sub(rr, s2, a.y);

  Fe x3, y3, z3;
  Fe rr2;
  fe_double(rr2, rr);
  fe_double(i, h);
  fe_sqr(i, i);
  fe_mul(j, h, i);
  fe_mul(v, a.x, i);
  fe_sqr(x3, rr2);
  fe_sub(x3, x3, j);
  fe_double(t, v);
  fe_sub(x3, x3, t);
  fe_sub(t, v, x3);
  fe_mul(t, rr2, t);
  fe_mul(u, a.y, j);
  fe_double(u, u);
  fe_sub(y3, t, u);
  fe_mul(z3, a.z, h);
  fe_double(z3, z3);

  Gej doubled;
  gej_double(doubled, a);

  Gej bj;
  gej_set_ge(bj, b);

  bool a_inf = gej_is_infinity(a);
  bool h_zero = fe_is_zero(h);
  bool rr_zero = fe_is_zero(rr);

  uint64_t m_double = mask_of(!a_inf && !b.inf && h_zero && rr_zero);
  uint64_t m_opposite = mask_of(!a_inf && !b.inf && h_zero && !rr_zero);
  uint64_t m_take_b = mask_of(a_inf && !b.inf);
  uint64_t m_take_a = mask_of(b.inf);

  Gej inf;
  gej_set_infinity(inf);

  Gej out;
  out.x = x3;
  out.y = y3;
  out.z = z3;
  fe_cmov(out.x, doubled.x, m_double);
  fe_cmov(out.y, doubled.y, m_double);
  fe_cmov(out.z, doubled.z, m_double);
  fe_cmov(out.x, inf.x, m_opposite);
  fe_cmov(out.y, inf.y, m_opposite);
  fe_cmov(out.z, inf.z, m_opposite);
  fe_cmov(out.x, bj.x, m_take_b);
  fe_cmov(out.y, bj.y, m_take_b);
  fe_cmov(out.z, bj.z, m_take_b);
  fe_cmov(out.x, a.x, m_take_a);
  fe_cmov(out.y, a.y, m_take_a);
  fe_cmov(out.z, a.z, m_take_a);
  r = out;
}

void gej_to_ge(Ge& r, const Gej& a) {
  if (gej_is_infinity(a)) { ge_set_infinity(r); return; }
  Fe zinv, zinv2, zinv3;
  fe_inv(zinv, a.z);
  fe_sqr(zinv2, zinv);
  fe_mul(zinv3, zinv2, zinv);
  fe_mul(r.x, a.x, zinv2);
  fe_mul(r.y, a.y, zinv3);
  r.inf = false;
}

bool ge_is_valid(const Ge& a) {
  if (a.inf) return false;
  Fe y2, x3, seven;
  fe_sqr(y2, a.y);
  fe_sqr(x3, a.x);
  fe_mul(x3, x3, a.x);
  fe_set_u64(seven, 7);
  fe_add(x3, x3, seven);
  return fe_equal(y2, x3);
}

bool ge_set_x_parity(Ge& r, const Fe& x, bool odd_y) {
  Fe x3, seven, y;
  fe_sqr(x3, x);
  fe_mul(x3, x3, x);
  fe_set_u64(seven, 7);
  fe_add(x3, x3, seven);
  if (!fe_sqrt(y, x3)) return false;
  if (fe_is_odd(y) != odd_y) fe_neg(y, y);
  r.x = x;
  r.y = y;
  r.inf = false;
  return true;
}

void ge_cmov(Ge& r, const Ge& a, uint64_t mask) {
  fe_cmov(r.x, a.x, mask);
  fe_cmov(r.y, a.y, mask);
  r.inf = ((mask & (uint64_t)a.inf) | (~mask & (uint64_t)r.inf)) != 0;
}

void ecmul_gen(Gej& r, const Scalar& k) {
  Gej acc;
  gej_set_infinity(acc);
  const Ge& g = generator();
  Ge zero;
  ge_set_infinity(zero);
  for (int i = 255; i >= 0; --i) {
    gej_double(acc, acc);
    Ge addend = zero;
    ge_cmov(addend, g, mask_of(sc_bit(k, (unsigned)i) != 0));
    Gej sum;
    gej_add_ge_ct(sum, acc, addend);
    acc = sum;
  }
  r = acc;
}

void ecmul_var(Gej& r, const Ge& point, const Scalar& a, const Scalar& b) {
  Gej acc;
  gej_set_infinity(acc);
  Gej pj;
  gej_set_ge(pj, point);
  const Ge& g = generator();
  Gej gj;
  gej_set_ge(gj, g);

  for (int i = 255; i >= 0; --i) {
    gej_double(acc, acc);
    if (sc_bit(a, (unsigned)i)) gej_add(acc, acc, pj);
    if (sc_bit(b, (unsigned)i)) gej_add(acc, acc, gj);
  }
  r = acc;
}

}  // namespace aios::crypto::k1
