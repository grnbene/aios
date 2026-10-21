#pragma once
// The evidence gate: what the agent is not allowed to get away with.
//
// A claim ("this is broken", "this is unused", "this fix works") is cheap to
// make and easy to leave hanging. This module is the ledger that makes each
// one cost something: it records the files and symbols actually read this
// session, refuses an edit to a file nobody opened, and refuses to let a
// claim leave the session without a verdict backed by evidence that resolves.
//
// Three verdicts only, and the same three words everywhere — here, in
// docs/evidence-first.md, in the `evidence` command, and in the audit log:
//   PROVEN_RIGHT  — the evidence shows it holds
//   PROVEN_WRONG  — the evidence shows it does not
//   UNPROVEN      — nobody has shown either way; this BLOCKS completion
// plus one flag on the evidence itself:
//   FABRICATED    — a cited file, line or command output that does not resolve
//
// The repo has no threads here and no iostreams: same shape as the rest of
// the command surface. Host-side only (it touches the filesystem to resolve a
// citation), so it is not linked into the bare-metal core.
#include <string>
#include <vector>
#include <utility>
#include <cstdint>

namespace aios::cli {

// Verdict labels. Used verbatim in output, in JSON, in the log, and in the
// docs. Change one and you have changed all four.
enum class Verdict { Unproven, ProvenRight, ProvenWrong };

const char* verdict_name(Verdict v);
bool verdict_from_name(const std::string& s, Verdict& out);   // exact label only

// One cited piece of proof.
//   kind "file"    → ref is "<path>:<line>", resolves when the file exists and
//                     the line is in range (1-based, last line inclusive).
//   kind "command" → ref is the command, detail is its captured output;
//                     resolves when the output is non-empty.
struct Evidence {
  std::string kind;
  std::string ref;
  std::string detail;
  bool resolved = false;
  std::string problem;   // why it did not resolve, when it did not
};

// A claim under test. One per distinct claim text.
struct Claim {
  std::string claim;
  Verdict verdict = Verdict::Unproven;
  std::vector<Evidence> evidence;
  std::string resolution;   // what settled it, or the check that would
};

class EvidenceGate {
 public:
  // root: the repo root, used to resolve relative citations.
  // log_dir: where the audit entries go; appended, never rewritten.
  explicit EvidenceGate(std::string root = ".", std::string log_dir = ".assay/reflexion");

  // ── look before acting ──────────────────────────────────────────────────
  // Record that a file was opened this session, and optionally which symbols
  // were read out of it. Both are recorded from the same call, so "I read the
  // call sites" is as cheap as "I read the file" and is auditable too.
  void record_read(const std::string& path, const std::vector<std::string>& symbols = {});
  bool has_read(const std::string& path) const;
  const std::vector<std::pair<std::string, std::vector<std::string>>>& reads() const { return reads_; }

  // Empty string means the edit may proceed. Otherwise: the rule broken, the
  // offending item, and the next step.
  std::string check_edit(const std::string& path) const;

  // ── claims need a verdict ───────────────────────────────────────────────
  Claim* find(const std::string& claim);
  // Records the claim at UNPROVEN if new, then attaches evidence and resolves
  // every citation against the repo. Returns "" on success; otherwise the
  // reason the verdict was refused.
  std::string add_evidence(const std::string& claim, const std::string& kind,
                           const std::string& ref, const std::string& detail = "");
  // Sets the verdict. PROVEN_RIGHT/PROVEN_WRONG with no resolving evidence are
  // refused. UNPROVEN clears nothing — it is where a claim starts and where a
  // failed attempt lands. Returns "" on success, else the reason.
  std::string set_verdict(const std::string& claim, Verdict v, const std::string& resolution = "");
  const std::vector<Claim>& claims() const { return claims_; }
  const Claim* fabricated() const;   // first claim carrying FABRICATED evidence

  // ── unresolved claims block completion ──────────────────────────────────
  bool blocks_completion() const;
  // "" when the task may be marked done. Otherwise: what is unproven or
  // fabricated, and the check that would settle it.
  std::string completion_report() const;

  // ── audit trail ─────────────────────────────────────────────────────────
  // Appends one entry per claim to <log_dir>/evidence.jsonl, in the shape of
  // the existing receipts.jsonl (version/id/timestamp/signature first).
  // Never touches existing entries. Returns the path written, or "" if it
  // could not be opened.
  std::string append_audit() const;

  // ── operator-visible output ─────────────────────────────────────────────
  std::string to_json() const;                  // the same data, machine-readable
  void render(std::string& out, int width = 80) const;   // plain text, wrapped

 private:
  Claim& claim_slot(const std::string& claim);
  Evidence resolve(const std::string& kind, const std::string& ref,
                   const std::string& detail) const;
  size_t line_count(const std::string& path) const;   // 0 if it cannot be read
  bool file_exists(const std::string& path) const;
  std::string abs_of(const std::string& path) const;

  std::string root_;
  std::string log_dir_;
  std::vector<std::pair<std::string, std::vector<std::string>>> reads_;
  std::vector<Claim> claims_;
};

// Splits "<path>:<line>". False when there is no line number to read.
bool split_citation(const std::string& s, std::string& path, uint64_t& line);

}  // namespace aios::cli