// The JavaScript tokeniser.
#include "lexer.hpp"
#include "aios/hal/host.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace aios::js {

namespace {

const std::unordered_map<std::u16string, Kw>& keywords() {
  static const std::unordered_map<std::u16string, Kw> m = {
      {u"break", Kw::Break},     {u"case", Kw::Case},       {u"catch", Kw::Catch},     {u"class", Kw::Class},
      {u"const", Kw::Const},     {u"continue", Kw::Continue}, {u"debugger", Kw::Debugger}, {u"default", Kw::Default},
      {u"delete", Kw::Delete},   {u"do", Kw::Do},           {u"else", Kw::Else},       {u"enum", Kw::Enum},
      {u"export", Kw::Export},   {u"extends", Kw::Extends}, {u"false", Kw::False},     {u"finally", Kw::Finally},
      {u"for", Kw::For},         {u"function", Kw::Function}, {u"if", Kw::If},         {u"import", Kw::Import},
      {u"in", Kw::In},           {u"instanceof", Kw::InstanceOf}, {u"new", Kw::New},   {u"null", Kw::Null},
      {u"return", Kw::Return},   {u"super", Kw::Super},     {u"switch", Kw::Switch},   {u"this", Kw::This},
      {u"throw", Kw::Throw},     {u"true", Kw::True},       {u"try", Kw::Try},         {u"typeof", Kw::TypeOf},
      {u"var", Kw::Var},         {u"void", Kw::Void},       {u"while", Kw::While},     {u"with", Kw::With},
  };
  return m;
}

inline int hexval(char16_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

void append_cp(std::u16string& out, uint32_t cp) {
  if (cp >= 0x10000) {
    cp -= 0x10000;
    out += static_cast<char16_t>(0xD800 + (cp >> 10));
    out += static_cast<char16_t>(0xDC00 + (cp & 0x3FF));
  } else {
    out += static_cast<char16_t>(cp);
  }
}

}  // namespace

bool Lexer::is_space(char16_t c) {
  switch (c) {
    case 0x09: case 0x0B: case 0x0C: case 0x20: case 0xA0: case 0xFEFF: case 0x1680: case 0x202F: case 0x205F: case 0x3000:
      return true;
    default:
      return c >= 0x2000 && c <= 0x200A;
  }
}

bool Lexer::is_id_start(uint32_t c) {
  if (c < 0x80) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '$' || c == '_';
  if (c == 0xD7 || c == 0xF7 || c < 0xAA) return false;
  if (c == 0xAA || c == 0xB5 || c == 0xBA) return true;
  if (c < 0xC0) return false;
  if (is_space(static_cast<char16_t>(c < 0x10000 ? c : 0)) && c < 0x10000) return false;
  if (c == 0x2028 || c == 0x2029) return false;
  if (c >= 0x2000 && c <= 0x2BFF && !(c >= 0x2071 && c <= 0x2071) && !(c >= 0x207F && c <= 0x207F) && !(c >= 0x2090 && c <= 0x209C) &&
      !(c >= 0x2102 && c <= 0x2188))
    return false;
  if (c >= 0x2E00 && c <= 0x2E7F) return false;
  if (c >= 0x3000 && c <= 0x3004) return false;
  if (c >= 0x3008 && c <= 0x3020) return false;
  if (c >= 0xFE30 && c <= 0xFE4F) return false;
  if (c >= 0xFF00 && c <= 0xFF0F) return false;
  if (c >= 0xFF1A && c <= 0xFF20) return false;
  if (c >= 0xD800 && c <= 0xDFFF) return false;
  return true;
}

bool Lexer::is_id_part(uint32_t c) {
  if (is_id_start(c)) return true;
  if (c < 0x80) return c >= '0' && c <= '9';
  if (c == 0x200C || c == 0x200D) return true;           // ZWNJ / ZWJ
  if (c >= 0x0300 && c <= 0x036F) return true;           // combining marks
  if (c >= 0x0660 && c <= 0x0669) return true;           // digits of other scripts (a small, common subset)
  if (c >= 0x06F0 && c <= 0x06F9) return true;
  if (c >= 0x0966 && c <= 0x096F) return true;
  if (c >= 0xFF10 && c <= 0xFF19) return true;
  if (c == 0xB7 || c == 0x203F || c == 0x2040) return true;
  return false;
}

void Lexer::newline_at(uint32_t i) {
  ++line_;
  line_start_ = i + 1;
}

bool Lexer::skip_trivia() {
  bool nl = false;
  // a hashbang line, only at the very start
  if (pos_ == 0 && at(0) == '#' && at(1) == '!') {
    while (pos_ < len_ && !is_line_terminator(src_[pos_])) ++pos_;
  }
  for (;;) {
    if (pos_ >= len_) return nl;
    char16_t c = src_[pos_];
    if (c == 0x0A || c == 0x2028 || c == 0x2029) { newline_at(pos_); ++pos_; nl = true; continue; }
    if (c == 0x0D) {
      if (at(pos_ + 1) == 0x0A) ++pos_;
      newline_at(pos_);
      ++pos_;
      nl = true;
      continue;
    }
    if (is_space(c)) { ++pos_; continue; }
    if (c == '/' && at(pos_ + 1) == '/') {
      while (pos_ < len_ && !is_line_terminator(src_[pos_])) ++pos_;
      continue;
    }
    if (c == '/' && at(pos_ + 1) == '*') {
      pos_ += 2;
      bool closed = false;
      while (pos_ < len_) {
        char16_t d = src_[pos_];
        if (d == '*' && at(pos_ + 1) == '/') { pos_ += 2; closed = true; break; }
        if (d == 0x0A || d == 0x2028 || d == 0x2029) { newline_at(pos_); nl = true; }
        else if (d == 0x0D) { if (at(pos_ + 1) == 0x0A) ++pos_; newline_at(pos_); nl = true; }
        ++pos_;
      }
      (void)closed;
      continue;
    }
    // HTML-like comments (Annex B): `<!--` anywhere, `-->` at the start of a line
    if (c == '<' && at(pos_ + 1) == '!' && at(pos_ + 2) == '-' && at(pos_ + 3) == '-') {
      while (pos_ < len_ && !is_line_terminator(src_[pos_])) ++pos_;
      continue;
    }
    if (c == '-' && at(pos_ + 1) == '-' && at(pos_ + 2) == '>' && (nl || pos_ == 0)) {
      while (pos_ < len_ && !is_line_terminator(src_[pos_])) ++pos_;
      continue;
    }
    return nl;
  }
}

void Lexer::read_identifier(Token& t, bool is_private) {
  std::u16string name;
  bool first = true;
  while (pos_ < len_) {
    uint32_t cp = src_[pos_];
    uint32_t adv = 1;
    if (cp >= 0xD800 && cp < 0xDC00 && pos_ + 1 < len_ && src_[pos_ + 1] >= 0xDC00 && src_[pos_ + 1] < 0xE000) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (src_[pos_ + 1] - 0xDC00);
      adv = 2;
    }
    if (cp == '\\') {
      // \uXXXX or \u{X...}
      if (at(pos_ + 1) != 'u') { t.error = "Invalid or unexpected token"; return; }
      uint32_t v = 0;
      uint32_t p = pos_ + 2;
      if (at(p) == '{') {
        ++p;
        int n = 0;
        while (hexval(at(p)) >= 0) { v = v * 16 + static_cast<uint32_t>(hexval(at(p))); ++p; ++n; if (v > 0x10FFFF) break; }
        if (at(p) != '}' || n == 0 || v > 0x10FFFF) { t.error = "Invalid Unicode escape sequence"; return; }
        ++p;
      } else {
        for (int i = 0; i < 4; ++i) {
          int h = hexval(at(p));
          if (h < 0) { t.error = "Invalid Unicode escape sequence"; return; }
          v = v * 16 + static_cast<uint32_t>(h);
          ++p;
        }
      }
      if (!(first ? is_id_start(v) : is_id_part(v))) { t.error = "Invalid Unicode escape sequence"; return; }
      append_cp(name, v);
      pos_ = p;
      t.escaped = true;
      first = false;
      continue;
    }
    if (!(first ? is_id_start(cp) : is_id_part(cp))) break;
    append_cp(name, cp);
    pos_ += adv;
    first = false;
  }
  if (name.empty()) { t.error = "Invalid or unexpected token"; return; }
  t.str = std::move(name);
  if (is_private) { t.type = TT::Private; return; }
  t.type = TT::Ident;
  auto it = keywords().find(t.str);
  if (it != keywords().end()) { t.type = TT::Keyword; t.kw = it->second; }
}

void Lexer::read_number(Token& t) {
  t.type = TT::Number;
  const uint32_t start = pos_;
  auto digit_run = [&](int base, std::string& out, bool* any) -> bool {
    bool last_sep = true;
    *any = false;
    while (pos_ < len_) {
      char16_t c = src_[pos_];
      if (c == '_') {
        if (last_sep) { t.error = "Numeric separators are not allowed here"; return false; }
        last_sep = true;
        ++pos_;
        continue;
      }
      int h = hexval(c);
      if (h < 0 || h >= base) break;
      out += static_cast<char>(c);
      *any = true;
      last_sep = false;
      ++pos_;
    }
    if (last_sep && *any) { t.error = "Numeric separators are not allowed at the end of numeric literals"; return false; }
    return true;
  };
  char16_t c0 = src_[pos_];
  if (c0 == '0' && (at(pos_ + 1) == 'x' || at(pos_ + 1) == 'X' || at(pos_ + 1) == 'o' || at(pos_ + 1) == 'O' || at(pos_ + 1) == 'b' || at(pos_ + 1) == 'B')) {
    int base = (at(pos_ + 1) | 0x20) == 'x' ? 16 : (at(pos_ + 1) | 0x20) == 'o' ? 8 : 2;
    pos_ += 2;
    std::string digits;
    bool any;
    if (!digit_run(base, digits, &any)) return;
    if (!any) { t.error = "Invalid or unexpected token"; return; }
    double v = 0;
    for (char d : digits) v = v * base + hexval(static_cast<char16_t>(d));
    t.num = v;
    if (at(pos_) == 'n') {
      t.bigint = true;
      ++pos_;
      t.str = std::u16string(1, u'0') + static_cast<char16_t>(at(start + 1) | 0x20);
      for (char d : digits) t.str += static_cast<char16_t>(d);
    }
  } else if (c0 == '0' && at(pos_ + 1) >= '0' && at(pos_ + 1) <= '9') {
    // legacy octal (0777) or a decimal with a leading zero (089)
    t.legacy_octal = true;
    uint32_t p = pos_ + 1;
    bool octal = true;
    while (at(p) >= '0' && at(p) <= '9') { if (at(p) >= '8') octal = false; ++p; }
    if (octal) {
      double v = 0;
      for (uint32_t i = pos_ + 1; i < p; ++i) v = v * 8 + (src_[i] - '0');
      t.num = v;
      pos_ = p;
    } else {
      std::string s;
      while (pos_ < len_ && ((src_[pos_] >= '0' && src_[pos_] <= '9') || src_[pos_] == '.')) s += static_cast<char>(src_[pos_++]);
      if ((at(pos_) | 0x20) == 'e') {
        s += static_cast<char>(src_[pos_++]);
        if (at(pos_) == '+' || at(pos_) == '-') s += static_cast<char>(src_[pos_++]);
        while (at(pos_) >= '0' && at(pos_) <= '9') s += static_cast<char>(src_[pos_++]);
      }
      t.num = aios::hal::parse_double(s.c_str(), nullptr);
    }
    if (strict) t.error = "Octal literals are not allowed in strict mode.";
  } else {
    std::string s;
    bool any_int = false, any_frac = false;
    if (c0 != '.') {
      if (!digit_run(10, s, &any_int)) return;
    }
    if (at(pos_) == '.') {
      s += '.';
      ++pos_;
      if (!digit_run(10, s, &any_frac)) return;
    }
    if (!any_int && !any_frac) { t.error = "Invalid or unexpected token"; return; }
    if ((at(pos_) | 0x20) == 'e') {
      uint32_t save = pos_;
      std::string e = "e";
      ++pos_;
      if (at(pos_) == '+' || at(pos_) == '-') e += static_cast<char>(src_[pos_++]);
      bool any;
      std::string ds;
      if (!digit_run(10, ds, &any)) return;
      if (!any) { pos_ = save; t.error = "Invalid or unexpected token"; return; }
      s += e + ds;
    }
    if (at(pos_) == 'n' && !any_frac && s.find('e') == std::string::npos && s.find('.') == std::string::npos && any_int) {
      t.bigint = true;
      ++pos_;
      for (char d : s) t.str += static_cast<char16_t>(d);
    }
    t.num = aios::hal::parse_double(s.c_str(), nullptr);
  }
  // A number may not run straight into an identifier or another digit.
  uint32_t nx = at(pos_);
  if (is_id_start(nx) || (nx >= '0' && nx <= '9') || nx == '\\') {
    if (t.error.empty()) t.error = "Invalid or unexpected token";
  }
  (void)start;
}

bool Lexer::read_escape(std::u16string& out, bool in_template, bool* octal) {
  // pos_ is at the character after the backslash
  char16_t c = at(pos_);
  switch (c) {
    case 'n': out += u'\n'; ++pos_; return true;
    case 't': out += u'\t'; ++pos_; return true;
    case 'r': out += u'\r'; ++pos_; return true;
    case 'b': out += u'\b'; ++pos_; return true;
    case 'f': out += u'\f'; ++pos_; return true;
    case 'v': out += u'\v'; ++pos_; return true;
    case 0x0D:
      ++pos_;
      if (at(pos_) == 0x0A) ++pos_;
      newline_at(pos_ - 1);
      return true;   // line continuation
    case 0x0A: case 0x2028: case 0x2029:
      newline_at(pos_);
      ++pos_;
      return true;
    case 'x': {
      int a = hexval(at(pos_ + 1)), b = hexval(at(pos_ + 2));
      if (a < 0 || b < 0) return false;
      out += static_cast<char16_t>(a * 16 + b);
      pos_ += 3;
      return true;
    }
    case 'u': {
      uint32_t v = 0;
      uint32_t p = pos_ + 1;
      if (at(p) == '{') {
        ++p;
        int n = 0;
        while (hexval(at(p)) >= 0) { v = v * 16 + static_cast<uint32_t>(hexval(at(p))); ++p; ++n; if (v > 0x10FFFF) return false; }
        if (at(p) != '}' || n == 0) return false;
        ++p;
      } else {
        for (int i = 0; i < 4; ++i) {
          int h = hexval(at(p));
          if (h < 0) return false;
          v = v * 16 + static_cast<uint32_t>(h);
          ++p;
        }
      }
      append_cp(out, v);
      pos_ = p;
      return true;
    }
    default:
      break;
  }
  if (c >= '0' && c <= '9') {
    if (c == '0' && !(at(pos_ + 1) >= '0' && at(pos_ + 1) <= '9')) { out += u'\0'; ++pos_; return true; }
    if (in_template) return false;   // octal escapes are not allowed in templates
    if (c >= '8') { out += c; ++pos_; *octal = true; return true; }
    // legacy octal escape: up to three digits, value <= 255
    uint32_t v = static_cast<uint32_t>(c - '0');
    ++pos_;
    if (at(pos_) >= '0' && at(pos_) <= '7') {
      v = v * 8 + static_cast<uint32_t>(at(pos_) - '0');
      ++pos_;
      if (c <= '3' && at(pos_) >= '0' && at(pos_) <= '7') { v = v * 8 + static_cast<uint32_t>(at(pos_) - '0'); ++pos_; }
    }
    out += static_cast<char16_t>(v);
    *octal = true;
    return true;
  }
  // a surrogate pair written literally after the backslash stays whole
  if (c >= 0xD800 && c < 0xDC00 && at(pos_ + 1) >= 0xDC00 && at(pos_ + 1) < 0xE000) { out += c; out += at(pos_ + 1); pos_ += 2; return true; }
  out += c;
  ++pos_;
  return true;
}

void Lexer::read_string(Token& t, char16_t quote) {
  t.type = TT::String;
  ++pos_;   // opening quote
  std::u16string s;
  for (;;) {
    if (pos_ >= len_) { t.error = "Invalid or unexpected token"; return; }
    char16_t c = src_[pos_];
    if (c == quote) { ++pos_; break; }
    if (c == '\\') {
      ++pos_;
      bool octal = false;
      if (!read_escape(s, false, &octal)) { t.error = "Invalid escape sequence"; return; }
      if (octal) { t.legacy_octal = true; if (strict) t.error = "Octal escape sequences are not allowed in strict mode."; }
      continue;
    }
    if (c == 0x0A || c == 0x0D) { t.error = "Invalid or unexpected token"; return; }
    s += c;
    ++pos_;
  }
  t.str = std::move(s);
}

void Lexer::read_template_chunk(Token& t) {
  // pos_ is just after ` or }
  t.type = TT::Template;
  std::u16string cooked, raw;
  bool ok = true;
  for (;;) {
    if (pos_ >= len_) { t.error = "Unterminated template literal"; return; }
    char16_t c = src_[pos_];
    if (c == '`') { ++pos_; t.template_tail = true; break; }
    if (c == '$' && at(pos_ + 1) == '{') { pos_ += 2; t.template_tail = false; break; }
    if (c == '\\') {
      uint32_t esc_start = pos_;
      ++pos_;
      bool octal = false;
      std::u16string tmp;
      uint32_t before = pos_;
      if (!read_escape(tmp, true, &octal)) {
        ok = false;
        // skip a character so the scan continues
        if (pos_ == before) ++pos_;
      } else {
        cooked += tmp;
      }
      // raw text: the source between the backslash and here, line terminators normalised
      for (uint32_t i = esc_start; i < pos_; ++i) {
        if (src_[i] == 0x0D) { raw += u'\n'; if (at(i + 1) == 0x0A) ++i; }
        else raw += src_[i];
      }
      continue;
    }
    if (c == 0x0D) {
      if (at(pos_ + 1) == 0x0A) ++pos_;
      newline_at(pos_);
      ++pos_;
      cooked += u'\n';
      raw += u'\n';
      continue;
    }
    if (c == 0x0A || c == 0x2028 || c == 0x2029) { newline_at(pos_); }
    cooked += c;
    raw += c;
    ++pos_;
  }
  t.str = std::move(cooked);
  t.str2 = std::move(raw);
  t.template_cooked_ok = ok;
}

Token Lexer::next() {
  Token t;
  t.nl_before = skip_trivia();
  t.start = pos_;
  t.line = line_;
  if (pos_ >= len_) { t.type = TT::End; t.end = pos_; return t; }
  const char16_t c = src_[pos_];
  uint32_t cp = c;
  if (c >= 0xD800 && c < 0xDC00 && pos_ + 1 < len_ && src_[pos_ + 1] >= 0xDC00 && src_[pos_ + 1] < 0xE000) cp = 0x10000 + ((c - 0xD800u) << 10) + (src_[pos_ + 1] - 0xDC00u);
  if (is_id_start(cp) || c == '\\') { read_identifier(t, false); t.end = pos_; return t; }
  if ((c >= '0' && c <= '9') || (c == '.' && at(pos_ + 1) >= '0' && at(pos_ + 1) <= '9')) { read_number(t); t.end = pos_; return t; }
  if (c == '"' || c == '\'') { read_string(t, c); t.end = pos_; return t; }
  if (c == '`') { ++pos_; read_template_chunk(t); t.end = pos_; return t; }
  if (c == '#') {
    ++pos_;
    read_identifier(t, true);
    t.end = pos_;
    return t;
  }
  t.type = TT::Punct;
  auto set = [&](P p, uint32_t n) { t.punct = p; pos_ += n; };
  const char16_t n1 = at(pos_ + 1), n2 = at(pos_ + 2), n3 = at(pos_ + 3);
  switch (c) {
    case '{': set(P::LBrace, 1); break;
    case '}': set(P::RBrace, 1); break;
    case '(': set(P::LParen, 1); break;
    case ')': set(P::RParen, 1); break;
    case '[': set(P::LBracket, 1); break;
    case ']': set(P::RBracket, 1); break;
    case ';': set(P::Semi, 1); break;
    case ',': set(P::Comma, 1); break;
    case ':': set(P::Colon, 1); break;
    case '~': set(P::Tilde, 1); break;
    case '.':
      if (n1 == '.' && n2 == '.') set(P::Ellipsis, 3); else set(P::Dot, 1);
      break;
    case '?':
      if (n1 == '?') { if (n2 == '=') set(P::QQAssign, 3); else set(P::QQ, 2); }
      else if (n1 == '.' && !(n2 >= '0' && n2 <= '9')) set(P::QDot, 2);
      else set(P::Question, 1);
      break;
    case '<':
      if (n1 == '<') { if (n2 == '=') set(P::ShlAssign, 3); else set(P::Shl, 2); }
      else if (n1 == '=') set(P::Le, 2);
      else set(P::Lt, 1);
      break;
    case '>':
      if (n1 == '>') {
        if (n2 == '>') { if (n3 == '=') set(P::UShrAssign, 4); else set(P::UShr, 3); }
        else if (n2 == '=') set(P::ShrAssign, 3);
        else set(P::Shr, 2);
      } else if (n1 == '=') set(P::Ge, 2);
      else set(P::Gt, 1);
      break;
    case '=':
      if (n1 == '=') { if (n2 == '=') set(P::EqEqEq, 3); else set(P::EqEq, 2); }
      else if (n1 == '>') set(P::Arrow, 2);
      else set(P::Assign, 1);
      break;
    case '!':
      if (n1 == '=') { if (n2 == '=') set(P::NotEqEq, 3); else set(P::NotEq, 2); }
      else set(P::Bang, 1);
      break;
    case '+':
      if (n1 == '+') set(P::PlusPlus, 2); else if (n1 == '=') set(P::PlusAssign, 2); else set(P::Plus, 1);
      break;
    case '-':
      if (n1 == '-') set(P::MinusMinus, 2); else if (n1 == '=') set(P::MinusAssign, 2); else set(P::Minus, 1);
      break;
    case '*':
      if (n1 == '*') { if (n2 == '=') set(P::StarStarAssign, 3); else set(P::StarStar, 2); }
      else if (n1 == '=') set(P::StarAssign, 2);
      else set(P::Star, 1);
      break;
    case '/':
      if (n1 == '=') set(P::SlashAssign, 2); else set(P::Slash, 1);
      break;
    case '%':
      if (n1 == '=') set(P::PercentAssign, 2); else set(P::Percent, 1);
      break;
    case '&':
      if (n1 == '&') { if (n2 == '=') set(P::AmpAmpAssign, 3); else set(P::AmpAmp, 2); }
      else if (n1 == '=') set(P::AmpAssign, 2);
      else set(P::Amp, 1);
      break;
    case '|':
      if (n1 == '|') { if (n2 == '=') set(P::PipePipeAssign, 3); else set(P::PipePipe, 2); }
      else if (n1 == '=') set(P::PipeAssign, 2);
      else set(P::Pipe, 1);
      break;
    case '^':
      if (n1 == '=') set(P::CaretAssign, 2); else set(P::Caret, 1);
      break;
    default:
      t.type = TT::Punct;
      t.punct = P::None;
      ++pos_;
      t.error = "Invalid or unexpected token";
      break;
  }
  t.end = pos_;
  return t;
}

Token Lexer::rescan_regex(const Token& slash) {
  pos_ = slash.start;
  line_ = slash.line;
  Token t;
  t.start = slash.start;
  t.line = slash.line;
  t.nl_before = slash.nl_before;
  t.type = TT::RegExp;
  ++pos_;   // the opening /
  std::u16string body;
  bool in_class = false;
  for (;;) {
    if (pos_ >= len_) { t.error = "Invalid regular expression: missing /"; return t; }
    char16_t c = src_[pos_];
    if (is_line_terminator(c)) { t.error = "Invalid regular expression: missing /"; return t; }
    if (c == '\\') {
      body += c;
      ++pos_;
      if (pos_ >= len_ || is_line_terminator(src_[pos_])) { t.error = "Invalid regular expression: missing /"; return t; }
      body += src_[pos_++];
      continue;
    }
    if (c == '[') in_class = true;
    else if (c == ']') in_class = false;
    else if (c == '/' && !in_class) { ++pos_; break; }
    body += c;
    ++pos_;
  }
  std::u16string flags;
  while (pos_ < len_) {
    uint32_t cp = src_[pos_];
    if (cp == '\\') { t.error = "Invalid regular expression flags"; return t; }
    if (!is_id_part(cp)) break;
    flags += src_[pos_++];
  }
  t.str = std::move(body);
  t.str2 = std::move(flags);
  t.end = pos_;
  return t;
}

Token Lexer::rescan_template_continuation(const Token& rbrace) {
  pos_ = rbrace.start + 1;
  line_ = rbrace.line;
  Token t;
  t.start = rbrace.start;
  t.line = rbrace.line;
  read_template_chunk(t);
  t.end = pos_;
  return t;
}

}  // namespace aios::js
