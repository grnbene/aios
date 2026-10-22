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

// f"..." -> "lit" + str(expr) + "lit" ... Escapes were resolved by the lexer,
// which leaves a literal brace doubled ({{ / }}), so here `{{` is a brace and
// `{` opens an expression that runs to its matching `}` - nested braces and
// quoted strings inside it (a map literal, a '}' in a string) are skipped over,
// not taken as the end. Errors inside an expression name the f-string's line.
ExprP parse_fstring(const Token& t, std::vector<Diagnostic>* errors) {
  const std::string& raw = t.text;
  const int line = t.line;
  ExprP result = nullptr;
  auto fail = [&](const std::string& msg) {
    if (errors && errors->size() < 12) errors->push_back(Diagnostic{line, msg});
  };
  auto append_piece = [&](ExprP piece) {
    if (!result) { result = std::move(piece); return; }
    auto add = std::make_unique<Expr>();
    add->kind = ExprKind::Binary;
    add->op = Tok::Plus;
    add->line = line;
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
    if (c != '{') { lit.push_back(c); ++i; continue; }
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

    std::vector<Token> toks = lex_all(src);
    for (auto& tk : toks) tk.line += line - 1;
    const size_t errors_before = errors ? errors->size() : 0;
    Parser inner(std::move(toks), errors);
    ExprP value = inner.expression();
    if (!inner.finished() && (!errors || errors->size() == errors_before))
      fail("unexpected text after the expression in {" + src + "}");

    flush_lit();
    auto str_call = std::make_unique<Expr>();
    str_call->kind = ExprKind::Call;
    str_call->line = line;
    auto str_name = std::make_unique<Expr>();
    str_name->kind = ExprKind::Name;
    str_name->text = "str";
    str_name->line = line;
    str_call->a = std::move(str_name);
    str_call->items.push_back(std::move(value));
    append_piece(std::move(str_call));
  }
  flush_lit();

  if (!result) {
    result = std::make_unique<Expr>();
    result->kind = ExprKind::StrLit;
    result->text = "";
    result->line = line;
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
  if (errors_->size() < 12) errors_->push_back(Diagnostic{t.line, message});
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
      auto e = std::make_unique<Expr>(); e->kind = ExprKind::IntLit; e->integer = t.integer; e->line = t.line;
      return e;
    }
    case Tok::Real: {
      advance();
      auto e = std::make_unique<Expr>(); e->kind = ExprKind::RealLit; e->real = t.real; e->line = t.line;
      return e;
    }
    case Tok::Str: {
      advance();
      auto e = std::make_unique<Expr>(); e->kind = ExprKind::StrLit; e->text = t.text; e->line = t.line;
      return e;
    }
    case Tok::FStr: {
      advance();
      return parse_fstring(t, errors_);
    }
    case Tok::True: case Tok::False: {
      advance();
      auto e = std::make_unique<Expr>(); e->kind = ExprKind::BoolLit;
      e->integer = (t.kind == Tok::True) ? 1 : 0; e->line = t.line;
      return e;
    }
    case Tok::NilLit: {
      advance();
      auto e = std::make_unique<Expr>(); e->kind = ExprKind::NilLit; e->line = t.line;
      return e;
    }
    case Tok::Ident: {
      std::string full_name = t.text;
      advance();
      while (match(Tok::ColonColon)) {
        if (check(Tok::Ident)) {
          full_name += "::" + peek().text;
          advance();
        } else {
          error_at(peek(), "expected identifier after '::'");
        }
      }
      if (check(Tok::LBrace)) {
        size_t look = 1;
        while (peek(look).kind == Tok::Newline) ++look;
        if (peek(look).kind == Tok::RBrace ||
            ((peek(look).kind == Tok::Ident || peek(look).kind == Tok::Str) && peek(look + 1).kind == Tok::Colon)) {
          ExprP map_arg = map_literal();
          auto call = std::make_unique<Expr>();
          call->kind = ExprKind::Call;
          call->line = t.line;
          auto callee = std::make_unique<Expr>();
          callee->kind = ExprKind::Name;
          callee->text = full_name;
          callee->line = t.line;
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
    case Tok::Fn: return lambda();
    default: break;
  }
  error_at(t, std::string("expected a value, found ") + tok_name(t.kind));
  advance();
  auto e = std::make_unique<Expr>(); e->kind = ExprKind::NilLit; e->line = t.line;
  return e;
}

ExprP Parser::list_literal() {
  const int line = peek().line;
  advance();                      // [
  auto e = std::make_unique<Expr>();
  e->kind = ExprKind::ListLit;
  e->line = line;
  skip_newlines();
  if (!check(Tok::RBracket)) {
    for (;;) {
      skip_newlines();
      e->items.push_back(expression());
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
  const int line = peek().line;
  advance();                      // {
  auto e = std::make_unique<Expr>();
  e->kind = ExprKind::MapLit;
  e->line = line;
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
  const int line = peek().line;
  advance();                      // fn
  auto e = std::make_unique<Expr>();
  e->kind = ExprKind::Lambda;
  e->line = line;
  if (check(Tok::Ident)) { e->fn_name = peek().text; advance(); }
  expect(Tok::LParen, "'(' after fn");
  if (!check(Tok::RParen)) {
    for (;;) {
      if (check(Tok::Ident)) {
        e->params.push_back(Param{peek().text, peek().line});
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
  e->body = block_body();
  return e;
}

// Calls, indexing and fields all bind tighter than any operator and chain left
// to right, so `a.b[0](x).c` is one loop rather than four grammar rules.
ExprP Parser::postfix(ExprP left) {
  for (;;) {
    if (check(Tok::LParen)) {
      const int line = peek().line;
      advance();
      auto call = std::make_unique<Expr>();
      call->kind = ExprKind::Call;
      call->line = line;
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
            }
            named_map->keys.push_back(arg_name);
            named_map->items.push_back(std::move(val));
          } else {
            call->items.push_back(expression());
          }
          skip_newlines();
          if (!match(Tok::Comma)) break;
        }
        if (named_map) {
          call->items.push_back(std::move(named_map));
        }
      }
      skip_newlines();
      expect(Tok::RParen, "')'");
      left = std::move(call);
      continue;
    }
    if (check(Tok::LBracket)) {
      const int line = peek().line;
      advance();
      skip_newlines();
      ExprP subscript = expression();
      skip_newlines();
      expect(Tok::RBracket, "']'");
      auto idx = std::make_unique<Expr>();
      idx->kind = ExprKind::Index;
      idx->line = line;
      idx->a = std::move(left);
      idx->b = std::move(subscript);
      left = std::move(idx);
      continue;
    }
    if (check(Tok::Dot)) {
      const int line = peek().line;
      advance();
      std::string name;
      if (check(Tok::Ident)) { name = peek().text; advance(); }
      else { error_at(peek(), "expected a field name after '.'"); advance(); }
      auto f = std::make_unique<Expr>();
      f->kind = ExprKind::Field;
      f->line = line;
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
    const int line = peek().line;
    advance();
    skip_newlines();   // an operator at end of line continues the expression
    // Left-associative: the right side may only take operators that bind
    // strictly tighter, so a - b - c is (a - b) - c and not a - (b - c).
    ExprP right = binary(power + 1);
    auto e = std::make_unique<Expr>();
    e->kind = (op == Tok::And || op == Tok::Or) ? ExprKind::Logical : ExprKind::Binary;
    e->op = op;
    e->line = line;
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

  if (check(Tok::Question)) {
    const int line = peek().line;
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

  const int line = peek().line;
  advance();
  skip_newlines();
  ExprP value = expression();

  // `a[k] += v` must evaluate `a` and `k` once (a[next()] += 1 would call
  // next() twice), so the operator rides on the assignment node and the
  // compiler reuses the evaluated target rather than this cloning it.
  auto e = std::make_unique<Expr>();
  e->line = line;
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
    s->kind = StmtKind::Let;
    s->line = t.line;
    if (check(Tok::Ident)) { s->name = peek().text; advance(); }
    else error_at(peek(), "expected a name after let");
    if (match(Tok::Assign)) {
      skip_newlines();
      s->expr = expression();
    }
    return s;
  }

  if (t.kind == Tok::Entity) {
    return entity_declaration();
  }

  // `fn name(..) { .. }` is sugar for `let name = fn name(..) { .. }`. Naming
  // the lambda as well is what makes a stack trace say `fib` and not `<fn>`.
  if (t.kind == Tok::Fn && peek(1).kind == Tok::Ident) {
    ExprP fn = lambda();
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::Let;
    s->line = t.line;
    s->name = fn->fn_name;
    s->expr = std::move(fn);
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
      s->body = block_body();
      return s;
    }
  }

  if (t.kind == Tok::If) {
    advance();
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::If;
    s->line = t.line;
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
    if (!check(Tok::Newline) && !check(Tok::RBrace) && !check(Tok::End))
      s->expr = expression();
    return s;
  }

  if (t.kind == Tok::Break || t.kind == Tok::Continue) {
    advance();
    auto s = std::make_unique<Stmt>();
    s->kind = (t.kind == Tok::Break) ? StmtKind::Break : StmtKind::Continue;
    s->line = t.line;
    return s;
  }

  if (t.kind == Tok::End) return nullptr;

  auto s = std::make_unique<Stmt>();
  s->kind = StmtKind::Expression;
  s->line = t.line;
  s->expr = expression();
  return s;
}

StmtP Parser::entity_declaration() {
  const Token t = advance();  // eat 'entity'
  auto s = std::make_unique<Stmt>();
  s->kind = StmtKind::EntityDecl;
  s->line = t.line;

  auto def = std::make_unique<EntityDef>();
  def->line = t.line;
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
      advance();  // eat 'on'
      auto h = std::make_unique<OnHandler>();
      h->line = peek().line;
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
