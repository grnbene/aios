#include "aios/userland/sql_functions.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>

#include "aios/core/bytes.hpp"
#include "aios/hal/sync.hpp"
#include "aios/userland/git.hpp"
#if defined(AIOS_HAVE_ZLIB)
#include <zlib.h>
#endif

namespace aios::userland::sqlfn {

namespace {

// --- a JSON document ------------------------------------------------------------
//
// Mutable, unlike channel::RpcValue: JSON_SET and friends edit a document in
// place. Numbers keep the text they were written with (12.50 stays 12.50);
// object members keep their written order, which is how MariaDB prints them.

struct J {
  enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
  std::string s;                                   // Bool "true"/"false", Num digits, Str value
  std::vector<J> a;
  std::vector<std::pair<std::string, J>> o;

  J* member(const std::string& k) {
    for (auto& m : o) if (m.first == k) return &m.second;
    return nullptr;
  }
};

J j_text(std::string v) { J j; j.kind = J::Str; j.s = std::move(v); return j; }

constexpr int kMaxDepth = 100;   // MySQL refuses deeper documents

class JsonReader {
 public:
  explicit JsonReader(const std::string& t) : p_(t.data()), e_(t.data() + t.size()) {}

  bool document(J* out) {
    if (!value(out, 1)) return false;
    ws();
    return p_ == e_;
  }

 private:
  void ws() { while (p_ < e_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_; }

  bool literal(const char* w) {
    size_t n = std::strlen(w);
    if ((size_t)(e_ - p_) < n || std::memcmp(p_, w, n) != 0) return false;
    p_ += n;
    return true;
  }

  static void put_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
      out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
    } else {
      out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F));
      out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
    }
  }

  bool hex4(uint32_t* v) {
    if (e_ - p_ < 4) return false;
    *v = 0;
    for (int i = 0; i < 4; ++i) {
      char c = *p_++;
      int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
      if (d < 0) return false;
      *v = *v * 16 + (uint32_t)d;
    }
    return true;
  }

  bool string(std::string* out) {
    if (p_ >= e_ || *p_ != '"') return false;
    ++p_;
    while (p_ < e_) {
      unsigned char c = (unsigned char)*p_++;
      if (c == '"') return true;
      if (c < 0x20) return false;
      if (c != '\\') { *out += (char)c; continue; }
      if (p_ >= e_) return false;
      char x = *p_++;
      switch (x) {
        case '"': *out += '"'; break;
        case '\\': *out += '\\'; break;
        case '/': *out += '/'; break;
        case 'b': *out += '\b'; break;
        case 'f': *out += '\f'; break;
        case 'n': *out += '\n'; break;
        case 'r': *out += '\r'; break;
        case 't': *out += '\t'; break;
        case 'u': {
          uint32_t cp = 0;
          if (!hex4(&cp)) return false;
          if (cp >= 0xD800 && cp < 0xDC00) {
            uint32_t lo = 0;
            if (e_ - p_ < 6 || p_[0] != '\\' || p_[1] != 'u') return false;
            p_ += 2;
            if (!hex4(&lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          } else if (cp >= 0xDC00 && cp < 0xE000) {
            return false;
          }
          put_utf8(*out, cp);
          break;
        }
        default: return false;
      }
    }
    return false;
  }

  bool number(J* out) {
    const char* b = p_;
    if (p_ < e_ && *p_ == '-') ++p_;
    if (p_ >= e_) return false;
    if (*p_ == '0') ++p_;
    else if (*p_ >= '1' && *p_ <= '9') while (p_ < e_ && *p_ >= '0' && *p_ <= '9') ++p_;
    else return false;
    if (p_ < e_ && *p_ == '.') {
      ++p_;
      if (p_ >= e_ || *p_ < '0' || *p_ > '9') return false;
      while (p_ < e_ && *p_ >= '0' && *p_ <= '9') ++p_;
    }
    if (p_ < e_ && (*p_ == 'e' || *p_ == 'E')) {
      ++p_;
      if (p_ < e_ && (*p_ == '+' || *p_ == '-')) ++p_;
      if (p_ >= e_ || *p_ < '0' || *p_ > '9') return false;
      while (p_ < e_ && *p_ >= '0' && *p_ <= '9') ++p_;
    }
    out->kind = J::Num;
    out->s.assign(b, p_);
    return true;
  }

  bool value(J* out, int depth) {
    if (depth > kMaxDepth) return false;
    ws();
    if (p_ >= e_) return false;
    char c = *p_;
    if (c == '{') {
      ++p_;
      out->kind = J::Obj;
      ws();
      if (p_ < e_ && *p_ == '}') { ++p_; return true; }
      while (true) {
        ws();
        std::string key;
        if (!string(&key)) return false;
        ws();
        if (p_ >= e_ || *p_ != ':') return false;
        ++p_;
        J v;
        if (!value(&v, depth + 1)) return false;
        // The last of a repeated key wins, as in MySQL.
        if (J* had = out->member(key)) *had = std::move(v);
        else out->o.emplace_back(std::move(key), std::move(v));
        ws();
        if (p_ < e_ && *p_ == ',') { ++p_; continue; }
        if (p_ < e_ && *p_ == '}') { ++p_; return true; }
        return false;
      }
    }
    if (c == '[') {
      ++p_;
      out->kind = J::Arr;
      ws();
      if (p_ < e_ && *p_ == ']') { ++p_; return true; }
      while (true) {
        J v;
        if (!value(&v, depth + 1)) return false;
        out->a.push_back(std::move(v));
        ws();
        if (p_ < e_ && *p_ == ',') { ++p_; continue; }
        if (p_ < e_ && *p_ == ']') { ++p_; return true; }
        return false;
      }
    }
    if (c == '"') {
      out->kind = J::Str;
      return string(&out->s);
    }
    if (literal("true")) { out->kind = J::Bool; out->s = "true"; return true; }
    if (literal("false")) { out->kind = J::Bool; out->s = "false"; return true; }
    if (literal("null")) { out->kind = J::Null; return true; }
    return number(out);
  }

  const char* p_;
  const char* e_;
};

bool parse_json(const std::string& t, J* out) { return JsonReader(t).document(out); }

void quote_into(std::string& out, const std::string& s) {
  out += '"';
  for (unsigned char ch : s) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (ch < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
          out += buf;
        } else {
          out += (char)ch;
        }
    }
  }
  out += '"';
}

// MariaDB's layout: ", " between members, ": " after a key.
void render_into(std::string& out, const J& j) {
  switch (j.kind) {
    case J::Null: out += "null"; return;
    case J::Bool:
    case J::Num: out += j.s; return;
    case J::Str: quote_into(out, j.s); return;
    case J::Arr:
      out += '[';
      for (size_t i = 0; i < j.a.size(); ++i) {
        if (i) out += ", ";
        render_into(out, j.a[i]);
      }
      out += ']';
      return;
    case J::Obj:
      out += '{';
      for (size_t i = 0; i < j.o.size(); ++i) {
        if (i) out += ", ";
        quote_into(out, j.o[i].first);
        out += ": ";
        render_into(out, j.o[i].second);
      }
      out += '}';
      return;
  }
}

std::string render(const J& j) {
  std::string out;
  render_into(out, j);
  return out;
}

// --- paths: $, .key, ."key", .*, [n], [last], [last-n], [*], ** -----------------

struct Leg {
  enum Kind { Key, Index, AnyKey, AnyIndex, AnyDepth } kind = Key;
  std::string key;
  long long index = 0;
  bool from_last = false;   // [last - index]
};

bool ident_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$' ||
         (unsigned char)c >= 0x80;
}

// Empty error on success; otherwise MySQL's message for a bad path.
std::string parse_path(const std::string& p, std::vector<Leg>* legs) {
  size_t i = 0, n = p.size();
  auto bad = [&]() { return "Invalid JSON path expression. The error is around character position " + std::to_string(i + 1) + "."; };
  auto sp = [&]() { while (i < n && (p[i] == ' ' || p[i] == '\t')) ++i; };
  sp();
  if (i >= n || p[i] != '$') return bad();
  ++i;
  while (true) {
    sp();
    if (i >= n) break;
    Leg leg;
    if (p[i] == '.') {
      ++i;
      sp();
      if (i < n && p[i] == '*') { leg.kind = Leg::AnyKey; ++i; }
      else if (i < n && p[i] == '"') {
        ++i;
        std::string k;
        while (i < n && p[i] != '"') {
          if (p[i] == '\\' && i + 1 < n) ++i;
          k += p[i++];
        }
        if (i >= n) return bad();
        ++i;
        leg.key = k;
      } else {
        size_t b = i;
        while (i < n && ident_char(p[i])) ++i;
        if (i == b || (p[b] >= '0' && p[b] <= '9')) return bad();
        leg.key = p.substr(b, i - b);
      }
    } else if (p[i] == '[') {
      ++i;
      sp();
      if (i < n && p[i] == '*') { leg.kind = Leg::AnyIndex; ++i; }
      else {
        leg.kind = Leg::Index;
        if (p.compare(i, 4, "last") == 0) {
          leg.from_last = true;
          i += 4;
          sp();
          if (i < n && p[i] == '-') {
            ++i;
            sp();
            if (i >= n || p[i] < '0' || p[i] > '9') return bad();
            while (i < n && p[i] >= '0' && p[i] <= '9') leg.index = leg.index * 10 + (p[i++] - '0');
          }
        } else {
          if (i >= n || p[i] < '0' || p[i] > '9') return bad();
          while (i < n && p[i] >= '0' && p[i] <= '9') {
            leg.index = leg.index * 10 + (p[i++] - '0');
            if (leg.index > (1LL << 40)) return bad();
          }
        }
      }
      sp();
      if (i >= n || p[i] != ']') return bad();
      ++i;
    } else if (p.compare(i, 2, "**") == 0) {
      leg.kind = Leg::AnyDepth;
      i += 2;
    } else {
      return bad();
    }
    legs->push_back(std::move(leg));
  }
  // ** has to lead somewhere: `$**` alone is not a path.
  if (!legs->empty() && legs->back().kind == Leg::AnyDepth) return bad();
  return {};
}

bool has_wildcard(const std::vector<Leg>& legs) {
  for (const auto& l : legs) if (l.kind != Leg::Key && l.kind != Leg::Index) return true;
  return false;
}

long long leg_index(const Leg& l, size_t size) {
  return l.from_last ? (long long)size - 1 - l.index : l.index;
}

std::string key_leg_text(const std::string& k) {
  bool plain = !k.empty() && !(k[0] >= '0' && k[0] <= '9');
  for (char c : k) if (!ident_char(c)) plain = false;
  if (plain) return "." + k;
  std::string q;
  quote_into(q, k);
  return "." + q;
}

// Every value a (possibly wildcarded) path reaches, with the plain path to it,
// in document order. A non-array at [0] is the value itself, as MySQL reads it.
void collect(const J& v, const std::vector<Leg>& legs, size_t at, const std::string& where,
             std::vector<std::pair<const J*, std::string>>* out) {
  if (at == legs.size()) {
    for (const auto& seen : *out) if (seen.first == &v) return;
    out->emplace_back(&v, where);
    return;
  }
  const Leg& l = legs[at];
  switch (l.kind) {
    case Leg::Key:
      if (v.kind == J::Obj)
        for (const auto& m : v.o) if (m.first == l.key) collect(m.second, legs, at + 1, where + key_leg_text(m.first), out);
      return;
    case Leg::AnyKey:
      if (v.kind == J::Obj)
        for (const auto& m : v.o) collect(m.second, legs, at + 1, where + key_leg_text(m.first), out);
      return;
    case Leg::Index:
      if (v.kind == J::Arr) {
        long long ix = leg_index(l, v.a.size());
        if (ix >= 0 && ix < (long long)v.a.size()) collect(v.a[ix], legs, at + 1, where + "[" + std::to_string(ix) + "]", out);
      } else if (leg_index(l, 1) == 0) {
        collect(v, legs, at + 1, where, out);
      }
      return;
    case Leg::AnyIndex:
      if (v.kind == J::Arr)
        for (size_t k = 0; k < v.a.size(); ++k) collect(v.a[k], legs, at + 1, where + "[" + std::to_string(k) + "]", out);
      return;
    case Leg::AnyDepth:
      collect(v, legs, at + 1, where, out);
      if (v.kind == J::Arr)
        for (size_t k = 0; k < v.a.size(); ++k) collect(v.a[k], legs, at, where + "[" + std::to_string(k) + "]", out);
      if (v.kind == J::Obj)
        for (const auto& m : v.o) collect(m.second, legs, at, where + key_leg_text(m.first), out);
      return;
  }
}

// The one value a plain path names, or null.
J* walk(J* v, const std::vector<Leg>& legs, size_t upto) {
  for (size_t i = 0; i < upto && v; ++i) {
    const Leg& l = legs[i];
    if (l.kind == Leg::Key) {
      v = v->kind == J::Obj ? v->member(l.key) : nullptr;
    } else if (v->kind == J::Arr) {
      long long ix = leg_index(l, v->a.size());
      v = ix >= 0 && ix < (long long)v->a.size() ? &v->a[ix] : nullptr;
    } else {
      if (leg_index(l, 1) != 0) v = nullptr;
    }
  }
  return v;
}

int depth_of(const J& j) {
  int d = 0;
  for (const auto& x : j.a) d = std::max(d, depth_of(x));
  for (const auto& m : j.o) d = std::max(d, depth_of(m.second));
  return (j.kind == J::Arr && !j.a.empty()) || (j.kind == J::Obj && !j.o.empty()) ? d + 1 : 1;
}

void merge_preserve(J* into, J&& from) {
  if (into->kind == J::Obj && from.kind == J::Obj) {
    for (auto& m : from.o) {
      if (J* had = into->member(m.first)) merge_preserve(had, std::move(m.second));
      else into->o.emplace_back(std::move(m.first), std::move(m.second));
    }
    return;
  }
  if (into->kind != J::Arr) {
    J wrap;
    wrap.kind = J::Arr;
    wrap.a.push_back(std::move(*into));
    *into = std::move(wrap);
  }
  if (from.kind == J::Arr) for (auto& x : from.a) into->a.push_back(std::move(x));
  else into->a.push_back(std::move(from));
}

// RFC 7396.
void merge_patch(J* target, const J& patch) {
  if (patch.kind != J::Obj) { *target = patch; return; }
  if (target->kind != J::Obj) { *target = J(); target->kind = J::Obj; }
  for (const auto& m : patch.o) {
    if (m.second.kind == J::Null) {
      auto& o = target->o;
      o.erase(std::remove_if(o.begin(), o.end(), [&](const auto& x) { return x.first == m.first; }), o.end());
      continue;
    }
    J* had = target->member(m.first);
    if (!had) {
      target->o.emplace_back(m.first, J());
      had = &target->o.back().second;
    }
    merge_patch(had, m.second);
  }
}

// LIKE with % and _ over UTF-8 characters, ASCII letters folded (the default
// collation's view of it), `esc` quoting the next character.
size_t u8_len(unsigned char c) { return c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4; }

char fold(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

bool like(const char* s, const char* se, const char* p, const char* pe, char esc) {
  while (p < pe) {
    if (*p == '%') {
      while (p < pe && *p == '%') ++p;
      if (p == pe) return true;
      for (const char* t = s; t <= se; t += t < se ? u8_len((unsigned char)*t) : 1) {
        if (like(t, se, p, pe, esc)) return true;
        if (t == se) break;
      }
      return false;
    }
    if (s >= se) return false;
    if (*p == '_') {
      s += u8_len((unsigned char)*s);
      ++p;
      continue;
    }
    if (*p == esc && p + 1 < pe) ++p;
    size_t n = u8_len((unsigned char)*p);
    if (n == 1) {
      if (fold(*s) != fold(*p)) return false;
    } else if ((size_t)(se - s) < n || std::memcmp(s, p, n) != 0) {
      return false;
    }
    s += n;
    p += n;
  }
  return s >= se;
}

void search_strings(const J& v, const std::string& where, const std::string& pat, char esc, std::vector<std::string>* hits) {
  if (v.kind == J::Str) {
    if (like(v.s.data(), v.s.data() + v.s.size(), pat.data(), pat.data() + pat.size(), esc)) hits->push_back(where);
    return;
  }
  if (v.kind == J::Arr)
    for (size_t k = 0; k < v.a.size(); ++k) search_strings(v.a[k], where + "[" + std::to_string(k) + "]", pat, esc, hits);
  if (v.kind == J::Obj)
    for (const auto& m : v.o) search_strings(m.second, where + key_leg_text(m.first), pat, esc, hits);
}

J value_of(const JsonArg& a) {
  J j;
  if (a.null) return j;
  if (a.json && parse_json(a.text, &j)) return j;
  if (a.numeric) {
    J n;
    if (JsonReader(a.text).document(&n) && n.kind == J::Num) return n;
  }
  return j_text(a.text);
}

JsonResult null_result() { return JsonResult(); }
JsonResult doc_result(const J& j) {
  JsonResult r;
  r.null = false;
  r.json = true;
  r.text = render(j);
  return r;
}
JsonResult num_result(long long v) {
  JsonResult r;
  r.null = false;
  r.numeric = true;
  r.text = std::to_string(v);
  return r;
}
JsonResult text_result(std::string t) {
  JsonResult r;
  r.null = false;
  r.text = std::move(t);
  return r;
}
JsonResult error_result(std::string e) {
  JsonResult r;
  r.error = std::move(e);
  return r;
}

std::string lower_name(const std::string& up) {
  std::string s = up;
  for (auto& c : s) c = fold(c);
  return s;
}

JsonResult arg_count_error(const std::string& up) {
  return error_result("Incorrect parameter count in the call to native function '" + up + "'");
}

const char* const kWildcardNotHere = "In this situation, path expressions may not contain the * and ** tokens.";

// JSON_SET / JSON_INSERT / JSON_REPLACE: one path and value at a time.
enum class Put { Set, Insert, Replace };

void put_at(J* root, const std::vector<Leg>& legs, J val, Put how) {
  if (legs.empty()) {
    if (how != Put::Insert) *root = std::move(val);
    return;
  }
  J* parent = walk(root, legs, legs.size() - 1);
  if (!parent) return;
  const Leg& l = legs.back();
  if (l.kind == Leg::Key) {
    if (parent->kind != J::Obj) return;
    if (J* had = parent->member(l.key)) {
      if (how != Put::Insert) *had = std::move(val);
    } else if (how != Put::Replace) {
      parent->o.emplace_back(l.key, std::move(val));
    }
    return;
  }
  if (parent->kind == J::Arr) {
    long long ix = leg_index(l, parent->a.size());
    if (ix >= 0 && ix < (long long)parent->a.size()) {
      if (how != Put::Insert) parent->a[ix] = std::move(val);
    } else if (how != Put::Replace && ix >= 0) {
      parent->a.push_back(std::move(val));
    }
    return;
  }
  // A non-array addressed by position is a one-element array of itself.
  if (leg_index(l, 1) == 0) {
    if (how != Put::Insert) *parent = std::move(val);
  } else if (how != Put::Replace && leg_index(l, 1) > 0) {
    J wrap;
    wrap.kind = J::Arr;
    wrap.a.push_back(std::move(*parent));
    wrap.a.push_back(std::move(val));
    *parent = std::move(wrap);
  }
}

}  // namespace

bool json_function(const std::string& up) {
  static const char* const kNames[] = {
      "JSON_VALID", "JSON_TYPE", "JSON_KEYS", "JSON_DEPTH", "JSON_QUOTE", "JSON_SET", "JSON_INSERT", "JSON_REPLACE",
      "JSON_REMOVE", "JSON_ARRAY_APPEND", "JSON_ARRAY_INSERT", "JSON_MERGE", "JSON_MERGE_PRESERVE",
      "JSON_MERGE_PATCH", "JSON_SEARCH", "JSON_CONTAINS_PATH", "JSON_EXISTS"};
  for (const char* n : kNames) if (up == n) return true;
  return false;
}

JsonResult json_call(const std::string& up, const std::vector<JsonArg>& args) {
  const size_t n = args.size();

  if (up == "JSON_QUOTE") {
    if (n != 1) return arg_count_error(up);
    if (args[0].null) return null_result();
    std::string q;
    quote_into(q, args[0].text);
    return text_result(q);
  }
  if (up == "JSON_VALID") {
    if (n != 1) return arg_count_error(up);
    if (args[0].null) return null_result();
    J j;
    return num_result(parse_json(args[0].text, &j) ? 1 : 0);
  }

  if (n == 0) return arg_count_error(up);
  // Every other function reads a document first: NULL, or text that is not
  // JSON, is NULL (MariaDB's answer; it warns where MySQL errors).
  if (args[0].null) return null_result();
  J doc;
  if (!parse_json(args[0].text, &doc)) return null_result();

  auto path_of = [&](size_t i, std::vector<Leg>* legs, bool wild_ok, std::string* err) -> bool {
    *err = parse_path(args[i].text, legs);
    if (err->empty() && !wild_ok && has_wildcard(*legs)) *err = kWildcardNotHere;
    return err->empty();
  };

  if (up == "JSON_TYPE" || up == "JSON_DEPTH") {
    if (n != 1) return arg_count_error(up);
    if (up == "JSON_DEPTH") return num_result(depth_of(doc));
    switch (doc.kind) {
      case J::Null: return text_result("NULL");
      case J::Bool: return text_result("BOOLEAN");
      case J::Str: return text_result("STRING");
      case J::Arr: return text_result("ARRAY");
      case J::Obj: return text_result("OBJECT");
      case J::Num: return text_result(doc.s.find_first_of(".eE") == std::string::npos ? "INTEGER" : "DOUBLE");
    }
  }

  if (up == "JSON_KEYS") {
    if (n > 2) return arg_count_error(up);
    const J* at = &doc;
    if (n == 2) {
      if (args[1].null) return null_result();
      std::vector<Leg> legs;
      std::string err;
      if (!path_of(1, &legs, false, &err)) return error_result(err);
      at = walk(&doc, legs, legs.size());
    }
    if (!at || at->kind != J::Obj) return null_result();
    J keys;
    keys.kind = J::Arr;
    for (const auto& m : at->o) keys.a.push_back(j_text(m.first));
    return doc_result(keys);
  }

  if (up == "JSON_EXISTS") {
    if (n != 2) return arg_count_error(up);
    if (args[1].null) return null_result();
    std::vector<Leg> legs;
    std::string err;
    if (!path_of(1, &legs, true, &err)) return error_result(err);
    std::vector<std::pair<const J*, std::string>> hits;
    collect(doc, legs, 0, "$", &hits);
    return num_result(hits.empty() ? 0 : 1);
  }

  if (up == "JSON_CONTAINS_PATH") {
    if (n < 3) return arg_count_error(up);
    for (size_t i = 1; i < n; ++i) if (args[i].null) return null_result();
    std::string mode = lower_name(args[1].text);
    if (mode != "one" && mode != "all") return error_result("The oneOrAll argument to json_contains_path may take these values: 'one' or 'all'.");
    bool any = false, all = true;
    for (size_t i = 2; i < n; ++i) {
      std::vector<Leg> legs;
      std::string err;
      if (!path_of(i, &legs, true, &err)) return error_result(err);
      std::vector<std::pair<const J*, std::string>> hits;
      collect(doc, legs, 0, "$", &hits);
      if (hits.empty()) all = false;
      else any = true;
    }
    return num_result(mode == "one" ? any : all);
  }

  if (up == "JSON_SEARCH") {
    if (n < 3) return arg_count_error(up);
    if (args[1].null || args[2].null) return null_result();
    std::string mode = lower_name(args[1].text);
    if (mode != "one" && mode != "all") return error_result("The oneOrAll argument to json_search may take these values: 'one' or 'all'.");
    char esc = '\\';
    if (n > 3 && !args[3].null && !args[3].text.empty()) {
      if (args[3].text.size() != 1) return error_result("Incorrect arguments to ESCAPE");
      esc = args[3].text[0];
    }
    std::vector<std::string> hits;
    if (n > 4) {
      for (size_t i = 4; i < n; ++i) {
        if (args[i].null) return null_result();
        std::vector<Leg> legs;
        std::string err;
        if (!path_of(i, &legs, true, &err)) return error_result(err);
        std::vector<std::pair<const J*, std::string>> roots;
        collect(doc, legs, 0, "$", &roots);
        for (const auto& r : roots) search_strings(*r.first, r.second, args[2].text, esc, &hits);
      }
    } else {
      search_strings(doc, "$", args[2].text, esc, &hits);
    }
    std::vector<std::string> uniq;
    for (auto& h : hits) if (std::find(uniq.begin(), uniq.end(), h) == uniq.end()) uniq.push_back(std::move(h));
    if (uniq.empty()) return null_result();
    if (mode == "one" || uniq.size() == 1) return doc_result(j_text(uniq.front()));
    J arr;
    arr.kind = J::Arr;
    for (auto& h : uniq) arr.a.push_back(j_text(h));
    return doc_result(arr);
  }

  if (up == "JSON_SET" || up == "JSON_INSERT" || up == "JSON_REPLACE" || up == "JSON_ARRAY_APPEND" || up == "JSON_ARRAY_INSERT") {
    if (n < 3 || n % 2 == 0) return arg_count_error(up);
    for (size_t i = 1; i < n; i += 2) {
      if (args[i].null) return null_result();
      std::vector<Leg> legs;
      std::string err;
      if (!path_of(i, &legs, false, &err)) return error_result(err);
      J val = value_of(args[i + 1]);
      if (up == "JSON_ARRAY_APPEND") {
        J* at = walk(&doc, legs, legs.size());
        if (!at) continue;
        if (at->kind != J::Arr) {
          J wrap;
          wrap.kind = J::Arr;
          wrap.a.push_back(std::move(*at));
          *at = std::move(wrap);
        }
        at->a.push_back(std::move(val));
      } else if (up == "JSON_ARRAY_INSERT") {
        if (legs.empty() || legs.back().kind != Leg::Index) return error_result("A path expression is not a path to a cell in an array.");
        J* parent = walk(&doc, legs, legs.size() - 1);
        if (!parent || parent->kind != J::Arr) continue;
        long long ix = std::max(0LL, std::min(leg_index(legs.back(), parent->a.size()), (long long)parent->a.size()));
        parent->a.insert(parent->a.begin() + ix, std::move(val));
      } else {
        put_at(&doc, legs, std::move(val), up == "JSON_SET" ? Put::Set : up == "JSON_INSERT" ? Put::Insert : Put::Replace);
      }
    }
    return doc_result(doc);
  }

  if (up == "JSON_REMOVE") {
    if (n < 2) return arg_count_error(up);
    for (size_t i = 1; i < n; ++i) {
      if (args[i].null) return null_result();
      std::vector<Leg> legs;
      std::string err;
      if (!path_of(i, &legs, false, &err)) return error_result(err);
      if (legs.empty()) return error_result("The path expression '$' is not allowed in this context.");
      J* parent = walk(&doc, legs, legs.size() - 1);
      if (!parent) continue;
      const Leg& l = legs.back();
      if (l.kind == Leg::Key && parent->kind == J::Obj) {
        auto& o = parent->o;
        o.erase(std::remove_if(o.begin(), o.end(), [&](const auto& m) { return m.first == l.key; }), o.end());
      } else if (l.kind == Leg::Index && parent->kind == J::Arr) {
        long long ix = leg_index(l, parent->a.size());
        if (ix >= 0 && ix < (long long)parent->a.size()) parent->a.erase(parent->a.begin() + ix);
      }
    }
    return doc_result(doc);
  }

  if (up == "JSON_MERGE" || up == "JSON_MERGE_PRESERVE" || up == "JSON_MERGE_PATCH") {
    if (n < 2) return arg_count_error(up);
    for (size_t i = 1; i < n; ++i) {
      if (args[i].null) return null_result();
      J next;
      if (!parse_json(args[i].text, &next)) return null_result();
      if (up == "JSON_MERGE_PATCH") merge_patch(&doc, next);
      else merge_preserve(&doc, std::move(next));
    }
    return doc_result(doc);
  }

  return error_result("unknown function: " + up + "()");
}

// --- base64 -----------------------------------------------------------------------

std::string to_base64(const std::string& in) {
  static const char* kAlpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t line = 0;
  auto put = [&](char c) {
    if (line == 76) { out += '\n'; line = 0; }
    out += c;
    ++line;
  };
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    uint32_t v = (uint32_t)(unsigned char)in[i] << 16 | (uint32_t)(unsigned char)in[i + 1] << 8 | (unsigned char)in[i + 2];
    put(kAlpha[v >> 18]); put(kAlpha[(v >> 12) & 63]); put(kAlpha[(v >> 6) & 63]); put(kAlpha[v & 63]);
  }
  if (i < in.size()) {
    uint32_t v = (uint32_t)(unsigned char)in[i] << 16;
    if (i + 1 < in.size()) v |= (uint32_t)(unsigned char)in[i + 1] << 8;
    put(kAlpha[v >> 18]);
    put(kAlpha[(v >> 12) & 63]);
    put(i + 1 < in.size() ? kAlpha[(v >> 6) & 63] : '=');
    put('=');
  }
  return out;
}

bool from_base64(const std::string& in, std::string* out) {
  out->clear();
  uint32_t acc = 0;
  int bits = 0, pad = 0;
  size_t chars = 0;
  for (char c : in) {
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+') v = 62;
    else if (c == '/') v = 63;
    else if (c == '=') { ++pad; ++chars; continue; }
    else return false;
    if (pad) return false;   // data after the padding
    ++chars;
    acc = acc << 6 | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out->push_back((char)((acc >> bits) & 0xFF));
    }
  }
  if (pad > 2 || (pad && chars % 4 != 0)) return false;
  if ((chars - pad) % 4 == 1) return false;
  return true;
}

// --- CONV -------------------------------------------------------------------------

bool conv(const std::string& s, int from_base, int to_base, std::string* out) {
  int fb = from_base < 0 ? -from_base : from_base;
  int tb = to_base < 0 ? -to_base : to_base;
  if (fb < 2 || fb > 36 || tb < 2 || tb > 36) return false;
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  bool neg = false;
  if (i < s.size() && (s[i] == '-' || s[i] == '+')) neg = s[i++] == '-';
  unsigned long long mag = 0;
  bool overflow = false;
  for (; i < s.size(); ++i) {
    char c = s[i];
    int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : 99;
    if (d >= fb) break;
    unsigned long long next = mag * (unsigned long long)fb + (unsigned long long)d;
    if (mag > (~0ULL - (unsigned long long)d) / (unsigned long long)fb) overflow = true;
    mag = next;
  }
  // my_strntoll / my_strntoull: a signed read clamps to the signed range, an
  // unsigned one saturates and negates in two's complement.
  unsigned long long v;
  if (from_base < 0) {
    long long sv;
    if (overflow || mag > (neg ? (1ULL << 63) : (1ULL << 63) - 1)) sv = neg ? (long long)(1ULL << 63) : (long long)((1ULL << 63) - 1);
    else sv = neg ? (long long)(0 - mag) : (long long)mag;
    v = (unsigned long long)sv;
  } else {
    if (overflow) mag = ~0ULL;
    v = neg ? 0 - mag : mag;
  }
  static const char* kDigits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  std::string r;
  bool minus = false;
  if (to_base < 0 && (long long)v < 0) { minus = true; v = 0 - v; }
  do { r += kDigits[v % (unsigned)tb]; v /= (unsigned)tb; } while (v);
  if (minus) r += '-';
  std::reverse(r.begin(), r.end());
  *out = r;
  return true;
}

// --- AES-128 (FIPS-197), ECB --------------------------------------------------------

namespace {

const uint8_t kSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

struct Aes128 {
  uint8_t rk[176];
  uint8_t inv[256];

  explicit Aes128(const uint8_t key[16]) {
    for (int i = 0; i < 256; ++i) inv[kSbox[i]] = (uint8_t)i;
    std::memcpy(rk, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
      uint8_t t[4] = {rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1]};
      if (i % 16 == 0) {
        uint8_t first = t[0];
        t[0] = (uint8_t)(kSbox[t[1]] ^ rcon);
        t[1] = kSbox[t[2]];
        t[2] = kSbox[t[3]];
        t[3] = kSbox[first];
        rcon = xt(rcon);
      }
      for (int k = 0; k < 4; ++k) rk[i + k] = (uint8_t)(rk[i - 16 + k] ^ t[k]);
    }
  }

  static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }
  static uint8_t mul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) { if (b & 1) r ^= a; a = xt(a); b >>= 1; }
    return r;
  }

  // The state is column-major: s[4*c + r].
  void encrypt(uint8_t s[16]) const {
    for (int i = 0; i < 16; ++i) s[i] ^= rk[i];
    for (int round = 1; round <= 10; ++round) {
      for (int i = 0; i < 16; ++i) s[i] = kSbox[s[i]];
      uint8_t t[16];
      for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) t[4 * c + r] = s[4 * ((c + r) % 4) + r];
      if (round != 10) {
        for (int c = 0; c < 4; ++c) {
          uint8_t* col = t + 4 * c;
          uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
          col[0] = (uint8_t)(xt(a0) ^ xt(a1) ^ a1 ^ a2 ^ a3);
          col[1] = (uint8_t)(a0 ^ xt(a1) ^ xt(a2) ^ a2 ^ a3);
          col[2] = (uint8_t)(a0 ^ a1 ^ xt(a2) ^ xt(a3) ^ a3);
          col[3] = (uint8_t)(xt(a0) ^ a0 ^ a1 ^ a2 ^ xt(a3));
        }
      }
      for (int i = 0; i < 16; ++i) s[i] = (uint8_t)(t[i] ^ rk[16 * round + i]);
    }
  }

  void decrypt(uint8_t s[16]) const {
    for (int i = 0; i < 16; ++i) s[i] ^= rk[160 + i];
    for (int round = 9; round >= 0; --round) {
      uint8_t t[16];
      for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) t[4 * ((c + r) % 4) + r] = s[4 * c + r];
      for (int i = 0; i < 16; ++i) s[i] = (uint8_t)(inv[t[i]] ^ rk[16 * round + i]);
      if (round != 0) {
        for (int c = 0; c < 4; ++c) {
          uint8_t* col = s + 4 * c;
          uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
          col[0] = (uint8_t)(mul(a0, 14) ^ mul(a1, 11) ^ mul(a2, 13) ^ mul(a3, 9));
          col[1] = (uint8_t)(mul(a0, 9) ^ mul(a1, 14) ^ mul(a2, 11) ^ mul(a3, 13));
          col[2] = (uint8_t)(mul(a0, 13) ^ mul(a1, 9) ^ mul(a2, 14) ^ mul(a3, 11));
          col[3] = (uint8_t)(mul(a0, 11) ^ mul(a1, 13) ^ mul(a2, 9) ^ mul(a3, 14));
        }
      }
    }
  }
};

// my_aes_create_key: every key byte XORed into a 16-byte buffer, cycling.
void fold_key(const std::string& key, uint8_t out[16]) {
  std::memset(out, 0, 16);
  for (size_t i = 0; i < key.size(); ++i) out[i % 16] ^= (uint8_t)key[i];
}

}  // namespace

std::string aes_encrypt(const std::string& data, const std::string& key) {
  uint8_t k[16];
  fold_key(key, k);
  Aes128 aes(k);
  size_t pad = 16 - data.size() % 16;
  std::string buf = data;
  buf.append(pad, (char)pad);
  for (size_t i = 0; i < buf.size(); i += 16) aes.encrypt(reinterpret_cast<uint8_t*>(&buf[i]));
  return buf;
}

bool aes_decrypt(const std::string& data, const std::string& key, std::string* out) {
  if (data.empty() || data.size() % 16 != 0) return false;
  uint8_t k[16];
  fold_key(key, k);
  Aes128 aes(k);
  std::string buf = data;
  for (size_t i = 0; i < buf.size(); i += 16) aes.decrypt(reinterpret_cast<uint8_t*>(&buf[i]));
  unsigned pad = (unsigned char)buf.back();
  if (pad == 0 || pad > 16) return false;
  for (size_t i = buf.size() - pad; i < buf.size(); ++i)
    if ((unsigned char)buf[i] != pad) return false;
  buf.resize(buf.size() - pad);
  *out = std::move(buf);
  return true;
}

// --- ENCODE / DECODE (sql_crypt.cc) -------------------------------------------------

namespace {

struct SqlCrypt {
  uint64_t seed1 = 0, seed2 = 0;
  static constexpr uint64_t kMax = 0x3FFFFFFFULL;
  uint8_t encode_buf[256], decode_buf[256];
  unsigned shift = 0;

  double rnd() {
    seed1 = (seed1 * 3 + seed2) % kMax;
    seed2 = (seed1 + seed2 + 33) % kMax;
    return (double)seed1 / (double)kMax;
  }

  explicit SqlCrypt(const std::string& key) {
    // hash_password(): the pre-4.1 password hash, spaces and tabs skipped.
    uint64_t nr = 1345345333ULL, add = 7, nr2 = 0x12345671ULL;
    for (unsigned char c : key) {
      if (c == ' ' || c == '\t') continue;
      uint64_t tmp = c;
      nr ^= (((nr & 63) + add) * tmp) + (nr << 8);
      nr2 += (nr2 << 8) ^ nr;
      add += tmp;
    }
    seed1 = (nr & ((1ULL << 31) - 1)) % kMax;
    seed2 = (nr2 & ((1ULL << 31) - 1)) % kMax;
    for (int i = 0; i < 256; ++i) decode_buf[i] = (uint8_t)i;
    for (int i = 0; i < 256; ++i) {
      int idx = (int)(unsigned)(rnd() * 255.0);
      uint8_t a = decode_buf[idx];
      decode_buf[idx] = decode_buf[i];
      decode_buf[i] = a;
    }
    for (int i = 0; i < 256; ++i) encode_buf[decode_buf[i]] = (uint8_t)i;
  }
};

}  // namespace

std::string sql_crypt_encode(const std::string& data, const std::string& key) {
  SqlCrypt c(key);
  std::string out = data;
  for (auto& ch : out) {
    c.shift ^= (unsigned)(c.rnd() * 255.0);
    unsigned idx = (unsigned char)ch;
    ch = (char)(uint8_t)(c.encode_buf[idx] ^ c.shift);
    c.shift ^= idx;
  }
  return out;
}

std::string sql_crypt_decode(const std::string& data, const std::string& key) {
  SqlCrypt c(key);
  std::string out = data;
  for (auto& ch : out) {
    c.shift ^= (unsigned)(c.rnd() * 255.0);
    unsigned idx = ((unsigned char)ch ^ c.shift) & 0xFF;
    ch = (char)c.decode_buf[idx];
    c.shift ^= (unsigned char)ch;
  }
  return out;
}

// --- COMPRESS ------------------------------------------------------------------------

std::string compress(const std::string& in) {
  if (in.empty()) return {};
  ByteBuffer z;
#if defined(AIOS_HAVE_ZLIB)
  uLongf cap = compressBound((uLong)in.size());
  z.resize(cap);
  if (compress2(z.data(), &cap, reinterpret_cast<const Bytef*>(in.data()), (uLong)in.size(), Z_DEFAULT_COMPRESSION) == Z_OK) z.resize(cap);
  else z = zlib_deflate(span_of(in), 6);
#else
  z = zlib_deflate(span_of(in), 6);
#endif
  uint32_t len = (uint32_t)in.size() & 0x3FFFFFFFu;
  std::string out;
  out.reserve(4 + z.size() + 1);
  for (int i = 0; i < 4; ++i) out += (char)(uint8_t)(len >> (8 * i));
  out.append(reinterpret_cast<const char*>(z.data()), z.size());
  // MySQL's guard against trailing-space trimming of the stored value.
  if (out.back() == ' ') out += '.';
  return out;
}

uint32_t uncompressed_length(const std::string& in) {
  if (in.size() < 4) return 0;
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= (uint32_t)(uint8_t)in[i] << (8 * i);
  return v & 0x3FFFFFFFu;
}

bool uncompress(const std::string& in, std::string* out) {
  if (in.empty()) { out->clear(); return true; }
  if (in.size() <= 4) return false;
  uint32_t want = uncompressed_length(in);
  auto r = zlib_inflate(ByteSpan(reinterpret_cast<const uint8_t*>(in.data()) + 4, in.size() - 4));
  if (!r.ok() || r->size() != want) return false;
  out->assign(reinterpret_cast<const char*>(r->data()), r->size());
  return true;
}

// --- named locks ----------------------------------------------------------------------

namespace {

struct NamedLock {
  uint64_t holder = 0;
  uint64_t count = 0;
};

struct LockTable {
  aios::hal::Mutex mu;
  aios::hal::CondVar freed;
  std::map<std::string, NamedLock> locks;
};

LockTable& lock_table() {
  static LockTable* t = new LockTable();
  return *t;
}

// Lock names are case-insensitive, as in MySQL 5.7+.
std::string lock_key(const std::string& name) {
  std::string k = name;
  for (auto& c : k) c = fold(c);
  return k;
}

}  // namespace

int lock_get(const std::string& name, double timeout_s, uint64_t session) {
  LockTable& t = lock_table();
  const std::string key = lock_key(name);
  std::unique_lock<aios::hal::Mutex> lk(t.mu);
  auto available = [&]() {
    auto it = t.locks.find(key);
    return it == t.locks.end() || it->second.holder == session;
  };
  if (!available()) {
    if (timeout_s < 0) {
      t.freed.wait(lk, available);
    } else {
      // The condition variable takes 32-bit milliseconds; a longer timeout
      // waits in slices of that.
      double remaining_ms = timeout_s * 1000.0;
      while (!available() && remaining_ms > 0) {
        uint32_t slice = remaining_ms > 3600000.0 ? 3600000u : (uint32_t)std::ceil(remaining_ms);
        t.freed.wait_for_ms(lk, slice, available);
        remaining_ms -= slice;
      }
      if (!available()) return 0;
    }
  }
  NamedLock& l = t.locks[key];
  l.holder = session;
  ++l.count;
  return 1;
}

int lock_wait_free(const std::string& name, double timeout_s, uint64_t session, const std::atomic<uint8_t>* kill) {
  LockTable& t = lock_table();
  const std::string key = lock_key(name);
  std::unique_lock<aios::hal::Mutex> lk(t.mu);
  auto killed = [&]() { return kill && kill->load(std::memory_order_acquire) != 0; };
  auto available = [&]() {
    auto it = t.locks.find(key);
    return it == t.locks.end() || it->second.holder == session;
  };
  auto done = [&]() { return available() || killed(); };
  if (timeout_s < 0) {
    t.freed.wait(lk, done);
  } else {
    double remaining_ms = timeout_s * 1000.0;
    while (!done() && remaining_ms > 0) {
      uint32_t slice = remaining_ms > 3600000.0 ? 3600000u : (uint32_t)std::ceil(remaining_ms);
      t.freed.wait_for_ms(lk, slice, done);
      remaining_ms -= slice;
    }
  }
  if (killed()) return -1;
  return available() ? 1 : 0;
}

void lock_wake_waiters() {
  LockTable& t = lock_table();
  // Under the table's mutex: a waiter that has just read its flag as clear is then already waiting, and wakes.
  std::lock_guard<aios::hal::Mutex> lk(t.mu);
  t.freed.notify_all();
}

int lock_release(const std::string& name, uint64_t session) {
  LockTable& t = lock_table();
  std::lock_guard<aios::hal::Mutex> lk(t.mu);
  auto it = t.locks.find(lock_key(name));
  if (it == t.locks.end()) return -1;
  if (it->second.holder != session) return 0;
  if (--it->second.count == 0) {
    t.locks.erase(it);
    t.freed.notify_all();
  }
  return 1;
}

bool lock_is_free(const std::string& name) {
  LockTable& t = lock_table();
  std::lock_guard<aios::hal::Mutex> lk(t.mu);
  return t.locks.find(lock_key(name)) == t.locks.end();
}

uint64_t lock_holder(const std::string& name) {
  LockTable& t = lock_table();
  std::lock_guard<aios::hal::Mutex> lk(t.mu);
  auto it = t.locks.find(lock_key(name));
  return it == t.locks.end() ? 0 : it->second.holder;
}

uint64_t lock_release_all(uint64_t session) {
  LockTable& t = lock_table();
  std::lock_guard<aios::hal::Mutex> lk(t.mu);
  uint64_t released = 0;
  for (auto it = t.locks.begin(); it != t.locks.end();) {
    if (it->second.holder == session) {
      released += it->second.count;
      it = t.locks.erase(it);
    } else {
      ++it;
    }
  }
  if (released) t.freed.notify_all();
  return released;
}

}  // namespace aios::userland::sqlfn
