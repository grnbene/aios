// RegExp: the constructor, exec and the Symbol.match / replace / search / split protocol.
#include <algorithm>
#include <cmath>

#include "builtins.hpp"
#include "regexp.hpp"
#include "unicode.hpp"

namespace aios::js {

namespace {

struct RegExpObject final : Object {
  RegExpObject() : Object(Class::RegExp) {}
  std::shared_ptr<re::Program> prog;
  JsString* source = nullptr;
  JsString* flags = nullptr;
  void trace(Tracer& t) override { Object::trace(t); t.mark(source); t.mark(flags); }
};

struct RegExpStringIterator final : Object {
  RegExpStringIterator() : Object(Class::RegExpStringIterator) {}
  Object* matcher = nullptr;
  JsString* str = nullptr;
  bool global = false, unicode = false, done = false;
  void trace(Tracer& t) override { Object::trace(t); t.mark(matcher); t.mark(str); }
};

RegExpObject* as_regexp(const Value& v) {
  return v.is_object() && v.as_object()->cls == Class::RegExp ? static_cast<RegExpObject*>(v.as_object()) : nullptr;
}

size_t advance_index(const std::u16string& s, size_t i, bool unicode) {
  if (!unicode || i + 1 >= s.size()) return i + 1;
  if (uni::is_high_surrogate(s[i]) && uni::is_low_surrogate(s[i + 1])) return i + 2;
  return i + 1;
}

// ---- construction ----

std::shared_ptr<re::Program> compile_cached(Context& ctx, JsString* pattern, JsString* flags, std::string* err) {
  std::u16string key = flags->str() + u"/" + pattern->str();
  auto it = ctx.regexp_cache.find(key);
  if (it != ctx.regexp_cache.end()) return std::static_pointer_cast<re::Program>(it->second);
  std::unique_ptr<re::Program> prog = re::compile(pattern->str(), flags->str(), err);
  if (!prog) return nullptr;
  std::shared_ptr<re::Program> sp(std::move(prog));
  if (ctx.regexp_cache.size() > 4096) ctx.regexp_cache.clear();
  ctx.regexp_cache[key] = sp;
  return sp;
}

bool regexp_initialize(Context& ctx, RegExpObject* rx, JsString* pattern, JsString* flags) {
  std::string err;
  std::shared_ptr<re::Program> prog = compile_cached(ctx, pattern, flags, &err);
  if (!prog) {
    if (err == "Invalid flags") ctx.throw_syntax_error("Invalid flags supplied to RegExp constructor '" + flags->utf8() + "'");
    else ctx.throw_syntax_error("Invalid regular expression: /" + pattern->utf8() + "/" + flags->utf8() + ": " + err);
    return false;
  }
  rx->prog = prog;
  rx->source = pattern;
  rx->flags = flags;
  return ctx.put(rx, ctx.atoms.lastIndex, Value::Number(0), true);
}

RegExpObject* regexp_alloc(Context& ctx, Object* proto) {
  RegExpObject* rx = ctx.heap.make<RegExpObject>();
  rx->proto = proto;
  ctx.define_data(rx, ctx.atoms.lastIndex, Value::Number(0), kWritable);
  return rx;
}

}  // namespace

bool is_regexp(Context& ctx, const Value& v, bool* out) {
  if (!v.is_object()) { *out = false; return true; }
  Value m;
  if (!v.as_object()->get(ctx, ctx.sym.match, v, &m)) return false;
  if (!m.is_undefined()) { *out = Context::to_boolean(m); return true; }
  *out = v.as_object()->cls == Class::RegExp;
  return true;
}

Object* regexp_create(Context& ctx, JsString* pattern, JsString* flags) {
  RegExpObject* rx = regexp_alloc(ctx, ctx.intr.regexp_proto);
  if (!regexp_initialize(ctx, rx, pattern, flags)) return nullptr;
  return rx;
}

namespace {

Value regexp_ctor(Context& ctx, CallArgs& a) {
  const Value pattern = a[0], flags = a[1];
  bool pattern_is_re;
  JS_TRY(is_regexp(ctx, pattern, &pattern_is_re));
  Value new_target = a.new_target;
  if (!a.is_construct()) {
    new_target = Value::Obj(a.callee);
    if (pattern_is_re && flags.is_undefined()) {
      Value c;
      JS_TRY(ctx.get(pattern, ctx.atoms.constructor, &c));
      if (c.is_object() && c.as_object() == a.callee) return pattern;
    }
  }
  Value p, f;
  if (RegExpObject* src = as_regexp(pattern)) {
    p = Value::String(src->source);
    f = flags.is_undefined() ? Value::String(src->flags) : flags;
  } else if (pattern_is_re) {
    JS_TRY(ctx.get(pattern, ctx.atoms.source, &p));
    if (flags.is_undefined()) JS_TRY(ctx.get(pattern, ctx.atoms.flags, &f));
    else f = flags;
  } else {
    p = pattern;
    f = flags;
  }
  Object* proto;
  JS_TRY(ctx.prototype_from_ctor(new_target, ctx.intr.regexp_proto, &proto));
  RegExpObject* rx = regexp_alloc(ctx, proto);
  JsString *ps, *fs;
  if (p.is_undefined()) ps = ctx.atoms.empty;
  else JS_TRY(ctx.to_string(p, &ps));
  if (f.is_undefined()) fs = ctx.atoms.empty;
  else JS_TRY(ctx.to_string(f, &fs));
  JS_TRY(regexp_initialize(ctx, rx, ps, fs));
  return Value::Obj(rx);
}

Value regexp_species(Context&, CallArgs& a) { return a.this_value; }

// ---- exec ----

Value make_match_result(Context& ctx, RegExpObject* rx, JsString* s, const std::vector<int>& caps) {
  const re::Program& p = *rx->prog;
  std::vector<Value> elems;
  for (int g = 0; g < p.ncaps; ++g) {
    const int st = caps[static_cast<size_t>(g) * 2], en = caps[static_cast<size_t>(g) * 2 + 1];
    elems.push_back(st >= 0 && en >= 0 ? Value::String(ctx.substring(s, static_cast<uint32_t>(st), static_cast<uint32_t>(en))) : Value::Undefined());
  }
  ArrayObject* arr = ctx.new_array_from(elems);
  ctx.define_data(arr, ctx.atoms.index, Value::Number(caps[0]));
  ctx.define_data(arr, ctx.atoms.input, Value::String(s));
  Value groups = Value::Undefined();
  if (p.has_named) {
    Object* g = ctx.new_object(nullptr);
    for (int i = 1; i < p.ncaps; ++i)
      if (!p.group_names[static_cast<size_t>(i)].empty()) ctx.define_data(g, ctx.atom(p.group_names[static_cast<size_t>(i)]), elems[static_cast<size_t>(i)]);
    groups = Value::Obj(g);
  }
  ctx.define_data(arr, ctx.atoms.groups, groups);
  if (p.has_indices) {
    std::vector<Value> ind;
    for (int g = 0; g < p.ncaps; ++g) {
      const int st = caps[static_cast<size_t>(g) * 2], en = caps[static_cast<size_t>(g) * 2 + 1];
      ind.push_back(st >= 0 && en >= 0 ? Value::Obj(ctx.new_array_from({Value::Number(st), Value::Number(en)})) : Value::Undefined());
    }
    ArrayObject* ia = ctx.new_array_from(ind);
    Value igroups = Value::Undefined();
    if (p.has_named) {
      Object* g = ctx.new_object(nullptr);
      for (int i = 1; i < p.ncaps; ++i)
        if (!p.group_names[static_cast<size_t>(i)].empty()) ctx.define_data(g, ctx.atom(p.group_names[static_cast<size_t>(i)]), ind[static_cast<size_t>(i)]);
      igroups = Value::Obj(g);
    }
    ctx.define_data(ia, ctx.atoms.groups, igroups);
    ctx.define_data(arr, ctx.atom_ascii("indices"), Value::Obj(ia));
  }
  return Value::Obj(arr);
}

// RegExpBuiltinExec: null, or the match array; `caps_out` (optional) receives the capture slots.
bool builtin_exec(Context& ctx, RegExpObject* rx, JsString* s, Value* out, std::vector<int>* caps_out = nullptr) {
  Value li;
  uint64_t last_index;
  Prop* lp = rx->props.find(ctx.atoms.lastIndex);
  if (lp && !lp->is_accessor()) li = lp->value;
  else if (!rx->get(ctx, ctx.atoms.lastIndex, Value::Obj(rx), &li)) return false;
  if (li.is_number() && li.num >= 0 && li.num <= 9007199254740991.0 && li.num == std::floor(li.num)) last_index = static_cast<uint64_t>(li.num);
  else if (!ctx.to_length(li, &last_index)) return false;
  const re::Program& p = *rx->prog;
  const bool global = p.global, sticky = p.sticky;
  if (!global && !sticky) last_index = 0;
  auto set_last_index = [&](double v) {
    if (lp && !lp->is_accessor() && (lp->attrs & kWritable)) { lp->value = Value::Number(v); return true; }
    return ctx.put(rx, ctx.atoms.lastIndex, Value::Number(v), true);
  };
  if (last_index > s->length) {
    if (global || sticky) { if (!set_last_index(0)) return false; }
    *out = Value::Null();
    return true;
  }
  std::vector<int> caps;
  re::Result r = re::exec(p, s->str(), static_cast<size_t>(last_index), sticky, &caps);
  if (r == re::Result::TooComplex) {
    ctx.throw_range_error("Maximum call stack size exceeded (regular expression too complex)");
    return false;
  }
  if (r == re::Result::NoMatch) {
    if (global || sticky) { if (!set_last_index(0)) return false; }
    *out = Value::Null();
    return true;
  }
  if (global || sticky) { if (!set_last_index(caps[1])) return false; }
  if (caps_out) *caps_out = caps;
  *out = make_match_result(ctx, rx, s, caps);
  return true;
}

// RegExpExec(R, S): honours a user-defined `exec`.
bool regexp_exec_abstract(Context& ctx, Object* r, JsString* s, Value* out) {
  Value exec;
  if (!r->get(ctx, ctx.atom_ascii("exec"), Value::Obj(r), &exec)) return false;
  if (exec.is_callable()) {
    if (RegExpObject* rx = as_regexp(Value::Obj(r)); rx && exec.as_object() == ctx.intr.regexp_exec) return builtin_exec(ctx, rx, s, out);
    Value res;
    if (!ctx.call(exec, Value::Obj(r), {Value::String(s)}, &res)) return false;
    if (!res.is_object() && !res.is_null()) {
      ctx.throw_type_error("object null is not a function");
      return false;
    }
    *out = res;
    return true;
  }
  RegExpObject* rx = as_regexp(Value::Obj(r));
  if (!rx) {
    ctx.throw_type_error("RegExp exec method called on incompatible receiver " + vm_describe(ctx, Value::Obj(r)));
    return false;
  }
  return builtin_exec(ctx, rx, s, out);
}

Value regexp_exec(Context& ctx, CallArgs& a) {
  RegExpObject* rx = as_regexp(a.this_value);
  if (!rx) {
    ctx.throw_type_error("Method RegExp.prototype.exec called on incompatible receiver " + vm_describe(ctx, a.this_value));
    return Value::Exception();
  }
  JsString* s;
  JS_TRY(ctx.to_string(a[0], &s));
  Value r;
  JS_TRY(builtin_exec(ctx, rx, s, &r));
  return r;
}

Value regexp_test(Context& ctx, CallArgs& a) {
  if (!a.this_value.is_object()) {
    ctx.throw_type_error("Method RegExp.prototype.test called on incompatible receiver " + vm_describe(ctx, a.this_value));
    return Value::Exception();
  }
  JsString* s;
  JS_TRY(ctx.to_string(a[0], &s));
  Value r;
  JS_TRY(regexp_exec_abstract(ctx, a.this_value.as_object(), s, &r));
  return Value::Bool(!r.is_null());
}

Value regexp_to_string(Context& ctx, CallArgs& a) {
  if (!a.this_value.is_object()) {
    ctx.throw_type_error("Method RegExp.prototype.toString called on incompatible receiver " + vm_describe(ctx, a.this_value));
    return Value::Exception();
  }
  Value src, fl;
  JS_TRY(ctx.get(a.this_value, ctx.atoms.source, &src));
  JS_TRY(ctx.get(a.this_value, ctx.atoms.flags, &fl));
  JsString *ss, *fs;
  JS_TRY(ctx.to_string(src, &ss));
  JS_TRY(ctx.to_string(fl, &fs));
  return StrV(ctx, u"/" + ss->str() + u"/" + fs->str());
}

// ---- accessors ----

Value flag_getter(Context& ctx, CallArgs& a, char16_t flag, const char* name) {
  if (!a.this_value.is_object()) {
    ctx.throw_type_error(std::string("RegExp.prototype.") + name + " getter called on non-object " + vm_describe(ctx, a.this_value));
    return Value::Exception();
  }
  RegExpObject* rx = as_regexp(a.this_value);
  if (!rx) {
    if (a.this_value.as_object() == ctx.intr.regexp_proto) return Value::Undefined();
    ctx.throw_type_error(std::string("RegExp.prototype.") + name + " getter called on non-RegExp object");
    return Value::Exception();
  }
  return Value::Bool(rx->flags->str().find(flag) != std::u16string::npos);
}

#define FLAG_GETTER(fn, ch, nm) \
  Value fn(Context& ctx, CallArgs& a) { return flag_getter(ctx, a, ch, nm); }
FLAG_GETTER(regexp_global, u'g', "global")
FLAG_GETTER(regexp_ignore_case, u'i', "ignoreCase")
FLAG_GETTER(regexp_multiline, u'm', "multiline")
FLAG_GETTER(regexp_dot_all, u's', "dotAll")
FLAG_GETTER(regexp_unicode, u'u', "unicode")
FLAG_GETTER(regexp_unicode_sets, u'v', "unicodeSets")
FLAG_GETTER(regexp_sticky, u'y', "sticky")
FLAG_GETTER(regexp_has_indices, u'd', "hasIndices")
#undef FLAG_GETTER

Value regexp_flags(Context& ctx, CallArgs& a) {
  if (!a.this_value.is_object()) {
    ctx.throw_type_error("RegExp.prototype.flags getter called on non-object " + vm_describe(ctx, a.this_value));
    return Value::Exception();
  }
  struct F { const char* name; char16_t ch; } fl[] = {{"hasIndices", u'd'}, {"global", u'g'}, {"ignoreCase", u'i'}, {"multiline", u'm'},
                                                       {"dotAll", u's'}, {"unicode", u'u'}, {"unicodeSets", u'v'}, {"sticky", u'y'}};
  std::u16string out;
  for (const F& f : fl) {
    Value v;
    JS_TRY(ctx.get(a.this_value, ctx.atom_ascii(f.name), &v));
    if (Context::to_boolean(v)) out.push_back(f.ch);
  }
  return StrV(ctx, std::move(out));
}

std::u16string escape_source(const std::u16string& src) {
  if (src.empty()) return u"(?:)";
  std::u16string out;
  bool in_class = false;
  for (size_t i = 0; i < src.size(); ++i) {
    char16_t c = src[i];
    if (c == u'\\') {
      out.push_back(c);
      if (i + 1 < src.size()) out.push_back(src[++i]);
      continue;
    }
    if (c == u'[') in_class = true;
    else if (c == u']') in_class = false;
    if (c == u'/' && !in_class) out += u"\\/";
    else if (c == u'\n') out += u"\\n";
    else if (c == u'\r') out += u"\\r";
    else if (c == 0x2028) out += u"\\u2028";
    else if (c == 0x2029) out += u"\\u2029";
    else out.push_back(c);
  }
  return out;
}

Value regexp_source(Context& ctx, CallArgs& a) {
  if (!a.this_value.is_object()) {
    ctx.throw_type_error("RegExp.prototype.source getter called on non-object " + vm_describe(ctx, a.this_value));
    return Value::Exception();
  }
  RegExpObject* rx = as_regexp(a.this_value);
  if (!rx) {
    if (a.this_value.as_object() == ctx.intr.regexp_proto) return Str(ctx, "(?:)");
    ctx.throw_type_error("RegExp.prototype.source getter called on non-RegExp object");
    return Value::Exception();
  }
  return StrV(ctx, escape_source(rx->source->str()));
}

Value regexp_compile(Context& ctx, CallArgs& a) {
  RegExpObject* rx = as_regexp(a.this_value);
  if (!rx) {
    ctx.throw_type_error("Method RegExp.prototype.compile called on incompatible receiver " + vm_describe(ctx, a.this_value));
    return Value::Exception();
  }
  JsString *p, *f;
  if (RegExpObject* src = as_regexp(a[0])) {
    if (!a[1].is_undefined()) {
      ctx.throw_type_error("Invalid flags");
      return Value::Exception();
    }
    p = src->source;
    f = src->flags;
  } else {
    if (a[0].is_undefined()) p = ctx.atoms.empty;
    else JS_TRY(ctx.to_string(a[0], &p));
    if (a[1].is_undefined()) f = ctx.atoms.empty;
    else JS_TRY(ctx.to_string(a[1], &f));
  }
  JS_TRY(regexp_initialize(ctx, rx, p, f));
  return a.this_value;
}

// ---- protocol methods ----

bool flags_string(Context& ctx, Object* rx, JsString** out) {
  Value fl;
  if (!rx->get(ctx, ctx.atoms.flags, Value::Obj(rx), &fl)) return false;
  return ctx.to_string(fl, out);
}

bool set_last_index(Context& ctx, Object* rx, double v) { return ctx.put_v(Value::Obj(rx), ctx.atoms.lastIndex, Value::Number(v), true); }

bool get_last_index(Context& ctx, Object* rx, uint64_t* out) {
  Value li;
  if (!rx->get(ctx, ctx.atoms.lastIndex, Value::Obj(rx), &li)) return false;
  return ctx.to_length(li, out);
}

Value regexp_match(Context& ctx, CallArgs& a) {
  if (!a.this_value.is_object()) {
    ctx.throw_type_error("RegExp.prototype[Symbol.match] called on non-object");
    return Value::Exception();
  }
  Object* rx = a.this_value.as_object();
  JsString* s;
  JS_TRY(ctx.to_string(a[0], &s));
  JsString* flags;
  JS_TRY(flags_string(ctx, rx, &flags));
  if (flags->str().find(u'g') == std::u16string::npos) {
    Value r;
    JS_TRY(regexp_exec_abstract(ctx, rx, s, &r));
    return r;
  }
  const bool unicode = flags->str().find(u'u') != std::u16string::npos || flags->str().find(u'v') != std::u16string::npos;
  JS_TRY(set_last_index(ctx, rx, 0));
  std::vector<Value> matches;
  for (;;) {
    Value r;
    JS_TRY(regexp_exec_abstract(ctx, rx, s, &r));
    if (r.is_null()) break;
    Value m0;
    JS_TRY(get_idx(ctx, r.as_object(), 0, &m0));
    JsString* ms;
    JS_TRY(ctx.to_string(m0, &ms));
    matches.push_back(Value::String(ms));
    if (ms->length == 0) {
      uint64_t li;
      JS_TRY(get_last_index(ctx, rx, &li));
      JS_TRY(set_last_index(ctx, rx, static_cast<double>(advance_index(s->str(), static_cast<size_t>(li), unicode))));
    }
  }
  if (matches.empty()) return Value::Null();
  return Value::Obj(ctx.new_array_from(matches));
}

Value regexp_match_all(Context& ctx, CallArgs& a) {
  if (!a.this_value.is_object()) {
    ctx.throw_type_error("RegExp.prototype[Symbol.matchAll] called on non-object");
    return Value::Exception();
  }
  Object* r = a.this_value.as_object();
  JsString* s;
  JS_TRY(ctx.to_string(a[0], &s));
  Value c;
  JS_TRY(ctx.species_constructor(r, Value::Obj(ctx.intr.regexp_ctor), &c));
  JsString* flags;
  JS_TRY(flags_string(ctx, r, &flags));
  Value args[2] = {a.this_value, Value::String(flags)};
  Value matcher;
  JS_TRY(ctx.construct(c, args, 2, c, &matcher));
  uint64_t li;
  JS_TRY(get_last_index(ctx, r, &li));
  JS_TRY(set_last_index(ctx, matcher.as_object(), static_cast<double>(li)));
  RegExpStringIterator* it = ctx.heap.make<RegExpStringIterator>();
  it->proto = ctx.intr.regexp_string_iterator_proto;
  it->matcher = matcher.as_object();
  it->str = s;
  it->global = flags->str().find(u'g') != std::u16string::npos;
  it->unicode = flags->str().find(u'u') != std::u16string::npos || flags->str().find(u'v') != std::u16string::npos;
  return Value::Obj(it);
}

Value regexp_string_iterator_next(Context& ctx, CallArgs& a) {
  if (!a.this_value.is_object() || a.this_value.as_object()->cls != Class::RegExpStringIterator) {
    ctx.throw_type_error("next method called on incompatible receiver " + vm_describe(ctx, a.this_value));
    return Value::Exception();
  }
  RegExpStringIterator* it = static_cast<RegExpStringIterator*>(a.this_value.as_object());
  if (it->done) return Value::Obj(ctx.create_iter_result(Value::Undefined(), true));
  Value r;
  JS_TRY(regexp_exec_abstract(ctx, it->matcher, it->str, &r));
  if (r.is_null()) {
    it->done = true;
    return Value::Obj(ctx.create_iter_result(Value::Undefined(), true));
  }
  if (!it->global) {
    it->done = true;
    return Value::Obj(ctx.create_iter_result(r, false));
  }
  Value m0;
  JS_TRY(get_idx(ctx, r.as_object(), 0, &m0));
  JsString* ms;
  JS_TRY(ctx.to_string(m0, &ms));
  if (ms->length == 0) {
    uint64_t li;
    JS_TRY(get_last_index(ctx, it->matcher, &li));
    JS_TRY(set_last_index(ctx, it->matcher, static_cast<double>(advance_index(it->str->str(), static_cast<size_t>(li), it->unicode))));
  }
  return Value::Obj(ctx.create_iter_result(r, false));
}

Value regexp_search(Context& ctx, CallArgs& a) {
  if (!a.this_value.is_object()) {
    ctx.throw_type_error("RegExp.prototype[Symbol.search] called on non-object");
    return Value::Exception();
  }
  Object* rx = a.this_value.as_object();
  JsString* s;
  JS_TRY(ctx.to_string(a[0], &s));
  Value prev;
  JS_TRY(rx->get(ctx, ctx.atoms.lastIndex, a.this_value, &prev));
  if (!ctx.same_value(prev, Value::Number(0))) JS_TRY(set_last_index(ctx, rx, 0));
  Value r;
  JS_TRY(regexp_exec_abstract(ctx, rx, s, &r));
  Value cur;
  JS_TRY(rx->get(ctx, ctx.atoms.lastIndex, a.this_value, &cur));
  if (!ctx.same_value(cur, prev)) JS_TRY(ctx.put_v(a.this_value, ctx.atoms.lastIndex, prev, true));
  if (r.is_null()) return Value::Number(-1);
  Value idx;
  JS_TRY(r.as_object()->get(ctx, ctx.atoms.index, r, &idx));
  return idx;
}

Value regexp_replace(Context& ctx, CallArgs& a) {
  if (!a.this_value.is_object()) {
    ctx.throw_type_error("RegExp.prototype[Symbol.replace] called on non-object");
    return Value::Exception();
  }
  Object* rx = a.this_value.as_object();
  JsString* s;
  JS_TRY(ctx.to_string(a[0], &s));
  const size_t length = s->length;
  Value replace_value = a[1];
  const bool functional = replace_value.is_callable();
  JsString* replace_str = nullptr;
  if (!functional) JS_TRY(ctx.to_string(replace_value, &replace_str));
  JsString* flags;
  JS_TRY(flags_string(ctx, rx, &flags));
  const bool global = flags->str().find(u'g') != std::u16string::npos;
  const bool unicode = flags->str().find(u'u') != std::u16string::npos || flags->str().find(u'v') != std::u16string::npos;
  if (global) JS_TRY(set_last_index(ctx, rx, 0));
  std::vector<Value> results;
  for (;;) {
    Value r;
    JS_TRY(regexp_exec_abstract(ctx, rx, s, &r));
    if (r.is_null()) break;
    results.push_back(r);
    if (!global) break;
    Value m0;
    JS_TRY(get_idx(ctx, r.as_object(), 0, &m0));
    JsString* ms;
    JS_TRY(ctx.to_string(m0, &ms));
    if (ms->length == 0) {
      uint64_t li;
      JS_TRY(get_last_index(ctx, rx, &li));
      JS_TRY(set_last_index(ctx, rx, static_cast<double>(advance_index(s->str(), static_cast<size_t>(li), unicode))));
    }
  }
  std::u16string out;
  size_t next_pos = 0;
  for (const Value& rv : results) {
    Object* r = rv.as_object();
    uint64_t ncaps;
    JS_TRY(ctx.length_of(r, &ncaps));
    ncaps = ncaps > 0 ? ncaps - 1 : 0;
    Value m0;
    JS_TRY(get_idx(ctx, r, 0, &m0));
    JsString* matched;
    JS_TRY(ctx.to_string(m0, &matched));
    Value pv;
    JS_TRY(r->get(ctx, ctx.atoms.index, rv, &pv));
    double pd;
    JS_TRY(ctx.to_integer(pv, &pd));
    const size_t position = static_cast<size_t>(std::max(std::min(pd, static_cast<double>(length)), 0.0));
    std::vector<Value> captures;
    for (uint64_t n = 1; n <= ncaps; ++n) {
      Value c;
      JS_TRY(get_idx(ctx, r, n, &c));
      if (!c.is_undefined()) {
        JsString* cs;
        JS_TRY(ctx.to_string(c, &cs));
        c = Value::String(cs);
      }
      captures.push_back(c);
    }
    Value named;
    JS_TRY(r->get(ctx, ctx.atoms.groups, rv, &named));
    std::u16string replacement;
    if (functional) {
      std::vector<Value> args;
      args.push_back(Value::String(matched));
      for (const Value& c : captures) args.push_back(c);
      args.push_back(Value::Number(static_cast<double>(position)));
      args.push_back(Value::String(s));
      if (!named.is_undefined()) args.push_back(named);
      Value rep;
      JS_TRY(ctx.call(replace_value, Value::Undefined(), args.data(), static_cast<int>(args.size()), &rep));
      JsString* rs;
      JS_TRY(ctx.to_string(rep, &rs));
      replacement = rs->str();
    } else {
      Value named_obj = named;
      if (!named.is_undefined()) {
        Object* no;
        JS_TRY(ctx.to_object(named, &no));
        named_obj = Value::Obj(no);
      }
      JS_TRY(get_substitution(ctx, matched, s, position, captures, named_obj, replace_str, &replacement));
    }
    if (position >= next_pos) {
      out.append(s->str(), next_pos, position - next_pos);
      out += replacement;
      next_pos = position + matched->length;
    }
  }
  if (next_pos < length) out.append(s->str(), next_pos, std::u16string::npos);
  return StrV(ctx, std::move(out));
}

Value regexp_split(Context& ctx, CallArgs& a) {
  if (!a.this_value.is_object()) {
    ctx.throw_type_error("RegExp.prototype[Symbol.split] called on non-object");
    return Value::Exception();
  }
  Object* rx = a.this_value.as_object();
  JsString* s;
  JS_TRY(ctx.to_string(a[0], &s));
  Value c;
  JS_TRY(ctx.species_constructor(rx, Value::Obj(ctx.intr.regexp_ctor), &c));
  JsString* flags;
  JS_TRY(flags_string(ctx, rx, &flags));
  const bool unicode = flags->str().find(u'u') != std::u16string::npos || flags->str().find(u'v') != std::u16string::npos;
  std::u16string new_flags = flags->str();
  if (new_flags.find(u'y') == std::u16string::npos) new_flags += u'y';
  Value args[2] = {a.this_value, StrV(ctx, new_flags)};
  Value splitter_v;
  JS_TRY(ctx.construct(c, args, 2, c, &splitter_v));
  Object* splitter = splitter_v.as_object();
  std::vector<Value> out;
  uint32_t lim = 0xFFFFFFFFu;
  if (!a[1].is_undefined()) JS_TRY(ctx.to_uint32(a[1], &lim));
  if (lim == 0) return Value::Obj(ctx.new_array_from(out));
  const size_t size = s->length;
  if (size == 0) {
    Value z;
    JS_TRY(regexp_exec_abstract(ctx, splitter, s, &z));
    if (!z.is_null()) return Value::Obj(ctx.new_array_from(out));
    out.push_back(Value::String(s));
    return Value::Obj(ctx.new_array_from(out));
  }
  size_t p = 0, q = 0;
  while (q < size) {
    JS_TRY(set_last_index(ctx, splitter, static_cast<double>(q)));
    Value z;
    JS_TRY(regexp_exec_abstract(ctx, splitter, s, &z));
    if (z.is_null()) { q = advance_index(s->str(), q, unicode); continue; }
    uint64_t e;
    JS_TRY(get_last_index(ctx, splitter, &e));
    e = std::min<uint64_t>(e, size);
    if (e == p) { q = advance_index(s->str(), q, unicode); continue; }
    out.push_back(Value::String(ctx.substring(s, static_cast<uint32_t>(p), static_cast<uint32_t>(q))));
    if (out.size() == lim) return Value::Obj(ctx.new_array_from(out));
    p = static_cast<size_t>(e);
    uint64_t ncaps;
    JS_TRY(ctx.length_of(z.as_object(), &ncaps));
    ncaps = ncaps > 0 ? ncaps - 1 : 0;
    for (uint64_t i = 1; i <= ncaps; ++i) {
      Value cap;
      JS_TRY(get_idx(ctx, z.as_object(), i, &cap));
      out.push_back(cap);
      if (out.size() == lim) return Value::Obj(ctx.new_array_from(out));
    }
    q = p;
  }
  out.push_back(Value::String(ctx.substring(s, static_cast<uint32_t>(p), static_cast<uint32_t>(size))));
  return Value::Obj(ctx.new_array_from(out));
}

}  // namespace

void setup_regexp(Context& ctx) {
  Object* proto = ctx.new_object();
  ctx.intr.regexp_proto = proto;
  Function* ctor = new_ctor(ctx, "RegExp", regexp_ctor, 2, proto);
  ctx.intr.regexp_ctor = ctor;
  ctx.define_getter(ctor, ctx.sym.species, "[Symbol.species]", regexp_species);
  Function* exec = ctx.define_method(proto, "exec", regexp_exec, 1);
  ctx.intr.regexp_exec = exec;
  ctx.define_method(proto, "test", regexp_test, 1);
  ctx.define_method(proto, "toString", regexp_to_string, 0);
  ctx.define_method(proto, "compile", regexp_compile, 2);
  ctx.define_getter(proto, "dotAll", regexp_dot_all);
  ctx.define_getter(proto, "flags", regexp_flags);
  ctx.define_getter(proto, "global", regexp_global);
  ctx.define_getter(proto, "hasIndices", regexp_has_indices);
  ctx.define_getter(proto, "ignoreCase", regexp_ignore_case);
  ctx.define_getter(proto, "multiline", regexp_multiline);
  ctx.define_getter(proto, "source", regexp_source);
  ctx.define_getter(proto, "sticky", regexp_sticky);
  ctx.define_getter(proto, "unicode", regexp_unicode);
  ctx.define_getter(proto, "unicodeSets", regexp_unicode_sets);
  ctx.define_method(proto, ctx.sym.match, "[Symbol.match]", regexp_match, 1);
  ctx.define_method(proto, ctx.sym.match_all, "[Symbol.matchAll]", regexp_match_all, 1);
  ctx.define_method(proto, ctx.sym.replace, "[Symbol.replace]", regexp_replace, 2);
  ctx.define_method(proto, ctx.sym.search, "[Symbol.search]", regexp_search, 1);
  ctx.define_method(proto, ctx.sym.split, "[Symbol.split]", regexp_split, 2);
  Object* sip = ctx.new_object(ctx.intr.iterator_proto);
  ctx.intr.regexp_string_iterator_proto = sip;
  ctx.define_method(sip, "next", regexp_string_iterator_next, 0);
  ctx.define_data(sip, ctx.sym.to_string_tag, Str(ctx, "RegExp String Iterator"), kConfigurable);
}

}  // namespace aios::js
