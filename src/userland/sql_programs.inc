// Stored programs: CREATE PROCEDURE / FUNCTION / TRIGGER / EVENT, kept as the
// statements that made them (<root>/programs.db) and parsed again at open, and
// the interpreter that runs their bodies. Included by sql_database.cpp, whose
// types (Val, Token, the tokenizer) it uses.
//
// A body is parsed once into a tree of statements. Each plain SQL statement in
// it runs through the ordinary engine with the routine's variables bound by
// token: an identifier that names a variable in scope becomes its value as a
// literal (MariaDB's rule: a local variable wins over a column of the same
// name), except where a name is a name - after '.', before '(', after AS, in
// an INSERT column list, and as the target of a SET col = in UPDATE.

namespace {
thread_local std::string tls_signal_state, tls_signal_message;
}  // namespace
// The SQLSTATE of the SIGNAL this thread's last error came from, if it did.
bool sql_signal_state(const std::string& msg, std::string* state) {
  if (tls_signal_state.empty() || tls_signal_message != msg) return false;
  *state = tls_signal_state;
  return true;
}
namespace {
std::vector<SqlQueryResult> sql_run_procedure(SqlDatabase& db, const SqlDatabase::StoredProgram& p, const std::vector<Val>& in,
                                              std::vector<Val>* out, const std::string& session_db, std::string* err);

namespace psm {

struct Node;
using Nodes = std::vector<Node>;

struct Node {
  enum Kind {
    Block, DeclareVar, DeclareCursor, DeclareHandler, DeclareCondition, Set, SelectInto, If, Case, While, Repeat,
    Loop, Leave, Iterate, Return, Open, Fetch, Close, Signal, Call, Sql
  } kind = Sql;
  std::string label;                 // Block / Loop / While / Repeat
  std::string text;                  // Sql / Call / SelectInto (without INTO) / Return / cursor query / signal state
  std::string type;                  // DeclareVar type
  std::string deflt;                 // DeclareVar DEFAULT expression ("" = NULL)
  std::vector<std::string> names;    // DeclareVar names, INTO / FETCH targets, SET targets, handler conditions
  std::vector<std::string> exprs;    // SET values
  std::vector<std::pair<std::string, Nodes>> branches;   // If / ElseIf / Case When: condition, body
  Nodes body, else_body;
  bool has_else = false;
  std::string cond;                  // While / Repeat UNTIL / simple Case operand
  bool handler_exit = false;         // EXIT handler (CONTINUE otherwise)
  std::string message;               // SIGNAL ... SET MESSAGE_TEXT
};

// Token-cursor parser over a body's text.
class Parser {
 public:
  Parser(const std::string& text, const std::vector<Token>& toks) : s_(text), t_(toks) {}
  bool parse_all(Nodes* out, std::string* err) {
    i_ = 0;
    *out = list({});
    if (!err_.empty()) { *err = err_; return false; }
    if (!at_end()) { *err = "unexpected '" + t_[i_].text + "' in the routine body"; return false; }
    return true;
  }
  // The routine body is one statement (a BEGIN ... END or a single one).
  bool parse_one(Nodes* out, std::string* err) {
    i_ = 0;
    out->clear();
    if (!at_end()) out->push_back(stmt());
    if (!err_.empty()) { *err = err_; return false; }
    while (!at_end() && punct(";")) ++i_;
    if (!at_end()) { *err = "unexpected '" + t_[i_].text + "' after the routine body"; return false; }
    return true;
  }

 private:
  const std::string& s_;
  const std::vector<Token>& t_;
  size_t i_ = 0;
  std::string err_;

  bool at_end() const { return i_ >= t_.size() || t_[i_].kind == Tok::End; }
  bool word(const char* w, size_t at) const { return at < t_.size() && t_[at].kind == Tok::Ident && t_[at].up == w; }
  bool word(const char* w) const { return word(w, i_); }
  bool punct(const char* p, size_t at) const { return at < t_.size() && t_[at].kind == Tok::Punct && t_[at].text == p; }
  bool punct(const char* p) const { return punct(p, i_); }
  bool take(const char* w) { if (word(w)) { ++i_; return true; } return false; }
  void fail(const std::string& m) { if (err_.empty()) err_ = m; }
  void expect(const char* w) { if (!take(w)) fail(std::string("expected ") + w + (at_end() ? " at the end" : " near '" + t_[i_].text + "'")); }
  void end_stmt() { if (punct(";")) ++i_; }
  size_t pos_of(size_t k) const { return k < t_.size() && t_[k].kind != Tok::End ? t_[k].start_pos : s_.size(); }
  std::string slice(size_t a, size_t b) const {   // tokens [a, b)
    if (a >= b) return {};
    return trim(s_.substr(pos_of(a), (b < t_.size() ? pos_of(b) : s_.size()) - pos_of(a)));
  }
  bool at_stop(const std::vector<const char*>& stops) const {
    if (at_end()) return true;
    for (const char* w : stops) if (word(w)) return true;
    return false;
  }
  // The next keyword `w` at this nesting level: parentheses and CASE ... END
  // (an expression's own END) are stepped over. Returns its index, or end.
  size_t find_kw(size_t from, std::initializer_list<const char*> ws) const {
    int depth = 0, cases = 0;
    for (size_t k = from; k < t_.size() && t_[k].kind != Tok::End; ++k) {
      if (punct("(", k)) { ++depth; continue; }
      if (punct(")", k)) { --depth; continue; }
      if (depth) continue;
      if (word("CASE", k)) { ++cases; continue; }
      if (cases && word("END", k)) { --cases; continue; }
      if (cases) continue;
      for (const char* w : ws) if (word(w, k)) return k;
      if (punct(";", k)) return k;
    }
    return t_.size();
  }
  // To the ';' that ends a plain statement.
  size_t stmt_end(size_t from) const {
    int depth = 0;
    for (size_t k = from; k < t_.size() && t_[k].kind != Tok::End; ++k) {
      if (punct("(", k)) ++depth;
      else if (punct(")", k)) --depth;
      else if (depth == 0 && punct(";", k)) return k;
    }
    return t_.size();
  }

  Nodes list(const std::vector<const char*>& stops) {
    Nodes out;
    while (err_.empty() && !at_stop(stops)) {
      if (punct(";")) { ++i_; continue; }
      out.push_back(stmt());
    }
    return out;
  }

  // END <WORD> [label]
  void end_compound(const char* what, const std::string& label) {
    expect("END");
    if (what) expect(what);
    if (!label.empty() && !at_end() && t_[i_].kind == Tok::Ident && lower_of(t_[i_].text) == lower_of(label)) ++i_;
    end_stmt();
  }

  Node stmt() {
    Node n;
    std::string label;
    if (!at_end() && t_[i_].kind == Tok::Ident && punct(":", i_ + 1) &&
        (word("BEGIN", i_ + 2) || word("LOOP", i_ + 2) || word("WHILE", i_ + 2) || word("REPEAT", i_ + 2))) {
      label = t_[i_].text;
      i_ += 2;
    }
    n.label = label;
    if (take("BEGIN")) {
      if (word("NOT") && word("ATOMIC", i_ + 1)) i_ += 2;
      n.kind = Node::Block;
      n.body = list({"END"});
      end_compound(nullptr, label);
      return n;
    }
    if (take("IF")) {
      n.kind = Node::If;
      for (;;) {
        size_t th = find_kw(i_, {"THEN"});
        if (!word("THEN", th)) { fail("IF without THEN"); return n; }
        std::string c = slice(i_, th);
        i_ = th + 1;
        n.branches.push_back({c, list({"ELSEIF", "ELSE", "END"})});
        if (take("ELSEIF")) continue;
        break;
      }
      if (take("ELSE")) { n.has_else = true; n.else_body = list({"END"}); }
      end_compound("IF", "");
      return n;
    }
    if (take("CASE")) {
      n.kind = Node::Case;
      if (!word("WHEN")) {
        size_t w = find_kw(i_, {"WHEN"});
        n.cond = slice(i_, w);
        i_ = w;
      }
      while (take("WHEN")) {
        size_t th = find_kw(i_, {"THEN"});
        if (!word("THEN", th)) { fail("CASE WHEN without THEN"); return n; }
        std::string c = slice(i_, th);
        i_ = th + 1;
        n.branches.push_back({c, list({"WHEN", "ELSE", "END"})});
      }
      if (take("ELSE")) { n.has_else = true; n.else_body = list({"END"}); }
      end_compound("CASE", "");
      return n;
    }
    if (take("WHILE")) {
      n.kind = Node::While;
      size_t d = find_kw(i_, {"DO"});
      if (!word("DO", d)) { fail("WHILE without DO"); return n; }
      n.cond = slice(i_, d);
      i_ = d + 1;
      n.body = list({"END"});
      end_compound("WHILE", label);
      return n;
    }
    if (take("REPEAT")) {
      n.kind = Node::Repeat;
      n.body = list({"UNTIL"});
      expect("UNTIL");
      size_t e = find_kw(i_, {"END"});
      n.cond = slice(i_, e);
      i_ = e;
      end_compound("REPEAT", label);
      return n;
    }
    if (take("LOOP")) {
      n.kind = Node::Loop;
      n.body = list({"END"});
      end_compound("LOOP", label);
      return n;
    }
    if (word("LEAVE") || word("ITERATE")) {
      n.kind = word("LEAVE") ? Node::Leave : Node::Iterate;
      ++i_;
      if (!at_end() && t_[i_].kind == Tok::Ident) n.label = t_[i_++].text;
      end_stmt();
      return n;
    }
    if (take("RETURN")) {
      n.kind = Node::Return;
      size_t e = stmt_end(i_);
      n.text = slice(i_, e);
      i_ = e;
      end_stmt();
      return n;
    }
    if (take("DECLARE")) return declare();
    if (word("OPEN") || word("CLOSE")) {
      n.kind = word("OPEN") ? Node::Open : Node::Close;
      ++i_;
      if (!at_end()) n.text = lower_of(t_[i_++].text);
      end_stmt();
      return n;
    }
    if (take("FETCH")) {
      n.kind = Node::Fetch;
      if (take("GROUP")) {   // FETCH GROUP NEXT ROW: an aggregate function's next row
        take("NEXT");
        take("ROW");
        n.text = "#group";
        end_stmt();
        return n;
      }
      if (take("NEXT")) expect("FROM");
      else take("FROM");
      if (!at_end()) n.text = lower_of(t_[i_++].text);
      expect("INTO");
      size_t e = stmt_end(i_);
      for (size_t k = i_; k < e; ++k) if (!punct(",", k)) n.names.push_back(t_[k].text);
      i_ = e;
      end_stmt();
      return n;
    }
    if (word("SIGNAL") || word("RESIGNAL")) {
      n.kind = Node::Signal;
      ++i_;
      size_t e = stmt_end(i_);
      if (take("SQLSTATE")) { take("VALUE"); if (!at_end() && t_[i_].kind == Tok::Str) n.text = t_[i_++].text; }
      else if (!at_end() && t_[i_].kind == Tok::Ident) n.text = t_[i_++].text;   // a named condition
      for (size_t k = i_; k + 2 < e; ++k)
        if (word("MESSAGE_TEXT", k) && punct("=", k + 1)) n.message = slice(k + 2, find_comma(k + 2, e));
      i_ = e;
      end_stmt();
      return n;
    }
    if (word("SET")) {
      size_t e = stmt_end(i_);
      // SET a = x, @b := y: each target and its value, split at the top-level commas.
      n.kind = Node::Set;
      size_t k = i_ + 1;
      while (k < e) {
        size_t c = find_comma(k, e);
        size_t eq = k;
        while (eq < c && !punct("=", eq) && !punct(":=", eq)) ++eq;
        if (eq >= c) { n.kind = Node::Sql; break; }   // SET NAMES utf8, SET TRANSACTION ...: the engine's
        n.names.push_back(slice(k, eq));
        n.exprs.push_back(slice(eq + 1, c));
        k = c + 1;
      }
      if (n.kind == Node::Sql) { n.names.clear(); n.exprs.clear(); n.text = slice(i_, e); }
      i_ = e;
      end_stmt();
      return n;
    }
    if (word("CALL")) {
      size_t e = stmt_end(i_);
      n.kind = Node::Call;
      n.text = slice(i_, e);
      i_ = e;
      end_stmt();
      return n;
    }
    if (word("SELECT") || word("WITH")) {
      size_t e = stmt_end(i_);
      // SELECT ... INTO v1, v2 [FROM ...]: INTO at the top level, not INTO OUTFILE/DUMPFILE.
      int depth = 0;
      for (size_t k = i_; k < e; ++k) {
        if (punct("(", k)) { ++depth; continue; }
        if (punct(")", k)) { --depth; continue; }
        if (depth || !word("INTO", k) || word("OUTFILE", k + 1) || word("DUMPFILE", k + 1)) continue;
        size_t v = k + 1, ve = v;
        while (ve < e && (t_[ve].kind == Tok::Ident || punct(",", ve)) && !word("FROM", ve) && !word("WHERE", ve) &&
               !word("GROUP", ve) && !word("ORDER", ve) && !word("LIMIT", ve) && !word("HAVING", ve) &&
               !word("FOR", ve) && !word("LOCK", ve) && !word("UNION", ve))
          ++ve;
        for (size_t q = v; q < ve; ++q) if (!punct(",", q)) n.names.push_back(t_[q].text);
        n.kind = Node::SelectInto;
        n.text = trim(slice(i_, k) + " " + slice(ve, e));
        break;
      }
      if (n.kind != Node::SelectInto) n.text = slice(i_, e);
      i_ = e;
      end_stmt();
      return n;
    }
    size_t e = stmt_end(i_);
    n.kind = Node::Sql;
    n.text = slice(i_, e);
    i_ = e;
    end_stmt();
    return n;
  }

  size_t find_comma(size_t from, size_t to) const {
    int depth = 0;
    for (size_t k = from; k < to; ++k) {
      if (punct("(", k)) ++depth;
      else if (punct(")", k)) --depth;
      else if (depth == 0 && punct(",", k)) return k;
    }
    return to;
  }

  Node declare() {
    Node n;
    size_t e = stmt_end(i_);
    if (word("CONTINUE") || word("EXIT") || word("UNDO")) {
      n.kind = Node::DeclareHandler;
      n.handler_exit = !word("CONTINUE");
      ++i_;
      expect("HANDLER");
      expect("FOR");
      // conditions up to the statement that is the handler's body
      for (;;) {
        if (take("SQLSTATE")) {
          take("VALUE");
          if (!at_end() && t_[i_].kind == Tok::Str) n.names.push_back("sqlstate:" + t_[i_++].text);
        } else if (word("NOT") && word("FOUND", i_ + 1)) {
          i_ += 2;
          n.names.push_back("not found");
        } else if (take("SQLEXCEPTION")) {
          n.names.push_back("sqlexception");
        } else if (take("SQLWARNING")) {
          n.names.push_back("sqlwarning");
        } else if (!at_end() && t_[i_].kind == Tok::Num) {
          n.names.push_back("errno:" + t_[i_++].text);
        } else if (!at_end() && t_[i_].kind == Tok::Ident) {
          n.names.push_back("named:" + lower_of(t_[i_++].text));
        } else {
          fail("expected a handler condition");
          return n;
        }
        if (punct(",")) { ++i_; continue; }
        break;
      }
      n.body.push_back(stmt());
      return n;
    }
    // DECLARE name CURSOR FOR select / name CONDITION FOR ...
    if (!at_end() && word("CURSOR", i_ + 1)) {
      n.kind = Node::DeclareCursor;
      n.names.push_back(lower_of(t_[i_].text));
      i_ += 2;
      expect("FOR");
      n.text = slice(i_, e);
      i_ = e;
      end_stmt();
      return n;
    }
    if (!at_end() && word("CONDITION", i_ + 1)) {
      n.kind = Node::DeclareCondition;
      n.names.push_back(lower_of(t_[i_].text));
      i_ += 2;
      expect("FOR");
      if (take("SQLSTATE")) { take("VALUE"); if (!at_end() && t_[i_].kind == Tok::Str) n.text = "sqlstate:" + t_[i_++].text; }
      else if (!at_end() && t_[i_].kind == Tok::Num) n.text = "errno:" + t_[i_++].text;
      i_ = e;
      end_stmt();
      return n;
    }
    n.kind = Node::DeclareVar;
    size_t k = i_;
    while (k < e && t_[k].kind == Tok::Ident && !word("DEFAULT", k)) {
      // names, comma-separated, then the type
      n.names.push_back(lower_of(t_[k].text));
      ++k;
      if (punct(",", k)) { ++k; continue; }
      break;
    }
    size_t d = k;
    while (d < e && !word("DEFAULT", d)) ++d;
    n.type = slice(k, d);
    if (d < e) n.deflt = slice(d + 1, e);
    i_ = e;
    end_stmt();
    return n;
  }
};

// A condition the body raised: NOT FOUND (no row for SELECT ... INTO / FETCH),
// an SQL error, or a SIGNAL.
struct Condition {
  enum Kind { None, NotFound, Exception } kind = None;
  std::string sqlstate;
  std::string message;
};

}  // namespace psm
}  // namespace


namespace {
namespace psm {
const Nodes& body_of(const SqlDatabase::StoredProgram& p) {
  static const Nodes none;
  return p.body_tree ? *static_cast<const Nodes*>(p.body_tree.get()) : none;
}

std::string literal_of(const Val& v) {
  if (v.null) return "NULL";
  if (v.numeric) {
    std::string s = v.str();
    if (!s.empty()) return s;
  }
  std::string out = "'";
  for (char c : v.str()) {
    if (c == '\'') out += "''";
    else if (c == '\\') out += "\\\\";
    else out += c;
  }
  return out + "'";
}

Val cell_val(const SqlQueryResult& r, size_t row, size_t col) {
  if (row >= r.rows.size() || col >= r.rows[row].values.size()) return val_null();
  const std::string& c = r.rows[row].values[col];
  if (sql_is_null(c)) return val_null();
  if (col < r.column_types.size() && r.column_types[col] == SqlDataType::Text) return val_text(c);
  return val_cell(c);
}

// The routine's run: variables in scopes, cursors, handlers, the trigger's
// rows, and where control is going.
struct Run {
  SqlDatabase& db;
  std::string session_db;
  std::vector<std::map<std::string, Val>> scopes;
  struct Cursor { std::string query; SqlQueryResult rows; size_t at = 0; bool open = false; };
  std::vector<std::map<std::string, Cursor>> cursors;
  struct Handler { std::vector<std::string> conds; bool exit = false; const Node* body = nullptr; size_t scope = 0; };
  std::vector<Handler> handlers;
  std::map<std::string, std::string> named_conditions;
  // trigger rows
  const SqlTable* trig_table = nullptr;
  const SqlRow* old_row = nullptr;
  SqlRow* new_row = nullptr;
  bool new_writable = false;
  // results
  std::vector<SqlQueryResult> result_sets;
  bool allow_results = true;
  Val ret;
  bool returned = false;
  std::string error;
  // an aggregate function's group
  const std::vector<std::vector<Val>>* group_rows = nullptr;
  size_t group_at = 0;
  const SqlDatabase::StoredProgram* group_fn = nullptr;
  enum Flow { Normal, Leave, Iterate, Return, Exit } flow = Normal;
  std::string flow_label;
  size_t exit_scope = 0;

  explicit Run(SqlDatabase& d, std::string sdb) : db(d), session_db(std::move(sdb)) { scopes.emplace_back(); cursors.emplace_back(); }

  Val* find_var(const std::string& name) {
    const std::string k = lower_of(name);
    for (size_t i = scopes.size(); i-- > 0;) {
      auto it = scopes[i].find(k);
      if (it != scopes[i].end()) return &it->second;
    }
    return nullptr;
  }
  int trig_col(const std::string& col) const {
    if (!trig_table) return -1;
    return trig_table->get_column_index(col);
  }

  // The statement with variables, NEW.col and OLD.col bound as literals.
  bool bind(const std::string& sql, std::string* out) {
    std::vector<Token> toks;
    std::string terr;
    if (!tokenize_sql(sql, &toks, &terr)) { error = terr; return false; }
    out->clear();
    size_t last = 0;
    bool in_insert_cols = false, saw_into = false, in_set = false, upd = false;
    int depth = 0, insert_cols_depth = -1;
    const bool is_update = !toks.empty() && toks[0].kind == Tok::Ident && toks[0].up == "UPDATE";
    for (size_t k = 0; k < toks.size() && toks[k].kind != Tok::End; ++k) {
      const Token& t = toks[k];
      if (t.kind == Tok::Punct && t.text == "(") {
        ++depth;
        if (saw_into && !in_insert_cols && k >= 2 && toks[k - 1].kind == Tok::Ident) { in_insert_cols = true; insert_cols_depth = depth; }
        continue;
      }
      if (t.kind == Tok::Punct && t.text == ")") {
        if (in_insert_cols && depth == insert_cols_depth) in_insert_cols = false, saw_into = false;
        --depth;
        continue;
      }
      if (t.kind != Tok::Ident) continue;
      if (t.up == "INTO" && k > 0 && (toks[k - 1].up == "INSERT" || toks[k - 1].up == "REPLACE" || toks[k - 1].up == "IGNORE")) { saw_into = true; continue; }
      if (t.up == "VALUES" || t.up == "SELECT" || t.up == "VALUE") saw_into = false;
      if (is_update && t.up == "SET" && depth == 0) { in_set = true; continue; }
      if (t.up == "WHERE" && depth == 0) in_set = false;
      if (t.up == "UPDATE" && k > 0 && toks[k - 1].up == "KEY") { upd = true; continue; }   // ON DUPLICATE KEY UPDATE
      const bool next_dot = k + 1 < toks.size() && toks[k + 1].kind == Tok::Punct && toks[k + 1].text == ".";
      const bool prev_dot = k > 0 && toks[k - 1].kind == Tok::Punct && toks[k - 1].text == ".";
      const bool next_paren = k + 1 < toks.size() && toks[k + 1].kind == Tok::Punct && toks[k + 1].text == "(";
      // NEW.col / OLD.col in a trigger body
      if (trig_table && next_dot && k + 2 < toks.size() && toks[k + 2].kind == Tok::Ident && (t.up == "NEW" || t.up == "OLD")) {
        const int ci = trig_col(toks[k + 2].text);
        const SqlRow* row = t.up == "NEW" ? new_row : old_row;
        if (ci >= 0) {
          std::string lit = "NULL";
          if (row && (size_t)ci < row->values.size() && !sql_is_null(row->values[(size_t)ci])) {
            const SqlColumn& c = trig_table->columns[(size_t)ci];
            lit = literal_of(sql_type_numeric(c.type) ? val_cell(row->values[(size_t)ci]) : val_text(row->values[(size_t)ci]));
          }
          *out += sql.substr(last, t.start_pos - last) + lit;
          last = toks[k + 2].end_pos;
          k += 2;
          continue;
        }
      }
      if (next_dot || prev_dot || next_paren || in_insert_cols) continue;
      if (k > 0 && toks[k - 1].kind == Tok::Ident && toks[k - 1].up == "AS") continue;
      if ((in_set || upd) && depth == 0 && k + 1 < toks.size() && toks[k + 1].kind == Tok::Punct && (toks[k + 1].text == "=" || toks[k + 1].text == ":=")) continue;
      if (sql.compare(t.start_pos, 1, "`") == 0) continue;   // a quoted name is a name
      Val* v = find_var(t.text);
      if (!v) continue;
      *out += sql.substr(last, t.start_pos - last) + literal_of(*v);
      last = t.end_pos;
    }
    *out += sql.substr(last);
    return true;
  }

  // One statement through the engine; a failure is raised as a condition.
  bool exec(const std::string& sql, SqlQueryResult* res, Condition* cond) {
    std::string bound;
    if (!bind(sql, &bound)) { cond->kind = Condition::Exception; cond->message = error; error.clear(); return false; }
    *res = db.execute_local_query_unguarded(bound, session_db);
    if (!res->success) { cond->kind = Condition::Exception; cond->message = res->error_message; return false; }
    return true;
  }
  bool eval(const std::string& expr, Val* out, Condition* cond) {
    if (trim(expr).empty()) { *out = val_null(); return true; }
    SqlQueryResult r;
    if (!exec("SELECT " + expr, &r, cond)) return false;
    *out = cell_val(r, 0, 0);
    return true;
  }
  // A condition is true when its value is a non-zero number, as MySQL reads
  // IF / WHILE / UNTIL: '0' and 'abc' are false.
  bool truthy(const Val& v) const { return !v.null && (v.numeric ? val_truthy(v) : val_as_number(v) != 0.0); }

  // Assign to a variable, a user @variable, or NEW.col in a BEFORE trigger.
  bool assign(const std::string& target_raw, const Val& v, Condition* cond) {
    std::string target = trim(target_raw);
    if (!target.empty() && target.front() == '`' && target.back() == '`') target = target.substr(1, target.size() - 2);
    if (is_user_var_name(target)) { user_var_set(target, v); return true; }
    const std::string up = upper_of(target);
    if (trig_table && (up.rfind("NEW.", 0) == 0 || up.rfind("OLD.", 0) == 0)) {
      if (up[0] == 'O' || !new_writable || !new_row) {
        cond->kind = Condition::Exception;
        cond->message = up[0] == 'O' ? "Updating of OLD row is not allowed in trigger"
                                     : "Updating of NEW row is not allowed in after trigger";
        return false;
      }
      const int ci = trig_col(target.substr(4));
      if (ci < 0) { cond->kind = Condition::Exception; cond->message = "Unknown column '" + target.substr(4) + "' in 'NEW'"; return false; }
      new_row->values[(size_t)ci] = v.null ? sql_null() : v.str();
      return true;
    }
    if (Val* var = find_var(target)) { *var = v; return true; }
    cond->kind = Condition::Exception;
    cond->message = "Unknown system variable '" + target + "'";
    return false;
  }

  // A raised condition: the innermost handler that matches runs; with none,
  // NOT FOUND is a warning (execution goes on) and anything else is the error.
  bool raise(const Condition& c) {
    for (size_t i = handlers.size(); i-- > 0;) {
      const Handler& h = handlers[i];
      bool hit = false;
      for (const std::string& want : h.conds) {
        std::string w = want;
        if (w.rfind("named:", 0) == 0) {
          auto it = named_conditions.find(w.substr(6));
          if (it == named_conditions.end()) continue;
          w = it->second;
        }
        if (w == "not found" && c.kind == Condition::NotFound) hit = true;
        else if (w == "sqlexception" && c.kind == Condition::Exception) hit = true;
        else if (w.rfind("sqlstate:", 0) == 0 && w.substr(9) == (c.kind == Condition::NotFound ? "02000" : c.sqlstate)) hit = true;
        if (hit) break;
      }
      if (!hit) continue;
      const size_t scope = h.scope;
      const bool ex = h.exit;
      const Node* body = h.body;
      // A handler's body runs with the handlers outside it in force, not its own.
      std::vector<Handler> saved(handlers.begin() + (long)i, handlers.end());
      handlers.resize(i);
      run_list(body->kind == Node::Block ? body->body : Nodes{*body}, body->kind == Node::Block);
      handlers.insert(handlers.end(), saved.begin(), saved.end());
      if (!error.empty()) return false;
      if (ex && flow == Normal) { flow = Exit; exit_scope = scope; }
      return true;
    }
    if (c.kind == Condition::NotFound) return true;
    error = c.message.empty() ? "Unhandled user-defined exception condition" : c.message;
    // A SIGNAL that escapes: the wire answers it with its own SQLSTATE (1644).
    if (!c.sqlstate.empty()) { tls_signal_state = c.sqlstate; tls_signal_message = error; }
    return false;
  }

  void run_list(const Nodes& nodes, bool new_scope) {
    if (new_scope) { scopes.emplace_back(); cursors.emplace_back(); }
    const size_t my_scope = scopes.size() - 1;
    const size_t handlers_at = handlers.size();
    for (const Node& n : nodes) {
      run(n, my_scope);
      if (!error.empty() || flow != Normal) break;
    }
    handlers.resize(std::min(handlers.size(), handlers_at));
    if (flow == Exit && exit_scope == my_scope) flow = Normal;   // EXIT leaves the block that declared it
    if (new_scope) { scopes.pop_back(); cursors.pop_back(); }
  }

  bool loop_continues(const Node& n) {
    if (flow == Leave && lower_of(flow_label) == lower_of(n.label)) { flow = Normal; return false; }
    if (flow == Iterate && lower_of(flow_label) == lower_of(n.label)) { flow = Normal; return true; }
    return flow == Normal;
  }

  void run(const Node& n, size_t scope) {
    Condition c;
    switch (n.kind) {
      case Node::Block:
        run_list(n.body, true);
        if (flow == Leave && !n.label.empty() && lower_of(flow_label) == lower_of(n.label)) flow = Normal;
        return;
      case Node::DeclareVar: {
        Val v = val_null();
        if (!n.deflt.empty() && !eval(n.deflt, &v, &c)) { raise(c); return; }
        for (const std::string& name : n.names) scopes[scope][name] = v;
        return;
      }
      case Node::DeclareCursor:
        cursors[scope][n.names[0]] = Cursor{n.text, {}, 0, false};
        return;
      case Node::DeclareCondition:
        named_conditions[n.names[0]] = n.text;
        return;
      case Node::DeclareHandler: {
        Handler h;
        h.conds = n.names;
        h.exit = n.handler_exit;
        h.body = &n.body[0];
        h.scope = scope;
        handlers.push_back(h);
        return;
      }
      case Node::Set: {
        for (size_t i = 0; i < n.names.size(); ++i) {
          Val v;
          if (!eval(n.exprs[i], &v, &c)) { raise(c); return; }
          if (!assign(n.names[i], v, &c)) { raise(c); return; }
        }
        return;
      }
      case Node::SelectInto: {
        SqlQueryResult r;
        if (!exec(n.text, &r, &c)) { raise(c); return; }
        if (r.rows.empty()) { c.kind = Condition::NotFound; c.message = "No data - zero rows fetched, selected, or processed"; raise(c); return; }
        if (r.rows.size() > 1) { c.kind = Condition::Exception; c.message = "Result consisted of more than one row"; raise(c); return; }
        if (n.names.size() != r.rows[0].values.size()) { c.kind = Condition::Exception; c.message = "The used SELECT statements have a different number of columns"; raise(c); return; }
        for (size_t i = 0; i < n.names.size(); ++i)
          if (!assign(n.names[i], cell_val(r, 0, i), &c)) { raise(c); return; }
        return;
      }
      case Node::If: {
        for (const auto& br : n.branches) {
          Val v;
          if (!eval(br.first, &v, &c)) { raise(c); return; }
          if (truthy(v)) { run_list(br.second, false); return; }
        }
        if (n.has_else) run_list(n.else_body, false);
        return;
      }
      case Node::Case: {
        Val operand;
        if (!n.cond.empty() && !eval(n.cond, &operand, &c)) { raise(c); return; }
        for (const auto& br : n.branches) {
          Val v;
          if (!eval(n.cond.empty() ? br.first : "(" + literal_of(operand) + ") = (" + br.first + ")", &v, &c)) { raise(c); return; }
          if (truthy(v)) { run_list(br.second, false); return; }
        }
        if (n.has_else) { run_list(n.else_body, false); return; }
        c.kind = Condition::Exception;
        c.sqlstate = "20000";
        c.message = "Case not found for CASE statement";
        raise(c);
        return;
      }
      case Node::While: {
        for (;;) {
          Val v;
          if (!eval(n.cond, &v, &c)) { raise(c); return; }
          if (!truthy(v)) return;
          run_list(n.body, false);
          if (!error.empty() || !loop_continues(n)) return;
        }
      }
      case Node::Repeat: {
        for (;;) {
          run_list(n.body, false);
          if (!error.empty() || !loop_continues(n)) return;
          Val v;
          if (!eval(n.cond, &v, &c)) { raise(c); return; }
          if (truthy(v)) return;
        }
      }
      case Node::Loop: {
        for (;;) {
          run_list(n.body, false);
          if (!error.empty() || !loop_continues(n)) return;
        }
      }
      case Node::Leave:
      case Node::Iterate:
        flow = n.kind == Node::Leave ? Leave : Iterate;
        flow_label = n.label;
        return;
      case Node::Return: {
        if (!eval(n.text, &ret, &c)) { raise(c); return; }
        returned = true;
        flow = Return;
        return;
      }
      case Node::Open: {
        Cursor* cur = find_cursor(n.text);
        if (!cur) { c.kind = Condition::Exception; c.message = "Undefined CURSOR: " + n.text; raise(c); return; }
        if (cur->open) { c.kind = Condition::Exception; c.message = "Cursor is already open"; raise(c); return; }
        if (!exec(cur->query, &cur->rows, &c)) { raise(c); return; }
        cur->at = 0;
        cur->open = true;
        return;
      }
      case Node::Fetch: {
        if (n.text == "#group") {
          if (!group_rows || !group_fn) { c.kind = Condition::Exception; c.message = "FETCH GROUP NEXT ROW outside an aggregate function"; raise(c); return; }
          if (group_at >= group_rows->size()) { c.kind = Condition::NotFound; c.message = "No data - zero rows fetched, selected, or processed"; raise(c); return; }
          const auto& row = (*group_rows)[group_at++];
          for (size_t k = 0; k < group_fn->params.size() && k < row.size(); ++k)
            if (!assign(group_fn->params[k].name, row[k], &c)) { raise(c); return; }
          return;
        }
        Cursor* cur = find_cursor(n.text);
        if (!cur || !cur->open) { c.kind = Condition::Exception; c.message = "Cursor is not open"; raise(c); return; }
        if (cur->at >= cur->rows.rows.size()) { c.kind = Condition::NotFound; c.message = "No data - zero rows fetched, selected, or processed"; raise(c); return; }
        if (n.names.size() != cur->rows.rows[cur->at].values.size()) { c.kind = Condition::Exception; c.message = "Incorrect number of FETCH variables"; raise(c); return; }
        for (size_t i = 0; i < n.names.size(); ++i)
          if (!assign(n.names[i], cell_val(cur->rows, cur->at, i), &c)) { raise(c); return; }
        cur->at++;
        return;
      }
      case Node::Close: {
        Cursor* cur = find_cursor(n.text);
        if (!cur || !cur->open) { c.kind = Condition::Exception; c.message = "Cursor is not open"; raise(c); return; }
        cur->open = false;
        cur->rows = SqlQueryResult{};
        return;
      }
      case Node::Signal: {
        c.kind = Condition::Exception;
        std::string state = n.text;
        auto it = named_conditions.find(lower_of(state));
        if (it != named_conditions.end() && it->second.rfind("sqlstate:", 0) == 0) state = it->second.substr(9);
        c.sqlstate = state;
        Val m;
        if (!n.message.empty() && eval(n.message, &m, &c) && !m.null) c.message = m.str();
        else if (c.message.empty()) c.message = "Unhandled user-defined exception condition";
        if (state.rfind("02", 0) == 0) c.kind = Condition::NotFound;
        raise(c);
        return;
      }
      case Node::Call:
        call(n.text, &c);
        return;
      case Node::Sql: {
        SqlQueryResult r;
        if (!exec(n.text, &r, &c)) { raise(c); return; }
        if (r.is_result_set && allow_results) result_sets.push_back(std::move(r));
        return;
      }
    }
  }

  Cursor* find_cursor(const std::string& name) {
    for (size_t i = cursors.size(); i-- > 0;) {
      auto it = cursors[i].find(name);
      if (it != cursors[i].end()) return &it->second;
    }
    return nullptr;
  }

  void call(const std::string& text, Condition* c);
};

}  // namespace psm

// ---- Parsing a CREATE ------------------------------------------------------

std::string name_part(const Token& t) { return strip_ident(t.text); }

// [db.]name at toks[*i]; advances past it.
bool take_program_name(const std::vector<Token>& toks, size_t* i, const std::string& default_db, std::string* db, std::string* name) {
  if (*i >= toks.size() || (toks[*i].kind != Tok::Ident && toks[*i].kind != Tok::Str)) return false;
  std::string first = name_part(toks[*i]);
  ++*i;
  if (*i + 1 < toks.size() && toks[*i].kind == Tok::Punct && toks[*i].text == ".") {
    *db = first;
    *name = name_part(toks[*i + 1]);
    *i += 2;
  } else {
    *db = default_db;
    *name = first;
  }
  return true;
}

std::string program_key(char kind, const std::string& db, const std::string& name) {
  return std::string(1, kind) + ":" + lower_of(db) + "." + lower_of(name);
}

const char* kind_word(char k) { return k == 'P' ? "PROCEDURE" : k == 'F' ? "FUNCTION" : k == 'T' ? "TRIGGER" : "EVENT"; }

// The parsed program, or an error. Does not touch the catalog.
Result<std::shared_ptr<SqlDatabase::StoredProgram>> parse_program(SqlDatabase& db, const std::string& sql,
                                                                         const std::string& default_db, bool* or_replace,
                                                                         bool* if_not_exists) {
  using R = Result<std::shared_ptr<SqlDatabase::StoredProgram>>;
  std::vector<Token> toks;
  std::string terr;
  if (!tokenize_sql(sql, &toks, &terr)) return R(Status::Invalid, terr);
  auto p = std::make_shared<SqlDatabase::StoredProgram>();
  p->create_text = trim(sql);
  while (!p->create_text.empty() && p->create_text.back() == ';') p->create_text = trim(p->create_text.substr(0, p->create_text.size() - 1));
  size_t i = 0;
  auto w = [&](const char* s) { return i < toks.size() && toks[i].kind == Tok::Ident && toks[i].up == s; };
  auto take = [&](const char* s) { if (w(s)) { ++i; return true; } return false; };
  auto punct = [&](const char* s) { return i < toks.size() && toks[i].kind == Tok::Punct && toks[i].text == s; };
  if (!take("CREATE")) return R(Status::Invalid, "expected CREATE");
  *or_replace = false;
  if (take("OR")) { if (!take("REPLACE")) return R(Status::Invalid, "expected OR REPLACE"); *or_replace = true; }
  if (take("DEFINER")) {
    if (!punct("=")) return R(Status::Invalid, "expected DEFINER = user");
    ++i;
    size_t a = i;
    while (i < toks.size() && !(toks[i].kind == Tok::Ident && (toks[i].up == "PROCEDURE" || toks[i].up == "FUNCTION" ||
                                                              toks[i].up == "TRIGGER" || toks[i].up == "EVENT" ||
                                                              toks[i].up == "AGGREGATE" || toks[i].up == "SQL")))
      ++i;
    if (i > a) p->definer = trim(sql.substr(toks[a].start_pos, toks[i - 1].end_pos - toks[a].start_pos));
  }
  if (take("SQL")) { take("SECURITY"); if (i < toks.size()) p->security = toks[i++].up; }   // views only, but harmless
  if (take("AGGREGATE")) {
    if (!w("FUNCTION")) return R(Status::Invalid, "expected FUNCTION after AGGREGATE");
    p->aggregate = true;
  }
  if (take("PROCEDURE")) p->kind = 'P';
  else if (take("FUNCTION")) p->kind = 'F';
  else if (take("TRIGGER")) p->kind = 'T';
  else if (take("EVENT")) p->kind = 'E';
  else return R(Status::Invalid, "expected PROCEDURE, FUNCTION, TRIGGER or EVENT");
  *if_not_exists = false;
  if (w("IF") && i + 2 < toks.size() && toks[i + 1].up == "NOT" && toks[i + 2].up == "EXISTS") { i += 3; *if_not_exists = true; }
  if (!take_program_name(toks, &i, default_db, &p->db, &p->name)) return R(Status::Invalid, "expected a name");
  const std::string kw = kind_word(p->kind);

  // The body: everything from `body_from` to the end.
  size_t body_from = toks.size();
  if (p->kind == 'P' || p->kind == 'F') {
    if (!punct("(")) return R(Status::Invalid, "expected ( after the " + lower_of(kw) + " name");
    ++i;
    int depth = 1;
    size_t start = i;
    auto add_param = [&](size_t a, size_t b) -> Result<void> {
      if (a >= b) return Ok();
      SqlDatabase::StoredProgram::Param pr;
      size_t k = a;
      if (p->kind == 'P' && toks[k].kind == Tok::Ident) {
        if (toks[k].up == "IN") { pr.mode = 0; ++k; }
        else if (toks[k].up == "OUT") { pr.mode = 1; ++k; }
        else if (toks[k].up == "INOUT") { pr.mode = 2; ++k; }
      }
      if (k >= b) return Result<void>(Status::Invalid, "a parameter without a name");
      pr.name = name_part(toks[k]);
      ++k;
      pr.type = k < b ? trim(sql.substr(toks[k].start_pos, toks[b - 1].end_pos - toks[k].start_pos)) : "";
      p->params.push_back(pr);
      return Ok();
    };
    for (; i < toks.size() && toks[i].kind != Tok::End; ++i) {
      if (toks[i].kind == Tok::Punct && toks[i].text == "(") ++depth;
      else if (toks[i].kind == Tok::Punct && toks[i].text == ")") {
        if (--depth == 0) { if (auto r = add_param(start, i); !r.ok()) return R(r.error().status, r.error().message); ++i; break; }
      } else if (depth == 1 && toks[i].kind == Tok::Punct && toks[i].text == ",") {
        if (auto r = add_param(start, i); !r.ok()) return R(r.error().status, r.error().message);
        start = i + 1;
      }
    }
    if (p->kind == 'F') {
      if (!take("RETURNS")) return R(Status::Invalid, "a function needs RETURNS <type>");
      size_t a = i;
      // the type runs until a characteristic or the body
      static const std::set<std::string> kChar = {"LANGUAGE", "DETERMINISTIC", "NOT", "CONTAINS", "NO", "READS", "MODIFIES", "SQL", "COMMENT", "BEGIN", "RETURN"};
      int d = 0;
      while (i < toks.size() && toks[i].kind != Tok::End) {
        if (toks[i].kind == Tok::Punct && toks[i].text == "(") ++d;
        else if (toks[i].kind == Tok::Punct && toks[i].text == ")") --d;
        else if (d == 0 && toks[i].kind == Tok::Ident && kChar.count(toks[i].up) && !(toks[i].up == "NOT" && i + 1 < toks.size() && toks[i + 1].up == "NULL")) break;
        ++i;
      }
      if (i > a) p->returns = trim(sql.substr(toks[a].start_pos, toks[i - 1].end_pos - toks[a].start_pos));
    }
    // characteristics
    for (;;) {
      if (take("LANGUAGE")) { take("SQL"); continue; }
      if (w("NOT") && i + 1 < toks.size() && toks[i + 1].up == "DETERMINISTIC") { i += 2; p->deterministic = false; continue; }
      if (take("DETERMINISTIC")) { p->deterministic = true; continue; }
      if (w("CONTAINS") && i + 1 < toks.size() && toks[i + 1].up == "SQL") { i += 2; p->data_access = "CONTAINS SQL"; continue; }
      if (w("NO") && i + 1 < toks.size() && toks[i + 1].up == "SQL") { i += 2; p->data_access = "NO SQL"; continue; }
      if (w("READS") && i + 2 < toks.size()) { i += 3; p->data_access = "READS SQL DATA"; continue; }
      if (w("MODIFIES") && i + 2 < toks.size()) { i += 3; p->data_access = "MODIFIES SQL DATA"; continue; }
      if (w("SQL") && i + 1 < toks.size() && toks[i + 1].up == "SECURITY") { i += 2; if (i < toks.size()) p->security = toks[i++].up; continue; }
      if (take("COMMENT")) { if (i < toks.size() && toks[i].kind == Tok::Str) p->comment = toks[i++].text; continue; }
      break;
    }
    body_from = i;
  } else if (p->kind == 'T') {
    if (take("BEFORE")) p->timing = "BEFORE";
    else if (take("AFTER")) p->timing = "AFTER";
    else return R(Status::Invalid, "a trigger is BEFORE or AFTER");
    if (take("INSERT")) p->event = "INSERT";
    else if (take("UPDATE")) p->event = "UPDATE";
    else if (take("DELETE")) p->event = "DELETE";
    else return R(Status::Invalid, "a trigger fires on INSERT, UPDATE or DELETE");
    if (!take("ON")) return R(Status::Invalid, "expected ON <table>");
    std::string tdb;
    if (!take_program_name(toks, &i, p->db, &tdb, &p->table)) return R(Status::Invalid, "expected the trigger's table");
    if (lower_of(tdb) != lower_of(p->db)) return R(Status::Invalid, "a trigger is in its table's database");
    if (!take("FOR")) return R(Status::Invalid, "expected FOR EACH ROW");
    take("EACH");
    if (!take("ROW")) return R(Status::Invalid, "expected FOR EACH ROW");
    if (take("FOLLOWS") || take("PRECEDES")) { std::string d1, n1; take_program_name(toks, &i, p->db, &d1, &n1); }
    body_from = i;
  } else {
    // ON SCHEDULE AT ts [+ INTERVAL ...] | EVERY n unit [STARTS ts] [ENDS ts]
    if (!take("ON") || !take("SCHEDULE")) return R(Status::Invalid, "expected ON SCHEDULE");
    auto until_kw = [&](std::initializer_list<const char*> stops) {
      size_t a = i;
      while (i < toks.size() && toks[i].kind != Tok::End) {
        bool stop = false;
        for (const char* s : stops) if (toks[i].kind == Tok::Ident && toks[i].up == s) stop = true;
        if (stop) break;
        ++i;
      }
      return i > a ? trim(sql.substr(toks[a].start_pos, toks[i - 1].end_pos - toks[a].start_pos)) : std::string();
    };
    auto epoch_of = [&](const std::string& expr, int64_t* out) -> Result<void> {
      SqlQueryResult r = db.execute_local_query_unguarded("SELECT UNIX_TIMESTAMP(" + expr + ")", default_db);
      if (!r.success || r.rows.empty() || r.rows[0].values.empty() || sql_is_null(r.rows[0].values[0]))
        return Result<void>(Status::Invalid, "the event's time '" + expr + "' is not a time");
      *out = (int64_t)std::strtoll(r.rows[0].values[0].c_str(), nullptr, 10);
      return Ok();
    };
    static const std::initializer_list<const char*> kAfterSchedule = {"ON", "ENABLE", "DISABLE", "COMMENT", "DO", "STARTS", "ENDS"};
    if (take("AT")) {
      p->at = true;
      std::string e = until_kw(kAfterSchedule);
      if (auto r = epoch_of(e, &p->at_s); !r.ok()) return R(r.error().status, r.error().message);
    } else if (take("EVERY")) {
      size_t a = i;
      static const std::set<std::string> kUnits = {"SECOND", "MINUTE", "HOUR", "DAY", "WEEK", "MONTH", "QUARTER", "YEAR"};
      while (i < toks.size() && !(toks[i].kind == Tok::Ident && (kUnits.count(toks[i].up) || toks[i].up.find('_') != std::string::npos))) ++i;
      if (i >= toks.size() || i == a) return R(Status::Invalid, "EVERY needs a number and a unit");
      p->every_n = trim(sql.substr(toks[a].start_pos, toks[i - 1].end_pos - toks[a].start_pos));
      p->every_unit = toks[i].up;
      if (!kUnits.count(p->every_unit)) return R(Status::Unsupported, "EVERY ... " + p->every_unit + ": only single units (SECOND ... YEAR)");
      ++i;
      if (take("STARTS")) { std::string e = until_kw(kAfterSchedule); if (auto r = epoch_of(e, &p->starts_s); !r.ok()) return R(r.error().status, r.error().message); }
      if (take("ENDS")) { std::string e = until_kw(kAfterSchedule); if (auto r = epoch_of(e, &p->ends_s); !r.ok()) return R(r.error().status, r.error().message); }
    } else {
      return R(Status::Invalid, "ON SCHEDULE AT ... or EVERY ...");
    }
    for (;;) {
      if (w("ON") && i + 1 < toks.size() && toks[i + 1].up == "COMPLETION") {
        i += 2;
        p->preserve = true;
        if (take("NOT")) p->preserve = false;
        take("PRESERVE");
        continue;
      }
      if (take("ENABLE")) { p->enabled = true; continue; }
      if (take("DISABLE")) { p->enabled = false; if (w("ON")) { i += 2; } continue; }
      if (take("COMMENT")) { if (i < toks.size() && toks[i].kind == Tok::Str) p->comment = toks[i++].text; continue; }
      break;
    }
    if (!take("DO")) return R(Status::Invalid, "expected DO <statement>");
    body_from = i;
  }
  if (body_from >= toks.size() || toks[body_from].kind == Tok::End) return R(Status::Invalid, "the " + lower_of(kw) + " has no body");
  p->body_text = trim(sql.substr(toks[body_from].start_pos));
  while (!p->body_text.empty() && p->body_text.back() == ';') p->body_text = trim(p->body_text.substr(0, p->body_text.size() - 1));
  std::vector<Token> btoks;
  if (!tokenize_sql(p->body_text, &btoks, &terr)) return R(Status::Invalid, terr);
  psm::Parser bp(p->body_text, btoks);
  std::string perr;
  auto tree = std::make_shared<psm::Nodes>();
  if (!bp.parse_one(tree.get(), &perr)) return R(Status::Invalid, "in the body of " + lower_of(kw) + " " + p->name + ": " + perr);
  p->body_tree = tree;
  return p;
}
}  // namespace

// ---- The catalog ------------------------------------------------------------

std::shared_ptr<const SqlDatabase::StoredProgram> SqlDatabase::find_program(char kind, const std::string& name) const {
  std::string pdb, pname;
  split_qualified_name(name, effective_database(), &pdb, &pname);
  hal::LockGuard g(programs_mu_);
  auto it = programs_.find(program_key(kind, pdb, strip_ident(pname)));
  return it == programs_.end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<const SqlDatabase::StoredProgram>> SqlDatabase::table_triggers(const SqlTable& t, const char* event) const {
  std::vector<std::shared_ptr<const StoredProgram>> out;
  if (!has_triggers_.load(std::memory_order_acquire)) return out;
  if (g_undo_applying) return out;   // taking a change back is not a new change: no trigger reacts to it
  // Replicated rows already carry what the primary's triggers did, and a dump
  // is the record of rows its triggers already wrote. A redo replay fires them
  // (and skips the records they wrote, kRedoTriggerTag), so a BEFORE trigger's
  // changes and refusals replay as they ran.
  if (bulk_load_ || seeding_ || (g_cdc_thread && !redo_replaying_)) return out;   // replay runs flagged as cdc
  const std::string tdb = lower_of(t.database.empty() ? std::string("aios_db") : t.database), tname = lower_of(t.name);
  hal::LockGuard g(programs_mu_);
  for (const auto& kv : programs_) {
    const StoredProgram& p = *kv.second;
    if (p.kind == 'T' && p.event == event && lower_of(p.db) == tdb && lower_of(p.table) == tname) out.push_back(kv.second);
  }
  // BEFORE first, then AFTER; within each, the order they were created.
  std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
    if (a->timing != b->timing) return a->timing == "BEFORE";
    return a->created_ns < b->created_ns;
  });
  return out;
}

void SqlDatabase::save_programs() {
  if (root_path_.empty()) return;
  Writer w;
  w.u32(0x41515047);   // "AQPG"
  {
    hal::LockGuard g(programs_mu_);
    w.u32((uint32_t)programs_.size());
    for (const auto& kv : programs_) {
      w.str(kv.second->db);
      w.str(kv.second->create_text);
      w.u64(kv.second->created_ns);
      w.u64(kv.second->modified_ns);
    }
  }
  kernel_.host().write_file(root_path_ + "/programs.db", ByteSpan(w.buf().data(), w.buf().size()));
}

void SqlDatabase::load_programs() {
  if (root_path_.empty()) return;
  auto raw = kernel_.host().read_file(root_path_ + "/programs.db");
  if (!raw.ok() || raw->empty()) return;
  Reader r(ByteSpan(raw->data(), raw->size()));
  if (r.u32() != 0x41515047) return;
  const uint32_t n = r.u32();
  for (uint32_t k = 0; k < n && r.ok(); ++k) {
    const std::string pdb = r.str(), text = r.str();
    const uint64_t created = r.u64(), modified = r.u64();
    if (!r.ok()) break;
    bool orr = false, ine = false;
    auto p = parse_program(*this, text, pdb, &orr, &ine);
    if (!p.ok()) {
      fprintf(stderr, "[asql] a stored program did not load: %s\n", p.error().message.c_str());
      continue;
    }
    (*p)->created_ns = created;
    (*p)->modified_ns = modified;
    if ((*p)->kind == 'E') (*p)->last_run.store(0);
    hal::LockGuard g(programs_mu_);
    programs_[program_key((*p)->kind, (*p)->db, (*p)->name)] = *p;
  }
  refresh_program_flags();
}

void SqlDatabase::refresh_program_flags() {
  bool f = false, t = false, e = false;
  {
    hal::LockGuard g(programs_mu_);
    for (const auto& kv : programs_) {
      f |= kv.second->kind == 'F';
      t |= kv.second->kind == 'T';
      e |= kv.second->kind == 'E';
    }
  }
  {
    std::set<std::string> aggs;
    hal::LockGuard g(programs_mu_);
    for (const auto& kv : programs_)
      if (kv.second->kind == 'F' && kv.second->aggregate) aggs.insert(upper_of(kv.second->name));
    sql_set_stored_aggregates(std::move(aggs));
  }
  has_functions_.store(f, std::memory_order_release);
  has_triggers_.store(t, std::memory_order_release);
  has_events_.store(e, std::memory_order_release);
}

// CREATE / DROP / ALTER of a stored program.
SqlQueryResult SqlDatabase::execute_program_ddl(const std::string& sql) {
  std::vector<Token> toks;
  std::string terr;
  if (!tokenize_sql(sql, &toks, &terr)) return make_error(terr);
  if (toks.empty()) return make_error("empty statement");
  const std::string verb = toks[0].up;
  if (verb == "CREATE") {
    bool or_replace = false, if_not_exists = false;
    auto p = parse_program(*this, sql, effective_database(), &or_replace, &if_not_exists);
    if (!p.ok()) return make_error(p.error().message);
    StoredProgram& np = **p;
    if (np.kind == 'T') {
      SqlTable* t = resolve_table(np.db + "." + np.table);
      if (!t) return make_error("Table '" + np.db + "." + np.table + "' doesn't exist");
    }
    const std::string key = program_key(np.kind, np.db, np.name);
    // A dump, a replicated statement or a redo record that names a program
    // already here replaces it: they are the record of what it now is.
    const bool replaying = bulk_load_ || seeding_ || redo_replaying_ || g_cdc_thread;
    np.created_ns = np.modified_ns = kernel_.host().wall_ns();
    {
      hal::LockGuard g(programs_mu_);
      auto it = programs_.find(key);
      if (it != programs_.end()) {
        if (if_not_exists) {
          SqlQueryResult r;
          r.formatted_table = std::string(kind_word(np.kind)) + " " + np.name + " already exists (IF NOT EXISTS).";
          return r;
        }
        if (!or_replace && !replaying)
          return make_error(std::string(kind_word(np.kind)) + " " + np.name + " already exists");
        np.created_ns = it->second->created_ns;
      }
      if (np.kind == 'T') {
        // One trigger name per database.
        for (const auto& kv : programs_)
          if (kv.first != key && kv.second->kind == 'T' && lower_of(kv.second->db) == lower_of(np.db) &&
              lower_of(kv.second->name) == lower_of(np.name))
            return make_error("Trigger " + np.name + " already exists");
      }
      programs_[key] = *p;
    }
    refresh_program_flags();
    save_programs();
    SqlQueryResult r;
    r.formatted_table = std::string(kind_word(np.kind)) + " " + np.name + " created.";
    return r;
  }
  if (verb == "DROP" && toks.size() > 2) {
    char kind = toks[1].up == "PROCEDURE" ? 'P' : toks[1].up == "FUNCTION" ? 'F' : toks[1].up == "TRIGGER" ? 'T' : 'E';
    size_t i = 2;
    bool if_exists = false;
    if (i + 1 < toks.size() && toks[i].up == "IF" && toks[i + 1].up == "EXISTS") { if_exists = true; i += 2; }
    std::string pdb, pname;
    if (!take_program_name(toks, &i, effective_database(), &pdb, &pname)) return make_error("expected a name");
    bool gone = false;
    {
      hal::LockGuard g(programs_mu_);
      gone = programs_.erase(program_key(kind, pdb, pname)) > 0;
    }
    if (!gone && !if_exists && !(bulk_load_ || seeding_ || redo_replaying_ || g_cdc_thread))
      return make_error(std::string(kind_word(kind)) + " " + pdb + "." + pname + " does not exist");
    refresh_program_flags();
    if (gone) save_programs();
    SqlQueryResult r;
    r.formatted_table = std::string(kind_word(kind)) + " " + pname + (gone ? " dropped." : " did not exist.");
    return r;
  }
  if (verb == "ALTER" && toks.size() > 2) {
    // ALTER EVENT e ENABLE | DISABLE; ALTER PROCEDURE/FUNCTION p COMMENT '..' (the characteristics only)
    const char kind = toks[1].up == "EVENT" ? 'E' : toks[1].up == "FUNCTION" ? 'F' : 'P';
    size_t i = 2;
    std::string pdb, pname;
    if (!take_program_name(toks, &i, effective_database(), &pdb, &pname)) return make_error("expected a name");
    bool altered = false;
    struct SaveAfter { SqlDatabase* d; bool* a; ~SaveAfter() { if (*a) { d->refresh_program_flags(); d->save_programs(); } } } save_after{this, &altered};
    hal::LockGuard g(programs_mu_);
    auto it = programs_.find(program_key(kind, pdb, pname));
    if (it == programs_.end()) return make_error(std::string(kind_word(kind)) + " " + pdb + "." + pname + " does not exist");
    auto np = std::make_shared<StoredProgram>();
    const StoredProgram& old = *it->second;
    bool orr = false, ine = false;
    auto re = parse_program(*this, old.create_text, old.db, &orr, &ine);
    if (!re.ok()) return make_error(re.error().message);
    np = *re;
    np->created_ns = old.created_ns;
    for (; i < toks.size() && toks[i].kind != Tok::End; ++i) {
      if (toks[i].up == "ENABLE") np->enabled = true;
      else if (toks[i].up == "DISABLE") np->enabled = false;
      else if (toks[i].up == "COMMENT" && i + 1 < toks.size() && toks[i + 1].kind == Tok::Str) np->comment = toks[++i].text;
      else return make_error("ALTER " + std::string(kind_word(kind)) + ": only ENABLE, DISABLE and COMMENT are supported; use CREATE OR REPLACE");
    }
    np->modified_ns = kernel_.host().wall_ns();
    // The stored text says the new state, so it survives a restart.
    std::string t = np->create_text;
    if (kind == 'E') {
      const size_t dpos = upper_of(t).rfind(" DO ");
      std::string head = t.substr(0, dpos), tail = t.substr(dpos);
      head = std::regex_replace(head, std::regex("\\s+(ENABLE|DISABLE(\\s+ON\\s+SLAVE)?)\\b", std::regex::icase), "");
      t = head + (np->enabled ? " ENABLE" : " DISABLE") + tail;
      np->create_text = t;
    }
    it->second = np;
    altered = true;
    SqlQueryResult r;
    r.formatted_table = std::string(kind_word(kind)) + " " + pname + " altered.";
    return r;
  }
  return make_error("unsupported stored-program statement");
}

// ---- Running ------------------------------------------------------------------

namespace {
// Recursion: MariaDB refuses a procedure that calls itself unless
// max_sp_recursion_depth allows it, and a function always; this is the bound.
thread_local int tls_program_depth = 0;
// The tables a statement in progress is changing, which a trigger or function
// it fires may not write (MariaDB's error 1442).
thread_local std::vector<const SqlTable*> tls_tables_in_use;
// Also: the statements a program runs are their own; the outer statement's
// "plain INSERT, append only" does not describe them (a trigger's UPDATE of
// another table must be captured whole for an undo).
struct DepthGuard {
  bool ok;
  bool append_only;
  DepthGuard() : ok(++tls_program_depth <= 32), append_only(tls_txn_append_only) { tls_txn_append_only = false; }
  ~DepthGuard() { --tls_program_depth; tls_txn_append_only = append_only; }
};

bool sql_table_in_use_by_trigger(const SqlTable* t) {
  for (const SqlTable* u : tls_tables_in_use) if (u == t) return true;
  return false;
}

void psm::Run::call(const std::string& text, Condition* c) {
  // CALL p(args): IN values bound here, OUT/INOUT written back to this run's
  // variables (or @variables), as a nested CALL does.
  std::string s = trim(text);
  s = trim(s.substr(4));
  const size_t lp = s.find('(');
  std::string name = trim(lp == std::string::npos ? s : s.substr(0, lp));
  std::vector<std::string> args;
  if (lp != std::string::npos) {
    const size_t rp = s.rfind(')');
    if (rp != std::string::npos && rp > lp)
      for (const std::string& a : split_top_level_commas(s.substr(lp + 1, rp - lp - 1))) if (!trim(a).empty()) args.push_back(trim(a));
  }
  auto proc = db.find_program('P', name);
  if (!proc) {
    // Not a stored procedure: the engine's own (getNextTransNo) with values bound.
    SqlQueryResult r;
    if (!exec(text, &r, c)) { raise(*c); return; }
    return;
  }
  std::vector<Val> in(proc->params.size(), val_null());
  for (size_t k = 0; k < proc->params.size() && k < args.size(); ++k) {
    if (proc->params[k].mode == 1) continue;
    if (!eval(args[k], &in[k], c)) { raise(*c); return; }
  }
  std::vector<Val> out;
  std::string err;
  auto r = sql_run_procedure(db, *proc, in, &out, session_db, &err);
  if (!err.empty()) { c->kind = Condition::Exception; c->message = err; raise(*c); return; }
  for (auto& rs : r) if (allow_results) result_sets.push_back(std::move(rs));
  for (size_t k = 0; k < proc->params.size() && k < args.size() && k < out.size(); ++k) {
    if (proc->params[k].mode == 0) continue;
    if (!assign(args[k], out[k], c)) { raise(*c); return; }
  }
}

std::vector<SqlQueryResult> sql_run_procedure(SqlDatabase& db, const SqlDatabase::StoredProgram& p, const std::vector<Val>& in,
                                              std::vector<Val>* out, const std::string& session_db, std::string* err) {
  DepthGuard dg;
  if (!dg.ok) { *err = "Recursive limit 32 (as max_sp_recursion_depth variable) was exceeded for routine " + p.name; return {}; }
  psm::Run run(db, p.db.empty() ? session_db : p.db);
  for (size_t k = 0; k < p.params.size(); ++k) run.scopes[0][lower_of(p.params[k].name)] = k < in.size() ? in[k] : val_null();
  run.run_list(psm::body_of(p), false);
  if (!run.error.empty()) { *err = run.error; return {}; }
  if (out) {
    out->clear();
    for (const auto& prm : p.params) out->push_back(run.scopes[0][lower_of(prm.name)]);
  }
  return std::move(run.result_sets);
}

Val sql_run_function(SqlDatabase& db, const SqlDatabase::StoredProgram& f, const std::vector<Val>& args,
                     const std::string& session_db, std::string* err) {
  DepthGuard dg;
  if (!dg.ok) { *err = "Recursive stored functions and triggers are not allowed"; return val_null(); }
  if (args.size() != f.params.size()) {
    *err = "Incorrect number of arguments for FUNCTION " + f.db + "." + f.name + "; expected " +
           std::to_string(f.params.size()) + ", got " + std::to_string(args.size());
    return val_null();
  }
  psm::Run run(db, f.db.empty() ? session_db : f.db);
  run.allow_results = false;
  for (size_t k = 0; k < f.params.size(); ++k) run.scopes[0][lower_of(f.params[k].name)] = args[k];
  run.run_list(psm::body_of(f), false);
  if (!run.error.empty()) { *err = run.error; return val_null(); }
  if (!run.returned) { *err = "No RETURN found in FUNCTION " + f.db + "." + f.name; return val_null(); }
  return run.ret;
}

Val sql_run_aggregate(SqlDatabase& db, const SqlDatabase::StoredProgram& f, const std::vector<std::vector<Val>>& rows,
                      const std::string& session_db, std::string* err) {
  DepthGuard dg;
  if (!dg.ok) { *err = "Recursive stored functions and triggers are not allowed"; return val_null(); }
  if (!rows.empty() && rows[0].size() != f.params.size()) {
    *err = "Incorrect number of arguments for FUNCTION " + f.db + "." + f.name + "; expected " +
           std::to_string(f.params.size()) + ", got " + std::to_string(rows[0].size());
    return val_null();
  }
  psm::Run run(db, f.db.empty() ? session_db : f.db);
  run.allow_results = false;
  for (size_t k = 0; k < f.params.size(); ++k) run.scopes[0][lower_of(f.params[k].name)] = val_null();
  run.group_rows = &rows;
  run.group_fn = &f;
  run.run_list(psm::body_of(f), false);
  if (!run.error.empty()) { *err = run.error; return val_null(); }
  if (!run.returned) { *err = "No RETURN found in FUNCTION " + f.db + "." + f.name; return val_null(); }
  return run.ret;
}

// One row's trigger. new_row may be changed by a BEFORE trigger.
bool sql_fire_trigger(SqlDatabase& db, const SqlDatabase::StoredProgram& t, const SqlTable& table, const SqlRow* old_row,
                      SqlRow* new_row, std::string* err) {
  DepthGuard dg;
  if (!dg.ok) { *err = "Recursive stored functions and triggers are not allowed"; return false; }
  // An INSERT the trigger runs leaves the caller's ids as they were, as on
  // MariaDB: an audit trigger's history row is not the id the client asked for.
  struct KeepIds {
    uint64_t last = tls_last_insert_id, stmt = tls_statement_insert_id;
    ~KeepIds() { tls_last_insert_id = last; tls_statement_insert_id = stmt; }
  } keep_ids;
  psm::Run run(db, t.db);
  run.allow_results = false;
  run.trig_table = &table;
  run.old_row = old_row;
  run.new_row = new_row;
  run.new_writable = t.timing == "BEFORE" && new_row;
  tls_tables_in_use.push_back(&table);
  ++tls_trigger_depth;
  run.run_list(psm::body_of(t), false);
  --tls_trigger_depth;
  tls_tables_in_use.pop_back();
  if (!run.error.empty()) { *err = run.error; return false; }
  return true;
}

// Every trigger for one row: BEFORE ones (which may change new_row) or AFTER ones.
bool sql_fire_triggers(SqlDatabase& db, const std::vector<std::shared_ptr<const SqlDatabase::StoredProgram>>& trigs,
                       const char* timing, const SqlTable& table, const SqlRow* old_row, SqlRow* new_row, std::string* err) {
  for (const auto& t : trigs) {
    if (t->timing != timing) continue;
    if (!sql_fire_trigger(db, *t, table, old_row, new_row, err)) return false;
  }
  return true;
}
}  // namespace

void SqlDatabase::run_due_events() {
  if (!event_scheduler_.load(std::memory_order_acquire) || !has_events_.load(std::memory_order_acquire)) return;
  if (g_cdc_thread) return;
  const int64_t now = (int64_t)(kernel_.host().wall_ns() / 1000000000ull);
  std::vector<std::shared_ptr<const StoredProgram>> due;
  std::vector<std::string> finished;
  {
    hal::LockGuard g(programs_mu_);
    for (const auto& kv : programs_) {
      const StoredProgram& e = *kv.second;
      if (e.kind != 'E' || !e.enabled) continue;
      const int64_t last = e.last_run.load();
      if (e.at) {
        if (now >= e.at_s && last == 0) { due.push_back(kv.second); if (!e.preserve) finished.push_back(kv.first); }
        continue;
      }
      if (e.ends_s && now > e.ends_s) { if (!e.preserve) finished.push_back(kv.first); continue; }
      const int64_t start = e.starts_s ? e.starts_s : (int64_t)(e.created_ns / 1000000000ull);
      if (now < start) continue;
      // The step in seconds; MONTH, QUARTER and YEAR are calendar steps, taken
      // by the engine's own date arithmetic from the last run.
      if (last == 0) { due.push_back(kv.second); continue; }
      SqlQueryResult r = const_cast<SqlDatabase*>(this)->execute_local_query_unguarded(
          "SELECT UNIX_TIMESTAMP(FROM_UNIXTIME(" + std::to_string(last) + ") + INTERVAL (" + e.every_n + ") " + e.every_unit + ")", e.db);
      if (r.success && !r.rows.empty() && !r.rows[0].values.empty() && !sql_is_null(r.rows[0].values[0]) &&
          now >= (int64_t)std::strtoll(r.rows[0].values[0].c_str(), nullptr, 10))
        due.push_back(kv.second);
    }
  }
  for (const auto& e : due) {
    e->last_run.store(now);
    std::vector<Val> out;
    std::string err;
    sql_run_procedure(*this, *e, {}, &out, e->db, &err);
    if (!err.empty()) fprintf(stderr, "[asql] event %s.%s: %s\n", e->db.c_str(), e->name.c_str(), err.c_str());
  }
  if (!finished.empty()) {
    {
      hal::LockGuard g(programs_mu_);
      for (const auto& k : finished) programs_.erase(k);
    }
    refresh_program_flags();
    save_programs();
  }
}

// ---- Listing ----------------------------------------------------------------

SqlQueryResult SqlDatabase::show_create_program(char kind, const std::string& name) {
  auto p = find_program(kind, name);
  if (!p) return make_error(std::string(kind_word(kind)) + " " + name + " does not exist");
  SqlQueryResult r;
  r.is_result_set = true;
  if (kind == 'T') {
    r.columns = {"Trigger", "sql_mode", "SQL Original Statement", "character_set_client", "collation_connection", "Database Collation", "Created"};
    r.rows.push_back(SqlRow{{p->name, "", p->create_text, "utf8mb4", "utf8mb4_general_ci", "utf8mb4_general_ci",
                             format_datetime((std::time_t)(p->created_ns / 1000000000ull), false)}});
  } else if (kind == 'E') {
    r.columns = {"Event", "sql_mode", "time_zone", "Create Event", "character_set_client", "collation_connection", "Database Collation"};
    r.rows.push_back(SqlRow{{p->name, "", "SYSTEM", p->create_text, "utf8mb4", "utf8mb4_general_ci", "utf8mb4_general_ci"}});
  } else {
    r.columns = {kind == 'P' ? "Procedure" : "Function", "sql_mode", kind == 'P' ? "Create Procedure" : "Create Function",
                 "character_set_client", "collation_connection", "Database Collation"};
    r.rows.push_back(SqlRow{{p->name, "", p->create_text, "utf8mb4", "utf8mb4_general_ci", "utf8mb4_general_ci"}});
  }
  r.rows_affected = 1;
  r.formatted_table = format_result_table(r.columns, r.rows);
  return r;
}

std::vector<std::shared_ptr<const SqlDatabase::StoredProgram>> SqlDatabase::list_programs(char kind) const {
  std::vector<std::shared_ptr<const StoredProgram>> out;
  hal::LockGuard g(programs_mu_);
  for (const auto& kv : programs_) if (kv.second->kind == kind) out.push_back(kv.second);
  return out;
}
