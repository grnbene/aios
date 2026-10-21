// JSON.parse and JSON.stringify.
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "builtins.hpp"
#include "internal_num.hpp"

namespace aios::js {

namespace {

struct JsonParser {
  Context& ctx;
  const std::u16string& s;
  size_t pos = 0;
  int depth = 0;

  JsonParser(Context& c, const std::u16string& src) : ctx(c), s(src) {}

  Value fail_at(size_t at) {
    std::string tok;
    if (at >= s.size()) {
      ctx.throw_syntax_error("Unexpected end of JSON input");
      return Value::Exception();
    }
    JsString* one = ctx.new_string(s.substr(at, 1));
    tok = one->utf8();
    ctx.throw_syntax_error("Unexpected token " + tok + " in JSON at position " + std::to_string(at));
    return Value::Exception();
  }

  void skip_ws() {
    while (pos < s.size() && (s[pos] == u' ' || s[pos] == u'\t' || s[pos] == u'\n' || s[pos] == u'\r')) ++pos;
  }

  Value parse_value() {
    skip_ws();
    if (pos >= s.size()) return fail_at(pos);
    char16_t c = s[pos];
    switch (c) {
      case u'{': return parse_object();
      case u'[': return parse_array();
      case u'"': {
        std::u16string str;
        if (!parse_string(&str)) return Value::Exception();
        return Value::String(ctx.new_string(std::move(str)));
      }
      case u't':
        if (s.compare(pos, 4, u"true") == 0) { pos += 4; return Value::Bool(true); }
        return fail_at(pos);
      case u'f':
        if (s.compare(pos, 5, u"false") == 0) { pos += 5; return Value::Bool(false); }
        return fail_at(pos);
      case u'n':
        if (s.compare(pos, 4, u"null") == 0) { pos += 4; return Value::Null(); }
        return fail_at(pos);
      default:
        if (c == u'-' || (c >= u'0' && c <= u'9')) return parse_number();
        return fail_at(pos);
    }
  }

  Value parse_number() {
    size_t start = pos;
    if (s[pos] == u'-') ++pos;
    if (pos >= s.size()) return fail_at(pos);
    if (s[pos] == u'0') {
      ++pos;
    } else if (s[pos] >= u'1' && s[pos] <= u'9') {
      while (pos < s.size() && s[pos] >= u'0' && s[pos] <= u'9') ++pos;
    } else {
      return fail_at(pos);
    }
    if (pos < s.size() && s[pos] == u'.') {
      ++pos;
      if (pos >= s.size() || s[pos] < u'0' || s[pos] > u'9') return fail_at(pos);
      while (pos < s.size() && s[pos] >= u'0' && s[pos] <= u'9') ++pos;
    }
    if (pos < s.size() && (s[pos] == u'e' || s[pos] == u'E')) {
      ++pos;
      if (pos < s.size() && (s[pos] == u'+' || s[pos] == u'-')) ++pos;
      if (pos >= s.size() || s[pos] < u'0' || s[pos] > u'9') return fail_at(pos);
      while (pos < s.size() && s[pos] >= u'0' && s[pos] <= u'9') ++pos;
    }
    std::string t;
    for (size_t i = start; i < pos; ++i) t.push_back(static_cast<char>(s[i]));
    double d;
    if (!parse_decimal_ascii(t, &d)) d = std::strtod(t.c_str(), nullptr);
    return Value::Number(d);
  }

  bool parse_string(std::u16string* out) {
    ++pos;   // opening quote
    for (;;) {
      if (pos >= s.size()) {
        ctx.throw_syntax_error("Unterminated string in JSON at position " + std::to_string(pos));
        return false;
      }
      char16_t c = s[pos];
      if (c == u'"') { ++pos; return true; }
      if (c < 0x20) {
        fail_at(pos);
        return false;
      }
      if (c != u'\\') { out->push_back(c); ++pos; continue; }
      ++pos;
      if (pos >= s.size()) { fail_at(pos); return false; }
      char16_t e = s[pos++];
      switch (e) {
        case u'"': out->push_back(u'"'); break;
        case u'\\': out->push_back(u'\\'); break;
        case u'/': out->push_back(u'/'); break;
        case u'b': out->push_back(u'\b'); break;
        case u'f': out->push_back(u'\f'); break;
        case u'n': out->push_back(u'\n'); break;
        case u'r': out->push_back(u'\r'); break;
        case u't': out->push_back(u'\t'); break;
        case u'u': {
          unsigned v = 0;
          for (int i = 0; i < 4; ++i) {
            if (pos >= s.size()) { fail_at(pos); return false; }
            char16_t h = s[pos];
            int d = h >= u'0' && h <= u'9' ? h - u'0' : h >= u'a' && h <= u'f' ? h - u'a' + 10 : h >= u'A' && h <= u'F' ? h - u'A' + 10 : -1;
            if (d < 0) { fail_at(pos); return false; }
            v = v * 16 + static_cast<unsigned>(d);
            ++pos;
          }
          out->push_back(static_cast<char16_t>(v));
          break;
        }
        default:
          fail_at(pos - 1);
          return false;
      }
    }
  }

  Value parse_array() {
    if (++depth > 4000) { ctx.throw_range_error("Maximum call stack size exceeded"); return Value::Exception(); }
    ++pos;
    std::vector<Value> items;
    skip_ws();
    if (pos < s.size() && s[pos] == u']') {
      ++pos;
      --depth;
      return Value::Obj(ctx.new_array_from(items));
    }
    for (;;) {
      Value v = parse_value();
      if (v.is_exception()) return v;
      items.push_back(v);
      skip_ws();
      if (pos >= s.size()) return fail_at(pos);
      if (s[pos] == u',') { ++pos; continue; }
      if (s[pos] == u']') { ++pos; break; }
      return fail_at(pos);
    }
    --depth;
    return Value::Obj(ctx.new_array_from(items));
  }

  Value parse_object() {
    if (++depth > 4000) { ctx.throw_range_error("Maximum call stack size exceeded"); return Value::Exception(); }
    ++pos;
    Object* o = ctx.new_object();
    skip_ws();
    if (pos < s.size() && s[pos] == u'}') {
      ++pos;
      --depth;
      return Value::Obj(o);
    }
    for (;;) {
      skip_ws();
      if (pos >= s.size() || s[pos] != u'"') return fail_at(pos);
      std::u16string key;
      if (!parse_string(&key)) return Value::Exception();
      skip_ws();
      if (pos >= s.size() || s[pos] != u':') return fail_at(pos);
      ++pos;
      Value v = parse_value();
      if (v.is_exception()) return v;
      PropKey k = ctx.atom(key);
      if (!ctx.create_data_property(o, k, v)) return Value::Exception();
      skip_ws();
      if (pos >= s.size()) return fail_at(pos);
      if (s[pos] == u',') { ++pos; continue; }
      if (s[pos] == u'}') { ++pos; break; }
      return fail_at(pos);
    }
    --depth;
    return Value::Obj(o);
  }
};

bool internalize(Context& ctx, Object* holder, PropKey name, const Value& reviver, Value* out) {
  Value val;
  if (!holder->get(ctx, name, Value::Obj(holder), &val)) return false;
  if (val.is_object()) {
    Object* o = val.as_object();
    bool is_arr;
    if (!ctx.is_array(val, &is_arr)) return false;
    if (is_arr) {
      uint64_t len;
      if (!ctx.length_of(o, &len)) return false;
      for (uint64_t i = 0; i < len; ++i) {
        PropKey k;
        if (!ctx.property_key_from_index(i, &k)) return false;
        Value nv;
        if (!internalize(ctx, o, k, reviver, &nv)) return false;
        bool ok;
        if (nv.is_undefined()) { if (!o->delete_property(ctx, k, &ok)) return false; }
        else if (!ctx.create_data_property(o, k, nv)) return false;
      }
    } else {
      std::vector<PropKey> keys;
      if (!o->own_keys(ctx, &keys)) return false;
      for (PropKey k : keys) {
        if (k->kind != CellKind::String) continue;
        PropDesc d;
        bool found;
        if (!o->get_own_property(ctx, k, &d, &found)) return false;
        if (!found || !d.enumerable) continue;
        Value nv;
        if (!internalize(ctx, o, k, reviver, &nv)) return false;
        bool ok;
        if (nv.is_undefined()) { if (!o->delete_property(ctx, k, &ok)) return false; }
        else if (!ctx.create_data_property(o, k, nv)) return false;
      }
    }
  }
  Value key_v = name->kind == CellKind::String ? Value::String(static_cast<JsString*>(name)) : Value::Sym(static_cast<Symbol*>(name));
  return ctx.call(reviver, Value::Obj(holder), {key_v, val}, out);
}

Value json_parse(Context& ctx, CallArgs& a) {
  JsString* text;
  JS_TRY(ctx.to_string(a[0], &text));
  JsonParser p(ctx, text->str());
  Value v = p.parse_value();
  if (v.is_exception()) return v;
  p.skip_ws();
  if (p.pos < p.s.size()) return p.fail_at(p.pos);
  if (a[1].is_callable()) {
    Object* root = ctx.new_object();
    JS_TRY(ctx.create_data_property(root, ctx.atoms.empty, v));
    Value r;
    JS_TRY(internalize(ctx, root, ctx.atoms.empty, a[1], &r));
    return r;
  }
  return v;
}

// ---- stringify ----

struct Stringifier {
  Context& ctx;
  Value replacer_fn;
  std::vector<PropKey> allow;
  bool has_allow = false;
  std::u16string gap;
  std::u16string indent;
  std::vector<Object*> stack;
  std::u16string out;

  explicit Stringifier(Context& c) : ctx(c) {}

  void quote(const std::u16string& s) {
    out.push_back(u'"');
    static const char16_t hex[] = u"0123456789abcdef";
    for (size_t i = 0; i < s.size(); ++i) {
      char16_t c = s[i];
      switch (c) {
        case u'"': out += u"\\\""; continue;
        case u'\\': out += u"\\\\"; continue;
        case u'\b': out += u"\\b"; continue;
        case u'\f': out += u"\\f"; continue;
        case u'\n': out += u"\\n"; continue;
        case u'\r': out += u"\\r"; continue;
        case u'\t': out += u"\\t"; continue;
        default: break;
      }
      bool escape = c < 0x20;
      if (c >= 0xD800 && c <= 0xDBFF) {
        if (i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) { out.push_back(c); out.push_back(s[++i]); continue; }
        escape = true;
      } else if (c >= 0xDC00 && c <= 0xDFFF) {
        escape = true;
      }
      if (escape) {
        out += u"\\u";
        out.push_back(hex[(c >> 12) & 15]);
        out.push_back(hex[(c >> 8) & 15]);
        out.push_back(hex[(c >> 4) & 15]);
        out.push_back(hex[c & 15]);
      } else {
        out.push_back(c);
      }
    }
    out.push_back(u'"');
  }

  // Returns 1 if something was written, 0 for "undefined" (skip), -1 on error.
  int serialize_property(Object* holder, PropKey key, Value value) {
    if (value.is_object() || value.is_bigint()) {
      Value tj;
      if (!ctx.get(value, ctx.atoms.toJSON, &tj)) return -1;
      if (tj.is_callable()) {
        Value kv = key->kind == CellKind::String ? Value::String(static_cast<JsString*>(key)) : Value::Sym(static_cast<Symbol*>(key));
        Value r;
        if (!ctx.call(tj, value, {kv}, &r)) return -1;
        value = r;
      }
    }
    if (replacer_fn.is_callable()) {
      Value kv = key->kind == CellKind::String ? Value::String(static_cast<JsString*>(key)) : Value::Sym(static_cast<Symbol*>(key));
      Value r;
      if (!ctx.call(replacer_fn, Value::Obj(holder), {kv, value}, &r)) return -1;
      value = r;
    }
    if (value.is_object()) {
      Object* o = value.as_object();
      if (o->cls == Class::Number) {
        double d;
        if (!ctx.to_number(value, &d)) return -1;
        value = Value::Number(d);
      } else if (o->cls == Class::String) {
        JsString* s;
        if (!ctx.to_string(value, &s)) return -1;
        value = Value::String(s);
      } else if (o->cls == Class::Boolean || o->cls == Class::BigInt) {
        value = static_cast<PrimitiveObject*>(o)->prim;
      }
    }
    switch (value.tag) {
      case Tag::BigInt: ctx.throw_type_error("Do not know how to serialize a BigInt"); return -1;
      case Tag::Null: out += u"null"; return 1;
      case Tag::Boolean: out += value.boolean ? u"true" : u"false"; return 1;
      case Tag::String: quote(value.as_string()->str()); return 1;
      case Tag::Number:
        if (!std::isfinite(value.num)) out += u"null";
        else {
          std::string t = number_to_ascii(value.num);
          for (char c : t) out.push_back(static_cast<char16_t>(c));
        }
        return 1;
      case Tag::Object: {
        if (value.as_object()->callable) return 0;
        bool is_arr;
        if (!ctx.is_array(value, &is_arr)) return -1;
        return (is_arr ? serialize_array(value.as_object()) : serialize_object(value.as_object())) ? 1 : -1;
      }
      default: return 0;
    }
  }

  bool enter(Object* o) {
    if (std::find(stack.begin(), stack.end(), o) != stack.end()) {
      ctx.throw_type_error("Converting circular structure to JSON");
      return false;
    }
    if (stack.size() > 3000) {
      ctx.throw_range_error("Maximum call stack size exceeded");
      return false;
    }
    stack.push_back(o);
    return true;
  }

  bool serialize_object(Object* o) {
    if (!enter(o)) return false;
    const std::u16string stepback = indent;
    indent += gap;
    std::vector<PropKey> keys;
    if (has_allow) keys = allow;
    else {
      std::vector<PropKey> all;
      if (!o->own_keys(ctx, &all)) return false;
      for (PropKey k : all) {
        if (k->kind != CellKind::String) continue;
        PropDesc d;
        bool found;
        if (!o->get_own_property(ctx, k, &d, &found)) return false;
        if (found && d.enumerable) keys.push_back(k);
      }
    }
    out.push_back(u'{');
    bool first = true;
    for (PropKey k : keys) {
      Value v;
      if (!o->get(ctx, k, Value::Obj(o), &v)) return false;
      const size_t mark = out.size();
      if (!first) out.push_back(u',');
      if (!gap.empty()) { out.push_back(u'\n'); out += indent; }
      quote(static_cast<JsString*>(k)->str());
      out.push_back(u':');
      if (!gap.empty()) out.push_back(u' ');
      int r = serialize_property(o, k, v);
      if (r < 0) return false;
      if (r == 0) out.resize(mark);
      else first = false;
    }
    if (!first && !gap.empty()) { out.push_back(u'\n'); out += stepback; }
    out.push_back(u'}');
    stack.pop_back();
    indent = stepback;
    return true;
  }

  bool serialize_array(Object* o) {
    if (!enter(o)) return false;
    const std::u16string stepback = indent;
    indent += gap;
    uint64_t len;
    if (!ctx.length_of(o, &len)) return false;
    out.push_back(u'[');
    for (uint64_t i = 0; i < len; ++i) {
      if (i) out.push_back(u',');
      if (!gap.empty()) { out.push_back(u'\n'); out += indent; }
      PropKey k;
      if (!ctx.property_key_from_index(i, &k)) return false;
      Value v;
      if (!get_idx(ctx, o, i, &v)) return false;
      int r = serialize_property(o, k, v);
      if (r < 0) return false;
      if (r == 0) out += u"null";
    }
    if (len && !gap.empty()) { out.push_back(u'\n'); out += stepback; }
    out.push_back(u']');
    stack.pop_back();
    indent = stepback;
    return true;
  }
};

Value json_stringify(Context& ctx, CallArgs& a) {
  Stringifier st(ctx);
  if (a[1].is_callable()) {
    st.replacer_fn = a[1];
  } else if (a[1].is_object()) {
    bool is_arr;
    JS_TRY(ctx.is_array(a[1], &is_arr));
    if (is_arr) {
      st.has_allow = true;
      Object* r = a[1].as_object();
      uint64_t len;
      JS_TRY(ctx.length_of(r, &len));
      for (uint64_t i = 0; i < len; ++i) {
        Value v;
        JS_TRY(get_idx(ctx, r, i, &v));
        JsString* item = nullptr;
        if (v.is_string()) item = v.as_string();
        else if (v.is_number()) item = ctx.number_to_string(v.num);
        else if (v.is_object() && (v.as_object()->cls == Class::String || v.as_object()->cls == Class::Number)) JS_TRY(ctx.to_string(v, &item));
        if (!item) continue;
        PropKey k = ctx.to_atom(item);
        if (std::find(st.allow.begin(), st.allow.end(), k) == st.allow.end()) st.allow.push_back(k);
      }
    }
  }
  Value space = a[2];
  if (space.is_object()) {
    if (space.as_object()->cls == Class::Number) {
      double d;
      JS_TRY(ctx.to_number(space, &d));
      space = Value::Number(d);
    } else if (space.as_object()->cls == Class::String) {
      JsString* s;
      JS_TRY(ctx.to_string(space, &s));
      space = Value::String(s);
    }
  }
  if (space.is_number()) {
    double n;
    JS_TRY(ctx.to_integer(space, &n));
    n = std::min(n, 10.0);
    if (n >= 1) st.gap = std::u16string(static_cast<size_t>(n), u' ');
  } else if (space.is_string()) {
    st.gap = space.as_string()->str().substr(0, 10);
  }
  Object* wrapper = ctx.new_object();
  JS_TRY(ctx.create_data_property(wrapper, ctx.atoms.empty, a[0]));
  int r = st.serialize_property(wrapper, ctx.atoms.empty, a[0]);
  if (r < 0) return Value::Exception();
  if (r == 0) return Value::Undefined();
  return StrV(ctx, std::move(st.out));
}

}  // namespace

void setup_json(Context& ctx) {
  Object* j = ctx.new_object();
  j->cls = Class::JSON;
  ctx.define_data(ctx.intr.global, ctx.atom_ascii("JSON"), Value::Obj(j), kHiddenAttrs);
  ctx.define_method(j, "parse", json_parse, 2);
  ctx.define_method(j, "stringify", json_stringify, 3);
  ctx.define_data(j, ctx.sym.to_string_tag, Str(ctx, "JSON"), kConfigurable);
}

}  // namespace aios::js
