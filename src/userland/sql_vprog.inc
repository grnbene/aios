// Vectorized typed expression programs (vp): an expression over a statement's sources compiled once into lanes
// of int64 / double with a null byte per row, and evaluated over many joined rows at a time - an aggregate's
// argument over its group, a join's key over its probe rows, a select item over the output rows. The interpreter
// (eval_expr) makes a Val per node per row, about 90 ns each; a lane operation is a few ns a row.
//
// Only what is certain to give the interpreter's answer compiles: integers and doubles (no DECIMAL arithmetic,
// whose scale the interpreter carries through), comparisons, AND / OR / NOT, IS [NOT] NULL, CASE / IF / COALESCE /
// NULLIF / GREATEST / LEAST over one type, ABS / FLOOR / CEIL, SQRT / POWER and the trigonometry, CAST to SIGNED or
// DOUBLE, DIV, MOD, the bit operators, and `text_col = 'lit'` / `text_col IN ('a', 'b')` over a dictionary-coded
// column. Anything else - a subquery, a string function, an exact decimal in arithmetic, a DECIMAL column, an
// outer reference, an unknown function - leaves the expression to the interpreter. The one dynamic rule it keeps
// static: the interpreter divides two exact numbers as decimals, so `/` compiles only when an operand is a double
// by construction (a DOUBLE column, an approximate literal, a function of one). AIOS_SQL_VPROG=0 turns it off.
namespace vp {

// Str: a text lane. Its int64 holds a reference - a dictionary code of one of the program's text columns (the
// column's slot + 1 in the high 32 bits, the code below) or a literal (slot 0, the literal's index) - and the
// text is read through it only when compared or printed.
enum class Ty : uint8_t { I64, F64, Str };

struct Ins {
  enum Op : uint8_t {
    Konst, ColI, ColF, CodeI, CodeF, ColNull, StrIn, ColS,
    Neg, Add, Sub, Mul, Div, Mod, IDiv,
    Eq, Ne, Lt, Le, Gt, Ge, And, Or, Not, IsNull, NotNull,
    Case, Coalesce, NullIf, Greatest, Least, Abs, Floor, Ceil, Sqrt, Pow, Sin, Cos, Tan, Atan, Atan2, Radians,
    Degrees, CastI, CastF, BitAnd, BitOr, BitXor, Shl, Shr, BitNot,
    RoundI,  // a stored routine's INT parameter or result: the double rounded half away from zero (std::round)
    TupleIn,  // (a, b) IN (SELECT ..): the tuple of numbers looked up in the subquery's rows, hashed whole
    // CAST(x AS DECIMAL(p, s)) of a DOUBLE or an integer, read by the double arithmetic around it: each row's value
    // rounded as the interpreter's CAST rounds it (round_double_scaled), the lane the double that decimal reads as
    // (its digits over 10^s: the nearest double, as parse_number of its text). An exact decimal (dec): only a
    // double's arithmetic, a comparison or a CAST may read it, as a decimal literal's lane is read.
    CastDec,
    // ROUND(x) of a DOUBLE: the double the interpreter's ROUND gives (round_value: half away from zero on its 15
    // significant digits as written, a rounded zero unsigned), still a DOUBLE
    RoundF
  } op = Konst;
  Ty ty = Ty::I64;
  bool approx = false;    // the interpreter's Val here would be approximate (a DOUBLE)
  bool dec = false;       // an exact decimal literal with a fraction (0.5): a double lane, exact to the interpreter
  bool dyadic = false;    // dec: a value a double holds exactly (1.0, 0.5, 2.25 - a finite binary fraction)
  bool exact_dyadic = false;   // Add/Sub/Mul of such exact values: the result must stay exactly representable
  // The interpreter's value here may be an exact decimal at some rows and a double at others (GREATEST(0.0, d)):
  // the number is the lane's, the text may not be - so a tainted value is read only by what erases text (a CAST
  // to SIGNED, a comparison, a truth test), never printed.
  bool taint = false;
  bool uns = false;       // the bit operators' unsigned 64-bit result, printed as such
  // A DATE's day number (days since 1970-01-01), dlo..dhi the values it can hold: an integer lane at run time, but to
  // the compiler a date - compared with another date or moved by an INTERVAL of days, never read as a number (to
  // MariaDB a date in arithmetic is YYYYMMDD) and never a program's value (emit, compile).
  bool day = false;
  int64_t dlo = 0, dhi = 0;
  // CASE whose branches mix unsigned values with signed ones: the interpreter hands back each row's branch as it
  // is, so the lane is signed and a row whose unsigned branch holds a value past INT64_MAX (where the two would
  // print and compare differently) fails the program - the interpreter takes those rows.
  bool uns_check = false;
  bool has_uns = false, has_signed = false;   // compiling a CASE: the kinds of its value branches so far
  bool knull = false;
  double kf = 0;
  int64_t ki = 0;
  uint32_t src = 0;       // a column's source
  uint32_t slot = 0;      // ColS: the column's index in Prog::text_cols
  int dplaces = 0;        // CastDec: s
  int64_t dlim = 0;       // CastDec: 10^p, past which the interpreter clamps (the chunk is the interpreter's then)
  const ColumnStore* cs = nullptr;
  std::vector<uint32_t> args;
  std::shared_ptr<const RowInSet> set;   // TupleIn: the subquery's rows
  // CodeI / CodeF: the dictionary's entries as numbers, one per code, shared across the programs that read the
  // column (CodeTable); StrIn: 1 per code where the entry matches.
  // temporal: a date column's table over its codes (days past `base`, 1 << bits of them), not a dictionary's
  struct CodeTable { std::vector<int64_t> i; std::vector<double> f; std::vector<uint8_t> null; bool temporal = false; int64_t base = 0; int bits = 0; };
  std::shared_ptr<const CodeTable> codes;
  std::vector<uint8_t> code_null;
  const std::vector<int64_t>& code_i() const { return codes->i; }
  const std::vector<double>& code_f() const { return codes->f; }
  size_t code_count() const { return codes ? codes->null.size() : code_null.size(); }
  bool code_is_null(uint32_t k) const { return codes ? codes->null[k] != 0 : code_null[k] != 0; }
};

struct Prog {
  std::vector<Ins> ins;   // operands before the operators that read them; the last is the result
  std::vector<const ColumnStore*> text_cols;   // the text columns Str lanes refer to, by slot
  std::vector<std::string> lits;               // the text literals, by index
  std::string_view text_of(int64_t ref) const {
    const uint32_t slot = (uint32_t)((uint64_t)ref >> 32), idx = (uint32_t)ref;
    if (slot == 0) return idx < lits.size() ? std::string_view(lits[idx]) : std::string_view();
    return slot - 1 < text_cols.size() ? vx::dict_view(*text_cols[slot - 1], idx) : std::string_view();
  }
  // The literals the program was compiled with, and the division rule: a cached statement shape rebinds its
  // literals in place (shape_bind), and the program on the expression is then stale - program_for checks.
  std::string lit_sig;
  bool pg_division = false;
  // A stored function inlined: the programs generation it was compiled under (a CREATE OR REPLACE moves it).
  bool has_fn = false;
  uint64_t fn_gen = 0;
  const Ins& out() const { return ins.back(); }
  // The leaves the native code gathers itself and the column layout each was built for (LeafKind; 0: the lane
  // path), one per instruction; a layout that has moved since recompiles the program (valid).
  std::vector<uint32_t> leaf_kind;
  // Native code for the program's non-leaf instructions (sql_vjit_x64.inc), built once with the program; null
  // where none (disabled, another architecture, or an instruction the code does not cover).
  std::shared_ptr<void> jit;
  // The native code computes its loop-invariant instructions once a chunk when every source in hoist_srcs has
  // one row id across the chunk (a one-row table's leaves); hoisted says the code has such a preamble.
  bool hoisted = false;
  uint64_t hoist_srcs = 0;
  std::vector<uint8_t> inv;
};

// The leaves of a program: the instructions that read a column or a constant, which the interpreter's own
// vectorized readers decode into lanes before native code computes the rest.
inline bool vjit_leaf(Ins::Op op) {
  return op == Ins::Konst || op == Ins::ColI || op == Ins::ColF || op == Ins::CodeI || op == Ins::CodeF || op == Ins::ColNull ||
         op == Ins::StrIn || op == Ins::ColS;
}
// A leaf the native code gathers itself (sql_vjit_x64.inc), described from the column as it is when a chunk runs:
// the row's id from the source's row ids, then the value straight from a flat column - an int64 or double array,
// or 32-bit dictionary codes through the leaf's table. `kind` says which layout, 0 where the leaf keeps the lane
// path (a packed or run-length column); the code compares it with the one it was built for and bails otherwise.
struct LeafDesc {
  const void* data = nullptr;          // +0  int64_t* / double* / uint32_t* codes
  const uint8_t* nulls = nullptr;      // +8  the null bitmap
  uint64_t nulls_bytes = 0;            // +16 its bytes (0: no NULL in the column, nothing to test)
  uint64_t size = 0;                   // +24 the column's rows
  const void* table = nullptr;         // +32 CodeI: int64_t per code; CodeF: double per code
  const uint8_t* table_null = nullptr; // +40 CodeI / CodeF: 1 where the code's entry is NULL; StrIn: 1 where it matches
  uint64_t table_n = 0;                // +48 the table's codes
  uint64_t imm = 0;                    // +56 Konst: the value's bits; ColS: the slot's high word
  uint32_t src = 0;                    // +64 the source whose row ids to read
  uint32_t kind = 0;                   // +68 LeafKind
};
enum LeafKind : uint32_t { LeafLane = 0, LeafKonst = 1, LeafColNull = 2, LeafI64 = 3, LeafF64 = 4, LeafCodeI = 5, LeafCodeF = 6, LeafColS = 7, LeafStrIn = 8 };
inline uint32_t leaf_kind_of(const Ins& in) {
  switch (in.op) {
    case Ins::Konst: return LeafKonst;
    case Ins::ColNull: return in.cs ? LeafColNull : LeafLane;
    case Ins::ColI: return in.cs && in.cs->encoding == ColumnEncoding::TypedInt64 && in.cs->int64_data.size() >= in.cs->size() ? LeafI64 : LeafLane;
    case Ins::ColF: return in.cs && in.cs->encoding == ColumnEncoding::TypedDouble && in.cs->double_data.size() >= in.cs->size() ? LeafF64 : LeafLane;
    case Ins::CodeI: case Ins::CodeF: case Ins::ColS: case Ins::StrIn: {
      const ColumnStore* cs = in.cs;
      const bool flat = cs && cs->encoding != ColumnEncoding::RLE && cs->encoding != ColumnEncoding::Constant && !cs->seg_packed_ &&
                        cs->bit_width_ == 32 && cs->data32.size() >= cs->size();
      if (!flat) return LeafLane;
      return in.op == Ins::CodeI ? LeafCodeI : in.op == Ins::CodeF ? LeafCodeF : in.op == Ins::ColS ? LeafColS : LeafStrIn;
    }
    default: return LeafLane;
  }
}

std::shared_ptr<void> vjit_build_for(const Prog& P, std::string* why, bool* hoisted, uint64_t* hoist_srcs);   // sql_vjit_x64.inc; null elsewhere
// 0: the output lane is computed; 1: bail (an overflow or a non-finite double the interpreter would refuse);
// 2: a column's layout moved. hoist: 1 where the chunk's row ids allow the invariant preamble (Prog::hoist_srcs).
int vjit_run(const void* code, size_t n, const void* const* lane_i, const void* const* lane_f, const void* const* lane_n,
             int64_t* out_i, double* out_f, uint8_t* out_n, const LeafDesc* leaves, const size_t* const* rid, uint64_t hoist);
bool vjit_check_mode();

// Every literal of e in tree order, as it would compile: value text and kind.
inline void literal_sig(const SqlExpr* e, std::string* out, int depth = 0) {
  if (!e || depth > 64) return;
  if (e->op == ExprOp::Literal) {
    const Val& v = e->literal;
    *out += v.null ? "N" : v.numeric ? (v.approx ? "a" : "n") : "t";
    if (!v.null) { *out += v.str(); }
    *out += '|';
  }
  for (const auto& a : e->args) literal_sig(a.get(), out, depth + 1);
}

inline bool enabled() {
  static const bool on = [] { const char* v = std::getenv("AIOS_SQL_VPROG"); return !(v && *v == '0'); }();
  return on;
}

// An expression as text for the trace: its source text where the parser kept it, else its shape.
inline std::string expr_text(const SqlExpr* e, int depth = 0) {
  if (!e) return "?";
  // the tree itself, not source_text: a merged view's text is stitched from pieces and may name an alias
  if (depth > 12) return e->source_text.empty() ? ".." : e->source_text;
  if (e->op == ExprOp::Column) return e->name.empty() ? "<col?>" : e->qualifier.empty() ? e->name : e->qualifier + "." + e->name;
  if (e->op == ExprOp::Literal) {
    if (e->literal.null) return "NULL";
    const std::string& t = e->literal.str();
    return e->literal.numeric ? (t.empty() ? "<num " + std::to_string(e->literal.num) + ">" : t) : "'" + t + "'";
  }
  std::string s = e->op == ExprOp::Func ? e->name : "op" + std::to_string((int)e->op);
  s += "(";
  for (size_t i = 0; i < e->args.size(); ++i) { if (i) s += ","; s += expr_text(e->args[i].get(), depth + 1); }
  return s + ")";
}

struct Compiler {
  const EvalCtx& ctx;
  Prog& P;
  bool ok = true;
  int depth = 0;
  std::string why;   // the first reason the expression did not compile (for the trace)
  bool retry = false;   // it did not compile this time for a reason that may pass next time (a set not yet run)
  bool pg_div = tls_pg_division;   // the division rule in force: the statement's, or an inlined routine's own
  int inlined = 0;                 // stored functions being inlined, one inside another
  // An inlined function's parameter nodes (in its RETURN template) and the lanes its arguments compiled to.
  std::vector<std::pair<const SqlExpr*, uint32_t>> bound;

  // Common subexpression elimination (CSE): any instruction identical to one the program already has
  // (the same column read the same way, the same literal, or the same operator over the same input lanes)
  // is reused directly.
  static bool same_ins(const Ins& a, const Ins& b) {
    if (a.op != b.op || a.ty != b.ty || a.approx != b.approx || a.dec != b.dec || a.dyadic != b.dyadic || a.taint != b.taint ||
        a.uns != b.uns || a.day != b.day || a.knull != b.knull || a.ki != b.ki || std::memcmp(&a.kf, &b.kf, sizeof a.kf) != 0 || a.src != b.src ||
        a.slot != b.slot || a.cs != b.cs || a.codes.get() != b.codes.get() || a.set.get() != b.set.get() || a.dplaces != b.dplaces || a.dlim != b.dlim ||
        a.args.size() != b.args.size())
      return false;
    for (size_t i = 0; i < a.args.size(); ++i) if (a.args[i] != b.args[i]) return false;
    return a.code_null == b.code_null;
  }
  uint32_t emit(Ins in) {
    // A date reaches only a comparison with another date or IS [NOT] NULL (an INTERVAL's move makes its own day
    // instruction): anything else would read the day number as a number.
    if (!in.day) {
      bool any = false, all = !in.args.empty();
      for (uint32_t x : in.args) { const bool d = x < P.ins.size() && P.ins[x].day; any = any || d; all = all && d; }
      if (any) {
        const bool cmp = in.op == Ins::Eq || in.op == Ins::Ne || in.op == Ins::Lt || in.op == Ins::Le || in.op == Ins::Gt || in.op == Ins::Ge;
        const bool nul = in.op == Ins::IsNull || in.op == Ins::NotNull;
        if (!((cmp && all) || nul)) return fail("a date read as a number");
      }
    }
    for (uint32_t x = 0; x < P.ins.size(); ++x)
      if (same_ins(P.ins[x], in)) return x;
    P.ins.push_back(std::move(in));
    return (uint32_t)P.ins.size() - 1;
  }
  const SqlExpr* cur = nullptr;    // the node being compiled, named by a reason-less fail
  uint32_t fail(const std::string& reason = std::string()) {
    if (ok) why = !reason.empty() ? reason : "at " + expr_text(cur);
    ok = false;
    return 0;
  }
  const Ins& at(uint32_t i) const { return P.ins[i]; }

  // The source and column a column expression names in this scope (not an outer one).
  bool place(const SqlExpr* e, size_t* s, size_t* c) {
    if (!ctx.sources) return false;
    if (e->bound_depth == 0 && e->bound_src >= 0 && e->bound_col >= 0) {
      *s = (size_t)e->bound_src;
      *c = (size_t)e->bound_col;
    } else if (lookup_column_here(ctx, *e, s, c) != ColumnLookup::Found) {
      return false;
    }
    if (*s >= ctx.sources->size()) return false;
    const SqlTable* t = (*ctx.sources)[*s].table;
    if (!t || *c >= t->columns.size() || *c >= t->col_data.size()) return false;
    if (t->col_data[*c].size() != t->row_count()) return false;   // rows still in a delta buffer
    return true;
  }

  // A dictionary's entries as numbers, one per code: worked out once per column and dictionary size and kept on
  // this thread (CodeCache), since every program over the column asks for the same table and a result table's
  // 50,000-entry dictionary took milliseconds to parse per statement.
  struct CodeCache {
    struct Entry { uint64_t uid; size_t n; bool as_int; std::shared_ptr<const Ins::CodeTable> table; bool bad; int part = 0; };
    std::vector<Entry> entries;   // a few dozen live at once; cleared when many have gone stale
  };
  static CodeCache& code_cache() { thread_local CodeCache c; return c; }
  bool codes_of(const ColumnStore& cs, bool as_int, Ins* in) {
    if (cs.shared_dict) return false;
    const size_t n = vx::dict_size(cs);
    if (n == 0 || n > (1u << 20)) return false;
    CodeCache& cc = code_cache();
    for (const auto& e : cc.entries)
      if (e.uid == cs.uid.v && e.n == n && e.as_int == as_int && e.part == 0 && !(e.table && e.table->temporal)) {
        if (e.bad) return false;
        in->codes = e.table;
        return true;
      }
    auto t = std::make_shared<Ins::CodeTable>();
    t->null.assign(n, 0);
    if (as_int) t->i.assign(n, 0); else t->f.assign(n, 0);
    bool ok = true;
    for (size_t k = 0; ok && k < n; ++k) {
      const std::string& s = vx::dict_entry(cs, (uint32_t)k);
      if (sql_is_null(s)) { t->null[k] = 1; continue; }
      double d = 0;
      if (!parse_number(s, &d)) { ok = false; break; }
      if (as_int) {
        if (d != std::floor(d) || std::fabs(d) >= 9.2e18) { ok = false; break; }
        char* end = nullptr;
        errno = 0;
        const long long v = std::strtoll(s.c_str(), &end, 10);
        if (errno == ERANGE || !end || *end != '\0') { ok = false; break; }
        t->i[k] = v;
      } else {
        t->f[k] = d;
      }
    }
    if (cc.entries.size() >= 512) cc.entries.clear();
    cc.entries.push_back({cs.uid.v, n, as_int, ok ? t : nullptr, !ok});
    if (!ok) return false;
    in->codes = t;
    return true;
  }

  // YEAR / MONTH / DAY / QUARTER (part 1..4) of a dictionary-coded text column (a DATE column's text): each entry's
  // value worked out once by the interpreter's own rule (date_part_of_text), and the lane reads it by code (CodeI).
  uint32_t date_part(const SqlExpr* e, int part) {
    if (e->op != ExprOp::Column || is_user_var_name(e->name) || e->name.rfind("@@", 0) == 0) return fail("date part of an expression");
    size_t s = 0, c = 0;
    if (!place(e, &s, &c)) return fail("column not in this scope");
    const SqlTable* t = (*ctx.sources)[s].table;
    const SqlColumn& col = t->columns[c];
    const ColumnStore& cs = t->col_data[c];
    // A DATE column held as days (TemporalDays) whose span fits 16 bits is read per code, as the column kernels read
    // it (temporal_codes): code k is day base + k, its cell's text temporal_format's - the table works the date part
    // out of that text. A dictionary-coded text column: out of each entry.
    const bool days = col.temporal && cs.encoding == ColumnEncoding::TemporalDays && cs.bit_width_ <= 16 && cs.size() == t->row_count();
    if (part == 0 && !days) return fail("temporal column");
    if (!days && (col.type != SqlDataType::Text || cs.shared_dict || !vx::dict_coded(cs))) return fail("date part of a column not dictionary-coded or days");
    const size_t n = days ? (size_t)1 << cs.bit_width_ : vx::dict_size(cs);
    if (n == 0 || n > (1u << 20)) return fail("dictionary too large");
    Ins in;
    in.op = Ins::CodeI;
    in.ty = Ty::I64;
    in.src = (uint32_t)s;
    in.cs = &cs;
    CodeCache& cc = code_cache();
    if (part == 0) {   // the day itself: code k is day base + k
      in.day = true;
      in.dlo = cs.base_int64;
      in.dhi = cs.base_int64 + (int64_t)n - 1;
    }
    for (const auto& en : cc.entries)
      if (en.uid == cs.uid.v && en.n == n && en.part == part && en.table && en.table->temporal == days && (!days || en.table->base == cs.base_int64)) {
        in.codes = en.table;
        return emit(std::move(in));
      }
    auto tb = std::make_shared<Ins::CodeTable>();
    tb->null.assign(n, 0);
    tb->i.assign(n, 0);
    tb->temporal = days;
    tb->base = days ? cs.base_int64 : 0;
    tb->bits = days ? (int)cs.bit_width_ : 0;
    std::string text;
    for (size_t k = 0; k < n; ++k) {
      if (part == 0) { tb->i[k] = cs.base_int64 + (int64_t)k; continue; }
      if (days) sql_temporal_format(cs.base_int64 + (int64_t)k, false, text);
      const std::string& v = days ? text : vx::dict_entry(cs, (uint32_t)k);
      if ((!days && sql_is_null(v)) || !date_part_of_text(v, part, &tb->i[k])) tb->null[k] = 1;
    }
    if (cc.entries.size() >= 512) cc.entries.clear();
    CodeCache::Entry en{cs.uid.v, n, true, tb, false};
    en.part = part;
    cc.entries.push_back(std::move(en));
    in.codes = std::move(tb);
    return emit(std::move(in));
  }

  // A dictionary-coded text column as column() admits one (the same checks), or null: for text_class_pair.
  const ColumnStore* text_dict_column(const SqlExpr* e, size_t* src) {
    if (is_user_var_name(e->name) || e->name.rfind("@@", 0) == 0) return nullptr;
    size_t s = 0, c = 0;
    if (!place(e, &s, &c)) return nullptr;
    const SqlTable* t = (*ctx.sources)[s].table;
    const SqlColumn& col = t->columns[c];
    const ColumnStore& cs = t->col_data[c];
    if (col.temporal || col.type != SqlDataType::Text || cs.shared_dict || !vx::dict_coded(cs)) return nullptr;
    const size_t nd = vx::dict_size(cs);
    if (nd == 0 || nd > (1u << 20)) return nullptr;
    if (t->inferred_types) {   // a result table's "text" may hold numbers, which the interpreter reads as numbers
      for (size_t k = 0; k < nd; ++k) {
        const std::string& d = vx::dict_entry(cs, (uint32_t)k);
        double x = 0;
        if (!sql_is_null(d) && parse_number(d, &x)) return nullptr;
      }
    }
    *src = s;
    return &cs;
  }
  // text_col = text_col, both dictionary-coded: each dictionary entry's collation class is worked out once (two
  // entries are in one class exactly when pad_compare_ci finds them equal - they share a collate_key), and the rows
  // compare as integers: two CodeI leaves holding the classes, for an integer Eq / Ne (native). Each row's two
  // texts through pad_compare_ci was ~350 ns a row (a join's ON `fl.name = p.tex_name` over 56,000 candidates
  // in a SQLDoom frame: 20 ms). False (nothing emitted): not two such columns, and the caller compiles as before.
  bool text_class_pair(const SqlExpr* l, const SqlExpr* r, uint32_t* a, uint32_t* b) {
    size_t sl = 0, sr = 0;
    const ColumnStore* cl = text_dict_column(l, &sl);
    const ColumnStore* cr = cl ? text_dict_column(r, &sr) : nullptr;
    if (!cl || !cr) return false;
    std::unordered_map<std::string, int64_t> cls;
    auto table_of = [&](const ColumnStore& cs) {
      auto t = std::make_shared<Ins::CodeTable>();
      const size_t n = vx::dict_size(cs);
      t->null.assign(n, 0);
      t->i.assign(n, 0);
      for (size_t k = 0; k < n; ++k) {
        const std::string& d = vx::dict_entry(cs, (uint32_t)k);
        if (sql_is_null(d)) { t->null[k] = 1; continue; }
        const int64_t next = (int64_t)cls.size();
        t->i[k] = cls.emplace(collate_key(d), next).first->second;
      }
      return t;
    };
    auto leaf = [&](const ColumnStore& cs, size_t s, std::shared_ptr<const Ins::CodeTable> t) {
      Ins in; in.op = Ins::CodeI; in.ty = Ty::I64; in.src = (uint32_t)s; in.cs = &cs; in.codes = std::move(t);
      return emit(std::move(in));
    };
    *a = leaf(*cl, sl, table_of(*cl));
    *b = leaf(*cr, sr, table_of(*cr));
    return true;
  }

  uint32_t column(const SqlExpr* e) {
    if (is_user_var_name(e->name) || e->name.rfind("@@", 0) == 0) return fail("variable");
    size_t s = 0, c = 0;
    if (!place(e, &s, &c)) return fail("column not in this scope");
    const SqlTable* t = (*ctx.sources)[s].table;
    const SqlColumn& col = t->columns[c];
    const ColumnStore& cs = t->col_data[c];
    if (col.temporal) {
      // a DATE held as days in a 16-bit code space: its day numbers (date_part's part 0)
      if (cs.encoding == ColumnEncoding::TemporalDays && cs.bit_width_ <= 16 && cs.size() == t->row_count()) return date_part(e, 0);
      return fail("temporal column");
    }
    Ins in;
    in.src = (uint32_t)s;
    in.cs = &cs;
    // A DECIMAL of scale 0 holding integers reads as the INT's exact integers (val_cell of "5" either way):
    // SQLDoom's node_path_steps.depth is DECIMAL(65,0), made by CREATE TABLE .. AS from depth + 1. Only where the
    // store itself proves every value an integer of 64 bits: a typed integer encoding, or a dictionary whose every
    // entry is one (codes_of); past that, no program.
    const bool whole_decimal = col.type == SqlDataType::Decimal && col.scale == 0;
    if (col.type == SqlDataType::Int || whole_decimal) {
      in.ty = Ty::I64;
      if (vx::typed_integer(cs)) in.op = Ins::ColI;
      else if (vx::dict_coded(cs) && codes_of(cs, true, &in)) in.op = Ins::CodeI;
      else return fail(whole_decimal ? "DECIMAL column " + t->name + "." + col.name + " not held as integers"
                                     : "INT column not typed or dictionary-coded");
    } else if (col.type == SqlDataType::Real) {
      in.ty = Ty::F64;
      in.approx = true;
      if (vx::typed_numeric(cs)) in.op = Ins::ColF;
      else if (vx::dict_coded(cs) && codes_of(cs, false, &in)) in.op = Ins::CodeF;
      else return fail("DOUBLE column not typed or dictionary-coded");
    } else if (col.type == SqlDataType::Text) {
      // a dictionary-coded text column: the lane holds codes, the text is read when compared or printed
      if (cs.shared_dict || !vx::dict_coded(cs)) return fail("text column " + t->name + "." + col.name + " not dictionary-coded");
      if (t->inferred_types) {   // a result table's "text" may hold numbers, which the interpreter reads as numbers
        const size_t nd = vx::dict_size(cs);
        if (nd > (1u << 20)) return fail("dictionary too large");
        for (size_t k = 0; k < nd; ++k) {
          const std::string& d = vx::dict_entry(cs, (uint32_t)k);
          double x = 0;
          if (!sql_is_null(d) && parse_number(d, &x)) return fail("result column " + t->name + "." + col.name + " with numbers as text");
        }
      }
      in.ty = Ty::Str;
      in.op = Ins::ColS;
      uint32_t slot = (uint32_t)P.text_cols.size();
      for (uint32_t k = 0; k < P.text_cols.size(); ++k) if (P.text_cols[k] == &cs) { slot = k; break; }
      if (slot == P.text_cols.size()) P.text_cols.push_back(&cs);
      in.slot = slot;
    } else if (col.type == SqlDataType::Decimal) {
      // A DECIMAL with places: each value's nearest double, held as a decimal literal's lane is (dec) - only a
      // double's arithmetic, a comparison or a CAST may read it (numeric_pair keeps it out of exact arithmetic, and
      // no program's result is one), which is all MariaDB does with it there: DECIMAL beside a DOUBLE is the
      // DOUBLE's arithmetic on the decimal's double. Only where every value has at most 15 significant digits, so
      // the doubles keep the decimals' order and no two are equal. A camera's `SET @cy = 3.6` read through a CTE
      // is such a column, and every expression it met went row by row (a Pick Rush frame: 14 ms of 42).
      in.ty = Ty::F64;
      in.dec = true;
      in.dyadic = false;
      if (vx::dict_coded(cs) && !cs.shared_dict) {
        const size_t nd = vx::dict_size(cs);
        if (nd == 0 || nd > (1u << 20)) return fail("DECIMAL column " + t->name + "." + col.name + ": dictionary too large");
        for (size_t k = 0; k < nd; ++k) {
          const std::string& d = vx::dict_entry(cs, (uint32_t)k);
          if (sql_is_null(d)) continue;
          if (significant_digits(d) > 15) return fail("DECIMAL column " + t->name + "." + col.name + " finer than a double");
        }
        if (!codes_of(cs, false, &in)) return fail("DECIMAL column " + t->name + "." + col.name + " not numbers");
        in.op = Ins::CodeF;
      } else if (cs.encoding == ColumnEncoding::DecimalScaled && cs.bit_width_ < 63) {
        const double lo = (double)cs.base_int64, hi = (double)cs.base_int64 + std::ldexp(1.0, (int)cs.bit_width_);
        if (std::max(std::fabs(lo), std::fabs(hi)) >= 1e15) return fail("DECIMAL column " + t->name + "." + col.name + " finer than a double");
        in.op = Ins::ColF;
      } else {
        return fail("DECIMAL column " + t->name + "." + col.name);
      }
    } else {
      return fail("DECIMAL column " + t->name + "." + col.name);
    }
    return emit(std::move(in));
  }

  // The significant digits of a number's text (3.60 has 2, 0.0036 has 2, 120 has 3), or 99 when it is not plain
  // decimal digits.
  static int significant_digits(const std::string& d) {
    size_t i = 0;
    if (i < d.size() && (d[i] == '-' || d[i] == '+')) ++i;
    std::string digits;
    bool point = false;
    for (; i < d.size(); ++i) {
      const char c = d[i];
      if (c == '.' && !point) { point = true; continue; }
      if (c < '0' || c > '9') return 99;
      digits.push_back(c);
    }
    const size_t first = digits.find_first_not_of('0');
    if (first == std::string::npos) return 0;
    // trailing zeros do not count, in the fraction (3.60) or the whole part (1200 is 12 * 100): the order and the
    // distinctness of the doubles depend only on how many digits come between the first and last that are not 0
    const size_t last = digits.find_last_not_of('0');
    return (int)(last - first + 1);
  }

  uint32_t literal(const Val& v) {
    Ins in;
    in.op = Ins::Konst;
    if (v.null) { in.knull = true; in.ty = Ty::F64; in.approx = true; return emit(std::move(in)); }
    if (!v.numeric) {
      if (v.temporal) return fail("temporal literal");
      in.ty = Ty::Str;
      in.ki = (int64_t)P.lits.size();
      P.lits.push_back(v.str());
      return emit(std::move(in));
    }
    if (v.approx) { in.ty = Ty::F64; in.approx = true; in.kf = v.num; return emit(std::move(in)); }
    const std::string& t = v.str();
    if (t.find_first_of(".eE") == std::string::npos && t.size() <= 18) {
      in.ty = Ty::I64;
      in.ki = (int64_t)std::strtoll(t.c_str(), nullptr, 10);
      return emit(std::move(in));
    }
    if (v.num == std::floor(v.num) && std::fabs(v.num) < 9e15 && t.find_first_of("eE") == std::string::npos &&
        t.find('.') != std::string::npos) {
      // 2.0, 1.00: an exact decimal that is whole. As an operand of + - * with an integer the interpreter keeps
      // the scale (1 + 1.0 is 2.0): a decimal literal, not an integer.
    }
    in.ty = Ty::F64;
    in.dec = true;
    in.kf = v.num;
    {
      double f = std::fabs(v.num);
      int k = 0;
      while (f != std::floor(f) && k < 60) { f *= 2; ++k; }
      in.dyadic = f == std::floor(f) && std::fabs(v.num) < 9e15;
    }
    return emit(std::move(in));
  }

  // `text_col = 'lit'`, `text_col IN ('a', 'b')`: the codes that match, compared as = compares text.
  uint32_t str_in(const SqlExpr* col, const std::vector<const SqlExpr*>& lits) {
    size_t s = 0, c = 0;
    if (!place(col, &s, &c)) return fail();
    const SqlTable* t = (*ctx.sources)[s].table;
    const SqlColumn& sc = t->columns[c];
    const ColumnStore& cs = t->col_data[c];
    if (sc.type != SqlDataType::Text || sc.temporal || cs.shared_dict || !vx::dict_coded(cs)) return fail("text column not dictionary-coded");
    const size_t n = vx::dict_size(cs);
    if (n == 0 || n > (1u << 20)) return fail("dictionary too large");
    // a result table's "text" column may hold numbers, which = reads as numbers (12 = '12.0'): only one with none
    if (t->inferred_types) {
      for (size_t k = 0; k < n; ++k) {
        const std::string& d = vx::dict_entry(cs, (uint32_t)k);
        double x = 0;
        if (!sql_is_null(d) && parse_number(d, &x)) return fail("result column with numbers as text");
      }
      for (const SqlExpr* l : lits) { double x = 0; if (parse_number(l->literal.str(), &x)) return fail("numeric text literal"); }
    }
    Ins in;
    in.op = Ins::StrIn;
    in.ty = Ty::I64;
    in.src = (uint32_t)s;
    in.cs = &cs;
    in.code_null.assign(n, 0);
    for (size_t k = 0; k < n; ++k) {
      const std::string& d = vx::dict_entry(cs, (uint32_t)k);
      if (sql_is_null(d)) continue;
      for (const SqlExpr* l : lits)
        if (pad_compare_ci(d, l->literal.str()) == 0) { in.code_null[k] = 1; break; }
    }
    return emit(std::move(in));
  }

  uint32_t unary(Ins::Op op, uint32_t a, Ty ty, bool approx, bool taint = false) {
    Ins in; in.op = op; in.ty = ty; in.approx = approx; in.taint = taint; in.args = {a};
    return emit(std::move(in));
  }
  uint32_t binary(Ins::Op op, uint32_t a, uint32_t b, Ty ty, bool approx, bool taint = false) {
    Ins in; in.op = op; in.ty = ty; in.approx = approx; in.taint = taint; in.args = {a, b};
    return emit(std::move(in));
  }
  static bool decimalish(const Ins& x) { return x.dec || x.taint; }
  static bool is_text(const Ins& x) { return x.ty == Ty::Str; }
  // an exact value a double holds exactly: a dyadic literal, or + - * and unary minus of such
  static bool exact_dyadic(const Ins& x) { return (x.dec && x.dyadic) || x.exact_dyadic; }

  // Both operands' numbers as one lane type: doubles if either is one.
  bool numeric_pair(uint32_t a, uint32_t b, Ty* ty, bool* approx) {
    const Ins& x = at(a);
    const Ins& y = at(b);
    if (x.uns || y.uns || x.ty == Ty::Str || y.ty == Ty::Str) return false;
    *ty = (x.ty == Ty::F64 || y.ty == Ty::F64) ? Ty::F64 : Ty::I64;
    *approx = x.approx || y.approx;
    // An exact decimal (or a value that may be one) beside an exact number: DECIMAL arithmetic, whose digits are
    // the interpreter's (0.1 * 3 is 0.3, not 0.30000000000000004). Beside a double it is a double's arithmetic
    // on the same number; the result stays tainted where the decimal could have won a GREATEST.
    // (a tainted double - one that could be a decimal at some rows - beside an exact integer is the double's
    // arithmetic at the rows that matter; at a row where the decimal won, an exact result the double also
    // holds, as 0.0 * 4096 and 131071.0 * 4096 are)
    // A decimal literal a double holds exactly (1.0) beside an integer: screen_h - 1.0 is the decimal 167.0 to the
    // interpreter and exactly 167 in the lane - the same number, so a tainted exact double (its text is not).
    auto exact_decimal = [](const Ins& v) { return (v.dec && !v.dyadic) || (v.taint && !v.approx && !exact_dyadic(v)); };
    if ((exact_decimal(x) && !y.approx && !y.knull) || (exact_decimal(y) && !x.approx && !x.knull)) return false;
    return true;
  }
  bool tainted(uint32_t a, uint32_t b) const { return decimalish(at(a)) || decimalish(at(b)); }

  uint32_t compile(const SqlExpr* e) {
    if (!ok) return 0;
    if (!e || ++depth > 64) return fail("too deep");
    struct Depth { int& d; const SqlExpr*& c; const SqlExpr* prev; ~Depth() { --d; c = prev; } } dd{depth, cur, cur};
    cur = e;
    switch (e->op) {
      case ExprOp::Literal: return literal(e->literal);
      case ExprOp::Column:
        for (const auto& b : bound) if (b.first == e) return b.second;
        return column(e);
      case ExprOp::Neg: {
        if (e->args.size() != 1) return fail();
        const uint32_t a = compile(e->args[0].get());
        if (!ok) return 0;
        if (at(a).uns || is_text(at(a))) return fail("text operand");
        {
          const uint32_t r = unary(Ins::Neg, a, at(a).ty, at(a).approx, decimalish(at(a)));
          if (exact_dyadic(at(a))) P.ins[r].exact_dyadic = true;
          return r;
        }
      }
      case ExprOp::Add: case ExprOp::Sub: case ExprOp::Mul: case ExprOp::Div: case ExprOp::Mod: {
        if (e->args.size() != 2) return fail();
        // date +/- INTERVAL n DAY | WEEK: the interpreter's apply_date_interval moves the day by n (7n) and prints the
        // date - the same day number, while it stays within years 1..9999 (refused where it might not)
        if ((e->op == ExprOp::Add || e->op == ExprOp::Sub) && e->args[1] && e->args[1]->op == ExprOp::Func &&
            upper_of(e->args[1]->name) == "INTERVAL") {
          const SqlExpr* iv = e->args[1].get();
          if (iv->args.size() != 2 || !iv->args[0] || !iv->args[1] || iv->args[0]->op != ExprOp::Literal || iv->args[1]->op != ExprOp::Literal)
            return fail("an INTERVAL not of literals");
          const Val& q = iv->args[0]->literal;
          const Val& u = iv->args[1]->literal;
          if (q.null || u.null || !q.numeric || q.approx || q.num != std::floor(q.num) || std::fabs(q.num) > 1e7) return fail("an INTERVAL not of whole days");
          const std::string unit = upper_of(u.str());
          if (unit != "DAY" && unit != "DAYS" && unit != "WEEK" && unit != "WEEKS") return fail("an INTERVAL of " + unit);
          const uint32_t a = compile(e->args[0].get());
          if (!ok) return 0;
          if (!at(a).day) return fail("an INTERVAL on what is not a date");
          int64_t nd = (int64_t)q.num * (unit[0] == 'W' ? 7 : 1);
          if (e->op == ExprOp::Sub) nd = -nd;
          const int64_t lo = at(a).dlo + nd, hi = at(a).dhi + nd;
          if (lo < -719162 || hi > 2932896) return fail("an INTERVAL past the years 1..9999");
          Ins k; k.op = Ins::Konst; k.ty = Ty::I64; k.ki = nd;
          const uint32_t kx = emit(std::move(k));
          Ins in; in.op = Ins::Add; in.ty = Ty::I64; in.args = {a, kx}; in.day = true; in.dlo = lo; in.dhi = hi;
          return emit(std::move(in));
        }
        uint32_t a = compile(e->args[0].get());
        uint32_t b = compile(e->args[1].get());
        if (!ok) return 0;
        // A bit operator's BIGINT UNSIGNED beside a DOUBLE (((color >> 16) & 255) * shade): the DOUBLE's arithmetic
        // on the unsigned number, as MariaDB does it - converted through CastF, which reads the lane unsigned.
        if (e->op != ExprOp::Mod) {
          auto pure_double = [&](uint32_t x) { return at(x).ty == Ty::F64 && at(x).approx && !decimalish(at(x)); };
          if (at(a).uns && pure_double(b)) a = unary(Ins::CastF, a, Ty::F64, true);
          else if (at(b).uns && pure_double(a)) b = unary(Ins::CastF, b, Ty::F64, true);
        }
        Ty ty; bool approx;
        if (!numeric_pair(a, b, &ty, &approx)) return fail(at(a).uns || at(b).uns ? "unsigned arithmetic" : "decimal arithmetic");
        if (e->op == ExprOp::Div) {
          const bool pure_approx = (at(a).approx && !decimalish(at(a))) || (at(b).approx && !decimalish(at(b)));
          if ((pg_div && !pure_approx) || !approx) return fail(pg_div ? "PG division" : "exact division");   // two exact operands divide as decimals
          return binary(Ins::Div, a, b, Ty::F64, true, at(a).taint || at(b).taint);
        }
        if (e->op == ExprOp::Mod && ty == Ty::F64 && !approx) return fail("decimal MOD");   // exact decimals' MOD
        if (ty == Ty::F64 && !approx && (e->op == ExprOp::Add || e->op == ExprOp::Sub || e->op == ExprOp::Mul)) {
          // an exact result the lane holds exactly while it stays a whole number of halves, quarters.. below 2^53:
          // the operands are, and a sum or product of two such is one; past 2^53 the double rounds - refused
          Ins::Op op = e->op == ExprOp::Add ? Ins::Add : e->op == ExprOp::Sub ? Ins::Sub : Ins::Mul;
          Ins in; in.op = op; in.ty = Ty::F64; in.approx = false; in.taint = true; in.exact_dyadic = true; in.args = {a, b};
          return emit(std::move(in));
        }
        const Ins::Op op = e->op == ExprOp::Add ? Ins::Add : e->op == ExprOp::Sub ? Ins::Sub : e->op == ExprOp::Mul ? Ins::Mul : Ins::Mod;
        // A double beside an exact decimal (price * qty - CAST(.. AS DECIMAL(10,2)), x - 0.5) is a double at every row
        // (exact_arith declines a DOUBLE operand): tainted only through an operand that may itself be either.
        if (approx) return binary(op, a, b, ty, approx, at(a).taint || at(b).taint);
        return binary(op, a, b, ty, approx, tainted(a, b));
      }
      case ExprOp::Eq: case ExprOp::Ne: case ExprOp::Lt: case ExprOp::Le: case ExprOp::Gt: case ExprOp::Ge: {
        if (e->args.size() != 2 || e->negated) return fail();
        // text_col = 'lit'
        const SqlExpr* l = e->args[0].get();
        const SqlExpr* r = e->args[1].get();
        if (e->op == ExprOp::Eq || e->op == ExprOp::Ne) {
          const SqlExpr* col = nullptr; const SqlExpr* lit = nullptr;
          if (l && l->op == ExprOp::Column && r && r->op == ExprOp::Literal && !r->literal.numeric && !r->literal.null) { col = l; lit = r; }
          else if (r && r->op == ExprOp::Column && l && l->op == ExprOp::Literal && !l->literal.numeric && !l->literal.null) { col = r; lit = l; }
          if (col) {
            size_t s = 0, c = 0;
            if (place(col, &s, &c) && (*ctx.sources)[s].table->columns[c].type == SqlDataType::Text) {
              const uint32_t m = str_in(col, {lit});
              if (!ok) return 0;
              return e->op == ExprOp::Eq ? m : unary(Ins::Not, m, Ty::I64, false);
            }
          }
        }
        if ((e->op == ExprOp::Eq || e->op == ExprOp::Ne) && l && r && l->op == ExprOp::Column && r->op == ExprOp::Column) {
          uint32_t ca = 0, cb = 0;   // text_col = text_col over the dictionaries' collation classes
          if (text_class_pair(l, r, &ca, &cb)) return binary(e->op == ExprOp::Eq ? Ins::Eq : Ins::Ne, ca, cb, Ty::I64, false);
        }
        const uint32_t a = compile(l);
        const uint32_t b = compile(r);
        if (!ok) return 0;
        if (at(a).uns || at(b).uns) return fail("unsigned compared");
        if (is_text(at(a)) || is_text(at(b))) {
          // two texts compare as texts (= and <> alone: their order is the collation's)
          if (!is_text(at(a)) || !is_text(at(b))) return fail("text compared with a number");
          if (e->op != ExprOp::Eq && e->op != ExprOp::Ne) return fail("text order");
        }
        const Ins::Op op = e->op == ExprOp::Eq ? Ins::Eq : e->op == ExprOp::Ne ? Ins::Ne : e->op == ExprOp::Lt ? Ins::Lt
                         : e->op == ExprOp::Le ? Ins::Le : e->op == ExprOp::Gt ? Ins::Gt : Ins::Ge;
        return binary(op, a, b, Ty::I64, false);
      }
      case ExprOp::And: case ExprOp::Or: {
        if (e->args.size() != 2) return fail();
        const uint32_t a = compile(e->args[0].get());
        const uint32_t b = compile(e->args[1].get());
        if (!ok) return 0;
        if (is_text(at(a)) || is_text(at(b))) return fail("truth of a text");
        return binary(e->op == ExprOp::And ? Ins::And : Ins::Or, a, b, Ty::I64, false);
      }
      case ExprOp::Not: {
        if (e->args.size() != 1) return fail();
        const uint32_t a = compile(e->args[0].get());
        if (!ok) return 0;
        if (is_text(at(a))) return fail("truth of a text");
        return unary(Ins::Not, a, Ty::I64, false);
      }
      case ExprOp::IsNull: {
        if (e->args.size() != 1) return fail();
        const SqlExpr* a = e->args[0].get();
        if (a && a->op == ExprOp::Column && !is_user_var_name(a->name) && a->name.rfind("@@", 0) != 0) {
          size_t s = 0, c = 0;
          if (!place(a, &s, &c)) return fail();
          Ins in; in.op = Ins::ColNull; in.ty = Ty::I64; in.src = (uint32_t)s; in.cs = &(*ctx.sources)[s].table->col_data[c];
          const uint32_t m = emit(std::move(in));
          return e->negated ? unary(Ins::Not, m, Ty::I64, false) : m;
        }
        const uint32_t x = compile(a);
        if (!ok) return 0;
        return unary(e->negated ? Ins::NotNull : Ins::IsNull, x, Ty::I64, false);
      }
      case ExprOp::Between: {
        // x BETWEEN lo AND hi: lo <= x AND x <= hi, as the interpreter reads it (NOT BETWEEN negated)
        if (e->args.size() != 3) return fail("BETWEEN arity");
        const uint32_t x = compile(e->args[0].get());
        const uint32_t lo = compile(e->args[1].get());
        const uint32_t hi = compile(e->args[2].get());
        if (!ok) return 0;
        if (at(x).uns || at(lo).uns || at(hi).uns) return fail("unsigned BETWEEN");
        const uint32_t a = binary(Ins::Ge, x, lo, Ty::I64, false);
        const uint32_t b = binary(Ins::Le, x, hi, Ty::I64, false);
        const uint32_t both = binary(Ins::And, a, b, Ty::I64, false);
        return e->negated ? unary(Ins::Not, both, Ty::I64, false) : both;
      }
      case ExprOp::In: {
        // text_col IN ('a', 'b'): literals only, the column dictionary-coded
        if (e->args.size() < 2) return fail();
        const SqlExpr* col = e->args[0].get();
        if (!col || col->op != ExprOp::Column) return fail();
        std::vector<const SqlExpr*> lits;
        for (size_t i = 1; i < e->args.size(); ++i) {
          const SqlExpr* l = e->args[i].get();
          if (!l || l->op != ExprOp::Literal || l->literal.numeric || l->literal.null) return fail();
          lits.push_back(l);
        }
        const uint32_t m = str_in(col, lits);
        if (!ok) return 0;
        return e->negated ? unary(Ins::Not, m, Ty::I64, false) : m;
      }
      case ExprOp::Case: {
        if (e->args.size() < 2) return fail("CASE of no values");
        Ins in;
        in.op = Ins::Case;
        bool first = true;
        if (!e->negated) {
          for (size_t i = 0; i < e->args.size(); ++i) {
            const uint32_t a = compile(e->args[i].get());
            if (!ok) return 0;
            in.args.push_back(a);
            const bool is_value = (i % 2 == 1) || (i + 1 == e->args.size() && e->args.size() % 2 == 1);
            if (!is_value) { if (is_text(at(a))) return fail("truth of a text"); continue; }
            if (!same_kind(in, at(a), &first)) return fail("CASE branches of mixed kinds");
          }
          if (first) return fail("CASE of no values");
          if (!uns_kinds(in)) return fail("unsigned CASE branch mixed with a non-integer");
          double_case(e, &in);
          return emit(std::move(in));
        }
        // CASE x WHEN v THEN r ...: each WHEN is x = v (a NULL never equal, as compare_vals has it) - of a text
        // column against text literals through the dictionary, of numbers as numbers
        const SqlExpr* x = e->args[0].get();
        bool text_case = x && x->op == ExprOp::Column;
        for (size_t i = 1; text_case && i + 1 < e->args.size(); i += 2) {
          const SqlExpr* v = e->args[i].get();
          if (!v || v->op != ExprOp::Literal || v->literal.numeric || v->literal.null) text_case = false;
        }
        if (text_case) {
          size_t cs = 0, cc = 0;
          text_case = place(x, &cs, &cc) && (*ctx.sources)[cs].table->columns[cc].type == SqlDataType::Text;
        }
        uint32_t ox = 0;
        if (!text_case) {
          ox = compile(x);
          if (!ok) return 0;
          if (at(ox).uns) return fail("unsigned CASE operand");
        }
        const bool ox_text = !text_case && is_text(at(ox));
        for (size_t i = 1; i + 1 < e->args.size(); i += 2) {
          uint32_t cond;
          if (text_case) {
            cond = str_in(x, {e->args[i].get()});
          } else {
            const uint32_t v = compile(e->args[i].get());
            if (!ok) return 0;
            if (at(v).uns) return fail("unsigned CASE value");
            if (is_text(at(v)) != ox_text) return fail("text compared with a number");
            cond = binary(Ins::Eq, ox, v, Ty::I64, false);
          }
          if (!ok) return 0;
          const uint32_t r = compile(e->args[i + 1].get());
          if (!ok) return 0;
          in.args.push_back(cond);
          in.args.push_back(r);
          if (!same_kind(in, at(r), &first)) return fail("CASE branches of mixed kinds");
        }
        if ((e->args.size() - 1) % 2 == 1) {
          const uint32_t d = compile(e->args.back().get());
          if (!ok) return 0;
          in.args.push_back(d);
          if (!same_kind(in, at(d), &first)) return fail("CASE branches of mixed kinds");
        }
        if (first) return fail("CASE of no values");
        if (!uns_kinds(in)) return fail("unsigned CASE branch mixed with a non-integer");
        double_case(e, &in);
        return emit(std::move(in));
      }
      case ExprOp::Func: {
        const std::string& up = func_upper(e);
        const size_t n = e->args.size();
        if (is_aggregate_name(up) || e->window) return fail("aggregate or window function");
        // SQRT and the rest give a DOUBLE of any number: a decimal argument is read as its double
        auto one = [&](Ins::Op op, Ty ty, bool approx) -> uint32_t {
          if (n != 1) return fail();
          const uint32_t a = compile(e->args[0].get());
          if (!ok) return 0;
          if (at(a).uns || is_text(at(a))) return fail("text operand");
          return unary(op, a, ty, approx);
        };
        if (up == "YEAR"sv || up == "MONTH"sv || up == "DAY"sv || up == "DAYOFMONTH"sv || up == "QUARTER"sv) {
          if (n != 1 || !e->args[0]) return fail();
          return date_part(e->args[0].get(), up == "YEAR"sv ? 1 : up == "MONTH"sv ? 2 : up == "QUARTER"sv ? 4 : 3);
        }
        if (up == "ABS"sv || up == "FLOOR"sv || up == "CEIL"sv || up == "CEILING"sv) {
          if (n != 1) return fail();
          const uint32_t a = compile(e->args[0].get());
          if (!ok) return 0;
          if (at(a).uns || is_text(at(a))) return fail("text operand");
          // FLOOR of an exact decimal is an exact integer, of a double a double: the same number, so tainted
          return unary(up == "ABS"sv ? Ins::Abs : up == "FLOOR"sv ? Ins::Floor : Ins::Ceil, a, at(a).ty, at(a).approx, decimalish(at(a)));
        }
        if (up == "ROUND"sv) {
          // ROUND(x) or ROUND(x, 0): of an integer that integer, of a DOUBLE a DOUBLE (RoundF). An exact decimal
          // rounds on its digits to a decimal: the interpreter's.
          if (n != 1 && !(n == 2 && e->args[1] && e->args[1]->op == ExprOp::Literal && e->args[1]->literal.numeric &&
                          !e->args[1]->literal.approx && e->args[1]->literal.str() == "0"))
            return fail("ROUND to places");
          const uint32_t a = compile(e->args[0].get());
          if (!ok) return 0;
          const Ins& x = at(a);
          if (x.uns || is_text(x) || x.day) return fail("ROUND operand");
          if (x.ty == Ty::I64 && !decimalish(x)) return a;
          if (x.ty == Ty::F64 && x.approx && !decimalish(x)) return unary(Ins::RoundF, a, Ty::F64, true);
          return fail("ROUND of an exact decimal");
        }
        if (up == "SQRT"sv) return one(Ins::Sqrt, Ty::F64, true);
        if (up == "SIN"sv) return one(Ins::Sin, Ty::F64, true);
        if (up == "COS"sv) return one(Ins::Cos, Ty::F64, true);
        if (up == "TAN"sv) return one(Ins::Tan, Ty::F64, true);
        if (up == "ATAN"sv && n == 1) return one(Ins::Atan, Ty::F64, true);
        if (up == "RADIANS"sv) return one(Ins::Radians, Ty::F64, true);
        if (up == "DEGREES"sv) return one(Ins::Degrees, Ty::F64, true);
        if (up == "ATAN2"sv || up == "POW"sv || up == "POWER"sv) {
          if (n != 2) return fail();
          const uint32_t a = compile(e->args[0].get());
          const uint32_t b = compile(e->args[1].get());
          if (!ok) return 0;
          if (at(a).uns || at(b).uns || is_text(at(a)) || is_text(at(b))) return fail("text operand");
          return binary(up == "ATAN2"sv ? Ins::Atan2 : Ins::Pow, a, b, Ty::F64, true);   // a DOUBLE: no taint
        }
        if (up == "__INTDIV"sv) {
          if (n != 2) return fail();
          const uint32_t a = compile(e->args[0].get());
          const uint32_t b = compile(e->args[1].get());
          if (!ok) return 0;
          if (at(a).ty != Ty::I64 || at(b).ty != Ty::I64 || at(a).uns || at(b).uns) return fail("DIV operands");
          return binary(Ins::IDiv, a, b, Ty::I64, false);
        }
        if (up == "MOD"sv) {
          if (n != 2) return fail();
          const uint32_t a = compile(e->args[0].get());
          const uint32_t b = compile(e->args[1].get());
          if (!ok) return 0;
          Ty ty; bool approx;
          if (!numeric_pair(a, b, &ty, &approx) || (ty == Ty::F64 && !approx)) return fail();
          return binary(Ins::Mod, a, b, ty, approx);
        }
        if (up == "__BITAND"sv || up == "__BITOR"sv || up == "__BITXOR"sv || up == "__SHL"sv || up == "__SHR"sv) {
          if (n != 2) return fail();
          const uint32_t a = compile(e->args[0].get());
          const uint32_t b = compile(e->args[1].get());
          if (!ok) return 0;
          if (at(a).dec || at(b).dec || is_text(at(a)) || is_text(at(b))) return fail("bit operand");
          Ins in;
          in.op = up == "__BITAND"sv ? Ins::BitAnd : up == "__BITOR"sv ? Ins::BitOr : up == "__BITXOR"sv ? Ins::BitXor
                : up == "__SHL"sv ? Ins::Shl : Ins::Shr;
          in.ty = Ty::I64; in.uns = true; in.args = {a, b};
          return emit(std::move(in));
        }
        if (up == "__BITNOT"sv) {
          if (n != 1) return fail();
          const uint32_t a = compile(e->args[0].get());
          if (!ok) return 0;
          if (at(a).dec || is_text(at(a))) return fail("bit operand");
          Ins in; in.op = Ins::BitNot; in.ty = Ty::I64; in.uns = true; in.args = {a};
          return emit(std::move(in));
        }
        if (up == "CAST"sv) {
          if (n < 2 || !e->args[1] || e->args[1]->op != ExprOp::Literal) return fail();
          const std::string target = upper_of(e->args[1]->literal.str());
          const std::string_view pg = n > 2 && e->args[2] ? e->args[2]->literal.sv() : std::string_view();
          if (!pg.empty()) return fail("x::type cast");   // x::type's own roundings
          const uint32_t a = compile(e->args[0].get());
          if (!ok) return 0;
          if (is_text(at(a))) return fail("CAST of a text");
          // CAST(x AS SIGNED) rounds half away from zero, an exact decimal on its digits and a double on its 15
          // significant digits as written (round_double_as_written): the same integer for either reading of
          // the lane's number. CAST AS DOUBLE is a DOUBLE of either. Both erase the text, and the taint.
          if (target.rfind("SIGNED", 0) == 0) return unary(Ins::CastI, a, Ty::I64, false);
          if (target.rfind("DOUBLE", 0) == 0 || target.rfind("FLOAT", 0) == 0 || target.rfind("REAL", 0) == 0)
            return unary(Ins::CastF, a, Ty::F64, true);
          if (target.rfind("DECIMAL", 0) == 0 || target.rfind("NUMERIC", 0) == 0) {
            // of a DOUBLE (its rounding is round_double_scaled's) or an exact integer (its digits, padded); not of an
            // exact decimal, whose digits the lane does not hold. At most 15 digits: every such value has its own
            // double, so the double's comparisons are the decimal's.
            const size_t lp = target.find('('), comma = target.find(',');
            const int prec = lp == std::string::npos ? 10 : std::atoi(target.c_str() + lp + 1);
            const int places = comma == std::string::npos ? 0 : std::atoi(target.c_str() + comma + 1);
            if (prec < 1 || prec > 15 || places < 0 || places > prec) return fail("CAST to DECIMAL past 15 digits");
            const Ins& x = at(a);
            if (x.uns || x.taint || x.day || (x.ty == Ty::F64 && !x.approx)) return fail("CAST to DECIMAL of an exact decimal");
            Ins in;
            in.op = Ins::CastDec;
            in.ty = Ty::F64;
            in.approx = false;
            in.dec = true;
            in.dplaces = places;
            in.dlim = 1;
            for (int i = 0; i < prec; ++i) in.dlim *= 10;
            in.args = {a};
            return emit(std::move(in));
          }
          return fail();
        }
        if (up == "IF"sv) {
          if (n != 3) return fail();
          Ins in;
          in.op = Ins::Case;
          bool first = true;
          for (size_t i = 0; i < 3; ++i) {
            const uint32_t a = compile(e->args[i].get());
            if (!ok) return 0;
            in.args.push_back(a);
            if (i > 0 && !same_kind(in, at(a), &first)) return fail();
          }
          if (!uns_kinds(in)) return fail("unsigned IF branch mixed with a non-integer");
          double_case(e, &in);
          return emit(std::move(in));
        }
        if (up == "COALESCE"sv || up == "IFNULL"sv || up == "GREATEST"sv || up == "LEAST"sv || up == "NULLIF"sv) {
          if (n < 1 || (up == "NULLIF"sv && n != 2) || (up == "IFNULL"sv && n != 2)) return fail();
          Ins in;
          in.op = up == "NULLIF"sv ? Ins::NullIf : up == "GREATEST"sv ? Ins::Greatest : up == "LEAST"sv ? Ins::Least : Ins::Coalesce;
          bool first = true;
          for (size_t i = 0; i < n; ++i) {
            const uint32_t a = compile(e->args[i].get());
            if (!ok) return 0;
            in.args.push_back(a);
            // NULLIF(x, y) is x or NULL: y only decides, and may be of any numeric kind (or a text beside a text)
            if (up == "NULLIF"sv && i == 1) {
              if (at(a).uns) return fail("unsigned NULLIF");
              if (is_text(at(a)) != is_text(at(in.args[0]))) return fail("text compared with a number");
              continue;
            }
            if ((up == "GREATEST"sv || up == "LEAST"sv) && is_text(at(a))) return fail("text order");
            if (!same_kind(in, at(a), &first)) return fail("branches of mixed kinds");
          }
          if (first) return fail();
          if (in.has_uns) return fail("unsigned argument");   // these run without CASE's check: as before
          if (up != "NULLIF"sv) double_case(e, &in);
          return emit(std::move(in));
        }
        if (up == "__ROW_IN"sv) return tuple_in(e);
        if (uint32_t r = stored_function(e, up); ok) return r;
        return 0;
      }
      default:
        return fail("operator " + std::to_string((int)e->op));
    }
  }

  // (a, b) [NOT] IN (SELECT x, y ..) whose subquery has run and read nothing from outside, and whose rows are all
  // numbers (RowInSet::by_tuple): each row's tuple is one lookup. A set not run yet is left to the interpreter
  // this time, and tried again on the next statement (retry).
  uint32_t tuple_in(const SqlExpr* e) {
    if (e->args.size() != 1 || !e->args[0] || e->subquery_sql.empty()) return fail("row-value IN form");
    std::shared_ptr<const RowInSet> set;
    {
      std::unique_lock<aios::hal::Mutex> lock;
      if (e->sub_mu) lock = std::unique_lock<aios::hal::Mutex>(*e->sub_mu);
      if (e->sub_evaluated && !e->sub_correlated) set = e->row_in_set;
    }
    if (!set) { retry = true; return fail("row-value IN set not run yet"); }
    size_t ncols = 0;
    if (!row_in_set_by_tuple(*set, &ncols)) return fail("row-value IN set not all numbers");
    const SqlExpr* row = e->args[0].get();
    if (row->args.size() != ncols || ncols == 0 || ncols > 8) return fail("row-value IN width");
    Ins in;
    in.op = Ins::TupleIn;
    in.ty = Ty::I64;
    in.set = set;
    for (const auto& a : row->args) {
      const uint32_t x = compile(a.get());
      if (!ok) return 0;
      if (is_text(at(x)) || at(x).uns) return fail("row-value IN of a text");
      in.args.push_back(x);
    }
    const uint32_t m = emit(std::move(in));
    return e->negated ? unary(Ins::Not, m, Ty::I64, false) : m;
  }

  // A call of a stored function whose body is RETURN <expression> over its parameters: the arguments compile to
  // lanes, coerced to the parameters' declared types as a call coerces them (psm::as_returns_type: an INT rounds
  // half away from zero, a DOUBLE is a double), the template compiles with its parameter nodes bound to those
  // lanes under the routine's own division rule, and the result is coerced to the RETURNS type the same way.
  uint32_t stored_function(const SqlExpr* e, const std::string& up) {
    if (!ctx.db || inlined >= 8) return fail("function " + up);
    std::shared_ptr<const SqlDatabase::StoredProgram> fn = ctx.db->find_program('F', e->name);
    if (!fn || fn->aggregate || fn->params.size() != e->args.size()) return fail("function " + up);
    std::shared_ptr<const FastReturn> fr = fast_return_of(*fn);
    if (!fr || !fr->tmpl) return fail("stored function " + up + " is not a RETURN expression");
    // the declared type's coercion of a lane, or none for a type the call leaves alone
    auto coerce = [&](uint32_t a, const std::string& type, bool* known) -> uint32_t {
      std::string k = lower_of(type);
      size_t i = 0;
      while (i < k.size() && std::isalpha((unsigned char)k[i])) ++i;
      k = k.substr(0, i);
      *known = true;
      if (is_text(at(a))) return fail("text into a routine parameter");
      if (k == "int" || k == "integer" || k == "bigint" || k == "smallint" || k == "tinyint" || k == "mediumint") {
        if (at(a).uns) return fail("unsigned into INT");
        if (at(a).ty == Ty::I64) return a;
        return unary(Ins::RoundI, a, Ty::I64, false);
      }
      if (k == "double" || k == "float" || k == "real") {
        if (at(a).uns) return fail("unsigned into DOUBLE");
        return unary(Ins::CastF, a, Ty::F64, true);
      }
      if (k == "decimal" || k == "numeric" || k == "dec" || k == "char" || k == "varchar" || k == "text" ||
          k == "tinytext" || k == "mediumtext" || k == "longtext")
        return fail("stored function " + up + ": " + k + " parameter or result");
      *known = false;   // left as it is
      return a;
    };
    std::vector<uint32_t> args;
    for (size_t i = 0; i < e->args.size(); ++i) {
      const uint32_t a = compile(e->args[i].get());
      if (!ok) return 0;
      bool known = false;
      const uint32_t c = coerce(a, fn->params[i].type, &known);
      if (!ok) return 0;
      args.push_back(c);
    }
    // the parameter nodes, by their paths in the template
    const size_t bound_before = bound.size();
    for (const auto& [path, k] : fr->params) {
      const SqlExpr* n = fr->tmpl.get();
      for (size_t i : path) n = i < n->args.size() ? n->args[i].get() : nullptr;
      if (!n || k >= args.size()) return fail("stored function " + up + ": parameter path");
      bound.emplace_back(n, args[k]);
    }
    const bool saved_div = pg_div;
    pg_div = fn->pg_division;
    ++inlined;
    const uint32_t body = compile(fr->tmpl.get());
    --inlined;
    pg_div = saved_div;
    bound.resize(bound_before);
    if (!ok) return 0;
    bool known = false;
    const uint32_t r = coerce(body, fn->returns, &known);
    if (!ok) return 0;
    P.has_fn = true;
    P.fn_gen = ctx.db->programs_generation();
    return r;
  }

  // The values a CASE / COALESCE / GREATEST may give are of one kind: all integers (exact) or all doubles
  // (approximate); a NULL fits either. Mixed, the interpreter's answer type would change row by row.
  // An exact decimal among doubles (GREATEST(0.0, d), LEAST(d, 131071.0)) is allowed: the number is right at every
  // row, the text may not be, so the result is tainted and only erased by a CAST, a comparison or a truth test.
  // An exact integer beside a double (GREATEST(0, FLOOR(d) + n)) is the same: the result is a double lane that is
  // tainted, since the interpreter's answer is the exact integer at the rows where it won.
  // Once a CASE's branches are all seen: all unsigned, an unsigned lane; unsigned mixed with signed integers, a
  // signed lane checked as it runs (uns_check); unsigned with a double or text, no program.
  bool uns_kinds(Ins& in) {
    if (!in.has_uns) return true;
    if (in.ty != Ty::I64) return false;
    if (in.has_signed) in.uns_check = true; else in.uns = true;
    return true;
  }
  // A CASE MariaDB types a DOUBLE (a DOUBLE branch beside integers: CASE WHEN m = 1 THEN qty ELSE 0 END): the
  // interpreter hands back every branch as a DOUBLE (nt_adjust), so the lane is one at every row - not tainted.
  void double_case(const SqlExpr* e, Ins* in) {
    if (in->ty != Ty::F64 || in->has_uns) return;
    if (case_result_kind(e, ctx) != 3) return;
    in->approx = true;
    in->taint = false;
  }
  bool same_kind(Ins& in, const Ins& v, bool* first) {
    if (v.knull) return true;
    if (v.uns) { if (v.ty != Ty::I64) return false; in.has_uns = true; }   // settled once every branch is seen (uns_kinds)
    else in.has_signed = true;
    if (v.ty == Ty::Str || (!*first && in.ty == Ty::Str)) {
      if (*first) { in.ty = Ty::Str; *first = false; return true; }
      return in.ty == Ty::Str && v.ty == Ty::Str;
    }
    if (*first) { in.ty = v.ty; in.approx = v.approx; in.taint = decimalish(v); *first = false; return true; }
    if (in.ty == Ty::I64 && v.ty == Ty::I64) return true;
    if (in.ty == Ty::I64) { in.ty = Ty::F64; in.taint = true; }   // the integers so far were exact: now a mix
    if (v.ty == Ty::I64) in.taint = true;
    in.approx = in.approx || v.approx;
    in.taint = in.taint || decimalish(v);
    return true;
  }
};


// The program for e over ctx's sources, or null.
inline std::shared_ptr<Prog> compile(const SqlExpr* e, const EvalCtx& ctx, std::string* why = nullptr, bool* retry = nullptr) {
  if (!enabled() || !e || !ctx.sources) return nullptr;
  if (e->op == ExprOp::Literal || e->op == ExprOp::Column) return nullptr;   // nothing to gain
  auto P = std::make_shared<Prog>();
  literal_sig(e, &P->lit_sig);
  P->pg_division = tls_pg_division;
  Compiler c{ctx, *P};
  c.compile(e);
  if (!c.ok || P->ins.empty() || P->ins.size() > 256 || P->out().taint || P->out().dec || P->out().day) {
    if (why) *why = !c.ok ? c.why : P->ins.size() > 256 ? "too long" : P->out().day ? "a date's value" : "result may be an exact decimal";
    if (retry) *retry = c.retry;
    return nullptr;
  }
  std::string jwhy;
  P->leaf_kind.assign(P->ins.size(), LeafLane);
  for (size_t x = 0; x < P->ins.size(); ++x) if (vjit_leaf(P->ins[x].op)) P->leaf_kind[x] = leaf_kind_of(P->ins[x]);
  P->jit = vjit_build_for(*P, &jwhy, &P->hoisted, &P->hoist_srcs);
  if (!P->jit) {
    P->leaf_kind.assign(P->ins.size(), LeafLane);
    P->inv.assign(P->ins.size(), 0);
    uint64_t hsrcs = 0;
    bool any_inv = false;
    for (size_t x = 0; x < P->ins.size(); ++x) {
      const Ins& in = P->ins[x];
      if (in.op == Ins::Konst) { P->inv[x] = 1; continue; }
      if (vjit_leaf(in.op)) {
        if (in.cs && in.cs->size() == 1 && in.src < 64) { P->inv[x] = 1; hsrcs |= (1ULL << in.src); }
        continue;
      }
      if (in.args.empty()) continue;
      bool all = true;
      for (uint32_t a : in.args) if (a >= x || !P->inv[a]) { all = false; break; }
      if (all) { P->inv[x] = 1; any_inv = true; }
    }
    P->hoisted = any_inv;
    P->hoist_srcs = any_inv ? hsrcs : 0;
  }
  if (vx_trace_on() && !P->jit && !jwhy.empty()) vx_trace("vp: no native code: " + jwhy);
  if (vx_trace_on() && P->jit) {
    size_t one_row = 0, lanes = 0, natives = 0;
    for (size_t x = 0; x < P->ins.size(); ++x) {
      if (!vjit_leaf(P->ins[x].op) || P->ins[x].op == Ins::Konst) continue;
      if (P->leaf_kind[x] == LeafLane) ++lanes; else ++natives;
      if (P->ins[x].cs && P->ins[x].cs->size() == 1) ++one_row;
    }
    vx_trace("vp: native code: " + std::string(P->hoisted ? "hoisted" : "not hoisted") + ", sources " + std::to_string(P->hoist_srcs) +
             ", one-row leaves " + std::to_string(one_row) + ", lane leaves " + std::to_string(lanes) + ", gathered leaves " + std::to_string(natives));
  }
  return P;
}

// One lane per instruction over a chunk of rows.
struct Lanes {
  std::vector<std::vector<int64_t>> i;
  std::vector<std::vector<double>> f;
  std::vector<std::vector<uint8_t>> n;   // 1: NULL
  // A column read by the program, decoded whole once (the kernels' block decoders) and gathered by row id, where
  // the column is not much larger than the row set: a per-row accessor (a switch on the encoding, a bit unpack)
  // was 20-40 ns a cell, most of a 55-op program's time.
  // A column already laid out flat (a typed column's values, a 32-bit dictionary's codes) is read in place. The
  // decode is skipped only when the column is much larger than the row set this Lanes serves: a program run over
  // a chunk of a few hundred rows (a join's ON check over a chunk's candidates) decoded a 56,000-row column whole
  // for each chunk under a flat 65,536 floor - 220 decodes of the column for one pass over it.
  size_t total_rows = 0;
  struct Cached { std::vector<int64_t> i; std::vector<double> f; std::vector<uint32_t> c; bool ints = false, dbls = false, codes = false; };
  std::unordered_map<const ColumnStore*, Cached> cols;
  Cached* cached(const ColumnStore& cs) {
    const size_t sz = cs.size();
    if (sz > std::max<size_t>(4096, total_rows * 16)) return nullptr;
    return &cols[&cs];
  }
  const int64_t* ints(const ColumnStore& cs) {
    if (cs.encoding == ColumnEncoding::TypedInt64 && cs.int64_data.size() >= cs.size()) return cs.int64_data.data();
    Cached* cc = cached(cs);
    if (!cc) return nullptr;
    if (!cc->ints) { cc->i.resize(cs.size()); if (cs.size()) vx::typed_ints(cs, 0, cs.size(), cc->i.data()); cc->ints = true; }
    return cc->i.data();
  }
  const double* dbls(const ColumnStore& cs) {
    if (cs.encoding == ColumnEncoding::TypedDouble && cs.double_data.size() >= cs.size()) return cs.double_data.data();
    Cached* cc = cached(cs);
    if (!cc) return nullptr;
    if (!cc->dbls) { cc->f.resize(cs.size()); if (cs.size()) vx::typed_values(cs, 0, cs.size(), cc->f.data()); cc->dbls = true; }
    return cc->f.data();
  }
  const uint32_t* codes(const ColumnStore& cs) {
    if (cs.encoding != ColumnEncoding::RLE && cs.encoding != ColumnEncoding::Constant && !cs.seg_packed_ && cs.bit_width_ == 32 &&
        cs.data32.size() >= cs.size())
      return cs.data32.data();
    Cached* cc = cached(cs);
    if (!cc) return nullptr;
    if (!cc->codes) { cc->c.resize(cs.size()); if (cs.size()) vx::decode_codes(cs, 0, cs.size(), cc->c.data()); cc->codes = true; }
    return cc->c.data();
  }
  // The row set's row ids in source `src`, worked out once over the set (rowat(r).parts[src]) and read by the
  // leaves of every program run over it, instead of each leaf resolving each row's JoinedRow again (a projection
  // of five programs over 56,000 joined rows resolved 30 row ids a row). Kept across programs when the Lanes is
  // shared (eval_column's `shared`).
  std::vector<std::vector<size_t>> rid;
  std::vector<uint8_t> rid_ok;
  template <typename RowAt>
  const size_t* rows_in(uint32_t src, RowAt&& rowat) {
    if (rid.size() <= src) { rid.resize(src + 1); rid_ok.resize(src + 1, 0); }
    if (!rid_ok[src]) {
      rid[src].resize(total_rows);
      for (size_t r = 0; r < total_rows; ++r) {
        const JoinedRow& jr = rowat(r);
        rid[src][r] = src < jr.parts.size() ? jr.parts[src] : kNullRow;
      }
      rid_ok[src] = 1;
    }
    return rid[src].data();
  }
  // The chunk's row ids per source, for the leaves the native code gathers (eval).
  std::vector<std::vector<size_t>> ridc;
  // A different row set: what was worked out over the last one goes.
  void reset_for(size_t total) {
    if (total_rows == total) return;
    total_rows = total;
    cols.clear();
    rid.clear();
    rid_ok.clear();
  }
};

// A DOUBLE rounded to a whole number as the interpreter rounds it (round_value -> round_double_as_written): half
// away from zero on its 15 significant digits as written. Writing the double out and reading it back is ~300 ns,
// so it is done only where it can differ from std::round: a fraction within the rendering's resolution of a half
// (2.4999999999999996 is written 2.5).
inline double round_whole_as_written(double a) {
  const double fl = std::floor(a);
  const double frac = a - fl;
  const double tol = std::fabs(a) * 1e-13 + 1e-13;
  if (frac == 0) return a;
  if (std::fabs(frac - 0.5) > tol) return std::round(a);
  Val r;
  return round_double_as_written(a, 0, &r) ? r.num : std::nearbyint(a);
}

// Evaluates P over n rows, rowid(j, src) being row j's row in source src (or kNullRow). False with *err set when
// a value the interpreter would refuse comes up (an integer out of range), and the caller takes the interpreter's
// path.
template <typename RowId>
inline bool eval_ins(const Prog& P, size_t x, RowId&& rowid, size_t n, Lanes& L, std::string* err) {
  {
    const Ins& in = P.ins[x];
    std::vector<int64_t>& vi = L.i[x];
    std::vector<double>& vf = L.f[x];
    std::vector<uint8_t>& nl = L.n[x];
    nl.assign(n, 0);
    if (in.ty != Ty::F64) vi.resize(n); else vf.resize(n);
    auto A = [&](size_t k) -> const std::vector<int64_t>& { return L.i[in.args[k]]; };
    auto F = [&](size_t k) -> const std::vector<double>& { return L.f[in.args[k]]; };
    auto N = [&](size_t k) -> const std::vector<uint8_t>& { return L.n[in.args[k]]; };
    auto ty = [&](size_t k) { return P.ins[in.args[k]].ty; };
    // an operand's value as a double (either lane)
    auto fd = [&](size_t k, size_t j) { return ty(k) == Ty::F64 ? L.f[in.args[k]][j] : (double)L.i[in.args[k]][j]; };
    auto row_of = [&](size_t j) -> size_t { return rowid(j, in.src); };
    switch (in.op) {
      case Ins::Konst:
        if (in.knull) std::fill(nl.begin(), nl.end(), 1);
        else if (in.ty != Ty::F64) std::fill(vi.begin(), vi.end(), in.ki);   // an integer, or a text literal's reference
        else std::fill(vf.begin(), vf.end(), in.kf);
        break;
      case Ins::ColS: {
        const ColumnStore& cs = *in.cs;
        const int64_t hi = (int64_t)((uint64_t)(in.slot + 1) << 32);
        const uint32_t* cc = L.codes(cs);
        for (size_t j = 0; j < n; ++j) {
          const size_t r = row_of(j);
          if (r == kNullRow || r >= cs.size() || cs.is_null(r)) { nl[j] = 1; continue; }
          vi[j] = hi | (int64_t)(cc ? cc[r] : vx::code_at(cs, r));
        }
        break;
      }
      case Ins::ColI: case Ins::ColF: {
        const ColumnStore& cs = *in.cs;
        const bool nulls = vx::has_nulls(cs);
        const bool as_int = vx::typed_integer(cs);
        const int64_t* ci = (in.op == Ins::ColI || as_int) ? L.ints(cs) : nullptr;
        const double* cf = (in.op == Ins::ColF && !as_int) ? L.dbls(cs) : nullptr;
        for (size_t j = 0; j < n; ++j) {
          const size_t r = row_of(j);
          if (r == kNullRow || r >= cs.size() || (nulls && cs.is_null(r))) { nl[j] = 1; continue; }
          if (in.op == Ins::ColI) vi[j] = ci ? ci[r] : vx::typed_int(cs, r);
          else if (as_int) vf[j] = (double)(ci ? ci[r] : vx::typed_int(cs, r));
          else vf[j] = cf ? cf[r] : vx::typed_value(cs, r);
        }
        break;
      }
      case Ins::CodeI: case Ins::CodeF: {
        const ColumnStore& cs = *in.cs;
        const uint32_t* cc = L.codes(cs);
        for (size_t j = 0; j < n; ++j) {
          const size_t r = row_of(j);
          if (r == kNullRow || r >= cs.size() || cs.is_null(r)) { nl[j] = 1; continue; }
          const uint32_t k = cc ? cc[r] : vx::code_at(cs, r);
          if (k >= in.code_count() || in.code_is_null(k)) { nl[j] = 1; continue; }
          if (in.op == Ins::CodeI) vi[j] = in.code_i()[k]; else vf[j] = in.code_f()[k];
        }
        break;
      }
      case Ins::ColNull: {
        const ColumnStore& cs = *in.cs;
        for (size_t j = 0; j < n; ++j) {
          const size_t r = row_of(j);
          vi[j] = (r == kNullRow || r >= cs.size() || cs.is_null(r)) ? 1 : 0;
        }
        break;
      }
      case Ins::StrIn: {
        const ColumnStore& cs = *in.cs;
        const uint32_t* cc = L.codes(cs);
        for (size_t j = 0; j < n; ++j) {
          const size_t r = row_of(j);
          if (r == kNullRow || r >= cs.size() || cs.is_null(r)) { nl[j] = 1; continue; }
          const uint32_t k = cc ? cc[r] : vx::code_at(cs, r);
          vi[j] = k < in.code_null.size() && in.code_null[k] ? 1 : 0;
        }
        break;
      }
      case Ins::Neg:
        for (size_t j = 0; j < n; ++j) {
          if (N(0)[j]) { nl[j] = 1; continue; }
          if (in.ty == Ty::I64) { if (A(0)[j] == INT64_MIN) { *err = "BIGINT value is out of range in 'expression'"; return false; } vi[j] = -A(0)[j]; }
          else vf[j] = -F(0)[j];
        }
        break;
      case Ins::Add: case Ins::Sub: case Ins::Mul:
        for (size_t j = 0; j < n; ++j) {
          if (N(0)[j] || N(1)[j]) { nl[j] = 1; continue; }
          if (in.ty == Ty::I64) {
            int64_t r = 0;
            const bool over = in.op == Ins::Add ? __builtin_add_overflow(A(0)[j], A(1)[j], &r)
                            : in.op == Ins::Sub ? __builtin_sub_overflow(A(0)[j], A(1)[j], &r)
                                                : __builtin_mul_overflow(A(0)[j], A(1)[j], &r);
            if (over) { *err = "BIGINT value is out of range in 'expression'"; return false; }
            vi[j] = r;
          } else {
            const double a = fd(0, j), b = fd(1, j);
            const double r = in.op == Ins::Add ? a + b : in.op == Ins::Sub ? a - b : a * b;
            if (!std::isfinite(r)) { *err = "DOUBLE value is out of range in 'expression'"; return false; }
            if (in.exact_dyadic) {
              // the interpreter's exact decimal: the lane must hold it exactly (a sum of halves below 2^52 does;
              // a product of two 30-bit values may not) - else the interpreter's path
              if (std::fabs(r) >= 4503599627370496.0 || (in.op == Ins::Mul && std::fabs(a) >= 1073741824.0 && std::fabs(b) >= 1073741824.0) ||
                  (in.op == Ins::Mul && std::fma(a, b, -r) != 0)) { *err = "exact decimal past a double"; return false; }
            }
            vf[j] = r;
          }
        }
        break;
      case Ins::Div:
        for (size_t j = 0; j < n; ++j) {
          if (N(0)[j] || N(1)[j]) { nl[j] = 1; continue; }
          const double b = fd(1, j);
          if (b == 0) { nl[j] = 1; continue; }
          const double r = fd(0, j) / b;
          if (!std::isfinite(r)) { *err = "DOUBLE value is out of range in 'expression'"; return false; }
          vf[j] = r;
        }
        break;
      case Ins::Mod:
        for (size_t j = 0; j < n; ++j) {
          if (N(0)[j] || N(1)[j]) { nl[j] = 1; continue; }
          if (in.ty == Ty::I64) {
            const int64_t b = A(1)[j];
            if (b == 0) { nl[j] = 1; continue; }
            vi[j] = b == -1 ? 0 : A(0)[j] % b;
          } else {
            const double b = fd(1, j);
            if (b == 0) { nl[j] = 1; continue; }
            vf[j] = std::fmod(fd(0, j), b);
          }
        }
        break;
      case Ins::IDiv:
        for (size_t j = 0; j < n; ++j) {
          if (N(0)[j] || N(1)[j]) { nl[j] = 1; continue; }
          const int64_t b = A(1)[j];
          if (b == 0) { nl[j] = 1; continue; }
          if (b == -1 && A(0)[j] == INT64_MIN) { *err = "BIGINT value is out of range in 'expression'"; return false; }
          vi[j] = A(0)[j] / b;
        }
        break;
      case Ins::Eq: case Ins::Ne: case Ins::Lt: case Ins::Le: case Ins::Gt: case Ins::Ge: {
        const bool both_int = ty(0) == Ty::I64 && ty(1) == Ty::I64;
        const bool texts = ty(0) == Ty::Str;   // then both are (the compiler saw to it)
        for (size_t j = 0; j < n; ++j) {
          if (N(0)[j] || N(1)[j]) { nl[j] = 1; continue; }
          int c;
          if (texts) c = pad_compare_ci(P.text_of(A(0)[j]), P.text_of(A(1)[j]));
          else if (both_int) { const int64_t a = A(0)[j], b = A(1)[j]; c = a < b ? -1 : a > b ? 1 : 0; }
          else { const double a = fd(0, j), b = fd(1, j); c = a < b ? -1 : a > b ? 1 : 0; }
          vi[j] = in.op == Ins::Eq ? c == 0 : in.op == Ins::Ne ? c != 0 : in.op == Ins::Lt ? c < 0
                : in.op == Ins::Le ? c <= 0 : in.op == Ins::Gt ? c > 0 : c >= 0;
        }
        break;
      }
      case Ins::And: case Ins::Or: {
        // operands as truth values: a number is true when not 0
        auto truth = [&](size_t k, size_t j) { return ty(k) == Ty::I64 ? L.i[in.args[k]][j] != 0 : L.f[in.args[k]][j] != 0; };
        for (size_t j = 0; j < n; ++j) {
          const bool an = N(0)[j], bn = N(1)[j];
          const bool a = !an && truth(0, j), b = !bn && truth(1, j);
          if (in.op == Ins::And) {
            if ((!an && !a) || (!bn && !b)) vi[j] = 0;
            else if (an || bn) nl[j] = 1;
            else vi[j] = 1;
          } else {
            if (a || b) vi[j] = 1;
            else if (an || bn) nl[j] = 1;
            else vi[j] = 0;
          }
        }
        break;
      }
      case Ins::Not:
        for (size_t j = 0; j < n; ++j) {
          if (N(0)[j]) { nl[j] = 1; continue; }
          vi[j] = (ty(0) == Ty::I64 ? A(0)[j] != 0 : F(0)[j] != 0) ? 0 : 1;
        }
        break;
      case Ins::IsNull:
        for (size_t j = 0; j < n; ++j) vi[j] = N(0)[j] ? 1 : 0;
        break;
      case Ins::NotNull:
        for (size_t j = 0; j < n; ++j) vi[j] = N(0)[j] ? 0 : 1;
        break;
      case Ins::Case: {
        // args: cond, value, cond, value, ..., [else]
        const size_t na = in.args.size();
        const bool has_else = na % 2 == 1;
        for (size_t j = 0; j < n; ++j) {
          int hit = -1;
          for (size_t k = 0; k + 1 < na; k += 2) {
            if (N(k)[j]) continue;
            const bool t = ty(k) == Ty::I64 ? L.i[in.args[k]][j] != 0 : L.f[in.args[k]][j] != 0;
            if (t) { hit = (int)(k + 1); break; }
          }
          if (hit < 0 && has_else) hit = (int)(na - 1);
          if (hit < 0 || N((size_t)hit)[j]) { nl[j] = 1; continue; }
          if (in.uns_check && P.ins[in.args[(size_t)hit]].uns && L.i[in.args[(size_t)hit]][j] < 0) {
            *err = "an unsigned CASE branch past BIGINT";
            return false;
          }
          if (in.ty != Ty::F64) vi[j] = L.i[in.args[(size_t)hit]][j]; else vf[j] = fd((size_t)hit, j);
        }
        break;
      }
      case Ins::Coalesce:
        for (size_t j = 0; j < n; ++j) {
          size_t k = 0;
          while (k < in.args.size() && N(k)[j]) ++k;
          if (k == in.args.size()) { nl[j] = 1; continue; }
          if (in.ty != Ty::F64) vi[j] = L.i[in.args[k]][j]; else vf[j] = fd(k, j);
        }
        break;
      case Ins::NullIf:
        for (size_t j = 0; j < n; ++j) {
          if (N(0)[j]) { nl[j] = 1; continue; }
          if (!N(1)[j]) {
            const bool same = ty(0) == Ty::Str ? pad_compare_ci(P.text_of(A(0)[j]), P.text_of(A(1)[j])) == 0
                            : (ty(0) == Ty::I64 && ty(1) == Ty::I64) ? A(0)[j] == A(1)[j] : fd(0, j) == fd(1, j);
            if (same) { nl[j] = 1; continue; }
          }
          if (in.ty != Ty::F64) vi[j] = A(0)[j]; else vf[j] = F(0)[j];
        }
        break;
      case Ins::Greatest: case Ins::Least:
        for (size_t j = 0; j < n; ++j) {
          bool any_null = false;
          for (size_t k = 0; k < in.args.size(); ++k) if (N(k)[j]) { any_null = true; break; }
          if (any_null) { nl[j] = 1; continue; }
          if (in.ty == Ty::I64) {
            int64_t best = A(0)[j];
            for (size_t k = 1; k < in.args.size(); ++k) { const int64_t v = A(k)[j]; if (in.op == Ins::Greatest ? v > best : v < best) best = v; }
            vi[j] = best;
          } else {
            double best = fd(0, j);
            for (size_t k = 1; k < in.args.size(); ++k) { const double v = fd(k, j); if (in.op == Ins::Greatest ? v > best : v < best) best = v; }
            vf[j] = best;
          }
        }
        break;
      case Ins::Abs: case Ins::Floor: case Ins::Ceil:
        for (size_t j = 0; j < n; ++j) {
          if (N(0)[j]) { nl[j] = 1; continue; }
          if (in.ty == Ty::I64) {
            const int64_t a = A(0)[j];
            if (in.op == Ins::Abs) { if (a == INT64_MIN) { *err = "BIGINT value is out of range in 'expression'"; return false; } vi[j] = a < 0 ? -a : a; }
            else vi[j] = a;
          } else {
            const double a = F(0)[j];
            vf[j] = in.op == Ins::Abs ? std::fabs(a) : in.op == Ins::Floor ? std::floor(a) : std::ceil(a);
          }
        }
        break;
      case Ins::Sqrt: case Ins::Sin: case Ins::Cos: case Ins::Tan: case Ins::Atan: case Ins::Radians: case Ins::Degrees:
        for (size_t j = 0; j < n; ++j) {
          if (N(0)[j]) { nl[j] = 1; continue; }
          const double a = fd(0, j);
          switch (in.op) {
            case Ins::Sqrt: if (a < 0) { nl[j] = 1; continue; } vf[j] = std::sqrt(a); break;
            case Ins::Sin: vf[j] = std::sin(a); break;
            case Ins::Cos: vf[j] = std::cos(a); break;
            case Ins::Tan: vf[j] = std::tan(a); break;
            case Ins::Atan: vf[j] = std::atan(a); break;
            // in the interpreter's order of operations, to the last bit
            case Ins::Radians: vf[j] = a * 3.14159265358979323846 / 180.0; break;
            default: vf[j] = a * 180.0 / 3.14159265358979323846; break;
          }
        }
        break;
      case Ins::Atan2: case Ins::Pow:
        for (size_t j = 0; j < n; ++j) {
          if (N(0)[j] || N(1)[j]) { nl[j] = 1; continue; }
          const double r = in.op == Ins::Atan2 ? std::atan2(fd(0, j), fd(1, j)) : std::pow(fd(0, j), fd(1, j));
          if (!std::isfinite(r)) { *err = "DOUBLE value is out of range in 'expression'"; return false; }
          vf[j] = r;
        }
        break;
      case Ins::CastI:
        // The interpreter's CAST(x AS SIGNED) of an exact integer is that integer by its digits; of an unsigned
        // value past INT64_MAX it is the wrap into SIGNED (as MariaDB's) - the lane's own bits either way.
        for (size_t j = 0; j < n; ++j) {
          if (N(0)[j]) { nl[j] = 1; continue; }
          if (ty(0) == Ty::I64) { vi[j] = A(0)[j]; continue; }
          const double a = F(0)[j];
          if (!(std::fabs(a) < 9.2e18)) { *err = "CAST past BIGINT: the interpreter's text"; return false; }
          // a DOUBLE as ROUND rounds it: half away from zero on its 15 significant digits, as written; an exact
          // decimal (a literal's lane) on its value. Writing the double out and reading it back is ~300 ns a
          // row, so it is done only where it can differ from std::round: a fraction within the rendering's
          // resolution of a half (2.4999999999999996 is written 2.5).
          if (!P.ins[in.args[0]].approx) { vi[j] = (int64_t)std::llround(a); continue; }
          vi[j] = (int64_t)round_whole_as_written(a);
        }
        break;
      case Ins::RoundF:
        for (size_t j = 0; j < n; ++j) {
          if (N(0)[j]) { nl[j] = 1; continue; }
          const double r = round_whole_as_written(F(0)[j]);
          vf[j] = r == 0 ? 0.0 : r;   // ROUND(-0.4) is 0, not -0 (its text is "0")
        }
        break;
      case Ins::CastF:
        for (size_t j = 0; j < n; ++j) {
          if (N(0)[j]) { nl[j] = 1; continue; }
          vf[j] = P.ins[in.args[0]].uns ? (double)(uint64_t)A(0)[j] : fd(0, j);
        }
        break;
      case Ins::CastDec: {
        static const double p10d[16] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15};
        const double div = p10d[in.dplaces];
        for (size_t j = 0; j < n; ++j) {
          if (N(0)[j]) { nl[j] = 1; continue; }
          AIOS_INT128 q = 0;
          if (ty(0) == Ty::I64) q = (AIOS_INT128)A(0)[j] * (int64_t)div;
          else if (!round_double_scaled(F(0)[j], in.dplaces, &q)) { *err = "CAST of a DOUBLE: the interpreter's"; return false; }
          if (q >= in.dlim || q <= -(AIOS_INT128)in.dlim) { *err = "CAST past its digits: the interpreter clamps it"; return false; }
          vf[j] = (double)(int64_t)q / div;
        }
        break;
      }
      case Ins::TupleIn: {
        // a NULL in the tuple is unknown (the set holds no NULL row to equal it); a value past a double's 53 bits
        // the interpreter compares by its digits - the interpreter's path, then
        double key[8];
        const size_t w = in.args.size();
        for (size_t j = 0; j < n; ++j) {
          bool any_null = false;
          for (size_t k = 0; k < w; ++k) {
            if (N(k)[j]) { any_null = true; break; }
            if (ty(k) == Ty::I64) {
              const int64_t v = L.i[in.args[k]][j];
              if (v > 9007199254740992LL || v < -9007199254740992LL) { *err = "row-value IN past a double"; return false; }
              key[k] = (double)v;
            } else {
              key[k] = L.f[in.args[k]][j];
            }
          }
          if (any_null) { nl[j] = 1; continue; }
          vi[j] = row_in_set_has(*in.set, key, w) ? 1 : 0;
        }
        break;
      }
      case Ins::RoundI:
        for (size_t j = 0; j < n; ++j) {
          if (N(0)[j]) { nl[j] = 1; continue; }
          // (a routine's INT coercion is val_num(std::round(n)) too: past 1e15 it prints as a double)
          if (ty(0) == Ty::I64) {
            if (A(0)[j] >= 1000000000000000LL || A(0)[j] <= -1000000000000000LL) { *err = "INT past 1e15: the interpreter's text"; return false; }
            vi[j] = A(0)[j];
            continue;
          }
          const double a = std::round(F(0)[j]);
          if (!(std::fabs(a) < 1e15)) { *err = "INT past 1e15: the interpreter's text"; return false; }
          vi[j] = (int64_t)a;
        }
        break;
      case Ins::BitAnd: case Ins::BitOr: case Ins::BitXor: case Ins::Shl: case Ins::Shr: case Ins::BitNot: {
        // as the interpreter reads an operand: a whole number's bits; a fraction rounded first
        auto u = [&](size_t k, size_t j) -> uint64_t {
          if (ty(k) == Ty::I64) return (uint64_t)L.i[in.args[k]][j];
          double d = std::round(L.f[in.args[k]][j]);
          if (d <= -9223372036854775808.0) return (uint64_t)1 << 63;
          if (d >= 18446744073709551615.0) return ~(uint64_t)0;
          return d < 0 ? (uint64_t)(int64_t)d : (uint64_t)d;
        };
        for (size_t j = 0; j < n; ++j) {
          if (N(0)[j] || (in.op != Ins::BitNot && N(1)[j])) { nl[j] = 1; continue; }
          const uint64_t l = u(0, j);
          uint64_t r = 0;
          if (in.op == Ins::BitNot) r = ~l;
          else {
            const uint64_t m = u(1, j);
            switch (in.op) {
              case Ins::BitAnd: r = l & m; break;
              case Ins::BitOr: r = l | m; break;
              case Ins::BitXor: r = l ^ m; break;
              case Ins::Shl: r = m >= 64 ? 0 : l << m; break;
              default: r = m >= 64 ? 0 : l >> m; break;
            }
          }
          vi[j] = (int64_t)r;
        }
        break;
      }
    }
  }
  return true;
}

// Evaluates P over n rows, rowid(j, src) being row j's row in source src (or kNullRow). False with *err set when
// a value the interpreter would refuse comes up (an integer out of range), and the caller takes the interpreter's
// path. With native code (P.jit): the leaves through their readers, the rest as one native loop writing the
// output lane; a bail runs the interpreter over the rest, which reports as it always has.
template <typename RowId>
inline bool eval(const Prog& P, RowId&& rowid, size_t n, Lanes& L, std::string* err) {
  const size_t K = P.ins.size();
  L.i.resize(K); L.f.resize(K); L.n.resize(K);
  if (!P.jit) {
    bool can_hoist = P.hoisted && n > 1 && !vjit_check_mode();
    for (uint32_t s = 0; can_hoist && s < 64; ++s) {
      if (!((P.hoist_srcs >> s) & 1)) continue;
      const size_t r0 = rowid(0, s);
      for (size_t j = 1; j < n; ++j) if (rowid(j, s) != r0) { can_hoist = false; break; }
    }
    for (size_t x = 0; x < K; ++x) {
      if (can_hoist && x < P.inv.size() && P.inv[x]) {
        if (!eval_ins(P, x, rowid, 1, L, err)) return false;
        const uint8_t n0 = L.n[x][0];
        L.n[x].assign(n, n0);
        if (P.ins[x].ty != Ty::F64) {
          const int64_t v0 = L.i[x][0];
          if (!n0) L.i[x].assign(n, v0); else L.i[x].resize(n);
        } else {
          const double v0 = L.f[x][0];
          if (!n0) L.f[x].assign(n, v0); else L.f[x].resize(n);
        }
      } else {
        if (!eval_ins(P, x, rowid, n, L, err)) return false;
      }
    }
    return true;
  }
  // The leaves the native code gathers itself: described from their columns as they are now, with the chunk's
  // row ids per source; the others through their readers, into lanes.
  auto native_leaf = [&](size_t x) { return x < P.leaf_kind.size() && P.leaf_kind[x] != LeafLane; };
  for (size_t x = 0; x < K; ++x) if (vjit_leaf(P.ins[x].op) && !native_leaf(x) && !eval_ins(P, x, rowid, n, L, err)) return false;
  const void* li[256]; const void* lf[256]; const void* ln[256];
  LeafDesc leaves[256];
  const size_t* rid[64] = {nullptr};
  size_t nsrc = 0;
  uint64_t hoist = P.hoisted ? 1 : 0;   // cleared where a hoisted source's row id varies over the chunk
  for (size_t x = 0; x < K; ++x) {
    const bool leaf = vjit_leaf(P.ins[x].op) && !native_leaf(x);
    li[x] = leaf && P.ins[x].ty != Ty::F64 ? (const void*)L.i[x].data() : nullptr;
    lf[x] = leaf && P.ins[x].ty == Ty::F64 ? (const void*)L.f[x].data() : nullptr;
    ln[x] = leaf ? (const void*)L.n[x].data() : nullptr;
    if (!native_leaf(x)) continue;
    const Ins& in = P.ins[x];
    LeafDesc& d = leaves[x];
    d = LeafDesc();
    d.kind = leaf_kind_of(in);
    d.src = in.src;
    if (in.op == Ins::Konst) {
      if (in.ty == Ty::F64) std::memcpy(&d.imm, &in.kf, 8); else d.imm = (uint64_t)in.ki;
      d.kind = in.knull ? LeafKonst : LeafKonst;   // knull: the code sets NULL (imm unused)
      d.table_n = in.knull ? 1 : 0;
      continue;
    }
    const ColumnStore& cs = *in.cs;
    d.size = cs.size();
    d.nulls = cs.null_bitmap.data();
    d.nulls_bytes = cs.null_bitmap.size();
    if ((in.op == Ins::ColI || in.op == Ins::ColF) && !vx::has_nulls(cs)) d.nulls_bytes = 0;   // as their readers skip the bitmap
    if (d.kind == LeafI64) d.data = cs.int64_data.data();
    else if (d.kind == LeafF64) d.data = cs.double_data.data();
    else if (d.kind != LeafColNull) d.data = cs.data32.data();
    if (in.op == Ins::CodeI) { d.table = in.code_i().data(); d.table_null = in.codes ? in.codes->null.data() : in.code_null.data(); d.table_n = in.code_count(); }
    else if (in.op == Ins::CodeF) { d.table = in.code_f().data(); d.table_null = in.codes ? in.codes->null.data() : in.code_null.data(); d.table_n = in.code_count(); }
    else if (in.op == Ins::StrIn) { d.table_null = in.code_null.data(); d.table_n = in.code_null.size(); }
    else if (in.op == Ins::ColS) d.imm = (uint64_t)(in.slot + 1) << 32;
    if (in.src >= 64) { d.kind = LeafLane; continue; }   // a source past the row-id table: the code bails to the interpreter
    if (in.src + 1 > nsrc) nsrc = in.src + 1;
    if (L.ridc.size() <= in.src) L.ridc.resize(in.src + 1);
    if (!rid[in.src]) {
      std::vector<size_t>& r = L.ridc[in.src];
      r.resize(n);
      for (size_t j = 0; j < n; ++j) r[j] = rowid(j, in.src);
      if (hoist && (P.hoist_srcs >> in.src) & 1)
        for (size_t j = 1; j < n; ++j) if (r[j] != r[0]) { hoist = 0; break; }
      rid[in.src] = r.data();
    }
  }
  const size_t o = K - 1;
  L.n[o].resize(n);
  if (P.out().ty != Ty::F64) L.i[o].resize(n); else L.f[o].resize(n);
  const int rc = vjit_run(P.jit.get(), n, li, lf, ln, L.i[o].data(), L.f[o].data(), L.n[o].data(), leaves, rid, hoist);
  if (rc == 0 && !vjit_check_mode()) return true;
  if (rc == 2) {   // a column laid out differently than the code was built for: the interpreter, the whole chunk
    for (size_t x = 0; x < K; ++x) if (!eval_ins(P, x, rowid, n, L, err)) return false;
    return true;
  }
  if (rc == 0) {
    // check mode: the interpreter's answer must be these bytes exactly
    std::vector<int64_t> ji = L.i[o]; std::vector<double> jf = L.f[o]; std::vector<uint8_t> jn = L.n[o];
    for (size_t x = 0; x < K; ++x) if ((!vjit_leaf(P.ins[x].op) || native_leaf(x)) && !eval_ins(P, x, rowid, n, L, err)) {
      std::fprintf(stderr, "[vjit] check: native code answered where the interpreter refuses: %s\n", err->c_str());
      std::abort();
    }
    const bool same_n = std::memcmp(jn.data(), L.n[o].data(), n) == 0;
    bool same_v = true;
    for (size_t j = 0; j < n && same_v; ++j) {
      if (L.n[o][j]) continue;   // a NULL's value bytes are not compared
      same_v = P.out().ty != Ty::F64 ? ji[j] == L.i[o][j] : std::memcmp(&jf[j], &L.f[o][j], 8) == 0;
    }
    if (!same_n || !same_v) {
      size_t j = 0;
      for (; j < n; ++j) if (jn[j] != L.n[o][j] || (!L.n[o][j] && (P.out().ty != Ty::F64 ? ji[j] != L.i[o][j] : std::memcmp(&jf[j], &L.f[o][j], 8) != 0))) break;
      std::fprintf(stderr, "[vjit] check: mismatch at row %zu of %zu: native null=%d value=%lld/%.17g, interpreter null=%d value=%lld/%.17g (out op %d)\n",
                   j, n, (int)jn[j], (long long)(j < ji.size() ? ji[j] : 0), j < jf.size() ? jf[j] : 0.0, (int)L.n[o][j],
                   (long long)(j < L.i[o].size() ? L.i[o][j] : 0), j < L.f[o].size() ? L.f[o][j] : 0.0, (int)P.out().op);
      std::abort();
    }
    return true;
  }
  // bail: the interpreter decides the chunk (and sets *err where it refuses)
  for (size_t x = 0; x < K; ++x) if ((!vjit_leaf(P.ins[x].op) || native_leaf(x)) && !eval_ins(P, x, rowid, n, L, err)) return false;
  if (vjit_check_mode()) {
    std::string ops;
    for (const Ins& in : P.ins) { ops += std::to_string((int)in.op) + (in.ty == Ty::F64 ? "f" : in.ty == Ty::Str ? "s" : "i") + "("; for (uint32_t a : in.args) ops += std::to_string(a) + ","; ops += ") "; }
    std::fprintf(stderr, "[vjit] check: native code bailed where the interpreter accepts (out op %d): %s\n", (int)P.out().op, ops.c_str());
    std::abort();
  }
  return true;
}

// The result lane's value of row j as the Val the interpreter would have made.
inline Val value_at(const Prog& P, const Lanes& L, size_t j) {
  const Ins& o = P.out();
  const size_t K = P.ins.size() - 1;
  if (L.n[K][j]) return val_null();
  if (o.ty == Ty::F64) return o.approx ? val_approx(L.f[K][j]) : val_num(L.f[K][j]);
  if (o.ty == Ty::Str) return val_text(std::string(P.text_of(L.i[K][j])));
  const int64_t v = L.i[K][j];
  if (o.uns) { const uint64_t u = (uint64_t)v; return val_num_with_text((double)u, std::to_string(u)); }
  return val_exact((AIOS_INT128)v, 0);
}

// The result lane's value of row j printed as Val::str() prints it.
inline std::string text_at(const Prog& P, const Lanes& L, size_t j) {
  const Ins& o = P.out();
  const size_t K = P.ins.size() - 1;
  if (L.n[K][j]) return sql_null();
  if (o.ty == Ty::F64) {
    const double d = L.f[K][j];
    if (d == 0 && o.approx && std::signbit(d)) return "-0";
    if (d == std::floor(d) && std::fabs(d) < 1e15) return std::to_string((long long)d);
    return sql_format_double(d, 0);
  }
  if (o.ty == Ty::Str) return std::string(P.text_of(L.i[K][j]));
  if (o.uns) return std::to_string((uint64_t)L.i[K][j]);
  return std::to_string((long long)L.i[K][j]);
}

// A program's result over every row of a set: one value per row, compact (a lane and a null byte), read back as
// the Val or the text the interpreter would have given.
struct Column {
  Ty ty = Ty::I64;
  bool approx = false, uns = false;
  std::vector<int64_t> i;
  std::vector<double> f;
  std::vector<uint8_t> n;
  std::vector<std::string> s;   // Str: the text per row
  size_t size() const { return n.size(); }
  bool is_null(size_t j) const { return n[j] != 0; }
  Val at(size_t j) const {
    if (n[j]) return val_null();
    if (ty == Ty::Str) return val_text(s[j]);
    if (ty == Ty::F64) return approx ? val_approx(f[j]) : val_num(f[j]);
    if (uns) { const uint64_t u = (uint64_t)i[j]; return val_num_with_text((double)u, std::to_string(u)); }
    return val_exact((AIOS_INT128)i[j], 0);
  }
  std::string text(size_t j) const {
    if (n[j]) return sql_null();
    if (ty == Ty::Str) return s[j];
    if (ty == Ty::F64) {
      const double d = f[j];
      if (d == 0 && approx && std::signbit(d)) return "-0";
      if (d == std::floor(d) && std::fabs(d) < 1e15) return std::to_string((long long)d);
      return sql_format_double(d, 0);
    }
    if (uns) return std::to_string((uint64_t)i[j]);
    return std::to_string((long long)i[j]);
  }
};

// P over rows 0..total (rowat(j) the row), in chunks, into *out. False (with *err) where eval fails.
// `shared`: a Lanes kept by the caller across the programs it runs over the same row set (a projection's items):
// the row ids per source and the decoded columns are then worked out once for all of them.
template <typename RowAt>
inline bool eval_column(const Prog& P, RowAt&& rowat, size_t total, Column* out, std::string* err, Lanes* shared = nullptr) {
  const Ins& o = P.out();
  out->ty = o.ty; out->approx = o.approx; out->uns = o.uns;
  out->n.assign(total, 0);
  if (o.ty == Ty::Str) out->s.assign(total, std::string());
  else if (o.ty == Ty::I64) out->i.assign(total, 0); else out->f.assign(total, 0);
  Lanes own;
  Lanes& L = shared ? *shared : own;
  if (shared) L.reset_for(total); else L.total_rows = total;
  // the sources the program's leaves read: their row ids over the whole set, once
  std::vector<const size_t*> rs;
  for (const Ins& in : P.ins) {
    if (!in.cs) continue;
    if (rs.size() <= in.src) rs.resize(in.src + 1, nullptr);
    if (!rs[in.src]) rs[in.src] = L.rows_in(in.src, rowat);
  }
  const size_t chunk = 4096;
  const size_t K = P.ins.size() - 1;
  for (size_t at = 0; at < total; at += chunk) {
    const size_t n = std::min(chunk, total - at);
    auto rowid = [&](size_t j, uint32_t src) -> size_t { return src < rs.size() && rs[src] ? rs[src][at + j] : kNullRow; };
    if (!eval(P, rowid, n, L, err)) return false;
    std::memcpy(out->n.data() + at, L.n[K].data(), n);
    if (o.ty == Ty::Str) { for (size_t j = 0; j < n; ++j) if (!L.n[K][j]) out->s[at + j] = std::string(P.text_of(L.i[K][j])); }
    else if (o.ty == Ty::I64) std::memcpy(out->i.data() + at, L.i[K].data(), n * sizeof(int64_t));
    else std::memcpy(out->f.data() + at, L.f[K].data(), n * sizeof(double));
  }
  return true;
}

// P over rows given as one row-id column per source (a star join's arrays: ids[src][first + j], UINT32_MAX for a
// NULL-padded row), rows first .. first + total: no joined row made.
inline bool eval_column_srcs(const Prog& P, const std::vector<const uint32_t*>& ids, size_t first, size_t total, Column* out, std::string* err) {
  const Ins& o = P.out();
  out->ty = o.ty; out->approx = o.approx; out->uns = o.uns;
  out->n.assign(total, 0);
  if (o.ty == Ty::Str) out->s.assign(total, std::string());
  else if (o.ty == Ty::I64) out->i.assign(total, 0); else out->f.assign(total, 0);
  Lanes L;
  L.total_rows = total;
  const size_t chunk = 4096;
  const size_t K = P.ins.size() - 1;
  for (size_t at = 0; at < total; at += chunk) {
    const size_t n = std::min(chunk, total - at);
    auto rowid = [&](size_t j, uint32_t src) -> size_t {
      if (src >= ids.size() || !ids[src]) return kNullRow;
      const uint32_t r = ids[src][first + at + j];
      return r == UINT32_MAX ? kNullRow : (size_t)r;
    };
    if (!eval(P, rowid, n, L, err)) return false;
    std::memcpy(out->n.data() + at, L.n[K].data(), n);
    if (o.ty == Ty::Str) { for (size_t j = 0; j < n; ++j) if (!L.n[K][j]) out->s[at + j] = std::string(P.text_of(L.i[K][j])); }
    else if (o.ty == Ty::I64) std::memcpy(out->i.data() + at, L.i[K].data(), n * sizeof(int64_t));
    else std::memcpy(out->f.data() + at, L.f[K].data(), n * sizeof(double));
  }
  return true;
}

// P over a single table's rows: ids(j) is row j's row id in that table (the program's only source, 0).
template <typename Ids>
inline bool eval_column_ids(const Prog& P, Ids&& ids, size_t total, Column* out, std::string* err) {
  const Ins& o = P.out();
  out->ty = o.ty; out->approx = o.approx; out->uns = o.uns;
  out->n.assign(total, 0);
  if (o.ty == Ty::Str) out->s.assign(total, std::string());
  else if (o.ty == Ty::I64) out->i.assign(total, 0); else out->f.assign(total, 0);
  Lanes L;
  L.total_rows = total;
  const size_t chunk = 4096;
  const size_t K = P.ins.size() - 1;
  for (size_t at = 0; at < total; at += chunk) {
    const size_t n = std::min(chunk, total - at);
    auto rowid = [&](size_t j, uint32_t src) -> size_t { return src == 0 ? (size_t)ids(at + j) : kNullRow; };
    if (!eval(P, rowid, n, L, err)) return false;
    std::memcpy(out->n.data() + at, L.n[K].data(), n);
    if (o.ty == Ty::Str) { for (size_t j = 0; j < n; ++j) if (!L.n[K][j]) out->s[at + j] = std::string(P.text_of(L.i[K][j])); }
    else if (o.ty == Ty::I64) std::memcpy(out->i.data() + at, L.i[K].data(), n * sizeof(int64_t));
    else std::memcpy(out->f.data() + at, L.f[K].data(), n * sizeof(double));
  }
  return true;
}

// Whether P, compiled against ctx's sources, still reads the columns it was compiled over: the same source list
// and the same column stores (a statement's sources do not move while it runs; a cached plan's may).
inline bool valid(const Prog& P, const EvalCtx& ctx) {
  if (!ctx.sources) return false;
  for (const Ins& in : P.ins) {
    if (!in.cs) continue;
    if (in.src >= ctx.sources->size()) return false;
    const SqlTable* t = (*ctx.sources)[in.src].table;
    if (!t) return false;
    bool found = false;
    for (const ColumnStore& cs : t->col_data)
      if (&cs == in.cs) { found = true; break; }
    if (!found || in.cs->size() != t->row_count()) return false;
    // a dictionary that has grown since: codes the tables do not know
    if (in.codes && in.codes->temporal) {
      // a date column's table: the same base and code width, or the codes mean other days now
      if (in.cs->encoding != ColumnEncoding::TemporalDays || (int)in.cs->bit_width_ != in.codes->bits || in.cs->base_int64 != in.codes->base) return false;
    } else if ((in.op == Ins::CodeI || in.op == Ins::CodeF || in.op == Ins::StrIn) && vx::dict_size(*in.cs) != in.code_count()) {
      return false;
    }
  }
  // a column the native code reads itself laid out differently now (re-encoded): built again for the new layout
  if (P.jit)
    for (size_t x = 0; x < P.ins.size() && x < P.leaf_kind.size(); ++x)
      if (P.leaf_kind[x] != LeafLane && leaf_kind_of(P.ins[x]) != P.leaf_kind[x]) return false;
  return true;
}

// The program for e over ctx's sources, compiled once per expression and source list and kept on the expression;
// null when it does not compile. A program over a column store that has since changed is compiled afresh. Only
// the statement's own thread compiles (and writes the cache); a worker thread reads what it finds, so the
// aggregates a parallel emit evaluates are compiled before it starts (vp::warm).
inline const Prog* program_for(const SqlExpr* e, const EvalCtx& ctx, bool* retry_out = nullptr) {
  if (retry_out) *retry_out = false;
  if (!enabled() || !e || !ctx.sources) return nullptr;
  if (e->vprog_scope == (const void*)ctx.sources) {
    const Prog* P = static_cast<const Prog*>(e->vprog.get());
    if (P && P->pg_division == tls_pg_division && valid(*P, ctx) &&
        (!P->has_fn || (ctx.db && P->fn_gen == ctx.db->programs_generation()))) {
      std::string sig;
      literal_sig(e, &sig);
      if (sig == P->lit_sig) return P;
    } else if (e->vprog_none && !P) {
      // it did not compile last time; with other literals it might, so only the same literals are trusted
      std::string sig;
      literal_sig(e, &sig);
      if (sig == e->vprog_none_sig) return nullptr;
    }
  }
  if (SqlThreadPool::is_in_worker_thread()) return nullptr;
  std::string why;
  bool retry = false;
  std::shared_ptr<Prog> P = compile(e, ctx, &why, &retry);
  if (retry_out) *retry_out = retry;
  if (vx_trace_on() && e->op != ExprOp::Literal && e->op != ExprOp::Column)
    vx_trace((P ? "vp: compiled (" + std::to_string(P->ins.size()) + " ops) " : "vp: interpreted (" + why + ") ") + expr_text(e));
  e->vprog_scope = ctx.sources;
  e->vprog_none = !P && !retry;
  if (!P) { e->vprog_none_sig.clear(); literal_sig(e, &e->vprog_none_sig); }
  e->vprog = P;
  return P.get();
}

// Compiles, on the statement's thread, the argument of every aggregate in e (MIN(x) in MIN(x) + 1 too), so a
// worker evaluating the aggregate over a group finds the program ready (group_values).
inline void warm(const SqlExpr* e, const EvalCtx& ctx, int depth = 0) {
  if (!e || depth > 64 || !enabled()) return;
  if (e->op == ExprOp::Func && !e->window && is_aggregate_name(func_upper(e)) && !e->negated && e->args.size() == 1 &&
      e->args[0] && e->args[0]->op != ExprOp::Star) {
    program_for(e->args[0].get(), ctx);
    return;
  }
  for (const auto& a : e->args) warm(a.get(), ctx, depth + 1);
}

}  // namespace vp
