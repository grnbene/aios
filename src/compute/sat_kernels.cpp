#include "aios/compute/sat_kernels.hpp"

#include <algorithm>
#include <cstring>

#include "aios/hal/host.hpp"
#include "aios/userland/sat_solver.hpp"

namespace aios::sat {

void drat_binary_line(std::string* out, bool deletion, const std::vector<int>& lits) {
  out->push_back(deletion ? 'd' : 'a');
  for (int l : lits) {
    uint32_t u = 2u * (uint32_t)(l > 0 ? l : -l) + (l < 0 ? 1u : 0u);
    while (u > 127) { out->push_back((char)(0x80 | (u & 0x7f))); u >>= 7; }
    out->push_back((char)u);
  }
  out->push_back(0);
}

Result<void> register_sat_kernels(Device& dev) {
  auto rc = dev.register_kernel(kCheckKernel, [](KernelArgs& a) -> Result<void> {
    if (a.op.inputs.size() < 3 || a.op.outputs.size() < 1) return Result<void>(Status::Invalid, "sat.check needs 3 inputs and 1 output");
    const Tensor& tf = a.op.inputs[0];
    const Tensor& tc = a.op.inputs[1];
    const Tensor& tp = a.op.inputs[2];
    const Tensor& tmeta = a.op.outputs[0];
    if (tf.dtype != DType::I32 || tc.dtype != DType::I32 || tp.dtype != DType::U8 || tmeta.dtype != DType::F64 || tmeta.elements() < 4)
      return Result<void>(Status::Invalid, "sat.check: formula/cube I32, proof U8, meta F64 [4]");
    const int32_t* f = reinterpret_cast<const int32_t*>(a.inputs[0].base + tf.offset);
    const int32_t* c = reinterpret_cast<const int32_t*>(a.inputs[1].base + tc.offset);
    const uint8_t* p = reinterpret_cast<const uint8_t*>(a.inputs[2].base + tp.offset);
    std::vector<std::vector<int>> formula;
    std::vector<int> cl;
    for (uint64_t i = 1; i < tf.elements(); ++i) { if (f[i] == 0) { formula.push_back(cl); cl.clear(); } else cl.push_back(f[i]); }
    if (!cl.empty()) formula.push_back(cl);
    std::vector<int> cube;
    if (tc.elements() >= 1) for (uint64_t i = 1; i <= (uint64_t)c[0] && i < tc.elements(); ++i) cube.push_back(c[i]);
    // the proof's real length rides in the op's scalar (the tensor is a capacity)
    const size_t n = a.op.scalar > 0 ? std::min<size_t>((size_t)a.op.scalar, tp.elements()) : tp.elements();
    static std::unique_ptr<hal::Host> clock = hal::make_host();
    const uint64_t start = clock->monotonic_ns();
    const RupCheck r = check_rup(formula, cube, p, n);
    double* meta = reinterpret_cast<double*>(a.outputs[0].base + tmeta.offset);
    meta[0] = r.verified ? 1 : 0;
    meta[1] = (double)r.lemmas;
    meta[2] = (double)(clock->monotonic_ns() - start) / 1e6;
    meta[3] = (double)r.failed_at;
    return Ok();
  });
  if (!rc.ok()) return rc;
  return dev.register_kernel(kSolveKernel, [](KernelArgs& a) -> Result<void> {
    if (a.op.inputs.size() < 2 || a.op.outputs.size() < 3) return Result<void>(Status::Invalid, "sat.solve needs 2 inputs and 3 outputs");
    const Tensor& tf = a.op.inputs[0];
    const Tensor& tc = a.op.inputs[1];
    const Tensor& tm = a.op.outputs[0];
    const Tensor& tmeta = a.op.outputs[1];
    const Tensor& tp = a.op.outputs[2];
    if (tf.dtype != DType::I32 || tc.dtype != DType::I32 || tm.dtype != DType::I32 || tmeta.dtype != DType::F64 || tp.dtype != DType::U8)
      return Result<void>(Status::Invalid, "sat.solve: formula/cube/model I32, meta F64, proof U8");
    if (tmeta.elements() < 8) return Result<void>(Status::Invalid, "sat.solve: meta must be F64 [8]");
    const int32_t* f = reinterpret_cast<const int32_t*>(a.inputs[0].base + tf.offset);
    const uint64_t fn = tf.elements();
    const int32_t* c = reinterpret_cast<const int32_t*>(a.inputs[1].base + tc.offset);
    const uint64_t cn = tc.elements();
    if (fn < 1) return Result<void>(Status::Invalid, "sat.solve: empty formula tensor");
    const int vars = f[0];
    if (vars < 0 || tm.elements() < (uint64_t)vars + 1) return Result<void>(Status::Invalid, "sat.solve: model must be I32 [vars + 1]");

    userland::SatSolver s;
    s.reserve_vars(vars);
    std::string proof;
    const uint64_t cap = tp.elements();
    bool truncated = false;
    s.set_proof([&](bool del, const std::vector<int>& lits) {
      if (truncated) return;
      const size_t before = proof.size();
      drat_binary_line(&proof, del, lits);
      if (proof.size() > cap) { proof.resize(before); truncated = true; }
    });
    static std::unique_ptr<hal::Host> clock = hal::make_host();   // timing only; nothing decides by it
    const uint64_t start = clock->monotonic_ns();
    bool ok = true;
    std::vector<int> clause;
    for (uint64_t i = 1; i < fn && ok; ++i) {
      if (f[i] == 0) { ok = s.add_clause(clause); clause.clear(); }
      else clause.push_back(f[i]);
    }
    if (ok && !clause.empty()) ok = s.add_clause(clause);
    std::vector<int> cube;
    if (cn >= 1) for (uint64_t i = 1; i <= (uint64_t)c[0] && i < cn; ++i) cube.push_back(c[i]);
    if (a.op.scalar > 0) s.set_conflict_limit((uint64_t)a.op.scalar);
    // every 256 conflicts: let the machine breathe (a no-op where a job has a
    // thread of its own; on the Pi, the watchdog, the network, the hub link)
    s.set_stop_check([] { hal::compute_yield(); return false; });
    const auto r = ok ? s.solve(cube) : userland::SatSolver::Result::Unsat;

    int32_t* model = reinterpret_cast<int32_t*>(a.outputs[0].base + tm.offset);
    double* meta = reinterpret_cast<double*>(a.outputs[1].base + tmeta.offset);
    uint8_t* pout = reinterpret_cast<uint8_t*>(a.outputs[2].base + tp.offset);
    model[0] = 0;
    for (int v = 1; v <= vars; ++v) model[v] = (r == userland::SatSolver::Result::Sat && s.model()[(size_t)v]) ? v : -v;
    const auto& st = s.stats();
    meta[0] = r == userland::SatSolver::Result::Sat ? 10 : r == userland::SatSolver::Result::Unsat ? 20 : 0;
    meta[1] = (double)st.conflicts;
    meta[2] = (double)st.decisions;
    meta[3] = (double)st.propagations;
    meta[4] = (double)(clock->monotonic_ns() - start) / 1e6;
    // A model is its own evidence: a SAT answer ships no proof, and with the
    // buffer left zero a remote caller reads nothing back for it.
    if (r == userland::SatSolver::Result::Sat) { proof.clear(); truncated = false; }
    meta[5] = (double)proof.size();
    meta[6] = truncated ? 1 : 0;
    meta[7] = vars;
    std::memcpy(pout, proof.data(), std::min<size_t>(proof.size(), cap));
    // zero past the proof: a remote caller then takes only what was written
    if (proof.size() < cap) std::memset(pout + proof.size(), 0, cap - proof.size());
    return Ok();
  });
}

}  // namespace aios::sat

// ---- the checker ------------------------------------------------------------
// Deliberately small and separate from the solver: a clause list, two watched
// literals, unit propagation from scratch per lemma (no learning, no
// heuristics). Slower than drat-trim's backward checking; its value is that it
// shares nothing with the code whose answers it judges.
namespace aios::sat {
namespace {
struct Checker {
  std::vector<std::vector<int>> cls;     // clause store; a deleted clause is emptied
  std::vector<bool> dead;
  std::vector<std::vector<uint32_t>> watch;   // by literal index: clauses watching it
  std::vector<int8_t> val;                    // by var: 0 unassigned, +1 true, -1 false
  std::vector<int> trail;
  std::vector<std::pair<std::vector<int>, uint32_t>> index_list;   // for deletion lookup
  std::vector<std::vector<uint32_t>> by_hash;
  std::vector<std::vector<uint32_t>> occ;     // by literal index: clauses holding it (RAT partners)
  int nvars = 0;
  bool base_conflict = false;   // the formula + cube + lemmas so far already propagate to a conflict

  static uint32_t li(int l) { return l > 0 ? 2u * (uint32_t)l : 2u * (uint32_t)(-l) + 1; }
  void grow(int v) {
    if (v <= nvars) return;
    nvars = v;
    val.resize((size_t)v + 1, 0);
    watch.resize(2 * ((size_t)v + 1));
    occ.resize(2 * ((size_t)v + 1));
  }
  int value(int l) const { const int8_t x = val[(size_t)(l > 0 ? l : -l)]; return l > 0 ? x : -x; }
  static uint64_t key(std::vector<int> c) {
    std::sort(c.begin(), c.end());
    uint64_t h = 1469598103934665603ull;
    for (int l : c) { h ^= (uint64_t)(uint32_t)l; h *= 1099511628211ull; }
    return h;
  }
  void add(const std::vector<int>& c) {
    for (int l : c) grow(l > 0 ? l : -l);
    const uint32_t id = (uint32_t)cls.size();
    cls.push_back(c);
    dead.push_back(false);
    if (c.size() >= 2) { watch[li(c[0])].push_back(id); watch[li(c[1])].push_back(id); }
    for (int l : c) occ[li(l)].push_back(id);
    const uint64_t k = key(c) % 65521;
    if (by_hash.empty()) by_hash.resize(65521);
    by_hash[k].push_back(id);
  }
  void remove(const std::vector<int>& c) {
    if (by_hash.empty()) return;
    std::vector<int> s = c;
    std::sort(s.begin(), s.end());
    auto& bucket = by_hash[key(c) % 65521];
    for (size_t i = 0; i < bucket.size(); ++i) {
      const uint32_t id = bucket[i];
      if (dead[id]) continue;
      std::vector<int> t = cls[id];
      std::sort(t.begin(), t.end());
      // unit clauses are kept: drat-trim ignores deletions of units too
      if (t == s && t.size() > 1) { dead[id] = true; bucket.erase(bucket.begin() + (long)i); return; }
    }
  }
  bool assign(int l) {   // false = conflict
    const int v = value(l);
    if (v > 0) return true;
    if (v < 0) return false;
    val[(size_t)(l > 0 ? l : -l)] = (int8_t)(l > 0 ? 1 : -1);
    trail.push_back(l);
    return true;
  }
  // Propagate from trail position `from`; true when a conflict arises.
  bool propagate(size_t from) {
    for (size_t q = from; q < trail.size(); ++q) {
      const int falsified = -trail[q];
      auto& ws = watch[li(falsified)];
      for (size_t i = 0; i < ws.size();) {
        const uint32_t id = ws[i];
        if (dead[id]) { ws[i] = ws.back(); ws.pop_back(); continue; }
        auto& c = cls[id];
        if (c[0] == falsified) std::swap(c[0], c[1]);
        if (value(c[0]) > 0) { ++i; continue; }
        bool moved = false;
        for (size_t k = 2; k < c.size(); ++k)
          if (value(c[k]) >= 0) { std::swap(c[1], c[k]); watch[li(c[1])].push_back(id); ws[i] = ws.back(); ws.pop_back(); moved = true; break; }
        if (moved) continue;
        if (value(c[0]) < 0) return true;
        assign(c[0]);
        ++i;
      }
    }
    return false;
  }
  void undo(size_t to) {
    while (trail.size() > to) { const int l = trail.back(); trail.pop_back(); val[(size_t)(l > 0 ? l : -l)] = 0; }
  }
  // Units (formula + cube + unit lemmas) and their consequences, kept assigned.
  // RUP: every literal of c false, then propagation must conflict.
  bool rup(const std::vector<int>& c) {
    const size_t mark = trail.size();
    bool conflict = false;
    for (int l : c) if (!assign(-l)) { conflict = true; break; }
    if (!conflict) conflict = propagate(mark);
    undo(mark);
    return conflict;
  }
  // RAT on the lemma's first literal p (how a new variable's definition
  // enters a proof): for every live clause D holding -p, the resolvent
  // lemma | D\{-p} is a tautology or RUP.
  bool rat(const std::vector<int>& c) {
    if (c.empty()) return false;
    const int p = c[0];
    std::vector<int> r;
    for (uint32_t id : occ[li(-p)]) {
      if (dead[id]) continue;
      r = c;
      bool taut = false;
      for (int l : cls[id]) {
        if (l == -p) continue;
        if (std::find(c.begin(), c.end(), -l) != c.end()) { taut = true; break; }
        r.push_back(l);
      }
      if (!taut && !rup(r)) return false;
    }
    return true;
  }
  bool settle_units(size_t from_clause) {
    const size_t before = trail.size();
    for (size_t i = from_clause; i < cls.size(); ++i)
      if (!dead[i] && cls[i].size() == 1 && !assign(cls[i][0])) return true;
    for (size_t i = from_clause; i < cls.size(); ++i)
      if (!dead[i] && cls[i].empty()) return true;
    return propagate(before);
  }
};
}  // namespace

RupCheck check_rup(const std::vector<std::vector<int>>& formula, const std::vector<int>& cube, const uint8_t* drat, size_t n) {
  RupCheck out;
  Checker ck;
  for (const auto& c : formula) ck.add(c);
  for (int l : cube) ck.add({l});
  if (ck.settle_units(0)) { out.verified = true; out.why = "formula and cube conflict by propagation alone"; return out; }
  size_t units_done = ck.cls.size();
  size_t i = 0;
  std::vector<int> lemma;
  while (i < n) {
    const uint8_t tag = drat[i++];
    if (tag != 'a' && tag != 'd') { out.why = "not binary DRAT at byte " + std::to_string(i - 1); return out; }
    lemma.clear();
    for (;;) {
      if (i >= n) { out.why = "truncated proof"; return out; }
      uint32_t u = 0; int shift = 0;
      for (;;) {
        if (i >= n) { out.why = "truncated literal"; return out; }
        const uint8_t b = drat[i++];
        u |= (uint32_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80)) break;
        if (shift > 28) { out.why = "literal too long"; return out; }
      }
      if (u == 0) break;
      lemma.push_back((u & 1) ? -(int)(u >> 1) : (int)(u >> 1));
    }
    if (tag == 'd') { ck.remove(lemma); continue; }
    // RUP, or else RAT on its first literal
    for (int l : lemma) ck.grow(l > 0 ? l : -l);
    if (!ck.rup(lemma)) {
      if (!ck.rat(lemma)) { out.failed_at = (long)out.lemmas; out.why = "lemma " + std::to_string(out.lemmas) + " is neither RUP nor RAT"; return out; }
      ++out.rat_lemmas;
    }
    ++out.lemmas;
    if (lemma.empty()) { out.verified = true; return out; }
    // Watches must sit on literals that are not already false: true ones
    // first, then unassigned, false last. None left = a conflict at level 0
    // (proved); one unassigned and the rest false = a new unit.
    std::stable_sort(lemma.begin(), lemma.end(), [&](int a, int b) { return ck.value(a) > ck.value(b); });
    const int v0 = ck.value(lemma[0]);
    const int v1 = lemma.size() > 1 ? ck.value(lemma[1]) : -1;
    ck.add(lemma);
    units_done = ck.cls.size();
    if (v0 < 0) { out.verified = true; return out; }
    if (v0 == 0 && v1 < 0) {
      const size_t mark = ck.trail.size();
      ck.assign(lemma[0]);
      if (ck.propagate(mark)) { out.verified = true; return out; }
    }
  }
  out.why = "the proof never reaches the empty clause";
  return out;
}

}  // namespace aios::sat
