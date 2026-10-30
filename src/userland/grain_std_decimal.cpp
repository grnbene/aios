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
#include <charconv>
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

Value make(I m, int s) { return Value::object(Type::Decimal, std::make_shared<DecimalObj>(m, s)); }
Value frac_at30(const DecimalObj* o);

bool get(const Value& v, Dec& d) {
  if (v.type == Type::Int) { d.m = v.i; d.s = 0; return true; }
  if (v.type == Type::Decimal) {
    auto* o = static_cast<const DecimalObj*>(v.obj.get());
    if (o->den != 0) {   // a fraction: as its 30 places, for what wants a decimal that ends (printing, scale)
      const Value r = frac_at30(o);
      auto* ro = static_cast<const DecimalObj*>(r.obj.get());
      d.m = ro->mant; d.s = ro->scale;
      return true;
    }
    d.m = o->mant; d.s = o->scale;
    return true;
  }
  return false;
}

bool mul_checked(I a, I b, I& out) { return !__builtin_mul_overflow(a, b, &out); }

// The mantissa of m (at scale `from`) at scale `to`; false on overflow.
bool rescale(I m, int from, int to, I& out) {
  if (to == from) { out = m; return true; }
  if (to > from) return mul_checked(m, pow10(to - from), out);
  out = round_div(m, pow10(from - to));
  return true;
}

bool overflow(std::string& error) { error = "decimal overflow (more than 38 digits)"; return false; }

// n / d rounded by `mode`. `over` says d is larger than any n here (a divisor past 10^38), so the quotient is 0
// with all of n left over.
I round_div_mode(I n, I d, RoundMode mode, bool over = false) {
  const I q = over ? 0 : n / d;
  const I r = over ? n : n % d;
  if (r == 0) return q;
  const I away = ((n < 0) != (d < 0)) ? -1 : 1;   // the direction away from zero
  int half = -1;                                   // the remainder against half the divisor: below, at, above
  if (!over) {
    const I r2 = iabs(r), dd = iabs(d);
    half = r2 > dd - r2 ? 1 : (r2 == dd - r2 ? 0 : -1);
  }
  switch (mode) {
    case RoundMode::Down: return q;
    case RoundMode::Up: return q + away;
    case RoundMode::Floor: return away < 0 ? q - 1 : q;
    case RoundMode::Ceil: return away > 0 ? q + 1 : q;
    case RoundMode::HalfUp: return half >= 0 ? q + away : q;
    case RoundMode::HalfDown: return half > 0 ? q + away : q;
    case RoundMode::HalfEven: return half > 0 || (half == 0 && q % 2 != 0) ? q + away : q;
  }
  return q;
}

// A real as the decimal it prints as: its shortest digits (to_chars), so 2.675 is 2.675 and not the
// 2.67499999999999982236431605997495353221893310546875 its bits hold. The scale may pass 38 for a tiny real (5e-324
// is 5 at scale 324): the mantissa is at most 17 digits, and number_round divides such a scale away exactly.
bool real_digits(double x, I& m, int& s, std::string& error) {
  if (std::isnan(x) || std::isinf(x)) { error = "a real that is not a finite number"; return false; }
  char buf[64];
  auto r = std::to_chars(buf, buf + sizeof buf, x, std::chars_format::scientific);
  const std::string t(buf, r.ptr);
  const size_t e = t.find('e');
  const int exp10 = std::atoi(t.c_str() + e + 1);
  const bool neg = t[0] == '-';
  I v = 0;
  int ndigits = 0;
  for (size_t i = neg ? 1 : 0; i < e; ++i) if (t[i] != '.') { v = v * 10 + (t[i] - '0'); ++ndigits; }
  int scale = ndigits - 1 - exp10;   // value = v * 10^-scale
  if (scale < 0) {
    if (-scale > 38 || !mul_checked(v, pow10(-scale), v)) { error = "too large for a decimal (38 digits)"; return false; }
    scale = 0;
  }
  m = neg ? -v : v;
  s = scale;
  return true;
}

// ---- fractions: a quotient that does not end, kept exact -------------------------------------------------------
//
// 1 / 3 is the fraction 1/3, not 0.333333333333: (1 / 3) * 3 is 1 and 2 / 3 * 3 is 2, where a quotient cut to 12
// places gives 2.000000000001. A fraction is held reduced, its denominator up to 10^36 and never only 2s and 5s
// (that would be a decimal that ends, and is made one). It prints to 12 places - or as many as the decimals it came
// from had - and rounds exactly. Arithmetic that would pass 128 bits rounds the answer to 30 places instead.

struct Frac { I n = 0; I d = 1; };

I gcd128(I a, I b) {
  a = iabs(a);
  b = iabs(b);
  while (b != 0) { const I t = a % b; a = b; b = t; }
  return a;
}

const I kMaxDen = pow10(36);

const DecimalObj* dec_obj(const Value& v) {
  return v.type == Type::Decimal && v.obj ? static_cast<const DecimalObj*>(v.obj.get()) : nullptr;
}
bool is_frac(const Value& v) { const DecimalObj* o = dec_obj(v); return o && o->den != 0; }

bool frac_of(const Value& v, Frac& f) {
  if (v.type == Type::Int) { f = {v.i, 1}; return true; }
  const DecimalObj* o = dec_obj(v);
  if (!o) return false;
  if (o->den != 0) { f = {o->mant, o->den}; return true; }
  f = {o->mant, pow10(o->scale)};
  return true;
}

int print_places(const Value& v) {   // how many places a decimal or fraction prints with
  const DecimalObj* o = dec_obj(v);
  return o ? o->scale : 0;
}

// round(n / d) to `places` places (0..30) by `mode`, exactly, as the mantissa at that scale. d > 0, d <= 10^37.
bool frac_round(I n, I d, int places, RoundMode mode, I& out) {
  const bool neg = n < 0;
  const I a = iabs(n);
  const I whole = a / d;
  I r = a % d;
  I q;
  if (!mul_checked(whole, pow10(places), q)) return false;
  I digits = 0;
  for (int k = 0; k < places; ++k) {   // r < d <= 10^37, so r * 10 fits
    r *= 10;
    digits = digits * 10 + r / d;
    r %= d;
  }
  if (__builtin_add_overflow(q, digits, &q)) return false;
  if (r != 0) {
    const int half = r * 2 > d ? 1 : (r * 2 == d ? 0 : -1);
    bool up = false;   // away from zero, in magnitude
    switch (mode) {
      case RoundMode::Down: up = false; break;
      case RoundMode::Up: up = true; break;
      case RoundMode::Floor: up = neg; break;
      case RoundMode::Ceil: up = !neg; break;
      case RoundMode::HalfUp: up = half >= 0; break;
      case RoundMode::HalfDown: up = half > 0; break;
      case RoundMode::HalfEven: up = half > 0 || (half == 0 && q % 2 != 0); break;
    }
    if (up) ++q;
  }
  out = neg ? -q : q;
  return true;
}

// n / d at 30 places (half away from zero, trailing zeros dropped): what a fraction is when something wants a decimal
// that ends. d may be any size here: past 10^37 both sides shed digits first (an error far below the 30th place).
Value frac_rounded(I n, I d) {
  if (d < 0) { n = -n; d = -d; }
  while (d > pow10(37)) { n /= 10; d /= 10; }
  I q = 0;
  frac_round(n, d, kMaxScale, RoundMode::HalfUp, q);
  int sc = kMaxScale;
  while (sc > 0 && q % 10 == 0) { q /= 10; --sc; }
  return make(q, sc);
}

// The value of n / d: reduced; an int's worth or a decimal that ends when it is one (whole: scale 0); a fraction
// printing with `places` places otherwise; rounded to 30 places when the denominator would pass 10^36.
Value from_frac(I n, I d, int places) {
  if (d < 0) { n = -n; d = -d; }
  const I g = gcd128(n, d);
  if (g > 1) { n /= g; d /= g; }
  I t = d;
  int twos = 0, fives = 0;
  while (t % 2 == 0) { t /= 2; ++twos; }
  while (t % 5 == 0) { t /= 5; ++fives; }
  if (t == 1) {   // it ends
    const int k = std::max(twos, fives);
    I m;
    if (k <= kMaxScale && mul_checked(n, pow10(k) / d, m)) return make(m, k);
    return frac_rounded(n, d);
  }
  if (d > kMaxDen) return frac_rounded(n, d);
  return Value::object(Type::Decimal, std::make_shared<DecimalObj>(n, std::max(places, kMinDivScale), d));
}

Value frac_at30(const DecimalObj* o) { return frac_rounded(o->mant, o->den); }

// a op b with a fraction on either side (or a plain quotient that does not end): exactly, or - past 128 bits - each
// side at 30 places and the decimal answer.
bool frac_arith(char op, const Value& a, const Value& b, Value& out, std::string& error);

// Compare two fractions exactly (no overflow: continued-fraction steps when cross products would not fit).
int frac_compare(Frac x, Frac y) {
  const int sx = x.n < 0 ? -1 : (x.n > 0 ? 1 : 0), sy = y.n < 0 ? -1 : (y.n > 0 ? 1 : 0);
  if (sx != sy) return sx < sy ? -1 : 1;
  if (sx == 0) return 0;
  I a = iabs(x.n), b = x.d, c = iabs(y.n), d = y.d;
  int flip = sx;   // comparing magnitudes; negatives reverse the answer
  for (;;) {
    I l, r;
    if (mul_checked(a, d, l) && mul_checked(c, b, r)) return l < r ? -flip : (l > r ? flip : 0);
    const I qa = a / b, qc = c / d;
    if (qa != qc) return qa < qc ? -flip : flip;
    const I ra = a % b, rc = c % d;
    if (ra == 0 || rc == 0) return ra == rc ? 0 : (ra == 0 ? -flip : flip);
    // a/b vs c/d with equal whole parts: compare the remainders, which is the reciprocals the other way round
    a = b; b = ra;
    const I c2 = d;
    d = rc;
    c = c2;
    flip = -flip;
  }
}

// An int, decimal or real as an exact decimal (a fraction as its 30 places).
bool exact_of(const Value& v, Dec& d, std::string& error) {
  if (get(v, d)) return true;
  if (v.type == Type::Real) return real_digits(v.d, d.m, d.s, error);
  error = std::string("wants a number, got a ") + type_name(v.type);
  return false;
}

}  // namespace

Value decimal_from_int(int64_t v) { return make(v, 0); }

std::string decimal_text(const Value& v) {
  Dec d;
  if (const DecimalObj* o = dec_obj(v); o && o->den != 0) {   // a fraction: to its places, half away from zero
    I q = 0;
    frac_round(o->mant, o->den, o->scale, RoundMode::HalfUp, q);
    d.m = q;
    d.s = o->scale;
  } else if (!get(v, d)) {
    return "?";
  }
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
  if (const DecimalObj* o = dec_obj(a); o && o->den != 0) {
    out = Value::object(Type::Decimal, std::make_shared<DecimalObj>(-o->mant, o->scale, o->den));
    return true;
  }
  Dec x;
  get(a, x);
  out = make(-x.m, x.s);
  return true;
}

int decimal_compare(const Value& a, const Value& b) {
  if (is_frac(a) || is_frac(b)) {   // exactly, as fractions
    Frac x, y;
    frac_of(a, x);
    frac_of(b, y);
    return frac_compare(x, y);
  }
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
  // With a real on either side the answer is a real: the decimal (or int) as its nearest real, then binary
  // arithmetic. A real is inexact; the result being one says so.
  if ((a.type == Type::Real && (b.type == Type::Decimal || b.type == Type::Int || b.type == Type::Real)) ||
      (b.type == Type::Real && (a.type == Type::Decimal || a.type == Type::Int))) {
    const double x = a.as_double(), y = b.as_double();
    switch (op) {
      case '+': out = Value::real(x + y); return true;
      case '-': out = Value::real(x - y); return true;
      case '*': out = Value::real(x * y); return true;
      case '/': if (y == 0) { error = "division by zero"; return false; } out = Value::real(x / y); return true;
      case '%': if (y == 0) { error = "modulo by zero"; return false; } out = Value::real(std::fmod(x, y)); return true;
    }
  }
  if (is_frac(a) || is_frac(b)) return frac_arith(op, a, b, out, error);
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
      if (up > 38 || !mul_checked(x.m, pow10(up), num)) return frac_arith('/', a, b, out, error);   // big: exactly
      // An exact quotient keeps no more places than it needs (10 / 4 is 2.5, 10.00 / 4 is 2.50); one that does not
      // end is the exact fraction (1 / 3 is 1/3, printing 0.333333333333).
      I q = num / y.m;
      if (q * y.m == num) {
        int qs = s;
        const int keep = std::max(x.s, y.s);
        while (qs > keep && q % 10 == 0) { q /= 10; --qs; }
        out = make(q, qs);
        return true;
      }
      return frac_arith('/', a, b, out, error);
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

namespace {
bool frac_arith(char op, const Value& a, const Value& b, Value& out, std::string& error) {
  Frac x, y;
  if (!frac_of(a, x) || !frac_of(b, y)) { error = "cannot mix a decimal with that"; return false; }
  const int places = std::max(print_places(a), print_places(b));
  I n = 0, d = 1;
  bool fits = true;
  switch (op) {
    case '+': case '-': {
      const I g = gcd128(x.d, y.d);
      I l, r, dd;
      fits = mul_checked(x.n, y.d / g, l) && mul_checked(y.n, x.d / g, r) && mul_checked(x.d / g, y.d, dd) &&
             !(op == '+' ? __builtin_add_overflow(l, r, &n) : __builtin_sub_overflow(l, r, &n));
      d = dd;
      break;
    }
    case '*': {
      const I g1 = gcd128(x.n, y.d), g2 = gcd128(y.n, x.d);
      fits = mul_checked(g1 ? x.n / g1 : 0, g2 ? y.n / g2 : 0, n) && mul_checked(x.d / (g2 ? g2 : 1), y.d / (g1 ? g1 : 1), d);
      break;
    }
    case '/': {
      if (y.n == 0) { error = "division by zero"; return false; }
      const I g1 = gcd128(x.n, y.n), g2 = gcd128(x.d, y.d);
      fits = mul_checked(g1 ? x.n / g1 : 0, y.d / g2, n) && mul_checked(x.d / g2, g1 ? y.n / g1 : 1, d);
      break;
    }
    case '%': {   // x - trunc(x / y) * y
      if (y.n == 0) { error = "modulo by zero"; return false; }
      Value q;
      if (!frac_arith('/', a, b, q, error)) return false;
      I whole = 0;
      if (const DecimalObj* o = dec_obj(q); o && o->den != 0) whole = o->mant / o->den;
      else if (q.type == Type::Int) whole = q.i;
      else { Dec qd; get(q, qd); whole = qd.m / pow10(qd.s); }
      Value prod, r;
      if (!frac_arith('*', make(whole, 0), b, prod, error) || !frac_arith('-', a, prod, r, error)) return false;
      out = r;
      return true;
    }
    default: error = "unknown decimal operator"; return false;
  }
  if (fits) { out = from_frac(n, d, places); return true; }
  // past 128 bits: each side at 30 places, then the decimal answer (which may round in its turn). A quotient is the
  // first side times the second's reciprocal, both at 30 places, so it never comes back here.
  if (op == '/') return decimal_arith('*', frac_rounded(x.n, x.d), frac_rounded(y.d, y.n), out, error);
  return decimal_arith(op, frac_rounded(x.n, x.d), frac_rounded(y.n, y.d), out, error);
}
}  // namespace

bool decimal_round(const Value& v, int places, Value& out, std::string& error) {
  Dec d;
  if (!get(v, d)) { error = "round() wants a decimal"; return false; }
  if (places < 0 || places > kMaxScale) { error = "a decimal rounds to 0..30 places"; return false; }
  if (places >= d.s) {   // already that exact: written with exactly `places` places (round(2.5, 2) is 2.50)
    I m;
    if (!rescale(d.m, d.s, places, m)) return overflow(error);
    out = make(m, places);
    return true;
  }
  out = make(round_div(d.m, pow10(d.s - places)), places);
  return true;
}

// The one rounding there is: v (int, decimal or real - a real from its 15 significant digits) to `places` places by
// `mode`, as a decimal with exactly that many places. places < 0 rounds to tens, hundreds, ... (scale 0).
bool number_round(const Value& v, int places, RoundMode mode, Value& out, std::string& error) {
  if (places > kMaxScale) { error = "rounds to at most 30 places"; return false; }
  if (places < -38) { error = "rounds to at most 38 places left of the point"; return false; }
  if (const DecimalObj* o = dec_obj(v); o && o->den != 0) {   // a fraction: exactly, digit by digit
    I q = 0;
    if (places >= 0) {
      if (!frac_round(o->mant, o->den, places, mode, q)) return overflow(error);
      out = make(q, places);
      return true;
    }
    I d10;
    if (!mul_checked(o->den, pow10(-places), d10) || d10 > pow10(37)) { out = make(0, 0); return true; }
    if (!frac_round(o->mant, d10, 0, mode, q)) return overflow(error);
    I m;
    if (!mul_checked(q, pow10(-places), m)) return overflow(error);
    out = make(m, 0);
    return true;
  }
  Dec d;
  if (!exact_of(v, d, error)) return false;
  // A real computed by arithmetic carries binary noise past its 15th digit: 1.55e0 * 0.3e0 is 0.46499999999999997,
  // which its shortest digits would round to 0.46 where the sum done by hand, PHP and the ERP's invoices say 0.47.
  // So a real rounds from its value to 15 significant digits (half away from zero), then to the places.
  if (v.type == Type::Real) {
    int n = 0;
    for (I a = iabs(d.m); a > 0; a /= 10) ++n;
    if (n > 15) {
      d.m = round_div_mode(d.m, pow10(n - 15), RoundMode::HalfUp);
      d.s -= n - 15;
      if (d.s < 0) { I up; if (!mul_checked(d.m, pow10(-d.s), up)) return overflow(error); d.m = up; d.s = 0; }
    }
  }
  const int target = places < 0 ? 0 : places;
  I m;
  if (places >= d.s) {
    if (!rescale(d.m, d.s, target, m)) return overflow(error);
  } else {
    const int drop = d.s - places;   // digits to take off, >= 1
    // past 10^38 the divisor is larger than any mantissa: the quotient is 0, the remainder all of it, under half
    const I q = drop > 38 ? round_div_mode(d.m, 1, mode, true) : round_div_mode(d.m, pow10(drop), mode);
    m = q;
    if (places < 0 && !mul_checked(q, pow10(-places), m)) return overflow(error);
  }
  out = make(m, target);
  return true;
}

// a op b for two numbers, as the language does it: ints exactly (past 64 bits the exact decimal), an int or decimal
// with a decimal exactly, anything with a real as a real. For builtins that add, multiply, ... (sum, add_into, pow).
bool number_arith(char op, const Value& a, const Value& b, Value& out, std::string& error) {
  const bool an = a.type == Type::Int || a.type == Type::Real || a.type == Type::Decimal;
  const bool bn = b.type == Type::Int || b.type == Type::Real || b.type == Type::Decimal;
  if (!an || !bn) { error = std::string("wants numbers, got a ") + type_name(an ? b.type : a.type); return false; }
  if (a.type == Type::Int && b.type == Type::Int && op != '/') {
    int64_t r = 0;
    bool over = false;
    switch (op) {
      case '+': over = __builtin_add_overflow(a.i, b.i, &r); break;
      case '-': over = __builtin_sub_overflow(a.i, b.i, &r); break;
      case '*': over = __builtin_mul_overflow(a.i, b.i, &r); break;
      case '%':
        if (b.i == 0) { error = "modulo by zero"; return false; }
        r = b.i == -1 ? 0 : a.i % b.i;
        break;
      default: error = "unknown operator"; return false;
    }
    if (!over) { out = Value::integer(r); return true; }
  }
  if (a.type == Type::Real && b.type == Type::Real) {
    switch (op) {
      case '+': out = Value::real(a.d + b.d); return true;
      case '-': out = Value::real(a.d - b.d); return true;
      case '*': out = Value::real(a.d * b.d); return true;
      case '/': if (b.d == 0) { error = "division by zero"; return false; } out = Value::real(a.d / b.d); return true;
      case '%': if (b.d == 0) { error = "modulo by zero"; return false; } out = Value::real(std::fmod(a.d, b.d)); return true;
    }
  }
  return decimal_arith(op, a, b, out, error);   // exact; with a real on one side, a real
}

// pow(x, n): exact for an int or decimal raised to a whole n (2^64 is 18446744073709551616, 1.1^2 is 1.21; a negative
// n divides, as / does); a real or a fractional exponent gives a real.
bool number_pow(const Value& x, const Value& n, Value& out, std::string& error) {
  const bool exact_base = x.type == Type::Int || x.type == Type::Decimal;
  if (exact_base && n.type == Type::Int && n.i >= -1000 && n.i <= 1000) {
    Value acc = Value::integer(1), base = x;
    int64_t e = n.i < 0 ? -n.i : n.i;
    while (e > 0) {   // by squaring, each step exact (past 38 digits: an error, never a wrong answer)
      if (e & 1) { Value t; if (!number_arith('*', acc, base, t, error)) return false; acc = t; }
      e >>= 1;
      if (e) { Value t; if (!number_arith('*', base, base, t, error)) return false; base = t; }
    }
    if (n.i < 0) return number_arith('/', Value::integer(1), acc, out, error);
    out = acc;
    return true;
  }
  out = Value::real(std::pow(x.as_double(), n.as_double()));
  return true;
}

// An int, decimal or real as an exact decimal: a real as the decimal it prints as (rounded at 30 places past that).
bool number_exact(const Value& v, Value& out, std::string& error) {
  if (v.type == Type::Decimal) { out = v; return true; }
  if (v.type == Type::Int) { out = make(v.i, 0); return true; }
  Dec d;
  if (!exact_of(v, d, error)) return false;
  if (d.s <= kMaxScale) { out = make(d.m, d.s); return true; }
  Value r;
  if (!number_round(v, kMaxScale, RoundMode::HalfUp, r, error)) return false;
  Dec rd;
  get(r, rd);
  while (rd.s > 0 && rd.m % 10 == 0) { rd.m /= 10; --rd.s; }
  out = make(rd.m, rd.s);
  return true;
}

bool round_mode_named(const std::string& name, RoundMode& mode) {
  static const std::pair<const char*, RoundMode> names[] = {
      {"half_up", RoundMode::HalfUp}, {"half_even", RoundMode::HalfEven}, {"half_down", RoundMode::HalfDown},
      {"up", RoundMode::Up}, {"down", RoundMode::Down}, {"floor", RoundMode::Floor}, {"ceil", RoundMode::Ceil}};
  for (const auto& [n, m] : names) if (name == n) { mode = m; return true; }
  return false;
}

double Value::decimal_as_double() const { return decimal_to_double(*this); }
int64_t Value::decimal_as_int() const {
  if (const DecimalObj* o = dec_obj(*this); o && o->den != 0) {
    const I whole = o->mant / o->den;
    return whole > (I)INT64_MAX ? INT64_MAX : whole < (I)INT64_MIN ? INT64_MIN : (int64_t)whole;
  }
  Dec d;
  if (!get(*this, d)) return 0;
  const I whole = d.m / pow10(d.s);
  return whole > (I)INT64_MAX ? INT64_MAX : whole < (I)INT64_MIN ? INT64_MIN : (int64_t)whole;
}

double decimal_to_double(const Value& v) {
  if (const DecimalObj* o = dec_obj(v); o && o->den != 0) return std::strtod(decimal_text(frac_rounded(o->mant, o->den)).c_str(), nullptr);
  Dec d;
  if (!get(v, d)) return 0.0;
  return std::strtod(decimal_text(v).c_str(), nullptr);
}

bool decimal_to_int(const Value& v, int64_t& out) {
  if (const DecimalObj* o = dec_obj(v); o && o->den != 0) {
    const I whole = o->mant / o->den;
    if (whole > (I)INT64_MAX || whole < (I)INT64_MIN) return false;
    out = (int64_t)whole;
    return true;
  }
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
    // the decimal it prints as (decimal(0.1e0) is 0.1); past 30 places, rounded there
    if (!number_exact(v, out, error)) { error = "decimal(): " + error; return Value::nil(); }
  } else {
    error = std::string("decimal() cannot convert a ") + type_name(v.type);
    return Value::nil();
  }
  if (args.size() > 1) {
    if (!want_number(args[1], error, "decimal")) return Value::nil();
    if (is_frac(out)) {
      Value r;
      if (!number_round(out, (int)args[1].as_int(), RoundMode::HalfUp, r, error)) { error = "decimal(): " + error; return Value::nil(); }
      return r;
    }
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
  {   // the exact quotient (fractions included), rounded once, half away from zero
    Value q;
    if (!decimal_arith('/', args[0].type == Type::Int ? make(args[0].i, 0) : args[0], args[1], q, error)) {
      error = "dec_div(): " + error;
      return Value::nil();
    }
    Value r;
    if (!number_round(q, places, RoundMode::HalfUp, r, error)) { error = "dec_div(): " + error; return Value::nil(); }
    return r;
  }
}

// scale(d): digits after the point, as it prints.
Value bi_scale(Vm&, std::vector<Value>& args, std::string& error) {
  if (is_frac(args[0])) return Value::integer(print_places(args[0]));   // a fraction: the places it prints with
  Dec d;
  if (!get(args[0], d)) { error = std::string("scale() wants a decimal, got a ") + type_name(args[0].type); return Value::nil(); }
  return Value::integer(d.s);
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// decimal_prefix(s): the number a string starts with, as PHP and C's strtod read one - leading whitespace,
// a sign, digits with an optional fraction (or a fraction alone), an optional exponent with digits - as
// {"d": decimal, "whole": the string is that number and nothing after it}, or nil when it starts with none.
Value bi_decimal_prefix(Vm&, std::vector<Value>& args, std::string& error) {
  if (args[0].type != Type::Str) { error = std::string("decimal_prefix() wants a string, got a ") + type_name(args[0].type); return Value::nil(); }
  const std::string& s = args[0].str();
  const size_t n = s.size();
  size_t i = 0;
  while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' || s[i] == '\v' || s[i] == '\f')) ++i;
  const size_t start = i;
  if (i < n && (s[i] == '+' || s[i] == '-')) ++i;
  size_t whole_digits = 0, frac_digits = 0;
  while (i + whole_digits < n && is_digit(s[i + whole_digits])) ++whole_digits;
  i += whole_digits;
  if (i < n && s[i] == '.') {
    while (i + 1 + frac_digits < n && is_digit(s[i + 1 + frac_digits])) ++frac_digits;
    if (whole_digits > 0 || frac_digits > 0) i += 1 + frac_digits;
  }
  if (whole_digits == 0 && frac_digits == 0) return Value::nil();
  if (i < n && (s[i] == 'e' || s[i] == 'E')) {   // an exponent counts only with digits
    size_t j = i + 1;
    if (j < n && (s[j] == '+' || s[j] == '-')) ++j;
    size_t k = j;
    while (k < n && is_digit(s[k])) ++k;
    if (k > j) i = k;
  }
  std::string text = s.substr(start, i - start);
  if (whole_digits == 0) text.insert(text[0] == '+' || text[0] == '-' ? 1 : 0, "0");   // ".5": decimal_parse wants a digit first
  Value d;
  if (!decimal_parse(text, d, error)) { error = "decimal_prefix(): " + error; return Value::nil(); }
  auto m = std::make_shared<MapObj>();
  m->put("d", d);
  m->put("whole", Value::boolean(i == n));
  return Value::object(Type::Map, m);
}

// round_sig(d, n): d with no more than n significant digits, the extra ones rounded off half away from zero -
// but only digits after the point: a whole part longer than n keeps every digit (as printf's %.*g would not).
Value bi_round_sig(Vm&, std::vector<Value>& args, std::string& error) {
  Dec d;
  if (!get(args[0], d)) { error = std::string("round_sig() wants a decimal, got a ") + type_name(args[0].type); return Value::nil(); }
  if (!want_number(args[1], error, "round_sig")) return Value::nil();
  const int64_t n = args[1].as_int();
  if (n < 1) { error = "round_sig(): at least 1 significant digit"; return Value::nil(); }
  int digits = 1;
  for (I a = iabs(d.m); a >= 10; a /= 10) ++digits;
  const int64_t keep = (int64_t)d.s - (digits - n);
  if (digits <= n || keep < 0) return make(d.m, d.s);
  return make(round_div(d.m, pow10(d.s - (int)keep)), (int)keep);
}

// is_numeric(v): a number, or a string that is one number and nothing more (leading whitespace allowed, trailing not:
// PHP 7.4's is_numeric, "1e3" and ".5" count, "12abc" and "" do not).
Value bi_is_numeric(Vm& vm, std::vector<Value>& args, std::string& error) {
  const Value& v = args[0];
  if (v.type == Type::Int || v.type == Type::Decimal) return Value::boolean(true);
  if (v.type == Type::Real) return Value::boolean(!std::isnan(v.d) && !std::isinf(v.d));
  if (v.type != Type::Str) return Value::boolean(false);
  std::vector<Value> one{v};
  Value p = bi_decimal_prefix(vm, one, error);
  if (!error.empty() || p.type == Type::Nil) { error.clear(); return Value::boolean(false); }
  Value* whole = std::static_pointer_cast<MapObj>(p.obj)->find("whole");
  return Value::boolean(whole && whole->truthy());
}

// to_number(v [, default]): a number as it is; a string by the number it starts with, as a decimal ("12.50abc" is
// 12.50, as PHP and C's strtod read it); default (nil when not given) for a string that starts with none, or
// anything else.
Value bi_to_number(Vm& vm, std::vector<Value>& args, std::string& error) {
  const Value& v = args[0];
  const Value fallback = args.size() > 1 ? args[1] : Value::nil();
  if (v.type == Type::Int || v.type == Type::Decimal || v.type == Type::Real) return v;
  if (v.type != Type::Str) return fallback;
  std::vector<Value> one{v};
  Value p = bi_decimal_prefix(vm, one, error);
  if (!error.empty()) { error = "to_number(): " + error.substr(error.find("decimal_prefix(): ") == 0 ? 18 : 0); return Value::nil(); }
  if (p.type == Type::Nil) return fallback;
  Value* d = std::static_pointer_cast<MapObj>(p.obj)->find("d");
  if (!d) return fallback;
  // a whole number written without a point ("12") is an int, as the literal 12 is; "12.50" stays the decimal 12.50
  Dec dd;
  if (d->type == Type::Decimal && get(*d, dd) && dd.s == 0 && dd.m <= (I)INT64_MAX && dd.m >= (I)INT64_MIN &&
      v.str().find_first_of(".eE") == std::string::npos)
    return Value::integer((int64_t)dd.m);
  return *d;
}

// number_format(v [, places [, dec_point [, thousands_sep]]]): v printed with `places` digits after the point
// (default 0), groups of three split by thousands_sep (default ",") and the point written dec_point (default ".") -
// PHP's number_format(). As PHP 7.4 reads v: nil and false are 0, true 1, a string by the number it starts with
// (one that starts with none gives nil, where PHP warns and gives NULL), a real as the shortest decimal that reads
// back as it (1.005 gives 1.01, as PHP's pre-rounding does). Rounded as round() rounds: half away from zero to the
// places; a zero prints with no sign; places below 0 count as 0.
Value bi_number_format(Vm& vm, std::vector<Value>& args, std::string& error) {
  const Value& v = args[0];
  Value dv;
  if (v.type == Type::Nil || (v.type == Type::Bool && !v.b)) dv = decimal_from_int(0);
  else if (v.type == Type::Bool) dv = decimal_from_int(1);
  else if (v.type == Type::Str) {
    std::vector<Value> one{v};
    Value p = bi_decimal_prefix(vm, one, error);
    if (!error.empty()) { error = "number_format(): " + error; return Value::nil(); }
    if (p.type == Type::Nil) return Value::nil();
    Value* d = std::static_pointer_cast<MapObj>(p.obj)->find("d");
    if (!d) return Value::nil();
    dv = *d;
  } else if (v.type == Type::Int || v.type == Type::Decimal || v.type == Type::Real) {
    dv = v;
  } else {
    error = std::string("number_format() cannot format a ") + type_name(v.type);
    return Value::nil();
  }
  int64_t places = 0;
  if (args.size() > 1) {
    if (!want_number(args[1], error, "number_format")) return Value::nil();
    places = std::max<int64_t>(0, args[1].as_int());
    if (places > kMaxScale) { error = "number_format(): places from 0 to 30"; return Value::nil(); }
  }
  const std::string point = args.size() > 2 && args[2].type == Type::Str ? args[2].str() : std::string(".");
  const std::string sep = args.size() > 3 && args[3].type == Type::Str ? args[3].str() : std::string(",");
  if ((args.size() > 2 && args[2].type != Type::Str) || (args.size() > 3 && args[3].type != Type::Str)) {
    error = "number_format(): the point and the separator are strings";
    return Value::nil();
  }
  Value r;   // as round() rounds: half away from zero, a real from its 15 significant digits
  if (!number_round(dv, (int)places, RoundMode::HalfUp, r, error)) { error = "number_format(): " + error; return Value::nil(); }
  Dec d;
  get(r, d);
  const I m = d.m;
  const std::string text = decimal_text(r);   // "-1234.50"
  const bool neg = m != 0 && !text.empty() && text[0] == '-';
  const std::string body = text[0] == '-' ? text.substr(1) : text;
  const size_t dot = body.find('.');
  const std::string whole = dot == std::string::npos ? body : body.substr(0, dot);
  std::string out = neg ? "-" : "";
  for (size_t i = 0; i < whole.size(); ++i) {
    if (i > 0 && (whole.size() - i) % 3 == 0) out += sep;
    out += whole[i];
  }
  if (dot != std::string::npos) out += point + body.substr(dot + 1);
  return Value::string(out);
}

}  // namespace

void install_decimal_builtins(Vm& vm) {
  vm.define_native("decimal", 1, 2, bi_decimal);
  vm.define_native("dec_div", 3, 3, bi_dec_div);
  vm.define_native("scale", 1, 1, bi_scale);
  vm.define_native("decimal_prefix", 1, 1, bi_decimal_prefix);
  vm.define_native("round_sig", 2, 2, bi_round_sig);
  vm.define_native("number_format", 1, 4, bi_number_format);
  vm.define_native("is_numeric", 1, 1, bi_is_numeric);
  vm.define_native("to_number", 1, 2, bi_to_number);
}

}  // namespace aios::userland::grain
