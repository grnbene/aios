#pragma once
// Engine-internal declarations shared by the VM, the compiler and the builtins.
#include "aios/js/context.hpp"

namespace aios::js {

// An object with a primitive inside: new Number(1), new String("a"), new Boolean(true), Object(Symbol()).
struct PrimitiveObject : Object {
  PrimitiveObject(Class c, Value v) : Object(c), prim(v) {}
  Value prim;
  void trace(Tracer& t) override { Object::trace(t); t.mark(prim); }
};

// new String("abc"): indexed characters and `length` are own, read-only properties.
struct StringObject final : PrimitiveObject {
  explicit StringObject(JsString* s) : PrimitiveObject(Class::String, Value::String(s)) { exotic_props = true; has_index_props = true; }
  bool get_own_property(Context& ctx, PropKey k, PropDesc* out, bool* found) override;
  bool define_own_property(Context& ctx, PropKey k, const PropDesc& d, bool* ok) override;
  bool delete_property(Context& ctx, PropKey k, bool* ok) override;
  bool own_keys(Context& ctx, std::vector<PropKey>* out) override;
};

// --- the VM (vm.cpp) ---
void vm_init(Context& ctx);
void vm_shutdown(Context& ctx);
void vm_trace(Context& ctx, Tracer& t);
bool vm_call_closure(Context& ctx, Function* f, const Value& this_v, const Value* argv, int argc, const Value& new_target, Value* out);
// "    at fn (file:line:col)" lines for the current call stack, newest first.
std::string vm_stack_trace(Context& ctx, int skip_frames);

// --- builtins: each installs its constructor, prototype and statics ---
void setup_object(Context& ctx);
void setup_function(Context& ctx);
void setup_error(Context& ctx);
void setup_array(Context& ctx);
void setup_string(Context& ctx);
void setup_number(Context& ctx);
void setup_boolean(Context& ctx);
void setup_bigint(Context& ctx);
void setup_symbol(Context& ctx);
void setup_math(Context& ctx);
void setup_json(Context& ctx);
void setup_global_functions(Context& ctx);
void setup_iterators(Context& ctx);
void setup_generator(Context& ctx);
void setup_promise(Context& ctx);
void setup_collections(Context& ctx);
void setup_regexp(Context& ctx);
void setup_date(Context& ctx);
void setup_typedarray(Context& ctx);
void setup_reflect(Context& ctx);
void setup_proxy(Context& ctx);
void setup_misc(Context& ctx);

void promise_report_unhandled(Context& ctx);   // after the job queue drains: tell the host about rejections nobody handled
// The object a Proxy forwards to (*revoked is true, and null returned, once it has been revoked).
Object* proxy_target_of(Object* proxy, bool* revoked);
bool object_freeze(Context& ctx, Object* o);   // SetIntegrityLevel(o, frozen); false with an exception pending

// helpers the builtins share
bool this_number_value(Context& ctx, const Value& v, const char* method, double* out);
Object* make_wrapper(Context& ctx, Value prim);
JsString* ascii(Context& ctx, const char* s);
void install_ctor(Context& ctx, Function* ctor, Object* proto, std::string_view name);
Function* new_ctor(Context& ctx, std::string_view name, NativeFn fn, int length, Object* proto);

}  // namespace aios::js
