// Generators, async functions and async generators: frames that suspend and resume.
#include "vm.hpp"

namespace aios::js {

// ---------------------------------------------------------------------------------------------------
// suspending and resuming a frame
// ---------------------------------------------------------------------------------------------------

void vm_suspend(Context& ctx, Frame& f, Value* sp, uint32_t resume_pc, uint32_t instr_pc, bool awaiting, bool in_yield_star) {
  Context::VM& vm = *ctx.vm;
  Coroutine* co = f.co;
  co->saved.assign(f.base, sp);
  co->env = f.env;
  co->env_depth = f.env_depth;
  co->pc = resume_pc;
  co->yield_pc = instr_pc;
  co->awaiting = awaiting;
  co->in_yield_star = in_yield_star;
  co->this_v = f.this_v;
  co->new_target = f.new_target;
  co->extra = std::move(f.extra);
  co->argc = f.argc;
  co->state = CoState::Suspended;
  vm.sp = f.base - 2;
  vm.frames.pop_back();
}

Exit vm_resume(Context& ctx, Coroutine* co, int mode, const Value& value, Value* result) {
  Context::VM& vm = *ctx.vm;
  FunctionCode* code = co->fn->code;
  const size_t n = co->saved.size();
  if (vm.frames.size() >= kMaxFrames || !vm_reserve(ctx, n + 2 + code->nlocals + code->max_stack)) {
    ctx.throw_range_error("Maximum call stack size exceeded");
    co->state = CoState::Done;
    return Exit::Throw;
  }
  const bool was_start = co->state == CoState::Start;
  Value* sp = vm.sp;
  sp[0] = Value::Obj(co->fn);
  sp[1] = co->this_v;
  Value* base = sp + 2;
  for (size_t i = 0; i < n; ++i) base[i] = co->saved[i];
  vm.sp = base + n;
  vm.frames.emplace_back();
  Frame& f = vm.frames.back();
  f.fn = co->fn;
  f.code = code;
  f.env = co->env;
  f.base = base;
  f.pc = co->pc;
  f.env_depth = co->env_depth;
  f.argc = co->argc;
  f.this_v = co->this_v;
  f.new_target = co->new_target;
  f.extra = std::move(co->extra);
  f.co = co;
  f.entry = true;
  co->saved.clear();
  co->state = CoState::Running;
  const bool in_star = co->in_yield_star;
  co->in_yield_star = false;
  vm.pending_throw = false;
  if (!was_start) {
    if (in_star) {
      // yield* re-runs its instruction with the resumption
      f.rmode = mode;
      f.rvalue = value;
      co->in_yield_star = true;
    } else if (mode == 0) {
      *vm.sp++ = value;
    } else if (mode == 1) {
      ctx.set_exception(value);
      f.pc = co->yield_pc;
      vm.pending_throw = true;
    } else {
      ctx.set_exception(Value::Obj(vm.return_sentinel));
      co->return_value = value;
      f.pc = co->yield_pc;
      vm.pending_throw = true;
    }
  }
  Exit e = vm_execute(ctx, result);
  if (e == Exit::Return || e == Exit::Throw) co->state = CoState::Done;
  return e;
}

// ---------------------------------------------------------------------------------------------------
// generators
// ---------------------------------------------------------------------------------------------------

bool generator_resume(Context& ctx, const Value& gen, int mode, const Value& arg, Value* out) {
  Coroutine* co = nullptr;
  if (gen.is_object() && gen.as_object()->cls == Class::Generator) {
    co = static_cast<Coroutine*>(gen.as_object());
    if (co->kind != CoKind::Generator) co = nullptr;
  }
  if (!co) {
    ctx.throw_type_error("next method called on incompatible receiver " + vm_describe(ctx, gen));
    return false;
  }
  if (co->state == CoState::Running) {
    ctx.throw_type_error("Generator is already running");
    return false;
  }
  if (co->state == CoState::Start && mode != 0) co->state = CoState::Done;
  if (co->state == CoState::Done) {
    if (mode == 1) { ctx.set_exception(arg); return false; }
    *out = Value::Obj(ctx.create_iter_result(mode == 2 ? arg : Value::Undefined(), true));
    return true;
  }
  Value r;
  Exit e = vm_resume(ctx, co, mode, arg, &r);
  switch (e) {
    case Exit::Yield:
      *out = co->in_yield_star ? r : Value::Obj(ctx.create_iter_result(r, false));
      return true;
    case Exit::Return:
      *out = Value::Obj(ctx.create_iter_result(r, true));
      return true;
    case Exit::Throw:
      return false;
    case Exit::Await:
      ctx.throw_syntax_error("await is only valid in async functions");
      co->state = CoState::Done;
      return false;
  }
  return false;
}

// ---------------------------------------------------------------------------------------------------
// await
// ---------------------------------------------------------------------------------------------------

namespace {

void async_gen_step(Context& ctx, Coroutine* co, int mode, const Value& value);
void async_gen_drain(Context& ctx, Coroutine* co);

Value await_fulfilled(Context& ctx, CallArgs& a) {
  Coroutine* co = static_cast<Coroutine*>(a.callee->slots[0].as_object());
  if (co->kind == CoKind::AsyncGenerator) async_gen_step(ctx, co, 0, a[0]);
  else vm_async_step(ctx, co, 0, a[0]);
  return Value::Undefined();
}

Value await_rejected(Context& ctx, CallArgs& a) {
  Coroutine* co = static_cast<Coroutine*>(a.callee->slots[0].as_object());
  if (co->kind == CoKind::AsyncGenerator) async_gen_step(ctx, co, 1, a[0]);
  else vm_async_step(ctx, co, 1, a[0]);
  return Value::Undefined();
}

}  // namespace

bool vm_await(Context& ctx, Coroutine* co, const Value& v) {
  Object* p = promise_resolve_value(ctx, v);
  if (!p) {
    // PromiseResolve itself threw: the await throws
    Value ex = ctx.take_exception();
    if (co->kind == CoKind::AsyncGenerator) async_gen_step(ctx, co, 1, ex);
    else vm_async_step(ctx, co, 1, ex);
    return true;
  }
  Function* on_ok = ctx.new_native("", await_fulfilled, 1);
  on_ok->gc_safe = true;
  on_ok->slots.push_back(Value::Obj(co));
  Function* on_err = ctx.new_native("", await_rejected, 1);
  on_err->gc_safe = true;
  on_err->slots.push_back(Value::Obj(co));
  promise_then_internal(ctx, p, Value::Obj(on_ok), Value::Obj(on_err));
  return true;
}

// ---------------------------------------------------------------------------------------------------
// async functions
// ---------------------------------------------------------------------------------------------------

bool vm_async_step(Context& ctx, Coroutine* co, int mode, const Value& value) {
  Value r;
  Exit e = vm_resume(ctx, co, mode, value, &r);
  switch (e) {
    case Exit::Return:
      promise_resolve_with(ctx, co->promise, r);
      if (ctx.has_exception()) promise_reject_with(ctx, co->promise, ctx.take_exception());
      return true;
    case Exit::Throw:
      promise_reject_with(ctx, co->promise, ctx.take_exception());
      return true;
    case Exit::Await:
      return vm_await(ctx, co, r);
    case Exit::Yield:
      promise_reject_with(ctx, co->promise, Value::Obj(ctx.new_error(ErrorKind::SyntaxError, "yield in an async function")));
      return true;
  }
  return true;
}

bool vm_start_coroutine(Context& ctx, Function* fn, const Value& this_v, int argc, Value* out) {
  FunctionCode* code = fn->code;
  const CoKind kind = code->is_generator ? (code->is_async ? CoKind::AsyncGenerator : CoKind::Generator) : CoKind::Async;
  Class cls = kind == CoKind::AsyncGenerator ? Class::AsyncGenerator : Class::Generator;
  Coroutine* co = ctx.heap.make<Coroutine>(cls, kind);
  co->fn = fn;
  if (kind != CoKind::Async) {
    Value p;
    if (!ctx.get(Value::Obj(fn), ctx.atoms.prototype, &p)) return false;
    co->proto = p.is_object() ? p.as_object() : (kind == CoKind::Generator ? ctx.intr.generator_proto : ctx.intr.async_generator_proto);
  }
  if (!vm_push_frame(ctx, fn, argc, this_v, Value::Undefined(), true, false, co)) return false;
  Value r;
  if (kind == CoKind::Async) {
    co->promise = promise_new(ctx);
    co->state = CoState::Running;
    Exit e = vm_execute(ctx, &r);
    if (e == Exit::Return || e == Exit::Throw) co->state = CoState::Done;
    switch (e) {
      case Exit::Return:
        promise_resolve_with(ctx, co->promise, r);
        if (ctx.has_exception()) promise_reject_with(ctx, co->promise, ctx.take_exception());
        break;
      case Exit::Throw: promise_reject_with(ctx, co->promise, ctx.take_exception()); break;
      case Exit::Await: vm_await(ctx, co, r); break;
      case Exit::Yield: break;
    }
    *out = Value::Obj(co->promise);
    return true;
  }
  // a generator runs its parameter setup now and stops at InitialYield
  Exit e = vm_execute(ctx, &r);
  if (e == Exit::Throw) return false;
  if (e != Exit::Yield) {
    ctx.throw_error(ErrorKind::Error, "generator did not reach its initial yield");
    return false;
  }
  co->state = CoState::Start;
  *out = Value::Obj(co);
  return true;
}

// ---------------------------------------------------------------------------------------------------
// async generators
// ---------------------------------------------------------------------------------------------------

namespace {

void async_gen_settle_front(Context& ctx, Coroutine* co, bool reject, const Value& v, bool done) {
  AsyncGenRequest req = co->queue.front();
  co->queue.erase(co->queue.begin());
  if (reject) promise_reject_with(ctx, req.promise, v);
  else {
    promise_resolve_with(ctx, req.promise, Value::Obj(ctx.create_iter_result(v, done)));
    if (ctx.has_exception()) promise_reject_with(ctx, req.promise, ctx.take_exception());
  }
}

void async_gen_step(Context& ctx, Coroutine* co, int mode, const Value& value) {
  Value r;
  Exit e = vm_resume(ctx, co, mode, value, &r);
  switch (e) {
    case Exit::Await:
      vm_await(ctx, co, r);
      return;
    case Exit::Yield:
      co->draining = false;
      async_gen_settle_front(ctx, co, false, r, false);
      break;
    case Exit::Return:
      co->draining = false;
      async_gen_settle_front(ctx, co, false, r, true);
      break;
    case Exit::Throw:
      co->draining = false;
      async_gen_settle_front(ctx, co, true, ctx.take_exception(), false);
      break;
  }
  async_gen_drain(ctx, co);
}

void async_gen_drain(Context& ctx, Coroutine* co) {
  while (!co->queue.empty() && !co->draining) {
    AsyncGenRequest& req = co->queue.front();
    if (co->state == CoState::Start && req.mode != 0) co->state = CoState::Done;
    if (co->state == CoState::Done) {
      const int mode = req.mode;
      const Value v = req.value;
      if (mode == 1) async_gen_settle_front(ctx, co, true, v, false);
      else async_gen_settle_front(ctx, co, false, mode == 2 ? v : Value::Undefined(), true);
      continue;
    }
    co->draining = true;
    async_gen_step(ctx, co, req.mode, req.value);
    return;
  }
}

}  // namespace

bool async_generator_enqueue(Context& ctx, const Value& gen, int mode, const Value& arg, Value* promise_out) {
  Object* promise = promise_new(ctx);
  *promise_out = Value::Obj(promise);
  Coroutine* co = nullptr;
  if (gen.is_object() && gen.as_object()->cls == Class::AsyncGenerator) co = static_cast<Coroutine*>(gen.as_object());
  if (!co) {
    promise_reject_with(ctx, promise, Value::Obj(ctx.new_error(ErrorKind::TypeError, "next method called on incompatible receiver " + vm_describe(ctx, gen))));
    return true;
  }
  AsyncGenRequest req;
  req.mode = mode;
  req.value = arg;
  req.promise = promise;
  co->queue.push_back(req);
  if (!co->draining) async_gen_drain(ctx, co);
  return true;
}

bool vm_async_gen_yield(Context&, Coroutine*, const Value&) { return true; }

}  // namespace aios::js
