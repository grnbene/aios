// Native code for a lane program (docs/asql-vectorized-plan.md, stage 3a). The interpreter (vp::eval) runs each
// instruction as a pass over all rows of a chunk, allocating that instruction's lanes: K passes and K allocations
// a chunk. Here the whole program runs as one loop over the chunk's rows - each non-leaf instruction's value in a
// stack slot, the leaves (columns decoded by the interpreter's own vectorized readers, constants) read from their
// lanes - and only the output lane is written. The generated code keeps the interpreter's rules exactly: NULL
// rules per operator, integer overflow and a non-finite double bail out of the chunk (return 1) as the
// interpreter's *err does, and the interpreter then runs the chunk and reports as it always has; the libm
// functions and the casts the interpreter calls are called, not re-derived. A program with an instruction this
// does not cover (text, tuple IN, exact-decimal arithmetic) has no native code and runs as before.
//
// Checked differentially: with AIOS_SQL_VJIT_CHECK=1 every chunk is computed both ways and compared bit for bit
// (a mismatch aborts, naming the program); AIOS_SQL_VJIT=0 turns native code off. x86-64 SysV only; the AArch64
// emitter follows the same plan.
namespace vp {
namespace vjit {

inline bool enabled() {
  static const bool on = [] { const char* v = std::getenv("AIOS_SQL_VJIT"); return !(v && *v == '0'); }();
  return on;
}
inline std::atomic<uint64_t>& native_runs() { static std::atomic<uint64_t> n{0}; return n; }   // chunks computed natively
inline bool check_mode() {
  static const bool on = [] { const char* v = std::getenv("AIOS_SQL_VJIT_CHECK"); return v && *v == '1'; }();
  return on;
}

// What the generated function is handed: the leaves' lanes (by instruction index; a pointer per index, null
// where an index is not a leaf of that kind) and the output lane.
struct Env {
  size_t n;                       // +0
  const void* const* lane_i;      // +8   int64 lanes (I64 leaves), or the text references of a ColS leaf
  const void* const* lane_f;      // +16  double lanes (F64 leaves)
  const void* const* lane_n;      // +24  null bytes
  int64_t* out_i;                 // +32
  double* out_f;                  // +40
  uint8_t* out_n;                 // +48
};
using Fn = int (*)(const Env*);   // 0: done; 1: bail - the interpreter decides the chunk

// The helpers the code calls, in the interpreter's own terms.
extern "C" {
inline double vjit_floor(double a) { return std::floor(a); }
inline double vjit_ceil(double a) { return std::ceil(a); }
inline double vjit_fmod(double a, double b) { return std::fmod(a, b); }
inline double vjit_sin(double a) { return std::sin(a); }
inline double vjit_cos(double a) { return std::cos(a); }
inline double vjit_tan(double a) { return std::tan(a); }
inline double vjit_atan(double a) { return std::atan(a); }
inline double vjit_round(double a) { return std::round(a); }
inline double vjit_u64_to_double(int64_t bits) { return (double)(uint64_t)bits; }
// A double's bits as the bit operators read them (vp::eval's u()).
inline int64_t vjit_bits_of_double(double d) {
  d = std::round(d);
  if (d <= -9223372036854775808.0) return (int64_t)((uint64_t)1 << 63);
  if (d >= 18446744073709551615.0) return (int64_t)~(uint64_t)0;
  return d < 0 ? (int64_t)d : (int64_t)(uint64_t)d;
}
// CAST(double AS SIGNED) as the interpreter rounds it (vp::eval's CastI on a double lane); 1: bail.
inline int vjit_cast_i(double a, int64_t approx, int64_t* out) {
  if (!(std::fabs(a) < 9.2e18)) return 1;
  if (!approx) { *out = (int64_t)std::llround(a); return 0; }
  const double fl = std::floor(a);
  const double frac = a - fl;
  const double tol = std::fabs(a) * 1e-13 + 1e-13;
  if (frac == 0) *out = (int64_t)a;
  else if (std::fabs(frac - 0.5) > tol) *out = (int64_t)std::round(a);
  else { Val r; *out = round_double_as_written(a, 0, &r) ? (int64_t)std::llround(r.num) : (int64_t)std::nearbyint(a); }
  return 0;
}
}

enum Reg : uint8_t { RAX = 0, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
enum Cc : uint8_t { CcO = 0, CcB = 2, CcAE = 3, CcE = 4, CcNE = 5, CcBE = 6, CcA = 7, CcS = 8, CcNS = 9, CcP = 0xA, CcNP = 0xB, CcL = 0xC, CcGE = 0xD, CcLE = 0xE, CcG = 0xF };

struct Asm {
  std::vector<uint8_t> buf;
  size_t here() const { return buf.size(); }
  void b(uint8_t v) { buf.push_back(v); }
  void d32(uint32_t v) { for (int i = 0; i < 4; ++i) b((uint8_t)(v >> (8 * i))); }
  void d64(uint64_t v) { for (int i = 0; i < 8; ++i) b((uint8_t)(v >> (8 * i))); }
  void rex(bool w, uint8_t reg, uint8_t idx, uint8_t base, bool force = false) {
    const uint8_t r = (uint8_t)(0x40 | (w ? 8 : 0) | ((reg & 8) ? 4 : 0) | ((idx & 8) ? 2 : 0) | ((base & 8) ? 1 : 0));
    if (r != 0x40 || force) b(r);
  }
  void modrm_disp(uint8_t reg, uint8_t base, int32_t disp) {   // [base + disp32]
    b((uint8_t)(0x80 | ((reg & 7) << 3) | (base & 7)));
    if ((base & 7) == 4) b(0x24);
    d32((uint32_t)disp);
  }
  void modrm_sib8(uint8_t reg, uint8_t base, uint8_t idx) {    // [base + idx*8]
    b((uint8_t)(0x04 | ((reg & 7) << 3)));
    b((uint8_t)(0xC0 | ((idx & 7) << 3) | (base & 7)));
  }
  void modrm_sib1(uint8_t reg, uint8_t base, uint8_t idx) {    // [base + idx]
    b((uint8_t)(0x04 | ((reg & 7) << 3)));
    b((uint8_t)(((idx & 7) << 3) | (base & 7)));
  }
  void rr(uint8_t reg, uint8_t rm) { b((uint8_t)(0xC0 | ((reg & 7) << 3) | (rm & 7))); }
  // integers
  void mov(Reg d, Reg s) { rex(true, s, 0, d); b(0x89); rr(s, d); }
  void mov_imm(Reg d, uint64_t v) {
    if (v <= 0xFFFFFFFFull) { rex(false, 0, 0, d); b((uint8_t)(0xB8 + (d & 7))); d32((uint32_t)v); }
    else { rex(true, 0, 0, d); b((uint8_t)(0xB8 + (d & 7))); d64(v); }
  }
  void load(Reg d, Reg base, int32_t disp) { rex(true, d, 0, base); b(0x8B); modrm_disp(d, base, disp); }
  void store(Reg base, int32_t disp, Reg s) { rex(true, s, 0, base); b(0x89); modrm_disp(s, base, disp); }
  void store_imm32(Reg base, int32_t disp, int32_t imm) { rex(true, 0, 0, base); b(0xC7); modrm_disp(0, base, disp); d32((uint32_t)imm); }
  void load_idx8(Reg d, Reg base, Reg idx) { rex(true, d, idx, base); b(0x8B); modrm_sib8(d, base, idx); }          // mov d, [base+idx*8]
  void store_idx8(Reg base, Reg idx, Reg s) { rex(true, s, idx, base); b(0x89); modrm_sib8(s, base, idx); }         // mov [base+idx*8], s
  void loadb_idx(Reg d, Reg base, Reg idx) { rex(false, d, idx, base); b(0x0F); b(0xB6); modrm_sib1(d, base, idx); }   // movzx d32, byte [base+idx]
  void storeb_idx(Reg base, Reg idx, Reg s) { rex(false, s, idx, base, (s & 7) >= 4); b(0x88); modrm_sib1(s, base, idx); }   // mov byte [base+idx], s8
  void alu(uint8_t op, Reg d, Reg s) { rex(true, s, 0, d); b(op); rr(s, d); }
  void add(Reg d, Reg s) { alu(0x01, d, s); }
  void sub(Reg d, Reg s) { alu(0x29, d, s); }
  void and_(Reg d, Reg s) { alu(0x21, d, s); }
  void or_(Reg d, Reg s) { alu(0x09, d, s); }
  void xor_(Reg d, Reg s) { alu(0x31, d, s); }
  void cmp(Reg d, Reg s) { alu(0x39, d, s); }
  void test(Reg d, Reg s) { alu(0x85, d, s); }
  void imul(Reg d, Reg s) { rex(true, d, 0, s); b(0x0F); b(0xAF); rr(d, s); }
  void neg(Reg d) { rex(true, 0, 0, d); b(0xF7); rr(3, d); }
  void not_(Reg d) { rex(true, 0, 0, d); b(0xF7); rr(2, d); }
  void cqo() { b(0x48); b(0x99); }
  void idiv(Reg s) { rex(true, 0, 0, s); b(0xF7); rr(7, s); }
  void shl_cl(Reg d) { rex(true, 0, 0, d); b(0xD3); rr(4, d); }
  void shr_cl(Reg d) { rex(true, 0, 0, d); b(0xD3); rr(5, d); }
  void shl_imm(Reg d, uint8_t n) { rex(true, 0, 0, d); b(0xC1); rr(4, d); b(n); }
  void btc63(Reg d) { rex(true, 0, 0, d); b(0x0F); b(0xBA); rr(7, d); b(63); }   // flip the sign bit
  void btr63(Reg d) { rex(true, 0, 0, d); b(0x0F); b(0xBA); rr(6, d); b(63); }   // clear the sign bit
  void cmp_imm32(Reg d, int32_t imm) { rex(true, 0, 0, d); b(0x81); rr(7, d); d32((uint32_t)imm); }
  void setcc(Cc c, Reg d) { rex(false, 0, 0, d, (d & 7) >= 4); b(0x0F); b((uint8_t)(0x90 | c)); rr(0, d); }
  void movzx_b(Reg d, Reg s) { rex(true, d, 0, s); b(0x0F); b(0xB6); rr(d, s); }
  void cmov(Cc c, Reg d, Reg s) { rex(true, d, 0, s); b(0x0F); b((uint8_t)(0x40 | c)); rr(d, s); }
  void inc(Reg d) { rex(true, 0, 0, d); b(0xFF); rr(0, d); }
  void push(Reg r) { rex(false, 0, 0, r); b((uint8_t)(0x50 + (r & 7))); }
  void pop(Reg r) { rex(false, 0, 0, r); b((uint8_t)(0x58 + (r & 7))); }
  void call(Reg r) { rex(false, 0, 0, r); b(0xFF); rr(2, r); }
  void ret() { b(0xC3); }
  void lea(Reg d, Reg base, int32_t disp) { rex(true, d, 0, base); b(0x8D); modrm_disp(d, base, disp); }
  void sub_rsp(int32_t n) { b(0x48); b(0x81); b(0xEC); d32((uint32_t)n); }
  void add_rsp(int32_t n) { b(0x48); b(0x81); b(0xC4); d32((uint32_t)n); }
  // SSE2 doubles (xmm0..xmm15 numbered as Reg)
  void sse(uint8_t pfx, uint8_t op, Reg x, Reg rm, bool w = false) { if (pfx) b(pfx); rex(w, x, 0, rm); b(0x0F); b(op); rr(x, rm); }
  void movsd(Reg x, Reg y) { sse(0xF2, 0x10, x, y); }
  void movsd_load(Reg x, Reg base, int32_t disp) { b(0xF2); rex(false, x, 0, base); b(0x0F); b(0x10); modrm_disp(x, base, disp); }
  void movsd_store(Reg base, int32_t disp, Reg x) { b(0xF2); rex(false, x, 0, base); b(0x0F); b(0x11); modrm_disp(x, base, disp); }
  void movsd_load_idx8(Reg x, Reg base, Reg idx) { b(0xF2); rex(false, x, idx, base); b(0x0F); b(0x10); modrm_sib8(x, base, idx); }
  void movq_xr(Reg x, Reg r) { b(0x66); rex(true, x, 0, r); b(0x0F); b(0x6E); rr(x, r); }   // movq xmm, r64
  void movq_rx(Reg r, Reg x) { b(0x66); rex(true, x, 0, r); b(0x0F); b(0x7E); rr(x, r); }   // movq r64, xmm
  void addsd(Reg x, Reg y) { sse(0xF2, 0x58, x, y); }
  void subsd(Reg x, Reg y) { sse(0xF2, 0x5C, x, y); }
  void mulsd(Reg x, Reg y) { sse(0xF2, 0x59, x, y); }
  void divsd(Reg x, Reg y) { sse(0xF2, 0x5E, x, y); }
  void sqrtsd(Reg x, Reg y) { sse(0xF2, 0x51, x, y); }
  void ucomisd(Reg x, Reg y) { sse(0x66, 0x2E, x, y); }
  void cvtsi2sd(Reg x, Reg r) { sse(0xF2, 0x2A, x, r, true); }     // xmm = (double)r64
  void cvttsd2si(Reg r, Reg x) { sse(0xF2, 0x2C, r, x, true); }    // r64 = (int64)trunc(xmm)
  void xorpd(Reg x, Reg y) { sse(0x66, 0x57, x, y); }
  // control
  size_t jmp() { b(0xE9); d32(0); return here() - 4; }
  size_t jcc(Cc c) { b(0x0F); b((uint8_t)(0x80 | c)); d32(0); return here() - 4; }
  void patch(size_t at, size_t target) { const int32_t rel = (int32_t)((int64_t)target - (int64_t)(at + 4)); std::memcpy(buf.data() + at, &rel, 4); }
  void patch_here(size_t at) { patch(at, here()); }
};

inline bool is_leaf(Ins::Op op) { return vjit_leaf(op); }

struct Code {
  uint8_t* mem = nullptr;
  size_t bytes = 0;
  Fn fn = nullptr;
  ~Code() { if (mem) munmap(mem, bytes); }
};

// Builds the function for P; null where an instruction is not covered (why says which).
class Builder {
 public:
  explicit Builder(const Prog& P) : P_(P) {}
  std::shared_ptr<Code> build(std::string* why) {
    const size_t K = P_.ins.size();
    if (K < 2 || K > 256) { if (why) *why = "program size"; return nullptr; }
    for (size_t x = 0; x < K; ++x) if (!covered(x, why)) return nullptr;
    frame_ = (int32_t)(16 * K);
    // rsp after the six pushes and the return address is 8 mod 16; the frame makes it 0 mod 16 for the calls
    if ((frame_ + 8) % 16 != 0) frame_ += 8;
    a_.push(RBX); a_.push(RBP); a_.push(R12); a_.push(R13); a_.push(R14); a_.push(R15);
    a_.sub_rsp(frame_);
    a_.mov(RBX, RDI);                 // env
    a_.load(R13, RBX, 0);             // n
    a_.load(R14, RBX, 32 + (P_.out().ty == Ty::F64 ? 8 : 0));   // out_i or out_f
    a_.load(R15, RBX, 48);            // out_n
    // the leaves' lane pointers into their slots
    for (size_t x = 0; x < K; ++x) {
      if (!is_leaf(P_.ins[x].op)) continue;
      a_.load(RAX, RBX, P_.ins[x].ty == Ty::F64 ? 16 : 8);
      a_.load(RAX, RAX, (int32_t)(8 * x));
      a_.store(RSP, slot_v(x), RAX);
      a_.load(RAX, RBX, 24);
      a_.load(RAX, RAX, (int32_t)(8 * x));
      a_.store(RSP, slot_n(x), RAX);
    }
    a_.mov_imm(R12, 0);
    a_.test(R13, R13);
    const size_t empty = a_.jcc(CcE);
    const size_t loop = a_.here();
    for (size_t x = 0; x < K; ++x) if (!is_leaf(P_.ins[x].op)) emit(x);
    // the output lane
    const size_t K1 = K - 1;
    if (is_leaf(P_.ins[K1].op)) { if (why) *why = "a leaf as the result"; return nullptr; }
    a_.load(RAX, RSP, slot_v(K1));
    a_.store_idx8(R14, R12, RAX);
    a_.load(RAX, RSP, slot_n(K1));
    a_.storeb_idx(R15, R12, RAX);
    a_.inc(R12);
    a_.cmp(R12, R13);
    const size_t back = a_.jcc(CcB);
    a_.patch(back, loop);
    a_.patch_here(empty);
    a_.mov_imm(RAX, 0);
    epilogue();
    const size_t bail = a_.here();
    for (size_t at : bails_) a_.patch(at, bail);
    a_.mov_imm(RAX, 1);
    epilogue();
    auto code = std::make_shared<Code>();
    code->bytes = a_.buf.size();
    void* p = mmap(nullptr, code->bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { if (why) *why = "mmap"; return nullptr; }
    std::memcpy(p, a_.buf.data(), code->bytes);
    if (mprotect(p, code->bytes, PROT_READ | PROT_EXEC) != 0) { munmap(p, code->bytes); if (why) *why = "mprotect"; return nullptr; }
    code->mem = (uint8_t*)p;
    code->fn = reinterpret_cast<Fn>(p);
    return code;
  }

 private:
  const Prog& P_;
  Asm a_;
  int32_t frame_ = 0;
  std::vector<size_t> bails_;
  static constexpr Reg X0 = RAX, X1 = RCX, X2 = RDX, X3 = RBX, X4 = RSP;   // xmm0..xmm4 as Reg numbers
  int32_t slot_v(size_t x) const { return (int32_t)(16 * x); }
  int32_t slot_n(size_t x) const { return (int32_t)(16 * x + 8); }
  const Ins& at(size_t x) const { return P_.ins[x]; }
  Ty ty(size_t x) const { return P_.ins[x].ty; }
  void bail_if(Cc c) { bails_.push_back(a_.jcc(c)); }
  void epilogue() {
    a_.add_rsp(frame_);
    a_.pop(R15); a_.pop(R14); a_.pop(R13); a_.pop(R12); a_.pop(RBP); a_.pop(RBX);
    a_.ret();
  }

  bool covered(size_t x, std::string* why) {
    const Ins& in = at(x);
    auto no = [&](const char* w) { if (why) *why = std::string(w) + " (" + std::to_string((int)in.op) + ")"; return false; };
    if (is_leaf(in.op)) return true;
    for (uint32_t a : in.args) if (a >= x) return no("operand order");
    if (in.ty == Ty::Str) return no("text result");
    for (uint32_t a : in.args) if (ty(a) == Ty::Str) return no("text operand");
    if (in.exact_dyadic) return no("exact decimal arithmetic");
    switch (in.op) {
      case Ins::Neg: case Ins::Add: case Ins::Sub: case Ins::Mul: case Ins::Div: case Ins::Mod: case Ins::IDiv:
      case Ins::Eq: case Ins::Ne: case Ins::Lt: case Ins::Le: case Ins::Gt: case Ins::Ge:
      case Ins::And: case Ins::Or: case Ins::Not: case Ins::IsNull: case Ins::NotNull:
      case Ins::Case: case Ins::Coalesce: case Ins::Greatest: case Ins::Least:
      case Ins::Abs: case Ins::Floor: case Ins::Ceil: case Ins::Sqrt: case Ins::Sin: case Ins::Cos: case Ins::Tan: case Ins::Atan:
      case Ins::Radians: case Ins::Degrees: case Ins::CastI: case Ins::CastF:
      case Ins::BitAnd: case Ins::BitOr: case Ins::BitXor: case Ins::Shl: case Ins::Shr: case Ins::BitNot: case Ins::RoundI:
        break;
      case Ins::NullIf:
        if (in.ty != ty(in.args[0])) return no("NULLIF of mixed kinds");
        break;
      default:
        return no("operator");
    }
    if (in.op == Ins::Case && in.args.size() < 2) return no("CASE arity");
    return true;
  }

  // Operand loads. Value into a GPR (I64 lane) or xmm (F64 lane); the null byte into a GPR (0/1).
  void ld_i(size_t a, Reg d) {
    if (is_leaf(at(a).op)) { a_.load(d, RSP, slot_v(a)); a_.load_idx8(d, d, R12); }
    else a_.load(d, RSP, slot_v(a));
  }
  void ld_f(size_t a, Reg x, Reg scratch) {
    if (is_leaf(at(a).op)) { a_.load(scratch, RSP, slot_v(a)); a_.movsd_load_idx8(x, scratch, R12); }
    else a_.movsd_load(x, RSP, slot_v(a));
  }
  void ld_n(size_t a, Reg d) {
    if (is_leaf(at(a).op)) { a_.load(d, RSP, slot_n(a)); a_.loadb_idx(d, d, R12); }
    else a_.load(d, RSP, slot_n(a));
  }
  // operand a as a double in x (vp::eval's fd)
  void fd(size_t a, Reg x, Reg scratch) {
    if (ty(a) == Ty::I64) { ld_i(a, scratch); a_.cvtsi2sd(x, scratch); }
    else ld_f(a, x, scratch);
  }
  void set_null(size_t x) { a_.store_imm32(RSP, slot_n(x), 1); }
  void set_not_null(size_t x) { a_.store_imm32(RSP, slot_n(x), 0); }
  void store_i(size_t x, Reg s) { a_.store(RSP, slot_v(x), s); }
  void store_f(size_t x, Reg xmm) { a_.movsd_store(RSP, slot_v(x), xmm); }
  void load_const_f(Reg x, double d, Reg scratch) { uint64_t bits; std::memcpy(&bits, &d, 8); a_.mov_imm(scratch, bits); a_.movq_xr(x, scratch); }
  void call_fn(const void* fn) { a_.mov_imm(RAX, (uint64_t)(uintptr_t)fn); a_.call(RAX); }
  // the double in xmm0 non-finite: bail (the interpreter's "DOUBLE value is out of range")
  void bail_if_nonfinite_x0() {
    a_.movq_rx(RAX, X0);
    a_.shl_imm(RAX, 1);
    a_.mov_imm(RCX, 0xFFE0000000000000ull);
    a_.cmp(RAX, RCX);
    bail_if(CcAE);
  }
  // rax = 1 where the operand is true (a number other than 0; a NaN is), 0 otherwise - the operand loaded already:
  // an I64 in rax, an F64 in xmm0
  void truth_of_loaded(size_t a) {
    if (ty(a) == Ty::I64) { a_.test(RAX, RAX); a_.setcc(CcNE, RAX); a_.movzx_b(RAX, RAX); }
    else {
      a_.xorpd(X1, X1);
      a_.ucomisd(X0, X1);
      a_.setcc(CcNE, RAX);   // ZF=0: not equal (ordered); for a NaN ZF=1 and PF=1
      a_.setcc(CcP, RCX);
      a_.movzx_b(RAX, RAX); a_.movzx_b(RCX, RCX);
      a_.or_(RAX, RCX);
    }
  }
  void load_truth(size_t a) {   // rax = truth of operand a (its null not looked at)
    if (ty(a) == Ty::I64) ld_i(a, RAX); else ld_f(a, X0, RCX);
    truth_of_loaded(a);
  }
  // null rule for one or two operands: dest null if any is; jumps to `done` after setting it
  size_t null_any(size_t x, std::initializer_list<size_t> ops) {
    bool first = true;
    for (size_t a : ops) {
      ld_n(a, first ? RAX : RCX);
      if (!first) a_.or_(RAX, RCX);
      first = false;
    }
    a_.test(RAX, RAX);
    const size_t not_null = a_.jcc(CcE);
    set_null(x);
    const size_t done = a_.jmp();
    a_.patch_here(not_null);
    return done;
  }

  void emit(size_t x) {
    const Ins& in = at(x);
    std::vector<size_t> done;
    switch (in.op) {
      case Ins::Neg: {
        done.push_back(null_any(x, {in.args[0]}));
        if (in.ty == Ty::I64) {
          ld_i(in.args[0], RAX);
          a_.mov_imm(RCX, 0x8000000000000000ull);
          a_.cmp(RAX, RCX);
          bail_if(CcE);
          a_.neg(RAX);
          store_i(x, RAX);
        } else {
          ld_f(in.args[0], X0, RCX);
          a_.movq_rx(RAX, X0);
          a_.btc63(RAX);
          store_i(x, RAX);
        }
        set_not_null(x);
        break;
      }
      case Ins::Add: case Ins::Sub: case Ins::Mul: {
        done.push_back(null_any(x, {in.args[0], in.args[1]}));
        if (in.ty == Ty::I64) {
          ld_i(in.args[0], RAX);
          ld_i(in.args[1], RCX);
          if (in.op == Ins::Add) a_.add(RAX, RCX); else if (in.op == Ins::Sub) a_.sub(RAX, RCX); else a_.imul(RAX, RCX);
          bail_if(CcO);
          store_i(x, RAX);
        } else {
          fd(in.args[0], X0, RAX);
          fd(in.args[1], X1, RCX);
          if (in.op == Ins::Add) a_.addsd(X0, X1); else if (in.op == Ins::Sub) a_.subsd(X0, X1); else a_.mulsd(X0, X1);
          bail_if_nonfinite_x0();
          store_f(x, X0);
        }
        set_not_null(x);
        break;
      }
      case Ins::Div: {
        done.push_back(null_any(x, {in.args[0], in.args[1]}));
        fd(in.args[1], X1, RCX);
        a_.xorpd(X2, X2);
        a_.ucomisd(X1, X2);
        const size_t nz1 = a_.jcc(CcNE);
        const size_t nz2 = a_.jcc(CcP);
        set_null(x);
        done.push_back(a_.jmp());
        a_.patch_here(nz1); a_.patch_here(nz2);
        fd(in.args[0], X0, RAX);
        a_.divsd(X0, X1);
        bail_if_nonfinite_x0();
        store_f(x, X0);
        set_not_null(x);
        break;
      }
      case Ins::Mod: {
        done.push_back(null_any(x, {in.args[0], in.args[1]}));
        if (in.ty == Ty::I64) {
          ld_i(in.args[1], RCX);
          a_.test(RCX, RCX);
          const size_t nz = a_.jcc(CcNE);
          set_null(x);
          done.push_back(a_.jmp());
          a_.patch_here(nz);
          a_.cmp_imm32(RCX, -1);
          const size_t not_m1 = a_.jcc(CcNE);
          a_.mov_imm(RAX, 0);
          const size_t stored = a_.jmp();
          a_.patch_here(not_m1);
          ld_i(in.args[0], RAX);
          a_.cqo();
          a_.idiv(RCX);
          a_.mov(RAX, RDX);
          a_.patch_here(stored);
          store_i(x, RAX);
        } else {
          fd(in.args[1], X1, RCX);
          a_.xorpd(X2, X2);
          a_.ucomisd(X1, X2);
          const size_t nz1 = a_.jcc(CcNE);
          const size_t nz2 = a_.jcc(CcP);
          set_null(x);
          done.push_back(a_.jmp());
          a_.patch_here(nz1); a_.patch_here(nz2);
          fd(in.args[0], X0, RAX);
          call_fn((const void*)&vjit_fmod);
          store_f(x, X0);
        }
        set_not_null(x);
        break;
      }
      case Ins::IDiv: {
        done.push_back(null_any(x, {in.args[0], in.args[1]}));
        ld_i(in.args[1], RCX);
        a_.test(RCX, RCX);
        const size_t nz = a_.jcc(CcNE);
        set_null(x);
        done.push_back(a_.jmp());
        a_.patch_here(nz);
        ld_i(in.args[0], RAX);
        a_.cmp_imm32(RCX, -1);
        const size_t not_m1 = a_.jcc(CcNE);
        a_.mov_imm(RDX, 0x8000000000000000ull);
        a_.cmp(RAX, RDX);
        bail_if(CcE);
        a_.patch_here(not_m1);
        a_.cqo();
        a_.idiv(RCX);
        store_i(x, RAX);
        set_not_null(x);
        break;
      }
      case Ins::Eq: case Ins::Ne: case Ins::Lt: case Ins::Le: case Ins::Gt: case Ins::Ge: {
        done.push_back(null_any(x, {in.args[0], in.args[1]}));
        const bool both_int = ty(in.args[0]) == Ty::I64 && ty(in.args[1]) == Ty::I64;
        if (both_int) {
          ld_i(in.args[0], RAX);
          ld_i(in.args[1], RCX);
          a_.cmp(RAX, RCX);
          const Cc c = in.op == Ins::Eq ? CcE : in.op == Ins::Ne ? CcNE : in.op == Ins::Lt ? CcL : in.op == Ins::Le ? CcLE : in.op == Ins::Gt ? CcG : CcGE;
          a_.setcc(c, RAX);
          a_.movzx_b(RAX, RAX);
        } else {
          fd(in.args[0], X0, RAX);
          fd(in.args[1], X1, RCX);
          a_.ucomisd(X0, X1);
          const size_t unordered = a_.jcc(CcP);
          const Cc c = in.op == Ins::Eq ? CcE : in.op == Ins::Ne ? CcNE : in.op == Ins::Lt ? CcB : in.op == Ins::Le ? CcBE : in.op == Ins::Gt ? CcA : CcAE;
          a_.setcc(c, RAX);
          a_.movzx_b(RAX, RAX);
          const size_t ordered = a_.jmp();
          a_.patch_here(unordered);   // c == 0 in the interpreter: Eq, Le, Ge true, the rest false
          a_.mov_imm(RAX, (in.op == Ins::Eq || in.op == Ins::Le || in.op == Ins::Ge) ? 1 : 0);
          a_.patch_here(ordered);
        }
        store_i(x, RAX);
        set_not_null(x);
        break;
      }
      case Ins::And: case Ins::Or: {
        // an = null(0), bn = null(1); a = !an && truth(0); b = !bn && truth(1)
        ld_n(in.args[0], R8);
        ld_n(in.args[1], R9);
        load_truth(in.args[0]); a_.mov(R10, RAX);
        load_truth(in.args[1]); a_.mov(R11, RAX);
        // a &= !an ; b &= !bn
        a_.mov(RAX, R8); a_.cmp_imm32(RAX, 0);
        a_.mov_imm(RCX, 0); a_.cmov(CcNE, R10, RCX);   // an -> a = 0
        a_.mov(RAX, R9); a_.cmp_imm32(RAX, 0); a_.cmov(CcNE, R11, RCX);   // bn -> b = 0
        if (in.op == Ins::And) {
          // (!an && !a) || (!bn && !b): 0; else an || bn: null; else 1
          a_.mov(RAX, R8); a_.or_(RAX, R10); a_.test(RAX, RAX);   // an | a == 0  <=> !an && !a
          const size_t f1 = a_.jcc(CcE);
          a_.mov(RAX, R9); a_.or_(RAX, R11); a_.test(RAX, RAX);
          const size_t f2 = a_.jcc(CcE);
          a_.mov(RAX, R8); a_.or_(RAX, R9); a_.test(RAX, RAX);
          const size_t some_null = a_.jcc(CcNE);
          a_.mov_imm(RAX, 1); store_i(x, RAX); set_not_null(x);
          done.push_back(a_.jmp());
          a_.patch_here(some_null);
          set_null(x);
          done.push_back(a_.jmp());
          a_.patch_here(f1); a_.patch_here(f2);
          a_.mov_imm(RAX, 0); store_i(x, RAX); set_not_null(x);
        } else {
          a_.mov(RAX, R10); a_.or_(RAX, R11); a_.test(RAX, RAX);
          const size_t t = a_.jcc(CcNE);
          a_.mov(RAX, R8); a_.or_(RAX, R9); a_.test(RAX, RAX);
          const size_t some_null = a_.jcc(CcNE);
          a_.mov_imm(RAX, 0); store_i(x, RAX); set_not_null(x);
          done.push_back(a_.jmp());
          a_.patch_here(some_null);
          set_null(x);
          done.push_back(a_.jmp());
          a_.patch_here(t);
          a_.mov_imm(RAX, 1); store_i(x, RAX); set_not_null(x);
        }
        break;
      }
      case Ins::Not: {
        done.push_back(null_any(x, {in.args[0]}));
        load_truth(in.args[0]);
        a_.xor_(RCX, RCX);   // 1 - truth
        a_.mov_imm(RCX, 1); a_.sub(RCX, RAX); a_.mov(RAX, RCX);
        store_i(x, RAX);
        set_not_null(x);
        break;
      }
      case Ins::IsNull: case Ins::NotNull: {
        ld_n(in.args[0], RAX);
        if (in.op == Ins::NotNull) { a_.mov_imm(RCX, 1); a_.sub(RCX, RAX); a_.mov(RAX, RCX); }
        store_i(x, RAX);
        set_not_null(x);
        break;
      }
      case Ins::Case: {
        const size_t na = in.args.size();
        const bool has_else = na % 2 == 1;
        std::vector<size_t> to_value_jumps;   // (jump site, value arg) pairs are emitted inline instead
        std::vector<size_t> nexts;
        for (size_t k = 0; k + 1 < na; k += 2) {
          for (size_t j : nexts) a_.patch_here(j);
          nexts.clear();
          ld_n(in.args[k], RAX);
          a_.test(RAX, RAX);
          nexts.push_back(a_.jcc(CcNE));
          load_truth(in.args[k]);
          a_.test(RAX, RAX);
          nexts.push_back(a_.jcc(CcE));
          emit_case_value(x, in, in.args[k + 1], done);
        }
        for (size_t j : nexts) a_.patch_here(j);
        if (has_else) emit_case_value(x, in, in.args[na - 1], done);
        else { set_null(x); done.push_back(a_.jmp()); }
        break;
      }
      case Ins::Coalesce: {
        std::vector<size_t> nexts;
        for (size_t k = 0; k < in.args.size(); ++k) {
          for (size_t j : nexts) a_.patch_here(j);
          nexts.clear();
          ld_n(in.args[k], RAX);
          a_.test(RAX, RAX);
          nexts.push_back(a_.jcc(CcNE));
          if (in.ty != Ty::F64) { ld_i(in.args[k], RAX); store_i(x, RAX); }
          else { fd(in.args[k], X0, RAX); store_f(x, X0); }
          set_not_null(x);
          done.push_back(a_.jmp());
        }
        for (size_t j : nexts) a_.patch_here(j);
        set_null(x);
        done.push_back(a_.jmp());
        break;
      }
      case Ins::NullIf: {
        done.push_back(null_any(x, {in.args[0]}));
        ld_n(in.args[1], RAX);
        a_.test(RAX, RAX);
        const size_t b_null = a_.jcc(CcNE);
        if (ty(in.args[0]) == Ty::I64 && ty(in.args[1]) == Ty::I64) {
          ld_i(in.args[0], RAX); ld_i(in.args[1], RCX); a_.cmp(RAX, RCX);
          const size_t differ = a_.jcc(CcNE);
          set_null(x); done.push_back(a_.jmp());
          a_.patch_here(differ);
        } else {
          fd(in.args[0], X0, RAX); fd(in.args[1], X1, RCX); a_.ucomisd(X0, X1);
          const size_t differ = a_.jcc(CcNE);
          const size_t nan = a_.jcc(CcP);
          set_null(x); done.push_back(a_.jmp());
          a_.patch_here(differ); a_.patch_here(nan);
        }
        a_.patch_here(b_null);
        if (in.ty != Ty::F64) { ld_i(in.args[0], RAX); store_i(x, RAX); }
        else { ld_f(in.args[0], X0, RAX); store_f(x, X0); }
        set_not_null(x);
        break;
      }
      case Ins::Greatest: case Ins::Least: {
        std::vector<size_t> ops(in.args.begin(), in.args.end());
        done.push_back(null_any_list(x, ops));
        if (in.ty == Ty::I64) {
          ld_i(in.args[0], RAX);
          for (size_t k = 1; k < in.args.size(); ++k) {
            ld_i(in.args[k], RCX);
            a_.cmp(RCX, RAX);
            a_.cmov(in.op == Ins::Greatest ? CcG : CcL, RAX, RCX);
          }
          store_i(x, RAX);
        } else {
          fd(in.args[0], X0, RAX);
          for (size_t k = 1; k < in.args.size(); ++k) {
            fd(in.args[k], X1, RCX);
            if (in.op == Ins::Greatest) a_.ucomisd(X1, X0); else a_.ucomisd(X0, X1);   // v > best  |  best > v
            const size_t keep = a_.jcc(CcBE);
            a_.movsd(X0, X1);
            a_.patch_here(keep);
          }
          store_f(x, X0);
        }
        set_not_null(x);
        break;
      }
      case Ins::Abs: case Ins::Floor: case Ins::Ceil: {
        done.push_back(null_any(x, {in.args[0]}));
        if (in.ty == Ty::I64) {
          ld_i(in.args[0], RAX);
          if (in.op == Ins::Abs) {
            a_.mov_imm(RCX, 0x8000000000000000ull); a_.cmp(RAX, RCX); bail_if(CcE);
            a_.mov(RCX, RAX); a_.neg(RCX); a_.test(RAX, RAX); a_.cmov(CcS, RAX, RCX);
          }
          store_i(x, RAX);
        } else {
          ld_f(in.args[0], X0, RCX);
          if (in.op == Ins::Abs) { a_.movq_rx(RAX, X0); a_.btr63(RAX); store_i(x, RAX); }
          else { call_fn(in.op == Ins::Floor ? (const void*)&vjit_floor : (const void*)&vjit_ceil); store_f(x, X0); }
        }
        set_not_null(x);
        break;
      }
      case Ins::Sqrt: case Ins::Sin: case Ins::Cos: case Ins::Tan: case Ins::Atan: case Ins::Radians: case Ins::Degrees: {
        done.push_back(null_any(x, {in.args[0]}));
        fd(in.args[0], X0, RAX);
        if (in.op == Ins::Sqrt) {
          a_.xorpd(X1, X1);
          a_.ucomisd(X0, X1);          // a < 0: CF=1 and ordered
          const size_t nan = a_.jcc(CcP);
          const size_t nonneg = a_.jcc(CcAE);
          set_null(x); done.push_back(a_.jmp());
          a_.patch_here(nan); a_.patch_here(nonneg);
          a_.sqrtsd(X0, X0);
        } else if (in.op == Ins::Radians) {
          load_const_f(X1, 3.14159265358979323846, RAX); a_.mulsd(X0, X1);
          load_const_f(X1, 180.0, RAX); a_.divsd(X0, X1);
        } else if (in.op == Ins::Degrees) {
          load_const_f(X1, 180.0, RAX); a_.mulsd(X0, X1);
          load_const_f(X1, 3.14159265358979323846, RAX); a_.divsd(X0, X1);
        } else {
          call_fn(in.op == Ins::Sin ? (const void*)&vjit_sin : in.op == Ins::Cos ? (const void*)&vjit_cos : in.op == Ins::Tan ? (const void*)&vjit_tan : (const void*)&vjit_atan);
        }
        store_f(x, X0);
        set_not_null(x);
        break;
      }
      case Ins::CastI: {
        done.push_back(null_any(x, {in.args[0]}));
        if (ty(in.args[0]) == Ty::I64) { ld_i(in.args[0], RAX); store_i(x, RAX); }
        else {
          ld_f(in.args[0], X0, RAX);
          a_.mov_imm(RDI, at(in.args[0]).approx ? 1 : 0);
          a_.lea(RSI, RSP, slot_v(x));
          call_fn((const void*)&vjit_cast_i);
          a_.test(RAX, RAX);
          bail_if(CcNE);
        }
        set_not_null(x);
        break;
      }
      case Ins::CastF: {
        done.push_back(null_any(x, {in.args[0]}));
        if (at(in.args[0]).uns) { ld_i(in.args[0], RDI); call_fn((const void*)&vjit_u64_to_double); }
        else fd(in.args[0], X0, RAX);
        store_f(x, X0);
        set_not_null(x);
        break;
      }
      case Ins::BitAnd: case Ins::BitOr: case Ins::BitXor: case Ins::Shl: case Ins::Shr: case Ins::BitNot: {
        if (in.op == Ins::BitNot) done.push_back(null_any(x, {in.args[0]}));
        else done.push_back(null_any(x, {in.args[0], in.args[1]}));
        bits_of(in.args[0], RAX);
        if (in.op == Ins::BitNot) { a_.not_(RAX); }
        else {
          a_.store(RSP, slot_v(x), RAX);   // keep l while m is made (m may call out)
          bits_of(in.args[1], RCX);
          a_.load(RAX, RSP, slot_v(x));
          if (in.op == Ins::BitAnd) a_.and_(RAX, RCX);
          else if (in.op == Ins::BitOr) a_.or_(RAX, RCX);
          else if (in.op == Ins::BitXor) a_.xor_(RAX, RCX);
          else {
            a_.cmp_imm32(RCX, 64);
            const size_t small = a_.jcc(CcB);
            a_.mov_imm(RAX, 0);
            const size_t shifted = a_.jmp();
            a_.patch_here(small);
            if (in.op == Ins::Shl) a_.shl_cl(RAX); else a_.shr_cl(RAX);
            a_.patch_here(shifted);
          }
        }
        store_i(x, RAX);
        set_not_null(x);
        break;
      }
      case Ins::RoundI: {
        done.push_back(null_any(x, {in.args[0]}));
        if (ty(in.args[0]) == Ty::I64) {
          ld_i(in.args[0], RAX);
          a_.mov_imm(RCX, 1000000000000000ull); a_.cmp(RAX, RCX); bail_if(CcGE);
          a_.mov_imm(RCX, (uint64_t)(-1000000000000000ll)); a_.cmp(RAX, RCX); bail_if(CcLE);
          store_i(x, RAX);
        } else {
          ld_f(in.args[0], X0, RAX);
          call_fn((const void*)&vjit_round);
          a_.movq_rx(RAX, X0); a_.btr63(RAX); a_.movq_xr(X1, RAX);   // |a|
          load_const_f(X2, 1e15, RCX);
          a_.ucomisd(X1, X2);          // |a| < 1e15 else bail (a NaN bails too: not < 1e15)
          bail_if(CcP);
          bail_if(CcAE);
          a_.cvttsd2si(RAX, X0);
          store_i(x, RAX);
        }
        set_not_null(x);
        break;
      }
      default:
        break;
    }
    for (size_t j : done) a_.patch_here(j);
  }
  void emit_case_value(size_t x, const Ins& in, size_t v, std::vector<size_t>& done) {
    ld_n(v, RAX);
    a_.test(RAX, RAX);
    const size_t has = a_.jcc(CcE);
    set_null(x); done.push_back(a_.jmp());
    a_.patch_here(has);
    if (in.uns_check && at(v).uns) { ld_i(v, RAX); a_.test(RAX, RAX); bail_if(CcS); }
    if (in.ty != Ty::F64) { ld_i(v, RAX); store_i(x, RAX); }
    else { fd(v, X0, RAX); store_f(x, X0); }
    set_not_null(x);
    done.push_back(a_.jmp());
  }
  size_t null_any_list(size_t x, const std::vector<size_t>& ops) {
    bool first = true;
    for (size_t a : ops) {
      ld_n(a, first ? RAX : RCX);
      if (!first) a_.or_(RAX, RCX);
      first = false;
    }
    a_.test(RAX, RAX);
    const size_t not_null = a_.jcc(CcE);
    set_null(x);
    const size_t done = a_.jmp();
    a_.patch_here(not_null);
    return done;
  }
  // d = operand a's bits as the bit operators read them
  void bits_of(size_t a, Reg d) {
    if (ty(a) == Ty::I64) { ld_i(a, d); return; }
    ld_f(a, X0, RAX);
    call_fn((const void*)&vjit_bits_of_double);
    if (d != RAX) a_.mov(d, RAX);
  }
};

inline std::shared_ptr<Code> build(const Prog& P, std::string* why) {
  if (!enabled()) return nullptr;
  Builder b(P);
  return b.build(why);
}

}  // namespace vjit
}  // namespace vp
