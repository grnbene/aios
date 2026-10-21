// VM support: operators, closures, classes, private names, destructuring helpers, for-in and global bindings.
#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "bigint.hpp"
#include "vm.hpp"

namespace aios::js {

// ---------------------------------------------------------------------------------------------------
// messages
// ---------------------------------------------------------------------------------------------------

std::string vm_describe(Context& ctx, const Value& v) {
  switch (v.tag) {
    case Tag::Undefined: return "undefined";
    case Tag::Null: return "null";
    case Tag::Boolean: return v.boolean ? "true" : "false";
    case Tag::Number: return ctx.number_to_string(v.num)->utf8();
    case Tag::String: return "\"" + v.as_string()->utf8() + "\"";
    case Tag::Symbol: {
      Symbol* s = v.as_symbol();
      return "Symbol(" + (s->description ? s->description->utf8() : std::string()) + ")";
    }
    case Tag::BigInt: return big::to_string(v.as_bigint()->n) + "n";
    case Tag::Object: {
      Object* o = v.as_object();
      if (o->callable) {
        Value n;
        if (o->get(ctx, ctx.atoms.name, v, &n) && n.is_string() && n.as_string()->length) return n.as_string()->utf8();
        ctx.clear_exception();
        return "function";
      }
      if (o->cls == Class::Array) return "[object Array]";
      return "#<Object>";
    }
    default: return "value";
  }
}

// ---------------------------------------------------------------------------------------------------
// operators
// ---------------------------------------------------------------------------------------------------

static double js_pow(double x, double y) {
  if (std::isnan(y)) return NAN;
  if (y == 0) return 1;
  if ((x == 1 || x == -1) && std::isinf(y)) return NAN;
  return std::pow(x, y);
}

bool vm_add(Context& ctx, const Value& a, const Value& b, Value* out) {
  if (a.is_number() && b.is_number()) { *out = Value::Number(a.num + b.num); return true; }
  if (a.is_string() && b.is_string()) { *out = Value::String(ctx.concat(a.as_string(), b.as_string())); return true; }
  Value pa, pb;
  if (!ctx.to_primitive(a, Hint::Default, &pa)) return false;
  if (!ctx.to_primitive(b, Hint::Default, &pb)) return false;
  if (pa.is_string() || pb.is_string()) {
    JsString *sa, *sb;
    if (!ctx.to_string(pa, &sa)) return false;
    if (!ctx.to_string(pb, &sb)) return false;
    *out = Value::String(ctx.concat(sa, sb));
    return true;
  }
  Value na, nb;
  if (!ctx.to_numeric(pa, &na) || !ctx.to_numeric(pb, &nb)) return false;
  if (na.is_bigint() || nb.is_bigint()) {
    if (!(na.is_bigint() && nb.is_bigint())) { ctx.throw_mixed_bigint(); return false; }
    *out = Value::Big(ctx.new_bigint(big::add(na.as_bigint()->n, nb.as_bigint()->n)));
    return true;
  }
  *out = Value::Number(na.num + nb.num);
  return true;
}

// `a OP b` on two BigInts (the operators the spec defines for them).
static bool vm_bigint_arith(Context& ctx, OpCode op, const BigNum& x, const BigNum& y, Value* out) {
  BigNum r;
  big::Err e = big::Err::None;
  switch (op) {
    case OpCode::Sub: r = big::sub(x, y); break;
    case OpCode::Mul: r = big::mul(x, y); break;
    case OpCode::Div: e = big::divmod(x, y, &r, nullptr); break;
    case OpCode::Mod: e = big::divmod(x, y, nullptr, &r); break;
    case OpCode::Exp: e = big::pow(x, y, &r); break;
    case OpCode::BitAnd: r = big::bit_and(x, y); break;
    case OpCode::BitOr: r = big::bit_or(x, y); break;
    case OpCode::BitXor: r = big::bit_xor(x, y); break;
    case OpCode::Shl: e = big::shift_left(x, y, &r); break;
    case OpCode::Shr: e = big::shift_right(x, y, &r); break;
    case OpCode::UShr:
      ctx.throw_type_error("BigInts have no unsigned right shift, use >> instead");
      return false;
    default: return false;
  }
  switch (e) {
    case big::Err::None: break;
    case big::Err::DivideByZero: ctx.throw_range_error("Division by zero"); return false;
    case big::Err::NegativeExponent: ctx.throw_range_error("Exponent must be non-negative"); return false;
    case big::Err::TooBig: ctx.throw_range_error("Maximum BigInt size exceeded"); return false;
  }
  *out = Value::Big(ctx.new_bigint(std::move(r)));
  return true;
}

bool vm_arith(Context& ctx, OpCode op, const Value& a, const Value& b, Value* out) {
  Value na, nb;
  if (a.is_number()) na = a;
  else if (!ctx.to_numeric(a, &na)) return false;
  if (b.is_number()) nb = b;
  else if (!ctx.to_numeric(b, &nb)) return false;
  if (na.is_bigint() || nb.is_bigint()) {
    if (!(na.is_bigint() && nb.is_bigint())) { ctx.throw_mixed_bigint(); return false; }
    return vm_bigint_arith(ctx, op, na.as_bigint()->n, nb.as_bigint()->n, out);
  }
  const double x = na.num, y = nb.num;
  switch (op) {
    case OpCode::Sub: *out = Value::Number(x - y); return true;
    case OpCode::Mul: *out = Value::Number(x * y); return true;
    case OpCode::Div: *out = Value::Number(x / y); return true;
    case OpCode::Mod:
      if (std::isinf(y) && std::isfinite(x)) { *out = Value::Number(x); return true; }
      *out = Value::Number(std::fmod(x, y));
      return true;
    case OpCode::Exp: *out = Value::Number(js_pow(x, y)); return true;
    case OpCode::BitAnd: *out = Value::Number(static_cast<double>(static_cast<int32_t>(Context::to_int32_d(x)) & static_cast<int32_t>(Context::to_int32_d(y)))); return true;
    case OpCode::BitOr: *out = Value::Number(static_cast<double>(static_cast<int32_t>(Context::to_int32_d(x)) | static_cast<int32_t>(Context::to_int32_d(y)))); return true;
    case OpCode::BitXor: *out = Value::Number(static_cast<double>(static_cast<int32_t>(Context::to_int32_d(x)) ^ static_cast<int32_t>(Context::to_int32_d(y)))); return true;
    case OpCode::Shl: {
      uint32_t s = static_cast<uint32_t>(Context::to_uint32_d(y)) & 31;
      *out = Value::Number(static_cast<double>(static_cast<int32_t>(static_cast<uint32_t>(static_cast<int32_t>(Context::to_int32_d(x))) << s)));
      return true;
    }
    case OpCode::Shr: {
      uint32_t s = static_cast<uint32_t>(Context::to_uint32_d(y)) & 31;
      *out = Value::Number(static_cast<double>(static_cast<int32_t>(Context::to_int32_d(x)) >> s));
      return true;
    }
    case OpCode::UShr: {
      uint32_t s = static_cast<uint32_t>(Context::to_uint32_d(y)) & 31;
      *out = Value::Number(static_cast<double>(static_cast<uint32_t>(Context::to_uint32_d(x)) >> s));
      return true;
    }
    default: return false;
  }
}

// Abstract relational comparison: 1 true, 0 false, 2 undefined (a NaN took part).
static bool less_than(Context& ctx, const Value& a, const Value& b, bool left_first, int* res) {
  Value pa, pb;
  if (left_first) {
    if (!ctx.to_primitive(a, Hint::Number, &pa)) return false;
    if (!ctx.to_primitive(b, Hint::Number, &pb)) return false;
  } else {
    if (!ctx.to_primitive(b, Hint::Number, &pb)) return false;
    if (!ctx.to_primitive(a, Hint::Number, &pa)) return false;
  }
  if (pa.is_string() && pb.is_string()) {
    *res = pa.as_string()->str() < pb.as_string()->str() ? 1 : 0;
    return true;
  }
  if (pa.is_bigint() && pb.is_string()) {
    BigNum n;
    if (!big::string_to_bigint(pb.as_string()->str(), &n)) { *res = 2; return true; }
    *res = big::cmp(pa.as_bigint()->n, n) < 0 ? 1 : 0;
    return true;
  }
  if (pa.is_string() && pb.is_bigint()) {
    BigNum n;
    if (!big::string_to_bigint(pa.as_string()->str(), &n)) { *res = 2; return true; }
    *res = big::cmp(n, pb.as_bigint()->n) < 0 ? 1 : 0;
    return true;
  }
  Value na, nb;
  if (!ctx.to_numeric(pa, &na) || !ctx.to_numeric(pb, &nb)) return false;
  if (na.is_bigint() && nb.is_bigint()) { *res = big::cmp(na.as_bigint()->n, nb.as_bigint()->n) < 0 ? 1 : 0; return true; }
  if (na.is_bigint() || nb.is_bigint()) {
    const bool left_big = na.is_bigint();
    const int c = big::cmp_double((left_big ? na : nb).as_bigint()->n, (left_big ? nb : na).num);
    if (c == 2) { *res = 2; return true; }
    *res = (left_big ? c < 0 : c > 0) ? 1 : 0;
    return true;
  }
  const double x = na.num, y = nb.num;
  if (std::isnan(x) || std::isnan(y)) { *res = 2; return true; }
  *res = x < y ? 1 : 0;
  return true;
}

bool vm_compare(Context& ctx, OpCode op, const Value& a, const Value& b, bool* out) {
  if (a.is_number() && b.is_number()) {
    switch (op) {
      case OpCode::Lt: *out = a.num < b.num; return true;
      case OpCode::Gt: *out = a.num > b.num; return true;
      case OpCode::Le: *out = a.num <= b.num; return true;
      default: *out = a.num >= b.num; return true;
    }
  }
  int r;
  switch (op) {
    case OpCode::Lt: if (!less_than(ctx, a, b, true, &r)) return false; *out = r == 1; return true;
    case OpCode::Gt: if (!less_than(ctx, b, a, false, &r)) return false; *out = r == 1; return true;
    case OpCode::Le: if (!less_than(ctx, b, a, false, &r)) return false; *out = r == 0; return true;
    default: if (!less_than(ctx, a, b, true, &r)) return false; *out = r == 0; return true;
  }
}

// ---------------------------------------------------------------------------------------------------
// closures and classes
// ---------------------------------------------------------------------------------------------------

static void define_fn_name_and_length(Context& ctx, Function* fn, FunctionCode* fc) {
  ctx.define_data(fn, ctx.atoms.length, Value::Number(fc->length), kConfigurable);
  ctx.define_data(fn, ctx.atoms.name, Value::String(fc->name ? fc->name : ctx.atoms.empty), kConfigurable);
}

Function* vm_make_closure(Context& ctx, Frame& f, FunctionCode* fc) {
  Function* fn = ctx.heap.make<Function>();
  fn->kind = Function::Kind::Closure;
  fn->code = fc;
  fn->env = f.env;
  if (fc->is_async && fc->is_generator) fn->proto = ctx.intr.async_generator_function_proto;
  else if (fc->is_generator) fn->proto = ctx.intr.generator_function_proto;
  else if (fc->is_async) fn->proto = ctx.intr.async_function_proto;
  else fn->proto = ctx.intr.function_proto;
  if (fc->is_arrow) {
    fn->this_val = f.this_v;
    fn->new_target_lexical = f.new_target;
    fn->home_object = f.fn ? f.fn->home_object : nullptr;
    fn->fields_owner = f.fn ? (f.fn->is_class ? f.fn : f.fn->fields_owner) : nullptr;
  }
  define_fn_name_and_length(ctx, fn, fc);
  if (fc->is_generator) {
    Object* p = ctx.new_object(fc->is_async ? ctx.intr.async_generator_proto : ctx.intr.generator_proto);
    ctx.define_data(fn, ctx.atoms.prototype, Value::Obj(p), kWritable);
  } else if (!fc->is_arrow && !fc->is_method && !fc->is_async) {
    fn->is_constructor = true;
    Object* p = ctx.new_object();
    ctx.define_data(p, ctx.atoms.constructor, Value::Obj(fn), kHiddenAttrs);
    ctx.define_data(fn, ctx.atoms.prototype, Value::Obj(p), kWritable);
  }
  return fn;
}

Object* vm_make_arguments(Context& ctx, Frame& f) {
  Object* a = ctx.new_object(ctx.intr.object_proto);
  a->cls = Class::Arguments;
  const uint32_t n = f.argc;
  const uint32_t np = f.code->nparams;
  for (uint32_t i = 0; i < n; ++i) {
    Value v = i < np ? f.base[i] : f.extra[i - np];
    ctx.define_data(a, ctx.index_atom(i), v);
  }
  if (n) a->has_index_props = true;
  ctx.define_data(a, ctx.atoms.length, Value::Number(n), kHiddenAttrs);
  if (f.code->strict || !f.code->has_simple_params) {
    ctx.define_accessor(a, ctx.atoms.callee, ctx.intr.throw_type_error, ctx.intr.throw_type_error, 0);
  } else {
    ctx.define_data(a, ctx.atoms.callee, Value::Obj(f.fn), kHiddenAttrs);
  }
  Value values;
  if (ctx.get(ctx.intr.array_proto, ctx.atoms.values, &values)) ctx.define_data(a, ctx.sym.iterator, values, kHiddenAttrs);
  else ctx.clear_exception();
  return a;
}

bool vm_create_class(Context& ctx, Frame& f, FunctionCode* fc, bool has_super, Value super_v, Function** ctor_out, Object** proto_out) {
  Object* proto_parent = ctx.intr.object_proto;
  Object* ctor_parent = ctx.intr.function_proto;
  if (has_super) {
    if (super_v.is_null()) {
      proto_parent = nullptr;
    } else {
      if (!super_v.is_object() || !super_v.as_object()->is_constructor) {
        ctx.throw_type_error("Class extends value " + vm_describe(ctx, super_v) + " is not a constructor or null");
        return false;
      }
      Value pp;
      if (!ctx.get(super_v, ctx.atoms.prototype, &pp)) return false;
      if (pp.is_object()) proto_parent = pp.as_object();
      else if (pp.is_null()) proto_parent = nullptr;
      else {
        ctx.throw_type_error("Class extends value does not have valid prototype property " + vm_describe(ctx, pp));
        return false;
      }
      ctor_parent = super_v.as_object();
    }
  }
  Object* proto = ctx.new_object(proto_parent);
  Function* ctor = ctx.heap.make<Function>();
  ctor->kind = Function::Kind::Closure;
  ctor->code = fc;
  ctor->env = f.env;
  ctor->proto = ctor_parent;
  ctor->is_class = true;
  ctor->is_derived = has_super;
  ctor->is_constructor = true;
  ctor->home_object = proto;
  define_fn_name_and_length(ctx, ctor, fc);
  ctx.define_data(ctor, ctx.atoms.prototype, Value::Obj(proto), 0);
  ctx.define_data(proto, ctx.atoms.constructor, Value::Obj(ctor), kHiddenAttrs);
  *ctor_out = ctor;
  *proto_out = proto;
  return true;
}

bool vm_construct_this(Context& ctx, Function* fn, const Value& new_target, Value* this_out) {
  if (fn->is_derived) {
    *this_out = Value::Empty();
    return true;
  }
  Object* proto;
  if (!ctx.prototype_from_ctor(new_target, ctx.intr.object_proto, &proto)) return false;
  Object* obj = ctx.new_object(proto);
  *this_out = Value::Obj(obj);
  if (fn->is_class && !vm_init_instance(ctx, fn, obj)) return false;
  return true;
}

bool vm_init_instance(Context& ctx, Function* ctor, Object* obj) {
  if (!ctor->fields) return true;
  std::vector<Value>& e = ctor->fields->elems;
  const size_t n = e.size() / 3;
  for (size_t i = 0; i < n; ++i) {
    const int kind = static_cast<int>(e[3 * i + 2].num);
    if (kind < 2) continue;
    if (!vm_private_define(ctx, Value::Obj(obj), e[3 * i], e[3 * i + 1], kind == 2 ? 1 : kind == 3 ? 2 : 3)) return false;
  }
  for (size_t i = 0; i < n; ++i) {
    const int kind = static_cast<int>(e[3 * i + 2].num);
    if (kind >= 2) continue;
    Value v;
    const Value init = e[3 * i + 1];
    if (init.is_callable()) {
      if (!ctx.call(init, Value::Obj(obj), nullptr, 0, &v)) return false;
    }
    if (kind == 1) {
      if (!vm_private_define(ctx, Value::Obj(obj), e[3 * i], v, 0)) return false;
    } else {
      PropKey key;
      if (!ctx.to_property_key(e[3 * i], &key)) return false;
      bool ok = true;
      if (!ctx.create_data_property(obj, key, v, &ok)) return false;
      if (!ok) {
        ctx.throw_type_error("Cannot define property " + vm_describe(ctx, e[3 * i]) + ", object is not extensible");
        return false;
      }
    }
  }
  return true;
}

// ---------------------------------------------------------------------------------------------------
// private names
// ---------------------------------------------------------------------------------------------------

static std::string private_name(const Value& name) {
  Symbol* s = name.as_symbol();
  return s->description ? s->description->utf8() : "#?";
}

bool vm_private_get(Context& ctx, const Value& obj, const Value& name, Value* out) {
  Prop* p = obj.is_object() ? obj.as_object()->props.find(name.cell) : nullptr;
  if (!p) {
    ctx.throw_type_error("Cannot read private member " + private_name(name) + " from an object whose class did not declare it");
    return false;
  }
  if (p->is_accessor()) {
    if (!p->getter) {
      ctx.throw_type_error("'" + private_name(name) + "' was defined without a getter");
      return false;
    }
    return ctx.call(Value::Obj(p->getter), obj, nullptr, 0, out);
  }
  *out = p->value;
  return true;
}

bool vm_private_set(Context& ctx, const Value& obj, const Value& name, const Value& v) {
  Prop* p = obj.is_object() ? obj.as_object()->props.find(name.cell) : nullptr;
  if (!p) {
    ctx.throw_type_error("Cannot write private member " + private_name(name) + " to an object whose class did not declare it");
    return false;
  }
  if (p->is_accessor()) {
    if (!p->setter) {
      ctx.throw_type_error("'" + private_name(name) + "' was defined without a setter");
      return false;
    }
    Value r;
    return ctx.call(Value::Obj(p->setter), obj, {v}, &r);
  }
  if (!(p->attrs & kWritable)) {
    ctx.throw_type_error("Private method '" + private_name(name) + "' is not writable");
    return false;
  }
  p->value = v;
  return true;
}

// kind: 0 field, 1 method, 2 getter, 3 setter
bool vm_private_define(Context& ctx, const Value& obj, const Value& name, const Value& v, int kind) {
  if (!obj.is_object()) {
    ctx.throw_type_error("Cannot define private member on a non-object");
    return false;
  }
  Object* o = obj.as_object();
  Prop* p = o->props.find(name.cell);
  if (kind >= 2) {
    if (p && !p->is_accessor()) {
      ctx.throw_type_error("Cannot initialize " + private_name(name) + " twice on the same object");
      return false;
    }
    if (!p) {
      p = &o->props.add(name.cell);
      p->attrs = kAccessor;
    }
    if (kind == 2) p->getter = v.is_object() ? v.as_object() : nullptr;
    else p->setter = v.is_object() ? v.as_object() : nullptr;
    return true;
  }
  if (p) {
    ctx.throw_type_error("Cannot initialize " + private_name(name) + " twice on the same object");
    return false;
  }
  p = &o->props.add(name.cell);
  p->value = v;
  p->attrs = kind == 0 ? kWritable : 0;
  return true;
}

// ---------------------------------------------------------------------------------------------------
// objects
// ---------------------------------------------------------------------------------------------------

bool vm_copy_data_properties(Context& ctx, Object* target, const Value& src, const std::vector<PropKey>* excluded) {
  if (src.is_nullish()) return true;
  Object* from;
  if (!ctx.to_object(src, &from)) return false;
  std::vector<PropKey> keys;
  if (!from->own_keys(ctx, &keys)) return false;
  for (PropKey k : keys) {
    if (excluded && std::find(excluded->begin(), excluded->end(), k) != excluded->end()) continue;
    PropDesc d;
    bool found = false;
    if (!from->get_own_property(ctx, k, &d, &found)) return false;
    if (!found || !d.enumerable) continue;
    Value v;
    if (!from->get(ctx, k, Value::Obj(from), &v)) return false;
    if (!ctx.create_data_property(target, k, v)) return false;
  }
  return true;
}

bool vm_set_function_name(Context& ctx, Object* fn, const Value& key, int kind) {
  if (!fn->callable) return true;
  PropDesc cur;
  bool found = false;
  if (!fn->get_own_property(ctx, ctx.atoms.name, &cur, &found)) return false;
  if (kind == 0 && found && cur.has_value && cur.value.is_string() && cur.value.as_string()->length > 0) return true;
  std::u16string name;
  if (key.is_symbol()) {
    Symbol* s = key.as_symbol();
    if (s->description) name = u"[" + s->description->str() + u"]";
  } else if (key.is_string()) {
    name = key.as_string()->str();
  } else {
    JsString* s;
    if (!ctx.to_string(key, &s)) return false;
    name = s->str();
  }
  if (kind == 1) name = u"get " + name;
  else if (kind == 2) name = u"set " + name;
  ctx.define_data(fn, ctx.atoms.name, Value::String(ctx.new_string(std::move(name))), kConfigurable);
  return true;
}

bool vm_define_accessor_half(Context& ctx, Object* o, PropKey key, Object* fn, bool getter, bool enumerable) {
  PropDesc d;
  if (getter) { d.getter = fn; d.has_get = true; }
  else { d.setter = fn; d.has_set = true; }
  d.has_enumerable = true;
  d.enumerable = enumerable;
  d.has_configurable = true;
  d.configurable = true;
  bool ok = false;
  if (!o->define_own_property(ctx, key, d, &ok)) return false;
  if (!ok) {
    ctx.throw_type_error("Cannot redefine property");
    return false;
  }
  return true;
}

bool vm_get_super(Context& ctx, Frame& f, const Value& this_v, PropKey key, Value* out) {
  Object* home = f.fn ? f.fn->home_object : nullptr;
  if (!home) {
    ctx.throw_syntax_error("'super' keyword unexpected here");
    return false;
  }
  Object* p;
  if (!home->get_prototype_of(ctx, &p)) return false;
  if (!p) {
    ctx.throw_type_error("Cannot read properties of null");
    return false;
  }
  return p->get(ctx, key, this_v, out);
}

bool vm_set_super(Context& ctx, Frame& f, const Value& this_v, PropKey key, const Value& v, bool strict) {
  Object* home = f.fn ? f.fn->home_object : nullptr;
  if (!home) {
    ctx.throw_syntax_error("'super' keyword unexpected here");
    return false;
  }
  Object* p;
  if (!home->get_prototype_of(ctx, &p)) return false;
  if (!p) {
    ctx.throw_type_error("Cannot set properties of null");
    return false;
  }
  bool ok = true;
  if (!p->set(ctx, key, v, this_v, &ok)) return false;
  if (!ok && strict) {
    ctx.throw_type_error("Cannot assign to read only property of object");
    return false;
  }
  return true;
}

Object* vm_template_object(Context& ctx, FunctionCode* fc, uint32_t idx) {
  FunctionCode::TemplateSite& site = fc->templates[idx];
  if (site.object) return site.object;
  std::vector<Value> cooked, raw;
  for (size_t i = 0; i < site.cooked.size(); ++i) {
    cooked.push_back(site.ok[i] ? Value::String(ctx.new_string(site.cooked[i])) : Value::Undefined());
    raw.push_back(Value::String(ctx.new_string(site.raw[i])));
  }
  ArrayObject* c = ctx.new_array_from(cooked);
  ArrayObject* r = ctx.new_array_from(raw);
  object_freeze(ctx, r);
  ctx.define_data(c, ctx.atoms.raw, Value::Obj(r), 0);
  object_freeze(ctx, c);
  site.object = c;
  return c;
}

// ---------------------------------------------------------------------------------------------------
// for-in
// ---------------------------------------------------------------------------------------------------

namespace {
struct ForInState final : Object {
  ForInState() : Object(Class::Iterator) {}
  Object* target = nullptr;
  std::vector<PropKey> keys;
  size_t next = 0;
  void trace(Tracer& t) override {
    Object::trace(t);
    t.mark(target);
    for (PropKey k : keys) t.mark(k);
  }
};
}  // namespace

Object* vm_for_in_start(Context& ctx, const Value& v) {
  ForInState* st = ctx.heap.make<ForInState>();
  st->proto = nullptr;
  if (v.is_nullish()) return st;
  Object* obj;
  if (!ctx.to_object(v, &obj)) return nullptr;
  st->target = obj;
  std::unordered_set<PropKey> seen;
  for (Object* o = obj; o;) {
    std::vector<PropKey> keys;
    if (!o->own_keys(ctx, &keys)) return nullptr;
    for (PropKey k : keys) {
      if (k->kind != CellKind::String) continue;
      if (!seen.insert(k).second) continue;
      PropDesc d;
      bool found = false;
      if (!o->get_own_property(ctx, k, &d, &found)) return nullptr;
      if (found && d.enumerable) st->keys.push_back(k);
    }
    Object* p;
    if (!o->get_prototype_of(ctx, &p)) return nullptr;
    o = p;
  }
  return st;
}

bool vm_for_in_next(Context& ctx, Object* state, Value* key, bool* done) {
  ForInState* st = static_cast<ForInState*>(state);
  while (st->next < st->keys.size()) {
    PropKey k = st->keys[st->next++];
    bool has = false;
    if (!st->target->has_property(ctx, k, &has)) return false;
    if (!has) continue;
    *key = Value::String(static_cast<JsString*>(k));
    *done = false;
    return true;
  }
  *done = true;
  return true;
}

// ---------------------------------------------------------------------------------------------------
// global bindings
// ---------------------------------------------------------------------------------------------------

static void throw_redeclared(Context& ctx, JsString* name) {
  ctx.throw_syntax_error("Identifier '" + name->utf8() + "' has already been declared");
}

bool vm_has_global(Context& ctx, JsString* name, Value* value, bool* present) {
  auto it = ctx.global_lexicals.find(name);
  if (it != ctx.global_lexicals.end()) {
    if (it->second.is_empty()) {
      ctx.throw_reference_error("Cannot access '" + name->utf8() + "' before initialization");
      return false;
    }
    *value = it->second;
    *present = true;
    return true;
  }
  Object* g = ctx.intr.global;
  if (!g->exotic_props && !g->exotic_get) {
    Prop* p = g->props.find(name);
    if (p && !p->is_accessor()) {
      *value = p->value;
      *present = true;
      return true;
    }
  }
  bool has = false;
  if (!g->has_property(ctx, name, &has)) return false;
  *present = has;
  if (!has) return true;
  return g->get(ctx, name, Value::Obj(g), value);
}

bool vm_get_global(Context& ctx, JsString* name, Value* out) {
  bool present = false;
  if (!vm_has_global(ctx, name, out, &present)) return false;
  if (!present) {
    ctx.throw_reference_error(name->utf8() + " is not defined");
    return false;
  }
  return true;
}

bool vm_set_global(Context& ctx, JsString* name, const Value& v, bool strict) {
  auto it = ctx.global_lexicals.find(name);
  if (it != ctx.global_lexicals.end()) {
    if (it->second.is_empty()) {
      ctx.throw_reference_error("Cannot access '" + name->utf8() + "' before initialization");
      return false;
    }
    auto c = ctx.global_lexical_const.find(name);
    if (c != ctx.global_lexical_const.end() && c->second) {
      ctx.throw_type_error("Assignment to constant variable.");
      return false;
    }
    it->second = v;
    return true;
  }
  Object* g = ctx.intr.global;
  if (strict) {
    bool has = false;
    if (!g->has_property(ctx, name, &has)) return false;
    if (!has) {
      ctx.throw_reference_error(name->utf8() + " is not defined");
      return false;
    }
  }
  return ctx.put(g, name, v, strict);
}

bool vm_declare_global_var(Context& ctx, JsString* name) {
  if (ctx.global_lexicals.count(name)) { throw_redeclared(ctx, name); return false; }
  Object* g = ctx.intr.global;
  bool has = false;
  if (!ctx.has_own_property(g, name, &has)) return false;
  if (has) return true;
  bool ok = false;
  if (!g->define_own_property(ctx, name, PropDesc::Data(Value::Undefined(), kWritable | kEnumerable), &ok)) return false;
  return true;
}

bool vm_declare_global_func(Context& ctx, JsString* name, const Value& fn) {
  if (ctx.global_lexicals.count(name)) { throw_redeclared(ctx, name); return false; }
  Object* g = ctx.intr.global;
  PropDesc cur;
  bool found = false;
  if (!g->get_own_property(ctx, name, &cur, &found)) return false;
  bool ok = false;
  if (!found || cur.configurable) {
    if (!g->define_own_property(ctx, name, PropDesc::Data(fn, kWritable | kEnumerable), &ok)) return false;
  } else if (cur.is_data() && cur.writable && cur.enumerable) {
    ok = true;
  } else {
    ctx.throw_type_error("Cannot redefine global function '" + name->utf8() + "'");
    return false;
  }
  return ctx.put(g, name, fn, true);
}

bool vm_declare_global_lex(Context& ctx, JsString* name, bool is_const) {
  if (ctx.global_lexicals.count(name)) { throw_redeclared(ctx, name); return false; }
  Object* g = ctx.intr.global;
  PropDesc cur;
  bool found = false;
  if (!g->get_own_property(ctx, name, &cur, &found)) return false;
  if (found && !cur.configurable) { throw_redeclared(ctx, name); return false; }
  ctx.global_lexicals[name] = Value::Empty();
  ctx.global_lexical_const[name] = is_const;
  return true;
}

}  // namespace aios::js
