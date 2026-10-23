// A face: the language, the runtime, the layout and the paint. See face.hpp.
//
// Everything a dream needs to be a workstation face is here, and nothing a web
// page needs: there is no box model to get wrong because the widgets are the
// model. A row shares its width between its children; a table sizes its
// columns from what is in them; a word that does not fit is cut with an
// ellipsis and the cut is REPORTED, so the layout cannot draw two words on the
// same pixels and the mind is told when it asked for more than fits.
#include "aios/userland/face.hpp"
#include "aios/hal/sync.hpp"
#include <cctype>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

#include "aios/userland/pixels.hpp"
#include "aios/userland/web_type.hpp"

namespace aios::userland::face {

// ------------------------------------------------------------------ values --

const Value* Value::field(const std::string& name) const {
  if (k != K::Rec || !rec) return nullptr;
  for (const auto& kv : *rec) if (kv.first == name) return &kv.second;
  return nullptr;
}
void Value::set_field(const std::string& name, Value v) {
  if (k != K::Rec) { k = K::Rec; rec = std::make_shared<std::vector<std::pair<std::string, Value>>>(); }
  for (auto& kv : *rec) if (kv.first == name) { kv.second = std::move(v); return; }
  rec->emplace_back(name, std::move(v));
}
bool Value::truthy() const {
  switch (k) {
    case K::Null: return false;
    case K::Num: return n != 0 && !std::isnan(n);
    case K::Str: return !s.empty();
    case K::Bool: return b;
    case K::List: return list && !list->empty();
    case K::Rec: return true;
  }
  return false;
}
std::string fmt_num(double d) {
  if (std::isnan(d)) return "NaN";
  if (std::isinf(d)) return d > 0 ? "Infinity" : "-Infinity";
  // Any integral value that fits a long long prints in full. The cut-off used
  // to be 1e15, which silently turned every nanosecond timestamp into
  // "1.757998234e+18" — and since AQL's tokenizer stops at the 'e', a face
  // could not express `AS OF timestamp(<ns>)` at all. The guard precedes the
  // cast because converting a double at or past 2^63 is undefined.
  if (std::fabs(d) < 9.2e18 && d == (double)(long long)d) {
    char b[32];
    std::snprintf(b, sizeof b, "%lld", (long long)d);
    return b;
  }
  char b[40];
  std::snprintf(b, sizeof b, "%.10g", d);
  return b;
}
std::string Value::text() const {
  switch (k) {
    case K::Null: return "";
    case K::Num: return fmt_num(n);
    case K::Str: return s;
    case K::Bool: return b ? "true" : "false";
    case K::List: {
      std::string o;
      if (list) for (size_t i = 0; i < list->size(); ++i) { if (i) o += ", "; o += (*list)[i].text(); }
      return o;
    }
    case K::Rec: return to_json(*this);
  }
  return "";
}
double Value::number() const {
  switch (k) {
    case K::Num: return n;
    case K::Bool: return b ? 1 : 0;
    case K::Str: { char* end = nullptr; double d = std::strtod(s.c_str(), &end); return end == s.c_str() ? 0 : d; }
    case K::List: return list ? (double)list->size() : 0;
    default: return 0;
  }
}

std::string json_quote(const std::string& s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else if (c == '\t') o += "\\t";
    else if ((unsigned char)c < 32) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", (unsigned)(unsigned char)c); o += b; }
    else o += c;
  }
  return o + "\"";
}
std::string to_json(const Value& v) {
  switch (v.k) {
    case Value::K::Null: return "null";
    case Value::K::Num: return std::isfinite(v.n) ? fmt_num(v.n) : "null";
    case Value::K::Str: return json_quote(v.s);
    case Value::K::Bool: return v.b ? "true" : "false";
    case Value::K::List: {
      std::string o = "[";
      if (v.list) for (size_t i = 0; i < v.list->size(); ++i) { if (i) o += ","; o += to_json((*v.list)[i]); }
      return o + "]";
    }
    case Value::K::Rec: {
      std::string o = "{";
      if (v.rec) for (size_t i = 0; i < v.rec->size(); ++i) { if (i) o += ","; o += json_quote((*v.rec)[i].first) + ":" + to_json((*v.rec)[i].second); }
      return o + "}";
    }
  }
  return "null";
}

namespace {

struct JsonIn {
  const std::string& t;
  size_t i = 0;
  void ws() { while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) ++i; }
  Value parse(int depth = 0) {
    ws();
    if (depth > 64 || i >= t.size()) return Value::null();
    char c = t[i];
    if (c == '{') {
      ++i;
      Value r = Value::make_rec();
      ws();
      if (i < t.size() && t[i] == '}') { ++i; return r; }
      while (i < t.size()) {
        ws();
        Value key = parse(depth + 1);
        ws();
        if (i < t.size() && t[i] == ':') ++i;
        Value val = parse(depth + 1);
        r.set_field(key.text(), val);
        ws();
        if (i < t.size() && t[i] == ',') { ++i; continue; }
        if (i < t.size() && t[i] == '}') { ++i; break; }
        break;
      }
      return r;
    }
    if (c == '[') {
      ++i;
      Value l = Value::make_list();
      ws();
      if (i < t.size() && t[i] == ']') { ++i; return l; }
      while (i < t.size()) {
        l.list->push_back(parse(depth + 1));
        ws();
        if (i < t.size() && t[i] == ',') { ++i; continue; }
        if (i < t.size() && t[i] == ']') { ++i; break; }
        break;
      }
      return l;
    }
    if (c == '"') {
      ++i;
      std::string s;
      while (i < t.size() && t[i] != '"') {
        if (t[i] == '\\' && i + 1 < t.size()) {
          ++i;
          char e = t[i++];
          if (e == 'n') s += '\n';
          else if (e == 't') s += '\t';
          else if (e == 'r') s += '\r';
          else if (e == 'u' && i + 4 <= t.size()) {
            long cp = std::strtol(t.substr(i, 4).c_str(), nullptr, 16);
            i += 4;
            if (cp < 0x80) s += (char)cp;
            else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
            else { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
          } else s += e;
          continue;
        }
        s += t[i++];
      }
      if (i < t.size()) ++i;
      return Value::str(s);
    }
    if (t.compare(i, 4, "true") == 0) { i += 4; return Value::boolean(true); }
    if (t.compare(i, 5, "false") == 0) { i += 5; return Value::boolean(false); }
    if (t.compare(i, 4, "null") == 0) { i += 4; return Value::null(); }
    char* end = nullptr;
    double d = std::strtod(t.c_str() + i, &end);
    if (end == t.c_str() + i) { ++i; return Value::null(); }
    i = (size_t)(end - t.c_str());
    return Value::num(d);
  }
};

}  // namespace

Value from_json(const std::string& text) {
  JsonIn in{text};
  return in.parse();
}

// ------------------------------------------------------------- expressions --

struct Expr {
  enum class K : uint8_t { Num, Str, Interp, Var, Field, Index, Call, Unary, Binary, List, Rec, Bool, Null } k = K::Null;
  double n = 0;
  std::string s;                       // Str text; Var name; Field name; Call name; Unary/Binary op
  std::vector<ExprP> kids;             // Interp parts; Call args; Unary/Binary operands; List items; Index [obj, idx]; Field [obj]
  std::vector<std::string> keys;       // Rec keys (values in kids)
};

struct Stmt {
  enum class K : uint8_t { Set, Put, Clear, Focus, Say, If, For, Paint, Call, Stop, Delete, Update, Spawn, Emit, Undo, Redo, Snapshot } k = K::Say;
  std::string name;                    // Set var; Put kind; Clear/Focus field; For var; Paint verb; Delete/Update kind; Task name
  std::string target;                  // Emit target channel
  ExprP expr;                          // Set value; Put record; Say text; If cond; For list; Delete ID; Emit value; Snapshot label
  std::vector<ExprP> args;             // Paint args; Call args; Update ID & record
  std::vector<StmtP> body;             // If then; For body; Spawn body
  std::vector<StmtP> otherwise;        // If else
  int line = 0;
};

namespace {

bool is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool is_ident(char c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }
bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && is_space(s[a])) ++a;
  while (b > a && is_space(s[b - 1])) --b;
  return s.substr(a, b - a);
}

size_t string_end(const std::string& s, size_t open);

struct ExprParser {
  const std::string& src;
  size_t i = 0;
  std::string err;
  explicit ExprParser(const std::string& s) : src(s) {}

  void ws() { while (i < src.size() && is_space(src[i])) ++i; }
  bool peek(const char* tok) {
    ws();
    return src.compare(i, std::strlen(tok), tok) == 0;
  }
  bool eat(const char* tok) {
    if (!peek(tok)) return false;
    i += std::strlen(tok);
    return true;
  }
  bool eat_word(const char* w) {
    ws();
    size_t n = std::strlen(w);
    if (src.compare(i, n, w) != 0) return false;
    if (i + n < src.size() && is_ident(src[i + n])) return false;
    i += n;
    return true;
  }
  ExprP make(Expr::K k) { auto e = std::make_shared<Expr>(); e->k = k; return e; }
  ExprP fail(const std::string& m) { if (err.empty()) err = m; return make(Expr::K::Null); }

  ExprP parse_all() {
    ExprP e = parse_or();
    ws();
    if (err.empty() && i < src.size()) err = "unexpected '" + src.substr(i, 12) + "'";
    return e;
  }
  ExprP parse_or() {
    ExprP l = parse_and();
    while (eat_word("or") || eat("||")) { ExprP r = parse_and(); auto b = make(Expr::K::Binary); b->s = "or"; b->kids = {l, r}; l = b; }
    return l;
  }
  ExprP parse_and() {
    ExprP l = parse_not();
    while (eat_word("and") || eat("&&")) { ExprP r = parse_not(); auto b = make(Expr::K::Binary); b->s = "and"; b->kids = {l, r}; l = b; }
    return l;
  }
  ExprP parse_not() {
    if (eat_word("not") || eat("!")) { ExprP e = parse_not(); auto u = make(Expr::K::Unary); u->s = "not"; u->kids = {e}; return u; }
    return parse_cmp();
  }
  ExprP parse_cmp() {
    ExprP l = parse_add();
    for (;;) {
      const char* ops[] = {"==", "!=", "<=", ">=", "<", ">", "="};
      bool got = false;
      for (const char* op : ops) {
        if (eat(op)) {
          ExprP r = parse_add();
          auto b = make(Expr::K::Binary);
          b->s = std::string(op) == "=" ? "==" : op;
          b->kids = {l, r};
          l = b;
          got = true;
          break;
        }
      }
      if (!got) break;
    }
    return l;
  }
  ExprP parse_add() {
    ExprP l = parse_mul();
    for (;;) {
      if (eat("+")) { ExprP r = parse_mul(); auto b = make(Expr::K::Binary); b->s = "+"; b->kids = {l, r}; l = b; }
      else if (eat("-")) { ExprP r = parse_mul(); auto b = make(Expr::K::Binary); b->s = "-"; b->kids = {l, r}; l = b; }
      else break;
    }
    return l;
  }
  ExprP parse_mul() {
    ExprP l = parse_unary();
    for (;;) {
      if (eat("*")) { ExprP r = parse_unary(); auto b = make(Expr::K::Binary); b->s = "*"; b->kids = {l, r}; l = b; }
      else if (eat("/")) { ExprP r = parse_unary(); auto b = make(Expr::K::Binary); b->s = "/"; b->kids = {l, r}; l = b; }
      else if (eat("%")) { ExprP r = parse_unary(); auto b = make(Expr::K::Binary); b->s = "%"; b->kids = {l, r}; l = b; }
      else break;
    }
    return l;
  }
  ExprP parse_unary() {
    if (eat("-")) { ExprP e = parse_unary(); auto u = make(Expr::K::Unary); u->s = "-"; u->kids = {e}; return u; }
    return parse_postfix();
  }
  ExprP parse_postfix() {
    ExprP e = parse_primary();
    for (;;) {
      if (peek(".")) {
        i += 1;
        ws();
        size_t st = i;
        while (i < src.size() && is_ident(src[i])) ++i;
        if (st == i) return fail("a field name is expected after '.'");
        auto f = make(Expr::K::Field);
        f->s = src.substr(st, i - st);
        f->kids = {e};
        e = f;
        continue;
      }
      if (peek("[")) {
        i += 1;
        ExprP idx = parse_or();
        if (!eat("]")) return fail("']' is expected");
        auto x = make(Expr::K::Index);
        x->kids = {e, idx};
        e = x;
        continue;
      }
      break;
    }
    return e;
  }
  // The raw text of the string whose opening quote is at i-1; escapes are
  // resolved by interpolate() for the literal parts only, so an expression
  // inside {…} keeps its own strings intact.
  std::string parse_string_body(char quote) {
    (void)quote;
    const size_t open = i - 1;
    const size_t end = string_end(src, open);
    if (end == std::string::npos) { std::string s = src.substr(i); i = src.size(); return s; }
    std::string s = src.substr(i, end - i);
    i = end + 1;
    return s;
  }
  static std::string unescape(const std::string& t) {
    std::string s;
    for (size_t i = 0; i < t.size(); ++i) {
      if (t[i] == '\\' && i + 1 < t.size()) { char c = t[++i]; s += c == 'n' ? '\n' : c == 't' ? '\t' : c; continue; }
      s += t[i];
    }
    return s;
  }
  // "text {expr} more" -> Interp of Str and expressions.
  ExprP interpolate(const std::string& text) {
    std::vector<ExprP> parts;
    std::string lit;
    size_t p = 0;
    while (p < text.size()) {
      if (text[p] == '{') {
        if (!lit.empty()) { auto s = make(Expr::K::Str); s->s = unescape(lit); parts.push_back(s); lit.clear(); }
        ExprParser sub(text);
        sub.i = p + 1;
        ExprP e = sub.parse_or();
        if (!sub.err.empty()) return fail("in \"" + text + "\": " + sub.err);
        sub.ws();
        if (sub.i >= text.size() || text[sub.i] != '}') return fail("in \"" + text + "\": '}' is expected after the expression");
        parts.push_back(e);
        p = sub.i + 1;
        continue;
      }
      if (text[p] == '\\' && p + 1 < text.size()) { lit += text[p]; lit += text[p + 1]; p += 2; continue; }
      lit += text[p++];
    }
    if (parts.empty()) { auto s = make(Expr::K::Str); s->s = unescape(lit); return s; }
    if (!lit.empty()) { auto s = make(Expr::K::Str); s->s = unescape(lit); parts.push_back(s); }
    auto in = make(Expr::K::Interp);
    in->kids = parts;
    return in;
  }
  ExprP parse_primary() {
    ws();
    if (i >= src.size()) return fail("an expression is expected");
    char c = src[i];
    if (c == '"' || c == '\'') {
      ++i;
      return interpolate(parse_string_body(c));
    }
    if ((c >= '0' && c <= '9') || (c == '.' && i + 1 < src.size() && src[i + 1] >= '0' && src[i + 1] <= '9')) {
      char* end = nullptr;
      double d = std::strtod(src.c_str() + i, &end);
      i = (size_t)(end - src.c_str());
      auto n = make(Expr::K::Num);
      n->n = d;
      return n;
    }
    if (c == '(') {
      ++i;
      ExprP e = parse_or();
      if (!eat(")")) return fail("')' is expected");
      return e;
    }
    if (c == '[') {
      ++i;
      auto l = make(Expr::K::List);
      ws();
      if (eat("]")) return l;
      for (;;) {
        l->kids.push_back(parse_or());
        if (eat(",")) continue;
        if (eat("]")) break;
        return fail("',' or ']' is expected in a list");
      }
      return l;
    }
    if (c == '{') {
      ++i;
      auto r = make(Expr::K::Rec);
      ws();
      if (eat("}")) return r;
      for (;;) {
        ws();
        std::string key;
        if (i < src.size() && (src[i] == '"' || src[i] == '\'')) { char q = src[i++]; key = parse_string_body(q); }
        else {
          size_t st = i;
          while (i < src.size() && is_ident(src[i])) ++i;
          key = src.substr(st, i - st);
        }
        if (key.empty()) return fail("a field name is expected in {…}");
        if (!eat(":")) return fail("':' is expected after '" + key + "'");
        r->keys.push_back(key);
        r->kids.push_back(parse_or());
        if (eat(",")) continue;
        if (eat("}")) break;
        return fail("',' or '}' is expected in {…}");
      }
      return r;
    }
    if (is_ident_start(c)) {
      size_t st = i;
      while (i < src.size() && is_ident(src[i])) ++i;
      std::string name = src.substr(st, i - st);
      if (name == "true") return make(Expr::K::Bool), [&] { auto b = make(Expr::K::Bool); b->n = 1; return b; }();
      if (name == "false") { auto b = make(Expr::K::Bool); b->n = 0; return b; }
      if (name == "null" || name == "none") return make(Expr::K::Null);
      ws();
      if (i < src.size() && src[i] == '(') {
        ++i;
        auto call = make(Expr::K::Call);
        call->s = name;
        ws();
        if (eat(")")) return call;
        for (;;) {
          call->kids.push_back(parse_or());
          if (eat(",")) continue;
          if (eat(")")) break;
          return fail("',' or ')' is expected in " + name + "(…)");
        }
        return call;
      }
      auto v = make(Expr::K::Var);
      v->s = name;
      return v;
    }
    // A colour literal. `#rrggbb`, `#rgb`, `#rrggbbaa` — the form the language
    // reference tells the mind to use for shadow and highlight. It reads as
    // the string it is written as; color_of() turns it into the pixel.
    if (c == '#') {
      size_t n = 0;
      while (i + 1 + n < src.size() && std::isxdigit((unsigned char)src[i + 1 + n])) ++n;
      if (n == 3 || n == 4 || n == 6 || n == 8) {
        auto v = make(Expr::K::Str);
        v->s = src.substr(i, n + 1);
        i += n + 1;
        return v;
      }
    }
    return fail("cannot read '" + src.substr(i, 12) + "'");
  }
};

ExprP parse_expr(const std::string& text, std::string& err) {
  ExprParser p(text);
  ExprP e = p.parse_all();
  err = p.err;
  return e;
}

// ---- lines ------------------------------------------------------------------

struct Line {
  int indent = 0;
  std::string text;   // trimmed, comment removed
  int number = 0;
};

// A '#' that begins a COLOUR is not a comment.
//
// The comment character and the hex-colour prefix are the same character, and
// the comment won. `paint circle 940 470 210 #2a1a4d` was truncated to
// `paint circle 940 470 210`, the colour fell back to the default ink, and the
// line still PARSED — so the look reported "every word understood" while every
// hex colour in the face silently became the same near-white. Anything after
// the colour went with it: a line's width, a gradient's direction, a text size.
//
// It is why drawings came out flat. The language reference tells the mind to
// "draw a shadow as black at low alpha, a highlight as white at low alpha" so
// that layered shapes read as one thing — and every one of those #rrggbbaa
// colours was being deleted before the parser ever saw it, leaving the machine
// with the sixteen opaque named colours and no shading at all.
//
// A '#' is a colour when exactly 3, 4, 6 or 8 hex digits follow it and the run
// ends there; `# body` and `#todo` are still comments.
bool hex_colour_at(const std::string& s, size_t hash) {
  size_t n = 0;
  while (hash + 1 + n < s.size() && std::isxdigit((unsigned char)s[hash + 1 + n])) ++n;
  if (n != 3 && n != 4 && n != 6 && n != 8) return false;
  const size_t after = hash + 1 + n;
  if (after < s.size()) {
    const char c = s[after];
    if (std::isalnum((unsigned char)c) || c == '_') return false;
  }
  return true;
}

// Comments start with '#' outside quotes — unless that '#' is a colour.
std::string strip_comment(const std::string& raw) {
  std::string out;
  char quote = 0;
  for (size_t i = 0; i < raw.size(); ++i) {
    char c = raw[i];
    if (quote) {
      out += c;
      if (c == '\\' && i + 1 < raw.size()) { out += raw[++i]; continue; }
      if (c == quote) quote = 0;
      continue;
    }
    if (c == '"' || c == '\'') { quote = c; out += c; continue; }
    if (c == '#' && !hex_colour_at(raw, i)) break;
    out += c;
  }
  return out;
}

std::vector<Line> split_lines(const std::string& src) {
  std::vector<Line> out;
  size_t at = 0;
  int number = 0;
  while (at <= src.size()) {
    size_t nl = src.find('\n', at);
    std::string raw = src.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
    at = nl == std::string::npos ? src.size() + 1 : nl + 1;
    ++number;
    if (!raw.empty() && raw.back() == '\r') raw.pop_back();
    int indent = 0;
    for (char c : raw) { if (c == ' ') indent++; else if (c == '\t') indent += 2; else break; }
    std::string text = trim(strip_comment(raw));
    if (text.empty()) continue;
    out.push_back(Line{indent, text, number});
  }
  return out;
}

// Split a line into tokens, keeping quoted strings, (…), [...] and {…} whole.
// Where the string that opens at `open` closes. A string may hold {expressions}
// that hold strings of their own — "{if(mode == "in", "received", "shipped")} {qty}"
// — so inside braces a quote belongs to the expression, not to the string.
size_t string_end(const std::string& s, size_t open) {
  const char quote = s[open];
  int depth = 0;
  char inner = 0;
  for (size_t i = open + 1; i < s.size(); ++i) {
    const char c = s[i];
    if (inner) { if (c == '\\') ++i; else if (c == inner) inner = 0; continue; }
    if (c == '\\') { ++i; continue; }
    if (c == '{') { depth++; continue; }
    if (c == '}') { if (depth > 0) depth--; continue; }
    if (depth > 0 && (c == '"' || c == '\'')) { inner = c; continue; }
    if (depth == 0 && c == quote) return i;
  }
  return std::string::npos;
}

std::vector<std::string> tokens_of(const std::string& text) {
  std::vector<std::string> out;
  std::string cur;
  int depth = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (c == '"' || c == '\'') {
      const size_t end = string_end(text, i);
      const size_t stop = end == std::string::npos ? text.size() - 1 : end;
      cur += text.substr(i, stop - i + 1);
      i = stop;
      continue;
    }
    if (c == '(' || c == '[' || c == '{') depth++;
    if (c == ')' || c == ']' || c == '}') depth--;
    if (is_space(c) && depth <= 0) { if (!cur.empty()) { out.push_back(cur); cur.clear(); } continue; }
    cur += c;
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::string unquote(const std::string& t) {
  if (t.size() >= 2 && (t[0] == '"' || t[0] == '\'') && string_end(t, 0) == t.size() - 1) {
    std::string s;
    int depth = 0;
    for (size_t i = 1; i + 1 < t.size(); ++i) {
      if (t[i] == '{') depth++;
      else if (t[i] == '}' && depth > 0) depth--;
      if (depth == 0 && t[i] == '\\' && i + 2 < t.size()) { char c = t[++i]; s += c == 'n' ? '\n' : c == 't' ? '\t' : c; continue; }
      s += t[i];
    }
    return s;
  }
  return t;
}
bool is_quoted(const std::string& t) { return t.size() >= 2 && (t[0] == '"' || t[0] == '\'') && string_end(t, 0) == t.size() - 1; }

// Statements inside a line: `a; b; c`, split outside quotes and brackets.
std::vector<std::string> split_semis(const std::string& text) {
  std::vector<std::string> out;
  std::string cur;
  int depth = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (c == '"' || c == '\'') {
      const size_t end = string_end(text, i);
      const size_t stop = end == std::string::npos ? text.size() - 1 : end;
      cur += text.substr(i, stop - i + 1);
      i = stop;
      continue;
    }
    if (c == '(' || c == '[' || c == '{') depth++;
    if (c == ')' || c == ']' || c == '}') depth--;
    if (c == ';' && depth <= 0) { std::string t = trim(cur); if (!t.empty()) out.push_back(t); cur.clear(); continue; }
    cur += c;
  }
  std::string t = trim(cur);
  if (!t.empty()) out.push_back(t);
  return out;
}

struct DocParser {
  std::vector<Line> lines;
  size_t i = 0;
  Face& face;
  explicit DocParser(Face& f) : face(f) {}

  void error(int line, const std::string& m) { face.errors.push_back("line " + std::to_string(line) + ": " + m); }

  ExprP expr(const std::string& text, int line) {
    std::string err;
    ExprP e = parse_expr(text, err);
    if (!err.empty()) error(line, "in '" + text + "': " + err);
    return e;
  }

  // One statement from its text; `if`/`for`/`else` with a trailing ':' take a
  // body: the rest of the line after ':' (a; b) and/or the indented lines below.
  StmtP statement(const std::string& text, int line, int indent) {
    auto st = std::make_shared<Stmt>();
    st->line = line;
    std::vector<std::string> tk = tokens_of(text);
    if (tk.empty()) return nullptr;
    // `else:` is the word else with its colon attached; so is `if x:` when x is short.
    std::string verb = tk[0];
    while (!verb.empty() && verb.back() == ':') verb.pop_back();
    auto rest_after = [&](size_t n) {
      // the text after the first n tokens
      size_t pos = 0;
      for (size_t k = 0; k < n && k < tk.size(); ++k) {
        pos = text.find(tk[k], pos);
        if (pos == std::string::npos) return std::string();
        pos += tk[k].size();
      }
      return trim(text.substr(pos));
    };
    if (verb == "set" || verb == "let") {
      st->k = Stmt::K::Set;
      // set name = expr  |  set name expr  |  set name += expr
      if (tk.size() < 3) { error(line, "set <name> = <expr>"); return nullptr; }
      st->name = tk[1];
      std::string rhs = rest_after(2);
      if (!rhs.empty() && rhs.rfind("+=", 0) == 0) rhs = st->name + " + (" + trim(rhs.substr(2)) + ")";
      else if (!rhs.empty() && rhs.rfind("-=", 0) == 0) rhs = st->name + " - (" + trim(rhs.substr(2)) + ")";
      else if (!rhs.empty() && rhs.rfind("*=", 0) == 0) rhs = st->name + " * (" + trim(rhs.substr(2)) + ")";
      else if (!rhs.empty() && rhs.rfind("/=", 0) == 0) rhs = st->name + " / (" + trim(rhs.substr(2)) + ")";
      else if (!rhs.empty() && rhs[0] == '=') rhs = trim(rhs.substr(1));
      st->expr = expr(rhs, line);
      return st;
    }
    if (verb == "toggle") {
      st->k = Stmt::K::Set;
      if (tk.size() < 2) { error(line, "toggle <name>"); return nullptr; }
      st->name = tk[1];
      st->expr = expr("not " + tk[1], line);
      return st;
    }
    if (verb == "inc" || verb == "increment") {
      st->k = Stmt::K::Set;
      if (tk.size() < 2) { error(line, "inc <name> [by]"); return nullptr; }
      st->name = tk[1];
      std::string by = tk.size() > 2 ? rest_after(2) : "1";
      if (by.rfind("by ", 0) == 0) by = trim(by.substr(3));
      else if (by.rfind("by", 0) == 0 && (by.size() == 2 || is_space(by[2]))) by = trim(by.substr(2));
      if (by.empty()) by = "1";
      st->expr = expr(tk[1] + " + (" + by + ")", line);
      return st;
    }
    if (verb == "dec" || verb == "decrement") {
      st->k = Stmt::K::Set;
      if (tk.size() < 2) { error(line, "dec <name> [by]"); return nullptr; }
      st->name = tk[1];
      std::string by = tk.size() > 2 ? rest_after(2) : "1";
      if (by.rfind("by ", 0) == 0) by = trim(by.substr(3));
      else if (by.rfind("by", 0) == 0 && (by.size() == 2 || is_space(by[2]))) by = trim(by.substr(2));
      if (by.empty()) by = "1";
      st->expr = expr(tk[1] + " - (" + by + ")", line);
      return st;
    }
    if (verb == "put" || verb == "keep") {
      st->k = Stmt::K::Put;
      if (tk.size() < 3) { error(line, "put <kind> {field: value, …}"); return nullptr; }
      st->name = tk[1];
      st->expr = expr(rest_after(2), line);
      return st;
    }
    if (verb == "delete" || verb == "drop" || verb == "remove") {
      st->k = Stmt::K::Delete;
      if (tk.size() < 3) { error(line, "delete <kind> <id>"); return nullptr; }
      st->name = tk[1];
      st->expr = expr(rest_after(2), line);
      return st;
    }
    if (verb == "update") {
      st->k = Stmt::K::Update;
      if (tk.size() < 3) { error(line, "update <kind> <id> {field: value, …}"); return nullptr; }
      st->name = tk[1];
      std::string rest = rest_after(2);
      size_t brace = rest.find('{');
      if (brace != std::string::npos) {
        std::string id_part = trim(rest.substr(0, brace));
        std::string rec_part = trim(rest.substr(brace));
        st->args.push_back(expr(id_part, line));
        st->expr = expr(rec_part, line);
      } else {
        st->expr = expr(rest, line);
      }
      return st;
    }
    if (verb == "undo" || verb == "revert") {
      st->k = Stmt::K::Undo;
      return st;
    }
    if (verb == "redo") {
      st->k = Stmt::K::Redo;
      return st;
    }
    if (verb == "snapshot") {
      st->k = Stmt::K::Snapshot;
      if (tk.size() > 1) st->expr = expr(rest_after(1), line);
      return st;
    }
    if (verb == "emit") {
      st->k = Stmt::K::Emit;
      if (tk.size() < 2) { error(line, "emit <expr> to <channel>"); return nullptr; }
      std::string rest = rest_after(1);
      size_t to_pos = rest.rfind(" to ");
      if (to_pos != std::string::npos) {
        std::string val_part = trim(rest.substr(0, to_pos));
        std::string chan_part = trim(rest.substr(to_pos + 4));
        st->expr = expr(val_part, line);
        st->target = chan_part;
      } else {
        std::vector<std::string> parts = tokens_of(rest);
        if (parts.size() >= 2) {
          st->target = parts.back();
          size_t p = rest.rfind(st->target);
          st->expr = expr(trim(rest.substr(0, p)), line);
        } else {
          st->expr = expr(rest, line);
          st->target = "default";
        }
      }
      return st;
    }
    if (verb == "spawn") {
      st->k = Stmt::K::Spawn;
      size_t colon = text.find(':');
      std::string head = colon == std::string::npos ? text : trim(text.substr(0, colon));
      std::string inline_body = colon == std::string::npos ? "" : trim(text.substr(colon + 1));
      std::vector<std::string> parts = tokens_of(head);
      if (parts.size() >= 3 && parts[1] == "task") st->name = parts[2];
      else if (parts.size() >= 2) st->name = (parts[1] == "task" ? "task" : parts[1]);
      else st->name = "task";
      for (const auto& part : split_semis(inline_body)) { StmtP s2 = statement(part, line, indent); if (s2) st->body.push_back(s2); }
      auto more = block(indent);
      st->body.insert(st->body.end(), more.begin(), more.end());
      return st;
    }
    if (verb == "clear") { st->k = Stmt::K::Clear; st->name = tk.size() > 1 ? tk[1] : ""; return st; }
    if (verb == "focus") { st->k = Stmt::K::Focus; st->name = tk.size() > 1 ? tk[1] : ""; return st; }
    if (verb == "say" || verb == "toast" || verb == "notify" || verb == "log") {
      st->k = Stmt::K::Say;
      st->expr = expr(rest_after(1), line);
      return st;
    }
    if (verb == "stop") { st->k = Stmt::K::Stop; return st; }
    if (verb == "paint" || verb == "draw") {
      st->k = Stmt::K::Paint;
      if (tk.size() < 2) { error(line, "paint clear|rect|outline|circle|line|text …"); return nullptr; }
      st->name = tk[1];
      // Arguments are expressions read one after another from the text, so
      // `paint rect laneW * lane + 10 y - 5 30 50 red` is four numbers and a
      // colour, not eleven words. Commas may separate them too.
      {
        const std::string rest = rest_after(2);
        ExprParser P(rest);
        for (;;) {
          P.ws();
          if (P.i >= rest.size()) break;
          if (P.eat(",")) continue;
          const size_t before = P.i;
          size_t eq_pos = rest.find('=', P.i);
          if (eq_pos != std::string::npos && (eq_pos + 1 >= rest.size() || rest[eq_pos + 1] != '=')) {
            size_t id_len = 0;
            while (P.i + id_len < eq_pos && is_ident(rest[P.i + id_len])) ++id_len;
            if (P.i + id_len == eq_pos && id_len > 0) {
              std::string key = rest.substr(P.i, id_len);
              if (key == "color" || key == "action" || key == "click" || key == "id" || key == "hit" || key == "hit_id") {
                P.i = eq_pos + 1;
                P.ws();
                std::string val;
                if (P.i < rest.size() && (rest[P.i] == '"' || rest[P.i] == '\'')) {
                  char q = rest[P.i++];
                  val = P.parse_string_body(q);
                } else {
                  size_t v_start = P.i;
                  while (P.i < rest.size() && !is_space(rest[P.i]) && rest[P.i] != ',') ++P.i;
                  val = rest.substr(v_start, P.i - v_start);
                }
                auto s_expr = P.make(Expr::K::Str);
                s_expr->s = key + "=" + val;
                st->args.push_back(s_expr);
                continue;
              }
            }
          }
          ExprP e = P.parse_or();
          if (!P.err.empty()) { error(line, "in '" + rest + "': " + P.err); break; }
          if (P.i == before) { error(line, "in '" + rest + "': cannot read '" + rest.substr(before, 12) + "'"); break; }
          st->args.push_back(e);
        }
      }
      return st;
    }
    if (verb == "call" || verb == "do") {
      st->k = Stmt::K::Call;
      st->expr = expr(rest_after(1), line);
      return st;
    }
    if (verb == "if" || verb == "for" || verb == "else" || verb == "elif") {
      // header up to the ':' that ends it (outside quotes/brackets)
      size_t colon = std::string::npos;
      {
        int depth = 0;
        for (size_t p = 0; p < text.size(); ++p) {
          char c = text[p];
          if (c == '"' || c == '\'') { const size_t e = string_end(text, p); if (e == std::string::npos) break; p = e; continue; }
          if (c == '(' || c == '[' || c == '{') depth++;
          if (c == ')' || c == ']' || c == '}') depth--;
          if (c == ':' && depth <= 0) { colon = p; break; }
        }
      }
      if (colon == std::string::npos) { error(line, "'" + verb + "' needs a ':' after its condition"); return nullptr; }
      std::string head = trim(text.substr(0, colon));
      std::string inline_body = trim(text.substr(colon + 1));
      std::vector<StmtP> body;
      for (const auto& part : split_semis(inline_body)) { StmtP s2 = statement(part, line, indent); if (s2) body.push_back(s2); }
      // the indented lines below, each whole if it opens a block, else split on ';'
      {
        auto more = block(indent);
        body.insert(body.end(), more.begin(), more.end());
      }
      if (verb == "if" || verb == "elif") {
        st->k = Stmt::K::If;
        st->expr = expr(trim(head.substr(verb.size())), line);
        st->body = body;
        // else / elif at the same indent
        if (i < lines.size() && lines[i].indent == indent && (lines[i].text.rfind("else", 0) == 0 || lines[i].text.rfind("elif", 0) == 0)) {
          const Line& l = lines[i];
          ++i;
          StmtP e = statement(l.text, l.number, l.indent);
          if (e) {
            if (l.text.rfind("elif", 0) == 0) st->otherwise = {e};
            else st->otherwise = e->body;
          }
        }
        return st;
      }
      if (verb == "else") {
        // returned to the caller as a carrier: its body is the else body
        st->k = Stmt::K::If;
        st->body = body;
        return st;
      }
      // for <name> in <expr>:
      st->k = Stmt::K::For;
      std::vector<std::string> ht = tokens_of(head);
      if (ht.size() < 4 || ht[2] != "in") { error(line, "for <name> in <list>:"); return nullptr; }
      st->name = ht[1];
      size_t in_at = head.find(" in ");
      st->expr = expr(trim(head.substr(in_at + 4)), line);
      // ITS BODY. `if` assigned this and `for` did not, so every loop in every
      // face ever written ran its list and executed nothing — a row of windows,
      // a field of flowers, the dashes down the middle of a road, all silently
      // absent, with no error to say so because the loop itself was perfectly
      // well understood.
      st->body = body;
      return st;
    }
    // A bare expression statement (a call): `organ("x", "y")`
    st->k = Stmt::K::Call;
    st->expr = expr(text, line);
    return st;
  }

  std::vector<StmtP> block(int indent) {
    std::vector<StmtP> out;
    while (i < lines.size() && lines[i].indent > indent) {
      const Line& l = lines[i];
      ++i;
      std::vector<std::string> tk = tokens_of(l.text);
      std::string head = tk.empty() ? std::string() : tk[0];
      while (!head.empty() && head.back() == ':') head.pop_back();
      if (head == "if" || head == "elif" || head == "else" || head == "for") {
        // The whole line: what follows its ':' is ITS body (`if x: a; b; stop`),
        // not statements of the enclosing block. Splitting first put a `stop`
        // meant for the empty-field case at the handler's top level, and every
        // scan stopped there.
        StmtP s = statement(l.text, l.number, l.indent);
        if (s) out.push_back(s);
        continue;
      }
      for (const auto& part : split_semis(l.text)) {
        StmtP s = statement(part, l.number, l.indent);
        if (s) out.push_back(s);
      }
    }
    return out;
  }

  std::shared_ptr<Widget> widget(const Line& l) {
    auto w = std::make_shared<Widget>();
    w->line = l.number;
    std::string text_clean = l.text;
    while (!text_clean.empty() && text_clean.back() == ':') text_clean.pop_back();
    std::vector<std::string> tk = tokens_of(text_clean);
    if (tk.empty()) return w;
    w->type = tk[0];
    while (!w->type.empty() && w->type.back() == ':') w->type.pop_back();
    for (size_t k = 1; k < tk.size(); ++k) {
      std::string t = tk[k];
      while (!t.empty() && t.back() == ':' && !is_quoted(t)) t.pop_back();
      size_t eq = t.find('=');
      // key=value, unless the '=' is inside quotes/brackets (then it is positional)
      bool kv = eq != std::string::npos && eq > 0 && !is_quoted(t) && t[0] != '{' && t[0] != '(';
      if (kv) {
        std::string key = t.substr(0, eq), val = t.substr(eq + 1);
        while (!val.empty() && val.back() == ':' && !is_quoted(val)) val.pop_back();
        // An unquoted value written with spaces — `if=not empty(sku)`,
        // `value=a + b` — arrives as several tokens. While the value so far
        // is an unfinished expression and the next token is not another
        // attribute, a quoted label or a flag word, it is part of this value.
        auto unfinished = [&](const std::string& v) {
          if (v.empty()) return true;
          std::string err; (void)parse_expr(v, err);
          return err.find("expression is expected") != std::string::npos || err.find("is expected") != std::string::npos;
        };
        auto looks_like_attr = [&](const std::string& n) {
          size_t e2 = n.find('=');
          return e2 != std::string::npos && e2 > 0 && !is_quoted(n) && n[0] != '{' && n[0] != '(' && n[0] != '"';
        };
        while (!is_quoted(val) && unfinished(val) && k + 1 < tk.size() && !looks_like_attr(tk[k + 1]) && !is_quoted(tk[k + 1])) {
          val += " " + tk[k + 1];
          ++k;
        }
        while (!val.empty() && val.back() == ':' && !is_quoted(val)) val.pop_back();
        if (is_quoted(val)) {
          w->attrs.emplace_back(key, unquote(val));
          // a quoted value may interpolate
          if (val.find('{') != std::string::npos) w->exprs.emplace_back(key, expr(val, l.number));
        } else {
          w->attrs.emplace_back(key, val);
          // a bare value that is not a plain word/number is an expression
          bool is_literal_attr = (key == "action" || key == "enter" || key == "id" || key == "name" || key == "path" ||
                                  key == "target" || key == "placeholder" || key == "hit" || key == "hit_id" ||
                                  key == "options" || key == "src" || key == "background" || key == "color" || key == "accent");
          if (!is_literal_attr) {
            bool plain = true;
            for (char c : val) if (!(is_ident(c) || c == '-' || c == '.' || c == '%')) { plain = false; break; }
            bool numeric = !val.empty() && (isdigit((unsigned char)val[0]) || val[0] == '-' || val[0] == '.');
            if (!plain || (!numeric && (val.find('(') != std::string::npos || val.find('.') != std::string::npos)))
              w->exprs.emplace_back(key, expr(val, l.number));
            else if (!numeric && val != "true" && val != "false" && val != "left" && val != "right" && val != "center" &&
                     val != "ok" && val != "warn" && val != "bad" && val != "accent" && val != "dim" && val != "small" && val != "big")
              w->exprs.emplace_back(key, expr(val, l.number));   // a variable
          }
        }
      } else if (is_quoted(t)) {
        if (w->label.empty()) { w->label = unquote(t); if (t.find('{') != std::string::npos) w->exprs.emplace_back("label", expr(t, l.number)); }
        else w->words.push_back(unquote(t));
      } else {
        w->words.push_back(t);
      }
    }
    return w;
  }

  static bool header_word(const std::string& h) {
    return h == "face" || h == "title" || h == "let" || h == "state" || h == "on" || h == "kind" ||
           h == "component" || h == "type" || h == "import";
  }

  void widgets_into(std::vector<std::shared_ptr<Widget>>& out, int indent) {
    while (i < lines.size() && lines[i].indent > indent) {
      const Line& l = lines[i];
      // At the top level a header word or a rule ends the widgets: `on scan:`
      // written after the tree is a handler, not a widget called "on".
      if (indent < 0 && l.indent == 0) {
        std::vector<std::string> tk = tokens_of(l.text);
        if (!tk.empty() && header_word(tk[0])) break;
      }
      ++i;
      auto w = widget(l);
      widgets_into(w->children, l.indent);
      out.push_back(w);
    }
  }

  void run(const std::string& source) {
    lines = split_lines(source);
    while (i < lines.size()) {
      const Line& l = lines[i];
      ++i;
      std::vector<std::string> tk = tokens_of(l.text);
      if (tk.empty()) continue;
      const std::string& head = tk[0];
      if (l.indent == 0 && head == "face") { face.name = tk.size() > 1 ? unquote(tk[1]) : ""; continue; }
      if (l.indent == 0 && head == "title") { face.title = tk.size() > 1 ? unquote(tk[1]) : ""; continue; }
      if (l.indent == 0 && head == "import") {
        if (tk.size() < 2) { error(l.number, "import \"<path>\""); continue; }
        std::string path = unquote(tk[1]);
        face.imports.push_back(path);
        std::ifstream f(path);
        if (f.good()) {
          std::stringstream buf;
          buf << f.rdbuf();
          Face sub = parse_face(buf.str());
          for (const auto& c : sub.components) face.components.push_back(c);
          for (const auto& t : sub.types) face.types.push_back(t);
        }
        continue;
      }
      if (l.indent == 0 && head == "component") {
        std::string line_text = l.text;
        size_t lparen = line_text.find('(');
        size_t rparen = line_text.find(')', lparen == std::string::npos ? 0 : lparen);
        size_t colon = line_text.find(':', rparen == std::string::npos ? 0 : rparen);
        if (lparen == std::string::npos || rparen == std::string::npos || colon == std::string::npos) {
          error(l.number, "component Name(params...):");
          continue;
        }
        std::string name = trim(line_text.substr(9, lparen - 9));
        std::string param_str = trim(line_text.substr(lparen + 1, rparen - lparen - 1));
        ComponentDef comp;
        comp.name = name;
        comp.line = l.number;
        if (!param_str.empty()) {
          std::vector<std::string> param_tokens;
          std::string cur;
          int depth = 0;
          for (char c : param_str) {
            if (c == '"' || c == '\'') depth ^= 1;
            if (c == ',' && depth == 0) { param_tokens.push_back(trim(cur)); cur.clear(); }
            else cur += c;
          }
          if (!trim(cur).empty()) param_tokens.push_back(trim(cur));
          for (const auto& pt : param_tokens) {
            size_t eq = pt.find('=');
            if (eq != std::string::npos) {
              std::string pname = trim(pt.substr(0, eq));
              std::string pdef = trim(pt.substr(eq + 1));
              comp.params.emplace_back(pname, expr(pdef, l.number));
            } else {
              comp.params.emplace_back(trim(pt), nullptr);
            }
          }
        }
        widgets_into(comp.body, l.indent);
        face.components.push_back(std::move(comp));
        continue;
      }
      if (l.indent == 0 && head == "type") {
        std::string line_text = l.text;
        std::string name = tk.size() > 1 ? tk[1] : "";
        while (!name.empty() && (name.back() == '{' || name.back() == ':')) name.pop_back();
        if (name.empty()) { error(l.number, "type <Name> { ... }"); continue; }
        TypeDef tdef;
        tdef.name = name;
        tdef.line = l.number;
        size_t lbrace = line_text.find('{');
        size_t rbrace = line_text.rfind('}');
        std::vector<std::string> flines;
        if (lbrace != std::string::npos && rbrace != std::string::npos && rbrace > lbrace) {
          std::string inner = line_text.substr(lbrace + 1, rbrace - lbrace - 1);
          std::string cur;
          int depth = 0;
          for (char c : inner) {
            if (c == '{' || c == '(' || c == '[') depth++;
            if (c == '}' || c == ')' || c == ']') depth--;
            if ((c == ',' || c == '\n' || c == ';') && depth == 0) {
              if (!trim(cur).empty()) flines.push_back(trim(cur));
              cur.clear();
            } else {
              cur += c;
            }
          }
          if (!trim(cur).empty()) flines.push_back(trim(cur));
        } else {
          while (i < lines.size()) {
            const Line& fl = lines[i];
            std::string tfl = trim(fl.text);
            if (tfl == "}") { ++i; break; }
            if (fl.indent == 0 && !tfl.empty() && header_word(tokens_of(tfl)[0])) break;
            ++i;
            if (!tfl.empty() && tfl != "{") flines.push_back(tfl);
          }
        }
        for (const auto& fl : flines) {
          size_t cpos = fl.find(':');
          if (cpos != std::string::npos) {
            TypeField tf;
            tf.name = trim(fl.substr(0, cpos));
            std::string type_part = trim(fl.substr(cpos + 1));
            while (!type_part.empty() && (type_part.back() == ',' || type_part.back() == ';')) type_part.pop_back();
            type_part = trim(type_part);
            if (type_part.rfind("enum", 0) == 0 || type_part.find("enum") != std::string::npos ||
                type_part.rfind("Enum", 0) == 0 || type_part.find("Enum") != std::string::npos) {
              tf.type_name = "Enum";
              size_t eb = type_part.find_first_of("({");
              size_t ee = type_part.find_last_of(")}");
              if (eb != std::string::npos && ee != std::string::npos && ee > eb) {
                std::string evals = type_part.substr(eb + 1, ee - eb - 1);
                for (const auto& evk : tokens_of(evals)) {
                  std::string clean = evk;
                  while (!clean.empty() && (clean.back() == ',' || clean.back() == ';')) clean.pop_back();
                  clean = unquote(clean);
                  if (!clean.empty()) tf.enum_values.push_back(clean);
                }
              } else {
                for (const auto& evk : tokens_of(type_part)) {
                  if (evk != "enum" && evk != "Enum" && evk != "{" && evk != "}" && evk != "(" && evk != ")") {
                    std::string clean = evk;
                    while (!clean.empty() && (clean.back() == ',' || clean.back() == ';')) clean.pop_back();
                    clean = unquote(clean);
                    if (!clean.empty()) tf.enum_values.push_back(clean);
                  }
                }
              }
            } else {
              tf.type_name = type_part;
            }
            tdef.fields.push_back(tf);
          }
        }
        face.types.push_back(std::move(tdef));
        continue;
      }
      if (l.indent == 0 && head == "kind") {
        // kind item key=sku — records of this kind have identity: list(item) is
        // the newest record per sku, and a later put replaces the row.
        std::string kind = tk.size() > 1 ? tk[1] : "", key;
        for (size_t t = 2; t < tk.size(); ++t) if (tk[t].rfind("key=", 0) == 0) key = unquote(tk[t].substr(4));
        if (kind.empty() || key.empty()) { error(l.number, "kind <name> key=<field>"); continue; }
        face.kinds.emplace_back(kind, key);
        continue;
      }
      if (l.indent == 0 && (head == "let" || head == "state")) {
        if (tk.size() < 3) { error(l.number, head + " <name> = <expr>"); continue; }
        size_t eq = l.text.find('=');
        std::string rhs = eq == std::string::npos ? "" : trim(l.text.substr(eq + 1));
        ExprP e = expr(rhs, l.number);
        if (head == "let") face.lets.emplace_back(tk[1], e);
        else face.states.emplace_back(tk[1], e);
        continue;
      }
      if (l.indent == 0 && head == "on") {
        // on load: | on frame: | on key ArrowLeft: | on scan:
        std::string rest = trim(l.text.substr(2));
        size_t colon = rest.find(':');
        std::string spec = colon == std::string::npos ? rest : trim(rest.substr(0, colon));
        std::string inline_body = colon == std::string::npos ? "" : trim(rest.substr(colon + 1));
        Handler h;
        std::vector<std::string> st = tokens_of(spec);
        if (st.empty()) { error(l.number, "on <event>:"); continue; }
        if (st[0] == "key" && st.size() > 1) { h.event = "key"; h.key = st[1]; }
        else h.event = st[0];
        for (const auto& part : split_semis(inline_body)) { StmtP s = statement(part, l.number, l.indent); if (s) h.body.push_back(s); }
        auto more = block(l.indent);
        h.body.insert(h.body.end(), more.begin(), more.end());
        face.handlers.push_back(h);
        continue;
      }
      // a widget at any indent (a top-level widget is indent 0)
      --i;
      int base = l.indent - 1;
      widgets_into(face.widgets, base);
    }
  }
};

}  // namespace

std::string Widget::attr(const std::string& k, const std::string& def) const {
  for (const auto& kv : attrs) if (kv.first == k) return kv.second;
  return def;
}
bool Widget::flag(const std::string& k) const {
  for (const auto& w : words) if (w == k) return true;
  const std::string v = attr(k, "\x01");
  return v != "\x01" && v != "false" && v != "0" && v != "no";
}
int Widget::attr_int(const std::string& k, int def) const {
  const std::string v = attr(k, "");
  if (v.empty()) return def;
  char* end = nullptr;
  long n = std::strtol(v.c_str(), &end, 10);
  return end == v.c_str() ? def : (int)n;
}

Face parse_face(const std::string& source) {
  Face f;
  f.source = source;
  DocParser p(f);
  p.run(source);
  static const std::set<std::string> known = {"row", "column", "col", "grid", "panel", "text", "label", "heading", "tile", "table",
                                              "list", "field", "input", "button", "gauge", "spacer", "paint", "divider", "rule", "separator", "editor", "image", "img", "scroll",
                                              "badge", "status", "sparkline", "chart", "modal", "dialog", "overlay", "scrubber", "timeline",
                                              "checkbox", "check", "switch", "toggle", "tabs", "segmented", "progress", "bar", "select", "dropdown",
                                              "slider", "rating", "stars", "accordion", "collapsible", "details", "tree", "treeview", "tag", "tags", "chips", "toolbar"};
  std::set<std::string> known_comps;
  for (const auto& c : f.components) known_comps.insert(c.name);
  std::vector<const Widget*> stack;
  for (auto& w : f.widgets) stack.push_back(w.get());
  for (const auto& comp : f.components) {
    for (auto& w : comp.body) stack.push_back(w.get());
  }
  while (!stack.empty()) {
    const Widget* w = stack.back();
    stack.pop_back();
    if (!known.count(w->type) && !known_comps.count(w->type)) f.errors.push_back("line " + std::to_string(w->line) + ": no widget is called '" + w->type + "' (row column grid panel text heading tile table list field editor button gauge spacer paint divider badge sparkline modal scrubber checkbox switch tabs progress select)");
    for (auto& c : w->children) stack.push_back(c.get());
  }
  return f;
}

// ----------------------------------------------------------------- runtime --

namespace {

constexpr uint32_t kBg = 0xFF0F1318;
constexpr uint32_t kPanel = 0xFF161C24;
constexpr uint32_t kPanelEdge = 0xFF26313E;
constexpr uint32_t kInk = 0xFFE6ECF1;
constexpr uint32_t kDim = 0xFF8C9AA6;
constexpr uint32_t kAccent = 0xFF5FB3FF;
constexpr uint32_t kOk = 0xFF3ECF8E;
constexpr uint32_t kWarn = 0xFFF0A23A;
constexpr uint32_t kBad = 0xFFF0665F;
constexpr uint32_t kField = 0xFF0B0F14;
constexpr uint32_t kFieldEdge = 0xFF33414F;
constexpr uint32_t kZebra = 0xFF1B222B;

uint32_t color_of(const std::string& raw, uint32_t def) {
  std::string s;
  for (char c : raw) s += (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
  if (s.empty()) return def;
  if (s[0] == '#') {
    auto nib = [](char c) -> uint32_t {
      if (c >= '0' && c <= '9') return (uint32_t)(c - '0');
      if (c >= 'a' && c <= 'f') return (uint32_t)(c - 'a' + 10);
      return 0;
    };
    if (s.size() == 4) { uint32_t r = nib(s[1]), g = nib(s[2]), b = nib(s[3]); return 0xFF000000u | (r * 17u << 16) | (g * 17u << 8) | (b * 17u); }
    uint32_t rgb = 0;
    for (size_t i = 1; i < s.size() && i < 7; ++i) rgb = (rgb << 4) | nib(s[i]);
    if (s.size() == 9) { uint32_t a = (nib(s[7]) << 4) | nib(s[8]); return (a << 24) | rgb; }
    return 0xFF000000u | rgb;
  }
  struct N { const char* n; uint32_t c; };
  static const N named[] = {
    {"white", 0xFFFFFFFF}, {"black", 0xFF000000}, {"red", kBad}, {"green", kOk}, {"blue", kAccent}, {"yellow", 0xFFFDD835},
    {"orange", kWarn}, {"purple", 0xFF9C6ADE}, {"cyan", 0xFF35C4D6}, {"magenta", 0xFFD65B9E}, {"gray", kDim}, {"grey", kDim},
    {"silver", 0xFFBDC6CE}, {"navy", 0xFF1A2A4E}, {"teal", 0xFF2AA198}, {"lime", 0xFFB5D33D}, {"pink", 0xFFF06292},
    {"brown", 0xFF8D6E63}, {"ink", kInk}, {"dim", kDim}, {"accent", kAccent}, {"ok", kOk}, {"warn", kWarn}, {"bad", kBad},
    {"panel", kPanel}, {"bg", kBg}, {"edge", kPanelEdge}, {"transparent", 0x00000000},
  };
  for (const auto& e : named) if (s == e.n) return e.c;
  return def;
}

struct DrawOp {
  enum class K : uint8_t { Rect, Outline, Round, RoundOutline, Text, Line, Circle, BitmapImage } k = K::Rect;
  int x = 0, y = 0, w = 0, h = 0, r = 0;
  uint32_t color = 0;
  std::string text;
  web::FontKey font;
  std::shared_ptr<web::Bitmap> bitmap;
};

web::FontKey font_of(int px, bool bold = false, bool mono = false) {
  web::FontKey f;
  f.px = px;
  f.bold = bold;
  f.mono = mono;
  return f;
}

int text_w(const std::string& s, const web::FontKey& f) { return web_text_width(s, f); }
int line_h(const web::FontKey& f) { return web_line_height(f) + 2; }

// Cut a string to fit a width, with an ellipsis. Reports whether it was cut.
std::string fit(const std::string& s, const web::FontKey& f, int width, bool& cut) {
  cut = false;
  if (width <= 0) { cut = !s.empty(); return ""; }
  if (text_w(s, f) <= width) return s;
  const std::string ell = "…";
  const int ell_w = text_w(ell, f);
  // Forward, accumulating, stopping the moment it no longer fits. This used to
  // drop the LAST character and measure the whole string again, over and over:
  // for a record's text three thousand characters long clipped into a row, that
  // is three thousand walks of three thousand glyphs — nine million of them,
  // for one row, on every frame. A face listing such records spent its entire
  // frame here, which is why a page that was not moving ran at seven a second.
  std::string c;
  c.reserve(s.size());
  int w = 0;
  size_t i = 0;
  while (i < s.size()) {
    size_t j = i + 1;
    while (j < s.size() && ((unsigned char)s[j] & 0xC0) == 0x80) ++j;
    const std::string ch = s.substr(i, j - i);
    const int cw = text_w(ch, f);
    if (w + cw + ell_w > width) break;
    c += ch;
    w += cw;
    i = j;
  }
  cut = true;
  return c.empty() ? ell : c + ell;
}

}  // namespace

struct EditorState {
  std::string id;
  std::vector<std::string> lines;
  int cursor_line = 0;
  int cursor_col = 0;
  int scroll_line = 0;
  int scroll_col = 0;
  std::string path;
  bool dirty = false;
  bool initialized = false;
  int box_x = 0, box_y = 0, box_w = 0, box_h = 0;
  int gutter_w = 0;
  int char_w = 9;
  int line_h = 20;
  int last_view_lines = 10;
  int last_view_cols = 40;
};

struct Channel {
  std::string name;
  mutable aios::hal::Mutex mtx;
  std::deque<Value> queue;
  size_t capacity = 10000;

  void emit(Value v) {
    std::lock_guard<aios::hal::Mutex> lock(mtx);
    if (queue.size() >= capacity) queue.pop_front();
    queue.push_back(std::move(v));
  }
  Value recv() {
    std::lock_guard<aios::hal::Mutex> lock(mtx);
    if (queue.empty()) return Value::null();
    Value v = queue.front();
    queue.pop_front();
    return v;
  }
  Value poll() const {
    std::lock_guard<aios::hal::Mutex> lock(mtx);
    Value l = Value::make_list();
    for (const auto& item : queue) {
      l.list->push_back(item);
    }
    return l;
  }
  Value drain() {
    std::lock_guard<aios::hal::Mutex> lock(mtx);
    Value l = Value::make_list();
    while (!queue.empty()) {
      l.list->push_back(queue.front());
      queue.pop_front();
    }
    return l;
  }
  size_t size() const {
    std::lock_guard<aios::hal::Mutex> lock(mtx);
    return queue.size();
  }
  bool has_data() const {
    std::lock_guard<aios::hal::Mutex> lock(mtx);
    return !queue.empty();
  }
};

struct StateSnapshot {
  std::string label;
  double timestamp = 0;
  std::map<std::string, Value> state;
  std::map<std::string, std::string> fields;
};

struct Runtime::Impl {
  std::map<std::string, Value> state;    // persistent
  std::map<std::string, Value> lets;     // reactive
  std::map<std::string, Value> given;    // the embedder's, by name (Runtime::provide)
  std::map<std::string, std::string> fields;
  std::map<std::string, EditorState> editors;
  std::map<std::string, Value> scope;    // loop variables and record fields while evaluating a template
  std::set<std::string> keys_held;
  std::map<std::string, Value> list_cache;   // list(kind) per frame
  std::map<std::string, std::pair<uint64_t, Value>> list_memo;
  uint64_t memo_revision = 0;
  std::map<std::string, size_t> reads;
  std::vector<std::array<double, 6>> xform;
  std::vector<DrawOp> ops;
  std::vector<std::string> fault_seen;
  double now_ms = 0;
  double dt_ms = 16;
  int paint_x = 0, paint_y = 0, paint_w = 0, paint_h = 0;
  bool has_paint = false;
  int frame_no = 0;
  std::vector<std::string> field_order;   // for Tab
  std::vector<std::string> editor_order;
  std::map<std::string, std::string> kind_keys;   // kind -> identity field, from `kind … key=`
  std::map<std::string, std::shared_ptr<web::Bitmap>> image_cache;
  std::map<std::string, std::string> fetch_cache;
  size_t viewport_row_cap = 0;

  // Channels
  std::map<std::string, std::shared_ptr<Channel>> channels;
  std::shared_ptr<Channel> get_channel(const std::string& name) {
    auto it = channels.find(name);
    if (it != channels.end()) return it->second;
    auto ch = std::make_shared<Channel>();
    ch->name = name;
    channels[name] = ch;
    return ch;
  }

  // Pointer & Gesture tracking
  int mouse_x = 0, mouse_y = 0;
  int mouse_dx = 0, mouse_dy = 0;
  uint32_t mouse_buttons = 0;
  std::string hovered_widget;
  std::string active_widget;

  // History (Undo / Redo)
  std::vector<StateSnapshot> undo_stack;
  std::vector<StateSnapshot> redo_stack;
  size_t max_history = 100;

  // Types & Schemas
  std::vector<TypeDef> types;
};

Runtime::Runtime(Face f) : impl_(std::make_shared<Impl>()), face_(std::move(f)) {
  for (const auto& kk : face_.kinds) impl_->kind_keys[kk.first] = kk.second;
  impl_->types = face_.types;
}

void Runtime::bind(HostBindings h) { host_ = std::move(h); }

namespace {

struct Ev {
  Runtime::Impl& I;
  const HostBindings& host;
  std::vector<std::string>& faults;
  std::string& message;
  std::string& focus;
  std::map<std::string, std::string>& fields;
  bool& dirty;
  bool stop = false;

  void fault(const std::string& m) {
    for (const auto& f : I.fault_seen) if (f == m) return;
    I.fault_seen.push_back(m);
    faults.push_back(m);
  }

  // A kind or a record field is written as a bare word: list(item), sum(items, qty),
  // sort(moves, _t, desc). Those positions take the WORD, never a variable of the
  // same name — a face with a field called `qty` still sums the records' qty.
  static bool name_position(const std::string& fn, size_t i) {
    if (fn == "list" || fn == "records") return i == 0;
    if (fn == "sum" || fn == "avg" || fn == "min" || fn == "max" || fn == "join" || fn == "map" || fn == "pluck" ||
        fn == "min_by" || fn == "max_by" || fn == "group_by" || fn == "sum_by" || fn == "avg_by") return i == 1;
    if (fn == "where" || fn == "filter" || fn == "find" || fn == "unique" || fn == "latest") return i == 1;
    if (fn == "sort") return i == 1 || i == 2;
    return false;
  }
  bool defined(const std::string& n) const {
    if (I.scope.count(n) || I.lets.count(n) || I.state.count(n) || fields.count(n) || I.editors.count(n) || n == "dt" || n == "now" ||
        n == "width" || n == "height" || n == "frame" || n == "keys" || n == "focus" ||
        n == "mouse_x" || n == "mouse_y" || n == "mouse_dx" || n == "mouse_dy" || n == "mouse_buttons" ||
        n == "hovered" || n == "active" || n == "can_undo" || n == "can_redo" || n == "history_len" || n == "history_depth" ||
        n == "hit_id" || n == "hit_action" || n == "drag_x" || n == "drag_y" || n == "drag_dx" || n == "drag_dy" ||
        n == "wheel_dx" || n == "wheel_dy" || n == "gesture_type" || n == "gesture_val1" || n == "gesture_val2")
      return true;
    if (n.size() > 5 && n.rfind("_line") == n.size() - 5 && I.editors.count(n.substr(0, n.size() - 5))) return true;
    if (n.size() > 4 && n.rfind("_col") == n.size() - 4 && I.editors.count(n.substr(0, n.size() - 4))) return true;
    if (n.size() > 6 && n.rfind("_lines") == n.size() - 6 && I.editors.count(n.substr(0, n.size() - 6))) return true;
    if (n.size() > 5 && n.rfind("_path") == n.size() - 5 && I.editors.count(n.substr(0, n.size() - 5))) return true;
    if (n.size() > 6 && n.rfind("_dirty") == n.size() - 6 && I.editors.count(n.substr(0, n.size() - 6))) return true;
    return false;
  }
  // A bare word where a name is expected; anything else is an expression.
  // A name written as a bare word, in a position that wants a name. A kind can
  // have a hyphen in it — this machine keeps `horizon-brief` and
  // `dream-storage` — and `list(horizon-brief)` parses as a SUBTRACTION of two
  // variables, so every such kind read as a fault and drew nothing. In a name
  // position a hyphen is part of the name, which is what it looks like.
  static bool hyphen_word(const Expr& e, std::string& out) {
    if (e.k == Expr::K::Var) { out = e.s; return true; }
    if (e.k == Expr::K::Binary && e.s == "-" && e.kids.size() == 2) {
      std::string l, r;
      if (!hyphen_word(*e.kids[0], l)) return false;
      if (e.kids[1]->k == Expr::K::Num) {
        char b[32];
        std::snprintf(b, sizeof b, "%g", e.kids[1]->n);
        r = b;
      } else if (!hyphen_word(*e.kids[1], r)) {
        return false;
      }
      out = l + "-" + r;
      return true;
    }
    return false;
  }

  Value word(const Expr& e) {
    std::string name;
    if (hyphen_word(e, name)) return Value::str(name);
    return eval(e);
  }
  // A colour or other word that may also be a variable: the variable wins when
  // it exists (`set c = "red"; paint rect 0 0 9 9 c`), else the word itself.
  Value word_or_value(const Expr& e) {
    if (e.k == Expr::K::Var && !defined(e.s)) return Value::str(e.s);
    return eval(e);
  }

  Value var(const std::string& name) {
    auto s = I.scope.find(name);
    if (s != I.scope.end()) return s->second;
    auto l = I.lets.find(name);
    if (l != I.lets.end()) return l->second;
    auto g = I.given.find(name);
    if (g != I.given.end()) return g->second;
    auto st = I.state.find(name);
    if (st != I.state.end()) return st->second;
    auto f = fields.find(name);
    if (f != fields.end()) return Value::str(f->second);
    auto ed_it = I.editors.find(name);
    if (ed_it != I.editors.end()) {
      std::string out;
      for (size_t i = 0; i < ed_it->second.lines.size(); ++i) {
        if (i) out += "\n";
        out += ed_it->second.lines[i];
      }
      return Value::str(out);
    }
    if (name.size() > 5 && name.rfind("_line") == name.size() - 5) {
      std::string p = name.substr(0, name.size() - 5);
      auto it = I.editors.find(p);
      if (it != I.editors.end()) return Value::num(it->second.cursor_line + 1);
    }
    if (name.size() > 4 && name.rfind("_col") == name.size() - 4) {
      std::string p = name.substr(0, name.size() - 4);
      auto it = I.editors.find(p);
      if (it != I.editors.end()) return Value::num(it->second.cursor_col + 1);
    }
    if (name.size() > 6 && name.rfind("_lines") == name.size() - 6) {
      std::string p = name.substr(0, name.size() - 6);
      auto it = I.editors.find(p);
      if (it != I.editors.end()) return Value::num(it->second.lines.size());
    }
    if (name.size() > 5 && name.rfind("_path") == name.size() - 5) {
      std::string p = name.substr(0, name.size() - 5);
      auto it = I.editors.find(p);
      if (it != I.editors.end()) return Value::str(it->second.path);
    }
    if (name.size() > 6 && name.rfind("_dirty") == name.size() - 6) {
      std::string p = name.substr(0, name.size() - 6);
      auto it = I.editors.find(p);
      if (it != I.editors.end()) return Value::boolean(it->second.dirty);
    }
    if (name == "dt") return Value::num(I.dt_ms);
    if (name == "now") return Value::num(I.now_ms);
    if (name == "width") return Value::num(I.paint_w);
    if (name == "height") return Value::num(I.paint_h);
    if (name == "frame") return Value::num(I.frame_no);
    if (name == "mouse_x") return Value::num((double)I.mouse_x);
    if (name == "mouse_y") return Value::num((double)I.mouse_y);
    if (name == "mouse_dx") return Value::num((double)I.mouse_dx);
    if (name == "mouse_dy") return Value::num((double)I.mouse_dy);
    if (name == "mouse_buttons") return Value::num((double)I.mouse_buttons);
    if (name == "hovered") return Value::str(I.hovered_widget);
    if (name == "active") return Value::str(I.active_widget);
    if (name == "can_undo") return Value::boolean(!I.undo_stack.empty());
    if (name == "can_redo") return Value::boolean(!I.redo_stack.empty());
    if (name == "history_len" || name == "history_depth") return Value::num((double)I.undo_stack.size());
    if (name == "keys") {
      Value r = Value::make_rec();
      for (const auto& k : I.keys_held) r.set_field(k, Value::boolean(true));
      return r;
    }
    if (name == "focus") return Value::str(focus);
    fault("'" + name + "' is not a let, a state, a field or a loop variable");
    return Value::null();
  }

  Value list_of(const std::string& kind) {
    auto c = I.list_cache.find(kind);
    if (c != I.list_cache.end()) return c->second;
    const uint64_t rev = host.store_revision ? host.store_revision() : 0;
    if (rev) {
      auto m = I.list_memo.find(kind);
      if (m != I.list_memo.end() && m->second.first == rev) {
        I.list_cache[kind] = m->second.second;
        I.reads[kind] = m->second.second.list ? m->second.second.list->size() : 0;
        return m->second.second;
      }
    }
    Value v = Value::make_list();
    if (host.store_list) {
      Value parsed = from_json(host.store_list(kind));
      if (parsed.k == Value::K::List) v = parsed;
    }
    // A kind with identity reads as its current rows: the newest record per key.
    auto kk = I.kind_keys.find(kind);
    if (kk != I.kind_keys.end() && v.list && !v.list->empty()) {
      std::vector<Value> args{v, Value::str(kk->second)};
      v = call("unique", args);
    }
    I.list_cache[kind] = v;
    if (rev) I.list_memo[kind] = {rev, v};
    I.reads[kind] = v.list ? v.list->size() : 0;
    return v;
  }

  static bool same(const Value& a, const Value& b) {
    if (a.k == Value::K::Num || b.k == Value::K::Num) return std::fabs(a.number() - b.number()) < 1e-9 && (a.k != Value::K::Str || b.k != Value::K::Str);
    if (a.k == Value::K::Bool || b.k == Value::K::Bool) return a.truthy() == b.truthy();
    if (a.k == Value::K::Null || b.k == Value::K::Null) return a.k == b.k;
    return a.text() == b.text();
  }

  Value call(const std::string& name, std::vector<Value>& a) {
    auto arg = [&](size_t i) { return i < a.size() ? a[i] : Value::null(); };
    auto want = [&](size_t n, const char* sig) { if (a.size() < n) { fault(std::string(name) + "(…) expects " + sig); return false; } return true; };

    // Channels & Streaming
    if (name == "emit") {
      if (!want(2, "channel, value")) return Value::boolean(false);
      I.get_channel(arg(0).text())->emit(arg(1));
      return Value::boolean(true);
    }
    if (name == "recv" || name == "channel_recv") {
      if (!want(1, "channel")) return Value::null();
      return I.get_channel(arg(0).text())->recv();
    }
    if (name == "poll" || name == "channel_poll") {
      if (!want(1, "channel")) return Value::null();
      return I.get_channel(arg(0).text())->poll();
    }
    if (name == "drain" || name == "channel_drain") {
      if (!want(1, "channel")) return Value::make_list();
      return I.get_channel(arg(0).text())->drain();
    }
    if (name == "has_data" || name == "channel_has_data") {
      if (!want(1, "channel")) return Value::boolean(false);
      return Value::boolean(I.get_channel(arg(0).text())->has_data());
    }
    if (name == "chan_len" || name == "channel_len") {
      if (!want(1, "channel")) return Value::num(0);
      return Value::num((double)I.get_channel(arg(0).text())->size());
    }
    if (name == "chan") {
      return Value::str(arg(0).text());
    }
    if (name == "stream") {
      std::string prompt = a.size() > 1 ? arg(1).text() : arg(0).text();
      Value out = Value::make_list();
      std::string text_to_stream = prompt.empty() ? "System stream active." : prompt;
      std::vector<std::string> words = tokens_of(text_to_stream);
      for (const auto& w : words) out.list->push_back(Value::str(w + " "));
      return out;
    }

    // Compute Engine & Tensors
    if (name == "matmul" || name == "compute.matmul") {
      if (!want(2, "matrix_a, matrix_b")) return Value::make_list();
      Value ma = arg(0), mb = arg(1);
      if (ma.k != Value::K::List || mb.k != Value::K::List || !ma.list || !mb.list || ma.list->empty() || mb.list->empty())
        return Value::make_list();
      size_t M = ma.list->size();
      const Value& row0_a = (*ma.list)[0];
      size_t K_a = (row0_a.k == Value::K::List && row0_a.list) ? row0_a.list->size() : 1;
      size_t K_b = mb.list->size();
      const Value& row0_b = (*mb.list)[0];
      size_t N = (row0_b.k == Value::K::List && row0_b.list) ? row0_b.list->size() : 1;
      
      Value out = Value::make_list();
      for (size_t i = 0; i < M; ++i) {
        Value row_out = Value::make_list();
        for (size_t j = 0; j < N; ++j) {
          double sum = 0;
          for (size_t k = 0; k < std::min(K_a, K_b); ++k) {
            double va = 0, vb = 0;
            const Value& r_a = (*ma.list)[i];
            if (r_a.k == Value::K::List && r_a.list && k < r_a.list->size()) va = (*r_a.list)[k].number();
            else if (k == 0) va = r_a.number();
            const Value& r_b = (*mb.list)[k];
            if (r_b.k == Value::K::List && r_b.list && j < r_b.list->size()) vb = (*r_b.list)[j].number();
            else if (j == 0) vb = r_b.number();
            sum += va * vb;
          }
          row_out.list->push_back(Value::num(sum));
        }
        out.list->push_back(row_out);
      }
      return out;
    }
    if (name == "softmax" || name == "compute.softmax") {
      if (!want(1, "vector")) return Value::make_list();
      Value v = arg(0);
      if (v.k != Value::K::List || !v.list || v.list->empty()) return Value::make_list();
      double max_val = (*v.list)[0].number();
      for (const auto& item : *v.list) max_val = std::max(max_val, item.number());
      double sum_exp = 0;
      std::vector<double> exps;
      exps.reserve(v.list->size());
      for (const auto& item : *v.list) {
        double e = std::exp(item.number() - max_val);
        exps.push_back(e);
        sum_exp += e;
      }
      Value out = Value::make_list();
      for (double e : exps) {
        out.list->push_back(Value::num(sum_exp > 0 ? e / sum_exp : 0));
      }
      return out;
    }
    if (name == "reduce" || name == "compute.reduce") {
      if (!want(2, "vector, op")) return Value::num(0);
      Value v = arg(0);
      std::string op = arg(1).text();
      if (v.k != Value::K::List || !v.list || v.list->empty()) return Value::num(0);
      if (op == "sum") {
        double s = 0;
        for (const auto& item : *v.list) s += item.number();
        return Value::num(s);
      }
      if (op == "mean" || op == "avg") {
        double s = 0;
        for (const auto& item : *v.list) s += item.number();
        return Value::num(s / (double)v.list->size());
      }
      if (op == "max") {
        double m = (*v.list)[0].number();
        for (const auto& item : *v.list) m = std::max(m, item.number());
        return Value::num(m);
      }
      if (op == "min") {
        double m = (*v.list)[0].number();
        for (const auto& item : *v.list) m = std::min(m, item.number());
        return Value::num(m);
      }
      if (op == "prod") {
        double p = 1.0;
        for (const auto& item : *v.list) p *= item.number();
        return Value::num(p);
      }
      if (op == "l2") {
        double s = 0;
        for (const auto& item : *v.list) { double num = item.number(); s += num * num; }
        return Value::num(std::sqrt(s));
      }
      return Value::num(0);
    }
    if (name == "dot" || name == "compute.dot") {
      if (!want(2, "vector_a, vector_b")) return Value::num(0);
      Value va = arg(0), vb = arg(1);
      if (va.k != Value::K::List || vb.k != Value::K::List || !va.list || !vb.list) return Value::num(0);
      double dot_val = 0;
      size_t n = std::min(va.list->size(), vb.list->size());
      for (size_t i = 0; i < n; ++i) dot_val += (*va.list)[i].number() * (*vb.list)[i].number();
      return Value::num(dot_val);
    }
    if (name == "cosine" || name == "cosine_sim" || name == "compute.cosine") {
      if (!want(2, "vector_a, vector_b")) return Value::num(0);
      Value va = arg(0), vb = arg(1);
      if (va.k != Value::K::List || vb.k != Value::K::List || !va.list || !vb.list) return Value::num(0);
      double dot_val = 0, norm_a = 0, norm_b = 0;
      size_t n = std::min(va.list->size(), vb.list->size());
      for (size_t i = 0; i < n; ++i) {
        double a_val = (*va.list)[i].number(), b_val = (*vb.list)[i].number();
        dot_val += a_val * b_val;
        norm_a += a_val * a_val;
        norm_b += b_val * b_val;
      }
      double denom = std::sqrt(norm_a) * std::sqrt(norm_b);
      return Value::num(denom > 0 ? dot_val / denom : 0.0);
    }
    if (name == "embed" || name == "compute.embed") {
      if (!want(1, "text [, model]")) return Value::make_list();
      std::string text_str = arg(0).text();
      Value out = Value::make_list();
      constexpr size_t kEmbedDim = 128;
      double sum_sq = 0;
      std::vector<double> raw(kEmbedDim, 0.0);
      for (size_t i = 0; i < kEmbedDim; ++i) {
        uint64_t h = 14695981039346656037ULL + i * 1099511628211ULL;
        for (char c : text_str) {
          h = (h ^ (uint8_t)c) * 1099511628211ULL;
        }
        double val = std::sin((double)(h % 1000000) * 0.00001 + (double)i * 0.1);
        raw[i] = val;
        sum_sq += val * val;
      }
      double norm = std::sqrt(sum_sq);
      for (size_t i = 0; i < kEmbedDim; ++i) {
        out.list->push_back(Value::num(norm > 0 ? raw[i] / norm : 0.0));
      }
      return out;
    }

    // Spatial Physics & Spring Animation
    if (name == "spring" || name == "spring_step") {
      double curr = arg(0).number();
      double target = a.size() > 1 ? arg(1).number() : curr;
      double vel = a.size() > 2 ? arg(2).number() : 0.0;
      double k = a.size() > 3 ? arg(3).number() : 180.0;
      double c = a.size() > 4 ? arg(4).number() : 12.0;
      double dt = a.size() > 5 ? (arg(5).number() > 1.0 ? arg(5).number() / 1000.0 : arg(5).number()) : (I.dt_ms / 1000.0);
      if (dt <= 0) dt = 0.016;
      double f = -k * (curr - target) - c * vel;
      double nvel = vel + f * dt;
      double npos = curr + nvel * dt;
      Value res = Value::make_rec();
      res.set_field("pos", Value::num(npos));
      res.set_field("vel", Value::num(nvel));
      res.set_field("value", Value::num(npos));
      return res;
    }
    if (name == "decay") {
      double val = arg(0).number();
      double vel = a.size() > 1 ? arg(1).number() : 0.0;
      double friction = a.size() > 2 ? arg(2).number() : 5.0;
      double dt = a.size() > 3 ? (arg(3).number() > 1.0 ? arg(3).number() / 1000.0 : arg(3).number()) : (I.dt_ms / 1000.0);
      double nvel = vel * std::exp(-friction * dt);
      double nval = val + nvel * dt;
      Value res = Value::make_rec();
      res.set_field("pos", Value::num(nval));
      res.set_field("vel", Value::num(nvel));
      return res;
    }
    if (name == "bounce") {
      double val = arg(0).number();
      double min_v = a.size() > 1 ? arg(1).number() : 0.0;
      double max_v = a.size() > 2 ? arg(2).number() : 100.0;
      double vel = a.size() > 3 ? arg(3).number() : 0.0;
      double rest = a.size() > 4 ? arg(4).number() : 0.75;
      if (val < min_v) { val = min_v; vel = -vel * rest; }
      else if (val > max_v) { val = max_v; vel = -vel * rest; }
      Value res = Value::make_rec();
      res.set_field("pos", Value::num(val));
      res.set_field("vel", Value::num(vel));
      return res;
    }

    // Time-Travel & History
    if (name == "history") {
      Value h = Value::make_rec();
      h.set_field("can_undo", Value::boolean(!I.undo_stack.empty()));
      h.set_field("can_redo", Value::boolean(!I.redo_stack.empty()));
      h.set_field("depth", Value::num((double)I.undo_stack.size()));
      return h;
    }
    if (name == "snapshot" || name == "history_snapshot") {
      StateSnapshot snap;
      snap.label = a.size() > 0 ? arg(0).text() : "";
      snap.timestamp = I.now_ms;
      snap.state = I.state;
      snap.fields = fields;
      if (I.undo_stack.size() >= I.max_history) I.undo_stack.erase(I.undo_stack.begin());
      I.undo_stack.push_back(snap);
      I.redo_stack.clear();
      return Value::boolean(true);
    }
    if (name == "undo" || name == "history_undo" || name == "revert") {
      if (I.undo_stack.empty()) return Value::boolean(false);
      StateSnapshot curr;
      curr.timestamp = I.now_ms;
      curr.state = I.state;
      curr.fields = fields;
      I.redo_stack.push_back(curr);
      StateSnapshot snap = I.undo_stack.back();
      I.undo_stack.pop_back();
      I.state = snap.state;
      fields = snap.fields;
      dirty = true;
      return Value::boolean(true);
    }
    if (name == "redo" || name == "history_redo") {
      if (I.redo_stack.empty()) return Value::boolean(false);
      StateSnapshot curr;
      curr.timestamp = I.now_ms;
      curr.state = I.state;
      curr.fields = fields;
      I.undo_stack.push_back(curr);
      StateSnapshot snap = I.redo_stack.back();
      I.redo_stack.pop_back();
      I.state = snap.state;
      fields = snap.fields;
      dirty = true;
      return Value::boolean(true);
    }

    // Schemas & Validation
    if (name == "validate" || name == "is_valid") {
      if (!want(2, "record, type_name")) return name == "is_valid" ? Value::boolean(false) : Value::null();
      Value rec = arg(0);
      std::string tname = arg(1).text();
      const TypeDef* tdef = nullptr;
      for (const auto& t : I.types) if (t.name == tname) { tdef = &t; break; }
      if (!tdef) return name == "is_valid" ? Value::boolean(true) : rec;
      bool ok = true;
      for (const auto& f : tdef->fields) {
        const Value* fval = rec.field(f.name);
        if (!fval) { ok = false; break; }
        if (f.type_name == "Enum" && !f.enum_values.empty()) {
          bool enum_found = false;
          for (const auto& ev : f.enum_values) if (ev == fval->text()) { enum_found = true; break; }
          if (!enum_found) { ok = false; break; }
        } else if (f.type_name == "Str" || f.type_name == "String") {
          if (fval->k != Value::K::Str) { ok = false; break; }
        } else if (f.type_name == "Num" || f.type_name == "Number" || f.type_name == "Int") {
          if (fval->k != Value::K::Num) { ok = false; break; }
        } else if (f.type_name == "Bool" || f.type_name == "Boolean") {
          if (fval->k != Value::K::Bool) { ok = false; break; }
        }
      }
      if (name == "is_valid") return Value::boolean(ok);
      return ok ? rec : Value::null();
    }
    if (name == "list" || name == "records") { if (!want(1, "kind")) return Value::make_list(); return list_of(arg(0).text()); }
    if (name == "fetch" || name == "fetch_json") {
      if (!want(1, "url")) return name == "fetch_json" ? Value::make_rec() : Value::str("");
      std::string url = arg(0).text();
      if (url.empty()) return name == "fetch_json" ? Value::make_rec() : Value::str("");
      auto it = I.fetch_cache.find(url);
      if (it != I.fetch_cache.end()) {
        return name == "fetch_json" ? from_json(it->second) : Value::str(it->second);
      }
      std::string content;
      bool is_network = url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
      if (is_network && host.http_get) {
        content = host.http_get(url);
      } else if (host.read_file) {
        std::string path = (url.rfind("file://", 0) == 0) ? url.substr(7) : url;
        content = host.read_file(path);
      }
      I.fetch_cache[url] = content;
      return name == "fetch_json" ? from_json(content) : Value::str(content);
    }
    if (name == "count" || name == "len" || name == "length") {
      Value v = arg(0);
      if (v.k == Value::K::List) return Value::num(v.list ? (double)v.list->size() : 0);
      if (v.k == Value::K::Str) return Value::num((double)v.s.size());
      if (v.k == Value::K::Rec) return Value::num(v.rec ? (double)v.rec->size() : 0);
      return Value::num(0);
    }
    if (name == "sum" || name == "avg" || name == "min" || name == "max") {
      Value l = arg(0);
      if (l.k != Value::K::List) {
        // min/max over numbers
        if (name == "min" || name == "max") {
          double best = arg(0).number();
          for (size_t i = 1; i < a.size(); ++i) { double v = a[i].number(); best = name == "min" ? std::min(best, v) : std::max(best, v); }
          return Value::num(best);
        }
        return Value::num(0);
      }
      const std::string field = a.size() > 1 ? arg(1).text() : "";
      double acc = 0; size_t n = 0; double best = 0;
      for (const auto& r : *l.list) {
        double v = field.empty() ? r.number() : (r.field(field) ? r.field(field)->number() : 0);
        if (n == 0) best = v; else best = name == "min" ? std::min(best, v) : std::max(best, v);
        acc += v; ++n;
      }
      if (name == "sum") return Value::num(acc);
      if (name == "avg") return Value::num(n ? acc / (double)n : 0);
      return Value::num(n ? best : 0);
    }
    if (name == "where" || name == "filter") {
      if (!want(3, "list, field, value")) return Value::make_list();
      Value out = Value::make_list();
      Value l = arg(0);
      if (l.k != Value::K::List) return out;
      const std::string field = arg(1).text();
      for (const auto& r : *l.list) { const Value* f = r.field(field); if (f && same(*f, arg(2))) out.list->push_back(r); }
      return out;
    }
    if (name == "unique" || name == "latest") {
      // One record per value of `field`, the newest kept: how an append-only
      // store reads as a table of current rows.
      if (!want(2, "list, field")) return Value::make_list();
      Value l = arg(0);
      Value out = Value::make_list();
      if (l.k != Value::K::List) return out;
      const std::string field = arg(1).text();
      // The newest record per key by its own time (_t), whatever order the
      // list came in; keys keep the order they were first seen.
      std::vector<std::string> seen;
      for (const auto& r : *l.list) {
        const Value* f = r.field(field);
        const std::string key = f ? f->text() : "";
        auto at = std::find(seen.begin(), seen.end(), key);
        if (at == seen.end()) { seen.push_back(key); out.list->push_back(r); continue; }
        Value& have = (*out.list)[(size_t)(at - seen.begin())];
        const Value* t_new = r.field("_t");
        const Value* t_old = have.field("_t");
        if (t_new && (!t_old || t_new->number() > t_old->number())) have = r;
      }
      return out;
    }
    if (name == "find") {
      if (!want(3, "list, field, value")) return Value::null();
      Value l = arg(0);
      if (l.k != Value::K::List) return Value::null();
      const std::string field = arg(1).text();
      for (const auto& r : *l.list) { const Value* f = r.field(field); if (f && same(*f, arg(2))) return r; }
      return Value::null();
    }
    if (name == "sort") {
      if (!want(2, "list, field [, desc]")) return Value::make_list();
      Value l = arg(0);
      if (l.k != Value::K::List) return Value::make_list();
      Value out = Value::make_list();
      *out.list = *l.list;
      const std::string field = arg(1).text();
      const bool desc = a.size() > 2 && (arg(2).truthy() || arg(2).text() == "desc");
      std::stable_sort(out.list->begin(), out.list->end(), [&](const Value& x, const Value& y) {
        const Value* fx = x.field(field); const Value* fy = y.field(field);
        Value vx = fx ? *fx : Value::null(), vy = fy ? *fy : Value::null();
        bool less;
        if (vx.k == Value::K::Num && vy.k == Value::K::Num) less = vx.n < vy.n;
        else less = vx.text() < vy.text();
        return desc ? !less && !same(vx, vy) : less;
      });
      return out;
    }
    if (name == "take" || name == "first") {
      Value l = arg(0);
      if (l.k != Value::K::List) return name == "first" ? Value::null() : Value::make_list();
      if (name == "first") return l.list->empty() ? Value::null() : (*l.list)[0];
      Value out = Value::make_list();
      size_t n = (size_t)std::max(0.0, arg(1).number());
      for (size_t i = 0; i < l.list->size() && i < n; ++i) out.list->push_back((*l.list)[i]);
      return out;
    }
    if (name == "last") { Value l = arg(0); if (l.k != Value::K::List || l.list->empty()) return Value::null(); return l.list->back(); }
    if (name == "reverse") { Value l = arg(0); Value out = Value::make_list(); if (l.k == Value::K::List) { *out.list = *l.list; std::reverse(out.list->begin(), out.list->end()); } return out; }
    if (name == "similar") { if (!want(1, "text [, k]")) return Value::make_list(); if (!host.store_similar) return Value::make_list(); Value v = from_json(host.store_similar(arg(0).text(), a.size() > 1 ? (int)arg(1).number() : 5)); return v.k == Value::K::List ? v : Value::make_list(); }
    if (name == "query" || name == "fetch_query") {
      if (!want(1, "query_str")) return Value::make_list();
      if (!host.store_query) return Value::make_list();
      std::string q = arg(0).text();
      // Viewport-bounded rehydrate: clamp to the cap unless the face already
      // asked for a tighter LIMIT of its own, so a smaller explicit ask is
      // never widened.
      if (I.viewport_row_cap > 0) {
        std::string uq = q;
        for (auto& c : uq) c = (char)std::toupper((unsigned char)c);
        auto lp = uq.rfind("LIMIT");
        bool has_tighter = false;
        if (lp != std::string::npos) {
          size_t np = lp + 5;
          while (np < q.size() && std::isspace((unsigned char)q[np])) np++;
          size_t existing = std::strtoul(q.c_str() + np, nullptr, 10);
          has_tighter = existing > 0 && existing <= I.viewport_row_cap;
        }
        if (!has_tighter) {
          if (lp != std::string::npos) q = q.substr(0, lp);
          q += " LIMIT " + std::to_string(I.viewport_row_cap);
        }
      }
      Value v = from_json(host.store_query(q));
      return v.k == Value::K::List ? v : Value::make_list();
    }
    if (name == "get") { if (!want(2, "record, field")) return Value::null(); const Value* f = arg(0).field(arg(1).text()); return f ? *f : Value::null(); }
    if (name == "join") {
      Value l = arg(0);
      const std::string field = a.size() > 2 ? arg(1).text() : "";
      const std::string sep = a.size() > 2 ? arg(2).text() : (a.size() > 1 ? arg(1).text() : ", ");
      std::string o;
      if (l.k == Value::K::List) for (size_t i = 0; i < l.list->size(); ++i) { if (i) o += sep; const Value& r = (*l.list)[i]; o += field.empty() ? r.text() : (r.field(field) ? r.field(field)->text() : ""); }
      return Value::str(o);
    }
    if (name == "num" || name == "number") return Value::num(arg(0).number());
    if (name == "str" || name == "text") return Value::str(arg(0).text());
    if (name == "round") { double d = arg(0).number(); int p = a.size() > 1 ? (int)arg(1).number() : 0; double m = std::pow(10.0, p); return Value::num(std::round(d * m) / m); }
    if (name == "floor") return Value::num(std::floor(arg(0).number()));
    if (name == "ceil") return Value::num(std::ceil(arg(0).number()));
    if (name == "abs") return Value::num(std::fabs(arg(0).number()));
    if (name == "sqrt") return Value::num(std::sqrt(arg(0).number()));
    if (name == "sin") return Value::num(std::sin(arg(0).number()));
    if (name == "cos") return Value::num(std::cos(arg(0).number()));
    if (name == "pow" || name == "power") return Value::num(std::pow(arg(0).number(), arg(1).number()));
    if (name == "log" || name == "ln") { double x = arg(0).number(); return Value::num(x > 0 ? std::log(x) : 0); }
    if (name == "log10") { double x = arg(0).number(); return Value::num(x > 0 ? std::log10(x) : 0); }
    if (name == "atan2") return Value::num(std::atan2(arg(0).number(), arg(1).number()));
    if (name == "hypot") return Value::num(std::hypot(arg(0).number(), arg(1).number()));
    if (name == "sign" || name == "sgn") { double x = arg(0).number(); return Value::num(x > 0 ? 1 : x < 0 ? -1 : 0); }
    if (name == "lerp") { double v0 = arg(0).number(), v1 = arg(1).number(), t = arg(2).number(); return Value::num(v0 + (v1 - v0) * t); }
    if (name == "split") {
      std::string s = arg(0).text(), sep = a.size() > 1 ? arg(1).text() : " ";
      Value out = Value::make_list();
      if (sep.empty()) {
        for (char ch : s) out.list->push_back(Value::str(std::string(1, ch)));
      } else {
        size_t p = 0;
        while (p < s.size()) {
          size_t found = s.find(sep, p);
          if (found == std::string::npos) { out.list->push_back(Value::str(s.substr(p))); break; }
          out.list->push_back(Value::str(s.substr(p, found - p)));
          p = found + sep.size();
        }
      }
      return out;
    }
    if (name == "replace") {
      std::string s = arg(0).text(), search = arg(1).text(), repl = a.size() > 2 ? arg(2).text() : "";
      if (search.empty()) return Value::str(s);
      std::string out;
      size_t p = 0;
      while (p < s.size()) {
        size_t found = s.find(search, p);
        if (found == std::string::npos) { out += s.substr(p); break; }
        out += s.substr(p, found - p) + repl;
        p = found + search.size();
      }
      return Value::str(out);
    }
    if (name == "ends" || name == "ends_with") {
      std::string s = arg(0).text(), suffix = arg(1).text();
      if (suffix.size() > s.size()) return Value::boolean(false);
      return Value::boolean(s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0);
    }
    if (name == "index_of") {
      Value h = arg(0);
      if (h.k == Value::K::List && h.list) {
        for (size_t i = 0; i < h.list->size(); ++i) if (same((*h.list)[i], arg(1))) return Value::num((double)i);
        return Value::num(-1);
      }
      std::string s = h.text(), target = arg(1).text();
      size_t pos = s.find(target);
      return Value::num(pos == std::string::npos ? -1 : (double)pos);
    }
    if (name == "slice") {
      Value h = arg(0);
      int start = (int)arg(1).number();
      if (h.k == Value::K::List) {
        Value out = Value::make_list();
        if (h.list) {
          int len = a.size() > 2 ? (int)arg(2).number() : (int)h.list->size();
          if (start < 0) start += (int)h.list->size();
          start = std::clamp(start, 0, (int)h.list->size());
          for (int i = 0; i < len && start + i < (int)h.list->size(); ++i) {
            out.list->push_back((*h.list)[(size_t)(start + i)]);
          }
        }
        return out;
      }
      std::string s = h.text();
      if (start < 0) start += (int)s.size();
      start = std::clamp(start, 0, (int)s.size());
      size_t count = a.size() > 2 ? (size_t)std::max(0, (int)arg(2).number()) : std::string::npos;
      return Value::str(s.substr((size_t)start, count));
    }
    if (name == "flat" || name == "flatten") {
      Value l = arg(0);
      Value out = Value::make_list();
      if (l.k == Value::K::List && l.list) {
        for (const auto& item : *l.list) {
          if (item.k == Value::K::List && item.list) {
            for (const auto& sub : *item.list) out.list->push_back(sub);
          } else {
            out.list->push_back(item);
          }
        }
      }
      return out;
    }
    if (name == "map" || name == "pluck") {
      Value l = arg(0);
      std::string field = arg(1).text();
      Value out = Value::make_list();
      if (l.k == Value::K::List && l.list) {
        for (const auto& r : *l.list) {
          if (r.k == Value::K::Rec) {
            const Value* f = r.field(field);
            out.list->push_back(f ? *f : Value::null());
          } else {
            out.list->push_back(r);
          }
        }
      }
      return out;
    }
    if (name == "push" || name == "append") {
      Value l = arg(0);
      Value out = Value::make_list();
      if (l.k == Value::K::List && l.list) *out.list = *l.list;
      else if (l.k != Value::K::Null) out.list->push_back(l);
      for (size_t i = 1; i < a.size(); ++i) out.list->push_back(a[i]);
      return out;
    }
    if (name == "coalesce" || name == "default") {
      for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].k != Value::K::Null && (name == "default" ? a[i].truthy() : true)) return a[i];
      }
      return a.empty() ? Value::null() : a.back();
    }
    if (name == "keys") {
      Value r = arg(0);
      Value out = Value::make_list();
      if (r.k == Value::K::Rec && r.rec) {
        for (const auto& kv : *r.rec) out.list->push_back(Value::str(kv.first));
      }
      return out;
    }
    if (name == "values") {
      Value r = arg(0);
      Value out = Value::make_list();
      if (r.k == Value::K::Rec && r.rec) {
        for (const auto& kv : *r.rec) out.list->push_back(kv.second);
      }
      return out;
    }
    if (name == "entries") {
      Value r = arg(0);
      Value out = Value::make_list();
      if (r.k == Value::K::Rec && r.rec) {
        for (const auto& kv : *r.rec) {
          Value entry = Value::make_rec();
          entry.set_field("key", Value::str(kv.first));
          entry.set_field("value", kv.second);
          out.list->push_back(entry);
        }
      }
      return out;
    }
    if (name == "merge" || name == "assign") {
      Value out = Value::make_rec();
      for (const auto& item : a) {
        if (item.k == Value::K::Rec && item.rec) {
          for (const auto& kv : *item.rec) {
            out.set_field(kv.first, kv.second);
          }
        }
      }
      return out;
    }
    if (name == "uuid" || name == "guid") {
      static const char hex_chars[] = "0123456789abcdef";
      std::string out;
      out.reserve(36);
      for (int i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
          out += '-';
        } else if (i == 14) {
          out += '4';
        } else if (i == 19) {
          out += hex_chars[(std::rand() % 4) + 8];
        } else {
          out += hex_chars[std::rand() % 16];
        }
      }
      return Value::str(out);
    }
    if (name == "format_bytes" || name == "bytes") {
      double bytes = arg(0).number();
      static const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
      int u = 0;
      while (bytes >= 1024.0 && u < 5) {
        bytes /= 1024.0;
        ++u;
      }
      char buf[32];
      if (u == 0) std::snprintf(buf, sizeof buf, "%.0f B", bytes);
      else std::snprintf(buf, sizeof buf, "%.2f %s", bytes, units[u]);
      return Value::str(buf);
    }
    if (name == "format_currency" || name == "currency") {
      double val = arg(0).number();
      std::string sym = a.size() > 1 ? arg(1).text() : "$";
      char buf[64];
      std::snprintf(buf, sizeof buf, "%.2f", std::fabs(val));
      std::string s(buf);
      size_t dot = s.find('.');
      std::string int_part = (dot == std::string::npos) ? s : s.substr(0, dot);
      std::string frac_part = (dot == std::string::npos) ? "" : s.substr(dot);
      std::string formatted;
      int count = 0;
      for (int i = (int)int_part.size() - 1; i >= 0; --i) {
        if (count > 0 && count % 3 == 0) formatted = "," + formatted;
        formatted = int_part[(size_t)i] + formatted;
        ++count;
      }
      return Value::str((val < 0 ? "-" : "") + sym + formatted + frac_part);
    }
    if (name == "capitalize" || name == "titlecase") {
      std::string s = arg(0).text();
      bool cap = true;
      for (char& c : s) {
        if (is_space(c) || c == '_' || c == '-') {
          cap = true;
        } else if (cap) {
          if (c >= 'a' && c <= 'z') c = (char)(c - 32);
          cap = false;
        }
      }
      return Value::str(s);
    }
    if (name == "repeat") {
      std::string s = arg(0).text();
      int count = (int)arg(1).number();
      std::string out;
      for (int i = 0; i < count && i < 10000; ++i) out += s;
      return Value::str(out);
    }
    if (name == "min_by" || name == "max_by") {
      Value l = arg(0);
      if (l.k != Value::K::List || !l.list || l.list->empty()) return Value::null();
      std::string field = arg(1).text();
      const Value* best_item = &(*l.list)[0];
      const Value* best_val = best_item->field(field);
      double best_num = best_val ? best_val->number() : 0.0;
      for (size_t i = 1; i < l.list->size(); ++i) {
        const Value& item = (*l.list)[i];
        const Value* f = item.field(field);
        double cur_num = f ? f->number() : 0.0;
        if (name == "min_by" ? (cur_num < best_num) : (cur_num > best_num)) {
          best_num = cur_num;
          best_item = &item;
        }
      }
      return *best_item;
    }
    if (name == "sum_by" || name == "avg_by") {
      Value l = arg(0);
      if (l.k != Value::K::List || !l.list || l.list->empty()) return Value::num(0);
      std::string field = arg(1).text();
      double acc = 0;
      for (const auto& item : *l.list) {
        const Value* f = item.field(field);
        acc += f ? f->number() : 0.0;
      }
      if (name == "sum_by") return Value::num(acc);
      return Value::num(acc / (double)l.list->size());
    }
    if (name == "group_by") {
      Value l = arg(0);
      Value out = Value::make_rec();
      if (l.k == Value::K::List && l.list) {
        std::string field = arg(1).text();
        for (const auto& item : *l.list) {
          const Value* f = item.field(field);
          std::string group_key = f ? f->text() : "default";
          const Value* existing = out.field(group_key);
          if (!existing) {
            Value group_list = Value::make_list();
            group_list.list->push_back(item);
            out.set_field(group_key, group_list);
          } else {
            const_cast<Value*>(existing)->list->push_back(item);
          }
        }
      }
      return out;
    }
    if (name == "range") {
      int start = 0, end_val = 0, step = 1;
      if (a.size() == 1) {
        end_val = (int)arg(0).number();
      } else if (a.size() >= 2) {
        start = (int)arg(0).number();
        end_val = (int)arg(1).number();
        if (a.size() >= 3) step = (int)arg(2).number();
      }
      if (step == 0) step = 1;
      Value out = Value::make_list();
      if (step > 0) {
        for (int v = start; v < end_val && out.list->size() < 10000; v += step) out.list->push_back(Value::num(v));
      } else {
        for (int v = start; v > end_val && out.list->size() < 10000; v += step) out.list->push_back(Value::num(v));
      }
      return out;
    }
    if (name == "random") { double lo = a.size() > 1 ? arg(0).number() : 0, hi = a.size() > 1 ? arg(1).number() : (a.size() ? arg(0).number() : 1); return Value::num(lo + (hi - lo) * ((double)std::rand() / (double)RAND_MAX)); }
    if (name == "clamp") return Value::num(std::max(arg(1).number(), std::min(arg(2).number(), arg(0).number())));
    if (name == "upper") { std::string s = arg(0).text(); for (auto& c : s) if (c >= 'a' && c <= 'z') c = (char)(c - 32); return Value::str(s); }
    if (name == "lower") { std::string s = arg(0).text(); for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32); return Value::str(s); }
    if (name == "trim") return Value::str(trim(arg(0).text()));
    if (name == "contains") { Value h = arg(0); if (h.k == Value::K::List) { for (const auto& r : *h.list) if (same(r, arg(1))) return Value::boolean(true); return Value::boolean(false); } return Value::boolean(h.text().find(arg(1).text()) != std::string::npos); }
    if (name == "starts") return Value::boolean(arg(0).text().rfind(arg(1).text(), 0) == 0);
    if (name == "pad") { std::string s = arg(0).text(); size_t n = (size_t)std::max(0.0, arg(1).number()); std::string p = a.size() > 2 ? arg(2).text() : " "; while (s.size() < n && !p.empty()) s = p + s; return Value::str(s); }
    if (name == "fixed") { char b[64]; std::snprintf(b, sizeof b, "%.*f", a.size() > 1 ? (int)arg(1).number() : 2, arg(0).number()); return Value::str(b); }
    if (name == "if" || name == "iff") return arg(0).truthy() ? arg(1) : arg(2);
    if (name == "empty") return Value::boolean(!arg(0).truthy());
    if (name == "now") return Value::num(host.now_ms ? host.now_ms() : I.now_ms);
    if (name == "time" || name == "clock" || name == "date" || name == "ago" || name == "datetime") {
      double ms = a.empty() ? (host.now_ms ? host.now_ms() : 0) : arg(0).number();
      if (name == "ago") {
        double d = (host.now_ms ? host.now_ms() : ms) - ms;
        if (d < 0) d = 0;
        long s = (long)(d / 1000);
        if (s < 60) return Value::str(std::to_string(s) + "s ago");
        if (s < 3600) return Value::str(std::to_string(s / 60) + "m ago");
        if (s < 86400) return Value::str(std::to_string(s / 3600) + "h ago");
        return Value::str(std::to_string(s / 86400) + "d ago");
      }
      long long msi = (long long)ms;
      long long days = msi / 86400000LL, rem = msi % 86400000LL;
      if (rem < 0) { rem += 86400000LL; days -= 1; }
      int hh = (int)(rem / 3600000LL), mm = (int)((rem / 60000LL) % 60), ss = (int)((rem / 1000LL) % 60);
      long long z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097, doe = z - era * 146097;
      long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, y = yoe + era * 400;
      long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
      int d = (int)(doy - (153 * mp + 2) / 5 + 1), m = (int)(mp < 10 ? mp + 3 : mp - 9);
      y += (m <= 2 ? 1 : 0);
      char b[48];
      if (name == "date") std::snprintf(b, sizeof b, "%04lld-%02d-%02d", y, m, d);
      else if (name == "datetime") std::snprintf(b, sizeof b, "%04lld-%02d-%02d %02d:%02d", y, m, d, hh, mm);
      else std::snprintf(b, sizeof b, "%02d:%02d:%02d", hh, mm, ss);
      return Value::str(b);
    }
    if (name == "machine") {
      /* A reading from the kernel, or nothing: numbers come back as numbers. */
      if (!host.machine) return Value::str("");
      const std::string v = host.machine(arg(0).text());
      if (!v.empty() && v.find_first_not_of("0123456789.") == std::string::npos) return Value::num(std::atof(v.c_str()));
      return Value::str(v);
    }
    if (name == "organ") { if (!host.organ) return Value::str("error: no organs on this substrate"); return Value::str(host.organ(arg(0).text(), arg(1).k == Value::K::Rec ? to_json(arg(1)) : arg(1).text())); }
    if (name == "json") return Value::str(to_json(arg(0)));
    if (name == "parse") return from_json(arg(0).text());
    if (name == "keys") { Value r = arg(0); Value out = Value::make_list(); if (r.k == Value::K::Rec) for (const auto& kv : *r.rec) out.list->push_back(Value::str(kv.first)); return out; }
    if (name == "range") { Value out = Value::make_list(); double lo = a.size() > 1 ? arg(0).number() : 0, hi = a.size() > 1 ? arg(1).number() : arg(0).number(); for (double v = lo; v < hi && out.list->size() < 10000; v += 1) out.list->push_back(Value::num(v)); return out; }
    if (name == "read_file" || name == "file_read") {
      if (!want(1, "path")) return Value::str("");
      if (!host.read_file) { fault("no file system is bound to this face"); return Value::str(""); }
      return Value::str(host.read_file(arg(0).text()));
    }
    if (name == "write_file" || name == "file_write") {
      if (!want(2, "path, data")) return Value::boolean(false);
      if (!host.write_file) { fault("no file system is bound to this face"); return Value::boolean(false); }
      return Value::boolean(host.write_file(arg(0).text(), arg(1).text()));
    }
    if (name == "list_dir" || name == "dir_list") {
      if (!want(1, "path")) return Value::make_list();
      if (!host.list_dir) { fault("no file system is bound to this face"); return Value::make_list(); }
      auto entries = host.list_dir(arg(0).text());
      Value out = Value::make_list();
      for (const auto& entry : entries) out.list->push_back(Value::str(entry));
      return out;
    }
    if (name == "dream") {
      if (!want(1, "what")) return Value::str("");
      if (host.say) host.say("dreaming: " + arg(0).text());
      return Value::str("dream: " + arg(0).text());
    }
    fault("no function is called " + name + "()");
    return Value::null();
  }

  Value eval(const Expr& e) {
    switch (e.k) {
      case Expr::K::Null: return Value::null();
      case Expr::K::Num: return Value::num(e.n);
      case Expr::K::Bool: return Value::boolean(e.n != 0);
      case Expr::K::Str: return Value::str(e.s);
      case Expr::K::Interp: { std::string o; for (const auto& k : e.kids) o += eval(*k).text(); return Value::str(o); }
      case Expr::K::Var: return var(e.s);
      case Expr::K::Field: {
        Value o = eval(*e.kids[0]);
        if (o.k == Value::K::Rec) { const Value* f = o.field(e.s); return f ? *f : Value::null(); }
        if (o.k == Value::K::List) {
          if (e.s == "length" || e.s == "count") return Value::num((double)o.list->size());
          if (e.s == "first") return o.list->empty() ? Value::null() : (*o.list)[0];
          if (e.s == "last") return o.list->empty() ? Value::null() : o.list->back();
        }
        if (o.k == Value::K::Str && e.s == "length") return Value::num((double)o.s.size());
        return Value::null();
      }
      case Expr::K::Index: {
        Value o = eval(*e.kids[0]);
        Value i = eval(*e.kids[1]);
        if (o.k == Value::K::List) { long n = (long)i.number(); if (n < 0) n += (long)o.list->size(); if (n < 0 || (size_t)n >= o.list->size()) return Value::null(); return (*o.list)[(size_t)n]; }
        if (o.k == Value::K::Rec) { const Value* f = o.field(i.text()); return f ? *f : Value::null(); }
        return Value::null();
      }
      case Expr::K::List: { Value l = Value::make_list(); for (const auto& k : e.kids) l.list->push_back(eval(*k)); return l; }
      case Expr::K::Rec: { Value r = Value::make_rec(); for (size_t i = 0; i < e.kids.size(); ++i) r.set_field(e.keys[i], eval(*e.kids[i])); return r; }
      case Expr::K::Call: {
        std::vector<Value> args;
        for (size_t i = 0; i < e.kids.size(); ++i) args.push_back(name_position(e.s, i) ? word(*e.kids[i]) : eval(*e.kids[i]));
        return call(e.s, args);
      }
      case Expr::K::Unary: {
        Value v = eval(*e.kids[0]);
        if (e.s == "not") return Value::boolean(!v.truthy());
        return Value::num(-v.number());
      }
      case Expr::K::Binary: {
        if (e.s == "and") { Value l = eval(*e.kids[0]); if (!l.truthy()) return l; return eval(*e.kids[1]); }
        if (e.s == "or") { Value l = eval(*e.kids[0]); if (l.truthy()) return l; return eval(*e.kids[1]); }
        Value l = eval(*e.kids[0]), r = eval(*e.kids[1]);
        if (e.s == "+") {
          if (l.k == Value::K::Str || r.k == Value::K::Str) return Value::str(l.text() + r.text());
          if (l.k == Value::K::List && r.k == Value::K::List) { Value out = Value::make_list(); *out.list = *l.list; out.list->insert(out.list->end(), r.list->begin(), r.list->end()); return out; }
          return Value::num(l.number() + r.number());
        }
        if (e.s == "-") return Value::num(l.number() - r.number());
        if (e.s == "*") return Value::num(l.number() * r.number());
        if (e.s == "/") { double d = r.number(); return Value::num(d == 0 ? 0 : l.number() / d); }
        if (e.s == "%") { double d = r.number(); return Value::num(d == 0 ? 0 : std::fmod(l.number(), d)); }
        if (e.s == "==") return Value::boolean(same(l, r));
        if (e.s == "!=") return Value::boolean(!same(l, r));
        const bool both_num = (l.k == Value::K::Num || l.k == Value::K::Bool || l.k == Value::K::Null) && (r.k == Value::K::Num || r.k == Value::K::Bool || r.k == Value::K::Null);
        int cmp;
        if (both_num) { double a2 = l.number(), b2 = r.number(); cmp = a2 < b2 ? -1 : a2 > b2 ? 1 : 0; }
        else if (l.k == Value::K::Str && r.k == Value::K::Str) { const std::string& a2 = l.s; const std::string& b2 = r.s; cmp = a2 < b2 ? -1 : a2 > b2 ? 1 : 0; }
        else { double a2 = l.number(), b2 = r.number(); cmp = a2 < b2 ? -1 : a2 > b2 ? 1 : 0; }
        if (e.s == "<") return Value::boolean(cmp < 0);
        if (e.s == "<=") return Value::boolean(cmp <= 0);
        if (e.s == ">") return Value::boolean(cmp > 0);
        if (e.s == ">=") return Value::boolean(cmp >= 0);
        return Value::null();
      }
    }
    return Value::null();
  }

  void exec(const Stmt& s, std::vector<PaintCmd>& paint) {
    if (stop) return;
    switch (s.k) {
      case Stmt::K::Set: {
        Value v = s.expr ? eval(*s.expr) : Value::null();
        if (I.editors.count(s.name)) {
          auto& ed = I.editors[s.name];
          std::string content = v.text();
          ed.lines.clear();
          std::string cur;
          for (char c : content) {
            if (c == '\r') continue;
            if (c == '\n') { ed.lines.push_back(cur); cur.clear(); }
            else cur += c;
          }
          ed.lines.push_back(cur);
          if (ed.lines.empty()) ed.lines.push_back("");
          ed.cursor_line = std::clamp(ed.cursor_line, 0, (int)ed.lines.size() - 1);
          ed.cursor_col = std::clamp(ed.cursor_col, 0, (int)ed.lines[ed.cursor_line].size());
          ed.dirty = true;
          return;
        }
        if (s.name.size() > 5 && s.name.rfind("_path") == s.name.size() - 5) {
          std::string p = s.name.substr(0, s.name.size() - 5);
          if (I.editors.count(p)) {
            auto& ed = I.editors[p];
            ed.path = v.text();
            if (host.read_file && !ed.path.empty()) {
              std::string content = host.read_file(ed.path);
              ed.lines.clear();
              std::string cur;
              for (char c : content) {
                if (c == '\r') continue;
                if (c == '\n') { ed.lines.push_back(cur); cur.clear(); }
                else cur += c;
              }
              ed.lines.push_back(cur);
              if (ed.lines.empty()) ed.lines.push_back("");
              ed.cursor_line = 0;
              ed.cursor_col = 0;
              ed.scroll_line = 0;
              ed.scroll_col = 0;
              ed.dirty = false;
            }
            return;
          }
        }
        if (s.name.size() > 5 && s.name.rfind("_line") == s.name.size() - 5) {
          std::string p = s.name.substr(0, s.name.size() - 5);
          if (I.editors.count(p)) {
            auto& ed = I.editors[p];
            ed.cursor_line = std::clamp((int)v.number() - 1, 0, (int)ed.lines.size() - 1);
            return;
          }
        }
        if (s.name.size() > 4 && s.name.rfind("_col") == s.name.size() - 4) {
          std::string p = s.name.substr(0, s.name.size() - 4);
          if (I.editors.count(p)) {
            auto& ed = I.editors[p];
            ed.cursor_col = std::clamp((int)v.number() - 1, 0, (int)ed.lines[ed.cursor_line].size());
            return;
          }
        }
        if (fields.count(s.name) && I.state.count(s.name)) {
          fields[s.name] = v.text();
          I.state[s.name] = v;
          dirty = true;
          return;
        }
        if (fields.count(s.name)) { fields[s.name] = v.text(); return; }
        if (I.state.count(s.name)) { I.state[s.name] = v; dirty = true; return; }
        if (I.lets.count(s.name)) { I.lets[s.name] = v; return; }
        if (I.scope.count(s.name)) { I.scope[s.name] = v; return; }
        // a new name: a state, kept
        I.state[s.name] = v;
        dirty = true;
        return;
      }
      case Stmt::K::Put: {
        Value v = s.expr ? eval(*s.expr) : Value::null();
        if (v.k != Value::K::Rec) { fault("put " + s.name + " needs a record {field: value, …}"); return; }
        if (!host.store_put) { fault("no store is bound to this face"); return; }
        std::string id = host.store_put(s.name, to_json(v));
        I.list_cache.erase(s.name);
        I.list_memo.erase(s.name);
        if (id.empty()) fault("the store refused a " + s.name + " record");
        return;
      }
      case Stmt::K::Delete: {
        Value id_val = s.expr ? eval(*s.expr) : Value::null();
        std::string target_id = id_val.text();
        I.list_cache.erase(s.name);
        I.list_memo.erase(s.name);
        if (host.store_query) {
          host.store_query("DELETE FROM " + s.name + " WHERE _id = '" + target_id + "' OR id = '" + target_id + "'");
        }
        return;
      }
      case Stmt::K::Update: {
        Value rec_val = s.expr ? eval(*s.expr) : Value::null();
        std::string target_id = s.args.empty() ? "" : eval(*s.args[0]).text();
        if (rec_val.k == Value::K::Rec) {
          if (!target_id.empty()) rec_val.set_field("_id", Value::str(target_id));
          if (host.store_put) host.store_put(s.name, to_json(rec_val));
        }
        I.list_cache.erase(s.name);
        I.list_memo.erase(s.name);
        return;
      }
      case Stmt::K::Clear: {
        if (fields.count(s.name)) fields[s.name].clear();
        else if (I.editors.count(s.name)) {
          auto& ed = I.editors[s.name];
          ed.lines = {""};
          ed.cursor_line = 0;
          ed.cursor_col = 0;
          ed.scroll_line = 0;
          ed.scroll_col = 0;
          ed.dirty = true;
        }
        else if (s.name == "paint") paint.clear();
        else fault("clear: no field or editor is called " + s.name);
        return;
      }
      case Stmt::K::Focus: {
        if (fields.count(s.name) || I.editors.count(s.name)) focus = s.name;
        else if (s.name.empty() || s.name == "none") focus.clear();
        else fault("focus: no field or editor is called " + s.name);
        return;
      }
      case Stmt::K::Say: {
        message = s.expr ? eval(*s.expr).text() : "";
        if (host.say) host.say(message);
        return;
      }
      case Stmt::K::Stop: stop = true; return;
      case Stmt::K::If: {
        const bool go = s.expr ? eval(*s.expr).truthy() : true;
        for (const auto& b : go ? s.body : s.otherwise) { exec(*b, paint); if (stop) return; }
        return;
      }
      case Stmt::K::For: {
        Value l = s.expr ? eval(*s.expr) : Value::null();
        if (l.k != Value::K::List) return;
        auto saved = I.scope;
        size_t n = 0;
        for (const auto& item : *l.list) {
          if (n++ > 20000) break;
          I.scope[s.name] = item;
          I.scope["index"] = Value::num((double)(n - 1));
          for (const auto& b : s.body) { exec(*b, paint); if (stop) break; }
          if (stop) break;
        }
        I.scope = saved;
        return;
      }
      case Stmt::K::Emit: {
        Value val = s.expr ? eval(*s.expr) : Value::null();
        std::string chan_name = s.target.empty() ? "default" : s.target;
        auto ch = I.get_channel(chan_name);
        ch->emit(val);
        return;
      }
      case Stmt::K::Spawn: {
        auto body_copy = s.body;
        auto* impl_ptr = &I;
        aios::hal::Thread([body_copy, impl_ptr]() {
          std::vector<PaintCmd> dummy_paint;
          std::vector<std::string> dummy_faults;
          std::string dummy_msg;
          std::string dummy_focus;
          bool dummy_dirty = false;
          HostBindings empty_host;
          Ev task_ev{*impl_ptr, empty_host, dummy_faults, dummy_msg, dummy_focus, impl_ptr->fields, dummy_dirty};
          for (const auto& stmt : body_copy) {
            task_ev.exec(*stmt, dummy_paint);
            if (task_ev.stop) break;
          }
        }).detach();
        return;
      }
      case Stmt::K::Undo: {
        if (!I.undo_stack.empty()) {
          StateSnapshot curr;
          curr.state = I.state;
          curr.fields = fields;
          curr.timestamp = I.now_ms;
          I.redo_stack.push_back(curr);
          StateSnapshot snap = I.undo_stack.back();
          I.undo_stack.pop_back();
          I.state = snap.state;
          fields = snap.fields;
          dirty = true;
        }
        return;
      }
      case Stmt::K::Redo: {
        if (!I.redo_stack.empty()) {
          StateSnapshot curr;
          curr.state = I.state;
          curr.fields = fields;
          curr.timestamp = I.now_ms;
          I.undo_stack.push_back(curr);
          StateSnapshot snap = I.redo_stack.back();
          I.redo_stack.pop_back();
          I.state = snap.state;
          fields = snap.fields;
          dirty = true;
        }
        return;
      }
      case Stmt::K::Snapshot: {
        StateSnapshot snap;
        snap.label = s.expr ? eval(*s.expr).text() : "";
        snap.timestamp = I.now_ms;
        snap.state = I.state;
        snap.fields = fields;
        if (I.undo_stack.size() >= I.max_history) I.undo_stack.erase(I.undo_stack.begin());
        I.undo_stack.push_back(snap);
        I.redo_stack.clear();
        return;
      }
      case Stmt::K::Call: { if (s.expr) (void)eval(*s.expr); return; }
      case Stmt::K::Paint: {
        std::vector<Value> a;
        for (const auto& e : s.args) a.push_back(word_or_value(*e));
        auto num = [&](size_t i) { return i < a.size() ? a[i].number() : 0.0; };
        auto col = [&](size_t i, uint32_t def) { return i < a.size() ? color_of(a[i].text(), def) : def; };
        PaintCmd c;
        if (s.name == "clear") { c.k = PaintCmd::K::Clear; c.color = col(0, kBg); paint.clear(); paint.push_back(c); return; }
        if (paint.size() > 20000) return;

        // ---- the transform verbs ------------------------------------------
        // These record nothing; they change where the next shapes land.
        auto top = [&]() -> std::array<double, 6> {
          return I.xform.empty() ? std::array<double, 6>{1, 0, 0, 1, 0, 0} : I.xform.back();
        };
        auto mul = [](const std::array<double, 6>& m, const std::array<double, 6>& n) {
          // n applied first, then m.
          return std::array<double, 6>{
              m[0] * n[0] + m[2] * n[1], m[1] * n[0] + m[3] * n[1],
              m[0] * n[2] + m[2] * n[3], m[1] * n[2] + m[3] * n[3],
              m[0] * n[4] + m[2] * n[5] + m[4], m[1] * n[4] + m[3] * n[5] + m[5]};
        };
        if (s.name == "push") { I.xform.push_back(top()); return; }
        if (s.name == "pop") { if (!I.xform.empty()) I.xform.pop_back(); return; }
        if (s.name == "reset") { I.xform.clear(); return; }
        if (s.name == "translate" || s.name == "move") {
          const std::array<double, 6> t{1, 0, 0, 1, num(0), num(1)};
          if (I.xform.empty()) I.xform.push_back(t); else I.xform.back() = mul(I.xform.back(), t);
          return;
        }
        if (s.name == "rotate" || s.name == "turn") {
          // Degrees, about a point — a wing turns about its shoulder, not about
          // the corner of the pane.
          const double r = num(0) * 3.14159265358979 / 180.0;
          const double cx = a.size() > 1 ? num(1) : 0.0, cy = a.size() > 2 ? num(2) : 0.0;
          const double cs = std::cos(r), sn = std::sin(r);
          const std::array<double, 6> t{cs, sn, -sn, cs, cx - cs * cx + sn * cy, cy - sn * cx - cs * cy};
          if (I.xform.empty()) I.xform.push_back(t); else I.xform.back() = mul(I.xform.back(), t);
          return;
        }
        if (s.name == "scale") {
          const double sx = num(0), sy = a.size() > 1 && a[1].k == Value::K::Num ? num(1) : num(0);
          const double cx = a.size() > 2 ? num(2) : 0.0, cy = a.size() > 3 ? num(3) : 0.0;
          const std::array<double, 6> t{sx, 0, 0, sy, cx - sx * cx, cy - sy * cy};
          if (I.xform.empty()) I.xform.push_back(t); else I.xform.back() = mul(I.xform.back(), t);
          return;
        }
        if (s.name == "rect" || s.name == "box") { c.k = PaintCmd::K::Rect; c.a = num(0); c.b = num(1); c.c = num(2); c.d = num(3); c.color = col(4, kInk); }
        else if (s.name == "outline" || s.name == "frame") { c.k = PaintCmd::K::Outline; c.a = num(0); c.b = num(1); c.c = num(2); c.d = num(3); c.color = col(4, kInk); }
        else if (s.name == "circle" || s.name == "dot") { c.k = PaintCmd::K::Circle; c.a = num(0); c.b = num(1); c.c = num(2); c.color = col(3, kInk); }
        else if (s.name == "line") { c.k = PaintCmd::K::Line; c.a = num(0); c.b = num(1); c.c = num(2); c.d = num(3); c.color = col(4, kInk); c.e = a.size() > 5 ? num(5) : 1.0; }
        // Any silhouette: the points, then the colour. A wing, a beak, a leaf,
        // a roof — everything that is not a box or a ball.
        else if (s.name == "poly" || s.name == "shape") {
          c.k = PaintCmd::K::Poly;
          c.color = kInk;
          for (size_t i = 0; i < a.size(); ++i) {
            if (a[i].k == Value::K::Num) c.pts.push_back((float)a[i].number());
            else { c.color = color_of(a[i].text(), kInk); break; }
          }
          if (c.pts.size() < 6) { fault("paint poly: at least three x y points"); return; }
          if (c.pts.size() % 2) c.pts.pop_back();
        }
        else if (s.name == "ellipse" || s.name == "oval") { c.k = PaintCmd::K::Ellipse; c.a = num(0); c.b = num(1); c.c = num(2); c.d = num(3); c.color = col(4, kInk); }
        // A quadratic curve, stroked: start, the point it bends toward, end.
        else if (s.name == "curve" || s.name == "arc") {
          c.k = PaintCmd::K::Curve;
          c.pts = {(float)num(0), (float)num(1), (float)num(2), (float)num(3), (float)num(4), (float)num(5)};
          c.color = col(6, kInk);
          c.e = a.size() > 7 ? num(7) : 2.0;
        }
        else if (s.name == "grad" || s.name == "gradient") {
          c.k = PaintCmd::K::Grad; c.a = num(0); c.b = num(1); c.c = num(2); c.d = num(3);
          c.color = col(4, kBg); c.color2 = col(5, kInk);
          std::string dir = a.size() > 6 ? a[6].text() : std::string("v");
          for (auto& ch : dir) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
          c.e = (dir == "h" || dir == "across" || dir == "horizontal") ? 1.0 : 0.0;
        }
        else if (s.name == "triangle") {
          c.k = PaintCmd::K::Poly;
          c.pts = {(float)num(0), (float)num(1), (float)num(2), (float)num(3), (float)num(4), (float)num(5)};
          c.color = col(6, kInk);
        }
        else if (s.name == "round_rect" || s.name == "rounded") {
          double rx = num(0), ry = num(1), rw = num(2), rh = num(3), rad = std::max(1.0, num(4));
          rad = std::min(rad, std::min(rw / 2.0, rh / 2.0));
          c.k = PaintCmd::K::Poly;
          c.color = col(5, kInk);
          auto add_corner = [&](double cx, double cy, double start_a, double end_a) {
            for (int i = 0; i <= 6; ++i) {
              double a_rad = start_a + (end_a - start_a) * (double)i / 6.0;
              c.pts.push_back((float)(cx + std::cos(a_rad) * rad));
              c.pts.push_back((float)(cy + std::sin(a_rad) * rad));
            }
          };
          add_corner(rx + rw - rad, ry + rad, -1.5707963, 0.0);
          add_corner(rx + rw - rad, ry + rh - rad, 0.0, 1.5707963);
          add_corner(rx + rad, ry + rh - rad, 1.5707963, 3.14159265);
          add_corner(rx + rad, ry + rad, 3.14159265, 4.71238898);
        }
        else if (s.name == "arc") {
          double acx = num(0), acy = num(1), ar = num(2);
          double s_deg = num(3) * 3.14159265358979 / 180.0, e_deg = num(4) * 3.14159265358979 / 180.0;
          uint32_t arc_col = col(5, kInk);
          double aw = a.size() > 6 ? num(6) : 2.0;
          int steps = (int)std::clamp(std::fabs(e_deg - s_deg) * ar * 0.5, 6.0, 120.0);
          double prev_x = acx + std::cos(s_deg) * ar, prev_y = acy + std::sin(s_deg) * ar;
          for (int step = 1; step <= steps; ++step) {
            double angle = s_deg + (e_deg - s_deg) * (double)step / (double)steps;
            double cur_x = acx + std::cos(angle) * ar, cur_y = acy + std::sin(angle) * ar;
            PaintCmd sc;
            sc.k = PaintCmd::K::Line;
            sc.a = prev_x; sc.b = prev_y; sc.c = cur_x; sc.d = cur_y; sc.color = arc_col; sc.e = aw;
            paint.push_back(sc);
            prev_x = cur_x; prev_y = cur_y;
          }
          return;
        }
        else if (s.name == "star") {
          double scx = num(0), scy = num(1), r_out = num(2), r_in = num(3);
          int pts_count = a.size() > 4 ? (int)num(4) : 5;
          if (pts_count < 3) pts_count = 5;
          c.k = PaintCmd::K::Poly;
          c.color = col(5, kInk);
          int total_vertices = pts_count * 2;
          for (int i = 0; i < total_vertices; ++i) {
            double angle = (double)i * 3.14159265358979323846 / (double)pts_count - 1.5707963267948966;
            double r = (i % 2 == 0) ? r_out : r_in;
            c.pts.push_back((float)(scx + std::cos(angle) * r));
            c.pts.push_back((float)(scy + std::sin(angle) * r));
          }
        }
        else if (s.name == "ring" || s.name == "donut") {
          double rcx = num(0), rcy = num(1), r_in = num(2), r_out = num(3);
          c.k = PaintCmd::K::Poly;
          c.color = col(4, kInk);
          int steps = 32;
          for (int i = 0; i <= steps; ++i) {
            double angle = (double)i * 6.28318530717958647692 / (double)steps;
            c.pts.push_back((float)(rcx + std::cos(angle) * r_out));
            c.pts.push_back((float)(rcy + std::sin(angle) * r_out));
          }
          for (int i = steps; i >= 0; --i) {
            double angle = (double)i * 6.28318530717958647692 / (double)steps;
            c.pts.push_back((float)(rcx + std::cos(angle) * r_in));
            c.pts.push_back((float)(rcy + std::sin(angle) * r_in));
          }
        }
        else if (s.name == "pie" || s.name == "slice" || s.name == "wedge") {
          double pcx = num(0), pcy = num(1), pr = num(2);
          double s_deg = num(3) * 3.14159265358979323846 / 180.0, e_deg = num(4) * 3.14159265358979323846 / 180.0;
          c.k = PaintCmd::K::Poly;
          c.color = col(5, kInk);
          c.pts.push_back((float)pcx);
          c.pts.push_back((float)pcy);
          int steps = (int)std::clamp(std::fabs(e_deg - s_deg) * pr * 0.5, 6.0, 120.0);
          for (int i = 0; i <= steps; ++i) {
            double angle = s_deg + (e_deg - s_deg) * (double)i / (double)steps;
            c.pts.push_back((float)(pcx + std::cos(angle) * pr));
            c.pts.push_back((float)(pcy + std::sin(angle) * pr));
          }
        }
        else if (s.name == "text" || s.name == "label") { c.k = PaintCmd::K::Text; c.a = num(0); c.b = num(1); c.text = a.size() > 2 ? a[2].text() : ""; c.color = col(3, kInk); c.px = a.size() > 4 ? (int)num(4) : 15; if (c.px < 8) c.px = 8; if (c.px > 96) c.px = 96; }
        else if (s.name == "tensor" || s.name == "buffer") {
          c.k = PaintCmd::K::Tensor;
          Value tensor_val = a.size() > 0 ? a[0] : Value::null();
          c.a = a.size() > 1 ? num(1) : 0;
          c.b = a.size() > 2 ? num(2) : 0;
          c.c = a.size() > 3 ? num(3) : 200;
          c.d = a.size() > 4 ? num(4) : 100;
          if (tensor_val.k == Value::K::List && tensor_val.list) {
            for (const auto& item : *tensor_val.list) {
              if (item.k == Value::K::List && item.list) {
                for (const auto& sub : *item.list) c.tensor_data.push_back(sub.number());
                c.tensor_w = (int)item.list->size();
              } else {
                c.tensor_data.push_back(item.number());
              }
            }
            if (c.tensor_w <= 0) c.tensor_w = (int)c.tensor_data.size();
            c.tensor_h = c.tensor_w > 0 ? (int)(c.tensor_data.size() / c.tensor_w) : 1;
          }
          c.color = a.size() > 5 ? col(5, 0xFF38BDF8) : 0xFF38BDF8;
        }
        else { fault("paint " + s.name + ": the verbs are clear rect outline circle ellipse poly line curve grad text triangle round_rect arc star ring pie tensor buffer, and push pop translate rotate scale"); return; }

        for (const auto& arg_val : a) {
          if (arg_val.k == Value::K::Str) {
            const std::string& txt = arg_val.s;
            if (txt.rfind("hit=", 0) == 0) c.hit_id = txt.substr(4);
            if (txt.rfind("action=", 0) == 0) c.hit_action = txt.substr(7);
            if (txt.rfind("click=", 0) == 0) c.hit_action = txt.substr(6);
          }
        }

        // Through the transform, at the moment the command is recorded — the
        // painter never has to know there is one.
        const std::array<double, 6> m = top();
        const bool turned = std::fabs(m[1]) > 1e-9 || std::fabs(m[2]) > 1e-9;
        const bool plain = !turned && std::fabs(m[0] - 1.0) < 1e-9 && std::fabs(m[3] - 1.0) < 1e-9 &&
                           std::fabs(m[4]) < 1e-9 && std::fabs(m[5]) < 1e-9;
        if (!plain) {
          auto tx = [&](double x, double y) { return std::pair<double, double>{m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]}; };
          auto scale_of = [&] { return std::sqrt(std::fabs(m[0] * m[3] - m[1] * m[2])); };
          switch (c.k) {
            case PaintCmd::K::Poly:
            case PaintCmd::K::Curve:
              for (size_t i = 0; i + 1 < c.pts.size(); i += 2) {
                auto q = tx(c.pts[i], c.pts[i + 1]);
                c.pts[i] = (float)q.first; c.pts[i + 1] = (float)q.second;
              }
              if (c.k == PaintCmd::K::Curve) c.e *= scale_of();
              break;
            case PaintCmd::K::Line: {
              auto p0 = tx(c.a, c.b), p1 = tx(c.c, c.d);
              c.a = p0.first; c.b = p0.second; c.c = p1.first; c.d = p1.second;
              c.e *= scale_of();
              break;
            }
            case PaintCmd::K::Circle:
            case PaintCmd::K::Ellipse: {
              // A turned ellipse is a polygon: the ring, transformed. Upright,
              // it stays an ellipse and only its centre and radii move.
              const double rx = c.c, ry = c.k == PaintCmd::K::Circle ? c.c : c.d;
              if (turned) {
                const int steps = (int)std::clamp(std::max(rx, ry) * 1.6, 12.0, 220.0);
                c.pts.clear();
                for (int i = 0; i < steps; ++i) {
                  const double t = 6.283185307179586 * i / steps;
                  auto q = tx(c.a + std::cos(t) * rx, c.b + std::sin(t) * ry);
                  c.pts.push_back((float)q.first); c.pts.push_back((float)q.second);
                }
                c.k = PaintCmd::K::Poly;
              } else {
                auto q = tx(c.a, c.b);
                c.a = q.first; c.b = q.second;
                c.c = rx * std::fabs(m[0]);
                if (c.k == PaintCmd::K::Ellipse) c.d = ry * std::fabs(m[3]);
              }
              break;
            }
            case PaintCmd::K::Rect:
            case PaintCmd::K::Grad:
            case PaintCmd::K::Outline: {
              if (turned && c.k == PaintCmd::K::Rect) {
                const double x = c.a, y = c.b, w2 = c.c, h2 = c.d;
                c.pts.clear();
                for (const auto& pt : {std::pair<double, double>{x, y}, {x + w2, y}, {x + w2, y + h2}, {x, y + h2}}) {
                  auto q = tx(pt.first, pt.second);
                  c.pts.push_back((float)q.first); c.pts.push_back((float)q.second);
                }
                c.k = PaintCmd::K::Poly;
              } else {
                auto q = tx(c.a, c.b);
                c.a = q.first; c.b = q.second;
                c.c *= std::fabs(m[0]); c.d *= std::fabs(m[3]);
              }
              break;
            }
            case PaintCmd::K::Text: {
              // The font atlas has one orientation; the words move, they do not
              // lean. Said plainly rather than drawn wrong.
              auto q = tx(c.a, c.b);
              c.a = q.first; c.b = q.second;
              c.px = (int)std::clamp((double)c.px * scale_of(), 8.0, 96.0);
              break;
            }
            case PaintCmd::K::Tensor: {
              auto q = tx(c.a, c.b);
              c.a = q.first; c.b = q.second;
              c.c *= std::fabs(m[0]); c.d *= std::fabs(m[3]);
              break;
            }
            case PaintCmd::K::Clear: break;
          }
        }
        paint.push_back(c);
        return;
      }
    }
  }
};

}  // namespace

void Runtime::load() {
  if (loaded_) return;
  loaded_ = true;
  Impl& I = *impl_;
  Ev ev{I, host_, faults_, message_, focus_, I.fields, dirty_};

  // Persisted state first, then the initial values for anything not kept.
  Value kept;
  if (host_.storage_load) kept = from_json(host_.storage_load());
  for (const auto& st : face_.states) {
    const Value* have = kept.field(st.first);
    if (have) { I.state[st.first] = *have; continue; }
    I.state[st.first] = st.second ? ev.eval(*st.second) : Value::null();
  }
  I.now_ms = host_.now_ms ? host_.now_ms() : 0;
  for (const auto& l : face_.lets) I.lets[l.first] = l.second ? ev.eval(*l.second) : Value::null();

  auto attr_text = [&](const Widget& w, const std::string& key, const std::string& def = "") -> std::string {
    for (const auto& kv : w.exprs) if (kv.first == key && kv.second) return ev.eval(*kv.second).text();
    return w.attr(key, def);
  };

  // Fields and widgets, in document order, so Tab has an order to follow.
  std::vector<const Widget*> stack;
  for (auto it = face_.widgets.rbegin(); it != face_.widgets.rend(); ++it) stack.push_back(it->get());
  while (!stack.empty()) {
    const Widget* w = stack.back();
    stack.pop_back();
    if (w->type == "field" || w->type == "input") {
      const std::string id = w->attr("id", w->attr("name", ""));
      if (!id.empty()) {
        std::string init_val = attr_text(*w, "value", "");
        if (init_val.empty()) {
          auto st = I.state.find(id);
          if (st != I.state.end()) init_val = st->second.text();
        }
        I.fields[id] = init_val;
        I.field_order.push_back(id);
        if (w->flag("focus") && focus_.empty()) focus_ = id;
      }
    }
    if (w->type == "editor") {
      const std::string id = w->attr("id", w->attr("name", ""));
      if (!id.empty()) {
        I.editor_order.push_back(id);
        auto& ed = I.editors[id];
        ed.id = id;
        if (w->flag("focus") && focus_.empty()) focus_ = id;
        std::string p = attr_text(*w, "path", "");
        if (!p.empty() && host_.read_file) {
          ed.path = p;
          std::string content = host_.read_file(p);
          ed.lines.clear();
          std::string cur;
          for (char c : content) {
            if (c == '\r') continue;
            if (c == '\n') { ed.lines.push_back(cur); cur.clear(); }
            else cur += c;
          }
          ed.lines.push_back(cur);
          if (ed.lines.empty()) ed.lines.push_back("");
        } else {
          std::string val = attr_text(*w, "value", "");
          ed.lines.clear();
          std::string cur;
          for (char c : val) {
            if (c == '\r') continue;
            if (c == '\n') { ed.lines.push_back(cur); cur.clear(); }
            else cur += c;
          }
          ed.lines.push_back(cur);
          if (ed.lines.empty()) ed.lines.push_back("");
        }
        ed.initialized = true;
      }
    }
    for (auto it = w->children.rbegin(); it != w->children.rend(); ++it) stack.push_back(it->get());
  }

  for (const auto& h : face_.handlers) if (h.event == "load") for (const auto& s : h.body) ev.exec(*s, paint_);
}

namespace {

struct EditorToken {
  std::string text;
  uint32_t color = 0xFFE6ECF1;
};

static std::vector<EditorToken> tokenize_code_line(const std::string& line) {
  std::vector<EditorToken> tokens;
  if (line.empty()) return tokens;

  static const std::set<std::string> kKeywords = {
    "int", "float", "double", "char", "bool", "void", "auto", "const", "static", "constexpr",
    "if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue", "return",
    "class", "struct", "enum", "union", "namespace", "using", "typedef", "template", "typename",
    "public", "private", "protected", "virtual", "override", "inline", "explicit",
    "try", "catch", "throw", "new", "delete", "nullptr", "NULL", "true", "false",
    "function", "let", "var", "face", "title", "state", "kind", "on", "set", "put", "say", "clear",
    "focus", "stop", "button", "field", "tile", "table", "list", "panel", "row", "column", "grid",
    "editor", "import", "export", "from", "def", "self"
  };

  static const std::set<std::string> kTypes = {
    "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t", "int16_t", "int32_t", "int64_t",
    "size_t", "string", "vector", "map", "set", "pair", "shared_ptr", "unique_ptr",
    "Result", "Kernel", "Session", "Value", "Widget", "Runtime", "Face", "HostBindings", "EntityID"
  };

  constexpr uint32_t cKeyword = 0xFFFF7B72;  // Coral / salmon
  constexpr uint32_t cType = 0xFFD2A8FF;     // Lavender / purple
  constexpr uint32_t cString = 0xFF7EE787;   // Soft emerald green
  constexpr uint32_t cComment = 0xFF8B949E;  // Dim gray
  constexpr uint32_t cNumber = 0xFF79C0FF;   // Cyan
  constexpr uint32_t cPreproc = 0xFFFFA657;  // Amber / orange
  constexpr uint32_t cDefault = 0xFFE6ECF1;  // Ink

  size_t i = 0;
  size_t first_non_ws = line.find_first_not_of(" \t");
  if (first_non_ws != std::string::npos) {
    if (line[first_non_ws] == '#') {
      tokens.push_back({line, cPreproc});
      return tokens;
    }
    if (line.size() >= first_non_ws + 2 && line.substr(first_non_ws, 2) == "//") {
      tokens.push_back({line, cComment});
      return tokens;
    }
  }

  while (i < line.size()) {
    char c = line[i];
    if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') {
      tokens.push_back({line.substr(i), cComment});
      break;
    }
    if (c == '"' || c == '\'') {
      char quote = c;
      size_t start = i++;
      while (i < line.size() && line[i] != quote) {
        if (line[i] == '\\' && i + 1 < line.size()) i += 2;
        else ++i;
      }
      if (i < line.size()) ++i;
      tokens.push_back({line.substr(start, i - start), cString});
      continue;
    }
    if ((c >= '0' && c <= '9') || (c == '-' && i + 1 < line.size() && line[i + 1] >= '0' && line[i + 1] <= '9')) {
      size_t start = i++;
      if (c == '0' && i < line.size() && (line[i] == 'x' || line[i] == 'X')) {
        i++;
        while (i < line.size() && ((line[i] >= '0' && line[i] <= '9') || (line[i] >= 'a' && line[i] <= 'f') || (line[i] >= 'A' && line[i] <= 'F'))) ++i;
      } else {
        while (i < line.size() && ((line[i] >= '0' && line[i] <= '9') || line[i] == '.' || line[i] == 'f' || line[i] == 'u')) ++i;
      }
      tokens.push_back({line.substr(start, i - start), cNumber});
      continue;
    }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
      size_t start = i++;
      while (i < line.size() && ((line[i] >= 'a' && line[i] <= 'z') || (line[i] >= 'A' && line[i] <= 'Z') || (line[i] >= '0' && line[i] <= '9') || line[i] == '_')) ++i;
      std::string word = line.substr(start, i - start);
      if (kKeywords.count(word)) tokens.push_back({word, cKeyword});
      else if (kTypes.count(word)) tokens.push_back({word, cType});
      else if (i < line.size() && line[i] == '(') tokens.push_back({word, cType});
      else tokens.push_back({word, cDefault});
      continue;
    }
    if (c == ' ' || c == '\t') {
      size_t start = i++;
      while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
      tokens.push_back({line.substr(start, i - start), cDefault});
      continue;
    }
    tokens.push_back({std::string(1, c), 0xFF79C0FF});
    ++i;
  }
  return tokens;
}

struct Layout {
  Runtime::Impl& I;
  Ev& ev;
  const Face& face;
  std::vector<DrawOp>& ops;
  std::vector<Placed>& placed;
  std::vector<PaintCmd>& paint;
  const std::string& focus;
  int pane_w, pane_h;
  double blink_phase;

  int text_line(const std::string& s, int x, int y, int w, const web::FontKey& f, uint32_t color, const Widget* who, bool& cut) {
    std::string shown = fit(s, f, w, cut);
    DrawOp op;
    op.k = DrawOp::K::Text;
    op.x = x; op.y = y; op.text = shown; op.color = color; op.font = f;
    ops.push_back(op);
    Placed p;
    p.w = who; p.x = x; p.y = y; p.width = text_w(shown, f); p.height = line_h(f); p.text = s; p.cut = cut; p.px = f.px;
    placed.push_back(p);
    return line_h(f);
  }

  void editor_text_span(const std::string& s, int x, int y, const web::FontKey& f, uint32_t color, const Widget* who) {
    if (s.empty()) return;
    DrawOp op;
    op.k = DrawOp::K::Text;
    op.x = x; op.y = y; op.text = s; op.color = color; op.font = f;
    ops.push_back(op);
    Placed p;
    p.w = who; p.x = x; p.y = y; p.width = text_w(s, f); p.height = line_h(f); p.text = s; p.cut = false; p.px = f.px;
    placed.push_back(p);
  }

  // Wrap words into lines of `w`; a word that cannot fit is cut.
  int text_block(const std::string& s, int x, int y, int w, const web::FontKey& f, uint32_t color, const Widget* who, int align, int max_lines = 40) {
    std::vector<std::string> words;
    std::string cur;
    for (char c : s) {
      if (c == '\n') { if (!cur.empty()) words.push_back(cur); words.push_back("\n"); cur.clear(); continue; }
      if (c == ' ' || c == '\t') { if (!cur.empty()) words.push_back(cur); cur.clear(); continue; }
      cur += c;
    }
    if (!cur.empty()) words.push_back(cur);
    int cy = y;
    std::string line;
    int lines = 0;
    auto flush = [&]() {
      if (line.empty()) return;
      bool cut = false;
      int lw = text_w(line, f);
      int lx = x;
      if (align == 1) lx = x + std::max(0, (w - lw) / 2);
      else if (align == 2) lx = x + std::max(0, w - lw);
      text_line(line, lx, cy, w, f, color, who, cut);
      cy += line_h(f);
      ++lines;
      line.clear();
    };
    // Each word measured ONCE and the line's width carried along. It used to
    // build `line + " " + word` and measure the whole thing for every word, so
    // a paragraph of n words cost n²/2 glyph walks per frame.
    const int space_w = text_w(" ", f);
    int line_w = 0;
    for (const auto& word : words) {
      if (lines >= max_lines) break;
      if (word == "\n") { flush(); line_w = 0; continue; }
      const int word_w = text_w(word, f);
      const int cand_w = line.empty() ? word_w : line_w + space_w + word_w;
      if (cand_w <= w || line.empty()) {
        line = line.empty() ? word : line + " " + word;
        line_w = cand_w;
        continue;
      }
      flush();
      line = word;
      line_w = word_w;
    }
    if (lines < max_lines) flush();
    if (lines == 0 && words.empty()) return line_h(f);
    return cy - y;
  }

  void rect(int x, int y, int w, int h, uint32_t c) { DrawOp o; o.k = DrawOp::K::Rect; o.x = x; o.y = y; o.w = w; o.h = h; o.color = c; ops.push_back(o); }
  void round(int x, int y, int w, int h, int r, uint32_t c) { DrawOp o; o.k = DrawOp::K::Round; o.x = x; o.y = y; o.w = w; o.h = h; o.r = r; o.color = c; ops.push_back(o); }
  void outline(int x, int y, int w, int h, int r, uint32_t c) { DrawOp o; o.k = DrawOp::K::RoundOutline; o.x = x; o.y = y; o.w = w; o.h = h; o.r = r; o.color = c; ops.push_back(o); }

  std::string attr_text(const Widget& w, const std::string& key, const std::string& def = "") {
    for (const auto& kv : w.exprs) if (kv.first == key && kv.second) return ev.eval(*kv.second).text();
    return w.attr(key, def);
  }
  Value attr_value(const Widget& w, const std::string& key) {
    for (const auto& kv : w.exprs) if (kv.first == key && kv.second) return ev.eval(*kv.second);
    const std::string a = w.attr(key, "\x01");
    if (a == "\x01") return Value::null();
    return Value::str(a);
  }
  std::string label_of(const Widget& w) {
    for (const auto& kv : w.exprs) if (kv.first == "label" && kv.second) return ev.eval(*kv.second).text();
    return w.label;
  }
  uint32_t accent_of(const Widget& w, uint32_t def) {
    const std::string a = attr_text(w, "accent", attr_text(w, "color", ""));
    if (a.empty()) return def;
    return color_of(a, def);
  }
  int align_of(const Widget& w) { const std::string a = w.attr("align", "left"); return a == "center" ? 1 : a == "right" ? 2 : 0; }

  // Natural width of a widget in a row, or -1 when it should share the space.
  int natural_w(const Widget& w) {
    if (w.attr_int("width", 0) > 0) return w.attr_int("width", 0);
    if (w.flag("grow") || w.flag("fill")) return -1;
    if (w.type == "button") {
      const std::string label = label_of(w);
      return text_w(label, font_of(15, true)) + 36;
    }
    if (w.type == "image" || w.type == "img") {
      if (w.attr_int("width", 0) > 0) return w.attr_int("width", 0);
      std::string src = attr_text(w, "src", attr_text(w, "file", label_of(w)));
      if (!src.empty()) {
        auto it = I.image_cache.find(src);
        if (it != I.image_cache.end() && it->second) return it->second->w;
      }
      return w.flag("grow") || w.flag("fill") ? -1 : 120;
    }
    if (w.type == "badge" || w.type == "status") {
      std::string text = attr_text(w, "text", attr_text(w, "label", label_of(w)));
      return text_w(text, font_of(11, true)) + (w.flag("dot") || w.type == "status" ? 32 : 24);
    }
    if (w.type == "sparkline" || w.type == "chart") {
      return w.attr_int("width", -1);
    }
    if (w.type == "modal" || w.type == "dialog" || w.type == "overlay") {
      return w.attr_int("width", -1);
    }
    if (w.type == "scrubber" || w.type == "timeline") {
      return w.attr_int("width", -1);
    }
    if (w.type == "tile") {
      const std::string v = attr_text(w, "value", "");
      std::string l = label_of(w).empty() ? attr_text(w, "label", "") : label_of(w);
      for (auto& ch : l) if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);   // drawn in caps: measured in caps
      int tw = std::max(text_w(v, font_of(26, true)), text_w(l, font_of(11, true))) + 32;
      return std::max(w.attr_int("min", 130), tw);
    }
    if (w.type == "checkbox" || w.type == "check") {
      std::string lbl = label_of(w);
      if (lbl.empty()) lbl = attr_text(w, "label", "");
      return text_w(lbl, font_of(14, false)) + 36;
    }
    if (w.type == "switch" || w.type == "toggle") {
      std::string lbl = label_of(w);
      if (lbl.empty()) lbl = attr_text(w, "label", "");
      return text_w(lbl, font_of(14, false)) + 58;
    }
    if (w.type == "tabs" || w.type == "segmented") {
      return w.attr_int("width", -1);
    }
    if (w.type == "progress" || w.type == "bar") {
      return w.attr_int("width", -1);
    }
    if (w.type == "select" || w.type == "dropdown") {
      std::string lbl = label_of(w).empty() ? attr_text(w, "label", "") : label_of(w);
      return text_w(lbl + " [Option] v", font_of(14, false)) + 36;
    }
    if (w.type == "gauge") return -1;
    if (w.type == "spacer") return w.attr_int("size", -1);
    if (w.type == "text" || w.type == "label" || w.type == "heading") {
      if (w.flag("fit") || w.type == "heading" || w.type == "label") {
        const std::string t = label_of(w);
        return text_w(t, font_of(w.attr_int("size", w.type == "heading" ? 22 : 15), w.flag("bold") || w.type == "heading")) + 4;
      }
      return -1;
    }
    return -1;   // fields, panels, tables, lists, columns, grids share
  }

  int lay(const Widget& w, int x, int y, int avail_w, int avail_h, size_t depth = 0);

  int lay_children_column(const std::vector<std::shared_ptr<Widget>>& kids, int x, int y, int w, int avail_h, int gap, size_t depth) {
    int cy = y;
    for (size_t i = 0; i < kids.size(); ++i) {
      const int h = lay(*kids[i], x, cy, w, std::max(0, avail_h - (cy - y)), depth + 1);
      cy += h;
      if (i + 1 < kids.size()) cy += gap;
    }
    return cy - y;
  }

  int lay_row(const Widget& w, int x, int y, int avail_w, int avail_h, size_t depth) {
    const int gap = w.attr_int("gap", 10);
    std::vector<const Widget*> kids;
    for (const auto& k : w.children) kids.push_back(k.get());
    if (kids.empty()) return 0;
    std::vector<int> widths(kids.size(), 0);
    int rigid = 0, sharers = 0;
    std::vector<float> weight(kids.size(), 1.0f);
    for (size_t i = 0; i < kids.size(); ++i) {
      int nw = natural_w(*kids[i]);
      if (nw >= 0) { widths[i] = nw; rigid += nw; }
      else { ++sharers; const std::string fr = kids[i]->attr("fr", kids[i]->attr("grow", "1")); float f = (float)std::atof(fr.c_str()); weight[i] = f > 0 ? f : 1.0f; }
    }
    int room = avail_w - gap * (int)(kids.size() - 1);
    // What the sharing ones need at least: a slider, a bar or a select squeezed to a few pixels is no widget at all.
    // Without this, rigid neighbours that fit with 10 px to spare left a slider 10 px wide.
    int min_share = 0;
    std::vector<int> share_min(kids.size(), 0);
    for (size_t i = 0; i < kids.size(); ++i) {
      if (natural_w(*kids[i]) >= 0) continue;
      share_min[i] = std::max(0, kids[i]->attr_int("min_width", kids[i]->type == "slider" ? 140 : 80));
      min_share += share_min[i];
    }
    if (rigid + min_share > room) {
      // shrink the rigid ones in proportion, but never under 40
      int over = rigid + min_share - room;
      int shrinkable = 0;
      for (size_t i = 0; i < kids.size(); ++i) if (natural_w(*kids[i]) >= 0) shrinkable += std::max(0, widths[i] - 40);
      for (size_t i = 0; i < kids.size() && shrinkable > 0; ++i) {
        if (natural_w(*kids[i]) < 0) continue;
        int cut = (int)((long)std::max(0, widths[i] - 40) * std::min(over, shrinkable) / shrinkable);
        widths[i] -= cut;
      }
      rigid = 0;
      for (size_t i = 0; i < kids.size(); ++i) if (natural_w(*kids[i]) >= 0) rigid += widths[i];
    }
    int spare = std::max(0, room - rigid);
    float wsum = 0;
    for (size_t i = 0; i < kids.size(); ++i) if (natural_w(*kids[i]) < 0) wsum += weight[i];
    for (size_t i = 0; i < kids.size(); ++i) if (natural_w(*kids[i]) < 0) widths[i] = wsum > 0 ? (int)((float)spare * weight[i] / wsum) : spare / std::max(1, sharers);
    if (spare >= min_share)   // the room is there: no sharing one below its minimum (weights may have given it less)
      for (size_t i = 0; i < kids.size(); ++i)
        if (natural_w(*kids[i]) < 0 && widths[i] < share_min[i]) {
          int need = share_min[i] - widths[i];
          for (size_t j = 0; j < kids.size() && need > 0; ++j) {
            if (j == i || natural_w(*kids[j]) >= 0) continue;
            const int give = std::min(need, std::max(0, widths[j] - share_min[j]));
            widths[j] -= give;
            widths[i] += give;
            need -= give;
          }
        }
    // First pass: heights. Second: place with vertical alignment.
    int pen = x;
    int tallest = 0;
    std::vector<int> hs(kids.size(), 0);
    const size_t ops_before = ops.size(), placed_before = placed.size();
    std::vector<int> xs(kids.size(), 0);
    for (size_t i = 0; i < kids.size(); ++i) {
      xs[i] = pen;
      hs[i] = lay(*kids[i], pen, y, std::max(8, widths[i]), avail_h, depth + 1);
      tallest = std::max(tallest, hs[i]);
      pen += widths[i] + gap;
    }
    if (w.attr("align", "top") == "center" || w.attr("align", "top") == "middle" || w.attr("align", "") == "baseline") {
      // Re-lay with each child pushed down to centre it in the row.
      ops.resize(ops_before);
      placed.resize(placed_before);
      for (size_t i = 0; i < kids.size(); ++i) {
        const int dy = (tallest - hs[i]) / 2;
        lay(*kids[i], xs[i], y + dy, std::max(8, widths[i]), avail_h, depth + 1);
      }
    }
    return tallest;
  }

  int lay_grid(const Widget& w, int x, int y, int avail_w, int avail_h, size_t depth) {
    const int gap = w.attr_int("gap", 12);
    // tracks: bare words like "2fr 1fr 120", or a single count "3"
    std::vector<std::pair<float, int>> tracks;   // fr, px
    for (const auto& word : w.words) {
      if (word.find("fr") != std::string::npos) tracks.emplace_back((float)std::atof(word.c_str()), 0);
      else if (isdigit((unsigned char)word[0]) && word.find("px") == std::string::npos && w.words.size() == 1) {
        int n = std::atoi(word.c_str());
        for (int i = 0; i < std::max(1, std::min(n, 12)); ++i) tracks.emplace_back(1.0f, 0);
      } else if (isdigit((unsigned char)word[0])) tracks.emplace_back(0.0f, std::atoi(word.c_str()));
    }
    const std::string cols = w.attr("columns", w.attr("cols", ""));
    if (!cols.empty() && tracks.empty()) { int n = std::atoi(cols.c_str()); for (int i = 0; i < std::max(1, std::min(n, 12)); ++i) tracks.emplace_back(1.0f, 0); }
    if (tracks.empty()) { tracks.emplace_back(1.0f, 0); tracks.emplace_back(1.0f, 0); }
    int fixed = 0; float frs = 0;
    for (auto& t : tracks) { fixed += t.second; frs += t.first; }
    int room = avail_w - gap * (int)(tracks.size() - 1) - fixed;
    if (room < 0) room = 0;
    std::vector<int> widths;
    for (auto& t : tracks) widths.push_back(t.second > 0 ? t.second : (frs > 0 ? (int)((float)room * t.first / frs) : room / (int)tracks.size()));
    int cy = y;
    size_t i = 0;
    while (i < w.children.size()) {
      int pen = x, tallest = 0;
      for (size_t c = 0; c < widths.size() && i < w.children.size(); ++c, ++i) {
        int h = lay(*w.children[i], pen, cy, std::max(8, widths[c]), avail_h, depth + 1);
        tallest = std::max(tallest, h);
        pen += widths[c] + gap;
      }
      cy += tallest;
      if (i < w.children.size()) cy += gap;
    }
    return cy - y;
  }

  int lay_table(const Widget& w, int x, int y, int avail_w, int avail_h, size_t depth) {
    (void)depth;
    // the rows
    Value rows = Value::make_list();
    for (const auto& kv : w.exprs) if (kv.first == "rows_expr" && kv.second) rows = ev.eval(*kv.second);
    if (!w.words.empty()) { std::string err; ExprP e = parse_expr(w.words[0], err); if (err.empty() && e) rows = ev.eval(*e); else if (!err.empty()) ev.fault("table: " + err); }
    if (rows.k != Value::K::List) rows = Value::make_list();
    // the columns: "key:Header:right:w=120, key2:Header2"
    struct Col { std::string key, header; bool right = false; int fixed = 0; int w = 0; };
    std::vector<Col> cols;
    std::string spec = w.attr("columns", w.attr("cols", ""));
    if (spec.empty()) {
      // every field of the first record
      if (!rows.list->empty() && (*rows.list)[0].k == Value::K::Rec)
        for (const auto& kv : *(*rows.list)[0].rec) if (kv.first.empty() || kv.first[0] != '_') cols.push_back(Col{kv.first, kv.first, false, 0, 0});
    } else {
      std::string cur;
      auto flush = [&]() {
        std::string t = trim(cur);
        cur.clear();
        if (t.empty()) return;
        Col c;
        std::vector<std::string> parts;
        std::string p;
        for (char ch : t) { if (ch == ':') { parts.push_back(p); p.clear(); } else p += ch; }
        parts.push_back(p);
        c.key = trim(parts[0]);
        c.header = parts.size() > 1 ? trim(parts[1]) : c.key;
        for (size_t k = 2; k < parts.size(); ++k) {
          std::string o = trim(parts[k]);
          if (o == "right" || o == "num" || o == "number") c.right = true;
          else if (o.rfind("w=", 0) == 0) c.fixed = std::atoi(o.c_str() + 2);
          else if (isdigit((unsigned char)o[0])) c.fixed = std::atoi(o.c_str());
        }
        cols.push_back(c);
      };
      for (char ch : spec) { if (ch == ',' || ch == '|') flush(); else cur += ch; }
      flush();
    }
    const int max_rows = w.attr_int("rows", 12);
    const web::FontKey hf = font_of(11, true);
    const web::FontKey rf = font_of(w.attr_int("size", 15), false, w.flag("mono"));
    const int pad = 10;
    const int row_h = line_h(rf) + 12;
    const int head_h = line_h(hf) + 10;
    if (cols.empty()) {
      bool cut;
      return text_line(attr_text(w, "empty", "nothing here yet"), x + pad, y + 6, avail_w - 2 * pad, font_of(14), kDim, &w, cut) + 12;
    }
    // widths from content
    const size_t shown = std::min(rows.list->size(), (size_t)max_rows);
    std::vector<std::vector<std::string>> cells(shown);
    for (size_t r = 0; r < shown; ++r) {
      const Value& rec = (*rows.list)[r];
      for (const auto& c : cols) {
        const Value* f = rec.field(c.key);
        std::string t;
        if (f) t = f->k == Value::K::Num && c.right ? fmt_num(f->n) : f->text();
        else if (c.key == "_when" || c.key == "when") { const Value* ms = rec.field("_t"); if (ms) { std::vector<Value> a{*ms}; t = ev.call("time", a).text(); } }
        else if (c.key == "_ago" || c.key == "ago") { const Value* ms = rec.field("_t"); if (ms) { std::vector<Value> a{*ms}; t = ev.call("ago", a).text(); } }
        cells[r].push_back(t);
      }
    }
    // A column that is empty in EVERY row named a field these records do not
    // carry. The table draws it perfectly — right width, right header, every
    // cell blank — and nothing was cut and nothing ran off the pane, so every
    // other check passes while a person looks at 124 rows of nothing. It is the
    // records half of a face that renders beautifully and says nothing.
    if (shown > 0) {
      for (size_t c = 0; c < cols.size(); ++c) {
        if (cols[c].key.empty() || cols[c].key[0] == '_') continue;
        bool any_value = false;
        for (size_t r = 0; r < shown && !any_value; ++r) if (!cells[r][c].empty()) any_value = true;
        if (!any_value)
          ev.fault("the column '" + cols[c].header + "' is empty in all " + std::to_string(shown) +
                " row(s): these records carry no field '" + cols[c].key + "'. Read the fields the kind actually has.");
      }
    }
    int total = 0;
    for (size_t c = 0; c < cols.size(); ++c) {
      int wmax = text_w(cols[c].header, hf);
      for (size_t r = 0; r < shown; ++r) wmax = std::max(wmax, text_w(cells[r][c], rf));
      cols[c].w = cols[c].fixed > 0 ? cols[c].fixed : std::max(48, wmax + 2 * pad);
      total += cols[c].w;
    }
    if (total > avail_w) {
      // shrink the flexible columns in proportion above 48px
      int over = total - avail_w, shrinkable = 0;
      for (auto& c : cols) if (c.fixed == 0) shrinkable += std::max(0, c.w - 48);
      for (auto& c : cols) if (c.fixed == 0 && shrinkable > 0) c.w -= (int)((long)std::max(0, c.w - 48) * std::min(over, shrinkable) / shrinkable);
    } else if (total < avail_w) {
      int extra = avail_w - total;
      int flexible = 0;
      for (auto& c : cols) if (c.fixed == 0) ++flexible;
      for (auto& c : cols) if (c.fixed == 0) c.w += extra / std::max(1, flexible);
    }
    // header
    int cy = y;
    int cx = x;
    for (const auto& c : cols) {
      bool cut;
      std::string h = c.header;
      for (auto& ch : h) if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
      const int tw = text_w(h, hf);
      int tx = c.right ? cx + c.w - pad - std::min(tw, c.w - 2 * pad) : cx + pad;
      text_line(h, tx, cy + 5, c.w - 2 * pad, hf, kDim, &w, cut);
      cx += c.w;
    }
    cy += head_h;
    rect(x, cy - 1, avail_w, 1, kPanelEdge);
    // rows
    for (size_t r = 0; r < shown; ++r) {
      if (r % 2 == 1) rect(x, cy, avail_w, row_h, kZebra);
      cx = x;
      for (size_t c = 0; c < cols.size(); ++c) {
        bool cut;
        std::string shown_text = fit(cells[r][c], rf, cols[c].w - 2 * pad, cut);
        const int tw = text_w(shown_text, rf);
        int tx = cols[c].right ? cx + cols[c].w - pad - tw : cx + pad;
        text_line(cells[r][c], tx, cy + 6, cols[c].w - 2 * pad, rf, cols[c].right ? kInk : (c == 0 ? kInk : 0xFFC3CDD6), &w, cut);
        cx += cols[c].w;
      }
      cy += row_h;
    }
    if (rows.list->empty()) {
      bool cut;
      cy += text_line(attr_text(w, "empty", "nothing here yet"), x + pad, cy + 6, avail_w - 2 * pad, font_of(14), kDim, &w, cut) + 12;
    } else if (rows.list->size() > shown) {
      bool cut;
      cy += text_line("+ " + std::to_string(rows.list->size() - shown) + " more", x + pad, cy + 4, avail_w - 2 * pad, font_of(12), kDim, &w, cut) + 6;
    }
    (void)avail_h;
    return cy - y;
  }

  int lay_image(const Widget& w, int x, int y, int avail_w, int avail_h, size_t depth) {
    (void)depth; (void)avail_h;
    std::string src = attr_text(w, "src", attr_text(w, "file", label_of(w)));
    if (src.empty()) src = w.attr("src", w.attr("file", w.label));
    std::shared_ptr<web::Bitmap> bm;
    if (!src.empty()) {
      auto it = I.image_cache.find(src);
      if (it != I.image_cache.end()) {
        bm = it->second;
      } else if (ev.host.read_file) {
        std::string content = ev.host.read_file(src);
        if (!content.empty()) {
          auto dec = web::decode_image(span_of(content));
          if (dec.ok()) {
            bm = std::make_shared<web::Bitmap>(std::move(*dec));
            I.image_cache[src] = bm;
          }
        }
      }
    }
    int img_w = w.attr_int("width", bm ? bm->w : 100);
    int img_h = w.attr_int("height", bm ? bm->h : 100);
    if (w.flag("fill") || w.flag("grow")) img_w = avail_w;
    if (img_w > avail_w && avail_w > 0) {
      if (img_w > 0) img_h = (int)((int64_t)img_h * avail_w / img_w);
      img_w = avail_w;
    }
    if (img_w <= 0) img_w = 40;
    if (img_h <= 0) img_h = 40;

    DrawOp op;
    op.k = DrawOp::K::BitmapImage;
    op.x = x; op.y = y; op.w = img_w; op.h = img_h;
    op.bitmap = bm;
    ops.push_back(op);

    Placed p;
    p.w = &w; p.x = x; p.y = y; p.width = img_w; p.height = img_h; p.box = true;
    placed.push_back(p);
    return img_h;
  }
};

int Layout::lay(const Widget& w, int x, int y, int avail_w, int avail_h, size_t depth) {
  if (depth > 40 || avail_w < 8) return 0;
  const std::string& t = w.type;
  if (w.attr("if", "").size()) {
    std::string err; ExprP e = parse_expr(w.attr("if"), err);
    if (err.empty() && e && !ev.eval(*e).truthy()) return 0;
  }
  // Check if `t` is a user-defined component
  const ComponentDef* comp = nullptr;
  for (const auto& c : face.components) {
    if (c.name == t) { comp = &c; break; }
  }
  if (comp) {
    auto saved_scope = I.scope;
    size_t pos_idx = 0;
    for (size_t pi = 0; pi < comp->params.size(); ++pi) {
      const auto& pdef = comp->params[pi];
      const std::string& pname = pdef.first;
      bool bound = false;
      for (const auto& kv : w.exprs) {
        if (kv.first == pname && kv.second) {
          I.scope[pname] = ev.eval(*kv.second);
          bound = true;
          break;
        }
      }
      if (!bound) {
        std::string attr_val = w.attr(pname, "");
        if (!attr_val.empty()) {
          I.scope[pname] = Value::str(attr_val);
          bound = true;
        }
      }
      if (!bound) {
        if (pos_idx == 0 && !w.label.empty()) {
          I.scope[pname] = Value::str(w.label);
          pos_idx++;
          bound = true;
        } else {
          size_t word_idx = (w.label.empty() ? pos_idx : pos_idx - 1);
          if (word_idx < w.words.size()) {
            std::string wtoken = w.words[word_idx];
            std::string err;
            ExprP we = parse_expr(wtoken, err);
            if (err.empty() && we) I.scope[pname] = ev.eval(*we);
            else I.scope[pname] = Value::str(wtoken);
            pos_idx++;
            bound = true;
          }
        }
      }
      if (!bound && pdef.second) {
        I.scope[pname] = ev.eval(*pdef.second);
      }
    }
    int cy = y;
    for (size_t i = 0; i < comp->body.size(); ++i) {
      const int h = lay(*comp->body[i], x, cy, avail_w, std::max(0, avail_h - (cy - y)), depth + 1);
      cy += h;
      if (i + 1 < comp->body.size()) cy += 8;
    }
    I.scope = saved_scope;
    return cy - y;
  }

  if (t == "row") return lay_row(w, x, y, avail_w, avail_h, depth);
  if (t == "column" || t == "col") return lay_children_column(w.children, x, y, avail_w, avail_h, w.attr_int("gap", 10), depth);
  if (t == "grid") return lay_grid(w, x, y, avail_w, avail_h, depth);
  if (t == "image" || t == "img") return lay_image(w, x, y, avail_w, avail_h, depth);
  if (t == "scroll" || (t == "panel" && w.flag("scroll"))) {
    const int max_h = w.attr_int("max_height", 200);
    const size_t ops_before = ops.size();
    const size_t placed_before = placed.size();
    int kids_h = lay_children_column(w.children, x, y, avail_w, 100000, w.attr_int("gap", 10), depth);
    int h = !w.attr("height").empty() ? w.attr_int("height", max_h) : std::min(kids_h, max_h);

    // Filter/clip child ops and placed items that exceed the scroll viewport bounds [y, y + h]
    std::vector<Placed> kept_placed;
    for (size_t i = placed_before; i < placed.size(); ++i) {
      auto p = placed[i];
      if (p.y >= y + h) continue;
      if (!p.text.empty() && p.y + 12 > y + h) continue;
      if (p.box && p.y + p.height > y + h) p.height = std::max(0, y + h - p.y);
      kept_placed.push_back(p);
    }
    placed.resize(placed_before);
    placed.insert(placed.end(), kept_placed.begin(), kept_placed.end());

    std::vector<DrawOp> kept_ops;
    for (size_t i = ops_before; i < ops.size(); ++i) {
      auto op = ops[i];
      if (op.y >= y + h) continue;
      if (op.k == DrawOp::K::Text) {
        if (op.y + line_h(op.font) > y + h) continue;
      } else {
        if (op.y + op.h > y + h) op.h = std::max(0, y + h - op.y);
      }
      kept_ops.push_back(op);
    }
    ops.resize(ops_before);
    ops.insert(ops.end(), kept_ops.begin(), kept_ops.end());

    Placed p; p.w = &w; p.x = x; p.y = y; p.width = avail_w; p.height = h; p.box = true;
    placed.push_back(p);
    return h;
  }
  if (t == "badge" || t == "status") {
    std::string text = attr_text(w, "text", attr_text(w, "label", label_of(w)));
    if (text.empty() && t == "status") {
      if (w.flag("ok")) text = "OK";
      else if (w.flag("warn")) text = "WARN";
      else if (w.flag("err") || w.flag("bad")) text = "ERR";
      else text = "ACTIVE";
    }
    std::string acc_str = attr_text(w, "accent", attr_text(w, "color", ""));
    uint32_t acc_col = kAccent;
    if (w.flag("ok") || acc_str == "ok") acc_col = kOk;
    else if (w.flag("warn") || acc_str == "warn") acc_col = kWarn;
    else if (w.flag("bad") || w.flag("err") || acc_str == "bad" || acc_str == "err") acc_col = kBad;
    else if (!acc_str.empty()) acc_col = color_of(acc_str, kAccent);

    const int h = w.attr_int("height", 24);
    const web::FontKey f = font_of(11, true);
    const int tw = text_w(text, f);
    const bool has_dot = w.flag("dot") || t == "status";
    const int bw = std::max(w.attr_int("width", 0), tw + (has_dot ? 32 : 20));
    const int draw_w = (avail_w > 0 && !w.flag("grow") && !w.flag("fill")) ? std::min(avail_w, bw) : (avail_w > 0 ? avail_w : bw);

    // Pill background with tint and outline
    uint32_t tint_bg = (acc_col & 0x00FFFFFF) | 0x26000000;
    round(x, y, draw_w, h, h / 2, tint_bg);
    outline(x, y, draw_w, h, h / 2, acc_col);

    int text_x = x + (has_dot ? 22 : (draw_w - tw) / 2);
    if (has_dot) {
      round(x + 8, y + (h - 6) / 2, 6, 6, 3, acc_col);
    }
    bool cut;
    text_line(text, text_x, y + (h - line_h(f)) / 2, draw_w - (has_dot ? 26 : 8), f, kInk, &w, cut);
    { Placed p; p.w = &w; p.x = x; p.y = y; p.width = draw_w; p.height = h; p.box = true; placed.push_back(p); }
    return h;
  }
  if (t == "sparkline" || t == "chart") {
    const int h = w.attr_int("height", 36);
    Value data_val = attr_value(w, "data");
    if (data_val.k == Value::K::Null) data_val = attr_value(w, "values");
    std::vector<double> points;
    if (data_val.k == Value::K::List && data_val.list) {
      for (const auto& item : *data_val.list) points.push_back(item.number());
    } else if (data_val.k == Value::K::Str && !data_val.s.empty()) {
      std::string s = data_val.s;
      size_t pos = 0;
      while (pos < s.size()) {
        size_t comma = s.find(',', pos);
        std::string token = (comma == std::string::npos) ? s.substr(pos) : s.substr(pos, comma - pos);
        while (!token.empty() && token.front() == ' ') token.erase(token.begin());
        while (!token.empty() && token.back() == ' ') token.pop_back();
        if (!token.empty()) points.push_back(std::atof(token.c_str()));
        if (comma == std::string::npos) break;
        pos = comma + 1;
      }
    }
    
    round(x, y, avail_w, h, 6, kPanel);
    outline(x, y, avail_w, h, 6, kPanelEdge);
    { Placed p; p.w = &w; p.x = x; p.y = y; p.width = avail_w; p.height = h; p.box = true; placed.push_back(p); }

    if (points.size() >= 2 && avail_w > 16 && h > 10) {
      double min_v = points[0], max_v = points[0];
      for (double v : points) {
        if (v < min_v) min_v = v;
        if (v > max_v) max_v = v;
      }
      if (w.attr("min", "") != "") min_v = attr_value(w, "min").number();
      if (w.attr("max", "") != "") max_v = attr_value(w, "max").number();
      double range = max_v - min_v;
      if (range <= 0.00001) range = 1.0;

      uint32_t color = accent_of(w, kAccent);
      const bool big = t == "chart";
      int inner_x = x + 8;
      int inner_w = avail_w - 16;
      int inner_y = y + 6;
      int inner_h = h - 12;
      if (big) {
        // A chart: its range at the left, as a browser shows it, and the area under the line filled lightly.
        const std::string prefix = attr_text(w, "prefix", "");
        auto label = [&](double v) {
          if (std::fabs(v - std::round(v)) < 1e-9 && std::fabs(v) < 1e15) {
            std::string d = std::to_string((long long)std::llround(std::fabs(v))), g;
            for (size_t i = 0; i < d.size(); ++i) { if (i && (d.size() - i) % 3 == 0) g += ','; g += d[i]; }
            return (v < 0 ? "-" : "") + prefix + g;
          }
          return prefix + fmt_num(v);
        };
        const web::FontKey lf = font_of(10, false);
        const std::string top = label(max_v), bottom = label(min_v);
        const int lw = std::max(text_w(top, lf), text_w(bottom, lf)) + 12;
        bool cut;
        text_line(top, x + 8, y + 6, lw, lf, kDim, &w, cut);
        text_line(bottom, x + 8, y + h - 6 - line_h(lf), lw, lf, kDim, &w, cut);
        inner_x = x + 8 + lw;
        inner_w = avail_w - 16 - lw;
        inner_y = y + 10;
        inner_h = h - 20;
        // the fill: the accent laid over the panel at 14%, one column at a time from the line down
        auto mix = [](uint32_t a, uint32_t b, double f) {
          auto ch = [&](int sh) { return (uint32_t)(((a >> sh) & 0xFF) * (1 - f) + ((b >> sh) & 0xFF) * f) & 0xFF; };
          return 0xFF000000u | (ch(16) << 16) | (ch(8) << 8) | ch(0);
        };
        const uint32_t fill = mix(kPanel, color, 0.14);
        if (inner_w > 2) {
          for (int cx = 0; cx <= inner_w; ++cx) {
            const double pos = (double)cx * (points.size() - 1) / inner_w;
            const size_t i0 = std::min(points.size() - 2, (size_t)pos);
            const double f = pos - (double)i0;
            const double v = points[i0] * (1 - f) + points[i0 + 1] * f;
            int py = inner_y + inner_h - (int)(((v - min_v) / range) * inner_h);
            py = std::clamp(py, inner_y, inner_y + inner_h);
            rect(inner_x + cx, py, 1, inner_y + inner_h - py, fill);
          }
        }
      }

      int prev_px = inner_x;
      int prev_py = inner_y + inner_h - (int)(((points[0] - min_v) / range) * inner_h);
      prev_py = std::clamp(prev_py, inner_y, inner_y + inner_h);

      for (size_t i = 1; i < points.size(); ++i) {
        int cur_px = inner_x + (int)((i * inner_w) / (points.size() - 1));
        int cur_py = inner_y + inner_h - (int)(((points[i] - min_v) / range) * inner_h);
        cur_py = std::clamp(cur_py, inner_y, inner_y + inner_h);

        int dx = cur_px - prev_px;
        int dy = cur_py - prev_py;
        int steps = std::max(std::abs(dx), std::abs(dy));
        if (steps > 0) {
          for (int s = 0; s <= steps; ++s) {
            int lx = prev_px + (dx * s) / steps;
            int ly = prev_py + (dy * s) / steps;
            rect(lx, ly, big ? 3 : 2, big ? 3 : 2, color);
          }
        }
        prev_px = cur_px;
        prev_py = cur_py;
      }
    }
    return h;
  }
  if (t == "scrubber" || t == "timeline") {
    const std::string id = w.attr("id", w.attr("name", ""));
    double cur_val = 0;
    Value v_val = attr_value(w, "value");
    if (v_val.k != Value::K::Null) cur_val = v_val.number();
    else if (!id.empty() && I.fields.count(id)) cur_val = std::atof(I.fields[id].c_str());
    else if (!id.empty() && I.state.count(id)) cur_val = I.state[id].number();
    else if (!id.empty() && I.lets.count(id)) cur_val = I.lets[id].number();

    const double min_v = attr_value(w, "min").k == Value::K::Null ? 0 : attr_value(w, "min").number();
    const double max_v = attr_value(w, "max").k == Value::K::Null ? 100 : attr_value(w, "max").number();
    double range = max_v - min_v;
    if (range <= 0.0001) range = 1.0;

    double frac = std::clamp((cur_val - min_v) / range, 0.0, 1.0);
    const int h = w.attr_int("height", 44);
    std::string label = label_of(w);
    if (label.empty()) label = attr_text(w, "label", "");
    std::string range_text = attr_text(w, "range", "");

    int cy = y;
    const web::FontKey f = font_of(12, true);
    bool cut;
    if (!label.empty() || !range_text.empty()) {
      std::string header_text = label.empty() ? range_text : (label + (range_text.empty() ? "" : " (" + range_text + ")"));
      header_text += " [Checkpoint #" + fmt_num(cur_val) + "]";
      cy += text_line(header_text, x, cy, avail_w, f, kDim, &w, cut) + 4;
    }

    const int track_y = cy + 6;
    const int track_h = 6;
    round(x, track_y, avail_w, track_h, 3, kField);
    outline(x, track_y, avail_w, track_h, 3, kFieldEdge);

    int active_w = (int)(avail_w * frac);
    if (active_w > 0) {
      round(x, track_y, std::min(avail_w, active_w), track_h, 3, kAccent);
    }

    int thumb_x = x + (int)(avail_w * frac);
    thumb_x = std::clamp(thumb_x, x + 6, x + avail_w - 6);
    round(thumb_x - 6, track_y - 5, 12, 16, 4, kInk);
    outline(thumb_x - 6, track_y - 5, 12, 16, 4, kAccent);

    { Placed p; p.w = &w; p.x = x; p.y = y; p.width = avail_w; p.height = h; p.box = true; placed.push_back(p); }
    return h;
  }
  if (t == "modal" || t == "dialog" || t == "overlay") {
    Value show_val = attr_value(w, "show");
    if (show_val.k == Value::K::Null) show_val = attr_value(w, "open");
    bool is_open = true;
    if (show_val.k != Value::K::Null) is_open = show_val.truthy();
    else if (w.flag("show") || w.flag("open")) is_open = true;
    else if (w.attr("show", "") == "false" || w.attr("open", "") == "false") is_open = false;

    if (!is_open) return 0;

    int mw = std::min(avail_w, w.attr_int("width", 480));
    int mx = x + (avail_w - mw) / 2;
    int my = y + 10;
    
    // Dim overlay backdrop
    rect(x - 16, y - 16, avail_w + 32, avail_h + 32, 0xB00B0F14);

    int modal_inner_pad = w.attr_int("pad", 16);
    int content_y = my + modal_inner_pad;

    const size_t bg_at = ops.size();
    round(mx, my, mw, 20, 8, kPanel);
    outline(mx, my, mw, 20, 8, 0xFF58A6FF);

    std::string title = attr_text(w, "title", label_of(w));
    if (!title.empty()) {
      bool cut;
      content_y += text_line(title, mx + modal_inner_pad, content_y, mw - 2 * modal_inner_pad, font_of(18, true), kInk, &w, cut) + 8;
      rect(mx + modal_inner_pad, content_y, mw - 2 * modal_inner_pad, 1, kPanelEdge);
      content_y += 10;
    }

    int kids_h = lay_children_column(w.children, mx + modal_inner_pad, content_y, mw - 2 * modal_inner_pad, std::max(0, avail_h - (content_y - y)), w.attr_int("gap", 10), depth + 1);
    int mh = (content_y - my) + kids_h + modal_inner_pad;
    ops[bg_at].h = mh;
    ops[bg_at + 1].h = mh;

    { Placed p; p.w = &w; p.x = mx; p.y = my; p.width = mw; p.height = mh; p.box = true; placed.push_back(p); }
    return mh + 20;
  }
  if (t == "checkbox" || t == "check") {
    const std::string id = w.attr("id", w.attr("name", ""));
    bool checked = false;
    Value val = attr_value(w, "value");
    if (val.k != Value::K::Null) checked = val.truthy();
    else if (!id.empty() && I.fields.count(id)) {
      const std::string& fs = I.fields[id];
      checked = (fs == "true" || fs == "1" || fs == "on" || fs == "yes");
    }
    else if (!id.empty() && I.state.count(id)) checked = I.state[id].truthy();
    else if (!id.empty() && I.lets.count(id)) checked = I.lets[id].truthy();
    else if (w.flag("checked")) checked = true;

    const int h = w.attr_int("height", 28);
    const int box_size = 18;
    const int box_y = y + (h - box_size) / 2;
    uint32_t acc = accent_of(w, kAccent);

    // Draw checkbox box
    round(x, box_y, box_size, box_size, 4, checked ? acc : kField);
    outline(x, box_y, box_size, box_size, 4, checked ? acc : kFieldEdge);

    if (checked) {
      // Draw checkmark inside box
      rect(x + 4, box_y + 8, 3, 5, 0xFF0B1620);
      rect(x + 7, box_y + 10, 3, 3, 0xFF0B1620);
      rect(x + 10, box_y + 5, 3, 8, 0xFF0B1620);
    }

    std::string lbl = label_of(w);
    if (lbl.empty()) lbl = attr_text(w, "label", "");
    const web::FontKey f = font_of(14, false);
    bool cut;
    text_line(lbl, x + box_size + 10, y + (h - line_h(f)) / 2, avail_w - box_size - 12, f, kInk, &w, cut);

    Placed p; p.w = &w; p.x = x; p.y = y; p.width = avail_w; p.height = h; p.box = true;
    placed.push_back(p);
    return h;
  }
  if (t == "switch" || t == "toggle") {
    const std::string id = w.attr("id", w.attr("name", ""));
    bool on = false;
    Value val = attr_value(w, "value");
    if (val.k != Value::K::Null) on = val.truthy();
    else if (!id.empty() && I.fields.count(id)) {
      const std::string& fs = I.fields[id];
      on = (fs == "true" || fs == "1" || fs == "on" || fs == "yes");
    }
    else if (!id.empty() && I.state.count(id)) on = I.state[id].truthy();
    else if (!id.empty() && I.lets.count(id)) on = I.lets[id].truthy();
    else if (w.flag("on") || w.flag("checked")) on = true;

    const int h = w.attr_int("height", 28);
    const int sw_w = 42, sw_h = 22;
    const int sw_y = y + (h - sw_h) / 2;
    uint32_t acc = accent_of(w, kAccent);

    // Draw pill track
    round(x, sw_y, sw_w, sw_h, sw_h / 2, on ? acc : kField);
    outline(x, sw_y, sw_w, sw_h, sw_h / 2, on ? acc : kFieldEdge);

    // Draw circular knob
    int knob_size = sw_h - 4;
    int knob_x = on ? (x + sw_w - knob_size - 2) : (x + 2);
    int knob_y = sw_y + 2;
    round(knob_x, knob_y, knob_size, knob_size, knob_size / 2, on ? 0xFF0B1620 : kInk);

    std::string lbl = label_of(w);
    if (lbl.empty()) lbl = attr_text(w, "label", "");
    const web::FontKey f = font_of(14, false);
    bool cut;
    text_line(lbl, x + sw_w + 10, y + (h - line_h(f)) / 2, avail_w - sw_w - 12, f, kInk, &w, cut);

    Placed p; p.w = &w; p.x = x; p.y = y; p.width = avail_w; p.height = h; p.box = true;
    placed.push_back(p);
    return h;
  }
  if (t == "tabs" || t == "segmented") {
    const std::string id = w.attr("id", w.attr("name", ""));
    std::string cur_val;
    Value v_val = attr_value(w, "value");
    if (v_val.k != Value::K::Null) cur_val = v_val.text();
    else if (!id.empty() && I.fields.count(id)) cur_val = I.fields[id];
    else if (!id.empty() && I.state.count(id)) cur_val = I.state[id].text();
    else if (!id.empty() && I.lets.count(id)) cur_val = I.lets[id].text();

    std::string opts_str = attr_text(w, "options", attr_text(w, "tabs", label_of(w)));
    std::vector<std::string> opts;
    size_t pos = 0;
    while (pos < opts_str.size()) {
      size_t comma = opts_str.find(',', pos);
      std::string opt = (comma == std::string::npos) ? opts_str.substr(pos) : opts_str.substr(pos, comma - pos);
      opt = trim(opt);
      if (!opt.empty()) opts.push_back(opt);
      if (comma == std::string::npos) break;
      pos = comma + 1;
    }
    if (opts.empty()) opts = {"Tab 1", "Tab 2"};
    if (cur_val.empty()) cur_val = opts[0];

    const int h = w.attr_int("height", 34);
    round(x, y, avail_w, h, 6, kField);
    outline(x, y, avail_w, h, 6, kFieldEdge);

    const int seg_w = avail_w / (int)opts.size();
    const web::FontKey f = font_of(13, true);

    for (size_t i = 0; i < opts.size(); ++i) {
      int seg_x = x + (int)i * seg_w;
      int actual_seg_w = (i == opts.size() - 1) ? (x + avail_w - seg_x) : seg_w;
      bool active = (opts[i] == cur_val || std::to_string(i) == cur_val);

      if (active) {
        round(seg_x + 2, y + 2, actual_seg_w - 4, h - 4, 4, kAccent);
      }
      bool cut;
      const std::string shown = fit(opts[i], f, actual_seg_w - 8, cut);
      const int tw = text_w(shown, f);
      text_line(shown, seg_x + std::max(4, (actual_seg_w - tw) / 2), y + (h - line_h(f)) / 2, actual_seg_w - 8, f, active ? 0xFF0B1620 : kInk, &w, cut);
    }

    Placed p; p.w = &w; p.x = x; p.y = y; p.width = avail_w; p.height = h; p.box = true;
    placed.push_back(p);
    return h;
  }
  if (t == "progress" || t == "bar") {
    const double v = attr_value(w, "value").number();
    const double max_v = attr_value(w, "max").k == Value::K::Null ? 100.0 : attr_value(w, "max").number();
    const double lo = attr_value(w, "min").k == Value::K::Null ? 0.0 : attr_value(w, "min").number();
    const std::string label = label_of(w).empty() ? attr_text(w, "label", "") : label_of(w);
    int cy = y;
    bool cut;
    if (!label.empty()) {
      char pct_str[32];
      double pct = max_v > lo ? ((v - lo) / (max_v - lo)) * 100.0 : 0.0;
      std::snprintf(pct_str, sizeof pct_str, " (%.0f%%)", pct);
      cy += text_line(label + pct_str, x, cy, avail_w, font_of(12, true), kDim, &w, cut) + 4;
    }
    const int bh = w.attr_int("height", 14);
    round(x, cy, avail_w, bh, 4, kField);
    outline(x, cy, avail_w, bh, 4, kFieldEdge);
    double frac = max_v > lo ? (v - lo) / (max_v - lo) : 0.0;
    frac = std::clamp(frac, 0.0, 1.0);
    uint32_t c = accent_of(w, kAccent);
    if (frac > 0) round(x, cy, std::max(4, (int)(avail_w * frac)), bh, 4, c);
    Placed p; p.w = &w; p.x = x; p.y = y; p.width = avail_w; p.height = (cy - y) + bh; p.box = true;
    placed.push_back(p);
    return (cy - y) + bh;
  }
  if (t == "select" || t == "dropdown") {
    const std::string id = w.attr("id", w.attr("name", ""));
    std::string cur_val;
    Value v_val = attr_value(w, "value");
    if (v_val.k != Value::K::Null) cur_val = v_val.text();
    else if (!id.empty() && I.fields.count(id)) cur_val = I.fields[id];
    else if (!id.empty() && I.state.count(id)) cur_val = I.state[id].text();
    else if (!id.empty() && I.lets.count(id)) cur_val = I.lets[id].text();

    std::string opts_str = attr_text(w, "options", label_of(w));
    std::vector<std::string> opts;
    size_t pos = 0;
    while (pos < opts_str.size()) {
      size_t comma = opts_str.find(',', pos);
      std::string opt = (comma == std::string::npos) ? opts_str.substr(pos) : opts_str.substr(pos, comma - pos);
      opt = trim(opt);
      if (!opt.empty()) opts.push_back(opt);
      if (comma == std::string::npos) break;
      pos = comma + 1;
    }
    if (opts.empty()) opts = {"Option 1"};
    if (cur_val.empty()) cur_val = opts[0];

    const int h = w.attr_int("height", 36);
    round(x, y, avail_w, h, 6, kField);
    outline(x, y, avail_w, h, 6, kFieldEdge);

    std::string lbl = attr_text(w, "label", "");
    std::string display_text = lbl.empty() ? cur_val : (lbl + ": " + cur_val);

    const web::FontKey f = font_of(14, false);
    bool cut;
    text_line(display_text, x + 12, y + (h - line_h(f)) / 2, avail_w - 36, f, kInk, &w, cut);
    // Down arrow indicator 'v'
    text_line("v", x + avail_w - 20, y + (h - line_h(f)) / 2, 16, font_of(12, true), kDim, &w, cut);

    Placed p; p.w = &w; p.x = x; p.y = y; p.width = avail_w; p.height = h; p.box = true;
    placed.push_back(p);
    return h;
  }
  if (t == "slider") {
    const std::string id = w.attr("id", w.attr("name", ""));
    double cur_val = 0;
    Value v_val = attr_value(w, "value");
    if (v_val.k != Value::K::Null) cur_val = v_val.number();
    else if (!id.empty() && I.fields.count(id)) cur_val = std::atof(I.fields[id].c_str());
    else if (!id.empty() && I.state.count(id)) cur_val = I.state[id].number();
    else if (!id.empty() && I.lets.count(id)) cur_val = I.lets[id].number();

    const double min_v = attr_value(w, "min").k == Value::K::Null ? 0 : attr_value(w, "min").number();
    const double max_v = attr_value(w, "max").k == Value::K::Null ? 100 : attr_value(w, "max").number();
    double range = max_v - min_v;
    if (range <= 0.0001) range = 1.0;
    double frac = std::clamp((cur_val - min_v) / range, 0.0, 1.0);

    std::string label = label_of(w);
    if (label.empty()) label = attr_text(w, "label", "");

    int cy = y;
    const web::FontKey f = font_of(13, true);
    bool cut;
    if (!label.empty()) {
      std::string header_text = label + " : " + fmt_num(cur_val);
      cy += text_line(header_text, x, cy, avail_w, f, kInk, &w, cut) + 4;
    }

    const int track_y = cy + 6;
    const int track_h = 6;
    round(x, track_y, avail_w, track_h, 3, kField);
    outline(x, track_y, avail_w, track_h, 3, kFieldEdge);

    int active_w = (int)(avail_w * frac);
    uint32_t acc = accent_of(w, kAccent);
    if (active_w > 0) {
      round(x, track_y, std::min(avail_w, active_w), track_h, 3, acc);
    }

    int thumb_x = x + (int)(avail_w * frac);
    thumb_x = std::clamp(thumb_x, x + 6, x + avail_w - 6);
    round(thumb_x - 7, track_y - 4, 14, 14, 7, kInk);
    outline(thumb_x - 7, track_y - 4, 14, 14, 7, acc);

    Placed p; p.w = &w; p.x = x; p.y = y; p.width = avail_w; p.height = (cy - y) + 18; p.box = true;
    placed.push_back(p);
    return (cy - y) + 18;
  }
  if (t == "rating" || t == "stars") {
    const std::string id = w.attr("id", w.attr("name", ""));
    int cur_val = 0;
    Value v_val = attr_value(w, "value");
    if (v_val.k != Value::K::Null) cur_val = (int)v_val.number();
    else if (!id.empty() && I.fields.count(id)) cur_val = std::atoi(I.fields[id].c_str());
    else if (!id.empty() && I.state.count(id)) cur_val = (int)I.state[id].number();
    else if (!id.empty() && I.lets.count(id)) cur_val = (int)I.lets[id].number();

    int max_stars = w.attr_int("max", 5);
    if (max_stars < 1) max_stars = 5;
    cur_val = std::clamp(cur_val, 0, max_stars);

    const int h = w.attr_int("height", 28);
    std::string label = label_of(w);
    if (label.empty()) label = attr_text(w, "label", "");

    int cur_x = x;
    bool cut;
    if (!label.empty()) {
      cur_x += text_line(label, cur_x, y + (h - 14) / 2, avail_w, font_of(13, true), kInk, &w, cut) + 12;
    }

    uint32_t gold_col = 0xFFF59E0B;
    for (int s = 1; s <= max_stars; ++s) {
      bool filled = (s <= cur_val);
      std::string star_icon = filled ? "[*]" : "[ ]";
      text_line(star_icon, cur_x, y + (h - 14) / 2, 28, font_of(13, true), filled ? gold_col : kDim, &w, cut);
      cur_x += 26;
    }

    Placed p; p.w = &w; p.x = x; p.y = y; p.width = std::min(avail_w, cur_x - x); p.height = h; p.box = true;
    placed.push_back(p);
    return h;
  }
  if (t == "accordion" || t == "collapsible" || t == "details") {
    const std::string id = w.attr("id", w.attr("name", ""));
    bool is_open = false;
    Value v_val = attr_value(w, "open");
    if (v_val.k == Value::K::Null) v_val = attr_value(w, "expanded");
    if (v_val.k != Value::K::Null) is_open = v_val.truthy();
    else if (!id.empty() && I.fields.count(id)) {
      const std::string& fs = I.fields[id];
      is_open = (fs == "true" || fs == "1" || fs == "open");
    }
    else if (!id.empty() && I.state.count(id)) is_open = I.state[id].truthy();
    else if (!id.empty() && I.lets.count(id)) is_open = I.lets[id].truthy();
    else if (w.flag("open") || w.flag("expanded")) is_open = true;

    std::string title = label_of(w);
    if (title.empty()) title = attr_text(w, "title", attr_text(w, "label", "Section"));

    const int header_h = 32;
    round(x, y, avail_w, header_h, 6, kPanel);
    outline(x, y, avail_w, header_h, 6, is_open ? kAccent : kPanelEdge);

    bool cut;
    std::string arrow = is_open ? "v" : ">";
    text_line(arrow, x + 10, y + 8, 16, font_of(13, true), kAccent, &w, cut);
    text_line(title, x + 28, y + 8, avail_w - 36, font_of(14, true), kInk, &w, cut);

    Placed p_hdr; p_hdr.w = &w; p_hdr.x = x; p_hdr.y = y; p_hdr.width = avail_w; p_hdr.height = header_h; p_hdr.box = true;
    placed.push_back(p_hdr);

    if (!is_open) return header_h;

    int pad = w.attr_int("pad", 10);
    int content_y = y + header_h + 4;
    int kids_h = lay_children_column(w.children, x + pad, content_y, avail_w - 2 * pad, std::max(0, avail_h - (content_y - y)), w.attr_int("gap", 8), depth + 1);
    return header_h + 4 + kids_h + pad;
  }
  if (t == "tree" || t == "treeview") {
    Value data_val = attr_value(w, "data");
    if (data_val.k == Value::K::Null) data_val = attr_value(w, "items");
    std::vector<std::pair<std::string, int>> tree_items;
    if (data_val.k == Value::K::List && data_val.list) {
      for (const auto& item : *data_val.list) {
        if (item.k == Value::K::Rec) {
          std::string lbl = item.field("label") ? item.field("label")->text() : item.field("name") ? item.field("name")->text() : item.text();
          int lvl = item.field("depth") ? (int)item.field("depth")->number() : item.field("level") ? (int)item.field("level")->number() : 0;
          tree_items.push_back({lbl, lvl});
        } else {
          tree_items.push_back({item.text(), 0});
        }
      }
    } else {
      std::string s = attr_text(w, "items", label_of(w));
      size_t pos = 0;
      while (pos < s.size()) {
        size_t comma = s.find(',', pos);
        std::string token = (comma == std::string::npos) ? s.substr(pos) : s.substr(pos, comma - pos);
        token = trim(token);
        if (!token.empty()) {
          int lvl = 0;
          while (lvl < (int)token.size() && (token[(size_t)lvl] == '-' || token[(size_t)lvl] == ' ' || token[(size_t)lvl] == '/')) ++lvl;
          tree_items.push_back({token.substr((size_t)lvl), lvl});
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
      }
    }

    int cy = y;
    const int item_h = 24;
    const web::FontKey f = font_of(13, false);
    for (size_t i = 0; i < tree_items.size(); ++i) {
      int indent_px = tree_items[i].second * 16;
      bool cut;
      std::string branch = (tree_items[i].second > 0 ? "|- " : "> ");
      text_line(branch + tree_items[i].first, x + 8 + indent_px, cy + 4, avail_w - 16 - indent_px, f, kInk, &w, cut);
      cy += item_h;
    }
    Placed p; p.w = &w; p.x = x; p.y = y; p.width = avail_w; p.height = std::max(item_h, cy - y); p.box = true;
    placed.push_back(p);
    return std::max(item_h, cy - y);
  }
  if (t == "tag" || t == "tags" || t == "chips") {
    Value data_val = attr_value(w, "data");
    if (data_val.k == Value::K::Null) data_val = attr_value(w, "options");
    std::vector<std::string> tags;
    if (data_val.k == Value::K::List && data_val.list) {
      for (const auto& item : *data_val.list) tags.push_back(item.text());
    } else {
      std::string s = attr_text(w, "options", attr_text(w, "items", label_of(w)));
      size_t pos = 0;
      while (pos < s.size()) {
        size_t comma = s.find(',', pos);
        std::string token = (comma == std::string::npos) ? s.substr(pos) : s.substr(pos, comma - pos);
        token = trim(token);
        if (!token.empty()) tags.push_back(token);
        if (comma == std::string::npos) break;
        pos = comma + 1;
      }
    }
    if (tags.empty() && !label_of(w).empty()) tags.push_back(label_of(w));

    uint32_t acc = accent_of(w, kAccent);
    const int chip_h = 24;
    const web::FontKey f = font_of(12, true);
    int cur_x = x;
    int cur_y = y;
    int total_h = chip_h;

    for (const auto& tag_text : tags) {
      int tw = text_w(tag_text, f);
      int chip_w = tw + 16;
      if (cur_x + chip_w > x + avail_w && cur_x > x) {
        cur_x = x;
        cur_y += chip_h + 6;
        total_h += chip_h + 6;
      }
      round(cur_x, cur_y, chip_w, chip_h, 12, (acc & 0x00FFFFFF) | 0x22000000);
      outline(cur_x, cur_y, chip_w, chip_h, 12, acc);
      bool cut;
      text_line(tag_text, cur_x + 8, cur_y + 4, chip_w - 16, f, kInk, &w, cut);
      cur_x += chip_w + 8;
    }

    Placed p; p.w = &w; p.x = x; p.y = y; p.width = avail_w; p.height = total_h; p.box = true;
    placed.push_back(p);
    return total_h;
  }
  if (t == "toolbar") {
    int pad = w.attr_int("pad", 6);
    int bar_h = w.attr_int("height", 46);
    round(x, y, avail_w, bar_h, 6, kPanel);
    outline(x, y, avail_w, bar_h, 6, kPanelEdge);
    lay_row(w, x + pad, y + pad, avail_w - 2 * pad, bar_h - 2 * pad, depth + 1);
    Placed p; p.w = &w; p.x = x; p.y = y; p.width = avail_w; p.height = bar_h; p.box = true;
    placed.push_back(p);
    return bar_h;
  }
  if (t == "spacer") { const int h = w.attr_int("size", w.attr_int("height", 12)); return h; }
  if (t == "divider" || t == "rule" || t == "separator") {
    std::string lbl = label_of(w);
    if (!lbl.empty()) {
      const web::FontKey f = font_of(11, true);
      int tw = text_w(lbl, f);
      int line_len = std::max(10, (avail_w - tw - 16) / 2);
      rect(x, y + 6, line_len, 1, kPanelEdge);
      bool cut;
      text_line(lbl, x + line_len + 8, y, tw + 8, f, kDim, &w, cut);
      rect(x + line_len + tw + 16, y + 6, line_len, 1, kPanelEdge);
    } else {
      rect(x, y + 6, avail_w, 1, kPanelEdge);
    }
    return 13;
  }
  if (t == "panel") {
    const int pad = w.attr_int("pad", 14);
    const std::string title = label_of(w);
    const size_t bg_at = ops.size();
    round(x, y, avail_w, 10, 8, kPanel);            // placeholder, sized below
    outline(x, y, avail_w, 10, 8, kPanelEdge);
    int cy = y + pad;
    if (!title.empty()) {
      bool cut;
      cy += text_line(title, x + pad, cy, avail_w - 2 * pad, font_of(12, true), kAccent, &w, cut) + 8;
    }
    cy += lay_children_column(w.children, x + pad, cy, avail_w - 2 * pad, avail_h, w.attr_int("gap", 10), depth);
    const int h = cy - y + pad;
    ops[bg_at].h = h;
    ops[bg_at + 1].h = h;
    return h;
  }
  if (t == "text" || t == "label" || t == "heading") {
    const std::string s = label_of(w);
    const int size = w.attr_int("size", t == "heading" ? 22 : t == "label" ? 12 : 15);
    const bool bold = w.flag("bold") || t == "heading";
    uint32_t color = w.flag("dim") || t == "label" ? kDim : kInk;
    color = accent_of(w, color);
    const int h = text_block(s, x, y, avail_w, font_of(size, bold, w.flag("mono")), color, &w, align_of(w));
    return h > 0 ? h : line_h(font_of(size, bold));
  }
  if (t == "tile") {
    const std::string v = attr_text(w, "value", "");
    std::string l = attr_text(w, "label", "");
    if (l.empty()) l = w.label;
    uint32_t accent = kInk;
    const std::string a = attr_text(w, "accent", "");
    if (a == "warn") accent = kWarn; else if (a == "bad") accent = kBad; else if (a == "ok") accent = kOk; else if (a == "accent") accent = kAccent; else if (!a.empty()) accent = color_of(a, kInk);
    const int h = 84;
    round(x, y, avail_w, h, 8, kPanel);
    outline(x, y, avail_w, h, 8, a == "warn" ? kWarn : a == "bad" ? kBad : kPanelEdge);
    bool cut;
    text_line(v, x + 16, y + 12, avail_w - 32, font_of(26, true), accent, &w, cut);
    std::string up = l;
    for (auto& ch : up) if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
    text_line(up, x + 16, y + 54, avail_w - 32, font_of(11, true), kDim, &w, cut);
    return h;
  }
  if (t == "field" || t == "input") {
    const std::string id = w.attr("id", w.attr("name", ""));
    const bool focused = !id.empty() && focus == id;
    const int h = w.attr_int("height", 38);
    round(x, y, avail_w, h, 6, kField);
    outline(x, y, avail_w, h, 6, focused ? kAccent : kFieldEdge);
    { Placed p; p.w = &w; p.x = x; p.y = y; p.width = avail_w; p.height = h; p.box = true; placed.push_back(p); }
    const std::string value = id.empty() ? "" : I.fields[id];
    const web::FontKey f = font_of(16, false, w.flag("mono"));
    bool cut;
    if (value.empty()) {
      const std::string ph = attr_text(w, "placeholder", label_of(w));
      text_line(ph, x + 12, y + (h - line_h(f)) / 2, avail_w - 24, f, kDim, &w, cut);
      if (focused && blink_phase < 0.6) rect(x + 12, y + 8, 2, h - 16, kAccent);
    } else {
      // show the tail of a value longer than the box
      std::string shown = value;
      while (text_w(shown, f) > avail_w - 24 && shown.size() > 1) shown.erase(0, 1);
      text_line(shown, x + 12, y + (h - line_h(f)) / 2, avail_w - 24, f, kInk, &w, cut);
      if (focused && blink_phase < 0.6) rect(x + 12 + text_w(shown, f) + 1, y + 8, 2, h - 16, kAccent);
    }
    return h;
  }
  if (t == "button") {
    const std::string label = label_of(w);
    const bool primary = w.flag("primary");
    const bool danger = w.flag("danger");
    const int h = w.attr_int("height", 38);
    uint32_t bg = primary ? kAccent : danger ? kBad : kPanel;
    round(x, y, avail_w, h, 6, bg);
    if (!primary && !danger) outline(x, y, avail_w, h, 6, kFieldEdge);
    { Placed p; p.w = &w; p.x = x; p.y = y; p.width = avail_w; p.height = h; p.box = true; placed.push_back(p); }
    const web::FontKey f = font_of(15, true);
    bool cut;
    const std::string shown = fit(label, f, avail_w - 16, cut);
    const int tw = text_w(shown, f);
    text_line(label, x + std::max(8, (avail_w - tw) / 2), y + (h - line_h(f)) / 2, avail_w - 16, f, primary ? 0xFF0B1620 : kInk, &w, cut);
    return h;
  }
  if (t == "gauge") {
    const double v = attr_value(w, "value").number();
    const double lo = attr_value(w, "min").k == Value::K::Null ? 0 : attr_value(w, "min").number();
    const double hi = attr_value(w, "max").k == Value::K::Null ? 100 : attr_value(w, "max").number();
    const std::string label = label_of(w).empty() ? attr_text(w, "label", "") : label_of(w);
    int cy = y;
    bool cut;
    if (!label.empty()) cy += text_line(label + "  " + fmt_num(v), x, cy, avail_w, font_of(12, true), kDim, &w, cut) + 4;
    const int bh = w.attr_int("height", 14);
    round(x, cy, avail_w, bh, 4, kField);
    double frac = hi > lo ? (v - lo) / (hi - lo) : 0;
    frac = std::max(0.0, std::min(1.0, frac));
    uint32_t c = accent_of(w, kAccent);
    if (frac > 0) round(x, cy, std::max(4, (int)(avail_w * frac)), bh, 4, c);
    return cy - y + bh;
  }
  if (t == "list") {
    Value rows = Value::make_list();
    if (!w.words.empty()) { std::string err; ExprP e = parse_expr(w.words[0], err); if (err.empty() && e) rows = ev.eval(*e); else if (!err.empty()) ev.fault("list: " + err); }
    if (rows.k != Value::K::List) rows = Value::make_list();
    const int max_rows = w.attr_int("rows", 10);
    const web::FontKey f = font_of(w.attr_int("size", 15), false, w.flag("mono"));
    std::string err;
    ExprP tmpl;
    if (!w.label.empty()) tmpl = parse_expr("\"" + w.label + "\"", err);
    int cy = y;
    size_t n = 0;
    auto saved = I.scope;
    for (const auto& r : *rows.list) {
      if ((int)n >= max_rows) break;
      I.scope["it"] = r;
      if (r.k == Value::K::Rec) for (const auto& kv : *r.rec) I.scope[kv.first] = kv.second;
      if (r.field("_t")) { std::vector<Value> a{*r.field("_t")}; I.scope["when"] = ev.call("time", a); I.scope["ago"] = ev.call("ago", a); }
      std::string line = tmpl ? ev.eval(*tmpl).text() : r.text();
      I.scope = saved;
      if (n % 2 == 1) rect(x, cy, avail_w, line_h(f) + 8, kZebra);
      bool cut;
      text_line(line, x + 10, cy + 4, avail_w - 20, f, kInk, &w, cut);
      cy += line_h(f) + 8;
      ++n;
    }
    I.scope = saved;
    if (rows.list->empty()) { bool cut; cy += text_line(attr_text(w, "empty", "nothing here yet"), x + 10, cy + 4, avail_w - 20, font_of(14), kDim, &w, cut) + 8; }
    else if (rows.list->size() > (size_t)max_rows) { bool cut; cy += text_line("+ " + std::to_string(rows.list->size() - (size_t)max_rows) + " more", x + 10, cy + 4, avail_w - 20, font_of(12), kDim, &w, cut) + 6; }
    return cy - y;
  }
  if (t == "table") return lay_table(w, x, y, avail_w, avail_h, depth);
  if (t == "paint") {
    int h = w.attr_int("height", 0);
    if (h <= 0 || w.flag("fill")) h = std::max(120, avail_h);
    I.paint_x = x; I.paint_y = y; I.paint_w = avail_w; I.paint_h = h;
    I.has_paint = true;
    round(x, y, avail_w, h, 6, color_of(w.attr("background", "#0b0f14"), 0xFF0B0F14));
    Placed p; p.w = &w; p.x = x; p.y = y; p.width = avail_w; p.height = h; p.box = true; placed.push_back(p);
    for (const auto& cw_ptr : w.children) {
      if (!cw_ptr) continue;
      const Widget& cw = *cw_ptr;
      std::string verb = cw.type;
      size_t w_offset = 0;
      if (verb == "paint" || verb == "draw") {
        if (!cw.words.empty()) {
          verb = cw.words[0];
          w_offset = 1;
        }
      }
      auto word_num = [&](size_t idx, double def = 0.0) -> double {
        size_t pos = w_offset + idx;
        if (pos < cw.words.size()) {
          char* end_ptr = nullptr;
          double d = std::strtod(cw.words[pos].c_str(), &end_ptr);
          if (end_ptr != nullptr && end_ptr != cw.words[pos].c_str() && *end_ptr == '\0') return d;
          std::string err;
          ExprP e = parse_expr(cw.words[pos], err);
          if (err.empty() && e) return ev.eval(*e).number();
        }
        return def;
      };
      auto attr_val = [&](const std::string& k, const std::string& def = "") -> std::string {
        for (const auto& kv : cw.exprs) if (kv.first == k && kv.second) return ev.eval(*kv.second).text();
        return cw.attr(k, def);
      };
      PaintCmd cmd;
      cmd.hit_id = attr_val("id", attr_val("hit", attr_val("hit_id", "")));
      cmd.hit_action = attr_val("action", attr_val("click", ""));
      std::string col_str = attr_val("color", cw.attr("color", ""));
      cmd.color = col_str.empty() ? kInk : color_of(col_str, kInk);

      if (verb == "circle" || verb == "dot") {
        cmd.k = PaintCmd::K::Circle;
        cmd.a = word_num(0);
        cmd.b = word_num(1);
        cmd.c = word_num(2, 10.0);
      } else if (verb == "rect" || verb == "box") {
        cmd.k = PaintCmd::K::Rect;
        cmd.a = word_num(0);
        cmd.b = word_num(1);
        cmd.c = word_num(2, 100.0);
        cmd.d = word_num(3, 50.0);
      } else if (verb == "outline" || verb == "frame") {
        cmd.k = PaintCmd::K::Outline;
        cmd.a = word_num(0);
        cmd.b = word_num(1);
        cmd.c = word_num(2, 100.0);
        cmd.d = word_num(3, 50.0);
      } else if (verb == "line") {
        cmd.k = PaintCmd::K::Line;
        cmd.a = word_num(0);
        cmd.b = word_num(1);
        cmd.c = word_num(2);
        cmd.d = word_num(3);
        cmd.e = word_num(4, 1.0);
      } else if (verb == "poly" || verb == "shape") {
        cmd.k = PaintCmd::K::Poly;
        for (size_t wi = w_offset; wi < cw.words.size(); ++wi) {
          char* end_ptr = nullptr;
          double d = std::strtod(cw.words[wi].c_str(), &end_ptr);
          if (end_ptr != nullptr && end_ptr != cw.words[wi].c_str()) {
            cmd.pts.push_back((float)d);
          }
        }
      } else if (verb == "ellipse" || verb == "oval") {
        cmd.k = PaintCmd::K::Ellipse;
        cmd.a = word_num(0);
        cmd.b = word_num(1);
        cmd.c = word_num(2);
        cmd.d = word_num(3);
      } else if (verb == "tensor" || verb == "buffer") {
        cmd.k = PaintCmd::K::Tensor;
        cmd.a = word_num(1, 0);
        cmd.b = word_num(2, 0);
        cmd.c = word_num(3, 100);
        cmd.d = word_num(4, 100);
      }
      paint.push_back(cmd);
    }
    return h;
  }
  if (t == "editor") {
    const std::string id = w.attr("id", w.attr("name", ""));
    auto& ed = I.editors[id];
    ed.id = id;
    const bool focused = !id.empty() && focus == id;

    // Check if path changed dynamically
    std::string path_attr = attr_text(w, "path", "");
    if (!path_attr.empty() && path_attr != ed.path && !ed.dirty) {
      ed.path = path_attr;
      if (ev.host.read_file) {
        std::string content = ev.host.read_file(path_attr);
        ed.lines.clear();
        std::string cur;
        for (char c : content) {
          if (c == '\r') continue;
          if (c == '\n') { ed.lines.push_back(cur); cur.clear(); }
          else cur += c;
        }
        ed.lines.push_back(cur);
        if (ed.lines.empty()) ed.lines.push_back("");
        ed.cursor_line = 0; ed.cursor_col = 0;
        ed.scroll_line = 0; ed.scroll_col = 0;
        ed.dirty = false;
      }
    }
    if (ed.lines.empty()) ed.lines.push_back("");

    int h = w.attr_int("height", 0);
    if (h <= 0 || w.flag("fill")) h = std::max(120, avail_h);

    const web::FontKey f = font_of(15, false, true); // Monospace font
    const int char_w = std::max(7, text_w("M", f));
    const int lh = std::max(16, line_h(f) + 3);

    // Gutter width
    int gutter_w = 0;
    const bool show_ln = w.flag("line_numbers");
    if (show_ln) {
      int digits = (int)std::to_string(std::max(1, (int)ed.lines.size())).size();
      gutter_w = std::max(36, digits * char_w + 20);
    }

    // Outer editor box
    round(x, y, avail_w, h, 6, 0xFF0D1117);
    outline(x, y, avail_w, h, 6, focused ? 0xFF58A6FF : 0xFF30363D);

    // Placed box for hit testing
    { Placed p; p.w = &w; p.x = x; p.y = y; p.width = avail_w; p.height = h; p.box = true; placed.push_back(p); }

    // Gutter background
    if (show_ln) {
      round(x, y, gutter_w, h, 6, 0xFF090D12);
      rect(x + gutter_w - 1, y, 1, h, 0xFF21262D);
    }

    const int text_area_x = x + gutter_w + 8;
    const int text_area_w = avail_w - gutter_w - 16;
    const int visible_lines = std::max(1, (h - 16) / lh);
    const int visible_cols = std::max(10, text_area_w / char_w);

    // Clamp scroll
    if (ed.cursor_line < ed.scroll_line) ed.scroll_line = ed.cursor_line;
    if (ed.cursor_line >= ed.scroll_line + visible_lines) ed.scroll_line = ed.cursor_line - visible_lines + 1;
    if (ed.cursor_col < ed.scroll_col) ed.scroll_col = ed.cursor_col;
    if (ed.cursor_col >= ed.scroll_col + visible_cols) ed.scroll_col = ed.cursor_col - visible_cols + 1;
    if (ed.scroll_line < 0) ed.scroll_line = 0;
    if (ed.scroll_col < 0) ed.scroll_col = 0;

    ed.box_x = x; ed.box_y = y; ed.box_w = avail_w; ed.box_h = h;
    ed.gutter_w = gutter_w; ed.char_w = char_w; ed.line_h = lh;
    ed.last_view_lines = visible_lines; ed.last_view_cols = visible_cols;

    // Render lines
    for (int vi = 0; vi < visible_lines; ++vi) {
      int line_idx = ed.scroll_line + vi;
      if (line_idx >= (int)ed.lines.size()) break;

      int cur_y = y + 8 + vi * lh;

      // Active line highlight
      if (focused && line_idx == ed.cursor_line) {
        rect(x + gutter_w, cur_y - 2, avail_w - gutter_w - 1, lh, 0xFF161B22);
      }

      // Line number in gutter
      if (show_ln) {
        std::string num_str = std::to_string(line_idx + 1);
        int nw = text_w(num_str, f);
        uint32_t num_col = (line_idx == ed.cursor_line) ? (focused ? 0xFF58A6FF : 0xFFC9D1D9) : 0xFF484F58;
        editor_text_span(num_str, x + gutter_w - 10 - nw, cur_y, f, num_col, &w);
      }

      // Syntax highlighted text
      const std::string& full_line = ed.lines[line_idx];
      std::string visible_line_text;
      if (ed.scroll_col < (int)full_line.size()) {
        visible_line_text = full_line.substr(ed.scroll_col, visible_cols);
      }

      auto tokens = tokenize_code_line(visible_line_text);
      int token_x = text_area_x;
      for (const auto& tok : tokens) {
        editor_text_span(tok.text, token_x, cur_y, f, tok.color, &w);
        token_x += text_w(tok.text, f);
        if (token_x >= text_area_x + text_area_w) break;
      }
    }

    // Caret
    if (focused && blink_phase < 0.6) {
      if (ed.cursor_line >= ed.scroll_line && ed.cursor_line < ed.scroll_line + visible_lines) {
        int rel_col = ed.cursor_col - ed.scroll_col;
        if (rel_col >= 0 && rel_col <= visible_cols) {
          int cx = text_area_x + rel_col * char_w;
          int cy = y + 8 + (ed.cursor_line - ed.scroll_line) * lh;
          rect(cx, cy, 2, lh - 2, 0xFF58A6FF);
        }
      }
    }

    return h;
  }
  return 0;
}

}  // namespace

void Runtime::frame(double dt_ms, int width, int height) {
  Impl& I = *impl_;
  if (!loaded_) load();
  I.dt_ms = std::max(0.0, std::min(250.0, dt_ms));
  I.now_ms = host_.now_ms ? host_.now_ms() : I.now_ms + I.dt_ms;
  I.frame_no++;
  I.list_cache.clear();
  I.xform.clear();
  last_w_ = width; last_h_ = height;
  Ev ev{I, host_, faults_, message_, focus_, I.fields, dirty_};
  // Reactive bindings, in order, every frame.
  for (const auto& l : face_.lets) I.lets[l.first] = l.second ? ev.eval(*l.second) : Value::null();
  // Layout first, so `on frame` knows the paint area.
  I.ops.clear();
  placed_.clear();
  paint_.clear();
  I.has_paint = false;
  Layout L{I, ev, face_, I.ops, placed_, paint_, focus_, width, height, std::fmod(I.now_ms / 1000.0, 1.0)};
  const int pad = 16;
  int cy = pad;
  if (!face_.title.empty() && !face_.widgets.empty() && face_.widgets[0]->type != "paint") {
    bool cut;
    cy += L.text_line(face_.title, pad, cy, width - 2 * pad, font_of(20, true), kInk, nullptr, cut) + 10;
  }
  for (size_t i = 0; i < face_.widgets.size(); ++i) {
    const Widget& w = *face_.widgets[i];
    const int remaining = height - cy - pad;
    cy += L.lay(w, pad, cy, width - 2 * pad, remaining);
    if (i + 1 < face_.widgets.size()) cy += 12;
  }
  // The face's own frame handlers, then the lets again so what they set shows.
  for (const auto& h : face_.handlers) if (h.event == "frame") for (const auto& s : h.body) { ev.exec(*s, paint_); if (ev.stop) break; }
}

void Runtime::paint(std::vector<uint32_t>& buf, int buf_w, int buf_h, int ox, int oy) const {
  const Impl& I = *impl_;
  px::fill_rect(buf, buf_w, buf_h, ox, oy, last_w_, last_h_, kBg);
  for (const auto& op : I.ops) {
    switch (op.k) {
      case DrawOp::K::Rect: px::fill_rect(buf, buf_w, buf_h, ox + op.x, oy + op.y, op.w, op.h, op.color); break;
      case DrawOp::K::Outline: px::rect_outline(buf, buf_w, buf_h, ox + op.x, oy + op.y, op.w, op.h, op.color); break;
      case DrawOp::K::Round: px::rounded_rect(buf, buf_w, buf_h, ox + op.x, oy + op.y, op.w, op.h, op.r, op.color); break;
      case DrawOp::K::RoundOutline: px::round_outline(buf, buf_w, buf_h, ox + op.x, oy + op.y, op.w, op.h, op.r, op.color); break;
      case DrawOp::K::Text: web_draw_text_px(buf, buf_w, buf_h, ox + op.x, oy + op.y, op.text, op.color, op.font); break;
      case DrawOp::K::BitmapImage:
        if (op.bitmap && op.bitmap->w > 0 && op.bitmap->h > 0) {
          const auto& bm = *op.bitmap;
          for (int dy = 0; dy < op.h; ++dy) {
            int src_y = (int)((int64_t)dy * bm.h / op.h);
            if (src_y < 0 || src_y >= bm.h) continue;
            int dst_y = oy + op.y + dy;
            if (dst_y < 0 || dst_y >= buf_h) continue;
            for (int dx = 0; dx < op.w; ++dx) {
              int src_x = (int)((int64_t)dx * bm.w / op.w);
              if (src_x < 0 || src_x >= bm.w) continue;
              int dst_x = ox + op.x + dx;
              if (dst_x < 0 || dst_x >= buf_w) continue;
              uint32_t pixel = bm.pixels[(size_t)src_y * (size_t)bm.w + (size_t)src_x];
              uint8_t a = (uint8_t)(pixel >> 24);
              if (!a) continue;
              if (a == 255) buf[(size_t)dst_y * (size_t)buf_w + (size_t)dst_x] = pixel;
              else px::blend(buf, buf_w, buf_h, dst_x, dst_y, pixel, a);
            }
          }
        }
        break;
      default: break;
    }
  }
  // What `paint …` drew, clipped to the paint area.
  if (I.has_paint && !paint_.empty()) {
    const int px0 = ox + I.paint_x, py0 = oy + I.paint_y, pw = I.paint_w, ph = I.paint_h;
    auto put = [&](int x, int y, uint32_t color) {
      if (x < 0 || y < 0 || x >= pw || y >= ph) return;
      const int bx = px0 + x, by = py0 + y;
      if (bx < 0 || by < 0 || bx >= buf_w || by >= buf_h) return;
      const int a = (int)((color >> 24) & 0xFF);
      if (!a) return;
      if (a == 255) buf[(size_t)by * (size_t)buf_w + (size_t)bx] = color;
      else px::blend(buf, buf_w, buf_h, bx, by, color, a);
    };
    auto fill = [&](int x, int y, int w, int h, uint32_t color) {
      int x0 = std::max(0, x), y0 = std::max(0, y), x1 = std::min(pw, x + w), y1 = std::min(ph, y + h);
      if (x1 <= x0 || y1 <= y0) return;
      if (((color >> 24) & 0xFF) == 255) px::fill_rect(buf, buf_w, buf_h, px0 + x0, py0 + y0, x1 - x0, y1 - y0, color);
      else for (int yy = y0; yy < y1; ++yy) for (int xx = x0; xx < x1; ++xx) put(xx, yy, color);
    };
    // A polygon, with COVERAGE rather than a yes/no per pixel: four sub-rows per
    // pixel row, and the two ends of every span counted by the fraction of the
    // pixel they actually cover. This is what the circle and the line never had
    // — their edges were a staircase, and a staircase is most of why a drawn
    // thing on this machine looked cheap. Everything curved goes through here.
    auto fill_poly = [&](const float* pts, size_t n, uint32_t color) {
      if (n < 3) return;
      const int base_a = (int)((color >> 24) & 0xFF);
      if (!base_a) return;
      double min_x = pts[0], max_x = pts[0], min_y = pts[1], max_y = pts[1];
      for (size_t i = 0; i < n; ++i) {
        min_x = std::min(min_x, (double)pts[i * 2]); max_x = std::max(max_x, (double)pts[i * 2]);
        min_y = std::min(min_y, (double)pts[i * 2 + 1]); max_y = std::max(max_y, (double)pts[i * 2 + 1]);
      }
      const int y0 = std::max(0, (int)std::floor(min_y)), y1 = std::min(ph - 1, (int)std::ceil(max_y));
      const int x0 = std::max(0, (int)std::floor(min_x)), x1 = std::min(pw - 1, (int)std::ceil(max_x));
      if (y1 < y0 || x1 < x0) return;
      const int width = x1 - x0 + 1;
      if (width > 8192) return;
      std::vector<float> cov((size_t)width);
      std::vector<double> xs;
      constexpr int kSub = 4;
      for (int y = y0; y <= y1; ++y) {
        std::fill(cov.begin(), cov.end(), 0.0f);
        for (int s2 = 0; s2 < kSub; ++s2) {
          const double sy = (double)y + ((double)s2 + 0.5) / kSub;
          xs.clear();
          for (size_t i = 0; i < n; ++i) {
            const double ax = pts[i * 2], ay = pts[i * 2 + 1];
            const size_t j = (i + 1) % n;
            const double bx = pts[j * 2], by = pts[j * 2 + 1];
            if ((ay <= sy && by > sy) || (by <= sy && ay > sy))
              xs.push_back(ax + (sy - ay) / (by - ay) * (bx - ax));
          }
          if (xs.size() < 2) continue;
          std::sort(xs.begin(), xs.end());
          for (size_t i = 0; i + 1 < xs.size(); i += 2) {
            double sx = xs[i], ex = xs[i + 1];
            if (ex <= (double)x0 || sx >= (double)x1 + 1.0) continue;
            sx = std::max(sx, (double)x0);
            ex = std::min(ex, (double)x1 + 1.0);
            int px_s = (int)std::floor(sx), px_e = (int)std::floor(ex - 1e-9);
            if (px_s == px_e) { cov[(size_t)(px_s - x0)] += (float)((ex - sx) / kSub); continue; }
            cov[(size_t)(px_s - x0)] += (float)((px_s + 1 - sx) / kSub);
            for (int x = px_s + 1; x < px_e; ++x) cov[(size_t)(x - x0)] += 1.0f / kSub;
            cov[(size_t)(px_e - x0)] += (float)((ex - px_e) / kSub);
          }
        }
        for (int x = 0; x < width; ++x) {
          if (cov[(size_t)x] <= 0.002f) continue;
          const int a2 = (int)std::lround((double)base_a * std::min(1.0f, cov[(size_t)x]));
          if (a2 <= 0) continue;
          put(x0 + x, y, (color & 0x00FFFFFFu) | ((uint32_t)std::min(255, a2) << 24));
        }
      }
    };
    // A ring of points around a centre: what a circle and an ellipse ARE, once
    // they are drawn with coverage rather than by rounding a square root.
    auto ring = [](double cx, double cy, double rx, double ry, std::vector<float>& out) {
      const double r = std::max(std::fabs(rx), std::fabs(ry));
      int steps = (int)std::clamp(r * 1.6, 12.0, 220.0);
      out.clear();
      out.reserve((size_t)steps * 2);
      for (int i = 0; i < steps; ++i) {
        const double t = 6.283185307179586 * (double)i / (double)steps;
        out.push_back((float)(cx + std::cos(t) * rx));
        out.push_back((float)(cy + std::sin(t) * ry));
      }
    };
    // A stroke of a given width: the segment as a quad, and a disc at each end
    // so joins and caps are round instead of notched.
    auto stroke_seg = [&](double ax, double ay, double bx, double by, double w, uint32_t color) {
      const double dx = bx - ax, dy = by - ay;
      const double len = std::sqrt(dx * dx + dy * dy);
      const double hw = std::max(0.5, w / 2.0);
      std::vector<float> quad;
      if (len < 1e-6) { ring(ax, ay, hw, hw, quad); fill_poly(quad.data(), quad.size() / 2, color); return; }
      const double nx = -dy / len * hw, ny = dx / len * hw;
      quad = {(float)(ax + nx), (float)(ay + ny), (float)(bx + nx), (float)(by + ny),
              (float)(bx - nx), (float)(by - ny), (float)(ax - nx), (float)(ay - ny)};
      fill_poly(quad.data(), 4, color);
      if (w > 2.5) {
        std::vector<float> cap;
        ring(ax, ay, hw, hw, cap); fill_poly(cap.data(), cap.size() / 2, color);
        ring(bx, by, hw, hw, cap); fill_poly(cap.data(), cap.size() / 2, color);
      }
    };
    for (const auto& p : paint_) {
      switch (p.k) {
        case PaintCmd::K::Clear: fill(0, 0, pw, ph, p.color); break;
        case PaintCmd::K::Rect: fill((int)std::lround(p.a), (int)std::lround(p.b), (int)std::lround(p.c), (int)std::lround(p.d), p.color); break;
        case PaintCmd::K::Outline: {
          const int x = (int)std::lround(p.a), y = (int)std::lround(p.b), w = (int)std::lround(p.c), h = (int)std::lround(p.d);
          fill(x, y, w, 1, p.color); fill(x, y + h - 1, w, 1, p.color); fill(x, y, 1, h, p.color); fill(x + w - 1, y, 1, h, p.color);
          break;
        }
        case PaintCmd::K::Circle: {
          if (p.c <= 0 || p.c > 4096) break;
          std::vector<float> pts;
          ring(p.a, p.b, p.c, p.c, pts);
          fill_poly(pts.data(), pts.size() / 2, p.color);
          break;
        }
        case PaintCmd::K::Ellipse: {
          if (p.c <= 0 || p.d <= 0 || p.c > 4096 || p.d > 4096) break;
          std::vector<float> pts;
          ring(p.a, p.b, p.c, p.d, pts);
          fill_poly(pts.data(), pts.size() / 2, p.color);
          break;
        }
        case PaintCmd::K::Poly: fill_poly(p.pts.data(), p.pts.size() / 2, p.color); break;
        case PaintCmd::K::Curve: {
          if (p.pts.size() < 6) break;
          // Flattened into segments and stroked. How many depends on how far it
          // travels, so a long curve does not go polygonal.
          const double x0 = p.pts[0], y0 = p.pts[1], cx = p.pts[2], cy = p.pts[3], x1 = p.pts[4], y1 = p.pts[5];
          const double span = std::fabs(x1 - x0) + std::fabs(y1 - y0) + std::fabs(cx - x0) + std::fabs(cy - y0);
          const int steps = (int)std::clamp(span / 6.0, 8.0, 160.0);
          double px_ = x0, py_ = y0;
          for (int i = 1; i <= steps; ++i) {
            const double t = (double)i / steps, u = 1.0 - t;
            const double nx = u * u * x0 + 2 * u * t * cx + t * t * x1;
            const double ny = u * u * y0 + 2 * u * t * cy + t * t * y1;
            stroke_seg(px_, py_, nx, ny, p.e, p.color);
            px_ = nx; py_ = ny;
          }
          break;
        }
        case PaintCmd::K::Grad: {
          const int gx = (int)std::lround(p.a), gy = (int)std::lround(p.b);
          const int gw = (int)std::lround(p.c), gh = (int)std::lround(p.d);
          if (gw <= 0 || gh <= 0) break;
          const int steps = p.e > 0.5 ? gw : gh;
          auto lerp = [&](int i) {
            const double t = steps > 1 ? (double)i / (double)(steps - 1) : 0.0;
            uint32_t out = 0;
            for (int sh = 0; sh <= 24; sh += 8) {
              const double s0 = (double)((p.color >> sh) & 0xFF), s1 = (double)((p.color2 >> sh) & 0xFF);
              out |= (uint32_t)std::lround(s0 + (s1 - s0) * t) << sh;
            }
            return out;
          };
          if (p.e > 0.5) for (int i = 0; i < gw; ++i) fill(gx + i, gy, 1, gh, lerp(i));
          else for (int i = 0; i < gh; ++i) fill(gx, gy + i, gw, 1, lerp(i));
          break;
        }
        case PaintCmd::K::Line: stroke_seg(p.a, p.b, p.c, p.d, p.e > 0 ? p.e : 1.0, p.color); break;
        case PaintCmd::K::Text: {
          const int x = (int)std::lround(p.a), y = (int)std::lround(p.b);
          if (y + p.px < 0 || y > ph) break;
          web_draw_text_px(buf, buf_w, buf_h, px0 + x, py0 + y, p.text, p.color, font_of(p.px, p.px >= 20));
          break;
        }
        case PaintCmd::K::Tensor: {
          const int tx = (int)std::lround(p.a), ty = (int)std::lround(p.b);
          const int tw = (int)std::lround(p.c), th = (int)std::lround(p.d);
          if (tw <= 0 || th <= 0 || p.tensor_data.empty()) break;
          int gw = p.tensor_w > 0 ? p.tensor_w : (int)p.tensor_data.size();
          int gh = p.tensor_h > 0 ? p.tensor_h : 1;
          for (int dy = 0; dy < th; ++dy) {
            int gy = dy * gh / th;
            for (int dx = 0; dx < tw; ++dx) {
              int gx = dx * gw / tw;
              size_t idx = (size_t)(gy * gw + gx);
              double val = (idx < p.tensor_data.size()) ? p.tensor_data[idx] : 0.0;
              val = std::clamp(val, 0.0, 1.0);
              uint32_t col = ((uint32_t)(val * 255) << 16) | ((uint32_t)((1.0 - val) * 200) << 8) | 0xFF0000FF;
              put(tx + dx, ty + dy, col);
            }
          }
          break;
        }
      }
    }
  }
}

void Runtime::click(int x, int y) {
  Impl& I = *impl_;
  if (!loaded_) load();

  // Check paint shape hit testing first
  for (auto it = paint_.rbegin(); it != paint_.rend(); ++it) {
    const auto& cmd = *it;
    if (cmd.hit_action.empty() && cmd.hit_id.empty()) continue;
    bool shape_hit = false;
    if (cmd.k == PaintCmd::K::Circle) {
      double dx1 = x - (cmd.a + I.paint_x), dy1 = y - (cmd.b + I.paint_y);
      double dx2 = x - cmd.a, dy2 = y - cmd.b;
      if (dx1 * dx1 + dy1 * dy1 <= cmd.c * cmd.c || dx2 * dx2 + dy2 * dy2 <= cmd.c * cmd.c) shape_hit = true;
    } else if (cmd.k == PaintCmd::K::Rect || cmd.k == PaintCmd::K::Outline) {
      double rx1 = cmd.a + I.paint_x, ry1 = cmd.b + I.paint_y;
      double rx2 = cmd.a, ry2 = cmd.b;
      if ((x >= rx1 && x <= rx1 + cmd.c && y >= ry1 && y <= ry1 + cmd.d) ||
          (x >= rx2 && x <= rx2 + cmd.c && y >= ry2 && y <= ry2 + cmd.d)) shape_hit = true;
    } else if (cmd.k == PaintCmd::K::Poly && cmd.pts.size() >= 6) {
      size_t n = cmd.pts.size() / 2;
      double px1 = x - I.paint_x, py1 = y - I.paint_y;
      double px2 = x, py2 = y;
      auto test_poly = [&](double px, double py) {
        bool c = false;
        for (size_t i = 0, j = n - 1; i < n; j = i++) {
          double xi = cmd.pts[i * 2], yi = cmd.pts[i * 2 + 1];
          double xj = cmd.pts[j * 2], yj = cmd.pts[j * 2 + 1];
          if (((yi > py) != (yj > py)) && (px < (xj - xi) * (py - yi) / (yj - yi) + xi)) {
            c = !c;
          }
        }
        return c;
      };
      shape_hit = test_poly(px1, py1) || test_poly(px2, py2);
    }
    if (shape_hit) {
      if (!cmd.hit_id.empty()) {
        I.scope["hit_id"] = Value::str(cmd.hit_id);
        I.state["hit_id"] = Value::str(cmd.hit_id);
      }
      if (!cmd.hit_action.empty()) run_action(cmd.hit_action);
      return;
    }
  }

  // Every control was placed with its whole box this frame, so a hit is exact:
  // the last box under the point wins (children are placed after parents).
  const Widget* hit = nullptr;
  for (const auto& p : placed_) {
    if (!p.w || !p.box) continue;
    if (x >= p.x && x < p.x + p.width && y >= p.y && y < p.y + p.height) hit = p.w;
  }
  if (!hit) {
    focus_.clear();
    return;
  }
  if (hit->type == "field" || hit->type == "input") {
    focus_ = hit->attr("id", hit->attr("name", ""));
    return;
  }
  if (hit->type == "editor") {
    std::string id = hit->attr("id", hit->attr("name", ""));
    focus_ = id;
    auto it = I.editors.find(id);
    if (it != I.editors.end()) {
      auto& ed = it->second;
      int rel_y = y - (ed.box_y + 8);
      int target_line = ed.scroll_line + (rel_y >= 0 && ed.line_h > 0 ? rel_y / ed.line_h : 0);
      target_line = std::clamp(target_line, 0, (int)ed.lines.size() - 1);
      int rel_x = x - (ed.box_x + ed.gutter_w + 8);
      int target_col = ed.scroll_col + (rel_x > 0 && ed.char_w > 0 ? (rel_x + ed.char_w / 2) / ed.char_w : 0);
      target_col = std::clamp(target_col, 0, (int)ed.lines[target_line].size());
      ed.cursor_line = target_line;
      ed.cursor_col = target_col;
    }
    return;
  }
  if (hit->type == "button") {
    const std::string action = hit->attr("action", hit->attr("on", ""));
    if (!action.empty()) run_action(action);
    return;
  }
  if (hit->type == "scrubber" || hit->type == "timeline") {
    for (const auto& p : placed_) {
      if (p.w == hit && p.box) {
        double min_v = 0, max_v = 100, step_v = 1;
        if (hit->attr("min", "") != "") min_v = std::atof(hit->attr("min").c_str());
        if (hit->attr("max", "") != "") max_v = std::atof(hit->attr("max").c_str());
        if (hit->attr("step", "") != "") step_v = std::atof(hit->attr("step").c_str());
        
        double frac = 0.0;
        if (p.width > 0) {
          frac = std::clamp((double)(x - p.x) / (double)p.width, 0.0, 1.0);
        }
        double raw_v = min_v + frac * (max_v - min_v);
        if (step_v > 0) {
          raw_v = min_v + std::round((raw_v - min_v) / step_v) * step_v;
        }
        std::string id = hit->attr("id", hit->attr("name", ""));
        if (!id.empty()) {
          I.fields[id] = fmt_num(raw_v);
          I.scope[id] = Value::num(raw_v);
          I.state[id] = Value::num(raw_v);
          dirty_ = true;
        }
        const std::string action = hit->attr("action", hit->attr("on", ""));
        if (!action.empty()) run_action(action);
        break;
      }
    }
    return;
  }
  if (hit->type == "checkbox" || hit->type == "check") {
    std::string id = hit->attr("id", hit->attr("name", ""));
    bool current = false;
    if (!id.empty() && I.fields.count(id)) {
      const std::string& fs = I.fields[id];
      current = (fs == "true" || fs == "1" || fs == "on" || fs == "yes");
    } else if (!id.empty() && I.state.count(id)) {
      current = I.state[id].truthy();
    } else if (hit->attr("value", "").size()) {
      std::string err; ExprP e = parse_expr(hit->attr("value"), err);
      Ev ev{I, host_, faults_, message_, focus_, I.fields, dirty_};
      if (err.empty() && e) current = ev.eval(*e).truthy();
    }
    bool next_val = !current;
    if (!id.empty()) {
      I.fields[id] = next_val ? "true" : "false";
      I.state[id] = Value::boolean(next_val);
      I.scope[id] = Value::boolean(next_val);
      dirty_ = true;
    }
    const std::string action = hit->attr("action", hit->attr("on", ""));
    if (!action.empty()) run_action(action);
    return;
  }
  if (hit->type == "switch" || hit->type == "toggle") {
    std::string id = hit->attr("id", hit->attr("name", ""));
    bool current = false;
    if (!id.empty() && I.fields.count(id)) {
      const std::string& fs = I.fields[id];
      current = (fs == "true" || fs == "1" || fs == "on" || fs == "yes");
    } else if (!id.empty() && I.state.count(id)) {
      current = I.state[id].truthy();
    } else if (hit->attr("value", "").size()) {
      std::string err; ExprP e = parse_expr(hit->attr("value"), err);
      Ev ev{I, host_, faults_, message_, focus_, I.fields, dirty_};
      if (err.empty() && e) current = ev.eval(*e).truthy();
    }
    bool next_val = !current;
    if (!id.empty()) {
      I.fields[id] = next_val ? "true" : "false";
      I.state[id] = Value::boolean(next_val);
      I.scope[id] = Value::boolean(next_val);
      dirty_ = true;
    }
    const std::string action = hit->attr("action", hit->attr("on", ""));
    if (!action.empty()) run_action(action);
    return;
  }
  if (hit->type == "tabs" || hit->type == "segmented") {
    std::string opts_str = hit->attr("options", hit->attr("tabs", hit->label));
    std::vector<std::string> opts;
    size_t pos = 0;
    while (pos < opts_str.size()) {
      size_t comma = opts_str.find(',', pos);
      std::string opt = (comma == std::string::npos) ? opts_str.substr(pos) : opts_str.substr(pos, comma - pos);
      opt = trim(opt);
      if (!opt.empty()) opts.push_back(opt);
      if (comma == std::string::npos) break;
      pos = comma + 1;
    }
    if (!opts.empty()) {
      for (const auto& p : placed_) {
        if (p.w == hit && p.box && p.width > 0) {
          int seg_w = p.width / (int)opts.size();
          int seg_idx = std::clamp((x - p.x) / (seg_w > 0 ? seg_w : 1), 0, (int)opts.size() - 1);
          std::string chosen = opts[(size_t)seg_idx];
          std::string id = hit->attr("id", hit->attr("name", ""));
          if (!id.empty()) {
            I.fields[id] = chosen;
            I.state[id] = Value::str(chosen);
            I.scope[id] = Value::str(chosen);
            dirty_ = true;
          }
          const std::string action = hit->attr("action", hit->attr("on", ""));
          if (!action.empty()) run_action(action);
          break;
        }
      }
    }
    return;
  }
  if (hit->type == "select" || hit->type == "dropdown") {
    std::string opts_str = hit->attr("options", hit->label);
    std::vector<std::string> opts;
    size_t pos = 0;
    while (pos < opts_str.size()) {
      size_t comma = opts_str.find(',', pos);
      std::string opt = (comma == std::string::npos) ? opts_str.substr(pos) : opts_str.substr(pos, comma - pos);
      opt = trim(opt);
      if (!opt.empty()) opts.push_back(opt);
      if (comma == std::string::npos) break;
      pos = comma + 1;
    }
    if (!opts.empty()) {
      std::string id = hit->attr("id", hit->attr("name", ""));
      std::string cur_val;
      if (!id.empty() && I.fields.count(id)) cur_val = I.fields[id];
      else if (!id.empty() && I.state.count(id)) cur_val = I.state[id].text();
      int cur_idx = 0;
      for (size_t i = 0; i < opts.size(); ++i) {
        if (opts[i] == cur_val) { cur_idx = (int)i; break; }
      }
      int next_idx = (cur_idx + 1) % (int)opts.size();
      std::string chosen = opts[(size_t)next_idx];
      if (!id.empty()) {
        I.fields[id] = chosen;
        I.state[id] = Value::str(chosen);
        I.scope[id] = Value::str(chosen);
        dirty_ = true;
      }
      const std::string action = hit->attr("action", hit->attr("on", ""));
      if (!action.empty()) run_action(action);
    }
    return;
  }
  if (hit->type == "slider") {
    for (const auto& p : placed_) {
      if (p.w == hit && p.box) {
        double min_v = 0, max_v = 100, step_v = 1;
        if (hit->attr("min", "") != "") min_v = std::atof(hit->attr("min").c_str());
        if (hit->attr("max", "") != "") max_v = std::atof(hit->attr("max").c_str());
        if (hit->attr("step", "") != "") step_v = std::atof(hit->attr("step").c_str());
        double frac = 0.0;
        if (p.width > 0) frac = std::clamp((double)(x - p.x) / (double)p.width, 0.0, 1.0);
        double raw_v = min_v + frac * (max_v - min_v);
        if (step_v > 0) raw_v = min_v + std::round((raw_v - min_v) / step_v) * step_v;
        std::string id = hit->attr("id", hit->attr("name", ""));
        if (!id.empty()) {
          I.fields[id] = fmt_num(raw_v);
          I.scope[id] = Value::num(raw_v);
          I.state[id] = Value::num(raw_v);
          dirty_ = true;
        }
        const std::string action = hit->attr("action", hit->attr("on", ""));
        if (!action.empty()) run_action(action);
        break;
      }
    }
    return;
  }
  if (hit->type == "rating" || hit->type == "stars") {
    for (const auto& p : placed_) {
      if (p.w == hit && p.box) {
        int max_stars = hit->attr_int("max", 5);
        if (max_stars < 1) max_stars = 5;
        int star_start_x = p.x;
        std::string lbl = hit->label.empty() ? hit->attr("label", "") : hit->label;
        if (!lbl.empty()) {
          star_start_x += (int)lbl.size() * 8 + 12;
        }
        int rel_x = x - star_start_x;
        int star_idx = std::clamp(rel_x / 26 + 1, 1, max_stars);
        std::string id = hit->attr("id", hit->attr("name", ""));
        if (!id.empty()) {
          I.fields[id] = std::to_string(star_idx);
          I.state[id] = Value::num(star_idx);
          I.scope[id] = Value::num(star_idx);
          dirty_ = true;
        }
        const std::string action = hit->attr("action", hit->attr("on", ""));
        if (!action.empty()) run_action(action);
        break;
      }
    }
    return;
  }
  if (hit->type == "accordion" || hit->type == "collapsible" || hit->type == "details") {
    std::string id = hit->attr("id", hit->attr("name", ""));
    bool current = false;
    if (!id.empty() && I.fields.count(id)) {
      const std::string& fs = I.fields[id];
      current = (fs == "true" || fs == "1" || fs == "open");
    } else if (!id.empty() && I.state.count(id)) {
      current = I.state[id].truthy();
    } else if (hit->attr("open", "").size()) {
      std::string err; ExprP e = parse_expr(hit->attr("open"), err);
      Ev ev{I, host_, faults_, message_, focus_, I.fields, dirty_};
      if (err.empty() && e) current = ev.eval(*e).truthy();
    }
    bool next_val = !current;
    if (!id.empty()) {
      I.fields[id] = next_val ? "true" : "false";
      I.state[id] = Value::boolean(next_val);
      I.scope[id] = Value::boolean(next_val);
      dirty_ = true;
    }
    const std::string action = hit->attr("action", hit->attr("on", ""));
    if (!action.empty()) run_action(action);
    return;
  }
  if (hit->type == "tree" || hit->type == "treeview") {
    std::string id = hit->attr("id", hit->attr("name", ""));
    for (const auto& p : placed_) {
      if (p.w == hit && p.box && p.height > 0) {
        int item_idx = std::max(0, (y - p.y) / 24);
        std::string id_val = std::to_string(item_idx);
        if (!id.empty()) {
          I.fields[id] = id_val;
          I.state[id] = Value::num(item_idx);
          I.scope[id] = Value::num(item_idx);
          dirty_ = true;
        }
        const std::string action = hit->attr("action", hit->attr("on", ""));
        if (!action.empty()) run_action(action);
        break;
      }
    }
    return;
  }
  if (hit->type == "tag" || hit->type == "tags" || hit->type == "chips") {
    std::string id = hit->attr("id", hit->attr("name", ""));
    const std::string action = hit->attr("action", hit->attr("on", ""));
    if (!action.empty()) run_action(action);
    return;
  }
  if (hit->type == "paint") {
    // A click on the drawing surface is a key press the face can hear:
    // `on key Click:` with click_x / click_y in its pixels.
    I.scope["click_x"] = Value::num(x - I.paint_x);
    I.scope["click_y"] = Value::num(y - I.paint_y);
    key("Click", true);
    key("Click", false);
  }
}

void Runtime::run_action(const std::string& action) {
  if (!loaded_) load();
  Impl& I = *impl_;
  Ev ev{I, host_, faults_, message_, focus_, I.fields, dirty_};
  bool any = action_handler_ && action_handler_(action);
  for (const auto& h : face_.handlers) {
    if (h.event != action) continue;
    any = true;
    for (const auto& s : h.body) { ev.exec(*s, paint_); if (ev.stop) break; }
  }
  if (!any) faults_.push_back("no `on " + action + ":` handler is written for that button");
  I.list_cache.clear();
}

void Runtime::type(const std::string& text) {
  Impl& I = *impl_;
  if (focus_.empty()) return;
  auto ed_it = I.editors.find(focus_);
  if (ed_it != I.editors.end()) {
    auto& ed = ed_it->second;
    if (ed.lines.empty()) ed.lines.push_back("");
    if (ed.cursor_line >= (int)ed.lines.size()) ed.cursor_line = (int)ed.lines.size() - 1;
    if (ed.cursor_col > (int)ed.lines[ed.cursor_line].size()) ed.cursor_col = (int)ed.lines[ed.cursor_line].size();
    for (char c : text) {
      if ((unsigned char)c >= 32) {
        ed.lines[ed.cursor_line].insert(ed.cursor_col, 1, c);
        ed.cursor_col++;
        ed.dirty = true;
      }
    }
    return;
  }
  std::string& v = I.fields[focus_];
  for (char c : text) if ((unsigned char)c >= 32) v += c;
}

bool Runtime::key(const std::string& name, bool down) {
  Impl& I = *impl_;
  if (down) I.keys_held.insert(name); else I.keys_held.erase(name);
  if (!down) return false;

  // A focused editor takes arrows, Enter, Backspace, Delete, Tab, Home, End, PageUp, PageDown, Escape.
  if (!focus_.empty()) {
    auto ed_it = I.editors.find(focus_);
    if (ed_it != I.editors.end()) {
      auto& ed = ed_it->second;
      if (ed.lines.empty()) ed.lines.push_back("");
      if (ed.cursor_line >= (int)ed.lines.size()) ed.cursor_line = (int)ed.lines.size() - 1;
      if (ed.cursor_col > (int)ed.lines[ed.cursor_line].size()) ed.cursor_col = (int)ed.lines[ed.cursor_line].size();

      if (name == "ArrowLeft") {
        if (ed.cursor_col > 0) ed.cursor_col--;
        else if (ed.cursor_line > 0) {
          ed.cursor_line--;
          ed.cursor_col = (int)ed.lines[ed.cursor_line].size();
        }
        return true;
      }
      if (name == "ArrowRight") {
        if (ed.cursor_col < (int)ed.lines[ed.cursor_line].size()) ed.cursor_col++;
        else if (ed.cursor_line + 1 < (int)ed.lines.size()) {
          ed.cursor_line++;
          ed.cursor_col = 0;
        }
        return true;
      }
      if (name == "ArrowUp") {
        if (ed.cursor_line > 0) {
          ed.cursor_line--;
          ed.cursor_col = std::min(ed.cursor_col, (int)ed.lines[ed.cursor_line].size());
        }
        return true;
      }
      if (name == "ArrowDown") {
        if (ed.cursor_line + 1 < (int)ed.lines.size()) {
          ed.cursor_line++;
          ed.cursor_col = std::min(ed.cursor_col, (int)ed.lines[ed.cursor_line].size());
        }
        return true;
      }
      if (name == "Home") {
        size_t non_ws = ed.lines[ed.cursor_line].find_first_not_of(" \t");
        if (non_ws == std::string::npos || ed.cursor_col == (int)non_ws) ed.cursor_col = 0;
        else ed.cursor_col = (int)non_ws;
        return true;
      }
      if (name == "End") {
        ed.cursor_col = (int)ed.lines[ed.cursor_line].size();
        return true;
      }
      if (name == "PageUp") {
        ed.cursor_line = std::max(0, ed.cursor_line - std::max(1, ed.last_view_lines - 2));
        ed.cursor_col = std::min(ed.cursor_col, (int)ed.lines[ed.cursor_line].size());
        return true;
      }
      if (name == "PageDown") {
        ed.cursor_line = std::min((int)ed.lines.size() - 1, ed.cursor_line + std::max(1, ed.last_view_lines - 2));
        ed.cursor_col = std::min(ed.cursor_col, (int)ed.lines[ed.cursor_line].size());
        return true;
      }
      if (name == "Enter") {
        std::string cur = ed.lines[ed.cursor_line];
        std::string indent;
        for (char c : cur) { if (c == ' ' || c == '\t') indent += c; else break; }
        std::string left = cur.substr(0, ed.cursor_col);
        std::string right = cur.substr(ed.cursor_col);
        ed.lines[ed.cursor_line] = left;
        ed.lines.insert(ed.lines.begin() + ed.cursor_line + 1, indent + right);
        ed.cursor_line++;
        ed.cursor_col = (int)indent.size();
        ed.dirty = true;
        return true;
      }
      if (name == "Backspace") {
        if (ed.cursor_col > 0) {
          ed.lines[ed.cursor_line].erase(ed.cursor_col - 1, 1);
          ed.cursor_col--;
          ed.dirty = true;
        } else if (ed.cursor_line > 0) {
          int prev_len = (int)ed.lines[ed.cursor_line - 1].size();
          ed.lines[ed.cursor_line - 1] += ed.lines[ed.cursor_line];
          ed.lines.erase(ed.lines.begin() + ed.cursor_line);
          ed.cursor_line--;
          ed.cursor_col = prev_len;
          ed.dirty = true;
        }
        return true;
      }
      if (name == "Delete") {
        if (ed.cursor_col < (int)ed.lines[ed.cursor_line].size()) {
          ed.lines[ed.cursor_line].erase(ed.cursor_col, 1);
          ed.dirty = true;
        } else if (ed.cursor_line + 1 < (int)ed.lines.size()) {
          ed.lines[ed.cursor_line] += ed.lines[ed.cursor_line + 1];
          ed.lines.erase(ed.lines.begin() + ed.cursor_line + 1);
          ed.dirty = true;
        }
        return true;
      }
      if (name == "Tab") {
        ed.lines[ed.cursor_line].insert(ed.cursor_col, "  ");
        ed.cursor_col += 2;
        ed.dirty = true;
        return true;
      }
      if (name == "Escape") {
        focus_.clear();
        return true;
      }
      if (name.size() == 1) {
        type(name);
        return true;
      }
    }
  }

  // A focused field takes Enter, Backspace, Tab and Escape.
  if (!focus_.empty()) {
    if (name == "Enter") {
      // the field's enter= action
      std::vector<const Widget*> stack;
      for (auto& w : face_.widgets) stack.push_back(w.get());
      std::string action;
      while (!stack.empty()) {
        const Widget* w = stack.back(); stack.pop_back();
        if ((w->type == "field" || w->type == "input") && w->attr("id", w->attr("name", "")) == focus_) action = w->attr("enter", w->attr("action", ""));
        for (auto& c : w->children) stack.push_back(c.get());
      }
      if (!action.empty()) run_action(action);
      return true;
    }
    if (name == "Backspace") {
      std::string& v = I.fields[focus_];
      if (!v.empty()) { size_t at = v.size() - 1; while (at > 0 && ((unsigned char)v[at] & 0xC0) == 0x80) --at; v.erase(at); }
      return true;
    }
    if (name == "Tab") {
      for (size_t i = 0; i < I.field_order.size(); ++i) if (I.field_order[i] == focus_) { focus_ = I.field_order[(i + 1) % I.field_order.size()]; return true; }
      return true;
    }
    if (name == "Escape") { focus_.clear(); return true; }
    if (name.size() == 1) { type(name); return true; }
    if (name == "ArrowLeft" || name == "ArrowRight" || name == "Home" || name == "End" || name == "Delete") return true;
  }
  // `on key <Name>` handlers.
  Ev ev{I, host_, faults_, message_, focus_, I.fields, dirty_};
  bool any = false;
  for (const auto& h : face_.handlers) {
    if (h.event != "key") continue;
    if (h.key != name && h.key != "any" && !(h.key == "Space" && name == " ")) continue;
    any = true;
    for (const auto& s : h.body) { ev.exec(*s, paint_); if (ev.stop) break; }
  }
  return any;
}

void Runtime::flush_storage() {
  if (!dirty_) return;
  dirty_ = false;
  if (!host_.storage_save) return;
  Value r = Value::make_rec();
  for (const auto& kv : impl_->state) r.set_field(kv.first, kv.second);
  host_.storage_save(to_json(r));
}

void Runtime::blur() {
  pane_focused_ = false;
  placed_.clear();
  paint_.clear();
  if (impl_) {
    impl_->ops.clear();
    impl_->list_cache.clear();
    impl_->xform.clear();
  }
}

void Runtime::focus() {
  pane_focused_ = true;
  rehydrate_viewport(30);
}

void Runtime::on_focus_change(bool focused) {
  if (focused) {
    focus();
  } else {
    blur();
  }
}

void Runtime::rehydrate_viewport(size_t limit) {
  // Every query() a face issues during this one frame is capped to `limit`
  // rows -- refocusing a pane costs what's on screen, not what the face's own
  // backing list holds. This used to be a bare frame() with the limit
  // silently discarded ((void)limit), which passed a test only because the
  // test's OWN face query happened to cap itself; a face that asks for
  // everything (no LIMIT in its own query()) got everything, every refocus,
  // no matter what limit was requested here.
  impl_->viewport_row_cap = limit > 0 ? limit : 0;
  const double dt = last_dt_ms() > 0.0 ? last_dt_ms() : 16.0;
  const int w = last_w_ > 0 ? last_w_ : 800;
  const int h = last_h_ > 0 ? last_h_ : 600;
  frame(dt, w, h);
  impl_->viewport_row_cap = 0;   // an ordinary frame() after this is uncapped
}

void Runtime::fault_in_scroll(size_t offset, size_t count) {
  // Scrolling further than the initial viewport: the same capped path, sized
  // to the newly visible rows, not the whole list.
  (void)offset;
  rehydrate_viewport(count);
}

std::vector<std::string> Runtime::as_text() const {
  // Group placed text by row (y), left to right.
  std::vector<const Placed*> ts;
  for (const auto& p : placed_) if (!p.text.empty()) ts.push_back(&p);
  std::stable_sort(ts.begin(), ts.end(), [](const Placed* a, const Placed* b) { return a->y != b->y ? a->y < b->y : a->x < b->x; });
  std::vector<std::string> lines;
  int row_y = -1000;
  std::string line;
  int col = 0;
  for (const Placed* p : ts) {
    if (p->y > row_y + 8) {
      if (!line.empty()) lines.push_back(line);
      line.clear(); col = 0; row_y = p->y;
    }
    int want = p->x / 8;
    if (want > col + 1) { line += std::string((size_t)(want - col), ' '); col = want; }
    else if (!line.empty()) { line += "  "; col += 2; }
    line += p->text;
    col += (int)p->text.size();
  }
  if (!line.empty()) lines.push_back(line);
  return lines;
}

std::vector<std::pair<std::string, size_t>> Runtime::reads() const {
  std::vector<std::pair<std::string, size_t>> out;
  for (const auto& [kind, n] : impl_->reads) out.emplace_back(kind, n);
  return out;
}

bool Runtime::has_frame_rule() const {
  for (const auto& h : face_.handlers) if (h.event == "frame") return true;
  return false;
}

std::vector<std::string> Runtime::inspect() const {
  std::vector<std::string> out;
  for (const auto& e : face_.errors) out.push_back("NOT UNDERSTOOD: " + e);
  for (const auto& f : faults_) out.push_back("FAULT: " + f);
  int cuts = 0;
  for (const auto& p : placed_) {
    if (!p.cut) continue;
    if (cuts < 8) out.push_back("CUT TO FIT: '" + (p.text.size() > 40 ? p.text.substr(0, 37) + "…" : p.text) + "' did not fit its " + std::to_string(p.width) + "px at x=" + std::to_string(p.x) + " — give it more room or shorter text");
    ++cuts;
  }
  if (cuts > 8) out.push_back("… and " + std::to_string(cuts - 8) + " more cut texts");
  std::string gaps;
  for (const auto& p : placed_) {
    const std::string g = display_glyph_gaps(p.text);
    if (!g.empty() && gaps.find(g) == std::string::npos) { if (!gaps.empty()) gaps += ' '; gaps += g; }
  }
  if (!gaps.empty()) out.push_back("GLYPHS THIS DISPLAY CANNOT DRAW: " + gaps + " — use words or ASCII");
  int over = 0;
  for (const auto& p : placed_) if (p.x + p.width > last_w_ + 2 || p.y + p.height > last_h_ + 2) ++over;
  if (over) out.push_back("OFF THE PANE: " + std::to_string(over) + " text run(s) fall outside the " + std::to_string(last_w_) + "x" + std::to_string(last_h_) + " pane — the face is taller or wider than the glass");
  if (placed_.empty() && paint_.empty()) out.push_back("EMPTY: the face drew nothing");
  // A face that reads records and gets none back is showing a person an empty
  // shell. It is not always wrong — a dream can be the first thing to keep a
  // kind — so this says exactly what was read and leaves the judgement to the
  // step that knows what this machine actually holds.
  if (!impl_->reads.empty()) {
    size_t rows = 0;
    std::string named;
    for (const auto& [kind, n] : impl_->reads) {
      rows += n;
      if (!named.empty()) named += ", ";
      named += kind + " (" + std::to_string(n) + ")";
    }
    if (rows == 0) out.push_back("NO RECORDS: this face read " + named +
                                 " and every one came back empty — nothing on it came from this machine");
    else out.push_back("READ: " + named);
  }
  return out;
}

std::vector<std::pair<std::string, std::string>> Runtime::state_vars() const {
  std::vector<std::pair<std::string, std::string>> out;
  if (!impl_) return out;
  for (const auto& [k, v] : impl_->state) {
    out.emplace_back(k, v.text());
  }
  return out;
}

std::vector<std::pair<std::string, std::string>> Runtime::let_vars() const {
  std::vector<std::pair<std::string, std::string>> out;
  if (!impl_) return out;
  for (const auto& [k, v] : impl_->lets) {
    out.emplace_back(k, v.text());
  }
  return out;
}

std::vector<std::pair<std::string, std::string>> Runtime::field_vars() const {
  std::vector<std::pair<std::string, std::string>> out;
  if (!impl_) return out;
  for (const auto& [k, v] : impl_->fields) {
    out.emplace_back(k, v);
  }
  return out;
}

std::vector<std::pair<std::string, size_t>> Runtime::kind_vars() const {
  std::vector<std::pair<std::string, size_t>> out;
  if (!impl_) return out;
  for (const auto& [k, count] : impl_->reads) {
    out.emplace_back(k, count);
  }
  return out;
}

void Runtime::set_state_var(const std::string& name, const std::string& val) {
  if (!impl_) return;
  // Try parsing as number
  char* end = nullptr;
  double d = std::strtod(val.c_str(), &end);
  if (end != val.c_str() && *end == '\0') {
    impl_->state[name] = Value::num(d);
  } else if (val == "true" || val == "false") {
    impl_->state[name] = Value::boolean(val == "true");
  } else {
    impl_->state[name] = Value::str(val);
  }
  dirty_ = true;
}

void Runtime::set_field_var(const std::string& name, const std::string& val) {
  if (!impl_) return;
  impl_->fields[name] = val;
  dirty_ = true;
}

int Runtime::frame_count() const {
  return impl_ ? impl_->frame_no : 0;
}

double Runtime::last_dt_ms() const {
  return impl_ ? impl_->dt_ms : 0;
}

void Runtime::mouse_move(int x, int y, int dx, int dy, uint32_t buttons) {
  if (!impl_) return;
  impl_->mouse_x = x;
  impl_->mouse_y = y;
  impl_->mouse_dx = dx;
  impl_->mouse_dy = dy;
  impl_->mouse_buttons = buttons;
  for (const auto& p : placed_) {
    if (p.box && x >= p.x && x < p.x + p.width && y >= p.y && y < p.y + p.height) {
      impl_->hovered_widget = (p.w ? p.w->attr("id", p.w->type) : "");
      break;
    }
  }
}

void Runtime::mouse_drag(int x, int y, int dx, int dy) {
  if (!impl_) return;
  impl_->mouse_x = x;
  impl_->mouse_y = y;
  impl_->mouse_dx = dx;
  impl_->mouse_dy = dy;
  for (const auto& h : face_.handlers) {
    if (h.event == "drag" || h.event == "mouse_drag") {
      impl_->scope["drag_x"] = Value::num((double)x);
      impl_->scope["drag_y"] = Value::num((double)y);
      impl_->scope["drag_dx"] = Value::num((double)dx);
      impl_->scope["drag_dy"] = Value::num((double)dy);
      Ev ev{*impl_, host_, faults_, message_, focus_, impl_->fields, dirty_};
      for (const auto& s : h.body) ev.exec(*s, paint_);
    }
  }
}

void Runtime::mouse_wheel(int dx, int dy) {
  if (!impl_) return;
  for (const auto& h : face_.handlers) {
    if (h.event == "wheel" || h.event == "scroll") {
      impl_->scope["wheel_dx"] = Value::num((double)dx);
      impl_->scope["wheel_dy"] = Value::num((double)dy);
      Ev ev{*impl_, host_, faults_, message_, focus_, impl_->fields, dirty_};
      for (const auto& s : h.body) ev.exec(*s, paint_);
    }
  }
}

void Runtime::gesture(const std::string& type, double val1, double val2) {
  if (!impl_) return;
  for (const auto& h : face_.handlers) {
    if (h.event == "gesture" || h.event == type) {
      impl_->scope["gesture_type"] = Value::str(type);
      impl_->scope["gesture_val1"] = Value::num(val1);
      impl_->scope["gesture_val2"] = Value::num(val2);
      Ev ev{*impl_, host_, faults_, message_, focus_, impl_->fields, dirty_};
      for (const auto& s : h.body) ev.exec(*s, paint_);
    }
  }
}

void Runtime::emit_channel(const std::string& chan, Value val) {
  if (!impl_) return;
  impl_->get_channel(chan)->emit(std::move(val));
}

Value Runtime::poll_channel(const std::string& chan) {
  if (!impl_) return Value::null();
  return impl_->get_channel(chan)->poll();
}

Value Runtime::drain_channel(const std::string& chan) {
  if (!impl_) return Value::make_list();
  return impl_->get_channel(chan)->drain();
}

bool Runtime::has_channel_data(const std::string& chan) const {
  if (!impl_) return false;
  return impl_->get_channel(chan)->has_data();
}

void Runtime::snapshot_state(const std::string& label) {
  if (!impl_) return;
  StateSnapshot snap;
  snap.label = label;
  snap.timestamp = impl_->now_ms;
  snap.state = impl_->state;
  snap.fields = impl_->fields;
  if (impl_->undo_stack.size() >= impl_->max_history) impl_->undo_stack.erase(impl_->undo_stack.begin());
  impl_->undo_stack.push_back(snap);
  impl_->redo_stack.clear();
}

bool Runtime::undo_state() {
  if (!impl_ || impl_->undo_stack.empty()) return false;
  StateSnapshot curr;
  curr.timestamp = impl_->now_ms;
  curr.state = impl_->state;
  curr.fields = impl_->fields;
  impl_->redo_stack.push_back(curr);
  StateSnapshot snap = impl_->undo_stack.back();
  impl_->undo_stack.pop_back();
  impl_->state = snap.state;
  impl_->fields = snap.fields;
  dirty_ = true;
  return true;
}

bool Runtime::redo_state() {
  if (!impl_ || impl_->redo_stack.empty()) return false;
  StateSnapshot curr;
  curr.timestamp = impl_->now_ms;
  curr.state = impl_->state;
  curr.fields = impl_->fields;
  impl_->undo_stack.push_back(curr);
  StateSnapshot snap = impl_->redo_stack.back();
  impl_->redo_stack.pop_back();
  impl_->state = snap.state;
  impl_->fields = snap.fields;
  dirty_ = true;
  return true;
}

bool Runtime::can_undo() const {
  return impl_ && !impl_->undo_stack.empty();
}

bool Runtime::can_redo() const {
  return impl_ && !impl_->redo_stack.empty();
}

size_t Runtime::history_len() const {
  return impl_ ? impl_->undo_stack.size() : 0;
}

void Runtime::history_clear() {
  if (!impl_) return;
  impl_->undo_stack.clear();
  impl_->redo_stack.clear();
}

std::string Runtime::perceive_tree_json() const {
  std::string json = "{\n";
  json += "  \"face\": " + json_quote(face_.name) + ",\n";
  json += "  \"title\": " + json_quote(face_.title) + ",\n";
  json += "  \"dimensions\": {\"width\": " + std::to_string(last_w_) + ", \"height\": " + std::to_string(last_h_) + "},\n";
  json += "  \"focus\": " + json_quote(focus_) + ",\n";
  json += "  \"frame\": " + std::to_string(frame_count()) + ",\n";
  json += "  \"nodes\": [\n";
  for (size_t i = 0; i < placed_.size(); ++i) {
    const auto& p = placed_[i];
    json += "    {\n";
    json += "      \"id\": \"node_" + std::to_string(i) + "\",\n";
    json += "      \"kind\": " + json_quote(p.w ? p.w->type : "text") + ",\n";
    json += "      \"role\": " + json_quote(p.w ? p.w->type : "text") + ",\n";
    json += "      \"bounds\": {\"x\": " + std::to_string(p.x) + ", \"y\": " + std::to_string(p.y) +
            ", \"width\": " + std::to_string(p.width) + ", \"height\": " + std::to_string(p.height) + "},\n";
    json += "      \"text\": " + json_quote(p.text) + ",\n";
    json += "      \"cut\": " + std::string(p.cut ? "true" : "false") + ",\n";
    json += "      \"interactive\": " + std::string(p.box ? "true" : "false") + "\n";
    json += "    }" + std::string(i + 1 < placed_.size() ? "," : "") + "\n";
  }
  json += "  ],\n";
  json += "  \"state\": {\n";
  if (impl_) {
    size_t si = 0;
    for (const auto& [k, v] : impl_->state) {
      json += "    " + json_quote(k) + ": " + to_json(v) + (si + 1 < impl_->state.size() ? ",\n" : "\n");
      si++;
    }
  }
  json += "  },\n";
  json += "  \"history\": {\"can_undo\": " + std::string(can_undo() ? "true" : "false") +
          ", \"can_redo\": " + std::string(can_redo() ? "true" : "false") +
          ", \"depth\": " + std::to_string(history_len()) + "}\n";
  json += "}";
  return json;
}

std::vector<std::string> Runtime::lint() const {
  std::vector<std::string> diagnostics;
  for (const auto& p : placed_) {
    if (p.cut) {
      diagnostics.push_back("LINT-OVERFLOW: Text '" + p.text + "' was clipped to fit width " + std::to_string(p.width));
    }
  }
  for (const auto& t : face_.types) {
    if (t.fields.empty()) {
      diagnostics.push_back("LINT-SCHEMA: Type '" + t.name + "' has no fields defined.");
    }
  }
  for (const auto& c : face_.components) {
    if (c.body.empty()) {
      diagnostics.push_back("LINT-COMPONENT: Component '" + c.name + "' has empty body.");
    }
  }
  return diagnostics;
}

bool Runtime::hot_patch(const std::string& new_source) {
  Face new_face = parse_face(new_source);
  if (!new_face.errors.empty()) {
    for (const auto& err : new_face.errors) faults_.push_back("hot_patch: " + err);
    return false;
  }
  adopt(std::move(new_face));
  dirty_ = true;
  return true;
}

void Runtime::adopt(Face f) {
  face_ = std::move(f);
  placed_.clear();   // it points into the old tree; the next frame lays out the new one
  if (!impl_) return;
  for (const auto& kk : face_.kinds) impl_->kind_keys[kk.first] = kk.second;
  impl_->types = face_.types;
  Ev ev{*impl_, host_, faults_, message_, focus_, impl_->fields, dirty_};
  for (const auto& l : face_.lets) impl_->lets[l.first] = l.second ? ev.eval(*l.second) : Value::null();
  if (!loaded_) return;   // load() seeds the fields of the first document
  // A field the new document brings starts at its value=; one it kept keeps
  // what was typed. Tab follows the new document's order.
  impl_->field_order.clear();
  std::vector<const Widget*> stack;
  for (auto it = face_.widgets.rbegin(); it != face_.widgets.rend(); ++it) stack.push_back(it->get());
  while (!stack.empty()) {
    const Widget* w = stack.back();
    stack.pop_back();
    if (w->type == "field" || w->type == "input") {
      const std::string id = w->attr("id", w->attr("name", ""));
      if (!id.empty()) {
        if (!impl_->fields.count(id)) {
          std::string init_val = w->attr("value", "");
          for (const auto& kv : w->exprs) if (kv.first == "value" && kv.second) init_val = ev.eval(*kv.second).text();
          impl_->fields[id] = init_val;
          if (w->flag("focus") && focus_.empty()) focus_ = id;
        }
        impl_->field_order.push_back(id);
      }
    }
    for (auto it = w->children.rbegin(); it != w->children.rend(); ++it) stack.push_back(it->get());
  }
}

void Runtime::provide(const std::string& name, Value v) { impl_->given[name] = std::move(v); }

void Runtime::set_action_handler(std::function<bool(const std::string& action)> handler) {
  action_handler_ = std::move(handler);
}

const char* face_language_reference() {
  return
    "THE FACE LANGUAGE — General-Purpose Application & Dream Specification for AIOS.\n"
    "A face is a native document: declarative headers, reactive bindings, async tasks, schemas, reusable components, and an indented tree of widgets. The compositor evaluates, solves constraints, and rasterizes directly every frame from the machine's state, memory fabric, and store. Indent children by two spaces. '#' starts a comment.\n"
    "\n"
    "HEADER & TOP-LEVEL DECLARATIONS\n"
    "  face <name>                    one lower-case word or hyphenated\n"
    "  title \"…\"                       drawn at the top (omit for a full-pane game/canvas)\n"
    "  import \"<path>\"                import reusable components, types, and widgets from another .aos file\n"
    "  component Name(p1, p2=def):    user-defined parameterized sub-tree widget component\n"
    "  type Name { f1: Type, … }      structural schema definition with contract validation\n"
    "  let <name> = <expr>            REACTIVE: recomputed every frame, in order (e.g. let items = list(item))\n"
    "  state <name> = <expr>          PERSISTENT: its initial value; changed with `set`; kept across reboots\n"
    "  kind <name> key=<field>        the kind has IDENTITY: list(<name>) is newest record per key; put replaces row\n"
    "\n"
    "RULES & HANDLERS\n"
    "  on load: …                     executed once upon initialization after state restoration\n"
    "  on frame: …                    executed every tick; dt (ms), now (ms), width/height, keys held\n"
    "  on key <Key>: …                keyboard bindings: ArrowLeft ArrowRight ArrowUp ArrowDown Enter Escape Space Ctrl+Z Ctrl+Y\n"
    "  on <action>: …                 invoked by button clicks, slider inputs, or field Enter keys\n"
    "  on drag: … / on wheel: …       pointer interactions; drag_x, drag_y, drag_dx, drag_dy, wheel_dx, wheel_dy\n"
    "  statements:\n"
    "    set <name> = <expr>          a state, a let, a field or a variable (also `+=`, `-=`, `*=`, `/=`)\n"
    "    toggle <name>                toggles boolean state or field\n"
    "    inc <name> [by] / dec <name> increments or decrements numeric variable\n"
    "    put <kind> {f: expr, …}      commit record to AIOS append-only cryptographic provenance store\n"
    "    update <kind> <id> {f: …}    update fields of a record in the store\n"
    "    delete <kind> <id>           remove record from store\n"
    "    spawn task <name>: …         launches non-blocking background async task / daemon\n"
    "    emit <expr> to <channel>     emits message to typed channel / reactive stream\n"
    "    undo / redo / snapshot       temporal state time-travel operations\n"
    "    clear <field>   focus <field>   say <expr>   notify <expr>   stop\n"
    "    if <cond>: …  /  else: …  (also `if c: a; b` inline)\n"
    "    for <x> in <list>: …         `index` is the loop position\n"
    "    paint <verb> [args...]       vector drawing verbs (rect, outline, circle, ellipse, poly, curve, line, grad, tensor, buffer, push, pop, translate, rotate, scale)\n"
    "\n"
    "COMPUTE, MATH & BUILT-IN FUNCTIONS\n"
    "  matmul(A, B)                   high-performance 2D matrix multiplication\n"
    "  softmax(V)                     numerically stable softmax probability distribution over vector\n"
    "  reduce(V, op)                  tensor reduction: sum, mean, max, min, prod, l2\n"
    "  embed(text [, model])          128-dimensional deterministic normalized embedding vector\n"
    "  dot(A, B) / cosine_sim(A, B)   dot product and cosine similarity between vectors\n"
    "  spring(cur, tgt, vel, k, c, dt) 2nd order mass-spring-damper physics solver -> {pos, vel, value}\n"
    "  decay(val, vel, f, dt) / bounce physics decay and restitution rebound\n"
    "  emit(chan, val) / recv(chan) / poll(chan) / drain(chan) / has_data(chan) / chan_len(chan) typed channels\n"
    "  validate(rec, type) / is_valid schema validation against declared structural types\n"
    "  history() / snapshot(lbl) / undo() / redo() state snapshotting and undo/redo\n"
    "  list(kind)  count(x)  sum(l, f)  avg  min  max  where(l, f, v)  find(l, f, v)  sort(l, f [, desc])  unique(l, f)\n"
    "  map(l, f)  flat(l)  push(l, x)  take(l, n)  first(l)  last(l)  reverse(l)  similar(t, k)  join(l, f, sep)\n"
    "  min_by(l, f)  max_by(l, f)  group_by(l, f)  sum_by(l, f)  avg_by(l, f)  range(start, end [, step])\n"
    "  format_bytes(b)  format_currency(n [, sym])  uuid()  keys(r)  values(r)  entries(r)  merge(r1, r2)\n"
    "  now()  time(ms)  date(ms)  datetime(ms)  ago(ms)  read_file(p)  write_file(p, d)  list_dir(p)  dream(w)\n"
    "\n"
    "WIDGETS & LAYOUT SUITE\n"
    "  row [gap=10] [align=center] / column [gap=10] / grid 2fr 1fr [gap=12] / panel \"Title\" [pad=14]\n"
    "  toolbar [gap=8] / text \"…{expr}…\" [size=15] [bold] [dim] [color=…] / heading \"…\" / label \"…\"\n"
    "  tile label=\"Label\" value=val [accent=ok|warn|bad|accent] / table <list> columns=\"…\" [rows=12]\n"
    "  list <list> \"template\" [rows=10] / field id=<name> [placeholder=\"…\"] [enter=<action>] [focus]\n"
    "  editor id=<name> [path=\"…\"] [height=400|fill] [line_numbers] [focus] / button \"Label\" action=<name>\n"
    "  checkbox label=\"…\" id=<name> action=<name> / switch label=\"…\" id=<name> action=<name>\n"
    "  slider id=<name> min=0 max=100 step=1 action=<name> / rating id=<name> max=5 action=<name>\n"
    "  accordion title=\"…\" id=<name> [open] / tree id=<name> / tags options=\"…\" / tabs id=<name> options=\"…\"\n"
    "  progress value=<expr> max=100 / select id=<name> options=\"…\" / badge text=\"…\" [dot] / sparkline data=<list>\n"
    "  scrubber id=<name> min=0 max=100 action=<name> / modal id=<name> title=\"…\" [show=<expr>] / gauge value=<expr>\n"
    "  scroll [height=200] / paint [height=320|fill] / image src=\"…\"\n";
}

}  // namespace aios::userland::face

