// The engine context: strings, objects, conversions, property access, calls, iteration, jobs and collection.
#include <algorithm>
#include "aios/hal/host.hpp"
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "bigint.hpp"
#include "internal.hpp"
#include "internal_num.hpp"

namespace aios::js {

namespace {

const char* const kErrorNames[] = {"Error", "TypeError", "RangeError", "ReferenceError", "SyntaxError", "EvalError", "URIError", "AggregateError"};

std::string describe_for_error(Context& ctx, const Value& v) {
  switch (v.tag) {
    case Tag::Undefined: return "undefined";
    case Tag::Null: return "null";
    case Tag::Boolean: return v.boolean ? "true" : "false";
    case Tag::Number: return number_to_ascii(v.num);
    case Tag::String: return "\"" + v.as_string()->utf8() + "\"";
    case Tag::Symbol: return "Symbol()";
    case Tag::BigInt: return big::to_string(v.as_bigint()->n) + "n";
    case Tag::Object: {
      Object* o = v.as_object();
      if (o->callable) {
        Value n;
        if (o->exotic_get == false && o->get(ctx, ctx.atoms.name, v, &n) && n.is_string() && n.as_string()->length) return n.as_string()->utf8();
        return "function";
      }
      return o->cls == Class::Array ? "array" : "object";
    }
    default: return "value";
  }
}

}  // namespace

Context::Context() {
  now_ms = [] { return 0.0; };
  tz_offset_minutes = [](double) { return 0.0; };
  random_double = [] {
    static uint64_t s = 88172645463325252ull;
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    return static_cast<double>(s >> 11) / 9007199254740992.0;
  };
  vm_init(*this);
#ifndef AIOS_METAL
  if (!aios::hal::process_host().env("AIOS_JS_GC_STRESS").empty()) heap.set_stress(true);
#endif
  init_realm();
}

Context::~Context() { vm_shutdown(*this); }

void Context::init_realm() {
  // atoms
#define X(n)                                                      \
  {                                                               \
    std::string nm = #n;                                          \
    if (nm == "proto__") nm = "__proto__";                        \
    else if (!nm.empty() && nm.back() == '_') nm.pop_back();      \
    atoms.n = heap.intern_utf8(nm);                               \
  }
  AIOS_JS_ATOMS(X)
#undef X
  atoms.empty = heap.intern(u"");
  // well-known symbols
  auto mk = [&](const char* d) { Symbol* s = new_symbol(heap.intern_utf8(d)); return s; };
  sym.iterator = mk("Symbol.iterator");
  sym.async_iterator = mk("Symbol.asyncIterator");
  sym.has_instance = mk("Symbol.hasInstance");
  sym.to_primitive = mk("Symbol.toPrimitive");
  sym.to_string_tag = mk("Symbol.toStringTag");
  sym.species = mk("Symbol.species");
  sym.is_concat_spreadable = mk("Symbol.isConcatSpreadable");
  sym.unscopables = mk("Symbol.unscopables");
  sym.match = mk("Symbol.match");
  sym.match_all = mk("Symbol.matchAll");
  sym.replace = mk("Symbol.replace");
  sym.search = mk("Symbol.search");
  sym.split = mk("Symbol.split");

  // The two prototypes every other object hangs from.
  intr.object_proto = heap.make<Object>(Class::Object);
  Function* fp = heap.make<Function>();
  fp->proto = intr.object_proto;
  fp->kind = Function::Kind::Native;
  fp->native = [](Context&, CallArgs&) { return Value::Undefined(); };
  intr.function_proto = fp;
  intr.global = heap.make<Object>(Class::Global);
  intr.global->proto = intr.object_proto;
  // Array.prototype is itself an array.
  ArrayObject* ap = heap.make<ArrayObject>();
  ap->proto = intr.object_proto;
  intr.array_proto = ap;

  setup_object(*this);
  setup_function(*this);
  setup_error(*this);
  setup_array(*this);
  setup_iterators(*this);
  setup_string(*this);
  setup_number(*this);
  setup_boolean(*this);
  setup_bigint(*this);
  setup_symbol(*this);
  setup_math(*this);
  setup_json(*this);
  setup_global_functions(*this);
  setup_generator(*this);
  setup_promise(*this);
  setup_collections(*this);
  setup_regexp(*this);
  setup_date(*this);
  setup_typedarray(*this);
  setup_reflect(*this);
  setup_proxy(*this);
  setup_misc(*this);
}

// ---------------------------------------------------------------------------------------------------
// errors
// ---------------------------------------------------------------------------------------------------

Object* Context::new_error(ErrorKind k, std::string_view msg) {
  Object* e = heap.make<Object>(Class::Error);
  e->proto = intr.error_protos[static_cast<int>(k)] ? intr.error_protos[static_cast<int>(k)] : intr.error_proto;
  if (!msg.empty()) define_data(e, atoms.message, Value::String(new_string_utf8(msg)), kHiddenAttrs);
  std::string st = std::string(kErrorNames[static_cast<int>(k)]);
  if (!msg.empty()) st += ": " + std::string(msg);
  st += "\n" + vm_stack_trace(*this, 0);
  define_data(e, atoms.stack, Value::String(new_string_utf8(st)), kHiddenAttrs);
  return e;
}

Value Context::throw_error(ErrorKind k, std::string_view msg) {
  return throw_value(Value::Obj(new_error(k, msg)));
}

std::string Context::describe_exception(Value ex) {
  if (ex.is_object()) {
    Object* o = ex.as_object();
    Value st;
    if (o->get(*this, atoms.stack, ex, &st) && st.is_string() && st.as_string()->length) return st.as_string()->utf8();
    clear_exception();
    Value name, msg;
    bool ok = o->get(*this, atoms.name, ex, &name) && o->get(*this, atoms.message, ex, &msg);
    if (ok && name.is_string()) {
      std::string s = name.as_string()->utf8();
      if (msg.is_string() && msg.as_string()->length) s += ": " + msg.as_string()->utf8();
      return s;
    }
    clear_exception();
  }
  JsString* s = nullptr;
  Value saved = exception_;
  bool had = exception_set_;
  if (to_string(ex, &s)) return s->utf8();
  exception_ = saved;
  exception_set_ = had;
  return "(unprintable exception)";
}

// ---------------------------------------------------------------------------------------------------
// strings and symbols
// ---------------------------------------------------------------------------------------------------

JsString* Context::new_string(std::u16string s) {
  JsString* r = heap.make<JsString>();
  r->length = static_cast<uint32_t>(s.size());
  r->flat = std::move(s);
  heap.note_bytes(static_cast<long>(r->flat.capacity() * 2));
  return r;
}

JsString* Context::new_string_utf8(std::string_view s) {
  JsString* a = heap.intern_utf8(s);   // decode via the atom path, then copy: simple and correct
  return new_string(a->str());
}

JsString* Context::new_string_ascii(std::string_view s) {
  std::u16string u(s.size(), u' ');
  for (size_t i = 0; i < s.size(); ++i) u[i] = static_cast<unsigned char>(s[i]);
  return new_string(std::move(u));
}

JsString* Context::atom_ascii(std::string_view s) {
  std::u16string u(s.size(), u' ');
  for (size_t i = 0; i < s.size(); ++i) u[i] = static_cast<unsigned char>(s[i]);
  return heap.intern(u);
}

JsString* Context::to_atom(JsString* s) {
  if (s->is_atom) return s;
  return heap.intern(s->str());
}

JsString* Context::concat(JsString* a, JsString* b) {
  if (a->length == 0) return b;
  if (b->length == 0) return a;
  JsString* r = heap.make<JsString>();
  r->length = a->length + b->length;
  if (r->length < 32) {
    r->flat = a->str();
    r->flat += b->str();
  } else {
    r->left = a;
    r->right = b;
  }
  return r;
}

JsString* Context::substring(JsString* s, uint32_t from, uint32_t to) {
  if (from >= to) return atoms.empty;
  if (from == 0 && to == s->length) return s;
  return new_string(s->str().substr(from, to - from));
}

JsString* Context::index_atom(uint32_t i) {
  if (i < 1024) {
    if (!index_atoms_[i]) index_atoms_[i] = heap.intern_utf8(std::to_string(i));
    return index_atoms_[i];
  }
  return heap.intern_utf8(std::to_string(i));
}

JsString* Context::number_to_string(double d, int radix) {
  if (radix == 10) {
    if (d >= 0 && d < 1024 && d == std::floor(d)) return index_atom(static_cast<uint32_t>(d));
    return new_string_ascii(number_to_ascii(d));
  }
  return new_string_ascii(number_to_radix(d, radix));
}

BigInt* Context::new_bigint(BigNum n) { return heap.make<BigInt>(std::move(n)); }

Symbol* Context::new_symbol(JsString* description) {
  Symbol* s = heap.make<Symbol>();
  s->description = description;
  return s;
}

// ---------------------------------------------------------------------------------------------------
// objects
// ---------------------------------------------------------------------------------------------------

Object* Context::new_object(Object* proto) {
  Object* o = heap.make<Object>(Class::Object);
  o->proto = proto;
  return o;
}

ArrayObject* Context::new_array(uint32_t length) {
  ArrayObject* a = heap.make<ArrayObject>();
  a->proto = intr.array_proto;
  a->length_ = length;
  if (length <= (1u << 16)) a->elems.assign(length, Value::Empty());
  else a->slow = false;
  return a;
}

ArrayObject* Context::new_array_from(const std::vector<Value>& v) {
  ArrayObject* a = heap.make<ArrayObject>();
  a->proto = intr.array_proto;
  a->elems = v;
  a->length_ = static_cast<uint32_t>(v.size());
  a->has_index_props = !v.empty();
  return a;
}

Function* Context::new_native(std::string_view name, NativeFn fn, int length, bool is_ctor) {
  Function* f = heap.make<Function>();
  f->proto = intr.function_proto;
  f->kind = Function::Kind::Native;
  f->native = fn;
  f->is_constructor = is_ctor;
  define_data(f, atoms.length, Value::Number(length), kConfigurable);
  define_data(f, atoms.name, Value::String(atom_utf8(name)), kConfigurable);
  return f;
}

Function* Context::new_host_function(std::string_view name, HostFn fn, int length, bool is_ctor) {
  Function* f = heap.make<Function>();
  f->is_constructor = is_ctor;
  f->proto = intr.function_proto;
  f->kind = Function::Kind::Host;
  f->host = std::move(fn);
  define_data(f, atoms.length, Value::Number(length), kConfigurable);
  define_data(f, atoms.name, Value::String(atom_utf8(name)), kConfigurable);
  return f;
}

// ---------------------------------------------------------------------------------------------------
// property helpers
// ---------------------------------------------------------------------------------------------------

bool Context::define_data(Object* o, PropKey k, Value v, uint8_t attrs) {
  if (!o->exotic_props) {
    if (Prop* p = o->props.find(k)) {
      p->value = v;
      p->attrs = attrs;
      p->getter = p->setter = nullptr;
      return true;
    }
    Prop& p = o->props.add(k);
    p.value = v;
    p.attrs = attrs;
    if (k->kind == CellKind::String && static_cast<JsString*>(k)->array_index() >= 0) o->has_index_props = true;
    return true;
  }
  bool ok = false;
  PropDesc d = PropDesc::Data(v, attrs);
  return o->define_own_property(*this, k, d, &ok);
}

bool Context::define_accessor(Object* o, PropKey k, Object* getter, Object* setter, uint8_t attrs) {
  Prop* p = o->props.find(k);
  if (!p) p = &o->props.add(k);
  p->attrs = static_cast<uint8_t>((attrs & (kEnumerable | kConfigurable)) | kAccessor);
  p->getter = getter;
  p->setter = setter;
  p->value = Value::Undefined();
  return true;
}

void Context::define_value(Object* o, std::string_view name, Value v, uint8_t attrs) { define_data(o, atom_utf8(name), v, attrs); }

Function* Context::define_method(Object* o, std::string_view name, NativeFn fn, int length, uint8_t attrs) {
  Function* f = new_native(name, fn, length);
  define_data(o, atom_utf8(name), Value::Obj(f), attrs);
  return f;
}

Function* Context::define_method(Object* o, Symbol* s, std::string_view name, NativeFn fn, int length, uint8_t attrs) {
  Function* f = new_native(name, fn, length);
  define_data(o, s, Value::Obj(f), attrs);
  return f;
}

void Context::define_getter(Object* o, std::string_view name, NativeFn fn) {
  Function* g = new_native("get " + std::string(name), fn, 0);
  define_accessor(o, atom_utf8(name), g, nullptr, kConfigurable);
}

void Context::define_getter(Object* o, Symbol* s, std::string_view name, NativeFn fn) {
  Function* g = new_native("get " + std::string(name), fn, 0);
  define_accessor(o, s, g, nullptr, kConfigurable);
}

PropKey Context::key(std::string_view s) { return atom_utf8(s); }

bool Context::has_own_property(Object* o, PropKey k, bool* out) {
  PropDesc d;
  return o->get_own_property(*this, k, &d, out);
}

bool Context::property_key_from_index(uint64_t i, PropKey* out) {
  if (i < 4294967295ull) *out = index_atom(static_cast<uint32_t>(i));
  else *out = heap.intern_utf8(std::to_string(i));
  return true;
}

bool Context::to_property_key(Value v, PropKey* out) {
  switch (v.tag) {
    case Tag::String: *out = to_atom(v.as_string()); return true;
    case Tag::Symbol: *out = v.as_symbol(); return true;
    case Tag::Number: {
      double d = v.num;
      if (d >= 0 && d < 4294967295.0 && d == std::floor(d)) *out = index_atom(static_cast<uint32_t>(d));
      else *out = to_atom(number_to_string(d));
      return true;
    }
    default: break;
  }
  Value p;
  if (!to_primitive(v, Hint::String, &p)) return false;
  if (p.is_symbol()) { *out = p.as_symbol(); return true; }
  JsString* s;
  if (!to_string(p, &s)) return false;
  *out = to_atom(s);
  return true;
}

bool Context::get(Value obj, PropKey k, Value* out) {
  if (obj.is_object()) return obj.as_object()->get(*this, k, obj, out);
  return get_v(obj, k, out);
}

bool Context::get_v(Value base, PropKey k, Value* out) {
  switch (base.tag) {
    case Tag::Object: return base.as_object()->get(*this, k, base, out);
    case Tag::String: {
      JsString* s = base.as_string();
      if (k == atoms.length) { *out = Value::Number(s->length); return true; }
      if (k->kind == CellKind::String) {
        int64_t i = static_cast<JsString*>(k)->array_index();
        if (i >= 0 && static_cast<uint64_t>(i) < s->length) {
          *out = Value::String(substring(s, static_cast<uint32_t>(i), static_cast<uint32_t>(i) + 1));
          return true;
        }
      }
      return intr.string_proto->get(*this, k, base, out);
    }
    case Tag::Number: return intr.number_proto->get(*this, k, base, out);
    case Tag::Boolean: return intr.boolean_proto->get(*this, k, base, out);
    case Tag::Symbol: return intr.symbol_proto->get(*this, k, base, out);
    case Tag::BigInt: return intr.bigint_proto->get(*this, k, base, out);
    default: {
      std::string name = k->kind == CellKind::String ? static_cast<JsString*>(k)->utf8() : "symbol";
      throw_type_error("Cannot read properties of " + std::string(base.is_null() ? "null" : "undefined") + " (reading '" + name + "')");
      return false;
    }
  }
}

bool Context::get_index(Value base, uint32_t i, Value* out) {
  if (base.is_object()) {
    Object* o = base.as_object();
    if (o->cls == Class::Array) {
      ArrayObject* a = static_cast<ArrayObject*>(o);
      if (!a->slow && i < a->elems.size() && !a->elems[i].is_empty()) { *out = a->elems[i]; return true; }
    }
  }
  return get_v(base, index_atom(i), out);
}

bool Context::put(Object* obj, PropKey k, Value v, bool throw_on_fail) {
  bool ok = false;
  if (!obj->set(*this, k, v, Value::Obj(obj), &ok)) return false;
  if (!ok && throw_on_fail) {
    std::string name = k->kind == CellKind::String ? static_cast<JsString*>(k)->utf8() : "symbol";
    throw_type_error("Cannot assign to read only property '" + name + "' of object");
    return false;
  }
  return true;
}

bool Context::put_v(Value base, PropKey k, Value v, bool strict) {
  if (base.is_object()) {
    bool ok = false;
    if (!base.as_object()->set(*this, k, v, base, &ok)) return false;
    if (!ok && strict) {
      std::string name = k->kind == CellKind::String ? static_cast<JsString*>(k)->utf8() : "symbol";
      throw_type_error("Cannot assign to read only property '" + name + "' of object");
      return false;
    }
    return true;
  }
  if (base.is_nullish()) {
    std::string name = k->kind == CellKind::String ? static_cast<JsString*>(k)->utf8() : "symbol";
    throw_type_error("Cannot set properties of " + std::string(base.is_null() ? "null" : "undefined") + " (setting '" + name + "')");
    return false;
  }
  Object* proto = base.is_string() ? intr.string_proto : base.is_number() ? intr.number_proto : base.is_bool() ? intr.boolean_proto : base.is_bigint() ? intr.bigint_proto : intr.symbol_proto;
  bool ok = false;
  if (!proto->set(*this, k, v, base, &ok)) return false;
  if (!ok && strict) {
    throw_type_error("Cannot create property on a primitive value");
    return false;
  }
  return true;
}

bool Context::create_data_property(Object* o, PropKey k, Value v, bool* ok) {
  bool r = false;
  PropDesc d = PropDesc::Data(v);
  if (!o->define_own_property(*this, k, d, &r)) return false;
  if (ok) *ok = r;
  return true;
}

bool Context::get_named(Value obj, std::string_view name, Value* out) { return get_v(obj, key(name), out); }

bool Context::set_named(Object* obj, std::string_view name, Value v) { return put(obj, key(name), v); }

bool Context::length_of(Object* o, uint64_t* out) {
  Value v;
  if (!o->get(*this, atoms.length, Value::Obj(o), &v)) return false;
  return to_length(v, out);
}

// ---------------------------------------------------------------------------------------------------
// conversions
// ---------------------------------------------------------------------------------------------------

bool Context::get_method(Value v, PropKey k, Value* out) {
  Value f;
  if (!get_v(v, k, &f)) return false;
  if (f.is_nullish()) { *out = Value::Undefined(); return true; }
  if (!f.is_callable()) {
    throw_type_error(describe_for_error(*this, f) + " is not a function");
    return false;
  }
  *out = f;
  return true;
}

bool Context::to_primitive(Value v, Hint hint, Value* out) {
  if (!v.is_object()) { *out = v; return true; }
  Value ex;
  if (!get_method(v, sym.to_primitive, &ex)) return false;
  if (!ex.is_undefined()) {
    Value h = Value::String(atom_ascii(hint == Hint::Default ? "default" : hint == Hint::Number ? "number" : "string"));
    Value r;
    if (!call(ex, v, &h, 1, &r)) return false;
    if (r.is_object()) { throw_type_error("Cannot convert object to primitive value"); return false; }
    *out = r;
    return true;
  }
  // OrdinaryToPrimitive
  PropKey first = hint == Hint::String ? static_cast<PropKey>(atoms.toString) : static_cast<PropKey>(atoms.valueOf);
  PropKey second = hint == Hint::String ? static_cast<PropKey>(atoms.valueOf) : static_cast<PropKey>(atoms.toString);
  for (PropKey name : {first, second}) {
    Value f;
    if (!get_v(v, name, &f)) return false;
    if (f.is_callable()) {
      Value r;
      if (!call(f, v, nullptr, 0, &r)) return false;
      if (!r.is_object()) { *out = r; return true; }
    }
  }
  throw_type_error("Cannot convert object to primitive value");
  return false;
}

bool Context::to_number(Value v, double* out) {
  switch (v.tag) {
    case Tag::Number: *out = v.num; return true;
    case Tag::Undefined: *out = NAN; return true;
    case Tag::Null: *out = 0; return true;
    case Tag::Boolean: *out = v.boolean ? 1 : 0; return true;
    case Tag::String: *out = string_to_number(v.as_string()->str()); return true;
    case Tag::Symbol: throw_type_error("Cannot convert a Symbol value to a number"); return false;
    case Tag::BigInt: throw_type_error("Cannot convert a BigInt value to a number"); return false;
    case Tag::Object: {
      Value p;
      if (!to_primitive(v, Hint::Number, &p)) return false;
      return to_number(p, out);
    }
    default: *out = NAN; return true;
  }
}

Value Context::throw_mixed_bigint() {
  return throw_type_error("Cannot mix BigInt and other types, use explicit conversions");
}

bool Context::to_numeric(Value v, Value* out) {
  Value p;
  if (!to_primitive(v, Hint::Number, &p)) return false;
  if (p.is_number() || p.is_bigint()) { *out = p; return true; }
  double d;
  if (!to_number(p, &d)) return false;
  *out = Value::Number(d);
  return true;
}

bool Context::to_bigint(Value v, BigInt** out) {
  Value p;
  if (!to_primitive(v, Hint::Number, &p)) return false;
  switch (p.tag) {
    case Tag::BigInt: *out = p.as_bigint(); return true;
    case Tag::Boolean: *out = new_bigint(big::from_u64(p.boolean ? 1 : 0)); return true;
    case Tag::String: {
      BigNum n;
      if (!big::string_to_bigint(p.as_string()->str(), &n)) {
        throw_syntax_error("Cannot convert " + p.as_string()->utf8() + " to a BigInt");
        return false;
      }
      *out = new_bigint(std::move(n));
      return true;
    }
    case Tag::Number: throw_type_error("Cannot convert " + number_to_string(p.num)->utf8() + " to a BigInt"); return false;
    case Tag::Symbol: throw_type_error("Cannot convert Symbol() to a BigInt"); return false;
    default: throw_type_error("Cannot convert " + std::string(p.is_null() ? "null" : "undefined") + " to a BigInt"); return false;
  }
}

bool Context::to_string(Value v, JsString** out) {
  switch (v.tag) {
    case Tag::String: *out = v.as_string(); return true;
    case Tag::Number: *out = number_to_string(v.num); return true;
    case Tag::Undefined: *out = atoms.undefined; return true;
    case Tag::Null: *out = atoms.null; return true;
    case Tag::Boolean: *out = v.boolean ? atoms.true_ : atoms.false_; return true;
    case Tag::Symbol: throw_type_error("Cannot convert a Symbol value to a string"); return false;
    case Tag::BigInt: *out = new_string_ascii(big::to_string(v.as_bigint()->n)); return true;
    case Tag::Object: {
      Value p;
      if (!to_primitive(v, Hint::String, &p)) return false;
      return to_string(p, out);
    }
    default: *out = atoms.empty; return true;
  }
}

bool Context::to_object(Value v, Object** out) {
  if (v.is_object()) { *out = v.as_object(); return true; }
  if (v.is_nullish()) { throw_type_error("Cannot convert undefined or null to object"); return false; }
  *out = make_wrapper(*this, v);
  return true;
}

bool Context::to_integer(Value v, double* out) {
  double n;
  if (!to_number(v, &n)) return false;
  if (std::isnan(n)) { *out = 0; return true; }
  if (std::isinf(n)) { *out = n; return true; }
  *out = std::trunc(n) + 0.0;   // + 0.0 turns -0 into 0
  return true;
}

bool Context::to_length(Value v, uint64_t* out) {
  double n;
  if (!to_integer(v, &n)) return false;
  if (n <= 0) { *out = 0; return true; }
  if (n > 9007199254740991.0) n = 9007199254740991.0;
  *out = static_cast<uint64_t>(n);
  return true;
}

bool Context::to_index(Value v, uint64_t* out) {
  if (v.is_undefined()) { *out = 0; return true; }
  double n;
  if (!to_integer(v, &n)) return false;
  if (n < 0 || n > 9007199254740991.0) { throw_range_error("Invalid index"); return false; }
  *out = static_cast<uint64_t>(n);
  return true;
}

double Context::to_int32_d(double d) {
  if (!std::isfinite(d)) return 0;
  d = std::trunc(d);
  double m = std::fmod(d, 4294967296.0);
  if (m < 0) m += 4294967296.0;
  if (m >= 2147483648.0) m -= 4294967296.0;
  return m;
}

double Context::to_uint32_d(double d) {
  if (!std::isfinite(d)) return 0;
  d = std::trunc(d);
  double m = std::fmod(d, 4294967296.0);
  if (m < 0) m += 4294967296.0;
  return m;
}

bool Context::to_int32(Value v, int32_t* out) {
  double n;
  if (v.is_number()) n = v.num; else if (!to_number(v, &n)) return false;
  *out = static_cast<int32_t>(to_int32_d(n));
  return true;
}

bool Context::to_uint32(Value v, uint32_t* out) {
  double n;
  if (v.is_number()) n = v.num; else if (!to_number(v, &n)) return false;
  *out = static_cast<uint32_t>(to_uint32_d(n));
  return true;
}

bool Context::to_boolean(const Value& v) {
  switch (v.tag) {
    case Tag::Boolean: return v.boolean;
    case Tag::Number: return !(v.num == 0 || std::isnan(v.num));
    case Tag::String: return v.as_string()->length != 0;
    case Tag::Object: case Tag::Symbol: return true;
    case Tag::BigInt: return !v.as_bigint()->n.is_zero();
    default: return false;
  }
}

bool Context::strict_equals(const Value& a, const Value& b) {
  if (a.tag != b.tag) return false;
  switch (a.tag) {
    case Tag::Undefined: case Tag::Null: return true;
    case Tag::Boolean: return a.boolean == b.boolean;
    case Tag::Number: return a.num == b.num;
    case Tag::String: {
      JsString* x = a.as_string();
      JsString* y = b.as_string();
      if (x == y) return true;
      if (x->length != y->length) return false;
      if (x->is_atom && y->is_atom) return false;
      return x->str() == y->str();
    }
    case Tag::Symbol: case Tag::Object: return a.cell == b.cell;
    case Tag::BigInt: return a.cell == b.cell || big::cmp(a.as_bigint()->n, b.as_bigint()->n) == 0;
    default: return false;
  }
}

bool Context::same_value_zero(const Value& a, const Value& b) {
  if (a.is_number() && b.is_number()) return a.num == b.num || (std::isnan(a.num) && std::isnan(b.num));
  return strict_equals(a, b);
}

bool Context::same_value(const Value& a, const Value& b) {
  if (a.is_number() && b.is_number()) {
    if (std::isnan(a.num) && std::isnan(b.num)) return true;
    if (a.num == 0 && b.num == 0) return std::signbit(a.num) == std::signbit(b.num);
    return a.num == b.num;
  }
  return strict_equals(a, b);
}

bool Context::loose_equals(const Value& a, const Value& b, bool* out) {
  if (a.tag == b.tag) { *out = strict_equals(a, b); return true; }
  if (a.is_nullish() && b.is_nullish()) { *out = true; return true; }
  if (a.is_nullish() || b.is_nullish()) { *out = false; return true; }
  if (a.is_number() && b.is_string()) { *out = a.num == string_to_number(b.as_string()->str()); return true; }
  if (a.is_string() && b.is_number()) { *out = string_to_number(a.as_string()->str()) == b.num; return true; }
  if (a.is_bigint() || b.is_bigint()) {
    const Value& x = a.is_bigint() ? a : b;
    const Value& y = a.is_bigint() ? b : a;
    if (y.is_number()) { *out = big::cmp_double(x.as_bigint()->n, y.num) == 0; return true; }
    if (y.is_string()) {
      BigNum n;
      *out = big::string_to_bigint(y.as_string()->str(), &n) && big::cmp(x.as_bigint()->n, n) == 0;
      return true;
    }
  }
  if (a.is_bool()) return loose_equals(Value::Number(a.boolean ? 1 : 0), b, out);
  if (b.is_bool()) return loose_equals(a, Value::Number(b.boolean ? 1 : 0), out);
  if (b.is_object() && !a.is_object()) {
    Value p;
    if (!to_primitive(b, Hint::Default, &p)) return false;
    return loose_equals(a, p, out);
  }
  if (a.is_object() && !b.is_object()) {
    Value p;
    if (!to_primitive(a, Hint::Default, &p)) return false;
    return loose_equals(p, b, out);
  }
  *out = false;
  return true;
}

const char* Context::type_of(const Value& v) {
  switch (v.tag) {
    case Tag::Undefined: return "undefined";
    case Tag::Null: return "object";
    case Tag::Boolean: return "boolean";
    case Tag::Number: return "number";
    case Tag::String: return "string";
    case Tag::Symbol: return "symbol";
    case Tag::BigInt: return "bigint";
    case Tag::Object: return v.as_object()->callable ? "function" : "object";
    default: return "undefined";
  }
}

// ---------------------------------------------------------------------------------------------------
// calls
// ---------------------------------------------------------------------------------------------------

bool Object::call_internal(Context& ctx, const Value&, const Value*, int, Value*) {
  ctx.throw_type_error("object is not a function");
  return false;
}

bool Object::construct_internal(Context& ctx, const Value*, int, const Value&, Value*) {
  ctx.throw_type_error("object is not a constructor");
  return false;
}

bool Function::call_internal(Context& ctx, const Value& this_v, const Value* argv, int argc, Value* out) {
  switch (kind) {
    case Kind::Native:
    case Kind::Host: {
      if (ctx.native_depth > ctx.max_call_depth) {
        ctx.throw_range_error("Maximum call stack size exceeded");
        return false;
      }
      CallArgs a;
      a.this_value = this_v;
      a.argv = argv;
      a.argc = argc;
      a.callee = this;
      if (!gc_safe) ++ctx.native_depth;
      Value r = kind == Kind::Native ? native(ctx, a) : host(ctx, a);
      if (!gc_safe) --ctx.native_depth;
      if (r.is_exception()) return false;
      *out = r;
      return true;
    }
    case Kind::Bound: {
      std::vector<Value> args(slots.begin() + 2, slots.end());
      args.insert(args.end(), argv, argv + argc);
      return ctx.call(slots[0], slots[1], args.data(), static_cast<int>(args.size()), out);
    }
    case Kind::Closure:
      if (is_class) {
        Value n;
        std::string nm = "constructor";
        if (get(ctx, ctx.atoms.name, Value::Obj(this), &n) && n.is_string() && n.as_string()->length) nm = n.as_string()->utf8();
        ctx.throw_type_error("Class constructor " + nm + " cannot be invoked without 'new'");
        return false;
      }
      return vm_call_closure(ctx, this, this_v, argv, argc, Value::Undefined(), out);
  }
  return false;
}

bool Function::construct_internal(Context& ctx, const Value* argv, int argc, const Value& new_target, Value* out) {
  switch (kind) {
    case Kind::Native:
    case Kind::Host: {
      if (ctx.native_depth > ctx.max_call_depth) {
        ctx.throw_range_error("Maximum call stack size exceeded");
        return false;
      }
      CallArgs a;
      a.this_value = Value::Undefined();
      a.argv = argv;
      a.argc = argc;
      a.new_target = new_target;
      a.callee = this;
      if (!gc_safe) ++ctx.native_depth;
      Value r = kind == Kind::Native ? native(ctx, a) : host(ctx, a);
      if (!gc_safe) --ctx.native_depth;
      if (r.is_exception()) return false;
      *out = r;
      return true;
    }
    case Kind::Bound: {
      std::vector<Value> args(slots.begin() + 2, slots.end());
      args.insert(args.end(), argv, argv + argc);
      Value nt = new_target;
      if (nt.is_object() && nt.as_object() == this) nt = slots[0];
      return ctx.construct(slots[0], args.data(), static_cast<int>(args.size()), nt, out);
    }
    case Kind::Closure:
      return vm_call_closure(ctx, this, Value::Undefined(), argv, argc, new_target, out);
  }
  return false;
}

bool Context::call(Value fn, Value this_v, const Value* argv, int argc, Value* out) {
  if (!fn.is_callable()) {
    throw_type_error(describe_for_error(*this, fn) + " is not a function");
    return false;
  }
  return fn.as_object()->call_internal(*this, this_v, argv, argc, out);
}

bool Context::construct(Value fn, const Value* argv, int argc, Value new_target, Value* out) {
  if (!fn.is_object() || !fn.as_object()->is_constructor) {
    throw_type_error(describe_for_error(*this, fn) + " is not a constructor");
    return false;
  }
  if (new_target.is_undefined()) new_target = fn;
  return fn.as_object()->construct_internal(*this, argv, argc, new_target, out);
}

bool Context::call_method(Value obj, PropKey name, const Value* argv, int argc, Value* out) {
  Value f;
  if (!get_v(obj, name, &f)) return false;
  return call(f, obj, argv, argc, out);
}

bool Context::ordinary_has_instance(Value c, Value v, bool* out) {
  if (!c.is_callable()) { *out = false; return true; }
  Object* co = c.as_object();
  if (co->cls == Class::Function && static_cast<Function*>(co)->kind == Function::Kind::Bound)
    return instance_of(v, static_cast<Function*>(co)->slots[0], out);
  if (!v.is_object()) { *out = false; return true; }
  Value p;
  if (!co->get(*this, atoms.prototype, c, &p)) return false;
  if (!p.is_object()) { throw_type_error("Function has non-object prototype in instanceof check"); return false; }
  Object* o = v.as_object();
  for (;;) {
    Object* next = nullptr;
    if (!o->get_prototype_of(*this, &next)) return false;
    if (!next) { *out = false; return true; }
    if (next == p.as_object()) { *out = true; return true; }
    o = next;
  }
}

bool Context::instance_of(Value v, Value target, bool* out) {
  if (!target.is_object()) { throw_type_error("Right-hand side of 'instanceof' is not an object"); return false; }
  Value h;
  if (!get_method(target, sym.has_instance, &h)) return false;
  if (!h.is_undefined()) {
    Value r;
    if (!call(h, target, &v, 1, &r)) return false;
    *out = to_boolean(r);
    return true;
  }
  if (!target.is_callable()) { throw_type_error("Right-hand side of 'instanceof' is not callable"); return false; }
  return ordinary_has_instance(target, v, out);
}

bool Context::is_array(Value v, bool* out) {
  *out = false;
  if (!v.is_object()) return true;
  Object* o = v.as_object();
  while (o->cls == Class::Proxy) {
    bool revoked;
    Object* t = proxy_target_of(o, &revoked);
    if (revoked) { throw_type_error("Cannot perform 'IsArray' on a proxy that has been revoked"); return false; }
    o = t;
  }
  if (o->cls == Class::Array) *out = true;
  return true;
}

bool Context::species_constructor(Object* o, Value default_ctor, Value* out) {
  Value c;
  if (!o->get(*this, atoms.constructor, Value::Obj(o), &c)) return false;
  if (c.is_undefined()) { *out = default_ctor; return true; }
  if (!c.is_object()) { throw_type_error("object.constructor is not an object"); return false; }
  Value s;
  if (!c.as_object()->get(*this, sym.species, c, &s)) return false;
  if (s.is_nullish()) { *out = default_ctor; return true; }
  if (s.is_object() && s.as_object()->is_constructor) { *out = s; return true; }
  throw_type_error("object.constructor[Symbol.species] is not a constructor");
  return false;
}

bool Context::prototype_from_ctor(Value new_target, Object* fallback, Object** out) {
  if (!new_target.is_object()) { *out = fallback; return true; }
  Value p;
  if (!new_target.as_object()->get(*this, atoms.prototype, new_target, &p)) return false;
  *out = p.is_object() ? p.as_object() : fallback;
  return true;
}

// ---------------------------------------------------------------------------------------------------
// iteration
// ---------------------------------------------------------------------------------------------------

Object* Context::create_iter_result(Value v, bool done) {
  Object* o = new_object();
  define_data(o, atoms.value, v);
  define_data(o, atoms.done, Value::Bool(done));
  return o;
}

bool Context::get_iterator(Value iterable, Iter* out, bool async) {
  Value method;
  if (async) {
    if (!get_method(iterable, sym.async_iterator, &method)) return false;
    if (method.is_undefined()) {
      // fall back to the sync iterator (wrapped by the caller's async-from-sync machinery)
      if (!get_method(iterable, sym.iterator, &method)) return false;
    }
  } else {
    if (!get_method(iterable, sym.iterator, &method)) return false;
  }
  if (method.is_undefined()) {
    throw_type_error(describe_for_error(*this, iterable) + " is not iterable");
    return false;
  }
  Value it;
  if (!call(method, iterable, nullptr, 0, &it)) return false;
  if (!it.is_object()) { throw_type_error("Result of the Symbol.iterator method is not an object"); return false; }
  Value next;
  if (!get(it, atoms.next, &next)) return false;
  out->iterator = it;
  out->next = next;
  out->done = false;
  return true;
}

bool Context::iterator_step(Iter& it, bool* done, Value* value) {
  Value r;
  if (!call(it.next, it.iterator, nullptr, 0, &r)) { it.done = true; return false; }
  if (!r.is_object()) { it.done = true; throw_type_error("Iterator result is not an object"); return false; }
  Value d;
  if (!get(r, atoms.done, &d)) { it.done = true; return false; }
  if (to_boolean(d)) { *done = true; it.done = true; return true; }
  *done = false;
  if (!get(r, atoms.value, value)) { it.done = true; return false; }
  return true;
}

bool Context::iterator_close(Iter& it) {
  Value ret;
  if (!get_method(it.iterator, atoms.return_, &ret)) return false;
  if (ret.is_undefined()) return true;
  Value r;
  if (!call(ret, it.iterator, nullptr, 0, &r)) return false;
  if (!r.is_object()) { throw_type_error("Iterator .return() result is not an object"); return false; }
  return true;
}

bool Context::iterate(Value iterable, const std::function<bool(Value)>& fn) {
  Iter it;
  if (!get_iterator(iterable, &it)) return false;
  Rooted r1(*this, it.iterator), r2(*this, it.next);
  for (;;) {
    bool done = false;
    Value v;
    if (!iterator_step(it, &done, &v)) return false;
    if (done) return true;
    Rooted rv(*this, v);
    if (!fn(v)) {
      if (has_exception()) {
        // an error in the callback: close the iterator but report the original error
        Value saved = take_exception();
        Value ret;
        if (get_method(it.iterator, atoms.return_, &ret) && !ret.is_undefined()) {
          Value ignored;
          (void)call(ret, it.iterator, nullptr, 0, &ignored);
        }
        clear_exception();
        set_exception(saved);
        return false;
      }
      return iterator_close(it);
    }
  }
}

bool Context::iterable_to_list(Value iterable, std::vector<Value>* out) {
  return iterate(iterable, [&](Value v) { out->push_back(v); return true; });
}

// ---------------------------------------------------------------------------------------------------
// jobs
// ---------------------------------------------------------------------------------------------------

void Context::enqueue_job_value(Value fn, std::vector<Value> args) { value_jobs_.emplace_back(fn, std::move(args)); }

void Context::enqueue_job(std::function<void()> job) {
  jobs_.push_back(std::move(job));
}

bool Context::run_jobs() {
  if (running_jobs_) return true;
  running_jobs_ = true;
  bool ok = true;
  while (!value_jobs_.empty() || !jobs_.empty()) {
    if (!value_jobs_.empty()) {
      auto job = std::move(value_jobs_.front());
      value_jobs_.pop_front();
      Rooted rf(*this, job.first);
      std::vector<Rooted*> keep;
      for (const Value& a : job.second) keep.push_back(new Rooted(*this, a));
      kept_alive.clear();
      Value ignored;
      if (!call(job.first, Value::Undefined(), job.second.data(), static_cast<int>(job.second.size()), &ignored)) {
        if (print) print("Uncaught (in job) " + describe_exception(take_exception()) + "\n");
        else clear_exception();
        ok = false;
      }
      for (Rooted* r : keep) delete r;
    } else {
      auto job = std::move(jobs_.front());
      jobs_.pop_front();
      job();
    }
    maybe_gc();
  }
  running_jobs_ = false;
  promise_report_unhandled(*this);
  return ok;
}

// ---------------------------------------------------------------------------------------------------
// garbage collection
// ---------------------------------------------------------------------------------------------------

size_t Context::push_persistent(Value v) {
  if (!free_persistents_.empty()) {
    size_t s = free_persistents_.back();
    free_persistents_.pop_back();
    persistents_[s] = v;
    return s;
  }
  persistents_.push_back(v);
  return persistents_.size() - 1;
}

void Context::release_persistent(size_t slot) {
  persistents_[slot] = Value::Undefined();
  free_persistents_.push_back(slot);
}

void Context::add_root_tracer(std::function<void(Tracer&)> t, void**) { root_tracers.push_back(std::move(t)); }

void Context::maybe_gc() {
  if (native_depth == 0 && heap.should_collect()) gc();
}

void Context::gc() {
  if (native_depth != 0) return;
  Tracer t;
  static_assert(sizeof(Intrinsics) % sizeof(void*) == 0, "Intrinsics must be all pointers");
  static_assert(sizeof(CommonAtoms) % sizeof(void*) == 0, "CommonAtoms must be all pointers");
  static_assert(sizeof(WellKnownSymbols) % sizeof(void*) == 0, "WellKnownSymbols must be all pointers");
  {
    Cell** p = reinterpret_cast<Cell**>(&intr);
    for (size_t i = 0; i < sizeof(Intrinsics) / sizeof(void*); ++i) t.mark(p[i]);
    p = reinterpret_cast<Cell**>(&atoms);
    for (size_t i = 0; i < sizeof(CommonAtoms) / sizeof(void*); ++i) t.mark(p[i]);
    p = reinterpret_cast<Cell**>(&sym);
    for (size_t i = 0; i < sizeof(WellKnownSymbols) / sizeof(void*); ++i) t.mark(p[i]);
  }
  for (JsString* a : index_atoms_) t.mark(a);
  for (auto& kv : global_lexicals) { t.mark(kv.first); t.mark(kv.second); }
  for (const Value& v : persistents_) t.mark(v);
  for (Value* v : temp_roots) t.mark(*v);
  for (const Value& v : pending_rejections) t.mark(v);
  t.mark(exception_);
  for (auto& kv : symbol_registry) t.mark(kv.second);
  for (auto& j : value_jobs_) { t.mark(j.first); for (const Value& a : j.second) t.mark(a); }
  for (auto& fn : root_tracers) fn(t);
  for (const Value& v : kept_alive) t.mark(v);
  vm_trace(*this, t);
  t.drain();
  // ephemerons: a WeakMap value lives only while its key does, which may in turn depend on other values
  for (bool again = true; again;) {
    again = false;
    for (Cell* c : std::vector<Cell*>(heap.weak_cells.begin(), heap.weak_cells.end()))
      if (c->marked && c->trace_weak(t)) again = true;
    t.drain();
  }
  for (Cell* c : std::vector<Cell*>(heap.weak_cells.begin(), heap.weak_cells.end()))
    if (c->marked) c->sweep_weak(*this);
  heap.sweep();
}

}  // namespace aios::js
