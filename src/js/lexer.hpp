#pragma once
// The tokeniser. It is driven by the parser, which tells it when a `/` starts a regular expression and when a
// `}` closes a template substitution.
#include <cstdint>
#include <string>

namespace aios::js {

enum class TT : uint8_t { End, Ident, Keyword, Punct, Number, String, Template, RegExp, Private };

enum class P : uint8_t {
  None,
  LBrace, RBrace, LParen, RParen, LBracket, RBracket, Dot, Ellipsis, Semi, Comma, Colon, Question, QDot,
  Lt, Gt, Le, Ge, EqEq, NotEq, EqEqEq, NotEqEq,
  Plus, Minus, Star, Slash, Percent, StarStar, PlusPlus, MinusMinus,
  Shl, Shr, UShr, Amp, Pipe, Caret, Bang, Tilde, AmpAmp, PipePipe, QQ,
  Assign, PlusAssign, MinusAssign, StarAssign, SlashAssign, PercentAssign, StarStarAssign,
  ShlAssign, ShrAssign, UShrAssign, AmpAssign, PipeAssign, CaretAssign, AmpAmpAssign, PipePipeAssign, QQAssign,
  Arrow,
};

enum class Kw : uint8_t {
  None, Break, Case, Catch, Class, Const, Continue, Debugger, Default, Delete, Do, Else, Enum, Export, Extends,
  False, Finally, For, Function, If, Import, In, InstanceOf, New, Null, Return, Super, Switch, This, Throw, True,
  Try, TypeOf, Var, Void, While, With,
};

struct Token {
  TT type = TT::End;
  P punct = P::None;
  Kw kw = Kw::None;
  uint32_t start = 0, end = 0;
  uint32_t line = 1;
  bool nl_before = false;       // a line terminator between the previous token and this one
  bool escaped = false;         // an identifier/keyword written with \u escapes
  bool legacy_octal = false;    // 017, "\07": a SyntaxError in strict mode
  std::u16string str;           // identifier name, string value, template cooked text, regexp pattern
  std::u16string str2;          // regexp flags, template raw text
  double num = 0;
  bool template_tail = false;   // the template chunk ends with ` (not ${)
  bool template_cooked_ok = true;
  bool bigint = false;
  std::string error;            // set on a malformed token (the parser reports it)
};

class Lexer {
 public:
  Lexer(const char16_t* src, uint32_t len) : src_(src), len_(len) {}
  Token next();
  // After the parser saw a `/` or `/=` token where an expression may start: re-read it as a regular expression.
  Token rescan_regex(const Token& slash);
  // After the parser consumed the expression of a `${ ... }`: re-read from the `}` as the rest of the template.
  Token rescan_template_continuation(const Token& rbrace);

  struct State {
    uint32_t pos;
    uint32_t line;
    uint32_t line_start;
  };
  State save() const { return {pos_, line_, line_start_}; }
  void restore(const State& s) { pos_ = s.pos; line_ = s.line; line_start_ = s.line_start; }
  uint32_t pos() const { return pos_; }
  uint32_t line_start() const { return line_start_; }
  uint32_t length() const { return len_; }
  const char16_t* source() const { return src_; }
  bool strict = false;          // octal literals are errors once strict code has been entered

  static bool is_id_start(uint32_t cp);
  static bool is_id_part(uint32_t cp);
  static bool is_line_terminator(char16_t c) { return c == 0x0A || c == 0x0D || c == 0x2028 || c == 0x2029; }
  static bool is_space(char16_t c);

 private:
  const char16_t* src_;
  uint32_t len_;
  uint32_t pos_ = 0;
  uint32_t line_ = 1;
  uint32_t line_start_ = 0;

  bool skip_trivia();           // returns whether a line terminator was crossed
  void read_identifier(Token& t, bool is_private);
  void read_number(Token& t);
  void read_string(Token& t, char16_t quote);
  void read_template_chunk(Token& t);   // after ` or }
  bool read_escape(std::u16string& out, bool in_template, bool* octal);
  void newline_at(uint32_t i);
  char16_t at(uint32_t i) const { return i < len_ ? src_[i] : 0; }
};

}  // namespace aios::js
