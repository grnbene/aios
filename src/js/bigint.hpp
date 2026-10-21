#pragma once
// Arbitrary-precision integer arithmetic for the JavaScript BigInt type. Pure magnitude/sign math on BigNum (js.hpp):
// no heap, no Context, no exceptions, no libm. The VM and the builtins wrap these in the spec's operations.
#include <string>
#include <string_view>

#include "aios/js/js.hpp"

namespace aios::js::big {

// A BigInt this large is refused with a RangeError instead of exhausting memory (8 MiB of magnitude).
constexpr uint64_t kMaxBits = 1ull << 26;

enum class Err : uint8_t { None, DivideByZero, TooBig, NegativeExponent };

BigNum from_i64(int64_t v);
BigNum from_u64(uint64_t v);
// `d` must be a finite integral value.
BigNum from_double(double d);
// Correctly rounded (nearest, ties to even); +-Infinity once the value exceeds the largest double.
double to_double(const BigNum& a);
// The low 64 bits of the two's complement representation: BigInt.asUintN(64, a) / BigInt.asIntN(64, a).
uint64_t to_u64_wrap(const BigNum& a);
inline int64_t to_i64_wrap(const BigNum& a) { return static_cast<int64_t>(to_u64_wrap(a)); }

uint64_t bit_length(const BigNum& a);          // of the magnitude; 0 for zero
bool is_odd(const BigNum& a);

int cmp(const BigNum& a, const BigNum& b);     // -1, 0, 1
int cmp_double(const BigNum& a, double d);     // -1, 0, 1, or 2 when d is NaN

BigNum negate(const BigNum& a);
BigNum add(const BigNum& a, const BigNum& b);
BigNum sub(const BigNum& a, const BigNum& b);
BigNum mul(const BigNum& a, const BigNum& b);
Err divmod(const BigNum& a, const BigNum& b, BigNum* q, BigNum* r);   // truncating; r takes the sign of a
Err pow(const BigNum& a, const BigNum& e, BigNum* out);

BigNum bit_and(const BigNum& a, const BigNum& b);
BigNum bit_or(const BigNum& a, const BigNum& b);
BigNum bit_xor(const BigNum& a, const BigNum& b);
BigNum bit_not(const BigNum& a);
Err shift_left(const BigNum& a, const BigNum& count, BigNum* out);    // a negative count shifts right
Err shift_right(const BigNum& a, const BigNum& count, BigNum* out);   // arithmetic: rounds toward -Infinity

Err as_int_n(uint64_t bits, const BigNum& a, BigNum* out);
Err as_uint_n(uint64_t bits, const BigNum& a, BigNum* out);

// radix 2..36, lower-case digits, a leading '-' for negatives.
std::string to_string(const BigNum& a, int radix = 10);
// Digits only (no sign, no prefix, no separators) in `radix`; false if any character is not a digit of it or there are none.
bool parse_digits(std::u16string_view digits, int radix, BigNum* out);
// StringToBigInt: surrounding whitespace, "" is 0n, an optional sign for decimal, or a 0x/0o/0b prefix. False: not a valid integer.
bool string_to_bigint(std::u16string_view s, BigNum* out);

// Hash of the value, consistent with cmp()==0.
size_t hash(const BigNum& a);

}  // namespace aios::js::big
