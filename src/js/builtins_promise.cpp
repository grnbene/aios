// Promise, and the generator / async prototypes that sit on top of it.
#include "builtins.hpp"

namespace aios::js {

namespace {

struct Reaction {
  Value handler;          // a callable, or undefined
  Value cap_promise;      // the derived promise (a capability), or undefined for an internal reaction
  Value cap_resolve;
  Value cap_reject;
};

struct PromiseObject final : Object {
  PromiseObject() : Object(Class::Promise) {}
  enum State : uint8_t { Pending, Fulfilled, Rejected } state = Pending;
  Value result;
  std::vector<Reaction> on_fulfilled, on_rejected;
  bool handled = false;
  void trace(Tracer& t) override {
    Object::trace(t);
    t.mark(result);
    for (auto* list : {&on_fulfilled, &on_rejected})
      for (Reaction& r : *list) { t.mark(r.handler); t.mark(r.cap_promise); t.mark(r.cap_resolve); t.mark(r.cap_reject); }
  }
};

struct Capability {
  Value promise, resolve, reject;
};

PromiseObject* as_promise(const Value& v) {
  return v.is_object() && v.as_object()->cls == Class::Promise ? static_cast<PromiseObject*>(v.as_object()) : nullptr;
}

void make_gc_safe(Function* f) { f->gc_safe = true; }

// ---- reactions and jobs ----

// args: [handler, argument, cap_promise, cap_resolve, cap_reject, is_reject]
Value reaction_job(Context& ctx, CallArgs& a) {
  const Value handler = a[0], argument = a[1], cap_promise = a[2], cap_resolve = a[3], cap_reject = a[4];
  const bool is_reject = a[5].boolean;
  Value result;
  bool threw = false;
  if (handler.is_undefined()) {
    result = argument;
    threw = is_reject;
  } else {
    if (!ctx.call(handler, Value::Undefined(), {argument}, &result)) {
      result = ctx.take_exception();
      threw = true;
    }
  }
  if (cap_promise.is_undefined()) {
    if (threw && handler.is_undefined()) return Value::Undefined();   // an internal reaction with no handler: nothing to do
    return Value::Undefined();
  }
  Value ignored;
  if (!ctx.call(threw ? cap_reject : cap_resolve, Value::Undefined(), {result}, &ignored)) {
    ctx.clear_exception();
  }
  return Value::Undefined();
}

void enqueue_reaction(Context& ctx, const Reaction& r, const Value& argument, bool is_reject) {
  Function* job = ctx.new_native("", reaction_job, 6);
  make_gc_safe(job);
  ctx.enqueue_job_value(Value::Obj(job), {r.handler, argument, r.cap_promise, r.cap_resolve, r.cap_reject, Value::Bool(is_reject)});
}

void settle(Context& ctx, PromiseObject* p, PromiseObject::State state, const Value& v) {
  std::vector<Reaction> list = std::move(state == PromiseObject::Fulfilled ? p->on_fulfilled : p->on_rejected);
  p->on_fulfilled.clear();
  p->on_rejected.clear();
  p->state = state;
  p->result = v;
  for (const Reaction& r : list) enqueue_reaction(ctx, r, v, state == PromiseObject::Rejected);
  if (state == PromiseObject::Rejected && !p->handled) ctx.pending_rejections.push_back(Value::Obj(p));
}

void fulfill(Context& ctx, PromiseObject* p, const Value& v) { settle(ctx, p, PromiseObject::Fulfilled, v); }
void reject(Context& ctx, PromiseObject* p, const Value& v) { settle(ctx, p, PromiseObject::Rejected, v); }

// args: [promise, thenable, then]
Value resolve_thenable_job(Context& ctx, CallArgs& a);

void create_resolving_functions(Context& ctx, PromiseObject* p, Value* resolve, Value* reject);

// The body of a promise's resolve function, after the already-resolved check.
void resolve_inner(Context& ctx, PromiseObject* p, const Value& resolution) {
  if (resolution.is_object() && resolution.as_object() == p) {
    reject(ctx, p, Value::Obj(ctx.new_error(ErrorKind::TypeError, "Chaining cycle detected for promise #<Promise>")));
    return;
  }
  if (!resolution.is_object()) {
    fulfill(ctx, p, resolution);
    return;
  }
  Value then;
  if (!ctx.get(resolution, ctx.atoms.then, &then)) {
    reject(ctx, p, ctx.take_exception());
    return;
  }
  if (!then.is_callable()) {
    fulfill(ctx, p, resolution);
    return;
  }
  Function* job = ctx.new_native("", resolve_thenable_job, 3);
  make_gc_safe(job);
  ctx.enqueue_job_value(Value::Obj(job), {Value::Obj(p), resolution, then});
}

Value resolve_function(Context& ctx, CallArgs& a) {
  Function* self = a.callee;
  Object* st = self->slots[1].as_object();
  if (st->elems[0].boolean) return Value::Undefined();
  st->elems[0] = Value::Bool(true);
  resolve_inner(ctx, static_cast<PromiseObject*>(self->slots[0].as_object()), a[0]);
  return Value::Undefined();
}

Value reject_function(Context& ctx, CallArgs& a) {
  Function* self = a.callee;
  Object* st = self->slots[1].as_object();
  if (st->elems[0].boolean) return Value::Undefined();
  st->elems[0] = Value::Bool(true);
  reject(ctx, static_cast<PromiseObject*>(self->slots[0].as_object()), a[0]);
  return Value::Undefined();
}

void create_resolving_functions(Context& ctx, PromiseObject* p, Value* resolve, Value* rej) {
  Object* st = ctx.new_object(nullptr);
  st->elems.push_back(Value::Bool(false));
  Function* rf = ctx.new_native("", resolve_function, 1);
  Function* jf = ctx.new_native("", reject_function, 1);
  rf->slots = {Value::Obj(p), Value::Obj(st)};
  jf->slots = {Value::Obj(p), Value::Obj(st)};
  *resolve = Value::Obj(rf);
  *rej = Value::Obj(jf);
}

Value resolve_thenable_job(Context& ctx, CallArgs& a) {
  PromiseObject* p = static_cast<PromiseObject*>(a[0].as_object());
  Value resolve, rej;
  create_resolving_functions(ctx, p, &resolve, &rej);
  Value ignored;
  if (!ctx.call(a[2], a[1], {resolve, rej}, &ignored)) {
    Value ex = ctx.take_exception();
    Value r2;
    ctx.call(rej, Value::Undefined(), {ex}, &r2);
    ctx.clear_exception();
  }
  return Value::Undefined();
}

PromiseObject* new_promise(Context& ctx, Object* proto = nullptr) {
  PromiseObject* p = ctx.heap.make<PromiseObject>();
  p->proto = proto ? proto : ctx.intr.promise_proto;
  return p;
}

// ---- capabilities ----

Value capability_executor(Context& ctx, CallArgs& a) {
  Object* holder = a.callee->slots[0].as_object();
  if (!holder->elems[0].is_undefined() || !holder->elems[1].is_undefined()) {
    ctx.throw_type_error("Promise executor has already been invoked with non-undefined arguments");
    return Value::Exception();
  }
  holder->elems[0] = a[0];
  holder->elems[1] = a[1];
  return Value::Undefined();
}

bool new_capability(Context& ctx, const Value& c, Capability* out) {
  if (c.is_object() && c.as_object() == ctx.intr.promise_ctor) {
    PromiseObject* p = new_promise(ctx);
    out->promise = Value::Obj(p);
    create_resolving_functions(ctx, p, &out->resolve, &out->reject);
    return true;
  }
  if (!c.is_object() || !c.as_object()->is_constructor) {
    ctx.throw_type_error(vm_describe(ctx, c) + " is not a constructor");
    return false;
  }
  Object* holder = ctx.new_object(nullptr);
  holder->elems = {Value::Undefined(), Value::Undefined()};
  Function* ex = ctx.new_native("", capability_executor, 2);
  ex->slots.push_back(Value::Obj(holder));
  Value p;
  Value exv = Value::Obj(ex);
  if (!ctx.construct(c, &exv, 1, c, &p)) return false;
  if (!holder->elems[0].is_callable() || !holder->elems[1].is_callable()) {
    ctx.throw_type_error("Promise resolve or reject function is not callable");
    return false;
  }
  out->promise = p;
  out->resolve = holder->elems[0];
  out->reject = holder->elems[1];
  return true;
}

void perform_then(Context& ctx, PromiseObject* p, const Value& on_f, const Value& on_r, const Capability* cap) {
  Reaction fr, rr;
  fr.handler = on_f.is_callable() ? on_f : Value::Undefined();
  rr.handler = on_r.is_callable() ? on_r : Value::Undefined();
  if (cap) {
    fr.cap_promise = rr.cap_promise = cap->promise;
    fr.cap_resolve = rr.cap_resolve = cap->resolve;
    fr.cap_reject = rr.cap_reject = cap->reject;
  }
  if (p->state == PromiseObject::Pending) {
    p->on_fulfilled.push_back(fr);
    p->on_rejected.push_back(rr);
  } else if (p->state == PromiseObject::Fulfilled) {
    enqueue_reaction(ctx, fr, p->result, false);
  } else {
    if (!p->handled) {
      auto& v = ctx.pending_rejections;
      for (size_t i = 0; i < v.size(); ++i)
        if (v[i].is_object() && v[i].as_object() == p) { v.erase(v.begin() + static_cast<long>(i)); break; }
    }
    enqueue_reaction(ctx, rr, p->result, true);
  }
  p->handled = true;
}

// PromiseResolve(C, x)
bool promise_resolve_c(Context& ctx, const Value& c, const Value& x, Value* out) {
  if (PromiseObject* p = as_promise(x)) {
    Value ctor;
    if (!p->get(ctx, ctx.atoms.constructor, x, &ctor)) return false;
    if (ctor.is_object() && c.is_object() && ctor.as_object() == c.as_object()) {
      *out = x;
      return true;
    }
  }
  Capability cap;
  if (!new_capability(ctx, c, &cap)) return false;
  Value ignored;
  if (!ctx.call(cap.resolve, Value::Undefined(), {x}, &ignored)) return false;
  *out = cap.promise;
  return true;
}

// ---- constructor and prototype ----

Value promise_ctor(Context& ctx, CallArgs& a) {
  if (!a.is_construct()) {
    ctx.throw_type_error("Promise constructor cannot be invoked without 'new'");
    return Value::Exception();
  }
  if (!a[0].is_callable()) {
    ctx.throw_type_error("Promise resolver " + vm_describe(ctx, a[0]) + " is not a function");
    return Value::Exception();
  }
  Object* proto;
  JS_TRY(ctx.prototype_from_ctor(a.new_target, ctx.intr.promise_proto, &proto));
  PromiseObject* p = new_promise(ctx, proto);
  Value resolve, rej;
  create_resolving_functions(ctx, p, &resolve, &rej);
  Value ignored;
  if (!ctx.call(a[0], Value::Undefined(), {resolve, rej}, &ignored)) {
    Value ex = ctx.take_exception();
    Value r2;
    JS_TRY(ctx.call(rej, Value::Undefined(), {ex}, &r2));
  }
  return Value::Obj(p);
}

Value promise_species(Context&, CallArgs& a) { return a.this_value; }

Value promise_then(Context& ctx, CallArgs& a) {
  PromiseObject* p = as_promise(a.this_value);
  if (!p) {
    ctx.throw_type_error("Method Promise.prototype.then called on incompatible receiver " + vm_describe(ctx, a.this_value));
    return Value::Exception();
  }
  Value c;
  JS_TRY(ctx.species_constructor(p, Value::Obj(ctx.intr.promise_ctor), &c));
  Capability cap;
  JS_TRY(new_capability(ctx, c, &cap));
  perform_then(ctx, p, a[0], a[1], &cap);
  return cap.promise;
}

Value promise_catch(Context& ctx, CallArgs& a) {
  Value then;
  JS_TRY(ctx.get_v(a.this_value, ctx.atoms.then, &then));
  Value r;
  JS_TRY(ctx.call(then, a.this_value, {Value::Undefined(), a[0]}, &r));
  return r;
}

// finally: thenFinally / catchFinally with slots [onFinally, C]
Value value_thunk(Context&, CallArgs& a) { return a.callee->slots[0]; }
Value thrower(Context& ctx, CallArgs& a) {
  ctx.set_exception(a.callee->slots[0]);
  return Value::Exception();
}

Value finally_function(Context& ctx, CallArgs& a) {
  const Value on_finally = a.callee->slots[0], c = a.callee->slots[1];
  const bool is_catch = a.callee->slots[2].boolean;
  Value result;
  JS_TRY(ctx.call(on_finally, Value::Undefined(), nullptr, 0, &result));
  Value promise;
  JS_TRY(promise_resolve_c(ctx, c, result, &promise));
  Function* thunk = ctx.new_native("", is_catch ? thrower : value_thunk, 0);
  thunk->slots.push_back(a[0]);
  Value then;
  JS_TRY(ctx.get_v(promise, ctx.atoms.then, &then));
  Value r;
  JS_TRY(ctx.call(then, promise, {Value::Obj(thunk)}, &r));
  return r;
}

Value promise_finally(Context& ctx, CallArgs& a) {
  if (!a.this_value.is_object()) {
    ctx.throw_type_error("Promise.prototype.finally called on a non-object");
    return Value::Exception();
  }
  Value c;
  JS_TRY(ctx.species_constructor(a.this_value.as_object(), Value::Obj(ctx.intr.promise_ctor), &c));
  Value then_finally = a[0], catch_finally = a[0];
  if (a[0].is_callable()) {
    Function* tf = ctx.new_native("", finally_function, 1);
    tf->slots = {a[0], c, Value::Bool(false)};
    Function* cf = ctx.new_native("", finally_function, 1);
    cf->slots = {a[0], c, Value::Bool(true)};
    then_finally = Value::Obj(tf);
    catch_finally = Value::Obj(cf);
  }
  Value then;
  JS_TRY(ctx.get_v(a.this_value, ctx.atoms.then, &then));
  Value r;
  JS_TRY(ctx.call(then, a.this_value, {then_finally, catch_finally}, &r));
  return r;
}

// ---- statics ----

Value promise_resolve_static(Context& ctx, CallArgs& a) {
  if (!a.this_value.is_object()) {
    ctx.throw_type_error("PromiseResolve called on non-object");
    return Value::Exception();
  }
  Value r;
  JS_TRY(promise_resolve_c(ctx, a.this_value, a[0], &r));
  return r;
}

Value promise_reject_static(Context& ctx, CallArgs& a) {
  Capability cap;
  JS_TRY(new_capability(ctx, a.this_value, &cap));
  Value ignored;
  JS_TRY(ctx.call(cap.reject, Value::Undefined(), {a[0]}, &ignored));
  return cap.promise;
}

Value promise_with_resolvers(Context& ctx, CallArgs& a) {
  Capability cap;
  JS_TRY(new_capability(ctx, a.this_value, &cap));
  Object* o = ctx.new_object();
  ctx.define_data(o, ctx.atom_ascii("promise"), cap.promise);
  ctx.define_data(o, ctx.atoms.resolve, cap.resolve);
  ctx.define_data(o, ctx.atoms.reject, cap.reject);
  return Value::Obj(o);
}

// Combinators share one shape: iterate, resolve each item through C.resolve, and attach element functions.
enum class Combinator { All, AllSettled, Any, Race };

// slots: [values_array, counter_array, cap_resolve_or_reject, index, called, kind]
//   counter_array->elems[0] = remaining count
Value element_function(Context& ctx, CallArgs& a) {
  Function* self = a.callee;
  if (self->slots[4].boolean) return Value::Undefined();
  self->slots[4] = Value::Bool(true);
  ArrayObject* values = static_cast<ArrayObject*>(self->slots[0].as_object());
  Object* counter = self->slots[1].as_object();
  const size_t index = static_cast<size_t>(self->slots[3].num);
  const int kind = static_cast<int>(self->slots[5].num);   // 0 all-resolve, 1 settled-fulfilled, 2 settled-rejected, 3 any-reject
  Value stored = a[0];
  if (kind == 1 || kind == 2) {
    Object* o = ctx.new_object();
    ctx.define_data(o, ctx.atom_ascii("status"), Str(ctx, kind == 1 ? "fulfilled" : "rejected"));
    ctx.define_data(o, kind == 1 ? ctx.atoms.value : ctx.atom_ascii("reason"), a[0]);
    stored = Value::Obj(o);
  }
  values->elems[index] = stored;
  counter->elems[0] = Value::Number(counter->elems[0].num - 1);
  if (counter->elems[0].num == 0) {
    Value fn = self->slots[2];
    Value arg = Value::Obj(values);
    if (kind == 3) {
      Object* err = ctx.new_error(ErrorKind::AggregateError, "All promises were rejected");
      ctx.define_data(err, ctx.atoms.errors, Value::Obj(values), kHiddenAttrs);
      arg = Value::Obj(err);
    }
    Value r;
    JS_TRY(ctx.call(fn, Value::Undefined(), {arg}, &r));
  }
  return Value::Undefined();
}

Value combinator_impl(Context& ctx, CallArgs& a, Combinator kind) {
  Capability cap;
  const Value c = a.this_value;
  JS_TRY(new_capability(ctx, c, &cap));
  auto fail = [&]() -> Value {
    Value ex = ctx.take_exception();
    Value ignored;
    if (!ctx.call(cap.reject, Value::Undefined(), {ex}, &ignored)) return Value::Exception();
    return cap.promise;
  };
  Value promise_resolve;
  if (!ctx.get_v(c, ctx.atoms.resolve, &promise_resolve)) return fail();
  if (!promise_resolve.is_callable()) {
    ctx.throw_type_error("Promise resolve or reject function is not callable");
    return fail();
  }
  ArrayObject* values = ctx.new_array(0);
  ArrayObject* counter = ctx.new_array(0);
  counter->elems.push_back(Value::Number(1));
  counter->length_ = 1;
  size_t index = 0;
  const bool ok = ctx.iterate(a[0], [&](Value item) {
    Value next;
    if (!ctx.call(promise_resolve, c, {item}, &next)) return false;
    Value on_f, on_r;
    if (kind == Combinator::Race) {
      on_f = cap.resolve;
      on_r = cap.reject;
    } else {
      values->elems.push_back(Value::Undefined());
      ++values->length_;
      counter->elems[0] = Value::Number(counter->elems[0].num + 1);
      auto make = [&](int k, const Value& target) {
        Function* f = ctx.new_native("", element_function, 1);
        f->slots = {Value::Obj(values), Value::Obj(counter), target, Value::Number(static_cast<double>(index)), Value::Bool(false), Value::Number(k)};
        return Value::Obj(f);
      };
      switch (kind) {
        case Combinator::All: on_f = make(0, cap.resolve); on_r = cap.reject; break;
        case Combinator::AllSettled: on_f = make(1, cap.resolve); on_r = make(2, cap.resolve); break;
        case Combinator::Any: on_f = cap.resolve; on_r = make(3, cap.reject); break;
        default: break;
      }
      ++index;
    }
    Value then, ignored;
    if (!ctx.get_v(next, ctx.atoms.then, &then)) return false;
    return ctx.call(then, next, {on_f, on_r}, &ignored);
  });
  if (!ok) return fail();
  if (kind != Combinator::Race) {
    counter->elems[0] = Value::Number(counter->elems[0].num - 1);
    if (counter->elems[0].num == 0) {
      Value arg = Value::Obj(values);
      Value fn = cap.resolve;
      if (kind == Combinator::Any) {
        Object* err = ctx.new_error(ErrorKind::AggregateError, "All promises were rejected");
        ctx.define_data(err, ctx.atoms.errors, Value::Obj(values), kHiddenAttrs);
        arg = Value::Obj(err);
        fn = cap.reject;
      }
      Value ignored;
      if (!ctx.call(fn, Value::Undefined(), {arg}, &ignored)) return fail();
    }
  }
  return cap.promise;
}

Value promise_all(Context& ctx, CallArgs& a) { return combinator_impl(ctx, a, Combinator::All); }
Value promise_all_settled(Context& ctx, CallArgs& a) { return combinator_impl(ctx, a, Combinator::AllSettled); }
Value promise_any(Context& ctx, CallArgs& a) { return combinator_impl(ctx, a, Combinator::Any); }
Value promise_race(Context& ctx, CallArgs& a) { return combinator_impl(ctx, a, Combinator::Race); }

// ---- generator prototypes ----

Value generator_next(Context& ctx, CallArgs& a) {
  Value r;
  JS_TRY(generator_resume(ctx, a.this_value, 0, a[0], &r));
  return r;
}
Value generator_return(Context& ctx, CallArgs& a) {
  Value r;
  JS_TRY(generator_resume(ctx, a.this_value, 2, a[0], &r));
  return r;
}
Value generator_throw(Context& ctx, CallArgs& a) {
  Value r;
  JS_TRY(generator_resume(ctx, a.this_value, 1, a[0], &r));
  return r;
}
Value async_generator_next(Context& ctx, CallArgs& a) {
  Value r;
  JS_TRY(async_generator_enqueue(ctx, a.this_value, 0, a[0], &r));
  return r;
}
Value async_generator_return(Context& ctx, CallArgs& a) {
  Value r;
  JS_TRY(async_generator_enqueue(ctx, a.this_value, 2, a[0], &r));
  return r;
}
Value async_generator_throw(Context& ctx, CallArgs& a) {
  Value r;
  JS_TRY(async_generator_enqueue(ctx, a.this_value, 1, a[0], &r));
  return r;
}

}  // namespace

// ---- exported to the VM and the host ----

Object* promise_new(Context& ctx) { return new_promise(ctx); }

bool promise_resolve_with(Context& ctx, Object* promise, const Value& v) {
  resolve_inner(ctx, static_cast<PromiseObject*>(promise), v);
  return true;
}

void promise_reject_with(Context& ctx, Object* promise, const Value& reason) { reject(ctx, static_cast<PromiseObject*>(promise), reason); }

Object* promise_resolve_value(Context& ctx, const Value& v) {
  Value r;
  if (!promise_resolve_c(ctx, Value::Obj(ctx.intr.promise_ctor), v, &r)) return nullptr;
  return r.as_object();
}

void promise_then_internal(Context& ctx, Object* promise, const Value& on_fulfilled, const Value& on_rejected) {
  perform_then(ctx, static_cast<PromiseObject*>(promise), on_fulfilled, on_rejected, nullptr);
}

void promise_report_unhandled(Context& ctx) {
  std::vector<Value> list;
  list.swap(ctx.pending_rejections);
  for (const Value& v : list) {
    PromiseObject* p = as_promise(v);
    if (!p || p->handled) continue;
    if (ctx.on_unhandled_rejection) ctx.on_unhandled_rejection(v, p->result);
    else if (ctx.print) ctx.print("Uncaught (in promise) " + ctx.describe_exception(p->result) + "\n");
  }
}

void setup_promise(Context& ctx) {
  Object* proto = ctx.new_object();
  ctx.intr.promise_proto = proto;
  Function* ctor = new_ctor(ctx, "Promise", promise_ctor, 1, proto);
  ctx.intr.promise_ctor = ctor;
  ctx.define_method(ctor, "all", promise_all, 1);
  ctx.define_method(ctor, "allSettled", promise_all_settled, 1);
  ctx.define_method(ctor, "any", promise_any, 1);
  ctx.define_method(ctor, "race", promise_race, 1);
  ctx.define_method(ctor, "reject", promise_reject_static, 1);
  ctx.define_method(ctor, "resolve", promise_resolve_static, 1);
  ctx.define_method(ctor, "withResolvers", promise_with_resolvers, 0);
  ctx.define_getter(ctor, ctx.sym.species, "[Symbol.species]", promise_species);
  ctx.define_method(proto, "catch", promise_catch, 1);
  ctx.define_method(proto, "finally", promise_finally, 1);
  ctx.define_method(proto, "then", promise_then, 2);
  ctx.define_data(proto, ctx.sym.to_string_tag, Str(ctx, "Promise"), kConfigurable);
}

void setup_generator(Context& ctx) {
  // %GeneratorPrototype%
  Object* gp = ctx.new_object(ctx.intr.iterator_proto);
  ctx.intr.generator_proto = gp;
  ctx.define_method(gp, "next", generator_next, 1);
  ctx.define_method(gp, "return", generator_return, 1);
  ctx.define_method(gp, "throw", generator_throw, 1);
  ctx.define_data(gp, ctx.sym.to_string_tag, Str(ctx, "Generator"), kConfigurable);
  ctx.define_data(ctx.intr.generator_function_proto, ctx.atoms.prototype, Value::Obj(gp), kConfigurable);
  ctx.define_data(gp, ctx.atoms.constructor, Value::Obj(ctx.intr.generator_function_proto), kConfigurable);
  // %AsyncGeneratorPrototype%
  Object* agp = ctx.new_object(ctx.intr.async_iterator_proto);
  ctx.intr.async_generator_proto = agp;
  ctx.define_method(agp, "next", async_generator_next, 1);
  ctx.define_method(agp, "return", async_generator_return, 1);
  ctx.define_method(agp, "throw", async_generator_throw, 1);
  ctx.define_data(agp, ctx.sym.to_string_tag, Str(ctx, "AsyncGenerator"), kConfigurable);
  ctx.define_data(ctx.intr.async_generator_function_proto, ctx.atoms.prototype, Value::Obj(agp), kConfigurable);
  ctx.define_data(agp, ctx.atoms.constructor, Value::Obj(ctx.intr.async_generator_function_proto), kConfigurable);
}

}  // namespace aios::js
