#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <deque>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <set>
#include <memory>
#include <array>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <functional>
#include <cstring>
#include <cstdlib>
#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#include "aios/core/result.hpp"
#include "aios/hal/sync.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/userland/task_ledger.hpp"
#include "aios/userland/asql_capability.hpp"
#include "aios/userland/sql_wire.hpp"
#include "aios/userland/sql_dialect.hpp"

namespace aios::userland {

class MariaWireSession;
class SqlWireSession;

enum class WriteSyncMode {
  LocalOnly,
  WriteThrough,
  WriteBehind
};

enum class MutationStatus : uint8_t {
  Pending = 0,
  InFlight = 1,
  Acked = 2,
  Failed = 3
};

struct SqlMutation {
  uint64_t seq = 0;
  uint64_t timestamp_ns = 0;
  std::string database;
  std::string sql;
  MutationStatus status = MutationStatus::Pending;
  std::string error_message;
  uint32_t retry_count = 0;
};

struct WriteBackStats {
  uint64_t total_mutations = 0;
  uint64_t pending_mutations = 0;
  uint64_t acked_mutations = 0;
  uint64_t failed_mutations = 0;
  uint64_t last_sync_timestamp_ns = 0;
  std::string last_error;
  double avg_drain_latency_us = 0.0;
  bool drain_worker_running = false;
};

enum class SqlDataType {
  Int,
  Real,       // FLOAT / DOUBLE: approximate, shown as the shortest text that reads back
  Text,
  Decimal     // DECIMAL(p,s) / NUMERIC: exact, stored as its decimal text with exactly s places
};
// DECIMAL behaves as a number everywhere a REAL does (comparison, ordering,
// arithmetic); it differs in what it stores and prints.
inline bool sql_type_numeric(SqlDataType t) { return t == SqlDataType::Int || t == SqlDataType::Real || t == SqlDataType::Decimal; }
// A value in canonical form for a column of this type: DECIMAL to exactly
// `scale` places (rounding half away from zero, as MySQL does), INT without
// leading zeros, REAL as the shortest text that reads back as the same double.
// Anything that does not read as a number, and NULL, comes back unchanged.
std::string sql_normalize_value(SqlDataType t, int scale, const std::string& v);
// A double as MariaDB prints it: sig 0 = shortest round-trip (DOUBLE), else
// that many significant digits (6 for FLOAT).
std::string sql_format_double(double v, int sig);
struct SqlColumn;
void apply_column_type(const std::string& type, const std::vector<int>& args, SqlColumn& col);
std::string sql_mysql_column_type(const SqlColumn& c);
std::string sql_mysql_data_type(const SqlColumn& c);
std::string sql_engine_column_type(const SqlColumn& c);

// SQL NULL is a value, not an empty string. A task with no payload is not a
// task whose payload is "": COUNT(payload) must skip the first and count the
// second, and `WHERE payload = ''` must not find the first. It travels as a
// sentinel no SQL literal can produce (it leads with a NUL byte), so SqlRow
// stays a vector<string> and the on-disk row format is unchanged.
const std::string& sql_null();
uint64_t sql_next_mutation_seq();   // process-unique, increasing
// A value's collation key (utf8_general_ci + PAD SPACE): equal keys are equal values.
std::string sql_collate_key(std::string_view v);
// The NULL marker: one NUL byte (sql_null). Inline: it runs for every cell a
// result set carries, and as a call into another unit it was a tenth of the
// server's time streaming 2.8M rows.
inline bool sql_is_null(const std::string& value) { return value.size() == 1 && value[0] == '\0'; }
// What a grid, a CSV or a printed table shows for a value.
std::string sql_display(const std::string& value);
bool like_match(std::string_view s, std::string_view p);

// SqlColumn::default_value carries two things in one string, so the table files
// need no new field:
//   [kDefaultExprMark] default [kOnUpdateMark on-update-expression]
// A plain default is just its value. A default that changes from row to row
// (CURRENT_TIMESTAMP, NOW(), UUID() ...) is its expression text behind
// kDefaultExprMark, worked out for each row inserted. `ON UPDATE
// CURRENT_TIMESTAMP` follows kOnUpdateMark, and is applied to a row an UPDATE
// changes. Everything that reads a default goes through these four.
constexpr char kDefaultExprMark = '\x1f';
constexpr char kOnUpdateMark = '\x1e';
constexpr char kNoDefaultMark = '\x1d';   // ALTER ... DROP DEFAULT on a nullable column: no default at all (not DEFAULT NULL)
// The default alone, mark and all (empty: none).
inline std::string column_default_part(const std::string& d) {
  const size_t i = d.find(kOnUpdateMark);
  return i == std::string::npos ? d : d.substr(0, i);
}
// The ON UPDATE expression (empty: none).
inline std::string column_on_update_text(const std::string& d) {
  const size_t i = d.find(kOnUpdateMark);
  return i == std::string::npos ? std::string() : d.substr(i + 1);
}
inline bool column_default_is_expr(const std::string& d) { return !d.empty() && d[0] == kDefaultExprMark; }
inline bool column_has_default(const std::string& d) {
  const std::string p = column_default_part(d);
  return !p.empty() && !(p.size() == 1 && p[0] == kNoDefaultMark);
}
// What a client is shown: the expression as written (`current_timestamp()`), or the value.
inline std::string column_default_text(const std::string& d) {
  const std::string part = column_default_part(d);
  return column_default_is_expr(part) ? part.substr(1) : part;
}
inline std::string column_with_on_update(const std::string& default_part, const std::string& on_update) {
  return on_update.empty() ? default_part : default_part + std::string(1, kOnUpdateMark) + on_update;
}

struct SqlColumn {
  std::string name;
  SqlDataType type = SqlDataType::Text;
  uint8_t precision = 0;   // DECIMAL(p,s): total digits
  uint8_t scale = 0;       // DECIMAL(p,s): digits after the point
                           // FLOAT(M,D)/DOUBLE(M,D): D, the decimals it is shown with
  // DATE (1), DATETIME/TIMESTAMP (2), TIME (3): stored as text, but compared
  // as the point in time it names - DATE '2026-06-01' = '2026-06-01 00:00:00'.
  uint8_t temporal = 0;
  // Declared BLOB-family (1) or BINARY/VARBINARY (2): still stored as text, but
  // a client is told its values are bytes (charset 63) as MariaDB tells it.
  uint8_t binary = 0;
  bool is_primary_key = false;
  bool not_null = false;
  bool auto_increment = false;
  // The column's DEFAULT. A plain value is kept as it is. An expression that
  // changes from row to row (CURRENT_TIMESTAMP, NOW(), UUID() ...) is kept as
  // its text behind kDefaultExprMark and worked out for each row inserted -
  // evaluated once at CREATE TABLE it froze at the moment the table was loaded,
  // and every row written since got that old time.
  std::string default_value{};
  // A column of a materialised result (derived table, view) whose expression
  // is a string whatever its values look like: its cells compare as text, not
  // as the numbers they spell.
  bool declared_text = false;
  // The type as it was declared, in MariaDB's COLUMN_TYPE spelling
  // ("varchar(20)", "char(3)", "mediumtext", "bigint(20)", "timestamp").
  // Empty: derived from type/precision/scale/temporal/binary as before (a
  // column created by an older build, an expression column, ENUM/SET).
  std::string declared{};
  // GENERATED ALWAYS AS (expr): the expression's text, "" for a plain column.
  // VIRTUAL or STORED: both are held in the row here, worked out at every
  // write, so a read is a plain read either way.
  std::string generated{};
  bool generated_virtual = false;
  std::string comment{};   // COMMENT 'text' as declared
};

// MariaDB's `Extra` for a column in SHOW COLUMNS / DESCRIBE / information_schema.
inline std::string sql_column_extra(const SqlColumn& c) {
  std::string extra = c.auto_increment ? "auto_increment" : "";
  const std::string on_update = column_on_update_text(c.default_value);
  if (!on_update.empty()) extra += std::string(extra.empty() ? "" : " ") + "on update " + on_update;
  return extra;
}

// One row of the visual table designer's column grid. `original_name` is
// the identity that survives an edit - a designer diffing two whole
// SqlTable snapshots by column NAME cannot tell "renamed id -> user_id"
// from "dropped id, added an unrelated user_id" (data loss either way is
// silent), so the grid carries this instead: empty means a brand new
// column, non-empty and different from `col.name` means a rename, and a
// column present in the table but with no corresponding edit row at all is
// what's actually being dropped.
struct ColumnEdit {
  std::string original_name;
  bool deleted = false;
  SqlColumn col;
};

struct SqlRow {
  std::vector<std::string> values;
};

// Where a result column came from, for the column definitions the wire sends:
// clients read them (dump tools decide quoting by type, drivers convert by it,
// GUIs show the source table). Empty strings and scale -1: not a plain column
// of a table (an expression), as MariaDB reports one.
struct SqlColumnOrigin {
  std::string schema;      // the table's database
  std::string table;       // the table as the query names it (its alias)
  std::string org_table;   // the table itself
  std::string org_name;    // the column itself
  int scale = -1;          // DECIMAL digits after the point, -1 unknown
  uint8_t temporal = 0;    // DATE 1, DATETIME/TIMESTAMP 2, TIME 3
  uint8_t binary = 0;      // BLOB family 1, BINARY/VARBINARY 2
  uint8_t mysql_type = 0;  // declared mysql type (MYSQL_TYPE_TINY, LONG, LONGLONG, etc.)
  uint8_t is_unsigned = 0; // declared UNSIGNED: the wire marks it so (UNSIGNED_FLAG)
  uint8_t not_null = 0;    // NOT NULL or part of the primary key (NOT_NULL_FLAG): a client sizes "NULL" into a column without it
  uint8_t primary = 0;     // in the primary key (PRI_KEY_FLAG)
  uint8_t auto_inc = 0;    // AUTO_INCREMENT (AUTO_INCREMENT_FLAG)
};

// A consumer of a SELECT's rows as they are produced: the wire server sets
// one for a statement whose answer is not going to be cached, and the
// engine's single-table path then hands the rows over as cell views, one
// at a time, instead of building them as strings and copying them once more
// into packets. begin() gets the header (columns, types, origins; no rows).
struct SqlRowSink {
  virtual ~SqlRowSink() = default;
  virtual bool begin(const struct SqlQueryResult& header) = 0;
  virtual bool row(const std::string_view* cells, const uint8_t* null, size_t n) = 0;
  virtual bool end() = 0;
};
void sql_set_row_sink(SqlRowSink* sink);   // this thread's next statement; consumed by the statement
SqlRowSink* sql_row_sink();

struct SqlQueryResult {
  bool success = true;
  std::string error_message;
  // True when the rows already went to the statement's SqlRowSink: rows is
  // empty, rows_affected counts them, and nothing more is to be sent.
  bool streamed = false;
  std::vector<std::string> columns;
  // What each column is, when the engine knows. A result that gets
  // materialised back into a table -- a derived table, a view -- needs this:
  // a COUNT(*) is a number while it is being computed, and putting it in a
  // Text column made `ORDER BY n DESC` sort "99" above "10097". Empty when
  // nothing worked it out, which reads as all-Text.
  std::vector<SqlDataType> column_types;
  // Parallel to `columns` when the engine knows: 1 where the column is a
  // string by construction (a declared string column, CONCAT, a quoted
  // literal), so a table made from this result keeps it text.
  std::vector<uint8_t> column_is_string;
  std::vector<SqlColumnOrigin> column_origins;   // parallel to `columns` when the engine knows; else empty
  std::vector<SqlRow> rows;
  uint64_t rows_affected = 0;
  // UPDATE: the rows its WHERE matched (rows_affected counts those it changed, as MySQL does); -1 for anything
  // else. A client that asked for found rows (CLIENT_FOUND_ROWS) is told this one.
  int64_t rows_matched = -1;
  // The OK packet's info text as MySQL words it ("Rows matched: 1  Changed: 0  Warnings: 0", "Records: 2
  // Duplicates: 0  Warnings: 0"); empty where MySQL sends none. formatted_table stays this engine's own wording.
  std::string ok_info;
  std::string formatted_table;
  // True when the statement produced a grid to look at (SELECT, SHOW,
  // DESCRIBE) rather than changing rows. The studio needs to tell the two
  // apart to know whether to repaint the grid or the row count.
  bool is_result_set = false;
  // Empty means the executing path never classified itself. Leave it empty
  // rather than defaulting to a named route: a join or scan that never set
  // this would otherwise claim the fastest path in the engine it may never
  // have taken.
  std::string execution_route;
  uint32_t zone_pruned = 0;
  uint32_t bloom_pruned = 0;
  std::string primary_table;
};

struct SqlIndex {
  std::string name;
  std::vector<std::string> columns;
  bool is_unique = false;
  std::string type = "BTREE";   // BTREE, FULLTEXT, SPATIAL
  std::vector<uint16_t> sub_parts;   // parallel to columns when any: a prefix length `col(10)`, 0 for none
  std::string comment;
  uint16_t sub_part(size_t i) const { return i < sub_parts.size() ? sub_parts[i] : 0; }
};

struct SqlForeignKey {
  std::string constraint_name;
  std::string column;
  std::string ref_table;
  std::string ref_column;
  std::string on_delete = "NO ACTION";
  std::string on_update = "NO ACTION";
};

// CHECK (expr): refused when expr is false for a row written (NULL passes, as in SQL).
struct SqlCheck {
  std::string name;
  std::string expr;
  bool column_level = false;   // `col INT CHECK (...)`: printed on the column's line
};

struct SqlProbeIndex {
  int column = -1;
  size_t rows_indexed = 0;
  uint64_t last_access_ns = 0;
  uint64_t access_count = 0;
  // Row ids are uint32_t, not size_t: 4 billion rows is past what fits in RAM
  // anyway, and the halving is per row of every indexed column.
  std::unordered_map<double, std::vector<uint32_t>> by_number;
  std::unordered_map<std::string, std::vector<uint32_t>> by_text;
  std::vector<std::vector<uint32_t>> by_code;
  // Indexes a declared string column: a quoted probe matches by text only.
  bool text_column = false;
  // Indexes a declared number column (INT / REAL / DECIMAL, not a date, of a
  // table whose types are known): a quoted probe compares as its number.
  bool numeric_column = false;
  std::vector<std::pair<double, uint32_t>> sorted_by_number;
  // Row ids in text order, not (text, row) pairs. The pair carried a whole
  // std::string per row -- about 72 bytes against this 4 -- to hold a value the
  // column already stores. Ordering comes from comparing the cells themselves,
  // which also works for a column the encoder has since moved off strings.
  std::vector<uint32_t> sorted_by_text;
  std::vector<std::string> sorted_dict_vals;
  std::vector<uint32_t> dict_val_offsets;
  bool is_dict_sorted = false;
  std::vector<uint32_t> no_numeric_prefix;
  bool sorted_built = false;

  void shrink_memory() {
    for (auto& kv : by_number) kv.second.shrink_to_fit();
    for (auto& kv : by_text) kv.second.shrink_to_fit();
    for (auto& v : by_code) v.shrink_to_fit();
    sorted_by_number.shrink_to_fit();
    sorted_by_text.shrink_to_fit();
    sorted_dict_vals.shrink_to_fit();
    dict_val_offsets.shrink_to_fit();
    no_numeric_prefix.shrink_to_fit();
  }

  // Never counted anywhere before: this cache is built lazily per queried
  // column, never evicted, and was invisible to every memory figure the
  // engine reported -- the RAM panel could read 436 MB while this alone held
  // gigabytes. bucket_count() approximates the hash table's own overhead
  // (node + bucket array), not just the values stored in it.
  size_t estimate_memory_bytes() const {
    size_t sz = sizeof(*this);
    for (const auto& kv : by_number) sz += sizeof(kv) + kv.second.capacity() * sizeof(uint32_t);
    sz += by_number.bucket_count() * sizeof(void*);
    for (const auto& kv : by_text) sz += sizeof(kv) + kv.first.capacity() + kv.second.capacity() * sizeof(uint32_t);
    sz += by_text.bucket_count() * sizeof(void*);
    for (const auto& v : by_code) sz += sizeof(v) + v.capacity() * sizeof(uint32_t);
    sz += sorted_by_number.capacity() * sizeof(std::pair<double, uint32_t>);
    sz += sorted_by_text.capacity() * sizeof(uint32_t);
    for (const auto& s : sorted_dict_vals) sz += sizeof(s) + s.capacity();
    sz += dict_val_offsets.capacity() * sizeof(uint32_t);
    sz += no_numeric_prefix.capacity() * sizeof(uint32_t);
    return sz;
  }
};

enum class ColumnEncoding : uint8_t {
  Constant = 0,        // ndv == 1 (0 bits/row beyond null map)
  DictBitPacked = 1,   // ndv < 40% (arbitrary 1..32 bits per code)
  TypedInt32 = 2,      // numeric int32 / dates
  TypedInt64 = 3,      // numeric int64 / epoch / decimal
  TypedDouble = 4,     // numeric double
  RawArena = 5,        // high-cardinality text (offsets + lengths in contiguous arena)
  BlockCompressed = 6, // block compressed fat text with LRU (LZ4 64KB blocks)
  FOR = 7,             // Frame-of-Reference integer encoding (base_int64 + bit-packed deltas)
  RLE = 8,             // Run-Length Encoding (value/code runs + end offsets)
  DecimalScaled = 9,   // Decimal fixed-point scaled with FOR (base_int64 + bit-packed deltas / scale)
  FSST = 10,           // Fast Static Symbol Table text compression (trained 256-symbol dictionary)
  SegmentFOR = 11,     // integers, each 2048-row segment with its own base and bit width (spans up to 64 bits)
  ALP = 12,            // doubles as decimal integers per segment (power of ten + base + bit width), the rest kept whole
  TemporalDays = 13,   // 'YYYY-MM-DD' text as days since 1970-01-01 (base_int64 + bit-packed deltas)
  TemporalSeconds = 14 // 'YYYY-MM-DD HH:MM:SS' text as seconds since 1970-01-01 (base_int64 + bit-packed deltas)
};

struct SegmentZoneMap {
  int64_t min_int64 = INT64_MAX;
  int64_t max_int64 = INT64_MIN;
  double min_double = 1e300;
  double max_double = -1e300;
  // The text bounds, as collation keys. A key longer than 32 bytes is kept
  // as its first characters (str_cut; the upper one with U+FFFF after them):
  // still a bound - the lower at or below every value, the upper at or above
  // - but no longer a value a row holds, so MIN/MAX from zone maps reads such
  // a segment rather than trusting it. Pruning is the same.
  std::string min_str;
  std::string max_str;
  // 64-bit Split-Block Bloom Filter for instant point-lookup pruning:
  uint64_t bloom_filter = 0;
  uint32_t null_count = 0;
  bool all_null = false;
  bool has_stats = false;
  bool str_cut = false;

  void add_bloom(uint64_t h) {
    bloom_filter |= (1ULL << (h & 63ULL)) | (1ULL << ((h >> 6) & 63ULL));
  }
  bool test_bloom(uint64_t h) const {
    uint64_t mask = (1ULL << (h & 63ULL)) | (1ULL << ((h >> 6) & 63ULL));
    return (bloom_filter & mask) == mask;
  }
};

struct FsstTable {
  std::vector<std::string> symbols; // up to 256 symbols (1..8 bytes each)
  // symbol -> code (the code is its index in `symbols`), only what compress() needs: built on first use and dropped
  // once a column is compressed, since decompress() reads `symbols` alone
  mutable std::unordered_map<std::string, uint8_t> symbol_to_code;
  bool trained = false;
  void drop_index() const { std::unordered_map<std::string, uint8_t>().swap(symbol_to_code); }

  void train(const std::vector<std::string>& samples);
  std::vector<uint8_t> compress(const std::string& str) const;
  std::string decompress(const uint8_t* data, size_t len) const;
};

enum class ZoneCmpOp : uint8_t {
  Eq = 0,
  Ne = 1,
  Gt = 2,
  Ge = 3,
  Lt = 4,
  Le = 5
};

struct MorselScheduler {
  std::atomic<size_t> cursor{0};
  size_t total_rows{0};
  size_t morsel_size{2048};

  MorselScheduler(size_t total, size_t morsel_sz = 2048)
    : cursor(0), total_rows(total), morsel_size(morsel_sz) {}

  bool fetch_next(size_t& start, size_t& end) {
    size_t s = cursor.fetch_add(morsel_size, std::memory_order_relaxed);
    if (s >= total_rows) return false;
    start = s;
    end = std::min(s + morsel_size, total_rows);
    return true;
  }
};

// What a statement's worker threads must see exactly as the thread that
// started it does: the session's database (unqualified names inside a
// correlated EXISTS resolved against aios_db on a worker - "table not found:
// purchdata"), the one statement instant NOW() reads, and the enclosing
// SELECTs a nested subquery's columns may refer to.
struct SqlWorkerContext {
  std::vector<const void*> cte_parents;   // the submitting thread's CTE scopes, read-only for a worker
  int rw_depth = 0;              // the statement lock the caller already holds
  std::string session_db;
  int clock_depth = 0;
  long long clock_at = 0;
  std::vector<const void*> outer_scopes;
  // Where a column found in no scope is counted while a subquery is run ahead
  // of its rows (see g_unresolved_sink in sql_database.cpp).
  std::atomic<int>* unresolved_sink = nullptr;
  // Where outer columns read by a subquery's scan are noted (OuterRefLog).
  void* outer_ref_log = nullptr;
  // The connection's @user variables, shared and read-only for a worker: a
  // parallel scan of `WHERE col = @x` must see the same @x, not NULL.
  std::shared_ptr<const void> user_vars;
  // The statement's KILL flag (sql_interrupted), when it may be cut short.
  const void* kill = nullptr;
  uint64_t read_deadline_ns = 0;   // the top-level read's deadline (steady ns), carried to its workers; 0 = none
};

// True once the running statement's session was KILLed (KILL QUERY or
// KILL CONNECTION) and the statement is one that may stop part way (a read):
// long loops and the pool's morsel loops check it and stop handing out work,
// and the statement answers "Query execution was interrupted".
bool sql_interrupted();
// The client statement running on this thread reserves `bytes` of query
// memory before it builds an intermediate that size (SqlDatabase's query
// memory admission), waiting while the budget is taken. False: the statement
// was KILLed while it waited. A no-op on any other thread, and below 1 MB.
bool sql_reserve_query_memory(size_t bytes);
SqlWorkerContext sql_capture_worker_context();
// Installs `in` on this thread; the previous state goes to *saved if given.
void sql_install_worker_context(const SqlWorkerContext& in, SqlWorkerContext* saved);

class SqlThreadPool {
 public:
  static SqlThreadPool& instance();

  size_t num_workers() const;

  // Below this many rows of work a fan-out costs more than it saves: waking
  // the workers is tens of microseconds, scanning a few thousand rows is a
  // few. A 5,000-row lookup table paid ~45 us a query to thread wake-ups.
  static size_t min_parallel_rows();

  // `n` units in chunks of `chunk`, across the pool only when `work_rows`
  // clears min_parallel_rows(); otherwise on this thread, chunk by chunk, so
  // a body that indexes per-chunk state by bstart / chunk sees the same calls.
  template <typename Func>
  void for_chunks(size_t n, size_t chunk, size_t work_rows, bool static_split, Func&& func) {
    if (n == 0) return;
    if (chunk == 0) chunk = 1;
    if (work_rows < min_parallel_rows() || num_workers() == 0 || in_worker_thread_) {
      if (chunk == 1) { func((size_t)0, n, (size_t)0); return; }   // as the pool's own serial fallback
      for (size_t b = 0; b < n && !sql_interrupted(); b += chunk) func(b, std::min(b + chunk, n), (size_t)0);
      return;
    }
    if (static_split) parallel_for_static(n, chunk, std::forward<Func>(func));
    else parallel_for(n, chunk, std::forward<Func>(func));
  }

  template <typename Func>
  void parallel_for(size_t total_rows, size_t morsel_size, Func&& func) {
    if (total_rows == 0) return;
    if (total_rows <= morsel_size || num_workers() == 0 || in_worker_thread_) {
      note_serial_fallback();
      func(0, total_rows, 0);
      return;
    }

    size_t num_morsels = (total_rows + morsel_size - 1) / morsel_size;
    size_t active_workers = std::min(num_workers(), num_morsels > 1 ? num_morsels - 1 : 0);
    if (active_workers == 0) {
      note_serial_fallback();
      func(0, total_rows, 0);
      return;
    }

    MorselScheduler sched(total_rows, morsel_size);
    const SqlWorkerContext ctx = sql_capture_worker_context();
    std::function<void(size_t)> task = [&](size_t tid) {
      SqlWorkerContext saved;
      if (tid != 0) sql_install_worker_context(ctx, &saved);
      size_t bstart = 0, bend = 0;
      while (!sql_interrupted() && sched.fetch_next(bstart, bend)) {
        func(bstart, bend, tid);
      }
      if (tid != 0) sql_install_worker_context(saved, nullptr);
    };

    std::shared_ptr<DispatchJob> job;
    if (!dispatch_parallel(task, active_workers, job)) {
      note_serial_fallback();
      func(0, total_rows, 0);
      return;
    }

    run_own_lane(task);
    wait_parallel(job);
  }

  // The same split every run: morsel i always goes to thread i % workers,
  // rather than to whichever thread asks next. A floating-point sum is not
  // associative, so when the rows a thread accumulates change between runs its
  // partial changes, and the total changes with it -- the same GROUP BY
  // returned 526958187.3100063, then ...052, then ...0137 on one live table.
  // Stealing is worth losing to make an aggregate reproducible.
  template <typename Func>
  void parallel_for_static(size_t total_rows, size_t morsel_size, Func&& func) {
    if (total_rows == 0) return;
    if (total_rows <= morsel_size || num_workers() == 0 || in_worker_thread_) {
      note_serial_fallback();
      func(0, total_rows, 0);
      return;
    }

    size_t num_morsels = (total_rows + morsel_size - 1) / morsel_size;
    size_t active_workers = std::min(num_workers(), num_morsels > 1 ? num_morsels - 1 : 0);
    if (active_workers == 0) {
      note_serial_fallback();
      func(0, total_rows, 0);
      return;
    }

    size_t lanes = active_workers + 1;
    const SqlWorkerContext ctx = sql_capture_worker_context();
    std::function<void(size_t)> task = [&](size_t tid) {
      SqlWorkerContext saved;
      if (tid != 0) sql_install_worker_context(ctx, &saved);
      for (size_t m = tid; m < num_morsels && !sql_interrupted(); m += lanes) {
        size_t bstart = m * morsel_size;
        size_t bend = std::min(bstart + morsel_size, total_rows);
        func(bstart, bend, tid);
      }
      if (tid != 0) sql_install_worker_context(saved, nullptr);
    };

    std::shared_ptr<DispatchJob> job;
    if (!dispatch_parallel(task, active_workers, job)) {
      note_serial_fallback();
      func(0, total_rows, 0);
      return;
    }

    run_own_lane(task);
    wait_parallel(job);
  }

  // The caller works lane 0 itself, and while it does it is a pool thread like
  // the rest: anything nested in its rows runs inline. It used to fan out again
  // and wait for workers that were all parked behind the caller - an UPDATE ...
  // JOIN whose row ran an IN (SELECT ...) held that subquery's fill lock, the
  // subquery's aggregate waited on the pool, every worker waited on the fill
  // lock, and the whole engine stopped with 17 connections queued.
  static void run_own_lane(const std::function<void(size_t)>& task) {
    struct Restore {
      bool was;
      ~Restore() { in_worker_thread_ = was; }
    } restore{in_worker_thread_};
    in_worker_thread_ = true;
    task(0);
  }

  static void set_in_worker_thread(bool val) { in_worker_thread_ = val; }
  static bool is_in_worker_thread() { return in_worker_thread_; }

  // How many parallel regions actually got workers, and how many fell back to
  // running on the calling thread. Asql_parallel_scans used to be read from a
  // counter nothing incremented, so it reported 0 whether the pool was saturated
  // or idle -- which is worse than not reporting it. These are what it reads now.
  uint64_t dispatches() const { return dispatches_.load(std::memory_order_relaxed); }
  uint64_t serial_fallbacks() const { return serial_fallbacks_.load(std::memory_order_relaxed); }
  void note_serial_fallback() { serial_fallbacks_.fetch_add(1, std::memory_order_relaxed); }

 private:
  SqlThreadPool();
  ~SqlThreadPool();

  struct DispatchJob {
    std::function<void(size_t)> task;
    std::atomic<size_t> remaining{0};
    std::atomic<size_t> next_lane{1};
    aios::hal::CondVar done_cv;
    aios::hal::Mutex job_mu;
  };

  bool dispatch_parallel(const std::function<void(size_t)>& task, size_t worker_count, std::shared_ptr<DispatchJob>& out_job);
  void wait_parallel(std::shared_ptr<DispatchJob>& job);
  void worker_loop(size_t worker_id);

  std::vector<aios::hal::Thread> workers_;
  aios::hal::Mutex queue_mu_;
  aios::hal::CondVar queue_cv_;
  struct QueueItem {
    std::shared_ptr<DispatchJob> job;
    size_t lane;
  };
  std::deque<QueueItem> work_queue_;
  bool stop_{false};
  std::atomic<uint64_t> dispatches_{0};
  std::atomic<uint64_t> serial_fallbacks_{0};

  static inline thread_local bool in_worker_thread_{false};
};

// A static symbol table for short strings (the FSST idea; see
// sql_database.cpp): up to 255 symbols of 1-8 bytes, each coded as one byte,
// any other byte as 255 and itself. Strings are coded one by one, so each
// decodes on its own.
struct SymbolCoder {
  std::vector<uint64_t> sym;   // symbol c's bytes, first byte lowest, zero past its length
  std::vector<uint8_t> len;    // its length, 1..8
  std::array<std::vector<uint8_t>, 256> by_first;   // the symbols starting with each byte, longest first
  void train(const std::vector<std::string_view>& sample);
  void index();   // by_first from sym / len
  size_t encode(std::string_view in, uint8_t* out) const;        // out holds 2 * in.size() bytes
  size_t decode(const uint8_t* in, size_t n, char* out) const;   // out holds 8 * n + 8 bytes
  bool well_formed(const uint8_t* in, size_t n) const;           // every code a symbol, no cut-off escape
  size_t memory_bytes() const { return sym.capacity() * 8 + len.capacity() + 256 * 24 + len.size(); }
};

// A dictionary's strings in one byte buffer with 4-byte offsets. A
// std::vector<std::string> spends a 32-byte object on every entry (plus a heap
// block past 15 bytes): 270 MB across st's 8.85M dictionary entries, a third of
// the whole store, holding nothing. view() is free; operator[] hands back a
// per-thread scratch copy, as ColumnStore::get() already does for every
// non-dictionary encoding.
// The entries below coded_count() may be held coded with a SymbolCoder
// (ColumnStore::code_dictionary): view() of one decodes it into the same
// per-thread scratch ring operator[] uses. Entries added later are plain.
class DictArena {
 public:
  DictArena() : offs_(1, 0), gen_(next_gen()) {}
  size_t size() const { return front_coded_ ? entry_count_ : pfirst_ + offs_.size() - 1; }
  bool empty() const { return size() == 0; }
  void clear() {
    bytes_.clear(); offs_.assign(1, 0);
    pfirst_ = 0; cbytes_.clear(); coffs_.clear(); coder_.reset();
    blocked_ = false; per_block_ = 0; blocks_.clear();
    bucket_offs_.clear(); entry_count_ = 0; front_coded_ = false; sorted_ = false;
    gen_ = next_gen();
  }
  // Blocked: entries [0, coded_count()) held as compressed blocks of per_block() entries each (deflate or LZ4; the
  // payload a varint length per entry, then its bytes). For dictionaries of long entries - JSON options, notes,
  // request bodies - 4-7x smaller than the text where symbol codes gave 2.7x; a point read decodes one block, and
  // the block stays decoded for the reads after it. Exclusive with symbol codes and front coding.
  struct Block { uint32_t count = 0; uint32_t raw_size = 0; uint8_t codec = 1; std::vector<uint8_t> bytes; };
  static constexpr uint8_t kBlockLz4 = 0, kBlockDeflate = 1;
  bool blocked() const { return blocked_; }
  // Plain entries from coded_count() on into blocks of `per_block` (fixed by the first call), whole blocks only:
  // what does not fill a block stays plain for a later call.
  void block_upto(size_t upto, uint8_t codec, uint32_t per_block);
  const std::vector<Block>& blocks() const { return blocks_; }
  uint32_t per_block() const { return per_block_; }
  // On load, into an empty arena: the blocks as stored; false when they do not add up.
  bool set_blocked(uint32_t per_block, std::vector<Block> blocks);
  // A copy for the store: an arena only grows between clears (a compaction
  // builds a new one), so a copy that shares this arena's generation and
  // is a prefix of it takes the appended tail only - a checkpoint's
  // snapshot copied 18 MB of sysbench text per table, of which a few KB
  // were new. Coding entries is a new generation too.
  void copy_into(DictArena& dst) const {
    if (!front_coded_ && !dst.front_coded_ && dst.gen_ == gen_ && dst.pfirst_ == pfirst_ && dst.offs_.size() <= offs_.size() && dst.bytes_.size() <= bytes_.size()) {
      dst.bytes_.insert(dst.bytes_.end(), bytes_.begin() + (long)dst.bytes_.size(), bytes_.end());
      dst.offs_.insert(dst.offs_.end(), offs_.begin() + (long)dst.offs_.size(), offs_.end());
      return;
    }
    dst = *this;
  }
  void push_back(std::string_view s);
  // n entries as a table file stores them, each a little-endian u64 length and its bytes (`total` of them in all)
  void push_back_stored(const uint8_t* run, uint64_t n, size_t total);
  // src's entry k, coded as it is there when this arena is still all coded
  // entries of the same coder (a compaction keeps them coded).
  void push_from(const DictArena& src, size_t k);
  void reserve(size_t entries, size_t bytes) { if (!front_coded_) { offs_.reserve(entries + 1); bytes_.reserve(bytes); } }
  void shrink_to_fit() { bytes_.shrink_to_fit(); offs_.shrink_to_fit(); cbytes_.shrink_to_fit(); coffs_.shrink_to_fit(); bucket_offs_.shrink_to_fit(); }
  std::string_view view(size_t k) const {
    if (front_coded_) return view_front_coded(k);
    if (k < pfirst_) return blocked_ ? blocked_view(k) : coded_view(k);
    k -= pfirst_;
    return std::string_view(bytes_.data() + offs_[k], offs_[k + 1] - offs_[k]);
  }
  const std::string& operator[](size_t k) const;
  size_t memory_bytes() const {
    size_t b = bytes_.capacity() + offs_.capacity() * sizeof(uint32_t) + cbytes_.capacity() + coffs_.capacity() * sizeof(uint32_t) +
               bucket_offs_.capacity() * sizeof(uint32_t) + (coder_ ? coder_->memory_bytes() : 0) + blocks_.capacity() * sizeof(Block);
    for (const Block& bl : blocks_) b += bl.bytes.capacity();
    return b;
  }
  size_t byte_size() const {   // the entries as held
    size_t b = bytes_.size() + cbytes_.size();
    for (const Block& bl : blocks_) b += bl.bytes.size();
    return b;
  }
  size_t plain_bytes() const { return bytes_.size(); }
  // Entries [0, coded_count()) are coded (symbol codes, or blocks when blocked()); data() / offsets() describe the
  // plain ones only, so a reader of the raw buffer checks coded_count() first.
  bool coded() const { return pfirst_ > 0; }   // (true for blocks as well: a reader of the raw buffer must decode)
  size_t coded_count() const { return pfirst_; }
  const std::shared_ptr<const SymbolCoder>& coder() const { return coder_; }
  // Entries [coded_count(), upto) coded with c (the arena's coder once it has one).
  void code_upto(const std::shared_ptr<const SymbolCoder>& c, size_t upto);
  // The coded part as stored, and the same put back (on load, into an empty
  // arena, before its plain entries); false when it does not check out.
  const std::vector<uint8_t>& coded_bytes() const { return cbytes_; }
  const std::vector<uint32_t>& coded_offsets() const { return coffs_; }
  bool set_coded(std::shared_ptr<const SymbolCoder> c, std::vector<uint8_t> cbytes, std::vector<uint32_t> coffs);
  // Back to plain entries (a front-coded arena is decoded; entry order is kept, so codes stay valid).
  void to_plain() { un_front_code(); }
  // Every entry, decoded, into `out` (cleared): one contiguous plain copy.
  void decode_into(DictArena& out) const;
  // The raw buffer: entry k is [offsets()[k], offsets()[k + 1]) of data() (when !coded()).
  const char* data() const { return bytes_.data(); }
  const uint32_t* offsets() const { return offs_.data(); }

  bool is_front_coded() const { return front_coded_; }
  bool is_sorted() const { return front_coded_ && sorted_; }
  void front_code(std::vector<uint32_t>* remap_out = nullptr, size_t bucket_size = 16);
  uint32_t find_sorted(std::string_view val) const;

 private:
  std::string_view coded_view(size_t k) const;
  static uint64_t next_gen() { static std::atomic<uint64_t> n{1}; return n.fetch_add(1, std::memory_order_relaxed); }
  std::vector<char> bytes_;       // the plain entries, from pfirst_ on
  std::vector<uint32_t> offs_;
  size_t pfirst_ = 0;             // entries before it are coded
  std::vector<uint8_t> cbytes_;
  std::vector<uint32_t> coffs_;   // pfirst_ + 1 of them when coded
  std::shared_ptr<const SymbolCoder> coder_;
  bool blocked_ = false;           // entries [0, pfirst_) are in blocks_ (then coder_ is null)
  uint32_t per_block_ = 0;
  std::vector<Block> blocks_;
  std::string_view blocked_view(size_t k) const;
  static constexpr size_t kBucketSize = 16;
  static constexpr size_t kBucketShift = 4;
  void un_front_code();
  std::string_view view_front_coded(size_t k) const;
  std::vector<uint32_t> bucket_offs_;   // front-coded: where each bucket of 16 entries starts in bytes_
  size_t entry_count_ = 0;              // front-coded: how many entries
  bool front_coded_ = false;            // (then the entries are not coded with a SymbolCoder: the two are exclusive)
  bool sorted_ = false;                 // front-coded and the entries ascend, so a code is the entry's rank
  uint64_t gen_ = 0;   // copied with the content: a copy of this arena shares it until either clears
};

struct DomainDict {
  DictArena dict;
  std::unordered_map<std::string, uint32_t> str_to_id;
  aios::hal::Mutex mu;
  DomainDict();
  uint32_t intern(const std::string& val);
  const std::string& get(uint32_t id) const;
};

// A number no other ColumnStore has ever had, new for every object, copy and
// assignment. A cache keyed on a column's address alone can be handed a
// different column that was later allocated at the same place.
struct ColumnUid {
  uint64_t v;
  static uint64_t next() { static std::atomic<uint64_t> n{1}; return n.fetch_add(1, std::memory_order_relaxed); }
  ColumnUid() : v(next()) {}
  ColumnUid(const ColumnUid&) : v(next()) {}
  ColumnUid(ColumnUid&&) noexcept : v(next()) {}
  ColumnUid& operator=(const ColumnUid&) { v = next(); return *this; }
  ColumnUid& operator=(ColumnUid&&) noexcept { v = next(); return *this; }
};

struct ColumnStore {
  ColumnUid uid;
  ColumnEncoding encoding = ColumnEncoding::DictBitPacked;
  size_t row_count_ = 0;
  uint8_t bit_width_ = 32;  // 1..32 bits

  // Null validity bitmap (1 bit per row)
  std::vector<uint8_t> null_bitmap;

  // Segment Zone Maps for data skipping (1 entry per 2048 rows)
  std::vector<SegmentZoneMap> zone_maps;

  // Constant encoding:
  std::string constant_val;

  // FOR & DecimalScaled encoding:
  int64_t base_int64 = 0;
  double scale_factor = 1.0;
  double inv_scale_factor = 1.0;

  // RLE encoding:
  uint32_t rle_num_runs = 0;
  uint8_t rle_code_bits = 0;
  std::vector<uint8_t> rle_codes_data;
  std::vector<uint16_t> rle_run_lengths;
  std::vector<uint32_t> rle_escape_runs;
  std::vector<uint32_t> rle_escape_lengths;
  std::vector<uint32_t> rle_seg_base_rows;
  mutable size_t rle_cursor_idx = 0;

  size_t rle_num_runs_count() const { return rle_num_runs; }
  uint32_t rle_get_code(size_t i) const;
  void rle_set_code(size_t i, uint32_t code);
  uint32_t rle_get_length(size_t i) const;
  uint32_t rle_get_start_row(size_t i) const;
  uint32_t rle_get_end_row(size_t i) const;
  size_t rle_find_run(size_t row) const;
  void rle_clear() {
    rle_num_runs = 0; rle_code_bits = 0; rle_cursor_idx = 0;
    rle_codes_data.clear(); rle_run_lengths.clear(); rle_escape_runs.clear(); rle_escape_lengths.clear(); rle_seg_base_rows.clear();
  }

  // FSST symbol compression:
  FsstTable fsst_table;
  std::vector<uint8_t> fsst_data;
  // The row boundaries (row_count_ + 1 of them) of fsst_data, in one of two forms: the compact one (a base for every
  // 16th boundary and each boundary's 16-bit distance from its base: 2.25 bytes a row) whenever every distance fits,
  // else the wide one (fsst_offsets, 4 bytes a row). Exactly one is in use; both empty means no rows.
  std::vector<uint32_t> fsst_offsets;
  std::vector<uint32_t> fsst_off_base;
  std::vector<uint16_t> fsst_off_rel;
  size_t fsst_boundaries() const { return fsst_offsets.empty() ? fsst_off_rel.size() : fsst_offsets.size(); }
  uint32_t fsst_off(size_t i) const { return fsst_offsets.empty() ? fsst_off_base[i >> 4] + fsst_off_rel[i] : fsst_offsets[i]; }
  void fsst_push_boundary(uint32_t off);
  void fsst_set_boundaries(std::vector<uint32_t>&& all);
  std::vector<uint32_t> fsst_all_boundaries() const;
  void fsst_clear_boundaries() { fsst_offsets.clear(); fsst_off_base.clear(); fsst_off_rel.clear(); }

  // DictBitPacked & bit-packed data:
  std::vector<uint8_t> packed_data;  // for bit_width < 32
  std::vector<uint32_t> data32;       // fast path for bit_width == 32
  // Per-segment code packing, a read-only layout of a checkpointed DictBitPacked
  // column: each 2048-row segment holds its codes either as base + idx (frame of
  // reference) or as idx into a local table of at most 256 codes, at the narrower
  // width. packed_data then holds the segments' byte-aligned payloads back to back
  // (data32 is empty) and bit_width_ keeps the column's global width. Every write
  // first returns the column to the global layout (unpack_segments).
  struct SegCodeHdr {
    uint32_t base = 0;       // FOR: the smallest code of the segment
    uint32_t byte_off = 0;   // payload start in packed_data
    uint32_t tab_off = 0;    // table mode: first of 1 << width entries in seg_tab
    uint8_t width = 0;       // bits per row, 0..32
    uint8_t mode = 0;        // 0 = FOR, 1 = local table
    uint16_t reserved = 0;
  };
  bool seg_packed_ = false;
  size_t seg_tried_rows = 0;   // the row count when maintain_storage last tried pack_segments (not saved)
  std::vector<SegCodeHdr> seg_hdr;
  std::vector<uint32_t> seg_tab;
  bool pack_segments();                 // false: the global layout is not larger
  // A FOR integer column re-laid per segment (SegmentFOR) when that is at most 60% of it, as optimize_encoding would
  // choose; decided and done from the codes alone. False: kept as it is.
  bool for_to_segments();
  size_t for_seg_tried_rows = 0;        // the row count when maintain_storage last tried for_to_segments (not saved)
  void unpack_segments();
  uint32_t seg_read_code(size_t row) const;
  void seg_decode(size_t start, size_t end, uint32_t* out) const;
  DictArena dict;
  // Value -> code for writes: an open-addressing table of dictionary codes,
  // probed by comparing against the dictionary's own bytes; built on the
  // first write. The std::unordered_map<std::string, uint32_t> it replaces
  // held a second copy of every string (~60 B an entry) - hundreds of MB once
  // a busy table's million-entry columns had all been written to.
  std::vector<uint32_t> id_index;
  // A byte of each occupied slot's hash, kept beside it (when the sizes
  // match): a probe decodes an entry only when its byte matches - a coded
  // dictionary decoded two or three entries a lookup, half of an INSERT's
  // interning.
  std::vector<uint8_t> id_tag;
  size_t id_index_count = 0;
  std::shared_ptr<DomainDict> shared_dict;
  // Bumped whenever existing rows get new codes (materialize_to_dict_packed):
  // an append that recodes must move the table's layout_seq.
  uint32_t recodes = 0;
  // This column's own version: a new process-unique value whenever the codes
  // or values of rows it already holds change (set, erase, recode,
  // re-encode) - not on append. Per-column caches (the column index, the
  // per-value truth tables) key on it, so an UPDATE of one column no longer
  // throws away every other column's index.
  uint64_t epoch = sql_next_mutation_seq();
  // What a dictionary code's verdict on a predicate is cached under (the per-code truths): the epoch, except that a
  // cell rewritten in place under the same codes (set(), nothing recoded) keeps the one before it - no code changed
  // what it means, so no verdict did. A price UPDATE made the next `price > 0` decide all 300k prices again (12 ms).
  uint64_t truth_epoch = 0, truth_epoch_at = ~0ull;
  uint64_t truth_key() const { return truth_epoch_at == epoch ? truth_epoch : epoch; }
  // The same for what is worked out per code and kept for codes that have rows only (each code's value, the
  // dictionary's collation ranks): kept by an erase alone, since a rewrite in place can give a code its first row.
  uint64_t codes_epoch = 0, codes_epoch_at = ~0ull;
  uint64_t codes_key() const { return codes_epoch_at == epoch ? codes_epoch : epoch; }

  // Sorted dictionary for binary search & range scans:
  std::vector<uint32_t> sorted_dict_indices;
  bool sorted_built = false;

  // Cached numeric representations for dictionary entries:
  // Everything derived from the dictionary's entries by position - the value -> code index, the numeric
  // readings per code, the sorted order. Whatever clears or rewrites the dictionary drops these with it: they
  // are rebuilt on use, and a stale one answers for a different value at the same code.
  void drop_dict_derived();
  mutable std::vector<double> dict_doubles;
  mutable std::vector<uint8_t> dict_doubles_valid;
  mutable std::vector<int64_t> dict_ints;
  mutable std::vector<uint8_t> dict_ints_valid;

  // TypedNative storage:
  std::vector<int32_t> int32_data;
  std::vector<int64_t> int64_data;
  std::vector<double> double_data;

  // RawArena storage (contiguous bytes):
  std::vector<char> text_arena;
  std::vector<uint32_t> arena_offsets; // CSR layout (size: row_count_ + 1)

  // BlockCompressed storage: 512-row blocks, each compressed on its own.
  // The codec is per block: LZ4 (decodes at memory speed) or raw deflate
  // (about four times smaller on the text of a log table, several times
  // the CPU to read back). A table chooses with TEXT_CODEC = LZ4 | DEFLATE
  // (SqlTable::text_codec); the column keeps the choice in block_codec_pref
  // and applies it to every block it writes from then on.
  enum BlockCodec : uint8_t { kBlockLz4 = 0, kBlockDeflate = 1 };
  struct CompressedBlock {
    size_t start_row = 0;
    size_t num_rows = 0;
    uint32_t raw_size = 0;          // bytes once decompressed
    uint8_t codec = kBlockLz4;      // BlockCodec (AQT7; every older file's block is LZ4)
    // codec( one varint length per row, then the rows' bytes ): no offset array to keep beside it
    std::vector<uint8_t> compressed_bytes;
    // A block written before AQT6: its payload is the rows' bytes only and these are its CSR offsets (num_rows + 1).
    std::vector<uint32_t> offsets;
  };
  std::vector<CompressedBlock> compressed_blocks;
  // Rows appended since the last block was sealed (an open tail): their
  // bytes in text_arena, CSR offsets in arena_offsets (tail rows + 1). The
  // tail becomes a block at kBlockRows rows. Before it, an append to a block
  // column decoded the whole column into a dictionary and the next
  // checkpoint re-encoded it whole: a write-only log table of 2 GB paid
  // both for every batch of inserts.
  static constexpr size_t kBlockRows = 512;
  size_t block_rows_sealed() const { return compressed_blocks.empty() ? 0 : compressed_blocks.back().start_row + compressed_blocks.back().num_rows; }
  uint8_t block_codec_pref = kBlockLz4;   // the codec for blocks written from now on
  // Blocks decoded (cache misses) since the column was loaded or encoded:
  // SHOW ASQL STORAGE shows it, so an operator can see whether a table's
  // blocks are read before putting it on DEFLATE.
  mutable uint64_t block_decodes = 0;
  mutable std::unordered_map<size_t, std::vector<std::string>> block_lru_cache;
  // The open tail sealed into a block (when it holds at least kBlockRows rows, or `force` and any at all).
  void seal_block_tail(bool force);
  // Every block transcoded to block_codec_pref (decode, encode again); blocks already in it are left alone.
  // Returns how many were transcoded.
  size_t recode_blocks();
  // The blocks' codecs as one word: "lz4", "deflate", "mixed" ("" for a column without blocks).
  const char* block_codec_name() const;

  // SegmentFOR and ALP: per 2048-row segment the base its offsets count
  // from and their bit width; seg_byte_at[s] is where segment s starts in
  // packed_data (one more entry than segments: the end). Each segment has
  // room for 2048 offsets at its width, so the last one takes appends and
  // any one can be rewritten in place while its width does not grow.
  std::vector<int64_t> seg_base;
  std::vector<uint8_t> seg_bits;
  std::vector<uint64_t> seg_byte_at;
  // ALP: per segment the power of ten its values were scaled by (low 7
  // bits; 0x80: the segment has exceptions, whose offset is all ones), and
  // the exceptions - values no decimal of that many places gives back
  // exactly - by row, ascending.
  std::vector<uint8_t> alp_exp;
  std::vector<uint32_t> alp_exc_rows;
  std::vector<double> alp_exc_vals;
  uint64_t seg_offset(size_t row) const;
  int64_t segfor_value(size_t row) const { return (int64_t)((uint64_t)seg_base[row >> 11] + seg_offset(row)); }
  double alp_value(size_t row) const;
  // Values of rows [start, end) (NULL rows read as whatever their slot holds).
  void seg_ints(size_t start, size_t end, int64_t* out) const;
  void alp_doubles(size_t start, size_t end, double* out) const;
  // Segment s laid out again from its values (`n` rows from s * 2048): false,
  // with nothing changed, when it no longer fits where it is (not the last).
  bool segfor_put(size_t s, const int64_t* vals, size_t n);
  bool alp_put(size_t s, const double* vals, size_t n);
  void seg_clear();

  // Reconstructed string buffer for typed/arena/block get()
  mutable std::string scratch_val;

  ColumnStore();
  size_t size() const { return row_count_; }
  bool empty() const { return row_count_ == 0; }
  bool is_null(size_t row) const;
  // Drops the bitmap's bytes past its last NULL (all of it for a column with none): 1 bit a row otherwise.
  void trim_nulls();
  void set_null(size_t row, bool null_val);

  uint32_t intern(const std::string& val);
  const std::string& get(size_t row) const;
  // The cell's text without a copy where the encoding holds it as bytes
  // (dictionary, arena); otherwise get()'s scratch string. Valid until the
  // next call on this thread or the next write to the column.
  std::string_view get_view(size_t row) const;
  bool get_double(size_t row, double* out_val) const;
  bool get_int64(size_t row, int64_t* out_val) const;
  void ensure_dict_doubles() const;
  void ensure_dict_ints() const;
  void set(size_t row, const std::string& val);
  void push_back(const std::string& val);
  // A write in the column's current encoding - no decompression. False when
  // the encoding cannot take this value in place (then push_back/set, which
  // decompress, are the fallback and reencode_pending is raised).
  bool append_encoded(const std::string& val);
  bool set_encoded(size_t row, const std::string& val);
  // Zone maps for the segments covering rows [from, to], after a write there.
  void refresh_zone_maps(size_t from, size_t to);
  // Set while ColumnStore::set rewrites one cell: the setters below it leave
  // the zone map to set(), which updates it from the old and new value.
  bool defer_zone_refresh_ = false;
  // Segments whose stats were recomputed in this process: stats loaded from
  // the store came from another builder and are not trusted to be exact, so
  // the first incremental update of a segment starts from a rescan.
  mutable std::vector<uint8_t> zone_exact_;
  // Segments whose min/max are wider than the values they hold: a one-cell
  // UPDATE that replaced a value sitting ON a bound keeps the bound (still a
  // true superset - pruning stays right) instead of rescanning the segment's
  // 2048 rows, which a low-cardinality column (a status, a flag) paid on
  // nearly every write. MIN/MAX from zone maps reads these segments in full;
  // the store writes them without stats; a refresh makes them exact again.
  mutable std::vector<uint8_t> zone_loose_;
  bool zone_loose(size_t seg) const { return seg < zone_loose_.size() && zone_loose_[seg]; }
  // Set when a write had to decompress an encoded column; the table re-encodes
  // it at its next maintenance pass instead of on the write path.
  bool reencode_pending = false;
  // Whether any row is NULL: -1 not known (recount), 0 none, 1 some. Set to 1
  // by any write of a NULL, reset by structural changes; only a stale "some"
  // is possible, which costs a null test and never a wrong answer.
  mutable int8_t nulls_known = -1;
  // A dictionary column of a declared INT type is tried once, at a
  // checkpoint, for a typed encoding (optimize_encoding): rows that arrive
  // by INSERT are dictionary-coded, and only an import ever re-encoded them,
  // so a table built by INSERTs compared its integer key as text, row by row.
  bool typed_tried = false;
  // A DOUBLE column's dictionary fits the scaled encoding exactly; with dec_scale (a DECIMAL(p,s)'s s) every entry
  // is the canonical text of its scaled integer, all s places printed.
  bool dict_scales_exactly(int dec_scale = 0) const;
  void dict_to_scaled(int dec_scale = 0);   // ... and is moved to it
  // The fields a store file holds (put_column), copied; the caches and the
  // write-side index are not.
  ColumnStore save_copy() const;
  void save_copy_into(ColumnStore& c) const;   // the same, into a copy whose buffers are already warm
  // Times a write (set / append the encoding could not take) recoded this
  // column to the dictionary. Once is an accident; twice is a column that is
  // written to, and re-encoding it at every checkpoint only to recode it at
  // the next write (a 100k-row text column each way) is left off: it stays
  // dictionary-coded.
  uint32_t write_recodes = 0;
  void clear();
  void erase(size_t row);
  // Remove rows (ascending, distinct) in one pass, keeping the column's
  // encoding and codes: a replicated DELETE on 12M rows used to turn every
  // column into strings and shift the rest one row at a time.
  void erase_rows(const std::vector<size_t>& rows);
  bool erase_one_row(size_t row);   // the one-row fast path of erase_rows; false: not taken
  void reserve(size_t sz);
  void resize(size_t sz, const std::string& val);
  void materialize_to_dict_packed();
  // The column as a dictionary at the width its entries need (materialize_to_dict_packed leaves it at 32 bits a
  // code): what a write the current encoding cannot take in place turns the column into.
  void to_dict_narrow();
  void widen_for(uint32_t id);   // the codes a bit wider when `id` does not fit (never 32 at once)

  // Bit-packing & integer code helpers
  uint32_t read_code(size_t row) const;
  void write_code(size_t row, uint32_t code);
  // Hands back the capacity the column's vectors grew past their size (appends over-allocate).
  void release_slack();
  // dec_scale: a DECIMAL(p,s) column's s, so DecimalScaled may hold it with
  // its fixed s places ("12.50"); 0 for any other column.
  // `temporal` is the declared column's SqlColumnDecl::temporal: a DATE / DATETIME / TIMESTAMP
  // column whose every value is canonical text may take TemporalDays / TemporalSeconds.
  void optimize_encoding(SqlDataType type = SqlDataType::Text, int dec_scale = 0, uint8_t temporal = 0);
  bool dict_temporal_fits() const;    // a dictionary column whose every entry is canonical temporal text, worth packing
  void dict_to_temporal();            // ... and is moved to TemporalDays / TemporalSeconds
  // A DecimalScaled column holding a DECIMAL(p,s): get() prints all s places.
  // Persisted as the sign of inv_scale_factor (nothing else reads it), so
  // files written before this load as the stripping form they were.
  bool decimal_fixed() const { return inv_scale_factor < 0.0; }
  void apply_permutation(const std::vector<uint32_t>& perm);
  void build_sorted_dict();
  void build_zone_maps();
  bool compact_dictionary();   // drops the dictionary entries no row uses (see maintain_storage)
  // Codes up to `budget` more bytes of this column's dictionary with a symbol
  // table (trained on its first call): true when it did any. Large
  // dictionaries of long entries only (1 MB, 32 bytes an entry on average),
  // and only where the table saves a fifth or more.
  bool code_dictionary(size_t budget);
  size_t dict_code_tried_at = 0;   // the dictionary's size when a table was found not to pay
  bool segment_can_match_num(size_t seg_idx, ZoneCmpOp op, double num_val) const;
  bool segment_can_match_str(size_t seg_idx, ZoneCmpOp op, const std::string& str_val) const;
  void prefetch_morsel(size_t start_row, size_t count) const;
  void warm_up() const;
  size_t estimate_memory_bytes() const;
};

uint64_t sql_next_mutation_seq();

// Readers share, writers are alone: every client session has its own thread and
// replication applies on another, and a replicated DELETE shifting a column
// while a report scanned it was a live hazard. Writer-preferring, so a stream
// of reads cannot starve replication. Built on mutex + condition_variable,
// which every target this engine builds for has.
// Readers take and release the lock with one atomic step each while no writer
// is about or waiting; only then do they go through the mutex. With eight
// sessions reading, the mutex taken and released around every statement was
// a futex handoff per statement.

// The primary-key map of a row index: open addressing in one array, the key
// held as a 64-bit integer while every key is one (an INT key, which is most
// of them; the text form row_key makes is parsed back), as text otherwise.
// A node-per-key hash map allocated and freed a node for every INSERT and
// DELETE and walked three cache lines per lookup. The call surface is the
// part of unordered_map the row index used: find / end / erase (by key or
// iterator) / emplace / operator[] / size / empty / reserve / iteration with
// a mutable `second`.
class PkMap {
 public:
  struct Slot {
    uint64_t key = 0;
    uint32_t second = 0;   // virtual row id: 32-bit like every row id the executor passes around; 16 B a slot, not 24
    uint8_t state = 0;     // 0 empty, 1 used, 2 deleted (a tombstone keeps the probe chain)
  };
  struct iterator {
    PkMap* m = nullptr;
    size_t i = 0;
    // Runs mode (below): a hit is its own entry (i == 1; end() is i == 0). Read-only in effect - what writes
    // through an entry (operator[], a walk with begin()) turns the map back into slots first.
    mutable Slot run_hit{};
    bool run = false;
    Slot* operator->() const { return run ? &run_hit : &m->slots_[i]; }
    Slot& operator*() const { return run ? run_hit : m->slots_[i]; }
    iterator& operator++() { ++i; skip(); return *this; }
    bool operator==(const iterator& o) const { return i == o.i; }
    bool operator!=(const iterator& o) const { return i != o.i; }
    void skip() { while (i < m->slots_.size() && m->slots_[i].state != 1) ++i; }
  };
  iterator begin() { if (runs_) to_slots(); if (sorted_) sorted_to_slots(); iterator it{this, 0}; it.skip(); return it; }
  iterator end() { return iterator{this, (runs_ || sorted_) ? 0 : slots_.size()}; }
  size_t size() const { return count_; }
  bool empty() const { return count_ == 0; }
  // `hash` is for the tests: a degenerate one makes every key collide, and the map must still be exact.
  explicit PkMap(uint64_t (*hash)(const std::string&) = nullptr) : hash_fn_(hash), runs_(runs_enabled()) {}
  void clear() {
    slots_.clear(); koff_.clear(); arena_.clear(); dead_ = 0; count_ = used_ = 0; text_ = false;
    runs_ = runs_enabled(); runs_v_.clear(); gone_.clear(); late_.clear(); shape_ = 0; pair_swap_ = false;
    sorted_ = false; skeys_.clear(); soff_.clear(); sval_.clear(); sidx_.clear(); sgone_.clear(); slate_.clear();
  }
  void reserve(size_t n) { if (runs_ || sorted_) return; if (n * 3 + 4 > slots_.size() * 2) rehash(n); }
  iterator find(const std::string& key) {
    if (runs_) return run_find(key);
    if (sorted_) return sorted_find(key);
    if (slots_.empty()) return end();
    uint64_t ik = 0;
    if (!text_ && !int_key(key, &ik)) return end();   // not this map's integer shape: not one of its keys
    if (text_) ik = hash_text(key);
    const size_t i = locate(key, ik, false);
    return i == npos ? end() : iterator{this, i};
  }
  void erase(const std::string& key) {
    auto it = find(key);
    if (it != end()) erase(it);
  }
  iterator erase(iterator it) {
    if (runs_) {
      if (it.i == 0) return it;
      run_erase(it.run_hit.key);
      maybe_fold();
      return end();
    }
    if (sorted_) {
      if (it.i == 0) return it;
      sorted_erase_hit(it.run_hit.key);
      maybe_sfold();
      return end();
    }
    slots_[it.i].state = 2;
    if (text_) {
      dead_ += entry_bytes(it.i);
      if (dead_ > (1u << 20) && dead_ * 2 > arena_.size()) compact();
    }
    --count_;
    ++it;
    return it;
  }
  // The reference is into slots_, or in runs mode into a run of the overlay holding this key alone - never into an
  // iterator's copy. It is good until the next change to the map (its callers write through it at once).
  uint32_t& operator[](const std::string& key) {
    if (runs_) {
      uint64_t ik = 0;
      if (int_key(key, &ik, true)) {
        uint32_t v = 0;
        auto have = run_find(key);
        if (have != end()) { v = have->second; run_erase(ik); }
        return late_[late_insert(ik, v, false)].val;
      }
      to_slots();
    }
    if (sorted_) {
      // A live key's id is written where it is; a new key goes to the overlay (no fold here: the reference
      // handed out must outlive this call, and a fold would move it).
      uint32_t* ref = sorted_ref(key);
      if (ref) return *ref;
      ++count_;
      auto at = std::lower_bound(slate_.begin(), slate_.end(), key, [](const SlateEntry& e, const std::string& k) { return e.key < k; });
      return slate_.insert(at, SlateEntry{key, 0})->val;
    }
    const size_t i = emplace(key, 0).first.i;
    return slots_[i].second;
  }
  std::pair<iterator, bool> emplace(const std::string& key, size_t v) {
    uint64_t ik = 0;
    const bool is_int = int_key(key, &ik, true);
    if (runs_) {
      if (is_int) {
        if (run_append(ik, v)) return {run_iter(ik, (uint32_t)v), true};
        auto have = run_find(key);
        if (have != end()) return {have, false};
        if (v <= UINT32_MAX) {   // a key before the last (a line added to an old order): into the overlay
          late_insert(ik, (uint32_t)v, true);
          maybe_fold();
          return {run_iter(ik, (uint32_t)v), true};
        }
      }
      to_slots();   // a text key: slots from here on
    }
    if (sorted_) {
      auto have = sorted_find(key);
      if (have != end()) return {have, false};
      if (v > UINT32_MAX) sorted_to_slots();   // an id past 32 bits: as slots, below
      else {
        ++count_;
        auto at = std::lower_bound(slate_.begin(), slate_.end(), key, [](const SlateEntry& e, const std::string& k) { return e.key < k; });
        const size_t s = (size_t)(slate_.insert(at, SlateEntry{key, (uint32_t)v}) - slate_.begin());
        if (slate_.size() + sgone_.size() > 4096 + count_ / 64) { sfold(); return {sorted_find(key), true}; }
        return {run_iter(kSlateBit | (uint64_t)s, (uint32_t)v), true};
      }
    }
    if (!text_ && !is_int) to_text();
    if (text_) ik = hash_text(key);
    if ((used_ + 1) * 10 > slots_.size() * 7 || slots_.empty()) rehash(count_ + 1);
    const size_t i = locate(key, ik, true);
    if (slots_[i].state == 1) return {iterator{this, i}, false};
    if (slots_[i].state == 0) ++used_;
    slots_[i].state = 1;
    slots_[i].key = ik;
    slots_[i].second = (uint32_t)v;
    if (text_) koff_[i] = put_text(key);
    ++count_;
    return {iterator{this, i}, true};
  }
  size_t memory_bytes() const {
    size_t slate = slate_.capacity() * sizeof(SlateEntry);
    for (const SlateEntry& e : slate_) if (e.key.capacity() > sizeof(std::string)) slate += e.key.capacity();
    return slots_.capacity() * sizeof(Slot) + koff_.capacity() * sizeof(uint32_t) + arena_.capacity() +
           runs_v_.capacity() * sizeof(Run) + gone_.capacity() * sizeof(uint64_t) + late_.capacity() * sizeof(Run) +
           skeys_.capacity() + (soff_.capacity() + sval_.capacity() + sgone_.capacity()) * sizeof(uint32_t) +
           sidx_.capacity() * sizeof(uint64_t) + slate;
  }
  // Every entry's row id through `fn` (a monotonic renumbering: the row index folding its gaps). In runs mode the
  // runs are rebuilt from the new ids; slots are rewritten in place.
  template <class F> void remap_values(F fn) {
    if (runs_) fold();   // the overlay into the runs first: one list to renumber
    if (sorted_) {
      sfold();
      for (uint32_t& v : sval_) v = (uint32_t)fn(v);
      return;
    }
    if (!runs_) {
      for (Slot& sl : slots_) if (sl.state == 1) sl.second = (uint32_t)fn(sl.second);
      return;
    }
    // The new runs are built aside; if any entry will not go into runs (an id past 32 bits), the map turns into
    // slots first and is renumbered there - nothing is dropped.
    std::vector<Run> fresh;
    fresh.reserve(runs_v_.size() + gone_.size());
    size_t g = 0, n = 0;
    bool ok = true;
    for (const Run& r : runs_v_) {
      for (uint32_t j = 0; j < r.n && ok; ++j) {
        const uint64_t k = r.key + j;
        while (g < gone_.size() && int_less(gone_[g], k)) ++g;
        if (g < gone_.size() && gone_[g] == k) continue;
        ok = append_run(fresh, k, fn((size_t)r.val + j));
        ++n;
      }
      if (!ok) break;
    }
    if (!ok) {
      to_slots();
      for (Slot& sl : slots_) if (sl.state == 1) sl.second = (uint32_t)fn(sl.second);
      return;
    }
    runs_v_.swap(fresh);
    gone_.clear();
    count_ = n;
  }
  // Every entry's id through `fn`, and every entry `fn` answers kDrop for goes (the row index folding its gaps
  // after deletes: the erased rows' entries out, the ids above them down). Runs stay runs - a walk with begin() did
  // this before and turned every map with a deleted row into slots at the next checkpoint. Returns how many went.
  static constexpr size_t kDrop = (size_t)-1;
  template <class F> size_t remap_or_erase(F fn) {
    if (runs_) fold();
    size_t gone = 0;
    if (sorted_) {
      sfold();   // the overlay in, the erased out: one sorted list to renumber
      for (uint32_t p = 0; p < sval_.size(); ++p) {
        const size_t nv = fn(sval_[p]);
        if (nv == kDrop) { sgone_.push_back(p); ++gone; --count_; }   // in position order: sgone_ stays sorted
        else sval_[p] = (uint32_t)nv;
      }
      maybe_sfold();
      return gone;
    }
    if (runs_) {
      std::vector<Run> fresh;
      fresh.reserve(runs_v_.size());
      size_t n = 0;
      bool ok = true;
      for (const Run& r : runs_v_) {
        for (uint32_t j = 0; j < r.n && ok; ++j) {
          const size_t nv = fn((size_t)r.val + j);
          if (nv == kDrop) { ++gone; continue; }
          ok = append_run(fresh, r.key + j, nv);
          ++n;
        }
        if (!ok) break;
      }
      if (ok) {
        fresh.shrink_to_fit();
        runs_v_.swap(fresh);
        count_ = n;
        return gone;
      }
      to_slots();   // an id past 32 bits: as slots, below (nothing was dropped yet)
      gone = 0;
    }
    for (size_t i = 0; i < slots_.size(); ++i) {
      Slot& sl = slots_[i];
      if (sl.state != 1) continue;
      const size_t nv = fn(sl.second);
      if (nv == kDrop) {
        sl.state = 2;
        if (text_) dead_ += entry_bytes(i);
        --count_;
        ++gone;
      } else {
        sl.second = (uint32_t)nv;
      }
    }
    if (text_ && dead_ > (1u << 20) && dead_ * 2 > arena_.size()) compact();
    return gone;
  }
  bool runs_mode() const { return runs_; }
  // How many maps have turned from runs back into slots (SHOW STATUS Asql_pk_runs_to_slots).
  static std::atomic<uint64_t>& runs_to_slots_count() { static std::atomic<uint64_t> n{0}; return n; }
  // Text keys as a sorted arena (below): what a checkpoint asks of a map of text keys held as slots. True when the
  // map changed form; a small map (under kSortedMin keys) or one not of text keys is left as it is.
  bool sort_text_keys() {
    if (sorted_ || runs_ || !text_ || count_ < kSortedMin || !sorted_enabled()) return false;
    // Each live key's bytes located once (the varint decoded), then sorted by memcmp alone: 3M keys in ~1 s.
    struct Ref { const char* p; uint32_t len; uint32_t val; };
    std::vector<Ref> order;
    order.reserve(count_);
    for (size_t i = 0; i < slots_.size(); ++i) {
      if (slots_[i].state != 1) continue;
      size_t len;
      const char* p = text_of(i, &len);
      order.push_back(Ref{p, (uint32_t)len, slots_[i].second});
    }
    std::sort(order.begin(), order.end(), [](const Ref& a, const Ref& b) {
      const int c = std::memcmp(a.p, b.p, std::min(a.len, b.len));
      return c < 0 || (c == 0 && a.len < b.len);
    });
    std::vector<char> keys;
    keys.reserve(arena_.size() - dead_);
    std::vector<uint32_t> off, val;
    off.reserve(order.size());
    val.reserve(order.size());
    for (const Ref& r : order) {
      if (keys.size() + r.len + 5 > UINT32_MAX) return false;   // offsets are 32-bit: a map this big stays slots
      off.push_back((uint32_t)keys.size());
      put_skey(keys, r.p, r.len);
      val.push_back(r.val);
    }
    std::vector<Slot>().swap(slots_);
    std::vector<uint32_t>().swap(koff_);
    std::vector<char>().swap(arena_);
    dead_ = 0;
    used_ = count_;
    keys.shrink_to_fit();
    skeys_.swap(keys);
    soff_.swap(off);
    sval_.swap(val);
    build_sidx();
    sorted_ = true;
    sorted_count().fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  bool sorted_mode() const { return sorted_; }
  // Text keys held as slots: what sort_text_keys would take (an integer map that fell back to slots is not one).
  bool text_slots() const { return text_ && !sorted_ && !runs_; }
  // How many maps have been sorted, and how many sorted maps turned back into slots (SHOW STATUS).
  static std::atomic<uint64_t>& sorted_count() { static std::atomic<uint64_t> n{0}; return n; }
  static std::atomic<uint64_t>& sorted_to_slots_count() { static std::atomic<uint64_t> n{0}; return n; }
  static constexpr size_t kSortedMin = 4096;

 private:
  static constexpr size_t npos = (size_t)-1;
  // A key that is not an integer is held as its text, exactly, in one packed arena - a varint length and the
  // bytes, 4-byte aligned, `koff_[slot]` units in - not as a std::string a slot (32 B, and a heap block past 15
  // characters), which made a composite-key row ~130 bytes of index. A lookup compares the slot's stored 64-bit
  // hash first and the full text after it: two keys are the same key only when their text is. The text of an
  // erased key is reclaimed when the arena is compacted (once half of it is dead) or the map rehashed.
  bool text_ = false;                 // keys as text: koff_ runs beside slots_, the text in arena_
  std::vector<Slot> slots_;
  std::vector<uint32_t> koff_;
  std::vector<char> arena_;
  size_t dead_ = 0;                   // bytes of arena_ held by erased keys
  size_t count_ = 0, used_ = 0;       // live entries; live plus tombstones
  uint64_t (*hash_fn_)(const std::string&) = nullptr;
  // Runs mode: while every key is an integer, arriving in increasing order with the row ids following each other
  // (an auto-increment table, loaded or replicated in order - most of an ERP's), the map is runs of consecutive keys,
  // each with the id of its first row: 16 bytes a run instead of a slot (16 B, at ~2/3 load) a key. Erased keys
  // are listed in `gone_`. Anything else - a text key, a key not after the last, a write through an entry, too many
  // runs or erased keys - turns it into the slots above for good (to_slots), so what a lookup answers is the same
  // either way; only the memory differs. AIOS_SQL_PK_RUNS=0 starts every map as slots.
  struct Run { uint64_t key; uint32_t val; uint32_t n; };   // keys key .. key+n-1 -> ids val .. val+n-1
  bool runs_ = true;
  std::vector<Run> runs_v_;
  std::vector<uint64_t> gone_;   // erased keys, sorted as signed integers
  static bool runs_enabled() {
    static const bool on = [] { const char* e = std::getenv("AIOS_SQL_PK_RUNS"); return !(e && e[0] == '0'); }();
    return on;
  }
  static bool int_less(uint64_t a, uint64_t b) { return (int64_t)a < (int64_t)b; }
  iterator run_iter(uint64_t k, uint32_t v) {
    iterator it{this, 1};
    it.run = true;
    it.run_hit.key = k;
    it.run_hit.second = v;
    it.run_hit.state = 1;
    return it;
  }
  // The overlay on the runs. late_: keys that arrived before the last one (a line added to an old order), and keys
  // re-pointed at another row - runs of their own, sorted, never overlapping a live key of runs_v_ (such a key is in
  // gone_ as well, and late_ answers first). gone_: erased keys of runs_v_. Both are folded into runs_v_ in one pass
  // once they have grown (maybe_fold). Before this, any of these turned the map into slots for good: after a day of
  // replication every map on atl was slots, 340 MB for keys that are ~2 MB as runs.
  std::vector<Run> late_;
  // The run of `rv` that holds key k, or nullptr.
  static const Run* run_at(const std::vector<Run>& rv, uint64_t k) {
    auto r = std::upper_bound(rv.begin(), rv.end(), k, [](uint64_t a, const Run& x) { return int_less(a, x.key); });
    if (r == rv.begin()) return nullptr;
    --r;
    return (k - r->key) < r->n ? &*r : nullptr;
  }
  iterator run_find(const std::string& key) {
    uint64_t ik = 0;
    if ((runs_v_.empty() && late_.empty()) || !int_key(key, &ik)) return end();
    if (const Run* l = run_at(late_, ik)) return run_iter(ik, (uint32_t)(l->val + (ik - l->key)));
    const Run* r = run_at(runs_v_, ik);
    if (!r) return end();
    if (!gone_.empty() && std::binary_search(gone_.begin(), gone_.end(), ik, int_less)) return end();
    return run_iter(ik, (uint32_t)(r->val + (ik - r->key)));
  }
  // Key k -> id v into the overlay (k is not a live key). With `merge` it joins a neighbouring run it continues;
  // without, it is a run of its own (operator[] hands out a reference to that run's id). Returns the run's index.
  size_t late_insert(uint64_t k, uint32_t v, bool merge) {
    auto r = std::upper_bound(late_.begin(), late_.end(), k, [](uint64_t a, const Run& x) { return int_less(a, x.key); });
    const size_t at = (size_t)(r - late_.begin());   // the first run after k
    ++count_;
    if (merge) {
      if (at > 0) {
        Run& p = late_[at - 1];
        if (p.key + p.n == k && (uint64_t)p.val + p.n == v && p.n < UINT32_MAX) { ++p.n; return at - 1; }
      }
      if (at < late_.size()) {
        Run& s = late_[at];
        if (k + 1 == s.key && v + 1 == s.val && s.n < UINT32_MAX) { s.key = k; s.val = v; ++s.n; return at; }
      }
    }
    late_.insert(late_.begin() + (long)at, Run{k, v, 1});
    return at;
  }
  // Key k, a live key, out of the map: out of the overlay if it is there, else marked gone in the runs.
  void run_erase(uint64_t k) {
    --count_;
    auto r = std::upper_bound(late_.begin(), late_.end(), k, [](uint64_t a, const Run& x) { return int_less(a, x.key); });
    if (r != late_.begin() && (k - (r - 1)->key) < (r - 1)->n) {
      --r;
      const uint64_t off = k - r->key;
      if (r->n == 1) late_.erase(r);
      else if (off == 0) { ++r->key; ++r->val; --r->n; }
      else if (off == r->n - 1) --r->n;
      else {
        const Run tail{k + 1, (uint32_t)(r->val + off + 1), (uint32_t)(r->n - off - 1)};
        r->n = (uint32_t)off;
        late_.insert(r + 1, tail);
      }
      return;
    }
    gone_.insert(std::lower_bound(gone_.begin(), gone_.end(), k, int_less), k);
  }
  // The overlay into the runs, once it holds a few thousand entries or a 64th of the map (a pass over the runs).
  void maybe_fold() {
    if (late_.size() + gone_.size() > 4096 + count_ / 64) fold();
  }
  void fold() {
    if (!runs_ || (late_.empty() && gone_.empty())) return;
    std::vector<Run> fresh;
    fresh.reserve(runs_v_.size() + late_.size() + gone_.size());
    size_t g = 0, li = 0, n = 0;
    uint32_t lj = 0;
    bool ok = true;
    auto take = [&](uint64_t k, uint32_t v) { ok = ok && append_run(fresh, k, v); ++n; };
    // The overlay's entries before key k (all of them when `all`); true when the next one is k itself.
    auto late_upto = [&](uint64_t k, bool all) {
      while (li < late_.size() && ok) {
        const uint64_t lk = late_[li].key + lj;
        if (!all) {
          if (lk == k) return true;
          if (!int_less(lk, k)) return false;
        }
        take(lk, late_[li].val + lj);
        if (++lj == late_[li].n) { ++li; lj = 0; }
      }
      return false;
    };
    for (const Run& r : runs_v_) {
      for (uint32_t j = 0; j < r.n && ok; ++j) {
        const uint64_t k = r.key + j;
        if (late_upto(k, false)) {   // re-pointed: the overlay's id, not this run's
          take(k, late_[li].val + lj);
          if (++lj == late_[li].n) { ++li; lj = 0; }
          continue;
        }
        while (g < gone_.size() && int_less(gone_[g], k)) ++g;
        if (g < gone_.size() && gone_[g] == k) continue;
        take(k, r.val + j);
      }
      if (!ok) break;
    }
    late_upto(0, true);
    std::vector<Run>().swap(late_);
    std::vector<uint64_t>().swap(gone_);
    // Nearly a run a key, and the keys are pairs: the other packing may make runs of them (see pair_swap_).
    if (ok && shape_ == 2 && !pair_swap_ && fresh.size() * 2 > n + 64) {
      std::vector<std::pair<uint64_t, uint32_t>> all;   // (swapped key, id)
      all.reserve(n);
      bool fits = true;
      for (const Run& r : fresh) {
        for (uint32_t j = 0; j < r.n && fits; ++j) {
          const uint64_t k = r.key + j, a = k >> kPairLowBits, b = k & ((1ull << kPairLowBits) - 1);
          if (a >= (1ull << kPairLowBits)) { fits = false; break; }
          all.push_back({(b << kPairLowBits) | a, r.val + j});
        }
        if (!fits) break;
      }
      if (fits) {
        std::sort(all.begin(), all.end(), [](const auto& x, const auto& y) { return int_less(x.first, y.first); });
        std::vector<Run> swapped;
        bool ok2 = true;
        for (const auto& [k, v] : all) if (!(ok2 = append_run(swapped, k, v))) break;
        if (ok2 && swapped.size() * 2 <= fresh.size()) { pair_swap_ = true; swapped.shrink_to_fit(); fresh.swap(swapped); }
      }
    }
    fresh.shrink_to_fit();
    runs_v_.swap(fresh);
    count_ = n;
    if (!ok) to_slots();
  }
  // Key `k` -> id `v` after the last entry of `rv`, when `k` is after every key there; false otherwise (unchanged).
  static bool append_run(std::vector<Run>& rv, uint64_t k, size_t v) {
    if (v > UINT32_MAX) return false;
    if (!rv.empty()) {
      Run& last = rv.back();
      const uint64_t last_key = last.key + last.n - 1;
      if (!int_less(last_key, k)) return false;
      if (k == last_key + 1 && v == (size_t)last.val + last.n && last.n < UINT32_MAX) { ++last.n; return true; }
    }
    rv.push_back(Run{k, (uint32_t)v, 1});
    return true;
  }
  bool run_append(uint64_t k, size_t v) {
    if (!append_run(runs_v_, k, v)) return false;
    ++count_;
    // (A run per key, keys with gaps, still costs less than a slot a key: 16 B against ~23, so nothing here turns
    // scattered keys into slots.)
    return true;
  }
  // Runs into slots, for good.
  void to_slots() {
    if (!runs_) return;
    runs_to_slots_count().fetch_add(1, std::memory_order_relaxed);
    std::vector<Run> rv;
    rv.swap(runs_v_);
    std::vector<uint64_t> gone;
    gone.swap(gone_);
    runs_ = false;
    const size_t live = count_;
    slots_.clear();
    count_ = used_ = 0;
    rehash(live);
    size_t g = 0;
    for (const Run& r : rv) {
      for (uint32_t j = 0; j < r.n; ++j) {
        const uint64_t k = r.key + j;
        while (g < gone.size() && int_less(gone[g], k)) ++g;
        if (g < gone.size() && gone[g] == k) continue;
        const size_t at = place(k);
        slots_[at] = Slot{k, r.val + j, 1};
        ++count_; ++used_;
      }
    }
    for (const Run& r : late_) {
      for (uint32_t j = 0; j < r.n; ++j) {
        const size_t at = place(r.key + j);
        slots_[at] = Slot{r.key + j, r.val + j, 1};
        ++count_; ++used_;
      }
    }
    late_.clear();
  }
  uint64_t hash_text(const std::string& key) const { return hash_fn_ ? hash_fn_(key) : (uint64_t)std::hash<std::string>{}(key); }
  // The keys of one map are one shape (one table's primary key): shape_ 1, a single integer; 2, two integers packed
  // into 64 bits - an (order, line) pair costs what a single integer does, and its keys, arriving in order, can be
  // runs. 0: no integer key yet. A key of another shape (or a pair out of packing range) makes the map text.
  int shape_ = 0;
  static constexpr int kPairLowBits = 24;   // second part 0 .. 16,777,215; first 0 .. 2^39-1
  // A key's 64-bit form in this map's shape; `adopt`: an empty map takes the key's shape.
  bool int_key(const std::string& key, uint64_t* v, bool adopt = false) {
    int shape = 0;
    if (as_int(key, v)) shape = 1;
    else if (as_pair(key, v, pair_swap_)) shape = 2;
    else return false;
    if (shape_ == 0 && adopt && !text_) shape_ = shape;
    return shape == shape_;
  }
  std::string int_text(uint64_t k) const {
    if (shape_ == 2) {
      const uint64_t hi = k >> kPairLowBits, lo = k & ((1ull << kPairLowBits) - 1);
      return pair_swap_ ? std::to_string(lo) + '\x1f' + std::to_string(hi) + '\x1f' : std::to_string(hi) + '\x1f' + std::to_string(lo) + '\x1f';
    }
    return std::to_string((int64_t)k) + '\x1f';
  }
  // A two-column key packed with its second column first: an order's lines (orderlineno, orderno) arrive line by
  // line within the order, so packed (line << 24 | order) every key was a run of its own; packed (order << 24 |
  // line) an order's lines are one run. Chosen at a fold when it halves the runs (every key re-packed, the second
  // column must fit 24 bits); a lookup packs the same way from then on.
  bool pair_swap_ = false;
  // A canonical integer's digits [p, p+n): no sign handling (the pair's parts are non-negative), no leading zero.
  static bool canon_uint(const char* p, size_t n, uint64_t* v) {
    if (n == 0 || n > 19 || (p[0] == '0' && n > 1)) return false;
    uint64_t a = 0;
    for (size_t i = 0; i < n; ++i) { if (p[i] < '0' || p[i] > '9') return false; a = a * 10 + (uint64_t)(p[i] - '0'); }
    *v = a;
    return true;
  }
  // "<a>\x1f<b>\x1f": a two-column integer key, packed (a << 24 | b) when both fit; the packing keeps the key order.
  static bool as_pair(const std::string& key, uint64_t* v, bool swap = false) {
    const size_t n = key.size();
    if (n < 4 || key[n - 1] != '\x1f') return false;
    const size_t mid = key.find('\x1f');
    if (mid == std::string::npos || mid + 1 >= n - 1) return false;
    uint64_t a = 0, b = 0;
    if (!canon_uint(key.data(), mid, &a) || !canon_uint(key.data() + mid + 1, n - 1 - (mid + 1), &b)) return false;
    if (swap) std::swap(a, b);
    if (a >= (1ull << (63 - kPairLowBits)) || b >= (1ull << kPairLowBits)) return false;
    *v = (a << kPairLowBits) | b;
    return true;
  }
  // "<canonical integer>\x1f": what row_key makes of a single INT key.
  static bool as_int(const std::string& key, uint64_t* v) {
    const size_t n = key.size();
    if (n < 2 || n > 21 || key[n - 1] != '\x1f') return false;
    size_t i = 0;
    const bool neg = key[0] == '-';
    if (neg) i = 1;
    if (i >= n - 1) return false;
    if (key[i] == '0' && n - 1 - i > 1) return false;   // "007" is not canonical
    uint64_t a = 0;
    for (; i < n - 1; ++i) {
      const char c = key[i];
      if (c < '0' || c > '9') return false;
      if (a > (uint64_t)922337203685477580ull) return false;
      a = a * 10 + (uint64_t)(c - '0');
    }
    if (a > (uint64_t)9223372036854775807ull) return false;
    *v = neg ? (uint64_t)(-(int64_t)a) : a;
    return true;
  }
  static uint64_t mix(uint64_t k) {
    k ^= k >> 33; k *= 0xff51afd7ed558ccdull; k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ull; k ^= k >> 33;
    return k;
  }
  // Text entry at arena_[off*4]: varint length, bytes, zero padding to the next multiple of 4.
  uint32_t put_text(const std::string& key) {
    const size_t at = arena_.size();   // always a multiple of 4
    size_t n = key.size();
    do { arena_.push_back((char)((n & 0x7f) | (n > 0x7f ? 0x80 : 0))); n >>= 7; } while (n);
    arena_.insert(arena_.end(), key.begin(), key.end());
    while (arena_.size() & 3) arena_.push_back(0);
    return (uint32_t)(at >> 2);
  }
  // The text of slot i: where its bytes start and how long they are.
  const char* text_of(size_t i, size_t* len) const {
    const char* p = arena_.data() + (size_t)koff_[i] * 4;
    size_t n = 0;
    int shift = 0;
    for (;; ++p, shift += 7) {
      n |= (size_t)((unsigned char)*p & 0x7f) << shift;
      if (!((unsigned char)*p & 0x80)) { ++p; break; }
    }
    *len = n;
    return p;
  }
  size_t entry_bytes(size_t i) const {
    size_t len;
    const char* p = text_of(i, &len);
    return (((size_t)(p - (arena_.data() + (size_t)koff_[i] * 4)) + len) + 3) & ~(size_t)3;
  }
  bool same(size_t i, const std::string& key, uint64_t ik) const {
    if (slots_[i].key != ik) return false;
    if (!text_) return true;
    size_t len;
    const char* p = text_of(i, &len);
    return len == key.size() && std::memcmp(p, key.data(), len) == 0;
  }
  // The slot holding `key`, or - when inserting - the first free slot of its chain.
  size_t locate(const std::string& key, uint64_t ik, bool for_insert) const {
    const size_t mask = slots_.size() - 1;
    size_t i = (size_t)mix(ik) & mask;
    size_t first_free = npos;
    for (size_t n = 0; n < slots_.size(); ++n, i = (i + 1) & mask) {
      const Slot& s = slots_[i];
      if (s.state == 0) return for_insert ? (first_free == npos ? i : first_free) : npos;
      if (s.state == 2) { if (first_free == npos) first_free = i; continue; }
      if (same(i, key, ik)) return i;
    }
    return for_insert ? first_free : npos;
  }
  // The first empty slot of the chain: for a map being rebuilt, where no key is there yet.
  size_t place(uint64_t ik) const {
    const size_t mask = slots_.size() - 1;
    size_t i = (size_t)mix(ik) & mask;
    while (slots_[i].state != 0) i = (i + 1) & mask;
    return i;
  }
  // The live text of `old` into a fresh arena, offsets rewritten: drops what erased keys held.
  void move_texts(const std::vector<Slot>& old_slots, const std::vector<uint32_t>& old_off, const std::vector<char>& old_arena,
                  std::vector<char>* arena, std::vector<uint32_t>* off, const std::vector<size_t>& dest) {
    for (size_t i = 0; i < old_slots.size(); ++i) {
      if (old_slots[i].state != 1) continue;
      const char* base = old_arena.data() + (size_t)old_off[i] * 4;
      const char* p = base;
      size_t n = 0;
      int shift = 0;
      for (;; ++p, shift += 7) {
        n |= (size_t)((unsigned char)*p & 0x7f) << shift;
        if (!((unsigned char)*p & 0x80)) { ++p; break; }
      }
      const size_t total = (((size_t)(p - base) + n) + 3) & ~(size_t)3;
      (*off)[dest[i]] = (uint32_t)(arena->size() >> 2);
      arena->insert(arena->end(), base, base + total);
    }
  }
  void rehash(size_t live) {
    size_t cap = 16;
    while (cap * 2 < live * 3 + 4) cap <<= 1;   // load at most ~2/3 after a rehash (grows again at 0.7)
    std::vector<Slot> old;
    std::vector<uint32_t> old_off;
    std::vector<char> old_arena;
    old.swap(slots_);
    old_off.swap(koff_);
    old_arena.swap(arena_);
    slots_.assign(cap, Slot{});
    if (text_) koff_.assign(cap, 0);
    count_ = used_ = 0;
    dead_ = 0;
    std::vector<size_t> dest(text_ ? old.size() : 0);
    for (size_t i = 0; i < old.size(); ++i) {
      if (old[i].state != 1) continue;
      const size_t j = place(old[i].key);
      slots_[j] = old[i];
      if (text_) dest[i] = j;
      ++count_; ++used_;
    }
    if (text_) move_texts(old, old_off, old_arena, &arena_, &koff_, dest);
  }
  // Half of the arena is erased keys' text: the live text moves to a fresh one, slots stay where they are.
  void compact() {
    std::vector<char> fresh;
    fresh.reserve(arena_.size() - dead_ + 16);
    std::vector<uint32_t> off(koff_.size(), 0);
    std::vector<size_t> dest(slots_.size());
    for (size_t i = 0; i < slots_.size(); ++i) dest[i] = i;
    move_texts(slots_, koff_, arena_, &fresh, &off, dest);
    arena_.swap(fresh);
    koff_.swap(off);
    dead_ = 0;
  }
  // A key that is not an integer: every key from now on is held as text.
  void to_text() {
    std::vector<Slot> old;
    old.swap(slots_);
    text_ = true;
    slots_.assign(old.size(), Slot{});
    koff_.assign(old.size(), 0);
    arena_.clear();
    dead_ = 0;
    count_ = used_ = 0;
    for (const Slot& s : old) {
      if (s.state != 1) continue;
      const std::string k = int_text(s.key);
      const uint64_t ik = hash_text(k);
      const size_t j = place(ik);
      slots_[j] = s;
      slots_[j].key = ik;
      koff_[j] = put_text(k);
      ++count_; ++used_;
    }
  }

  // Sorted mode: the text keys of a big map (a checkpoint sorts one of kSortedMin+ keys) as one arena in key
  // order - a varint length and the bytes, nothing between - with a 32-bit offset and a 32-bit row id a key
  // (skeys_, soff_, sval_): ~9 B a key over the text, where a slot and its koff_ cost ~29 B at 70% load.
  // A lookup bisects: sidx_ (the first 8 bytes of every 64th key) narrows it to a window of a few groups, then
  // the window by full compare. Writes go to an overlay as runs mode's do: slate_, new keys sorted (a key that
  // is in the arena and live is never there: a re-point writes sval_ in place); sgone_, erased arena positions,
  // sorted. Both fold into the arena in one merge (sfold) once they have grown. An id past 32 bits, or a walk
  // with begin(), turns the map back into slots (sorted_to_slots), to be sorted again at a checkpoint.
  // AIOS_SQL_PK_SORTED=0 leaves every map as slots.
  struct SlateEntry { std::string key; uint32_t val; };
  bool sorted_ = false;
  std::vector<char> skeys_;
  std::vector<uint32_t> soff_, sval_;
  std::vector<uint64_t> sidx_;
  std::vector<uint32_t> sgone_;
  std::vector<SlateEntry> slate_;
  static constexpr uint32_t kSidxGroup = 64;
  static constexpr uint64_t kSlateBit = 1ull << 63;   // an iterator's run_hit.key: kSlateBit | overlay index, else the arena position
  static bool sorted_enabled() {
    static const bool on = [] { const char* e = std::getenv("AIOS_SQL_PK_SORTED"); return !(e && e[0] == '0'); }();
    return on;
  }
  static void put_skey(std::vector<char>& keys, const char* p, size_t n) {
    size_t len = n;
    do { keys.push_back((char)((len & 0x7f) | (len > 0x7f ? 0x80 : 0))); len >>= 7; } while (len);
    keys.insert(keys.end(), p, p + n);
  }
  // The key at arena position `pos`: where its bytes start and how long they are.
  const char* skey_at(size_t pos, size_t* len) const {
    const char* p = skeys_.data() + soff_[pos];
    size_t n = 0;
    int shift = 0;
    for (;; ++p, shift += 7) {
      n |= (size_t)((unsigned char)*p & 0x7f) << shift;
      if (!((unsigned char)*p & 0x80)) { ++p; break; }
    }
    *len = n;
    return p;
  }
  // <0, 0, >0 as the key at `pos` compares with `key` (bytes, then length: std::string's order).
  int skey_cmp(size_t pos, const std::string& key) const {
    size_t len;
    const char* p = skey_at(pos, &len);
    const int c = std::memcmp(p, key.data(), std::min(len, key.size()));
    return c != 0 ? c : (len < key.size() ? -1 : len > key.size() ? 1 : 0);
  }
  static uint64_t key_prefix(const char* p, size_t n) {
    uint64_t u = 0;
    for (size_t i = 0; i < 8; ++i) u = (u << 8) | (i < n ? (unsigned char)p[i] : 0);
    return u;
  }
  void build_sidx() {
    sidx_.clear();
    sidx_.reserve(soff_.size() / kSidxGroup + 1);
    for (size_t g = 0; g < soff_.size(); g += kSidxGroup) {
      size_t len;
      const char* p = skey_at(g, &len);
      sidx_.push_back(key_prefix(p, len));
    }
    sidx_.shrink_to_fit();
  }
  // The arena position of `key`, live or erased, or npos.
  size_t sorted_pos(const std::string& key) const {
    if (soff_.empty()) return npos;
    const uint64_t u = key_prefix(key.data(), key.size());
    // Groups whose first key's prefix is below u may hold the key (it sorts after their first key); groups whose
    // first key's prefix is above u cannot: the window is from the last group starting at or below u (or the one
    // before the first starting above it) to the first group starting above u.
    const size_t g_hi = (size_t)(std::upper_bound(sidx_.begin(), sidx_.end(), u) - sidx_.begin());
    const size_t g_lo = (size_t)(std::lower_bound(sidx_.begin(), sidx_.end(), u) - sidx_.begin());
    size_t lo = (g_lo > 0 ? g_lo - 1 : 0) * kSidxGroup;
    size_t hi = std::min(soff_.size(), g_hi * kSidxGroup);
    while (lo < hi) {
      const size_t mid = lo + (hi - lo) / 2;
      const int c = skey_cmp(mid, key);
      if (c == 0) return mid;
      if (c < 0) lo = mid + 1; else hi = mid;
    }
    return npos;
  }
  size_t slate_pos(const std::string& key) const {
    auto at = std::lower_bound(slate_.begin(), slate_.end(), key, [](const SlateEntry& e, const std::string& k) { return e.key < k; });
    return (at != slate_.end() && at->key == key) ? (size_t)(at - slate_.begin()) : npos;
  }
  bool sgone_has(uint32_t pos) const { return !sgone_.empty() && std::binary_search(sgone_.begin(), sgone_.end(), pos); }
  iterator sorted_find(const std::string& key) {
    const size_t s = slate_pos(key);
    if (s != npos) return run_iter(kSlateBit | (uint64_t)s, slate_[s].val);
    const size_t p = sorted_pos(key);
    if (p == npos || sgone_has((uint32_t)p)) return end();
    return run_iter((uint64_t)p, sval_[p]);
  }
  // The id of a live key, to write in place, or nullptr.
  uint32_t* sorted_ref(const std::string& key) {
    const size_t s = slate_pos(key);
    if (s != npos) return &slate_[s].val;
    const size_t p = sorted_pos(key);
    if (p == npos || sgone_has((uint32_t)p)) return nullptr;
    return &sval_[p];
  }
  // A hit of sorted_find out of the map: the overlay entry (the hit is its index, good until the next change,
  // and erase follows find at once), or the arena position marked erased.
  void sorted_erase_hit(uint64_t hit) {
    --count_;
    if (hit & kSlateBit) {
      const size_t s = (size_t)(hit & ~kSlateBit);
      if (s < slate_.size()) slate_.erase(slate_.begin() + (long)s);
      return;
    }
    sgone_.insert(std::lower_bound(sgone_.begin(), sgone_.end(), (uint32_t)hit), (uint32_t)hit);
  }
  void maybe_sfold() {
    if (slate_.size() + sgone_.size() > 4096 + count_ / 64) sfold();
  }
  // The overlay and the erasures into the arena: one merge, in key order.
  void sfold() {
    if (!sorted_ || (slate_.empty() && sgone_.empty())) return;
    std::vector<char> keys;
    keys.reserve(skeys_.size() + slate_.size() * 24);
    std::vector<uint32_t> off, val;
    off.reserve(count_);
    val.reserve(count_);
    size_t g = 0, li = 0;
    bool overflow = false;
    auto take = [&](const char* p, size_t n, uint32_t v) {
      if (keys.size() + n + 5 > UINT32_MAX) { overflow = true; return; }
      off.push_back((uint32_t)keys.size());
      put_skey(keys, p, n);
      val.push_back(v);
    };
    for (size_t p = 0; p < soff_.size() && !overflow; ++p) {
      if (g < sgone_.size() && sgone_[g] == p) { ++g; continue; }
      size_t len;
      const char* kp = skey_at(p, &len);
      // the overlay's keys before this one
      while (li < slate_.size() && !overflow) {
        const std::string& lk = slate_[li].key;
        const int c = std::memcmp(kp, lk.data(), std::min(len, lk.size()));
        const bool arena_first = c < 0 || (c == 0 && len < lk.size());
        if (arena_first) break;
        take(lk.data(), lk.size(), slate_[li].val);
        ++li;
      }
      take(kp, len, sval_[p]);
    }
    for (; li < slate_.size() && !overflow; ++li) take(slate_[li].key.data(), slate_[li].key.size(), slate_[li].val);
    if (overflow) { sorted_to_slots(); return; }
    keys.shrink_to_fit();
    skeys_.swap(keys);
    soff_.swap(off);
    sval_.swap(val);
    std::vector<uint32_t>().swap(sgone_);
    std::vector<SlateEntry>().swap(slate_);
    build_sidx();
    count_ = used_ = sval_.size();
  }
  // The sorted arena back into slots (text), for good until a checkpoint sorts it again.
  void sorted_to_slots() {
    if (!sorted_) return;
    sorted_to_slots_count().fetch_add(1, std::memory_order_relaxed);
    std::vector<char> keys;
    keys.swap(skeys_);
    std::vector<uint32_t> off, val, gone;
    off.swap(soff_);
    val.swap(sval_);
    gone.swap(sgone_);
    std::vector<SlateEntry> late;
    late.swap(slate_);
    std::vector<uint64_t>().swap(sidx_);
    sorted_ = false;
    text_ = true;
    count_ = used_ = 0;
    slots_.clear();
    rehash(val.size() + late.size());
    size_t g = 0;
    std::string k;
    for (size_t p = 0; p < off.size(); ++p) {
      if (g < gone.size() && gone[g] == p) { ++g; continue; }
      const char* q = keys.data() + off[p];
      size_t n = 0;
      int shift = 0;
      for (;; ++q, shift += 7) {
        n |= (size_t)((unsigned char)*q & 0x7f) << shift;
        if (!((unsigned char)*q & 0x80)) { ++q; break; }
      }
      k.assign(q, n);
      emplace(k, val[p]);
    }
    for (const SlateEntry& e : late) emplace(e.key, e.val);
  }
};

// The statement lock and each table's lock. Holds are short (a single-row
// write keeps its table a few microseconds), so a contended taker spins a
// moment before it sleeps, and a release goes to the kernel only when
// someone is asleep.
//
// Phase-fair: a waiting writer holds new readers back, but a reader that
// went to sleep behind writer phase P goes in as soon as P is over, however
// many writers queue after it - each writer lets every reader parked before
// it through. With plain writer preference and one wakeup for everyone,
// 64 sessions of read_write convoyed: a reader could wait out a chain of
// writers, and every release woke all 64 threads to find most must sleep
// again (80% of the server's futex calls; 17% of the CPU idle).
class SqlRwLock {
 public:
  void lock_shared() {
    for (int spin = 0;; ++spin) {
      int32_t s = state_.load(std::memory_order_acquire);
      if (s >= 0 && (waiting_writers_.load(std::memory_order_acquire) == 0 || in_grace())) {
        if (state_.compare_exchange_weak(s, s + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) return;
        continue;
      }
      if (spin < spins()) { relax(); continue; }
      break;
    }
    const uint64_t phase = phase_.load(std::memory_order_seq_cst);
    for (;;) {
      const uint32_t seen = rseq_.load(std::memory_order_seq_cst);   // before the check: a wake after it ends the wait
      rsleep_.fetch_add(1, std::memory_order_seq_cst);
      int32_t s = state_.load(std::memory_order_seq_cst);
      bool got = false;
      while (s >= 0 && (waiting_writers_.load(std::memory_order_seq_cst) == 0 || phase_.load(std::memory_order_seq_cst) != phase || in_grace())) {
        if (state_.compare_exchange_weak(s, s + 1, std::memory_order_seq_cst, std::memory_order_seq_cst)) { got = true; break; }
      }
      if (!got) futex_wait(rseq_, seen);
      rsleep_.fetch_sub(1, std::memory_order_seq_cst);
      if (got) return;
    }
  }
  // The shared lock if it is free right now (no holder writing, no writer waiting); false without waiting.
  bool try_lock_shared() {
    int32_t s = state_.load(std::memory_order_acquire);
    while (s >= 0 && waiting_writers_.load(std::memory_order_acquire) == 0) {
      if (state_.compare_exchange_weak(s, s + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) return true;
    }
    return false;
  }
  void unlock_shared() {
    if (state_.fetch_sub(1, std::memory_order_seq_cst) != 1) return;
    if (wsleep_.load(std::memory_order_seq_cst) > 0) {
      wseq_.fetch_add(1, std::memory_order_seq_cst);
      futex_wake(wseq_, 1);
    }
    if (gsleep_.load(std::memory_order_seq_cst) > 0) {
      gseq_.fetch_add(1, std::memory_order_seq_cst);
      futex_wake(gseq_, INT32_MAX);
    }
  }
  // A short latch of the table's column `c` (columns share 16), taken by a
  // row writer around what it reads and writes of that column: row writers
  // run together, and a column's codes, dictionary and zone maps are one
  // writer's at a time. Taken in ascending order when a writer needs several.
  // A latch is held for one cell's write: a few spins before sleeping, as a
  // mutex that slept at once queued every row writer of a busy column.
  void latch_column(size_t c) {
    std::atomic<uint32_t>& l = latches_[c % kLatches];
    for (int spin = 0, n = spins();; ++spin) {
      uint32_t free = 0;
      if (l.load(std::memory_order_relaxed) == 0 && l.compare_exchange_weak(free, 1, std::memory_order_acquire)) return;
      if (spin >= n) break;
      relax();
    }
    // 2: held, someone may sleep on it
    while (l.exchange(2, std::memory_order_acquire) != 0) futex_wait(l, 2);
  }
  void unlatch_column(size_t c) {
    std::atomic<uint32_t>& l = latches_[c % kLatches];
    if (l.exchange(0, std::memory_order_release) == 2) futex_wake(l, 1);
  }
  static constexpr size_t kLatches = 16;
  // Row writers: any number together (state_ kRows + n), none beside a
  // reader or the exclusive writer. A row writer joins a running group only
  // while no reader sleeps and no exclusive writer waits for the table, so
  // neither starves behind a stream of row writes.
  void lock_rows() {
    if (waiting_writers_.fetch_add(1, std::memory_order_seq_cst) == 0) writer_since_.store(now_ns(), std::memory_order_relaxed);
    auto try_take = [&]() {
      int32_t s = state_.load(std::memory_order_seq_cst);
      for (;;) {
        int32_t next;
        if (s == 0) next = kRows + 1;
        else if (s < -1 && xwait_.load(std::memory_order_seq_cst) == 0 && rsleep_.load(std::memory_order_seq_cst) == 0) next = s + 1;
        else return false;
        if (state_.compare_exchange_weak(s, next, std::memory_order_seq_cst, std::memory_order_seq_cst)) return true;
      }
    };
    bool got = false;
    for (int spin = 0, n = spins(); spin < n && !(got = try_take()); ++spin) relax();
    while (!got) {
      const uint32_t seen = gseq_.load(std::memory_order_seq_cst);
      gsleep_.fetch_add(1, std::memory_order_seq_cst);
      got = try_take();
      if (!got) futex_wait(gseq_, seen);
      gsleep_.fetch_sub(1, std::memory_order_seq_cst);
      for (int spin = 0, n = spins(); !got && spin < n && state_.load(std::memory_order_relaxed) != 0; ++spin) relax();
    }
    waiting_writers_.fetch_sub(1, std::memory_order_seq_cst);
  }
  void unlock_rows() {
    int32_t s = state_.load(std::memory_order_seq_cst);
    while (!state_.compare_exchange_weak(s, s == kRows + 1 ? 0 : s - 1, std::memory_order_seq_cst, std::memory_order_seq_cst)) {}
    if (s == kRows + 1) wake_all_after_writer();
  }
  void lock() {
    xwait_.fetch_add(1, std::memory_order_seq_cst);
    struct XWait { std::atomic<int32_t>& w; ~XWait() { w.fetch_sub(1, std::memory_order_seq_cst); } } xw{xwait_};
    if (waiting_writers_.fetch_add(1, std::memory_order_seq_cst) == 0) writer_since_.store(now_ns(), std::memory_order_relaxed);
    for (int spin = 0, n = spins(); spin < n; ++spin) {
      int32_t none = 0;
      if (state_.load(std::memory_order_relaxed) == 0 &&
          state_.compare_exchange_strong(none, -1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        waiting_writers_.fetch_sub(1, std::memory_order_seq_cst);
        writer_since_.store(now_ns(), std::memory_order_relaxed);   // the next waiting writer's grace starts now
        return;
      }
      relax();
    }
    for (;;) {
      const uint32_t seen = wseq_.load(std::memory_order_seq_cst);
      wsleep_.fetch_add(1, std::memory_order_seq_cst);
      int32_t none = 0;
      const bool got = state_.compare_exchange_strong(none, -1, std::memory_order_seq_cst, std::memory_order_seq_cst);
      if (!got) futex_wait(wseq_, seen);
      wsleep_.fetch_sub(1, std::memory_order_seq_cst);
      if (got) break;
      // Woken: a few spins first - the lock is often taken again by a
      // running thread a moment before this one gets the CPU back.
      for (int spin = 0, n = spins(); spin < n && state_.load(std::memory_order_relaxed) != 0; ++spin) relax();
    }
    waiting_writers_.fetch_sub(1, std::memory_order_seq_cst);
    writer_since_.store(now_ns(), std::memory_order_relaxed);
  }
  void unlock() {
    state_.store(0, std::memory_order_seq_cst);
    wake_all_after_writer();
  }
 private:
  // AIOS_LOCK_SPIN: the spins before a taker sleeps (200; 0 parks at once).
  static int spins() {
    static const int n = [] { const char* e = std::getenv("AIOS_LOCK_SPIN"); return e && *e ? std::atoi(e) : 200; }();
    return n;
  }
  static void relax() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
  }
  // Sleeping is on two futex words, readers' and writers', with no mutex: a
  // sleeper reads its word, counts itself, checks the lock and sleeps only if
  // the word still reads the same; a waker changes the lock, then reads the
  // counts and bumps the word (all seq_cst), so no wake falls between a
  // sleeper's check and its sleep. A mutex and condition variables here had
  // every unlocker of a busy table queue on the mutex behind the waiters it
  // woke - 5,000 stalls a second at 128 sessions writing one table.
  static void futex_wait(std::atomic<uint32_t>& w, uint32_t seen) {
#if defined(__linux__)
    ::syscall(SYS_futex, reinterpret_cast<uint32_t*>(&w), FUTEX_WAIT_PRIVATE, seen, nullptr, nullptr, 0);
#else
    w.wait(seen, std::memory_order_seq_cst);
#endif
  }
  static void futex_wake(std::atomic<uint32_t>& w, int n) {
#if defined(__linux__)
    ::syscall(SYS_futex, reinterpret_cast<uint32_t*>(&w), FUTEX_WAKE_PRIVATE, n, nullptr, nullptr, 0);
#else
    if (n == 1) w.notify_one(); else w.notify_all();
#endif
  }
  // The exclusive writer (or a row writers' group) is done: a new phase, so
  // the readers parked behind it may go in, and the writers waiting woken -
  // one exclusive writer, every row writer.
  void wake_all_after_writer() {
    phase_.fetch_add(1, std::memory_order_seq_cst);
    if (rsleep_.load(std::memory_order_seq_cst) > 0) {
      rseq_.fetch_add(1, std::memory_order_seq_cst);
      futex_wake(rseq_, INT32_MAX);
    }
    if (wsleep_.load(std::memory_order_seq_cst) > 0) {
      wseq_.fetch_add(1, std::memory_order_seq_cst);
      futex_wake(wseq_, 1);
    }
    if (gsleep_.load(std::memory_order_seq_cst) > 0) {
      gseq_.fetch_add(1, std::memory_order_seq_cst);
      futex_wake(gseq_, INT32_MAX);
    }
  }
  static constexpr int32_t kRows = INT32_MIN;   // state_ kRows + n: n row writers hold it
  std::atomic<uint32_t> rseq_{0};             // readers sleep on it
  std::atomic<uint32_t> wseq_{0};             // the exclusive writers sleep on it
  std::atomic<uint32_t> gseq_{0};             // row writers sleep on it
  std::atomic<int32_t> gsleep_{0};
  std::atomic<int32_t> xwait_{0};             // exclusive writers waiting (row writers do not join a group then)
  std::atomic<uint32_t> latches_[kLatches] = {};
  std::atomic<int32_t> state_{0};             // readers holding it, -1 for the writer, kRows + n for n row writers
  std::atomic<int32_t> waiting_writers_{0};
  std::atomic<int32_t> rsleep_{0};
  std::atomic<int32_t> wsleep_{0};
  std::atomic<uint64_t> phase_{0};            // writer releases so far
  std::atomic<uint64_t> writer_since_{0};     // when the writers now waiting began to (monotonic ns)
  // Readers still go in past a waiting writer for AIOS_LOCK_READER_GRACE_US (5,000)
  // of its wait, while the table is held shared anyway: a reader that queued
  // behind it waited for the writer's hold as well as for the readers ahead.
  // Past the grace new readers wait, so the writer is never starved.
  static uint64_t now_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }
  static uint64_t grace_ns() {
    static const uint64_t g = [] {
      const char* e = std::getenv("AIOS_LOCK_READER_GRACE_US");
      return (uint64_t)(e && *e ? std::strtoull(e, nullptr, 10) : 5000) * 1000ull;
    }();
    return g;
  }
  bool in_grace() const {
    const uint64_t g = grace_ns();
    return g && now_ns() - writer_since_.load(std::memory_order_relaxed) < g;
  }
};

// One source table of an incrementally maintained grouped view (a view kept as
// a table, refreshed a group at a time). The table's writers report the rows
// they change here; the view, next time it is read, recomputes only the groups
// those rows belong to. Anything a watch cannot attribute to a group sets
// `all`, and the view is rebuilt whole - so a gap costs time, never a wrong
// answer.
struct MatViewWatch {
  aios::hal::Mutex mu;
  std::vector<int> key_cols;                 // the table's columns that hold the view's group key; empty: none
  std::vector<char> refs;                    // per column: the view reads it
  std::unordered_set<std::string> keys;      // groups with a changed row since the last refresh
  bool all = false;                          // a change that belongs to no single group
  uint64_t seq = 0;                          // the table's mutation_seq after the last change seen
  int64_t delta_rows = 0;                    // rows added minus rows removed since the last refresh
  size_t base_rows = 0;                      // the table's row count at the last refresh
};

// The watches of one table, behind their own lock. A table is copied by value
// in places, and a watch is added by a statement that only holds the read
// lock, so the list cannot be a plain vector inside it.
struct MatViewWatchList {
  aios::hal::Mutex mu;
  std::vector<std::weak_ptr<MatViewWatch>> list;
  void add(const std::shared_ptr<MatViewWatch>& w) {
    std::lock_guard<aios::hal::Mutex> lk(mu);
    list.push_back(w);
  }
  bool has(const std::shared_ptr<MatViewWatch>& w) {
    std::lock_guard<aios::hal::Mutex> lk(mu);
    for (const auto& x : list) if (x.lock() == w) return true;
    return false;
  }
  // The watches still alive; the dead ones are dropped.
  std::vector<std::shared_ptr<MatViewWatch>> live() {
    std::lock_guard<aios::hal::Mutex> lk(mu);
    std::vector<std::shared_ptr<MatViewWatch>> out;
    for (auto it = list.begin(); it != list.end();) {
      if (auto w = it->lock()) { out.push_back(std::move(w)); ++it; }
      else it = list.erase(it);
    }
    return out;
  }
  bool empty() {
    std::lock_guard<aios::hal::Mutex> lk(mu);
    return list.empty();
  }
};

struct SqlTable {
  std::string name;
  std::string create_time;
  // The AUTO_INCREMENT counter as MariaDB keeps it: one past the largest id handed out, kept across deletes,
  // ALTER TABLE ... AUTO_INCREMENT = n and restarts (0: not known yet; the largest stored id then decides).
  long long auto_increment_next = 0;
  std::string comment;   // COMMENT='text' table option
  // TEXT_CODEC = LZ4 | DEFLATE table option (ColumnStore::BlockCodec): how
  // this table's block-compressed text columns are packed. LZ4 unless
  // said otherwise; DEFLATE for a table written far more than it is read
  // (an audit log), whose blocks then take about a quarter of the memory.
  // Kept in the store (AQTC trailer) and shown by SHOW CREATE TABLE.
  uint8_t text_codec = 0;
  // Stamped by every write (sql_next_mutation_seq); the store checkpoint
  // compares it with the stamp of the file it last wrote for this table.
  uint64_t mutation_seq = 0;
  // The writes since the table's file was last saved, in the order they were made, so a checkpoint can add them
  // to the file as a part instead of writing the table whole (snapshot_tables_v4): rows erased (by their places
  // then), cells set, rows appended (as stored). Only set_cell, erase_rows / erase_row and the appends record; a
  // write that does not still stamps mutation_seq, and the journal is used only when its last recorded stamp is
  // the table's current one and each record found the stamp the one before it left - anything else in between
  // and the table is written whole, as before. On from the checkpoint that saved the table (journal_begin).
  struct ChangeJournal {
    struct Op {
      uint8_t kind = 0;                  // 'E' erase rows, 'S' set a cell, 'A' append rows
      std::vector<uint64_t> rows;        // E: the rows, ascending, numbered as they were
      uint64_t row = 0;                  // S
      uint32_t col = 0;                  // S
      std::string val;                   // S: as stored
      std::vector<std::string> cells;    // A: the rows appended, row-major, as stored
    };
    bool on = false;
    bool broken = false;
    uint64_t start_seq = 0;   // mutation_seq of the save it starts from
    uint64_t last_seq = 0;    // mutation_seq the newest record left
    size_t bytes = 0;
    size_t cells = 0;         // rows erased + cells set: what a part would replay
    std::vector<Op> ops;
  };
  ChangeJournal journal;
  std::shared_ptr<aios::hal::Mutex> journal_mu = std::make_shared<aios::hal::Mutex>();
  void journal_begin();
  // A recorded write: `before` is mutation_seq as the write found it, `after` what it left.
  void journal_note(uint64_t before, uint64_t after, ChangeJournal::Op&& op);
  // A change of form, not of rows (stats refreshed, a dictionary compacted, the table re-encoded or packed):
  // stamped as a write is, so a checkpoint saves it, while the journal stays good - its rows are the same rows.
  void stamp_form_change();
  std::vector<size_t> cluster_columns() const;
  bool rows_in_key_order(const std::vector<size_t>& cols, size_t from = 0) const;
  std::vector<size_t> scan_order_key() const;   // see the .cpp
  mutable uint64_t key_order_seq_ = ~0ull;   // the layout_seq it was last known at; ~0 never
  mutable bool key_order_ok_ = true;
  mutable size_t key_order_rows_ = 0;        // the row count it was known for: appended rows are checked from there
  // Stamped only when rows already stored change place or value (UPDATE,
  // DELETE, re-encoding) - not by appends. An index over the first N rows
  // stays valid while it holds; appended rows are its tail.
  uint64_t layout_seq = 0;
  // Built from a query result (a materialized view, a derived table): its
  // column types were inferred, not declared, so a "text" column may really
  // hold numbers and keeps the look-at-the-value reading.
  bool inferred_types = false;
  std::string database = "aios_db";
  std::vector<SqlColumn> columns;
  mutable std::vector<ColumnStore> col_data;
  std::vector<SqlIndex> indices;
  std::vector<SqlForeignKey> foreign_keys;
  std::vector<SqlCheck> checks;
  // Generated columns or CHECKs: a row written is worked out and tried (apply_row_rules).
  bool has_rules() const {
    if (!checks.empty()) return true;
    for (const auto& c : columns) if (!c.generated.empty()) return true;
    return false;
  }
  mutable std::shared_ptr<aios::hal::Mutex> probe_mu = std::make_shared<aios::hal::Mutex>();
  // The table's own statement lock: readers take it shared as they resolve
  // the table, a table-scoped write takes it exclusive (sql_database.cpp,
  // "per-table statement locks"). Shared by copies of the table.
  mutable std::shared_ptr<SqlRwLock> tlock = std::make_shared<SqlRwLock>();
  // Shared owners, not values: a scan holds its index for the length of the
  // query, while eviction (the byte budget, FLUSH, a rebuild) can drop the map
  // entry underneath it at any moment. Held by value that was a use-after-free
  // -- FLUSH MEMORY under live traffic SIGSEGV'd chi-il-1 on 2026-09-18.
  mutable std::unordered_map<int, std::shared_ptr<SqlProbeIndex>> probe_indices;

  // Lock-Free / Fast Delta Buffer for instant O(1) hot write ingest
  struct DeltaStore {
    mutable std::shared_ptr<aios::hal::Mutex> mu = std::make_shared<aios::hal::Mutex>();
    std::vector<SqlRow> rows;
    std::atomic<size_t> count{0};

    DeltaStore() = default;
    DeltaStore(const DeltaStore& o) {
      if (o.mu) {
        std::lock_guard<aios::hal::Mutex> lk(*o.mu);
        rows = o.rows;
        count.store(o.count.load(std::memory_order_relaxed), std::memory_order_relaxed);
      }
    }
    DeltaStore& operator=(const DeltaStore& o) {
      if (this != &o) {
        std::lock_guard<aios::hal::Mutex> lk1(*mu);
        std::lock_guard<aios::hal::Mutex> lk2(*o.mu);
        rows = o.rows;
        count.store(o.count.load(std::memory_order_relaxed), std::memory_order_relaxed);
      }
      return *this;
    }
  };
  mutable DeltaStore delta_buffer;

  std::string full_name() const {
    return database.empty() ? name : (database + "." + name);
  }
  int get_column_index(const std::string& col_name) const;

  size_t row_count() const {
    size_t base = col_data.empty() ? 0 : col_data[0].size();
    return base + delta_buffer.count.load(std::memory_order_relaxed);
  }
  bool empty() const {
    return row_count() == 0;
  }
  const std::string& cell(size_t r, size_t c) const;
  std::string_view cell_view(size_t r, size_t c) const;   // see ColumnStore::get_view
  void set_cell(size_t r, size_t c, const std::string& val);
  void normalize_row(std::vector<std::string>& vals) const;
  void append_row(const std::vector<std::string>& vals);
  void append_row(const SqlRow& r);
  void append_row_canonical(std::vector<std::string> vals);   // already in each column's form: no normalising
  // A load's rows (each one a value per column, as append_row takes them), appended column by column with the
  // columns spread over the cores; the rows are consumed. The same result as append_row for each, in order.
  void append_rows_bulk(std::vector<std::vector<std::string>>& rows);
  void append_row_hot(const std::vector<std::string>& vals);
  void append_row_hot(const SqlRow& r);
  void compact_delta_buffer() const;
  void release_slack();   // every column's spare vector capacity goes back to the allocator
  // Materialized grouped views reading this table (see MatViewWatch). Weak: a
  // view dropped or replaced stops being told without the table knowing.
  mutable std::shared_ptr<MatViewWatchList> watch_list = std::make_shared<MatViewWatchList>();
  std::vector<std::shared_ptr<MatViewWatch>> live_watches() const { return watch_list->live(); }
  bool has_watch(const std::shared_ptr<MatViewWatch>& w) const { return watch_list->has(w); }
  void erase_row(size_t r);
  void erase_rows(const std::vector<size_t>& rows);   // ascending, distinct
  void clear_rows();
  SqlRow get_row(size_t r) const;
  void ensure_columns();
  void reserve(size_t sz);
  // Re-encodes every column; `cluster` first sorts the rows into key order (cluster_by_key), which moves them.
  void optimize_storage(bool cluster = true);
  // optimize_storage(false) in three steps, so the encoding itself holds no lock: take_encoding_copy (under the
  // exclusive lock: the delta folded in, the columns copied), encode_columns (the copy, holding nothing), then
  // install_encoding (under the exclusive lock again), which keeps the new columns only if no write came between
  // - false, and the table as it was, when one did. A table whose columns share a dictionary is not copied (the
  // copy would still share it with the live table): take_encoding_copy says false and it is re-encoded in place.
  bool take_encoding_copy(std::vector<ColumnStore>* out, uint64_t* seq);
  static void encode_columns(std::vector<ColumnStore>& cols, const std::vector<SqlColumn>& defs);
  bool install_encoding(std::vector<ColumnStore>& cols, uint64_t seq);
  void cluster_by_key();
  void warm_up_all() const;
  // Caller already holds probe_mu. The budget sweep does, and probe_mu is not
  // recursive.
  size_t probe_indices_memory_bytes_locked() const {
    size_t mem = 0;
    for (const auto& kv : probe_indices) {
      if (kv.second) mem += kv.second->estimate_memory_bytes();
    }
    return mem;
  }
  size_t probe_indices_memory_bytes() const {
    if (!probe_mu) return probe_indices_memory_bytes_locked();
    std::lock_guard<aios::hal::Mutex> lk(*probe_mu);
    return probe_indices_memory_bytes_locked();
  }
  // Caller holds probe_mu.
  void prune_probe_indices(size_t max_count = 2, int keep_col = -1) const {
    while (probe_indices.size() > max_count) {
      int oldest_col = -1;
      uint64_t oldest_ns = UINT64_MAX;
      for (const auto& kv : probe_indices) {
        if (kv.first == keep_col) continue;
        if (!kv.second) continue;
        if (kv.second->last_access_ns < oldest_ns) {
          oldest_ns = kv.second->last_access_ns;
          oldest_col = kv.first;
        }
      }
      if (oldest_col >= 0) {
        probe_indices.erase(oldest_col);
      } else {
        break;
      }
    }
  }
  size_t estimated_memory_bytes() const {
    size_t mem = 0;
    for (const auto& c : col_data) mem += c.estimate_memory_bytes();
    // probe_indices and delta_buffer were excluded here before: the RAM panel
    // read only the compressed columnstore and missed the two things that
    // actually grow without bound -- the lazily-built, never-evicted probe
    // index cache, and rows staged ahead of the next CDC flush.
    mem += probe_indices_memory_bytes();
    {
      std::lock_guard<aios::hal::Mutex> lk(*delta_buffer.mu);
      for (const auto& r : delta_buffer.rows) {
        mem += sizeof(r);
        for (const auto& v : r.values) mem += sizeof(v) + v.capacity();
      }
    }
    return mem;
  }
  size_t delta_rows() const {
    return delta_buffer.count.load(std::memory_order_relaxed);
  }
};

struct SqlView {
  std::string name;
  std::string database = "aios_db";
  std::string query_sql;
  std::vector<std::string> column_aliases;
  std::string definer = "`root`@`localhost`";
  std::string algorithm = "UNDEFINED";
  std::string security = "DEFINER";
  std::vector<SqlColumn> columns;

  std::string full_name() const {
    return database.empty() ? name : (database + "." + name);
  }
};

struct DbConnectionProfile {
  std::string id = "local_embedded";
  std::string name = "Local Embedded DB";
  std::string host = "127.0.0.1";
  uint16_t port = 3306;
  std::string socket_path;
  std::string user = "root";
  std::string password;
  // The password's file (first line), instead of the password itself in
  // connections.ini: password_file=/path. When set, `password` is read from it.
  std::string password_file;
  std::string database = "aios_db";
  bool use_ssl = false;
  std::string ssl_ca;
  std::string ssl_cert;
  std::string ssl_key;
  std::string ssl_cipher;
  std::string ssl_capath;
  std::string ssl_crl;
  std::string ssl_crlpath;
  std::string ssl_mode;
  std::string ssh_host;
  uint16_t ssh_port = 22;
  std::string ssh_user;
  std::string ssh_password;
  std::string ssh_key;
  bool is_local_embedded = true;
  // A Thing that acts (WRITES ... TO <this profile>) may write here. Off unless
  // someone marks the profile - things_write=1 in connections.ini.
  bool things_write = false;
  bool connected = false;
  SqlDialect dialect = SqlDialect::MariaDB;

  static Result<DbConnectionProfile> from_uri(const std::string& uri);
};

class SqlDatabase;
// What a session to this profile dials: its unix socket when socket_path is
// set (MariaWireSession reads a host that is a path as one), else its host.
std::string sql_profile_dial_host(const DbConnectionProfile& p);
// Ok when this build can connect the way the profile asks. SSL and SSH
// tunnels are not implemented by MariaWireSession: a profile that asks for
// either is refused rather than connected in plain text.
Result<void> sql_profile_transport_ok(const DbConnectionProfile& p);
// Set by the MySQL wire server around a statement: its client reads the rows,
// so a SELECT skips rendering the text table (a 19k-row SELECT * spent more
// time padding an ASCII table nobody saw than finding the rows).
void sql_set_wire_results(bool on);
bool sql_wire_results();
// The auto-increment id the connection's last INSERT generated (its first one), as MySQL keeps it per connection:
// LAST_INSERT_ID() reads it, and the wire reports the id each statement generated in its OK packet (what
// mysqli's insert_id and PDO's lastInsertId read). sql_take_statement_insert_id() returns that id for the
// statement just run (0 when it generated none) and clears it for the next.
uint64_t sql_last_insert_id();
uint64_t sql_take_statement_insert_id();
// While set on this thread, every table a SELECT's plan reads is added to
// *sink (a Thing's links are the tables its query actually touches).
void sql_set_read_capture(std::unordered_set<const struct SqlTable*>* sink);
// Set by the binlog consumer on its thread: the Things react at each
// transaction's commit rather than after every row statement.
void sql_set_cdc_thread(bool on);
// While > 0 on this thread, writes leave the Things' reactions pending: the
// caller settles them at its own boundary (a replayed invoice, a batch).
void sql_things_defer(int delta);
// While > 0 on this thread, redo records are held and written together at
// SqlDatabase::flush_redo_pending (one fdatasync per batch, not per statement).
void sql_redo_group(int delta);
class ThingEngine;
class CellEngine;
class ActiveDataEngine;
struct SelectEngine;
struct ResultViewStore;

// A SQL expression compiled once against a table's columns and evaluated on
// single rows, including a row the table does not hold (a deleted row's
// before image). It is the engine's own evaluator - the same NULLs,
// collations and functions a WHERE clause gets - over one row only: a
// subquery is refused. What cells (sql_cells.hpp) run on every row change.
class SqlRowExpr {
 public:
  static std::unique_ptr<SqlRowExpr> compile(SqlDatabase& db, const SqlTable& like, const std::string& expr,
                                             std::string* err);
  ~SqlRowExpr();
  struct Value {
    bool null = true;
    bool numeric = false;
    std::string text;
  };
  bool eval(const SqlRow& row, Value* out, std::string* err);
  // A WHEN: true only when the value is non-NULL and true.
  bool holds(const SqlRow& row, std::string* err);
  // The parsed expression as the engine's own WHERE matching reads it: two texts of one condition agree.
  std::string signature() const;

 private:
  SqlRowExpr();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Runs a nested SELECT against this database's own engine, for the expression
// evaluator. A free function because evaluation is one, and a friend rather
// than a public method because a subquery must never take the path
// execute_query() would: with a remote profile selected that ships the
// statement to another server, where the outer row a correlated subquery reads
// does not exist. Not part of the API.
SqlQueryResult run_subselect(SqlDatabase& db, const std::string& sql);

// CTE / ephemeral-table scopes, innermost last (see SqlDatabase::cte_stack).
// The tables shared, not copied: a view, derived table, window or UNION result is the whole result as a table, and
// a scope holding its own copy kept two of it (three, with the rows it was built from) for the statement's life.
using SqlCteStack = std::deque<std::unordered_map<std::string, std::shared_ptr<SqlTable>>>;

// The SQLSTATE a stored program's SIGNAL gave the error `msg` on this thread.
bool sql_signal_state(const std::string& msg, std::string* state);

class SqlDatabase {
 public:
  SqlDatabase(Kernel& kernel, TaskLedger* ledger = nullptr);
  ~SqlDatabase();

  friend struct SelectEngine;
  friend SqlQueryResult run_subselect(SqlDatabase& db, const std::string& sql);

  struct MariaDbConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 3306;
    std::string user = "root";
    std::string password;
    std::string database = "aios_db";
    bool connected = false;
  };

  Result<void> connect_mariadb(const std::string& host, uint16_t port, const std::string& user, const std::string& pass, const std::string& db);
  const MariaDbConfig& mariadb_config() const { return mariadb_config_; }

  std::vector<DbConnectionProfile> list_profiles() const;
  std::vector<DbConnectionProfile> discover_fleet_databases() const;
  Result<void> add_or_update_profile(const DbConnectionProfile& prof);
  Result<void> remove_profile(const std::string& id);
  const DbConnectionProfile& active_profile() const;
  Result<void> switch_profile(const std::string& id);
  Result<void> test_profile_connection(const DbConnectionProfile& prof);
  std::vector<std::string> list_databases();
  Result<void> use_database(const std::string& db);
  Result<void> save_profiles();
  Result<void> load_profiles();
  void sync_telemetry_registry(uint16_t asql_port = 0, uint16_t mysql_port = 0, bool read_only = false);
  void scan_local_stores(const std::string& extra_dir = "");

  Result<void> init(const std::string& db_dir = "sql_db");
  std::string effective_database() const;
  SqlQueryResult execute_query(const std::string& sql_query, const std::string& session_db = "");
  SqlQueryResult execute_local_query(const std::string& sql_query, const std::string& session_db = "");
  // The same, without the out-of-memory guard (the guarded entry points wrap these).
  SqlQueryResult execute_local_query_unguarded(const std::string& sql_query, const std::string& session_db = "");
  SqlQueryResult execute_gateway_query_unguarded(const std::string& sql_query, const std::string& session_db = "");
  // The write-side counterpart to `SELECT ... FROM OBJECTS`: puts a real,
  // AQL-queryable StoredObject into the kernel's object store instead of a
  // row in a flat SQL table. `INSERT INTO OBJECTS (kind, key, payload) VALUES
  // ('conv_kv', 'c123', '{"json":true}')` — kind/key become metadata, payload
  // becomes the object body. Each call appends a new version; readers pick
  // the latest with `ORDER BY timestamp DESC LIMIT 1`, same as any other
  // AQL-visible history.
  SqlQueryResult execute_object_insert(const std::string& sql_query);
  // Every statement in `script`, stopping at the first that fails. This is what
  // a studio's editor runs when the person selects three statements and hits
  // go, so a half-applied script reports which statement stopped it.
  SqlQueryResult execute_script(const std::string& script);
  SqlQueryResult import_mysql_sql(const std::string& sql_text);
  SqlQueryResult import_sql_file(const std::string& file_path);
  SqlQueryResult pull_mariadb_sql(const std::string& customer_id, const std::string& sql_statements);
  Result<std::string> push_mariadb_sql(const std::string& customer_id, const std::string& table_name = "tasks") const;
  Result<void> sync_with_task_ledger(const std::string& customer_id);

  std::vector<std::string> list_tables(const std::string& db = "") const;
  std::vector<std::string> list_views(const std::string& db = "") const;
  bool has_views() const { return !views_.empty(); }
  // What a table IS — its columns, their types, its rows — so a browser can
  // show a table's shape without selecting every row out of it first.
  SqlTable* find_table(const std::string& name, const std::string& db = "");
  const SqlTable* find_table(const std::string& name, const std::string& db = "") const;
  const SqlView* find_view(const std::string& name, const std::string& db = "") const;
  // A view's stored query, read under the database's own lock: for callers on
  // a thread that holds none (the replication worker). False when there is no
  // such view.
  bool view_body(const std::string& name, const std::string& db, std::string* out) const;
  // SYNC VIEWS: re-reads the source's view definitions and replaces any that
  // differ (see BinlogConsumer::sync_views).
  SqlQueryResult sync_views_statement();
  // Whether a view is small enough to build whole (what a UNION / GROUP BY /
  // DISTINCT view costs on first read): the rows of the tables it reads,
  // largest per SELECT branch, summed over branches, through nested views,
  // against AIOS_VIEW_MAX_ROWS (default 1,000,000). On st a view over
  // stockmoves' 11.7M rows grew the process ~45 MB/s towards the box's RAM,
  // on the machine that also runs the customer's MariaDB; refusing is an
  // error the caller falls back on, never an empty answer.
  bool view_within_build_budget(const SqlView* v, std::string* why) const;
  std::shared_ptr<SqlTable> get_or_build_materialized_view(const SqlView* v);
  size_t row_count(const std::string& name, const std::string& db = "") const;
  SqlTable* resolve_table(const std::string& name, const std::string& db = "");
  // True for a table the database keeps (not a CTE, view materialization or
  // information_schema image, which live no longer than a statement).
  bool is_stored_table(const SqlTable* t) const;
  const SqlTable* resolve_table(const std::string& name, const std::string& db = "") const;
  Result<void> save_tables();
  // Store format 4 (per-table files of encoded columns, under a manifest).
  // dense: the file is deflate-packed (a table that had stopped changing); else LZ4, the checkpoint's fast form.
  // still_ns: since when the table has not changed (monotonic).
  // layout: the table's layout_seq when the file was written - moved only
  // by a change to rows already stored, so while it stands and rows were
  // only appended, the checkpoint appends them to the file as a part
  // (write_checkpoint) instead of writing the table whole. base_rows: the
  // rows of the whole image; part_rows: rows appended as parts since;
  // file_bytes: where the valid file ends (the manifest records it).
  struct SavedTableFile {
    std::string file; uint64_t seq = 0; size_t rows = 0; uint64_t shape = 0; size_t bytes = 0; bool dense = false; uint64_t still_ns = 0;
    uint64_t layout = 0; size_t base_rows = 0; size_t part_rows = 0; uint64_t file_bytes = 0; uint32_t parts = 0;
    uint32_t op_parts = 0;   // of the parts, those with erased rows or set cells (kOpsPartMagic)
    size_t op_cells = 0;     // rows erased + cells set, over those parts
  };
  Result<void> save_tables_v4();
  bool load_tables_v4();
  Result<void> save_views();
  Result<void> save_tables_v3_unused();
  Result<void> load_tables();
  void optimize_table_storage(const std::string& name, const std::string& db = "");
  void optimize_all_tables(const std::string& db = "");
  void invalidate_query_cache();

  // Writes the store out now and empties the redo log. Every statement used to
  // do this, which on the ERP's 1.34 GB of tables meant rewriting all of it to
  // record one changed row. Ordinary statements now only append to the redo
  // log; this is what the interval, a shutdown, or an explicit CHECKPOINT runs.
  Result<void> flush_now();
  // A checkpoint whose files are written off the statement lock (as a due checkpoint's are): the lock is held for
  // the snapshot in memory only. False, and nothing done, while the last checkpoint's files are still being
  // written - waiting for them under the lock is the stall this avoids; the caller asks again shortly.
  bool flush_off_lock();
  Result<void> flush_locked();   // flush_now's body, for a caller already holding the exclusive statement lock
  // flush_now for a process about to exit: what changed, in the fast form - no storage upkeep and no deflating of
  // the tables gone still (the next checkpoint does those). A stop deflated the 2.2 GB audittrail: 38 s.
  Result<void> flush_for_exit();
  bool exit_flush_ = false;
  // Checkpoints off the lock (see "checkpoints off the lock" in sql_database.cpp).
  struct CheckpointImage {
    uint64_t gen = 0;
    // A changed table: its header and trailers serialized, its columns
    // copied (memcpy under the lock; serialized by the writer thread).
    struct TableImage {
      std::string key, file; std::vector<uint8_t> head, tail; std::vector<ColumnStore> cols; bool dense = false;
      // An append-only change: rows [rows_from, rows_to) as text, row-major
      // (rows * ncols cells), appended to the file as a part; cols stays empty.
      bool append = false; size_t rows_from = 0, rows_to = 0, ncols = 0; std::vector<std::string> part_cells;
      // A change the table's journal holds: its writes in order (kOpsPartMagic), already serialized; appended
      // to the file as a part the same way.
      bool ops = false; std::vector<uint8_t> ops_plain;
    };
    std::vector<TableImage> tables;
    std::vector<uint8_t> manifest;
    std::vector<std::string> databases;                                  // databases_ as of the snapshot (the manifest names them)
    std::vector<std::pair<std::string, std::string>> entries;            // key -> file
    std::unordered_map<std::string, SavedTableFile> saved;
    std::string cdc_text;        // cdc_position as of the snapshot ("" = unchanged)
    std::string prev_redo;       // the rotated log, deleted once the manifest names gen
    size_t rewritten = 0;
    size_t bytes = 0;
  };
  Result<void> flush_impl(bool async, bool maintain = true);
  Result<void> snapshot_tables_v4(CheckpointImage& img);   // under the exclusive statement lock
  Result<void> write_checkpoint(CheckpointImage& img);     // off the lock
  std::vector<uint8_t> manifest_bytes(const CheckpointImage& img) const;   // the manifest naming img's generation
  bool rotate_redo(CheckpointImage& img);
  void checkpoint_loop();
  void checkpoint_wait_idle();
  size_t replay_redo_file(const std::string& path);
  // EXPORT SNAPSHOT 'name': checkpoints, then lays a complete, self-consistent
  // copy of the store (one generation of table files, replication position,
  // Things, view definitions) under <store>/exports/<name>, hard-linked so it
  // costs no space and takes no time, and cannot be changed by the next save.
  // EXPORT SNAPSHOT 'name' [TO '/dir']: a hot, consistent copy of the store (see export_snapshot in the .cpp).
  SqlQueryResult export_snapshot(const std::string& name, const std::string& to_dir = std::string());
  // This server's own binlog position ("file:pos") when it writes one (binlog master); "" when it does not.
  std::string binlog_master_position() const;
  SqlQueryResult export_snapshot_statement(const std::string& q);
  // Re-encode columns a write had to decompress, rebuild dropped zone maps -
  // the engine tidying its own storage, run from the periodic flush.
  void maintain_storage();
  void save_index_list();
  // Things (sql_things.hpp): change notices from the write paths, and the
  // reactions they call for.
  void notify_row_change(const SqlTable& t, const SqlRow* before, const SqlRow* after, size_t row = (size_t)-1);
  void active_data_tick();
  void notify_table_reset(const SqlTable& t);
  // COMMIT: the change notices this session's transaction held back (SessionTxn::held), delivered.
  void deliver_held_notices();
  void things_settle();
  void things_tick();
  bool things_watch(const SqlTable& t) const;
  ActiveDataEngine* active_data() { return active_data_.get(); }
  const ActiveDataEngine* active_data() const { return active_data_.get(); }
  SqlRwLock& rw_lock() const { return rw_lock_; }
  void set_checkpoint_interval(size_t statements, uint64_t redo_bytes);

  // The statement that would recreate a table, for the studio's DDL pane.
  Result<std::string> table_ddl(const std::string& name, const std::string& db = "") const;
  Result<std::string> view_ddl(const std::string& name, const std::string& db = "") const;
  // Diffs the table designer's edit grid against the live table and returns
  // the ALTER TABLE statement(s) that would apply it - one per line, in the
  // order apply_table_design() actually runs them (renames/retypes first,
  // then drops, then adds, so a rename never collides with an add of the
  // same name mid-sequence).
  Result<std::string> generate_alter_table_ddl(const std::string& table_name, const std::vector<ColumnEdit>& edits) const;
  // Runs that diff for real, through the same execute_alter() path a typed
  // ALTER TABLE statement goes through - not a second, parallel
  // implementation of column add/drop that could drift from it or (the
  // defect this replaced) silently discard every existing row by swapping
  // in a bare new SqlTable with no data.
  Result<void> apply_table_design(const std::string& table_name, const std::vector<ColumnEdit>& edits);

  // --- Write-Through / Write-Behind Gateway & Write-Back Methods ---
  WriteSyncMode sync_mode() const { return sync_mode_; }
  // Selecting WriteBehind IS the explicit turn-on for the write-behind
  // feature; selecting anything else turns it back off. Defined out of line
  // because it now has to arm/disarm the WAL and the drain worker.
  void set_sync_mode(WriteSyncMode mode);

  // Write-behind is OPT-IN and OFF by default. With it off nothing in this
  // feature runs: no WAL file is created or read, no drain thread is started,
  // and the query path never takes the write-behind branch — writes go
  // straight down the ordinary durable path (redo log + store). Turn it on
  // with set_sync_mode(WriteBehind), init_write_behind(), or AIOS_WRITE_BEHIND=1.
  bool write_behind_enabled() const { return write_behind_enabled_; }
  Result<void> set_write_behind_enabled(bool on);

  void set_downstream_target(const DbConnectionProfile& prof);
  const DbConnectionProfile& downstream_target() const { return downstream_target_; }

  void set_downstream_session(std::shared_ptr<SqlWireSession> session);
  std::shared_ptr<SqlWireSession> downstream_session() const;

  // Unified Query execution (gateway aware: routes local read queries & handles write-through/write-behind)
  SqlQueryResult execute_gateway_query(const std::string& sql_query, const std::string& session_db = "");
  // One run of a gateway statement; execute_gateway_query runs it again after a GET_LOCK wait.
  SqlQueryResult execute_gateway_query_once(const std::string& sql_query, const std::string& session_db);
  // What in a statement can't safely run twice (for GET_LOCK's wait); empty when it can.
  std::string cant_repeat_reason(const std::string& sql, const std::string& session_db);
  SqlQueryResult execute_gateway_query_inner(const std::string& sql_query, const std::string& session_db);
  // Called by a wire connection's thread as it ends: a transaction the client
  // left open (it was killed, timed out or dropped) is rolled back so the
  // server-wide writer lock it holds is released, and its @variables go.
  void abandon_open_transaction();
  void reset_session();
  // The session's TEMPORARY tables, dropped (abandon_open_transaction).
  void drop_session_temp_tables();
  // A table's generated columns worked out and its CHECKs tried for the row
  // about to be inserted (*row) or row `at` of the table (row null).
  bool apply_row_rules(SqlTable* t, SqlRow* row, size_t at, std::string* err);
  void shrink_session_buffers(bool idle = false);
  void clear_checkpoint_pool();
  // The wire's entry: the answer as the result cache holds it. A cached
  // answer is shared, not copied; a computed one goes into the cache itself
  // and is shared back, where execute_gateway_query copied every cell of it
  // into the cache and handed the original over - a deep copy per miss (the
  // open-orders page's 26k distinct lookups, 160 cells each).
  std::shared_ptr<const SqlQueryResult> execute_gateway_query_shared(const std::string& sql_query,
                                                                    const std::string& session_db = "");
  // The MySQL text-protocol packets of a cached answer, exactly as they go on
  // the wire (column count through terminating EOF, sequence from 1), so a
  // cache hit is one memcpy instead of re-encoding every row. Null when the
  // statement has no cached answer or none encoded yet. `wire_db` is the
  // schema name the column definitions carry.
  // `binary` selects the COM_STMT_EXECUTE (binary row) encoding of the same
  // answer, kept beside the text one: a prepared statement's hit is then the
  // same memcpy, and each variant only ever serves its own protocol.
  std::shared_ptr<const std::string> cached_wire_packets(const std::string& sql_query, const std::string& session_db,
                                                         const std::string& wire_db, bool binary = false, bool latin1 = false);
  // Hands back the packets encoded for the answer `gen_before` saw; dropped if
  // a write landed since or the statement is not cached.
  void attach_cached_wire_packets(const std::string& sql_query, const std::string& session_db,
                                  const std::string& wire_db, std::string packets, uint64_t gen_before,
                                  bool binary = false, bool latin1 = false);
  uint64_t query_cache_generation() const { return query_cache_gen_.load(std::memory_order_acquire); }
  // Bumped by every DDL: anything derived from the set of tables or their
  // columns (a cached `*` expansion, the table-name set) is stale past it.
  uint64_t schema_generation() const { return schema_gen_.load(std::memory_order_acquire); }

  // Table lists for same-host clients. With a directory set, publish_table_lists()
  // keeps <dir>/<database>.tables (one table name per line) current: it rewrites a
  // database's file, by temp file + rename, only when its set of tables changed.
  // The CDC loop calls it after every step, so a CREATE/DROP/RENAME applied from
  // the binlog is on disk before the next statement. PHP reads the file in place
  // of asking SHOW TABLES on every request. No directory = does nothing.
  void set_table_list_dir(const std::string& dir);
  void publish_table_lists();

  // WAL management & Write-Behind operations
  Result<void> init_write_behind(const std::string& wal_path = "");
  Result<void> append_mutation_wal(const std::string& sql, const std::string& db = "");
  Result<size_t> drain_wal_once(size_t max_batch = 100);
  Result<void> flush_write_behind();
  Result<void> recover_uncommitted_wal();

  void start_wal_drain_worker(int interval_ms = 20);
  void stop_wal_drain_worker();
  bool is_wal_drain_worker_running() const { return wal_drain_running_; }

  WriteBackStats write_back_stats() const;
  std::vector<SqlMutation> list_pending_mutations() const;

  // --- Native Binlog CDC Streaming Consumer & Replication Methods ---
  Result<void> start_binlog_sync(const struct BinlogConsumerConfig& config);
  // Copy whitelisted tables out of the replication source from ONE consistent
  // snapshot (START TRANSACTION WITH CONSISTENT SNAPSHOT) and remember the
  // binlog position that snapshot corresponds to, per table, so the CDC stream
  // takes over each table exactly where its rows end. Tables already seeded
  // (or present from an import) are left alone; one interrupted mid-copy is
  // dropped and copied again. Big tables are read in primary-key order, a
  // chunk at a time. `log` gets one line per table.
  // The replication stream's position after a committed transaction, logged
  // beside that transaction's statements so replay restores both together.
  void record_cdc_commit(const std::string& file, uint64_t pos);
  // The Things, Cells and active data a replicated commit touched react at most every 100 ms, many commits' keys at
  // once; the replication loop calls this after each step so the last of them react within 100 ms of quiet.
  void cdc_settle_if_due();
  uint64_t cdc_settled_ns_ = 0;
  bool cdc_settle_owed_ = false;
  // The master's column definitions re-read for tables already here: a mirror
  // seeded by an older engine, or a master whose columns were altered since,
  // carries stale metadata (a LONGBLOB mirrored as text is converted for a
  // latin1 session where MariaDB sends bytes). Metadata only, no row touched;
  // returns how many tables changed.
  // The master this mirror follows, as the startup handed it to the seed: what
  // SHOW ASQL MIRROR compares against.
  struct MirrorSource { std::string host; uint16_t port = 3306; std::string user, password, database; std::vector<std::string> tables; };
  void set_mirror_source(const MirrorSource& s) { mirror_source_ = s; }
  const MirrorSource& mirror_source() const { return mirror_source_; }
  Result<size_t> repair_schema_from_source(const std::string& host, uint16_t port, const std::string& user,
                                           const std::string& password, const std::string& database,
                                           const std::vector<std::string>& tables, std::vector<std::string>* log);
  // Told of each seed line as it happens - including a row count every few seconds while a big table streams -
  // so a long seed shows it is alive instead of printing its whole log at the end.
  void set_seed_progress(std::function<void(const std::string&)> f) { seed_progress_ = std::move(f); }
  Result<size_t> seed_from_source(const std::string& host, uint16_t port, const std::string& user,
                                  const std::string& password, const std::string& database,
                                  const std::vector<std::string>& tables, std::vector<std::string>* log);
  // Where a starting engine is: what the wire server tells a client that connects before it answers queries.
  // init() reports "loading tables", seed_from_source() "seeding tables", each with its count; the server that
  // owns the engine sets the rest (its own steps, and ready once they are done).
  void set_startup_phase(const std::string& phase, size_t done = 0, size_t total = 0);
  void startup_progress(size_t done) { startup_done_.store(done, std::memory_order_relaxed); }
  std::string startup_status() const;
  // Replication's place in the master's binlog, kept beside the store so a
  // restart resumes rather than skipping whatever happened while it was down.
  Result<void> save_cdc_position();
  bool load_cdc_position(std::string* out_file, uint64_t* out_pos) const;
  void stop_binlog_sync();
  struct BinlogConsumerStats binlog_sync_stats() const;
  void set_replication_whitelist(const std::vector<std::string>& tables);
  void set_replication_column_whitelist(const std::string& table, const std::vector<std::string>& columns);
  void set_replication_max_rows(const std::string& table, size_t max_rows);
  class BinlogConsumer* binlog_consumer();

  // --- transactions ---------------------------------------------------------
  // A snapshot restored by ROLLBACK. This is what makes a wrong UPDATE
  // survivable, which on an engine people run ad-hoc statements against
  // matters more than speed.
  //
  // Per session, not per engine. One global flag meant a second connection's
  // BEGIN was refused because the first still held one, and a ROLLBACK undid
  // whatever every other session had written in the meantime. A session's
  // transaction now serializes against other writers for its duration and
  // copies only the tables it actually touches, rather than all of them.
  bool in_transaction() const;

 private:
  friend class ThingEngine;
  friend class CellEngine;
  friend class ActiveDataEngine;
  friend class SqlRowExpr;
  friend SqlQueryResult sql_run_sat(SqlDatabase& db, const std::string& sql, const std::string& session_db);
  friend SqlQueryResult sql_run_result_view(SqlDatabase& db, const std::string& sql, const std::string& session_db);
  std::shared_ptr<ResultViewStore> result_views_;   // CALL VIEW (sql_result_views.hpp)
  std::unique_ptr<ThingEngine> things_;
  std::unique_ptr<CellEngine> cells_;   // CREATE CELLS (sql_cells.hpp)
  std::unique_ptr<ActiveDataEngine> active_data_;
  Kernel& kernel_;
  TaskLedger* ledger_;
  std::string root_path_;
  MirrorSource mirror_source_;
  mutable std::mutex startup_mu_;
  std::string startup_phase_;
  std::atomic<size_t> startup_done_{0};
  std::atomic<size_t> startup_total_{0};
  std::unordered_map<std::string, SqlTable> tables_;
  std::unordered_map<std::string, SqlView> views_;
  // Stored programs (PROCEDURE, FUNCTION, TRIGGER, EVENT), each kept as the
  // CREATE that made it in <root>/programs.db and parsed again at open; the
  // interpreter is sql_programs.inc. Keyed "<P|F|T|E>:db.name", lower-cased.
 public:
  struct StoredProgram;
  std::shared_ptr<const StoredProgram> find_program(char kind, const std::string& name) const;
  bool has_stored_functions() const { return has_functions_.load(std::memory_order_acquire); }

  // Whether a capability may run this statement, decided from its text before
  // anything executes. Empty means yes; otherwise the reason it may not. It
  // reads the statement with the executor's own tokenizer and fails closed:
  // a verb it does not know, a table it cannot place, a second statement.
  std::string authorize_statement(const std::string& sql, const AsqlAuthority& authority,
                                  const std::string& session_db) const;
 private:
  mutable aios::hal::Mutex programs_mu_;
  std::map<std::string, std::shared_ptr<const StoredProgram>> programs_;
  std::atomic<bool> has_functions_{false}, has_triggers_{false}, has_events_{false};
  std::atomic<bool> event_scheduler_{false};   // SET GLOBAL event_scheduler; off, as MariaDB ships
  SqlQueryResult execute_program_ddl(const std::string& sql);
  SqlQueryResult show_create_program(char kind, const std::string& name);
  std::vector<std::shared_ptr<const StoredProgram>> list_programs(char kind) const;
  std::vector<std::shared_ptr<const StoredProgram>> table_triggers(const SqlTable& t, const char* event) const;
  void load_programs();
  void save_programs();
  void refresh_program_flags();
  void run_due_events();
  mutable std::unordered_map<std::string, SqlTable> view_table_cache_;
  mutable std::unordered_map<std::string, std::shared_ptr<SqlTable>> materialized_view_cache_;
  // What a cached materialized view depends on and how it stays right: the
  // tables (and views) its text names, whether it reads the clock, and - for a
  // grouped view of the shape that allows it - the plan for refreshing only
  // the groups whose input rows changed (type-erased: the plan's types live in
  // the .cpp). Guarded by materialized_view_mu_, like the cache itself.
  struct MatViewMeta {
    std::unordered_set<std::string> deps;    // lower-cased names
    bool day_dependent = false;              // reads the date: stale once the day changes
    bool fine_dependent = false;             // reads the time of day: valid for a minute
    int64_t built_day = 0;
    uint64_t built_ms = 0;
    std::shared_ptr<void> plan;              // MatViewPlan, or null: rebuilt whole when a dependency changes
  };
  mutable std::unordered_map<std::string, MatViewMeta> matview_meta_;
  // Cached view tables that were replaced or dropped. resolve_table hands out
  // a raw pointer into the cached table and the statement goes on using it, so
  // a table may only be freed when no statement can be reading - when a writer
  // holds the database exclusively (execute_local_query reaps them there).
  // Guarded by materialized_view_mu_.
  mutable std::vector<std::shared_ptr<SqlTable>> matview_retired_;
  // The cached table if it is still right; null when it has to be rebuilt
  // (the entry dropped), or - *refresh set - when only the groups whose input
  // changed need refreshing: matview_refresh does that, without the lock.
  // Caller holds materialized_view_mu_. Runs no query: a query under that
  // mutex takes table locks while writers holding a table lock wait for it.
  std::shared_ptr<SqlTable> matview_current(const std::string& key, const SqlView* v, std::shared_ptr<void>* refresh);
  // The refresh matview_current asked for, run without materialized_view_mu_
  // and published only if the entry it refreshed is still the cached one.
  // Null when it could not be done (the entry dropped: rebuilt whole).
  std::shared_ptr<SqlTable> matview_refresh(const std::string& key, const std::shared_ptr<void>& refresh);
  // Recursive: building mixedinventorypivot resolves mixedinventory inside
  // the same call, on the same thread, which re-enters this lock.
  mutable aios::hal::RecursiveMutex materialized_view_mu_;
  // Serializes materialized view builds. Never waited on for long and never
  // held with materialized_view_mu_: a build takes table locks, and a write
  // holding a table's lock takes materialized_view_mu_ (see
  // get_or_build_materialized_view).
  mutable aios::hal::RecursiveMutex matview_build_mu_;
  MariaDbConfig mariadb_config_;
  std::vector<DbConnectionProfile> profiles_;
  std::string active_profile_id_ = "local_embedded";
  std::string active_database_name_ = "aios_db";
  std::string home_database_ = "aios_db";   // the database a PostgreSQL / MS SQL downstream's profile stands for
  std::string downstream_database_for(const std::string& db) const;
  std::string active_host_ = "127.0.0.1";
  uint16_t active_port_ = 3306;
  bool bulk_load_ = false;
  std::function<void(const std::string&)> seed_progress_;
  bool seeding_ = false;                 // seed_from_source: rows go straight to the store, not the redo log
  uint64_t store_generation_ = 0;        // the manifest generation last saved or loaded
  uint64_t checkpoint_seq_ = 0;          // the highest generation snapshotted or seen as a rotated log
  aios::hal::Thread checkpoint_thread_;  // writes a snapshot's files while statements run
  aios::hal::Mutex checkpoint_mu_;
  aios::hal::CondVar checkpoint_cv_;
  bool checkpoint_stop_ = false;
  std::unique_ptr<CheckpointImage> checkpoint_pending_;
  // The column copies of the last checkpoint, per table, kept for the next
  // snapshot to copy into: a fresh 90 MB costs 2.5 us a page in first-touch
  // faults on a microVM, most of the time the snapshot held the lock.
  std::unordered_map<std::string, std::vector<ColumnStore>> checkpoint_pool_;
  size_t checkpoint_pool_bytes_ = 0;
  std::vector<uint8_t> checkpoint_wbuf_;   // the writer's serialization buffer, capacity kept
  static size_t checkpoint_pool_cap();     // AIOS_CHECKPOINT_POOL_MB, default 512
  std::atomic<bool> checkpoint_writing_{false};
  // EXPORT SNAPSHOT in progress: no checkpoint starts (its cleanup would remove the generation being copied).
  std::atomic<int> snapshot_pins_{0};
  // Table files that would not load at startup, each moved to tables-damaged/ (see load_tables_v4).
  std::vector<std::string> damaged_tables_;
  // What each table's current v4 file holds, so a checkpoint skips the ones
  // nothing has written to.
  std::unordered_map<std::string, SavedTableFile> saved_tables_;
  // Tables whose file on disk predates the packed format (store_pack): each
  // checkpoint rewrites a few of them (maintain_storage), so an upgraded
  // store converts without one checkpoint copying all of it.
  std::unordered_set<std::string> unpacked_tables_;
  mutable SqlRwLock rw_lock_;
  // Rebuilds, after a restart, the column row indexes the last run was using
  // (sql_db/indexes.list), so the first lookups do not pay for them. Joined
  // by the destructor.
  aios::hal::Thread index_warm_;
  // Builds the indexes request_background_probe asks for. Joined by the
  // destructor.
  aios::hal::Thread bg_probe_thread_;
  aios::hal::Mutex bg_probe_mu_;
  aios::hal::CondVar bg_probe_cv_;
  bool bg_probe_stop_ = false;
  std::deque<std::pair<const SqlTable*, int>> bg_probe_queue_;
  std::map<std::pair<const SqlTable*, int>, uint64_t> bg_probe_last_ns_;   // last request, to rate-limit rebuilds
  // The Things' clock: wakes the ones with a time (an event due, a new day)
  // when no write or binlog tick would (sql_things.hpp, due_now).
  aios::hal::Thread things_clock_;
  std::atomic<bool> things_clock_stop_{false};
  aios::hal::Mutex things_clock_mu_;
  aios::hal::CondVar things_clock_cv_;
  std::string last_index_list_;
  std::string last_probe_list_;   // probes.list as last written: the join hash indexes to warm at boot
  std::string last_rowindex_list_;   // rowindexes.list as last written: the tables whose primary-key row map is built at boot

  // Schemas that exist, so an empty one still shows in SHOW DATABASES and a
  // dropped one stops showing. Before this a database was only ever inferred
  // from the tables in it, which is why DROP DATABASE could not remove one.
  std::set<std::string> databases_;

  // Redo log: statements are appended here, fdatasync'd, before they are
  // applied, and replayed over the last full store on startup.
  std::string redo_path_;
  // The log is written at this offset into space set aside ahead
  // (Host::preallocate, the log's maximum size): a commit's fdatasync then
  // flushes data only. An append grew the file by every record, and each
  // fdatasync committed the size change through the filesystem's journal
  // as well as the data - two flushes for one commit.
  uint64_t redo_write_off_ = 0;
  uint64_t redo_logical_end_ = 0;   // set by replay_redo_file: the end of the last whole record it read
  Result<void> redo_write(ByteSpan b, bool synced, uint64_t* seq = nullptr);
  // Records are appended to redo_buf_ (under redo_mu_, numbered in order)
  // and written into the file by redo_drain - by the commit that syncs, after
  // every lock: the write system call inside a table's write hold was a
  // seventh of a single-row UPDATE's hold. redo_drain_mu_ keeps the drains
  // (and a rotation of the file) in order; it is taken before redo_mu_.
  std::string redo_buf_, redo_buf_spare_;
  aios::hal::Mutex redo_drain_mu_;
  uint64_t redo_drain();   // the number of the last record now in the file
  uint64_t redo_drain_locked();   // redo_drain_mu_ held
  bool redo_write_direct();       // redo_drain_mu_ held
  bool redo_direct_ok_ = true;
  std::string redo_tail_;         // the log's bytes from its last block's start to its end
  std::vector<uint8_t> redo_dbuf_;
  void redo_prepare_file();   // a fresh, preallocated log at offset 0 (the spare, when one is ready)
  std::string redo_spare_path() const;
  void prepare_redo_spare();
  std::atomic<bool> redo_spare_busy_{false};
  aios::hal::Thread redo_spare_thread_;
  std::atomic<uint64_t> redo_bytes_{0};   // written by every committing session
  std::atomic<size_t> redo_records_{0};
  bool redo_replaying_ = false;
  // A checkpoint rewrites every changed table's file under the writer lock;
  // 2,000 statements made that a stall every half second of OLTP writes.
  size_t checkpoint_every_ = 20000;
  uint64_t redo_max_bytes_ = 64ull << 20;
  void append_redo(const std::string& sql, const std::string& db);
  void append_redo_batch(const std::vector<std::pair<std::string, std::string>>& recs);   // (db, sql) pairs, one append
  void append_redo_bytes(const std::vector<uint8_t>& b, size_t records);
  // Redo records held back while a replicated transaction (or a replay batch)
  // is being applied, written with one fdatasync at its commit.
  std::string redo_pending_;
  // Group commit for the redo log. A statement appends its record without a
  // sync and takes a sequence number; one thread syncs the file and raises
  // redo_synced_seq_ to the last record it covered; the statement, its lock
  // released, waits for its number. Under concurrent writers one sync covers
  // all of them, where one sync per statement held the writer lock for the
  // disk's whole latency each time.
  // The waiters touch no lock: they read the atomics and park on
  // redo_sync_epoch_, which every finished sync raises (a mutex and a
  // condition variable here were a third of a write session's futex calls,
  // every waiter woken by a sync queueing on the mutex again to look).
  // redo_mu_ orders the writes into the file and the lazy sync thread.
  aios::hal::Mutex redo_mu_;
  aios::hal::CondVar redo_cv_;
  std::atomic<uint64_t> redo_written_seq_{0};
  std::atomic<uint64_t> redo_synced_seq_{0};
  std::atomic<bool> redo_sync_stop_{false};
  std::atomic<bool> redo_sync_failed_{false};
  std::atomic<bool> redo_syncing_{false};   // a sync of the file is under way (by a waiter or the sync thread)
  std::atomic<uint32_t> redo_sync_epoch_{0};
  // A waiting commit sleeps on the slot of its record's number, and a sync
  // wakes only the slots it covered, plus those of the records still waiting
  // (one of them syncs next). Waking every waiter on every sync, most to find
  // their record not covered yet, was five futex calls a commit at 32
  // sessions on one table.
  static constexpr size_t kRedoWaitSlots = 64;
  std::array<std::atomic<uint32_t>, kRedoWaitSlots> redo_wait_slots_{};
  std::array<std::atomic<uint32_t>, kRedoWaitSlots> redo_wait_count_{};   // threads parked on each slot
  void redo_wake(uint64_t from, uint64_t to);
  std::atomic<uint64_t> redo_wait_max_{0};   // the highest record number a commit has slept for
  std::atomic<int32_t> redo_waiting_{0};      // commits asleep for a sync   // the slots of records (from, to]
  aios::hal::Thread redo_syncer_;
  void redo_sync_loop();
  void redo_wait_durable();
  void redo_mark_all_synced();
 public:
  void flush_redo_pending();
  uint64_t cdc_synced_ns_ = 0;   // when a replicated commit last put the redo log on the medium (record_cdc_commit)
 private:
  Result<void> replay_redo();
  Result<void> truncate_redo();

  // Held from BEGIN to COMMIT/ROLLBACK, so one session's rollback can never
  // discard another's committed rows.
  hal::Mutex writer_mu_;
  std::atomic<int> open_txns_{0};   // BEGIN..COMMIT/ROLLBACK in flight, on any session
  // Takes a copy of `key`'s table for the calling session's rollback, once.
  void txn_capture(const std::string& key);
  void restore_captured(std::unordered_map<std::string, SqlTable>& snapshot,
                        const std::unordered_map<std::string, size_t>& appended, const std::set<std::string>& created);
  void frames_roll_back(size_t from);
  // Takes back the row changes logged since `mark` (newest first) and drops them from the log.
  void undo_row_changes(size_t mark);
  void cut_back_appended(SqlTable& tb, size_t keep);   // a failed INSERT's rows off again

  // ---- foreign keys (sql_database.cpp, "foreign keys") ----------------------
 public:
  // One constraint: the child's columns and the parent's, both by name.
  struct FkDef {
    std::string name;
    std::string child_key, parent_key;          // tables_ keys
    std::vector<std::string> ccols, pcols;
    std::string on_delete, on_update;
  };
  struct FkGraph {
    uint64_t epoch = ~0ull;
    std::unordered_map<std::string, std::vector<FkDef>> outgoing;   // child table -> its constraints
    std::unordered_map<std::string, std::vector<FkDef>> incoming;   // parent table -> constraints naming it
  };
  // A statement to run after the parent row changed: CASCADE / SET NULL.
  struct FkAction { std::string sql, db; };
 public:
  void bump_schema_epoch() { schema_epoch_.fetch_add(1, std::memory_order_acq_rel); }
  bool fk_any();                                    // any table declares a foreign key
 private:
  std::atomic<uint64_t> schema_epoch_{0};
  aios::hal::Mutex fk_mu_;
  std::shared_ptr<const FkGraph> fk_graph_;
  std::atomic<int> fk_any_cache_{-1};               // 0 none, 1 some - for fk_any_epoch_
  std::atomic<uint64_t> fk_any_epoch_{~0ull};
  std::shared_ptr<const FkGraph> fk_graph();
  bool fk_checks_on() const;                        // FOREIGN_KEY_CHECKS=1 and a client's statement
  bool fk_actions_on() const;                       // ... or the replication stream (MariaDB does not binlog cascades)
  bool fk_touches(const SqlTable& t);               // t is a child or a parent of some constraint
  std::string fk_child_error(const SqlTable& child, const std::vector<std::string>& row, const std::vector<bool>* only_cols = nullptr);
  std::string fk_parent_change(const SqlTable& parent, const std::vector<std::string>& old_row,
                               const std::vector<std::string>* new_row, std::vector<FkAction>* actions);
  std::string fk_run_actions(std::vector<FkAction>& actions);
  // Concurrent transactions (AIOS_TXN_CONCURRENT=1): a statement run, taken back and run again around another
  // transaction's rows; and the rows a SELECT ... FOR UPDATE claims.
  SqlQueryResult execute_gateway_concurrent(const std::string& sql_query, const std::string& session_db, int begins = -1);
  uint64_t claim_locked_rows(const std::string& sql, const std::string& session_db, std::string* run_sql);
  void frames_release(size_t from);
  SqlQueryResult execute_savepoint_statement(const std::string& sql);
  // A statement with triggers runs in one: undone if it fails, kept if done().
  class StatementUndo {
   public:
    StatementUndo(SqlDatabase& db, const SqlTable& t);
    ~StatementUndo();
    void done() { done_ = true; }
   private:
    SqlDatabase& db_;
    size_t at_ = 0;
    bool done_ = false;
  };

  // Write-Through / Write-Behind Gateway state
  WriteSyncMode sync_mode_ = WriteSyncMode::LocalOnly;
  DbConnectionProfile downstream_target_;
  // A default-constructed profile already carries 127.0.0.1:3306, so "has a
  // host" was true before anyone set one and every query took the gateway
  // path. Whether a downstream exists is its own answer, not one inferred
  // from a field that is never empty.
  bool downstream_configured_ = false;
  mutable std::shared_ptr<SqlWireSession> downstream_session_;
  std::string wal_path_;
  // The single gate for the whole write-behind feature. Off, the WAL is never
  // opened, appended to, recovered or drained.
  std::atomic<bool> write_behind_enabled_{false};
  uint64_t next_mutation_seq_ = 1;
  uint64_t last_acked_seq_ = 0;
  std::vector<SqlMutation> pending_mutations_;
  mutable hal::Mutex wal_mu_;
  std::atomic<bool> wal_drain_running_{false};
  std::unique_ptr<hal::Thread> wal_drain_thread_;
  int wal_drain_interval_ms_ = 20;

  mutable std::atomic<uint64_t> total_mutations_count_{0};
  mutable std::atomic<uint64_t> acked_mutations_count_{0};
  mutable std::atomic<uint64_t> failed_mutations_count_{0};
  mutable std::atomic<uint64_t> last_sync_timestamp_ns_{0};
  mutable std::string last_sync_error_;
  mutable double total_drain_time_us_ = 0.0;
  mutable uint64_t total_drain_batches_ = 0;

  // Native Binlog CDC Streaming Consumer
  std::unique_ptr<class BinlogConsumer> binlog_consumer_;
  // This server's own binlog, when it is a binlog master (enable_binlog).
  std::unique_ptr<class AsqlBinlog> binlog_writer_;
  void binlog_records(const std::vector<std::pair<std::string, std::string>>& recs);

 public:
  // Writes every committed statement into <dir>/asql-bin.NNNNNN as well, for
  // MySQL and MariaDB replicas to read over COM_BINLOG_DUMP.
  Result<void> enable_binlog(const std::string& dir = std::string());   // default <store>/binlog
  std::string binlog_default_dir() const { return root_path_ + "/binlog"; }
  class AsqlBinlog* binlog_writer() const { return binlog_writer_.get(); }
  // mysql.user's rows: what the wire server knows of its accounts, each
  // {user, host, mysql_native_password hash, rights ("read,write,...")}.
  using UserRows = std::vector<std::array<std::string, 4>>;
  void set_user_lister(std::function<UserRows()> f) { user_lister_ = std::move(f); }
  std::function<UserRows()> user_lister_;
  SqlTable build_mysql_user_table() const;
  // The id this server writes its binlog under and registers with a source as.
  uint32_t own_server_id() const;

 public:
  // INSERT used to answer two questions by walking every existing row: "what
  // is the next auto_increment" and "does this primary key already exist".
  // Two O(N) scans per row is O(N^2) per import — a 20k-row ERP dump spent 28
  // seconds there, and a 550k-row one would never finish. This caches both
  // answers for one table at a time. It is rebuilt whenever the row count
  // moves underneath it and dropped outright by any statement that can
  // rewrite a row in place.
  struct RowIndex {
    std::string table;
    std::vector<int> pk_columns;
    int ai_column = -1;
    long long auto_max = 0;
    size_t rows_indexed = 0;
    // Rows auto_max has been taken over when a big table's insert computed it
    // from the auto-increment column alone, without the key map.
    size_t ai_rows_seen = 0;
    uint64_t last_access_ns = 0;
    // Key -> the row's id in the index's own numbering: the table's row
    // number when the entry was made, which later deletes do not shift.
    // `gaps` holds, sorted, the ids of rows deleted since; a lookup takes
    // the gaps before an id off it (row_of), a new entry adds them back
    // (virt_of). A delete used to walk every entry to shift the rows after
    // it - 2 ms per deleted row over 100k keys - and dropped the whole index
    // past 200k keys. The gaps are folded into the ids (one walk) once there
    // are many.
    PkMap pk_to_row;
    std::vector<uint32_t> gaps;
    size_t row_of(size_t id) const {
      if (gaps.empty()) return id;
      return id - (size_t)(std::lower_bound(gaps.begin(), gaps.end(), (uint32_t)id) - gaps.begin());
    }
    // The id a row takes in the index's numbering: the row plus the gaps
    // at or below it, each round adding the gaps the last round's answer
    // reached (a binary search per round; walking the gaps one by one was
    // up to 4,096 steps per INSERT on a table under steady deletes).
    // The gaps are distinct and sorted, so gaps[i] - i never falls: the
    // row's id is row + i for the first i with gaps[i] - i > row (every gap
    // before it lies below that id, gaps[i] above it) - one binary search.
    // A fixed-point loop of searches here was 9% of a write session.
    size_t virt_of(size_t row) const {
      size_t lo = 0, hi = gaps.size();
      while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if ((size_t)gaps[mid] - mid > row) hi = mid; else lo = mid + 1;
      }
      return row + lo;
    }

    // One entry per row of the table, and it was counted nowhere: a single
    // primary-key SELECT on pickingfreeze (143,981 rows) moved RSS 23 MB while
    // Asql_memory_bytes did not move one byte.
    size_t estimate_memory_bytes() const {
      size_t sz = sizeof(*this) + table.capacity() + pk_columns.capacity() * sizeof(int);
      sz += pk_to_row.memory_bytes();
      return sz;
    }
  };

  struct RecentQueryLog {
    uint64_t timestamp_ns = 0;
    std::string sql;
    uint64_t duration_us = 0;
    uint64_t rows_affected = 0;
    bool success = true;
    std::string route = "POINT_LOOKUP";
    uint32_t zone_pruned_blocks = 0;
    uint32_t bloom_pruned_blocks = 0;
    std::string table_name;
    std::string error_message;
    // Which door the statement came in by. Without this the log is one
    // undifferentiated stream, and a monitor polling over the wire drowns out
    // the application traffic it was opened to watch.
    std::string client = "unknown";
    std::string database;
    // Stable across evictions, so a monitor can ask for only what is new
    // (SHOW ASQL QUERIES SINCE n) instead of re-reading the whole window.
    uint64_t seq = 0;
  };

  struct SessionInfo {
    uint64_t id = 0;
    std::string user = "root";
    std::string host = "127.0.0.1";
    std::string db = "aios_db";
    std::string command = "Sleep";
    uint64_t start_time_ns = 0;
    uint64_t query_start_ns = 0;
    std::string state = "idle";
    std::string current_query;
  };

  static void register_active_session(uint64_t id, const std::string& user, const std::string& host,
                                      const std::string& db, const std::string& command = "Sleep",
                                      const std::string& state = "idle", const std::string& info = "");
  static void update_active_session(uint64_t id, const std::string& command, const std::string& state,
                                    const std::string& info, const std::string& db = "");
  static void unregister_active_session(uint64_t id);
  // KILL: what ends the session's connection (closes its socket), and the
  // kill itself - QUERY stops the running statement, CONNECTION also ends the
  // session. False: no such session.
  static void set_session_closer(uint64_t id, std::function<void()> closer);
  static bool kill_session(uint64_t id, bool query_only);
  // The wire's hang-up watcher stopped a statement (SHOW STATUS Asql_hangup_kills).
  static void note_hangup_kill();
  static std::vector<SessionInfo> active_sessions();
  static void set_current_session_id(uint64_t id);
  static uint64_t current_session_id();

  // Set by each entry point on its own serving thread before it hands a
  // statement to the engine; read back when that statement is recorded.
  static void set_client_tag(const std::string& tag);
  // The wire session's charset, for @@character_set_* and SHOW VARIABLES: what the
  // client asked for (handshake collation, SET NAMES), per connection thread.
  static void set_session_latin1(bool in, bool out);
  // The session's compressed protocol, for SHOW STATUS: "" (none), "zlib" or "zstd", and its level.
  static void set_session_compression(const char* algo, int level);
  static bool session_latin1_in();
  static bool session_latin1_out();
  static const std::string& client_tag();

  enum class QueryLogFilter { All, ErrorsOnly, SlowOnly };

  // Shared by "SHOW ASQL QUERIES", "SHOW ASQL ERRORS", "SHOW ASQL SLOW", etc.
  SqlQueryResult recent_query_log_result(QueryLogFilter filter = QueryLogFilter::All,
                                         uint64_t since_seq = 0, size_t limit = 0) const;

  struct EngineMetrics {
    std::atomic<uint64_t> total_queries{0};
    std::atomic<uint64_t> total_selects{0};
    std::atomic<uint64_t> total_inserts{0};
    std::atomic<uint64_t> total_updates{0};
    std::atomic<uint64_t> total_deletes{0};
    std::atomic<uint64_t> total_slow_queries{0};
    std::atomic<uint64_t> total_errors{0};
    std::atomic<uint64_t> total_query_time_us{0};
    std::atomic<uint64_t> last_query_time_us{0};
    std::atomic<uint64_t> cache_hits{0};
    std::atomic<uint64_t> insert_key_via_map{0};     // big-table inserts whose key check used the row map
    std::atomic<uint64_t> insert_key_via_index{0};   // ...and the column index plus tail map
    std::atomic<uint64_t> cache_misses{0};
    std::atomic<uint64_t> zone_map_pruned_blocks{0};
    std::atomic<uint64_t> bloom_filter_pruned_blocks{0};
    std::atomic<uint64_t> parallel_scans{0};
    std::atomic<uint64_t> radix_joins{0};
    std::atomic<uint64_t> bytes_received{0};
    std::atomic<uint64_t> bytes_sent{0};
    std::atomic<uint64_t> bytes_sent_compressed{0};       // what compressed sessions' frames put on the wire
    std::atomic<uint64_t> bytes_received_compressed{0};
    std::atomic<uint64_t> total_connections{0};
    std::atomic<uint64_t> max_used_connections{0};
    std::atomic<uint64_t> current_connections{0};
    std::atomic<uint64_t> rejected_connections{0};
    std::atomic<uint64_t> query_memory_waits{0};      // statements that waited for query memory
    std::atomic<uint64_t> row_index_evictions{0};     // primary-key row indices dropped to fit the budget
    std::atomic<uint64_t> row_index_rows_indexed{0};  // rows walked into a primary-key row index (a rebuild walks the whole table)
    std::atomic<uint64_t> query_memory_wait_us{0};
    uint64_t start_time_ns = 0;
  };

  // Query memory admission. A statement a client sends reserves the memory
  // of each large intermediate it builds (row sets, join outputs and build
  // sides, group tables, projected rows, its result, the views and derived
  // tables it materializes) just before building it. A read whose
  // reservation would take all running statements past the budget waits -
  // except the statement holding a reservation longest, which always goes
  // on, so one of them always finishes - and one larger than the whole
  // budget runs once it is alone. A point lookup reserves nothing and never
  // waits; writes, statements in a transaction and the engine's own
  // (replication, Things) never wait. AIOS_QUERY_MEMORY_MB (default 2048;
  // 0 = no limit), SET GLOBAL asql_query_memory_mb. Eight dashboards' reports
  // sent at once used to build their joins and groups side by side.
  // Connections past max_connections (default 151, AIOS_MAX_CONNECTIONS) get
  // error 1040.
  static size_t default_query_memory_budget();
  void set_query_memory_budget(size_t bytes);
  size_t query_memory_budget() const { return qmem_budget_.load(std::memory_order_relaxed); }
  size_t query_memory_reserved() const { return qmem_reserved_.load(std::memory_order_relaxed); }
  size_t query_memory_peak() const { return qmem_peak_.load(std::memory_order_relaxed); }
  size_t query_memory_waiting() const { return qmem_waiting_.load(std::memory_order_relaxed); }
  // The ledger, for the statement tickets in sql_database.cpp: a ticket's
  // place in line (older first), a reservation for it (false: KILLed while
  // waiting), and giving back everything it holds.
  uint64_t qmem_next_order();
  bool qmem_reserve(uint64_t order, size_t* held, size_t bytes, bool may_wait);
  void qmem_give_back(uint64_t order, size_t* held, size_t bytes);   // part of a ticket's reservation, freed early
  void qmem_release(uint64_t order, size_t held);
  static size_t default_max_connections();
  void set_max_connections(size_t limit);
  size_t max_connections() const { return max_connections_.load(std::memory_order_relaxed); }

  using ProbeIndex = SqlProbeIndex;

  const EngineMetrics& metrics() const { return metrics_; }
  EngineMetrics& metrics() { return metrics_; }
  std::vector<RecentQueryLog> recent_queries() const;
  std::string engine_status_report() const;
  // Row totals are live; the byte estimates walk every entry of every index
  // (O(total rows): ~200 ms over chi's 848-table versioneight mirror), so they
  // are recomputed at most every kFootprintTtlNs and shared by SHOW STATUS and
  // SHOW ENGINE ASQL STATUS -- both of which monitors poll every second.
  struct MemoryFootprint {
    size_t rows = 0, delta_rows = 0;
    size_t est_mem = 0, probe_mem = 0, row_index_mem = 0;
    uint64_t bytes_age_ms = 0;
    std::unordered_map<std::string, size_t> table_bytes;   // "db.table" -> estimate
  };
  // `cached_only`: the last snapshot, however old, without walking the tables (SHOW STATUS while the statement
  // lock is held by someone else: the walk takes per-table locks that need it).
  MemoryFootprint memory_footprint(bool cached_only = false) const;
  // The probe-index cache is bounded by TOTAL BYTES across every table, not by
  // a count per table: counts cannot distinguish one index over four million
  // rows from eight over a lookup table.
  static size_t probe_index_budget_bytes();   // AIOS_SQL_PROBE_MB, default 2048
  static constexpr size_t kMaxProbeIndicesPerTable = 8;
  // The primary-key row index is the same kind of cache and needs the same
  // kind of ceiling. It costs 110-170 bytes per row of whatever table it
  // covers, so an unbudgeted one is worth ~0.5 GB across this ERP's 4.07M rows.
  // 1 GB: at 128 MB one table's index (prices: 878k rows, a 7-column key, ~130 MB) did not fit beside another's, and a
  // replicated stream updating both rebuilt one of them for every statement (chi's mirror applied ~2 KB of binlog a
  // second).
  static size_t row_index_budget_bytes();   // AIOS_SQL_ROWINDEX_MB, default 1024
  // A row index used (inserted into, looked up) this recently is not evicted for the budget: the next statement
  // on that table would rebuild it whole, and with more hot tables than the budget holds, LRU eviction made every
  // insert of a replicated stream do exactly that (stockmoves 20 s, stockmovestaxes 11 s per order). The budget
  // is then overrun, and said so, rather than enforced at that price. AIOS_SQL_ROWINDEX_HOT_S, default 600.
  static uint64_t row_index_hot_ns();
  void prune_all_probe_indices(size_t max_total_bytes = probe_index_budget_bytes(),
                               const SqlTable* exempt_table = nullptr, int exempt_col = -1);
  // Running total of probe-index bytes. Recomputing it meant walking every
  // hash bucket of every index on EVERY query that probes -- unsynchronised,
  // which is what four threads were doing inside the budget sweep when
  // chi-il-1 aborted on 2026-09-18 (std::length_error / std::bad_alloc).
  std::atomic<size_t> probe_mem_cached_{0};
  void flush_all_probe_indices();
  void record_query_stat(const std::string& sql, uint64_t duration_us, uint64_t rows, bool success,
                         const std::string& err_msg, const std::string& route = "",
                         uint32_t zone_pruned = 0, uint32_t bloom_pruned = 0,
                         const std::string& table_name = "");

 private:
  EngineMetrics metrics_;
  std::atomic<size_t> qmem_budget_{default_query_memory_budget()};
  std::atomic<size_t> qmem_reserved_{0};   // the ledger's total, readable without its lock
  std::atomic<size_t> qmem_peak_{0};
  std::atomic<size_t> qmem_waiting_{0};
  mutable aios::hal::Mutex qmem_mu_;
  mutable aios::hal::CondVar qmem_cv_;
  size_t qmem_total_ = 0;
  uint64_t qmem_next_ = 0;
  std::map<uint64_t, size_t> qmem_holders_;   // ticket order -> bytes it holds
  std::set<uint64_t> qmem_waiters_;
  std::atomic<size_t> max_connections_{default_max_connections()};
  mutable hal::Mutex recent_queries_mu_;
  // A deque: the window drops its oldest entry on every statement, and a
  // vector shifted all 1024 entries (strings and all) each time - over half a
  // one-row lookup's engine time.
  std::deque<RecentQueryLog> recent_queries_;
  // Errors and bounces, kept apart: at ~95 statements a second the shared
  // 1024-entry window turned over in ~11 s, so a failure was gone before
  // anyone looking for it could poll.
  std::deque<RecentQueryLog> recent_problems_;
  uint64_t recent_query_seq_ = 0;
  mutable aios::hal::Mutex footprint_mu_;
  mutable MemoryFootprint footprint_bytes_;
  mutable uint64_t footprint_at_ns_ = 0;

  // Shared owners for the same reason the probe indices are: eviction under
  // the byte budget, or a FLUSH, can drop one while a statement is reading it.
  std::unordered_map<std::string, std::shared_ptr<RowIndex>> table_row_indices_;
  // Guards the STRUCTURE of table_row_indices_ (insert / erase / iterate).
  // Two SELECTs on different connections reach it at the same time.
  mutable aios::hal::Mutex row_index_mu_;
  // Null when the column cannot usefully be indexed; never a wrong answer.
  std::shared_ptr<ProbeIndex> get_or_build_probe_index(const SqlTable& t, int column);
  // max_rows > 0 indexes at most that many more rows and returns; the index
  // is ready once a later call reaches the table's end.
  std::shared_ptr<ProbeIndex> get_or_build_probe_index(const SqlTable& t, int column, size_t max_rows);
  // A key lookup on a table too big to index inline asks for its index here;
  // one worker builds it a slice at a time off the query path.
  void request_background_probe(const SqlTable& t, int column);
  void background_probe_loop();
  void ensure_sorted_probe_index(ProbeIndex* idx, const SqlTable& t);

  // Caller holds row_index_mu_.
  size_t row_indices_memory_bytes_locked() const {
    size_t mem = 0;
    for (const auto& kv : table_row_indices_) {
      if (kv.second) mem += kv.second->estimate_memory_bytes();
    }
    return mem;
  }
  size_t row_indices_memory_bytes() const {
    std::lock_guard<aios::hal::Mutex> lk(row_index_mu_);
    return row_indices_memory_bytes_locked();
  }
  // Evicts least-recently-used row indices until the total fits the budget.
  // `exempt` is the one the caller is about to hand back.
  // Caller holds row_index_mu_.
  void prune_all_row_indices_locked(size_t max_total_bytes = row_index_budget_bytes(),
                                    const std::string& exempt_key = std::string());
  // Returns the index only if one already exists for this table -- the read
  // path must never be what creates it.
  void grow_row_index(const SqlTable& t);
  std::shared_ptr<RowIndex> find_row_index(const SqlTable& t);
  void invalidate_row_index() {
    {
      std::lock_guard<aios::hal::Mutex> lk(row_index_mu_);
      table_row_indices_.clear();
    }
    flush_all_probe_indices();
    invalidate_query_cache();
  }
  void invalidate_table_index(const std::string& name, const std::string& db = "");
  void invalidate_table_caches(SqlTable& t);
  // After an UPDATE: rows keep their places, so only the indexes on the
  // columns it wrote are stale.
  void invalidate_table_caches(SqlTable& t, const std::vector<size_t>& written_cols);
  void row_index_after_erase(const SqlTable& t, std::vector<size_t> erased, size_t old_count,
                             const std::vector<std::string>* erased_keys = nullptr);
  // The index keys of `rows` (empty where a row has none), read before the
  // rows go: row_index_after_erase takes them out of the map by key.
  std::vector<std::string> row_index_keys_before_erase(const SqlTable& t, const std::vector<size_t>& rows);
  void probe_indices_after_erase(SqlTable& t, std::vector<size_t> erased, size_t old_count);
  // Brings the index for `t` into agreement, rebuilding or extending it.
  std::shared_ptr<RowIndex> get_or_sync_row_index(const SqlTable& t);
  std::shared_ptr<RowIndex> sync_row_index(const SqlTable& t, const std::vector<int>& pk_cols, int ai_idx,
                                           size_t max_rows = SIZE_MAX);   // at most this many rows indexed now (a big table builds its map a chunk per statement)
  // The identity of a row under a primary key that may span several columns.
  // A single-column key is just the one value; `PRIMARY KEY (a,b,c)` is all
  // three, which is why two stockmoves rows may share a stkmoveno and still
  // be different rows.
  static std::string row_key(const SqlRow& r, const std::vector<int>& pk_cols);

  // Persist unless a transaction is holding the writes back.
  void checkpoint();

  // Dynamic virtual table builder for MySQL information_schema compatibility (Navicat, DBeaver, Workbench)
  SqlTable build_info_schema_table(const std::string& table_name) const;
  mutable std::unordered_map<std::string, SqlTable> info_schema_cache_;
  // information_schema.columns, built once per schema shape: rebuilding it
  // on every reference (every column of every table - 17 ms on the ST mirror)
  // was the ERP's "does this column exist" check, once per getOpenCredits.
  // Rebuilt when schema_gen_ (bumped by every DDL, through the full cache
  // invalidation), views_gen_, the table count or the column count moves.
  mutable std::atomic<uint64_t> schema_gen_{0};
  mutable hal::Mutex info_columns_mu_;
  mutable std::shared_ptr<const SqlTable> info_columns_;
  mutable uint64_t info_columns_sig_ = ~0ull;
  const SqlTable* info_schema_columns() const;

  // CTE stack: active scoped temporary tables defined by WITH [RECURSIVE] clauses.
  // The CTE and ephemeral-table (materialized view, derived table) scopes of
  // the statement running on THIS thread. It was one vector on the database
  // shared by every session: two statements that used a CTE or a grouped view
  // at once pushed, popped and read it together, and a table copy landed on a
  // mutex another thread had already destroyed (heap corruption, found by four
  // concurrent readers and no writes). A deque: execute_with holds
  // .back() by reference while nested statements push more scopes.
  static SqlCteStack& cte_stack();

  // Query & Tax Lookup Cache for consolidating small lookups and eliminating N+1 query patterns
  // A cached answer, and the names its statement mentions: a write to a table
  // drops only the answers whose statement names it (MariaDB's query cache
  // rule), where it used to drop them all - on a live mirror every
  // replicated row emptied the cache. `all`: it read a view, so any write
  // can change it.
  struct QueryCacheEntry {
    std::string key;
    uint64_t id = 0;
    std::shared_ptr<const SqlQueryResult> result;   // shared: a hit copies it outside the lock
    std::vector<std::string> deps;
    bool all = false;
    size_t bytes = 0;
    std::shared_ptr<const std::string> wire;   // encoded text-protocol packets for wire_db; under the shard lock
    std::string wire_db;
    std::shared_ptr<const std::string> wire_bin;   // the binary-protocol (COM_STMT_EXECUTE) encoding, for wire_bin_db
    std::string wire_bin_db;
    // The same two for a latin1 session (text cells sent as cp1252): a session
    // that asked for latin1 hits its own copy instead of re-encoding every time.
    std::shared_ptr<const std::string> wire_l1, wire_bin_l1;
    std::string wire_l1_db, wire_bin_l1_db;
    // Set by a hit (under the shared lock, so no list is touched), cleared
    // when eviction passes the entry over once: a second-chance clock in
    // place of moving the entry to the list's front on every hit.
    mutable std::atomic<uint8_t> hit{0};
    QueryCacheEntry() = default;
    QueryCacheEntry(QueryCacheEntry&& o) noexcept
        : key(std::move(o.key)), id(o.id), result(std::move(o.result)), deps(std::move(o.deps)), all(o.all), bytes(o.bytes),
          wire(std::move(o.wire)), wire_db(std::move(o.wire_db)), wire_bin(std::move(o.wire_bin)), wire_bin_db(std::move(o.wire_bin_db)),
          wire_l1(std::move(o.wire_l1)), wire_bin_l1(std::move(o.wire_bin_l1)), wire_l1_db(std::move(o.wire_l1_db)),
          wire_bin_l1_db(std::move(o.wire_bin_l1_db)), hit(o.hit.load(std::memory_order_relaxed)) {}
    QueryCacheEntry& operator=(QueryCacheEntry&& o) noexcept {
      key = std::move(o.key); id = o.id; result = std::move(o.result); deps = std::move(o.deps); all = o.all; bytes = o.bytes;
      wire = std::move(o.wire); wire_db = std::move(o.wire_db); wire_bin = std::move(o.wire_bin); wire_bin_db = std::move(o.wire_bin_db);
      wire_l1 = std::move(o.wire_l1); wire_bin_l1 = std::move(o.wire_bin_l1); wire_l1_db = std::move(o.wire_l1_db);
      wire_bin_l1_db = std::move(o.wire_bin_l1_db); hit.store(o.hit.load(std::memory_order_relaxed), std::memory_order_relaxed);
      return *this;
    }
  };
  // The cache in shards by key hash, each with its own lock: a miss stores
  // its answer (and attaches its wire packets) under a shard's exclusive
  // lock, and with one lock four sessions of never-repeating statements ran
  // at half the engine's speed, waiting on each other's stores.
  struct QueryCacheShard {
    mutable SqlRwLock mu;
    std::list<QueryCacheEntry> lru;
    std::unordered_map<std::string, std::list<QueryCacheEntry>::iterator> lookup;
    // Entries by id: the name and view sets hold ids, not copies of the key.
    // A key is the whole statement text, and every name a statement mentions
    // (~25 for a three-table join) got its own copy of it, per store.
    std::unordered_map<uint64_t, std::list<QueryCacheEntry>::iterator> by_id;
    std::unordered_map<std::string, std::unordered_set<uint64_t>> deps;   // name -> ids
    std::unordered_set<uint64_t> all;
    size_t bytes = 0;
    uint64_t next_id = 1;
    // Read without the lock by a write's invalidation: how many entries
    // here depend on a name hashing to each slot, and how many read a view
    // (`all`). A write skips the shards that hold nothing for its tables -
    // with eight sessions, sixteen lock takes per write statement were 8%
    // of the server.
    std::array<std::atomic<uint32_t>, 64> dep_hits{};
    std::atomic<uint32_t> all_count{0};
  };
  static size_t query_cache_dep_slot(const std::string& lower_name) { return std::hash<std::string>{}(lower_name) & 63; }
  // When a table (by its dependency slot) was last written, steady-clock ns:
  // an answer that reads a table written within the last few dozen
  // milliseconds is not stored - a write would drop it before any reuse,
  // and the store, the copy it keeps and the drop cost a write-heavy mix
  // 10-20% (sysbench read_write) for nothing.
  std::array<std::atomic<uint64_t>, 64> dep_write_ns_{};
  std::atomic<bool> matview_maybe_{false};   // a materialized view has been kept at some point
  static constexpr size_t kQueryCacheShards = 16;
  // Entries across the shards, read without a lock: a write with nothing
  // cached (sysbench's write_only) skips the sixteen locks of an invalidation.
  mutable std::atomic<size_t> query_cache_entries_{0};
  mutable std::array<QueryCacheShard, kQueryCacheShards> query_cache_shards_;
  QueryCacheShard& query_cache_shard(const std::string& key) const {
    return query_cache_shards_[std::hash<std::string>{}(key) % kQueryCacheShards];
  }
  // Admission: a statement's answer is cached the second time the statement
  // is seen, not the first. Storing every answer (its key, its rows' byte
  // count, an eviction, the dependency scan) cost as much as running the
  // statement, and a workload of never-repeating statements (sysbench's
  // random ranges) got nothing for it. One 64-bit hash per slot, raced
  // freely: a lost write only delays an admission.
  mutable std::unique_ptr<std::atomic<uint64_t>[]> query_cache_seen_;
  bool query_cache_admit(const std::string& key) const;
  // A statement whose answer was past kMaxQueryCacheRows: never admitted after
  // (it streams), where it was run unstreamed to build an answer too big to keep.
  void query_cache_refuse(const std::string& key) const;
  // Lower-cased view names, rebuilt only when views_ changes (views_gen_).
  // Every cache-missing SELECT rebuilt it from views_, lowering each name.
  mutable std::atomic<uint64_t> views_gen_{0};
  mutable hal::Mutex view_names_mu_;
  mutable std::shared_ptr<const std::unordered_set<std::string>> view_names_lower_;
  mutable uint64_t view_names_gen_ = ~0ull;
  std::shared_ptr<const std::unordered_set<std::string>> view_names_lower() const;
  hal::Mutex table_list_mu_;
  std::string table_list_dir_;
  uint64_t table_list_sig_ = ~0ull;                         // schema_gen_/table count last published
  std::map<std::string, std::string> table_list_published_; // database -> file body as last written
  // Lower-cased table names, rebuilt when schema_gen_ or the table count moves.
  mutable hal::Mutex table_names_mu_;
  mutable std::shared_ptr<const std::unordered_set<std::string>> table_names_lower_;
  mutable uint64_t table_names_gen_ = ~0ull;
  mutable size_t table_names_count_ = 0;
  std::shared_ptr<const std::unordered_set<std::string>> table_names_lower() const;
  // Bumped by every invalidation: an answer computed while a write landed is
  // not stored, or it would outlive the write that made it stale.
  mutable std::atomic<uint64_t> query_cache_gen_{0};
  // Readers (a probe for a cached answer, twice per statement) share a
  // shard's lock; a store, an attach of wire packets, an eviction or an
  // invalidation takes it alone. One mutex around every probe was the most
  // contended lock in the server with four sessions reading.
  static constexpr size_t kMaxQueryCacheEntries = 1u << 20;   // the byte budget is the real limit
  static constexpr size_t kMaxQueryCacheRows = 4096;
  static size_t query_cache_budget_bytes();   // AIOS_SQL_CACHE_MB, default 1024
  // Table-scoped writes (see "per-table statement locks" in sql_database.cpp).
  bool table_write_precheck(SqlTable* t, const char* event);
  bool table_write_lock(SqlTable* t, const char* event);
  bool table_scoped_escalate();
  static SqlQueryResult escalate_result();
  void after_statement_unlocked();
  std::atomic<bool> checkpoint_due_{false};   // a checkpoint came due under the shared lock
  uint64_t last_checkpoint_ns_ = 0;
  static uint64_t checkpoint_secs();          // AIOS_CHECKPOINT_SECS, default 30
  static bool redo_sync_lazy();               // AIOS_REDO_SYNC=0: a thread syncs every 200 ms, statements do not wait
  std::atomic<bool> settle_due_{false};       // Things to settle once a table-scoped write's locks are gone
  void query_cache_erase_locked(QueryCacheShard& sh, std::list<QueryCacheEntry>::iterator it);
  void query_cache_store(const std::string& key, std::shared_ptr<const SqlQueryResult> r,
                         std::vector<std::string> deps, bool all, uint64_t gen_before);
 public:
  // Drops the cached answers whose statement names `table` (unqualified).
  void invalidate_query_cache_for(const std::string& table);
  void invalidate_query_cache_for(const std::vector<std::string>& tables);
 private:

  SqlQueryResult execute_create_table(const std::string& sql);
  SqlQueryResult execute_create_view(const std::string& sql);
  SqlQueryResult execute_insert(const std::string& sql);
  SqlQueryResult execute_select(const std::string& sql);
  SqlQueryResult execute_select_single(const std::string& sql);
  SqlQueryResult execute_with(const std::string& sql);
  SqlQueryResult execute_update(const std::string& sql);
  SqlQueryResult execute_update_join(const std::string& sql, const std::string& target_name);
  SqlQueryResult execute_delete(const std::string& sql);
  SqlQueryResult execute_drop(const std::string& sql);
  SqlQueryResult execute_drop_view(const std::string& sql);
  SqlQueryResult execute_alter(const std::string& sql);
  SqlQueryResult execute_truncate(const std::string& sql);
  SqlQueryResult execute_show(const std::string& sql);
  SqlQueryResult execute_describe(const std::string& sql);
  SqlQueryResult execute_explain(const std::string& sql);
  SqlQueryResult execute_transaction(const std::string& verb);
  SqlQueryResult execute_what_if(const std::string& sql, const std::string& session_db);
  // A client statement writing a table a Thing keeps (INTO): the refusal, or "".
  std::string kept_table_refusal(const std::string& sql, const std::string& session_db);
  // CALL of a stored procedure this engine implements natively (the ERP's
  // getNextTransNo); and `SET @name = expr` for the connection's variables.
  SqlQueryResult execute_call(const std::string& sql);
  SqlQueryResult execute_set_user_vars(const std::string& sql);
  std::string format_result_table(const std::vector<std::string>& cols, const std::vector<SqlRow>& rows) const;
};

}  // namespace aios::userland
