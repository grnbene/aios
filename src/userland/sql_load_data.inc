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

// append_sql_literal for a view.
void append_sql_literal_sv(std::string& out, std::string_view s) {
  out += '\'';
  size_t from = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c != '\'' && c != '\\') continue;
    out.append(s.data() + from, i - from);
    out += c == '\'' ? "''" : "\\\\";
    from = i + 1;
  }
  out.append(s.data() + from, s.size() - from);
  out += '\'';
}

// load_field_plain's Int case on a view, with the value: plain digits, no leading zero, within the type's range.
inline bool load_int_plain(const LoadColCheck& k, std::string_view v, int64_t* out) {
  const size_t s0 = !v.empty() && v[0] == '-' ? 1 : 0;
  const size_t nd = v.size() - s0;
  if (nd < 1 || nd > 18 || (v[s0] == '0' && !(nd == 1 && s0 == 0))) return false;
  int64_t x = 0;
  for (size_t i = s0; i < v.size(); ++i) {
    const unsigned d = (unsigned)(v[i] - '0');
    if (d > 9) return false;
    x = x * 10 + (int64_t)d;
  }
  const AIOS_INT128 xv = s0 ? -(AIOS_INT128)x : (AIOS_INT128)x;
  if (xv < k.lo || xv > k.hi) return false;
  *out = s0 ? -x : x;
  return true;
}

// A DECIMAL(p,s) field already as stored, read as its scaled integer: [-]digits '.' s digits, no leading zero, not
// a negative zero, at most min(9, p - s) digits before the point - the text load_field_plain takes (normalized,
// within the precision) that decimal_fixed_scaled also takes. Anything else is left to those two.
inline bool load_decimal_plain(const LoadColCheck& k, std::string_view v, int places, int64_t* out) {
  const size_t s0 = !v.empty() && v[0] == '-' ? 1 : 0;
  if (s0 && k.is_unsigned) return false;
  if (v.size() < s0 + 2 + (size_t)places) return false;
  const size_t dot = v.size() - (size_t)places - 1;
  if (v[dot] != '.') return false;
  const size_t ilen = dot - s0;
  if (ilen < 1 || ilen > 9 || (int)ilen > k.precision - k.scale || (ilen > 1 && v[s0] == '0')) return false;
  int64_t acc = 0;
  for (size_t i = s0; i < v.size(); ++i) {
    if (i == dot) continue;
    const unsigned d = (unsigned)(v[i] - '0');
    if (d > 9) return false;
    acc = acc * 10 + (int64_t)d;
  }
  if (s0 && acc == 0) return false;
  *out = s0 ? -acc : acc;
  return true;
}

// A hash of a field's bytes for the loader's own dictionaries: eight bytes at a time.
inline uint64_t load_hash(std::string_view v) {
  uint64_t h = 0x9E3779B97F4A7C15ull ^ v.size();
  size_t i = 0;
  for (; i + 8 <= v.size(); i += 8) {
    uint64_t w;
    std::memcpy(&w, v.data() + i, 8);
    h = (h ^ w) * 0xff51afd7ed558ccdull;
    h ^= h >> 32;
  }
  if (i < v.size()) {
    uint64_t w = 0;
    std::memcpy(&w, v.data() + i, v.size() - i);
    h = (h ^ w) * 0xc4ceb9fe1a85ec53ull;
  }
  return h ^ (h >> 29);
}

// A plain load read whole into its columns (LoadColumns), on every core: the file's lines found first, then each
// 16,384-line group (8 zone-map segments, one logged INSERT) read on its own - its values checked and converted,
// its typed columns' zone maps made, its INSERT's text written - with each thread keeping its own dictionaries,
// merged after in the order the rows first hold their values. Only a file that splits plainly: one-byte field
// terminator, '\n' lines, no enclosing, no escape character anywhere in it. False (with *why) wherever the
// row-wise load would do something other than store each value as it reads: that load then runs instead.
bool load_read_columns(const char* data, size_t size, const LoadFormat& fmt, size_t skip_lines,
                       const std::vector<LoadColCheck>& checks, const SqlTable& t, int pk, const std::string& head,
                       LoadColumns* out, std::vector<std::string>* recs, std::string* why) {
  constexpr size_t kSeg = 2048, kGroup = 16384;
  const size_t ncols = checks.size();
  if (fmt.field_term.size() != 1 || fmt.line_term != "\n" || !fmt.line_start.empty() || fmt.enclosed >= 0 ||
      fmt.field_term[0] == '\n' || (fmt.escape >= 0 && (fmt.escape == '\n' || fmt.escape == fmt.field_term[0])) ||
      ncols == 0 || ncols != t.columns.size()) {
    *why = "format";
    return false;
  }
  if (!load_columns_fit(t)) { *why = "table not as created"; return false; }
  const char ft = fmt.field_term[0];
  auto& pool = SqlThreadPool::instance();
  const uint64_t ph0 = hal_now_ns();
  uint64_t ph1 = 0, ph2 = 0;
  const size_t lanes = pool.num_workers() + 1;

  // 1. The lines: where each starts, found a slice of the file per thread; and no escape character anywhere.
  const size_t nslices = std::min<size_t>(lanes * 2, std::max<size_t>(1, size >> 16));
  std::vector<std::vector<size_t>> slice_starts(nslices);
  std::atomic<bool> escaped{false};
  pool.parallel_for(nslices, 1, [&](size_t s0, size_t s1, size_t) {
    for (size_t s = s0; s < s1; ++s) {
      const size_t b = size * s / nslices, e = size * (s + 1) / nslices;
      if (fmt.escape >= 0 && std::memchr(data + b, fmt.escape, e - b)) escaped.store(true);
      auto& v = slice_starts[s];
      v.reserve((e - b) / 32 + 16);
      for (const char* q = data + b; q < data + e;) {
        const char* nl = (const char*)std::memchr(q, '\n', (size_t)(data + e - q));
        if (!nl) break;
        if ((size_t)(nl + 1 - data) < size) v.push_back((size_t)(nl + 1 - data));
        q = nl + 1;
      }
    }
  });
  if (escaped.load()) { *why = "escape character in the file"; return false; }
  std::vector<size_t> starts;
  {
    size_t total = 1;
    for (const auto& v : slice_starts) total += v.size();
    starts.reserve(total + 1);
    starts.push_back(0);
    for (auto& v : slice_starts) { starts.insert(starts.end(), v.begin(), v.end()); std::vector<size_t>().swap(v); }
  }
  if (size == 0) starts.clear();
  if (skip_lines >= starts.size()) { *why = "no lines"; return false; }
  starts.erase(starts.begin(), starts.begin() + (ptrdiff_t)skip_lines);
  const size_t n = starts.size();
  // A line's end: the byte before the next line's start (its '\n'), or for the last line the file's end less a
  // final '\n'.
  auto line_end = [&](size_t r) -> size_t {
    if (r + 1 < n) return starts[r + 1] - 1;
    return size > 0 && data[size - 1] == '\n' && size - 1 >= starts[r] ? size - 1 : size;
  };

  ph1 = hal_now_ns();
  out->n = n;
  out->cols.assign(ncols, {});
  const size_t nsegs = (n + kSeg - 1) / kSeg, ngroups = (n + kGroup - 1) / kGroup;
  std::vector<int64_t> scale_div(ncols, 1);
  std::vector<double> scale_f(ncols, 1.0);
  for (size_t c = 0; c < ncols; ++c) {
    auto& col = out->cols[c];
    col.kind = load_column_kind(t.columns[c]);
    // the kind of check a value of the column has must be one that gives that column's values
    const LoadColCheck::Kind ck = checks[c].kind;
    const bool fits = col.kind == LoadColumns::For ? ck == LoadColCheck::Int
                      : col.kind == LoadColumns::Scaled ? ck == LoadColCheck::Decimal
                      : col.kind == LoadColumns::Double ? ck == LoadColCheck::Real
                      : ck != LoadColCheck::Unsupported;
    if (!fits) { *why = "column " + t.columns[c].name + " is not one read whole"; return false; }
    if (col.kind == LoadColumns::Scaled) {
      col.places = (int)t.columns[c].scale;
      for (int i = 0; i < col.places; ++i) { scale_div[c] *= 10; scale_f[c] *= 10.0; }
    }
    if (col.kind == LoadColumns::Double) col.dbls.resize(n);
    else if (col.kind == LoadColumns::Dict) col.codes.resize(n);
    else col.ints.resize(n);
    col.zones.resize(nsegs);
  }
  if (pk >= 0 && out->cols[(size_t)pk].kind != LoadColumns::For) { *why = "key"; return false; }

  // Per thread: a dictionary per dictionary column (the file's text -> a code of the thread's own, with what the
  // column stores and what its zone maps take of it: build_zone_maps' readings of an entry), the least and greatest
  // value of each For / Scaled column, and which of its codes the segment being read holds.
  struct Local {
    // the file's text -> code: open addressing over (hash, code + 1), the text compared against raw[code]
    std::vector<uint64_t> slot_hash;
    std::vector<uint32_t> slot_code;
    std::vector<std::string_view> raw;
    size_t mask = 0;
    std::vector<std::string> text, low, lit;   // as stored; its collation key; as a quoted literal
    std::vector<size_t> first_row;
    std::vector<uint64_t> h_orig, h_low;
    std::vector<double> d;
    std::vector<int64_t> i;
    std::vector<uint8_t> has_d, has_i;
    std::vector<uint32_t> stamp, seen;           // the segment (+1) each code was last seen in; the segment's codes
  };
  struct Lane {
    std::vector<Local> dicts;
    std::vector<int64_t> lo, hi;
    std::string tmp;
  };
  std::vector<Lane> lane(lanes);
  for (auto& l : lane) { l.dicts.resize(ncols); l.lo.assign(ncols, INT64_MAX); l.hi.assign(ncols, INT64_MIN); }
  std::vector<uint32_t> group_lane(ngroups, 0);
  recs->assign(ngroups, std::string());
  std::atomic<bool> failed{false};
  aios::hal::Mutex why_mu;
  auto fail = [&](std::string w) {
    std::lock_guard<aios::hal::Mutex> lk(why_mu);
    if (!failed.exchange(true)) *why = std::move(w);
  };
  // A new entry of a thread's dictionary: false when it is not a value stored as it reads.
  auto add_entry = [&](Local& d, size_t c, std::string_view v, size_t r, uint32_t* code) -> bool {
    std::string s(v);
    if (!load_field_canon(checks[c], s) || sql_is_null(s)) return false;
    *code = (uint32_t)d.text.size();
    d.raw.push_back(v);
    if ((d.raw.size() + 1) * 2 > d.slot_code.size()) {   // grown (and every code placed again) at half full
      const size_t cap = std::max<size_t>(64, d.slot_code.size() * 2);
      d.slot_code.assign(cap, 0);
      d.slot_hash.assign(cap, 0);
      d.mask = cap - 1;
      for (uint32_t k2 = 0; k2 < (uint32_t)d.raw.size(); ++k2) {
        const uint64_t h = load_hash(d.raw[k2]);
        size_t at = h & d.mask;
        while (d.slot_code[at]) at = (at + 1) & d.mask;
        d.slot_code[at] = k2 + 1;
        d.slot_hash[at] = h;
      }
    } else {
      const uint64_t h = load_hash(v);
      size_t at = h & d.mask;
      while (d.slot_code[at]) at = (at + 1) & d.mask;
      d.slot_code[at] = *code + 1;
      d.slot_hash[at] = h;
    }
    std::string low = collate_key(s), lit;
    append_sql_literal_sv(lit, s);
    d.h_orig.push_back(fnv1a(std::string_view(s)));
    d.h_low.push_back(fnv1a(std::string_view(low)));
    // ensure_dict_doubles / ensure_dict_ints: strtod and strtoll of the entry, none for an empty one
    double dv = 0;
    long long lv = 0;
    bool hd = false, hi = false;
    if (!s.empty()) {
      char* endp = nullptr;
      dv = std::strtod(s.c_str(), &endp);
      hd = endp != s.c_str();
      endp = nullptr;
      lv = std::strtoll(s.c_str(), &endp, 10);
      hi = endp != s.c_str();
    }
    d.d.push_back(hd ? dv : 0.0);
    d.has_d.push_back(hd ? 1 : 0);
    d.i.push_back(hi ? (int64_t)lv : 0);
    d.has_i.push_back(hi ? 1 : 0);
    d.text.push_back(std::move(s));
    d.low.push_back(std::move(low));
    d.lit.push_back(std::move(lit));
    d.first_row.push_back(r);
    d.stamp.push_back(0);
    return true;
  };

  std::atomic<uint64_t> busy_ns{0};
  std::atomic<uint32_t> lanes_used{0};
  pool.parallel_for(ngroups, 1, [&](size_t g0, size_t g1, size_t tid) {
    const uint64_t tb0 = hal_now_ns();
    struct Busy { std::atomic<uint64_t>& b; uint64_t t0; ~Busy() { b.fetch_add(hal_now_ns() - t0); } } busy{busy_ns, tb0};
    Lane& L = lane[tid < lanes ? tid : 0];
    lanes_used.fetch_or(1u << (tid & 31));
    // per segment, per typed column: the text bounds so far
    std::vector<std::string> mn(ncols), mx(ncols);
    std::vector<uint8_t> first(ncols, 1);
    for (size_t g = g0; g < g1 && !failed.load(std::memory_order_relaxed); ++g) {
      group_lane[g] = (uint32_t)tid;
      const size_t r0 = g * kGroup, r1 = std::min(n, r0 + kGroup);
      // The INSERT's text, written straight into its buffer.
      std::string& rec = (*recs)[g];
      rec.resize(head.size() + 16 + (line_end(r1 - 1) - starts[r0]) * 5 / 4 + (r1 - r0) * (ncols * 2 + 4));
      char* w = rec.data();
      char* wend = rec.data() + rec.size();
      auto need = [&](size_t k) {
        if ((size_t)(wend - w) >= k) return;
        const size_t used = (size_t)(w - rec.data());
        rec.resize(std::max(rec.size() * 2, used + k + 4096));
        w = rec.data() + used;
        wend = rec.data() + rec.size();
      };
      auto put = [&](const char* s, size_t k) { need(k); std::memcpy(w, s, k); w += k; };
      put(head.data(), head.size());
      put(" VALUES ", 8);
      for (size_t r = r0; r < r1; ++r) {
        const size_t seg = r / kSeg;
        const bool seg_start = r % kSeg == 0, seg_end = r % kSeg == kSeg - 1 || r + 1 == n;
        if (seg_start) {
          for (size_t c = 0; c < ncols; ++c) {
            SegmentZoneMap& zm = out->cols[c].zones[seg];
            zm.min_int64 = INT64_MAX; zm.max_int64 = INT64_MIN;
            zm.min_double = 1e300; zm.max_double = -1e300;
            zm.null_count = 0; zm.has_stats = true; zm.bloom_filter = 0;
            first[c] = 1;
            L.dicts[c].seen.clear();
          }
        }
        const char* p = data + starts[r];
        const char* const e = data + line_end(r);
        if (r == r0) put("(", 1); else put(",(", 2);
        for (size_t c = 0; c < ncols; ++c) {
          const char* q = p;
          while (q < e && *q != ft) ++q;
          if (c + 1 < ncols) {
            if (q == e) { fail("line " + std::to_string(r + 1 + skip_lines) + " has too few fields"); return; }
          } else if (q != e) {
            fail("line " + std::to_string(r + 1 + skip_lines) + " has too many fields");
            return;
          }
          std::string_view v(p, (size_t)(q - p));
          p = q + 1;
          LoadColumns::Col& col = out->cols[c];
          const LoadColCheck& k = checks[c];
          if (c) put(",", 1);
          if (col.kind == LoadColumns::Dict) {
            Local& d = L.dicts[c];
            uint32_t code = UINT32_MAX;
            if (d.mask) {
              const uint64_t h = load_hash(v);
              for (size_t at = h & d.mask; d.slot_code[at]; at = (at + 1) & d.mask) {
                const uint32_t k2 = d.slot_code[at] - 1;
                if (d.slot_hash[at] == h && d.raw[k2] == v) { code = k2; break; }
              }
            }
            if (code == UINT32_MAX && !add_entry(d, c, v, r, &code)) { fail("a value of " + t.columns[c].name + " is not stored as it reads"); return; }
            col.codes[r] = code;
            if (d.stamp[code] != seg + 1) { d.stamp[code] = (uint32_t)(seg + 1); d.seen.push_back(code); }
            put(d.lit[code].data(), d.lit[code].size());
            continue;
          }
          // a typed column: the value, the text it is stored as, and its readings for the zone map
          std::string_view text = v;
          double dv = 0;
          int64_t iv = 0;
          bool ok = false;
          if (col.kind == LoadColumns::For) {
            int64_t x = 0;
            ok = load_int_plain(k, v, &x);
            if (!ok) {
              L.tmp.assign(v);
              ok = load_field_canon(k, L.tmp) && canonical_int(L.tmp, &x);
              text = L.tmp;
            }
            col.ints[r] = x;
            dv = (double)x;
            iv = x;
          } else if (col.kind == LoadColumns::Scaled) {
            int64_t sv = 0;
            ok = load_decimal_plain(k, v, col.places, &sv);
            if (!ok) {
              L.tmp.assign(v);
              ok = load_field_canon(k, L.tmp) && decimal_fixed_scaled(L.tmp, col.places, &sv);
              text = L.tmp;
            }
            col.ints[r] = sv;
            dv = scaled_decimal_double(sv, scale_f[c]);
            iv = sv / scale_div[c];
          } else {   // Double: as append_encoded's TypedDouble case takes it without printing it back
            int64_t ip = 0;
            ok = !(k.is_unsigned && !v.empty() && v[0] == '-') && normalized_already(SqlDataType::Real, 0, v) &&
                 zone_plain_number(v, &dv, &ip);
            if (!ok) {
              L.tmp.assign(v);
              ok = load_field_canon(k, L.tmp) && normalized_already(SqlDataType::Real, 0, L.tmp) &&
                   zone_plain_number(L.tmp, &dv, &ip);
              text = L.tmp;
            }
            col.dbls[r] = dv;
            iv = (int64_t)dv;
          }
          if (!ok) { fail("a value of " + t.columns[c].name + " is not stored as it reads"); return; }
          if (col.kind != LoadColumns::Double) {
            const int64_t x = col.ints[r];
            if (x < L.lo[c]) L.lo[c] = x;
            if (x > L.hi[c]) L.hi[c] = x;
          }
          // extend_zone_map's reading of the row. A number's text ([-]digits[.digits]) is its own collation key,
          // and PAD SPACE, case-insensitive order on such text is plain byte order (every byte is above the space
          // and none is a letter).
          SegmentZoneMap& zm = col.zones[seg];
          if (dv < zm.min_double) zm.min_double = dv;
          if (dv > zm.max_double) zm.max_double = dv;
          if (iv < zm.min_int64) zm.min_int64 = iv;
          if (iv > zm.max_int64) zm.max_int64 = iv;
          zm.add_bloom(fnv1a(text));
          if (first[c]) { mn[c].assign(text); mx[c].assign(text); first[c] = 0; }
          else if (text.compare(mn[c]) < 0) mn[c].assign(text);
          else if (text.compare(mx[c]) > 0) mx[c].assign(text);
          need(text.size() + 2);
          *w++ = '\'';
          std::memcpy(w, text.data(), text.size());
          w += text.size();
          *w++ = '\'';
        }
        put(")", 1);
        if (seg_end) {
          for (size_t c = 0; c < ncols; ++c) {
            SegmentZoneMap& zm = out->cols[c].zones[seg];
            if (out->cols[c].kind == LoadColumns::Dict) {
              // build_zone_maps' dictionary pass over the segment's distinct codes, in the order the rows first
              // hold them (so a tie under the collation goes the same way)
              const Local& d = L.dicts[c];
              const std::string* lo_s = nullptr;
              const std::string* hi_s = nullptr;
              for (uint32_t k2 : d.seen) {
                if (d.has_d[k2]) { if (d.d[k2] < zm.min_double) zm.min_double = d.d[k2]; if (d.d[k2] > zm.max_double) zm.max_double = d.d[k2]; }
                if (d.has_i[k2]) { if (d.i[k2] < zm.min_int64) zm.min_int64 = d.i[k2]; if (d.i[k2] > zm.max_int64) zm.max_int64 = d.i[k2]; }
                zm.add_bloom(d.h_orig[k2]);
                zm.add_bloom(d.h_low[k2]);
                if (!lo_s || pad_compare_ci(d.low[k2], *lo_s) < 0) lo_s = &d.low[k2];
                if (!hi_s || pad_compare_ci(d.low[k2], *hi_s) > 0) hi_s = &d.low[k2];
              }
              if (lo_s) zone_set_min(zm, *lo_s);
              if (hi_s) zone_set_max(zm, *hi_s);
            } else {
              zone_set_min(zm, mn[c]);
              zone_set_max(zm, mx[c]);
            }
            zm.all_null = false;
          }
        }
      }
      rec.resize((size_t)(w - rec.data()));
    }
  });
  if (failed.load()) return false;
  ph2 = hal_now_ns();

  // 2. Each For / Scaled column's range (a span past 32 bits is a dictionary row-wise), and the key.
  for (size_t c = 0; c < ncols; ++c) {
    auto& col = out->cols[c];
    if (col.kind != LoadColumns::For && col.kind != LoadColumns::Scaled) continue;
    for (const auto& l : lane) { col.lo = std::min(col.lo, l.lo[c]); col.hi = std::max(col.hi, l.hi[c]); }
    if ((uint64_t)col.hi - (uint64_t)col.lo > 0xFFFFFFFFull) { *why = "the values of " + t.columns[c].name + " span past 32 bits"; return false; }
  }
  if (pk >= 0) {
    const auto& k = out->cols[(size_t)pk].ints;
    bool rising = true;
    for (size_t r = 1; r < n && rising; ++r) rising = k[r] > k[r - 1];
    if (!rising) {
      std::vector<int64_t> s(k);
      std::sort(s.begin(), s.end());
      if (std::adjacent_find(s.begin(), s.end()) != s.end()) { *why = "a duplicate key"; return false; }
    }
  }

  // 3. The dictionaries: every thread's values in the order rows first hold them, one entry per text, and each
  // row's code turned into its entry's index.
  for (size_t c = 0; c < ncols; ++c) {
    auto& col = out->cols[c];
    if (col.kind != LoadColumns::Dict) continue;
    struct Ent { size_t row; uint32_t lane, code; };
    std::vector<Ent> all;
    for (size_t l = 0; l < lanes; ++l)
      for (size_t k = 0; k < lane[l].dicts[c].text.size(); ++k) all.push_back({lane[l].dicts[c].first_row[k], (uint32_t)l, (uint32_t)k});
    std::sort(all.begin(), all.end(), [](const Ent& a, const Ent& b) { return a.row < b.row; });
    std::vector<std::vector<uint32_t>> remap(lanes);
    for (size_t l = 0; l < lanes; ++l) remap[l].assign(lane[l].dicts[c].text.size(), 0);
    std::unordered_map<std::string_view, uint32_t> global;
    global.reserve(all.size());
    for (const Ent& en : all) {
      const std::string& s = lane[en.lane].dicts[c].text[en.code];
      auto ins = global.emplace(std::string_view(s), (uint32_t)col.entries.size());
      if (ins.second) col.entries.push_back(s);
      remap[en.lane][en.code] = ins.first->second;
    }
    pool.parallel_for(ngroups, 1, [&](size_t g0, size_t g1, size_t) {
      for (size_t g = g0; g < g1; ++g) {
        const auto& m = remap[group_lane[g]];
        for (size_t r = g * kGroup, r1 = std::min(n, r + kGroup); r < r1; ++r) col.codes[r] = m[col.codes[r]];
      }
    });
  }
  if (vx_trace_on())
    vx_trace("load data: lines " + std::to_string((ph1 - ph0) / 1000) + " us, read " + std::to_string((ph2 - ph1) / 1000) +
             " us (lanes " + std::to_string(__builtin_popcount(lanes_used.load())) + ", busy " + std::to_string(busy_ns.load() / 1000) +
             " us), keys and dictionaries " + std::to_string((hal_now_ns() - ph2) / 1000) + " us");
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
  // An empty table and a load of its own: the file is read whole into the columns on every core
  // (load_read_columns) and the columns taken as they are. Anything it does not read plainly leaves the table as it
  // was and the load goes on row by row below.
  bool bulk_done = false;
  static const bool bulk_off = std::getenv("AIOS_SQL_LOAD_ROWWISE") != nullptr;
  if (canon_ok && own_txn && rows_at_start == 0 && !bulk_off) {
    const uint64_t b0 = hal_now_ns();
    LoadColumns lc;
    std::vector<std::string> recs;
    std::string why;
    if (load_read_columns(data, size, fmt, skip_lines, checks, *t, canon_pk, head, &lc, &recs, &why)) {
      const uint64_t b1 = hal_now_ns();
      std::vector<std::vector<std::string>> none;
      LoadCanonBatch cb;
      cb.rows = &none;
      cb.pk = canon_pk;
      cb.keys = &canon_keys;
      cb.no_undo = true;
      cb.cols = &lc;
      t->ensure_columns();   // a table never written has none yet, and execute_insert takes rows deferred only into columns
      tls_insert_canon = &cb;
      SqlQueryResult r = execute_insert(head);
      tls_insert_canon = nullptr;
      if (cb.cols_taken) {
        bulk_done = true;
        if (cb.undo_skipped) canon_unlogged = true;
        if (!r.success) {
          failure = r.error_message;
        } else {
          records = lc.n;
          affected = r.rows_affected;
          if (!redo_replaying_ && !g_undo_applying) {
            for (auto& rec : recs) {
              if (g_stmt_clock_depth > 0 && !g_cdc_thread)
                rec = std::string(kRedoClockTag) + std::to_string((long long)g_stmt_clock_at) + "*/ " + rec;
              tls_txn.redo.push_back({effective_database(), std::move(rec)});
            }
          }
        }
        if (vx_trace_on())
          vx_trace("load data: read whole into the columns: " + std::to_string(lc.n) + " rows, read " +
                   std::to_string((b1 - b0) / 1000) + " us, taken " + std::to_string((hal_now_ns() - b1) / 1000) + " us");
      } else if (vx_trace_on()) {
        vx_trace("load data: the columns were not taken whole (" + r.error_message + ")");
      }
    } else if (vx_trace_on()) {
      vx_trace("load data: not read whole: " + why);
    }
  }
  if (pipelined && !bulk_done) {
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
  while (!pipelined && !bulk_done && rd.next_line(&fields)) {
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
