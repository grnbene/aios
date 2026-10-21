// BigInt: the constructor function, asIntN/asUintN, and the prototype.
#include <cmath>

#include "builtins.hpp"

namespace aios::js {

namespace {

bool this_bigint(Context& ctx, const Value& v, const char* method, BigInt** out) {
  if (v.is_bigint()) { *out = v.as_bigint(); return true; }
  if (v.is_object() && v.as_object()->cls == Class::BigInt) {
    *out = static_cast<PrimitiveObject*>(v.as_object())->prim.as_bigint();
    return true;
  }
  ctx.throw_type_error(std::string(method) + " requires that 'this' be a BigInt");
  return false;
}

Value throw_big_err(Context& ctx, big::Err e) {
  switch (e) {
    case big::Err::DivideByZero: return ctx.throw_range_error("Division by zero");
    case big::Err::NegativeExponent: return ctx.throw_range_error("Exponent must be non-negative");
    default: return ctx.throw_range_error("Maximum BigInt size exceeded");
  }
}

NATIVE(bigint_ctor) {
  if (a.is_construct()) return ctx.throw_type_error("BigInt is not a constructor");
  Value prim;
  JS_TRY(ctx.to_primitive(a[0], Hint::Number, &prim));
  if (prim.is_number()) {
    const double d = prim.num;
    if (!std::isfinite(d) || std::trunc(d) != d)
      return ctx.throw_range_error("The number " + ctx.number_to_string(d)->utf8() + " cannot be converted to a BigInt because it is not an integer");
    return Value::Big(ctx.new_bigint(big::from_double(d)));
  }
  BigInt* b;
  JS_TRY(ctx.to_bigint(prim, &b));
  return Value::Big(b);
}

Value as_n(Context& ctx, CallArgs& a, bool is_signed) {
  uint64_t bits;
  JS_TRY(ctx.to_index(a[0], &bits));
  BigInt* b;
  JS_TRY(ctx.to_bigint(a[1], &b));
  BigNum r;
  const big::Err e = is_signed ? big::as_int_n(bits, b->n, &r) : big::as_uint_n(bits, b->n, &r);
  if (e != big::Err::None) return throw_big_err(ctx, e);
  return Value::Big(ctx.new_bigint(std::move(r)));
}

NATIVE(bigint_as_int_n) { return as_n(ctx, a, true); }
NATIVE(bigint_as_uint_n) { return as_n(ctx, a, false); }

NATIVE(bigint_to_string) {
  BigInt* b;
  JS_TRY(this_bigint(ctx, a.this_value, "BigInt.prototype.toString", &b));
  int radix = 10;
  if (!a[0].is_undefined()) {
    double r;
    JS_TRY(ctx.to_integer(a[0], &r));
    if (r < 2 || r > 36) return ctx.throw_range_error("toString() radix argument must be between 2 and 36");
    radix = static_cast<int>(r);
  }
  return Value::String(ctx.new_string_ascii(big::to_string(b->n, radix)));
}

// en-US: thousands separated by commas.
NATIVE(bigint_to_locale_string) {
  BigInt* b;
  JS_TRY(this_bigint(ctx, a.this_value, "BigInt.prototype.toLocaleString", &b));
  const std::string digits = big::to_string(b->n);
  std::string out;
  const size_t start = b->n.neg ? 1 : 0;
  if (start) out.push_back('-');
  for (size_t i = start; i < digits.size(); ++i) {
    if (i > start && (digits.size() - i) % 3 == 0) out.push_back(',');
    out.push_back(digits[i]);
  }
  return Value::String(ctx.new_string_ascii(out));
}

NATIVE(bigint_value_of) {
  BigInt* b;
  JS_TRY(this_bigint(ctx, a.this_value, "BigInt.prototype.valueOf", &b));
  return Value::Big(b);
}

}  // namespace

void setup_bigint(Context& ctx) {
  Object* proto = ctx.new_object(ctx.intr.object_proto);
  ctx.intr.bigint_proto = proto;
  Function* ctor = new_ctor(ctx, "BigInt", bigint_ctor, 1, proto);
  ctx.define_method(ctor, "asIntN", bigint_as_int_n, 2);
  ctx.define_method(ctor, "asUintN", bigint_as_uint_n, 2);
  ctx.define_method(proto, "toString", bigint_to_string, 0);
  ctx.define_method(proto, "toLocaleString", bigint_to_locale_string, 0);
  ctx.define_method(proto, "valueOf", bigint_value_of, 0);
  ctx.define_data(proto, ctx.sym.to_string_tag, Value::String(ctx.atom_ascii("BigInt")), kConfigurable);
}

}  // namespace aios::js
