// Native code for a lane program on AArch64 (AAPCS64: Apple Silicon macOS, arm64 Linux): the plan of
// sql_vjit_x64.inc instruction for instruction - the whole program as one loop over the chunk's rows, each non-leaf
// instruction's value in a stack slot (and, write-through, a register), the leaves gathered from their columns or
// read from their lanes, only the output lane written. The same coverage and the same rules: NULL rules per
// operator, integer overflow and a non-finite double bail out of the chunk (return 1), a moved column layout returns
// 2, the libm functions and casts the interpreter calls are called through the same helpers (sql_vjit_common.inc),
// the loop-invariant instructions hoisted the same way. FLOOR / CEIL are FRINTM / FRINTP - IEEE floor and ceil, the
// instructions std::floor / std::ceil are on this architecture, where x86 derives them from a truncation.
//
// AIOS_SQL_VJIT_CHECK=1 compares every chunk with the interpreter bit for bit (a mismatch aborts); AIOS_SQL_VJIT=0
// turns native code off. Code pages: MAP_JIT on macOS (written with this thread's JIT write protection lifted, then
// sys_icache_invalidate), mmap + mprotect + __builtin___clear_cache on Linux - as grain_jit_arm64.cpp does.
namespace vp {
namespace vjit {

using R = uint32_t;   // a register number: x0..x30 (31: xzr or sp by context), or d0..d31
enum : R { X0 = 0, X1, X2, X3, X4, X5, X6, X7, X8, X9, X10, X11, X12, X13, X14, X15, X16, X17,
           X19 = 19, X20, X21, X22, X23, X24, X29 = 29, X30 = 30, XZR = 31, SPR = 31 };
enum Cond : uint32_t { EQ = 0, NE = 1, HS = 2, LO = 3, MI = 4, PL = 5, VS = 6, VC = 7, HI = 8, LS = 9, GE = 10, LT = 11, GT = 12, LE = 13 };

struct Asm {
  std::vector<uint32_t> buf;
  bool range_bad = false;            // a branch past its field's reach (the code is not used)
  size_t here() const { return buf.size(); }
  void w(uint32_t v) { buf.push_back(v); }
  // moves
  void movz(R d, uint32_t imm16, unsigned hw) { w(0xD2800000u | (hw << 21) | ((imm16 & 0xFFFF) << 5) | d); }
  void movn(R d, uint32_t imm16, unsigned hw) { w(0x92800000u | (hw << 21) | ((imm16 & 0xFFFF) << 5) | d); }
  void movk(R d, uint32_t imm16, unsigned hw) { w(0xF2800000u | (hw << 21) | ((imm16 & 0xFFFF) << 5) | d); }
  void mov_imm(R d, uint64_t v) {
    int zeros = 0, ones = 0;
    for (unsigned hw = 0; hw < 4; ++hw) {
      const uint32_t part = (uint32_t)((v >> (16 * hw)) & 0xFFFF);
      if (part == 0) ++zeros; else if (part == 0xFFFF) ++ones;
    }
    if (ones > zeros) {
      bool first = true;
      for (unsigned hw = 0; hw < 4; ++hw) {
        const uint32_t part = (uint32_t)((v >> (16 * hw)) & 0xFFFF);
        if (part == 0xFFFF) continue;
        if (first) { movn(d, ~part & 0xFFFF, hw); first = false; } else movk(d, part, hw);
      }
      if (first) movn(d, 0, 0);   // all ones
      return;
    }
    bool first = true;
    for (unsigned hw = 0; hw < 4; ++hw) {
      const uint32_t part = (uint32_t)((v >> (16 * hw)) & 0xFFFF);
      if (part == 0) continue;
      if (first) { movz(d, part, hw); first = false; } else movk(d, part, hw);
    }
    if (first) movz(d, 0, 0);
  }
  void mov(R d, R m) { w(0xAA0003E0u | (m << 16) | d); }                                   // orr d, xzr, m (not sp)
  void add_imm(R d, R n, uint32_t imm) { w(0x91000000u | (imm << 10) | (n << 5) | d); }    // imm < 4096; n / d may be sp
  void sub_imm(R d, R n, uint32_t imm) { w(0xD1000000u | (imm << 10) | (n << 5) | d); }
  void sub_sp(uint32_t n) {   // n % 16 == 0
    if (n >> 12) w(0xD1400000u | ((n >> 12) << 10) | (SPR << 5) | SPR);
    if (n & 0xFFF) sub_imm(SPR, SPR, n & 0xFFF);
  }
  void add_sp(uint32_t n) {
    if (n >> 12) w(0x91400000u | ((n >> 12) << 10) | (SPR << 5) | SPR);
    if (n & 0xFFF) add_imm(SPR, SPR, n & 0xFFF);
  }
  // loads / stores at [n + off]: the scaled unsigned offset where it reaches, else the offset in x17
  void ldst(uint32_t imm_op, uint32_t reg_op, unsigned scale, R t, R n, uint32_t off) {
    if (off % (1u << scale) == 0 && (off >> scale) < 4096) w(imm_op | ((off >> scale) << 10) | (n << 5) | t);
    else { mov_imm(X17, off); w(reg_op | (X17 << 16) | (n << 5) | t); }
  }
  void ldr(R t, R n, uint32_t off) { ldst(0xF9400000u, 0xF8606800u, 3, t, n, off); }        // 64-bit
  void str(R t, R n, uint32_t off) { ldst(0xF9000000u, 0xF8206800u, 3, t, n, off); }
  void ldr_w(R t, R n, uint32_t off) { ldst(0xB9400000u, 0xB8606800u, 2, t, n, off); }      // 32-bit, zero-extended
  void str_w(R t, R n, uint32_t off) { ldst(0xB9000000u, 0xB8206800u, 2, t, n, off); }
  void ldr_d(R t, R n, uint32_t off) { ldst(0xFD400000u, 0xFC606800u, 3, t, n, off); }      // a double
  void str_d(R t, R n, uint32_t off) { ldst(0xFD000000u, 0xFC206800u, 3, t, n, off); }
  void ldr_x8(R t, R n, R m) { w(0xF8607800u | (m << 16) | (n << 5) | t); }   // ldr t, [n, m, lsl #3]
  void str_x8(R t, R n, R m) { w(0xF8207800u | (m << 16) | (n << 5) | t); }
  void ldr_d8(R t, R n, R m) { w(0xFC607800u | (m << 16) | (n << 5) | t); }   // ldr d_t, [n, m, lsl #3]
  void str_d8(R t, R n, R m) { w(0xFC207800u | (m << 16) | (n << 5) | t); }
  void ldr_w4(R t, R n, R m) { w(0xB8607800u | (m << 16) | (n << 5) | t); }   // ldr w_t, [n, m, lsl #2]
  void str_w4(R t, R n, R m) { w(0xB8207800u | (m << 16) | (n << 5) | t); }
  void ldrb_x(R t, R n, R m) { w(0x38606800u | (m << 16) | (n << 5) | t); }   // ldrb w_t, [n, m]
  void strb_x(R t, R n, R m) { w(0x38206800u | (m << 16) | (n << 5) | t); }
  void stp_pre16(R a, R b) { w(0xA9800000u | ((uint32_t)(-2 & 0x7F) << 15) | (b << 10) | (SPR << 5) | a); }   // stp a, b, [sp, #-16]!
  void ldp_post16(R a, R b) { w(0xA8C00000u | (2u << 15) | (b << 10) | (SPR << 5) | a); }                    // ldp a, b, [sp], #16
  // integers
  void add(R d, R n, R m) { w(0x8B000000u | (m << 16) | (n << 5) | d); }
  void add_lsl(R d, R n, R m, unsigned sh) { w(0x8B000000u | (m << 16) | (sh << 10) | (n << 5) | d); }
  void sub(R d, R n, R m) { w(0xCB000000u | (m << 16) | (n << 5) | d); }
  void adds(R d, R n, R m) { w(0xAB000000u | (m << 16) | (n << 5) | d); }
  void subs(R d, R n, R m) { w(0xEB000000u | (m << 16) | (n << 5) | d); }
  void adc(R d, R n, R m) { w(0x9A000000u | (m << 16) | (n << 5) | d); }
  void adcs(R d, R n, R m) { w(0xBA000000u | (m << 16) | (n << 5) | d); }
  void and_(R d, R n, R m) { w(0x8A000000u | (m << 16) | (n << 5) | d); }
  void orr(R d, R n, R m) { w(0xAA000000u | (m << 16) | (n << 5) | d); }
  void eor(R d, R n, R m) { w(0xCA000000u | (m << 16) | (n << 5) | d); }
  void mvn(R d, R m) { w(0xAA2003E0u | (m << 16) | d); }
  void neg(R d, R m) { w(0xCB0003E0u | (m << 16) | d); }
  void mul(R d, R n, R m) { w(0x9B007C00u | (m << 16) | (n << 5) | d); }
  void smulh(R d, R n, R m) { w(0x9B407C00u | (m << 16) | (n << 5) | d); }
  void sdiv(R d, R n, R m) { w(0x9AC00C00u | (m << 16) | (n << 5) | d); }
  void msub(R d, R n, R m, R a) { w(0x9B008000u | (m << 16) | (a << 10) | (n << 5) | d); }   // d = a - n * m
  void lslv(R d, R n, R m) { w(0x9AC02000u | (m << 16) | (n << 5) | d); }
  void lsrv(R d, R n, R m) { w(0x9AC02400u | (m << 16) | (n << 5) | d); }
  void lsr_imm(R d, R n, unsigned sh) { w(0xD340FC00u | (sh << 16) | (n << 5) | d); }
  void lsl_imm(R d, R n, unsigned sh) { w(0xD3400000u | (((64 - sh) & 63) << 16) | ((63 - sh) << 10) | (n << 5) | d); }
  void asr_imm(R d, R n, unsigned sh) { w(0x9340FC00u | (sh << 16) | (n << 5) | d); }
  void and7(R d, R n) { w(0x92400800u | (n << 5) | d); }        // and d, n, #7
  void clear_sign(R d, R n) { w(0x9240F800u | (n << 5) | d); }  // and d, n, #0x7fffffffffffffff
  void flip_sign(R d, R n) { w(0xD2410000u | (n << 5) | d); }   // eor d, n, #0x8000000000000000
  void cmp(R n, R m) { w(0xEB00001Fu | (m << 16) | (n << 5)); }
  void cmp_imm(R n, uint32_t imm) { w(0xF100001Fu | (imm << 10) | (n << 5)); }     // imm < 4096
  void cmn_imm(R n, uint32_t imm) { w(0xB100001Fu | (imm << 10) | (n << 5)); }     // cmp n, #-imm
  void cmp_w_imm(R n, uint32_t imm) { w(0x7100001Fu | (imm << 10) | (n << 5)); }
  void cmp_asr63(R n, R m) { w(0xEB80FC1Fu | (m << 16) | (n << 5)); }               // cmp n, m, asr #63
  void cset(R d, Cond c) { w(0x9A9F07E0u | ((c ^ 1u) << 12) | d); }
  void csel(R d, R n, R m, Cond c) { w(0x9A800000u | (m << 16) | (c << 12) | (n << 5) | d); }
  // doubles
  void fmov_dx(R d, R n) { w(0x9E670000u | (n << 5) | d); }   // fmov d, x
  void fmov_xd(R d, R n) { w(0x9E660000u | (n << 5) | d); }   // fmov x, d
  void fmov(R d, R n) { w(0x1E604000u | (n << 5) | d); }
  void fadd(R d, R n, R m) { w(0x1E602800u | (m << 16) | (n << 5) | d); }
  void fsub(R d, R n, R m) { w(0x1E603800u | (m << 16) | (n << 5) | d); }
  void fmul(R d, R n, R m) { w(0x1E600800u | (m << 16) | (n << 5) | d); }
  void fdiv(R d, R n, R m) { w(0x1E601800u | (m << 16) | (n << 5) | d); }
  void fsqrt(R d, R n) { w(0x1E61C000u | (n << 5) | d); }
  void fneg(R d, R n) { w(0x1E614000u | (n << 5) | d); }
  void frintm(R d, R n) { w(0x1E654000u | (n << 5) | d); }
  void frintp(R d, R n) { w(0x1E64C000u | (n << 5) | d); }
  void fcmp(R n, R m) { w(0x1E602000u | (m << 16) | (n << 5)); }
  void fcmp0(R n) { w(0x1E602008u | (n << 5)); }
  void fcsel(R d, R n, R m, Cond c) { w(0x1E600C00u | (m << 16) | (c << 12) | (n << 5) | d); }
  void scvtf(R d, R n) { w(0x9E620000u | (n << 5) | d); }     // d = (double)(int64)x
  void fcvtzs(R d, R n) { w(0x9E780000u | (n << 5) | d); }    // x = (int64)trunc(d)
  // control
  size_t b() { w(0x14000000u); return here() - 1; }
  size_t bcond(Cond c) { w(0x54000000u | c); return here() - 1; }
  size_t cbz(R t) { w(0xB4000000u | t); return here() - 1; }
  size_t cbnz(R t) { w(0xB5000000u | t); return here() - 1; }
  size_t cbz_w(R t) { w(0x34000000u | t); return here() - 1; }
  size_t cbnz_w(R t) { w(0x35000000u | t); return here() - 1; }
  size_t tbnz(R t, unsigned bit) { w(0x37000000u | ((bit >> 5) << 31) | ((bit & 31) << 19) | t); return here() - 1; }
  void blr(R n) { w(0xD63F0000u | (n << 5)); }
  void ret() { w(0xD65F03C0u); }
  void patch(size_t at, size_t target) {
    const int64_t rel = (int64_t)target - (int64_t)at;   // in instructions
    uint32_t& ins = buf[at];
    if ((ins & 0x7C000000u) == 0x14000000u) {                                       // b
      if (rel < -(1 << 25) || rel >= (1 << 25)) range_bad = true;
      ins = (ins & 0xFC000000u) | ((uint32_t)rel & 0x3FFFFFFu);
    } else if ((ins & 0xFF000010u) == 0x54000000u || (ins & 0x7E000000u) == 0x34000000u) {   // b.cond, cbz / cbnz
      if (rel < -(1 << 18) || rel >= (1 << 18)) range_bad = true;
      ins = (ins & ~(0x7FFFFu << 5)) | (((uint32_t)rel & 0x7FFFFu) << 5);
    } else {                                                                         // tbz / tbnz
      if (rel < -(1 << 13) || rel >= (1 << 13)) range_bad = true;
      ins = (ins & ~(0x3FFFu << 5)) | (((uint32_t)rel & 0x3FFFu) << 5);
    }
  }
  void patch_here(size_t at) { patch(at, here()); }
  void call(const void* fn) { mov_imm(X16, (uint64_t)(uintptr_t)fn); blr(X16); }
};

// Executable pages holding `words`; null (why says which step failed) otherwise.
inline uint8_t* a64_install(const std::vector<uint32_t>& words, size_t* bytes_out, std::string* why) {
  const size_t bytes = words.size() * 4;
#if defined(__APPLE__)
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
  if (p == MAP_FAILED) { if (why) *why = "mmap"; return nullptr; }
  pthread_jit_write_protect_np(0);
  std::memcpy(p, words.data(), bytes);
  pthread_jit_write_protect_np(1);
  sys_icache_invalidate(p, bytes);
#else
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) { if (why) *why = "mmap"; return nullptr; }
  std::memcpy(p, words.data(), bytes);
  __builtin___clear_cache(static_cast<char*>(p), static_cast<char*>(p) + bytes);
  if (mprotect(p, bytes, PROT_READ | PROT_EXEC) != 0) { munmap(p, bytes); if (why) *why = "mprotect"; return nullptr; }
#endif
  *bytes_out = bytes;
  return static_cast<uint8_t*>(p);
}

inline bool is_leaf(Ins::Op op) { return vjit_leaf(op); }

struct Code {
  uint8_t* mem = nullptr;
  size_t bytes = 0;
  Fn fn = nullptr;
  // Loop-invariant instructions are emitted once before the row loop as well as in it; Env::hoist chooses
  // (sql_vjit_x64.inc's Code says when the preamble is right).
  bool hoisted = false;
  uint64_t hoist_srcs = 0;
  ~Code() { if (mem) munmap(mem, bytes); }
};

// Builds the function for P; null where an instruction is not covered (why says which).
// Registers: x19 env, x20 n, x21 the output values, x22 the output null bytes, x23 the leaf descriptors, x24 the
// row; x0..x8, x16, x17 scratch (x0..x2 also the helpers' arguments and result); x9..x15 and d16..d31 the
// instruction-result cache; d0..d4 scratch doubles. The frame: per instruction a value and a null slot, then the
// sources' row-id arrays.
class Builder {
 public:
  explicit Builder(const Prog& P) : P_(P) {}
  std::shared_ptr<Code> build(std::string* why) {
    const size_t K = P_.ins.size();
    if (K < 2 || K > 256) { if (why) *why = "program size"; return nullptr; }
    for (size_t x = 0; x < K; ++x) if (!covered(x, why)) return nullptr;
    native_.assign(K, 0);
    nsrc_ = 0;
    for (size_t x = 0; x < K; ++x) {
      if (!is_leaf(P_.ins[x].op) || x >= P_.leaf_kind.size() || P_.leaf_kind[x] == LeafLane) continue;
      if (P_.ins[x].op != Ins::Konst && P_.ins[x].src >= 64) continue;   // past the row-id table: the lane path
      native_[x] = 1;
      if (P_.ins[x].op != Ins::Konst && P_.ins[x].src + 1 > nsrc_) nsrc_ = P_.ins[x].src + 1;
    }
    // loop-invariant instructions, as sql_vjit_x64.inc decides them
    inv_.assign(K, 0);
    uint64_t hoist_srcs = 0;
    for (size_t x = 0; x < K; ++x) {
      const Ins& in = P_.ins[x];
      if (is_leaf(in.op)) {
        if (!native_[x]) continue;
        if (in.op == Ins::Konst) { inv_[x] = 1; continue; }
        if (in.cs && in.cs->size() == 1 && in.src < 64) { inv_[x] = 1; hoist_srcs |= (uint64_t)1 << in.src; }
        continue;
      }
      if (in.op == Ins::TupleIn || in.args.empty()) continue;
      bool all = true;
      for (uint32_t a : in.args) if (a >= x || !inv_[a]) { all = false; break; }
      if (all) inv_[x] = 1;
    }
    bool any_inv = false;
    for (size_t x = 0; x < K; ++x) if (inv_[x]) any_inv = true;
    hoisted_ = any_inv && hoist_enabled();
    hoist_srcs_ = any_inv ? hoist_srcs : 0;
    frame_ = (uint32_t)(16 * K + 8 * nsrc_);
    frame_ = (frame_ + 15) & ~15u;   // sp stays 16-aligned
    a_.stp_pre16(X29, X30);
    a_.add_imm(X29, SPR, 0);
    a_.stp_pre16(X19, X20);
    a_.stp_pre16(X21, X22);
    a_.stp_pre16(X23, X24);
    a_.sub_sp(frame_);
    a_.mov(X19, X0);                                         // env
    a_.ldr(X20, X19, 0);                                     // n
    a_.ldr(X21, X19, 32 + (P_.out().ty == Ty::F64 ? 8 : 0)); // out_i or out_f
    a_.ldr(X22, X19, 48);                                    // out_n
    a_.ldr(X23, X19, 56);                                    // the leaf descriptors
    // the lane leaves' pointers into their slots; the gathered leaves' layouts checked (moved: return 2)
    for (size_t x = 0; x < K; ++x) {
      if (!is_leaf(P_.ins[x].op)) continue;
      if (native_[x]) {
        a_.ldr_w(X0, X23, (uint32_t)(72 * x + 68));
        a_.cmp_w_imm(X0, P_.leaf_kind[x]);
        moved_.push_back(a_.bcond(NE));
        continue;
      }
      a_.ldr(X0, X19, P_.ins[x].ty == Ty::F64 ? 16 : 8);
      a_.ldr(X0, X0, (uint32_t)(8 * x));
      a_.str(X0, SPR, slot_v(x));
      a_.ldr(X0, X19, 24);
      a_.ldr(X0, X0, (uint32_t)(8 * x));
      a_.str(X0, SPR, slot_n(x));
    }
    // the sources' row-id arrays into their slots
    for (size_t sidx = 0; sidx < nsrc_; ++sidx) {
      a_.ldr(X0, X19, 64);
      a_.ldr(X0, X0, (uint32_t)(8 * sidx));
      a_.str(X0, SPR, slot_rid(sidx));
    }
    a_.mov_imm(X24, 0);
    const size_t empty = a_.cbz(X20);
    auto emit_inv = [&] {
      for (size_t x = 0; x < K; ++x) {
        if (!inv_[x]) continue;
        if (native_[x]) emit_leaf(x);
        else if (!is_leaf(P_.ins[x].op)) emit(x);
      }
      forget_all();
    };
    if (hoisted_) {
      a_.ldr_w(X0, X19, 72);
      const size_t skip_pre = a_.cbz_w(X0);
      emit_inv();
      a_.patch_here(skip_pre);
    }
    const size_t loop = a_.here();
    if (hoisted_) {
      a_.ldr_w(X0, X19, 72);
      const size_t skip_loop = a_.cbnz_w(X0);
      emit_inv();
      a_.patch_here(skip_loop);
    }
    for (size_t x = 0; x < K; ++x) {
      if (hoisted_ && inv_[x]) continue;
      if (native_[x]) emit_leaf(x);
      else if (!is_leaf(P_.ins[x].op)) emit(x);
    }
    // the output lane
    const size_t K1 = K - 1;
    if (is_leaf(P_.ins[K1].op) && !native_[K1]) { if (why) *why = "a leaf as the result"; return nullptr; }
    a_.ldr(X0, SPR, slot_v(K1));
    a_.str_x8(X0, X21, X24);
    a_.ldr(X0, SPR, slot_n(K1));
    a_.strb_x(X0, X22, X24);
    a_.add_imm(X24, X24, 1);
    a_.cmp(X24, X20);
    a_.patch(a_.bcond(LO), loop);
    a_.patch_here(empty);
    a_.mov_imm(X0, 0);
    epilogue();
    const size_t bail = a_.here();
    for (size_t at : bails_) a_.patch(at, bail);
    a_.mov_imm(X0, 1);
    epilogue();
    const size_t moved = a_.here();
    for (size_t at : moved_) a_.patch(at, moved);
    a_.mov_imm(X0, 2);
    epilogue();
    if (a_.range_bad) { if (why) *why = "branch range"; return nullptr; }
    auto code = std::make_shared<Code>();
    code->mem = a64_install(a_.buf, &code->bytes, why);
    if (!code->mem) return nullptr;
    code->fn = reinterpret_cast<Fn>(code->mem);
    code->hoisted = hoisted_;
    code->hoist_srcs = hoist_srcs_;
    return code;
  }

 private:
  const Prog& P_;
  Asm a_;
  uint32_t frame_ = 0;
  std::vector<size_t> bails_;
  std::vector<size_t> moved_;      // the layout checks' jumps (return 2)
  std::vector<uint8_t> native_;    // per instruction: a leaf the loop gathers itself
  std::vector<uint8_t> inv_;       // per instruction: loop-invariant (Code::hoisted)
  bool hoisted_ = false;
  uint64_t hoist_srcs_ = 0;
  size_t nsrc_ = 0;                // sources with a row-id slot
  static constexpr R D0 = 0, D1 = 1, D2 = 2, D3 = 3;
  uint32_t slot_rid(size_t sidx) const { return (uint32_t)(16 * P_.ins.size() + 8 * sidx); }
  uint32_t slot_v(size_t x) const { return (uint32_t)(16 * x); }
  uint32_t slot_n(size_t x) const { return (uint32_t)(16 * x + 8); }
  bool lane_leaf(size_t x) const { return is_leaf(at(x).op) && !native_[x]; }
  const Ins& at(size_t x) const { return P_.ins[x]; }
  Ty ty(size_t x) const { return P_.ins[x].ty; }
  void bail_if(Cond c) { bails_.push_back(a_.bcond(c)); }
  void epilogue() {
    a_.add_sp(frame_);
    a_.ldp_post16(X23, X24);
    a_.ldp_post16(X21, X22);
    a_.ldp_post16(X19, X20);
    a_.ldp_post16(X29, X30);
    a_.ret();
  }

  // The same coverage as sql_vjit_x64.inc's Builder::covered.
  bool covered(size_t x, std::string* why) {
    const Ins& in = at(x);
    auto no = [&](const char* w) { if (why) *why = std::string(w) + " (" + std::to_string((int)in.op) + ")"; return false; };
    if (is_leaf(in.op)) return true;
    for (uint32_t a : in.args) if (a >= x) return no("operand order");
    const bool text_out = in.ty == Ty::Str;
    bool text_in = false;
    for (uint32_t a : in.args) if (ty(a) == Ty::Str) text_in = true;
    switch (in.op) {
      case Ins::Eq: case Ins::Ne: case Ins::Lt: case Ins::Le: case Ins::Gt: case Ins::Ge:
        if (text_in && (ty(in.args[0]) != Ty::Str || ty(in.args[1]) != Ty::Str)) return no("text compared with a number");
        break;
      case Ins::Case: case Ins::Coalesce:
        if (text_in && !text_out) return no("text in a numeric CASE");
        break;
      case Ins::NullIf:
        if (in.ty != ty(in.args[0])) return no("NULLIF of mixed kinds");
        if (ty(in.args[1]) == Ty::Str && ty(in.args[0]) != Ty::Str) return no("NULLIF of text against a number");
        break;
      case Ins::IsNull: case Ins::NotNull:
        break;
      case Ins::Neg: case Ins::Add: case Ins::Sub: case Ins::Mul: case Ins::Div: case Ins::Mod: case Ins::IDiv:
      case Ins::And: case Ins::Or: case Ins::Not: case Ins::Greatest: case Ins::Least:
      case Ins::Abs: case Ins::Floor: case Ins::Ceil: case Ins::Sqrt: case Ins::Sin: case Ins::Cos: case Ins::Tan: case Ins::Atan:
      case Ins::Atan2: case Ins::Pow:
      case Ins::Radians: case Ins::Degrees: case Ins::CastI: case Ins::CastF:
      case Ins::BitAnd: case Ins::BitOr: case Ins::BitXor: case Ins::Shl: case Ins::Shr: case Ins::BitNot: case Ins::RoundI:
        if (text_in || text_out) return no("text in arithmetic");
        break;
      default:
        return no("operator");
    }
    if (text_out && in.op != Ins::Case && in.op != Ins::Coalesce && in.op != Ins::NullIf) return no("text result");
    if (in.op == Ins::Case && in.args.size() < 2) return no("CASE arity");
    return true;
  }

  // The write-through register cache of sql_vjit_x64.inc: a result goes to its slot and to a caller-saved
  // register; readers take the register. A helper call forgets every register (all are caller-saved).
  static constexpr R kGprPool[7] = {X9, X10, X11, X12, X13, X14, X15};
  static constexpr R kFprPool[16] = {16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
  int gpr_holds_[32] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
  int fpr_holds_[32] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
  std::vector<int> cached_in_;   // instruction -> its register (a GPR for an I64 or text lane, a d register for F64), or -1
  size_t rr_gpr_ = 0, rr_fpr_ = 0;
  int cached(size_t x) const { return x < cached_in_.size() ? cached_in_[x] : -1; }
  void forget_all() {
    for (R r : kGprPool) { if (gpr_holds_[r] >= 0) cached_in_[(size_t)gpr_holds_[r]] = -1; gpr_holds_[r] = -1; }
    for (R r : kFprPool) { if (fpr_holds_[r] >= 0) cached_in_[(size_t)fpr_holds_[r]] = -1; fpr_holds_[r] = -1; }
  }
  R cache_gpr(size_t x) {
    if (cached_in_.size() <= x) cached_in_.resize(x + 1, -1);
    if (cached_in_[x] >= 0) return (R)cached_in_[x];
    const R r = kGprPool[rr_gpr_++ % 7];
    if (gpr_holds_[r] >= 0) cached_in_[(size_t)gpr_holds_[r]] = -1;
    gpr_holds_[r] = (int)x;
    cached_in_[x] = (int)r;
    return r;
  }
  R cache_fpr(size_t x) {
    if (cached_in_.size() <= x) cached_in_.resize(x + 1, -1);
    if (cached_in_[x] >= 0) return (R)cached_in_[x];
    const R r = kFprPool[rr_fpr_++ % 16];
    if (fpr_holds_[r] >= 0) cached_in_[(size_t)fpr_holds_[r]] = -1;
    fpr_holds_[r] = (int)x;
    cached_in_[x] = (int)r;
    return r;
  }
  // Operand loads: value into a GPR (I64 or text lane) or a d register (F64); the null into a GPR (0/1).
  void ld_i(size_t a, R d) {
    if (lane_leaf(a)) { a_.ldr(d, SPR, slot_v(a)); a_.ldr_x8(d, d, X24); }
    else if (cached(a) >= 0) { if (ty(a) == Ty::F64) a_.fmov_xd(d, (R)cached(a)); else if ((R)cached(a) != d) a_.mov(d, (R)cached(a)); }
    else a_.ldr(d, SPR, slot_v(a));
  }
  void ld_f(size_t a, R v, R scratch) {
    if (lane_leaf(a)) { a_.ldr(scratch, SPR, slot_v(a)); a_.ldr_d8(v, scratch, X24); }
    else if (cached(a) >= 0) { if (ty(a) != Ty::F64) a_.fmov_dx(v, (R)cached(a)); else if ((R)cached(a) != v) a_.fmov(v, (R)cached(a)); }
    else a_.ldr_d(v, SPR, slot_v(a));
  }
  void ld_n(size_t a, R d) {
    if (lane_leaf(a)) { a_.ldr(d, SPR, slot_n(a)); a_.ldrb_x(d, d, X24); }
    else a_.ldr(d, SPR, slot_n(a));
  }
  // A leaf gathered in the loop (sql_vjit_x64.inc's emit_leaf): the row's id from its source's row ids (kNullRow,
  // past the column or a NULL row: NULL), then the value from the column's flat storage (LeafDesc, by kind).
  void emit_leaf(size_t x) {
    const Ins& in = at(x);
    const uint32_t d = (uint32_t)(72 * x);   // the descriptor, at [x23 + d]
    std::vector<size_t> done;
    if (in.op == Ins::Konst) {
      if (in.knull) { set_null(x); return; }
      a_.ldr(X0, X23, d + 56);
      if (in.ty == Ty::F64) { a_.fmov_dx(D0, X0); store_f(x, D0); } else store_i(x, X0);
      set_not_null(x);
      return;
    }
    // x0 = the row's id in the source
    a_.ldr(X1, SPR, slot_rid(in.src));
    a_.ldr_x8(X0, X1, X24);
    std::vector<size_t> nulls;   // jumps to the NULL (ColNull: 1) outcome
    a_.cmn_imm(X0, 1);
    nulls.push_back(a_.bcond(EQ));
    a_.ldr(X1, X23, d + 24);
    a_.cmp(X0, X1);
    nulls.push_back(a_.bcond(HS));
    // the null bitmap: byte x0>>3, where it is within the bitmap's bytes (0 bytes: no NULL in the column)
    a_.ldr(X1, X23, d + 16);
    const size_t no_bitmap = a_.cbz(X1);
    a_.lsr_imm(X2, X0, 3);
    a_.cmp(X2, X1);
    const size_t past_bitmap = a_.bcond(HS);
    a_.ldr(X1, X23, d + 8);
    a_.ldrb_x(X2, X1, X2);
    a_.and7(X1, X0);
    a_.lsrv(X2, X2, X1);
    nulls.push_back(a_.tbnz(X2, 0));   // the bit is set
    a_.patch_here(no_bitmap);
    a_.patch_here(past_bitmap);
    switch (in.op) {
      case Ins::ColNull:
        a_.mov_imm(X0, 0); store_i(x, X0); set_not_null(x);
        done.push_back(a_.b());
        for (size_t j : nulls) a_.patch_here(j);
        a_.mov_imm(X0, 1); store_i(x, X0); set_not_null(x);
        for (size_t j : done) a_.patch_here(j);
        return;
      case Ins::ColI:
        a_.ldr(X1, X23, d + 0);
        a_.ldr_x8(X2, X1, X0);
        store_i(x, X2);
        break;
      case Ins::ColF:
        a_.ldr(X1, X23, d + 0);
        a_.ldr_d8(D0, X1, X0);
        store_f(x, D0);
        break;
      default: {   // 32-bit codes: x2 = the code
        a_.ldr(X1, X23, d + 0);
        a_.ldr_w4(X2, X1, X0);
        if (in.op == Ins::ColS) {
          a_.ldr(X1, X23, d + 56);
          a_.orr(X2, X2, X1);
          store_i(x, X2);
        } else if (in.op == Ins::StrIn) {
          a_.ldr(X1, X23, d + 48);
          a_.cmp(X2, X1);
          const size_t past = a_.bcond(HS);
          a_.ldr(X1, X23, d + 40);
          a_.ldrb_x(X0, X1, X2);
          const size_t have = a_.b();
          a_.patch_here(past);
          a_.mov_imm(X0, 0);
          a_.patch_here(have);
          store_i(x, X0);
        } else {   // CodeI / CodeF: the code's entry, NULL where the entry is or the code is past the table
          a_.ldr(X1, X23, d + 48);
          a_.cmp(X2, X1);
          nulls.push_back(a_.bcond(HS));
          a_.ldr(X1, X23, d + 40);
          a_.ldrb_x(X0, X1, X2);
          nulls.push_back(a_.cbnz(X0));
          a_.ldr(X1, X23, d + 32);
          if (in.op == Ins::CodeI) { a_.ldr_x8(X0, X1, X2); store_i(x, X0); }
          else { a_.ldr_d8(D0, X1, X2); store_f(x, D0); }
        }
        break;
      }
    }
    set_not_null(x);
    done.push_back(a_.b());
    for (size_t j : nulls) a_.patch_here(j);
    set_null(x);
    for (size_t j : done) a_.patch_here(j);
  }
  // operand a as a double in v (vp::eval's fd)
  void fd(size_t a, R v, R scratch) {
    if (ty(a) == Ty::I64) { ld_i(a, scratch); a_.scvtf(v, scratch); }
    else ld_f(a, v, scratch);
  }
  void set_null(size_t x) { a_.mov_imm(X16, 1); a_.str(X16, SPR, slot_n(x)); }   // x16: str's offset temp is x17
  void set_not_null(size_t x) { a_.str(XZR, SPR, slot_n(x)); }
  void store_i(size_t x, R s) {
    a_.str(s, SPR, slot_v(x));
    if (ty(x) == Ty::F64) { a_.fmov_dx(cache_fpr(x), s); return; }
    const R r = cache_gpr(x);
    if (r != s) a_.mov(r, s);
  }
  void store_f(size_t x, R v) {
    a_.str_d(v, SPR, slot_v(x));
    if (ty(x) != Ty::F64) { a_.fmov_xd(X0, v); const R r = cache_gpr(x); a_.mov(r, X0); return; }
    const R r = cache_fpr(x);
    if (r != v) a_.fmov(r, v);
  }
  void load_const_f(R v, double d, R scratch) { uint64_t bits; std::memcpy(&bits, &d, 8); a_.mov_imm(scratch, bits); a_.fmov_dx(v, scratch); }
  void call_fn(const void* fn) { forget_all(); a_.call(fn); }
  // the double in d0 non-finite: bail (the interpreter's "DOUBLE value is out of range")
  void bail_if_nonfinite_d0() {
    a_.fmov_xd(X0, D0);
    a_.lsl_imm(X0, X0, 1);
    a_.mov_imm(X1, 0xFFE0000000000000ull);
    a_.cmp(X0, X1);
    bail_if(HS);
  }
  // x0 = 1 where the operand is true (a number other than 0; a NaN is), 0 otherwise - the operand loaded already:
  // an I64 in x0, an F64 in d0
  void truth_of_loaded(size_t a) {
    if (ty(a) == Ty::I64) { a_.cmp_imm(X0, 0); a_.cset(X0, NE); }
    else { a_.fcmp0(D0); a_.cset(X0, NE); }   // NE: not equal, or unordered (a NaN)
  }
  void load_truth(size_t a) {   // x0 = truth of operand a (its null not looked at)
    if (ty(a) == Ty::I64) ld_i(a, X0); else ld_f(a, D0, X1);
    truth_of_loaded(a);
  }
  // null rule: dest null if any operand is; returns the jump to `done` taken after setting it
  size_t null_any_list(size_t x, const std::vector<size_t>& ops) {
    bool first = true;
    for (size_t a : ops) {
      ld_n(a, first ? X0 : X1);
      if (!first) a_.orr(X0, X0, X1);
      first = false;
    }
    const size_t not_null = a_.cbz(X0);
    set_null(x);
    const size_t done = a_.b();
    a_.patch_here(not_null);
    return done;
  }
  size_t null_any(size_t x, std::initializer_list<size_t> ops) { return null_any_list(x, std::vector<size_t>(ops)); }
  static Cond int_cond(Ins::Op op) { return op == Ins::Eq ? EQ : op == Ins::Ne ? NE : op == Ins::Lt ? LT : op == Ins::Le ? LE : op == Ins::Gt ? GT : GE; }

  void emit(size_t x) {
    const Ins& in = at(x);
    std::vector<size_t> done;
    switch (in.op) {
      case Ins::Neg: {
        done.push_back(null_any(x, {in.args[0]}));
        if (in.ty == Ty::I64) {
          ld_i(in.args[0], X0);
          a_.mov_imm(X1, 0x8000000000000000ull);
          a_.cmp(X0, X1);
          bail_if(EQ);
          a_.neg(X0, X0);
          store_i(x, X0);
        } else {
          ld_f(in.args[0], D0, X1);
          a_.fmov_xd(X0, D0);
          a_.flip_sign(X0, X0);
          store_i(x, X0);
        }
        set_not_null(x);
        break;
      }
      case Ins::Add: case Ins::Sub: case Ins::Mul: {
        done.push_back(null_any(x, {in.args[0], in.args[1]}));
        if (in.ty == Ty::I64) {
          ld_i(in.args[0], X0);
          ld_i(in.args[1], X1);
          if (in.op == Ins::Add) { a_.adds(X0, X0, X1); bail_if(VS); }
          else if (in.op == Ins::Sub) { a_.subs(X0, X0, X1); bail_if(VS); }
          else {
            // the product fits 64 bits where its high half is the low half's sign
            a_.mul(X2, X0, X1);
            a_.smulh(X3, X0, X1);
            a_.cmp_asr63(X3, X2);
            bail_if(NE);
            a_.mov(X0, X2);
          }
          store_i(x, X0);
        } else {
          fd(in.args[0], D0, X0);
          fd(in.args[1], D1, X1);
          if (in.exact_dyadic) a_.fmov(D2, D0);   // a, for the exactness check
          if (in.op == Ins::Add) a_.fadd(D0, D0, D1); else if (in.op == Ins::Sub) a_.fsub(D0, D0, D1); else a_.fmul(D0, D0, D1);
          bail_if_nonfinite_d0();
          store_f(x, D0);
          if (in.exact_dyadic) {
            // vjit_exact_dyadic_bad(a, b, r, is_mul): the lane must hold the interpreter's exact decimal
            a_.fmov(D3, D0); a_.fmov(D0, D2); a_.fmov(D2, D3);
            a_.mov_imm(X0, in.op == Ins::Mul ? 1 : 0);
            call_fn((const void*)&vjit_exact_dyadic_bad);
            a_.cmp_imm(X0, 0);
            bail_if(NE);
          }
        }
        set_not_null(x);
        break;
      }
      case Ins::Div: {
        done.push_back(null_any(x, {in.args[0], in.args[1]}));
        fd(in.args[1], D1, X1);
        a_.fcmp0(D1);
        const size_t nz = a_.bcond(NE);   // not zero, or NaN
        set_null(x);
        done.push_back(a_.b());
        a_.patch_here(nz);
        fd(in.args[0], D0, X0);
        a_.fdiv(D0, D0, D1);
        bail_if_nonfinite_d0();
        store_f(x, D0);
        set_not_null(x);
        break;
      }
      case Ins::Mod: {
        done.push_back(null_any(x, {in.args[0], in.args[1]}));
        if (in.ty == Ty::I64) {
          ld_i(in.args[1], X1);
          const size_t nz = a_.cbnz(X1);
          set_null(x);
          done.push_back(a_.b());
          a_.patch_here(nz);
          a_.cmn_imm(X1, 1);
          const size_t not_m1 = a_.bcond(NE);
          a_.mov_imm(X0, 0);
          const size_t stored = a_.b();
          a_.patch_here(not_m1);
          ld_i(in.args[0], X0);
          a_.sdiv(X2, X0, X1);
          a_.msub(X0, X2, X1, X0);   // a - (a / b) * b: the remainder with a's sign, as idiv's
          a_.patch_here(stored);
          store_i(x, X0);
        } else {
          fd(in.args[1], D1, X1);
          a_.fcmp0(D1);
          const size_t nz = a_.bcond(NE);
          set_null(x);
          done.push_back(a_.b());
          a_.patch_here(nz);
          fd(in.args[0], D0, X0);
          call_fn((const void*)&vjit_fmod);
          store_f(x, D0);
        }
        set_not_null(x);
        break;
      }
      case Ins::IDiv: {
        done.push_back(null_any(x, {in.args[0], in.args[1]}));
        ld_i(in.args[1], X1);
        const size_t nz = a_.cbnz(X1);
        set_null(x);
        done.push_back(a_.b());
        a_.patch_here(nz);
        ld_i(in.args[0], X0);
        a_.cmn_imm(X1, 1);
        const size_t not_m1 = a_.bcond(NE);
        a_.mov_imm(X2, 0x8000000000000000ull);
        a_.cmp(X0, X2);
        bail_if(EQ);
        a_.patch_here(not_m1);
        a_.sdiv(X0, X0, X1);
        store_i(x, X0);
        set_not_null(x);
        break;
      }
      case Ins::Eq: case Ins::Ne: case Ins::Lt: case Ins::Le: case Ins::Gt: case Ins::Ge: {
        done.push_back(null_any(x, {in.args[0], in.args[1]}));
        const bool both_int = ty(in.args[0]) == Ty::I64 && ty(in.args[1]) == Ty::I64;
        const bool texts = ty(in.args[0]) == Ty::Str;
        if (texts) {
          ld_i(in.args[0], X1);
          ld_i(in.args[1], X2);
          a_.mov_imm(X0, (uint64_t)(uintptr_t)&P_);
          call_fn((const void*)&vjit_text_cmp);   // x0 = -1, 0, 1
          a_.cmp_imm(X0, 0);
          a_.cset(X0, int_cond(in.op));
        } else if (both_int) {
          ld_i(in.args[0], X0);
          ld_i(in.args[1], X1);
          a_.cmp(X0, X1);
          a_.cset(X0, int_cond(in.op));
        } else {
          fd(in.args[0], D0, X0);
          fd(in.args[1], D1, X1);
          a_.fcmp(D0, D1);
          const size_t unordered = a_.bcond(VS);
          const Cond c = in.op == Ins::Eq ? EQ : in.op == Ins::Ne ? NE : in.op == Ins::Lt ? MI : in.op == Ins::Le ? LS : in.op == Ins::Gt ? GT : GE;
          a_.cset(X0, c);
          const size_t ordered = a_.b();
          a_.patch_here(unordered);   // c == 0 in the interpreter: Eq, Le, Ge true, the rest false
          a_.mov_imm(X0, (in.op == Ins::Eq || in.op == Ins::Le || in.op == Ins::Ge) ? 1 : 0);
          a_.patch_here(ordered);
        }
        store_i(x, X0);
        set_not_null(x);
        break;
      }
      case Ins::And: case Ins::Or: {
        // an = null(0), bn = null(1); a = !an && truth(0); b = !bn && truth(1) - in x3..x6 (no cached result lives there)
        ld_n(in.args[0], X3);
        ld_n(in.args[1], X4);
        load_truth(in.args[0]); a_.mov(X5, X0);
        load_truth(in.args[1]); a_.mov(X6, X0);
        a_.cmp_imm(X3, 0); a_.csel(X5, XZR, X5, NE);   // an -> a = 0
        a_.cmp_imm(X4, 0); a_.csel(X6, XZR, X6, NE);   // bn -> b = 0
        if (in.op == Ins::And) {
          // (!an && !a) || (!bn && !b): 0; else an || bn: null; else 1
          a_.orr(X0, X3, X5);
          const size_t f1 = a_.cbz(X0);
          a_.orr(X0, X4, X6);
          const size_t f2 = a_.cbz(X0);
          a_.orr(X0, X3, X4);
          const size_t some_null = a_.cbnz(X0);
          a_.mov_imm(X0, 1); store_i(x, X0); set_not_null(x);
          done.push_back(a_.b());
          a_.patch_here(some_null);
          set_null(x);
          done.push_back(a_.b());
          a_.patch_here(f1); a_.patch_here(f2);
          a_.mov_imm(X0, 0); store_i(x, X0); set_not_null(x);
        } else {
          a_.orr(X0, X5, X6);
          const size_t t = a_.cbnz(X0);
          a_.orr(X0, X3, X4);
          const size_t some_null = a_.cbnz(X0);
          a_.mov_imm(X0, 0); store_i(x, X0); set_not_null(x);
          done.push_back(a_.b());
          a_.patch_here(some_null);
          set_null(x);
          done.push_back(a_.b());
          a_.patch_here(t);
          a_.mov_imm(X0, 1); store_i(x, X0); set_not_null(x);
        }
        break;
      }
      case Ins::Not: {
        done.push_back(null_any(x, {in.args[0]}));
        load_truth(in.args[0]);
        a_.mov_imm(X1, 1); a_.sub(X0, X1, X0);   // 1 - truth
        store_i(x, X0);
        set_not_null(x);
        break;
      }
      case Ins::IsNull: case Ins::NotNull: {
        ld_n(in.args[0], X0);
        if (in.op == Ins::NotNull) { a_.mov_imm(X1, 1); a_.sub(X0, X1, X0); }
        store_i(x, X0);
        set_not_null(x);
        break;
      }
      case Ins::Case: {
        const size_t na = in.args.size();
        const bool has_else = na % 2 == 1;
        std::vector<size_t> nexts;
        for (size_t k = 0; k + 1 < na; k += 2) {
          for (size_t j : nexts) a_.patch_here(j);
          nexts.clear();
          ld_n(in.args[k], X0);
          nexts.push_back(a_.cbnz(X0));
          load_truth(in.args[k]);
          nexts.push_back(a_.cbz(X0));
          emit_case_value(x, in, in.args[k + 1], done);
        }
        for (size_t j : nexts) a_.patch_here(j);
        if (has_else) emit_case_value(x, in, in.args[na - 1], done);
        else { set_null(x); done.push_back(a_.b()); }
        break;
      }
      case Ins::Coalesce: {
        std::vector<size_t> nexts;
        for (size_t k = 0; k < in.args.size(); ++k) {
          for (size_t j : nexts) a_.patch_here(j);
          nexts.clear();
          ld_n(in.args[k], X0);
          nexts.push_back(a_.cbnz(X0));
          if (in.ty != Ty::F64) { ld_i(in.args[k], X0); store_i(x, X0); }
          else { fd(in.args[k], D0, X0); store_f(x, D0); }
          set_not_null(x);
          done.push_back(a_.b());
        }
        for (size_t j : nexts) a_.patch_here(j);
        set_null(x);
        done.push_back(a_.b());
        break;
      }
      case Ins::NullIf: {
        done.push_back(null_any(x, {in.args[0]}));
        ld_n(in.args[1], X0);
        const size_t b_null = a_.cbnz(X0);
        if (ty(in.args[0]) == Ty::Str) {
          ld_i(in.args[0], X1); ld_i(in.args[1], X2);
          a_.mov_imm(X0, (uint64_t)(uintptr_t)&P_);
          call_fn((const void*)&vjit_text_cmp);
          const size_t differ = a_.cbnz(X0);
          set_null(x); done.push_back(a_.b());
          a_.patch_here(differ);
        } else if (ty(in.args[0]) == Ty::I64 && ty(in.args[1]) == Ty::I64) {
          ld_i(in.args[0], X0); ld_i(in.args[1], X1); a_.cmp(X0, X1);
          const size_t differ = a_.bcond(NE);
          set_null(x); done.push_back(a_.b());
          a_.patch_here(differ);
        } else {
          fd(in.args[0], D0, X0); fd(in.args[1], D1, X1); a_.fcmp(D0, D1);
          const size_t differ = a_.bcond(NE);   // not equal, or unordered
          set_null(x); done.push_back(a_.b());
          a_.patch_here(differ);
        }
        a_.patch_here(b_null);
        if (in.ty != Ty::F64) { ld_i(in.args[0], X0); store_i(x, X0); }
        else { ld_f(in.args[0], D0, X0); store_f(x, D0); }
        set_not_null(x);
        break;
      }
      case Ins::Greatest: case Ins::Least: {
        std::vector<size_t> ops(in.args.begin(), in.args.end());
        done.push_back(null_any_list(x, ops));
        if (in.ty == Ty::I64) {
          ld_i(in.args[0], X0);
          for (size_t k = 1; k < in.args.size(); ++k) {
            ld_i(in.args[k], X1);
            a_.cmp(X1, X0);
            a_.csel(X0, X1, X0, in.op == Ins::Greatest ? GT : LT);   // v > best  |  v < best
          }
          store_i(x, X0);
        } else {
          fd(in.args[0], D0, X0);
          for (size_t k = 1; k < in.args.size(); ++k) {
            fd(in.args[k], D1, X1);
            // v > best  |  best > v, ordered (GT is false for unordered): take v
            if (in.op == Ins::Greatest) a_.fcmp(D1, D0); else a_.fcmp(D0, D1);
            a_.fcsel(D0, D1, D0, GT);
          }
          store_f(x, D0);
        }
        set_not_null(x);
        break;
      }
      case Ins::Abs: case Ins::Floor: case Ins::Ceil: {
        done.push_back(null_any(x, {in.args[0]}));
        if (in.ty == Ty::I64) {
          ld_i(in.args[0], X0);
          if (in.op == Ins::Abs) {
            a_.mov_imm(X1, 0x8000000000000000ull); a_.cmp(X0, X1); bail_if(EQ);
            a_.neg(X1, X0); a_.cmp_imm(X0, 0); a_.csel(X0, X1, X0, LT);
          }
          store_i(x, X0);
        } else {
          ld_f(in.args[0], D0, X1);
          if (in.op == Ins::Abs) { a_.fmov_xd(X0, D0); a_.clear_sign(X0, X0); store_i(x, X0); }
          else {
            if (in.op == Ins::Floor) a_.frintm(D0, D0); else a_.frintp(D0, D0);
            store_f(x, D0);
          }
        }
        set_not_null(x);
        break;
      }
      case Ins::Sqrt: case Ins::Sin: case Ins::Cos: case Ins::Tan: case Ins::Atan: case Ins::Radians: case Ins::Degrees: {
        done.push_back(null_any(x, {in.args[0]}));
        fd(in.args[0], D0, X0);
        if (in.op == Ins::Sqrt) {
          a_.fcmp0(D0);
          const size_t nonneg = a_.bcond(PL);   // not below zero (a NaN: unordered, N clear)
          set_null(x); done.push_back(a_.b());
          a_.patch_here(nonneg);
          a_.fsqrt(D0, D0);
        } else if (in.op == Ins::Radians) {
          load_const_f(D1, 3.14159265358979323846, X0); a_.fmul(D0, D0, D1);
          load_const_f(D1, 180.0, X0); a_.fdiv(D0, D0, D1);
        } else if (in.op == Ins::Degrees) {
          load_const_f(D1, 180.0, X0); a_.fmul(D0, D0, D1);
          load_const_f(D1, 3.14159265358979323846, X0); a_.fdiv(D0, D0, D1);
        } else {
          call_fn(in.op == Ins::Sin ? (const void*)&vjit_sin : in.op == Ins::Cos ? (const void*)&vjit_cos : in.op == Ins::Tan ? (const void*)&vjit_tan : (const void*)&vjit_atan);
        }
        store_f(x, D0);
        set_not_null(x);
        break;
      }
      case Ins::Atan2: case Ins::Pow: {
        done.push_back(null_any(x, {in.args[0], in.args[1]}));
        fd(in.args[0], D0, X0);
        fd(in.args[1], D1, X1);
        call_fn(in.op == Ins::Pow ? (const void*)&vjit_pow : (const void*)&vjit_atan2);
        bail_if_nonfinite_d0();
        store_f(x, D0);
        set_not_null(x);
        break;
      }
      case Ins::CastI: {
        done.push_back(null_any(x, {in.args[0]}));
        if (ty(in.args[0]) == Ty::I64) { ld_i(in.args[0], X0); store_i(x, X0); }
        else {
          // a whole number below 9.2e18 is its integer, in line; the helper (vjit_cast_i) decides the rest
          ld_f(in.args[0], D0, X0);
          a_.fmov_xd(X1, D0);
          a_.clear_sign(X1, X1);                           // |a| as bits: ordered as the doubles are
          { uint64_t lim; const double dl = 9.2e18; std::memcpy(&lim, &dl, 8); a_.mov_imm(X2, lim); }
          a_.cmp(X1, X2);
          const size_t slow1 = a_.bcond(HS);               // 9.2e18 or more, or NaN
          a_.fcvtzs(X0, D0);
          a_.scvtf(D1, X0);
          a_.fcmp(D1, D0);
          const size_t slow2 = a_.bcond(NE);               // a fraction
          store_i(x, X0);
          const size_t fast = a_.b();
          a_.patch_here(slow1); a_.patch_here(slow2);
          a_.mov_imm(X0, at(in.args[0]).approx ? 1 : 0);   // vjit_cast_i(a: d0, approx, out: the slot)
          a_.add_imm(X1, SPR, slot_v(x));
          call_fn((const void*)&vjit_cast_i);
          a_.cmp_w_imm(X0, 0);                             // an int: its low word
          bail_if(NE);
          a_.patch_here(fast);
        }
        set_not_null(x);
        break;
      }
      case Ins::CastF: {
        done.push_back(null_any(x, {in.args[0]}));
        if (at(in.args[0]).uns) { ld_i(in.args[0], X0); call_fn((const void*)&vjit_u64_to_double); }
        else fd(in.args[0], D0, X0);
        store_f(x, D0);
        set_not_null(x);
        break;
      }
      case Ins::BitAnd: case Ins::BitOr: case Ins::BitXor: case Ins::Shl: case Ins::Shr: case Ins::BitNot: {
        if (in.op == Ins::BitNot) done.push_back(null_any(x, {in.args[0]}));
        else done.push_back(null_any(x, {in.args[0], in.args[1]}));
        bits_of(in.args[0], X0);
        if (in.op == Ins::BitNot) { a_.mvn(X0, X0); }
        else {
          a_.str(X0, SPR, slot_v(x));   // keep l while m is made (m may call out)
          bits_of(in.args[1], X1);
          a_.ldr(X0, SPR, slot_v(x));
          if (in.op == Ins::BitAnd) a_.and_(X0, X0, X1);
          else if (in.op == Ins::BitOr) a_.orr(X0, X0, X1);
          else if (in.op == Ins::BitXor) a_.eor(X0, X0, X1);
          else {
            a_.cmp_imm(X1, 64);
            const size_t small = a_.bcond(LO);
            a_.mov_imm(X0, 0);
            const size_t shifted = a_.b();
            a_.patch_here(small);
            if (in.op == Ins::Shl) a_.lslv(X0, X0, X1); else a_.lsrv(X0, X0, X1);
            a_.patch_here(shifted);
          }
        }
        store_i(x, X0);
        set_not_null(x);
        break;
      }
      case Ins::RoundI: {
        done.push_back(null_any(x, {in.args[0]}));
        if (ty(in.args[0]) == Ty::I64) {
          ld_i(in.args[0], X0);
          a_.mov_imm(X1, 1000000000000000ull); a_.cmp(X0, X1); bail_if(GE);
          a_.mov_imm(X1, (uint64_t)(-1000000000000000ll)); a_.cmp(X0, X1); bail_if(LE);
          store_i(x, X0);
        } else {
          ld_f(in.args[0], D0, X0);
          call_fn((const void*)&vjit_round);
          a_.fmov_xd(X0, D0); a_.clear_sign(X0, X0); a_.fmov_dx(D1, X0);   // |a|
          load_const_f(D2, 1e15, X1);
          a_.fcmp(D1, D2);             // |a| < 1e15 else bail (a NaN bails too: not < 1e15)
          bail_if(VS);
          bail_if(GE);
          a_.fcvtzs(X0, D0);
          store_i(x, X0);
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
    ld_n(v, X0);
    const size_t has = a_.cbz(X0);
    set_null(x); done.push_back(a_.b());
    a_.patch_here(has);
    if (in.uns_check && at(v).uns) { ld_i(v, X0); a_.cmp_imm(X0, 0); bail_if(LT); }
    if (in.ty != Ty::F64) { ld_i(v, X0); store_i(x, X0); }
    else { fd(v, D0, X0); store_f(x, D0); }
    set_not_null(x);
    done.push_back(a_.b());
  }
  // d = operand a's bits as the bit operators read them
  void bits_of(size_t a, R d) {
    if (ty(a) == Ty::I64) { ld_i(a, d); return; }
    ld_f(a, D0, X0);
    call_fn((const void*)&vjit_bits_of_double);
    if (d != X0) a_.mov(d, X0);
  }
};

inline std::shared_ptr<Code> build(const Prog& P, std::string* why) {
  if (!enabled()) return nullptr;
  Builder b(P);
  return b.build(why);
}

}  // namespace vjit
}  // namespace vp
