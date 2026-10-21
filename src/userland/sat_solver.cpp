#include "aios/userland/sat_solver.hpp"

#include <algorithm>
#include <cstdlib>
#include <new>
#include <cmath>
#include <cstring>
#include <queue>

#if defined(__linux__) && !defined(AIOS_METAL)
#include <sys/mman.h>
#endif

namespace aios::userland {

template <class T>
T* ArenaAlloc<T>::allocate(size_t n) {
  const size_t bytes = n * sizeof(T);
#if defined(__linux__) && !defined(AIOS_METAL)
  constexpr size_t kHuge = 2u << 20;
  if (bytes >= kHuge) {
    const size_t rounded = (bytes + kHuge - 1) & ~(kHuge - 1);
    void* p = std::aligned_alloc(kHuge, rounded);
    if (p) { (void)madvise(p, rounded, MADV_HUGEPAGE); return static_cast<T*>(p); }
  }
#endif
  void* p = std::malloc(bytes ? bytes : 1);
#if !defined(AIOS_METAL) && !defined(__wasi__)
  if (!p) throw std::bad_alloc();   // (the bare-metal image has no exceptions)
#endif
  return static_cast<T*>(p);
}
template <class T>
void ArenaAlloc<T>::deallocate(T* p, size_t) { std::free(p); }
template struct ArenaAlloc<uint32_t>;

namespace {
constexpr uint32_t kLearnt = 1, kDeleted = 2;
constexpr uint32_t kNotInHeap = 0xffffffffu;
constexpr float kClaDecay = 0.999f;
// glucose restarts: restart when the recent learnt LBD runs this much above
// the long-run average
constexpr uint64_t kRestartMinConflicts = 50;
// The Luby sequence 1 1 2 1 1 2 4 1 1 2 1 1 2 4 8 ... (i from 1).
uint64_t luby(uint64_t i) {
  uint64_t k = 1;
  while ((1ull << k) - 1 < i) ++k;
  while (i != (1ull << k) - 1) {
    i -= (1ull << (k - 1)) - 1;
    k = 1;
    while ((1ull << k) - 1 < i) ++k;
  }
  return 1ull << (k - 1);
}
}  // namespace

void SatSolver::set_diversity(uint32_t k) {
  diversity_ = k;
  switch (k % 8) {
    case 0: break;                                                  // the default
    case 1: initial_phase_ = 0; break;                              // true first
    case 2: stable_ = true; mode_switch_at_ = mode_len_ = 2000; luby_index_ = 1; stable_restart_at_ = stable_unit_; break;
    case 3: restart_margin_ = 1.10; var_decay_max_ = 0.99; break;   // restart eagerly, remember longer
    case 4: eliminate_ = false; initial_phase_ = 2; break;
    case 5: stable_unit_ = 512; activity_nudge_ = 1e-3; break;
    case 6: restart_margin_ = 1.40; stable_unit_ = 2048; initial_phase_ = 0; break;
    case 7: activity_nudge_ = 1e-3; stable_ = true; mode_switch_at_ = mode_len_ = 4000; luby_index_ = 1; stable_restart_at_ = stable_unit_; break;
  }
  if (k >= 8) activity_nudge_ = 1e-3;                               // beyond eight: seeded orders
}

SatSolver::SatSolver() {
  // variable 0 is unused (DIMACS starts at 1)
  assigns_.push_back(2);
  vals_.push_back(2); vals_.push_back(2);
  phase_.push_back(1);
  level_.push_back(0);
  reason_.push_back(kNoReason);
  activity_.push_back(0);
  heap_pos_.push_back(kNotInHeap);
  seen_.push_back(0);
  target_.push_back(2);
  watches_.resize(2);
}

// The assignment at a conflict, before backjumping: when it reaches further
// than any before (since the last mode switch), it is the new target.
void SatSolver::update_target() {
  const size_t n = trail_lim_.empty() ? trail_.size() : trail_lim_.back();   // what held before the last decision
  if (n > best_size_) {
    best_size_ = n;
    if (best_.size() < assigns_.size()) best_.resize(assigns_.size(), 2);
    for (size_t i = 0; i < n; ++i) best_[var(trail_[i])] = (uint8_t)(trail_[i] & 1);
  }
  if (n <= target_size_) return;
  target_size_ = n;
  for (size_t i = 0; i < n; ++i) target_[var(trail_[i])] = (uint8_t)(trail_[i] & 1);
}

void SatSolver::switch_mode() {
  stable_ = !stable_;
  stats_.mode_switches++;
  mode_len_ *= 2;
  mode_switch_at_ = stats_.conflicts + mode_len_;
  target_size_ = 0;
  if (stable_) {
    luby_index_ = 1;
    stable_restart_at_ = stats_.conflicts + stable_unit_ * luby(luby_index_);
  }
  backtrack(0);
}

float SatSolver::cact(CRef c) const {
  float f;
  std::memcpy(&f, &arena_[c + 3], sizeof f);
  return f;
}
void SatSolver::set_cact(CRef c, float a) { std::memcpy(&arena_[c + 3], &a, sizeof a); }

void SatSolver::new_var() {
  const uint32_t v = (uint32_t)assigns_.size();
  assigns_.push_back(2);
  vals_.push_back(2); vals_.push_back(2);
  phase_.push_back(initial_phase_ == 2 ? (uint8_t)(v & 1) : (uint8_t)initial_phase_);   // false first, unless diversified
  level_.push_back(0);
  reason_.push_back(kNoReason);
  {
    double a0 = 0;
    if (activity_nudge_ > 0) {   // a deterministic hash of (member, variable)
      uint64_t x = ((uint64_t)diversity_ << 32) ^ v ^ 0x9e3779b97f4a7c15ull;
      x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull; x ^= x >> 33;
      a0 = activity_nudge_ * (double)(x & 0xffff) / 65536.0;
    }
    activity_.push_back(a0);
  }
  heap_pos_.push_back(kNotInHeap);
  seen_.push_back(0);
  target_.push_back(2);
  watches_.resize(2 * (v + 1));
  lbd_mark_.resize(v + 1, 0);
  shrink_mark_.resize(v + 1, 0);
  heap_insert(v);
}

void SatSolver::reserve_vars(int n) {
  while (num_vars() < n) new_var();
}

SatSolver::CRef SatSolver::alloc_clause(const std::vector<Lit>& lits, bool learnt, uint32_t lbd) {
  const CRef c = (CRef)arena_.size();
  arena_.push_back((uint32_t)lits.size());
  arena_.push_back(learnt ? kLearnt : 0);
  arena_.push_back(lbd);
  arena_.push_back(0);
  set_cact(c, 0.0f);
  for (Lit l : lits) arena_.push_back(l);
  return c;
}

// A two-literal clause's watch carries kBinary: its other literal is the
// blocker, so propagating it never touches the clause itself.
void SatSolver::attach(CRef c) {
  Lit* l = clits(c);
  const CRef tag = csize(c) == 2 ? (c | kBinary) : c;
  watches_[neg(l[0])].push_back({tag, l[1]});
  watches_[neg(l[1])].push_back({tag, l[0]});
}

void SatSolver::proof_add(const std::vector<Lit>& lits) {
  if (!proof_) return;
  std::vector<int> d;
  d.reserve(lits.size());
  for (Lit l : lits) d.push_back(to_dimacs(l));
  proof_(false, d);
}

std::vector<std::vector<int>> SatSolver::current_formula() {
  std::vector<std::vector<int>> out;
  for (size_t i = 0; i < trail_.size() && (trail_lim_.empty() || i < trail_lim_[0]); ++i) out.push_back({to_dimacs(trail_[i])});
  for (CRef c : originals_) {
    if (cflags(c) & kDeleted) continue;
    std::vector<int> d;
    for (uint32_t i = 0; i < csize(c); ++i) d.push_back(to_dimacs(clits(c)[i]));
    out.push_back(std::move(d));
  }
  return out;
}

void SatSolver::proof_del_lits(const std::vector<Lit>& lits) {
  if (!proof_) return;
  std::vector<int> d;
  d.reserve(lits.size());
  for (Lit l : lits) d.push_back(to_dimacs(l));
  proof_(true, d);
}

void SatSolver::proof_del(CRef c) {
  if (!proof_) return;
  std::vector<int> d;
  const uint32_t n = csize(c);
  for (uint32_t i = 0; i < n; ++i) d.push_back(to_dimacs(clits(c)[i]));
  proof_(true, d);
}

bool SatSolver::add_clause(const std::vector<int>& in) {
  if (!ok_) return false;
  std::vector<Lit> lits;
  lits.reserve(in.size());
  for (int d : in) {
    if (d == 0) continue;
    const int v = d > 0 ? d : -d;
    while (num_vars() < v) new_var();
    lits.push_back(to_lit(d));
  }
  // Clauses are added at level 0 only (before or between solves).
  std::sort(lits.begin(), lits.end());
  std::vector<Lit> kept;
  Lit prev = 0xffffffffu;
  for (Lit l : lits) {
    if (l == prev) continue;                          // duplicate literal
    if (prev != 0xffffffffu && l == neg(prev)) return true;   // tautology (x and -x sort adjacent)
    const uint8_t v = value(l);
    if (v == 0 && level_[var(l)] == 0) return true;   // already satisfied
    if (v == 1 && level_[var(l)] == 0) { prev = l; continue; }   // false literal drops out
    kept.push_back(l);
    prev = l;
  }
  if (kept.size() != lits.size() && proof_ && !kept.empty()) proof_add(kept);
  if (kept.empty()) {
    ok_ = false;
    if (proof_) proof_(false, {});
    return false;
  }
  if (kept.size() == 1) {
    assign(kept[0], kNoReason);
    if (propagate() != kNoReason) {
      ok_ = false;
      if (proof_) proof_(false, {});
      return false;
    }
    return true;
  }
  const CRef c = alloc_clause(kept, false, 0);
  originals_.push_back(c);
  attach(c);
  return true;
}

void SatSolver::assign(Lit l, CRef reason) {
  const uint32_t v = var(l);
  assigns_[v] = (uint8_t)(l & 1);
  vals_[l] = 0;
  vals_[l ^ 1] = 1;
  level_[v] = (uint32_t)trail_lim_.size();
  reason_[v] = reason;
  trail_.push_back(l);
}

// Unit propagation over the two watched literals. Returns the conflicting
// clause, or kNoReason.
SatSolver::CRef SatSolver::propagate() {
  CRef confl = kNoReason;
  while (qhead_ < trail_.size()) {
    const Lit p = trail_[qhead_++];   // p became true: clauses watching -p may be unit
    std::vector<Watch>& ws = watches_[p];
    stats_.propagations++;
    size_t i = 0, j = 0;
    const size_t n = ws.size();
    while (i < n) {
      const Watch w = ws[i];
      const uint8_t bv = value(w.blocker);
      if (bv == 0) { ws[j++] = ws[i++]; continue; }
      if (w.cref & kBinary) {
        ws[j++] = ws[i++];
        const CRef bc = w.cref & ~kBinary;
        if (bv == 1) {   // both literals false
          confl = bc;
          qhead_ = trail_.size();
          while (i < n) ws[j++] = ws[i++];
          break;
        }
        // The reason is the binary's other literal itself (tagged), not the
        // clause: a binary implication never touches clause memory.
        assign(w.blocker, kBinary | neg(p));
        continue;
      }
      const CRef c = w.cref;
      Lit* lits = clits(c);
      const Lit false_lit = neg(p);
      if (lits[0] == false_lit) { lits[0] = lits[1]; lits[1] = false_lit; }
      ++i;
      const Lit first = lits[0];
      if (first != w.blocker && value(first) == 0) { ws[j++] = {c, first}; continue; }
      // look for a new literal to watch, resuming where the last search on
      // this clause stopped (CaDiCaL's saved position, kept in the flags'
      // upper half): a long clause is not rescanned from its start each time
      const uint32_t sz = csize(c);
      bool moved = false;
      uint32_t start = sz > 3 ? (cflags(c) >> 16) : 2;
      if (start < 2 || start >= sz) start = 2;
      for (uint32_t pass = 0, k = start; pass < 2 && !moved; ++pass, k = 2) {
        const uint32_t end = pass == 0 ? sz : start;
        for (; k < end; ++k) {
          if (value(lits[k]) != 1) {
            lits[1] = lits[k];
            lits[k] = false_lit;
            watches_[neg(lits[1])].push_back({c, first});
            if (sz > 3) cflags(c) = (cflags(c) & 0xffffu) | (std::min<uint32_t>(k, 0xffffu) << 16);
            moved = true;
            break;
          }
        }
      }
      if (moved) continue;
      ws[j++] = {c, first};
      if (value(first) == 1) {
        confl = c;
        qhead_ = trail_.size();
        while (i < n) ws[j++] = ws[i++];
      } else {
        assign(first, c);
      }
    }
    ws.resize(j);
    if (confl != kNoReason) break;
  }
  return confl;
}

void SatSolver::bump_var(uint32_t v) {
  if ((activity_[v] += var_inc_) > 1e100) {
    for (size_t i = 1; i < activity_.size(); ++i) activity_[i] *= 1e-100;
    var_inc_ *= 1e-100;
  }
  if (heap_pos_[v] != kNotInHeap) heap_up(heap_pos_[v]);
}

void SatSolver::bump_clause(CRef c) {
  const float a = cact(c) + cla_inc_;
  set_cact(c, a);
  if (a > 1e20f) {
    for (CRef l : learnts_) set_cact(l, cact(l) * 1e-20f);
    cla_inc_ *= 1e-20f;
  }
}

// First-UIP learning. The learnt clause's first literal is the asserting one;
// the second is from the highest remaining level (the backjump level).
void SatSolver::analyze(CRef confl, std::vector<Lit>* learnt, uint32_t* back_level, uint32_t* lbd) {
  learnt->clear();
  learnt->push_back(0);   // room for the asserting literal
  int path = 0;
  Lit p = 0xffffffffu;
  size_t idx = trail_.size();
  const uint32_t cur = (uint32_t)trail_lim_.size();
  do {
    auto see = [&](Lit q) {
      const uint32_t v = var(q);
      if (seen_[v] || level_[v] == 0) return;
      seen_[v] = 1;
      bump_var(v);
      if (level_[v] >= cur) ++path;
      else learnt->push_back(q);
    };
    if (p != 0xffffffffu && (confl & kBinary)) {
      see((Lit)(confl & ~kBinary));   // a binary reason: its one other literal
    } else {
      if (cflags(confl) & kLearnt) bump_clause(confl);
      Lit* lits = clits(confl);
      const uint32_t sz = csize(confl);
      for (uint32_t k = (p == 0xffffffffu ? 0 : 1); k < sz; ++k) see(lits[k]);
    }
    // the next seen literal on the trail, walking back
    while (!seen_[var(trail_[--idx])]) {}
    p = trail_[idx];
    confl = reason_[var(p)];
    seen_[var(p)] = 0;
    --path;
    // a reason clause keeps its implied literal first; propagate() arranged it
    if (path > 0 && confl != kNoReason && !(confl & kBinary) && clits(confl)[0] != p) {
      Lit* rl = clits(confl);
      for (uint32_t k = 1; k < csize(confl); ++k)
        if (rl[k] == p) { std::swap(rl[0], rl[k]); break; }
    }
  } while (path > 0);
  (*learnt)[0] = neg(p);

  // recursive minimization (MiniSat): drop literals implied by the rest
  analyze_toclear_.assign(learnt->begin(), learnt->end());
  uint32_t abstract = 0;
  for (size_t i = 1; i < learnt->size(); ++i) abstract |= 1u << (level_[var((*learnt)[i])] & 31);
  size_t j = 1;
  for (size_t i = 1; i < learnt->size(); ++i) {
    const Lit l = (*learnt)[i];
    if (reason_[var(l)] == kNoReason || !redundant(l, abstract)) (*learnt)[j++] = l;
  }
  stats_.minimized_literals += learnt->size() - j;
  learnt->resize(j);

  // Shrinking (Feng & Bacchus 2020; kissat's shrink=3): the literals of one
  // lower decision level are replaced by that level's own UIP for them - the
  // one trail literal every one of them was implied through - when the walk
  // to it only meets literals of that level, ones already in the clause, or
  // ones minimization shows redundant. Four literals of a level become one.
  if (learnt->size() > 2) {
    std::vector<Lit>& L = *learnt;
    // (no stable_sort: it allocates a merge buffer on every conflict)
    std::sort(L.begin() + 1, L.end(), [&](Lit a, Lit b) {
      return level_[var(a)] != level_[var(b)] ? level_[var(a)] > level_[var(b)] : a < b;
    });
    size_t out = 1;
    for (size_t i = 1; i < L.size();) {
      const uint32_t lv = level_[var(L[i])];
      size_t k = i;
      while (k < L.size() && level_[var(L[k])] == lv) ++k;
      if (k - i >= 2) {
        const Lit u = shrink_block(L, i, k, lv, abstract);
        if (u != 0xffffffffu) {
          stats_.shrunken_literals += k - i - 1;
          stats_.shrunken_blocks++;
          L[out++] = u;
          i = k;
          continue;
        }
      }
      for (size_t q = i; q < k; ++q) L[out++] = L[q];
      i = k;
    }
    L.resize(out);
  }
  for (Lit l : analyze_toclear_) seen_[var(l)] = 0;

  // backjump level: the highest level among the rest, moved to position 1
  *back_level = 0;
  if (learnt->size() > 1) {
    size_t mx = 1;
    for (size_t i = 2; i < learnt->size(); ++i)
      if (level_[var((*learnt)[i])] > level_[var((*learnt)[mx])]) mx = i;
    std::swap((*learnt)[1], (*learnt)[mx]);
    *back_level = level_[var((*learnt)[1])];
  }
  // LBD: distinct decision levels in the clause
  ++lbd_stamp_;
  uint32_t n = 0;
  for (Lit l : *learnt) {
    const uint32_t lv = level_[var(l)];
    if (lv < lbd_mark_.size() && lbd_mark_[lv] != lbd_stamp_) { lbd_mark_[lv] = lbd_stamp_; ++n; }
  }
  *lbd = n;
}

// One level's block of the learnt clause, lits[from, to) (all at `level`, at
// least two): walk that level's trail back from its end; each marked literal
// is either the last one open - the block's UIP, returned negated - or is
// resolved through its reason. Anything else fails (0xffffffff).
SatSolver::Lit SatSolver::shrink_block(const std::vector<Lit>& lits, size_t from, size_t to, uint32_t level, uint32_t abstract) {
  ++shrink_stamp_;
  for (size_t i = from; i < to; ++i) shrink_mark_[var(lits[i])] = shrink_stamp_;
  uint32_t open = (uint32_t)(to - from);
  const size_t begin = trail_lim_[level - 1];
  const size_t end = level < trail_lim_.size() ? trail_lim_[level] : trail_.size();
  for (size_t k = end; k-- > begin;) {
    const Lit t = trail_[k];
    const uint32_t v = var(t);
    if (shrink_mark_[v] != shrink_stamp_) continue;
    if (--open == 0) return neg(t);
    const CRef r = reason_[v];
    if (r == kNoReason) return 0xffffffffu;   // a decision, with others still open
    Lit bin[1];
    const Lit* rl;
    uint32_t rn;
    if (r & kBinary) { bin[0] = (Lit)(r & ~kBinary); rl = bin; rn = 1; }
    else { rl = clits(r); rn = csize(r); }
    for (uint32_t i = 0; i < rn; ++i) {
      const Lit q = rl[i];
      const uint32_t u = var(q);
      if (u == v || level_[u] == 0) continue;
      if (level_[u] == level) {
        if (shrink_mark_[u] != shrink_stamp_) { shrink_mark_[u] = shrink_stamp_; ++open; }
        continue;
      }
      if (level_[u] > level) return 0xffffffffu;   // (cannot happen for a reason; be safe)
      if (seen_[u]) continue;                      // in the clause, or already shown redundant
      if (reason_[u] != kNoReason && redundant(q, abstract)) continue;
      return 0xffffffffu;
    }
  }
  return 0xffffffffu;
}

bool SatSolver::redundant(Lit l, uint32_t abstract) {
  analyze_stack_.clear();
  analyze_stack_.push_back(l);
  const size_t top = analyze_toclear_.size();
  while (!analyze_stack_.empty()) {
    const CRef c = reason_[var(analyze_stack_.back())];
    analyze_stack_.pop_back();
    Lit bin[2];
    const Lit* lits;
    uint32_t sz;
    if (c & kBinary) { bin[0] = 0; bin[1] = (Lit)(c & ~kBinary); lits = bin; sz = 2; }   // a binary reason: its other literal
    else { lits = clits(c); sz = csize(c); }
    for (uint32_t k = 1; k < sz; ++k) {
      const Lit q = lits[k];
      const uint32_t v = var(q);
      if (seen_[v] || level_[v] == 0) continue;
      if (reason_[v] != kNoReason && (abstract & (1u << (level_[v] & 31)))) {
        seen_[v] = 1;
        analyze_stack_.push_back(q);
        analyze_toclear_.push_back(q);
      } else {
        for (size_t i = top; i < analyze_toclear_.size(); ++i) seen_[var(analyze_toclear_[i])] = 0;
        analyze_toclear_.resize(top);
        return false;
      }
    }
  }
  return true;
}

void SatSolver::backtrack(uint32_t lv) {
  if (trail_lim_.size() <= lv) return;
  for (size_t i = trail_.size(); i-- > trail_lim_[lv];) {
    const uint32_t v = var(trail_[i]);
    assigns_[v] = 2;
    vals_[2 * v] = vals_[2 * v + 1] = 2;
    phase_[v] = (uint8_t)(trail_[i] & 1);   // phase saving
    reason_[v] = kNoReason;
    if (heap_pos_[v] == kNotInHeap) heap_insert(v);
  }
  trail_.resize(trail_lim_[lv]);
  trail_lim_.resize(lv);
  qhead_ = trail_.size();
}

// Trail reuse on a restart (van der Tak, Ramos, Heule 2011): the decisions
// the solver would make again anyway - each more active than the best
// unassigned variable - stay, with everything they propagated. On a deep
// circuit a full restart re-propagated tens of thousands of literals to
// arrive where it already was.
uint32_t SatSolver::reuse_level() {
  while (!heap_.empty()) {
    const uint32_t top = heap_[0];
    if (assigns_[top] == 2 && !(top < eliminated_.size() && eliminated_[top])) break;
    heap_pop();   // assigned or gone: pick_branch would skip it too (backtrack puts it back)
  }
  if (heap_.empty()) return (uint32_t)trail_lim_.size();
  const double next = activity_[heap_[0]];
  for (uint32_t l = 0; l < trail_lim_.size(); ++l) {
    const uint32_t d = var(trail_[trail_lim_[l]]);
    if (activity_[d] < next) { stats_.reused_levels += l; return l; }
  }
  stats_.reused_levels += (uint32_t)trail_lim_.size();
  return (uint32_t)trail_lim_.size();
}

SatSolver::Lit SatSolver::pick_branch() {
  while (!heap_.empty()) {
    const uint32_t v = heap_pop();
    if (assigns_[v] == 2 && !(v < eliminated_.size() && eliminated_[v])) return (Lit)(2 * v + (stable_ && target_[v] != 2 ? target_[v] : phase_[v]));
  }
  return 0xffffffffu;
}

bool SatSolver::locked(CRef c) {
  const Lit l = clits(c)[0];
  return value(l) == 0 && reason_[var(l)] == c;
}

// Keep the glue clauses (LBD <= 2) and the better half of the rest by LBD,
// then activity; a clause that is a current reason stays.
void SatSolver::reduce_db() {
  stats_.reductions++;
  std::vector<CRef> cand;
  cand.reserve(learnts_.size());
  for (CRef c : learnts_) if (clbd(c) > 2 && !locked(c)) cand.push_back(c);
  std::sort(cand.begin(), cand.end(), [&](CRef a, CRef b) {
    if (clbd(a) != clbd(b)) return clbd(a) > clbd(b);
    if (cact(a) != cact(b)) return cact(a) < cact(b);
    return a < b;
  });
  const size_t drop = cand.size() / 2;
  for (size_t i = 0; i < drop; ++i) {
    proof_del(cand[i]);
    cflags(cand[i]) |= kDeleted;
    arena_wasted_ += kHeader + csize(cand[i]);
    stats_.deleted_clauses++;
  }
  std::vector<CRef> keep;
  keep.reserve(learnts_.size() - drop);
  for (CRef c : learnts_) if (!(cflags(c) & kDeleted)) keep.push_back(c);
  learnts_.swap(keep);
  // drop the watches of deleted clauses
  for (auto& ws : watches_) {
    size_t j = 0;
    for (size_t i = 0; i < ws.size(); ++i) if (!(cflags(ws[i].cref & ~kBinary) & kDeleted)) ws[j++] = ws[i];
    ws.resize(j);
  }
  if (arena_wasted_ > arena_.size() / 2) garbage_collect();
}

// Compact the arena, remapping every reference.
void SatSolver::garbage_collect() {
  std::vector<uint32_t, ArenaAlloc<uint32_t>> fresh;
  fresh.reserve(arena_.size() - arena_wasted_);
  auto move = [&](CRef c) -> CRef {
    const CRef nc = (CRef)fresh.size();
    const uint32_t n = kHeader + csize(c);
    fresh.insert(fresh.end(), arena_.begin() + c, arena_.begin() + c + n);
    arena_[c + 1] |= kDeleted;              // mark as moved
    arena_[c + 2] = nc;                     // forwarding address in the lbd slot
    return nc;
  };
  // reasons first need their forwarding, so collect every live clause
  std::vector<CRef> live;
  live.reserve(originals_.size() + learnts_.size());
  for (CRef c : originals_) live.push_back(c);
  for (CRef c : learnts_) live.push_back(c);
  std::vector<uint32_t> lbd_of(live.size());
  for (size_t i = 0; i < live.size(); ++i) lbd_of[i] = clbd(live[i]);
  std::vector<CRef> remap(live.size());
  for (size_t i = 0; i < live.size(); ++i) remap[i] = move(live[i]);
  for (size_t i = 0; i < live.size(); ++i) fresh[remap[i] + 2] = lbd_of[i];   // restore the real lbd in the copy
  auto fwd = [&](CRef c) -> CRef { return arena_[c + 2]; };
  for (auto& ws : watches_) for (auto& w : ws) w.cref = fwd(w.cref & ~kBinary) | (w.cref & kBinary);
  for (size_t v = 1; v < reason_.size(); ++v)
    if (reason_[v] != kNoReason && !(reason_[v] & kBinary) && assigns_[v] != 2) reason_[v] = fwd(reason_[v]);
  for (size_t i = 0; i < originals_.size(); ++i) originals_[i] = remap[i];
  for (size_t i = 0; i < learnts_.size(); ++i) learnts_[i] = remap[originals_.size() + i];
  arena_.swap(fresh);
  arena_wasted_ = 0;
}

void SatSolver::heap_insert(uint32_t v) {
  heap_pos_[v] = (uint32_t)heap_.size();
  heap_.push_back(v);
  heap_up(heap_pos_[v]);
}
void SatSolver::heap_up(uint32_t i) {
  const uint32_t v = heap_[i];
  while (i > 0) {
    const uint32_t p = (i - 1) / 2;
    if (!heap_less(v, heap_[p])) break;
    heap_[i] = heap_[p];
    heap_pos_[heap_[i]] = i;
    i = p;
  }
  heap_[i] = v;
  heap_pos_[v] = i;
}
void SatSolver::heap_down(uint32_t i) {
  const uint32_t v = heap_[i];
  const uint32_t n = (uint32_t)heap_.size();
  for (;;) {
    uint32_t c = 2 * i + 1;
    if (c >= n) break;
    if (c + 1 < n && heap_less(heap_[c + 1], heap_[c])) ++c;
    if (!heap_less(heap_[c], v)) break;
    heap_[i] = heap_[c];
    heap_pos_[heap_[i]] = i;
    i = c;
  }
  heap_[i] = v;
  heap_pos_[v] = i;
}
uint32_t SatSolver::heap_pop() {
  const uint32_t v = heap_[0];
  heap_pos_[v] = kNotInHeap;
  const uint32_t last = heap_.back();
  heap_.pop_back();
  if (!heap_.empty()) {
    heap_[0] = last;
    heap_pos_[last] = 0;
    heap_down(0);
  }
  return v;
}

// `a` is an assumption the trail makes false. The assumptions behind that: walking back from it over the reasons,
// every decision met is an assumption (they are the first decision levels, and a search decision never comes before
// all of them are placed), and the conflict is a clause over them.
void SatSolver::analyze_final(Lit a) {
  failed_.assign(1, to_dimacs(a));
  if (trail_lim_.empty() || level_[var(a)] == 0) return;   // false by the formula alone: it fails by itself
  seen_[var(a)] = 1;
  for (size_t i = trail_.size(); i-- > trail_lim_[0];) {
    const uint32_t x = var(trail_[i]);
    if (!seen_[x]) continue;
    seen_[x] = 0;
    const CRef r = reason_[x];
    if (r == kNoReason) {
      if (level_[x] > 0) failed_.push_back(to_dimacs(trail_[i]));   // an assumption, true on the trail
      continue;
    }
    if (r & kBinary) {
      const Lit o = (Lit)(r & ~kBinary);
      if (level_[var(o)] > 0) seen_[var(o)] = 1;
      continue;
    }
    Lit* lits = clits(r);
    for (uint32_t k = 1; k < csize(r); ++k)
      if (level_[var(lits[k])] > 0) seen_[var(lits[k])] = 1;
  }
  seen_[var(a)] = 0;
  std::sort(failed_.begin(), failed_.end());
  failed_.erase(std::unique(failed_.begin(), failed_.end()), failed_.end());
}

SatSolver::Result SatSolver::solve(const std::vector<int>& assumptions) {
  model_.clear();
  failed_.clear();
  if (!ok_) return Result::Unsat;
  for (int a : assumptions) { const int v = a > 0 ? a : -a; while (num_vars() < v) new_var(); }
  std::vector<Lit> assume;
  for (int a : assumptions) assume.push_back(to_lit(a));
  if (propagate() != kNoReason) {
    ok_ = false;
    if (proof_) proof_(false, {});
    return Result::Unsat;
  }
  // An assumption on a variable the first solve eliminated has nothing to
  // stand on: the clauses that mentioned it are gone.
  for (Lit a : assume) if (var(a) < eliminated_.size() && eliminated_[var(a)]) return Result::Unknown;
  if (eliminate_ && !eliminated_done_) {
    eliminated_done_ = true;
    // Factoring first: elimination run before it merges the very clauses a
    // grid is made of (a colouring's vertex clauses into width-22 resolvents)
    // and leaves nothing to factor. Then elimination at its strict bound.
    // Where factoring found no grid, elimination's bound grows (kissat's
    // schedule: 0, 1, 2 .. 16 more clauses per variable) while a round still
    // takes variables away - on a factored formula that growth undoes the
    // re-encoding (coloring instances: solved in 1-3 s strict, not in 40 s).
    bool fine = substitute_equivalences(assume) && factor() && eliminate(assume);
    for (uint32_t bound = 1; fine && !stats_.factored && bound <= 16; bound *= 2) {
      const uint64_t before = stats_.eliminated_vars;
      fine = eliminate(assume, bound);
      if (fine && stats_.eliminated_vars == before) break;
    }
    if (!fine) {
      ok_ = false;
      if (proof_) proof_(false, {});
      return Result::Unsat;
    }
  }
  std::vector<Lit> learnt;
  const uint64_t start_conflicts = stats_.conflicts;
  for (;;) {
    const CRef confl = propagate();
    if (confl != kNoReason) {
      stats_.conflicts++;
      conflicts_since_restart_++;
      if (trail_lim_.empty()) {
        ok_ = false;
        if (proof_) proof_(false, {});
        return Result::Unsat;
      }
      if (stable_) update_target();
      uint32_t back = 0, lbd = 0;
      analyze(confl, &learnt, &back, &lbd);
      // (backjumping below the assumptions is fine: the decision loop puts
      // them back, and one now forced false answers UNSAT for this cube)
      backtrack(back);
      proof_add(learnt);
      stats_.learnt_literals += learnt.size();
      if (learnt.size() == 1) {
        backtrack(0);
        assign(learnt[0], kNoReason);
      } else {
        const CRef c = alloc_clause(learnt, true, lbd);
        learnts_.push_back(c);
        attach(c);
        bump_clause(c);
        assign(learnt[0], c);
      }
      var_inc_ /= var_decay_;
      cla_inc_ /= kClaDecay;
      if (stats_.conflicts % 5000 == 0 && var_decay_ < var_decay_max_) var_decay_ += 0.01;
      // glucose moving averages of learnt LBD
      lbd_fast_ += (lbd - lbd_fast_) / 32.0;
      lbd_slow_ += (lbd - lbd_slow_) / 4096.0;
      if (stats_.conflicts < 32) lbd_fast_ = lbd_slow_ = lbd;
      // The limit is checked here, after each learnt clause, and not only at a
      // decision: a run of conflicts with no decision between them used to
      // carry a --conflicts=200 run on to 202.
      if (conflict_limit_ && stats_.conflicts - start_conflicts >= conflict_limit_) { backtrack(0); return Result::Unknown; }
      continue;
    }
    if (conflict_limit_ && stats_.conflicts - start_conflicts >= conflict_limit_) { backtrack(0); return Result::Unknown; }
    if (stop_ && (stats_.conflicts & 255) == 0 && stop_()) { backtrack(0); return Result::Unknown; }
    if (stats_.conflicts >= mode_switch_at_) switch_mode();
    if (stable_ && stats_.conflicts >= rephase_at_) { rephase(); continue; }
    // restart
    if (!stable_ && conflicts_since_restart_ >= kRestartMinConflicts && lbd_fast_ > restart_margin_ * lbd_slow_) {
      stats_.restarts++;
      conflicts_since_restart_ = 0;
      backtrack(reuse_level());
    } else if (stable_ && stats_.conflicts >= stable_restart_at_) {
      stats_.restarts++;
      conflicts_since_restart_ = 0;
      stable_restart_at_ = stats_.conflicts + stable_unit_ * luby(++luby_index_);
      backtrack(reuse_level());
    }
    if (stats_.conflicts >= next_reduce_) {
      next_reduce_ = stats_.conflicts + 2000 + reduce_inc_ * stats_.reductions;
      reduce_db();
    }
    // assumptions first, as their own decision levels
    Lit next = 0xffffffffu;
    while (trail_lim_.size() < assume.size()) {
      const Lit a = assume[trail_lim_.size()];
      if (value(a) == 0) { trail_lim_.push_back((uint32_t)trail_.size()); continue; }   // already true: an empty level
      if (value(a) == 1) { analyze_final(a); backtrack(0); return Result::Unsat; }     // forced false: this cube is UNSAT
      next = a;
      break;
    }
    if (next == 0xffffffffu) {
      next = pick_branch();
      if (next == 0xffffffffu) {
        model_.assign(assigns_.size(), false);
        for (size_t v = 1; v < assigns_.size(); ++v) model_[v] = assigns_[v] == 0;
        extend_model();
        backtrack(0);
        return Result::Sat;
      }
      stats_.decisions++;
    }
    trail_lim_.push_back((uint32_t)trail_.size());
    assign(next, kNoReason);
  }
}

bool SatSolver::parse_dimacs(const std::string& text, int* vars, std::vector<std::vector<int>>* clauses, std::string* err) {
  *vars = 0;
  clauses->clear();
  size_t i = 0, n = text.size();
  std::vector<int> cur;
  long declared_vars = -1, declared_clauses = -1;
  while (i < n) {
    const char c = text[i];
    if (c == 'c') { while (i < n && text[i] != '\n') ++i; continue; }   // comment
    // SATLIB ends its files with a '%' line and a stray "0": the formula is
    // over. Read on, that "0" would be an empty clause - every file UNSAT.
    if (c == '%' && (i == 0 || text[i - 1] == '\n')) break;
    if (c == 'p') {
      const size_t e = text.find('\n', i);
      const std::string line = text.substr(i, e == std::string::npos ? std::string::npos : e - i);
      char fmt[16] = {0};
      int used = 0;
      if (std::sscanf(line.c_str(), "p %15s %ld %ld%n", fmt, &declared_vars, &declared_clauses, &used) != 3 || std::strcmp(fmt, "cnf") != 0) {
        *err = "bad problem line: " + line;
        return false;
      }
      // Only the header itself: clauses may follow on the same line (a whole
      // formula on one line is legal DIMACS). Skipping to the newline dropped
      // them, and an UNSAT formula solved as the empty one - SAT.
      i += (size_t)used;
      continue;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
      bool negv = false;
      if (text[i] == '-') { negv = true; ++i; }
      long v = 0;
      if (i >= n || text[i] < '0' || text[i] > '9') { *err = "bad literal"; return false; }
      while (i < n && text[i] >= '0' && text[i] <= '9') { v = v * 10 + (text[i] - '0'); ++i; if (v > 0x3fffffff) { *err = "variable out of range"; return false; } }
      if (v == 0) { clauses->push_back(cur); cur.clear(); continue; }
      const int lit = negv ? -(int)v : (int)v;
      if (v > *vars) *vars = (int)v;
      cur.push_back(lit);
      continue;
    }
    ++i;
  }
  if (!cur.empty()) clauses->push_back(cur);   // a last clause without its 0
  if (declared_vars > *vars) *vars = (int)declared_vars;
  // Fewer clauses than the header promised is a cut-off file, and solving what
  // is left answers a different question. More is tolerated, as other solvers do.
  if (declared_clauses >= 0 && (long)clauses->size() < declared_clauses) {
    *err = "the problem line declares " + std::to_string(declared_clauses) + " clauses but only " +
           std::to_string(clauses->size()) + " were read";
    return false;
  }
  return true;
}

bool SatSolver::check_model(const std::vector<std::vector<int>>& clauses, const std::vector<bool>& model) {
  for (const auto& cl : clauses) {
    bool sat = false;
    for (int l : cl) {
      const size_t v = (size_t)(l > 0 ? l : -l);
      if (v < model.size() && model[v] == (l > 0)) { sat = true; break; }
    }
    if (!sat) return false;
  }
  return true;
}


// ---- bounded variable elimination ----------------------------------------

namespace {
constexpr size_t kElimMaxOcc = 16;         // a side with more occurrences is not tried
constexpr size_t kElimMaxResolvent = 32;   // a longer resolvent stops the variable
constexpr uint64_t kElimBudget = 40000000; // literal visits in all; deterministic
}  // namespace

bool SatSolver::eliminate(const std::vector<Lit>& frozen_lits, uint32_t bound) {
  const uint64_t steps_at_start = stats_.elim_steps;
  const uint32_t nv = (uint32_t)assigns_.size();
  if (eliminated_.size() < nv) eliminated_.resize(nv, 0);   // merged variables are already marked
  std::vector<uint8_t> frozen(nv, 0);
  for (Lit l : frozen_lits) frozen[var(l)] = 1;
  // Occurrences of every live original clause (none is learnt yet).
  std::vector<std::vector<CRef>> occ(2 * (size_t)nv);
  auto live = [&](CRef c) { return !(cflags(c) & kDeleted); };
  auto satisfied = [&](CRef c) {
    for (uint32_t i = 0; i < csize(c); ++i) if (value(clits(c)[i]) == 0) return true;
    return false;
  };
  auto drop = [&](CRef c) {
    proof_del(c);
    cflags(c) |= kDeleted;
    arena_wasted_ += kHeader + csize(c);
    stats_.elim_removed_clauses++;
  };
  for (CRef c : originals_) {
    if (!live(c)) continue;
    if (satisfied(c)) { drop(c); continue; }
    for (uint32_t i = 0; i < csize(c); ++i) occ[clits(c)[i]].push_back(c);
  }
  // Candidates: fewest pairs first, ties by variable - the same order on
  // every machine.
  std::vector<uint32_t> cand;
  for (uint32_t v = 1; v < nv; ++v)
    if (assigns_[v] == 2 && !frozen[v] && !eliminated_[v]) cand.push_back(v);
  auto count_live = [&](Lit l) {
    size_t n = 0;
    for (CRef c : occ[l]) if (live(c)) ++n;
    return n;
  };
  std::vector<uint64_t> cost(nv, 0);
  for (uint32_t v : cand) cost[v] = (uint64_t)count_live(2 * v) * count_live(2 * v + 1);
  std::sort(cand.begin(), cand.end(), [&](uint32_t a, uint32_t b) { return cost[a] != cost[b] ? cost[a] < cost[b] : a < b; });

  std::vector<uint32_t> mark(2 * (size_t)nv, 0);
  uint32_t stamp = 0;
  std::vector<uint32_t> pmark(2 * (size_t)nv, 0), pidx(2 * (size_t)nv, 0);   // gate detection
  uint32_t pstamp = 0;
  std::vector<std::vector<Lit>> resolvents;
  std::vector<Lit> r;
  for (uint32_t v : cand) {
    if (stats_.elim_steps - steps_at_start > kElimBudget) break;
    if (assigns_[v] != 2) continue;
    const Lit lpos = 2 * v, lneg = 2 * v + 1;
    std::vector<CRef> P, N;
    for (CRef c : occ[lpos]) if (live(c) && !satisfied(c)) P.push_back(c);
    for (CRef c : occ[lneg]) if (live(c) && !satisfied(c)) N.push_back(c);
    if (P.empty() && N.empty()) continue;
    // Is v the output of a gate the encoding spelled out? x = AND(a..) is
    // (x | -a | -b ..) with (-x | a), (-x | b) ..; x = OR(..) the mirror image;
    // x = a is the two-literal case. With a gate, only resolvents across the
    // gate boundary are needed (gate x non-gate): the rest follow from the
    // definition - far fewer, so far more circuit signals can go.
    std::vector<uint8_t> gP(P.size(), 0), gN(N.size(), 0);
    auto find_and = [&](const std::vector<CRef>& L, const std::vector<CRef>& B, Lit lpivot, Lit bpivot,
                        std::vector<uint8_t>& gL, std::vector<uint8_t>& gB) -> bool {
      ++pstamp;
      for (size_t j = 0; j < B.size(); ++j) {
        if (csize(B[j]) != 2) continue;
        const Lit* bl = clits(B[j]);
        const Lit other = bl[0] == bpivot ? bl[1] : bl[0];
        pmark[other] = pstamp;
        pidx[other] = (uint32_t)j;
      }
      for (size_t i = 0; i < L.size(); ++i) {
        const CRef c = L[i];
        bool all = csize(c) >= 2;
        for (uint32_t k = 0; k < csize(c) && all; ++k) {
          const Lit l = clits(c)[k];
          if (l == lpivot) continue;
          all = pmark[neg(l)] == pstamp;
        }
        if (!all) continue;
        gL[i] = 1;
        for (uint32_t k = 0; k < csize(c); ++k) {
          const Lit l = clits(c)[k];
          if (l != lpivot) gB[pidx[neg(l)]] = 1;
        }
        return true;
      }
      return false;
    };
    const bool gate = find_and(P, N, lpos, lneg, gP, gN) || find_and(N, P, lneg, lpos, gN, gP);
    const size_t occ_limit = gate ? 4 * kElimMaxOcc : kElimMaxOcc;
    if (P.size() > occ_limit || N.size() > occ_limit) continue;
    if (gate) stats_.gates++;
    // every resolvent, or stop as soon as there would be too many
    resolvents.clear();
    bool ok = true;
    for (size_t pi = 0; pi < P.size() && ok; ++pi) {
      const CRef c = P[pi];
      for (size_t ni = 0; ni < N.size(); ++ni) {
        const CRef d = N[ni];
        if (gate && gP[pi] == gN[ni]) continue;   // gate x gate, or non-gate x non-gate: not needed
        ++stamp;
        r.clear();
        bool taut = false;
        auto take = [&](CRef x, Lit pivot) {
          for (uint32_t i = 0; i < csize(x) && !taut; ++i) {
            const Lit l = clits(x)[i];
            if (l == pivot) continue;
            const uint8_t val = value(l);
            if (val == 0) { taut = true; return; }   // satisfied at level 0
            if (val == 1) continue;                  // false at level 0: drops out
            if (mark[neg(l)] == stamp) { taut = true; return; }
            if (mark[l] == stamp) continue;
            mark[l] = stamp;
            r.push_back(l);
          }
          stats_.elim_steps += csize(x);
        };
        take(c, lpos);
        if (!taut) take(d, lneg);
        if (taut) continue;
        if (r.size() > kElimMaxResolvent) { ok = false; break; }
        resolvents.push_back(r);
        if (resolvents.size() > P.size() + N.size() + bound) { ok = false; break; }
      }
    }
    if (!ok) continue;
    // Eliminate: resolvents in (each is RUP from its two parents), then the
    // parents out; the positive side is kept to fill v back in later.
    for (auto& res : resolvents) {
      if (res.empty()) return false;   // the formula is unsatisfiable
      proof_add(res);
      stats_.resolvents++;
      if (res.size() == 1) { if (value(res[0]) == 2) assign(res[0], kNoReason); else if (value(res[0]) == 1) return false; continue; }
      const CRef nc = alloc_clause(res, false, 0);
      originals_.push_back(nc);
      for (Lit l : res) occ[l].push_back(nc);
    }
    Elim e{v, (uint32_t)elim_lits_.size(), 0};
    for (CRef c : P) {
      elim_lits_.push_back(csize(c));
      for (uint32_t i = 0; i < csize(c); ++i) elim_lits_.push_back(clits(c)[i]);
    }
    e.end = (uint32_t)elim_lits_.size();
    elims_.push_back(e);
    for (CRef c : P) drop(c);
    for (CRef c : N) drop(c);
    eliminated_[v] = 1;
    stats_.eliminated_vars++;
  }
  // Watches afresh over what is left, and the level-0 trail propagated
  // through them again (a watch may now sit on a literal already false).
  for (auto& ws : watches_) ws.clear();
  std::vector<CRef> keep;
  keep.reserve(originals_.size());
  for (CRef c : originals_) if (live(c)) { keep.push_back(c); attach(c); }
  originals_.swap(keep);
  qhead_ = 0;
  if (propagate() != kNoReason) return false;
  if (arena_wasted_ > arena_.size() / 2) garbage_collect();
  return true;
}

// Eliminated variables back into a model, latest eliminated first: each is
// false unless one of its positive clauses needs it - and then true breaks
// none of its negative ones (their resolvents with that clause hold).
void SatSolver::extend_model() {
  // eliminated variables first (they may be representatives), then the
  // merged ones from their representatives
  for (size_t k = elims_.size(); k-- > 0;) {
    const Elim& e = elims_[k];
    model_[e.var] = false;
    for (uint32_t i = e.begin; i < e.end;) {
      const uint32_t len = elim_lits_[i];
      bool sat = false;
      for (uint32_t j = 1; j <= len; ++j) {
        const Lit l = elim_lits_[i + j];
        if (var(l) == e.var) continue;
        if (model_[var(l)] == !(l & 1)) { sat = true; break; }
      }
      if (!sat) { model_[e.var] = true; break; }
      i += len + 1;
    }
  }
  for (size_t v = 1; v < repr_.size(); ++v) {
    const Lit r = repr_[v];
    if (r == (Lit)(2 * v)) continue;
    model_[v] = model_[var(r)] == !(r & 1);
  }
}


// ---- bounded variable addition (Manthey, Heule, Biere 2012) ----------------
// A grid of clauses {m | C' : m in M, C' in Q} - every literal of M with every
// cofactor of Q - is |M|*|Q| clauses; with a fresh x it is |M| + |Q|:
// (x | C') for each cofactor and (-x | m) for each literal (resolving on x
// gives the grid back). The pairwise at-most-one constraints of a colouring,
// scheduling or pigeonhole encoding are such grids, and the re-encoded form is
// what makes their refutations short: a definition resolution cannot make
// (extended resolution, in the small). In the proof, each (x | C') is RAT on x
// (nothing holds -x yet), each (-x | m) RAT on -x (its resolvents are the grid,
// still present), and the grid is deleted after.
bool SatSolver::factor() {
  constexpr uint32_t kMaxSize = 5;             // clauses considered (kissat's default too)
  constexpr uint64_t kBudgetCap = 80000000;    // literal visits, the whole pass, at most
  // Clause literals flat (lits_of[id] .. + fsize[id]), sizes and liveness in
  // their own arrays: the candidate loop filters on those without touching
  // the literals, and the index is open addressing over clause ids.
  std::vector<Lit> flat;
  std::vector<uint32_t> fstart, fsize;
  std::vector<uint8_t> falive;
  std::vector<CRef> fcref;
  std::vector<std::vector<uint32_t>> occ(watches_.size());
  std::vector<uint32_t> count(watches_.size(), 0);
  auto hkey = [](const std::vector<Lit>& l) {
    uint64_t h = 1469598103934665603ull;
    for (Lit x : l) { h ^= x; h *= 1099511628211ull; }
    return h;
  };
  std::vector<uint32_t> table(1024, UINT32_MAX);   // ids by hash, linear probing; grows at half full
  std::vector<uint64_t> fhash;
  size_t used = 0;
  auto same = [&](uint32_t id, const std::vector<Lit>& sorted) {
    if (fsize[id] != sorted.size()) return false;
    return std::equal(sorted.begin(), sorted.end(), flat.begin() + fstart[id]);
  };
  auto place = [&](uint32_t id) {
    const size_t mask = table.size() - 1;
    size_t h = (size_t)fhash[id] & mask;
    while (table[h] != UINT32_MAX) h = (h + 1) & mask;
    table[h] = id;
  };
  auto insert = [&](std::vector<Lit> lits, CRef cref) {
    std::sort(lits.begin(), lits.end());
    const uint32_t id = (uint32_t)fsize.size();
    for (Lit l : lits) { occ[l].push_back(id); count[l]++; }
    fstart.push_back((uint32_t)flat.size());
    fsize.push_back((uint32_t)lits.size());
    falive.push_back(1);
    fcref.push_back(cref);
    fhash.push_back(hkey(lits));
    flat.insert(flat.end(), lits.begin(), lits.end());
    if (2 * (++used) > table.size()) {
      table.assign(table.size() * 2, UINT32_MAX);
      for (uint32_t i = 0; i < fsize.size(); ++i) place(i);
    } else {
      place(id);
    }
  };
  auto find = [&](const std::vector<Lit>& sorted) -> int64_t {
    const uint64_t k = hkey(sorted);
    const size_t mask = table.size() - 1;
    for (size_t h = (size_t)k & mask; table[h] != UINT32_MAX; h = (h + 1) & mask) {
      const uint32_t id = table[h];
      if (fhash[id] == k && falive[id] && same(id, sorted)) return id;
    }
    return -1;
  };
  std::vector<Lit> tmp;
  auto lits_of = [&](uint32_t id) { return &flat[fstart[id]]; };
  auto kill = [&](uint32_t id) {
    falive[id] = 0;
    const Lit* l = lits_of(id);
    for (uint32_t i = 0; i < fsize[id]; ++i) count[l[i]]--;
    tmp.assign(l, l + fsize[id]);
    proof_del_lits(tmp);
  };
  for (CRef c : originals_) {
    if (cflags(c) & kDeleted) continue;
    const uint32_t sz = csize(c);
    if (sz > kMaxSize) continue;
    bool assigned = false;
    for (uint32_t i = 0; i < sz && !assigned; ++i) assigned = value(clits(c)[i]) != 2;
    if (!assigned) insert(std::vector<Lit>(clits(c), clits(c) + sz), c);
  }
  if (fsize.empty()) return true;
  // a million visits plus five per literal: a colouring's grids need a few
  // tens of thousands; a million-literal circuit no longer spends more on
  // factoring than on solving
  const uint64_t kBudget = std::min<uint64_t>(kBudgetCap, 1000000ull + 5ull * flat.size());
  std::priority_queue<std::pair<uint32_t, Lit>> pq;   // most occurrences first; stale entries skipped
  for (Lit l = 2; l < count.size(); ++l) if (count[l] >= 3) pq.push({count[l], l});
  std::vector<uint32_t> mark(watches_.size(), 0);
  uint32_t stamp = 0;
  std::vector<uint32_t> cmark;   // by clause id: in the grid being grown
  uint32_t cstamp = 0;
  uint64_t steps = 0;
  std::vector<std::pair<Lit, uint32_t>> pairs;
  std::vector<Lit> mlit, cof, nw;
  std::vector<uint32_t> mcls, next;
  auto gain = [](size_t m, size_t q) { return (long)(m * q) - (long)m - (long)q; };
  while (!pq.empty() && steps < kBudget) {
    const auto top = pq.top();
    pq.pop();
    const Lit l = top.second;
    if (top.first != count[l]) { if (count[l] >= 3) pq.push({count[l], l}); continue; }
    mlit.assign(1, l);
    mcls.clear();
    if (cmark.size() < fsize.size()) cmark.resize(fsize.size() + fsize.size() / 2, 0);
    for (uint32_t id : occ[l]) if (falive[id]) mcls.push_back(id);
    if (mcls.size() < 2) continue;
    // Every match once: for each clause C holding l, the clauses D equal to C
    // with l swapped for one other literal e - the pair (e, C). A match
    // depends on C and e alone, so the greedy rounds below only filter this
    // list; they never scan occurrences again.
    pairs.clear();
    for (uint32_t cid : mcls) {
      const Lit* C = lits_of(cid);
      const uint32_t csz = fsize[cid];
      Lit lmin = 0;
      for (uint32_t i = 0; i < csz; ++i) if (C[i] != l && (!lmin || count[C[i]] < count[lmin])) lmin = C[i];
      if (!lmin) continue;
      ++stamp;
      for (uint32_t i = 0; i < csz; ++i) if (C[i] != l) mark[C[i]] = stamp;
      auto& ol = occ[lmin];
      size_t keep = 0;
      for (size_t oi = 0; oi < ol.size(); ++oi) {
        const uint32_t did = ol[oi];
        if (!falive[did]) continue;   // (dropped from the list as we go)
        ol[keep++] = did;
        ++steps;
        if (did == cid || fsize[did] != csz) continue;
        const Lit* D = lits_of(did);
        steps += csz;
        Lit extra = 0;
        bool two = false;
        for (uint32_t i = 0; i < csz; ++i) if (mark[D[i]] != stamp) { if (extra) { two = true; break; } extra = D[i]; }
        if (two || !extra || extra == l || var(extra) == var(l)) continue;
        pairs.push_back({extra, cid});
      }
      ol.resize(keep);
      if (steps >= kBudget) break;
    }
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    // greedy rows: the literal matching most of the grid's clauses, while the
    // grid still shrinks the formula more
    ++cstamp;
    for (uint32_t cid : mcls) cmark[cid] = cstamp;
    for (;;) {
      Lit best = 0;
      size_t best_n = 0;
      for (size_t i = 0; i < pairs.size();) {
        size_t k = i, n = 0;
        const Lit e = pairs[i].first;
        while (k < pairs.size() && pairs[k].first == e) { n += cmark[pairs[k].second] == cstamp; ++k; }
        steps += k - i;
        bool clash = false;
        for (Lit m : mlit) clash |= var(m) == var(e);
        if (!clash && n > best_n) { best_n = n; best = e; }
        i = k;
      }
      if (!best || gain(mlit.size() + 1, best_n) <= gain(mlit.size(), mcls.size())) break;
      next.clear();
      for (const auto& pr : pairs) if (pr.first == best && cmark[pr.second] == cstamp) next.push_back(pr.second);
      ++cstamp;
      for (uint32_t cid : next) cmark[cid] = cstamp;
      mlit.push_back(best);
      mcls.swap(next);
    }
    if (mlit.size() < 2 || gain(mlit.size(), mcls.size()) <= 0) continue;
    // apply: x fresh
    new_var();
    const Lit x = (Lit)(2 * (uint32_t)num_vars());
    occ.resize(watches_.size());
    count.resize(watches_.size(), 0);
    mark.resize(watches_.size(), 0);
    std::vector<std::vector<Lit>> cofs;
    for (uint32_t cid : mcls) {
      cof.clear();
      const Lit* C = lits_of(cid);
      for (uint32_t i = 0; i < fsize[cid]; ++i) if (C[i] != l) cof.push_back(C[i]);
      cofs.push_back(cof);
      nw.assign(1, x);
      nw.insert(nw.end(), cof.begin(), cof.end());
      proof_add(nw);   // x first: the RAT pivot
      insert(nw, kNoReason);
    }
    for (Lit m : mlit) {
      nw = {neg(x), m};
      proof_add(nw);   // -x first
      insert(nw, kNoReason);
    }
    for (Lit m : mlit)
      for (const auto& cf : cofs) {
        nw = cf;
        nw.push_back(m);
        std::sort(nw.begin(), nw.end());
        const int64_t id = find(nw);
        if (id >= 0) { kill((uint32_t)id); stats_.factor_removed_clauses++; }
      }
    stats_.factored++;
    for (Lit m : mlit) if (count[m] >= 3) pq.push({count[m], m});
    for (const auto& cf : cofs) for (Lit y : cf) if (count[y] >= 3) pq.push({count[y], y});
    if (count[neg(x)] >= 3) pq.push({count[neg(x)], neg(x)});
  }
  stats_.factor_steps += steps;
  if (!stats_.factored) return true;
  // into the arena: new clauses in, factored-away originals out (their
  // deletions are in the proof already)
  for (uint32_t id = 0; id < fsize.size(); ++id) {
    if (fcref[id] != kNoReason && !falive[id]) { cflags(fcref[id]) |= kDeleted; arena_wasted_ += kHeader + csize(fcref[id]); }
    if (fcref[id] == kNoReason && falive[id]) { tmp.assign(lits_of(id), lits_of(id) + fsize[id]); originals_.push_back(alloc_clause(tmp, false, 0)); }
  }
  size_t j = 0;
  for (CRef c : originals_) if (!(cflags(c) & kDeleted)) originals_[j++] = c;
  originals_.resize(j);
  for (auto& ws : watches_) ws.clear();
  for (CRef c : originals_) attach(c);
  qhead_ = 0;
  if (propagate() != kNoReason) return false;
  if (arena_wasted_ > arena_.size() / 2) garbage_collect();
  return true;
}

// ---- rephasing and local search --------------------------------------------

namespace {
// xorshift64*: the walk's only randomness, seeded from (member, round) - the
// same flips on every machine.
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed ? seed : 0x9e3779b97f4a7c15ull) {}
  uint64_t next() { s ^= s >> 12; s ^= s << 25; s ^= s >> 27; return s * 0x2545f4914f6cdd1dull; }
  double unit() { return (double)(next() >> 11) * (1.0 / 9007199254740992.0); }
};
}  // namespace

// The next phases, in kissat's cycle: best, walk, original, best, walk, inverted.
void SatSolver::rephase() {
  backtrack(0);
  stats_.rephases++;
  const uint64_t which = rephase_count_++ % 6;
  const uint32_t nv = (uint32_t)assigns_.size();
  if (best_.size() < nv) best_.resize(nv, 2);
  switch (which) {
    case 0: case 3:
      for (uint32_t v = 1; v < nv; ++v) if (best_[v] != 2) phase_[v] = best_[v];
      break;
    case 1: case 4:
      walk();
      break;
    case 2:
      for (uint32_t v = 1; v < nv; ++v) phase_[v] = 1;
      break;
    case 5:
      for (uint32_t v = 1; v < nv; ++v) phase_[v] ^= 1;
      break;
  }
  // the target starts over from the new phases; the next rephase comes later
  for (uint32_t v = 1; v < nv; ++v) target_[v] = phase_[v];
  target_size_ = 0;
  best_size_ = 0;
  rephase_at_ = stats_.conflicts + 1000 * (rephase_count_ + 1);
}

// probSAT over the irredundant clauses from the saved phases, a bounded number
// of flips; the best assignment it reaches becomes the phases. Level-0
// assignments stay fixed.
bool SatSolver::walk() {
  stats_.walks++;
  const uint32_t nv = (uint32_t)assigns_.size();
  std::vector<uint8_t> val(nv, 0);   // 1 true, 0 false
  for (uint32_t v = 1; v < nv; ++v) val[v] = assigns_[v] == 2 ? (uint8_t)!phase_[v] : (uint8_t)(assigns_[v] == 0);
  auto lit_true = [&](Lit l) { return val[var(l)] == (uint8_t)!(l & 1); };
  // the clauses: live originals not satisfied at level 0
  std::vector<CRef> cls;
  for (CRef c : originals_) {
    if (cflags(c) & kDeleted) continue;
    bool sat0 = false;
    for (uint32_t i = 0; i < csize(c) && !sat0; ++i) sat0 = value(clits(c)[i]) == 0 && level_[var(clits(c)[i])] == 0;
    if (!sat0) cls.push_back(c);
  }
  if (cls.empty()) return false;
  std::vector<std::vector<uint32_t>> occ(2 * (size_t)nv);   // literal -> clause indexes
  for (uint32_t k = 0; k < cls.size(); ++k)
    for (uint32_t i = 0; i < csize(cls[k]); ++i) occ[clits(cls[k])[i]].push_back(k);
  std::vector<uint32_t> ntrue(cls.size(), 0);
  std::vector<uint32_t> unsat, where(cls.size(), UINT32_MAX);
  auto add_unsat = [&](uint32_t k) { where[k] = (uint32_t)unsat.size(); unsat.push_back(k); };
  auto del_unsat = [&](uint32_t k) { const uint32_t p = where[k], last = unsat.back(); unsat[p] = last; where[last] = p; unsat.pop_back(); where[k] = UINT32_MAX; };
  for (uint32_t k = 0; k < cls.size(); ++k) {
    for (uint32_t i = 0; i < csize(cls[k]); ++i) if (lit_true(clits(cls[k])[i])) ntrue[k]++;
    if (!ntrue[k]) add_unsat(k);
  }
  // probSAT's break-count weights: (eps + break)^-cb, cb by clause width
  double avg = 0;
  for (CRef c : cls) avg += csize(c);
  avg /= (double)cls.size();
  const double cb = avg <= 3.5 ? 2.06 : avg <= 4.5 ? 3.0 : avg <= 5.5 ? 3.7 : 5.1;
  std::vector<double> weight(64);
  for (size_t b = 0; b < weight.size(); ++b) weight[b] = std::pow(0.9 + (double)b, -cb);
  Rng rng(((uint64_t)diversity_ << 40) ^ (rephase_count_ * 0x9e3779b97f4a7c15ull) ^ 0x5851f42d4c957f2dull);
  std::vector<uint8_t> best(val);
  size_t best_unsat = unsat.size();
  // Effort: a tenth of the propagation work since the last walk, counted in
  // clause visits (plus a small floor) - a walk that pays on random formulas
  // must not eat a third of the time on a big circuit where it never lands
  // (bmc-ibm-12: walk() was 37% of all samples).
  // The larger of: that share, and a base proportional to the formula but
  // capped - a small random formula keeps the long walk that finds its
  // models (uf250: ~1M visits), a 195k-clause circuit gets at most 1.1M
  // instead of the ~200M it used to (each visit there is a cache miss).
  const uint64_t base = std::min<uint64_t>(1000ull * cls.size(), 1100000ull);
  const uint64_t budget = std::max<uint64_t>((stats_.propagations - walk_props_mark_) / 10 + 20000, base);
  walk_props_mark_ = stats_.propagations;
  uint64_t ticks = 0;
  std::vector<double> probs;
  for (uint64_t f = 0; ticks < budget && !unsat.empty(); ++f) {
    const uint32_t k = unsat[(size_t)(rng.next() % unsat.size())];
    const CRef c = cls[k];
    const uint32_t sz = csize(c);
    probs.assign(sz, 0);
    double sum = 0;
    for (uint32_t i = 0; i < sz; ++i) {
      const Lit l = clits(c)[i];
      if (assigns_[var(l)] != 2) continue;   // fixed at level 0
      // break: clauses the flip would leave with no true literal (those whose
      // only true literal is the one this flip turns false: ~l is true now)
      uint32_t brk = 0;
      for (uint32_t k2 : occ[neg(l)]) if (ntrue[k2] == 1) brk++;
      ticks += occ[neg(l)].size() + 1;
      probs[i] = weight[std::min<size_t>(brk, weight.size() - 1)];
      sum += probs[i];
    }
    if (sum <= 0) continue;
    double pick = rng.unit() * sum;
    uint32_t chosen = 0;
    for (uint32_t i = 0; i < sz; ++i) { pick -= probs[i]; if (pick <= 0 && probs[i] > 0) { chosen = i; break; } chosen = i; }
    const Lit make = clits(c)[chosen];   // becomes true
    const uint32_t v = var(make);
    val[v] ^= 1;
    stats_.walk_flips++;
    for (uint32_t k2 : occ[make]) { if (ntrue[k2]++ == 0) del_unsat(k2); }
    for (uint32_t k2 : occ[neg(make)]) { if (--ntrue[k2] == 0) add_unsat(k2); }
    ticks += occ[make].size() + occ[neg(make)].size();
    if (unsat.size() < best_unsat) { best_unsat = unsat.size(); best = val; }
  }
  for (uint32_t v = 1; v < nv; ++v) if (assigns_[v] == 2) phase_[v] = (uint8_t)!best[v];
  if (best_unsat == 0) { stats_.walk_models++; return true; }
  return false;
}


// ---- equivalent literals (Tarjan on the binary implication graph) -----------
// (-x | y) and (x | -y) make x and y equivalent; a cycle in the implication
// graph makes every literal on it equivalent. Each strongly connected
// component is merged into one representative (its smallest variable): every
// clause is rewritten - the rewritten clause added to the proof, the old one
// deleted from it - and the merged variables leave the search. x = -x in one
// component is a contradiction.
bool SatSolver::substitute_equivalences(const std::vector<Lit>& frozen_lits) {
  const uint32_t nv = (uint32_t)assigns_.size();
  const size_t nl = 2 * (size_t)nv;
  repr_.resize(nv);
  for (uint32_t v = 0; v < nv; ++v) repr_[v] = 2 * v;
  std::vector<uint8_t> frozen(nv, 0);
  for (Lit l : frozen_lits) frozen[var(l)] = 1;
  // the graph: (a | b) gives -a -> b and -b -> a
  std::vector<std::vector<Lit>> out(nl);
  for (CRef c : originals_) {
    if ((cflags(c) & kDeleted) || csize(c) != 2) continue;
    const Lit a = clits(c)[0], b = clits(c)[1];
    if (value(a) != 2 || value(b) != 2) continue;
    out[neg(a)].push_back(b);
    out[neg(b)].push_back(a);
  }
  // Tarjan, iterative
  std::vector<uint32_t> index(nl, 0), low(nl, 0);
  std::vector<uint8_t> on_stack(nl, 0);
  std::vector<Lit> stack;
  uint32_t counter = 1;
  struct Frame { Lit l; size_t next; };
  std::vector<Frame> call;
  for (Lit root = 2; root < nl; ++root) {
    if (index[root] || out[root].empty()) continue;
    call.push_back({root, 0});
    index[root] = low[root] = counter++;
    stack.push_back(root); on_stack[root] = 1;
    while (!call.empty()) {
      Frame& f = call.back();
      if (f.next < out[f.l].size()) {
        const Lit w = out[f.l][f.next++];
        if (!index[w]) {
          index[w] = low[w] = counter++;
          stack.push_back(w); on_stack[w] = 1;
          call.push_back({w, 0});
        } else if (on_stack[w]) {
          low[f.l] = std::min(low[f.l], index[w]);
        }
        continue;
      }
      const Lit l = f.l;
      call.pop_back();
      if (!call.empty()) low[call.back().l] = std::min(low[call.back().l], low[l]);
      if (low[l] != index[l]) continue;
      // one component: from the stack down to l
      size_t from = stack.size();
      while (from > 0 && stack[from - 1] != l) --from;
      --from;
      if (stack.size() - from >= 2) {
        // representative: the smallest variable, with its sign as it appears
        Lit rep = stack[from];
        for (size_t i = from; i < stack.size(); ++i) if (var(stack[i]) < var(rep)) rep = stack[i];
        for (size_t i = from; i < stack.size(); ++i) {
          const Lit m = stack[i];
          if (m == rep) continue;
          // x and -x in one component: rep reaches -rep along binary
          // implications, so the unit -rep is RUP; -rep reaches rep back, so
          // the empty clause the caller writes next is RUP too. Without the
          // unit the proof is a bare "0" no checker can follow.
          if (var(m) == var(rep)) { proof_add({neg(rep)}); return false; }
          if (frozen[var(m)] || frozen[var(rep)]) continue;
          // m == rep: repr of var(m) is rep with m's sign folded in
          repr_[var(m)] = (m & 1) ? neg(rep) : rep;
          stats_.equivalences++;
        }
      }
      for (size_t i = from; i < stack.size(); ++i) on_stack[stack[i]] = 0;
      stack.resize(from);
    }
  }
  if (!stats_.equivalences) return true;
  // resolve chains (a's representative may itself have been merged)
  auto canon = [&](Lit l) {
    Lit r = repr_[var(l)];
    Lit outl = (l & 1) ? neg(r) : r;
    for (int guard = 0; guard < 64 && repr_[var(outl)] != (Lit)(2 * var(outl)); ++guard) {
      const Lit rr = repr_[var(outl)];
      outl = (outl & 1) ? neg(rr) : rr;
    }
    return outl;
  };
  for (uint32_t v = 1; v < nv; ++v) if (repr_[v] != (Lit)(2 * v)) repr_[v] = canon(2 * v);
  // rewrite every clause
  std::vector<uint32_t> mark(nl, 0);
  uint32_t stamp = 0;
  std::vector<Lit> nw;
  std::vector<CRef> keep, dead;
  for (CRef c : originals_) {
    if (cflags(c) & kDeleted) continue;
    bool changed = false;
    for (uint32_t i = 0; i < csize(c); ++i) if (repr_[var(clits(c)[i])] != (Lit)(2 * var(clits(c)[i]))) { changed = true; break; }
    if (!changed) { keep.push_back(c); continue; }
    ++stamp;
    nw.clear();
    bool taut = false;
    for (uint32_t i = 0; i < csize(c) && !taut; ++i) {
      const Lit l = canon(clits(c)[i]);
      if (value(l) == 0) { taut = true; break; }
      if (value(l) == 1) continue;
      if (mark[neg(l)] == stamp) { taut = true; break; }
      if (mark[l] == stamp) continue;
      mark[l] = stamp;
      nw.push_back(l);
    }
    stats_.substituted_clauses++;
    if (!taut) {
      if (nw.empty()) return false;
      proof_add(nw);
      if (nw.size() == 1) {
        if (value(nw[0]) == 2) assign(nw[0], kNoReason);
      } else {
        const CRef nc = alloc_clause(nw, false, 0);
        keep.push_back(nc);
      }
    }
    dead.push_back(c);
  }
  for (CRef c : dead) {
    proof_del(c);
    cflags(c) |= kDeleted;
    arena_wasted_ += kHeader + csize(c);
  }
  originals_.swap(keep);
  for (uint32_t v = 1; v < nv; ++v)
    if (repr_[v] != (Lit)(2 * v)) {
      if (eliminated_.size() < nv) eliminated_.resize(nv, 0);
      eliminated_[v] = 1;   // never branched on again
    }
  for (auto& ws : watches_) ws.clear();
  for (CRef c : originals_) attach(c);
  qhead_ = 0;
  if (propagate() != kNoReason) return false;
  return true;
}

}  // namespace aios::userland

namespace aios::userland {

SatSolver::Result SatSolver::unsat_core(int vars, const std::vector<std::vector<int>>& clauses, std::vector<size_t>* core, bool minimal,
                                        uint64_t conflict_limit, bool* minimal_proven) {
  core->clear();
  if (minimal_proven) *minimal_proven = false;
  // selector of clause i: vars + 1 + i
  SatSolver s;
  s.reserve_vars(vars + (int)clauses.size());
  for (size_t i = 0; i < clauses.size(); ++i) {
    std::vector<int> c = clauses[i];
    c.push_back(-(vars + 1 + (int)i));
    s.add_clause(c);
  }
  if (conflict_limit) s.set_conflict_limit(conflict_limit);
  std::vector<int> all;
  for (size_t i = 0; i < clauses.size(); ++i) all.push_back(vars + 1 + (int)i);
  const Result r = s.solve(all);
  if (r != Result::Unsat) return r;
  auto to_core = [&](const std::vector<int>& failed) {
    std::vector<size_t> out;
    for (int f : failed) if (f > vars) out.push_back((size_t)(f - vars - 1));
    std::sort(out.begin(), out.end());
    return out;
  };
  *core = to_core(s.failed_assumptions());
  if (!minimal) return Result::Unsat;
  // Deletion: a clause the rest can do without goes (and the rest shrinks to what that refutation used); one the
  // rest cannot do without is kept. Done when every clause left was found necessary.
  std::vector<size_t> keep;   // proven necessary
  bool proven = true;
  while (true) {
    size_t candidate = SIZE_MAX;
    for (size_t c : *core) if (std::find(keep.begin(), keep.end(), c) == keep.end()) { candidate = c; break; }
    if (candidate == SIZE_MAX) break;
    std::vector<int> assume;
    for (size_t c : *core) if (c != candidate) assume.push_back(vars + 1 + (int)c);
    const Result t = s.solve(assume);
    if (t == Result::Unsat) {
      std::vector<size_t> smaller = to_core(s.failed_assumptions());
      // the necessary ones found so far stay necessary: any refutation without them is impossible
      *core = smaller;
      for (size_t k : keep) if (std::find(core->begin(), core->end(), k) == core->end()) core->push_back(k);
      std::sort(core->begin(), core->end());
    } else {
      if (t == Result::Unknown) proven = false;   // kept without proof that it is needed
      keep.push_back(candidate);
    }
  }
  if (minimal_proven) *minimal_proven = proven;
  return Result::Unsat;
}

}  // namespace aios::userland
