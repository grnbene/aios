// JSON. See include/aios/core/json.hpp.
#include "aios/core/json.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace aios::json {

namespace {
const Value kNull;
const std::string kEmpty;
}  // namespace

const std::string& Value::str() const { return type_ == Type::String ? str_ : kEmpty; }

int64_t Value::i64(int64_t def) const {
  if (type_ != Type::Number) return def;
  if (is_int_) return int_;
  if (!(num_ > -9.2e18 && num_ < 9.2e18)) return def;
  return static_cast<int64_t>(num_);
}

const Value& Value::operator[](std::string_view key) const {
  if (type_ == Type::Object)
    for (const auto& kv : obj_)
      if (kv.first == key) return kv.second;
  return kNull;
}

bool Value::has(std::string_view key) const {
  if (type_ != Type::Object) return false;
  for (const auto& kv : obj_)
    if (kv.first == key) return true;
  return false;
}

const Value& Value::operator[](size_t i) const { return (type_ == Type::Array && i < arr_.size()) ? arr_[i] : kNull; }

Value& Value::set(std::string key, Value v) {
  if (type_ != Type::Object) { *this = Value::object(); }
  for (auto& kv : obj_)
    if (kv.first == key) { kv.second = std::move(v); return *this; }
  obj_.emplace_back(std::move(key), std::move(v));
  return *this;
}

Value& Value::push(Value v) {
  if (type_ != Type::Array) { *this = Value::array(); }
  arr_.push_back(std::move(v));
  return *this;
}

void Value::erase(std::string_view key) {
  if (type_ != Type::Object) return;
  for (size_t i = 0; i < obj_.size(); ++i)
    if (obj_[i].first == key) { obj_.erase(obj_.begin() + static_cast<long>(i)); return; }
}

std::string quote(std::string_view s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\b': o += "\\b"; break;
      case '\f': o += "\\f"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += static_cast<char>(c);
    }
  }
  return o + "\"";
}

static void dump_into(const Value& v, std::string& out) {
  switch (v.type()) {
    case Value::Type::Null: out += "null"; break;
    case Value::Type::Bool: out += v.boolean_or() ? "true" : "false"; break;
    case Value::Type::Number: {
      double d = v.num();
      if (d > -9e15 && d < 9e15 && d == static_cast<double>(static_cast<int64_t>(d))) { out += std::to_string(static_cast<int64_t>(d)); break; }
      if (!(d > -1.7976931348623157e308 && d < 1.7976931348623157e308)) { out += "null"; break; }
      char b[40];
      std::snprintf(b, sizeof b, "%.17g", d);
      out += b;
      break;
    }
    case Value::Type::String: out += quote(v.str()); break;
    case Value::Type::Array: {
      out += '[';
      bool first = true;
      for (const auto& e : v.items()) { if (!first) out += ','; first = false; dump_into(e, out); }
      out += ']';
      break;
    }
    case Value::Type::Object: {
      out += '{';
      bool first = true;
      for (const auto& kv : v.members()) {
        if (!first) out += ',';
        first = false;
        out += quote(kv.first);
        out += ':';
        dump_into(kv.second, out);
      }
      out += '}';
      break;
    }
  }
}

std::string Value::dump() const {
  std::string o;
  dump_into(*this, o);
  return o;
}

class Parser {
 public:
  explicit Parser(std::string_view t) : t_(t) {}

  Result<Value> run() {
    if (t_.size() > (64u << 20)) return Result<Value>(Status::Exhausted, "JSON text over 64 MB");
    skip();
    Value v;
    if (!value(v, 0)) return Result<Value>(Status::Invalid, err_ + " at offset " + std::to_string(p_));
    skip();
    if (p_ != t_.size()) return Result<Value>(Status::Invalid, "trailing characters after the JSON value at offset " + std::to_string(p_));
    return v;
  }

 private:
  bool fail(const char* m) { err_ = m; return false; }
  void skip() { while (p_ < t_.size() && (t_[p_] == ' ' || t_[p_] == '\t' || t_[p_] == '\n' || t_[p_] == '\r')) ++p_; }

  bool value(Value& out, int depth) {
    if (depth > 64) return fail("nesting deeper than 64");
    skip();
    if (p_ >= t_.size()) return fail("unexpected end of JSON");
    char c = t_[p_];
    if (c == '{') return object(out, depth);
    if (c == '[') return array(out, depth);
    if (c == '"') { std::string s; if (!str(s)) return false; out = Value::string(std::move(s)); return true; }
    if (c == 't') return lit("true") ? (out = Value::boolean(true), true) : false;
    if (c == 'f') return lit("false") ? (out = Value::boolean(false), true) : false;
    if (c == 'n') return lit("null") ? (out = Value::null(), true) : false;
    return number(out);
  }

  bool lit(const char* w) {
    size_t n = std::strlen(w);
    if (t_.compare(p_, n, w) != 0) return fail("invalid literal");
    p_ += n;
    return true;
  }

  bool number(Value& out) {
    size_t s = p_;
    if (p_ < t_.size() && t_[p_] == '-') ++p_;
    size_t ds = p_;
    if (p_ < t_.size() && t_[p_] == '0') ++p_;
    else while (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9') ++p_;
    if (p_ == ds) return fail("invalid number");
    bool is_int = true;
    if (p_ < t_.size() && t_[p_] == '.') {
      is_int = false;
      ++p_;
      size_t fs = p_;
      while (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9') ++p_;
      if (p_ == fs) return fail("invalid number");
    }
    if (p_ < t_.size() && (t_[p_] == 'e' || t_[p_] == 'E')) {
      is_int = false;
      ++p_;
      if (p_ < t_.size() && (t_[p_] == '+' || t_[p_] == '-')) ++p_;
      size_t es = p_;
      while (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9') ++p_;
      if (p_ == es) return fail("invalid number");
    }
    std::string text(t_.substr(s, p_ - s));
    if (is_int && text.size() < 19) { out = Value::integer(std::strtoll(text.c_str(), nullptr, 10)); return true; }
    double d = std::strtod(text.c_str(), nullptr);
    if (!(d > -1.7976931348623157e308 && d < 1.7976931348623157e308)) return fail("number out of range");
    out = Value::number(d);
    return true;
  }

  static int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }

  bool hex4(uint32_t& v) {
    if (p_ + 4 > t_.size()) return fail("truncated \\u escape");
    v = 0;
    for (int i = 0; i < 4; ++i) {
      int h = hex(t_[p_ + static_cast<size_t>(i)]);
      if (h < 0) return fail("invalid \\u escape");
      v = v * 16 + static_cast<uint32_t>(h);
    }
    p_ += 4;
    return true;
  }

  static void utf8(std::string& o, uint32_t cp) {
    if (cp < 0x80) o += static_cast<char>(cp);
    else if (cp < 0x800) { o += static_cast<char>(0xC0 | (cp >> 6)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
      o += static_cast<char>(0xE0 | (cp >> 12)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      o += static_cast<char>(0xF0 | (cp >> 18)); o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }

  bool str(std::string& out) {
    ++p_;   // opening quote
    for (;;) {
      if (p_ >= t_.size()) return fail("unterminated string");
      unsigned char c = static_cast<unsigned char>(t_[p_++]);
      if (c == '"') return true;
      if (c < 0x20) return fail("control character in a string");
      if (c != '\\') { out += static_cast<char>(c); continue; }
      if (p_ >= t_.size()) return fail("unterminated escape");
      char e = t_[p_++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          uint32_t cp;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp < 0xDC00) {
            if (p_ + 6 <= t_.size() && t_[p_] == '\\' && t_[p_ + 1] == 'u') {
              size_t save = p_;
              p_ += 2;
              uint32_t lo;
              if (hex4(lo) && lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              else { p_ = save; cp = 0xFFFD; }
            } else cp = 0xFFFD;
          } else if (cp >= 0xDC00 && cp < 0xE000) cp = 0xFFFD;
          utf8(out, cp);
          break;
        }
        default: return fail("invalid escape");
      }
    }
  }

  bool array(Value& out, int depth) {
    ++p_;
    out = Value::array();
    skip();
    if (p_ < t_.size() && t_[p_] == ']') { ++p_; return true; }
    for (;;) {
      Value e;
      if (!value(e, depth + 1)) return false;
      out.push(std::move(e));
      skip();
      if (p_ >= t_.size()) return fail("unterminated array");
      if (t_[p_] == ',') { ++p_; continue; }
      if (t_[p_] == ']') { ++p_; return true; }
      return fail("expected , or ] in an array");
    }
  }

  bool object(Value& out, int depth) {
    ++p_;
    out = Value::object();
    skip();
    if (p_ < t_.size() && t_[p_] == '}') { ++p_; return true; }
    for (;;) {
      skip();
      if (p_ >= t_.size() || t_[p_] != '"') return fail("expected a string key");
      std::string k;
      if (!str(k)) return false;
      skip();
      if (p_ >= t_.size() || t_[p_] != ':') return fail("expected : after a key");
      ++p_;
      Value e;
      if (!value(e, depth + 1)) return false;
      out.obj_.emplace_back(std::move(k), std::move(e));
      skip();
      if (p_ >= t_.size()) return fail("unterminated object");
      if (t_[p_] == ',') { ++p_; continue; }
      if (t_[p_] == '}') { ++p_; return true; }
      return fail("expected , or } in an object");
    }
  }

  std::string_view t_;
  size_t p_ = 0;
  std::string err_;
};

Result<Value> parse(std::string_view text) { return Parser(text).run(); }

}  // namespace aios::json
