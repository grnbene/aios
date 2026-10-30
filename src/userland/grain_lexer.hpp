#pragma once
// Tokens for grain. Private to the front end: nothing outside the lexer, the
// parser and their tests has any business naming a token kind.
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace aios::userland::grain {

enum class Tok : uint8_t {
  End, Newline, Error,
  Int, Real, Str, FStr, Ident,
  And, Or, Not, If, Else, While, For, In, Fn, Return, Break, Continue, Let,
  True, False, NilLit,
  Plus, Minus, Star, Slash, Percent,
  Amp, Pipe, Caret, Tilde, LtLt, GtGt,
  Assign, PlusEq, MinusEq, StarEq, SlashEq, PercentEq,
  AmpEq, PipeEq, CaretEq, LtLtEq, GtGtEq,
  EqEq, BangEq, Lt, LtEq, Gt, GtEq,
  LParen, RParen, LBrace, RBrace, LBracket, RBracket,
  Comma, Dot, Colon, ColonColon, Question, QuestionDot,
  Entity, On,
  Match, QuestionQuestion, FatArrow   // `match x { a => ... }`, `a ?? b`
};

const char* tok_name(Tok t);

struct Token {
  Tok kind = Tok::End;
  std::string text;      // identifiers, strings, and the error message
  int64_t integer = 0;
  double real = 0;
  int line = 1;
  int column = 1;
  // Last, after the fields Token{...} lists by position: a Real written with a point and no exponent, or an int past
  // 64 bits - an exact decimal in grain (its digits are `text`); a native fn reads `real`.
  bool exact = false;
};

class Lexer {
 public:
  explicit Lexer(const std::string& source);
  Token next();

 private:
  char peek(size_t ahead = 0) const;
  char advance();
  void skip_run();
  Token string_token(bool is_fstr = false);
  Token number_token();

  const std::string& src_;
  size_t pos_ = 0;
  int line_ = 1;
  size_t line_start_pos_ = 0;
};

std::vector<Token> lex_all(const std::string& source);

}  // namespace aios::userland::grain
