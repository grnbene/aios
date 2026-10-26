#pragma once
// Private interface between grain's lexer and its compiler.
#include <vector>

#include "aios/userland/grain.hpp"
#include "grain_ast.hpp"
#include "grain_lexer.hpp"

namespace aios::userland::grain {

class Parser {
 public:
  Parser(std::vector<Token> tokens, std::vector<Diagnostic>* errors);
  std::vector<StmtP> parse_program();
  ExprP expression();
  // Nothing but newlines left - how an f-string's {expression} is checked
  // for trailing junk.
  bool finished();

 private:
  const Token& peek(size_t ahead = 0) const;
  const Token& advance();
  bool check(Tok k) const;
  bool match(Tok k);
  bool expect(Tok k, const char* what);
  void error_at(const Token& t, const std::string& message);
  void skip_newlines();
  void synchronise();

  ExprP binary(int min_power);
  ExprP unary();
  ExprP postfix(ExprP left);
  ExprP primary();
  ExprP list_literal();
  // `...` written together (three `.` tokens side by side) before an argument or list element: eats it.
  bool match_spread();
  // `match x { ... }`, as a value or (as_statement) a statement whose arms may be blocks
  ExprP match_expr(bool as_statement);
  bool no_brace_call_ = false;   // while a match subject is read: `x {` is not a call of x with a map
  ExprP map_literal();
  ExprP lambda();

  StmtP statement();
  StmtP entity_declaration();
  std::vector<StmtP> block_body();

  std::vector<Token> toks_;
  std::vector<Diagnostic>* errors_;
  size_t pos_ = 0;
  bool panicked_ = false;
};

}  // namespace aios::userland::grain
