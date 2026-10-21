// grain, stage one: text to tokens.
//
// Newlines are tokens rather than whitespace. The alternative is semicolons
// everywhere or significant indentation, and indentation is what makes `face`
// files break when an editor retabs them. A statement ends at a newline unless
// the line obviously cannot have ended -- a trailing operator, an open bracket
// -- which is decided here, once, rather than in the parser at every site.
#include "grain_lexer.hpp"

namespace aios::userland::grain {

namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_alpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool is_alnum(char c) { return is_alpha(c) || is_digit(c); }

struct Keyword { const char* text; Tok kind; };
const Keyword KEYWORDS[] = {
  {"and", Tok::And}, {"or", Tok::Or}, {"not", Tok::Not},
  {"if", Tok::If}, {"else", Tok::Else}, {"while", Tok::While}, {"for", Tok::For},
  {"in", Tok::In}, {"fn", Tok::Fn}, {"return", Tok::Return},
  {"break", Tok::Break}, {"continue", Tok::Continue}, {"let", Tok::Let},
  {"true", Tok::True}, {"false", Tok::False}, {"nil", Tok::NilLit},
};

}  // namespace

const char* tok_name(Tok t) {
  switch (t) {
    case Tok::End: return "end of input";
    case Tok::Newline: return "end of line";
    case Tok::Int: return "integer";
    case Tok::Real: return "number";
    case Tok::Str: return "string";
    case Tok::FStr: return "formatted string";
    case Tok::Ident: return "name";
    case Tok::And: return "and"; case Tok::Or: return "or"; case Tok::Not: return "not";
    case Tok::If: return "if"; case Tok::Else: return "else";
    case Tok::While: return "while"; case Tok::For: return "for"; case Tok::In: return "in";
    case Tok::Fn: return "fn"; case Tok::Return: return "return";
    case Tok::Break: return "break"; case Tok::Continue: return "continue";
    case Tok::Let: return "let"; case Tok::True: return "true";
    case Tok::False: return "false"; case Tok::NilLit: return "nil";
    case Tok::Plus: return "+"; case Tok::Minus: return "-";
    case Tok::Star: return "*"; case Tok::Slash: return "/"; case Tok::Percent: return "%";
    case Tok::Amp: return "&"; case Tok::Pipe: return "|"; case Tok::Caret: return "^"; case Tok::Tilde: return "~";
    case Tok::LtLt: return "<<"; case Tok::GtGt: return ">>";
    case Tok::Assign: return "=";
    case Tok::PlusEq: return "+="; case Tok::MinusEq: return "-=";
    case Tok::StarEq: return "*="; case Tok::SlashEq: return "/="; case Tok::PercentEq: return "%=";
    case Tok::AmpEq: return "&="; case Tok::PipeEq: return "|="; case Tok::CaretEq: return "^=";
    case Tok::LtLtEq: return "<<="; case Tok::GtGtEq: return ">>=";
    case Tok::EqEq: return "==" ; case Tok::BangEq: return "!=";
    case Tok::Lt: return "<"; case Tok::LtEq: return "<="; case Tok::Gt: return ">"; case Tok::GtEq: return ">=";
    case Tok::LParen: return "("; case Tok::RParen: return ")";
    case Tok::LBrace: return "{"; case Tok::RBrace: return "}";
    case Tok::LBracket: return "["; case Tok::RBracket: return "]";
    case Tok::Comma: return ","; case Tok::Dot: return "."; case Tok::Colon: return ":";
    case Tok::Question: return "?";
    case Tok::Error: return "bad character";
  }
  return "?";
}

Lexer::Lexer(const std::string& source) : src_(source) {}

char Lexer::peek(size_t ahead) const {
  return pos_ + ahead < src_.size() ? src_[pos_ + ahead] : '\0';
}

char Lexer::advance() {
  char c = src_[pos_++];
  if (c == '\n') line_++;
  return c;
}

void Lexer::skip_run() {
  for (;;) {
    char c = peek();
    if (is_space(c)) { advance(); continue; }
    // A backslash at end of line joins it to the next, for the rare statement
    // that genuinely wants to wrap without a bracket to hide behind.
    if (c == '\\' && peek(1) == '\n') { advance(); advance(); continue; }
    if (c == '#') { while (peek() && peek() != '\n') advance(); continue; }
    return;
  }
}

// A string literal, with the escapes a program actually needs. An unknown
// escape is an error rather than the character itself: "\d" is far more often
// a typo than an intent, and silently eating the backslash hides it.
Token Lexer::string_token(bool is_fstr) {
  const int start_line = line_;
  const char quote = advance();
  std::string out;
  for (;;) {
    char c = peek();
    if (c == '\0') return Token{Tok::Error, "unterminated string", 0, 0, start_line};
    if (c == '\n') return Token{Tok::Error, "unterminated string", 0, 0, start_line};
    advance();
    if (c == quote) break;
    if (c != '\\') { out.push_back(c); continue; }
    char e = peek();
    if (e == '\0') return Token{Tok::Error, "unterminated string", 0, 0, start_line};
    advance();
    switch (e) {
      case 'n': out.push_back('\n'); break;
      case 't': out.push_back('\t'); break;
      case 'r': out.push_back('\r'); break;
      case '0': out.push_back('\0'); break;
      case '\\': out.push_back('\\'); break;
      case '"': out.push_back('"'); break;
      case '\'': out.push_back('\''); break;
      // In an f-string \{ and \} are literal braces - kept doubled so the
      // interpolation parser can tell them from a {expression}. Elsewhere
      // they are the error any other unknown escape is.
      case '{': if (!is_fstr) return Token{Tok::Error, "unknown escape \\{ (only f-strings have braces to escape)", 0, 0, start_line}; out += "{{"; break;
      case '}': if (!is_fstr) return Token{Tok::Error, "unknown escape \\} (only f-strings have braces to escape)", 0, 0, start_line}; out += "}}"; break;
      default:
        return Token{Tok::Error, std::string("unknown escape \\") + e, 0, 0, start_line};
    }
  }
  return Token{is_fstr ? Tok::FStr : Tok::Str, out, 0, 0, start_line};
}

// Integer unless something in it forces a double: a decimal point or an
// exponent. `1e3` is a real, `1000` is an integer, and they are not the same
// value even though they print the same.
Token Lexer::number_token() {
  const int start_line = line_;
  const size_t start = pos_;
  // 0x.. and 0b..: at least one digit, and it has to fit in 64 bits -
  // "0x" alone used to be 0 and an overlong literal silently wrapped.
  if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X' || peek(1) == 'b' || peek(1) == 'B')) {
    const bool hex = peek(1) == 'x' || peek(1) == 'X';
    advance();
    advance();
    const size_t digits_at = pos_;
    auto is_hex = [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'); };
    while (hex ? is_hex(peek()) : (peek() == '0' || peek() == '1')) advance();
    const std::string digits = src_.substr(digits_at, pos_ - digits_at);
    const std::string text = src_.substr(start, pos_ - start);
    if (digits.empty()) return Token{Tok::Error, text + " has no digits", 0, 0, start_line};
    if (is_alpha(peek()) || is_digit(peek()))
      return Token{Tok::Error, std::string("'") + peek() + "' is not a " + (hex ? "hex" : "binary") + " digit", 0, 0, start_line};
    size_t first = digits.find_first_not_of('0');
    const size_t sig = first == std::string::npos ? 0 : digits.size() - first;
    if (sig > (hex ? 16u : 64u)) return Token{Tok::Error, text + " does not fit in 64 bits", 0, 0, start_line};
    Token t;
    t.line = start_line;
    t.text = text;
    t.kind = Tok::Int;
    t.integer = (int64_t)std::strtoull(digits.c_str(), nullptr, hex ? 16 : 2);
    return t;
  }
  bool real = false;
  while (is_digit(peek())) advance();
  if (peek() == '.' && is_digit(peek(1))) {
    real = true;
    advance();
    while (is_digit(peek())) advance();
  }
  if (peek() == 'e' || peek() == 'E') {
    const char sign = peek(1);
    const size_t digit_at = (sign == '+' || sign == '-') ? 2 : 1;
    if (is_digit(peek(digit_at))) {
      real = true;
      advance();
      if (sign == '+' || sign == '-') advance();
      while (is_digit(peek())) advance();
    }
  }
  const std::string text = src_.substr(start, pos_ - start);
  Token t;
  t.line = start_line;
  t.text = text;
  if (real) {
    t.kind = Tok::Real;
    t.real = std::strtod(text.c_str(), nullptr);
  } else {
    t.kind = Tok::Int;
    errno = 0;
    t.integer = (int64_t)std::strtoll(text.c_str(), nullptr, 10);
    // A literal too large for the integer it was written as becomes a real
    // rather than a silently wrapped negative.
    if (errno == ERANGE) { t.kind = Tok::Real; t.real = std::strtod(text.c_str(), nullptr); }
  }
  return t;
}

Token Lexer::next() {
  skip_run();
  if (pos_ >= src_.size()) return Token{Tok::End, "", 0, 0, line_};

  const char c = peek();
  if (c == '\n') { advance(); return Token{Tok::Newline, "", 0, 0, line_ - 1}; }
  if (c == '"' || c == '\'') return string_token(false);
  if (is_digit(c)) return number_token();

  if ((c == 'f' || c == 'F') && (peek(1) == '"' || peek(1) == '\'')) {
    advance();
    return string_token(true);
  }

  if (is_alpha(c)) {
    const size_t start = pos_;
    const int start_line = line_;
    while (is_alnum(peek())) advance();
    const std::string word = src_.substr(start, pos_ - start);
    for (const auto& k : KEYWORDS)
      if (word == k.text) return Token{k.kind, word, 0, 0, start_line};
    return Token{Tok::Ident, word, 0, 0, start_line};
  }

  const int start_line = line_;
  advance();
  auto two = [&](char want, Tok both, Tok single) {
    if (peek() == want) { advance(); return Token{both, "", 0, 0, start_line}; }
    return Token{single, "", 0, 0, start_line};
  };
  switch (c) {
    case '+': return two('=', Tok::PlusEq, Tok::Plus);
    case '-': return two('=', Tok::MinusEq, Tok::Minus);
    case '*': return two('=', Tok::StarEq, Tok::Star);
    case '/': return two('=', Tok::SlashEq, Tok::Slash);
    case '%': return two('=', Tok::PercentEq, Tok::Percent);
    case '^': return two('=', Tok::CaretEq, Tok::Caret);
    case '~': return Token{Tok::Tilde, "", 0, 0, start_line};
    case '&':
      if (peek() == '&') { advance(); return Token{Tok::And, "", 0, 0, start_line}; }
      if (peek() == '=') { advance(); return Token{Tok::AmpEq, "", 0, 0, start_line}; }
      return Token{Tok::Amp, "", 0, 0, start_line};
    case '|':
      if (peek() == '|') { advance(); return Token{Tok::Or, "", 0, 0, start_line}; }
      if (peek() == '=') { advance(); return Token{Tok::PipeEq, "", 0, 0, start_line}; }
      return Token{Tok::Pipe, "", 0, 0, start_line};
    case '(': return Token{Tok::LParen, "", 0, 0, start_line};
    case ')': return Token{Tok::RParen, "", 0, 0, start_line};
    case '{': return Token{Tok::LBrace, "", 0, 0, start_line};
    case '}': return Token{Tok::RBrace, "", 0, 0, start_line};
    case '[': return Token{Tok::LBracket, "", 0, 0, start_line};
    case ']': return Token{Tok::RBracket, "", 0, 0, start_line};
    case ',': return Token{Tok::Comma, "", 0, 0, start_line};
    case '.': return Token{Tok::Dot, "", 0, 0, start_line};
    case ':': return Token{Tok::Colon, "", 0, 0, start_line};
    case '?': return Token{Tok::Question, "", 0, 0, start_line};
    case ';': return Token{Tok::Newline, "", 0, 0, start_line};
    case '=': return two('=', Tok::EqEq, Tok::Assign);
    case '<':
      if (peek() == '<') {
        advance();
        if (peek() == '=') { advance(); return Token{Tok::LtLtEq, "", 0, 0, start_line}; }
        return Token{Tok::LtLt, "", 0, 0, start_line};
      }
      return two('=', Tok::LtEq, Tok::Lt);
    case '>':
      if (peek() == '>') {
        advance();
        if (peek() == '=') { advance(); return Token{Tok::GtGtEq, "", 0, 0, start_line}; }
        return Token{Tok::GtGt, "", 0, 0, start_line};
      }
      return two('=', Tok::GtEq, Tok::Gt);
    case '!':
      if (peek() == '=') { advance(); return Token{Tok::BangEq, "", 0, 0, start_line}; }
      return Token{Tok::Not, "", 0, 0, start_line};
    default: break;
  }
  return Token{Tok::Error, std::string("unexpected character '") + c + "'", 0, 0, start_line};
}

std::vector<Token> lex_all(const std::string& source) {
  Lexer lx(source);
  std::vector<Token> out;
  for (;;) {
    Token t = lx.next();
    const bool done = (t.kind == Tok::End || t.kind == Tok::Error);
    out.push_back(std::move(t));
    if (done) break;
  }
  return out;
}

}  // namespace aios::userland::grain
