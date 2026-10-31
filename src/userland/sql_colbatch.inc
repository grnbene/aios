// A query result as typed columns (ColBatch): what a statement whose rows go straight into a table - a CTE, a
// UNION ALL feeding one - hands over instead of rows of text. The row path printed every cell to a string at the
// select's emit and parsed it back into a column store on the way in: about half of a CTE-heavy statement's time
// (docs/asql-typed-pipeline.md). A batch carries each column as int64 or double lanes, as dictionary codes with
// the entries they use, or as text where nothing better is known, each with its NULLs.
//
// A batch is an alternative form of SqlQueryResult::rows, never a different answer: text(r) of a column is the
// cell the row path would have held, so any consumer may turn a batch back into rows (to_rows) and go on as before.
// Only a consumer that can use one asks (ColOutRequest); the select that starts first under the request takes it
// (take_col_out), so nothing nested - a subquery, a derived table - sees it; a select that cannot fill it (a sort,
// DISTINCT, a LIMIT, an answer from the result cache) leaves it empty and returns rows as before.
// AIOS_SQL_COLUMNAR=0 turns it off.
struct ColBatch {
  enum Kind : uint8_t { Text, I64, F64, Codes };
  struct Col {
    Kind kind = Text;
    std::vector<int64_t> i;          // I64
    std::vector<double> f;           // F64
    std::vector<uint32_t> codes;     // Codes: an index into dict
    std::vector<std::string> dict;   // Codes: the entries the rows use, each once
    std::vector<std::string> s;      // Text: the cell, sql_null() for NULL
    std::vector<uint8_t> nul;        // I64 / F64 / Codes: 1 where the row is NULL
    bool is_null(size_t r) const { return kind == Text ? sql_is_null(s[r]) : nul[r] != 0; }
    // The cell the row path would have held. A double prints as Val::str prints a DOUBLE, less its "-0": no -0 is
    // ever put in an F64 column (from_vp makes such a column text).
    std::string text(size_t r) const {
      switch (kind) {
        case Text: return s[r];
        case I64: return nul[r] ? sql_null() : std::to_string((long long)i[r]);
        case F64: {
          if (nul[r]) return sql_null();
          const double d = f[r];
          if (d == std::floor(d) && std::fabs(d) < 1e15) return std::to_string((long long)d);
          return sql_format_double(d, 0);
        }
        case Codes: return nul[r] ? sql_null() : dict[codes[r]];
      }
      return sql_null();
    }
    // The column as text cells, in place (a consumer that needs the strings).
    void to_text(size_t n) {
      if (kind == Text) return;
      std::vector<std::string> out(n);
      for (size_t r = 0; r < n; ++r) out[r] = text(r);
      s.swap(out);
      kind = Text;
      std::vector<int64_t>().swap(i);
      std::vector<double>().swap(f);
      std::vector<uint32_t>().swap(codes);
      std::vector<std::string>().swap(dict);
      std::vector<uint8_t>().swap(nul);
    }
  };
  bool filled = false;
  size_t rows = 0;
  std::vector<Col> cols;

  // The rows the row path would have returned.
  void to_rows(std::vector<SqlRow>* out) const {
    out->clear();
    out->resize(rows);
    for (size_t r = 0; r < rows; ++r) {
      SqlRow& row = (*out)[r];
      row.values.reserve(cols.size());
      for (const Col& c : cols) row.values.push_back(c.text(r));
    }
  }
};

inline bool colbatch_enabled() {
  static const bool on = [] { const char* v = std::getenv("AIOS_SQL_COLUMNAR"); return !(v && *v == '0'); }();
  return on;
}

// The request a consumer makes, for the statement it runs next on this thread.
thread_local ColBatch* g_col_out = nullptr;

// The top select's request, lent to the single-table engine for the call it makes (try_vx_single_table).
thread_local ColBatch* g_vx_col_out = nullptr;

// Taken by the select that starts first: it is that statement's top select.
inline ColBatch* take_col_out() {
  ColBatch* b = g_col_out;
  g_col_out = nullptr;
  return b;
}

// A consumer's request, for exactly the call it brackets: whatever happens below, the request is gone after.
struct ColOutRequest {
  ColBatch* prev;
  explicit ColOutRequest(ColBatch* b) : prev(g_col_out) { g_col_out = colbatch_enabled() ? b : nullptr; }
  ~ColOutRequest() { g_col_out = prev; }
};

// A stored column's cells for the given rows (row id per output row, kNullRow for the padded side of an outer
// join), in the form that loses nothing: integers of a typed integer column as I64 (its cells print as
// to_string), doubles of a TypedDouble column as F64 (its cells print as F64's text), a dictionary column of its
// own as Codes, anything else as the text cell() gives. `dest` is the result column's type: an integer goes in
// typed only where the result column is INT, a double only where it is a plain DOUBLE.
template <typename RowId>
inline void colbatch_gather(const SqlTable& t, size_t c, SqlDataType dest, int dest_scale, RowId&& row_of, size_t n,
                            ColBatch::Col* out) {
  const ColumnStore& cs = t.col_data[c];
  const bool whole = cs.size() == t.row_count();   // no rows in a delta buffer
  const bool col_temporal = c < t.columns.size() && t.columns[c].temporal;
  const bool ints = whole && !col_temporal && dest == SqlDataType::Int && vx::typed_integer(cs) &&
                    cs.encoding != ColumnEncoding::TemporalDays && cs.encoding != ColumnEncoding::TemporalSeconds;
  const bool dbls = whole && !col_temporal && dest == SqlDataType::Real && dest_scale == 0 && cs.encoding == ColumnEncoding::TypedDouble;
  const bool codes = whole && !ints && !dbls && vx::dict_coded(cs) && !cs.shared_dict && cs.encoding != ColumnEncoding::Constant;
  if (ints || dbls) {
    out->kind = ints ? ColBatch::I64 : ColBatch::F64;
    out->nul.assign(n, 0);
    if (ints) out->i.assign(n, 0); else out->f.assign(n, 0);
    const bool nulls = vx::has_nulls(cs);
    for (size_t r = 0; r < n; ++r) {
      const size_t id = row_of(r);
      if (id == kNullRow || id >= cs.size() || (nulls && cs.is_null(id))) { out->nul[r] = 1; continue; }
      if (ints) out->i[r] = vx::typed_int(cs, id);
      else out->f[r] = cs.double_data[id];
    }
    return;
  }
  if (codes) {
    // the entries the rows use, each copied once: the batch holds no pointer into a table that may go away
    out->kind = ColBatch::Codes;
    out->nul.assign(n, 0);
    out->codes.assign(n, 0);
    std::vector<int32_t> remap(vx::dict_size(cs), -1);
    for (size_t r = 0; r < n; ++r) {
      const size_t id = row_of(r);
      if (id == kNullRow || id >= cs.size() || cs.is_null(id)) { out->nul[r] = 1; continue; }
      const uint32_t k = vx::code_at(cs, id);
      if (k >= remap.size()) { out->nul[r] = 1; continue; }
      if (remap[k] < 0) {
        const std::string& e = vx::dict_entry(cs, k);
        if (sql_is_null(e)) { out->nul[r] = 1; continue; }
        remap[k] = (int32_t)out->dict.size();
        out->dict.push_back(e);
      }
      out->codes[r] = (uint32_t)remap[k];
    }
    return;
  }
  out->kind = ColBatch::Text;
  out->s.resize(n);
  for (size_t r = 0; r < n; ++r) {
    const size_t id = row_of(r);
    out->s[r] = id == kNullRow ? sql_null() : t.cell(id, c);
  }
}

// A vectorized program's result as a batch column: an integer lane as I64 where the result column is INT, a double
// lane as F64 where it is a plain DOUBLE, anything else as the text the row path prints (vp::Column::text).
inline void colbatch_from_vp(vp::Column&& col, SqlDataType dest, int dest_scale, ColBatch::Col* out) {
  const size_t n = col.size();
  if (col.ty == vp::Ty::I64 && !col.uns && dest == SqlDataType::Int) {
    out->kind = ColBatch::I64;
    out->i = std::move(col.i);
    out->nul = std::move(col.n);
    return;
  }
  if (col.ty == vp::Ty::F64 && col.approx && dest == SqlDataType::Real && dest_scale == 0) {
    // a DOUBLE's -0 prints "-0" here and "0" once stored: such a column goes as text
    bool neg_zero = false;
    for (size_t r = 0; r < n && !neg_zero; ++r) if (!col.n[r] && col.f[r] == 0 && std::signbit(col.f[r])) neg_zero = true;
    if (!neg_zero) {
      out->kind = ColBatch::F64;
      out->f = std::move(col.f);
      out->nul = std::move(col.n);
      return;
    }
  }
  out->kind = ColBatch::Text;
  out->s.resize(n);
  for (size_t r = 0; r < n; ++r) out->s[r] = col.text(r);
}

// A batch column's values for fit_decimal_scales_with and cte_columns_from: a dictionary column's entries (the
// set the rows hold), an integer's or a double's text as text(r) prints it, a text column's cells.
template <typename Fn>
void colbatch_each_value(const ColBatch& b, size_t c, Fn&& fn) {
  if (c >= b.cols.size()) return;
  const ColBatch::Col& col = b.cols[c];
  if (col.kind == ColBatch::Codes) {
    for (const std::string& e : col.dict) if (!fn(e)) return;
    return;
  }
  for (size_t r = 0; r < b.rows; ++r) {
    if (col.is_null(r)) continue;
    if (col.kind == ColBatch::Text) { if (!fn(col.s[r])) return; continue; }
    if (!fn(col.text(r))) return;
  }
}

bool canonical_int(const std::string& s, int64_t* out);   // with the column encodings, below

// A text or dictionary column of an INT result whose every value is a canonical integer (a literal 0 the
// interpreter printed, a stored integer column kept as a dictionary): as integer lanes. Its cells print the same
// (to_string of a canonical integer is the integer's text), and the table can hold them typed. Anything else -
// "007", "1.5", "x" - leaves the column as it was.
inline void colbatch_narrow_int(ColBatch::Col& c, size_t n) {
  if (c.kind == ColBatch::Codes) {
    std::vector<int64_t> v(c.dict.size());
    for (size_t k = 0; k < c.dict.size(); ++k) if (!canonical_int(c.dict[k], &v[k])) return;
    c.i.assign(n, 0);
    for (size_t r = 0; r < n; ++r) if (!c.nul[r]) c.i[r] = v[c.codes[r]];
    c.kind = ColBatch::I64;
    std::vector<uint32_t>().swap(c.codes);
    std::vector<std::string>().swap(c.dict);
    return;
  }
  if (c.kind != ColBatch::Text) return;
  std::vector<int64_t> v(n, 0);
  std::vector<uint8_t> nul(n, 0);
  for (size_t r = 0; r < n; ++r) {
    if (sql_is_null(c.s[r])) { nul[r] = 1; continue; }
    if (!canonical_int(c.s[r], &v[r])) return;
  }
  c.kind = ColBatch::I64;
  c.i.swap(v);
  c.nul.swap(nul);
  std::vector<std::string>().swap(c.s);
}

// The batch's rows in another order: row r becomes the row perm[r] was (an ORDER BY's, or a GROUP BY's implicit one,
// worked out on indexes with the row path's own comparator).
inline void colbatch_permute(ColBatch& B, const std::vector<uint32_t>& perm) {
  const size_t n = perm.size();
  for (ColBatch::Col& c : B.cols) {
    switch (c.kind) {
      case ColBatch::Text: { std::vector<std::string> o(n); for (size_t r = 0; r < n; ++r) o[r] = std::move(c.s[perm[r]]); c.s.swap(o); break; }
      case ColBatch::I64: { std::vector<int64_t> o(n); for (size_t r = 0; r < n; ++r) o[r] = c.i[perm[r]]; c.i.swap(o); break; }
      case ColBatch::F64: { std::vector<double> o(n); for (size_t r = 0; r < n; ++r) o[r] = c.f[perm[r]]; c.f.swap(o); break; }
      case ColBatch::Codes: { std::vector<uint32_t> o(n); for (size_t r = 0; r < n; ++r) o[r] = c.codes[perm[r]]; c.codes.swap(o); break; }
    }
    if (c.kind != ColBatch::Text) { std::vector<uint8_t> o(n); for (size_t r = 0; r < n; ++r) o[r] = c.nul[perm[r]]; c.nul.swap(o); }
  }
  B.rows = n;
}
