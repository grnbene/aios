// Number, Math and the global functions (parseInt, URI coding, eval, ...).
#include <algorithm>
#include "aios/core/numparse.hpp"
#include "aios/hal/host.hpp"
#include <cmath>
#include <cstdint>
#include <cstring>

#include "builtins.hpp"
#include "internal_num.hpp"

namespace aios::js {

namespace {

// ---------------------------------------------------------------------------------------------------
// exact decimal expansion of a double, for toFixed / toExponential / toPrecision
// ---------------------------------------------------------------------------------------------------

// A little unsigned big integer, little-endian base 2^32.
struct Big {
  std::vector<uint32_t> w;
  void mul_small(uint32_t m) {
    uint64_t carry = 0;
    for (uint32_t& x : w) {
      uint64_t v = static_cast<uint64_t>(x) * m + carry;
      x = static_cast<uint32_t>(v);
      carry = v >> 32;
    }
    if (carry) w.push_back(static_cast<uint32_t>(carry));
  }
  void shl(int bits) {
    int words = bits / 32, rem = bits % 32;
    if (rem) {
      uint32_t carry = 0;
      for (uint32_t& x : w) {
        uint32_t nc = x >> (32 - rem);
        x = (x << rem) | carry;
        carry = nc;
      }
      if (carry) w.push_back(carry);
    }
    if (words) w.insert(w.begin(), static_cast<size_t>(words), 0u);
  }
  uint32_t divmod_small(uint32_t d) {
    uint64_t rem = 0;
    for (size_t i = w.size(); i-- > 0;) {
      uint64_t v = (rem << 32) | w[i];
      w[i] = static_cast<uint32_t>(v / d);
      rem = v % d;
    }
    while (!w.empty() && w.back() == 0) w.pop_back();
    return static_cast<uint32_t>(rem);
  }
  bool zero() const { return w.empty(); }
};

// value = 0.DIGITS x 10^n, exactly (no rounding); digits have no leading or trailing zeros. d > 0 and finite.
void exact_digits(double d, std::string* digits, int* n) {
  uint64_t bits;
  std::memcpy(&bits, &d, 8);
  int exp = static_cast<int>((bits >> 52) & 0x7FF);
  uint64_t mant = bits & ((1ull << 52) - 1);
  if (exp == 0) exp = 1;
  else mant |= 1ull << 52;
  exp -= 1075;   // value = mant * 2^exp
  std::string int_part, frac_part;
  if (exp >= 0) {
    Big num;
    num.w = {static_cast<uint32_t>(mant), static_cast<uint32_t>(mant >> 32)};
    while (!num.w.empty() && num.w.back() == 0) num.w.pop_back();
    num.shl(exp);
    std::string rev;
    while (!num.zero()) rev.push_back(static_cast<char>('0' + num.divmod_small(10)));
    int_part.assign(rev.rbegin(), rev.rend());
  } else {
    const int k = -exp;
    const uint64_t ip = k < 64 ? mant >> k : 0;
    const uint64_t fr = k < 64 ? mant & ((1ull << k) - 1) : mant;
    int_part = ip ? std::to_string(ip) : std::string();
    Big f;
    f.w = {static_cast<uint32_t>(fr), static_cast<uint32_t>(fr >> 32)};
    while (!f.w.empty() && f.w.back() == 0) f.w.pop_back();
    const size_t kw = static_cast<size_t>(k / 32);
    const int kb = k % 32;
    while (!f.zero()) {
      f.mul_small(10);
      uint32_t digit = 0;
      if (f.w.size() > kw) {
        uint64_t v = f.w[kw] >> kb;
        if (kb && f.w.size() > kw + 1) v |= static_cast<uint64_t>(f.w[kw + 1]) << (32 - kb);
        digit = static_cast<uint32_t>(v);
        f.w.resize(kw + 1);
        f.w[kw] &= kb ? ((1u << kb) - 1) : 0u;
        while (!f.w.empty() && f.w.back() == 0) f.w.pop_back();
      }
      frac_part.push_back(static_cast<char>('0' + digit));
    }
  }
  std::string all = int_part + frac_part;
  int point = static_cast<int>(int_part.size());
  size_t lead = 0;
  while (lead < all.size() && all[lead] == '0') ++lead;
  all.erase(0, lead);
  point -= static_cast<int>(lead);
  while (!all.empty() && all.back() == '0') all.pop_back();
  *digits = all;
  *n = point;
}

// Keep `keep` leading digits of `digits`, rounding half up on the next digit. A carry out of the top digit
// (999 -> 1000) raises `*n` and keeps the digit count at `keep`. keep may be 0.
std::string round_digits(const std::string& digits, int* n, int keep) {
  const bool up = keep >= 0 && static_cast<size_t>(keep) < digits.size() && digits[static_cast<size_t>(keep)] >= '5';
  std::string out = digits.substr(0, std::min<size_t>(static_cast<size_t>(keep < 0 ? 0 : keep), digits.size()));
  while (static_cast<int>(out.size()) < keep) out.push_back('0');
  if (!up) return out;
  int i = static_cast<int>(out.size()) - 1;
  while (i >= 0 && out[static_cast<size_t>(i)] == '9') { out[static_cast<size_t>(i)] = '0'; --i; }
  if (i >= 0) {
    ++out[static_cast<size_t>(i)];
  } else {
    out.insert(out.begin(), '1');
    ++*n;
    if (keep > 0) out.pop_back();
  }
  return out;
}

std::string to_fixed(double x, int f) {
  std::string sign;
  if (x < 0) { sign = "-"; x = -x; }
  std::string digits;
  int n = 0;
  std::string N = "0";
  if (x != 0) {
    exact_digits(x, &digits, &n);
    const int m = n + f;   // digits in the integer x * 10^f
    if (m >= 0) {
      int nn = m;
      N = round_digits(digits, &nn, m);
      while (static_cast<int>(N.size()) < nn) N.push_back('0');
      if (N.empty()) N = "0";
    }
  }
  if (static_cast<int>(N.size()) < f + 1) N.insert(0, static_cast<size_t>(f + 1) - N.size(), '0');
  std::string out = N.substr(0, N.size() - static_cast<size_t>(f));
  if (f > 0) out += "." + N.substr(N.size() - static_cast<size_t>(f));
  return sign + out;
}

std::string exp_string(const std::string& digits, int e) {
  std::string out;
  out += digits[0];
  if (digits.size() > 1) out += "." + digits.substr(1);
  out += "e";
  out += e < 0 ? "-" : "+";
  out += std::to_string(e < 0 ? -e : e);
  return out;
}

std::string to_exponential(double x, int f, bool shortest) {
  std::string sign;
  if (x < 0) { sign = "-"; x = -x; }
  if (x == 0) {
    std::string d = "0";
    if (!shortest && f > 0) d += std::string(static_cast<size_t>(f), '0');
    return sign + exp_string(d, 0);
  }
  std::string digits;
  int n;
  if (shortest) {
    char buf[64];
    for (int prec = 1; prec <= 17; ++prec) {
      std::snprintf(buf, sizeof buf, "%.*e", prec - 1, x);
      if (aios::hal::parse_double(buf, nullptr) == x || prec == 17) break;
    }
    std::string s(buf);
    size_t e = s.find('e');
    for (size_t i = 0; i < e; ++i) if (s[i] >= '0' && s[i] <= '9') digits.push_back(s[i]);
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    n = aios::num::to_int(s.c_str() + e + 1) + 1;
  } else {
    exact_digits(x, &digits, &n);
    digits = round_digits(digits, &n, f + 1);
    while (static_cast<int>(digits.size()) < f + 1) digits.push_back('0');
  }
  return sign + exp_string(digits, n - 1);
}

std::string to_precision(double x, int p) {
  std::string sign;
  if (x < 0) { sign = "-"; x = -x; }
  std::string digits;
  int n;
  if (x == 0) {
    digits = std::string(static_cast<size_t>(p), '0');
    n = 1;
  } else {
    exact_digits(x, &digits, &n);
    digits = round_digits(digits, &n, p);
    while (static_cast<int>(digits.size()) < p) digits.push_back('0');
  }
  const int e = n - 1;
  if (x != 0 && (e < -6 || e >= p)) return sign + exp_string(digits, e);
  if (x == 0) {
    std::string out = "0";
    if (p > 1) out += "." + std::string(static_cast<size_t>(p - 1), '0');
    return sign + out;
  }
  if (e >= 0) {
    std::string ip = digits.substr(0, static_cast<size_t>(e + 1));
    std::string fp = digits.substr(static_cast<size_t>(e + 1));
    return sign + ip + (fp.empty() ? "" : "." + fp);
  }
  return sign + "0." + std::string(static_cast<size_t>(-(e + 1)), '0') + digits;
}

// ---------------------------------------------------------------------------------------------------
// Number
// ---------------------------------------------------------------------------------------------------

Value number_ctor(Context& ctx, CallArgs& a) {
  double n = 0;
  if (a.argc > 0) {
    Value prim;
    JS_TRY(ctx.to_numeric(a[0], &prim));
    n = prim.is_bigint() ? big::to_double(prim.as_bigint()->n) : prim.num;
  }
  if (!a.is_construct()) return Value::Number(n);
  Object* proto;
  JS_TRY(ctx.prototype_from_ctor(a.new_target, ctx.intr.number_proto, &proto));
  PrimitiveObject* o = ctx.heap.make<PrimitiveObject>(Class::Number, Value::Number(n));
  o->proto = proto;
  return Value::Obj(o);
}

Value number_is_finite(Context&, CallArgs& a) { return Value::Bool(a[0].is_number() && std::isfinite(a[0].num)); }
Value number_is_nan(Context&, CallArgs& a) { return Value::Bool(a[0].is_number() && std::isnan(a[0].num)); }
Value number_is_integer(Context&, CallArgs& a) { return Value::Bool(a[0].is_number() && std::isfinite(a[0].num) && std::trunc(a[0].num) == a[0].num); }
Value number_is_safe_integer(Context&, CallArgs& a) {
  return Value::Bool(a[0].is_number() && std::isfinite(a[0].num) && std::trunc(a[0].num) == a[0].num && std::fabs(a[0].num) <= 9007199254740991.0);
}

Value number_to_string(Context& ctx, CallArgs& a) {
  double x;
  JS_TRY(this_number_value(ctx, a.this_value, "Number.prototype.toString", &x));
  int radix = 10;
  if (!a[0].is_undefined()) {
    double r;
    JS_TRY(ctx.to_integer(a[0], &r));
    if (r < 2 || r > 36) {
      ctx.throw_range_error("toString() radix must be between 2 and 36");
      return Value::Exception();
    }
    radix = static_cast<int>(r);
  }
  return Value::String(ctx.number_to_string(x, radix));
}

Value number_to_locale_string(Context& ctx, CallArgs& a) {
  double x;
  JS_TRY(this_number_value(ctx, a.this_value, "Number.prototype.toLocaleString", &x));
  if (!std::isfinite(x)) return Value::String(ctx.number_to_string(x));
  std::string s = to_fixed(std::fabs(x), 3);
  size_t dot = s.find('.');
  std::string ip = s.substr(0, dot), fp = s.substr(dot + 1);
  while (!fp.empty() && fp.back() == '0') fp.pop_back();
  std::string grouped;
  for (size_t i = 0; i < ip.size(); ++i) {
    if (i && (ip.size() - i) % 3 == 0) grouped += ',';
    grouped += ip[i];
  }
  return Str(ctx, std::string(x < 0 ? "-" : "") + grouped + (fp.empty() ? "" : "." + fp));
}

Value number_value_of(Context& ctx, CallArgs& a) {
  double x;
  JS_TRY(this_number_value(ctx, a.this_value, "Number.prototype.valueOf", &x));
  return Value::Number(x);
}

Value number_to_fixed(Context& ctx, CallArgs& a) {
  double x;
  JS_TRY(this_number_value(ctx, a.this_value, "Number.prototype.toFixed", &x));
  double f;
  JS_TRY(ctx.to_integer(a[0], &f));
  if (f < 0 || f > 100) {
    ctx.throw_range_error("toFixed() digits argument must be between 0 and 100");
    return Value::Exception();
  }
  if (!std::isfinite(x) || std::fabs(x) >= 1e21) return Value::String(ctx.number_to_string(x));
  return Str(ctx, to_fixed(x, static_cast<int>(f)));
}

Value number_to_exponential(Context& ctx, CallArgs& a) {
  double x;
  JS_TRY(this_number_value(ctx, a.this_value, "Number.prototype.toExponential", &x));
  double f;
  JS_TRY(ctx.to_integer(a[0], &f));
  if (!std::isfinite(x)) return Value::String(ctx.number_to_string(x));
  if (f < 0 || f > 100) {
    ctx.throw_range_error("toExponential() argument must be between 0 and 100");
    return Value::Exception();
  }
  return Str(ctx, to_exponential(x, static_cast<int>(f), a[0].is_undefined()));
}

Value number_to_precision(Context& ctx, CallArgs& a) {
  double x;
  JS_TRY(this_number_value(ctx, a.this_value, "Number.prototype.toPrecision", &x));
  if (a[0].is_undefined()) return Value::String(ctx.number_to_string(x));
  double p;
  JS_TRY(ctx.to_integer(a[0], &p));
  if (!std::isfinite(x)) return Value::String(ctx.number_to_string(x));
  if (p < 1 || p > 100) {
    ctx.throw_range_error("toPrecision() argument must be between 1 and 100");
    return Value::Exception();
  }
  return Str(ctx, to_precision(x, static_cast<int>(p)));
}

// ---------------------------------------------------------------------------------------------------
// parseInt / parseFloat
// ---------------------------------------------------------------------------------------------------

size_t skip_ws(const std::u16string& s) {
  size_t i = 0;
  while (i < s.size() && is_js_whitespace(s[i])) ++i;
  return i;
}

Value parse_int(Context& ctx, CallArgs& a) {
  JsString* js;
  JS_TRY(ctx.to_string(a[0], &js));
  const std::u16string& s = js->str();
  int32_t radix;
  JS_TRY(ctx.to_int32(a[1], &radix));
  size_t i = skip_ws(s);
  bool neg = false;
  if (i < s.size() && (s[i] == u'+' || s[i] == u'-')) { neg = s[i] == u'-'; ++i; }
  bool strip_prefix = true;
  if (radix != 0) {
    if (radix < 2 || radix > 36) return Value::Number(NAN);
    if (radix != 16) strip_prefix = false;
  } else {
    radix = 10;
  }
  if (strip_prefix && i + 1 < s.size() && s[i] == u'0' && (s[i + 1] == u'x' || s[i + 1] == u'X')) {
    i += 2;
    radix = 16;
  }
  size_t start = i;
  std::string digits;
  for (; i < s.size(); ++i) {
    char16_t c = s[i];
    int d;
    if (c >= u'0' && c <= u'9') d = c - u'0';
    else if (c >= u'a' && c <= u'z') d = c - u'a' + 10;
    else if (c >= u'A' && c <= u'Z') d = c - u'A' + 10;
    else break;
    if (d >= radix) break;
    digits.push_back(static_cast<char>(c));
  }
  if (i == start) return Value::Number(NAN);
  double v = 0;
  if (radix == 10) {
    v = aios::hal::parse_double(digits.c_str(), nullptr);
  } else {
    for (char c : digits) {
      int d = c >= '0' && c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
      v = v * radix + d;
    }
  }
  return Value::Number(neg ? -v : v);
}

Value parse_float(Context& ctx, CallArgs& a) {
  JsString* js;
  JS_TRY(ctx.to_string(a[0], &js));
  const std::u16string& s = js->str();
  size_t i = skip_ws(s);
  std::string t;
  size_t j = i;
  if (j < s.size() && (s[j] == u'+' || s[j] == u'-')) t.push_back(static_cast<char>(s[j++]));
  static const char16_t inf[] = u"Infinity";
  if (s.compare(j, 8, inf) == 0) return Value::Number(t == "-" ? -INFINITY : INFINITY);
  size_t digits_start = j;
  bool any = false;
  while (j < s.size() && s[j] >= u'0' && s[j] <= u'9') { t.push_back(static_cast<char>(s[j++])); any = true; }
  if (j < s.size() && s[j] == u'.') {
    size_t save = j;
    t.push_back('.');
    ++j;
    bool frac = false;
    while (j < s.size() && s[j] >= u'0' && s[j] <= u'9') { t.push_back(static_cast<char>(s[j++])); frac = true; }
    if (!any && !frac) { t.resize(t.size() - 1); j = save; }
    any = any || frac;
  }
  if (!any) { (void)digits_start; return Value::Number(NAN); }
  if (j < s.size() && (s[j] == u'e' || s[j] == u'E')) {
    size_t k = j + 1;
    std::string e = "e";
    if (k < s.size() && (s[k] == u'+' || s[k] == u'-')) e.push_back(static_cast<char>(s[k++]));
    size_t ds = k;
    while (k < s.size() && s[k] >= u'0' && s[k] <= u'9') e.push_back(static_cast<char>(s[k++]));
    if (k > ds) t += e;
  }
  double v;
  if (!parse_decimal_ascii(t, &v)) return Value::Number(NAN);
  return Value::Number(v);
}

Value global_is_nan(Context& ctx, CallArgs& a) {
  double d;
  JS_TRY(ctx.to_number(a[0], &d));
  return Value::Bool(std::isnan(d));
}

Value global_is_finite(Context& ctx, CallArgs& a) {
  double d;
  JS_TRY(ctx.to_number(a[0], &d));
  return Value::Bool(std::isfinite(d));
}

// ---------------------------------------------------------------------------------------------------
// URI coding
// ---------------------------------------------------------------------------------------------------

const char* const kUriReserved = ";/?:@&=+$,#";
const char* const kUriUnescapedExtra = "-_.!~*'()";

bool is_alnum16(char16_t c) { return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9'); }

Value uri_encode(Context& ctx, const Value& v, bool component) {
  JsString* js;
  JS_TRY(ctx.to_string(v, &js));
  const std::u16string& s = js->str();
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    char16_t c = s[i];
    if (c < 128 && (is_alnum16(c) || std::strchr(kUriUnescapedExtra, static_cast<char>(c)) || (!component && std::strchr(kUriReserved, static_cast<char>(c))))) {
      out.push_back(static_cast<char>(c));
      continue;
    }
    uint32_t cp = c;
    if (c >= 0xDC00 && c <= 0xDFFF) {
      ctx.throw_error(ErrorKind::URIError, "URI malformed");
      return Value::Exception();
    }
    if (c >= 0xD800 && c <= 0xDBFF) {
      if (i + 1 >= s.size() || s[i + 1] < 0xDC00 || s[i + 1] > 0xDFFF) {
        ctx.throw_error(ErrorKind::URIError, "URI malformed");
        return Value::Exception();
      }
      cp = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
      ++i;
    }
    unsigned char b[4];
    int n;
    if (cp < 0x80) { b[0] = static_cast<unsigned char>(cp); n = 1; }
    else if (cp < 0x800) { b[0] = static_cast<unsigned char>(0xC0 | (cp >> 6)); b[1] = static_cast<unsigned char>(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { b[0] = static_cast<unsigned char>(0xE0 | (cp >> 12)); b[1] = static_cast<unsigned char>(0x80 | ((cp >> 6) & 0x3F)); b[2] = static_cast<unsigned char>(0x80 | (cp & 0x3F)); n = 3; }
    else { b[0] = static_cast<unsigned char>(0xF0 | (cp >> 18)); b[1] = static_cast<unsigned char>(0x80 | ((cp >> 12) & 0x3F)); b[2] = static_cast<unsigned char>(0x80 | ((cp >> 6) & 0x3F)); b[3] = static_cast<unsigned char>(0x80 | (cp & 0x3F)); n = 4; }
    for (int k = 0; k < n; ++k) {
      static const char hex[] = "0123456789ABCDEF";
      out.push_back('%');
      out.push_back(hex[b[k] >> 4]);
      out.push_back(hex[b[k] & 15]);
    }
  }
  return Str(ctx, out);
}

int hex_val(char16_t c) {
  if (c >= u'0' && c <= u'9') return c - u'0';
  if (c >= u'a' && c <= u'f') return c - u'a' + 10;
  if (c >= u'A' && c <= u'F') return c - u'A' + 10;
  return -1;
}

Value uri_decode(Context& ctx, const Value& v, bool component) {
  JsString* js;
  JS_TRY(ctx.to_string(v, &js));
  const std::u16string& s = js->str();
  std::u16string out;
  auto bad = [&]() {
    ctx.throw_error(ErrorKind::URIError, "URI malformed");
    return Value::Exception();
  };
  // the byte spelled %XX at s[pos], or -1
  auto byte_at = [&](size_t pos) -> int {
    if (pos + 2 >= s.size()) return -1;
    if (s[pos] != u'%') return -1;
    int h1 = hex_val(s[pos + 1]), h2 = hex_val(s[pos + 2]);
    if (h1 < 0 || h2 < 0) return -1;
    return h1 * 16 + h2;
  };
  for (size_t i = 0; i < s.size(); ++i) {
    char16_t c = s[i];
    if (c != u'%') { out.push_back(c); continue; }
    int first = byte_at(i);
    if (first < 0) return bad();
    const unsigned b = static_cast<unsigned>(first);
    const size_t start = i;
    i += 2;
    if (b < 0x80) {
      const char16_t ch = static_cast<char16_t>(b);
      if (!component && std::strchr(kUriReserved, static_cast<char>(ch)) && ch != 0) out.append(s, start, 3);
      else out.push_back(ch);
      continue;
    }
    int n;
    uint32_t cp;
    if ((b & 0xE0) == 0xC0) { n = 2; cp = b & 0x1F; }
    else if ((b & 0xF0) == 0xE0) { n = 3; cp = b & 0x0F; }
    else if ((b & 0xF8) == 0xF0) { n = 4; cp = b & 0x07; }
    else return bad();
    for (int k = 1; k < n; ++k) {
      int cb = byte_at(i + 1);
      if (cb < 0 || (cb & 0xC0) != 0x80) return bad();
      cp = (cp << 6) | static_cast<uint32_t>(cb & 0x3F);
      i += 3;
    }
    if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return bad();
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
      out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
    } else {
      out.push_back(static_cast<char16_t>(cp));
    }
  }
  return StrV(ctx, std::move(out));
}

Value global_encode_uri(Context& ctx, CallArgs& a) { return uri_encode(ctx, a[0], false); }
Value global_encode_uri_component(Context& ctx, CallArgs& a) { return uri_encode(ctx, a[0], true); }
Value global_decode_uri(Context& ctx, CallArgs& a) { return uri_decode(ctx, a[0], false); }
Value global_decode_uri_component(Context& ctx, CallArgs& a) { return uri_decode(ctx, a[0], true); }

Value global_escape(Context& ctx, CallArgs& a) {
  JsString* js;
  JS_TRY(ctx.to_string(a[0], &js));
  std::string out;
  static const char hex[] = "0123456789ABCDEF";
  for (char16_t c : js->str()) {
    if (c < 128 && (is_alnum16(c) || std::strchr("@*_+-./", static_cast<char>(c)))) out.push_back(static_cast<char>(c));
    else if (c < 256) { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    else { out += "%u"; out += hex[c >> 12]; out += hex[(c >> 8) & 15]; out += hex[(c >> 4) & 15]; out += hex[c & 15]; }
  }
  return Str(ctx, out);
}

Value global_unescape(Context& ctx, CallArgs& a) {
  JsString* js;
  JS_TRY(ctx.to_string(a[0], &js));
  const std::u16string& s = js->str();
  std::u16string out;
  for (size_t i = 0; i < s.size(); ++i) {
    char16_t c = s[i];
    if (c == u'%') {
      if (i + 5 < s.size() + 0 && s[i + 1] == u'u') {
        int h[4] = {hex_val(s[i + 2]), hex_val(s[i + 3]), hex_val(s[i + 4]), hex_val(s[i + 5])};
        if (h[0] >= 0 && h[1] >= 0 && h[2] >= 0 && h[3] >= 0) {
          out.push_back(static_cast<char16_t>((h[0] << 12) | (h[1] << 8) | (h[2] << 4) | h[3]));
          i += 5;
          continue;
        }
      }
      if (i + 2 < s.size()) {
        int h1 = hex_val(s[i + 1]), h2 = hex_val(s[i + 2]);
        if (h1 >= 0 && h2 >= 0) {
          out.push_back(static_cast<char16_t>(h1 * 16 + h2));
          i += 2;
          continue;
        }
      }
    }
    out.push_back(c);
  }
  return StrV(ctx, std::move(out));
}

// Indirect eval: the source runs as a global script and yields its completion value.
Value global_eval(Context& ctx, CallArgs& a) {
  if (!a[0].is_string()) return a[0];
  Value r;
  JS_TRY(vm_eval_source(ctx, a[0].as_string()->str(), "<eval>", false, &r));
  return r;
}

// ---------------------------------------------------------------------------------------------------
// Math
// ---------------------------------------------------------------------------------------------------

#define MATH1(fn_name, expr)                                  \
  Value fn_name(Context& ctx, CallArgs& a) {                  \
    double x;                                                 \
    JS_TRY(ctx.to_number(a[0], &x));                          \
    return Value::Number(expr);                               \
  }

MATH1(math_abs, std::fabs(x))
MATH1(math_acos, std::acos(x))
MATH1(math_acosh, std::acosh(x))
MATH1(math_asin, std::asin(x))
MATH1(math_asinh, std::asinh(x))
MATH1(math_atan, std::atan(x))
MATH1(math_atanh, std::atanh(x))
MATH1(math_cbrt, std::cbrt(x))
MATH1(math_ceil, std::ceil(x))
MATH1(math_cos, std::cos(x))
MATH1(math_cosh, std::cosh(x))
MATH1(math_exp, std::exp(x))
MATH1(math_expm1, std::expm1(x))
MATH1(math_floor, std::floor(x))
MATH1(math_fround, static_cast<double>(static_cast<float>(x)))
MATH1(math_log, std::log(x))
MATH1(math_log1p, std::log1p(x))
MATH1(math_log10, std::log10(x))
MATH1(math_log2, std::log2(x))
MATH1(math_sin, std::sin(x))
MATH1(math_sinh, std::sinh(x))
MATH1(math_sqrt, std::sqrt(x))
MATH1(math_tan, std::tan(x))
MATH1(math_tanh, std::tanh(x))
MATH1(math_trunc, std::trunc(x))
#undef MATH1

Value math_sign(Context& ctx, CallArgs& a) {
  double x;
  JS_TRY(ctx.to_number(a[0], &x));
  if (std::isnan(x) || x == 0) return Value::Number(x);
  return Value::Number(x > 0 ? 1 : -1);
}

Value math_round(Context& ctx, CallArgs& a) {
  double x;
  JS_TRY(ctx.to_number(a[0], &x));
  if (!std::isfinite(x) || x == 0) return Value::Number(x);
  if (x > 0 && x < 0.5) return Value::Number(0);
  if (x < 0 && x >= -0.5) return Value::Number(-0.0);
  if (std::fabs(x) >= 4503599627370496.0) return Value::Number(x);
  double r = std::floor(x);
  if (x - r >= 0.5) r += 1;
  return Value::Number(r);
}

Value math_atan2(Context& ctx, CallArgs& a) {
  double y, x;
  JS_TRY(ctx.to_number(a[0], &y));
  JS_TRY(ctx.to_number(a[1], &x));
  return Value::Number(std::atan2(y, x));
}

Value math_pow(Context& ctx, CallArgs& a) {
  double x, y;
  JS_TRY(ctx.to_number(a[0], &x));
  JS_TRY(ctx.to_number(a[1], &y));
  if (std::isnan(y)) return Value::Number(NAN);
  if (y == 0) return Value::Number(1);
  if ((x == 1 || x == -1) && std::isinf(y)) return Value::Number(NAN);
  return Value::Number(std::pow(x, y));
}

Value math_clz32(Context& ctx, CallArgs& a) {
  uint32_t x;
  JS_TRY(ctx.to_uint32(a[0], &x));
  int n = 0;
  if (x == 0) n = 32;
  else while (!(x & 0x80000000u)) { x <<= 1; ++n; }
  return Value::Number(n);
}

Value math_imul(Context& ctx, CallArgs& a) {
  uint32_t x, y;
  JS_TRY(ctx.to_uint32(a[0], &x));
  JS_TRY(ctx.to_uint32(a[1], &y));
  return Value::Number(static_cast<double>(static_cast<int32_t>(x * y)));
}

Value math_hypot(Context& ctx, CallArgs& a) {
  double sum = 0, mx = 0;
  bool inf = false, nan = false;
  std::vector<double> v;
  for (int i = 0; i < a.argc; ++i) {
    double x;
    JS_TRY(ctx.to_number(a.argv[i], &x));
    x = std::fabs(x);
    if (std::isinf(x)) inf = true;
    if (std::isnan(x)) nan = true;
    v.push_back(x);
    mx = std::max(mx, x);
  }
  if (inf) return Value::Number(INFINITY);
  if (nan) return Value::Number(NAN);
  if (mx == 0) return Value::Number(0);
  for (double x : v) sum += (x / mx) * (x / mx);
  return Value::Number(std::sqrt(sum) * mx);
}

Value math_max(Context& ctx, CallArgs& a) {
  double r = -INFINITY;
  bool nan = false;
  for (int i = 0; i < a.argc; ++i) {
    double x;
    JS_TRY(ctx.to_number(a.argv[i], &x));
    if (std::isnan(x)) nan = true;
    else if (x > r || (x == 0 && r == 0 && !std::signbit(x))) r = x;
  }
  return Value::Number(nan ? NAN : r);
}

Value math_min(Context& ctx, CallArgs& a) {
  double r = INFINITY;
  bool nan = false;
  for (int i = 0; i < a.argc; ++i) {
    double x;
    JS_TRY(ctx.to_number(a.argv[i], &x));
    if (std::isnan(x)) nan = true;
    else if (x < r || (x == 0 && r == 0 && std::signbit(x))) r = x;
  }
  return Value::Number(nan ? NAN : r);
}

uint64_t g_rng_state[2] = {0x9E3779B97F4A7C15ull, 0xD1B54A32D192ED03ull};

Value math_random(Context& ctx, CallArgs&) {
  if (ctx.random_double) return Value::Number(ctx.random_double());
  uint64_t s1 = g_rng_state[0];
  const uint64_t s0 = g_rng_state[1];
  g_rng_state[0] = s0;
  s1 ^= s1 << 23;
  g_rng_state[1] = s1 ^ s0 ^ (s1 >> 18) ^ (s0 >> 5);
  const uint64_t r = g_rng_state[1] + s0;
  return Value::Number(static_cast<double>(r >> 11) / 9007199254740992.0);
}

}  // namespace

void setup_number(Context& ctx) {
  PrimitiveObject* proto = ctx.heap.make<PrimitiveObject>(Class::Number, Value::Number(0));
  proto->proto = ctx.intr.object_proto;
  ctx.intr.number_proto = proto;
  Function* ctor = new_ctor(ctx, "Number", number_ctor, 1, proto);
  ctx.define_method(ctor, "isFinite", number_is_finite, 1);
  ctx.define_method(ctor, "isInteger", number_is_integer, 1);
  ctx.define_method(ctor, "isNaN", number_is_nan, 1);
  ctx.define_method(ctor, "isSafeInteger", number_is_safe_integer, 1);
  Function* pf = ctx.define_method(ctor, "parseFloat", parse_float, 1);
  Function* pi = ctx.define_method(ctor, "parseInt", parse_int, 2);
  ctx.define_data(ctx.intr.global, ctx.atom_ascii("parseFloat"), Value::Obj(pf), kHiddenAttrs);
  ctx.define_data(ctx.intr.global, ctx.atom_ascii("parseInt"), Value::Obj(pi), kHiddenAttrs);
  struct C { const char* n; double v; } consts[] = {{"EPSILON", 2.220446049250313e-16}, {"MAX_SAFE_INTEGER", 9007199254740991.0},
                                                    {"MIN_SAFE_INTEGER", -9007199254740991.0}, {"MAX_VALUE", 1.7976931348623157e308},
                                                    {"MIN_VALUE", 5e-324}, {"NaN", NAN}, {"NEGATIVE_INFINITY", -INFINITY}, {"POSITIVE_INFINITY", INFINITY}};
  for (const C& c : consts) ctx.define_data(ctor, ctx.atom_ascii(c.n), Value::Number(c.v), 0);
  ctx.define_method(proto, "toExponential", number_to_exponential, 1);
  ctx.define_method(proto, "toFixed", number_to_fixed, 1);
  ctx.define_method(proto, "toLocaleString", number_to_locale_string, 0);
  ctx.define_method(proto, "toPrecision", number_to_precision, 1);
  ctx.define_method(proto, "toString", number_to_string, 1);
  ctx.define_method(proto, "valueOf", number_value_of, 0);
}

void setup_math(Context& ctx) {
  Object* m = ctx.new_object();
  m->cls = Class::Math;
  ctx.define_data(ctx.intr.global, ctx.atom_ascii("Math"), Value::Obj(m), kHiddenAttrs);
  struct F { const char* n; NativeFn fn; int len; } fns[] = {
      {"abs", math_abs, 1}, {"acos", math_acos, 1}, {"acosh", math_acosh, 1}, {"asin", math_asin, 1}, {"asinh", math_asinh, 1},
      {"atan", math_atan, 1}, {"atanh", math_atanh, 1}, {"atan2", math_atan2, 2}, {"cbrt", math_cbrt, 1}, {"ceil", math_ceil, 1},
      {"clz32", math_clz32, 1}, {"cos", math_cos, 1}, {"cosh", math_cosh, 1}, {"exp", math_exp, 1}, {"expm1", math_expm1, 1},
      {"floor", math_floor, 1}, {"fround", math_fround, 1}, {"hypot", math_hypot, 2}, {"imul", math_imul, 2}, {"log", math_log, 1},
      {"log1p", math_log1p, 1}, {"log10", math_log10, 1}, {"log2", math_log2, 1}, {"max", math_max, 2}, {"min", math_min, 2},
      {"pow", math_pow, 2}, {"random", math_random, 0}, {"round", math_round, 1}, {"sign", math_sign, 1}, {"sin", math_sin, 1},
      {"sinh", math_sinh, 1}, {"sqrt", math_sqrt, 1}, {"tan", math_tan, 1}, {"tanh", math_tanh, 1}, {"trunc", math_trunc, 1}};
  for (const F& f : fns) ctx.define_method(m, f.n, f.fn, f.len);
  struct C { const char* n; double v; } consts[] = {{"E", 2.718281828459045}, {"LN10", 2.302585092994046}, {"LN2", 0.6931471805599453},
                                                    {"LOG10E", 0.4342944819032518}, {"LOG2E", 1.4426950408889634}, {"PI", 3.141592653589793},
                                                    {"SQRT1_2", 0.7071067811865476}, {"SQRT2", 1.4142135623730951}};
  for (const C& c : consts) ctx.define_data(m, ctx.atom_ascii(c.n), Value::Number(c.v), 0);
  ctx.define_data(m, ctx.sym.to_string_tag, Str(ctx, "Math"), kConfigurable);
}

void setup_global_functions(Context& ctx) {
  Object* g = ctx.intr.global;
  ctx.define_data(g, ctx.atom_ascii("globalThis"), Value::Obj(g), kHiddenAttrs);
  ctx.define_data(g, ctx.atoms.NaN, Value::Number(NAN), 0);
  ctx.define_data(g, ctx.atoms.Infinity, Value::Number(INFINITY), 0);
  ctx.define_data(g, ctx.atoms.undefined, Value::Undefined(), 0);
  ctx.define_method(g, "isNaN", global_is_nan, 1);
  ctx.define_method(g, "isFinite", global_is_finite, 1);
  ctx.define_method(g, "eval", global_eval, 1);
  ctx.define_method(g, "encodeURI", global_encode_uri, 1);
  ctx.define_method(g, "encodeURIComponent", global_encode_uri_component, 1);
  ctx.define_method(g, "decodeURI", global_decode_uri, 1);
  ctx.define_method(g, "decodeURIComponent", global_decode_uri_component, 1);
  ctx.define_method(g, "escape", global_escape, 1);
  ctx.define_method(g, "unescape", global_unescape, 1);
}

}  // namespace aios::js
