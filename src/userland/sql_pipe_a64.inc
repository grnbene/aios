// Native code for the fused star join aggregate on AArch64 (AAPCS64): sql_pipe_x64.inc's fold and root loop
// instruction for instruction - the same coverage (COUNT; SUM / AVG of a NumProg of typed columns and constants
// with Neg / Add / Sub / Mul / Div, at most 14 instructions; of a typed INT or DECIMAL column; of exact scaled-integer
// arithmetic, at most 16), the same order of additions into the group's totals, the same key packing, hash and
// probe of star_aggregate's table, the same new_group and fold calls. AIOS_SQL_PIPE_JIT=0 turns it off;
// AIOS_SQL_PIPE_CHECK=1 compares every run with np_eval bit for bit. Uses sql_vjit_a64.inc's Asm and a64_install.
namespace vp {
namespace vjit {

struct PipeCode {
  uint8_t* mem = nullptr;
  size_t bytes = 0;
  using Fn = void (*)(const uint32_t* rows, uint64_t n, uint64_t g);
  Fn fn = nullptr;
  ~PipeCode() { if (mem) munmap(mem, bytes); }
};

// The fold: x0 rows, x1 n, x2 g. A leaf function using only caller-saved registers: x9 rows, x10 n, x11 g, x12 the
// run's counted rows, x13 the row index, x0 / x1 scratch, x2 the row id or value, x3 the row (exact arithmetic),
// x4 / x5 a 128-bit total, x6 its address, x7 still-exact (or the rows counted), x8 a sign; d16..d29 the program's
// instructions, d30 scratch (10^S), d31 the group's running total, d0 a row's double.
inline std::shared_ptr<PipeCode> pipe_fold_build(const std::vector<PipeFoldAgg>& aggs, std::string* why) {
  Asm a;
  a.mov(X9, X0);
  a.mov(X10, X1);
  a.mov(X11, X2);
  // An array's base read through its field in the spec (PipeFoldAgg lives in the caller's vector, which does not
  // move): the arrays grow as groups appear, and the caller updates the fields.
  auto base_of = [&](R d, const void* field) { a.mov_imm(d, (uint64_t)(uintptr_t)field); a.ldr(d, d, 0); };
  auto add_cnt = [&](uint64_t* const* cnt, R by) {   // cnt[g] += by
    base_of(X0, cnt);
    a.ldr_x8(X1, X0, X11);
    a.add(X1, X1, by);
    a.str_x8(X1, X0, X11);
  };
  constexpr R D0 = 0, DT = 30, DS = 31;   // a row's double, 10^S / scratch, the running total
  auto dreg = [](uint32_t x) -> R { return (R)(16 + x); };
  for (const PipeFoldAgg& g : aggs) {
    if (g.kind == 0) { add_cnt(&g.cnt, X10); continue; }
    if (g.kind == 3) {
      // SUM / AVG of exact arithmetic: sql_pipe_x64.inc's kind 3, step for step
      const size_t K = g.xprog.size();
      if (K == 0 || K > 16) { if (why) *why = "program size"; return nullptr; }
      const uint32_t frame = (uint32_t)((8 * K + 15) & ~(size_t)15);
      a.sub_sp(frame);
      base_of(X0, &g.exact);
      a.ldrb_x(X7, X0, X11);                         // x7: still exact
      const size_t inexact = a.cbz(X7);
      a.mov_imm(X1, (uint64_t)g.S);
      base_of(X0, &g.escale);
      a.str_w4(X1, X0, X11);                         // escale: 0 (nothing added yet) or S - S either way
      base_of(X0, &g.dscale);
      a.ldr_w4(X2, X0, X11);
      a.cmp(X2, X1);
      const size_t ds_ok = a.bcond(GE);
      a.str_w4(X1, X0, X11);
      a.patch_here(ds_ok);
      a.patch_here(inexact);
      base_of(X6, &g.etot);
      a.add_lsl(X6, X6, X11, 4);
      a.ldr(X4, X6, 0);
      a.ldr(X5, X6, 8);
      base_of(X0, &g.num);
      a.ldr_d8(DS, X0, X11);
      uint64_t pbits;
      std::memcpy(&pbits, &g.p10, 8);
      a.mov_imm(X0, pbits);
      a.fmov_dx(DT, X0);
      a.mov_imm(X13, 0);
      const size_t xtop = a.here();
      a.cmp(X13, X10);
      const size_t xdone = a.bcond(HS);
      a.ldr_w4(X3, X9, X13);                         // the row
      std::vector<size_t> to_xnext;
      auto scaled = [&](R d, uint32_t x, int to) {   // instruction x's value at scale `to`
        a.ldr(d, SPR, 8 * x);
        int up = to - g.xprog[x].scale;
        if (up > 0) {
          uint64_t m = 1;
          while (up-- > 0) m *= 10;
          a.mov_imm(X1, m);
          a.mul(d, d, X1);
        }
      };
      for (size_t x = 0; x < K; ++x) {
        const PipeXIns& in = g.xprog[x];
        switch (in.op) {
          case PipeXIns::Leaf:
            if (in.nul) {                            // a NULL operand: the row's value is NULL, skipped
              a.mov_imm(X0, (uint64_t)(uintptr_t)in.nul);
              a.ldrb_x(X1, X0, X3);
              to_xnext.push_back(a.cbnz(X1));
            }
            a.mov_imm(X0, (uint64_t)(uintptr_t)in.col);
            a.ldr_x8(X2, X0, X3);
            break;
          case PipeXIns::Add: scaled(X2, in.a, in.scale); scaled(X0, in.b, in.scale); a.add(X2, X2, X0); break;
          case PipeXIns::Sub: scaled(X2, in.a, in.scale); scaled(X0, in.b, in.scale); a.sub(X2, X2, X0); break;
          case PipeXIns::Mul: a.ldr(X2, SPR, 8 * in.a); a.ldr(X0, SPR, 8 * in.b); a.mul(X2, X2, X0); break;
        }
        a.str(X2, SPR, (uint32_t)(8 * x));
      }
      // x in x2
      a.scvtf(D0, X2);
      a.fdiv(D0, D0, DT);
      a.fadd(DS, DS, D0);
      const size_t no_exact = a.cbz(X7);
      a.asr_imm(X8, X2, 63);
      a.adds(X4, X4, X2);
      a.adcs(X5, X5, X8);
      const size_t no_ovf = a.bcond(VC);
      a.mov_imm(X7, 0);                              // the 128-bit total overflowed: no longer exact (it keeps the wrapped sum)
      a.patch_here(no_ovf);
      a.patch_here(no_exact);
      // the row's count: cnt[g] += 1 (not the run's length: a NULL row is not counted)
      base_of(X0, &g.cnt);
      a.ldr_x8(X1, X0, X11);
      a.add_imm(X1, X1, 1);
      a.str_x8(X1, X0, X11);
      for (size_t at : to_xnext) a.patch_here(at);
      a.add_imm(X13, X13, 1);
      a.patch(a.b(), xtop);
      a.patch_here(xdone);
      a.str(X4, X6, 0);
      a.str(X5, X6, 8);
      base_of(X0, &g.num);
      a.str_d8(DS, X0, X11);
      base_of(X0, &g.exact);
      a.strb_x(X7, X0, X11);                         // 0 if it overflowed (or was 0)
      a.add_sp(frame);
      continue;
    }
    if (g.kind == 2) {
      // SUM / AVG of a typed INT or fixed-scale DECIMAL column: per row not NULL, isum[g] (128 bits) += v,
      // num[g] += the cell's double ((double)v, or v / 10^s), cnt[g] += 1
      base_of(X6, &g.isum);
      a.add_lsl(X6, X6, X11, 4);
      a.ldr(X4, X6, 0);
      a.ldr(X5, X6, 8);
      base_of(X0, &g.num);
      a.ldr_d8(DS, X0, X11);
      if (g.dec) {
        uint64_t pbits;
        std::memcpy(&pbits, &g.p10, 8);
        a.mov_imm(X0, pbits);
        a.fmov_dx(DT, X0);
      }
      a.mov_imm(X7, 0);                              // the rows counted
      a.mov_imm(X13, 0);
      const size_t itop = a.here();
      a.cmp(X13, X10);
      const size_t idone = a.bcond(HS);
      a.ldr_w4(X2, X9, X13);
      size_t is_null = 0;
      if (g.nul) {
        a.mov_imm(X0, (uint64_t)(uintptr_t)g.nul);
        a.ldrb_x(X1, X0, X2);
        is_null = a.cbnz(X1);
      }
      a.mov_imm(X0, (uint64_t)(uintptr_t)g.ints);
      a.ldr_x8(X2, X0, X2);
      a.asr_imm(X8, X2, 63);
      a.adds(X4, X4, X2);
      a.adc(X5, X5, X8);
      a.scvtf(D0, X2);
      if (g.dec) a.fdiv(D0, D0, DT);
      a.fadd(DS, DS, D0);
      a.add_imm(X7, X7, 1);
      if (g.nul) a.patch_here(is_null);
      a.add_imm(X13, X13, 1);
      a.patch(a.b(), itop);
      a.patch_here(idone);
      a.str(X4, X6, 0);
      a.str(X5, X6, 8);
      base_of(X0, &g.num);
      a.str_d8(DS, X0, X11);
      add_cnt(&g.cnt, X7);
      continue;
    }
    const size_t K = g.prog.size();
    if (K == 0 || K > 14) { if (why) *why = "program size"; return nullptr; }
    base_of(X0, &g.num);
    a.ldr_d8(DS, X0, X11);
    a.mov_imm(X12, 0);
    a.mov_imm(X13, 0);
    const size_t top = a.here();
    a.cmp(X13, X10);
    const size_t to_done = a.bcond(HS);
    a.ldr_w4(X2, X9, X13);
    std::vector<size_t> to_next;                     // a NULL row: skipped
    for (size_t x = 0; x < K; ++x) {
      const PipeIns& in = g.prog[x];
      const R d = dreg((uint32_t)x);
      switch (in.op) {
        case PipeIns::Leaf:
          if (in.nul) {                              // a NULL cell: the row is NULL, skipped
            a.mov_imm(X0, (uint64_t)(uintptr_t)in.nul);
            a.ldrb_x(X1, X0, X2);
            to_next.push_back(a.cbnz(X1));
          }
          a.mov_imm(X0, (uint64_t)(uintptr_t)in.col);
          a.ldr_d8(d, X0, X2);
          break;
        case PipeIns::Konst: {
          uint64_t bits;
          std::memcpy(&bits, &in.k, 8);
          a.mov_imm(X0, bits);
          a.fmov_dx(d, X0);
          break;
        }
        case PipeIns::Neg:   // the sign bit flipped (x86's xorpd with the sign mask)
          a.fmov_xd(X0, dreg(in.a));
          a.flip_sign(X0, X0);
          a.fmov_dx(d, X0);
          break;
        case PipeIns::Add: a.fadd(d, dreg(in.a), dreg(in.b)); break;
        case PipeIns::Sub: a.fsub(d, dreg(in.a), dreg(in.b)); break;
        case PipeIns::Mul: a.fmul(d, dreg(in.a), dreg(in.b)); break;
        case PipeIns::Div: {
          // b == 0 (either zero; not NaN, which compares unordered: EQ clear) - the row is NULL
          a.fcmp0(dreg(in.b));
          to_next.push_back(a.bcond(EQ));
          a.fdiv(d, dreg(in.a), dreg(in.b));
          break;
        }
        default:
          if (why) *why = "instruction";
          return nullptr;
      }
    }
    a.fadd(DS, DS, dreg((uint32_t)(K - 1)));
    a.add_imm(X12, X12, 1);
    const size_t next = a.here();
    for (size_t at : to_next) a.patch(at, next);
    a.add_imm(X13, X13, 1);
    a.patch(a.b(), top);
    a.patch_here(to_done);
    base_of(X0, &g.num);
    a.str_d8(DS, X0, X11);
    add_cnt(&g.cnt, X12);
    // a row added: the total is approximate from here (the fold's exact[g] = 0)
    const size_t none = a.cbz(X12);
    base_of(X0, &g.exact);
    a.strb_x(XZR, X0, X11);
    a.patch_here(none);
  }
  a.ret();
  if (a.range_bad) { if (why) *why = "branch range"; return nullptr; }
  auto code = std::make_shared<PipeCode>();
  code->mem = a64_install(a.buf, &code->bytes, why);
  if (!code->mem) return nullptr;
  code->fn = reinterpret_cast<PipeCode::Fn>(code->mem);
  return code;
}

// The fused loop over the root rows (sql_pipe_x64.inc's pipe_root_build, step for step). x19 the root row index,
// x20 the run's length, x21 the root rows, x22 their count, x23 the env, x24 the run's start (callee-saved: the
// loop calls new_group and the fold); x8 the key; parts[] (uint32, the rows of each source) at [sp].
// Env: +0 total rows (added to), +8 root rows, +16 their count, +24 the new_group context, +32 per root row its
// group (AIOS_SQL_PIPE_CHECK; null otherwise), +40 / +48 / +56 / +64 the key table's groups, ids, mask and shift.
inline std::shared_ptr<PipeCode> pipe_root_build(const PipeRootSpec& sp, std::string* why) {
  if (sp.keys.size() > 2 || (sp.keys.empty() && !sp.slots) || (sp.slots && sp.keys.size() == 1 && !sp.keys[0].dict) ||
      (sp.slots && sp.keys.size() > 1)) { if (why) *why = "keys"; return nullptr; }
  Asm a;
  a.stp_pre16(X29, X30);
  a.add_imm(X29, SPR, 0);
  a.stp_pre16(X19, X20);
  a.stp_pre16(X21, X22);
  a.stp_pre16(X23, X24);
  a.sub_sp(48);                                      // parts[8] (uint32) at [sp], and room past it as x86's 40 bytes have
  a.mov(X23, X0);
  a.ldr(X21, X23, 8);
  a.ldr(X22, X23, 16);
  a.mov_imm(X19, 0);
  const size_t top = a.here();
  a.cmp(X19, X22);
  const size_t to_end = a.bcond(HS);
  std::vector<size_t> to_next;                       // no row for this root row
  a.ldr_w4(X0, X21, X19);
  a.str_w(X0, SPR, 4 * sp.root);
  auto load_part = [&](R d, uint32_t src) { a.ldr_w(d, SPR, 4 * src); };
  for (const PipeRootLink& L : sp.links) {
    load_part(X0, L.parent);
    a.mov_imm(X1, (uint64_t)(uintptr_t)L.pkeys);
    a.ldr_x8(X2, X1, X0);                            // the parent's key
    a.mov_imm(X1, (uint64_t)L.lo);
    a.sub(X2, X2, X1);
    a.mov_imm(X1, L.dense_n);
    a.cmp(X2, X1);
    to_next.push_back(a.bcond(HS));                  // outside the map (unsigned: below lo too): no partner
    a.mov_imm(X1, (uint64_t)(uintptr_t)L.dense);
    a.ldr_w4(X0, X1, X2);
    a.mov_imm(X1, 0xFFFFFFFFull);
    a.cmp(X0, X1);
    to_next.push_back(a.bcond(EQ));                  // no partner: an inner join drops the row
    a.str_w(X0, SPR, 4 * L.child);
  }
  const PipeRootMulti& M = sp.multi;
  if (!sp.has_multi) {
    // no one-to-many link: the run is the root row itself (index x19 into the root rows)
    a.mov_imm(X20, 1);
    a.mov(X24, X19);
  } else {
    load_part(X0, M.parent);
    a.mov_imm(X1, (uint64_t)(uintptr_t)M.pkeys);
    a.ldr_x8(X2, X1, X0);
    a.mov_imm(X1, (uint64_t)M.lo);
    a.sub(X2, X2, X1);                               // k = key - lo
    a.mov_imm(X1, M.off_n - 1);
    a.cmp(X2, X1);
    to_next.push_back(a.bcond(HS));                  // key < lo, or k + 1 >= off.size(): no run
    a.mov_imm(X1, (uint64_t)(uintptr_t)M.off);
    a.ldr_w4(X3, X1, X2);                            // b = off[k]
    a.add_imm(X8, X2, 1);
    a.ldr_w4(X4, X1, X8);                            // e = off[k + 1]
    a.cmp(X4, X3);
    to_next.push_back(a.bcond(EQ));                  // an empty run: an inner join, no row
    a.sub(X4, X4, X3);
    a.mov(X20, X4);                                  // the run's length
    a.mov(X24, X3);                                  // its start
    a.mov_imm(X1, (uint64_t)(uintptr_t)M.rows);
    a.ldr_w4(X0, X1, X24);
    a.str_w(X0, SPR, 4 * M.child);                   // the group's first row's many-side row
  }
  // the key: x8
  const size_t ng = sp.keys.size();
  if (ng == 0) a.mov_imm(X8, 0);
  for (size_t k = 0; k < ng; ++k) {
    const PipeRootKey& K = sp.keys[k];
    load_part(X0, K.src);
    if (K.dict) {
      a.mov_imm(X1, (uint64_t)(uintptr_t)K.codes);
      a.ldr_w4(X2, X1, X0);                          // code
      a.mov_imm(X1, K.canon_n);
      a.cmp(X2, X1);
      const size_t in_range = a.bcond(LO);
      a.mov_imm(X2, sp.slots ? K.canon_n : 0xFFFFFFFEull);   // a code past the canonical table: key_group's NULL slot / 0xFFFFFFFE
      const size_t joined = a.b();
      a.patch_here(in_range);
      a.mov_imm(X1, (uint64_t)(uintptr_t)K.canon);
      a.ldr_w4(X2, X1, X2);
      a.patch_here(joined);
    } else {
      a.mov_imm(X1, (uint64_t)(uintptr_t)K.ints);
      a.ldr_x8(X2, X1, X0);
      if (ng == 2) { a.mov_imm(X1, (uint64_t)K.ilo); a.sub(X2, X2, X1); }
    }
    if (ng == 1) a.mov(X8, X2);
    else if (k == 0) { a.lsl_imm(X2, X2, 32); a.mov(X8, X2); }
    else { a.mov_imm(X1, 0xFFFFFFFFull); a.and_(X2, X2, X1); a.orr(X8, X8, X2); }
  }
  std::vector<size_t> to_found_v;
  if (sp.slots) {
    // one dictionary key: its slot (the canonical id) holds the group, or -1
    a.mov_imm(X9, (uint64_t)(uintptr_t)sp.slots);
    a.ldr_w4(X2, X9, X8);
    a.mov_imm(X1, 0xFFFFFFFFull);
    a.cmp(X2, X1);
    to_found_v.push_back(a.bcond(NE));
    a.ldr(X0, X23, 24);                              // new_group(ctx, parts, id, slot = id)
    a.add_imm(X1, SPR, 0);
    a.mov(X2, X8);
    a.mov(X3, X8);
    a.call((const void*)sp.new_group);
    a.mov(X2, X0);
    to_found_v.push_back(a.b());
  }
  // the probe: h = (id * C) >> shift & mask, linear (the table, its mask and shift read from the env each row:
  // new_group grows it, as key_group does)
  a.mov_imm(X1, 0x9E3779B97F4A7C15ull);
  a.mul(X0, X8, X1);
  a.ldr(X1, X23, 64);
  a.lsrv(X0, X0, X1);
  a.ldr(X11, X23, 56);
  a.and_(X0, X0, X11);
  a.ldr(X9, X23, 40);
  a.ldr(X10, X23, 48);
  const size_t probe = a.here();
  a.ldr_w4(X2, X9, X0);
  a.mov_imm(X1, 0xFFFFFFFFull);
  a.cmp(X2, X1);
  const size_t to_empty = a.bcond(EQ);
  a.ldr_x8(X1, X10, X0);
  a.cmp(X1, X8);
  const size_t to_found = a.bcond(EQ);
  a.add_imm(X0, X0, 1);
  a.and_(X0, X0, X11);
  a.patch(a.b(), probe);
  a.patch_here(to_empty);
  // a new group: new_group(ctx, parts, id, slot)
  a.mov(X3, X0);
  a.ldr(X0, X23, 24);
  a.add_imm(X1, SPR, 0);
  a.mov(X2, X8);
  a.call((const void*)sp.new_group);
  a.mov(X2, X0);
  a.patch_here(to_found);
  for (size_t at : to_found_v) a.patch_here(at);
  // record (check mode), count, fold - the group in x2, the fold's third argument
  a.ldr(X1, X23, 32);
  const size_t no_rec = a.cbz(X1);
  a.str_w4(X2, X1, X19);
  a.patch_here(no_rec);
  a.ldr(X1, X23, 0);
  a.add(X1, X1, X20);
  a.str(X1, X23, 0);
  if (sp.has_multi) a.mov_imm(X0, (uint64_t)(uintptr_t)M.rows);
  else a.mov(X0, X21);                               // the root rows: the run is root row x19
  a.add_lsl(X0, X0, X24, 2);                         // rows + b
  a.mov(X1, X20);
  a.call(sp.fold_fn);
  const size_t next = a.here();
  for (size_t at : to_next) a.patch(at, next);
  a.add_imm(X19, X19, 1);
  a.patch(a.b(), top);
  a.patch_here(to_end);
  a.add_sp(48);
  a.ldp_post16(X23, X24);
  a.ldp_post16(X21, X22);
  a.ldp_post16(X19, X20);
  a.ldp_post16(X29, X30);
  a.ret();
  if (a.range_bad) { if (why) *why = "branch range"; return nullptr; }
  auto code = std::make_shared<PipeCode>();
  code->mem = a64_install(a.buf, &code->bytes, why);
  if (!code->mem) return nullptr;
  code->fn = reinterpret_cast<PipeCode::Fn>(code->mem);
  return code;
}

}  // namespace vjit
}  // namespace vp
