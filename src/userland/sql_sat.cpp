#include "aios/userland/sql_sat.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <memory>

#include "aios/compute/sat_kernels.hpp"
#include "aios/hal/host.hpp"
#include "aios/userland/sat_solver.hpp"
#include "aios/userland/sql_database.hpp"

namespace aios::userland {

namespace {
double elapsed_ms(uint64_t from_ns) {
  static std::unique_ptr<hal::Host> clock = hal::make_host();   // timing only; nothing decides by it
  return (double)(clock->monotonic_ns() - from_ns) / 1e6;
}
uint64_t now_ns() {
  static std::unique_ptr<hal::Host> clock = hal::make_host();
  return clock->monotonic_ns();
}
std::string upper(std::string s) { for (auto& c : s) c = (char)std::toupper((unsigned char)c); return s; }
std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace((unsigned char)s[a])) ++a;
  while (b > a && (std::isspace((unsigned char)s[b - 1]) || s[b - 1] == ';')) --b;
  return s.substr(a, b - a);
}
// "1 -2 3 0" -> {1, -2, 3}; false on anything that is not a literal
bool parse_clause(const std::string& text, std::vector<int>* out) {
  out->clear();
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && (std::isspace((unsigned char)text[i]) || text[i] == ',')) ++i;
    if (i >= text.size()) break;
    char* e = nullptr;
    const long v = std::strtol(text.c_str() + i, &e, 10);
    if (e == text.c_str() + i) return false;
    i = (size_t)(e - text.c_str());
    if (v == 0) break;
    out->push_back((int)v);
  }
  return true;
}
SqlQueryResult error(const std::string& msg) {
  SqlQueryResult r;
  r.success = false;
  r.error_message = msg;
  return r;
}
}  // namespace

namespace {
// Why an UNSAT formula is: its minimal core, checked without trusting the search that found it. The core alone must
// be refuted by a proof the RUP checker accepts; each of its clauses must be needed - the core without it has a model,
// checked against those clauses.
void explain_unsat(int vars, const std::vector<std::vector<int>>& clauses, uint64_t conflict_limit, SatAnswer* a) {
  bool minimal = false;
  const auto r = SatSolver::unsat_core(vars, clauses, &a->core, true, conflict_limit, &minimal);
  if (r != SatSolver::Result::Unsat) { a->core.clear(); a->core_evidence = "no core: the search for one hit the conflict limit"; return; }
  std::vector<std::vector<int>> sub;
  for (size_t i : a->core) sub.push_back(clauses[i]);
  std::string proof;
  SatSolver s;
  s.set_proof([&proof](bool del, const std::vector<int>& lits) { sat::drat_binary_line(&proof, del, lits); });
  s.reserve_vars(vars);
  bool fine = true;
  for (const auto& c : sub) if (!s.add_clause(c)) { fine = false; break; }
  const auto rr = fine ? s.solve() : SatSolver::Result::Unsat;
  const auto ck = rr == SatSolver::Result::Unsat ? sat::check_rup(sub, {}, reinterpret_cast<const uint8_t*>(proof.data()), proof.size())
                                                  : sat::RupCheck{};
  if (rr != SatSolver::Result::Unsat || !ck.verified) {
    a->core_evidence = "CORE FAILED its check: its clauses alone were not refuted";
    return;
  }
  // necessity, one clause at a time (a large core is reported as refuted only: k more solves would be k times the cost)
  const size_t kNecessityLimit = 256;
  if (sub.size() > kNecessityLimit) {
    a->core_verified = true;
    a->core_evidence = "core of " + std::to_string(sub.size()) + " clauses refuted (proof verified); minimality " +
                       (minimal ? "found by the search, not re-checked (over " + std::to_string(kNecessityLimit) + " clauses)" : "not proven");
    return;
  }
  for (size_t k = 0; k < sub.size(); ++k) {
    std::vector<std::vector<int>> rest;
    for (size_t j = 0; j < sub.size(); ++j) if (j != k) rest.push_back(sub[j]);
    SatSolver t;
    t.reserve_vars(vars);
    if (conflict_limit) t.set_conflict_limit(conflict_limit);
    bool ok = true;
    for (const auto& c : rest) if (!t.add_clause(c)) { ok = false; break; }
    if (!ok || t.solve() != SatSolver::Result::Sat || !SatSolver::check_model(rest, t.model())) {
      a->core_verified = true;
      a->core_evidence = "core of " + std::to_string(sub.size()) + " clauses refuted (proof verified); clause " +
                         std::to_string(a->core[k] + 1) + " could not be shown necessary: the core may not be minimal";
      return;
    }
  }
  a->core_verified = true;
  a->core_evidence = "minimal core of " + std::to_string(sub.size()) + " clauses: refuted (proof verified), and each one needed (a checked model of the rest)";
}
}  // namespace

SatAnswer solve_verified(int vars, const std::vector<std::vector<int>>& clauses, uint64_t conflict_limit) {
  SatAnswer a;
  a.vars = vars;
  a.clauses = clauses.size();
  SatSolver s;
  std::string proof;
  s.set_proof([&proof](bool del, const std::vector<int>& lits) { sat::drat_binary_line(&proof, del, lits); });
  s.reserve_vars(vars);
  if (conflict_limit) s.set_conflict_limit(conflict_limit);
  const uint64_t t0 = now_ns();
  bool ok = true;
  for (const auto& c : clauses) if (!s.add_clause(c)) { ok = false; break; }
  const SatSolver::Result r = ok ? s.solve() : SatSolver::Result::Unsat;
  a.solve_ms = elapsed_ms(t0);
  a.conflicts = s.stats().conflicts;
  a.proof_bytes = proof.size();
  const uint64_t t1 = now_ns();
  if (r == SatSolver::Result::Sat) {
    a.status = 10;
    a.verified = SatSolver::check_model(clauses, s.model());
    a.evidence = a.verified ? "model checked against every clause" : "MODEL FAILED its check";
    for (int v = 1; v <= vars; ++v) a.model.push_back(s.model()[(size_t)v] ? v : -v);
  } else if (r == SatSolver::Result::Unsat) {
    a.status = 20;
    const auto ck = sat::check_rup(clauses, {}, reinterpret_cast<const uint8_t*>(proof.data()), proof.size());
    a.verified = ck.verified;
    a.evidence = ck.verified ? "proof verified (" + std::to_string(ck.lemmas) + " lemmas, independent RUP/RAT checker)"
                             : "PROOF FAILED: " + ck.why;
    explain_unsat(vars, clauses, conflict_limit, &a);
  } else {
    a.status = 0;
    a.evidence = "unknown: the conflict limit was reached";
  }
  a.check_ms = elapsed_ms(t1);
  return a;
}

// Asked of every statement: reads the first two words in place instead of
// copying and upper-casing the whole text.
bool sql_is_sat(const std::string& sql) {
  size_t i = 0;
  while (i < sql.size() && std::isspace((unsigned char)sql[i])) ++i;
  auto word_is = [&](const char* w) {
    size_t n = std::strlen(w);
    if (i + n > sql.size()) return false;
    for (size_t k = 0; k < n; ++k) if (std::toupper((unsigned char)sql[i + k]) != w[k]) return false;
    if (i + n < sql.size() && std::isalnum((unsigned char)sql[i + n])) return false;
    i += n;
    return true;
  };
  if (!word_is("CALL")) return false;
  while (i < sql.size() && std::isspace((unsigned char)sql[i])) ++i;
  return word_is("SAT");
}

SqlQueryResult sql_run_sat(SqlDatabase& db, const std::string& sql, const std::string& session_db) {
  const std::string t = trim(sql);
  const std::string u = upper(t);
  size_t i = u.find("SAT") + 3;
  while (i < t.size() && std::isspace((unsigned char)t[i])) ++i;
  std::vector<std::vector<int>> clauses;
  int vars = 0;
  uint64_t limit = 0;
  if (u.compare(i, 4, "FROM") == 0) {
    // CALL SAT FROM (SELECT ...) [LIMIT n]: the clauses are rows
    std::string q = trim(t.substr(i + 4));
    const size_t lim = upper(q).rfind(" LIMIT ");
    if (lim != std::string::npos && q.back() != ')') {
      limit = std::strtoull(q.c_str() + lim + 7, nullptr, 10);
      q = trim(q.substr(0, lim));
    }
    if (!q.empty() && q.front() == '(' && q.back() == ')') q = trim(q.substr(1, q.size() - 2));
    if (upper(q).rfind("SELECT", 0) != 0 && upper(q).rfind("WITH", 0) != 0)
      return error("CALL SAT FROM (SELECT <clause column> FROM ...): the clauses come from a query");
    SqlQueryResult rows = db.execute_local_query(q, session_db);
    if (!rows.success) return error("CALL SAT FROM: " + rows.error_message);
    std::vector<int> c;
    for (const auto& row : rows.rows) {
      if (row.values.empty() || sql_is_null(row.values[0])) continue;
      if (!parse_clause(row.values[0], &c)) return error("CALL SAT FROM: not a clause: " + row.values[0].substr(0, 60));
      for (int l : c) vars = std::max(vars, l > 0 ? l : -l);
      clauses.push_back(c);
    }
  } else {
    // CALL SAT('<DIMACS>' [, n])
    if (i >= t.size() || t[i] != '(' || t.back() != ')') return error("CALL SAT('<DIMACS CNF>' [, conflict limit]) | CALL SAT FROM (SELECT ...)");
    std::string inner = t.substr(i + 1, t.size() - i - 2);
    size_t q0 = inner.find_first_of("'\"");
    if (q0 == std::string::npos) return error("CALL SAT: the CNF goes in a quoted string");
    const char qc = inner[q0];
    std::string cnf;
    size_t k = q0 + 1;
    for (; k < inner.size(); ++k) {
      if (inner[k] == qc && k + 1 < inner.size() && inner[k + 1] == qc) { cnf += qc; ++k; continue; }
      if (inner[k] == '\\' && k + 1 < inner.size()) { const char n = inner[++k]; cnf += n == 'n' ? '\n' : n; continue; }
      if (inner[k] == qc) break;
      cnf += inner[k];
    }
    const size_t comma = inner.find(',', k);
    if (comma != std::string::npos) limit = std::strtoull(inner.c_str() + comma + 1, nullptr, 10);
    std::string err;
    if (!SatSolver::parse_dimacs(cnf, &vars, &clauses, &err)) return error("CALL SAT: " + err);
  }
  const SatAnswer a = solve_verified(vars, clauses, limit);
  SqlQueryResult r;
  r.is_result_set = true;
  r.columns = {"status", "verified", "evidence", "vars", "clauses", "conflicts", "solve_ms", "check_ms", "model", "core", "core_evidence"};
  std::string model, core;
  for (int l : a.model) { if (!model.empty()) model += ' '; model += std::to_string(l); }
  for (size_t c : a.core) { if (!core.empty()) core += ' '; core += std::to_string(c + 1); }   // clause numbers, from 1 (DIMACS order)
  char sm[32], cm[32];
  std::snprintf(sm, sizeof sm, "%.3f", a.solve_ms);
  std::snprintf(cm, sizeof cm, "%.3f", a.check_ms);
  r.rows.push_back(SqlRow{{a.status == 10 ? "SAT" : a.status == 20 ? "UNSAT" : "UNKNOWN", a.verified ? "1" : "0", a.evidence,
                           std::to_string(a.vars), std::to_string(a.clauses), std::to_string(a.conflicts), sm, cm,
                           a.status == 10 ? model : sql_null(), a.status == 20 && !a.core.empty() ? core : sql_null(),
                           a.status == 20 ? a.core_evidence : sql_null()}});
  if (!sql_wire_results()) r.formatted_table = db.format_result_table(r.columns, r.rows);
  return r;
}

}  // namespace aios::userland
