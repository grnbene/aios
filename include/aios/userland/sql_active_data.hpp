#pragma once
// Active data for ASQL: structures the engine keeps current as rows change, so a read finds its answer
// already made instead of computing it.
//
//   CREATE ACTIVE FILTER name ON table WHERE cond
//       A bitset of the rows meeting `cond`, kept up to date row by row. A SELECT whose WHERE is that
//       condition reads the bits and never evaluates the predicate.
//   CREATE ACTIVE CACHE name AS SELECT COUNT(*), SUM(x), ... FROM table [WHERE cond]
//       The aggregates of one query, adjusted by the before / after image of every row change. The same
//       SELECT text is answered from them; no write ever discards them.
//   CREATE ACTIVE LINK name ON child(col) REFERENCES parent(col)
//       Child row -> parent row and parent row -> child rows, by integer key, kept as rows change. A join
//       on those columns follows the row numbers instead of hashing the keys.
//   CREATE ACTIVE VIEW name AS SELECT g, COUNT(*), SUM(x) ... FROM table [WHERE cond] GROUP BY g
//       A table `name` holding the grouped result. A write adjusts only the groups its rows belong to
//       (applied when the statement settles, as Things and Cells are); the query is never run again.
//
// Rows are numbered by position, so what a write that moves them (DELETE, OPTIMIZE) leaves behind is
// not patched but marked stale and rebuilt on the next read that needs it.

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "aios/hal/sync.hpp"
#include "aios/userland/portable_int128.hpp"

namespace aios::userland {

class SqlDatabase;
class SqlRowExpr;
struct SqlTable;
struct SqlRow;
struct SqlQueryResult;

class ActiveDataEngine {
 public:
  explicit ActiveDataEngine(SqlDatabase& db);
  ~ActiveDataEngine();

  // The objects saved by earlier runs, built again from the tables as they were loaded.
  void load();
  // After the redo log was replayed: every object rebuilt from the tables as they now are.
  void rebuild_all();

  // CREATE / DROP ACTIVE {FILTER|CACHE|LINK|VIEW}, SHOW ACTIVE {FILTERS|CACHES|LINKS|VIEWS|STATUS|TIERING}
  bool handles(const std::string& sql) const;
  SqlQueryResult execute(const std::string& sql);

  // Row changes, as Things and Cells receive them. `row` is the row's position (UINT64_MAX when the
  // caller does not know it: the structures that need it go stale).
  bool watches(const SqlTable& t) const;
  void on_row_change(const SqlTable& t, const SqlRow* before, const SqlRow* after, size_t row);
  void on_table_reset(const SqlTable& t);     // contents replaced wholesale (TRUNCATE, ALTER, rollback)
  void on_table_dropped(const SqlTable& t);   // the table is going away (its objects wait for it to come back)
  void on_schema_change();                    // an ALTER: column positions may have moved

  // View groups changed since the view table was written; settle() writes them.
  bool has_pending() const { return pending_views_.load(std::memory_order_acquire) != 0; }
  void settle();

  // The bits of the rows of `t` meeting the filter whose condition is `where_norm`, or null when no
  // filter has that condition. A stale filter is rebuilt first.
  const std::vector<uint64_t>* get_filter_bits(const SqlTable& t, const std::string& where_norm,
                                               size_t* out_count);

  // The answer to `sql` when it is the query of an active cache.
  bool try_eval_cache(const std::string& sql, SqlQueryResult* out);

  // A join of `base` and `joined` on base.col = joined.col that an active link covers. A usable link
  // gives, for a row of `base`, the row(s) of `joined` it meets. The vectors stay valid while the
  // caller holds the statement lock that kept the tables still.
  struct LinkLookup {
    bool found = false;
    const std::vector<uint32_t>* to_parent = nullptr;                    // base is the child
    const std::vector<std::vector<uint32_t>>* to_children = nullptr;     // base is the parent
  };
  LinkLookup find_link(const SqlTable& base, size_t base_col, const SqlTable& joined, size_t joined_col);

  // Tiering: tables nothing has written for a while have their delta buffer folded in, then are
  // re-encoded. tick_scan() runs under the shared statement lock, tick_apply() under the exclusive one.
  struct TierWork {
    std::string key;          // database.table, lower case
    bool fold_delta = false;
    bool reencode = false;
    bool release_slack = false;   // spare vector capacity from appends goes back
  };
  bool tick_due();
  std::vector<TierWork> tick_scan();
  void tick_apply(const std::vector<TierWork>& work);
  // A re-encode finished off the lock (SqlDatabase::active_data_tick): as tick_apply notes one it did itself.
  void note_reencoded(const std::string& key, uint64_t seq);

 private:
  using I128 = AIOS_INT128;

  // ---- aggregates, shared by caches and views ----
  struct Extreme {
    bool numeric = false;
    double num = 0;
    std::string text;
  };
  struct AggSpec {
    enum Kind { CountStar, Count, Sum, Avg, Min, Max } kind = CountStar;
    std::string label;                 // the column name the client sees
    std::string expr_text;
    std::unique_ptr<SqlRowExpr> expr;
    int scale = 0;                     // most fractional digits any value has had
    std::string plain_column;          // the expression, when it is one column
  };
  struct Acc {
    int64_t nn = 0;                    // non-NULL values
    I128 isum = 0;                     // exact sum, in units of 1e-9
    double dsum = 0;                   // the sum as a double, for values that are not exact decimals
    bool inexact = false;
    bool has_min = false, has_max = false;
    bool min_stale = false, max_stale = false;   // the extreme left: the table is read again
    Extreme min, max;
  };
  struct GroupAcc {
    int64_t rows = 0;
    std::vector<Acc> acc;
    std::vector<std::string> key_vals;   // the group columns as last seen
  };
  void agg_apply(std::vector<AggSpec>& specs, GroupAcc& g, const SqlRow& row, int sign);
  static std::string agg_render(const AggSpec& s, const GroupAcc& g, const Acc& a);

  struct ActiveFilter {
    std::string name, db, table_key, table_name, where_sql, where_norm;
    std::unique_ptr<SqlRowExpr> expr;
    std::vector<uint64_t> words;
    size_t count = 0;
    size_t synced_rows = 0;        // rows the bits cover
    uint64_t layout_seq = 0;       // the table's layout when they were built
    bool stale = true;
    uint64_t hits = 0, mutations = 0;
  };

  struct ActiveCache {
    std::string name, db, table_key, table_name, query_norm, query_sql, where_sql;
    bool qualified = false;        // the query names its database: any session may read the answer
    std::unique_ptr<SqlRowExpr> where;
    std::vector<AggSpec> specs;
    GroupAcc total;
    bool stale = true;
    uint64_t hits = 0, mutations = 0;
  };

  struct ActiveLink {
    std::string name, db;
    std::string child_key, parent_key, child_name, parent_name, child_col_name, parent_col_name;
    size_t child_col = 0, parent_col = 0;
    std::vector<uint32_t> child_to_parent;
    std::vector<std::vector<uint32_t>> parent_to_children;
    std::unordered_map<int64_t, uint32_t> parent_by_key;
    std::unordered_map<int64_t, std::vector<uint32_t>> orphans;   // children whose parent is not there (yet)
    size_t child_rows = 0, parent_rows = 0;
    uint64_t child_layout = 0, parent_layout = 0;
    bool stale = true;
    bool usable = false;
    std::string why_not;
    uint64_t hits = 0, mutations = 0;
  };

  struct ActiveView {
    std::string name, db, table_key, table_name, select_sql, where_sql;
    std::unique_ptr<SqlRowExpr> where;
    std::vector<size_t> group_cols;          // columns of the base table
    std::vector<std::string> group_col_names;   // ... by name, to find them again after an ALTER
    bool broken = false;                     // a group column was dropped
    std::vector<AggSpec> specs;
    // Output column order: for each, either a group column (index into group_cols) or an aggregate.
    struct Out { bool is_group; size_t idx; std::string label; };
    std::vector<Out> outs;
    std::unordered_map<std::string, GroupAcc> groups;
    std::unordered_map<std::string, std::vector<std::string>> shown;   // group key -> key values in the view table
    std::unordered_set<std::string> dirty;          // groups changed since the view table was written
    std::unordered_set<std::string> stale_groups;   // groups whose MIN / MAX left: read from the table again
    bool all_dirty = true;       // the whole view table is written again
    bool state_stale = true;     // the groups are rebuilt from the table first
    bool pending = false;        // counted in pending_views_
    uint64_t updates = 0;
  };

  struct TableTelemetry {
    uint64_t seen_seq = 0;
    uint64_t changed_ns = 0;
    bool reencoded = false;
    bool slack_released = false;
  };

  SqlDatabase& db_;
  mutable aios::hal::Mutex mu_;
  std::string load_db_;                  // set while load() runs: the database unqualified names mean

  std::map<std::string, std::shared_ptr<ActiveFilter>> filters_;   // "db.name"
  std::map<std::string, std::shared_ptr<ActiveCache>> caches_;
  std::map<std::string, std::shared_ptr<ActiveLink>> links_;
  std::map<std::string, std::shared_ptr<ActiveView>> views_;
  std::unordered_map<std::string, TableTelemetry> telemetry_;     // "db.table"
  std::unordered_set<std::string> watched_;                       // "db.table"
  std::atomic<size_t> cache_count_{0};
  std::atomic<size_t> pending_views_{0};
  std::atomic<uint64_t> last_tick_ns_{0};

  // Statement handlers
  SqlQueryResult create_filter(const std::string& sql, bool persist);
  SqlQueryResult create_cache(const std::string& sql, bool persist);
  SqlQueryResult create_link(const std::string& sql, bool persist);
  SqlQueryResult create_view(const std::string& sql, bool persist);
  SqlQueryResult drop_object(const std::string& sql, char kind);
  SqlQueryResult show(char kind);
  SqlQueryResult show_status();
  SqlQueryResult show_tiering();

  // Helpers (hold mu_ unless noted)
  void rebuild_filter(ActiveFilter& f, const SqlTable& t);
  void rebuild_cache(ActiveCache& c, const SqlTable& t);
  void rebuild_link(ActiveLink& l, const SqlTable& child, const SqlTable& parent);
  void rebuild_view_state(ActiveView& v, const SqlTable& t);
  void link_attach_child(ActiveLink& l, size_t row, const std::string& key_text);
  void link_detach_child(ActiveLink& l, size_t row, const std::string& key_text);
  void link_attach_parent(ActiveLink& l, size_t row, const std::string& key_text);
  void link_detach_parent(ActiveLink& l, size_t row, const std::string& key_text);
  void note_view_dirty(ActiveView& v, const std::string& gkey);
  void settle_view(ActiveView& v);
  void persist(const std::string& kind, const std::string& db, const std::string& name, const std::string& sql);
  void unpersist(const std::string& kind, const std::string& db, const std::string& name);
  void register_watch(const std::string& table_key);
  void recompute_watched();
};

}  // namespace aios::userland
