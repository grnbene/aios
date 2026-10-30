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
  {"entity", Tok::Entity}, {"on", Tok::On}, {"match", Tok::Match},
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
    case Tok::ColonColon: return "::";
    case Tok::Question: return "?";
    case Tok::QuestionDot: return "?.";
    case Tok::Entity: return "entity"; case Tok::On: return "on";
    case Tok::Match: return "match"; case Tok::QuestionQuestion: return "??"; case Tok::FatArrow: return "=>";
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
  if (c == '\n') {
    line_++;
    line_start_pos_ = pos_;
  }
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
    if (c == '/' && peek(1) == '/') { while (peek() && peek() != '\n') advance(); continue; }
    return;
  }
}

static int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// A string literal, with the escapes a program actually needs. An unknown
// escape is an error rather than the character itself: "\d" is far more often
// a typo than an intent, and silently eating the backslash hides it.
Token Lexer::string_token(bool is_fstr) {
  const int start_line = line_;
  const int start_col = static_cast<int>(pos_ >= line_start_pos_ ? pos_ - line_start_pos_ + 1 : 1);
  const char quote = advance();
  std::string out;
  int brace_depth = 0;
  char inner_quote = 0;
  bool inner_quote_escaped = false;
  bool has_interp = is_fstr;

  for (;;) {
    char c = peek();
    if (c == '\0' || c == '\n') return Token{Tok::Error, "unterminated string", 0, 0, start_line, start_col};

    // If outside an interpolation expression, the matching quote terminates the string.
    if (brace_depth == 0 && c == quote) {
      advance();
      break;
    }

    // Inside an interpolation expression (brace_depth > 0)
    if (brace_depth > 0) {
      if (inner_quote) {
        if (c == '\\') {
          advance();
          char e = peek();
          if (e == '\0' || e == '\n') return Token{Tok::Error, "unterminated string", 0, 0, start_line, start_col};
          advance();
          if (inner_quote_escaped && e == inner_quote) {
            inner_quote = 0;
            inner_quote_escaped = false;
            out.push_back(e);
          } else {
            out.push_back('\\');
            out.push_back(e);
          }
          continue;
        }
        advance();
        if (!inner_quote_escaped && c == inner_quote) {
          inner_quote = 0;
        }
        out.push_back(c);
        continue;
      }

      // Not currently inside an inner string:
      if (c == '\\') {
        advance();
        char e = peek();
        if (e == '\0' || e == '\n') return Token{Tok::Error, "unterminated string", 0, 0, start_line, start_col};
        advance();
        if (e == '"' || e == '\'') {
          inner_quote = e;
          inner_quote_escaped = true;
          out.push_back(e);
        } else {
          out.push_back('\\');
          out.push_back(e);
        }
        continue;
      }

      if (c == '"' || c == '\'') {
        advance();
        inner_quote = c;
        inner_quote_escaped = false;
        out.push_back(c);
        continue;
      }

      if (c == '{') {
        advance();
        brace_depth++;
        out.push_back('{');
        continue;
      }

      if (c == '}') {
        advance();
        brace_depth--;
        out.push_back('}');
        continue;
      }

      advance();
      out.push_back(c);
      continue;
    }

    // brace_depth == 0 (string literal text):
    // ${expr} interpolates; a ${ that no expression can follow ("${:.2f}": a price's dollar sign before a format()
    // placeholder, "${}") is text, as written.
    if (c == '$' && peek(1) == '{' && peek(2) != ':' && peek(2) != '}' && peek(2) != quote && peek(2) != '\0') {
      advance(); // '$'
      advance(); // '{'
      out += "${";
      brace_depth = 1;
      has_interp = true;
      continue;
    }

    if (is_fstr && c == '{') {
      advance();
      if (peek() == '{') {
        advance();
        out += "{{";
      } else {
        out.push_back('{');
        brace_depth = 1;
      }
      continue;
    }

    if (is_fstr && c == '}') {
      advance();
      if (peek() == '}') {
        advance();
        out += "}}";
      } else {
        out.push_back('}');
      }
      continue;
    }

    // In a plain string only ${expr} interpolates: every other brace is text, written doubled so that if the string
    // turns out to interpolate ("${x} costs ${:.2f}") the f-string reader takes it as text; if it does not, the
    // doubling is undone below and the text is exactly as written.
    if (!is_fstr && (c == '{' || c == '}')) {
      advance();
      out.push_back(c);
      out.push_back(c);
      continue;
    }

    advance();
    if (c != '\\') { out.push_back(c); continue; }
    char e = peek();
    if (e == '\0') return Token{Tok::Error, "unterminated string", 0, 0, start_line, start_col};
    advance();
    switch (e) {
      case 'n': out.push_back('\n'); break;
      case 't': out.push_back('\t'); break;
      case 'r': out.push_back('\r'); break;
      case '0': out.push_back('\0'); break;
      case 'a': out.push_back('\a'); break;
      case 'b': out.push_back('\b'); break;
      case 'f': out.push_back('\f'); break;
      case 'v': out.push_back('\v'); break;
      case 'e': out.push_back('\x1b'); break;
      // \xHH is one byte; \u{H..} is one code point, written as UTF-8.
      case 'x': {
        int v = 0;
        for (int k = 0; k < 2; ++k) {
          const int h = hex_digit(peek());
          if (h < 0) return Token{Tok::Error, "\\x wants two hex digits", 0, 0, start_line, start_col};
          v = v * 16 + h;
          advance();
        }
        out.push_back((char)v);
        break;
      }
      case 'u': {
        uint32_t cp = 0;
        if (peek() == '{') {
          advance();
          int n = 0;
          while (hex_digit(peek()) >= 0 && n < 8) { cp = cp * 16 + (uint32_t)hex_digit(peek()); advance(); ++n; }
          if (peek() != '}' || n == 0) return Token{Tok::Error, "\\u{...} wants 1 to 6 hex digits and a closing brace", 0, 0, start_line, start_col};
          advance();
        } else {
          // \uXXXX, four hex digits, as JSON and JavaScript write it (a string pasted from either compiled nowhere
          // before); a surrogate pair 😀 is one code point.
          auto four = [&](uint32_t* v) {
            *v = 0;
            for (int k = 0; k < 4; ++k) {
              const int h = hex_digit(peek());
              if (h < 0) return false;
              *v = *v * 16 + (uint32_t)h;
              advance();
            }
            return true;
          };
          if (!four(&cp)) return Token{Tok::Error, "\\u wants four hex digits (\\u00B7) or a code point in braces (\\u{1F600})", 0, 0, start_line, start_col};
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            uint32_t lo = 0;
            if (peek() != '\\' || peek(1) != 'u') return Token{Tok::Error, "\\uD800-\\uDBFF is half a surrogate pair: the low half must follow", 0, 0, start_line, start_col};
            advance(); advance();
            if (!four(&lo) || lo < 0xDC00 || lo > 0xDFFF) return Token{Tok::Error, "\\uD800-\\uDBFF must be followed by \\uDC00-\\uDFFF", 0, 0, start_line, start_col};
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
        }
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return Token{Tok::Error, "\\u escape is not a Unicode scalar value", 0, 0, start_line, start_col};
        if (cp < 0x80) out.push_back((char)cp);
        else if (cp < 0x800) { out.push_back((char)(0xC0 | (cp >> 6))); out.push_back((char)(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) { out.push_back((char)(0xE0 | (cp >> 12))); out.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); out.push_back((char)(0x80 | (cp & 0x3F))); }
        else { out.push_back((char)(0xF0 | (cp >> 18))); out.push_back((char)(0x80 | ((cp >> 12) & 0x3F))); out.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); out.push_back((char)(0x80 | (cp & 0x3F))); }
        break;
      }
      case '\\': out.push_back('\\'); break;
      case '"': out.push_back('"'); break;
      case '\'': out.push_back('\''); break;
      case '$': out.push_back('$'); break;
      // In an f-string \{ and \} are literal braces - kept doubled so the
      // interpolation parser can tell them from a {expression}. Elsewhere
      // they are the error any other unknown escape is.
      case '{': if (!is_fstr) return Token{Tok::Error, "unknown escape \\{ (only f-strings have braces to escape)", 0, 0, start_line, start_col}; out += "{{"; break;
      case '}': if (!is_fstr) return Token{Tok::Error, "unknown escape \\} (only f-strings have braces to escape)", 0, 0, start_line, start_col}; out += "}}"; break;
      default:
        return Token{Tok::Error, std::string("unknown escape \\") + e, 0, 0, start_line, start_col};
    }
  }
  if (!is_fstr && !has_interp) {   // no ${expr}: every brace was doubled above; as written again
    std::string plain;
    plain.reserve(out.size());
    for (size_t k = 0; k < out.size(); ++k) {
      plain.push_back(out[k]);
      if ((out[k] == '{' || out[k] == '}') && k + 1 < out.size() && out[k + 1] == out[k]) ++k;
    }
    out = std::move(plain);
  }
  return Token{has_interp ? Tok::FStr : Tok::Str, out, 0, 0, start_line, start_col};
}

// An integer, unless it has a point or an exponent. With a point and no exponent (19.99) it is an exact decimal, as
// written; with an exponent (1e3, 6.02e23) a real - scientific notation is for scientific numbers. An integer too big
// for 64 bits is an exact decimal too. (Both come out as Tok::Real with `exact` set and the digits in `text`.)
Token Lexer::number_token() {
  const int start_line = line_;
  const size_t start = pos_;
  const int start_col = static_cast<int>(start >= line_start_pos_ ? start - line_start_pos_ + 1 : 1);
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
    if (digits.empty()) return Token{Tok::Error, text + " has no digits", 0, 0, start_line, start_col};
    if (is_alpha(peek()) || is_digit(peek()))
      return Token{Tok::Error, std::string("'") + peek() + "' is not a " + (hex ? "hex" : "binary") + " digit", 0, 0, start_line, start_col};
    size_t first = digits.find_first_not_of('0');
    const size_t sig = first == std::string::npos ? 0 : digits.size() - first;
    if (sig > (hex ? 16u : 64u)) return Token{Tok::Error, text + " does not fit in 64 bits", 0, 0, start_line, start_col};
    Token t;
    t.line = start_line;
    t.column = start_col;
    t.text = text;
    t.kind = Tok::Int;
    t.integer = (int64_t)std::strtoull(digits.c_str(), nullptr, hex ? 16 : 2);
    return t;
  }
  bool real = false, exponent = false;
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
      exponent = true;
      advance();
      if (sign == '+' || sign == '-') advance();
      while (is_digit(peek())) advance();
    }
  }
  const std::string text = src_.substr(start, pos_ - start);
  Token t;
  t.line = start_line;
  t.column = start_col;
  t.text = text;
  if (real) {
    t.kind = Tok::Real;
    t.real = std::strtod(text.c_str(), nullptr);
    t.exact = !exponent;
  } else {
    t.kind = Tok::Int;
    errno = 0;
    t.integer = (int64_t)std::strtoll(text.c_str(), nullptr, 10);
    // Too large for 64 bits: the exact decimal (never a silently wrapped negative, nor a rounded real).
    if (errno == ERANGE) { t.kind = Tok::Real; t.real = std::strtod(text.c_str(), nullptr); t.exact = true; }
  }
  return t;
}

Token Lexer::next() {
  skip_run();
  if (pos_ >= src_.size()) {
    int col = static_cast<int>(pos_ >= line_start_pos_ ? pos_ - line_start_pos_ + 1 : 1);
    return Token{Tok::End, "", 0, 0, line_, col};
  }

  const size_t start_pos = pos_;
  const int start_line = line_;
  const int start_col = static_cast<int>(start_pos >= line_start_pos_ ? start_pos - line_start_pos_ + 1 : 1);

  const char c = peek();
  if (c == '\n') { advance(); return Token{Tok::Newline, "", 0, 0, line_ - 1, start_col}; }
  if (c == '"' || c == '\'') return string_token(false);
  if (is_digit(c)) return number_token();

  if ((c == 'f' || c == 'F' || c == '$') && (peek(1) == '"' || peek(1) == '\'')) {
    advance();
    return string_token(true);
  }

  if (is_alpha(c)) {
    while (is_alnum(peek())) advance();
    const std::string word = src_.substr(start_pos, pos_ - start_pos);
    for (const auto& k : KEYWORDS)
      if (word == k.text) return Token{k.kind, word, 0, 0, start_line, start_col};
    return Token{Tok::Ident, word, 0, 0, start_line, start_col};
  }

  advance();
  auto two = [&](char want, Tok both, Tok single) {
    if (peek() == want) { advance(); return Token{both, "", 0, 0, start_line, start_col}; }
    return Token{single, "", 0, 0, start_line, start_col};
  };
  switch (c) {
    case '+': return two('=', Tok::PlusEq, Tok::Plus);
    case '-': return two('=', Tok::MinusEq, Tok::Minus);
    case '*': return two('=', Tok::StarEq, Tok::Star);
    case '/': return two('=', Tok::SlashEq, Tok::Slash);
    case '%': return two('=', Tok::PercentEq, Tok::Percent);
    case '^': return two('=', Tok::CaretEq, Tok::Caret);
    case '~': return Token{Tok::Tilde, "", 0, 0, start_line, start_col};
    case '&':
      if (peek() == '&') { advance(); return Token{Tok::And, "", 0, 0, start_line, start_col}; }
      if (peek() == '=') { advance(); return Token{Tok::AmpEq, "", 0, 0, start_line, start_col}; }
      return Token{Tok::Amp, "", 0, 0, start_line, start_col};
    case '|':
      if (peek() == '|') { advance(); return Token{Tok::Or, "", 0, 0, start_line, start_col}; }
      if (peek() == '=') { advance(); return Token{Tok::PipeEq, "", 0, 0, start_line, start_col}; }
      return Token{Tok::Pipe, "", 0, 0, start_line, start_col};
    case '(': return Token{Tok::LParen, "", 0, 0, start_line, start_col};
    case ')': return Token{Tok::RParen, "", 0, 0, start_line, start_col};
    case '{': return Token{Tok::LBrace, "", 0, 0, start_line, start_col};
    case '}': return Token{Tok::RBrace, "", 0, 0, start_line, start_col};
    case '[': return Token{Tok::LBracket, "", 0, 0, start_line, start_col};
    case ']': return Token{Tok::RBracket, "", 0, 0, start_line, start_col};
    case ',': return Token{Tok::Comma, "", 0, 0, start_line, start_col};
    case '.': return Token{Tok::Dot, "", 0, 0, start_line, start_col};
    case ':':
      if (peek() == ':') { advance(); return Token{Tok::ColonColon, "::", 0, 0, start_line, start_col}; }
      return Token{Tok::Colon, "", 0, 0, start_line, start_col};
    case '?':
      if (peek() == '.') { advance(); return Token{Tok::QuestionDot, "?.", 0, 0, start_line, start_col}; }
      if (peek() == '?') { advance(); return Token{Tok::QuestionQuestion, "??", 0, 0, start_line, start_col}; }
      return Token{Tok::Question, "", 0, 0, start_line, start_col};
    case ';': return Token{Tok::Newline, "", 0, 0, start_line, start_col};
    case '=':
      if (peek() == '>') { advance(); return Token{Tok::FatArrow, "=>", 0, 0, start_line, start_col}; }
      return two('=', Tok::EqEq, Tok::Assign);
    case '<':
      if (peek() == '<') {
        advance();
        if (peek() == '=') { advance(); return Token{Tok::LtLtEq, "", 0, 0, start_line, start_col}; }
        return Token{Tok::LtLt, "", 0, 0, start_line, start_col};
      }
      return two('=', Tok::LtEq, Tok::Lt);
    case '>':
      if (peek() == '>') {
        advance();
        if (peek() == '=') { advance(); return Token{Tok::GtGtEq, "", 0, 0, start_line, start_col}; }
        return Token{Tok::GtGt, "", 0, 0, start_line, start_col};
      }
      return two('=', Tok::GtEq, Tok::Gt);
    case '!':
      if (peek() == '=') { advance(); return Token{Tok::BangEq, "", 0, 0, start_line, start_col}; }
      return Token{Tok::Not, "", 0, 0, start_line, start_col};
    default: break;
  }
  return Token{Tok::Error, std::string("unexpected character '") + c + "'", 0, 0, start_line, start_col};
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
