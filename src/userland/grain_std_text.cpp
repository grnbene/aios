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
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "aios/userland/grain.hpp"
#include "grain_builtins_internal.hpp"
#include "aios/core/strbuf.hpp"

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
    bool neg = v.type == Type::Decimal ? decimal_compare(v, Value::integer(0)) < 0
             : v.type == Type::Int ? v.i < 0 : std::signbit(d);
    if ((t == 'f' || t == '%' || (!t && sp.precision >= 0 && v.type != Type::Int)) &&
        !(v.type == Type::Real && !std::isfinite(v.d))) {
      // As round() rounds (number_round, half away from zero) - an int and a decimal exactly, a real as the decimal
      // it prints as - so {:.2f} of 2.675 is 2.68 like round(2.675, 2), never the 2.67 of its binary expansion.
      // {:%} is the number times 100, exactly.
      Value x = v;   // a real goes to number_round as it is (it rounds a real from its 15 significant digits)
      if (v.type != Type::Real && !number_exact(v, x, error)) { error = "format(): " + error; return false; }
      if (t == '%' && v.type == Type::Real) {   // times 100 from the real's 15 significant digits, then exactly
        Value r15;
        if (!number_round(v, 30, RoundMode::HalfUp, r15, error)) { error = "format(): " + error; return false; }
        x = r15;
      }
      if (t == '%') {
        Value hundred;
        if (!decimal_arith('*', x, Value::integer(100), hundred, error)) { error = "format(): " + error; return false; }
        x = hundred;
      }
      Value r = x;
      const int prec = sp.precision >= 0 ? sp.precision : (t == 'f' || t == '%' ? 6 : -1);
      if (prec >= 0 && !number_round(x, prec, RoundMode::HalfUp, r, error)) { error = "format(): " + error; return false; }
      body = decimal_text(r);
      neg = decimal_compare(r, Value::integer(0)) < 0;   // what it rounded to: -0.004 at 2 places is 0.00, not -0.00
      if (!body.empty() && body[0] == '-') body.erase(0, 1);
      if (sp.precision >= 0 || t == 'f' || t == '%') {
        const int want = sp.precision >= 0 ? sp.precision : 6;
        const size_t dot = body.find('.');
        const size_t have = dot == std::string::npos ? 0 : body.size() - dot - 1;
        if (have < (size_t)want) {
          if (dot == std::string::npos && want > 0) body += ".";
          body.append((size_t)want - have, '0');
        }
      }
      if (sp.group) {
        const size_t dot = body.find('.');
        body = group_thousands(body.substr(0, dot)) + (dot == std::string::npos ? "" : body.substr(dot));
      }
      if (t == '%') body += "%";
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
    else if (is_exact_number(a) && is_exact_number(b)) r = decimal_compare(a, b);   // decimals: exactly
    else { const double x = a.as_double(), y = b.as_double(); r = x < y ? -1 : x > y ? 1 : 0; }
    return true;
  }
  if (a.type == Type::Str && b.type == Type::Str) { const int c = a.str().compare(b.str()); r = c < 0 ? -1 : c > 0 ? 1 : 0; return true; }
  if (a.type == Type::Bool && b.type == Type::Bool) { r = (a.b == b.b) ? 0 : a.b ? 1 : -1; return true; }
  // a list key orders by its first item, then its second on a tie, ...: sort_by(xs, fn(u) { return [-u.n, u.name] })
  if (a.type == Type::List && b.type == Type::List) {
    const auto& x = a.list_items();
    const auto& y = b.list_items();
    for (size_t k = 0; k < x.size() && k < y.size(); ++k) {
      if (!compare_keys(x[k], y[k], r, error)) return false;
      if (r != 0) return true;
    }
    r = x.size() < y.size() ? -1 : x.size() > y.size() ? 1 : 0;
    return true;
  }
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

// chunk(list, n): the list in pieces of n (the last one shorter).
Value bi_chunk(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_list(args[0], error, "chunk")) return Value::nil();
  if (args[1].type != Type::Int || args[1].i < 1) { error = "chunk() wants a piece size of 1 or more"; return Value::nil(); }
  const std::vector<Value>& items = args[0].list_items();
  const size_t n = (size_t)args[1].i;
  std::vector<Value> out;
  for (size_t at = 0; at < items.size(); at += n)
    out.push_back(Value::list(std::vector<Value>(items.begin() + (long)at, items.begin() + (long)std::min(at + n, items.size()))));
  return Value::list(std::move(out));
}
// partition(list, fn): [the items fn is true for, the rest], each in order.
Value bi_partition(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_list(args[0], error, "partition")) return Value::nil();
  const std::vector<Value> items = args[0].list_items();
  std::vector<Value> yes, no;
  for (const auto& item : items) {
    Value v = vm.call(args[1], {item}, error);
    if (!error.empty()) return Value::nil();
    (v.truthy() ? yes : no).push_back(item);
  }
  return Value::list({Value::list(std::move(yes)), Value::list(std::move(no))});
}
// take_while(list, fn) / drop_while(list, fn): the leading items fn is true for, or everything after them.
static Value take_or_drop_while(Vm& vm, std::vector<Value>& args, std::string& error, bool take) {
  if (!want_list(args[0], error, take ? "take_while" : "drop_while")) return Value::nil();
  const std::vector<Value> items = args[0].list_items();
  size_t n = 0;
  for (; n < items.size(); ++n) {
    Value v = vm.call(args[1], {items[n]}, error);
    if (!error.empty()) return Value::nil();
    if (!v.truthy()) break;
  }
  if (take) return Value::list(std::vector<Value>(items.begin(), items.begin() + (long)n));
  return Value::list(std::vector<Value>(items.begin() + (long)n, items.end()));
}
Value bi_take_while(Vm& vm, std::vector<Value>& args, std::string& error) { return take_or_drop_while(vm, args, error, true); }
Value bi_drop_while(Vm& vm, std::vector<Value>& args, std::string& error) { return take_or_drop_while(vm, args, error, false); }
// sum(list): the items added as sum_by adds them (integers stay integers, decimals exact, anything else a real);
// nil items count nothing; an empty list is 0.
Value bi_sum(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_list(args[0], error, "sum")) return Value::nil();
  Value sum = Value::integer(0);
  for (const auto& v : args[0].list_items()) {
    if (v.type == Type::Nil) continue;
    Value r;   // as + adds: exact, past 64 bits the exact decimal (it wrapped), a real only with a real
    if (!number_arith('+', sum, v, r, error)) { error = "sum(): " + error; return Value::nil(); }
    sum = std::move(r);
  }
  return sum;
}
// min_by(list, fn) / max_by(list, fn): the item whose fn(item) is least / greatest (as sort_by orders keys); the
// first of equals; nil for an empty list.
static Value min_or_max_by(Vm& vm, std::vector<Value>& args, std::string& error, bool want_max) {
  if (!want_list(args[0], error, want_max ? "max_by" : "min_by")) return Value::nil();
  const std::vector<Value> items = args[0].list_items();
  Value best, best_key;
  bool have = false;
  for (const auto& item : items) {
    Value key = vm.call(args[1], {item}, error);
    if (!error.empty()) return Value::nil();
    if (!have) { best = item; best_key = key; have = true; continue; }
    int r = 0;
    if (!compare_keys(key, best_key, r, error)) return Value::nil();
    if (want_max ? r > 0 : r < 0) { best = item; best_key = key; }
  }
  return have ? best : Value::nil();
}
Value bi_min_by(Vm& vm, std::vector<Value>& args, std::string& error) { return min_or_max_by(vm, args, error, false); }
Value bi_max_by(Vm& vm, std::vector<Value>& args, std::string& error) { return min_or_max_by(vm, args, error, true); }
// find_index(list, fn): the index of the first item fn is true for, -1 when none.
Value bi_find_index(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_list(args[0], error, "find_index")) return Value::nil();
  const std::vector<Value> items = args[0].list_items();
  for (size_t i = 0; i < items.size(); ++i) {
    Value v = vm.call(args[1], {items[i]}, error);
    if (!error.empty()) return Value::nil();
    if (v.truthy()) return Value::integer((int64_t)i);
  }
  return Value::integer(-1);
}
// zip_with(a, b, fn): fn(a[i], b[i]) for each pair, as long as the shorter list.
Value bi_zip_with(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!want_list(args[0], error, "zip_with") || !want_list(args[1], error, "zip_with")) return Value::nil();
  const std::vector<Value> a = args[0].list_items(), b = args[1].list_items();
  std::vector<Value> out;
  const size_t n = std::min(a.size(), b.size());
  out.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    Value v = vm.call(args[2], {a[i], b[i]}, error);
    if (!error.empty()) return Value::nil();
    out.push_back(std::move(v));
  }
  return Value::list(std::move(out));
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
    Value sum;   // as + adds
    if (!number_arith('+', *cur, v, sum, error)) { error = "sum_by(): " + error; return Value::nil(); }
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
          // Every character a digit of its base, and a code point UTF-8 may hold (not a surrogate, not past
          // U+10FFFF, not NUL); anything else stays as written.
          uint32_t cp = 0;
          bool ok = false;
          const bool hex = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X');
          const size_t from = hex ? 2 : 1;
          if (ent.size() > from && ent.size() - from <= 8) {
            ok = true;
            for (size_t d = from; d < ent.size() && ok; ++d) {
              const int v = digit_value(ent[d], hex ? 16 : 10);
              if (v < 0) ok = false;
              else cp = cp * (hex ? 16 : 10) + (uint32_t)v;
            }
            if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) ok = false;
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
    skip_ws();
    if (pos < s.size()) {
      err = "xml_parse: content after the root element";
      return Value::nil();
    }
    return root;
  }

 private:
  const std::string& s;
  size_t pos;
  // Each element is a native call: nesting past this is refused rather than overflowing the stack.
  static constexpr int kMaxDepth = 512;
  int depth = 0;

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
        int open = 1;   // the DOCTYPE's own <...> nesting, not the parser's element depth
        while (pos < s.size() && open > 0) {
          if (s[pos] == '<') open++;
          else if (s[pos] == '>') open--;
          pos++;
        }
        if (open > 0) {
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
           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ':' || c == '.' ||
           static_cast<unsigned char>(c) >= 0x80;   // UTF-8: XML names may be any letter
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
    if (++depth > kMaxDepth) {
      err = "xml_parse: elements nested deeper than " + std::to_string(kMaxDepth);
      return Value::nil();
    }
    struct Leave { int& d; ~Leave() { --d; } } leave{depth};

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
    bool closed = false;

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
        closed = true;
        break;
      } else {
        Value child = parse_element(err);
        if (!err.empty()) return Value::nil();
        children_list->items.push_back(child);
      }
    }

    if (!closed) {
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

// ---- markup: html_escape, el, html_raw, html_stringify, xml_stringify ---------------------------------------
//
// A page or a document is built as nodes and written out once, escaped in one place, instead of strings glued
// together with escaping remembered at every value:
//
//   el("td", {"class": "who", "data-v": lower(name)}, el("b", name), el("small", u.userid))
//   html_stringify(node)   -> <td class="who" data-v="ann"><b>Ann &amp; co</b><small>ann</small></td>
//
// A node is a string or number (text, escaped), nil or false (nothing), a list (each in turn), html_raw(markup)
// (markup already written, as is), or an element map {tag, attrs, children} - what el() makes, and what
// xml_parse() hands back (its `text`, when there is one, comes before the children). An attribute that is nil or
// false is left out, true is written bare (HTML) or ="true" (XML), a list is its items joined by spaces
// (class lists). HTML knows its empty elements (<br>, <input>, <meta> ...) and leaves <script> and <style>
// unescaped; XML writes an element with no content as <name/>.

// Appends `in` to `out`, escaped: the runs between the five special characters are copied whole.
void escape_markup_into(std::string& out, const std::string& in, bool html) {
  size_t from = 0;
  for (size_t i = 0; i < in.size(); ++i) {
    const char* rep;
    switch (in[i]) {
      case '&': rep = "&amp;"; break;
      case '<': rep = "&lt;"; break;
      case '>': rep = "&gt;"; break;
      case '"': rep = "&quot;"; break;
      case '\'': rep = html ? "&#39;" : "&apos;"; break;
      default: continue;
    }
    out.append(in, from, i - from);
    out.append(rep);
    from = i + 1;
  }
  out.append(in, from, std::string::npos);
}

std::string escape_html(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 16);
  escape_markup_into(out, in, true);
  return out;
}

bool markup_name_ok(const std::string& n) {
  if (n.empty()) return false;
  const unsigned char c0 = static_cast<unsigned char>(n[0]);
  if (!(std::isalpha(c0) || c0 == '_')) return false;
  for (unsigned char c : n)
    if (!(std::isalnum(c) || c == '-' || c == '_' || c == ':' || c == '.')) return false;
  return true;
}

bool html_void(const std::string& t) {
  static const char* const kVoid[] = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta",
                                      "source", "track", "wbr"};
  for (const char* v : kVoid)
    if (t == v) return true;
  return false;
}

struct MarkupWriter {
  bool html;
  std::string out;
  std::string error;
  const MarkupHooks* hooks = nullptr;

  void esc_into(const std::string& s) { escape_markup_into(out, s, html); }

  bool attr_text(const Value& v, std::string& text) {
    if (v.type == Type::Str) { text = v.str(); return true; }
    if (v.is_number() || v.type == Type::Decimal) { text = v.to_text(); return true; }
    if (v.type == Type::List) {
      text.clear();
      for (const Value& item : v.list_items()) {
        if (item.type == Type::Nil || (item.type == Type::Bool && !item.b)) continue;
        std::string one;
        if (item.type == Type::List || !attr_text(item, one)) {
          error = "an attribute list holds strings and numbers, not a " + std::string(type_name(item.type));
          return false;
        }
        if (one.empty()) continue;
        if (!text.empty()) text.push_back(' ');
        text += one;
      }
      return true;
    }
    error = std::string("an attribute is a string, number, list, true, false or nil, not a ") + type_name(v.type);
    return false;
  }

  bool write(const Value& v, int depth, bool raw_text) {
    if (depth > 512) { error = "nested more than 512 deep (a node that holds itself?)"; return false; }
    switch (v.type) {
      case Type::Nil: return true;
      case Type::Bool:
        if (!v.b) return true;
        error = "true is not a node (a condition that wanted ? node : nil?)";
        return false;
      case Type::Str: if (raw_text) out += v.str(); else esc_into(v.str()); return true;
      case Type::Int: case Type::Real: case Type::Decimal: out += v.to_text(); return true;
      case Type::List:
        for (const Value& c : v.list_items())
          if (!write(c, depth + 1, raw_text)) return false;
        return true;
      case Type::Map: break;
      default:
        error = std::string("a ") + type_name(v.type) + " is not a node";
        return false;
    }
    MapObj* m = static_cast<MapObj*>(v.obj.get());
    if (m->find("widget")) {
      if (hooks && hooks->widget) return hooks->widget(v, out, error);
      error = "a ui.* widget is shown by a ui.pane view, not written as markup";
      return false;
    }
    if (Value* r = m->find("raw"); r && !m->find("tag")) {
      if (r->type != Type::Str) { error = "html_raw() holds a string"; return false; }
      out += r->str();
      return true;
    }
    Value* tag = m->find("tag");
    if (!tag || tag->type != Type::Str) { error = "a map node is an element {tag, attrs, children} or html_raw(markup)"; return false; }
    const std::string& name = tag->str();
    if (!markup_name_ok(name)) { error = "\"" + name + "\" is not an element name"; return false; }
    out += '<';
    out += name;
    if (Value* a = m->find("attrs"); a && a->type != Type::Nil) {
      if (a->type != Type::Map) { error = "<" + name + ">'s attrs is a map"; return false; }
      for (auto& [k, val] : static_cast<MapObj*>(a->obj.get())->entries) {
        if (!markup_name_ok(k)) { error = "\"" + k + "\" is not an attribute name"; return false; }
        if (hooks && hooks->attr) {
          const int took = hooks->attr(v, name, k, val, out, error);
          if (took < 0) { error = "<" + name + " " + k + ">: " + error; return false; }
          if (took > 0) continue;
        }
        if (val.type == Type::Fn || val.type == Type::Native) {
          error = "<" + name + " " + k + ">: a function is an action, which only a ui.pane view can run";
          return false;
        }
        if (val.type == Type::Nil || (val.type == Type::Bool && !val.b)) continue;
        out += ' ';
        out += k;
        if (val.type == Type::Bool) {
          if (!html) out += "=\"true\"";
          continue;
        }
        if (val.type == Type::Str) {   // the usual case: no copy
          out += "=\"";
          esc_into(val.str());
          out += '"';
          continue;
        }
        std::string text;
        if (!attr_text(val, text)) { error = "<" + name + " " + k + ">: " + error; return false; }
        out += "=\"";
        esc_into(text);
        out += '"';
      }
    }
    Value* text = m->find("text");
    Value* kids = m->find("children");
    const bool has_text = text && text->type == Type::Str && !text->str().empty();
    const bool has_kids = kids && kids->type != Type::Nil && !(kids->type == Type::List && kids->list_items().empty());
    if (html && html_void(name)) {
      if (has_text || has_kids) { error = "<" + name + "> holds nothing"; return false; }
      out += '>';
      return true;
    }
    if (!html && !has_text && !has_kids) { out += "/>"; return true; }
    out += '>';
    const bool raw_inside = html && (name == "script" || name == "style");
    const size_t body = out.size();
    if (has_text) { if (raw_inside) out += text->str(); else esc_into(text->str()); }
    if (has_kids && !write(*kids, depth + 1, raw_inside)) return false;
    if (raw_inside) {
      // the one thing that ends a script early: written as <\/script, which the script reads the same
      const std::string close = "</" + name;
      for (size_t at = out.find(close, body); at != std::string::npos; at = out.find(close, at + 2)) out.insert(at + 1, "\\");
    }
    out += "</";
    out += name;
    out += '>';
    return true;
  }
};

Value markup_stringify(std::vector<Value>& args, std::string& error, bool html) {
  MarkupWriter w{html, {}, {}};
  if (!w.write(args[0], 0, false)) {
    error = std::string(html ? "html_stringify" : "xml_stringify") + "(): " + w.error;
    return Value::nil();
  }
  return Value::string(std::move(w.out));
}

Value bi_html_stringify(Vm&, std::vector<Value>& args, std::string& error) { return markup_stringify(args, error, true); }
Value bi_xml_stringify(Vm&, std::vector<Value>& args, std::string& error) { return markup_stringify(args, error, false); }

// html_escape(v): v as text, safe inside an element or a quoted attribute; nil is "".
Value bi_html_escape(Vm&, std::vector<Value>& args, std::string&) {
  if (args[0].type == Type::Nil) return Value::string("");
  return Value::string(escape_html(args[0].type == Type::Str ? args[0].str() : args[0].to_text()));
}

// html_raw(markup): markup already written, put in a node tree as it is.
Value bi_html_raw(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "html_raw")) return Value::nil();
  auto m = std::make_shared<MapObj>();
  m->put("raw", args[0]);
  return Value::object(Type::Map, m);
}

// el(tag, [attrs], children...): an element node. The second argument is the attrs when it is nil or a map that
// is not itself a node (no tag, no raw); otherwise it is the first child.
Value bi_el(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "el")) return Value::nil();
  if (!markup_name_ok(args[0].str())) { error = "el(): \"" + args[0].str() + "\" is not an element name"; return Value::nil(); }
  size_t first_child = 1;
  Value attrs = Value::nil();
  if (args.size() > 1) {
    const Value& a = args[1];
    if (a.type == Type::Nil) {
      first_child = 2;
    } else if (a.type == Type::Map) {
      MapObj* am = static_cast<MapObj*>(a.obj.get());
      if (!am->find("tag") && !am->find("raw")) { attrs = a; first_child = 2; }
    }
  }
  std::vector<Value> kids(std::make_move_iterator(args.begin() + static_cast<std::ptrdiff_t>(std::min(first_child, args.size()))),
                          std::make_move_iterator(args.end()));
  auto m = std::make_shared<MapObj>();
  m->entries.reserve(3);   // one allocation for the three fields, not three as the vector grows
  m->put("tag", std::move(args[0]));
  m->put("attrs", attrs.type == Type::Nil ? Value::object(Type::Map, std::make_shared<MapObj>()) : std::move(attrs));
  m->put("children", Value::list(std::move(kids)));
  return Value::object(Type::Map, m);
}

}  // namespace

bool db_connect_args(std::vector<Value>& args, std::string& error) {
  if (args.size() != 1 || args[0].type != Type::Map) return true;   // the five arguments: as they are
  MapObj* m = static_cast<MapObj*>(args[0].obj.get());
  auto field = [&](const char* k) { Value* v = m->find(k); return v ? *v : Value::nil(); };
  Value host = field("host"), port = field("port"), user = field("user"), pass = field("password"), db = field("db");
  if (db.type == Type::Nil) db = field("database");
  if (host.type != Type::Str || host.str().empty()) { error = "db_connect(): the map has no \"host\""; return false; }
  if (port.type == Type::Str) port = Value::integer(std::strtoll(port.str().c_str(), nullptr, 10));
  if (port.type == Type::Nil) port = Value::integer(3306);
  if (!port.is_number()) { error = "db_connect(): \"port\" is a number"; return false; }
  args = {host, port, user.type == Type::Nil ? Value::string("") : user, pass.type == Type::Nil ? Value::string("") : pass, db};
  return true;
}

bool is_markup_node(const Value& v) {
  if (v.type != Type::Map) return false;
  MapObj* m = static_cast<MapObj*>(v.obj.get());
  if (m->find("widget")) return false;
  Value* tag = m->find("tag");
  if (tag) return tag->type == Type::Str;
  Value* raw = m->find("raw");
  return raw && raw->type == Type::Str;
}

bool markup_write(const Value& node, bool html, std::string& out, std::string& err, const MarkupHooks* hooks) {
  MarkupWriter w{html, {}, {}, hooks};
  if (!w.write(node, 0, false)) { err = w.error; return false; }
  out += w.out;
  return true;
}

std::string markup_text(const Value& node) {
  std::string out;
  std::function<void(const Value&, int)> walk = [&](const Value& v, int depth) {
    if (depth > 512) return;
    if (v.type == Type::Str) { out += v.str(); return; }
    if (v.is_number() || v.type == Type::Decimal) { out += v.to_text(); return; }
    if (v.type == Type::List) { for (const Value& c : v.list_items()) walk(c, depth + 1); return; }
    if (v.type != Type::Map) return;
    MapObj* m = static_cast<MapObj*>(v.obj.get());
    if (Value* t = m->find("text"); t && t->type == Type::Str) out += t->str();
    if (Value* k = m->find("children")) walk(*k, depth + 1);
  };
  walk(node, 0);
  return out;
}

void install_text_builtins(Vm& vm) {
  vm.define_native("format", 1, -1, bi_format);
  vm.define_native("sort_by", 2, 3, bi_sort_by);
  vm.define_native("group_by", 2, 2, bi_group_by);
  vm.define_native("key_by", 2, 2, bi_key_by);
  vm.define_native("sum_by", 3, 3, bi_sum_by);
  vm.define_native("chunk", 2, 2, bi_chunk);
  vm.define_native("partition", 2, 2, bi_partition);
  vm.define_native("take_while", 2, 2, bi_take_while);
  vm.define_native("drop_while", 2, 2, bi_drop_while);
  vm.define_native("sum", 1, 1, bi_sum);
  vm.define_native("min_by", 2, 2, bi_min_by);
  vm.define_native("max_by", 2, 2, bi_max_by);
  vm.define_native("find_index", 2, 2, bi_find_index);
  vm.define_native("zip_with", 3, 3, bi_zip_with);
  vm.define_native("uuid", 0, 0, bi_uuid);
  vm.define_native("xml_parse", 1, 1, bi_xml_parse);
  vm.define_native("xml_escape", 1, 1, bi_xml_escape);
  vm.define_native("xml_unescape", 1, 1, bi_xml_unescape);
  vm.define_native("xml_stringify", 1, 1, bi_xml_stringify);
  vm.define_native("html_stringify", 1, 1, bi_html_stringify);
  vm.define_native("html_escape", 1, 1, bi_html_escape);
  vm.define_native("html_raw", 1, 1, bi_html_raw);
  vm.define_native("el", 1, -1, bi_el);
}

}  // namespace aios::userland::grain
