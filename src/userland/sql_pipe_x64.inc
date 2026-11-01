// Native code for the fused star join aggregate (docs/asql-vectorized-plan.md, stage 3c.2). The fused loop
// (star_aggregate, 3c.1) finds a root row's group and its run of rows on the one-to-many side; this code folds
// that run into the group's aggregates in one pass: a COUNT is one addition for the run, and a SUM / AVG's
// expression (vx::NumProg: typed columns, constants, - + - * /) is computed per row in registers from the
// columns decoded whole (vx::decoded_values) and added into the group's total in row order - the order, and the
// operations, np_eval and the fold use, so the totals are the same bits. No morsel, no gather, no lane between the
// expression and the fold.
//
// Covered: leaves that are typed columns with no NULL and non-NULL constants; Neg, Add, Sub, Mul, and Div (a zero
// divisor makes the row NULL: skipped, as np_eval's null flag does); at most 14 instructions. Anything else has no
// code and the fused loop runs as before. AIOS_SQL_PIPE_JIT=0 turns it off; AIOS_SQL_PIPE_CHECK=1 computes every run
// through np_eval as well and compares bit for bit (a mismatch aborts). x86-64 SysV only.
namespace vp {
namespace vjit {

struct PipeCode {
  uint8_t* mem = nullptr;
  size_t bytes = 0;
  using Fn = void (*)(const uint32_t* rows, uint64_t n, uint64_t g);
  Fn fn = nullptr;
  ~PipeCode() { if (mem) munmap(mem, bytes); }
};

inline std::shared_ptr<PipeCode> pipe_fold_build(const std::vector<PipeFoldAgg>& aggs, std::string* why) {
  Asm a;
  // rdi rows, rsi n, rdx g -> r12 rows, r13 n, r14 g; r15 the run's counted rows; rcx the row index; rdx the row id;
  // rax / rbx scratch; xmm0..13 the program's instructions, xmm14 scratch, xmm15 the group's running total
  a.push(RBX); a.push(R12); a.push(R13); a.push(R14); a.push(R15);
  a.mov(R12, RDI);
  a.mov(R13, RSI);
  a.mov(R14, RDX);
  // An array's base read through its field in the spec (PipeFoldAgg lives in the caller's vector, which does not
  // move): the arrays grow as groups appear, and the caller updates the fields.
  auto base_of = [&](Reg d, const void* field) { a.mov_imm(d, (uint64_t)(uintptr_t)field); a.load(d, d, 0); };
  auto add_cnt = [&](uint64_t* const* cnt, Reg by) {   // cnt[g] += by
    base_of(RAX, cnt);
    a.load_idx8(RBX, RAX, R14);
    a.add(RBX, by);
    a.store_idx8(RAX, R14, RBX);
  };
  for (const PipeFoldAgg& g : aggs) {
    if (g.kind == 0) { add_cnt(&g.cnt, R13); continue; }
    if (g.kind == 3) {
      // SUM / AVG of exact arithmetic (INT and fixed-scale DECIMAL columns, + - *): the evaluator's exact value
      // per row as a scaled integer x at scale S (every intermediate within 2^53, checked when the program was
      // made: plain 64-bit arithmetic is exact), then what acc_row does with it - num[g] += x / 10^S (the double
      // its text parses to: both exact, so the division rounds as strtod does), cnt[g] += 1, and while exact[g]:
      // escale[g] / dscale[g] to S, etot[g] (128 bits) += x, an overflow ending exactness.
      const size_t K = g.xprog.size();
      if (K == 0 || K > 16) { if (why) *why = "program size"; return nullptr; }
      const int32_t frame = (int32_t)(8 * ((K + 1) & ~(size_t)1));
      a.sub_rsp(frame);
      base_of(RAX, &g.exact);
      a.loadb_idx(R11, RAX, R14);                      // R11: still exact
      a.test(R11, R11);
      const size_t inexact = a.jcc(CcE);
      a.mov_imm(RCX, (uint64_t)g.S);
      base_of(RAX, &g.escale);
      a.store32_idx4(RAX, R14, RCX);                   // escale: 0 (nothing added yet) or S - S either way
      base_of(RAX, &g.dscale);
      a.load32_idx4(RDX, RAX, R14);
      a.cmp(RDX, RCX);
      const size_t ds_ok = a.jcc(CcGE);
      a.store32_idx4(RAX, R14, RCX);
      a.patch_here(ds_ok);
      a.patch_here(inexact);
      base_of(R10, &g.etot);
      a.mov(RAX, R14);
      a.shl_imm(RAX, 4);
      a.add(R10, RAX);
      a.load(R8, R10, 0);
      a.load(R9, R10, 8);
      base_of(RAX, &g.num);
      a.movsd_load_idx8((Reg)15, RAX, R14);
      uint64_t pbits;
      std::memcpy(&pbits, &g.p10, 8);
      a.mov_imm(RAX, pbits);
      a.movq_xr((Reg)14, RAX);
      a.xor_(RCX, RCX);
      const size_t xtop = a.here();
      a.cmp(RCX, R13);
      const size_t xdone = a.jcc(CcAE);
      a.load32_idx4(RSI, R12, RCX);                    // the row
      auto scaled = [&](Reg d, uint32_t x, int to) {   // instruction x's value at scale `to`
        a.load(d, RSP, (int32_t)(8 * x));
        int up = to - g.xprog[x].scale;
        if (up > 0) {
          uint64_t m = 1;
          while (up-- > 0) m *= 10;
          a.mov_imm(RBX, m);
          a.imul(d, RBX);
        }
      };
      for (size_t x = 0; x < K; ++x) {
        const PipeXIns& in = g.xprog[x];
        switch (in.op) {
          case PipeXIns::Leaf:
            a.mov_imm(RAX, (uint64_t)(uintptr_t)in.col);
            a.load_idx8(RDX, RAX, RSI);
            break;
          case PipeXIns::Add: scaled(RDX, in.a, in.scale); scaled(RAX, in.b, in.scale); a.add(RDX, RAX); break;
          case PipeXIns::Sub: scaled(RDX, in.a, in.scale); scaled(RAX, in.b, in.scale); a.sub(RDX, RAX); break;
          case PipeXIns::Mul: a.load(RDX, RSP, (int32_t)(8 * in.a)); a.load(RAX, RSP, (int32_t)(8 * in.b)); a.imul(RDX, RAX); break;
        }
        a.store(RSP, (int32_t)(8 * x), RDX);
      }
      // x in RDX
      a.cvtsi2sd((Reg)0, RDX);
      a.divsd((Reg)0, (Reg)14);
      a.addsd((Reg)15, (Reg)0);
      a.test(R11, R11);
      const size_t no_exact = a.jcc(CcE);
      a.mov(RAX, RDX);
      a.sar_imm(RAX, 63);
      a.add(R8, RDX);
      a.adc(R9, RAX);
      const size_t no_ovf = a.jcc(CcNO);
      a.xor_(R11, R11);                                // the 128-bit total overflowed: no longer exact (it keeps the wrapped sum, as __builtin_add_overflow leaves it)
      a.patch_here(no_ovf);
      a.patch_here(no_exact);
      a.inc(RCX);
      a.patch(a.jmp(), xtop);
      a.patch_here(xdone);
      a.store(R10, 0, R8);
      a.store(R10, 8, R9);
      base_of(RAX, &g.num);
      a.movsd_store_idx8(RAX, R14, (Reg)15);
      add_cnt(&g.cnt, R13);
      base_of(RAX, &g.exact);
      a.storeb_idx(RAX, R14, R11);                     // 0 if it overflowed (or was 0)
      a.add_rsp(frame);
      continue;
    }
    if (g.kind == 2) {
      // SUM / AVG of a typed INT column with no NULL: per row, isum[g] (128 bits) += v, num[g] += (double)v - the
      // fold's own fast path; cnt[g] += the run
      base_of(R10, &g.isum);
      a.mov(RAX, R14);
      a.shl_imm(RAX, 4);
      a.add(R10, RAX);
      a.load(R8, R10, 0);
      a.load(R9, R10, 8);
      base_of(RAX, &g.num);
      a.movsd_load_idx8((Reg)15, RAX, R14);
      a.xor_(RCX, RCX);
      const size_t itop = a.here();
      a.cmp(RCX, R13);
      const size_t idone = a.jcc(CcAE);
      a.load32_idx4(RDX, R12, RCX);
      a.mov_imm(RAX, (uint64_t)(uintptr_t)g.ints);
      a.load_idx8(RDX, RAX, RDX);
      a.mov(RAX, RDX);
      a.sar_imm(RAX, 63);
      a.add(R8, RDX);
      a.adc(R9, RAX);
      a.cvtsi2sd((Reg)0, RDX);
      a.addsd((Reg)15, (Reg)0);
      a.inc(RCX);
      a.patch(a.jmp(), itop);
      a.patch_here(idone);
      a.store(R10, 0, R8);
      a.store(R10, 8, R9);
      base_of(RAX, &g.num);
      a.movsd_store_idx8(RAX, R14, (Reg)15);
      add_cnt(&g.cnt, R13);
      continue;
    }
    const size_t K = g.prog.size();
    if (K == 0 || K > 14) { if (why) *why = "program size"; return nullptr; }
    base_of(RAX, &g.num);
    a.movsd_load_idx8((Reg)15, RAX, R14);
    a.xor_(R15, R15);
    a.xor_(RCX, RCX);
    const size_t top = a.here();
    a.cmp(RCX, R13);
    const size_t to_done = a.jcc(CcAE);
    a.load32_idx4(RDX, R12, RCX);
    std::vector<size_t> to_next;                       // a NULL row: skipped
    for (size_t x = 0; x < K; ++x) {
      const PipeIns& in = g.prog[x];
      const Reg d = (Reg)x;
      switch (in.op) {
        case PipeIns::Leaf:
          a.mov_imm(RAX, (uint64_t)(uintptr_t)in.col);
          a.movsd_load_idx8(d, RAX, RDX);
          break;
        case PipeIns::Konst: {
          uint64_t bits;
          std::memcpy(&bits, &in.k, 8);
          a.mov_imm(RAX, bits);
          a.movq_xr(d, RAX);
          break;
        }
        case PipeIns::Neg:
          a.movsd(d, (Reg)in.a);
          a.mov_imm(RAX, 0x8000000000000000ull);
          a.movq_xr((Reg)14, RAX);
          a.xorpd(d, (Reg)14);
          break;
        case PipeIns::Add: a.movsd(d, (Reg)in.a); a.addsd(d, (Reg)in.b); break;
        case PipeIns::Sub: a.movsd(d, (Reg)in.a); a.subsd(d, (Reg)in.b); break;
        case PipeIns::Mul: a.movsd(d, (Reg)in.a); a.mulsd(d, (Reg)in.b); break;
        case PipeIns::Div: {
          // b == 0 (either zero; not NaN, which ucomisd reports unordered: PF set) - the row is NULL
          a.xorpd((Reg)14, (Reg)14);
          a.ucomisd((Reg)in.b, (Reg)14);
          const size_t nan = a.jcc(CcP);
          to_next.push_back(a.jcc(CcE));
          a.patch_here(nan);
          a.movsd(d, (Reg)in.a);
          a.divsd(d, (Reg)in.b);
          break;
        }
        default:
          if (why) *why = "instruction";
          return nullptr;
      }
    }
    a.addsd((Reg)15, (Reg)(K - 1));
    a.inc(R15);
    const size_t next = a.here();
    for (size_t at : to_next) a.patch(at, next);
    a.inc(RCX);
    a.patch(a.jmp(), top);
    a.patch_here(to_done);
    base_of(RAX, &g.num);
    a.movsd_store_idx8(RAX, R14, (Reg)15);
    add_cnt(&g.cnt, R15);
    // a row added: the total is approximate from here (the fold's exact[g] = 0)
    a.test(R15, R15);
    const size_t none = a.jcc(CcE);
    base_of(RAX, &g.exact);
    a.xor_(RBX, RBX);
    a.storeb_idx(RAX, R14, RBX);
    a.patch_here(none);
  }
  a.pop(R15); a.pop(R14); a.pop(R13); a.pop(R12); a.pop(RBX);
  a.ret();
  auto code = std::make_shared<PipeCode>();
  code->bytes = a.buf.size();
  void* p = mmap(nullptr, code->bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) { if (why) *why = "mmap"; return nullptr; }
  std::memcpy(p, a.buf.data(), code->bytes);
  if (mprotect(p, code->bytes, PROT_READ | PROT_EXEC) != 0) { munmap(p, code->bytes); if (why) *why = "mprotect"; return nullptr; }
  code->mem = (uint8_t*)p;
  code->fn = reinterpret_cast<PipeCode::Fn>(p);
  return code;
}

// The fused loop over the root rows itself (3c.2, second part): for each root row its unique partners (dense
// integer maps over the parent key decoded whole), its run on the one-to-many link (the CSR map), its key (two
// parts packed as star_aggregate packs them: a dictionary key's canonical id, an integer key offset by its least
// value), the probe into star_aggregate's own open-addressing table (the same hash, the same slots), and the run's
// fold (the native fold above, called). A key not in the table: spec.new_group (C++) makes the group in that slot,
// as key_group does. A root row with no partner or no run makes no row and no group, as before.
// Env: +0 total rows (added to), +8 root rows, +16 their count, +24 the new_group context, +32 per root row its
// group (AIOS_SQL_PIPE_CHECK; null otherwise), +40 / +48 / +56 / +64 the key table's groups, ids, mask and shift
// (new_group updates them when it grows the table).
inline std::shared_ptr<PipeCode> pipe_root_build(const PipeRootSpec& sp, std::string* why) {
  // keys: one or two packed into the table, one dictionary key's slots, or none (one group: slot 0 of sp.slots)
  if (sp.keys.size() > 2 || (sp.keys.empty() && !sp.slots) || (sp.slots && sp.keys.size() == 1 && !sp.keys[0].dict) ||
      (sp.slots && sp.keys.size() > 1)) { if (why) *why = "keys"; return nullptr; }
  Asm a;
  a.push(RBX); a.push(RBP); a.push(R12); a.push(R13); a.push(R14); a.push(R15);
  a.sub_rsp(40);                                     // parts[8] (uint32) at [rsp]; 6 pushes + 40: calls aligned
  a.mov(R14, RDI);
  a.load(R12, R14, 8);
  a.load(R13, R14, 16);
  a.xor_(RBX, RBX);
  const size_t top = a.here();
  a.cmp(RBX, R13);
  const size_t to_end = a.jcc(CcAE);
  std::vector<size_t> to_next;                       // no row for this root row
  a.load32_idx4(RAX, R12, RBX);
  a.store32(RSP, (int32_t)(4 * sp.root), RAX);
  auto load_part = [&](Reg d, uint32_t src) { a.load32(d, RSP, (int32_t)(4 * src)); };
  for (const PipeRootLink& L : sp.links) {
    load_part(RAX, L.parent);
    a.mov_imm(RCX, (uint64_t)(uintptr_t)L.pkeys);
    a.load_idx8(RDX, RCX, RAX);                      // the parent's key
    a.mov_imm(RCX, (uint64_t)L.lo);
    a.sub(RDX, RCX);
    a.mov_imm(RCX, L.dense_n);
    a.cmp(RDX, RCX);
    to_next.push_back(a.jcc(CcAE));                  // outside the map (unsigned: below lo too): no partner
    a.mov_imm(RCX, (uint64_t)(uintptr_t)L.dense);
    a.load32_idx4(RAX, RCX, RDX);
    a.mov_imm(RCX, 0xFFFFFFFFull);
    a.cmp(RAX, RCX);
    to_next.push_back(a.jcc(CcE));                   // no partner: an inner join drops the row
    a.store32(RSP, (int32_t)(4 * L.child), RAX);
  }
  const PipeRootMulti& M = sp.multi;
  if (!sp.has_multi) {
    // no one-to-many link: the run is the root row itself (index RBX into the root rows)
    a.mov_imm(RBP, 1);
    a.mov(R15, RBX);
  } else {
  load_part(RAX, M.parent);
  a.mov_imm(RCX, (uint64_t)(uintptr_t)M.pkeys);
  a.load_idx8(RDX, RCX, RAX);
  a.mov_imm(RCX, (uint64_t)M.lo);
  a.sub(RDX, RCX);                                   // k = key - lo
  a.mov_imm(RCX, M.off_n - 1);
  a.cmp(RDX, RCX);
  to_next.push_back(a.jcc(CcAE));                    // key < lo, or k + 1 >= off.size(): no run
  a.mov_imm(RCX, (uint64_t)(uintptr_t)M.off);
  a.load32_idx4(RSI, RCX, RDX);                      // b = off[k]
  a.mov(R8, RDX);
  a.inc(R8);
  a.load32_idx4(RDI, RCX, R8);                       // e = off[k + 1]
  a.cmp(RDI, RSI);
  to_next.push_back(a.jcc(CcE));                     // an empty run: an inner join, no row
  a.sub(RDI, RSI);
  a.mov(RBP, RDI);                                   // the run's length
  a.mov(R15, RSI);                                   // its start
  a.mov_imm(RCX, (uint64_t)(uintptr_t)M.rows);
  a.load32_idx4(RAX, RCX, R15);
  a.store32(RSP, (int32_t)(4 * M.child), RAX);       // the group's first row's many-side row
  }
  // the key: R8
  const size_t ng = sp.keys.size();
  if (ng == 0) a.xor_(R8, R8);
  for (size_t k = 0; k < ng; ++k) {
    const PipeRootKey& K = sp.keys[k];
    load_part(RAX, K.src);
    if (K.dict) {
      a.mov_imm(RCX, (uint64_t)(uintptr_t)K.codes);
      a.load32_idx4(RDX, RCX, RAX);                  // code
      a.mov_imm(RCX, K.canon_n);
      a.cmp(RDX, RCX);
      const size_t in_range = a.jcc(CcB);
      a.mov_imm(RDX, sp.slots ? K.canon_n : 0xFFFFFFFEull);   // a code past the canonical table: key_group's NULL slot / 0xFFFFFFFE
      const size_t joined = a.jmp();
      a.patch_here(in_range);
      a.mov_imm(RCX, (uint64_t)(uintptr_t)K.canon);
      a.load32_idx4(RDX, RCX, RDX);
      a.patch_here(joined);
    } else {
      a.mov_imm(RCX, (uint64_t)(uintptr_t)K.ints);
      a.load_idx8(RDX, RCX, RAX);
      if (ng == 2) { a.mov_imm(RCX, (uint64_t)K.ilo); a.sub(RDX, RCX); }
    }
    if (ng == 1) a.mov(R8, RDX);
    else if (k == 0) { a.shl_imm(RDX, 32); a.mov(R8, RDX); }
    else { a.mov_imm(RCX, 0xFFFFFFFFull); a.and_(RDX, RCX); a.or_(R8, RDX); }
  }
  std::vector<size_t> to_found_v;
  if (sp.slots) {
    // one dictionary key: its slot (the canonical id) holds the group, or -1
    a.mov(RAX, R8);
    a.mov_imm(R9, (uint64_t)(uintptr_t)sp.slots);
    a.load32_idx4(RDX, R9, RAX);
    a.mov_imm(RCX, 0xFFFFFFFFull);
    a.cmp(RDX, RCX);
    to_found_v.push_back(a.jcc(CcNE));
    a.load(RDI, R14, 24);                            // new_group(ctx, parts, id, slot = id)
    a.mov(RSI, RSP);
    a.mov(RDX, R8);
    a.mov(RCX, R8);
    a.mov_imm(RAX, (uint64_t)(uintptr_t)sp.new_group);
    a.call(RAX);
    a.mov(RDX, RAX);
    to_found_v.push_back(a.jmp());
  }
  // the probe: h = (id * C) >> shift & mask, linear
  // (the table, its mask and shift read from the env each row: new_group grows it, as key_group does)
  a.mov(RAX, R8);
  a.mov_imm(RCX, 0x9E3779B97F4A7C15ull);
  a.imul(RAX, RCX);
  a.load(RCX, R14, 64);
  a.shr_cl(RAX);
  a.load(R11, R14, 56);
  a.and_(RAX, R11);
  a.load(R9, R14, 40);
  a.load(R10, R14, 48);
  const size_t probe = a.here();
  a.load32_idx4(RDX, R9, RAX);
  a.mov_imm(RCX, 0xFFFFFFFFull);
  a.cmp(RDX, RCX);
  const size_t to_empty = a.jcc(CcE);
  a.load_idx8(RCX, R10, RAX);
  a.cmp(RCX, R8);
  const size_t to_found = a.jcc(CcE);
  a.inc(RAX);
  a.and_(RAX, R11);
  a.patch(a.jmp(), probe);
  a.patch_here(to_empty);
  // a new group: new_group(ctx, parts, id, slot)
  a.load(RDI, R14, 24);
  a.mov(RSI, RSP);
  a.mov(RDX, R8);
  a.mov(RCX, RAX);
  a.mov_imm(RAX, (uint64_t)(uintptr_t)sp.new_group);
  a.call(RAX);
  a.mov(RDX, RAX);
  a.patch_here(to_found);
  for (size_t at : to_found_v) a.patch_here(at);
  // record (check mode), count, fold
  a.load(RCX, R14, 32);
  a.test(RCX, RCX);
  const size_t no_rec = a.jcc(CcE);
  a.store32_idx4(RCX, RBX, RDX);
  a.patch_here(no_rec);
  a.load(RCX, R14, 0);
  a.add(RCX, RBP);
  a.store(R14, 0, RCX);
  if (sp.has_multi) a.mov_imm(RDI, (uint64_t)(uintptr_t)M.rows);
  else a.mov(RDI, R12);                              // the root rows: the run is root row RBX
  a.mov(RAX, R15);
  a.shl_imm(RAX, 2);
  a.add(RDI, RAX);                                   // rows + b
  a.mov(RSI, RBP);
  a.mov_imm(RAX, (uint64_t)(uintptr_t)sp.fold_fn);
  a.call(RAX);
  const size_t next = a.here();
  for (size_t at : to_next) a.patch(at, next);
  a.inc(RBX);
  a.patch(a.jmp(), top);
  a.patch_here(to_end);
  a.add_rsp(40);
  a.pop(R15); a.pop(R14); a.pop(R13); a.pop(R12); a.pop(RBP); a.pop(RBX);
  a.ret();
  auto code = std::make_shared<PipeCode>();
  code->bytes = a.buf.size();
  void* p = mmap(nullptr, code->bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) { if (why) *why = "mmap"; return nullptr; }
  std::memcpy(p, a.buf.data(), code->bytes);
  if (mprotect(p, code->bytes, PROT_READ | PROT_EXEC) != 0) { munmap(p, code->bytes); if (why) *why = "mprotect"; return nullptr; }
  code->mem = (uint8_t*)p;
  code->fn = reinterpret_cast<PipeCode::Fn>(p);
  return code;
}

}  // namespace vjit
}  // namespace vp
