// grain, stage four: the machine that runs the bytecode.
//
// A stack machine with call frames and real closures. Two things here are not
// the textbook shape and both are deliberate:
//
//   Upvalues hold a stack INDEX, not a pointer. The value stack is a vector
//   and a vector moves when it grows; a captured pointer into it is a
//   use-after-free that only shows up in the one program deep enough to
//   reallocate mid-call.
//
//   Every instruction that can fail returns a message and a line rather than
//   trapping. The REPL has to survive a bad expression and offer the next
//   prompt, and a dream that divides by zero must not take the compositor with
//   it.
#include <cstddef>
#include <atomic>
#include <deque>
#include "aios/userland/grain.hpp"
#include "grain_jit.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>

namespace aios::userland::grain {

// An open upvalue names a slot in the running value stack. Closing it copies
// the value out, so the closure keeps working after the frame is gone.
class Upvalue {
 public:
  size_t slot = 0;
  Value closed;
  bool is_closed = false;
};

// --- values ----------------------------------------------------------------

Value Value::string(std::string s) {
  return Value::object(Type::Str, std::make_shared<StrObj>(std::move(s)));
}

Value Value::list(std::vector<Value> v) {
  return Value::object(Type::List, std::make_shared<ListObj>(std::move(v)));
}

namespace {
const std::string kEmpty;
std::vector<Value> g_empty_items;
}  // namespace

const std::string& Value::str() const {
  if (type != Type::Str || !obj) return kEmpty;
  return static_cast<StrObj*>(obj.get())->text;
}

std::vector<Value>& Value::list_items() const {
  if (type != Type::List || !obj) return g_empty_items;
  return static_cast<ListObj*>(obj.get())->items;
}

Value* MapObj::find(const std::string& k) {
  if (index.empty()) {
    const size_t n = k.size();
    for (auto& e : entries)
      if (e.first.size() == n && std::memcmp(e.first.data(), k.data(), n) == 0) return &e.second;
    return nullptr;
  }
  auto it = index.find(k);
  return it == index.end() ? nullptr : &entries[it->second].second;
}

void MapObj::put(const std::string& k, Value v) {
  if (Value* have = find(k)) { *have = std::move(v); return; }
  entries.emplace_back(k, std::move(v));
  if (!index.empty()) index[k] = entries.size() - 1;
  else if (entries.size() > kIndexFrom) {
    index.reserve(entries.size() * 2);
    for (size_t i = 0; i < entries.size(); ++i) index[entries[i].first] = i;
  }
}

// Erasing shifts the entries after it, so the index (when there is one) has to
// be told. Maps are small and deletion is rare; keeping insertion order is
// worth more than an O(1) erase that leaves holes.
bool MapObj::erase(const std::string& k) {
  size_t at = entries.size();
  if (index.empty()) {
    for (size_t i = 0; i < entries.size(); ++i) if (entries[i].first == k) { at = i; break; }
  } else {
    auto it = index.find(k);
    if (it != index.end()) at = it->second;
  }
  if (at == entries.size()) return false;
  entries.erase(entries.begin() + (long)at);
  if (!index.empty()) {
    index.erase(k);
    for (auto& [key, pos] : index) if (pos > at) pos--;
  }
  return true;
}

namespace {

// Integers print without a decimal point; reals always print with one, so a
// program never has to wonder which kind it is holding.
std::string number_text(const Value& v) {
  char buf[40];
  if (v.type == Type::Int) { std::snprintf(buf, sizeof buf, "%lld", (long long)v.i); return buf; }
  if (v.d == (double)(int64_t)v.d && std::fabs(v.d) < 1e15) {
    std::snprintf(buf, sizeof buf, "%.1f", v.d);
  } else {
    std::snprintf(buf, sizeof buf, "%.12g", v.d);
  }
  return buf;
}

std::string quote(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      default: out.push_back(c);
    }
  }
  out.push_back('"');
  return out;
}

}  // namespace

std::string Value::to_text() const {
  switch (type) {
    case Type::Nil: return "nil";
    case Type::Bool: return b ? "true" : "false";
    case Type::Int: case Type::Real: return number_text(*this);
    case Type::Str: return str();
    case Type::List: {
      std::string out = "[";
      const auto& items = list_items();
      for (size_t i = 0; i < items.size(); ++i) { if (i) out += ", "; out += items[i].repr(); }
      return out + "]";
    }
    case Type::Map: {
      std::string out = "{";
      auto* m = static_cast<MapObj*>(obj.get());
      for (size_t i = 0; i < m->entries.size(); ++i) {
        if (i) out += ", ";
        out += m->entries[i].first + ": " + m->entries[i].second.repr();
      }
      return out + "}";
    }
    case Type::Fn: {
      if (auto* c = dynamic_cast<ClosureObj*>(obj.get())) return "<fn " + c->fn->name + ">";
      if (auto* f = dynamic_cast<FnObj*>(obj.get())) return "<fn " + f->name + ">";
      return "<fn>";
    }
    case Type::Native: return "<native " + static_cast<NativeObj*>(obj.get())->name + ">";
    case Type::Extern: {
      auto* e = static_cast<ExternObj*>(obj.get());
      // A closed handle still prints, and says so: a program that logs one
      // after closing it should not be told it is looking at a live socket.
      return "<" + e->kind_name + (e->ptr ? "" : " closed") + ">";
    }
  }
  return "?";
}

// Inside a container a string has to be quoted, or ["a, b"] and ["a", "b"]
// print the same and neither can be read back.
std::string Value::repr() const {
  return type == Type::Str ? quote(str()) : to_text();
}

bool Value::equals(const Value& o) const {
  // 2 and 2.0 are the same number written two ways. Everything else has to
  // match kind as well as content.
  if (is_number() && o.is_number()) {
    if (type == Type::Int && o.type == Type::Int) return i == o.i;
    return as_double() == o.as_double();
  }
  if (type != o.type) return false;
  switch (type) {
    case Type::Nil: return true;
    case Type::Bool: return b == o.b;
    case Type::Str: return str() == o.str();
    case Type::List: {
      const auto& x = list_items();
      const auto& y = o.list_items();
      if (x.size() != y.size()) return false;
      for (size_t k = 0; k < x.size(); ++k) if (!x[k].equals(y[k])) return false;
      return true;
    }
    case Type::Map: {
      auto* a = static_cast<MapObj*>(obj.get());
      auto* c = static_cast<MapObj*>(o.obj.get());
      if (!a || !c || a->entries.size() != c->entries.size()) return false;
      for (auto& [k, v] : a->entries) {
        Value* other = c->find(k);
        if (!other || !v.equals(*other)) return false;
      }
      return true;
    }
    default: return obj == o.obj;   // functions are the same if identical
  }
}

// --- disassembler ----------------------------------------------------------

namespace {

size_t print_one(std::ostringstream& out, const FnObj& fn, size_t at, int& last_line) {
  const Chunk& c = fn.chunk;
  char buf[32];
  std::snprintf(buf, sizeof buf, "%04zu ", at);
  out << buf;
  const int line = c.lines[at];
  if (line == last_line) out << "   | ";
  else { std::snprintf(buf, sizeof buf, "%4d ", line); out << buf; last_line = line; }

  const Op op = (Op)c.code[at];
  out << op_name(op);
  switch (op) {
    case Op::Constant: case Op::GetGlobal: case Op::SetGlobal: case Op::DefineGlobal:
    case Op::FieldGet: case Op::FieldSet: {
      const int k = (c.code[at + 1] << 8) | c.code[at + 2];
      out << " " << k << "  ; " << c.constants[k].repr() << "\n";
      return at + 3;
    }
    case Op::BuildList: case Op::BuildMap:
      out << " " << ((c.code[at + 1] << 8) | c.code[at + 2]) << "\n";
      return at + 3;
    case Op::GetLocal: case Op::SetLocal: case Op::GetUpvalue: case Op::SetUpvalue:
    case Op::Call: case Op::IterNext:
      out << " " << (int)c.code[at + 1] << "\n";
      return at + 2;
    case Op::Jump: case Op::JumpIfFalse: case Op::JumpIfTrue: {
      const int d = (c.code[at + 1] << 8) | c.code[at + 2];
      out << " -> " << (at + 3 + (size_t)d) << "\n";
      return at + 3;
    }
    case Op::Loop: {
      const int d = (c.code[at + 1] << 8) | c.code[at + 2];
      out << " -> " << (at + 3 - (size_t)d) << "\n";
      return at + 3;
    }
    case Op::Closure: {
      const int k = (c.code[at + 1] << 8) | c.code[at + 2];
      auto sub = std::static_pointer_cast<FnObj>(c.constants[k].obj);
      out << " " << k << "  ; fn " << (sub ? sub->name : "?") << "\n";
      size_t next = at + 3;
      const size_t n = sub ? sub->upvalue_index.size() : 0;
      for (size_t u = 0; u < n; ++u) {
        std::snprintf(buf, sizeof buf, "%04zu ", next);
        out << buf << "   |   capture " << (c.code[next] ? "local" : "upvalue")
            << " " << (int)c.code[next + 1] << "\n";
        next += 2;
      }
      return next;
    }
    default:
      out << "\n";
      return at + 1;
  }
}

void disassemble_into(std::ostringstream& out, const FnObj& fn) {
  out << "== fn " << fn.name << " (" << fn.arity << " param(s), "
      << fn.chunk.code.size() << " bytes, " << fn.chunk.constants.size() << " constant(s)) ==\n";
  int last_line = -1;
  for (size_t at = 0; at < fn.chunk.code.size();) at = print_one(out, fn, at, last_line);
  // Nested functions are constants of their parent, so printing them after it
  // gives the whole program in one listing rather than one per invocation.
  for (const Value& k : fn.chunk.constants) {
    if (k.type == Type::Fn && k.obj) {
      if (auto* sub = dynamic_cast<FnObj*>(k.obj.get())) { out << "\n"; disassemble_into(out, *sub); }
    }
  }
}

}  // namespace

std::string disassemble(const FnObj& fn) {
  std::ostringstream out;
  disassemble_into(out, fn);
  return out.str();
}

// --- the machine -----------------------------------------------------------

struct Vm::Frame {
  // Raw: the callee Value sits in stack slot `base` for the whole call (slot 0
  // is reserved for it), which keeps the closure alive without an atomic
  // refcount pair per call. `owned` is only set when the VM had to wrap a bare
  // FnObj in a fresh closure that nothing else holds.
  ClosureObj* closure = nullptr;
  size_t ip = 0;
  size_t base = 0;
  std::shared_ptr<ClosureObj> owned;
  const Chunk* chunk = nullptr;
  Frame(ClosureObj* c, size_t i, size_t b, std::shared_ptr<ClosureObj> o = nullptr)
      : closure(c), ip(i), base(b), owned(std::move(o)), chunk(&c->fn->chunk) {}
};

namespace {
// The closure a Fn value calls, without RTTI. A bare FnObj gets a new closure,
// returned through `owned` so the caller can keep it alive.
ClosureObj* closure_of(const Value& v, std::shared_ptr<ClosureObj>& owned) {
  if (v.type != Type::Fn || !v.obj) return nullptr;
  if (v.obj->is_closure) return static_cast<ClosureObj*>(v.obj.get());
  owned = std::make_shared<ClosureObj>();
  owned->fn = std::static_pointer_cast<FnObj>(v.obj);
  return owned.get();
}

// Where a global lives, through the chunk's inline cache.
Value* cached_global(const Chunk& chunk, uint16_t k, uint64_t owner,
                     std::unordered_map<std::string, Value>& globals) {
  if (chunk.global_cache_owner != owner) {
    chunk.global_cache.assign(chunk.constants.size(), nullptr);
    chunk.global_cache_owner = owner;
  } else if (chunk.global_cache.size() < chunk.constants.size()) {
    chunk.global_cache.resize(chunk.constants.size(), nullptr);
  }
  Value*& slot = chunk.global_cache[k];
  if (!slot) {
    auto it = globals.find(chunk.constants[k].str());
    if (it != globals.end()) slot = &it->second;
  }
  return slot;
}
}  // namespace

struct Vm::State {
  std::vector<Value> stack;
  std::vector<Frame> frames;
  std::vector<std::shared_ptr<Upvalue>> open;
  std::unordered_map<std::string, Value> globals;
  uint64_t serial = next_serial();
  jit::Ctx jit_ctx{};
  std::deque<std::vector<Value>> native_args;    // see call_value: one per native nesting depth (deque: growing it must not move the ones in use)
  size_t native_depth = 0;
  static uint64_t next_serial() { static std::atomic<uint64_t> n{0}; return ++n; }
  std::string error;
  int error_line = 0;
};

Vm::Vm() : st_(std::make_unique<State>()) { st_->stack.reserve(1024); st_->frames.reserve(256); }
Vm::~Vm() = default;

void Vm::define_global(const std::string& name, Value v) { st_->globals[name] = std::move(v); }
Value* Vm::find_global(const std::string& name) {
  auto it = st_->globals.find(name);
  return it == st_->globals.end() ? nullptr : &it->second;
}

void Vm::retain_globals(const std::vector<std::string>& keep) {
  for (auto it = st_->globals.begin(); it != st_->globals.end();) {
    if (std::find(keep.begin(), keep.end(), it->first) == keep.end()) it = st_->globals.erase(it);
    else ++it;
  }
}

void Vm::define_native(const std::string& name, int min_args, int max_args, NativeFn fn) {
  st_->globals[name] = Value::object(Type::Native,
      std::make_shared<NativeObj>(name, min_args, max_args, fn));
}

void Vm::emit(const std::string& line) {
  if (sink) { sink->push_back(line); return; }
  // Flushed per line. A server's log is read while the server is still
  // running, and a buffered stream redirected to a file shows nothing at all
  // until the program ends -- which for a server is never.
  std::cout << line << "\n" << std::flush;
}

namespace {

bool numeric_pair(const Value& a, const Value& b) { return a.is_number() && b.is_number(); }
bool both_int(const Value& a, const Value& b) { return a.type == Type::Int && b.type == Type::Int; }

// Push a copy of `src` without touching its shared_ptr when it has none -
// numbers, bools and nil are most of what a loop moves. The fields are read
// before the push because the push may reallocate the stack `src` lives in.
inline void push_copy(std::vector<Value>& stack, const Value& src) {
  if (src.obj) { stack.push_back(src); return; }
  const Type t = src.type; const int64_t bits = src.i;
  stack.emplace_back();
  Value& d = stack.back();
  d.type = t; d.i = bits;
}

}  // namespace

FnObj::~FnObj() { jit::release(jit_code); }

// Cycle accounting for the native call path (Pi only: the PMU cycle counter
// is readable at EL1). `grain prof` prints and resets it.
JitProfile g_jit_profile;
#if defined(AIOS_METAL) && defined(__aarch64__)
static inline uint64_t prof_now() { uint64_t v; asm volatile("mrs %0, pmccntr_el0" : "=r"(v)); return v; }
#define PROF_BEGIN() const uint64_t prof_t0 = prof_now()
#define PROF_END(field) (g_jit_profile.field += prof_now() - prof_t0, ++g_jit_profile.field##_n)
#else
#define PROF_BEGIN() do {} while (0)
#define PROF_END(field) do {} while (0)
#endif

bool Vm::step_one(size_t ip) {
  PROF_BEGIN();
  st_->frames.back().ip = ip;
  ++g_jit_profile.step_by_op[st_->frames.back().chunk->code[ip] & 63];
  const bool ok = execute_loop(0, true);
  PROF_END(step);
  return ok;
}

// The callee's frame is already pushed; run its native code to its Return.
bool Vm::run_native(void* code, size_t base) {
#if UINTPTR_MAX == UINT64_MAX  // the JITs are 64-bit; a 32-bit build (wasm) never runs native code
  static_assert(sizeof(Frame) == jit::kFrameSize, "the native call path writes Frames directly");
  static_assert(offsetof(Frame, closure) == jit::kFrameClosure && offsetof(Frame, ip) == jit::kFrameIp &&
                offsetof(Frame, base) == jit::kFrameBase && offsetof(Frame, owned) == jit::kFrameOwnedPtr &&
                offsetof(Frame, chunk) == jit::kFrameChunk, "Frame layout");
#endif
  State& S = *st_;
  S.jit_ctx.vm = this;
  S.jit_ctx.stack = &S.stack;
  S.jit_ctx.steps = &steps;
  S.jit_ctx.step_limit = &step_limit;
  S.jit_ctx.frames = &S.frames;
  S.jit_ctx.open = &S.open;
  S.jit_ctx.serial = S.serial;
  return reinterpret_cast<jit::Entry>(code)(&S.jit_ctx, base) != 0 && S.error.empty();
}

namespace {
void* native_for(FnObj& fn) {
  if (!fn.jit_tried) {
    fn.jit_tried = true;
    fn.jit_code = reinterpret_cast<void*>(jit::compile(fn));
  }
  return fn.jit_code;
}
}  // namespace

void Vm::close_upvalues_from(size_t from) {
  State& S = *st_;
  for (size_t i = S.open.size(); i-- > 0;) {
    if (S.open[i]->slot < from) continue;
    S.open[i]->closed = S.stack[S.open[i]->slot];
    S.open[i]->is_closed = true;
    S.open.erase(S.open.begin() + (long)i);
  }
}

// A call: `callee` and its argc arguments are the top of the stack. Natives
// run to completion here; a grain function gets its frame pushed and - when it
// has native code - runs to its Return before this returns.
bool Vm::invoke(const Value& callee, uint8_t argc, int line) {
  State& S = *st_;
  auto fail = [&](int l, const std::string& msg) { S.error = msg; S.error_line = l; };
  if (callee.type == Type::Native) {
    auto* nat = static_cast<NativeObj*>(callee.obj.get());
    if (argc < nat->min_args || (nat->max_args >= 0 && argc > nat->max_args)) {
      fail(line, nat->name + "() takes " + std::to_string(nat->min_args) +
           (nat->max_args == nat->min_args ? "" : "-" + (nat->max_args < 0 ? std::string("any")
                                                         : std::to_string(nat->max_args))) +
           " argument(s), got " + std::to_string((int)argc));
      return false;
    }
    // Argument vectors come from a per-depth pool instead of a fresh heap
    // allocation per native call; depth-indexed because a native (map,
    // filter, each...) can call back into grain, which calls natives.
    if (S.native_depth >= S.native_args.size()) S.native_args.emplace_back();
    std::vector<Value>& args = S.native_args[S.native_depth];
    args.assign(std::make_move_iterator(S.stack.end() - argc), std::make_move_iterator(S.stack.end()));
    std::string err;
    ++S.native_depth;
    Value out = nat->fn(*this, args, err);
    --S.native_depth;
    args.clear();
    S.stack.resize(S.stack.size() - argc - 1);
    if (!err.empty()) { fail(line, err); return false; }
    S.stack.push_back(std::move(out));
    return true;
  }
  if (callee.type == Type::Fn && callee.obj) {
    std::shared_ptr<ClosureObj> owned;
    ClosureObj* closure = closure_of(callee, owned);
    if (!closure) { fail(line, "that function was never closed over"); return false; }
    if (argc != closure->fn->arity) {
      fail(line, closure->fn->name + "() takes " + std::to_string(closure->fn->arity) +
           " argument(s), got " + std::to_string((int)argc));
      return false;
    }
    if (S.frames.size() >= jit::kMaxFrames) { fail(line, "call depth exceeded (runaway recursion?)"); return false; }
    const size_t base = S.stack.size() - argc - 1;
    S.frames.emplace_back(closure, 0, base, std::move(owned));
    if (jit_enabled) {
      if (void* code = native_for(*closure->fn)) return run_native(code, base);
    }
    return true;
  }
  fail(line, std::string("cannot call a ") + type_name(callee.type) +
       " -- only a function or a native is callable");
  return false;
}

// The native code's Call and Return (grain_jit_arm64.cpp): the interpreter's
// own semantics without an interpreter entry per call.
bool Vm::jit_call(size_t ip) {
  ++g_jit_profile.slow_calls;
  State& S = *st_;
  Frame& f = S.frames.back();
  f.ip = ip + 2;                                   // where the interpreter leaves it: traces name this call
  const Chunk& chunk = *f.chunk;
  const uint8_t argc = chunk.code[ip + 1];
  // By reference: invoke() reads the callee before anything can grow the
  // stack, and the slot keeps it alive until the frame is torn down. (A copy
  // was an atomic refcount increment and decrement per call.)
  return invoke(S.stack[S.stack.size() - argc - 1], argc, chunk.lines[ip]) && S.error.empty();
}

bool Vm::jit_get_global(size_t ip) {
  PROF_BEGIN();
  State& S = *st_;
  const Chunk& chunk = *S.frames.back().chunk;
  const uint16_t k = (uint16_t)((chunk.code[ip + 1] << 8) | chunk.code[ip + 2]);
  Value* g = cached_global(chunk, k, S.serial, S.globals);
  if (!g) {
    S.frames.back().ip = ip + 3;
    S.error = "'" + chunk.constants[k].str() + "' is not defined";
    S.error_line = chunk.lines[ip];
    return false;
  }
  push_copy(S.stack, *g);
  PROF_END(get_global);
  return true;
}

void Vm::jit_return() {
  PROF_BEGIN();
  State& S = *st_;
  const size_t base = S.frames.back().base;
  Value out = std::move(S.stack.back());
  S.stack.pop_back();
  close_upvalues_from(base);
  S.stack.resize(base);
  S.frames.pop_back();
  S.stack.push_back(std::move(out));
  PROF_END(ret);
}

bool Vm::execute_loop(size_t target_frame_depth, bool single_step) {
  State& S = *st_;

  auto fail = [&](int line, const std::string& msg) {
    S.error = msg;
    S.error_line = line;
  };

  auto frame_line = [&](const Frame& f) {
    const Chunk& c = f.closure->fn->chunk;
    const size_t at = f.ip > 0 ? f.ip - 1 : 0;
    return at < c.lines.size() ? c.lines[at] : 0;
  };



  if (S.frames.size() <= target_frame_depth) return S.error.empty();
  // The current frame and its chunk change only on Call and Return, so they
  // are re-read there instead of on every instruction. (Call includes natives,
  // which can re-enter the VM and reallocate `frames` - hence the reload after
  // any Call, not just grain-to-grain ones.)
  Frame* frame = &S.frames.back();
  const Chunk* cur = frame->chunk;
  for (;;) {
    if (step_limit && ++steps > step_limit) {
      fail(frame_line(S.frames.back()),
           "step limit of " + std::to_string(step_limit) + " reached");
      return false;
    }

    const Chunk& chunk = *cur;
    if (frame->ip >= chunk.code.size()) { fail(0, "ran off the end of a function"); break; }

    // Only a fault reads the line; looking it up per instruction was a load
    // on every step for nothing.
    struct LazyLine {
      const Chunk& c; size_t at;
      operator int() const { return c.lines[at]; }
    } line{chunk, frame->ip};
    const Op op = (Op)chunk.code[frame->ip++];

    auto read_byte = [&]() -> uint8_t { return chunk.code[frame->ip++]; };
    auto read_short = [&]() -> uint16_t {
      const uint16_t v = (uint16_t)((chunk.code[frame->ip] << 8) | chunk.code[frame->ip + 1]);
      frame->ip += 2;
      return v;
    };
    auto pop = [&]() { Value v = std::move(S.stack.back()); S.stack.pop_back(); return v; };

    switch (op) {
      case Op::Constant: push_copy(S.stack, chunk.constants[read_short()]); break;
      case Op::Nil: S.stack.push_back(Value::nil()); break;
      case Op::True: S.stack.push_back(Value::boolean(true)); break;
      case Op::False: S.stack.push_back(Value::boolean(false)); break;
      case Op::Pop: S.stack.pop_back(); break;

      case Op::GetLocal: push_copy(S.stack, S.stack[frame->base + read_byte()]); break;
      case Op::SetLocal: {
        Value& dst = S.stack[frame->base + read_byte()];
        const Value& top = S.stack.back();
        if (!dst.obj && !top.obj) { dst.type = top.type; dst.i = top.i; } else dst = top;
        break;
      }

      case Op::GetGlobal: {
        const uint16_t k = read_short();
        Value* g = cached_global(chunk, k, S.serial, S.globals);
        if (!g) { fail(line, "'" + chunk.constants[k].str() + "' is not defined"); break; }
        S.stack.push_back(*g);
        break;
      }
      case Op::SetGlobal: {
        const uint16_t k = read_short();
        Value* g = cached_global(chunk, k, S.serial, S.globals);
        // Assigning to a name that was never declared is a typo far more often
        // than an intent, and silently creating it is how one survives to
        // production.
        if (!g) { fail(line, "'" + chunk.constants[k].str() + "' is not defined"); break; }
        *g = S.stack.back();
        break;
      }
      case Op::DefineGlobal: {
        const std::string& name = chunk.constants[read_short()].str();
        S.globals[name] = pop();
        break;
      }

      case Op::GetUpvalue: {
        const auto& uv = frame->closure->upvalues[read_byte()];
        S.stack.push_back(uv->is_closed ? uv->closed : S.stack[uv->slot]);
        break;
      }
      case Op::SetUpvalue: {
        const auto& uv = frame->closure->upvalues[read_byte()];
        if (uv->is_closed) uv->closed = S.stack.back();
        else S.stack[uv->slot] = S.stack.back();
        break;
      }

      case Op::Equal: case Op::NotEqual: {
        Value& a = S.stack[S.stack.size() - 2];
        const Value& b = S.stack.back();
        if (both_int(a, b)) {           // in place: no Value built or destroyed
          const bool eq = a.i == b.i;
          a.type = Type::Bool; a.i = ((op == Op::Equal) ? eq : !eq) ? 1 : 0;   // whole word, as Value::boolean leaves it
          S.stack.pop_back();
          break;
        }
        Value bv = pop(), av = pop();
        S.stack.push_back(Value::boolean((op == Op::Equal) == av.equals(bv)));
        break;
      }

      case Op::Less: case Op::LessEqual: case Op::Greater: case Op::GreaterEqual: {
        {
          Value& a = S.stack[S.stack.size() - 2];
          const Value& b = S.stack.back();
          if (both_int(a, b)) {
            const bool r = op == Op::Less ? a.i < b.i : op == Op::LessEqual ? a.i <= b.i
                         : op == Op::Greater ? a.i > b.i : a.i >= b.i;
            a.type = Type::Bool; a.i = r ? 1 : 0;
            S.stack.pop_back();
            break;
          }
        }
        Value b = pop(), a = pop();
        int cmp = 0;
        if (numeric_pair(a, b)) {
          if (both_int(a, b)) cmp = a.i < b.i ? -1 : (a.i > b.i ? 1 : 0);
          else { const double x = a.as_double(), y = b.as_double(); cmp = x < y ? -1 : (x > y ? 1 : 0); }
        } else if (a.type == Type::Str && b.type == Type::Str) {
          const int r = a.str().compare(b.str());
          cmp = r < 0 ? -1 : (r > 0 ? 1 : 0);
        } else {
          fail(line, std::string("cannot compare ") + type_name(a.type) + " with " + type_name(b.type));
          break;
        }
        bool r = false;
        if (op == Op::Less) r = cmp < 0;
        else if (op == Op::LessEqual) r = cmp <= 0;
        else if (op == Op::Greater) r = cmp > 0;
        else r = cmp >= 0;
        S.stack.push_back(Value::boolean(r));
        break;
      }

      case Op::Add: {
        {
          Value& x = S.stack[S.stack.size() - 2];
          const Value& y = S.stack.back();
          if (both_int(x, y)) { x.i += y.i; S.stack.pop_back(); break; }
          if (x.type == Type::Real && y.type == Type::Real) { x.d += y.d; S.stack.pop_back(); break; }
        }
        Value b = pop(), a = pop();
        if (both_int(a, b)) { S.stack.push_back(Value::integer(a.i + b.i)); break; }
        if (numeric_pair(a, b)) { S.stack.push_back(Value::real(a.as_double() + b.as_double())); break; }
        if (a.type == Type::Str && b.type == Type::Str) { S.stack.push_back(Value::string(a.str() + b.str())); break; }
        if (a.type == Type::List && b.type == Type::List) {
          std::vector<Value> out = a.list_items();
          const auto& more = b.list_items();
          out.insert(out.end(), more.begin(), more.end());
          S.stack.push_back(Value::list(std::move(out)));
          break;
        }
        // No implicit "a" + 1. The one time it is what you meant it saves four
        // characters; every other time it hides that a number arrived where a
        // string was expected.
        fail(line, std::string("cannot add ") + type_name(a.type) + " and " + type_name(b.type));
        break;
      }
      case Op::Subtract: case Op::Multiply: {
        {
          Value& x = S.stack[S.stack.size() - 2];
          const Value& y = S.stack.back();
          if (both_int(x, y)) { x.i = op == Op::Subtract ? x.i - y.i : x.i * y.i; S.stack.pop_back(); break; }
          if (x.type == Type::Real && y.type == Type::Real) { x.d = op == Op::Subtract ? x.d - y.d : x.d * y.d; S.stack.pop_back(); break; }
        }
        Value b = pop(), a = pop();
        if (!numeric_pair(a, b)) {
          fail(line, std::string("cannot ") + (op == Op::Subtract ? "subtract " : "multiply ") +
               type_name(a.type) + " and " + type_name(b.type));
          break;
        }
        if (both_int(a, b)) {
          S.stack.push_back(Value::integer(op == Op::Subtract ? a.i - b.i : a.i * b.i));
        } else {
          const double x = a.as_double(), y = b.as_double();
          S.stack.push_back(Value::real(op == Op::Subtract ? x - y : x * y));
        }
        break;
      }
      // Division always produces a real. Integer division that silently
      // truncates is the single most reliable source of an off-by-one in any
      // language that has it; `int(a / b)` says what it means.
      case Op::Divide: {
        Value b = pop(), a = pop();
        if (!numeric_pair(a, b)) {
          fail(line, std::string("cannot divide ") + type_name(a.type) + " by " + type_name(b.type));
          break;
        }
        if (b.as_double() == 0.0) { fail(line, "division by zero"); break; }
        S.stack.push_back(Value::real(a.as_double() / b.as_double()));
        break;
      }
      case Op::Modulo: {
        Value b = pop(), a = pop();
        if (!numeric_pair(a, b)) {
          fail(line, std::string("cannot take ") + type_name(a.type) + " modulo " + type_name(b.type));
          break;
        }
        if (both_int(a, b)) {
          if (b.i == 0) { fail(line, "modulo by zero"); break; }
          S.stack.push_back(Value::integer(a.i % b.i));
        } else {
          if (b.as_double() == 0.0) { fail(line, "modulo by zero"); break; }
          S.stack.push_back(Value::real(std::fmod(a.as_double(), b.as_double())));
        }
        break;
      }
      // Bits are an integer's: 1.5 | 0 is an error, not a silent truncation
      // (the same rule that keeps "a" + 1 an error). Shifts: a negative count
      // is an error; 64 or more shifts everything out - 0 to the left, the
      // sign to the right (-8 >> 99 is -1, as -8 >> 63 is).
      case Op::BitAnd: case Op::BitOr: case Op::BitXor: case Op::Shl: case Op::Shr: {
        Value& x = S.stack[S.stack.size() - 2];
        const Value& y = S.stack.back();
        if (!both_int(x, y)) {
          fail(line, std::string("bitwise operations take integers, got ") + type_name(x.type) + " and " + type_name(y.type));
          break;
        }
        if ((op == Op::Shl || op == Op::Shr) && y.i < 0) { fail(line, "negative shift count"); break; }
        if (op == Op::BitAnd) x.i = x.i & y.i;
        else if (op == Op::BitOr) x.i = x.i | y.i;
        else if (op == Op::BitXor) x.i = x.i ^ y.i;
        else if (op == Op::Shl) x.i = y.i < 64 ? (int64_t)((uint64_t)x.i << y.i) : 0;
        else x.i = y.i < 64 ? (x.i >> y.i) : (x.i < 0 ? -1 : 0);
        S.stack.pop_back();
        break;
      }
      case Op::BitNot: {
        Value& a = S.stack.back();
        if (a.type != Type::Int) { fail(line, std::string("cannot bitwise invert a ") + type_name(a.type)); break; }
        a.i = ~a.i;
        break;
      }
      case Op::Dup: push_copy(S.stack, S.stack.back()); break;
      case Op::Dup2: {
        push_copy(S.stack, S.stack[S.stack.size() - 2]);
        push_copy(S.stack, S.stack[S.stack.size() - 2]);
        break;
      }
      case Op::Negate: {
        Value a = pop();
        if (a.type == Type::Int) S.stack.push_back(Value::integer(-a.i));
        else if (a.type == Type::Real) S.stack.push_back(Value::real(-a.d));
        else { fail(line, std::string("cannot negate a ") + type_name(a.type)); }
        break;
      }
      case Op::Not: { Value a = pop(); S.stack.push_back(Value::boolean(!a.truthy())); break; }

      case Op::Jump: frame->ip += read_short(); break;
      case Op::JumpIfFalse: { const uint16_t d = read_short(); if (!S.stack.back().truthy()) frame->ip += d; break; }
      case Op::JumpIfTrue: { const uint16_t d = read_short(); if (S.stack.back().truthy()) frame->ip += d; break; }
      case Op::Loop: { const uint16_t d = read_short(); frame->ip -= d; break; }

      case Op::Call: {
        const uint8_t argc = read_byte();
        const bool ok = invoke(S.stack[S.stack.size() - argc - 1], argc, line);
        frame = &S.frames.back(); cur = frame->chunk;
        if (!ok) break;
        break;
      }

      case Op::Closure: {
        auto fn = std::static_pointer_cast<FnObj>(chunk.constants[read_short()].obj);
        auto closure = std::make_shared<ClosureObj>();
        closure->fn = fn;
        for (size_t u = 0; u < fn->upvalue_index.size(); ++u) {
          const bool from_local = chunk.code[frame->ip++] != 0;
          const uint8_t index = chunk.code[frame->ip++];
          if (from_local) {
            const size_t slot = frame->base + index;
            // One Upvalue per slot, shared. Two closures capturing the same
            // variable have to SEE each other's writes -- a counter handed to
            // an incrementer and a reader is the whole point.
            std::shared_ptr<Upvalue> found;
            for (auto& o : S.open) if (!o->is_closed && o->slot == slot) { found = o; break; }
            if (!found) {
              found = std::make_shared<Upvalue>();
              found->slot = slot;
              S.open.push_back(found);
            }
            closure->upvalues.push_back(found);
          } else {
            closure->upvalues.push_back(frame->closure->upvalues[index]);
          }
        }
        S.stack.push_back(Value::object(Type::Fn, closure));
        break;
      }

      case Op::CloseUpvalue:
        close_upvalues_from(S.stack.size() - 1);
        S.stack.pop_back();
        break;

      case Op::Return: {
        Value out = pop();
        close_upvalues_from(frame->base);
        S.stack.resize(frame->base);
        S.frames.pop_back();
        S.stack.push_back(std::move(out));
        if (S.frames.size() <= target_frame_depth) {
          return true;
        }
        frame = &S.frames.back(); cur = frame->chunk;
        break;
      }

      case Op::BuildList: {
        const uint16_t n = read_short();
        std::vector<Value> items(S.stack.end() - n, S.stack.end());
        S.stack.resize(S.stack.size() - n);
        S.stack.push_back(Value::list(std::move(items)));
        break;
      }
      case Op::BuildMap: {
        const uint16_t n = read_short();
        auto m = std::make_shared<MapObj>();
        m->entries.reserve(n);
        for (size_t i = 0; i < n; ++i) {
          const Value& k = S.stack[S.stack.size() - 2 * (n - i)];
          const Value& v = S.stack[S.stack.size() - 2 * (n - i) + 1];
          m->put(k.str(), v);
        }
        S.stack.resize(S.stack.size() - 2 * (size_t)n);
        S.stack.push_back(Value::object(Type::Map, m));
        break;
      }

      case Op::IndexGet: {
        Value key = pop(), target = pop();
        if (target.type == Type::List) {
          if (!key.is_number()) { fail(line, "a list index must be a number"); break; }
          auto& items = target.list_items();
          int64_t at = key.as_int();
          if (at < 0) at += (int64_t)items.size();   // a[-1] is the last element
          if (at < 0 || at >= (int64_t)items.size()) {
            fail(line, "index " + std::to_string(key.as_int()) + " is outside a list of " +
                 std::to_string(items.size()));
            break;
          }
          S.stack.push_back(items[(size_t)at]);
          break;
        }
        if (target.type == Type::Map) {
          if (key.type != Type::Str) { fail(line, "a map key must be a string"); break; }
          Value* v = static_cast<MapObj*>(target.obj.get())->find(key.str());
          S.stack.push_back(v ? *v : Value::nil());
          break;
        }
        if (target.type == Type::Str) {
          if (!key.is_number()) { fail(line, "a string index must be a number"); break; }
          const std::string& s = target.str();
          int64_t at = key.as_int();
          if (at < 0) at += (int64_t)s.size();
          if (at < 0 || at >= (int64_t)s.size()) { fail(line, "string index out of range"); break; }
          S.stack.push_back(Value::string(std::string(1, s[(size_t)at])));
          break;
        }
        fail(line, std::string("cannot index a ") + type_name(target.type));
        break;
      }

      case Op::IndexSet: {
        Value v = pop(), key = pop(), target = pop();
        if (target.type == Type::List) {
          if (!key.is_number()) { fail(line, "a list index must be a number"); break; }
          auto& items = target.list_items();
          int64_t at = key.as_int();
          if (at < 0) at += (int64_t)items.size();
          if (at < 0 || at >= (int64_t)items.size()) { fail(line, "list index out of range"); break; }
          items[(size_t)at] = v;
        } else if (target.type == Type::Map) {
          if (key.type != Type::Str) { fail(line, "a map key must be a string"); break; }
          static_cast<MapObj*>(target.obj.get())->put(key.str(), v);
        } else {
          fail(line, std::string("cannot assign into a ") + type_name(target.type));
          break;
        }
        S.stack.push_back(v);
        break;
      }

      case Op::FieldGet: {
        const std::string& name = chunk.constants[read_short()].str();
        Value target = pop();
        if (target.type != Type::Map) {
          fail(line, std::string("a ") + type_name(target.type) + " has no field '" + name + "'");
          break;
        }
        Value* v = static_cast<MapObj*>(target.obj.get())->find(name);
        S.stack.push_back(v ? *v : Value::nil());
        break;
      }
      case Op::FieldSet: {
        const std::string& name = chunk.constants[read_short()].str();
        Value v = pop(), target = pop();
        if (target.type != Type::Map) { fail(line, "only a map has fields to set"); break; }
        static_cast<MapObj*>(target.obj.get())->put(name, v);
        S.stack.push_back(v);
        break;
      }

      // A for loop walks a list. Anything else that can be walked is turned
      // into one here, once, so the loop instruction itself stays trivial.
      case Op::IterPrep: {
        Value seq = pop();
        if (seq.type == Type::List) { S.stack.push_back(seq); break; }
        if (seq.type == Type::Map) {
          std::vector<Value> keys;
          for (auto& [k, v] : static_cast<MapObj*>(seq.obj.get())->entries) keys.push_back(Value::string(k));
          S.stack.push_back(Value::list(std::move(keys)));
          break;
        }
        if (seq.type == Type::Str) {
          std::vector<Value> chars;
          for (char ch : seq.str()) chars.push_back(Value::string(std::string(1, ch)));
          S.stack.push_back(Value::list(std::move(chars)));
          break;
        }
        fail(line, std::string("cannot walk a ") + type_name(seq.type));
        break;
      }

      case Op::IterNext: {
        const uint8_t seq_slot = read_byte();
        const Value& seq = S.stack[frame->base + seq_slot];
        Value& cursor = S.stack[frame->base + seq_slot + 1];
        const auto& items = seq.list_items();
        if ((size_t)cursor.i >= items.size()) { S.stack.push_back(Value::boolean(false)); break; }
        S.stack.push_back(items[(size_t)cursor.i]);
        cursor.i++;
        S.stack.push_back(Value::boolean(true));
        break;
      }

      case Op::Halt:
        return true;
    }

    if (!S.error.empty()) {
      return false;
    }
    if (single_step) return true;
  }

  return S.error.empty();
}

Vm::RunResult Vm::run(const std::shared_ptr<FnObj>& script) {
  RunResult result;
  if (!script) { result.error = "nothing to run"; return result; }

  State& S = *st_;
  S.stack.clear();
  S.frames.clear();
  S.open.clear();
  S.error.clear();
  steps = 0;

  auto top_closure = std::make_shared<ClosureObj>();
  top_closure->fn = script;
  S.stack.push_back(Value::object(Type::Fn, top_closure));
  S.frames.emplace_back(top_closure.get(), 0, 0);

  auto frame_line = [&](const Frame& f) {
    const Chunk& c = f.closure->fn->chunk;
    const size_t at = f.ip > 0 ? f.ip - 1 : 0;
    return at < c.lines.size() ? c.lines[at] : 0;
  };

  auto capture_trace = [&]() {
    const size_t n = S.frames.size();
    const size_t head = 6;
    for (size_t i = n; i-- > 0;) {
      const size_t shown = n - 1 - i;
      if (n > head + 2 && shown == head) {
        result.trace.push_back("... " + std::to_string(n - head - 1) + " more frame(s)");
        i = 1;   // the next iteration lands on the outermost frame
        continue;
      }
      const Frame& f = S.frames[i];
      result.trace.push_back(f.closure->fn->name + " (line " + std::to_string(frame_line(f)) + ")");
    }
  };

  bool ok;
  void* code = jit_enabled ? native_for(*script) : nullptr;
  if (code) ok = run_native(code, 0);
  else ok = execute_loop(0);
  if (ok && S.error.empty()) {
    result.ok = true;
    if (!S.stack.empty()) {
      result.value = S.stack.back();
    }
    return result;
  }

  capture_trace();
  result.error = S.error.empty() ? "stopped" : S.error;
  result.line = S.error_line;
  return result;
}

Value Vm::call(const Value& callee, const std::vector<Value>& args, std::string& error) {
  State& S = *st_;
  if (callee.type == Type::Native) {
    auto* nat = static_cast<NativeObj*>(callee.obj.get());
    if ((int)args.size() < nat->min_args || (nat->max_args >= 0 && (int)args.size() > nat->max_args)) {
      error = nat->name + "() takes " + std::to_string(nat->min_args) +
              (nat->max_args == nat->min_args ? "" : "-" + (nat->max_args < 0 ? std::string("any")
                                                           : std::to_string(nat->max_args))) +
              " argument(s), got " + std::to_string(args.size());
      return Value::nil();
    }
    std::vector<Value> arg_copy = args;
    return nat->fn(*this, arg_copy, error);
  }

  std::shared_ptr<ClosureObj> owned;
  ClosureObj* closure = closure_of(callee, owned);
  if (!closure) {
    error = std::string("cannot call a ") + type_name(callee.type) +
            " -- only a function or a native is callable";
    return Value::nil();
  }

  if ((int)args.size() != closure->fn->arity) {
    error = closure->fn->name + "() takes " + std::to_string(closure->fn->arity) +
            " argument(s), got " + std::to_string(args.size());
    return Value::nil();
  }

  if (S.frames.size() >= jit::kMaxFrames) {
    error = "call depth exceeded (runaway recursion?)";
    return Value::nil();
  }

  size_t target_depth = S.frames.size();
  size_t base = S.stack.size();
  S.stack.push_back(callee);
  for (const auto& a : args) S.stack.push_back(a);
  S.frames.emplace_back(closure, 0, base, std::move(owned));

  std::string prev_error = S.error;
  int prev_error_line = S.error_line;
  S.error.clear();
  S.error_line = 0;

  bool ok;
  void* code = jit_enabled ? native_for(*closure->fn) : nullptr;
  if (code) ok = run_native(code, base);
  else ok = execute_loop(target_depth);
  if (!ok || !S.error.empty()) {
    error = S.error;
    while (S.frames.size() > target_depth) S.frames.pop_back();
    S.stack.resize(base);
    S.error = prev_error;
    S.error_line = prev_error_line;
    return Value::nil();
  }

  Value ret = Value::nil();
  if (!S.stack.empty()) {
    ret = S.stack.back();
    S.stack.pop_back();
  }
  S.error = prev_error;
  S.error_line = prev_error_line;
  return ret;
}

}  // namespace aios::userland::grain
