#pragma once
// The bytecode VM's internal structures, shared by vm.cpp (the interpreter loop), vm_ops.cpp (operators, closures,
// classes, iteration helpers) and vm_coro.cpp (generators, async functions, async generators).
#include "bytecode.hpp"
#include "internal.hpp"

namespace aios::js {

struct Coroutine;

// One activation. Locals live on the VM stack at `base`; the operand stack grows above them.
struct Frame {
  Function* fn = nullptr;
  FunctionCode* code = nullptr;
  Env* env = nullptr;
  Value* base = nullptr;
  uint32_t pc = 0;            // start of the instruction being executed (handler lookup, line numbers)
  uint32_t ret_pc = 0;        // where to continue after a call from this frame returns
  int env_depth = 0;          // environments pushed by this frame
  uint32_t argc = 0;          // arguments actually passed
  Value this_v;
  Value new_target;
  std::vector<Value> extra;   // arguments beyond the declared parameters, kept for `arguments` and rest parameters
  Coroutine* co = nullptr;    // a generator or async function body
  int rmode = 0;              // resumption of a yield* (0 next, 1 throw, 2 return)
  Value rvalue;
  bool entry = false;         // returning from this frame leaves execute()
  bool construct = false;
};

enum class CoKind : uint8_t { Generator, Async, AsyncGenerator };
enum class CoState : uint8_t { Start, Suspended, Running, Done };

struct AsyncGenRequest {
  int mode = 0;               // 0 next, 1 throw, 2 return
  Value value;
  Object* promise = nullptr;
};

// A suspended function body: the saved frame, plus (for async functions and async generators) the promise machinery.
struct Coroutine final : Object {
  Coroutine(Class c, CoKind k) : Object(c), kind(k) {}
  CoKind kind;
  CoState state = CoState::Start;
  Function* fn = nullptr;
  Env* env = nullptr;
  int env_depth = 0;
  uint32_t pc = 0;            // where to continue
  uint32_t yield_pc = 0;      // the instruction that suspended (handler lookup when it is resumed with a throw)
  bool in_yield_star = false;
  bool awaiting = false;      // suspended at an await rather than a yield
  std::vector<Value> saved;   // locals then operand stack
  Value this_v, new_target;
  std::vector<Value> extra;
  uint32_t argc = 0;
  Value return_value;         // the value of a pending return() completion
  Object* promise = nullptr;  // an async function's result promise
  // async generators
  std::vector<AsyncGenRequest> queue;
  bool draining = false;
  void trace(Tracer& t) override;
  size_t extra_bytes() const override { return saved.capacity() * sizeof(Value); }
};

constexpr size_t kMaxFrames = 9000;

struct Context::VM {
  Value* stack = nullptr;
  size_t capacity = 0;
  Value* sp = nullptr;
  std::vector<Frame> frames;
  Object* return_sentinel = nullptr;   // the pseudo-exception that carries a generator's return() through finally blocks
  bool pending_throw = false;          // vm_execute starts by throwing the context's pending exception at the frame's pc
  VM() = default;
  VM(const VM&) = delete;
  VM& operator=(const VM&) = delete;
};

enum class Exit : uint8_t { Return, Yield, Await, Throw };

// ---- vm.cpp ----
Exit vm_execute(Context& ctx, Value* result);
bool vm_push_frame(Context& ctx, Function* fn, int argc, const Value& this_v, const Value& new_target, bool entry, bool construct, Coroutine* co);
bool vm_reserve(Context& ctx, size_t values);

// A listing of the bytecode (and of the functions nested in it), for debugging.
std::string vm_disassemble(FunctionCode* fc, int indent = 0);

// ---- vm_ops.cpp ----
std::string vm_describe(Context& ctx, const Value& v);
bool vm_add(Context& ctx, const Value& a, const Value& b, Value* out);
bool vm_arith(Context& ctx, OpCode op, const Value& a, const Value& b, Value* out);
bool vm_compare(Context& ctx, OpCode op, const Value& a, const Value& b, bool* out);
Function* vm_make_closure(Context& ctx, Frame& f, FunctionCode* fc);
Object* vm_make_arguments(Context& ctx, Frame& f);
bool vm_create_class(Context& ctx, Frame& f, FunctionCode* fc, bool has_super, Value super_v, Function** ctor_out, Object** proto_out);
bool vm_init_instance(Context& ctx, Function* ctor, Object* obj);
bool vm_construct_this(Context& ctx, Function* fn, const Value& new_target, Value* this_out);
bool vm_private_get(Context& ctx, const Value& obj, const Value& name, Value* out);
bool vm_private_set(Context& ctx, const Value& obj, const Value& name, const Value& v);
bool vm_private_define(Context& ctx, const Value& obj, const Value& name, const Value& v, int kind);
bool vm_copy_data_properties(Context& ctx, Object* target, const Value& src, const std::vector<PropKey>* excluded);
bool vm_set_function_name(Context& ctx, Object* fn, const Value& key, int kind);
bool vm_define_accessor_half(Context& ctx, Object* o, PropKey key, Object* fn, bool getter, bool enumerable);
bool vm_get_super(Context& ctx, Frame& f, const Value& this_v, PropKey key, Value* out);
bool vm_set_super(Context& ctx, Frame& f, const Value& this_v, PropKey key, const Value& v, bool strict);
Object* vm_template_object(Context& ctx, FunctionCode* fc, uint32_t idx);
// for-in
Object* vm_for_in_start(Context& ctx, const Value& v);
bool vm_for_in_next(Context& ctx, Object* state, Value* key, bool* done);
// global bindings
bool vm_get_global(Context& ctx, JsString* name, Value* out);
bool vm_set_global(Context& ctx, JsString* name, const Value& v, bool strict);
bool vm_declare_global_var(Context& ctx, JsString* name);
bool vm_declare_global_func(Context& ctx, JsString* name, const Value& fn);
bool vm_declare_global_lex(Context& ctx, JsString* name, bool is_const);
bool vm_has_global(Context& ctx, JsString* name, Value* value_if_present, bool* present);

// ---- vm_coro.cpp ----
// Start a generator, async function or async generator call whose arguments are already on the VM stack.
bool vm_start_coroutine(Context& ctx, Function* fn, const Value& this_v, int argc, Value* out);
// Run a coroutine frame until it yields, awaits, returns or throws.
Exit vm_resume(Context& ctx, Coroutine* co, int mode, const Value& value, Value* result);
// What the interpreter loop does when a coroutine frame suspends.
void vm_suspend(Context& ctx, Frame& f, Value* sp, uint32_t resume_pc, uint32_t instr_pc, bool awaiting, bool in_yield_star);
bool vm_await(Context& ctx, Coroutine* co, const Value& v);                      // subscribe the coroutine to `v`
bool vm_async_step(Context& ctx, Coroutine* co, int mode, const Value& value);   // continue after an await
bool vm_async_gen_yield(Context& ctx, Coroutine* co, const Value& v);

// ---- exposed to the builtins ----
bool generator_resume(Context& ctx, const Value& gen, int mode, const Value& arg, Value* out);
bool async_generator_enqueue(Context& ctx, const Value& gen, int mode, const Value& arg, Value* promise_out);

// Promise operations the VM needs (builtins_promise.cpp).
Object* promise_new(Context& ctx);
bool promise_resolve_with(Context& ctx, Object* promise, const Value& v);
void promise_reject_with(Context& ctx, Object* promise, const Value& reason);
Object* promise_resolve_value(Context& ctx, const Value& v);   // PromiseResolve(%Promise%, v); null with an exception pending
void promise_then_internal(Context& ctx, Object* promise, const Value& on_fulfilled, const Value& on_rejected);

// RegExp literals (builtins_regexp.cpp).
Object* regexp_create(Context& ctx, JsString* pattern, JsString* flags);

// Compile and run source text as a script in the global scope.
bool vm_eval_source(Context& ctx, const std::u16string& source, const std::string& filename, bool strict, Value* out);
// `new Function(params, body)` and friends: compile to a function object.
bool vm_function_from_source(Context& ctx, const std::u16string& params, const std::u16string& body, bool is_async, bool is_generator, Value* out);

}  // namespace aios::js
