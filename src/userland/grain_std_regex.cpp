// grain's regular expressions: the JavaScript engine's matcher (src/js/regexp),
// so a pattern means in grain exactly what it means in ECMAScript - classes,
// lookaround, named groups, backreferences, \p{...} with the "u" flag - and a
// pattern that would backtrack without end is refused with an error rather
// than hanging the program.
//
//   regex_test(pattern, text [, flags])               -> true / false
//   regex_match(pattern, text [, flags])              -> the first match, or nil
//   regex_find_all(pattern, text [, flags])           -> every match, in order
//   regex_replace(pattern, text, with [, flags])      -> text with every match replaced
//   regex_split(pattern, text [, flags])              -> the pieces between matches
//
// flags: any of i (ignore case), m (^ and $ at line ends), s (. matches a line
// end), u (code points and \p{...}). A match is {text, start, end, groups,
// named}: start and end are byte offsets into the text, as len, slice and find
// count; groups holds each group's text (nil for one that did not take part),
// named the named groups'. `with` is a string - $& the match, $1..$99 a group,
// $<name> a named one, $$ a dollar - or a function given the match and
// returning the replacement.
//
// Matching runs on UTF-16, as the engine does; the text is converted once per
// call and every offset handed back is mapped to the text's own bytes, which
// are copied, never re-encoded, so bytes that are not valid UTF-8 survive a
// replace untouched.
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../js/regexp.hpp"
#include "aios/userland/grain.hpp"
#include "grain_builtins_internal.hpp"

namespace aios::userland::grain {

namespace {

namespace re = aios::js::re;

// UTF-8 bytes to UTF-16, with where each code unit started in the bytes
// (one entry per unit, plus the end). A byte that does not begin a valid
// sequence becomes U+FFFD on its own, so the mapping never loses a byte.
struct Utf16Text {
  std::u16string units;
  std::vector<size_t> byte_at;   // units.size() + 1 entries
};

Utf16Text to_utf16(const std::string& s) {
  Utf16Text t;
  t.units.reserve(s.size());
  t.byte_at.reserve(s.size() + 1);
  const auto* b = reinterpret_cast<const unsigned char*>(s.data());
  const size_t n = s.size();
  size_t i = 0;
  while (i < n) {
    uint32_t cp = 0xFFFD;
    size_t len = 1;
    const unsigned char c = b[i];
    auto cont = [&](size_t k) { return i + k < n && (b[i + k] & 0xC0) == 0x80; };
    if (c < 0x80) {
      cp = c;
    } else if (c >= 0xC2 && c <= 0xDF && cont(1)) {
      cp = ((c & 0x1Fu) << 6) | (b[i + 1] & 0x3Fu);
      len = 2;
    } else if (c >= 0xE0 && c <= 0xEF && cont(1) && cont(2)) {
      const uint32_t v = ((c & 0x0Fu) << 12) | ((b[i + 1] & 0x3Fu) << 6) | (b[i + 2] & 0x3Fu);
      if (v >= 0x800 && (v < 0xD800 || v > 0xDFFF)) { cp = v; len = 3; }
    } else if (c >= 0xF0 && c <= 0xF4 && cont(1) && cont(2) && cont(3)) {
      const uint32_t v = ((c & 0x07u) << 18) | ((b[i + 1] & 0x3Fu) << 12) | ((b[i + 2] & 0x3Fu) << 6) | (b[i + 3] & 0x3Fu);
      if (v >= 0x10000 && v <= 0x10FFFF) { cp = v; len = 4; }
    }
    if (cp >= 0x10000) {
      cp -= 0x10000;
      t.units.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
      t.byte_at.push_back(i);
      t.units.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
      t.byte_at.push_back(i + len);   // the low half has no byte of its own: a match cannot split the pair in bytes
    } else {
      t.units.push_back(static_cast<char16_t>(cp));
      t.byte_at.push_back(i);
    }
    i += len;
  }
  t.byte_at.push_back(n);
  return t;
}

std::u16string ascii16(const std::string& s) {
  std::u16string out;
  out.reserve(s.size());
  for (unsigned char c : s) out.push_back(static_cast<char16_t>(c));
  return out;
}

std::string utf8_of(const std::u16string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    uint32_t cp = s[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
      ++i;
    }
    if (cp < 0x80) out.push_back(static_cast<char>(cp));
    else if (cp < 0x800) { out.push_back(static_cast<char>(0xC0 | (cp >> 6))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) { out.push_back(static_cast<char>(0xE0 | (cp >> 12))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
    else { out.push_back(static_cast<char>(0xF0 | (cp >> 18))); out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
  }
  return out;
}

// Compiled patterns, per thread (VMs inside aios run on several), most recent
// first. A program that loops over the same few patterns compiles each once.
struct Cache {
  static constexpr size_t kMax = 64;
  std::list<std::pair<std::string, std::shared_ptr<const re::Program>>> order;
  std::unordered_map<std::string, decltype(order)::iterator> index;
};

std::shared_ptr<const re::Program> compiled(const std::string& pattern, const std::string& flags, const char* who, std::string& error) {
  for (char f : flags) {
    if (f != 'i' && f != 'm' && f != 's' && f != 'u') {
      error = std::string(who) + "(): unknown flag '" + f + "' (flags are i, m, s, u)";
      return nullptr;
    }
  }
  thread_local Cache cache;
  const std::string key = flags + '\x1f' + pattern;
  if (auto it = cache.index.find(key); it != cache.index.end()) {
    cache.order.splice(cache.order.begin(), cache.order, it->second);
    return it->second->second;
  }
  std::string why;
  const Utf16Text p16 = to_utf16(pattern);
  std::shared_ptr<const re::Program> prog(re::compile(p16.units, ascii16(flags), &why).release());
  if (!prog) {
    error = std::string(who) + "(): " + (why.empty() ? std::string("not a valid pattern") : why);
    return nullptr;
  }
  cache.order.emplace_front(key, prog);
  cache.index[key] = cache.order.begin();
  if (cache.order.size() > Cache::kMax) {
    cache.index.erase(cache.order.back().first);
    cache.order.pop_back();
  }
  return prog;
}

// One match as grain sees it, from the engine's capture slots (UTF-16 units).
Value match_value(const re::Program& p, const std::string& text, const Utf16Text& t, const std::vector<int>& caps) {
  auto m = std::make_shared<MapObj>();
  const size_t s0 = t.byte_at[static_cast<size_t>(caps[0])], e0 = t.byte_at[static_cast<size_t>(caps[1])];
  m->put("text", Value::string(text.substr(s0, e0 - s0)));
  m->put("start", Value::integer(static_cast<int64_t>(s0)));
  m->put("end", Value::integer(static_cast<int64_t>(e0)));
  std::vector<Value> groups;
  auto named = std::make_shared<MapObj>();
  for (int g = 1; g < p.ncaps; ++g) {
    const int a = caps[static_cast<size_t>(2 * g)], b = caps[static_cast<size_t>(2 * g + 1)];
    Value v = Value::nil();
    if (a >= 0 && b >= 0) {
      const size_t sa = t.byte_at[static_cast<size_t>(a)], sb = t.byte_at[static_cast<size_t>(b)];
      v = Value::string(text.substr(sa, sb - sa));
    }
    groups.push_back(v);
    if (static_cast<size_t>(g) < p.group_names.size() && !p.group_names[static_cast<size_t>(g)].empty())
      named->put(utf8_of(p.group_names[static_cast<size_t>(g)]), v);
  }
  m->put("groups", Value::list(std::move(groups)));
  m->put("named", Value::object(Type::Map, named));
  return Value::object(Type::Map, m);
}

// Every match from the start, as the engine's capture vectors. An empty match
// moves the search on by one character so it cannot match in the same place again.
bool all_matches(const re::Program& p, const Utf16Text& t, std::vector<std::vector<int>>& out, size_t limit, const char* who,
                 std::string& error) {
  size_t at = 0;
  const bool unicode = p.unicode || p.unicode_sets;
  while (at <= t.units.size() && out.size() < limit) {
    std::vector<int> caps;
    const re::Result r = re::exec(p, t.units, at, false, &caps);
    if (r == re::Result::TooComplex) { error = std::string(who) + "(): the pattern is too complex for this text"; return false; }
    if (r == re::Result::NoMatch) break;
    const size_t start = static_cast<size_t>(caps[0]), end = static_cast<size_t>(caps[1]);
    out.push_back(std::move(caps));
    if (end > start) {
      at = end;
    } else {
      at = end + 1;
      if (unicode && end + 1 < t.units.size() && t.units[end] >= 0xD800 && t.units[end] <= 0xDBFF) ++at;
    }
  }
  return true;
}

bool args_ok(std::vector<Value>& args, size_t flags_at, const char* who, std::string& error, std::string& flags) {
  for (size_t i = 0; i < flags_at; ++i)
    if (!want_str(args[i], error, who)) return false;
  if (args.size() > flags_at) {
    if (!want_str(args[flags_at], error, who)) return false;
    flags = args[flags_at].str();
  }
  return true;
}

Value bi_regex_test(Vm&, std::vector<Value>& args, std::string& error) {
  std::string flags;
  if (!args_ok(args, 2, "regex_test", error, flags)) return Value::nil();
  auto p = compiled(args[0].str(), flags, "regex_test", error);
  if (!p) return Value::nil();
  const Utf16Text t = to_utf16(args[1].str());
  std::vector<int> caps;
  const re::Result r = re::exec(*p, t.units, 0, false, &caps);
  if (r == re::Result::TooComplex) { error = "regex_test(): the pattern is too complex for this text"; return Value::nil(); }
  return Value::boolean(r == re::Result::Match);
}

Value bi_regex_match(Vm&, std::vector<Value>& args, std::string& error) {
  std::string flags;
  if (!args_ok(args, 2, "regex_match", error, flags)) return Value::nil();
  auto p = compiled(args[0].str(), flags, "regex_match", error);
  if (!p) return Value::nil();
  const std::string& text = args[1].str();
  const Utf16Text t = to_utf16(text);
  std::vector<std::vector<int>> found;
  if (!all_matches(*p, t, found, 1, "regex_match", error)) return Value::nil();
  return found.empty() ? Value::nil() : match_value(*p, text, t, found[0]);
}

Value bi_regex_find_all(Vm&, std::vector<Value>& args, std::string& error) {
  std::string flags;
  if (!args_ok(args, 2, "regex_find_all", error, flags)) return Value::nil();
  auto p = compiled(args[0].str(), flags, "regex_find_all", error);
  if (!p) return Value::nil();
  const std::string& text = args[1].str();
  const Utf16Text t = to_utf16(text);
  std::vector<std::vector<int>> found;
  if (!all_matches(*p, t, found, SIZE_MAX, "regex_find_all", error)) return Value::nil();
  std::vector<Value> out;
  out.reserve(found.size());
  for (const auto& caps : found) out.push_back(match_value(*p, text, t, caps));
  return Value::list(std::move(out));
}

// $&, $1..$99, $<name> and $$ in a replacement string, for one match.
std::string expand(const std::string& with, const re::Program& p, const std::string& text, const Utf16Text& t, const std::vector<int>& caps) {
  auto group = [&](int g) -> std::string {
    if (g < 0 || g >= p.ncaps) return "";
    const int a = caps[static_cast<size_t>(2 * g)], b = caps[static_cast<size_t>(2 * g + 1)];
    if (a < 0 || b < 0) return "";
    const size_t sa = t.byte_at[static_cast<size_t>(a)], sb = t.byte_at[static_cast<size_t>(b)];
    return text.substr(sa, sb - sa);
  };
  std::string out;
  for (size_t i = 0; i < with.size(); ++i) {
    const char c = with[i];
    if (c != '$' || i + 1 >= with.size()) { out.push_back(c); continue; }
    const char d = with[i + 1];
    if (d == '$') { out.push_back('$'); ++i; continue; }
    if (d == '&') { out += group(0); ++i; continue; }
    if (d >= '0' && d <= '9') {
      int g = d - '0';
      size_t used = 1;
      if (i + 2 < with.size() && with[i + 2] >= '0' && with[i + 2] <= '9' && g * 10 + (with[i + 2] - '0') < p.ncaps) {
        g = g * 10 + (with[i + 2] - '0');
        used = 2;
      }
      if (g >= 1 && g < p.ncaps) { out += group(g); i += used; continue; }
      out.push_back(c);
      continue;
    }
    if (d == '<') {
      const size_t close = with.find('>', i + 2);
      if (close != std::string::npos) {
        const std::string name = with.substr(i + 2, close - i - 2);
        for (int g = 1; g < p.ncaps; ++g) {
          if (static_cast<size_t>(g) < p.group_names.size() && utf8_of(p.group_names[static_cast<size_t>(g)]) == name) {
            out += group(g);
            break;
          }
        }
        i = close;
        continue;
      }
    }
    out.push_back(c);
  }
  return out;
}

Value bi_regex_replace(Vm& vm, std::vector<Value>& args, std::string& error) {
  std::string flags;
  if (!want_str(args[0], error, "regex_replace") || !want_str(args[1], error, "regex_replace")) return Value::nil();
  const bool with_fn = args[2].type == Type::Fn || args[2].type == Type::Native;
  if (!with_fn && !want_str(args[2], error, "regex_replace")) return Value::nil();
  if (args.size() > 3) {
    if (!want_str(args[3], error, "regex_replace")) return Value::nil();
    flags = args[3].str();
  }
  auto p = compiled(args[0].str(), flags, "regex_replace", error);
  if (!p) return Value::nil();
  const std::string text = args[1].str();   // a copy: the callback may change what args[1] refers to
  const Utf16Text t = to_utf16(text);
  std::vector<std::vector<int>> found;
  if (!all_matches(*p, t, found, SIZE_MAX, "regex_replace", error)) return Value::nil();
  std::string out;
  size_t copied = 0;
  for (const auto& caps : found) {
    const size_t s0 = t.byte_at[static_cast<size_t>(caps[0])], e0 = t.byte_at[static_cast<size_t>(caps[1])];
    out.append(text, copied, s0 - copied);
    if (with_fn) {
      Value r = vm.call(args[2], {match_value(*p, text, t, caps)}, error);
      if (!error.empty()) return Value::nil();
      if (r.type != Type::Str) { error = std::string("regex_replace(): the function returned a ") + type_name(r.type) + ", not a string"; return Value::nil(); }
      out += r.str();
    } else {
      out += expand(args[2].str(), *p, text, t, caps);
    }
    copied = e0;
  }
  out.append(text, copied, std::string::npos);
  return Value::string(std::move(out));
}

Value bi_regex_split(Vm&, std::vector<Value>& args, std::string& error) {
  std::string flags;
  if (!args_ok(args, 2, "regex_split", error, flags)) return Value::nil();
  auto p = compiled(args[0].str(), flags, "regex_split", error);
  if (!p) return Value::nil();
  const std::string& text = args[1].str();
  const Utf16Text t = to_utf16(text);
  std::vector<std::vector<int>> found;
  if (!all_matches(*p, t, found, SIZE_MAX, "regex_split", error)) return Value::nil();
  std::vector<Value> out;
  size_t from = 0;
  for (const auto& caps : found) {
    const size_t s0 = t.byte_at[static_cast<size_t>(caps[0])], e0 = t.byte_at[static_cast<size_t>(caps[1])];
    if (e0 == s0 && (s0 == 0 || s0 == text.size())) continue;   // an empty match at either end splits nothing off
    out.push_back(Value::string(text.substr(from, s0 - from)));
    from = e0;
  }
  out.push_back(Value::string(text.substr(from)));
  return Value::list(std::move(out));
}

}  // namespace

void install_regex_builtins(Vm& vm) {
  vm.define_native("regex_test", 2, 3, bi_regex_test);
  vm.define_native("regex_match", 2, 3, bi_regex_match);
  vm.define_native("regex_find_all", 2, 3, bi_regex_find_all);
  vm.define_native("regex_replace", 3, 4, bi_regex_replace);
  vm.define_native("regex_split", 2, 3, bi_regex_split);
}

}  // namespace aios::userland::grain
