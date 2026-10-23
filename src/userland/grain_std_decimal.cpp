// Exact decimal numbers for grain: money, quantities, anything where 0.1 + 0.2 must be 0.3.
//
//   let price = decimal("19.99")
//   let total = price * 3 + decimal("0.05")      # 60.02, exactly
//   round(total / 7, 2)                          # half away from zero, as PHP's round() does
//   decimal("2.50") == 2.5d                      # (no literal yet: decimal("2.5"))  equal: 2.50 is 2.5
//
// A decimal is an integer mantissa (128 bits, 38 digits) and a scale (digits after the point, 0..30). The
// rules, so a result is never a surprise:
//   - + - and % align to the larger scale; * adds the scales (past 30 it rounds half-up to 30)
//   - / keeps at least 12 digits (more if an operand has them), rounded half away from zero; dec_div(a, b, n)
//     picks the scale
//   - an int mixes freely (it is a decimal with scale 0); a real does not - decimal(x) says that is meant
//   - 1.50 == 1.5; the scale only decides how it prints, so a price keeps its two places
//   - overflow past 38 digits and division by zero stop the program with an error, never a wrong answer
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "aios/userland/grain.hpp"
#include "grain_builtins_internal.hpp"

namespace aios::userland::grain {
namespace {

constexpr int kMaxScale = 30;
constexpr int kMinDivScale = 12;

using I = AIOS_INT128;

// 10^n for n up to 38 (10^38 < 2^127).
I pow10(int n) {
  I r = 1;
  for (int k = 0; k < n; ++k) r = r * 10;
  return r;
}

I iabs(I v) { return v < 0 ? -v : v; }

// n / d rounded half away from zero.
I round_div(I n, I d) {
  I q = n / d, r = n % d;
  if (iabs(r) * 2 >= iabs(d)) q += ((n < 0) != (d < 0)) ? -1 : 1;
  return q;
}

struct Dec { I m = 0; int s = 0; };

bool get(const Value& v, Dec& d) {
  if (v.type == Type::Int) { d.m = v.i; d.s = 0; return true; }
  if (v.type == Type::Decimal) {
    auto* o = static_cast<const DecimalObj*>(v.obj.get());
    d.m = o->mant; d.s = o->scale;
    return true;
  }
  return false;
}

Value make(I m, int s) { return Value::object(Type::Decimal, std::make_shared<DecimalObj>(m, s)); }

bool mul_checked(I a, I b, I& out) { return !__builtin_mul_overflow(a, b, &out); }

// The mantissa of m (at scale `from`) at scale `to`; false on overflow.
bool rescale(I m, int from, int to, I& out) {
  if (to == from) { out = m; return true; }
  if (to > from) return mul_checked(m, pow10(to - from), out);
  out = round_div(m, pow10(from - to));
  return true;
}

bool overflow(std::string& error) { error = "decimal overflow (more than 38 digits)"; return false; }

}  // namespace

Value decimal_from_int(int64_t v) { return make(v, 0); }

std::string decimal_text(const Value& v) {
  Dec d;
  if (!get(v, d)) return "?";
  const bool neg = d.m < 0;
  I a = iabs(d.m);
  std::string digits;
  if (a == 0) digits = "0";
  while (a > 0) { digits.insert(digits.begin(), (char)('0' + (int)(a % 10))); a /= 10; }
  if (d.s > 0) {
    if ((int)digits.size() <= d.s) digits.insert(0, (size_t)(d.s + 1 - (int)digits.size()), '0');
    digits.insert(digits.size() - (size_t)d.s, ".");
  }
  return neg ? "-" + digits : digits;
}

bool decimal_parse(const std::string& text, Value& out, std::string& error) {
  size_t i = 0, n = text.size();
  while (i < n && (text[i] == ' ' || text[i] == '\t')) ++i;
  while (n > i && (text[n - 1] == ' ' || text[n - 1] == '\t')) --n;
  bool neg = false;
  if (i < n && (text[i] == '-' || text[i] == '+')) neg = text[i++] == '-';
  I m = 0;
  int scale = 0, digits = 0;
  bool point = false;
  for (; i < n && text[i] != 'e' && text[i] != 'E'; ++i) {
    const char c = text[i];
    if (c == '.' && !point) { point = true; continue; }
    if (c < '0' || c > '9') { error = "'" + text + "' is not a decimal number"; return false; }
    ++digits;
    if (__builtin_mul_overflow(m, (I)10, &m) || __builtin_add_overflow(m, (I)(c - '0'), &m)) return overflow(error);
    if (point) ++scale;
  }
  if (digits == 0) { error = "'" + text + "' is not a decimal number"; return false; }
  int exp = 0;
  if (i < n) {   // 1e3, 2.5E-2
    ++i;
    bool eneg = false;
    if (i < n && (text[i] == '-' || text[i] == '+')) eneg = text[i++] == '-';
    if (i >= n) { error = "'" + text + "' is not a decimal number"; return false; }
    for (; i < n; ++i) {
      if (text[i] < '0' || text[i] > '9' || exp > 1000) { error = "'" + text + "' is not a decimal number"; return false; }
      exp = exp * 10 + (text[i] - '0');
    }
    if (eneg) exp = -exp;
  }
  scale -= exp;
  if (scale < 0) {   // 1e3: whole digits
    I scaled;
    if (-scale > 38 || !mul_checked(m, pow10(-scale), scaled)) return overflow(error);
    m = scaled;
    scale = 0;
  }
  while (scale > kMaxScale) {   // 0.1234...: more places than a decimal keeps - rounded, like everything else
    m = round_div(m, 10);
    --scale;
  }
  out = make(neg ? -m : m, scale);
  return true;
}

bool decimal_negate(const Value& a, Value& out, std::string&) {
  Dec x;
  get(a, x);
  out = make(-x.m, x.s);
  return true;
}

int decimal_compare(const Value& a, const Value& b) {
  Dec x, y;
  get(a, x);
  get(b, y);
  const int s = std::max(x.s, y.s);
  I xm, ym;
  const bool xo = !rescale(x.m, x.s, s, xm), yo = !rescale(y.m, y.s, s, ym);
  if (xo || yo) {   // one side is too big to line up: it is the larger in magnitude
    if (xo && yo) return 0;
    return xo ? (x.m < 0 ? -1 : 1) : (y.m < 0 ? 1 : -1);
  }
  return xm < ym ? -1 : xm > ym ? 1 : 0;
}

bool decimal_arith(char op, const Value& a, const Value& b, Value& out, std::string& error) {
  Dec x, y;
  if (!get(a, x) || !get(b, y)) {
    error = std::string("cannot mix a decimal with a ") + type_name(a.type == Type::Decimal ? b.type : a.type) +
            " (use decimal(x) to make it exact)";
    return false;
  }
  switch (op) {
    case '+': case '-': {
      const int s = std::max(x.s, y.s);
      I xm, ym, r;
      if (!rescale(x.m, x.s, s, xm) || !rescale(y.m, y.s, s, ym)) return overflow(error);
      if (op == '+' ? __builtin_add_overflow(xm, ym, &r) : __builtin_sub_overflow(xm, ym, &r)) return overflow(error);
      out = make(r, s);
      return true;
    }
    case '*': {
      I r;
      if (!mul_checked(x.m, y.m, r)) return overflow(error);
      int s = x.s + y.s;
      while (s > kMaxScale) { r = round_div(r, 10); --s; }
      out = make(r, s);
      return true;
    }
    case '/': {
      if (y.m == 0) { error = "division by zero"; return false; }
      const int s = std::max({kMinDivScale, x.s, y.s});
      // (x.m / 10^x.s) / (y.m / 10^y.s) at scale s = x.m * 10^(s - x.s + y.s) / y.m
      I num;
      const int up = s - x.s + y.s;
      if (up > 38 || !mul_checked(x.m, pow10(up), num)) return overflow(error);
      out = make(round_div(num, y.m), s);
      return true;
    }
    case '%': {
      if (y.m == 0) { error = "modulo by zero"; return false; }
      const int s = std::max(x.s, y.s);
      I xm, ym;
      if (!rescale(x.m, x.s, s, xm) || !rescale(y.m, y.s, s, ym)) return overflow(error);
      out = make(ym == -1 ? 0 : xm % ym, s);
      return true;
    }
  }
  error = "unknown decimal operator";
  return false;
}

bool decimal_round(const Value& v, int places, Value& out, std::string& error) {
  Dec d;
  if (!get(v, d)) { error = "round() wants a decimal"; return false; }
  if (places < 0 || places > kMaxScale) { error = "a decimal rounds to 0..30 places"; return false; }
  if (places >= d.s) { out = make(d.m, d.s); return true; }   // already that exact: keep its own scale
  out = make(round_div(d.m, pow10(d.s - places)), places);
  return true;
}

double decimal_to_double(const Value& v) {
  Dec d;
  if (!get(v, d)) return 0.0;
  return std::strtod(decimal_text(v).c_str(), nullptr);
}

bool decimal_to_int(const Value& v, int64_t& out) {
  Dec d;
  if (!get(v, d)) return false;
  const I whole = d.m / pow10(d.s);
  if (whole > (I)INT64_MAX || whole < (I)INT64_MIN) return false;
  out = (int64_t)whole;
  return true;
}

namespace {

// decimal(x [, places]): from an int, a string ("19.99", "1e3"), a decimal, or a real - which becomes the
// shortest decimal that reads back as that real, so decimal(0.1) is 0.1 and not 0.1000000000000000055.
Value bi_decimal(Vm&, std::vector<Value>& args, std::string& error) {
  Value out;
  const Value& v = args[0];
  if (v.type == Type::Decimal || v.type == Type::Int) out = v.type == Type::Int ? decimal_from_int(v.i) : v;
  else if (v.type == Type::Str) { if (!decimal_parse(v.str(), out, error)) { error = "decimal(): " + error; return Value::nil(); } }
  else if (v.type == Type::Real) {
    if (std::isnan(v.d) || std::isinf(v.d)) { error = "decimal(): a real that is not a finite number"; return Value::nil(); }
    char buf[64];
    for (int digits = 15; digits <= 17; ++digits) {
      std::snprintf(buf, sizeof buf, "%.*g", digits, v.d);
      if (std::strtod(buf, nullptr) == v.d) break;
    }
    if (!decimal_parse(buf, out, error)) { error = "decimal(): " + error; return Value::nil(); }
  } else {
    error = std::string("decimal() cannot convert a ") + type_name(v.type);
    return Value::nil();
  }
  if (args.size() > 1) {
    if (!want_number(args[1], error, "decimal")) return Value::nil();
    // an explicit scale pads (2 -> 2.00) as well as rounds
    Dec d;
    get(out, d);
    const int places = (int)args[1].as_int();
    if (places < 0 || places > kMaxScale) { error = "decimal(): places from 0 to 30"; return Value::nil(); }
    I m;
    if (!rescale(d.m, d.s, places, m)) { overflow(error); error = "decimal(): " + error; return Value::nil(); }
    out = make(m, places);
  }
  return out;
}

// dec_div(a, b, places): a / b rounded half away from zero to `places`.
Value bi_dec_div(Vm&, std::vector<Value>& args, std::string& error) {
  if (!is_exact_number(args[0]) || !is_exact_number(args[1])) { error = "dec_div() wants decimals or ints"; return Value::nil(); }
  if (!want_number(args[2], error, "dec_div")) return Value::nil();
  const int places = (int)args[2].as_int();
  if (places < 0 || places > kMaxScale) { error = "dec_div(): places from 0 to 30"; return Value::nil(); }
  Dec x, y;
  get(args[0], x);
  get(args[1], y);
  if (y.m == 0) { error = "dec_div(): division by zero"; return Value::nil(); }
  I num;
  const int up = places - x.s + y.s;
  if (up >= 0) {
    if (up > 38 || !mul_checked(x.m, pow10(up), num)) { overflow(error); error = "dec_div(): " + error; return Value::nil(); }
    return make(round_div(num, y.m), places);
  }
  // fewer places than the operands carry: divide the divisor up instead
  I den;
  if (-up > 38 || !mul_checked(y.m, pow10(-up), den)) { overflow(error); error = "dec_div(): " + error; return Value::nil(); }
  return make(round_div(x.m, den), places);
}

// scale(d): digits after the point, as it prints.
Value bi_scale(Vm&, std::vector<Value>& args, std::string& error) {
  Dec d;
  if (!get(args[0], d)) { error = std::string("scale() wants a decimal, got a ") + type_name(args[0].type); return Value::nil(); }
  return Value::integer(d.s);
}

}  // namespace

void install_decimal_builtins(Vm& vm) {
  vm.define_native("decimal", 1, 2, bi_decimal);
  vm.define_native("dec_div", 3, 3, bi_dec_div);
  vm.define_native("scale", 1, 1, bi_scale);
}

}  // namespace aios::userland::grain
