#pragma once
#include <cstdint>
#include <cstddef>
#include "aios/crypto/portable_uint128.hpp"

// Internal arithmetic for the secp256k1 curve. Not a public header: everything
// above talks to include/aios/crypto/secp256k1.hpp.
//
// Field elements are four 64-bit limbs, little-endian, and are fully reduced
// (< p) after every operation. The lazy-magnitude representations that faster
// implementations use are deliberately absent: an unreduced limb that is one
// carry too large produces a wrong signature roughly once in a very large
// number of operations, which is the worst failure mode available here.

namespace aios::crypto::k1 {

#if defined(_MSC_VER) && !defined(__clang__)
typedef aios::crypto::portable::U128 u128;
#else
__extension__ typedef unsigned __int128 u128;
#endif

struct Fe { uint64_t n[4]; };
struct Scalar { uint64_t n[4]; };

struct Gej { Fe x, y, z; };   // jacobian; z == 0 is the point at infinity
struct Ge { Fe x, y; bool inf; };

void fe_zero(Fe& r);
void fe_set_u64(Fe& r, uint64_t v);
bool fe_set_b32(Fe& r, const uint8_t b[32]);
void fe_get_b32(uint8_t b[32], const Fe& a);
void fe_add(Fe& r, const Fe& a, const Fe& b);
void fe_sub(Fe& r, const Fe& a, const Fe& b);
void fe_neg(Fe& r, const Fe& a);
void fe_mul(Fe& r, const Fe& a, const Fe& b);
void fe_sqr(Fe& r, const Fe& a);
void fe_inv(Fe& r, const Fe& a);
bool fe_sqrt(Fe& r, const Fe& a);
bool fe_is_zero(const Fe& a);
bool fe_is_odd(const Fe& a);
bool fe_equal(const Fe& a, const Fe& b);
void fe_cmov(Fe& r, const Fe& a, uint64_t mask);

void sc_zero(Scalar& r);
void sc_set_u64(Scalar& r, uint64_t v);
bool sc_set_b32(Scalar& r, const uint8_t b[32]);   // false when the input was >= n (value is reduced)
void sc_get_b32(uint8_t b[32], const Scalar& a);
void sc_add(Scalar& r, const Scalar& a, const Scalar& b);
void sc_mul(Scalar& r, const Scalar& a, const Scalar& b);
void sc_neg(Scalar& r, const Scalar& a);
void sc_inv(Scalar& r, const Scalar& a);
bool sc_is_zero(const Scalar& a);
bool sc_is_high(const Scalar& a);
bool sc_equal(const Scalar& a, const Scalar& b);
void sc_reduce_b64(Scalar& r, const uint8_t b[64]);
int sc_bit(const Scalar& a, unsigned i);
void sc_cmov(Scalar& r, const Scalar& a, uint64_t mask);

// The x coordinate of a curve point, reduced into the scalar field, plus a
// flag saying whether that reduction wrapped. ECDSA recovery needs the flag.
void fe_to_scalar(Scalar& r, const Fe& a, bool& overflowed);

void gej_set_infinity(Gej& r);
bool gej_is_infinity(const Gej& a);
void gej_set_ge(Gej& r, const Ge& a);
void gej_double(Gej& r, const Gej& a);
void gej_add(Gej& r, const Gej& a, const Gej& b);
void gej_add_ge_ct(Gej& r, const Gej& a, const Ge& b);
void gej_neg(Gej& r, const Gej& a);
void gej_to_ge(Ge& r, const Gej& a);

void ge_set_infinity(Ge& r);
bool ge_is_valid(const Ge& a);
bool ge_set_x_parity(Ge& r, const Fe& x, bool odd_y);
void ge_cmov(Ge& r, const Ge& a, uint64_t mask);

// k*G in constant time, through a precomputed comb over the generator.
void ecmul_gen(Gej& r, const Scalar& k);
// a*A + b*G with no timing guarantee; only ever called on public values.
void ecmul_var(Gej& r, const Ge& point, const Scalar& a, const Scalar& b);

const Ge& generator();

}  // namespace aios::crypto::k1
