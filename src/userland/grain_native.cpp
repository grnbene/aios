// grain's native-code subset: `native fn` compiles a typed slice of grain to
// x86-64 machine code, for the inner loops where the bytecode VM is too slow
// and the C++ you would otherwise write is more than you want to ship.
//
//   native fn checksum(b: bytes) -> i64 {
//     let h = 0
//     let i = 0
//     while i < b.len {
//       h = (h * 31 + b[i]) & 0xFFFFFFFF
//       i += 1
//     }
//     return h
//   }
//   print(checksum(bytes("hello")))
//
// Types are written out and nothing converts silently: i64, f64, bool, bytes
// and (unsafe) ptr. Arithmetic wraps; division by zero, an index outside the
// bytes, and runaway recursion stop the program with an error rather than the
// process. `bytes` is the only memory a native function may touch unless the
// host allows `unsafe`, and every access to it is bounds-checked.
//
// This file is the typed parser and checker, a small x86-64 assembler, the code
// generator, and the glue that calls generated code from the VM. Generated code
// follows the System V calling convention, so one native function calls
// another, and `extern fn` (unsafe) calls C.
#include "grain_native.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#if defined(__x86_64__) && (defined(__unix__) || defined(__APPLE__)) && !defined(_WIN32)
#define GRAIN_NATIVE_X64 1
#endif

namespace aios::userland::grain {

namespace {
NativePolicy g_policy;
}
void set_native_policy(NativePolicy p) { g_policy = p; }
NativePolicy native_policy() { return g_policy; }

namespace {

// ---- types and the typed tree ---------------------------------------------------

enum class Ty : uint8_t { Void, I64, F64, Bool, Ptr, Bytes };

struct Var {
  std::string name;
  Ty ty = Ty::I64;
  int slot = 0;          // first 8-byte word; a bytes variable has the pointer here and the length in slot + 1
  bool narrow32 = false; // an extern parameter declared i32
};

enum class EK : uint8_t { Int, Real, Bool, Var, Neg, Not, BitNot, Bin, Cmp, AndAnd, OrOr, Call, Index, Len, Cast, Rd, Wr, Sqrt, Fabs };

struct Fn;
struct E;
using EP = std::unique_ptr<E>;
struct E {
  EK k = EK::Int;
  Ty ty = Ty::I64;
  int line = 0;
  int64_t iv = 0;
  double dv = 0;
  Tok op = Tok::End;
  EP a, b, c;
  std::vector<EP> args;
  Var* var = nullptr;
  Fn* fn = nullptr;
  int size = 8;        // Rd / Wr: bytes moved
  bool sgn = false;    // Rd: sign-extend
  bool real = false;   // Rd / Wr: a double
};

enum class SK : uint8_t { Let, Assign, IndexSet, Expr, If, While, Return, Break, Continue };

struct S;
using SP = std::unique_ptr<S>;
struct S {
  SK k = SK::Expr;
  int line = 0;
  Var* var = nullptr;
  EP e, idx;
  std::vector<SP> body, other;
};

struct Fn {
  std::string name;
  std::vector<Var*> params;
  Ty ret = Ty::Void;
  bool ret_narrow32 = false;
  bool is_extern = false;
  std::deque<Var> vars;
  int nslots = 0;
  std::vector<SP> body;
  void* code = nullptr;       // where the generated code lives
  void* extern_addr = nullptr;
};

}  // namespace

struct NativeModule {
  std::map<std::string, std::shared_ptr<Fn>> fns;
};

std::shared_ptr<NativeModule> new_native_module() { return std::make_shared<NativeModule>(); }

namespace {

const char* ty_name(Ty t) {
  switch (t) {
    case Ty::Void: return "nothing";
    case Ty::I64: return "i64";
    case Ty::F64: return "f64";
    case Ty::Bool: return "bool";
    case Ty::Ptr: return "ptr";
    case Ty::Bytes: return "bytes";
  }
  return "?";
}

// ---- the parser and checker: one pass, types worked out as nodes are built ---

class NParser {
 public:
  NParser(const std::vector<Token>& toks, NativeModule& mod, std::vector<Diagnostic>& errors)
      : t_(toks), mod_(mod), errors_(errors) {}

  std::shared_ptr<Fn> declaration();
  bool ok() const { return !failed_; }

 private:
  const std::vector<Token>& t_;
  NativeModule& mod_;
  std::vector<Diagnostic>& errors_;
  size_t p_ = 0;
  std::shared_ptr<Fn> fn_;
  std::vector<std::unordered_map<std::string, Var*>> scopes_;
  int loops_ = 0;
  bool failed_ = false;

  const Token& peek(size_t n = 0) const { static const Token end; return p_ + n < t_.size() ? t_[p_ + n] : end; }
  const Token& advance() { const Token& t = peek(); if (p_ < t_.size()) ++p_; return t; }
  bool check(Tok k) const { return peek().kind == k; }
  bool match(Tok k) { if (check(k)) { advance(); return true; } return false; }
  bool ident(const char* word) const { return check(Tok::Ident) && peek().text == word; }
  void skip_nl() { while (check(Tok::Newline)) advance(); }

  void error(int line, const std::string& msg) {
    failed_ = true;
    if (errors_.size() < 12) errors_.push_back(Diagnostic{line, msg});
  }
  bool expect(Tok k, const char* what) {
    if (match(k)) return true;
    error(peek().line, std::string("expected ") + what);
    return false;
  }

  bool parse_type(Ty& out, bool& narrow, bool is_extern);
  Var* declare(const std::string& name, Ty ty, int line);
  Var* lookup(const std::string& name);
  void need_unsafe(int line, const char* what) {
    if (!g_policy.unsafe) error(line, std::string(what) + " is unsafe, and this program is not allowed it");
  }

  std::vector<SP> block();
  SP statement();
  SP let_statement();
  SP if_statement();
  SP while_statement();
  SP for_statement();
  SP simple_statement();
  EP expression(int min_power = 0);
  EP unary();
  EP postfix(EP e);
  EP primary();
  EP call(const Token& name);
  EP intrinsic(const Token& name, std::vector<EP>& args);

  EP mk(EK k, Ty ty, int line) { auto e = std::make_unique<E>(); e->k = k; e->ty = ty; e->line = line; return e; }
  EP bad(int line) { return mk(EK::Int, Ty::I64, line); }   // stands in for an expression that failed to parse
  bool same(const EP& a, const EP& b, int line, const char* what) {
    if (a->ty == b->ty) return true;
    error(line, std::string(what) + ": " + ty_name(a->ty) + " and " + ty_name(b->ty) + " do not mix (convert with `as`)");
    return false;
  }
  SP stmt(SK k, int line) { auto s = std::make_unique<S>(); s->k = k; s->line = line; return s; }
};

int power_of(Tok t) {
  switch (t) {
    case Tok::Or: return 1;
    case Tok::And: return 2;
    case Tok::Pipe: return 3;
    case Tok::Caret: return 4;
    case Tok::Amp: return 5;
    case Tok::EqEq: case Tok::BangEq: return 6;
    case Tok::Lt: case Tok::LtEq: case Tok::Gt: case Tok::GtEq: return 7;
    case Tok::LtLt: case Tok::GtGt: return 8;
    case Tok::Plus: case Tok::Minus: return 9;
    case Tok::Star: case Tok::Slash: case Tok::Percent: return 10;
    default: return 0;
  }
}

bool NParser::parse_type(Ty& out, bool& narrow, bool is_extern) {
  narrow = false;
  if (!check(Tok::Ident)) { error(peek().line, "expected a type (i64, f64, bool, bytes or ptr)"); return false; }
  const Token tk = advance();
  const std::string& n = tk.text;
  if (n == "i64") out = Ty::I64;
  else if (n == "f64") out = Ty::F64;
  else if (n == "bool") out = Ty::Bool;
  else if (n == "bytes") out = Ty::Bytes;
  else if (n == "ptr") { need_unsafe(tk.line, "ptr"); out = Ty::Ptr; }
  else if (n == "i32" && is_extern) { out = Ty::I64; narrow = true; }
  else { error(tk.line, "\"" + n + "\" is not a type (i64, f64, bool, bytes or ptr)"); return false; }
  return true;
}

Var* NParser::declare(const std::string& name, Ty ty, int line) {
  auto it = scopes_.back().find(name);
  if (it != scopes_.back().end()) { error(line, "\"" + name + "\" is already declared in this block"); return it->second; }
  fn_->vars.emplace_back();
  Var* v = &fn_->vars.back();
  v->name = name;
  v->ty = ty;
  v->slot = fn_->nslots;
  fn_->nslots += ty == Ty::Bytes ? 2 : 1;
  scopes_.back()[name] = v;
  return v;
}

Var* NParser::lookup(const std::string& name) {
  for (size_t i = scopes_.size(); i-- > 0;) {
    auto it = scopes_[i].find(name);
    if (it != scopes_[i].end()) return it->second;
  }
  return nullptr;
}

// native fn name(a: i64, b: bytes) -> i64 { ... }      extern fn name(a: i64) -> i32
std::shared_ptr<Fn> NParser::declaration() {
  fn_ = std::make_shared<Fn>();
  const int first_line = peek().line;
  const bool is_extern = ident("extern");
  advance();   // native / extern
  if (!match(Tok::Fn)) { error(peek().line, "expected fn"); return nullptr; }
  if (!check(Tok::Ident)) { error(peek().line, "expected the function's name"); return nullptr; }
  fn_->name = advance().text;
  fn_->is_extern = is_extern;
  if (is_extern) need_unsafe(first_line, "extern fn");
  scopes_.emplace_back();
  if (!expect(Tok::LParen, "(")) return nullptr;
  skip_nl();
  while (!check(Tok::RParen) && !check(Tok::End)) {
    if (!check(Tok::Ident)) { error(peek().line, "expected a parameter name"); return nullptr; }
    const Token name = advance();
    if (!expect(Tok::Colon, ": and the parameter's type")) return nullptr;
    Ty ty; bool narrow;
    if (!parse_type(ty, narrow, is_extern)) return nullptr;
    if (is_extern && ty == Ty::Bytes) { error(name.line, "an extern parameter is i64, i32, f64 or ptr (pass a buffer as `b as ptr`)"); return nullptr; }
    Var* v = declare(name.text, ty, name.line);
    v->narrow32 = narrow;
    fn_->params.push_back(v);
    skip_nl();
    if (!match(Tok::Comma)) break;
    skip_nl();
  }
  if (!expect(Tok::RParen, ")")) return nullptr;
  if (check(Tok::Minus) && peek(1).kind == Tok::Gt) {
    advance(); advance();
    bool narrow;
    if (!parse_type(fn_->ret, narrow, is_extern)) return nullptr;
    if (fn_->ret == Ty::Bytes) { error(peek().line, "a native function cannot return bytes"); return nullptr; }
    fn_->ret_narrow32 = narrow;
  }
  if (mod_.fns.count(fn_->name)) { error(first_line, "\"" + fn_->name + "\" is already a native function here"); return nullptr; }
  mod_.fns[fn_->name] = fn_;   // before the body, so it can call itself
  if (is_extern) {
#if defined(__unix__) || defined(__APPLE__)
    fn_->extern_addr = ::dlsym(RTLD_DEFAULT, fn_->name.c_str());
#endif
    if (!fn_->extern_addr) { error(first_line, "extern fn " + fn_->name + ": no such symbol in this program"); mod_.fns.erase(fn_->name); return nullptr; }
    return failed_ ? nullptr : fn_;
  }
  skip_nl();
  fn_->body = block();
  skip_nl();
  if (!check(Tok::End)) error(peek().line, "unexpected text after the function's closing brace");
  if (failed_) { mod_.fns.erase(fn_->name); return nullptr; }
  return fn_;
}

std::vector<SP> NParser::block() {
  std::vector<SP> out;
  if (!expect(Tok::LBrace, "{")) return out;
  scopes_.emplace_back();
  skip_nl();
  while (!check(Tok::RBrace) && !check(Tok::End)) {
    SP s = statement();
    if (s) out.push_back(std::move(s));
    if (failed_ && errors_.size() >= 12) break;
    if (failed_ && !s) { while (!check(Tok::Newline) && !check(Tok::RBrace) && !check(Tok::End)) advance(); }   // resynchronise after a bad statement
    skip_nl();
  }
  expect(Tok::RBrace, "}");
  scopes_.pop_back();
  return out;
}

SP NParser::statement() {
  skip_nl();
  const Token t = peek();
  if (t.kind == Tok::Let) return let_statement();
  if (t.kind == Tok::If) return if_statement();
  if (t.kind == Tok::While) return while_statement();
  if (t.kind == Tok::For) return for_statement();
  if (t.kind == Tok::Return) {
    advance();
    auto s = stmt(SK::Return, t.line);
    if (!check(Tok::Newline) && !check(Tok::RBrace)) {
      s->e = expression();
      if (fn_->ret == Ty::Void) error(t.line, fn_->name + "() returns nothing, so `return` takes no value");
      else if (s->e->ty != fn_->ret) error(t.line, fn_->name + "() returns " + ty_name(fn_->ret) + " but this is " + ty_name(s->e->ty));
    } else if (fn_->ret != Ty::Void) {
      error(t.line, fn_->name + "() returns " + ty_name(fn_->ret) + ", so `return` needs a value");
    }
    return s;
  }
  if (t.kind == Tok::Break || t.kind == Tok::Continue) {
    advance();
    if (loops_ == 0) error(t.line, std::string(t.kind == Tok::Break ? "break" : "continue") + " outside a loop");
    return stmt(t.kind == Tok::Break ? SK::Break : SK::Continue, t.line);
  }
  return simple_statement();
}

// let x = e   |   let x: T = e
SP NParser::let_statement() {
  const Token let = advance();
  if (!check(Tok::Ident)) { error(peek().line, "expected a name after let"); return nullptr; }
  const Token name = advance();
  Ty want = Ty::Void;
  bool has_type = false;
  if (match(Tok::Colon)) {
    bool narrow;
    if (!parse_type(want, narrow, false)) return nullptr;
    has_type = true;
  }
  if (!expect(Tok::Assign, "= and a value (a native variable always starts with one)")) return nullptr;
  skip_nl();
  EP init = expression();
  if (init->ty == Ty::Void) error(let.line, "that has no value to keep");
  if (has_type && init->ty != want) error(let.line, std::string("\"") + name.text + "\" is " + ty_name(want) + " but its value is " + ty_name(init->ty));
  auto s = stmt(SK::Let, let.line);
  s->e = std::move(init);
  s->var = declare(name.text, s->e->ty == Ty::Void ? Ty::I64 : s->e->ty, name.line);   // after the value: `let x = x` reads the outer x
  return s;
}

SP NParser::if_statement() {
  const Token t = advance();
  auto s = stmt(SK::If, t.line);
  s->e = expression();
  if (s->e->ty != Ty::Bool) error(t.line, std::string("an if wants a bool, got ") + ty_name(s->e->ty) + " (compare it, e.g. `x != 0`)");
  skip_nl();
  s->body = block();
  // `else` may follow the closing brace on the next line
  size_t save = p_;
  skip_nl();
  if (match(Tok::Else)) {
    skip_nl();
    if (check(Tok::If)) { SP inner = if_statement(); if (inner) s->other.push_back(std::move(inner)); }
    else s->other = block();
  } else {
    p_ = save;
  }
  return s;
}

SP NParser::while_statement() {
  const Token t = advance();
  auto s = stmt(SK::While, t.line);
  s->e = expression();
  if (s->e->ty != Ty::Bool) error(t.line, std::string("a while wants a bool, got ") + ty_name(s->e->ty));
  skip_nl();
  ++loops_;
  s->body = block();
  --loops_;
  return s;
}

// for i in a..b { }   - i runs from a up to but not including b
SP NParser::for_statement() {
  const Token t = advance();
  if (!check(Tok::Ident)) { error(peek().line, "expected a loop variable after for"); return nullptr; }
  const Token name = advance();
  if (!match(Tok::In)) { error(peek().line, "expected `in`"); return nullptr; }
  EP from = expression(1);
  if (!(check(Tok::Dot) && peek(1).kind == Tok::Dot)) { error(peek().line, "expected `..` between the loop's bounds"); return nullptr; }
  advance(); advance();
  EP to = expression(1);
  if (from->ty != Ty::I64 || to->ty != Ty::I64) error(t.line, "the bounds of a for loop are i64");
  // for i in a..b { body }  ==>  { let i = a; let end = b; while i < end { body; i += 1 } }
  scopes_.emplace_back();
  auto outer = stmt(SK::If, t.line);   // an always-true block that scopes the loop variable
  outer->e = mk(EK::Bool, Ty::Bool, t.line);
  outer->e->iv = 1;
  auto init = stmt(SK::Let, t.line);
  init->e = std::move(from);
  init->var = declare(name.text, Ty::I64, name.line);
  auto limit = stmt(SK::Let, t.line);
  limit->e = std::move(to);
  limit->var = declare("\x01" "end" + std::to_string(fn_->nslots), Ty::I64, t.line);
  auto loop = stmt(SK::While, t.line);
  loop->e = mk(EK::Cmp, Ty::Bool, t.line);
  loop->e->op = Tok::Lt;
  loop->e->a = mk(EK::Var, Ty::I64, t.line); loop->e->a->var = init->var;
  loop->e->b = mk(EK::Var, Ty::I64, t.line); loop->e->b->var = limit->var;
  skip_nl();
  ++loops_;
  loop->body = block();
  --loops_;
  auto step = stmt(SK::Assign, t.line);   // i = i + 1  (a `continue` must still step: see Gen, which steps at the loop's continue label)
  step->var = init->var;
  step->e = mk(EK::Bin, Ty::I64, t.line);
  step->e->op = Tok::Plus;
  step->e->a = mk(EK::Var, Ty::I64, t.line); step->e->a->var = init->var;
  step->e->b = mk(EK::Int, Ty::I64, t.line); step->e->b->iv = 1;
  loop->other.push_back(std::move(step));   // `other` of a While is its per-iteration step
  outer->body.push_back(std::move(init));
  outer->body.push_back(std::move(limit));
  outer->body.push_back(std::move(loop));
  scopes_.pop_back();
  return outer;
}

// an assignment, an indexed store, or an expression used for its effect
SP NParser::simple_statement() {
  const Token first = peek();
  EP e = expression();
  const Tok op = peek().kind;
  const bool compound = op == Tok::PlusEq || op == Tok::MinusEq || op == Tok::StarEq || op == Tok::SlashEq || op == Tok::PercentEq ||
                        op == Tok::AmpEq || op == Tok::PipeEq || op == Tok::CaretEq || op == Tok::LtLtEq || op == Tok::GtGtEq;
  if (op != Tok::Assign && !compound) {
    auto s = stmt(SK::Expr, first.line);
    s->e = std::move(e);
    return s;
  }
  advance();
  skip_nl();
  EP rhs = expression();
  auto compound_value = [&](EP target_read) -> EP {   // x op= v  ==>  x = x op v
    static const std::map<Tok, Tok> base = {{Tok::PlusEq, Tok::Plus}, {Tok::MinusEq, Tok::Minus}, {Tok::StarEq, Tok::Star},
        {Tok::SlashEq, Tok::Slash}, {Tok::PercentEq, Tok::Percent}, {Tok::AmpEq, Tok::Amp}, {Tok::PipeEq, Tok::Pipe},
        {Tok::CaretEq, Tok::Caret}, {Tok::LtLtEq, Tok::LtLt}, {Tok::GtGtEq, Tok::GtGt}};
    auto b = mk(EK::Bin, target_read->ty, first.line);
    b->op = base.at(op);
    if (target_read->ty != rhs->ty) error(first.line, std::string("an update of ") + ty_name(target_read->ty) + " by " + ty_name(rhs->ty));
    if (target_read->ty == Ty::F64 && (b->op == Tok::Amp || b->op == Tok::Pipe || b->op == Tok::Caret || b->op == Tok::LtLt || b->op == Tok::GtGt || b->op == Tok::Percent))
      error(first.line, "that operator is not defined for f64");
    if (target_read->ty != Ty::F64 && target_read->ty != Ty::I64 && target_read->ty != Ty::Ptr) error(first.line, "that update does not apply to " + std::string(ty_name(target_read->ty)));
    b->a = std::move(target_read);
    b->b = std::move(rhs);
    return b;
  };
  if (e->k == EK::Var) {
    auto s = stmt(SK::Assign, first.line);
    s->var = e->var;
    if (e->var->ty == Ty::Bytes && compound) { error(first.line, "bytes have no arithmetic"); return s; }
    if (compound) { auto read = mk(EK::Var, e->ty, first.line); read->var = e->var; s->e = compound_value(std::move(read)); }
    else {
      if (rhs->ty != e->ty) error(first.line, std::string("\"") + e->var->name + "\" is " + ty_name(e->ty) + " but this is " + ty_name(rhs->ty));
      s->e = std::move(rhs);
    }
    return s;
  }
  if (e->k == EK::Index) {
    auto s = stmt(SK::IndexSet, first.line);
    s->var = e->a->var;
    s->idx = std::move(e->b);
    if (compound) { error(first.line, "update a byte with `b[i] = b[i] + v`"); return s; }
    if (rhs->ty != Ty::I64) error(first.line, "a byte is stored from an i64");
    s->e = std::move(rhs);
    return s;
  }
  error(first.line, "the left side of = must be a variable or an index into bytes");
  return nullptr;
}

EP NParser::expression(int min_power) {
  EP left = unary();
  for (;;) {
    const Tok op = peek().kind;
    const int pw = power_of(op);
    if (pw == 0 || pw <= min_power) break;
    const Token optok = advance();
    skip_nl();
    EP right = expression(pw);
    const int line = optok.line;
    if (op == Tok::And || op == Tok::Or) {
      if (left->ty != Ty::Bool || right->ty != Ty::Bool) error(line, std::string("`") + (op == Tok::And ? "and" : "or") + "` joins bools, got " + ty_name(left->ty) + " and " + ty_name(right->ty));
      auto e = mk(op == Tok::And ? EK::AndAnd : EK::OrOr, Ty::Bool, line);
      e->a = std::move(left); e->b = std::move(right);
      left = std::move(e);
      continue;
    }
    const bool cmp = op == Tok::EqEq || op == Tok::BangEq || op == Tok::Lt || op == Tok::LtEq || op == Tok::Gt || op == Tok::GtEq;
    same(left, right, line, "the two sides of an operator");
    if (cmp) {
      if (left->ty == Ty::Bytes) error(line, "bytes cannot be compared here (compare their bytes)");
      if (left->ty == Ty::Bool && op != Tok::EqEq && op != Tok::BangEq) error(line, "bools are only compared for equality");
      auto e = mk(EK::Cmp, Ty::Bool, line);
      e->op = op; e->a = std::move(left); e->b = std::move(right);
      left = std::move(e);
      continue;
    }
    const Ty ty = left->ty;
    const bool bitop = op == Tok::Amp || op == Tok::Pipe || op == Tok::Caret || op == Tok::LtLt || op == Tok::GtGt || op == Tok::Percent;
    if (ty != Ty::I64 && ty != Ty::F64 && !(ty == Ty::Ptr && (op == Tok::Plus || op == Tok::Minus))) error(line, std::string("arithmetic is not defined for ") + ty_name(ty));
    if (ty == Ty::F64 && bitop) error(line, "that operator is not defined for f64");
    auto e = mk(EK::Bin, ty, line);
    e->op = op; e->a = std::move(left); e->b = std::move(right);
    left = std::move(e);
  }
  return left;
}

EP NParser::unary() {
  const Token t = peek();
  if (t.kind == Tok::Minus) {
    advance();
    EP a = unary();
    if (a->ty != Ty::I64 && a->ty != Ty::F64) error(t.line, std::string("cannot negate ") + ty_name(a->ty));
    if (a->k == EK::Int) { a->iv = -a->iv; return a; }
    if (a->k == EK::Real) { a->dv = -a->dv; return a; }
    auto e = mk(EK::Neg, a->ty, t.line);
    e->a = std::move(a);
    return e;
  }
  if (t.kind == Tok::Not) {
    advance();
    EP a = unary();
    if (a->ty != Ty::Bool) error(t.line, std::string("`not` wants a bool, got ") + ty_name(a->ty));
    auto e = mk(EK::Not, Ty::Bool, t.line);
    e->a = std::move(a);
    return e;
  }
  if (t.kind == Tok::Tilde) {
    advance();
    EP a = unary();
    if (a->ty != Ty::I64) error(t.line, std::string("~ wants an i64, got ") + ty_name(a->ty));
    auto e = mk(EK::BitNot, Ty::I64, t.line);
    e->a = std::move(a);
    return e;
  }
  return postfix(primary());
}

// b[i], b.len, x as f64
EP NParser::postfix(EP e) {
  for (;;) {
    if (check(Tok::LBracket)) {
      const Token t = advance();
      EP idx = expression();
      expect(Tok::RBracket, "]");
      if (e->ty != Ty::Bytes || e->k != EK::Var) { error(t.line, "only a bytes variable can be indexed"); return e; }
      if (idx->ty != Ty::I64) error(t.line, "an index is an i64");
      auto n = mk(EK::Index, Ty::I64, t.line);
      n->a = std::move(e); n->b = std::move(idx);
      e = std::move(n);
      continue;
    }
    if (check(Tok::Dot) && peek(1).kind == Tok::Ident && peek(1).text == "len") {
      const Token t = advance(); advance();
      if (e->ty != Ty::Bytes || e->k != EK::Var) { error(t.line, ".len is for a bytes variable"); return e; }
      auto n = mk(EK::Len, Ty::I64, t.line);
      n->a = std::move(e);
      e = std::move(n);
      continue;
    }
    if (check(Tok::Ident) && peek().text == "as") {
      const Token t = advance();
      Ty to; bool narrow;
      if (!parse_type(to, narrow, false)) return e;
      const Ty from = e->ty;
      const bool ok = (from == to) || (from == Ty::I64 && to == Ty::F64) || (from == Ty::F64 && to == Ty::I64) ||
                      (from == Ty::Bool && to == Ty::I64) || (from == Ty::I64 && to == Ty::Ptr) || (from == Ty::Ptr && to == Ty::I64) ||
                      (from == Ty::Bytes && to == Ty::Ptr);
      if (!ok) { error(t.line, std::string("cannot convert ") + ty_name(from) + " to " + ty_name(to)); return e; }
      if (from == to) continue;
      if (to == Ty::Ptr) need_unsafe(t.line, from == Ty::Bytes ? "bytes as ptr" : "i64 as ptr");
      if (from == Ty::Ptr) need_unsafe(t.line, "ptr as i64");
      auto n = mk(EK::Cast, to, t.line);
      n->a = std::move(e);
      e = std::move(n);
      continue;
    }
    return e;
  }
}

EP NParser::primary() {
  const Token t = advance();
  switch (t.kind) {
    case Tok::Int: { auto e = mk(EK::Int, Ty::I64, t.line); e->iv = t.integer; return e; }
    case Tok::Real: { auto e = mk(EK::Real, Ty::F64, t.line); e->dv = t.real; return e; }
    case Tok::True: case Tok::False: { auto e = mk(EK::Bool, Ty::Bool, t.line); e->iv = t.kind == Tok::True; return e; }
    case Tok::LParen: {
      skip_nl();
      EP e = expression();
      skip_nl();
      expect(Tok::RParen, ")");
      return e;
    }
    case Tok::Ident: {
      if (check(Tok::LParen)) return call(t);
      Var* v = lookup(t.text);
      if (!v) { error(t.line, "\"" + t.text + "\" is not declared"); return bad(t.line); }
      auto e = mk(EK::Var, v->ty, t.line);
      e->var = v;
      return e;
    }
    default:
      error(t.line, "expected a value here");
      return bad(t.line);
  }
}

EP NParser::call(const Token& name) {
  advance();   // (
  std::vector<EP> args;
  skip_nl();
  while (!check(Tok::RParen) && !check(Tok::End)) {
    args.push_back(expression());
    skip_nl();
    if (!match(Tok::Comma)) break;
    skip_nl();
  }
  expect(Tok::RParen, ")");
  if (EP r = intrinsic(name, args)) return r;
  auto it = mod_.fns.find(name.text);
  if (it == mod_.fns.end()) { error(name.line, "\"" + name.text + "\" is not a native function (declare it above this one)"); return bad(name.line); }
  Fn* callee = it->second.get();
  if (args.size() != callee->params.size()) {
    error(name.line, name.text + "() takes " + std::to_string(callee->params.size()) + " argument(s), got " + std::to_string(args.size()));
    return bad(name.line);
  }
  for (size_t i = 0; i < args.size(); ++i)
    if (args[i]->ty != callee->params[i]->ty)
      error(name.line, name.text + "(): argument " + std::to_string(i + 1) + " is " + ty_name(args[i]->ty) + ", wanted " + ty_name(callee->params[i]->ty));
  for (size_t i = 0; i < args.size(); ++i)
    if (args[i]->ty == Ty::Bytes && args[i]->k != EK::Var) error(name.line, "pass bytes by name");
  auto e = mk(EK::Call, callee->ret, name.line);
  e->fn = callee;
  e->args = std::move(args);
  return e;
}

// rd8(b, off) rdi8 rd16 rdi16 rd32 rdi32 rd64 rdf64 / wr8(b, off, v) wr16 wr32 wr64 wrf64 / sqrt fabs
EP NParser::intrinsic(const Token& name, std::vector<EP>& args) {
  const std::string& n = name.text;
  const int line = name.line;
  if (n == "sqrt" || n == "fabs") {
    if (args.size() != 1 || args[0]->ty != Ty::F64) { error(line, n + "() takes one f64"); return bad(line); }
    auto e = mk(n == "sqrt" ? EK::Sqrt : EK::Fabs, Ty::F64, line);
    e->a = std::move(args[0]);
    return e;
  }
  const bool rd = n.rfind("rd", 0) == 0, wr = n.rfind("wr", 0) == 0;
  if (!rd && !wr) return nullptr;
  int size = 0; bool sgn = false, real = false;
  const std::string rest = n.substr(2);
  if (rest == "8") size = 1;
  else if (rest == "16") size = 2;
  else if (rest == "32") size = 4;
  else if (rest == "64") size = 8;
  else if (rd && rest == "i8") { size = 1; sgn = true; }
  else if (rd && rest == "i16") { size = 2; sgn = true; }
  else if (rd && rest == "i32") { size = 4; sgn = true; }
  else if (rest == "f64") { size = 8; real = true; }
  else return nullptr;
  const size_t want_args = rd ? 2 : 3;
  if (args.size() != want_args) { error(line, n + "() takes " + std::to_string(want_args) + " arguments"); return bad(line); }
  if (args[0]->ty != Ty::Bytes && args[0]->ty != Ty::Ptr) { error(line, n + "(): the first argument is bytes or a ptr"); return bad(line); }
  if (args[0]->ty == Ty::Bytes && args[0]->k != EK::Var) { error(line, "pass bytes by name"); return bad(line); }
  if (args[0]->ty == Ty::Ptr) need_unsafe(line, "reading or writing through a ptr");
  if (args[1]->ty != Ty::I64) { error(line, n + "(): the offset is an i64"); return bad(line); }
  if (wr) {
    const Ty want = real ? Ty::F64 : Ty::I64;
    if (args[2]->ty != want) { error(line, n + "(): the value is " + ty_name(want)); return bad(line); }
  }
  auto e = mk(rd ? EK::Rd : EK::Wr, rd ? (real ? Ty::F64 : Ty::I64) : Ty::Void, line);
  e->size = size; e->sgn = sgn; e->real = real;
  e->a = std::move(args[0]); e->b = std::move(args[1]);
  if (wr) e->c = std::move(args[2]);
  return e;
}

// ---- an x86-64 assembler: only what the generator uses ------------------------------

#if defined(GRAIN_NATIVE_X64)

enum Reg { RAX = 0, RCX = 1, RDX = 2, RBX = 3, RSP = 4, RBP = 5, RSI = 6, RDI = 7, R8 = 8, R9 = 9 };
enum Cc { CC_B = 2, CC_AE = 3, CC_E = 4, CC_NE = 5, CC_BE = 6, CC_A = 7, CC_P = 0xA, CC_NP = 0xB, CC_L = 0xC, CC_GE = 0xD, CC_LE = 0xE, CC_G = 0xF };

struct Label {
  int pos = -1;
  std::vector<size_t> fixups;   // offsets of rel32 fields waiting for this label
};

class Asm {
 public:
  std::vector<uint8_t> b;

  size_t size() const { return b.size(); }
  void u8(uint32_t v) { b.push_back((uint8_t)v); }
  void u32(uint32_t v) { for (int i = 0; i < 4; ++i) u8(v >> (8 * i)); }
  void u64(uint64_t v) { for (int i = 0; i < 8; ++i) u8((uint32_t)(v >> (8 * i))); }
  void rex(bool w, int reg, int rm) {
    const int r = 0x40 | (w ? 8 : 0) | ((reg >> 3) & 1) << 2 | ((rm >> 3) & 1);
    if (r != 0x40) u8(r);
  }
  void modrm_reg(int reg, int rm) { u8(0xC0 | ((reg & 7) << 3) | (rm & 7)); }
  void modrm_mem(int reg, int base, int32_t disp) {   // [base + disp32]
    u8(0x80 | ((reg & 7) << 3) | (base & 7));
    if ((base & 7) == 4) u8(0x24);
    u32((uint32_t)disp);
  }

  void mov_ri(int r, uint64_t v) { rex(true, 0, r); u8(0xB8 + (r & 7)); u64(v); }
  void mov_fs(int r, int32_t disp) { u8(0x64); rex(true, r, 0); u8(0x8B); u8(0x04 | ((r & 7) << 3)); u8(0x25); u32((uint32_t)disp); }   // r = fs:[disp]
  void mov_rr(int dst, int src) { rex(true, src, dst); u8(0x89); modrm_reg(src, dst); }
  void mov_rm(int dst, int base, int32_t d) { rex(true, dst, base); u8(0x8B); modrm_mem(dst, base, d); }
  void mov_mr(int base, int32_t d, int src) { rex(true, src, base); u8(0x89); modrm_mem(src, base, d); }
  void alu(int opcode, int dst, int src) { rex(true, src, dst); u8(opcode); modrm_reg(src, dst); }   // dst = dst op src
  void add(int d, int s) { alu(0x01, d, s); }
  void sub(int d, int s) { alu(0x29, d, s); }
  void and_(int d, int s) { alu(0x21, d, s); }
  void or_(int d, int s) { alu(0x09, d, s); }
  void xor_(int d, int s) { alu(0x31, d, s); }
  void cmp(int d, int s) { alu(0x39, d, s); }
  void test(int d, int s) { alu(0x85, d, s); }
  void cmp_ri8(int r, int8_t v) { rex(true, 0, r); u8(0x83); modrm_reg(7, r); u8((uint8_t)v); }
  void imul(int dst, int src) { rex(true, dst, src); u8(0x0F); u8(0xAF); modrm_reg(dst, src); }
  void unary(int ext, int r) { rex(true, 0, r); u8(0xF7); modrm_reg(ext, r); }
  void neg(int r) { unary(3, r); }
  void not_(int r) { unary(2, r); }
  void idiv(int r) { unary(7, r); }
  void cqo() { u8(0x48); u8(0x99); }
  void shl_cl(int r) { rex(true, 0, r); u8(0xD3); modrm_reg(4, r); }
  void sar_cl(int r) { rex(true, 0, r); u8(0xD3); modrm_reg(7, r); }
  void setcc(int cc, int r) { u8(0x0F); u8(0x90 + cc); modrm_reg(0, r); }   // r must be al, cl or dl
  void movzx_eax_al() { u8(0x0F); u8(0xB6); u8(0xC0); }
  void push(int r) { if (r >= 8) u8(0x41); u8(0x50 + (r & 7)); }
  void pop(int r) { if (r >= 8) u8(0x41); u8(0x58 + (r & 7)); }
  void ret() { u8(0xC3); }
  void call_r(int r) { if (r >= 8) u8(0x41); u8(0xFF); modrm_reg(2, r); }
  void add_rsp(int32_t n) { u8(0x48); u8(0x81); u8(0xC4); u32((uint32_t)n); }
  void sub_rsp(int32_t n) { u8(0x48); u8(0x81); u8(0xEC); u32((uint32_t)n); }
  void and_rsp_16() { u8(0x48); u8(0x83); u8(0xE4); u8(0xF0); }

  // loads through [rax] into rax / xmm0, and stores from rdx / xmm0 through [rax]
  void load_u8() { u8(0x48); u8(0x0F); u8(0xB6); u8(0x00); }
  void load_u16() { u8(0x48); u8(0x0F); u8(0xB7); u8(0x00); }
  void load_u32() { u8(0x8B); u8(0x00); }
  void load_i8() { u8(0x48); u8(0x0F); u8(0xBE); u8(0x00); }
  void load_i16() { u8(0x48); u8(0x0F); u8(0xBF); u8(0x00); }
  void load_i32() { u8(0x48); u8(0x63); u8(0x00); }
  void load_u64() { u8(0x48); u8(0x8B); u8(0x00); }
  void load_f64() { u8(0xF2); u8(0x0F); u8(0x10); u8(0x00); }
  void store_u8() { u8(0x88); u8(0x10); }                 // mov [rax], dl
  void store_u16() { u8(0x66); u8(0x89); u8(0x10); }
  void store_u32() { u8(0x89); u8(0x10); }
  void store_u64() { u8(0x48); u8(0x89); u8(0x10); }
  void store_f64() { u8(0xF2); u8(0x0F); u8(0x11); u8(0x00); }   // movsd [rax], xmm0
  void movsxd_rax_eax() { u8(0x48); u8(0x63); u8(0xC0); }

  // SSE2 on the low double
  void sse(int prefix, int op, int dst, int src) { u8(prefix); u8(0x0F); u8(op); modrm_reg(dst, src); }
  void movsd_xm(int x, int base, int32_t d) { u8(0xF2); u8(0x0F); u8(0x10); modrm_mem(x, base, d); }
  void movsd_mx(int base, int32_t d, int x) { u8(0xF2); u8(0x0F); u8(0x11); modrm_mem(x, base, d); }
  void movsd_xx(int dst, int src) { sse(0xF2, 0x10, dst, src); }
  void addsd(int d, int s) { sse(0xF2, 0x58, d, s); }
  void subsd(int d, int s) { sse(0xF2, 0x5C, d, s); }
  void mulsd(int d, int s) { sse(0xF2, 0x59, d, s); }
  void divsd(int d, int s) { sse(0xF2, 0x5E, d, s); }
  void sqrtsd(int d, int s) { sse(0xF2, 0x51, d, s); }
  void ucomisd(int a_, int b_) { sse(0x66, 0x2E, a_, b_); }
  void xorpd(int d, int s) { sse(0x66, 0x57, d, s); }
  void cvtsi2sd(int x, int r) { u8(0xF2); u8(0x48); u8(0x0F); u8(0x2A); modrm_reg(x, r); }
  void cvttsd2si(int r, int x) { u8(0xF2); u8(0x48); u8(0x0F); u8(0x2C); modrm_reg(r, x); }
  void movq_xr(int x, int r) { u8(0x66); u8(0x48); u8(0x0F); u8(0x6E); modrm_reg(x, r); }
  void movq_rx(int r, int x) { u8(0x66); u8(0x48); u8(0x0F); u8(0x7E); modrm_reg(x, r); }

  // control flow
  void rel32(Label& l) {
    if (l.pos >= 0) u32((uint32_t)(l.pos - (int)(b.size() + 4)));
    else { l.fixups.push_back(b.size()); u32(0); }
  }
  void jmp(Label& l) { u8(0xE9); rel32(l); }
  void jcc(int cc, Label& l) { u8(0x0F); u8(0x80 + cc); rel32(l); }
  void call(Label& l) { u8(0xE8); rel32(l); }
  void bind(Label& l) {
    l.pos = (int)b.size();
    for (size_t at : l.fixups) {
      const uint32_t rel = (uint32_t)(l.pos - (int)(at + 4));
      for (int i = 0; i < 4; ++i) b[at + i] = (uint8_t)(rel >> (8 * i));
    }
    l.fixups.clear();
  }
};

// ---- runtime: stack limit, traps, executable memory ---------------------------------

enum Trap { kTrapIndex = 1, kTrapDivide = 2, kTrapStack = 3, kTrapByte = 4 };

// Generated code compares rsp with the calling thread's stack limit on entry. Where a thread_local sits at the
// same distance from the thread pointer in every thread (Linux x86-64, the program's own TLS), the code reads it
// through fs and native calls run on many threads at once. Elsewhere it reads one global, and calls take turns.
uint64_t g_stack_limit = 0;
thread_local uint64_t t_stack_limit = 0;
thread_local std::jmp_buf* t_jmp = nullptr;
thread_local int t_trap = 0;

extern "C" void grain_native_trap(int code) {
  t_trap = code;
  std::longjmp(*t_jmp, 1);
}

#if defined(__linux__)
int64_t fs_offset_of_limit() {
  uintptr_t tp;
  __asm__ volatile("mov %%fs:0, %0" : "=r"(tp));
  return (int64_t)((uintptr_t)&t_stack_limit - tp);
}
#endif

// The fs displacement of t_stack_limit, or 0 when it is not the same in every thread (then: the global, a lock).
int64_t tls_limit_offset() {
  static const int64_t off = [] {
#if defined(__linux__)
    const int64_t here = fs_offset_of_limit();
    int64_t there = 0;
    std::thread([&] { there = fs_offset_of_limit(); }).join();
    if (here != there || here == 0 || here < INT32_MIN || here > INT32_MAX) return (int64_t)0;
    return here;
#else
    return (int64_t)0;
#endif
  }();
  return off;
}

void* make_executable(const std::vector<uint8_t>& code, std::string& error) {
  const size_t page = (size_t)::sysconf(_SC_PAGESIZE);
  const size_t len = (code.size() + page - 1) / page * page;
  void* mem = ::mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) { error = "cannot allocate memory for native code"; return nullptr; }
  std::memcpy(mem, code.data(), code.size());
  if (::mprotect(mem, len, PROT_READ | PROT_EXEC) != 0) { ::munmap(mem, len); error = "cannot make native code executable (is W^X enforced?)"; return nullptr; }
  return mem;   // never freed: a function lives as long as the process
}

// ---- code generation ---------------------------------------------------------------------

class Gen {
 public:
  explicit Gen(Fn& f) : f_(f) {}

  bool generate(std::vector<uint8_t>& out, std::string& error) {
    a_.bind(start_);
    prologue();
    for (auto& s : f_.body) stmt(*s);
    // falling off the end returns zero
    a_.mov_ri(RAX, 0);
    a_.movq_xr(0, RAX);
    a_.bind(epilogue_);
    a_.mov_rr(RSP, RBP);
    a_.pop(RBP);
    a_.ret();
    traps();
    if (!error_.empty()) { error = error_; return false; }
    out = std::move(a_.b);
    return true;
  }

 private:
  Fn& f_;
  Asm a_;
  Label start_, epilogue_, trap_[5];
  bool trap_used_[5] = {false, false, false, false, false};
  int depth_ = 0;   // 8-byte words pushed on top of the frame
  struct Loop { Label* brk; Label* cont; };
  std::vector<Loop> loops_;
  std::deque<Label> temp_;
  std::string error_;

  Label& label() { temp_.emplace_back(); return temp_.back(); }
  static int32_t off(int slot) { return -8 * (slot + 1); }
  void trap_if(int cc, int code) { trap_used_[code] = true; a_.jcc(cc, trap_[code]); }

  void push_rax() { a_.push(RAX); ++depth_; }
  void pop_to(int r) { a_.pop(r); --depth_; }
  void push_xmm0() { a_.sub_rsp(8); a_.movsd_mx(RSP, 0, 0); ++depth_; }
  void pop_xmm(int x) { a_.movsd_xm(x, RSP, 0); a_.add_rsp(8); --depth_; }

  void prologue() {
    static const int int_regs[6] = {RDI, RSI, RDX, RCX, R8, R9};
    a_.push(RBP);
    a_.mov_rr(RBP, RSP);
    const int frame = ((f_.nslots * 8) + 15) & ~15;
    if (frame) a_.sub_rsp(frame);
    if (const int64_t tls = tls_limit_offset()) {
      a_.mov_fs(RAX, (int32_t)tls);   // this thread's limit
    } else {
      a_.mov_ri(RAX, (uint64_t)(uintptr_t)&g_stack_limit);
      a_.mov_rm(RAX, RAX, 0);
    }
    a_.cmp(RSP, RAX);
    trap_if(CC_B, kTrapStack);
    int ii = 0, fi = 0, si = 0;
    auto take_int = [&](int slot) {
      if (ii < 6) a_.mov_mr(RBP, off(slot), int_regs[ii++]);
      else { a_.mov_rm(RAX, RBP, 16 + 8 * si++); a_.mov_mr(RBP, off(slot), RAX); }
    };
    for (Var* p : f_.params) {
      if (p->ty == Ty::Bytes) { take_int(p->slot); take_int(p->slot + 1); }
      else if (p->ty == Ty::F64) {
        if (fi < 8) a_.movsd_mx(RBP, off(p->slot), fi++);
        else { a_.mov_rm(RAX, RBP, 16 + 8 * si++); a_.mov_mr(RBP, off(p->slot), RAX); }
      } else take_int(p->slot);
    }
  }

  void traps() {
    for (int code = 1; code <= 4; ++code) {
      if (!trap_used_[code]) continue;
      a_.bind(trap_[code]);
      a_.mov_ri(RDI, (uint64_t)code);
      a_.and_rsp_16();
      a_.mov_ri(RAX, (uint64_t)(uintptr_t)&grain_native_trap);
      a_.call_r(RAX);
    }
  }

  // ---- statements
  void store_var(Var* v) {   // the value in rax / xmm0 into v
    if (v->ty == Ty::F64) a_.movsd_mx(RBP, off(v->slot), 0);
    else a_.mov_mr(RBP, off(v->slot), RAX);
  }

  void value_into(Var* v, E& e) {
    if (v->ty == Ty::Bytes) {
      a_.mov_rm(RAX, RBP, off(e.var->slot));     a_.mov_mr(RBP, off(v->slot), RAX);
      a_.mov_rm(RAX, RBP, off(e.var->slot + 1)); a_.mov_mr(RBP, off(v->slot + 1), RAX);
      return;
    }
    expr(e);
    store_var(v);
  }

  void stmt(S& s) {
    switch (s.k) {
      case SK::Let: case SK::Assign: value_into(s.var, *s.e); return;
      case SK::Expr: expr(*s.e); return;
      case SK::IndexSet: {
        expr(*s.e);
        push_rax();
        expr(*s.idx);
        a_.mov_rm(RCX, RBP, off(s.var->slot + 1));
        a_.cmp(RAX, RCX);
        trap_if(CC_AE, kTrapIndex);
        a_.mov_rm(RCX, RBP, off(s.var->slot));
        a_.add(RAX, RCX);
        pop_to(RDX);
        a_.mov_rr(RCX, RDX);
        a_.mov_ri(RDX, 255);
        a_.cmp(RCX, RDX);          // unsigned: negative or above 255
        trap_if(CC_A, kTrapByte);
        a_.mov_rr(RDX, RCX);
        a_.store_u8();
        return;
      }
      case SK::If: {
        Label& other = label();
        Label& end = label();
        expr(*s.e);
        a_.test(RAX, RAX);
        a_.jcc(CC_E, other);
        for (auto& c : s.body) stmt(*c);
        a_.jmp(end);
        a_.bind(other);
        for (auto& c : s.other) stmt(*c);
        a_.bind(end);
        return;
      }
      case SK::While: {
        Label& top = label();
        Label& cont = label();
        Label& end = label();
        a_.bind(top);
        expr(*s.e);
        a_.test(RAX, RAX);
        a_.jcc(CC_E, end);
        loops_.push_back({&end, &cont});
        for (auto& c : s.body) stmt(*c);
        loops_.pop_back();
        a_.bind(cont);
        for (auto& c : s.other) stmt(*c);   // a for loop's step
        a_.jmp(top);
        a_.bind(end);
        return;
      }
      case SK::Return:
        if (s.e) expr(*s.e);
        a_.jmp(epilogue_);
        return;
      case SK::Break: a_.jmp(*loops_.back().brk); return;
      case SK::Continue: a_.jmp(*loops_.back().cont); return;
    }
  }

  // ---- expressions: the value is left in rax (i64, bool, ptr) or xmm0 (f64)
  void bytes_ptr_len_checked_index(E& b, E& index) {   // rax = address of b[index]
    expr(index);
    a_.mov_rm(RCX, RBP, off(b.var->slot + 1));
    a_.cmp(RAX, RCX);
    trap_if(CC_AE, kTrapIndex);
    a_.mov_rm(RCX, RBP, off(b.var->slot));
    a_.add(RAX, RCX);
  }

  void load_by(E& e) {
    if (e.real) { a_.load_f64(); return; }
    switch (e.size) {
      case 1: e.sgn ? a_.load_i8() : a_.load_u8(); break;
      case 2: e.sgn ? a_.load_i16() : a_.load_u16(); break;
      case 4: e.sgn ? a_.load_i32() : a_.load_u32(); break;
      default: a_.load_u64(); break;
    }
  }

  void expr(E& e) {
    switch (e.k) {
      case EK::Int: a_.mov_ri(RAX, (uint64_t)e.iv); return;
      case EK::Bool: a_.mov_ri(RAX, (uint64_t)e.iv); return;
      case EK::Real: { uint64_t bits; std::memcpy(&bits, &e.dv, 8); a_.mov_ri(RAX, bits); a_.movq_xr(0, RAX); return; }
      case EK::Var:
        if (e.var->ty == Ty::F64) a_.movsd_xm(0, RBP, off(e.var->slot));
        else if (e.var->ty == Ty::Bytes) error_ = "internal: a bytes value used as a number";
        else a_.mov_rm(RAX, RBP, off(e.var->slot));
        return;
      case EK::Neg:
        expr(*e.a);
        if (e.ty == Ty::F64) { a_.mov_ri(RAX, 0x8000000000000000ull); a_.movq_xr(1, RAX); a_.xorpd(0, 1); }
        else a_.neg(RAX);
        return;
      case EK::Not: expr(*e.a); a_.mov_ri(RCX, 1); a_.xor_(RAX, RCX); return;
      case EK::BitNot: expr(*e.a); a_.not_(RAX); return;
      case EK::AndAnd: case EK::OrOr: {
        Label& end = label();
        expr(*e.a);
        a_.test(RAX, RAX);
        a_.jcc(e.k == EK::AndAnd ? CC_E : CC_NE, end);
        expr(*e.b);
        a_.bind(end);
        return;
      }
      case EK::Cast: {
        if (e.a->ty == Ty::Bytes) { a_.mov_rm(RAX, RBP, off(e.a->var->slot)); return; }
        expr(*e.a);
        if (e.a->ty == Ty::I64 && e.ty == Ty::F64) a_.cvtsi2sd(0, RAX);
        else if (e.a->ty == Ty::F64 && e.ty == Ty::I64) a_.cvttsd2si(RAX, 0);
        return;
      }
      case EK::Len: a_.mov_rm(RAX, RBP, off(e.a->var->slot + 1)); return;
      case EK::Index:
        bytes_ptr_len_checked_index(*e.a, *e.b);
        a_.load_u8();
        return;
      case EK::Sqrt: expr(*e.a); a_.sqrtsd(0, 0); return;
      case EK::Fabs: expr(*e.a); a_.movq_rx(RAX, 0); a_.mov_ri(RCX, 0x7FFFFFFFFFFFFFFFull); a_.and_(RAX, RCX); a_.movq_xr(0, RAX); return;
      case EK::Rd: {
        if (e.a->ty == Ty::Bytes) {
          expr(*e.b);
          a_.mov_rm(RCX, RBP, off(e.a->var->slot + 1));
          a_.mov_ri(RDX, (uint64_t)e.size);
          a_.cmp(RCX, RDX);
          trap_if(CC_B, kTrapIndex);          // fewer than `size` bytes in all
          a_.sub(RCX, RDX);
          a_.cmp(RAX, RCX);
          trap_if(CC_A, kTrapIndex);          // offset past len - size (or negative)
          a_.mov_rm(RCX, RBP, off(e.a->var->slot));
          a_.add(RAX, RCX);
        } else {
          expr(*e.a); push_rax(); expr(*e.b); pop_to(RCX); a_.add(RAX, RCX);
        }
        load_by(e);
        return;
      }
      case EK::Wr: {
        // value, then base, then offset
        if (e.real) { expr(*e.c); a_.movq_rx(RAX, 0); push_rax(); }
        else { expr(*e.c); push_rax(); }
        if (e.a->ty == Ty::Bytes) {
          expr(*e.b);
          a_.mov_rm(RCX, RBP, off(e.a->var->slot + 1));
          a_.mov_ri(RDX, (uint64_t)e.size);
          a_.cmp(RCX, RDX);
          trap_if(CC_B, kTrapIndex);
          a_.sub(RCX, RDX);
          a_.cmp(RAX, RCX);
          trap_if(CC_A, kTrapIndex);
          a_.mov_rm(RCX, RBP, off(e.a->var->slot));
          a_.add(RAX, RCX);
        } else {
          expr(*e.a); push_rax(); expr(*e.b); pop_to(RCX); a_.add(RAX, RCX);
        }
        pop_to(RDX);
        if (e.real) { a_.movq_xr(0, RDX); a_.store_f64(); }
        else switch (e.size) {
          case 1: a_.store_u8(); break;
          case 2: a_.store_u16(); break;
          case 4: a_.store_u32(); break;
          default: a_.store_u64(); break;
        }
        return;
      }
      case EK::Bin: binary(e); return;
      case EK::Cmp: compare(e); return;
      case EK::Call: call(e); return;
    }
  }

  // a op b for i64 / ptr (rax, rcx) and f64 (xmm0, xmm1)
  void operands(E& e) {
    if (e.a->ty == Ty::F64) {
      expr(*e.a); push_xmm0(); expr(*e.b); a_.movsd_xx(1, 0); pop_xmm(0);
    } else {
      expr(*e.a); push_rax(); expr(*e.b); a_.mov_rr(RCX, RAX); pop_to(RAX);
    }
  }

  void binary(E& e) {
    operands(e);
    if (e.ty == Ty::F64) {
      switch (e.op) {
        case Tok::Plus: a_.addsd(0, 1); break;
        case Tok::Minus: a_.subsd(0, 1); break;
        case Tok::Star: a_.mulsd(0, 1); break;
        default: a_.divsd(0, 1); break;
      }
      return;
    }
    switch (e.op) {
      case Tok::Plus: a_.add(RAX, RCX); return;
      case Tok::Minus: a_.sub(RAX, RCX); return;
      case Tok::Star: a_.imul(RAX, RCX); return;
      case Tok::Amp: a_.and_(RAX, RCX); return;
      case Tok::Pipe: a_.or_(RAX, RCX); return;
      case Tok::Caret: a_.xor_(RAX, RCX); return;
      case Tok::Slash: case Tok::Percent: {
        Label& minus_one = label();
        Label& done = label();
        a_.test(RCX, RCX);
        trap_if(CC_E, kTrapDivide);
        a_.cmp_ri8(RCX, -1);                 // INT64_MIN / -1 would fault
        a_.jcc(CC_E, minus_one);
        a_.cqo();
        a_.idiv(RCX);
        if (e.op == Tok::Percent) a_.mov_rr(RAX, RDX);
        a_.jmp(done);
        a_.bind(minus_one);
        if (e.op == Tok::Slash) a_.neg(RAX); else a_.mov_ri(RAX, 0);
        a_.bind(done);
        return;
      }
      case Tok::LtLt: {
        Label& zero = label();
        Label& done = label();
        a_.cmp_ri8(RCX, 63);
        a_.jcc(CC_A, zero);                  // a count above 63 (or negative) shifts everything out
        a_.shl_cl(RAX);
        a_.jmp(done);
        a_.bind(zero);
        a_.mov_ri(RAX, 0);
        a_.bind(done);
        return;
      }
      case Tok::GtGt: {
        Label& ok = label();
        a_.cmp_ri8(RCX, 63);
        a_.jcc(CC_BE, ok);
        a_.mov_ri(RCX, 63);                  // the sign, as -8 >> 99 is -1
        a_.bind(ok);
        a_.sar_cl(RAX);
        return;
      }
      default: error_ = "internal: unknown binary operator"; return;
    }
  }

  void compare(E& e) {
    operands(e);
    if (e.a->ty == Ty::F64) {
      // ucomisd sets CF/ZF/PF; NaN sets all three, so every ordered test below is false for NaN.
      switch (e.op) {
        case Tok::Lt: a_.ucomisd(1, 0); a_.setcc(CC_A, RAX); break;     // b > a
        case Tok::LtEq: a_.ucomisd(1, 0); a_.setcc(CC_AE, RAX); break;
        case Tok::Gt: a_.ucomisd(0, 1); a_.setcc(CC_A, RAX); break;
        case Tok::GtEq: a_.ucomisd(0, 1); a_.setcc(CC_AE, RAX); break;
        case Tok::EqEq: a_.ucomisd(0, 1); a_.setcc(CC_E, RAX); a_.setcc(CC_NP, RCX); a_.movzx_eax_al(); a_.u8(0x0F); a_.u8(0xB6); a_.u8(0xC9); a_.and_(RAX, RCX); return;
        default: a_.ucomisd(0, 1); a_.setcc(CC_NE, RAX); a_.setcc(CC_P, RCX); a_.movzx_eax_al(); a_.u8(0x0F); a_.u8(0xB6); a_.u8(0xC9); a_.or_(RAX, RCX); return;
      }
      a_.movzx_eax_al();
      return;
    }
    a_.cmp(RAX, RCX);
    int cc = CC_E;
    switch (e.op) {
      case Tok::EqEq: cc = CC_E; break;
      case Tok::BangEq: cc = CC_NE; break;
      case Tok::Lt: cc = CC_L; break;
      case Tok::LtEq: cc = CC_LE; break;
      case Tok::Gt: cc = CC_G; break;
      default: cc = CC_GE; break;
    }
    a_.setcc(cc, RAX);
    a_.movzx_eax_al();
  }

  void call(E& e) {
    static const int int_regs[6] = {RDI, RSI, RDX, RCX, R8, R9};
    // Push every argument word in order: an i64/ptr/bool, an f64 (its bits), or a bytes' pointer then length.
    struct Word { bool fp; };
    std::vector<Word> words;
    for (auto& arg : e.args) {
      if (arg->ty == Ty::Bytes) {
        a_.mov_rm(RAX, RBP, off(arg->var->slot));     push_rax(); words.push_back({false});
        a_.mov_rm(RAX, RBP, off(arg->var->slot + 1)); push_rax(); words.push_back({false});
      } else if (arg->ty == Ty::F64) {
        expr(*arg); push_xmm0(); words.push_back({true});
      } else {
        expr(*arg); push_rax(); words.push_back({false});
      }
    }
    const int total = (int)words.size();
    // Which words travel in registers, and which go on the stack (in order).
    std::vector<int> int_word, fp_word, stack_word;
    for (int j = 0; j < total; ++j) {
      if (words[j].fp) { if (fp_word.size() < 8) fp_word.push_back(j); else stack_word.push_back(j); }
      else { if (int_word.size() < 6) int_word.push_back(j); else stack_word.push_back(j); }
    }
    std::sort(stack_word.begin(), stack_word.end());
    const int pad = (depth_ + (int)stack_word.size()) & 1;
    if (pad) { a_.sub_rsp(8); ++depth_; }
    int pushed = 0;   // words pushed above the originals (padding included)
    if (pad) ++pushed;
    for (size_t k = stack_word.size(); k-- > 0;) {   // the last stack argument goes first
      const int j = stack_word[k];
      a_.mov_rm(RAX, RSP, (total - 1 - j + pushed) * 8);
      a_.push(RAX); ++depth_; ++pushed;
    }
    for (size_t k = 0; k < int_word.size(); ++k) a_.mov_rm(int_regs[k], RSP, (total - 1 - int_word[k] + pushed) * 8);
    for (size_t k = 0; k < fp_word.size(); ++k) a_.movsd_xm((int)k, RSP, (total - 1 - fp_word[k] + pushed) * 8);
    if (e.fn == &f_) { a_.call(start_); }
    else {
      void* target = e.fn->is_extern ? e.fn->extern_addr : e.fn->code;
      a_.mov_ri(RAX, (uint64_t)(uintptr_t)target);
      a_.call_r(RAX);
    }
    a_.add_rsp((total + pushed) * 8);
    depth_ -= total + pushed;
    if (e.fn->ret_narrow32) a_.movsxd_rax_eax();
  }
};

#endif  // GRAIN_NATIVE_X64

// ---- calling generated code from the VM ------------------------------------------------

#if defined(GRAIN_NATIVE_X64)

using IntShim = uint64_t (*)(uint64_t, ...);
using FpShim = double (*)(uint64_t, ...);

std::vector<std::shared_ptr<Fn>>& registry() {
  static std::vector<std::shared_ptr<Fn>> r;   // native functions live as long as the process
  return r;
}

// The registry's lock, and - where the stack limit is one global (tls_limit_offset() == 0) - the lock native
// calls take turns on, so a web server running requests on many threads stays correct.
std::mutex& native_mutex() {
  static std::mutex m;
  return m;
}

struct Regs {
  uint64_t i[6] = {0};
  double d[8] = {0};
  uint64_t s[8] = {0};
  int ni = 0, nd = 0, ns = 0;
};

// noinline: it owns the setjmp, and nothing with a destructor may be live across it.
__attribute__((noinline)) int guarded_call(void* code, const Regs& r, bool fp, uint64_t* iret, double* dret) {
  std::jmp_buf jb;
  uint64_t& limit = tls_limit_offset() ? t_stack_limit : g_stack_limit;
  std::jmp_buf* prev = t_jmp;
  const uint64_t prev_limit = limit;
  t_jmp = &jb;
  limit = (uint64_t)(uintptr_t)__builtin_frame_address(0) - 256 * 1024;   // native recursion gets 256 KB
  if (setjmp(jb)) {
    t_jmp = prev;
    limit = prev_limit;
    return t_trap;
  }
  if (fp) *dret = reinterpret_cast<FpShim>(code)(r.i[0], r.i[1], r.i[2], r.i[3], r.i[4], r.i[5],
                                                  r.d[0], r.d[1], r.d[2], r.d[3], r.d[4], r.d[5], r.d[6], r.d[7],
                                                  r.s[0], r.s[1], r.s[2], r.s[3], r.s[4], r.s[5], r.s[6], r.s[7]);
  else *iret = reinterpret_cast<IntShim>(code)(r.i[0], r.i[1], r.i[2], r.i[3], r.i[4], r.i[5],
                                                r.d[0], r.d[1], r.d[2], r.d[3], r.d[4], r.d[5], r.d[6], r.d[7],
                                                r.s[0], r.s[1], r.s[2], r.s[3], r.s[4], r.s[5], r.s[6], r.s[7]);
  t_jmp = prev;
  limit = prev_limit;
  return 0;
}

bool push_word(Regs& r, bool fp, uint64_t bits, std::string& error) {
  if (fp) {
    if (r.nd < 8) { std::memcpy(&r.d[r.nd++], &bits, 8); return true; }
  } else if (r.ni < 6) { r.i[r.ni++] = bits; return true; }
  if (r.ns < 8) { r.s[r.ns++] = bits; return true; }
  error = "too many arguments for a native call";
  return false;
}

Value method_native_call(Vm&, BoundMethodObj& bm, std::vector<Value>& args, std::string& error) {
  const size_t id = (size_t)static_cast<MapObj*>(bm.receiver.obj.get())->find("id")->i;
  std::shared_ptr<Fn> keep;
  {
    std::lock_guard<std::mutex> lock(native_mutex());
    keep = registry()[id];
  }
  Fn& f = *keep;
  Regs regs;
  for (size_t k = 0; k < f.params.size(); ++k) {
    const Var& p = *f.params[k];
    const Value& v = args[k];
    auto wrong = [&](const char* want) { error = f.name + "(): argument " + std::to_string(k + 1) + " is a " + type_name(v.type) + ", wanted " + want; return Value::nil(); };
    switch (p.ty) {
      case Ty::I64:
        if (v.type != Type::Int) return wrong("a whole number");
        if (!push_word(regs, false, (uint64_t)v.i, error)) return Value::nil();
        break;
      case Ty::F64:
        if (!v.is_number()) return wrong("a number");
        { double d = v.as_double(); uint64_t bits; std::memcpy(&bits, &d, 8); if (!push_word(regs, true, bits, error)) return Value::nil(); }
        break;
      case Ty::Bool:
        if (v.type != Type::Bool) return wrong("true or false");
        if (!push_word(regs, false, v.b ? 1 : 0, error)) return Value::nil();
        break;
      case Ty::Ptr:
        if (v.type == Type::Bytes) { if (!push_word(regs, false, (uint64_t)(uintptr_t)v.bytes_data().data(), error)) return Value::nil(); }
        else if (v.type == Type::Int) { if (!push_word(regs, false, (uint64_t)v.i, error)) return Value::nil(); }
        else return wrong("a whole number or bytes");
        break;
      case Ty::Bytes:
        if (v.type != Type::Bytes) return wrong("bytes");
        if (!push_word(regs, false, (uint64_t)(uintptr_t)v.bytes_data().data(), error) ||
            !push_word(regs, false, (uint64_t)v.bytes_data().size(), error)) return Value::nil();
        break;
      case Ty::Void: break;
    }
  }
  uint64_t iret = 0;
  double dret = 0;
  int trap;
  if (tls_limit_offset()) {
    trap = guarded_call(f.code, regs, f.ret == Ty::F64, &iret, &dret);   // this thread's own limit and trap
  } else {
    std::lock_guard<std::mutex> lock(native_mutex());
    trap = guarded_call(f.code, regs, f.ret == Ty::F64, &iret, &dret);
  }
  if (trap) {
    switch (trap) {
      case kTrapIndex: error = f.name + "(): index outside the bytes"; break;
      case kTrapDivide: error = f.name + "(): division by zero"; break;
      case kTrapStack: error = f.name + "(): native recursion too deep"; break;
      case kTrapByte: error = f.name + "(): a byte is a whole number from 0 to 255"; break;
      default: error = f.name + "(): native code stopped (" + std::to_string(trap) + ")"; break;
    }
    return Value::nil();
  }
  switch (f.ret) {
    case Ty::I64: case Ty::Ptr: return Value::integer((int64_t)iret);
    case Ty::F64: return Value::real(dret);
    case Ty::Bool: return Value::boolean((iret & 0xFF) != 0);
    default: return Value::nil();
  }
}

#endif  // GRAIN_NATIVE_X64

}  // namespace

bool compile_native_decl(NativeModule& mod, const std::vector<Token>& toks, std::string& name, Value& out,
                         std::vector<Diagnostic>& errors) {
  const int line = toks.empty() ? 0 : toks[0].line;
  if (!g_policy.allow) {
    errors.push_back(Diagnostic{line, "native fn is not available here: the host has not turned native code on"});
    return false;
  }
#if !defined(GRAIN_NATIVE_X64)
  (void)mod; (void)name; (void)out;
  errors.push_back(Diagnostic{line, "native fn needs x86-64 Linux or macOS for now (AArch64 and Windows are not done)"});
  return false;
#else
  NParser parser(toks, mod, errors);
  std::shared_ptr<Fn> fn = parser.declaration();
  if (!fn || !parser.ok()) return false;
  if (fn->is_extern) { name.clear(); out = Value::nil(); return true; }   // an extern fn only declares a C function for native code
  std::vector<uint8_t> code;
  std::string why;
  Gen gen(*fn);
  if (!gen.generate(code, why)) { errors.push_back(Diagnostic{line, "native fn " + fn->name + ": " + why}); mod.fns.erase(fn->name); return false; }
  fn->code = make_executable(code, why);
  if (!fn->code) { errors.push_back(Diagnostic{line, "native fn " + fn->name + ": " + why}); mod.fns.erase(fn->name); return false; }
  int64_t id;
  {
    std::lock_guard<std::mutex> lock(native_mutex());
    registry().push_back(fn);
    id = (int64_t)registry().size() - 1;
  }
  auto rec = std::make_shared<MapObj>();
  rec->put("id", Value::integer(id));
  name = fn->name;
  const int argc = (int)fn->params.size();
  out = Value::object(Type::BoundMethod, std::make_shared<BoundMethodObj>(Value::object(Type::Map, rec), fn->name, argc, argc, method_native_call));
  return true;
#endif
}

}  // namespace aios::userland::grain
