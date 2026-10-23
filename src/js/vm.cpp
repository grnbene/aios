// The bytecode interpreter.
#include <cstdio>
#include "aios/hal/host.hpp"
#include <cstdlib>
#include <cstring>

#include "bigint.hpp"
#include "codegen.hpp"
#include "typedarray.hpp"
#include "vm.hpp"

namespace aios::js {

// ---------------------------------------------------------------------------------------------------
// setup and tracing
// ---------------------------------------------------------------------------------------------------

void vm_init(Context& ctx) {
  Context::VM* vm = new Context::VM();
  vm->capacity = ctx.max_stack_values;
  vm->stack = static_cast<Value*>(std::calloc(vm->capacity, sizeof(Value)));   // all-zero bits are `undefined`
  vm->sp = vm->stack;
  vm->frames.reserve(kMaxFrames + 64);
  vm->return_sentinel = ctx.heap.make<Object>(Class::Object);
  ctx.vm = vm;
}

void vm_shutdown(Context& ctx) {
  if (!ctx.vm) return;
  std::free(ctx.vm->stack);
  delete ctx.vm;
  ctx.vm = nullptr;
}

void vm_trace(Context& ctx, Tracer& t) {
  Context::VM& vm = *ctx.vm;
  for (Value* p = vm.stack; p < vm.sp; ++p) t.mark(*p);
  for (Frame& f : vm.frames) {
    t.mark(f.fn);
    t.mark(f.env);
    t.mark(f.this_v);
    t.mark(f.new_target);
    t.mark(f.rvalue);
    for (const Value& v : f.extra) t.mark(v);
    t.mark(f.co);
  }
  t.mark(vm.return_sentinel);
}

void Coroutine::trace(Tracer& t) {
  Object::trace(t);
  t.mark(fn);
  t.mark(env);
  for (const Value& v : saved) t.mark(v);
  t.mark(this_v);
  t.mark(new_target);
  for (const Value& v : extra) t.mark(v);
  t.mark(return_value);
  t.mark(promise);
  for (const AsyncGenRequest& r : queue) {
    t.mark(r.value);
    t.mark(r.promise);
  }
}

std::string vm_stack_trace(Context& ctx, int skip_frames) {
  std::string out;
  Context::VM& vm = *ctx.vm;
  int skipped = 0, shown = 0;
  for (size_t i = vm.frames.size(); i-- > 0;) {
    Frame& f = vm.frames[i];
    if (skipped < skip_frames) { ++skipped; continue; }
    if (shown++ >= 10) break;
    uint32_t line = 0;
    for (const LineEntry& e : f.code->lines) {
      if (e.pc <= f.pc) line = e.line;
      else break;
    }
    std::string name = f.code->name ? f.code->name->utf8() : std::string();
    std::string file = f.code->filename ? f.code->filename->utf8() : std::string("<anonymous>");
    if (!out.empty()) out += "\n";
    out += "    at ";
    if (!name.empty()) out += name + " (" + file + ":" + std::to_string(line) + ")";
    else out += file + ":" + std::to_string(line);
  }
  return out;
}

bool vm_reserve(Context& ctx, size_t values) {
  Context::VM& vm = *ctx.vm;
  if (vm.sp + values + 16 > vm.stack + vm.capacity) {
    ctx.throw_range_error("Maximum call stack size exceeded");
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------------------------------
// frames
// ---------------------------------------------------------------------------------------------------

// The arguments are on the stack at [sp - argc, sp), with the callee and `this` just below them.
bool vm_push_frame(Context& ctx, Function* fn, int argc, const Value& this_in, const Value& new_target, bool entry, bool construct, Coroutine* co) {
  Context::VM& vm = *ctx.vm;
  FunctionCode* code = fn->code;
  if (vm.frames.size() >= kMaxFrames) {
    ctx.throw_range_error("Maximum call stack size exceeded");
    return false;
  }
  Value* base = vm.sp - argc;
  if (base + code->nlocals + code->max_stack + 16 > vm.stack + vm.capacity) {
    ctx.throw_range_error("Maximum call stack size exceeded");
    return false;
  }
  Value this_v = this_in;
  if (!code->is_arrow && !code->strict && !construct) {
    if (this_v.is_nullish()) {
      this_v = Value::Obj(ctx.intr.global);
    } else if (!this_v.is_object()) {
      Object* o;
      if (!ctx.to_object(this_v, &o)) return false;
      this_v = Value::Obj(o);
    }
  }
  vm.frames.emplace_back();
  Frame& f = vm.frames.back();
  f.fn = fn;
  f.code = code;
  f.env = fn->env;
  f.base = base;
  f.argc = static_cast<uint32_t>(argc);
  f.entry = entry;
  f.construct = construct;
  f.co = co;
  const uint32_t np = code->nparams;
  if (static_cast<uint32_t>(argc) > np) {
    if (code->uses_arguments || code->has_rest) f.extra.assign(base + np, base + argc);
  } else {
    for (uint32_t i = static_cast<uint32_t>(argc); i < np; ++i) base[i] = Value::Undefined();
  }
  for (uint32_t i = np; i < code->nlocals; ++i) base[i] = Value::Undefined();
  vm.sp = base + code->nlocals;
  if (code->is_arrow) {
    f.this_v = fn->this_val;
    f.new_target = fn->new_target_lexical;
  } else {
    f.this_v = this_v;
    f.new_target = new_target;
  }
  return true;
}

// ---------------------------------------------------------------------------------------------------
// the interpreter loop
// ---------------------------------------------------------------------------------------------------

namespace {

inline uint16_t rd16(const uint8_t*& ip) { uint16_t v; std::memcpy(&v, ip, 2); ip += 2; return v; }
inline uint32_t rd32(const uint8_t*& ip) { uint32_t v; std::memcpy(&v, ip, 4); ip += 4; return v; }
inline int32_t rdi32(const uint8_t*& ip) { int32_t v; std::memcpy(&v, ip, 4); ip += 4; return v; }

void throw_tdz(Context& ctx, JsString* name) {
  if (name->str() == u"this") ctx.throw_reference_error("Must call super constructor in derived class before accessing 'this' or returning from derived constructor");
  else ctx.throw_reference_error("Cannot access '" + name->utf8() + "' before initialization");
}

Function* active_ctor(Frame& f) {
  if (f.fn->is_class) return f.fn;
  return static_cast<Function*>(f.fn->fields_owner);
}

// ---- iteration over the three registers an iterator occupies: iterator, next method, done flag ----

bool iter_init(Context& ctx, Value* regs, const Value& iterable, bool async) {
  Context::Iter it;
  if (!ctx.get_iterator(iterable, &it, async)) return false;
  regs[0] = it.iterator;
  regs[1] = it.next;
  regs[2] = Value::Bool(false);
  // for await over a sync iterable: remember it, so each value is awaited (what the spec's async-from-sync iterator does)
  // (only the async sites reserve a fourth register)
  if (async) {
    Value m;
    if (!ctx.get_method(iterable, ctx.sym.async_iterator, &m)) return false;
    regs[3] = Value::Bool(m.is_undefined());
  }
  return true;
}

bool iter_result_unpack(Context& ctx, Value* regs, const Value& r, bool* done, Value* value) {
  if (!r.is_object()) {
    regs[2] = Value::Bool(true);
    ctx.throw_type_error("Iterator result " + vm_describe(ctx, r) + " is not an object");
    return false;
  }
  Value d;
  if (!ctx.get(r, ctx.atoms.done, &d)) { regs[2] = Value::Bool(true); return false; }
  if (Context::to_boolean(d)) {
    regs[2] = Value::Bool(true);
    *done = true;
    return true;
  }
  if (!ctx.get(r, ctx.atoms.value, value)) { regs[2] = Value::Bool(true); return false; }
  *done = false;
  return true;
}

bool iter_step(Context& ctx, Value* regs, bool* done, Value* value) {
  if (regs[2].boolean) { *done = true; return true; }
  Value r;
  regs[2] = Value::Bool(true);   // until the step succeeds an exception leaves the iterator finished
  if (!ctx.call(regs[1], regs[0], nullptr, 0, &r)) return false;
  if (!iter_result_unpack(ctx, regs, r, done, value)) return false;
  if (!*done) regs[2] = Value::Bool(false);
  return true;
}

bool iter_close(Context& ctx, Value* regs, bool silent) {
  if (regs[2].boolean) return true;
  regs[2] = Value::Bool(true);
  Value ret;
  bool ok = ctx.get_method(regs[0], ctx.atoms.return_, &ret);
  if (ok && !ret.is_undefined()) {
    Value r;
    ok = ctx.call(ret, regs[0], nullptr, 0, &r);
    if (ok && !silent && !r.is_object()) {
      ctx.throw_type_error("Iterator result " + vm_describe(ctx, r) + " is not an object");
      ok = false;
    }
  }
  if (!ok && silent) ctx.clear_exception();
  return ok || silent;
}

}  // namespace

Exit vm_execute(Context& ctx, Value* result) {
  Context::VM& vm = *ctx.vm;
  Frame* f = nullptr;
  const uint8_t* ip = nullptr;
  const uint8_t* cb = nullptr;
  const Value* K = nullptr;
  Value* base = nullptr;
  Value* sp = nullptr;

#define LOAD_FRAME()                       \
  do {                                     \
    f = &vm.frames.back();                 \
    cb = f->code->code.data();             \
    K = f->code->consts.data();            \
    base = f->base;                        \
    sp = vm.sp;                            \
  } while (0)

#define THROW_IF(cond)         \
  do {                         \
    if (!(cond)) goto throw_exc; \
  } while (0)

  LOAD_FRAME();
  ip = cb + f->pc;
  bool pending_throw = vm.pending_throw;
  vm.pending_throw = false;
  if (pending_throw) ip = cb + f->pc;

  for (;;) {
    OpCode op;
    if (pending_throw) {
      pending_throw = false;
      goto throw_exc;
    }
    f->pc = static_cast<uint32_t>(ip - cb);
    vm.sp = sp;
    op = static_cast<OpCode>(*ip++);
    switch (op) {
      case OpCode::Nop: break;
      case OpCode::Pop: --sp; break;
      case OpCode::Dup: sp[0] = sp[-1]; ++sp; break;
      case OpCode::Dup2: sp[0] = sp[-2]; sp[1] = sp[-1]; sp += 2; break;
      case OpCode::Swap: { Value t = sp[-1]; sp[-1] = sp[-2]; sp[-2] = t; break; }
      case OpCode::Rot3: { Value a = sp[-3]; sp[-3] = sp[-2]; sp[-2] = sp[-1]; sp[-1] = a; break; }
      case OpCode::Rot4: { Value a = sp[-4]; sp[-4] = sp[-3]; sp[-3] = sp[-2]; sp[-2] = sp[-1]; sp[-1] = a; break; }
      case OpCode::Undefined: *sp++ = Value::Undefined(); break;
      case OpCode::Null: *sp++ = Value::Null(); break;
      case OpCode::True: *sp++ = Value::Bool(true); break;
      case OpCode::False: *sp++ = Value::Bool(false); break;
      case OpCode::Int: *sp++ = Value::Number(static_cast<double>(rdi32(ip))); break;
      case OpCode::Const: *sp++ = K[rd32(ip)]; break;

      // ---- locals and environments ----
      case OpCode::GetLocal: *sp++ = base[rd16(ip)]; break;
      case OpCode::SetLocal: base[rd16(ip)] = *--sp; break;
      case OpCode::GetLocalChecked: {
        uint16_t i = rd16(ip);
        JsString* name = K[rd32(ip)].as_string();
        if (base[i].is_empty()) { throw_tdz(ctx, name); goto throw_exc; }
        *sp++ = base[i];
        break;
      }
      case OpCode::SetLocalChecked: {
        uint16_t i = rd16(ip);
        JsString* name = K[rd32(ip)].as_string();
        if (base[i].is_empty()) { throw_tdz(ctx, name); goto throw_exc; }
        base[i] = *--sp;
        break;
      }
      case OpCode::SetLocalEmpty: base[rd16(ip)] = Value::Empty(); break;
      case OpCode::GetEnv: {
        uint16_t hops = rd16(ip), slot = rd16(ip);
        Env* e = f->env;
        while (hops--) e = e->parent;
        *sp++ = e->slots[slot];
        break;
      }
      case OpCode::SetEnv: {
        uint16_t hops = rd16(ip), slot = rd16(ip);
        Env* e = f->env;
        while (hops--) e = e->parent;
        e->slots[slot] = *--sp;
        break;
      }
      case OpCode::GetEnvChecked: {
        uint16_t hops = rd16(ip), slot = rd16(ip);
        JsString* name = K[rd32(ip)].as_string();
        Env* e = f->env;
        while (hops--) e = e->parent;
        if (e->slots[slot].is_empty()) { throw_tdz(ctx, name); goto throw_exc; }
        *sp++ = e->slots[slot];
        break;
      }
      case OpCode::SetEnvChecked: {
        uint16_t hops = rd16(ip), slot = rd16(ip);
        JsString* name = K[rd32(ip)].as_string();
        Env* e = f->env;
        while (hops--) e = e->parent;
        if (e->slots[slot].is_empty()) { throw_tdz(ctx, name); goto throw_exc; }
        e->slots[slot] = *--sp;
        break;
      }
      case OpCode::SetEnvEmpty: {
        uint16_t hops = rd16(ip), slot = rd16(ip);
        Env* e = f->env;
        while (hops--) e = e->parent;
        e->slots[slot] = Value::Empty();
        break;
      }
      case OpCode::PushEnv: {
        uint16_t n = rd16(ip);
        Env* e = ctx.heap.make<Env>();
        e->parent = f->env;
        e->slots.assign(n, Value::Undefined());
        f->env = e;
        ++f->env_depth;
        break;
      }
      case OpCode::PopEnv:
        f->env = f->env->parent;
        --f->env_depth;
        break;
      case OpCode::CopyEnv: {
        Env* e = ctx.heap.make<Env>();
        e->parent = f->env->parent;
        e->slots = f->env->slots;
        f->env = e;
        break;
      }

      // ---- globals ----
      case OpCode::GetGlobal: {
        JsString* name = K[rd32(ip)].as_string();
        Value v;
        THROW_IF(vm_get_global(ctx, name, &v));
        *sp++ = v;
        break;
      }
      case OpCode::SetGlobal: {
        JsString* name = K[rd32(ip)].as_string();
        bool strict = *ip++ != 0;
        THROW_IF(vm_set_global(ctx, name, sp[-1], strict));
        --sp;
        break;
      }
      case OpCode::TypeofGlobal: {
        JsString* name = K[rd32(ip)].as_string();
        Value v;
        bool present = false;
        THROW_IF(vm_has_global(ctx, name, &v, &present));
        *sp++ = Value::String(ctx.atom_ascii(present ? ctx.type_of(v) : "undefined"));
        break;
      }
      case OpCode::DeclareGlobalVar: THROW_IF(vm_declare_global_var(ctx, K[rd32(ip)].as_string())); break;
      case OpCode::DeclareGlobalFunc: {
        JsString* name = K[rd32(ip)].as_string();
        THROW_IF(vm_declare_global_func(ctx, name, sp[-1]));
        --sp;
        break;
      }
      case OpCode::DeclareGlobalLex: {
        JsString* name = K[rd32(ip)].as_string();
        bool is_const = *ip++ != 0;
        THROW_IF(vm_declare_global_lex(ctx, name, is_const));
        break;
      }
      case OpCode::InitGlobalLex: {
        JsString* name = K[rd32(ip)].as_string();
        ctx.global_lexicals[name] = *--sp;
        break;
      }

      // ---- properties ----
      case OpCode::GetProp: {
        PropKey key = K[rd32(ip)].cell;
        Value base_v = sp[-1];
        Value r;
        if (base_v.is_object()) {
          Object* o = base_v.as_object();
          Prop* p = (!o->exotic_props && !o->exotic_get) ? o->props.find(key) : nullptr;
          if (p && !p->is_accessor()) r = p->value;
          else THROW_IF(o->get(ctx, key, base_v, &r));
        } else {
          THROW_IF(ctx.get_v(base_v, key, &r));
        }
        sp[-1] = r;
        break;
      }
      case OpCode::SetProp: {
        PropKey key = K[rd32(ip)].cell;
        bool strict = *ip++ != 0;
        Value v = sp[-1], base_v = sp[-2];
        bool done = false;
        if (base_v.is_object()) {
          Object* o = base_v.as_object();
          if (!o->exotic_props && !o->exotic_get) {
            Prop* p = o->props.find(key);
            if (p && !p->is_accessor() && (p->attrs & kWritable)) { p->value = v; done = true; }
          }
        }
        if (!done) THROW_IF(ctx.put_v(base_v, key, v, strict));
        --sp;
        sp[-1] = v;
        break;
      }
      case OpCode::GetElem: {
        Value key_v = sp[-1], base_v = sp[-2];
        Value r;
        bool done = false;
        if (key_v.is_number() && base_v.is_object() && base_v.as_object()->cls == Class::Array) {
          ArrayObject* a = static_cast<ArrayObject*>(base_v.as_object());
          double d = key_v.num;
          if (!a->slow && d >= 0 && d < static_cast<double>(a->elems.size()) && d == static_cast<double>(static_cast<uint32_t>(d))) {
            const Value& e = a->elems[static_cast<size_t>(d)];
            if (!e.is_empty()) { r = e; done = true; }
          }
        }
        if (!done && key_v.is_number() && base_v.is_object() && base_v.as_object()->cls == Class::TypedArray) {
          if (typed_array_get(ctx, base_v.as_object(), key_v.num, &r)) done = true;
          else if (key_v.num >= 0 && key_v.num == static_cast<double>(static_cast<uint32_t>(key_v.num))) { r = Value::Undefined(); done = true; }
        }
        if (!done) {
          if (base_v.is_nullish()) {
            PropKey k;
            THROW_IF(ctx.to_property_key(key_v, &k));
            THROW_IF(ctx.get_v(base_v, k, &r));
          } else if (key_v.is_number() && key_v.num >= 0 && key_v.num < 4294967295.0 && key_v.num == static_cast<double>(static_cast<uint32_t>(key_v.num))) {
            THROW_IF(ctx.get_index(base_v, static_cast<uint32_t>(key_v.num), &r));
          } else {
            PropKey k;
            THROW_IF(ctx.to_property_key(key_v, &k));
            THROW_IF(ctx.get_v(base_v, k, &r));
          }
        }
        --sp;
        sp[-1] = r;
        break;
      }
      case OpCode::SetElem: {
        bool strict = *ip++ != 0;
        Value v = sp[-1], key_v = sp[-2], base_v = sp[-3];
        bool done = false;
        if (key_v.is_number() && base_v.is_object() && base_v.as_object()->cls == Class::Array) {
          ArrayObject* a = static_cast<ArrayObject*>(base_v.as_object());
          double d = key_v.num;
          if (!a->slow && d >= 0 && d == static_cast<double>(static_cast<uint32_t>(d))) {
            size_t i = static_cast<size_t>(d);
            if (i < a->elems.size() && !a->elems[i].is_empty()) {
              a->elems[i] = v;
              done = true;
            } else if (i == a->elems.size() && a->length_ == a->elems.size() && a->extensible && a->length_writable &&
                       !ctx.intr.array_proto->has_index_props && !ctx.intr.object_proto->has_index_props && a->proto == ctx.intr.array_proto) {
              a->elems.push_back(v);
              ++a->length_;
              done = true;
            }
          }
        }
        if (!done && key_v.is_number() && base_v.is_object() && base_v.as_object()->cls == Class::TypedArray) {
          bool handled;
          THROW_IF(typed_array_set(ctx, base_v.as_object(), key_v.num, v, &handled));
          done = handled;
        }
        if (!done) {
          PropKey k;
          THROW_IF(ctx.to_property_key(key_v, &k));
          THROW_IF(ctx.put_v(base_v, k, v, strict));
        }
        sp -= 2;
        sp[-1] = v;
        break;
      }
      case OpCode::DeleteProp: {
        PropKey key = K[rd32(ip)].cell;
        bool strict = *ip++ != 0;
        Object* o;
        THROW_IF(ctx.to_object(sp[-1], &o));
        bool ok = true;
        THROW_IF(ctx.delete_prop(o, key, &ok));
        if (!ok && strict) {
          ctx.throw_type_error("Cannot delete property '" + static_cast<JsString*>(key)->utf8() + "' of " + vm_describe(ctx, sp[-1]));
          goto throw_exc;
        }
        sp[-1] = Value::Bool(ok);
        break;
      }
      case OpCode::DeleteElem: {
        bool strict = *ip++ != 0;
        Object* o;
        THROW_IF(ctx.to_object(sp[-2], &o));
        PropKey k;
        THROW_IF(ctx.to_property_key(sp[-1], &k));
        bool ok = true;
        THROW_IF(ctx.delete_prop(o, k, &ok));
        if (!ok && strict) {
          ctx.throw_type_error("Cannot delete property of " + vm_describe(ctx, sp[-2]));
          goto throw_exc;
        }
        --sp;
        sp[-1] = Value::Bool(ok);
        break;
      }
      case OpCode::GetSuperProp: {
        PropKey k;
        THROW_IF(ctx.to_property_key(sp[-1], &k));
        Value r;
        THROW_IF(vm_get_super(ctx, *f, sp[-2], k, &r));
        --sp;
        sp[-1] = r;
        break;
      }
      case OpCode::SetSuperProp: {
        bool strict = *ip++ != 0;
        PropKey k;
        THROW_IF(ctx.to_property_key(sp[-2], &k));
        Value v = sp[-1];
        THROW_IF(vm_set_super(ctx, *f, sp[-3], k, v, strict));
        sp -= 2;
        sp[-1] = v;
        break;
      }
      case OpCode::In: {
        Value key_v = sp[-2], obj = sp[-1];
        if (!obj.is_object()) {
          ctx.throw_type_error("Cannot use 'in' operator to search for '" + (key_v.is_symbol() ? std::string("symbol") : [&] {
            JsString* s = nullptr;
            ctx.to_string(key_v, &s);
            ctx.clear_exception();
            return s ? s->utf8() : std::string();
          }()) + "' in " + vm_describe(ctx, obj));
          goto throw_exc;
        }
        PropKey k;
        THROW_IF(ctx.to_property_key(key_v, &k));
        bool has;
        THROW_IF(obj.as_object()->has_property(ctx, k, &has));
        --sp;
        sp[-1] = Value::Bool(has);
        break;
      }
      case OpCode::InstanceOf: {
        bool r;
        THROW_IF(ctx.instance_of(sp[-2], sp[-1], &r));
        --sp;
        sp[-1] = Value::Bool(r);
        break;
      }

      // ---- literals ----
      case OpCode::NewObject: *sp++ = Value::Obj(ctx.new_object()); break;
      case OpCode::NewArray: {
        uint32_t n = rd32(ip);
        ArrayObject* a = ctx.new_array(0);
        a->elems.reserve(n);
        *sp++ = Value::Obj(a);
        break;
      }
      case OpCode::ArrayPush: {
        ArrayObject* a = static_cast<ArrayObject*>(sp[-2].as_object());
        a->elems.push_back(sp[-1]);
        ++a->length_;
        --sp;
        break;
      }
      case OpCode::ArrayHole: {
        ArrayObject* a = static_cast<ArrayObject*>(sp[-1].as_object());
        a->elems.push_back(Value::Empty());
        ++a->length_;
        break;
      }
      case OpCode::ArraySpread: {
        ArrayObject* a = static_cast<ArrayObject*>(sp[-2].as_object());
        Value src = sp[-1];
        THROW_IF(ctx.iterate(src, [&](Value v) {
          a->elems.push_back(v);
          ++a->length_;
          return true;
        }));
        --sp;
        break;
      }
      case OpCode::ObjectSpread: {
        THROW_IF(vm_copy_data_properties(ctx, sp[-2].as_object(), sp[-1], nullptr));
        --sp;
        break;
      }
      case OpCode::SetProto: {
        Value p = sp[-1];
        if (p.is_object() || p.is_null()) sp[-2].as_object()->proto = p.is_object() ? p.as_object() : nullptr;
        --sp;
        break;
      }
      case OpCode::DefineField: {
        PropKey key = K[rd32(ip)].cell;
        if (!sp[-2].is_object()) { ctx.throw_type_error("Cannot define a field on a value that is not an object"); goto throw_exc; }
        THROW_IF(ctx.create_data_property(sp[-2].as_object(), key, sp[-1]));
        --sp;
        break;
      }
      case OpCode::DefineFieldComputed: {
        PropKey key;
        THROW_IF(ctx.to_property_key(sp[-2], &key));
        if (!sp[-3].is_object()) { ctx.throw_type_error("Cannot define a field on a value that is not an object"); goto throw_exc; }
        THROW_IF(ctx.create_data_property(sp[-3].as_object(), key, sp[-1]));
        sp -= 2;
        break;
      }
      case OpCode::DefineMethod: case OpCode::DefineGetter: case OpCode::DefineSetter: {
        PropKey key = K[rd32(ip)].cell;
        bool enumerable = (*ip++ & 1) != 0;
        Object* target = sp[-2].as_object();
        Object* fn = sp[-1].as_object();
        const Value key_v = Value::String(static_cast<JsString*>(key));
        if (op == OpCode::DefineMethod) {
          bool ok = false;
          THROW_IF(target->define_own_property(ctx, key, PropDesc::Data(Value::Obj(fn), enumerable ? kDefaultAttrs : kHiddenAttrs), &ok));
        } else {
          THROW_IF(vm_set_function_name(ctx, fn, key_v, op == OpCode::DefineGetter ? 1 : 2));
          THROW_IF(vm_define_accessor_half(ctx, target, key, fn, op == OpCode::DefineGetter, enumerable));
        }
        --sp;
        break;
      }
      case OpCode::DefineMethodComputed: case OpCode::DefineGetterComputed: case OpCode::DefineSetterComputed: {
        bool enumerable = (*ip++ & 1) != 0;
        PropKey key;
        THROW_IF(ctx.to_property_key(sp[-2], &key));
        Object* target = sp[-3].as_object();
        Object* fn = sp[-1].as_object();
        if (op == OpCode::DefineMethodComputed) {
          bool ok = false;
          THROW_IF(target->define_own_property(ctx, key, PropDesc::Data(Value::Obj(fn), enumerable ? kDefaultAttrs : kHiddenAttrs), &ok));
        } else {
          THROW_IF(vm_define_accessor_half(ctx, target, key, fn, op == OpCode::DefineGetterComputed, enumerable));
        }
        sp -= 2;
        break;
      }
      case OpCode::SetHomeObject: {
        uint8_t between = *ip++;
        Value home = sp[-2 - between];
        static_cast<Function*>(sp[-1].as_object())->home_object = home.as_object();
        break;
      }
      case OpCode::NameFunction: {
        uint8_t kind = *ip++;
        THROW_IF(vm_set_function_name(ctx, sp[-1].as_object(), sp[-2], kind));
        break;
      }
      case OpCode::Closure: {
        FunctionCode* fc = f->code->functions[rd32(ip)];
        *sp++ = Value::Obj(vm_make_closure(ctx, *f, fc));
        break;
      }
      case OpCode::CreateClass: {
        FunctionCode* fc = f->code->functions[rd32(ip)];
        bool has_super = *ip++ != 0;
        uint16_t ctor_reg = rd16(ip), proto_reg = rd16(ip);
        Value super_v;
        if (has_super) super_v = sp[-1];
        Function* ctor;
        Object* proto;
        THROW_IF(vm_create_class(ctx, *f, fc, has_super, super_v, &ctor, &proto));
        if (has_super) --sp;
        base[ctor_reg] = Value::Obj(ctor);
        base[proto_reg] = Value::Obj(proto);
        break;
      }
      case OpCode::ClassFields: {
        uint16_t ctor_reg = rd16(ip);
        static_cast<Function*>(base[ctor_reg].as_object())->fields = sp[-1].as_object();
        --sp;
        break;
      }
      case OpCode::NewPrivateName: {
        JsString* nm = K[rd32(ip)].as_string();
        Symbol* s = ctx.new_symbol(ctx.new_string(u"#" + nm->str()));
        s->is_private = true;
        *sp++ = Value::Sym(s);
        break;
      }
      case OpCode::PrivateGet: {
        Value r;
        THROW_IF(vm_private_get(ctx, sp[-2], sp[-1], &r));
        --sp;
        sp[-1] = r;
        break;
      }
      case OpCode::PrivateSet: {
        Value v = sp[-1];
        THROW_IF(vm_private_set(ctx, sp[-3], sp[-2], v));
        sp -= 2;
        sp[-1] = v;
        break;
      }
      case OpCode::PrivateDefine: {
        uint8_t kind = *ip++;
        THROW_IF(vm_private_define(ctx, sp[-3], sp[-2], sp[-1], kind));
        sp -= 3;
        break;
      }
      case OpCode::PrivateIn: {
        Value name = sp[-2], obj = sp[-1];
        if (!obj.is_object()) {
          ctx.throw_type_error("Cannot use 'in' operator to search for a private field in " + vm_describe(ctx, obj));
          goto throw_exc;
        }
        bool has = obj.as_object()->props.find(name.cell) != nullptr;
        --sp;
        sp[-1] = Value::Bool(has);
        break;
      }

      // ---- calls ----
      case OpCode::Call: case OpCode::CallSpread: {
        int argc;
        if (op == OpCode::Call) {
          argc = rd16(ip);
        } else {
          ArrayObject* arr = static_cast<ArrayObject*>(sp[-1].as_object());
          --sp;
          argc = static_cast<int>(arr->elems.size());
          if (sp + argc + 16 > vm.stack + vm.capacity) { ctx.throw_range_error("Maximum call stack size exceeded"); goto throw_exc; }
          for (int i = 0; i < argc; ++i) *sp++ = arr->elems[static_cast<size_t>(i)].is_empty() ? Value::Undefined() : arr->elems[static_cast<size_t>(i)];
        }
        if (ctx.heap.should_collect()) { vm.sp = sp; ctx.maybe_gc(); }
        if (ctx.step_budget && ++ctx.steps > ctx.step_budget) {
          ctx.step_budget_exceeded = true;
          ctx.throw_error(ErrorKind::Error, "Script execution budget exceeded");
          goto throw_exc;
        }
        Value* args = sp - argc;
        Value callee = args[-2];
        if (!callee.is_callable()) {
          std::string what = vm_describe(ctx, callee);
          const uint32_t at = static_cast<uint32_t>(ip - cb);
          for (const auto& cs : f->code->call_sites)
            if (cs.first == at) { if (cs.second < f->code->consts.size() && f->code->consts[cs.second].is_string()) what = f->code->consts[cs.second].as_string()->utf8(); break; }
          ctx.throw_type_error(what + " is not a function");
          goto throw_exc;
        }
        Object* co = callee.as_object();
        Function* cf = co->cls == Class::Function ? static_cast<Function*>(co) : nullptr;
        if (cf && cf->kind == Function::Kind::Closure && !cf->is_class && !cf->code->is_generator && !cf->code->is_async) {
          f->ret_pc = static_cast<uint32_t>(ip - cb);
          vm.sp = sp;
          THROW_IF(vm_push_frame(ctx, cf, argc, args[-1], Value::Undefined(), false, false, nullptr));
          LOAD_FRAME();
          ip = cb;
          break;
        }
        Value r;
        f->ret_pc = static_cast<uint32_t>(ip - cb);
        THROW_IF(co->call_internal(ctx, args[-1], args, argc, &r));
        sp = args - 2;
        *sp++ = r;
        break;
      }
      case OpCode::New: case OpCode::NewSpread: {
        int argc;
        if (op == OpCode::New) {
          argc = rd16(ip);
        } else {
          ArrayObject* arr = static_cast<ArrayObject*>(sp[-1].as_object());
          --sp;
          argc = static_cast<int>(arr->elems.size());
          if (sp + argc + 16 > vm.stack + vm.capacity) { ctx.throw_range_error("Maximum call stack size exceeded"); goto throw_exc; }
          for (int i = 0; i < argc; ++i) *sp++ = arr->elems[static_cast<size_t>(i)].is_empty() ? Value::Undefined() : arr->elems[static_cast<size_t>(i)];
        }
        if (ctx.heap.should_collect()) { vm.sp = sp; ctx.maybe_gc(); }
        // make room for a `this` slot between the constructor and its arguments
        Value* args = sp - argc;
        for (int i = argc; i > 0; --i) args[i] = args[i - 1];
        args[0] = Value::Undefined();
        ++sp;
        vm.sp = sp;
        Value callee = args[-1];
        if (!callee.is_object() || !callee.as_object()->is_constructor) {
          ctx.throw_type_error(vm_describe(ctx, callee) + " is not a constructor");
          goto throw_exc;
        }
        Object* co = callee.as_object();
        f->ret_pc = static_cast<uint32_t>(ip - cb);
        Function* cf = co->cls == Class::Function ? static_cast<Function*>(co) : nullptr;
        if (cf && cf->kind == Function::Kind::Closure && !cf->code->is_generator && !cf->code->is_async) {
          Value this_v;
          THROW_IF(vm_construct_this(ctx, cf, callee, &this_v));
          args[0] = this_v;
          THROW_IF(vm_push_frame(ctx, cf, argc, this_v, callee, false, true, nullptr));
          LOAD_FRAME();
          ip = cb;
          break;
        }
        Value r;
        THROW_IF(co->construct_internal(ctx, args + 1, argc, callee, &r));
        sp = args - 1;
        *sp++ = r;
        break;
      }
      case OpCode::SuperCall: case OpCode::SuperCallSpread: {
        int argc;
        if (op == OpCode::SuperCall) {
          argc = rd16(ip);
        } else {
          ArrayObject* arr = static_cast<ArrayObject*>(sp[-1].as_object());
          --sp;
          argc = static_cast<int>(arr->elems.size());
          if (sp + argc + 16 > vm.stack + vm.capacity) { ctx.throw_range_error("Maximum call stack size exceeded"); goto throw_exc; }
          for (int i = 0; i < argc; ++i) *sp++ = arr->elems[static_cast<size_t>(i)].is_empty() ? Value::Undefined() : arr->elems[static_cast<size_t>(i)];
        }
        vm.sp = sp;
        Function* ctor = active_ctor(*f);
        Object* parent = nullptr;
        if (!ctor || !ctor->get_prototype_of(ctx, &parent)) {
          if (!ctor) ctx.throw_syntax_error("'super' keyword unexpected here");
          goto throw_exc;
        }
        if (!parent || !parent->is_constructor) {
          ctx.throw_type_error("Super constructor " + (parent ? vm_describe(ctx, Value::Obj(parent)) : std::string("null")) + " of anonymous class is not a constructor");
          goto throw_exc;
        }
        Value* args = sp - argc;
        Value r;
        f->ret_pc = static_cast<uint32_t>(ip - cb);
        THROW_IF(parent->construct_internal(ctx, args, argc, f->new_target, &r));
        sp = args;
        *sp++ = r;
        break;
      }
      case OpCode::RunFieldInit: {
        Function* ctor = active_ctor(*f);
        if (ctor) THROW_IF(vm_init_instance(ctx, ctor, sp[-1].as_object()));
        break;
      }
      case OpCode::Return: {
        Value v = sp[-1];
        if (f->construct && !v.is_object()) v = f->this_v;
        goto do_return_value;
      do_return_value:
        {
          const bool entry = f->entry;
          Coroutine* co = f->co;
          (void)co;
          vm.sp = f->base - 2;
          vm.frames.pop_back();
          if (entry) {
            *result = v;
            return Exit::Return;
          }
          LOAD_FRAME();
          *sp++ = v;
          ip = cb + f->ret_pc;
        }
        break;
      }
      case OpCode::ReturnCtor: {
        Value this_v = sp[-1], v = sp[-2];
        Value r;
        if (v.is_object()) r = v;
        else if (!v.is_undefined()) {
          ctx.throw_type_error("Derived constructors may only return object or undefined");
          goto throw_exc;
        } else if (this_v.is_empty()) {
          ctx.throw_reference_error("Must call super constructor in derived class before accessing 'this' or returning from derived constructor");
          goto throw_exc;
        } else {
          r = this_v;
        }
        const bool entry = f->entry;
        vm.sp = f->base - 2;
        vm.frames.pop_back();
        if (entry) {
          *result = r;
          return Exit::Return;
        }
        LOAD_FRAME();
        *sp++ = r;
        ip = cb + f->ret_pc;
        break;
      }
      case OpCode::Throw: {
        ctx.throw_value(sp[-1]);
        goto throw_exc;
      }
      case OpCode::ThrowError: {
        uint8_t kind = *ip++;
        JsString* msg = K[rd32(ip)].as_string();
        ctx.throw_error(static_cast<ErrorKind>(kind), msg->utf8());
        goto throw_exc;
      }
      case OpCode::ThrowConstAssign:
        rd32(ip);
        ctx.throw_type_error("Assignment to constant variable.");
        goto throw_exc;

      // ---- jumps ----
      case OpCode::Jump: {
        int32_t rel = rdi32(ip);
        ip += rel;
        if (rel < 0) {
          if (ctx.heap.should_collect()) { vm.sp = sp; ctx.maybe_gc(); }
          if (ctx.step_budget && ++ctx.steps > ctx.step_budget) {
            ctx.step_budget_exceeded = true;
            ctx.throw_error(ErrorKind::Error, "Script execution budget exceeded");
            goto throw_exc;
          }
        }
        break;
      }
      case OpCode::JumpIfFalse: {
        int32_t rel = rdi32(ip);
        Value v = *--sp;
        if (!(v.is_bool() ? v.boolean : Context::to_boolean(v))) ip += rel;
        break;
      }
      case OpCode::JumpIfTrue: {
        int32_t rel = rdi32(ip);
        Value v = *--sp;
        if (v.is_bool() ? v.boolean : Context::to_boolean(v)) {
          ip += rel;
          if (rel < 0) {
            if (ctx.heap.should_collect()) { vm.sp = sp; ctx.maybe_gc(); }
            if (ctx.step_budget && ++ctx.steps > ctx.step_budget) {
              ctx.step_budget_exceeded = true;
              ctx.throw_error(ErrorKind::Error, "Script execution budget exceeded");
              goto throw_exc;
            }
          }
        }
        break;
      }
      case OpCode::JumpIfFalseKeep: {
        int32_t rel = rdi32(ip);
        if (!Context::to_boolean(sp[-1])) ip += rel;
        else --sp;
        break;
      }
      case OpCode::JumpIfTrueKeep: {
        int32_t rel = rdi32(ip);
        if (Context::to_boolean(sp[-1])) ip += rel;
        else --sp;
        break;
      }
      case OpCode::JumpIfNotNullishKeep: {
        int32_t rel = rdi32(ip);
        if (!sp[-1].is_nullish()) ip += rel;
        else --sp;
        break;
      }
      case OpCode::JumpIfNullish: {
        int32_t rel = rdi32(ip);
        Value v = *--sp;
        if (v.is_nullish()) ip += rel;
        break;
      }
      case OpCode::JumpIfNotUndefined: {
        int32_t rel = rdi32(ip);
        if (!sp[-1].is_undefined()) ip += rel;
        else --sp;
        break;
      }

      // ---- operators ----
      case OpCode::Add: {
        Value r;
        if (sp[-2].is_number() && sp[-1].is_number()) r = Value::Number(sp[-2].num + sp[-1].num);
        else THROW_IF(vm_add(ctx, sp[-2], sp[-1], &r));
        --sp;
        sp[-1] = r;
        break;
      }
      case OpCode::Sub: case OpCode::Mul: case OpCode::Div: case OpCode::Mod: case OpCode::Exp:
      case OpCode::Shl: case OpCode::Shr: case OpCode::UShr: case OpCode::BitAnd: case OpCode::BitOr: case OpCode::BitXor: {
        Value r;
        if (sp[-2].is_number() && sp[-1].is_number()) {
          double a = sp[-2].num, b = sp[-1].num;
          switch (op) {
            case OpCode::Sub: r = Value::Number(a - b); goto arith_done;
            case OpCode::Mul: r = Value::Number(a * b); goto arith_done;
            case OpCode::Div: r = Value::Number(a / b); goto arith_done;
            case OpCode::BitAnd: r = Value::Number(static_cast<double>(static_cast<int32_t>(Context::to_int32_d(a)) & static_cast<int32_t>(Context::to_int32_d(b)))); goto arith_done;
            case OpCode::BitOr: r = Value::Number(static_cast<double>(static_cast<int32_t>(Context::to_int32_d(a)) | static_cast<int32_t>(Context::to_int32_d(b)))); goto arith_done;
            default: break;
          }
        }
        THROW_IF(vm_arith(ctx, op, sp[-2], sp[-1], &r));
      arith_done:
        --sp;
        sp[-1] = r;
        break;
      }
      case OpCode::Eq: case OpCode::Ne: {
        bool r;
        THROW_IF(ctx.loose_equals(sp[-2], sp[-1], &r));
        --sp;
        sp[-1] = Value::Bool(op == OpCode::Eq ? r : !r);
        break;
      }
      case OpCode::StrictEq: case OpCode::StrictNe: {
        bool r = Context::strict_equals(sp[-2], sp[-1]);
        --sp;
        sp[-1] = Value::Bool(op == OpCode::StrictEq ? r : !r);
        break;
      }
      case OpCode::Lt: case OpCode::Gt: case OpCode::Le: case OpCode::Ge: {
        bool r;
        if (sp[-2].is_number() && sp[-1].is_number()) {
          double a = sp[-2].num, b = sp[-1].num;
          r = op == OpCode::Lt ? a < b : op == OpCode::Gt ? a > b : op == OpCode::Le ? a <= b : a >= b;
        } else {
          THROW_IF(vm_compare(ctx, op, sp[-2], sp[-1], &r));
        }
        --sp;
        sp[-1] = Value::Bool(r);
        break;
      }
      case OpCode::Neg: {
        if (sp[-1].is_number()) { sp[-1] = Value::Number(-sp[-1].num); break; }
        Value n;
        THROW_IF(ctx.to_numeric(sp[-1], &n));
        sp[-1] = n.is_bigint() ? Value::Big(ctx.new_bigint(big::negate(n.as_bigint()->n))) : Value::Number(-n.num);
        break;
      }
      case OpCode::Pos: {
        double d;
        if (sp[-1].is_number()) d = sp[-1].num;
        else THROW_IF(ctx.to_number(sp[-1], &d));
        sp[-1] = Value::Number(d);
        break;
      }
      case OpCode::Not: sp[-1] = Value::Bool(!Context::to_boolean(sp[-1])); break;
      case OpCode::BitNot: {
        Value n;
        THROW_IF(ctx.to_numeric(sp[-1], &n));
        if (n.is_bigint()) sp[-1] = Value::Big(ctx.new_bigint(big::bit_not(n.as_bigint()->n)));
        else sp[-1] = Value::Number(static_cast<double>(~static_cast<int32_t>(Context::to_int32_d(n.num))));
        break;
      }
      case OpCode::TypeOf: sp[-1] = Value::String(ctx.atom_ascii(ctx.type_of(sp[-1]))); break;
      case OpCode::Inc: case OpCode::Dec: {
        if (sp[-1].is_number()) { sp[-1] = Value::Number(op == OpCode::Inc ? sp[-1].num + 1 : sp[-1].num - 1); break; }
        Value n;
        THROW_IF(ctx.to_numeric(sp[-1], &n));
        if (n.is_bigint()) {
          const BigNum one = big::from_u64(1);
          sp[-1] = Value::Big(ctx.new_bigint(op == OpCode::Inc ? big::add(n.as_bigint()->n, one) : big::sub(n.as_bigint()->n, one)));
        } else {
          sp[-1] = Value::Number(op == OpCode::Inc ? n.num + 1 : n.num - 1);
        }
        break;
      }
      case OpCode::ToNumeric: {
        if (!sp[-1].is_number()) {
          Value n;
          THROW_IF(ctx.to_numeric(sp[-1], &n));
          sp[-1] = n;
        }
        break;
      }
      case OpCode::ToPropertyKey: {
        if (!sp[-1].is_string() && !sp[-1].is_symbol()) {
          PropKey k;
          THROW_IF(ctx.to_property_key(sp[-1], &k));
          sp[-1] = k->kind == CellKind::String ? Value::String(static_cast<JsString*>(k)) : Value::Sym(static_cast<Symbol*>(k));
        }
        break;
      }
      case OpCode::ToString: {
        if (!sp[-1].is_string()) {
          JsString* s;
          THROW_IF(ctx.to_string(sp[-1], &s));
          sp[-1] = Value::String(s);
        }
        break;
      }
      case OpCode::Void: sp[-1] = Value::Undefined(); break;

      // ---- function context ----
      case OpCode::LoadThis: *sp++ = f->this_v; break;
      case OpCode::LoadThisChecked:
        if (f->this_v.is_empty()) {
          ctx.throw_reference_error("Must call super constructor in derived class before accessing 'this' or returning from derived constructor");
          goto throw_exc;
        }
        *sp++ = f->this_v;
        break;
      case OpCode::LoadNewTarget: *sp++ = f->new_target; break;
      case OpCode::LoadFunction: *sp++ = Value::Obj(f->fn); break;
      case OpCode::LoadHome: *sp++ = f->fn->home_object ? Value::Obj(f->fn->home_object) : Value::Undefined(); break;
      case OpCode::InitThisLocal: {
        uint16_t i = rd16(ip);
        if (!base[i].is_empty()) {
          ctx.throw_reference_error("Super constructor may only be called once");
          goto throw_exc;
        }
        base[i] = *--sp;
        break;
      }
      case OpCode::InitThisEnv: {
        uint16_t hops = rd16(ip), slot = rd16(ip);
        Env* e = f->env;
        while (hops--) e = e->parent;
        if (!e->slots[slot].is_empty()) {
          ctx.throw_reference_error("Super constructor may only be called once");
          goto throw_exc;
        }
        e->slots[slot] = *--sp;
        break;
      }
      case OpCode::GlobalObject: *sp++ = Value::Obj(ctx.intr.global); break;
      case OpCode::CreateArguments: *sp++ = Value::Obj(vm_make_arguments(ctx, *f)); break;
      case OpCode::CreateRest: {
        rd16(ip);
        std::vector<Value> rest(f->extra.begin(), f->extra.end());
        *sp++ = Value::Obj(ctx.new_array_from(rest));
        break;
      }
      case OpCode::GetTemplateObject: *sp++ = Value::Obj(vm_template_object(ctx, f->code, rd32(ip))); break;
      case OpCode::NewRegExp: {
        JsString* pattern = K[rd32(ip)].as_string();
        JsString* flags = K[rd32(ip)].as_string();
        Object* re = regexp_create(ctx, pattern, flags);
        THROW_IF(re != nullptr);
        *sp++ = Value::Obj(re);
        break;
      }
      case OpCode::RequireObjectCoercible:
        if (sp[-1].is_nullish()) {
          ctx.throw_type_error("Cannot destructure '" + vm_describe(ctx, sp[-1]) + "' as it is " + (sp[-1].is_null() ? "null." : "undefined."));
          goto throw_exc;
        }
        break;
      case OpCode::CopyDataProps: {
        std::vector<PropKey> excluded;
        ArrayObject* ex = static_cast<ArrayObject*>(sp[-1].as_object());
        for (const Value& v : ex->elems) {
          PropKey k;
          THROW_IF(ctx.to_property_key(v, &k));
          excluded.push_back(k);
        }
        Object* target = ctx.new_object();
        THROW_IF(vm_copy_data_properties(ctx, target, sp[-2], &excluded));
        --sp;
        sp[-1] = Value::Obj(target);
        break;
      }

      // ---- iteration ----
      case OpCode::IterInit: case OpCode::AsyncIterInit: {
        uint16_t t = rd16(ip);
        THROW_IF(iter_init(ctx, base + t, sp[-1], op == OpCode::AsyncIterInit));
        --sp;
        break;
      }
      case OpCode::IterNext: {
        uint16_t t = rd16(ip);
        int32_t rel = rdi32(ip);
        bool done;
        Value v;
        THROW_IF(iter_step(ctx, base + t, &done, &v));
        if (done) ip += rel;
        else *sp++ = v;
        break;
      }
      case OpCode::IterNextValue: {
        uint16_t t = rd16(ip);
        bool done;
        Value v;
        THROW_IF(iter_step(ctx, base + t, &done, &v));
        *sp++ = done ? Value::Undefined() : v;
        break;
      }
      case OpCode::IterRest: {
        uint16_t t = rd16(ip);
        std::vector<Value> rest;
        for (;;) {
          bool done;
          Value v;
          THROW_IF(iter_step(ctx, base + t, &done, &v));
          if (done) break;
          rest.push_back(v);
        }
        *sp++ = Value::Obj(ctx.new_array_from(rest));
        break;
      }
      case OpCode::IterClose: THROW_IF(iter_close(ctx, base + rd16(ip), false)); break;
      case OpCode::IterCloseSilent: iter_close(ctx, base + rd16(ip), true); break;
      case OpCode::IterCallNext: {
        uint16_t t = rd16(ip);
        Value* regs = base + t;
        Value r;
        regs[2] = Value::Bool(true);
        THROW_IF(ctx.call(regs[1], regs[0], nullptr, 0, &r));
        regs[2] = Value::Bool(false);
        *sp++ = r;
        break;
      }
      case OpCode::IterResult: {
        uint16_t t = rd16(ip);
        int32_t rel = rdi32(ip);
        bool done;
        Value v;
        Value r = *--sp;
        THROW_IF(iter_result_unpack(ctx, base + t, r, &done, &v));
        if (done) ip += rel;
        else *sp++ = v;
        break;
      }
      case OpCode::ForInInit: {
        uint16_t t = rd16(ip);
        Object* st = vm_for_in_start(ctx, sp[-1]);
        THROW_IF(st != nullptr);
        base[t] = Value::Obj(st);
        --sp;
        break;
      }
      case OpCode::ForInNext: {
        uint16_t t = rd16(ip);
        int32_t rel = rdi32(ip);
        bool done;
        Value key;
        THROW_IF(vm_for_in_next(ctx, base[t].as_object(), &key, &done));
        if (done) ip += rel;
        else *sp++ = key;
        break;
      }

      // ---- generators and async functions ----
      case OpCode::InitialYield:
        vm_suspend(ctx, *f, sp, static_cast<uint32_t>(ip - cb), f->pc, false, false);
        *result = Value::Undefined();
        return Exit::Yield;
      case OpCode::Yield: case OpCode::AsyncGenYield: {
        Value v = sp[-1];
        vm_suspend(ctx, *f, sp - 1, static_cast<uint32_t>(ip - cb), f->pc, false, false);
        *result = v;
        return Exit::Yield;
      }
      case OpCode::Await: {
        Value v = sp[-1];
        vm_suspend(ctx, *f, sp - 1, static_cast<uint32_t>(ip - cb), f->pc, true, false);
        *result = v;
        return Exit::Await;
      }
      case OpCode::AwaitIfSync: {
        uint16_t t = rd16(ip);
        if (!base[t + 3].boolean) break;
        Value v = sp[-1];
        vm_suspend(ctx, *f, sp - 1, static_cast<uint32_t>(ip - cb), f->pc, true, false);
        *result = v;
        return Exit::Await;
      }
      case OpCode::YieldStar: {
        uint16_t t = rd16(ip);
        Value* regs = base + t;
        Coroutine* co = f->co;
        const bool async_gen = co && co->kind == CoKind::AsyncGenerator;
        int mode = 0;
        Value received;
        if (co->in_yield_star) {
          mode = f->rmode;
          received = f->rvalue;
        }
        co->in_yield_star = false;
        Value r;
        if (mode == 0) {
          THROW_IF(ctx.call(regs[1], regs[0], {received}, &r));
        } else if (mode == 1) {
          Value m;
          THROW_IF(ctx.get_method(regs[0], ctx.atoms.throw_, &m));
          if (m.is_undefined()) {
            iter_close(ctx, regs, true);
            ctx.throw_type_error("The iterator does not provide a 'throw' method");
            goto throw_exc;
          }
          THROW_IF(ctx.call(m, regs[0], {received}, &r));
        } else {
          Value m;
          THROW_IF(ctx.get_method(regs[0], ctx.atoms.return_, &m));
          if (m.is_undefined()) {
            ctx.set_exception(Value::Obj(vm.return_sentinel));
            co->return_value = received;
            goto throw_exc;
          }
          THROW_IF(ctx.call(m, regs[0], {received}, &r));
        }
        if (async_gen && !co->awaiting) {
          // the inner result is a promise for the result object: await it, then come back to this instruction
          co->awaiting = false;
        }
        if (!r.is_object()) {
          ctx.throw_type_error("Iterator result " + vm_describe(ctx, r) + " is not an object");
          goto throw_exc;
        }
        Value d;
        THROW_IF(ctx.get(r, ctx.atoms.done, &d));
        if (Context::to_boolean(d)) {
          Value v;
          THROW_IF(ctx.get(r, ctx.atoms.value, &v));
          if (mode == 2) {
            ctx.set_exception(Value::Obj(vm.return_sentinel));
            co->return_value = v;
            goto throw_exc;
          }
          *sp++ = v;
          break;
        }
        // yield the inner result as it is (a sync generator) or its value (an async generator)
        if (async_gen) {
          Value v;
          THROW_IF(ctx.get(r, ctx.atoms.value, &v));
          vm_suspend(ctx, *f, sp, static_cast<uint32_t>(ip - cb) - 3, f->pc, false, true);
          *result = v;
        } else {
          vm_suspend(ctx, *f, sp, static_cast<uint32_t>(ip - cb) - 3, f->pc, false, true);
          *result = r;
        }
        return Exit::Yield;
      }

      case OpCode::Debugger: break;
      case OpCode::CheckPrivateBrand: break;
      case OpCode::End: case OpCode::Count:
      default:
        ctx.throw_error(ErrorKind::Error, std::string("bad opcode ") + opcode_name(op));
        goto throw_exc;
    }
    continue;

  throw_exc: {
    vm.sp = sp;
    for (;;) {
      Frame* fr = &vm.frames.back();
      const Value ex = ctx.exception();
      const bool is_ret = ex.is_object() && ex.as_object() == vm.return_sentinel;
      const Handler* found = nullptr;
      if (!ctx.step_budget_exceeded) {
        for (const Handler& h : fr->code->handlers) {
          if (fr->pc >= h.start && fr->pc < h.end && (h.is_finally || !is_ret)) { found = &h; break; }
        }
      }
      if (found) {
        while (fr->env_depth > found->env_depth) {
          fr->env = fr->env->parent;
          --fr->env_depth;
        }
        f = fr;
        cb = f->code->code.data();
        K = f->code->consts.data();
        base = f->base;
        sp = base + f->code->nlocals;
        *sp++ = ctx.take_exception();
        ip = cb + found->target;
        break;
      }
      const bool entry = fr->entry;
      Coroutine* co = fr->co;
      vm.sp = fr->base - 2;
      vm.frames.pop_back();
      if (entry) {
        if (is_ret && co) {
          *result = co->return_value;
          ctx.clear_exception();
          return Exit::Return;
        }
        return Exit::Throw;
      }
    }
  }
  }
}

// ---------------------------------------------------------------------------------------------------
// entry from native code
// ---------------------------------------------------------------------------------------------------

bool vm_call_closure(Context& ctx, Function* f, const Value& this_v, const Value* argv, int argc, const Value& new_target, Value* out) {
  Context::VM& vm = *ctx.vm;
  FunctionCode* code = f->code;
  if (vm.frames.size() >= kMaxFrames) {
    ctx.throw_range_error("Maximum call stack size exceeded");
    return false;
  }
  if (!vm_reserve(ctx, static_cast<size_t>(argc) + 2 + code->nlocals + code->max_stack)) return false;
  Value* start_sp = vm.sp;
  Value* sp = vm.sp;
  const bool construct = !new_target.is_undefined();
  Value this_val = this_v;
  const bool nested = !vm.frames.empty();
  if (nested) ++ctx.native_depth;
  struct Leave {
    Context& c;
    bool nested;
    ~Leave() { if (nested) --c.native_depth; }
  } leave{ctx, nested};
  // a base-class construct creates `this` first; it must be rooted before the arguments are written
  sp[0] = Value::Obj(f);
  sp[1] = Value::Undefined();
  vm.sp = sp + 2;
  if (construct && !code->is_generator && !code->is_async) {
    if (!vm_construct_this(ctx, f, new_target, &this_val)) { vm.sp = start_sp; return false; }
  }
  sp[1] = this_val;
  for (int i = 0; i < argc; ++i) sp[2 + i] = argv[i];
  vm.sp = sp + 2 + argc;
  if (code->is_generator || code->is_async) {
    bool ok = vm_start_coroutine(ctx, f, this_val, argc, out);
    vm.sp = start_sp;
    return ok;
  }
  if (!vm_push_frame(ctx, f, argc, this_val, new_target, true, construct, nullptr)) {
    vm.sp = start_sp;
    return false;
  }
  Value r;
  Exit e = vm_execute(ctx, &r);
  vm.sp = start_sp;
  if (e == Exit::Throw) return false;
  *out = r;
  return true;
}

// ---------------------------------------------------------------------------------------------------
// scripts
// ---------------------------------------------------------------------------------------------------

namespace {
void syntax_error_at(Context& ctx, const std::string& msg, const std::string& file, uint32_t line) {
  ctx.throw_syntax_error(msg + " (" + file + ":" + std::to_string(line) + ")");
}
}  // namespace

bool vm_eval_source(Context& ctx, const std::u16string& source, const std::string& filename, bool strict, Value* out) {
  auto src = std::make_shared<const std::u16string>(source);
  Program prog;
  ParseOptions opt;
  opt.strict = strict;
  ParseError perr;
  if (!parse_program(src, opt, prog, &perr)) {
    syntax_error_at(ctx, perr.message, filename, perr.line);
    return false;
  }
  Analysis an;
  if (!analyze_program(prog, an)) {
    syntax_error_at(ctx, an.error, filename, an.error_line);
    return false;
  }
  std::string err;
  uint32_t err_line = 0;
  FunctionCode* code = compile_program(ctx, prog, an, filename, &err, &err_line);
  if (!code) {
    syntax_error_at(ctx, err, filename, err_line);
    return false;
  }
  Context::VM& vm = *ctx.vm;
#ifndef AIOS_METAL
  if (!aios::hal::process_host().env("AIOS_JS_DUMP").empty()) aios::hal::host_note(vm_disassemble(code));
#endif
  if (!vm_reserve(ctx, 2 + code->nlocals + code->max_stack)) return false;
  Function* fn = ctx.heap.make<Function>();
  fn->kind = Function::Kind::Closure;
  fn->code = code;
  fn->proto = ctx.intr.function_proto;
  Value* start_sp = vm.sp;
  vm.sp[0] = Value::Obj(fn);
  vm.sp[1] = Value::Obj(ctx.intr.global);
  vm.sp += 2;
  const bool nested = !vm.frames.empty();
  if (nested) ++ctx.native_depth;
  bool ok = vm_push_frame(ctx, fn, 0, Value::Obj(ctx.intr.global), Value::Undefined(), true, false, nullptr);
  if (ok) {
    Value r;
    Exit e = vm_execute(ctx, &r);
    if (e == Exit::Throw) ok = false;
    else *out = r;
  }
  if (nested) --ctx.native_depth;
  vm.sp = start_sp;
  return ok;
}

bool vm_function_from_source(Context& ctx, const std::u16string& params, const std::u16string& body, bool is_async, bool is_generator, Value* out) {
  std::u16string text = u"(";
  if (is_async) text += u"async ";
  text += u"function";
  if (is_generator) text += u"*";
  text += u" anonymous(" + params + u"\n) {\n" + body + u"\n})";
  return vm_eval_source(ctx, text, "<anonymous>", false, out);
}

bool Context::eval_script16(const std::u16string& source, std::string_view filename, Value* result) {
  Value r;
  if (!vm_eval_source(*this, source, std::string(filename), false, &r)) return false;
  if (result) *result = r;
  return true;
}

bool Context::make_function(std::string_view params, std::string_view body, Value* out) {
  JsString* p = new_string_utf8(params);
  JsString* b = new_string_utf8(body);
  return vm_function_from_source(*this, p->str(), b->str(), false, false, out);
}

bool Context::eval_script(std::string_view utf8_source, std::string_view filename, Value* result) {
  JsString* s = new_string_utf8(utf8_source);
  return eval_script16(s->str(), filename, result);
}

}  // namespace aios::js
