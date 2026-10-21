#pragma once
// A minimal unsigned 128-bit integer for MSVC, which has no __int128. Built
// only for what curve25519.cpp / nistp.cpp / rsa.cpp actually do with
// `unsigned __int128`: widen a 64-bit multiply, carry an add or a subtract
// across 64-bit limbs, and split the result back into 64-bit words. Each
// operator is one compiler intrinsic with a documented, exact contract
// (_umul128, _addcarry_u64, _subborrow_u64) — this is a spelling change for
// those three files, not a reimplementation of bignum arithmetic.
//
// operator* is the full, general 128x128->128 truncating multiply (the same
// modular-2^128 semantics real `unsigned __int128` has), not a shortcut that
// assumes one side is small — even though every call site here happens to
// multiply two values that fit in 64 bits, a crypto primitive should not
// depend on a caller never proving that assumption wrong.
#if defined(_MSC_VER) && !defined(__clang__)
#include <cstdint>
#include <intrin.h>

namespace aios::crypto::portable {

struct U128 {
  uint64_t lo = 0, hi = 0;
  U128() = default;
  U128(uint64_t v) : lo(v), hi(0) {}           // NOLINT: implicit, like widening a uint64_t to __int128
  U128(int v) : lo((uint64_t)(int64_t)v), hi(0) {}  // NOLINT: only ever called here with small non-negative literals (19, 1)

  explicit operator uint64_t() const { return lo; }
  explicit operator bool() const { return lo != 0 || hi != 0; }

  friend U128 operator+(U128 a, U128 b) {
    U128 r;
    const unsigned char c = _addcarry_u64(0, a.lo, b.lo, &r.lo);
    _addcarry_u64(c, a.hi, b.hi, &r.hi);
    return r;
  }
  U128& operator+=(U128 b) { *this = *this + b; return *this; }

  friend U128 operator-(U128 a, U128 b) {
    U128 r;
    const unsigned char c = _subborrow_u64(0, a.lo, b.lo, &r.lo);
    _subborrow_u64(c, a.hi, b.hi, &r.hi);
    return r;
  }

  friend U128 operator*(U128 a, U128 b) {
    // lo-by-lo is a true 128-bit product (_umul128); the cross terms
    // (a.lo*b.hi, a.hi*b.lo) land entirely in the high word, where plain
    // 64-bit wraparound multiplication already keeps only the bits that
    // belong there; a.hi*b.hi would land at bit 128 or above and is dropped,
    // same as real unsigned __int128 multiplication truncates it.
    U128 r;
    uint64_t hi_of_lolo;
    r.lo = _umul128(a.lo, b.lo, &hi_of_lolo);
    r.hi = hi_of_lolo + a.lo * b.hi + a.hi * b.lo;
    return r;
  }

  friend U128 operator>>(U128 a, int n) {
    U128 r;
    if (n <= 0) return a;
    if (n < 64) { r.lo = (a.lo >> n) | (n == 0 ? 0 : a.hi << (64 - n)); r.hi = a.hi >> n; }
    else if (n < 128) { r.lo = a.hi >> (n - 64); r.hi = 0; }
    return r;
  }
  U128& operator>>=(int n) { *this = *this >> n; return *this; }

  // secp256k1's carry/borrow extraction (`(t >> 64) & 1`): the mask is always
  // a small non-negative literal, entirely within the low 64 bits.
  friend U128 operator&(U128 a, int mask) {
    U128 r;
    r.lo = a.lo & (uint64_t)(int64_t)mask;
    return r;
  }
};

}  // namespace aios::crypto::portable
#endif  // defined(_MSC_VER) && !defined(__clang__)
