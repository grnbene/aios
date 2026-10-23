// grain's decimals: exact base-10 numbers for money and quantities, carried as text so they go
// to and from a database or JSON as they are.
//
//   decimal(v)                    v as a decimal ("12.50" -> "12.5", 3 -> "3", 0.1 -> "0.1"), or nil
//                                 when v is not a number; spaces around it are allowed
//   decimal_prefix(s)             the number a string starts with, read the way MySQL and PHP read one
//                                 ("12abc", " 7", "1e3x"): {value, rest}, or nil when it starts with none
//   dec_add(a, b) dec_sub dec_mul exact
//   dec_div(a, b, places, mode?)  the quotient rounded to that many places
//   dec_cmp(a, b)                 -1, 0 or 1
//   dec_round(a, places, mode?)   modes: "half_up" (the default: halves away from zero), "half_even",
//                                 "down" (toward zero), "floor", "ceil"
//   dec_round_sig(a, digits)      to that many significant digits, halves away from zero
//   dec_format(a, places)         fixed places, rounded half up: dec_format("2.675", 2) is "2.68",
//                                 dec_format("-0.004", 2) is "0.00"
//
// An operand is a decimal string, an int, or a real (taken at its shortest exact text, so 0.1 is
// "0.1"). Results are strings in one form: no exponent, no trailing zeros after the point, no "-0".
// A string that is not a number is an error naming the function; it never counts as 0.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "aios/userland/grain.hpp"
#include "grain_builtins_internal.hpp"

namespace aios::userland::grain {

namespace {

constexpr size_t kMaxDigits = 1000;   // "1e999999" would otherwise be a megabyte of zeros

// sign * mag * 10^-scale; mag is decimal digits with no leading zeros ("" is zero)
struct Dec {
  bool neg = false;
  std::string mag;
  int scale = 0;
};

void normalize(Dec& d) {
  size_t lead = 0;
  while (lead < d.mag.size() && d.mag[lead] == '0') ++lead;
  d.mag.erase(0, lead);
  while (d.scale > 0 && !d.mag.empty() && d.mag.back() == '0') {
    d.mag.pop_back();
    --d.scale;
  }
  if (d.mag.empty()) {
    d.neg = false;
    d.scale = 0;
  }
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

// Reads a number at s[i]: [+-] digits [. digits] [e [+-] digits]; an exponent only counts when digits
// follow it. Leaves i after it. False when there are no digits.
bool read_number(const std::string& s, size_t& i, Dec& out, std::string& error) {
  Dec d;
  size_t p = i;
  if (p < s.size() && (s[p] == '+' || s[p] == '-')) d.neg = s[p++] == '-';
  size_t digits = 0;
  while (p < s.size() && s[p] >= '0' && s[p] <= '9') { d.mag += s[p++]; ++digits; }
  if (p < s.size() && s[p] == '.') {
    size_t q = p + 1, frac = 0;
    std::string f;
    while (q < s.size() && s[q] >= '0' && s[q] <= '9') { f += s[q++]; ++frac; }
    if (digits + frac > 0) {
      d.mag += f;
      d.scale = (int)frac;
      digits += frac;
      p = q;
    }
  }
  if (digits == 0) return false;
  if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
    size_t q = p + 1;
    bool eneg = false;
    if (q < s.size() && (s[q] == '+' || s[q] == '-')) eneg = s[q++] == '-';
    if (q < s.size() && s[q] >= '0' && s[q] <= '9') {
      long e = 0;
      while (q < s.size() && s[q] >= '0' && s[q] <= '9') {
        if (e < 100000) e = e * 10 + (s[q] - '0');
        ++q;
      }
      long sc = (long)d.scale + (eneg ? e : -e);
      if (sc < 0) {
        if ((size_t)-sc + d.mag.size() > kMaxDigits) { error = "a number with more than 1000 digits"; return false; }
        d.mag.append((size_t)-sc, '0');
        sc = 0;
      }
      if ((size_t)sc > kMaxDigits) { error = "a number with more than 1000 digits"; return false; }
      d.scale = (int)sc;
      p = q;
    }
  }
  if (d.mag.size() > kMaxDigits) { error = "a number with more than 1000 digits"; return false; }
  normalize(d);
  out = d;
  i = p;
  return true;
}

// The shortest text that reads back as the same double
std::string real_text(double v) {
  char buf[40];
  for (int prec = 1; prec <= 17; ++prec) {
    std::snprintf(buf, sizeof buf, "%.*g", prec, v);
    if (std::strtod(buf, nullptr) == v) break;
  }
  return buf;
}

// A whole operand: a decimal string (spaces around it allowed), an int or a finite real
bool operand(const Value& v, Dec& out, const char* who, std::string& error) {
  std::string text;
  if (v.type == Type::Int) text = std::to_string(v.i);
  else if (v.type == Type::Real) {
    if (!(v.d == v.d) || v.d > 1.7976931348623157e308 || v.d < -1.7976931348623157e308) {
      error = std::string(who) + ": " + real_text(v.d) + " is not a number with digits";
      return false;
    }
    text = real_text(v.d);
  } else if (v.type == Type::Str) text = v.str();
  else {
    error = std::string(who) + ": wants a number or a decimal string, got " + type_name(v.type);
    return false;
  }
  size_t i = 0;
  while (i < text.size() && is_space(text[i])) ++i;
  std::string why;
  if (!read_number(text, i, out, why)) {
    error = std::string(who) + ": " + (why.empty() ? "\"" + text + "\" is not a number" : why);
    return false;
  }
  while (i < text.size() && is_space(text[i])) ++i;
  if (i != text.size()) {
    error = std::string(who) + ": \"" + text + "\" is not a number";
    return false;
  }
  return true;
}

std::string text_of(const Dec& d) {
  if (d.mag.empty()) return "0";
  std::string s = d.mag;
  if (d.scale > 0) {
    if ((int)s.size() <= d.scale) s.insert(0, (size_t)(d.scale - (int)s.size() + 1), '0');
    s.insert(s.size() - (size_t)d.scale, 1, '.');
  }
  return d.neg ? "-" + s : s;
}

// ---- magnitudes: digit strings with no leading zeros ----

int cmp_mag(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
  int c = a.compare(b);
  return c < 0 ? -1 : c > 0 ? 1 : 0;
}

std::string add_mag(const std::string& a, const std::string& b) {
  std::string r;
  int carry = 0;
  for (size_t k = 0; k < a.size() || k < b.size() || carry; ++k) {
    int s = carry;
    if (k < a.size()) s += a[a.size() - 1 - k] - '0';
    if (k < b.size()) s += b[b.size() - 1 - k] - '0';
    r.insert(r.begin(), (char)('0' + s % 10));
    carry = s / 10;
  }
  return r;
}

// a - b, a >= b
std::string sub_mag(const std::string& a, const std::string& b) {
  std::string r(a.size(), '0');
  int borrow = 0;
  for (size_t k = 0; k < a.size(); ++k) {
    int s = (a[a.size() - 1 - k] - '0') - borrow - (k < b.size() ? b[b.size() - 1 - k] - '0' : 0);
    borrow = s < 0;
    if (s < 0) s += 10;
    r[r.size() - 1 - k] = (char)('0' + s);
  }
  size_t lead = 0;
  while (lead < r.size() && r[lead] == '0') ++lead;
  return r.substr(lead);
}

std::string mul_mag(const std::string& a, const std::string& b) {
  if (a.empty() || b.empty()) return "";
  std::vector<int> acc(a.size() + b.size(), 0);
  for (size_t x = 0; x < a.size(); ++x)
    for (size_t y = 0; y < b.size(); ++y) acc[x + y + 1] += (a[x] - '0') * (b[y] - '0');
  for (size_t k = acc.size() - 1; k > 0; --k) {
    acc[k - 1] += acc[k] / 10;
    acc[k] %= 10;
  }
  std::string r;
  for (int v : acc) if (!(r.empty() && v == 0)) r += (char)('0' + v);
  return r;
}

// quotient and remainder of magnitudes, d non-zero
void divmod_mag(const std::string& n, const std::string& d, std::string& q, std::string& rem) {
  q.clear();
  rem.clear();
  for (char c : n) {
    if (!(rem.empty() && c == '0')) rem += c;
    int digit = 0;
    while (cmp_mag(rem, d) >= 0) {
      rem = sub_mag(rem, d);
      ++digit;
    }
    if (!(q.empty() && digit == 0)) q += (char)('0' + digit);
  }
}

void align(Dec& a, Dec& b) {
  if (a.scale < b.scale) { if (!a.mag.empty()) a.mag.append((size_t)(b.scale - a.scale), '0'); a.scale = b.scale; }
  else if (b.scale < a.scale) { if (!b.mag.empty()) b.mag.append((size_t)(a.scale - b.scale), '0'); b.scale = a.scale; }
}

Dec add(Dec a, Dec b) {
  align(a, b);
  Dec r;
  r.scale = a.scale;
  if (a.neg == b.neg) {
    r.mag = add_mag(a.mag, b.mag);
    r.neg = a.neg;
  } else if (cmp_mag(a.mag, b.mag) >= 0) {
    r.mag = sub_mag(a.mag, b.mag);
    r.neg = a.neg;
  } else {
    r.mag = sub_mag(b.mag, a.mag);
    r.neg = b.neg;
  }
  normalize(r);
  return r;
}

int cmp(Dec a, Dec b) {
  if (a.neg != b.neg) return a.neg ? -1 : 1;
  align(a, b);
  int c = cmp_mag(a.mag, b.mag);
  return a.neg ? -c : c;
}

enum class Mode { HalfUp, HalfEven, Down, Floor, Ceil };

bool mode_of(const std::vector<Value>& args, size_t at, Mode& m, const char* who, std::string& error) {
  m = Mode::HalfUp;
  if (args.size() <= at || args[at].type == Type::Nil) return true;
  if (args[at].type != Type::Str) { error = std::string(who) + ": the mode is a string"; return false; }
  const std::string& s = args[at].str();
  if (s == "half_up") m = Mode::HalfUp;
  else if (s == "half_even") m = Mode::HalfEven;
  else if (s == "down") m = Mode::Down;
  else if (s == "floor") m = Mode::Floor;
  else if (s == "ceil") m = Mode::Ceil;
  else { error = std::string(who) + ": mode is half_up, half_even, down, floor or ceil, not \"" + s + "\""; return false; }
  return true;
}

// q from |n| / |d| with remainder rem: rounded to a whole number by the mode, for a result of sign neg
std::string round_quotient(std::string q, const std::string& rem, const std::string& d, bool neg, Mode m) {
  if (rem.empty()) return q;
  bool up = false;
  const int half = cmp_mag(add_mag(rem, rem), d);
  switch (m) {
    case Mode::HalfUp: up = half >= 0; break;
    case Mode::HalfEven: up = half > 0 || (half == 0 && !q.empty() && (q.back() - '0') % 2 == 1); break;
    case Mode::Down: up = false; break;
    case Mode::Floor: up = neg; break;
    case Mode::Ceil: up = !neg; break;
  }
  return up ? add_mag(q, "1") : q;
}

Dec round_to(const Dec& a, int places, Mode m) {
  if (a.scale <= places) return a;
  const size_t drop = (size_t)(a.scale - places);
  std::string q = a.mag.size() > drop ? a.mag.substr(0, a.mag.size() - drop) : "";
  std::string rem = a.mag.size() > drop ? a.mag.substr(a.mag.size() - drop) : a.mag;
  size_t lead = 0;
  while (lead < rem.size() && rem[lead] == '0') ++lead;
  rem.erase(0, lead);
  Dec r;
  r.neg = a.neg;
  r.mag = round_quotient(q, rem, "1" + std::string(drop, '0'), a.neg, m);
  r.scale = places;
  if (places < 0) {   // whole tens, hundreds...
    if (!r.mag.empty()) r.mag.append((size_t)-places, '0');
    r.scale = 0;
  }
  normalize(r);
  return r;
}

bool places_arg(const Value& v, int& places, const char* who, std::string& error) {
  if (v.type != Type::Int || v.i < -(int64_t)kMaxDigits || v.i > (int64_t)kMaxDigits) {
    error = std::string(who) + ": places is a whole number from -1000 to 1000";
    return false;
  }
  places = (int)v.i;
  return true;
}

Value bi_decimal(Vm&, std::vector<Value>& args, std::string&) {
  Dec d;
  std::string ignored;
  if (!operand(args[0], d, "decimal", ignored)) return Value::nil();
  return Value::string(text_of(d));
}

Value bi_decimal_prefix(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "decimal_prefix")) return Value::nil();
  const std::string& s = args[0].str();
  size_t i = 0;
  while (i < s.size() && is_space(s[i])) ++i;
  Dec d;
  std::string why;
  if (!read_number(s, i, d, why)) {
    if (!why.empty()) error = "decimal_prefix: " + why;
    return Value::nil();
  }
  auto m = std::make_shared<MapObj>();
  m->put("value", Value::string(text_of(d)));
  m->put("rest", Value::string(s.substr(i)));
  return Value::object(Type::Map, m);
}

Value bi_dec_add(Vm&, std::vector<Value>& args, std::string& error) {
  Dec a, b;
  if (!operand(args[0], a, "dec_add", error) || !operand(args[1], b, "dec_add", error)) return Value::nil();
  return Value::string(text_of(add(a, b)));
}

Value bi_dec_sub(Vm&, std::vector<Value>& args, std::string& error) {
  Dec a, b;
  if (!operand(args[0], a, "dec_sub", error) || !operand(args[1], b, "dec_sub", error)) return Value::nil();
  b.neg = !b.neg;
  normalize(b);
  return Value::string(text_of(add(a, b)));
}

Value bi_dec_mul(Vm&, std::vector<Value>& args, std::string& error) {
  Dec a, b;
  if (!operand(args[0], a, "dec_mul", error) || !operand(args[1], b, "dec_mul", error)) return Value::nil();
  Dec r;
  r.neg = a.neg != b.neg;
  r.mag = mul_mag(a.mag, b.mag);
  r.scale = a.scale + b.scale;
  if (r.mag.size() > kMaxDigits) { error = "dec_mul: a result of more than 1000 digits"; return Value::nil(); }
  normalize(r);
  return Value::string(text_of(r));
}

Value bi_dec_div(Vm&, std::vector<Value>& args, std::string& error) {
  Dec a, b;
  int places = 0;
  Mode m;
  if (!operand(args[0], a, "dec_div", error) || !operand(args[1], b, "dec_div", error)) return Value::nil();
  if (!places_arg(args[2], places, "dec_div", error) || !mode_of(args, 3, m, "dec_div", error)) return Value::nil();
  if (b.mag.empty()) { error = "dec_div: division by zero"; return Value::nil(); }
  if (places < 0) { error = "dec_div: places is 0 or more"; return Value::nil(); }
  // a/b * 10^places = (A * 10^(b.scale + places)) / (B * 10^a.scale)
  std::string n = a.mag.empty() ? "" : a.mag + std::string((size_t)(b.scale + places), '0');
  std::string d = b.mag + std::string((size_t)a.scale, '0');
  if (n.size() > 3 * kMaxDigits) { error = "dec_div: a result of more than 1000 digits"; return Value::nil(); }
  std::string q, rem;
  divmod_mag(n, d, q, rem);
  Dec r;
  r.neg = a.neg != b.neg;
  r.mag = round_quotient(q, rem, d, r.neg, m);
  r.scale = places;
  normalize(r);
  return Value::string(text_of(r));
}

Value bi_dec_cmp(Vm&, std::vector<Value>& args, std::string& error) {
  Dec a, b;
  if (!operand(args[0], a, "dec_cmp", error) || !operand(args[1], b, "dec_cmp", error)) return Value::nil();
  return Value::integer(cmp(a, b));
}

Value bi_dec_round(Vm&, std::vector<Value>& args, std::string& error) {
  Dec a;
  int places = 0;
  Mode m;
  if (!operand(args[0], a, "dec_round", error)) return Value::nil();
  if (args.size() > 1 && !places_arg(args[1], places, "dec_round", error)) return Value::nil();
  if (!mode_of(args, 2, m, "dec_round", error)) return Value::nil();
  return Value::string(text_of(round_to(a, places, m)));
}

Value bi_dec_round_sig(Vm&, std::vector<Value>& args, std::string& error) {
  Dec a;
  if (!operand(args[0], a, "dec_round_sig", error)) return Value::nil();
  if (args[1].type != Type::Int || args[1].i < 1 || args[1].i > (int64_t)kMaxDigits) {
    error = "dec_round_sig: digits is a whole number from 1 to 1000";
    return Value::nil();
  }
  const int digits = (int)args[1].i;
  if ((int)a.mag.size() <= digits) return Value::string(text_of(a));
  // the first digit sits at 10^(size - scale - 1); keep `digits` of them
  const int places = digits - ((int)a.mag.size() - a.scale);
  return Value::string(text_of(round_to(a, places, Mode::HalfUp)));
}

Value bi_dec_format(Vm&, std::vector<Value>& args, std::string& error) {
  Dec a;
  int places = 0;
  if (!operand(args[0], a, "dec_format", error)) return Value::nil();
  if (args.size() > 1 && !places_arg(args[1], places, "dec_format", error)) return Value::nil();
  if (places < 0) { error = "dec_format: places is 0 or more"; return Value::nil(); }
  Dec r = round_to(a, places, Mode::HalfUp);
  std::string s = text_of(r);
  if (places > 0) {
    if (r.scale == 0) s += ".";
    s.append((size_t)(places - r.scale), '0');
  }
  return Value::string(s);
}

}  // namespace

void install_decimal_builtins(Vm& vm) {
  vm.define_native("decimal", 1, 1, bi_decimal);
  vm.define_native("decimal_prefix", 1, 1, bi_decimal_prefix);
  vm.define_native("dec_add", 2, 2, bi_dec_add);
  vm.define_native("dec_sub", 2, 2, bi_dec_sub);
  vm.define_native("dec_mul", 2, 2, bi_dec_mul);
  vm.define_native("dec_div", 3, 4, bi_dec_div);
  vm.define_native("dec_cmp", 2, 2, bi_dec_cmp);
  vm.define_native("dec_round", 1, 3, bi_dec_round);
  vm.define_native("dec_round_sig", 2, 2, bi_dec_round_sig);
  vm.define_native("dec_format", 1, 2, bi_dec_format);
}

}  // namespace aios::userland::grain
