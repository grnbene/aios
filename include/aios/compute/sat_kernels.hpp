#pragma once
// sat.solve: the aios CDCL solver (userland/sat_solver.hpp) as a Custom
// operation, so the scheduler places a solve on any device that has it - this
// machine's CPU, or a fleet peer's through a remote device - like any other
// work. Registered on every CPU device at boot.
//
// Tensors:
//   in[0]  I32 formula: [vars, lit, lit, 0, lit, ..., 0]  (DIMACS literals)
//   in[1]  I32 cube:    [count, lit, lit, ...]            (assumptions; count 0 = none)
//   out[0] I32 model:   [vars + 1]  model[v] = +v / -v    (SAT only)
//   out[1] F64 meta:    [8] status (10 SAT, 20 UNSAT, 0 unknown), conflicts,
//                        decisions, propagations, ms, proof bytes, proof
//                        truncated (1/0), vars
//   out[2] U8  proof:   binary DRAT (what drat-trim reads with -f binary);
//                        as much as fits - meta[6] says whether it all did
//   scalar             conflict limit (0 = none): a budgeted run stays
//                        deterministic, the same stop on every machine
#include <cstdint>
#include <string>
#include <vector>
#include "aios/compute/device.hpp"
#include "aios/core/result.hpp"

namespace aios::sat {

constexpr const char* kSolveKernel = "sat.solve";
// sat.check: an independent forward RUP checker (none of the solver's code).
//   in[0]  I32 formula (as sat.solve)   in[1] I32 cube   in[2] U8 binary DRAT
//   out[0] F64 meta [4]: verified (1/0), lemmas checked, ms, failing lemma index (-1 none)
// Verified = every added lemma is RUP (reverse unit propagation) against the
// formula, the cube's units and the lemmas before it - or, failing that, RAT
// on its first literal (how a fresh variable's definition enters: bounded
// variable addition) - deletions honoured -
// and the empty clause is reached: formula AND cube is unsatisfiable. The
// fleet runs it on a different machine than the one whose proof it checks.
constexpr const char* kCheckKernel = "sat.check";
Result<void> register_sat_kernels(Device& dev);

struct RupCheck { bool verified = false; uint64_t lemmas = 0, rat_lemmas = 0; long failed_at = -1; std::string why; };
RupCheck check_rup(const std::vector<std::vector<int>>& formula, const std::vector<int>& cube, const uint8_t* drat, size_t n);

// Binary DRAT: 'a' or 'd', each literal as a variable-length unsigned of
// 2*|lit| + (lit < 0), then 0.
void drat_binary_line(std::string* out, bool deletion, const std::vector<int>& lits);

}  // namespace aios::sat
