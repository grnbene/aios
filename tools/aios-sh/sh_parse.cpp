// aios-sh's parser: POSIX shell text to a tree (sh.hpp). Recursive descent over a lexer that knows quoting,
// substitutions and here-documents; reserved words are recognised only where the grammar allows them.
#include "sh.hpp"

#include <cctype>
#include <cstring>

namespace aiossh {

namespace {

bool is_name_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool is_name_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
bool is_meta(char c) { return c == ' ' || c == '\t' || c == '\n' || c == ';' || c == '&' || c == '|' || c == '<' || c == '>' || c == '(' || c == ')'; }

struct Token {
  enum Type { Word, Op, Newline, IoNumber, End } type = End;
  std::string raw;        // as written (for reserved words and assignments)
  aiossh::Word word;      // Word only
  bool plain = false;     // no quotes, no expansions: may be a reserved word
  int line = 0;
};

struct PendingHeredoc { std::shared_ptr<Node> node; size_t index; std::string delim; };

class Parser {
 public:
  explicit Parser(const std::string& text) : s_(text) {
    // CRLF scripts (written on Windows) parse as LF ones.
    std::string t;
    t.reserve(s_.size());
    for (size_t i = 0; i < s_.size(); ++i) if (!(s_[i] == '\r' && i + 1 < s_.size() && s_[i + 1] == '\n')) t += s_[i];
    s_ = t;
  }

  std::shared_ptr<Node> program() {
    skip_newlines();
    auto list = std::make_shared<Node>();
    list->kind = Node::List;
    while (peek().type != Token::End) {
      parse_list_into(*list, /*top=*/true);
      skip_newlines();
      if (peek().type != Token::End) {
        const Token& t = peek();
        fail("unexpected '" + t.raw + "'");
      }
    }
    return list;
  }

  [[noreturn]] void fail(const std::string& m) { throw ParseError{m, line_}; }
  bool hit_end = false;
  bool word_only = false;   // parse_word_text: blanks and operators are part of the word
  Word whole_word() { bool plain = false; return scan_word(false, &plain); }

 private:
  std::string s_;
  size_t p_ = 0;
  int line_ = 1;
  bool have_peek_ = false;
  Token peek_;
  std::vector<PendingHeredoc> heredocs_;

  // ------------------------------------------------------------------ lexing

  char cur() const { return p_ < s_.size() ? s_[p_] : '\0'; }
  char at(size_t k) const { return p_ + k < s_.size() ? s_[p_ + k] : '\0'; }

  const Token& peek() {
    if (!have_peek_) { peek_ = lex(); have_peek_ = true; }
    return peek_;
  }
  Token take() { peek(); have_peek_ = false; return peek_; }

  void read_heredocs() {
    for (auto& h : heredocs_) {
      Redirect& r = h.node->redirects[h.index];
      std::string body;
      for (;;) {
        if (p_ >= s_.size()) { hit_end = true; break; }
        size_t e = s_.find('\n', p_);
        std::string ln = s_.substr(p_, e == std::string::npos ? std::string::npos : e - p_);
        p_ = e == std::string::npos ? s_.size() : e + 1;
        ++line_;
        std::string cmp = ln;
        if (r.strip_tabs) { size_t k = 0; while (k < cmp.size() && cmp[k] == '\t') ++k; cmp = cmp.substr(k); }
        if (cmp == h.delim) break;
        body += (r.strip_tabs ? cmp : ln) + "\n";
      }
      r.heredoc = body;
    }
    heredocs_.clear();
  }

  Token lex() {
    // blanks, comments, line continuations
    for (;;) {
      char c = cur();
      if (c == ' ' || c == '\t') { ++p_; continue; }
      if (c == '\\' && at(1) == '\n') { p_ += 2; ++line_; continue; }
      if (c == '#') { while (p_ < s_.size() && s_[p_] != '\n') ++p_; continue; }
      break;
    }
    Token t;
    t.line = line_;
    if (p_ >= s_.size()) { t.type = Token::End; t.raw = "end of input"; return t; }
    char c = cur();
    if (c == '\n') {
      ++p_; ++line_;
      t.type = Token::Newline; t.raw = "newline";
      if (!heredocs_.empty()) read_heredocs();
      return t;
    }
    // an IO number: digits directly followed by < or >
    if (std::isdigit(static_cast<unsigned char>(c))) {
      size_t k = p_;
      while (k < s_.size() && std::isdigit(static_cast<unsigned char>(s_[k]))) ++k;
      if (k < s_.size() && (s_[k] == '<' || s_[k] == '>')) { t.type = Token::IoNumber; t.raw = s_.substr(p_, k - p_); p_ = k; return t; }
    }
    // <( ... ) and >( ... ): process substitution, a word of its own
    if ((c == '<' || c == '>') && at(1) == '(') {
      p_ += 2;
      t.type = Token::Word;
      WordPart wp; wp.kind = WordPart::ProcSub; wp.text = scan_balanced_parens(); wp.braced = c == '>';
      t.word.parts.push_back(wp);
      t.raw = "<(...)";
      return t;
    }
    static const char* ops[] = {"&&", "||", ";;", "|&", "<<<", "<<-", "<<", ">>", "<&", ">&", "<>", ">|", "&>>", "&>", "&", "|", ";", "(", ")", "<", ">"};
    for (const char* op : ops) {
      size_t n = std::strlen(op);
      if (s_.compare(p_, n, op) == 0) { p_ += n; t.type = Token::Op; t.raw = op; return t; }
    }
    t.type = Token::Word;
    size_t start = p_;
    t.word = scan_word(/*in_dquote=*/false, &t.plain);
    t.raw = s_.substr(start, p_ - start);
    return t;
  }

  // Text up to the matching close of a $( ... ), respecting quotes and nesting. p_ is just past "$(".
  std::string scan_balanced_parens() {
    size_t start = p_;
    int depth = 1;
    while (p_ < s_.size()) {
      char c = s_[p_];
      if (c == '\\') { p_ += 2; continue; }
      if (c == '\'') { size_t e = s_.find('\'', p_ + 1); p_ = e == std::string::npos ? s_.size() : e + 1; continue; }
      if (c == '"') { ++p_; while (p_ < s_.size() && s_[p_] != '"') { if (s_[p_] == '\\') ++p_; ++p_; } ++p_; continue; }
      if (c == '`') { ++p_; while (p_ < s_.size() && s_[p_] != '`') { if (s_[p_] == '\\') ++p_; ++p_; } ++p_; continue; }
      if (c == '#' && (p_ == start || std::isspace(static_cast<unsigned char>(s_[p_ - 1])))) { while (p_ < s_.size() && s_[p_] != '\n') ++p_; continue; }
      if (c == '\n') ++line_;
      if (c == '(') ++depth;
      else if (c == ')') { if (--depth == 0) { std::string body = s_.substr(start, p_ - start); ++p_; return body; } }
      ++p_;
    }
    hit_end = true;
    fail("unterminated $(");
  }

  // ${ ... } body, p_ just past "${".
  std::string scan_brace_param() {
    size_t start = p_;
    int depth = 1;
    while (p_ < s_.size()) {
      char c = s_[p_];
      if (c == '\\') { p_ += 2; continue; }
      if (c == '\'') { size_t e = s_.find('\'', p_ + 1); p_ = e == std::string::npos ? s_.size() : e + 1; continue; }
      if (c == '"') { ++p_; while (p_ < s_.size() && s_[p_] != '"') { if (s_[p_] == '\\') ++p_; ++p_; } ++p_; continue; }
      if (c == '{') ++depth;
      else if (c == '}') { if (--depth == 0) { std::string body = s_.substr(start, p_ - start); ++p_; return body; } }
      ++p_;
    }
    hit_end = true;
    fail("unterminated ${");
  }

  // One $-expansion at p_ (on the '$'); appends to parts.
  bool scan_dollar(std::vector<WordPart>& parts) {
    char n = at(1);
    if (n == '(' && at(2) == '(') {
      // $(( ... )): arithmetic, unless it is really $( (subshell) ... ) -- decided by where the parens close.
      size_t save = p_;
      int save_line = line_;
      p_ += 3;
      int depth = 2;
      size_t start = p_;
      while (p_ < s_.size() && depth > 0) {
        if (s_[p_] == '(') ++depth;
        else if (s_[p_] == ')') { --depth; if (depth == 0) break; }
        ++p_;
      }
      if (p_ < s_.size() && p_ >= start + 1 && s_[p_ - 1] == ')' ) {
        // text between "$((" and "))"
        std::string body = s_.substr(start, p_ - 1 - start);
        ++p_;
        WordPart w; w.kind = WordPart::Arith; w.text = body; parts.push_back(w);
        return true;
      }
      p_ = save; line_ = save_line;
      p_ += 2;
      WordPart w; w.kind = WordPart::CommandSub; w.text = scan_balanced_parens(); parts.push_back(w);
      return true;
    }
    if (n == '(') {
      p_ += 2;
      WordPart w; w.kind = WordPart::CommandSub; w.text = scan_balanced_parens(); parts.push_back(w);
      return true;
    }
    if (n == '{') {
      p_ += 2;
      WordPart w; w.kind = WordPart::Param; w.braced = true; w.text = scan_brace_param(); parts.push_back(w);
      return true;
    }
    if (is_name_start(n)) {
      size_t k = p_ + 1;
      while (k < s_.size() && is_name_char(s_[k])) ++k;
      WordPart w; w.kind = WordPart::Param; w.text = s_.substr(p_ + 1, k - p_ - 1); parts.push_back(w);
      p_ = k;
      return true;
    }
    if (n && (std::isdigit(static_cast<unsigned char>(n)) || std::strchr("@*#?$!-", n))) {
      WordPart w; w.kind = WordPart::Param; w.text = std::string(1, n); parts.push_back(w);
      p_ += 2;
      return true;
    }
    return false;   // a lone $
  }

  std::string scan_backquote() {
    ++p_;   // past `
    std::string body;
    while (p_ < s_.size() && s_[p_] != '`') {
      if (s_[p_] == '\\' && p_ + 1 < s_.size() && std::strchr("$`\\", s_[p_ + 1])) { body += s_[p_ + 1]; p_ += 2; continue; }
      if (s_[p_] == '\n') ++line_;
      body += s_[p_++];
    }
    if (p_ >= s_.size()) { hit_end = true; fail("unterminated `"); }
    ++p_;
    return body;
  }

  Word scan_word(bool, bool* plain) {
    Word w;
    std::string lit;
    bool is_plain = true;
    auto flush = [&]() { if (!lit.empty()) { WordPart wp; wp.kind = WordPart::Literal; wp.text = lit; w.parts.push_back(wp); lit.clear(); } };
    // A leading ~ (unquoted): its user part runs to the first /.
    if (cur() == '~') {
      size_t k = p_ + 1;
      while (k < s_.size() && !is_meta(s_[k]) && s_[k] != '/' && s_[k] != '"' && s_[k] != '\'') ++k;
      WordPart wp; wp.kind = WordPart::Tilde; wp.text = s_.substr(p_ + 1, k - p_ - 1); w.parts.push_back(wp);
      p_ = k;
      is_plain = false;
    }
    while (p_ < s_.size()) {
      char c = s_[p_];
      // a=( ... ): an array literal, when what precedes is the assignment's name
      if (c == '(' && !word_only && w.parts.empty() && lit.size() >= 2 && lit.back() == '=') {
        std::string name = lit.substr(0, lit.size() - 1);
        if (!name.empty() && name.back() == '+') name.pop_back();
        bool ok = !name.empty() && is_name_start(name[0]);
        for (char ch : name) ok = ok && is_name_char(ch);
        if (ok) {
          ++p_;
          flush();
          WordPart wp; wp.kind = WordPart::ArrayLit; wp.text = scan_balanced_parens(); w.parts.push_back(wp);
          is_plain = false;
          continue;
        }
      }
      if (is_meta(c) && !word_only) break;
      // $'...': ANSI-C quoting
      if (c == '$' && at(1) == '\'') {
        flush();
        p_ += 2;
        std::string v;
        while (p_ < s_.size() && s_[p_] != '\'') {
          char d = s_[p_++];
          if (d != '\\' || p_ >= s_.size()) { v += d; continue; }
          char e = s_[p_++];
          switch (e) {
            case 'n': v += '\n'; break; case 't': v += '\t'; break; case 'r': v += '\r'; break; case 'a': v += '\a'; break;
            case 'b': v += '\b'; break; case 'f': v += '\f'; break; case 'v': v += '\v'; break; case 'e': case 'E': v += '\x1b'; break;
            case '\\': v += '\\'; break; case '\'': v += '\''; break; case '"': v += '"'; break;
            case 'x': { int n = 0, k = 0; while (k < 2 && p_ < s_.size() && std::isxdigit(static_cast<unsigned char>(s_[p_]))) { char h = s_[p_++]; n = n * 16 + (std::isdigit(static_cast<unsigned char>(h)) ? h - '0' : std::tolower(h) - 'a' + 10); ++k; } v += static_cast<char>(n); break; }
            default:
              if (e >= '0' && e <= '7') { int n = e - '0', k = 1; while (k < 3 && p_ < s_.size() && s_[p_] >= '0' && s_[p_] <= '7') { n = n * 8 + (s_[p_++] - '0'); ++k; } v += static_cast<char>(n); }
              else { v += '\\'; v += e; }
          }
        }
        if (p_ >= s_.size()) { hit_end = true; fail("unterminated $'"); }
        ++p_;
        WordPart wp; wp.kind = WordPart::SingleQuoted; wp.text = v; w.parts.push_back(wp);
        is_plain = false;
        continue;
      }
      if (c == '\\') {
        if (at(1) == '\n') { p_ += 2; ++line_; continue; }
        flush();
        // an escaped char is a quoted literal (no globbing, no splitting)
        WordPart wp; wp.kind = WordPart::SingleQuoted; wp.text = std::string(1, at(1)); w.parts.push_back(wp);
        p_ += 2;
        is_plain = false;
        continue;
      }
      if (c == '\'') {
        flush();
        size_t e = s_.find('\'', p_ + 1);
        if (e == std::string::npos) { hit_end = true; fail("unterminated '"); }
        WordPart wp; wp.kind = WordPart::SingleQuoted; wp.text = s_.substr(p_ + 1, e - p_ - 1);
        for (char ch : wp.text) if (ch == '\n') ++line_;
        w.parts.push_back(wp);
        p_ = e + 1;
        is_plain = false;
        continue;
      }
      if (c == '"') {
        flush();
        ++p_;
        WordPart dq; dq.kind = WordPart::DoubleQuoted;
        std::string in;
        auto iflush = [&]() { if (!in.empty()) { WordPart wp; wp.kind = WordPart::Literal; wp.text = in; dq.inner.push_back(wp); in.clear(); } };
        while (p_ < s_.size() && s_[p_] != '"') {
          char d = s_[p_];
          if (d == '\\' && p_ + 1 < s_.size() && std::strchr("$`\"\\\n", s_[p_ + 1])) {
            if (s_[p_ + 1] != '\n') in += s_[p_ + 1]; else ++line_;
            p_ += 2;
            continue;
          }
          if (d == '$') { size_t before = p_; iflush(); if (!scan_dollar(dq.inner)) { in += '$'; p_ = before + 1; } continue; }
          if (d == '`') { iflush(); WordPart wp; wp.kind = WordPart::CommandSub; wp.text = scan_backquote(); dq.inner.push_back(wp); continue; }
          if (d == '\n') ++line_;
          in += d;
          ++p_;
        }
        if (p_ >= s_.size()) { hit_end = true; fail("unterminated \""); }
        ++p_;
        iflush();
        w.parts.push_back(dq);
        is_plain = false;
        continue;
      }
      if (c == '$') {
        size_t before = p_;
        flush();
        if (!scan_dollar(w.parts)) { lit += '$'; p_ = before + 1; continue; }
        is_plain = false;
        continue;
      }
      if (c == '`') { flush(); WordPart wp; wp.kind = WordPart::CommandSub; wp.text = scan_backquote(); w.parts.push_back(wp); is_plain = false; continue; }
      lit += c;
      ++p_;
    }
    flush();
    if (plain) *plain = is_plain;
    return w;
  }

  // ------------------------------------------------------------------ grammar

  static bool is_reserved(const Token& t, const char* word) { return t.type == Token::Word && t.plain && t.raw == word; }
  bool peek_reserved(const char* word) { return is_reserved(peek(), word); }
  bool peek_op(const char* op) { const Token& t = peek(); return t.type == Token::Op && t.raw == op; }

  void skip_newlines() { while (peek().type == Token::Newline) take(); }

  void expect_reserved(const char* word) {
    skip_newlines();
    if (!peek_reserved(word)) {
      if (peek().type == Token::End) hit_end = true;
      fail(std::string("expected '") + word + "' but found '" + peek().raw + "'");
    }
    take();
  }

  // Words that end a list inside a compound command.
  bool at_list_end() {
    const Token& t = peek();
    if (t.type == Token::End) return true;
    if (t.type == Token::Op && (t.raw == ")" || t.raw == ";;")) return true;
    if (t.type == Token::Word && t.plain)
      for (const char* r : {"then", "else", "elif", "fi", "do", "done", "esac", "}"}) if (t.raw == r) return true;
    return false;
  }

  // and_or ((; | & | newline) and_or)*  into a List node. `top`: a newline at top level is a separator too.
  void parse_list_into(Node& list, bool top) {
    for (;;) {
      skip_newlines();
      if (at_list_end()) return;
      auto ao = and_or();
      list.kids.push_back(ao);
      list.ops.push_back(";");
      const Token& t = peek();
      if (t.type == Token::Op && (t.raw == ";" || t.raw == "&")) {
        list.ops.back() = take().raw;
        continue;
      }
      if (t.type == Token::Newline) { take(); continue; }
      (void)top;
      return;
    }
  }

  std::shared_ptr<Node> compound_list() {
    auto list = std::make_shared<Node>();
    list->kind = Node::List;
    parse_list_into(*list, false);
    return list;
  }

  std::shared_ptr<Node> and_or() {
    auto first = pipeline();
    if (!(peek_op("&&") || peek_op("||"))) return first;
    auto n = std::make_shared<Node>();
    n->kind = Node::AndOr;
    n->kids.push_back(first);
    while (peek_op("&&") || peek_op("||")) {
      n->ops.push_back(take().raw);
      skip_newlines();
      n->kids.push_back(pipeline());
    }
    return n;
  }

  std::shared_ptr<Node> pipeline() {
    bool bang = false;
    if (peek_reserved("!")) { take(); bang = true; }
    auto first = command();
    std::shared_ptr<Node> result = first;
    if (peek_op("|") || peek_op("|&")) {
      auto n = std::make_shared<Node>();
      n->kind = Node::Pipeline;
      n->kids.push_back(first);
      while (peek_op("|") || peek_op("|&")) {
        n->ops.push_back(take().raw);   // "|&": the stage's stderr goes down the pipe too
        skip_newlines();
        n->kids.push_back(command());
      }
      result = n;
    }
    if (bang) {
      auto n = std::make_shared<Node>();
      n->kind = Node::Not;
      n->kids.push_back(result);
      result = n;
    }
    return result;
  }

  bool redirect_ahead() {
    const Token& t = peek();
    if (t.type == Token::IoNumber) return true;
    if (t.type != Token::Op) return false;
    for (const char* r : {"<", ">", ">>", "<&", ">&", "<>", ">|", "<<", "<<-", "<<<", "&>", "&>>"}) if (t.raw == r) return true;
    return false;
  }

  void parse_redirect(const std::shared_ptr<Node>& node) {
    Redirect r;
    int fd = -1;
    if (peek().type == Token::IoNumber) fd = std::stoi(take().raw);
    std::string op = take().raw;
    if (op == "<") { r.kind = Redirect::In; r.fd = 0; }
    else if (op == ">") { r.kind = Redirect::Out; r.fd = 1; }
    else if (op == ">|") { r.kind = Redirect::Clobber; r.fd = 1; }
    else if (op == ">>") { r.kind = Redirect::Append; r.fd = 1; }
    else if (op == "<&") { r.kind = Redirect::DupIn; r.fd = 0; }
    else if (op == ">&") { r.kind = Redirect::DupOut; r.fd = 1; }
    else if (op == "<>") { r.kind = Redirect::ReadWrite; r.fd = 0; }
    else if (op == "&>") { r.kind = Redirect::OutErr; r.fd = 1; }
    else if (op == "&>>") { r.kind = Redirect::AppendOutErr; r.fd = 1; }
    else if (op == "<<" || op == "<<-") { r.kind = Redirect::HereDoc; r.fd = 0; r.strip_tabs = op == "<<-"; }
    else if (op == "<<<") { r.kind = Redirect::HereString; r.fd = 0; }
    if (fd >= 0) r.fd = fd;
    const Token& t = peek();
    if (t.type != Token::Word) { if (t.type == Token::End) hit_end = true; fail("expected a word after " + op); }
    Token target = take();
    if (r.kind == Redirect::HereDoc) {
      // the delimiter: quote removal, and any quoting turns expansion off
      std::string delim;
      bool quoted = false;
      for (const auto& part : target.word.parts) {
        if (part.kind == WordPart::Literal) delim += part.text;
        else if (part.kind == WordPart::SingleQuoted) { delim += part.text; quoted = true; }
        else if (part.kind == WordPart::DoubleQuoted) { quoted = true; for (const auto& in : part.inner) delim += in.text; }
        else delim += (part.kind == WordPart::Param ? "$" + part.text : part.text);
      }
      r.heredoc_expand = !quoted;
      node->redirects.push_back(r);
      heredocs_.push_back({node, node->redirects.size() - 1, delim});
      return;
    }
    // >&2, 2>&1, <&- : a DupOut whose word is a number or '-'; >& file is &>
    if ((r.kind == Redirect::DupOut || r.kind == Redirect::DupIn) && target.plain) {
      bool num = !target.raw.empty();
      for (char c : target.raw) num = num && std::isdigit(static_cast<unsigned char>(c));
      if (!num && target.raw != "-" && r.kind == Redirect::DupOut && fd < 0) r.kind = Redirect::OutErr;
    }
    r.target = target.word;
    node->redirects.push_back(r);
  }

  void trailing_redirects(const std::shared_ptr<Node>& n) { while (redirect_ahead()) parse_redirect(n); }

  std::shared_ptr<Node> command() {
    const Token& t = peek();
    int line = t.line;
    std::shared_ptr<Node> n;
    if (t.type == Token::Op && t.raw == "(" && cur() == '(') {
      // (( expr )): an arithmetic command
      ++p_;
      size_t start = p_;
      int depth = 2;
      while (p_ < s_.size()) {
        if (s_[p_] == '(') ++depth;
        else if (s_[p_] == ')' && --depth == 0) break;
        if (s_[p_] == '\n') ++line_;
        ++p_;
      }
      if (p_ >= s_.size() || s_[p_ - 1] != ')') { hit_end = p_ >= s_.size(); fail("expected '))'"); }
      have_peek_ = false;
      n = std::make_shared<Node>();
      n->kind = Node::ArithCmd;
      n->name = s_.substr(start, p_ - 1 - start);
      ++p_;
      n->line = line;
      trailing_redirects(n);
      return n;
    }
    if (is_reserved(t, "[[")) {
      take();
      n = std::make_shared<Node>();
      n->kind = Node::Cond;
      for (;;) {
        const Token& u = peek();
        if (u.type == Token::End) { hit_end = true; fail("expected ']]'"); }
        if (u.type == Token::Newline) { take(); continue; }
        if (is_reserved(u, "]]")) { take(); break; }
        Token tk = take();
        if (tk.type == Token::IoNumber) { Word w; WordPart wp; wp.kind = WordPart::Literal; wp.text = tk.raw; w.parts.push_back(wp); n->words.push_back(w); n->ops.push_back(""); }
        else if (tk.type == Token::Op) { Word w; WordPart wp; wp.kind = WordPart::Literal; wp.text = tk.raw; w.parts.push_back(wp); n->words.push_back(w); n->ops.push_back("op"); }
        else { n->words.push_back(tk.word); n->ops.push_back(""); }
      }
      n->line = line;
      trailing_redirects(n);
      return n;
    }
    if (t.type == Token::Op && t.raw == "(") {
      take();
      n = std::make_shared<Node>();
      n->kind = Node::Subshell;
      n->kids.push_back(compound_list());
      skip_newlines();
      if (!peek_op(")")) { if (peek().type == Token::End) hit_end = true; fail("expected ')'"); }
      take();
    } else if (is_reserved(t, "{")) {
      take();
      n = std::make_shared<Node>();
      n->kind = Node::Group;
      n->kids.push_back(compound_list());
      expect_reserved("}");
    } else if (is_reserved(t, "if")) {
      n = if_clause();
    } else if (is_reserved(t, "while") || is_reserved(t, "until")) {
      bool until = take().raw == "until";
      n = std::make_shared<Node>();
      n->kind = until ? Node::Until : Node::While;
      n->kids.push_back(compound_list());
      expect_reserved("do");
      n->kids.push_back(compound_list());
      expect_reserved("done");
    } else if (is_reserved(t, "for")) {
      n = for_clause();
    } else if (is_reserved(t, "case")) {
      n = case_clause();
    } else if (is_reserved(t, "function")) {
      take();
      Token name = take();
      if (peek_op("(")) { take(); if (!peek_op(")")) fail("expected ')'"); take(); }
      skip_newlines();
      n = std::make_shared<Node>();
      n->kind = Node::FuncDef;
      n->name = name.raw;
      n->kids.push_back(command());
      n->line = line;
      return n;
    } else {
      n = simple_command();
      n->line = line;
      return n;
    }
    n->line = line;
    trailing_redirects(n);
    return n;
  }

  std::shared_ptr<Node> if_clause() {
    take();   // if
    auto n = std::make_shared<Node>();
    n->kind = Node::If;
    n->kids.push_back(compound_list());
    expect_reserved("then");
    n->kids.push_back(compound_list());
    for (;;) {
      skip_newlines();
      if (peek_reserved("elif")) {
        take();
        n->kids.push_back(compound_list());
        expect_reserved("then");
        n->kids.push_back(compound_list());
        continue;
      }
      if (peek_reserved("else")) { take(); n->kids.push_back(compound_list()); }
      break;
    }
    expect_reserved("fi");
    return n;
  }

  std::shared_ptr<Node> for_clause() {
    take();   // for
    auto n = std::make_shared<Node>();
    n->kind = Node::For;
    if (peek_op("(") && cur() == '(') {
      // for (( init; cond; step )): name "((" and the three expressions in ops
      ++p_;
      size_t start = p_;
      int depth = 2;
      while (p_ < s_.size()) {
        if (s_[p_] == '(') ++depth;
        else if (s_[p_] == ')' && --depth == 0) break;
        ++p_;
      }
      if (p_ >= s_.size()) { hit_end = true; fail("expected '))'"); }
      std::string body = s_.substr(start, p_ - 1 - start);
      ++p_;
      have_peek_ = false;
      n->name = "((";
      size_t a = body.find(';'), b = a == std::string::npos ? a : body.find(';', a + 1);
      if (b == std::string::npos) fail("expected 'init; cond; step' in for ((...))");
      n->ops = {body.substr(0, a), body.substr(a + 1, b - a - 1), body.substr(b + 1)};
      skip_newlines();
      if (peek_op(";")) take();
      skip_newlines();
      expect_reserved("do");
      n->kids.push_back(compound_list());
      expect_reserved("done");
      return n;
    }
    Token name = take();
    if (name.type != Token::Word) fail("expected a name after for");
    n->name = name.raw;
    skip_newlines();
    if (peek_reserved("in")) {
      take();
      n->has_in = true;
      while (peek().type == Token::Word) n->items.push_back(take().word);
    }
    if (peek_op(";")) take();
    expect_reserved("do");
    n->kids.push_back(compound_list());
    expect_reserved("done");
    return n;
  }

  std::shared_ptr<Node> case_clause() {
    take();   // case
    auto n = std::make_shared<Node>();
    n->kind = Node::Case;
    if (peek().type != Token::Word) fail("expected a word after case");
    n->words.push_back(take().word);
    expect_reserved("in");
    for (;;) {
      skip_newlines();
      if (peek_reserved("esac")) { take(); break; }
      if (peek().type == Token::End) { hit_end = true; fail("expected 'esac'"); }
      if (peek_op("(")) take();
      std::vector<Word> pats;
      for (;;) {
        if (peek().type != Token::Word) fail("expected a pattern in case");
        pats.push_back(take().word);
        if (peek_op("|")) { take(); continue; }
        break;
      }
      if (!peek_op(")")) fail("expected ')' after a case pattern");
      take();
      n->case_patterns.push_back(pats);
      n->kids.push_back(compound_list());
      skip_newlines();
      if (peek_op(";;")) { take(); continue; }
      if (peek_reserved("esac")) { take(); break; }
      fail("expected ';;' or 'esac'");
    }
    return n;
  }

  // NAME[subscript]=value or NAME[subscript]+=value: the name keeps the subscript's source, expanded when it runs
  static bool element_assignment(const Token& t, std::string& name, Word& value) {
    if (t.type != Token::Word || t.word.parts.empty() || t.word.parts[0].kind != WordPart::Literal) return false;
    const std::string& raw = t.raw;
    size_t k = 0;
    if (raw.empty() || !is_name_start(raw[0])) return false;
    while (k < raw.size() && is_name_char(raw[k])) ++k;
    if (k >= raw.size() || raw[k] != '[') return false;
    int depth = 0;
    size_t close = std::string::npos;
    for (size_t i = k; i < raw.size(); ++i) {
      if (raw[i] == '[') ++depth;
      else if (raw[i] == ']' && --depth == 0) { close = i; break; }
    }
    if (close == std::string::npos) return false;
    size_t eq = close + 1;
    bool append = eq < raw.size() && raw[eq] == '+';
    if (append) ++eq;
    if (eq >= raw.size() || raw[eq] != '=') return false;
    name = raw.substr(0, close + 1) + (append ? "+" : "");
    value = parse_word_text(raw.substr(eq + 1));
    return true;
  }

  static bool assignment_word(const Token& t, std::string& name) {
    if (t.type != Token::Word || t.word.parts.empty() || t.word.parts[0].kind != WordPart::Literal) return false;
    const std::string& first = t.word.parts[0].text;
    size_t eq = first.find('=');
    if (eq == std::string::npos || eq == 0 || !is_name_start(first[0])) return false;
    size_t name_end = first[eq - 1] == '+' ? eq - 1 : eq;   // a+=b appends
    if (name_end == 0) return false;
    for (size_t i = 0; i < name_end; ++i) if (!is_name_char(first[i])) return false;
    name = first.substr(0, eq);   // "a" or "a+"
    return true;
  }

  std::shared_ptr<Node> simple_command() {
    auto n = std::make_shared<Node>();
    n->kind = Node::Simple;
    // prefix: assignments and redirections
    for (;;) {
      if (redirect_ahead()) { parse_redirect(n); continue; }
      std::string name;
      Word element_value;
      if (n->words.empty() && element_assignment(peek(), name, element_value)) {
        take();
        n->assigns.push_back({name, element_value});
        continue;
      }
      if (n->words.empty() && assignment_word(peek(), name)) {
        Token t = take();
        Word value = t.word;
        value.parts[0].text = value.parts[0].text.substr(name.size() + 1);
        if (value.parts[0].text.empty()) value.parts.erase(value.parts.begin());
        n->assigns.push_back({name, value});
        continue;
      }
      break;
    }
    // A function definition: NAME ( ) body
    if (n->assigns.empty() && n->redirects.empty() && peek().type == Token::Word && peek().plain) {
      Token name = take();
      if (peek_op("(")) {
        take();
        if (!peek_op(")")) fail("expected ')' in a function definition");
        take();
        skip_newlines();
        auto f = std::make_shared<Node>();
        f->kind = Node::FuncDef;
        f->name = name.raw;
        f->kids.push_back(command());
        return f;
      }
      n->words.push_back(name.word);
    }
    for (;;) {
      if (redirect_ahead()) { parse_redirect(n); continue; }
      const Token& t = peek();
      if (t.type != Token::Word) break;
      n->words.push_back(take().word);
    }
    if (n->words.empty() && n->assigns.empty() && n->redirects.empty()) {
      const Token& t = peek();
      if (t.type == Token::End) hit_end = true;
      fail("unexpected '" + t.raw + "'");
    }
    return n;
  }
};

}  // namespace

Word parse_word_text(const std::string& text) {
  Parser p(text);
  p.word_only = true;
  return p.whole_word();
}

std::shared_ptr<Node> parse(const std::string& text, bool* incomplete) {
  Parser p(text);
  try {
    return p.program();
  } catch (const ParseError&) {
    if (incomplete && p.hit_end) { *incomplete = true; return nullptr; }
    throw;
  }
}

}  // namespace aiossh
