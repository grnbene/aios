#include "aios/channel/rpc_value.hpp"
#include <unordered_map>
#include <cstdlib>

namespace aios::channel {

namespace {

const RpcValue& nothing() {
  static const RpcValue empty;
  return empty;
}

void escape_into(std::string& out, const std::string& text) {
  out.push_back('"');
  for (char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if ((uint8_t)c < 0x20) {
          static const char* HEX = "0123456789abcdef";
          out += "\\u00";
          out.push_back(HEX[((uint8_t)c >> 4) & 15]);
          out.push_back(HEX[(uint8_t)c & 15]);
        } else {
          out.push_back(c);
        }
    }
  }
  out.push_back('"');
}

class Parser {
 public:
  Parser(const char* data, size_t len) : p_(data), end_(data + len) {}

  Result<RpcValue> parse() {
    skip();
    auto v = value(0);
    if (!v.ok()) return v;
    skip();
    if (p_ != end_) return Result<RpcValue>(Status::Invalid, "trailing content after document");
    return v;
  }

 private:
  const char* p_;
  const char* end_;

  void skip() {
    while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
  }

  bool literal(const char* text) {
    size_t n = 0;
    while (text[n]) ++n;
    if ((size_t)(end_ - p_) < n) return false;
    for (size_t i = 0; i < n; ++i) {
      if (p_[i] != text[i]) return false;
    }
    p_ += n;
    return true;
  }

  Result<std::string> string_body() {
    if (p_ >= end_ || *p_ != '"') return Result<std::string>(Status::Invalid, "expected a string");
    ++p_;
    std::string out;
    while (p_ < end_ && *p_ != '"') {
      if (*p_ == '\\') {
        ++p_;
        if (p_ >= end_) return Result<std::string>(Status::Invalid, "escape at end of input");
        char c = *p_++;
        switch (c) {
          case 'n': out.push_back('\n'); break;
          case 't': out.push_back('\t'); break;
          case 'r': out.push_back('\r'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case '/': out.push_back('/'); break;
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case 'u': {
            if (end_ - p_ < 4) return Result<std::string>(Status::Invalid, "short unicode escape");
            uint32_t cp = 0;
            for (int i = 0; i < 4; ++i) {
              char h = p_[i];
              cp <<= 4;
              if (h >= '0' && h <= '9') cp |= (uint32_t)(h - '0');
              else if (h >= 'a' && h <= 'f') cp |= (uint32_t)(h - 'a' + 10);
              else if (h >= 'A' && h <= 'F') cp |= (uint32_t)(h - 'A' + 10);
              else return Result<std::string>(Status::Invalid, "bad unicode escape");
            }
            p_ += 4;
            if (cp < 0x80) {
              out.push_back((char)cp);
            } else if (cp < 0x800) {
              out.push_back((char)(0xc0 | (cp >> 6)));
              out.push_back((char)(0x80 | (cp & 0x3f)));
            } else {
              out.push_back((char)(0xe0 | (cp >> 12)));
              out.push_back((char)(0x80 | ((cp >> 6) & 0x3f)));
              out.push_back((char)(0x80 | (cp & 0x3f)));
            }
            break;
          }
          default: return Result<std::string>(Status::Invalid, "unknown escape");
        }
      } else {
        out.push_back(*p_++);
      }
    }
    if (p_ >= end_) return Result<std::string>(Status::Invalid, "unterminated string");
    ++p_;
    return out;
  }

  Result<RpcValue> value(int depth) {
    if (depth > 64) return Result<RpcValue>(Status::Invalid, "nesting too deep");
    skip();
    if (p_ >= end_) return Result<RpcValue>(Status::Invalid, "empty document");

    char c = *p_;
    if (c == '{') {
      ++p_;
      RpcValue obj = RpcValue::object();
      std::unordered_map<std::string, size_t> index;   // key -> position, once the object is large
      skip();
      if (p_ < end_ && *p_ == '}') { ++p_; return obj; }
      while (true) {
        skip();
        auto key = string_body();
        if (!key.ok()) return Result<RpcValue>(key.error());
        skip();
        if (p_ >= end_ || *p_ != ':') return Result<RpcValue>(Status::Invalid, "expected a colon");
        ++p_;
        auto v = value(depth + 1);
        if (!v.ok()) return v;
        // An object with a vocabulary's worth of keys (a tokenizer.json holds
        // 150,000) cannot afford set()'s linear scan for a duplicate per key.
        if (obj.size() < 16) obj.set(*key, std::move(*v));
        else {
          if (index.empty()) for (size_t i = 0; i < obj.fields().size(); ++i) index.emplace(obj.fields()[i].first, i);
          auto hit = index.find(*key);
          if (hit != index.end()) obj.set(*key, std::move(*v));
          else { index.emplace(*key, obj.size()); obj.set_new(std::move(*key), std::move(*v)); }
        }
        skip();
        if (p_ < end_ && *p_ == ',') { ++p_; continue; }
        if (p_ < end_ && *p_ == '}') { ++p_; return obj; }
        return Result<RpcValue>(Status::Invalid, "expected a comma or closing brace");
      }
    }

    if (c == '[') {
      ++p_;
      RpcValue arr = RpcValue::list();
      skip();
      if (p_ < end_ && *p_ == ']') { ++p_; return arr; }
      while (true) {
        auto v = value(depth + 1);
        if (!v.ok()) return v;
        arr.push(std::move(*v));
        skip();
        if (p_ < end_ && *p_ == ',') { ++p_; continue; }
        if (p_ < end_ && *p_ == ']') { ++p_; return arr; }
        return Result<RpcValue>(Status::Invalid, "expected a comma or closing bracket");
      }
    }

    if (c == '"') {
      auto s = string_body();
      if (!s.ok()) return Result<RpcValue>(s.error());
      return RpcValue::text(*s);
    }

    if (literal("true")) return RpcValue::boolean(true);
    if (literal("false")) return RpcValue::boolean(false);
    if (literal("null")) return RpcValue();

    const char* start = p_;
    if (p_ < end_ && (*p_ == '-' || *p_ == '+')) ++p_;
    while (p_ < end_ && ((*p_ >= '0' && *p_ <= '9') || *p_ == '.' || *p_ == 'e' || *p_ == 'E' ||
                         *p_ == '-' || *p_ == '+')) {
      ++p_;
    }
    if (p_ == start) return Result<RpcValue>(Status::Invalid, "unrecognised token");
    return RpcValue::number(std::string(start, (size_t)(p_ - start)));
  }
};

}  // namespace

RpcValue RpcValue::boolean(bool v) {
  RpcValue r;
  r.kind_ = Kind::Bool;
  r.scalar_ = v ? "true" : "false";
  return r;
}

RpcValue RpcValue::number(const std::string& digits) {
  RpcValue r;
  r.kind_ = Kind::Number;
  r.scalar_ = digits;
  return r;
}

RpcValue RpcValue::number(uint64_t v) { return number(std::to_string(v)); }

RpcValue RpcValue::text(const std::string& v) {
  RpcValue r;
  r.kind_ = Kind::Text;
  r.scalar_ = v;
  return r;
}

RpcValue RpcValue::list() {
  RpcValue r;
  r.kind_ = Kind::List;
  return r;
}

RpcValue RpcValue::object() {
  RpcValue r;
  r.kind_ = Kind::Object;
  return r;
}

bool RpcValue::truthy() const {
  switch (kind_) {
    case Kind::Bool: return scalar_ == "true";
    case Kind::Number: return as_double() != 0.0;
    case Kind::Text: return !scalar_.empty();
    case Kind::List: return !list_.empty();
    case Kind::Object: return !object_.empty();
    case Kind::Null:
    default: return false;
  }
}

std::string RpcValue::as_text() const { return scalar_; }

uint64_t RpcValue::as_u64(uint64_t fallback) const {
  if (kind_ != Kind::Number && kind_ != Kind::Text) return fallback;
  if (scalar_.empty()) return fallback;
  if (scalar_.size() > 2 && scalar_[0] == '0' && (scalar_[1] == 'x' || scalar_[1] == 'X')) {
    uint64_t v = 0;
    for (size_t i = 2; i < scalar_.size(); ++i) {
      char c = scalar_[i];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= (uint64_t)(c - '0');
      else if (c >= 'a' && c <= 'f') v |= (uint64_t)(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= (uint64_t)(c - 'A' + 10);
      else return fallback;
    }
    return v;
  }
  uint64_t v = 0;
  for (char c : scalar_) {
    if (c < '0' || c > '9') return v;
    v = v * 10 + (uint64_t)(c - '0');
  }
  return v;
}

double RpcValue::as_double(double fallback) const {
  if (scalar_.empty()) return fallback;
  const char* p = scalar_.c_str();
  bool neg = false;
  if (*p == '-') { neg = true; ++p; }
  else if (*p == '+') { ++p; }

  // Digits are gathered as one integer and a power of ten, then scaled once:
  // a JSON number may carry an exponent (1e-06), and summing 0.1, 0.01, ...
  // drifts in the last bits.
  uint64_t mant = 0;
  int exp10 = 0, kept = 0;
  bool parsed_any = false;
  auto digit = [&](int d, bool after_point) {
    parsed_any = true;
    if (kept < 19) { mant = mant * 10 + (uint64_t)d; if (mant) ++kept; if (after_point) --exp10; }
    else if (!after_point) ++exp10;
  };
  while (*p >= '0' && *p <= '9') digit(*p++ - '0', false);
  if (*p == '.') {
    ++p;
    while (*p >= '0' && *p <= '9') digit(*p++ - '0', true);
  }
  if (!parsed_any) return fallback;
  if (*p == 'e' || *p == 'E') {
    ++p;
    bool eneg = false;
    if (*p == '-') { eneg = true; ++p; }
    else if (*p == '+') { ++p; }
    int e = 0;
    while (*p >= '0' && *p <= '9') { if (e < 100000) e = e * 10 + (*p - '0'); ++p; }
    exp10 += eneg ? -e : e;
  }
  double val = (double)mant;
  // Applied 256 decades at a time at most, so the scale itself never
  // overflows on the way to a value near the ends of the double range.
  while (exp10 != 0 && val != 0.0) {
    const int step = exp10 > 256 ? 256 : exp10 < -256 ? -256 : exp10;
    double scale = 1.0, base = 10.0;
    for (unsigned n = (unsigned)(step < 0 ? -step : step); n; n >>= 1, base *= base)
      if (n & 1) scale *= base;
    val = step < 0 ? val / scale : val * scale;
    exp10 -= step;
  }
  return neg ? -val : val;
}

ByteBuffer RpcValue::as_quantity() const {
  ByteBuffer out;
  if (scalar_.size() < 3 || scalar_[0] != '0' || (scalar_[1] != 'x' && scalar_[1] != 'X')) return out;
  std::string digits = scalar_.substr(2);
  if (digits.size() % 2) digits.insert(digits.begin(), '0');
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i + 1 < digits.size(); i += 2) {
    int hi = nibble(digits[i]);
    int lo = nibble(digits[i + 1]);
    if (hi < 0 || lo < 0) return ByteBuffer();
    out.push_back((uint8_t)((hi << 4) | lo));
  }
  size_t lead = 0;
  while (lead + 1 < out.size() && out[lead] == 0) ++lead;
  return ByteBuffer(out.begin() + (ptrdiff_t)lead, out.end());
}

void RpcValue::push(RpcValue v) {
  kind_ = Kind::List;
  list_.push_back(std::move(v));
}

void RpcValue::set(const std::string& key, RpcValue v) {
  kind_ = Kind::Object;
  for (auto& field : object_) {
    if (field.first == key) {
      field.second = std::move(v);
      return;
    }
  }
  object_.emplace_back(key, std::move(v));
}

void RpcValue::set_new(std::string key, RpcValue v) {
  kind_ = Kind::Object;
  object_.emplace_back(std::move(key), std::move(v));
}

bool RpcValue::has(const std::string& key) const {
  for (const auto& field : object_) {
    if (field.first == key) return true;
  }
  return false;
}

size_t RpcValue::size() const {
  if (kind_ == Kind::List) return list_.size();
  if (kind_ == Kind::Object) return object_.size();
  return 0;
}

const RpcValue& RpcValue::operator[](size_t i) const {
  if (kind_ != Kind::List || i >= list_.size()) return nothing();
  return list_[i];
}

const RpcValue& RpcValue::operator[](const std::string& key) const {
  for (const auto& field : object_) {
    if (field.first == key) return field.second;
  }
  return nothing();
}

std::string RpcValue::render() const {
  switch (kind_) {
    case Kind::Null: return "null";
    case Kind::Bool: return scalar_;
    case Kind::Number: return scalar_.empty() ? "0" : scalar_;
    case Kind::Text: {
      std::string out;
      escape_into(out, scalar_);
      return out;
    }
    case Kind::List: {
      std::string out = "[";
      for (size_t i = 0; i < list_.size(); ++i) {
        if (i) out.push_back(',');
        out += list_[i].render();
      }
      out.push_back(']');
      return out;
    }
    case Kind::Object:
    default: {
      std::string out = "{";
      for (size_t i = 0; i < object_.size(); ++i) {
        if (i) out.push_back(',');
        escape_into(out, object_[i].first);
        out.push_back(':');
        out += object_[i].second.render();
      }
      out.push_back('}');
      return out;
    }
  }
}

Result<RpcValue> rpc_parse(ByteSpan document) {
  Parser p(reinterpret_cast<const char*>(document.data()), document.size());
  return p.parse();
}

Result<RpcValue> rpc_parse(const std::string& document) {
  Parser p(document.data(), document.size());
  return p.parse();
}

std::string rpc_request(const std::string& method, const std::vector<RpcValue>& params, uint64_t id) {
  RpcValue envelope = RpcValue::object();
  envelope.set("jsonrpc", RpcValue::text("2.0"));
  envelope.set("id", RpcValue::number(id));
  envelope.set("method", RpcValue::text(method));
  RpcValue list = RpcValue::list();
  for (const auto& p : params) list.push(p);
  envelope.set("params", list);
  return envelope.render();
}

}  // namespace aios::channel
