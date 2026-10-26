#pragma once
#include "aios/userland/portable_int128.hpp"  // MSVC has no __int128
// Things: live objects kept inside the SQL engine.
//
// A Thing kind is declared once, as a grouped aggregate over the mirror's own
// tables:
//
//   CREATE THING salesman_mtd KEY salesperson PARTITION BY transno
//     READS stockmoves, debtortrans, custbranch, territory, stockmaster, debtorsmaster
//     AS SELECT salesperson, SUM(discountedlinetotal)*-1 AS total, COUNT(discountedlinetotal) AS lines
//        FROM stocktrans WHERE DATE(trandate) BETWEEN DATE_FORMAT(CURDATE(), '%Y-%m-01') AND CURDATE()
//        GROUP BY salesperson
//
// Each distinct KEY value is one Thing (one salesperson's month to date). A
// Thing holds its values, a version, a history of what changed it and why,
// what it is waiting for, and what it may read.
//
// Connected: the tables its query actually reads (captured from the executor's
// plan, and required to be within READS) are its links. Every row written to
// one of them is a change notice. A notice on a table carrying the PARTITION
// column (an invoice's transno) names the partitions it touched; a notice on
// any other linked table (a customer's branch, a territory) touches everything.
//
// Reacting: each touched partition's contribution is re-read from the tables
// through the ordinary executor and replaces the old one - replaced, never
// added, so a notice delivered twice (redo replay after a crash, a re-applied
// binlog event) cannot count anything twice. Values must be linear in SUM and
// COUNT so partitions add up to exactly what the declared query returns.
//
// Remembering: the Things are saved with the tables at every checkpoint; the
// redo log replayed at boot re-delivers the notices since, so a restart picks
// up exactly where it stopped. A window that moves with the date (CURDATE())
// waits for the next day and recomputes when it comes.
//
// A Thing keyed by its own partition column (KEY orderno PARTITION BY orderno)
// is one row per key: every group lies inside one partition, so re-reading the
// partition gives the whole group, and its values may be any expression - the
// order's status, customer, MAX(itemdue), a CASE - not only SUMs and COUNTs.
// A key whose partition no longer returns a row (every line completed, the
// order cancelled) stops being a Thing:
//
//   CREATE THING open_orders KEY so.orderno PARTITION BY so.orderno
//     READS salesorders, salesorderdetails, config
//     AS SELECT so.orderno, so.debtorno, so.orderstatusid, COUNT(*) AS open_lines, ...
//        FROM salesorders so JOIN salesorderdetails sd ON sd.orderno = so.orderno
//        WHERE sd.completed = 0 GROUP BY so.orderno
//
// When several systems report the same Thing (a customer in the CRM and in the ERP), SOURCE BY names the column
// that says which one a row came from; the query groups by it, so each source gives its own row per key:
//
//   CREATE THING customer KEY id SOURCE BY origin READS customer_feed
//     AS SELECT id, origin, email, phone FROM customer_feed GROUP BY id, origin
//
// The Thing keeps every source's row. A field they agree on is that value; a field they disagree on is not settled
// by whichever row came last - it is a conflict, both values kept and named in the Thing's `conflicts` (SHOW THINGS,
// SHOW THING .. JSON, history), until the sources agree again or one is chosen:
//
//   RESOLVE THING customer 'k1' email TAKE 'crm'      the CRM's email stands while both still report one
//   RESOLVE THING customer 'k1' email UNRESOLVE       back to a conflict
//   SHOW THING CONFLICTS customer ['k1']              one row per side of every conflicted field
//
// A choice is kept (with the Thing, across restarts) and dropped once the sources agree, or the chosen one stops
// reporting the Thing.
//
// Conflicts that share a cause are settled together:
//
//   SHOW THING CONFLICT GROUPS customer               the conflicts grouped by field and by which sources side
//                                                      together ("billing,crm | erp"), with the change that raised them
//   RESOLVE THING customer * email TAKE 'erp'          every email conflict erp reports on, at once
//   RESOLVE THING customer * email UNRESOLVE           every email pick undone
//   RESOLVE THING customer * email PREFER 'erp','crm'  a standing rule: email conflicts, now and later, take the first
//                                                      of these that reports the Thing (a pick by hand still wins;
//                                                      field * = every field without a rule of its own)
//   RESOLVE THING customer * email NOPREFER            the rule dropped
//
// And the picks are checked against each other (a SAT problem; an UNSAT answer names the picks and rules that clash,
// its minimal core, verified):
//
//   CHECK THING customer ADD UNIQUE email              no two Things end up with the same email
//   CHECK THING customer ADD TOGETHER email, phone     a Thing's email and phone come from one source
//   CHECK THING customer DROP UNIQUE email | DROP TOGETHER email, phone
//   CHECK THING customer                               consistent? and a choice for each open conflict that keeps it so
//   CHECK THING customer APPLY                         ... and take that choice
//
// A Thing whose logic is more than a grouped query is written in grain:
//
//   CREATE THING reorder_daily GRAIN KEY stockid READS stockmoves, locstock, ...
//     WATCH stockmoves(stockid), locstock(stockid), ...  LINKS open_order(stockid)
//     AS '<grain program>'
//
// The program defines enroll(), shared(), things(keys, shared) and optionally
// touched(table, op, before, after, shared). It runs in a VM with no files,
// clock, randomness, network or writes: it reads only through read(sql, ...),
// the engine's own executor, and only the tables it declared - so a reaction
// replayed after a crash computes exactly what it computed the first time.
// LINKS lets it read another Thing - claims(kind, keys) gives a claims kind's
// queues - and hear whenever those queues are shared out again.
#include <cstdint>
#include "aios/hal/sync.hpp"
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aios::userland {

class SqlDatabase;
struct SqlTable;
struct SqlRow;
struct SqlQueryResult;
struct ThingGrain;   // a grain kind's program and VM

// The fleet, as the Things see it: run one statement on the members whose
// identity starts with `target` ("ALL" for every member), each answer tagged
// with the member's short identity. Set once per process by machine_sql.cpp
// (which knows the kernel and its fleet link); unset, ON <machine> is refused.
struct ThingFleetAnswer {
  std::string machine;
  bool ok = false;
  std::string error;
  std::shared_ptr<SqlQueryResult> result;
};
using ThingFleetFn = std::function<void(const std::string& target, const std::string& sql, std::vector<ThingFleetAnswer>* out)>;
void set_things_fleet(ThingFleetFn fn);

// An exact decimal: value * 10^-scale.
struct ThingDec {
  AIOS_INT128 v = 0;
  int scale = 0;
};

class ThingEngine {
  friend struct ThingGrainAccess;   // the grain natives that read other kinds (sql_things.cpp)
 public:
  explicit ThingEngine(SqlDatabase& db);

  // True when some kind reads this table (checked before building row images).
  bool watches(const SqlTable& t) const;
  // A row written to a watched table: before and/or after image.
  void on_row_change(const SqlTable& t, const SqlRow* before, const SqlRow* after);
  // A watched table emptied or replaced wholesale.
  void on_table_reset(const SqlTable& t);

  // Run the reactions pending notices call for. The caller holds the
  // statement lock exclusively (SqlDatabase::things_settle).
  void settle();
  // Time passing: a window keyed to the date moves at midnight.
  void tick();
  bool has_pending() const;

  // CREATE THING / DROP THING / SHOW THINGS / SHOW THING HISTORY / REPLAY THING /
  // CALL THING (a grain Thing's report for inputs given with the call).
  bool handles(const std::string& sql) const;
  SqlQueryResult execute(const std::string& sql);

  // Persistence under <db root>/things.
  void load();
  void save();

 private:
  struct Item {
    std::string text;      // the expression as declared
    std::string label;     // output column name
    bool count_like = false;   // an empty group reads 0, not NULL
  };
  struct Partial {
    std::string key;
    bool key_null = false;
    std::vector<std::string> vals;   // as the partition query printed them (sql_null() for NULL)
  };
  struct History {
    uint64_t wall_ns = 0;
    std::string cause;
    std::string before;
    std::string after;
  };
  struct Instance {
    std::string key;
    bool key_null = false;
    std::vector<ThingDec> sums;
    std::vector<int64_t> nonnull;   // partials that were not NULL, per item
    std::vector<int> scale;         // the widest scale seen, per item
    int64_t parts = 0;              // partitions contributing
    uint64_t version = 0;
    uint64_t updated_ns = 0;
    std::vector<History> history;   // newest last, bounded
    std::vector<std::string> row;   // (row kinds) the values as the query printed them
    // SOURCE BY kinds: each source's values for this key (in the order the partition gave them), the fields the
    // sources disagree on (labels; their value in `row` is NULL unless picked), and RESOLVE THING's choices
    // (label -> source). `row` is merged from the sides.
    std::vector<std::pair<std::string, std::vector<std::string>>> sides;
    std::vector<std::string> conflicts;
    std::map<std::string, std::string> picks;
    std::map<std::string, std::string> ruled;   // conflicts a PREFER rule settled (label -> source), no pick by hand
    // PART rows that belong to this key, by part name, as the part's query printed them.
    std::map<std::string, std::vector<std::vector<std::string>>> part_rows;
    uint64_t stamp = 0;             // the kind's clock when this Thing last changed (SHOW THING .. JSON SINCE)
    bool acted = false;             // WHEN held and DO ran; re-armed when WHEN stops holding
    // The document SHOW THING .. JSON hands out, rendered once per change: a read of a
    // Thing that did not change is a copy, not a build.
    mutable std::string json_doc;
    mutable uint64_t json_stamp = 0, json_version = 0;
    mutable bool json_valid = false;
  };
  struct Kind {
    std::string name;
    std::string declaration;         // the CREATE THING text, replayed on load
    std::string db;                  // the database it was declared in
    std::string key_col, part_col;   // part_col bare: the column change notices carry
    std::string part_expr;           // PARTITION BY as declared (so.orderno), what the queries use
    bool row_mode = false;           // KEY is the partition column: one row per key, any values
    uint64_t left = 0;               // (row kinds) keys that stopped being Things
    std::vector<std::string> reads;  // table names it may read (lower case)
    // Tables reached through the views its query (or READS) names, "table (via
    // view)": derived from the views' current definitions at each full
    // recompute, never persisted - a redefined view changes them.
    std::vector<std::string> view_reads;
    // PART <name> AS SELECT ... WHERE <expr> = KEY ...: rows that belong to each
    // Thing, any number per key (a row kind only). Re-read for a key when a row
    // of the part's key table changes for that key, for every key when any
    // other table the part reads changes. All of it derived from the query.
    struct PartDef {
      std::string name;
      std::string key_expr;              // what is compared to KEY, as written (sd.orderno)
      std::string key_table;             // its table, bare lower-case name
      std::string key_col;               // its bare column (change notices carry it)
      std::string key_dep;               // "db.table" once resolved
      std::vector<std::string> tables;   // every table the part names, bare lower-case
      std::set<std::string> deps_full;   // the other tables, "db.table" once resolved
      std::string head, tail;            // the query around the comparison: SELECT <head><expr> IN (..)<tail>
      std::vector<std::string> cols;     // output names, from the first run
    };
    std::vector<PartDef> part_defs;
    std::map<std::string, std::set<std::string>> pending_part_keys;   // part -> canon keys to re-read
    std::set<std::string> pending_part_full;                          // parts to re-read for every key
    std::map<std::string, uint64_t> departed;   // key -> the clock when it stopped being a Thing
    // WHEN <condition over the row's columns> DO <statement>[; ...] TO <profile>: the work a
    // Thing does itself. Checked for every Thing that changed; runs once when the condition
    // comes to hold (one transaction on the profile, KEY filled in), again after it clears.
    std::string when_expr, do_sql, do_target;
    uint64_t act_clock = 0;                     // the clock up to which every change was checked
    std::vector<std::string> out_labels;        // the head's output names when an item is a `*` (from the first run)
    std::string source_by;                      // SOURCE BY: the value column naming the system a row came from
    // RESOLVE THING .. PREFER: field label (or "*") -> the sources in the order preferred
    std::vector<std::pair<std::string, std::vector<std::string>>> prefer;
    // CHECK THING .. ADD: UNIQUE (one field) or TOGETHER (two or more) over the merged values
    struct Constraint { bool unique = true; std::vector<std::string> fields; };
    std::vector<Constraint> constraints;
    std::string source, where_text, group_text;
    std::vector<Item> items;         // items[0] is the key
    bool time_dependent = false;
    std::string day;                 // CURDATE() the window was last computed for
    std::set<std::string> deps_partition;   // linked tables carrying part_col ("db.table")
    std::set<std::string> deps_full;        // linked tables without it
    std::unordered_map<std::string, std::vector<Partial>> parts;
    std::map<std::string, Instance> things; // by key ("\x01NULL" for a NULL key)
    std::unordered_map<std::string, std::string> pending;   // partition -> cause
    bool pending_full = false;
    std::string full_cause;
    uint64_t version = 0;
    uint64_t created_ns = 0;
    uint64_t reactions = 0;          // partition recomputes run
    uint64_t full_recomputes = 0;
    // What kind of Thing: a grouped aggregate, a resource others claim
    // (stock at a location), or claimants (orders whose lines claim it).
    enum Type { Aggregate = 0, Resource = 1, Claims = 2, Grain = 3, Event = 4, Mirror = 5 } type = Aggregate;
    bool paused = false;               // PAUSE THING: notices wait, nothing reacts or fires
    bool building = false;             // CREATE OR REPLACE THING's new version, before it takes the name: not saved or dropped
    // EVENT (CREATE THING name AT '<when>' GRAIN ... WRITES ... AS '<grain>'): a
    // Thing that acts once. At its time the program's fire(ctx) runs with
    // write() on the WRITES tables, then verify and cleanup; it leaves a
    // receipt and removes itself. A crash mid-fire fires again at start.
    std::string at;                    // 'YYYY-MM-DD HH:MM:SS', local time
    std::vector<std::string> writes;   // tables it may write (lower case)
    std::string write_target;          // WRITES ... TO <profile>: a real database's tables, not ASQL's
    std::string params_json;           // PARAMS, the program's global `params`
    std::string state = "armed";       // armed | firing | failed
    std::string fire_error;
    // MIRROR (CREATE THING name MIRROR kind ON machine [EVERY n SECONDS]): another
    // machine's Things, pulled when their versions move, with history and alerts
    // of their own here. MACHINES (CREATE THING name MACHINES [EVERY n SECONDS]):
    // one Thing per fleet member - reachable, what it runs. Kept in gthings.
    std::string mirror_kind;           // empty: MACHINES
    std::string mirror_machine;
    int every_s = 10;
    uint64_t next_pull_ns = 0;         // (not saved: the first tick after load pulls)
    std::string mirror_error;          // the last pull that failed; the values are as last seen
    uint64_t mirror_ok_ns = 0;         // the last pull that answered
    // GRAIN: the Thing is a grain program's answer per key (see
    // "grain Things" in sql_things.cpp). WATCH maps a table to the column naming the key
    // its rows touch; the program's touched() hears every other change.
    std::string grain_src;
    std::map<std::string, std::string> watch;   // table (lower case) -> key column
    std::map<std::string, std::string> links;   // claims kind it may read -> the resource key column naming its key
    std::shared_ptr<ThingGrain> grain;
    struct GInst {
      std::string key;                                          // as the program printed it
      std::vector<std::pair<std::string, std::string>> vals;   // field -> text, in the program's order
      uint64_t version = 0, updated_ns = 0;
      std::vector<History> history;
    };
    std::map<std::string, GInst> gthings;       // by collation key ('ABC' and 'abc' are one item)
    // The rows SHOW THING <name> answers, built at `shown_version` (k.version): a full read takes this under the
    // lock and copies it after the lock is let go, so readers of a big kind no longer queue behind one another.
    std::shared_ptr<const std::vector<SqlRow>> shown_rows;
    uint64_t shown_version = ~0ull;
    // Things that wait for a moment: a program's wake_at ('YYYY-MM-DD HH:MM:SS')
    // wakes that Thing then - its key re-read, cause "woke".
    std::map<std::string, std::set<std::string>> wakes;   // time -> collation keys
    std::unordered_map<std::string, std::string> wake_of; // collation key -> time
    std::vector<std::string> gcols;             // every field seen, first-seen order
    bool pending_shared = false;
    std::string grain_error;                    // the last run that failed, shown by SHOW THINGS
    std::string shared_json;                    // shared() as saved, until the VM takes it back
    double last_full_ms = 0;
    // RESOURCE: one Thing per key, capacity = SUM(cap_col) of cap_table rows.
    // Only keys something claims are held (the rest cannot change anything).
    std::vector<std::string> key_cols;
    std::string cap_col, cap_table;
    struct ResInst {
      std::vector<std::string> keyvals;   // as first seen
      ThingDec cap;
      bool cap_known = false;             // a capacity row exists
      ThingDec demand, promised;          // derived from the claims
      int64_t claims = 0;
      uint64_t version = 0, updated_ns = 0;
      std::vector<History> history;
    };
    std::map<std::string, ResInst> res;                        // by collation key of the key values
    std::unordered_map<std::string, std::string> pending_res;  // key -> cause
    std::string claimed_by;                                    // the claims kind
    // CLAIMS: one Thing per claimant key; its lines claim the resource in
    // PRIORITY order, first come first served - or, with ALLOCATE READY, so
    // that the most claimants are complete (an exact plan by the SAT solver,
    // its optimality proven), the rest still served in PRIORITY order.
    std::string resource;
    bool alloc_ready = false;
    std::set<std::string> ready_plan;      // (READY) the claimants the plan makes whole
    int64_t alloc_orders = 0, alloc_ready_n = 0, alloc_fifo_n = 0, alloc_components = 0, alloc_proven = 0;
    std::string alloc_evidence;            // (READY) the last plan's evidence
    std::string claim_key_expr, claim_items, claim_from, claim_where;
    std::vector<std::string> prio_exprs;
    struct Line {
      std::string lineid;
      std::string reskey;                 // collation key into the resource
      std::vector<std::string> resvals;   // as the query printed them
      ThingDec need, alloc;
      std::vector<std::string> prio;
      int64_t pos = 0, qlen = 0;          // place in the queue, queue length
    };
    struct OrderInst {
      std::vector<Line> lines;
      uint64_t version = 0, updated_ns = 0;
      std::vector<History> history;
    };
    std::map<std::string, OrderInst> orders;
    std::unordered_map<std::string, std::vector<std::pair<std::string, size_t>>> queue_index;  // reskey -> (order, line)
    std::set<std::string> dirty_queues;
    std::string wave_cause;
    std::map<std::string, std::string> order_pre, res_pre;   // displays before this wave touched them
    // REPLAY (proof harness): the captured rows, grouped by day, and a cursor.
    std::vector<std::pair<std::string, std::vector<std::string>>> replay_days;  // day -> INSERT statements
    size_t replay_cursor = 0;
  };

  std::string table_key(const SqlTable& t) const;
  bool parse_create(const std::string& sql, Kind* k, std::string* err) const;
  bool capture_links(Kind& k, std::string* err);
  std::string partition_sql(const Kind& k, const std::vector<std::string>& parts) const;
  std::string full_sql(const Kind& k) const;
  // The stored tables behind every view that `texts` (FROM/JOIN clauses, or a
  // READS list) name, through nested views; each as "db.table" in `tables`
  // and "table (via view)" in `via`.
  void view_tables(const std::vector<std::string>& texts, const std::string& db,
                   std::set<std::string>* tables, std::vector<std::string>* via) const;
  // A table the executor read that the declaration allows: in READS, or
  // behind a view the query names (the views are resolved on the first call).
  bool may_read(const std::vector<std::string>& reads, const std::vector<std::string>& texts, const std::string& db,
                const SqlTable& t, std::set<std::string>* view_tabs, std::vector<std::string>* via) const;
  bool run_partitions(Kind& k, const std::vector<std::string>& parts, const std::string& cause, std::string* err);
  bool run_full(Kind& k, const std::string& cause, std::string* err);
  void apply_partition(Kind& k, const std::string& part, std::vector<Partial> fresh,
                       std::map<std::string, std::string>* before_display);
  void finish_changes(Kind& k, const std::map<std::string, std::string>& before_display, const std::string& cause);
  std::string display(const Kind& k, const Instance& in) const;
  // SOURCE BY: a value column's name, the source column's index among the values (-1 none), and the merge of a
  // Thing's sides into its row and conflicts (picks applied, stale picks dropped)
  std::string value_label(const Kind& k, size_t v) const;
  int source_index(const Kind& k) const;
  void merge_sides(const Kind& k, Instance& in) const;
  SqlQueryResult resolve(const std::string& sql);
  SqlQueryResult show_conflicts(const std::string& name, const std::string& key);
  SqlQueryResult conflict_groups(const std::string& name);
  SqlQueryResult check(const std::string& sql);
  std::string value_text(const Kind& k, const Instance& in, size_t item) const;
  std::string waiting_for(const Kind& k) const;
  std::string current_day();
  void settle_kind(Kind& k);
  SqlQueryResult show(const std::string& name);
  SqlQueryResult show_history(const std::string& name, const std::string& key);
  SqlQueryResult show_json(const Kind& k, uint64_t since, bool clock_only) const;
  std::string part_sql(const Kind& k, const Kind::PartDef& p, const std::vector<std::string>& keys) const;
  bool resolve_part_deps(Kind& k, std::string* err);
  bool run_part(Kind& k, Kind::PartDef& p, const std::vector<std::string>* keys, const std::string& cause, std::string* err);
  void settle_parts(Kind& k);
  void stamp_instance(Kind& k, Instance& in);
  void act_due(Kind& k);
  bool condition_holds(const Kind& k, const Instance& in, bool* holds, std::string* err);
  bool run_act(const Kind& k, const Instance& in, std::string* err);
  SqlQueryResult replay(const std::string& name, const std::string& action, const std::string& from);
  std::string things_dir() const;
  std::string serialize(const Kind& k) const;
  bool deserialize(const std::string& text, Kind* k, std::string* err);
  void save_locked();   // save() with mu_ already held
  // Resource / claims kinds (two kinds that know about each other).
  bool parse_resource(const std::string& sql, Kind* k, std::string* err) const;
  bool parse_claims(const std::string& sql, Kind* k, std::string* err) const;
  std::string reskey_of(const std::vector<std::string>& vals) const;
  bool load_capacity(Kind& r, const std::vector<std::string>& keys, const std::string& cause, std::string* err);
  bool load_claims(Kind& c, const std::vector<std::string>& orders, bool all, const std::string& cause, std::string* err);
  void reallocate(Kind& c, Kind& r, const std::string& cause, bool quiet);
  void plan_ready(Kind& c, Kind& r);                  // ALLOCATE READY: which claimants are made whole
  SqlQueryResult show_allocation(const Kind& c) const;
  void settle_links();
  void settle_all_locked();   // aggregates, then resources and claims, then grain kinds (which read them)
  std::string res_display(const Kind::ResInst& ri) const;
  std::string order_display(const Kind::OrderInst& oi) const;
  std::string order_waiting(const Kind& c, const Kind::OrderInst& oi) const;
  SqlQueryResult show_resource(const Kind& k);
  SqlQueryResult show_claims(const Kind& k);
  SqlQueryResult show_lines(const Kind& k);
  SqlQueryResult replay_claims(Kind& k, const std::string& action, size_t n);
  void rebuild_watched();
  // Grain kinds.
  bool parse_grain(const std::string& sql, Kind* k, std::string* err) const;
  bool grain_boot(Kind& k, std::string* err);
  void grain_notice(Kind& k, const SqlTable& t, const char* op, const SqlRow* before, const SqlRow* after);
  void settle_grain(Kind& k);
  // keep_notices: what arrived meanwhile stays to be seen (a replacement built while writes go on)
  bool grain_full(Kind& k, const std::string& cause, std::string* err, bool keep_notices = false);
  SqlQueryResult replace_grain(Kind&& k, std::unique_lock<aios::hal::Mutex>& lk);
 public:
  // CREATE OR REPLACE THING of a grain Thing that exists, run by the caller outside any statement lock (so the old
  // Thing is read, and tables written, while the new one builds): false when the statement is not that one, and
  // the ordinary path takes it.
  bool try_replace_unlocked(const std::string& sql, SqlQueryResult* out);
 private:
  bool grain_keys(Kind& k, const std::vector<std::string>& keys, const std::string& cause, std::string* err);
  // every Thing of a grain kind, or (keys given) those Things only, in the order asked, by collation key
  SqlQueryResult show_grain(const Kind& k, const std::vector<std::string>* keys = nullptr);
  // The rows of SHOW THING <name> for a grain kind, built once per version of the kind; under mu_.
  std::shared_ptr<const std::vector<SqlRow>> grain_rows_locked(Kind& k);
  static std::vector<std::string> grain_columns(const Kind& k);
  static SqlRow grain_row(const Kind& k, const Kind::GInst& gi, uint64_t* last_sec, std::string* last_text);
  std::string grain_serialize(const Kind& k) const;
  void grain_restore(Kind& k);
  std::string grain_display(const Kind::GInst& g) const;
  void note_wake(Kind& k, const std::string& ck, const Kind::GInst* g);
  // One keyed Thing's new answer (grain and mirror kinds): history, version,
  // wake_at, alert. False when nothing changed.
  bool put_ginst(Kind& k, const std::string& ck, Kind::GInst fresh, const std::string& cause, uint64_t now);
  void drop_ginst(Kind& k, const std::string& ck, const std::string& why);
  bool parse_mirror(const std::string& sql, Kind* k, std::string* err) const;
  void apply_mirror(Kind& k, std::vector<ThingFleetAnswer>& answers, size_t self_kinds, const std::string& self_host);
  SqlQueryResult call_report(const std::string& sql);
  bool parse_event(const std::string& sql, Kind* k, std::string* err) const;
  SqlQueryResult show_receipts();
  // ON <machine> / ON ALL, and the controls.
  std::string machine_id() const;
  SqlQueryResult execute_local(const std::string& sql);
  SqlQueryResult control(const std::string& verb, const std::string& name);
  // Alerts: what a Thing wants someone to know. A grain Thing raises one by
  // giving itself an `alert` field and clears it by dropping the field (one
  // record when it starts, one when it changes or ends - not one per reaction);
  // a kind whose program breaks and an event that fails raise their own. Kept
  // in <things>/alerts.log, shown by SHOW THING ALERTS, printed on the
  // machine's console, and POSTed as JSON to SET THING ALERTS TO 'http://...'
  // from an outbox that survives restarts (at least once, in order).
  struct Alert { uint64_t seq = 0; std::string at, thing, key, state, text; };
  void alert(const std::string& thing, const std::string& key, const std::string& state, const std::string& text);
  void load_alerts();
  void save_alert_config();
  SqlQueryResult show_alerts(const std::string& name);
  SqlQueryResult set_alerts(const std::string& sql);
 public:
  // True when an alert is waiting for the address it goes to.
  bool alerts_waiting();
  // Send what is waiting. Called by the clock with no lock held: a slow or
  // absent receiver costs its own time, never a statement's.
  void deliver_alerts();
 private:
  std::vector<Alert> alerts_;   // newest last, bounded
  std::map<std::string, std::pair<std::string, std::string>> alert_last_;   // thing \x01 key -> (state, text)
  uint64_t alert_seq_ = 0, alert_sent_ = 0;
  size_t alert_log_lines_ = 0;
  std::string alert_url_, alert_error_;
  uint64_t alert_retry_ns_ = 0;
  int alert_backoff_s_ = 0;
  aios::hal::Mutex deliver_mu_;   // one delivery at a time
 public:
  // An engine clock: true when some Thing is due (an event's time, or a new
  // day for the ones that wait on the date). Cheap; no queries.
  bool due_now();
  // Pull the mirrors whose time has come. No lock held: a peer that is slow
  // or gone costs the pull its own time, never a statement's.
  void pull_mirrors();
  // Fire the events whose time has come. The caller holds the statement lock
  // exclusively and NOT the engine mutex: a firing writes, and its writes
  // deliver notices like any other.
  void fire_due();
 private:

  SqlDatabase& db_;
  mutable aios::hal::Mutex mu_;
  std::map<std::string, Kind> kinds_;
  std::unordered_set<std::string> watched_;   // "db.table" of every kind's links
  std::set<std::string> firing_now_;           // events firing in this process (never two firings of one)
  bool dirty_ = false;
};

}  // namespace aios::userland
