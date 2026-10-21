// Error and its subclasses.
#include "builtins.hpp"

namespace aios::js {

namespace {

const char* const kNames[] = {"Error", "TypeError", "RangeError", "ReferenceError", "SyntaxError", "EvalError", "URIError", "AggregateError"};

// "Name: message", the first line of a stack.
std::string error_header(Context& ctx, Object* e) {
  std::string name = "Error", msg;
  Value v;
  if (e->get(ctx, ctx.atoms.name, Value::Obj(e), &v) && v.is_string()) name = v.as_string()->utf8();
  else ctx.clear_exception();
  if (e->get(ctx, ctx.atoms.message, Value::Obj(e), &v) && v.is_string()) msg = v.as_string()->utf8();
  else ctx.clear_exception();
  if (msg.empty()) return name;
  if (name.empty()) return msg;
  return name + ": " + msg;
}

void install_stack(Context& ctx, Object* e) {
  std::string st = error_header(ctx, e);
  std::string trace = vm_stack_trace(ctx, 0);
  if (!trace.empty()) st += "\n" + trace;
  ctx.define_data(e, ctx.atoms.stack, Value::String(ctx.new_string_utf8(st)), kHiddenAttrs);
}

Value make_error(Context& ctx, CallArgs& a, ErrorKind kind) {
  Value nt = a.is_construct() ? a.new_target : Value::Obj(a.callee);
  Object* proto;
  JS_TRY(ctx.prototype_from_ctor(nt, ctx.intr.error_protos[static_cast<int>(kind)], &proto));
  Object* e = ctx.heap.make<Object>(Class::Error);
  e->proto = proto;
  const bool aggregate = kind == ErrorKind::AggregateError;
  const Value message = aggregate ? a[1] : a[0];
  const Value options = aggregate ? a[2] : a[1];
  if (!message.is_undefined()) {
    JsString* m;
    JS_TRY(ctx.to_string(message, &m));
    ctx.define_data(e, ctx.atoms.message, Value::String(m), kHiddenAttrs);
  }
  if (options.is_object()) {
    bool has;
    JS_TRY(options.as_object()->has_property(ctx, ctx.atoms.cause, &has));
    if (has) {
      Value c;
      JS_TRY(options.as_object()->get(ctx, ctx.atoms.cause, options, &c));
      ctx.define_data(e, ctx.atoms.cause, c, kHiddenAttrs);
    }
  }
  if (aggregate) {
    std::vector<Value> list;
    JS_TRY(ctx.iterable_to_list(a[0], &list));
    ctx.define_data(e, ctx.atoms.errors, Value::Obj(ctx.new_array_from(list)), kHiddenAttrs);
  }
  install_stack(ctx, e);
  return Value::Obj(e);
}

#define ERROR_CTOR(fn, kind) \
  Value fn(Context& ctx, CallArgs& a) { return make_error(ctx, a, kind); }
ERROR_CTOR(error_ctor, ErrorKind::Error)
ERROR_CTOR(type_error_ctor, ErrorKind::TypeError)
ERROR_CTOR(range_error_ctor, ErrorKind::RangeError)
ERROR_CTOR(reference_error_ctor, ErrorKind::ReferenceError)
ERROR_CTOR(syntax_error_ctor, ErrorKind::SyntaxError)
ERROR_CTOR(eval_error_ctor, ErrorKind::EvalError)
ERROR_CTOR(uri_error_ctor, ErrorKind::URIError)
ERROR_CTOR(aggregate_error_ctor, ErrorKind::AggregateError)
#undef ERROR_CTOR

Value error_to_string(Context& ctx, CallArgs& a) {
  if (!a.this_value.is_object()) {
    ctx.throw_type_error("Error.prototype.toString requires that 'this' be an Object");
    return Value::Exception();
  }
  Object* o = a.this_value.as_object();
  Value n, m;
  JS_TRY(o->get(ctx, ctx.atoms.name, a.this_value, &n));
  JS_TRY(o->get(ctx, ctx.atoms.message, a.this_value, &m));
  JsString *ns, *ms;
  if (n.is_undefined()) ns = ctx.atom_ascii("Error");
  else JS_TRY(ctx.to_string(n, &ns));
  if (m.is_undefined()) ms = ctx.atoms.empty;
  else JS_TRY(ctx.to_string(m, &ms));
  if (ns->length == 0) return Value::String(ms);
  if (ms->length == 0) return Value::String(ns);
  return StrV(ctx, ns->str() + u": " + ms->str());
}

Value error_capture_stack_trace(Context& ctx, CallArgs& a) {
  if (!a[0].is_object()) {
    ctx.throw_type_error("Invalid argument");
    return Value::Exception();
  }
  install_stack(ctx, a[0].as_object());
  return Value::Undefined();
}

}  // namespace

void setup_error(Context& ctx) {
  NativeFn ctors[] = {error_ctor, type_error_ctor, range_error_ctor, reference_error_ctor, syntax_error_ctor, eval_error_ctor, uri_error_ctor, aggregate_error_ctor};
  Function* base = nullptr;
  for (int i = 0; i < 8; ++i) {
    Object* proto = ctx.new_object(i == 0 ? ctx.intr.object_proto : ctx.intr.error_protos[0]);
    ctx.intr.error_protos[i] = proto;
    if (i == 0) ctx.intr.error_proto = proto;
    Function* c = new_ctor(ctx, kNames[i], ctors[i], i == 7 ? 2 : 1, proto);
    if (i == 0) base = c;
    else c->proto = base;
    ctx.define_data(proto, ctx.atoms.name, Str(ctx, kNames[i]), kHiddenAttrs);
    ctx.define_data(proto, ctx.atoms.message, Value::String(ctx.atoms.empty), kHiddenAttrs);
  }
  ctx.define_method(ctx.intr.error_proto, "toString", error_to_string, 0);
  ctx.define_method(base, "captureStackTrace", error_capture_stack_trace, 2);
  ctx.define_data(base, ctx.atom_ascii("stackTraceLimit"), Value::Number(10));
}

}  // namespace aios::js
