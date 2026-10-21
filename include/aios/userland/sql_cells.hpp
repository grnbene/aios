#pragma once
#include <functional>
#include "aios/userland/portable_int128.hpp"  // MSVC has no __int128
// Cells: active data the size of a bit. A cell set keeps, per key of one
// table, a few small values that follow every row change exactly - counts,
// sums, a min or max, and bits made of them - and acts when a bit flips:
//
//   CREATE CELLS picked ON pickingfreeze KEY orderno (
//     lines  COUNT,
//     done   COUNT WHEN qtypicked >= qty,
//     ready  BIT ALL qtypicked >= qty
//   ) ON RISE ready DO UPDATE salesorders SET orderstatusid = 21 WHERE orderno = KEY TO st_maria
//
// Nothing is stored but the values: no rows, no documents. A changed row is
// evaluated once, by the engine's own evaluator (SqlRowExpr), and moves its
// key's values by what it added and took away - no query runs. A Thing that
// needs another key's answer (allocation, a sorted board) is still a Thing;
// a fact about one key is a cell.
//
// A bit is ALL c (the key has rows and every one meets c), ANY c (some row
// does) or NONE c (it has rows and none does). ON RISE b runs once when b
// turns on for a key, ON FALL b when it turns off; both see the bit as it
// stood when the statement or replicated transaction finished, so a bit that
// flickers inside one transaction does not act. Acts run on their own thread,
// outside the statement lock, in one transaction on the named profile
// (connections.ini, things_write=1), KEY replaced by the key - or, with
// NOTIFY '<url>' in place of DO .. TO, as an HTTP POST of the edge:
//   {"cells":"picked","key":"1281815","bit":"ready","edge":"rise",
//    "id":42,"values":{"lines":"9","done":"9","ready":"1"}}
// An act is never lost to a crash: queued acts are journaled with the set
// and run again at start until they commit - so a kill between an act's
// COMMIT and its journal entry going repeats it. Make DO statements
// idempotent; a NOTIFY receiver drops a repeated id.
// (a socket server can hand that straight to the browsers). By default a
// bit already on at CREATE counts as acted on; ACT EXISTING acts on those too.
//
//   SHOW CELLS                        every set: keys, bits on, bytes, acts
//   SHOW CELLS name [WHERE bit]       the values per key (only keys with bit on)
//   SHOW CELLS name KEY 'k'           one key
//   SHOW CELLS name ACTS              the last acts: when, key, ok / error
//   DROP CELLS name
#include "aios/hal/sync.hpp"
#include <cstdint>
#include <deque>
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
class SqlRowExpr;
struct SqlTable;
struct SqlRow;
struct SqlQueryResult;

class CellEngine {
 public:
  // An edge handler in this process: NOTIFY 'local:<name>' calls it with the JSON an http NOTIFY
  // would POST, on the set's worker thread, instead of going out on the network. False (with err)
  // counts as a failed act, retried at the key's next change like a non-2xx answer.
  using LocalHandler = std::function<bool(const std::string& body, std::string* err)>;
  static void register_local(const std::string& name, LocalHandler handler);
  static void unregister_local(const std::string& name);

  explicit CellEngine(SqlDatabase& db);
  ~CellEngine();
  bool watches(const SqlTable& t) const;
  void on_row_change(const SqlTable& t, const SqlRow* before, const SqlRow* after);
  void on_table_reset(const SqlTable& t);
  bool has_pending() const;
  // Under the statement lock (SqlDatabase::things_settle): MIN/MAX keys that
  // lost their extreme are re-read, and every key whose bits moved is
  // compared with what was last acted on; the acts go to the worker.
  void settle();
  bool handles(const std::string& sql) const;
  SqlQueryResult execute(const std::string& sql);
  void load();   // after the tables load, before the redo log replays
  void save();

 private:
  struct Dec {   // an exact decimal: v / 10^scale
    AIOS_INT128 v = 0;
    int scale = 0;
  };
  struct Cell {
    std::string name;
    enum Kind { Count, Sum, Min, Max, BitAll, BitAny, BitNone } kind = Count;
    std::string expr_text, when_text;         // SUM(expr) / MIN / MAX; WHEN or the bit's condition
    std::unique_ptr<SqlRowExpr> expr, when;
    bool is_bit() const { return kind == BitAll || kind == BitAny || kind == BitNone; }
  };
  struct Act {
    size_t bit = 0;          // index into cells
    bool rise = true;
    std::string sql;         // statements, ';'-separated, KEY outside quotes = the key
    std::string target;      // connection profile
    std::string notify;      // NOTIFY '<url>' instead: the edge POSTed there as JSON
  };
  struct KeyState {
    int64_t rows = 0;
    std::vector<int64_t> n;       // per cell: rows meeting WHEN / the bit's condition (non-NULL ones for SUM/MIN/MAX)
    std::vector<Dec> sum;         // per SUM cell slot
    std::vector<std::string> ext; // per MIN/MAX cell slot: the extreme as the row printed it
    uint64_t bits = 0;            // bit i = cells' i-th bit, as computed now
    uint64_t acted = 0;           // bit i = what the acts last saw (RISE ran, FALL not since)
  };
  struct ActLog {
    uint64_t wall_ns = 0;
    std::string key, what, result;
  };
  struct Set {
    std::string name, declaration, db, table, table_key, key_col;
    int key_index = -1;
    std::vector<Cell> cells;
    std::vector<size_t> bit_cells;        // indexes of the bit cells, in order (bit i)
    std::vector<int> slot;                // per cell: its index into sum / ext, -1 for none
    std::vector<Act> acts;
    bool act_existing = false;
    std::unordered_map<std::string, KeyState> keys;
    std::unordered_set<std::string> dirty;        // keys whose bits may have moved
    std::set<std::string> minmax_dirty;           // keys whose MIN/MAX lost its extreme
    bool reset = false;                           // the table was replaced: rebuild
    uint64_t changes = 0, acts_run = 0, acts_failed = 0, created_ns = 0;
    std::deque<ActLog> log;                       // newest last, bounded
    // Acts queued and not yet committed, on disk with the set: a kill before
    // the worker runs one leaves it here, and load queues it again - an act is
    // run at least once (twice only if killed between its COMMIT and this).
    std::map<uint64_t, std::pair<std::string, std::pair<size_t, bool>>> inflight;   // id -> key, (bit, rise)
  };
  struct Job {
    std::string set, key, what, sql, target, notify;
    size_t bit = 0;
    bool rise = true;
    uint64_t id = 0;   // its inflight entry
  };

  bool parse_create(const std::string& sql, Set* s, std::string* err);
  bool compile(Set& s, std::string* err);
  bool rebuild(Set& s, std::string* err);       // every key from the table, one GROUP BY
  void apply(Set& s, const std::string& key, KeyState& k, const SqlRow& row, int sign);
  uint64_t bits_of(const Set& s, const KeyState& k) const;
  KeyState blank_state(const Set& s) const;
  bool recompute_extremes(Set& s, const std::string& key, std::string* err);
  void queue_edges(Set& s, const std::string& key, KeyState& k);
  bool run_job(const Job& j, std::string* err);
  bool post_edge(const Job& j, std::string* err);
  void worker_loop();
  size_t bytes_of(const Set& s) const;
  std::string cells_dir() const;
  void write_set(const Set& s);
  SqlQueryResult show(const std::string& sql);

  SqlDatabase& db_;
  mutable aios::hal::Mutex mu_;
  std::map<std::string, std::unique_ptr<Set>> sets_;
  std::unordered_set<std::string> watched_;   // "db.table" lower case
  bool dirty_ = false;
  uint64_t next_job_id_ = 1;
  void queue_job(Set& s, const std::string& key, size_t bit_index, bool rise, uint64_t id);

  aios::hal::Mutex jobs_mu_;
  aios::hal::CondVar jobs_cv_;
  std::deque<Job> jobs_;
  bool stop_ = false;
  aios::hal::Thread worker_;
};

}  // namespace aios::userland
