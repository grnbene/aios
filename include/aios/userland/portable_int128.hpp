#pragma once
// A portable drop-in for signed __int128 on MSVC, which has none: the exact-decimal arithmetic in
// sql_database.cpp, sql_things.cpp and sql_cells.cpp (the ERP "Things" type, SUM/AVG scaling, Cells)
// needs real 128-bit multiply, divide and modulo, not just storage.
//
// Built on MSVC's own internal __msvc_int128.hpp (the std::_Signed128 its <charconv> big-integer
// formatting already uses — correct, maintained, and backed by the _udiv128 intrinsic) rather than a
// hand-rolled bignum: 128-bit division is exactly the kind of numeric code that is easy to get subtly
// wrong, and this reuses the compiler's own.
#if defined(_MSC_VER) && !defined(__clang__)
#include <__msvc_int128.hpp>

using AIOS_INT128 = std::_Signed128;
using AIOS_UINT128 = std::_Unsigned128;

// GCC/Clang builtins MSVC has none of: same (a, b, *out) -> did it overflow? contract as the real
// ones, from the wrapping (modular) +/* _Signed128 already does and one division-based identity
// (r / b != a after a wrapped multiply) — the same check GCC's own generic mul-overflow fallback uses.
inline bool __builtin_add_overflow(AIOS_INT128 a, AIOS_INT128 b, AIOS_INT128* out) {
  AIOS_INT128 r = a + b;
  *out = r;
  return b >= AIOS_INT128(0) ? r < a : r > a;
}
inline bool __builtin_mul_overflow(AIOS_INT128 a, AIOS_INT128 b, AIOS_INT128* out) {
  AIOS_INT128 r = a * b;
  *out = r;
  return b != AIOS_INT128(0) && (r / b) != a;
}

// __int128 converts a float/double/long double by truncation directly; _Signed128's constructor is
// integral-only. A SQL decimal's magnitude after its *1e8 scientific-notation scale-up (parse_dec in
// sql_cells.cpp) never approaches even int64's range in practice, so round through that instead of a
// bit-exact (and, for MSVC's long double, which is just double, unnecessary) 128-bit float conversion.
inline AIOS_INT128 aios_i128_from_ld(long double x) { return AIOS_INT128(static_cast<long long>(x)); }
#else
// __extension__: GCC's -Wpedantic reports every __int128 otherwise.
__extension__ typedef __int128 AIOS_INT128;
__extension__ typedef unsigned __int128 AIOS_UINT128;
inline AIOS_INT128 aios_i128_from_ld(long double x) { return (AIOS_INT128)x; }
#endif
