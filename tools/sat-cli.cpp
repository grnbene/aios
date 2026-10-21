// aios-sat: the aios CDCL solver on a DIMACS file, in the SAT Competition's
// output format and exit codes (10 SAT, 20 UNSAT, 0 unknown).
//
//   aios-sat FILE.cnf [--proof=FILE.drat] [--binary] [--threads=N] [--no-elim] [--conflicts=N] [--seconds=N] [--quiet]
//   (--threads: a racing portfolio; default every hardware thread, 1 = one solver)
//   aios-sat --check FILE.cnf PROOF.bin     (the independent RUP checker on a binary DRAT proof)
//
// A SAT answer's model is checked against the file's clauses before it is
// printed; a failed check exits 1 ("s ERROR") rather than claim a wrong answer.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "aios/compute/sat_kernels.hpp"
#include "aios/userland/sat_solver.hpp"

using aios::userland::SatSolver;

int main(int argc, char** argv) {
  std::string path, proof_path, dump_path;   // --dump=FILE: the preprocessed formula (then stop)
  uint64_t conflicts = 0;
  double seconds = 0;
  bool quiet = false, binary = false, no_elim = false;
  unsigned threads = 0;   // 0: every hardware thread
  if (argc == 4 && std::string(argv[1]) == "--check") {
    std::ifstream fin(argv[2], std::ios::binary), pin(argv[3], std::ios::binary);
    std::stringstream fs, ps;
    fs << fin.rdbuf(); ps << pin.rdbuf();
    int v = 0; std::vector<std::vector<int>> cls; std::string e;
    if (!SatSolver::parse_dimacs(fs.str(), &v, &cls, &e)) { std::fprintf(stderr, "parse: %s\n", e.c_str()); return 2; }
    const std::string pr = ps.str();
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = aios::sat::check_rup(cls, {}, reinterpret_cast<const uint8_t*>(pr.data()), pr.size());
    std::printf("c lemmas=%llu (rat %llu) time=%.1fms %s\n", (unsigned long long)r.lemmas, (unsigned long long)r.rat_lemmas,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), r.why.c_str());
    std::printf("s %s\n", r.verified ? "VERIFIED" : "NOT VERIFIED");
    return r.verified ? 0 : 1;
  }
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a.rfind("--proof=", 0) == 0) proof_path = a.substr(8);
    else if (a.rfind("--conflicts=", 0) == 0) conflicts = std::strtoull(a.c_str() + 12, nullptr, 10);
    else if (a.rfind("--seconds=", 0) == 0) seconds = std::atof(a.c_str() + 10);
    else if (a == "--quiet") quiet = true;
    else if (a == "--binary") binary = true;
    else if (a == "--no-elim") no_elim = true;
    else if (a.rfind("--dump=", 0) == 0) dump_path = a.substr(7);
    else if (a.rfind("--threads=", 0) == 0) threads = (unsigned)std::atoi(a.c_str() + 10);
    else if (a.rfind("--", 0) == 0) { if (a != "--help") std::fprintf(stderr, "aios-sat: unknown option %s\n", a.c_str()); path.clear(); break; }
    else path = a;
  }
  if (path.empty()) {
    std::fprintf(stderr,
                 "usage: aios-sat FILE.cnf [--proof=FILE] [--binary] [--threads=N] [--no-elim] [--conflicts=N] [--seconds=N] [--quiet] [--dump=FILE]\n"
                 "       aios-sat --check FILE.cnf PROOF.bin\n"
                 "  --proof=FILE   write a DRAT proof (text; --binary for binary DRAT)\n"
                 "  --threads=N    racing portfolio of N solvers (default: every hardware thread; 1 = one solver)\n"
                 "  --no-elim      no bounded variable elimination\n"
                 "  --conflicts=N  stop with s UNKNOWN after N conflicts (per portfolio member)\n"
                 "  --seconds=N    stop with s UNKNOWN after N seconds\n"
                 "  --dump=FILE    write the preprocessed formula and stop\n"
                 "  --check        verify a binary DRAT proof with the independent RUP checker\n"
                 "exit: 10 SAT, 20 UNSAT, 0 UNKNOWN, 1 bad model/proof, 2 usage or input error\n");
    return 2;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) { std::fprintf(stderr, "cannot read %s\n", path.c_str()); return 2; }
  std::stringstream ss;
  ss << in.rdbuf();
  int vars = 0;
  std::vector<std::vector<int>> clauses;
  std::string err;
  if (!SatSolver::parse_dimacs(ss.str(), &vars, &clauses, &err)) { std::fprintf(stderr, "parse: %s\n", err.c_str()); return 2; }

  if (!dump_path.empty()) {
    SatSolver d;
    d.reserve_vars(vars);
    if (no_elim) d.set_elimination(false);
    bool ok = true;
    for (const auto& c : clauses) if (!d.add_clause(c)) { ok = false; break; }
    d.set_conflict_limit(1);
    if (ok) d.solve();
    const auto f = d.current_formula();
    FILE* o = std::fopen(dump_path.c_str(), "w");
    if (!o) { std::fprintf(stderr, "cannot write %s\n", dump_path.c_str()); return 2; }
    std::fprintf(o, "p cnf %d %zu\n", d.num_vars(), f.size());
    for (const auto& c : f) { for (int l : c) std::fprintf(o, "%d ", l); std::fprintf(o, "0\n"); }
    std::fclose(o);
    std::printf("c dumped %zu clauses over %d variables\n", f.size(), d.num_vars());
    return 0;
  }
  const auto t0 = std::chrono::steady_clock::now();
  // A portfolio: one differently-configured solver per thread (member 0 is
  // the default), racing; the first to answer wins and the rest stop. Each
  // member's proof is its own, so the winner's proof stands alone.
  if (threads == 0) threads = std::max(1u, std::thread::hardware_concurrency());
  struct Member {
    SatSolver s;
    std::string proof;
    SatSolver::Result r = SatSolver::Result::Unknown;
    bool ok = true;
  };
  std::vector<std::unique_ptr<Member>> members;
  for (unsigned i = 0; i < threads; ++i) members.push_back(std::make_unique<Member>());
  std::atomic<int> winner{-1};
  const auto deadline = t0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds > 0 ? seconds : 1e9));
  auto run_member = [&](unsigned i) {
    Member& m = *members[i];
    m.s.set_diversity(i);
    if (!proof_path.empty())
      m.s.set_proof([&m, binary](bool del, const std::vector<int>& lits) {
        if (binary) { aios::sat::drat_binary_line(&m.proof, del, lits); return; }
        if (del) m.proof += "d ";
        char b[16];
        for (int l : lits) { const int n = std::snprintf(b, sizeof b, "%d ", l); m.proof.append(b, (size_t)n); }
        m.proof += "0\n";
      });
    m.s.reserve_vars(vars);
    if (no_elim) m.s.set_elimination(false);
    for (const auto& c : clauses) if (!m.s.add_clause(c)) { m.ok = false; break; }
    if (conflicts) m.s.set_conflict_limit(conflicts);
    m.s.set_stop_check([&] { return winner.load() >= 0 || std::chrono::steady_clock::now() >= deadline; });
    m.r = m.ok ? m.s.solve() : SatSolver::Result::Unsat;
    if (m.r != SatSolver::Result::Unknown) { int none = -1; winner.compare_exchange_strong(none, (int)i); }
  };
  if (threads == 1) run_member(0);
  else {
    std::vector<std::thread> ts;
    for (unsigned i = 0; i < threads; ++i) ts.emplace_back(run_member, i);
    for (auto& t : ts) t.join();
  }
  const int w = winner.load() >= 0 ? winner.load() : 0;
  Member& win = *members[(size_t)w];
  SatSolver& s = win.s;
  const SatSolver::Result r = win.r;
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (!proof_path.empty()) {
    FILE* proof = std::fopen(proof_path.c_str(), "wb");
    if (!proof) { std::fprintf(stderr, "cannot write %s\n", proof_path.c_str()); return 2; }
    std::fwrite(win.proof.data(), 1, win.proof.size(), proof);
    std::fclose(proof);
  }
  if (!quiet && threads > 1) std::printf("c portfolio: %u members, member %d answered first\n", threads, w);
  const auto& st = s.stats();
  if (!quiet)
    std::printf("c vars=%d clauses=%zu conflicts=%llu decisions=%llu propagations=%llu restarts=%llu reductions=%llu equiv=%llu eliminated=%llu gates=%llu resolvents=%llu rephases=%llu walks=%llu walk_models=%llu time=%.1fms\n", vars,
                clauses.size(), (unsigned long long)st.conflicts, (unsigned long long)st.decisions,
                (unsigned long long)st.propagations, (unsigned long long)st.restarts, (unsigned long long)st.reductions, (unsigned long long)st.equivalences, (unsigned long long)st.eliminated_vars, (unsigned long long)st.gates, (unsigned long long)st.resolvents, (unsigned long long)st.rephases, (unsigned long long)st.walks, (unsigned long long)st.walk_models, ms);
  if (!quiet && st.conflicts)
    std::printf("c learnt_avg=%.2f minimized=%.1f%% reductions=%llu deleted=%llu shrunken=%llu/%llu\n", (double)st.learnt_literals / (double)st.conflicts,
                100.0 * (double)st.minimized_literals / (double)(st.learnt_literals + st.minimized_literals),
                (unsigned long long)st.reductions, (unsigned long long)st.deleted_clauses,
                (unsigned long long)st.shrunken_literals, (unsigned long long)st.shrunken_blocks);
  if (!quiet && st.factored)
    std::printf("c factored=%llu factor_removed=%llu factor_steps=%llu\n", (unsigned long long)st.factored,
                (unsigned long long)st.factor_removed_clauses, (unsigned long long)st.factor_steps);
  if (r == SatSolver::Result::Sat) {
    if (!SatSolver::check_model(clauses, s.model())) { std::printf("s ERROR model does not satisfy the formula\n"); return 1; }
    std::printf("s SATISFIABLE\n");
    std::string line = "v";
    for (int v = 1; v <= vars; ++v) {
      line += ' ';
      line += std::to_string(s.model()[(size_t)v] ? v : -v);
      if (line.size() > 70) { std::printf("%s\n", line.c_str()); line = "v"; }
    }
    std::printf("%s 0\n", line.c_str());
    return 10;
  }
  if (r == SatSolver::Result::Unsat) { std::printf("s UNSATISFIABLE\n"); return 20; }
  std::printf("s UNKNOWN\n");
  return 0;
}
