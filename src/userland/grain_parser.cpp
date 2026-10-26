// grain, stage two: tokens to a tree.
//
// Expressions are parsed by precedence climbing, which is the one technique
// that gets `a = b + c * -d(e)[f].g` right without a grammar table and without
// twelve mutually recursive functions named after precedence levels nobody can
// keep straight.
//
// The parser does not stop at the first error. A language you are writing IN
// wants to hear about the second mistake on the same run, so after a bad
// statement it discards tokens to the next line or `}` and carries on. Only
// the first few are worth printing; a cascade past that is noise.
#include "grain_parser.hpp"

namespace aios::userland::grain {

namespace {

int infix_power(Tok t) {
  switch (t) {
    case Tok::QuestionQuestion: return 1;   // a ?? b: looser than or, so `x ?? y or z` is x ?? (y or z)
    case Tok::Or: return 2;
    case Tok::And: return 3;
    case Tok::Pipe: return 4;
    case Tok::Caret: return 5;
    case Tok::Amp: return 6;
    case Tok::EqEq: case Tok::BangEq: return 7;
    case Tok::Lt: case Tok::LtEq: case Tok::Gt: case Tok::GtEq: return 8;
    case Tok::LtLt: case Tok::GtGt: return 9;
    case Tok::Plus: case Tok::Minus: return 10;
    case Tok::Star: case Tok::Slash: case Tok::Percent: return 11;
    default: return 0;
  }
}

// f"..." -> "lit" + str(expr) + "lit" ... Escapes were resolved by the lexer,
// which leaves a literal brace doubled ({{ / }}), so here `{{` is a brace and
// `{` opens an expression that runs to its matching `}` - nested braces and
// quoted strings inside it (a map literal, a '}' in a string) are skipped over,
// not taken as the end. Errors inside an expression name the f-string's line.
ExprP parse_fstring(const Token& t, std::vector<Diagnostic>* errors) {
  const std::string& raw = t.text;
  const int line = t.line;
  const int col = t.column;
  ExprP result = nullptr;
  auto fail = [&](const std::string& msg) {
    if (errors && errors->size() < 12) errors->push_back(Diagnostic{line, col, msg});
  };
  auto append_piece = [&](ExprP piece) {
    if (!result) { result = std::move(piece); return; }
    auto add = std::make_unique<Expr>();
    add->kind = ExprKind::Binary;
    add->op = Tok::Plus;
    add->line = line;
    add->column = col;
    add->a = std::move(result);
    add->b = std::move(piece);
    result = std::move(add);
  };
  std::string lit;
  auto flush_lit = [&]() {
    if (lit.empty()) return;
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::StrLit;
    e->text = lit;
    e->line = line;
    e->column = col;
    append_piece(std::move(e));
    lit.clear();
  };

  size_t i = 0;
  while (i < raw.size()) {
    const char c = raw[i];
    if (c == '}') {
      if (i + 1 < raw.size() && raw[i + 1] == '}') { lit.push_back('}'); i += 2; continue; }
      fail("a lone '}' in a formatted string (write }} for a brace)");
      return result ? std::move(result) : std::make_unique<Expr>();
    }
    if (c == '$' && i + 1 < raw.size() && raw[i + 1] == '{') {
      ++i;
    } else if (c != '{') {
      lit.push_back(c);
      ++i;
      continue;
    }
    if (i + 1 < raw.size() && raw[i + 1] == '{') { lit.push_back('{'); i += 2; continue; }

    // {expression}: find the matching close brace.
    size_t j = i + 1;
    int depth = 1;
    char quote = 0;
    for (; j < raw.size(); ++j) {
      const char ch = raw[j];
      if (quote) {
        if (ch == '\\') { ++j; continue; }
        if (ch == quote) quote = 0;
        continue;
      }
      if (ch == '"' || ch == '\'') quote = ch;
      else if (ch == '{') ++depth;
      else if (ch == '}' && --depth == 0) break;
    }
    if (j >= raw.size()) { fail("unterminated '{' in formatted string"); break; }
    const std::string src = raw.substr(i + 1, j - i - 1);
    i = j + 1;
    if (src.find_first_not_of(" \t") == std::string::npos) { fail("empty {} in formatted string"); continue; }

    std::vector<Token> all_toks = lex_all(src);
    for (auto& tk : all_toks) tk.line += line - 1;
    Parser full_inner(all_toks, nullptr);
    ExprP full_val = full_inner.expression();

    ExprP value = nullptr;
    std::string spec_part;

    if (full_val && full_inner.finished()) {
      value = std::move(full_val);
    } else {
      size_t colon = std::string::npos;
      int pdepth = 0;
      char pquote = 0;
      for (size_t k = 0; k < src.size(); ++k) {
        char ch = src[k];
        if (pquote) {
          if (ch == '\\') { ++k; continue; }
          if (ch == pquote) pquote = 0;
          continue;
        }
        if (ch == '"' || ch == '\'') pquote = ch;
        else if (ch == '(' || ch == '[' || ch == '{') ++pdepth;
        else if (ch == ')' || ch == ']' || ch == '}') --pdepth;
        else if (ch == ':' && pdepth == 0) {
          colon = k;
          break;
        }
      }
      const std::string expr_part = colon != std::string::npos ? src.substr(0, colon) : src;
      spec_part = colon != std::string::npos ? src.substr(colon + 1) : "";

      std::vector<Token> toks = lex_all(expr_part);
      for (auto& tk : toks) tk.line += line - 1;
      const size_t errors_before = errors ? errors->size() : 0;
      Parser inner(std::move(toks), errors);
      value = inner.expression();
      if (!inner.finished() && (!errors || errors->size() == errors_before))
        fail("unexpected text after the expression in {" + expr_part + "}");
    }

    flush_lit();
    if (!spec_part.empty()) {
      auto fmt_call = std::make_unique<Expr>();
      fmt_call->kind = ExprKind::Call;
      fmt_call->line = line;
      fmt_call->column = col;
      auto fmt_name = std::make_unique<Expr>();
      fmt_name->kind = ExprKind::Name;
      fmt_name->text = "format";
      fmt_name->line = line;
      fmt_name->column = col;
      fmt_call->a = std::move(fmt_name);
      auto fmt_spec = std::make_unique<Expr>();
      fmt_spec->kind = ExprKind::StrLit;
      fmt_spec->text = "{:" + spec_part + "}";
      fmt_spec->line = line;
      fmt_spec->column = col;
      fmt_call->items.push_back(std::move(fmt_spec));
      fmt_call->items.push_back(std::move(value));
      append_piece(std::move(fmt_call));
    } else {
      auto str_call = std::make_unique<Expr>();
      str_call->kind = ExprKind::Call;
      str_call->line = line;
      str_call->column = col;
      auto str_name = std::make_unique<Expr>();
      str_name->kind = ExprKind::Name;
      str_name->text = "str";
      str_name->line = line;
      str_name->column = col;
      str_call->a = std::move(str_name);
      str_call->items.push_back(std::move(value));
      append_piece(std::move(str_call));
    }
  }
  flush_lit();

  if (!result) {
    result = std::make_unique<Expr>();
    result->kind = ExprKind::StrLit;
    result->text = "";
    result->line = line;
    result->column = col;
  }
  return result;
}

}  // namespace

bool Parser::finished() {
  skip_newlines();
  return check(Tok::End);
}

Parser::Parser(std::vector<Token> tokens, std::vector<Diagnostic>* errors)
    : toks_(std::move(tokens)), errors_(errors) {}

const Token& Parser::peek(size_t ahead) const {
  const size_t i = pos_ + ahead;
  return i < toks_.size() ? toks_[i] : toks_.back();
}

const Token& Parser::advance() {
  if (pos_ + 1 < toks_.size()) return toks_[pos_++];
  return toks_.back();
}

bool Parser::check(Tok k) const { return peek().kind == k; }

bool Parser::match(Tok k) {
  if (!check(k)) return false;
  advance();
  return true;
}

void Parser::error_at(const Token& t, const std::string& message) {
  if (panicked_) return;   // one complaint per bad construct, not one per token
  panicked_ = true;
  if (errors_->size() < 12) errors_->push_back(Diagnostic{t.line, t.column, message});
}

bool Parser::expect(Tok k, const char* what) {
  if (match(k)) return true;
  error_at(peek(), std::string("expected ") + what + ", found " + tok_name(peek().kind));
  return false;
}

// Newlines separate statements, so most places have to be willing to step over
// any number of them -- including none.
void Parser::skip_newlines() {
  while (check(Tok::Newline)) advance();
}

// After a bad statement, get back to something that can start a new one rather
// than reporting every token of the wreckage.
void Parser::synchronise() {
  panicked_ = false;
  while (!check(Tok::End)) {
    if (check(Tok::Newline)) { advance(); return; }
    if (check(Tok::RBrace)) return;
    switch (peek().kind) {
      case Tok::Fn: case Tok::Let: case Tok::If:
      case Tok::While: case Tok::For: case Tok::Return:
        return;
      default: advance();
    }
  }
}

// --- expressions -----------------------------------------------------------

ExprP Parser::primary() {
  const Token t = peek();
  switch (t.kind) {
    case Tok::Int: {
      advance();
      auto e = std::make_unique<Expr>(); e->kind = ExprKind::IntLit; e->integer = t.integer; e->line = t.line; e->column = t.column;
      return e;
    }
    case Tok::Real: {
      advance();
      auto e = std::make_unique<Expr>(); e->kind = ExprKind::RealLit; e->real = t.real; e->line = t.line; e->column = t.column;
      return e;
    }
    case Tok::Str: {
      advance();
      auto e = std::make_unique<Expr>(); e->kind = ExprKind::StrLit; e->text = t.text; e->line = t.line; e->column = t.column;
      return e;
    }
    case Tok::FStr: {
      advance();
      return parse_fstring(t, errors_);
    }
    case Tok::True: case Tok::False: {
      advance();
      auto e = std::make_unique<Expr>(); e->kind = ExprKind::BoolLit;
      e->integer = (t.kind == Tok::True) ? 1 : 0; e->line = t.line; e->column = t.column;
      return e;
    }
    case Tok::NilLit: {
      advance();
      auto e = std::make_unique<Expr>(); e->kind = ExprKind::NilLit; e->line = t.line; e->column = t.column;
      return e;
    }
    case Tok::Ident: {
      std::string full_name = t.text;
      advance();
      // import "lib.gr" is import("lib.gr"): a statement that loads another file's functions and values.
      if (full_name == "import" && check(Tok::Str)) {
        auto call = std::make_unique<Expr>();
        call->kind = ExprKind::Call;
        call->line = t.line;
        call->column = t.column;
        auto callee = std::make_unique<Expr>();
        callee->kind = ExprKind::Name;
        callee->text = "import";
        callee->line = t.line;
        callee->column = t.column;
        auto path = std::make_unique<Expr>();
        path->kind = ExprKind::StrLit;
        path->text = peek().text;
        path->line = peek().line;
        path->column = peek().column;
        advance();
        call->a = std::move(callee);
        call->items.push_back(std::move(path));
        if (check(Tok::Ident) && peek().text == "as") {
          advance();
          if (check(Tok::Ident)) {
            auto alias = std::make_unique<Expr>();
            alias->kind = ExprKind::StrLit;
            alias->text = peek().text;
            alias->line = peek().line;
            alias->column = peek().column;
            advance();
            call->items.push_back(std::move(alias));
          } else {
            error_at(peek(), "expected identifier after 'as'");
          }
        }
        return call;
      }
      if (full_name == "import" && check(Tok::LBrace)) {
        advance();
        struct ImportSpec {
          std::string orig;
          std::string alias;
        };
        std::vector<ImportSpec> symbols;
        skip_newlines();
        while (!check(Tok::RBrace) && !check(Tok::End)) {
          if (!check(Tok::Ident)) { error_at(peek(), "expected identifier in import list"); break; }
          std::string orig = peek().text;
          advance();
          std::string alias = orig;
          if (check(Tok::Ident) && peek().text == "as") {
            advance();
            if (check(Tok::Ident)) {
              alias = peek().text;
              advance();
            } else {
              error_at(peek(), "expected identifier after 'as'");
            }
          }
          symbols.push_back({orig, alias});
          skip_newlines();
          if (!match(Tok::Comma)) break;
          skip_newlines();
        }
        expect(Tok::RBrace, "'}'");
        skip_newlines();
        if (check(Tok::Ident) && peek().text == "from") {
          advance();
        } else {
          error_at(peek(), "expected 'from' after import list");
        }
        skip_newlines();
        if (!check(Tok::Str)) {
          error_at(peek(), "expected file path string after 'from'");
        }
        auto call = std::make_unique<Expr>();
        call->kind = ExprKind::Call;
        call->line = t.line;
        call->column = t.column;
        auto callee = std::make_unique<Expr>();
        callee->kind = ExprKind::Name;
        callee->text = "import";
        callee->line = t.line;
        callee->column = t.column;
        auto path = std::make_unique<Expr>();
        path->kind = ExprKind::StrLit;
        path->text = peek().text;
        path->line = peek().line;
        path->column = peek().column;
        advance();
        call->a = std::move(callee);
        call->items.push_back(std::move(path));
        auto sym_list = std::make_unique<Expr>();
        sym_list->kind = ExprKind::ListLit;
        sym_list->line = t.line;
        sym_list->column = t.column;
        for (const auto& sym : symbols) {
          if (sym.orig == sym.alias) {
            auto item = std::make_unique<Expr>();
            item->kind = ExprKind::StrLit;
            item->text = sym.orig;
            item->line = t.line;
            item->column = t.column;
            sym_list->items.push_back(std::move(item));
          } else {
            auto pair = std::make_unique<Expr>();
            pair->kind = ExprKind::ListLit;
            pair->line = t.line;
            pair->column = t.column;
            auto orig_item = std::make_unique<Expr>();
            orig_item->kind = ExprKind::StrLit;
            orig_item->text = sym.orig;
            orig_item->line = t.line;
            orig_item->column = t.column;
            auto alias_item = std::make_unique<Expr>();
            alias_item->kind = ExprKind::StrLit;
            alias_item->text = sym.alias;
            alias_item->line = t.line;
            alias_item->column = t.column;
            pair->items.push_back(std::move(orig_item));
            pair->items.push_back(std::move(alias_item));
            sym_list->items.push_back(std::move(pair));
          }
        }
        call->items.push_back(std::move(sym_list));
        return call;
      }
      while (match(Tok::ColonColon)) {
        if (check(Tok::Ident)) {
          full_name += "::" + peek().text;
          advance();
        } else {
          error_at(peek(), "expected identifier after '::'");
        }
      }
      if (check(Tok::LBrace) && !no_brace_call_) {
        size_t look = 1;
        while (peek(look).kind == Tok::Newline) ++look;
        if (peek(look).kind == Tok::RBrace ||
            ((peek(look).kind == Tok::Ident || peek(look).kind == Tok::Str) && peek(look + 1).kind == Tok::Colon)) {
          ExprP map_arg = map_literal();
          auto call = std::make_unique<Expr>();
          call->kind = ExprKind::Call;
          call->line = t.line;
          call->column = t.column;
          auto callee = std::make_unique<Expr>();
          callee->kind = ExprKind::Name;
          callee->text = full_name;
          callee->line = t.line;
          callee->column = t.column;
          call->a = std::move(callee);
          call->items.push_back(std::move(map_arg));
          return call;
        }
      }
      auto e = std::make_unique<Expr>();
      if (full_name.find("::") != std::string::npos) {
        e->kind = ExprKind::StrLit;
        e->text = full_name;
      } else {
        e->kind = ExprKind::Name;
        e->text = full_name;
      }
      e->line = t.line;
      e->column = t.column;
      return e;
    }
    case Tok::LParen: {
      advance();
      skip_newlines();
      ExprP inner = expression();
      skip_newlines();
      expect(Tok::RParen, "')'");
      return inner;
    }
    case Tok::LBracket: return list_literal();
    case Tok::LBrace: return map_literal();
    case Tok::Match: return match_expr(false);
    case Tok::Fn: return lambda();
    default: break;
  }
  error_at(t, std::string("expected a value, found ") + tok_name(t.kind));
  advance();
  auto e = std::make_unique<Expr>(); e->kind = ExprKind::NilLit; e->line = t.line; e->column = t.column;
  return e;
}

// match <subject> { <pattern>[, <pattern>...] => <arm>  ...  _ => <arm> }
// Patterns are expressions, compared with == in order; the first arm whose pattern holds runs. Arms are separated by
// newlines or commas. As an expression an arm is an expression and the match is its value (nil when no arm holds
// and there is no `_`); as a statement an arm may also be a `{ block }`.
ExprP Parser::match_expr(bool as_statement) {
  const Token t = peek();
  advance();   // match
  auto e = std::make_unique<Expr>();
  e->kind = ExprKind::Match;
  e->line = t.line;
  e->column = t.column;
  e->match_statement = as_statement;
  const bool saved = no_brace_call_;
  no_brace_call_ = true;   // `match x {` - that brace opens the arms, not a call of x with a map
  e->a = expression();
  no_brace_call_ = saved;
  skip_newlines();
  expect(Tok::LBrace, "'{' after the match subject");
  skip_newlines();
  bool saw_default = false;
  while (!check(Tok::RBrace) && !check(Tok::End)) {
    Expr::MatchArm arm;
    arm.line = peek().line;
    arm.column = peek().column;
    if (check(Tok::Ident) && peek().text == "_" && peek(1).kind == Tok::FatArrow) {
      advance();
      if (saw_default) error_at(peek(), "a match has one `_` arm");
      saw_default = true;
    } else {
      if (saw_default) error_at(peek(), "an arm after `_` would never run");
      for (;;) {
        arm.patterns.push_back(expression());
        skip_newlines();
        if (!match(Tok::Comma)) break;
        skip_newlines();
      }
    }
    if (!expect(Tok::FatArrow, "'=>' after the arm's pattern")) { synchronise(); break; }
    skip_newlines();
    if (check(Tok::LBrace) && as_statement &&
        !((peek(1).kind == Tok::Ident || peek(1).kind == Tok::Str) && peek(2).kind == Tok::Colon) && peek(1).kind != Tok::RBrace) {
      advance();
      arm.is_block = true;
      arm.block = block_body();
    } else if (check(Tok::LBrace) && !as_statement && peek(1).kind != Tok::RBrace &&
               !((peek(1).kind == Tok::Ident || peek(1).kind == Tok::Str) && peek(2).kind == Tok::Colon)) {
      error_at(peek(), "a block arm needs the match as a statement (as a value, an arm is one expression)");
      arm.value = expression();
    } else {
      arm.value = expression();
    }
    e->arms.push_back(std::move(arm));
    skip_newlines();
    if (match(Tok::Comma)) skip_newlines();
  }
  expect(Tok::RBrace, "'}' to close the match");
  return e;
}

bool Parser::match_spread() {
  const Token& a = peek(0);
  const Token& b = peek(1);
  const Token& c = peek(2);
  if (a.kind != Tok::Dot || b.kind != Tok::Dot || c.kind != Tok::Dot) return false;
  if (b.line != a.line || c.line != a.line || b.column != a.column + 1 || c.column != a.column + 2) return false;
  advance();
  advance();
  advance();
  return true;
}

ExprP Parser::list_literal() {
  const Token t = peek();
  const int line = t.line;
  const int col = t.column;
  advance();                      // [
  auto e = std::make_unique<Expr>();
  e->kind = ExprKind::ListLit;
  e->line = line;
  e->column = col;
  skip_newlines();
  if (!check(Tok::RBracket)) {
    for (;;) {
      skip_newlines();
      const bool spread = match_spread();
      e->items.push_back(expression());
      if (spread || !e->spread.empty()) e->spread.resize(e->items.size(), 0);
      if (spread) e->spread.back() = 1;
      skip_newlines();
      if (!match(Tok::Comma)) break;
      skip_newlines();
      if (check(Tok::RBracket)) break;   // a trailing comma is allowed
    }
  }
  skip_newlines();
  expect(Tok::RBracket, "']'");
  return e;
}

// `{ "a": 1 }`. A bare name is taken as its own key, so `{x: 1}` needs no
// quotes -- the keys of a map are almost always identifiers in practice.
ExprP Parser::map_literal() {
  const Token t = peek();
  const int line = t.line;
  const int col = t.column;
  advance();                      // {
  auto e = std::make_unique<Expr>();
  e->kind = ExprKind::MapLit;
  e->line = line;
  e->column = col;
  skip_newlines();
  if (!check(Tok::RBrace)) {
    for (;;) {
      skip_newlines();
      std::string key;
      if (check(Tok::Str) || check(Tok::Ident)) {
        key = peek().text;
        advance();
      } else {
        error_at(peek(), "a map key must be a name or a string");
        advance();
      }
      expect(Tok::Colon, "':'");
      skip_newlines();
      e->keys.push_back(key);
      e->items.push_back(expression());
      skip_newlines();
      if (!match(Tok::Comma)) break;
      skip_newlines();
      if (check(Tok::RBrace)) break;
    }
  }
  skip_newlines();
  expect(Tok::RBrace, "'}'");
  return e;
}

ExprP Parser::lambda() {
  const Token t = peek();
  const int line = t.line;
  const int col = t.column;
  advance();                      // fn
  auto e = std::make_unique<Expr>();
  e->kind = ExprKind::Lambda;
  e->line = line;
  e->column = col;
  if (check(Tok::Ident)) { e->fn_name = peek().text; advance(); }
  expect(Tok::LParen, "'(' after fn");
  if (!check(Tok::RParen)) {
    for (;;) {
      if (check(Tok::Ident)) {
        e->params.push_back(Param{peek().text, peek().line, peek().column});
        advance();
      } else {
        error_at(peek(), "expected a parameter name");
        advance();
      }
      if (!match(Tok::Comma)) break;
    }
  }
  expect(Tok::RParen, "')'");
  skip_newlines();
  expect(Tok::LBrace, "'{'");
  const bool outer_has_defer = fn_has_defer_;
  fn_has_defer_ = false;
  ++fn_depth_;
  e->body = block_body();
  --fn_depth_;
  if (fn_has_defer_) {
    // A body with `defer` runs as
    //   let __grain_defers = []
    //   return __grain_run_deferred(fn () { <body> }, __grain_defers)
    // and each `defer S` in it as __grain_defer(__grain_defers, fn () { S }). The native runs the body, then every
    // deferred closure, last first, whether the body returned or raised; then gives back the body's value or its
    // error (else the first cleanup error). Nothing else changes: the interpreter, the JIT and error unwinding are
    // the same for every function.
    auto name = [&](const char* n) { auto x = std::make_unique<Expr>(); x->kind = ExprKind::Name; x->text = n; x->line = line; x->column = col; return x; };
    auto decl = std::make_unique<Stmt>();
    decl->kind = StmtKind::Let;
    decl->line = line; decl->column = col;
    decl->name = "__grain_defers";
    decl->expr = std::make_unique<Expr>();
    decl->expr->kind = ExprKind::ListLit;
    decl->expr->line = line; decl->expr->column = col;
    auto inner = std::make_unique<Expr>();
    inner->kind = ExprKind::Lambda;
    inner->line = line; inner->column = col;
    inner->fn_name = e->fn_name;
    inner->body = std::move(e->body);
    auto call = std::make_unique<Expr>();
    call->kind = ExprKind::Call;
    call->line = line; call->column = col;
    call->a = name("__grain_run_deferred");
    call->items.push_back(std::move(inner));
    call->items.push_back(name("__grain_defers"));
    auto ret = std::make_unique<Stmt>();
    ret->kind = StmtKind::Return;
    ret->line = line; ret->column = col;
    ret->expr = std::move(call);
    e->body.clear();
    e->body.push_back(std::move(decl));
    e->body.push_back(std::move(ret));
  }
  fn_has_defer_ = outer_has_defer;
  return e;
}

// `defer S` / `defer { ... }`: S runs when the function returns or raises, after the code below it; several run last
// first. S reads variables as they are then (it is a closure), not as they were at the defer.
StmtP Parser::defer_statement() {
  const Token t = peek();
  advance();   // defer
  if (fn_depth_ == 0) error_at(t, "defer belongs in a function body: it runs when the function returns");
  fn_has_defer_ = true;
  auto lam = std::make_unique<Expr>();
  lam->kind = ExprKind::Lambda;
  lam->line = t.line; lam->column = t.column;
  lam->fn_name = "<defer>";
  if (check(Tok::LBrace)) {
    advance();
    lam->body = block_body();
  } else {
    StmtP s = statement();
    if (s) lam->body.push_back(std::move(s));
  }
  auto call = std::make_unique<Expr>();
  call->kind = ExprKind::Call;
  call->line = t.line; call->column = t.column;
  auto fname = std::make_unique<Expr>();
  fname->kind = ExprKind::Name; fname->text = "__grain_defer"; fname->line = t.line; fname->column = t.column;
  auto list = std::make_unique<Expr>();
  list->kind = ExprKind::Name; list->text = "__grain_defers"; list->line = t.line; list->column = t.column;
  call->a = std::move(fname);
  call->items.push_back(std::move(list));
  call->items.push_back(std::move(lam));
  auto st = std::make_unique<Stmt>();
  st->kind = StmtKind::Expression;
  st->line = t.line; st->column = t.column;
  st->expr = std::move(call);
  return st;
}

// Calls, indexing and fields all bind tighter than any operator and chain left
// to right, so `a.b[0](x).c` is one loop rather than four grammar rules.
ExprP Parser::postfix(ExprP left) {
  for (;;) {
    if (check(Tok::LParen)) {
      const Token t = peek();
      const int line = t.line;
      const int col = t.column;
      advance();
      auto call = std::make_unique<Expr>();
      call->kind = ExprKind::Call;
      call->line = left ? left->line : line;
      call->column = left ? left->column : col;
      call->a = std::move(left);
      skip_newlines();
      if (!check(Tok::RParen)) {
        std::unique_ptr<Expr> named_map;
        for (;;) {
          skip_newlines();
          if (check(Tok::Ident) && peek(1).kind == Tok::Assign) {
            std::string arg_name = peek().text;
            advance();  // eat name
            advance();  // eat '='
            skip_newlines();
            ExprP val = expression();
            if (!named_map) {
              named_map = std::make_unique<Expr>();
              named_map->kind = ExprKind::MapLit;
              named_map->line = line;
              named_map->column = col;
            }
            named_map->keys.push_back(arg_name);
            named_map->items.push_back(std::move(val));
          } else {
            const bool spread = match_spread();
            call->items.push_back(expression());
            if (spread || !call->spread.empty()) call->spread.resize(call->items.size(), 0);
            if (spread) call->spread.back() = 1;
          }
          skip_newlines();
          if (!match(Tok::Comma)) break;
        }
        if (named_map) {
          call->items.push_back(std::move(named_map));
          if (!call->spread.empty()) call->spread.resize(call->items.size(), 0);
        }
      }
      skip_newlines();
      expect(Tok::RParen, "')'");
      left = std::move(call);
      continue;
    }
    if (check(Tok::LBracket)) {
      const Token t = peek();
      const int line = t.line;
      const int col = t.column;
      advance();
      skip_newlines();
      ExprP subscript = expression();
      skip_newlines();
      expect(Tok::RBracket, "']'");
      auto idx = std::make_unique<Expr>();
      idx->kind = ExprKind::Index;
      idx->line = line;
      idx->column = col;
      idx->a = std::move(left);
      idx->b = std::move(subscript);
      left = std::move(idx);
      continue;
    }
    if (check(Tok::Dot) && peek(1).kind != Tok::Dot) {   // `..` is a range, not a field
      const Token t = peek();
      const int line = t.line;
      const int col = t.column;
      advance();
      std::string name;
      if (check(Tok::Ident)) { name = peek().text; advance(); }
      else { error_at(peek(), "expected a field name after '.'"); advance(); }
      auto f = std::make_unique<Expr>();
      f->kind = ExprKind::Field;
      f->line = line;
      f->column = col;
      f->a = std::move(left);
      f->text = name;
      left = std::move(f);
      continue;
    }
    if (check(Tok::QuestionDot)) {
      const Token t = peek();
      const int line = t.line;
      const int col = t.column;
      advance();
      std::string name;
      if (check(Tok::Ident)) { name = peek().text; advance(); }
      else { error_at(peek(), "expected a field name after '?.'"); advance(); }
      auto f = std::make_unique<Expr>();
      f->kind = ExprKind::OptField;
      f->line = line;
      f->column = col;
      f->a = std::move(left);
      f->text = name;
      left = std::move(f);
      continue;
    }
    return left;
  }
}

ExprP Parser::unary() {
  if (check(Tok::Minus) || check(Tok::Not) || check(Tok::Tilde)) {
    const Token t = advance();
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Unary;
    e->op = t.kind;
    e->line = t.line;
    e->column = t.column;
    e->a = unary();
    return e;
  }
  return postfix(primary());
}

ExprP Parser::binary(int min_power) {
  ExprP left = unary();
  for (;;) {
    const Tok op = peek().kind;
    const int power = infix_power(op);
    if (power == 0 || power < min_power) return left;
    const Token op_token = peek();
    const int line = op_token.line;
    const int col = op_token.column;
    advance();
    skip_newlines();   // an operator at end of line continues the expression
    // Left-associative: the right side may only take operators that bind
    // strictly tighter, so a - b - c is (a - b) - c and not a - (b - c).
    ExprP right = binary(power + 1);
    auto e = std::make_unique<Expr>();
    e->kind = (op == Tok::And || op == Tok::Or || op == Tok::QuestionQuestion) ? ExprKind::Logical : ExprKind::Binary;
    e->op = op;
    e->line = line;
    e->column = col;
    e->a = std::move(left);
    e->b = std::move(right);
    left = std::move(e);
  }
}

// Assignment is an expression so `a = b = 0` works, but its left side has to be
// something that can be assigned TO. That is checked here, where the shape is
// known, rather than in the compiler where it would be a confusing error.
ExprP Parser::expression() {
  ExprP left = binary(1);

  // `a..b` is range(a, b): the integers from a up to b, b excluded - the same
  // bounds a native fn's `for i in a..b` takes. It binds looser than any
  // operator, so `0..n - 1` is 0..(n - 1).
  if (check(Tok::Dot) && peek(1).kind == Tok::Dot) {
    const Token dot_tok = peek();
    const int line = dot_tok.line;
    const int col = dot_tok.column;
    advance();
    advance();
    skip_newlines();
    ExprP hi = binary(1);
    auto callee = std::make_unique<Expr>();
    callee->kind = ExprKind::Name;
    callee->text = "range";
    callee->line = line;
    callee->column = col;
    auto call = std::make_unique<Expr>();
    call->kind = ExprKind::Call;
    call->line = line;
    call->column = col;
    call->a = std::move(callee);
    call->items.push_back(std::move(left));
    call->items.push_back(std::move(hi));
    left = std::move(call);
  }

  if (check(Tok::Question)) {
    const Token q_tok = peek();
    const int line = q_tok.line;
    const int col = q_tok.column;
    advance();
    skip_newlines();
    ExprP then_expr = expression();
    skip_newlines();
    expect(Tok::Colon, "':' in ternary expression");
    skip_newlines();
    ExprP else_expr = expression();

    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Ternary;
    e->line = line;
    e->column = col;
    e->a = std::move(left);
    e->b = std::move(then_expr);
    e->c = std::move(else_expr);
    return e;
  }

  const Tok op_tok = peek().kind;
  bool is_compound = false;
  Tok bin_op = Tok::End;
  switch (op_tok) {
    case Tok::PlusEq: is_compound = true; bin_op = Tok::Plus; break;
    case Tok::MinusEq: is_compound = true; bin_op = Tok::Minus; break;
    case Tok::StarEq: is_compound = true; bin_op = Tok::Star; break;
    case Tok::SlashEq: is_compound = true; bin_op = Tok::Slash; break;
    case Tok::PercentEq: is_compound = true; bin_op = Tok::Percent; break;
    case Tok::AmpEq: is_compound = true; bin_op = Tok::Amp; break;
    case Tok::PipeEq: is_compound = true; bin_op = Tok::Pipe; break;
    case Tok::CaretEq: is_compound = true; bin_op = Tok::Caret; break;
    case Tok::LtLtEq: is_compound = true; bin_op = Tok::LtLt; break;
    case Tok::GtGtEq: is_compound = true; bin_op = Tok::GtGt; break;
    default: break;
  }

  if (op_tok != Tok::Assign && !is_compound) return left;

  const Token asgn_tok = peek();
  const int line = asgn_tok.line;
  const int col = asgn_tok.column;
  advance();
  skip_newlines();
  ExprP value = expression();

  // `a[k] += v` must evaluate `a` and `k` once (a[next()] += 1 would call
  // next() twice), so the operator rides on the assignment node and the
  // compiler reuses the evaluated target rather than this cloning it.
  auto e = std::make_unique<Expr>();
  e->line = line;
  e->column = col;
  if (is_compound) e->op = bin_op;
  switch (left->kind) {
    case ExprKind::Name:
      e->kind = ExprKind::Assign;
      e->text = left->text;
      e->a = std::move(value);
      return e;
    case ExprKind::Index:
      e->kind = ExprKind::IndexSet;
      e->a = std::move(left->a);
      e->b = std::move(left->b);
      e->c = std::move(value);
      return e;
    case ExprKind::Field:
      e->kind = ExprKind::FieldSet;
      e->a = std::move(left->a);
      e->text = left->text;
      e->c = std::move(value);
      return e;
    default:
      error_at(peek(), "that is not something you can assign to");
      return left;
  }
}

// --- statements ------------------------------------------------------------

std::vector<StmtP> Parser::block_body() {
  std::vector<StmtP> out;
  skip_newlines();
  while (!check(Tok::RBrace) && !check(Tok::End)) {
    StmtP s = statement();
    if (s) out.push_back(std::move(s));
    if (panicked_) synchronise();
    skip_newlines();
  }
  expect(Tok::RBrace, "'}'");
  return out;
}

StmtP Parser::statement() {
  skip_newlines();
  const Token t = peek();

  if (t.kind == Tok::Let) {
    advance();
    auto s = std::make_unique<Stmt>();
    s->line = t.line;
    s->column = t.column;
    if (check(Tok::LBrace)) {
      advance();
      s->kind = StmtKind::Destructure;
      s->is_destructure_map = true;
      skip_newlines();
      while (!check(Tok::RBrace) && !check(Tok::End)) {
        if (!check(Tok::Ident)) { error_at(peek(), "expected identifier in destructuring pattern"); break; }
        std::string key = peek().text;
        std::string var_name = key;
        advance();
        if (match(Tok::Colon)) {
          if (!check(Tok::Ident)) { error_at(peek(), "expected variable name after ':' in destructuring"); break; }
          var_name = peek().text;
          advance();
        }
        s->keys.push_back(key);
        s->names.push_back(var_name);
        skip_newlines();
        if (!match(Tok::Comma)) break;
        skip_newlines();
      }
      expect(Tok::RBrace, "'}'");
      expect(Tok::Assign, "'='");
      skip_newlines();
      s->expr = expression();
      return s;
    } else if (check(Tok::LBracket)) {
      advance();
      s->kind = StmtKind::Destructure;
      s->is_destructure_map = false;
      skip_newlines();
      while (!check(Tok::RBracket) && !check(Tok::End)) {
        if (!check(Tok::Ident)) { error_at(peek(), "expected identifier in destructuring pattern"); break; }
        s->names.push_back(peek().text);
        advance();
        skip_newlines();
        if (!match(Tok::Comma)) break;
        skip_newlines();
      }
      expect(Tok::RBracket, "']'");
      expect(Tok::Assign, "'='");
      skip_newlines();
      s->expr = expression();
      return s;
    } else {
      s->kind = StmtKind::Let;
      if (check(Tok::Ident)) { s->name = peek().text; advance(); }
      else error_at(peek(), "expected a name after let");
      if (match(Tok::Assign)) {
        skip_newlines();
        s->expr = expression();
      }
      return s;
    }
  }

  if (t.kind == Tok::Entity) {
    return entity_declaration();
  }

  // `defer` starts a defer statement only when a block or a name follows on the same line (`defer { .. }`,
  // `defer close(f)`): anywhere else - `let defer = true`, `if defer {`, `defer ? a : b` - it is a name, as before.
  if (t.kind == Tok::Ident && t.text == "defer" && peek(1).line == t.line &&
      (peek(1).kind == Tok::LBrace || peek(1).kind == Tok::Ident || peek(1).kind == Tok::If)) {
    return defer_statement();
  }

  // `native fn name(..) -> T { .. }` and `extern fn name(..) -> T` are typed code for the
  // machine, compiled by grain_native.cpp. Take their tokens whole: the body is its own language.
  if (t.kind == Tok::Ident && (t.text == "native" || t.text == "extern") && peek(1).kind == Tok::Fn) {
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::NativeDecl;
    s->line = t.line;
    s->column = t.column;
    const bool is_extern = t.text == "extern";
    int depth = 0;
    bool in_body = false;
    while (!check(Tok::End)) {
      const Token& k = peek();
      if (is_extern && k.kind == Tok::Newline) break;
      if (k.kind == Tok::LBrace) { ++depth; in_body = true; }
      s->native_tokens.push_back(k);
      advance();
      if (k.kind == Tok::RBrace && --depth == 0 && in_body) break;
    }
    if (!is_extern && (!in_body || depth != 0)) error_at(t, "a native fn needs a body in { }");
    return s;
  }

  // `fn name(..) { .. }` is sugar for `let name = fn name(..) { .. }`. Naming
  // the lambda as well is what makes a stack trace say `fib` and not `<fn>`.
  if (t.kind == Tok::Fn && peek(1).kind == Tok::Ident) {
    ExprP fn = lambda();
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::Let;
    s->line = t.line;
    s->column = t.column;
    s->name = fn->fn_name;
    s->expr = std::move(fn);
    return s;
  }

  // `match` at the start of a statement: its arms may be blocks, and it leaves no value.
  if (t.kind == Tok::Match) {
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::Expression;
    s->line = t.line;
    s->column = t.column;
    s->expr = match_expr(true);
    return s;
  }

  // `{` starts a block everywhere except where it obviously starts a map: a
  // key and a colon cannot begin a statement, so three tokens of lookahead
  // settle it. Without this a map literal could never be the first thing on a
  // line, which the REPL runs into immediately.
  if (t.kind == Tok::LBrace) {
    const bool is_map = (peek(1).kind == Tok::Ident || peek(1).kind == Tok::Str) &&
                        peek(2).kind == Tok::Colon;
    if (!is_map) {
      advance();
      auto s = std::make_unique<Stmt>();
      s->kind = StmtKind::Block;
      s->line = t.line;
      s->column = t.column;
      s->body = block_body();
      return s;
    }
  }

  if (t.kind == Tok::If) {
    advance();
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::If;
    s->line = t.line;
    s->column = t.column;
    s->expr = expression();
    skip_newlines();
    expect(Tok::LBrace, "'{' after the condition");
    s->body = block_body();
    // `else if` chains without needing a dangling-else rule: the else branch
    // is simply one more if statement.
    size_t save = pos_;
    skip_newlines();
    if (match(Tok::Else)) {
      skip_newlines();
      if (check(Tok::If)) {
        s->other.push_back(statement());
      } else {
        expect(Tok::LBrace, "'{' after else");
        s->other = block_body();
      }
    } else {
      pos_ = save;
    }
    return s;
  }

  if (t.kind == Tok::While) {
    advance();
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::While;
    s->line = t.line;
    s->column = t.column;
    s->expr = expression();
    skip_newlines();
    expect(Tok::LBrace, "'{' after the condition");
    s->body = block_body();
    return s;
  }

  if (t.kind == Tok::For) {
    advance();
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::ForIn;
    s->line = t.line;
    s->column = t.column;
    if (check(Tok::Ident)) { s->name = peek().text; advance(); }
    else error_at(peek(), "expected a name after for");
    expect(Tok::In, "'in'");
    s->iterable = expression();
    skip_newlines();
    expect(Tok::LBrace, "'{' after the sequence");
    s->body = block_body();
    return s;
  }

  if (t.kind == Tok::Return) {
    advance();
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::Return;
    s->line = t.line;
    s->column = t.column;
    if (!check(Tok::Newline) && !check(Tok::RBrace) && !check(Tok::End))
      s->expr = expression();
    return s;
  }

  if (t.kind == Tok::Break || t.kind == Tok::Continue) {
    advance();
    auto s = std::make_unique<Stmt>();
    s->kind = (t.kind == Tok::Break) ? StmtKind::Break : StmtKind::Continue;
    s->line = t.line;
    s->column = t.column;
    return s;
  }

  if (t.kind == Tok::End) return nullptr;

  auto s = std::make_unique<Stmt>();
  s->kind = StmtKind::Expression;
  s->line = t.line;
  s->column = t.column;
  s->expr = expression();
  return s;
}

StmtP Parser::entity_declaration() {
  const Token t = advance();  // eat 'entity'
  auto s = std::make_unique<Stmt>();
  s->kind = StmtKind::EntityDecl;
  s->line = t.line;
  s->column = t.column;

  auto def = std::make_unique<EntityDef>();
  def->line = t.line;
  def->column = t.column;
  if (check(Tok::Ident)) {
    def->name = peek().text;
    advance();
  } else {
    error_at(peek(), "expected entity name after 'entity'");
  }

  skip_newlines();
  expect(Tok::LBrace, "'{' after entity name");
  skip_newlines();

  while (!check(Tok::RBrace) && !check(Tok::End)) {
    skip_newlines();
    if (check(Tok::RBrace)) break;

    if (check(Tok::On)) {
      const Token on_tok = advance();  // eat 'on'
      auto h = std::make_unique<OnHandler>();
      h->line = on_tok.line;
      h->column = on_tok.column;
      if (check(Tok::Ident)) {
        h->event = peek().text;
        advance();
      } else {
        error_at(peek(), "expected event name after 'on'");
      }
      expect(Tok::LParen, "'(' after event name");
      if (check(Tok::Ident)) {
        std::string first_id = peek().text;
        advance();
        if (check(Tok::Ident)) {
          h->param_type = first_id;
          h->param = peek().text;
          advance();
        } else {
          h->param = first_id;
        }
      }
      expect(Tok::RParen, "')' after event parameter");
      skip_newlines();
      expect(Tok::LBrace, "'{' for 'on' handler body");
      h->body = block_body();
      def->handlers.push_back(std::move(h));
      if (match(Tok::Comma)) skip_newlines();
    } else if (check(Tok::Ident) || check(Tok::Str)) {
      std::string field_name = peek().text;
      advance();
      expect(Tok::Colon, "':' after field name");
      skip_newlines();
      ExprP val_expr = expression();
      def->fields.push_back({field_name, std::move(val_expr)});
      if (match(Tok::Comma)) {
        skip_newlines();
      }
    } else {
      error_at(peek(), "expected field or 'on' handler in entity definition");
      advance();
    }
    skip_newlines();
  }
  expect(Tok::RBrace, "'}' at end of entity definition");
  s->entity_def = std::move(def);
  return s;
}

std::vector<StmtP> Parser::parse_program() {
  std::vector<StmtP> out;
  skip_newlines();
  while (!check(Tok::End)) {
    if (check(Tok::Error)) {
      error_at(peek(), peek().text);
      break;
    }
    StmtP s = statement();
    if (s) out.push_back(std::move(s));
    if (panicked_) synchronise();
    skip_newlines();
  }
  return out;
}

}  // namespace aios::userland::grain
