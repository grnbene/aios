#pragma once
// ArrayBuffer and typed-array objects, shared by the builtins and the VM's indexed-access fast path.
#include <vector>

#include "internal.hpp"

namespace aios::js {

struct ArrayBufferObject final : Object {
  ArrayBufferObject() : Object(Class::ArrayBuffer) {}
  std::vector<uint8_t> data;
  size_t max_len = 0;
  bool resizable = false;
  bool detached = false;
  size_t extra_bytes_hint() const { return data.capacity(); }
  size_t extra_bytes() const override { return Object::extra_bytes() + data.capacity(); }
};

struct TypedArrayObject final : Object {
  explicit TypedArrayObject(int k) : Object(Class::TypedArray), kind(k) {
    exotic_props = true;
    exotic_get = true;
    has_index_props = true;
  }
  int kind;                           // 0 Int8, 1 Uint8, 2 Uint8Clamped, 3 Int16, 4 Uint16, 5 Int32, 6 Uint32, 7 Float32, 8 Float64, 9 BigInt64, 10 BigUint64
  ArrayBufferObject* buffer = nullptr;
  size_t byte_offset = 0;
  size_t fixed_length = 0;
  bool length_tracking = false;       // a view of a resizable buffer without an explicit length

  size_t length() const;              // 0 when detached or out of bounds
  bool out_of_bounds() const;
  int element_size() const;
  bool is_bigint_kind() const { return kind >= 9; }   // elements are BigInts, not Numbers
  bool get_element(Context& ctx, size_t i, Value* out) const;
  // `v` is already of the element type: a Number, or a BigInt for the two BigInt kinds (see coerce_element).
  void set_element(size_t i, const Value& v);
  void set_element(size_t i, double v) { set_element(i, Value::Number(v)); }

  void trace(Tracer& t) override;
  bool get_own_property(Context& ctx, PropKey k, PropDesc* out, bool* found) override;
  bool has_property(Context& ctx, PropKey k, bool* out) override;
  bool define_own_property(Context& ctx, PropKey k, const PropDesc& d, bool* ok) override;
  bool get(Context& ctx, PropKey k, const Value& receiver, Value* out) override;
  bool set(Context& ctx, PropKey k, const Value& v, const Value& receiver, bool* ok) override;
  bool delete_property(Context& ctx, PropKey k, bool* ok) override;
  bool own_keys(Context& ctx, std::vector<PropKey>* out) override;
};

// ToNumber, or ToBigInt for the BigInt kinds: the value a store converts to before it writes (it may run script).
bool coerce_element(Context& ctx, const TypedArrayObject* t, const Value& v, Value* out);

// Integer-keyed element access for the interpreter: true when `index` names an element (and *out is set).
bool typed_array_get(Context& ctx, Object* o, double index, Value* out);
// Store a number-keyed element: *handled says the key was an integer index (valid or not) and the store is complete.
bool typed_array_set(Context& ctx, Object* o, double index, const Value& v, bool* handled);

}  // namespace aios::js
