#pragma once
// The SAT solver as something ASQL and grain can use, not a tool beside them.
//
//   CALL SAT('<DIMACS CNF>' [, <conflict limit>])
//   CALL SAT FROM (SELECT <clause column> FROM ...) [LIMIT <conflicts>]
//       each row one clause, its literals as text ("1 -2 3", a trailing 0 optional)
//   grain: sat(clauses [, conflict_limit])   clauses: list of lists of ints (or DIMACS text)
//
// An UNSAT answer also says why: its core, the fewest of the given clauses that still cannot all hold together
// (a minimal unsatisfiable subset: drop any one of them and the rest are satisfiable). The core is checked too - its
// own refutation through the RUP checker, and for each of its clauses a model of the others checked clause by clause.
//
// Every answer is verified before it is returned: a model against the
// clauses it was asked about, an UNSAT answer by running its DRAT proof
// through the independent RUP checker (sat::check_rup - none of the solver's
// code). The solver is deterministic, so a Thing may call it: a reaction
// replayed after a crash gets the same answer.
#include <cstdint>
#include <string>
#include <vector>

namespace aios::userland {

class SqlDatabase;
struct SqlQueryResult;

struct SatAnswer {
  int status = 0;                 // 10 SAT, 20 UNSAT, 0 unknown (budget spent)
  bool verified = false;
  std::string evidence;           // "model checked" / "proof verified (N lemmas)" / why not
  std::vector<int> model;         // SAT: +v / -v for v = 1..vars
  int vars = 0;
  size_t clauses = 0;
  uint64_t conflicts = 0, proof_bytes = 0;
  double solve_ms = 0, check_ms = 0;
  // UNSAT: the clauses (0-based indices into what was asked) that cannot all hold together
  std::vector<size_t> core;
  bool core_verified = false;     // the core alone refuted (proof checked), and each clause shown needed (model checked)
  std::string core_evidence;
};

SatAnswer solve_verified(int vars, const std::vector<std::vector<int>>& clauses, uint64_t conflict_limit = 0);

bool sql_is_sat(const std::string& sql);                         // CALL SAT ...
SqlQueryResult sql_run_sat(SqlDatabase& db, const std::string& sql, const std::string& session_db);

}  // namespace aios::userland
