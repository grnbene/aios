#pragma once
// The tree between the parser and the compiler.
//
// It exists so the compiler has something to look at twice. A parser that
// emits bytecode as it goes can never constant-fold, can never warn that a
// local is never read, and can never be pointed at a second target -- and the
// whole reason for building a language from the ground up was to not be stuck
// with the first shape it took.
#include <memory>
#include <string>
#include <vector>

#include "grain_lexer.hpp"

namespace aios::userland::grain {

struct Expr;
struct Stmt;
using ExprP = std::unique_ptr<Expr>;
using StmtP = std::unique_ptr<Stmt>;

enum class ExprKind : uint8_t {
  IntLit, RealLit, StrLit, BoolLit, NilLit,
  Name,        // a
  Unary,       // -a, not a, ~a
  Binary,      // a + b
  Logical,     // a and b   (short-circuits, so not a Binary)
  Ternary,     // a ? b : c
  Assign,      // a = b
  Call,        // f(a, b)
  Index,       // a[b]
  IndexSet,    // a[b] = c
  Field,       // a.b
  FieldSet,    // a.b = c
  ListLit,     // [a, b]
  MapLit,      // {k: v}
  Lambda       // fn (a) { ... } used as a value
};

struct Param { std::string name; int line = 0; };

struct Expr {
  ExprKind kind;
  int line = 0;

  int64_t integer = 0;
  double real = 0;
  std::string text;          // string literal, or the name
  Tok op = Tok::End;         // Unary / Binary / Logical; on Assign/IndexSet/FieldSet, the operator of a compound `op=`

  ExprP a, b, c;             // operands; Index uses a[b], IndexSet a[b] = c
  std::vector<ExprP> items;  // call arguments, list elements, map values
  std::vector<std::string> keys;   // map keys, parallel to items

  // Lambda body. A named `fn` declaration is a Let of a Lambda, so there is
  // one code path for both and closures work the same either way.
  std::vector<Param> params;
  std::vector<StmtP> body;
  std::string fn_name = "<fn>";
};

struct OnHandler {
  std::string event;
  std::string param_type;
  std::string param;
  std::vector<StmtP> body;
  int line = 0;
};

struct EntityDef {
  std::string name;
  std::vector<std::pair<std::string, ExprP>> fields;
  std::vector<std::unique_ptr<OnHandler>> handlers;
  int line = 0;
};

enum class StmtKind : uint8_t {
  Expression, Let, Block, If, While, ForIn, Return, Break, Continue, EntityDecl,
  NativeDecl   // `native fn` / `extern fn`: its tokens, compiled to machine code (grain_native.cpp)
};

struct Stmt {
  StmtKind kind;
  int line = 0;

  ExprP expr;                 // Expression, Let initialiser, If/While condition, Return
  std::string name;           // Let target, ForIn variable
  std::vector<StmtP> body;    // Block, loop body, if-then
  std::vector<StmtP> other;   // else branch
  ExprP iterable;             // ForIn
  std::unique_ptr<EntityDef> entity_def;
  std::vector<Token> native_tokens;   // NativeFn
};

}  // namespace aios::userland::grain
