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

enum class Ty : uint8_t { I64, F64 };

struct Ins {
  enum Op : uint8_t {
    Konst, ColI, ColF, CodeI, CodeF, ColNull, StrIn,
    Neg, Add, Sub, Mul, Div, Mod, IDiv,
    Eq, Ne, Lt, Le, Gt, Ge, And, Or, Not, IsNull, NotNull,
    Case, Coalesce, NullIf, Greatest, Least, Abs, Floor, Ceil, Sqrt, Pow, Sin, Cos, Tan, Atan, Atan2, Radians,
    Degrees, CastI, CastF, BitAnd, BitOr, BitXor, Shl, Shr, BitNot
  } op = Konst;
  Ty ty = Ty::I64;
  bool approx = false;    // the interpreter's Val here would be approximate (a DOUBLE)
  bool dec = false;       // an exact decimal literal with a fraction (0.5): a double lane, exact to the interpreter
  // The interpreter's value here may be an exact decimal at some rows and a double at others (GREATEST(0.0, d)):
  // the number is the lane's, the text may not be - so a tainted value is read only by what erases text (a CAST
  // to SIGNED, a comparison, a truth test), never printed.
  bool taint = false;
  bool uns = false;       // the bit operators' unsigned 64-bit result, printed as such
  bool knull = false;
  double kf = 0;
  int64_t ki = 0;
  uint32_t src = 0;       // a column's source
  const ColumnStore* cs = nullptr;
  std::vector<uint32_t> args;
  std::vector<int64_t> code_i;      // CodeI: per dictionary code
  std::vector<double> code_f;       // CodeF
  std::vector<uint8_t> code_null;   // CodeI / CodeF: 1 where the code is NULL; StrIn: 1 where the code matches
};

struct Prog {
  std::vector<Ins> ins;   // operands before the operators that read them; the last is the result
  // The literals the program was compiled with, and the division rule: a cached statement shape rebinds its
  // literals in place (shape_bind), and the program on the expression is then stale - program_for checks.
  std::string lit_sig;
  bool pg_division = false;
  const Ins& out() const { return ins.back(); }
};

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

struct Compiler {
  const EvalCtx& ctx;
  Prog& P;
  bool ok = true;
  int depth = 0;
  std::string why;   // the first reason the expression did not compile (for the trace)

  uint32_t emit(Ins in) { P.ins.push_back(std::move(in)); return (uint32_t)P.ins.size() - 1; }
  uint32_t fail(const char* reason = "") { if (ok) why = reason; ok = false; return 0; }
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

  // A dictionary's codes as numbers, once.
  bool codes_of(const ColumnStore& cs, bool as_int, Ins* in) {
    if (cs.shared_dict) return false;
    const size_t n = vx::dict_size(cs);
    if (n == 0 || n > (1u << 20)) return false;
    in->code_null.assign(n, 0);
    if (as_int) in->code_i.assign(n, 0); else in->code_f.assign(n, 0);
    for (size_t k = 0; k < n; ++k) {
      const std::string& s = vx::dict_entry(cs, (uint32_t)k);
      if (sql_is_null(s)) { in->code_null[k] = 1; continue; }
      double d = 0;
      if (!parse_number(s, &d)) return false;
      if (as_int) {
        if (d != std::floor(d) || std::fabs(d) >= 9.2e18) return false;
        char* end = nullptr;
        errno = 0;
        const long long v = std::strtoll(s.c_str(), &end, 10);
        if (errno == ERANGE || !end || *end != '\0') return false;
        in->code_i[k] = v;
      } else {
        in->code_f[k] = d;
      }
    }
    return true;
  }

  uint32_t column(const SqlExpr* e) {
    if (is_user_var_name(e->name) || e->name.rfind("@@", 0) == 0) return fail("variable");
    size_t s = 0, c = 0;
    if (!place(e, &s, &c)) return fail("column not in this scope");
    const SqlTable* t = (*ctx.sources)[s].table;
    const SqlColumn& col = t->columns[c];
    const ColumnStore& cs = t->col_data[c];
    if (col.temporal) return fail("temporal column");
    Ins in;
    in.src = (uint32_t)s;
    in.cs = &cs;
    if (col.type == SqlDataType::Int) {
      in.ty = Ty::I64;
      if (vx::typed_integer(cs)) in.op = Ins::ColI;
      else if (vx::dict_coded(cs) && codes_of(cs, true, &in)) in.op = Ins::CodeI;
      else return fail("INT column not typed or dictionary-coded");
    } else if (col.type == SqlDataType::Real) {
      in.ty = Ty::F64;
      in.approx = true;
      if (vx::typed_numeric(cs)) in.op = Ins::ColF;
      else if (vx::dict_coded(cs) && codes_of(cs, false, &in)) in.op = Ins::CodeF;
      else return fail("DOUBLE column not typed or dictionary-coded");
    } else {
      if (ok) why = std::string(col.type == SqlDataType::Decimal ? "DECIMAL" : "text") + " column " + t->name + "." + col.name;
      return fail();
    }
    return emit(std::move(in));
  }

  uint32_t literal(const Val& v) {
    Ins in;
    in.op = Ins::Konst;
    if (v.null) { in.knull = true; in.ty = Ty::F64; in.approx = true; return emit(std::move(in)); }
    if (!v.numeric) return fail("text literal");
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
    return emit(std::move(in));
  }

  // `text_col = 'lit'`, `text_col IN ('a', 'b')`: the codes that match, compared as = compares text.
  uint32_t str_in(const SqlExpr* col, const std::vector<const SqlExpr*>& lits) {
    size_t s = 0, c = 0;
    if (!place(col, &s, &c)) return fail();
    const SqlTable* t = (*ctx.sources)[s].table;
    const SqlColumn& sc = t->columns[c];
    const ColumnStore& cs = t->col_data[c];
    // (a result table's "text" column may hold numbers, which = reads as numbers: not here)
    if (sc.type != SqlDataType::Text || sc.temporal || t->inferred_types || cs.shared_dict || !vx::dict_coded(cs)) return fail();
    const size_t n = vx::dict_size(cs);
    if (n == 0 || n > (1u << 20)) return fail();
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

  // Both operands' numbers as one lane type: doubles if either is one.
  bool numeric_pair(uint32_t a, uint32_t b, Ty* ty, bool* approx) {
    const Ins& x = at(a);
    const Ins& y = at(b);
    if (x.uns || y.uns) return false;
    *ty = (x.ty == Ty::F64 || y.ty == Ty::F64) ? Ty::F64 : Ty::I64;
    *approx = x.approx || y.approx;
    // An exact decimal (or a value that may be one) beside an exact number: DECIMAL arithmetic, whose digits are
    // the interpreter's (0.1 * 3 is 0.3, not 0.30000000000000004). Beside a double it is a double's arithmetic
    // on the same number; the result stays tainted where the decimal could have won a GREATEST.
    // (a tainted double - one that could be a decimal at some rows - beside an exact integer is the double's
    // arithmetic at the rows that matter; at a row where the decimal won, an exact result the double also
    // holds, as 0.0 * 4096 and 131071.0 * 4096 are)
    auto exact_decimal = [](const Ins& v) { return v.dec || (v.taint && !v.approx); };
    if ((exact_decimal(x) && !y.approx && !y.knull) || (exact_decimal(y) && !x.approx && !x.knull)) return false;
    return true;
  }
  bool tainted(uint32_t a, uint32_t b) const { return decimalish(at(a)) || decimalish(at(b)); }

  uint32_t compile(const SqlExpr* e) {
    if (!ok) return 0;
    if (!e || ++depth > 64) return fail();
    struct Depth { int& d; ~Depth() { --d; } } dd{depth};
    switch (e->op) {
      case ExprOp::Literal: return literal(e->literal);
      case ExprOp::Column: return column(e);
      case ExprOp::Neg: {
        if (e->args.size() != 1) return fail();
        const uint32_t a = compile(e->args[0].get());
        if (!ok) return 0;
        if (at(a).uns) return fail();
        return unary(Ins::Neg, a, at(a).ty, at(a).approx, decimalish(at(a)));
      }
      case ExprOp::Add: case ExprOp::Sub: case ExprOp::Mul: case ExprOp::Div: case ExprOp::Mod: {
        if (e->args.size() != 2) return fail();
        const uint32_t a = compile(e->args[0].get());
        const uint32_t b = compile(e->args[1].get());
        if (!ok) return 0;
        Ty ty; bool approx;
        if (!numeric_pair(a, b, &ty, &approx)) return fail("decimal arithmetic");
        if (e->op == ExprOp::Div) {
          if (tls_pg_division || !approx) return fail("exact division");   // two exact operands divide as decimals
          return binary(Ins::Div, a, b, Ty::F64, true, tainted(a, b));
        }
        if (e->op == ExprOp::Mod && ty == Ty::F64 && !approx) return fail("decimal MOD");   // exact decimals' MOD
        const Ins::Op op = e->op == ExprOp::Add ? Ins::Add : e->op == ExprOp::Sub ? Ins::Sub : e->op == ExprOp::Mul ? Ins::Mul : Ins::Mod;
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
        const uint32_t a = compile(l);
        const uint32_t b = compile(r);
        if (!ok) return 0;
        if (at(a).uns || at(b).uns) return fail();
        const Ins::Op op = e->op == ExprOp::Eq ? Ins::Eq : e->op == ExprOp::Ne ? Ins::Ne : e->op == ExprOp::Lt ? Ins::Lt
                         : e->op == ExprOp::Le ? Ins::Le : e->op == ExprOp::Gt ? Ins::Gt : Ins::Ge;
        return binary(op, a, b, Ty::I64, false);
      }
      case ExprOp::And: case ExprOp::Or: {
        if (e->args.size() != 2) return fail();
        const uint32_t a = compile(e->args[0].get());
        const uint32_t b = compile(e->args[1].get());
        if (!ok) return 0;
        return binary(e->op == ExprOp::And ? Ins::And : Ins::Or, a, b, Ty::I64, false);
      }
      case ExprOp::Not: {
        if (e->args.size() != 1) return fail();
        const uint32_t a = compile(e->args[0].get());
        if (!ok) return 0;
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
        if (e->negated || e->args.size() < 2) return fail();   // the simple CASE x WHEN v: not here
        Ins in;
        in.op = Ins::Case;
        bool first = true;
        for (size_t i = 0; i < e->args.size(); ++i) {
          const uint32_t a = compile(e->args[i].get());
          if (!ok) return 0;
          in.args.push_back(a);
          const bool is_value = (i % 2 == 1) || (i + 1 == e->args.size() && e->args.size() % 2 == 1);
          if (!is_value) continue;
          if (!same_kind(in, at(a), &first)) return fail("CASE branches of mixed kinds");
        }
        if (first) return fail("CASE of no values");
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
          if (at(a).uns) return fail();
          return unary(op, a, ty, approx);
        };
        if (up == "ABS"sv || up == "FLOOR"sv || up == "CEIL"sv || up == "CEILING"sv) {
          if (n != 1) return fail();
          const uint32_t a = compile(e->args[0].get());
          if (!ok) return 0;
          if (at(a).uns) return fail();
          // FLOOR of an exact decimal is an exact integer, of a double a double: the same number, so tainted
          return unary(up == "ABS"sv ? Ins::Abs : up == "FLOOR"sv ? Ins::Floor : Ins::Ceil, a, at(a).ty, at(a).approx, decimalish(at(a)));
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
          if (at(a).uns || at(b).uns) return fail();
          return binary(up == "ATAN2"sv ? Ins::Atan2 : Ins::Pow, a, b, Ty::F64, true);   // a DOUBLE: no taint
        }
        if (up == "__INTDIV"sv) {
          if (n != 2) return fail();
          const uint32_t a = compile(e->args[0].get());
          const uint32_t b = compile(e->args[1].get());
          if (!ok) return 0;
          if (at(a).ty != Ty::I64 || at(b).ty != Ty::I64 || at(a).uns || at(b).uns) return fail();
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
          if (at(a).dec || at(b).dec) return fail();
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
          if (at(a).dec) return fail();
          Ins in; in.op = Ins::BitNot; in.ty = Ty::I64; in.uns = true; in.args = {a};
          return emit(std::move(in));
        }
        if (up == "CAST"sv) {
          if (n < 2 || !e->args[1] || e->args[1]->op != ExprOp::Literal) return fail();
          const std::string target = upper_of(e->args[1]->literal.str());
          const std::string_view pg = n > 2 && e->args[2] ? e->args[2]->literal.sv() : std::string_view();
          if (!pg.empty()) return fail();   // x::type's own roundings
          const uint32_t a = compile(e->args[0].get());
          if (!ok) return 0;
          if (at(a).uns) return fail();
          // CAST(x AS SIGNED) rounds half away from zero, an exact decimal on its digits and a double on its 15
          // significant digits as written (round_double_as_written): the same integer for either reading of
          // the lane's number. CAST AS DOUBLE is a DOUBLE of either. Both erase the text, and the taint.
          if (target.rfind("SIGNED", 0) == 0) return unary(Ins::CastI, a, Ty::I64, false);
          if (target.rfind("DOUBLE", 0) == 0 || target.rfind("FLOAT", 0) == 0 || target.rfind("REAL", 0) == 0)
            return unary(Ins::CastF, a, Ty::F64, true);
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
            // NULLIF(x, y) is x or NULL: y only decides, and may be of any numeric kind
            if (up == "NULLIF"sv && i == 1) { if (at(a).uns) return fail(); continue; }
            if (!same_kind(in, at(a), &first)) return fail();
          }
          if (first) return fail();
          return emit(std::move(in));
        }
        if (ok) why = "function " + up;
        return fail();
      }
      default:
        if (ok) why = "operator " + std::to_string((int)e->op);
        return fail();
    }
  }

  // The values a CASE / COALESCE / GREATEST may give are of one kind: all integers (exact) or all doubles
  // (approximate); a NULL fits either. Mixed, the interpreter's answer type would change row by row.
  // An exact decimal among doubles (GREATEST(0.0, d), LEAST(d, 131071.0)) is allowed: the number is right at every
  // row, the text may not be, so the result is tainted and only erased by a CAST, a comparison or a truth test.
  bool same_kind(Ins& in, const Ins& v, bool* first) {
    if (v.uns) return false;
    if (v.knull) return true;
    const bool vf = v.ty == Ty::F64;   // a double or a decimal
    if (*first) { in.ty = v.ty; in.approx = v.approx; in.taint = decimalish(v); *first = false; return true; }
    if (in.ty == Ty::I64) return v.ty == Ty::I64 && !decimalish(v);
    if (!vf) return false;
    in.approx = in.approx || v.approx;
    in.taint = in.taint || decimalish(v);
    return true;
  }
};

// An expression as text for the trace: its source text where the parser kept it, else its shape.
inline std::string expr_text(const SqlExpr* e, int depth = 0) {
  if (!e) return "?";
  if (!e->source_text.empty() || depth > 6) return e->source_text;
  std::string s = e->op == ExprOp::Func ? e->name : e->op == ExprOp::Column ? e->name : e->op == ExprOp::Literal ? e->literal.str() : "op" + std::to_string((int)e->op);
  s += "(";
  for (size_t i = 0; i < e->args.size(); ++i) { if (i) s += ","; s += expr_text(e->args[i].get(), depth + 1); }
  return s + ")";
}

// The program for e over ctx's sources, or null.
inline std::shared_ptr<Prog> compile(const SqlExpr* e, const EvalCtx& ctx, std::string* why = nullptr) {
  if (!enabled() || !e || !ctx.sources) return nullptr;
  if (e->op == ExprOp::Literal || e->op == ExprOp::Column) return nullptr;   // nothing to gain
  auto P = std::make_shared<Prog>();
  literal_sig(e, &P->lit_sig);
  P->pg_division = tls_pg_division;
  Compiler c{ctx, *P};
  c.compile(e);
  if (!c.ok || P->ins.empty() || P->ins.size() > 256 || P->out().taint || P->out().dec) {
    if (why) *why = !c.ok ? c.why : (P->ins.size() > 256 ? "too long" : "result may be an exact decimal");
    return nullptr;
  }
  return P;
}

// One lane per instruction over a chunk of rows.
struct Lanes {
  std::vector<std::vector<int64_t>> i;
  std::vector<std::vector<double>> f;
  std::vector<std::vector<uint8_t>> n;   // 1: NULL
};

// Evaluates P over n rows, rowid(j, src) being row j's row in source src (or kNullRow). False with *err set when
// a value the interpreter would refuse comes up (an integer out of range), and the caller takes the interpreter's
// path.
template <typename RowId>
inline bool eval(const Prog& P, RowId&& rowid, size_t n, Lanes& L, std::string* err) {
  const size_t K = P.ins.size();
  L.i.resize(K); L.f.resize(K); L.n.resize(K);
  for (size_t x = 0; x < K; ++x) {
    const Ins& in = P.ins[x];
    std::vector<int64_t>& vi = L.i[x];
    std::vector<double>& vf = L.f[x];
    std::vector<uint8_t>& nl = L.n[x];
    nl.assign(n, 0);
    if (in.ty == Ty::I64) vi.assign(n, 0); else vf.assign(n, 0);
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
        else if (in.ty == Ty::I64) std::fill(vi.begin(), vi.end(), in.ki);
        else std::fill(vf.begin(), vf.end(), in.kf);
        break;
      case Ins::ColI: case Ins::ColF: {
        const ColumnStore& cs = *in.cs;
        const bool nulls = vx::has_nulls(cs);
        const bool as_int = vx::typed_integer(cs);
        for (size_t j = 0; j < n; ++j) {
          const size_t r = row_of(j);
          if (r == kNullRow || r >= cs.size() || (nulls && cs.is_null(r))) { nl[j] = 1; continue; }
          if (in.op == Ins::ColI) vi[j] = vx::typed_int(cs, r);
          else vf[j] = as_int ? (double)vx::typed_int(cs, r) : vx::typed_value(cs, r);
        }
        break;
      }
      case Ins::CodeI: case Ins::CodeF: {
        const ColumnStore& cs = *in.cs;
        for (size_t j = 0; j < n; ++j) {
          const size_t r = row_of(j);
          if (r == kNullRow || r >= cs.size() || cs.is_null(r)) { nl[j] = 1; continue; }
          const uint32_t k = vx::code_at(cs, r);
          if (k >= in.code_null.size() || in.code_null[k]) { nl[j] = 1; continue; }
          if (in.op == Ins::CodeI) vi[j] = in.code_i[k]; else vf[j] = in.code_f[k];
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
        for (size_t j = 0; j < n; ++j) {
          const size_t r = row_of(j);
          if (r == kNullRow || r >= cs.size() || cs.is_null(r)) { nl[j] = 1; continue; }
          const uint32_t k = vx::code_at(cs, r);
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
        for (size_t j = 0; j < n; ++j) {
          if (N(0)[j] || N(1)[j]) { nl[j] = 1; continue; }
          int c;
          if (both_int) { const int64_t a = A(0)[j], b = A(1)[j]; c = a < b ? -1 : a > b ? 1 : 0; }
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
          if (in.ty == Ty::I64) vi[j] = L.i[in.args[(size_t)hit]][j]; else vf[j] = fd((size_t)hit, j);
        }
        break;
      }
      case Ins::Coalesce:
        for (size_t j = 0; j < n; ++j) {
          size_t k = 0;
          while (k < in.args.size() && N(k)[j]) ++k;
          if (k == in.args.size()) { nl[j] = 1; continue; }
          if (in.ty == Ty::I64) vi[j] = L.i[in.args[k]][j]; else vf[j] = fd(k, j);
        }
        break;
      case Ins::NullIf:
        for (size_t j = 0; j < n; ++j) {
          if (N(0)[j]) { nl[j] = 1; continue; }
          if (!N(1)[j]) {
            const bool same = (ty(0) == Ty::I64 && ty(1) == Ty::I64) ? A(0)[j] == A(1)[j] : fd(0, j) == fd(1, j);
            if (same) { nl[j] = 1; continue; }
          }
          if (in.ty == Ty::I64) vi[j] = A(0)[j]; else vf[j] = F(0)[j];
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
        for (size_t j = 0; j < n; ++j) {
          if (N(0)[j]) { nl[j] = 1; continue; }
          if (ty(0) == Ty::I64) { vi[j] = A(0)[j]; continue; }
          const double a = F(0)[j];
          if (!(std::fabs(a) < 9.2e18)) { *err = "BIGINT value is out of range in 'expression'"; return false; }
          // a DOUBLE as ROUND rounds it: half away from zero on its 15 significant digits, as written; an exact
          // decimal (a literal's lane) on its value
          Val r;
          if (!P.ins[in.args[0]].approx) vi[j] = (int64_t)std::llround(a);
          else vi[j] = round_double_as_written(a, 0, &r) ? (int64_t)std::llround(r.num) : (int64_t)std::nearbyint(a);
        }
        break;
      case Ins::CastF:
        for (size_t j = 0; j < n; ++j) {
          if (N(0)[j]) { nl[j] = 1; continue; }
          vf[j] = fd(0, j);
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

// The result lane's value of row j as the Val the interpreter would have made.
inline Val value_at(const Prog& P, const Lanes& L, size_t j) {
  const Ins& o = P.out();
  const size_t K = P.ins.size() - 1;
  if (L.n[K][j]) return val_null();
  if (o.ty == Ty::F64) return o.approx ? val_approx(L.f[K][j]) : val_num(L.f[K][j]);
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
  size_t size() const { return n.size(); }
  bool is_null(size_t j) const { return n[j] != 0; }
  Val at(size_t j) const {
    if (n[j]) return val_null();
    if (ty == Ty::F64) return approx ? val_approx(f[j]) : val_num(f[j]);
    if (uns) { const uint64_t u = (uint64_t)i[j]; return val_num_with_text((double)u, std::to_string(u)); }
    return val_exact((AIOS_INT128)i[j], 0);
  }
  std::string text(size_t j) const {
    if (n[j]) return sql_null();
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
template <typename RowAt>
inline bool eval_column(const Prog& P, RowAt&& rowat, size_t total, Column* out, std::string* err) {
  const Ins& o = P.out();
  out->ty = o.ty; out->approx = o.approx; out->uns = o.uns;
  out->n.assign(total, 0);
  if (o.ty == Ty::I64) out->i.assign(total, 0); else out->f.assign(total, 0);
  Lanes L;
  const size_t chunk = 4096;
  const size_t K = P.ins.size() - 1;
  for (size_t at = 0; at < total; at += chunk) {
    const size_t n = std::min(chunk, total - at);
    auto rowid = [&](size_t j, uint32_t src) -> size_t {
      const JoinedRow& r = rowat(at + j);
      return src < r.parts.size() ? r.parts[src] : kNullRow;
    };
    if (!eval(P, rowid, n, L, err)) return false;
    std::memcpy(out->n.data() + at, L.n[K].data(), n);
    if (o.ty == Ty::I64) std::memcpy(out->i.data() + at, L.i[K].data(), n * sizeof(int64_t));
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
  if (o.ty == Ty::I64) out->i.assign(total, 0); else out->f.assign(total, 0);
  Lanes L;
  const size_t chunk = 4096;
  const size_t K = P.ins.size() - 1;
  for (size_t at = 0; at < total; at += chunk) {
    const size_t n = std::min(chunk, total - at);
    auto rowid = [&](size_t j, uint32_t src) -> size_t { return src == 0 ? (size_t)ids(at + j) : kNullRow; };
    if (!eval(P, rowid, n, L, err)) return false;
    std::memcpy(out->n.data() + at, L.n[K].data(), n);
    if (o.ty == Ty::I64) std::memcpy(out->i.data() + at, L.i[K].data(), n * sizeof(int64_t));
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
    if ((in.op == Ins::CodeI || in.op == Ins::CodeF || in.op == Ins::StrIn) && vx::dict_size(*in.cs) != in.code_null.size()) return false;
  }
  return true;
}

// The program for e over ctx's sources, compiled once per expression and source list and kept on the expression;
// null when it does not compile. A program over a column store that has since changed is compiled afresh. Only
// the statement's own thread compiles (and writes the cache); a worker thread reads what it finds, so the
// aggregates a parallel emit evaluates are compiled before it starts (vp::warm).
inline const Prog* program_for(const SqlExpr* e, const EvalCtx& ctx) {
  if (!enabled() || !e || !ctx.sources) return nullptr;
  if (e->vprog_scope == (const void*)ctx.sources) {
    const Prog* P = static_cast<const Prog*>(e->vprog.get());
    if (P && P->pg_division == tls_pg_division && valid(*P, ctx)) {
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
  std::shared_ptr<Prog> P = compile(e, ctx, &why);
  if (vx_trace_on() && e->op != ExprOp::Literal && e->op != ExprOp::Column)
    vx_trace((P ? "vp: compiled (" + std::to_string(P->ins.size()) + " ops) " : "vp: interpreted (" + why + ") ") + expr_text(e));
  e->vprog_scope = ctx.sources;
  e->vprog_none = !P;
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
