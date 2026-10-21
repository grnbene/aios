// Proxy: every internal method forwards to a handler trap, with the invariant checks the specification requires.
#include <algorithm>

#include "builtins.hpp"

namespace aios::js {

namespace {

struct ProxyObject final : Object {
  ProxyObject() : Object(Class::Proxy) {
    exotic_props = true;
    exotic_get = true;
  }
  Object* target = nullptr;
  Object* handler = nullptr;   // null once revoked
  void trace(Tracer& t) override {
    Object::trace(t);
    t.mark(target);
    t.mark(handler);
  }
  bool get_prototype_of(Context& ctx, Object** out) override;
  bool set_prototype_of(Context& ctx, Object* p, bool* ok) override;
  bool is_extensible_obj(Context& ctx, bool* out) override;
  bool prevent_extensions(Context& ctx, bool* ok) override;
  bool get_own_property(Context& ctx, PropKey k, PropDesc* out, bool* found) override;
  bool define_own_property(Context& ctx, PropKey k, const PropDesc& d, bool* ok) override;
  bool has_property(Context& ctx, PropKey k, bool* out) override;
  bool get(Context& ctx, PropKey k, const Value& receiver, Value* out) override;
  bool set(Context& ctx, PropKey k, const Value& v, const Value& receiver, bool* ok) override;
  bool delete_property(Context& ctx, PropKey k, bool* ok) override;
  bool own_keys(Context& ctx, std::vector<PropKey>* out) override;
  bool call_internal(Context& ctx, const Value& this_v, const Value* argv, int argc, Value* out) override;
  bool construct_internal(Context& ctx, const Value* argv, int argc, const Value& new_target, Value* out) override;

  // The trap named `name`, or undefined. False with an exception when the proxy is revoked or the lookup throws.
  bool trap(Context& ctx, const char* name, Value* out) {
    if (!handler) {
      ctx.throw_type_error(std::string("Cannot perform '") + name + "' on a proxy that has been revoked");
      return false;
    }
    return ctx.get_method(Value::Obj(handler), ctx.atom_ascii(name), out);
  }
};

Value key_value(PropKey k) { return k->kind == CellKind::String ? Value::String(static_cast<JsString*>(k)) : Value::Sym(static_cast<Symbol*>(k)); }

// IsCompatiblePropertyDescriptor(extensible, desc, current)
bool compatible(Context& ctx, bool extensible, const PropDesc& d, const PropDesc* cur) {
  if (!cur) return extensible;
  if (!d.has_value && !d.has_get && !d.has_set && !d.has_writable && !d.has_enumerable && !d.has_configurable) return true;
  if (!cur->configurable) {
    if (d.has_configurable && d.configurable) return false;
    if (d.has_enumerable && d.enumerable != cur->enumerable) return false;
    const bool generic = !d.is_accessor() && !d.is_data();
    if (!generic && d.is_accessor() != cur->is_accessor()) return false;
    if (cur->is_accessor()) {
      if (d.has_get && d.getter != cur->getter) return false;
      if (d.has_set && d.setter != cur->setter) return false;
    } else if (!cur->writable) {
      if (d.has_writable && d.writable) return false;
      if (d.has_value && !ctx.same_value(d.value, cur->value)) return false;
    }
  }
  return true;
}

bool ProxyObject::get_prototype_of(Context& ctx, Object** out) {
  Value t;
  if (!trap(ctx, "getPrototypeOf", &t)) return false;
  if (t.is_undefined()) return target->get_prototype_of(ctx, out);
  Value r;
  if (!ctx.call(t, Value::Obj(handler), {Value::Obj(target)}, &r)) return false;
  if (!r.is_object() && !r.is_null()) {
    ctx.throw_type_error("'getPrototypeOf' on proxy: trap returned neither object nor null");
    return false;
  }
  bool ext;
  if (!target->is_extensible_obj(ctx, &ext)) return false;
  Object* result = r.is_object() ? r.as_object() : nullptr;
  if (!ext) {
    Object* tp;
    if (!target->get_prototype_of(ctx, &tp)) return false;
    if (tp != result) {
      ctx.throw_type_error("'getPrototypeOf' on proxy: proxy target is non-extensible but the trap did not return its actual prototype");
      return false;
    }
  }
  *out = result;
  return true;
}

bool ProxyObject::set_prototype_of(Context& ctx, Object* p, bool* ok) {
  Value t;
  if (!trap(ctx, "setPrototypeOf", &t)) return false;
  if (t.is_undefined()) return target->set_prototype_of(ctx, p, ok);
  Value r;
  if (!ctx.call(t, Value::Obj(handler), {Value::Obj(target), p ? Value::Obj(p) : Value::Null()}, &r)) return false;
  if (!Context::to_boolean(r)) { *ok = false; return true; }
  bool ext;
  if (!target->is_extensible_obj(ctx, &ext)) return false;
  if (!ext) {
    Object* tp;
    if (!target->get_prototype_of(ctx, &tp)) return false;
    if (tp != p) {
      ctx.throw_type_error("'setPrototypeOf' on proxy: trap returned truish for setting a new prototype on the non-extensible proxy target");
      return false;
    }
  }
  *ok = true;
  return true;
}

bool ProxyObject::is_extensible_obj(Context& ctx, bool* out) {
  Value t;
  if (!trap(ctx, "isExtensible", &t)) return false;
  if (t.is_undefined()) return target->is_extensible_obj(ctx, out);
  Value r;
  if (!ctx.call(t, Value::Obj(handler), {Value::Obj(target)}, &r)) return false;
  const bool b = Context::to_boolean(r);
  bool tgt;
  if (!target->is_extensible_obj(ctx, &tgt)) return false;
  if (b != tgt) {
    ctx.throw_type_error(std::string("'isExtensible' on proxy: trap result does not reflect extensibility of proxy target (which is '") + (tgt ? "true" : "false") + "')");
    return false;
  }
  *out = b;
  return true;
}

bool ProxyObject::prevent_extensions(Context& ctx, bool* ok) {
  Value t;
  if (!trap(ctx, "preventExtensions", &t)) return false;
  if (t.is_undefined()) return target->prevent_extensions(ctx, ok);
  Value r;
  if (!ctx.call(t, Value::Obj(handler), {Value::Obj(target)}, &r)) return false;
  const bool b = Context::to_boolean(r);
  if (b) {
    bool ext;
    if (!target->is_extensible_obj(ctx, &ext)) return false;
    if (ext) {
      ctx.throw_type_error("'preventExtensions' on proxy: trap returned truish but the proxy target is extensible");
      return false;
    }
  }
  *ok = b;
  return true;
}

bool ProxyObject::get_own_property(Context& ctx, PropKey k, PropDesc* out, bool* found) {
  Value t;
  if (!trap(ctx, "getOwnPropertyDescriptor", &t)) return false;
  if (t.is_undefined()) return target->get_own_property(ctx, k, out, found);
  Value r;
  if (!ctx.call(t, Value::Obj(handler), {Value::Obj(target), key_value(k)}, &r)) return false;
  if (!r.is_object() && !r.is_undefined()) {
    ctx.throw_type_error("'getOwnPropertyDescriptor' on proxy: trap returned neither object nor undefined for property '" + key_to_display(k) + "'");
    return false;
  }
  PropDesc tgt;
  bool tgt_found;
  if (!target->get_own_property(ctx, k, &tgt, &tgt_found)) return false;
  if (r.is_undefined()) {
    if (!tgt_found) { *found = false; return true; }
    if (!tgt.configurable) {
      ctx.throw_type_error("'getOwnPropertyDescriptor' on proxy: trap returned undefined for property '" + key_to_display(k) + "' which is non-configurable in the proxy target");
      return false;
    }
    bool ext;
    if (!target->is_extensible_obj(ctx, &ext)) return false;
    if (!ext) {
      ctx.throw_type_error("'getOwnPropertyDescriptor' on proxy: trap returned undefined for property '" + key_to_display(k) + "' which exists in the non-extensible proxy target");
      return false;
    }
    *found = false;
    return true;
  }
  bool ext;
  if (!target->is_extensible_obj(ctx, &ext)) return false;
  PropDesc result;
  if (!to_prop_desc(ctx, r, &result)) return false;
  // CompletePropertyDescriptor
  if (!result.is_accessor()) {
    if (!result.has_value) { result.has_value = true; result.value = Value::Undefined(); }
    if (!result.has_writable) { result.has_writable = true; result.writable = false; }
  } else {
    if (!result.has_get) { result.has_get = true; result.getter = nullptr; }
    if (!result.has_set) { result.has_set = true; result.setter = nullptr; }
  }
  if (!result.has_enumerable) { result.has_enumerable = true; result.enumerable = false; }
  if (!result.has_configurable) { result.has_configurable = true; result.configurable = false; }
  if (!compatible(ctx, ext, result, tgt_found ? &tgt : nullptr)) {
    ctx.throw_type_error("'getOwnPropertyDescriptor' on proxy: trap returned descriptor for property '" + key_to_display(k) + "' that is incompatible with the existing property in the proxy target");
    return false;
  }
  if (!result.configurable) {
    if (!tgt_found || tgt.configurable) {
      ctx.throw_type_error("'getOwnPropertyDescriptor' on proxy: trap reported non-configurability for property '" + key_to_display(k) + "' which is either non-existent or configurable in the proxy target");
      return false;
    }
    if (result.has_writable && !result.writable && tgt.writable) {
      ctx.throw_type_error("'getOwnPropertyDescriptor' on proxy: trap reported non-configurable and writable for property '" + key_to_display(k) + "' which is non-configurable, non-writable in the proxy target");
      return false;
    }
  }
  *out = result;
  *found = true;
  return true;
}

bool ProxyObject::define_own_property(Context& ctx, PropKey k, const PropDesc& d, bool* ok) {
  Value t;
  if (!trap(ctx, "defineProperty", &t)) return false;
  if (t.is_undefined()) return target->define_own_property(ctx, k, d, ok);
  // FromPropertyDescriptor
  Object* desc_obj = ctx.new_object();
  if (d.has_value) ctx.define_data(desc_obj, ctx.atoms.value, d.value);
  if (d.has_writable) ctx.define_data(desc_obj, ctx.atoms.writable, Value::Bool(d.writable));
  if (d.has_get) ctx.define_data(desc_obj, ctx.atoms.get, d.getter ? Value::Obj(d.getter) : Value::Undefined());
  if (d.has_set) ctx.define_data(desc_obj, ctx.atoms.set, d.setter ? Value::Obj(d.setter) : Value::Undefined());
  if (d.has_enumerable) ctx.define_data(desc_obj, ctx.atoms.enumerable, Value::Bool(d.enumerable));
  if (d.has_configurable) ctx.define_data(desc_obj, ctx.atoms.configurable, Value::Bool(d.configurable));
  Value r;
  if (!ctx.call(t, Value::Obj(handler), {Value::Obj(target), key_value(k), Value::Obj(desc_obj)}, &r)) return false;
  if (!Context::to_boolean(r)) { *ok = false; return true; }
  PropDesc tgt;
  bool tgt_found;
  if (!target->get_own_property(ctx, k, &tgt, &tgt_found)) return false;
  bool ext;
  if (!target->is_extensible_obj(ctx, &ext)) return false;
  const bool setting_non_configurable = d.has_configurable && !d.configurable;
  if (!tgt_found) {
    if (!ext) {
      ctx.throw_type_error("'defineProperty' on proxy: trap returned truish for adding property '" + key_to_display(k) + "'  to the non-extensible proxy target");
      return false;
    }
    if (setting_non_configurable) {
      ctx.throw_type_error("'defineProperty' on proxy: trap returned truish for defining non-configurable property '" + key_to_display(k) + "' which is either non-existent or configurable in the proxy target");
      return false;
    }
  } else {
    if (!compatible(ctx, ext, d, &tgt)) {
      ctx.throw_type_error("'defineProperty' on proxy: trap returned truish for adding property '" + key_to_display(k) + "'  that is incompatible with the existing property in the proxy target");
      return false;
    }
    if (setting_non_configurable && tgt.configurable) {
      ctx.throw_type_error("'defineProperty' on proxy: trap returned truish for defining non-configurable property '" + key_to_display(k) + "' which is either non-existent or configurable in the proxy target");
      return false;
    }
    if (!tgt.is_accessor() && !tgt.configurable && tgt.writable && d.has_writable && !d.writable) {
      ctx.throw_type_error("'defineProperty' on proxy: trap returned truish for defining non-configurable property '" + key_to_display(k) + "' which cannot be non-writable, unless there exists a corresponding non-configurable, non-writable property of the target object.");
      return false;
    }
  }
  *ok = true;
  return true;
}

bool ProxyObject::has_property(Context& ctx, PropKey k, bool* out) {
  Value t;
  if (!trap(ctx, "has", &t)) return false;
  if (t.is_undefined()) return target->has_property(ctx, k, out);
  Value r;
  if (!ctx.call(t, Value::Obj(handler), {Value::Obj(target), key_value(k)}, &r)) return false;
  const bool b = Context::to_boolean(r);
  if (!b) {
    PropDesc tgt;
    bool found;
    if (!target->get_own_property(ctx, k, &tgt, &found)) return false;
    if (found) {
      if (!tgt.configurable) {
        ctx.throw_type_error("'has' on proxy: trap returned falsish for property '" + key_to_display(k) + "' which exists in the proxy target as non-configurable");
        return false;
      }
      bool ext;
      if (!target->is_extensible_obj(ctx, &ext)) return false;
      if (!ext) {
        ctx.throw_type_error("'has' on proxy: trap returned falsish for property '" + key_to_display(k) + "' but the proxy target is not extensible");
        return false;
      }
    }
  }
  *out = b;
  return true;
}

bool ProxyObject::get(Context& ctx, PropKey k, const Value& receiver, Value* out) {
  Value t;
  if (!trap(ctx, "get", &t)) return false;
  if (t.is_undefined()) return target->get(ctx, k, receiver, out);
  Value r;
  if (!ctx.call(t, Value::Obj(handler), {Value::Obj(target), key_value(k), receiver}, &r)) return false;
  PropDesc tgt;
  bool found;
  if (!target->get_own_property(ctx, k, &tgt, &found)) return false;
  if (found && !tgt.configurable) {
    if (!tgt.is_accessor() && !tgt.writable && !ctx.same_value(r, tgt.value)) {
      ctx.throw_type_error("'get' on proxy: property '" + key_to_display(k) + "' is a read-only and non-configurable data property on the proxy target but the proxy did not return its actual value");
      return false;
    }
    if (tgt.is_accessor() && !tgt.getter && !r.is_undefined()) {
      ctx.throw_type_error("'get' on proxy: property '" + key_to_display(k) + "' is a non-configurable accessor property on the proxy target and does not have a getter function, but the trap did not return 'undefined'");
      return false;
    }
  }
  *out = r;
  return true;
}

bool ProxyObject::set(Context& ctx, PropKey k, const Value& v, const Value& receiver, bool* ok) {
  Value t;
  if (!trap(ctx, "set", &t)) return false;
  if (t.is_undefined()) return target->set(ctx, k, v, receiver, ok);
  Value r;
  if (!ctx.call(t, Value::Obj(handler), {Value::Obj(target), key_value(k), v, receiver}, &r)) return false;
  if (!Context::to_boolean(r)) { *ok = false; return true; }
  PropDesc tgt;
  bool found;
  if (!target->get_own_property(ctx, k, &tgt, &found)) return false;
  if (found && !tgt.configurable) {
    if (!tgt.is_accessor() && !tgt.writable && !ctx.same_value(v, tgt.value)) {
      ctx.throw_type_error("'set' on proxy: trap returned truish for property '" + key_to_display(k) + "' which exists in the proxy target as a non-configurable and non-writable data property with a different value");
      return false;
    }
    if (tgt.is_accessor() && !tgt.setter) {
      ctx.throw_type_error("'set' on proxy: trap returned truish for property '" + key_to_display(k) + "' which exists in the proxy target as a non-configurable and non-writable accessor property without a setter");
      return false;
    }
  }
  *ok = true;
  return true;
}

bool ProxyObject::delete_property(Context& ctx, PropKey k, bool* ok) {
  Value t;
  if (!trap(ctx, "deleteProperty", &t)) return false;
  if (t.is_undefined()) return target->delete_property(ctx, k, ok);
  Value r;
  if (!ctx.call(t, Value::Obj(handler), {Value::Obj(target), key_value(k)}, &r)) return false;
  if (!Context::to_boolean(r)) { *ok = false; return true; }
  PropDesc tgt;
  bool found;
  if (!target->get_own_property(ctx, k, &tgt, &found)) return false;
  if (found) {
    if (!tgt.configurable) {
      ctx.throw_type_error("'deleteProperty' on proxy: trap returned truish for property '" + key_to_display(k) + "' which is non-configurable in the proxy target");
      return false;
    }
    bool ext;
    if (!target->is_extensible_obj(ctx, &ext)) return false;
    if (!ext) {
      ctx.throw_type_error("'deleteProperty' on proxy: trap returned truish for property '" + key_to_display(k) + "' but the proxy target is non-extensible");
      return false;
    }
  }
  *ok = true;
  return true;
}

bool ProxyObject::own_keys(Context& ctx, std::vector<PropKey>* out) {
  Value t;
  if (!trap(ctx, "ownKeys", &t)) return false;
  if (t.is_undefined()) return target->own_keys(ctx, out);
  Value r;
  if (!ctx.call(t, Value::Obj(handler), {Value::Obj(target)}, &r)) return false;
  if (!r.is_object()) {
    ctx.throw_type_error("CreateListFromArrayLike called on non-object");
    return false;
  }
  std::vector<Value> list;
  if (!create_list_from_array_like(ctx, r, &list)) return false;
  std::vector<PropKey> keys;
  for (const Value& v : list) {
    if (!v.is_string() && !v.is_symbol()) {
      ctx.throw_type_error(vm_describe(ctx, v) + " is not a valid property name");
      return false;
    }
    PropKey k;
    if (!ctx.to_property_key(v, &k)) return false;
    if (std::find(keys.begin(), keys.end(), k) != keys.end()) {
      ctx.throw_type_error("'ownKeys' on proxy: trap returned duplicate entries");
      return false;
    }
    keys.push_back(k);
  }
  bool ext;
  if (!target->is_extensible_obj(ctx, &ext)) return false;
  std::vector<PropKey> target_keys;
  if (!target->own_keys(ctx, &target_keys)) return false;
  std::vector<PropKey> configurable_keys, nonconfigurable_keys;
  for (PropKey k : target_keys) {
    PropDesc d;
    bool found;
    if (!target->get_own_property(ctx, k, &d, &found)) return false;
    (found && !d.configurable ? nonconfigurable_keys : configurable_keys).push_back(k);
  }
  if (ext && nonconfigurable_keys.empty()) { *out = keys; return true; }
  std::vector<PropKey> unchecked = keys;
  for (PropKey k : nonconfigurable_keys) {
    auto it = std::find(unchecked.begin(), unchecked.end(), k);
    if (it == unchecked.end()) {
      ctx.throw_type_error("'ownKeys' on proxy: trap result did not include '" + key_to_display(k) + "'");
      return false;
    }
    unchecked.erase(it);
  }
  if (ext) { *out = keys; return true; }
  for (PropKey k : configurable_keys) {
    auto it = std::find(unchecked.begin(), unchecked.end(), k);
    if (it == unchecked.end()) {
      ctx.throw_type_error("'ownKeys' on proxy: trap result did not include '" + key_to_display(k) + "'");
      return false;
    }
    unchecked.erase(it);
  }
  if (!unchecked.empty()) {
    ctx.throw_type_error("'ownKeys' on proxy: trap returned extra keys but proxy target is non-extensible");
    return false;
  }
  *out = keys;
  return true;
}

bool ProxyObject::call_internal(Context& ctx, const Value& this_v, const Value* argv, int argc, Value* out) {
  Value t;
  if (!trap(ctx, "apply", &t)) return false;
  if (t.is_undefined()) return target->call_internal(ctx, this_v, argv, argc, out);
  std::vector<Value> args(argv, argv + argc);
  Value arr = Value::Obj(ctx.new_array_from(args));
  return ctx.call(t, Value::Obj(handler), {Value::Obj(target), this_v, arr}, out);
}

bool ProxyObject::construct_internal(Context& ctx, const Value* argv, int argc, const Value& new_target, Value* out) {
  Value t;
  if (!trap(ctx, "construct", &t)) return false;
  if (t.is_undefined()) return target->construct_internal(ctx, argv, argc, new_target.is_object() && new_target.as_object() == this ? Value::Obj(target) : new_target, out);
  std::vector<Value> args(argv, argv + argc);
  Value arr = Value::Obj(ctx.new_array_from(args));
  Value r;
  if (!ctx.call(t, Value::Obj(handler), {Value::Obj(target), arr, new_target}, &r)) return false;
  if (!r.is_object()) {
    ctx.throw_type_error("proxy [[Construct]] must return an object");
    return false;
  }
  *out = r;
  return true;
}

ProxyObject* make_proxy(Context& ctx, const Value& target, const Value& handler) {
  if (!target.is_object() || !handler.is_object()) {
    ctx.throw_type_error("Cannot create proxy with a non-object as target or handler");
    return nullptr;
  }
  ProxyObject* p = ctx.heap.make<ProxyObject>();
  p->target = target.as_object();
  p->handler = handler.as_object();
  p->callable = target.as_object()->callable;
  p->is_constructor = target.as_object()->is_constructor;
  p->proto = nullptr;
  return p;
}

Value proxy_ctor(Context& ctx, CallArgs& a) {
  if (!a.is_construct()) {
    ctx.throw_type_error("Constructor Proxy requires 'new'");
    return Value::Exception();
  }
  ProxyObject* p = make_proxy(ctx, a[0], a[1]);
  if (!p) return Value::Exception();
  return Value::Obj(p);
}

Value proxy_revoke(Context& ctx, CallArgs& a) {
  (void)ctx;
  Function* self = a.callee;
  if (self->slots[0].is_object()) {
    ProxyObject* p = static_cast<ProxyObject*>(self->slots[0].as_object());
    p->handler = nullptr;
    p->target = nullptr;
    self->slots[0] = Value::Null();
  }
  return Value::Undefined();
}

Value proxy_revocable(Context& ctx, CallArgs& a) {
  ProxyObject* p = make_proxy(ctx, a[0], a[1]);
  if (!p) return Value::Exception();
  Function* revoke = ctx.new_native("", proxy_revoke, 0);
  revoke->slots.push_back(Value::Obj(p));
  Object* r = ctx.new_object();
  ctx.define_data(r, ctx.atom_ascii("proxy"), Value::Obj(p));
  ctx.define_data(r, ctx.atom_ascii("revoke"), Value::Obj(revoke));
  return Value::Obj(r);
}

}  // namespace

Object* proxy_target_of(Object* proxy, bool* revoked) {
  ProxyObject* p = static_cast<ProxyObject*>(proxy);
  *revoked = p->handler == nullptr;
  return p->target;
}

void setup_proxy(Context& ctx) {
  Function* ctor = ctx.new_native("Proxy", proxy_ctor, 2, true);
  ctx.define_data(ctx.intr.global, ctx.atom_ascii("Proxy"), Value::Obj(ctor), kHiddenAttrs);
  ctx.intr.proxy_ctor = ctor;
  ctx.define_method(ctor, "revocable", proxy_revocable, 2);
}

}  // namespace aios::js
