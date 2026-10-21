// String and the string iterator.
#include <algorithm>
#include <cmath>

#include "builtins.hpp"
#include "internal_num.hpp"
#include "unicode.hpp"

namespace aios::js {

// ---------------------------------------------------------------------------------------------------
// StringObject (new String("abc")): indexed characters and `length` are read-only own properties
// ---------------------------------------------------------------------------------------------------

static int64_t string_index_key(PropKey k) {
  if (k->kind != CellKind::String) return -1;
  return static_cast<JsString*>(k)->array_index();
}

bool StringObject::get_own_property(Context& ctx, PropKey k, PropDesc* out, bool* found) {
  JsString* s = prim.as_string();
  if (k == ctx.atoms.length) {
    *out = PropDesc::Data(Value::Number(s->length), 0);
    *found = true;
    return true;
  }
  int64_t i = string_index_key(k);
  if (i >= 0 && static_cast<uint64_t>(i) < s->length) {
    *out = PropDesc::Data(Value::String(ctx.substring(s, static_cast<uint32_t>(i), static_cast<uint32_t>(i) + 1)), kEnumerable);
    *found = true;
    return true;
  }
  return Object::get_own_property(ctx, k, out, found);
}

bool StringObject::define_own_property(Context& ctx, PropKey k, const PropDesc& d, bool* ok) {
  JsString* s = prim.as_string();
  int64_t i = string_index_key(k);
  if (k == ctx.atoms.length || (i >= 0 && static_cast<uint64_t>(i) < s->length)) {
    PropDesc cur;
    bool found;
    get_own_property(ctx, k, &cur, &found);
    // an immutable property: only a compatible redefinition succeeds
    bool same = true;
    if (d.has_configurable && d.configurable) same = false;
    if (d.has_enumerable && d.enumerable != cur.enumerable) same = false;
    if (d.is_accessor()) same = false;
    if (d.has_writable && d.writable) same = false;
    if (d.has_value && !ctx.same_value(d.value, cur.value)) same = false;
    *ok = same;
    return true;
  }
  return Object::define_own_property(ctx, k, d, ok);
}

bool StringObject::delete_property(Context& ctx, PropKey k, bool* ok) {
  JsString* s = prim.as_string();
  int64_t i = string_index_key(k);
  if (k == ctx.atoms.length || (i >= 0 && static_cast<uint64_t>(i) < s->length)) {
    *ok = false;
    return true;
  }
  return Object::delete_property(ctx, k, ok);
}

bool StringObject::own_keys(Context& ctx, std::vector<PropKey>* out) {
  JsString* s = prim.as_string();
  for (uint32_t i = 0; i < s->length; ++i) out->push_back(ctx.index_atom(i));
  std::vector<PropKey> rest;
  if (!Object::own_keys(ctx, &rest)) return false;
  // integer keys beyond the string's own come first among the rest, then `length`, then the other strings
  std::vector<PropKey> idx, other, syms;
  for (PropKey k : rest) {
    if (k->kind == CellKind::Symbol) syms.push_back(k);
    else if (static_cast<JsString*>(k)->array_index() >= 0) idx.push_back(k);
    else other.push_back(k);
  }
  for (PropKey k : idx) out->push_back(k);
  out->push_back(ctx.atoms.length);
  for (PropKey k : other) out->push_back(k);
  for (PropKey k : syms) out->push_back(k);
  return true;
}

namespace {

struct StringIteratorObject final : Object {
  StringIteratorObject() : Object(Class::StringIterator) {}
  JsString* str = nullptr;
  size_t pos = 0;
  void trace(Tracer& t) override { Object::trace(t); t.mark(str); }
};

// RequireObjectCoercible(this) then ToString.
bool this_str(Context& ctx, const Value& v, const char* method, JsString** out) {
  if (v.is_nullish()) {
    ctx.throw_type_error(std::string("String.prototype.") + method + " called on null or undefined");
    return false;
  }
  return ctx.to_string(v, out);
}

#define THIS_STR(name)                                  \
  JsString* name;                                       \
  JS_TRY(this_str(ctx, a.this_value, __func__, &name))

Value string_ctor(Context& ctx, CallArgs& a) {
  JsString* s;
  if (a.argc == 0) s = ctx.atoms.empty;
  else if (!a.is_construct() && a[0].is_symbol()) {
    Symbol* sym = a[0].as_symbol();
    s = ctx.new_string(u"Symbol(" + (sym->description ? sym->description->str() : std::u16string()) + u")");
  } else {
    JS_TRY(ctx.to_string(a[0], &s));
  }
  if (!a.is_construct()) return Value::String(s);
  Object* proto;
  JS_TRY(ctx.prototype_from_ctor(a.new_target, ctx.intr.string_proto, &proto));
  StringObject* o = ctx.heap.make<StringObject>(s);
  o->proto = proto;
  return Value::Obj(o);
}

Value string_from_char_code(Context& ctx, CallArgs& a) {
  std::u16string out;
  for (int i = 0; i < a.argc; ++i) {
    uint32_t c;
    JS_TRY(ctx.to_uint32(a.argv[i], &c));
    out.push_back(static_cast<char16_t>(c & 0xFFFF));
  }
  return StrV(ctx, std::move(out));
}

Value string_from_code_point(Context& ctx, CallArgs& a) {
  std::u16string out;
  for (int i = 0; i < a.argc; ++i) {
    double d;
    JS_TRY(ctx.to_number(a.argv[i], &d));
    if (d != std::trunc(d) || d < 0 || d > 0x10FFFF || std::isnan(d)) {
      ctx.throw_range_error("Invalid code point " + ctx.number_to_string(d)->utf8());
      return Value::Exception();
    }
    uni::append_code_point(out, static_cast<uint32_t>(d));
  }
  return StrV(ctx, std::move(out));
}

Value string_raw(Context& ctx, CallArgs& a) {
  Object* cooked;
  JS_TRY(ctx.to_object(a[0], &cooked));
  Value rawv;
  JS_TRY(cooked->get(ctx, ctx.atoms.raw, Value::Obj(cooked), &rawv));
  Object* raw;
  JS_TRY(ctx.to_object(rawv, &raw));
  uint64_t len;
  JS_TRY(ctx.length_of(raw, &len));
  std::u16string out;
  for (uint64_t i = 0; i < len; ++i) {
    Value seg;
    JS_TRY(get_idx(ctx, raw, i, &seg));
    JsString* s;
    JS_TRY(ctx.to_string(seg, &s));
    out += s->str();
    if (i + 1 < len && static_cast<int>(i) + 1 < a.argc) {
      JsString* sub;
      JS_TRY(ctx.to_string(a.argv[i + 1], &sub));
      out += sub->str();
    }
  }
  return StrV(ctx, std::move(out));
}

bool this_string_value(Context& ctx, const Value& v, JsString** out, const char* method) {
  if (v.is_string()) { *out = v.as_string(); return true; }
  if (v.is_object() && v.as_object()->cls == Class::String) { *out = static_cast<PrimitiveObject*>(v.as_object())->prim.as_string(); return true; }
  ctx.throw_type_error(std::string("String.prototype.") + method + " requires that 'this' be a String");
  return false;
}

Value string_to_string(Context& ctx, CallArgs& a) {
  JsString* s;
  JS_TRY(this_string_value(ctx, a.this_value, &s, "toString"));
  return Value::String(s);
}

Value string_value_of(Context& ctx, CallArgs& a) {
  JsString* s;
  JS_TRY(this_string_value(ctx, a.this_value, &s, "valueOf"));
  return Value::String(s);
}

// ---- character access ----

Value string_at(Context& ctx, CallArgs& a) {
  THIS_STR(s);
  double rel;
  JS_TRY(ctx.to_integer(a[0], &rel));
  double k = rel >= 0 ? rel : s->length + rel;
  if (k < 0 || k >= s->length) return Value::Undefined();
  return Value::String(ctx.substring(s, static_cast<uint32_t>(k), static_cast<uint32_t>(k) + 1));
}

Value string_char_at(Context& ctx, CallArgs& a) {
  THIS_STR(s);
  double p;
  JS_TRY(ctx.to_integer(a[0], &p));
  if (p < 0 || p >= s->length) return Value::String(ctx.atoms.empty);
  return Value::String(ctx.substring(s, static_cast<uint32_t>(p), static_cast<uint32_t>(p) + 1));
}

Value string_char_code_at(Context& ctx, CallArgs& a) {
  THIS_STR(s);
  double p;
  JS_TRY(ctx.to_integer(a[0], &p));
  if (p < 0 || p >= s->length) return Value::Number(NAN);
  return Value::Number(s->str()[static_cast<size_t>(p)]);
}

Value string_code_point_at(Context& ctx, CallArgs& a) {
  THIS_STR(s);
  double p;
  JS_TRY(ctx.to_integer(a[0], &p));
  if (p < 0 || p >= s->length) return Value::Undefined();
  int len;
  return Value::Number(uni::code_point_at(s->str(), static_cast<size_t>(p), &len));
}

// ---- searching ----

Value string_index_of(Context& ctx, CallArgs& a) {
  THIS_STR(s);
  JsString* search;
  JS_TRY(ctx.to_string(a[0], &search));
  double pos;
  JS_TRY(ctx.to_integer(a[1], &pos));
  size_t start = static_cast<size_t>(std::min(std::max(pos, 0.0), static_cast<double>(s->length)));
  size_t r = s->str().find(search->str(), start);
  return Value::Number(r == std::u16string::npos ? -1 : static_cast<double>(r));
}

Value string_last_index_of(Context& ctx, CallArgs& a) {
  THIS_STR(s);
  JsString* search;
  JS_TRY(ctx.to_string(a[0], &search));
  double num;
  JS_TRY(ctx.to_number(a[1], &num));
  double pos = std::isnan(num) ? INFINITY : std::trunc(num);
  size_t start = static_cast<size_t>(std::min(std::max(pos, 0.0), static_cast<double>(s->length)));
  size_t r = s->str().rfind(search->str(), start);
  return Value::Number(r == std::u16string::npos ? -1 : static_cast<double>(r));
}

bool reject_regexp(Context& ctx, const Value& v, const char* method) {
  bool re;
  if (!is_regexp(ctx, v, &re)) return false;
  if (re) {
    ctx.throw_type_error(std::string("First argument to String.prototype.") + method + " must not be a regular expression");
    return false;
  }
  return true;
}

Value string_includes(Context& ctx, CallArgs& a) {
  THIS_STR(s);
  if (!reject_regexp(ctx, a[0], "includes")) return Value::Exception();
  JsString* search;
  JS_TRY(ctx.to_string(a[0], &search));
  double pos;
  JS_TRY(ctx.to_integer(a[1], &pos));
  size_t start = static_cast<size_t>(std::min(std::max(pos, 0.0), static_cast<double>(s->length)));
  return Value::Bool(s->str().find(search->str(), start) != std::u16string::npos);
}

Value string_starts_with(Context& ctx, CallArgs& a) {
  THIS_STR(s);
  if (!reject_regexp(ctx, a[0], "startsWith")) return Value::Exception();
  JsString* search;
  JS_TRY(ctx.to_string(a[0], &search));
  double pos;
  JS_TRY(ctx.to_integer(a[1], &pos));
  size_t start = static_cast<size_t>(std::min(std::max(pos, 0.0), static_cast<double>(s->length)));
  if (start + search->length > s->length) return Value::Bool(false);
  return Value::Bool(s->str().compare(start, search->length, search->str()) == 0);
}

Value string_ends_with(Context& ctx, CallArgs& a) {
  THIS_STR(s);
  if (!reject_regexp(ctx, a[0], "endsWith")) return Value::Exception();
  JsString* search;
  JS_TRY(ctx.to_string(a[0], &search));
  double end = s->length;
  if (!a[1].is_undefined()) JS_TRY(ctx.to_integer(a[1], &end));
  size_t e = static_cast<size_t>(std::min(std::max(end, 0.0), static_cast<double>(s->length)));
  if (search->length > e) return Value::Bool(false);
  return Value::Bool(s->str().compare(e - search->length, search->length, search->str()) == 0);
}

// ---- building ----

Value string_concat(Context& ctx, CallArgs& a) {
  THIS_STR(s);
  JsString* r = s;
  for (int i = 0; i < a.argc; ++i) {
    JsString* t;
    JS_TRY(ctx.to_string(a.argv[i], &t));
    r = ctx.concat(r, t);
  }
  return Value::String(r);
}

Value string_slice(Context& ctx, CallArgs& a) {
  THIS_STR(s);
  uint64_t from, to;
  JS_TRY(relative_index(ctx, a[0], s->length, 0, &from));
  JS_TRY(relative_index(ctx, a[1], s->length, s->length, &to));
  if (from >= to) return Value::String(ctx.atoms.empty);
  return Value::String(ctx.substring(s, static_cast<uint32_t>(from), static_cast<uint32_t>(to)));
}

Value string_substring(Context& ctx, CallArgs& a) {
  THIS_STR(s);
  double start, end = s->length;
  JS_TRY(ctx.to_integer(a[0], &start));
  if (!a[1].is_undefined()) JS_TRY(ctx.to_integer(a[1], &end));
  double len = s->length;
  double f = std::min(std::max(start, 0.0), len), t = std::min(std::max(end, 0.0), len);
  if (f > t) std::swap(f, t);
  return Value::String(ctx.substring(s, static_cast<uint32_t>(f), static_cast<uint32_t>(t)));
}

Value string_substr(Context& ctx, CallArgs& a) {
  THIS_STR(s);
  double start, length = INFINITY;
  JS_TRY(ctx.to_integer(a[0], &start));
  if (!a[1].is_undefined()) JS_TRY(ctx.to_integer(a[1], &length));
  double size = s->length;
  if (start < 0) start = std::max(size + start, 0.0);
  start = std::min(start, size);
  double end = std::min(start + std::min(std::max(length, 0.0), size), size);
  if (start >= end) return Value::String(ctx.atoms.empty);
  return Value::String(ctx.substring(s, static_cast<uint32_t>(start), static_cast<uint32_t>(end)));
}

Value pad_impl(Context& ctx, CallArgs& a, bool at_start) {
  THIS_STR(s);
  uint64_t max_len;
  JS_TRY(ctx.to_length(a[0], &max_len));
  if (max_len <= s->length) return Value::String(s);
  std::u16string filler = u" ";
  if (!a[1].is_undefined()) {
    JsString* f;
    JS_TRY(ctx.to_string(a[1], &f));
    filler = f->str();
  }
  if (filler.empty()) return Value::String(s);
  if (max_len > (1u << 29)) {
    ctx.throw_range_error("Invalid string length");
    return Value::Exception();
  }
  const size_t fill_len = static_cast<size_t>(max_len) - s->length;
  std::u16string pad;
  while (pad.size() < fill_len) pad += filler;
  pad.resize(fill_len);
  return StrV(ctx, at_start ? pad + s->str() : s->str() + pad);
}

Value string_pad_start(Context& ctx, CallArgs& a) { return pad_impl(ctx, a, true); }
Value string_pad_end(Context& ctx, CallArgs& a) { return pad_impl(ctx, a, false); }

Value string_repeat(Context& ctx, CallArgs& a) {
  THIS_STR(s);
  double n;
  JS_TRY(ctx.to_integer(a[0], &n));
  if (n < 0 || std::isinf(n)) {
    ctx.throw_range_error("Invalid count value: " + ctx.number_to_string(n)->utf8());
    return Value::Exception();
  }
  if (n * s->length > (1u << 29)) {
    ctx.throw_range_error("Invalid string length");
    return Value::Exception();
  }
  std::u16string out;
  out.reserve(static_cast<size_t>(n) * s->length);
  for (double i = 0; i < n; ++i) out += s->str();
  return StrV(ctx, std::move(out));
}

Value trim_impl(Context& ctx, CallArgs& a, bool start, bool end) {
  THIS_STR(s);
  const std::u16string& t = s->str();
  size_t b = 0, e = t.size();
  if (start) while (b < e && is_js_whitespace(t[b])) ++b;
  if (end) while (e > b && is_js_whitespace(t[e - 1])) --e;
  if (b == 0 && e == t.size()) return Value::String(s);
  return Value::String(ctx.substring(s, static_cast<uint32_t>(b), static_cast<uint32_t>(e)));
}

Value string_trim(Context& ctx, CallArgs& a) { return trim_impl(ctx, a, true, true); }
Value string_trim_start(Context& ctx, CallArgs& a) { return trim_impl(ctx, a, true, false); }
Value string_trim_end(Context& ctx, CallArgs& a) { return trim_impl(ctx, a, false, true); }

Value string_to_upper(Context& ctx, CallArgs& a) {
  THIS_STR(s);
  return StrV(ctx, uni::to_upper_string(s->str()));
}

Value string_to_lower(Context& ctx, CallArgs& a) {
  THIS_STR(s);
  return StrV(ctx, uni::to_lower_string(s->str()));
}

Value string_normalize(Context& ctx, CallArgs& a) {
  THIS_STR(s);
  int form = 0;
  if (!a[0].is_undefined()) {
    JsString* f;
    JS_TRY(ctx.to_string(a[0], &f));
    const std::u16string& n = f->str();
    if (n == u"NFC") form = 0;
    else if (n == u"NFD") form = 1;
    else if (n == u"NFKC") form = 2;
    else if (n == u"NFKD") form = 3;
    else {
      ctx.throw_range_error("The normalization form should be one of NFC, NFD, NFKC, NFKD.");
      return Value::Exception();
    }
  }
  return StrV(ctx, uni::normalize(s->str(), form));
}

Value string_is_well_formed(Context& ctx, CallArgs& a) {
  THIS_STR(s);
  const std::u16string& t = s->str();
  for (size_t i = 0; i < t.size(); ++i) {
    if (uni::is_high_surrogate(t[i])) {
      if (i + 1 < t.size() && uni::is_low_surrogate(t[i + 1])) { ++i; continue; }
      return Value::Bool(false);
    }
    if (uni::is_low_surrogate(t[i])) return Value::Bool(false);
  }
  return Value::Bool(true);
}

Value string_to_well_formed(Context& ctx, CallArgs& a) {
  THIS_STR(s);
  std::u16string t = s->str();
  for (size_t i = 0; i < t.size(); ++i) {
    if (uni::is_high_surrogate(t[i])) {
      if (i + 1 < t.size() && uni::is_low_surrogate(t[i + 1])) { ++i; continue; }
      t[i] = 0xFFFD;
    } else if (uni::is_low_surrogate(t[i])) {
      t[i] = 0xFFFD;
    }
  }
  return StrV(ctx, std::move(t));
}

// Collation: letters compare ignoring case and accents first, then accents, then case (lower before upper).
Value string_locale_compare(Context& ctx, CallArgs& a) {
  THIS_STR(s);
  JsString* that;
  JS_TRY(ctx.to_string(a[0], &that));
  auto key = [](const std::u16string& in, std::u16string* primary, std::u16string* accents, std::u16string* cases) {
    std::u16string d = uni::normalize(in, 1);
    for (size_t i = 0; i < d.size();) {
      int len;
      uint32_t cp = uni::code_point_at(d, i, &len);
      i += static_cast<size_t>(len);
      uni::Cat c = uni::category(cp);
      if (c == uni::Cat::Mn) { if (!accents->empty() || !primary->empty()) accents->push_back(static_cast<char16_t>(cp)); continue; }
      uint32_t f = uni::fold(cp);
      // punctuation and symbols sort before digits, digits before letters
      uint32_t weight = f;
      if (c == uni::Cat::Zs) weight = 0x100;
      else if (c == uni::Cat::Po || c == uni::Cat::Pd || c == uni::Cat::Ps || c == uni::Cat::Pe || c == uni::Cat::Pc || c == uni::Cat::Sm || c == uni::Cat::Sc || c == uni::Cat::Sk || c == uni::Cat::So) weight = 0x200 + (cp < 0x10000 ? cp : 0xFFFF) / 0x10;
      else if (c == uni::Cat::Nd) weight = 0x10000 + cp;
      else weight = 0x20000 + f;
      if (weight >= 0x10000) {
        primary->push_back(static_cast<char16_t>(0xD800 + ((weight - 0x10000) >> 10)));
        primary->push_back(static_cast<char16_t>(0xDC00 + ((weight - 0x10000) & 0x3FF)));
      } else {
        primary->push_back(static_cast<char16_t>(weight));
      }
      cases->push_back(uni::simple_lower(cp) == cp && uni::simple_upper(cp) != cp ? u'0' : (uni::simple_upper(cp) == cp && uni::simple_lower(cp) != cp ? u'1' : u'0'));
    }
  };
  std::u16string p1, a1, c1, p2, a2, c2;
  key(s->str(), &p1, &a1, &c1);
  key(that->str(), &p2, &a2, &c2);
  int r = p1.compare(p2);
  if (r == 0) r = a1.compare(a2);
  if (r == 0) r = c1.compare(c2);
  return Value::Number(r < 0 ? -1 : r > 0 ? 1 : 0);
}

// ---- pattern methods ----

bool regexp_create_value(Context& ctx, const Value& pattern, const Value& flags, Value* out) {
  Value args[2] = {pattern, flags};
  return ctx.construct(Value::Obj(ctx.intr.regexp_ctor), args, 2, Value::Obj(ctx.intr.regexp_ctor), out);
}

Value string_match(Context& ctx, CallArgs& a) {
  if (a.this_value.is_nullish()) {
    ctx.throw_type_error("String.prototype.match called on null or undefined");
    return Value::Exception();
  }
  if (!a[0].is_nullish()) {
    Value matcher;
    JS_TRY(ctx.get_method(a[0], ctx.sym.match, &matcher));
    if (!matcher.is_undefined()) {
      Value r;
      JS_TRY(ctx.call(matcher, a[0], {a.this_value}, &r));
      return r;
    }
  }
  JsString* s;
  JS_TRY(ctx.to_string(a.this_value, &s));
  Value rx;
  JS_TRY(regexp_create_value(ctx, a[0], Value::Undefined(), &rx));
  Value m;
  JS_TRY(ctx.get_method(rx, ctx.sym.match, &m));
  Value r;
  JS_TRY(ctx.call(m, rx, {Value::String(s)}, &r));
  return r;
}

Value string_match_all(Context& ctx, CallArgs& a) {
  if (a.this_value.is_nullish()) {
    ctx.throw_type_error("String.prototype.matchAll called on null or undefined");
    return Value::Exception();
  }
  if (!a[0].is_nullish()) {
    bool re;
    JS_TRY(is_regexp(ctx, a[0], &re));
    if (re) {
      Value flags;
      JS_TRY(ctx.get(a[0], ctx.atoms.flags, &flags));
      if (flags.is_nullish()) {
        ctx.throw_type_error("String.prototype.matchAll called with a non-global RegExp argument");
        return Value::Exception();
      }
      JsString* fs;
      JS_TRY(ctx.to_string(flags, &fs));
      if (fs->str().find(u'g') == std::u16string::npos) {
        ctx.throw_type_error("String.prototype.matchAll called with a non-global RegExp argument");
        return Value::Exception();
      }
    }
    Value matcher;
    JS_TRY(ctx.get_method(a[0], ctx.sym.match_all, &matcher));
    if (!matcher.is_undefined()) {
      Value r;
      JS_TRY(ctx.call(matcher, a[0], {a.this_value}, &r));
      return r;
    }
  }
  JsString* s;
  JS_TRY(ctx.to_string(a.this_value, &s));
  Value rx;
  JS_TRY(regexp_create_value(ctx, a[0], Str(ctx, "g"), &rx));
  Value m;
  JS_TRY(ctx.get_method(rx, ctx.sym.match_all, &m));
  Value r;
  JS_TRY(ctx.call(m, rx, {Value::String(s)}, &r));
  return r;
}

Value string_search(Context& ctx, CallArgs& a) {
  if (a.this_value.is_nullish()) {
    ctx.throw_type_error("String.prototype.search called on null or undefined");
    return Value::Exception();
  }
  if (!a[0].is_nullish()) {
    Value searcher;
    JS_TRY(ctx.get_method(a[0], ctx.sym.search, &searcher));
    if (!searcher.is_undefined()) {
      Value r;
      JS_TRY(ctx.call(searcher, a[0], {a.this_value}, &r));
      return r;
    }
  }
  JsString* s;
  JS_TRY(ctx.to_string(a.this_value, &s));
  Value rx;
  JS_TRY(regexp_create_value(ctx, a[0], Value::Undefined(), &rx));
  Value m;
  JS_TRY(ctx.get_method(rx, ctx.sym.search, &m));
  Value r;
  JS_TRY(ctx.call(m, rx, {Value::String(s)}, &r));
  return r;
}

Value replace_impl(Context& ctx, CallArgs& a, bool all) {
  if (a.this_value.is_nullish()) {
    ctx.throw_type_error(std::string("String.prototype.") + (all ? "replaceAll" : "replace") + " called on null or undefined");
    return Value::Exception();
  }
  const Value search = a[0];
  Value replace = a[1];
  if (!search.is_nullish()) {
    if (all) {
      bool re;
      JS_TRY(is_regexp(ctx, search, &re));
      if (re) {
        Value flags;
        JS_TRY(ctx.get(search, ctx.atoms.flags, &flags));
        if (flags.is_nullish()) {
          ctx.throw_type_error("replaceAll must be called with a global RegExp");
          return Value::Exception();
        }
        JsString* fs;
        JS_TRY(ctx.to_string(flags, &fs));
        if (fs->str().find(u'g') == std::u16string::npos) {
          ctx.throw_type_error("replaceAll must be called with a global RegExp");
          return Value::Exception();
        }
      }
    }
    Value replacer;
    JS_TRY(ctx.get_method(search, ctx.sym.replace, &replacer));
    if (!replacer.is_undefined()) {
      Value r;
      JS_TRY(ctx.call(replacer, search, {a.this_value, replace}, &r));
      return r;
    }
  }
  JsString* s;
  JS_TRY(ctx.to_string(a.this_value, &s));
  JsString* search_str;
  JS_TRY(ctx.to_string(search, &search_str));
  const bool functional = replace.is_callable();
  JsString* repl_str = nullptr;
  if (!functional) JS_TRY(ctx.to_string(replace, &repl_str));
  const std::u16string& str = s->str();
  const std::u16string& pat = search_str->str();
  const size_t adv = std::max<size_t>(1, pat.size());
  std::vector<size_t> positions;
  size_t pos = str.find(pat, 0);
  while (pos != std::u16string::npos) {
    positions.push_back(pos);
    if (!all) break;
    pos = pos + adv > str.size() ? std::u16string::npos : str.find(pat, pos + adv);
  }
  if (positions.empty()) return Value::String(s);
  std::u16string out;
  size_t end_of_last = 0;
  for (size_t p : positions) {
    out.append(str, end_of_last, p - end_of_last);
    if (functional) {
      Value r;
      JS_TRY(ctx.call(replace, Value::Undefined(), {Value::String(search_str), Value::Number(static_cast<double>(p)), Value::String(s)}, &r));
      JsString* rs;
      JS_TRY(ctx.to_string(r, &rs));
      out += rs->str();
    } else {
      std::u16string sub;
      JS_TRY(get_substitution(ctx, search_str, s, p, {}, Value::Undefined(), repl_str, &sub));
      out += sub;
    }
    end_of_last = p + pat.size();
  }
  if (end_of_last < str.size()) out.append(str, end_of_last, std::u16string::npos);
  return StrV(ctx, std::move(out));
}

Value string_replace(Context& ctx, CallArgs& a) { return replace_impl(ctx, a, false); }
Value string_replace_all(Context& ctx, CallArgs& a) { return replace_impl(ctx, a, true); }

Value string_split(Context& ctx, CallArgs& a) {
  if (a.this_value.is_nullish()) {
    ctx.throw_type_error("String.prototype.split called on null or undefined");
    return Value::Exception();
  }
  if (!a[0].is_nullish()) {
    Value splitter;
    JS_TRY(ctx.get_method(a[0], ctx.sym.split, &splitter));
    if (!splitter.is_undefined()) {
      Value r;
      JS_TRY(ctx.call(splitter, a[0], {a.this_value, a[1]}, &r));
      return r;
    }
  }
  JsString* s;
  JS_TRY(ctx.to_string(a.this_value, &s));
  uint32_t lim = 0xFFFFFFFFu;
  if (!a[1].is_undefined()) JS_TRY(ctx.to_uint32(a[1], &lim));
  JsString* sep;
  JS_TRY(ctx.to_string(a[0], &sep));
  std::vector<Value> out;
  if (lim == 0) return Value::Obj(ctx.new_array_from(out));
  if (a[0].is_undefined()) {
    out.push_back(Value::String(s));
    return Value::Obj(ctx.new_array_from(out));
  }
  const std::u16string& str = s->str();
  const std::u16string& pat = sep->str();
  if (str.empty()) {
    if (!pat.empty()) out.push_back(Value::String(s));
    return Value::Obj(ctx.new_array_from(out));
  }
  if (pat.empty()) {
    for (size_t i = 0; i < str.size() && out.size() < lim; ++i) out.push_back(Value::String(ctx.substring(s, static_cast<uint32_t>(i), static_cast<uint32_t>(i) + 1)));
    return Value::Obj(ctx.new_array_from(out));
  }
  size_t p = 0;
  for (;;) {
    size_t q = str.find(pat, p);
    if (q == std::u16string::npos) break;
    out.push_back(Value::String(ctx.substring(s, static_cast<uint32_t>(p), static_cast<uint32_t>(q))));
    if (out.size() >= lim) return Value::Obj(ctx.new_array_from(out));
    p = q + pat.size();
  }
  out.push_back(Value::String(ctx.substring(s, static_cast<uint32_t>(p), s->length)));
  return Value::Obj(ctx.new_array_from(out));
}

// ---- iterator and legacy HTML methods ----

Value string_iterator(Context& ctx, CallArgs& a) {
  THIS_STR(s);
  StringIteratorObject* it = ctx.heap.make<StringIteratorObject>();
  it->proto = ctx.intr.string_iterator_proto;
  it->str = s;
  return Value::Obj(it);
}

Value string_iterator_next(Context& ctx, CallArgs& a) {
  if (!a.this_value.is_object() || a.this_value.as_object()->cls != Class::StringIterator) {
    ctx.throw_type_error("next method called on incompatible receiver " + vm_describe(ctx, a.this_value));
    return Value::Exception();
  }
  StringIteratorObject* it = static_cast<StringIteratorObject*>(a.this_value.as_object());
  if (!it->str || it->pos >= it->str->length) {
    it->str = nullptr;
    return Value::Obj(ctx.create_iter_result(Value::Undefined(), true));
  }
  int len;
  uni::code_point_at(it->str->str(), it->pos, &len);
  Value v = Value::String(ctx.substring(it->str, static_cast<uint32_t>(it->pos), static_cast<uint32_t>(it->pos) + static_cast<uint32_t>(len)));
  it->pos += static_cast<size_t>(len);
  return Value::Obj(ctx.create_iter_result(v, false));
}

Value html_impl(Context& ctx, CallArgs& a, const char* tag, const char* attr) {
  THIS_STR(s);
  std::u16string out = u"<";
  for (const char* p = tag; *p; ++p) out.push_back(static_cast<char16_t>(*p));
  if (attr) {
    JsString* v;
    JS_TRY(ctx.to_string(a[0], &v));
    out += u" ";
    for (const char* p = attr; *p; ++p) out.push_back(static_cast<char16_t>(*p));
    out += u"=\"";
    for (char16_t c : v->str()) {
      if (c == u'"') out += u"&quot;";
      else out.push_back(c);
    }
    out += u"\"";
  }
  out += u">" + s->str() + u"</";
  for (const char* p = tag; *p; ++p) out.push_back(static_cast<char16_t>(*p));
  out += u">";
  return StrV(ctx, std::move(out));
}

#define HTML_METHOD(fn, tag, attr) \
  Value fn(Context& ctx, CallArgs& a) { return html_impl(ctx, a, tag, attr); }
HTML_METHOD(string_anchor, "a", "name")
HTML_METHOD(string_big, "big", nullptr)
HTML_METHOD(string_blink, "blink", nullptr)
HTML_METHOD(string_bold, "b", nullptr)
HTML_METHOD(string_fixed, "tt", nullptr)
HTML_METHOD(string_fontcolor, "font", "color")
HTML_METHOD(string_fontsize, "font", "size")
HTML_METHOD(string_italics, "i", nullptr)
HTML_METHOD(string_link, "a", "href")
HTML_METHOD(string_small, "small", nullptr)
HTML_METHOD(string_strike, "strike", nullptr)
HTML_METHOD(string_sub, "sub", nullptr)
HTML_METHOD(string_sup, "sup", nullptr)
#undef HTML_METHOD

}  // namespace

// GetSubstitution: $$, $&, $`, $', $n, $nn, $<name>.
bool get_substitution(Context& ctx, JsString* matched, JsString* str, size_t position, const std::vector<Value>& captures, const Value& named, JsString* replacement, std::u16string* out) {
  const std::u16string& r = replacement->str();
  const std::u16string& s = str->str();
  const size_t m = captures.size();
  const size_t match_len = matched->length;
  for (size_t i = 0; i < r.size(); ++i) {
    char16_t c = r[i];
    if (c != u'$' || i + 1 >= r.size()) { out->push_back(c); continue; }
    char16_t n = r[i + 1];
    if (n == u'$') { out->push_back(u'$'); ++i; }
    else if (n == u'&') { *out += matched->str(); ++i; }
    else if (n == u'`') { out->append(s, 0, position); ++i; }
    else if (n == u'\'') {
      size_t tail = std::min(position + match_len, s.size());
      out->append(s, tail, std::u16string::npos);
      ++i;
    } else if (n >= u'0' && n <= u'9') {
      size_t digits = 1;
      size_t idx = static_cast<size_t>(n - u'0');
      if (i + 2 < r.size() && r[i + 2] >= u'0' && r[i + 2] <= u'9') {
        size_t two = idx * 10 + static_cast<size_t>(r[i + 2] - u'0');
        if (two >= 1 && two <= m) { idx = two; digits = 2; }
      }
      if (idx >= 1 && idx <= m) {
        const Value& cap = captures[idx - 1];
        if (!cap.is_undefined()) {
          JsString* cs;
          if (!ctx.to_string(cap, &cs)) return false;
          *out += cs->str();
        }
        i += digits;
      } else {
        out->push_back(u'$');
      }
    } else if (n == u'<' && !named.is_undefined()) {
      size_t close = r.find(u'>', i + 2);
      if (close == std::u16string::npos) { out->push_back(u'$'); continue; }
      JsString* group = ctx.new_string(r.substr(i + 2, close - i - 2));
      Value v;
      if (!ctx.get_v(named, ctx.to_atom(group), &v)) return false;
      if (!v.is_undefined()) {
        JsString* vs;
        if (!ctx.to_string(v, &vs)) return false;
        *out += vs->str();
      }
      i = close;
    } else {
      out->push_back(u'$');
    }
  }
  return true;
}

void setup_string(Context& ctx) {
  StringObject* proto = ctx.heap.make<StringObject>(ctx.atoms.empty);
  proto->proto = ctx.intr.object_proto;
  ctx.intr.string_proto = proto;
  Function* ctor = new_ctor(ctx, "String", string_ctor, 1, proto);
  ctx.define_method(ctor, "fromCharCode", string_from_char_code, 1);
  ctx.define_method(ctor, "fromCodePoint", string_from_code_point, 1);
  ctx.define_method(ctor, "raw", string_raw, 1);
  struct M { const char* name; NativeFn fn; int len; } methods[] = {
      {"at", string_at, 1}, {"charAt", string_char_at, 1}, {"charCodeAt", string_char_code_at, 1}, {"codePointAt", string_code_point_at, 1},
      {"concat", string_concat, 1}, {"endsWith", string_ends_with, 1}, {"includes", string_includes, 1}, {"indexOf", string_index_of, 1},
      {"isWellFormed", string_is_well_formed, 0}, {"lastIndexOf", string_last_index_of, 1}, {"localeCompare", string_locale_compare, 1},
      {"match", string_match, 1}, {"matchAll", string_match_all, 1}, {"normalize", string_normalize, 0}, {"padEnd", string_pad_end, 1},
      {"padStart", string_pad_start, 1}, {"repeat", string_repeat, 1}, {"replace", string_replace, 2}, {"replaceAll", string_replace_all, 2},
      {"search", string_search, 1}, {"slice", string_slice, 2}, {"split", string_split, 2}, {"startsWith", string_starts_with, 1},
      {"substr", string_substr, 2}, {"substring", string_substring, 2}, {"toLocaleLowerCase", string_to_lower, 0},
      {"toLocaleUpperCase", string_to_upper, 0}, {"toLowerCase", string_to_lower, 0}, {"toString", string_to_string, 0},
      {"toUpperCase", string_to_upper, 0}, {"toWellFormed", string_to_well_formed, 0}, {"trim", string_trim, 0},
      {"trimEnd", string_trim_end, 0}, {"trimStart", string_trim_start, 0}, {"valueOf", string_value_of, 0},
      {"anchor", string_anchor, 1}, {"big", string_big, 0}, {"blink", string_blink, 0}, {"bold", string_bold, 0}, {"fixed", string_fixed, 0},
      {"fontcolor", string_fontcolor, 1}, {"fontsize", string_fontsize, 1}, {"italics", string_italics, 0}, {"link", string_link, 1},
      {"small", string_small, 0}, {"strike", string_strike, 0}, {"sub", string_sub, 0}, {"sup", string_sup, 0}};
  for (const M& m : methods) ctx.define_method(proto, m.name, m.fn, m.len);
  // trimLeft / trimRight are the same function objects as trimStart / trimEnd
  Value ts, te;
  ctx.get(proto, ctx.atom_ascii("trimStart"), &ts);
  ctx.get(proto, ctx.atom_ascii("trimEnd"), &te);
  ctx.define_data(proto, ctx.atom_ascii("trimLeft"), ts, kHiddenAttrs);
  ctx.define_data(proto, ctx.atom_ascii("trimRight"), te, kHiddenAttrs);
  ctx.define_method(proto, ctx.sym.iterator, "[Symbol.iterator]", string_iterator, 0);
  Object* sip = ctx.new_object(ctx.intr.iterator_proto);
  ctx.intr.string_iterator_proto = sip;
  ctx.define_method(sip, "next", string_iterator_next, 0);
  ctx.define_data(sip, ctx.sym.to_string_tag, Str(ctx, "String Iterator"), kConfigurable);
}

}  // namespace aios::js
