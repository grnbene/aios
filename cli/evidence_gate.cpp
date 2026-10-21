#include "evidence_gate.hpp"
// Host-side only: stat/fopen to resolve a citation, rename to append a log.
#include <sys/stat.h>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace aios::cli {

const char* verdict_name(Verdict v) {
  switch (v) {
    case Verdict::ProvenRight: return "PROVEN_RIGHT";
    case Verdict::ProvenWrong: return "PROVEN_WRONG";
    case Verdict::Unproven:     return "UNPROVEN";
  }
  return "UNPROVEN";
}

bool verdict_from_name(const std::string& s, Verdict& out) {
  if (s == "PROVEN_RIGHT") { out = Verdict::ProvenRight; return true; }
  if (s == "PROVEN_WRONG") { out = Verdict::ProvenWrong; return true; }
  if ( s == "UNPROVEN")     { out = Verdict::Unproven;     return true; }
  return false;
}

bool split_citation(const std::string& s, std::string& path, uint64_t& line) {
  size_t colon = s.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= s.size()) return false;
  std::string tail = s.substr(colon + 1);
  for (char ch : tail) if (ch < '0' || ch > '9') return false;
  if (tail.size() > 1 && tail[0] == '0') return false;   // no 007, no empty line
  path = s.substr(0, colon);
  line = strtoull(tail.c_str(), nullptr, 10);
  return line > 0;
}

EvidenceGate::EvidenceGate(std::string root, std::string log_dir)
    : root_(root.empty() ? std::string(".") : std::move(root)),
      log_dir_(log_dir.empty() ? std::string(".assay/reflexion") : std::move(log_dir)) {}

std::string EvidenceGate::abs_of(const std::string& path) const {
  if (!path.empty() && path[0] == '/') return path;
  std::string r = root_;
  if (r.empty()) r = ".";
  if (r.back() != '/') r += '/';
  return r + path;
}

bool EvidenceGate::file_exists(const std::string& path) const {
  struct stat st{};
  return stat(abs_of(path).c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

size_t EvidenceGate::line_count(const std::string& path) const {
  std::ifstream in(abs_of(path), std::ios::binary);
  if (!in) return 0;
  size_t n = 0;
  char buf[65536];
  size_t got;
  while ((got = in.read(buf, sizeof buf).gcount()) > 0)
    for (size_t i = 0; i < got; ++i) if (buf[i] == '\n') ++n;
  if (in.gcount() == 0 && !in.eof()) return 0;   // unreadable, not empty
  // A file not ending in a newline still has that last (unterminated) line.
  if (n == 0) return 1;
  std::ifstream probe(abs_of(path), std::ios::binary);
  std::string last;
  probe.seekg(0, std::ios::end);
  if (probe.tellg() > 0) {
    probe.seekg(-1, std::ios::end);
    char c = 0;
    if (probe.get(c) && c != '\n') ++n;
  }
  return n;
}

void EvidenceGate::record_read(const std::string& path, const std::vector<std::string>& symbols) {
  for (auto& r : reads_)
    if (r.first == path) {
      for (auto& sym : symbols) {
        bool have = false;
        for (auto& s : r.second) if (s == sym) have = true;
        if (!have) r.second.push_back(sym);
      }
      return;
    }
  reads_.emplace_back(path, symbols);
}

bool EvidenceGate::has_read(const std::string& path) const {
  for (auto& r : reads_) if (r.first == path) return true;
  return false;
}

std::string EvidenceGate::check_edit(const std::string& path) const {
  if (path.empty()) return "Edit blocked: no file named. Name the file, then read it first.";
  if (has_read(path)) return "";
  std::string why = "Edit blocked: `" + path + "` has not been read this session. Read it first.";
  // If the file exists but went unread, say so — that is the worse mistake.
  if (file_exists(path))
    why += " It exists; a file that was never opened cannot be edited safely.";
  return why;
}

Claim& EvidenceGate::claim_slot(const std::string& claim) {
  for (auto& c : claims_) if (c.claim == claim) return c;
  claims_.push_back(Claim{claim, Verdict::Unproven, {}, ""});
  return claims_.back();
}

Claim* EvidenceGate::find(const std::string& claim) {
  for (auto& c : claims_) if (c.claim == claim) return &c;
  return nullptr;
}

Evidence EvidenceGate::resolve(const std::string& kind, const std::string& ref,
                               const std::string& detail) const {
  Evidence e;
  e.kind = kind;
  e.ref = ref;
  e.detail = detail;
  if (kind == "file") {
    std::string path;
    uint64_t line = 0;
    if (!split_citation(ref, path, line)) {
      e.problem = "citation is not <path>:<line>";
      return e;
    }
    if (!file_exists(path)) {
      e.problem = "no such file in the repo: " + path;
      return e;
    }
    size_t n = line_count(path);
    if (n == 0) {
      e.problem = "file could not be read: " + path;
      return e;
    }
    if (line > n) {
      char b[64];
      snprintf(b, sizeof b, "%llu", (unsigned long long)n);
      e.problem = "line " + std::to_string(line) + " is past end of " + path +
                  " (last line " + b + ")";
      return e;
    }
    e.resolved = true;
    return e;
  }
  if (kind == "command") {
    if (ref.empty()) { e.problem = "no command given"; return e; }
    if (detail.empty()) {
      e.problem = "command output is empty — nothing was captured to cite";
      return e;
    }
    e.resolved = true;
    return e;
  }
  e.problem = "unknown evidence kind `" + kind + "` (use file or command)";
  return e;
}

std::string EvidenceGate::add_evidence(const std::string& claim, const std::string& kind,
                                       const std::string& ref, const std::string& detail) {
  if (claim.empty()) return "Evidence rejected: no claim named. Cite evidence against a claim.";
  if (kind.empty() || ref.empty()) return "Evidence rejected: give a kind (file|command) and a reference.";
  Claim& c = claim_slot(claim);
  Evidence e = resolve(kind, ref, detail);
  c.evidence.push_back(e);
  if (!e.resolved)
    return "Evidence FABRICATED for claim `" + claim + "`: " + e.problem +
           ". Cite a real path and line, or paste the command's output.";
  return "";
}

std::string EvidenceGate::set_verdict(const std::string& claim, Verdict v,
                                      const std::string& resolution) {
  if (claim.empty()) return "Verdict refused: no claim named.";
  Claim& c = claim_slot(claim);
  c.resolution = resolution;
  if (v == Verdict::Unproven) { c.verdict = v; return ""; }
  size_t good = 0;
  for (auto& e : c.evidence) if (e.resolved) ++good;
  if (good == 0)
    return "Verdict refused: claim `" + claim + "` has no resolving evidence. " +
           std::string(verdict_name(v)) + " needs at least one citation that resolves (" +
           "a file:line inside the repo, or a command with captured output).";
  c.verdict = v;
  return "";
}

bool EvidenceGate::blocks_completion() const { return !completion_report().empty(); }

std::string EvidenceGate::completion_report() const {
  std::vector<const Claim*> unproven, fake;
  for (auto& c : claims_) {
    bool has_fake = false;
    for (auto& e : c.evidence) if (!e.resolved) has_fake = true;
    if (has_fake) fake.push_back(&c);
    if (c.verdict == Verdict::Unproven) unproven.push_back(&c);
  }
  if (unproven.empty() && fake.empty()) return "";
  std::string r = "Completion blocked:";
  if (!fake.empty()) {
    r += "\n  FABRICATED — evidence cited does not resolve:";
    for (auto* c : fake) {
      r += "\n    claim: " + c->claim;
      for (auto& e : c->evidence)
        if (!e.resolved) r += "\n      " + e.ref + "  (" + e.problem + ")";
      r += "\n    next: read the cited file for real, or re-cite what you actually saw.";
    }
  }
  if (!unproven.empty()) {
    r += "\n  UNPROVEN — a claim was made and never settled:";
    for (auto* c : unproven) {
      r += "\n    claim: " + c->claim;
      r += "\n      check: " + (c->resolution.empty()
          ? std::string("gather evidence, or say plainly this stays unresolved")
          : c->resolution);
    }
  }
  r += "\n  Mark the task done only when every claim is PROVEN_RIGHT or PROVEN_WRONG.";
  return r;
}

const Claim* EvidenceGate::fabricated() const {
  for (auto& c : claims_)
    for (auto& e : c.evidence)
      if (!e.resolved) return &c;
  return nullptr;
}

// ── audit trail ───────────────────────────────────────────────────────────
// One line per claim, in the field order receipts.jsonl uses: version, id,
// timestamp, signature, then the payload. Appended with O_APPEND; existing
// bytes are never rewritten.
std::string EvidenceGate::append_audit() const {
  std::string dir = log_dir_;
  if (!dir.empty() && dir[0] != '/') dir = root_ + "/" + dir;
  std::string path = dir + "/evidence.jsonl";
  FILE* f = fopen(path.c_str(), "ab");   // create if absent, never truncate
  if (!f) return "";
  // Milliseconds, matching the receipts' ISO-8601 with a Z.
  timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  struct tm tmv{};
  gmtime_r(&ts.tv_sec, &tmv);
  char stamp[64];
  snprintf(stamp, sizeof stamp, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
           tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min,
           tmv.tm_sec, ts.tv_nsec / 1000000);
  for (size_t i = 0; i < claims_.size(); ++i) {
    const Claim& c = claims_[i];
    std::ostringstream sig;
    sig << "evidence:" << verdict_name(c.verdict) << ":" << c.evidence.size();
    fprintf(f,
            "{\"version\":1,\"id\":\"rcpt-%08x%08x\",\"timestamp\":\"%s\","
            "\"signature\":\"%s\",\"toolName\":\"evidence_gate\",\"toolArgs\":null,"
            "\"claim\":\"%s\",\"verdict\":\"%s\",\"resolution\":\"%s\",\"evidence\":[",
            (unsigned)(i + 1) ^ (unsigned)claims_.size(),
            (unsigned)(ts.tv_nsec & 0xFFFFFFFFu), stamp, sig.str().c_str(),
            c.claim.c_str(), verdict_name(c.verdict), c.resolution.c_str());
    for (size_t k = 0; k < c.evidence.size(); ++k) {
      const Evidence& e = c.evidence[k];
      if (k) fputs(",", f);
      fprintf(f,
              "{\"kind\":\"%s\",\"ref\":\"%s\",\"resolved\":%s,\"problem\":\"%s\"}",
              e.kind.c_str(), e.ref.c_str(), e.resolved ? "true" : "false",
              e.problem.c_str());
    }
    fputs("]}\n", f);
  }
  fclose(f);
  return path;
}

// ── output ────────────────────────────────────────────────────────────────
static std::string json_escape(const std::string& s) {
  std::string o;
  for (char ch : s) {
    if (ch == '"' || ch == '\\') { o += '\\'; o += ch; }
    else if (ch == '\n') o += "\\n";
    else o += ch;
  }
  return o;
}

std::string EvidenceGate::to_json() const {
  std::string j = "{\"claims\":[";
  for (size_t i = 0; i < claims_.size(); ++i) {
    const Claim& c = claims_[i];
    if (i) j += ",";
    j += "{\"claim\":\"" + json_escape(c.claim) + "\",\"verdict\":\"" +
         verdict_name(c.verdict) + "\",\"resolution\":\"" + json_escape(c.resolution) +
         "\",\"fabricated\":" + (fabricated() == &c ? "true" : "false") + ",\"evidence\":[";
    for (size_t k = 0; k < c.evidence.size(); ++k) {
      const Evidence& e = c.evidence[k];
      if (k) j += ",";
      j += "{\"kind\":\"" + json_escape(e.kind) + "\",\"ref\":\"" + json_escape(e.ref) +
           "\",\"resolved\":" + (e.resolved ? "true" : "false") + ",\"problem\":\"" +
           json_escape(e.problem) + "\"}";
    }
    j += "]}";
  }
  j += "],\"reads\":[";
  for (size_t i = 0; i < reads_.size(); ++i) {
    if (i) j += ",";
    j += "\"" + json_escape(reads_[i].first) + "\"";
  }
  j += "],\"blocksCompletion\":" + std::string(blocks_completion() ? "true" : "false") + "}";
  return j;
}

// Wrap on word boundaries at `width`. No colour, no cursor: readable piped.
static std::string wrap(const std::string& s, size_t width, size_t indent) {
  std::string pad(indent, ' ');
  std::string line = pad, out;
  size_t col = indent;
  size_t i = 0;
  while (i <= s.size()) {
    size_t sp = s.find(' ', i);
    std::string word = s.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
    if (!line.empty() && col + 1 + word.size() > width) {
      out += line + "\n";
      line = pad + word;
      col = indent + word.size();
    } else {
      if (line.size() > pad.size()) { line += " "; ++col; }
      line += word;
      col += word.size();
    }
    if (sp == std::string::npos) break;
    i = sp + 1;
  }
  if (line.size() > pad.size() || out.empty()) out += line + "\n";
  return out;
}

void EvidenceGate::render(std::string& out, int width) const {
  size_t w = width < 20 ? 20 : static_cast<size_t>(width);
  if (claims_.empty()) {
    out += "No claims recorded. A claim is anything asserted that could be wrong:\n";
    out += "  evidence claim \"<text>\"\n";
    out += "Nothing is proven and nothing is unproven: there is nothing to check.\n";
    return;
  }
  size_t shown = 0;
  for (auto& c : claims_) {
    bool fake = false;
    for (auto& e : c.evidence) if (!e.resolved) fake = true;
    std::string tag = fake ? "FABRICATED " : verdict_name(c.verdict);
    std::string head = tag + "  " + c.claim;
    // Verdict column first, claim wrapped under it — the same order as JSON.
    out += head.size() <= w ? head + "\n"
                            : tag + "\n" + wrap(c.claim, w, 2);
    if (c.resolution.size() && c.verdict != Verdict::Unproven)
      out += wrap("resolution: " + c.resolution, w, 4);
    for (auto& e : c.evidence) {
      std::string line = (e.resolved ? "  ok  " : "  BAD ") + e.kind + ": " + e.ref;
      if (!e.resolved) line += "  — " + e.problem;
      out += line.size() <= w ? line + "\n" : wrap(line, w, 6);
    }
    if (c.evidence.empty())
      out += wrap("no evidence attached — this claim is still open", w, 4);
    ++shown;
  }
  if (shown)
    out += "\n" + std::to_string(shown) + " claim" + (shown == 1 ? "" : "s") +
           ", " + std::to_string(reads_.size()) + " file" +
           (reads_.size() == 1 ? "" : "s") + " read this session\n";
  std::string block = completion_report();
  if (block.empty()) out += "Completion permitted: every claim has a verdict.\n";
  else out += block + "\n";
}

}  // namespace aios::cli