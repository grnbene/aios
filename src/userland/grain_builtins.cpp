// grain's standard library.
//
// Every one of these is a native taking the same shape, so adding a bridge to
// the rest of AOS later is one function and one line rather than a new calling
// convention. A native reports failure by filling `error`; it never throws,
// because the VM has a stack to unwind and C++ unwinding would skip it.
#include "aios/hal/sync.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <mutex>
#include <random>
#include <sstream>
#if !defined(_WIN32)
#include <unistd.h>
#else
#include <direct.h>
#define getcwd _getcwd
static int setenv(const char* name, const char* value, int) { return ::_putenv_s(name, value); }
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#endif
#include <unordered_set>


#include "aios/userland/grain.hpp"
#include "aios/userland/web.hpp"   // decode_image
#include "grain_builtins_internal.hpp"
#include "aios/userland/font_atlas.hpp"
#include "aios/userland/glyph_engine.hpp"

namespace aios::userland::grain {

namespace {


// --- output ----------------------------------------------------------------

Value bi_print(Vm& vm, std::vector<Value>& args, std::string&) {
  std::string line;
  for (size_t i = 0; i < args.size(); ++i) {
    if (i) line += " ";
    line += args[i].to_text();
  }
  vm.emit(line);
  return Value::nil();
}

Value bi_eprint(Vm&, std::vector<Value>& args, std::string&) {
  std::string line;
  for (size_t i = 0; i < args.size(); ++i) {
    if (i) line += " ";
    line += args[i].to_text();
  }
  std::cerr << line << "\n" << std::flush;
  return Value::nil();
}

// --- inspection ------------------------------------------------------------

Value bi_len(Vm&, std::vector<Value>& args, std::string& error) {
  const Value& v = args[0];
  switch (v.type) {
    case Type::Str: return Value::integer((int64_t)v.str().size());
    case Type::List: return Value::integer((int64_t)v.list_items().size());
    case Type::Map: return Value::integer((int64_t)static_cast<MapObj*>(v.obj.get())->entries.size());
    case Type::Bytes: return Value::integer((int64_t)v.bytes_data().size());
    default:
      error = std::string("len() wants a string, list, map or bytes, got a ") + type_name(v.type);
      return Value::nil();
  }
}

// One string per type name, made once per VM and shared (strings are immutable): `type(v) == "string"` on every value a
// loop touched made a string for each.
Value bi_type(Vm& vm, std::vector<Value>& args, std::string&) {
  if (vm.type_names.empty())
    for (int t = 0; t <= (int)Type::Decimal; ++t) vm.type_names.push_back(Value::string(type_name((Type)t)));
  const size_t t = (size_t)args[0].type;
  return t < vm.type_names.size() ? vm.type_names[t] : Value::string(type_name(args[0].type));
}

// A string is its own text: handed back as is (strings are immutable, so sharing one is safe), as the trims and
// case changes below hand back what they would not change - ck()'s upper(rtrim(str(s))) over keys already in
// that form made three copies of every key it was asked about.
Value bi_str(Vm&, std::vector<Value>& args, std::string&) {
  if (args[0].type == Type::Str && args[0].obj) return args[0];
  if (args[0].type == Type::Bytes) {
    const auto& b = args[0].bytes_data();
    return Value::string(std::string(b.begin(), b.end()));
  }
  return Value::string(args[0].to_text());
}

Value bi_repr(Vm&, std::vector<Value>& args, std::string&) {
  return Value::string(args[0].repr());
}

// Parsing a string that is not a number is an error rather than a zero: a
// silent 0 from bad input is indistinguishable from a real 0 downstream.
Value bi_int(Vm&, std::vector<Value>& args, std::string& error) {
  const Value& v = args[0];
  if (v.type == Type::Int) return v;
  if (v.type == Type::Real) return Value::integer((int64_t)v.d);
  if (v.type == Type::Decimal) {
    int64_t n = 0;
    if (!decimal_to_int(v, n)) { error = "int(): the decimal does not fit in 64 bits"; return Value::nil(); }
    return Value::integer(n);
  }
  if (v.type == Type::Bool) return Value::integer(v.b ? 1 : 0);
  if (v.type == Type::Str) {
    const std::string& s = v.str();
    // strtoll rather than stoll: stoll reports a bad parse by THROWING, and
    // this file is compiled with exceptions off in the bare-metal image, where
    // a throw crossing a frame that cannot unwind is a dead machine rather
    // than an error message.
    errno = 0;
    const char* begin = s.c_str();
    char* end = nullptr;
    const long long n = std::strtoll(begin, &end, 10);
    size_t used = end ? (size_t)(end - begin) : 0;
    while (used < s.size() && (s[used] == ' ' || s[used] == '\t')) used++;
    if (end == begin || used != s.size() || errno == ERANGE) {
      error = "int(): '" + s + "' is not a whole number";
      return Value::nil();
    }
    return Value::integer((int64_t)n);
  }
  error = std::string("int() cannot convert a ") + type_name(v.type);
  return Value::nil();
}

Value bi_real(Vm&, std::vector<Value>& args, std::string& error) {
  if (args[0].type == Type::Decimal) return Value::real(decimal_to_double(args[0]));
  const Value& v = args[0];
  if (v.is_number()) return Value::real(v.as_double());
  if (v.type == Type::Str) {
    const std::string& s = v.str();
    errno = 0;
    const char* begin = s.c_str();
    char* end = nullptr;
    const double d = std::strtod(begin, &end);
    size_t used = end ? (size_t)(end - begin) : 0;
    while (used < s.size() && (s[used] == ' ' || s[used] == '\t')) used++;
    if (end == begin || used != s.size() || errno == ERANGE) {
      error = "real(): '" + s + "' is not a number";
      return Value::nil();
    }
    return Value::real(d);
  }
  error = std::string("real() cannot convert a ") + type_name(v.type);
  return Value::nil();
}

// --- lists -----------------------------------------------------------------

Value bi_push(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_list(args[0], error, "push")) return Value::nil();
  auto& items = args[0].list_items();
  bool has_c = false;
  for (size_t i = 1; i < args.size(); ++i) {
    if (args[i].is_container()) has_c = true;
    items.push_back(std::move(args[i]));
  }
  if (has_c) vm.track_container(args[0].obj);
  return std::move(args[0]);
}

Value bi_pop(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_list(args[0], error, "pop")) return Value::nil();
  auto& items = args[0].list_items();
  if (items.empty()) { error = "pop() on an empty list"; return Value::nil(); }
  Value out = std::move(items.back());
  items.pop_back();
  return out;
}

Value bi_range(Vm&, std::vector<Value>& args, std::string& error) {
  int64_t from = 0, to = 0, step = 1;
  if (args.size() == 1) { if (!want_number(args[0], error, "range")) return Value::nil(); to = args[0].as_int(); }
  else {
    if (!want_number(args[0], error, "range") || !want_number(args[1], error, "range")) return Value::nil();
    from = args[0].as_int();
    to = args[1].as_int();
    if (args.size() > 2) {
      if (!want_number(args[2], error, "range")) return Value::nil();
      step = args[2].as_int();
    }
  }
  if (step == 0) { error = "range() step cannot be zero"; return Value::nil(); }
  // A range is a real list, so it can be indexed, kept and printed. The cost
  // is that range(1e9) allocates; the bound below says so instead of dying in
  // the allocator.
  const int64_t span = step > 0 ? (to - from) : (from - to);
  const int64_t magnitude = step > 0 ? step : -step;
  const int64_t count = span <= 0 ? 0 : (span + magnitude - 1) / magnitude;
  if (count > 10000000) { error = "range() of " + std::to_string(count) + " is too large"; return Value::nil(); }
  std::vector<Value> out;
  out.reserve((size_t)count);
  for (int64_t i = 0, v = from; i < count; ++i, v += step) out.push_back(Value::integer(v));
  return Value::list(std::move(out));
}

Value bi_sort(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_list(args[0], error, "sort")) return Value::nil();
  std::vector<Value> items = args[0].list_items();
  bool comparable = true;
  for (const Value& v : items)
    if (!v.is_number() && v.type != Type::Str && v.type != Type::Id128) comparable = false;
  if (!comparable) { error = "sort() needs a list of numbers, strings, or id128 values"; return Value::nil(); }
  std::stable_sort(items.begin(), items.end(), [](const Value& a, const Value& b) {
    if (a.is_number() && b.is_number()) return a.as_double() < b.as_double();
    if (a.type == Type::Str && b.type == Type::Str) return a.str() < b.str();
    if (a.type == Type::Id128 && b.type == Type::Id128) return a.as_id128() < b.as_id128();
    if (a.is_number() && !b.is_number()) return true;
    if (a.type == Type::Str && b.type == Type::Id128) return true;
    return false;
  });
  return Value::list(std::move(items));
}

Value bi_reverse(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_list(args[0], error, "reverse")) return Value::nil();
  std::vector<Value> items = args[0].list_items();
  std::reverse(items.begin(), items.end());
  return Value::list(std::move(items));
}

// --- maps ------------------------------------------------------------------

Value bi_keys(Vm&, std::vector<Value>& args, std::string& error) {
  if (args[0].type != Type::Map) { error = "keys() wants a map"; return Value::nil(); }
  std::vector<Value> out;
  for (auto& [k, v] : static_cast<MapObj*>(args[0].obj.get())->entries) out.push_back(Value::string(k));
  return Value::list(std::move(out));
}

Value bi_values(Vm&, std::vector<Value>& args, std::string& error) {
  if (args[0].type != Type::Map) { error = "values() wants a map"; return Value::nil(); }
  std::vector<Value> out;
  for (auto& [k, v] : static_cast<MapObj*>(args[0].obj.get())->entries) out.push_back(v);
  return Value::list(std::move(out));
}

// `has` exists because a map returns nil for a missing key, and nil is also a
// value you are allowed to store.
Value bi_has(Vm&, std::vector<Value>& args, std::string& error) {
  if (args[0].type != Type::Map) { error = "has() wants a map"; return Value::nil(); }
  if (!want_str(args[1], error, "has")) return Value::nil();
  return Value::boolean(static_cast<MapObj*>(args[0].obj.get())->find(args[1].str()) != nullptr);
}

Value bi_del(Vm&, std::vector<Value>& args, std::string& error) {
  if (args[0].type != Type::Map) { error = "del() wants a map"; return Value::nil(); }
  if (!want_str(args[1], error, "del")) return Value::nil();
  return Value::boolean(static_cast<MapObj*>(args[0].obj.get())->erase(args[1].str()));
}

// --- strings ---------------------------------------------------------------

Value bi_split(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "split")) return Value::nil();
  const std::string& s = args[0].str();
  const std::string sep = args.size() > 1 ? args[1].str() : " ";
  std::vector<Value> out;
  if (sep.empty()) {
    for (char c : s) out.push_back(Value::string(std::string(1, c)));
    return Value::list(std::move(out));
  }
  size_t at = 0;
  for (;;) {
    const size_t found = s.find(sep, at);
    if (found == std::string::npos) { out.push_back(Value::string(s.substr(at))); break; }
    out.push_back(Value::string(s.substr(at, found - at)));
    at = found + sep.size();
  }
  return Value::list(std::move(out));
}

Value bi_join(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_list(args[0], error, "join")) return Value::nil();
  const std::string sep = args.size() > 1 ? args[1].str() : "";
  std::string out;
  const auto& items = args[0].list_items();
  for (size_t i = 0; i < items.size(); ++i) { if (i) out += sep; out += items[i].to_text(); }
  return Value::string(out);
}

// --- characters in UTF-8 text ----------------------------------------------------------------------
// Strings are bytes; len, s[i], slice and index_of count bytes. The builtins that speak in characters
// (chars, char_at, ord, chr, pad_left, pad_right, upper, lower) step through the text one UTF-8 sequence
// at a time with these. A byte that is not well-formed UTF-8 is one character of its own, kept as it is.

// How many bytes the character starting at s[at] takes (1 for an invalid lead or a cut-off sequence).
size_t utf8_char_len(const std::string& s, size_t at) {
  const unsigned char b = (unsigned char)s[at];
  size_t n = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 1;
  if (at + n > s.size()) return 1;
  for (size_t k = 1; k < n; ++k) if (((unsigned char)s[at + k] & 0xC0) != 0x80) return 1;
  return n;
}

// The code point of the character at s[at] (the byte itself when it is not well-formed UTF-8).
uint32_t utf8_code_point(const std::string& s, size_t at, size_t n) {
  const unsigned char b = (unsigned char)s[at];
  if (n == 1) return b;
  uint32_t cp = n == 2 ? (b & 0x1F) : n == 3 ? (b & 0x0F) : (b & 0x07);
  for (size_t k = 1; k < n; ++k) cp = (cp << 6) | ((unsigned char)s[at + k] & 0x3F);
  return cp;
}

void utf8_append(std::string& out, uint32_t cp) {
  if (cp < 0x80) out += (char)cp;
  else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
  else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
}

size_t utf8_count(const std::string& s) {
  size_t n = 0;
  for (size_t at = 0; at < s.size(); at += utf8_char_len(s, at)) ++n;
  return n;
}

// Case for ASCII, the Latin-1 Supplement and Latin Extended-A: the letters of the Western European
// languages (names, addresses, product text). Other scripts are returned as they are. ß has no
// single-character upper case and stays ß; ÿ's upper case Ÿ is in Extended-A.
uint32_t to_upper_cp(uint32_t c) {
  if (c >= 'a' && c <= 'z') return c - 32;
  if (c < 0xE0) return c;
  if (c <= 0xFE && c != 0xF7) return c - 32;                                  // à..þ -> À..Þ (÷ is not a letter)
  if (c == 0xFF) return 0x178;                                                // ÿ -> Ÿ
  if (c == 0x131) return 'I';                                                 // dotless ı
  if (c >= 0x100 && c <= 0x137) return (c & 1) ? c - 1 : c;                   // pairs: even upper, odd lower
  if (c >= 0x139 && c <= 0x148) return (c & 1) ? c : c - 1;                   // pairs: odd upper, even lower
  if (c >= 0x14A && c <= 0x177) return (c & 1) ? c - 1 : c;
  if (c >= 0x179 && c <= 0x17E) return (c & 1) ? c : c - 1;
  if (c == 0x17F) return 'S';                                                 // long s
  return c;
}

uint32_t to_lower_cp(uint32_t c) {
  if (c >= 'A' && c <= 'Z') return c + 32;
  if (c < 0xC0) return c;
  if (c <= 0xDE && c != 0xD7) return c + 32;                                  // À..Þ -> à..þ (× is not a letter)
  if (c == 0x178) return 0xFF;                                                // Ÿ -> ÿ
  if (c == 0x130) return 'i';                                                 // İ (dotted I)
  if (c >= 0x100 && c <= 0x137) return (c & 1) ? c : c + 1;
  if (c >= 0x139 && c <= 0x148) return (c & 1) ? c + 1 : c;
  if (c >= 0x14A && c <= 0x177) return (c & 1) ? c : c + 1;
  if (c >= 0x179 && c <= 0x17E) return (c & 1) ? c + 1 : c;
  return c;
}

Value recase(const Value& in_v, uint32_t (*map)(uint32_t)) {
  const std::string& in = in_v.str();
  std::string out;
  bool changed = false;
  for (size_t at = 0; at < in.size();) {
    const size_t n = utf8_char_len(in, at);
    const uint32_t cp = utf8_code_point(in, at, n);
    const uint32_t mapped = (n == 1 && cp >= 0x80) ? cp : map(cp);   // a stray byte is not a letter
    if (mapped == cp) { out.append(in, at, n); }
    else { utf8_append(out, mapped); changed = true; }
    at += n;
  }
  return changed ? Value::string(std::move(out)) : in_v;
}

Value bi_upper(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "upper")) return Value::nil();
  return recase(args[0], to_upper_cp);
}

Value bi_lower(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "lower")) return Value::nil();
  return recase(args[0], to_lower_cp);
}

// -1 for "not here", so a caller can test it without a second call.
Value bi_find(Vm&, std::vector<Value>& args, std::string& error) {
  if (args[0].type == Type::List) {
    const auto& items = args[0].list_items();
    for (size_t i = 0; i < items.size(); ++i) if (items[i].equals(args[1])) return Value::integer((int64_t)i);
    return Value::integer(-1);
  }
  if (!want_str(args[0], error, "find") || !want_str(args[1], error, "find")) return Value::nil();
  const size_t at = args[0].str().find(args[1].str());
  return Value::integer(at == std::string::npos ? -1 : (int64_t)at);
}

Value bi_replace(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "replace")) return Value::nil();
  std::string s = args[0].str();
  const std::string from = args[1].str(), to = args[2].str();
  if (from.empty()) return Value::string(s);
  std::string out;
  size_t at = 0;
  for (;;) {
    const size_t found = s.find(from, at);
    if (found == std::string::npos) { out += s.substr(at); break; }
    out += s.substr(at, found - at) + to;
    at = found + from.size();
  }
  return Value::string(out);
}

// One slice for strings and lists, with Python's forgiving bounds: an end past
// the end is the end, which is what every caller wanted anyway.
Value bi_slice(Vm&, std::vector<Value>& args, std::string& error) {
  const Value& v = args[0];
  const int64_t size = v.type == Type::Str ? (int64_t)v.str().size()
                     : v.type == Type::Bytes ? (int64_t)v.bytes_data().size()
                     : v.type == Type::List ? (int64_t)v.list_items().size() : -1;
  if (size < 0) { error = std::string("slice() wants a string, list or bytes, got a ") + type_name(v.type); return Value::nil(); }
  int64_t from = args.size() > 1 ? args[1].as_int() : 0;
  int64_t to = args.size() > 2 ? args[2].as_int() : size;
  if (from < 0) from += size;
  if (to < 0) to += size;
  from = std::max<int64_t>(0, std::min(from, size));
  to = std::max<int64_t>(from, std::min(to, size));
  if (v.type == Type::Str) return Value::string(v.str().substr((size_t)from, (size_t)(to - from)));
  if (v.type == Type::Bytes) {
    const auto& d = v.bytes_data();
    return Value::bytes(std::vector<uint8_t>(d.begin() + from, d.begin() + to));
  }
  const auto& items = v.list_items();
  return Value::list(std::vector<Value>(items.begin() + from, items.begin() + to));
}

Value bi_ord(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "ord")) return Value::nil();
  if (args[0].str().empty()) { error = "ord() of an empty string"; return Value::nil(); }
  const std::string& s = args[0].str();
  return Value::integer((int64_t)utf8_code_point(s, 0, utf8_char_len(s, 0)));
}

Value bi_chr(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "chr")) return Value::nil();
  const int64_t c = args[0].as_int();
  if (c < 0 || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) { error = "chr() takes a code point, 0..1114111 and not a surrogate"; return Value::nil(); }
  std::string out;
  utf8_append(out, (uint32_t)c);
  return Value::string(std::move(out));
}

// --- arithmetic ------------------------------------------------------------

Value bi_abs(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_numeric(args[0], error, "abs")) return Value::nil();
  if (args[0].type == Type::Decimal) {
    Value r = args[0];
    if (decimal_compare(r, Value::integer(0)) < 0) decimal_negate(args[0], r, error);
    return r;
  }
  if (args[0].type == Type::Int) return Value::integer(args[0].i < 0 ? -args[0].i : args[0].i);
  return Value::real(std::fabs(args[0].d));
}

// utf8_valid(text or bytes) -> bool: is every byte part of well-formed UTF-8
// (no overlong forms, no surrogates, nothing past U+10FFFF)? A string can hold
// any bytes, so this is how a program asks before it hands one to something
// that insists on text.
Value bi_utf8_valid(Vm&, std::vector<Value>& args, std::string& error) {
  const uint8_t* p = nullptr;
  size_t n = 0;
  if (args[0].type == Type::Str) { p = (const uint8_t*)args[0].str().data(); n = args[0].str().size(); }
  else if (args[0].type == Type::Bytes) { p = args[0].bytes_data().data(); n = args[0].bytes_data().size(); }
  else { error = std::string("utf8_valid() wants a string or bytes, got a ") + type_name(args[0].type); return Value::nil(); }
  size_t i = 0;
  while (i < n) {
    const uint8_t c = p[i];
    size_t len;
    uint32_t cp;
    if (c < 0x80) { ++i; continue; }
    else if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0F; }
    else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; }
    else return Value::boolean(false);
    if (i + len > n) return Value::boolean(false);
    for (size_t k = 1; k < len; ++k) {
      if ((p[i + k] & 0xC0) != 0x80) return Value::boolean(false);
      cp = (cp << 6) | (p[i + k] & 0x3F);
    }
    if ((len == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)))
      return Value::boolean(false);
    i += len;
  }
  return Value::boolean(true);
}

// `/` always gives a real, so whole-number division is its own call. It
// truncates toward zero, which is what makes idiv(a, b) * b + a % b == a.
// Integers only, as for the bit operations: idiv(7.5, 2) is an error, not a
// quiet truncation.
Value bi_idiv(Vm&, std::vector<Value>& args, std::string& error) {
  if (args[0].type != Type::Int || args[1].type != Type::Int) {
    error = std::string("idiv() takes integers, got ") + type_name(args[0].type) + " and " + type_name(args[1].type);
    return Value::nil();
  }
  if (args[1].i == 0) { error = "idiv(): division by zero"; return Value::nil(); }
  // INT64_MIN / -1 traps on x86 (SIGFPE); arithmetic wraps, so it stays INT64_MIN.
  if (args[1].i == -1) return Value::integer((int64_t)(0 - (uint64_t)args[0].i));
  return Value::integer(args[0].i / args[1].i);
}

// ---- import --------------------------------------------------------------------------------------

std::string import_dir(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? "" : path.substr(0, slash);
}

// a/b/../c.gr -> a/c.gr, ./x.gr -> x.gr: one spelling per file, so importing it two ways runs it once.
std::string import_normalize(const std::string& path) {
  std::vector<std::string> parts;
  size_t i = 0;
  const bool absolute = !path.empty() && path[0] == '/';
  while (i <= path.size()) {
    size_t j = path.find('/', i);
    if (j == std::string::npos) j = path.size();
    const std::string part = path.substr(i, j - i);
    if (part == "..") { if (!parts.empty() && parts.back() != "..") parts.pop_back(); else if (!absolute) parts.push_back(part); }
    else if (!part.empty() && part != ".") parts.push_back(part);
    i = j + 1;
  }
  std::string out = absolute ? "/" : "";
  for (size_t k = 0; k < parts.size(); ++k) out += (k ? "/" : "") + parts[k];
  return out;
}

// A compiled function and every function inside it, copied: the same code and constants, with empty inline caches
// and no native code yet. Constants other than functions are literals nothing changes, so they are shared.
std::shared_ptr<FnObj> clone_fn_tree(const std::shared_ptr<FnObj>& src) {
  if (!src) return nullptr;
  auto dst = std::make_shared<FnObj>();
  dst->name = src->name;
  dst->arity = src->arity;
  dst->local_slots = src->local_slots;
  dst->debug_locals = src->debug_locals;
  dst->upvalue_from_local = src->upvalue_from_local;
  dst->upvalue_index = src->upvalue_index;
  dst->chunk.code = src->chunk.code;
  dst->chunk.lines = src->chunk.lines;
  dst->chunk.columns = src->chunk.columns;
  dst->chunk.constants = src->chunk.constants;
  for (Value& c : dst->chunk.constants)
    if (c.type == Type::Fn && c.obj && !c.obj->is_closure)
      c.obj = clone_fn_tree(std::static_pointer_cast<FnObj>(c.obj));
  return dst;
}

// import "lib.gr": runs another file in this program, once, so its functions and values are defined here.
// The path is relative to the file that imports it. Everything it defines at its top level is global, as if it
// had been written in place; a name it shares with the program is the later definition's. An error inside it
// names it: in the message while it loads, and in the stack trace after (its functions are listed as lib.gr:name).
Value bi_import(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "import")) return Value::nil();
  std::string path = args[0].str();
  if (path.empty()) { error = "import(): an empty path"; return Value::nil(); }
  if (path[0] != '/' && !(path.size() > 2 && path[1] == ':') && !vm.base_dir.empty()) path = vm.base_dir + "/" + path;
  path = import_normalize(path);
  std::string as_name;
  struct ImportSym {
    std::string orig;
    std::string alias;
  };
  std::vector<ImportSym> symbols_filter;
  if (args.size() >= 2) {
    if (args[1].type == Type::Str) {
      as_name = args[1].str();
    } else if (args[1].type == Type::List) {
      for (const auto& item : args[1].list_items()) {
        if (item.type == Type::Str) {
          symbols_filter.push_back({item.str(), item.str()});
        } else if (item.type == Type::List && item.list_items().size() >= 2) {
          symbols_filter.push_back({item.list_items()[0].str(), item.list_items()[1].str()});
        }
      }
    }
  }

  // plain: path; as a name: path#name; picking names out: path#* (their values live under __mod_<hash>::)
  const bool selective = as_name.empty() && !symbols_filter.empty();
  const std::string import_tag = path + (selective ? "#*" : as_name.empty() ? "" : "#" + as_name);
  for (const auto& done : vm.imported) {
    if (done == import_tag || done == path) {
      if (!symbols_filter.empty()) {
        // already loaded: the names are where that load put them. Plain-imported, they are top-level globals;
        // picked out, they are under the module's own prefix. Anything else - another module's, or the
        // importing program's - is not this file's, so it is an error rather than a quiet wrong binding.
        const std::string prefix = "__mod_" + std::to_string(std::hash<std::string>{}(path));
        for (const auto& sym : symbols_filter) {
          Value* val = vm.find_global(prefix + "::" + sym.orig);
          if (!val && done == path) val = vm.find_global(sym.orig);
          if (!val) { error = "import(\"" + path + "\") defines no '" + sym.orig + "'"; return Value::nil(); }
          vm.define_global(sym.alias, *val);
        }
      }
      return Value::nil();
    }
  }
  std::string source, why;
  if (!(vm.packed_source && vm.packed_source(path, source))) {
    if (!vm.files) { error = "import(): this program has no file storage to import \"" + path + "\" from"; return Value::nil(); }
    if (!vm.files->read(path, source, why)) { error = "import(\"" + path + "\"): " + why; return Value::nil(); }
  }
  vm.imported.push_back(import_tag);   // before it runs: a file that imports its importer finds it already loaded

  struct CachedModule {
    CompileResult result;
    size_t hash = 0;
  };
  static std::mutex s_mod_cache_mu;
  static std::unordered_map<std::string, CachedModule> s_mod_cache;

  size_t src_hash = std::hash<std::string>{}(source);
  CompileResult raw_compiled;
  {
    std::lock_guard<std::mutex> lk(s_mod_cache_mu);
    auto it = s_mod_cache.find(path);
    if (it != s_mod_cache.end() && it->second.hash == src_hash) {
      raw_compiled = it->second.result;
    }
  }
  if (!raw_compiled.ok()) {
    raw_compiled = compile_source(source, path, false);
    if (raw_compiled.ok()) {
      std::lock_guard<std::mutex> lk(s_mod_cache_mu);
      s_mod_cache[path] = {raw_compiled, src_hash};
    }
  }
  if (!raw_compiled.ok()) {
    error = "import(\"" + path + "\") does not compile:";
    for (const auto& d : raw_compiled.errors) error += "\n  " + path + ":" + std::to_string(d.line) + ": " + d.message;
    return Value::nil();
  }

  // The cache holds the compile; each import runs its own copy of it. A function's chunk keeps per-VM state (the
  // global slot cache, native code), and the renaming and rewriting below change it, so VMs on other threads must
  // never share one: two page sessions importing the same file at once corrupted the heap, and read each other's
  // globals. The same rule as a worker's functions (grain_std_parallel.cpp, Copier::function).
  CompileResult compiled = raw_compiled;
  compiled.script = clone_fn_tree(raw_compiled.script);

  std::string mod_prefix;
  if (!as_name.empty()) {
    mod_prefix = as_name;
  } else if (!symbols_filter.empty()) {
    mod_prefix = "__mod_" + std::to_string(std::hash<std::string>{}(path));
  }

  std::unordered_set<std::string> defined_names;
  if (!mod_prefix.empty()) {
    const auto& code = compiled.script->chunk.code;
    const auto& consts = compiled.script->chunk.constants;
    size_t ci = 0;
    while (ci < code.size()) {
      Op op = static_cast<Op>(code[ci]);
      if (op == Op::DefineGlobal && ci + 2 < code.size()) {
        uint16_t k = (static_cast<uint16_t>(code[ci + 1]) << 8) | code[ci + 2];
        if (k < consts.size() && consts[k].type == Type::Str) {
          defined_names.insert(consts[k].str());
        }
      }
      switch (op) {
        case Op::Nil: case Op::True: case Op::False: case Op::Pop:
        case Op::Equal: case Op::NotEqual: case Op::Less: case Op::LessEqual: case Op::Greater: case Op::GreaterEqual:
        case Op::Add: case Op::Subtract: case Op::Multiply: case Op::Divide: case Op::Modulo: case Op::Negate: case Op::Not:
        case Op::BitAnd: case Op::BitOr: case Op::BitXor: case Op::BitNot: case Op::Shl: case Op::Shr:
        case Op::Dup: case Op::Dup2:
        case Op::CloseUpvalue: case Op::Return: case Op::IndexGet: case Op::IndexSet: case Op::IterPrep: case Op::Halt:
        case Op::CallList:
          ci += 1; break;
        case Op::GetLocal: case Op::SetLocal: case Op::GetUpvalue: case Op::SetUpvalue: case Op::Call: case Op::IterNext:
          ci += 2; break;
        case Op::Constant: case Op::GetGlobal: case Op::SetGlobal: case Op::DefineGlobal:
        case Op::FieldGet: case Op::OptFieldGet: case Op::FieldSet: case Op::BuildList: case Op::BuildMap: case Op::ListConcat:
        case Op::Jump: case Op::JumpIfFalse: case Op::JumpIfTrue: case Op::Loop:
          ci += 3; break;
        case Op::Closure: {
          if (ci + 2 >= code.size()) { ci = code.size(); break; }
          uint16_t ck = (static_cast<uint16_t>(code[ci + 1]) << 8) | code[ci + 2];
          if (ck < consts.size() && consts[ck].obj) {
            const auto* sub = static_cast<const FnObj*>(consts[ck].obj.get());
            ci += 3 + 2 * sub->upvalue_index.size();
          } else {
            ci += 3;
          }
          break;
        }
        default: ci += 1; break;
      }
    }
  }

  // Rewrite global access opcodes to target namespaced constants, leaving string literals intact.
  auto rewrite_chunk_globals = [&](Chunk& ch) {
    if (mod_prefix.empty()) return;
    auto& code = ch.code;
    size_t ci = 0;
    while (ci < code.size()) {
      Op op = static_cast<Op>(code[ci]);
      if ((op == Op::GetGlobal || op == Op::SetGlobal || op == Op::DefineGlobal) && ci + 2 < code.size()) {
        uint16_t k = (static_cast<uint16_t>(code[ci + 1]) << 8) | code[ci + 2];
        if (k < ch.constants.size() && ch.constants[k].type == Type::Str) {
          const std::string& old_name = ch.constants[k].str();
          if (defined_names.count(old_name) > 0) {
            size_t new_k = ch.add_constant(Value::string(mod_prefix + "::" + old_name));
            code[ci + 1] = static_cast<uint8_t>((new_k >> 8) & 0xff);
            code[ci + 2] = static_cast<uint8_t>(new_k & 0xff);
          }
        }
      }
      switch (op) {
        case Op::Nil: case Op::True: case Op::False: case Op::Pop:
        case Op::Equal: case Op::NotEqual: case Op::Less: case Op::LessEqual: case Op::Greater: case Op::GreaterEqual:
        case Op::Add: case Op::Subtract: case Op::Multiply: case Op::Divide: case Op::Modulo: case Op::Negate: case Op::Not:
        case Op::BitAnd: case Op::BitOr: case Op::BitXor: case Op::BitNot: case Op::Shl: case Op::Shr:
        case Op::Dup: case Op::Dup2:
        case Op::CloseUpvalue: case Op::Return: case Op::IndexGet: case Op::IndexSet: case Op::IterPrep: case Op::Halt:
        case Op::CallList:
          ci += 1; break;
        case Op::GetLocal: case Op::SetLocal: case Op::GetUpvalue: case Op::SetUpvalue: case Op::Call: case Op::IterNext:
          ci += 2; break;
        case Op::Constant: case Op::GetGlobal: case Op::SetGlobal: case Op::DefineGlobal:
        case Op::FieldGet: case Op::OptFieldGet: case Op::FieldSet: case Op::BuildList: case Op::BuildMap: case Op::ListConcat:
        case Op::Jump: case Op::JumpIfFalse: case Op::JumpIfTrue: case Op::Loop:
          ci += 3; break;
        case Op::Closure: {
          if (ci + 2 >= code.size()) { ci = code.size(); break; }
          uint16_t ck = (static_cast<uint16_t>(code[ci + 1]) << 8) | code[ci + 2];
          if (ck < ch.constants.size() && ch.constants[ck].obj) {
            const auto* sub = static_cast<const FnObj*>(ch.constants[ck].obj.get());
            ci += 3 + 2 * sub->upvalue_index.size();
          } else {
            ci += 3;
          }
          break;
        }
        default: ci += 1; break;
      }
    }
  };

  rewrite_chunk_globals(compiled.script->chunk);
  std::vector<FnObj*> todo;
  for (const Value& c : compiled.script->chunk.constants)
    if (c.type == Type::Fn && c.obj && !c.obj->is_closure) todo.push_back(static_cast<FnObj*>(c.obj.get()));
  while (!todo.empty()) {
    FnObj* f = todo.back();
    todo.pop_back();
    f->name = path + ":" + f->name;
    rewrite_chunk_globals(f->chunk);
    for (const Value& c : f->chunk.constants)
      if (c.type == Type::Fn && c.obj && !c.obj->is_closure) todo.push_back(static_cast<FnObj*>(c.obj.get()));
  }
  const std::string saved_dir = vm.base_dir;
  vm.base_dir = import_dir(path);
  Value ignored = vm.call(Value::object(Type::Fn, compiled.script), {}, why);
  vm.base_dir = saved_dir;
  if (!why.empty()) { error = "import(\"" + path + "\"): " + why; return Value::nil(); }
  (void)ignored;

  auto mod_map = std::make_shared<MapObj>();
  if (!as_name.empty()) {
    for (const auto& name : defined_names) {
      std::string prefixed = as_name + "::" + name;
      Value* val = vm.find_global(prefixed);
      if (val) mod_map->put(name, *val);
    }
    vm.define_global(as_name, Value::object(Type::Map, mod_map));
  } else if (!symbols_filter.empty()) {
    for (const auto& sym : symbols_filter) {
      std::string prefixed = mod_prefix + "::" + sym.orig;
      Value* val = vm.find_global(prefixed);
      if (!val) { error = "import(\"" + path + "\") defines no '" + sym.orig + "'"; return Value::nil(); }
      vm.define_global(sym.alias, *val);
      mod_map->put(sym.alias, *val);
    }
  }
  return Value::object(Type::Map, mod_map);
}

// min/max of numbers, or of strings (by bytes, as < compares them: "2026-10-27 08:05" before "2026-10-27 14:30"),
// given as arguments or as one list. nil is no value and is passed over (all nil: nil), so a running latest starts
// as nil: last = max(last, t). Numbers and strings together are an error: there is no order between them.
static Value extreme(std::vector<Value>& args, std::string& error, const char* who, int want) {
  const std::vector<Value>& vals = args.size() == 1 && args[0].type == Type::List ? args[0].list_items() : args;
  const Value* best = nullptr;
  bool strings = false;
  bool saw_nil = false;
  for (const Value& v : vals) {
    if (v.type == Type::Nil) { saw_nil = true; continue; }   // no value: max(seen, nil) is seen
    const bool s = v.type == Type::Str;
    if (!s && !v.is_number() && v.type != Type::Decimal) {
      error = std::string(who) + "() compares numbers or strings, got a " + type_name(v.type);
      return Value::nil();
    }
    if (!best) { best = &v; strings = s; continue; }
    if (s != strings) { error = std::string(who) + "() of numbers and strings together"; return Value::nil(); }
    int c;
    if (s) {
      const int r = v.str().compare(best->str());
      c = r < 0 ? -1 : r > 0 ? 1 : 0;
    } else if ((v.type == Type::Decimal || best->type == Type::Decimal) && is_exact_number(v) && is_exact_number(*best)) {
      c = decimal_compare(v, *best);
    } else {
      c = v.as_double() < best->as_double() ? -1 : v.as_double() > best->as_double() ? 1 : 0;
    }
    if (c == want) best = &v;
  }
  if (!best) {
    if (saw_nil) return Value::nil();
    error = std::string(who) + "() of nothing";
    return Value::nil();
  }
  return *best;
}

Value bi_min(Vm&, std::vector<Value>& args, std::string& error) { return extreme(args, error, "min", -1); }

Value bi_max(Vm&, std::vector<Value>& args, std::string& error) { return extreme(args, error, "max", 1); }

Value bi_floor(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_numeric(args[0], error, "floor")) return Value::nil();
  if (args[0].type == Type::Decimal) {
    int64_t t = 0;
    if (!decimal_to_int(args[0], t)) { error = "floor(): the decimal does not fit in 64 bits"; return Value::nil(); }
    return Value::integer(decimal_compare(args[0], decimal_from_int(t)) < 0 ? t - 1 : t);
  }
  return Value::integer((int64_t)std::floor(args[0].as_double()));
}

Value bi_ceil(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_numeric(args[0], error, "ceil")) return Value::nil();
  if (args[0].type == Type::Decimal) {
    int64_t t = 0;
    if (!decimal_to_int(args[0], t)) { error = "ceil(): the decimal does not fit in 64 bits"; return Value::nil(); }
    return Value::integer(decimal_compare(args[0], decimal_from_int(t)) > 0 ? t + 1 : t);
  }
  return Value::integer((int64_t)std::ceil(args[0].as_double()));
}

Value bi_sqrt(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "sqrt")) return Value::nil();
  if (args[0].as_double() < 0) { error = "sqrt() of a negative number"; return Value::nil(); }
  return Value::real(std::sqrt(args[0].as_double()));
}

Value bi_pow(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "pow") || !want_number(args[1], error, "pow")) return Value::nil();
  return Value::real(std::pow(args[0].as_double(), args[1].as_double()));
}

// --- the rest --------------------------------------------------------------

Value bi_assert(Vm&, std::vector<Value>& args, std::string& error) {
  if (args[0].truthy()) return args[0];
  error = args.size() > 1 ? ("assertion failed: " + args[1].to_text()) : "assertion failed";
  return Value::nil();
}

Value bi_clock(Vm&, std::vector<Value>&, std::string&) {
  using namespace std::chrono;
  return Value::real((double)duration_cast<microseconds>(
      steady_clock::now().time_since_epoch()).count() / 1000.0);
}

// --- enhanced strings ------------------------------------------------------

Value bi_trim(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "trim")) return Value::nil();
  const std::string& s = args[0].str();
  std::string chars = args.size() > 1 && args[1].type == Type::Str ? args[1].str() : " \t\r\n";
  size_t start = s.find_first_not_of(chars);
  if (start == std::string::npos) return Value::string("");
  size_t end = s.find_last_not_of(chars);
  if (start == 0 && end + 1 == s.size()) return args[0];
  return Value::string(s.substr(start, end - start + 1));
}

// is_blank(v): true for nil and for a string of nothing but spaces, tabs and line breaks - a column left empty,
// whether it came back NULL or ''. Anything else (0, false, an empty list) is a value, so false.
Value bi_is_blank(Vm&, std::vector<Value>& args, std::string&) {
  const Value& v = args[0];
  if (v.type == Type::Nil) return Value::boolean(true);
  if (v.type != Type::Str) return Value::boolean(false);
  return Value::boolean(v.str().find_first_not_of(" \t\r\n") == std::string::npos);
}

// first_present(a, b, ...): the first argument that is not blank (is_blank: nil or only whitespace) - a name that may
// have come back NULL or '' falls through to the next; nil when every one is blank.
Value bi_first_present(Vm& vm, std::vector<Value>& args, std::string& error) {
  for (const Value& v : args) {
    std::vector<Value> one{v};
    if (!bi_is_blank(vm, one, error).b) return v;
  }
  return Value::nil();
}

// get_or_put(map, key, value): map[key] when it is there; otherwise value, put at key first. The record a key
// stands for, made the first time the key is seen. value is worked out before the call either way.
Value bi_get_or_put(Vm&, std::vector<Value>& args, std::string& error) {
  if (args[0].type != Type::Map) { error = std::string("get_or_put() wants a map, got a ") + type_name(args[0].type); return Value::nil(); }
  MapObj* m = static_cast<MapObj*>(args[0].obj.get());
  const std::string k = args[1].type == Type::Str ? args[1].str() : args[1].to_text();
  if (Value* have = m->find(k); have && have->type != Type::Nil) return *have;
  m->put(k, args[2]);
  return args[2];
}

// add_keys(map, list, [value]): map[item] = value (true) for every item of list - a map used as a set, len(map)
// its size. Returns the map.
Value bi_add_keys(Vm&, std::vector<Value>& args, std::string& error) {
  if (args[0].type != Type::Map) { error = std::string("add_keys() wants a map, got a ") + type_name(args[0].type); return Value::nil(); }
  if (args[1].type != Type::List) { error = std::string("add_keys() wants a list of keys, got a ") + type_name(args[1].type); return Value::nil(); }
  MapObj* m = static_cast<MapObj*>(args[0].obj.get());
  const Value val = args.size() > 2 ? args[2] : Value::boolean(true);
  for (const Value& k : args[1].list_items()) m->put(k.type == Type::Str ? k.str() : k.to_text(), val);
  return args[0];
}

// add_into(total, record, [fields]): adds record's numbers into total's, field by field, into nested maps too (a
// field total lacks starts at record's value; a nested map it lacks starts empty). Text, lists and nil are left
// alone. fields, when given, names the top-level fields to add. Returns total.
static bool add_into_map(MapObj* total, MapObj* rec, const std::vector<std::string>* only, int depth, const std::string& at,
                         std::string& error) {
  if (depth > 64) { error = "add_into(): nested more than 64 deep"; return false; }
  auto one = [&](const std::string& k, const Value& rv) -> bool {
    if (rv.type == Type::Nil) return true;
    Value* tv = total->find(k);
    const std::string where = at.empty() ? k : at + "." + k;
    const bool rnum = rv.is_number() || rv.type == Type::Decimal;
    if (rnum) {
      if (!tv || tv->type == Type::Nil) { total->put(k, rv); return true; }
      if (!(tv->is_number() || tv->type == Type::Decimal)) {
        error = "add_into(): the total's " + where + " is a " + type_name(tv->type) + ", not a number";
        return false;
      }
      Value sum;
      if (tv->type == Type::Decimal || rv.type == Type::Decimal) {
        if (!decimal_arith('+', *tv, rv, sum, error)) { error = "add_into(): " + where + ": " + error; return false; }
      } else if (tv->type == Type::Int && rv.type == Type::Int) {
        int64_t r = 0;
        sum = __builtin_add_overflow(tv->i, rv.i, &r) ? Value::real((double)tv->i + (double)rv.i) : Value::integer(r);
      } else {
        sum = Value::real(tv->as_double() + rv.as_double());
      }
      *tv = sum;
      return true;
    }
    if (rv.type == Type::Map) {
      if (!tv || tv->type == Type::Nil) {
        total->put(k, Value::object(Type::Map, std::make_shared<MapObj>()));
        tv = total->find(k);
      }
      if (tv->type != Type::Map) { error = "add_into(): the total's " + where + " is a " + type_name(tv->type) + ", not a map"; return false; }
      return add_into_map(static_cast<MapObj*>(tv->obj.get()), static_cast<MapObj*>(rv.obj.get()), nullptr, depth + 1, where, error);
    }
    return true;   // text, a list, a flag: not something to add
  };
  if (only) {
    for (const auto& k : *only)
      if (Value* rv = rec->find(k)) { if (!one(k, *rv)) return false; }
    return true;
  }
  const auto entries = rec->entries;   // a copy: total and record may be the same map
  for (const auto& [k, rv] : entries)
    if (!one(k, rv)) return false;
  return true;
}

Value bi_add_into(Vm&, std::vector<Value>& args, std::string& error) {
  if (args[0].type != Type::Map || args[1].type != Type::Map) {
    error = std::string("add_into() wants two maps, got a ") + type_name(args[0].type) + " and a " + type_name(args[1].type);
    return Value::nil();
  }
  std::vector<std::string> fields;
  const bool some = args.size() > 2 && args[2].type != Type::Nil;
  if (some) {
    if (args[2].type != Type::List) { error = "add_into(): fields is a list of names"; return Value::nil(); }
    for (const Value& f : args[2].list_items()) fields.push_back(f.type == Type::Str ? f.str() : f.to_text());
  }
  if (!add_into_map(static_cast<MapObj*>(args[0].obj.get()), static_cast<MapObj*>(args[1].obj.get()), some ? &fields : nullptr, 0, "", error))
    return Value::nil();
  return args[0];
}

// thing_rows(db, name, [keys]): SHOW THING name [KEYS (...)] over a db_connect connection, its rows as maps with the
// columns that hold JSON (a grain Thing's lists and maps) read back as values. An error is raised, not returned.
Value bi_thing_rows(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[1], error, "thing_rows")) return Value::nil();
  const std::string& name = args[1].str();
  if (name.empty() || !std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum((unsigned char)c) || c == '_'; })) {
    error = "thing_rows(): \"" + name + "\" is not a Thing's name";
    return Value::nil();
  }
  std::string sql = "SHOW THING " + name;
  if (args.size() > 2 && args[2].type != Type::Nil) {
    if (args[2].type != Type::List) { error = "thing_rows(): keys is a list"; return Value::nil(); }
    const auto& keys = args[2].list_items();
    if (keys.empty()) return Value::list({});
    sql += " KEYS (";
    for (size_t i = 0; i < keys.size(); ++i) {
      const Value& k = keys[i];
      if (i) sql += ", ";
      if (k.is_number()) { sql += k.to_text(); continue; }
      if (k.type != Type::Str) { error = std::string("thing_rows(): a key is text or a number, not a ") + type_name(k.type); return Value::nil(); }
      sql += "'";
      for (char c : k.str()) {
        if (c == '\'') sql += "''";
        else if (c == '\\') sql += "\\\\";
        else if (c == '\0') sql += "\\0";
        else sql += c;
      }
      sql += "'";
    }
    sql += ")";
  }
  Value* q = vm.find_global("db_query");
  if (!q) { error = "thing_rows(): this program has no db_query"; return Value::nil(); }
  Value r = vm.call(*q, {args[0], Value::string(sql)}, error);
  if (!error.empty()) return Value::nil();
  if (r.type != Type::Map) { error = "thing_rows(): db_query gave no result"; return Value::nil(); }
  MapObj* rm = static_cast<MapObj*>(r.obj.get());
  Value* ok = rm->find("ok");
  if (!ok || !ok->truthy()) {
    Value* e = rm->find("error");
    error = "thing_rows(" + name + "): " + (e ? e->to_text() : std::string("the query failed"));
    return Value::nil();
  }
  // grain's db_query gives records (maps); aios's gives columns and rows (lists): the same rows either way
  std::vector<std::vector<std::pair<std::string, Value>>> rows;
  if (Value* recs = rm->find("records"); recs && recs->type == Type::List) {
    for (const Value& row : recs->list_items())
      if (row.type == Type::Map) rows.push_back(static_cast<MapObj*>(row.obj.get())->entries);
  } else if (Value* cols = rm->find("columns"), *rs = rm->find("rows"); cols && rs && cols->type == Type::List && rs->type == Type::List) {
    const auto& names = cols->list_items();
    for (const Value& row : rs->list_items()) {
      if (row.type != Type::List) continue;
      std::vector<std::pair<std::string, Value>> fields;
      const auto& vals = row.list_items();
      for (size_t i = 0; i < names.size() && i < vals.size(); ++i) fields.push_back({names[i].to_text(), vals[i]});
      rows.push_back(std::move(fields));
    }
  }
  std::vector<Value> out;
  {
    for (const auto& row : rows) {
      auto copy = std::make_shared<MapObj>();
      for (const auto& [k, v] : row) {
        if (v.type == Type::Str && !v.str().empty() && (v.str()[0] == '{' || v.str()[0] == '[')) {
          std::string jerr;
          Value parsed = grain_json_parse(v.str(), jerr);
          copy->put(k, jerr.empty() ? parsed : v);
        } else {
          copy->put(k, v);
        }
      }
      out.push_back(Value::object(Type::Map, copy));
    }
  }
  return Value::list(std::move(out));
}

Value bi_ltrim(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "ltrim")) return Value::nil();
  const std::string& s = args[0].str();
  std::string chars = args.size() > 1 && args[1].type == Type::Str ? args[1].str() : " \t\r\n";
  size_t start = s.find_first_not_of(chars);
  if (start == std::string::npos) return Value::string("");
  if (start == 0) return args[0];
  return Value::string(s.substr(start));
}

Value bi_rtrim(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "rtrim")) return Value::nil();
  const std::string& s = args[0].str();
  std::string chars = args.size() > 1 && args[1].type == Type::Str ? args[1].str() : " \t\r\n";
  size_t end = s.find_last_not_of(chars);
  if (end == std::string::npos) return Value::string("");
  if (end + 1 == s.size()) return args[0];
  return Value::string(s.substr(0, end + 1));
}

// upper_rtrim(v) = upper(rtrim(str(v), " ")): a key as MariaDB's default collation compares codes (trailing
// spaces dropped, case folded), the one call a lookup makes for every key - and the value itself when it is
// already in that form.
Value bi_upper_rtrim(Vm&, std::vector<Value>& args, std::string&) {
  if (args[0].type != Type::Str || !args[0].obj) {
    std::string t = args[0].to_text();
    size_t n = t.size();
    while (n > 0 && t[n - 1] == ' ') --n;
    t.resize(n);
    for (char& c : t) if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    return Value::string(std::move(t));
  }
  const std::string& in = args[0].str();
  size_t n = in.size();
  while (n > 0 && in[n - 1] == ' ') --n;
  bool lower = false;
  for (size_t i = 0; i < n && !lower; ++i) lower = in[i] >= 'a' && in[i] <= 'z';
  if (!lower && n == in.size()) return args[0];
  std::string t = in.substr(0, n);
  for (char& c : t) if (c >= 'a' && c <= 'z') c = (char)(c - 32);
  return Value::string(std::move(t));
}

Value bi_starts_with(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "starts_with") || !want_str(args[1], error, "starts_with")) return Value::nil();
  const std::string& s = args[0].str();
  const std::string& prefix = args[1].str();
  if (prefix.size() > s.size()) return Value::boolean(false);
  return Value::boolean(s.compare(0, prefix.size(), prefix) == 0);
}

Value bi_ends_with(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "ends_with") || !want_str(args[1], error, "ends_with")) return Value::nil();
  const std::string& s = args[0].str();
  const std::string& suffix = args[1].str();
  if (suffix.size() > s.size()) return Value::boolean(false);
  return Value::boolean(s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0);
}

Value bi_contains(Vm&, std::vector<Value>& args, std::string& error) {
  const Value& v = args[0];
  if (v.type == Type::Str) {
    if (!want_str(args[1], error, "contains")) return Value::nil();
    return Value::boolean(v.str().find(args[1].str()) != std::string::npos);
  }
  if (v.type == Type::List) {
    for (const auto& item : v.list_items()) {
      if (item.equals(args[1])) return Value::boolean(true);
    }
    return Value::boolean(false);
  }
  if (v.type == Type::Map) {
    if (args[1].type == Type::Str) {
      return Value::boolean(static_cast<MapObj*>(v.obj.get())->find(args[1].str()) != nullptr);
    }
    return Value::boolean(false);
  }
  error = std::string("contains() wants a string, list or map, got a ") + type_name(v.type);
  return Value::nil();
}

Value bi_index_of(Vm&, std::vector<Value>& args, std::string& error) {
  const Value& v = args[0];
  if (v.type == Type::Str) {
    if (!want_str(args[1], error, "index_of")) return Value::nil();
    size_t start = 0;
    if (args.size() > 2) {
      if (!want_number(args[2], error, "index_of")) return Value::nil();
      int64_t s = args[2].as_int();
      start = s < 0 ? 0 : (size_t)s;
    }
    size_t pos = v.str().find(args[1].str(), start);
    return Value::integer(pos == std::string::npos ? -1 : (int64_t)pos);
  }
  if (v.type == Type::List) {
    const auto& items = v.list_items();
    size_t start = 0;
    if (args.size() > 2) {
      if (!want_number(args[2], error, "index_of")) return Value::nil();
      int64_t s = args[2].as_int();
      start = s < 0 ? 0 : (size_t)s;
    }
    for (size_t i = start; i < items.size(); ++i) {
      if (items[i].equals(args[1])) return Value::integer((int64_t)i);
    }
    return Value::integer(-1);
  }
  error = std::string("index_of() wants a string or list, got a ") + type_name(v.type);
  return Value::nil();
}

Value bi_last_index_of(Vm&, std::vector<Value>& args, std::string& error) {
  const Value& v = args[0];
  if (v.type == Type::Str) {
    if (!want_str(args[1], error, "last_index_of")) return Value::nil();
    size_t pos = v.str().rfind(args[1].str());
    return Value::integer(pos == std::string::npos ? -1 : (int64_t)pos);
  }
  if (v.type == Type::List) {
    const auto& items = v.list_items();
    for (size_t i = items.size(); i-- > 0;) {
      if (items[i].equals(args[1])) return Value::integer((int64_t)i);
    }
    return Value::integer(-1);
  }
  error = std::string("last_index_of() wants a string or list, got a ") + type_name(v.type);
  return Value::nil();
}

Value bi_pad_left(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "pad_left") || !want_number(args[1], error, "pad_left")) return Value::nil();
  const std::string& s = args[0].str();
  int64_t target_len = args[1].as_int();
  const int64_t have = (int64_t)utf8_count(s);   // characters, so padded columns line up
  if (target_len <= have) return args[0];
  std::string pad_char = " ";
  if (args.size() > 2 && args[2].type == Type::Str && !args[2].str().empty()) {
    pad_char = args[2].str().substr(0, utf8_char_len(args[2].str(), 0));
  }
  if (target_len > 1000000) { error = "pad_left() target length is too large"; return Value::nil(); }
  std::string out;
  for (int64_t i = have; i < target_len; ++i) out += pad_char;
  out += s;
  return Value::string(out);
}

Value bi_pad_right(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "pad_right") || !want_number(args[1], error, "pad_right")) return Value::nil();
  const std::string& s = args[0].str();
  int64_t target_len = args[1].as_int();
  const int64_t have = (int64_t)utf8_count(s);
  if (target_len <= have) return args[0];
  std::string pad_char = " ";
  if (args.size() > 2 && args[2].type == Type::Str && !args[2].str().empty()) {
    pad_char = args[2].str().substr(0, utf8_char_len(args[2].str(), 0));
  }
  if (target_len > 1000000) { error = "pad_right() target length is too large"; return Value::nil(); }
  std::string out = s;
  for (int64_t i = have; i < target_len; ++i) out += pad_char;
  return Value::string(out);
}

Value bi_repeat(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "repeat") || !want_number(args[1], error, "repeat")) return Value::nil();
  const std::string& s = args[0].str();
  int64_t count = args[1].as_int();
  if (count <= 0) return Value::string("");
  if (count * (int64_t)s.size() > 10000000) { error = "repeat() result is too large"; return Value::nil(); }
  std::string out;
  out.reserve((size_t)(count * s.size()));
  for (int64_t i = 0; i < count; ++i) out += s;
  return Value::string(out);
}

Value bi_char_at(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "char_at") || !want_number(args[1], error, "char_at")) return Value::nil();
  const std::string& s = args[0].str();
  int64_t idx = args[1].as_int();
  if (idx < 0) idx += (int64_t)utf8_count(s);   // a character index, from the end when negative
  if (idx < 0) return Value::string("");
  size_t at = 0;
  for (int64_t i = 0; at < s.size(); ++i) {
    const size_t n = utf8_char_len(s, at);
    if (i == idx) return Value::string(s.substr(at, n));
    at += n;
  }
  return Value::string("");
}

Value bi_chars(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "chars")) return Value::nil();
  const std::string& s = args[0].str();
  std::vector<Value> out;
  out.reserve(s.size());
  for (size_t at = 0; at < s.size();) {
    const size_t n = utf8_char_len(s, at);
    out.push_back(Value::string(s.substr(at, n)));
    at += n;
  }
  return Value::list(std::move(out));
}

Value bi_is_digit(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "is_digit")) return Value::nil();
  const std::string& s = args[0].str();
  if (s.empty()) return Value::boolean(false);
  for (unsigned char c : s) if (!std::isdigit(c)) return Value::boolean(false);
  return Value::boolean(true);
}

Value bi_is_alpha(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "is_alpha")) return Value::nil();
  const std::string& s = args[0].str();
  if (s.empty()) return Value::boolean(false);
  for (unsigned char c : s) if (!std::isalpha(c)) return Value::boolean(false);
  return Value::boolean(true);
}

Value bi_is_alnum(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "is_alnum")) return Value::nil();
  const std::string& s = args[0].str();
  if (s.empty()) return Value::boolean(false);
  for (unsigned char c : s) if (!std::isalnum(c)) return Value::boolean(false);
  return Value::boolean(true);
}

Value bi_is_space(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "is_space")) return Value::nil();
  const std::string& s = args[0].str();
  if (s.empty()) return Value::boolean(false);
  for (unsigned char c : s) if (!std::isspace(c)) return Value::boolean(false);
  return Value::boolean(true);
}

// --- collections -----------------------------------------------------------

Value bi_map(Vm& vm, std::vector<Value>& args, std::string& error) {
  const Value& col = args[0];
  const Value& fn = args[1];
  if (col.type == Type::List) {
    const auto& items = col.list_items();
    std::vector<Value> out;
    out.reserve(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
      Value res = vm.call(fn, {items[i]}, error);
      if (!error.empty()) return Value::nil();
      out.push_back(std::move(res));
    }
    return Value::list(std::move(out));
  }
  if (col.type == Type::Map) {
    auto* m = static_cast<MapObj*>(col.obj.get());
    std::vector<Value> out;
    out.reserve(m->entries.size());
    for (const auto& [k, v] : m->entries) {
      Value res = vm.call(fn, {Value::string(k), v}, error);
      if (!error.empty()) return Value::nil();
      out.push_back(std::move(res));
    }
    return Value::list(std::move(out));
  }
  error = std::string("map() wants a list or map, got a ") + type_name(col.type);
  return Value::nil();
}

// flat_map(list, fn): fn's results joined into one list - a list result contributes its items, nil nothing, any
// other value itself. One level, as JavaScript's flatMap: `flat_map(orders, fn (o) { return o.lines })`.
Value bi_flat_map(Vm& vm, std::vector<Value>& args, std::string& error) {
  const Value& col = args[0];
  const Value& fn = args[1];
  std::vector<Value> out;
  auto take = [&](Value res) {
    if (res.type == Type::List) { const auto& items = res.list_items(); out.insert(out.end(), items.begin(), items.end()); }
    else if (res.type != Type::Nil) out.push_back(std::move(res));
  };
  if (col.type == Type::List) {
    const auto& items = col.list_items();
    out.reserve(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
      Value res = vm.call(fn, {items[i]}, error);
      if (!error.empty()) return Value::nil();
      take(std::move(res));
    }
    return Value::list(std::move(out));
  }
  if (col.type == Type::Map) {
    auto* m = static_cast<MapObj*>(col.obj.get());
    for (const auto& [k, v] : m->entries) {
      Value res = vm.call(fn, {Value::string(k), v}, error);
      if (!error.empty()) return Value::nil();
      take(std::move(res));
    }
    return Value::list(std::move(out));
  }
  error = std::string("flat_map() wants a list or map, got a ") + type_name(col.type);
  return Value::nil();
}

Value bi_filter(Vm& vm, std::vector<Value>& args, std::string& error) {
  const Value& col = args[0];
  const Value& fn = args[1];
  if (col.type == Type::List) {
    const auto& items = col.list_items();
    std::vector<Value> out;
    for (const auto& item : items) {
      Value res = vm.call(fn, {item}, error);
      if (!error.empty()) return Value::nil();
      if (res.truthy()) out.push_back(item);
    }
    return Value::list(std::move(out));
  }
  if (col.type == Type::Map) {
    auto* m = static_cast<MapObj*>(col.obj.get());
    auto out_map = std::make_shared<MapObj>();
    for (const auto& [k, v] : m->entries) {
      Value res = vm.call(fn, {Value::string(k), v}, error);
      if (!error.empty()) return Value::nil();
      if (res.truthy()) out_map->put(k, v);
    }
    return Value::object(Type::Map, out_map);
  }
  error = std::string("filter() wants a list or map, got a ") + type_name(col.type);
  return Value::nil();
}

Value bi_reduce(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_list(args[0], error, "reduce")) return Value::nil();
  const auto& items = args[0].list_items();
  const Value& fn = args[1];
  if (items.empty() && args.size() < 3) {
    error = "reduce() of empty list with no initial value";
    return Value::nil();
  }
  Value acc = args.size() > 2 ? args[2] : (items.empty() ? Value::nil() : items[0]);
  size_t start = args.size() > 2 ? 0 : 1;
  for (size_t i = start; i < items.size(); ++i) {
    acc = vm.call(fn, {acc, items[i]}, error);
    if (!error.empty()) return Value::nil();
  }
  return acc;
}

Value bi_each(Vm& vm, std::vector<Value>& args, std::string& error) {
  const Value& col = args[0];
  const Value& fn = args[1];
  if (col.type == Type::List) {
    for (const auto& item : col.list_items()) {
      vm.call(fn, {item}, error);
      if (!error.empty()) return Value::nil();
    }
    return Value::nil();
  }
  if (col.type == Type::Map) {
    auto* m = static_cast<MapObj*>(col.obj.get());
    for (const auto& [k, v] : m->entries) {
      vm.call(fn, {Value::string(k), v}, error);
      if (!error.empty()) return Value::nil();
    }
    return Value::nil();
  }
  error = std::string("each() wants a list or map, got a ") + type_name(col.type);
  return Value::nil();
}

Value bi_any(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_list(args[0], error, "any")) return Value::nil();
  const auto& items = args[0].list_items();
  const Value& fn = args[1];
  for (const auto& item : items) {
    Value res = vm.call(fn, {item}, error);
    if (!error.empty()) return Value::nil();
    if (res.truthy()) return Value::boolean(true);
  }
  return Value::boolean(false);
}

Value bi_all(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_list(args[0], error, "all")) return Value::nil();
  const auto& items = args[0].list_items();
  const Value& fn = args[1];
  for (const auto& item : items) {
    Value res = vm.call(fn, {item}, error);
    if (!error.empty()) return Value::nil();
    if (!res.truthy()) return Value::boolean(false);
  }
  return Value::boolean(true);
}

Value bi_find_if(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_list(args[0], error, "find_if")) return Value::nil();
  const auto& items = args[0].list_items();
  const Value& fn = args[1];
  for (const auto& item : items) {
    Value res = vm.call(fn, {item}, error);
    if (!error.empty()) return Value::nil();
    if (res.truthy()) return item;
  }
  return Value::nil();
}

namespace {
void flatten_helper(const Value& v, int64_t depth, std::vector<Value>& out) {
  if (depth != 0 && v.type == Type::List) {
    for (const auto& item : v.list_items()) {
      flatten_helper(item, depth > 0 ? depth - 1 : -1, out);
    }
  } else {
    out.push_back(v);
  }
}
}  // namespace

Value bi_flatten(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_list(args[0], error, "flatten")) return Value::nil();
  int64_t depth = 1;
  if (args.size() > 1) {
    if (!want_number(args[1], error, "flatten")) return Value::nil();
    depth = args[1].as_int();
  }
  /* depth counts levels INSIDE the list: flatten([1, [2, [3]]]) is
   * [1, 2, [3]]. Handing the outer list itself to the helper spent the one
   * level on it, so a default flatten returned its argument unchanged. */
  std::vector<Value> out;
  for (const auto& item : args[0].list_items()) flatten_helper(item, depth, out);
  return Value::list(std::move(out));
}

Value bi_concat(Vm&, std::vector<Value>& args, std::string& error) {
  if (args.empty()) return Value::nil();
  if (args[0].type == Type::List) {
    std::vector<Value> out;
    for (const auto& a : args) {
      if (a.type == Type::List) {
        for (const auto& it : a.list_items()) out.push_back(it);
      } else {
        out.push_back(a);
      }
    }
    return Value::list(std::move(out));
  }
  if (args[0].type == Type::Str) {
    std::string out;
    for (const auto& a : args) out += a.to_text();
    return Value::string(out);
  }
  error = std::string("concat() wants lists or strings, got a ") + type_name(args[0].type);
  return Value::nil();
}

Value bi_insert(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_list(args[0], error, "insert") || !want_number(args[1], error, "insert")) return Value::nil();
  auto& items = args[0].list_items();
  int64_t idx = args[1].as_int();
  if (idx < 0) idx += (int64_t)items.size() + 1;
  if (idx < 0 || idx > (int64_t)items.size()) {
    error = "insert() index out of bounds";
    return Value::nil();
  }
  if (args[2].is_container()) vm.track_container(args[0].obj);
  items.insert(items.begin() + idx, args[2]);
  return args[0];
}

Value bi_remove_at(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_list(args[0], error, "remove_at") || !want_number(args[1], error, "remove_at")) return Value::nil();
  auto& items = args[0].list_items();
  int64_t idx = args[1].as_int();
  if (idx < 0) idx += (int64_t)items.size();
  if (idx < 0 || idx >= (int64_t)items.size()) {
    error = "remove_at() index out of bounds";
    return Value::nil();
  }
  Value removed = items[(size_t)idx];
  items.erase(items.begin() + idx);
  return removed;
}

Value bi_zip(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_list(args[0], error, "zip") || !want_list(args[1], error, "zip")) return Value::nil();
  const auto& l1 = args[0].list_items();
  const auto& l2 = args[1].list_items();
  size_t n = std::min(l1.size(), l2.size());
  std::vector<Value> out;
  out.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    out.push_back(Value::list({l1[i], l2[i]}));
  }
  return Value::list(std::move(out));
}

Value bi_enumerate(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_list(args[0], error, "enumerate")) return Value::nil();
  const auto& items = args[0].list_items();
  std::vector<Value> out;
  out.reserve(items.size());
  for (size_t i = 0; i < items.size(); ++i) {
    out.push_back(Value::list({Value::integer((int64_t)i), items[i]}));
  }
  return Value::list(std::move(out));
}

Value bi_take(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[1], error, "take")) return Value::nil();
  int64_t n = std::max<int64_t>(0, args[1].as_int());
  if (args[0].type == Type::List) {
    const auto& items = args[0].list_items();
    size_t count = std::min((size_t)n, items.size());
    return Value::list(std::vector<Value>(items.begin(), items.begin() + count));
  }
  if (args[0].type == Type::Str) {
    const std::string& s = args[0].str();
    size_t count = std::min((size_t)n, s.size());
    return Value::string(s.substr(0, count));
  }
  error = std::string("take() wants a list or string, got a ") + type_name(args[0].type);
  return Value::nil();
}

Value bi_drop(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[1], error, "drop")) return Value::nil();
  int64_t n = std::max<int64_t>(0, args[1].as_int());
  if (args[0].type == Type::List) {
    const auto& items = args[0].list_items();
    size_t start = std::min((size_t)n, items.size());
    return Value::list(std::vector<Value>(items.begin() + start, items.end()));
  }
  if (args[0].type == Type::Str) {
    const std::string& s = args[0].str();
    size_t start = std::min((size_t)n, s.size());
    return Value::string(s.substr(start));
  }
  error = std::string("drop() wants a list or string, got a ") + type_name(args[0].type);
  return Value::nil();
}

Value bi_count(Vm&, std::vector<Value>& args, std::string& error) {
  if (args[0].type == Type::List) {
    const auto& items = args[0].list_items();
    int64_t c = 0;
    for (const auto& it : items) {
      if (it.equals(args[1])) c++;
    }
    return Value::integer(c);
  }
  if (args[0].type == Type::Str) {
    if (!want_str(args[1], error, "count")) return Value::nil();
    const std::string& s = args[0].str();
    const std::string& sub = args[1].str();
    if (sub.empty()) return Value::integer(0);
    int64_t c = 0;
    size_t pos = 0;
    while ((pos = s.find(sub, pos)) != std::string::npos) {
      c++;
      pos += sub.size();
    }
    return Value::integer(c);
  }
  error = std::string("count() wants a list or string, got a ") + type_name(args[0].type);
  return Value::nil();
}

Value bi_unique(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_list(args[0], error, "unique")) return Value::nil();
  const auto& items = args[0].list_items();
  std::vector<Value> out;
  // Strings only (the usual case: keys, codes): a set of them, first seen kept - comparing each item with every one
  // kept before was n^2, seconds for the 20,000 item codes a shipper's open orders name.
  bool all_str = true;
  for (const auto& it : items) if (it.type != Type::Str) { all_str = false; break; }
  if (all_str) {
    std::unordered_set<std::string> seen;
    seen.reserve(items.size());
    for (const auto& it : items) if (seen.insert(it.str()).second) out.push_back(it);
    return Value::list(std::move(out));
  }
  for (const auto& it : items) {
    bool found = false;
    for (const auto& existing : out) {
      if (existing.equals(it)) { found = true; break; }
    }
    if (!found) out.push_back(it);
  }
  return Value::list(std::move(out));
}

// compact(list): the list without its nils, in order.
Value bi_compact(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_list(args[0], error, "compact")) return Value::nil();
  std::vector<Value> out;
  for (const auto& it : args[0].list_items()) if (it.type != Type::Nil) out.push_back(it);
  return Value::list(std::move(out));
}

Value bi_merge(Vm&, std::vector<Value>& args, std::string& error) {
  auto out = std::make_shared<MapObj>();
  for (size_t i = 0; i < args.size(); ++i) {
    if (args[i].type != Type::Map) {
      error = std::string("merge() wants maps, got a ") + type_name(args[i].type);
      return Value::nil();
    }
    auto* m = static_cast<MapObj*>(args[i].obj.get());
    for (const auto& [k, v] : m->entries) {
      out->put(k, v);
    }
  }
  return Value::object(Type::Map, out);
}

Value bi_entries(Vm&, std::vector<Value>& args, std::string& error) {
  if (args[0].type != Type::Map) { error = "entries() wants a map"; return Value::nil(); }
  auto* m = static_cast<MapObj*>(args[0].obj.get());
  std::vector<Value> out;
  out.reserve(m->entries.size());
  for (const auto& [k, v] : m->entries) {
    out.push_back(Value::list({Value::string(k), v}));
  }
  return Value::list(std::move(out));
}

Value bi_from_entries(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_list(args[0], error, "from_entries")) return Value::nil();
  auto m = std::make_shared<MapObj>();
  for (const auto& pair : args[0].list_items()) {
    if (pair.type != Type::List || pair.list_items().size() < 2 || pair.list_items()[0].type != Type::Str) {
      error = "from_entries() wants a list of [string_key, value] pairs";
      return Value::nil();
    }
    m->put(pair.list_items()[0].str(), pair.list_items()[1]);
  }
  return Value::object(Type::Map, m);
}

// --- advanced math ---------------------------------------------------------

Value bi_round(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_numeric(args[0], error, "round")) return Value::nil();
  if (args[0].type == Type::Decimal) {
    // exact, half away from zero: round(2.675d, 2) is 2.68, which no real can say
    int places = 0;
    if (args.size() > 1) {
      if (!want_number(args[1], error, "round")) return Value::nil();
      places = (int)args[1].as_int();
    }
    Value r;
    if (!decimal_round(args[0], places, r, error)) return Value::nil();
    if (args.size() > 1) return r;
    int64_t n = 0;
    if (!decimal_to_int(r, n)) { error = "round(): the decimal does not fit in 64 bits"; return Value::nil(); }
    return Value::integer(n);
  }
  double d = args[0].as_double();
  if (args.size() > 1) {
    if (!want_number(args[1], error, "round")) return Value::nil();
    int64_t decimals = args[1].as_int();
    double factor = std::pow(10.0, (double)decimals);
    return Value::real(std::round(d * factor) / factor);
  }
  return Value::integer((int64_t)std::round(d));
}

Value bi_trunc(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_numeric(args[0], error, "trunc")) return Value::nil();
  if (args[0].type == Type::Decimal) {
    int64_t n = 0;
    if (!decimal_to_int(args[0], n)) { error = "trunc(): the decimal does not fit in 64 bits"; return Value::nil(); }
    return Value::integer(n);
  }
  return Value::integer((int64_t)std::trunc(args[0].as_double()));
}

Value bi_sign(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_numeric(args[0], error, "sign")) return Value::nil();
  if (args[0].type == Type::Decimal) return Value::integer(decimal_compare(args[0], Value::integer(0)));
  double d = args[0].as_double();
  if (d > 0.0) return Value::integer(1);
  if (d < 0.0) return Value::integer(-1);
  return Value::integer(0);
}

Value bi_clamp(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "clamp") || !want_number(args[1], error, "clamp") || !want_number(args[2], error, "clamp")) return Value::nil();
  double val = args[0].as_double();
  double lo = args[1].as_double();
  double hi = args[2].as_double();
  if (lo > hi) std::swap(lo, hi);
  double clamped = std::max(lo, std::min(val, hi));
  if (args[0].type == Type::Int && args[1].type == Type::Int && args[2].type == Type::Int) {
    return Value::integer((int64_t)clamped);
  }
  return Value::real(clamped);
}

Value bi_sin(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "sin")) return Value::nil();
  return Value::real(std::sin(args[0].as_double()));
}

Value bi_cos(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "cos")) return Value::nil();
  return Value::real(std::cos(args[0].as_double()));
}

Value bi_tan(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "tan")) return Value::nil();
  return Value::real(std::tan(args[0].as_double()));
}

Value bi_asin(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "asin")) return Value::nil();
  double d = args[0].as_double();
  if (d < -1.0 || d > 1.0) { error = "asin() domain error: argument must be between -1.0 and 1.0"; return Value::nil(); }
  return Value::real(std::asin(d));
}

Value bi_acos(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "acos")) return Value::nil();
  double d = args[0].as_double();
  if (d < -1.0 || d > 1.0) { error = "acos() domain error: argument must be between -1.0 and 1.0"; return Value::nil(); }
  return Value::real(std::acos(d));
}

Value bi_atan(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "atan")) return Value::nil();
  return Value::real(std::atan(args[0].as_double()));
}

Value bi_atan2(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "atan2") || !want_number(args[1], error, "atan2")) return Value::nil();
  return Value::real(std::atan2(args[0].as_double(), args[1].as_double()));
}

Value bi_log(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "log")) return Value::nil();
  double d = args[0].as_double();
  if (d <= 0) { error = "log() domain error: argument must be > 0"; return Value::nil(); }
  return Value::real(std::log(d));
}

Value bi_log2(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "log2")) return Value::nil();
  double d = args[0].as_double();
  if (d <= 0) { error = "log2() domain error: argument must be > 0"; return Value::nil(); }
  return Value::real(std::log2(d));
}

Value bi_log10(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "log10")) return Value::nil();
  double d = args[0].as_double();
  if (d <= 0) { error = "log10() domain error: argument must be > 0"; return Value::nil(); }
  return Value::real(std::log10(d));
}

Value bi_exp(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "exp")) return Value::nil();
  return Value::real(std::exp(args[0].as_double()));
}

Value bi_hypot(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "hypot") || !want_number(args[1], error, "hypot")) return Value::nil();
  return Value::real(std::hypot(args[0].as_double(), args[1].as_double()));
}

// haversine(lat1, lng1, lat2, lng2): the great-circle distance in metres, on
// the same 3958.8-mile sphere as the ERP's route kernels (compute/erp_kernels),
// so a distance grain computes agrees with the one a route was planned on.
Value bi_haversine(Vm&, std::vector<Value>& args, std::string& error) {
  for (int i = 0; i < 4; ++i)
    if (!want_number(args[i], error, "haversine")) return Value::nil();
  const double lat1 = args[0].as_double(), lng1 = args[1].as_double(), lat2 = args[2].as_double(), lng2 = args[3].as_double();
  if (lat1 == lat2 && lng1 == lng2) return Value::real(0.0);
  const double rad = M_PI / 180.0;
  const double dlat = (lat2 - lat1) * rad, dlng = (lng2 - lng1) * rad;
  const double a = std::sin(dlat / 2) * std::sin(dlat / 2) +
                   std::cos(lat1 * rad) * std::cos(lat2 * rad) * std::sin(dlng / 2) * std::sin(dlng / 2);
  return Value::real(3958.8 * 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a)) * 1609.344);
}

Value bi_rad(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "rad")) return Value::nil();
  return Value::real(args[0].as_double() * (M_PI / 180.0));
}

Value bi_deg(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "deg")) return Value::nil();
  return Value::real(args[0].as_double() * (180.0 / M_PI));
}

namespace {
std::mt19937_64 g_grain_rng(1337);
}

Value bi_rand(Vm&, std::vector<Value>& args, std::string& error) {
  if (args.empty()) {
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    return Value::real(dist(g_grain_rng));
  }
  if (args.size() == 1) {
    if (!want_number(args[0], error, "rand")) return Value::nil();
    if (args[0].type == Type::Int) {
      int64_t max_val = args[0].i;
      if (max_val <= 0) return Value::integer(0);
      std::uniform_int_distribution<int64_t> dist(0, max_val - 1);
      return Value::integer(dist(g_grain_rng));
    }
    std::uniform_real_distribution<double> dist(0.0, args[0].as_double());
    return Value::real(dist(g_grain_rng));
  }
  if (!want_number(args[0], error, "rand") || !want_number(args[1], error, "rand")) return Value::nil();
  if (args[0].type == Type::Int && args[1].type == Type::Int) {
    int64_t min_v = args[0].i, max_v = args[1].i;
    if (min_v > max_v) std::swap(min_v, max_v);
    std::uniform_int_distribution<int64_t> dist(min_v, max_v);
    return Value::integer(dist(g_grain_rng));
  }
  double min_d = args[0].as_double(), max_d = args[1].as_double();
  if (min_d > max_d) std::swap(min_d, max_d);
  std::uniform_real_distribution<double> dist(min_d, max_d);
  return Value::real(dist(g_grain_rng));
}

Value bi_seed(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "seed")) return Value::nil();
  g_grain_rng.seed((uint64_t)args[0].as_int());
  return Value::nil();
}

Value bi_pi(Vm&, std::vector<Value>&, std::string&) {
  return Value::real(3.14159265358979323846);
}

Value bi_e(Vm&, std::vector<Value>&, std::string&) {
  return Value::real(2.71828182845904523536);
}

// --- system & environment --------------------------------------------------

Value bi_getenv(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "getenv")) return Value::nil();
  if (vm.settings) {   // a hosted app's own settings come first (Vm::settings)
    std::lock_guard<std::mutex> lk(vm.settings->mu);
    auto it = vm.settings->values.find(args[0].str());
    if (it != vm.settings->values.end()) return Value::string(it->second);
  }
  // The process's own variables only when the host allows it (Vm::allow_env): an app host's hold the fleet keys.
  if (vm.allow_env)
    if (const char* val = std::getenv(args[0].str().c_str())) return Value::string(val);
  return args.size() > 1 ? args[1] : Value::nil();
}

Value bi_setenv(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "setenv") || !want_str(args[1], error, "setenv")) return Value::nil();
  // A hosted app, or any VM not allowed the process's environment, changes its own settings, not the process
  // every app shares.
  if (vm.settings || !vm.allow_env) {
    if (!vm.settings) vm.settings = std::make_shared<VmSettings>();
    std::lock_guard<std::mutex> lk(vm.settings->mu);
    vm.settings->values[args[0].str()] = args[1].str();
    return Value::boolean(true);
  }
  int r = setenv(args[0].str().c_str(), args[1].str().c_str(), 1);
  return Value::boolean(r == 0);
}

Value bi_time(Vm&, std::vector<Value>&, std::string&) {
  using namespace std::chrono;
  auto now = system_clock::now().time_since_epoch();
  return Value::real((double)duration_cast<microseconds>(now).count() / 1e6);
}

Value bi_time_ms(Vm&, std::vector<Value>&, std::string&) {
  using namespace std::chrono;
  auto now = system_clock::now().time_since_epoch();
  return Value::integer((int64_t)duration_cast<milliseconds>(now).count());
}

Value bi_sleep(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "sleep")) return Value::nil();
  int64_t ms = args[0].as_int();
  if (ms > 0 && vm.stop_flag) {   // a hosted app: in slices, so its host's stop is not held up by a long sleep
    while (ms > 0 && !vm.stop_flag->load()) {
      const int64_t step = ms < 50 ? ms : 50;
      aios::hal::sleep_ms((uint32_t)step);
      ms -= step;
    }
    return Value::nil();
  }
  if (ms > 0) {
    aios::hal::sleep_ms((uint32_t)ms);
  }
  return Value::nil();
}

Value bi_gc(Vm& vm, std::vector<Value>&, std::string&) {
  return Value::integer(static_cast<int64_t>(vm.collect_cycles()));
}

// Stops at the next line when `grain debug` is driving; anywhere else there is nobody to stop for, so it does nothing.
Value bi_debugger(Vm& vm, std::vector<Value>&, std::string&) {
  if (!vm.debug_state.on_break) return Value::nil();
  vm.debug_state.enabled = true;
  vm.debug_state.step_line = true;
  return Value::nil();
}

Value bi_is_stopping(Vm& vm, std::vector<Value>&, std::string&) {
  return Value::boolean(vm.stop_flag && vm.stop_flag->load());
}

Value bi_uuid_v4(Vm&, std::vector<Value>&, std::string&) {
  uint8_t b[16];
  std::random_device rd;
  for (int i = 0; i < 16; i += 4) {
    uint32_t r = rd();
    std::memcpy(&b[i], &r, 4);
  }
  b[6] = (b[6] & 0x0f) | 0x40; // version 4
  b[8] = (b[8] & 0x3f) | 0x80; // variant 1 (RFC 4122)
  char buf[37];
  std::snprintf(buf, sizeof(buf),
    "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
    b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
    b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
  return Value::string(buf);
}

Value bi_iso8601(Vm&, std::vector<Value>& args, std::string& error) {
  double t = 0;
  if (args.empty()) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
    t = static_cast<double>(ms) / 1000.0;
  } else {
    if (!want_number(args[0], error, "iso8601")) return Value::nil();
    t = args[0].as_double();
    if (t > 1e11) t /= 1000.0;
  }
  time_t sec = static_cast<time_t>(t);
  struct tm tm_buf;
#if defined(_WIN32)
  gmtime_s(&tm_buf, &sec);
#else
  gmtime_r(&sec, &tm_buf);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
  return Value::string(buf);
}

Value bi_get_in(Vm&, std::vector<Value>& args, std::string& error) {
  Value cur = args[0];
  const Value& path = args[1];
  Value def_val = args.size() > 2 ? args[2] : Value::nil();
  std::vector<std::string> keys;
  if (path.type == Type::List) {
    for (const auto& item : path.list_items()) keys.push_back(item.to_text());
  } else if (path.type == Type::Str) {
    const std::string& p = path.str();
    size_t at = 0;
    while (at < p.size()) {
      size_t dot = p.find('.', at);
      if (dot == std::string::npos) { keys.push_back(p.substr(at)); break; }
      keys.push_back(p.substr(at, dot - at));
      at = dot + 1;
    }
  } else {
    error = "get_in() wants a list of keys or a dot-separated string path";
    return Value::nil();
  }
  for (const auto& k : keys) {
    if (cur.type == Type::Nil) return def_val;
    if (cur.type == Type::Map && cur.obj) {
      Value* v = static_cast<MapObj*>(cur.obj.get())->find(k);
      if (!v) return def_val;
      cur = *v;
    } else if (cur.type == Type::List && cur.obj) {
      char* end = nullptr;
      long idx = std::strtol(k.c_str(), &end, 10);
      if (*end != '\0') return def_val;
      const auto& items = cur.list_items();
      if (idx < 0) idx += (long)items.size();
      if (idx < 0 || idx >= (long)items.size()) return def_val;
      cur = items[(size_t)idx];
    } else {
      return def_val;
    }
  }
  return cur;
}

Value bi_get(Vm&, std::vector<Value>& args, std::string& error) {
  const Value& target = args[0];
  const Value& key = args[1];
  Value def_val = args.size() > 2 ? args[2] : Value::nil();
  if (target.type == Type::Nil) return def_val;
  if (target.type == Type::Map && target.obj) {
    if (key.type != Type::Str) { error = "get() map key must be a string"; return Value::nil(); }
    Value* v = static_cast<MapObj*>(target.obj.get())->find(key.str());
    return v ? *v : def_val;
  }
  if (target.type == Type::List && target.obj) {
    if (!key.is_number()) return def_val;
    int64_t at = key.as_int();
    const auto& items = target.list_items();
    if (at < 0) at += (int64_t)items.size();
    if (at < 0 || at >= (int64_t)items.size()) return def_val;
    return items[(size_t)at];
  }
  return def_val;
}

Value bi_assert_eq(Vm&, std::vector<Value>& args, std::string& error) {
  const Value& a = args[0];
  const Value& b = args[1];
  if (!a.equals(b)) {
    if (args.size() > 2 && args[2].type == Type::Str) {
      error = args[2].str() + " (got " + a.repr() + ", expected " + b.repr() + ")";
    } else {
      error = "assert_eq failed: got " + a.repr() + ", expected " + b.repr();
    }
    return Value::nil();
  }
  return Value::boolean(true);
}

Value bi_assert_ne(Vm&, std::vector<Value>& args, std::string& error) {
  const Value& a = args[0];
  const Value& b = args[1];
  if (a.equals(b)) {
    if (args.size() > 2 && args[2].type == Type::Str) {
      error = args[2].str() + " (values are equal: " + a.repr() + ")";
    } else {
      error = "assert_ne failed: values are equal (" + a.repr() + ")";
    }
    return Value::nil();
  }
  return Value::boolean(true);
}

Value bi_assert_true(Vm&, std::vector<Value>& args, std::string& error) {
  if (!args[0].truthy()) {
    error = args.size() > 1 && args[1].type == Type::Str ? args[1].str() : "assert_true failed: condition is not true";
    return Value::nil();
  }
  return Value::boolean(true);
}

Value bi_assert_false(Vm&, std::vector<Value>& args, std::string& error) {
  if (args[0].truthy()) {
    error = args.size() > 1 && args[1].type == Type::Str ? args[1].str() : "assert_false failed: condition is not false";
    return Value::nil();
  }
  return Value::boolean(true);
}

Value bi_res_json(Vm&, std::vector<Value>& args, std::string&) {
  int64_t status = args.size() > 1 && args[1].type == Type::Int ? args[1].as_int() : 200;
  auto m = std::make_shared<MapObj>();
  m->put("status", Value::integer(status));
  m->put("content_type", Value::string("application/json; charset=utf-8"));
  m->put("json", args[0]);
  if (args.size() > 2 && args[2].type == Type::Map) {
    m->put("headers", args[2]);
  }
  return Value::object(Type::Map, m);
}

Value bi_res_text(Vm&, std::vector<Value>& args, std::string&) {
  int64_t status = args.size() > 1 && args[1].type == Type::Int ? args[1].as_int() : 200;
  auto m = std::make_shared<MapObj>();
  m->put("status", Value::integer(status));
  m->put("content_type", Value::string("text/plain; charset=utf-8"));
  m->put("body", Value::string(args[0].to_text()));
  if (args.size() > 2 && args[2].type == Type::Map) {
    m->put("headers", args[2]);
  }
  return Value::object(Type::Map, m);
}

Value bi_res_html(Vm&, std::vector<Value>& args, std::string&) {
  int64_t status = args.size() > 1 && args[1].type == Type::Int ? args[1].as_int() : 200;
  auto m = std::make_shared<MapObj>();
  m->put("status", Value::integer(status));
  m->put("content_type", Value::string("text/html; charset=utf-8"));
  m->put("body", Value::string(args[0].to_text()));
  if (args.size() > 2 && args[2].type == Type::Map) {
    m->put("headers", args[2]);
  }
  return Value::object(Type::Map, m);
}

// res_download(body, filename, [headers]): a file to save - Content-Disposition: attachment with the name (quotes,
// backslashes and control characters made safe; a name outside ASCII also as filename*=UTF-8''...), the type from
// the name's extension, and no caching. headers, when given, are added last and win.
Value bi_res_download(Vm&, std::vector<Value>& args, std::string& error) {
  if (args[0].type != Type::Str && args[0].type != Type::Bytes) {
    error = std::string("res_download() wants the body as a string or bytes, got a ") + type_name(args[0].type);
    return Value::nil();
  }
  if (!want_str(args[1], error, "res_download")) return Value::nil();
  if (args.size() > 2 && args[2].type != Type::Map && args[2].type != Type::Nil) {
    error = "res_download(): headers is a map";
    return Value::nil();
  }
  const std::string& name = args[1].str();
  std::string plain;
  bool ascii = true;
  for (unsigned char c : name) {
    if (c < 0x20 || c == 0x7f) continue;
    if (c >= 0x80) { ascii = false; plain.push_back('_'); continue; }
    plain.push_back(c == '"' || c == '\\' ? '_' : static_cast<char>(c));
  }
  if (plain.empty()) plain = "download";
  std::string disposition = "attachment; filename=\"" + plain + "\"";
  if (!ascii) {
    static const char* hex = "0123456789ABCDEF";
    std::string enc;
    for (unsigned char c : name) {
      if (std::isalnum(c) || c == '.' || c == '-' || c == '_') enc.push_back(static_cast<char>(c));
      else if (c >= 0x20 && c != 0x7f) { enc.push_back('%'); enc.push_back(hex[c >> 4]); enc.push_back(hex[c & 15]); }
    }
    disposition += "; filename*=UTF-8''" + enc;
  }
  std::string ext;
  if (const size_t dot = name.rfind('.'); dot != std::string::npos) {
    for (char c : name.substr(dot + 1)) ext.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  static const std::pair<const char*, const char*> kTypes[] = {
      {"pdf", "application/pdf"},
      {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
      {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
      {"csv", "text/csv; charset=utf-8"},
      {"json", "application/json; charset=utf-8"},
      {"txt", "text/plain; charset=utf-8"},
      {"html", "text/html; charset=utf-8"},
      {"xml", "application/xml; charset=utf-8"},
      {"zip", "application/zip"},
      {"gz", "application/gzip"},
      {"png", "image/png"},
      {"jpg", "image/jpeg"},
      {"jpeg", "image/jpeg"},
      {"svg", "image/svg+xml"},
  };
  std::string type = "application/octet-stream";
  for (const auto& [e, t] : kTypes)
    if (ext == e) { type = t; break; }
  auto headers = std::make_shared<MapObj>();
  headers->put("Content-Disposition", Value::string(disposition));
  headers->put("Cache-Control", Value::string("no-cache, no-store, must-revalidate"));
  if (args.size() > 2 && args[2].type == Type::Map)
    for (const auto& [k, v] : static_cast<MapObj*>(args[2].obj.get())->entries) headers->put(k, v);
  auto m = std::make_shared<MapObj>();
  m->put("status", Value::integer(200));
  m->put("content_type", Value::string(type));
  m->put("headers", Value::object(Type::Map, headers));
  m->put("body", args[0]);
  return Value::object(Type::Map, m);
}

Value bi_res_error(Vm&, std::vector<Value>& args, std::string&) {
  int64_t status = args[0].is_number() ? args[0].as_int() : 500;
  std::string msg = args.size() > 1 ? args[1].to_text() : "Internal Error";
  auto err_map = std::make_shared<MapObj>();
  err_map->put("error", Value::string(msg));
  err_map->put("status", Value::integer(status));
  auto m = std::make_shared<MapObj>();
  m->put("status", Value::integer(status));
  m->put("content_type", Value::string("application/json; charset=utf-8"));
  m->put("json", Value::object(Type::Map, err_map));
  return Value::object(Type::Map, m);
}

Value bi_res_redirect(Vm&, std::vector<Value>& args, std::string&) {
  std::string loc = args[0].to_text();
  int64_t status = args.size() > 1 && args[1].type == Type::Int ? args[1].as_int() : 302;
  auto hdrs = std::make_shared<MapObj>();
  hdrs->put("Location", Value::string(loc));
  auto m = std::make_shared<MapObj>();
  m->put("status", Value::integer(status));
  m->put("content_type", Value::string("text/plain; charset=utf-8"));
  m->put("headers", Value::object(Type::Map, hdrs));
  m->put("body", Value::string("Redirecting to " + loc));
  return Value::object(Type::Map, m);
}

Value bi_cwd(Vm&, std::vector<Value>&, std::string&) {
  char buf[4096];
  if (getcwd(buf, sizeof(buf))) return Value::string(buf);
  return Value::string(".");
}

// --- error handling --------------------------------------------------------

// What `defer` compiles to (grain_parser.cpp, lambda()): __grain_defer(list, fn) registers a cleanup;
// __grain_run_deferred(body, list) runs the function's body, then its cleanups last first - every one of them, also
// when the body raised or a cleanup did - and gives back the body's value, else the body's error, else the first
// cleanup's error.
Value bi_grain_defer(Vm&, std::vector<Value>& args, std::string& error) {
  if (args[0].type != Type::List) { error = "__grain_defer() is what `defer` compiles to; it is not called directly"; return Value::nil(); }
  args[0].list_items().push_back(args[1]);
  return Value::nil();
}

Value bi_grain_run_deferred(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (args[1].type != Type::List) { error = "__grain_run_deferred() is what `defer` compiles to; it is not called directly"; return Value::nil(); }
  std::string body_error;
  Value result = vm.call(args[0], {}, body_error);
  std::string cleanup_error;
  // Index from the end each time: a cleanup that registers more (it cannot - its own defers are its own) or a list
  // grown by the body is read as it is now.
  for (size_t done = 0; done < args[1].list_items().size(); ++done) {
    const auto& cleanups = args[1].list_items();
    const Value fn = cleanups[cleanups.size() - 1 - done];
    std::string e;
    (void)vm.call(fn, {}, e);
    if (!e.empty() && cleanup_error.empty()) cleanup_error = "in a deferred cleanup: " + e;
  }
  if (!body_error.empty()) { error = body_error; return Value::nil(); }
  if (!cleanup_error.empty()) { error = cleanup_error; return Value::nil(); }
  return result;
}

Value bi_try_call(Vm& vm, std::vector<Value>& args, std::string&) {
  const Value& fn = args[0];
  std::vector<Value> fn_args;
  if (args.size() == 2 && args[1].type == Type::List) {
    fn_args = args[1].list_items();
  } else {
    for (size_t i = 1; i < args.size(); ++i) fn_args.push_back(args[i]);
  }
  std::string call_err;
  Value res = vm.call(fn, fn_args, call_err);
  auto out = std::make_shared<MapObj>();
  if (call_err.empty()) {
    out->put("ok", Value::boolean(true));
    out->put("value", res);
  } else {
    out->put("ok", Value::boolean(false));
    out->put("error", Value::string(call_err));
  }
  return Value::object(Type::Map, out);
}

Value bi_error(Vm&, std::vector<Value>& args, std::string& error) {
  error = args.empty() ? "error" : args[0].to_text();
  return Value::nil();
}

Value bi_is_err(Vm&, std::vector<Value>& args, std::string&) {
  const Value& v = args[0];
  if (v.type == Type::Map) {
    auto* m = static_cast<MapObj*>(v.obj.get());
    Value* ok = m->find("ok");
    if (ok && !ok->truthy()) return Value::boolean(true);
  }
  return Value::boolean(false);
}

Value bi_is_ok(Vm&, std::vector<Value>& args, std::string&) {
  const Value& v = args[0];
  if (v.type == Type::Map) {
    auto* m = static_cast<MapObj*>(v.obj.get());
    Value* ok = m->find("ok");
    if (ok && ok->truthy()) return Value::boolean(true);
  }
  return Value::boolean(false);
}

// --- structured data handling (JSON, Base64, Hex, URL) ---------------------

namespace {

class JsonParser {
 public:
  explicit JsonParser(const std::string& src) : s(src), pos(0) {}

  Value parse(std::string& err) {
    skip_ws();
    if (pos >= s.size()) { err = "json_parse: empty string"; return Value::nil(); }
    Value v = parse_value(err);
    if (!err.empty()) return Value::nil();
    skip_ws();
    if (pos < s.size()) { err = "json_parse: trailing characters after value"; return Value::nil(); }
    return v;
  }

 private:
  const std::string& s;
  size_t pos;

  void skip_ws() {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\r' || s[pos] == '\n')) pos++;
  }

  char peek() { return pos < s.size() ? s[pos] : '\0'; }
  char get() { return pos < s.size() ? s[pos++] : '\0'; }

  Value parse_value(std::string& err) {
    skip_ws();
    char c = peek();
    if (c == 'n') return parse_null(err);
    if (c == 't' || c == 'f') return parse_bool(err);
    if (c == '"') return parse_string(err);
    if (c == '[') return parse_array(err);
    if (c == '{') return parse_object(err);
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(err);
    err = std::string("json_parse: unexpected character '") + c + "'";
    return Value::nil();
  }

  Value parse_null(std::string& err) {
    if (s.compare(pos, 4, "null") == 0) { pos += 4; return Value::nil(); }
    err = "json_parse: expected 'null'";
    return Value::nil();
  }

  Value parse_bool(std::string& err) {
    if (s.compare(pos, 4, "true") == 0) { pos += 4; return Value::boolean(true); }
    if (s.compare(pos, 5, "false") == 0) { pos += 5; return Value::boolean(false); }
    err = "json_parse: expected boolean";
    return Value::nil();
  }

  Value parse_number(std::string& err) {
    size_t start = pos;
    if (peek() == '-') get();
    if (peek() == '0') {
      get();
    } else if (peek() >= '1' && peek() <= '9') {
      while (peek() >= '0' && peek() <= '9') get();
    } else {
      err = "json_parse: invalid number";
      return Value::nil();
    }
    bool is_float = false;
    if (peek() == '.') {
      is_float = true;
      get();
      if (peek() < '0' || peek() > '9') { err = "json_parse: invalid fraction in number"; return Value::nil(); }
      while (peek() >= '0' && peek() <= '9') get();
    }
    if (peek() == 'e' || peek() == 'E') {
      is_float = true;
      get();
      if (peek() == '+' || peek() == '-') get();
      if (peek() < '0' || peek() > '9') { err = "json_parse: invalid exponent in number"; return Value::nil(); }
      while (peek() >= '0' && peek() <= '9') get();
    }
    std::string num_str = s.substr(start, pos - start);
    if (!is_float) {
      // An integer that fits is an integer; one past 64 bits is a real of that size (as a source
      // literal is), never the largest integer standing in for it.
      int64_t n = 0;
      const auto r = std::from_chars(num_str.data(), num_str.data() + num_str.size(), n, 10);
      if (r.ec == std::errc() && r.ptr == num_str.data() + num_str.size()) return Value::integer(n);
    }
    return Value::real(std::strtod(num_str.c_str(), nullptr));
  }

  Value parse_string(std::string& err) {
    get(); // skip '"'
    std::string out;
    while (pos < s.size()) {
      char c = get();
      if (c == '"') return Value::string(out);
      if (c == '\\') {
        if (pos >= s.size()) { err = "json_parse: unterminated escape sequence"; return Value::nil(); }
        char esc = get();
        switch (esc) {
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case '/': out += '/'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'n': out += '\n'; break;
          case 'r': out += '\r'; break;
          case 't': out += '\t'; break;
          case 'u': {
            if (pos + 4 > s.size()) { err = "json_parse: invalid unicode escape"; return Value::nil(); }
            std::string hex_code = s.substr(pos, 4);
            pos += 4;
            char* endp = nullptr;
            unsigned long cp = std::strtoul(hex_code.c_str(), &endp, 16);
            if (cp <= 0x7F) {
              out += (char)cp;
            } else if (cp <= 0x7FF) {
              out += (char)(0xC0 | (cp >> 6));
              out += (char)(0x80 | (cp & 0x3F));
            } else {
              out += (char)(0xE0 | (cp >> 12));
              out += (char)(0x80 | ((cp >> 6) & 0x3F));
              out += (char)(0x80 | (cp & 0x3F));
            }
            break;
          }
          default:
            err = std::string("json_parse: unknown escape sequence '\\") + esc + "'";
            return Value::nil();
        }
      } else {
        out += c;
      }
    }
    err = "json_parse: unterminated string";
    return Value::nil();
  }

  Value parse_array(std::string& err) {
    get(); // skip '['
    skip_ws();
    if (peek() == ']') { get(); return Value::list({}); }
    std::vector<Value> items;
    for (;;) {
      Value elem = parse_value(err);
      if (!err.empty()) return Value::nil();
      items.push_back(std::move(elem));
      skip_ws();
      char c = get();
      if (c == ']') break;
      if (c != ',') { err = "json_parse: expected ',' or ']' in array"; return Value::nil(); }
    }
    return Value::list(std::move(items));
  }

  Value parse_object(std::string& err) {
    get(); // skip '{'
    skip_ws();
    auto map_obj = std::make_shared<MapObj>();
    if (peek() == '}') { get(); return Value::object(Type::Map, map_obj); }
    for (;;) {
      skip_ws();
      if (peek() != '"') { err = "json_parse: expected string key in object"; return Value::nil(); }
      Value key_val = parse_string(err);
      if (!err.empty()) return Value::nil();
      skip_ws();
      if (get() != ':') { err = "json_parse: expected ':' after key in object"; return Value::nil(); }
      Value val = parse_value(err);
      if (!err.empty()) return Value::nil();
      map_obj->put(key_val.str(), std::move(val));
      skip_ws();
      char c = get();
      if (c == '}') break;
      if (c != ',') { err = "json_parse: expected ',' or '}' in object"; return Value::nil(); }
    }
    return Value::object(Type::Map, map_obj);
  }
};

// JSON output options: ascii writes every character past ASCII as \uXXXX (lowercase hex; a surrogate pair past
// U+FFFF; a byte that is no UTF-8 as U+FFFD), escape_slashes writes "/" as "\/" - PHP's json_encode() with no flags.
enum : int { kJsonAscii = 1, kJsonEscapeSlashes = 2 };

void json_write_string(const std::string& s, std::string& out, int flags) {
  out += '"';
  const size_t n = s.size();
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = (unsigned char)s[i];
    switch (c) {
      case '"': out += "\\\""; continue;
      case '\\': out += "\\\\"; continue;
      case '\b': out += "\\b"; continue;
      case '\f': out += "\\f"; continue;
      case '\n': out += "\\n"; continue;
      case '\r': out += "\\r"; continue;
      case '\t': out += "\\t"; continue;
      case '/': out += (flags & kJsonEscapeSlashes) ? "\\/" : "/"; continue;
      default: break;
    }
    char h[16];
    if (c < 0x20) { std::snprintf(h, sizeof h, "\\u%04x", c); out += h; continue; }
    if (c < 0x80 || !(flags & kJsonAscii)) { out += (char)c; continue; }
    // a UTF-8 sequence: its code point, checked
    const int len = c >= 0xF0 && c < 0xF8 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 && c < 0xE0 ? 2 : 0;
    uint32_t cp = len == 4 ? (c & 7u) : len == 3 ? (c & 15u) : (c & 31u);
    bool ok = len > 0 && i + (size_t)len <= n;
    for (int k = 1; ok && k < len; ++k) {
      const unsigned char cc = (unsigned char)s[i + (size_t)k];
      if ((cc & 0xC0) != 0x80) ok = false;
      else cp = (cp << 6) | (cc & 63u);
    }
    if (!ok) { out += "\\ufffd"; continue; }
    i += (size_t)len - 1;
    if (cp >= 0x10000) {
      std::snprintf(h, sizeof h, "\\u%04x\\u%04x", 0xD800 + ((cp - 0x10000) >> 10), 0xDC00 + ((cp - 0x10000) & 1023));
    } else {
      std::snprintf(h, sizeof h, "\\u%04x", cp);
    }
    out += h;
  }
  out += '"';
}

void json_stringify_helper(const Value& v, std::string& out, int indent_level, int indent_spaces, int depth, int flags = 0) {
  if (depth > 128) { out += "null"; return; }
  switch (v.type) {
    case Type::Nil: out += "null"; break;
    case Type::Bool: out += v.b ? "true" : "false"; break;
    case Type::Int: out += std::to_string(v.i); break;
    case Type::Decimal: out += decimal_text(v); break;   // a JSON number with every digit it has
    case Type::Real: {
      char buf[64];
      if (std::isnan(v.d) || std::isinf(v.d)) {
        out += "null";
      } else {
        // The fewest digits that read back as the same value: 0.1 is "0.1", 0.1 + 0.2 is
        // "0.30000000000000004" (what JavaScript and Python write), not 17 digits for every real.
        const auto r = std::to_chars(buf, buf + sizeof(buf), v.d);
        out.append(buf, r.ptr);
      }
      break;
    }
    case Type::Str: json_write_string(v.str(), out, flags); break;
    case Type::List: {
      const auto& items = v.list_items();
      if (items.empty()) { out += "[]"; break; }
      out += "[";
      if (indent_spaces > 0) out += "\n";
      for (size_t i = 0; i < items.size(); ++i) {
        if (indent_spaces > 0) out.append((indent_level + 1) * indent_spaces, ' ');
        json_stringify_helper(items[i], out, indent_level + 1, indent_spaces, depth + 1, flags);
        if (i + 1 < items.size()) out += ",";
        if (indent_spaces > 0) out += "\n";
      }
      if (indent_spaces > 0) out.append(indent_level * indent_spaces, ' ');
      out += "]";
      break;
    }
    case Type::Map: {
      auto* m = static_cast<MapObj*>(v.obj.get());
      if (m->entries.empty()) { out += "{}"; break; }
      out += "{";
      if (indent_spaces > 0) out += "\n";
      for (size_t i = 0; i < m->entries.size(); ++i) {
        if (indent_spaces > 0) out.append((indent_level + 1) * indent_spaces, ' ');
        json_write_string(m->entries[i].first, out, flags);
        out += ":";
        if (indent_spaces > 0) out += " ";
        json_stringify_helper(m->entries[i].second, out, indent_level + 1, indent_spaces, depth + 1, flags);
        if (i + 1 < m->entries.size()) out += ",";
        if (indent_spaces > 0) out += "\n";
      }
      if (indent_spaces > 0) out.append(indent_level * indent_spaces, ' ');
      out += "}";
      break;
    }
    default:
      out += "\"<" + std::string(type_name(v.type)) + ">\"";
      break;
  }
}

static const char kB64Chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode_str(const std::string& in) {
  std::string out;
  int val = 0, valb = -6;
  for (unsigned char c : in) {
    val = (val << 8) + c;
    valb += 8;
    while (valb >= 0) {
      out.push_back(kB64Chars[(val >> valb) & 0x3F]);
      valb -= 6;
    }
  }
  if (valb > -6) out.push_back(kB64Chars[((val << 8) >> (valb + 8)) & 0x3F]);
  while (out.size() % 4) out.push_back('=');
  return out;
}

std::string base64_decode_str(const std::string& in, bool& ok) {
  std::string out;
  std::vector<int> T(256, -1);
  for (int i = 0; i < 64; i++) T[(unsigned char)kB64Chars[i]] = i;

  int val = 0, valb = -8;
  for (unsigned char c : in) {
    if (c == '=') break;
    if (T[c] == -1) {
      if (std::isspace(c)) continue;
      ok = false;
      return "";
    }
    val = (val << 6) + T[c];
    valb += 6;
    if (valb >= 0) {
      out.push_back(char((val >> valb) & 0xFF));
      valb -= 8;
    }
  }
  ok = true;
  return out;
}

std::string hex_encode_str(const std::string& in) {
  static const char hex_digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(in.size() * 2);
  for (unsigned char c : in) {
    out.push_back(hex_digits[c >> 4]);
    out.push_back(hex_digits[c & 0x0F]);
  }
  return out;
}

std::string hex_decode_str(const std::string& in, bool& ok) {
  if (in.size() % 2 != 0) { ok = false; return ""; }
  std::string out;
  out.reserve(in.size() / 2);
  for (size_t i = 0; i < in.size(); i += 2) {
    char h[3] = {in[i], in[i + 1], '\0'};
    char* endp = nullptr;
    unsigned long byte = std::strtoul(h, &endp, 16);
    if (endp != h + 2) { ok = false; return ""; }
    out.push_back((char)byte);
  }
  ok = true;
  return out;
}

std::string url_encode_str(const std::string& in) {
  static const char hex_digits[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : in) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back((char)c);
    } else {
      out.push_back('%');
      out.push_back(hex_digits[c >> 4]);
      out.push_back(hex_digits[c & 0x0F]);
    }
  }
  return out;
}

std::string url_decode_str(const std::string& in, bool& ok) {
  std::string out;
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '+') {
      out.push_back(' ');
    } else if (in[i] == '%') {
      if (i + 2 >= in.size()) { ok = false; return ""; }
      char h[3] = {in[i + 1], in[i + 2], '\0'};
      char* endp = nullptr;
      unsigned long byte = std::strtoul(h, &endp, 16);
      if (endp != h + 2) { ok = false; return ""; }
      out.push_back((char)byte);
      i += 2;
    } else {
      out.push_back(in[i]);
    }
  }
  ok = true;
  return out;
}

}  // namespace


Value bi_json_parse(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "json_parse")) return Value::nil();
  JsonParser parser(args[0].str());
  std::string err;
  Value v = parser.parse(err);
  if (!err.empty()) { error = err; return Value::nil(); }
  return v;
}

// json_stringify(v [, indent | {indent, ascii, escape_slashes}])
Value bi_json_stringify(Vm&, std::vector<Value>& args, std::string& error) {
  int indent_spaces = 0, flags = 0;
  if (args.size() > 1 && args[1].type == Type::Map) {
    auto* o = static_cast<MapObj*>(args[1].obj.get());
    for (const auto& kv : o->entries) {
      if (kv.first == "indent") {
        if (!want_number(kv.second, error, "json_stringify")) return Value::nil();
        indent_spaces = std::max(0, (int)kv.second.as_int());
      } else if (kv.first == "ascii") {
        if (kv.second.truthy()) flags |= kJsonAscii;
      } else if (kv.first == "escape_slashes") {
        if (kv.second.truthy()) flags |= kJsonEscapeSlashes;
      } else {
        error = "json_stringify(): no option '" + kv.first + "' (indent, ascii, escape_slashes)";
        return Value::nil();
      }
    }
  } else if (args.size() > 1) {
    if (!want_number(args[1], error, "json_stringify")) return Value::nil();
    indent_spaces = std::max(0, (int)args[1].as_int());
  }
  std::string out;
  json_stringify_helper(args[0], out, 0, indent_spaces, 0, flags);
  return Value::string(out);
}

enum LogLevel : int {
  kLogDebug = 0,
  kLogInfo = 1,
  kLogWarn = 2,
  kLogError = 3
};

static std::string current_iso8601() {
  auto now = std::chrono::system_clock::now();
  std::time_t tt = std::chrono::system_clock::to_time_t(now);
  std::tm gmt;
#if defined(_WIN32)
  gmtime_s(&gmt, &tt);
#else
  gmtime_r(&tt, &gmt);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &gmt);
  return buf;
}

static const char* log_level_str(int lvl) {
  switch (lvl) {
    case kLogDebug: return "DEBUG";
    case kLogInfo: return "INFO";
    case kLogWarn: return "WARN";
    case kLogError: return "ERROR";
    default: return "INFO";
  }
}

static int parse_log_level(std::string s, bool& ok) {
  for (char& c : s) c = std::tolower(static_cast<unsigned char>(c));
  ok = true;
  if (s == "debug") return kLogDebug;
  if (s == "info") return kLogInfo;
  if (s == "warn" || s == "warning") return kLogWarn;
  if (s == "error") return kLogError;
  ok = false;
  return kLogInfo;
}

static void do_log(Vm& vm, int level, const std::string& msg, const Value* extra) {
  if (level < vm.log_level) return;

  std::string time_str = current_iso8601();
  const char* lvl_str = log_level_str(level);

  if (vm.log_format == 1) { // JSON
    std::string json = "{";
    json += "\"time\":";
    json_write_string(time_str, json, 0);
    json += ",\"level\":";
    json_write_string(lvl_str, json, 0);
    json += ",\"msg\":";
    json_write_string(msg, json, 0);

    if (extra && extra->type == Type::Map) {
      auto* o = static_cast<MapObj*>(extra->obj.get());
      for (const auto& kv : o->entries) {
        json += ",";
        json_write_string(kv.first, json, 0);
        json += ":";
        json_stringify_helper(kv.second, json, 0, 0, 0, 0);
      }
    }
    json += "}";
    vm.emit(json);
  } else { // Text
    std::string text = "[" + time_str + "] [" + lvl_str + "] " + msg;
    if (extra && extra->type == Type::Map) {
      auto* o = static_cast<MapObj*>(extra->obj.get());
      if (!o->entries.empty()) {
        text += " {";
        bool first = true;
        for (const auto& kv : o->entries) {
          if (!first) text += ", ";
          first = false;
          json_write_string(kv.first, text, 0);
          text += ":";
          std::string v_str;
          json_stringify_helper(kv.second, v_str, 0, 0, 0, 0);
          text += v_str;
        }
        text += "}";
      }
    }
    vm.emit(text);
  }
}

Value bi_log_set_level(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "log_set_level")) return Value::nil();
  bool ok = false;
  int lvl = parse_log_level(args[0].str(), ok);
  if (!ok) {
    error = "log_set_level(): unknown log level '" + args[0].str() + "' (expected 'debug', 'info', 'warn', 'error')";
    return Value::nil();
  }
  vm.log_level = lvl;
  return Value::nil();
}

Value bi_log_get_level(Vm& vm, std::vector<Value>&, std::string&) {
  switch (vm.log_level) {
    case kLogDebug: return Value::string("debug");
    case kLogInfo: return Value::string("info");
    case kLogWarn: return Value::string("warn");
    case kLogError: return Value::string("error");
    default: return Value::string("info");
  }
}

Value bi_log_set_format(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "log_set_format")) return Value::nil();
  std::string f = args[0].str();
  for (char& c : f) c = std::tolower(static_cast<unsigned char>(c));
  if (f == "text") {
    vm.log_format = 0;
  } else if (f == "json") {
    vm.log_format = 1;
  } else {
    error = "log_set_format(): unknown log format '" + args[0].str() + "' (expected 'text' or 'json')";
    return Value::nil();
  }
  return Value::nil();
}

Value bi_log_get_format(Vm& vm, std::vector<Value>&, std::string&) {
  return Value::string(vm.log_format == 1 ? "json" : "text");
}

Value bi_log_debug(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "log_debug")) return Value::nil();
  const Value* extra = args.size() > 1 ? &args[1] : nullptr;
  do_log(vm, kLogDebug, args[0].str(), extra);
  return Value::nil();
}

Value bi_log_info(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "log_info")) return Value::nil();
  const Value* extra = args.size() > 1 ? &args[1] : nullptr;
  do_log(vm, kLogInfo, args[0].str(), extra);
  return Value::nil();
}

Value bi_log_warn(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "log_warn")) return Value::nil();
  const Value* extra = args.size() > 1 ? &args[1] : nullptr;
  do_log(vm, kLogWarn, args[0].str(), extra);
  return Value::nil();
}

Value bi_log_error(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "log_error")) return Value::nil();
  const Value* extra = args.size() > 1 ? &args[1] : nullptr;
  do_log(vm, kLogError, args[0].str(), extra);
  return Value::nil();
}

// A field's value must stay on its own line: SSE ends a line at \n, \r or \r\n, so one of those inside an id,
// event name or retry would start a field the program never wrote.
static std::string sse_one_line(std::string s) {
  s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == '\n' || c == '\r'; }), s.end());
  return s;
}

}  // namespace

// The same JSON json_parse / json_stringify read and write, for other builtins (jwt_sign, jwt_verify).
Value grain_json_parse(const std::string& text, std::string& err) {
  JsonParser parser(text);
  return parser.parse(err);
}
std::string grain_json_stringify(const Value& v) {
  std::string out;
  json_stringify_helper(v, out, 0, 0, 0, 0);
  return out;
}

void format_one_sse_event(const Value& ev, std::string& out) {
  if (ev.type == Type::Map) {
    auto m = std::dynamic_pointer_cast<MapObj>(ev.obj);
    if (!m) return;
    if (Value* id = m->find("id")) {
      out += "id: " + sse_one_line(id->to_text()) + "\n";
    }
    if (Value* event = m->find("event")) {
      out += "event: " + sse_one_line(event->to_text()) + "\n";
    }
    if (Value* retry = m->find("retry")) {
      out += "retry: " + sse_one_line(retry->to_text()) + "\n";
    }
    if (Value* data = m->find("data")) {
      if (data->type == Type::Map || data->type == Type::List) {
        std::string j;
        json_stringify_helper(*data, j, 0, 0, 0);
        out += "data: " + j + "\n";
      } else {
        std::string s = data->to_text();
        size_t pos = 0;
        for (;;) {
          const size_t nl = s.find_first_of("\r\n", pos);
          out += "data: " + s.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos) + "\n";
          if (nl == std::string::npos) break;
          pos = nl + ((s[nl] == '\r' && nl + 1 < s.size() && s[nl + 1] == '\n') ? 2 : 1);
          if (pos >= s.size()) break;
        }
      }
    }
    out += "\n";
  } else if (ev.type == Type::Str) {
    // always data, whatever it looks like: text from a user must not be able to add fields or events
    auto one = std::make_shared<MapObj>();
    one->put("data", ev);
    format_one_sse_event(Value::object(Type::Map, one), out);
  } else {
    out += "data: " + ev.to_text() + "\n\n";
  }
}

namespace {

Value bi_res_sse(Vm&, std::vector<Value>& args, std::string&) {
  int64_t status = args.size() > 1 && args[1].type == Type::Int ? args[1].as_int() : 200;
  std::string body;
  if (args[0].type == Type::List) {
    auto l = std::dynamic_pointer_cast<ListObj>(args[0].obj);
    if (l) {
      for (const auto& item : l->items) {
        format_one_sse_event(item, body);
      }
    }
  } else {
    format_one_sse_event(args[0], body);
  }
  auto m = std::make_shared<MapObj>();
  m->put("status", Value::integer(status));
  m->put("content_type", Value::string("text/event-stream; charset=utf-8"));
  m->put("body", Value::string(std::move(body)));
  auto hdrs = std::make_shared<MapObj>();
  hdrs->put("Cache-Control", Value::string("no-cache"));
  hdrs->put("X-Accel-Buffering", Value::string("no"));
  if (args.size() > 2 && args[2].type == Type::Map) {
    auto given = std::dynamic_pointer_cast<MapObj>(args[2].obj);
    if (given) {
      for (const auto& kv : given->entries) hdrs->put(kv.first, kv.second);
    }
  }
  m->put("headers", Value::object(Type::Map, hdrs));
  return Value::object(Type::Map, m);
}

Value bi_base64_encode(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "base64_encode")) return Value::nil();
  return Value::string(base64_encode_str(args[0].str()));
}

Value bi_base64_decode(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "base64_decode")) return Value::nil();
  bool ok = false;
  std::string out = base64_decode_str(args[0].str(), ok);
  if (!ok) { error = "base64_decode: invalid base64 input"; return Value::nil(); }
  return Value::string(out);
}

Value bi_hex_encode(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "hex_encode")) return Value::nil();
  return Value::string(hex_encode_str(args[0].str()));
}

Value bi_hex_decode(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "hex_decode")) return Value::nil();
  bool ok = false;
  std::string out = hex_decode_str(args[0].str(), ok);
  if (!ok) { error = "hex_decode: invalid hex string"; return Value::nil(); }
  return Value::string(out);
}

Value bi_url_encode(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "url_encode")) return Value::nil();
  return Value::string(url_encode_str(args[0].str()));
}

Value bi_url_decode(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "url_decode")) return Value::nil();
  bool ok = false;
  std::string out = url_decode_str(args[0].str(), ok);
  if (!ok) { error = "url_decode: invalid percent-encoded string"; return Value::nil(); }
  return Value::string(out);
}

// ---------------------------------------------------------------------------
// 2D Canvas & Visual Rendering Primitives
// ---------------------------------------------------------------------------

struct GrainGradient {
  enum class Kind { Linear, Radial };
  Kind kind = Kind::Linear;
  double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  double cx = 0, cy = 0, r = 0;
  std::vector<std::pair<double, uint32_t>> stops;
};

struct GrainCanvas {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint32_t> pixels; // 0xAARRGGBB

  uint32_t fill_color = 0xFFFFFFFF;
  uint32_t stroke_color = 0xFF000000;
  std::shared_ptr<GrainGradient> fill_gradient = nullptr;
  std::shared_ptr<GrainGradient> stroke_gradient = nullptr;
  double line_width = 1.0;
  double global_alpha = 1.0;
  std::string font_name = "ui";
  int font_size = 17;

  double tx = 0, ty = 0;
  double sx = 1, sy = 1;
  double rotation = 0;

  struct Point { double x, y; };
  struct Subpath {
    std::vector<Point> pts;
    bool closed = false;
  };
  std::vector<Subpath> paths;
  Point current_pt{0, 0};

  struct State {
    uint32_t fill_color;
    uint32_t stroke_color;
    std::shared_ptr<GrainGradient> fill_gradient;
    std::shared_ptr<GrainGradient> stroke_gradient;
    double line_width;
    double global_alpha;
    std::string font_name;
    int font_size;
    double tx, ty, sx, sy, rotation;
  };
  std::vector<State> state_stack;

  void save() {
    state_stack.push_back({fill_color, stroke_color, fill_gradient, stroke_gradient,
                           line_width, global_alpha, font_name, font_size,
                           tx, ty, sx, sy, rotation});
  }

  void restore() {
    if (state_stack.empty()) return;
    auto s = state_stack.back();
    state_stack.pop_back();
    fill_color = s.fill_color;
    stroke_color = s.stroke_color;
    fill_gradient = s.fill_gradient;
    stroke_gradient = s.stroke_gradient;
    line_width = s.line_width;
    global_alpha = s.global_alpha;
    font_name = s.font_name;
    font_size = s.font_size;
    tx = s.tx; ty = s.ty; sx = s.sx; sy = s.sy; rotation = s.rotation;
  }

  Point transform(double x, double y) const {
    if (rotation != 0.0) {
      double c = std::cos(rotation);
      double s = std::sin(rotation);
      double rx = x * c - y * s;
      double ry = x * s + y * c;
      x = rx;
      y = ry;
    }
    return {x * sx + tx, y * sy + ty};
  }
};

uint32_t parse_color_value(const Value& v, uint32_t fallback = 0xFF000000) {
  if (v.type == Type::Int) {
    uint32_t c = static_cast<uint32_t>(v.i);
    if ((c & 0xFF000000u) == 0 && c != 0) c |= 0xFF000000u;
    return c;
  }
  if (v.type == Type::Real) {
    uint32_t c = static_cast<uint32_t>(v.d);
    if ((c & 0xFF000000u) == 0 && c != 0) c |= 0xFF000000u;
    return c;
  }
  if (v.type == Type::Str) {
    std::string s = v.str();
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    if (s.empty()) return fallback;
    if (s[0] == '#') {
      std::string hex = s.substr(1);
      if (hex.size() == 3) {
        int r = std::stoi(std::string(2, hex[0]), nullptr, 16);
        int g = std::stoi(std::string(2, hex[1]), nullptr, 16);
        int b = std::stoi(std::string(2, hex[2]), nullptr, 16);
        return 0xFF000000u | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
      } else if (hex.size() == 4) {
        int r = std::stoi(std::string(2, hex[0]), nullptr, 16);
        int g = std::stoi(std::string(2, hex[1]), nullptr, 16);
        int b = std::stoi(std::string(2, hex[2]), nullptr, 16);
        int a = std::stoi(std::string(2, hex[3]), nullptr, 16);
        return ((a & 0xFF) << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
      } else if (hex.size() == 6) {
        uint32_t rgb = static_cast<uint32_t>(std::strtoul(hex.c_str(), nullptr, 16));
        return 0xFF000000u | rgb;
      } else if (hex.size() == 8) {
        uint32_t raw = static_cast<uint32_t>(std::strtoul(hex.c_str(), nullptr, 16));
        uint32_t r = (raw >> 24) & 0xFF;
        uint32_t g = (raw >> 16) & 0xFF;
        uint32_t b = (raw >> 8) & 0xFF;
        uint32_t a = raw & 0xFF;
        return (a << 24) | (r << 16) | (g << 8) | b;
      }
    } else if (s.rfind("rgb(", 0) == 0) {
      int r = 0, g = 0, b = 0;
      if (std::sscanf(s.c_str(), "rgb(%d,%d,%d)", &r, &g, &b) >= 3 ||
          std::sscanf(s.c_str(), "rgb(%d , %d , %d)", &r, &g, &b) >= 3) {
        return 0xFF000000u | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
      }
    } else if (s.rfind("rgba(", 0) == 0) {
      int r = 0, g = 0, b = 0;
      float a = 1.0f;
      if (std::sscanf(s.c_str(), "rgba(%d,%d,%d,%f)", &r, &g, &b, &a) >= 4 ||
          std::sscanf(s.c_str(), "rgba(%d , %d , %d , %f)", &r, &g, &b, &a) >= 4) {
        int ia = std::clamp((int)(a * 255.0f), 0, 255);
        return ((ia & 0xFF) << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
      }
    } else {
      std::string lower = s;
      for (char& c : lower) c = (char)std::tolower((unsigned char)c);
      if (lower == "black") return 0xFF000000u;
      if (lower == "white") return 0xFFFFFFFFu;
      if (lower == "red") return 0xFFFF0000u;
      if (lower == "green") return 0xFF00FF00u;
      if (lower == "blue") return 0xFF0000FFu;
      if (lower == "yellow") return 0xFFFFFF00u;
      if (lower == "cyan" || lower == "aqua") return 0xFF00FFFFu;
      if (lower == "magenta" || lower == "fuchsia") return 0xFFFF00FFu;
      if (lower == "gray" || lower == "grey") return 0xFF808080u;
      if (lower == "silver") return 0xFFC0C0C0u;
      if (lower == "maroon") return 0xFF800000u;
      if (lower == "olive") return 0xFF808000u;
      if (lower == "purple") return 0xFF800080u;
      if (lower == "teal") return 0xFF008080u;
      if (lower == "navy") return 0xFF000080u;
      if (lower == "orange") return 0xFFFFA500u;
      if (lower == "transparent") return 0x00000000u;
      if (lower == "darkgray" || lower == "darkgrey") return 0xFFA9A9A9u;
      if (lower == "lightgray" || lower == "lightgrey") return 0xFFD3D3D3u;
    }
  }
  return fallback;
}

struct FontCut { const fonts::Face* face; int base_px; };

inline FontCut lookup_face_cut(const std::string& font_name, int font_size = 17) {
  std::string lower = font_name;
  for (char& c : lower) c = (char)std::tolower((unsigned char)c);

  bool is_mono = lower.find("mono") != std::string::npos || lower.find("code") != std::string::npos ||
                 lower.find("courier") != std::string::npos || lower.find("console") != std::string::npos;
  bool is_bold = lower.find("bold") != std::string::npos || lower.find("heavy") != std::string::npos ||
                 lower.find("semibold") != std::string::npos || lower.find("strong") != std::string::npos;
  bool is_italic = lower.find("italic") != std::string::npos || lower.find("oblique") != std::string::npos;

  if (is_mono) {
    if (font_size >= 19 || lower.find("21") != std::string::npos) return {&fonts::MONO_21, 21};
    return {&fonts::MONO, 16};
  }

  if (is_bold && is_italic) {
    return {&fonts::UI_BOLD_ITALIC, 17};
  }

  if (is_italic) {
    if (font_size >= 19 || lower.find("21") != std::string::npos) return {&fonts::UI_ITALIC_21, 21};
    return {&fonts::UI_ITALIC, 17};
  }

  if (is_bold) {
    if (font_size >= 26 || lower.find("30") != std::string::npos) return {&fonts::UI_BOLD_30, 30};
    if (font_size >= 19 || lower.find("21") != std::string::npos) return {&fonts::UI_BOLD_21, 21};
    if (font_size <= 13 || lower.find("small") != std::string::npos) return {&fonts::UI_SMALL, 12};
    return {&fonts::UI_BOLD, 17};
  }

  // Regular UI Sans:
  // Note: UI_SMALL is a bold cut (12px bold), so regular text uses UI (17px regular)
  // scaled down with smooth bilinear filtering, giving crisp, clean regular weight.
  if (font_size >= 26 || lower.find("30") != std::string::npos) return {&fonts::UI_30, 30};
  if (font_size >= 19 || lower.find("21") != std::string::npos) return {&fonts::UI_21, 21};
  return {&fonts::UI, 17};
}

inline int cov_at(const uint8_t* cov, const fonts::Glyph& g, float sx, float sy) {
  int x0 = (int)(sx < 0 ? sx - 1 : sx);
  int y0 = (int)(sy < 0 ? sy - 1 : sy);
  float fx = sx - (float)x0;
  float fy = sy - (float)y0;
  auto at = [&](int x, int y) -> int {
    if (x < 0 || y < 0 || x >= (int)g.w || y >= (int)g.h) return 0;
    return cov[g.off + (size_t)y * g.w + (size_t)x];
  };
  float c00 = (float)at(x0, y0), c10 = (float)at(x0 + 1, y0);
  float c01 = (float)at(x0, y0 + 1), c11 = (float)at(x0 + 1, y0 + 1);
  float top = c00 + (c10 - c00) * fx;
  float bot = c01 + (c11 - c01) * fx;
  float v = top + (bot - top) * fy;
  int out = (int)std::round(v);
  return out < 0 ? 0 : out > 255 ? 255 : out;
}

std::shared_ptr<GrainCanvas> get_canvas(const Value& v, std::string& error, const char* func) {
  if (v.type == Type::Extern) {
    auto ext = static_cast<ExternObj*>(v.obj.get());
    if (ext->kind_name == "canvas" && ext->ptr) {
      return std::static_pointer_cast<GrainCanvas>(ext->ptr);
    }
  }
  error = std::string(func) + "() expects a canvas handle as first argument";
  return nullptr;
}

std::shared_ptr<GrainGradient> get_gradient(const Value& v, std::string& error, const char* func) {
  if (v.type == Type::Extern) {
    auto ext = static_cast<ExternObj*>(v.obj.get());
    if (ext->kind_name == "gradient" && ext->ptr) {
      return std::static_pointer_cast<GrainGradient>(ext->ptr);
    }
  }
  error = std::string(func) + "() expects a gradient handle";
  return nullptr;
}

namespace {
struct GrainGammaTables {
  uint16_t srgb_to_lin[256];
  uint8_t lin_to_srgb[65536];
  uint8_t font_cov_smooth[256];

  GrainGammaTables() {
    for (int i = 0; i < 256; ++i) {
      double v = i / 255.0;
      double lin = std::pow(v, 2.2);
      srgb_to_lin[i] = static_cast<uint16_t>(std::round(lin * 65535.0));

      if (i == 0) {
        font_cov_smooth[0] = 0;
      } else {
        static const double kCovGamma = [] {
          if (const char* e = std::getenv("AIOS_FONT_GAMMA")) {
            const double g = std::atof(e);
            if (g > 0.2 && g < 4.0) return g;
          }
          return 1.25; // 1.25 stem sharpness matching compositor transfer curve
        }();
        double smoothed = std::pow(v, kCovGamma);
        int sc = static_cast<int>(std::round(smoothed * 255.0));
        font_cov_smooth[i] = static_cast<uint8_t>(sc > 255 ? 255 : sc);
      }
    }
    int at = 0;
    for (int j = 1; j <= 255; ++j) {
      double edge = (j - 0.5) / 255.0;
      double start_d = std::ceil(std::pow(edge, 2.2) * 65535.0);
      int start = start_d < 0.0 ? 0 : (start_d > 65536.0 ? 65536 : static_cast<int>(start_d));
      for (; at < start && at <= 65535; ++at) lin_to_srgb[at] = static_cast<uint8_t>(j - 1);
      if (start > at) at = start;
    }
    for (; at <= 65535; ++at) lin_to_srgb[at] = 255;
  }
};

static const GrainGammaTables g_grain_gamma;

static inline void blend_px_lin(uint32_t& dst, uint32_t color, uint32_t lin_sr, uint32_t lin_sg,
                                uint32_t lin_sb, uint32_t alpha) {
  if (alpha >= 255) { dst = color; return; }
  if (alpha <= 0) return;

  uint32_t d = dst;
  uint32_t dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;

  uint32_t a = static_cast<uint32_t>(alpha);
  uint32_t inv_a = 255 - a;

  uint32_t lin_dr = g_grain_gamma.srgb_to_lin[dr];
  uint32_t lin_dg = g_grain_gamma.srgb_to_lin[dg];
  uint32_t lin_db = g_grain_gamma.srgb_to_lin[db];

  uint32_t lin_r = (lin_sr * a + lin_dr * inv_a) / 255;
  uint32_t lin_g = (lin_sg * a + lin_dg * inv_a) / 255;
  uint32_t lin_b = (lin_sb * a + lin_db * inv_a) / 255;

  if (lin_r > 65535) lin_r = 65535;
  if (lin_g > 65535) lin_g = 65535;
  if (lin_b > 65535) lin_b = 65535;

  uint32_t r = g_grain_gamma.lin_to_srgb[lin_r];
  uint32_t g = g_grain_gamma.lin_to_srgb[lin_g];
  uint32_t b = g_grain_gamma.lin_to_srgb[lin_b];

  dst = 0xFF000000u | (r << 16) | (g << 8) | b;
}
} // namespace

inline void blend_pixel(GrainCanvas& cv, int x, int y, uint32_t color, double alpha_factor = 1.0) {
  if (x < 0 || x >= (int)cv.width || y < 0 || y >= (int)cv.height) return;
  double a = ((color >> 24) & 0xFF) / 255.0 * cv.global_alpha * alpha_factor;
  if (a <= 0.001) return;
  size_t idx = (size_t)y * cv.width + (size_t)x;
  if (a >= 0.999) {
    cv.pixels[idx] = color;
    return;
  }
  uint32_t a_byte = static_cast<uint32_t>(std::clamp(a * 255.0, 0.0, 255.0));
  uint32_t lsr = g_grain_gamma.srgb_to_lin[(color >> 16) & 0xFF];
  uint32_t lsg = g_grain_gamma.srgb_to_lin[(color >> 8) & 0xFF];
  uint32_t lsb = g_grain_gamma.srgb_to_lin[color & 0xFF];
  blend_px_lin(cv.pixels[idx], color, lsr, lsg, lsb, a_byte);
}

uint32_t sample_gradient(const GrainGradient& grad, double x, double y) {
  if (grad.stops.empty()) return 0xFF000000;
  if (grad.stops.size() == 1) return grad.stops[0].second;
  double t = 0.0;
  if (grad.kind == GrainGradient::Kind::Linear) {
    double dx = grad.x1 - grad.x0;
    double dy = grad.y1 - grad.y0;
    double len_sq = dx * dx + dy * dy;
    if (len_sq > 1e-6) {
      t = ((x - grad.x0) * dx + (y - grad.y0) * dy) / len_sq;
    }
  } else {
    double dist = std::hypot(x - grad.cx, y - grad.cy);
    if (grad.r > 1e-6) {
      t = dist / grad.r;
    }
  }
  t = std::clamp(t, 0.0, 1.0);

  if (t <= grad.stops.front().first) return grad.stops.front().second;
  if (t >= grad.stops.back().first) return grad.stops.back().second;
  for (size_t i = 0; i + 1 < grad.stops.size(); ++i) {
    if (t >= grad.stops[i].first && t <= grad.stops[i + 1].first) {
      double span = grad.stops[i + 1].first - grad.stops[i].first;
      double factor = span > 1e-6 ? (t - grad.stops[i].first) / span : 0.0;
      uint32_t c0 = grad.stops[i].second;
      uint32_t c1 = grad.stops[i + 1].second;
      int a0 = (c0 >> 24) & 0xFF, r0 = (c0 >> 16) & 0xFF, g0 = (c0 >> 8) & 0xFF, b0 = c0 & 0xFF;
      int a1 = (c1 >> 24) & 0xFF, r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF;
      int a = (int)(a0 + (a1 - a0) * factor);
      int r = (int)(r0 + (r1 - r0) * factor);
      int g = (int)(g0 + (g1 - g0) * factor);
      int b = (int)(b0 + (b1 - b0) * factor);
      return (a << 24) | (r << 16) | (g << 8) | b;
    }
  }
  return grad.stops.back().second;
}

void draw_line_aa(GrainCanvas& cv, double x0, double y0, double x1, double y1, uint32_t color, double width) {
  double hw = width * 0.5;
  int min_x = std::max(0, (int)std::floor(std::min(x0, x1) - hw - 1.0));
  int max_x = std::min((int)cv.width - 1, (int)std::ceil(std::max(x0, x1) + hw + 1.0));
  int min_y = std::max(0, (int)std::floor(std::min(y0, y1) - hw - 1.0));
  int max_y = std::min((int)cv.height - 1, (int)std::ceil(std::max(y0, y1) + hw + 1.0));

  double dx = x1 - x0;
  double dy = y1 - y0;
  double len_sq = dx * dx + dy * dy;

  for (int y = min_y; y <= max_y; ++y) {
    for (int x = min_x; x <= max_x; ++x) {
      double px = x + 0.5;
      double py = y + 0.5;
      double dist = 0.0;
      if (len_sq < 1e-6) {
        dist = std::hypot(px - x0, py - y0);
      } else {
        double t = std::clamp(((px - x0) * dx + (py - y0) * dy) / len_sq, 0.0, 1.0);
        double proj_x = x0 + t * dx;
        double proj_y = y0 + t * dy;
        dist = std::hypot(px - proj_x, py - proj_y);
      }
      double cov = std::clamp(hw + 0.5 - dist, 0.0, 1.0);
      if (cov > 0.001) {
        uint32_t col = cv.stroke_gradient ? sample_gradient(*cv.stroke_gradient, px, py) : color;
        blend_pixel(cv, x, y, col, cov);
      }
    }
  }
}

void fill_rect_aa(GrainCanvas& cv, double rx, double ry, double rw, double rh, uint32_t color) {
  int x0 = std::max(0, (int)std::floor(rx));
  int x1 = std::min((int)cv.width, (int)std::ceil(rx + rw));
  int y0 = std::max(0, (int)std::floor(ry));
  int y1 = std::min((int)cv.height, (int)std::ceil(ry + rh));

  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      double px = x + 0.5;
      double py = y + 0.5;
      double cov = 1.0;
      if (px < rx) cov *= (1.0 - (rx - px));
      if (px > rx + rw) cov *= (1.0 - (px - (rx + rw)));
      if (py < ry) cov *= (1.0 - (ry - py));
      if (py > ry + rh) cov *= (1.0 - (py - (ry + rh)));
      cov = std::clamp(cov, 0.0, 1.0);
      if (cov > 0.001) {
        uint32_t col = cv.fill_gradient ? sample_gradient(*cv.fill_gradient, px, py) : color;
        blend_pixel(cv, x, y, col, cov);
      }
    }
  }
}

void stroke_rect_aa(GrainCanvas& cv, double rx, double ry, double rw, double rh, uint32_t color, double width) {
  draw_line_aa(cv, rx, ry, rx + rw, ry, color, width);
  draw_line_aa(cv, rx + rw, ry, rx + rw, ry + rh, color, width);
  draw_line_aa(cv, rx + rw, ry + rh, rx, ry + rh, color, width);
  draw_line_aa(cv, rx, ry + rh, rx, ry, color, width);
}

void fill_circle_aa(GrainCanvas& cv, double cx, double cy, double r, uint32_t color) {
  int min_x = std::max(0, (int)std::floor(cx - r - 1.0));
  int max_x = std::min((int)cv.width - 1, (int)std::ceil(cx + r + 1.0));
  int min_y = std::max(0, (int)std::floor(cy - r - 1.0));
  int max_y = std::min((int)cv.height - 1, (int)std::ceil(cy + r + 1.0));

  for (int y = min_y; y <= max_y; ++y) {
    for (int x = min_x; x <= max_x; ++x) {
      double dist = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
      double cov = std::clamp(r + 0.5 - dist, 0.0, 1.0);
      if (cov > 0.001) {
        uint32_t col = cv.fill_gradient ? sample_gradient(*cv.fill_gradient, x + 0.5, y + 0.5) : color;
        blend_pixel(cv, x, y, col, cov);
      }
    }
  }
}

void stroke_circle_aa(GrainCanvas& cv, double cx, double cy, double r, uint32_t color, double width) {
  double hw = width * 0.5;
  int min_x = std::max(0, (int)std::floor(cx - r - hw - 1.0));
  int max_x = std::min((int)cv.width - 1, (int)std::ceil(cx + r + hw + 1.0));
  int min_y = std::max(0, (int)std::floor(cy - r - hw - 1.0));
  int max_y = std::min((int)cv.height - 1, (int)std::ceil(cy + r + hw + 1.0));

  for (int y = min_y; y <= max_y; ++y) {
    for (int x = min_x; x <= max_x; ++x) {
      double dist = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
      double cov = std::clamp(hw + 0.5 - std::abs(dist - r), 0.0, 1.0);
      if (cov > 0.001) {
        uint32_t col = cv.stroke_gradient ? sample_gradient(*cv.stroke_gradient, x + 0.5, y + 0.5) : color;
        blend_pixel(cv, x, y, col, cov);
      }
    }
  }
}

void fill_round_rect_aa(GrainCanvas& cv, double rx, double ry, double rw, double rh, double radius, uint32_t color) {
  radius = std::clamp(radius, 0.0, std::min(rw, rh) * 0.5);
  int min_x = std::max(0, (int)std::floor(rx - 1.0));
  int max_x = std::min((int)cv.width - 1, (int)std::ceil(rx + rw + 1.0));
  int min_y = std::max(0, (int)std::floor(ry - 1.0));
  int max_y = std::min((int)cv.height - 1, (int)std::ceil(ry + rh + 1.0));

  double cx0 = rx + radius;
  double cx1 = rx + rw - radius;
  double cy0 = ry + radius;
  double cy1 = ry + rh - radius;

  for (int y = min_y; y <= max_y; ++y) {
    for (int x = min_x; x <= max_x; ++x) {
      double px = x + 0.5;
      double py = y + 0.5;
      double cov = 0.0;
      if (px >= rx && px <= rx + rw && py >= ry && py <= ry + rh) {
        if (radius <= 0.0) {
          cov = 1.0;
        } else if (px >= cx0 && px <= cx1) {
          cov = 1.0;
        } else if (py >= cy0 && py <= cy1) {
          cov = 1.0;
        } else {
          double qx = px < cx0 ? cx0 : cx1;
          double qy = py < cy0 ? cy0 : cy1;
          double dist = std::hypot(px - qx, py - qy);
          cov = std::clamp(radius + 0.5 - dist, 0.0, 1.0);
        }
      }
      if (cov > 0.001) {
        uint32_t col = cv.fill_gradient ? sample_gradient(*cv.fill_gradient, px, py) : color;
        blend_pixel(cv, x, y, col, cov);
      }
    }
  }
}

void stroke_round_rect_aa(GrainCanvas& cv, double rx, double ry, double rw, double rh, double radius, uint32_t color, double width) {
  radius = std::clamp(radius, 0.0, std::min(rw, rh) * 0.5);
  double hw = width * 0.5;
  int min_x = std::max(0, (int)std::floor(rx - hw - 1.0));
  int max_x = std::min((int)cv.width - 1, (int)std::ceil(rx + rw + hw + 1.0));
  int min_y = std::max(0, (int)std::floor(ry - hw - 1.0));
  int max_y = std::min((int)cv.height - 1, (int)std::ceil(ry + rh + hw + 1.0));

  double cx0 = rx + radius;
  double cx1 = rx + rw - radius;
  double cy0 = ry + radius;
  double cy1 = ry + rh - radius;

  for (int y = min_y; y <= max_y; ++y) {
    for (int x = min_x; x <= max_x; ++x) {
      double px = x + 0.5;
      double py = y + 0.5;
      double dist = 1e9;
      if (px >= cx0 && px <= cx1) {
        dist = std::min(std::abs(py - ry), std::abs(py - (ry + rh)));
      } else if (py >= cy0 && py <= cy1) {
        dist = std::min(std::abs(px - rx), std::abs(px - (rx + rw)));
      } else {
        double qx = px < cx0 ? cx0 : cx1;
        double qy = py < cy0 ? cy0 : cy1;
        dist = std::abs(std::hypot(px - qx, py - qy) - radius);
      }
      double cov = std::clamp(hw + 0.5 - dist, 0.0, 1.0);
      if (cov > 0.001) {
        uint32_t col = cv.stroke_gradient ? sample_gradient(*cv.stroke_gradient, px, py) : color;
        blend_pixel(cv, x, y, col, cov);
      }
    }
  }
}

void fill_ellipse_aa(GrainCanvas& cv, double cx, double cy, double rx, double ry, uint32_t color) {
  if (rx <= 0 || ry <= 0) return;
  int min_x = std::max(0, (int)std::floor(cx - rx - 1.0));
  int max_x = std::min((int)cv.width - 1, (int)std::ceil(cx + rx + 1.0));
  int min_y = std::max(0, (int)std::floor(cy - ry - 1.0));
  int max_y = std::min((int)cv.height - 1, (int)std::ceil(cy + ry + 1.0));

  for (int y = min_y; y <= max_y; ++y) {
    for (int x = min_x; x <= max_x; ++x) {
      double dx = (x + 0.5 - cx) / rx;
      double dy = (y + 0.5 - cy) / ry;
      double d = std::hypot(dx, dy);
      double dist = (d - 1.0) * std::min(rx, ry);
      double cov = std::clamp(0.5 - dist, 0.0, 1.0);
      if (cov > 0.001) {
        uint32_t col = cv.fill_gradient ? sample_gradient(*cv.fill_gradient, x + 0.5, y + 0.5) : color;
        blend_pixel(cv, x, y, col, cov);
      }
    }
  }
}

void stroke_ellipse_aa(GrainCanvas& cv, double cx, double cy, double rx, double ry, uint32_t color, double width) {
  if (rx <= 0 || ry <= 0) return;
  double hw = width * 0.5;
  int min_x = std::max(0, (int)std::floor(cx - rx - hw - 1.0));
  int max_x = std::min((int)cv.width - 1, (int)std::ceil(cx + rx + hw + 1.0));
  int min_y = std::max(0, (int)std::floor(cy - ry - hw - 1.0));
  int max_y = std::min((int)cv.height - 1, (int)std::ceil(cy + ry + hw + 1.0));

  for (int y = min_y; y <= max_y; ++y) {
    for (int x = min_x; x <= max_x; ++x) {
      double dx = (x + 0.5 - cx) / rx;
      double dy = (y + 0.5 - cy) / ry;
      double d = std::hypot(dx, dy);
      double dist = std::abs((d - 1.0) * std::min(rx, ry));
      double cov = std::clamp(hw + 0.5 - dist, 0.0, 1.0);
      if (cov > 0.001) {
        uint32_t col = cv.stroke_gradient ? sample_gradient(*cv.stroke_gradient, x + 0.5, y + 0.5) : color;
        blend_pixel(cv, x, y, col, cov);
      }
    }
  }
}

bool point_in_poly(double px, double py, const std::vector<GrainCanvas::Point>& pts) {
  bool inside = false;
  size_t n = pts.size();
  for (size_t i = 0, j = n - 1; i < n; j = i++) {
    double xi = pts[i].x, yi = pts[i].y;
    double xj = pts[j].x, yj = pts[j].y;
    if (((yi > py) != (yj > py)) && (px < (xj - xi) * (py - yi) / (yj - yi + 1e-12) + xi)) {
      inside = !inside;
    }
  }
  return inside;
}

void fill_polygon_aa(GrainCanvas& cv, const std::vector<GrainCanvas::Point>& pts, uint32_t color) {
  if (pts.size() < 3) return;
  double min_x = pts[0].x, max_x = pts[0].x;
  double min_y = pts[0].y, max_y = pts[0].y;
  for (const auto& p : pts) {
    min_x = std::min(min_x, p.x);
    max_x = std::max(max_x, p.x);
    min_y = std::min(min_y, p.y);
    max_y = std::max(max_y, p.y);
  }
  int x0 = std::max(0, (int)std::floor(min_x));
  int x1 = std::min((int)cv.width - 1, (int)std::ceil(max_x));
  int y0 = std::max(0, (int)std::floor(min_y));
  int y1 = std::min((int)cv.height - 1, (int)std::ceil(max_y));

  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      if (point_in_poly(x + 0.5, y + 0.5, pts)) {
        uint32_t col = cv.fill_gradient ? sample_gradient(*cv.fill_gradient, x + 0.5, y + 0.5) : color;
        blend_pixel(cv, x, y, col, 1.0);
      }
    }
  }
}

void draw_text_aa(GrainCanvas& cv, const std::string& text, double start_x, double start_y,
                  uint32_t color, const std::string& font_name, int font_size) {
  auto cut = lookup_face_cut(font_name, font_size);
  const fonts::Face& face = *cut.face;
  double scale = (font_size > 0 && cut.base_px > 0) ? ((double)font_size / (double)cut.base_px) : 1.0;
  bool exact = (scale > 0.97 && scale < 1.03);

  double pen = start_x;
  double line_top = start_y;
  double line_step = face.line_h * scale + 1.0;

  const uint32_t lin_sr = g_grain_gamma.srgb_to_lin[(color >> 16) & 0xFF];
  const uint32_t lin_sg = g_grain_gamma.srgb_to_lin[(color >> 8) & 0xFF];
  const uint32_t lin_sb = g_grain_gamma.srgb_to_lin[color & 0xFF];
  const double text_alpha = ((color >> 24) & 0xFF) / 255.0 * cv.global_alpha;

  for (size_t i = 0; i < text.size(); ++i) {
    unsigned char c = (unsigned char)text[i];
    if (c == '\n') {
      pen = start_x;
      line_top += line_step;
      continue;
    }
    const fonts::Glyph* gp = nullptr;
    const uint8_t* cov_base = face.cov();
    if ((c & 0x80) != 0) {
      uint32_t cp = 0;
      int extra = 0;
      if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
      else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
      else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
      else continue;
      for (int e = 0; e < extra && i + 1 < text.size(); ++e)
        cp = (cp << 6) | ((unsigned char)text[++i] & 0x3F);
      gp = face.find(cp);
      if (!gp) {
        if (const glyphs::Rasterized* r = glyphs::rasterize(face, cp)) {
          gp = &r->g;
          cov_base = r->cov;
        }
      }
    } else {
      gp = &face.at((char)c);
    }
    if (!gp) continue;
    const fonts::Glyph& g = *gp;
    if (g.w && g.h && cov_base) {
      if (exact) {
        int glyph_x = (int)std::round(pen) + g.bx;
        int glyph_y = (int)std::round(line_top) + g.by;
        for (int gy = 0; gy < (int)g.h; ++gy) {
          int py = glyph_y + gy;
          if (py < 0 || py >= (int)cv.height) continue;
          const uint8_t* cov_row = cov_base + g.off + (size_t)gy * g.w;
          for (int gx = 0; gx < (int)g.w; ++gx) {
            int px = glyph_x + gx;
            if (px < 0 || px >= (int)cv.width) continue;
            uint8_t raw_cov = cov_row[gx];
            if (!raw_cov) continue;
            uint8_t cov = g_grain_gamma.font_cov_smooth[raw_cov];
            if (cov > 0) {
              uint32_t a_byte = static_cast<uint32_t>(std::clamp(cov * text_alpha, 0.0, 255.0));
              if (a_byte > 0) {
                size_t idx = (size_t)py * cv.width + (size_t)px;
                if (cv.fill_gradient) {
                  uint32_t rgb = sample_gradient(*cv.fill_gradient, px + 0.5, py + 0.5);
                  uint32_t lsr = g_grain_gamma.srgb_to_lin[(rgb >> 16) & 0xFF];
                  uint32_t lsg = g_grain_gamma.srgb_to_lin[(rgb >> 8) & 0xFF];
                  uint32_t lsb = g_grain_gamma.srgb_to_lin[rgb & 0xFF];
                  blend_px_lin(cv.pixels[idx], rgb, lsr, lsg, lsb, a_byte);
                } else {
                  blend_px_lin(cv.pixels[idx], color, lin_sr, lin_sg, lin_sb, a_byte);
                }
              }
            }
          }
        }
      } else {
        int tw = (int)((float)g.w * scale + 0.999f);
        int th = (int)((float)g.h * scale + 0.999f);
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
        int gx = (int)std::round(pen + (double)g.bx * scale);
        int gy = (int)std::round(line_top + (double)g.by * scale);
        for (int row = 0; row < th; ++row) {
          int py = gy + row;
          if (py < 0 || py >= (int)cv.height) continue;
          float sy = ((float)row + 0.5f) / (float)scale - 0.5f;
          for (int col = 0; col < tw; ++col) {
            int px = gx + col;
            if (px < 0 || px >= (int)cv.width) continue;
            float sx = ((float)col + 0.5f) / (float)scale - 0.5f;
            int raw_cov = cov_at(cov_base, g, sx, sy);
            if (!raw_cov) continue;
            uint8_t cov = g_grain_gamma.font_cov_smooth[raw_cov];
            if (cov > 0) {
              uint32_t a_byte = static_cast<uint32_t>(std::clamp(cov * text_alpha, 0.0, 255.0));
              if (a_byte > 0) {
                size_t idx = (size_t)py * cv.width + (size_t)px;
                if (cv.fill_gradient) {
                  uint32_t rgb = sample_gradient(*cv.fill_gradient, px + 0.5, py + 0.5);
                  uint32_t lsr = g_grain_gamma.srgb_to_lin[(rgb >> 16) & 0xFF];
                  uint32_t lsg = g_grain_gamma.srgb_to_lin[(rgb >> 8) & 0xFF];
                  uint32_t lsb = g_grain_gamma.srgb_to_lin[rgb & 0xFF];
                  blend_px_lin(cv.pixels[idx], rgb, lsr, lsg, lsb, a_byte);
                } else {
                  blend_px_lin(cv.pixels[idx], color, lin_sr, lin_sg, lin_sb, a_byte);
                }
              }
            }
          }
        }
      }
    }
    pen += (double)g.adv * scale;
    if (pen >= (double)cv.width) break;
  }
}

std::shared_ptr<MapObj> measure_text_metrics(const std::string& text, const std::string& font_name, int font_size) {
  auto cut = lookup_face_cut(font_name, font_size);
  const fonts::Face& face = *cut.face;
  double scale = (font_size > 0 && cut.base_px > 0) ? ((double)font_size / (double)cut.base_px) : 1.0;

  double max_w = 0.0;
  double cur_w = 0.0;
  int lines = 1;
  for (size_t i = 0; i < text.size(); ++i) {
    unsigned char c = (unsigned char)text[i];
    if (c == '\n') {
      max_w = std::max(max_w, cur_w);
      cur_w = 0.0;
      lines++;
      continue;
    }
    if ((c & 0x80) != 0) {
      uint32_t cp = 0;
      int extra = 0;
      if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
      else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
      else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
      else continue;
      for (int e = 0; e < extra && i + 1 < text.size(); ++e)
        cp = (cp << 6) | ((unsigned char)text[++i] & 0x3F);
      const fonts::Glyph* gp = face.find(cp);
      if (!gp) {
        if (const glyphs::Rasterized* r = glyphs::rasterize(face, cp)) {
          gp = &r->g;
        }
      }
      if (gp) cur_w += (double)gp->adv * scale;
    } else {
      cur_w += (double)face.at((char)c).adv * scale;
    }
  }
  max_w = std::max(max_w, cur_w);
  int total_h = (int)std::ceil(lines * face.line_h * scale);
  int asc = (int)std::ceil(face.ascent * scale);
  int lh = (int)std::ceil(face.line_h * scale);

  auto res = std::make_shared<MapObj>();
  res->put("width", Value::integer((int64_t)std::ceil(max_w)));
  res->put("height", Value::integer(total_h));
  res->put("ascent", Value::integer(asc));
  res->put("line_height", Value::integer(lh));
  return res;
}

std::string canvas_to_ppm_data(const GrainCanvas& cv) {
  std::ostringstream ss;
  ss << "P6\n" << cv.width << " " << cv.height << "\n255\n";
  std::string head = ss.str();
  std::string data;
  data.reserve(head.size() + (size_t)cv.width * cv.height * 3);
  data.append(head);
  for (uint32_t px : cv.pixels) {
    uint8_t r = (px >> 16) & 0xFF;
    uint8_t g = (px >> 8) & 0xFF;
    uint8_t b = px & 0xFF;
    data.push_back((char)r);
    data.push_back((char)g);
    data.push_back((char)b);
  }
  return data;
}

bool save_canvas_ppm(const GrainCanvas& cv, const std::string& path) {
  // stdio, not ofstream: the same everywhere, including WebAssembly builds (whose libc++ has no fstream)
  FILE* out = std::fopen(path.c_str(), "wb");
  if (!out) return false;
  std::string data = canvas_to_ppm_data(cv);
  const bool ok = std::fwrite(data.data(), 1, data.size(), out) == data.size();
  return std::fclose(out) == 0 && ok;
}

void blit_canvas(GrainCanvas& dst, const GrainCanvas& src, int dx, int dy,
                 int sx, int sy, int sw, int sh, double alpha = 1.0) {
  if (sw <= 0) sw = src.width - sx;
  if (sh <= 0) sh = src.height - sy;
  for (int y = 0; y < sh; ++y) {
    int src_y = sy + y;
    int dst_y = dy + y;
    if (src_y < 0 || src_y >= (int)src.height || dst_y < 0 || dst_y >= (int)dst.height) continue;
    for (int x = 0; x < sw; ++x) {
      int src_x = sx + x;
      int dst_x = dx + x;
      if (src_x < 0 || src_x >= (int)src.width || dst_x < 0 || dst_x >= (int)dst.width) continue;
      uint32_t col = src.pixels[(size_t)src_y * src.width + src_x];
      blend_pixel(dst, dst_x, dst_y, col, alpha);
    }
  }
}

// Builtin functions

Value bi_canvas_create(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "canvas_create") || !want_number(args[1], error, "canvas_create"))
    return Value::nil();
  int64_t w = args[0].as_int();
  int64_t h = args[1].as_int();
  if (w <= 0 || h <= 0 || w > 16384 || h > 16384) {
    error = "canvas_create(): dimensions out of valid range (1..16384)";
    return Value::nil();
  }
  uint32_t bg = 0x00000000;
  if (args.size() > 2) bg = parse_color_value(args[2], 0x00000000);

  auto cv = std::make_shared<GrainCanvas>();
  cv->width = (uint32_t)w;
  cv->height = (uint32_t)h;
  cv->pixels.resize((size_t)w * h, bg);
  return Value::object(Type::Extern, std::make_shared<ExternObj>("canvas", cv));
}

Value bi_canvas_width(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_width");
  if (!cv) return Value::nil();
  return Value::integer(cv->width);
}

Value bi_canvas_height(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_height");
  if (!cv) return Value::nil();
  return Value::integer(cv->height);
}

Value bi_canvas_clear(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_clear");
  if (!cv) return Value::nil();
  uint32_t c = args.size() > 1 ? parse_color_value(args[1], 0x00000000) : 0x00000000;
  std::fill(cv->pixels.begin(), cv->pixels.end(), c);
  return args[0];
}

Value bi_canvas_get_pixel(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_get_pixel");
  if (!cv) return Value::nil();
  if (!want_number(args[1], error, "canvas_get_pixel") || !want_number(args[2], error, "canvas_get_pixel"))
    return Value::nil();
  int x = (int)args[1].as_int();
  int y = (int)args[2].as_int();
  if (x < 0 || x >= (int)cv->width || y < 0 || y >= (int)cv->height) return Value::integer(0);
  return Value::integer((int64_t)cv->pixels[(size_t)y * cv->width + x]);
}

Value bi_canvas_set_pixel(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_set_pixel");
  if (!cv) return Value::nil();
  if (!want_number(args[1], error, "canvas_set_pixel") || !want_number(args[2], error, "canvas_set_pixel"))
    return Value::nil();
  int x = (int)args[1].as_int();
  int y = (int)args[2].as_int();
  uint32_t color = parse_color_value(args[3]);
  if (x >= 0 && x < (int)cv->width && y >= 0 && y < (int)cv->height) {
    cv->pixels[(size_t)y * cv->width + x] = color;
  }
  return args[0];
}

Value bi_canvas_get_pixels(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_get_pixels");
  if (!cv) return Value::nil();
  std::vector<Value> out;
  out.reserve(cv->pixels.size());
  for (uint32_t p : cv->pixels) out.push_back(Value::integer((int64_t)p));
  return Value::list(std::move(out));
}

Value bi_canvas_set_pixels(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_set_pixels");
  if (!cv) return Value::nil();
  if (!want_list(args[1], error, "canvas_set_pixels")) return Value::nil();
  const auto& items = args[1].list_items();
  size_t n = std::min(items.size(), cv->pixels.size());
  for (size_t i = 0; i < n; ++i) {
    cv->pixels[i] = parse_color_value(items[i]);
  }
  return args[0];
}

// image_decode(data) -> a canvas holding the picture (PNG, JPEG, GIF, BMP); data is bytes or a string of them.
Value bi_image_decode(Vm&, std::vector<Value>& args, std::string& error) {
  ByteSpan in;
  if (args[0].type == Type::Bytes) in = ByteSpan(args[0].bytes_data().data(), args[0].bytes_data().size());
  else if (args[0].type == Type::Str) in = ByteSpan(reinterpret_cast<const uint8_t*>(args[0].str().data()), args[0].str().size());
  else { error = std::string("image_decode() wants bytes or a string, found a ") + type_name(args[0].type); return Value::nil(); }
  auto bm = web::decode_image(in);
  if (!bm.ok()) { error = "image_decode(): " + bm.error().message; return Value::nil(); }
  if (bm->empty() || bm->w > 16384 || bm->h > 16384) { error = "image_decode(): no picture of a size a canvas can hold"; return Value::nil(); }
  auto cv = std::make_shared<GrainCanvas>();
  cv->width = (uint32_t)bm->w;
  cv->height = (uint32_t)bm->h;
  cv->pixels = std::move(bm->pixels);
  return Value::object(Type::Extern, std::make_shared<ExternObj>("canvas", cv));
}

Value bi_canvas_set_fill_style(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_set_fill_style");
  if (!cv) return Value::nil();
  if (args[1].type == Type::Extern) {
    auto ext = static_cast<ExternObj*>(args[1].obj.get());
    if (ext->kind_name == "gradient" && ext->ptr) {
      cv->fill_gradient = std::static_pointer_cast<GrainGradient>(ext->ptr);
      return args[0];
    }
  }
  cv->fill_gradient = nullptr;
  cv->fill_color = parse_color_value(args[1], cv->fill_color);
  return args[0];
}

Value bi_canvas_set_stroke_style(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_set_stroke_style");
  if (!cv) return Value::nil();
  if (args[1].type == Type::Extern) {
    auto ext = static_cast<ExternObj*>(args[1].obj.get());
    if (ext->kind_name == "gradient" && ext->ptr) {
      cv->stroke_gradient = std::static_pointer_cast<GrainGradient>(ext->ptr);
      return args[0];
    }
  }
  cv->stroke_gradient = nullptr;
  cv->stroke_color = parse_color_value(args[1], cv->stroke_color);
  return args[0];
}

Value bi_canvas_set_line_width(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_set_line_width");
  if (!cv) return Value::nil();
  if (!want_number(args[1], error, "canvas_set_line_width")) return Value::nil();
  cv->line_width = std::max(0.1, args[1].as_double());
  return args[0];
}

Value bi_canvas_set_global_alpha(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_set_global_alpha");
  if (!cv) return Value::nil();
  if (!want_number(args[1], error, "canvas_set_global_alpha")) return Value::nil();
  cv->global_alpha = std::clamp(args[1].as_double(), 0.0, 1.0);
  return args[0];
}

Value bi_color_rgb(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "color_rgb") || !want_number(args[1], error, "color_rgb") || !want_number(args[2], error, "color_rgb"))
    return Value::nil();
  int r = std::clamp((int)args[0].as_int(), 0, 255);
  int g = std::clamp((int)args[1].as_int(), 0, 255);
  int b = std::clamp((int)args[2].as_int(), 0, 255);
  return Value::integer((int64_t)(0xFF000000u | (r << 16) | (g << 8) | b));
}

Value bi_color_rgba(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "color_rgba") || !want_number(args[1], error, "color_rgba") || !want_number(args[2], error, "color_rgba"))
    return Value::nil();
  int r = std::clamp((int)args[0].as_int(), 0, 255);
  int g = std::clamp((int)args[1].as_int(), 0, 255);
  int b = std::clamp((int)args[2].as_int(), 0, 255);
  int a = 255;
  if (args.size() > 3) {
    if (args[3].type == Type::Real) a = std::clamp((int)(args[3].d * 255.0), 0, 255);
    else a = std::clamp((int)args[3].as_int(), 0, 255);
  }
  return Value::integer((int64_t)(((uint32_t)a << 24) | (r << 16) | (g << 8) | b));
}

Value bi_color_hex(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "color_hex")) return Value::nil();
  return Value::integer((int64_t)parse_color_value(args[0]));
}

Value bi_color_to_hex(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "color_to_hex")) return Value::nil();
  /* CSS form, which is what color_hex()/color_parse() read back: #rrggbb
   * when opaque, #rrggbbaa otherwise. This printed the internal 0xAARRGGBB
   * word as "#AARRGGBB", which the parser takes as RRGGBBAA - so
   * color_hex(color_to_hex(c)) came back a different color. */
  const uint32_t c = (uint32_t)args[0].as_int();
  const unsigned a = (c >> 24) & 0xFF, rgb = c & 0xFFFFFF;
  char buf[32];
  if (a == 0xFF) std::snprintf(buf, sizeof(buf), "#%06x", rgb);
  else std::snprintf(buf, sizeof(buf), "#%06x%02x", rgb, a);
  return Value::string(buf);
}

Value bi_color_parse(Vm&, std::vector<Value>& args, std::string&) {
  return Value::integer((int64_t)parse_color_value(args[0]));
}

Value bi_canvas_fill_rect(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_fill_rect");
  if (!cv) return Value::nil();
  if (!want_number(args[1], error, "canvas_fill_rect") || !want_number(args[2], error, "canvas_fill_rect") ||
      !want_number(args[3], error, "canvas_fill_rect") || !want_number(args[4], error, "canvas_fill_rect"))
    return Value::nil();
  double x = args[1].as_double(), y = args[2].as_double();
  double w = args[3].as_double(), h = args[4].as_double();
  uint32_t color = args.size() > 5 ? parse_color_value(args[5], cv->fill_color) : cv->fill_color;
  auto p0 = cv->transform(x, y);
  fill_rect_aa(*cv, p0.x, p0.y, w * cv->sx, h * cv->sy, color);
  return args[0];
}

Value bi_canvas_stroke_rect(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_stroke_rect");
  if (!cv) return Value::nil();
  if (!want_number(args[1], error, "canvas_stroke_rect") || !want_number(args[2], error, "canvas_stroke_rect") ||
      !want_number(args[3], error, "canvas_stroke_rect") || !want_number(args[4], error, "canvas_stroke_rect"))
    return Value::nil();
  double x = args[1].as_double(), y = args[2].as_double();
  double w = args[3].as_double(), h = args[4].as_double();
  uint32_t color = args.size() > 5 ? parse_color_value(args[5], cv->stroke_color) : cv->stroke_color;
  double width = args.size() > 6 ? args[6].as_double() : cv->line_width;
  auto p0 = cv->transform(x, y);
  stroke_rect_aa(*cv, p0.x, p0.y, w * cv->sx, h * cv->sy, color, width);
  return args[0];
}

Value bi_canvas_clear_rect(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_clear_rect");
  if (!cv) return Value::nil();
  if (!want_number(args[1], error, "canvas_clear_rect") || !want_number(args[2], error, "canvas_clear_rect") ||
      !want_number(args[3], error, "canvas_clear_rect") || !want_number(args[4], error, "canvas_clear_rect"))
    return Value::nil();
  int x = std::max(0, (int)args[1].as_int());
  int y = std::max(0, (int)args[2].as_int());
  int w = (int)args[3].as_int();
  int h = (int)args[4].as_int();
  int x1 = std::min((int)cv->width, x + w);
  int y1 = std::min((int)cv->height, y + h);
  for (int r = y; r < y1; ++r) {
    for (int c = x; c < x1; ++c) {
      cv->pixels[(size_t)r * cv->width + c] = 0x00000000;
    }
  }
  return args[0];
}

Value bi_canvas_fill_round_rect(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_fill_round_rect");
  if (!cv) return Value::nil();
  if (!want_number(args[1], error, "canvas_fill_round_rect") || !want_number(args[2], error, "canvas_fill_round_rect") ||
      !want_number(args[3], error, "canvas_fill_round_rect") || !want_number(args[4], error, "canvas_fill_round_rect") ||
      !want_number(args[5], error, "canvas_fill_round_rect"))
    return Value::nil();
  double x = args[1].as_double(), y = args[2].as_double(), w = args[3].as_double(), h = args[4].as_double();
  double r = args[5].as_double();
  uint32_t color = args.size() > 6 ? parse_color_value(args[6], cv->fill_color) : cv->fill_color;
  auto p0 = cv->transform(x, y);
  fill_round_rect_aa(*cv, p0.x, p0.y, w * cv->sx, h * cv->sy, r * cv->sx, color);
  return args[0];
}

Value bi_canvas_stroke_round_rect(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_stroke_round_rect");
  if (!cv) return Value::nil();
  if (!want_number(args[1], error, "canvas_stroke_round_rect") || !want_number(args[2], error, "canvas_stroke_round_rect") ||
      !want_number(args[3], error, "canvas_stroke_round_rect") || !want_number(args[4], error, "canvas_stroke_round_rect") ||
      !want_number(args[5], error, "canvas_stroke_round_rect"))
    return Value::nil();
  double x = args[1].as_double(), y = args[2].as_double(), w = args[3].as_double(), h = args[4].as_double();
  double r = args[5].as_double();
  uint32_t color = args.size() > 6 ? parse_color_value(args[6], cv->stroke_color) : cv->stroke_color;
  double width = args.size() > 7 ? args[7].as_double() : cv->line_width;
  auto p0 = cv->transform(x, y);
  stroke_round_rect_aa(*cv, p0.x, p0.y, w * cv->sx, h * cv->sy, r * cv->sx, color, width);
  return args[0];
}

Value bi_canvas_fill_circle(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_fill_circle");
  if (!cv) return Value::nil();
  if (!want_number(args[1], error, "canvas_fill_circle") || !want_number(args[2], error, "canvas_fill_circle") ||
      !want_number(args[3], error, "canvas_fill_circle"))
    return Value::nil();
  double cx = args[1].as_double(), cy = args[2].as_double(), r = args[3].as_double();
  uint32_t color = args.size() > 4 ? parse_color_value(args[4], cv->fill_color) : cv->fill_color;
  auto p0 = cv->transform(cx, cy);
  fill_circle_aa(*cv, p0.x, p0.y, r * cv->sx, color);
  return args[0];
}

Value bi_canvas_stroke_circle(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_stroke_circle");
  if (!cv) return Value::nil();
  if (!want_number(args[1], error, "canvas_stroke_circle") || !want_number(args[2], error, "canvas_stroke_circle") ||
      !want_number(args[3], error, "canvas_stroke_circle"))
    return Value::nil();
  double cx = args[1].as_double(), cy = args[2].as_double(), r = args[3].as_double();
  uint32_t color = args.size() > 4 ? parse_color_value(args[4], cv->stroke_color) : cv->stroke_color;
  double width = args.size() > 5 ? args[5].as_double() : cv->line_width;
  auto p0 = cv->transform(cx, cy);
  stroke_circle_aa(*cv, p0.x, p0.y, r * cv->sx, color, width);
  return args[0];
}

Value bi_canvas_fill_ellipse(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_fill_ellipse");
  if (!cv) return Value::nil();
  if (!want_number(args[1], error, "canvas_fill_ellipse") || !want_number(args[2], error, "canvas_fill_ellipse") ||
      !want_number(args[3], error, "canvas_fill_ellipse") || !want_number(args[4], error, "canvas_fill_ellipse"))
    return Value::nil();
  double cx = args[1].as_double(), cy = args[2].as_double(), rx = args[3].as_double(), ry = args[4].as_double();
  uint32_t color = args.size() > 5 ? parse_color_value(args[5], cv->fill_color) : cv->fill_color;
  auto p0 = cv->transform(cx, cy);
  fill_ellipse_aa(*cv, p0.x, p0.y, rx * cv->sx, ry * cv->sy, color);
  return args[0];
}

Value bi_canvas_stroke_ellipse(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_stroke_ellipse");
  if (!cv) return Value::nil();
  if (!want_number(args[1], error, "canvas_stroke_ellipse") || !want_number(args[2], error, "canvas_stroke_ellipse") ||
      !want_number(args[3], error, "canvas_stroke_ellipse") || !want_number(args[4], error, "canvas_stroke_ellipse"))
    return Value::nil();
  double cx = args[1].as_double(), cy = args[2].as_double(), rx = args[3].as_double(), ry = args[4].as_double();
  uint32_t color = args.size() > 5 ? parse_color_value(args[5], cv->stroke_color) : cv->stroke_color;
  double width = args.size() > 6 ? args[6].as_double() : cv->line_width;
  auto p0 = cv->transform(cx, cy);
  stroke_ellipse_aa(*cv, p0.x, p0.y, rx * cv->sx, ry * cv->sy, color, width);
  return args[0];
}

Value bi_canvas_draw_line(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_draw_line");
  if (!cv) return Value::nil();
  if (!want_number(args[1], error, "canvas_draw_line") || !want_number(args[2], error, "canvas_draw_line") ||
      !want_number(args[3], error, "canvas_draw_line") || !want_number(args[4], error, "canvas_draw_line"))
    return Value::nil();
  double x0 = args[1].as_double(), y0 = args[2].as_double(), x1 = args[3].as_double(), y1 = args[4].as_double();
  uint32_t color = args.size() > 5 ? parse_color_value(args[5], cv->stroke_color) : cv->stroke_color;
  double width = args.size() > 6 ? args[6].as_double() : cv->line_width;
  auto p0 = cv->transform(x0, y0);
  auto p1 = cv->transform(x1, y1);
  draw_line_aa(*cv, p0.x, p0.y, p1.x, p1.y, color, width);
  return args[0];
}

Value bi_canvas_fill_polygon(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_fill_polygon");
  if (!cv) return Value::nil();
  if (!want_list(args[1], error, "canvas_fill_polygon")) return Value::nil();
  const auto& items = args[1].list_items();
  std::vector<GrainCanvas::Point> pts;
  for (const auto& it : items) {
    if (it.type == Type::List && it.list_items().size() >= 2) {
      pts.push_back(cv->transform(it.list_items()[0].as_double(), it.list_items()[1].as_double()));
    }
  }
  uint32_t color = args.size() > 2 ? parse_color_value(args[2], cv->fill_color) : cv->fill_color;
  fill_polygon_aa(*cv, pts, color);
  return args[0];
}

Value bi_canvas_stroke_polygon(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_stroke_polygon");
  if (!cv) return Value::nil();
  if (!want_list(args[1], error, "canvas_stroke_polygon")) return Value::nil();
  const auto& items = args[1].list_items();
  std::vector<GrainCanvas::Point> pts;
  for (const auto& it : items) {
    if (it.type == Type::List && it.list_items().size() >= 2) {
      pts.push_back(cv->transform(it.list_items()[0].as_double(), it.list_items()[1].as_double()));
    }
  }
  uint32_t color = args.size() > 2 ? parse_color_value(args[2], cv->stroke_color) : cv->stroke_color;
  double width = args.size() > 3 ? args[3].as_double() : cv->line_width;
  if (pts.size() >= 2) {
    for (size_t i = 0; i < pts.size(); ++i) {
      size_t next = (i + 1) % pts.size();
      draw_line_aa(*cv, pts[i].x, pts[i].y, pts[next].x, pts[next].y, color, width);
    }
  }
  return args[0];
}

Value bi_canvas_create_linear_gradient(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "canvas_create_linear_gradient") || !want_number(args[1], error, "canvas_create_linear_gradient") ||
      !want_number(args[2], error, "canvas_create_linear_gradient") || !want_number(args[3], error, "canvas_create_linear_gradient"))
    return Value::nil();
  auto grad = std::make_shared<GrainGradient>();
  grad->kind = GrainGradient::Kind::Linear;
  grad->x0 = args[0].as_double();
  grad->y0 = args[1].as_double();
  grad->x1 = args[2].as_double();
  grad->y1 = args[3].as_double();
  return Value::object(Type::Extern, std::make_shared<ExternObj>("gradient", grad));
}

Value bi_canvas_create_radial_gradient(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "canvas_create_radial_gradient") || !want_number(args[1], error, "canvas_create_radial_gradient") ||
      !want_number(args[2], error, "canvas_create_radial_gradient"))
    return Value::nil();
  auto grad = std::make_shared<GrainGradient>();
  grad->kind = GrainGradient::Kind::Radial;
  grad->cx = args[0].as_double();
  grad->cy = args[1].as_double();
  grad->r = args[2].as_double();
  return Value::object(Type::Extern, std::make_shared<ExternObj>("gradient", grad));
}

Value bi_canvas_gradient_add_color_stop(Vm&, std::vector<Value>& args, std::string& error) {
  auto grad = get_gradient(args[0], error, "canvas_gradient_add_color_stop");
  if (!grad) return Value::nil();
  if (!want_number(args[1], error, "canvas_gradient_add_color_stop")) return Value::nil();
  double offset = std::clamp(args[1].as_double(), 0.0, 1.0);
  uint32_t color = parse_color_value(args[2]);
  grad->stops.push_back({offset, color});
  std::stable_sort(grad->stops.begin(), grad->stops.end(), [](const auto& a, const auto& b) {
    return a.first < b.first;
  });
  return args[0];
}

Value bi_canvas_fill_linear_gradient(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_fill_linear_gradient");
  if (!cv) return Value::nil();
  if (!want_number(args[1], error, "canvas_fill_linear_gradient") || !want_number(args[2], error, "canvas_fill_linear_gradient") ||
      !want_number(args[3], error, "canvas_fill_linear_gradient") || !want_number(args[4], error, "canvas_fill_linear_gradient"))
    return Value::nil();
  double x = args[1].as_double(), y = args[2].as_double(), w = args[3].as_double(), h = args[4].as_double();
  uint32_t c1 = parse_color_value(args[5]);
  uint32_t c2 = parse_color_value(args[6]);
  bool vertical = args.size() > 7 ? args[7].truthy() : true;

  auto grad = std::make_shared<GrainGradient>();
  grad->kind = GrainGradient::Kind::Linear;
  grad->x0 = x; grad->y0 = y;
  grad->x1 = vertical ? x : (x + w);
  grad->y1 = vertical ? (y + h) : y;
  grad->stops.push_back({0.0, c1});
  grad->stops.push_back({1.0, c2});

  auto old_grad = cv->fill_gradient;
  cv->fill_gradient = grad;
  auto p0 = cv->transform(x, y);
  fill_rect_aa(*cv, p0.x, p0.y, w * cv->sx, h * cv->sy, c1);
  cv->fill_gradient = old_grad;
  return args[0];
}

Value bi_canvas_fill_gradient_v(Vm& vm, std::vector<Value>& args, std::string& error) {
  std::vector<Value> fargs = args;
  fargs.push_back(Value::boolean(true));
  return bi_canvas_fill_linear_gradient(vm, fargs, error);
}

Value bi_canvas_fill_gradient_h(Vm& vm, std::vector<Value>& args, std::string& error) {
  std::vector<Value> fargs = args;
  fargs.push_back(Value::boolean(false));
  return bi_canvas_fill_linear_gradient(vm, fargs, error);
}

Value bi_canvas_begin_path(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_begin_path");
  if (!cv) return Value::nil();
  cv->paths.clear();
  cv->paths.push_back(GrainCanvas::Subpath{});
  return args[0];
}

Value bi_canvas_move_to(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_move_to");
  if (!cv) return Value::nil();
  if (!want_number(args[1], error, "canvas_move_to") || !want_number(args[2], error, "canvas_move_to"))
    return Value::nil();
  double x = args[1].as_double(), y = args[2].as_double();
  cv->current_pt = {x, y};
  GrainCanvas::Subpath sp;
  sp.pts.push_back(cv->transform(x, y));
  cv->paths.push_back(std::move(sp));
  return args[0];
}

Value bi_canvas_line_to(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_line_to");
  if (!cv) return Value::nil();
  if (!want_number(args[1], error, "canvas_line_to") || !want_number(args[2], error, "canvas_line_to"))
    return Value::nil();
  double x = args[1].as_double(), y = args[2].as_double();
  if (cv->paths.empty()) cv->paths.push_back(GrainCanvas::Subpath{});
  cv->paths.back().pts.push_back(cv->transform(x, y));
  cv->current_pt = {x, y};
  return args[0];
}

Value bi_canvas_close_path(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_close_path");
  if (!cv) return Value::nil();
  if (!cv->paths.empty() && !cv->paths.back().pts.empty()) {
    cv->paths.back().closed = true;
    cv->paths.back().pts.push_back(cv->paths.back().pts.front());
  }
  return args[0];
}

Value bi_canvas_arc(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_arc");
  if (!cv) return Value::nil();
  if (!want_number(args[1], error, "canvas_arc") || !want_number(args[2], error, "canvas_arc") ||
      !want_number(args[3], error, "canvas_arc") || !want_number(args[4], error, "canvas_arc") ||
      !want_number(args[5], error, "canvas_arc"))
    return Value::nil();
  double cx = args[1].as_double(), cy = args[2].as_double(), r = args[3].as_double();
  double sa = args[4].as_double(), ea = args[5].as_double();
  bool anticlockwise = args.size() > 6 ? args[6].truthy() : false;

  if (anticlockwise && ea > sa) ea -= 2.0 * 3.14159265358979323846;
  else if (!anticlockwise && ea < sa) ea += 2.0 * 3.14159265358979323846;

  int steps = std::max(12, (int)(r * 1.5));
  if (cv->paths.empty()) cv->paths.push_back(GrainCanvas::Subpath{});
  for (int s = 0; s <= steps; ++s) {
    double t = (double)s / steps;
    double ang = sa + (ea - sa) * t;
    double px = cx + r * std::cos(ang);
    double py = cy + r * std::sin(ang);
    cv->paths.back().pts.push_back(cv->transform(px, py));
    cv->current_pt = {px, py};
  }
  return args[0];
}

Value bi_canvas_bezier_curve_to(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_bezier_curve_to");
  if (!cv) return Value::nil();
  if (args.size() < 7) { error = "canvas_bezier_curve_to wants 6 coordinates (cp1x, cp1y, cp2x, cp2y, x, y)"; return Value::nil(); }
  double cp1x = args[1].as_double(), cp1y = args[2].as_double();
  double cp2x = args[3].as_double(), cp2y = args[4].as_double();
  double x = args[5].as_double(), y = args[6].as_double();
  double p0x = cv->current_pt.x, p0y = cv->current_pt.y;

  if (cv->paths.empty()) cv->paths.push_back(GrainCanvas::Subpath{});
  int steps = 24;
  for (int i = 1; i <= steps; ++i) {
    double t = (double)i / steps;
    double u = 1.0 - t;
    double px = u * u * u * p0x + 3 * u * u * t * cp1x + 3 * u * t * t * cp2x + t * t * t * x;
    double py = u * u * u * p0y + 3 * u * u * t * cp1y + 3 * u * t * t * cp2y + t * t * t * y;
    cv->paths.back().pts.push_back(cv->transform(px, py));
  }
  cv->current_pt = {x, y};
  return args[0];
}

Value bi_canvas_quadratic_curve_to(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_quadratic_curve_to");
  if (!cv) return Value::nil();
  if (args.size() < 5) { error = "canvas_quadratic_curve_to wants 4 coordinates (cpx, cpy, x, y)"; return Value::nil(); }
  double cpx = args[1].as_double(), cpy = args[2].as_double();
  double x = args[3].as_double(), y = args[4].as_double();
  double p0x = cv->current_pt.x, p0y = cv->current_pt.y;

  if (cv->paths.empty()) cv->paths.push_back(GrainCanvas::Subpath{});
  int steps = 20;
  for (int i = 1; i <= steps; ++i) {
    double t = (double)i / steps;
    double u = 1.0 - t;
    double px = u * u * p0x + 2 * u * t * cpx + t * t * x;
    double py = u * u * p0y + 2 * u * t * cpy + t * t * y;
    cv->paths.back().pts.push_back(cv->transform(px, py));
  }
  cv->current_pt = {x, y};
  return args[0];
}

Value bi_canvas_fill(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_fill");
  if (!cv) return Value::nil();
  uint32_t color = args.size() > 1 ? parse_color_value(args[1], cv->fill_color) : cv->fill_color;
  for (const auto& sp : cv->paths) {
    if (sp.pts.size() >= 3) {
      fill_polygon_aa(*cv, sp.pts, color);
    }
  }
  return args[0];
}

Value bi_canvas_stroke(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_stroke");
  if (!cv) return Value::nil();
  uint32_t color = args.size() > 1 ? parse_color_value(args[1], cv->stroke_color) : cv->stroke_color;
  double width = args.size() > 2 ? args[2].as_double() : cv->line_width;
  for (const auto& sp : cv->paths) {
    if (sp.pts.size() >= 2) {
      for (size_t i = 0; i + 1 < sp.pts.size(); ++i) {
        draw_line_aa(*cv, sp.pts[i].x, sp.pts[i].y, sp.pts[i + 1].x, sp.pts[i + 1].y, color, width);
      }
    }
  }
  return args[0];
}

Value bi_canvas_save(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_save");
  if (!cv) return Value::nil();
  cv->save();
  return args[0];
}

Value bi_canvas_restore(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_restore");
  if (!cv) return Value::nil();
  cv->restore();
  return args[0];
}

Value bi_canvas_translate(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_translate");
  if (!cv) return Value::nil();
  if (!want_number(args[1], error, "canvas_translate") || !want_number(args[2], error, "canvas_translate"))
    return Value::nil();
  cv->tx += args[1].as_double();
  cv->ty += args[2].as_double();
  return args[0];
}

Value bi_canvas_scale(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_scale");
  if (!cv) return Value::nil();
  if (!want_number(args[1], error, "canvas_scale")) return Value::nil();
  double sx = args[1].as_double();
  double sy = args.size() > 2 ? args[2].as_double() : sx;
  cv->sx *= sx;
  cv->sy *= sy;
  return args[0];
}

Value bi_canvas_rotate(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_rotate");
  if (!cv) return Value::nil();
  if (!want_number(args[1], error, "canvas_rotate")) return Value::nil();
  cv->rotation += args[1].as_double();
  return args[0];
}

Value bi_canvas_set_font(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_set_font");
  if (!cv) return Value::nil();
  if (args[1].type == Type::Str) cv->font_name = args[1].str();
  if (args.size() > 2 && args[2].is_number()) cv->font_size = (int)args[2].as_int();
  return args[0];
}

Value bi_canvas_fill_text(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_fill_text");
  if (!cv) return Value::nil();
  std::string text = args[1].to_text();
  if (!want_number(args[2], error, "canvas_fill_text") || !want_number(args[3], error, "canvas_fill_text"))
    return Value::nil();
  double x = args[2].as_double(), y = args[3].as_double();
  uint32_t color = args.size() > 4 ? parse_color_value(args[4], cv->fill_color) : cv->fill_color;
  std::string font = args.size() > 5 ? args[5].str() : cv->font_name;
  int size = args.size() > 6 ? (int)args[6].as_int() : cv->font_size;
  auto p0 = cv->transform(x, y);
  draw_text_aa(*cv, text, p0.x, p0.y, color, font, size);
  return args[0];
}

Value bi_canvas_draw_text(Vm& vm, std::vector<Value>& args, std::string& error) {
  return bi_canvas_fill_text(vm, args, error);
}

Value bi_canvas_stroke_text(Vm& vm, std::vector<Value>& args, std::string& error) {
  return bi_canvas_fill_text(vm, args, error);
}

Value bi_canvas_measure_text(Vm&, std::vector<Value>& args, std::string& error) {
  std::string text;
  std::string font = "ui";
  int size = 17;
  if (args[0].type == Type::Extern) {
    auto cv = get_canvas(args[0], error, "canvas_measure_text");
    if (!cv) return Value::nil();
    text = args[1].to_text();
    font = args.size() > 2 ? args[2].str() : cv->font_name;
    size = args.size() > 3 ? (int)args[3].as_int() : cv->font_size;
  } else {
    text = args[0].to_text();
    if (args.size() > 1 && args[1].type == Type::Str) font = args[1].str();
    if (args.size() > 2 && args[2].is_number()) size = (int)args[2].as_int();
  }
  auto res = measure_text_metrics(text, font, size);
  return Value::object(Type::Map, res);
}

Value bi_canvas_to_ppm(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_to_ppm");
  if (!cv) return Value::nil();
  return Value::string(canvas_to_ppm_data(*cv));
}

Value bi_canvas_save_ppm(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_save_ppm");
  if (!cv) return Value::nil();
  if (!want_str(args[1], error, "canvas_save_ppm")) return Value::nil();
  bool ok = save_canvas_ppm(*cv, args[1].str());
  return Value::boolean(ok);
}

// canvas_to_png(cv): the canvas as a PNG file's bytes, transparency kept.
Value bi_canvas_to_png(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_to_png");
  if (!cv) return Value::nil();
  ByteBuffer png = web::encode_png(cv->pixels.data(), (int)cv->width, (int)cv->height);
  return Value::string(std::string(reinterpret_cast<const char*>(png.data()), png.size()));
}

Files* files_of(Vm& vm, std::string& error, const char* who);
std::string file_error(const char* who, const std::string& path, const std::string& why);

// canvas_save_png(cv, path): written through the program's own files (Vm::files), like write_file.
Value bi_canvas_save_png(Vm& vm, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_save_png");
  if (!cv) return Value::nil();
  if (!want_str(args[1], error, "canvas_save_png")) return Value::nil();
  Files* f = files_of(vm, error, "canvas_save_png");
  if (!f) return Value::nil();
  ByteBuffer png = web::encode_png(cv->pixels.data(), (int)cv->width, (int)cv->height);
  std::string why;
  if (!f->write(args[1].str(), std::string(reinterpret_cast<const char*>(png.data()), png.size()), false, why)) {
    error = file_error("canvas_save_png", args[1].str(), why);
    return Value::nil();
  }
  return Value::boolean(true);
}

Value bi_canvas_to_bytes(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_to_bytes");
  if (!cv) return Value::nil();
  std::string fmt = args.size() > 1 ? args[1].str() : "ppm";
  if (fmt == "png") {
    ByteBuffer png = web::encode_png(cv->pixels.data(), (int)cv->width, (int)cv->height);
    return Value::string(std::string(reinterpret_cast<const char*>(png.data()), png.size()));
  }
  if (fmt == "raw" || fmt == "argb") {
    std::string raw((const char*)cv->pixels.data(), cv->pixels.size() * sizeof(uint32_t));
    return Value::string(std::move(raw));
  }
  return Value::string(canvas_to_ppm_data(*cv));
}

Value bi_canvas_blit(Vm&, std::vector<Value>& args, std::string& error) {
  auto dst = get_canvas(args[0], error, "canvas_blit");
  if (!dst) return Value::nil();
  auto src = get_canvas(args[1], error, "canvas_blit");
  if (!src) return Value::nil();
  int dx = (int)args[2].as_int(), dy = (int)args[3].as_int();
  int sx = args.size() > 4 ? (int)args[4].as_int() : 0;
  int sy = args.size() > 5 ? (int)args[5].as_int() : 0;
  int sw = args.size() > 6 ? (int)args[6].as_int() : src->width;
  int sh = args.size() > 7 ? (int)args[7].as_int() : src->height;
  double alpha = args.size() > 8 ? args[8].as_double() : 1.0;
  blit_canvas(*dst, *src, dx, dy, sx, sy, sw, sh, alpha);
  return args[0];
}

Value bi_canvas_present(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_present");
  if (!cv) return Value::nil();
  auto res = std::make_shared<MapObj>();
  res->put("width", Value::integer(cv->width));
  res->put("height", Value::integer(cv->height));
  res->put("format", Value::string("argb32"));
  res->put("pixels", Value::integer(cv->pixels.size()));
  return Value::object(Type::Map, res);
}

// ---------------------------------------------------------------------------
// Advanced Color Science & Palette Generation
// ---------------------------------------------------------------------------

void hsl_to_rgb(double h, double s, double l, uint8_t& r, uint8_t& g, uint8_t& b) {
  h = std::fmod(h, 360.0);
  if (h < 0.0) h += 360.0;
  if (s > 1.0) s /= 100.0;
  if (l > 1.0) l /= 100.0;
  s = std::clamp(s, 0.0, 1.0);
  l = std::clamp(l, 0.0, 1.0);

  double c = (1.0 - std::abs(2.0 * l - 1.0)) * s;
  double hp = h / 60.0;
  double x = c * (1.0 - std::abs(std::fmod(hp, 2.0) - 1.0));
  double m = l - c / 2.0;

  double r1 = 0, g1 = 0, b1 = 0;
  if (hp >= 0 && hp < 1) { r1 = c; g1 = x; b1 = 0; }
  else if (hp >= 1 && hp < 2) { r1 = x; g1 = c; b1 = 0; }
  else if (hp >= 2 && hp < 3) { r1 = 0; g1 = c; b1 = x; }
  else if (hp >= 3 && hp < 4) { r1 = 0; g1 = x; b1 = c; }
  else if (hp >= 4 && hp < 5) { r1 = x; g1 = 0; b1 = c; }
  else if (hp >= 5 && hp < 6) { r1 = c; g1 = 0; b1 = x; }

  r = static_cast<uint8_t>(std::clamp((r1 + m) * 255.0, 0.0, 255.0));
  g = static_cast<uint8_t>(std::clamp((g1 + m) * 255.0, 0.0, 255.0));
  b = static_cast<uint8_t>(std::clamp((b1 + m) * 255.0, 0.0, 255.0));
}

void rgb_to_hsl(uint8_t r, uint8_t g, uint8_t b, double& h, double& s, double& l) {
  double rd = r / 255.0;
  double gd = g / 255.0;
  double bd = b / 255.0;

  double max_val = std::max({rd, gd, bd});
  double min_val = std::min({rd, gd, bd});
  double delta = max_val - min_val;

  l = (max_val + min_val) / 2.0;

  if (delta < 1e-6) {
    h = 0;
    s = 0;
  } else {
    s = (l > 0.5) ? (delta / (2.0 - max_val - min_val)) : (delta / (max_val + min_val));
    if (max_val == rd) {
      h = (gd - bd) / delta + (gd < bd ? 6.0 : 0.0);
    } else if (max_val == gd) {
      h = (bd - rd) / delta + 2.0;
    } else {
      h = (rd - gd) / delta + 4.0;
    }
    h *= 60.0;
  }
}

Value bi_color_hsl(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "color_hsl") || !want_number(args[1], error, "color_hsl") || !want_number(args[2], error, "color_hsl"))
    return Value::nil();
  double h = args[0].as_double();
  double s = args[1].as_double();
  double l = args[2].as_double();
  uint8_t r = 0, g = 0, b = 0;
  hsl_to_rgb(h, s, l, r, g, b);
  return Value::integer((int64_t)(0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b));
}

Value bi_color_hsla(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "color_hsla") || !want_number(args[1], error, "color_hsla") || !want_number(args[2], error, "color_hsla"))
    return Value::nil();
  double h = args[0].as_double();
  double s = args[1].as_double();
  double l = args[2].as_double();
  double a = 1.0;
  if (args.size() > 3) {
    if (args[3].is_number()) a = args[3].as_double();
    if (a > 1.0) a /= 255.0;
  }
  uint8_t r = 0, g = 0, b = 0;
  hsl_to_rgb(h, s, l, r, g, b);
  uint8_t ia = static_cast<uint8_t>(std::clamp(a * 255.0, 0.0, 255.0));
  return Value::integer((int64_t)(((uint32_t)ia << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b));
}

Value bi_color_to_hsl(Vm&, std::vector<Value>& args, std::string&) {
  uint32_t c = parse_color_value(args[0]);
  uint8_t a = (c >> 24) & 0xFF;
  uint8_t r = (c >> 16) & 0xFF;
  uint8_t g = (c >> 8) & 0xFF;
  uint8_t b = c & 0xFF;
  double h = 0, s = 0, l = 0;
  rgb_to_hsl(r, g, b, h, s, l);

  auto res = std::make_shared<MapObj>();
  res->put("h", Value::real(h));
  res->put("s", Value::real(s));
  res->put("l", Value::real(l));
  res->put("a", Value::real(a / 255.0));
  return Value::object(Type::Map, res);
}

Value bi_color_lighten(Vm&, std::vector<Value>& args, std::string& error) {
  uint32_t c = parse_color_value(args[0]);
  double amount = args.size() > 1 && args[1].is_number() ? args[1].as_double() : 0.1;
  if (amount > 1.0) amount /= 100.0;
  uint8_t a = (c >> 24) & 0xFF;
  uint8_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
  double h = 0, s = 0, l = 0;
  rgb_to_hsl(r, g, b, h, s, l);
  l = std::clamp(l + amount, 0.0, 1.0);
  hsl_to_rgb(h, s, l, r, g, b);
  return Value::integer((int64_t)(((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b));
}

Value bi_color_darken(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (args.size() > 1 && args[1].is_number()) {
    args[1] = Value::real(-args[1].as_double());
  }
  return bi_color_lighten(vm, args, error);
}

Value bi_color_blend(Vm&, std::vector<Value>& args, std::string& error) {
  uint32_t c1 = parse_color_value(args[0]);
  uint32_t c2 = parse_color_value(args[1]);
  double factor = args.size() > 2 && args[2].is_number() ? std::clamp(args[2].as_double(), 0.0, 1.0) : 0.5;

  int a1 = (c1 >> 24) & 0xFF, r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF;
  int a2 = (c2 >> 24) & 0xFF, r2 = (c2 >> 16) & 0xFF, g2 = (c2 >> 8) & 0xFF, b2 = c2 & 0xFF;

  int a = (int)(a1 + (a2 - a1) * factor);
  int r = (int)(r1 + (r2 - r1) * factor);
  int g = (int)(g1 + (g2 - g1) * factor);
  int b = (int)(b1 + (b2 - b1) * factor);

  return Value::integer((int64_t)(((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b));
}

Value bi_color_invert(Vm&, std::vector<Value>& args, std::string&) {
  uint32_t c = parse_color_value(args[0]);
  uint32_t a = (c >> 24) & 0xFF;
  uint32_t r = 255 - ((c >> 16) & 0xFF);
  uint32_t g = 255 - ((c >> 8) & 0xFF);
  uint32_t b = 255 - (c & 0xFF);
  return Value::integer((int64_t)((a << 24) | (r << 16) | (g << 8) | b));
}

double get_luminance(uint32_t c) {
  auto srgb = [](double v) {
    v /= 255.0;
    return (v <= 0.03928) ? (v / 12.92) : std::pow((v + 0.055) / 1.055, 2.4);
  };
  double r = srgb((c >> 16) & 0xFF);
  double g = srgb((c >> 8) & 0xFF);
  double b = srgb(c & 0xFF);
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

Value bi_color_luminance(Vm&, std::vector<Value>& args, std::string&) {
  uint32_t c = parse_color_value(args[0]);
  return Value::real(get_luminance(c));
}

Value bi_color_contrast(Vm&, std::vector<Value>& args, std::string&) {
  uint32_t c1 = parse_color_value(args[0]);
  uint32_t c2 = parse_color_value(args[1]);
  double l1 = get_luminance(c1);
  double l2 = get_luminance(c2);
  double ratio = (std::max(l1, l2) + 0.05) / (std::min(l1, l2) + 0.05);
  return Value::real(ratio);
}

Value bi_palette_analogous(Vm&, std::vector<Value>& args, std::string&) {
  uint32_t c = parse_color_value(args[0]);
  int count = args.size() > 1 && args[1].is_number() ? std::max(1, (int)args[1].as_int()) : 3;
  double step = args.size() > 2 && args[2].is_number() ? args[2].as_double() : 30.0;

  uint8_t a = (c >> 24) & 0xFF, r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
  double h = 0, s = 0, l = 0;
  rgb_to_hsl(r, g, b, h, s, l);

  std::vector<Value> out;
  double start_h = h - ((count - 1) * step * 0.5);
  for (int i = 0; i < count; ++i) {
    uint8_t nr = 0, ng = 0, nb = 0;
    hsl_to_rgb(start_h + i * step, s, l, nr, ng, nb);
    out.push_back(Value::integer((int64_t)(((uint32_t)a << 24) | ((uint32_t)nr << 16) | ((uint32_t)ng << 8) | nb)));
  }
  return Value::list(std::move(out));
}

Value bi_palette_triadic(Vm&, std::vector<Value>& args, std::string&) {
  uint32_t c = parse_color_value(args[0]);
  uint8_t a = (c >> 24) & 0xFF, r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
  double h = 0, s = 0, l = 0;
  rgb_to_hsl(r, g, b, h, s, l);

  std::vector<Value> out;
  for (int i = 0; i < 3; ++i) {
    uint8_t nr = 0, ng = 0, nb = 0;
    hsl_to_rgb(h + i * 120.0, s, l, nr, ng, nb);
    out.push_back(Value::integer((int64_t)(((uint32_t)a << 24) | ((uint32_t)nr << 16) | ((uint32_t)ng << 8) | nb)));
  }
  return Value::list(std::move(out));
}

Value bi_palette_monochromatic(Vm&, std::vector<Value>& args, std::string&) {
  uint32_t c = parse_color_value(args[0]);
  int count = args.size() > 1 && args[1].is_number() ? std::max(2, (int)args[1].as_int()) : 5;
  uint8_t a = (c >> 24) & 0xFF, r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
  double h = 0, s = 0, l = 0;
  rgb_to_hsl(r, g, b, h, s, l);

  std::vector<Value> out;
  for (int i = 0; i < count; ++i) {
    double nl = 0.15 + 0.70 * ((double)i / (count - 1));
    uint8_t nr = 0, ng = 0, nb = 0;
    hsl_to_rgb(h, s, nl, nr, ng, nb);
    out.push_back(Value::integer((int64_t)(((uint32_t)a << 24) | ((uint32_t)nr << 16) | ((uint32_t)ng << 8) | nb)));
  }
  return Value::list(std::move(out));
}

// ---------------------------------------------------------------------------
// Animation, Easing & Motion Dynamics
// ---------------------------------------------------------------------------

Value bi_lerp(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "lerp") || !want_number(args[1], error, "lerp") || !want_number(args[2], error, "lerp"))
    return Value::nil();
  double a = args[0].as_double();
  double b = args[1].as_double();
  double t = args[2].as_double();
  return Value::real(a + (b - a) * t);
}

Value bi_smoothstep(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "smoothstep") || !want_number(args[1], error, "smoothstep") || !want_number(args[2], error, "smoothstep"))
    return Value::nil();
  double edge0 = args[0].as_double();
  double edge1 = args[1].as_double();
  double x = args[2].as_double();
  double t = std::clamp((x - edge0) / (edge1 - edge0 > 1e-6 ? edge1 - edge0 : 1.0), 0.0, 1.0);
  return Value::real(t * t * (3.0 - 2.0 * t));
}

double compute_ease_in(double t, const std::string& type) {
  t = std::clamp(t, 0.0, 1.0);
  if (type == "quad") return t * t;
  if (type == "cubic") return t * t * t;
  if (type == "quart") return t * t * t * t;
  if (type == "sine") return 1.0 - std::cos((t * 3.141592653589793) * 0.5);
  if (type == "expo") return t == 0.0 ? 0.0 : std::pow(2.0, 10.0 * (t - 1.0));
  if (type == "circ") return 1.0 - std::sqrt(1.0 - t * t);
  if (type == "back") { double c1 = 1.70158; return (c1 + 1.0) * t * t * t - c1 * t * t; }
  if (type == "elastic") {
    if (t == 0.0 || t == 1.0) return t;
    return -std::pow(2.0, 10.0 * (t - 1.0)) * std::sin((t - 1.1) * 5.0 * 3.141592653589793);
  }
  if (type == "bounce") {
    // easeOutBounce(1 - t)
    double inv = 1.0 - t;
    double n1 = 7.5625, d1 = 2.75;
    double out = 0;
    if (inv < 1.0 / d1) out = n1 * inv * inv;
    else if (inv < 2.0 / d1) { inv -= 1.5 / d1; out = n1 * inv * inv + 0.75; }
    else if (inv < 2.5 / d1) { inv -= 2.25 / d1; out = n1 * inv * inv + 0.9375; }
    else { inv -= 2.625 / d1; out = n1 * inv * inv + 0.984375; }
    return 1.0 - out;
  }
  return t; // default linear
}

Value bi_ease_in(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "ease_in")) return Value::nil();
  double t = args[0].as_double();
  std::string type = args.size() > 1 && args[1].type == Type::Str ? args[1].str() : "cubic";
  return Value::real(compute_ease_in(t, type));
}

Value bi_ease_out(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "ease_out")) return Value::nil();
  double t = args[0].as_double();
  std::string type = args.size() > 1 && args[1].type == Type::Str ? args[1].str() : "cubic";
  return Value::real(1.0 - compute_ease_in(1.0 - t, type));
}

Value bi_ease_in_out(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "ease_in_out")) return Value::nil();
  double t = args[0].as_double();
  std::string type = args.size() > 1 && args[1].type == Type::Str ? args[1].str() : "cubic";
  if (t < 0.5) return Value::real(compute_ease_in(t * 2.0, type) * 0.5);
  return Value::real(1.0 - compute_ease_in((1.0 - t) * 2.0, type) * 0.5);
}

Value bi_spring(Vm&, std::vector<Value>& args, std::string& error) {
  if (args.size() < 6) { error = "spring() wants (pos, target, vel, stiffness, damping, dt)"; return Value::nil(); }
  double pos = args[0].as_double();
  double target = args[1].as_double();
  double vel = args[2].as_double();
  double k = args[3].as_double();
  double d = args[4].as_double();
  double dt = args[5].as_double();

  double force = -k * (pos - target) - d * vel;
  vel += force * dt;
  pos += vel * dt;

  auto res = std::make_shared<MapObj>();
  res->put("pos", Value::real(pos));
  res->put("vel", Value::real(vel));
  return Value::object(Type::Map, res);
}

// ---------------------------------------------------------------------------
// Image Filters, Shadows & Frosted Glass
// ---------------------------------------------------------------------------

void blur_canvas_box(GrainCanvas& cv, int radius) {
  if (radius <= 0 || cv.width == 0 || cv.height == 0) return;
  std::vector<uint32_t> temp = cv.pixels;
  int w = (int)cv.width;
  int h = (int)cv.height;

  // Horizontal pass
  for (int y = 0; y < h; ++y) {
    int row_off = y * w;
    for (int x = 0; x < w; ++x) {
      int r_acc = 0, g_acc = 0, b_acc = 0, a_acc = 0, count = 0;
      for (int k = -radius; k <= radius; ++k) {
        int nx = std::clamp(x + k, 0, w - 1);
        uint32_t p = cv.pixels[row_off + nx];
        a_acc += (p >> 24) & 0xFF;
        r_acc += (p >> 16) & 0xFF;
        g_acc += (p >> 8) & 0xFF;
        b_acc += p & 0xFF;
        count++;
      }
      temp[row_off + x] = ((a_acc / count) << 24) | ((r_acc / count) << 16) | ((g_acc / count) << 8) | (b_acc / count);
    }
  }

  // Vertical pass
  for (int x = 0; x < w; ++x) {
    for (int y = 0; y < h; ++y) {
      int r_acc = 0, g_acc = 0, b_acc = 0, a_acc = 0, count = 0;
      for (int k = -radius; k <= radius; ++k) {
        int ny = std::clamp(y + k, 0, h - 1);
        uint32_t p = temp[ny * w + x];
        a_acc += (p >> 24) & 0xFF;
        r_acc += (p >> 16) & 0xFF;
        g_acc += (p >> 8) & 0xFF;
        b_acc += p & 0xFF;
        count++;
      }
      cv.pixels[y * w + x] = ((a_acc / count) << 24) | ((r_acc / count) << 16) | ((g_acc / count) << 8) | (b_acc / count);
    }
  }
}

Value bi_canvas_blur(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_blur");
  if (!cv) return Value::nil();
  int radius = args.size() > 1 && args[1].is_number() ? (int)args[1].as_int() : 3;
  blur_canvas_box(*cv, radius);
  return args[0];
}

Value bi_canvas_draw_shadow(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_draw_shadow");
  if (!cv) return Value::nil();
  if (args.size() < 5) { error = "canvas_draw_shadow wants (cv, x, y, w, h, [radius, color, blur, ox, oy])"; return Value::nil(); }
  double x = args[1].as_double(), y = args[2].as_double(), w = args[3].as_double(), h = args[4].as_double();
  double radius = args.size() > 5 ? args[5].as_double() : 8.0;
  uint32_t color = args.size() > 6 ? parse_color_value(args[6], 0x40000000) : 0x40000000;
  int blur = args.size() > 7 ? (int)args[7].as_int() : 8;
  double ox = args.size() > 8 ? args[8].as_double() : 0.0;
  double oy = args.size() > 9 ? args[9].as_double() : 4.0;

  // Draw soft shadow layers
  for (int b = blur; b >= 1; b -= 2) {
    double expand = b * 0.5;
    double alpha_factor = (1.0 / (blur + 1)) * 1.5;
    uint8_t a = static_cast<uint8_t>(std::clamp(((color >> 24) & 0xFF) * alpha_factor, 0.0, 255.0));
    uint32_t c = (a << 24) | (color & 0x00FFFFFF);
    fill_round_rect_aa(*cv, x + ox - expand, y + oy - expand, w + expand * 2.0, h + expand * 2.0, radius + expand, c);
  }
  return args[0];
}

Value bi_canvas_draw_glow(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_draw_glow");
  if (!cv) return Value::nil();
  if (args.size() < 4) { error = "canvas_draw_glow wants (cv, cx, cy, r, [color, blur])"; return Value::nil(); }
  double cx = args[1].as_double(), cy = args[2].as_double(), r = args[3].as_double();
  uint32_t color = args.size() > 4 ? parse_color_value(args[4], 0x803B82F6) : 0x803B82F6;
  int blur = args.size() > 5 ? (int)args[5].as_int() : 12;

  for (int b = blur; b >= 1; b -= 2) {
    double expand = b * 0.75;
    double factor = (1.0 / (blur + 1)) * 1.2;
    uint8_t a = static_cast<uint8_t>(std::clamp(((color >> 24) & 0xFF) * factor, 0.0, 255.0));
    uint32_t c = (a << 24) | (color & 0x00FFFFFF);
    fill_circle_aa(*cv, cx, cy, r + expand, c);
  }
  return args[0];
}

Value bi_canvas_draw_glass(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "canvas_draw_glass");
  if (!cv) return Value::nil();
  if (args.size() < 5) { error = "canvas_draw_glass wants (cv, x, y, w, h, [radius, bg_color, border_color])"; return Value::nil(); }
  double x = args[1].as_double(), y = args[2].as_double(), w = args[3].as_double(), h = args[4].as_double();
  double radius = 12.0;
  uint32_t bg = 0x30FFFFFF;
  uint32_t border = 0x50FFFFFF;

  if (args.size() > 5) radius = args[5].as_double();
  if (args.size() > 8) {
    // Format: (cv, x, y, w, h, radius, blur, bg, border)
    bg = parse_color_value(args[7], bg);
    border = parse_color_value(args[8], border);
  } else {
    // Format: (cv, x, y, w, h, radius, bg, border)
    if (args.size() > 6) bg = parse_color_value(args[6], bg);
    if (args.size() > 7) border = parse_color_value(args[7], border);
  }

  fill_round_rect_aa(*cv, x, y, w, h, radius, bg);
  stroke_round_rect_aa(*cv, x, y, w, h, radius, border, 1.0);
  return args[0];
}

// ---------------------------------------------------------------------------
// High Quality Application UI Widgets & Data Visualizations
// ---------------------------------------------------------------------------

Value bi_draw_card(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "draw_card");
  if (!cv) return Value::nil();
  if (args.size() < 5) { error = "draw_card wants (cv, x, y, w, h, [radius, bg_color, border_color, shadow_blur])"; return Value::nil(); }
  double x = args[1].as_double(), y = args[2].as_double(), w = args[3].as_double(), h = args[4].as_double();
  double radius = args.size() > 5 ? args[5].as_double() : 10.0;
  uint32_t bg = args.size() > 6 ? parse_color_value(args[6], 0xFF1E293B) : 0xFF1E293B; // Slate 800
  uint32_t border = args.size() > 7 ? parse_color_value(args[7], 0xFF334155) : 0xFF334155; // Slate 700
  int shadow_blur = args.size() > 8 ? (int)args[8].as_int() : 6;

  if (shadow_blur > 0) {
    for (int b = shadow_blur; b >= 1; b -= 2) {
      double expand = b * 0.5;
      fill_round_rect_aa(*cv, x - expand, y + 3.0 - expand, w + expand * 2.0, h + expand * 2.0, radius + expand, 0x18000000);
    }
  }
  fill_round_rect_aa(*cv, x, y, w, h, radius, bg);
  stroke_round_rect_aa(*cv, x, y, w, h, radius, border, 1.0);
  return args[0];
}

Value bi_draw_badge(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "draw_badge");
  if (!cv) return Value::nil();
  if (args.size() < 4) { error = "draw_badge wants (cv, x, y, label, [bg_color, text_color, radius])"; return Value::nil(); }
  double x = args[1].as_double(), y = args[2].as_double();
  std::string label = args[3].to_text();
  uint32_t bg = args.size() > 4 ? parse_color_value(args[4], 0xFF0284C7) : 0xFF0284C7; // Sky 600
  uint32_t text_col = args.size() > 5 ? parse_color_value(args[5], 0xFFFFFFFF) : 0xFFFFFFFF;
  double radius = args.size() > 6 ? args[6].as_double() : 6.0;

  auto metrics = measure_text_metrics(label, "ui_small", 12);
  int text_w = (int)metrics->find("width")->as_int();
  int text_h = (int)metrics->find("height")->as_int();
  double pad_x = 8.0, pad_y = 3.0;
  double bw = text_w + pad_x * 2.0;
  double bh = text_h + pad_y * 2.0;

  fill_round_rect_aa(*cv, x, y, bw, bh, radius, bg);
  draw_text_aa(*cv, label, x + pad_x, y + pad_y, text_col, "ui_small", 12);
  return args[0];
}

Value bi_draw_button(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "draw_button");
  if (!cv) return Value::nil();
  if (args.size() < 6) { error = "draw_button wants (cv, x, y, w, h, label, [variant, state])"; return Value::nil(); }
  double x = args[1].as_double(), y = args[2].as_double(), w = args[3].as_double(), h = args[4].as_double();
  std::string label = args[5].to_text();
  std::string variant = args.size() > 6 && args[6].type == Type::Str ? args[6].str() : "primary";
  std::string state = args.size() > 7 && args[7].type == Type::Str ? args[7].str() : "normal";

  uint32_t bg = 0xFF2563EB; // Blue 600
  uint32_t text_col = 0xFFFFFFFF;
  uint32_t border = 0x00000000;

  if (variant == "secondary") {
    bg = 0xFF334155; border = 0xFF475569;
  } else if (variant == "danger") {
    bg = 0xFFDC2626;
  } else if (variant == "ghost") {
    bg = 0x00000000; text_col = 0xFF94A3B8; border = 0xFF475569;
  } else if (variant == "success") {
    bg = 0xFF16A34A;
  }

  if (state == "hover") {
    uint8_t a = (bg >> 24) & 0xFF, r = (bg >> 16) & 0xFF, g = (bg >> 8) & 0xFF, b = bg & 0xFF;
    double hue = 0, sat = 0, lit = 0;
    rgb_to_hsl(r, g, b, hue, sat, lit);
    lit = std::clamp(lit + 0.08, 0.0, 1.0);
    hsl_to_rgb(hue, sat, lit, r, g, b);
    bg = (a << 24) | (r << 16) | (g << 8) | b;
  } else if (state == "active" || state == "pressed") {
    uint8_t a = (bg >> 24) & 0xFF, r = (bg >> 16) & 0xFF, g = (bg >> 8) & 0xFF, b = bg & 0xFF;
    double hue = 0, sat = 0, lit = 0;
    rgb_to_hsl(r, g, b, hue, sat, lit);
    lit = std::clamp(lit - 0.08, 0.0, 1.0);
    hsl_to_rgb(hue, sat, lit, r, g, b);
    bg = (a << 24) | (r << 16) | (g << 8) | b;
  }

  fill_round_rect_aa(*cv, x, y, w, h, 6.0, bg);
  if ((border & 0xFF000000) != 0) {
    stroke_round_rect_aa(*cv, x, y, w, h, 6.0, border, 1.0);
  }

  auto metrics = measure_text_metrics(label, "ui_bold", 14);
  int tw = (int)metrics->find("width")->as_int();
  int th = (int)metrics->find("height")->as_int();
  double tx = x + (w - tw) * 0.5;
  double ty = y + (h - th) * 0.5;
  draw_text_aa(*cv, label, tx, ty, text_col, "ui_bold", 14);
  return args[0];
}

Value bi_draw_progress_bar(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "draw_progress_bar");
  if (!cv) return Value::nil();
  if (args.size() < 6) { error = "draw_progress_bar wants (cv, x, y, w, h, percent, [fill_color, bg_color, radius])"; return Value::nil(); }
  double x = args[1].as_double(), y = args[2].as_double(), w = args[3].as_double(), h = args[4].as_double();
  double pct = args[5].as_double();
  if (pct > 1.0) pct /= 100.0;
  pct = std::clamp(pct, 0.0, 1.0);
  uint32_t fill_col = args.size() > 6 ? parse_color_value(args[6], 0xFF3B82F6) : 0xFF3B82F6; // Blue 500
  uint32_t bg_col = args.size() > 7 ? parse_color_value(args[7], 0xFF334155) : 0xFF334155;     // Slate 700
  double radius = args.size() > 8 ? args[8].as_double() : h * 0.5;

  fill_round_rect_aa(*cv, x, y, w, h, radius, bg_col);
  if (pct > 0.01) {
    fill_round_rect_aa(*cv, x, y, w * pct, h, radius, fill_col);
  }
  return args[0];
}

Value bi_draw_sparkline(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "draw_sparkline");
  if (!cv) return Value::nil();
  if (args.size() < 6) { error = "draw_sparkline wants (cv, x, y, w, h, data_points, [line_color, fill_color, line_width])"; return Value::nil(); }
  double x = args[1].as_double(), y = args[2].as_double(), w = args[3].as_double(), h = args[4].as_double();
  if (!want_list(args[5], error, "draw_sparkline")) return Value::nil();
  const auto& pts = args[5].list_items();
  if (pts.size() < 2) return args[0];

  uint32_t line_col = args.size() > 6 ? parse_color_value(args[6], 0xFF38BDF8) : 0xFF38BDF8; // Light Blue
  uint32_t fill_col = args.size() > 7 ? parse_color_value(args[7], 0x3038BDF8) : 0x3038BDF8;
  double line_w = args.size() > 8 ? args[8].as_double() : 2.0;

  double min_v = pts[0].as_double(), max_v = pts[0].as_double();
  for (const auto& p : pts) {
    double v = p.as_double();
    min_v = std::min(min_v, v);
    max_v = std::max(max_v, v);
  }
  double range = (max_v - min_v > 1e-6) ? (max_v - min_v) : 1.0;

  std::vector<GrainCanvas::Point> coords;
  coords.reserve(pts.size());
  for (size_t i = 0; i < pts.size(); ++i) {
    double px = x + (w * i) / (pts.size() - 1);
    double norm = (pts[i].as_double() - min_v) / range;
    double py = y + h - (norm * h);
    coords.push_back({px, py});
  }

  // Draw fill polygon under sparkline
  if ((fill_col & 0xFF000000) != 0) {
    std::vector<GrainCanvas::Point> poly = coords;
    poly.push_back({x + w, y + h});
    poly.push_back({x, y + h});
    fill_polygon_aa(*cv, poly, fill_col);
  }

  // Draw lines
  for (size_t i = 0; i + 1 < coords.size(); ++i) {
    draw_line_aa(*cv, coords[i].x, coords[i].y, coords[i + 1].x, coords[i + 1].y, line_col, line_w);
  }
  return args[0];
}

Value bi_draw_bar_chart(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "draw_bar_chart");
  if (!cv) return Value::nil();
  if (args.size() < 6) { error = "draw_bar_chart wants (cv, x, y, w, h, data_points, [labels, bar_color, bg_color])"; return Value::nil(); }
  double x = args[1].as_double(), y = args[2].as_double(), w = args[3].as_double(), h = args[4].as_double();
  if (!want_list(args[5], error, "draw_bar_chart")) return Value::nil();
  const auto& pts = args[5].list_items();
  if (pts.empty()) return args[0];

  uint32_t bar_col = args.size() > 7 ? parse_color_value(args[7], 0xFF3B82F6) : 0xFF3B82F6;
  uint32_t bg_col = args.size() > 8 ? parse_color_value(args[8], 0xFF1E293B) : 0xFF1E293B;

  fill_round_rect_aa(*cv, x, y, w, h, 8.0, bg_col);

  double max_v = 1.0;
  for (const auto& p : pts) max_v = std::max(max_v, p.as_double());

  double chart_h = h - 24.0; // reserve bottom for labels
  double slot_w = (w - 16.0) / pts.size();
  double bar_w = slot_w * 0.7;
  double gap = slot_w * 0.3;

  for (size_t i = 0; i < pts.size(); ++i) {
    double bh = (pts[i].as_double() / max_v) * (chart_h - 10.0);
    double bx = x + 8.0 + i * slot_w + gap * 0.5;
    double by = y + chart_h - bh;
    fill_round_rect_aa(*cv, bx, by, bar_w, bh, 3.0, bar_col);

    if (args.size() > 6 && args[6].type == Type::List && i < args[6].list_items().size()) {
      std::string lbl = args[6].list_items()[i].to_text();
      draw_text_aa(*cv, lbl, bx, y + chart_h + 4.0, 0xFF94A3B8, "ui_small", 11);
    }
  }
  return args[0];
}

Value bi_draw_donut_chart(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "draw_donut_chart");
  if (!cv) return Value::nil();
  if (args.size() < 6) { error = "draw_donut_chart wants (cv, cx, cy, outer_r, inner_r, values, [colors])"; return Value::nil(); }
  double cx = args[1].as_double(), cy = args[2].as_double();
  double outer_r = args[3].as_double(), inner_r = args[4].as_double();
  if (!want_list(args[5], error, "draw_donut_chart")) return Value::nil();
  const auto& vals = args[5].list_items();
  if (vals.empty()) return args[0];

  double total = 0.0;
  for (const auto& v : vals) total += std::max(0.0, v.as_double());
  if (total < 1e-6) total = 1.0;

  static const uint32_t default_colors[] = {0xFF3B82F6, 0xFF10B981, 0xFFF59E0B, 0xFFEF4444, 0xFF8B5CF6, 0xFF06B6D4};
  size_t def_colors_cnt = sizeof(default_colors) / sizeof(default_colors[0]);

  double start_angle = -3.141592653589793 * 0.5;
  for (size_t i = 0; i < vals.size(); ++i) {
    double slice = (vals[i].as_double() / total) * (2.0 * 3.141592653589793);
    double end_angle = start_angle + slice;

    uint32_t col = default_colors[i % def_colors_cnt];
    if (args.size() > 6 && args[6].type == Type::List && i < args[6].list_items().size()) {
      col = parse_color_value(args[6].list_items()[i], col);
    }

    // Rasterize wedge
    int steps = std::max(8, (int)(slice * outer_r * 0.5));
    std::vector<GrainCanvas::Point> wedge;
    for (int s = 0; s <= steps; ++s) {
      double a = start_angle + (slice * s) / steps;
      wedge.push_back({cx + outer_r * std::cos(a), cy + outer_r * std::sin(a)});
    }
    for (int s = steps; s >= 0; --s) {
      double a = start_angle + (slice * s) / steps;
      wedge.push_back({cx + inner_r * std::cos(a), cy + inner_r * std::sin(a)});
    }
    fill_polygon_aa(*cv, wedge, col);
    start_angle = end_angle;
  }
  return args[0];
}

Value bi_draw_metric_tile(Vm& vm, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "draw_metric_tile");
  if (!cv) return Value::nil();
  if (args.size() < 7) { error = "draw_metric_tile wants (cv, x, y, w, h, title, value, [unit, delta, delta_good, spark_data])"; return Value::nil(); }
  double x = args[1].as_double(), y = args[2].as_double(), w = args[3].as_double(), h = args[4].as_double();
  std::string title = args[5].to_text();
  std::string value = args[6].to_text();
  std::string unit = args.size() > 7 && args[7].type == Type::Str ? args[7].str() : "";
  std::string delta = args.size() > 8 && args[8].type == Type::Str ? args[8].str() : "";
  bool delta_good = args.size() > 9 ? args[9].truthy() : true;

  // Background Card
  std::vector<Value> card_args = {args[0], args[1], args[2], args[3], args[4], Value::real(10.0), Value::integer((int64_t)0xFF1E293B)};
  bi_draw_card(vm, card_args, error);

  // Title
  draw_text_aa(*cv, title, x + 14.0, y + 12.0, 0xFF94A3B8, "ui_small", 12);

  // Big Value
  std::string val_str = value + (unit.empty() ? "" : " " + unit);
  draw_text_aa(*cv, val_str, x + 14.0, y + 30.0, 0xFFF8FAFC, "ui_bold_21", 20);

  // Delta Badge
  if (!delta.empty()) {
    uint32_t badge_bg = delta_good ? 0x3016A34A : 0x30DC2626;
    uint32_t badge_fg = delta_good ? 0xFF4ADE80 : 0xFFF87171;
    std::vector<Value> badge_args = {args[0], Value::real(x + 14.0), Value::real(y + h - 26.0), Value::string(delta),
                                     Value::integer((int64_t)badge_bg), Value::integer((int64_t)badge_fg)};
    bi_draw_badge(vm, badge_args, error);
  }

  // Mini Sparkline
  if (args.size() > 10 && args[10].type == Type::List) {
    double sp_w = w * 0.35;
    double sp_h = 24.0;
    std::vector<Value> sp_args = {args[0], Value::real(x + w - sp_w - 14.0), Value::real(y + h - sp_h - 14.0),
                                  Value::real(sp_w), Value::real(sp_h), args[10]};
    bi_draw_sparkline(vm, sp_args, error);
  }
  return args[0];
}

Value bi_draw_icon(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "draw_icon");
  if (!cv) return Value::nil();
  if (args.size() < 5) { error = "draw_icon wants (cv, x, y, size, icon_name, [color, line_width])"; return Value::nil(); }
  double x = args[1].as_double(), y = args[2].as_double(), s = args[3].as_double();
  std::string name = args[4].str();
  uint32_t col = args.size() > 5 ? parse_color_value(args[5], 0xFFFFFFFF) : 0xFFFFFFFF;
  double lw = args.size() > 6 ? args[6].as_double() : 2.0;

  if (name == "check") {
    draw_line_aa(*cv, x + s * 0.2, y + s * 0.5, x + s * 0.45, y + s * 0.75, col, lw);
    draw_line_aa(*cv, x + s * 0.45, y + s * 0.75, x + s * 0.8, y + s * 0.25, col, lw);
  } else if (name == "chevron_right" || name == "arrow_right") {
    draw_line_aa(*cv, x + s * 0.35, y + s * 0.25, x + s * 0.65, y + s * 0.5, col, lw);
    draw_line_aa(*cv, x + s * 0.65, y + s * 0.5, x + s * 0.35, y + s * 0.75, col, lw);
  } else if (name == "chevron_down" || name == "arrow_down") {
    draw_line_aa(*cv, x + s * 0.25, y + s * 0.35, x + s * 0.5, y + s * 0.65, col, lw);
    draw_line_aa(*cv, x + s * 0.5, y + s * 0.65, x + s * 0.75, y + s * 0.35, col, lw);
  } else if (name == "cross" || name == "close") {
    draw_line_aa(*cv, x + s * 0.25, y + s * 0.25, x + s * 0.75, y + s * 0.75, col, lw);
    draw_line_aa(*cv, x + s * 0.75, y + s * 0.25, x + s * 0.25, y + s * 0.75, col, lw);
  } else if (name == "search") {
    stroke_circle_aa(*cv, x + s * 0.45, y + s * 0.45, s * 0.25, col, lw);
    draw_line_aa(*cv, x + s * 0.65, y + s * 0.65, x + s * 0.85, y + s * 0.85, col, lw);
  } else if (name == "star") {
    stroke_circle_aa(*cv, x + s * 0.5, y + s * 0.5, s * 0.35, col, lw);
    fill_circle_aa(*cv, x + s * 0.5, y + s * 0.5, s * 0.15, col);
  } else if (name == "warning" || name == "alert") {
    draw_line_aa(*cv, x + s * 0.5, y + s * 0.15, x + s * 0.15, y + s * 0.85, col, lw);
    draw_line_aa(*cv, x + s * 0.15, y + s * 0.85, x + s * 0.85, y + s * 0.85, col, lw);
    draw_line_aa(*cv, x + s * 0.85, y + s * 0.85, x + s * 0.5, y + s * 0.15, col, lw);
    draw_line_aa(*cv, x + s * 0.5, y + s * 0.4, x + s * 0.5, y + s * 0.65, col, lw);
    fill_circle_aa(*cv, x + s * 0.5, y + s * 0.75, lw * 0.75, col);
  } else if (name == "graph" || name == "chart") {
    draw_line_aa(*cv, x + s * 0.2, y + s * 0.8, x + s * 0.8, y + s * 0.8, col, lw);
    draw_line_aa(*cv, x + s * 0.2, y + s * 0.8, x + s * 0.2, y + s * 0.2, col, lw);
    draw_line_aa(*cv, x + s * 0.2, y + s * 0.7, x + s * 0.45, y + s * 0.45, col, lw);
    draw_line_aa(*cv, x + s * 0.45, y + s * 0.45, x + s * 0.65, y + s * 0.55, col, lw);
    draw_line_aa(*cv, x + s * 0.65, y + s * 0.55, x + s * 0.8, y + s * 0.3, col, lw);
  } else if (name == "play") {
    std::vector<GrainCanvas::Point> tri = {
      {x + s * 0.3, y + s * 0.2}, {x + s * 0.75, y + s * 0.5}, {x + s * 0.3, y + s * 0.8}
    };
    fill_polygon_aa(*cv, tri, col);
  } else if (name == "pause") {
    fill_rect_aa(*cv, x + s * 0.25, y + s * 0.2, s * 0.18, s * 0.6, col);
    fill_rect_aa(*cv, x + s * 0.57, y + s * 0.2, s * 0.18, s * 0.6, col);
  } else if (name == "database" || name == "db") {
    stroke_ellipse_aa(*cv, x + s * 0.5, y + s * 0.25, s * 0.35, s * 0.14, col, lw);
    draw_line_aa(*cv, x + s * 0.15, y + s * 0.25, x + s * 0.15, y + s * 0.75, col, lw);
    draw_line_aa(*cv, x + s * 0.85, y + s * 0.25, x + s * 0.85, y + s * 0.75, col, lw);
    stroke_ellipse_aa(*cv, x + s * 0.5, y + s * 0.5, s * 0.35, s * 0.14, col, lw);
    stroke_ellipse_aa(*cv, x + s * 0.5, y + s * 0.75, s * 0.35, s * 0.14, col, lw);
  } else if (name == "table") {
    stroke_rect_aa(*cv, x + s * 0.15, y + s * 0.15, s * 0.7, s * 0.7, col, lw);
    draw_line_aa(*cv, x + s * 0.15, y + s * 0.38, x + s * 0.85, y + s * 0.38, col, lw);
    draw_line_aa(*cv, x + s * 0.48, y + s * 0.38, x + s * 0.48, y + s * 0.85, col, lw);
    draw_line_aa(*cv, x + s * 0.15, y + s * 0.62, x + s * 0.85, y + s * 0.62, col, lw);
  } else if (name == "view") {
    draw_line_aa(*cv, x + s * 0.15, y + s * 0.5, x + s * 0.5, y + s * 0.25, col, lw);
    draw_line_aa(*cv, x + s * 0.5, y + s * 0.25, x + s * 0.85, y + s * 0.5, col, lw);
    draw_line_aa(*cv, x + s * 0.85, y + s * 0.5, x + s * 0.5, y + s * 0.75, col, lw);
    draw_line_aa(*cv, x + s * 0.5, y + s * 0.75, x + s * 0.15, y + s * 0.5, col, lw);
    fill_circle_aa(*cv, x + s * 0.5, y + s * 0.5, s * 0.14, col);
  } else if (name == "key" || name == "pk") {
    stroke_circle_aa(*cv, x + s * 0.35, y + s * 0.38, s * 0.2, col, lw);
    draw_line_aa(*cv, x + s * 0.48, y + s * 0.5, x + s * 0.82, y + s * 0.82, col, lw);
    draw_line_aa(*cv, x + s * 0.7, y + s * 0.7, x + s * 0.8, y + s * 0.6, col, lw);
    draw_line_aa(*cv, x + s * 0.58, y + s * 0.58, x + s * 0.68, y + s * 0.48, col, lw);
  } else if (name == "query" || name == "sql") {
    stroke_rect_aa(*cv, x + s * 0.2, y + s * 0.15, s * 0.6, s * 0.7, col, lw);
    draw_line_aa(*cv, x + s * 0.32, y + s * 0.35, x + s * 0.68, y + s * 0.35, col, lw);
    draw_line_aa(*cv, x + s * 0.32, y + s * 0.5, x + s * 0.68, y + s * 0.5, col, lw);
    draw_line_aa(*cv, x + s * 0.32, y + s * 0.65, x + s * 0.52, y + s * 0.65, col, lw);
  } else if (name == "server" || name == "connect") {
    stroke_round_rect_aa(*cv, x + s * 0.15, y + s * 0.2, s * 0.7, s * 0.25, 2.0, col, lw);
    stroke_round_rect_aa(*cv, x + s * 0.15, y + s * 0.55, s * 0.7, s * 0.25, 2.0, col, lw);
    fill_circle_aa(*cv, x + s * 0.75, y + s * 0.32, s * 0.05, 0xFF22C55E);
    fill_circle_aa(*cv, x + s * 0.75, y + s * 0.67, s * 0.05, 0xFF22C55E);
  } else if (name == "refresh") {
    stroke_circle_aa(*cv, x + s * 0.5, y + s * 0.5, s * 0.28, col, lw);
    draw_line_aa(*cv, x + s * 0.65, y + s * 0.22, x + s * 0.8, y + s * 0.32, col, lw);
    draw_line_aa(*cv, x + s * 0.75, y + s * 0.15, x + s * 0.8, y + s * 0.32, col, lw);
  } else if (name == "filter") {
    std::vector<GrainCanvas::Point> funnel = {
      {x + s * 0.18, y + s * 0.25}, {x + s * 0.82, y + s * 0.25},
      {x + s * 0.56, y + s * 0.55}, {x + s * 0.56, y + s * 0.82},
      {x + s * 0.44, y + s * 0.76}, {x + s * 0.44, y + s * 0.55}
    };
    fill_polygon_aa(*cv, funnel, col);
  } else if (name == "export") {
    draw_line_aa(*cv, x + s * 0.2, y + s * 0.6, x + s * 0.2, y + s * 0.8, col, lw);
    draw_line_aa(*cv, x + s * 0.2, y + s * 0.8, x + s * 0.8, y + s * 0.8, col, lw);
    draw_line_aa(*cv, x + s * 0.8, y + s * 0.8, x + s * 0.8, y + s * 0.6, col, lw);
    draw_line_aa(*cv, x + s * 0.5, y + s * 0.65, x + s * 0.5, y + s * 0.2, col, lw);
    draw_line_aa(*cv, x + s * 0.32, y + s * 0.35, x + s * 0.5, y + s * 0.2, col, lw);
    draw_line_aa(*cv, x + s * 0.68, y + s * 0.35, x + s * 0.5, y + s * 0.2, col, lw);
  } else if (name == "import") {
    draw_line_aa(*cv, x + s * 0.2, y + s * 0.6, x + s * 0.2, y + s * 0.8, col, lw);
    draw_line_aa(*cv, x + s * 0.2, y + s * 0.8, x + s * 0.8, y + s * 0.8, col, lw);
    draw_line_aa(*cv, x + s * 0.8, y + s * 0.8, x + s * 0.8, y + s * 0.6, col, lw);
    draw_line_aa(*cv, x + s * 0.5, y + s * 0.2, x + s * 0.5, y + s * 0.65, col, lw);
    draw_line_aa(*cv, x + s * 0.32, y + s * 0.5, x + s * 0.5, y + s * 0.65, col, lw);
    draw_line_aa(*cv, x + s * 0.68, y + s * 0.5, x + s * 0.5, y + s * 0.65, col, lw);
  } else if (name == "function" || name == "routine" || name == "fx") {
    // Clean vector fx icon
    draw_line_aa(*cv, x + s * 0.42, y + s * 0.2, x + s * 0.32, y + s * 0.8, col, lw);
    draw_line_aa(*cv, x + s * 0.22, y + s * 0.45, x + s * 0.52, y + s * 0.45, col, lw);
    draw_line_aa(*cv, x + s * 0.58, y + s * 0.45, x + s * 0.78, y + s * 0.75, col, lw);
    draw_line_aa(*cv, x + s * 0.78, y + s * 0.45, x + s * 0.58, y + s * 0.75, col, lw);
  } else if (name == "folder") {
    fill_round_rect_aa(*cv, x + s * 0.15, y + s * 0.2, s * 0.35, s * 0.2, 2.0, col);
    fill_round_rect_aa(*cv, x + s * 0.15, y + s * 0.3, s * 0.7, s * 0.5, 2.0, col);
  } else if (name == "model" || name == "er") {
    stroke_rect_aa(*cv, x + s * 0.15, y + s * 0.18, s * 0.32, s * 0.32, col, lw);
    stroke_rect_aa(*cv, x + s * 0.53, y + s * 0.5, s * 0.32, s * 0.32, col, lw);
    draw_line_aa(*cv, x + s * 0.47, y + s * 0.34, x + s * 0.53, y + s * 0.66, col, lw);
  } else if (name == "terminal" || name == "console") {
    stroke_rect_aa(*cv, x + s * 0.15, y + s * 0.15, s * 0.7, s * 0.7, col, lw);
    draw_line_aa(*cv, x + s * 0.28, y + s * 0.35, x + s * 0.45, y + s * 0.5, col, lw);
    draw_line_aa(*cv, x + s * 0.45, y + s * 0.5, x + s * 0.28, y + s * 0.65, col, lw);
    draw_line_aa(*cv, x + s * 0.52, y + s * 0.65, x + s * 0.72, y + s * 0.65, col, lw);
  } else {
    // Default bullet icon
    fill_circle_aa(*cv, x + s * 0.5, y + s * 0.5, s * 0.3, col);
  }
  return args[0];
}

// ---------------------------------------------------------------------------
// Full Navicat-Class Application UI Widgets & SQL Database Tooling
// ---------------------------------------------------------------------------

static void draw_truncated_text_aa(GrainCanvas& cv, const std::string& text, double x, double y, double max_w, uint32_t col, const std::string& font, int sz) {
  if (max_w <= 8.0) return;
  std::string display = text;
  auto m = measure_text_metrics(display, font, sz);
  int w = (int)m->find("width")->as_int();
  if (w > max_w) {
    auto m_dot = measure_text_metrics("...", font, sz);
    int dot_w = (int)m_dot->find("width")->as_int();
    if (max_w <= dot_w + 4) return;
    while (!display.empty()) {
      display.pop_back();
      m = measure_text_metrics(display, font, sz);
      w = (int)m->find("width")->as_int();
      if (w + dot_w <= max_w) {
        display += "...";
        break;
      }
    }
  }
  draw_text_aa(cv, display, x, y, col, font, sz);
}

struct SqlToken {
  enum class Kind { Keyword, Function, String, Number, Comment, Operator, Identifier, Whitespace };
  Kind kind;
  std::string text;
};

static std::vector<SqlToken> tokenize_sql_string(const std::string& sql) {
  static const std::unordered_set<std::string> kw = {
    "SELECT", "FROM", "WHERE", "JOIN", "LEFT", "RIGHT", "INNER", "OUTER", "CROSS", "ON",
    "GROUP", "BY", "ORDER", "HAVING", "LIMIT", "OFFSET", "INSERT", "INTO", "VALUES",
    "UPDATE", "SET", "DELETE", "CREATE", "TABLE", "ALTER", "DROP", "INDEX", "PRIMARY",
    "KEY", "FOREIGN", "REFERENCES", "VIEW", "FUNCTION", "PROCEDURE", "TRIGGER", "DATABASE",
    "SCHEMA", "USE", "SHOW", "DESCRIBE", "EXPLAIN", "AND", "OR", "NOT", "IN", "EXISTS",
    "AS", "DISTINCT", "UNION", "ALL", "CASE", "WHEN", "THEN", "ELSE", "END", "NULL",
    "TRUE", "FALSE", "IS", "LIKE", "BETWEEN", "ASC", "DESC", "WITH", "DEFAULT", "UNIQUE",
    "AUTO_INCREMENT", "CASCADE", "CONSTRAINT"
  };
  static const std::unordered_set<std::string> fn = {
    "COUNT", "SUM", "AVG", "MIN", "MAX", "NOW", "CURDATE", "CURTIME", "DATE", "DATEDIFF",
    "DATE_ADD", "DATE_SUB", "CONCAT", "COALESCE", "IFNULL", "ROUND", "ABS", "CEIL", "FLOOR",
    "LENGTH", "SUBSTR", "SUBSTRING", "UPPER", "LOWER", "TRIM", "REPLACE", "CAST", "CONVERT",
    "JSON_EXTRACT", "MD5", "SHA1", "SHA2", "UUID"
  };

  std::vector<SqlToken> out;
  size_t i = 0;
  while (i < sql.size()) {
    char c = sql[i];
    if (std::isspace((unsigned char)c)) {
      size_t start = i;
      while (i < sql.size() && std::isspace((unsigned char)sql[i])) ++i;
      out.push_back({SqlToken::Kind::Whitespace, sql.substr(start, i - start)});
    } else if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
      size_t start = i;
      while (i < sql.size() && sql[i] != '\n') ++i;
      out.push_back({SqlToken::Kind::Comment, sql.substr(start, i - start)});
    } else if (c == '#') {
      size_t start = i;
      while (i < sql.size() && sql[i] != '\n') ++i;
      out.push_back({SqlToken::Kind::Comment, sql.substr(start, i - start)});
    } else if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
      size_t start = i;
      i += 2;
      while (i + 1 < sql.size() && !(sql[i] == '*' && sql[i + 1] == '/')) ++i;
      if (i + 1 < sql.size()) i += 2;
      out.push_back({SqlToken::Kind::Comment, sql.substr(start, i - start)});
    } else if (c == '\'' || c == '"' || c == '`') {
      char quote = c;
      size_t start = i++;
      while (i < sql.size() && sql[i] != quote) {
        if (sql[i] == '\\' && i + 1 < sql.size()) i += 2;
        else ++i;
      }
      if (i < sql.size()) ++i;
      out.push_back({c == '`' ? SqlToken::Kind::Identifier : SqlToken::Kind::String, sql.substr(start, i - start)});
    } else if (std::isdigit((unsigned char)c) || (c == '.' && i + 1 < sql.size() && std::isdigit((unsigned char)sql[i + 1]))) {
      size_t start = i;
      while (i < sql.size() && (std::isdigit((unsigned char)sql[i]) || sql[i] == '.' || sql[i] == 'x' || sql[i] == 'X' || (sql[i] >= 'a' && sql[i] <= 'f') || (sql[i] >= 'A' && sql[i] <= 'F'))) ++i;
      out.push_back({SqlToken::Kind::Number, sql.substr(start, i - start)});
    } else if (std::isalpha((unsigned char)c) || c == '_') {
      size_t start = i;
      while (i < sql.size() && (std::isalnum((unsigned char)sql[i]) || sql[i] == '_')) ++i;
      std::string word = sql.substr(start, i - start);
      std::string upper_word = word;
      for (char& uc : upper_word) uc = std::toupper((unsigned char)uc);
      if (kw.count(upper_word)) {
        out.push_back({SqlToken::Kind::Keyword, word});
      } else if (fn.count(upper_word)) {
        out.push_back({SqlToken::Kind::Function, word});
      } else {
        out.push_back({SqlToken::Kind::Identifier, word});
      }
    } else {
      out.push_back({SqlToken::Kind::Operator, std::string(1, c)});
      ++i;
    }
  }
  return out;
}


// draw_conflict(cv, x, y, w, h, conflict, [hover_source]): one conflicted field of a Thing (an entry of
// thing_conflicts) as its sides next to each other - each with its source, its value and a toggle to take it. The
// side taken is marked, the others dimmed; with none taken every side is outlined as in conflict. Returns the toggles'
// areas, [{source, x, y, w, h}], for the pane's click handler to match.
Value bi_draw_conflict(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "draw_conflict");
  if (!cv) return Value::nil();
  if (args.size() < 6 || args[5].type != Type::Map || !args[5].obj) { error = "draw_conflict wants (cv, x, y, w, h, conflict, [hover_source])"; return Value::nil(); }
  const double x = args[1].as_double(), y = args[2].as_double(), w = args[3].as_double(), h = args[4].as_double();
  auto c = std::dynamic_pointer_cast<MapObj>(args[5].obj);
  const std::string hover = args.size() > 6 && args[6].type == Type::Str ? args[6].str() : "";
  auto text_of = [&](const char* k) { Value* v = c->find(k); return v && v->type != Type::Nil ? v->to_text() : std::string(); };
  const std::string key = text_of("key"), field = text_of("field"), taken = text_of("taken");
  std::vector<Value> sides;
  if (Value* v = c->find("sides")) if (v->type == Type::List) sides = v->list_items();
  const uint32_t panel = 0xFF0F172A, edge = 0xFF334155, amber = 0xFFF59E0B, green = 0xFF22C55E, ink = 0xFFE2E8F0, dim = 0xFF64748B;
  fill_round_rect_aa(*cv, x, y, w, h, 10.0, panel);
  stroke_round_rect_aa(*cv, x, y, w, h, 10.0, taken.empty() ? amber : edge, 1.5);
  // header: which Thing, which field, and where it stands
  draw_truncated_text_aa(*cv, key + "  \xC2\xB7  " + field, x + 16, y + 12, w - 180, ink, "ui_bold", 15);
  const std::string badge = taken.empty() ? "in conflict" : "took " + taken + (text_of("by") == "rule" ? " by rule" : "");
  auto bm = measure_text_metrics(badge, "ui_bold", 12);
  const double bw = (double)bm->find("width")->as_int() + 20;
  fill_round_rect_aa(*cv, x + w - bw - 14, y + 10, bw, 22, 11.0, taken.empty() ? 0xFF78350F : 0xFF14532D);
  draw_text_aa(*cv, badge, x + w - bw - 4, y + 14, taken.empty() ? amber : green, "ui_bold", 12);
  // the sides, side by side
  std::vector<Value> hits;
  const size_t n = std::max<size_t>(1, sides.size());
  const double gap = 12, top = y + 44, sh = h - 56, sw = (w - 24 - gap * (double)(n - 1)) / (double)n;
  for (size_t i = 0; i < sides.size(); ++i) {
    if (sides[i].type != Type::Map || !sides[i].obj) continue;
    auto sd = std::dynamic_pointer_cast<MapObj>(sides[i].obj);
    Value* sv = sd->find("source");
    Value* vv = sd->find("value");
    const std::string src = sv ? sv->to_text() : "?";
    const std::string val = vv && vv->type != Type::Nil ? vv->to_text() : "NULL";
    const bool is_taken = !taken.empty() && taken == src, other = !taken.empty() && !is_taken;
    const double sx = x + 12 + (sw + gap) * (double)i;
    fill_round_rect_aa(*cv, sx, top, sw, sh, 8.0, 0xFF1E293B);
    stroke_round_rect_aa(*cv, sx, top, sw, sh, 8.0, is_taken ? green : other ? edge : amber, is_taken ? 2.0 : 1.0);
    draw_truncated_text_aa(*cv, src, sx + 12, top + 10, sw - 24, other ? dim : 0xFF94A3B8, "ui_bold", 12);
    draw_truncated_text_aa(*cv, val, sx + 12, top + 32, sw - 24, other ? dim : ink, "ui", 18);
    // the toggle
    const double bx = sx + 12, by = top + sh - 42, bwid = sw - 24, bh = 30;
    const bool hot = hover == src;
    const uint32_t bg = is_taken ? 0xFF16A34A : hot ? 0xFF3B82F6 : 0xFF2563EB;
    fill_round_rect_aa(*cv, bx, by, bwid, bh, 6.0, other ? 0xFF334155 : bg);
    const std::string label = is_taken ? "Taken" : "Take " + src;
    auto lm = measure_text_metrics(label, "ui_bold", 13);
    draw_text_aa(*cv, label, bx + (bwid - (double)lm->find("width")->as_int()) * 0.5, by + (bh - (double)lm->find("height")->as_int()) * 0.5,
                 0xFFFFFFFF, "ui_bold", 13);
    auto hit = std::make_shared<MapObj>();
    hit->put("source", Value::string(src));
    hit->put("x", Value::real(bx));
    hit->put("y", Value::real(by));
    hit->put("w", Value::real(bwid));
    hit->put("h", Value::real(bh));
    hits.push_back(Value::object(Type::Map, hit));
  }
  return Value::list(std::move(hits));
}

Value bi_draw_window_frame(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "draw_window_frame");
  if (!cv) return Value::nil();
  if (args.size() < 6) { error = "draw_window_frame wants (cv, x, y, w, h, title, [subtitle, is_dark])"; return Value::nil(); }
  double x = args[1].as_double(), y = args[2].as_double(), w = args[3].as_double(), h = args[4].as_double();
  std::string title = args[5].to_text();
  std::string subtitle = args.size() > 6 && args[6].type == Type::Str ? args[6].str() : "";
  bool is_dark = args.size() > 7 ? args[7].truthy() : true;

  uint32_t bg_main = is_dark ? 0xFF0F172A : 0xFFF8FAFC;
  uint32_t title_bg = is_dark ? 0xFF1E293B : 0xFFE2E8F0;
  uint32_t border_col = is_dark ? 0xFF334155 : 0xFFCBD5E1;
  uint32_t title_fg = is_dark ? 0xFFF8FAFC : 0xFF0F172A;
  uint32_t sub_fg = is_dark ? 0xFF94A3B8 : 0xFF64748B;

  // Outer drop shadow
  for (int b = 8; b >= 1; b -= 2) {
    double expand = b * 0.6;
    fill_round_rect_aa(*cv, x - expand, y + 4.0 - expand, w + expand * 2.0, h + expand * 2.0, 10.0 + expand, 0x14000000);
  }

  // Main window body
  fill_round_rect_aa(*cv, x, y, w, h, 10.0, bg_main);
  stroke_round_rect_aa(*cv, x, y, w, h, 10.0, border_col, 1.0);

  // Title bar
  double th = 32.0;
  fill_round_rect_aa(*cv, x, y, w, th + 4.0, 10.0, title_bg);
  fill_rect_aa(*cv, x, y + th - 4.0, w, 8.0, title_bg);
  draw_line_aa(*cv, x, y + th, x + w, y + th, border_col, 1.0);

  // Traffic lights
  fill_circle_aa(*cv, x + 16.0, y + 16.0, 5.0, 0xFFEF4444); // Red
  fill_circle_aa(*cv, x + 32.0, y + 16.0, 5.0, 0xFFF59E0B); // Yellow
  fill_circle_aa(*cv, x + 48.0, y + 16.0, 5.0, 0xFF10B981); // Green

  // Title & Subtitle
  double tx = x + 68.0;
  draw_text_aa(*cv, title, tx, y + 8.0, title_fg, "ui_bold", 13);
  if (!subtitle.empty()) {
    auto m = measure_text_metrics(title, "ui_bold", 13);
    int tw = (int)m->find("width")->as_int();
    draw_text_aa(*cv, "— " + subtitle, tx + tw + 8.0, y + 9.0, sub_fg, "ui", 12);
  }
  return args[0];
}

Value bi_draw_menu_bar(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "draw_menu_bar");
  if (!cv) return Value::nil();
  if (args.size() < 5) { error = "draw_menu_bar wants (cv, x, y, w, items, [active_idx, is_dark])"; return Value::nil(); }
  double x = args[1].as_double(), y = args[2].as_double(), w = args[3].as_double();
  if (!want_list(args[4], error, "draw_menu_bar")) return Value::nil();
  const auto& items = args[4].list_items();
  int64_t active_idx = args.size() > 5 ? args[5].as_int() : -1;
  bool is_dark = args.size() > 6 ? args[6].truthy() : true;

  double h = 24.0;
  uint32_t bg = is_dark ? 0xFF1E293B : 0xFFE2E8F0;
  uint32_t border_col = is_dark ? 0xFF334155 : 0xFFCBD5E1;
  uint32_t fg = is_dark ? 0xFFCBD5E1 : 0xFF334155;
  uint32_t active_bg = is_dark ? 0xFF334155 : 0xFFCBD5E1;
  uint32_t active_fg = is_dark ? 0xFFFFFFFF : 0xFF0F172A;

  fill_rect_aa(*cv, x, y, w, h, bg);
  draw_line_aa(*cv, x, y + h, x + w, y + h, border_col, 1.0);

  double cur_x = x + 10.0;
  for (size_t i = 0; i < items.size(); ++i) {
    std::string text = items[i].to_text();
    auto m = measure_text_metrics(text, "ui", 12);
    int tw = (int)m->find("width")->as_int();
    double item_w = tw + 18.0;

    if ((int64_t)i == active_idx) {
      fill_round_rect_aa(*cv, cur_x, y + 2.0, item_w, h - 4.0, 4.0, active_bg);
      draw_text_aa(*cv, text, cur_x + 9.0, y + 4.0, active_fg, "ui_bold", 12);
    } else {
      draw_text_aa(*cv, text, cur_x + 9.0, y + 4.0, fg, "ui", 12);
    }
    cur_x += item_w + 6.0;
  }
  return args[0];
}

Value bi_draw_toolbar(Vm& vm, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "draw_toolbar");
  if (!cv) return Value::nil();
  if (args.size() < 5) { error = "draw_toolbar wants (cv, x, y, w, tools, [active_id, is_dark])"; return Value::nil(); }
  double x = args[1].as_double(), y = args[2].as_double(), w = args[3].as_double();
  if (!want_list(args[4], error, "draw_toolbar")) return Value::nil();
  const auto& tools = args[4].list_items();
  std::string active_id = args.size() > 5 && args[5].type == Type::Str ? args[5].str() : "";
  bool is_dark = args.size() > 6 ? args[6].truthy() : true;

  double h = 42.0;
  uint32_t bg = is_dark ? 0xFF1E293B : 0xFFF1F5F9;
  uint32_t border_col = is_dark ? 0xFF334155 : 0xFFCBD5E1;
  uint32_t fg = is_dark ? 0xFF94A3B8 : 0xFF475569;
  uint32_t active_bg = is_dark ? 0xFF2563EB : 0xFF3B82F6;
  uint32_t active_fg = 0xFFFFFFFF;

  fill_rect_aa(*cv, x, y, w, h, bg);
  draw_line_aa(*cv, x, y + h, x + w, y + h, border_col, 1.0);

  double cur_x = x + 8.0;
  for (size_t i = 0; i < tools.size(); ++i) {
    std::string id, label, icon_name;
    bool is_separator = false;
    if (tools[i].type == Type::Str) {
      if (tools[i].str() == "|") is_separator = true;
      else { id = tools[i].str(); label = tools[i].str(); icon_name = tools[i].str(); }
    } else if (tools[i].type == Type::Map) {
      auto* m = static_cast<MapObj*>(tools[i].obj.get());
      if (auto* v = m->find("id")) id = v->to_text();
      if (auto* v = m->find("label")) label = v->to_text();
      if (auto* v = m->find("icon")) icon_name = v->to_text();
      if (id == "|") is_separator = true;
    }

    if (is_separator) {
      draw_line_aa(*cv, cur_x + 4.0, y + 8.0, cur_x + 4.0, y + h - 8.0, border_col, 1.0);
      cur_x += 10.0;
      continue;
    }

    auto m = measure_text_metrics(label, "ui", 12);
    int tw = (int)m->find("width")->as_int();
    double btn_w = icon_name.empty() ? (tw + 18.0) : (tw + 34.0);
    bool is_active = (!active_id.empty() && id == active_id);

    if (is_active) {
      fill_round_rect_aa(*cv, cur_x, y + 4.0, btn_w, h - 8.0, 5.0, active_bg);
    }

    uint32_t icon_col = is_active ? active_fg : (is_dark ? 0xFF38BDF8 : 0xFF0284C7);
    uint32_t text_col = is_active ? active_fg : fg;

    if (!icon_name.empty()) {
      std::vector<Value> icon_args = {args[0], Value::real(cur_x + 6.0), Value::real(y + 13.0), Value::real(16.0), Value::string(icon_name), Value::integer((int64_t)icon_col)};
      bi_draw_icon(vm, icon_args, error);
      draw_text_aa(*cv, label, cur_x + 26.0, y + 13.0, text_col, is_active ? "ui_bold" : "ui", 12);
    } else {
      draw_text_aa(*cv, label, cur_x + 9.0, y + 13.0, text_col, is_active ? "ui_bold" : "ui", 12);
    }

    cur_x += btn_w + 6.0;
  }
  return args[0];
}

Value bi_draw_nav_tree(Vm& vm, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "draw_nav_tree");
  if (!cv) return Value::nil();
  if (args.size() < 6) { error = "draw_nav_tree wants (cv, x, y, w, h, nodes, [selected_id, scroll_y, is_dark])"; return Value::nil(); }
  double x = args[1].as_double(), y = args[2].as_double(), w = args[3].as_double(), h = args[4].as_double();
  if (!want_list(args[5], error, "draw_nav_tree")) return Value::nil();
  const auto& nodes = args[5].list_items();
  std::string sel_id = args.size() > 6 && args[6].type == Type::Str ? args[6].str() : "";
  double scroll_y = args.size() > 7 ? args[7].as_double() : 0.0;
  bool is_dark = args.size() > 8 ? args[8].truthy() : true;

  uint32_t bg = is_dark ? 0xFF0F172A : 0xFFF8FAFC;
  uint32_t border_col = is_dark ? 0xFF334155 : 0xFFCBD5E1;
  uint32_t search_bg = is_dark ? 0xFF1E293B : 0xFFFFFFFF;
  uint32_t search_fg = is_dark ? 0xFF64748B : 0xFF94A3B8;
  uint32_t fg = is_dark ? 0xFFCBD5E1 : 0xFF334155;
  uint32_t sel_bg = is_dark ? 0xFF1E3A8A : 0xFFDBEAFE;
  uint32_t sel_fg = is_dark ? 0xFFFFFFFF : 0xFF1E40AF;

  // Tree panel background and right border
  fill_rect_aa(*cv, x, y, w, h, bg);
  draw_line_aa(*cv, x + w, y, x + w, y + h, border_col, 1.0);

  // Search box at top
  double search_h = 28.0;
  fill_round_rect_aa(*cv, x + 8.0, y + 8.0, w - 16.0, search_h, 5.0, search_bg);
  stroke_round_rect_aa(*cv, x + 8.0, y + 8.0, w - 16.0, search_h, 5.0, border_col, 1.0);
  std::vector<Value> s_icon = {args[0], Value::real(x + 14.0), Value::real(y + 14.0), Value::real(14.0), Value::string("search"), Value::integer((int64_t)search_fg)};
  bi_draw_icon(vm, s_icon, error);
  draw_text_aa(*cv, "Search objects...", x + 34.0, y + 14.0, search_fg, "ui", 12);

  double node_y = y + 8.0 + search_h + 8.0 - scroll_y;
  double row_h = 24.0;

  for (size_t i = 0; i < nodes.size(); ++i) {
    if (node_y + row_h > y + h) break;
    if (node_y + row_h >= y + search_h + 16.0) {
      std::string id, label, type = "table", badge;
      int level = 0;
      bool expanded = false;

      if (nodes[i].type == Type::Map) {
        auto* m = static_cast<MapObj*>(nodes[i].obj.get());
        if (auto* v = m->find("id")) id = v->to_text();
        if (auto* v = m->find("label")) label = v->to_text();
        if (auto* v = m->find("type")) type = v->to_text();
        if (auto* v = m->find("level")) level = (int)v->as_int();
        if (auto* v = m->find("expanded")) expanded = v->truthy();
        if (auto* v = m->find("badge")) badge = v->to_text();
      } else {
        label = nodes[i].to_text();
        id = label;
      }

      bool is_selected = (!sel_id.empty() && id == sel_id);
      if (is_selected) {
        fill_round_rect_aa(*cv, x + 4.0, node_y, w - 8.0, row_h, 4.0, sel_bg);
      }

      double indent_x = x + 10.0 + level * 16.0;

      // Chevron for expandable types
      if (type == "server" || type == "connection" || type == "database" || type == "folder") {
        std::vector<Value> chev_args = {args[0], Value::real(indent_x), Value::real(node_y + 5.0), Value::real(14.0),
                                        Value::string(expanded ? "chevron_down" : "chevron_right"), Value::integer((int64_t)(is_dark ? 0xFF94A3B8 : 0xFF64748B))};
        bi_draw_icon(vm, chev_args, error);
      }
      indent_x += 16.0;

      // Node Icon
      uint32_t icon_col = 0xFF60A5FA;
      std::string icon_name = "table";
      if (type == "server" || type == "connection") { icon_col = 0xFF10B981; icon_name = "server"; }
      else if (type == "database") { icon_col = 0xFF38BDF8; icon_name = "database"; }
      else if (type == "table") { icon_col = 0xFF60A5FA; icon_name = "table"; }
      else if (type == "view") { icon_col = 0xFFA855F7; icon_name = "view"; }
      else if (type == "function" || type == "routine" || type == "fx") { icon_col = 0xFFF59E0B; icon_name = "function"; }
      else if (type == "key" || type == "index") { icon_col = 0xFFEAB308; icon_name = "key"; }
      else if (type == "query") { icon_col = 0xFF34D399; icon_name = "query"; }
      else if (type == "folder") { icon_col = 0xFFFCD34D; icon_name = "folder"; }

      std::vector<Value> ni_args = {args[0], Value::real(indent_x), Value::real(node_y + 4.0), Value::real(16.0), Value::string(icon_name), Value::integer((int64_t)icon_col)};
      bi_draw_icon(vm, ni_args, error);
      indent_x += 20.0;

      // Label & Badge with proper bounding box calculation
      double badge_space = 0.0;
      int bw = 0;
      if (!badge.empty()) {
        auto m_b = measure_text_metrics(badge, "ui", 10);
        bw = (int)m_b->find("width")->as_int() + 12;
        badge_space = bw + 12.0;
      }

      double max_text_w = w - (indent_x - x) - badge_space - 8.0;
      uint32_t row_text_col = is_selected ? sel_fg : fg;
      draw_truncated_text_aa(*cv, label, indent_x, node_y + 4.0, max_text_w, row_text_col, is_selected ? "ui_bold" : "ui", 12);

      // Badge
      if (!badge.empty()) {
        double bx = x + w - bw - 8.0;
        fill_round_rect_aa(*cv, bx, node_y + 3.0, bw, 16.0, 4.0, is_dark ? 0xFF1E293B : 0xFFE2E8F0);
        draw_text_aa(*cv, badge, bx + 6.0, node_y + 4.0, is_dark ? 0xFF94A3B8 : 0xFF64748B, "ui", 10);
      }
    }
    node_y += row_h;
  }
  return args[0];
}

Value bi_draw_tab_bar(Vm& vm, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "draw_tab_bar");
  if (!cv) return Value::nil();
  if (args.size() < 5) { error = "draw_tab_bar wants (cv, x, y, w, tabs, [active_idx, is_dark])"; return Value::nil(); }
  double x = args[1].as_double(), y = args[2].as_double(), w = args[3].as_double();
  if (!want_list(args[4], error, "draw_tab_bar")) return Value::nil();
  const auto& tabs = args[4].list_items();
  int64_t active_idx = args.size() > 5 ? args[5].as_int() : 0;
  bool is_dark = args.size() > 6 ? args[6].truthy() : true;

  double h = 30.0;
  uint32_t bg = is_dark ? 0xFF0B1120 : 0xFFE2E8F0;
  uint32_t tab_inactive_bg = is_dark ? 0xFF0F172A : 0xFFCBD5E1;
  uint32_t tab_active_bg = is_dark ? 0xFF1E293B : 0xFFF8FAFC;
  uint32_t border_col = is_dark ? 0xFF334155 : 0xFFCBD5E1;
  uint32_t accent_col = 0xFF38BDF8;
  uint32_t active_fg = is_dark ? 0xFFFFFFFF : 0xFF0F172A;
  uint32_t inactive_fg = is_dark ? 0xFF94A3B8 : 0xFF64748B;

  fill_rect_aa(*cv, x, y, w, h, bg);
  draw_line_aa(*cv, x, y + h, x + w, y + h, border_col, 1.0);

  double cur_x = x;
  for (size_t i = 0; i < tabs.size(); ++i) {
    std::string title, icon_name = "table";
    bool is_dirty = false;
    if (tabs[i].type == Type::Str) {
      title = tabs[i].str();
    } else if (tabs[i].type == Type::Map) {
      auto* m = static_cast<MapObj*>(tabs[i].obj.get());
      if (auto* v = m->find("title")) title = v->to_text();
      if (auto* v = m->find("icon")) icon_name = v->to_text();
      if (auto* v = m->find("dirty")) is_dirty = v->truthy();
    }

    bool is_active = ((int64_t)i == active_idx);
    auto m = measure_text_metrics(title, is_active ? "ui_bold" : "ui", 12);
    int tw = (int)m->find("width")->as_int();
    double tab_w = tw + 48.0;

    uint32_t t_bg = is_active ? tab_active_bg : tab_inactive_bg;
    fill_rect_aa(*cv, cur_x, y + 2.0, tab_w, h - 2.0, t_bg);
    draw_line_aa(*cv, cur_x + tab_w, y + 2.0, cur_x + tab_w, y + h, border_col, 1.0);

    if (is_active) {
      fill_rect_aa(*cv, cur_x, y, tab_w, 3.0, accent_col);
    }

    // Icon
    std::vector<Value> icon_args = {args[0], Value::real(cur_x + 8.0), Value::real(y + 7.0), Value::real(14.0), Value::string(icon_name),
                                    Value::integer((int64_t)(is_active ? 0xFF38BDF8 : inactive_fg))};
    bi_draw_icon(vm, icon_args, error);

    // Title
    draw_text_aa(*cv, title, cur_x + 26.0, y + 6.0, is_active ? active_fg : inactive_fg, is_active ? "ui_bold" : "ui", 12);

    // Dirty dot or Close button
    if (is_dirty) {
      fill_circle_aa(*cv, cur_x + tab_w - 12.0, y + 15.0, 3.5, 0xFFF59E0B);
    } else {
      std::vector<Value> close_args = {args[0], Value::real(cur_x + tab_w - 18.0), Value::real(y + 9.0), Value::real(10.0), Value::string("close"),
                                       Value::integer((int64_t)(is_dark ? 0xFF64748B : 0xFF94A3B8))};
      bi_draw_icon(vm, close_args, error);
    }

    cur_x += tab_w;
  }
  return args[0];
}

Value bi_draw_sql_editor(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "draw_sql_editor");
  if (!cv) return Value::nil();
  if (args.size() < 6) { error = "draw_sql_editor wants (cv, x, y, w, h, code, [cursor_line, cursor_col, scroll_line, is_dark])"; return Value::nil(); }
  double x = args[1].as_double(), y = args[2].as_double(), w = args[3].as_double(), h = args[4].as_double();
  std::string code = args[5].to_text();
  int64_t cur_line = args.size() > 6 ? args[6].as_int() : 1;
  int64_t cur_col = args.size() > 7 ? args[7].as_int() : 0;
  int64_t scroll_line = args.size() > 8 ? args[8].as_int() : 0;
  bool is_dark = args.size() > 9 ? args[9].truthy() : true;

  uint32_t bg = is_dark ? 0xFF0B0F19 : 0xFFFFFFFF;
  uint32_t gutter_bg = is_dark ? 0xFF070A10 : 0xFFF1F5F9;
  uint32_t gutter_fg = is_dark ? 0xFF475569 : 0xFF94A3B8;
  uint32_t gutter_active_fg = is_dark ? 0xFF38BDF8 : 0xFF0284C7;
  uint32_t border_col = is_dark ? 0xFF334155 : 0xFFCBD5E1;
  uint32_t active_line_bg = is_dark ? 0x2038BDF8 : 0x203B82F6;

  fill_rect_aa(*cv, x, y, w, h, bg);
  stroke_rect_aa(*cv, x, y, w, h, border_col, 1.0);

  double gutter_w = 44.0;
  fill_rect_aa(*cv, x, y, gutter_w, h, gutter_bg);
  draw_line_aa(*cv, x + gutter_w, y, x + gutter_w, y + h, border_col, 1.0);

  // Split code into lines
  std::vector<std::string> lines;
  std::stringstream ss(code);
  std::string l;
  while (std::getline(ss, l)) {
    if (!l.empty() && l.back() == '\r') l.pop_back();
    lines.push_back(l);
  }
  if (lines.empty()) lines.push_back("");

  double line_h = 20.0;
  double line_y = y + 8.0;

  for (size_t i = (size_t)std::max((int64_t)0, scroll_line); i < lines.size(); ++i) {
    if (line_y + line_h > y + h) break;
    int line_num = (int)i + 1;
    bool is_active_line = (line_num == cur_line);

    if (is_active_line) {
      fill_rect_aa(*cv, x + gutter_w + 1.0, line_y - 1.0, w - gutter_w - 2.0, line_h, active_line_bg);
    }

    // Line number in gutter
    std::string ln_str = std::to_string(line_num);
    auto m_ln = measure_text_metrics(ln_str, "ui", 11);
    int ln_w = (int)m_ln->find("width")->as_int();
    draw_text_aa(*cv, ln_str, x + gutter_w - ln_w - 8.0, line_y + 2.0, is_active_line ? gutter_active_fg : gutter_fg, is_active_line ? "ui_bold" : "ui", 11);

    // Lex and syntax highlight line tokens using MONOSPACE font
    auto tokens = tokenize_sql_string(lines[i]);
    double token_x = x + gutter_w + 12.0;

    for (const auto& tok : tokens) {
      if (tok.kind == SqlToken::Kind::Whitespace) {
        auto m_sp = measure_text_metrics(tok.text, "mono", 13);
        token_x += m_sp->find("width")->as_double();
        continue;
      }

      uint32_t tok_col = is_dark ? 0xFFF1F5F9 : 0xFF0F172A;
      if (tok.kind == SqlToken::Kind::Keyword) {
        tok_col = is_dark ? 0xFF38BDF8 : 0xFF0284C7;
      } else if (tok.kind == SqlToken::Kind::Function) {
        tok_col = 0xFF34D399; // Emerald
      } else if (tok.kind == SqlToken::Kind::String) {
        tok_col = 0xFFFBBF24; // Amber
      } else if (tok.kind == SqlToken::Kind::Number) {
        tok_col = 0xFFFB923C; // Orange
      } else if (tok.kind == SqlToken::Kind::Comment) {
        tok_col = 0xFF64748B; // Slate gray
      } else if (tok.kind == SqlToken::Kind::Operator) {
        tok_col = 0xFF94A3B8; // Muted sky
      }

      auto m = measure_text_metrics(tok.text, "mono", 13);
      int tw = (int)m->find("width")->as_int();
      if (token_x + tw < x + w - 8.0) {
        draw_text_aa(*cv, tok.text, token_x, line_y + 2.0, tok_col, "mono", 13);
      }
      token_x += tw;
    }

    // Cursor bar
    if (is_active_line && cur_col >= 0) {
      std::string prefix = lines[i].substr(0, std::min((size_t)cur_col, lines[i].size()));
      auto m_pre = measure_text_metrics(prefix, "mono", 13);
      double cursor_x = x + gutter_w + 12.0 + m_pre->find("width")->as_double();
      draw_line_aa(*cv, cursor_x, line_y + 1.0, cursor_x, line_y + line_h - 2.0, 0xFF38BDF8, 2.0);
    }

    line_y += line_h;
  }
  return args[0];
}

Value bi_draw_data_grid(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "draw_data_grid");
  if (!cv) return Value::nil();
  if (args.size() < 7) { error = "draw_data_grid wants (cv, x, y, w, h, columns, rows, [selected_row, selected_col, sort_col, sort_asc, scroll_x, scroll_y, page_info, is_dark, dirty_cells, editing_cell])"; return Value::nil(); }
  double x = args[1].as_double(), y = args[2].as_double(), w = args[3].as_double(), h = args[4].as_double();
  if (!want_list(args[5], error, "draw_data_grid (columns)")) return Value::nil();
  if (!want_list(args[6], error, "draw_data_grid (rows)")) return Value::nil();
  const auto& cols = args[5].list_items();
  const auto& rows = args[6].list_items();

  int64_t sel_row = args.size() > 7 ? args[7].as_int() : -1;
  int64_t sel_col = args.size() > 8 ? args[8].as_int() : -1;
  std::string sort_col = args.size() > 9 && args[9].type == Type::Str ? args[9].str() : "";
  bool sort_asc = args.size() > 10 ? args[10].truthy() : true;
  double scroll_x = args.size() > 11 ? args[11].as_double() : 0.0;
  double scroll_y = args.size() > 12 ? args[12].as_double() : 0.0;
  std::string page_info = args.size() > 13 && args[13].type == Type::Str ? args[13].str() : "";
  bool is_dark = args.size() > 14 ? args[14].truthy() : true;

  MapObj* dirty_map = (args.size() > 15 && args[15].type == Type::Map) ? static_cast<MapObj*>(args[15].obj.get()) : nullptr;
  MapObj* edit_map = (args.size() > 16 && args[16].type == Type::Map) ? static_cast<MapObj*>(args[16].obj.get()) : nullptr;
  int64_t edit_r = edit_map && edit_map->find("row") ? edit_map->find("row")->as_int() : -1;
  int64_t edit_c = edit_map && edit_map->find("col") ? edit_map->find("col")->as_int() : -1;
  std::string edit_val = edit_map && edit_map->find("value") ? edit_map->find("value")->to_text() : "";
  int64_t edit_cur = edit_map && edit_map->find("cursor") ? edit_map->find("cursor")->as_int() : (int64_t)edit_val.size();

  uint32_t bg = is_dark ? 0xFF0F172A : 0xFFFFFFFF;
  uint32_t header_bg = is_dark ? 0xFF1E293B : 0xFFF1F5F9;
  uint32_t header_fg = is_dark ? 0xFFF8FAFC : 0xFF0F172A;
  uint32_t header_type_fg = is_dark ? 0xFF94A3B8 : 0xFF64748B;
  uint32_t border_col = is_dark ? 0xFF334155 : 0xFFCBD5E1;
  uint32_t row_even_bg = is_dark ? 0xFF0F172A : 0xFFFFFFFF;
  uint32_t row_odd_bg = is_dark ? 0xFF1E293B : 0xFFF8FAFC;
  uint32_t row_sel_bg = is_dark ? 0xFF1E3A8A : 0xFFDBEAFE;
  uint32_t fg = is_dark ? 0xFFCBD5E1 : 0xFF334155;

  fill_rect_aa(*cv, x, y, w, h, bg);
  stroke_rect_aa(*cv, x, y, w, h, border_col, 1.0);

  double header_h = 42.0;
  double footer_h = 26.0;
  double row_num_w = 40.0;
  double col_default_w = 120.0;

  // Header background
  fill_rect_aa(*cv, x, y, w, header_h, header_bg);
  draw_line_aa(*cv, x, y + header_h, x + w, y + header_h, border_col, 1.0);

  // Row number header
  draw_text_aa(*cv, "#", x + 14.0, y + 13.0, header_type_fg, "ui_bold", 12);
  draw_line_aa(*cv, x + row_num_w, y, x + row_num_w, y + h - footer_h, border_col, 1.0);

  // Columns header
  double cur_cx = x + row_num_w - scroll_x;
  for (size_t c = 0; c < cols.size(); ++c) {
    std::string col_name, col_type = "VARCHAR";
    double cw = col_default_w;
    if (cols[c].type == Type::Str) {
      col_name = cols[c].str();
    } else if (cols[c].type == Type::Map) {
      auto* m = static_cast<MapObj*>(cols[c].obj.get());
      if (auto* v = m->find("name")) col_name = v->to_text();
      if (auto* v = m->find("type")) col_type = v->to_text();
      if (auto* v = m->find("width")) cw = v->as_double();
    }

    if (cur_cx + cw > x && cur_cx < x + w) {
      bool is_sorted = (!sort_col.empty() && col_name == sort_col);
      double sort_space = is_sorted ? 20.0 : 8.0;

      // Column title + type clearly separated vertically
      draw_truncated_text_aa(*cv, col_name, cur_cx + 8.0, y + 5.0, cw - 16.0 - sort_space, header_fg, "ui_bold", 12);
      draw_truncated_text_aa(*cv, col_type, cur_cx + 8.0, y + 24.0, cw - 16.0 - sort_space, header_type_fg, "ui", 10);

      // Sort indicator
      if (is_sorted) {
        draw_text_aa(*cv, sort_asc ? "^" : "v", cur_cx + cw - 16.0, y + 13.0, 0xFF38BDF8, "ui_bold", 12);
      }
      draw_line_aa(*cv, cur_cx + cw, y, cur_cx + cw, y + h - footer_h, border_col, 1.0);
    }
    cur_cx += cw;
  }

  // Rows rendering
  double row_h = 24.0;
  double cur_ry = y + header_h - scroll_y;

  for (size_t r = 0; r < rows.size(); ++r) {
    if (cur_ry + row_h > y + h - footer_h) break;
    if (cur_ry + row_h >= y + header_h) {
      bool is_sel_r = ((int64_t)r == sel_row);
      uint32_t r_bg = is_sel_r ? row_sel_bg : ((r % 2 == 0) ? row_even_bg : row_odd_bg);
      fill_rect_aa(*cv, x + 1.0, cur_ry, w - 2.0, row_h, r_bg);

      // Row number
      std::string rn_str = std::to_string(r + 1);
      draw_text_aa(*cv, rn_str, x + 8.0, cur_ry + 5.0, header_type_fg, "ui", 11);

      // Cells
      if (rows[r].type == Type::List) {
        const auto& cells = rows[r].list_items();
        double cell_x = x + row_num_w - scroll_x;
        for (size_t c = 0; c < cols.size() && c < cells.size(); ++c) {
          double cw = col_default_w;
          std::string col_name = "col_" + std::to_string(c);
          if (cols[c].type == Type::Str) {
            col_name = cols[c].str();
          } else if (cols[c].type == Type::Map) {
            auto* m = static_cast<MapObj*>(cols[c].obj.get());
            if (auto* v = m->find("name")) col_name = v->to_text();
            if (auto* v = m->find("width")) cw = v->as_double();
          }

          if (cell_x + cw > x && cell_x < x + w) {
            bool is_sel_cell = (is_sel_r && (int64_t)c == sel_col);
            bool is_editing_this = (edit_r == (int64_t)r && edit_c == (int64_t)c);

            // Check dirty status
            bool is_dirty = false;
            if (dirty_map) {
              std::string key1 = std::to_string(r) + ":" + std::to_string(c);
              std::string key2 = std::to_string(r) + ":" + col_name;
              std::string pk_str = cells.empty() ? "" : cells[0].to_text();
              std::string key3 = pk_str + ":" + col_name;
              if (dirty_map->find(key1) || dirty_map->find(key2) || dirty_map->find(key3)) {
                is_dirty = true;
              }
            }

            if (is_editing_this) {
              // Active inline editing text input box
              fill_rect_aa(*cv, cell_x + 1.0, cur_ry + 1.0, cw - 2.0, row_h - 2.0, is_dark ? 0xFF1E293B : 0xFFFFFFFF);
              stroke_rect_aa(*cv, cell_x + 1.0, cur_ry + 1.0, cw - 2.0, row_h - 2.0, 0xFF38BDF8, 2.0);
              draw_truncated_text_aa(*cv, edit_val, cell_x + 8.0, cur_ry + 5.0, cw - 20.0, is_dark ? 0xFFFFFFFF : 0xFF0F172A, "ui", 12);
              auto m_cur = measure_text_metrics(edit_val.substr(0, std::max((int64_t)0, std::min((int64_t)edit_val.size(), edit_cur))), "ui", 12);
              double cur_w = m_cur->find("width")->as_double();
              draw_line_aa(*cv, cell_x + 8.0 + cur_w, cur_ry + 3.0, cell_x + 8.0 + cur_w, cur_ry + row_h - 3.0, 0xFF38BDF8, 1.5);
            } else {
              if (is_dirty) {
                fill_rect_aa(*cv, cell_x + 1.0, cur_ry + 1.0, cw - 2.0, row_h - 2.0, is_dark ? 0x40F59E0B : 0x25F59E0B);
                // Amber corner indicator
                fill_rect_aa(*cv, cell_x + cw - 5.0, cur_ry + 1.0, 4.0, 4.0, 0xFFF59E0B);
              }

              if (is_sel_cell) {
                stroke_rect_aa(*cv, cell_x + 1.0, cur_ry + 1.0, cw - 2.0, row_h - 2.0, 0xFF38BDF8, 1.5);
              }

              if (cells[c].type == Type::Nil) {
                draw_text_aa(*cv, "[NULL]", cell_x + 8.0, cur_ry + 5.0, 0xFF64748B, "ui", 11);
              } else {
                std::string val_str = cells[c].to_text();
                draw_truncated_text_aa(*cv, val_str, cell_x + 8.0, cur_ry + 5.0, cw - 16.0, is_dirty ? 0xFFF59E0B : fg, is_sel_cell ? "ui_bold" : "ui", 12);
              }
            }
          }
          cell_x += cw;
        }
      }
      draw_line_aa(*cv, x, cur_ry + row_h, x + w, cur_ry + row_h, border_col, 1.0);
    }
    cur_ry += row_h;
  }

  // Footer / Pagination Bar
  double footer_y = y + h - footer_h;
  fill_rect_aa(*cv, x, footer_y, w, footer_h, header_bg);
  draw_line_aa(*cv, x, footer_y, x + w, footer_y, border_col, 1.0);

  // Pagination navigation icons
  draw_text_aa(*cv, "|<   <   >   >|", x + 12.0, footer_y + 5.0, header_fg, "ui_bold", 11);

  if (!page_info.empty()) {
    draw_text_aa(*cv, page_info, x + 180.0, footer_y + 5.0, header_type_fg, "ui", 11);
  } else {
    std::string info = "Showing 1 - " + std::to_string(std::min((size_t)50, rows.size())) + " of " + std::to_string(rows.size()) + " rows";
    draw_text_aa(*cv, info, x + 180.0, footer_y + 5.0, header_type_fg, "ui", 11);
  }
  return args[0];
}

Value bi_draw_table_designer(Vm& vm, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "draw_table_designer");
  if (!cv) return Value::nil();
  if (args.size() < 6) { error = "draw_table_designer wants (cv, x, y, w, h, fields, [selected_idx, active_subtab, is_dark])"; return Value::nil(); }
  double x = args[1].as_double(), y = args[2].as_double(), w = args[3].as_double(), h = args[4].as_double();
  if (!want_list(args[5], error, "draw_table_designer")) return Value::nil();
  const auto& fields = args[5].list_items();
  int64_t sel_idx = args.size() > 6 ? args[6].as_int() : 0;
  std::string active_subtab = args.size() > 7 && args[7].type == Type::Str ? args[7].str() : "Fields";
  bool is_dark = args.size() > 8 ? args[8].truthy() : true;

  uint32_t bg = is_dark ? 0xFF0F172A : 0xFFFFFFFF;
  uint32_t border_col = is_dark ? 0xFF334155 : 0xFFCBD5E1;
  uint32_t tab_bg = is_dark ? 0xFF1E293B : 0xFFF1F5F9;
  uint32_t tab_active_bg = is_dark ? 0xFF2563EB : 0xFF3B82F6;

  fill_rect_aa(*cv, x, y, w, h, bg);
  stroke_rect_aa(*cv, x, y, w, h, border_col, 1.0);

  // Subtabs at top
  double tab_h = 28.0;
  std::vector<std::string> subtabs = {"Fields", "Indexes", "Foreign Keys", "Triggers", "Options", "SQL Preview"};
  double cur_tx = x + 8.0;
  for (const auto& st : subtabs) {
    bool is_active = (st == active_subtab);
    auto m = measure_text_metrics(st, is_active ? "ui_bold" : "ui", 11);
    int tw = (int)m->find("width")->as_int();
    double bw = tw + 16.0;
    fill_round_rect_aa(*cv, cur_tx, y + 4.0, bw, tab_h - 6.0, 4.0, is_active ? tab_active_bg : tab_bg);
    draw_text_aa(*cv, st, cur_tx + 8.0, y + 8.0, is_active ? 0xFFFFFFFF : (is_dark ? 0xFF94A3B8 : 0xFF64748B), is_active ? "ui_bold" : "ui", 11);
    cur_tx += bw + 6.0;
  }
  draw_line_aa(*cv, x, y + tab_h, x + w, y + tab_h, border_col, 1.0);

  // Field Designer Headers
  double hdr_y = y + tab_h;
  double hdr_h = 24.0;
  fill_rect_aa(*cv, x, hdr_y, w, hdr_h, is_dark ? 0xFF1E293B : 0xFFE2E8F0);
  draw_line_aa(*cv, x, hdr_y + hdr_h, x + w, hdr_y + hdr_h, border_col, 1.0);

  draw_text_aa(*cv, "#", x + 10.0, hdr_y + 5.0, 0xFF94A3B8, "ui", 11);
  draw_text_aa(*cv, "Key", x + 34.0, hdr_y + 5.0, 0xFF94A3B8, "ui", 11);
  draw_text_aa(*cv, "Field Name", x + 70.0, hdr_y + 5.0, 0xFFF8FAFC, "ui_bold", 11);
  draw_text_aa(*cv, "Type", x + 210.0, hdr_y + 5.0, 0xFFF8FAFC, "ui_bold", 11);
  draw_text_aa(*cv, "Length", x + 310.0, hdr_y + 5.0, 0xFF94A3B8, "ui", 11);
  draw_text_aa(*cv, "Not Null", x + 380.0, hdr_y + 5.0, 0xFF94A3B8, "ui", 11);
  draw_text_aa(*cv, "Auto Inc", x + 450.0, hdr_y + 5.0, 0xFF94A3B8, "ui", 11);
  draw_text_aa(*cv, "Default", x + 530.0, hdr_y + 5.0, 0xFF94A3B8, "ui", 11);
  draw_text_aa(*cv, "Comment", x + 630.0, hdr_y + 5.0, 0xFF94A3B8, "ui", 11);

  // Field rows
  double row_y = hdr_y + hdr_h;
  double row_h = 22.0;

  for (size_t i = 0; i < fields.size(); ++i) {
    if (row_y + row_h > y + h - 34.0) break;
    bool is_sel = ((int64_t)i == sel_idx);
    uint32_t r_bg = is_sel ? (is_dark ? 0xFF1E3A8A : 0xFFDBEAFE) : ((i % 2 == 0) ? (is_dark ? 0xFF0F172A : 0xFFFFFFFF) : (is_dark ? 0xFF1E293B : 0xFFF8FAFC));
    fill_rect_aa(*cv, x + 1.0, row_y, w - 2.0, row_h, r_bg);

    std::string name, type = "VARCHAR", len = "255", def_val, comment;
    bool is_pk = false, not_null = true, auto_inc = false;

    if (fields[i].type == Type::Map) {
      auto* m = static_cast<MapObj*>(fields[i].obj.get());
      if (auto* v = m->find("name")) name = v->to_text();
      if (auto* v = m->find("type")) type = v->to_text();
      if (auto* v = m->find("length")) len = v->to_text();
      if (auto* v = m->find("pk")) is_pk = v->truthy();
      if (auto* v = m->find("not_null")) not_null = v->truthy();
      if (auto* v = m->find("auto_inc")) auto_inc = v->truthy();
      if (auto* v = m->find("default")) def_val = v->to_text();
      if (auto* v = m->find("comment")) comment = v->to_text();
    } else {
      name = fields[i].to_text();
    }

    draw_text_aa(*cv, std::to_string(i + 1), x + 10.0, row_y + 4.0, 0xFF94A3B8, "ui", 11);

    if (is_pk) {
      std::vector<Value> k_args = {args[0], Value::real(x + 36.0), Value::real(row_y + 4.0), Value::real(14.0), Value::string("key"), Value::integer((int64_t)0xFFEAB308)};
      bi_draw_icon(vm, k_args, error);
    }

    draw_text_aa(*cv, name, x + 70.0, row_y + 4.0, is_dark ? 0xFFFFFFFF : 0xFF0F172A, is_sel ? "ui_bold" : "ui", 12);
    draw_text_aa(*cv, type, x + 210.0, row_y + 4.0, 0xFF38BDF8, "ui_bold", 11);
    draw_text_aa(*cv, len, x + 310.0, row_y + 4.0, 0xFFCBD5E1, "ui", 11);
    draw_text_aa(*cv, not_null ? "[x]" : "[ ]", x + 395.0, row_y + 4.0, not_null ? 0xFF22C55E : 0xFF64748B, "ui", 11);
    draw_text_aa(*cv, auto_inc ? "[x]" : "[ ]", x + 465.0, row_y + 4.0, auto_inc ? 0xFF22C55E : 0xFF64748B, "ui", 11);
    draw_text_aa(*cv, def_val, x + 530.0, row_y + 4.0, 0xFF94A3B8, "ui", 11);
    draw_truncated_text_aa(*cv, comment, x + 630.0, row_y + 4.0, w - 640.0, 0xFF64748B, "ui", 11);

    draw_line_aa(*cv, x, row_y + row_h, x + w, row_y + row_h, border_col, 1.0);
    row_y += row_h;
  }

  // Designer Bottom Action Bar
  double bar_y = y + h - 30.0;
  fill_rect_aa(*cv, x, bar_y, w, 30.0, is_dark ? 0xFF1E293B : 0xFFE2E8F0);
  draw_line_aa(*cv, x, bar_y, x + w, bar_y, border_col, 1.0);

  draw_text_aa(*cv, "+ Add Field   - Delete Field   ^ Move Up   v Move Down   |   [Save Table]", x + 12.0, bar_y + 8.0, is_dark ? 0xFF38BDF8 : 0xFF0284C7, "ui_bold", 11);
  return args[0];
}

Value bi_draw_connection_dialog(Vm& vm, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "draw_connection_dialog");
  if (!cv) return Value::nil();
  if (args.size() < 6) { error = "draw_connection_dialog wants (cv, x, y, w, h, conn_info, [active_field_id, is_dark])"; return Value::nil(); }
  double x = args[1].as_double(), y = args[2].as_double(), w = args[3].as_double(), h = args[4].as_double();
  if (args[5].type != Type::Map) { error = "draw_connection_dialog wants a map of conn_info"; return Value::nil(); }
  auto* m = static_cast<MapObj*>(args[5].obj.get());
  std::string active_field = args.size() > 6 && args[6].type == Type::Str ? args[6].str() : "host";
  bool is_dark = args.size() > 7 ? args[7].truthy() : true;

  // What the dialog shows when the caller gives nothing: an empty profile and no claim about a connection. A result
  // ("status", "test_ok") is only ever shown when something was actually tested and reported it.
  std::string name = "New connection";
  std::string host = "127.0.0.1";
  std::string port = "3306";
  std::string user;
  std::string pass;
  std::string db;
  std::string status_msg;
  bool test_ok = false;

  if (auto* v = m->find("name")) name = v->to_text();
  if (auto* v = m->find("host")) host = v->to_text();
  if (auto* v = m->find("port")) port = v->to_text();
  if (auto* v = m->find("user")) user = v->to_text();
  if (auto* v = m->find("password")) pass = v->to_text();
  if (auto* v = m->find("database")) db = v->to_text();
  if (auto* v = m->find("status")) status_msg = v->to_text();
  if (auto* v = m->find("test_ok")) test_ok = v->truthy();

  // Dim overlay
  fill_rect_aa(*cv, 0, 0, cv->width, cv->height, 0x80000000);

  // Dialog Window
  fill_round_rect_aa(*cv, x, y, w, h, 10.0, is_dark ? 0xFF0F172A : 0xFFFFFFFF);
  stroke_round_rect_aa(*cv, x, y, w, h, 10.0, is_dark ? 0xFF334155 : 0xFFCBD5E1, 1.5);

  // Dialog Header
  fill_round_rect_aa(*cv, x, y, w, 36.0, 10.0, is_dark ? 0xFF1E293B : 0xFFE2E8F0);
  fill_rect_aa(*cv, x, y + 26.0, w, 10.0, is_dark ? 0xFF1E293B : 0xFFE2E8F0);
  draw_line_aa(*cv, x, y + 36.0, x + w, y + 36.0, is_dark ? 0xFF334155 : 0xFFCBD5E1, 1.0);

  std::vector<Value> db_icon = {args[0], Value::real(x + 14.0), Value::real(y + 10.0), Value::real(16.0), Value::string("server"), Value::integer((int64_t)0xFF10B981)};
  bi_draw_icon(vm, db_icon, error);
  draw_text_aa(*cv, "New Database Connection — MySQL / MariaDB / ASQL", x + 36.0, y + 10.0, is_dark ? 0xFFFFFFFF : 0xFF0F172A, "ui_bold", 13);

  // Form Fields
  struct FormField { std::string id; std::string label; std::string val; };
  std::vector<FormField> fields = {
    {"name", "Connection Name:", name},
    {"host", "Host / IP Address:", host},
    {"port", "Port:", port},
    {"user", "User Name:", user},
    {"password", "Password:", pass},
    {"database", "Default Database:", db}
  };

  double fy = y + 52.0;
  for (const auto& ff : fields) {
    draw_text_aa(*cv, ff.label, x + 24.0, fy + 5.0, is_dark ? 0xFFCBD5E1 : 0xFF334155, "ui", 12);

    double input_x = x + 160.0;
    double input_w = w - 184.0;
    bool is_act = (ff.id == active_field);

    fill_round_rect_aa(*cv, input_x, fy, input_w, 28.0, 4.0, is_dark ? 0xFF1E293B : 0xFFF1F5F9);
    stroke_round_rect_aa(*cv, input_x, fy, input_w, 28.0, 4.0, is_act ? 0xFF38BDF8 : (is_dark ? 0xFF334155 : 0xFFCBD5E1), is_act ? 1.5 : 1.0);
    draw_text_aa(*cv, ff.val, input_x + 8.0, fy + 6.0, is_dark ? 0xFFFFFFFF : 0xFF0F172A, "ui", 12);

    fy += 36.0;
  }

  // Test Connection status pill
  if (!status_msg.empty()) {
    uint32_t stat_bg = test_ok ? 0x2016A34A : 0x20DC2626;
    uint32_t stat_fg = test_ok ? 0xFF4ADE80 : 0xFFF87171;
    fill_round_rect_aa(*cv, x + 24.0, fy + 6.0, w - 48.0, 28.0, 6.0, stat_bg);
    stroke_round_rect_aa(*cv, x + 24.0, fy + 6.0, w - 48.0, 28.0, 6.0, test_ok ? 0xFF16A34A : 0xFFDC2626, 1.0);
    draw_text_aa(*cv, status_msg, x + 36.0, fy + 12.0, stat_fg, "ui_bold", 11);
  }

  // Action Buttons
  double btn_y = y + h - 42.0;
  fill_round_rect_aa(*cv, x + 24.0, btn_y, 130.0, 30.0, 5.0, 0xFF334155);
  draw_text_aa(*cv, "Test Connection", x + 36.0, btn_y + 8.0, 0xFFFFFFFF, "ui", 12);

  fill_round_rect_aa(*cv, x + w - 170.0, btn_y, 70.0, 30.0, 5.0, 0xFF334155);
  draw_text_aa(*cv, "Cancel", x + w - 156.0, btn_y + 8.0, 0xFFFFFFFF, "ui", 12);

  fill_round_rect_aa(*cv, x + w - 90.0, btn_y, 70.0, 30.0, 5.0, 0xFF2563EB);
  draw_text_aa(*cv, "OK", x + w - 66.0, btn_y + 8.0, 0xFFFFFFFF, "ui_bold", 12);
  return args[0];
}

Value bi_draw_query_plan(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "draw_query_plan");
  if (!cv) return Value::nil();
  if (args.size() < 6) { error = "draw_query_plan wants (cv, x, y, w, h, plan_nodes, [is_dark])"; return Value::nil(); }
  double x = args[1].as_double(), y = args[2].as_double(), w = args[3].as_double(), h = args[4].as_double();
  if (!want_list(args[5], error, "draw_query_plan")) return Value::nil();
  const auto& nodes = args[5].list_items();
  bool is_dark = args.size() > 6 ? args[6].truthy() : true;

  uint32_t bg = is_dark ? 0xFF0F172A : 0xFFFFFFFF;
  uint32_t border_col = is_dark ? 0xFF334155 : 0xFFCBD5E1;

  fill_rect_aa(*cv, x, y, w, h, bg);
  stroke_rect_aa(*cv, x, y, w, h, border_col, 1.0);

  // Title
  draw_text_aa(*cv, "Visual EXPLAIN / Execution Plan", x + 14.0, y + 12.0, is_dark ? 0xFFFFFFFF : 0xFF0F172A, "ui_bold", 13);
  draw_line_aa(*cv, x, y + 32.0, x + w, y + 32.0, border_col, 1.0);

  double node_w = 160.0;
  double node_h = 60.0;
  double cur_nx = x + 20.0;
  double cur_ny = y + 50.0;

  for (size_t i = 0; i < nodes.size(); ++i) {
    if (cur_nx + node_w > x + w - 10.0) {
      cur_nx = x + 20.0;
      cur_ny += node_h + 30.0;
    }
    if (cur_ny + node_h > y + h - 10.0) break;

    std::string op = "Index Scan", tbl = "orders", cost = "12.4", rows_cnt = "1,420";
    if (nodes[i].type == Type::Map) {
      auto* m = static_cast<MapObj*>(nodes[i].obj.get());
      if (auto* v = m->find("op")) op = v->to_text();
      if (auto* v = m->find("table")) tbl = v->to_text();
      if (auto* v = m->find("cost")) cost = v->to_text();
      if (auto* v = m->find("rows")) rows_cnt = v->to_text();
    } else {
      op = nodes[i].to_text();
    }

    fill_round_rect_aa(*cv, cur_nx, cur_ny, node_w, node_h, 6.0, is_dark ? 0xFF1E293B : 0xFFF1F5F9);
    stroke_round_rect_aa(*cv, cur_nx, cur_ny, node_w, node_h, 6.0, 0xFF38BDF8, 1.0);

    draw_text_aa(*cv, op, cur_nx + 8.0, cur_ny + 6.0, 0xFF38BDF8, "ui_bold", 11);
    draw_text_aa(*cv, "Table: " + tbl, cur_nx + 8.0, cur_ny + 22.0, is_dark ? 0xFFF8FAFC : 0xFF0F172A, "ui", 11);
    draw_text_aa(*cv, "Rows: " + rows_cnt + " | Cost: " + cost, cur_nx + 8.0, cur_ny + 38.0, is_dark ? 0xFF94A3B8 : 0xFF64748B, "ui", 10);

    // Draw connecting arrow to next node
    if (i + 1 < nodes.size() && cur_nx + node_w + 30.0 < x + w) {
      draw_line_aa(*cv, cur_nx + node_w, cur_ny + node_h * 0.5, cur_nx + node_w + 24.0, cur_ny + node_h * 0.5, 0xFF64748B, 1.5);
      draw_line_aa(*cv, cur_nx + node_w + 18.0, cur_ny + node_h * 0.5 - 4.0, cur_nx + node_w + 24.0, cur_ny + node_h * 0.5, 0xFF64748B, 1.5);
      draw_line_aa(*cv, cur_nx + node_w + 18.0, cur_ny + node_h * 0.5 + 4.0, cur_nx + node_w + 24.0, cur_ny + node_h * 0.5, 0xFF64748B, 1.5);
    }

    cur_nx += node_w + 30.0;
  }
  return args[0];
}

Value bi_draw_status_bar(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "draw_status_bar");
  if (!cv) return Value::nil();
  if (args.size() < 9) { error = "draw_status_bar wants (cv, x, y, w, h, host_info, db_info, query_stats, encoding, [is_connected, is_dark])"; return Value::nil(); }
  double x = args[1].as_double(), y = args[2].as_double(), w = args[3].as_double(), h = args[4].as_double();
  std::string host_info = args[5].to_text();
  std::string db_info = args[6].to_text();
  std::string query_stats = args[7].to_text();
  std::string encoding = args[8].to_text();
  bool is_connected = args.size() > 9 ? args[9].truthy() : true;
  bool is_dark = args.size() > 10 ? args[10].truthy() : true;

  uint32_t bg = is_dark ? 0xFF0B1120 : 0xFFE2E8F0;
  uint32_t border_col = is_dark ? 0xFF334155 : 0xFFCBD5E1;
  uint32_t fg = is_dark ? 0xFF94A3B8 : 0xFF475569;

  fill_rect_aa(*cv, x, y, w, h, bg);
  draw_line_aa(*cv, x, y, x + w, y, border_col, 1.0);

  // Status LED dot
  fill_circle_aa(*cv, x + 12.0, y + h * 0.5, 4.0, is_connected ? 0xFF10B981 : 0xFFEF4444);

  double cur_x = x + 24.0;
  draw_text_aa(*cv, host_info, cur_x, y + 4.0, is_dark ? 0xFFFFFFFF : 0xFF0F172A, "ui_bold", 11);
  auto m_host = measure_text_metrics(host_info, "ui_bold", 11);
  cur_x += m_host->find("width")->as_int() + 14.0;

  draw_line_aa(*cv, cur_x, y + 4.0, cur_x, y + h - 4.0, border_col, 1.0);
  cur_x += 12.0;

  draw_text_aa(*cv, "DB: " + db_info, cur_x, y + 4.0, is_dark ? 0xFF38BDF8 : 0xFF0284C7, "ui_bold", 11);
  auto m_db = measure_text_metrics("DB: " + db_info, "ui_bold", 11);
  cur_x += m_db->find("width")->as_int() + 14.0;

  draw_line_aa(*cv, cur_x, y + 4.0, cur_x, y + h - 4.0, border_col, 1.0);
  cur_x += 12.0;

  double enc_w = 90.0;
  double max_stats_w = w - (cur_x - x) - enc_w - 16.0;
  draw_truncated_text_aa(*cv, query_stats, cur_x, y + 4.0, max_stats_w, fg, "ui", 11);

  // Right-aligned encoding & engine
  draw_text_aa(*cv, encoding, x + w - enc_w, y + 4.0, fg, "ui", 11);
  return args[0];
}

// ---------------------------------------------------------------------------
// SQL Parsing, Formatting, Schema DDL & Data Export Builtins
// ---------------------------------------------------------------------------

Value bi_sql_tokenize(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "sql_tokenize")) return Value::nil();
  auto tokens = tokenize_sql_string(args[0].str());
  std::vector<Value> out;
  out.reserve(tokens.size());

  for (const auto& tok : tokens) {
    auto m = std::make_shared<MapObj>();
    std::string k_str = "identifier";
    if (tok.kind == SqlToken::Kind::Keyword) k_str = "keyword";
    else if (tok.kind == SqlToken::Kind::Function) k_str = "function";
    else if (tok.kind == SqlToken::Kind::String) k_str = "string";
    else if (tok.kind == SqlToken::Kind::Number) k_str = "number";
    else if (tok.kind == SqlToken::Kind::Comment) k_str = "comment";
    else if (tok.kind == SqlToken::Kind::Operator) k_str = "operator";
    else if (tok.kind == SqlToken::Kind::Whitespace) k_str = "whitespace";

    m->put("type", Value::string(k_str));
    m->put("text", Value::string(tok.text));
    out.push_back(Value::object(Type::Map, m));
  }
  return Value::list(std::move(out));
}

Value bi_sql_format(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "sql_format")) return Value::nil();
  auto tokens = tokenize_sql_string(args[0].str());
  std::string out;

  for (size_t i = 0; i < tokens.size(); ++i) {
    if (tokens[i].kind == SqlToken::Kind::Whitespace) continue;

    std::string upper_tok = tokens[i].text;
    for (char& uc : upper_tok) uc = std::toupper((unsigned char)uc);

    if (upper_tok == "FROM" || upper_tok == "WHERE" || upper_tok == "GROUP" ||
        upper_tok == "ORDER" || upper_tok == "HAVING" || upper_tok == "LIMIT" ||
        upper_tok == "LEFT" || upper_tok == "RIGHT" || upper_tok == "INNER" || upper_tok == "JOIN") {
      out += "\n  ";
    }

    if (tokens[i].kind == SqlToken::Kind::Keyword) {
      out += upper_tok + " ";
    } else if (tokens[i].kind == SqlToken::Kind::Function) {
      out += upper_tok;
    } else if (tokens[i].text == ",") {
      out += ",\n    ";
    } else {
      out += tokens[i].text + " ";
    }
  }
  return Value::string(out);
}

Value bi_sql_generate_ddl(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "sql_generate_ddl (table_name)")) return Value::nil();
  if (!want_list(args[1], error, "sql_generate_ddl (columns)")) return Value::nil();
  std::string tbl = args[0].str();
  const auto& cols = args[1].list_items();

  std::string ddl = "CREATE TABLE `" + tbl + "` (\n";
  std::string pks = "";

  for (size_t i = 0; i < cols.size(); ++i) {
    if (cols[i].type != Type::Map) continue;
    auto* m = static_cast<MapObj*>(cols[i].obj.get());
    std::string name = m->find("name") ? m->find("name")->to_text() : ("col_" + std::to_string(i + 1));
    std::string type = m->find("type") ? m->find("type")->to_text() : "VARCHAR(255)";
    bool is_pk = m->find("pk") ? m->find("pk")->truthy() : false;
    bool not_null = m->find("not_null") ? m->find("not_null")->truthy() : false;
    bool auto_inc = m->find("auto_inc") ? m->find("auto_inc")->truthy() : false;

    ddl += "  `" + name + "` " + type;
    if (not_null) ddl += " NOT NULL";
    if (auto_inc) ddl += " AUTO_INCREMENT";
    ddl += ",\n";

    if (is_pk) {
      if (!pks.empty()) pks += ", ";
      pks += "`" + name + "`";
    }
  }

  if (!pks.empty()) {
    ddl += "  PRIMARY KEY (" + pks + ")\n";
  } else if (ddl.size() > 2 && ddl[ddl.size() - 2] == ',') {
    ddl.erase(ddl.size() - 2, 1);
  }
  ddl += ") ENGINE=ASQL DEFAULT CHARSET=utf8mb4;";
  return Value::string(ddl);
}

Value bi_data_to_csv(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_list(args[0], error, "data_to_csv (columns)")) return Value::nil();
  if (!want_list(args[1], error, "data_to_csv (rows)")) return Value::nil();
  const auto& cols = args[0].list_items();
  const auto& rows = args[1].list_items();

  auto format_cell = [](std::string& out, const Value& v) {
    if (v.type == Type::Nil) return;
    if (v.type == Type::Int) { out += std::to_string(v.as_int()); return; }
    if (v.type == Type::Real) {
      char b[64];
      std::snprintf(b, sizeof b, "%.10g", v.as_double());
      out += b;
      return;
    }
    if (v.type == Type::Bool) { out += v.b ? "true" : "false"; return; }
    std::string s = v.type == Type::Str ? v.str() : v.to_text();
    bool needs = false;
    for (char c : s) {
      if (c == ',' || c == '"' || c == '\n' || c == '\r') { needs = true; break; }
    }
    if (!needs) {
      out += s;
    } else {
      out += '"';
      for (char c : s) {
        if (c == '"') out += "\"\"";
        else out += c;
      }
      out += '"';
    }
  };

  std::string csv;
  for (size_t c = 0; c < cols.size(); ++c) {
    if (c > 0) csv += ",";
    format_cell(csv, cols[c]);
  }
  csv += "\r\n";

  for (size_t r = 0; r < rows.size(); ++r) {
    if (rows[r].type != Type::List) continue;
    const auto& cells = rows[r].list_items();
    for (size_t c = 0; c < cells.size(); ++c) {
      if (c > 0) csv += ",";
      format_cell(csv, cells[c]);
    }
    csv += "\r\n";
  }
  return Value::string(csv);
}

Value bi_data_to_json(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_list(args[0], error, "data_to_json (columns)")) return Value::nil();
  if (!want_list(args[1], error, "data_to_json (rows)")) return Value::nil();
  const auto& cols = args[0].list_items();
  const auto& rows = args[1].list_items();

  std::string json = "[\n";
  for (size_t r = 0; r < rows.size(); ++r) {
    if (rows[r].type != Type::List) continue;
    if (r > 0) json += ",\n";
    json += "  {";
    const auto& cells = rows[r].list_items();
    for (size_t c = 0; c < cols.size() && c < cells.size(); ++c) {
      if (c > 0) json += ", ";
      json += "\"" + cols[c].to_text() + "\": ";
      if (cells[c].type == Type::Nil) json += "null";
      else if (cells[c].type == Type::Int || cells[c].type == Type::Real) json += cells[c].to_text();
      else if (cells[c].type == Type::Bool) json += cells[c].b ? "true" : "false";
      else json += "\"" + cells[c].to_text() + "\"";
    }
    json += "}";
  }
  json += "\n]";
  return Value::string(json);
}

Value bi_data_to_sql_insert(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "data_to_sql_insert (table_name)")) return Value::nil();
  if (!want_list(args[1], error, "data_to_sql_insert (columns)")) return Value::nil();
  if (!want_list(args[2], error, "data_to_sql_insert (rows)")) return Value::nil();
  std::string tbl = args[0].str();
  const auto& cols = args[1].list_items();
  const auto& rows = args[2].list_items();
  if (rows.empty()) return Value::string("");

  std::string sql = "INSERT INTO `" + tbl + "` (";
  for (size_t c = 0; c < cols.size(); ++c) {
    if (c > 0) sql += ", ";
    sql += "`" + cols[c].to_text() + "`";
  }
  sql += ") VALUES\n";

  for (size_t r = 0; r < rows.size(); ++r) {
    if (rows[r].type != Type::List) continue;
    if (r > 0) sql += ",\n";
    sql += "  (";
    const auto& cells = rows[r].list_items();
    for (size_t c = 0; c < cols.size() && c < cells.size(); ++c) {
      if (c > 0) sql += ", ";
      if (cells[c].type == Type::Nil) sql += "NULL";
      else if (cells[c].type == Type::Int || cells[c].type == Type::Real) sql += cells[c].to_text();
      else if (cells[c].type == Type::Bool) sql += cells[c].b ? "1" : "0";
      else sql += "'" + cells[c].to_text() + "'";
    }
    sql += ")";
  }
  sql += ";";
  return Value::string(sql);
}


Value bi_sql_suggest(Vm&, std::vector<Value>& args, std::string& error) {
  if (args.empty()) {
    error = "sql_suggest wants (sql, [cursor_pos, schema_tables, schema_columns])";
    return Value::nil();
  }
  std::string sql = args[0].to_text();
  int64_t cursor_pos = args.size() > 1 ? args[1].as_int() : (int64_t)sql.size();
  std::vector<std::string> extra_tables;
  std::vector<std::string> extra_columns;

  if (args.size() > 2) {
    if (args[2].type == Type::List) {
      for (const auto& item : args[2].list_items()) extra_tables.push_back(item.to_text());
    } else if (args[2].type == Type::Map) {
      auto* m = static_cast<MapObj*>(args[2].obj.get());
      for (const auto& e : m->entries) extra_tables.push_back(e.first);
    }
  }
  if (args.size() > 3) {
    if (args[3].type == Type::List) {
      for (const auto& item : args[3].list_items()) extra_columns.push_back(item.to_text());
    }
  }

  if (cursor_pos < 0) cursor_pos = 0;
  if (cursor_pos > (int64_t)sql.size()) cursor_pos = (int64_t)sql.size();

  size_t start = (size_t)cursor_pos;
  while (start > 0 && (std::isalnum((unsigned char)sql[start - 1]) || sql[start - 1] == '_' || sql[start - 1] == '$')) {
    start--;
  }
  std::string prefix = sql.substr(start, cursor_pos - start);
  std::string prefix_upper = prefix;
  for (char& c : prefix_upper) c = (char)std::toupper((unsigned char)c);

  std::string before = sql.substr(0, start);
  while (!before.empty() && std::isspace((unsigned char)before.back())) before.pop_back();
  bool has_dot = (!before.empty() && before.back() == '.');
  if (has_dot) {
    before.pop_back();
    while (!before.empty() && std::isspace((unsigned char)before.back())) before.pop_back();
  }

  std::string last_word;
  size_t w_end = before.size();
  while (w_end > 0 && (std::isalnum((unsigned char)before[w_end - 1]) || before[w_end - 1] == '_')) w_end--;
  last_word = before.substr(w_end);
  for (char& c : last_word) c = (char)std::toupper((unsigned char)c);

  bool is_table_context = !has_dot && (last_word == "FROM" || last_word == "JOIN" || last_word == "INTO" || last_word == "TABLE" || last_word == "UPDATE");
  bool is_column_context = has_dot || (last_word == "SELECT" || last_word == "WHERE" || last_word == "ON" || last_word == "AND" || last_word == "OR" || last_word == "BY" || last_word == "SET");

  static const std::vector<std::string> kKeywords = {
    "SELECT", "FROM", "WHERE", "INSERT INTO", "UPDATE", "DELETE FROM",
    "JOIN", "LEFT JOIN", "RIGHT JOIN", "INNER JOIN", "CROSS JOIN",
    "GROUP BY", "ORDER BY", "HAVING", "LIMIT", "OFFSET",
    "AND", "OR", "NOT", "IN", "LIKE", "IS NULL", "IS NOT NULL",
    "AS", "ON", "DISTINCT", "UNION ALL", "UNION", "VALUES", "SET",
    "CREATE TABLE", "ALTER TABLE", "DROP TABLE", "SHOW TABLES",
    "SHOW COLUMNS FROM", "DESCRIBE", "EXPLAIN"
  };

  static const std::vector<std::string> kFunctions = {
    "COUNT()", "SUM()", "AVG()", "MIN()", "MAX()", "NOW()",
    "CONCAT()", "COALESCE()", "IFNULL()", "ROUND()", "DATE_FORMAT()",
    "LOWER()", "UPPER()", "SUBSTRING()", "TRIM()", "LENGTH()"
  };

  std::vector<Value> suggestions;
  auto add_sugg = [&](const std::string& text, const std::string& kind, const std::string& desc, int priority) {
    auto m = std::make_shared<MapObj>();
    m->put("text", Value::string(text));
    m->put("kind", Value::string(kind));
    m->put("desc", Value::string(desc));
    m->put("priority", Value::integer(priority));
    suggestions.push_back(Value::object(Type::Map, m));
  };

  for (const auto& tbl : extra_tables) {
    std::string tbl_upper = tbl;
    for (char& c : tbl_upper) c = (char)std::toupper((unsigned char)c);
    if (prefix.empty() || tbl_upper.find(prefix_upper) == 0 || tbl.find(prefix) == 0) {
      add_sugg(tbl, "table", "Database Table", is_table_context ? 100 : (has_dot ? 10 : 50));
    }
  }

  for (const auto& col : extra_columns) {
    std::string col_upper = col;
    for (char& c : col_upper) c = (char)std::toupper((unsigned char)c);
    if (prefix.empty() || col_upper.find(prefix_upper) == 0 || col.find(prefix) == 0) {
      add_sugg(col, "column", "Table Column", is_column_context ? 100 : 40);
    }
  }

  if (!has_dot) {
    for (const auto& kw : kKeywords) {
      if (prefix.empty() || kw.find(prefix_upper) == 0) {
        add_sugg(kw, "keyword", "SQL Keyword", is_table_context ? 20 : 60);
      }
    }

    for (const auto& fn_name : kFunctions) {
      if (prefix.empty() || fn_name.find(prefix_upper) == 0) {
        add_sugg(fn_name, "function", "SQL Function", is_column_context ? 70 : 30);
      }
    }
  }

  std::sort(suggestions.begin(), suggestions.end(), [](const Value& a, const Value& b) {
    auto* ma = static_cast<MapObj*>(a.obj.get());
    auto* mb = static_cast<MapObj*>(b.obj.get());
    int64_t pa = ma->find("priority")->as_int();
    int64_t pb = mb->find("priority")->as_int();
    if (pa != pb) return pa > pb;
    return ma->find("text")->str() < mb->find("text")->str();
  });

  auto out = std::make_shared<MapObj>();
  out->put("prefix", Value::string(prefix));
  out->put("start", Value::integer((int64_t)start));
  out->put("end", Value::integer(cursor_pos));
  out->put("items", Value::list(suggestions));
  return Value::object(Type::Map, out);
}

Value bi_draw_autocomplete_popup(Vm&, std::vector<Value>& args, std::string& error) {
  auto cv = get_canvas(args[0], error, "draw_autocomplete_popup");
  if (!cv) return Value::nil();
  if (args.size() < 6) {
    error = "draw_autocomplete_popup wants (cv, x, y, w, items, active_index, [is_dark])";
    return Value::nil();
  }
  double x = args[1].as_double(), y = args[2].as_double(), w = args[3].as_double();
  if (!want_list(args[4], error, "draw_autocomplete_popup (items)")) return Value::nil();
  const auto& items = args[4].list_items();
  int64_t active_idx = args[5].as_int();
  bool is_dark = args.size() > 6 ? args[6].truthy() : true;

  if (items.empty()) return args[0];

  double row_h = 24.0;
  size_t max_items = std::min((size_t)8, items.size());
  double h = max_items * row_h + 8.0;

  uint32_t bg = is_dark ? 0xFF1E293B : 0xFFFFFFFF;
  uint32_t border_col = is_dark ? 0xFF38BDF8 : 0xFF0284C7;
  uint32_t active_row_bg = is_dark ? 0xFF0F172A : 0xFFE0F2FE;
  uint32_t fg = is_dark ? 0xFFF8FAFC : 0xFF0F172A;
  uint32_t desc_fg = is_dark ? 0xFF94A3B8 : 0xFF64748B;

  // Drop shadow
  fill_round_rect_aa(*cv, x + 4.0, y + 4.0, w, h, 6.0, 0x40000000);
  // Background
  fill_round_rect_aa(*cv, x, y, w, h, 6.0, bg);
  stroke_round_rect_aa(*cv, x, y, w, h, 6.0, border_col, 1.5);

  double cur_y = y + 4.0;
  for (size_t i = 0; i < max_items; ++i) {
    bool is_active = ((int64_t)i == active_idx);
    if (is_active) {
      fill_round_rect_aa(*cv, x + 4.0, cur_y, w - 8.0, row_h, 4.0, active_row_bg);
    }

    std::string text, kind = "kw", desc = "";
    if (items[i].type == Type::Str) {
      text = items[i].str();
    } else if (items[i].type == Type::Map) {
      auto* m = static_cast<MapObj*>(items[i].obj.get());
      if (auto* v = m->find("text")) text = v->to_text();
      if (auto* v = m->find("kind")) kind = v->to_text();
      if (auto* v = m->find("desc")) desc = v->to_text();
    }

    // Badge for kind
    uint32_t badge_bg = 0xFF38BDF8;
    std::string badge_lbl = "KW";
    if (kind == "table") { badge_bg = 0xFF10B981; badge_lbl = "TBL"; }
    else if (kind == "column") { badge_bg = 0xFFF59E0B; badge_lbl = "COL"; }
    else if (kind == "function") { badge_bg = 0xFF8B5CF6; badge_lbl = "FN"; }

    fill_round_rect_aa(*cv, x + 8.0, cur_y + 4.0, 32.0, 16.0, 3.0, badge_bg);
    draw_text_aa(*cv, badge_lbl, x + 11.0, cur_y + 5.0, 0xFFFFFFFF, "ui_bold", 9);

    // Item text
    draw_truncated_text_aa(*cv, text, x + 46.0, cur_y + 4.0, w - 140.0, is_active ? 0xFF38BDF8 : fg, is_active ? "ui_bold" : "ui", 12);

    // Description text
    if (!desc.empty()) {
      draw_truncated_text_aa(*cv, desc, x + w - 90.0, cur_y + 5.0, 80.0, desc_fg, "ui", 10);
    }

    cur_y += row_h;
  }
  return args[0];
}

Value bi_time_us(Vm&, std::vector<Value>&, std::string&) {
  using namespace std::chrono;
  auto now = system_clock::now().time_since_epoch();
  return Value::integer((int64_t)duration_cast<microseconds>(now).count());
}


}  // namespace

namespace {

/* Files. Every one of these fails loudly - a script that thinks it saved
 * something must never be wrong about it. */
Files* files_of(Vm& vm, std::string& error, const char* who) {
  if (!vm.files) error = std::string(who) + "(): this machine has no file storage attached";
  return vm.files;
}

std::string file_error(const char* who, const std::string& path, const std::string& why) {
  return std::string(who) + "(\"" + path + "\"): " + why;
}

Value bi_read_file(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "read_file")) return Value::nil();
  Files* f = files_of(vm, error, "read_file");
  if (!f) return Value::nil();
  std::string out, why;
  if (!f->read(args[0].str(), out, why)) { error = file_error("read_file", args[0].str(), why); return Value::nil(); }
  return Value::string(std::move(out));
}

// read_bytes(path) -> bytes, the file as it is (read_file's text is bytes too, but a string).
Value bi_read_bytes(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "read_bytes")) return Value::nil();
  Files* f = files_of(vm, error, "read_bytes");
  if (!f) return Value::nil();
  std::string out, why;
  if (!f->read(args[0].str(), out, why)) { error = file_error("read_bytes", args[0].str(), why); return Value::nil(); }
  return Value::bytes(std::vector<uint8_t>(out.begin(), out.end()));
}

// env(name, [default]): an app setting or environment variable, or default (nil) when it is unset or empty -
// getenv() hands back "" for a variable set to nothing, which a port or a host never means.
Value bi_env(Vm& vm, std::vector<Value>& args, std::string& error) {
  std::vector<Value> one{args[0]};
  Value v = bi_getenv(vm, one, error);
  if (!error.empty()) return Value::nil();
  if (v.type == Type::Str && !v.str().empty()) return v;
  return args.size() > 1 ? args[1] : Value::nil();
}

// secret(name, [default]): env(name), or else the contents of the file NAME_FILE names, trimmed (a password kept in
// a file of its own, mode 600, rather than in the environment). Neither: default, or "".
Value bi_secret(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "secret")) return Value::nil();
  std::vector<Value> one{args[0]};
  Value v = bi_env(vm, one, error);
  if (!error.empty()) return Value::nil();
  if (v.type == Type::Str) return v;
  std::vector<Value> file_arg{Value::string(args[0].str() + "_FILE")};
  Value path = bi_env(vm, file_arg, error);
  if (!error.empty()) return Value::nil();
  if (path.type != Type::Str) return args.size() > 1 ? args[1] : Value::string("");
  Files* f = files_of(vm, error, "secret");
  if (!f) return Value::nil();
  std::string out, why;
  if (!f->read(path.str(), out, why)) { error = file_error("secret", path.str(), why); return Value::nil(); }
  const size_t a = out.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return args.size() > 1 ? args[1] : Value::string("");
  const size_t b = out.find_last_not_of(" \t\r\n");
  return Value::string(out.substr(a, b - a + 1));
}

// write_bytes(path, bytes) -> how many were written
Value bi_write_bytes(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "write_bytes")) return Value::nil();
  if (args[1].type != Type::Bytes) { error = std::string("write_bytes() wants bytes, got a ") + type_name(args[1].type); return Value::nil(); }
  Files* f = files_of(vm, error, "write_bytes");
  if (!f) return Value::nil();
  const auto& d = args[1].bytes_data();
  std::string why;
  if (!f->write(args[0].str(), std::string(d.begin(), d.end()), false, why)) { error = file_error("write_bytes", args[0].str(), why); return Value::nil(); }
  return Value::integer(static_cast<int64_t>(d.size()));
}

Value write_or_append(Vm& vm, std::vector<Value>& args, std::string& error, bool append) {
  const char* who = append ? "append_file" : "write_file";
  if (!want_str(args[0], error, who)) return Value::nil();
  Files* f = files_of(vm, error, who);
  if (!f) return Value::nil();
  std::string data;
  if (args[1].type == Type::Str) {
    data = args[1].str();
  } else if (args[1].type == Type::Bytes) {
    const auto& b = args[1].bytes_data();
    data.assign(reinterpret_cast<const char*>(b.data()), b.size());
  } else {
    data = args[1].to_text();
  }
  std::string why;
  if (!f->write(args[0].str(), data, append, why)) { error = file_error(who, args[0].str(), why); return Value::nil(); }
  return Value::integer(static_cast<int64_t>(data.size()));
}

Value bi_write_file(Vm& vm, std::vector<Value>& args, std::string& error) { return write_or_append(vm, args, error, false); }
Value bi_append_file(Vm& vm, std::vector<Value>& args, std::string& error) { return write_or_append(vm, args, error, true); }

Value bi_list_dir(Vm& vm, std::vector<Value>& args, std::string& error) {
  const std::string path = args.empty() ? std::string() : (want_str(args[0], error, "list_dir") ? args[0].str() : std::string());
  if (!error.empty()) return Value::nil();
  Files* f = files_of(vm, error, "list_dir");
  if (!f) return Value::nil();
  std::vector<FileEntry> entries;
  std::string why;
  if (!f->list(path, entries, why)) { error = file_error("list_dir", path, why); return Value::nil(); }
  std::vector<Value> out;
  for (const auto& e : entries) {
    auto m = std::make_shared<MapObj>();
    m->put("name", Value::string(e.name));
    m->put("size", Value::integer(e.size));
    m->put("dir", Value::boolean(e.dir));
    out.push_back(Value::object(Type::Map, m));
  }
  return Value::list(std::move(out));
}

Value bi_delete_file(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "delete_file")) return Value::nil();
  Files* f = files_of(vm, error, "delete_file");
  if (!f) return Value::nil();
  std::string why;
  if (!f->remove(args[0].str(), why)) { error = file_error("delete_file", args[0].str(), why); return Value::nil(); }
  return Value::boolean(true);
}

Value bi_file_exists(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "file_exists")) return Value::nil();
  Files* f = files_of(vm, error, "file_exists");
  if (!f) return Value::nil();
  return Value::boolean(f->exists(args[0].str()));
}

// file_stat(path): {size, dir, modified, mode} - modified in milliseconds since 1970 - or nil when nothing is there.
Value bi_file_stat(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "file_stat")) return Value::nil();
  Files* f = files_of(vm, error, "file_stat");
  if (!f) return Value::nil();
  FileStat st;
  std::string why;
  if (!f->stat(args[0].str(), st, why)) {
    if (!why.empty()) error = file_error("file_stat", args[0].str(), why);
    return Value::nil();
  }
  auto m = std::make_shared<MapObj>();
  m->put("size", Value::integer(st.size));
  m->put("dir", Value::boolean(st.dir));
  m->put("modified", Value::integer(st.modified_ms));
  m->put("mode", Value::integer(st.mode));
  return Value::object(Type::Map, m);
}

// rename_file(from, to): `from` takes the name `to`, replacing what was there (in one step where the storage can).
Value bi_rename_file(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "rename_file") || !want_str(args[1], error, "rename_file")) return Value::nil();
  Files* f = files_of(vm, error, "rename_file");
  if (!f) return Value::nil();
  std::string why;
  if (!f->rename(args[0].str(), args[1].str(), why)) { error = file_error("rename_file", args[0].str(), why); return Value::nil(); }
  return Value::boolean(true);
}

// copy_file(from, to): a file's bytes under a second name, replacing what was there.
Value bi_copy_file(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "copy_file") || !want_str(args[1], error, "copy_file")) return Value::nil();
  Files* f = files_of(vm, error, "copy_file");
  if (!f) return Value::nil();
  if (f->is_dir(args[0].str())) { error = file_error("copy_file", args[0].str(), "is a directory"); return Value::nil(); }
  std::string data, why;
  if (!f->read(args[0].str(), data, why)) { error = file_error("copy_file", args[0].str(), why); return Value::nil(); }
  if (!f->write(args[1].str(), data, false, why)) { error = file_error("copy_file", args[1].str(), why); return Value::nil(); }
  return Value::boolean(true);
}

// One path segment against one glob segment: * (any run), ? (one character), [abc] [a-z] [!abc]. A name that
// starts with '.' is matched only by a pattern that starts with one, as a shell does.
bool glob_segment(const std::string& pat, const std::string& name) {
  if (!name.empty() && name[0] == '.' && (pat.empty() || pat[0] != '.')) return false;
  size_t p = 0, n = 0, star_p = std::string::npos, star_n = 0;
  while (n < name.size()) {
    if (p < pat.size() && pat[p] == '*') { star_p = p++; star_n = n; continue; }
    if (p < pat.size() && pat[p] == '[') {
      size_t q = p + 1;
      const bool negate = q < pat.size() && (pat[q] == '!' || pat[q] == '^');
      if (negate) ++q;
      bool hit = false, closed = false;
      for (bool first = true; q < pat.size(); first = false) {
        if (pat[q] == ']' && !first) { closed = true; break; }
        if (q + 2 < pat.size() && pat[q + 1] == '-' && pat[q + 2] != ']') {
          if ((unsigned char)name[n] >= (unsigned char)pat[q] && (unsigned char)name[n] <= (unsigned char)pat[q + 2]) hit = true;
          q += 3;
        } else {
          if (pat[q] == name[n]) hit = true;
          ++q;
        }
      }
      if (closed && hit != negate) { p = q + 1; ++n; continue; }
      if (!closed && pat[p] == name[n]) { ++p; ++n; continue; }   // an unclosed [ is itself
    } else if (p < pat.size() && (pat[p] == '?' || pat[p] == name[n])) {
      ++p; ++n;
      continue;
    }
    if (star_p == std::string::npos) return false;
    p = star_p + 1;
    n = ++star_n;
  }
  while (p < pat.size() && pat[p] == '*') ++p;
  return p == pat.size();
}

bool glob_walk(Files* f, const std::string& dir, const std::vector<std::string>& segs, size_t i,
               std::vector<std::string>& out, size_t& budget, std::string& error) {
  auto under = [&](const std::string& name) { return dir.empty() ? name : (dir == "/" ? "/" + name : dir + "/" + name); };
  if (i == segs.size()) return true;
  const std::string& seg = segs[i];
  const bool last = i + 1 == segs.size();
  if (seg == "**") {   // zero or more directories
    if (last) {        // a trailing ** is everything below
      std::vector<FileEntry> entries;
      std::string why;
      if (!f->list(dir, entries, why)) return true;
      for (const auto& e : entries) {
        if (!e.name.empty() && e.name[0] == '.') continue;
        if (budget == 0) { error = "glob(): more than 100000 entries looked at"; return false; }
        --budget;
        out.push_back(under(e.name));
        if (e.dir && !glob_walk(f, under(e.name), segs, i, out, budget, error)) return false;
      }
      return true;
    }
    if (!glob_walk(f, dir, segs, i + 1, out, budget, error)) return false;
    std::vector<FileEntry> entries;
    std::string why;
    if (!f->list(dir, entries, why)) return true;
    for (const auto& e : entries) {
      if (!e.dir || (!e.name.empty() && e.name[0] == '.')) continue;
      if (budget == 0) { error = "glob(): more than 100000 entries looked at"; return false; }
      --budget;
      if (!glob_walk(f, under(e.name), segs, i, out, budget, error)) return false;
    }
    return true;
  }
  if (seg.find_first_of("*?[") == std::string::npos) {   // a plain name: no listing needed
    const std::string p = under(seg);
    if (last) { if (f->exists(p) || f->is_dir(p)) out.push_back(p); return true; }
    return f->is_dir(p) ? glob_walk(f, p, segs, i + 1, out, budget, error) : true;
  }
  std::vector<FileEntry> entries;
  std::string why;
  if (!f->list(dir, entries, why)) return true;   // not a directory, or not readable: no matches here
  for (const auto& e : entries) {
    if (budget == 0) { error = "glob(): more than 100000 entries looked at"; return false; }
    --budget;
    if (!glob_segment(seg, e.name)) continue;
    if (last) out.push_back(under(e.name));
    else if (e.dir && !glob_walk(f, under(e.name), segs, i + 1, out, budget, error)) return false;
  }
  return true;
}

// glob(pattern): the paths that match, sorted - "*.csv", "logs/2026-10-*/*.jsonl", "**/*.gr" (** is any depth).
Value bi_glob(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "glob")) return Value::nil();
  Files* f = files_of(vm, error, "glob");
  if (!f) return Value::nil();
  const std::string& pat = args[0].str();
  std::vector<std::string> segs;
  for (size_t i = 0; i <= pat.size();) {
    size_t j = pat.find('/', i);
    if (j == std::string::npos) j = pat.size();
    if (j > i) segs.push_back(pat.substr(i, j - i));
    i = j + 1;
  }
  std::vector<std::string> out;
  size_t budget = 100000;
  if (!segs.empty() && !glob_walk(f, !pat.empty() && pat[0] == '/' ? "/" : "", segs, 0, out, budget, error)) return Value::nil();
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  std::vector<Value> vals;
  vals.reserve(out.size());
  for (auto& p : out) vals.push_back(Value::string(std::move(p)));
  return Value::list(std::move(vals));
}

Value bi_entity_assemble(Vm&, std::vector<Value>& args, std::string& error) {
  if (args.size() < 3 || args[0].type != Type::Str || args[1].type != Type::Map || args[2].type != Type::Map) {
    error = "__entity_assemble expects (name, fields, handlers)";
    return Value::nil();
  }
  const std::string& name = args[0].str();
  auto* f_map = static_cast<MapObj*>(args[1].obj.get());
  auto* h_map = static_cast<MapObj*>(args[2].obj.get());

  aios::Id128 id{};
  Value* id_val = f_map->find("identity");
  if (id_val && id_val->is_id128()) {
    id = id_val->as_id128();
  } else if (id_val && id_val->type == Type::Str) {
    aios::Id128::parse(id_val->str(), id);
  }
  if (id.is_null()) {
    static std::atomic<uint64_t> entity_seq{0x1000};
    id = aios::Id128{0xfeedfacecafe0001ull, ++entity_seq};
  }

  auto ent = std::make_shared<EntityObj>(name, id);
  for (const auto& [k, v] : f_map->entries) {
    ent->fields[k] = v;
  }
  for (const auto& [k, v] : h_map->entries) {
    ent->handlers[k] = v;
  }
  return Value::object(Type::Entity, ent);
}

Value bi_id128(Vm&, std::vector<Value>& args, std::string& error) {
  if (args.empty()) {
    static std::atomic<uint64_t> id_counter{0x8000};
    aios::Id128 gen_id{0xbeefc00112345678ull, ++id_counter};
    return Value::id128(gen_id);
  }
  if (args.size() == 1) {
    if (args[0].type == Type::Str) {
      aios::Id128 id;
      std::string s = args[0].str();
      if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) s = s.substr(2);
      while (s.size() < 32) s.push_back('0');
      if (!aios::Id128::parse(s, id)) {
        error = "id128(): '" + args[0].str() + "' is not a valid hex identity";
        return Value::nil();
      }
      return Value::id128(id);
    }
    if (args[0].is_id128()) return args[0];
  }
  if (args.size() == 2 && args[0].type == Type::Int && args[1].type == Type::Int) {
    aios::Id128 id{(uint64_t)args[0].as_int(), (uint64_t)args[1].as_int()};
    return Value::id128(id);
  }
  error = "id128() expects 0 args, 1 hex string, or (hi, lo) integers";
  return Value::nil();
}

Value bi_Capability(Vm&, std::vector<Value>& args, std::string&) {
  std::string res = "default";
  uint16_t rights = 1;
  uint64_t expires = 0;
  std::string holder = "";
  if (!args.empty() && args[0].type == Type::Map) {
    auto* m = static_cast<MapObj*>(args[0].obj.get());
    if (Value* v = m->find("resource")) res = v->to_text();
    if (Value* v = m->find("rights")) rights = (uint16_t)v->as_int();
    if (Value* v = m->find("expires")) expires = (uint64_t)v->as_int();
    if (Value* v = m->find("expiry")) expires = (uint64_t)v->as_int();
    if (Value* v = m->find("holder")) holder = v->to_text();
  } else if (!args.empty()) {
    res = args[0].to_text();
    if (args.size() > 1 && args[1].type == Type::Int) rights = (uint16_t)args[1].as_int();
    if (args.size() > 2 && args[2].type == Type::Int) expires = (uint64_t)args[2].as_int();
  }
  static std::atomic<uint64_t> cap_cnt{0x2000};
  aios::Id128 id{0xca9ab11177000001ull, ++cap_cnt};
  auto cap = std::make_shared<CapObj>(id, res, rights, expires, aios::Id128{}, holder);
  return Value::object(Type::Capability, cap);
}

Value bi_StateChannel(Vm&, std::vector<Value>& args, std::string&) {
  std::string name = "channel";
  uint32_t capacity = 256;
  if (!args.empty() && args[0].type == Type::Map) {
    auto* m = static_cast<MapObj*>(args[0].obj.get());
    if (Value* v = m->find("name")) name = v->str();
    if (Value* v = m->find("capacity")) capacity = (uint32_t)v->as_int();
    if (Value* v = m->find("provider")) name += std::string(":") + v->to_text();
  } else if (!args.empty()) {
    name = args[0].to_text();
    if (args.size() > 1 && args[1].type == Type::Int) capacity = (uint32_t)args[1].as_int();
  }
  static std::atomic<uint64_t> ch_cnt{0x3000};
  aios::Id128 id{0xc4a44e1000000001ull, ++ch_cnt};
  auto ch = std::make_shared<ChannelObj>(name, id, capacity);
  return Value::object(Type::Channel, ch);
}

Value bi_memory_allocate(Vm&, std::vector<Value>& args, std::string& error) {
  if (args.empty() || args[0].type != Type::Int) {
    error = "memory_allocate expects (size, [provider])";
    return Value::nil();
  }
  size_t sz = (size_t)args[0].as_int();
  std::string prov = "ram";
  if (args.size() > 1 && args[1].type == Type::Str) prov = args[1].str();
  static std::atomic<uint64_t> mem_cnt{0x4000};
  aios::Id128 id{0x3e30410000000001ull, ++mem_cnt};
  auto mem = std::make_shared<MemObj>(id, sz, prov);
  return Value::object(Type::MemoryObject, mem);
}

Value bi_StateFrame(Vm&, std::vector<Value>& args, std::string& error) {
  if (args.empty()) {
    error = "StateFrame expects payload";
    return Value::nil();
  }
  uint64_t gen = 1;
  aios::Id128 pub{};
  if (args.size() > 1 && args[1].type == Type::Int) gen = (uint64_t)args[1].as_int();
  if (args.size() > 2 && args[2].is_id128()) pub = args[2].as_id128();
  static std::atomic<uint64_t> frame_cnt{0x5000};
  aios::Id128 id{0xf4a3e00000000001ull, ++frame_cnt};
  auto f = std::make_shared<FrameObj>(id, gen, pub, args[0], 0);
  return Value::object(Type::StateFrame, f);
}

}  // namespace

void install_core_builtins(Vm& vm) {
  vm.define_global("PI", Value::real(3.14159265358979323846));
  vm.define_global("E", Value::real(2.71828182845904523536));
  install_hash_builtins(vm);
  install_bytes_builtins(vm);
  install_kv_builtins(vm);
  install_process_builtins(vm);   // refuses unless the host set Vm::allow_process
  install_regex_builtins(vm);
  install_time_builtins(vm);
  install_text_builtins(vm);
  install_path_builtins(vm);
  install_parallel_builtins(vm);
  install_decimal_builtins(vm);
  install_doc_builtins(vm);

  vm.define_native("print", 0, -1, bi_print);
  vm.define_native("eprint", 0, -1, bi_eprint);
  vm.define_native("log_set_level", 1, 1, bi_log_set_level);
  vm.define_native("log_get_level", 0, 0, bi_log_get_level);
  vm.define_native("log_set_format", 1, 1, bi_log_set_format);
  vm.define_native("log_get_format", 0, 0, bi_log_get_format);
  vm.define_native("log_debug", 1, 2, bi_log_debug);
  vm.define_native("log_info", 1, 2, bi_log_info);
  vm.define_native("log_warn", 1, 2, bi_log_warn);
  vm.define_native("log_error", 1, 2, bi_log_error);
  vm.define_native("len", 1, 1, bi_len);
  vm.define_native("type", 1, 1, bi_type);
  vm.define_native("str", 1, 1, bi_str);
  vm.define_native("repr", 1, 1, bi_repr);
  vm.define_native("int", 1, 1, bi_int);
  vm.define_native("real", 1, 1, bi_real);

  // Collections & Lists
  vm.define_native("push", 1, -1, bi_push);
  vm.define_native("compact", 1, 1, bi_compact);
  vm.define_native("pop", 1, 1, bi_pop);
  vm.define_native("range", 1, 3, bi_range);
  vm.define_native("sort", 1, 1, bi_sort);
  vm.define_native("reverse", 1, 1, bi_reverse);
  vm.define_native("map", 2, 2, bi_map);
  vm.define_native("flat_map", 2, 2, bi_flat_map);
  vm.define_native("filter", 2, 2, bi_filter);
  vm.define_native("reduce", 2, 3, bi_reduce);
  vm.define_native("each", 2, 2, bi_each);
  vm.define_native("for_each", 2, 2, bi_each);
  vm.define_native("any", 2, 2, bi_any);
  vm.define_native("all", 2, 2, bi_all);
  vm.define_native("find_if", 2, 2, bi_find_if);
  vm.define_native("flatten", 1, 2, bi_flatten);
  vm.define_native("concat", 1, -1, bi_concat);
  vm.define_native("insert", 3, 3, bi_insert);
  vm.define_native("remove_at", 2, 2, bi_remove_at);
  vm.define_native("zip", 2, 2, bi_zip);
  vm.define_native("enumerate", 1, 1, bi_enumerate);
  vm.define_native("take", 2, 2, bi_take);
  vm.define_native("drop", 2, 2, bi_drop);
  vm.define_native("count", 2, 2, bi_count);
  vm.define_native("unique", 1, 1, bi_unique);
  vm.define_native("dedup", 1, 1, bi_unique);

  // Maps
  vm.define_native("keys", 1, 1, bi_keys);
  vm.define_native("values", 1, 1, bi_values);
  vm.define_native("has", 2, 2, bi_has);
  vm.define_native("del", 2, 2, bi_del);
  vm.define_native("get", 2, 3, bi_get);
  vm.define_native("get_in", 2, 3, bi_get_in);
  vm.define_native("merge", 1, -1, bi_merge);
  vm.define_native("entries", 1, 1, bi_entries);
  vm.define_native("from_entries", 1, 1, bi_from_entries);

  // Strings
  vm.define_native("split", 1, 2, bi_split);
  vm.define_native("join", 1, 2, bi_join);
  vm.define_native("upper", 1, 1, bi_upper);
  vm.define_native("lower", 1, 1, bi_lower);
  vm.define_native("find", 2, 2, bi_find);
  vm.define_native("replace", 3, 3, bi_replace);
  vm.define_native("slice", 1, 3, bi_slice);
  vm.define_native("ord", 1, 1, bi_ord);
  vm.define_native("chr", 1, 1, bi_chr);
  vm.define_native("trim", 1, 2, bi_trim);
  vm.define_native("is_blank", 1, 1, bi_is_blank);
  vm.define_native("add_into", 2, 3, bi_add_into);
  vm.define_native("thing_rows", 2, 3, bi_thing_rows);
  vm.define_native("first_present", 1, -1, bi_first_present);
  vm.define_native("get_or_put", 3, 3, bi_get_or_put);
  vm.define_native("add_keys", 2, 3, bi_add_keys);
  vm.define_native("ltrim", 1, 2, bi_ltrim);
  vm.define_native("rtrim", 1, 2, bi_rtrim);
  vm.define_native("upper_rtrim", 1, 1, bi_upper_rtrim);
  vm.define_native("starts_with", 2, 2, bi_starts_with);
  vm.define_native("ends_with", 2, 2, bi_ends_with);
  vm.define_native("contains", 2, 2, bi_contains);
  vm.define_native("index_of", 2, 3, bi_index_of);
  vm.define_native("last_index_of", 2, 2, bi_last_index_of);
  vm.define_native("pad_left", 2, 3, bi_pad_left);
  vm.define_native("pad_right", 2, 3, bi_pad_right);
  vm.define_native("repeat", 2, 2, bi_repeat);
  vm.define_native("char_at", 2, 2, bi_char_at);
  vm.define_native("chars", 1, 1, bi_chars);
  vm.define_native("is_digit", 1, 1, bi_is_digit);
  vm.define_native("is_alpha", 1, 1, bi_is_alpha);
  vm.define_native("is_alnum", 1, 1, bi_is_alnum);
  vm.define_native("is_space", 1, 1, bi_is_space);

  // Math
  vm.define_native("abs", 1, 1, bi_abs);
  vm.define_native("idiv", 2, 2, bi_idiv);
  vm.define_native("import", 1, 2, bi_import);
  vm.define_native("utf8_valid", 1, 1, bi_utf8_valid);
  vm.define_native("min", 1, -1, bi_min);
  vm.define_native("max", 1, -1, bi_max);
  vm.define_native("floor", 1, 1, bi_floor);
  vm.define_native("ceil", 1, 1, bi_ceil);
  vm.define_native("sqrt", 1, 1, bi_sqrt);
  vm.define_native("pow", 2, 2, bi_pow);
  vm.define_native("round", 1, 2, bi_round);
  vm.define_native("trunc", 1, 1, bi_trunc);
  vm.define_native("sign", 1, 1, bi_sign);
  vm.define_native("clamp", 3, 3, bi_clamp);
  vm.define_native("sin", 1, 1, bi_sin);
  vm.define_native("cos", 1, 1, bi_cos);
  vm.define_native("tan", 1, 1, bi_tan);
  vm.define_native("asin", 1, 1, bi_asin);
  vm.define_native("acos", 1, 1, bi_acos);
  vm.define_native("atan", 1, 1, bi_atan);
  vm.define_native("atan2", 2, 2, bi_atan2);
  vm.define_native("log", 1, 1, bi_log);
  vm.define_native("log2", 1, 1, bi_log2);
  vm.define_native("log10", 1, 1, bi_log10);
  vm.define_native("exp", 1, 1, bi_exp);
  vm.define_native("hypot", 2, 2, bi_hypot);
  vm.define_native("haversine", 4, 4, bi_haversine);
  vm.define_native("rad", 1, 1, bi_rad);
  vm.define_native("deg", 1, 1, bi_deg);
  vm.define_native("rand", 0, 2, bi_rand);
  vm.define_native("random", 0, 2, bi_rand);
  vm.define_native("seed", 1, 1, bi_seed);
  vm.define_native("pi", 0, 0, bi_pi);
  vm.define_native("e", 0, 0, bi_e);

  // System & Environment
  vm.define_native("getenv", 1, 2, bi_getenv);
  vm.define_native("env", 1, 2, bi_env);
  vm.define_native("secret", 1, 2, bi_secret);
  vm.define_native("setenv", 2, 2, bi_setenv);
  vm.define_native("time", 0, 0, bi_time);
  vm.define_native("time_ms", 0, 0, bi_time_ms);
  vm.define_native("sleep", 1, 1, bi_sleep);
  vm.define_native("is_stopping", 0, 0, bi_is_stopping);
  vm.define_native("should_stop", 0, 0, bi_is_stopping);
  vm.define_native("uuid_v4", 0, 0, bi_uuid_v4);
  vm.define_native("iso8601", 0, 1, bi_iso8601);
  vm.define_native("gc", 0, 0, bi_gc);
  vm.define_native("debugger", 0, 0, bi_debugger);
  vm.define_native("cwd", 0, 0, bi_cwd);

  // Error Handling & Verification
  vm.define_native("assert", 1, 2, bi_assert);
  vm.define_native("assert_eq", 2, 3, bi_assert_eq);
  vm.define_native("assert_ne", 2, 3, bi_assert_ne);
  vm.define_native("assert_true", 1, 2, bi_assert_true);
  vm.define_native("assert_false", 1, 2, bi_assert_false);
  vm.define_native("clock", 0, 0, bi_clock);
  vm.define_native("try_call", 1, -1, bi_try_call);
  vm.define_native("pcall", 1, -1, bi_try_call);
  vm.define_native("__grain_defer", 2, 2, bi_grain_defer);
  vm.define_native("__grain_run_deferred", 2, 2, bi_grain_run_deferred);
  vm.define_native("error", 0, 1, bi_error);
  vm.define_native("panic", 0, 1, bi_error);
  vm.define_native("is_err", 1, 1, bi_is_err);
  vm.define_native("is_ok", 1, 1, bi_is_ok);

  // HTTP Response Helpers
  vm.define_native("res_json", 1, 3, bi_res_json);
  vm.define_native("res_text", 1, 3, bi_res_text);
  vm.define_native("res_html", 1, 3, bi_res_html);
  vm.define_native("res_download", 2, 3, bi_res_download);
  vm.define_native("res_error", 1, 2, bi_res_error);
  vm.define_native("res_redirect", 1, 2, bi_res_redirect);
  vm.define_native("res_sse", 1, 3, bi_res_sse);

  // Structured Data Handling (JSON, Base64, Hex, URL)
  vm.define_native("json_parse", 1, 1, bi_json_parse);
  vm.define_native("json_stringify", 1, 2, bi_json_stringify);
  vm.define_native("base64_encode", 1, 1, bi_base64_encode);
  vm.define_native("base64_decode", 1, 1, bi_base64_decode);
  vm.define_native("hex_encode", 1, 1, bi_hex_encode);
  vm.define_native("hex_decode", 1, 1, bi_hex_decode);
  vm.define_native("url_encode", 1, 1, bi_url_encode);
  vm.define_native("url_decode", 1, 1, bi_url_decode);

  // 2D Visual Rendering & Canvas Primitives
  vm.define_native("canvas_create", 2, 3, bi_canvas_create);
  vm.define_native("canvas_new", 2, 3, bi_canvas_create);
  vm.define_native("cv_create", 2, 3, bi_canvas_create);
  vm.define_native("canvas_width", 1, 1, bi_canvas_width);
  vm.define_native("canvas_get_width", 1, 1, bi_canvas_width);
  vm.define_native("canvas_height", 1, 1, bi_canvas_height);
  vm.define_native("canvas_get_height", 1, 1, bi_canvas_height);
  vm.define_native("canvas_clear", 1, 2, bi_canvas_clear);
  vm.define_native("canvas_get_pixel", 3, 3, bi_canvas_get_pixel);
  vm.define_native("canvas_set_pixel", 4, 4, bi_canvas_set_pixel);
  vm.define_native("canvas_get_pixels", 1, 1, bi_canvas_get_pixels);
  vm.define_native("canvas_set_pixels", 2, 2, bi_canvas_set_pixels);
  vm.define_native("image_decode", 1, 1, bi_image_decode);

  // Styling & Colors
  vm.define_native("canvas_set_fill_style", 2, 2, bi_canvas_set_fill_style);
  vm.define_native("canvas_set_fill_color", 2, 2, bi_canvas_set_fill_style);
  vm.define_native("canvas_set_stroke_style", 2, 2, bi_canvas_set_stroke_style);
  vm.define_native("canvas_set_stroke_color", 2, 2, bi_canvas_set_stroke_style);
  vm.define_native("canvas_set_line_width", 2, 2, bi_canvas_set_line_width);
  vm.define_native("canvas_set_global_alpha", 2, 2, bi_canvas_set_global_alpha);
  vm.define_native("color_rgb", 3, 3, bi_color_rgb);
  vm.define_native("color_rgba", 3, 4, bi_color_rgba);
  vm.define_native("rgb", 3, 3, bi_color_rgb);
  vm.define_native("rgba", 3, 4, bi_color_rgba);
  vm.define_native("color_hex", 1, 1, bi_color_hex);
  vm.define_native("color_to_hex", 1, 1, bi_color_to_hex);
  vm.define_native("color_parse", 1, 1, bi_color_parse);

  // Rectangles
  vm.define_native("canvas_fill_rect", 5, 6, bi_canvas_fill_rect);
  vm.define_native("cv_fill_rect", 5, 6, bi_canvas_fill_rect);
  vm.define_native("canvas_stroke_rect", 5, 7, bi_canvas_stroke_rect);
  vm.define_native("cv_stroke_rect", 5, 7, bi_canvas_stroke_rect);
  vm.define_native("canvas_clear_rect", 5, 5, bi_canvas_clear_rect);
  vm.define_native("canvas_fill_round_rect", 6, 7, bi_canvas_fill_round_rect);
  vm.define_native("canvas_stroke_round_rect", 6, 8, bi_canvas_stroke_round_rect);

  // Circles & Ellipses
  vm.define_native("canvas_fill_circle", 4, 5, bi_canvas_fill_circle);
  vm.define_native("cv_fill_circle", 4, 5, bi_canvas_fill_circle);
  vm.define_native("canvas_stroke_circle", 4, 6, bi_canvas_stroke_circle);
  vm.define_native("cv_stroke_circle", 4, 6, bi_canvas_stroke_circle);
  vm.define_native("canvas_fill_ellipse", 5, 6, bi_canvas_fill_ellipse);
  vm.define_native("canvas_stroke_ellipse", 5, 7, bi_canvas_stroke_ellipse);

  // Lines & Polygons
  vm.define_native("canvas_draw_line", 5, 7, bi_canvas_draw_line);
  vm.define_native("canvas_line", 5, 7, bi_canvas_draw_line);
  vm.define_native("cv_draw_line", 5, 7, bi_canvas_draw_line);
  vm.define_native("canvas_fill_polygon", 2, 3, bi_canvas_fill_polygon);
  vm.define_native("canvas_stroke_polygon", 2, 4, bi_canvas_stroke_polygon);

  // Gradients
  vm.define_native("canvas_create_linear_gradient", 4, 4, bi_canvas_create_linear_gradient);
  vm.define_native("canvas_create_radial_gradient", 3, 3, bi_canvas_create_radial_gradient);
  vm.define_native("canvas_gradient_add_color_stop", 3, 3, bi_canvas_gradient_add_color_stop);
  vm.define_native("gradient_add_stop", 3, 3, bi_canvas_gradient_add_color_stop);
  vm.define_native("canvas_fill_linear_gradient", 7, 8, bi_canvas_fill_linear_gradient);
  vm.define_native("canvas_fill_gradient_v", 6, 6, bi_canvas_fill_gradient_v);
  vm.define_native("canvas_fill_gradient_h", 6, 6, bi_canvas_fill_gradient_h);

  // Paths
  vm.define_native("canvas_begin_path", 1, 1, bi_canvas_begin_path);
  vm.define_native("cv_begin_path", 1, 1, bi_canvas_begin_path);
  vm.define_native("canvas_move_to", 3, 3, bi_canvas_move_to);
  vm.define_native("cv_move_to", 3, 3, bi_canvas_move_to);
  vm.define_native("canvas_line_to", 3, 3, bi_canvas_line_to);
  vm.define_native("cv_line_to", 3, 3, bi_canvas_line_to);
  vm.define_native("canvas_close_path", 1, 1, bi_canvas_close_path);
  vm.define_native("cv_close_path", 1, 1, bi_canvas_close_path);
  vm.define_native("canvas_arc", 5, 7, bi_canvas_arc);
  vm.define_native("canvas_bezier_curve_to", 7, 7, bi_canvas_bezier_curve_to);
  vm.define_native("canvas_quadratic_curve_to", 5, 5, bi_canvas_quadratic_curve_to);
  vm.define_native("canvas_fill", 1, 2, bi_canvas_fill);
  vm.define_native("cv_fill", 1, 2, bi_canvas_fill);
  vm.define_native("canvas_stroke", 1, 3, bi_canvas_stroke);
  vm.define_native("cv_stroke", 1, 3, bi_canvas_stroke);

  // State & Transforms
  vm.define_native("canvas_save", 1, 1, bi_canvas_save);
  vm.define_native("canvas_restore", 1, 1, bi_canvas_restore);
  vm.define_native("canvas_translate", 3, 3, bi_canvas_translate);
  vm.define_native("canvas_scale", 2, 3, bi_canvas_scale);
  vm.define_native("canvas_rotate", 2, 2, bi_canvas_rotate);

  // Font rendering & text
  vm.define_native("canvas_set_font", 2, 3, bi_canvas_set_font);
  vm.define_native("canvas_fill_text", 4, 7, bi_canvas_fill_text);
  vm.define_native("canvas_draw_text", 4, 7, bi_canvas_draw_text);
  vm.define_native("canvas_stroke_text", 4, 7, bi_canvas_stroke_text);
  vm.define_native("cv_draw_text", 4, 7, bi_canvas_draw_text);
  vm.define_native("canvas_measure_text", 1, 4, bi_canvas_measure_text);

  // Framebuffer, export, presentation & blitting
  vm.define_native("canvas_to_ppm", 1, 1, bi_canvas_to_ppm);
  vm.define_native("cv_to_ppm", 1, 1, bi_canvas_to_ppm);
  vm.define_native("canvas_save_ppm", 2, 2, bi_canvas_save_ppm);
  vm.define_native("canvas_to_bytes", 1, 2, bi_canvas_to_bytes);
  vm.define_native("canvas_to_png", 1, 1, bi_canvas_to_png);
  vm.define_native("canvas_save_png", 2, 2, bi_canvas_save_png);
  vm.define_native("canvas_blit", 3, 9, bi_canvas_blit);
  vm.define_native("canvas_draw_image", 3, 9, bi_canvas_blit);
  vm.define_native("canvas_present", 1, 2, bi_canvas_present);

  // Advanced Color Science & Palettes
  vm.define_native("color_hsl", 3, 3, bi_color_hsl);
  vm.define_native("color_hsla", 3, 4, bi_color_hsla);
  vm.define_native("color_to_hsl", 1, 1, bi_color_to_hsl);
  vm.define_native("color_lighten", 1, 2, bi_color_lighten);
  vm.define_native("color_darken", 1, 2, bi_color_darken);
  vm.define_native("color_blend", 2, 3, bi_color_blend);
  vm.define_native("color_invert", 1, 1, bi_color_invert);
  vm.define_native("color_luminance", 1, 1, bi_color_luminance);
  vm.define_native("color_contrast", 2, 2, bi_color_contrast);
  vm.define_native("palette_analogous", 1, 3, bi_palette_analogous);
  vm.define_native("palette_triadic", 1, 1, bi_palette_triadic);
  vm.define_native("palette_monochromatic", 1, 2, bi_palette_monochromatic);

  // Animation, Motion & Physics
  vm.define_native("lerp", 3, 3, bi_lerp);
  vm.define_native("smoothstep", 3, 3, bi_smoothstep);
  vm.define_native("ease_in", 1, 2, bi_ease_in);
  vm.define_native("ease_out", 1, 2, bi_ease_out);
  vm.define_native("ease_in_out", 1, 2, bi_ease_in_out);
  vm.define_native("spring", 6, 6, bi_spring);

  // Filters, Shadows & Effects
  vm.define_native("canvas_blur", 1, 2, bi_canvas_blur);
  vm.define_native("canvas_draw_shadow", 5, 10, bi_canvas_draw_shadow);
  vm.define_native("canvas_draw_glow", 4, 6, bi_canvas_draw_glow);
  vm.define_native("canvas_draw_glass", 5, 9, bi_canvas_draw_glass);

  // High Quality Application UI Widgets & Data Visualizations
  vm.define_native("draw_card", 5, 9, bi_draw_card);
  vm.define_native("draw_badge", 4, 7, bi_draw_badge);
  vm.define_native("draw_button", 6, 8, bi_draw_button);
  vm.define_native("draw_conflict", 6, 7, bi_draw_conflict);
  vm.define_native("draw_progress_bar", 6, 9, bi_draw_progress_bar);
  vm.define_native("draw_sparkline", 6, 9, bi_draw_sparkline);
  vm.define_native("draw_bar_chart", 6, 9, bi_draw_bar_chart);
  vm.define_native("draw_donut_chart", 6, 7, bi_draw_donut_chart);
  vm.define_native("draw_metric_tile", 7, 11, bi_draw_metric_tile);
  vm.define_native("draw_icon", 5, 7, bi_draw_icon);

  // Full Navicat-Class Application UI Widgets
  vm.define_native("draw_window_frame", 6, 8, bi_draw_window_frame);
  vm.define_native("draw_menu_bar", 5, 7, bi_draw_menu_bar);
  vm.define_native("draw_toolbar", 5, 7, bi_draw_toolbar);
  vm.define_native("draw_nav_tree", 6, 9, bi_draw_nav_tree);
  vm.define_native("draw_tab_bar", 5, 7, bi_draw_tab_bar);
  vm.define_native("draw_sql_editor", 6, 10, bi_draw_sql_editor);
  vm.define_native("draw_data_grid", 7, 17, bi_draw_data_grid);
  vm.define_native("draw_table_designer", 6, 9, bi_draw_table_designer);
  vm.define_native("draw_connection_dialog", 6, 8, bi_draw_connection_dialog);
  vm.define_native("draw_query_plan", 6, 7, bi_draw_query_plan);
  vm.define_native("draw_status_bar", 9, 11, bi_draw_status_bar);
  vm.define_native("draw_autocomplete_popup", 6, 7, bi_draw_autocomplete_popup);

  // SQL Parsing, Formatting, Schema DDL, Typeahead & Data Export
  vm.define_native("sql_tokenize", 1, 1, bi_sql_tokenize);
  vm.define_native("sql_format", 1, 1, bi_sql_format);
  vm.define_native("sql_generate_ddl", 2, 2, bi_sql_generate_ddl);
  vm.define_native("sql_suggest", 1, 4, bi_sql_suggest);
  vm.define_native("data_to_csv", 2, 2, bi_data_to_csv);
  vm.define_native("data_to_json", 2, 2, bi_data_to_json);
  vm.define_native("data_to_sql_insert", 3, 3, bi_data_to_sql_insert);

  // Sockets and sys_exec are NOT here: this set is every grain VM's - the
  // Pi's kernel (no BSD sockets, no processes) and the sidecar's sandboxed
  // aios_grain_run included. They are host capabilities, installed by the
  // `aios grain` command only (cli/grain_host.cpp).
  vm.define_native("time_us", 0, 0, bi_time_us);  vm.define_native("time_us", 0, 0, bi_time_us);

  // Files (through Vm::files - see grain.hpp)
  vm.define_native("read_file", 1, 1, bi_read_file);
  vm.define_native("write_file", 2, 2, bi_write_file);
  vm.define_native("read_bytes", 1, 1, bi_read_bytes);
  vm.define_native("write_bytes", 2, 2, bi_write_bytes);
  vm.define_native("append_file", 2, 2, bi_append_file);
  vm.define_native("list_dir", 0, 1, bi_list_dir);
  vm.define_native("delete_file", 1, 1, bi_delete_file);
  vm.define_native("file_stat", 1, 1, bi_file_stat);
  vm.define_native("rename_file", 2, 2, bi_rename_file);
  vm.define_native("copy_file", 2, 2, bi_copy_file);
  vm.define_native("glob", 1, 1, bi_glob);
  vm.define_native("file_exists", 1, 1, bi_file_exists);

  // AIOS Substrate Primitives
  vm.define_native("__entity_assemble", 3, 3, bi_entity_assemble);
  vm.define_native("id128", 0, 2, bi_id128);
  vm.define_native("Capability", 0, 4, bi_Capability);
  vm.define_native("capability", 0, 4, bi_Capability);
  vm.define_native("cap_mint", 0, 4, bi_Capability);
  vm.define_native("StateChannel", 0, 2, bi_StateChannel);
  vm.define_native("channel_create", 0, 2, bi_StateChannel);
  vm.define_native("StateFrame", 1, 3, bi_StateFrame);
  vm.define_native("state_frame", 1, 3, bi_StateFrame);
  vm.define_native("memory_allocate", 1, 2, bi_memory_allocate);
  vm.define_native("MemoryObject", 1, 2, bi_memory_allocate);

  // Architecture Constants & Substrate Globals
  vm.define_global("read_only", Value::integer(1));
  vm.define_global("write_only", Value::integer(2));
  vm.define_global("read_write", Value::integer(3));
  vm.define_global("grant", Value::integer(8));
  vm.define_global("all_rights", Value::integer(63));
  vm.define_global("never", Value::integer(0));
  vm.define_global("shared_mem", Value::string("shared_mem"));
  vm.define_global("ram", Value::string("ram"));
  vm.define_global("persistent", Value::string("persistent"));
  vm.define_global("current_generation", Value::integer(1));

  install_window_builtins(vm);
}

struct GrainPane {
  uint64_t id = 0;
  std::string title;
  int x = 0;
  int y = 0;
  int width = 800;
  int height = 600;
  bool visible = true;
  bool focused = false;
  std::shared_ptr<GrainCanvas> canvas;
  Value draw_cb;
  Value event_cb;
  Value click_cb;
  Value key_cb;
  Value custom_data;
  std::shared_ptr<PaneDriver> driver;   // set: native code draws it and takes its events
  uint32_t compositor_window_id = 0;
  uint64_t owner = 0;   // the serial of the VM that registered it: its callbacks are that VM's closures
};

// Every VM's panes, each tagged with its owner. Inside aios several VMs live at
// once (a desktop app, `aios grain run` on the command threads), so the list
// is locked, and every pane call sees only its own VM's panes: one VM must
// never run another's callbacks.
static std::vector<std::shared_ptr<GrainPane>> g_grain_panes;
static uint64_t g_next_grain_pane_id = 1;
static aios::hal::Mutex g_grain_panes_mu;

static std::vector<std::shared_ptr<GrainPane>> panes_of(const Vm& vm) {
  std::lock_guard<aios::hal::Mutex> lk(g_grain_panes_mu);
  std::vector<std::shared_ptr<GrainPane>> out;
  for (const auto& p : g_grain_panes)
    if (p->owner == vm.serial()) out.push_back(p);
  return out;
}

static void add_pane(const std::shared_ptr<GrainPane>& pane) {
  std::lock_guard<aios::hal::Mutex> lk(g_grain_panes_mu);
  pane->id = g_next_grain_pane_id++;
  g_grain_panes.push_back(pane);
}

void drop_panes(const Vm& vm) {
  std::lock_guard<aios::hal::Mutex> lk(g_grain_panes_mu);
  const uint64_t owner = vm.serial();
  g_grain_panes.erase(std::remove_if(g_grain_panes.begin(), g_grain_panes.end(),
                                     [owner](const std::shared_ptr<GrainPane>& p) { return p->owner == owner; }),
                      g_grain_panes.end());
}

size_t pane_count() {
  std::lock_guard<aios::hal::Mutex> lk(g_grain_panes_mu);
  return g_grain_panes.size();
}

static bool remove_pane(const std::shared_ptr<GrainPane>& pane) {
  std::lock_guard<aios::hal::Mutex> lk(g_grain_panes_mu);
  const auto before = g_grain_panes.size();
  g_grain_panes.erase(std::remove(g_grain_panes.begin(), g_grain_panes.end(), pane), g_grain_panes.end());
  return g_grain_panes.size() != before;
}

static std::shared_ptr<MapObj> pane_to_map(const std::shared_ptr<GrainPane>& pane) {
  auto map = std::make_shared<MapObj>();
  map->put("id", Value::integer((int64_t)pane->id));
  map->put("title", Value::string(pane->title));
  map->put("x", Value::integer(pane->x));
  map->put("y", Value::integer(pane->y));
  map->put("width", Value::integer(pane->width));
  map->put("height", Value::integer(pane->height));
  map->put("visible", Value::boolean(pane->visible));
  map->put("focused", Value::boolean(pane->focused));
  if (pane->canvas) {
    map->put("canvas", Value::object(Type::Extern, std::make_shared<ExternObj>("canvas", pane->canvas)));
  } else {
    map->put("canvas", Value::nil());
  }
  map->put("on_draw", pane->draw_cb);
  map->put("on_event", pane->event_cb);
  map->put("on_click", pane->click_cb);
  map->put("on_key", pane->key_cb);
  map->put("data", pane->custom_data);
  map->put("handle", Value::object(Type::Extern, std::make_shared<ExternObj>("grain_pane", pane)));
  return map;
}

static std::shared_ptr<GrainPane> get_pane_from_value(const Vm& vm, const Value& v) {
  uint64_t target_id = 0;
  if (v.type == Type::Extern && v.obj) {
    auto ext = std::dynamic_pointer_cast<ExternObj>(v.obj);
    if (ext && ext->kind_name == "grain_pane") target_id = std::static_pointer_cast<GrainPane>(ext->ptr)->id;
  } else if (v.type == Type::Int) {
    target_id = (uint64_t)v.i;
  } else if (v.type == Type::Str) {
    // strtoull, not stoull: bare metal builds with exceptions off.
    const std::string digits = v.str();
    char* end = nullptr;
    const unsigned long long n = std::strtoull(digits.c_str(), &end, 10);
    if (end != digits.c_str()) target_id = (uint64_t)n;
  } else if (v.type == Type::Map && v.obj) {
    auto m = std::dynamic_pointer_cast<MapObj>(v.obj);
    if (m) {
      Value* id_val = m->find("id");
      if (id_val && id_val->type == Type::Int) target_id = (uint64_t)id_val->i;
    }
  }
  // A pane that is not registered (never was, closed since, or another VM's) is
  // no pane: falling back to the newest one closed or redrew a window nobody named.
  for (const auto& p : panes_of(vm)) {
    if (p->id == target_id) return p;
  }
  return nullptr;
}

// Calls a pane callback with (first, second), or (first) when it takes one.
static Value call_pane_cb(Vm& vm, const Value& cb, const Value& first, const Value& second, std::string& error) {
  if (callable_arity(cb) == 1) return vm.call(cb, {first}, error);
  return vm.call(cb, {first, second}, error);
}

Value bi_register_pane(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (args.empty()) {
    error = "register_pane(): expects options map or (title, [width, height], draw_cb, [event_cb])";
    return Value::nil();
  }

  auto pane = std::make_shared<GrainPane>();
  pane->owner = vm.serial();
  pane->title.clear();
  pane->width = 800;
  pane->height = 600;
  pane->x = 50;
  pane->y = 50;

  if (args[0].type == Type::Map && args[0].obj) {
    auto m = std::dynamic_pointer_cast<MapObj>(args[0].obj);
    if (m) {
      // the first of these names the map has
      auto pick = [&](std::initializer_list<const char*> names) -> Value* {
        for (const char* n : names)
          if (Value* v = m->find(n)) return v;
        return nullptr;
      };
      if (Value* v = pick({"title", "name"})) pane->title = v->str();
      if (Value* v = pick({"width", "w"})) pane->width = (int)v->as_int();
      if (Value* v = pick({"height", "h"})) pane->height = (int)v->as_int();
      if (Value* v = pick({"x"})) pane->x = (int)v->as_int();
      if (Value* v = pick({"y"})) pane->y = (int)v->as_int();

      if (Value* v = pick({"on_draw", "draw", "render"})) pane->draw_cb = *v;
      if (Value* v = pick({"on_event", "event"})) pane->event_cb = *v;
      if (Value* v = pick({"on_click", "click"})) pane->click_cb = *v;
      if (Value* v = pick({"on_key", "key"})) pane->key_cb = *v;

      if (Value* v = m->find("data")) pane->custom_data = *v;
    }
  } else {
    size_t idx = 0;
    if (args[idx].type == Type::Str) {
      pane->title = args[idx++].str();
    }
    if (idx < args.size() && args[idx].is_number() && idx + 1 < args.size() && args[idx + 1].is_number()) {
      pane->width = (int)args[idx++].as_int();
      pane->height = (int)args[idx++].as_int();
    }
    if (idx < args.size()) {
      pane->draw_cb = args[idx++];
    }
    if (idx < args.size()) {
      pane->event_cb = args[idx++];
    }
  }

  if (pane->width <= 0) pane->width = 800;
  if (pane->height <= 0) pane->height = 600;

  pane->canvas = std::make_shared<GrainCanvas>();
  pane->canvas->width = (uint32_t)pane->width;
  pane->canvas->height = (uint32_t)pane->height;
  pane->canvas->pixels.resize((size_t)pane->width * pane->height, 0x00000000);

  add_pane(pane);
  if (pane->title.empty()) pane->title = "Grain Pane " + std::to_string(pane->id);
  return Value::object(Type::Map, pane_to_map(pane));
}

Value bi_unregister_pane(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (args.empty()) { error = "unregister_pane(): wants pane ID or map"; return Value::boolean(false); }
  auto pane = get_pane_from_value(vm, args[0]);
  if (!pane) { error = "unregister_pane(): pane not found"; return Value::boolean(false); }
  return Value::boolean(remove_pane(pane));
}

Value bi_list_panes(Vm& vm, std::vector<Value>&, std::string&) {
  std::vector<Value> list;
  for (const auto& pane : panes_of(vm)) {
    list.push_back(Value::object(Type::Map, pane_to_map(pane)));
  }
  return Value::list(std::move(list));
}

Value bi_pane_render(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (args.empty()) { error = "pane_render(): wants pane ID or handle"; return Value::nil(); }
  auto pane = get_pane_from_value(vm, args[0]);
  if (!pane) { error = "pane_render(): pane not found"; return Value::nil(); }

  std::shared_ptr<GrainCanvas> cv = pane->canvas;
  if (args.size() > 1) {
    auto custom_cv = get_canvas(args[1], error, "pane_render");
    if (custom_cv) cv = custom_cv;
  }
  if (!cv) {
    cv = std::make_shared<GrainCanvas>();
    cv->width = (uint32_t)pane->width;
    cv->height = (uint32_t)pane->height;
    cv->pixels.resize((size_t)pane->width * pane->height, 0x00000000);
    pane->canvas = cv;
  }

  Value cv_val = Value::object(Type::Extern, std::make_shared<ExternObj>("canvas", cv));
  Value pane_map_val = Value::object(Type::Map, pane_to_map(pane));

  if (pane->driver) {
    std::string err;
    if (!pane->driver->draw(vm, cv->pixels, (int)cv->width, (int)cv->height, err)) {
      error = "pane_render(): " + err;
      return Value::nil();
    }
  } else if (pane->draw_cb.type == Type::Fn || pane->draw_cb.type == Type::Native) {
    std::string err;
    call_pane_cb(vm, pane->draw_cb, cv_val, pane_map_val, err);
    if (!err.empty()) {
      error = "pane_render(): the draw callback failed: " + err;
      return Value::nil();
    }
  }

  return cv_val;
}

Value bi_pane_dispatch_event(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (args.size() < 2) { error = "pane_dispatch_event(): wants (pane, event)"; return Value::boolean(false); }
  auto pane = get_pane_from_value(vm, args[0]);
  if (!pane) { error = "pane_dispatch_event(): pane not found"; return Value::boolean(false); }

  std::shared_ptr<MapObj> ev_map;
  if (args[1].type == Type::Map && args[1].obj) {
    ev_map = std::dynamic_pointer_cast<MapObj>(args[1].obj);
  } else if (args[1].type == Type::Str) {
    ev_map = std::make_shared<MapObj>();
    ev_map->put("type", args[1]);
  } else {
    ev_map = std::make_shared<MapObj>();
    ev_map->put("type", Value::string("custom"));
  }

  if (args.size() > 2 && args[2].type == Type::Map && args[2].obj) {
    auto extra = std::dynamic_pointer_cast<MapObj>(args[2].obj);
    if (extra) {
      for (const auto& entry : extra->entries) {
        ev_map->put(entry.first, entry.second);
      }
    }
  }

  Value ev_val = Value::object(Type::Map, ev_map);
  Value pane_map_val = Value::object(Type::Map, pane_to_map(pane));
  Value last_result = Value::boolean(true);

  std::string ev_type;
  Value* type_v = ev_map->find("type");
  if (type_v && type_v->type == Type::Str) ev_type = type_v->str();

  // Callbacks take (pane, event) or just (event). One that raises an error
  // stops the dispatch and the error is the caller's, not swallowed.
  auto deliver = [&](const Value& cb, const char* which) -> bool {
    if (cb.type != Type::Fn && cb.type != Type::Native) return true;
    std::string err;
    Value res = callable_arity(cb) == 1 ? vm.call(cb, {ev_val}, err) : vm.call(cb, {pane_map_val, ev_val}, err);
    if (!err.empty()) { error = std::string("pane_dispatch_event(): the ") + which + " callback failed: " + err; return false; }
    if (res.type != Type::Nil) last_result = res;
    return true;
  };
  if (pane->driver) {
    std::string err;
    if (!pane->driver->event(vm, ev_val, err)) { error = "pane_dispatch_event(): " + err; return Value::nil(); }
  }
  if (!deliver(pane->event_cb, "event")) return Value::nil();
  if (ev_type == "click" && !deliver(pane->click_cb, "click")) return Value::nil();
  if (ev_type == "key" && !deliver(pane->key_cb, "key")) return Value::nil();

  return last_result;
}

Value bi_pane_get_canvas(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (args.empty()) { error = "pane_get_canvas(): wants pane"; return Value::nil(); }
  auto pane = get_pane_from_value(vm, args[0]);
  if (!pane) { error = "pane_get_canvas(): pane not found"; return Value::nil(); }
  return Value::object(Type::Extern, std::make_shared<ExternObj>("canvas", pane->canvas));
}

Value bi_pane_set_bounds(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (args.size() < 3) { error = "pane_set_bounds(): wants (pane, w, h) or (pane, x, y, w, h)"; return Value::nil(); }
  auto pane = get_pane_from_value(vm, args[0]);
  if (!pane) { error = "pane_set_bounds(): pane not found"; return Value::nil(); }

  if (args.size() >= 5) {
    pane->x = (int)args[1].as_int();
    pane->y = (int)args[2].as_int();
    pane->width = (int)args[3].as_int();
    pane->height = (int)args[4].as_int();
  } else {
    pane->width = (int)args[1].as_int();
    pane->height = (int)args[2].as_int();
  }

  if (pane->width <= 0) pane->width = 100;
  if (pane->height <= 0) pane->height = 100;

  if (pane->canvas) {
    pane->canvas->width = (uint32_t)pane->width;
    pane->canvas->height = (uint32_t)pane->height;
    pane->canvas->pixels.resize((size_t)pane->width * pane->height, 0x00000000);
  }
  return Value::object(Type::Map, pane_to_map(pane));
}

Value bi_pane_get(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (args.empty()) { error = "pane_get(): wants pane"; return Value::nil(); }
  auto pane = get_pane_from_value(vm, args[0]);
  if (!pane) { error = "pane_get(): pane not found"; return Value::nil(); }
  return Value::object(Type::Map, pane_to_map(pane));
}

Value register_driven_pane(Vm& vm, const std::string& title, int width, int height, std::shared_ptr<PaneDriver> driver) {
  auto pane = std::make_shared<GrainPane>();
  pane->owner = vm.serial();
  pane->width = width > 0 ? width : 800;
  pane->height = height > 0 ? height : 600;
  pane->driver = std::move(driver);
  pane->canvas = std::make_shared<GrainCanvas>();
  pane->canvas->width = (uint32_t)pane->width;
  pane->canvas->height = (uint32_t)pane->height;
  pane->canvas->pixels.assign((size_t)pane->width * pane->height, 0x00000000);
  add_pane(pane);
  pane->title = title.empty() ? "Grain Pane " + std::to_string(pane->id) : title;
  return Value::object(Type::Map, pane_to_map(pane));
}

std::shared_ptr<PaneDriver> pane_driver(const Vm& vm, const Value& pane) {
  auto p = get_pane_from_value(vm, pane);
  return p ? p->driver : nullptr;
}

// --- the panes as a display sees them --------------------------------------

static Display* g_display = nullptr;

void set_display(Display* d) { g_display = d; }
Display* display() { return g_display; }

std::vector<PaneInfo> pane_list(const Vm& vm) {
  std::vector<PaneInfo> out;
  for (const auto& p : panes_of(vm)) {
    if (!p->visible) continue;
    PaneInfo info;
    info.id = p->id;
    info.title = p->title;
    info.x = p->x;
    info.y = p->y;
    info.width = p->width;
    info.height = p->height;
    out.push_back(std::move(info));
  }
  return out;
}

static std::shared_ptr<GrainPane> pane_by_id(const Vm& vm, uint64_t id) {
  for (const auto& p : panes_of(vm))
    if (p->id == id) return p;
  return nullptr;
}

bool pane_frame(Vm& vm, uint64_t id, std::vector<uint32_t>& argb, int& width, int& height, std::string& error) {
  auto pane = pane_by_id(vm, id);
  if (!pane) { error = "pane " + std::to_string(id) + " is not registered"; return false; }
  if (!pane->canvas) {
    pane->canvas = std::make_shared<GrainCanvas>();
    pane->canvas->width = (uint32_t)pane->width;
    pane->canvas->height = (uint32_t)pane->height;
    pane->canvas->pixels.assign((size_t)pane->width * pane->height, 0x00000000);
  }
  if (pane->driver) {
    const auto cv = pane->canvas;
    if (!pane->driver->draw(vm, cv->pixels, (int)cv->width, (int)cv->height, error)) {
      error = "\"" + pane->title + "\": " + error;
      return false;
    }
  } else if (pane->draw_cb.type == Type::Fn || pane->draw_cb.type == Type::Native) {
    Value cv_val = Value::object(Type::Extern, std::make_shared<ExternObj>("canvas", pane->canvas));
    call_pane_cb(vm, pane->draw_cb, cv_val, Value::object(Type::Map, pane_to_map(pane)), error);
    if (!error.empty()) { error = "\"" + pane->title + "\" draw callback: " + error; return false; }
  }
  const auto cv = pane->canvas;   // the callback may have closed the pane; its canvas is still this frame's
  width = (int)cv->width;
  height = (int)cv->height;
  argb.assign(cv->pixels.begin(), cv->pixels.end());
  return true;
}

bool pane_event(Vm& vm, uint64_t id, const Value& event, std::string& error) {
  auto pane = pane_by_id(vm, id);
  if (!pane) return true;   // closed by an earlier event of the same batch: nothing to deliver to
  std::vector<Value> args{Value::object(Type::Extern, std::make_shared<ExternObj>("grain_pane", pane)), event};
  bi_pane_dispatch_event(vm, args, error);
  return error.empty();
}

bool pane_resized(Vm& vm, uint64_t id, int width, int height, std::string& error) {
  auto pane = pane_by_id(vm, id);
  if (!pane) return true;
  if (width < 1 || height < 1) return true;   // minimised: keep the last size
  pane->width = width;
  pane->height = height;
  if (!pane->canvas) pane->canvas = std::make_shared<GrainCanvas>();
  pane->canvas->width = (uint32_t)width;
  pane->canvas->height = (uint32_t)height;
  pane->canvas->pixels.assign((size_t)width * height, 0x00000000);
  auto ev = std::make_shared<MapObj>();
  ev->put("type", Value::string("resize"));
  ev->put("width", Value::integer(width));
  ev->put("height", Value::integer(height));
  return pane_event(vm, id, Value::object(Type::Map, ev), error);
}

bool pane_closed(Vm& vm, uint64_t id, std::string& error) {
  auto pane = pane_by_id(vm, id);
  if (!pane) return true;
  auto ev = std::make_shared<MapObj>();
  ev->put("type", Value::string("close"));
  const bool ok = pane_event(vm, id, Value::object(Type::Map, ev), error);
  remove_pane(pane);
  return ok;
}

// run_panes([options]): shows every registered pane on the embedder's display
// until each is closed (by its window, or by the program unregistering it).
// options: frames (stop after that many), script (a list of input events played
// first, each with a "pane" naming its target).
Value bi_run_panes(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (vm.worker) { error = "run_panes(): a worker (parallel_map, spawn) has no windows; show panes from the program itself"; return Value::nil(); }
  if (!g_display) {
    error = "run_panes(): this grain has no display (the standalone grain shows windows on Linux, Windows, and on macOS when built with SDL2)";
    return Value::nil();
  }
  DisplayRun run;
  if (!args.empty() && args[0].type != Type::Nil) {
    if (args[0].type != Type::Map) { error = std::string("run_panes() wants its options as a map, got a ") + type_name(args[0].type); return Value::nil(); }
    auto* m = static_cast<MapObj*>(args[0].obj.get());
    if (Value* v = m->find("frames")) {
      if (!want_number(*v, error, "run_panes")) return Value::nil();
      run.max_frames = v->as_int();
    }
    if (Value* v = m->find("script")) {
      if (!want_list(*v, error, "run_panes")) return Value::nil();
      for (const auto& e : v->list_items()) {
        if (e.type != Type::Map) { error = "run_panes(): each scripted event is a map"; return Value::nil(); }
        run.script.push_back(e);
      }
    }
  }
  if (!g_display->run(vm, run, error)) return Value::nil();
  return Value::boolean(true);
}

void install_window_builtins(Vm& vm) {
  vm.define_native("run_panes", 0, 1, bi_run_panes);
  install_face_builtins(vm);
  vm.define_native("register_pane", 1, 6, bi_register_pane);
  vm.define_native("pane_register", 1, 6, bi_register_pane);
  vm.define_native("window_register", 1, 6, bi_register_pane);
  vm.define_native("canvas_register_pane", 1, 6, bi_register_pane);
  vm.define_native("create_pane", 1, 6, bi_register_pane);
  vm.define_native("register_window", 1, 6, bi_register_pane);

  vm.define_native("unregister_pane", 1, 1, bi_unregister_pane);
  vm.define_native("pane_unregister", 1, 1, bi_unregister_pane);
  vm.define_native("window_unregister", 1, 1, bi_unregister_pane);
  vm.define_native("window_close", 1, 1, bi_unregister_pane);

  vm.define_native("list_panes", 0, 0, bi_list_panes);
  vm.define_native("pane_list", 0, 0, bi_list_panes);
  vm.define_native("window_list", 0, 0, bi_list_panes);

  vm.define_native("pane_render", 1, 2, bi_pane_render);
  vm.define_native("window_render", 1, 2, bi_pane_render);
  vm.define_native("pane_draw", 1, 2, bi_pane_render);

  vm.define_native("pane_dispatch_event", 2, 3, bi_pane_dispatch_event);
  vm.define_native("window_dispatch_event", 2, 3, bi_pane_dispatch_event);
  vm.define_native("pane_trigger_event", 2, 3, bi_pane_dispatch_event);
  vm.define_native("pane_emit_event", 2, 3, bi_pane_dispatch_event);
  vm.define_native("pane_on_event", 2, 3, bi_pane_dispatch_event);

  vm.define_native("pane_get_canvas", 1, 1, bi_pane_get_canvas);
  vm.define_native("window_get_canvas", 1, 1, bi_pane_get_canvas);

  vm.define_native("pane_set_bounds", 3, 5, bi_pane_set_bounds);
  vm.define_native("window_set_bounds", 3, 5, bi_pane_set_bounds);
  vm.define_native("pane_resize", 3, 3, bi_pane_set_bounds);

  vm.define_native("pane_get", 1, 1, bi_pane_get);
  vm.define_native("window_get", 1, 1, bi_pane_get);
}

}  // namespace aios::userland::grain

