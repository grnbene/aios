// grain -> AArch64 machine code.
//
// A template compiler: each bytecode instruction becomes a short run of
// native code working on the VM's own value stack, so native and interpreted
// frames interleave freely (a native function can call an interpreted one and
// back), errors and stack traces come out the same, and a native call is just
// a frame push. What gets compiled inline is what loops are made of - locals,
// constants, integer arithmetic and comparisons, branches, loop back-edges.
// Everything else (strings, lists, maps, globals, natives, calls, returns,
// closures, any type the fast path does not expect) hands exactly that one
// instruction to the interpreter via Vm::step_one(), so the two can only
// agree: there is one implementation of the language's semantics.
//
// Registers inside generated code:
//   x19  Ctx*                x22  stack top (one past the last Value)
//   x20  this frame's slot 0 x23  stack capacity end
//   x21  frame base index    x24  &std::vector<Value> (begin/finish/cap)
// x22 lives in a register between helper calls and is written back to the
// vector's finish pointer before each one; x20/x22/x23 are reloaded after,
// because the helper may have grown (reallocated) the stack.
//
// On the Pi the heap is mapped executable (MAP_NORMAL|MAP_EXEC) and the code
// is made visible to instruction fetch with the architectural clean/invalidate
// sequence (__builtin___clear_cache). On an arm64 Linux host (how this is
// developed under qemu-aarch64) code pages come from mmap. macOS on Apple
// Silicon refuses writable+executable pages unless they are mapped MAP_JIT,
// and then a thread sees them either writable or executable, never both:
// pthread_jit_write_protect_np(0) opens them for writing on this thread only,
// (1) makes them executable again, and sys_icache_invalidate publishes the
// code. A hardened-runtime signature needs com.apple.security.cs.allow-jit.
#include "grain_jit.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(__aarch64__) && !defined(AIOS_METAL) && !defined(_WIN32)
#include <sys/mman.h>
#endif
#if defined(__aarch64__) && defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#endif

namespace aios::userland::grain::jit {

// Windows on Arm (clang-cl defines __aarch64__ too) interprets until the JIT has
// a VirtualAlloc/FlushInstructionCache allocator of its own.
#if defined(__aarch64__) && !defined(_WIN32)

namespace {

// ---- layout the generated code depends on --------------------------------
static_assert(sizeof(Value) == 32, "Value is 32 bytes: type @0, payload @8, shared_ptr @16/@24");
static_assert(offsetof(Value, type) == 0, "Value::type at 0");
static_assert(offsetof(Value, i) == 8, "Value payload at 8");
static_assert(offsetof(Value, obj) == 16, "Value::obj at 16");
static_assert(offsetof(Ctx, stack) == 8 && offsetof(Ctx, steps) == 16 && offsetof(Ctx, step_limit) == 24 &&
              offsetof(Ctx, frames) == 32 && offsetof(Ctx, open) == 40 && offsetof(Ctx, serial) == 48, "Ctx layout");
constexpr int kTypeOff = 0, kPayOff = 8, kCtrlOff = 24;   // ctrl block pointer: non-null = refcounted

// std::vector<Value> as {begin, finish, end_of_storage} - libstdc++'s layout,
// checked at run time before the first compile rather than assumed.
bool vector_layout_ok() {
  // A function-local static: worker VMs compile on several threads at once, and the first of them settles it.
  static const bool verdict = [] {
    std::vector<Value> v;
    v.reserve(8);
    v.resize(3);
    auto** raw = reinterpret_cast<Value**>(&v);
    return sizeof(v) == 24 && raw[0] == v.data() && raw[1] == v.data() + 3 && raw[2] == v.data() + v.capacity();
  }();
  return verdict;
}

// Where a ListObj keeps its items vector (the class has a vtable, so this is
// measured, not offsetof).
size_t list_items_offset() {
  static const size_t off = [] {
    ListObj probe({});
    return static_cast<size_t>(reinterpret_cast<const char*>(&probe.items) - reinterpret_cast<const char*>(&probe));
  }();
  return off;
}

// Where the native call path finds things inside objects that have vtables
// (so offsetof is not an option): measured on real instances, once.
struct Layout {
  uint32_t obj_is_closure;   // Obj::is_closure (byte)
  uint32_t closure_fn;       // ClosureObj::fn (shared_ptr: element pointer first)
  uint32_t fn_jit;           // FnObj::jit_code
  uint32_t fn_arity;         // FnObj::arity (int)
  uint32_t fn_chunk;         // FnObj::chunk
  uint32_t sp_use_count;     // shared_ptr control block: use count
  bool sp_wide;              // the count is a long (libc++), not an int (libstdc++)
  uint32_t sp_last;          // the count's stored value when one owner is left: 1, or 0 where it holds owners - 1 (libc++)
  bool ok;
};
template <class T, class M> uint32_t member_off(const T& o, const M& m) {
  return static_cast<uint32_t>(reinterpret_cast<const char*>(&m) - reinterpret_cast<const char*>(&o));
}
const Layout& layout() {
  static const Layout L = [] {
    Layout l{};
    auto fn = std::make_shared<FnObj>();
    ClosureObj c;
    c.fn = fn;
    l.obj_is_closure = member_off(static_cast<const Obj&>(c), c.is_closure);
    l.closure_fn = member_off(c, c.fn);
    l.fn_jit = member_off(*fn, fn->jit_code);
    l.fn_arity = member_off(*fn, fn->arity);
    l.fn_chunk = member_off(*fn, fn->chunk);
    // shared_ptr = {element*, control block*}; the control block's use count
    // follows its vtable pointer: an int counting owners (libstdc++), or a
    // long counting owners - 1 (libc++). Checked, not assumed: two owners
    // here, then three, then two again.
    auto** raw = reinterpret_cast<void**>(&c.fn);
    const char* ctrl = static_cast<const char*>(raw[1]);
    l.sp_use_count = 8;
    auto count = [&] {
      return l.sp_wide ? static_cast<long long>(*reinterpret_cast<const long*>(ctrl + 8))
                       : static_cast<long long>(*reinterpret_cast<const int*>(ctrl + 8));
    };
    l.sp_wide = sizeof(long) == 8 && *reinterpret_cast<const long*>(ctrl + 8) == 1;
    l.sp_last = l.sp_wide ? 0 : 1;
    const long long before = count();
    bool counts = false;
    {
      std::shared_ptr<FnObj> extra = c.fn;
      const long long during = count();
      counts = before == static_cast<long long>(l.sp_last) + 1 && during == before + 1;
    }
    counts = counts && count() == before;
    l.ok = raw[0] == fn.get() && counts && sizeof(std::shared_ptr<FnObj>) == 16 &&
           l.fn_jit % 8 == 0 && l.closure_fn % 8 == 0 && l.fn_arity % 4 == 0;
    return l;
  }();
  return L;
}

// ---- helper the generated code calls ---------------------------------------
extern "C" int grain_jit_step(Ctx* c, uint32_t ip) { return c->vm->step_one(ip) ? 1 : 0; }
extern "C" int grain_jit_call(Ctx* c, uint32_t ip) { return c->vm->jit_call(ip) ? 1 : 0; }
extern "C" int grain_jit_return(Ctx* c, uint32_t) { c->vm->jit_return(); return 1; }
extern "C" int grain_jit_get_global(Ctx* c, uint32_t ip) { return c->vm->jit_get_global(ip) ? 1 : 0; }

// ---- encoder ----------------------------------------------------------------
enum Reg : uint32_t { X0 = 0, X1, X2, X9 = 9, X10, X11, X12, X13, X14, X15, X16, X17,
                      X19 = 19, X20, X21, X22, X23, X24, X29 = 29, X30, XZR = 31, SP = 31 };
enum Cond : uint32_t { EQ = 0, NE = 1, HS = 2, LO = 3, HI = 8, LS = 9, GE = 10, LT = 11, GT = 12, LE = 13 };

struct Asm {
  std::vector<uint32_t> buf;
  size_t here() const { return buf.size(); }
  void w(uint32_t v) { buf.push_back(v); }

  void movz(Reg d, uint32_t imm16, unsigned hw) { w(0xD2800000u | (hw << 21) | ((imm16 & 0xFFFF) << 5) | d); }
  void movk(Reg d, uint32_t imm16, unsigned hw) { w(0xF2800000u | (hw << 21) | ((imm16 & 0xFFFF) << 5) | d); }
  void mov_imm(Reg d, uint64_t v) {
    movz(d, static_cast<uint32_t>(v & 0xFFFF), 0);
    for (unsigned hw = 1; hw < 4; ++hw) {
      const uint32_t part = static_cast<uint32_t>((v >> (16 * hw)) & 0xFFFF);
      if (part) movk(d, part, hw);
    }
  }
  void mov(Reg d, Reg m) { w(0xAA0003E0u | (m << 16) | d); }
  void ldr(Reg t, Reg n, uint32_t off) { w(0xF9400000u | ((off / 8) << 10) | (n << 5) | t); }       // off % 8 == 0, < 32768
  void str(Reg t, Reg n, uint32_t off) { w(0xF9000000u | ((off / 8) << 10) | (n << 5) | t); }
  void ldrb(Reg t, Reg n, uint32_t off) { w(0x39400000u | (off << 10) | (n << 5) | t); }             // off < 4096
  void ldur(Reg t, Reg n, int off) { w(0xF8400000u | ((static_cast<uint32_t>(off) & 0x1FF) << 12) | (n << 5) | t); }
  void stur(Reg t, Reg n, int off) { w(0xF8000000u | ((static_cast<uint32_t>(off) & 0x1FF) << 12) | (n << 5) | t); }
  void ldurb(Reg t, Reg n, int off) { w(0x38400000u | ((static_cast<uint32_t>(off) & 0x1FF) << 12) | (n << 5) | t); }
  void sturb(Reg t, Reg n, int off) { w(0x38000000u | ((static_cast<uint32_t>(off) & 0x1FF) << 12) | (n << 5) | t); }
  void add_imm(Reg d, Reg n, uint32_t imm) { w(0x91000000u | (imm << 10) | (n << 5) | d); }        // imm < 4096
  void sub_imm(Reg d, Reg n, uint32_t imm) { w(0xD1000000u | (imm << 10) | (n << 5) | d); }
  void add(Reg d, Reg n, Reg m) { w(0x8B000000u | (m << 16) | (n << 5) | d); }
  void add_lsl(Reg d, Reg n, Reg m, unsigned sh) { w(0x8B000000u | (m << 16) | (sh << 10) | (n << 5) | d); }
  void sub(Reg d, Reg n, Reg m) { w(0xCB000000u | (m << 16) | (n << 5) | d); }
  void mul(Reg d, Reg n, Reg m) { w(0x9B007C00u | (m << 16) | (n << 5) | d); }
  void sdiv(Reg d, Reg n, Reg m) { w(0x9AC00C00u | (m << 16) | (n << 5) | d); }
  void msub(Reg d, Reg n, Reg m, Reg a) { w(0x9B008000u | (m << 16) | (a << 10) | (n << 5) | d); }   // d = a - n*m
  void neg(Reg d, Reg m) { w(0xCB0003E0u | (m << 16) | d); }
  void cmp(Reg n, Reg m) { w(0xEB00001Fu | (m << 16) | (n << 5)); }
  void cmp_imm_w(Reg n, uint32_t imm) { w(0x7100001Fu | (imm << 10) | (n << 5)); }
  void cset(Reg d, Cond c) { w(0x9A9F07E0u | ((c ^ 1u) << 12) | d); }
  void blr(Reg n) { w(0xD63F0000u | (n << 5)); }
  void ldr_w(Reg t, Reg n, uint32_t off) { w(0xB9400000u | ((off / 4) << 10) | (n << 5) | t); }   // off % 4 == 0
  void ldxr_w(Reg t, Reg n) { w(0x885F7C00u | (n << 5) | t); }
  void stxr_w(Reg s, Reg t, Reg n) { w(0x88007C00u | (s << 16) | (n << 5) | t); }
  void ldxr_x(Reg t, Reg n) { w(0xC85F7C00u | (n << 5) | t); }
  void stxr_x(Reg s, Reg t, Reg n) { w(0xC8007C00u | (s << 16) | (n << 5) | t); }
  void cmp_imm(Reg n, uint32_t imm) { w(0xF100001Fu | (imm << 10) | (n << 5)); }
  void clrex() { w(0xD503305Fu); }
  void add_imm_w(Reg d, Reg n, uint32_t imm) { w(0x11000000u | (imm << 10) | (n << 5) | d); }
  void sub_imm_w(Reg d, Reg n, uint32_t imm) { w(0x51000000u | (imm << 10) | (n << 5) | d); }
  void lsr_imm(Reg d, Reg n, unsigned sh) { w(0xD340FC00u | (sh << 16) | (n << 5) | d); }        // ubfm d, n, #sh, #63
  void ret() { w(0xD65F03C0u); }
  // Branches: emitted with a zero offset, patched once the target is known.
  size_t b() { w(0x14000000u); return here() - 1; }
  size_t bcond(Cond c) { w(0x54000000u | c); return here() - 1; }
  size_t cbz_w(Reg t) { w(0x34000000u | t); return here() - 1; }
  size_t cbnz_w(Reg t) { w(0x35000000u | t); return here() - 1; }
  size_t cbz_x(Reg t) { w(0xB4000000u | t); return here() - 1; }
  size_t cbnz_x(Reg t) { w(0xB5000000u | t); return here() - 1; }
  void patch(size_t at, size_t target) {
    const int32_t d = static_cast<int32_t>(target) - static_cast<int32_t>(at);
    uint32_t& ins = buf[at];
    if ((ins & 0xFC000000u) == 0x14000000u) ins |= static_cast<uint32_t>(d) & 0x03FFFFFFu;
    else ins |= (static_cast<uint32_t>(d) & 0x7FFFFu) << 5;          // b.cond / cbz / cbnz: imm19 at bit 5
  }
};

// ---- bytecode shape ---------------------------------------------------------
// Length of the instruction at `ip`, or 0 if the opcode is not one this
// compiler knows (then the whole function stays interpreted).
size_t insn_len(const FnObj& fn, size_t ip) {
  const auto& code = fn.chunk.code;
  switch (static_cast<Op>(code[ip])) {
    case Op::Nil: case Op::True: case Op::False: case Op::Pop:
    case Op::Equal: case Op::NotEqual: case Op::Less: case Op::LessEqual: case Op::Greater: case Op::GreaterEqual:
    case Op::Add: case Op::Subtract: case Op::Multiply: case Op::Divide: case Op::Modulo: case Op::Negate: case Op::Not:
    case Op::BitAnd: case Op::BitOr: case Op::BitXor: case Op::BitNot: case Op::Shl: case Op::Shr:
    case Op::Dup: case Op::Dup2:
    case Op::CloseUpvalue: case Op::Return: case Op::IndexGet: case Op::IndexSet: case Op::IterPrep: case Op::Halt:
      return 1;
    case Op::GetLocal: case Op::SetLocal: case Op::GetUpvalue: case Op::SetUpvalue: case Op::Call: case Op::IterNext:
      return 2;
    case Op::Constant: case Op::GetGlobal: case Op::SetGlobal: case Op::DefineGlobal:
    case Op::FieldGet: case Op::OptFieldGet: case Op::FieldSet: case Op::BuildList: case Op::BuildMap:
    case Op::Jump: case Op::JumpIfFalse: case Op::JumpIfTrue: case Op::Loop:
      return 3;
    // spread: compiled as one interpreter step each (a function that is not compiled at all would be called from
    // native code as if it were, so every function compiles)
    case Op::CallList:
      return 1;
    case Op::ListConcat:
      return 3;
    case Op::Closure: {
      if (ip + 2 >= code.size()) return 0;
      const size_t k = (static_cast<size_t>(code[ip + 1]) << 8) | code[ip + 2];
      if (k >= fn.chunk.constants.size() || !fn.chunk.constants[k].obj) return 0;
      const auto* sub = static_cast<const FnObj*>(fn.chunk.constants[k].obj.get());
      return 3 + 2 * sub->upvalue_index.size();
    }
  }
  return 0;
}

class Compiler {
 public:
  explicit Compiler(const FnObj& fn) : fn_(fn), code_(fn.chunk.code) {}

  bool build() {
    // Instruction boundaries first: every jump target must be one.
    std::vector<size_t> starts;
    std::vector<char> is_start(code_.size() + 1, 0);
    for (size_t ip = 0; ip < code_.size();) {
      const size_t n = insn_len(fn_, ip);
      if (n == 0 || ip + n > code_.size()) return false;
      starts.push_back(ip);
      is_start[ip] = 1;
      ip += n;
    }
    is_start[code_.size()] = 1;
    label_.assign(code_.size() + 1, SIZE_MAX);

    prologue();
    for (size_t idx = 0; idx < starts.size(); ++idx) {
      const size_t ip = starts[idx];
      label_[ip] = a_.here();
      const Op op = static_cast<Op>(code_[ip]);
      switch (op) {
        case Op::Constant: emit_constant(ip); break;
        case Op::Nil: push_imm(ip, Type::Nil, 0); break;
        case Op::True: push_imm(ip, Type::Bool, 1); break;
        case Op::False: push_imm(ip, Type::Bool, 0); break;
        case Op::Pop: emit_pop(ip); break;
        case Op::GetLocal: emit_get_local(ip); break;
        case Op::SetLocal: emit_set_local(ip); break;
        case Op::Add: case Op::Subtract: case Op::Multiply: emit_arith(ip, op); break;
        case Op::Modulo: emit_modulo(ip); break;
        case Op::BitAnd: case Op::BitOr: case Op::BitXor: case Op::Shl: case Op::Shr: emit_bitop(ip, op); break;
        case Op::BitNot: emit_bitnot(ip); break;
        case Op::Equal: case Op::NotEqual: case Op::Less: case Op::LessEqual: case Op::Greater: case Op::GreaterEqual:
          emit_compare(ip, op); break;
        case Op::Negate: emit_negate(ip); break;
        case Op::IterNext: emit_iter_next(ip); break;
        case Op::Not: emit_not(ip); break;
        case Op::Jump: {
          const size_t t = ip + 3 + rd16(ip + 1);
          if (!is_start_at(is_start, t)) return false;
          fixups_.push_back({a_.b(), t});
          break;
        }
        case Op::JumpIfFalse: case Op::JumpIfTrue: {
          const size_t t = ip + 3 + rd16(ip + 1);
          if (!is_start_at(is_start, t)) return false;
          emit_cond_jump(op == Op::JumpIfFalse, t);
          break;
        }
        case Op::Loop: {
          const size_t d = rd16(ip + 1);
          if (d > ip + 3) return false;
          const size_t t = ip + 3 - d;
          if (!is_start_at(is_start, t)) return false;
          size_t body = 0;
          for (size_t k = 0; k <= idx; ++k) if (starts[k] >= t) ++body;
          emit_loop(ip, t, body);
          break;
        }
        case Op::Call: emit_call(ip); break;
        case Op::GetGlobal: emit_get_global(ip); break;
        case Op::Return: emit_return(ip); break;
        case Op::Halt:
          step(ip);
          fixups_ok_.push_back(a_.b());
          break;
        default:
          step(ip);   // one instruction, the interpreter's semantics
          break;
      }
    }
    // Falling off the end cannot happen (the compiler always ends a function
    // with RET), but if it did, fail like the interpreter does.
    label_[code_.size()] = a_.here();
    step(code_.size() ? starts.back() : 0);
    fixups_fail_.push_back(a_.b());

    // Epilogues.
    const size_t ok_at = a_.here();
    a_.movz(X0, 1, 0);
    const size_t join = a_.b();
    const size_t fail_at = a_.here();
    a_.movz(X0, 0, 0);
    a_.patch(join, a_.here());
    epilogue();

    for (auto& f : fixups_) a_.patch(f.at, label_[f.target_ip]);
    for (size_t at : fixups_ok_) a_.patch(at, ok_at);
    for (size_t at : fixups_fail_) a_.patch(at, fail_at);
    return true;
  }

  const std::vector<uint32_t>& words() const { return a_.buf; }

 private:
  struct Fixup { size_t at; size_t target_ip; };
  const FnObj& fn_;
  const std::vector<uint8_t>& code_;
  Asm a_;
  std::vector<size_t> label_;
  std::vector<Fixup> fixups_;
  std::vector<size_t> fixups_ok_, fixups_fail_;

  size_t rd16(size_t at) const { return (static_cast<size_t>(code_[at]) << 8) | code_[at + 1]; }
  static bool is_start_at(const std::vector<char>& s, size_t t) { return t < s.size() && s[t]; }

  void prologue() {
    a_.w(0xA9BC7BFDu);            // stp x29, x30, [sp, #-64]!
    a_.w(0x910003FDu);            // mov x29, sp
    a_.w(0xA90153F3u);            // stp x19, x20, [sp, #16]
    a_.w(0xA9025BF5u);            // stp x21, x22, [sp, #32]
    a_.w(0xA90363F7u);            // stp x23, x24, [sp, #48]
    a_.mov(X19, X0);
    a_.mov(X21, X1);
    a_.ldr(X24, X19, 8);          // &stack
    reload();
  }
  void epilogue() {
    a_.w(0xA94363F7u);            // ldp x23, x24, [sp, #48]
    a_.w(0xA9425BF5u);            // ldp x21, x22, [sp, #32]
    a_.w(0xA94153F3u);            // ldp x19, x20, [sp, #16]
    a_.w(0xA8C47BFDu);            // ldp x29, x30, [sp], #64
    a_.ret();
  }
  void reload() {
    a_.ldr(X9, X24, 0);                 // begin
    a_.add_lsl(X20, X9, X21, 5);        // + base * 32
    a_.ldr(X22, X24, 8);                // finish
    a_.ldr(X23, X24, 16);               // end of storage
  }
  // The instruction at `ip`, by the interpreter. Fails out on error.
  void step(size_t ip) { call_helper(ip, &grain_jit_step); }
  // Any helper of the shape int(Ctx*, uint32_t ip): flush, call, fail out on
  // 0, reload (the helper may have grown the stack).
  void call_helper(size_t ip, int (*fn)(Ctx*, uint32_t)) {
    a_.str(X22, X24, 8);                // hand the vector its real size
    a_.mov(X0, X19);
    a_.mov_imm(X1, ip);
    a_.mov_imm(X16, reinterpret_cast<uint64_t>(fn));
    a_.blr(X16);
    fixups_fail_.push_back(a_.cbz_w(X0));
    reload();
  }
  // Jump here when a fast path does not apply; returns the join point to patch.
  template <class Fast>
  void fast_or_step(size_t ip, Fast&& fast) { fast_or_step_with(ip, &grain_jit_step, fast); }
  template <class Fast>
  void fast_or_step_with(size_t ip, int (*helper)(Ctx*, uint32_t), Fast&& fast) {
    std::vector<size_t> slow;
    fast(slow);
    const size_t done = a_.b();
    const size_t slow_at = a_.here();
    for (size_t s : slow) a_.patch(s, slow_at);
    call_helper(ip, helper);
    a_.patch(done, a_.here());
  }

  // ++use_count of the control block in `ctrl` (non-null), with `addr`,
  // `v` and `status` as scratch. LL/SC, at the width layout() measured.
  void count_inc(Reg ctrl, Reg addr, Reg v, Reg status) {
    const Layout& L = layout();
    a_.add_imm(addr, ctrl, L.sp_use_count);
    const size_t again = a_.here();
    if (L.sp_wide) { a_.ldxr_x(v, addr); a_.add_imm(v, v, 1); a_.stxr_x(status, v, addr); }
    else { a_.ldxr_w(v, addr); a_.add_imm_w(v, v, 1); a_.stxr_w(status, v, addr); }
    a_.patch(a_.cbnz_w(status), again);
  }
  // --use_count unless it is the last owner; returns the branch taken when it
  // is (nothing changed, the exclusive monitor still open: clrex there).
  size_t count_dec_unless_last(Reg ctrl, Reg addr, Reg v, Reg status) {
    const Layout& L = layout();
    a_.add_imm(addr, ctrl, L.sp_use_count);
    const size_t again = a_.here();
    size_t last;
    if (L.sp_wide) {
      a_.ldxr_x(v, addr);
      a_.cmp_imm(v, L.sp_last);
      last = a_.bcond(LE);
      a_.sub_imm(v, v, 1);
      a_.stxr_x(status, v, addr);
    } else {
      a_.ldxr_w(v, addr);
      a_.cmp_imm_w(v, L.sp_last);
      last = a_.bcond(LS);
      a_.sub_imm_w(v, v, 1);
      a_.stxr_w(status, v, addr);
    }
    a_.patch(a_.cbnz_w(status), again);
    return last;
  }
  void ref_inc(Reg ctrl) { count_inc(ctrl, X15, X16, X17); }
  // --use_count, unless it is the last reference: then nothing has been
  // changed and control goes to the slow path (the destructor runs in C++).
  void ref_dec_or_slow(Reg ctrl, std::vector<size_t>& slow) {
    const size_t last = count_dec_unless_last(ctrl, X15, X16, X17);
    const size_t done = a_.b();
    a_.patch(last, a_.here());
    a_.clrex();
    slow.push_back(a_.b());
    a_.patch(done, a_.here());
  }
  // Push a copy of the Value at [src] (any type): refcount first, then bits.
  void push_copy_from(Reg src, std::vector<size_t>& slow) {
    a_.cmp(X22, X23);
    slow.push_back(a_.bcond(HS));
    a_.ldr(X13, src, kCtrlOff);
    const size_t plain = a_.cbz_x(X13);
    ref_inc(X13);
    a_.patch(plain, a_.here());
    a_.ldr(X10, src, 0);
    a_.ldr(X11, src, kPayOff);
    a_.ldr(X12, src, 16);
    a_.str(X10, X22, 0);
    a_.str(X11, X22, kPayOff);
    a_.str(X12, X22, 16);
    a_.str(X13, X22, kCtrlOff);
    a_.add_imm(X22, X22, 32);
  }

  void push_imm(size_t ip, Type t, uint64_t bits) {
    fast_or_step(ip, [&](std::vector<size_t>& slow) {
      a_.cmp(X22, X23);
      slow.push_back(a_.bcond(HS));              // full: let the vector grow
      a_.movz(X9, static_cast<uint32_t>(t), 0);
      a_.mov_imm(X10, bits);
      a_.str(X9, X22, 0);
      a_.str(X10, X22, kPayOff);
      a_.str(XZR, X22, 16);
      a_.str(XZR, X22, kCtrlOff);
      a_.add_imm(X22, X22, 32);
    });
  }

  void emit_constant(size_t ip) {
    const Value& v = fn_.chunk.constants[rd16(ip + 1)];
    if (!v.obj && (v.type == Type::Int || v.type == Type::Real || v.type == Type::Bool || v.type == Type::Nil)) {
      push_imm(ip, v.type, static_cast<uint64_t>(v.i));
    } else {
      // A string (or other object) constant: the chunk's own Value, which
      // lives as long as this code does.
      fast_or_step(ip, [&](std::vector<size_t>& slow) {
        a_.mov_imm(X9, reinterpret_cast<uint64_t>(&v));
        push_copy_from(X9, slow);
      });
    }
  }

  void emit_pop(size_t ip) {
    fast_or_step(ip, [&](std::vector<size_t>& slow) {
      a_.ldur(X9, X22, -32 + kCtrlOff);
      const size_t plain = a_.cbz_x(X9);
      ref_dec_or_slow(X9, slow);                 // the last reference: destructor, in C++
      a_.patch(plain, a_.here());
      a_.sub_imm(X22, X22, 32);
    });
  }

  void local_addr(Reg d, size_t slot) {
    const uint32_t off = static_cast<uint32_t>(slot * 32);
    if (off < 4096) a_.add_imm(d, X20, off);
    else { a_.mov_imm(d, off); a_.add(d, X20, d); }
  }

  void emit_get_local(size_t ip) {
    fast_or_step(ip, [&](std::vector<size_t>& slow) {
      local_addr(X9, code_[ip + 1]);
      push_copy_from(X9, slow);
    });
  }

  // local = top (top stays): release what the local held (unless that is the
  // last reference - then the interpreter does it), retain the new value.
  void emit_set_local(size_t ip) {
    fast_or_step(ip, [&](std::vector<size_t>& slow) {
      local_addr(X9, code_[ip + 1]);
      a_.ldr(X13, X9, kCtrlOff);                 // old
      a_.ldur(X14, X22, -32 + kCtrlOff);         // new
      a_.cmp(X13, X14);
      const size_t same = a_.bcond(EQ);          // same object (or both plain): counts unchanged
      const size_t old_plain = a_.cbz_x(X13);
      ref_dec_or_slow(X13, slow);
      a_.patch(old_plain, a_.here());
      a_.ldur(X14, X22, -32 + kCtrlOff);
      const size_t new_plain = a_.cbz_x(X14);
      ref_inc(X14);
      a_.patch(new_plain, a_.here());
      a_.patch(same, a_.here());
      a_.ldur(X10, X22, -32);
      a_.ldur(X11, X22, -32 + kPayOff);
      a_.ldur(X12, X22, -32 + 16);
      a_.ldur(X14, X22, -32 + kCtrlOff);
      a_.str(X10, X9, 0);
      a_.str(X11, X9, kPayOff);
      a_.str(X12, X9, 16);
      a_.str(X14, X9, kCtrlOff);
    });
  }

  // Both operands Int (type byte 2), or take the slow path.
  void both_int(std::vector<size_t>& slow) {
    a_.ldurb(X9, X22, -64);
    a_.cmp_imm_w(X9, static_cast<uint32_t>(Type::Int));
    slow.push_back(a_.bcond(NE));
    a_.ldurb(X10, X22, -32);
    a_.cmp_imm_w(X10, static_cast<uint32_t>(Type::Int));
    slow.push_back(a_.bcond(NE));
    a_.ldur(X11, X22, -64 + kPayOff);
    a_.ldur(X12, X22, -32 + kPayOff);
  }

  void emit_arith(size_t ip, Op op) {
    fast_or_step(ip, [&](std::vector<size_t>& slow) {
      both_int(slow);
      if (op == Op::Add) a_.add(X11, X11, X12);
      else if (op == Op::Subtract) a_.sub(X11, X11, X12);
      else a_.mul(X11, X11, X12);
      a_.stur(X11, X22, -64 + kPayOff);
      a_.sub_imm(X22, X22, 32);
    });
  }

  // Int % Int, C++ semantics (sdiv truncates; msub = a - (a/b)*b). Zero
  // divisor and every other type: the interpreter.
  void emit_modulo(size_t ip) {
    fast_or_step(ip, [&](std::vector<size_t>& slow) {
      both_int(slow);
      slow.push_back(a_.cbz_x(X12));
      a_.sdiv(X13, X11, X12);
      a_.msub(X11, X13, X12, X11);
      a_.stur(X11, X22, -64 + kPayOff);
      a_.sub_imm(X22, X22, 32);
    });
  }

  // Int op Int. Shifts only inline for counts 0..63: lslv/asrv take the count
  // mod 64, where grain shifts everything out (and errors on a negative) -
  // the interpreter handles those.
  void emit_bitop(size_t ip, Op op) {
    fast_or_step(ip, [&](std::vector<size_t>& slow) {
      both_int(slow);
      if (op == Op::Shl || op == Op::Shr) {
        a_.w(0xF100FD9Fu);                         // cmp x12, #63
        slow.push_back(a_.bcond(HI));              // unsigned: negative counts land here too
      }
      if (op == Op::BitAnd) a_.w(0x8A000000u | (X12 << 16) | (X11 << 5) | X11); // and x11, x11, x12
      else if (op == Op::BitOr) a_.w(0xAA000000u | (X12 << 16) | (X11 << 5) | X11); // orr x11, x11, x12
      else if (op == Op::BitXor) a_.w(0xCA000000u | (X12 << 16) | (X11 << 5) | X11); // eor x11, x11, x12
      else if (op == Op::Shl) a_.w(0x9AC02000u | (X12 << 16) | (X11 << 5) | X11); // lslv x11, x11, x12
      else if (op == Op::Shr) a_.w(0x9AC02800u | (X12 << 16) | (X11 << 5) | X11); // asrv x11, x11, x12
      a_.stur(X11, X22, -64 + kPayOff);
      a_.sub_imm(X22, X22, 32);
    });
  }

  void emit_bitnot(size_t ip) {
    fast_or_step(ip, [&](std::vector<size_t>& slow) {
      a_.ldurb(X9, X22, -32);
      a_.cmp_imm_w(X9, static_cast<uint32_t>(Type::Int));
      slow.push_back(a_.bcond(NE));
      a_.ldur(X11, X22, -32 + kPayOff);
      a_.w(0xAA2003E0u | (X11 << 16) | X11); // mvn x11, x11 (orn x11, xzr, x11)
      a_.stur(X11, X22, -32 + kPayOff);
    });
  }

  void emit_compare(size_t ip, Op op) {
    fast_or_step(ip, [&](std::vector<size_t>& slow) {
      both_int(slow);
      a_.cmp(X11, X12);
      const Cond c = op == Op::Equal ? EQ : op == Op::NotEqual ? NE : op == Op::Less ? LT
                   : op == Op::LessEqual ? LE : op == Op::Greater ? GT : GE;
      a_.cset(X13, c);
      a_.movz(X14, static_cast<uint32_t>(Type::Bool), 0);
      a_.stur(X14, X22, -64);
      a_.stur(X13, X22, -64 + kPayOff);
      a_.sub_imm(X22, X22, 32);
    });
  }

  // for-loop step: seq (a List) in local `slot`, cursor (Int) in slot+1.
  // Pushes the next item and true, or false when done - as the interpreter.
  void emit_iter_next(size_t ip) {
    const size_t slot = code_[ip + 1];
    const uint32_t L = static_cast<uint32_t>(list_items_offset());
    fast_or_step(ip, [&](std::vector<size_t>& slow) {
      local_addr(X9, slot);
      a_.ldrb(X10, X9, kTypeOff);
      a_.cmp_imm_w(X10, static_cast<uint32_t>(Type::List));
      slow.push_back(a_.bcond(NE));
      a_.ldrb(X10, X9, 32 + kTypeOff);
      a_.cmp_imm_w(X10, static_cast<uint32_t>(Type::Int));
      slow.push_back(a_.bcond(NE));
      a_.add_imm(X10, X22, 64);                  // room for two pushes
      a_.cmp(X10, X23);
      slow.push_back(a_.bcond(HI));
      a_.ldr(X11, X9, 16);                       // ListObj*
      a_.ldr(X12, X11, L);                       // items begin
      a_.ldr(X13, X11, L + 8);                   // items end
      a_.ldr(X14, X9, 32 + kPayOff);             // cursor
      a_.add_lsl(X12, X12, X14, 5);              // &items[cursor]
      a_.cmp(X12, X13);
      const size_t more = a_.bcond(LO);
      // done: push false
      a_.movz(X10, static_cast<uint32_t>(Type::Bool), 0);
      a_.str(X10, X22, 0);
      a_.str(XZR, X22, kPayOff);
      a_.str(XZR, X22, 16);
      a_.str(XZR, X22, kCtrlOff);
      a_.add_imm(X22, X22, 32);
      const size_t out = a_.b();
      a_.patch(more, a_.here());
      a_.ldr(X10, X12, kCtrlOff);
      slow.push_back(a_.cbnz_x(X10));            // refcounted item: the interpreter copies it
      a_.ldr(X10, X12, 0);
      a_.ldr(X11, X12, kPayOff);
      a_.str(X10, X22, 0);
      a_.str(X11, X22, kPayOff);
      a_.str(XZR, X22, 16);
      a_.str(XZR, X22, kCtrlOff);
      a_.add_imm(X14, X14, 1);
      a_.str(X14, X9, 32 + kPayOff);
      a_.movz(X10, static_cast<uint32_t>(Type::Bool), 0);
      a_.movz(X11, 1, 0);
      a_.str(X10, X22, 32);
      a_.str(X11, X22, 32 + kPayOff);
      a_.str(XZR, X22, 32 + 16);
      a_.str(XZR, X22, 32 + kCtrlOff);
      a_.add_imm(X22, X22, 64);
      a_.patch(out, a_.here());
    });
  }

  // GETGLOBAL through the chunk's inline cache (Chunk::global_cache, filled
  // by the VM on first use): owner check, slot, copy - with the refcount
  // increment done here, LL/SC, when the value is an object.
  void emit_get_global(size_t ip) {
    const uint32_t k = static_cast<uint32_t>(rd16(ip + 1));
    const Chunk& ch = fn_.chunk;
    const auto cache_off = static_cast<uint32_t>(offsetof(Chunk, global_cache));
    const auto owner_off = static_cast<uint32_t>(offsetof(Chunk, global_cache_owner));
    fast_or_step_with(ip, &grain_jit_get_global, [&](std::vector<size_t>& slow) {
      a_.mov_imm(X9, reinterpret_cast<uint64_t>(&ch));
      a_.ldr(X10, X9, owner_off);
      a_.ldr(X11, X19, 48);                      // ctx->serial
      a_.cmp(X10, X11);
      slow.push_back(a_.bcond(NE));
      a_.ldr(X10, X9, cache_off);                // cache data (sized to the constants when owned)
      if (k * 8 < 32768) a_.ldr(X12, X10, k * 8); else { a_.mov_imm(X11, k * 8); a_.add(X10, X10, X11); a_.ldr(X12, X10, 0); }
      slow.push_back(a_.cbz_x(X12));             // not resolved yet
      a_.cmp(X22, X23);
      slow.push_back(a_.bcond(HS));
      a_.ldr(X13, X12, kCtrlOff);
      const size_t plain = a_.cbz_x(X13);
      count_inc(X13, X14, X10, X11);
      a_.patch(plain, a_.here());
      a_.ldr(X10, X12, 0);
      a_.ldr(X11, X12, kPayOff);
      a_.ldr(X14, X12, 16);
      a_.str(X10, X22, 0);
      a_.str(X11, X22, kPayOff);
      a_.str(X14, X22, 16);
      a_.str(X13, X22, kCtrlOff);
      a_.add_imm(X22, X22, 32);
    });
  }

  // CALL: when the callee is a closure whose function already has native
  // code and takes exactly argc arguments, push its Frame here and branch to
  // it. Everything else - natives, first calls, arity errors, the depth
  // limit, bare FnObjs - goes through Vm::jit_call, i.e. Vm::invoke.
  void emit_call(size_t ip) {
    const Layout& L = layout();
    const uint32_t argc = code_[ip + 1];
    const int callee_off = -static_cast<int>(32 * (argc + 1));
    fast_or_step_with(ip, &grain_jit_call, [&](std::vector<size_t>& slow) {
      if (-callee_off < 4096) a_.sub_imm(X9, X22, static_cast<uint32_t>(-callee_off));
      else { a_.mov_imm(X9, static_cast<uint64_t>(-callee_off)); a_.sub(X9, X22, X9); }
      a_.ldrb(X10, X9, kTypeOff);
      a_.cmp_imm_w(X10, static_cast<uint32_t>(Type::Fn));
      slow.push_back(a_.bcond(NE));
      a_.ldr(X10, X9, 16);                       // ClosureObj* (or bare FnObj*)
      a_.ldrb(X11, X10, L.obj_is_closure);
      slow.push_back(a_.cbz_w(X11));
      a_.ldr(X12, X10, L.closure_fn);            // FnObj*
      a_.ldr(X13, X12, L.fn_jit);                // its native code
      slow.push_back(a_.cbz_x(X13));
      a_.ldr_w(X14, X12, L.fn_arity);
      a_.cmp_imm_w(X14, argc);
      slow.push_back(a_.bcond(NE));
      a_.ldr(X15, X19, 32);                      // &frames
      a_.ldr(X16, X15, 8);                       // finish
      a_.ldr(X17, X15, 16);                      // capacity end
      a_.cmp(X16, X17);
      slow.push_back(a_.bcond(HS));
      a_.ldr(X11, X15, 0);                       // begin
      a_.sub(X11, X16, X11);
      a_.mov_imm(X14, static_cast<uint64_t>(kMaxFrames) * kFrameSize);
      a_.cmp(X11, X14);
      slow.push_back(a_.bcond(HS));              // at the depth limit: the VM reports it
      // The caller's frame records where it is (traces name this call).
      a_.mov_imm(X14, ip + 2);
      a_.stur(X14, X16, -static_cast<int>(kFrameSize) + static_cast<int>(kFrameIp));
      // The callee's Frame{closure, ip 0, base, owned {}, chunk}.
      a_.ldr(X11, X24, 0);
      a_.sub(X14, X9, X11);
      a_.lsr_imm(X14, X14, 5);                   // base index
      a_.str(X10, X16, kFrameClosure);
      a_.str(XZR, X16, kFrameIp);
      a_.str(X14, X16, kFrameBase);
      a_.str(XZR, X16, kFrameOwnedPtr);
      a_.str(XZR, X16, kFrameOwnedCtrl);
      if (L.fn_chunk < 4096) a_.add_imm(X11, X12, L.fn_chunk); else { a_.mov_imm(X11, L.fn_chunk); a_.add(X11, X12, X11); }
      a_.str(X11, X16, kFrameChunk);
      a_.add_imm(X16, X16, kFrameSize);
      a_.str(X16, X15, 8);
      a_.str(X22, X24, 8);                       // the callee sees the real stack
      a_.mov(X0, X19);
      a_.mov(X1, X14);
      a_.blr(X13);
      fixups_fail_.push_back(a_.cbz_w(X0));
      reload();
    });
  }

  // RETURN: when no open upvalue, no refcounted local and no owned closure
  // needs tearing down, drop the frame here: the result replaces the callee
  // slot (whose closure reference is released, LL/SC, unless it is the last).
  // Otherwise Vm::jit_return does it.
  void emit_return(size_t ip) {
    std::vector<size_t> slow;
    a_.ldr(X9, X19, 40);                         // &open upvalues
    a_.ldr(X10, X9, 0);
    a_.ldr(X11, X9, 8);
    a_.cmp(X10, X11);
    slow.push_back(a_.bcond(NE));
    a_.ldr(X15, X19, 32);                        // &frames
    a_.ldr(X16, X15, 8);
    a_.ldur(X10, X16, -static_cast<int>(kFrameSize) + static_cast<int>(kFrameOwnedCtrl));
    slow.push_back(a_.cbnz_x(X10));
    // locals/args between the callee slot and the result: none refcounted
    a_.add_imm(X9, X20, 32);
    a_.sub_imm(X12, X22, 32);                    // the result
    const size_t loop = a_.here();
    a_.cmp(X9, X12);
    const size_t scanned = a_.bcond(HS);
    a_.ldr(X10, X9, kCtrlOff);
    slow.push_back(a_.cbnz_x(X10));
    a_.add_imm(X9, X9, 32);
    a_.patch(a_.b(), loop);
    a_.patch(scanned, a_.here());
    // release the callee slot's reference (never the last one - that would
    // need the destructor, so leave it to the VM)
    a_.ldr(X13, X20, kCtrlOff);
    const size_t no_ref = a_.cbz_x(X13);
    const size_t last = count_dec_unless_last(X13, X14, X10, X11);
    a_.patch(no_ref, a_.here());
    // result -> callee slot; pop the frame
    a_.ldr(X10, X12, 0);
    a_.ldr(X11, X12, kPayOff);
    a_.ldr(X13, X12, 16);
    a_.ldr(X14, X12, kCtrlOff);
    a_.str(X10, X20, 0);
    a_.str(X11, X20, kPayOff);
    a_.str(X13, X20, 16);
    a_.str(X14, X20, kCtrlOff);
    a_.add_imm(X22, X20, 32);
    a_.str(X22, X24, 8);
    a_.sub_imm(X16, X16, kFrameSize);
    a_.str(X16, X15, 8);
    fixups_ok_.push_back(a_.b());
    const size_t last_at = a_.here();
    a_.clrex();
    a_.patch(last, last_at);
    const size_t slow_at = a_.here();
    for (size_t s2 : slow) a_.patch(s2, slow_at);
    // (last_at falls into slow_at)
    call_helper(ip, &grain_jit_return);
    fixups_ok_.push_back(a_.b());
  }

  void emit_negate(size_t ip) {
    fast_or_step(ip, [&](std::vector<size_t>& slow) {
      a_.ldurb(X9, X22, -32);
      a_.cmp_imm_w(X9, static_cast<uint32_t>(Type::Int));
      slow.push_back(a_.bcond(NE));
      a_.ldur(X11, X22, -32 + kPayOff);
      a_.neg(X11, X11);
      a_.stur(X11, X22, -32 + kPayOff);
    });
  }

  // Falsy = nil, or a bool whose payload is 0. Leaves 1 in X13 when truthy.
  void truthy_to_x13() {
    a_.ldurb(X9, X22, -32);
    a_.movz(X13, 1, 0);
    const size_t not_nil = a_.cbnz_w(X9);
    a_.movz(X13, 0, 0);
    const size_t done1 = a_.b();
    a_.patch(not_nil, a_.here());
    a_.cmp_imm_w(X9, static_cast<uint32_t>(Type::Bool));
    const size_t not_bool = a_.bcond(NE);
    a_.ldurb(X13, X22, -32 + kPayOff);
    a_.patch(not_bool, a_.here());
    a_.patch(done1, a_.here());
  }

  void emit_not(size_t ip) {
    fast_or_step(ip, [&](std::vector<size_t>& slow) {
      a_.ldur(X10, X22, -32 + kCtrlOff);
      slow.push_back(a_.cbnz_x(X10));
      truthy_to_x13();
      a_.w(0xD2400000u | (0 << 16) | (0 << 10) | (X13 << 5) | X13);   // eor x13, x13, #1
      a_.movz(X14, static_cast<uint32_t>(Type::Bool), 0);
      a_.stur(X14, X22, -32);
      a_.stur(X13, X22, -32 + kPayOff);
    });
  }

  // JUMPF/JUMPT leave the condition on the stack (the compiler pops it on
  // both paths), so this only reads it.
  void emit_cond_jump(bool if_false, size_t target) {
    truthy_to_x13();
    fixups_.push_back({if_false ? a_.cbz_w(X13) : a_.cbnz_w(X13), target});
  }

  // Back-edge: count the loop body's instructions against the step limit
  // (a runaway script must still stop), then branch.
  void emit_loop(size_t ip, size_t target, size_t body) {
    a_.ldr(X9, X19, 16);                 // &steps
    a_.ldr(X10, X9, 0);
    if (body < 4096) a_.add_imm(X10, X10, static_cast<uint32_t>(body));
    else { a_.mov_imm(X11, body); a_.add(X10, X10, X11); }
    a_.str(X10, X9, 0);
    a_.ldr(X11, X19, 24);                // &step_limit
    a_.ldr(X11, X11, 0);
    const size_t unlimited = a_.cbz_x(X11);
    a_.cmp(X10, X11);
    const size_t within = a_.bcond(LS);
    step(ip);                            // over the limit: the interpreter reports it
    a_.patch(unlimited, a_.here());
    a_.patch(within, a_.here());
    fixups_.push_back({a_.b(), target});
  }
};

// ---- executable memory -----------------------------------------------------
void* alloc_exec(size_t bytes) {
#if defined(AIOS_METAL)
  return std::malloc(bytes);             // the kernel maps all of RAM executable
#elif defined(__APPLE__)
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
  return p == MAP_FAILED ? nullptr : p;
#else
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return p == MAP_FAILED ? nullptr : p;
#endif
}

// Write `bytes` of code at `mem` (the allocation's size goes in front of it,
// see compile()) and make it fetchable.
void install(uint8_t* mem, size_t alloc, const void* words, size_t bytes) {
  uint8_t* code = mem + 16;
#if defined(__APPLE__)
  pthread_jit_write_protect_np(0);
  *reinterpret_cast<size_t*>(mem) = alloc;
  std::memcpy(code, words, bytes);
  pthread_jit_write_protect_np(1);
  sys_icache_invalidate(code, bytes);
#else
  *reinterpret_cast<size_t*>(mem) = alloc;
  std::memcpy(code, words, bytes);
  __builtin___clear_cache(reinterpret_cast<char*>(code), reinterpret_cast<char*>(code + bytes));
#endif
}

}  // namespace

Entry compile(const FnObj& fn) {
  if (!vector_layout_ok() || !layout().ok) return nullptr;
  Compiler c(fn);
  if (!c.build()) return nullptr;
  const auto& words = c.words();
  const size_t bytes = words.size() * 4;
  // Remember the size in front of the code so release() can hand it back.
  auto* mem = static_cast<uint8_t*>(alloc_exec(bytes + 16));
  if (!mem) return nullptr;
  install(mem, bytes + 16, words.data(), bytes);
  return reinterpret_cast<Entry>(mem + 16);
}

void release(void* code) {
  if (!code) return;
  uint8_t* mem = static_cast<uint8_t*>(code) - 16;
#if defined(AIOS_METAL)
  std::free(mem);
#else
  munmap(mem, *reinterpret_cast<size_t*>(mem));
#endif
}

#elif !(defined(__x86_64__) && !defined(_WIN32) && !defined(AIOS_METAL))
// neither AArch64 nor x86-64 (grain_jit_x64.cpp), or Windows: interpret everything

Entry compile(const FnObj&) { return nullptr; }
void release(void*) {}

#endif

}  // namespace aios::userland::grain::jit
