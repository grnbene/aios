// ArrayBuffer, the typed arrays, DataView, and TextEncoder / TextDecoder.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "builtins.hpp"
#include "typedarray.hpp"
#include "internal_num.hpp"
#include "unicode.hpp"

namespace aios::js {

namespace {

constexpr int kKinds = 11;
const char* const kKindNames[kKinds] = {"Int8Array", "Uint8Array", "Uint8ClampedArray", "Int16Array", "Uint16Array", "Int32Array", "Uint32Array", "Float32Array", "Float64Array", "BigInt64Array", "BigUint64Array"};
const int kKindSizes[kKinds] = {1, 1, 1, 2, 2, 4, 4, 4, 8, 8, 8};
constexpr int kFirstBigKind = 9;

// ---- element access ----

double read_element(const uint8_t* p, int kind) {
  switch (kind) {
    case 0: return static_cast<int8_t>(*p);
    case 1: case 2: return *p;
    case 3: { int16_t v; std::memcpy(&v, p, 2); return v; }
    case 4: { uint16_t v; std::memcpy(&v, p, 2); return v; }
    case 5: { int32_t v; std::memcpy(&v, p, 4); return v; }
    case 6: { uint32_t v; std::memcpy(&v, p, 4); return v; }
    case 7: { float v; std::memcpy(&v, p, 4); return v; }
    default: { double v; std::memcpy(&v, p, 8); return v; }
  }
}

// The BigInt kinds: 8 bytes holding a signed (9) or unsigned (10) 64-bit integer.
BigNum read_big_element(const uint8_t* p, int kind) {
  uint64_t u;
  std::memcpy(&u, p, 8);
  return kind == 9 ? big::from_i64(static_cast<int64_t>(u)) : big::from_u64(u);
}

void write_big_element(uint8_t* p, const BigNum& v) {
  const uint64_t u = big::to_u64_wrap(v);   // ToBigInt64 and ToBigUint64 agree on the bit pattern
  std::memcpy(p, &u, 8);
}

void write_element(uint8_t* p, int kind, double v) {
  switch (kind) {
    case 0: case 1: *p = static_cast<uint8_t>(static_cast<int32_t>(Context::to_int32_d(v))); break;
    case 2: {
      double c = std::isnan(v) ? 0 : std::min(255.0, std::max(0.0, v));
      *p = static_cast<uint8_t>(std::nearbyint(c));
      break;
    }
    case 3: case 4: { uint16_t x = static_cast<uint16_t>(static_cast<int32_t>(Context::to_int32_d(v))); std::memcpy(p, &x, 2); break; }
    case 5: case 6: { uint32_t x = static_cast<uint32_t>(static_cast<int32_t>(Context::to_int32_d(v))); std::memcpy(p, &x, 4); break; }
    case 7: { float x = static_cast<float>(v); std::memcpy(p, &x, 4); break; }
    default: std::memcpy(p, &v, 8); break;
  }
}

// ---- canonical numeric index strings ----

bool canonical_numeric(PropKey k, double* out) {
  if (k->kind != CellKind::String) return false;
  JsString* s = static_cast<JsString*>(k);
  int64_t ai = s->array_index();
  if (ai >= 0) { *out = static_cast<double>(ai); return true; }
  const std::u16string& u = s->str();
  if (u.empty()) return false;
  const char16_t c = u[0];
  if (!((c >= u'0' && c <= u'9') || c == u'-' || c == u'I' || c == u'N')) return false;
  if (u == u"-0") { *out = -0.0; return true; }
  double d = Context::string_to_number(u);
  // the round trip must reproduce the string
  std::string back = number_to_ascii(d);
  std::u16string b16(back.begin(), back.end());
  if (b16 != u) return false;
  *out = d;
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------
// TypedArrayObject
// ---------------------------------------------------------------------------------------------------

size_t TypedArrayObject::length() const {
  if (!buffer || buffer->detached) return 0;
  const size_t buf_len = buffer->data.size();
  if (length_tracking) return byte_offset > buf_len ? 0 : (buf_len - byte_offset) / static_cast<size_t>(kKindSizes[kind]);
  if (byte_offset + fixed_length * static_cast<size_t>(kKindSizes[kind]) > buf_len) return 0;
  return fixed_length;
}

bool TypedArrayObject::out_of_bounds() const {
  if (!buffer || buffer->detached) return true;
  const size_t buf_len = buffer->data.size();
  if (length_tracking) return byte_offset > buf_len;
  return byte_offset + fixed_length * static_cast<size_t>(kKindSizes[kind]) > buf_len;
}

int TypedArrayObject::element_size() const { return kKindSizes[kind]; }

bool TypedArrayObject::get_element(Context& ctx, size_t i, Value* out) const {
  if (i >= length()) return false;
  const uint8_t* p = buffer->data.data() + byte_offset + i * static_cast<size_t>(kKindSizes[kind]);
  *out = is_bigint_kind() ? Value::Big(ctx.new_bigint(read_big_element(p, kind))) : Value::Number(read_element(p, kind));
  return true;
}

void TypedArrayObject::set_element(size_t i, const Value& v) {
  if (i >= length()) return;
  uint8_t* p = buffer->data.data() + byte_offset + i * static_cast<size_t>(kKindSizes[kind]);
  if (is_bigint_kind()) write_big_element(p, v.as_bigint()->n);
  else write_element(p, kind, v.num);
}

bool coerce_element(Context& ctx, const TypedArrayObject* t, const Value& v, Value* out) {
  if (t->is_bigint_kind()) {
    BigInt* b;
    if (v.is_bigint()) { *out = v; return true; }
    if (!ctx.to_bigint(v, &b)) return false;
    *out = Value::Big(b);
    return true;
  }
  if (v.is_number()) { *out = v; return true; }
  double d;
  if (!ctx.to_number(v, &d)) return false;
  *out = Value::Number(d);
  return true;
}

void TypedArrayObject::trace(Tracer& t) {
  Object::trace(t);
  t.mark(buffer);
}

namespace {

bool valid_index(const TypedArrayObject* ta, double d) {
  if (d != std::trunc(d) || (d == 0 && std::signbit(d))) return false;
  return d >= 0 && d < static_cast<double>(ta->length());
}

}  // namespace

bool TypedArrayObject::get_own_property(Context& ctx, PropKey k, PropDesc* out, bool* found) {
  double idx;
  if (canonical_numeric(k, &idx)) {
    if (!valid_index(this, idx)) { *found = false; return true; }
    Value v;
    get_element(ctx, static_cast<size_t>(idx), &v);
    *out = PropDesc::Data(v, kDefaultAttrs);
    *found = true;
    return true;
  }
  return Object::get_own_property(ctx, k, out, found);
}

bool TypedArrayObject::has_property(Context& ctx, PropKey k, bool* out) {
  double idx;
  if (canonical_numeric(k, &idx)) { *out = valid_index(this, idx); return true; }
  return Object::has_property(ctx, k, out);
}

bool TypedArrayObject::define_own_property(Context& ctx, PropKey k, const PropDesc& d, bool* ok) {
  double idx;
  if (canonical_numeric(k, &idx)) {
    if (!valid_index(this, idx)) { *ok = false; return true; }
    if ((d.has_configurable && !d.configurable) || (d.has_enumerable && !d.enumerable) || d.is_accessor() || (d.has_writable && !d.writable)) { *ok = false; return true; }
    if (d.has_value) {
      Value v;
      if (!coerce_element(ctx, this, d.value, &v)) return false;
      set_element(static_cast<size_t>(idx), v);
    }
    *ok = true;
    return true;
  }
  return Object::define_own_property(ctx, k, d, ok);
}

bool TypedArrayObject::get(Context& ctx, PropKey k, const Value& receiver, Value* out) {
  double idx;
  if (canonical_numeric(k, &idx)) {
    if (!valid_index(this, idx)) { *out = Value::Undefined(); return true; }
    get_element(ctx, static_cast<size_t>(idx), out);
    return true;
  }
  return Object::get(ctx, k, receiver, out);
}

bool TypedArrayObject::set(Context& ctx, PropKey k, const Value& v, const Value& receiver, bool* ok) {
  double idx;
  if (canonical_numeric(k, &idx)) {
    if (receiver.is_object() && receiver.as_object() == this) {
      Value n;
      if (!coerce_element(ctx, this, v, &n)) return false;
      if (valid_index(this, idx)) set_element(static_cast<size_t>(idx), n);
      *ok = true;
      return true;
    }
    if (!valid_index(this, idx)) { *ok = true; return true; }
  }
  return Object::set(ctx, k, v, receiver, ok);
}

bool TypedArrayObject::delete_property(Context& ctx, PropKey k, bool* ok) {
  double idx;
  if (canonical_numeric(k, &idx)) { *ok = !valid_index(this, idx); return true; }
  return Object::delete_property(ctx, k, ok);
}

bool TypedArrayObject::own_keys(Context& ctx, std::vector<PropKey>* out) {
  const size_t n = length();
  for (size_t i = 0; i < n; ++i) out->push_back(ctx.index_atom(static_cast<uint32_t>(i)));
  return Object::own_keys(ctx, out);
}

bool typed_array_get(Context& ctx, Object* o, double index, Value* out) {
  TypedArrayObject* ta = static_cast<TypedArrayObject*>(o);
  if (index < 0 || index != std::trunc(index) || index >= static_cast<double>(ta->length())) return false;
  return ta->get_element(ctx, static_cast<size_t>(index), out);
}

bool typed_array_set(Context& ctx, Object* o, double index, const Value& v, bool* handled) {
  TypedArrayObject* ta = static_cast<TypedArrayObject*>(o);
  *handled = false;
  if (index < 0 || index != std::trunc(index)) return true;
  Value n;
  if (!coerce_element(ctx, ta, v, &n)) return false;
  *handled = true;
  ta->set_element(static_cast<size_t>(index), n);
  return true;
}

namespace {

// ---------------------------------------------------------------------------------------------------
// ArrayBuffer
// ---------------------------------------------------------------------------------------------------

ArrayBufferObject* new_buffer(Context& ctx, size_t len, Object* proto = nullptr) {
  ArrayBufferObject* b = ctx.heap.make<ArrayBufferObject>();
  b->proto = proto ? proto : ctx.intr.arraybuffer_proto;
  b->data.assign(len, 0);
  return b;
}

ArrayBufferObject* this_buffer(Context& ctx, const Value& v, const char* method) {
  if (v.is_object() && v.as_object()->cls == Class::ArrayBuffer) return static_cast<ArrayBufferObject*>(v.as_object());
  ctx.throw_type_error(std::string("Method ArrayBuffer.prototype.") + method + " called on incompatible receiver " + vm_describe(ctx, v));
  return nullptr;
}

Value arraybuffer_ctor(Context& ctx, CallArgs& a) {
  if (!a.is_construct()) {
    ctx.throw_type_error("Constructor ArrayBuffer requires 'new'");
    return Value::Exception();
  }
  uint64_t len;
  JS_TRY(ctx.to_index(a[0], &len));
  bool resizable = false;
  uint64_t max_len = len;
  if (a[1].is_object()) {
    Value m;
    JS_TRY(a[1].as_object()->get(ctx, ctx.atom_ascii("maxByteLength"), a[1], &m));
    if (!m.is_undefined()) {
      JS_TRY(ctx.to_index(m, &max_len));
      resizable = true;
      if (len > max_len) {
        ctx.throw_range_error("Invalid array buffer max length");
        return Value::Exception();
      }
    }
  }
  if (len > (1ull << 32)) {
    ctx.throw_range_error("Array buffer allocation failed");
    return Value::Exception();
  }
  Object* proto;
  JS_TRY(ctx.prototype_from_ctor(a.new_target, ctx.intr.arraybuffer_proto, &proto));
  ArrayBufferObject* b = new_buffer(ctx, static_cast<size_t>(len), proto);
  b->resizable = resizable;
  b->max_len = static_cast<size_t>(max_len);
  return Value::Obj(b);
}

Value arraybuffer_is_view(Context&, CallArgs& a) {
  return Value::Bool(a[0].is_object() && (a[0].as_object()->cls == Class::TypedArray || a[0].as_object()->cls == Class::DataView));
}

Value arraybuffer_species(Context&, CallArgs& a) { return a.this_value; }

Value arraybuffer_byte_length(Context& ctx, CallArgs& a) {
  ArrayBufferObject* b = this_buffer(ctx, a.this_value, "byteLength");
  if (!b) return Value::Exception();
  return Value::Number(b->detached ? 0 : static_cast<double>(b->data.size()));
}

Value arraybuffer_max_byte_length(Context& ctx, CallArgs& a) {
  ArrayBufferObject* b = this_buffer(ctx, a.this_value, "maxByteLength");
  if (!b) return Value::Exception();
  return Value::Number(b->detached ? 0 : static_cast<double>(b->resizable ? b->max_len : b->data.size()));
}

Value arraybuffer_resizable(Context& ctx, CallArgs& a) {
  ArrayBufferObject* b = this_buffer(ctx, a.this_value, "resizable");
  if (!b) return Value::Exception();
  return Value::Bool(b->resizable);
}

Value arraybuffer_detached(Context& ctx, CallArgs& a) {
  ArrayBufferObject* b = this_buffer(ctx, a.this_value, "detached");
  if (!b) return Value::Exception();
  return Value::Bool(b->detached);
}

Value arraybuffer_slice(Context& ctx, CallArgs& a) {
  ArrayBufferObject* b = this_buffer(ctx, a.this_value, "slice");
  if (!b) return Value::Exception();
  if (b->detached) {
    ctx.throw_type_error("Cannot perform ArrayBuffer.prototype.slice on a detached ArrayBuffer");
    return Value::Exception();
  }
  const uint64_t len = b->data.size();
  uint64_t first, fin;
  JS_TRY(relative_index(ctx, a[0], len, 0, &first));
  JS_TRY(relative_index(ctx, a[1], len, len, &fin));
  const uint64_t new_len = fin > first ? fin - first : 0;
  Value c;
  JS_TRY(ctx.species_constructor(b, Value::Obj(ctx.intr.arraybuffer_ctor), &c));
  Value arg = Value::Number(static_cast<double>(new_len)), r;
  JS_TRY(ctx.construct(c, &arg, 1, c, &r));
  if (!r.is_object() || r.as_object()->cls != Class::ArrayBuffer) {
    ctx.throw_type_error("Species constructor did not return an ArrayBuffer");
    return Value::Exception();
  }
  ArrayBufferObject* nb = static_cast<ArrayBufferObject*>(r.as_object());
  if (nb == b || nb->data.size() < new_len) {
    ctx.throw_type_error("Species constructor returned an unsuitable ArrayBuffer");
    return Value::Exception();
  }
  if (new_len) std::memcpy(nb->data.data(), b->data.data() + first, static_cast<size_t>(new_len));
  return r;
}

Value arraybuffer_resize(Context& ctx, CallArgs& a) {
  ArrayBufferObject* b = this_buffer(ctx, a.this_value, "resize");
  if (!b) return Value::Exception();
  if (!b->resizable) {
    ctx.throw_type_error("Method ArrayBuffer.prototype.resize called on incompatible receiver #<ArrayBuffer>");
    return Value::Exception();
  }
  uint64_t n;
  JS_TRY(ctx.to_index(a[0], &n));
  if (b->detached) {
    ctx.throw_type_error("Cannot perform ArrayBuffer.prototype.resize on a detached ArrayBuffer");
    return Value::Exception();
  }
  if (n > b->max_len) {
    ctx.throw_range_error("ArrayBuffer.prototype.resize: Invalid length parameter");
    return Value::Exception();
  }
  b->data.resize(static_cast<size_t>(n), 0);
  return Value::Undefined();
}

Value transfer_impl(Context& ctx, CallArgs& a, bool fixed) {
  ArrayBufferObject* b = this_buffer(ctx, a.this_value, fixed ? "transferToFixedLength" : "transfer");
  if (!b) return Value::Exception();
  uint64_t n = b->data.size();
  if (!a[0].is_undefined()) JS_TRY(ctx.to_index(a[0], &n));
  if (b->detached) {
    ctx.throw_type_error("Cannot perform ArrayBuffer.prototype.transfer on a detached ArrayBuffer");
    return Value::Exception();
  }
  ArrayBufferObject* nb = new_buffer(ctx, static_cast<size_t>(n));
  std::memcpy(nb->data.data(), b->data.data(), std::min<size_t>(b->data.size(), static_cast<size_t>(n)));
  if (!fixed && b->resizable) { nb->resizable = true; nb->max_len = b->max_len; }
  b->data.clear();
  b->detached = true;
  return Value::Obj(nb);
}

Value arraybuffer_transfer(Context& ctx, CallArgs& a) { return transfer_impl(ctx, a, false); }
Value arraybuffer_transfer_fixed(Context& ctx, CallArgs& a) { return transfer_impl(ctx, a, true); }

// ---------------------------------------------------------------------------------------------------
// typed array construction
// ---------------------------------------------------------------------------------------------------

TypedArrayObject* make_typed_array(Context& ctx, int kind, ArrayBufferObject* buf, size_t offset, size_t length, bool tracking, Object* proto = nullptr) {
  TypedArrayObject* t = ctx.heap.make<TypedArrayObject>(kind);
  t->proto = proto ? proto : ctx.intr.typedarray_protos[kind];
  t->buffer = buf;
  t->byte_offset = offset;
  t->fixed_length = length;
  t->length_tracking = tracking;
  return t;
}

TypedArrayObject* new_typed_array(Context& ctx, int kind, size_t length, Object* proto = nullptr) {
  ArrayBufferObject* buf = new_buffer(ctx, length * static_cast<size_t>(kKindSizes[kind]));
  return make_typed_array(ctx, kind, buf, 0, length, false, proto);
}

TypedArrayObject* validate(Context& ctx, const Value& v, const char* method) {
  if (v.is_object() && v.as_object()->cls == Class::TypedArray) {
    TypedArrayObject* t = static_cast<TypedArrayObject*>(v.as_object());
    if (t->out_of_bounds()) {
      ctx.throw_type_error(std::string("Cannot perform ") + method + " on a detached ArrayBuffer");
      return nullptr;
    }
    return t;
  }
  ctx.throw_type_error("this is not a typed array.");
  return nullptr;
}

Value typed_array_ctor_impl(Context& ctx, CallArgs& a, int kind) {
  if (!a.is_construct()) {
    ctx.throw_type_error(std::string("Constructor ") + kKindNames[kind] + " requires 'new'");
    return Value::Exception();
  }
  Object* proto;
  JS_TRY(ctx.prototype_from_ctor(a.new_target, ctx.intr.typedarray_protos[kind], &proto));
  const size_t esize = static_cast<size_t>(kKindSizes[kind]);
  const Value first = a[0];
  if (!first.is_object()) {
    uint64_t len = 0;
    JS_TRY(ctx.to_index(first, &len));
    if (len > (1ull << 32) / esize) {
      ctx.throw_range_error("Invalid typed array length: " + std::to_string(len));
      return Value::Exception();
    }
    return Value::Obj(new_typed_array(ctx, kind, static_cast<size_t>(len), proto));
  }
  Object* fo = first.as_object();
  if (fo->cls == Class::ArrayBuffer) {
    ArrayBufferObject* buf = static_cast<ArrayBufferObject*>(fo);
    uint64_t offset;
    JS_TRY(ctx.to_index(a[1], &offset));
    if (offset % esize != 0) {
      ctx.throw_range_error(std::string("start offset of ") + kKindNames[kind] + " should be a multiple of " + std::to_string(esize));
      return Value::Exception();
    }
    uint64_t new_len = 0;
    bool have_len = !a[2].is_undefined();
    if (have_len) JS_TRY(ctx.to_index(a[2], &new_len));
    if (buf->detached) {
      ctx.throw_type_error("Cannot perform Construct on a detached ArrayBuffer");
      return Value::Exception();
    }
    const uint64_t buf_len = buf->data.size();
    if (!have_len) {
      if (buf->resizable) {
        if (offset > buf_len) {
          ctx.throw_range_error("Start offset " + std::to_string(offset) + " is outside the bounds of the buffer");
          return Value::Exception();
        }
        return Value::Obj(make_typed_array(ctx, kind, buf, static_cast<size_t>(offset), 0, true, proto));
      }
      if (buf_len % esize != 0) {
        ctx.throw_range_error(std::string("byte length of ") + kKindNames[kind] + " should be a multiple of " + std::to_string(esize));
        return Value::Exception();
      }
      if (offset > buf_len) {
        ctx.throw_range_error("Start offset " + std::to_string(offset) + " is outside the bounds of the buffer");
        return Value::Exception();
      }
      return Value::Obj(make_typed_array(ctx, kind, buf, static_cast<size_t>(offset), static_cast<size_t>((buf_len - offset) / esize), false, proto));
    }
    if (offset + new_len * esize > buf_len) {
      ctx.throw_range_error("Invalid typed array length: " + std::to_string(new_len));
      return Value::Exception();
    }
    return Value::Obj(make_typed_array(ctx, kind, buf, static_cast<size_t>(offset), static_cast<size_t>(new_len), false, proto));
  }
  if (fo->cls == Class::TypedArray) {
    TypedArrayObject* src = static_cast<TypedArrayObject*>(fo);
    if (src->out_of_bounds()) {
      ctx.throw_type_error("Cannot perform Construct on a detached ArrayBuffer");
      return Value::Exception();
    }
    const size_t n = src->length();
    if (src->is_bigint_kind() != (kind >= kFirstBigKind)) {
      ctx.throw_mixed_bigint();
      return Value::Exception();
    }
    TypedArrayObject* t = new_typed_array(ctx, kind, n, proto);
    for (size_t i = 0; i < n; ++i) {
      Value v;
      src->get_element(ctx, i, &v);
      t->set_element(i, v);
    }
    return Value::Obj(t);
  }
  // an iterable or an array-like
  std::vector<Value> values;
  Value using_iter;
  JS_TRY(ctx.get_method(first, ctx.sym.iterator, &using_iter));
  if (!using_iter.is_undefined()) {
    JS_TRY(ctx.iterable_to_list(first, &values));
  } else {
    uint64_t len;
    JS_TRY(ctx.length_of(fo, &len));
    for (uint64_t i = 0; i < len; ++i) {
      Value v;
      JS_TRY(get_idx(ctx, fo, i, &v));
      values.push_back(v);
    }
  }
  TypedArrayObject* t = new_typed_array(ctx, kind, values.size(), proto);
  for (size_t i = 0; i < values.size(); ++i) {
    Value e;
    JS_TRY(coerce_element(ctx, t, values[i], &e));
    t->set_element(i, e);
  }
  return Value::Obj(t);
}

#define KIND_CTOR(n, k) Value typed_array_ctor_##n(Context& ctx, CallArgs& a) { return typed_array_ctor_impl(ctx, a, k); }
KIND_CTOR(0, 0) KIND_CTOR(1, 1) KIND_CTOR(2, 2) KIND_CTOR(3, 3) KIND_CTOR(4, 4) KIND_CTOR(5, 5) KIND_CTOR(6, 6) KIND_CTOR(7, 7) KIND_CTOR(8, 8) KIND_CTOR(9, 9) KIND_CTOR(10, 10)
#undef KIND_CTOR
const NativeFn kKindCtors[kKinds] = {typed_array_ctor_0, typed_array_ctor_1, typed_array_ctor_2, typed_array_ctor_3, typed_array_ctor_4,
                                     typed_array_ctor_5, typed_array_ctor_6, typed_array_ctor_7, typed_array_ctor_8, typed_array_ctor_9, typed_array_ctor_10};

Value abstract_typed_array_ctor(Context& ctx, CallArgs&) {
  ctx.throw_type_error("Abstract class TypedArray not directly constructable");
  return Value::Exception();
}

// TypedArrayCreate(constructor, args): constructs and validates.
bool typed_array_create(Context& ctx, const Value& ctor, std::vector<Value> args, TypedArrayObject** out, size_t min_len = 0) {
  Value r;
  if (!ctx.construct(ctor, args.data(), static_cast<int>(args.size()), ctor, &r)) return false;
  TypedArrayObject* t = validate(ctx, r, "typed array creation");
  if (!t) return false;
  if (args.size() == 1 && args[0].is_number() && static_cast<double>(t->length()) < args[0].num) {
    ctx.throw_type_error("Derived TypedArray constructor created an array which was too small");
    return false;
  }
  (void)min_len;
  *out = t;
  return true;
}

bool species_create(Context& ctx, TypedArrayObject* exemplar, std::vector<Value> args, TypedArrayObject** out) {
  Value def = Value::Obj(ctx.intr.typedarray_ctors[exemplar->kind]);
  Value c;
  if (!ctx.species_constructor(exemplar, def, &c)) return false;
  if (!typed_array_create(ctx, c, std::move(args), out)) return false;
  return true;
}

Value typed_array_from(Context& ctx, CallArgs& a) {
  if (!a.this_value.is_object() || !a.this_value.as_object()->is_constructor) {
    ctx.throw_type_error(vm_describe(ctx, a.this_value) + " is not a constructor");
    return Value::Exception();
  }
  const bool mapping = !a[1].is_undefined();
  if (mapping && !a[1].is_callable()) {
    ctx.throw_type_error(vm_describe(ctx, a[1]) + " is not a function");
    return Value::Exception();
  }
  std::vector<Value> values;
  Value using_iter;
  JS_TRY(ctx.get_method(a[0], ctx.sym.iterator, &using_iter));
  if (!using_iter.is_undefined()) {
    JS_TRY(ctx.iterable_to_list(a[0], &values));
  } else {
    Object* o;
    JS_TRY(ctx.to_object(a[0], &o));
    uint64_t len;
    JS_TRY(ctx.length_of(o, &len));
    for (uint64_t i = 0; i < len; ++i) {
      Value v;
      JS_TRY(get_idx(ctx, o, i, &v));
      values.push_back(v);
    }
  }
  TypedArrayObject* t;
  JS_TRY(typed_array_create(ctx, a.this_value, {Value::Number(static_cast<double>(values.size()))}, &t));
  for (size_t i = 0; i < values.size(); ++i) {
    Value v = values[i];
    if (mapping) {
      Value m;
      JS_TRY(ctx.call(a[1], a[2], {v, Value::Number(static_cast<double>(i))}, &m));
      v = m;
    }
    JS_TRY(ctx.put_v(Value::Obj(t), ctx.index_atom(static_cast<uint32_t>(i)), v, true));
  }
  return Value::Obj(t);
}

Value typed_array_of(Context& ctx, CallArgs& a) {
  if (!a.this_value.is_object() || !a.this_value.as_object()->is_constructor) {
    ctx.throw_type_error(vm_describe(ctx, a.this_value) + " is not a constructor");
    return Value::Exception();
  }
  TypedArrayObject* t;
  JS_TRY(typed_array_create(ctx, a.this_value, {Value::Number(a.argc)}, &t));
  for (int i = 0; i < a.argc; ++i) JS_TRY(ctx.put_v(Value::Obj(t), ctx.index_atom(static_cast<uint32_t>(i)), a.argv[i], true));
  return Value::Obj(t);
}

// ---------------------------------------------------------------------------------------------------
// %TypedArray%.prototype
// ---------------------------------------------------------------------------------------------------

#define THIS_TA(var, method)                                   \
  TypedArrayObject* var = validate(ctx, a.this_value, method); \
  if (!var) return Value::Exception()

Value ta_buffer(Context& ctx, CallArgs& a) {
  if (!a.this_value.is_object() || a.this_value.as_object()->cls != Class::TypedArray) {
    ctx.throw_type_error("this is not a typed array.");
    return Value::Exception();
  }
  return Value::Obj(static_cast<TypedArrayObject*>(a.this_value.as_object())->buffer);
}

Value ta_byte_length(Context& ctx, CallArgs& a) {
  if (!a.this_value.is_object() || a.this_value.as_object()->cls != Class::TypedArray) {
    ctx.throw_type_error("this is not a typed array.");
    return Value::Exception();
  }
  TypedArrayObject* t = static_cast<TypedArrayObject*>(a.this_value.as_object());
  return Value::Number(static_cast<double>(t->length() * static_cast<size_t>(t->element_size())));
}

Value ta_byte_offset(Context& ctx, CallArgs& a) {
  if (!a.this_value.is_object() || a.this_value.as_object()->cls != Class::TypedArray) {
    ctx.throw_type_error("this is not a typed array.");
    return Value::Exception();
  }
  TypedArrayObject* t = static_cast<TypedArrayObject*>(a.this_value.as_object());
  return Value::Number(t->out_of_bounds() ? 0 : static_cast<double>(t->byte_offset));
}

Value ta_length(Context& ctx, CallArgs& a) {
  if (!a.this_value.is_object() || a.this_value.as_object()->cls != Class::TypedArray) {
    ctx.throw_type_error("this is not a typed array.");
    return Value::Exception();
  }
  return Value::Number(static_cast<double>(static_cast<TypedArrayObject*>(a.this_value.as_object())->length()));
}

Value ta_tag_getter(Context& ctx, CallArgs& a) {
  if (!a.this_value.is_object() || a.this_value.as_object()->cls != Class::TypedArray) return Value::Undefined();
  return Str(ctx, kKindNames[static_cast<TypedArrayObject*>(a.this_value.as_object())->kind]);
}

Value ta_at(Context& ctx, CallArgs& a) {
  THIS_TA(t, "%TypedArray%.prototype.at");
  const size_t len = t->length();
  double rel;
  JS_TRY(ctx.to_integer(a[0], &rel));
  double k = rel >= 0 ? rel : static_cast<double>(len) + rel;
  if (k < 0 || k >= static_cast<double>(t->length())) return Value::Undefined();
  Value v;
  t->get_element(ctx, static_cast<size_t>(k), &v);
  return v;
}

Value ta_fill(Context& ctx, CallArgs& a) {
  THIS_TA(t, "%TypedArray%.prototype.fill");
  const size_t len = t->length();
  Value v;
  JS_TRY(coerce_element(ctx, t, a[0], &v));
  uint64_t k, e;
  JS_TRY(relative_index(ctx, a[1], len, 0, &k));
  JS_TRY(relative_index(ctx, a[2], len, len, &e));
  if (t->out_of_bounds()) {
    ctx.throw_type_error("Cannot perform %TypedArray%.prototype.fill on a detached ArrayBuffer");
    return Value::Exception();
  }
  e = std::min<uint64_t>(e, t->length());
  for (; k < e; ++k) t->set_element(static_cast<size_t>(k), v);
  return a.this_value;
}

Value ta_set(Context& ctx, CallArgs& a) {
  if (!a.this_value.is_object() || a.this_value.as_object()->cls != Class::TypedArray) {
    ctx.throw_type_error("this is not a typed array.");
    return Value::Exception();
  }
  TypedArrayObject* t = static_cast<TypedArrayObject*>(a.this_value.as_object());
  double off;
  JS_TRY(ctx.to_integer(a[1], &off));
  if (off < 0) {
    ctx.throw_range_error("offset is out of bounds");
    return Value::Exception();
  }
  if (t->out_of_bounds()) {
    ctx.throw_type_error("Cannot perform %TypedArray%.prototype.set on a detached ArrayBuffer");
    return Value::Exception();
  }
  const size_t target_len = t->length();
  if (a[0].is_object() && a[0].as_object()->cls == Class::TypedArray) {
    TypedArrayObject* src = static_cast<TypedArrayObject*>(a[0].as_object());
    if (src->out_of_bounds()) {
      ctx.throw_type_error("Cannot perform %TypedArray%.prototype.set on a detached ArrayBuffer");
      return Value::Exception();
    }
    const size_t n = src->length();
    if (off + static_cast<double>(n) > static_cast<double>(target_len)) {
      ctx.throw_range_error("offset is out of bounds");
      return Value::Exception();
    }
    if (src->is_bigint_kind() != t->is_bigint_kind()) {
      ctx.throw_mixed_bigint();
      return Value::Exception();
    }
    std::vector<Value> tmp(n);
    for (size_t i = 0; i < n; ++i) src->get_element(ctx, i, &tmp[i]);
    for (size_t i = 0; i < n; ++i) t->set_element(static_cast<size_t>(off) + i, tmp[i]);
    return Value::Undefined();
  }
  Object* src;
  JS_TRY(ctx.to_object(a[0], &src));
  uint64_t n;
  JS_TRY(ctx.length_of(src, &n));
  if (off + static_cast<double>(n) > static_cast<double>(target_len)) {
    ctx.throw_range_error("offset is out of bounds");
    return Value::Exception();
  }
  for (uint64_t i = 0; i < n; ++i) {
    Value v;
    JS_TRY(get_idx(ctx, src, i, &v));
    Value e;
    JS_TRY(coerce_element(ctx, t, v, &e));
    t->set_element(static_cast<size_t>(off) + static_cast<size_t>(i), e);
  }
  return Value::Undefined();
}

Value ta_subarray(Context& ctx, CallArgs& a) {
  if (!a.this_value.is_object() || a.this_value.as_object()->cls != Class::TypedArray) {
    ctx.throw_type_error("this is not a typed array.");
    return Value::Exception();
  }
  TypedArrayObject* t = static_cast<TypedArrayObject*>(a.this_value.as_object());
  const size_t src_len = t->length();
  uint64_t begin, end;
  JS_TRY(relative_index(ctx, a[0], src_len, 0, &begin));
  const bool end_undefined = a[1].is_undefined();
  JS_TRY(relative_index(ctx, a[1], src_len, src_len, &end));
  const size_t esize = static_cast<size_t>(t->element_size());
  const size_t begin_byte = t->byte_offset + static_cast<size_t>(begin) * esize;
  std::vector<Value> args = {Value::Obj(t->buffer), Value::Number(static_cast<double>(begin_byte))};
  if (!(t->length_tracking && end_undefined)) args.push_back(Value::Number(static_cast<double>(end > begin ? end - begin : 0)));
  TypedArrayObject* r;
  Value def = Value::Obj(ctx.intr.typedarray_ctors[t->kind]);
  Value c;
  JS_TRY(ctx.species_constructor(t, def, &c));
  Value res;
  JS_TRY(ctx.construct(c, args.data(), static_cast<int>(args.size()), c, &res));
  r = validate(ctx, res, "subarray");
  if (!r) return Value::Exception();
  return res;
}

Value ta_slice(Context& ctx, CallArgs& a) {
  THIS_TA(t, "%TypedArray%.prototype.slice");
  const size_t len = t->length();
  uint64_t k, fin;
  JS_TRY(relative_index(ctx, a[0], len, 0, &k));
  JS_TRY(relative_index(ctx, a[1], len, len, &fin));
  const size_t count = fin > k ? static_cast<size_t>(fin - k) : 0;
  TypedArrayObject* r;
  JS_TRY(species_create(ctx, t, {Value::Number(static_cast<double>(count))}, &r));
  if (count) {
    if (t->out_of_bounds()) {
      ctx.throw_type_error("Cannot perform %TypedArray%.prototype.slice on a detached ArrayBuffer");
      return Value::Exception();
    }
    fin = std::min<uint64_t>(fin, t->length());
    size_t n = 0;
    for (uint64_t i = k; i < fin; ++i, ++n) {
      Value v;
      t->get_element(ctx, static_cast<size_t>(i), &v);
      JS_TRY(ctx.put_v(Value::Obj(r), ctx.index_atom(static_cast<uint32_t>(n)), v, true));
    }
  }
  return Value::Obj(r);
}

Value ta_copy_within(Context& ctx, CallArgs& a) {
  THIS_TA(t, "%TypedArray%.prototype.copyWithin");
  const size_t len = t->length();
  uint64_t to, from, fin;
  JS_TRY(relative_index(ctx, a[0], len, 0, &to));
  JS_TRY(relative_index(ctx, a[1], len, 0, &from));
  JS_TRY(relative_index(ctx, a[2], len, len, &fin));
  if (fin <= from) return a.this_value;
  uint64_t count = std::min(fin - from, len - to);
  if (t->out_of_bounds()) {
    ctx.throw_type_error("Cannot perform %TypedArray%.prototype.copyWithin on a detached ArrayBuffer");
    return Value::Exception();
  }
  const size_t cur = t->length();
  if (to >= cur || from >= cur) return a.this_value;
  count = std::min<uint64_t>(count, std::min(cur - to, cur - from));
  uint8_t* base = t->buffer->data.data() + t->byte_offset;
  const size_t es = static_cast<size_t>(t->element_size());
  std::memmove(base + to * es, base + from * es, static_cast<size_t>(count) * es);
  return a.this_value;
}

Value ta_reverse(Context& ctx, CallArgs& a) {
  THIS_TA(t, "%TypedArray%.prototype.reverse");
  const size_t len = t->length();
  for (size_t lo = 0, hi = len; lo + 1 < hi; ++lo, --hi) {
    Value x, y;
    t->get_element(ctx, lo, &x);
    t->get_element(ctx, hi - 1, &y);
    t->set_element(lo, y);
    t->set_element(hi - 1, x);
  }
  return a.this_value;
}

bool numeric_less(double x, double y) {
  if (std::isnan(y)) return !std::isnan(x);
  if (std::isnan(x)) return false;
  if (x < y) return true;
  if (x > y) return false;
  if (x == 0 && y == 0) return std::signbit(x) && !std::signbit(y);
  return false;
}

// Elements of one array are all Numbers or all BigInts.
bool elem_less(const Value& x, const Value& y) {
  if (x.is_bigint()) return big::cmp(x.as_bigint()->n, y.as_bigint()->n) < 0;
  return numeric_less(x.num, y.num);
}

bool sort_values(Context& ctx, std::vector<Value>& vals, const Value& cmp) {
  if (cmp.is_undefined()) {
    std::stable_sort(vals.begin(), vals.end(), elem_less);
    return true;
  }
  // a merge sort that tolerates a throwing comparator
  std::vector<Value> tmp(vals.size());
  bool failed = false;
  auto le = [&](const Value& x, const Value& y) {
    Value r;
    if (!ctx.call(cmp, Value::Undefined(), {x, y}, &r)) { failed = true; return true; }
    double d;
    if (!ctx.to_number(r, &d)) { failed = true; return true; }
    return !(d > 0);
  };
  const size_t n = vals.size();
  for (size_t width = 1; width < n && !failed; width *= 2) {
    for (size_t lo = 0; lo < n && !failed; lo += 2 * width) {
      const size_t mid = std::min(lo + width, n), hi = std::min(lo + 2 * width, n);
      size_t i = lo, j = mid, k = lo;
      while (i < mid && j < hi) tmp[k++] = le(vals[i], vals[j]) ? vals[i++] : vals[j++];
      while (i < mid) tmp[k++] = vals[i++];
      while (j < hi) tmp[k++] = vals[j++];
    }
    if (!failed) vals.swap(tmp);
  }
  return !failed;
}

Value ta_sort(Context& ctx, CallArgs& a) {
  if (!a[0].is_undefined() && !a[0].is_callable()) {
    ctx.throw_type_error("The comparison function must be either a function or undefined");
    return Value::Exception();
  }
  THIS_TA(t, "%TypedArray%.prototype.sort");
  const size_t len = t->length();
  std::vector<Value> vals(len);
  for (size_t i = 0; i < len; ++i) t->get_element(ctx, i, &vals[i]);
  JS_TRY(sort_values(ctx, vals, a[0]));
  for (size_t i = 0; i < len; ++i) t->set_element(i, vals[i]);
  return a.this_value;
}

Value ta_to_sorted(Context& ctx, CallArgs& a) {
  if (!a[0].is_undefined() && !a[0].is_callable()) {
    ctx.throw_type_error("The comparison function must be either a function or undefined");
    return Value::Exception();
  }
  THIS_TA(t, "%TypedArray%.prototype.toSorted");
  const size_t len = t->length();
  std::vector<Value> vals(len);
  for (size_t i = 0; i < len; ++i) t->get_element(ctx, i, &vals[i]);
  JS_TRY(sort_values(ctx, vals, a[0]));
  TypedArrayObject* r = new_typed_array(ctx, t->kind, len);
  for (size_t i = 0; i < len; ++i) r->set_element(i, vals[i]);
  return Value::Obj(r);
}

Value ta_to_reversed(Context& ctx, CallArgs& a) {
  THIS_TA(t, "%TypedArray%.prototype.toReversed");
  const size_t len = t->length();
  TypedArrayObject* r = new_typed_array(ctx, t->kind, len);
  for (size_t i = 0; i < len; ++i) { Value v; t->get_element(ctx, len - 1 - i, &v); r->set_element(i, v); }
  return Value::Obj(r);
}

Value ta_with(Context& ctx, CallArgs& a) {
  THIS_TA(t, "%TypedArray%.prototype.with");
  const size_t len = t->length();
  double rel;
  JS_TRY(ctx.to_integer(a[0], &rel));
  double idx = rel >= 0 ? rel : static_cast<double>(len) + rel;
  Value v;
  JS_TRY(coerce_element(ctx, t, a[1], &v));
  if (idx < 0 || idx >= static_cast<double>(t->length())) {
    ctx.throw_range_error("Invalid typed array index");
    return Value::Exception();
  }
  TypedArrayObject* r = new_typed_array(ctx, t->kind, len);
  for (size_t i = 0; i < len; ++i) {
    Value e;
    if (i == static_cast<size_t>(idx)) e = v;
    else t->get_element(ctx, i, &e);
    r->set_element(i, e);
  }
  return Value::Obj(r);
}

Value ta_join(Context& ctx, CallArgs& a) {
  THIS_TA(t, "%TypedArray%.prototype.join");
  const size_t len = t->length();
  std::u16string sep = u",";
  if (!a[0].is_undefined()) {
    JsString* s;
    JS_TRY(ctx.to_string(a[0], &s));
    sep = s->str();
  }
  std::u16string out;
  for (size_t i = 0; i < len; ++i) {
    if (i) out += sep;
    Value v;
    if (t->get_element(ctx, i, &v)) {
      JsString* s;
      JS_TRY(ctx.to_string(v, &s));
      out += s->str();
    }
  }
  return StrV(ctx, std::move(out));
}

Value ta_to_locale_string(Context& ctx, CallArgs& a) {
  THIS_TA(t, "%TypedArray%.prototype.toLocaleString");
  const size_t len = t->length();
  std::u16string out;
  for (size_t i = 0; i < len; ++i) {
    if (i) out += u",";
    Value v;
    if (!t->get_element(ctx, i, &v)) continue;
    Value f, r;
    JsString* s;
    JS_TRY(ctx.get_v(v, ctx.atom_ascii("toLocaleString"), &f));
    JS_TRY(ctx.call(f, v, nullptr, 0, &r));
    JS_TRY(ctx.to_string(r, &s));
    out += s->str();
  }
  return StrV(ctx, std::move(out));
}

Value ta_index_of_impl(Context& ctx, CallArgs& a, int mode) {   // 0 indexOf, 1 lastIndexOf, 2 includes
  THIS_TA(t, "%TypedArray%.prototype.indexOf");
  const size_t len = t->length();
  if (len == 0) return mode == 2 ? Value::Bool(false) : Value::Number(-1);
  if (mode == 1) {
    double from = static_cast<double>(len) - 1;
    if (a.argc > 1) {
      double n;
      JS_TRY(ctx.to_integer(a[1], &n));
      from = n >= 0 ? std::min(n, static_cast<double>(len) - 1) : static_cast<double>(len) + n;
    }
    for (double k = from; k >= 0; --k) {
      Value v;
      if (t->get_element(ctx, static_cast<size_t>(k), &v) && Context::strict_equals(v, a[0])) return Value::Number(k);
    }
    return Value::Number(-1);
  }
  uint64_t k;
  JS_TRY(relative_index(ctx, a[1], len, 0, &k));
  for (; k < len; ++k) {
    Value v;
    const bool has = t->get_element(ctx, static_cast<size_t>(k), &v);
    if (mode == 2) {
      if (!has) v = Value::Undefined();
      if (ctx.same_value_zero(v, a[0])) return Value::Bool(true);
    } else if (has && Context::strict_equals(v, a[0])) {
      return Value::Number(static_cast<double>(k));
    }
  }
  return mode == 2 ? Value::Bool(false) : Value::Number(-1);
}

Value ta_index_of(Context& ctx, CallArgs& a) { return ta_index_of_impl(ctx, a, 0); }
Value ta_last_index_of(Context& ctx, CallArgs& a) { return ta_index_of_impl(ctx, a, 1); }
Value ta_includes(Context& ctx, CallArgs& a) { return ta_index_of_impl(ctx, a, 2); }

enum class TCb { ForEach, Map, Filter, Some, Every, Find, FindIndex, FindLast, FindLastIndex };

Value ta_callback_impl(Context& ctx, CallArgs& a, TCb kind, const char* name) {
  THIS_TA(t, name);
  const size_t len = t->length();
  if (!a[0].is_callable()) {
    ctx.throw_type_error(vm_describe(ctx, a[0]) + " is not a function");
    return Value::Exception();
  }
  TypedArrayObject* result = nullptr;
  std::vector<Value> kept;
  if (kind == TCb::Map) JS_TRY(species_create(ctx, t, {Value::Number(static_cast<double>(len))}, &result));
  const bool reverse = kind == TCb::FindLast || kind == TCb::FindLastIndex;
  for (size_t n = 0; n < len; ++n) {
    const size_t k = reverse ? len - 1 - n : n;
    Value v;
    if (!t->get_element(ctx, k, &v)) v = Value::Undefined();
    Value r;
    JS_TRY(ctx.call(a[0], a[1], {v, Value::Number(static_cast<double>(k)), a.this_value}, &r));
    switch (kind) {
      case TCb::ForEach: break;
      case TCb::Map: JS_TRY(ctx.put_v(Value::Obj(result), ctx.index_atom(static_cast<uint32_t>(k)), r, true)); break;
      case TCb::Filter: if (Context::to_boolean(r)) kept.push_back(v); break;
      case TCb::Some: if (Context::to_boolean(r)) return Value::Bool(true); break;
      case TCb::Every: if (!Context::to_boolean(r)) return Value::Bool(false); break;
      case TCb::Find: case TCb::FindLast: if (Context::to_boolean(r)) return v; break;
      case TCb::FindIndex: case TCb::FindLastIndex: if (Context::to_boolean(r)) return Value::Number(static_cast<double>(k)); break;
    }
  }
  switch (kind) {
    case TCb::Map: return Value::Obj(result);
    case TCb::Filter: {
      TypedArrayObject* r;
      JS_TRY(species_create(ctx, t, {Value::Number(static_cast<double>(kept.size()))}, &r));
      for (size_t i = 0; i < kept.size(); ++i) JS_TRY(ctx.put_v(Value::Obj(r), ctx.index_atom(static_cast<uint32_t>(i)), kept[i], true));
      return Value::Obj(r);
    }
    case TCb::Some: return Value::Bool(false);
    case TCb::Every: return Value::Bool(true);
    case TCb::FindIndex: case TCb::FindLastIndex: return Value::Number(-1);
    default: return Value::Undefined();
  }
}

Value ta_for_each(Context& ctx, CallArgs& a) { return ta_callback_impl(ctx, a, TCb::ForEach, "%TypedArray%.prototype.forEach"); }
Value ta_map(Context& ctx, CallArgs& a) { return ta_callback_impl(ctx, a, TCb::Map, "%TypedArray%.prototype.map"); }
Value ta_filter(Context& ctx, CallArgs& a) { return ta_callback_impl(ctx, a, TCb::Filter, "%TypedArray%.prototype.filter"); }
Value ta_some(Context& ctx, CallArgs& a) { return ta_callback_impl(ctx, a, TCb::Some, "%TypedArray%.prototype.some"); }
Value ta_every(Context& ctx, CallArgs& a) { return ta_callback_impl(ctx, a, TCb::Every, "%TypedArray%.prototype.every"); }
Value ta_find(Context& ctx, CallArgs& a) { return ta_callback_impl(ctx, a, TCb::Find, "%TypedArray%.prototype.find"); }
Value ta_find_index(Context& ctx, CallArgs& a) { return ta_callback_impl(ctx, a, TCb::FindIndex, "%TypedArray%.prototype.findIndex"); }
Value ta_find_last(Context& ctx, CallArgs& a) { return ta_callback_impl(ctx, a, TCb::FindLast, "%TypedArray%.prototype.findLast"); }
Value ta_find_last_index(Context& ctx, CallArgs& a) { return ta_callback_impl(ctx, a, TCb::FindLastIndex, "%TypedArray%.prototype.findLastIndex"); }

Value ta_reduce_impl(Context& ctx, CallArgs& a, bool right) {
  THIS_TA(t, "%TypedArray%.prototype.reduce");
  const size_t len = t->length();
  if (!a[0].is_callable()) {
    ctx.throw_type_error(vm_describe(ctx, a[0]) + " is not a function");
    return Value::Exception();
  }
  size_t n = 0;
  Value acc;
  if (a.argc >= 2) acc = a[1];
  else {
    if (len == 0) {
      ctx.throw_type_error("Reduce of empty array with no initial value");
      return Value::Exception();
    }
    t->get_element(ctx, right ? len - 1 : 0, &acc);
    n = 1;
  }
  for (; n < len; ++n) {
    const size_t k = right ? len - 1 - n : n;
    Value v;
    if (!t->get_element(ctx, k, &v)) v = Value::Undefined();
    Value r;
    JS_TRY(ctx.call(a[0], Value::Undefined(), {acc, v, Value::Number(static_cast<double>(k)), a.this_value}, &r));
    acc = r;
  }
  return acc;
}

Value ta_reduce(Context& ctx, CallArgs& a) { return ta_reduce_impl(ctx, a, false); }
Value ta_reduce_right(Context& ctx, CallArgs& a) { return ta_reduce_impl(ctx, a, true); }

Value make_ta_iterator(Context& ctx, CallArgs& a, int kind, const char* name) {
  THIS_TA(t, name);
  // typed arrays share the array iterator: its next() reads `length` and the elements generically
  Value values_fn;
  JS_TRY(ctx.get(ctx.intr.array_proto, kind == 0 ? ctx.atoms.keys : kind == 1 ? ctx.atoms.values : ctx.atoms.entries, &values_fn));
  Value r;
  JS_TRY(ctx.call(values_fn, a.this_value, nullptr, 0, &r));
  return r;
}

Value ta_values(Context& ctx, CallArgs& a) { return make_ta_iterator(ctx, a, 1, "%TypedArray%.prototype.values"); }
Value ta_keys(Context& ctx, CallArgs& a) { return make_ta_iterator(ctx, a, 0, "%TypedArray%.prototype.keys"); }
Value ta_entries(Context& ctx, CallArgs& a) { return make_ta_iterator(ctx, a, 2, "%TypedArray%.prototype.entries"); }

// ---------------------------------------------------------------------------------------------------
// DataView
// ---------------------------------------------------------------------------------------------------

struct DataViewObject final : Object {
  DataViewObject() : Object(Class::DataView) {}
  ArrayBufferObject* buffer = nullptr;
  size_t byte_offset = 0;
  size_t fixed_length = 0;
  bool length_tracking = false;
  void trace(Tracer& t) override { Object::trace(t); t.mark(buffer); }
  bool out_of_bounds() const {
    if (!buffer || buffer->detached) return true;
    if (length_tracking) return byte_offset > buffer->data.size();
    return byte_offset + fixed_length > buffer->data.size();
  }
  size_t view_length() const {
    if (out_of_bounds()) return 0;
    return length_tracking ? buffer->data.size() - byte_offset : fixed_length;
  }
};

DataViewObject* this_view(Context& ctx, const Value& v) {
  if (v.is_object() && v.as_object()->cls == Class::DataView) return static_cast<DataViewObject*>(v.as_object());
  ctx.throw_type_error("Receiver is not a DataView");
  return nullptr;
}

Value dataview_ctor(Context& ctx, CallArgs& a) {
  if (!a.is_construct()) {
    ctx.throw_type_error("Constructor DataView requires 'new'");
    return Value::Exception();
  }
  if (!a[0].is_object() || a[0].as_object()->cls != Class::ArrayBuffer) {
    ctx.throw_type_error("First argument to DataView constructor must be an ArrayBuffer");
    return Value::Exception();
  }
  ArrayBufferObject* buf = static_cast<ArrayBufferObject*>(a[0].as_object());
  uint64_t offset;
  JS_TRY(ctx.to_index(a[1], &offset));
  if (buf->detached) {
    ctx.throw_type_error("Cannot perform DataView on a detached ArrayBuffer");
    return Value::Exception();
  }
  const uint64_t buf_len = buf->data.size();
  if (offset > buf_len) {
    ctx.throw_range_error("Start offset " + std::to_string(offset) + " is outside the bounds of the buffer");
    return Value::Exception();
  }
  bool tracking = false;
  uint64_t view_len = 0;
  if (a[2].is_undefined()) {
    if (buf->resizable) tracking = true;
    else view_len = buf_len - offset;
  } else {
    JS_TRY(ctx.to_index(a[2], &view_len));
    if (offset + view_len > buf_len) {
      ctx.throw_range_error("Invalid DataView length " + std::to_string(view_len));
      return Value::Exception();
    }
  }
  Object* proto;
  JS_TRY(ctx.prototype_from_ctor(a.new_target, ctx.intr.dataview_proto, &proto));
  DataViewObject* v = ctx.heap.make<DataViewObject>();
  v->proto = proto;
  v->buffer = buf;
  v->byte_offset = static_cast<size_t>(offset);
  v->fixed_length = static_cast<size_t>(view_len);
  v->length_tracking = tracking;
  return Value::Obj(v);
}

Value dataview_buffer(Context& ctx, CallArgs& a) {
  DataViewObject* v = this_view(ctx, a.this_value);
  if (!v) return Value::Exception();
  return Value::Obj(v->buffer);
}

Value dataview_byte_length(Context& ctx, CallArgs& a) {
  DataViewObject* v = this_view(ctx, a.this_value);
  if (!v) return Value::Exception();
  if (v->out_of_bounds()) {
    ctx.throw_type_error("Cannot perform DataView.prototype.byteLength on a detached or out-of-bounds buffer");
    return Value::Exception();
  }
  return Value::Number(static_cast<double>(v->view_length()));
}

Value dataview_byte_offset(Context& ctx, CallArgs& a) {
  DataViewObject* v = this_view(ctx, a.this_value);
  if (!v) return Value::Exception();
  if (v->out_of_bounds()) {
    ctx.throw_type_error("Cannot perform DataView.prototype.byteOffset on a detached or out-of-bounds buffer");
    return Value::Exception();
  }
  return Value::Number(static_cast<double>(v->byte_offset));
}

Value dataview_get(Context& ctx, CallArgs& a, int kind) {
  DataViewObject* v = this_view(ctx, a.this_value);
  if (!v) return Value::Exception();
  uint64_t idx;
  JS_TRY(ctx.to_index(a[0], &idx));
  const bool little = Context::to_boolean(a[1]);
  if (v->out_of_bounds()) {
    ctx.throw_type_error("Cannot perform DataView get on a detached or out-of-bounds buffer");
    return Value::Exception();
  }
  const size_t size = static_cast<size_t>(kKindSizes[kind]);
  if (idx + size > v->view_length()) {
    ctx.throw_range_error("Offset is outside the bounds of the DataView");
    return Value::Exception();
  }
  uint8_t bytes[8];
  std::memcpy(bytes, v->buffer->data.data() + v->byte_offset + idx, size);
  if (!little) std::reverse(bytes, bytes + size);   // the host is little-endian
  if (kind >= kFirstBigKind) return Value::Big(ctx.new_bigint(read_big_element(bytes, kind)));
  return Value::Number(read_element(bytes, kind));
}

Value dataview_set(Context& ctx, CallArgs& a, int kind) {
  DataViewObject* v = this_view(ctx, a.this_value);
  if (!v) return Value::Exception();
  uint64_t idx;
  JS_TRY(ctx.to_index(a[0], &idx));
  double n = 0;
  BigInt* big_v = nullptr;
  if (kind >= kFirstBigKind) JS_TRY(ctx.to_bigint(a[1], &big_v));
  else JS_TRY(ctx.to_number(a[1], &n));
  const bool little = Context::to_boolean(a[2]);
  if (v->out_of_bounds()) {
    ctx.throw_type_error("Cannot perform DataView set on a detached or out-of-bounds buffer");
    return Value::Exception();
  }
  const size_t size = static_cast<size_t>(kKindSizes[kind]);
  if (idx + size > v->view_length()) {
    ctx.throw_range_error("Offset is outside the bounds of the DataView");
    return Value::Exception();
  }
  uint8_t bytes[8];
  if (big_v) write_big_element(bytes, big_v->n);
  else write_element(bytes, kind, n);
  if (!little) std::reverse(bytes, bytes + size);
  std::memcpy(v->buffer->data.data() + v->byte_offset + idx, bytes, size);
  return Value::Undefined();
}

#define DV(name, kind)                                                                \
  Value dataview_get_##name(Context& ctx, CallArgs& a) { return dataview_get(ctx, a, kind); } \
  Value dataview_set_##name(Context& ctx, CallArgs& a) { return dataview_set(ctx, a, kind); }
DV(int8, 0) DV(uint8, 1) DV(int16, 3) DV(uint16, 4) DV(int32, 5) DV(uint32, 6) DV(float32, 7) DV(float64, 8) DV(bigint64, 9) DV(biguint64, 10)
#undef DV

// ---------------------------------------------------------------------------------------------------
// TextEncoder / TextDecoder
// ---------------------------------------------------------------------------------------------------

Value make_uint8_array(Context& ctx, const std::vector<uint8_t>& bytes) {
  TypedArrayObject* t = new_typed_array(ctx, 1, bytes.size());
  if (!bytes.empty()) std::memcpy(t->buffer->data.data(), bytes.data(), bytes.size());
  return Value::Obj(t);
}

void utf8_encode(const std::u16string& s, std::vector<uint8_t>* out) {
  for (size_t i = 0; i < s.size();) {
    int len;
    uint32_t cp = uni::code_point_at(s, i, &len);
    i += static_cast<size_t>(len);
    if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
    if (cp < 0x80) out->push_back(static_cast<uint8_t>(cp));
    else if (cp < 0x800) { out->push_back(static_cast<uint8_t>(0xC0 | (cp >> 6))); out->push_back(static_cast<uint8_t>(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) { out->push_back(static_cast<uint8_t>(0xE0 | (cp >> 12))); out->push_back(static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F))); out->push_back(static_cast<uint8_t>(0x80 | (cp & 0x3F))); }
    else { out->push_back(static_cast<uint8_t>(0xF0 | (cp >> 18))); out->push_back(static_cast<uint8_t>(0x80 | ((cp >> 12) & 0x3F))); out->push_back(static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F))); out->push_back(static_cast<uint8_t>(0x80 | (cp & 0x3F))); }
  }
}

Value text_encoder_ctor(Context& ctx, CallArgs& a) {
  if (!a.is_construct()) {
    ctx.throw_type_error("Class constructor TextEncoder cannot be invoked without 'new'");
    return Value::Exception();
  }
  Object* proto;
  JS_TRY(ctx.prototype_from_ctor(a.new_target, ctx.intr.object_proto, &proto));
  Value pv;
  JS_TRY(a.callee->get(ctx, ctx.atoms.prototype, Value::Obj(a.callee), &pv));
  Object* o = ctx.new_object(a.new_target.is_object() && a.new_target.as_object() != a.callee ? proto : pv.as_object());
  return Value::Obj(o);
}

Value text_encoder_encoding(Context& ctx, CallArgs&) { return Str(ctx, "utf-8"); }

Value text_encoder_encode(Context& ctx, CallArgs& a) {
  std::u16string s;
  if (!a[0].is_undefined()) {
    JsString* js;
    JS_TRY(ctx.to_string(a[0], &js));
    s = js->str();
  }
  std::vector<uint8_t> bytes;
  utf8_encode(s, &bytes);
  return make_uint8_array(ctx, bytes);
}

Value text_encoder_encode_into(Context& ctx, CallArgs& a) {
  JsString* js;
  JS_TRY(ctx.to_string(a[0], &js));
  if (!a[1].is_object() || a[1].as_object()->cls != Class::TypedArray || static_cast<TypedArrayObject*>(a[1].as_object())->kind != 1) {
    ctx.throw_type_error("The \"dest\" argument must be an instance of Uint8Array.");
    return Value::Exception();
  }
  TypedArrayObject* dest = static_cast<TypedArrayObject*>(a[1].as_object());
  const std::u16string& s = js->str();
  size_t read = 0, written = 0;
  const size_t cap = dest->length();
  for (size_t i = 0; i < s.size();) {
    int len;
    uint32_t cp = uni::code_point_at(s, i, &len);
    std::vector<uint8_t> enc;
    std::u16string one;
    uni::append_code_point(one, cp);
    utf8_encode(one, &enc);
    if (written + enc.size() > cap) break;
    for (uint8_t b : enc) dest->set_element(written++, static_cast<double>(b));
    i += static_cast<size_t>(len);
    read += static_cast<size_t>(len);
  }
  Object* r = ctx.new_object();
  ctx.define_data(r, ctx.atom_ascii("read"), Value::Number(static_cast<double>(read)));
  ctx.define_data(r, ctx.atom_ascii("written"), Value::Number(static_cast<double>(written)));
  return Value::Obj(r);
}

struct TextDecoderObject final : Object {
  TextDecoderObject() : Object(Class::TextDecoder) {}
  std::string encoding = "utf-8";
  bool fatal = false, ignore_bom = false;
  std::vector<uint8_t> pending;   // an incomplete sequence carried across decode(..., {stream: true}) calls
  bool bom_seen = false;
};

TextDecoderObject* this_decoder(Context& ctx, const Value& v) {
  if (v.is_object() && v.as_object()->cls == Class::TextDecoder) return static_cast<TextDecoderObject*>(v.as_object());
  ctx.throw_type_error("Value of \"this\" must be of type TextDecoder");
  return nullptr;
}

Value text_decoder_ctor(Context& ctx, CallArgs& a) {
  if (!a.is_construct()) {
    ctx.throw_type_error("Class constructor TextDecoder cannot be invoked without 'new'");
    return Value::Exception();
  }
  std::string label = "utf-8";
  if (!a[0].is_undefined()) {
    JsString* s;
    JS_TRY(ctx.to_string(a[0], &s));
    label = s->utf8();
  }
  std::string l;
  for (char c : label) if (c != ' ') l.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  std::string enc;
  if (l == "utf-8" || l == "utf8" || l == "unicode-1-1-utf-8") enc = "utf-8";
  else if (l == "utf-16le" || l == "utf-16" || l == "ucs-2") enc = "utf-16le";
  else if (l == "utf-16be") enc = "utf-16be";
  else if (l == "latin1" || l == "iso-8859-1" || l == "windows-1252" || l == "ascii" || l == "us-ascii" || l == "l1") enc = "windows-1252";
  else {
    ctx.throw_range_error("The \"" + label + "\" encoding is not supported");
    return Value::Exception();
  }
  Object* proto;
  JS_TRY(ctx.prototype_from_ctor(a.new_target, ctx.intr.object_proto, &proto));
  Value pv;
  JS_TRY(a.callee->get(ctx, ctx.atoms.prototype, Value::Obj(a.callee), &pv));
  TextDecoderObject* d = ctx.heap.make<TextDecoderObject>();
  d->proto = a.new_target.is_object() && a.new_target.as_object() != a.callee ? proto : pv.as_object();
  d->encoding = enc;
  if (a[1].is_object()) {
    Value f, b;
    JS_TRY(a[1].as_object()->get(ctx, ctx.atom_ascii("fatal"), a[1], &f));
    JS_TRY(a[1].as_object()->get(ctx, ctx.atom_ascii("ignoreBOM"), a[1], &b));
    d->fatal = Context::to_boolean(f);
    d->ignore_bom = Context::to_boolean(b);
  }
  return Value::Obj(d);
}

Value text_decoder_encoding(Context& ctx, CallArgs& a) {
  TextDecoderObject* d = this_decoder(ctx, a.this_value);
  if (!d) return Value::Exception();
  return Str(ctx, d->encoding == "windows-1252" ? "windows-1252" : d->encoding);
}
Value text_decoder_fatal(Context& ctx, CallArgs& a) {
  TextDecoderObject* d = this_decoder(ctx, a.this_value);
  if (!d) return Value::Exception();
  return Value::Bool(d->fatal);
}
Value text_decoder_ignore_bom(Context& ctx, CallArgs& a) {
  TextDecoderObject* d = this_decoder(ctx, a.this_value);
  if (!d) return Value::Exception();
  return Value::Bool(d->ignore_bom);
}

bool bytes_of(Context& ctx, const Value& v, std::vector<uint8_t>* out) {
  if (v.is_undefined()) return true;
  if (v.is_object()) {
    Object* o = v.as_object();
    if (o->cls == Class::ArrayBuffer) {
      *out = static_cast<ArrayBufferObject*>(o)->data;
      return true;
    }
    if (o->cls == Class::TypedArray) {
      TypedArrayObject* t = static_cast<TypedArrayObject*>(o);
      const size_t n = t->length() * static_cast<size_t>(t->element_size());
      if (n) out->assign(t->buffer->data.begin() + static_cast<long>(t->byte_offset), t->buffer->data.begin() + static_cast<long>(t->byte_offset + n));
      return true;
    }
    if (o->cls == Class::DataView) {
      DataViewObject* d = static_cast<DataViewObject*>(o);
      const size_t n = d->view_length();
      if (n) out->assign(d->buffer->data.begin() + static_cast<long>(d->byte_offset), d->buffer->data.begin() + static_cast<long>(d->byte_offset + n));
      return true;
    }
  }
  ctx.throw_type_error("The \"input\" argument must be an instance of ArrayBuffer or ArrayBufferView.");
  return false;
}

Value text_decoder_decode(Context& ctx, CallArgs& a) {
  TextDecoderObject* d = this_decoder(ctx, a.this_value);
  if (!d) return Value::Exception();
  std::vector<uint8_t> in, fresh;
  JS_TRY(bytes_of(ctx, a[0], &fresh));
  // an incomplete sequence kept from the last streaming call comes first (bytes_of fills its output, it does not append)
  in.swap(d->pending);
  d->pending.clear();
  in.insert(in.end(), fresh.begin(), fresh.end());
  bool stream = false;
  if (a[1].is_object()) {
    Value s;
    JS_TRY(a[1].as_object()->get(ctx, ctx.atom_ascii("stream"), a[1], &s));
    stream = Context::to_boolean(s);
  }
  std::u16string out;
  if (d->encoding == "utf-8") {
    size_t i = 0;
    const size_t n = in.size();
    auto bad = [&]() -> bool {
      if (d->fatal) {
        ctx.throw_type_error("The encoded data was not valid for encoding utf-8");
        return false;
      }
      out.push_back(0xFFFD);
      return true;
    };
    while (i < n) {
      const uint8_t b = in[i];
      if (b < 0x80) { out.push_back(b); ++i; continue; }
      int need;
      uint32_t cp;
      uint32_t lo = 0x80, hi = 0xBF;
      if (b >= 0xC2 && b <= 0xDF) { need = 1; cp = b & 0x1F; }
      else if (b >= 0xE0 && b <= 0xEF) { need = 2; cp = b & 0x0F; if (b == 0xE0) lo = 0xA0; if (b == 0xED) hi = 0x9F; }
      else if (b >= 0xF0 && b <= 0xF4) { need = 3; cp = b & 0x07; if (b == 0xF0) lo = 0x90; if (b == 0xF4) hi = 0x8F; }
      else { if (!bad()) return Value::Exception(); ++i; continue; }
      size_t j = i + 1;
      bool ok = true;
      int got = 0;
      for (; got < need; ++got, ++j) {
        if (j >= n) break;
        const uint8_t c = in[j];
        const uint32_t clo = got == 0 ? lo : 0x80, chi = got == 0 ? hi : 0xBF;
        if (c < clo || c > chi) { ok = false; break; }
        cp = (cp << 6) | (c & 0x3F);
      }
      if (got < need && ok && j >= n) {
        // truncated at the end of the input
        if (stream) { d->pending.assign(in.begin() + static_cast<long>(i), in.end()); i = n; break; }
        if (!bad()) return Value::Exception();
        i = n;
        break;
      }
      if (!ok) {
        if (!bad()) return Value::Exception();
        i = j;   // resume at the offending byte
        continue;
      }
      uni::append_code_point(out, cp);
      i = j;
    }
  } else if (d->encoding == "utf-16le" || d->encoding == "utf-16be") {
    const bool be = d->encoding == "utf-16be";
    size_t n = in.size() & ~static_cast<size_t>(1);
    if (stream && (in.size() & 1)) d->pending.push_back(in.back());
    for (size_t i = 0; i < n; i += 2) out.push_back(static_cast<char16_t>(be ? (in[i] << 8) | in[i + 1] : (in[i + 1] << 8) | in[i]));
    if (!stream && (in.size() & 1)) out.push_back(0xFFFD);
  } else {
    static const uint16_t cp1252[32] = {0x20AC, 0x81, 0x201A, 0x192, 0x201E, 0x2026, 0x2020, 0x2021, 0x2C6, 0x2030, 0x160, 0x2039, 0x152, 0x8D, 0x17D, 0x8F,
                                        0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x2DC, 0x2122, 0x161, 0x203A, 0x153, 0x9D, 0x17E, 0x178};
    for (uint8_t b : in) out.push_back(b >= 0x80 && b < 0xA0 ? cp1252[b - 0x80] : b);
  }
  if (!d->ignore_bom && !d->bom_seen && !out.empty()) {
    if (out[0] == 0xFEFF) out.erase(out.begin());
    d->bom_seen = true;
  }
  if (!stream) d->bom_seen = false;
  return StrV(ctx, std::move(out));
}

}  // namespace

Object* Context::new_array_buffer(const uint8_t* data, size_t length) {
  ArrayBufferObject* b = new_buffer(*this, length);
  if (length && data) std::memcpy(b->data.data(), data, length);
  return b;
}

Object* Context::new_uint8_array(const uint8_t* data, size_t length) {
  TypedArrayObject* t = new_typed_array(*this, 1, length);
  if (length && data) std::memcpy(t->buffer->data.data(), data, length);
  return t;
}

bool Context::bytes_of(const Value& v, std::string* out) {
  if (!v.is_object()) return false;
  Object* o = v.as_object();
  if (o->cls == Class::ArrayBuffer) {
    ArrayBufferObject* b = static_cast<ArrayBufferObject*>(o);
    out->assign(reinterpret_cast<const char*>(b->data.data()), b->data.size());
    return true;
  }
  if (o->cls == Class::TypedArray) {
    TypedArrayObject* t = static_cast<TypedArrayObject*>(o);
    const size_t n = t->length() * static_cast<size_t>(t->element_size());
    out->assign(n ? reinterpret_cast<const char*>(t->buffer->data.data() + t->byte_offset) : "", n);
    return true;
  }
  if (o->cls == Class::DataView) {
    DataViewObject* d = static_cast<DataViewObject*>(o);
    const size_t n = d->view_length();
    out->assign(n ? reinterpret_cast<const char*>(d->buffer->data.data() + d->byte_offset) : "", n);
    return true;
  }
  return false;
}

void setup_typedarray(Context& ctx) {
  // ArrayBuffer
  Object* abp = ctx.new_object();
  ctx.intr.arraybuffer_proto = abp;
  Function* abc = new_ctor(ctx, "ArrayBuffer", arraybuffer_ctor, 1, abp);
  ctx.intr.arraybuffer_ctor = abc;
  ctx.define_method(abc, "isView", arraybuffer_is_view, 1);
  ctx.define_getter(abc, ctx.sym.species, "[Symbol.species]", arraybuffer_species);
  ctx.define_getter(abp, "byteLength", arraybuffer_byte_length);
  ctx.define_getter(abp, "detached", arraybuffer_detached);
  ctx.define_getter(abp, "maxByteLength", arraybuffer_max_byte_length);
  ctx.define_getter(abp, "resizable", arraybuffer_resizable);
  ctx.define_method(abp, "resize", arraybuffer_resize, 1);
  ctx.define_method(abp, "slice", arraybuffer_slice, 2);
  ctx.define_method(abp, "transfer", arraybuffer_transfer, 0);
  ctx.define_method(abp, "transferToFixedLength", arraybuffer_transfer_fixed, 0);
  ctx.define_data(abp, ctx.sym.to_string_tag, Str(ctx, "ArrayBuffer"), kConfigurable);

  // %TypedArray%
  Object* tap = ctx.new_object();
  ctx.intr.typedarray_proto = tap;
  Function* tac = ctx.new_native("TypedArray", abstract_typed_array_ctor, 0, true);
  ctx.define_data(tac, ctx.atoms.prototype, Value::Obj(tap), 0);
  ctx.define_data(tap, ctx.atoms.constructor, Value::Obj(tac), kHiddenAttrs);
  ctx.define_method(tac, "from", typed_array_from, 1);
  ctx.define_method(tac, "of", typed_array_of, 0);
  ctx.define_getter(tac, ctx.sym.species, "[Symbol.species]", arraybuffer_species);
  ctx.define_getter(tap, "buffer", ta_buffer);
  ctx.define_getter(tap, "byteLength", ta_byte_length);
  ctx.define_getter(tap, "byteOffset", ta_byte_offset);
  ctx.define_getter(tap, "length", ta_length);
  ctx.define_getter(tap, ctx.sym.to_string_tag, "[Symbol.toStringTag]", ta_tag_getter);
  struct M { const char* n; NativeFn fn; int len; } methods[] = {
      {"at", ta_at, 1}, {"copyWithin", ta_copy_within, 2}, {"entries", ta_entries, 0}, {"every", ta_every, 1}, {"fill", ta_fill, 1},
      {"filter", ta_filter, 1}, {"find", ta_find, 1}, {"findIndex", ta_find_index, 1}, {"findLast", ta_find_last, 1},
      {"findLastIndex", ta_find_last_index, 1}, {"forEach", ta_for_each, 1}, {"includes", ta_includes, 1}, {"indexOf", ta_index_of, 1},
      {"join", ta_join, 1}, {"keys", ta_keys, 0}, {"lastIndexOf", ta_last_index_of, 1}, {"map", ta_map, 1}, {"reduce", ta_reduce, 1},
      {"reduceRight", ta_reduce_right, 1}, {"reverse", ta_reverse, 0}, {"set", ta_set, 1}, {"slice", ta_slice, 2}, {"some", ta_some, 1},
      {"sort", ta_sort, 1}, {"subarray", ta_subarray, 2}, {"toLocaleString", ta_to_locale_string, 0}, {"toReversed", ta_to_reversed, 0},
      {"toSorted", ta_to_sorted, 1}, {"with", ta_with, 2}};
  for (const M& m : methods) ctx.define_method(tap, m.n, m.fn, m.len);
  Function* values = ctx.define_method(tap, "values", ta_values, 0);
  ctx.define_data(tap, ctx.sym.iterator, Value::Obj(values), kHiddenAttrs);
  Value ats;
  ctx.get(ctx.intr.array_proto, ctx.atoms.toString, &ats);
  ctx.define_data(tap, ctx.atoms.toString, ats, kHiddenAttrs);
  for (int k = 0; k < kKinds; ++k) {
    Object* p = ctx.new_object(tap);
    ctx.intr.typedarray_protos[k] = p;
    Function* c = ctx.new_native(kKindNames[k], kKindCtors[k], 3, true);
    c->proto = tac;
    install_ctor(ctx, c, p, kKindNames[k]);
    ctx.intr.typedarray_ctors[k] = c;
    ctx.define_data(c, ctx.atom_ascii("BYTES_PER_ELEMENT"), Value::Number(kKindSizes[k]), 0);
    ctx.define_data(p, ctx.atom_ascii("BYTES_PER_ELEMENT"), Value::Number(kKindSizes[k]), 0);
  }

  // DataView
  Object* dvp = ctx.new_object();
  ctx.intr.dataview_proto = dvp;
  new_ctor(ctx, "DataView", dataview_ctor, 1, dvp);
  ctx.define_getter(dvp, "buffer", dataview_buffer);
  ctx.define_getter(dvp, "byteLength", dataview_byte_length);
  ctx.define_getter(dvp, "byteOffset", dataview_byte_offset);
  struct D { const char* n; NativeFn fn; int len; } dv[] = {
      {"getInt8", dataview_get_int8, 1}, {"getUint8", dataview_get_uint8, 1}, {"getInt16", dataview_get_int16, 1}, {"getUint16", dataview_get_uint16, 1},
      {"getInt32", dataview_get_int32, 1}, {"getUint32", dataview_get_uint32, 1}, {"getFloat32", dataview_get_float32, 1}, {"getFloat64", dataview_get_float64, 1},
      {"setInt8", dataview_set_int8, 2}, {"setUint8", dataview_set_uint8, 2}, {"setInt16", dataview_set_int16, 2}, {"setUint16", dataview_set_uint16, 2},
      {"setInt32", dataview_set_int32, 2}, {"setUint32", dataview_set_uint32, 2}, {"setFloat32", dataview_set_float32, 2}, {"setFloat64", dataview_set_float64, 2},
      {"getBigInt64", dataview_get_bigint64, 1}, {"getBigUint64", dataview_get_biguint64, 1}, {"setBigInt64", dataview_set_bigint64, 2}, {"setBigUint64", dataview_set_biguint64, 2}};
  for (const D& d : dv) ctx.define_method(dvp, d.n, d.fn, d.len);
  ctx.define_data(dvp, ctx.sym.to_string_tag, Str(ctx, "DataView"), kConfigurable);

  // TextEncoder / TextDecoder
  Object* tep = ctx.new_object();
  new_ctor(ctx, "TextEncoder", text_encoder_ctor, 0, tep);
  ctx.define_getter(tep, "encoding", text_encoder_encoding);
  ctx.define_method(tep, "encode", text_encoder_encode, 0);
  ctx.define_method(tep, "encodeInto", text_encoder_encode_into, 2);
  ctx.define_data(tep, ctx.sym.to_string_tag, Str(ctx, "TextEncoder"), kConfigurable);
  Object* tdp = ctx.new_object();
  new_ctor(ctx, "TextDecoder", text_decoder_ctor, 0, tdp);
  ctx.define_getter(tdp, "encoding", text_decoder_encoding);
  ctx.define_getter(tdp, "fatal", text_decoder_fatal);
  ctx.define_getter(tdp, "ignoreBOM", text_decoder_ignore_bom);
  ctx.define_method(tdp, "decode", text_decoder_decode, 0);
  ctx.define_data(tdp, ctx.sym.to_string_tag, Str(ctx, "TextDecoder"), kConfigurable);
}

}  // namespace aios::js
