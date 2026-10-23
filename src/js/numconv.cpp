// Number <-> string conversions with ECMAScript's rules (Number::toString, StringToNumber).
#include <algorithm>
#include "aios/core/numparse.hpp"
#include "aios/hal/host.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "aios/js/context.hpp"
#include "internal_num.hpp"

namespace aios::js {

namespace {

// The shortest digit string that reads back as `d`, and the decimal exponent n such that value = 0.DIGITS * 10^n.
void shortest_digits(double d, std::string* digits, int* n) {
  char buf[64];
  for (int prec = 1; prec <= 17; ++prec) {
    std::snprintf(buf, sizeof buf, "%.*e", prec - 1, d);
    if (aios::hal::parse_double(buf, nullptr) == d || prec == 17) break;
  }
  // buf is d.ddddde[+-]xx
  std::string s(buf);
  size_t e = s.find('e');
  std::string mant = s.substr(0, e);
  int exp10 = aios::num::to_int(s.c_str() + e + 1);
  digits->clear();
  for (char c : mant) if (c >= '0' && c <= '9') digits->push_back(c);
  while (digits->size() > 1 && digits->back() == '0') digits->pop_back();
  *n = exp10 + 1;
}

}  // namespace

std::string number_to_ascii(double d) {
  if (std::isnan(d)) return "NaN";
  if (d == 0) return "0";
  if (std::isinf(d)) return d < 0 ? "-Infinity" : "Infinity";
  std::string out;
  if (d < 0) { out += '-'; d = -d; }
  // Fast path: a whole number that fits exactly.
  if (d < 1e15 && d == std::floor(d)) {
    char b[32];
    std::snprintf(b, sizeof b, "%.0f", d);
    return out + b;
  }
  std::string digits;
  int n;
  shortest_digits(d, &digits, &n);
  const int k = static_cast<int>(digits.size());
  if (k <= n && n <= 21) {
    out += digits;
    out.append(static_cast<size_t>(n - k), '0');
  } else if (0 < n && n <= 21) {
    out += digits.substr(0, static_cast<size_t>(n));
    out += '.';
    out += digits.substr(static_cast<size_t>(n));
  } else if (-6 < n && n <= 0) {
    out += "0.";
    out.append(static_cast<size_t>(-n), '0');
    out += digits;
  } else {
    const int e = n - 1;
    out += digits[0];
    if (k > 1) { out += '.'; out += digits.substr(1); }
    out += 'e';
    out += e < 0 ? '-' : '+';
    out += std::to_string(e < 0 ? -e : e);
  }
  return out;
}

std::string number_to_radix(double value, int radix) {
  if (std::isnan(value)) return "NaN";
  if (value == 0) return "0";
  if (std::isinf(value)) return value < 0 ? "-Infinity" : "Infinity";
  static const char chars[] = "0123456789abcdefghijklmnopqrstuvwxyz";
  const bool neg = value < 0;
  if (neg) value = -value;
  double integer = std::floor(value);
  double fraction = value - integer;
  // How far the representable neighbours are: stop emitting fraction digits once they stop mattering.
  double next = std::nextafter(value, INFINITY);
  double delta = 0.5 * (next - value);
  delta = std::max(std::nextafter(0.0, 1.0), delta);
  std::string frac;
  if (fraction >= delta) {
    do {
      fraction *= radix;
      delta *= radix;
      int digit = static_cast<int>(fraction);
      frac += chars[digit];
      fraction -= digit;
      if (fraction > 0.5 || (fraction == 0.5 && (digit & 1))) {
        if (fraction + delta > 1) {
          // round up, propagating carries through the digits written so far
          for (;;) {
            if (frac.empty()) { integer += 1; break; }
            char c = frac.back();
            int dg = c > '9' ? (c - 'a' + 10) : (c - '0');
            if (dg + 1 < radix) { frac.back() = chars[dg + 1]; break; }
            frac.pop_back();
          }
          break;
        }
      }
    } while (fraction >= delta);
  }
  std::string ip;
  int exponent = 0;
  std::frexp(integer, &exponent);
  while (exponent > 53) {   // too many digits for exact remainders: the low ones are zero
    integer /= radix;
    ip += '0';
    std::frexp(integer, &exponent);
  }
  do {
    double rem = std::fmod(integer, radix);
    ip += chars[static_cast<int>(rem)];
    integer = (integer - rem) / radix;
  } while (integer > 0);
  std::reverse(ip.begin(), ip.end());
  std::string out = neg ? "-" : "";
  out += ip;
  if (!frac.empty()) { out += '.'; out += frac; }
  return out;
}

bool is_js_whitespace(char16_t c) {
  switch (c) {
    case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x20: case 0xA0: case 0x1680: case 0x2028: case 0x2029:
    case 0x202F: case 0x205F: case 0x3000: case 0xFEFF:
      return true;
    default:
      return c >= 0x2000 && c <= 0x200A;
  }
}

bool parse_decimal_ascii(const std::string& s, double* out) {
  // [sign] ( digits [. digits] | . digits ) [e [sign] digits], nothing else.
  size_t i = 0;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
  size_t int_digits = 0, frac_digits = 0;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i; ++int_digits; }
  if (i < s.size() && s[i] == '.') {
    ++i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i; ++frac_digits; }
  }
  if (int_digits + frac_digits == 0) return false;
  if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    ++i;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
    size_t ed = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i; ++ed; }
    if (ed == 0) return false;
  }
  if (i != s.size()) return false;
  *out = aios::hal::parse_double(s.c_str(), nullptr);
  return true;
}

double Context::string_to_number(const std::u16string& in) {
  size_t a = 0, b = in.size();
  while (a < b && is_js_whitespace(in[a])) ++a;
  while (b > a && is_js_whitespace(in[b - 1])) --b;
  if (a == b) return 0;
  std::string s;
  s.reserve(b - a);
  for (size_t i = a; i < b; ++i) {
    if (in[i] > 0x7F) return NAN;
    s += static_cast<char>(in[i]);
  }
  if (s == "Infinity" || s == "+Infinity") return INFINITY;
  if (s == "-Infinity") return -INFINITY;
  if (s.size() > 2 && s[0] == '0') {
    int base = 0;
    if (s[1] == 'x' || s[1] == 'X') base = 16;
    else if (s[1] == 'o' || s[1] == 'O') base = 8;
    else if (s[1] == 'b' || s[1] == 'B') base = 2;
    if (base) {
      double v = 0;
      for (size_t i = 2; i < s.size(); ++i) {
        int d;
        char c = s[i];
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return NAN;
        if (d >= base) return NAN;
        v = v * base + d;
      }
      return v;
    }
  }
  double d;
  if (!parse_decimal_ascii(s, &d)) return NAN;
  return d;
}

}  // namespace aios::js
