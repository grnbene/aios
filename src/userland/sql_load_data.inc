// LOAD DATA [LOW_PRIORITY | CONCURRENT] [LOCAL] INFILE 'file' [REPLACE | IGNORE] INTO TABLE t
//   [CHARACTER SET cs]
//   [{FIELDS | COLUMNS} [TERMINATED BY 's'] [[OPTIONALLY] ENCLOSED BY 'c'] [ESCAPED BY 'c']]
//   [LINES [STARTING BY 's'] [TERMINATED BY 's']]
//   [IGNORE n {LINES | ROWS}]
//   [(col_or_@var, ...)]
//   [SET col = expr, ...]
// as MariaDB reads it (included by sql_database.cpp, after execute_insert).
//
// The file's rows go through execute_insert as values - the same defaults, conversions, keys, auto-increment,
// triggers and foreign keys as an INSERT of them - 16,384 at a time, with no SQL text built or parsed for them.
// The whole load is one statement: its own transaction when none is open (a failure part-way leaves no row of it),
// else a savepoint of the one that is. What is logged - the redo log, and from it the binlog - is each batch as the
// INSERT of its values, so a replay or a replica never reads the file (a LOCAL one was never on this machine).
//
// Where the bytes come from:
//   - LOCAL over the wire: what the client sent (sql_set_local_infile); a client that did not send it is error
//     4166, as MariaDB answers.
//   - LOCAL not over the wire: the file itself (the client is this process).
//   - Without LOCAL: a file on the server, only under AIOS_SQL_SECURE_FILE_PRIV (MariaDB's secure_file_priv). Unset,
//     the statement is refused (error 1290): a SQL login is not handed the server's files - /etc/aios/asql.conf
//     among them - unless the operator names a directory for it. A relative name is in that directory; a path out
//     of it, through ".." or a symbolic link, is refused.
// Under a capability (a scoped ASQL login) LOAD is refused before it gets here (authorize_statement).

namespace {
thread_local bool tls_infile_over_wire = false;
thread_local const std::string* tls_infile_data = nullptr;

struct LoadFormat {
  std::string field_term = "\t";
  std::string line_term = "\n";
  std::string line_start;
  int enclosed = -1;       // ENCLOSED BY's character, or none
  bool optionally = false; // OPTIONALLY ENCLOSED BY: on reading, the same as ENCLOSED BY
  int escape = '\\';       // ESCAPED BY's character, or none (ESCAPED BY '')
};

// One field as read: its bytes, or NULL.
struct LoadField {
  std::string text;
  bool null = false;
};

// The file's lines, field by field, as MariaDB splits them (sql/sql_load.cc's READ_INFO): a field is enclosed when it
// starts with the ENCLOSED BY character, and then ends at that character followed by a field or line terminator (the
// character twice is one of it); otherwise it ends at the first terminator. The escape character takes the one after
// it literally, except \0 \b \n \r \t \Z, which are those bytes, and \N - the whole field - which is NULL. With
// ENCLOSED BY set, an unenclosed field that is the word NULL is NULL as well.
class LoadReader {
 public:
  LoadReader(const char* data, size_t size, const LoadFormat& f) : p_(data), end_(data + size), f_(f) {
    // The bytes that can end or change an unenclosed field: a terminator's first byte, the escape character.
    if (!f_.field_term.empty()) special_[(unsigned char)f_.field_term[0]] = 1;
    if (!f_.line_term.empty()) special_[(unsigned char)f_.line_term[0]] = 1;
    if (f_.escape >= 0) special_[(unsigned char)f_.escape] = 1;
  }
  bool at_end() const { return p_ >= end_; }

  // The next line's fields into *out. False at the end of the data. A line without LINES STARTING BY's prefix is
  // passed over whole, as MariaDB does; the text before the prefix is not part of the line.
  bool next_line(std::vector<LoadField>* out) {
    out->clear();
    while (p_ < end_) {
      if (!f_.line_start.empty()) {
        const char* line_end = find(p_, f_.line_term);
        const char* at = find_in(p_, line_end, f_.line_start);
        if (!at) { p_ = line_end == end_ ? end_ : line_end + f_.line_term.size(); continue; }
        p_ = at + f_.line_start.size();
      }
      read_fields(out);
      return true;
    }
    return false;
  }

 private:
  const char* p_;
  const char* end_;
  const LoadFormat& f_;
  uint8_t special_[256] = {};

  bool at(const char* q, const std::string& s) const {
    return !s.empty() && (size_t)(end_ - q) >= s.size() && std::memcmp(q, s.data(), s.size()) == 0;
  }
  const char* find(const char* from, const std::string& s) const {
    if (s.empty()) return end_;
    for (const char* q = from; q < end_; ++q) if (*q == s[0] && at(q, s)) return q;
    return end_;
  }
  const char* find_in(const char* from, const char* to, const std::string& s) const {
    for (const char* q = from; q + s.size() <= to; ++q) if (std::memcmp(q, s.data(), s.size()) == 0) return q;
    return nullptr;
  }
  static char unescape(char c) {
    switch (c) {
      case '0': return '\0';
      case 'b': return '\b';
      case 'n': return '\n';
      case 'r': return '\r';
      case 't': return '\t';
      case 'Z': return '\x1a';
      default: return c;
    }
  }

  void read_fields(std::vector<LoadField>* out) {
    for (;;) {
      LoadField fld;
      bool line_done = false;
      if (f_.enclosed >= 0 && p_ < end_ && (unsigned char)*p_ == (unsigned)f_.enclosed) {
        // enclosed: to the closing character that a terminator follows
        const char q = (char)f_.enclosed;
        ++p_;
        for (;;) {
          if (p_ >= end_) { line_done = true; break; }
          const char c = *p_;
          if (f_.escape >= 0 && (unsigned char)c == (unsigned)f_.escape && p_ + 1 < end_) {
            fld.text += unescape(p_[1]);
            p_ += 2;
            continue;
          }
          if (c == q) {
            if (p_ + 1 < end_ && p_[1] == q) { fld.text += q; p_ += 2; continue; }
            const char* after = p_ + 1;
            if (after >= end_) { p_ = end_; line_done = true; break; }
            if (at(after, f_.field_term)) { p_ = after + f_.field_term.size(); break; }
            if (at(after, f_.line_term)) { p_ = after + f_.line_term.size(); line_done = true; break; }
          }
          fld.text += c;
          ++p_;
        }
      } else {
        bool escaped_any = false;
        for (;;) {
          if (p_ >= end_) { line_done = true; break; }
          // A run of bytes none of which is special: the field's text, as the steps below would add it a byte at
          // a time (no terminator starts at any of them, and none is the escape) - one append instead of two
          // compares and an append per byte.
          if (!special_[(unsigned char)*p_]) {
            const char* q = p_ + 1;
            while (q < end_ && !special_[(unsigned char)*q]) ++q;
            if (fld.null) fld.null = false;
            fld.text.append(p_, (size_t)(q - p_));
            p_ = q;
            continue;
          }
          if (at(p_, f_.field_term)) { p_ += f_.field_term.size(); break; }
          if (at(p_, f_.line_term)) { p_ += f_.line_term.size(); line_done = true; break; }
          const char c = *p_;
          if (f_.escape >= 0 && (unsigned char)c == (unsigned)f_.escape && p_ + 1 < end_) {
            if (p_[1] == 'N' && fld.text.empty() && !escaped_any) {
              // \N is NULL only as the whole field
              const char* after = p_ + 2;
              if (after >= end_ || at(after, f_.field_term) || at(after, f_.line_term)) { fld.null = true; p_ = after; continue; }
            }
            fld.text += unescape(p_[1]);
            escaped_any = true;
            p_ += 2;
            continue;
          }
          if (fld.null) fld.null = false;   // \N followed by more: its text after all
          fld.text += c;
          ++p_;
        }
        if (!fld.null && f_.enclosed >= 0 && !escaped_any && fld.text == "NULL") { fld.null = true; fld.text.clear(); }
      }
      out->push_back(std::move(fld));
      if (line_done) return;
    }
  }
};

// The ISO-8859-1 bytes of a CHARACTER SET latin1 file as UTF-8.
std::string latin1_to_utf8(const char* p, size_t n) {
  std::string out;
  out.reserve(n + n / 8);
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = (unsigned char)p[i];
    if (c < 0x80) out += (char)c;
    else { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
  }
  return out;
}

// sql_quote_literal's text appended to `out` in place: one pass, no temporary per value (a 500,000-row load logs
// three million of them).
void append_sql_literal(std::string& out, const std::string& s) {
  out += '\'';
  size_t from = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c != '\'' && c != '\\') continue;
    out.append(s, from, i - from);
    out += c == '\'' ? "''" : "\\\\";
    from = i + 1;
  }
  out.append(s, from, std::string::npos);
  out += '\'';
}

// `path` with "." and ".." taken out and repeated slashes folded; false when ".." climbs above its start.
bool normalize_load_path(const std::string& path, std::string* out) {
  std::vector<std::string> parts;
  const bool absolute = !path.empty() && path[0] == '/';
  for (size_t i = 0; i <= path.size();) {
    size_t j = path.find('/', i);
    if (j == std::string::npos) j = path.size();
    const std::string part = path.substr(i, j - i);
    if (part.empty() || part == ".") {}
    else if (part == "..") { if (parts.empty()) return false; parts.pop_back(); }
    else parts.push_back(part);
    i = j + 1;
  }
  std::string r = absolute ? "/" : "";
  for (size_t k = 0; k < parts.size(); ++k) r += (k ? "/" : "") + parts[k];
  *out = r;
  return true;
}
}  // namespace

namespace {
// Whether a load reads its file the plain way: set while a load that found a value it cannot take as it stands
// (see LoadColCheck) is run again with every value converted, as an INSERT's would be.
thread_local bool tls_load_canon_off = false;

// What it takes for a field read from a file to be stored exactly as it is read - the text the column holds is the
// file's text, no conversion, no clamping, no warning - for the column types a load commonly has. Each is the
// condition under which sql_normalize_value, lenient_cell_value and strict_cell_error all leave the value alone
// (they are the rules it is held to, and a value that is not plainly so is left to them: the load runs again the
// ordinary way). Anything else about the column - a type not listed, a binary, enum or set, a TIME - is Unsupported.
struct LoadColCheck {
  enum Kind { Unsupported, Int, Real, Decimal, Varchar, FreeText, Date } kind = Unsupported;
  AIOS_INT128 lo = 0, hi = 0;   // Int: the type's range
  int scale = 0, precision = 0;
  size_t maxlen = 0;            // Varchar: at most this many bytes (so characters)
  bool char_pad = false;        // CHAR(n): a trailing space is cut on the way in
  bool tiny = false;            // TINYTEXT: at most 255 bytes
  bool is_unsigned = false;
  SqlDataType type = SqlDataType::Text;
  SqlColumn col;                // canonical_temporal reads the column
};

LoadColCheck make_load_col_check(const SqlColumn& c) {
  LoadColCheck k;
  k.col = c;
  if (c.auto_increment || !c.generated.empty()) return k;
  const std::string d = lower_of(c.declared);
  k.is_unsigned = d.find(" unsigned") != std::string::npos;
  k.type = c.type;
  if (c.temporal == 1 || c.temporal == 2) { k.kind = LoadColCheck::Date; return k; }
  if (c.temporal != 0) return k;
  switch (c.type) {
    case SqlDataType::Int:
      if (!d.empty() && int_type_range(d, &k.lo, &k.hi)) k.kind = LoadColCheck::Int;
      return k;
    case SqlDataType::Real:
      if (c.scale <= 0) { k.kind = LoadColCheck::Real; k.scale = c.scale; }
      return k;
    case SqlDataType::Decimal:
      if (c.precision > 0) { k.kind = LoadColCheck::Decimal; k.scale = (int)c.scale; k.precision = (int)c.precision; }
      return k;
    case SqlDataType::Text: {
      if (c.binary != 0) return k;
      const bool is_varchar = d.rfind("varchar", 0) == 0, is_char = d.rfind("char", 0) == 0;
      if (is_varchar || is_char) {
        const size_t a = d.find('(');
        if (a == std::string::npos) { k.kind = LoadColCheck::FreeText; return k; }
        const long n = std::atol(d.c_str() + a + 1);
        if (n < 0) return k;
        k.kind = LoadColCheck::Varchar;
        k.maxlen = (size_t)n;
        k.char_pad = is_char;
        return k;
      }
      if (d.rfind("tinytext", 0) == 0) { k.kind = LoadColCheck::FreeText; k.tiny = true; return k; }
      if (d.rfind("text", 0) == 0 || d.rfind("mediumtext", 0) == 0 || d.rfind("longtext", 0) == 0) k.kind = LoadColCheck::FreeText;
      return k;
    }
    default:
      return k;
  }
}

bool load_field_plain(const LoadColCheck& k, const std::string& v) {
  switch (k.kind) {
    case LoadColCheck::Int: {
      const size_t s0 = !v.empty() && v[0] == '-' ? 1 : 0;
      const size_t nd = v.size() - s0;
      if (nd < 1 || nd > 18 || (v[s0] == '0' && !(nd == 1 && s0 == 0))) return false;
      int64_t x = 0;
      for (size_t i = s0; i < v.size(); ++i) {
        if (v[i] < '0' || v[i] > '9') return false;
        x = x * 10 + (v[i] - '0');
      }
      const AIOS_INT128 xv = s0 ? -(AIOS_INT128)x : (AIOS_INT128)x;
      return xv >= k.lo && xv <= k.hi;
    }
    case LoadColCheck::Real:
      return !v.empty() && !(k.is_unsigned && v[0] == '-') && normalized_already(SqlDataType::Real, k.scale, v);
    case LoadColCheck::Decimal:
      return !v.empty() && !(k.is_unsigned && v[0] == '-') && normalized_already(SqlDataType::Decimal, k.scale, v) &&
             decimal_text_out_of_range(v, k.precision, k.scale) == 0;
    case LoadColCheck::Varchar:
      return v.size() <= k.maxlen && !(k.char_pad && !v.empty() && v.back() == ' ');
    case LoadColCheck::FreeText:
      return !k.tiny || v.size() <= 255;
    case LoadColCheck::Date:
      return !v.empty() && canonical_temporal(k.col, v) == v;
    default:
      return false;
  }
}
// The text a field is stored as, when that is plain (load_field_plain): the field as it is, or - for a number - what
// sql_normalize_value makes of it first, as the insert of the row would (448.80 into a DOUBLE is 448.8, 1.5 into a
// DECIMAL(12,2) is 1.50, 007 into an INT is 7), checked again. False: the value is left to the ordinary way.
bool load_field_canon(const LoadColCheck& k, std::string& v) {
  if (load_field_plain(k, v)) return true;
  if (v.empty() || (k.kind != LoadColCheck::Int && k.kind != LoadColCheck::Real && k.kind != LoadColCheck::Decimal)) return false;
  std::string n = sql_normalize_value(k.type, k.scale, v);
  if (n == v || !load_field_plain(k, n)) return false;
  v = std::move(n);
  return true;
}
}  // namespace

void sql_set_local_infile(bool over_wire, const std::string* data) {
  tls_infile_over_wire = over_wire;
  tls_infile_data = data;
}

SqlQueryResult SqlDatabase::execute_load_data(const std::string& sql) {
  std::vector<Token> toks;
  std::string terr;
  if (!tokenize_sql(sql, &toks, &terr)) return make_error(terr);
  SqlParser p(toks);
  p.set_source(&sql);
  p.take_word("LOAD");
  if (p.is_word("XML")) return make_error("LOAD XML is not supported");
  if (!p.take_word("DATA")) return make_error("expected DATA after LOAD");
  if (!p.take_word("LOW_PRIORITY")) p.take_word("CONCURRENT");
  const bool local = p.take_word("LOCAL");
  if (!p.take_word("INFILE")) return make_error("expected INFILE");
  if (p.peek().kind != Tok::Str) return make_error("expected the file name as a quoted string after INFILE");
  const std::string file = p.next().text;
  const bool replace = p.take_word("REPLACE");
  const bool ignore = !replace && p.take_word("IGNORE");
  if (!p.take_word("INTO") || !p.take_word("TABLE")) return make_error("expected INTO TABLE");
  std::string name;
  if (!p.parse_name(&name)) return make_error(p.error());
  if (p.is_word("PARTITION")) return make_error("LOAD DATA ... PARTITION is not supported");

  std::string charset;
  if (p.is_word("CHARACTER") && p.is_word("SET", 1)) {
    p.next(); p.next();
    const Token& cs = p.next();
    charset = lower_of(cs.text);
  } else if (p.take_word("CHARSET")) {
    charset = lower_of(p.next().text);
  }
  const bool latin1 = charset == "latin1";
  if (!charset.empty() && !latin1 && charset != "utf8" && charset != "utf8mb4" && charset != "utf8mb3" &&
      charset != "binary" && charset != "ascii")
    return make_error("LOAD DATA CHARACTER SET " + charset + " is not supported (utf8mb4, utf8, latin1, binary, ascii)");

  LoadFormat fmt;
  auto take_str = [&](std::string* out) -> bool {
    if (p.peek().kind != Tok::Str) return false;
    *out = p.next().text;
    return true;
  };
  auto one_char = [&](const std::string& s, const char* what, int* out) -> std::string {
    if (s.size() > 1) return std::string(what) + " must be a single character";
    *out = s.empty() ? -1 : (unsigned char)s[0];
    return "";
  };
  if (p.take_word("FIELDS") || p.take_word("COLUMNS")) {
    bool any = false;
    for (;;) {
      std::string s;
      if (p.is_word("TERMINATED") && p.is_word("BY", 1)) {
        p.next(); p.next();
        if (!take_str(&s)) return make_error("expected a string after FIELDS TERMINATED BY");
        fmt.field_term = s;
      } else if (p.is_word("OPTIONALLY") || (p.is_word("ENCLOSED") && p.is_word("BY", 1))) {
        if (p.take_word("OPTIONALLY")) fmt.optionally = true;
        if (!p.take_word("ENCLOSED") || !p.take_word("BY")) return make_error("expected ENCLOSED BY");
        if (!take_str(&s)) return make_error("expected a string after ENCLOSED BY");
        if (std::string e = one_char(s, "FIELDS ENCLOSED BY", &fmt.enclosed); !e.empty()) return make_error(e);
      } else if (p.is_word("ESCAPED") && p.is_word("BY", 1)) {
        p.next(); p.next();
        if (!take_str(&s)) return make_error("expected a string after ESCAPED BY");
        if (std::string e = one_char(s, "FIELDS ESCAPED BY", &fmt.escape); !e.empty()) return make_error(e);
      } else break;
      any = true;
    }
    if (!any) return make_error("expected TERMINATED BY, ENCLOSED BY or ESCAPED BY after FIELDS");
  }
  if (p.take_word("LINES")) {
    bool any = false;
    for (;;) {
      std::string s;
      if (p.is_word("STARTING") && p.is_word("BY", 1)) {
        p.next(); p.next();
        if (!take_str(&s)) return make_error("expected a string after LINES STARTING BY");
        fmt.line_start = s;
      } else if (p.is_word("TERMINATED") && p.is_word("BY", 1)) {
        p.next(); p.next();
        if (!take_str(&s)) return make_error("expected a string after LINES TERMINATED BY");
        fmt.line_term = s;
      } else break;
      any = true;
    }
    if (!any) return make_error("expected STARTING BY or TERMINATED BY after LINES");
  }
  if (fmt.field_term.empty() && fmt.enclosed < 0)
    return make_error("LOAD DATA with FIELDS TERMINATED BY '' and no ENCLOSED BY (fixed-width rows) is not supported");
  if (fmt.line_term.empty()) return make_error("LINES TERMINATED BY '' is not supported");
  size_t skip_lines = 0;
  if (p.take_word("IGNORE")) {
    if (p.peek().kind != Tok::Num) return make_error("expected a number after IGNORE");
    skip_lines = (size_t)std::strtoull(p.next().text.c_str(), nullptr, 10);
    if (!p.take_word("LINES") && !p.take_word("ROWS")) return make_error("expected LINES or ROWS after IGNORE n");
  }

  if (find_view(name)) return make_error("LOAD DATA into a view is not supported: " + name);
  SqlTable* t = resolve_table(name);
  if (!t) return make_error("table not found: " + name);

  // The fields in order: a column (its index) or a user variable (@name, ci = -1).
  struct Target { int ci = -1; std::string var; };
  std::vector<Target> targets;
  if (p.take_punct("(")) {
    if (!p.is_punct(")")) {
      do {
        const Token& tk = p.peek();
        if (tk.kind == Tok::Ident && !tk.text.empty() && tk.text[0] == '@') {
          targets.push_back({-1, p.next().text});
          continue;
        }
        std::string cn;
        if (!p.parse_name(&cn)) return make_error(p.error());
        const size_t dot = cn.rfind('.');
        const int ci = t->get_column_index(dot == std::string::npos ? cn : cn.substr(dot + 1));
        if (ci < 0) return make_error("Unknown column '" + cn + "' in 'field list'");
        targets.push_back({ci, ""});
      } while (p.take_punct(","));
    }
    if (!p.take_punct(")")) return make_error("expected ')' after the column list");
  } else {
    for (size_t c = 0; c < t->columns.size(); ++c) targets.push_back({(int)c, ""});
  }
  struct SetCol { int ci; ExprPtr e; };
  std::vector<SetCol> sets;
  if (p.take_word("SET")) {
    do {
      std::string cn;
      if (!p.parse_name(&cn)) return make_error(p.error());
      const int ci = t->get_column_index(cn);
      if (ci < 0) return make_error("Unknown column '" + cn + "' in 'field list'");
      if (!p.take_punct("=")) return make_error("expected '=' after " + cn + " in SET");
      ExprPtr e = p.parse_expr();
      if (p.failed() || !e) return make_error(p.failed() ? p.error() : "expected an expression in SET");
      sets.push_back({ci, std::move(e)});
    } while (p.take_punct(","));
  }
  p.take_punct(";");
  if (!p.at_end()) return make_error("unexpected '" + p.peek().text + "' in LOAD DATA");

  // The INSERT's columns: the listed ones in order, then SET's not among them; a listed column SET also names takes
  // SET's value (MariaDB assigns SET after the fields).
  std::vector<int> ins_cols;
  std::vector<int> field_slot(targets.size(), -1);   // per field: its slot in a row, or -1 (a variable)
  std::vector<int> set_slot(sets.size(), -1);
  auto slot_of = [&](int ci) -> int {
    for (size_t k = 0; k < ins_cols.size(); ++k) if (ins_cols[k] == ci) return (int)k;
    ins_cols.push_back(ci);
    return (int)ins_cols.size() - 1;
  };
  for (size_t f = 0; f < targets.size(); ++f) if (targets[f].ci >= 0) field_slot[f] = slot_of(targets[f].ci);
  for (size_t s = 0; s < sets.size(); ++s) set_slot[s] = slot_of(sets[s].ci);
  bool any_var = false;
  for (const auto& tg : targets) if (tg.ci < 0) any_var = true;
  if (ins_cols.empty()) return make_error("LOAD DATA names no column of " + name + " to load");

  // A column a line has no field for: its default (a function default worked out now), else NULL where it may be,
  // else the type's own empty value: 0, '' or a zero date, as MariaDB gives it.
  auto missing_value = [&](int ci) -> Val {
    const SqlColumn& c = t->columns[(size_t)ci];
    if (column_has_default(c.default_value)) return val_cell(column_default_now(c.default_value));
    if (!c.not_null) return val_null();
    // NOT NULL with no default: the type's own empty value (MariaDB's implicit default), not an empty string
    if (c.temporal == 1) return val_text("0000-00-00");
    if (c.temporal == 2) return val_text("0000-00-00 00:00:00");
    if (c.temporal == 3) return val_text("00:00:00");
    if (sql_type_numeric(c.type)) return val_text("0");
    return val_text("");
  };

  // The bytes.
  std::shared_ptr<const aios::hal::FileView> view;
  const char* data = nullptr;
  size_t size = 0;
  if (local) {
    if (tls_infile_over_wire) {
      if (!tls_infile_data)
        return make_error("The used command is not allowed because the MariaDB server or client has disabled the local infile capability");
      data = tls_infile_data->data();
      size = tls_infile_data->size();
    } else {
      auto v = kernel_.host().view_file(file);
      if (!v.ok()) return make_error("File '" + file + "' not found (" + v.error().message + ")");
      view = *v;
    }
  } else {
    const char* dir_env = std::getenv("AIOS_SQL_SECURE_FILE_PRIV");
    if (!dir_env || !*dir_env)
      return make_error("The MariaDB server is running with the --secure-file-priv option so it cannot execute this statement");
    std::string dir, full;
    if (!normalize_load_path(dir_env, &dir) || dir.empty() || dir[0] != '/')
      return make_error("AIOS_SQL_SECURE_FILE_PRIV must be an absolute directory");
    if (!normalize_load_path(!file.empty() && file[0] == '/' ? file : dir + "/" + file, &full) ||
        (full != dir && full.compare(0, dir.size() + 1, dir + "/") != 0))
      return make_error("The MariaDB server is running with the --secure-file-priv option so it cannot execute this statement");
    // No symbolic link on the way from the directory to the file: one could point anywhere.
    for (size_t at = dir.size() + 1; at <= full.size(); ++at) {
      if (at != full.size() && full[at] != '/') continue;
      auto st = kernel_.host().stat(full.substr(0, at));
      if (!st.ok()) return make_error("File '" + file + "' not found (Errcode: 2 \"No such file or directory\")");
      if (st->is_link)
        return make_error("The MariaDB server is running with the --secure-file-priv option so it cannot execute this statement");
    }
    auto v = kernel_.host().view_file(full);
    if (!v.ok()) return make_error("File '" + file + "' not found (" + v.error().message + ")");
    view = *v;
  }
  if (view) { data = (const char*)view->data; size = view->size; }
  std::string converted;
  if (latin1 && size) { converted = latin1_to_utf8(data, size); data = converted.data(); size = converted.size(); }

  // One statement: its own transaction, or a savepoint of the open one.
  const bool own_txn = !tls_txn.open;
  if (own_txn) {
    SqlQueryResult b = execute_transaction("BEGIN");
    if (!b.success) return b;
  } else {
    SqlQueryResult s = execute_savepoint_statement("SAVEPOINT `__asql_load_data`");
    if (!s.success) return s;
  }
  auto undo_all = [&]() {
    if (own_txn) execute_transaction("ROLLBACK");
    else execute_savepoint_statement("ROLLBACK TO SAVEPOINT `__asql_load_data`");
  };

  // The INSERT each batch is, as written to the log; its head is also what execute_insert is given.
  const std::string verb = replace ? "REPLACE" : (ignore || local) ? "INSERT IGNORE" : "INSERT";
  std::string head = verb + " INTO " + quote_sql_ident(t->database.empty() ? effective_database() : t->database) + "." +
                     quote_sql_ident(t->name) + " (";
  for (size_t k = 0; k < ins_cols.size(); ++k) head += (k ? ", " : "") + quote_sql_ident(t->columns[(size_t)ins_cols[k]].name);
  head += ")";

  EvalCtx ctx;
  std::string verr;
  ctx.err = &verr;
  ctx.db = this;

  const uint64_t t_start = hal_now_ns();
  uint64_t ns_rec = 0, ns_ins = 0;
  LoadReader rd(data, size, fmt);
  std::vector<LoadField> fields;
  std::vector<std::vector<Val>> batch;
  std::vector<std::vector<std::string>> canon_rows;   // a batch of a plain load (below): rows already as stored
  FlatI64Set canon_keys;                              // its integer primary key's values so far
  bool use_canon = false;
  bool canon_unlogged = false;   // a batch went in with no row logged for undo: a failure cuts the table back instead
  const size_t rows_at_start = t->row_count();
  constexpr size_t kBatch = 16384;
  uint64_t records = 0, affected = 0, dups = 0, field_warnings = 0;
  size_t line_no = 0;
  std::string failure;

  // The batch as the INSERT that is logged.
  auto batch_text = [&head](const std::vector<std::vector<Val>>& rows, std::string& rec) {
    rec.clear();
    rec.reserve(head.size() + 16 + rows.size() * (rows[0].size() * 12 + 4));
    rec += head;
    rec += " VALUES ";
    for (size_t r = 0; r < rows.size(); ++r) {
      rec += r ? ",(" : "(";
      for (size_t k = 0; k < rows[r].size(); ++k) {
        const Val& v = rows[r][k];
        if (k) rec += ',';
        if (v.null) rec += "NULL";
        else append_sql_literal(rec, v.str());
      }
      rec += ')';
    }
  };
  auto batch_text_canon = [&head](const std::vector<std::vector<std::string>>& rows, std::string& rec) {
    rec.clear();
    rec.reserve(head.size() + 16 + rows.size() * (rows[0].size() * 12 + 4));
    rec += head;
    rec += " VALUES ";
    for (size_t r = 0; r < rows.size(); ++r) {
      rec += r ? ",(" : "(";
      for (size_t k = 0; k < rows[r].size(); ++k) {
        if (k) rec += ',';
        append_sql_literal(rec, rows[r][k]);
      }
      rec += ')';
    }
  };
  std::string* batch_rec = nullptr;   // the batch's text, made already (by the reader)
  int canon_pk = -1;   // set below, with the checks, before the first flush
  auto flush = [&]() -> bool {
    if (use_canon ? canon_rows.empty() : batch.empty()) return true;
    const uint64_t f0 = hal_now_ns();
    std::string rec;
    if (batch_rec) rec = std::move(*batch_rec);
    else batch_text(batch, rec);   // (a plain load's text is always made by its reader)
    batch_rec = nullptr;
    const size_t n = use_canon ? canon_rows.size() : batch.size();
    const uint64_t f1 = hal_now_ns();
    ns_rec += f1 - f0;
    SqlQueryResult r;
    if (use_canon) {
      LoadCanonBatch cb;
      cb.rows = &canon_rows;
      cb.pk = canon_pk;
      cb.keys = &canon_keys;
      // Its own transaction (not a savepoint of one the session has open): the table is held for the whole statement
      // - no other session writes it meanwhile and its rows stay where they are - so the rows need no undo entry
      // each: a failure cuts the table back to its length at the start, as an INSERT's own guard does.
      cb.no_undo = own_txn;
      tls_insert_canon = &cb;
      r = execute_insert(head);
      tls_insert_canon = nullptr;
      if (cb.undo_skipped) canon_unlogged = true;
      canon_rows.clear();
    } else {
      tls_insert_given = &batch;
      r = execute_insert(head);
      tls_insert_given = nullptr;
      batch.clear();
    }
    ns_ins += hal_now_ns() - f1;
    if (!r.success) { failure = r.error_message; return false; }
    affected += r.rows_affected;
    // "Records: n  Duplicates: d  Warnings: w" (a batch of more than one row), else the one row went in or met a key
    uint64_t d = 0;
    if (!r.ok_info.empty()) {
      const size_t dp = r.ok_info.find("Duplicates: ");
      if (dp != std::string::npos) d = std::strtoull(r.ok_info.c_str() + dp + 12, nullptr, 10);
    } else if (!replace && r.rows_affected == 0) {
      d = n;
    }
    dups += d;
    // Logged as the INSERT it was: with the statement clock where one is running, as the dispatcher stamps its own.
    if (!redo_replaying_ && !g_undo_applying) {
      if (g_stmt_clock_depth > 0 && !g_cdc_thread)
        rec = std::string(kRedoClockTag) + std::to_string((long long)g_stmt_clock_at) + "*/ " + rec;
      tls_txn.redo.push_back({effective_database(), std::move(rec)});
    }
    return true;
  };

  // A load with no SET and no @variable: the file is read on a thread of its own, a batch ahead, while this one
  // inserts the batch before - reading and splitting the lines was a quarter of a load's time, all of it while the
  // insert waited. The reader gives back each line's fields and which columns it gave; a column a line has no field
  // for takes its value here (missing_value: a default may be the statement's own time), in order, as before.
  const bool pipelined = sets.empty() && !any_var && size >= ((size_t)1 << 18);
  // A plain load: a plain INSERT of the file's rows, every column given in the table's order, into a table with no
  // AUTO_INCREMENT or generated column and no rule, each column of a kind LoadColCheck knows, and an integer primary
  // key (or none) of a table that is empty. Each value is then checked as it is read and, if it is stored exactly as
  // it reads, the rows travel as the text they are stored as (LoadCanonBatch) - no Val, no conversion pass. A value
  // that is not, or a line with the wrong number of fields, or a NULL, ends it: the load is taken back and run again
  // the ordinary way.
  std::vector<LoadColCheck> checks;
  bool canon_ok = pipelined && !tls_load_canon_off && verb == "INSERT" && fmt.enclosed < 0 && !g_cdc_thread &&
                  !redo_replaying_ && !seeding_ && ins_cols.size() == t->columns.size() && targets.size() == t->columns.size() &&
                  !t->has_rules();
  for (size_t k = 0; canon_ok && k < ins_cols.size(); ++k)
    if (ins_cols[k] != (int)k || field_slot[k] != (int)k) canon_ok = false;
  if (canon_ok) {
    std::vector<int> pks;
    for (size_t k = 0; k < t->columns.size(); ++k) {
      checks.push_back(make_load_col_check(t->columns[k]));
      if (checks.back().kind == LoadColCheck::Unsupported) canon_ok = false;
      if (t->columns[k].is_primary_key) pks.push_back((int)k);
    }
    if (canon_ok) {
      if (pks.size() == 1 && checks[(size_t)pks[0]].kind == LoadColCheck::Int && t->row_count() == 0) canon_pk = pks[0];
      else if (!pks.empty()) canon_ok = false;
    }
  }
  bool canon_abort = false;
  if (canon_ok && canon_pk >= 0) {
    // about as many keys as the file has lines (the first line's length is the guess)
    size_t first_len = 0;
    while (first_len < 4096 && first_len < size && data[first_len] != '\n') ++first_len;
    if (first_len > 0) canon_keys.reserve(size / (first_len + 1) + 1024);
  }
  if (pipelined) {
    struct Parsed {
      std::vector<std::vector<std::string>> canon;   // a plain load's rows, as stored
      bool failed = false;                            // a plain load met what it cannot take as it stands
      std::vector<std::vector<Val>> rows;
      std::vector<std::vector<uint8_t>> given;
      uint64_t warnings = 0;
      bool all_given = true;   // every line gave every column: `rec` is the batch's logged text
      std::string rec;
    };
    aios::hal::Mutex qmu;
    aios::hal::CondVar qcv;
    std::deque<Parsed> queue;
    bool reader_done = false, reader_stop = false;
    aios::hal::Thread reader([&] {
      size_t ln = 0;
      std::vector<LoadField> fl;
      for (;;) {
        Parsed p;
        if (canon_ok) {
          p.canon.reserve(kBatch);
        } else {
          p.rows.reserve(kBatch);
          p.given.reserve(kBatch);
        }
        bool end = false;
        while ((canon_ok ? p.canon.size() : p.rows.size()) < kBatch) {
          if (!rd.next_line(&fl)) { end = true; break; }
          if (++ln <= skip_lines) continue;
          if (canon_ok) {
            bool good = fl.size() == targets.size();
            std::vector<std::string> crow;
            if (good) {
              crow.reserve(fl.size());
              for (size_t f = 0; f < fl.size(); ++f) {
                if (fl[f].null || !load_field_canon(checks[f], fl[f].text)) { good = false; break; }
                crow.push_back(std::move(fl[f].text));
              }
            }
            if (!good) { p.failed = true; end = true; break; }
            p.canon.push_back(std::move(crow));
            continue;
          }
          std::vector<Val> row(ins_cols.size());
          std::vector<uint8_t> given(ins_cols.size(), 0);
          if (fl.size() != targets.size()) ++p.warnings;   // too few (1261) or too many (1262) fields
          for (size_t f = 0; f < targets.size() && f < fl.size(); ++f) {
            if (field_slot[f] < 0) continue;
            row[(size_t)field_slot[f]] = fl[f].null ? val_null() : val_text(std::move(fl[f].text));
            given[(size_t)field_slot[f]] = 1;
          }
          for (uint8_t g : given) if (!g) p.all_given = false;
          p.rows.push_back(std::move(row));
          p.given.push_back(std::move(given));
        }
        // The logged text too, here, while the insert runs (it was a tenth of the load on the inserting thread):
        // only when no line left a column to its default, which is worked out on that thread.
        if (!p.rows.empty() && p.all_given) batch_text(p.rows, p.rec);
        if (!p.canon.empty()) batch_text_canon(p.canon, p.rec);
        {
          std::unique_lock<aios::hal::Mutex> lk(qmu);
          qcv.wait(lk, [&] { return queue.size() < 2 || reader_stop; });
          if (reader_stop) return;
          if (!p.rows.empty() || !p.canon.empty() || p.failed) queue.push_back(std::move(p));
          if (end) reader_done = true;
        }
        qcv.notify_all();
        if (end) return;
      }
    });
    for (;;) {
      Parsed p;
      {
        std::unique_lock<aios::hal::Mutex> lk(qmu);
        qcv.wait(lk, [&] { return !queue.empty() || reader_done; });
        if (queue.empty()) break;
        p = std::move(queue.front());
        queue.pop_front();
      }
      qcv.notify_all();
      if (sql_interrupted()) { failure = "Query execution was interrupted"; break; }
      if (p.failed) { canon_abort = true; break; }
      if (canon_ok) {
        records += p.canon.size();
        canon_rows = std::move(p.canon);
        batch_rec = &p.rec;
        use_canon = true;
        if (!flush()) break;
        continue;
      }
      field_warnings += p.warnings;
      for (size_t i = 0; i < p.rows.size(); ++i)
        for (size_t k = 0; k < ins_cols.size(); ++k)
          if (!p.given[i][k]) p.rows[i][k] = missing_value(ins_cols[k]);
      records += p.rows.size();
      batch = std::move(p.rows);
      if (p.all_given) batch_rec = &p.rec;
      if (!flush()) break;
    }
    {
      std::lock_guard<aios::hal::Mutex> lk(qmu);
      reader_stop = true;
    }
    qcv.notify_all();
    reader.join();
  }
  if (canon_abort) {
    // what the plain way cannot take as it stands: everything taken back, and the load run again with every value
    // converted as an INSERT's would be
    if (canon_unlogged && t->row_count() > rows_at_start) cut_back_appended(*t, rows_at_start);
    undo_all();
    if (vx_trace_on()) vx_trace("load data: a value is not stored as it reads; the load is run again with every value converted");
    tls_load_canon_off = true;
    struct ResetCanon { ~ResetCanon() { tls_load_canon_off = false; } } reset_canon;
    return execute_load_data(sql);
  }
  while (!pipelined && rd.next_line(&fields)) {
    ++line_no;
    if (line_no <= skip_lines) continue;
    if (sql_interrupted()) { failure = "Query execution was interrupted"; break; }
    std::vector<Val> row(ins_cols.size());
    std::vector<bool> given(ins_cols.size(), false);
    if (fields.size() != targets.size()) ++field_warnings;   // too few (1261) or too many (1262) fields, as MariaDB counts them
    for (size_t f = 0; f < targets.size(); ++f) {
      const bool have = f < fields.size();
      if (field_slot[f] >= 0) {
        if (have) {
          row[(size_t)field_slot[f]] = fields[f].null ? val_null() : val_text(std::move(fields[f].text));
          given[(size_t)field_slot[f]] = true;
        }
      } else if (any_var) {
        user_var_set(targets[f].var, have ? (fields[f].null ? val_null() : val_text(fields[f].text)) : val_null());
      }
    }
    for (size_t s = 0; s < sets.size(); ++s) {
      verr.clear();
      Val v = eval_expr(sets[s].e.get(), ctx);
      if (!verr.empty()) { failure = verr; break; }
      row[(size_t)set_slot[s]] = v;
      given[(size_t)set_slot[s]] = true;
    }
    if (!failure.empty()) break;
    for (size_t k = 0; k < ins_cols.size(); ++k) if (!given[k]) row[k] = missing_value(ins_cols[k]);
    batch.push_back(std::move(row));
    ++records;
    if (batch.size() >= kBatch && !flush()) break;
  }
  if (failure.empty()) flush();
  const uint64_t t_read = hal_now_ns();
  if (!failure.empty()) {
    if (canon_unlogged && t->row_count() > rows_at_start) cut_back_appended(*t, rows_at_start);
    undo_all();
    return make_error(failure);
  }
  if (own_txn) {
    SqlQueryResult c = execute_transaction("COMMIT");
    if (!c.success) return c;
    if (vx_trace_on())
      vx_trace("load data: " + std::to_string(records) + " rows, read+insert " + std::to_string((t_read - t_start) / 1000) +
               " us (redo text " + std::to_string(ns_rec / 1000) + " us, execute_insert " + std::to_string(ns_ins / 1000) +
               " us), commit " + std::to_string((hal_now_ns() - t_read) / 1000) + " us" +
               (canon_ok ? (canon_unlogged ? ", plain load, no undo log" : ", plain load") : ""));
  } else {
    execute_savepoint_statement("RELEASE SAVEPOINT `__asql_load_data`");
  }
  SqlQueryResult res;
  res.rows_affected = affected;
  const uint64_t deleted = replace ? dups : 0, skipped = replace ? 0 : dups;
  res.formatted_table = std::to_string(affected) + " row(s) loaded.";
  res.ok_info = "Records: " + std::to_string(records) + "  Deleted: " + std::to_string(deleted) + "  Skipped: " +
                std::to_string(skipped) + "  Warnings: " + std::to_string(field_warnings + skipped);
  return res;
}
