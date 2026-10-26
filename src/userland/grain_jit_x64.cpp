// grain -> x86-64 machine code.
//
// The AArch64 compiler's design (grain_jit_arm64.cpp) on the other common machine: a template compiler, each
// bytecode instruction a short run of native code on the VM's own value stack, native and interpreted frames
// interleaving freely. What is compiled inline is what loops are made of - locals, constants, integer arithmetic
// and comparisons, branches, loop back-edges, list iteration, cached globals (read and assigned), calls of compiled
// closures and their returns. Everything else, and any type a fast path does not expect, hands that one instruction to the
// interpreter (Vm::step_one), so there is one implementation of the language's semantics.
//
// Registers inside generated code (all callee-saved in the System V ABI):
//   rbx  Ctx*                r14  stack top (one past the last Value)
//   r12  this frame's slot 0 r15  stack capacity end
//   r13  frame base index    rbp  &std::vector<Value> (begin/finish/cap)
// r14 is written back to the vector's finish pointer before each helper call; r12/r14/r15 are reloaded after,
// because the helper may have grown (reallocated) the stack. Scratch: r8-r11, rsi, rdi, and rax/rcx/rdx (the
// reference-count sequences and the division need those).
#include "grain_jit.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(__x86_64__) && !defined(_WIN32) && !defined(AIOS_METAL)
#include <sys/mman.h>
#endif

namespace aios::userland::grain::jit {

// The Windows x64 ABI passes arguments elsewhere and saves other registers: interpreted there, for now.
#if defined(__x86_64__) && !defined(_WIN32) && !defined(AIOS_METAL)

namespace {

// ---- layout the generated code depends on --------------------------------
static_assert(sizeof(Value) == 32, "Value is 32 bytes: type @0, payload @8, shared_ptr @16/@24");
static_assert(offsetof(Value, type) == 0, "Value::type at 0");
static_assert(offsetof(Value, i) == 8, "Value payload at 8");
static_assert(offsetof(Value, obj) == 16, "Value::obj at 16");
static_assert(offsetof(Ctx, stack) == 8 && offsetof(Ctx, steps) == 16 && offsetof(Ctx, step_limit) == 24 &&
              offsetof(Ctx, frames) == 32 && offsetof(Ctx, open) == 40 && offsetof(Ctx, serial) == 48, "Ctx layout");
constexpr int kTypeOff = 0, kPayOff = 8, kCtrlOff = 24;   // ctrl block pointer: non-null = refcounted

// std::vector<Value> as {begin, finish, end_of_storage}, checked at run time before the first compile.
bool vector_layout_ok() {
  static const bool verdict = [] {
    std::vector<Value> v;
    v.reserve(8);
    v.resize(3);
    auto** raw = reinterpret_cast<Value**>(&v);
    return sizeof(v) == 24 && raw[0] == v.data() && raw[1] == v.data() + 3 && raw[2] == v.data() + v.capacity();
  }();
  return verdict;
}

size_t list_items_offset() {
  static const size_t off = [] {
    ListObj probe({});
    return static_cast<size_t>(reinterpret_cast<const char*>(&probe.items) - reinterpret_cast<const char*>(&probe));
  }();
  return off;
}

// Where the native call path finds things inside objects that have vtables, measured on real instances, once
// (the same measurements as grain_jit_arm64.cpp's).
struct Layout {
  uint32_t obj_is_closure, closure_fn, fn_jit, fn_arity, fn_chunk, sp_use_count;
  bool sp_wide;       // the count is a long (libc++), not an int (libstdc++)
  uint32_t sp_last;   // the count's value when one owner is left: 1, or 0 where it holds owners - 1 (libc++)
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
    l.ok = raw[0] == fn.get() && counts && sizeof(std::shared_ptr<FnObj>) == 16;
    return l;
  }();
  return L;
}

// ---- helpers the generated code calls ---------------------------------------
extern "C" int grain_jit_x64_step(Ctx* c, uint32_t ip) { return c->vm->step_one(ip) ? 1 : 0; }
extern "C" int grain_jit_x64_call(Ctx* c, uint32_t ip) { return c->vm->jit_call(ip) ? 1 : 0; }
extern "C" int grain_jit_x64_return(Ctx* c, uint32_t) { c->vm->jit_return(); return 1; }
extern "C" int grain_jit_x64_get_global(Ctx* c, uint32_t ip) { return c->vm->jit_get_global(ip) ? 1 : 0; }

// ---- encoder ----------------------------------------------------------------
enum Reg : uint8_t { RAX = 0, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
// condition codes (the low nibble of Jcc / SETcc)
enum Cc : uint8_t { O = 0, B = 2, AE = 3, E = 4, NE = 5, BE = 6, A = 7, L = 0xC, GE = 0xD, LE = 0xE, G = 0xF };

struct Asm {
  std::vector<uint8_t> buf;
  size_t here() const { return buf.size(); }
  void b(uint8_t v) { buf.push_back(v); }
  void d32(uint32_t v) { for (int i = 0; i < 4; ++i) b(static_cast<uint8_t>(v >> (8 * i))); }
  void d64(uint64_t v) { for (int i = 0; i < 8; ++i) b(static_cast<uint8_t>(v >> (8 * i))); }
  void rex(bool w, uint8_t reg, uint8_t base, bool force = false) {
    const uint8_t r = static_cast<uint8_t>(0x40 | (w ? 8 : 0) | ((reg & 8) ? 4 : 0) | ((base & 8) ? 1 : 0));
    if (r != 0x40 || force) b(r);
  }
  // [base + disp32], with `reg` in ModRM.reg (always the disp32 form: no special cases for rbp/r13)
  void mem(uint8_t reg, uint8_t base, int32_t disp) {
    b(static_cast<uint8_t>(0x80 | ((reg & 7) << 3) | (base & 7)));
    if ((base & 7) == 4) b(0x24);   // rsp/r12 as a base: a SIB byte
    d32(static_cast<uint32_t>(disp));
  }
  void rr(uint8_t reg, uint8_t rm) { b(static_cast<uint8_t>(0xC0 | ((reg & 7) << 3) | (rm & 7))); }

  void load(Reg d, Reg base, int32_t disp) { rex(true, d, base); b(0x8B); mem(d, base, disp); }      // mov d, [base+disp]
  void store(Reg base, int32_t disp, Reg s) { rex(true, s, base); b(0x89); mem(s, base, disp); }     // mov [base+disp], s
  void load32(Reg d, Reg base, int32_t disp) { rex(false, d, base); b(0x8B); mem(d, base, disp); }   // mov d32, [base+disp]
  void loadb(Reg d, Reg base, int32_t disp) { rex(false, d, base); b(0x0F); b(0xB6); mem(d, base, disp); }   // movzx d32, byte
  void store_imm(Reg base, int32_t disp, int32_t imm) { rex(true, 0, base); b(0xC7); mem(0, base, disp); d32(static_cast<uint32_t>(imm)); }
  void lea(Reg d, Reg base, int32_t disp) { rex(true, d, base); b(0x8D); mem(d, base, disp); }
  void mov(Reg d, Reg s) { rex(true, s, d); b(0x89); rr(s, d); }
  void mov_imm64(Reg d, uint64_t v) {
    if (v <= 0xFFFFFFFFull) { rex(false, 0, d); b(static_cast<uint8_t>(0xB8 + (d & 7))); d32(static_cast<uint32_t>(v)); return; }
    rex(true, 0, d); b(static_cast<uint8_t>(0xB8 + (d & 7))); d64(v);
  }
  void alu(uint8_t op, Reg d, Reg s) { rex(true, s, d); b(op); rr(s, d); }      // op d, s: 01 add, 29 sub, 39 cmp, 21 and, 09 or, 31 xor, 85 test
  void alu32(uint8_t op, Reg d, Reg s) { rex(false, s, d); b(op); rr(s, d); }
  void alu_imm(uint8_t ext, Reg d, int32_t imm) { rex(true, 0, d); b(0x81); rr(ext, d); d32(static_cast<uint32_t>(imm)); }   // /0 add, /5 sub, /7 cmp
  void alu_imm32(uint8_t ext, Reg d, int32_t imm) { rex(false, 0, d); b(0x81); rr(ext, d); d32(static_cast<uint32_t>(imm)); }
  void add(Reg d, Reg s) { alu(0x01, d, s); }
  void sub(Reg d, Reg s) { alu(0x29, d, s); }
  void cmp(Reg d, Reg s) { alu(0x39, d, s); }
  void test(Reg d, Reg s) { alu(0x85, d, s); }
  void test32(Reg d, Reg s) { alu32(0x85, d, s); }
  void add_imm(Reg d, int32_t imm) { alu_imm(0, d, imm); }
  void sub_imm(Reg d, int32_t imm) { alu_imm(5, d, imm); }
  void cmp_imm(Reg d, int32_t imm) { alu_imm(7, d, imm); }
  void cmp_imm32(Reg d, int32_t imm) { alu_imm32(7, d, imm); }
  void imul(Reg d, Reg s) { rex(true, d, s); b(0x0F); b(0xAF); rr(d, s); }
  void cqo() { b(0x48); b(0x99); }
  void idiv(Reg s) { rex(true, 0, s); b(0xF7); rr(7, s); }
  void neg(Reg d) { rex(true, 0, d); b(0xF7); rr(3, d); }
  void not_(Reg d) { rex(true, 0, d); b(0xF7); rr(2, d); }
  void shl_cl(Reg d) { rex(true, 0, d); b(0xD3); rr(4, d); }
  void sar_cl(Reg d) { rex(true, 0, d); b(0xD3); rr(7, d); }
  void shl_imm(Reg d, uint8_t n) { rex(true, 0, d); b(0xC1); rr(4, d); b(n); }
  void shr_imm(Reg d, uint8_t n) { rex(true, 0, d); b(0xC1); rr(5, d); b(n); }
  void setcc_movzx(Cc c, Reg d) {   // d32 = condition (d below r8 or above: setcc into d's low byte, then movzx)
    rex(false, 0, d, d >= RSP && d <= RDI); b(0x0F); b(static_cast<uint8_t>(0x90 | c)); rr(0, d);
    rex(false, d, d, d >= RSP && d <= RDI); b(0x0F); b(0xB6); rr(d, d);
  }
  void xor32_imm8(Reg d, uint8_t imm) { rex(false, 0, d); b(0x83); rr(6, d); b(imm); }
  void zero32(Reg d) { alu32(0x31, d, d); }
  void call(Reg r) { rex(false, 0, r); b(0xFF); rr(2, r); }
  void push(Reg r) { rex(false, 0, r); b(static_cast<uint8_t>(0x50 + (r & 7))); }
  void pop(Reg r) { rex(false, 0, r); b(static_cast<uint8_t>(0x58 + (r & 7))); }
  void ret() { b(0xC3); }
  // lock add [base+disp], 1 (32- or 64-bit)
  void lock_inc(Reg base, int32_t disp, bool wide) { b(0xF0); rex(wide, 0, base); b(0x83); mem(0, base, disp); b(1); }
  // lock cmpxchg [base+disp], s (compares with eax/rax)
  void lock_cmpxchg(Reg base, int32_t disp, Reg s, bool wide) { b(0xF0); rex(wide, s, base); b(0x0F); b(0xB1); mem(s, base, disp); }
  // Branches: rel32, patched once the target is known. The returned position is that of the rel32 field.
  size_t jmp() { b(0xE9); d32(0); return here() - 4; }
  size_t jcc(Cc c) { b(0x0F); b(static_cast<uint8_t>(0x80 | c)); d32(0); return here() - 4; }
  void patch(size_t at, size_t target) {
    const int32_t rel = static_cast<int32_t>(static_cast<int64_t>(target) - static_cast<int64_t>(at + 4));
    std::memcpy(buf.data() + at, &rel, 4);
  }
};

// ---- bytecode shape (as grain_jit_arm64.cpp) -----------------------------------
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
          fixups_.push_back({a_.jmp(), t});
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
        case Op::SetGlobal: emit_set_global(ip); break;
        case Op::Return: emit_return(ip); break;
        case Op::Halt:
          step(ip);
          fixups_ok_.push_back(a_.jmp());
          break;
        default:
          step(ip);   // one instruction, the interpreter's semantics
          break;
      }
    }
    label_[code_.size()] = a_.here();
    step(code_.size() ? starts.back() : 0);
    fixups_fail_.push_back(a_.jmp());

    const size_t ok_at = a_.here();
    a_.mov_imm64(RAX, 1);
    const size_t join = a_.jmp();
    const size_t fail_at = a_.here();
    a_.zero32(RAX);
    a_.patch(join, a_.here());
    epilogue();

    for (auto& f : fixups_) a_.patch(f.at, label_[f.target_ip]);
    for (size_t at : fixups_ok_) a_.patch(at, ok_at);
    for (size_t at : fixups_fail_) a_.patch(at, fail_at);
    return true;
  }

  const std::vector<uint8_t>& bytes() const { return a_.buf; }

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
    a_.push(RBP); a_.push(RBX); a_.push(R12); a_.push(R13); a_.push(R14); a_.push(R15);
    a_.sub_imm(RSP, 8);   // 6 pushes after the return address: back to a 16-byte boundary for the calls
    a_.mov(RBX, RDI);
    a_.mov(R13, RSI);
    a_.load(RBP, RBX, 8);  // &stack
    reload();
  }
  void epilogue() {
    a_.add_imm(RSP, 8);
    a_.pop(R15); a_.pop(R14); a_.pop(R13); a_.pop(R12); a_.pop(RBX); a_.pop(RBP);
    a_.ret();
  }
  void reload() {
    a_.mov(R12, R13);
    a_.shl_imm(R12, 5);
    a_.load(RAX, RBP, 0);      // begin
    a_.add(R12, RAX);          // + base * 32
    a_.load(R14, RBP, 8);      // finish
    a_.load(R15, RBP, 16);     // end of storage
  }
  void step(size_t ip) { call_helper(ip, &grain_jit_x64_step); }
  void call_helper(size_t ip, int (*fn)(Ctx*, uint32_t)) {
    a_.store(RBP, 8, R14);     // hand the vector its real size
    a_.mov(RDI, RBX);
    a_.mov_imm64(RSI, ip);
    a_.mov_imm64(RAX, reinterpret_cast<uint64_t>(fn));
    a_.call(RAX);
    a_.test32(RAX, RAX);
    fixups_fail_.push_back(a_.jcc(E));
    reload();
  }
  template <class Fast>
  void fast_or_step(size_t ip, Fast&& fast) { fast_or_step_with(ip, &grain_jit_x64_step, fast); }
  template <class Fast>
  void fast_or_step_with(size_t ip, int (*helper)(Ctx*, uint32_t), Fast&& fast) {
    std::vector<size_t> slow;
    fast(slow);
    const size_t done = a_.jmp();
    const size_t slow_at = a_.here();
    for (size_t s : slow) a_.patch(s, slow_at);
    call_helper(ip, helper);
    a_.patch(done, a_.here());
  }

  // ++use_count of the control block in `ctrl` (non-null)
  void ref_inc(Reg ctrl) { const Layout& L = layout(); a_.lock_inc(ctrl, static_cast<int32_t>(L.sp_use_count), L.sp_wide); }
  // --use_count unless it is the last owner; returns the branch taken when it is (nothing changed). Uses rax, rcx.
  size_t count_dec_unless_last(Reg ctrl) {
    const Layout& L = layout();
    const int32_t off = static_cast<int32_t>(L.sp_use_count);
    const size_t again = a_.here();
    if (L.sp_wide) a_.load(RAX, ctrl, off); else a_.load32(RAX, ctrl, off);
    if (L.sp_wide) a_.cmp_imm(RAX, static_cast<int32_t>(L.sp_last)); else a_.cmp_imm32(RAX, static_cast<int32_t>(L.sp_last));
    const size_t last = a_.jcc(L.sp_wide ? LE : BE);
    if (L.sp_wide) a_.lea(RCX, RAX, -1);
    else { a_.rex(false, RCX, RAX); a_.b(0x8D); a_.mem(RCX, RAX, -1); }   // lea ecx, [rax-1]
    a_.lock_cmpxchg(ctrl, off, RCX, L.sp_wide);
    a_.patch(a_.jcc(NE), again);
    return last;
  }
  // --use_count, unless it is the last reference: then nothing changed and control goes to the slow path (the
  // destructor runs in C++).
  void ref_dec_or_slow(Reg ctrl, std::vector<size_t>& slow) { slow.push_back(count_dec_unless_last(ctrl)); }
  // Push a copy of the Value at [src] (any type): refcount first, then bits. src is not r9-r11 or rsi.
  void push_copy_from(Reg src, std::vector<size_t>& slow) {
    a_.cmp(R14, R15);
    slow.push_back(a_.jcc(AE));
    a_.load(RSI, src, kCtrlOff);
    a_.test(RSI, RSI);
    const size_t plain = a_.jcc(E);
    ref_inc(RSI);
    a_.patch(plain, a_.here());
    a_.load(R9, src, 0);
    a_.load(R10, src, kPayOff);
    a_.load(R11, src, 16);
    a_.store(R14, 0, R9);
    a_.store(R14, kPayOff, R10);
    a_.store(R14, 16, R11);
    a_.store(R14, kCtrlOff, RSI);
    a_.add_imm(R14, 32);
  }

  void push_imm(size_t ip, Type t, uint64_t bits) {
    fast_or_step(ip, [&](std::vector<size_t>& slow) {
      a_.cmp(R14, R15);
      slow.push_back(a_.jcc(AE));              // full: let the vector grow
      a_.mov_imm64(R8, static_cast<uint32_t>(t));
      a_.mov_imm64(R9, bits);
      a_.zero32(RAX);
      a_.store(R14, 0, R8);
      a_.store(R14, kPayOff, R9);
      a_.store(R14, 16, RAX);
      a_.store(R14, kCtrlOff, RAX);
      a_.add_imm(R14, 32);
    });
  }

  void emit_constant(size_t ip) {
    const Value& v = fn_.chunk.constants[rd16(ip + 1)];
    if (!v.obj && (v.type == Type::Int || v.type == Type::Real || v.type == Type::Bool || v.type == Type::Nil)) {
      push_imm(ip, v.type, static_cast<uint64_t>(v.i));
    } else {
      fast_or_step(ip, [&](std::vector<size_t>& slow) {
        a_.mov_imm64(R8, reinterpret_cast<uint64_t>(&v));   // the chunk's own Value, alive as long as this code
        push_copy_from(R8, slow);
      });
    }
  }

  void emit_pop(size_t ip) {
    fast_or_step(ip, [&](std::vector<size_t>& slow) {
      a_.load(R8, R14, -32 + kCtrlOff);
      a_.test(R8, R8);
      const size_t plain = a_.jcc(E);
      ref_dec_or_slow(R8, slow);
      a_.patch(plain, a_.here());
      a_.sub_imm(R14, 32);
    });
  }

  void local_addr(Reg d, size_t slot) { a_.lea(d, R12, static_cast<int32_t>(slot * 32)); }

  void emit_get_local(size_t ip) {
    fast_or_step(ip, [&](std::vector<size_t>& slow) {
      local_addr(R8, code_[ip + 1]);
      push_copy_from(R8, slow);
    });
  }

  // local = top (top stays): release what the local held (unless that is the last reference - then the
  // interpreter does it), retain the new value.
  void emit_set_local(size_t ip) {
    fast_or_step(ip, [&](std::vector<size_t>& slow) {
      local_addr(R8, code_[ip + 1]);
      assign_top_to_r8(slow);
    });
  }

  // SETGLOBAL: the same assignment, into the global's slot through the chunk's inline cache (filled by the VM on
  // first use; until then, and for a name never declared, the interpreter).
  void emit_set_global(size_t ip) {
    const uint32_t k = static_cast<uint32_t>(rd16(ip + 1));
    const Chunk& ch = fn_.chunk;
    const auto cache_off = static_cast<int32_t>(offsetof(Chunk, global_cache));
    const auto owner_off = static_cast<int32_t>(offsetof(Chunk, global_cache_owner));
    fast_or_step(ip, [&](std::vector<size_t>& slow) {
      a_.mov_imm64(R8, reinterpret_cast<uint64_t>(&ch));
      a_.load(R9, R8, owner_off);
      a_.load(R10, RBX, 48);                   // ctx->serial
      a_.cmp(R9, R10);
      slow.push_back(a_.jcc(NE));
      a_.load(R9, R8, cache_off);
      a_.load(R8, R9, static_cast<int32_t>(k * 8));
      a_.test(R8, R8);
      slow.push_back(a_.jcc(E));
      assign_top_to_r8(slow);
    });
  }

  // [r8] = top, top staying: the old value released (unless it is the last reference - then nothing has changed
  // and the slow path does it all), the new one retained.
  void assign_top_to_r8(std::vector<size_t>& slow) {
    {
      a_.load(RSI, R8, kCtrlOff);                // old
      a_.load(RDI, R14, -32 + kCtrlOff);         // new
      a_.cmp(RSI, RDI);
      const size_t same = a_.jcc(E);
      a_.test(RSI, RSI);
      const size_t old_plain = a_.jcc(E);
      ref_dec_or_slow(RSI, slow);
      a_.patch(old_plain, a_.here());
      a_.load(RDI, R14, -32 + kCtrlOff);
      a_.test(RDI, RDI);
      const size_t new_plain = a_.jcc(E);
      ref_inc(RDI);
      a_.patch(new_plain, a_.here());
      a_.patch(same, a_.here());
      a_.load(R9, R14, -32);
      a_.load(R10, R14, -32 + kPayOff);
      a_.load(R11, R14, -32 + 16);
      a_.load(RDI, R14, -32 + kCtrlOff);
      a_.store(R8, 0, R9);
      a_.store(R8, kPayOff, R10);
      a_.store(R8, 16, R11);
      a_.store(R8, kCtrlOff, RDI);
    }
  }

  // Both operands Int, their payloads in r10 (left) and r11 (right), or the slow path.
  void both_int(std::vector<size_t>& slow) {
    a_.loadb(R8, R14, -64);
    a_.cmp_imm32(R8, static_cast<int32_t>(Type::Int));
    slow.push_back(a_.jcc(NE));
    a_.loadb(R9, R14, -32);
    a_.cmp_imm32(R9, static_cast<int32_t>(Type::Int));
    slow.push_back(a_.jcc(NE));
    a_.load(R10, R14, -64 + kPayOff);
    a_.load(R11, R14, -32 + kPayOff);
  }

  void emit_arith(size_t ip, Op op) {
    fast_or_step(ip, [&](std::vector<size_t>& slow) {
      both_int(slow);
      if (op == Op::Add) a_.add(R10, R11);
      else if (op == Op::Subtract) a_.sub(R10, R11);
      else a_.imul(R10, R11);
      a_.store(R14, -64 + kPayOff, R10);
      a_.sub_imm(R14, 32);
    });
  }

  // Int % Int, C++ semantics (idiv truncates; the remainder is in rdx). A zero divisor, -1 (INT64_MIN % -1
  // faults on this machine) and every other type: the interpreter.
  void emit_modulo(size_t ip) {
    fast_or_step(ip, [&](std::vector<size_t>& slow) {
      both_int(slow);
      a_.test(R11, R11);
      slow.push_back(a_.jcc(E));
      a_.cmp_imm(R11, -1);
      slow.push_back(a_.jcc(E));
      a_.mov(RAX, R10);
      a_.cqo();
      a_.idiv(R11);
      a_.store(R14, -64 + kPayOff, RDX);
      a_.sub_imm(R14, 32);
    });
  }

  // Shifts only inline for counts 0..63: the machine takes the count mod 64, where grain shifts everything out
  // (and errors on a negative) - the interpreter handles those.
  void emit_bitop(size_t ip, Op op) {
    fast_or_step(ip, [&](std::vector<size_t>& slow) {
      both_int(slow);
      if (op == Op::Shl || op == Op::Shr) {
        a_.cmp_imm(R11, 63);
        slow.push_back(a_.jcc(A));               // unsigned: negative counts land here too
        a_.mov(RCX, R11);
        if (op == Op::Shl) a_.shl_cl(R10); else a_.sar_cl(R10);
      } else if (op == Op::BitAnd) a_.alu(0x21, R10, R11);
      else if (op == Op::BitOr) a_.alu(0x09, R10, R11);
      else a_.alu(0x31, R10, R11);
      a_.store(R14, -64 + kPayOff, R10);
      a_.sub_imm(R14, 32);
    });
  }

  void emit_bitnot(size_t ip) {
    fast_or_step(ip, [&](std::vector<size_t>& slow) {
      a_.loadb(R8, R14, -32);
      a_.cmp_imm32(R8, static_cast<int32_t>(Type::Int));
      slow.push_back(a_.jcc(NE));
      a_.load(R10, R14, -32 + kPayOff);
      a_.not_(R10);
      a_.store(R14, -32 + kPayOff, R10);
    });
  }

  void emit_compare(size_t ip, Op op) {
    fast_or_step(ip, [&](std::vector<size_t>& slow) {
      both_int(slow);
      a_.cmp(R10, R11);
      const Cc c = op == Op::Equal ? E : op == Op::NotEqual ? NE : op == Op::Less ? L
                 : op == Op::LessEqual ? LE : op == Op::Greater ? G : GE;
      a_.setcc_movzx(c, RSI);
      a_.mov_imm64(RDI, static_cast<uint32_t>(Type::Bool));
      a_.store(R14, -64, RDI);
      a_.store(R14, -64 + kPayOff, RSI);
      a_.sub_imm(R14, 32);
    });
  }

  // for-loop step: seq (a List) in local `slot`, cursor (Int) in slot+1. Pushes the next item and true, or false
  // when done - as the interpreter.
  void emit_iter_next(size_t ip) {
    const size_t slot = code_[ip + 1];
    const int32_t Lo = static_cast<int32_t>(list_items_offset());
    fast_or_step(ip, [&](std::vector<size_t>& slow) {
      local_addr(R8, slot);
      a_.loadb(R9, R8, kTypeOff);
      a_.cmp_imm32(R9, static_cast<int32_t>(Type::List));
      slow.push_back(a_.jcc(NE));
      a_.loadb(R9, R8, 32 + kTypeOff);
      a_.cmp_imm32(R9, static_cast<int32_t>(Type::Int));
      slow.push_back(a_.jcc(NE));
      a_.lea(R9, R14, 64);                     // room for two pushes
      a_.cmp(R9, R15);
      slow.push_back(a_.jcc(A));
      a_.load(R10, R8, 16);                    // ListObj*
      a_.load(R11, R10, Lo);                   // items begin
      a_.load(RSI, R10, Lo + 8);               // items end
      a_.load(RDI, R8, 32 + kPayOff);          // cursor
      a_.mov(RAX, RDI);
      a_.shl_imm(RAX, 5);
      a_.add(R11, RAX);                        // &items[cursor]
      a_.cmp(R11, RSI);
      const size_t more = a_.jcc(B);
      // done: push false
      a_.mov_imm64(R9, static_cast<uint32_t>(Type::Bool));
      a_.zero32(RAX);
      a_.store(R14, 0, R9);
      a_.store(R14, kPayOff, RAX);
      a_.store(R14, 16, RAX);
      a_.store(R14, kCtrlOff, RAX);
      a_.add_imm(R14, 32);
      const size_t out = a_.jmp();
      a_.patch(more, a_.here());
      a_.load(R9, R11, kCtrlOff);
      a_.test(R9, R9);
      slow.push_back(a_.jcc(NE));              // refcounted item: the interpreter copies it
      a_.load(R9, R11, 0);
      a_.load(R10, R11, kPayOff);
      a_.zero32(RAX);
      a_.store(R14, 0, R9);
      a_.store(R14, kPayOff, R10);
      a_.store(R14, 16, RAX);
      a_.store(R14, kCtrlOff, RAX);
      a_.add_imm(RDI, 1);
      a_.store(R8, 32 + kPayOff, RDI);
      a_.mov_imm64(R9, static_cast<uint32_t>(Type::Bool));
      a_.mov_imm64(R10, 1);
      a_.store(R14, 32, R9);
      a_.store(R14, 32 + kPayOff, R10);
      a_.store(R14, 32 + 16, RAX);
      a_.store(R14, 32 + kCtrlOff, RAX);
      a_.add_imm(R14, 64);
      a_.patch(out, a_.here());
    });
  }

  // GETGLOBAL through the chunk's inline cache (Chunk::global_cache, filled by the VM on first use): owner check,
  // slot, copy - with the refcount increment done here when the value is an object.
  void emit_get_global(size_t ip) {
    const uint32_t k = static_cast<uint32_t>(rd16(ip + 1));
    const Chunk& ch = fn_.chunk;
    const auto cache_off = static_cast<int32_t>(offsetof(Chunk, global_cache));
    const auto owner_off = static_cast<int32_t>(offsetof(Chunk, global_cache_owner));
    fast_or_step_with(ip, &grain_jit_x64_get_global, [&](std::vector<size_t>& slow) {
      a_.mov_imm64(R8, reinterpret_cast<uint64_t>(&ch));
      a_.load(R9, R8, owner_off);
      a_.load(R10, RBX, 48);                   // ctx->serial
      a_.cmp(R9, R10);
      slow.push_back(a_.jcc(NE));
      a_.load(R9, R8, cache_off);              // cache data (sized to the constants when owned)
      a_.load(R8, R9, static_cast<int32_t>(k * 8));
      a_.test(R8, R8);
      slow.push_back(a_.jcc(E));               // not resolved yet
      push_copy_from(R8, slow);
    });
  }

  // CALL: when the callee is a closure whose function already has native code and takes exactly argc arguments,
  // push its Frame here and call it. Everything else - natives, first calls, arity errors, the depth limit, bare
  // FnObjs - goes through Vm::jit_call, i.e. Vm::invoke.
  void emit_call(size_t ip) {
    const Layout& Ly = layout();
    const uint32_t argc = code_[ip + 1];
    const int32_t callee_off = -static_cast<int32_t>(32 * (argc + 1));
    fast_or_step_with(ip, &grain_jit_x64_call, [&](std::vector<size_t>& slow) {
      a_.lea(R8, R14, callee_off);
      a_.loadb(R9, R8, kTypeOff);
      a_.cmp_imm32(R9, static_cast<int32_t>(Type::Fn));
      slow.push_back(a_.jcc(NE));
      a_.load(R9, R8, 16);                     // ClosureObj* (or bare FnObj*)
      a_.loadb(R10, R9, static_cast<int32_t>(Ly.obj_is_closure));
      a_.test32(R10, R10);
      slow.push_back(a_.jcc(E));
      a_.load(R11, R9, static_cast<int32_t>(Ly.closure_fn));   // FnObj*
      a_.load(RSI, R11, static_cast<int32_t>(Ly.fn_jit));      // its native code
      a_.test(RSI, RSI);
      slow.push_back(a_.jcc(E));
      a_.load32(RDI, R11, static_cast<int32_t>(Ly.fn_arity));
      a_.cmp_imm32(RDI, static_cast<int32_t>(argc));
      slow.push_back(a_.jcc(NE));
      a_.load(RAX, RBX, 32);                   // &frames
      a_.load(RCX, RAX, 8);                    // finish
      a_.load(RDX, RAX, 16);                   // capacity end
      a_.cmp(RCX, RDX);
      slow.push_back(a_.jcc(AE));
      a_.load(RDX, RAX, 0);                    // begin
      a_.mov(RDI, RCX);
      a_.sub(RDI, RDX);
      a_.cmp_imm(RDI, static_cast<int32_t>(kMaxFrames * kFrameSize));
      slow.push_back(a_.jcc(AE));              // at the depth limit: the VM reports it
      // The caller's frame records where it is (traces name this call).
      a_.store_imm(RCX, -static_cast<int32_t>(kFrameSize) + static_cast<int32_t>(kFrameIp), static_cast<int32_t>(ip + 2));
      // The callee's Frame{closure, ip 0, base, owned {}, chunk}.
      a_.load(RDX, RBP, 0);
      a_.mov(RDI, R8);
      a_.sub(RDI, RDX);
      a_.shr_imm(RDI, 5);                      // base index
      a_.store(RCX, kFrameClosure, R9);
      a_.store_imm(RCX, kFrameIp, 0);
      a_.store(RCX, kFrameBase, RDI);
      a_.store_imm(RCX, kFrameOwnedPtr, 0);
      a_.store_imm(RCX, kFrameOwnedCtrl, 0);
      a_.lea(RDX, R11, static_cast<int32_t>(Ly.fn_chunk));
      a_.store(RCX, kFrameChunk, RDX);
      a_.add_imm(RCX, static_cast<int32_t>(kFrameSize));
      a_.store(RAX, 8, RCX);
      a_.store(RBP, 8, R14);                   // the callee sees the real stack
      a_.mov(R10, RSI);                        // the code, out of the argument registers' way
      a_.mov(RSI, RDI);
      a_.mov(RDI, RBX);
      a_.call(R10);
      a_.test32(RAX, RAX);
      fixups_fail_.push_back(a_.jcc(E));
      reload();
    });
  }

  // RETURN: when no open upvalue, no refcounted local and no owned closure needs tearing down, drop the frame
  // here: the result replaces the callee slot (whose closure reference is released unless it is the last).
  // Otherwise Vm::jit_return does it.
  void emit_return(size_t ip) {
    std::vector<size_t> slow;
    a_.load(R8, RBX, 40);                      // &open upvalues
    a_.load(R9, R8, 0);
    a_.load(R10, R8, 8);
    a_.cmp(R9, R10);
    slow.push_back(a_.jcc(NE));
    a_.load(RAX, RBX, 32);                     // &frames
    a_.load(RCX, RAX, 8);
    a_.load(R9, RCX, -static_cast<int32_t>(kFrameSize) + static_cast<int32_t>(kFrameOwnedCtrl));
    a_.test(R9, R9);
    slow.push_back(a_.jcc(NE));
    // locals/args between the callee slot and the result: none refcounted
    a_.lea(R8, R12, 32);
    a_.lea(R11, R14, -32);                     // the result
    const size_t loop = a_.here();
    a_.cmp(R8, R11);
    const size_t scanned = a_.jcc(AE);
    a_.load(R9, R8, kCtrlOff);
    a_.test(R9, R9);
    slow.push_back(a_.jcc(NE));
    a_.add_imm(R8, 32);
    a_.patch(a_.jmp(), loop);
    a_.patch(scanned, a_.here());
    // release the callee slot's reference (never the last one - that needs the destructor: the VM)
    a_.load(RSI, R12, kCtrlOff);
    a_.test(RSI, RSI);
    const size_t no_ref = a_.jcc(E);
    slow.push_back(count_dec_unless_last(RSI));
    a_.patch(no_ref, a_.here());
    // result -> callee slot; pop the frame
    a_.load(R9, R11, 0);
    a_.load(R10, R11, kPayOff);
    a_.load(RDX, R11, 16);
    a_.load(RDI, R11, kCtrlOff);
    a_.store(R12, 0, R9);
    a_.store(R12, kPayOff, R10);
    a_.store(R12, 16, RDX);
    a_.store(R12, kCtrlOff, RDI);
    a_.lea(R14, R12, 32);
    a_.store(RBP, 8, R14);
    a_.load(RAX, RBX, 32);
    a_.load(RCX, RAX, 8);
    a_.sub_imm(RCX, static_cast<int32_t>(kFrameSize));
    a_.store(RAX, 8, RCX);
    fixups_ok_.push_back(a_.jmp());
    const size_t slow_at = a_.here();
    for (size_t s : slow) a_.patch(s, slow_at);
    call_helper(ip, &grain_jit_x64_return);
    fixups_ok_.push_back(a_.jmp());
  }

  void emit_negate(size_t ip) {
    fast_or_step(ip, [&](std::vector<size_t>& slow) {
      a_.loadb(R8, R14, -32);
      a_.cmp_imm32(R8, static_cast<int32_t>(Type::Int));
      slow.push_back(a_.jcc(NE));
      a_.load(R10, R14, -32 + kPayOff);
      a_.neg(R10);
      a_.store(R14, -32 + kPayOff, R10);
    });
  }

  // Falsy = nil, or a bool whose payload is 0. Leaves 1 in esi when truthy.
  void truthy_to_esi() {
    a_.loadb(R8, R14, -32);
    a_.mov_imm64(RSI, 1);
    a_.test32(R8, R8);
    const size_t not_nil = a_.jcc(NE);
    a_.zero32(RSI);
    const size_t done1 = a_.jmp();
    a_.patch(not_nil, a_.here());
    a_.cmp_imm32(R8, static_cast<int32_t>(Type::Bool));
    const size_t not_bool = a_.jcc(NE);
    a_.loadb(RSI, R14, -32 + kPayOff);
    a_.patch(not_bool, a_.here());
    a_.patch(done1, a_.here());
  }

  void emit_not(size_t ip) {
    fast_or_step(ip, [&](std::vector<size_t>& slow) {
      a_.load(R9, R14, -32 + kCtrlOff);
      a_.test(R9, R9);
      slow.push_back(a_.jcc(NE));
      truthy_to_esi();
      a_.xor32_imm8(RSI, 1);
      a_.mov_imm64(RDI, static_cast<uint32_t>(Type::Bool));
      a_.store(R14, -32, RDI);
      a_.store(R14, -32 + kPayOff, RSI);
    });
  }

  // JUMPF/JUMPT leave the condition on the stack (the compiler pops it on both paths), so this only reads it.
  void emit_cond_jump(bool if_false, size_t target) {
    truthy_to_esi();
    a_.test32(RSI, RSI);
    fixups_.push_back({a_.jcc(if_false ? E : NE), target});
  }

  // Back-edge: count the loop body's instructions against the step limit (a runaway script must still stop),
  // then branch.
  void emit_loop(size_t ip, size_t target, size_t body) {
    a_.load(R8, RBX, 16);                      // &steps
    a_.load(R9, R8, 0);
    a_.add_imm(R9, static_cast<int32_t>(body));
    a_.store(R8, 0, R9);
    a_.load(R10, RBX, 24);                     // &step_limit
    a_.load(R10, R10, 0);
    a_.test(R10, R10);
    const size_t unlimited = a_.jcc(E);
    a_.cmp(R9, R10);
    const size_t within = a_.jcc(BE);
    step(ip);                                  // over the limit: the interpreter reports it
    a_.patch(unlimited, a_.here());
    a_.patch(within, a_.here());
    fixups_.push_back({a_.jmp(), target});
  }
};

void* alloc_exec(size_t bytes) {
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return p == MAP_FAILED ? nullptr : p;
}

}  // namespace

Entry compile(const FnObj& fn) {
  if (std::getenv("GRAIN_NOJIT")) return nullptr;
  if (!vector_layout_ok() || !layout().ok) return nullptr;
  Compiler c(fn);
  if (!c.build()) return nullptr;
  const auto& code = c.bytes();
  // The allocation's size in front of the code, for release().
  auto* mem = static_cast<uint8_t*>(alloc_exec(code.size() + 16));
  if (!mem) return nullptr;
  *reinterpret_cast<size_t*>(mem) = code.size() + 16;
  std::memcpy(mem + 16, code.data(), code.size());
  return reinterpret_cast<Entry>(mem + 16);
}

void release(void* code) {
  if (!code) return;
  uint8_t* mem = static_cast<uint8_t*>(code) - 16;
  munmap(mem, *reinterpret_cast<size_t*>(mem));
}

#endif

}  // namespace aios::userland::grain::jit
