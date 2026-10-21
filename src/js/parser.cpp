// The JavaScript parser: ECMAScript 2022-era syntax (modules aside) into the tree in ast.hpp.
#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_set>

#include "ast.hpp"
#include "bigint.hpp"
#include "lexer.hpp"

namespace aios::js {

namespace {

struct Parser {
  Program& prog;
  std::shared_ptr<const std::u16string> source;
  Lexer lx;
  Token cur;
  uint32_t prev_end = 0;
  uint32_t prev_line = 1;
  bool failed = false;
  ParseError err;
  bool strict = false;
  bool no_in = false;

  // Module mode (module_to_function below): imports and exports are not compiled as such. Each one is recorded as an edit of
  // the source text plus a line of generated code, and the module becomes the body of an async function, so the compiler never
  // learns a second kind of program and top-level await is just await.
  struct ModEdit { uint32_t start, end; std::u16string repl; };
  bool module_mode = false;
  bool module_top_item = false;
  int module_seq = 0;
  std::vector<ModEdit> mod_edits;
  std::u16string mod_hoist;       // `const __aios_mN = await __aios_import("spec")` and the bindings taken from it
  std::u16string mod_exports;     // `"name":()=>local,` entries of the export table
  std::u16string mod_stars;       // `export * from` re-exports

  struct FnCtx {
    FuncInfo* info = nullptr;
    bool is_async = false, is_generator = false, is_arrow = false;
    bool allow_return = false;
    bool allow_super_prop = false, allow_super_call = false, allow_new_target = false;
    bool in_class_field_init = false;
    int loop_depth = 0, switch_depth = 0;
    std::vector<std::pair<std::u16string, bool>> labels;   // name, is_loop
    bool saw_arguments = false;
    bool saw_this = false;
  };
  std::vector<FnCtx> fns;
  // Private names declared by the classes being parsed (innermost last), to reject references to undeclared ones.
  std::vector<std::set<std::u16string>> private_scopes;
  std::vector<std::vector<std::pair<std::u16string, uint32_t>>> private_refs;

  Parser(Program& p, std::shared_ptr<const std::u16string> src)
      : prog(p), source(std::move(src)), lx(source->data(), static_cast<uint32_t>(source->size())) {}

  // ------------------------------------------------------------------ errors and tokens

  Node* fail(const std::string& msg, uint32_t pos = UINT32_MAX) {
    if (!failed) {
      failed = true;
      err.message = msg;
      uint32_t at_pos = pos == UINT32_MAX ? cur.start : pos;
      // line and column by rescanning: errors are rare, so this is cheap enough
      uint32_t line = 1, line_start = 0;
      const std::u16string& s = *source;
      for (uint32_t i = 0; i < at_pos && i < s.size(); ++i) {
        if (s[i] == 0x0A || s[i] == 0x2028 || s[i] == 0x2029) { ++line; line_start = i + 1; }
        else if (s[i] == 0x0D) { if (i + 1 < s.size() && s[i + 1] == 0x0A) ++i; ++line; line_start = i + 1; }
      }
      err.line = line;
      err.col = at_pos - line_start + 1;
    }
    return nullptr;
  }

  std::string describe(const Token& t) {
    switch (t.type) {
      case TT::End: return "end of input";
      case TT::Ident: case TT::Keyword: return "token '" + narrow(t.str) + "'";
      case TT::Number: return "number";
      case TT::String: return "string";
      case TT::Template: return "template string";
      case TT::Private: return "private name";
      default: return "token";
    }
  }

  static std::string narrow(const std::u16string& s) {
    std::string o;
    for (char16_t c : s) o += c < 0x80 ? static_cast<char>(c) : '?';
    return o;
  }

  Node* unexpected() {
    if (!cur.error.empty()) return fail(cur.error);
    if (cur.type == TT::End) return fail("Unexpected end of input");
    if (cur.type == TT::Punct) {
      std::string p = narrow(std::u16string(source->substr(cur.start, cur.end - cur.start)));
      return fail("Unexpected token '" + p + "'");
    }
    if (cur.type == TT::Ident && strict && is_strict_reserved(cur.str)) return fail("Unexpected strict mode reserved word");
    return fail("Unexpected " + describe(cur));
  }

  void advance() {
    prev_end = cur.end;
    prev_line = cur.line;
    cur = lx.next();
  }

  bool at_punct(P p) const { return cur.type == TT::Punct && cur.punct == p; }
  bool at_kw(Kw k) const { return cur.type == TT::Keyword && cur.kw == k && !cur.escaped; }
  bool at_ident(const char16_t* name) const { return cur.type == TT::Ident && !cur.escaped && cur.str == name; }
  bool eat_punct(P p) { if (at_punct(p)) { advance(); return true; } return false; }
  bool eat_kw(Kw k) { if (at_kw(k)) { advance(); return true; } return false; }

  bool expect_punct(P p, const char* what) {
    if (at_punct(p)) { advance(); return true; }
    if (!cur.error.empty()) { fail(cur.error); return false; }
    fail(std::string("Unexpected ") + describe(cur) + ", expected " + what);
    return false;
  }

  Token peek() {
    auto st = lx.save();
    Token t = lx.next();
    lx.restore(st);
    return t;
  }

  bool consume_semicolon() {
    if (at_punct(P::Semi)) { advance(); return true; }
    if (at_punct(P::RBrace) || cur.type == TT::End || cur.nl_before) return true;
    unexpected();
    return false;
  }

  Node* make(NK k) { return prog.make(k, cur.start, cur.line); }
  Node* make_at(NK k, const Token& t) { return prog.make(k, t.start, t.line); }

  FnCtx& fn() { return fns.back(); }

  static bool is_strict_reserved(const std::u16string& s) {
    return s == u"implements" || s == u"interface" || s == u"let" || s == u"package" || s == u"private" || s == u"protected" || s == u"public" ||
           s == u"static" || s == u"yield";
  }

  // ------------------------------------------------------------------ identifiers

  // Is `t` usable as an identifier reference / binding name here?
  bool ident_ok(const Token& t, bool binding) {
    if (t.type != TT::Ident) return false;
    if (strict && is_strict_reserved(t.str)) return false;
    if (t.str == u"yield" && (fn().is_generator || strict)) return false;
    if (t.str == u"await" && fn().is_async) return false;
    if (binding && strict && (t.str == u"eval" || t.str == u"arguments")) return false;
    return true;
  }

  Node* parse_identifier_name_token() {
    Node* n = make(NK::Ident);
    n->str = cur.str;
    advance();
    return n;
  }

  Node* parse_binding_identifier() {
    if (cur.type != TT::Ident || !cur.error.empty()) return unexpected();
    if (!ident_ok(cur, true)) {
      if (strict && (cur.str == u"eval" || cur.str == u"arguments")) return fail("Unexpected eval or arguments in strict mode");
      return fail("Unexpected reserved word '" + narrow(cur.str) + "'");
    }
    return parse_identifier_name_token();
  }

  Node* parse_identifier_reference() {
    if (cur.type != TT::Ident || !cur.error.empty()) return unexpected();
    if (!ident_ok(cur, false)) return fail("Unexpected reserved word '" + narrow(cur.str) + "'");
    Node* n = parse_identifier_name_token();
    if (n->str == u"arguments") {
      for (size_t i = fns.size(); i-- > 0;) {
        fns[i].saw_arguments = true;
        if (!fns[i].is_arrow) break;
      }
    }
    return n;
  }

  // ------------------------------------------------------------------ programs and statements

  Node* parse_program_body(Node* prog_node) {
    bool directives = true;
    while (cur.type != TT::End) {
      if (directives) {
        if (cur.type == TT::String && !cur.escaped) {
          Token t = cur;
          std::u16string raw(source->substr(t.start + 1, t.end - t.start - 2));
          Token nx = peek();
          bool is_directive = nx.type == TT::End || (nx.type == TT::Punct && (nx.punct == P::Semi || nx.punct == P::RBrace)) || nx.nl_before;
          if (is_directive && raw == u"use strict") { strict = true; lx.strict = true; prog_node->flags |= kFlagUseStrict; }
          if (!is_directive) directives = false;
        } else {
          directives = false;
        }
      }
      module_top_item = module_mode;
      Node* s = parse_statement_list_item();
      module_top_item = false;
      if (!s) return nullptr;
      prog_node->list.push_back(s);
      if (failed) return nullptr;
    }
    return prog_node;
  }

  bool at_let_declaration() {
    if (!at_ident(u"let")) return false;
    Token nx = peek();
    if (nx.type == TT::Ident) return true;
    if (nx.type == TT::Punct && (nx.punct == P::LBracket || nx.punct == P::LBrace)) return true;
    return false;
  }

  Node* parse_statement_list_item() {
    if (module_top_item) {
      module_top_item = false;
      if (at_kw(Kw::Export)) return parse_module_export();
      if (at_kw(Kw::Import)) {
        Token nx = peek();
        if (!(nx.type == TT::Punct && (nx.punct == P::LParen || nx.punct == P::Dot))) return parse_module_import();
      }
    }
    if (cur.type == TT::Keyword && !cur.escaped) {
      if (cur.kw == Kw::Function) return parse_function_declaration(false);
      if (cur.kw == Kw::Class) return parse_class(true);
      if (cur.kw == Kw::Const) return parse_var_statement(kFlagConst);
    }
    if (at_ident(u"let") && at_let_declaration()) return parse_var_statement(kFlagLet);
    if (at_ident(u"async")) {
      Token nx = peek();
      if (nx.type == TT::Keyword && nx.kw == Kw::Function && !nx.nl_before) return parse_function_declaration(true);
    }
    return parse_statement();
  }

  Node* parse_var_statement(uint32_t kind) {
    Node* n = parse_var_declarations(kind, false);
    if (!n) return nullptr;
    if (!consume_semicolon()) return nullptr;
    return n;
  }

  Node* parse_var_declarations(uint32_t kind, bool in_for_head) {
    Node* decl = make(NK::VarDecl);
    decl->flags = kind;
    advance();   // var / let / const
    for (;;) {
      Node* d = make(NK::Declarator);
      Node* target = parse_binding_target();
      if (!target) return nullptr;
      if ((kind & (kFlagLet | kFlagConst)) && target->kind == NK::Ident && target->str == u"let") return fail("let is disallowed as a lexically bound name");
      d->a = target;
      if (eat_punct(P::Assign)) {
        bool saved = no_in;
        no_in = in_for_head;
        d->b = parse_assignment();
        no_in = saved;
        if (!d->b) return nullptr;
        name_anonymous(d->b, target);
      } else if (!in_for_head) {
        if (kind == kFlagConst) return fail("Missing initializer in const declaration");
        if (target->kind != NK::Ident) return fail("Missing initializer in destructuring declaration");
      }
      decl->list.push_back(d);
      if (!eat_punct(P::Comma)) break;
    }
    return decl;
  }

  // A function or class written without a name, assigned to a binding, takes that binding's name.
  void name_anonymous(Node* value, Node* target) {
    if (!value || !target || target->kind != NK::Ident) return;
    if (value->has(kFlagParenthesized)) return;
    if ((value->kind == NK::FunctionExpr || value->kind == NK::ArrowFunction) && value->fn && value->fn->name.empty()) { value->fn->name = target->str; value->fn->name_inferred = true; }
    else if (value->kind == NK::ClassExpr && value->str.empty() && !value->has(kFlagAnonymousFn)) value->flags |= kFlagAnonymousFn, value->str2 = target->str;
  }

  Node* parse_binding_target() {
    if (at_punct(P::LBracket)) return parse_array_binding();
    if (at_punct(P::LBrace)) return parse_object_binding();
    return parse_binding_identifier();
  }

  Node* parse_binding_element() {
    Node* t = parse_binding_target();
    if (!t) return nullptr;
    if (at_punct(P::Assign)) {
      Node* a = make(NK::AssignPattern);
      advance();
      a->a = t;
      bool saved = no_in;
      no_in = false;
      a->b = parse_assignment();
      no_in = saved;
      if (!a->b) return nullptr;
      name_anonymous(a->b, t);
      return a;
    }
    return t;
  }

  Node* parse_array_binding() {
    Node* n = make(NK::ArrayPattern);
    advance();   // [
    while (!at_punct(P::RBracket)) {
      if (at_punct(P::Comma)) { advance(); n->list.push_back(nullptr); continue; }
      if (at_punct(P::Ellipsis)) {
        Node* r = make(NK::Rest);
        advance();
        r->a = parse_binding_target();
        if (!r->a) return nullptr;
        n->list.push_back(r);
        if (!at_punct(P::RBracket)) return fail("Rest element must be last element");
        break;
      }
      Node* e = parse_binding_element();
      if (!e) return nullptr;
      n->list.push_back(e);
      if (!at_punct(P::RBracket)) { if (!expect_punct(P::Comma, "','")) return nullptr; }
    }
    if (!expect_punct(P::RBracket, "']'")) return nullptr;
    return n;
  }

  Node* parse_object_binding() {
    Node* n = make(NK::ObjectPattern);
    advance();   // {
    while (!at_punct(P::RBrace)) {
      if (at_punct(P::Ellipsis)) {
        Node* r = make(NK::Rest);
        advance();
        r->a = parse_binding_identifier();
        if (!r->a) return nullptr;
        n->list.push_back(r);
        if (!at_punct(P::RBrace)) return fail("Rest element must be last element");
        break;
      }
      Node* prop = make(NK::Property);
      bool computed = false;
      Token key_tok = cur;
      Node* key = parse_property_key(&computed, false);
      if (!key) return nullptr;
      prop->a = key;
      if (computed) prop->flags |= kFlagComputed;
      if (eat_punct(P::Colon)) {
        prop->b = parse_binding_element();
        if (!prop->b) return nullptr;
      } else {
        // shorthand: { a } or { a = 1 }
        if (computed || key_tok.type != TT::Ident) return unexpected();
        if (!ident_ok(key_tok, true)) return fail("Unexpected reserved word '" + narrow(key_tok.str) + "'", key_tok.start);
        Node* id = prog.make(NK::Ident, key_tok.start, key_tok.line);
        id->str = key_tok.str;
        prop->flags |= kFlagShorthand;
        if (at_punct(P::Assign)) {
          Node* a = make(NK::AssignPattern);
          advance();
          a->a = id;
          a->b = parse_assignment();
          if (!a->b) return nullptr;
          name_anonymous(a->b, id);
          prop->b = a;
        } else {
          prop->b = id;
        }
      }
      n->list.push_back(prop);
      if (!at_punct(P::RBrace)) { if (!expect_punct(P::Comma, "','")) return nullptr; }
    }
    if (!expect_punct(P::RBrace, "'}'")) return nullptr;
    return n;
  }

  Node* parse_block_statement() {
    Node* b = make(NK::Block);
    if (!expect_punct(P::LBrace, "'{'")) return nullptr;
    while (!at_punct(P::RBrace)) {
      if (cur.type == TT::End) return unexpected();
      Node* s = parse_statement_list_item();
      if (!s) return nullptr;
      b->list.push_back(s);
    }
    advance();   // }
    return b;
  }

  Node* parse_statement() {
    if (!cur.error.empty()) return fail(cur.error);
    switch (cur.type) {
      case TT::Punct:
        if (cur.punct == P::LBrace) return parse_block_statement();
        if (cur.punct == P::Semi) { Node* n = make(NK::Empty); advance(); return n; }
        break;
      case TT::Keyword:
        if (cur.escaped) return fail("Keyword must not contain escaped characters");
        switch (cur.kw) {
          case Kw::Var: return parse_var_statement(kFlagVar);
          case Kw::If: return parse_if();
          case Kw::For: return parse_for();
          case Kw::While: return parse_while();
          case Kw::Do: return parse_do_while();
          case Kw::Return: return parse_return();
          case Kw::Break: case Kw::Continue: return parse_break_continue();
          case Kw::Throw: return parse_throw();
          case Kw::Try: return parse_try();
          case Kw::Switch: return parse_switch();
          case Kw::Debugger: { Node* n = make(NK::Debugger); advance(); if (!consume_semicolon()) return nullptr; return n; }
          case Kw::With:
            if (strict) return fail("Strict mode code may not include a with statement");
            return fail("The 'with' statement is not supported");
          case Kw::Function:
            // a function declaration in statement position (Annex B allows `if (x) function f(){}`)
            return fail("In strict mode code, functions can only be declared at top level or inside a block");
          case Kw::Class: return fail("Unexpected token 'class'");
          case Kw::Const: return fail("Unexpected token 'const'");
          default: break;
        }
        break;
      case TT::Ident: {
        // a labelled statement?
        if (ident_ok(cur, false) || cur.str == u"yield" || cur.str == u"await") {
          Token nx = peek();
          if (nx.type == TT::Punct && nx.punct == P::Colon && ident_ok(cur, false)) return parse_labelled();
        }
        if (at_ident(u"let") && strict) return fail("Unexpected strict mode reserved word");
        break;
      }
      default: break;
    }
    // expression statement
    Node* es = make(NK::ExprStmt);
    es->a = parse_expression();
    if (!es->a) return nullptr;
    if (!consume_semicolon()) return nullptr;
    return es;
  }

  Node* parse_labelled() {
    Node* l = make(NK::Labeled);
    l->str = cur.str;
    for (auto& e : fn().labels)
      if (e.first == l->str) return fail("Label '" + narrow(l->str) + "' has already been declared");
    advance();
    advance();   // :
    // is the labelled statement a loop? (continue needs to know)
    bool is_loop = cur.type == TT::Keyword && (cur.kw == Kw::For || cur.kw == Kw::While || cur.kw == Kw::Do);
    if (!is_loop && cur.type == TT::Ident) {
      // nested labels: a: b: while(...) — the inner label chain ends at a loop
      Token t = cur;
      auto st = lx.save();
      Token c2 = cur;
      while (c2.type == TT::Ident) {
        Token nx = lx.next();
        if (nx.type == TT::Punct && nx.punct == P::Colon) { c2 = lx.next(); } else break;
      }
      is_loop = c2.type == TT::Keyword && (c2.kw == Kw::For || c2.kw == Kw::While || c2.kw == Kw::Do);
      lx.restore(st);
      cur = t;
    }
    fn().labels.emplace_back(l->str, is_loop);
    if (at_kw(Kw::Function) && !strict) {
      // Annex B: a labelled function declaration
      l->a = parse_function_declaration(false);
    } else {
      l->a = parse_statement();
    }
    fn().labels.pop_back();
    if (!l->a) return nullptr;
    return l;
  }

  Node* parse_paren_condition() {
    if (!expect_punct(P::LParen, "'('")) return nullptr;
    Node* e = parse_expression();
    if (!e) return nullptr;
    if (!expect_punct(P::RParen, "')'")) return nullptr;
    return e;
  }

  Node* parse_sub_statement() {
    // the body of if/for/while: a statement, but a lexical declaration is an error
    if (at_kw(Kw::Class) || at_kw(Kw::Const)) return fail("Lexical declaration cannot appear in a single-statement context");
    if (at_ident(u"let")) {
      Token nx = peek();
      if (nx.type == TT::Punct && nx.punct == P::LBracket) return fail("Lexical declaration cannot appear in a single-statement context");
    }
    return parse_statement();
  }

  Node* parse_if() {
    Node* n = make(NK::If);
    advance();
    n->a = parse_paren_condition();
    if (!n->a) return nullptr;
    n->b = parse_if_branch();
    if (!n->b) return nullptr;
    if (eat_kw(Kw::Else)) {
      n->c = parse_if_branch();
      if (!n->c) return nullptr;
    }
    return n;
  }

  Node* parse_if_branch() {
    if (at_kw(Kw::Function) && !strict) {
      // Annex B.3.4: `if (x) function f() {}` behaves as if the declaration were in a block
      Node* b = make(NK::Block);
      Node* f = parse_function_declaration(false);
      if (!f) return nullptr;
      b->list.push_back(f);
      return b;
    }
    return parse_sub_statement();
  }

  Node* parse_while() {
    Node* n = make(NK::While);
    advance();
    n->a = parse_paren_condition();
    if (!n->a) return nullptr;
    ++fn().loop_depth;
    n->b = parse_sub_statement();
    --fn().loop_depth;
    if (!n->b) return nullptr;
    return n;
  }

  Node* parse_do_while() {
    Node* n = make(NK::DoWhile);
    advance();
    ++fn().loop_depth;
    n->a = parse_sub_statement();
    --fn().loop_depth;
    if (!n->a) return nullptr;
    if (!at_kw(Kw::While)) return unexpected();
    advance();
    n->b = parse_paren_condition();
    if (!n->b) return nullptr;
    eat_punct(P::Semi);   // a semicolon is always optional after do-while
    return n;
  }

  Node* parse_for() {
    Token for_tok = cur;
    Node* n = make(NK::For);
    advance();
    bool is_await = false;
    if (at_ident(u"await")) {
      if (!fn().is_async) return fail("Unexpected reserved word 'await'");
      is_await = true;
      advance();
    }
    if (!expect_punct(P::LParen, "'('")) return nullptr;
    Node* init = nullptr;
    bool is_decl = false;
    if (at_punct(P::Semi)) {
      // no init
    } else if (at_kw(Kw::Var) || at_kw(Kw::Const) || (at_ident(u"let") && for_let_is_decl())) {
      uint32_t kind = at_kw(Kw::Var) ? kFlagVar : at_kw(Kw::Const) ? kFlagConst : kFlagLet;
      init = parse_var_declarations(kind, true);
      if (!init) return nullptr;
      is_decl = true;
    } else {
      bool saved = no_in;
      no_in = true;
      Token start_tok = cur;
      init = parse_expression();
      no_in = saved;
      if (!init) return nullptr;
      (void)start_tok;
    }
    bool is_of = at_ident(u"of");
    bool is_in = at_kw(Kw::In);
    if (init && (is_of || is_in)) {
      if (is_decl) {
        if (init->list.size() != 1) return fail("Invalid left-hand side in for-loop: Must have a single binding.");
        Node* d = init->list[0];
        if (d->b) {
          // `for (var x = 1 in y)` is legal sloppy-mode legacy for simple var bindings only
          if (!(is_in && !strict && (init->flags & kFlagVar) && d->a->kind == NK::Ident)) return fail(std::string("for-") + (is_of ? "of" : "in") + " loop variable declaration may not have an initializer.");
        }
        if ((init->flags & kFlagLet) && d->a->kind == NK::Ident && d->a->str == u"let") return fail("let is disallowed as a lexically bound name");
      } else {
        if (is_of && init->kind == NK::Ident && init->str == u"async" && !init->has(kFlagParenthesized)) {
          // `for (async of ...)` is forbidden
          return fail("The left-hand side of a for-of loop may not be 'async'.");
        }
        init = to_pattern(init, false, "Invalid left-hand side in for-loop");
        if (!init) return nullptr;
      }
      n->kind = is_of ? NK::ForOf : NK::ForIn;
      if (is_await && !is_of) return fail("Unexpected token 'in'");
      if (is_await) n->flags |= kFlagAwait;
      advance();   // of / in
      n->a = init;
      bool saved = no_in;
      no_in = false;
      n->b = is_of ? parse_assignment() : parse_expression();
      no_in = saved;
      if (!n->b) return nullptr;
      if (!expect_punct(P::RParen, "')'")) return nullptr;
      ++fn().loop_depth;
      n->c = parse_sub_statement();
      --fn().loop_depth;
      if (!n->c) return nullptr;
      return n;
    }
    if (is_await) return fail("Unexpected token", for_tok.start);
    if (is_decl) {
      for (Node* d : init->list) {
        if (!d->b && ((init->flags & kFlagConst) || d->a->kind != NK::Ident)) return fail("Missing initializer in declaration");
      }
    }
    n->a = init;
    if (!expect_punct(P::Semi, "';'")) return nullptr;
    if (!at_punct(P::Semi)) {
      n->b = parse_expression();
      if (!n->b) return nullptr;
    }
    if (!expect_punct(P::Semi, "';'")) return nullptr;
    if (!at_punct(P::RParen)) {
      n->c = parse_expression();
      if (!n->c) return nullptr;
    }
    if (!expect_punct(P::RParen, "')'")) return nullptr;
    ++fn().loop_depth;
    n->d = parse_sub_statement();
    --fn().loop_depth;
    if (!n->d) return nullptr;
    return n;
  }

  bool for_let_is_decl() {
    Token nx = peek();
    if (nx.type == TT::Ident) return nx.str != u"in" && nx.str != u"of" ? true : nx.str == u"of" && false;
    if (nx.type == TT::Punct && (nx.punct == P::LBracket || nx.punct == P::LBrace)) return true;
    return false;
  }

  Node* parse_return() {
    if (!fn().allow_return) return fail("Illegal return statement");
    Node* n = make(NK::Return);
    advance();
    if (!at_punct(P::Semi) && !at_punct(P::RBrace) && cur.type != TT::End && !cur.nl_before) {
      n->a = parse_expression();
      if (!n->a) return nullptr;
    }
    if (!consume_semicolon()) return nullptr;
    return n;
  }

  Node* parse_break_continue() {
    bool is_break = cur.kw == Kw::Break;
    Node* n = make(is_break ? NK::Break : NK::Continue);
    advance();
    if (cur.type == TT::Ident && !cur.nl_before) {
      n->str = cur.str;
      bool found = false, loop = false;
      for (auto& e : fn().labels)
        if (e.first == n->str) { found = true; loop = e.second; }
      if (!found) return fail("Undefined label '" + narrow(n->str) + "'");
      if (!is_break && !loop) return fail("Illegal continue statement: '" + narrow(n->str) + "' does not denote an iteration statement");
      advance();
    } else if (is_break) {
      if (fn().loop_depth == 0 && fn().switch_depth == 0) return fail("Illegal break statement");
    } else if (fn().loop_depth == 0) {
      return fail("Illegal continue statement: no surrounding iteration statement");
    }
    if (!consume_semicolon()) return nullptr;
    return n;
  }

  Node* parse_throw() {
    Node* n = make(NK::Throw);
    advance();
    if (cur.nl_before) return fail("Illegal newline after throw");
    n->a = parse_expression();
    if (!n->a) return nullptr;
    if (!consume_semicolon()) return nullptr;
    return n;
  }

  Node* parse_try() {
    Node* n = make(NK::Try);
    advance();
    if (!at_punct(P::LBrace)) return unexpected();
    n->a = parse_block_statement();
    if (!n->a) return nullptr;
    if (at_kw(Kw::Catch)) {
      advance();
      n->flags |= kFlagHasCatch;
      if (eat_punct(P::LParen)) {
        n->b = parse_binding_target();
        if (!n->b) return nullptr;
        if (!expect_punct(P::RParen, "')'")) return nullptr;
      }
      if (!at_punct(P::LBrace)) return unexpected();
      n->c = parse_block_statement();
      if (!n->c) return nullptr;
    }
    if (at_kw(Kw::Finally)) {
      advance();
      if (!at_punct(P::LBrace)) return unexpected();
      n->d = parse_block_statement();
      if (!n->d) return nullptr;
    }
    if (!(n->flags & kFlagHasCatch) && !n->d) return fail("Missing catch or finally after try");
    return n;
  }

  Node* parse_switch() {
    Node* n = make(NK::Switch);
    advance();
    n->a = parse_paren_condition();
    if (!n->a) return nullptr;
    if (!expect_punct(P::LBrace, "'{'")) return nullptr;
    bool saw_default = false;
    ++fn().switch_depth;
    while (!at_punct(P::RBrace)) {
      Node* c = make(NK::Case);
      if (at_kw(Kw::Case)) {
        advance();
        c->a = parse_expression();
        if (!c->a) return nullptr;
      } else if (at_kw(Kw::Default)) {
        if (saw_default) return fail("More than one default clause in switch statement");
        saw_default = true;
        advance();
        c->flags |= kFlagDefaultCase;
      } else {
        return unexpected();
      }
      if (!expect_punct(P::Colon, "':'")) return nullptr;
      while (!at_punct(P::RBrace) && !at_kw(Kw::Case) && !at_kw(Kw::Default)) {
        if (cur.type == TT::End) return unexpected();
        Node* s = parse_statement_list_item();
        if (!s) return nullptr;
        c->list.push_back(s);
      }
      n->list.push_back(c);
    }
    --fn().switch_depth;
    advance();   // }
    return n;
  }

  // ------------------------------------------------------------------ functions

  Node* parse_function_declaration(bool is_async) {
    uint32_t start = cur.start;
    uint32_t line = cur.line;
    if (is_async) advance();   // async
    advance();                 // function
    bool is_gen = eat_punct(P::Star);
    Node* n = prog.make(NK::FunctionDecl, start, line);
    if (!(cur.type == TT::Ident)) return unexpected();
    // the declared name is bound in the enclosing scope, so the enclosing context's rules apply
    if (!ident_ok(cur, true)) return fail("Unexpected reserved word '" + narrow(cur.str) + "'");
    std::u16string name = cur.str;
    advance();
    FuncInfo* fi = parse_function_rest(start, line, name, FuncKind::Normal, is_async, is_gen, false);
    if (!fi) return nullptr;
    n->fn = fi;
    return n;
  }

  Node* parse_function_expression(bool is_async) {
    uint32_t start = cur.start;
    uint32_t line = cur.line;
    if (is_async) advance();
    advance();   // function
    bool is_gen = eat_punct(P::Star);
    Node* n = prog.make(NK::FunctionExpr, start, line);
    std::u16string name;
    if (cur.type == TT::Ident) {
      // the name of a function expression is bound inside it: generator/async-ness of THIS function decides
      bool ok = !(strict && is_strict_reserved(cur.str)) && !(cur.str == u"yield" && (is_gen || strict)) && !(cur.str == u"await" && is_async) &&
                !(strict && (cur.str == u"eval" || cur.str == u"arguments"));
      if (!ok) return fail("Unexpected reserved word '" + narrow(cur.str) + "'");
      name = cur.str;
      advance();
    }
    FuncInfo* fi = parse_function_rest(start, line, name, FuncKind::Normal, is_async, is_gen, true);
    if (!fi) return nullptr;
    n->fn = fi;
    return n;
  }

  // Parses `( params ) { body }`. The name and `function` keyword are already consumed.
  FuncInfo* parse_function_rest(uint32_t start, uint32_t line, const std::u16string& name, FuncKind kind, bool is_async, bool is_gen, bool is_expr) {
    FuncInfo* fi = prog.make_func();
    fi->name = name;
    fi->kind = kind;
    fi->is_async = is_async;
    fi->is_generator = is_gen;
    fi->is_expression = is_expr;
    fi->src_start = start;
    fi->line = line;
    FnCtx ctx;
    ctx.info = fi;
    ctx.is_async = is_async;
    ctx.is_generator = is_gen;
    ctx.allow_return = true;
    ctx.allow_new_target = true;
    if (kind == FuncKind::Method || kind == FuncKind::Getter || kind == FuncKind::Setter || kind == FuncKind::ClassConstructor ||
        kind == FuncKind::DerivedConstructor) {
      ctx.allow_super_prop = true;
      ctx.allow_super_call = kind == FuncKind::DerivedConstructor;
    }
    fns.push_back(ctx);
    bool saved_strict = strict, saved_no_in = no_in;
    no_in = false;
    bool ok = parse_params_and_body(fi, saved_strict);
    no_in = saved_no_in;
    FnCtx done = fns.back();
    fns.pop_back();
    strict = saved_strict;
    lx.strict = saved_strict;
    if (!ok) return nullptr;
    fi->uses_arguments = done.saw_arguments;
    fi->uses_this = done.saw_this;
    return fi;
  }

  bool parse_params_and_body(FuncInfo* fi, bool outer_strict) {
    if (!expect_punct(P::LParen, "'('")) return false;
    if (!parse_formal_parameters(fi, P::RParen)) return false;
    if (!expect_punct(P::RParen, "')'")) return false;
    return parse_function_body(fi, outer_strict);
  }

  bool parse_formal_parameters(FuncInfo* fi, P terminator) {
    bool seen_default_or_rest = false;
    uint32_t length = 0;
    while (!at_punct(terminator)) {
      Node* p;
      if (at_punct(P::Ellipsis)) {
        Node* r = make(NK::Rest);
        advance();
        r->a = parse_binding_target();
        if (!r->a) return false;
        if (at_punct(P::Assign)) { fail("Rest parameter may not have a default initializer"); return false; }
        p = r;
        fi->has_simple_params = false;
        seen_default_or_rest = true;
        fi->params.push_back(p);
        if (!at_punct(terminator)) { fail("Rest parameter must be last formal parameter"); return false; }
        break;
      }
      p = parse_binding_element();
      if (!p) return false;
      if (p->kind != NK::Ident) fi->has_simple_params = false;
      if (p->kind == NK::AssignPattern) seen_default_or_rest = true;
      if (!seen_default_or_rest) ++length;
      fi->params.push_back(p);
      if (!at_punct(terminator)) { if (!expect_punct(P::Comma, "','")) return false; }
    }
    fi->length = length;
    return true;
  }

  static void collect_bound_names(Node* n, std::vector<std::pair<std::u16string, uint32_t>>& out) {
    if (!n) return;
    switch (n->kind) {
      case NK::Ident: out.emplace_back(n->str, n->pos); break;
      case NK::AssignPattern: collect_bound_names(n->a, out); break;
      case NK::Rest: collect_bound_names(n->a, out); break;
      case NK::ArrayPattern: for (Node* e : n->list) collect_bound_names(e, out); break;
      case NK::ObjectPattern: for (Node* p : n->list) collect_bound_names(p->kind == NK::Rest ? p->a : p->b, out); break;
      default: break;
    }
  }

  bool parse_function_body(FuncInfo* fi, bool outer_strict) {
    if (!at_punct(P::LBrace)) { unexpected(); return false; }
    Node* body = make(NK::Block);
    advance();   // {
    bool directives = true;
    bool became_strict = false;
    while (!at_punct(P::RBrace)) {
      if (cur.type == TT::End) { unexpected(); return false; }
      if (directives) {
        if (cur.type == TT::String && !cur.escaped) {
          Token t = cur;
          std::u16string raw(source->substr(t.start + 1, t.end - t.start - 2));
          Token nx = peek();
          bool is_directive = nx.type == TT::End || (nx.type == TT::Punct && (nx.punct == P::Semi || nx.punct == P::RBrace)) || nx.nl_before;
          if (is_directive && raw == u"use strict") {
            if (!fi->has_simple_params) { fail("Illegal 'use strict' directive in function with non-simple parameter list"); return false; }
            strict = true;
            lx.strict = true;
            became_strict = true;
          }
          if (!is_directive) directives = false;
        } else {
          directives = false;
        }
      }
      Node* s = parse_statement_list_item();
      if (!s) return false;
      body->list.push_back(s);
    }
    fi->src_end = cur.end;
    advance();   // }
    fi->body = body;
    fi->strict = strict;
    // parameter early errors
    std::vector<std::pair<std::u16string, uint32_t>> names;
    for (Node* p : fi->params) collect_bound_names(p, names);
    bool no_dups = strict || !fi->has_simple_params || fi->kind == FuncKind::Arrow || fi->kind == FuncKind::Method || fi->kind == FuncKind::ClassConstructor ||
                   fi->kind == FuncKind::DerivedConstructor || fi->kind == FuncKind::Getter || fi->kind == FuncKind::Setter;
    std::set<std::u16string> seen;
    for (auto& nm : names) {
      if (strict && (nm.first == u"eval" || nm.first == u"arguments")) { fail("Unexpected eval or arguments in strict mode", nm.second); return false; }
      if (strict && is_strict_reserved(nm.first)) { fail("Unexpected strict mode reserved word", nm.second); return false; }
      if (!seen.insert(nm.first).second && no_dups) { fail("Duplicate parameter name not allowed in this context", nm.second); return false; }
    }
    if (became_strict && !outer_strict && !fi->name.empty() && (fi->name == u"eval" || fi->name == u"arguments")) { fail("Unexpected eval or arguments in strict mode"); return false; }
    return true;
  }

  // ------------------------------------------------------------------ classes

  Node* parse_class(bool is_decl) {
    uint32_t start = cur.start;
    Node* n = prog.make(is_decl ? NK::ClassDecl : NK::ClassExpr, start, cur.line);
    advance();   // class
    bool saved_strict = strict;
    strict = true;
    lx.strict = true;
    if (cur.type == TT::Ident && !at_kw(Kw::Extends)) {
      if (!ident_ok(cur, true) && !(cur.str == u"yield" || cur.str == u"let" || cur.str == u"static")) {
        strict = saved_strict;
        lx.strict = saved_strict;
        return fail("Unexpected reserved word '" + narrow(cur.str) + "'");
      }
      if (strict && is_strict_reserved(cur.str)) { strict = saved_strict; lx.strict = saved_strict; return fail("Unexpected strict mode reserved word"); }
      n->str = cur.str;
      advance();
    } else if (is_decl) {
      strict = saved_strict;
      lx.strict = saved_strict;
      return unexpected();
    }
    if (eat_kw(Kw::Extends)) {
      n->a = parse_lhs_expression();
      if (!n->a) { strict = saved_strict; lx.strict = saved_strict; return nullptr; }
    }
    if (!expect_punct(P::LBrace, "'{'")) { strict = saved_strict; lx.strict = saved_strict; return nullptr; }
    private_scopes.emplace_back();
    private_refs.emplace_back();
    bool has_ctor = false;
    bool derived = n->a != nullptr;
    while (!at_punct(P::RBrace)) {
      if (cur.type == TT::End) { strict = saved_strict; lx.strict = saved_strict; return unexpected(); }
      if (eat_punct(P::Semi)) continue;
      Node* m = parse_class_member(derived, &has_ctor);
      if (!m) { strict = saved_strict; lx.strict = saved_strict; return nullptr; }
      n->list.push_back(m);
    }
    n->fn = nullptr;
    FuncInfo* src = prog.make_func();   // carries the class source range for toString
    src->src_start = start;
    src->src_end = cur.end;
    n->fn = src;
    advance();   // }
    // every #name referenced must be declared by an enclosing class
    auto declared = private_scopes.back();
    auto refs = private_refs.back();
    private_scopes.pop_back();
    private_refs.pop_back();
    for (auto& r : refs) {
      if (declared.count(r.first)) continue;
      if (!private_refs.empty()) private_refs.back().push_back(r);
      else { strict = saved_strict; lx.strict = saved_strict; return fail("Private field '#" + narrow(r.first) + "' must be declared in an enclosing class", r.second); }
    }
    strict = saved_strict;
    lx.strict = saved_strict;
    return n;
  }

  // The name of a property: identifier name, string, number, [computed], or (in classes) #private.
  Node* parse_property_key(bool* computed, bool allow_private) {
    *computed = false;
    if (!cur.error.empty()) return fail(cur.error);
    switch (cur.type) {
      case TT::Ident: case TT::Keyword: {
        Node* k = make(NK::String);
        k->str = cur.str;
        if (cur.type == TT::Ident) k->flags |= 0;   // an identifier name; shorthand uses it
        advance();
        return k;
      }
      case TT::String: {
        if (cur.legacy_octal && strict) return fail("Octal escape sequences are not allowed in strict mode.");
        Node* k = make(NK::String);
        k->str = cur.str;
        advance();
        return k;
      }
      case TT::Number: {
        if (cur.bigint) {
          // a numeric property name is its canonical string: {1n: x} is {"1": x}
          BigNum v;
          if (!big::string_to_bigint(cur.str, &v)) return fail("Invalid BigInt syntax");
          Node* k = make(NK::String);
          for (char c : big::to_string(v)) k->str += static_cast<char16_t>(c);
          advance();
          return k;
        }
        if (cur.legacy_octal && strict) return fail("Octal literals are not allowed in strict mode.");
        Node* k = make(NK::Number);
        k->num = cur.num;
        advance();
        return k;
      }
      case TT::Private: {
        if (!allow_private) return unexpected();
        Node* k = make(NK::PrivateName);
        k->str = cur.str;
        advance();
        return k;
      }
      case TT::Punct:
        if (cur.punct == P::LBracket) {
          advance();
          bool saved = no_in;
          no_in = false;
          Node* e = parse_assignment();
          no_in = saved;
          if (!e) return nullptr;
          if (!expect_punct(P::RBracket, "']'")) return nullptr;
          *computed = true;
          return e;
        }
        return unexpected();
      default:
        return unexpected();
    }
  }

  Node* parse_class_member(bool derived, bool* has_ctor) {
    Node* m = make(NK::Property);
    uint32_t start = cur.start;
    uint32_t line = cur.line;
    bool is_static = false, is_async = false, is_gen = false;
    uint32_t kind_flags = 0;
    // `static`
    if (at_ident(u"static")) {
      Token nx = peek();
      bool is_key = nx.type == TT::Punct && (nx.punct == P::LParen || nx.punct == P::Assign || nx.punct == P::Semi || nx.punct == P::RBrace);
      if (!is_key) {
        advance();
        is_static = true;
        if (at_punct(P::LBrace)) {
          // static initialisation block
          Node* sb = prog.make(NK::StaticBlock, start, line);
          FuncInfo* fi = prog.make_func();
          fi->kind = FuncKind::Method;
          fi->strict = true;
          fi->is_class_static_block = true;
          fi->src_start = cur.start;
          FnCtx ctx;
          ctx.info = fi;
          ctx.allow_super_prop = true;
          ctx.allow_new_target = true;
          fns.push_back(ctx);
          Node* body = make(NK::Block);
          advance();
          while (!at_punct(P::RBrace)) {
            if (cur.type == TT::End) { fns.pop_back(); return unexpected(); }
            Node* s = parse_statement_list_item();
            if (!s) { fns.pop_back(); return nullptr; }
            body->list.push_back(s);
          }
          fi->src_end = cur.end;
          advance();
          fns.pop_back();
          fi->body = body;
          sb->fn = fi;
          return sb;
        }
      }
    }
    if (at_ident(u"async")) {
      Token nx = peek();
      bool is_key = nx.type == TT::Punct && (nx.punct == P::LParen || nx.punct == P::Assign || nx.punct == P::Semi || nx.punct == P::RBrace);
      if (!is_key && !nx.nl_before) { advance(); is_async = true; }
    }
    if (at_punct(P::Star)) { advance(); is_gen = true; }
    if (!is_async && !is_gen && (at_ident(u"get") || at_ident(u"set"))) {
      Token nx = peek();
      bool is_key = nx.type == TT::Punct && (nx.punct == P::LParen || nx.punct == P::Assign || nx.punct == P::Semi || nx.punct == P::RBrace);
      if (!is_key) {
        kind_flags = at_ident(u"get") ? kFlagGetter : kFlagSetter;
        advance();
      }
    }
    Token key_tok = cur;
    bool computed = false;
    Node* key = parse_property_key(&computed, true);
    if (!key) return nullptr;
    m->a = key;
    if (computed) m->flags |= kFlagComputed;
    if (is_static) m->flags |= kFlagStatic;
    if (key->kind == NK::PrivateName) {
      m->flags |= kFlagPrivate;
      if (key->str == u"constructor") return fail("Classes may not have a private field named '#constructor'");
      if (!private_scopes.back().insert(key->str).second) {
        // a getter and a setter may share a name
        bool pair_ok = false;
        (void)pair_ok;
      }
    }
    const bool key_is_ctor = !computed && !is_static && key->kind == NK::String && key->str == u"constructor";
    if (at_punct(P::LParen)) {
      // method / accessor
      FuncKind fk = kind_flags & kFlagGetter ? FuncKind::Getter : kind_flags & kFlagSetter ? FuncKind::Setter : FuncKind::Method;
      if (key_is_ctor) {
        if (kind_flags || is_async || is_gen) return fail("Class constructor may not be an accessor, generator or async");
        if (*has_ctor) return fail("A class may only have one constructor");
        *has_ctor = true;
        fk = derived ? FuncKind::DerivedConstructor : FuncKind::ClassConstructor;
      }
      if (!computed && is_static && key->kind == NK::String && key->str == u"prototype") return fail("Classes may not have a static property named 'prototype'");
      FuncInfo* fi = parse_function_rest(start, line, key->kind == NK::String && !computed ? key->str : std::u16string(), fk, is_async, is_gen, false);
      if (!fi) return nullptr;
      Node* f = prog.make(NK::FunctionExpr, start, line);
      f->fn = fi;
      m->b = f;
      m->flags |= kFlagMethod | kind_flags;
      if (key_is_ctor) m->flags |= kFlagProto;   // marks the constructor member
      if (fk == FuncKind::Getter && !fi->params.empty()) return fail("Getter must not have any formal parameters.", start);
      if (fk == FuncKind::Setter && (fi->params.size() != 1 || fi->params[0]->kind == NK::Rest)) return fail("Setter must have exactly one formal parameter.", start);
      return m;
    }
    // a field
    if (kind_flags || is_async || is_gen) return unexpected();
    if (!computed && key->kind == NK::String && (key->str == u"constructor" || (is_static && key->str == u"prototype"))) return fail("Classes may not have a field named 'constructor' or static 'prototype'");
    (void)key_tok;
    m->flags |= kFlagField;
    if (at_punct(P::Assign)) {
      advance();
      // the initialiser is evaluated as the body of a method of the class
      FuncInfo* fi = prog.make_func();
      fi->kind = FuncKind::ClassFieldInit;
      fi->strict = true;
      fi->expression_body = true;
      fi->src_start = cur.start;
      FnCtx ctx;
      ctx.info = fi;
      ctx.allow_super_prop = true;
      ctx.allow_new_target = true;
      ctx.in_class_field_init = true;
      fns.push_back(ctx);
      bool saved = no_in;
      no_in = false;
      Node* init = parse_assignment();
      no_in = saved;
      FnCtx done = fns.back();
      fns.pop_back();
      if (!init) return nullptr;
      fi->src_end = prev_end;
      fi->body = init;
      fi->uses_arguments = done.saw_arguments;
      if (done.saw_arguments) return fail("'arguments' is not allowed in class field initializer or static initialization block");
      if (key->kind == NK::String && !computed && (init->kind == NK::FunctionExpr || init->kind == NK::ArrowFunction || init->kind == NK::ClassExpr)) {
        if (init->kind == NK::ClassExpr) { if (init->str.empty()) init->str2 = key->str, init->flags |= kFlagAnonymousFn; }
        else if (init->fn && init->fn->name.empty() && !init->has(kFlagParenthesized)) { init->fn->name = key->str; init->fn->name_inferred = true; }
      }
      Node* f = prog.make(NK::FunctionExpr, fi->src_start, line);
      f->fn = fi;
      m->b = f;
    }
    if (!consume_semicolon()) return nullptr;
    return m;
  }

  // ------------------------------------------------------------------ expressions

  Node* parse_expression() {
    Node* e = parse_assignment();
    if (!e) return nullptr;
    if (at_punct(P::Comma)) {
      Node* seq = prog.make(NK::Sequence, e->pos, e->line);
      seq->list.push_back(e);
      while (eat_punct(P::Comma)) {
        Node* n = parse_assignment();
        if (!n) return nullptr;
        seq->list.push_back(n);
      }
      return seq;
    }
    return e;
  }

  static Op assign_op(P p, bool* is_assign) {
    *is_assign = true;
    switch (p) {
      case P::Assign: return Op::Plain;
      case P::PlusAssign: return Op::Add;
      case P::MinusAssign: return Op::Sub;
      case P::StarAssign: return Op::Mul;
      case P::SlashAssign: return Op::Div;
      case P::PercentAssign: return Op::Mod;
      case P::StarStarAssign: return Op::Exp;
      case P::ShlAssign: return Op::Shl;
      case P::ShrAssign: return Op::Shr;
      case P::UShrAssign: return Op::UShr;
      case P::AmpAssign: return Op::BitAnd;
      case P::PipeAssign: return Op::BitOr;
      case P::CaretAssign: return Op::BitXor;
      case P::AmpAmpAssign: return Op::And;
      case P::PipePipeAssign: return Op::Or;
      case P::QQAssign: return Op::Nullish;
      default: *is_assign = false; return Op::None;
    }
  }

  Node* parse_assignment() {
    if (!cur.error.empty()) return fail(cur.error);
    // yield
    if (at_ident(u"yield") && fn().is_generator) return parse_yield();
    // arrow functions: `x => ...`, `async x => ...`, `async (...) => ...`
    if (cur.type == TT::Ident && !cur.escaped) {
      Token nx = peek();
      if (nx.type == TT::Punct && nx.punct == P::Arrow && !nx.nl_before) {
        if (!ident_ok(cur, true) && !(cur.str == u"async")) return unexpected();
        return parse_arrow_single_param(false);
      }
      if (cur.str == u"async" && !nx.nl_before) {
        if (nx.type == TT::Ident) {
          // async x => ...
          auto st = lx.save();
          Token saved_cur = cur;
          Token a1 = lx.next();
          Token a2 = lx.next();
          lx.restore(st);
          cur = saved_cur;
          if (a2.type == TT::Punct && a2.punct == P::Arrow && !a2.nl_before && a1.type == TT::Ident) return parse_arrow_single_param(true);
        }
      }
    }
    uint32_t start_pos = cur.start;
    uint32_t start_line = cur.line;
    Token start_tok = cur;
    Node* lhs = parse_conditional();
    if (!lhs) return nullptr;
    if (cur.type == TT::Punct && cur.punct == P::Arrow) {
      // `(a, b) => ...` was parsed as a parenthesised expression (a cover grammar)
      if (cur.nl_before) return fail("Unexpected token '=>'");
      return arrow_from_cover(lhs, start_tok, false);
    }
    bool is_assign;
    Op op = cur.type == TT::Punct ? assign_op(cur.punct, &is_assign) : (is_assign = false, Op::None);
    if (!is_assign) return lhs;
    Node* a = prog.make(NK::Assign, start_pos, start_line);
    a->op = op;
    if (op == Op::Plain) {
      Node* target = to_pattern(lhs, false, "Invalid left-hand side in assignment");
      if (!target) return nullptr;
      a->a = target;
    } else {
      if (!is_simple_target(lhs)) return fail("Invalid left-hand side in assignment");
      a->a = lhs;
      if (strict && lhs->kind == NK::Ident && (lhs->str == u"eval" || lhs->str == u"arguments")) return fail("Unexpected eval or arguments in strict mode");
    }
    advance();
    a->b = parse_assignment();
    if (!a->b) return nullptr;
    if (op == Op::Plain || op == Op::And || op == Op::Or || op == Op::Nullish) name_anonymous(a->b, a->a->kind == NK::Ident ? a->a : nullptr);
    return a;
  }

  Node* parse_yield() {
    Node* y = make(NK::Yield);
    if (fn().in_class_field_init) return fail("Yield expression not allowed here");
    advance();
    if (cur.nl_before || at_punct(P::RParen) || at_punct(P::RBracket) || at_punct(P::RBrace) || at_punct(P::Comma) || at_punct(P::Semi) ||
        at_punct(P::Colon) || cur.type == TT::End || (at_kw(Kw::In)) || at_ident(u"of")) {
      return y;
    }
    if (eat_punct(P::Star)) y->flags |= kFlagDelegate;
    y->a = parse_assignment();
    if (!y->a) return nullptr;
    return y;
  }

  bool is_simple_target(Node* n) {
    if (n->kind == NK::Ident) return true;
    if (n->kind == NK::Member || n->kind == NK::Index) return !(n->flags & kFlagOptional);
    if (n->kind == NK::OptChain) return false;
    return false;
  }

  // Reinterpret an expression as an assignment/binding target (the cover grammar).
  Node* to_pattern(Node* n, bool binding, const char* msg) {
    if (!n) return nullptr;
    switch (n->kind) {
      case NK::Ident:
        if (strict && (n->str == u"eval" || n->str == u"arguments")) return fail("Unexpected eval or arguments in strict mode", n->pos);
        return n;
      case NK::Member: case NK::Index:
        if (binding) return fail(msg, n->pos);
        if (n->flags & kFlagOptional) return fail("Invalid left-hand side in assignment", n->pos);
        return n;
      case NK::ArrayPattern: case NK::ObjectPattern: case NK::AssignPattern: case NK::Rest:
        return n;
      case NK::ArrayLit: {
        if (n->has(kFlagParenthesized)) return fail(msg, n->pos);
        n->kind = NK::ArrayPattern;
        for (size_t i = 0; i < n->list.size(); ++i) {
          Node* e = n->list[i];
          if (!e) continue;
          if (e->kind == NK::Spread) {
            if (i + 1 != n->list.size() || (n->flags & kFlagTrailingComma)) return fail("Rest element must be last element", e->pos);
            e->kind = NK::Rest;
            Node* t = to_pattern(e->a, binding, msg);
            if (!t) return nullptr;
            if (t->kind == NK::AssignPattern) return fail("Invalid destructuring assignment target", e->pos);
            e->a = t;
            continue;
          }
          Node* t = to_pattern(e, binding, msg);
          if (!t) return nullptr;
          n->list[i] = t;
        }
        return n;
      }
      case NK::ObjectLit: {
        if (n->has(kFlagParenthesized)) return fail(msg, n->pos);
        n->kind = NK::ObjectPattern;
        for (size_t i = 0; i < n->list.size(); ++i) {
          Node* p = n->list[i];
          if (p->kind != NK::Property) return fail(msg, p->pos);
          if (p->flags & kFlagSpread) {
            if (i + 1 != n->list.size()) return fail("Rest element must be last element", p->pos);
            Node* r = prog.make(NK::Rest, p->pos, p->line);
            Node* t = to_pattern(p->a, binding, msg);
            if (!t) return nullptr;
            if (t->kind != NK::Ident && (binding || t->kind != NK::Member)) {
              if (!(t->kind == NK::Member || t->kind == NK::Index)) return fail("`...` must be followed by an assignable reference in assignment contexts", p->pos);
            }
            r->a = t;
            n->list[i] = r;
            continue;
          }
          if (p->flags & (kFlagMethod | kFlagGetter | kFlagSetter)) return fail("Invalid destructuring assignment target", p->pos);
          if (p->flags & kFlagShorthand) {
            // { a } or { a = 1 }
            Node* id = p->b->kind == NK::Assign ? p->b->a : p->b;
            if (id->kind != NK::Ident) return fail(msg, p->pos);
            if (strict && (id->str == u"eval" || id->str == u"arguments")) return fail("Unexpected eval or arguments in strict mode", id->pos);
            p->flags &= ~static_cast<uint32_t>(kFlagHasDefaultInit);
            if (p->b->kind == NK::Assign) {
              Node* ap = prog.make(NK::AssignPattern, p->b->pos, p->b->line);
              ap->a = id;
              ap->b = p->b->b;
              p->b = ap;
            }
            continue;
          }
          Node* t = to_pattern(p->b, binding, msg);
          if (!t) return nullptr;
          p->b = t;
        }
        return n;
      }
      case NK::Assign:
        if (n->op != Op::Plain || n->has(kFlagParenthesized)) return fail(msg, n->pos);
        {
          // `a = b` inside a pattern is a default: a was already converted to a target
          Node* ap = prog.make(NK::AssignPattern, n->pos, n->line);
          ap->a = n->a;
          ap->b = n->b;
          return ap;
        }
      default:
        return fail(msg, n->pos);
    }
  }

  Node* parse_arrow_single_param(bool is_async) {
    Token start_tok = cur;
    if (is_async) advance();
    Node* id = make(NK::Ident);
    id->str = cur.str;
    advance();
    Node* params_expr = id;
    // the `=>` is next; build the cover and convert
    std::vector<Node*> params = {params_expr};
    if (!at_punct(P::Arrow)) return unexpected();
    return finish_arrow(params, start_tok, is_async, false);
  }

  // The expression before `=>` was parsed as a parenthesised expression.
  Node* arrow_from_cover(Node* cover, const Token& start_tok, bool is_async) {
    std::vector<Node*> params;
    if (cover->kind == NK::Sequence && cover->has(kFlagParenthesized)) {
      params = cover->list;
    } else if (cover->kind == NK::ArrowFunction) {
      return fail("Unexpected token '=>'");
    } else if (cover->has(kFlagParenthesized) && cover->kind != NK::Ident) {
      params.push_back(cover);
    } else if (cover->has(kFlagParenthesized) && cover->kind == NK::Ident) {
      params.push_back(cover);
    } else if (cover->kind == NK::Call && is_async) {
      params = cover->list;
    } else if (cover->kind == NK::Ident && !cover->has(kFlagParenthesized)) {
      params.push_back(cover);
    } else if (cover->kind == NK::Sequence && !cover->has(kFlagParenthesized)) {
      return fail("Malformed arrow function parameter list");
    } else {
      return fail("Malformed arrow function parameter list");
    }
    return finish_arrow(params, start_tok, is_async, true);
  }

  Node* finish_arrow(std::vector<Node*>& cover_params, const Token& start_tok, bool is_async, bool from_cover) {
    (void)from_cover;
    FuncInfo* fi = prog.make_func();
    fi->kind = FuncKind::Arrow;
    fi->is_async = is_async;
    fi->src_start = start_tok.start;
    fi->line = start_tok.line;
    uint32_t length = 0;
    bool seen_def = false;
    for (size_t i = 0; i < cover_params.size(); ++i) {
      Node* p = cover_params[i];
      if (p == nullptr) return fail("Invalid arrow function parameter");
      if (p->kind == NK::Spread) {
        if (i + 1 != cover_params.size()) return fail("Rest parameter must be last formal parameter");
        p->kind = NK::Rest;
        Node* t = to_pattern(p->a, true, "Invalid destructuring target");
        if (!t) return nullptr;
        p->a = t;
        fi->has_simple_params = false;
        seen_def = true;
        fi->params.push_back(p);
        continue;
      }
      Node* t = to_pattern(p, true, "Invalid destructuring target");
      if (!t) return nullptr;
      if (t->kind != NK::Ident) fi->has_simple_params = false;
      if (t->kind == NK::AssignPattern) seen_def = true;
      if (!seen_def) ++length;
      if (t->kind == NK::Ident && is_async && t->str == u"await") return fail("Unexpected reserved word 'await'");
      if (t->kind == NK::Ident && !(ident_ok(t_as_token(t), true) || t->str == u"async")) {
        if (!(t->str == u"yield" && !strict && !fn().is_generator)) return fail("Unexpected reserved word '" + narrow(t->str) + "'", t->pos);
      }
      fi->params.push_back(t);
    }
    fi->length = length;
    // the arrow token
    if (!at_punct(P::Arrow)) return unexpected();
    if (cur.nl_before) return fail("Unexpected token '=>'");
    advance();
    FnCtx ctx;
    ctx.info = fi;
    ctx.is_async = is_async;
    ctx.is_generator = false;
    ctx.is_arrow = true;
    // an arrow inherits what the enclosing function allows
    FnCtx& outer = fns.back();
    ctx.allow_return = true;
    ctx.allow_super_prop = outer.allow_super_prop;
    ctx.allow_super_call = outer.allow_super_call;
    ctx.allow_new_target = outer.allow_new_target;
    ctx.in_class_field_init = outer.in_class_field_init;
    fns.push_back(ctx);
    bool saved_strict = strict;
    Node* n = prog.make(NK::ArrowFunction, start_tok.start, start_tok.line);
    n->fn = fi;
    if (at_punct(P::LBrace)) {
      bool saved_no_in = no_in;
      no_in = false;
      bool ok = parse_function_body(fi, saved_strict);
      no_in = saved_no_in;
      if (!ok) { fns.pop_back(); strict = saved_strict; lx.strict = saved_strict; return nullptr; }
    } else {
      fi->expression_body = true;
      Node* body = parse_assignment();
      if (!body) { fns.pop_back(); return nullptr; }
      fi->body = body;
      fi->src_end = prev_end;
      fi->strict = strict;
      std::vector<std::pair<std::u16string, uint32_t>> names;
      for (Node* p : fi->params) collect_bound_names(p, names);
      std::set<std::u16string> seen;
      for (auto& nm : names) {
        if (strict && (nm.first == u"eval" || nm.first == u"arguments")) { fns.pop_back(); return fail("Unexpected eval or arguments in strict mode", nm.second); }
        if (!seen.insert(nm.first).second) { fns.pop_back(); return fail("Duplicate parameter name not allowed in this context", nm.second); }
      }
    }
    FnCtx done = fns.back();
    fns.pop_back();
    strict = saved_strict;
    lx.strict = saved_strict;
    fi->uses_arguments = done.saw_arguments;
    fi->uses_this = done.saw_this;
    // `this`, `arguments`, `new.target` and `super` inside an arrow belong to the enclosing function
    if (done.saw_arguments) fn().saw_arguments = true;
    if (done.saw_this) fn().saw_this = true;
    return n;
  }

  Token t_as_token(Node* id) {
    Token t;
    t.type = TT::Ident;
    t.str = id->str;
    return t;
  }

  Node* parse_conditional() {
    Node* c = parse_binary(1);
    if (!c) return nullptr;
    if (!at_punct(P::Question)) return c;
    Node* n = prog.make(NK::Conditional, c->pos, c->line);
    advance();
    n->a = c;
    bool saved = no_in;
    no_in = false;
    n->b = parse_assignment();
    no_in = saved;
    if (!n->b) return nullptr;
    if (!expect_punct(P::Colon, "':'")) return nullptr;
    n->c = parse_assignment();
    if (!n->c) return nullptr;
    return n;
  }

  // Binary operator precedence; 0 = not a binary operator.
  int binary_prec(Op* op, bool* logical, bool* right_assoc) {
    *logical = false;
    *right_assoc = false;
    if (cur.type == TT::Keyword && !cur.escaped) {
      if (cur.kw == Kw::InstanceOf) { *op = Op::InstanceOf; return 9; }
      if (cur.kw == Kw::In && !no_in) { *op = Op::In; return 9; }
      return 0;
    }
    if (cur.type != TT::Punct) return 0;
    switch (cur.punct) {
      case P::QQ: *op = Op::Nullish; *logical = true; return 1;
      case P::PipePipe: *op = Op::Or; *logical = true; return 2;
      case P::AmpAmp: *op = Op::And; *logical = true; return 3;
      case P::Pipe: *op = Op::BitOr; return 4;
      case P::Caret: *op = Op::BitXor; return 5;
      case P::Amp: *op = Op::BitAnd; return 6;
      case P::EqEq: *op = Op::Eq; return 8;
      default: break;
    }
    return binary_prec_rest(op, right_assoc);
  }

  int binary_prec_rest(Op* op, bool* right_assoc) {
    switch (cur.punct) {
      case P::NotEq: *op = Op::Ne; return 8;
      case P::EqEqEq: *op = Op::StrictEq; return 8;
      case P::NotEqEq: *op = Op::StrictNe; return 8;
      case P::Lt: *op = Op::Lt; return 9;
      case P::Gt: *op = Op::Gt; return 9;
      case P::Le: *op = Op::Le; return 9;
      case P::Ge: *op = Op::Ge; return 9;
      case P::Shl: *op = Op::Shl; return 10;
      case P::Shr: *op = Op::Shr; return 10;
      case P::UShr: *op = Op::UShr; return 10;
      case P::Plus: *op = Op::Add; return 11;
      case P::Minus: *op = Op::Sub; return 11;
      case P::Star: *op = Op::Mul; return 12;
      case P::Slash: *op = Op::Div; return 12;
      case P::Percent: *op = Op::Mod; return 12;
      case P::StarStar: *op = Op::Exp; *right_assoc = true; return 13;
      default: return 0;
    }
  }

  Node* parse_binary(int min_prec) {
    // `#x in obj`
    Node* left;
    if (cur.type == TT::Private) {
      Token pt = cur;
      advance();
      if (!at_kw(Kw::In) || no_in) return unexpected();
      Node* pn = prog.make(NK::PrivateName, pt.start, pt.line);
      pn->str = pt.str;
      if (!private_refs.empty()) private_refs.back().emplace_back(pt.str, pt.start);
      else return fail("Private field '#" + narrow(pt.str) + "' must be declared in an enclosing class", pt.start);
      advance();   // in
      Node* rhs = parse_binary(10);
      if (!rhs) return nullptr;
      Node* n = prog.make(NK::PrivateIn, pt.start, pt.line);
      n->a = pn;
      n->b = rhs;
      left = n;
    } else {
      left = parse_unary();
      if (!left) return nullptr;
    }
    for (;;) {
      Op op = Op::None;
      bool logical = false, right_assoc = false;
      int prec = binary_prec(&op, &logical, &right_assoc);
      if (prec == 0 || prec < min_prec) break;
      if (op == Op::Exp && left->kind == NK::Unary && !left->has(kFlagParenthesized)) return fail("Unary operator used immediately before exponentiation expression. Parenthesis must be used to disambiguate operator precedence");
      if (op == Op::Exp && left->kind == NK::Await && !left->has(kFlagParenthesized)) return fail("Unary operator used immediately before exponentiation expression. Parenthesis must be used to disambiguate operator precedence");
      advance();
      Node* right = parse_binary(right_assoc ? prec : prec + 1);
      if (!right) return nullptr;
      Node* n = prog.make(logical ? NK::Logical : NK::Binary, left->pos, left->line);
      n->op = op;
      n->a = left;
      n->b = right;
      // `a ?? b || c` mixes without parentheses: a SyntaxError
      if (logical) {
        auto mixes = [&](Node* x) {
          return x->kind == NK::Logical && !x->has(kFlagParenthesized) && ((op == Op::Nullish) != (x->op == Op::Nullish));
        };
        if (mixes(left) || mixes(right)) return fail("Cannot mix ?? with || or && without parentheses");
      }
      left = n;
    }
    return left;
  }

  Node* parse_unary() {
    if (!cur.error.empty()) return fail(cur.error);
    if (cur.type == TT::Punct) {
      Op op = Op::None;
      switch (cur.punct) {
        case P::Minus: op = Op::Neg; break;
        case P::Plus: op = Op::Pos; break;
        case P::Bang: op = Op::Not; break;
        case P::Tilde: op = Op::BitNot; break;
        case P::PlusPlus: case P::MinusMinus: {
          Node* n = make(NK::Update);
          n->op = cur.punct == P::PlusPlus ? Op::Inc : Op::Dec;
          n->flags |= kFlagPrefix;
          advance();
          Node* t = parse_unary();
          if (!t) return nullptr;
          if (!is_simple_target(t)) return fail("Invalid left-hand side expression in prefix operation");
          if (strict && t->kind == NK::Ident && (t->str == u"eval" || t->str == u"arguments")) return fail("Unexpected eval or arguments in strict mode");
          n->a = t;
          return n;
        }
        default: break;
      }
      if (op != Op::None) {
        Node* n = make(NK::Unary);
        n->op = op;
        advance();
        n->a = parse_unary();
        if (!n->a) return nullptr;
        return n;
      }
    } else if (cur.type == TT::Keyword && !cur.escaped) {
      Op op = Op::None;
      if (cur.kw == Kw::TypeOf) op = Op::TypeOf;
      else if (cur.kw == Kw::Void) op = Op::Void;
      else if (cur.kw == Kw::Delete) op = Op::Delete;
      if (op != Op::None) {
        Node* n = make(NK::Unary);
        n->op = op;
        advance();
        n->a = parse_unary();
        if (!n->a) return nullptr;
        if (op == Op::Delete) {
          if (strict && n->a->kind == NK::Ident && !n->a->has(kFlagParenthesized)) return fail("Delete of an unqualified identifier in strict mode.");
          if (n->a->kind == NK::Member && n->a->b && n->a->b->kind == NK::PrivateName) return fail("Private fields can not be deleted");
        }
        return n;
      }
    } else if (cur.type == TT::Ident && cur.str == u"await" && !cur.escaped && fn().is_async) {
      if (fn().in_class_field_init) return fail("Unexpected reserved word 'await'");
      Node* n = make(NK::Await);
      advance();
      n->a = parse_unary();
      if (!n->a) return nullptr;
      return n;
    }
    return parse_postfix();
  }

  Node* parse_postfix() {
    Node* e = parse_lhs_expression();
    if (!e) return nullptr;
    if (cur.type == TT::Punct && (cur.punct == P::PlusPlus || cur.punct == P::MinusMinus) && !cur.nl_before) {
      if (!is_simple_target(e)) return fail("Invalid left-hand side expression in postfix operation");
      if (strict && e->kind == NK::Ident && (e->str == u"eval" || e->str == u"arguments")) return fail("Unexpected eval or arguments in strict mode");
      Node* n = prog.make(NK::Update, e->pos, e->line);
      n->op = cur.punct == P::PlusPlus ? Op::Inc : Op::Dec;
      n->a = e;
      advance();
      return n;
    }
    return e;
  }

  // member / call / new chains, tagged templates and optional chaining
  Node* parse_lhs_expression() {
    Node* e;
    Token start_tok = cur;
    if (at_kw(Kw::New)) {
      e = parse_new();
    } else if (at_kw(Kw::Super)) {
      e = parse_super();
    } else if (at_kw(Kw::Import)) {
      // import(x) is a call of a function the page defines (the module's own wrapper shadows it with one that knows the
      // module's URL); import.meta only exists in a module, where it is the wrapper's third parameter.
      Token nx = peek();
      if (nx.type == TT::Punct && nx.punct == P::LParen) {
        e = make(NK::Ident);
        e->str = u"__aios_import_dyn";
        advance();
      } else if (module_mode && nx.type == TT::Punct && nx.punct == P::Dot) {
        uint32_t from = cur.start;
        advance();
        advance();
        if (!at_ident(u"meta")) return unexpected();
        mod_edits.push_back({from, cur.end, u"__aios_meta"});
        e = prog.make(NK::Ident, from, cur.line);
        e->str = u"__aios_meta";
        advance();
      } else {
        return fail("Cannot use import statement outside a module");
      }
    } else {
      e = parse_primary();
    }
    if (!e) return nullptr;
    return parse_chain(e, start_tok, true);
  }

  Node* parse_chain(Node* e, const Token& start_tok, bool allow_call) {
    bool has_optional = false;
    for (;;) {
      if (!cur.error.empty()) return fail(cur.error);
      if (at_punct(P::Dot)) {
        advance();
        Node* m = prog.make(NK::Member, e->pos, e->line);
        m->a = e;
        if (cur.type == TT::Private) {
          Node* pn = make(NK::PrivateName);
          pn->str = cur.str;
          if (!private_refs.empty()) private_refs.back().emplace_back(cur.str, cur.start);
          else return fail("Private field '#" + narrow(cur.str) + "' must be declared in an enclosing class");
          m->b = pn;
          advance();
        } else if (cur.type == TT::Ident || cur.type == TT::Keyword) {
          m->str = cur.str;
          advance();
        } else {
          return unexpected();
        }
        e = m;
      } else if (at_punct(P::QDot)) {
        if (!allow_call) return fail("Invalid optional chain from new expression");
        advance();
        has_optional = true;
        if (at_punct(P::LParen)) {
          Node* c = prog.make(NK::Call, e->pos, e->line);
          c->a = e;
          c->flags |= kFlagOptional;
          if (!parse_arguments(c)) return nullptr;
          e = c;
        } else if (at_punct(P::LBracket)) {
          advance();
          Node* ix = prog.make(NK::Index, e->pos, e->line);
          ix->a = e;
          ix->flags |= kFlagOptional;
          bool saved = no_in;
          no_in = false;
          ix->b = parse_expression();
          no_in = saved;
          if (!ix->b) return nullptr;
          if (!expect_punct(P::RBracket, "']'")) return nullptr;
          e = ix;
        } else if (cur.type == TT::Template) {
          return fail("Invalid tagged template on optional chain");
        } else {
          Node* m = prog.make(NK::Member, e->pos, e->line);
          m->a = e;
          m->flags |= kFlagOptional;
          if (cur.type == TT::Private) {
            Node* pn = make(NK::PrivateName);
            pn->str = cur.str;
            if (!private_refs.empty()) private_refs.back().emplace_back(cur.str, cur.start);
            else return fail("Private field '#" + narrow(cur.str) + "' must be declared in an enclosing class");
            m->b = pn;
            advance();
          } else if (cur.type == TT::Ident || cur.type == TT::Keyword) {
            m->str = cur.str;
            advance();
          } else {
            return unexpected();
          }
          e = m;
        }
      } else if (at_punct(P::LBracket)) {
        advance();
        Node* ix = prog.make(NK::Index, e->pos, e->line);
        ix->a = e;
        bool saved = no_in;
        no_in = false;
        ix->b = parse_expression();
        no_in = saved;
        if (!ix->b) return nullptr;
        if (!expect_punct(P::RBracket, "']'")) return nullptr;
        e = ix;
      } else if (at_punct(P::LParen) && allow_call) {
        // `async (x) => ...` arrives here as a call of `async`
        if (e->kind == NK::Ident && e->str == u"async" && !cur.nl_before && !e->has(kFlagParenthesized) && e->pos == start_tok.start && start_tok.type == TT::Ident) {
          Node* c = prog.make(NK::Call, e->pos, e->line);
          c->a = e;
          if (!parse_arguments(c, true)) return nullptr;
          if (at_punct(P::Arrow) && !cur.nl_before) return arrow_from_cover(c, start_tok, true);
          e = c;
          continue;
        }
        Node* c = prog.make(NK::Call, e->pos, e->line);
        c->a = e;
        if (!parse_arguments(c)) return nullptr;
        e = c;
      } else if (cur.type == TT::Template) {
        if (has_optional) return fail("Invalid tagged template on optional chain");
        Node* t = prog.make(NK::TaggedTemplate, e->pos, e->line);
        t->a = e;
        t->b = parse_template(true);
        if (!t->b) return nullptr;
        e = t;
      } else {
        break;
      }
    }
    if (has_optional) {
      Node* oc = prog.make(NK::OptChain, e->pos, e->line);
      oc->a = e;
      return oc;
    }
    return e;
  }

  bool parse_arguments(Node* call, bool cover = false) {
    if (!expect_punct(P::LParen, "'('")) return false;
    bool saved = no_in;
    no_in = false;
    while (!at_punct(P::RParen)) {
      Node* a;
      if (at_punct(P::Ellipsis)) {
        Node* s = make(NK::Spread);
        advance();
        s->a = parse_assignment();
        if (!s->a) { no_in = saved; return false; }
        a = s;
      } else {
        a = parse_assignment();
        if (!a) { no_in = saved; return false; }
      }
      call->list.push_back(a);
      if (!at_punct(P::RParen)) {
        if (!expect_punct(P::Comma, "','")) { no_in = saved; return false; }
        if (at_punct(P::RParen) && cover) call->flags |= kFlagTrailingComma;
      }
    }
    no_in = saved;
    advance();   // )
    return true;
  }

  Node* parse_new() {
    Token new_tok = cur;
    advance();   // new
    if (at_punct(P::Dot)) {
      advance();
      if (!at_ident(u"target")) return unexpected();
      if (!fn().allow_new_target) return fail("new.target expression is not allowed here", new_tok.start);
      advance();
      Node* n = prog.make(NK::NewTarget, new_tok.start, new_tok.line);
      for (size_t i = fns.size(); i-- > 0;) { fns[i].saw_this = true; if (!fns[i].is_arrow) break; }
      return n;
    }
    Node* callee;
    Token callee_tok = cur;
    if (at_kw(Kw::New)) callee = parse_new();
    else if (at_kw(Kw::Super)) callee = parse_super();
    else if (at_kw(Kw::Import)) return fail("Cannot use new with import");
    else callee = parse_primary();
    if (!callee) return nullptr;
    callee = parse_chain(callee, callee_tok, false);
    if (!callee) return nullptr;
    Node* n = prog.make(NK::New, new_tok.start, new_tok.line);
    n->a = callee;
    if (at_punct(P::LParen)) {
      if (!parse_arguments(n)) return nullptr;
    }
    return n;
  }

  Node* parse_super() {
    Node* n = make(NK::Super);
    advance();
    if (at_punct(P::LParen)) {
      if (!fn().allow_super_call) return fail("'super' keyword unexpected here", n->pos);
    } else if (at_punct(P::Dot) || at_punct(P::LBracket)) {
      if (!fn().allow_super_prop) return fail("'super' keyword unexpected here", n->pos);
    } else {
      return fail("'super' keyword unexpected here", n->pos);
    }
    for (size_t i = fns.size(); i-- > 0;) { fns[i].saw_this = true; if (!fns[i].is_arrow) break; }
    return n;
  }

  Node* parse_primary() {
    if (!cur.error.empty()) return fail(cur.error);
    switch (cur.type) {
      case TT::Number: {
        if (cur.bigint) {
          BigNum v;
          if (!big::string_to_bigint(cur.str, &v)) return fail("Invalid BigInt syntax");
          Node* n = make(NK::BigIntLit);
          n->str = cur.str;
          advance();
          return n;
        }
        if (cur.legacy_octal && strict) return fail("Octal literals are not allowed in strict mode.");
        Node* n = make(NK::Number);
        n->num = cur.num;
        advance();
        return n;
      }
      case TT::String: {
        Node* n = make(NK::String);
        n->str = cur.str;
        if (cur.legacy_octal) n->flags |= kFlagLegacyOctal;
        advance();
        return n;
      }
      case TT::Template: return parse_template(false);
      case TT::Ident: {
        if (cur.str == u"async" && !cur.escaped) {
          Token nx = peek();
          if (nx.type == TT::Keyword && nx.kw == Kw::Function && !nx.nl_before) return parse_function_expression(true);
        }
        if (cur.str == u"let" && strict) return fail("Unexpected strict mode reserved word");
        Node* n = parse_identifier_reference();
        if (n && n->str == u"arguments" && fn().in_class_field_init) return fail("'arguments' is not allowed in class field initializer or static initialization block");
        return n;
      }
      case TT::Keyword:
        if (cur.escaped) return fail("Keyword must not contain escaped characters");
        switch (cur.kw) {
          case Kw::This: {
            Node* n = make(NK::This);
            advance();
            for (size_t i = fns.size(); i-- > 0;) { fns[i].saw_this = true; if (!fns[i].is_arrow) break; }
            return n;
          }
          case Kw::Null: { Node* n = make(NK::Null); advance(); return n; }
          case Kw::True: case Kw::False: {
            Node* n = make(NK::Bool);
            n->num = cur.kw == Kw::True ? 1 : 0;
            advance();
            return n;
          }
          case Kw::Function: return parse_function_expression(false);
          case Kw::Class: return parse_class(false);
          default: return unexpected();
        }
      case TT::Punct:
        switch (cur.punct) {
          case P::LParen: return parse_paren();
          case P::LBracket: return parse_array_literal();
          case P::LBrace: return parse_object_literal();
          case P::Slash: case P::SlashAssign: {
            Token rx = lx.rescan_regex(cur);
            cur = rx;
            if (!cur.error.empty()) return fail(cur.error);
            Node* n = make(NK::RegExpLit);
            n->str = cur.str;
            n->str2 = cur.str2;
            advance();
            return n;
          }
          default: return unexpected();
        }
      case TT::Private: return unexpected();
      default: return unexpected();
    }
  }

  Node* parse_paren() {
    Token open = cur;
    advance();   // (
    bool saved = no_in;
    no_in = false;
    std::vector<Node*> items;
    bool trailing_comma = false, has_rest = false;
    Node* result = nullptr;
    if (at_punct(P::RParen)) {
      advance();
      no_in = saved;
      if (!at_punct(P::Arrow) || cur.nl_before) return unexpected();
      // `() => ...`
      return finish_arrow(items, open, false, true);
    }
    while (true) {
      if (at_punct(P::Ellipsis)) {
        Node* s = make(NK::Spread);
        advance();
        s->a = parse_binding_target();
        if (!s->a) { no_in = saved; return nullptr; }
        items.push_back(s);
        has_rest = true;
        if (!at_punct(P::RParen)) { no_in = saved; return fail("Rest parameter must be last formal parameter"); }
        break;
      }
      Node* e = parse_assignment();
      if (!e) { no_in = saved; return nullptr; }
      items.push_back(e);
      if (at_punct(P::Comma)) {
        advance();
        if (at_punct(P::RParen)) { trailing_comma = true; break; }
        continue;
      }
      break;
    }
    no_in = saved;
    if (!expect_punct(P::RParen, "')'")) return nullptr;
    if (at_punct(P::Arrow) && !cur.nl_before) {
      return finish_arrow(items, open, false, true);
    }
    if (has_rest || trailing_comma) return fail("Unexpected token", open.start);
    if (items.size() == 1) {
      result = items[0];
    } else {
      result = prog.make(NK::Sequence, items[0]->pos, items[0]->line);
      result->list = items;
    }
    result->flags |= kFlagParenthesized;
    return result;
  }

  Node* parse_array_literal() {
    Node* n = make(NK::ArrayLit);
    advance();   // [
    bool saved = no_in;
    no_in = false;
    while (!at_punct(P::RBracket)) {
      if (at_punct(P::Comma)) { advance(); n->list.push_back(nullptr); continue; }
      Node* e;
      if (at_punct(P::Ellipsis)) {
        Node* s = make(NK::Spread);
        advance();
        s->a = parse_assignment();
        if (!s->a) { no_in = saved; return nullptr; }
        e = s;
      } else {
        e = parse_assignment();
        if (!e) { no_in = saved; return nullptr; }
      }
      n->list.push_back(e);
      if (!at_punct(P::RBracket)) {
        if (!expect_punct(P::Comma, "','")) { no_in = saved; return nullptr; }
        if (at_punct(P::RBracket)) n->flags |= kFlagTrailingComma;
      }
    }
    no_in = saved;
    advance();   // ]
    return n;
  }

  Node* parse_object_literal() {
    Node* n = make(NK::ObjectLit);
    advance();   // {
    bool saved = no_in;
    no_in = false;
    bool has_proto = false;
    while (!at_punct(P::RBrace)) {
      Node* p = parse_object_property(&has_proto);
      if (!p) { no_in = saved; return nullptr; }
      n->list.push_back(p);
      if (!at_punct(P::RBrace)) {
        if (!expect_punct(P::Comma, "','")) { no_in = saved; return nullptr; }
      }
    }
    no_in = saved;
    advance();   // }
    return n;
  }

  Node* parse_object_property(bool* has_proto) {
    Node* p = make(NK::Property);
    uint32_t start = cur.start;
    uint32_t line = cur.line;
    if (at_punct(P::Ellipsis)) {
      advance();
      p->flags |= kFlagSpread;
      p->a = parse_assignment();
      if (!p->a) return nullptr;
      return p;
    }
    bool is_async = false, is_gen = false;
    uint32_t kind_flags = 0;
    auto next_is_key_end = [&]() {
      Token nx = peek();
      return nx.type == TT::Punct && (nx.punct == P::Comma || nx.punct == P::Colon || nx.punct == P::LParen || nx.punct == P::RBrace || nx.punct == P::Assign);
    };
    if (at_ident(u"async") && !next_is_key_end()) {
      Token nx = peek();
      if (!nx.nl_before) { advance(); is_async = true; }
    }
    if (at_punct(P::Star)) { advance(); is_gen = true; }
    if (!is_async && !is_gen && (at_ident(u"get") || at_ident(u"set")) && !next_is_key_end()) {
      kind_flags = at_ident(u"get") ? kFlagGetter : kFlagSetter;
      advance();
    }
    Token key_tok = cur;
    bool computed = false;
    Node* key = parse_property_key(&computed, false);
    if (!key) return nullptr;
    p->a = key;
    if (computed) p->flags |= kFlagComputed;
    if (at_punct(P::LParen)) {
      FuncKind fk = kind_flags & kFlagGetter ? FuncKind::Getter : kind_flags & kFlagSetter ? FuncKind::Setter : FuncKind::Method;
      std::u16string name;
      if (!computed && key->kind == NK::String) name = key->str;
      else if (!computed && key->kind == NK::Number) name.clear();
      FuncInfo* fi = parse_function_rest(start, line, name, fk, is_async, is_gen, false);
      if (!fi) return nullptr;
      if (fk == FuncKind::Getter && !fi->params.empty()) return fail("Getter must not have any formal parameters.", start);
      if (fk == FuncKind::Setter && (fi->params.size() != 1 || fi->params[0]->kind == NK::Rest)) return fail("Setter must have exactly one formal parameter.", start);
      Node* f = prog.make(NK::FunctionExpr, start, line);
      f->fn = fi;
      p->b = f;
      p->flags |= kFlagMethod | kind_flags;
      return p;
    }
    if (kind_flags || is_async || is_gen) return unexpected();
    if (eat_punct(P::Colon)) {
      p->b = parse_assignment();
      if (!p->b) return nullptr;
      if (!computed && key->kind == NK::String && key->str == u"__proto__") {
        if (*has_proto) {
          // a duplicate __proto__ is an error unless this literal becomes a pattern; checked loosely
          p->flags |= kFlagProto;
        } else {
          p->flags |= kFlagProto;
          *has_proto = true;
        }
      } else if (!computed && key->kind == NK::String) {
        Node* idnode = prog.make(NK::Ident, key->pos, key->line);
        idnode->str = key->str;
        name_anonymous(p->b, idnode);
      }
      return p;
    }
    // shorthand { a } / { a = 1 }
    if (computed || key_tok.type != TT::Ident) return unexpected();
    if (!ident_ok(key_tok, false)) return fail("Unexpected reserved word '" + narrow(key_tok.str) + "'", key_tok.start);
    Node* id = prog.make(NK::Ident, key_tok.start, key_tok.line);
    id->str = key_tok.str;
    if (id->str == u"arguments") {
      for (size_t i = fns.size(); i-- > 0;) { fns[i].saw_arguments = true; if (!fns[i].is_arrow) break; }
    }
    p->flags |= kFlagShorthand;
    if (at_punct(P::Assign)) {
      // only valid if this literal turns out to be a destructuring pattern
      Node* a = make(NK::Assign);
      a->op = Op::Plain;
      advance();
      a->a = id;
      a->b = parse_assignment();
      if (!a->b) return nullptr;
      name_anonymous(a->b, id);
      p->b = a;
      p->flags |= kFlagHasDefaultInit;
    } else {
      p->b = id;
    }
    return p;
  }

  Node* parse_template(bool tagged) {
    Node* n = make(NK::Template);
    TemplateData* td = prog.make_template();
    n->tpl = td;
    for (;;) {
      if (!cur.error.empty()) return fail(cur.error);
      if (cur.type != TT::Template) return unexpected();
      if (!cur.template_cooked_ok && !tagged) return fail("Invalid escape sequence in template");
      td->cooked.push_back(cur.str);
      td->raw.push_back(cur.str2);
      td->cooked_ok.push_back(cur.template_cooked_ok);
      bool tail = cur.template_tail;
      advance();
      if (tail) break;
      bool saved = no_in;
      no_in = false;
      Node* e = parse_expression();
      no_in = saved;
      if (!e) return nullptr;
      n->list.push_back(e);
      if (!at_punct(P::RBrace)) return unexpected();
      cur = lx.rescan_template_continuation(cur);
      if (!cur.error.empty()) return fail(cur.error);
    }
    return n;
  }

  // ------------------------------------------------------------------ entry points


  // ---- ES module syntax (module mode only) ----------------------------------------------------

  static std::u16string quote16(const std::u16string& v) {
    std::u16string o = u"\"";
    for (char16_t c : v) {
      if (c == u'"' || c == u'\\' || c < 0x20 || c == 0x2028 || c == 0x2029) {
        static const char16_t hex[] = u"0123456789abcdef";
        o += u"\\u";
        for (int sh = 12; sh >= 0; sh -= 4) o += hex[(c >> sh) & 15];
      } else {
        o += c;
      }
    }
    return o + u"\"";
  }

  void blank_range(uint32_t from, uint32_t to, const std::u16string& with = std::u16string()) { mod_edits.push_back({from, to, with}); }

  // A module specifier or export name: a string literal or an identifier name (keywords included: `default`).
  bool take_module_name(std::u16string* out) {
    if (cur.type == TT::String || cur.type == TT::Ident || cur.type == TT::Keyword) { *out = cur.str; advance(); return true; }
    unexpected();
    return false;
  }

  bool take_from_clause(std::u16string* spec) {
    if (!at_ident(u"from")) { unexpected(); return false; }
    advance();
    if (cur.type != TT::String) { unexpected(); return false; }
    *spec = cur.str;
    advance();
    return true;
  }

  Node* parse_module_import() {
    const uint32_t start = cur.start;
    advance();   // import
    const std::u16string var = u"__aios_m" + to16(std::to_string(++module_seq));
    std::u16string spec, ns_local, def_local;
    std::vector<std::pair<std::u16string, std::u16string>> named;   // imported name, local name
    if (cur.type == TT::String) {
      spec = cur.str;
      advance();
      mod_hoist += u"await __aios_import(" + quote16(spec) + u");";
    } else {
      if (cur.type == TT::Ident) {
        def_local = cur.str;
        advance();
        if (!eat_punct(P::Comma) && !at_ident(u"from")) return unexpected();
      }
      if (at_punct(P::Star)) {
        advance();
        if (!at_ident(u"as")) return unexpected();
        advance();
        if (cur.type != TT::Ident) return unexpected();
        ns_local = cur.str;
        advance();
      } else if (at_punct(P::LBrace)) {
        advance();
        while (!at_punct(P::RBrace)) {
          std::u16string imported, local;
          const Token name_tok = cur;
          if (!take_module_name(&imported)) return nullptr;
          if (at_ident(u"as")) {
            advance();
            if (cur.type != TT::Ident) return unexpected();
            local = cur.str;
            advance();
          } else {
            if (name_tok.type != TT::Ident) return unexpected();
            local = imported;
          }
          named.push_back({imported, local});
          if (!at_punct(P::RBrace) && !expect_punct(P::Comma, "','")) return nullptr;
        }
        advance();
      }
      if (!take_from_clause(&spec)) return nullptr;
      mod_hoist += u"const " + var + u"=await __aios_import(" + quote16(spec) + u");";
      if (!ns_local.empty()) mod_hoist += u"const " + ns_local + u"=" + var + u";";
      if (!def_local.empty()) mod_hoist += u"const " + def_local + u"=" + var + u".default;";
      for (auto& nl : named) mod_hoist += u"const " + nl.second + u"=" + var + u"[" + quote16(nl.first) + u"];";
    }
    blank_range(start, prev_end);
    if (!consume_semicolon()) return nullptr;
    return prog.make(NK::Empty, start, cur.line);
  }

  static std::u16string to16(const std::string& s) { return std::u16string(s.begin(), s.end()); }

  void collect_bound_names(Node* t, std::vector<std::u16string>& out) {
    if (!t) return;
    switch (t->kind) {
      case NK::Ident: out.push_back(t->str); break;
      case NK::AssignPattern: collect_bound_names(t->a, out); break;
      case NK::Rest: collect_bound_names(t->a, out); break;
      case NK::ArrayPattern: for (Node* e : t->list) collect_bound_names(e, out); break;
      case NK::ObjectPattern:
        for (Node* e : t->list) collect_bound_names(e->kind == NK::Property ? e->b : e, out);
        break;
      default: break;
    }
  }

  void add_export(const std::u16string& exported, const std::u16string& expr) {
    mod_exports += quote16(exported) + u":()=>" + expr + u",";
  }

  Node* parse_module_export() {
    const uint32_t start = cur.start;
    advance();   // export
    if (at_punct(P::Star)) {
      advance();
      std::u16string as, spec;
      if (at_ident(u"as")) { advance(); if (!take_module_name(&as)) return nullptr; }
      if (!take_from_clause(&spec)) return nullptr;
      const std::u16string var = u"__aios_m" + to16(std::to_string(++module_seq));
      mod_hoist += u"const " + var + u"=await __aios_import(" + quote16(spec) + u");";
      if (as.empty()) mod_stars += u"__aios_exports(" + var + u",1);";
      else add_export(as, var);
      blank_range(start, prev_end);
      if (!consume_semicolon()) return nullptr;
      return prog.make(NK::Empty, start, cur.line);
    }
    if (at_punct(P::LBrace)) {
      advance();
      std::vector<std::pair<std::u16string, std::u16string>> list;   // local, exported
      while (!at_punct(P::RBrace)) {
        std::u16string local, exported;
        if (!take_module_name(&local)) return nullptr;
        exported = local;
        if (at_ident(u"as")) { advance(); if (!take_module_name(&exported)) return nullptr; }
        list.push_back({local, exported});
        if (!at_punct(P::RBrace) && !expect_punct(P::Comma, "','")) return nullptr;
      }
      advance();
      if (at_ident(u"from")) {
        std::u16string spec;
        if (!take_from_clause(&spec)) return nullptr;
        const std::u16string var = u"__aios_m" + to16(std::to_string(++module_seq));
        mod_hoist += u"const " + var + u"=await __aios_import(" + quote16(spec) + u");";
        for (auto& le : list) add_export(le.second, var + u"[" + quote16(le.first) + u"]");
      } else {
        for (auto& le : list) add_export(le.second, le.first);
      }
      blank_range(start, prev_end);
      if (!consume_semicolon()) return nullptr;
      return prog.make(NK::Empty, start, cur.line);
    }
    if (at_kw(Kw::Default)) {
      advance();
      // `export default function f(){}` / `class C{}` are declarations and bind their name; anything else (including an
      // anonymous function or class) is an expression whose value goes in a hidden variable.
      std::u16string name;
      auto st = lx.save();
      Token saved_cur = cur;
      const uint32_t saved_prev_end = prev_end, saved_prev_line = prev_line;
      bool decl = false;
      if (at_ident(u"async")) { Token nx = peek(); if (nx.type == TT::Keyword && nx.kw == Kw::Function && !nx.nl_before) advance(); }
      if (at_kw(Kw::Function)) { advance(); eat_punct(P::Star); if (cur.type == TT::Ident) { name = cur.str; decl = true; } }
      else if (at_kw(Kw::Class)) { advance(); if (cur.type == TT::Ident) { name = cur.str; decl = true; } }
      lx.restore(st);
      cur = saved_cur;
      prev_end = saved_prev_end;
      prev_line = saved_prev_line;
      if (decl) {
        blank_range(start, cur.start);
        Node* d = parse_statement_list_item();
        if (!d) return nullptr;
        add_export(u"default", name);
        return d;
      }
      blank_range(start, prev_end, u"__aios_def=");
      Node* e = parse_assignment();
      if (!e) return nullptr;
      add_export(u"default", u"__aios_def");
      if (!consume_semicolon()) return nullptr;
      return prog.make(NK::Empty, start, cur.line);
    }
    if (at_kw(Kw::Var) || at_kw(Kw::Const) || at_kw(Kw::Function) || at_kw(Kw::Class) || at_ident(u"let") || at_ident(u"async")) {
      blank_range(start, cur.start);
      Node* d = parse_statement_list_item();
      if (!d) return nullptr;
      std::vector<std::u16string> names;
      if (d->kind == NK::VarDecl) { for (Node* dc : d->list) collect_bound_names(dc->a, names); }
      else if (d->kind == NK::FunctionDecl && d->fn) names.push_back(d->fn->name);
      else if (d->kind == NK::ClassDecl) names.push_back(d->str);
      else return fail("Unexpected token", start);
      for (auto& nm : names) add_export(nm, nm);
      return d;
    }
    return unexpected();
  }

  Node* parse_script(const ParseOptions& opt) {
    strict = opt.strict;
    lx.strict = strict;
    FnCtx top;
    top.allow_return = opt.in_function;
    top.allow_new_target = opt.allow_new_target;
    top.allow_super_prop = opt.allow_super_property;
    top.allow_super_call = opt.allow_super_call;
    fns.push_back(top);
    cur = lx.next();
    Node* root = prog.make(NK::Program, 0, 1);
    if (strict) root->flags |= kFlagUseStrict;
    if (!parse_program_body(root)) return nullptr;
    if (strict) root->flags |= kFlagUseStrict;
    prog.strict = strict;
    return root;
  }
};

// After parsing: an object literal `{a = 1}` that was never converted to a pattern is an error.
bool validate_cover_init(Node* n, ParseError* err) {
  if (!n) return true;
  bool ok = true;
  auto fail_at = [&](Node* at) {
    if (ok) { ok = false; if (err) { err->message = "Invalid shorthand property initializer"; err->line = at->line; err->col = 0; } }
  };
  std::vector<Node*> stack = {n};
  std::unordered_set<Node*> seen;
  while (!stack.empty() && ok) {
    Node* x = stack.back();
    stack.pop_back();
    if (!x || !seen.insert(x).second) continue;
    if (x->kind == NK::Property && (x->flags & kFlagHasDefaultInit)) { fail_at(x); break; }
    for (Node* k : {x->a, x->b, x->c, x->d}) if (k) stack.push_back(k);
    for (Node* k : x->list) if (k) stack.push_back(k);
    if (x->fn) {
      for (Node* p : x->fn->params) if (p) stack.push_back(p);
      if (x->fn->body) stack.push_back(x->fn->body);
    }
  }
  return ok;
}

}  // namespace

bool parse_program(const std::shared_ptr<const std::u16string>& source, const ParseOptions& opt, Program& out, ParseError* err) {
  out.source = source;
  Parser p(out, source);
  Node* root = p.parse_script(opt);
  if (!root || p.failed) {
    if (err) *err = p.err;
    return false;
  }
  if (!validate_cover_init(root, err)) return false;
  out.root = root;
  return true;
}

bool parse_function_source(const std::shared_ptr<const std::u16string>& source, const std::u16string& params, const std::u16string& body, bool is_async,
                           bool is_generator, Program& out, ParseError* err) {
  // The text is "(<async|function*> anonymous(<params>\n) {\n<body>\n})" so errors point somewhere sensible.
  std::u16string text = u"(";
  if (is_async) text += u"async ";
  text += u"function";
  if (is_generator) text += u"*";
  text += u" anonymous(" + params + u"\n) {\n" + body + u"\n})";
  auto src = std::make_shared<const std::u16string>(std::move(text));
  ParseOptions opt;
  if (!parse_program(src, opt, out, err)) return false;
  (void)source;
  return true;
}

// A module as an ordinary function expression: `(async function(__aios_import, __aios_exports, __aios_meta, __aios_import_dyn) {...})`.
// The body is the module's own text with each import/export statement blanked out (newlines kept, so line numbers in stack
// traces still match the file) and the generated code in front of it, all on the first line. Imports become `const`
// bindings taken from the dependency's namespace once it has been evaluated, in source order; exports are getters registered
// before anything runs, so a cycle sees them, and the module's values stay live for whoever imports them later.
bool module_to_function(const std::u16string& source, std::u16string* out, ParseError* err) {
  auto src = std::make_shared<const std::u16string>(source);
  Program prog;
  prog.source = src;
  Parser p(prog, src);
  p.module_mode = true;
  ParseOptions opt;
  opt.strict = true;
  Node* root = p.parse_script(opt);
  if (!root || p.failed) {
    if (err) *err = p.err;
    return false;
  }
  if (!validate_cover_init(root, err)) return false;
  std::sort(p.mod_edits.begin(), p.mod_edits.end(), [](const Parser::ModEdit& a, const Parser::ModEdit& b) { return a.start < b.start; });
  std::u16string body;
  uint32_t at = 0;
  for (const Parser::ModEdit& e : p.mod_edits) {
    if (e.start < at || e.end > source.size()) continue;
    body.append(source, at, e.start - at);
    body += e.repl;
    for (uint32_t i = e.start; i < e.end; ++i) {
      const char16_t c = source[i];
      if (c == u'\n' || c == u'\r' || c == 0x2028 || c == 0x2029) body += c;
    }
    at = e.end;
  }
  body.append(source, at, std::u16string::npos);
  *out = u"(async function(__aios_import,__aios_exports,__aios_meta,__aios_import_dyn){\"use strict\";let __aios_def;__aios_exports({" + p.mod_exports +
         u"});" + p.mod_hoist + p.mod_stars + body + u"\n})";
  return true;
}

}  // namespace aios::js
