#pragma once
// A CDCL SAT solver: two watched literals with blockers, EVSIDS on a binary
// heap, phase saving, first-UIP learning with recursive minimization, restarts
// on the LBD moving averages (glucose), and learnt-clause reduction by LBD.
//
// Deterministic by construction: no clock and no randomness in any decision,
// so the same formula takes the same search on every machine - x86 or the Pi's
// ARM - and two machines that split a problem can check each other bit for
// bit. Limits (conflicts, a deadline) only decide when to stop.
//
// Every answer carries its evidence: a SAT model is checked against the
// original clauses before it is reported, and an UNSAT answer can stream a
// DRAT proof (every learnt clause added, every deleted one marked) that an
// independent checker (drat-trim) verifies. No stdio here: the proof goes to a
// callback, so the same code runs on the bare-metal kernel.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace aios::userland {

// The clause arena's memory: 2 MB-aligned and marked for transparent huge
// pages once it is large (the kernel here gives them only to memory that asks),
// so propagation over a big formula takes far fewer address-translation
// misses. Plain memory elsewhere (small arenas, the bare-metal build).
template <class T>
struct ArenaAlloc {
  using value_type = T;
  ArenaAlloc() = default;
  template <class U> ArenaAlloc(const ArenaAlloc<U>&) {}
  T* allocate(size_t n);
  void deallocate(T* p, size_t n);
  template <class U> bool operator==(const ArenaAlloc<U>&) const { return true; }
  template <class U> bool operator!=(const ArenaAlloc<U>&) const { return false; }
};

struct SatStats {
  uint64_t decisions = 0, propagations = 0, conflicts = 0, restarts = 0, mode_switches = 0;
  uint64_t learnt_literals = 0, minimized_literals = 0, reductions = 0, deleted_clauses = 0;
  uint64_t eliminated_vars = 0, resolvents = 0, elim_removed_clauses = 0, elim_steps = 0, gates = 0;
  uint64_t equivalences = 0, substituted_clauses = 0;
  uint64_t factored = 0, factor_removed_clauses = 0, factor_steps = 0;
  uint64_t shrunken_literals = 0, shrunken_blocks = 0;
  uint64_t rephases = 0, walks = 0, walk_flips = 0, walk_models = 0, reused_levels = 0;
};

class SatSolver {
 public:
  enum class Result { Sat, Unsat, Unknown };

  SatSolver();
  // Variables are 1..n; a literal is +v or -v (DIMACS). Returns false once the
  // formula is known unsatisfiable (an empty clause, or a conflict at level 0).
  bool add_clause(const std::vector<int>& lits);
  void reserve_vars(int n);
  int num_vars() const { return (int)assigns_.size() - 1; }

  // A DRAT line, without the trailing "0\n": ("", lits) adds, ("d", lits) deletes.
  using ProofSink = std::function<void(bool deletion, const std::vector<int>& lits)>;
  void set_proof(ProofSink sink) { proof_ = std::move(sink); }

  // Stop after this many conflicts (0 = no limit), or when `stop()` says so
  // (checked between conflicts; a wall-clock deadline is the caller's).
  void set_conflict_limit(uint64_t n) { conflict_limit_ = n; }
  void set_stop_check(std::function<bool()> stop) { stop_ = std::move(stop); }
  // Bounded variable elimination before the first search (on by default):
  // a variable goes when the resolvents of its clauses are no more than the
  // clauses themselves - each resolvent into the proof, each parent deleted
  // from it, and the eliminated variables filled back in after a model.
  // The first solve's assumptions are frozen (never eliminated).
  void set_elimination(bool on) { eliminate_ = on; }
  // A portfolio member's own way through the same formula: initial phases,
  // restart margin, stable-mode unit and start, decay ceiling, elimination,
  // a seeded nudge to the first branching order. Still deterministic - member
  // k takes the same path on every machine. Call before adding clauses.
  void set_diversity(uint32_t k);
  // Solve under temporary assumptions (literals forced first) - how a fleet
  // hands each machine one cube of the search space.
  Result solve(const std::vector<int>& assumptions = {});

  // After Unsat under assumptions: the assumptions (as given) that cannot all hold together - every one of them
  // took part in the refutation (a conflict over the implication graph, MiniSat's analyzeFinal). Empty when the
  // formula is unsatisfiable without any assumption.
  const std::vector<int>& failed_assumptions() const { return failed_; }

  // An unsatisfiable core of a formula: the clauses (indices into `clauses`) that cannot all hold together. Each
  // clause gets its own selector (clause or not s_i), the formula is solved assuming every s_i, and the failed
  // selectors are the core. `minimal`: then shrunk clause by clause (deletion, with the failed set of each UNSAT
  // answer as the new core) until removing any one clause makes it satisfiable - a minimal unsatisfiable subset.
  // Returns Sat when the whole formula is satisfiable, Unknown when the conflict limit stopped it (the core is then
  // the last one proven, perhaps not minimal: *minimal_proven says).
  static Result unsat_core(int vars, const std::vector<std::vector<int>>& clauses, std::vector<size_t>* core, bool minimal = true,
                           uint64_t conflict_limit = 0, bool* minimal_proven = nullptr);

  // After Sat: the value of each variable (index 1..n; true/false).
  const std::vector<bool>& model() const { return model_; }
  const SatStats& stats() const { return stats_; }
  // The formula as the search sees it (after preprocessing): live irredundant
  // clauses plus level-0 units, DIMACS. For looking at what a pass produced.
  std::vector<std::vector<int>> current_formula();

  // Parse DIMACS CNF text into clauses. False (with *err) on malformed input.
  static bool parse_dimacs(const std::string& text, int* vars, std::vector<std::vector<int>>* clauses, std::string* err);
  // True when every clause has a literal the model makes true.
  static bool check_model(const std::vector<std::vector<int>>& clauses, const std::vector<bool>& model);

 private:
  // Internal literal: 2*var + sign (sign 1 = negative). Values: 0 true, 1 false, 2 unassigned.
  using Lit = uint32_t;
  static Lit to_lit(int d) { return d > 0 ? (Lit)(2 * d) : (Lit)(2 * -d + 1); }
  static int to_dimacs(Lit l) { return (l & 1) ? -(int)(l >> 1) : (int)(l >> 1); }
  static Lit neg(Lit l) { return l ^ 1; }
  static uint32_t var(Lit l) { return l >> 1; }
  // One load, no branch: vals_ is indexed by literal (0 true, 1 false, 2 unassigned).
  uint8_t value(Lit l) const { return vals_[l]; }

  // Clauses live in one arena; a clause reference is its offset. Header:
  // [size][flags: bit0 learnt, bit1 deleted][lbd][activity as float bits]
  using CRef = uint32_t;
  static constexpr CRef kNoReason = 0xffffffffu;
  static constexpr CRef kBinary = 0x80000000u;   // on a watch's cref: a two-literal clause
  static constexpr uint32_t kHeader = 4;
  uint32_t& csize(CRef c) { return arena_[c]; }
  uint32_t& cflags(CRef c) { return arena_[c + 1]; }
  uint32_t& clbd(CRef c) { return arena_[c + 2]; }
  float cact(CRef c) const;
  void set_cact(CRef c, float a);
  Lit* clits(CRef c) { return reinterpret_cast<Lit*>(&arena_[c + kHeader]); }
  CRef alloc_clause(const std::vector<Lit>& lits, bool learnt, uint32_t lbd);

  struct Watch { CRef cref; Lit blocker; };
  void attach(CRef c);
  void new_var();
  void assign(Lit l, CRef reason);
  CRef propagate();
  void analyze(CRef confl, std::vector<Lit>* learnt, uint32_t* back_level, uint32_t* lbd);
  void analyze_final(Lit falsified);   // the assumption found false: which assumptions forced it (failed_)
  bool redundant(Lit l, uint32_t abstract_levels);
  Lit shrink_block(const std::vector<Lit>& lits, size_t from, size_t to, uint32_t level, uint32_t abstract_levels);
  void backtrack(uint32_t level);
  Lit pick_branch();
  void bump_var(uint32_t v);
  void bump_clause(CRef c);
  void reduce_db();
  void garbage_collect();
  void proof_add(const std::vector<Lit>& lits);
  void proof_del(CRef c);
  bool locked(CRef c);
  uint32_t reuse_level();
  // bound: how many more clauses a variable's resolvents may be than its own
  bool eliminate(const std::vector<Lit>& frozen_lits, uint32_t bound = 0);
  bool substitute_equivalences(const std::vector<Lit>& frozen_lits);   // SCCs of the binary implication graph
  bool factor();                  // bounded variable addition: fresh variables that shrink the formula
  void proof_del_lits(const std::vector<Lit>& lits);
  void extend_model();

  // heap of variables by activity
  void heap_insert(uint32_t v);
  void heap_up(uint32_t i);
  void heap_down(uint32_t i);
  uint32_t heap_pop();
  bool heap_less(uint32_t a, uint32_t b) const { return activity_[a] > activity_[b] || (activity_[a] == activity_[b] && a < b); }

  std::vector<uint32_t, ArenaAlloc<uint32_t>> arena_;
  uint64_t arena_wasted_ = 0;
  std::vector<CRef> originals_, learnts_;
  std::vector<std::vector<Watch>> watches_;   // by literal: clauses watching its negation's falsification
  std::vector<uint8_t> assigns_;              // by var: 0 true, 1 false, 2 unassigned
  std::vector<uint8_t> vals_;                 // by literal: 0 true, 1 false, 2 unassigned (kept with assigns_)
  std::vector<uint8_t> phase_;                // saved polarity: 1 = negative
  std::vector<uint32_t> level_;
  std::vector<CRef> reason_;
  std::vector<Lit> trail_;
  std::vector<uint32_t> trail_lim_;
  size_t qhead_ = 0;
  std::vector<double> activity_;
  double var_inc_ = 1.0, var_decay_ = 0.8;
  // tunables (set_diversity)
  uint32_t diversity_ = 0;
  double restart_margin_ = 1.25, var_decay_max_ = 0.95;
  uint64_t stable_unit_ = 1024;
  int initial_phase_ = 1;        // 1 false first, 0 true first, 2 alternate by variable
  double activity_nudge_ = 0;    // > 0: seeded initial activities (branching order)
  float cla_inc_ = 1.0f;
  std::vector<uint32_t> heap_, heap_pos_;     // heap_pos_ = UINT32_MAX when not in the heap
  std::vector<uint8_t> seen_;
  std::vector<uint32_t> shrink_mark_;   // by var: stamped while one level's block is shrunk
  uint32_t shrink_stamp_ = 0;
  std::vector<uint32_t> lbd_mark_;
  uint32_t lbd_stamp_ = 0;
  std::vector<Lit> analyze_stack_, analyze_toclear_;
  bool ok_ = true;
  std::vector<bool> model_;
  SatStats stats_;
  // restarts (glucose): moving averages of learnt LBD
  double lbd_fast_ = 0, lbd_slow_ = 0;
  uint64_t conflicts_since_restart_ = 0;
  // Modes (CaDiCaL/kissat): "focused" restarts on the LBD averages; "stable"
  // restarts rarely (Luby x kStableUnit conflicts) and steers by the target
  // phase - the longest conflict-free assignment seen - which is what finds
  // models of hard satisfiable formulas. The modes alternate, each length
  // doubling, all counted in conflicts (deterministic).
  bool stable_ = false;
  uint64_t mode_switch_at_ = 1000, mode_len_ = 1000;
  uint64_t luby_index_ = 0, stable_restart_at_ = 0;
  std::vector<uint8_t> target_;   // by var: 0 true, 1 false, 2 unset
  size_t target_size_ = 0;
  // Rephasing (kissat): every so often in stable mode the saved phases are
  // reset - to the best assignment seen, to what a local search (probSAT)
  // reaches from them, to the original, to its inverse - in a fixed cycle.
  std::vector<uint8_t> best_;     // by var, like target_, never reset by a mode switch
  size_t best_size_ = 0;
  uint64_t rephase_at_ = 1000, rephase_count_ = 0;
  uint64_t walk_props_mark_ = 0;   // propagations when the last walk ran (its budget is a share of the work since)
  void rephase();
  bool walk();                    // true: the walk satisfied every clause (phases hold the model)
  void update_target();
  void switch_mode();
  uint64_t next_reduce_ = 2000, reduce_inc_ = 300;
  uint64_t conflict_limit_ = 0;
  bool eliminate_ = true, eliminated_done_ = false;
  std::vector<uint8_t> eliminated_;            // by var
  struct Elim { uint32_t var; uint32_t begin, end; };   // its positive clauses, flattened into elim_lits_
  std::vector<Elim> elims_;
  std::vector<Lit> elim_lits_;                 // [len, lits...] per clause
  // Equivalent literals merged before search: repr_[v] is the literal that
  // stands for v (2v itself when v is its own representative). The model
  // fills a merged variable from its representative; a later elimination of
  // the representative is undone first (elims_ are latest-first).
  std::vector<Lit> repr_;
  std::function<bool()> stop_;
  ProofSink proof_;
  std::vector<int> failed_;
};

}  // namespace aios::userland
