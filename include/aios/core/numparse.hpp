#pragma once
// Numbers read from text without the C library: the core does not call strtol and its family (the zero-host gate),
// and these need nothing from a host. Integers follow strtoll's grammar - leading blanks, a sign, base 0 or 16 taking
// "0x", base 0 taking a leading 0 as octal - and stop at the first character that is not a digit of the base; an
// overflow clamps to the type's limit, as strtoll does. A double is the host's to read exactly (hal::parse_double).
#include <cstdint>
#include <limits>
#include <string>

namespace aios::num {

namespace detail {
inline bool blank(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
inline int digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'z') return c - 'a' + 10;
  if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
  return 99;
}
// The magnitude, the sign, and where the digits ended (s itself when there were none).
inline const char* magnitude(const char* s, int base, uint64_t* mag, bool* neg, bool* overflow) {
  const char* start = s;
  *mag = 0;
  *neg = false;
  *overflow = false;
  while (blank(*s)) ++s;
  if (*s == '+' || *s == '-') { *neg = *s == '-'; ++s; }
  if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && digit(s[2]) < 16) { s += 2; base = 16; }
  else if (base == 0) base = s[0] == '0' ? 8 : 10;
  if (base < 2 || base > 36) return start;
  const char* digits = s;
  for (; digit(*s) < base; ++s) {
    const uint64_t d = (uint64_t)digit(*s);
    if (*mag > (UINT64_MAX - d) / (uint64_t)base) *overflow = true;
    else *mag = *mag * (uint64_t)base + d;
  }
  return s == digits ? start : s;
}
}  // namespace detail

inline long long to_ll(const char* s, const char** end = nullptr, int base = 10) {
  uint64_t m;
  bool neg, over;
  const char* e = detail::magnitude(s, base, &m, &neg, &over);
  if (end) *end = e;
  constexpr uint64_t kMax = (uint64_t)std::numeric_limits<long long>::max();
  if (neg) return (over || m > kMax + 1) ? std::numeric_limits<long long>::min() : (long long)(0 - m);
  return (over || m > kMax) ? std::numeric_limits<long long>::max() : (long long)m;
}
inline unsigned long long to_ull(const char* s, const char** end = nullptr, int base = 10) {
  uint64_t m;
  bool neg, over;
  const char* e = detail::magnitude(s, base, &m, &neg, &over);
  if (end) *end = e;
  if (over) return std::numeric_limits<unsigned long long>::max();
  return neg ? (unsigned long long)(0 - m) : m;   // strtoull negates in the unsigned type
}
inline long long to_ll(const std::string& s, int base = 10) { return to_ll(s.c_str(), nullptr, base); }
inline int to_int(const char* s, int base = 10) { return (int)to_ll(s, nullptr, base); }   // atoi's answer for what fits

}  // namespace aios::num
