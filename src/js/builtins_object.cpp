// Object, Function, Boolean, Symbol, Reflect and the helpers every builtin shares.
#include <algorithm>
#include <cmath>

#include "builtins.hpp"

namespace aios::js {

// ---------------------------------------------------------------------------------------------------
// shared helpers
// ---------------------------------------------------------------------------------------------------

JsString* ascii(Context& ctx, const char* s) { return ctx.atom_ascii(s); }

Object* make_wrapper(Context& ctx, Value prim) {
  switch (prim.tag) {
    case Tag::Boolean: {
      PrimitiveObject* o = ctx.heap.make<PrimitiveObject>(Class::Boolean, prim);
      o->proto = ctx.intr.boolean_proto;
      return o;
    }
    case Tag::Number: {
      PrimitiveObject* o = ctx.heap.make<PrimitiveObject>(Class::Number, prim);
      o->proto = ctx.intr.number_proto;
      return o;
    }
    case Tag::String: {
      StringObject* o = ctx.heap.make<StringObject>(prim.as_string());
      o->proto = ctx.intr.string_proto;
      return o;
    }
    case Tag::Symbol: {
      PrimitiveObject* o = ctx.heap.make<PrimitiveObject>(Class::Symbol, prim);
      o->proto = ctx.intr.symbol_proto;
      return o;
    }
    case Tag::BigInt: {
      PrimitiveObject* o = ctx.heap.make<PrimitiveObject>(Class::BigInt, prim);
      o->proto = ctx.intr.bigint_proto;
      return o;
    }
    default: return nullptr;
  }
}

void install_ctor(Context& ctx, Function* ctor, Object* proto, std::string_view name) {
  ctx.define_data(ctor, ctx.atoms.prototype, Value::Obj(proto), 0);
  ctx.define_data(proto, ctx.atoms.constructor, Value::Obj(ctor), kHiddenAttrs);
  ctx.define_data(ctx.intr.global, ctx.atom_utf8(name), Value::Obj(ctor), kHiddenAttrs);
}

Function* new_ctor(Context& ctx, std::string_view name, NativeFn fn, int length, Object* proto) {
  Function* f = ctx.new_native(name, fn, length, true);
  install_ctor(ctx, f, proto, name);
  return f;
}

bool this_number_value(Context& ctx, const Value& v, const char* method, double* out) {
  if (v.is_number()) { *out = v.num; return true; }
  if (v.is_object() && v.as_object()->cls == Class::Number) {
    *out = static_cast<PrimitiveObject*>(v.as_object())->prim.num;
    return true;
  }
  ctx.throw_type_error(std::string(method) + " requires that 'this' be a Number");
  return false;
}

std::string key_to_display(PropKey k) {
  if (k->kind == CellKind::String) return static_cast<JsString*>(k)->utf8();
  Symbol* s = static_cast<Symbol*>(k);
  return "Symbol(" + (s->description ? s->description->utf8() : std::string()) + ")";
}

bool is_dense_array(Object* o) {
  if (o->cls != Class::Array) return false;
  ArrayObject* a = static_cast<ArrayObject*>(o);
  return !a->slow && a->elems.size() == a->length_;
}

bool get_idx(Context& ctx, Object* o, uint64_t i, Value* out) {
  if (o->cls == Class::Array) {
    ArrayObject* a = static_cast<ArrayObject*>(o);
    if (!a->slow && i < a->elems.size() && !a->elems[i].is_empty()) { *out = a->elems[i]; return true; }
  }
  PropKey k;
  if (!ctx.property_key_from_index(i, &k)) return false;
  return o->get(ctx, k, Value::Obj(o), out);
}

bool set_idx(Context& ctx, Object* o, uint64_t i, const Value& v) {
  if (o->cls == Class::Array) {
    ArrayObject* a = static_cast<ArrayObject*>(o);
    if (!a->slow && i < a->elems.size() && !a->elems[i].is_empty()) { a->elems[i] = v; return true; }
  }
  PropKey k;
  if (!ctx.property_key_from_index(i, &k)) return false;
  return ctx.put_v(Value::Obj(o), k, v, true);
}

bool has_idx(Context& ctx, Object* o, uint64_t i, bool* out) {
  if (o->cls == Class::Array) {
    ArrayObject* a = static_cast<ArrayObject*>(o);
    if (!a->slow && i < a->elems.size() && !a->elems[i].is_empty()) { *out = true; return true; }
  }
  PropKey k;
  if (!ctx.property_key_from_index(i, &k)) return false;
  return o->has_property(ctx, k, out);
}

bool delete_idx(Context& ctx, Object* o, uint64_t i) {
  PropKey k;
  if (!ctx.property_key_from_index(i, &k)) return false;
  bool ok = false;
  if (!o->delete_property(ctx, k, &ok)) return false;
  if (!ok) {
    ctx.throw_type_error("Cannot delete property '" + key_to_display(k) + "' of object");
    return false;
  }
  return true;
}

bool create_idx(Context& ctx, Object* o, uint64_t i, const Value& v) {
  PropKey k;
  if (!ctx.property_key_from_index(i, &k)) return false;
  bool ok = false;
  if (!ctx.create_data_property(o, k, v, &ok)) return false;
  if (!ok) {
    ctx.throw_type_error("Cannot add property " + key_to_display(k) + ", object is not extensible");
    return false;
  }
  return true;
}

bool set_length(Context& ctx, Object* o, uint64_t len) {
  return ctx.put_v(Value::Obj(o), ctx.atoms.length, Value::Number(static_cast<double>(len)), true);
}

bool create_list_from_array_like(Context& ctx, const Value& v, std::vector<Value>* out) {
  if (!v.is_object()) {
    ctx.throw_type_error("CreateListFromArrayLike called on non-object");
    return false;
  }
  Object* o = v.as_object();
  uint64_t n;
  if (!ctx.length_of(o, &n)) return false;
  if (n > 10000000) {
    ctx.throw_range_error("Too many arguments");
    return false;
  }
  out->reserve(n);
  for (uint64_t i = 0; i < n; ++i) {
    Value e;
    if (!get_idx(ctx, o, i, &e)) return false;
    out->push_back(e);
  }
  return true;
}

bool relative_index(Context& ctx, const Value& v, uint64_t len, uint64_t default_value, uint64_t* out) {
  if (v.is_undefined()) { *out = default_value; return true; }
  double d;
  if (!ctx.to_integer(v, &d)) return false;
  if (d < 0) {
    d += static_cast<double>(len);
    if (d < 0) d = 0;
  } else if (d > static_cast<double>(len)) {
    d = static_cast<double>(len);
  }
  *out = static_cast<uint64_t>(d);
  return true;
}

bool array_species_create(Context& ctx, Object* original, uint64_t length, Object** out) {
  bool is_arr;
  if (!ctx.is_array(Value::Obj(original), &is_arr)) return false;
  if (!is_arr) {
    if (length > 4294967295ull) { ctx.throw_range_error("Invalid array length"); return false; }
    *out = ctx.new_array(0);
    static_cast<ArrayObject*>(*out)->length_ = static_cast<uint32_t>(length);
    return true;
  }
  Value c;
  if (!original->get(ctx, ctx.atoms.constructor, Value::Obj(original), &c)) return false;
  if (c.is_object()) {
    Value s;
    if (!c.as_object()->get(ctx, ctx.sym.species, c, &s)) return false;
    c = s.is_null() ? Value::Undefined() : s;
  }
  if (c.is_undefined() || (c.is_object() && c.as_object() == ctx.intr.array_ctor)) {
    if (length > 4294967295ull) { ctx.throw_range_error("Invalid array length"); return false; }
    ArrayObject* a = ctx.new_array(0);
    a->length_ = static_cast<uint32_t>(length);
    *out = a;
    return true;
  }
  if (!c.is_object() || !c.as_object()->is_constructor) {
    ctx.throw_type_error("object.constructor[Symbol.species] is not a constructor");
    return false;
  }
  Value arg = Value::Number(static_cast<double>(length));
  Value r;
  if (!ctx.construct(c, &arg, 1, c, &r)) return false;
  *out = r.as_object();
  return true;
}

// ---------------------------------------------------------------------------------------------------
// property descriptors
// ---------------------------------------------------------------------------------------------------

bool to_prop_desc(Context& ctx, const Value& v, PropDesc* d) {
  if (!v.is_object()) {
    ctx.throw_type_error("Property description must be an object: " + vm_describe(ctx, v));
    return false;
  }
  Object* o = v.as_object();
  bool has;
  Value x;
  auto field = [&](JsString* name, bool* present, Value* val) {
    if (!o->has_property(ctx, name, present)) return false;
    if (*present && !o->get(ctx, name, v, val)) return false;
    return true;
  };
  bool present;
  if (!field(ctx.atoms.enumerable, &present, &x)) return false;
  if (present) { d->has_enumerable = true; d->enumerable = Context::to_boolean(x); }
  if (!field(ctx.atoms.configurable, &present, &x)) return false;
  if (present) { d->has_configurable = true; d->configurable = Context::to_boolean(x); }
  if (!field(ctx.atoms.value, &present, &x)) return false;
  if (present) { d->has_value = true; d->value = x; }
  if (!field(ctx.atoms.writable, &present, &x)) return false;
  if (present) { d->has_writable = true; d->writable = Context::to_boolean(x); }
  if (!field(ctx.atoms.get, &present, &x)) return false;
  if (present) {
    if (!x.is_undefined() && !x.is_callable()) {
      ctx.throw_type_error("Getter must be a function: " + vm_describe(ctx, x));
      return false;
    }
    d->has_get = true;
    d->getter = x.is_object() ? x.as_object() : nullptr;
  }
  if (!field(ctx.atoms.set, &present, &x)) return false;
  if (present) {
    if (!x.is_undefined() && !x.is_callable()) {
      ctx.throw_type_error("Setter must be a function: " + vm_describe(ctx, x));
      return false;
    }
    d->has_set = true;
    d->setter = x.is_object() ? x.as_object() : nullptr;
  }
  has = (d->has_get || d->has_set) && (d->has_value || d->has_writable);
  if (has) {
    ctx.throw_type_error("Invalid property descriptor. Cannot both specify accessors and a value or writable attribute");
    return false;
  }
  return true;
}

Value from_prop_desc(Context& ctx, const PropDesc& d) {
  Object* o = ctx.new_object();
  if (d.is_accessor()) {
    ctx.define_data(o, ctx.atoms.get, d.getter ? Value::Obj(d.getter) : Value::Undefined());
    ctx.define_data(o, ctx.atoms.set, d.setter ? Value::Obj(d.setter) : Value::Undefined());
  } else {
    ctx.define_data(o, ctx.atoms.value, d.value);
    ctx.define_data(o, ctx.atoms.writable, Value::Bool(d.writable));
  }
  ctx.define_data(o, ctx.atoms.enumerable, Value::Bool(d.enumerable));
  ctx.define_data(o, ctx.atoms.configurable, Value::Bool(d.configurable));
  return Value::Obj(o);
}

// ---------------------------------------------------------------------------------------------------
// Object
// ---------------------------------------------------------------------------------------------------

bool object_freeze(Context& ctx, Object* o) {
  bool ok;
  if (!o->prevent_extensions(ctx, &ok)) return false;
  if (!ok) { ctx.throw_type_error("Cannot freeze"); return false; }
  std::vector<PropKey> keys;
  if (!o->own_keys(ctx, &keys)) return false;
  for (PropKey k : keys) {
    PropDesc cur;
    bool found;
    if (!o->get_own_property(ctx, k, &cur, &found)) return false;
    if (!found) continue;
    PropDesc d;
    d.has_configurable = true;
    d.configurable = false;
    if (!cur.is_accessor()) { d.has_writable = true; d.writable = false; }
    if (!o->define_own_property(ctx, k, d, &ok)) return false;
    if (!ok) { ctx.throw_type_error("Cannot redefine property: " + key_to_display(k)); return false; }
  }
  return true;
}

static bool object_seal(Context& ctx, Object* o) {
  bool ok;
  if (!o->prevent_extensions(ctx, &ok)) return false;
  if (!ok) { ctx.throw_type_error("Cannot seal"); return false; }
  std::vector<PropKey> keys;
  if (!o->own_keys(ctx, &keys)) return false;
  for (PropKey k : keys) {
    PropDesc d;
    d.has_configurable = true;
    d.configurable = false;
    if (!o->define_own_property(ctx, k, d, &ok)) return false;
    if (!ok) { ctx.throw_type_error("Cannot redefine property: " + key_to_display(k)); return false; }
  }
  return true;
}

static bool test_integrity(Context& ctx, Object* o, bool frozen, bool* result) {
  bool ext;
  if (!o->is_extensible_obj(ctx, &ext)) return false;
  if (ext) { *result = false; return true; }
  std::vector<PropKey> keys;
  if (!o->own_keys(ctx, &keys)) return false;
  for (PropKey k : keys) {
    PropDesc d;
    bool found;
    if (!o->get_own_property(ctx, k, &d, &found)) return false;
    if (!found) continue;
    if (d.configurable) { *result = false; return true; }
    if (frozen && !d.is_accessor() && d.writable) { *result = false; return true; }
  }
  *result = true;
  return true;
}

// Enumerable own string-keyed properties (kind: 0 keys, 1 values, 2 entries).
static bool enumerable_own(Context& ctx, Object* o, int kind, std::vector<Value>* out) {
  std::vector<PropKey> keys;
  if (!o->own_keys(ctx, &keys)) return false;
  for (PropKey k : keys) {
    if (k->kind != CellKind::String) continue;
    PropDesc d;
    bool found;
    if (!o->get_own_property(ctx, k, &d, &found)) return false;
    if (!found || !d.enumerable) continue;
    Value key = Value::String(static_cast<JsString*>(k));
    if (kind == 0) { out->push_back(key); continue; }
    Value v;
    if (!o->get(ctx, k, Value::Obj(o), &v)) return false;
    if (kind == 1) out->push_back(v);
    else out->push_back(Value::Obj(ctx.new_array_from({key, v})));
  }
  return true;
}

NATIVE(object_ctor) {
  Value nt = a.new_target;
  if (!nt.is_undefined() && !(nt.is_object() && nt.as_object() == a.callee)) {
    Object* proto;
    JS_TRY(ctx.prototype_from_ctor(nt, ctx.intr.object_proto, &proto));
    return Value::Obj(ctx.new_object(proto));
  }
  if (a[0].is_nullish()) return Value::Obj(ctx.new_object());
  Object* o;
  JS_TRY(ctx.to_object(a[0], &o));
  return Value::Obj(o);
}

NATIVE(object_assign) {
  Object* target;
  JS_TRY(ctx.to_object(a[0], &target));
  for (int i = 1; i < a.argc; ++i) {
    if (a.argv[i].is_nullish()) continue;
    Object* from;
    JS_TRY(ctx.to_object(a.argv[i], &from));
    std::vector<PropKey> keys;
    JS_TRY(from->own_keys(ctx, &keys));
    for (PropKey k : keys) {
      PropDesc d;
      bool found;
      JS_TRY(from->get_own_property(ctx, k, &d, &found));
      if (!found || !d.enumerable) continue;
      Value v;
      JS_TRY(from->get(ctx, k, Value::Obj(from), &v));
      JS_TRY(ctx.put_v(Value::Obj(target), k, v, true));
    }
  }
  return Value::Obj(target);
}

static bool define_properties(Context& ctx, Object* o, const Value& props) {
  Object* p;
  if (!ctx.to_object(props, &p)) return false;
  std::vector<PropKey> keys;
  if (!p->own_keys(ctx, &keys)) return false;
  std::vector<std::pair<PropKey, PropDesc>> descs;
  for (PropKey k : keys) {
    PropDesc pd;
    bool found;
    if (!p->get_own_property(ctx, k, &pd, &found)) return false;
    if (!found || !pd.enumerable) continue;
    Value dv;
    if (!p->get(ctx, k, Value::Obj(p), &dv)) return false;
    PropDesc d;
    if (!to_prop_desc(ctx, dv, &d)) return false;
    descs.emplace_back(k, d);
  }
  for (auto& e : descs) {
    bool ok;
    if (!o->define_own_property(ctx, e.first, e.second, &ok)) return false;
    if (!ok) { ctx.throw_type_error("Cannot redefine property: " + key_to_display(e.first)); return false; }
  }
  return true;
}

NATIVE(object_create) {
  if (!a[0].is_object() && !a[0].is_null()) {
    ctx.throw_type_error("Object prototype may only be an Object or null: " + vm_describe(ctx, a[0]));
    return Value::Exception();
  }
  Object* o = ctx.new_object(a[0].is_object() ? a[0].as_object() : nullptr);
  if (!a[1].is_undefined()) JS_TRY(define_properties(ctx, o, a[1]));
  return Value::Obj(o);
}

NATIVE(object_define_property) {
  if (!a[0].is_object()) {
    ctx.throw_type_error("Object.defineProperty called on non-object");
    return Value::Exception();
  }
  PropKey k;
  JS_TRY(ctx.to_property_key(a[1], &k));
  PropDesc d;
  JS_TRY(to_prop_desc(ctx, a[2], &d));
  bool ok;
  JS_TRY(a[0].as_object()->define_own_property(ctx, k, d, &ok));
  if (!ok) {
    ctx.throw_type_error("Cannot redefine property: " + key_to_display(k));
    return Value::Exception();
  }
  return a[0];
}

NATIVE(object_define_properties) {
  if (!a[0].is_object()) {
    ctx.throw_type_error("Object.defineProperties called on non-object");
    return Value::Exception();
  }
  JS_TRY(define_properties(ctx, a[0].as_object(), a[1]));
  return a[0];
}

NATIVE(object_get_own_property_descriptor) {
  Object* o;
  JS_TRY(ctx.to_object(a[0], &o));
  PropKey k;
  JS_TRY(ctx.to_property_key(a[1], &k));
  PropDesc d;
  bool found;
  JS_TRY(o->get_own_property(ctx, k, &d, &found));
  if (!found) return Value::Undefined();
  return from_prop_desc(ctx, d);
}

NATIVE(object_get_own_property_descriptors) {
  Object* o;
  JS_TRY(ctx.to_object(a[0], &o));
  std::vector<PropKey> keys;
  JS_TRY(o->own_keys(ctx, &keys));
  Object* r = ctx.new_object();
  for (PropKey k : keys) {
    PropDesc d;
    bool found;
    JS_TRY(o->get_own_property(ctx, k, &d, &found));
    if (found) JS_TRY(ctx.create_data_property(r, k, from_prop_desc(ctx, d)));
  }
  return Value::Obj(r);
}

static Value own_names(Context& ctx, const Value& v, bool symbols) {
  Object* o;
  JS_TRY(ctx.to_object(v, &o));
  std::vector<PropKey> keys;
  JS_TRY(o->own_keys(ctx, &keys));
  std::vector<Value> out;
  for (PropKey k : keys) {
    if (symbols == (k->kind == CellKind::Symbol)) out.push_back(k->kind == CellKind::String ? Value::String(static_cast<JsString*>(k)) : Value::Sym(static_cast<Symbol*>(k)));
  }
  return Value::Obj(ctx.new_array_from(out));
}

NATIVE(object_get_own_property_names) { return own_names(ctx, a[0], false); }
NATIVE(object_get_own_property_symbols) { return own_names(ctx, a[0], true); }

NATIVE(object_get_prototype_of) {
  Object* o;
  JS_TRY(ctx.to_object(a[0], &o));
  Object* p;
  JS_TRY(o->get_prototype_of(ctx, &p));
  return p ? Value::Obj(p) : Value::Null();
}

NATIVE(object_set_prototype_of) {
  if (a[0].is_nullish()) {
    ctx.throw_type_error("Object.setPrototypeOf called on null or undefined");
    return Value::Exception();
  }
  if (!a[1].is_object() && !a[1].is_null()) {
    ctx.throw_type_error("Object prototype may only be an Object or null: " + vm_describe(ctx, a[1]));
    return Value::Exception();
  }
  if (!a[0].is_object()) return a[0];
  bool ok;
  JS_TRY(a[0].as_object()->set_prototype_of(ctx, a[1].is_object() ? a[1].as_object() : nullptr, &ok));
  if (!ok) {
    ctx.throw_type_error("Cyclic __proto__ value or object is not extensible");
    return Value::Exception();
  }
  return a[0];
}

NATIVE(object_is) { return Value::Bool(ctx.same_value(a[0], a[1])); }

NATIVE(object_keys) {
  Object* o;
  JS_TRY(ctx.to_object(a[0], &o));
  std::vector<Value> out;
  JS_TRY(enumerable_own(ctx, o, 0, &out));
  return Value::Obj(ctx.new_array_from(out));
}

NATIVE(object_values) {
  Object* o;
  JS_TRY(ctx.to_object(a[0], &o));
  std::vector<Value> out;
  JS_TRY(enumerable_own(ctx, o, 1, &out));
  return Value::Obj(ctx.new_array_from(out));
}

NATIVE(object_entries) {
  Object* o;
  JS_TRY(ctx.to_object(a[0], &o));
  std::vector<Value> out;
  JS_TRY(enumerable_own(ctx, o, 2, &out));
  return Value::Obj(ctx.new_array_from(out));
}

NATIVE(object_from_entries) {
  if (a[0].is_nullish()) {
    ctx.throw_type_error(vm_describe(ctx, a[0]) + " is not iterable");
    return Value::Exception();
  }
  Object* r = ctx.new_object();
  JS_TRY(ctx.iterate(a[0], [&](Value e) {
    if (!e.is_object()) {
      ctx.throw_type_error("Iterator value " + vm_describe(ctx, e) + " is not an entry object");
      return false;
    }
    Value k, v;
    if (!get_idx(ctx, e.as_object(), 0, &k) || !get_idx(ctx, e.as_object(), 1, &v)) return false;
    PropKey pk;
    if (!ctx.to_property_key(k, &pk)) return false;
    return ctx.create_data_property(r, pk, v);
  }));
  return Value::Obj(r);
}

NATIVE(object_freeze_fn) {
  if (a[0].is_object()) JS_TRY(object_freeze(ctx, a[0].as_object()));
  return a[0];
}

NATIVE(object_seal_fn) {
  if (a[0].is_object()) JS_TRY(object_seal(ctx, a[0].as_object()));
  return a[0];
}

NATIVE(object_prevent_extensions) {
  if (a[0].is_object()) {
    bool ok;
    JS_TRY(a[0].as_object()->prevent_extensions(ctx, &ok));
    if (!ok) {
      ctx.throw_type_error("Cannot prevent extensions");
      return Value::Exception();
    }
  }
  return a[0];
}

NATIVE(object_is_frozen) {
  if (!a[0].is_object()) return Value::Bool(true);
  bool r;
  JS_TRY(test_integrity(ctx, a[0].as_object(), true, &r));
  return Value::Bool(r);
}

NATIVE(object_is_sealed) {
  if (!a[0].is_object()) return Value::Bool(true);
  bool r;
  JS_TRY(test_integrity(ctx, a[0].as_object(), false, &r));
  return Value::Bool(r);
}

NATIVE(object_is_extensible) {
  if (!a[0].is_object()) return Value::Bool(false);
  bool r;
  JS_TRY(a[0].as_object()->is_extensible_obj(ctx, &r));
  return Value::Bool(r);
}

NATIVE(object_has_own) {
  Object* o;
  JS_TRY(ctx.to_object(a[0], &o));
  PropKey k;
  JS_TRY(ctx.to_property_key(a[1], &k));
  bool has;
  JS_TRY(ctx.has_own_property(o, k, &has));
  return Value::Bool(has);
}

NATIVE(object_group_by) {
  Object* r = ctx.new_object(nullptr);
  if (!a[1].is_callable()) {
    ctx.throw_type_error(vm_describe(ctx, a[1]) + " is not a function");
    return Value::Exception();
  }
  double idx = 0;
  JS_TRY(ctx.iterate(a[0], [&](Value v) {
    Value key;
    if (!ctx.call(a[1], Value::Undefined(), {v, Value::Number(idx++)}, &key)) return false;
    PropKey pk;
    if (!ctx.to_property_key(key, &pk)) return false;
    Value cur;
    if (!r->get(ctx, pk, Value::Obj(r), &cur)) return false;
    if (cur.is_undefined()) {
      cur = Value::Obj(ctx.new_array(0));
      if (!ctx.create_data_property(r, pk, cur)) return false;
    }
    ArrayObject* arr = static_cast<ArrayObject*>(cur.as_object());
    arr->elems.push_back(v);
    ++arr->length_;
    return true;
  }));
  return Value::Obj(r);
}

// ---- Object.prototype ----

NATIVE(proto_has_own_property) {
  PropKey k;
  JS_TRY(ctx.to_property_key(a[0], &k));
  Object* o;
  JS_TRY(ctx.to_object(a.this_value, &o));
  bool has;
  JS_TRY(ctx.has_own_property(o, k, &has));
  return Value::Bool(has);
}

NATIVE(proto_is_prototype_of) {
  if (!a[0].is_object()) return Value::Bool(false);
  Object* o;
  JS_TRY(ctx.to_object(a.this_value, &o));
  Object* v = a[0].as_object();
  for (;;) {
    Object* p;
    JS_TRY(v->get_prototype_of(ctx, &p));
    if (!p) return Value::Bool(false);
    if (p == o) return Value::Bool(true);
    v = p;
  }
}

NATIVE(proto_property_is_enumerable) {
  PropKey k;
  JS_TRY(ctx.to_property_key(a[0], &k));
  Object* o;
  JS_TRY(ctx.to_object(a.this_value, &o));
  PropDesc d;
  bool found;
  JS_TRY(o->get_own_property(ctx, k, &d, &found));
  return Value::Bool(found && d.enumerable);
}

bool ordinary_to_string_tag(Context& ctx, const Value& v, JsString** out) {
  if (v.is_undefined()) { *out = ctx.atom_ascii("[object Undefined]"); return true; }
  if (v.is_null()) { *out = ctx.atom_ascii("[object Null]"); return true; }
  Object* o;
  if (!ctx.to_object(v, &o)) return false;
  const char* builtin = "Object";
  bool is_arr;
  if (!ctx.is_array(Value::Obj(o), &is_arr)) return false;
  if (is_arr) builtin = "Array";
  else if (o->callable) builtin = "Function";
  else switch (o->cls) {
    case Class::Error: builtin = "Error"; break;
    case Class::Boolean: builtin = "Boolean"; break;
    case Class::Number: builtin = "Number"; break;
    case Class::String: builtin = "String"; break;
    case Class::Date: builtin = "Date"; break;
    case Class::RegExp: builtin = "RegExp"; break;
    case Class::Arguments: builtin = "Arguments"; break;
    default: break;
  }
  Value tag;
  if (!o->get(ctx, ctx.sym.to_string_tag, Value::Obj(o), &tag)) return false;
  std::string t = tag.is_string() ? tag.as_string()->utf8() : builtin;
  *out = ctx.atom_utf8("[object " + t + "]");
  return true;
}

NATIVE(proto_to_string) {
  JsString* s;
  JS_TRY(ordinary_to_string_tag(ctx, a.this_value, &s));
  return Value::String(s);
}

NATIVE(proto_to_locale_string) {
  Value f;
  JS_TRY(ctx.get_v(a.this_value, ctx.atoms.toString, &f));
  Value r;
  JS_TRY(ctx.call(f, a.this_value, nullptr, 0, &r));
  return r;
}

NATIVE(proto_value_of) {
  Object* o;
  JS_TRY(ctx.to_object(a.this_value, &o));
  return Value::Obj(o);
}

NATIVE(proto_get_proto) {
  Object* o;
  JS_TRY(ctx.to_object(a.this_value, &o));
  Object* p;
  JS_TRY(o->get_prototype_of(ctx, &p));
  return p ? Value::Obj(p) : Value::Null();
}

NATIVE(proto_set_proto) {
  if (a.this_value.is_nullish()) {
    ctx.throw_type_error("Object.prototype.__proto__ called on null or undefined");
    return Value::Exception();
  }
  if (!a[0].is_object() && !a[0].is_null()) return Value::Undefined();
  if (!a.this_value.is_object()) return Value::Undefined();
  bool ok;
  JS_TRY(a.this_value.as_object()->set_prototype_of(ctx, a[0].is_object() ? a[0].as_object() : nullptr, &ok));
  if (!ok) {
    ctx.throw_type_error("Cyclic __proto__ value");
    return Value::Exception();
  }
  return Value::Undefined();
}

NATIVE(proto_define_getter) {
  Object* o;
  JS_TRY(ctx.to_object(a.this_value, &o));
  if (!a[1].is_callable()) {
    ctx.throw_type_error("Object.prototype.__defineGetter__: Expecting function");
    return Value::Exception();
  }
  PropKey k;
  JS_TRY(ctx.to_property_key(a[0], &k));
  PropDesc d;
  d.has_get = true;
  d.getter = a[1].as_object();
  d.has_enumerable = d.enumerable = true;
  d.has_configurable = d.configurable = true;
  bool ok;
  JS_TRY(o->define_own_property(ctx, k, d, &ok));
  if (!ok) { ctx.throw_type_error("Cannot redefine property: " + key_to_display(k)); return Value::Exception(); }
  return Value::Undefined();
}

NATIVE(proto_define_setter) {
  Object* o;
  JS_TRY(ctx.to_object(a.this_value, &o));
  if (!a[1].is_callable()) {
    ctx.throw_type_error("Object.prototype.__defineSetter__: Expecting function");
    return Value::Exception();
  }
  PropKey k;
  JS_TRY(ctx.to_property_key(a[0], &k));
  PropDesc d;
  d.has_set = true;
  d.setter = a[1].as_object();
  d.has_enumerable = d.enumerable = true;
  d.has_configurable = d.configurable = true;
  bool ok;
  JS_TRY(o->define_own_property(ctx, k, d, &ok));
  if (!ok) { ctx.throw_type_error("Cannot redefine property: " + key_to_display(k)); return Value::Exception(); }
  return Value::Undefined();
}

static Value lookup_accessor(Context& ctx, CallArgs& a, bool getter) {
  Object* o;
  JS_TRY(ctx.to_object(a.this_value, &o));
  PropKey k;
  JS_TRY(ctx.to_property_key(a[0], &k));
  while (o) {
    PropDesc d;
    bool found;
    JS_TRY(o->get_own_property(ctx, k, &d, &found));
    if (found) {
      if (!d.is_accessor()) return Value::Undefined();
      Object* f = getter ? d.getter : d.setter;
      return f ? Value::Obj(f) : Value::Undefined();
    }
    Object* p;
    JS_TRY(o->get_prototype_of(ctx, &p));
    o = p;
  }
  return Value::Undefined();
}

NATIVE(proto_lookup_getter) { return lookup_accessor(ctx, a, true); }
NATIVE(proto_lookup_setter) { return lookup_accessor(ctx, a, false); }

void setup_object(Context& ctx) {
  Object* proto = ctx.intr.object_proto;
  Function* ctor = new_ctor(ctx, "Object", object_ctor, 1, proto);
  ctx.define_method(ctor, "assign", object_assign, 2);
  ctx.define_method(ctor, "create", object_create, 2);
  ctx.define_method(ctor, "defineProperty", object_define_property, 3);
  ctx.define_method(ctor, "defineProperties", object_define_properties, 2);
  ctx.define_method(ctor, "entries", object_entries, 1);
  ctx.define_method(ctor, "freeze", object_freeze_fn, 1);
  ctx.define_method(ctor, "fromEntries", object_from_entries, 1);
  ctx.define_method(ctor, "getOwnPropertyDescriptor", object_get_own_property_descriptor, 2);
  ctx.define_method(ctor, "getOwnPropertyDescriptors", object_get_own_property_descriptors, 1);
  ctx.define_method(ctor, "getOwnPropertyNames", object_get_own_property_names, 1);
  ctx.define_method(ctor, "getOwnPropertySymbols", object_get_own_property_symbols, 1);
  ctx.define_method(ctor, "getPrototypeOf", object_get_prototype_of, 1);
  ctx.define_method(ctor, "groupBy", object_group_by, 2);
  ctx.define_method(ctor, "hasOwn", object_has_own, 2);
  ctx.define_method(ctor, "is", object_is, 2);
  ctx.define_method(ctor, "isExtensible", object_is_extensible, 1);
  ctx.define_method(ctor, "isFrozen", object_is_frozen, 1);
  ctx.define_method(ctor, "isSealed", object_is_sealed, 1);
  ctx.define_method(ctor, "keys", object_keys, 1);
  ctx.define_method(ctor, "preventExtensions", object_prevent_extensions, 1);
  ctx.define_method(ctor, "seal", object_seal_fn, 1);
  ctx.define_method(ctor, "setPrototypeOf", object_set_prototype_of, 2);
  ctx.define_method(ctor, "values", object_values, 1);

  ctx.define_method(proto, "hasOwnProperty", proto_has_own_property, 1);
  ctx.define_method(proto, "isPrototypeOf", proto_is_prototype_of, 1);
  ctx.define_method(proto, "propertyIsEnumerable", proto_property_is_enumerable, 1);
  ctx.define_method(proto, "toLocaleString", proto_to_locale_string, 0);
  ctx.define_method(proto, "toString", proto_to_string, 0);
  ctx.define_method(proto, "valueOf", proto_value_of, 0);
  ctx.define_method(proto, "__defineGetter__", proto_define_getter, 2);
  ctx.define_method(proto, "__defineSetter__", proto_define_setter, 2);
  ctx.define_method(proto, "__lookupGetter__", proto_lookup_getter, 1);
  ctx.define_method(proto, "__lookupSetter__", proto_lookup_setter, 1);
  Function* getter = ctx.new_native("get __proto__", proto_get_proto, 0);
  Function* setter = ctx.new_native("set __proto__", proto_set_proto, 1);
  ctx.define_accessor(proto, ctx.atoms.proto__, getter, setter, kConfigurable);
}

// ---------------------------------------------------------------------------------------------------
// Function
// ---------------------------------------------------------------------------------------------------

static Value function_from_args(Context& ctx, CallArgs& a, bool is_async, bool is_generator) {
  std::u16string params, body;
  for (int i = 0; i + 1 < a.argc; ++i) {
    JsString* s;
    JS_TRY(ctx.to_string(a.argv[i], &s));
    if (i) params += u",";
    params += s->str();
  }
  if (a.argc > 0) {
    JsString* s;
    JS_TRY(ctx.to_string(a.argv[a.argc - 1], &s));
    body = s->str();
  }
  Value r;
  JS_TRY(vm_function_from_source(ctx, params, body, is_async, is_generator, &r));
  return r;
}

NATIVE(function_ctor) { return function_from_args(ctx, a, false, false); }
NATIVE(generator_function_ctor) { return function_from_args(ctx, a, false, true); }
NATIVE(async_function_ctor) { return function_from_args(ctx, a, true, false); }
NATIVE(async_generator_function_ctor) { return function_from_args(ctx, a, true, true); }

NATIVE(function_call) {
  if (!a.this_value.is_callable()) {
    ctx.throw_type_error("Function.prototype.call called on non-function");
    return Value::Exception();
  }
  Value r;
  JS_TRY(ctx.call(a.this_value, a[0], a.argc > 1 ? a.argv + 1 : nullptr, a.argc > 1 ? a.argc - 1 : 0, &r));
  return r;
}

NATIVE(function_apply) {
  if (!a.this_value.is_callable()) {
    ctx.throw_type_error("Function.prototype.apply was called on " + vm_describe(ctx, a.this_value) + ", which is not a function");
    return Value::Exception();
  }
  std::vector<Value> args;
  if (!a[1].is_nullish()) JS_TRY(create_list_from_array_like(ctx, a[1], &args));
  Value r;
  JS_TRY(ctx.call(a.this_value, a[0], args.data(), static_cast<int>(args.size()), &r));
  return r;
}

NATIVE(function_bind) {
  if (!a.this_value.is_callable()) {
    ctx.throw_type_error("Bind must be called on a function");
    return Value::Exception();
  }
  Object* target = a.this_value.as_object();
  Function* b = ctx.heap.make<Function>();
  b->kind = Function::Kind::Bound;
  Object* tp;
  JS_TRY(target->get_prototype_of(ctx, &tp));
  b->proto = tp;
  b->is_constructor = target->is_constructor;
  b->slots.push_back(a.this_value);
  b->slots.push_back(a[0]);
  for (int i = 1; i < a.argc; ++i) b->slots.push_back(a.argv[i]);
  double len = 0;
  bool has;
  JS_TRY(ctx.has_own_property(target, ctx.atoms.length, &has));
  if (has) {
    Value l;
    JS_TRY(target->get(ctx, ctx.atoms.length, a.this_value, &l));
    if (l.is_number()) {
      if (std::isinf(l.num)) len = l.num > 0 ? l.num : 0;
      else {
        len = std::trunc(l.num) - std::max(0, a.argc - 1);
        if (len < 0 || std::isnan(len)) len = 0;
      }
    }
  }
  ctx.define_data(b, ctx.atoms.length, Value::Number(len), kConfigurable);
  Value n;
  JS_TRY(target->get(ctx, ctx.atoms.name, a.this_value, &n));
  std::u16string name = n.is_string() ? n.as_string()->str() : std::u16string();
  ctx.define_data(b, ctx.atoms.name, StrV(ctx, u"bound " + name), kConfigurable);
  return Value::Obj(b);
}

NATIVE(function_to_string) {
  if (!a.this_value.is_callable()) {
    ctx.throw_type_error("Function.prototype.toString requires that 'this' be a Function");
    return Value::Exception();
  }
  Object* o = a.this_value.as_object();
  if (o->cls == Class::Function) {
    Function* f = static_cast<Function*>(o);
    if (f->kind == Function::Kind::Closure && f->code && f->code->source && f->code->src_end > f->code->src_start && f->code->src_end <= f->code->source->size()) {
      return StrV(ctx, f->code->source->substr(f->code->src_start, f->code->src_end - f->code->src_start));
    }
  }
  Value n;
  std::string name;
  if (o->get(ctx, ctx.atoms.name, a.this_value, &n) && n.is_string()) name = n.as_string()->utf8();
  else ctx.clear_exception();
  return Str(ctx, "function " + name + "() { [native code] }");
}

NATIVE(function_has_instance) {
  bool r;
  JS_TRY(ctx.ordinary_has_instance(a.this_value, a[0], &r));
  return Value::Bool(r);
}

NATIVE(throw_type_error_fn) {
  (void)a;
  ctx.throw_type_error("'caller', 'callee', and 'arguments' properties may not be accessed on strict mode functions or the arguments objects for calls to them");
  return Value::Exception();
}

void setup_function(Context& ctx) {
  Object* proto = ctx.intr.function_proto;
  ctx.define_data(proto, ctx.atoms.length, Value::Number(0), kConfigurable);
  ctx.define_data(proto, ctx.atoms.name, Value::String(ctx.atoms.empty), kConfigurable);
  Function* ctor = new_ctor(ctx, "Function", function_ctor, 1, proto);
  (void)ctor;
  ctx.define_method(proto, "apply", function_apply, 2);
  ctx.define_method(proto, "bind", function_bind, 1);
  ctx.define_method(proto, "call", function_call, 1);
  ctx.define_method(proto, "toString", function_to_string, 0);
  ctx.define_method(proto, ctx.sym.has_instance, "[Symbol.hasInstance]", function_has_instance, 1, 0);
  Function* thrower = ctx.new_native("", throw_type_error_fn, 0);
  ctx.define_data(thrower, ctx.atoms.length, Value::Number(0), 0);
  thrower->extensible = false;
  ctx.intr.throw_type_error = thrower;
  // %GeneratorFunction%, %AsyncFunction%, %AsyncGeneratorFunction%: constructors that are not globals
  struct Kind { const char* name; NativeFn fn; Object** proto_slot; };
  Kind kinds[] = {{"GeneratorFunction", generator_function_ctor, &ctx.intr.generator_function_proto},
                  {"AsyncFunction", async_function_ctor, &ctx.intr.async_function_proto},
                  {"AsyncGeneratorFunction", async_generator_function_ctor, &ctx.intr.async_generator_function_proto}};
  for (Kind& k : kinds) {
    Object* p = ctx.new_object(proto);
    *k.proto_slot = p;
    Function* c = ctx.new_native(k.name, k.fn, 1, true);
    c->proto = ctor;
    ctx.define_data(c, ctx.atoms.prototype, Value::Obj(p), 0);
    ctx.define_data(p, ctx.atoms.constructor, Value::Obj(c), kConfigurable);
    ctx.define_data(p, ctx.sym.to_string_tag, Str(ctx, k.name), kConfigurable);
  }
}

// ---------------------------------------------------------------------------------------------------
// Boolean
// ---------------------------------------------------------------------------------------------------

static bool this_boolean(Context& ctx, const Value& v, bool* out) {
  if (v.is_bool()) { *out = v.boolean; return true; }
  if (v.is_object() && v.as_object()->cls == Class::Boolean) { *out = static_cast<PrimitiveObject*>(v.as_object())->prim.boolean; return true; }
  ctx.throw_type_error("Boolean.prototype method requires that 'this' be a Boolean");
  return false;
}

NATIVE(boolean_ctor) {
  bool b = Context::to_boolean(a[0]);
  if (!a.is_construct()) return Value::Bool(b);
  Object* proto;
  JS_TRY(ctx.prototype_from_ctor(a.new_target, ctx.intr.boolean_proto, &proto));
  PrimitiveObject* o = ctx.heap.make<PrimitiveObject>(Class::Boolean, Value::Bool(b));
  o->proto = proto;
  return Value::Obj(o);
}

NATIVE(boolean_to_string) {
  bool b;
  JS_TRY(this_boolean(ctx, a.this_value, &b));
  return Value::String(b ? ctx.atoms.true_ : ctx.atoms.false_);
}

NATIVE(boolean_value_of) {
  bool b;
  JS_TRY(this_boolean(ctx, a.this_value, &b));
  return Value::Bool(b);
}

void setup_boolean(Context& ctx) {
  PrimitiveObject* proto = ctx.heap.make<PrimitiveObject>(Class::Boolean, Value::Bool(false));
  proto->proto = ctx.intr.object_proto;
  ctx.intr.boolean_proto = proto;
  new_ctor(ctx, "Boolean", boolean_ctor, 1, proto);
  ctx.define_method(proto, "toString", boolean_to_string, 0);
  ctx.define_method(proto, "valueOf", boolean_value_of, 0);
}

// ---------------------------------------------------------------------------------------------------
// Symbol
// ---------------------------------------------------------------------------------------------------

static bool this_symbol(Context& ctx, const Value& v, Symbol** out) {
  if (v.is_symbol()) { *out = v.as_symbol(); return true; }
  if (v.is_object() && v.as_object()->cls == Class::Symbol) { *out = static_cast<PrimitiveObject*>(v.as_object())->prim.as_symbol(); return true; }
  ctx.throw_type_error("Symbol.prototype method requires that 'this' be a Symbol");
  return false;
}

NATIVE(symbol_ctor) {
  if (a.is_construct()) {
    ctx.throw_type_error("Symbol is not a constructor");
    return Value::Exception();
  }
  JsString* d = nullptr;
  if (!a[0].is_undefined()) JS_TRY(ctx.to_string(a[0], &d));
  return Value::Sym(ctx.new_symbol(d));
}

NATIVE(symbol_for) {
  JsString* k;
  JS_TRY(ctx.to_string(a[0], &k));
  auto it = ctx.symbol_registry.find(k->str());
  if (it != ctx.symbol_registry.end()) return Value::Sym(it->second);
  Symbol* s = ctx.new_symbol(k);
  s->registered = true;
  ctx.symbol_registry[k->str()] = s;
  return Value::Sym(s);
}

NATIVE(symbol_key_for) {
  if (!a[0].is_symbol()) {
    ctx.throw_type_error(vm_describe(ctx, a[0]) + " is not a symbol");
    return Value::Exception();
  }
  Symbol* s = a[0].as_symbol();
  if (!s->registered) return Value::Undefined();
  return Value::String(s->description);
}

NATIVE(symbol_to_string) {
  Symbol* s;
  JS_TRY(this_symbol(ctx, a.this_value, &s));
  return StrV(ctx, u"Symbol(" + (s->description ? s->description->str() : std::u16string()) + u")");
}

NATIVE(symbol_value_of) {
  Symbol* s;
  JS_TRY(this_symbol(ctx, a.this_value, &s));
  return Value::Sym(s);
}

NATIVE(symbol_description) {
  Symbol* s;
  JS_TRY(this_symbol(ctx, a.this_value, &s));
  return s->description ? Value::String(s->description) : Value::Undefined();
}

void setup_symbol(Context& ctx) {
  Object* proto = ctx.new_object();
  ctx.intr.symbol_proto = proto;
  Function* ctor = new_ctor(ctx, "Symbol", symbol_ctor, 0, proto);
  ctx.define_method(ctor, "for", symbol_for, 1);
  ctx.define_method(ctor, "keyFor", symbol_key_for, 1);
  struct { const char* name; Symbol* s; } wk[] = {
      {"iterator", ctx.sym.iterator}, {"asyncIterator", ctx.sym.async_iterator}, {"hasInstance", ctx.sym.has_instance},
      {"toPrimitive", ctx.sym.to_primitive}, {"toStringTag", ctx.sym.to_string_tag}, {"species", ctx.sym.species},
      {"isConcatSpreadable", ctx.sym.is_concat_spreadable}, {"unscopables", ctx.sym.unscopables}, {"match", ctx.sym.match},
      {"matchAll", ctx.sym.match_all}, {"replace", ctx.sym.replace}, {"search", ctx.sym.search}, {"split", ctx.sym.split}};
  for (auto& w : wk) ctx.define_data(ctor, ctx.atom_ascii(w.name), Value::Sym(w.s), 0);
  ctx.define_method(proto, "toString", symbol_to_string, 0);
  ctx.define_method(proto, "valueOf", symbol_value_of, 0);
  ctx.define_getter(proto, "description", symbol_description);
  Function* tp = ctx.new_native("[Symbol.toPrimitive]", symbol_value_of, 1);
  ctx.define_data(proto, ctx.sym.to_primitive, Value::Obj(tp), kConfigurable);
  ctx.define_data(proto, ctx.sym.to_string_tag, Str(ctx, "Symbol"), kConfigurable);
}

// ---------------------------------------------------------------------------------------------------
// Reflect
// ---------------------------------------------------------------------------------------------------

static bool reflect_target(Context& ctx, const Value& v, const char* fn) {
  if (v.is_object()) return true;
  ctx.throw_type_error(std::string("Reflect.") + fn + " called on non-object");
  return false;
}

NATIVE(reflect_apply) {
  if (!a[0].is_callable()) {
    ctx.throw_type_error(vm_describe(ctx, a[0]) + " is not a function");
    return Value::Exception();
  }
  std::vector<Value> args;
  JS_TRY(create_list_from_array_like(ctx, a[2], &args));
  Value r;
  JS_TRY(ctx.call(a[0], a[1], args.data(), static_cast<int>(args.size()), &r));
  return r;
}

NATIVE(reflect_construct) {
  if (!a[0].is_object() || !a[0].as_object()->is_constructor) {
    ctx.throw_type_error(vm_describe(ctx, a[0]) + " is not a constructor");
    return Value::Exception();
  }
  Value nt = a.argc > 2 ? a[2] : a[0];
  if (!nt.is_object() || !nt.as_object()->is_constructor) {
    ctx.throw_type_error(vm_describe(ctx, nt) + " is not a constructor");
    return Value::Exception();
  }
  std::vector<Value> args;
  JS_TRY(create_list_from_array_like(ctx, a[1], &args));
  Value r;
  JS_TRY(ctx.construct(a[0], args.data(), static_cast<int>(args.size()), nt, &r));
  return r;
}

NATIVE(reflect_define_property) {
  if (!reflect_target(ctx, a[0], "defineProperty")) return Value::Exception();
  PropKey k;
  JS_TRY(ctx.to_property_key(a[1], &k));
  PropDesc d;
  JS_TRY(to_prop_desc(ctx, a[2], &d));
  bool ok;
  JS_TRY(a[0].as_object()->define_own_property(ctx, k, d, &ok));
  return Value::Bool(ok);
}

NATIVE(reflect_delete_property) {
  if (!reflect_target(ctx, a[0], "deleteProperty")) return Value::Exception();
  PropKey k;
  JS_TRY(ctx.to_property_key(a[1], &k));
  bool ok;
  JS_TRY(a[0].as_object()->delete_property(ctx, k, &ok));
  return Value::Bool(ok);
}

NATIVE(reflect_get) {
  if (!reflect_target(ctx, a[0], "get")) return Value::Exception();
  PropKey k;
  JS_TRY(ctx.to_property_key(a[1], &k));
  Value r;
  JS_TRY(a[0].as_object()->get(ctx, k, a.argc > 2 ? a[2] : a[0], &r));
  return r;
}

NATIVE(reflect_set) {
  if (!reflect_target(ctx, a[0], "set")) return Value::Exception();
  PropKey k;
  JS_TRY(ctx.to_property_key(a[1], &k));
  bool ok;
  JS_TRY(a[0].as_object()->set(ctx, k, a[2], a.argc > 3 ? a[3] : a[0], &ok));
  return Value::Bool(ok);
}

NATIVE(reflect_get_own_property_descriptor) {
  if (!reflect_target(ctx, a[0], "getOwnPropertyDescriptor")) return Value::Exception();
  PropKey k;
  JS_TRY(ctx.to_property_key(a[1], &k));
  PropDesc d;
  bool found;
  JS_TRY(a[0].as_object()->get_own_property(ctx, k, &d, &found));
  return found ? from_prop_desc(ctx, d) : Value::Undefined();
}

NATIVE(reflect_get_prototype_of) {
  if (!reflect_target(ctx, a[0], "getPrototypeOf")) return Value::Exception();
  Object* p;
  JS_TRY(a[0].as_object()->get_prototype_of(ctx, &p));
  return p ? Value::Obj(p) : Value::Null();
}

NATIVE(reflect_set_prototype_of) {
  if (!reflect_target(ctx, a[0], "setPrototypeOf")) return Value::Exception();
  if (!a[1].is_object() && !a[1].is_null()) {
    ctx.throw_type_error("Object prototype may only be an Object or null: " + vm_describe(ctx, a[1]));
    return Value::Exception();
  }
  bool ok;
  JS_TRY(a[0].as_object()->set_prototype_of(ctx, a[1].is_object() ? a[1].as_object() : nullptr, &ok));
  return Value::Bool(ok);
}

NATIVE(reflect_has) {
  if (!reflect_target(ctx, a[0], "has")) return Value::Exception();
  PropKey k;
  JS_TRY(ctx.to_property_key(a[1], &k));
  bool has;
  JS_TRY(a[0].as_object()->has_property(ctx, k, &has));
  return Value::Bool(has);
}

NATIVE(reflect_is_extensible) {
  if (!reflect_target(ctx, a[0], "isExtensible")) return Value::Exception();
  bool r;
  JS_TRY(a[0].as_object()->is_extensible_obj(ctx, &r));
  return Value::Bool(r);
}

NATIVE(reflect_prevent_extensions) {
  if (!reflect_target(ctx, a[0], "preventExtensions")) return Value::Exception();
  bool r;
  JS_TRY(a[0].as_object()->prevent_extensions(ctx, &r));
  return Value::Bool(r);
}

NATIVE(reflect_own_keys) {
  if (!reflect_target(ctx, a[0], "ownKeys")) return Value::Exception();
  std::vector<PropKey> keys;
  JS_TRY(a[0].as_object()->own_keys(ctx, &keys));
  std::vector<Value> out;
  for (PropKey k : keys) out.push_back(k->kind == CellKind::String ? Value::String(static_cast<JsString*>(k)) : Value::Sym(static_cast<Symbol*>(k)));
  return Value::Obj(ctx.new_array_from(out));
}

void setup_reflect(Context& ctx) {
  Object* r = ctx.new_object();
  r->cls = Class::Reflect;
  ctx.define_data(ctx.intr.global, ctx.atom_ascii("Reflect"), Value::Obj(r), kHiddenAttrs);
  ctx.define_method(r, "apply", reflect_apply, 3);
  ctx.define_method(r, "construct", reflect_construct, 2);
  ctx.define_method(r, "defineProperty", reflect_define_property, 3);
  ctx.define_method(r, "deleteProperty", reflect_delete_property, 2);
  ctx.define_method(r, "get", reflect_get, 2);
  ctx.define_method(r, "getOwnPropertyDescriptor", reflect_get_own_property_descriptor, 2);
  ctx.define_method(r, "getPrototypeOf", reflect_get_prototype_of, 1);
  ctx.define_method(r, "has", reflect_has, 2);
  ctx.define_method(r, "isExtensible", reflect_is_extensible, 1);
  ctx.define_method(r, "ownKeys", reflect_own_keys, 1);
  ctx.define_method(r, "preventExtensions", reflect_prevent_extensions, 1);
  ctx.define_method(r, "set", reflect_set, 3);
  ctx.define_method(r, "setPrototypeOf", reflect_set_prototype_of, 2);
  ctx.define_data(r, ctx.sym.to_string_tag, Str(ctx, "Reflect"), kConfigurable);
}

}  // namespace aios::js
