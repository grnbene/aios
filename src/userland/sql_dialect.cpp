// AIOS — Native SQL Dialect Translation Engine implementation.
//
// MariaDB/MySQL in, PostgreSQL or T-SQL out. The statement is read as tokens
// first: string literals (MySQL's '...' and "...", backslash escapes decoded),
// quoted names (`...`), numbers, hex literals, words and operators. Only words
// and operators are ever rewritten; a literal is carried as its value and
// written in the target's own syntax. Statements are then rewritten by kind -
// CREATE TABLE's types and options, INSERT's upsert forms, LIMIT - and every
// expression's MySQL functions. Anything that cannot be written faithfully is
// an error, not a guess: a write-behind mutation the downstream would apply
// differently is worse than one it refuses.
#include "aios/userland/sql_dialect.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>

namespace aios::userland {

namespace {

struct Tk {
  enum K { Word, QId, Str, Num, Hex, Op, Var, Raw } k = Raw;
  std::string text;   // Word/Num/Op/Var/Raw as written; QId the name; Str the value; Hex the hex digits
};
using Toks = std::vector<Tk>;

std::string upper(std::string s) {
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}
std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
bool is_word_char(unsigned char c) { return std::isalnum(c) || c == '_' || c == '$' || c >= 0x80; }

Tk raw(const std::string& s) { return Tk{Tk::Raw, s}; }
Tk op(const std::string& s) { return Tk{Tk::Op, s}; }
Tk word(const std::string& s) { return Tk{Tk::Word, s}; }

// MySQL's lexical rules (default sql_mode: "..." is a string, \ escapes in strings).
Result<Toks> lex_mysql(const std::string& s) {
  Toks out;
  size_t i = 0;
  const size_t n = s.size();
  while (i < n) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (std::isspace(c)) { ++i; continue; }
    if (c == '#' || (c == '-' && i + 1 < n && s[i + 1] == '-' && (i + 2 >= n || std::isspace(static_cast<unsigned char>(s[i + 2]))))) {
      while (i < n && s[i] != '\n') ++i;
      continue;
    }
    if (c == '/' && i + 1 < n && s[i + 1] == '*') {
      const size_t e = s.find("*/", i + 2);
      if (e == std::string::npos) return Result<Toks>(Status::Invalid, "an unclosed /* comment");
      // /*!50001 ... */ is MySQL-only SQL: none of it means anything elsewhere.
      i = e + 2;
      continue;
    }
    if (c == '\'' || c == '"') {
      std::string v;
      size_t j = i + 1;
      bool closed = false;
      while (j < n) {
        const char d = s[j];
        if (d == '\\' && j + 1 < n) {
          const char e = s[j + 1];
          switch (e) {
            case '0': v.push_back('\0'); break;
            case 'b': v.push_back('\b'); break;
            case 'n': v.push_back('\n'); break;
            case 'r': v.push_back('\r'); break;
            case 't': v.push_back('\t'); break;
            case 'Z': v.push_back('\x1A'); break;
            case '%': v += "\\%"; break;   // kept as written: LIKE's escaped wildcards
            case '_': v += "\\_"; break;
            default: v.push_back(e); break;
          }
          j += 2;
          continue;
        }
        if (d == static_cast<char>(c)) {
          if (j + 1 < n && s[j + 1] == static_cast<char>(c)) { v.push_back(d); j += 2; continue; }
          closed = true;
          ++j;
          break;
        }
        v.push_back(d);
        ++j;
      }
      if (!closed) return Result<Toks>(Status::Invalid, "an unclosed string literal");
      out.push_back(Tk{Tk::Str, v});
      i = j;
      continue;
    }
    if (c == '`') {
      std::string v;
      size_t j = i + 1;
      bool closed = false;
      while (j < n) {
        if (s[j] == '`') {
          if (j + 1 < n && s[j + 1] == '`') { v.push_back('`'); j += 2; continue; }
          closed = true;
          ++j;
          break;
        }
        v.push_back(s[j++]);
      }
      if (!closed) return Result<Toks>(Status::Invalid, "an unclosed `name`");
      out.push_back(Tk{Tk::QId, v});
      i = j;
      continue;
    }
    // X'0aff' and 0x0aff
    if ((c == 'x' || c == 'X') && i + 1 < n && s[i + 1] == '\'') {
      const size_t e = s.find('\'', i + 2);
      if (e == std::string::npos) return Result<Toks>(Status::Invalid, "an unclosed X'...' literal");
      out.push_back(Tk{Tk::Hex, s.substr(i + 2, e - i - 2)});
      i = e + 1;
      continue;
    }
    if (c == '0' && i + 2 < n && (s[i + 1] == 'x' || s[i + 1] == 'X') && std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
      size_t j = i + 2;
      while (j < n && std::isxdigit(static_cast<unsigned char>(s[j]))) ++j;
      if (j >= n || !is_word_char(static_cast<unsigned char>(s[j]))) {
        out.push_back(Tk{Tk::Hex, s.substr(i + 2, j - i - 2)});
        i = j;
        continue;
      }
    }
    if (std::isdigit(c) || (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
      size_t j = i;
      while (j < n && (std::isdigit(static_cast<unsigned char>(s[j])) || s[j] == '.')) ++j;
      if (j < n && (s[j] == 'e' || s[j] == 'E') && j + 1 < n &&
          (std::isdigit(static_cast<unsigned char>(s[j + 1])) || ((s[j + 1] == '-' || s[j + 1] == '+') && j + 2 < n))) {
        j += 2;
        while (j < n && std::isdigit(static_cast<unsigned char>(s[j]))) ++j;
      }
      if (j >= n || !is_word_char(static_cast<unsigned char>(s[j]))) {
        out.push_back(Tk{Tk::Num, s.substr(i, j - i)});
        i = j;
        continue;
      }
    }
    if (is_word_char(c)) {
      size_t j = i;
      while (j < n && is_word_char(static_cast<unsigned char>(s[j]))) ++j;
      out.push_back(Tk{Tk::Word, s.substr(i, j - i)});
      i = j;
      continue;
    }
    if (c == '@') {
      size_t j = i + 1;
      while (j < n && (is_word_char(static_cast<unsigned char>(s[j])) || s[j] == '@' || s[j] == '.')) ++j;
      out.push_back(Tk{Tk::Var, s.substr(i, j - i)});
      i = j;
      continue;
    }
    static const char* ops3[] = {"<=>"};
    static const char* ops2[] = {"<=", ">=", "<>", "!=", "||", "&&", ":=", "<<", ">>", "->"};
    bool hit = false;
    for (const char* o : ops3) if (s.compare(i, 3, o) == 0) { out.push_back(op(o)); i += 3; hit = true; break; }
    if (hit) continue;
    for (const char* o : ops2) if (s.compare(i, 2, o) == 0) { out.push_back(op(o)); i += 2; hit = true; break; }
    if (hit) continue;
    out.push_back(op(std::string(1, static_cast<char>(c))));
    ++i;
  }
  while (!out.empty() && out.back().k == Tk::Op && out.back().text == ";") out.pop_back();
  return out;
}

bool is_op(const Toks& t, size_t i, const char* o) { return i < t.size() && t[i].k == Tk::Op && t[i].text == o; }
bool is_w(const Toks& t, size_t i, const char* w) {
  if (i >= t.size() || t[i].k != Tk::Word) return false;
  const std::string& x = t[i].text;
  const size_t n = std::strlen(w);
  if (x.size() != n) return false;
  for (size_t k = 0; k < n; ++k) if (std::toupper(static_cast<unsigned char>(x[k])) != w[k]) return false;
  return true;
}
// The ')' that closes the '(' at i; t.size() if none.
size_t close_of(const Toks& t, size_t i) {
  int depth = 0;
  for (size_t k = i; k < t.size(); ++k) {
    if (is_op(t, k, "(")) ++depth;
    else if (is_op(t, k, ")") && --depth == 0) return k;
  }
  return t.size();
}
// Split [a, b) at top-level commas.
std::vector<Toks> split_commas(const Toks& t, size_t a, size_t b) {
  std::vector<Toks> parts(1);
  int depth = 0;
  for (size_t k = a; k < b; ++k) {
    if (is_op(t, k, "(")) ++depth;
    if (is_op(t, k, ")")) --depth;
    if (depth == 0 && is_op(t, k, ",")) { parts.emplace_back(); continue; }
    parts.back().push_back(t[k]);
  }
  if (parts.size() == 1 && parts[0].empty()) parts.clear();
  return parts;
}
Toks slice(const Toks& t, size_t a, size_t b) { return Toks(t.begin() + long(std::min(a, t.size())), t.begin() + long(std::min(b, t.size()))); }
void append(Toks* to, const Toks& from) { to->insert(to->end(), from.begin(), from.end()); }

class Tx {
 public:
  Tx(SqlDialect target, const SqlDialectTranslator::TableLookup& lookup) : pg_(target == SqlDialect::Postgres), lookup_(lookup) {}

  Result<std::string> run(Toks t);

 private:
  bool pg_;
  const SqlDialectTranslator::TableLookup& lookup_;
  std::string err_;

  bool fail(const std::string& why) { if (err_.empty()) err_ = why; return false; }
  std::string ident(const std::string& name) const { return SqlDialectTranslator::quote_identifier(name, pg_ ? SqlDialect::Postgres : SqlDialect::MsSql); }
  std::string str_lit(const std::string& v) {
    if (v.find('\0') != std::string::npos && pg_) { fail("a string with a NUL byte in it (PostgreSQL text cannot hold one)"); return "''"; }
    bool wide = false;
    std::string o = "'";
    for (char c : v) { if (c == '\'') o += '\''; o += c; if (static_cast<unsigned char>(c) >= 0x80) wide = true; }
    o += "'";
    return (!pg_ && wide) ? "N" + o : o;   // T-SQL: without N the text goes through the database's code page
  }
  std::string emit(const Toks& t);
  Toks expr(const Toks& t);   // MySQL functions and operators in an expression
  bool interval(const Toks& t, size_t i, std::string* amount, std::string* unit, size_t* next);
  Result<std::string> create_table(const Toks& t);
  Result<std::string> insert(const Toks& t);
  Result<std::string> alter_table(const Toks& t);
  Toks column_def(const Toks& def, bool* is_identity, const std::string& table);
  std::string map_type(const Toks& def, size_t* pos, bool* is_bool);
  Toks paginate(Toks t);
  bool table_info(const std::string& name, SqlDialectTranslator::TableInfo* info) {
    return lookup_ && lookup_(name, info);
  }
};

std::string Tx::emit(const Toks& t) {
  std::string o;
  auto tight_before = [&](const Tk& x) { return x.k == Tk::Op && (x.text == "," || x.text == ")" || x.text == "." || x.text == ";"); };
  auto tight_after = [&](const Tk& x) { return x.k == Tk::Op && (x.text == "(" || x.text == "."); };
  for (size_t i = 0; i < t.size(); ++i) {
    const Tk& x = t[i];
    if (i > 0 && !tight_before(x) && !tight_after(t[i - 1]) && !(is_op(t, i, "(") && (t[i - 1].k == Tk::Word || t[i - 1].k == Tk::QId)))
      o += ' ';
    switch (x.k) {
      case Tk::QId: o += ident(x.text); break;
      case Tk::Str: o += str_lit(x.text); break;
      case Tk::Hex: o += pg_ ? "'\\x" + lower(x.text) + "'::bytea" : "0x" + x.text; break;
      default: o += x.text; break;
    }
  }
  return o;
}

// INTERVAL <n> <unit> at i: the amount as text and the unit.
bool Tx::interval(const Toks& t, size_t i, std::string* amount, std::string* unit, size_t* next) {
  if (!is_w(t, i, "INTERVAL")) return false;
  size_t k = i + 1;
  Toks amt;
  if (is_op(t, k, "-")) { amt.push_back(op("-")); ++k; }
  if (k < t.size() && (t[k].k == Tk::Num || t[k].k == Tk::Str)) {
    amt.push_back(t[k].k == Tk::Str ? Tk{Tk::Num, t[k].text} : t[k]);
    ++k;
  } else if (is_op(t, k, "(")) {
    const size_t c = close_of(t, k);
    append(&amt, expr(slice(t, k, c + 1)));
    k = c + 1;
  } else if (k < t.size() && (t[k].k == Tk::Word || t[k].k == Tk::QId)) {
    amt.push_back(t[k++]);
  } else {
    return false;
  }
  if (k >= t.size() || t[k].k != Tk::Word) return false;
  std::string u = upper(t[k].text);
  static const char* units[] = {"MICROSECOND", "SECOND", "MINUTE", "HOUR", "DAY", "WEEK", "MONTH", "QUARTER", "YEAR"};
  bool known = false;
  for (const char* x : units) if (u == x) known = true;
  if (!known) { fail("INTERVAL " + u + " (only single units: SECOND ... YEAR)"); return false; }
  *amount = emit(amt);
  *unit = u;
  *next = k + 1;
  return true;
}

Toks Tx::expr(const Toks& t) {
  Toks o;
  for (size_t i = 0; i < t.size(); ++i) {
    const Tk& x = t[i];
    if (x.k == Tk::Op) {
      if (x.text == "||") { o.push_back(word("OR")); continue; }
      if (x.text == "&&") { o.push_back(word("AND")); continue; }
      if (x.text == "<=>") {
        if (pg_) { o.push_back(raw("IS NOT DISTINCT FROM")); continue; }
        fail("the <=> operator (T-SQL has no NULL-safe equality)");
        continue;
      }
      if (x.text == ":=") { fail("an @variable := assignment"); continue; }
    }
    if (x.k == Tk::Var) { fail("a MySQL variable (" + x.text + ")"); continue; }
    if (x.k != Tk::Word) { o.push_back(x); continue; }
    const std::string u = upper(x.text);
    if (u == "TRUE" || u == "FALSE") { o.push_back(Tk{Tk::Num, u == "TRUE" ? "1" : "0"}); continue; }   // MySQL's TRUE is the integer 1
    if (u == "INTERVAL") {
      std::string amount, unit;
      size_t next = 0;
      if (interval(t, i, &amount, &unit, &next)) {
        if (!pg_) { fail("INTERVAL arithmetic outside DATE_ADD/DATE_SUB (T-SQL has DATEADD only)"); return o; }
        o.push_back(raw("(" + amount + ") * INTERVAL '1 " + lower(unit) + "'"));
        i = next - 1;
        continue;
      }
    }
    if (u == "CURRENT_TIMESTAMP" || u == "CURRENT_DATE" || u == "LOCALTIME" || u == "LOCALTIMESTAMP" || u == "CURRENT_TIME") {
      if (!is_op(t, i + 1, "(")) {   // without parentheses: the same function
        if (u == "CURRENT_TIMESTAMP" || u == "LOCALTIMESTAMP" || u == "LOCALTIME") o.push_back(raw(pg_ ? "LOCALTIMESTAMP" : "SYSDATETIME()"));
        else if (u == "CURRENT_DATE") o.push_back(raw(pg_ ? "CURRENT_DATE" : "CAST(GETDATE() AS DATE)"));
        else o.push_back(raw(pg_ ? "LOCALTIME" : "CAST(GETDATE() AS TIME)"));
        continue;
      }
    }
    if (!is_op(t, i + 1, "(")) { o.push_back(x); continue; }
    // A function call: its arguments, each translated.
    const size_t c = close_of(t, i + 1);
    if (c >= t.size()) { o.push_back(x); continue; }
    std::vector<Toks> args = split_commas(t, i + 2, c);
    if ((u == "DATE_ADD" || u == "DATE_SUB" || u == "ADDDATE" || u == "SUBDATE") && args.size() == 2) {
      // Read before the arguments are translated: the second is INTERVAL n UNIT, not an expression.
      std::string amount, unit;
      size_t next = 0;
      const bool sub = u == "DATE_SUB" || u == "SUBDATE";
      // A quoted date is an unknown type to PostgreSQL: '2026-01-31' + INTERVAL reads the string as an interval.
      const std::string base = (pg_ && args[0].size() == 1 && args[0][0].k == Tk::Str) ? "CAST(" + str_lit(args[0][0].text) + " AS TIMESTAMP)"
                                                                                       : emit(expr(args[0]));
      if (is_w(args[1], 0, "INTERVAL") && interval(args[1], 0, &amount, &unit, &next) && next == args[1].size()) {
      } else if (u == "ADDDATE" || u == "SUBDATE") {
        amount = emit(expr(args[1]));
        unit = "DAY";
      } else {
        fail(u + " without INTERVAL n UNIT");
        i = c;
        continue;
      }
      if (pg_) o.push_back(raw("(" + base + (sub ? " - " : " + ") + "(" + amount + ") * INTERVAL '1 " + lower(unit) + "')"));
      else o.push_back(raw("DATEADD(" + lower(unit) + ", " + (sub ? "-(" + amount + ")" : amount) + ", " + base + ")"));
      i = c;
      continue;
    }
    for (auto& a : args) a = expr(a);
    std::vector<std::string> as;
    for (auto& a : args) as.push_back(emit(a));
    auto call = [&](const std::string& s) { o.push_back(raw(s)); i = c; };
    if (u == "NOW" || u == "SYSDATE" || u == "CURRENT_TIMESTAMP" || u == "LOCALTIME" || u == "LOCALTIMESTAMP") {
      // MySQL's NOW() is local time with no zone: LOCALTIMESTAMP, not PostgreSQL's now() (timestamptz).
      call(pg_ ? "LOCALTIMESTAMP" + (as.empty() ? std::string() : "(" + as[0] + ")") : "SYSDATETIME()");
    } else if (u == "CURDATE" || u == "CURRENT_DATE") {
      call(pg_ ? "CURRENT_DATE" : "CAST(GETDATE() AS DATE)");
    } else if (u == "CURTIME" || u == "CURRENT_TIME") {
      call(pg_ ? "LOCALTIME" : "CAST(GETDATE() AS TIME)");
    } else if (u == "UTC_TIMESTAMP") {
      call(pg_ ? "(now() AT TIME ZONE 'UTC')" : "SYSUTCDATETIME()");
    } else if (u == "UNIX_TIMESTAMP") {
      if (as.empty()) call(pg_ ? "CAST(EXTRACT(EPOCH FROM now()) AS BIGINT)" : "DATEDIFF_BIG(SECOND, '1970-01-01', SYSUTCDATETIME())");
      else call(pg_ ? "CAST(EXTRACT(EPOCH FROM (" + as[0] + ")) AS BIGINT)" : "DATEDIFF_BIG(SECOND, '1970-01-01', " + as[0] + ")");
    } else if (u == "FROM_UNIXTIME" && as.size() == 1) {
      call(pg_ ? "(TIMESTAMP 'epoch' + (" + as[0] + ") * INTERVAL '1 second')" : "DATEADD(SECOND, " + as[0] + ", '1970-01-01')");
    } else if (u == "IFNULL" && as.size() == 2) {
      call("COALESCE(" + as[0] + ", " + as[1] + ")");
    } else if (u == "IF" && as.size() == 3) {
      call("(CASE WHEN " + as[0] + " THEN " + as[1] + " ELSE " + as[2] + " END)");
    } else if (u == "LAST_INSERT_ID" && as.empty()) {
      call(pg_ ? "lastval()" : "SCOPE_IDENTITY()");
    } else if (u == "RAND" && as.empty()) {
      call(pg_ ? "random()" : "RAND()");
    } else if (u == "LCASE" || u == "UCASE") {
      call(std::string(u == "LCASE" ? "LOWER(" : "UPPER(") + (as.empty() ? "" : as[0]) + ")");
    } else if ((u == "LENGTH" || u == "CHAR_LENGTH" || u == "CHARACTER_LENGTH") && as.size() == 1 && !pg_) {
      // LEN drops trailing spaces and counts characters; DATALENGTH is MySQL's LENGTH (bytes) only for varchar.
      call(u == "LENGTH" ? "DATALENGTH(" + as[0] + ")" : "LEN(" + as[0] + ")");
    } else if (u == "GROUP_CONCAT") {
      // GROUP_CONCAT(x [SEPARATOR 's']): string_agg / STRING_AGG; DISTINCT and ORDER BY inside it are not translated.
      Toks inner = slice(t, i + 2, c);
      std::string sep = "','";
      for (size_t k = 0; k < inner.size(); ++k)
        if (is_w(inner, k, "SEPARATOR") && k + 1 < inner.size() && inner[k + 1].k == Tk::Str) {
          sep = str_lit(inner[k + 1].text);
          inner.resize(k);
          break;
        }
      for (size_t k = 0; k < inner.size(); ++k)
        if (is_w(inner, k, "DISTINCT") || is_w(inner, k, "ORDER")) { fail("GROUP_CONCAT with DISTINCT or ORDER BY"); break; }
      const std::string v = emit(expr(inner));
      call(pg_ ? "string_agg(CAST(" + v + " AS TEXT), " + sep + ")" : "STRING_AGG(CAST(" + v + " AS NVARCHAR(MAX)), " + sep + ")");
    } else {
      // Any other function: the same name, its arguments translated.
      o.push_back(x);
      o.push_back(op("("));
      for (size_t k = 0; k < args.size(); ++k) {
        if (k) o.push_back(op(","));
        append(&o, args[k]);
      }
      o.push_back(op(")"));
      i = c;
    }
  }
  return o;
}

// LIMIT at the end of the statement (outside any parentheses).
Toks Tx::paginate(Toks t) {
  int depth = 0;
  size_t at = t.size();
  for (size_t k = 0; k < t.size(); ++k) {
    if (is_op(t, k, "(")) ++depth;
    else if (is_op(t, k, ")")) --depth;
    else if (depth == 0 && is_w(t, k, "LIMIT")) at = k;
  }
  if (at == t.size()) return t;
  Toks tail = slice(t, at + 1, t.size());
  std::string count, offset = "0";
  Toks rest;
  if (tail.size() >= 3 && is_op(tail, 1, ",")) { offset = tail[0].text; count = tail[2].text; rest = slice(tail, 3, tail.size()); }
  else if (tail.size() >= 3 && is_w(tail, 1, "OFFSET")) { count = tail[0].text; offset = tail[2].text; rest = slice(tail, 3, tail.size()); }
  else if (!tail.empty()) { count = tail[0].text; rest = slice(tail, 1, tail.size()); }
  if (count.empty() || tail[0].k != Tk::Num) { fail("a LIMIT that is not a number"); return t; }
  Toks head = slice(t, 0, at);
  if (pg_) {
    head.push_back(raw("LIMIT " + count + " OFFSET " + offset));
  } else {
    bool ordered = false;
    depth = 0;
    for (size_t k = 0; k < head.size(); ++k) {
      if (is_op(head, k, "(")) ++depth;
      else if (is_op(head, k, ")")) --depth;
      else if (depth == 0 && is_w(head, k, "ORDER")) ordered = true;
    }
    if (!ordered) head.push_back(raw("ORDER BY (SELECT NULL)"));
    head.push_back(raw("OFFSET " + offset + " ROWS FETCH NEXT " + count + " ROWS ONLY"));
  }
  append(&head, rest);
  return head;
}

// A column type at *pos in a column definition, mapped; *pos moves past it and its UNSIGNED/ZEROFILL.
std::string Tx::map_type(const Toks& d, size_t* pos, bool* is_bool) {
  size_t i = *pos;
  if (i >= d.size() || d[i].k != Tk::Word) { fail("a column with no type"); return ""; }
  std::string ty = upper(d[i].text);
  ++i;
  if ((ty == "DOUBLE" && is_w(d, i, "PRECISION")) || (ty == "CHARACTER" && is_w(d, i, "VARYING"))) { ty += " " + upper(d[i].text); ++i; }
  std::vector<std::string> args;
  std::string enum_values;
  if (is_op(d, i, "(")) {
    const size_t c = close_of(d, i);
    for (auto& a : split_commas(d, i + 1, c)) {
      if (!a.empty() && a[0].k == Tk::Str) enum_values += (enum_values.empty() ? "" : ",") + str_lit(a[0].text);
      args.push_back(a.empty() ? "" : a[0].text);
    }
    i = c + 1;
  }
  while (is_w(d, i, "UNSIGNED") || is_w(d, i, "SIGNED") || is_w(d, i, "ZEROFILL")) ++i;
  *pos = i;
  auto a0 = [&](const std::string& dflt) { return args.empty() ? dflt : args[0]; };
  *is_bool = false;
  if ((ty == "TINYINT" && a0("") == "1") || ty == "BOOL" || ty == "BOOLEAN") { *is_bool = true; return pg_ ? "SMALLINT" : "BIT"; }
  if (ty == "TINYINT" || ty == "SMALLINT") return "SMALLINT";
  if (ty == "MEDIUMINT" || ty == "INT" || ty == "INTEGER") return "INT";
  if (ty == "BIGINT") return "BIGINT";
  if (ty == "SERIAL") return pg_ ? "BIGINT GENERATED BY DEFAULT AS IDENTITY" : "BIGINT IDENTITY(1,1)";
  if (ty == "DECIMAL" || ty == "NUMERIC" || ty == "DEC" || ty == "FIXED")
    return "DECIMAL(" + a0("10") + (args.size() > 1 ? ", " + args[1] : std::string(", 0")) + ")";
  if (ty == "FLOAT") return args.size() == 2 ? (pg_ ? "REAL" : "REAL") : (pg_ ? "REAL" : "REAL");
  if (ty == "DOUBLE" || ty == "DOUBLE PRECISION" || ty == "REAL") return pg_ ? "DOUBLE PRECISION" : "FLOAT";
  if (ty == "BIT") return pg_ ? "BIT(" + a0("1") + ")" : (a0("1") == "1" ? "BIT" : "BINARY(" + std::to_string((std::stoi(a0("1")) + 7) / 8) + ")");
  if (ty == "DATE") return "DATE";
  if (ty == "TIME") return pg_ ? "TIME" + (args.empty() ? std::string() : "(" + args[0] + ")") : "TIME" + (args.empty() ? std::string("(0)") : "(" + args[0] + ")");
  if (ty == "DATETIME" || ty == "TIMESTAMP")   // T-SQL's TIMESTAMP is a row version, not a time
    return pg_ ? "TIMESTAMP(" + a0("0") + ")" : "DATETIME2(" + a0("0") + ")";
  if (ty == "YEAR") return "SMALLINT";
  if (ty == "CHAR" || ty == "CHARACTER") return pg_ ? "CHAR(" + a0("1") + ")" : "NCHAR(" + a0("1") + ")";
  if (ty == "VARCHAR" || ty == "CHARACTER VARYING" || ty == "NVARCHAR") {
    if (pg_) return "VARCHAR(" + a0("255") + ")";
    const long n = std::strtol(a0("255").c_str(), nullptr, 10);
    return n > 4000 ? "NVARCHAR(MAX)" : "NVARCHAR(" + std::to_string(n) + ")";
  }
  if (ty == "TINYTEXT" || ty == "TEXT" || ty == "MEDIUMTEXT" || ty == "LONGTEXT" || ty == "JSON")
    return pg_ ? (ty == "JSON" ? "JSONB" : "TEXT") : "NVARCHAR(MAX)";
  if (ty == "BINARY") return pg_ ? "BYTEA" : "BINARY(" + a0("1") + ")";
  if (ty == "VARBINARY") return pg_ ? "BYTEA" : "VARBINARY(" + a0("255") + ")";
  if (ty == "TINYBLOB" || ty == "BLOB" || ty == "MEDIUMBLOB" || ty == "LONGBLOB") return pg_ ? "BYTEA" : "VARBINARY(MAX)";
  if (ty == "ENUM" || ty == "SET") return pg_ ? "TEXT" : "NVARCHAR(255)";
  if (ty == "UUID") return pg_ ? "UUID" : "UNIQUEIDENTIFIER";
  fail("the column type " + ty);
  return ty;
}

// One column of CREATE TABLE / ALTER TABLE ADD: its name, type and attributes, as the target writes them.
Toks Tx::column_def(const Toks& d, bool* is_identity, const std::string& table) {
  Toks o;
  if (d.empty()) return o;
  o.push_back(d[0].k == Tk::Word ? Tk{Tk::QId, d[0].text} : d[0]);   // the name, quoted (it may be a word the target reserves)
  size_t i = 1;
  bool is_bool = false;
  const std::string ty = map_type(d, &i, &is_bool);
  o.push_back(raw(ty));
  *is_identity = false;
  while (i < d.size()) {
    if (is_w(d, i, "AUTO_INCREMENT")) { *is_identity = true; o.push_back(raw(pg_ ? "GENERATED BY DEFAULT AS IDENTITY" : "IDENTITY(1,1)")); ++i; continue; }
    if (is_w(d, i, "COMMENT") && i + 1 < d.size() && d[i + 1].k == Tk::Str) { i += 2; continue; }
    if ((is_w(d, i, "CHARACTER") && is_w(d, i + 1, "SET")) ) { i += 3; continue; }
    if (is_w(d, i, "CHARSET") || is_w(d, i, "COLLATE")) { i += 2; continue; }
    if (is_w(d, i, "ON") && is_w(d, i + 1, "UPDATE")) {
      // ON UPDATE CURRENT_TIMESTAMP: neither target has it on a column; the write-behind sends the value MySQL set.
      i += 2;
      if (i < d.size()) ++i;
      if (is_op(d, i, "(")) i = close_of(d, i) + 1;
      continue;
    }
    if (is_w(d, i, "DEFAULT")) {
      o.push_back(word("DEFAULT"));
      ++i;
      Toks v;
      if (is_op(d, i, "(")) { const size_t c = close_of(d, i); v = slice(d, i, c + 1); i = c + 1; }
      else if (is_op(d, i, "-") && i + 1 < d.size()) { v = slice(d, i, i + 2); i += 2; }
      else if (i < d.size()) {
        v.push_back(d[i++]);
        if (is_op(d, i, "(")) { const size_t c = close_of(d, i); append(&v, slice(d, i, c + 1)); i = c + 1; }
      }
      if (v.size() == 1 && v[0].k == Tk::Str && (v[0].text.rfind("0000-00-00", 0) == 0)) {
        fail("DEFAULT '" + v[0].text + "' (a zero date neither target can hold)");
      }
      append(&o, expr(v));
      continue;
    }
    if (is_w(d, i, "GENERATED") || (is_w(d, i, "AS") && is_op(d, i + 1, "("))) {
      // GENERATED ALWAYS AS (expr) [VIRTUAL|STORED]
      while (i < d.size() && !is_op(d, i, "(")) ++i;
      const size_t c = close_of(d, i);
      const std::string e = emit(expr(slice(d, i, c + 1)));
      i = c + 1;
      while (is_w(d, i, "VIRTUAL") || is_w(d, i, "STORED") || is_w(d, i, "PERSISTENT")) ++i;
      if (pg_) o.push_back(raw("GENERATED ALWAYS AS " + e + " STORED"));
      else { o.erase(o.begin() + 1); o.push_back(raw("AS " + e + " PERSISTED")); }
      continue;
    }
    if (is_w(d, i, "UNIQUE")) { o.push_back(word("UNIQUE")); ++i; if (is_w(d, i, "KEY")) ++i; continue; }
    if (is_w(d, i, "PRIMARY") && is_w(d, i + 1, "KEY")) { o.push_back(raw("PRIMARY KEY")); i += 2; continue; }
    if (is_w(d, i, "REFERENCES") || is_w(d, i, "CHECK")) { append(&o, expr(slice(d, i, d.size()))); break; }
    o.push_back(d[i++]);
  }
  (void)table;
  return o;
}

// The name a table is looked up by: its last part.
std::string bare_name(const Toks& name) { return name.empty() ? std::string() : name.back().text; }

// A table name at t[i] (`db`.`t` or t), the index after it.
size_t read_name(const Toks& t, size_t i, Toks* name) {
  if (i < t.size() && (t[i].k == Tk::Word || t[i].k == Tk::QId)) {
    name->push_back(t[i]);
    ++i;
    if (is_op(t, i, ".") && i + 1 < t.size() && (t[i + 1].k == Tk::Word || t[i + 1].k == Tk::QId)) {
      name->push_back(op("."));
      name->push_back(t[i + 1]);
      i += 2;
    }
  }
  return i;
}

Result<std::string> Tx::create_table(const Toks& t) {
  size_t i = 1;
  bool temporary = false;
  if (is_w(t, i, "TEMPORARY")) { temporary = true; ++i; }
  ++i;   // TABLE
  bool if_not_exists = false;
  if (is_w(t, i, "IF") && is_w(t, i + 1, "NOT") && is_w(t, i + 2, "EXISTS")) { if_not_exists = true; i += 3; }
  Toks name;
  i = read_name(t, i, &name);
  if (name.empty()) return Result<std::string>(Status::Invalid, "CREATE TABLE without a name");
  const std::string tname = bare_name(name);
  if (is_w(t, i, "LIKE") || is_w(t, i, "AS") || is_w(t, i, "SELECT"))
    return Result<std::string>(Status::Unsupported, "CREATE TABLE ... LIKE / AS SELECT");
  if (!is_op(t, i, "(")) return Result<std::string>(Status::Invalid, "CREATE TABLE without its columns");
  const size_t c = close_of(t, i);
  std::vector<Toks> defs = split_commas(t, i + 1, c);
  std::vector<std::string> cols, after;
  for (auto& d : defs) {
    if (d.empty()) continue;
    // Key definitions inside the table.
    size_t k = 0;
    std::string cname;
    if (is_w(d, 0, "CONSTRAINT")) { if (d.size() > 1 && !is_w(d, 1, "PRIMARY") && !is_w(d, 1, "UNIQUE") && !is_w(d, 1, "FOREIGN") && !is_w(d, 1, "CHECK")) { cname = d[1].text; k = 2; } else k = 1; }
    auto key_cols = [&](size_t from) -> std::string {   // (a, b(10), c DESC): prefix lengths dropped
      size_t p = from;
      while (p < d.size() && !is_op(d, p, "(")) ++p;
      if (p >= d.size()) return "";
      const size_t e = close_of(d, p);
      std::string s;
      for (auto& part : split_commas(d, p + 1, e)) {
        if (part.empty()) continue;
        s += (s.empty() ? "" : ", ") + ident(part[0].text);
        if (part.size() > 1 && part.back().k == Tk::Word && (upper(part.back().text) == "DESC" || upper(part.back().text) == "ASC")) s += " " + upper(part.back().text);
      }
      return s;
    };
    if (is_w(d, k, "PRIMARY") && is_w(d, k + 1, "KEY")) {
      cols.push_back((cname.empty() ? "" : "CONSTRAINT " + ident(cname) + " ") + "PRIMARY KEY (" + key_cols(k + 2) + ")");
      continue;
    }
    if (is_w(d, k, "UNIQUE")) {
      size_t p = k + 1;
      if (is_w(d, p, "KEY") || is_w(d, p, "INDEX")) ++p;
      std::string iname = cname;
      if (p < d.size() && !is_op(d, p, "(") && (d[p].k == Tk::Word || d[p].k == Tk::QId) && !is_w(d, p, "USING")) iname = d[p].text;
      cols.push_back((iname.empty() ? "" : "CONSTRAINT " + ident(tname + "_" + iname) + " ") + "UNIQUE (" + key_cols(p) + ")");
      continue;
    }
    if (is_w(d, k, "KEY") || is_w(d, k, "INDEX")) {
      std::string iname = d.size() > 1 && !is_op(d, 1, "(") ? d[1].text : "idx";
      after.push_back("CREATE INDEX " + ident(tname + "_" + iname) + " ON " + emit(name) + " (" + key_cols(1) + ")");
      continue;
    }
    if (is_w(d, k, "FULLTEXT") || is_w(d, k, "SPATIAL")) continue;   // neither target's form of these: left out
    if (is_w(d, k, "FOREIGN") || is_w(d, k, "CHECK")) {
      Toks fk = slice(d, k, d.size());
      cols.push_back((cname.empty() ? "" : "CONSTRAINT " + ident(cname) + " ") + emit(expr(fk)));
      continue;
    }
    bool identity = false;
    cols.push_back(emit(column_def(d, &identity, tname)));
  }
  std::string body;
  for (size_t k = 0; k < cols.size(); ++k) body += (k ? ", " : "") + cols[k];
  // Everything after the column list is MySQL's table options (ENGINE=, CHARSET=, COMMENT=, ...): none carry over.
  std::string out;
  if (pg_) {
    out = std::string("CREATE ") + (temporary ? "TEMPORARY " : "") + "TABLE " + (if_not_exists ? "IF NOT EXISTS " : "") + emit(name) + " (" + body + ")";
    for (const auto& a : after) out += "; " + (if_not_exists ? "CREATE INDEX IF NOT EXISTS" + a.substr(12) : a);
  } else {
    if (temporary) return Result<std::string>(Status::Unsupported, "CREATE TEMPORARY TABLE (T-SQL names them #...)");
    std::string create = "CREATE TABLE " + emit(name) + " (" + body + ")";
    for (const auto& a : after) create += "; " + a;
    out = if_not_exists ? "IF OBJECT_ID(" + str_lit(tname) + ", 'U') IS NULL BEGIN " + create + " END" : create;
  }
  return out;
}

Result<std::string> Tx::alter_table(const Toks& t) {
  size_t i = 2;   // ALTER TABLE
  Toks name;
  i = read_name(t, i, &name);
  const std::string tname = bare_name(name);
  std::vector<std::string> stmts;
  for (auto& part : split_commas(t, i, t.size())) {
    if (part.empty()) continue;
    size_t k = 0;
    if (is_w(part, 0, "ADD") && (is_w(part, 1, "INDEX") || is_w(part, 1, "KEY") || (is_w(part, 1, "UNIQUE")))) {
      const bool uniq = is_w(part, 1, "UNIQUE");
      size_t p = uniq ? 2 : 1;
      if (is_w(part, p, "INDEX") || is_w(part, p, "KEY")) ++p;
      std::string iname = p < part.size() && !is_op(part, p, "(") ? part[p].text : "idx";
      while (p < part.size() && !is_op(part, p, "(")) ++p;
      const size_t e = close_of(part, p);
      std::string cl;
      for (auto& cc : split_commas(part, p + 1, e)) if (!cc.empty()) cl += (cl.empty() ? "" : ", ") + ident(cc[0].text);
      stmts.push_back(std::string("CREATE ") + (uniq ? "UNIQUE " : "") + "INDEX " + ident(tname + "_" + iname) + " ON " + emit(name) + " (" + cl + ")");
      continue;
    }
    if (is_w(part, 0, "ADD")) {
      k = 1;
      if (is_w(part, k, "COLUMN")) ++k;
      Toks def = slice(part, k, part.size());
      // AFTER x / FIRST: column order is MySQL's alone.
      for (size_t p = 0; p < def.size(); ++p)
        if (is_w(def, p, "AFTER") || is_w(def, p, "FIRST")) { def.resize(p); break; }
      if (!def.empty() && (is_w(def, 0, "PRIMARY") || is_w(def, 0, "CONSTRAINT") || is_w(def, 0, "FOREIGN"))) {
        stmts.push_back("ALTER TABLE " + emit(name) + " ADD " + emit(expr(def)));
        continue;
      }
      bool identity = false;
      stmts.push_back("ALTER TABLE " + emit(name) + (pg_ ? " ADD COLUMN " : " ADD ") + emit(column_def(def, &identity, tname)));
      continue;
    }
    if (is_w(part, 0, "DROP")) {
      k = 1;
      if (is_w(part, k, "COLUMN")) ++k;
      if (is_w(part, k, "INDEX") || is_w(part, k, "KEY")) {
        const std::string iname = k + 1 < part.size() ? part[k + 1].text : "";
        stmts.push_back(pg_ ? "DROP INDEX " + ident(tname + "_" + iname) : "DROP INDEX " + ident(tname + "_" + iname) + " ON " + emit(name));
        continue;
      }
      if (is_w(part, k, "PRIMARY") || is_w(part, k, "FOREIGN")) return Result<std::string>(Status::Unsupported, "ALTER TABLE DROP PRIMARY/FOREIGN KEY");
      stmts.push_back("ALTER TABLE " + emit(name) + " DROP COLUMN " + emit(slice(part, k, part.size())));
      continue;
    }
    if (is_w(part, 0, "MODIFY")) {
      k = 1;
      if (is_w(part, k, "COLUMN")) ++k;
      Toks def = slice(part, k, part.size());
      for (size_t p = 0; p < def.size(); ++p)
        if (is_w(def, p, "AFTER") || is_w(def, p, "FIRST")) { def.resize(p); break; }
      size_t p = 1;
      bool is_bool = false;
      const std::string ty = map_type(def, &p, &is_bool);
      bool not_null = false;
      for (size_t q = p; q + 1 < def.size(); ++q) if (is_w(def, q, "NOT") && is_w(def, q + 1, "NULL")) not_null = true;
      const std::string col = ident(def[0].text);
      if (pg_) {
        stmts.push_back("ALTER TABLE " + emit(name) + " ALTER COLUMN " + col + " TYPE " + ty);
        stmts.push_back("ALTER TABLE " + emit(name) + " ALTER COLUMN " + col + (not_null ? " SET NOT NULL" : " DROP NOT NULL"));
      } else {
        stmts.push_back("ALTER TABLE " + emit(name) + " ALTER COLUMN " + col + " " + ty + (not_null ? " NOT NULL" : " NULL"));
      }
      continue;
    }
    if (is_w(part, 0, "RENAME")) {
      k = 1;
      if (is_w(part, k, "TO") || is_w(part, k, "AS")) ++k;
      Toks to;
      read_name(part, k, &to);
      stmts.push_back(pg_ ? "ALTER TABLE " + emit(name) + " RENAME TO " + emit(to) : "EXEC sp_rename " + str_lit(tname) + ", " + str_lit(bare_name(to)));
      continue;
    }
    if (is_w(part, 0, "ENGINE") || is_w(part, 0, "AUTO_INCREMENT") || is_w(part, 0, "DEFAULT") || is_w(part, 0, "CHARSET") ||
        is_w(part, 0, "COMMENT") || is_w(part, 0, "CONVERT") || is_w(part, 0, "ROW_FORMAT"))
      continue;   // table options: MySQL's own
    return Result<std::string>(Status::Unsupported, "ALTER TABLE " + emit(slice(part, 0, std::min<size_t>(part.size(), 3))) + " ...");
  }
  std::string out;
  for (size_t k = 0; k < stmts.size(); ++k) out += (k ? "; " : "") + stmts[k];
  return out;
}

Result<std::string> Tx::insert(const Toks& t) {
  const bool replace = is_w(t, 0, "REPLACE");
  size_t i = 1;
  bool ignore = false;
  while (is_w(t, i, "LOW_PRIORITY") || is_w(t, i, "DELAYED") || is_w(t, i, "HIGH_PRIORITY") || is_w(t, i, "IGNORE")) {
    if (is_w(t, i, "IGNORE")) ignore = true;
    ++i;
  }
  if (is_w(t, i, "INTO")) ++i;
  Toks name;
  i = read_name(t, i, &name);
  if (name.empty()) return Result<std::string>(Status::Invalid, "INSERT without a table");
  if (!pg_ && name.size() == 3) {   // `db`.`t`: in T-SQL that is schema.table; the database's is db.dbo.t
    name.insert(name.begin() + 2, Tk{Tk::Raw, "dbo"});
    name.insert(name.begin() + 2, op("."));
    std::swap(name[2], name[3]);
  }
  const std::string tname = bare_name(name);
  SqlDialectTranslator::TableInfo info;
  const bool known = table_info(tname, &info);
  std::vector<std::string> cols;
  if (is_op(t, i, "(") && !(is_w(t, i + 1, "SELECT"))) {
    const size_t c = close_of(t, i);
    for (auto& cc : split_commas(t, i + 1, c)) if (!cc.empty()) cols.push_back(cc.back().text);
    i = c + 1;
  }
  std::vector<std::string> rows;   // each row's values, translated, "(a, b)"
  std::string select_src;
  if (is_w(t, i, "VALUES") || is_w(t, i, "VALUE")) {
    ++i;
    while (is_op(t, i, "(")) {
      const size_t c = close_of(t, i);
      std::string r;
      auto vals = split_commas(t, i + 1, c);
      for (size_t k = 0; k < vals.size(); ++k) r += (k ? ", " : "") + emit(expr(vals[k]));
      rows.push_back("(" + r + ")");
      i = c + 1;
      if (is_op(t, i, ",")) ++i;
      else break;
    }
  } else if (is_w(t, i, "SET")) {   // INSERT ... SET a = 1, b = 2
    ++i;
    size_t end = t.size();
    for (size_t k = i; k < t.size(); ++k) if (is_w(t, k, "ON") && is_w(t, k + 1, "DUPLICATE")) { end = k; break; }
    std::string r;
    for (auto& as : split_commas(t, i, end)) {
      if (as.size() < 3 || !is_op(as, 1, "=")) return Result<std::string>(Status::Invalid, "INSERT ... SET with something other than col = value");
      cols.push_back(as[0].text);
      r += (r.empty() ? "" : ", ") + emit(expr(slice(as, 2, as.size())));
    }
    rows.push_back("(" + r + ")");
    i = end;
  } else if (is_w(t, i, "SELECT") || is_op(t, i, "(")) {
    size_t end = t.size();
    for (size_t k = i; k < t.size(); ++k) if (is_w(t, k, "ON") && is_w(t, k + 1, "DUPLICATE")) { end = k; break; }
    select_src = emit(paginate(expr(slice(t, i, end))));
    i = end;
  } else {
    return Result<std::string>(Status::Invalid, "INSERT without VALUES, SET or SELECT");
  }
  // ON DUPLICATE KEY UPDATE a = VALUES(a), n = n + 1
  std::vector<std::pair<std::string, Toks>> updates;
  const bool upsert = is_w(t, i, "ON") && is_w(t, i + 1, "DUPLICATE");
  if (upsert) {
    i += 4;   // ON DUPLICATE KEY UPDATE
    for (auto& as : split_commas(t, i, t.size())) {
      if (as.size() < 3 || !is_op(as, 1, "=")) return Result<std::string>(Status::Invalid, "ON DUPLICATE KEY UPDATE with something other than col = value");
      updates.push_back({as[0].text, slice(as, 2, as.size())});
    }
  } else if (i < t.size()) {
    return Result<std::string>(Status::Invalid, "INSERT with something after its rows");
  }
  if (cols.empty() && known) cols = info.columns;
  const bool needs_key = upsert || replace || (ignore && !pg_);
  if (needs_key && info.key.empty())
    return Result<std::string>(Status::Unsupported, std::string(upsert ? "ON DUPLICATE KEY UPDATE" : replace ? "REPLACE" : "INSERT IGNORE") +
                                                       " on " + tname + ", whose key is not known");
  if ((needs_key || replace) && cols.empty()) return Result<std::string>(Status::Unsupported, "an upsert with no column list on " + tname);
  std::string col_list;
  for (size_t k = 0; k < cols.size(); ++k) col_list += (k ? ", " : "") + ident(cols[k]);
  std::string values;
  for (size_t k = 0; k < rows.size(); ++k) values += (k ? ", " : "") + rows[k];
  const std::string target = emit(name);
  auto key_is = [&](const std::string& c) {
    for (const auto& kc : info.key) if (lower(kc) == lower(c)) return true;
    return false;
  };
  // An update's right side: VALUES(c) is the row being inserted, a bare
  // column the row already there.
  auto rhs = [&](const Toks& e, const std::string& incoming, const std::string& existing) {
    Toks o;
    for (size_t k = 0; k < e.size(); ++k) {
      if (is_w(e, k, "VALUES") && is_op(e, k + 1, "(") && k + 3 < e.size() && is_op(e, k + 3, ")")) {
        o.push_back(raw(incoming + "." + ident(e[k + 2].text)));
        k += 3;
        continue;
      }
      bool is_col = false;
      if ((e[k].k == Tk::Word || e[k].k == Tk::QId) && !is_op(e, k + 1, "(") && !(k > 0 && is_op(e, k - 1, ".")))
        for (const auto& c : cols) if (lower(c) == lower(e[k].text)) is_col = true;
      if (is_col) { o.push_back(raw(existing + "." + ident(e[k].text))); continue; }
      o.push_back(e[k]);
    }
    return emit(expr(o));
  };
  const std::string src = select_src.empty() ? "VALUES " + values : select_src;
  std::string out;
  if (pg_) {
    out = "INSERT INTO " + target + (col_list.empty() ? "" : " (" + col_list + ")") + " " + src;
    std::string key_list;
    for (size_t k = 0; k < info.key.size(); ++k) key_list += (k ? ", " : "") + ident(info.key[k]);
    if (upsert) {
      std::string set;
      for (const auto& u : updates) set += (set.empty() ? "" : ", ") + ident(u.first) + " = " + rhs(u.second, "EXCLUDED", target);
      out += " ON CONFLICT (" + key_list + ") DO UPDATE SET " + set;
    } else if (replace) {
      std::string set;
      for (const auto& c : cols) if (!key_is(c)) set += (set.empty() ? "" : ", ") + ident(c) + " = EXCLUDED." + ident(c);
      out += " ON CONFLICT (" + key_list + ")" + (set.empty() ? " DO NOTHING" : " DO UPDATE SET " + set);
    } else if (ignore) {
      out += " ON CONFLICT DO NOTHING";
    }
  } else {
    if (needs_key || replace) {
      if (!select_src.empty()) return Result<std::string>(Status::Unsupported, "an upsert from a SELECT for T-SQL");
      std::string on;
      for (const auto& kc : info.key) on += (on.empty() ? "" : " AND ") + std::string("target.") + ident(kc) + " = source." + ident(kc);
      std::string src_cols, ins_vals;
      for (size_t k = 0; k < cols.size(); ++k) {
        src_cols += (k ? ", " : "") + ident(cols[k]);
        ins_vals += (k ? ", " : "") + std::string("source.") + ident(cols[k]);
      }
      out = "MERGE INTO " + target + " WITH (HOLDLOCK) AS target USING (VALUES " + values + ") AS source (" + src_cols + ") ON " + on;
      std::string set;
      if (upsert) for (const auto& u : updates) set += (set.empty() ? "" : ", ") + std::string("target.") + ident(u.first) + " = " + rhs(u.second, "source", "target");
      if (replace) for (const auto& c : cols) if (!key_is(c)) set += (set.empty() ? "" : ", ") + std::string("target.") + ident(c) + " = source." + ident(c);
      if (!set.empty()) out += " WHEN MATCHED THEN UPDATE SET " + set;
      out += " WHEN NOT MATCHED THEN INSERT (" + src_cols + ") VALUES (" + ins_vals + ");";   // MERGE must end in ;
    } else {
      out = "INSERT INTO " + target + (col_list.empty() ? "" : " (" + col_list + ")") + " " + src;
    }
    // An AUTO_INCREMENT column written with its value: IDENTITY_INSERT around it.
    bool writes_identity = false;
    if (!info.identity.empty()) for (const auto& c : cols) if (lower(c) == lower(info.identity)) writes_identity = true;
    if (writes_identity) out = "SET IDENTITY_INSERT " + target + " ON; " + out + (out.back() == ';' ? " " : "; ") + "SET IDENTITY_INSERT " + target + " OFF";
  }
  return out;
}

Result<std::string> Tx::run(Toks t) {
  if (t.empty()) return std::string();
  const std::string verb = t[0].k == Tk::Word ? upper(t[0].text) : "";
  Result<std::string> r = std::string();
  if (verb == "SET") {
    if (is_w(t, 1, "TRANSACTION") || (is_w(t, 1, "SESSION") && is_w(t, 2, "TRANSACTION"))) r = emit(t);
    else r = std::string();   // SET NAMES / sql_mode / autocommit / foreign_key_checks ...: MySQL session settings, nothing to send
  } else if (verb == "START" || verb == "BEGIN") {
    r = std::string(pg_ ? "BEGIN" : "BEGIN TRANSACTION");
  } else if (verb == "COMMIT" || verb == "ROLLBACK") {
    if (is_w(t, 1, "TO")) return Result<std::string>(Status::Unsupported, "ROLLBACK TO a savepoint");
    r = verb;
  } else if (verb == "LOCK" || verb == "UNLOCK") {
    r = std::string();   // LOCK TABLES: the downstream's own transaction does this work
  } else if (verb == "USE") {
    r = pg_ ? std::string() : "USE " + emit(slice(t, 1, t.size()));   // a PostgreSQL session follows the database by reconnecting (select_db)
  } else if (verb == "CREATE" && (is_w(t, 1, "TABLE") || (is_w(t, 1, "TEMPORARY") && is_w(t, 2, "TABLE")))) {
    r = create_table(t);
  } else if (verb == "CREATE" && (is_w(t, 1, "INDEX") || (is_w(t, 1, "UNIQUE") && is_w(t, 2, "INDEX")))) {
    Toks o;
    for (size_t k = 0; k < t.size(); ++k) {
      if (is_w(t, k, "USING") && k + 1 < t.size()) { ++k; continue; }
      if (is_op(t, k, "(") && k > 0 && (t[k - 1].k == Tk::Word || t[k - 1].k == Tk::QId) && o.size() > 4) {
        // the column list: prefix lengths dropped
        const size_t c = close_of(t, k);
        std::string cl;
        for (auto& cc : split_commas(t, k + 1, c)) if (!cc.empty()) cl += (cl.empty() ? "" : ", ") + ident(cc[0].text);
        o.push_back(raw("(" + cl + ")"));
        k = c;
        continue;
      }
      o.push_back(t[k]);
    }
    r = emit(o);
  } else if (verb == "ALTER" && is_w(t, 1, "TABLE")) {
    r = alter_table(t);
  } else if (verb == "DROP" && is_w(t, 1, "INDEX")) {
    // DROP INDEX name ON t: PostgreSQL's index names are its schema's, the table not named.
    if (pg_) { Toks name; read_name(t, 2, &name); r = "DROP INDEX " + emit(name); }
    else r = emit(t);
  } else if (verb == "INSERT" || verb == "REPLACE") {
    r = insert(t);
  } else if (verb == "UPDATE" || verb == "DELETE") {
    Toks e = expr(t);
    // UPDATE / DELETE ... LIMIT n: T-SQL's TOP (n); PostgreSQL has neither.
    int depth = 0;
    for (size_t k = 0; k < e.size(); ++k) {
      if (is_op(e, k, "(")) ++depth;
      else if (is_op(e, k, ")")) --depth;
      else if (depth == 0 && (is_w(e, k, "LIMIT") || is_w(e, k, "ORDER"))) {
        if (pg_ || is_w(e, k, "ORDER")) return Result<std::string>(Status::Unsupported, verb + " with ORDER BY or LIMIT");
        const std::string n = k + 1 < e.size() ? e[k + 1].text : "1";
        e.resize(k);
        e.insert(e.begin() + (verb == "DELETE" ? 1 : 1), raw("TOP (" + n + ")"));
        break;
      }
    }
    r = emit(e);
  } else {
    // SELECT, DROP, TRUNCATE, CREATE VIEW, ...: identifiers, literals and functions translated.
    r = emit(paginate(expr(t)));
  }
  if (!err_.empty()) return Result<std::string>(Status::Unsupported, err_);
  return r;
}

}  // namespace

std::string SqlDialectTranslator::quote_identifier(const std::string& ident, SqlDialect dialect) {
  if (ident.empty()) return ident;
  std::string clean = ident;
  if (clean.size() >= 2 && ((clean.front() == '`' && clean.back() == '`') || (clean.front() == '"' && clean.back() == '"') ||
                            (clean.front() == '[' && clean.back() == ']')))
    clean = clean.substr(1, clean.size() - 2);
  std::string o;
  switch (dialect) {
    case SqlDialect::MariaDB:
      o = "`";
      for (char c : clean) { if (c == '`') o += '`'; o += c; }
      return o + "`";
    case SqlDialect::Postgres:
      o = "\"";
      for (char c : lower(clean)) { if (c == '"') o += '"'; o += c; }
      return o + "\"";
    case SqlDialect::MsSql:
      o = "[";
      for (char c : clean) { if (c == ']') o += ']'; o += c; }
      return o + "]";
  }
  return clean;
}

Result<std::string> SqlDialectTranslator::translate_checked(const std::string& sql, SqlDialect source, SqlDialect target,
                                                            const TableLookup& lookup) {
  if (source == target) return sql;
  if (source != SqlDialect::MariaDB) return Result<std::string>(Status::Unsupported, "translation is from MariaDB/MySQL only");
  auto toks = lex_mysql(sql);
  if (!toks.ok()) return Result<std::string>(toks.error());
  Tx tx(target, lookup);
  return tx.run(std::move(*toks));
}

std::string SqlDialectTranslator::translate(const std::string& sql, SqlDialect source, SqlDialect target) {
  auto r = translate_checked(sql, source, target);
  if (!r.ok()) return "-- untranslatable: " + r.error().message;
  return *r;
}

}  // namespace aios::userland
