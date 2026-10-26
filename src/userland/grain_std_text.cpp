// grain's formatted text, keyed sorting and grouping, and identifiers.
//
//   format(template, values...)   "{} of {}", "{1} {0}", "{name}" (from one map
//                                  argument); after a colon, a spec:
//                                  [[fill]align][sign][0][width][,][.precision][type]
//                                  align < > ^, sign + or space, type d f e x X o b s %
//                                  - "{:>8.2f}", "{:,}", "{:08.3f}", "{:x}". {{ and }}
//                                  are literal braces.
//   sort_by(list, fn [, "desc"])  a new list ordered by fn(item): numbers by value,
//                                  strings by bytes; stable, so equal keys keep
//                                  their order
//   group_by(list, fn)            a map from each key (as text) to the items that
//                                  have it, in the order they came
//   uuid()                         a random (version 4) UUID
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "aios/userland/grain.hpp"
#include "grain_builtins_internal.hpp"

namespace aios::userland::grain {

namespace {

struct Spec {
  char fill = ' ', align = 0, sign = '-', type = 0;
  bool zero = false, group = false;
  int width = -1, precision = -1;
};

bool parse_spec(const std::string& s, Spec& sp, std::string& error) {
  size_t i = 0;
  auto is_align = [](char c) { return c == '<' || c == '>' || c == '^'; };
  if (s.size() >= 2 && is_align(s[1])) { sp.fill = s[0]; sp.align = s[1]; i = 2; }
  else if (!s.empty() && is_align(s[0])) { sp.align = s[0]; i = 1; }
  if (i < s.size() && (s[i] == '+' || s[i] == ' ' || s[i] == '-')) sp.sign = s[i++];
  if (i < s.size() && s[i] == '0') { sp.zero = true; ++i; }
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') sp.width = (sp.width < 0 ? 0 : sp.width * 10) + (s[i++] - '0');
  if (i < s.size() && s[i] == ',') { sp.group = true; ++i; }
  if (i < s.size() && s[i] == '.') {
    ++i;
    sp.precision = 0;
    if (i >= s.size() || s[i] < '0' || s[i] > '9') { error = "format(): a precision needs digits after the '.'"; return false; }
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') sp.precision = sp.precision * 10 + (s[i++] - '0');
  }
  if (i < s.size()) sp.type = s[i++];
  if (i != s.size() || (sp.type && std::string("dfexXobs%").find(sp.type) == std::string::npos)) {
    error = "format(): \"" + s + "\" is not a format spec";
    return false;
  }
  return true;
}

std::string group_thousands(const std::string& digits) {
  std::string out;
  const size_t n = digits.size();
  for (size_t k = 0; k < n; ++k) {
    out.push_back(digits[k]);
    if ((n - k - 1) % 3 == 0 && k + 1 < n) out.push_back(',');
  }
  return out;
}

std::string base_digits(uint64_t v, int base, bool upper) {
  if (v == 0) return "0";
  const char* d = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  std::string s;
  while (v) { s.insert(s.begin(), d[v % static_cast<uint64_t>(base)]); v /= static_cast<uint64_t>(base); }
  return s;
}

bool render(const Value& v, const Spec& sp, std::string& out, std::string& error) {
  std::string body, sign;
  const char t = sp.type;
  if (t == 's' || (!t && !v.is_number())) {
    body = v.to_text();
    if (sp.precision >= 0 && static_cast<int>(body.size()) > sp.precision) body.resize(static_cast<size_t>(sp.precision));
  } else {
    if (!v.is_number() && v.type != Type::Decimal) { error = std::string("format(): {:") + t + "} wants a number, got a " + type_name(v.type); return false; }
    const double d = v.type == Type::Decimal ? decimal_to_double(v) : v.as_double();
    const bool neg = v.type == Type::Decimal ? decimal_compare(v, Value::integer(0)) < 0
                   : v.type == Type::Int ? v.i < 0 : std::signbit(d);
    if (v.type == Type::Decimal && (t == 'f' || (!t && sp.precision >= 0))) {
      // exact: {:.2f} of a decimal rounds half away from zero on its digits, not on a real's
      Value r = v;
      if (sp.precision >= 0 && !decimal_round(v, sp.precision, r, error)) return false;
      body = decimal_text(r);
      if (!body.empty() && body[0] == '-') body.erase(0, 1);
      if (sp.precision >= 0) {
        const size_t dot = body.find('.');
        const size_t have = dot == std::string::npos ? 0 : body.size() - dot - 1;
        if (have < (size_t)sp.precision) {
          if (dot == std::string::npos && sp.precision > 0) body += ".";
          body.append((size_t)sp.precision - have, '0');
        }
      }
      if (sp.group) {
        const size_t dot = body.find('.');
        body = group_thousands(body.substr(0, dot)) + (dot == std::string::npos ? "" : body.substr(dot));
      }
    } else if (t == 'd' || t == 'x' || t == 'X' || t == 'o' || t == 'b') {
      if (v.type != Type::Int && d != std::floor(d)) { error = std::string("format(): {:") + t + "} wants a whole number, got " + v.to_text(); return false; }
      const int64_t n = v.type == Type::Int ? v.i : static_cast<int64_t>(d);
      const uint64_t mag = n < 0 ? 0 - static_cast<uint64_t>(n) : static_cast<uint64_t>(n);
      body = base_digits(mag, t == 'x' || t == 'X' ? 16 : t == 'o' ? 8 : t == 'b' ? 2 : 10, t == 'X');
      if (sp.group && t == 'd') body = group_thousands(body);
    } else if (t == 'f' || t == '%' || t == 'e' || (!t && (sp.precision >= 0 || (sp.group && v.type != Type::Int)))) {
      const double x = std::fabs(t == '%' ? d * 100.0 : d);
      const int prec = sp.precision >= 0 ? sp.precision : 6;
      char buf[512];
      std::snprintf(buf, sizeof buf, t == 'e' ? "%.*e" : "%.*f", prec, x);
      body = buf;
      if (sp.group && t != 'e') {
        const size_t dot = body.find('.');
        body = group_thousands(body.substr(0, dot)) + (dot == std::string::npos ? "" : body.substr(dot));
      }
      if (t == '%') body += "%";
    } else {
      // No type: the value as print shows it, without its sign (added below).
      body = v.to_text();
      if (!body.empty() && body[0] == '-') body.erase(0, 1);
      if (sp.group && v.type == Type::Int) body = group_thousands(body);
    }
    sign = neg ? "-" : sp.sign == '+' ? "+" : sp.sign == ' ' ? " " : "";
  }
  const int len = static_cast<int>(sign.size() + body.size());
  if (sp.width > len) {
    const size_t gap = static_cast<size_t>(sp.width - len);
    if (sp.zero && !sp.align && v.is_number()) {
      out += sign + std::string(gap, '0') + body;
      return true;
    }
    const char align = sp.align ? sp.align : (v.is_number() && t != 's' ? '>' : '<');
    const size_t left = align == '>' ? gap : align == '^' ? gap / 2 : 0;
    out += std::string(left, sp.fill) + sign + body + std::string(gap - left, sp.fill);
    return true;
  }
  out += sign + body;
  return true;
}

Value bi_format(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "format")) return Value::nil();
  const std::string& f = args[0].str();
  MapObj* names = args.size() == 2 && args[1].type == Type::Map ? static_cast<MapObj*>(args[1].obj.get()) : nullptr;
  std::string out;
  size_t next = 1;
  for (size_t i = 0; i < f.size(); ++i) {
    const char c = f[i];
    if (c == '}') {
      if (i + 1 < f.size() && f[i + 1] == '}') { out.push_back('}'); ++i; continue; }
      error = "format(): a lone '}' (write }} for a brace)";
      return Value::nil();
    }
    if (c != '{') { out.push_back(c); continue; }
    if (i + 1 < f.size() && f[i + 1] == '{') { out.push_back('{'); ++i; continue; }
    const size_t close = f.find('}', i + 1);
    if (close == std::string::npos) { error = "format(): a '{' with no '}'"; return Value::nil(); }
    const std::string field = f.substr(i + 1, close - i - 1);
    i = close;
    const size_t colon = field.find(':');
    const std::string key = field.substr(0, colon);
    Spec sp;
    if (colon != std::string::npos && !parse_spec(field.substr(colon + 1), sp, error)) return Value::nil();
    const Value* v = nullptr;
    if (key.empty()) {
      if (next >= args.size()) { error = "format(): more {} than values (" + std::to_string(args.size() - 1) + ")"; return Value::nil(); }
      v = &args[next++];
    } else if (std::all_of(key.begin(), key.end(), [](char k) { return k >= '0' && k <= '9'; })) {
      // strtoull, not stoul: bare metal builds with exceptions off (and the digits were checked above).
      const size_t at = static_cast<size_t>(std::strtoull(key.c_str(), nullptr, 10)) + 1;
      if (at >= args.size()) { error = "format(): {" + key + "} but only " + std::to_string(args.size() - 1) + " value(s)"; return Value::nil(); }
      v = &args[at];
    } else {
      if (!names) { error = "format(): {" + key + "} names a field, so pass one map of values"; return Value::nil(); }
      v = names->find(key);
      if (!v) { error = "format(): the map has no \"" + key + "\""; return Value::nil(); }
    }
    if (!render(*v, sp, out, error)) return Value::nil();
  }
  return Value::string(std::move(out));
}

// -1, 0, 1 for two sort keys; false (with error) for keys that cannot be compared.
bool compare_keys(const Value& a, const Value& b, int& r, std::string& error) {
  if (a.is_number() && b.is_number()) {
    if (a.type == Type::Int && b.type == Type::Int) r = a.i < b.i ? -1 : a.i > b.i ? 1 : 0;
    else { const double x = a.as_double(), y = b.as_double(); r = x < y ? -1 : x > y ? 1 : 0; }
    return true;
  }
  if (a.type == Type::Str && b.type == Type::Str) { const int c = a.str().compare(b.str()); r = c < 0 ? -1 : c > 0 ? 1 : 0; return true; }
  if (a.type == Type::Bool && b.type == Type::Bool) { r = (a.b == b.b) ? 0 : a.b ? 1 : -1; return true; }
  error = std::string("sort_by(): cannot order a ") + type_name(a.type) + " against a " + type_name(b.type);
  return false;
}

Value bi_sort_by(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_list(args[0], error, "sort_by")) return Value::nil();
  bool desc = false;
  if (args.size() > 2) {
    if (!want_str(args[2], error, "sort_by")) return Value::nil();
    if (args[2].str() == "desc") desc = true;
    else if (args[2].str() != "asc") { error = "sort_by(): the order is \"asc\" or \"desc\""; return Value::nil(); }
  }
  const std::vector<Value> items = args[0].list_items();   // a copy: the key function may change the list
  std::vector<std::pair<Value, size_t>> keyed;
  keyed.reserve(items.size());
  for (size_t k = 0; k < items.size(); ++k) {
    Value key = vm.call(args[1], {items[k]}, error);
    if (!error.empty()) return Value::nil();
    keyed.emplace_back(std::move(key), k);
  }
  bool failed = false;
  std::stable_sort(keyed.begin(), keyed.end(), [&](const auto& x, const auto& y) {
    if (failed) return false;
    int r = 0;
    if (!compare_keys(x.first, y.first, r, error)) { failed = true; return false; }
    return desc ? r > 0 : r < 0;
  });
  if (failed) return Value::nil();
  std::vector<Value> out;
  out.reserve(keyed.size());
  for (const auto& kv : keyed) out.push_back(items[kv.second]);
  return Value::list(std::move(out));
}

Value bi_group_by(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_list(args[0], error, "group_by")) return Value::nil();
  const std::vector<Value> items = args[0].list_items();
  auto groups = std::make_shared<MapObj>();
  for (const auto& item : items) {
    Value key = vm.call(args[1], {item}, error);
    if (!error.empty()) return Value::nil();
    const std::string k = key.to_text();
    if (Value* g = groups->find(k)) g->list_items().push_back(item);
    else groups->put(k, Value::list({item}));
  }
  return Value::object(Type::Map, groups);
}

// key_by(list, key_fn): a map from each key (as text) to the FIRST item with it, keys in the order first met.
Value bi_key_by(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_list(args[0], error, "key_by")) return Value::nil();
  const std::vector<Value> items = args[0].list_items();
  auto out = std::make_shared<MapObj>();
  for (const auto& item : items) {
    Value key = vm.call(args[1], {item}, error);
    if (!error.empty()) return Value::nil();
    const std::string k = key.to_text();
    if (!out->find(k)) out->put(k, item);
  }
  return Value::object(Type::Map, out);
}

// sum_by(list, key_fn, value_fn): a map from each key (as text) to the sum of value_fn over its items, as `+` adds:
// ints stay ints, a decimal makes the sum exact, a real makes it real. A nil value counts as nothing.
Value bi_sum_by(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_list(args[0], error, "sum_by")) return Value::nil();
  const std::vector<Value> items = args[0].list_items();
  auto out = std::make_shared<MapObj>();
  for (const auto& item : items) {
    Value key = vm.call(args[1], {item}, error);
    if (!error.empty()) return Value::nil();
    Value v = vm.call(args[2], {item}, error);
    if (!error.empty()) return Value::nil();
    if (v.type == Type::Nil) { if (!out->find(key.to_text())) out->put(key.to_text(), Value::integer(0)); continue; }
    if (v.type != Type::Int && v.type != Type::Real && v.type != Type::Decimal) {
      error = std::string("sum_by(): the value function gave a ") + type_name(v.type) + ", not a number";
      return Value::nil();
    }
    const std::string k = key.to_text();
    Value* cur = out->find(k);
    if (!cur) { out->put(k, v); continue; }
    Value sum;
    if (cur->type == Type::Int && v.type == Type::Int) sum = Value::integer(cur->i + v.i);
    else if (cur->type == Type::Decimal || v.type == Type::Decimal) {
      if (!decimal_arith('+', *cur, v, sum, error)) { error = "sum_by(): " + error; return Value::nil(); }
    } else sum = Value::real(cur->as_double() + v.as_double());
    *cur = sum;
  }
  return Value::object(Type::Map, out);
}

Value bi_uuid(Vm&, std::vector<Value>&, std::string&) {
  thread_local std::mt19937_64 rng{[] {
#if defined(AIOS_METAL)
    // Bare metal has no entropy device behind std::random_device: the cycle
    // counter at first use and a stack address differ from boot to boot.
    uint64_t seed = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    int here = 0;
    return seed ^ (reinterpret_cast<uintptr_t>(&here) * 0x9E3779B97F4A7C15ull);
#else
    std::random_device rd;
    return (static_cast<uint64_t>(rd()) << 32) ^ rd();
#endif
  }()};
  uint8_t b[16];
  for (int k = 0; k < 16; k += 8) {
    const uint64_t r = rng();
    for (int j = 0; j < 8; ++j) b[k + j] = static_cast<uint8_t>(r >> (8 * j));
  }
  b[6] = static_cast<uint8_t>((b[6] & 0x0F) | 0x40);   // version 4
  b[8] = static_cast<uint8_t>((b[8] & 0x3F) | 0x80);   // RFC 4122 variant
  static const char* hex = "0123456789abcdef";
  std::string s;
  for (int k = 0; k < 16; ++k) {
    if (k == 4 || k == 6 || k == 8 || k == 10) s.push_back('-');
    s.push_back(hex[b[k] >> 4]);
    s.push_back(hex[b[k] & 15]);
  }
  return Value::string(std::move(s));
}

std::string unescape_xml(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '&') {
      size_t semi = in.find(';', i + 1);
      if (semi != std::string::npos && semi - i <= 10) {
        std::string ent = in.substr(i + 1, semi - (i + 1));
        if (ent == "lt") { out.push_back('<'); i = semi; continue; }
        if (ent == "gt") { out.push_back('>'); i = semi; continue; }
        if (ent == "amp") { out.push_back('&'); i = semi; continue; }
        if (ent == "quot") { out.push_back('"'); i = semi; continue; }
        if (ent == "apos") { out.push_back('\''); i = semi; continue; }
        if (!ent.empty() && ent[0] == '#') {
          uint32_t cp = 0;
          bool ok = false;
          if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')) {
            try {
              cp = static_cast<uint32_t>(std::stoul(ent.substr(2), nullptr, 16));
              ok = true;
            } catch (...) {}
          } else {
            try {
              cp = static_cast<uint32_t>(std::stoul(ent.substr(1), nullptr, 10));
              ok = true;
            } catch (...) {}
          }
          if (ok) {
            if (cp <= 0x7F) {
              out.push_back(static_cast<char>(cp));
            } else if (cp <= 0x7FF) {
              out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
              out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp <= 0xFFFF) {
              out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
              out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
              out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp <= 0x10FFFF) {
              out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
              out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
              out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
              out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            i = semi;
            continue;
          }
        }
      }
    }
    out.push_back(in[i]);
  }
  return out;
}

std::string escape_xml(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 16);
  for (char c : in) {
    switch (c) {
      case '&': out.append("&amp;"); break;
      case '<': out.append("&lt;"); break;
      case '>': out.append("&gt;"); break;
      case '"': out.append("&quot;"); break;
      case '\'': out.append("&apos;"); break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

inline bool is_xml_space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

inline std::string trim_spaces(const std::string& s) {
  size_t start = 0;
  while (start < s.size() && is_xml_space(s[start])) start++;
  size_t end = s.size();
  while (end > start && is_xml_space(s[end - 1])) end--;
  return s.substr(start, end - start);
}

class XmlParser {
 public:
  explicit XmlParser(const std::string& src) : s(src), pos(0) {}

  Value parse(std::string& err) {
    skip_prolog(err);
    if (!err.empty()) return Value::nil();
    skip_ws();
    if (pos >= s.size()) {
      err = "xml_parse: empty or missing root element";
      return Value::nil();
    }
    Value root = parse_element(err);
    if (!err.empty()) return Value::nil();

    skip_misc(err);
    if (!err.empty()) return Value::nil();

    return root;
  }

 private:
  const std::string& s;
  size_t pos;

  void skip_ws() {
    while (pos < s.size() && is_xml_space(s[pos])) pos++;
  }

  char peek() const { return pos < s.size() ? s[pos] : '\0'; }

  bool starts_with(const char* prefix, size_t len) const {
    return pos + len <= s.size() && s.compare(pos, len, prefix) == 0;
  }

  void skip_misc(std::string& err) {
    while (pos < s.size()) {
      skip_ws();
      if (starts_with("<!--", 4)) {
        pos += 4;
        size_t end = s.find("-->", pos);
        if (end == std::string::npos) {
          err = "xml_parse: unclosed comment";
          return;
        }
        pos = end + 3;
      } else if (starts_with("<?", 2)) {
        pos += 2;
        size_t end = s.find("?>", pos);
        if (end == std::string::npos) {
          err = "xml_parse: unclosed processing instruction";
          return;
        }
        pos = end + 2;
      } else {
        break;
      }
    }
  }

  void skip_prolog(std::string& err) {
    while (pos < s.size()) {
      skip_ws();
      if (starts_with("<!--", 4)) {
        pos += 4;
        size_t end = s.find("-->", pos);
        if (end == std::string::npos) {
          err = "xml_parse: unclosed comment";
          return;
        }
        pos = end + 3;
      } else if (starts_with("<?", 2)) {
        pos += 2;
        size_t end = s.find("?>", pos);
        if (end == std::string::npos) {
          err = "xml_parse: unclosed processing instruction";
          return;
        }
        pos = end + 2;
      } else if (starts_with("<!DOCTYPE", 9) || starts_with("<!doctype", 9)) {
        pos += 9;
        int depth = 1;
        while (pos < s.size() && depth > 0) {
          if (s[pos] == '<') depth++;
          else if (s[pos] == '>') depth--;
          pos++;
        }
        if (depth > 0) {
          err = "xml_parse: unclosed DOCTYPE declaration";
          return;
        }
      } else {
        break;
      }
    }
  }

  bool is_name_char(char c) const {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ':' || c == '.';
  }

  std::string parse_name() {
    size_t start = pos;
    while (pos < s.size() && is_name_char(s[pos])) pos++;
    return s.substr(start, pos - start);
  }

  Value parse_element(std::string& err) {
    skip_ws();
    if (peek() != '<') {
      err = "xml_parse: expected '<' at start of element";
      return Value::nil();
    }
    pos++; // skip '<'

    std::string tag = parse_name();
    if (tag.empty()) {
      err = "xml_parse: expected tag name";
      return Value::nil();
    }

    auto attrs_map = std::make_shared<MapObj>();
    auto children_list = std::make_shared<ListObj>(std::vector<Value>{});

    while (pos < s.size()) {
      skip_ws();
      char c = peek();
      if (c == '>' || (c == '/' && pos + 1 < s.size() && s[pos + 1] == '>')) {
        break;
      }
      std::string attr_name = parse_name();
      if (attr_name.empty()) {
        err = "xml_parse: invalid attribute in tag <" + tag + ">";
        return Value::nil();
      }
      skip_ws();
      std::string attr_val;
      if (peek() == '=') {
        pos++; // skip '='
        skip_ws();
        char quote = peek();
        if (quote == '"' || quote == '\'') {
          pos++; // skip quote
          size_t val_start = pos;
          while (pos < s.size() && s[pos] != quote) pos++;
          if (pos >= s.size()) {
            err = "xml_parse: unterminated attribute quote for " + attr_name;
            return Value::nil();
          }
          attr_val = unescape_xml(s.substr(val_start, pos - val_start));
          pos++; // skip quote
        } else {
          size_t val_start = pos;
          while (pos < s.size() && !is_xml_space(s[pos]) && s[pos] != '>' && s[pos] != '/') pos++;
          attr_val = unescape_xml(s.substr(val_start, pos - val_start));
        }
      } else {
        attr_val = "true";
      }
      attrs_map->put(attr_name, Value::string(attr_val));
    }

    skip_ws();
    if (peek() == '/' && pos + 1 < s.size() && s[pos + 1] == '>') {
      pos += 2;
      auto elem = std::make_shared<MapObj>();
      elem->put("tag", Value::string(tag));
      elem->put("attrs", Value::object(Type::Map, attrs_map));
      elem->put("children", Value::object(Type::List, children_list));
      elem->put("text", Value::string(""));
      return Value::object(Type::Map, elem);
    }

    if (peek() != '>') {
      err = "xml_parse: expected '>' to close tag <" + tag + ">";
      return Value::nil();
    }
    pos++; // skip '>'

    std::string text_accum;

    while (pos < s.size()) {
      size_t next_lt = s.find('<', pos);
      if (next_lt == std::string::npos) {
        err = "xml_parse: unclosed tag <" + tag + ">";
        return Value::nil();
      }
      if (next_lt > pos) {
        text_accum.append(unescape_xml(s.substr(pos, next_lt - pos)));
        pos = next_lt;
      }

      if (starts_with("<!--", 4)) {
        pos += 4;
        size_t end = s.find("-->", pos);
        if (end == std::string::npos) {
          err = "xml_parse: unclosed comment in <" + tag + ">";
          return Value::nil();
        }
        pos = end + 3;
      } else if (starts_with("<?", 2)) {
        pos += 2;
        size_t end = s.find("?>", pos);
        if (end == std::string::npos) {
          err = "xml_parse: unclosed processing instruction in <" + tag + ">";
          return Value::nil();
        }
        pos = end + 2;
      } else if (starts_with("<![CDATA[", 9)) {
        pos += 9;
        size_t end = s.find("]]>", pos);
        if (end == std::string::npos) {
          err = "xml_parse: unclosed CDATA in <" + tag + ">";
          return Value::nil();
        }
        text_accum.append(s.substr(pos, end - pos));
        pos = end + 3;
      } else if (starts_with("</", 2)) {
        pos += 2;
        skip_ws();
        std::string close_tag = parse_name();
        skip_ws();
        if (peek() != '>') {
          err = "xml_parse: expected '>' in closing tag </" + close_tag + ">";
          return Value::nil();
        }
        pos++; // skip '>'
        if (close_tag != tag) {
          err = "xml_parse: mismatched closing tag, expected </" + tag + "> but got </" + close_tag + ">";
          return Value::nil();
        }
        break;
      } else {
        Value child = parse_element(err);
        if (!err.empty()) return Value::nil();
        children_list->items.push_back(child);
      }
    }

    if (pos > s.size()) {
      err = "xml_parse: unclosed tag <" + tag + ">";
      return Value::nil();
    }

    std::string final_text = trim_spaces(text_accum);

    auto elem = std::make_shared<MapObj>();
    elem->put("tag", Value::string(tag));
    elem->put("attrs", Value::object(Type::Map, attrs_map));
    elem->put("children", Value::object(Type::List, children_list));
    elem->put("text", Value::string(final_text));
    return Value::object(Type::Map, elem);
  }
};

Value bi_xml_parse(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "xml_parse")) return Value::nil();
  XmlParser parser(args[0].str());
  std::string err;
  Value v = parser.parse(err);
  if (!err.empty()) {
    error = err;
    return Value::nil();
  }
  return v;
}

Value bi_xml_escape(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "xml_escape")) return Value::nil();
  return Value::string(escape_xml(args[0].str()));
}

Value bi_xml_unescape(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "xml_unescape")) return Value::nil();
  return Value::string(unescape_xml(args[0].str()));
}

}  // namespace

void install_text_builtins(Vm& vm) {
  vm.define_native("format", 1, -1, bi_format);
  vm.define_native("sort_by", 2, 3, bi_sort_by);
  vm.define_native("group_by", 2, 2, bi_group_by);
  vm.define_native("key_by", 2, 2, bi_key_by);
  vm.define_native("sum_by", 3, 3, bi_sum_by);
  vm.define_native("uuid", 0, 0, bi_uuid);
  vm.define_native("xml_parse", 1, 1, bi_xml_parse);
  vm.define_native("xml_escape", 1, 1, bi_xml_escape);
  vm.define_native("xml_unescape", 1, 1, bi_xml_unescape);
}

}  // namespace aios::userland::grain
