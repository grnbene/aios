// console, queueMicrotask, atob/btoa and the value inspector the console prints with.
#include <algorithm>
#include "aios/hal/host.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "builtins.hpp"
#include "internal_num.hpp"

namespace aios::js {

namespace {

bool is_identifier(const std::u16string& s) {
  if (s.empty()) return false;
  for (size_t i = 0; i < s.size(); ++i) {
    char16_t c = s[i];
    const bool ok = (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || c == u'_' || c == u'$' || (i > 0 && c >= u'0' && c <= u'9');
    if (!ok) return false;
  }
  return true;
}

std::string quote_string(const std::u16string& s, char q) {
  std::u16string out;
  out.push_back(static_cast<char16_t>(q));
  for (char16_t c : s) {
    if (c == static_cast<char16_t>(q)) { out.push_back(u'\\'); out.push_back(c); }
    else if (c == u'\\') out += u"\\\\";
    else if (c == u'\n') out += u"\\n";
    else if (c == u'\r') out += u"\\r";
    else if (c == u'\t') out += u"\\t";
    else out.push_back(c);
  }
  out.push_back(static_cast<char16_t>(q));
  JsString tmp;
  tmp.flat = out;
  tmp.length = static_cast<uint32_t>(out.size());
  return tmp.utf8();
}

std::string function_label(Context& ctx, Object* o) {
  Value n;
  std::string name;
  if (o->get(ctx, ctx.atoms.name, Value::Obj(o), &n) && n.is_string()) name = n.as_string()->utf8();
  else ctx.clear_exception();
  bool is_class = o->cls == Class::Function && static_cast<Function*>(o)->is_class;
  if (is_class) return "[class " + (name.empty() ? std::string("(anonymous)") : name) + "]";
  return "[Function" + (name.empty() ? std::string(" (anonymous)") : ": " + name) + "]";
}

std::string constructor_name(Context& ctx, Object* o) {
  Object* p = o;
  for (int guard = 0; p && guard < 100; ++guard) {
    PropDesc d;
    bool found;
    if (!p->get_own_property(ctx, ctx.atoms.constructor, &d, &found)) { ctx.clear_exception(); return ""; }
    if (found && d.has_value && d.value.is_object()) {
      Value n;
      if (d.value.as_object()->get(ctx, ctx.atoms.name, d.value, &n) && n.is_string() && n.as_string()->length) return n.as_string()->utf8();
      ctx.clear_exception();
      return "";
    }
    Object* next;
    if (!p->get_prototype_of(ctx, &next)) { ctx.clear_exception(); return ""; }
    p = next;
  }
  return "";
}

std::string inspect(Context& ctx, const Value& v, int depth, std::vector<Object*>& seen, bool top);

std::string inspect_key(Context& ctx, PropKey k) {
  if (k->kind == CellKind::Symbol) return "[" + key_to_display(k) + "]";
  JsString* s = static_cast<JsString*>(k);
  if (is_identifier(s->str())) return s->utf8();
  return quote_string(s->str(), '\'');
}

std::string inspect(Context& ctx, const Value& v, int depth, std::vector<Object*>& seen, bool top) {
  switch (v.tag) {
    case Tag::Undefined: return "undefined";
    case Tag::Null: return "null";
    case Tag::Boolean: return v.boolean ? "true" : "false";
    case Tag::Number: return v.num == 0 && std::signbit(v.num) ? "-0" : number_to_ascii(v.num);
    case Tag::String: return top ? v.as_string()->utf8() : quote_string(v.as_string()->str(), '\'');
    case Tag::Symbol: return key_to_display(v.as_symbol());
    case Tag::BigInt: return big::to_string(v.as_bigint()->n) + "n";
    case Tag::Object: break;
    default: return "<empty>";
  }
  Object* o = v.as_object();
  if (std::find(seen.begin(), seen.end(), o) != seen.end()) return "[Circular]";
  if (o->callable) {
    std::string label = function_label(ctx, o);
    return label;
  }
  if (o->cls == Class::Error) {
    Value st;
    if (o->get(ctx, ctx.atoms.stack, v, &st) && st.is_string() && st.as_string()->length) return st.as_string()->utf8();
    ctx.clear_exception();
    return ctx.describe_exception(v);
  }
  if (o->cls == Class::Symbol || o->cls == Class::Number || o->cls == Class::String || o->cls == Class::Boolean || o->cls == Class::BigInt) {
    PrimitiveObject* p = static_cast<PrimitiveObject*>(o);
    const char* kind = o->cls == Class::Symbol ? "Symbol" : o->cls == Class::Number ? "Number" : o->cls == Class::String ? "String" : o->cls == Class::BigInt ? "BigInt" : "Boolean";
    return std::string("[") + kind + ": " + inspect(ctx, p->prim, 0, seen, false) + "]";
  }
  if (depth < 0) {
    bool is_arr;
    ctx.is_array(v, &is_arr);
    return is_arr ? "[Array]" : "[" + (constructor_name(ctx, o).empty() ? std::string("Object") : constructor_name(ctx, o)) + "]";
  }
  seen.push_back(o);
  std::vector<std::string> parts;
  bool is_arr = false;
  ctx.is_array(v, &is_arr);
  std::string prefix;
  std::string cname = constructor_name(ctx, o);
  if (o->cls == Class::Map || o->cls == Class::Set) {
    // keys are listed through the collection's own iteration protocol
    Value entries;
    Value iter_fn;
    std::vector<Value> items;
    if (ctx.get(v, ctx.atoms.entries, &iter_fn) && iter_fn.is_callable() && ctx.call(iter_fn, v, nullptr, 0, &entries) && ctx.iterable_to_list(entries, &items)) {
      for (const Value& e : items) {
        if (o->cls == Class::Map && e.is_object()) {
          Value k, val;
          get_idx(ctx, e.as_object(), 0, &k);
          get_idx(ctx, e.as_object(), 1, &val);
          parts.push_back(inspect(ctx, k, depth - 1, seen, false) + " => " + inspect(ctx, val, depth - 1, seen, false));
        } else if (e.is_object()) {
          Value k;
          get_idx(ctx, e.as_object(), 0, &k);
          parts.push_back(inspect(ctx, k, depth - 1, seen, false));
        }
      }
    }
    ctx.clear_exception();
    uint64_t size = items.size();
    prefix = (o->cls == Class::Map ? "Map(" : "Set(") + std::to_string(size) + ") ";
  } else if (o->cls == Class::Date || o->cls == Class::RegExp) {
    JsString* s = nullptr;
    Value ts;
    if (ctx.get(v, ctx.atoms.toString, &ts) && ts.is_callable()) {
      Value r;
      if (ctx.call(ts, v, nullptr, 0, &r) && r.is_string()) s = r.as_string();
    }
    ctx.clear_exception();
    seen.pop_back();
    return s ? s->utf8() : "[object]";
  } else if (o->cls == Class::Promise) {
    seen.pop_back();
    return "Promise { <state> }";
  }
  std::vector<PropKey> keys;
  o->own_keys(ctx, &keys);
  ctx.clear_exception();
  uint64_t len = 0;
  if (is_arr) {
    ctx.length_of(o, &len);
    ctx.clear_exception();
    uint64_t holes = 0;
    auto flush_holes = [&]() {
      if (holes) { parts.push_back("<" + std::to_string(holes) + " empty item" + (holes > 1 ? "s" : "") + ">"); holes = 0; }
    };
    for (uint64_t i = 0; i < len && parts.size() < 100; ++i) {
      bool has = false;
      has_idx(ctx, o, i, &has);
      if (!has) { ++holes; continue; }
      flush_holes();
      Value e;
      get_idx(ctx, o, i, &e);
      parts.push_back(inspect(ctx, e, depth - 1, seen, false));
    }
    flush_holes();
    ctx.clear_exception();
    if (len > 100) parts.push_back("... " + std::to_string(len - 100) + " more items");
  }
  for (PropKey k : keys) {
    if (is_arr && k->kind == CellKind::String && static_cast<JsString*>(k)->array_index() >= 0) continue;
    if (is_arr && k == ctx.atoms.length) continue;
    PropDesc d;
    bool found = false;
    if (!o->get_own_property(ctx, k, &d, &found) || !found || !d.enumerable) { ctx.clear_exception(); continue; }
    std::string val;
    if (d.is_accessor()) val = d.getter && d.setter ? "[Getter/Setter]" : d.getter ? "[Getter]" : "[Setter]";
    else val = inspect(ctx, d.value, depth - 1, seen, false);
    parts.push_back(inspect_key(ctx, k) + ": " + val);
  }
  seen.pop_back();
  if (is_arr) {
    if (parts.empty()) return "[]";
    std::string joined;
    size_t total = 0;
    for (auto& p : parts) total += p.size() + 2;
    if (total <= 72) {
      for (size_t i = 0; i < parts.size(); ++i) joined += (i ? ", " : "") + parts[i];
      return "[ " + joined + " ]";
    }
    for (size_t i = 0; i < parts.size(); ++i) joined += std::string(i ? ",\n  " : "\n  ") + parts[i];
    return "[" + joined + "\n]";
  }
  if (cname.empty() && o->proto == nullptr) prefix = "[Object: null prototype] ";
  else if (prefix.empty() && !cname.empty() && cname != "Object") prefix = cname + " ";
  if (parts.empty()) return prefix + "{}";
  size_t total = 0;
  for (auto& p : parts) total += p.size() + 2;
  std::string joined;
  if (total <= 72) {
    for (size_t i = 0; i < parts.size(); ++i) joined += (i ? ", " : "") + parts[i];
    return prefix + "{ " + joined + " }";
  }
  for (size_t i = 0; i < parts.size(); ++i) joined += std::string(i ? ",\n  " : "\n  ") + parts[i];
  return prefix + "{" + joined + "\n}";
}

// console.log's format-string handling: %s %d %i %f %j %o %O %%
std::string format_args(Context& ctx, CallArgs& a, int first = 0) {
  std::string out;
  int i = first;
  std::vector<Object*> seen;
  if (a.argc > first && a.argv[first].is_string()) {
    std::string f = a.argv[first].as_string()->utf8();
    ++i;
    for (size_t k = 0; k < f.size(); ++k) {
      if (f[k] == '%' && k + 1 < f.size()) {
        char c = f[k + 1];
        if (c == '%') { out += '%'; ++k; continue; }
        if ((c == 's' || c == 'd' || c == 'i' || c == 'f' || c == 'j' || c == 'o' || c == 'O' || c == 'c') && i < a.argc) {
          const Value& arg = a.argv[i++];
          ++k;
          if (c == 'c') continue;   // CSS styling for a browser console: consumes its argument and prints nothing
          if (c == 's') out += arg.is_string() ? arg.as_string()->utf8() : inspect(ctx, arg, 0, seen, true);
          else if (c == 'd' || c == 'i' || c == 'f') {
            double d;
            if (arg.is_object() || !ctx.to_number(arg, &d)) { ctx.clear_exception(); d = NAN; }
            if (c == 'i') d = std::trunc(d);
            out += number_to_ascii(d);
          } else if (c == 'j') {
            Value json, stringify, r;
            if (ctx.get_named(Value::Obj(ctx.intr.global), "JSON", &json) && ctx.get_v(json, ctx.atom_ascii("stringify"), &stringify) && ctx.call(stringify, json, {arg}, &r) && r.is_string())
              out += r.as_string()->utf8();
            else { ctx.clear_exception(); out += "undefined"; }
          } else out += inspect(ctx, arg, 4, seen, false);
          continue;
        }
      }
      out += f[k];
    }
  }
  for (; i < a.argc; ++i) {
    if (!out.empty() || i > first) out += ' ';
    out += inspect(ctx, a.argv[i], 2, seen, true);
  }
  return out;
}

int g_group_indent = 0;

void emit(Context& ctx, const std::string& text) {
  // AIOS_JS_ERRORS=1 mirrors the page's console to stderr: a framework that catches its own exception shows only its console.error.
  static const bool mirror = !aios::hal::process_host().env("AIOS_JS_ERRORS").empty();
  if (mirror) aios::hal::host_note("[console] " + text + "\n");
  if (!ctx.print) return;
  std::string indent(static_cast<size_t>(g_group_indent) * 2, ' ');
  std::string out;
  size_t start = 0;
  for (;;) {
    size_t nl = text.find('\n', start);
    out += indent + text.substr(start, nl == std::string::npos ? std::string::npos : nl - start) + "\n";
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
  ctx.print(out);
}

Value console_log(Context& ctx, CallArgs& a) {
  emit(ctx, format_args(ctx, a));
  return Value::Undefined();
}

Value console_assert(Context& ctx, CallArgs& a) {
  if (Context::to_boolean(a[0])) return Value::Undefined();
  std::string rest = format_args(ctx, a, 1);
  emit(ctx, "Assertion failed" + (rest.empty() ? std::string() : ": " + rest));
  return Value::Undefined();
}

Value console_group(Context& ctx, CallArgs& a) {
  if (a.argc) emit(ctx, format_args(ctx, a));
  ++g_group_indent;
  return Value::Undefined();
}

Value console_group_end(Context&, CallArgs&) {
  if (g_group_indent > 0) --g_group_indent;
  return Value::Undefined();
}

Value console_noop(Context&, CallArgs&) { return Value::Undefined(); }

Value console_trace(Context& ctx, CallArgs& a) {
  emit(ctx, "Trace: " + format_args(ctx, a) + "\n" + vm_stack_trace(ctx, 0));
  return Value::Undefined();
}

Value queue_microtask(Context& ctx, CallArgs& a) {
  if (!a[0].is_callable()) {
    ctx.throw_type_error("The callback provided as parameter 1 is not a function");
    return Value::Exception();
  }
  ctx.enqueue_job_value(a[0], {});
  return Value::Undefined();
}

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

Value global_btoa(Context& ctx, CallArgs& a) {
  JsString* s;
  JS_TRY(ctx.to_string(a[0], &s));
  std::string bytes;
  for (char16_t c : s->str()) {
    if (c > 255) {
      ctx.throw_error(ErrorKind::Error, "Invalid character");
      return Value::Exception();
    }
    bytes.push_back(static_cast<char>(c));
  }
  std::string out;
  for (size_t i = 0; i < bytes.size(); i += 3) {
    uint32_t n = static_cast<uint8_t>(bytes[i]) << 16;
    if (i + 1 < bytes.size()) n |= static_cast<uint8_t>(bytes[i + 1]) << 8;
    if (i + 2 < bytes.size()) n |= static_cast<uint8_t>(bytes[i + 2]);
    out += kB64[(n >> 18) & 63];
    out += kB64[(n >> 12) & 63];
    out += i + 1 < bytes.size() ? kB64[(n >> 6) & 63] : '=';
    out += i + 2 < bytes.size() ? kB64[n & 63] : '=';
  }
  return Str(ctx, out);
}

Value global_atob(Context& ctx, CallArgs& a) {
  JsString* s;
  JS_TRY(ctx.to_string(a[0], &s));
  std::u16string in;
  for (char16_t c : s->str())
    if (c != u' ' && c != u'\t' && c != u'\n' && c != u'\f' && c != u'\r') in.push_back(c);
  if (in.size() % 4 == 0 && !in.empty()) {
    if (in.back() == u'=') in.pop_back();
    if (!in.empty() && in.back() == u'=') in.pop_back();
  }
  if (in.size() % 4 == 1) {
    ctx.throw_error(ErrorKind::Error, "The string to be decoded is not correctly encoded.");
    return Value::Exception();
  }
  std::u16string out;
  uint32_t acc = 0;
  int bits = 0;
  for (char16_t c : in) {
    const char* p = c < 128 ? std::char_traits<char>::find(kB64, 64, static_cast<char>(c)) : nullptr;
    if (!p) {
      ctx.throw_error(ErrorKind::Error, "The string to be decoded is not correctly encoded.");
      return Value::Exception();
    }
    acc = (acc << 6) | static_cast<uint32_t>(p - kB64);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char16_t>((acc >> bits) & 0xFF));
    }
  }
  return StrV(ctx, std::move(out));
}

}  // namespace

std::string Context::inspect(const Value& v) {
  std::vector<Object*> seen;
  return aios::js::inspect(*this, v, 2, seen, false);
}

void setup_misc(Context& ctx) {
  Object* g = ctx.intr.global;
  Object* console = ctx.new_object();
  ctx.define_data(g, ctx.atom_ascii("console"), Value::Obj(console), kHiddenAttrs);
  for (const char* n : {"log", "info", "warn", "error", "debug", "dir"}) ctx.define_method(console, n, console_log, 0, kDefaultAttrs);
  ctx.define_method(console, "assert", console_assert, 0, kDefaultAttrs);
  ctx.define_method(console, "group", console_group, 0, kDefaultAttrs);
  ctx.define_method(console, "groupCollapsed", console_group, 0, kDefaultAttrs);
  ctx.define_method(console, "groupEnd", console_group_end, 0, kDefaultAttrs);
  ctx.define_method(console, "trace", console_trace, 0, kDefaultAttrs);
  for (const char* n : {"time", "timeEnd", "timeLog", "count", "countReset", "table", "clear", "profile", "profileEnd", "timeStamp"}) ctx.define_method(console, n, console_noop, 0, kDefaultAttrs);
  ctx.define_method(g, "queueMicrotask", queue_microtask, 1);
  ctx.define_method(g, "btoa", global_btoa, 1);
  ctx.define_method(g, "atob", global_atob, 1);
}

}  // namespace aios::js
