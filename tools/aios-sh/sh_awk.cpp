// awk: a POSIX awk interpreter. Patterns (BEGIN, END, expressions, regexes, ranges), actions, user
// functions, associative arrays, fields and the special variables (NR NF FNR FS OFS ORS RS FILENAME SUBSEP
// RSTART RLENGTH CONVFMT OFMT ENVIRON), the string, math and I/O builtins, print/printf with > >> | and
// getline. Regexes are POSIX extended, translated as grep's are.
#include "sh.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <regex>
#include <thread>
#include <unordered_map>

namespace aiossh {

std::string posix_regex_to_ecma(const std::string& re, bool extended);   // sh_grep.cpp

namespace {

struct AwkError { std::string msg; };

// ------------------------------------------------------------------------------------------- values

struct Val {
  enum Kind { Num, Str, StrNum } kind = Str;   // StrNum: from input, compares as a number when it looks like one
  double n = 0;
  std::string s;
  static Val num(double d) { Val v; v.kind = Num; v.n = d; return v; }
  static Val str(std::string x) { Val v; v.kind = Str; v.s = std::move(x); return v; }
};

bool looks_numeric(const std::string& s, double* out) {
  const char* p = s.c_str();
  while (*p == ' ' || *p == '\t' || *p == '\n') ++p;
  if (!*p) return false;
  char* e = nullptr;
  double d = std::strtod(p, &e);
  if (e == p) return false;
  while (*e == ' ' || *e == '\t' || *e == '\n') ++e;
  if (*e) return false;
  if ((p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) || std::strchr(p, 'n') || std::strchr(p, 'N') || std::strchr(p, 'i') || std::strchr(p, 'I')) return false;
  if (out) *out = d;
  return true;
}
Val strnum(const std::string& s) {
  Val v;
  double d;
  if (looks_numeric(s, &d)) { v.kind = Val::StrNum; v.n = d; v.s = s; }
  else { v.kind = Val::Str; v.s = s; }
  return v;
}

std::string fmt_num(double d, const std::string& f) {
  if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) < 1e16) {
    char b[64]; std::snprintf(b, sizeof b, "%lld", static_cast<long long>(d)); return b;
  }
  if (std::isnan(d)) return "nan";
  if (std::isinf(d)) return d > 0 ? "inf" : "-inf";
  char b[128]; std::snprintf(b, sizeof b, f.c_str(), d); return b;
}

// ------------------------------------------------------------------------------------------- tree

struct Expr;
using EP = std::shared_ptr<Expr>;
struct Expr {
  enum Kind { Num, Str, Regex, Var, Field, Index, Assign, Cond, And, Or, Not, Neg, Plus, Bin, Cmp, Match, Concat, In,
              PreInc, PreDec, PostInc, PostDec, Call, Builtin, Getline, Group } kind = Num;
  double n = 0;
  std::string s;            // Str value, Regex source, Var name, op, function name
  std::vector<EP> args;     // operands / subscripts / call arguments
  EP a, b, c;
  int getline_kind = 0;     // 0 plain (main input), 1 < file, 2 cmd |
};

struct Stmt;
using SP = std::shared_ptr<Stmt>;
struct Stmt {
  enum Kind { Expr_, Print, Printf, If, While, Do, For, ForIn, Block, Next, NextFile, Exit, Return, Break, Continue, Delete, Getline } kind = Expr_;
  EP e, e2, e3;
  std::vector<EP> args;
  std::string redir;        // ">", ">>", "|"
  EP dest;
  SP body, els;
  std::vector<SP> kids;
  std::string var;          // for-in variable
};

struct Rule { EP pattern, pattern2; bool begin = false, end = false, range_active = false; SP action; };
struct Func { std::vector<std::string> params; SP body; };

// ------------------------------------------------------------------------------------------- lexer

struct Tok {
  enum Type { End, Newline, Num, Str, Ere, Name, FuncName, Builtin, Keyword, Punct } type = End;
  std::string s;
  double n = 0;
};

const char* kBuiltins[] = {"length", "substr", "index", "split", "sub", "gsub", "match", "sprintf", "tolower", "toupper", "int", "sqrt",
                           "exp", "log", "sin", "cos", "atan2", "rand", "srand", "system", "close", "fflush"};
const char* kKeywords[] = {"BEGIN", "END", "function", "func", "if", "else", "while", "for", "do", "break", "continue", "next", "nextfile",
                           "exit", "return", "delete", "in", "getline", "print", "printf"};

class Lexer {
 public:
  explicit Lexer(const std::string& s) : s_(s) {}
  Tok next(bool regex_ok) {
    for (;;) {
      while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\r')) ++p_;
      if (p_ < s_.size() && s_[p_] == '\\' && p_ + 1 < s_.size() && (s_[p_ + 1] == '\n' || s_[p_ + 1] == '\r')) { p_ += 2; if (p_ < s_.size() && s_[p_] == '\n') ++p_; continue; }
      if (p_ < s_.size() && s_[p_] == '#') { while (p_ < s_.size() && s_[p_] != '\n') ++p_; continue; }
      break;
    }
    Tok t;
    if (p_ >= s_.size()) return t;
    char c = s_[p_];
    if (c == '\n') { ++p_; t.type = Tok::Newline; t.s = "\n"; return t; }
    if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && p_ + 1 < s_.size() && std::isdigit(static_cast<unsigned char>(s_[p_ + 1])))) {
      char* e = nullptr;
      t.n = std::strtod(s_.c_str() + p_, &e);
      p_ = static_cast<size_t>(e - s_.c_str());
      t.type = Tok::Num;
      return t;
    }
    if (c == '"') {
      ++p_;
      std::string v;
      while (p_ < s_.size() && s_[p_] != '"') {
        char d = s_[p_++];
        if (d == '\\' && p_ < s_.size()) {
          char e = s_[p_++];
          switch (e) {
            case 'n': v += '\n'; break; case 't': v += '\t'; break; case 'r': v += '\r'; break; case '\\': v += '\\'; break;
            case '"': v += '"'; break; case '/': v += '/'; break; case 'a': v += '\a'; break; case 'b': v += '\b'; break;
            case 'f': v += '\f'; break; case 'v': v += '\v'; break;
            case '\n': break;
            default:
              if (e >= '0' && e <= '7') { int o = e - '0'; for (int k = 0; k < 2 && p_ < s_.size() && s_[p_] >= '0' && s_[p_] <= '7'; ++k) o = o * 8 + (s_[p_++] - '0'); v += static_cast<char>(o); }
              else { v += '\\'; v += e; }
          }
          continue;
        }
        v += d;
      }
      ++p_;
      t.type = Tok::Str;
      t.s = v;
      return t;
    }
    if (c == '/' && regex_ok) {
      ++p_;
      std::string v;
      bool in_bracket = false;
      while (p_ < s_.size() && (s_[p_] != '/' || in_bracket)) {
        if (s_[p_] == '\\' && p_ + 1 < s_.size()) { if (s_[p_ + 1] == '/') { v += '/'; p_ += 2; continue; } v += s_[p_]; v += s_[p_ + 1]; p_ += 2; continue; }
        if (s_[p_] == '[') in_bracket = true; else if (s_[p_] == ']') in_bracket = false;
        v += s_[p_++];
      }
      ++p_;
      t.type = Tok::Ere;
      t.s = v;
      return t;
    }
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      size_t a = p_;
      while (p_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[p_])) || s_[p_] == '_')) ++p_;
      t.s = s_.substr(a, p_ - a);
      for (const char* k : kKeywords) if (t.s == k) { t.type = Tok::Keyword; return t; }
      for (const char* b : kBuiltins) if (t.s == b) { t.type = Tok::Builtin; return t; }
      t.type = (p_ < s_.size() && s_[p_] == '(') ? Tok::FuncName : Tok::Name;
      return t;
    }
    static const char* ops[] = {"+=", "-=", "*=", "/=", "%=", "^=", "**=", "**", "==", "<=", ">=", "!=", "++", "--", "&&", "||", ">>", "!~",
                                "{", "}", "(", ")", "[", "]", ";", ",", "+", "-", "*", "/", "%", "^", "!", ">", "<", "|", "?", ":", "~", "$", "="};
    for (const char* op : ops) {
      size_t n = std::strlen(op);
      if (s_.compare(p_, n, op) == 0) { p_ += n; t.type = Tok::Punct; t.s = op; if (t.s == "**") t.s = "^"; if (t.s == "**=") t.s = "^="; return t; }
    }
    throw AwkError{std::string("syntax error at '") + c + "'"};
  }

 private:
  std::string s_;
  size_t p_ = 0;
};

// ------------------------------------------------------------------------------------------- parser

class Parser {
 public:
  explicit Parser(const std::string& s) : lx_(s) { advance(true); }
  std::vector<Rule> rules;
  std::unordered_map<std::string, Func> funcs;

  void program() {
    skip_terms();
    while (t_.type != Tok::End) {
      if (kw("function") || kw("func")) { function(); skip_terms(); continue; }
      Rule r;
      if (kw("BEGIN")) { advance(true); r.begin = true; }
      else if (kw("END")) { advance(true); r.end = true; }
      else if (!punct("{")) {
        r.pattern = expr();
        if (punct(",")) { advance(true); skip_newlines(); r.pattern2 = expr(); }
      }
      if (punct("{")) r.action = block();
      else if (r.begin || r.end) throw AwkError{"BEGIN/END need an action"};
      rules.push_back(r);
      skip_terms();
    }
  }

 private:
  Lexer lx_;
  Tok t_;
  bool last_operand_ = false;   // the previous token ended an operand: a '/' now divides
  int no_gt_ = 0;               // inside print's argument list: '>' is a redirection
  int no_in_ = 0;               // in a for(...;...) header: 'in' is not the operator

  void advance(bool regex_ok) {
    t_ = lx_.next(regex_ok);
    last_operand_ = t_.type == Tok::Num || t_.type == Tok::Str || t_.type == Tok::Ere || t_.type == Tok::Name ||
                    (t_.type == Tok::Punct && (t_.s == ")" || t_.s == "]" || t_.s == "++" || t_.s == "--")) || (t_.type == Tok::Builtin);
  }
  void next() { advance(!last_operand_); }
  bool punct(const char* p) const { return t_.type == Tok::Punct && t_.s == p; }
  bool kw(const char* k) const { return t_.type == Tok::Keyword && t_.s == k; }
  void expect(const char* p) { if (!punct(p)) throw AwkError{std::string("expected '") + p + "' near '" + t_.s + "'"}; next(); }
  void skip_newlines() { while (t_.type == Tok::Newline) next(); }
  void skip_terms() { while (t_.type == Tok::Newline || punct(";")) next(); }

  void function() {
    next();
    if (t_.type != Tok::Name && t_.type != Tok::FuncName) throw AwkError{"expected a function name"};
    std::string name = t_.s;
    next();
    expect("(");
    Func f;
    while (!punct(")")) {
      if (t_.type != Tok::Name) throw AwkError{"expected a parameter name"};
      f.params.push_back(t_.s);
      next();
      if (punct(",")) { next(); skip_newlines(); }
    }
    next();
    skip_newlines();
    f.body = block();
    funcs[name] = f;
  }

  SP block() {
    expect("{");
    auto b = std::make_shared<Stmt>();
    b->kind = Stmt::Block;
    skip_terms();
    while (!punct("}")) {
      if (t_.type == Tok::End) throw AwkError{"unexpected end of program: missing '}'"};
      b->kids.push_back(statement());
      skip_terms();
    }
    next();
    return b;
  }

  void end_simple() {
    if (punct(";") || t_.type == Tok::Newline) { next(); return; }
    if (punct("}") || t_.type == Tok::End) return;
    throw AwkError{"unexpected '" + t_.s + "'"};
  }

  SP statement() {
    auto s = std::make_shared<Stmt>();
    if (punct("{")) return block();
    if (kw("if")) {
      next(); expect("("); s->kind = Stmt::If; s->e = expr(); expect(")"); skip_newlines();
      s->body = stmt_or_empty();
      size_t save_ok = 0; (void)save_ok;
      skip_terms_if_else();
      if (kw("else")) { next(); skip_newlines(); s->els = stmt_or_empty(); }
      return s;
    }
    if (kw("while")) {
      next(); expect("("); s->kind = Stmt::While; s->e = expr(); expect(")");
      if (punct(";")) { next(); s->body = std::make_shared<Stmt>(); s->body->kind = Stmt::Block; return s; }
      skip_newlines(); s->body = stmt_or_empty(); return s;
    }
    if (kw("do")) {
      next(); skip_newlines(); s->kind = Stmt::Do; s->body = stmt_or_empty(); skip_terms();
      if (!kw("while")) throw AwkError{"expected 'while' after do"};
      next(); expect("("); s->e = expr(); expect(")"); end_simple(); return s;
    }
    if (kw("for")) {
      next(); expect("(");
      if (t_.type == Tok::Name) {
        // for (k in a)?
        Tok save = t_;
        Lexer save_lx = lx_;
        bool save_last = last_operand_;
        std::string var = t_.s;
        next();
        if (kw("in")) {
          next();
          if (t_.type != Tok::Name) throw AwkError{"expected an array name after in"};
          s->kind = Stmt::ForIn; s->var = var; s->e = std::make_shared<Expr>(); s->e->kind = Expr::Var; s->e->s = t_.s;
          next(); expect(")"); skip_newlines(); s->body = stmt_or_empty(); return s;
        }
        t_ = save; lx_ = save_lx; last_operand_ = save_last;
      }
      s->kind = Stmt::For;
      if (!punct(";")) s->e = simple_statement_expr();
      expect(";"); skip_newlines();
      if (!punct(";")) s->e2 = expr();
      expect(";"); skip_newlines();
      if (!punct(")")) s->e3 = simple_statement_expr();
      expect(")");
      if (punct(";")) { next(); s->body = std::make_shared<Stmt>(); s->body->kind = Stmt::Block; return s; }
      skip_newlines(); s->body = stmt_or_empty(); return s;
    }
    if (punct(";")) { next(); s->kind = Stmt::Block; return s; }
    if (kw("next")) { next(); s->kind = Stmt::Next; end_simple(); return s; }
    if (kw("nextfile")) { next(); s->kind = Stmt::NextFile; end_simple(); return s; }
    if (kw("break")) { next(); s->kind = Stmt::Break; end_simple(); return s; }
    if (kw("continue")) { next(); s->kind = Stmt::Continue; end_simple(); return s; }
    if (kw("exit")) { next(); s->kind = Stmt::Exit; if (!punct(";") && !punct("}") && t_.type != Tok::Newline && t_.type != Tok::End) s->e = expr(); end_simple(); return s; }
    if (kw("return")) { next(); s->kind = Stmt::Return; if (!punct(";") && !punct("}") && t_.type != Tok::Newline && t_.type != Tok::End) s->e = expr(); end_simple(); return s; }
    if (kw("delete")) {
      next(); s->kind = Stmt::Delete;
      if (t_.type != Tok::Name) throw AwkError{"expected an array after delete"};
      s->e = std::make_shared<Expr>(); s->e->kind = Expr::Var; s->e->s = t_.s; next();
      if (punct("[")) { next(); s->args = expr_list("]"); expect("]"); }
      end_simple(); return s;
    }
    if (kw("print") || kw("printf")) {
      s->kind = kw("print") ? Stmt::Print : Stmt::Printf;
      next();
      ++no_gt_;
      if (!punct(";") && !punct("}") && t_.type != Tok::Newline && t_.type != Tok::End && !punct(">") && !punct(">>") && !punct("|")) {
        if (punct("(")) {
          // print (a, b) > "f"  or print (a) b
          next();
          --no_gt_;
          auto list = expr_list(")");
          expect(")");
          ++no_gt_;
          if (list.size() == 1 && !(punct(";") || punct("}") || t_.type == Tok::Newline || t_.type == Tok::End || punct(">") || punct(">>") || punct("|"))) {
            // (a) b ...: the parenthesised part was the start of an expression
            auto g = std::make_shared<Expr>(); g->kind = Expr::Group; g->a = list[0];
            s->args.push_back(continue_expr(g));
            while (punct(",")) { next(); skip_newlines(); s->args.push_back(expr()); }
          } else s->args = list;
        } else {
          s->args.push_back(expr());
          while (punct(",")) { next(); skip_newlines(); s->args.push_back(expr()); }
        }
      }
      --no_gt_;
      if (punct(">") || punct(">>") || punct("|")) { s->redir = t_.s; next(); ++no_gt_; s->dest = concat_expr(); --no_gt_; }
      end_simple();
      return s;
    }
    s->kind = Stmt::Expr_;
    s->e = simple_statement_expr();
    end_simple();
    return s;
  }
  void skip_terms_if_else() {
    // an else may follow after a terminator: look past ; and newlines only if an else is there
    Lexer save = lx_;
    Tok st = t_;
    bool sl = last_operand_;
    while (t_.type == Tok::Newline || punct(";")) next();
    if (kw("else")) return;
    lx_ = save; t_ = st; last_operand_ = sl;
  }
  SP stmt_or_empty() { if (punct(";")) { next(); auto s = std::make_shared<Stmt>(); s->kind = Stmt::Block; return s; } return statement(); }
  EP simple_statement_expr() { return expr(); }

  std::vector<EP> expr_list(const char* close) {
    std::vector<EP> v;
    skip_newlines();
    while (!punct(close)) {
      v.push_back(expr());
      skip_newlines();
      if (punct(",")) { next(); skip_newlines(); }
      else break;
    }
    return v;
  }

  EP mk(Expr::Kind k) { auto e = std::make_shared<Expr>(); e->kind = k; return e; }

  // An expression whose first operand (a parenthesised group) is already parsed: concatenation and up.
  EP continue_expr(EP first) {
    EP l = first;
    // binary operators of every level, left to right, by precedence climbing from the concat level
    l = concat_rest(mul_rest(l));
    l = cmp_rest(l);
    return ternary_rest(or_rest(and_rest(in_rest(match_rest(l)))));
  }

  EP expr() { return ternary(); }
  EP ternary() {
    EP c = or_e();
    return ternary_rest(c);
  }
  EP ternary_rest(EP c) {
    if (punct("?")) {
      next(); skip_newlines();
      EP a = ternary(); skip_newlines();
      expect(":"); skip_newlines();
      EP b = ternary();
      auto e = mk(Expr::Cond); e->a = c; e->b = a; e->c = b;
      return e;
    }
    if (t_.type == Tok::Punct && (t_.s == "=" || t_.s == "+=" || t_.s == "-=" || t_.s == "*=" || t_.s == "/=" || t_.s == "%=" || t_.s == "^=")) {
      if (c->kind != Expr::Var && c->kind != Expr::Field && c->kind != Expr::Index) throw AwkError{"assignment to a non-variable"};
      std::string op = t_.s;
      next(); skip_newlines();
      auto e = mk(Expr::Assign); e->s = op; e->a = c; e->b = ternary();
      return e;
    }
    return c;
  }
  EP or_e() { return or_rest(and_e()); }
  EP or_rest(EP l) { while (punct("||")) { next(); skip_newlines(); auto e = mk(Expr::Or); e->a = l; e->b = and_e(); l = e; } return l; }
  EP and_e() { return and_rest(in_e()); }
  EP and_rest(EP l) { while (punct("&&")) { next(); skip_newlines(); auto e = mk(Expr::And); e->a = l; e->b = in_e(); l = e; } return l; }
  EP in_e() { return in_rest(match_e()); }
  EP in_rest(EP l) {
    while (kw("in") && !no_in_) {
      next();
      if (t_.type != Tok::Name) throw AwkError{"expected an array after in"};
      auto e = mk(Expr::In); e->args.push_back(l); e->s = t_.s; next();
      l = e;
    }
    return l;
  }
  EP match_e() { return match_rest(cmp_e()); }
  EP match_rest(EP l) {
    while (punct("~") || punct("!~")) { std::string op = t_.s; next(); auto e = mk(Expr::Match); e->s = op; e->a = l; e->b = cmp_e(); l = e; }
    return l;
  }
  EP cmp_e() { return cmp_rest(concat_expr()); }
  EP cmp_rest(EP l) {
    for (;;) {
      if (t_.type != Tok::Punct) return l;
      std::string op = t_.s;
      if (op == ">" && no_gt_) return l;
      if (op != "<" && op != "<=" && op != "==" && op != "!=" && op != ">" && op != ">=") return l;
      next();
      auto e = mk(Expr::Cmp); e->s = op; e->a = l; e->b = concat_expr();
      l = e;
    }
  }
  bool starts_operand() const {
    switch (t_.type) {
      case Tok::Num: case Tok::Str: case Tok::Ere: case Tok::Name: case Tok::FuncName: case Tok::Builtin: return true;
      case Tok::Punct: return t_.s == "$" || t_.s == "!" || t_.s == "(" || t_.s == "-" || t_.s == "+" || t_.s == "++" || t_.s == "--";
      case Tok::Keyword: return t_.s == "getline";
      default: return false;
    }
  }
  EP concat_expr() { return concat_rest(additive()); }
  EP concat_rest(EP l) {
    while (starts_operand() && !(kw("in"))) {
      // "a -1" concatenates in awk only with no space... POSIX: a - 1 is subtraction (handled by additive already)
      if (t_.type == Tok::Punct && (t_.s == "-" || t_.s == "+")) break;
      auto e = mk(Expr::Concat); e->a = l; e->b = additive();
      l = e;
    }
    return l;
  }
  EP additive() { return add_rest(mul()); }
  EP add_rest(EP l) {
    while (punct("+") || punct("-")) { std::string op = t_.s; next(); skip_newlines(); auto e = mk(Expr::Bin); e->s = op; e->a = l; e->b = mul(); l = e; }
    return l;
  }
  EP mul() { return mul_rest(unary()); }
  EP mul_rest(EP l) {
    l = add_rest_from_mul(l);
    return l;
  }
  EP add_rest_from_mul(EP l) {
    while (punct("*") || punct("/") || punct("%")) { std::string op = t_.s; next(); skip_newlines(); auto e = mk(Expr::Bin); e->s = op; e->a = l; e->b = unary(); l = e; }
    return add_rest(l);
  }
  EP unary() {
    if (punct("!")) { next(); auto e = mk(Expr::Not); e->a = unary(); return e; }
    if (punct("-")) { next(); auto e = mk(Expr::Neg); e->a = unary(); return e; }
    if (punct("+")) { next(); auto e = mk(Expr::Plus); e->a = unary(); return e; }
    return power();
  }
  EP power() {
    EP b = postfix();
    if (punct("^")) { next(); auto e = mk(Expr::Bin); e->s = "^"; e->a = b; e->b = unary(); return e; }
    return b;
  }
  EP postfix() {
    EP p = primary();
    if ((p->kind == Expr::Var || p->kind == Expr::Field || p->kind == Expr::Index) && (punct("++") || punct("--"))) {
      auto e = mk(punct("++") ? Expr::PostInc : Expr::PostDec); e->a = p; next(); return e;
    }
    return p;
  }
  EP primary() {
    if (t_.type == Tok::Num) { auto e = mk(Expr::Num); e->n = t_.n; next(); return e; }
    if (t_.type == Tok::Str) { auto e = mk(Expr::Str); e->s = t_.s; next(); return e; }
    if (t_.type == Tok::Ere) { auto e = mk(Expr::Regex); e->s = t_.s; next(); return e; }
    if (punct("++") || punct("--")) {
      bool inc = punct("++"); next();
      auto e = mk(inc ? Expr::PreInc : Expr::PreDec); e->a = primary(); return e;
    }
    if (punct("$")) { next(); auto e = mk(Expr::Field); e->a = field_operand(); return e; }
    if (punct("(")) {
      next();
      int save_gt = no_gt_; no_gt_ = 0;
      auto list = expr_list(")");
      no_gt_ = save_gt;
      expect(")");
      if (list.size() > 1 && kw("in")) {
        next();
        auto e = mk(Expr::In); e->args = list; e->s = t_.s; next(); return e;
      }
      if (list.empty()) throw AwkError{"empty ()"};
      auto g = mk(Expr::Group); g->a = list[0];
      return g;
    }
    if (kw("getline")) {
      next();
      auto e = mk(Expr::Getline);
      if (t_.type == Tok::Name || punct("$")) e->a = lvalue_simple();
      if (punct("<")) { next(); e->getline_kind = 1; e->b = primary_for_redirect(); }
      return e;
    }
    if (t_.type == Tok::Builtin) {
      auto e = mk(Expr::Builtin); e->s = t_.s; next();
      if (punct("(")) { next(); e->args = expr_list(")"); expect(")"); }
      return pipe_getline(e);
    }
    if (t_.type == Tok::FuncName) {
      auto e = mk(Expr::Call); e->s = t_.s; next(); expect("("); e->args = expr_list(")"); expect(")");
      return pipe_getline(e);
    }
    if (t_.type == Tok::Name) {
      std::string name = t_.s; next();
      if (punct("[")) { next(); auto e = mk(Expr::Index); e->s = name; e->args = expr_list("]"); expect("]"); return pipe_getline(e); }
      auto e = mk(Expr::Var); e->s = name;
      return pipe_getline(e);
    }
    if (punct("-")) { next(); auto e = mk(Expr::Neg); e->a = unary(); return e; }
    throw AwkError{"syntax error near '" + t_.s + "'"};
  }
  // "cmd" | getline [var]
  EP pipe_getline(EP e) {
    if (punct("|")) {
      Lexer save = lx_; Tok st = t_; bool sl = last_operand_;
      next();
      if (kw("getline")) {
        next();
        auto g = mk(Expr::Getline); g->getline_kind = 2; g->b = e;
        if (t_.type == Tok::Name || punct("$")) g->a = lvalue_simple();
        return g;
      }
      lx_ = save; t_ = st; last_operand_ = sl;
    }
    return e;
  }
  EP primary_for_redirect() {
    if (t_.type == Tok::Str) { auto e = mk(Expr::Str); e->s = t_.s; next(); return e; }
    return concat_expr();
  }
  EP lvalue_simple() {
    if (punct("$")) { next(); auto e = mk(Expr::Field); e->a = field_operand(); return e; }
    std::string name = t_.s; next();
    if (punct("[")) { next(); auto e = mk(Expr::Index); e->s = name; e->args = expr_list("]"); expect("]"); return e; }
    auto e = mk(Expr::Var); e->s = name; return e;
  }
  EP field_operand() {
    if (t_.type == Tok::Num) { auto e = mk(Expr::Num); e->n = t_.n; next(); return e; }
    if (punct("(")) { next(); auto e = expr(); expect(")"); return e; }
    if (punct("$")) { next(); auto e = mk(Expr::Field); e->a = field_operand(); return e; }
    if (punct("++") || punct("--") || punct("-")) return unary();
    if (t_.type == Tok::Name) {
      std::string name = t_.s; next();
      if (punct("[")) { next(); auto e = mk(Expr::Index); e->s = name; e->args = expr_list("]"); expect("]"); return e; }
      auto e = mk(Expr::Var); e->s = name; return e;
    }
    return primary();
  }
};

// ------------------------------------------------------------------------------------------- interpreter

struct NextSignal {};
struct NextFileSignal {};
struct ExitAwk { int code; };
struct ReturnAwk { Val v; };
struct BreakAwk {};
struct ContinueAwk {};

using Array = std::map<std::string, Val>;

class Interp {
 public:
  Interp(Shell& sh, State& st, const Io& io, Parser& p) : sh_(sh), st_(st), io_(io), p_(p) {
    set_var("FS", Val::str(" ")); set_var("OFS", Val::str(" ")); set_var("ORS", Val::str("\n")); set_var("RS", Val::str("\n"));
    set_var("NR", Val::num(0)); set_var("NF", Val::num(0)); set_var("FNR", Val::num(0)); set_var("SUBSEP", Val::str("\x1c"));
    set_var("CONVFMT", Val::str("%.6g")); set_var("OFMT", Val::str("%.6g")); set_var("RSTART", Val::num(0)); set_var("RLENGTH", Val::num(-1));
    set_var("FILENAME", Val::str(""));
    for (const auto& [k, v] : st.vars) if (v.exported) arrays_["ENVIRON"][k] = strnum(v.value);
  }

  int run(const std::vector<std::string>& files, const std::vector<std::pair<std::string, std::string>>& assigns_before) {
    for (auto& [k, v] : assigns_before) set_var(k, strnum(unescape(v)));
    int code = 0;
    try {
      for (auto& r : p_.rules) if (r.begin) exec(r.action);
      bool has_main = false;
      for (auto& r : p_.rules) if (!r.begin) has_main = true;
      if (has_main) {
        std::vector<std::string> fs = files;
        if (fs.empty()) fs.push_back("-");
        for (const auto& f : fs) {
          size_t eq = f.find('=');
          if (eq != std::string::npos && eq > 0 && (std::isalpha(static_cast<unsigned char>(f[0])) || f[0] == '_') && f.find('/') == std::string::npos) {
            set_var(f.substr(0, eq), strnum(unescape(f.substr(eq + 1))));
            continue;
          }
          // The file is read a block at a time as records are asked for, so output keeps pace with a slow producer.
          FdCloser own;
          if (f != "-") {
            std::string p = host_path(st_.cwd, f), e;
            if (os::is_dir(p)) e = "Is a directory";
            else own.f = os::open_file(p, os::Open::Read, &e);
            if (own.f == kNoFd) { io_.ewrite("awk: cannot open \"" + f + "\": " + e + "\n"); code = 2; continue; }
          }
          set_var("FILENAME", Val::str(f == "-" ? "" : f));
          set_var("FNR", Val::num(0));
          input_.clear();
          ipos_ = 0;
          in_fd_ = f == "-" ? io_.in : own.f;
          in_eof_ = false;
          try {
            std::string rec;
            while (next_record(rec)) {
              set_var("NR", Val::num(num(get_var("NR")) + 1));
              set_var("FNR", Val::num(num(get_var("FNR")) + 1));
              set_record(rec);
              try {
                for (auto& r : p_.rules) {
                  if (r.begin || r.end) continue;
                  if (!rule_matches(r)) continue;
                  if (r.action) exec(r.action);
                  else out_main(record_ + str(get_var("ORS")));
                }
              } catch (const NextSignal&) {}
            }
          } catch (const NextFileSignal&) {}
          in_eof_ = true;   // the stream is closed with `own`: a getline in END finds no more input
        }
      }
      for (auto& r : p_.rules) if (r.end) exec(r.action);
    } catch (const ExitAwk& e) {
      in_eof_ = true;
      code = e.code;
      if (!in_end_) {
        in_end_ = true;
        try { for (auto& r : p_.rules) if (r.end) exec(r.action); } catch (const ExitAwk& e2) { code = e2.code; }
      }
    }
    flush_all();
    return code;
  }

 private:
  Shell& sh_;
  State& st_;
  const Io& io_;
  Parser& p_;
  std::unordered_map<std::string, Val> globals_;
  std::unordered_map<std::string, Array> arrays_;
  std::vector<std::unordered_map<std::string, Val>> frames_;
  std::vector<std::unordered_map<std::string, std::shared_ptr<Array>>> frame_arrays_;
  std::string record_;
  std::vector<std::string> fields_;   // $1..
  bool fields_valid_ = true;
  std::string input_;   // the main input read so far and not yet taken as records, from ipos_ on
  size_t ipos_ = 0;
  Fd in_fd_ = kNoFd;
  bool in_eof_ = true;   // also before the first file: a getline in BEGIN has no main input yet
  std::string out_buf_;
  std::map<std::string, Fd> files_;
  std::map<std::string, std::string> pipes_out_;
  std::map<std::string, std::pair<std::string, size_t>> readers_;   // getline < file / cmd |
  std::unordered_map<std::string, std::shared_ptr<std::regex>> re_cache_;
  bool in_end_ = false;
  unsigned long long rand_seed_ = 0;

  static std::string unescape(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
      if (s[i] == '\\' && i + 1 < s.size()) { char c = s[++i]; o += c == 'n' ? '\n' : c == 't' ? '\t' : c == '\\' ? '\\' : c; }
      else o += s[i];
    }
    return o;
  }

  // ---- variables
  Val get_var(const std::string& name) {
    if (!frames_.empty()) { auto it = frames_.back().find(name); if (it != frames_.back().end()) return it->second; }
    if (name == "NF") { split_fields(); return Val::num(static_cast<double>(fields_.size())); }
    auto it = globals_.find(name);
    return it == globals_.end() ? Val::str("") : it->second;
  }
  void set_var(const std::string& name, const Val& v) {
    if (!frames_.empty()) { auto it = frames_.back().find(name); if (it != frames_.back().end()) { it->second = v; return; } }
    if (name == "NF") {
      split_fields();
      size_t n = static_cast<size_t>(std::max(0.0, num(v)));
      fields_.resize(n);
      rebuild_record();
      return;
    }
    globals_[name] = v;
  }
  Array& array(const std::string& name) {
    if (!frame_arrays_.empty()) { auto it = frame_arrays_.back().find(name); if (it != frame_arrays_.back().end()) return *it->second; }
    return arrays_[name];
  }

  // ---- conversions
  double num(const Val& v) {
    if (v.kind != Val::Str) return v.n;
    return std::strtod(v.s.c_str(), nullptr);
  }
  std::string str(const Val& v) {
    if (v.kind == Val::Num) return fmt_num(v.n, globals_["CONVFMT"].s.empty() ? "%.6g" : globals_["CONVFMT"].s);
    return v.s;
  }
  std::string out_str(const Val& v) {
    if (v.kind == Val::Num) return fmt_num(v.n, globals_["OFMT"].s.empty() ? "%.6g" : globals_["OFMT"].s);
    return v.s;
  }
  bool truthy(const Val& v) { if (v.kind == Val::Num) return v.n != 0; if (v.kind == Val::StrNum) return v.n != 0; return !v.s.empty(); }

  std::shared_ptr<std::regex> regex(const std::string& src) {
    auto it = re_cache_.find(src);
    if (it != re_cache_.end()) return it->second;
    try {
      auto r = std::make_shared<std::regex>(posix_regex_to_ecma(src, true), std::regex::ECMAScript);
      re_cache_[src] = r;
      return r;
    } catch (const std::regex_error& e) { throw AwkError{"invalid regex /" + src + "/: " + e.what()}; }
  }

  // ---- records and fields
  // More of the main input onto input_; false at its end. What is printed so far goes out first, since the read
  // may wait on a slow producer.
  bool read_more() {
    if (in_eof_) return false;
    input_.erase(0, ipos_);
    ipos_ = 0;
    flush_main();
    char b[1 << 16];
    long k = os::read_fd(in_fd_, b, sizeof b);
    if (k <= 0) { in_eof_ = true; return false; }
    input_.append(b, static_cast<size_t>(k));
    return true;
  }
  bool next_record(std::string& rec) {
    std::string rs = str(get_var("RS"));
    bool paragraph = rs.empty();   // records end at blank lines, and blank lines before a record are skipped
    if (paragraph) {
      for (;;) {
        while (ipos_ < input_.size() && input_[ipos_] == '\n') ++ipos_;
        if (ipos_ < input_.size() || !read_more()) break;
      }
      rs = "\n\n";
    }
    // Read until the separator is seen or the input ends. A search resumes where the last one stopped (less a
    // separator's length, which may straddle two reads), so a long record is not scanned again for every block.
    size_t from = ipos_, e;
    for (;;) {
      e = rs.size() == 1 ? input_.find(rs[0], from) : input_.find(rs, from);
      if (e != std::string::npos) break;
      size_t seen = input_.size() - ipos_;
      if (!read_more()) break;
      from = ipos_ + (seen >= rs.size() ? seen - (rs.size() - 1) : 0);
    }
    if (ipos_ >= input_.size()) return false;
    rec = input_.substr(ipos_, e == std::string::npos ? std::string::npos : e - ipos_);
    ipos_ = e == std::string::npos ? input_.size() : e + rs.size();
    if (paragraph) while (!rec.empty() && rec.back() == '\n') rec.pop_back();
    else if (rs == "\n" && !rec.empty() && rec.back() == '\r') rec.pop_back();
    return true;
  }
  void set_record(const std::string& r) { record_ = r; fields_valid_ = false; }
  void split_fields() {
    if (fields_valid_) return;
    fields_.clear();
    split_into(record_, str(get_var("FS")), fields_);
    fields_valid_ = true;
  }
  void split_into(const std::string& s, const std::string& fs, std::vector<std::string>& out) {
    if (fs == " ") {
      size_t i = 0;
      while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n')) ++i;
        if (i >= s.size()) break;
        size_t j = i;
        while (j < s.size() && !(s[j] == ' ' || s[j] == '\t' || s[j] == '\n')) ++j;
        out.push_back(s.substr(i, j - i));
        i = j;
      }
      return;
    }
    if (s.empty()) return;
    if (fs.size() == 1 && fs != "\\") {
      size_t a = 0;
      for (;;) { size_t b = s.find(fs[0], a); out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a)); if (b == std::string::npos) break; a = b + 1; }
      return;
    }
    auto re = regex(fs);
    auto begin = s.cbegin();
    std::smatch m;
    while (std::regex_search(begin, s.cend(), m, *re) && m.length(0) > 0) {
      out.emplace_back(begin, m[0].first);
      begin = m[0].second;
    }
    out.emplace_back(begin, s.cend());
  }
  void rebuild_record() {
    std::string ofs = str(get_var("OFS"));
    std::string r;
    for (size_t i = 0; i < fields_.size(); ++i) r += (i ? ofs : "") + fields_[i];
    record_ = r;
    fields_valid_ = true;
  }
  Val get_field(long long i) {
    if (i == 0) return strnum(record_);
    split_fields();
    if (i < 0) throw AwkError{"negative field index"};
    if (static_cast<size_t>(i) > fields_.size()) return Val::str("");
    return strnum(fields_[static_cast<size_t>(i - 1)]);
  }
  void set_field(long long i, const std::string& v) {
    if (i == 0) { set_record(v); return; }
    split_fields();
    if (static_cast<size_t>(i) > fields_.size()) fields_.resize(static_cast<size_t>(i));
    fields_[static_cast<size_t>(i - 1)] = v;
    rebuild_record();
  }

  // ---- output
  void out_main(const std::string& s) { out_buf_ += s; if (out_buf_.size() > (1 << 16)) { io_.write(out_buf_); out_buf_.clear(); } }
  void flush_main() { if (!out_buf_.empty()) { io_.write(out_buf_); out_buf_.clear(); } }
  void out_to(const std::string& redir, const std::string& dest, const std::string& s) {
    if (redir.empty()) { out_main(s); return; }
    if (redir == "|") { pipes_out_[dest] += s; return; }
    if (dest == "/dev/stdout" || dest == "-") { out_main(s); return; }
    if (dest == "/dev/stderr") { flush_main(); io_.ewrite(s); return; }
    auto it = files_.find(dest);
    if (it == files_.end()) {
      std::string e;
      Fd f = os::open_file(host_path(st_.cwd, dest), redir == ">>" ? os::Open::Append : os::Open::Write, &e);
      if (f == kNoFd) throw AwkError{"can't redirect to " + dest + ": " + e};
      it = files_.emplace(dest, f).first;
    }
    os::write_all(it->second, s.data(), s.size());
  }
  int run_pipe(const std::string& cmd, const std::string& input) {
    flush_main();
    Fd r, w;
    os::make_pipe(r, w);
    std::thread t([w, input]() { os::write_all(w, input.data(), input.size()); os::close_fd(w); });
    State copy = st_;
    int s = sh_.run_text(copy, cmd, Io{r, io_.out, io_.err});
    t.join();
    os::close_fd(r);
    return s;
  }
  int close_stream(const std::string& name) {
    int s = -1;
    auto f = files_.find(name);
    if (f != files_.end()) { os::close_fd(f->second); files_.erase(f); s = 0; }
    auto p = pipes_out_.find(name);
    if (p != pipes_out_.end()) { s = run_pipe(name, p->second); pipes_out_.erase(p); }
    if (readers_.erase(name)) s = 0;
    return s;
  }
  void flush_all() {
    flush_main();
    for (auto& [k, v] : pipes_out_) run_pipe(k, v);
    pipes_out_.clear();
    for (auto& [k, f] : files_) os::close_fd(f);
    files_.clear();
  }

  // ---- patterns
  bool rule_matches(Rule& r) {
    if (!r.pattern) return true;
    if (!r.pattern2) return truthy(eval(r.pattern));
    if (r.range_active) { if (truthy(eval(r.pattern2))) r.range_active = false; return true; }
    if (truthy(eval(r.pattern))) { r.range_active = !truthy(eval(r.pattern2)); return true; }
    return false;
  }

  std::string subscript(const std::vector<EP>& args) {
    std::string key;
    std::string sep = str(get_var("SUBSEP"));
    for (size_t i = 0; i < args.size(); ++i) key += (i ? sep : "") + str(eval(args[i]));
    return key;
  }

  Val assign_to(const EP& target, const Val& v) {
    if (target->kind == Expr::Var) set_var(target->s, v);
    else if (target->kind == Expr::Field) set_field(static_cast<long long>(num(eval(target->a))), str(v));
    else if (target->kind == Expr::Index) array(target->s)[subscript(target->args)] = v;
    else if (target->kind == Expr::Group) return assign_to(target->a, v);
    return v;
  }

  bool match_val(const Val& s, const EP& re_e) {
    std::string src = re_e->kind == Expr::Regex ? re_e->s : str(eval(re_e));
    return std::regex_search(str(s), *regex(src));
  }

  int compare(const Val& a, const Val& b) {
    bool an = a.kind == Val::Num || a.kind == Val::StrNum, bn = b.kind == Val::Num || b.kind == Val::StrNum;
    if (an && bn) { double x = num(a), y = num(b); return x < y ? -1 : x > y ? 1 : 0; }
    std::string x = str(a), y = str(b);
    return x < y ? -1 : x > y ? 1 : 0;
  }

  std::string format(const std::string& f, const std::vector<Val>& args) {
    std::string out;
    size_t ai = 0;
    for (size_t i = 0; i < f.size(); ++i) {
      if (f[i] != '%') { out += f[i]; continue; }
      if (i + 1 < f.size() && f[i + 1] == '%') { out += '%'; ++i; continue; }
      std::string spec = "%";
      size_t j = i + 1;
      while (j < f.size() && std::strchr("-+ #0", f[j])) spec += f[j++];
      while (j < f.size() && (std::isdigit(static_cast<unsigned char>(f[j])) || f[j] == '*')) { if (f[j] == '*') { spec += std::to_string(static_cast<long long>(ai < args.size() ? num(args[ai++]) : 0)); ++j; } else spec += f[j++]; }
      if (j < f.size() && f[j] == '.') { spec += f[j++]; while (j < f.size() && (std::isdigit(static_cast<unsigned char>(f[j])) || f[j] == '*')) { if (f[j] == '*') { spec += std::to_string(static_cast<long long>(ai < args.size() ? num(args[ai++]) : 0)); ++j; } else spec += f[j++]; } }
      while (j < f.size() && std::strchr("hlLqjzt", f[j])) ++j;
      if (j >= f.size()) { out += spec; break; }
      char c = f[j];
      i = j;
      Val v = ai < args.size() ? args[ai++] : Val::str("");
      char buf[1024];
      if (c == 'd' || c == 'i') { std::snprintf(buf, sizeof buf, (spec + "lld").c_str(), static_cast<long long>(num(v))); out += buf; }
      else if (c == 'o' || c == 'x' || c == 'X' || c == 'u') { std::snprintf(buf, sizeof buf, (spec + "ll" + c).c_str(), static_cast<unsigned long long>(static_cast<long long>(num(v)))); out += buf; }
      else if (c == 'c') { if (v.kind == Val::Num) out += static_cast<char>(static_cast<int>(num(v))); else { std::string s = str(v); if (!s.empty()) out += s[0]; } }
      else if (c == 's') { std::string s = str(v); if (s.size() < 900) { std::snprintf(buf, sizeof buf, (spec + "s").c_str(), s.c_str()); out += buf; } else out += s; }
      else if (std::strchr("eEfFgGaA", c)) { std::snprintf(buf, sizeof buf, (spec + c).c_str(), num(v)); out += buf; }
      else { out += spec; out += c; }
    }
    return out;
  }

  int sub_impl(const EP& re_e, const std::string& repl, const EP& target, bool global) {
    std::string src = re_e->kind == Expr::Regex ? re_e->s : str(eval(re_e));
    auto re = regex(src);
    std::string s = target ? str(eval(target)) : record_;
    std::string out;
    int count = 0;
    auto begin = s.cbegin();
    std::smatch m;
    auto flags = std::regex_constants::match_default;
    while (std::regex_search(begin, s.cend(), m, *re, flags)) {
      out.append(begin, m[0].first);
      for (size_t i = 0; i < repl.size(); ++i) {
        if (repl[i] == '\\' && i + 1 < repl.size() && (repl[i + 1] == '&' || repl[i + 1] == '\\')) { out += repl[++i]; continue; }
        if (repl[i] == '&') out += m.str(0); else out += repl[i];
      }
      ++count;
      if (m.length(0) == 0) {
        if (m[0].second == s.cend()) { begin = s.cend(); break; }
        out += *m[0].second;
        begin = m[0].second + 1;
      } else begin = m[0].second;
      flags = std::regex_constants::match_prev_avail;
      if (!global) break;
    }
    if (!count) return 0;
    out.append(begin, s.cend());
    if (target) assign_to(target, Val::str(out)); else set_record(out);
    return count;
  }

  Val builtin(const Expr& e) {
    const std::string& f = e.s;
    auto arg = [&](size_t i) { return i < e.args.size() ? eval(e.args[i]) : Val::str(""); };
    if (f == "length") {
      if (e.args.empty()) return Val::num(static_cast<double>(record_.size()));
      if (e.args[0]->kind == Expr::Var && (arrays_.count(e.args[0]->s) || (!frame_arrays_.empty() && frame_arrays_.back().count(e.args[0]->s))))
        return Val::num(static_cast<double>(array(e.args[0]->s).size()));
      return Val::num(static_cast<double>(str(arg(0)).size()));
    }
    if (f == "substr") {
      std::string s = str(arg(0));
      double m = std::round(num(arg(1)));
      double n = e.args.size() > 2 ? std::round(num(arg(2))) : 1e18;
      double start = std::max(1.0, m), end = std::min(static_cast<double>(s.size()) + 1, m + n);
      if (end <= start) return Val::str("");
      return Val::str(s.substr(static_cast<size_t>(start - 1), static_cast<size_t>(end - start)));
    }
    if (f == "index") { size_t at = str(arg(0)).find(str(arg(1))); return Val::num(at == std::string::npos ? 0 : static_cast<double>(at + 1)); }
    if (f == "split") {
      std::string s = str(arg(0));
      if (e.args.size() < 2 || e.args[1]->kind != Expr::Var) throw AwkError{"split: second argument must be an array"};
      Array& a = array(e.args[1]->s);
      a.clear();
      std::vector<std::string> parts;
      std::string fs = e.args.size() > 2 ? (e.args[2]->kind == Expr::Regex ? e.args[2]->s : str(arg(2))) : str(get_var("FS"));
      split_into(s, fs, parts);
      for (size_t i = 0; i < parts.size(); ++i) a[std::to_string(i + 1)] = strnum(parts[i]);
      return Val::num(static_cast<double>(parts.size()));
    }
    if (f == "sub" || f == "gsub") return Val::num(sub_impl(e.args[0], str(arg(1)), e.args.size() > 2 ? e.args[2] : nullptr, f == "gsub"));
    if (f == "match") {
      std::string s = str(arg(0));
      std::string src = e.args[1]->kind == Expr::Regex ? e.args[1]->s : str(arg(1));
      std::smatch m;
      if (std::regex_search(s, m, *regex(src))) {
        set_var("RSTART", Val::num(static_cast<double>(m.position(0) + 1)));
        set_var("RLENGTH", Val::num(static_cast<double>(m.length(0))));
        if (e.args.size() > 2 && e.args[2]->kind == Expr::Var) {
          Array& a = array(e.args[2]->s);
          a.clear();
          for (size_t i = 0; i < m.size(); ++i) a[std::to_string(i)] = Val::str(m.str(i));
        }
        return Val::num(static_cast<double>(m.position(0) + 1));
      }
      set_var("RSTART", Val::num(0)); set_var("RLENGTH", Val::num(-1));
      return Val::num(0);
    }
    if (f == "sprintf") { std::vector<Val> v; for (size_t i = 1; i < e.args.size(); ++i) v.push_back(eval(e.args[i])); return Val::str(format(str(arg(0)), v)); }
    if (f == "tolower" || f == "toupper") { std::string s = str(arg(0)); for (auto& c : s) c = static_cast<char>(f == "tolower" ? std::tolower(static_cast<unsigned char>(c)) : std::toupper(static_cast<unsigned char>(c))); return Val::str(s); }
    if (f == "int") return Val::num(std::trunc(num(arg(0))));
    if (f == "sqrt") return Val::num(std::sqrt(num(arg(0))));
    if (f == "exp") return Val::num(std::exp(num(arg(0))));
    if (f == "log") return Val::num(std::log(num(arg(0))));
    if (f == "sin") return Val::num(std::sin(num(arg(0))));
    if (f == "cos") return Val::num(std::cos(num(arg(0))));
    if (f == "atan2") return Val::num(std::atan2(num(arg(0)), num(arg(1))));
    if (f == "rand") { rand_seed_ = rand_seed_ * 6364136223846793005ULL + 1442695040888963407ULL; return Val::num(static_cast<double>(rand_seed_ >> 11) / 9007199254740992.0); }
    if (f == "srand") { double prev = static_cast<double>(rand_seed_); rand_seed_ = e.args.empty() ? static_cast<unsigned long long>(os::now_seconds()) : static_cast<unsigned long long>(num(arg(0))); return Val::num(prev); }
    if (f == "system") { flush_main(); State copy = st_; return Val::num(sh_.run_text(copy, str(arg(0)), io_)); }
    if (f == "close") return Val::num(close_stream(str(arg(0))));
    if (f == "fflush") { flush_main(); return Val::num(0); }
    throw AwkError{"unknown function " + f};
  }

  Val call(const Expr& e) {
    auto it = p_.funcs.find(e.s);
    if (it == p_.funcs.end()) throw AwkError{"calling undefined function " + e.s};
    const Func& fn = it->second;
    std::unordered_map<std::string, Val> frame;
    std::unordered_map<std::string, std::shared_ptr<Array>> farr;
    for (size_t i = 0; i < fn.params.size(); ++i) {
      if (i < e.args.size() && e.args[i]->kind == Expr::Var) {
        const std::string& n = e.args[i]->s;
        // an array argument is passed by reference
        bool is_array = arrays_.count(n) || (!frame_arrays_.empty() && frame_arrays_.back().count(n));
        if (is_array) {
          std::shared_ptr<Array> ref;
          if (!frame_arrays_.empty() && frame_arrays_.back().count(n)) ref = frame_arrays_.back()[n];
          else ref = std::shared_ptr<Array>(&arrays_[n], [](Array*) {});
          farr[fn.params[i]] = ref;
          continue;
        }
      }
      if (i < e.args.size()) frame[fn.params[i]] = eval(e.args[i]);
      else { frame[fn.params[i]] = Val::str(""); farr[fn.params[i]] = std::make_shared<Array>(); }
    }
    // a local that is used as an array is an empty array of its own
    frames_.push_back(frame);
    frame_arrays_.push_back(farr);
    Val result = Val::str("");
    try { exec(fn.body); }
    catch (ReturnAwk& r) { result = r.v; }
    catch (...) { frames_.pop_back(); frame_arrays_.pop_back(); throw; }
    frames_.pop_back();
    frame_arrays_.pop_back();
    return result;
  }

  Val do_getline(const Expr& e) {
    std::string line;
    if (e.getline_kind == 0) {
      if (!next_record(line)) return Val::num(0);
      set_var("NR", Val::num(num(get_var("NR")) + 1));
      set_var("FNR", Val::num(num(get_var("FNR")) + 1));
      if (e.a) assign_to(e.a, strnum(line)); else set_record(line);
      return Val::num(1);
    }
    std::string name = str(eval(e.b));
    auto it = readers_.find(name);
    if (it == readers_.end()) {
      std::string data;
      if (e.getline_kind == 1) {
        if (name == "-" || name == "/dev/stdin") data = io_.read_all();
        else if (!os::read_whole(host_path(st_.cwd, name), data, nullptr)) return Val::num(-1);
      } else {
        flush_main();
        State copy = st_;
        data = sh_.command_substitution(copy, name, io_) + "\n";
        if (data == "\n") data.clear();
      }
      it = readers_.emplace(name, std::make_pair(data, size_t{0})).first;
    }
    auto& [data, pos] = it->second;
    if (pos >= data.size()) return Val::num(0);
    size_t nl = data.find('\n', pos);
    line = data.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = nl == std::string::npos ? data.size() : nl + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (e.a) assign_to(e.a, strnum(line));
    else { set_record(line); set_var("NR", Val::num(num(get_var("NR")) + 1)); }
    return Val::num(1);
  }

  Val eval(const EP& ep) {
    const Expr& e = *ep;
    switch (e.kind) {
      case Expr::Num: return Val::num(e.n);
      case Expr::Str: return Val::str(e.s);
      case Expr::Regex: return Val::num(std::regex_search(record_, *regex(e.s)) ? 1 : 0);
      case Expr::Group: return eval(e.a);
      case Expr::Var: return get_var(e.s);
      case Expr::Field: return get_field(static_cast<long long>(num(eval(e.a))));
      case Expr::Index: {
        Array& a = array(e.s);
        std::string k = subscript(e.args);
        auto it = a.find(k);
        if (it == a.end()) { a[k] = Val::str(""); return Val::str(""); }
        return it->second;
      }
      case Expr::In: {
        Array& a = array(e.s);
        return Val::num(a.count(subscript(e.args)) ? 1 : 0);
      }
      case Expr::Assign: {
        if (e.s == "=") return assign_to(e.a, eval(e.b));
        double l = num(eval(e.a)), r = num(eval(e.b)), v = 0;
        if (e.s == "+=") v = l + r; else if (e.s == "-=") v = l - r; else if (e.s == "*=") v = l * r;
        else if (e.s == "/=") { if (r == 0) throw AwkError{"division by zero in /="}; v = l / r; }
        else if (e.s == "%=") { if (r == 0) throw AwkError{"division by zero in %="}; v = std::fmod(l, r); }
        else v = std::pow(l, r);
        return assign_to(e.a, Val::num(v));
      }
      case Expr::Cond: return truthy(eval(e.a)) ? eval(e.b) : eval(e.c);
      case Expr::And: return Val::num(truthy(eval(e.a)) && truthy(eval(e.b)) ? 1 : 0);
      case Expr::Or: return Val::num(truthy(eval(e.a)) || truthy(eval(e.b)) ? 1 : 0);
      case Expr::Not: return Val::num(truthy(eval(e.a)) ? 0 : 1);
      case Expr::Neg: return Val::num(-num(eval(e.a)));
      case Expr::Plus: return Val::num(num(eval(e.a)));
      case Expr::Bin: {
        double l = num(eval(e.a)), r = num(eval(e.b));
        if (e.s == "+") return Val::num(l + r);
        if (e.s == "-") return Val::num(l - r);
        if (e.s == "*") return Val::num(l * r);
        if (e.s == "/") { if (r == 0) throw AwkError{"division by zero"}; return Val::num(l / r); }
        if (e.s == "%") { if (r == 0) throw AwkError{"division by zero in %"}; return Val::num(std::fmod(l, r)); }
        return Val::num(std::pow(l, r));
      }
      case Expr::Cmp: {
        int c = compare(eval(e.a), eval(e.b));
        bool r = e.s == "<" ? c < 0 : e.s == "<=" ? c <= 0 : e.s == ">" ? c > 0 : e.s == ">=" ? c >= 0 : e.s == "==" ? c == 0 : c != 0;
        return Val::num(r ? 1 : 0);
      }
      case Expr::Match: { bool m = match_val(eval(e.a), e.b); return Val::num((e.s == "~") == m ? 1 : 0); }
      case Expr::Concat: return Val::str(str(eval(e.a)) + str(eval(e.b)));
      case Expr::PreInc: case Expr::PreDec: return assign_to(e.a, Val::num(num(eval(e.a)) + (e.kind == Expr::PreInc ? 1 : -1)));
      case Expr::PostInc: case Expr::PostDec: { double v = num(eval(e.a)); assign_to(e.a, Val::num(v + (e.kind == Expr::PostInc ? 1 : -1))); return Val::num(v); }
      case Expr::Call: return call(e);
      case Expr::Builtin: return builtin(e);
      case Expr::Getline: return do_getline(e);
    }
    return Val::str("");
  }

  void exec(const SP& sp) {
    if (!sp) return;
    const Stmt& s = *sp;
    switch (s.kind) {
      case Stmt::Block: for (const auto& k : s.kids) exec(k); break;
      case Stmt::Expr_: eval(s.e); break;
      case Stmt::Print: {
        std::string line;
        if (s.args.empty()) line = record_;
        else { std::string ofs = str(get_var("OFS")); for (size_t i = 0; i < s.args.size(); ++i) line += (i ? ofs : "") + out_str(eval(s.args[i])); }
        line += str(get_var("ORS"));
        out_to(s.redir, s.dest ? str(eval(s.dest)) : "", line);
        break;
      }
      case Stmt::Printf: {
        if (s.args.empty()) throw AwkError{"printf: no format"};
        std::vector<Val> v;
        for (size_t i = 1; i < s.args.size(); ++i) v.push_back(eval(s.args[i]));
        out_to(s.redir, s.dest ? str(eval(s.dest)) : "", format(str(eval(s.args[0])), v));
        break;
      }
      case Stmt::If: if (truthy(eval(s.e))) exec(s.body); else exec(s.els); break;
      case Stmt::While:
        while (truthy(eval(s.e))) { try { exec(s.body); } catch (const BreakAwk&) { break; } catch (const ContinueAwk&) {} }
        break;
      case Stmt::Do:
        do { try { exec(s.body); } catch (const BreakAwk&) { break; } catch (const ContinueAwk&) {} } while (truthy(eval(s.e)));
        break;
      case Stmt::For:
        for (s.e ? (void)eval(s.e) : (void)0; s.e2 ? truthy(eval(s.e2)) : true; s.e3 ? (void)eval(s.e3) : (void)0) {
          try { exec(s.body); } catch (const BreakAwk&) { break; } catch (const ContinueAwk&) {}
        }
        break;
      case Stmt::ForIn: {
        std::vector<std::string> keys;
        for (const auto& [k, v] : array(s.e->s)) keys.push_back(k);
        for (const auto& k : keys) {
          set_var(s.var, strnum(k));
          try { exec(s.body); } catch (const BreakAwk&) { break; } catch (const ContinueAwk&) {}
        }
        break;
      }
      case Stmt::Next: throw NextSignal{};
      case Stmt::NextFile: throw NextFileSignal{};
      case Stmt::Exit: throw ExitAwk{s.e ? static_cast<int>(num(eval(s.e))) : 0};
      case Stmt::Return: throw ReturnAwk{s.e ? eval(s.e) : Val::str("")};
      case Stmt::Break: throw BreakAwk{};
      case Stmt::Continue: throw ContinueAwk{};
      case Stmt::Delete:
        if (s.args.empty()) array(s.e->s).clear();
        else array(s.e->s).erase(subscript(s.args));
        break;
      case Stmt::Getline: break;
    }
  }
};

}  // namespace

int util_awk(Shell& sh, State& st, const std::vector<std::string>& argv, const Io& io) {
  std::string program;
  bool have_program = false;
  std::vector<std::pair<std::string, std::string>> assigns;
  std::string fs;
  bool have_fs = false;
  std::vector<std::string> files;
  for (size_t i = 1; i < argv.size(); ++i) {
    const std::string& a = argv[i];
    if (!have_program && a == "--") continue;
    if (!have_program && a.size() > 1 && a[0] == '-' && a != "-") {
      if (a.rfind("-F", 0) == 0) { fs = a.size() > 2 ? a.substr(2) : (i + 1 < argv.size() ? argv[++i] : ""); have_fs = true; continue; }
      if (a.rfind("-v", 0) == 0) {
        std::string kv = a.size() > 2 ? a.substr(2) : (i + 1 < argv.size() ? argv[++i] : "");
        size_t eq = kv.find('=');
        if (eq != std::string::npos) assigns.push_back({kv.substr(0, eq), kv.substr(eq + 1)});
        continue;
      }
      if (a.rfind("-f", 0) == 0) {
        std::string f = a.size() > 2 ? a.substr(2) : (i + 1 < argv.size() ? argv[++i] : "");
        std::string d;
        os::read_whole(host_path(st.cwd, f), d, nullptr);
        program += d + "\n";
        have_program = true;
        continue;
      }
      continue;   // --posix, -e and other options: accepted
    }
    if (!have_program) { program = a; have_program = true; continue; }
    files.push_back(a);
  }
  if (!have_program) { io.ewrite("usage: awk [-F fs][-v var=value][prog | -f progfile][file ...]\n"); return 2; }
  try {
    Parser p(program);
    p.program();
    Interp in(sh, st, io, p);
    if (have_fs) {
      if (fs == "t") fs = "\t";
      std::string f2;
      for (size_t i = 0; i < fs.size(); ++i) { if (fs[i] == '\\' && i + 1 < fs.size() && fs[i + 1] == 't') { f2 += '\t'; ++i; } else f2 += fs[i]; }
      assigns.insert(assigns.begin(), {"FS", f2});
    }
    return in.run(files, assigns);
  } catch (const AwkError& e) {
    io.ewrite("awk: " + e.msg + "\n");
    return 2;
  } catch (const std::regex_error& e) {
    io.ewrite(std::string("awk: ") + e.what() + "\n");
    return 2;
  }
}

}  // namespace aiossh
