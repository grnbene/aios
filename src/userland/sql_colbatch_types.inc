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
// A DecimalScaled cell's text (ColumnStore::get's rule): sv at `places` decimal places, every place kept for a
// DECIMAL(p,s) column (fixed), trailing zeros and an empty fraction dropped for a DOUBLE held scaled.
inline std::string scaled_decimal_cell(int64_t sv, int places, bool fixed) {
  if (places <= 0) return std::to_string((long long)sv);
  const bool neg = sv < 0;
  uint64_t abs_v = neg ? (uint64_t)(-(sv + 1)) + 1 : (uint64_t)sv;
  uint64_t div = 1;
  for (int i = 0; i < places; ++i) div *= 10;
  const uint64_t int_part = abs_v / div, frac_part = abs_v % div;
  char buf[64];
  if (fixed) {
    std::snprintf(buf, sizeof(buf), "%s%llu.%0*llu", neg ? "-" : "", (unsigned long long)int_part, places, (unsigned long long)frac_part);
    return buf;
  }
  if (frac_part == 0) {
    std::snprintf(buf, sizeof(buf), "%s%llu", neg ? "-" : "", (unsigned long long)int_part);
    return buf;
  }
  char frac_buf[32];
  std::snprintf(frac_buf, sizeof(frac_buf), "%0*llu", places, (unsigned long long)frac_part);
  int flen = (int)std::strlen(frac_buf);
  while (flen > 0 && frac_buf[flen - 1] == '0') frac_buf[--flen] = '\0';
  if (flen == 0) std::snprintf(buf, sizeof(buf), "%s%llu", neg ? "-" : "", (unsigned long long)int_part);
  else std::snprintf(buf, sizeof(buf), "%s%llu.%s", neg ? "-" : "", (unsigned long long)int_part, frac_buf);
  return buf;
}

struct ColBatch {
  // Dec: a DecimalScaled column's values as its scaled integers (i) at `scale` places, printed as its cells are
  // (fixed: a DECIMAL(p,s) keeps its places; a DOUBLE held scaled drops trailing zeros) - no text formatted on the
  // way out of a statement and parsed back on the way into a table.
  enum Kind : uint8_t { Text, I64, F64, Codes, Dec };
  struct Col {
    Kind kind = Text;
    std::vector<int64_t> i;          // I64, Dec (scaled)
    int scale = 0;                   // Dec: decimal places
    bool fixed = false;              // Dec: every place printed
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
        case Dec: return nul[r] ? sql_null() : scaled_decimal_cell(i[r], scale, fixed);
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
